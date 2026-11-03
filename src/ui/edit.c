/* Text editing: caret and selection, word movement, clipboard, undo, password
 * masking, input-method preedit and auto-growing multi-line height. */
#include "cu_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *text;
    size_t caret;
} Snapshot;
static void snapshot_free(void *data) {
    Snapshot *s = data;
    free(s->text);
    free(s);
}
void cu_edit_init(CuEdit *e, bool multiline) {
    if (e->initialized)
        return;
    ch_buf_init(&e->text);
    ch_vec_init(&e->undo, snapshot_free);
    e->multiline = multiline;
    e->initialized = true;
}
void cu_edit_clear(CuEdit *e) {
    if (!e->initialized)
        return;
    if (e->password)
        memset(e->text.str, 0, e->text.len);
    ch_buf_clear(&e->text);
    ch_vec_clear(&e->undo);
    text_layout_free(e->layout);
    free(e->preedit);
    bool multiline = e->multiline, password = e->password;
    memset(e, 0, sizeof(*e));
    e->multiline = multiline;
    e->password = password;
}
static void invalidate_layout(CuEdit *e) {
    e->layout_key = 0;
}
void cu_edit_set(CuEdit *e, const char *text) {
    cu_edit_init(e, e->multiline);
    if (e->password)
        memset(e->text.str, 0, e->text.len);
    ch_buf_assign(&e->text, text ? text : "");
    e->caret = e->anchor = e->text.len;
    e->scroll_x = e->scroll_y = 0;
    ch_vec_clear(&e->undo);
    ch_vec_init(&e->undo, snapshot_free);
    invalidate_layout(e);
}
const char *cu_edit_text(const CuEdit *e) {
    return e->initialized ? e->text.str : "";
}
bool cu_edit_empty(const CuEdit *e) {
    return !e->initialized || !e->text.len;
}
enum { UNDO_TYPE = 1, UNDO_DELETE, UNDO_OTHER };
static void push_undo(CuEdit *e, int kind) {
    if (e->password)
        return;
    int64_t now = ch_monotonic_us();
    if (kind != UNDO_OTHER && kind == e->undo_kind && now - e->last_undo < 1000000) {
        e->last_undo = now;
        return;
    }
    Snapshot *s = ch_new0(Snapshot, 1);
    s->text = ch_strdup(e->text.str);
    s->caret = e->caret;
    ch_vec_push(&e->undo, s);
    if (e->undo.len > 200)
        ch_vec_remove(&e->undo, 0);
    e->undo_kind = kind;
    e->last_undo = now;
}
static void delete_range(CuEdit *e, size_t a, size_t b) {
    if (b > e->text.len)
        b = e->text.len;
    if (a >= b)
        return;
    ch_buf_erase(&e->text, a, b - a);
    e->caret = e->anchor = a;
    invalidate_layout(e);
}
static bool delete_selection(CuEdit *e) {
    if (e->caret == e->anchor)
        return false;
    delete_range(e, CH_MIN(e->caret, e->anchor), CH_MAX(e->caret, e->anchor));
    return true;
}
static void insert_raw(CuEdit *e, const char *text, size_t max_length) {
    char *clean = ch_utf8_make_valid(text, -1);
    if (!e->multiline)
        for (char *p = clean; *p; p++)
            if (*p == '\n' || *p == '\r' || *p == '\t')
                *p = ' ';
    size_t n = strlen(clean);
    if (max_length) {
        size_t chars = ch_utf8_strlen(e->text.str, -1), add = ch_utf8_strlen(clean, -1);
        if (chars >= max_length)
            n = 0;
        else if (chars + add > max_length)
            n = (size_t)(ch_utf8_offset(clean, max_length - chars) - clean);
    }
    ChBuf next;
    ch_buf_init(&next);
    ch_buf_append_len(&next, e->text.str, e->caret);
    ch_buf_append_len(&next, clean, n);
    ch_buf_append(&next, e->text.str + e->caret);
    if (e->password)
        memset(e->text.str, 0, e->text.len);
    ch_buf_clear(&e->text);
    e->text = next;
    e->caret += n;
    e->anchor = e->caret;
    if (e->password)
        memset(clean, 0, strlen(clean));
    free(clean);
    invalidate_layout(e);
}
void cu_edit_insert(CuEdit *e, const char *text) {
    cu_edit_init(e, e->multiline);
    push_undo(e, UNDO_OTHER);
    delete_selection(e);
    insert_raw(e, text, 0);
}
static size_t prev_char(const CuEdit *e, size_t o) {
    return (size_t)(ch_utf8_prev(e->text.str, e->text.str + o) - e->text.str);
}
static size_t next_char(const CuEdit *e, size_t o) {
    return o >= e->text.len ? e->text.len : (size_t)(ch_utf8_next(e->text.str + o) - e->text.str);
}
static size_t prev_word(const CuEdit *e, size_t o) {
    while (o > 0 && (e->text.str[o - 1] == ' ' || e->text.str[o - 1] == '\n'))
        o--;
    size_t start = text_word_start(e->text.str, o);
    return start == o && o > 0 ? prev_char(e, o) : start;
}
static size_t next_word(const CuEdit *e, size_t o) {
    while (o < e->text.len && (e->text.str[o] == ' ' || e->text.str[o] == '\n'))
        o++;
    size_t end = text_word_end(e->text.str, e->text.len, o);
    return end == o ? next_char(e, o) : end;
}

/* The layout shows bullets for passwords; offsets map by character index. */
static size_t to_display(const CuEdit *e, size_t offset) {
    return e->password ? ch_utf8_strlen(e->text.str, (ptrdiff_t)offset) * strlen("•") : offset;
}
static size_t from_display(const CuEdit *e, size_t offset) {
    return e->password ? (size_t)(ch_utf8_offset(e->text.str, offset / strlen("•")) - e->text.str) : offset;
}
static TextLayout *edit_layout(Cu *cu, CuEdit *e, CuFont *font, float width) {
    uint64_t key = text_hash(text_hash(14695981039346656037ull, e->text.str, e->text.len), &font, sizeof(font));
    key = text_hash(key, &width, sizeof(width));
    if (e->layout && e->layout_key == key)
        return e->layout;
    text_layout_free(e->layout);
    ChBuf display;
    ch_buf_init(&display);
    if (e->password)
        for (size_t i = ch_utf8_strlen(e->text.str, -1); i; i--)
            ch_buf_append(&display, "•");
    else
        ch_buf_append_len(&display, e->text.str, e->text.len);
    CuSpan span = {.text = display.str, .length = display.len, .font = font};
    TextOptions o = {.wrap = e->multiline ? CU_WRAP_WORD : CU_WRAP_NONE};
    e->layout = text_layout_new(cu, &span, 1, &o, e->multiline ? width : INFINITY);
    e->layout_key = key;
    ch_buf_clear(&display);
    return e->layout;
}
static void caret_xy(Cu *cu, CuEdit *e, size_t offset, float *x, float *y, float *h) {
    text_layout_caret(cu, e->layout, to_display(e, offset), x, y, h);
}
void edit_pointer(Cu *cu, CuEdit *e, CuRect rect, float x, float y, bool extend, int clicks) {
    if (!e->layout)
        return;
    size_t offset = from_display(e, text_layout_hit(cu, e->layout, x - rect.x + e->scroll_x, y - rect.y + e->scroll_y));
    if (clicks == 2 && !e->password) {
        e->anchor = text_word_start(e->text.str, offset);
        e->caret = text_word_end(e->text.str, e->text.len, offset);
    } else if (clicks >= 3) {
        e->anchor = 0;
        e->caret = e->text.len;
    } else {
        e->caret = offset;
        if (!extend)
            e->anchor = offset;
    }
    e->blink_start = ch_monotonic_us();
    e->desired_x = -1;
}
typedef struct {
    Cu *cu;
    CuEdit *edit;
    CuId focus;
} Paste;
static void pasted(const char *text, void *data) {
    Paste *p = data;
    Cu *cu = p->cu;
    bool alive = false;
    for (int i = 0; i < cu->seen_edit_count; i++)
        alive |= cu->seen_edits[i] == p->edit;
    if (text && alive && cu->focus == p->focus && cu->paste_edit == p->edit) {
        push_undo(p->edit, UNDO_OTHER);
        delete_selection(p->edit);
        insert_raw(p->edit, text, 0);
        p->edit->blink_start = ch_monotonic_us();
        cu->invalid = true;
        cu_invalidate(cu);
    }
    cu->paste_edit = NULL;
    free(p);
}
void edit_handle_input(Cu *cu, El *el, int *flags) {
    CuEdit *e = el->edit;
    const CuEditStyle *st = &el->edit_style;
    for (int i = 0; i < cu->key_count; i++) {
        if (cu->keys[i].consumed)
            continue;
        int key = cu->keys[i].key, mods = cu->keys[i].mods;
        bool shift = mods & CU_MOD_SHIFT, ctrl = mods & CU_MOD_CTRL, handled = true;
        size_t before_caret = e->caret, len_before = e->text.len;
        if (key == -1) {
            push_undo(e, UNDO_TYPE);
            delete_selection(e);
            insert_raw(e, cu->keys[i].text, st->max_length);
        } else if (key == CU_KEY_LEFT || key == CU_KEY_RIGHT) {
            bool left = key == CU_KEY_LEFT;
            if (e->caret != e->anchor && !shift)
                e->caret = left ? CH_MIN(e->caret, e->anchor) : CH_MAX(e->caret, e->anchor);
            else
                e->caret = left ? (ctrl ? prev_word(e, e->caret) : prev_char(e, e->caret))
                                : (ctrl ? next_word(e, e->caret) : next_char(e, e->caret));
            if (!shift)
                e->anchor = e->caret;
            e->desired_x = -1;
        } else if ((key == CU_KEY_UP || key == CU_KEY_DOWN) && e->multiline && e->layout) {
            float x, y, h;
            caret_xy(cu, e, e->caret, &x, &y, &h);
            if (e->desired_x < 0)
                e->desired_x = x;
            float target = key == CU_KEY_UP ? y - h / 2 : y + h * 1.5f;
            if (target < 0)
                e->caret = 0;
            else if (target > ((TextLayout *)e->layout)->h)
                e->caret = e->text.len;
            else
                e->caret = from_display(e, text_layout_hit(cu, e->layout, e->desired_x, target));
            if (!shift)
                e->anchor = e->caret;
        } else if (key == CU_KEY_HOME || key == CU_KEY_END) {
            bool home = key == CU_KEY_HOME;
            if (ctrl || !e->multiline)
                e->caret = home ? 0 : e->text.len;
            else {
                size_t o = e->caret;
                if (home)
                    while (o > 0 && e->text.str[o - 1] != '\n')
                        o--;
                else
                    while (o < e->text.len && e->text.str[o] != '\n')
                        o++;
                e->caret = o;
            }
            if (!shift)
                e->anchor = e->caret;
            e->desired_x = -1;
        } else if (key == CU_KEY_BACKSPACE || key == CU_KEY_DELETE) {
            push_undo(e, UNDO_DELETE);
            if (!delete_selection(e)) {
                if (key == CU_KEY_BACKSPACE && e->caret > 0)
                    delete_range(e, ctrl ? prev_word(e, e->caret) : prev_char(e, e->caret), e->caret);
                else if (key == CU_KEY_DELETE && e->caret < e->text.len)
                    delete_range(e, e->caret, ctrl ? next_word(e, e->caret) : next_char(e, e->caret));
            }
        } else if (key == CU_KEY_RETURN) {
            if (e->multiline && shift && !ctrl) {
                push_undo(e, UNDO_TYPE);
                delete_selection(e);
                insert_raw(e, "\n", st->max_length);
            } else if (st->submit_on_enter || !e->multiline || ctrl)
                *flags |= CU_EDIT_SUBMIT;
            else {
                push_undo(e, UNDO_TYPE);
                delete_selection(e);
                insert_raw(e, "\n", st->max_length);
            }
        } else if (ctrl && !shift && key == 'a') {
            e->anchor = 0;
            e->caret = e->text.len;
        } else if (ctrl && (key == 'c' || key == 'x' || key == CU_KEY_INSERT) && !shift) {
            if (e->caret == e->anchor || e->password)
                handled = key == 'x';
            else {
                size_t a = CH_MIN(e->caret, e->anchor), b = CH_MAX(e->caret, e->anchor);
                char *text = ch_strndup(e->text.str + a, b - a);
                cu_copy(cu, text);
                free(text);
                if (key == 'x') {
                    push_undo(e, UNDO_OTHER);
                    delete_selection(e);
                }
            }
        } else if ((ctrl && !shift && key == 'v') || (shift && !ctrl && key == CU_KEY_INSERT)) {
            if (cu->p->clipboard_request) {
                Paste *p = ch_new0(Paste, 1);
                *p = (Paste){cu, e, cu->focus};
                cu->paste_edit = e;
                cu->p->clipboard_request(cu->window, pasted, p);
            }
        } else if (ctrl && !shift && key == 'z') {
            if (e->undo.len) {
                Snapshot *s = ch_vec_steal(&e->undo, e->undo.len - 1);
                ch_buf_assign(&e->text, s->text);
                e->caret = e->anchor = CH_MIN(s->caret, e->text.len);
                snapshot_free(s);
                e->undo_kind = 0;
                invalidate_layout(e);
            }
        } else
            handled = false;
        if (handled) {
            cu->keys[i].consumed = true;
            e->blink_start = ch_monotonic_us();
            if (e->text.len != len_before || key == 'z')
                *flags |= CU_EDIT_CHANGED;
            (void)before_caret;
        }
    }
    free(e->preedit);
    e->preedit = cu->preedit ? ch_strdup(cu->preedit) : NULL;
}
void edit_measure(Cu *cu, El *el) {
    CuEdit *e = el->edit;
    const CuEditStyle *st = &el->edit_style;
    CuFontMetrics m = cu_font_metrics(cu, st->font);
    TextLayout *l = edit_layout(cu, e, st->font, fmaxf(1, el->w - 2));
    float h = e->multiline ? fmaxf(l->h, m.height) : m.height;
    if (st->min_h > 0)
        h = fmaxf(h, st->min_h);
    if (st->max_h > 0)
        h = fminf(h, st->max_h);
    el->fit_h = ceilf(h);
}
void edit_paint(Cu *cu, CuCanvas *c, El *el) {
    CuEdit *e = el->edit;
    const CuEditStyle *st = &el->edit_style;
    CuRect r = {el->x, el->y, el->w, el->h};
    e->rect = r;
    TextLayout *l = edit_layout(cu, e, st->font, fmaxf(1, el->w - 2));
    bool focused = cu->focus == el->id && !st->disabled;
    if (e->caret > e->text.len)
        e->caret = e->text.len;
    if (e->anchor > e->text.len)
        e->anchor = e->text.len;
    float cx, cy, ch;
    caret_xy(cu, e, e->caret, &cx, &cy, &ch);
    /* Keep the caret in view. */
    if (e->multiline) {
        float max = fmaxf(0, l->h - r.h);
        if (focused) {
            if (cy < e->scroll_y)
                e->scroll_y = cy;
            if (cy + ch > e->scroll_y + r.h)
                e->scroll_y = cy + ch - r.h;
        }
        e->scroll_y = CH_CLAMP(e->scroll_y, 0, max);
        e->scroll_x = 0;
    } else {
        if (cx - e->scroll_x > r.w - 2)
            e->scroll_x = cx - r.w + 2;
        if (cx < e->scroll_x)
            e->scroll_x = cx;
        e->scroll_x = CH_CLAMP(e->scroll_x, 0, fmaxf(0, l->w - r.w + 2));
        e->scroll_y = 0;
    }
    const CuPlatform *p = cu->p;
    p->save(c);
    p->clip(c, (CuRect){r.x - 1, r.y, r.w + 2, r.h});
    float ox = r.x + 1 - e->scroll_x, oy = r.y - e->scroll_y;
    if (!e->text.len && !(e->preedit && *e->preedit) && st->placeholder) {
        CuSpan span = {.text = st->placeholder, .length = strlen(st->placeholder), .font = st->font,
                       .color = st->placeholder_color};
        TextOptions o = {.wrap = e->multiline ? CU_WRAP_WORD : CU_WRAP_NONE, .ellipsize = CU_ELLIPSIZE_END};
        TextLayout *ph = text_layout_new(cu, &span, 1, &o, r.w - 2);
        text_layout_paint(cu, c, ph, r.x + 1, r.y, 0, 0, 0);
        text_layout_free(ph);
    }
    size_t a = focused ? to_display(e, CH_MIN(e->caret, e->anchor)) : 0, b = focused ? to_display(e, CH_MAX(e->caret, e->anchor)) : 0;
    /* The span colour is applied here so cached layouts stay colour-independent. */
    for (int i = 0; i < l->span_count; i++)
        l->spans[i].color = st->disabled ? cu_alpha(st->color, 0.5f) : st->color;
    text_layout_paint(cu, c, l, ox, oy, a, b, st->selection ? st->selection : 0x804a76c9);
    if (focused && e->preedit && *e->preedit) {
        float w = cu_text_width(cu, st->font, e->preedit, strlen(e->preedit));
        CuFontMetrics m = cu_font_metrics(cu, st->font);
        p->fill_rect(c, (CuRect){ox + cx, oy + cy, w, ch}, 0, 0xff303030);
        p->draw_text(c, st->font, ox + cx, oy + cy + m.ascent + (ch - m.height) / 2, e->preedit, strlen(e->preedit),
                     st->color);
        p->fill_rect(c, (CuRect){ox + cx, oy + cy + ch - 1, w, 1}, 0, st->color);
        cx += w;
    }
    if (focused && cu->window_active) {
        int64_t elapsed = cu->now - e->blink_start;
        bool on = elapsed > 10 * CH_USEC_PER_SEC || (elapsed / 530000) % 2 == 0;
        if (on)
            p->fill_rect(c, (CuRect){ox + cx - 0.5f, oy + cy + 1, 1.5f, ch - 2}, 0, cu_apply_alpha(cu, st->color));
        if (elapsed <= 10 * CH_USEC_PER_SEC)
            cu_animate(cu, (int)(530 - (elapsed / 1000) % 530));
        if (cu->p->window_set_text_input)
            cu->p->window_set_text_input(cu->window, true, (CuRect){ox + cx, oy + cy, 1, ch});
    }
    p->restore(c);
}
