/* Edge cases for the pure logic behind the interface: markdown, load progress and
 * ETAs, attachments and clipboard choice. Included by test_core.c. */
#include "core/markdown.h"
#include <cairo.h>
#include <math.h>

static const char *span_text(const ChMdInline *inl, int i, char *out, size_t size) {
    size_t n = CH_MIN(inl->spans[i].length, size - 1);
    memcpy(out, inl->text + inl->spans[i].start, n);
    out[n] = 0;
    return out;
}
#define CHECK_SPAN(inl, i, text_, style_)                                                                    \
    do {                                                                                                     \
        char check_span[256];                                                                                \
        CHECK_INT((inl)->span_count, >, i);                                                                  \
        CHECK_STR(span_text(inl, i, check_span, sizeof(check_span)), ==, text_);                             \
        CHECK_INT((inl)->spans[i].style, ==, style_);                                                        \
    } while (0)
static void test_md_inline(void) {
    ChMdInline s;
    #define PARSE(t) ch_md_inline_parse(&s, t, strlen(t))
    PARSE("plain");
    CHECK_STR(s.text, ==, "plain");
    CHECK_INT(s.span_count, ==, 1);
    ch_md_inline_clear(&s);
    PARSE("a **bold** b");
    CHECK_STR(s.text, ==, "a bold b");
    CHECK_SPAN(&s, 1, "bold", CH_MD_BOLD);
    ch_md_inline_clear(&s);
    PARSE("__bold__ and *it* and _it2_");
    CHECK_STR(s.text, ==, "bold and it and it2");
    CHECK_SPAN(&s, 0, "bold", CH_MD_BOLD);
    CHECK_SPAN(&s, 2, "it", CH_MD_ITALIC);
    CHECK_SPAN(&s, 4, "it2", CH_MD_ITALIC);
    ch_md_inline_clear(&s);
    PARSE("***both***");
    CHECK_STR(s.text, ==, "both");
    CHECK_SPAN(&s, 0, "both", CH_MD_BOLD | CH_MD_ITALIC);
    ch_md_inline_clear(&s);
    PARSE("**bold *nested* more**");
    CHECK_STR(s.text, ==, "bold nested more");
    CHECK_SPAN(&s, 1, "nested", CH_MD_BOLD | CH_MD_ITALIC);
    ch_md_inline_clear(&s);
    PARSE("*it **bold** more*");
    CHECK_STR(s.text, ==, "it bold more");
    CHECK_SPAN(&s, 1, "bold", CH_MD_BOLD | CH_MD_ITALIC);
    ch_md_inline_clear(&s);
    /* Stars and underscores that are not emphasis. */
    const char *literal[] = {"2 * 3 * 4", "snake_case_name", "a_b", "1*", "**", "***", "****", "*", "_", "~", "~~",
                             "__init__.py is ** not closed", "`unclosed", "[not a link]", "[x](", "![", "&unknown;",
                             "<div>", "\\", "price: $5 * 3", NULL};
    const char *expected[] = {"2 * 3 * 4", "snake_case_name", "a_b", "1*", "**", "***", "****", "*", "_", "~", "~~",
                              "init.py is ** not closed", "`unclosed", "[not a link]", "[x](", "![", "&unknown;",
                              "<div>", "\\", "price: $5 * 3", NULL};
    for (int i = 0; literal[i]; i++) {
        check_message("inline: %s", literal[i]);
        PARSE(literal[i]);
        CHECK_STR(s.text, ==, expected[i]);
        ch_md_inline_clear(&s);
    }
    /* Code keeps its contents exactly. */
    PARSE("run `a **b** *c*` now");
    CHECK_STR(s.text, ==, "run a **b** *c* now");
    CHECK_SPAN(&s, 1, "a **b** *c*", CH_MD_MONO);
    ch_md_inline_clear(&s);
    PARSE("`` a`b ``");
    CHECK_STR(s.text, ==, "a`b");
    ch_md_inline_clear(&s);
    PARSE("**`code` in bold**");
    CHECK_SPAN(&s, 0, "code", CH_MD_MONO | CH_MD_BOLD);
    ch_md_inline_clear(&s);
    /* A ` in a code span does not hide the closing **. */
    PARSE("**a `*` b**");
    CHECK_STR(s.text, ==, "a * b");
    ch_md_inline_clear(&s);
    /* Escapes, entities and line breaks. */
    PARSE("\\*not it\\* \\[x\\] a\\_b");
    CHECK_STR(s.text, ==, "*not it* [x] a_b");
    CHECK_INT(s.span_count, ==, 1);
    ch_md_inline_clear(&s);
    PARSE("a &amp; b &lt;c&gt; &#39;d&#39;");
    CHECK_STR(s.text, ==, "a & b <c> 'd'");
    ch_md_inline_clear(&s);
    PARSE("one\ntwo");
    CHECK_STR(s.text, ==, "one two");
    ch_md_inline_clear(&s);
    PARSE("one  \ntwo\\\nthree<br>four");
    CHECK_STR(s.text, ==, "one\ntwo\nthree\nfour");
    ch_md_inline_clear(&s);
    PARSE("~~gone~~ kept");
    CHECK_SPAN(&s, 0, "gone", CH_MD_STRIKE);
    ch_md_inline_clear(&s);
    /* Links. */
    PARSE("see [the **docs**](https://a.test/x_y \"title\") now");
    CHECK_STR(s.text, ==, "see the docs now");
    CHECK_SPAN(&s, 1, "the ", CH_MD_LINK);
    CHECK_SPAN(&s, 2, "docs", CH_MD_LINK | CH_MD_BOLD);
    CHECK_STR(s.spans[1].url, ==, "https://a.test/x_y");
    ch_md_inline_clear(&s);
    PARSE("![alt text](https://a.test/i.png)");
    CHECK_STR(s.text, ==, "alt text");
    CHECK_INT(s.spans[0].style, ==, CH_MD_LINK);
    ch_md_inline_clear(&s);
    PARSE("[paren](https://a.test/x_(y))");
    CHECK_STR(s.spans[0].url, ==, "https://a.test/x_(y)");
    ch_md_inline_clear(&s);
    PARSE("<https://a.test/?q=1>");
    CHECK_STR(s.text, ==, "https://a.test/?q=1");
    CHECK_STR(s.spans[0].url, ==, "https://a.test/?q=1");
    ch_md_inline_clear(&s);
    PARSE("visit https://a.test/path_x, or (https://b.test/y). Done");
    CHECK_STR(s.text, ==, "visit https://a.test/path_x, or (https://b.test/y). Done");
    CHECK_SPAN(&s, 1, "https://a.test/path_x", CH_MD_LINK);
    CHECK_SPAN(&s, 3, "https://b.test/y", CH_MD_LINK);
    ch_md_inline_clear(&s);
    PARSE("nothttps://a.test");
    CHECK_INT(s.span_count, ==, 1);
    CHECK_INT(s.spans[0].style, ==, 0);
    ch_md_inline_clear(&s);
    /* UTF-8 and spans cover the text exactly. */
    PARSE("é **日本語** 🦀 *x*");
    CHECK_STR(s.text, ==, "é 日本語 🦀 x");
    size_t covered = 0;
    for (int i = 0; i < s.span_count; i++) {
        CHECK_INT(s.spans[i].start, ==, covered);
        covered += s.spans[i].length;
    }
    CHECK_INT(covered, ==, strlen(s.text));
    ch_md_inline_clear(&s);
    /* Deep nesting is bounded rather than recursing without limit. */
    ChBuf deep;
    ch_buf_init(&deep);
    for (int i = 0; i < 2000; i++)
        ch_buf_append(&deep, "*a **b ");
    PARSE(deep.str);
    CHECK_NONNULL(s.text);
    ch_md_inline_clear(&s);
    ch_buf_clear(&deep);
    #undef PARSE
}
static ChMdBlock *block(ChMdDocument *d, size_t i) {
    CHECK_INT(d->count, >, i);
    return &d->blocks[i];
}
static void test_md_blocks(void) {
    ChMdDocument *d = ch_md_parse("");
    CHECK_INT(d->count, ==, 0);
    ch_md_free(d);
    d = ch_md_parse(NULL);
    CHECK_INT(d->count, ==, 0);
    ch_md_free(d);
    d = ch_md_parse("\n\n   \n\t\n");
    CHECK_INT(d->count, ==, 0);
    ch_md_free(d);
    /* Lines of a paragraph join; a blank line separates. */
    d = ch_md_parse("one\ntwo\n\nthree\r\n");
    CHECK_INT(d->count, ==, 2);
    CHECK_STR(block(d, 0)->body.text, ==, "one two");
    CHECK_STR(block(d, 1)->body.text, ==, "three");
    ch_md_free(d);
    /* Headings. */
    d = ch_md_parse("# One\n## Two ##\n###### Six\n####### Seven\n#nospace\n#\nTitle\n===\nSub\n---\n");
    CHECK_INT(block(d, 0)->kind, ==, CH_MD_HEADING);
    CHECK_INT(block(d, 0)->level, ==, 1);
    CHECK_STR(block(d, 1)->body.text, ==, "Two");
    CHECK_INT(block(d, 2)->level, ==, 6);
    CHECK_INT(block(d, 3)->kind, ==, CH_MD_PARAGRAPH);
    CHECK_STR(block(d, 3)->body.text, ==, "####### Seven #nospace");
    CHECK_INT(block(d, 4)->kind, ==, CH_MD_HEADING);
    CHECK_STR(block(d, 5)->body.text, ==, "Title");
    CHECK_INT(block(d, 5)->level, ==, 1);
    CHECK_STR(block(d, 6)->body.text, ==, "Sub");
    CHECK_INT(block(d, 6)->level, ==, 2);
    ch_md_free(d);
    /* Rules. */
    const char *rules[] = {"---", "***", "___", "- - -", "* * *", " ---  ", "-----------", NULL};
    for (int i = 0; rules[i]; i++) {
        check_message("rule: %s", rules[i]);
        d = ch_md_parse(rules[i]);
        CHECK_INT(d->count, ==, 1);
        CHECK_INT(block(d, 0)->kind, ==, CH_MD_RULE);
        ch_md_free(d);
    }
    /* Code fences. */
    d = ch_md_parse("```c\nint a;\n\n  indented\n```\nafter\n");
    CHECK_INT(d->count, ==, 2);
    CHECK_INT(block(d, 0)->kind, ==, CH_MD_CODE);
    CHECK_STR(block(d, 0)->language, ==, "c");
    CHECK_STR(block(d, 0)->body.text, ==, "int a;\n\n  indented");
    CHECK_FALSE(block(d, 0)->unclosed);
    ch_md_free(d);
    d = ch_md_parse("~~~python title=x\nprint(1)\n~~~");
    CHECK_STR(block(d, 0)->language, ==, "python");
    ch_md_free(d);
    /* A longer fence holds shorter ones; a shorter one does not close it. */
    d = ch_md_parse("````md\n```\ninner\n```\n````\n");
    CHECK_INT(d->count, ==, 1);
    CHECK_STR(block(d, 0)->body.text, ==, "```\ninner\n```");
    ch_md_free(d);
    /* What is streaming now, including a fence still open. */
    d = ch_md_parse("text\n```js\nlet x");
    CHECK_INT(d->count, ==, 2);
    CHECK(block(d, 1)->unclosed);
    CHECK_STR(block(d, 1)->body.text, ==, "let x");
    ch_md_free(d);
    d = ch_md_parse("```");
    CHECK_INT(d->count, ==, 1);
    CHECK_STR(block(d, 0)->body.text, ==, "");
    ch_md_free(d);
    /* ``` with a backtick after it is inline code, not a fence. */
    d = ch_md_parse("``` a ``` b");
    CHECK_INT(block(d, 0)->kind, ==, CH_MD_PARAGRAPH);
    ch_md_free(d);
    /* Indented code, but only where no paragraph is open. */
    d = ch_md_parse("para\n    continues\n\n    code\n\n      more\ntext");
    CHECK_INT(d->count, ==, 3);
    CHECK_STR(block(d, 0)->body.text, ==, "para continues");
    CHECK_INT(block(d, 1)->kind, ==, CH_MD_CODE);
    CHECK_STR(block(d, 1)->body.text, ==, "code\n\n  more");
    ch_md_free(d);
    /* Quotes. */
    d = ch_md_parse("> one\n> two\n\n> > deep\n\nout\n");
    CHECK_INT(block(d, 0)->quote_depth, ==, 1);
    CHECK_STR(block(d, 0)->body.text, ==, "one two");
    CHECK_INT(block(d, 1)->quote_depth, ==, 2);
    CHECK_INT(block(d, 2)->quote_depth, ==, 0);
    ch_md_free(d);
    d = ch_md_parse("> lazy\ncontinuation\n# heading\n");
    CHECK_STR(block(d, 0)->body.text, ==, "lazy continuation");
    CHECK_INT(block(d, 1)->quote_depth, ==, 0);
    ch_md_free(d);
    d = ch_md_parse("> ```\n> code\nnot quoted");
    CHECK_INT(block(d, 0)->kind, ==, CH_MD_CODE);
    CHECK(block(d, 0)->unclosed);
    CHECK_STR(block(d, 1)->body.text, ==, "not quoted");
    ch_md_free(d);
}
static void test_md_lists(void) {
    ChMdDocument *d = ch_md_parse("- a\n- b\n* c\n+ d\n");
    CHECK_INT(d->count, ==, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_INT(block(d, (size_t)i)->marker, ==, CH_MD_BULLET);
        CHECK_INT(block(d, (size_t)i)->list_depth, ==, 1);
    }
    CHECK(block(d, 1)->tight);
    ch_md_free(d);
    d = ch_md_parse("1. one\n2) two\n10. ten\n");
    CHECK_INT(block(d, 0)->marker, ==, CH_MD_NUMBER);
    CHECK_STR(block(d, 0)->number, ==, "1.");
    CHECK_STR(block(d, 1)->number, ==, "2)");
    CHECK_STR(block(d, 2)->number, ==, "10.");
    CHECK_STR(block(d, 2)->body.text, ==, "ten");
    ch_md_free(d);
    /* Nesting at the indents models use. */
    d = ch_md_parse("- a\n  - b\n    - c\n  - d\n- e\n");
    int depths[] = {1, 2, 3, 2, 1};
    CHECK_INT(d->count, ==, 5);
    for (size_t i = 0; i < 5; i++)
        CHECK_INT(block(d, i)->list_depth, ==, depths[i]);
    ch_md_free(d);
    d = ch_md_parse("1. first\n   - under\n2. second\n    - four spaces\n");
    CHECK_INT(block(d, 1)->list_depth, ==, 2);
    CHECK_INT(block(d, 2)->list_depth, ==, 1);
    CHECK_INT(block(d, 3)->list_depth, ==, 2);
    ch_md_free(d);
    d = ch_md_parse("1. a\n  - two space child\n");
    CHECK_INT(block(d, 1)->list_depth, ==, 2);
    ch_md_free(d);
    /* A blank line keeps the list; its next item is loose. */
    d = ch_md_parse("- a\n\n- b\n\n  more of b\n\nout\n");
    CHECK_INT(d->count, ==, 4);
    CHECK_FALSE(block(d, 1)->tight);
    CHECK_INT(block(d, 2)->marker, ==, CH_MD_NO_MARKER);
    CHECK_INT(block(d, 2)->list_depth, ==, 1);
    CHECK_INT(block(d, 3)->list_depth, ==, 0);
    ch_md_free(d);
    /* Items continue over lazy lines, and a list can follow text directly. */
    d = ch_md_parse("Intro:\n- a\nlazy\n- b\n");
    CHECK_INT(d->count, ==, 3);
    CHECK_STR(block(d, 0)->body.text, ==, "Intro:");
    CHECK_STR(block(d, 1)->body.text, ==, "a lazy");
    ch_md_free(d);
    /* Only a list starting at 1 interrupts a paragraph. */
    d = ch_md_parse("In 2024. Then\n2024. was a year\n");
    CHECK_INT(d->count, ==, 1);
    ch_md_free(d);
    /* Task items. */
    d = ch_md_parse("- [ ] open\n- [x] done\n- [X] upper\n- [y] not a task\n* [ ]\n");
    CHECK_INT(block(d, 0)->marker, ==, CH_MD_TASK_OPEN);
    CHECK_STR(block(d, 0)->body.text, ==, "open");
    CHECK_INT(block(d, 1)->marker, ==, CH_MD_TASK_DONE);
    CHECK_INT(block(d, 2)->marker, ==, CH_MD_TASK_DONE);
    CHECK_INT(block(d, 3)->marker, ==, CH_MD_BULLET);
    CHECK_STR(block(d, 3)->body.text, ==, "[y] not a task");
    CHECK_INT(block(d, 4)->marker, ==, CH_MD_TASK_OPEN);
    ch_md_free(d);
    /* Blocks inside items. */
    d = ch_md_parse("1. Run:\n\n   ```sh\n   make\n     indented\n   ```\n\n2. Done\n");
    CHECK_INT(d->count, ==, 3);
    CHECK_INT(block(d, 1)->kind, ==, CH_MD_CODE);
    CHECK_INT(block(d, 1)->list_depth, ==, 1);
    CHECK_STR(block(d, 1)->body.text, ==, "make\n  indented");
    CHECK_STR(block(d, 2)->number, ==, "2.");
    ch_md_free(d);
    d = ch_md_parse("- # heading in item\n- ```\n  code\n  ```\n");
    CHECK_INT(block(d, 0)->kind, ==, CH_MD_HEADING);
    CHECK_INT(block(d, 0)->marker, ==, CH_MD_BULLET);
    CHECK_INT(block(d, 1)->kind, ==, CH_MD_CODE);
    CHECK_INT(block(d, 1)->marker, ==, CH_MD_BULLET);
    CHECK_STR(block(d, 1)->body.text, ==, "code");
    ch_md_free(d);
    /* Empty items still draw, and markers need a following space. */
    d = ch_md_parse("-\n- \n1.\n-not a list\n1.not one\n");
    CHECK_INT(d->count, ==, 4);
    CHECK_INT(block(d, 0)->marker, ==, CH_MD_BULLET);
    CHECK_STR(block(d, 0)->body.text, ==, "");
    CHECK_INT(block(d, 2)->marker, ==, CH_MD_NUMBER);
    CHECK_STR(block(d, 3)->body.text, ==, "-not a list 1.not one");
    ch_md_free(d);
    /* Excessive nesting stays bounded and keeps its text. */
    ChBuf deep;
    ch_buf_init(&deep);
    for (int i = 0; i < 200; i++) {
        for (int k = 0; k < i * 2; k++)
            ch_buf_append_c(&deep, ' ');
        ch_buf_printf(&deep, "- level %d\n", i);
    }
    d = ch_md_parse(deep.str);
    CHECK_INT(d->count, >, 0);
    bool found = false;
    for (size_t i = 0; i < d->count; i++)
        found |= d->blocks[i].body.text && strstr(d->blocks[i].body.text, "level 199");
    CHECK(found);
    ch_md_free(d);
    ch_buf_clear(&deep);
}
static void test_md_tables(void) {
    ChMdDocument *d = ch_md_parse("| A | **B** | C |\n|:--|:-:|--:|\n| 1 | `a|b` | 3 |\n| 4 | 5 |\n\nafter\n");
    CHECK_INT(d->count, ==, 2);
    ChMdBlock *t = block(d, 0);
    CHECK_INT(t->kind, ==, CH_MD_TABLE);
    CHECK_INT(t->columns, ==, 3);
    CHECK_INT(t->rows, ==, 3);
    CHECK_INT(t->aligns[0], ==, CH_MD_LEFT);
    CHECK_INT(t->aligns[1], ==, CH_MD_CENTER);
    CHECK_INT(t->aligns[2], ==, CH_MD_RIGHT);
    CHECK_STR(t->cells[1].text, ==, "B");
    CHECK_INT(t->cells[1].spans[0].style, ==, CH_MD_BOLD);
    CHECK_STR(t->cells[4].text, ==, "a|b");
    CHECK_STR(t->cells[8].text, ==, ""); /* short rows are padded */
    ch_md_free(d);
    /* Without outer pipes, and with extra cells dropped. */
    d = ch_md_parse("a | b\n---|---\n1 | 2 | 3\n");
    t = block(d, 0);
    CHECK_INT(t->kind, ==, CH_MD_TABLE);
    CHECK_INT(t->columns, ==, 2);
    CHECK_STR(t->cells[3].text, ==, "2");
    ch_md_free(d);
    /* Escaped pipes stay in the cell. */
    d = ch_md_parse("| a\\|b |\n|---|\n");
    CHECK_STR(block(d, 0)->cells[0].text, ==, "a|b");
    ch_md_free(d);
    /* Text followed by a table: the table interrupts it. */
    d = ch_md_parse("Results:\n| a |\n|---|\n| 1 |\n");
    CHECK_INT(d->count, ==, 2);
    CHECK_INT(block(d, 1)->kind, ==, CH_MD_TABLE);
    ch_md_free(d);
    /* Not tables: mismatched header, or a header alone while it is still streaming. */
    const char *plain[] = {"| a | b |\n|---|\n", "| a | b |", "| a | b |\n", "a|b\n--\n", "|\n|\n", NULL};
    for (int i = 0; plain[i]; i++) {
        check_message("not a table: %s", plain[i]);
        d = ch_md_parse(plain[i]);
        for (size_t k = 0; k < d->count; k++)
            CHECK_INT(d->blocks[k].kind, !=, CH_MD_TABLE);
        ch_md_free(d);
    }
    /* In a list and a quote. */
    d = ch_md_parse("- item\n\n  | a |\n  |---|\n  | 1 |\n");
    CHECK_INT(block(d, 1)->kind, ==, CH_MD_TABLE);
    CHECK_INT(block(d, 1)->list_depth, ==, 1);
    ch_md_free(d);
    d = ch_md_parse("> | a |\n> |---|\n> | 1 |\n");
    CHECK_INT(block(d, 0)->kind, ==, CH_MD_TABLE);
    CHECK_INT(block(d, 0)->quote_depth, ==, 1);
    CHECK_STR(block(d, 0)->cells[1].text, ==, "1");
    ch_md_free(d);
}
/* Every prefix of a document, which is what streaming produces, must parse. */
static void test_md_streaming(void) {
    const char *text = "# T\n\nIntro **bold\n\n- a\n  - `b\n\n1. c\n\n   ```py\n   x = 1\n   ```\n\n| a | b |\n|---|:-:|\n"
                       "| 1 | [l](http://x.test) |\n\n> q\n> ```\n> c\n\n---\n~~~\nlast";
    size_t n = strlen(text);
    for (size_t i = 0; i <= n; i++) {
        char *part = ch_strndup(text, i);
        ChMdDocument *d = ch_md_parse(part);
        for (size_t k = 0; k < d->count; k++) {
            ChMdBlock *b = &d->blocks[k];
            for (int r = 0; b->cells && r < b->rows * b->columns; r++)
                CHECK_NONNULL(b->cells[r].text);
            if (b->kind != CH_MD_RULE && b->kind != CH_MD_TABLE)
                CHECK_NONNULL(b->body.text);
        }
        ch_md_free(d);
        free(part);
    }
    /* Arbitrary bytes do not crash it. */
    ChBuf noise;
    ch_buf_init(&noise);
    unsigned seed = 7;
    const char alphabet[] = "*_`~[]()<>#>-|:\\\n\n  1.xé\t!&";
    for (int round = 0; round < 300; round++) {
        ch_buf_truncate(&noise, 0);
        for (int i = 0; i < 200; i++) {
            seed = seed * 1103515245u + 12345u;
            ch_buf_append_c(&noise, alphabet[(seed >> 16) % (sizeof(alphabet) - 1)]);
        }
        ChMdDocument *d = ch_md_parse(noise.str);
        ch_md_free(d);
    }
    ch_buf_clear(&noise);
}

static ChLoadSample load_sample(ChLoadPhase phase, double target, double mapped) {
    return (ChLoadSample){phase, target, mapped, 8000, -1};
}
#define S(x) ((int64_t)((x)*1000000LL))
static void test_load_progress(void) {
    ChLoadTrack t;
    /* Nothing reported and no history: an empty bar and no made-up ETA. */
    ch_load_begin(&t, S(100), 0, 0, 0);
    ChLoadEstimate e = ch_load_estimate(&t, S(130));
    CHECK_FLOAT(e.fraction, ==, 0);
    CHECK_FLOAT(e.eta, <, 0);
    CHECK_FALSE(e.measured);
    /* With history alone, it follows the last load and stops short of the end. */
    ch_load_begin(&t, S(0), 60, 0, 0);
    e = ch_load_estimate(&t, S(30));
    CHECK_FLOAT(fabs(e.fraction - .5), <, .001);
    CHECK_FLOAT(fabs(e.eta - 30), <, .001);
    e = ch_load_estimate(&t, S(500));
    CHECK_FLOAT(e.fraction, <=, .97);
    CHECK_FLOAT(e.eta, ==, 0);
    CHECK(e.overdue);
    /* Measured reading: progress and ETA come from bytes. */
    ch_load_begin(&t, S(0), 0, 0, 0);
    ChLoadSample s = load_sample(CH_LOAD_READING, 10000, 1000);
    ch_load_sample(&t, &s, S(2));
    e = ch_load_estimate(&t, S(2));
    CHECK(e.measured);
    CHECK_FLOAT(e.fraction, >, .05);
    CHECK_FLOAT(e.fraction, <, .15);
    CHECK_FLOAT(e.eta, <, 0); /* one reading has no rate yet */
    s.mapped_mib = 3000;
    ch_load_sample(&t, &s, S(4));
    e = ch_load_estimate(&t, S(4));
    CHECK_FLOAT(e.rate_mib, ==, 1000);
    /* 7000 MiB at 1000 MiB/s, plus 3 s of setup and 8000 MiB of copying. */
    CHECK_FLOAT(fabs(e.eta - (7 + 3 + 8000.0 / 2048)), <, .5);
    /* Between reports the bar keeps moving at the measured rate, and the ETA counts down. */
    ChLoadEstimate later = ch_load_estimate(&t, S(5));
    CHECK_FLOAT(later.fraction, >, e.fraction);
    CHECK_FLOAT(fabs((e.eta - later.eta) - 1), <, .001);
    /* It never overtakes the file while still reading. */
    later = ch_load_estimate(&t, S(4000));
    CHECK_FLOAT(later.fraction, <, .81);
    CHECK_FLOAT(later.eta, <, 0); /* twenty quiet seconds: the ETA is withdrawn */
    CHECK(later.stalled);
    /* Stage changes move it on, and the bar never moves back. */
    ch_load_begin(&t, S(0), 0, 0, 0);
    double previous = 0;
    struct {
        ChLoadPhase phase;
        double mapped;
        int percent, at;
    } steps[] = {{CH_LOAD_STARTING, 0, -1, 1},  {CH_LOAD_READING, 2000, -1, 3}, {CH_LOAD_READING, 6000, -1, 5},
                 {CH_LOAD_READING, 1000, -1, 7}, {CH_LOAD_UPLOADING, 9000, -1, 9}, {CH_LOAD_UPLOADING, 9000, 10, 11},
                 {CH_LOAD_UPLOADING, 9000, 60, 13}, {CH_LOAD_UPLOADING, 9000, 40, 15}, {CH_LOAD_CONTEXT, 9000, -1, 17},
                 {CH_LOAD_READING, 0, -1, 19}, {CH_LOAD_WARMING, 9000, -1, 21}};
    for (size_t i = 0; i < CH_N_ELEMENTS(steps); i++) {
        s = load_sample(steps[i].phase, 10000, steps[i].mapped);
        s.upload_percent = steps[i].percent;
        ch_load_sample(&t, &s, S(steps[i].at));
        e = ch_load_estimate(&t, S(steps[i].at));
        CHECK_FLOAT(e.fraction, >=, previous);
        CHECK_FLOAT(e.fraction, <=, .99);
        previous = e.fraction;
    }
    CHECK_INT(t.last.phase, ==, CH_LOAD_WARMING); /* a stale report does not rewind the stage */
    CHECK_FLOAT(ch_load_tail_seconds(&t, S(29)), ==, 20);
    /* A file that shrinks the estimate is fine; a zero target is not a division. */
    ch_load_begin(&t, S(0), 0, 0, 0);
    s = load_sample(CH_LOAD_READING, 0, 0);
    ch_load_sample(&t, &s, S(1));
    s.mapped_mib = 500;
    ch_load_sample(&t, &s, S(2));
    e = ch_load_estimate(&t, S(2));
    CHECK_FLOAT(e.fraction, ==, e.fraction);
    CHECK_FLOAT(e.fraction, <=, .03);
    /* Memory already larger than the file does not overflow the bar. */
    ch_load_begin(&t, S(0), 0, 0, 0);
    s = load_sample(CH_LOAD_READING, 1000, 5000);
    ch_load_sample(&t, &s, S(1));
    e = ch_load_estimate(&t, S(1));
    CHECK_FLOAT(e.fraction, <=, .8);
    /* Clocks that step backwards and repeated reports are ignored. */
    ch_load_begin(&t, S(50), 0, 0, 0);
    s = load_sample(CH_LOAD_READING, 1000, 100);
    ch_load_sample(&t, &s, S(60));
    ch_load_sample(&t, &s, S(55));
    ch_load_sample(&t, &s, S(60));
    e = ch_load_estimate(&t, S(40));
    CHECK_FLOAT(e.fraction, ==, e.fraction);
    /* The last load says a shard is lazily mapped, so the target is the part really read. */
    ch_load_begin(&t, S(0), 66, 6, 30000);
    s = load_sample(CH_LOAD_READING, 58000, 15000);
    ch_load_sample(&t, &s, S(30));
    e = ch_load_estimate(&t, S(30));
    /* Half of the 30 GiB, not a quarter of 58, with reading worth 90% of this load's time. */
    CHECK_FLOAT(e.fraction, >, .4);
    CHECK_FLOAT(e.fraction, <, .5);
    char *text = ch_load_phase_text(&t, &e);
    CHECK_NONNULL(strstr(text, "of 29.3 GiB"));
    free(text);
    /* But a load that reads past the old target keeps counting. */
    s = load_sample(CH_LOAD_READING, 58000, 40000);
    ch_load_sample(&t, &s, S(40));
    e = ch_load_estimate(&t, S(40));
    CHECK_FLOAT(e.fraction, <=, .91);
    /* Rates that drop to nothing do not leave a stale ETA forever. */
    ch_load_begin(&t, S(0), 0, 0, 0);
    s = load_sample(CH_LOAD_READING, 10000, 1000);
    ch_load_sample(&t, &s, S(2));
    s.mapped_mib = 1001;
    for (int i = 1; i < 30; i++)
        ch_load_sample(&t, &s, S(2 + i * 2));
    e = ch_load_estimate(&t, S(60));
    CHECK(e.stalled);
    CHECK_FLOAT(e.eta, <, 0);
    /* Invalid history is treated as none. */
    ch_load_begin(&t, S(0), NAN, -3, INFINITY);
    CHECK_FLOAT(t.previous_seconds, ==, 0);
    CHECK_FLOAT(t.previous_tail, ==, 0);
    CHECK_FLOAT(t.previous_mapped, ==, 0);
}
static void test_load_text(void) {
    struct {
        double eta;
        const char *text;
    } cases[] = {{-1, NULL}, {NAN, NULL}, {INFINITY, NULL}, {0, "Almost done"}, {0.2, "1s left"}, {1, "1s left"},
                 {59.1, "1m 00s left"}, {60, "1m 00s left"}, {61, "1m 01s left"}, {3599, "59m 59s left"},
                 {3600, "1h 00m left"}, {7384, "2h 03m left"}};
    for (size_t i = 0; i < CH_N_ELEMENTS(cases); i++) {
        char *text = ch_load_eta_text(cases[i].eta);
        CHECK_STR(text, ==, cases[i].text);
        free(text);
    }
    ChLoadSample s;
    ChError *e = NULL;
    json_object *o = ch_json_parse("{\"phase\":\"uploading\",\"target_mib\":100.5,\"mapped_mib\":50,\"gpu_mib\":2048,\"upload_percent\":250}", &e);
    CHECK_NO_ERROR(e);
    CHECK(ch_load_sample_parse(o, &s));
    CHECK_INT(s.phase, ==, CH_LOAD_UPLOADING);
    CHECK_FLOAT(s.target_mib, ==, 100.5);
    CHECK_INT(s.upload_percent, ==, 100);
    json_object_put(o);
    o = ch_json_parse("{\"phase\":\"warming\",\"target_mib\":-5,\"mapped_mib\":\"x\"}", &e);
    CHECK(ch_load_sample_parse(o, &s));
    CHECK_FLOAT(s.target_mib, ==, 0);
    CHECK_FLOAT(s.mapped_mib, ==, 0);
    CHECK_INT(s.upload_percent, ==, -1);
    json_object_put(o);
    const char *bad[] = {"{}", "{\"phase\":\"nope\"}", "{\"phase\":1}", "[]", "1", NULL};
    for (int i = 0; bad[i]; i++) {
        o = ch_json_parse(bad[i], &e);
        CHECK_NO_ERROR(e);
        CHECK_FALSE(ch_load_sample_parse(o, &s));
        json_object_put(o);
    }
    CHECK_FALSE(ch_load_sample_parse(NULL, &s));
    /* Each stage has words, including when nothing is known yet. */
    ChLoadTrack t;
    ch_load_begin(&t, 0, 0, 0, 0);
    ChLoadEstimate estimate = ch_load_estimate(&t, 0);
    char *text = ch_load_phase_text(&t, &estimate);
    CHECK_STR(text, ==, "Waiting for the server");
    free(text);
    ChLoadPhase phases[] = {CH_LOAD_STARTING, CH_LOAD_READING, CH_LOAD_UPLOADING, CH_LOAD_CONTEXT, CH_LOAD_WARMING};
    for (size_t i = 0; i < CH_N_ELEMENTS(phases); i++) {
        ChLoadSample sample = {phases[i], 0, 0, 0, -1};
        ch_load_sample(&t, &sample, 0);
        text = ch_load_phase_text(&t, &estimate);
        CHECK(*text);
        free(text);
    }
}

static char *png_bytes(int width, int height, size_t *size) {
    ChImage image = {width, height, ch_new0(uint32_t, (size_t)width * (size_t)height)};
    for (int i = 0; i < width * height; i++)
        image.pixels[i] = 0xff336699;
    char *png = ch_image_png(&image, size);
    free(image.pixels);
    return png;
}
static void test_images(void) {
    ChError *e = NULL;
    size_t size = 0;
    char *png = png_bytes(40, 20, &size);
    CHECK_NONNULL(png);
    CHECK_STR(ch_mime_sniff(png, size), ==, "image/png");
    ChImage *image = ch_image_decode(png, size, &e);
    CHECK_NO_ERROR(e);
    CHECK_INT(image->width, ==, 40);
    CHECK_INT(image->height, ==, 20);
    CHECK_INT(image->pixels[0], ==, 0xff336699);
    ChImage *small = ch_image_fit(image, 10);
    CHECK_INT(small->width, ==, 10);
    CHECK_INT(small->height, ==, 5);
    CHECK_INT(small->pixels[12], ==, 0xff336699);
    ch_image_free(small);
    small = ch_image_fit(image, 4000); /* never enlarges */
    CHECK_INT(small->width, ==, 40);
    ch_image_free(small);
    ch_image_free(image);
    /* An extreme aspect ratio keeps at least one pixel. */
    ChImage tall = {1, 4000, ch_new0(uint32_t, 4000)};
    small = ch_image_fit(&tall, 16);
    CHECK_INT(small->width, ==, 1);
    CHECK_INT(small->height, ==, 16);
    ch_image_free(small);
    free(tall.pixels);
    /* Damaged data: a truncated file, flipped header bytes, an absurd declared size. */
    for (size_t cut = 0; cut < size; cut += CH_MAX(size / 12, 1)) {
        image = ch_image_decode(png, cut, &e);
        CHECK_NULL(image);
        CHECK_NONNULL(e);
        ch_clear_error(&e);
    }
    char *bomb = ch_alloc(size);
    memcpy(bomb, png, size);
    bomb[16] = 0x7f;
    image = ch_image_decode(bomb, size, &e);
    CHECK_NULL(image);
    ch_clear_error(&e);
    free(bomb);
    free(png);
    /* Formats by content, not by name. */
    struct {
        const char *bytes;
        size_t n;
        const char *mime;
    } magic[] = {{"\xff\xd8\xff\xe0", 4, "image/jpeg"}, {"GIF89a", 6, "image/gif"}, {"RIFFxxxxWEBPVP8 ", 16, "image/webp"},
                 {"BM", 2, "image/bmp"}, {"II*\0", 4, "image/tiff"}, {"MM\0*", 4, "image/tiff"}, {"%PDF-1.7", 8, "application/pdf"},
                 {"hello", 5, NULL}, {"", 0, NULL}, {"\x89PNG", 4, NULL}, {"RIFFxxxxWAVE", 12, NULL}};
    for (size_t i = 0; i < CH_N_ELEMENTS(magic); i++) {
        CHECK_STR(ch_mime_sniff(magic[i].bytes, magic[i].n), ==, magic[i].mime);
    }
    image = ch_image_decode("not an image", 12, &e);
    CHECK_NULL(image);
    CHECK_NONNULL(e);
    ch_clear_error(&e);
    /* A corrupt JPEG or WebP header is an error, not a crash, with or without the libraries. */
    image = ch_image_decode("\xff\xd8\xff\xe0garbage-after-the-marker", 27, &e);
    CHECK_NULL(image);
    ch_clear_error(&e);
    image = ch_image_decode("RIFF\x10\0\0\0WEBPVP8 garbage", 24, &e);
    CHECK_NULL(image);
    ch_clear_error(&e);
}
static void test_attachments(void) {
    char *dir = temp_dir(), *store = ch_build_path(dir, "store", NULL);
    ChError *e = NULL;
    /* Images are copied, with their size recorded. */
    size_t size = 0;
    char *png = png_bytes(12, 8, &size);
    ChAttachment *a = ch_attachment_from_bytes(png, size, "Pasted image", store, &e);
    CHECK_NO_ERROR(e);
    CHECK_INT(a->kind, ==, CH_ATTACH_IMAGE);
    CHECK_STR(a->mime, ==, "image/png");
    CHECK_INT(a->width, ==, 12);
    CHECK(ch_str_has_suffix(a->path, ".png"));
    CHECK(ch_file_is_regular(a->path));
    struct stat st;
    CHECK_INT(stat(a->path, &st), ==, 0);
    CHECK_INT(st.st_mode & 0777, ==, 0600);
    /* The same bytes are the same stored file. */
    ChAttachment *again = ch_attachment_from_bytes(png, size, "Other name", store, &e);
    CHECK_STR(again->path, ==, a->path);
    ch_attachment_free(again);
    /* Text from disk, with the name of the file. */
    char *text_path = ch_build_path(dir, "notes é.md", NULL);
    CHECK(ch_file_set_contents(text_path, "# Notes\n```\ncode\n```\n", -1, 0644, &e));
    ChAttachment *text = ch_attachment_from_path(text_path, store, &e);
    CHECK_NO_ERROR(e);
    CHECK_INT(text->kind, ==, CH_ATTACH_TEXT);
    CHECK_STR(text->name, ==, "notes é.md");
    /* Empty, too large, missing, damaged. */
    char *empty = ch_build_path(dir, "empty", NULL);
    CHECK(ch_file_set_contents(empty, "", 0, 0644, &e));
    CHECK_NULL(ch_attachment_from_path(empty, store, &e));
    CHECK_NONNULL(strstr(e->message, "empty"));
    ch_clear_error(&e);
    char *big = ch_alloc(CH_ATTACH_TEXT_LIMIT + 2);
    memset(big, 'x', CH_ATTACH_TEXT_LIMIT + 1);
    big[CH_ATTACH_TEXT_LIMIT + 1] = 0;
    CHECK_NULL(ch_attachment_from_bytes(big, CH_ATTACH_TEXT_LIMIT + 1, "big.txt", store, &e));
    CHECK_NONNULL(strstr(e->message, "256 KiB"));
    ch_clear_error(&e);
    ChAttachment *fits = ch_attachment_from_bytes(big, CH_ATTACH_TEXT_LIMIT, "fits.txt", store, &e);
    CHECK_NO_ERROR(e);
    ch_attachment_free(fits);
    free(big);
    CHECK_NULL(ch_attachment_from_path("/nonexistent/file", store, &e));
    ch_clear_error(&e);
    CHECK_NULL(ch_attachment_from_bytes("\x89PNG\r\n\x1a\n-broken", 15, "broken.png", store, &e));
    ch_clear_error(&e);
    /* Invalid UTF-8 and binary files are mentioned by path, not inlined. */
    char *binary = ch_build_path(dir, "tool.bin", NULL);
    CHECK(ch_file_set_contents(binary, "a\xffz\0b", 5, 0644, &e));
    ChAttachment *file = ch_attachment_from_path(binary, store, &e);
    CHECK_NO_ERROR(e);
    CHECK_INT(file->kind, ==, CH_ATTACH_FILE);
    CHECK_STR(file->path, ==, binary);
    CHECK(file->external);
    /* A folder too. */
    ChAttachment *folder = ch_attachment_from_path(dir, store, &e);
    CHECK_NO_ERROR(e);
    CHECK_INT(folder->kind, ==, CH_ATTACH_FILE);
    CHECK_STR(folder->mime, ==, "inode/directory");
    /* The message sent to a model carries all of them, and none of Chela's own fields. */
    json_object *m = ch_message("user", "look");
    json_object *list = json_object_new_array();
    ChAttachment *all[] = {a, text, file, folder};
    for (size_t i = 0; i < CH_N_ELEMENTS(all); i++)
        json_object_array_add(list, ch_attachment_json(all[i]));
    json_object_object_add(m, "_chela_attachments", list);
    json_object_object_add(m, "_chela_stats", json_object_new_object());
    json_object *wire = ch_wire_message(m, true);
    CHECK_NULL(ch_json_get(wire, "_chela_attachments"));
    CHECK_NULL(ch_json_get(wire, "_chela_stats"));
    json_object *parts = ch_json_get(wire, "content");
    CHECK(json_object_is_type(parts, json_type_array));
    CHECK_INT(json_object_array_length(parts), ==, 2);
    json_object *first = json_object_array_get_idx(parts, 0), *second = json_object_array_get_idx(parts, 1);
    const char *sent = ch_json_str(first, "text");
    CHECK(ch_str_has_prefix(sent, "look\n\nAttached file: notes é.md\n````\n# Notes"));
    CHECK_NONNULL(strstr(sent, "not included in this message: tool.bin"));
    CHECK_NONNULL(strstr(sent, "Attached folder"));
    CHECK_STR(ch_json_str(second, "type"), ==, "image_url");
    CHECK(ch_str_has_prefix(ch_json_str(ch_json_get(second, "image_url"), "url"), "data:image/png;base64,iVBORw0KGgo"));
    json_object_put(wire);
    /* A model without vision is told about an image rather than refusing the whole request. */
    wire = ch_wire_message(m, false);
    CHECK(json_object_is_type(ch_json_get(wire, "content"), json_type_string));
    CHECK_NONNULL(strstr(ch_json_str(wire, "content"), "Attached image Pasted image was left out"));
    CHECK_NONNULL(strstr(ch_json_str(wire, "content"), "Attached file: notes é.md"));
    json_object_put(wire);
    /* Harnesses take text, so images are named instead. */
    char *plain = ch_message_text(m);
    CHECK(ch_str_has_prefix(plain, "look\n\nAttached image: Pasted image ("));
    free(plain);
    /* A stored file that has gone is reported in the message rather than failing the request. */
    unlink(a->path);
    unlink(text->path);
    wire = ch_wire_message(m, true);
    CHECK(json_object_is_type(ch_json_get(wire, "content"), json_type_string));
    CHECK_NONNULL(strstr(ch_json_str(wire, "content"), "Attached image Pasted image is no longer available"));
    CHECK_NONNULL(strstr(ch_json_str(wire, "content"), "notes é.md is no longer available"));
    json_object_put(wire);
    json_object_put(m);
    /* An image alone sends no empty text part. */
    a->path = (free(a->path), ch_build_path(dir, "only.png", NULL));
    CHECK(ch_file_set_contents(a->path, png, (ptrdiff_t)size, 0600, &e));
    m = ch_message("user", "");
    list = json_object_new_array();
    json_object_array_add(list, ch_attachment_json(a));
    json_object_object_add(m, "_chela_attachments", list);
    wire = ch_wire_message(m, true);
    CHECK_INT(json_object_array_length(ch_json_get(wire, "content")), ==, 1);
    json_object_put(wire);
    json_object_put(m);
    /* An ordinary message passes through unchanged. */
    m = ch_message("assistant", "hi");
    json_object_object_add(m, "tool_calls", json_object_new_array());
    wire = ch_wire_message(m, true);
    CHECK_STR(ch_json_str(wire, "content"), ==, "hi");
    CHECK_NONNULL(ch_json_get(wire, "tool_calls"));
    json_object_put(wire);
    json_object_put(m);
    ch_attachment_free(a);
    ch_attachment_free(text);
    ch_attachment_free(file);
    ch_attachment_free(folder);
    free(png);
    free(text_path);
    free(empty);
    free(binary);
    free(store);
    remove_tree(dir);
    free(dir);
}
static void test_base64(void) {
    struct {
        const char *in, *out;
    } cases[] = {{"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"}};
    for (size_t i = 0; i < CH_N_ELEMENTS(cases); i++) {
        char *out = ch_base64_encode(cases[i].in, strlen(cases[i].in));
        CHECK_STR(out, ==, cases[i].out);
        free(out);
    }
    char *out = ch_base64_encode("\xff\xfe\xfd", 3);
    CHECK_STR(out, ==, "//79");
    free(out);
}
static void test_uri_list(void) {
    ChVec *p = ch_uri_list_parse("# comment\r\nfile:///home/me/a%20b.png\r\nfile://localhost/tmp/x\r\n\r\n"
                                 "https://a.test/file.png\nfile://otherhost/y\nfile:/one/slash\nfile:///caf%C3%A9.txt\n"
                                 "file:///bad%zz\nfile:///nul%00byte\nfile:///trunc%2\nfile:///q?x=1#frag\nFILE:///UPPER\n");
    const char *expected[] = {"/home/me/a b.png", "/tmp/x", "/one/slash", "/café.txt", "/q", "/UPPER"};
    CHECK_INT(p->len, ==, CH_N_ELEMENTS(expected));
    for (size_t i = 0; i < CH_N_ELEMENTS(expected); i++)
        CHECK_STR(p->data[i], ==, expected[i]);
    ch_vec_free(p);
    /* A file manager's copied-files format: an operation, then URIs. */
    p = ch_uri_list_parse("copy\nfile:///a\nfile:///b");
    CHECK_INT(p->len, ==, 2);
    ch_vec_free(p);
    p = ch_uri_list_parse("");
    CHECK_INT(p->len, ==, 0);
    ch_vec_free(p);
    p = ch_uri_list_parse(NULL);
    CHECK_INT(p->len, ==, 0);
    ch_vec_free(p);
}
static void test_clipboard_choice(void) {
    const char *files[] = {"text/plain", "text/uri-list", "image/png"};
    CHECK_INT(ch_clipboard_best(files, 3), ==, 1);
    const char *nautilus[] = {"UTF8_STRING", "x-special/gnome-copied-files"};
    CHECK_INT(ch_clipboard_best(nautilus, 2), ==, 1);
    const char *screenshot[] = {"image/png"};
    CHECK_INT(ch_clipboard_best(screenshot, 1), ==, 0);
    const char *browser[] = {"text/html", "image/jpeg", "image/png", "image/bmp"};
    CHECK_INT(ch_clipboard_best(browser, 4), ==, 2);
    const char *sheet[] = {"image/png", "text/html", "text/plain", "text/plain;charset=utf-8"};
    CHECK_INT(ch_clipboard_best(sheet, 4), ==, 3);
    const char *text[] = {"STRING", "TEXT", "UTF8_STRING"};
    CHECK_INT(ch_clipboard_best(text, 3), ==, 0);
    const char *unknown[] = {"application/x-thing", "text/html"};
    CHECK_INT(ch_clipboard_best(unknown, 2), ==, -1);
    CHECK_INT(ch_clipboard_best(NULL, 0), ==, -1);
}
static void test_model_short_name(void) {
    struct {
        const char *in, *out;
    } cases[] = {{"Qwen3.8-Flash-Next-GSQ-RCO-IQ1_M-ccb740ad", "Qwen3.8-Flash-Next-GSQ-RCO-IQ1_M"},
                 {"plain-model", "plain-model"}, {"", ""}, {"-abcdef12", "-abcdef12"}, {"a-abcdef12", "a"},
                 {"model-ABCDEF12", "model-ABCDEF12"}, {"model-abcdefg1", "model-abcdefg1"}, {"model-abcdef1", "model-abcdef1"},
                 {"model_abcdef12", "model_abcdef12"}};
    for (size_t i = 0; i < CH_N_ELEMENTS(cases); i++) {
        char *name = ch_model_short_name(cases[i].in);
        CHECK_STR(name, ==, cases[i].out);
        free(name);
    }
}
static void test_model_family_name(void) {
    struct {
        const char *in, *out;
    } cases[] = {{"Qwen3.8-Flash-Next-GSQ-RCO-IQ1_M-ccb740ad", "qwen3.8"},
                 {"OpenAI-20B-NEO-CODE-DI-Uncensored-Q8_0-ecda383c", "gpt-oss"},
                 {"gemma-4-26B-A4B-heretic-APEX-I-Quality-fb467308", "gemma4"},
                 {"gpt-oss-20b-Q8_0", "gpt-oss"}, {"llama-3.1-70b-instruct", "llama3.1"},
                 {"unsloth/gemma-4-GGUF", "gemma4"}, {"qwen3.8:latest", "qwen3.8"}, {"Qwen3.8", "qwen3.8"},
                 /* No version at the start: the first word alone would say nothing. */
                 {"local-model", "local-model"}, {"qwen-flash-next", "qwen-flash-next"}, {"Second-Q6", "Second-Q6"},
                 {"gpt-4o-mini", "gpt-4o-mini"}, {"ACP harness", "ACP harness"}, {"plain-model-abcdef12", "plain-model"},
                 {"", ""}, {"-abcdef12", "-abcdef12"}, {"4-model", "4-model"}, {"gemma-", "gemma-"}, {"gemma", "gemma"}};
    for (size_t i = 0; i < CH_N_ELEMENTS(cases); i++) {
        char *name = ch_model_family_name(cases[i].in);
        CHECK_STR(name, ==, cases[i].out);
        free(name);
    }
}
static void test_store_mentions(void) {
    char *dir = temp_dir(), *path = ch_build_path(dir, "s.db", NULL);
    ChError *e = NULL;
    ChStore *s = ch_store_open(path, &e);
    CHECK_NO_ERROR(e);
    json_object *m = json_object_new_array();
    json_object *message = ch_message("user", "x");
    json_object *list = json_object_new_array(), *a = json_object_new_object();
    json_object_object_add(a, "path", json_object_new_string("/store/abc123.png"));
    json_object_array_add(list, a);
    json_object_object_add(message, "_chela_attachments", list);
    json_object_array_add(m, message);
    ChSession v = {.id = "11111111-1111-4111-8111-111111111111", .title = "t", .project = "/p", .model = "m"};
    CHECK(ch_store_save(s, &v, m, &e));
    CHECK(ch_store_mentions(s, "abc123.png"));
    CHECK_FALSE(ch_store_mentions(s, "other.png"));
    CHECK(ch_store_delete(s, v.id, &e));
    CHECK_FALSE(ch_store_mentions(s, "abc123.png"));
    json_object_put(m);
    ch_store_close(s);
    free(path);
    remove_tree(dir);
    free(dir);
}
