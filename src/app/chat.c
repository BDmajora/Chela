/* Conversation transcript, markdown rendering and the welcome screens. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

enum { MD_PARA, MD_HEADING, MD_CODE, MD_LIST, MD_QUOTE, MD_BLANK };
typedef struct {
    size_t start, length;
    int style; /* 0 normal, 1 bold, 2 code */
} MdSpan;
typedef struct {
    int kind;
    char *text;
    MdSpan *spans;
    int span_count;
} MdBlock;
typedef struct MdCache {
    uint64_t hash;
    MdBlock *blocks;
    int count;
} MdCache;
static MdCache live_cache;
static ChMap expanded_tools;

static void md_free(MdCache *c) {
    for (int i = 0; i < c->count; i++) {
        free(c->blocks[i].text);
        free(c->blocks[i].spans);
    }
    free(c->blocks);
    c->blocks = NULL;
    c->count = 0;
    c->hash = 0;
}
void md_cache_clear(App *a) {
    for (size_t i = 0; i < a->md_count; i++)
        md_free(&a->md[i]);
    free(a->md);
    a->md = NULL;
    a->md_count = 0;
}
static void inline_spans(MdBlock *b) {
    /* **bold** and `code` within one line. */
    const char *s = b->text, *p = s, *segment = s;
    ChVec spans;
    ch_vec_init(&spans, free);
#define PUSH(st, a0, len)                                                                                    \
    do {                                                                                                     \
        if ((len) > 0) {                                                                                     \
            MdSpan *m = ch_new0(MdSpan, 1);                                                                  \
            *m = (MdSpan){(size_t)((a0) - s), (size_t)(len), (st)};                                         \
            ch_vec_push(&spans, m);                                                                          \
        }                                                                                                    \
    } while (0)
    while (*p) {
        int marker = p[0] == '*' && p[1] == '*' ? 2 : p[0] == '`' ? 1 : 0;
        const char *close = marker ? strstr(p + marker, marker == 2 ? "**" : "`") : NULL;
        if (close && close > p + marker) {
            PUSH(0, segment, p - segment);
            PUSH(marker == 2 ? 1 : 2, p + marker, close - p - marker);
            p = close + marker;
            segment = p;
        } else
            p++;
    }
    PUSH(0, segment, p - segment);
#undef PUSH
    b->span_count = (int)spans.len;
    b->spans = ch_new0(MdSpan, spans.len + 1);
    for (size_t i = 0; i < spans.len; i++)
        b->spans[i] = *(MdSpan *)spans.data[i];
    ch_vec_clear(&spans);
}
static void md_parse(MdCache *c, const char *text) {
    md_free(c);
    char **lines = ch_strsplit(text, "\n");
    ChVec blocks;
    ch_vec_init(&blocks, free);
    bool code = false;
    ChBuf code_text;
    ch_buf_init(&code_text);
    for (size_t i = 0; lines[i]; i++) {
        const char *s = lines[i];
        if (ch_str_has_prefix(s, "```")) {
            if (code) {
                MdBlock *b = ch_new0(MdBlock, 1);
                b->kind = MD_CODE;
                if (code_text.len && code_text.str[code_text.len - 1] == '\n')
                    ch_buf_truncate(&code_text, code_text.len - 1);
                b->text = ch_strdup(code_text.str);
                ch_vec_push(&blocks, b);
                ch_buf_truncate(&code_text, 0);
            } else if (s[3]) {
                /* The fence's language name is shown as the first code line. */
                ch_buf_append(&code_text, s + 3);
                ch_buf_append_c(&code_text, '\n');
            }
            code = !code;
            continue;
        }
        if (code) {
            ch_buf_append(&code_text, s);
            ch_buf_append_c(&code_text, '\n');
            continue;
        }
        MdBlock *b = ch_new0(MdBlock, 1);
        if (!*s) {
            b->kind = MD_BLANK;
            b->text = ch_strdup("");
        } else if (*s == '#') {
            while (*s == '#')
                s++;
            if (*s == ' ')
                s++;
            b->kind = MD_HEADING;
            b->text = ch_strdup(s);
        } else if (ch_str_has_prefix(s, "- ") || ch_str_has_prefix(s, "* ")) {
            b->kind = MD_LIST;
            b->text = ch_strdup(s + 2);
        } else if (ch_str_has_prefix(s, "> ")) {
            b->kind = MD_QUOTE;
            b->text = ch_strdup(s + 2);
        } else {
            b->kind = MD_PARA;
            b->text = ch_strdup(s);
        }
        inline_spans(b);
        ch_vec_push(&blocks, b);
    }
    if (code && code_text.len) {
        MdBlock *b = ch_new0(MdBlock, 1);
        b->kind = MD_CODE;
        ch_buf_truncate(&code_text, code_text.len - 1);
        b->text = ch_strdup(code_text.str);
        ch_vec_push(&blocks, b);
    }
    ch_buf_clear(&code_text);
    ch_strfreev(lines);
    c->count = (int)blocks.len;
    c->blocks = ch_new0(MdBlock, blocks.len + 1);
    for (size_t i = 0; i < blocks.len; i++)
        c->blocks[i] = *(MdBlock *)blocks.data[i];
    ch_vec_clear(&blocks);
}
static uint64_t hash_text(const char *s) {
    uint64_t h = 14695981039346656037ull;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 1099511628211ull;
    return h ? h : 1;
}
static MdCache *md_for(App *a, size_t index, const char *text) {
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
    if (c->hash != hash || !c->blocks) {
        md_parse(c, text);
        c->hash = hash;
    }
    return c;
}
static float body_size(App *a) {
    return a->code ? 14 + (float)a->text_size : 16 + (float)a->text_size * 2;
}
static CuFont *body_font(App *a, int weight) {
    return a->code ? ui_font(a, body_size(a), weight) : serif_font(a, body_size(a), weight);
}
static void markdown(App *a, CuId base, const char *text, size_t index) {
    Cu *cu = a->cu;
    MdCache *c = md_for(a, index, text);
    float size = body_size(a);
    for (int i = 0; i < c->count; i++) {
        MdBlock *b = &c->blocks[i];
        CuId id = cu_idn(cu, "md", (int64_t)(base * 31 + (CuId)i));
        if (b->kind == MD_BLANK) {
            cu_spacer(cu, CU_PX(1), CU_PX(size * 0.6f));
            continue;
        }
        if (b->kind == MD_CODE) {
            cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 12, .pad_r = 12, .pad_t = 6, .pad_b = 6});
            cu_open(cu, 0, &(CuBox){.w = CU_GROW, .bg = C_CODE_BG, CU_PAD2(8, 12), .radius = 6, .clip = true});
            CuSpan span = {.text = b->text, .length = SIZE_MAX, .font = mono_font(a, 14), .color = C_TEXT};
            cu_rich(cu, id, &span, 1, &(CuPara){.preformatted = true, .selectable = true, .line_height = 1.35f});
            cu_close(cu);
            cu_close(cu);
            continue;
        }
        CuSpan spans[64];
        int n = 0;
        if (b->kind == MD_LIST)
            spans[n++] = (CuSpan){.text = "•  ", .length = SIZE_MAX, .font = body_font(a, 400), .color = C_TEXT};
        for (int k = 0; k < b->span_count && n < 64; k++) {
            MdSpan *s = &b->spans[k];
            bool heading = b->kind == MD_HEADING;
            CuFont *font = s->style == 2 ? mono_font(a, size * 0.92f)
                         : heading     ? (a->code ? ui_font(a, size * 1.15f, 700) : serif_font(a, size * 1.15f, 700))
                                       : body_font(a, s->style == 1 ? 700 : 400);
            spans[n++] = (CuSpan){.text = b->text + s->start, .length = s->length, .font = font,
                                  .color = b->kind == MD_QUOTE ? C_MUTED : s->style == 2 ? 0xffdedede : C_TEXT,
                                  .bg = s->style == 2 ? C_INLINE_CODE : 0};
        }
        if (!n)
            continue;
        CuPara para = {.selectable = true, .line_height = 1.28f, .space_after = 5};
        if (b->kind == MD_LIST) {
            para.indent = 4;
            para.hanging = cu_text_width(cu, body_font(a, 400), "•  ", strlen("•  "));
        } else if (b->kind == MD_QUOTE)
            para.indent = 16;
        else if (b->kind == MD_HEADING)
            para.space_after = 7;
        if (b->kind == MD_HEADING)
            cu_spacer(cu, CU_PX(1), CU_PX(8));
        cu_rich(cu, id, spans, n, &para);
    }
}
static void message_actions(App *a, size_t index, const char *text, bool assistant) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 4, .align_y = CU_CENTER, .pad_t = 4});
    if (ui_icon_button_sized(a, cu_idn(cu, "copy", (int64_t)index), "edit-copy", 18, 3, "Copy message", true, false))
        cu_copy(cu, text);
    if (assistant) {
        ui_icon_button_sized(a, cu_idn(cu, "speak", (int64_t)index), "speaker", 18, 3,
                             "Read aloud is not available with this connection", false, false);
        const char *icons[] = {"thumb-up", "thumb-down"};
        const char *tips[] = {"Mark as helpful in this view", "Mark as unhelpful in this view"};
        for (int i = 0; i < 2; i++) {
            bool on = index < CH_N_ELEMENTS(a->feedback) && (a->feedback[index] & (1 << i));
            if (ui_icon_button_sized(a, cu_idn(cu, i ? "down" : "up", (int64_t)index), icons[i], 18, 3, tips[i], true, on) &&
                index < CH_N_ELEMENTS(a->feedback))
                a->feedback[index] = (bool)(on ? 0 : 1 << i);
        }
        size_t count = json_object_array_length(a->messages);
        bool can = !a->busy && index > 0 && index + 1 == count && a->config.provider != 5 &&
                   !strcmp(ch_json_str(json_object_array_get_idx(a->messages, index - 1), "role"), "user");
        if (ui_icon_button_sized(a, cu_idn(cu, "regenerate", (int64_t)index), "regenerate", 18, 3, "Retry this response", can, false))
            app_regenerate(a, index);
    }
    cu_close(cu);
}
static void user_message(App *a, const char *text, size_t index) {
    Cu *cu = a->cu;
    CuId bubble = cu_idn(cu, "user-bubble", (int64_t)index);
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_b = 24});
    cu_open(cu, bubble, &(CuBox){.row = true, .gap = 10, .bg = C_COMPOSER, CU_PAD2(10, 14), .radius = 10,
                                 .w = CU_FIT_MAX(620), .align_y = CU_START});
    cu_text(cu, cu_idn(cu, "user-text", (int64_t)index), text,
            &(CuTextStyle){.font = ui_font(a, 15, 400), .color = C_TEXT, .wrap = CU_WRAP_WORD, .selectable = true,
                           .w = CU_FIT_MAX(560), .line_height = 1.2f});
    CuId copy = cu_idn(cu, "user-copy", (int64_t)index);
    if (cu_hovered_within(cu, bubble)) {
        if (ui_icon_button_sized(a, copy, "edit-copy", 16, 1, "Copy message", true, false))
            cu_copy(cu, text);
    } else
        cu_spacer(cu, CU_PX(18), CU_PX(18));
    cu_close(cu);
    cu_close(cu);
}
static void metrics_line(App *a, json_object *stats) {
    char *summary = ch_stats_summary(stats);
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 16, .pad_b = 16});
    ui_label_wrap(a, summary, 11, 0xffaaaaaa, 0);
    cu_close(a->cu);
    free(summary);
}
static void assistant_message(App *a, const char *text, size_t index, bool live) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(8, 16), .pad_b = live ? 8 : 24, .gap = 9});
    markdown(a, (CuId)(live ? 999999 : index), text, live ? SIZE_MAX : index);
    if (!live)
        message_actions(a, index, text, true);
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
    char *summary = ch_strdup_printf("%s%s  %s", !result ? "…  " : ch_str_has_prefix(output, "Tool error:") ? "⚠  " : "✓  ",
                                     action, target);
    char *line = ch_title(summary);
    if (!expanded_tools.keys)
        ch_map_init(&expanded_tools, NULL);
    bool expanded = ch_map_has(&expanded_tools, id);
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 16, .pad_r = 16, .pad_t = 3, .pad_b = 12, .gap = 6});
    bool was = expanded;
    ui_expander(a, cu_ids(cu, "tool", id), line, &expanded);
    if (was != expanded) {
        if (expanded)
            ch_map_set(&expanded_tools, id, NULL);
        else
            ch_map_remove(&expanded_tools, id);
    }
    if (expanded) {
        char *detail = ch_strdup_printf("%s\n%s\n\n%s", name, arguments, result ? output : "Awaiting tool result");
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .bg = 0xff1e1e1e, .radius = 6, CU_PAD2(8, 10), .clip = true});
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
static void tool_message(App *a, const char *text, const char *name, size_t index) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(8, 16), .pad_b = 24, .gap = 6});
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER});
    cu_text(cu, 0, *name ? name : "Tool", &(CuTextStyle){.font = ui_font(a, 12, 700), .color = 0xffb5b5b5, .w = CU_GROW});
    if (ui_icon_button_sized(a, cu_idn(cu, "tool-copy", (int64_t)index), "edit-copy", 16, 2, "Copy message", true, false))
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
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(8, 16), .pad_b = 24, .gap = 6});
    cu_text(cu, 0, "Request stopped", &(CuTextStyle){.font = ui_font(a, 12, 700), .color = 0xffb5b5b5});
    ui_text_block(a, cu_id(cu, "stopped-note"), a->stopped_note, 14, 0xffefa68e, true);
    cu_close(cu);
}
float content_width(App *a) {
    float available = cu_width(a->cu) - (a->sidebar_hidden ? 0 : SIDEBAR_W);
    float limit = a->transcript_width == 2 ? available - 64 : a->transcript_width == 1 ? 896 : a->code ? 768 : 656;
    return CH_MIN(limit, CH_MAX(320, available - 64));
}
void chat_build(App *a) {
    Cu *cu = a->cu;
    CuId scroll = cu_id(cu, "chat-scroll");
    if (cu_scrolled_by_user(cu, scroll))
        a->follow = cu_scroll_y(cu, scroll) >= cu_scroll_max(cu, scroll) - 80;
    if (a->follow)
        cu_scroll_to(cu, scroll, INFINITY);
    cu_open(cu, scroll, &(CuBox){.w = CU_GROW, .h = CU_GROW, .scroll_y = true, .align_x = CU_CENTER, .pad_t = 28});
    cu_open(cu, cu_id(cu, "chat-column"), &(CuBox){.w = CU_PX(content_width(a))});
    ChMap shown;
    ch_map_init(&shown, NULL);
    size_t count = json_object_array_length(a->messages);
    for (size_t i = 0; i < count; i++) {
        json_object *m = json_object_array_get_idx(a->messages, i);
        const char *role = ch_json_str(m, "role"), *content = ch_json_str(m, "content");
        if (!strcmp(role, "system"))
            continue;
        if (!strcmp(role, "tool") && ch_map_has(&shown, ch_json_str(m, "tool_call_id")))
            continue; /* shown with its request in one expandable activity */
        if (*content) {
            if (!strcmp(role, "user"))
                user_message(a, content, i);
            else if (!strcmp(role, "tool"))
                tool_message(a, content, ch_json_str(m, "name"), i);
            else
                assistant_message(a, content, i, false);
            json_object *stats = ch_json_get(m, "_chela_stats");
            if (!strcmp(role, "assistant") && stats)
                metrics_line(a, stats);
        }
        json_object *calls = ch_json_get(m, "tool_calls");
        for (size_t j = 0; calls && j < json_object_array_length(calls); j++) {
            json_object *call = json_object_array_get_idx(calls, j);
            tool_activity(a, call, tool_result(a, ch_json_str(call, "id"), i + 1));
            ch_map_set(&shown, ch_json_str(call, "id"), NULL);
        }
    }
    ch_map_clear(&shown);
    if (a->live_active && a->live.len)
        assistant_message(a, a->live.str, SIZE_MAX, true);
    if (a->stopped_note && !a->busy)
        stopped_note(a);
    cu_close(cu);
    cu_close(cu);
}

/* Welcome */
static void home_sessions(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_t = 34, .pad_b = 12});
    ui_label(a, "Sessions", 13, C_MUTED);
    cu_close(cu);
    if (!a->rows || !a->rows->len) {
        cu_open(cu, 0, &(CuBox){.pad_b = 14});
        ui_label(a, "Your coding sessions will appear here.", 13, C_FAINT);
        cu_close(cu);
    }
    for (size_t i = 0; a->rows && i < CH_MIN(a->rows->len, 5); i++) {
        ChSession *s = a->rows->data[i];
        CuId id = cu_ids(cu, "home-session", s->id);
        cu_open(cu, id, &(CuBox){.row = true, .gap = 12, .w = CU_GROW, CU_PAD2(11, 0), .align_y = CU_CENTER,
                                 .border_b = 1, .border_color = 0xff292929, .bg = cu_hovered(cu, id) ? 0xff222222 : 0,
                                 .interactive = true, .opacity = a->busy ? 0.6f : 0});
        cu_text(cu, 0, s->title, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
        char *base = ch_path_basename(s->project);
        cu_text(cu, 0, base, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_FIT_MAX(160)});
        free(base);
        cu_icon(cu, "chevron-right", 12, C_MUTED);
        cu_close(cu);
        if (cu_clicked(cu, id))
            app_select_session(a, s->id);
    }
}
void welcome_build(App *a) {
    Cu *cu = a->cu;
    if (a->code) {
        cu_open(cu, cu_id(cu, "welcome"), &(CuBox){.w = CU_GROW, .h = CU_GROW, .align_x = CU_CENTER, .pad_t = 36, .scroll_y = true});
        cu_open(cu, 0, &(CuBox){.w = CU_PX(content_width(a))});
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 12, .align_y = CU_CENTER});
        ui_mark(a, 32);
        cu_text(cu, 0, "Let’s build something.", &(CuTextStyle){.font = ui_font(a, 22, 400), .color = C_TEXT});
        cu_close(cu);
        home_sessions(a);
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
