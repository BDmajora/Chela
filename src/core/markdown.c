/* Markdown parser. Blocks are found one line at a time against a stack of open lists;
 * inline text then goes through a recursive-descent pass. Every step works on bytes,
 * so multi-byte UTF-8 passes through untouched and a malformed sequence cannot derail
 * it. Nothing here allocates per character, and nesting is capped, so a hostile or
 * enormous reply costs linear time. */
#include "markdown.h"
#include "base.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_LISTS 16
#define MAX_INLINE_DEPTH 8
#define MAX_COLUMNS 64

/* Inline */
typedef struct {
    ChBuf text;
    ChVec spans;
} InlineOut;
static void span_free(void *data) {
    ChMdSpan *s = data;
    free(s->url);
    free(s);
}
static void out_mark(InlineOut *o, size_t start, unsigned style, const char *url) {
    if (o->text.len <= start)
        return;
    ChMdSpan *last = o->spans.len ? o->spans.data[o->spans.len - 1] : NULL;
    if (last && last->style == style && last->start + last->length == start && !ch_strcmp0(last->url, url)) {
        last->length += o->text.len - start;
        return;
    }
    ChMdSpan *s = ch_new0(ChMdSpan, 1);
    *s = (ChMdSpan){start, o->text.len - start, style, url ? ch_strdup(url) : NULL};
    ch_vec_push(&o->spans, s);
}
static bool punctuation(char c) {
    return c > 0x20 && c < 0x7f && !isalnum((unsigned char)c);
}
static bool word_byte(char c) {
    return isalnum((unsigned char)c) || (c & 0x80);
}
/* The closing run of n backticks, or NULL. */
static const char *code_close(const char *p, const char *end, size_t n) {
    while (p < end) {
        if (*p != '`') {
            p++;
            continue;
        }
        const char *run = p;
        while (p < end && *p == '`')
            p++;
        if ((size_t)(p - run) == n)
            return run;
    }
    return NULL;
}
/* Where delimiter c repeated n times closes the emphasis opened before p. */
static const char *emphasis_close(const char *p, const char *end, char c, size_t n) {
    const char *start = p;
    int nested = 0; /* ** pairs still open inside this *emphasis* */
    while (p < end) {
        if (*p == '\\' && p + 1 < end) {
            p += 2;
            continue;
        }
        if (*p == '`') {
            const char *run = p;
            while (p < end && *p == '`')
                p++;
            size_t ticks = (size_t)(p - run);
            const char *close = code_close(p, end, ticks);
            if (close)
                p = close + ticks;
            continue;
        }
        if (*p != c) {
            p++;
            continue;
        }
        const char *run = p;
        while (p < end && *p == c)
            p++;
        size_t length = (size_t)(p - run);
        bool right_flanking = run > start && !isspace((unsigned char)run[-1]);
        bool left_flanking = p < end && !isspace((unsigned char)*p);
        if (n == 1 && length == 2 && (left_flanking || nested)) {
            nested += left_flanking ? 1 : -1;
            continue; /* **nested bold** inside *italics* */
        }
        if (!right_flanking || length < n)
            continue;
        if (c == '_' && p < end && word_byte(*p))
            continue; /* snake_case */
        /* In ***, the inner emphasis closes first: *a **b*** ends **b** at the third star. */
        return run + (length - n);
    }
    return NULL;
}
static bool url_scheme(const char *p, const char *end) {
    return (end - p > 8 && !strncasecmp(p, "https://", 8)) || (end - p > 7 && !strncasecmp(p, "http://", 7));
}
/* Length of a bare URL at p, without the sentence punctuation that follows it. */
static size_t bare_url(const char *p, const char *end) {
    const char *q = p;
    int parens = 0;
    while (q < end && !isspace((unsigned char)*q) && *q != '<' && *q != '>' && *q != '`' && *q != '"') {
        parens += *q == '(' ? 1 : *q == ')' ? -1 : 0;
        q++;
    }
    while (q > p) {
        char c = q[-1];
        if (strchr(".,:;!?'*_~", c))
            q--;
        else if (c == ')' && parens < 0) {
            parens++;
            q--;
        } else
            break;
    }
    return (size_t)(q - p);
}
/* [text](target "title"): the end of the construct, with text and target ranges. */
static const char *link_parts(const char *p, const char *end, const char **text_end, const char **url, size_t *url_length) {
    int depth = 1;
    const char *q = p + 1;
    for (; q < end && depth; q++) {
        if (*q == '\\' && q + 1 < end)
            q++;
        else if (*q == '\n' && q + 1 < end && q[1] == '\n')
            return NULL;
        else if (*q == '[')
            depth++;
        else if (*q == ']')
            depth--;
    }
    if (depth || q >= end || *q != '(')
        return NULL;
    *text_end = q - 1;
    q++;
    while (q < end && *q == ' ')
        q++;
    bool angled = q < end && *q == '<';
    if (angled)
        q++;
    const char *target = q;
    int parens = 0;
    while (q < end && *q != '\n') {
        if (angled ? *q == '>' : *q == ' ' || (*q == ')' && !parens))
            break;
        parens += *q == '(' ? 1 : *q == ')' ? -1 : 0;
        q++;
    }
    *url = target;
    *url_length = (size_t)(q - target);
    if (angled && q < end)
        q++;
    while (q < end && *q == ' ')
        q++;
    if (q < end && (*q == '"' || *q == '\'')) {
        char quote = *q++;
        while (q < end && *q != quote && *q != '\n')
            q++;
        if (q < end)
            q++;
        while (q < end && *q == ' ')
            q++;
    }
    return q < end && *q == ')' ? q + 1 : NULL;
}
static const struct {
    const char *name, *text;
} entities[] = {{"&amp;", "&"},  {"&lt;", "<"},     {"&gt;", ">"},    {"&quot;", "\""},
                {"&#39;", "'"},  {"&apos;", "'"},   {"&nbsp;", " "},  {"&mdash;", "—"},
                {"&ndash;", "–"}, {"&hellip;", "…"}, {"&rarr;", "→"},  {"&larr;", "←"},
                {"&times;", "×"}, {"&copy;", "©"}};
static void inline_run(InlineOut *o, const char *p, const char *end, unsigned style, const char *url, int depth);
static void plain(InlineOut *o, const char *from, const char *to, unsigned style, const char *url) {
    size_t start = o->text.len;
    ch_buf_append_len(&o->text, from, (size_t)(to - from));
    out_mark(o, start, style, url);
}
static void literal(InlineOut *o, const char *text, unsigned style, const char *url) {
    plain(o, text, text + strlen(text), style, url);
}
static void inline_run(InlineOut *o, const char *p, const char *end, unsigned style, const char *url, int depth) {
    const char *flat = p;
#define FLUSH()                                                                                              \
    do {                                                                                                     \
        if (p > flat)                                                                                        \
            plain(o, flat, p, style, url);                                                                   \
    } while (0)
    const char *begin = p;
    while (p < end) {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            if (p[1] == '\n') {
                FLUSH();
                literal(o, "\n", style, url);
                p += 2;
                flat = p;
                continue;
            }
            if (punctuation(p[1])) {
                FLUSH();
                plain(o, p + 1, p + 2, style, url);
                p += 2;
                flat = p;
                continue;
            }
        }
        if (c == '\n') {
            /* A line that ended in two spaces breaks; other line ends are soft and read as a space. */
            bool hard = p - begin >= 2 && p[-1] == ' ' && p[-2] == ' ';
            const char *trim = p;
            while (trim > flat && trim[-1] == ' ')
                trim--;
            const char *keep = p;
            p = trim;
            FLUSH();
            p = keep;
            literal(o, hard ? "\n" : " ", style, url);
            p++;
            while (p < end && *p == ' ')
                p++;
            flat = p;
            continue;
        }
        if (c == '`') {
            const char *run = p;
            while (p < end && *p == '`')
                p++;
            size_t n = (size_t)(p - run);
            const char *close = code_close(p, end, n);
            if (!close)
                continue;
            const char *from = p, *to = close;
            /* One space of padding on both sides lets code start or end with a backtick. */
            if (to - from >= 2 && *from == ' ' && to[-1] == ' ' && to - from > 2) {
                from++;
                to--;
            }
            const char *hold = p;
            p = run;
            FLUSH();
            p = hold;
            size_t start = o->text.len;
            for (const char *q = from; q < to; q++)
                ch_buf_append_c(&o->text, *q == '\n' ? ' ' : *q);
            out_mark(o, start, style | CH_MD_MONO, url);
            p = close + n;
            flat = p;
            continue;
        }
        if ((c == '!' && p + 1 < end && p[1] == '[') || c == '[') {
            const char *open = c == '!' ? p + 1 : p, *text_end = NULL, *target = NULL;
            size_t target_length = 0;
            const char *after = link_parts(open, end, &text_end, &target, &target_length);
            if (after && depth < MAX_INLINE_DEPTH && !url) {
                FLUSH();
                char *copy = ch_strndup(target, target_length);
                inline_run(o, open + 1, text_end, style | CH_MD_LINK, copy, depth + 1);
                free(copy);
                p = after;
                flat = p;
                continue;
            }
        }
        if (c == '<' && p + 1 < end) {
            const char *close = memchr(p, '>', (size_t)(end - p));
            if (close && url_scheme(p + 1, close) && !memchr(p, ' ', (size_t)(close - p))) {
                FLUSH();
                char *copy = ch_strndup(p + 1, (size_t)(close - p - 1));
                literal(o, copy, style | CH_MD_LINK, copy);
                free(copy);
                p = close + 1;
                flat = p;
                continue;
            }
            if (close && (!strncasecmp(p, "<br>", 4) || !strncasecmp(p, "<br/>", 5) || !strncasecmp(p, "<br />", 6))) {
                FLUSH();
                literal(o, "\n", style, url);
                p = close + 1;
                flat = p;
                continue;
            }
        }
        if (c == '&') {
            bool found = false;
            for (size_t i = 0; i < CH_N_ELEMENTS(entities); i++) {
                size_t n = strlen(entities[i].name);
                if ((size_t)(end - p) >= n && !strncmp(p, entities[i].name, n)) {
                    FLUSH();
                    literal(o, entities[i].text, style, url);
                    p += n;
                    flat = p;
                    found = true;
                    break;
                }
            }
            if (found)
                continue;
        }
        if ((c == 'h' || c == 'H') && !url && url_scheme(p, end) && (p == begin || !word_byte(p[-1]))) {
            size_t n = bare_url(p, end);
            if (n > 8) {
                FLUSH();
                char *copy = ch_strndup(p, n);
                literal(o, copy, style | CH_MD_LINK, copy);
                free(copy);
                p += n;
                flat = p;
                continue;
            }
        }
        if ((c == '*' || c == '_' || c == '~') && depth < MAX_INLINE_DEPTH) {
            const char *run = p;
            while (p < end && *p == c)
                p++;
            size_t n = (size_t)(p - run);
            bool opens = p < end && !isspace((unsigned char)*p);
            if (c == '_' && run > begin && word_byte(run[-1]))
                opens = false;
            if (c == '~' && n != 2)
                opens = false;
            if (!opens || n > 3) {
                continue;
            }
            /* Longest first: *** is bold and italic together. */
            bool handled = false;
            for (size_t want = n; want >= 1 && !handled; want--) {
                const char *close = emphasis_close(run + want, end, c, want);
                if (!close || close == run + want)
                    continue;
                if (c == '~' && want != 2)
                    continue;
                unsigned add = c == '~' ? CH_MD_STRIKE
                             : want == 3 ? CH_MD_BOLD | CH_MD_ITALIC
                             : want == 2 ? CH_MD_BOLD
                                         : CH_MD_ITALIC;
                const char *hold = p;
                p = run;
                FLUSH();
                p = hold;
                /* A *** that only pairs as ** keeps its extra star as literal text. */
                if (want < n)
                    plain(o, run, run + (n - want), style, url);
                inline_run(o, run + n, close, style | add, url, depth + 1);
                p = close + want;
                flat = p;
                handled = true;
            }
            continue;
        }
        p++;
    }
    FLUSH();
#undef FLUSH
}
void ch_md_inline_parse(ChMdInline *out, const char *text, size_t length) {
    InlineOut o;
    ch_buf_init(&o.text);
    ch_vec_init(&o.spans, span_free);
    inline_run(&o, text, text + length, 0, NULL, 0);
    out->span_count = (int)o.spans.len;
    out->spans = ch_new0(ChMdSpan, o.spans.len + 1);
    for (size_t i = 0; i < o.spans.len; i++) {
        out->spans[i] = *(ChMdSpan *)o.spans.data[i];
        free(o.spans.data[i]);
    }
    o.spans.len = 0;
    ch_vec_clear(&o.spans);
    out->text = ch_buf_steal(&o.text);
}
void ch_md_inline_clear(ChMdInline *inl) {
    for (int i = 0; i < inl->span_count; i++)
        free(inl->spans[i].url);
    free(inl->spans);
    free(inl->text);
    memset(inl, 0, sizeof(*inl));
}

/* Blocks */
typedef struct {
    int content; /* column the item's text starts at */
    int marker;  /* column of its marker */
} OpenList;
typedef struct {
    ChVec blocks;
    OpenList lists[MAX_LISTS];
    int list_count;
    /* The paragraph being collected and what it belongs to. */
    ChBuf paragraph;
    bool in_paragraph, paragraph_tight;
    int paragraph_quote;
    /* The marker waiting for its item's first block. */
    ChMdMarker marker;
    char *number;
    int marker_depth;
    bool blank_before;
    /* An open code fence. */
    bool in_code;
    char fence;
    int fence_length, fence_indent, fence_quote;
    ChBuf code;
    char *language;
    bool code_tight;
} Parser;

static void block_free(ChMdBlock *b) {
    ch_md_inline_clear(&b->body);
    for (int i = 0; b->cells && i < b->rows * b->columns; i++)
        ch_md_inline_clear(&b->cells[i]);
    free(b->cells);
    free(b->aligns);
    free(b->number);
    free(b->language);
}
static ChMdBlock *push_block(Parser *p, ChMdKind kind, int quote) {
    ChMdBlock *b = ch_new0(ChMdBlock, 1);
    b->kind = kind;
    b->list_depth = p->list_count;
    b->quote_depth = quote;
    b->tight = p->list_count > 0 && !p->blank_before;
    ch_vec_push(&p->blocks, b);
    return b;
}
/* A marker belongs to the first block of its item, whatever kind that block is. */
static void take_marker(Parser *p, ChMdBlock *b) {
    b->marker = p->marker;
    b->number = p->number;
    if (p->marker)
        b->list_depth = p->marker_depth;
    p->marker = CH_MD_NO_MARKER;
    p->number = NULL;
}
static void flush_paragraph(Parser *p) {
    if (!p->in_paragraph)
        return;
    p->in_paragraph = false;
    char *text = p->paragraph.str;
    size_t n = p->paragraph.len;
    while (n && (text[n - 1] == '\n' || text[n - 1] == ' ' || text[n - 1] == '\t'))
        n--;
    size_t from = 0;
    while (from < n && (text[from] == ' ' || text[from] == '\t'))
        from++;
    ChMdBlock *b = push_block(p, CH_MD_PARAGRAPH, p->paragraph_quote);
    take_marker(p, b);
    b->tight = b->list_depth > 0 && p->paragraph_tight;
    ch_md_inline_parse(&b->body, text + from, n - from);
    ch_buf_truncate(&p->paragraph, 0);
}
static void emit_marker_only(Parser *p) {
    /* "- " alone still draws its bullet. */
    if (p->marker && !p->in_paragraph) {
        p->in_paragraph = true;
        p->paragraph_tight = !p->blank_before;
        flush_paragraph(p);
    }
}
static void close_lists_to(Parser *p, int count) {
    if (count < p->list_count)
        p->list_count = count;
}
/* Columns of leading space, with tabs reaching the next multiple of four. */
static int indent_of(const char *s, const char **rest) {
    int column = 0;
    while (*s == ' ' || *s == '\t') {
        column += *s == '\t' ? 4 - column % 4 : 1;
        s++;
    }
    *rest = s;
    return column;
}
/* Removes up to n columns of indentation. */
static const char *strip_columns(const char *s, int n) {
    int column = 0;
    while (column < n && (*s == ' ' || *s == '\t')) {
        column += *s == '\t' ? 4 - column % 4 : 1;
        s++;
    }
    return s;
}
static bool is_blank(const char *s) {
    for (; *s; s++)
        if (!isspace((unsigned char)*s))
            return false;
    return true;
}
static bool is_rule(const char *s) {
    char c = 0;
    int count = 0;
    for (; *s; s++) {
        if (*s == ' ' || *s == '\t' || *s == '\r')
            continue;
        if (*s != '-' && *s != '*' && *s != '_')
            return false;
        if (c && *s != c)
            return false;
        c = *s;
        count++;
    }
    return count >= 3;
}
/* "===" or "---" under a paragraph: 1 or 2 for the heading level, 0 otherwise. */
static int setext_level(const char *s) {
    char c = *s;
    if (c != '=' && c != '-')
        return 0;
    while (*s == c)
        s++;
    while (*s == ' ' || *s == '\t' || *s == '\r')
        s++;
    return *s ? 0 : c == '=' ? 1 : 2;
}
static int heading_level(const char *s, const char **text) {
    int level = 0;
    while (s[level] == '#' && level < 7)
        level++;
    if (!level || level > 6 || (s[level] && s[level] != ' ' && s[level] != '\t' && s[level] != '\r'))
        return 0;
    s += level;
    while (*s == ' ' || *s == '\t')
        s++;
    *text = s;
    return level;
}
/* A list marker at s, leaving the marker's width and the item text. */
static ChMdMarker list_marker(const char *s, int *width, char **number) {
    if ((*s == '-' || *s == '*' || *s == '+') && (s[1] == ' ' || s[1] == '\t' || !s[1] || s[1] == '\r')) {
        *width = 1;
        return CH_MD_BULLET;
    }
    int digits = 0;
    while (isdigit((unsigned char)s[digits]) && digits < 10)
        digits++;
    if (digits && (s[digits] == '.' || s[digits] == ')') &&
        (s[digits + 1] == ' ' || s[digits + 1] == '\t' || !s[digits + 1] || s[digits + 1] == '\r')) {
        *width = digits + 1;
        *number = ch_strndup(s, (size_t)digits + 1);
        return CH_MD_NUMBER;
    }
    return CH_MD_NO_MARKER;
}
static bool fence_open(const char *s, char *fence, int *length) {
    char c = *s;
    if (c != '`' && c != '~')
        return false;
    int n = 0;
    while (s[n] == c)
        n++;
    if (n < 3)
        return false;
    /* A backtick in the info string means inline code, not a fence. */
    if (c == '`' && strchr(s + n, '`'))
        return false;
    *fence = c;
    *length = n;
    return true;
}
static bool fence_close(const char *s, char fence, int length) {
    int n = 0;
    while (s[n] == fence)
        n++;
    return n >= length && is_blank(s + n);
}
static void finish_code(Parser *p, bool unclosed) {
    ChMdBlock *b = push_block(p, CH_MD_CODE, p->fence_quote);
    take_marker(p, b);
    b->tight = b->list_depth > 0 && p->code_tight;
    size_t n = p->code.len;
    if (n && p->code.str[n - 1] == '\n')
        n--;
    b->body.text = ch_strndup(p->code.str, n);
    b->language = ch_steal(&p->language);
    if (!b->language)
        b->language = ch_strdup("");
    b->unclosed = unclosed;
    ch_buf_truncate(&p->code, 0);
    p->in_code = false;
}
/* Cells of one table row: unescaped pipes separate them, and code spans protect theirs. */
static ChVec *split_row(const char *s) {
    ChVec *cells = ch_vec_new(free);
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s == '|')
        s++;
    const char *start = s;
    bool code = false;
    for (;; s++) {
        if (*s == '\\' && s[1]) {
            s++;
            continue;
        }
        if (*s == '`')
            code = !code;
        if (*s && (*s != '|' || code) && *s != '\r')
            continue;
        const char *from = start, *to = s;
        while (from < to && (*from == ' ' || *from == '\t'))
            from++;
        while (to > from && (to[-1] == ' ' || to[-1] == '\t'))
            to--;
        bool last = !*s || *s == '\r';
        /* The empty piece after a closing pipe is not a cell. */
        if (!last || to > from)
            ch_vec_push(cells, ch_strndup(from, (size_t)(to - from)));
        if (last)
            break;
        start = s + 1;
    }
    return cells;
}
static bool delimiter_row(const char *s, ChMdAlign *aligns, int *columns) {
    if (!strchr(s, '-') || (!strchr(s, '|') && !strchr(s, ':')))
        return false;
    ChVec *cells = split_row(s);
    bool ok = cells->len > 0 && cells->len <= MAX_COLUMNS;
    for (size_t i = 0; ok && i < cells->len; i++) {
        const char *c = cells->data[i];
        size_t n = strlen(c);
        bool left = n && c[0] == ':', right = n && c[n - 1] == ':';
        size_t from = left, to = n - (size_t)(right && n > 1);
        ok = to > from;
        for (size_t k = from; ok && k < to; k++)
            ok = c[k] == '-';
        if (ok && aligns)
            aligns[i] = left && right ? CH_MD_CENTER : right ? CH_MD_RIGHT : CH_MD_LEFT;
    }
    if (ok)
        *columns = (int)cells->len;
    ch_vec_free(cells);
    return ok;
}
/* A line without its quote markers and its list indentation. */
static const char *clean_line(const char *line, int strip, int quote) {
    for (int i = 0; i < quote; i++) {
        const char *q;
        indent_of(line, &q);
        if (*q != '>')
            break;
        line = q + 1;
        if (*line == ' ')
            line++;
    }
    return strip_columns(line, strip);
}
static size_t parse_table(Parser *p, char **lines, size_t index, int strip, int quote) {
    ChMdAlign aligns[MAX_COLUMNS];
    int columns = 0;
    delimiter_row(clean_line(lines[index + 1], strip, quote), aligns, &columns);
    ChVec rows;
    ch_vec_init(&rows, (void (*)(void *))ch_vec_free);
    ch_vec_push(&rows, split_row(clean_line(lines[index], strip, quote)));
    size_t i = index + 2;
    for (; lines[i]; i++) {
        const char *row = clean_line(lines[i], strip, quote);
        if (is_blank(row) || !strchr(row, '|'))
            break;
        ch_vec_push(&rows, split_row(row));
    }
    ChMdBlock *b = push_block(p, CH_MD_TABLE, quote);
    take_marker(p, b);
    b->rows = (int)rows.len;
    b->columns = columns;
    b->aligns = ch_new0(ChMdAlign, (size_t)columns);
    memcpy(b->aligns, aligns, sizeof(ChMdAlign) * (size_t)columns);
    b->cells = ch_new0(ChMdInline, (size_t)b->rows * (size_t)columns);
    for (int r = 0; r < b->rows; r++) {
        ChVec *cells = rows.data[r];
        for (int c = 0; c < columns; c++) {
            const char *text = (size_t)c < cells->len ? cells->data[c] : "";
            ch_md_inline_parse(&b->cells[r * columns + c], text, strlen(text));
        }
    }
    ch_vec_clear(&rows);
    return i - 1;
}
static int quote_depth(const char **line) {
    const char *s = *line;
    int depth = 0;
    for (;;) {
        const char *q;
        int indent = indent_of(s, &q);
        if (indent > 3 || *q != '>')
            break;
        s = q + 1;
        if (*s == ' ')
            s++;
        depth++;
    }
    *line = s;
    return depth;
}
static void paragraph_line(Parser *p, const char *s, int quote) {
    if (!p->in_paragraph) {
        p->in_paragraph = true;
        p->paragraph_quote = quote;
        p->paragraph_tight = !p->blank_before;
    }
    ch_buf_append(&p->paragraph, s);
    size_t n = p->paragraph.len;
    while (n && p->paragraph.str[n - 1] == '\r')
        n--;
    ch_buf_truncate(&p->paragraph, n);
    ch_buf_append_c(&p->paragraph, '\n');
}
static bool table_starts(Parser *p, char **lines, size_t index, const char *s, int base, int quote) {
    (void)p;
    if (!strchr(s, '|') || !lines[index + 1])
        return false;
    int columns = 0;
    if (!delimiter_row(clean_line(lines[index + 1], base, quote), NULL, &columns))
        return false;
    ChVec *header = split_row(s);
    bool matches = (int)header->len == columns;
    ch_vec_free(header);
    return matches;
}
/* Whether a line ends the paragraph being collected, rather than continuing it. */
static bool interrupts_paragraph(Parser *p, char **lines, size_t index, const char *s, int base, int quote) {
    const char *text;
    char fence;
    int length, width;
    char *number = NULL;
    if (heading_level(s, &text) || fence_open(s, &fence, &length) || is_rule(s) ||
        table_starts(p, lines, index, s, base, quote))
        return true;
    ChMdMarker marker = list_marker(s, &width, &number);
    /* A new list may interrupt text only from 1, so "in 2024. It was" stays a sentence. */
    bool interrupts = marker == CH_MD_BULLET ? !is_blank(s + width)
                    : marker == CH_MD_NUMBER ? !is_blank(s + width) && (p->list_count || atoi(number) == 1)
                                             : false;
    free(number);
    return interrupts;
}
static void indented_code(Parser *p, char **lines, size_t *index, int column, int quote) {
    size_t last = *index;
    for (size_t i = *index + 1; lines[i]; i++) {
        const char *line = clean_line(lines[i], 0, quote), *rest;
        if (is_blank(line))
            continue;
        if (indent_of(line, &rest) < column)
            break;
        last = i;
    }
    for (size_t i = *index; i <= last; i++) {
        ch_buf_append(&p->code, strip_columns(clean_line(lines[i], 0, quote), column));
        ch_buf_append_c(&p->code, '\n');
    }
    p->fence_quote = quote;
    p->language = NULL;
    p->code_tight = false;
    finish_code(p, false);
    *index = last;
}
static void line_block(Parser *p, char **lines, size_t *index, const char *body, int quote);
/* A list item: opens its list level, then treats the rest of the line as a new block. */
static void list_item(Parser *p, char **lines, size_t *index, const char *s, int indent, int keep, ChMdMarker marker,
                      char *number, int width, int quote) {
    flush_paragraph(p);
    emit_marker_only(p);
    close_lists_to(p, keep);
    if (keep >= MAX_LISTS) {
        free(number);
        paragraph_line(p, s + width, quote);
        return;
    }
    const char *rest = s + width, *content;
    int gap = indent_of(rest, &content);
    /* Five or more spaces start code inside the item, whose text then begins one column in. */
    int column = indent + width + (is_blank(rest) || gap > 4 ? 1 : CH_MAX(gap, 1));
    p->lists[keep] = (OpenList){column, indent};
    p->list_count = keep + 1;
    p->marker = marker;
    p->number = number;
    p->marker_depth = p->list_count;
    if (marker == CH_MD_BULLET && content[0] == '[' && strchr(" xX", content[1]) && content[1] && content[2] == ']' &&
        (content[3] == ' ' || !content[3] || content[3] == '\r')) {
        p->marker = content[1] == ' ' ? CH_MD_TASK_OPEN : CH_MD_TASK_DONE;
        content += content[3] == ' ' ? 4 : 3;
    }
    if (is_blank(content)) {
        emit_marker_only(p);
        return;
    }
    char *line = ch_strdup_printf("%*s%s", column, "", content);
    line_block(p, lines, index, line, quote);
    free(line);
}
static void line_block(Parser *p, char **lines, size_t *index, const char *body, int quote) {
    const char *s, *text;
    int indent = indent_of(body, &s), width = 0;
    char *number = NULL;
    ChMdMarker marker = is_rule(s) ? CH_MD_NO_MARKER : list_marker(s, &width, &number);
    /* The open lists this line stays inside. A marker nests under its parent from two
     * columns in, which is looser than CommonMark but matches how models indent. */
    int keep = p->list_count;
    if (marker)
        while (keep > 0 && indent < p->lists[keep - 1].marker + 2)
            keep--;
    else
        while (keep > 0 && indent < p->lists[keep - 1].content)
            keep--;
    int base = keep ? p->lists[keep - 1].content : 0, relative = indent - base;
    if (p->in_paragraph) {
        int level = quote == p->paragraph_quote ? setext_level(s) : 0;
        if (level) {
            size_t n = p->paragraph.len;
            while (n && strchr("\n \t", p->paragraph.str[n - 1]))
                n--;
            ChMdBlock *b = push_block(p, CH_MD_HEADING, quote);
            take_marker(p, b);
            b->level = level;
            ch_md_inline_parse(&b->body, p->paragraph.str, n);
            ch_buf_truncate(&p->paragraph, 0);
            p->in_paragraph = false;
            free(number);
            return;
        }
        if (!interrupts_paragraph(p, lines, *index, s, base, quote)) {
            /* Continuation lines, including lazy ones left of the item's text. */
            free(number);
            paragraph_line(p, s, p->paragraph_quote);
            return;
        }
        flush_paragraph(p);
    }
    if (relative >= 4 && !p->marker && !p->list_count) {
        free(number);
        indented_code(p, lines, index, base + 4, quote);
        return;
    }
    if (marker) {
        list_item(p, lines, index, s, indent, keep, marker, number, width, quote);
        return;
    }
    free(number);
    close_lists_to(p, keep);
    if (relative >= 4 && !p->marker) {
        indented_code(p, lines, index, base + 4, quote);
        return;
    }
    if (fence_open(s, &p->fence, &p->fence_length)) {
        const char *info = s + p->fence_length;
        while (*info == ' ' || *info == '\t')
            info++;
        size_t n = 0;
        while (info[n] && !isspace((unsigned char)info[n]) && info[n] != '{' && info[n] != ',')
            n++;
        free(p->language);
        p->language = ch_strndup(info, n);
        p->fence_indent = base + CH_MIN(relative, 3);
        p->fence_quote = quote;
        p->code_tight = !p->blank_before;
        p->in_code = true;
        return;
    }
    int level = heading_level(s, &text);
    if (level) {
        size_t n = strlen(text);
        while (n && strchr(" \r\t", text[n - 1]))
            n--;
        /* The closing ## of "## Title ##" is decoration. */
        size_t hashes = n;
        while (hashes && text[hashes - 1] == '#')
            hashes--;
        if (hashes < n && (!hashes || text[hashes - 1] == ' ')) {
            n = hashes;
            while (n && text[n - 1] == ' ')
                n--;
        }
        ChMdBlock *b = push_block(p, CH_MD_HEADING, quote);
        take_marker(p, b);
        b->level = level;
        ch_md_inline_parse(&b->body, text, n);
        return;
    }
    if (is_rule(s)) {
        take_marker(p, push_block(p, CH_MD_RULE, quote));
        return;
    }
    if (table_starts(p, lines, *index, s, base, quote)) {
        *index = parse_table(p, lines, *index, base, quote);
        return;
    }
    paragraph_line(p, s, quote);
}
ChMdDocument *ch_md_parse(const char *text) {
    Parser p = {0};
    ch_vec_init(&p.blocks, NULL);
    ch_buf_init(&p.paragraph);
    ch_buf_init(&p.code);
    char **lines = ch_strsplit(text ? text : "", "\n");
    int current_quote = 0;
    for (size_t i = 0; lines[i]; i++) {
        const char *line = lines[i];
        if (p.in_code) {
            const char *probe = line;
            /* A quote that ends also ends the code inside it. */
            if (p.fence_quote && quote_depth(&probe) < p.fence_quote)
                finish_code(&p, true);
            else {
                const char *body = clean_line(line, 0, p.fence_quote), *s;
                int indent = indent_of(body, &s);
                if (indent - p.fence_indent < 4 && fence_close(s, p.fence, p.fence_length)) {
                    finish_code(&p, false);
                    continue;
                }
                ch_buf_append(&p.code, strip_columns(body, p.fence_indent));
                size_t n = p.code.len;
                while (n && p.code.str[n - 1] == '\r')
                    n--;
                ch_buf_truncate(&p.code, n);
                ch_buf_append_c(&p.code, '\n');
                continue;
            }
        }
        const char *body = line;
        int quote = quote_depth(&body);
        if (is_blank(body)) {
            flush_paragraph(&p);
            emit_marker_only(&p);
            p.blank_before = true;
            current_quote = quote;
            continue;
        }
        if (quote != current_quote) {
            const char *s;
            indent_of(body, &s);
            /* Text under a quoted paragraph continues it, as long as it could not start a block. */
            if (quote < current_quote && p.in_paragraph && !interrupts_paragraph(&p, lines, i, s, 0, quote))
                quote = current_quote;
            else {
                flush_paragraph(&p);
                emit_marker_only(&p);
                close_lists_to(&p, 0);
                current_quote = quote;
            }
        }
        line_block(&p, lines, &i, body, quote);
        p.blank_before = false;
    }
    if (p.in_code)
        finish_code(&p, true);
    flush_paragraph(&p);
    emit_marker_only(&p);
    free(p.number);
    free(p.language);
    ch_strfreev(lines);
    ch_buf_clear(&p.paragraph);
    ch_buf_clear(&p.code);
    ChMdDocument *doc = ch_new0(ChMdDocument, 1);
    doc->count = p.blocks.len;
    doc->blocks = ch_new0(ChMdBlock, p.blocks.len + 1);
    for (size_t i = 0; i < p.blocks.len; i++) {
        doc->blocks[i] = *(ChMdBlock *)p.blocks.data[i];
        free(p.blocks.data[i]);
    }
    p.blocks.len = 0;
    ch_vec_clear(&p.blocks);
    return doc;
}
void ch_md_free(ChMdDocument *doc) {
    if (!doc)
        return;
    for (size_t i = 0; i < doc->count; i++)
        block_free(&doc->blocks[i]);
    free(doc->blocks);
    free(doc);
}
