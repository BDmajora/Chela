/* Conversation transcript, markdown rendering and the welcome screens. */
#include "app.h"
#include "../core/markdown.h"
#include <stdlib.h>
#include <string.h>

#define LIST_INDENT 24.0f
#define COLUMN_PAD 4.0f /* transcript text sits just inside the composer's edge */

typedef struct MdCache {
    uint64_t hash;
    ChMdDocument *doc;
} MdCache;
static MdCache live_cache;
static ChMap expanded_tools;

static void md_free(MdCache *c) {
    ch_clear_pointer(&c->doc, ch_md_free);
    c->hash = 0;
}
void md_cache_clear(App *a) {
    for (size_t i = 0; i < a->md_count; i++)
        md_free(&a->md[i]);
    free(a->md);
    a->md = NULL;
    a->md_count = 0;
}
static uint64_t hash_text(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h ? h : 1;
}
static ChMdDocument *md_for(App *a, size_t index, const char *text) {
    uint64_t hash = hash_text(text);
    MdCache *c;
    if (index == SIZE_MAX)
        c = &live_cache;
    else {
        if (index >= a->md_count) {
            a->md = ch_realloc(a->md, sizeof(MdCache) * (index + 1));
            memset(a->md + a->md_count, 0, sizeof(MdCache) * (index + 1 - a->md_count));
            a->md_count = index + 1;
        }
        c = &a->md[index];
    }
    if (c->hash != hash || !c->doc) {
        md_free(c);
        c->doc = ch_md_parse(text);
        c->hash = hash;
    }
    return c->doc;
}
static float body_size(App *a) {
    return (a->code ? 14 : 16) + (float)a->text_size;
}
static CuFont *text_font(App *a, float size, int weight, bool italic) {
    CuFontDesc d = {.family = a->code ? app_interface_family(a) : app_response_family(a), .size = size, .weight = weight,
                    .italic = italic};
    return cu_font_desc(a->cu, &d);
}
static CuSpan *inline_spans(App *a, const ChMdInline *inl, float size, int weight, CuColor color, int *count) {
    int n = CH_MAX(inl->span_count, 1);
    CuSpan *spans = cu_frame_alloc(a->cu, sizeof(CuSpan) * (size_t)n);
    if (!inl->span_count) {
        spans[0] = (CuSpan){.text = "", .length = 0, .font = text_font(a, size, weight, false), .color = color};
        *count = 1;
        return spans;
    }
    for (int k = 0; k < inl->span_count; k++) {
        const ChMdSpan *s = &inl->spans[k];
        bool mono = s->style & CH_MD_MONO, link = s->style & CH_MD_LINK;
        CuFont *font = mono ? mono_font(a, size * 0.9f)
                            : text_font(a, size, s->style & CH_MD_BOLD ? CH_MAX(weight, 600) : weight, s->style & CH_MD_ITALIC);
        spans[k] = (CuSpan){.text = inl->text + s->start, .length = s->length, .font = font,
                            .color = link ? C_LINK : mono ? 0xffe6c8b8 : color, .bg = mono ? C_INLINE_CODE : 0,
                            .underline = link, .strike = s->style & CH_MD_STRIKE, .link = s->url};
    }
    *count = inl->span_count;
    return spans;
}
static void code_block(App *a, CuId id, const ChMdBlock *b, float space_after) {
    Cu *cu = a->cu;
    bool header = *b->language != 0;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_b = space_after});
    cu_open(cu, cu_idn(cu, "code-box", (int64_t)id),
            &(CuBox){.w = CU_GROW, .bg = C_CODE_BG, .radius = 8, .border = 1, .border_color = C_CODE_BORDER, .clip = true});
    CuId copy = cu_idn(cu, "code-copy", (int64_t)id);
    bool copied = a->copied_id == copy && cu_now(cu) - a->copied_at < 1500000;
    if (copied)
        cu_animate(cu, 200);
    bool clicked = false;
    if (header) {
        cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .pad_l = 14, .pad_r = 6, .pad_t = 5,
                                .border_b = 1, .border_color = C_CODE_BORDER, .pad_b = 5});
        cu_text(cu, 0, b->language, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .w = CU_GROW});
        clicked = ui_icon_button_sized(a, copy, copied ? "check" : "edit-copy", 15, 3, "Copy code", true, false);
        cu_close(cu);
    }
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .pad_l = 14, .pad_r = header ? 14 : 6, .pad_t = header ? 10 : 8,
                            .pad_b = header ? 11 : 8, .gap = 6});
    CuSpan span = {.text = b->body.text, .length = SIZE_MAX, .font = mono_font(a, 13), .color = 0xffe3e3e3};
    cu_open(cu, 0, &(CuBox){.w = CU_GROW});
    cu_rich(cu, id, &span, 1, &(CuPara){.preformatted = true, .selectable = true, .line_height = 1.4f});
    cu_close(cu);
    if (!header)
        clicked = ui_icon_button_sized(a, copy, copied ? "check" : "edit-copy", 15, 3, "Copy code", true, false);
    cu_close(cu);
    cu_close(cu);
    cu_close(cu);
    if (clicked) {
        cu_copy(cu, b->body.text);
        a->copied_id = copy;
        a->copied_at = cu_now(cu);
    }
}
/* The widest word in some text: a column narrower than this has to break words. */
static float longest_word(App *a, CuFont *font, const char *text, size_t length) {
    float widest = 0;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && text[end] != ' ')
            end++;
        widest = CH_MAX(widest, cu_text_width(a->cu, font, text + start, end - start));
        start = end + 1;
    }
    return widest;
}
/* Columns take their natural width. When that does not fit, each gives up what it holds
 * beyond its longest word, so ordinary words stay whole. */
static void table_widths(App *a, const ChMdBlock *b, float available, float size, float *widths) {
    const float padding = 26;
    float total = 0, required = 0;
    float *minimum = cu_frame_alloc(a->cu, sizeof(float) * (size_t)b->columns);
    for (int c = 0; c < b->columns; c++) {
        float natural = 0, word = 0;
        for (int r = 0; r < b->rows; r++) {
            int n = 0;
            CuSpan *spans = inline_spans(a, &b->cells[r * b->columns + c], size, r ? 400 : 600, C_TEXT, &n);
            float line = 0;
            for (int s = 0; s < n; s++) {
                line += cu_text_width(a->cu, spans[s].font, spans[s].text, spans[s].length);
                word = CH_MAX(word, longest_word(a, spans[s].font, spans[s].text, spans[s].length));
            }
            natural = CH_MAX(natural, line);
        }
        /* An unbroken URL or number gets its fair share and wraps, rather than crushing the other columns. */
        minimum[c] = CH_MIN(word + padding, available / (float)b->columns);
        widths[c] = natural + padding;
        total += widths[c];
        required += minimum[c];
    }
    if (total <= available)
        return;
    for (int c = 0; c < b->columns; c++)
        widths[c] = minimum[c] + (widths[c] - minimum[c]) * (available - required) / (total - required);
}
static void table_block(App *a, CuId id, const ChMdBlock *b, float available, float space_after) {
    Cu *cu = a->cu;
    float size = body_size(a) - 1;
    float *widths = cu_frame_alloc(cu, sizeof(float) * (size_t)b->columns);
    table_widths(a, b, available, size, widths);
    cu_open(cu, 0, &(CuBox){.pad_b = space_after, .w = CU_GROW});
    cu_open(cu, 0, &(CuBox){.radius = 8, .border = 1, .border_color = C_CODE_BORDER, .clip = true});
    for (int r = 0; r < b->rows; r++) {
        cu_open(cu, 0, &(CuBox){.row = true, .bg = r ? 0 : C_CODE_BG, .border_b = r + 1 < b->rows ? 1 : 0,
                                .border_color = C_CODE_BORDER});
        for (int c = 0; c < b->columns; c++) {
            cu_open(cu, 0, &(CuBox){.w = CU_PX(widths[c]), CU_PAD2(7, 12)});
            int n = 0;
            CuSpan *spans = inline_spans(a, &b->cells[r * b->columns + c], size, r ? 400 : 600, C_TEXT, &n);
            uint8_t align = b->aligns[c] == CH_MD_RIGHT ? CU_END : b->aligns[c] == CH_MD_CENTER ? CU_CENTER : CU_START;
            cu_rich(cu, cu_idn(cu, "cell", (int64_t)(id * 257 + (CuId)(r * b->columns + c))), spans, n,
                    &(CuPara){.selectable = true, .tight = true, .line_height = 1.3f, .align = align});
            cu_close(cu);
        }
        cu_close(cu);
    }
    cu_close(cu);
    cu_close(cu);
}
static void block_body(App *a, CuId id, const ChMdBlock *b, float available, float space_after) {
    Cu *cu = a->cu;
    float size = body_size(a);
    switch (b->kind) {
    case CH_MD_CODE:
        code_block(a, id, b, space_after);
        return;
    case CH_MD_TABLE:
        table_block(a, id, b, available, space_after);
        return;
    case CH_MD_RULE:
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_t = 6, .pad_b = 14});
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .h = CU_PX(1), .bg = C_CODE_BORDER});
        cu_close(cu);
        cu_close(cu);
        return;
    default:
        break;
    }
    static const float scale[] = {1, 1.5f, 1.3f, 1.15f, 1.05f, 1, 1};
    bool heading = b->kind == CH_MD_HEADING;
    float font = heading ? size * scale[CH_CLAMP(b->level, 1, 6)] : size;
    CuColor color = b->quote_depth ? C_TEXT2 : C_TEXT;
    int n = 0;
    CuSpan *spans = inline_spans(a, &b->body, font, heading ? 600 : 400, color, &n);
    cu_rich(cu, id, spans, n, &(CuPara){.selectable = true, .line_height = heading ? 1.3f : 1.45f, .space_after = space_after});
}
static void list_marker(App *a, const ChMdBlock *b, float size) {
    Cu *cu = a->cu;
    static const char *const bullets[] = {"•", "◦", "▪"};
    cu_open(cu, 0, &(CuBox){.w = CU_PX(LIST_INDENT), .align_x = CU_CENTER, .pad_r = 4});
    if (b->marker == CH_MD_TASK_OPEN || b->marker == CH_MD_TASK_DONE) {
        cu_spacer(cu, CU_PX(1), CU_PX(CH_MAX(size * 0.25f, 3)));
        cu_icon(cu, b->marker == CH_MD_TASK_DONE ? "task-done" : "task-open", 16, b->marker == CH_MD_TASK_DONE ? C_OK : C_MUTED);
    } else {
        const char *text = b->marker == CH_MD_NUMBER ? b->number : bullets[(b->list_depth - 1) % 3];
        CuSpan span = {.text = text, .length = SIZE_MAX, .font = text_font(a, size, 400, false), .color = C_TEXT2};
        cu_rich(cu, 0, &span, 1, &(CuPara){.line_height = 1.45f, .align = b->marker == CH_MD_NUMBER ? CU_END : CU_CENTER});
    }
    cu_close(cu);
}
/* Space under a block depends on what follows: items in a tight list sit close. */
static float gap_after(App *a, const ChMdDocument *doc, size_t i) {
    float size = body_size(a);
    if (i + 1 >= doc->count)
        return 0;
    const ChMdBlock *next = &doc->blocks[i + 1], *b = &doc->blocks[i];
    if (next->kind == CH_MD_HEADING)
        return size * 1.15f;
    if (b->kind == CH_MD_HEADING)
        return size * 0.4f;
    if (next->tight && next->list_depth)
        return 3;
    return size * 0.75f;
}
static void blocks(App *a, CuId base, const ChMdDocument *doc, size_t *index, int quote, float available);
static void one_block(App *a, CuId base, const ChMdDocument *doc, size_t i, float available) {
    Cu *cu = a->cu;
    const ChMdBlock *b = &doc->blocks[i];
    CuId id = cu_idn(cu, "md", (int64_t)(base * 4099 + (CuId)i));
    float space = gap_after(a, doc, i), indent = LIST_INDENT * (float)CH_MAX(b->list_depth - (b->marker ? 1 : 0), 0);
    /* List blocks are indented for their depth; a first block also carries the marker. */
    if (!b->list_depth) {
        block_body(a, id, b, available, space);
        return;
    }
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .pad_l = indent});
    if (b->marker)
        list_marker(a, b, body_size(a));
    cu_open(cu, 0, &(CuBox){.w = CU_GROW});
    block_body(a, id, b, available - indent - (b->marker ? LIST_INDENT : 0), space);
    cu_close(cu);
    cu_close(cu);
}
static void blocks(App *a, CuId base, const ChMdDocument *doc, size_t *index, int quote, float available) {
    Cu *cu = a->cu;
    while (*index < doc->count) {
        const ChMdBlock *b = &doc->blocks[*index];
        if (b->quote_depth < quote)
            return;
        if (b->quote_depth == quote) {
            one_block(a, base, doc, *index, available);
            (*index)++;
            continue;
        }
        /* One bar runs down every block at this quote depth. */
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_b = 10});
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 14, .border_l = 3, .border_color = C_QUOTE, .pad_t = 2, .pad_b = 2});
        blocks(a, base, doc, index, quote + 1, available - 17);
        cu_close(cu);
        cu_close(cu);
    }
}
static void markdown(App *a, CuId base, const char *text, size_t index) {
    ChMdDocument *doc = md_for(a, index, text);
    size_t i = 0;
    blocks(a, base, doc, &i, 0, content_width(a) - COLUMN_PAD * 2);
}
static void message_actions(App *a, size_t index, const char *text, json_object *stats, bool show) {
    Cu *cu = a->cu;
    /* Hidden by keeping the row's height, so the transcript does not jump under the pointer. */
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_PX(28), .gap = 2, .align_y = CU_CENTER, .pad_t = 4});
    if (show) {
        CuId copy = cu_idn(cu, "copy", (int64_t)index);
        bool copied = a->copied_id == copy && cu_now(cu) - a->copied_at < 1500000;
        if (copied)
            cu_animate(cu, 200);
        if (ui_icon_button_sized(a, copy, copied ? "check" : "edit-copy", 16, 4, "Copy message", true, false)) {
            cu_copy(cu, text);
            a->copied_id = copy;
            a->copied_at = cu_now(cu);
        }
        const char *icons[] = {"thumb-up", "thumb-down"};
        const char *tips[] = {"Good response", "Bad response"};
        for (int i = 0; i < 2; i++) {
            bool on = index < CH_N_ELEMENTS(a->feedback) && (a->feedback[index] & (1 << i));
            if (ui_icon_button_sized(a, cu_idn(cu, i ? "down" : "up", (int64_t)index), icons[i], 16, 4, tips[i], true, on) &&
                index < CH_N_ELEMENTS(a->feedback))
                a->feedback[index] = (uint8_t)(on ? 0 : 1 << i);
        }
        size_t count = json_object_array_length(a->messages);
        bool can = !a->busy && index > 0 && index + 1 == count && a->config.provider != 5 &&
                   !strcmp(ch_json_str(json_object_array_get_idx(a->messages, index - 1), "role"), "user");
        if (can && ui_icon_button_sized(a, cu_idn(cu, "regenerate", (int64_t)index), "regenerate", 16, 4, "Retry this response", true, false))
            app_regenerate(a, index);
        if (stats) {
            char *summary = ch_stats_summary(stats);
            cu_open(cu, 0, &(CuBox){.pad_l = 8, .w = CU_GROW});
            cu_text(cu, 0, summary, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = C_FAINT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
            cu_close(cu);
            free(summary);
        }
    }
    cu_close(cu);
}
static void user_message(App *a, json_object *message, size_t index) {
    Cu *cu = a->cu;
    const char *text = ch_json_str(message, "content");
    json_object *attachments = ch_json_get(message, "_chela_attachments");
    CuId row = cu_idn(cu, "user-row", (int64_t)index), copy = cu_idn(cu, "user-copy", (int64_t)index);
    cu_open(cu, row, &(CuBox){.w = CU_GROW, .pad_b = 20, .align_x = CU_END, .gap = 8});
    if (attachments && json_object_array_length(attachments)) {
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_x = CU_END, .w = CU_GROW});
        for (size_t i = 0; i < json_object_array_length(attachments); i++)
            attachment_sent(a, cu_idn(cu, "sent", (int64_t)(index * 100 + i)), json_object_array_get_idx(attachments, i));
        cu_close(cu);
    }
    if (*text) {
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 6, .align_y = CU_START, .align_x = CU_END, .w = CU_GROW});
        if (cu_hovered_within(cu, row)) {
            if (ui_icon_button_sized(a, copy, "edit-copy", 16, 4, "Copy message", true, false))
                cu_copy(cu, text);
        }
        cu_open(cu, 0, &(CuBox){.bg = C_COMPOSER, CU_PAD2(10, 14), .radius = 12, .w = CU_FIT_MAX(content_width(a) * 0.82f)});
        cu_text(cu, cu_idn(cu, "user-text", (int64_t)index), text,
                &(CuTextStyle){.font = ui_font(a, 14 + (float)a->text_size, 400), .color = C_TEXT, .wrap = CU_WRAP_WORD,
                               .selectable = true, .w = CU_FIT, .line_height = 1.35f});
        cu_close(cu);
        cu_close(cu);
    }
    cu_close(cu);
}
static void assistant_message(App *a, const char *text, size_t index, bool live, json_object *stats) {
    Cu *cu = a->cu;
    CuId id = live ? cu_id(cu, "assistant-live") : cu_idn(cu, "assistant", (int64_t)index);
    bool last = !live && index + 1 == json_object_array_length(a->messages);
    cu_open(cu, id, &(CuBox){.w = CU_GROW, .pad_l = COLUMN_PAD, .pad_r = COLUMN_PAD, .pad_b = live ? 8 : 14});
    markdown(a, (CuId)(live ? 999999 : index), text, live ? SIZE_MAX : index);
    if (!live)
        message_actions(a, index, text, stats, last || cu_hovered_within(cu, id));
    cu_close(cu);
}
static json_object *tool_result(App *a, const char *id, size_t after) {
    for (size_t i = after; i < json_object_array_length(a->messages); i++) {
        json_object *m = json_object_array_get_idx(a->messages, i);
        if (!strcmp(ch_json_str(m, "role"), "tool") && !strcmp(ch_json_str(m, "tool_call_id"), id))
            return m;
    }
    return NULL;
}
/* One tool call inside an expanded group. */
static void tool_activity(App *a, json_object *call, json_object *result) {
    Cu *cu = a->cu;
    json_object *f = ch_json_get(call, "function");
    const char *name = ch_json_str(f, "name"), *arguments = ch_json_str(f, "arguments"), *id = ch_json_str(call, "id");
    json_object *args = ch_json_parse(arguments, NULL);
    const char *action = !strcmp(name, "read_file")    ? "Read"
                       : !strcmp(name, "list_files")   ? "List directory"
                       : !strcmp(name, "write_file")   ? "Write"
                       : !strcmp(name, "run_command")  ? "Run"
                                                       : name;
    const char *target = ch_json_str(args, !strcmp(name, "run_command") ? "command" : "path");
    const char *output = ch_json_str(result, "content");
    bool failed = result && ch_str_has_prefix(output, "Tool error:");
    char *summary = ch_strdup_printf("%s  %s", action, target), *line = ch_title(summary);
    if (!expanded_tools.keys)
        ch_map_init(&expanded_tools, NULL);
    bool expanded = ch_map_has(&expanded_tools, id), was = expanded;
    CuId row = cu_ids(cu, "tool", id);
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 20, .pad_b = 2, .gap = 4});
    cu_open(cu, row, &(CuBox){.row = true, .gap = 7, .align_y = CU_CENTER, CU_PAD2(3, 6), .radius = 6, .interactive = true,
                              .focusable = true, .w = CU_GROW, .bg = cu_hovered(cu, row) ? C_HOVER : 0});
    cu_icon(cu, !result ? "session-idle" : failed ? "warning" : "check", 14, failed ? C_DANGER : !result ? C_MUTED : C_FAINT);
    cu_text(cu, 0, line, &(CuTextStyle){.font = mono_font(a, 12), .color = C_TEXT2, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    cu_icon(cu, expanded ? "chevron-down" : "chevron-right", 12, C_FAINT);
    cu_close(cu);
    if (cu_clicked(cu, row))
        expanded = !expanded;
    if (was != expanded) {
        if (expanded)
            ch_map_set(&expanded_tools, id, NULL);
        else
            ch_map_remove(&expanded_tools, id);
    }
    if (expanded) {
        char *detail = ch_strdup_printf("%s\n%s\n\n%s", name, arguments, result ? output : "Awaiting tool result");
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .bg = C_CODE_BG, .radius = 8, .border = 1, .border_color = C_CODE_BORDER, CU_PAD2(8, 12), .clip = true});
        ui_mono_text(a, cu_ids(cu, "tool-detail", id), detail, 12.5f, true);
        cu_close(cu);
        free(detail);
    }
    cu_close(cu);
    if (args)
        json_object_put(args);
    free(summary);
    free(line);
}
/* Consecutive tool calls read as one line, "Ran 3 commands, read 2 files", that opens to show them. */
static size_t tool_group(App *a, size_t first) {
    Cu *cu = a->cu;
    size_t count = json_object_array_length(a->messages), end = first;
    int commands = 0, reads = 0, lists = 0, edits = 0, others = 0, failed = 0, pending = 0;
    const char *key = NULL;
    for (size_t i = first; i < count; i++) {
        json_object *m = json_object_array_get_idx(a->messages, i);
        const char *role = ch_json_str(m, "role");
        json_object *calls = ch_json_get(m, "tool_calls");
        bool has_calls = calls && json_object_array_length(calls);
        if (!strcmp(role, "tool") || (!strcmp(role, "assistant") && has_calls && (i == first || !*ch_json_str(m, "content")))) {
            end = i + 1;
            for (size_t j = 0; has_calls && j < json_object_array_length(calls); j++) {
                json_object *call = json_object_array_get_idx(calls, j), *result = tool_result(a, ch_json_str(call, "id"), i + 1);
                const char *name = ch_json_str(ch_json_get(call, "function"), "name");
                if (!key)
                    key = ch_json_str(call, "id");
                commands += !strcmp(name, "run_command");
                reads += !strcmp(name, "read_file");
                lists += !strcmp(name, "list_files");
                edits += !strcmp(name, "write_file");
                others += strcmp(name, "run_command") && strcmp(name, "read_file") && strcmp(name, "list_files") && strcmp(name, "write_file");
                pending += !result;
                failed += result && ch_str_has_prefix(ch_json_str(result, "content"), "Tool error:");
            }
        } else
            break;
    }
    ChBuf title;
    ch_buf_init(&title);
#define PHRASE(n, first_word, later_word, one, many)                                                         \
    if (n)                                                                                                   \
    ch_buf_printf(&title, "%s%s %d %s", title.len ? ", " : "", title.len ? later_word : first_word, n, n == 1 ? one : many)
    PHRASE(commands, "Ran", "ran", "command", "commands");
    PHRASE(reads, "Read", "read", "file", "files");
    PHRASE(lists, "Listed", "listed", "folder", "folders");
    PHRASE(edits, "Edited", "edited", "file", "files");
    PHRASE(others, "Used", "used", "tool", "tools");
#undef PHRASE
    if (failed)
        ch_buf_printf(&title, " (%d failed)", failed);
    if (!expanded_tools.keys)
        ch_map_init(&expanded_tools, NULL);
    char *group = ch_strdup_printf("#group-%s", key ? key : "");
    bool expanded = ch_map_has(&expanded_tools, group);
    CuId id = cu_ids(cu, "tool-group", group);
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_b = 12, .gap = 2});
    cu_open(cu, id, &(CuBox){.row = true, .gap = 5, .align_y = CU_CENTER, CU_PAD2(3, 4), .radius = 6, .interactive = true,
                             .focusable = true, .bg = cu_hovered(cu, id) ? C_HOVER : 0});
    if (pending)
        ui_spinner(a, 12);
    cu_text(cu, 0, title.str, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = 0xffa5a5a5});
    cu_icon(cu, expanded ? "chevron-down" : "chevron-right", 12, 0xffa5a5a5);
    cu_close(cu);
    if (cu_clicked(cu, id)) {
        if (expanded)
            ch_map_remove(&expanded_tools, group);
        else
            ch_map_set(&expanded_tools, group, NULL);
    }
    if (expanded)
        for (size_t i = first; i < end; i++) {
            json_object *m = json_object_array_get_idx(a->messages, i), *calls = ch_json_get(m, "tool_calls");
            for (size_t j = 0; calls && j < json_object_array_length(calls); j++) {
                json_object *call = json_object_array_get_idx(calls, j);
                tool_activity(a, call, tool_result(a, ch_json_str(call, "id"), i + 1));
            }
        }
    cu_close(cu);
    free(group);
    ch_buf_clear(&title);
    return end;
}
static void tool_message(App *a, const char *text, const char *name, size_t index) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = COLUMN_PAD, .pad_r = COLUMN_PAD, .pad_b = 14, .gap = 6});
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER});
    cu_text(cu, 0, *name ? name : "Tool", &(CuTextStyle){.font = ui_font(a, 12, 600), .color = 0xffb5b5b5, .w = CU_GROW});
    if (ui_icon_button_sized(a, cu_idn(cu, "tool-copy", (int64_t)index), "edit-copy", 16, 3, "Copy message", true, false))
        cu_copy(cu, text);
    cu_close(cu);
    char key[48];
    snprintf(key, sizeof(key), "#tool-output-%zu", index);
    if (!expanded_tools.keys)
        ch_map_init(&expanded_tools, NULL);
    bool expanded = ch_map_has(&expanded_tools, key), was = expanded;
    ui_expander(a, cu_idn(cu, "tool-output", (int64_t)index), "Output", &expanded);
    if (was != expanded) {
        if (expanded)
            ch_map_set(&expanded_tools, key, NULL);
        else
            ch_map_remove(&expanded_tools, key);
    }
    if (expanded)
        ui_mono_text(a, cu_idn(cu, "tool-text", (int64_t)index), text, 12.5f, true);
    cu_close(cu);
}
static void stopped_note(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, cu_id(cu, "stopped"), &(CuBox){.w = CU_GROW, .bg = 0xff2a2120, .border = 1, .border_color = 0xff5a3a32, .radius = 10,
                                               CU_PAD2(10, 14), .gap = 6});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER, .w = CU_GROW});
    cu_icon(cu, "warning", 15, C_DANGER);
    cu_text(cu, 0, "Request stopped", &(CuTextStyle){.font = ui_font(a, 13, 600), .color = 0xffefa68e, .w = CU_GROW});
    if (ui_flat_button(a, cu_id(cu, "stopped-diagnostics"), NULL, "Diagnostics", NULL, 12, C_TEXT2, NULL, true, 0))
        open_diagnostics(a);
    cu_close(cu);
    ui_text_block(a, cu_id(cu, "stopped-note"), a->stopped_note, 13, 0xffe0c4bb, true);
    if (a->recovery_visible) {
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 4, .pad_t = 2});
        if (ui_flat_button(a, cu_id(cu, "recovery-retry"), "regenerate", "Retry message", NULL, 13, C_TEXT, NULL, true, 0))
            app_retry(a);
        if (ui_flat_button(a, cu_id(cu, "recovery-settings"), NULL, "Connection settings", NULL, 13, C_TEXT2, NULL, true, 0))
            open_settings(a, SETTINGS_CONNECTIONS);
        cu_close(cu);
    }
    cu_close(cu);
    cu_spacer(cu, CU_PX(1), CU_PX(14));
}
float content_width(App *a) {
    float available = cu_width(a->cu) - (a->sidebar_hidden ? 0 : SIDEBAR_W) - (app_panel_visible(a) ? PANEL_W : 0);
    float limit = a->transcript_width == 2 ? available - 64 : a->transcript_width == 1 ? 896 : a->code ? 768 : 700;
    return CH_MIN(limit, CH_MAX(300, available - 64));
}
void chat_build(App *a) {
    Cu *cu = a->cu;
    CuId scroll = cu_id(cu, "chat-scroll");
    if (!cu_modal_active(cu) && !a->menu.kind) {
        float page = cu_rect(cu, scroll).h * 0.85f;
        if (cu_key(cu, CU_KEY_PAGE_UP, 0))
            cu_scroll_by(cu, scroll, -page);
        else if (cu_key(cu, CU_KEY_PAGE_DOWN, 0))
            cu_scroll_by(cu, scroll, page);
    }
    /* New text keeps the end in view only until the reader moves away from it, or starts
     * selecting. The target decides, because this frame's scroll is not laid out yet. */
    if (cu_scrolled_by_user(cu, scroll))
        a->follow = cu_scroll_target(cu, scroll) >= cu_scroll_max(cu, scroll) - 1;
    if (cu_selecting(cu))
        a->follow = false;
    if (a->follow)
        cu_scroll_to(cu, scroll, INFINITY);
    cu_open(cu, scroll, &(CuBox){.w = CU_GROW, .h = CU_GROW, .scroll_y = true, .align_x = CU_CENTER, .pad_t = 28});
    cu_open(cu, cu_id(cu, "chat-column"), &(CuBox){.w = CU_PX(content_width(a))});
    size_t count = json_object_array_length(a->messages);
    for (size_t i = 0; i < count;) {
        json_object *m = json_object_array_get_idx(a->messages, i);
        const char *role = ch_json_str(m, "role"), *content = ch_json_str(m, "content");
        json_object *calls = ch_json_get(m, "tool_calls");
        bool has_calls = calls && json_object_array_length(calls);
        if (!strcmp(role, "system")) {
            i++;
            continue;
        }
        if (!strcmp(role, "user"))
            user_message(a, m, i);
        else if (!strcmp(role, "tool") && *content)
            tool_message(a, content, ch_json_str(m, "name"), i);
        else if (!strcmp(role, "assistant") && *content)
            assistant_message(a, content, i, false, ch_json_get(m, "_chela_stats"));
        if (has_calls)
            i = tool_group(a, i);
        else
            i++;
    }
    if (a->live_active && a->live.len)
        assistant_message(a, a->live.str, SIZE_MAX, true, NULL);
    if (a->stopped_note && !a->busy)
        stopped_note(a);
    cu_close(cu);
    cu_close(cu);
    char *link = cu_link_take(cu);
    if (link)
        app_open_url(a, link);
    free(link);
}

/* Welcome */
static void project_row(App *a, const char *path, bool selected) {
    Cu *cu = a->cu;
    CuId id = cu_ids(cu, "welcome-project", path);
    char *name = ch_path_basename(path);
    cu_open(cu, id, &(CuBox){.row = true, .gap = 10, .w = CU_GROW, CU_PAD2(7, 10), .radius = 7, .align_y = CU_CENTER,
                             .bg = cu_hovered(cu, id) ? C_HOVER : 0, .interactive = true, .tooltip = path});
    cu_icon(cu, "folder", 16, 0xffd4d4d4);
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    if (selected)
        cu_icon(cu, "check", 15, C_TEXT2);
    cu_close(cu);
    if (cu_clicked(cu, id) && !a->busy && !selected) {
        if (!ch_file_is_dir(path))
            app_notice(a, "Project folder is missing", path);
        else
            app_set_project(a, path);
    }
    free(name);
}
void welcome_build(App *a) {
    Cu *cu = a->cu;
    if (a->code) {
        cu_open(cu, cu_id(cu, "welcome"), &(CuBox){.w = CU_GROW, .h = CU_GROW, .align_x = CU_CENTER, .align_y = CU_CENTER});
        cu_open(cu, 0, &(CuBox){.w = CU_PX(CH_MIN(280, content_width(a))), .gap = 8, .align_x = CU_CENTER});
        ui_mark(a, 36);
        cu_open(cu, cu_id(cu, "welcome-projects"),
                &(CuBox){.w = CU_GROW, CU_PAD(5), .gap = 1, .radius = 11, .border = 1, .border_color = C_BORDER, .bg = 0xff1f1f1f});
        ChVec *projects = recent_projects(a, 6);
        for (size_t i = 0; i < projects->len; i++)
            project_row(a, projects->data[i], !strcmp(projects->data[i], a->project));
        if (projects->len) {
            cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(4, 0)});
            cu_open(cu, 0, &(CuBox){.w = CU_GROW, .h = CU_PX(1), .bg = C_BORDER});
            cu_close(cu);
            cu_close(cu);
        }
        ch_vec_free(projects);
        CuId open = cu_id(cu, "welcome-open");
        cu_open(cu, open, &(CuBox){.row = true, .gap = 10, .w = CU_GROW, CU_PAD2(7, 10), .radius = 7, .align_y = CU_CENTER,
                                   .bg = cu_hovered(cu, open) ? C_HOVER : 0, .interactive = true});
        cu_icon(cu, "folder-open", 16, 0xffd4d4d4);
        cu_text(cu, 0, "Open folder…", &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT2});
        cu_close(cu);
        if (cu_clicked(cu, open))
            app_choose_project(a);
        cu_close(cu);
        cu_close(cu);
        cu_close(cu);
        return;
    }
    cu_open(cu, cu_id(cu, "welcome"), &(CuBox){.w = CU_GROW, .h = CU_GROW, .align_x = CU_CENTER, .align_y = CU_END, .pad_b = 30});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 12, .align_y = CU_CENTER});
    ui_mark(a, 32);
    cu_text(cu, 0, "How can I help?", &(CuTextStyle){.font = serif_font(a, 40, 400), .color = C_TEXT});
    cu_close(cu);
    cu_close(cu);
}
