/* Text layout: rich spans, word wrapping with character fallback, ellipsizing,
 * caret positions and hit-testing. Glyph shaping is the backend's job; this file
 * only needs the width of a run of text in one font. */
#include "cu_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { PIECE_WORD, PIECE_SPACE, PIECE_NEWLINE };
typedef struct {
    int kind, span;
    size_t start, end;
} Piece;

void text_layout_free(TextLayout *layout) {
    if (!layout)
        return;
    free(layout->text);
    for (int i = 0; i < layout->span_count; i++)
        free((char *)layout->spans[i].link);
    free(layout->spans);
    free(layout->span_start);
    free(layout->lines);
    free(layout->runs);
    free(layout);
}
static int classify(char c) {
    return c == '\n' ? PIECE_NEWLINE : c == ' ' || c == '\t' ? PIECE_SPACE : PIECE_WORD;
}
static float width(Cu *cu, TextLayout *l, int span, size_t start, size_t end) {
    return cu_text_width(cu, l->spans[span].font, l->text + start, end - start);
}
static CuFontMetrics metrics(Cu *cu, TextLayout *l, int span) {
    return cu_font_metrics(cu, l->spans[span].font);
}
/* Copies the spans' text into one buffer. Tabs become spaces: four in preformatted
 * text, one elsewhere so offsets keep matching the source. */
static TextLayout *collect(const CuSpan *spans, int count, const TextOptions *o) {
    TextLayout *l = ch_new0(TextLayout, 1);
    l->span_count = count ? count : 1;
    l->spans = ch_new0(CuSpan, (size_t)l->span_count);
    l->span_start = ch_new0(size_t, (size_t)l->span_count + 1);
    ChBuf b;
    ch_buf_init(&b);
    for (int i = 0; i < count; i++) {
        l->spans[i] = spans[i];
        /* A layout outlives the frame that built it, so it keeps its own copy. */
        l->spans[i].link = spans[i].link ? ch_strdup(spans[i].link) : NULL;
        l->span_start[i] = b.len;
        size_t n = spans[i].length == SIZE_MAX ? strlen(spans[i].text) : spans[i].length;
        for (size_t k = 0; k < n; k++) {
            char c = spans[i].text[k];
            if (c == '\t')
                ch_buf_append(&b, o->preformatted ? "    " : " ");
            else if (c == '\r')
                ch_buf_append_c(&b, ' ');
            else
                ch_buf_append_c(&b, c);
        }
    }
    if (!count)
        l->spans[0] = (CuSpan){.text = "", .length = 0};
    l->span_start[count ? count : 1] = b.len;
    l->length = b.len;
    l->text = ch_buf_steal(&b);
    return l;
}
static int span_at(const TextLayout *l, size_t offset) {
    for (int i = l->span_count - 1; i >= 0; i--)
        if (offset >= l->span_start[i] && (l->span_start[i] < l->span_start[i + 1] || i == 0))
            return i;
    return 0;
}
static Piece *pieces(const TextLayout *l, int *count) {
    ChVec v;
    ch_vec_init(&v, free);
    for (int s = 0; s < l->span_count; s++) {
        size_t i = l->span_start[s], end = l->span_start[s + 1];
        while (i < end) {
            int kind = classify(l->text[i]);
            size_t j = i + 1;
            if (kind != PIECE_NEWLINE)
                while (j < end && classify(l->text[j]) == kind)
                    j++;
            Piece *p = ch_new0(Piece, 1);
            *p = (Piece){kind, s, i, j};
            ch_vec_push(&v, p);
            i = j;
        }
    }
    Piece *out = ch_new0(Piece, v.len + 1);
    for (size_t i = 0; i < v.len; i++)
        out[i] = *(Piece *)v.data[i];
    *count = (int)v.len;
    ch_vec_clear(&v);
    return out;
}
float text_natural_width(Cu *cu, const CuSpan *spans, int count, const TextOptions *o) {
    float best = 0, line = 0;
    for (int s = 0; s < count; s++) {
        const char *t = spans[s].text;
        size_t n = spans[s].length == SIZE_MAX ? strlen(t) : spans[s].length, start = 0;
        for (size_t i = 0; i <= n; i++) {
            if (i == n || t[i] == '\n') {
                if (i > start) {
                    /* Measure tabs as the layout renders them. */
                    if (memchr(t + start, '\t', i - start)) {
                        CuSpan one = spans[s];
                        one.text = t + start;
                        one.length = i - start;
                        TextLayout *l = collect(&one, 1, o);
                        line += cu_text_width(cu, spans[s].font, l->text, l->length);
                        text_layout_free(l);
                    } else
                        line += cu_text_width(cu, spans[s].font, t + start, i - start);
                }
                if (i < n) {
                    best = fmaxf(best, line);
                    line = 0;
                }
                start = i + 1;
            }
        }
    }
    return ceilf(fmaxf(best, line));
}

typedef struct {
    Cu *cu;
    TextLayout *l;
    const TextOptions *o;
    ChVec lines, runs;
    TextLine *line;
    float x, max_w, y;
    bool content;
} Builder;
static void start_line(Builder *b, size_t start, bool continuation) {
    TextLine *line = ch_new0(TextLine, 1);
    line->start = line->end = start;
    line->first_run = (int)b->runs.len;
    b->line = line;
    b->x = b->o->indent + (continuation ? b->o->hanging : 0);
    line->x = b->x;
    b->content = false;
    ch_vec_push(&b->lines, line);
}
static void note_span(Builder *b, int span) {
    CuFontMetrics m = metrics(b->cu, b->l, span);
    float lh = m.height * (b->o->line_height > 0 ? b->o->line_height : 1);
    float ascent = m.ascent + (lh - m.height) / 2;
    if (lh > b->line->h) {
        b->line->ascent = fmaxf(b->line->ascent, ascent);
        b->line->h = lh;
    } else
        b->line->ascent = fmaxf(b->line->ascent, ascent);
}
static void add_run(Builder *b, int span, size_t start, size_t end, float w) {
    TextRun *last = b->runs.len > (size_t)b->line->first_run ? b->runs.data[b->runs.len - 1] : NULL;
    if (last && last->span == span && last->end == start) {
        last->end = end;
        last->w += w;
    } else {
        TextRun *r = ch_new0(TextRun, 1);
        *r = (TextRun){span, start, end, b->x, w};
        ch_vec_push(&b->runs, r);
        b->line->run_count++;
    }
    note_span(b, span);
    b->x += w;
    b->line->end = end;
    b->line->w = b->x;
    b->content = true;
}
static void place_word(Builder *b, int span, size_t start, size_t end) {
    float w = width(b->cu, b->l, span, start, end);
    bool wrap = b->o->wrap != CU_WRAP_NONE;
    if (wrap && b->content && b->x + w > b->max_w + 0.01f) {
        start_line(b, start, true);
    }
    if (!wrap || b->x + w <= b->max_w + 0.01f) {
        add_run(b, span, start, end, w);
        return;
    }
    /* Too wide for a whole line: break between characters. */
    size_t i = start;
    while (i < end) {
        size_t j = (size_t)(ch_utf8_next(b->l->text + i) - b->l->text);
        if (j > end)
            j = end;
        float cw = width(b->cu, b->l, span, i, j);
        if (b->content && b->x + cw > b->max_w + 0.01f)
            start_line(b, i, true);
        add_run(b, span, i, j, cw);
        i = j;
    }
}
static TextLayout *finish(Builder *b) {
    TextLayout *l = b->l;
    l->line_count = (int)b->lines.len;
    l->lines = ch_new0(TextLine, b->lines.len + 1);
    float y = 0, w = 0;
    for (size_t i = 0; i < b->lines.len; i++) {
        TextLine *line = b->lines.data[i];
        if (line->h <= 0)
            note_span(&(Builder){b->cu, l, b->o, {0}, {0}, line, 0, 0, 0, false}, span_at(l, line->start));
        line->y = y;
        y += line->h;
        w = fmaxf(w, line->w);
        l->lines[i] = *line;
    }
    l->run_count = (int)b->runs.len;
    l->runs = ch_new0(TextRun, b->runs.len + 1);
    for (size_t i = 0; i < b->runs.len; i++)
        l->runs[i] = *(TextRun *)b->runs.data[i];
    ch_vec_clear(&b->lines);
    ch_vec_clear(&b->runs);
    l->w = ceilf(w);
    l->h = ceilf(y);
    /* Alignment within the available width. */
    if (b->o->align && isfinite(b->max_w))
        for (int i = 0; i < l->line_count; i++) {
            float shift = (b->max_w - l->lines[i].w) * (b->o->align == CU_CENTER ? 0.5f : 1.0f);
            if (shift <= 0)
                continue;
            l->lines[i].x += shift;
            for (int r = 0; r < l->lines[i].run_count; r++)
                l->runs[l->lines[i].first_run + r].x += shift;
        }
    return l;
}
static TextLayout *ellipsized(Cu *cu, const CuSpan *span, const TextOptions *o, float max_w) {
    const char *t = span->text;
    size_t n = span->length == SIZE_MAX ? strlen(t) : span->length;
    if (memchr(t, '\n', n))
        n = (size_t)((const char *)memchr(t, '\n', n) - t);
    float ellipsis = cu_text_width(cu, span->font, "…", strlen("…"));
    size_t chars = ch_utf8_strlen(t, (ptrdiff_t)n);
    size_t lo = 0, hi = chars;
    ChBuf b;
    ch_buf_init(&b);
    while (lo < hi) {
        size_t mid = (lo + hi + 1) / 2;
        float w;
        if (o->ellipsize == CU_ELLIPSIZE_MIDDLE) {
            size_t head = (mid + 1) / 2, tail = mid / 2;
            const char *h_end = ch_utf8_offset(t, head), *t_start = ch_utf8_offset(t, chars - tail);
            w = cu_text_width(cu, span->font, t, (size_t)(h_end - t)) +
                cu_text_width(cu, span->font, t_start, (size_t)(t + n - t_start));
        } else
            w = cu_text_width(cu, span->font, t, (size_t)(ch_utf8_offset(t, mid) - t));
        if (w + ellipsis <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    if (o->ellipsize == CU_ELLIPSIZE_MIDDLE) {
        size_t head = (lo + 1) / 2, tail = lo / 2;
        const char *h_end = ch_utf8_offset(t, head), *t_start = ch_utf8_offset(t, chars - tail);
        ch_buf_append_len(&b, t, (size_t)(h_end - t));
        ch_buf_append(&b, "…");
        ch_buf_append_len(&b, t_start, (size_t)(t + n - t_start));
    } else {
        ch_buf_append_len(&b, t, (size_t)(ch_utf8_offset(t, lo) - t));
        ch_buf_append(&b, "…");
    }
    CuSpan cut = *span;
    cut.text = b.str;
    cut.length = b.len;
    TextOptions single = *o;
    single.ellipsize = CU_ELLIPSIZE_NONE;
    single.wrap = CU_WRAP_NONE;
    TextLayout *l = text_layout_new(cu, &cut, 1, &single, INFINITY);
    ch_buf_clear(&b);
    return l;
}
TextLayout *text_layout_new(Cu *cu, const CuSpan *spans, int count, const TextOptions *o, float max_w) {
    if (o->ellipsize && o->wrap == CU_WRAP_NONE && count == 1 && isfinite(max_w)) {
        float natural = text_natural_width(cu, spans, 1, o);
        if (natural > max_w + 0.5f)
            return ellipsized(cu, &spans[0], o, max_w);
    }
    Builder b = {.cu = cu, .o = o, .max_w = o->wrap == CU_WRAP_NONE ? INFINITY : fmaxf(1, max_w)};
    b.l = collect(spans, count, o);
    ch_vec_init(&b.lines, free);
    ch_vec_init(&b.runs, free);
    start_line(&b, 0, false);
    int n = 0;
    Piece *ps = pieces(b.l, &n);
    for (int i = 0; i < n; i++) {
        Piece *p = &ps[i];
        if (p->kind == PIECE_NEWLINE) {
            note_span(&b, p->span);
            if (o->max_lines && (int)b.lines.len >= o->max_lines)
                break;
            start_line(&b, p->end, false);
            continue;
        }
        if (p->kind == PIECE_SPACE) {
            /* Spaces separate words; at a wrap they are dropped, except in code. */
            float w = width(cu, b.l, p->span, p->start, p->end);
            bool at_start = !b.content && b.lines.len > 1 && !o->preformatted;
            if (at_start && b.line->start == p->start) {
                b.line->start = b.line->end = p->end;
                continue;
            }
            if (o->wrap != CU_WRAP_NONE && b.content && b.x + w > b.max_w && !o->preformatted) {
                b.line->end = p->start;
                start_line(&b, p->end, true);
                continue;
            }
            add_run(&b, p->span, p->start, p->end, w);
            continue;
        }
        size_t before = b.lines.len;
        place_word(&b, p->span, p->start, p->end);
        if (o->max_lines && (int)b.lines.len > o->max_lines) {
            (void)before;
            ch_vec_remove(&b.lines, b.lines.len - 1);
            while (b.runs.len && ((TextRun *)b.runs.data[b.runs.len - 1])->start >= ((TextLine *)b.lines.data[b.lines.len - 1])->end)
                ch_vec_remove(&b.runs, b.runs.len - 1);
            break;
        }
    }
    free(ps);
    return finish(&b);
}

const char *text_layout_link(Cu *cu, const TextLayout *l, float x, float y) {
    for (int i = 0; i < l->line_count; i++) {
        const TextLine *line = &l->lines[i];
        if (y < line->y || y >= line->y + line->h)
            continue;
        for (int r = 0; r < line->run_count; r++) {
            const TextRun *run = &l->runs[line->first_run + r];
            if (x >= run->x && x < run->x + run->w)
                return l->spans[run->span].link;
        }
    }
    (void)cu;
    return NULL;
}

/* Geometry queries */
static float run_x(Cu *cu, const TextLayout *l, const TextRun *r, size_t offset) {
    if (offset <= r->start)
        return r->x;
    if (offset >= r->end)
        return r->x + r->w;
    return r->x + cu_text_width(cu, l->spans[r->span].font, l->text + r->start, offset - r->start);
}
static int line_of(const TextLayout *l, size_t offset) {
    for (int i = 0; i < l->line_count; i++) {
        const TextLine *line = &l->lines[i];
        bool last = i == l->line_count - 1;
        size_t next = last ? SIZE_MAX : l->lines[i + 1].start;
        if (offset >= line->start && (offset < next || last))
            return i;
    }
    return l->line_count - 1;
}
/* Where an offset falls along one line. The end of a wrapped line is also the start of
 * the next, so anything measuring a line must ask here rather than for the caret. */
static float line_x(Cu *cu, const TextLayout *l, const TextLine *line, size_t offset) {
    float x = line->x;
    for (int r = 0; r < line->run_count; r++) {
        const TextRun *run = &l->runs[line->first_run + r];
        if (offset >= run->start)
            x = run_x(cu, l, run, offset);
    }
    return x;
}
void text_layout_caret(Cu *cu, const TextLayout *l, size_t offset, float *x, float *y, float *h) {
    if (!l->line_count) {
        *x = *y = 0;
        *h = 16;
        return;
    }
    const TextLine *line = &l->lines[line_of(l, offset)];
    *x = line_x(cu, l, line, offset);
    *y = line->y;
    *h = line->h;
}
size_t text_layout_hit(Cu *cu, const TextLayout *l, float x, float y) {
    if (!l->line_count)
        return 0;
    int li = l->line_count - 1;
    if (y < 0)
        li = 0;
    else
        for (int i = 0; i < l->line_count; i++)
            if (y < l->lines[i].y + l->lines[i].h) {
                li = i;
                break;
            }
    const TextLine *line = &l->lines[li];
    if (!line->run_count || x <= l->runs[line->first_run].x)
        return line->run_count ? l->runs[line->first_run].start : line->start;
    for (int r = 0; r < line->run_count; r++) {
        const TextRun *run = &l->runs[line->first_run + r];
        if (x > run->x + run->w)
            continue;
        /* Binary search the character boundary nearest to x. */
        size_t best = run->start;
        float best_d = fabsf(x - run->x);
        const char *p = l->text + run->start, *end = l->text + run->end;
        while (p < end) {
            p = ch_utf8_next(p);
            size_t off = (size_t)(p - l->text);
            float d = fabsf(x - run_x(cu, l, run, off));
            if (d < best_d) {
                best_d = d;
                best = off;
            } else if (run_x(cu, l, run, off) > x)
                break;
        }
        return best;
    }
    const TextRun *last = &l->runs[line->first_run + line->run_count - 1];
    size_t end = last->end;
    if (end > 0 && end <= l->length && li < l->line_count - 1 && end == l->lines[li + 1].start)
        end = (size_t)(ch_utf8_prev(l->text, l->text + end) - l->text);
    return end;
}
void text_layout_paint(Cu *cu, CuCanvas *c, const TextLayout *l, float x, float y, size_t a, size_t b,
                       CuColor selection) {
    const CuPlatform *p = cu->p;
    for (int i = 0; i < l->line_count; i++) {
        const TextLine *line = &l->lines[i];
        /* Backgrounds, the selection, then letters: selected code is tinted like its neighbours. */
        for (int r = 0; r < line->run_count; r++) {
            const TextRun *run = &l->runs[line->first_run + r];
            const CuSpan *span = &l->spans[run->span];
            if (span->bg >> 24) {
                CuFontMetrics m = cu_font_metrics(cu, span->font);
                float top = y + line->y + line->ascent - m.ascent - 1;
                p->fill_rect(c, (CuRect){x + run->x - 2, top, run->w + 4, m.ascent + m.descent + 2}, 3,
                             cu_apply_alpha(cu, span->bg));
            }
        }
        if (a < b) {
            size_t next = i + 1 < l->line_count ? l->lines[i + 1].start : l->length + 1;
            size_t s = CH_MAX(a, line->start), e = CH_MIN(b, CH_MAX(line->end, line->start));
            if (s < next && b > line->start && a <= CH_MAX(line->end, line->start)) {
                float x1 = line_x(cu, l, line, s), x2 = line_x(cu, l, line, e);
                /* A selected line break shows, so an empty line in a selection can be seen. */
                if (b > line->end && i + 1 < l->line_count && l->text[line->end] == '\n')
                    x2 += 5;
                if (x2 > x1)
                    p->fill_rect(c, (CuRect){x + x1, y + line->y, x2 - x1, line->h}, 0, cu_apply_alpha(cu, selection));
            }
        }
        for (int r = 0; r < line->run_count; r++) {
            const TextRun *run = &l->runs[line->first_run + r];
            const CuSpan *span = &l->spans[run->span];
            p->draw_text(c, span->font, x + run->x, y + line->y + line->ascent, l->text + run->start,
                         run->end - run->start, cu_apply_alpha(cu, span->color));
            if (span->underline || span->strike) {
                CuFontMetrics m = cu_font_metrics(cu, span->font);
                float baseline = y + line->y + line->ascent, thick = fmaxf(1, m.height / 18);
                float ly = span->strike ? baseline - m.ascent * 0.3f : baseline + fmaxf(1.5f, m.descent * 0.35f);
                p->fill_rect(c, (CuRect){x + run->x, ly, run->w, thick}, 0, cu_apply_alpha(cu, span->color));
            }
        }
    }
}

/* Word boundaries for double-click and Ctrl+arrow movement. */
static bool word_char(const char *p) {
    unsigned char c = (unsigned char)*p;
    return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
size_t text_word_start(const char *text, size_t offset) {
    const char *p = text + offset;
    while (p > text) {
        const char *prev = ch_utf8_prev(text, p);
        if (!word_char(prev))
            break;
        p = prev;
    }
    return (size_t)(p - text);
}
size_t text_word_end(const char *text, size_t length, size_t offset) {
    const char *p = text + offset, *end = text + length;
    while (p < end && word_char(p))
        p = ch_utf8_next(p);
    return (size_t)(p - text);
}
