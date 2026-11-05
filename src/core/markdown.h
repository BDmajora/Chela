/* Markdown for chat transcripts: a CommonMark and GitHub subset that models actually
 * write, parsed into a flat list of blocks so the interface can draw it without any
 * tree walking. The parser never fails: text a model is still streaming, or text
 * that is not valid markdown, comes back as the closest readable blocks. */
#ifndef CHELA_MARKDOWN_H
#define CHELA_MARKDOWN_H
#include <stdbool.h>
#include <stddef.h>

typedef enum {
    CH_MD_PARAGRAPH,
    CH_MD_HEADING, /* level 1–6 */
    CH_MD_CODE,    /* text is verbatim; language may be empty */
    CH_MD_RULE,
    CH_MD_TABLE,
} ChMdKind;
enum { CH_MD_BOLD = 1, CH_MD_ITALIC = 2, CH_MD_MONO = 4, CH_MD_STRIKE = 8, CH_MD_LINK = 16 };
typedef enum { CH_MD_LEFT, CH_MD_CENTER, CH_MD_RIGHT } ChMdAlign;
typedef enum { CH_MD_NO_MARKER, CH_MD_BULLET, CH_MD_NUMBER, CH_MD_TASK_OPEN, CH_MD_TASK_DONE } ChMdMarker;

/* A run of text with one style. Spans cover text in order, with no gaps. */
typedef struct {
    size_t start, length;
    unsigned style;
    char *url; /* CH_MD_LINK */
} ChMdSpan;
typedef struct {
    char *text;
    ChMdSpan *spans;
    int span_count;
} ChMdInline;
typedef struct {
    ChMdKind kind;
    int level;       /* heading level */
    int list_depth;  /* 0 outside a list; nested items count up from 1 */
    int quote_depth; /* > nesting */
    ChMdMarker marker;
    char *number;    /* CH_MD_NUMBER: "3." or "3)" */
    bool tight;      /* next to the previous block of the same list, with no blank line */
    ChMdInline body; /* paragraph and heading text; code text in body.text */
    char *language;  /* CH_MD_CODE */
    bool unclosed;   /* a code fence that has not ended yet */
    /* CH_MD_TABLE: rows × columns cells; row 0 is the header. */
    int rows, columns;
    ChMdInline *cells;
    ChMdAlign *aligns;
} ChMdBlock;
typedef struct {
    ChMdBlock *blocks;
    size_t count;
} ChMdDocument;

ChMdDocument *ch_md_parse(const char *text);
void ch_md_free(ChMdDocument *doc);
/* Inline markdown alone: emphasis, code, links, escapes and entities. */
void ch_md_inline_parse(ChMdInline *out, const char *text, size_t length);
void ch_md_inline_clear(ChMdInline *inl);
#endif
