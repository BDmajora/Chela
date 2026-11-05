/* Chela UI core: frame lifecycle, input routing, flex layout, layers and painting. */
#include "cu_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK_SIZE (256u * 1024u)
#define EDGE 6.0f

/* Arena for per-frame elements and strings. */
void *cu_arena(Cu *cu, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (!cu->chunk || cu->chunk_used + size > cu->chunk_size) {
        size_t n = size > CHUNK_SIZE ? size : CHUNK_SIZE;
        cu->chunk = ch_alloc(n);
        cu->chunk_size = n;
        cu->chunk_used = 0;
        ch_vec_push(&cu->chunks, cu->chunk);
    }
    void *p = cu->chunk + cu->chunk_used;
    cu->chunk_used += size;
    memset(p, 0, size);
    return p;
}
char *cu_arena_strndup(Cu *cu, const char *s, size_t n) {
    char *d = cu_arena(cu, n + 1);
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}
void *cu_frame_alloc(Cu *cu, size_t size) {
    return cu_arena(cu, size);
}
char *cu_frame_printf(Cu *cu, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *s = ch_strdup_vprintf(format, args);
    va_end(args);
    char *out = cu_arena_strndup(cu, s, strlen(s));
    free(s);
    return out;
}
static void arena_reset(Cu *cu) {
    ch_vec_clear(&cu->chunks);
    ch_vec_init(&cu->chunks, free);
    cu->chunk = NULL;
    cu->chunk_used = cu->chunk_size = 0;
}

/* Ids and state. */
uint64_t text_hash(uint64_t h, const void *data, size_t n) {
    const unsigned char *p = data;
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}
static CuId seed(Cu *cu) {
    return cu->id_depth ? cu->id_stack[cu->id_depth - 1] : 14695981039346656037ull;
}
CuId cu_id(Cu *cu, const char *name) {
    CuId h = text_hash(seed(cu), name, strlen(name));
    return h ? h : 1;
}
CuId cu_idn(Cu *cu, const char *name, int64_t n) {
    CuId h = text_hash(cu_id(cu, name), &n, sizeof(n));
    return h ? h : 1;
}
CuId cu_ids(Cu *cu, const char *name, const char *suffix) {
    CuId h = text_hash(text_hash(cu_id(cu, name), "/", 1), suffix ? suffix : "", suffix ? strlen(suffix) : 0);
    return h ? h : 1;
}
void cu_push_id(Cu *cu, CuId id) {
    if (cu->id_depth < (int)CH_N_ELEMENTS(cu->id_stack))
        cu->id_stack[cu->id_depth++] = id;
}
void cu_pop_id(Cu *cu) {
    if (cu->id_depth)
        cu->id_depth--;
}
static void state_insert(Cu *cu, State *s);
State *cu_state_find(Cu *cu, CuId id) {
    if (!id || !cu->state_cap)
        return NULL;
    for (size_t i = id % cu->state_cap;; i = (i + 1) % cu->state_cap) {
        State *s = cu->states[i];
        if (!s)
            return NULL;
        if (s->id == id)
            return s;
    }
}
static void state_grow(Cu *cu) {
    State **old = cu->states;
    size_t old_cap = cu->state_cap;
    cu->state_cap = old_cap ? old_cap * 2 : 1024;
    cu->states = ch_new0(State *, cu->state_cap);
    cu->state_count = 0;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i])
            state_insert(cu, old[i]);
    free(old);
}
static void state_insert(Cu *cu, State *s) {
    if ((cu->state_count + 1) * 4 >= cu->state_cap * 3)
        state_grow(cu);
    size_t i = s->id % cu->state_cap;
    while (cu->states[i])
        i = (i + 1) % cu->state_cap;
    cu->states[i] = s;
    cu->state_count++;
}
State *cu_state(Cu *cu, CuId id) {
    State *s = cu_state_find(cu, id);
    if (!s) {
        s = ch_new0(State, 1);
        s->id = id;
        state_insert(cu, s);
    }
    s->frame = cu->frame;
    return s;
}
static void state_collect(Cu *cu) {
    /* Forget elements that have not appeared for a while (rebuilds the table). */
    State **old = cu->states;
    size_t old_cap = cu->state_cap;
    cu->states = ch_new0(State *, old_cap);
    cu->state_count = 0;
    for (size_t i = 0; i < old_cap; i++) {
        State *s = old[i];
        if (!s)
            continue;
        if (cu->frame - s->frame > 600) {
            text_layout_free(s->layout);
            free(s);
        } else
            state_insert(cu, s);
    }
    free(old);
}

static void retired_free(void *layout) {
    text_layout_free(layout);
}

/* Context */
Cu *cu_new(const CuPlatform *platform, CuWindow *window) {
    Cu *cu = ch_new0(Cu, 1);
    cu->p = platform;
    cu->window = window;
    ch_vec_init(&cu->chunks, free);
    ch_vec_init(&cu->layers, NULL);
    ch_vec_init(&cu->events, free);
    ch_vec_init(&cu->payloads, NULL);
    ch_vec_init(&cu->retired, retired_free);
    ch_buf_init(&cu->typed);
    cu->mouse_x = cu->mouse_y = -1;
    cu->window_active = true;
    cu->alpha = 1;
    cu->sel_first = cu->sel_last = -1;
    return cu;
}
void cu_free(Cu *cu) {
    if (!cu)
        return;
    if (cu->timer)
        cu->p->timer_remove(cu->timer);
    for (size_t i = 0; i < cu->state_cap; i++)
        if (cu->states[i]) {
            text_layout_free(cu->states[i]->layout);
            free(cu->states[i]);
        }
    free(cu->states);
    ch_vec_clear(&cu->retired);
    arena_reset(cu);
    ch_vec_clear(&cu->chunks);
    ch_vec_clear(&cu->layers);
    for (size_t i = 0; i < cu->payloads.len; i++) {
        cu_payload_clear(cu->payloads.data[i]);
        free(cu->payloads.data[i]);
    }
    ch_vec_clear(&cu->payloads);
    free(cu->link_clicked);
    ch_vec_clear(&cu->events);
    ch_buf_clear(&cu->typed);
    free(cu->hits);
    free(cu->next_hits);
    free(cu->widths);
    free(cu->sel_text);
    free(cu->selectables);
    free(cu->tip_text);
    free(cu->preedit);
    free(cu);
}
void cu_set_build(Cu *cu, void (*build)(Cu *, void *), void *data) {
    cu->build = build;
    cu->build_data = data;
}
const CuPlatform *cu_platform(Cu *cu) {
    return cu->p;
}
CuWindow *cu_window(Cu *cu) {
    return cu->window;
}
float cu_width(Cu *cu) {
    return cu->width;
}
float cu_height(Cu *cu) {
    return cu->height;
}
int64_t cu_now(Cu *cu) {
    return cu->now ? cu->now : ch_monotonic_us();
}
bool cu_window_active(Cu *cu) {
    return cu->window_active;
}
void cu_invalidate(Cu *cu) {
    if (cu->painting)
        cu->invalid = true;
    else if (cu->window)
        cu->p->window_redraw(cu->window);
}
void cu_animate(Cu *cu, int milliseconds) {
    int64_t at = ch_monotonic_us() + (int64_t)milliseconds * 1000;
    if (!cu->wake_at || at < cu->wake_at)
        cu->wake_at = at;
}

/* Fonts */
CuFont *cu_font_desc(Cu *cu, const CuFontDesc *desc) {
    for (int i = 0; i < cu->font_count; i++) {
        struct FontEntry *f = &cu->fonts[i];
        if (f->desc.size == desc->size && f->desc.weight == desc->weight && f->desc.italic == desc->italic &&
            f->desc.monospace == desc->monospace && !strcmp(f->family, desc->family ? desc->family : ""))
            return f->font;
    }
    CuFont *font = cu->p->font_get(desc);
    if (cu->font_count < (int)CH_N_ELEMENTS(cu->fonts)) {
        struct FontEntry *f = &cu->fonts[cu->font_count++];
        f->desc = *desc;
        ch_strlcpy(f->family, desc->family ? desc->family : "", sizeof(f->family));
        f->desc.family = f->family;
        f->font = font;
        f->metrics = cu->p->font_metrics(font);
    }
    return font;
}
CuFont *cu_font(Cu *cu, const char *family, float size, int weight) {
    CuFontDesc d = {.family = family, .size = size, .weight = weight};
    d.monospace = family && strstr(family, "monospace");
    return cu_font_desc(cu, &d);
}
CuFontMetrics cu_font_metrics(Cu *cu, CuFont *font) {
    for (int i = 0; i < cu->font_count; i++)
        if (cu->fonts[i].font == font)
            return cu->fonts[i].metrics;
    return cu->p->font_metrics(font);
}
float cu_text_width(Cu *cu, CuFont *font, const char *text, size_t length) {
    if (!length)
        return 0;
    if (length > 256)
        return cu->p->text_width(font, text, length);
    uint64_t key = text_hash(text_hash(14695981039346656037ull, &font, sizeof(font)), text, length);
    if (!cu->widths)
        cu->widths = ch_new0(struct WidthEntry, 32768);
    size_t i = key & 32767;
    for (int probe = 0; probe < 8; probe++, i = (i + 1) & 32767) {
        struct WidthEntry *e = &cu->widths[i];
        if (e->key == key && e->font == font)
            return e->width;
        if (!e->key) {
            e->key = key;
            e->font = font;
            e->width = cu->p->text_width(font, text, length);
            if (++cu->width_count > 24000) {
                float w = e->width;
                memset(cu->widths, 0, sizeof(struct WidthEntry) * 32768);
                cu->width_count = 0;
                return w;
            }
            return e->width;
        }
    }
    return cu->p->text_width(font, text, length);
}

/* Colours */
CuColor cu_alpha(CuColor c, float alpha) {
    unsigned a = (unsigned)((c >> 24) * CH_CLAMP(alpha, 0.0f, 1.0f) + .5f);
    return (c & 0xffffff) | (a << 24);
}
CuColor cu_mix(CuColor a, CuColor b, float t) {
    CuColor out = 0;
    for (int shift = 0; shift < 32; shift += 8) {
        float x = (float)((a >> shift) & 255), y = (float)((b >> shift) & 255);
        out |= (CuColor)(unsigned)(x + (y - x) * t + .5f) << shift;
    }
    return out;
}
CuColor cu_apply_alpha(Cu *cu, CuColor c) {
    return cu->alpha >= 1 ? c : cu_alpha(c, cu->alpha);
}

/* Hit testing against the previous frame */
static bool contains(CuRect r, float x, float y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}
static CuRect intersect(CuRect a, CuRect b) {
    float x0 = fmaxf(a.x, b.x), y0 = fmaxf(a.y, b.y), x1 = fminf(a.x + a.w, b.x + b.w), y1 = fminf(a.y + a.h, b.y + b.h);
    return (CuRect){x0, y0, fmaxf(0, x1 - x0), fmaxf(0, y1 - y0)};
}
static int modal_floor(Cu *cu) {
    int floor = 0;
    for (int i = 0; i < cu->prev_layer_count; i++)
        if (cu->prev_layer_kinds[i] == CU_LAYER_MODAL)
            floor = i + 1;
    return floor;
}
static int layer_at(Cu *cu, float x, float y) {
    int top = 0;
    for (int i = 0; i < cu->prev_layer_count; i++)
        if (cu->prev_layer_kinds[i] != CU_LAYER_TOOLTIP && contains(cu->prev_layer_rects[i], x, y))
            top = i + 1;
    return top;
}
typedef enum { FIND_INTERACTIVE, FIND_SCROLL, FIND_SELECTABLE, FIND_ANY } FindKind;
static Hit *hit_at(Cu *cu, float x, float y, FindKind kind) {
    int floor = CH_MAX(modal_floor(cu), layer_at(cu, x, y));
    for (int i = cu->hit_count - 1; i >= 0; i--) {
        Hit *h = &cu->hits[i];
        if (h->layer < floor || !contains(h->rect, x, y))
            continue;
        if (kind == FIND_ANY || (kind == FIND_INTERACTIVE && (h->interactive || h->edit || h->drag_window)) ||
            (kind == FIND_SCROLL && h->scroll) || (kind == FIND_SELECTABLE && h->selectable))
            return h;
        /* An interactive element above blocks text selection beneath it. */
        if (kind == FIND_SELECTABLE && (h->interactive || h->edit))
            return NULL;
    }
    return NULL;
}
static Hit *hit_find(Cu *cu, CuId id) {
    for (int i = 0; i < cu->hit_count; i++)
        if (cu->hits[i].id == id)
            return &cu->hits[i];
    return NULL;
}
static CuEdge window_edge(Cu *cu, float x, float y) {
    if (!cu->p->window_has_client_decorations || !cu->p->window_has_client_decorations(cu->window) ||
        (cu->p->window_is_maximized && cu->p->window_is_maximized(cu->window)))
        return CU_EDGE_NONE;
    bool n = y < EDGE, s = y >= cu->height - EDGE, w = x < EDGE, e = x >= cu->width - EDGE;
    return n && w ? CU_EDGE_NW : n && e ? CU_EDGE_NE : s && w ? CU_EDGE_SW : s && e ? CU_EDGE_SE : n ? CU_EDGE_N
         : s ? CU_EDGE_S : w ? CU_EDGE_W : e ? CU_EDGE_E : CU_EDGE_NONE;
}
static CuCursor edge_cursor(CuEdge e) {
    static const CuCursor cursors[] = {CU_CURSOR_DEFAULT,   CU_CURSOR_RESIZE_N,  CU_CURSOR_RESIZE_S,
                                       CU_CURSOR_RESIZE_E,  CU_CURSOR_RESIZE_W,  CU_CURSOR_RESIZE_NE,
                                       CU_CURSOR_RESIZE_NW, CU_CURSOR_RESIZE_SE, CU_CURSOR_RESIZE_SW};
    return cursors[e];
}
static void update_hover(Cu *cu) {
    Hit *h = cu->mouse_x >= 0 ? hit_at(cu, cu->mouse_x, cu->mouse_y, FIND_INTERACTIVE) : NULL;
    CuId hot = h ? h->id : 0;
    if (hot != cu->hot) {
        cu->hot = hot;
        cu_invalidate(cu);
    }
    Hit *any = cu->mouse_x >= 0 ? hit_at(cu, cu->mouse_x, cu->mouse_y, FIND_ANY) : NULL;
    if (cu->tip_suppressed && hot != cu->tip_suppressed)
        cu->tip_suppressed = 0;
    const char *tip = h && h->tooltip && h->id != cu->tip_suppressed ? h->tooltip : NULL;
    CuId tip_id = tip ? h->id : 0;
    if (tip_id != cu->tip_id || (tip && ch_strcmp0(tip, cu->tip_text))) {
        bool was_shown = cu->tip_id && cu_now(cu) - cu->tip_since >= 550000;
        cu->tip_id = tip_id;
        cu->tip_since = ch_monotonic_us();
        free(cu->tip_text);
        cu->tip_text = ch_strdup(tip);
        if (tip_id)
            cu_animate(cu, 560);
        if (was_shown || tip_id)
            cu_invalidate(cu);
    }
    CuCursor cursor = CU_CURSOR_DEFAULT;
    CuEdge edge = window_edge(cu, cu->mouse_x, cu->mouse_y);
    if (edge)
        cursor = edge_cursor(edge);
    else if (cu->drag_sel || cu->drag_edit)
        cursor = CU_CURSOR_TEXT;
    else if (h && h->cursor)
        cursor = h->cursor;
    else if (h && h->edit)
        cursor = CU_CURSOR_TEXT;
    else if (!h && any && any->selectable) {
        State *s = cu_state_find(cu, any->id);
        bool link = s && s->layout && text_layout_link(cu, s->layout, cu->mouse_x - s->rect.x, cu->mouse_y - s->rect.y);
        cursor = link ? CU_CURSOR_POINTER : CU_CURSOR_TEXT;
    }
    if (cursor != cu->cursor && cu->p->window_set_cursor) {
        cu->cursor = cursor;
        cu->p->window_set_cursor(cu->window, cursor);
    }
}

/* Input */
void cu_payload_push(Cu *cu, const char *mime, const void *data, size_t size, bool dropped) {
    CuPayload *p = ch_new0(CuPayload, 1);
    p->mime = ch_strdup(mime);
    p->data = ch_alloc(size + 1);
    memcpy(p->data, data, size);
    p->data[size] = 0;
    p->size = size;
    p->dropped = dropped;
    ch_vec_push(&cu->payloads, p);
    cu_invalidate(cu);
}
bool cu_payload_take(Cu *cu, CuPayload *payload) {
    if (!cu->payloads.len)
        return false;
    CuPayload *p = ch_vec_steal(&cu->payloads, 0);
    *payload = *p;
    free(p);
    return true;
}
void cu_payload_clear(CuPayload *payload) {
    free(payload->mime);
    free(payload->data);
    memset(payload, 0, sizeof(*payload));
}
bool cu_drag_active(Cu *cu) {
    return cu->drag_over;
}
char *cu_link_take(Cu *cu) {
    return ch_steal(&cu->link_clicked);
}
void cu_handle_event(Cu *cu, const CuEvent *event) {
    switch (event->type) {
    case CU_EV_DRAG:
        cu->drag_over = event->focused;
        cu_invalidate(cu);
        return;
    case CU_EV_DROP:
        cu->drag_over = false;
        if (event->mime && event->data)
            cu_payload_push(cu, event->mime, event->data, event->size, true);
        cu_invalidate(cu);
        return;
    case CU_EV_MOTION:
        cu->mouse_x = event->x;
        cu->mouse_y = event->y;
        cu->mods = event->mods;
        if (cu->drag_edit || cu->drag_sel || cu->drag_scrollbar)
            cu_invalidate(cu);
        update_hover(cu);
        return;
    case CU_EV_LEAVE:
        cu->mouse_x = cu->mouse_y = -1;
        update_hover(cu);
        return;
    case CU_EV_FOCUS:
        cu->window_active = event->focused;
        cu_invalidate(cu);
        return;
    case CU_EV_RESIZE:
        cu_invalidate(cu);
        return;
    case CU_EV_CLOSE:
        cu->close_requested = true;
        cu_invalidate(cu);
        return;
    default:
        break;
    }
    CuEvent *copy = ch_new0(CuEvent, 1);
    *copy = *event;
    copy->text = NULL;
    if (event->type == CU_EV_PREEDIT) {
        free(cu->preedit);
        cu->preedit = event->text && *event->text ? ch_strdup(event->text) : NULL;
        free(copy);
        cu_invalidate(cu);
        return;
    }
    if (event->type == CU_EV_TEXT) {
        if (!event->text || !*event->text) {
            free(copy);
            return;
        }
        copy->text = ch_strdup(event->text);
    }
    if (event->type == CU_EV_PRESS || event->type == CU_EV_RELEASE) {
        cu->mouse_x = event->x;
        cu->mouse_y = event->y;
    }
    ch_vec_push(&cu->events, copy);
    cu_invalidate(cu);
}
bool cu_close_requested(Cu *cu) {
    bool r = cu->close_requested;
    cu->close_requested = false;
    return r;
}
static void dismiss(Cu *cu, CuId id) {
    if (cu->dismissed_count < (int)CH_N_ELEMENTS(cu->dismissed))
        cu->dismissed[cu->dismissed_count++] = id;
}
/* Read-only selection. Elements come from the last frame, in reading order. */
static bool same_group(const Selectable *e, int layer, CuId scroller) {
    return e->layer == layer && e->scroller == scroller;
}
/* The place in a group nearest to a point. Lines matter more than columns, so moving
 * past the end of a paragraph selects to its end instead of jumping to its neighbour. */
static bool selection_point(Cu *cu, int layer, CuId scroller, float x, float y, CuId *id, size_t *offset) {
    State *best = NULL;
    float best_score = INFINITY;
    for (int i = 0; i < cu->selectable_count; i++) {
        const Selectable *e = &cu->selectables[i];
        State *s = same_group(e, layer, scroller) ? cu_state_find(cu, e->id) : NULL;
        if (!s || !s->layout)
            continue;
        float dx = x < s->rect.x ? s->rect.x - x : x > s->rect.x + s->rect.w ? x - s->rect.x - s->rect.w : 0;
        float dy = y < s->rect.y ? s->rect.y - y : y >= s->rect.y + s->rect.h ? y - s->rect.y - s->rect.h : 0;
        float score = dy * 4096 + dx;
        if (score < best_score) {
            best_score = score;
            best = s;
        }
    }
    if (!best)
        return false;
    *id = best->id;
    if (y < best->rect.y)
        *offset = 0;
    else if (y >= best->rect.y + best->rect.h)
        *offset = best->layout->length;
    else
        *offset = text_layout_hit(cu, best->layout, x - best->rect.x, y - best->rect.y);
    return true;
}
static void selection_clear(Cu *cu) {
    cu->sel_anchor_id = cu->sel_caret_id = 0;
    cu->sel_first = cu->sel_last = -1;
    cu->sel_focus = false;
}
static void selection_press(Cu *cu, Hit *text, float x, float y, int mods) {
    Hit *area = hit_at(cu, x, y, FIND_SCROLL);
    int layer = text ? text->layer : area ? area->layer : 0;
    CuId scroller = area ? area->id : 0, id = 0;
    size_t offset = 0;
    /* Text in a scrolling area can be reached from the margins around it. */
    if ((!text && !area) || !selection_point(cu, layer, scroller, x, y, &id, &offset)) {
        selection_clear(cu);
        return;
    }
    State *s = cu_state_find(cu, id);
    bool extend = (mods & CU_MOD_SHIFT) && cu->sel_anchor_id && cu->sel_layer == layer && cu->sel_scroller == scroller;
    cu->sel_layer = layer;
    cu->sel_scroller = scroller;
    cu->sel_focus = true;
    cu->drag_sel = true;
    if (cu->click_count >= 3) {
        cu->sel_anchor_id = cu->sel_caret_id = id;
        cu->sel_anchor = 0;
        cu->sel_caret = s->layout->length;
    } else if (cu->click_count == 2) {
        cu->sel_anchor_id = cu->sel_caret_id = id;
        cu->sel_anchor = text_word_start(s->layout->text, offset);
        cu->sel_caret = text_word_end(s->layout->text, s->layout->length, offset);
    } else {
        if (!extend) {
            cu->sel_anchor_id = id;
            cu->sel_anchor = offset;
        }
        cu->sel_caret_id = id;
        cu->sel_caret = offset;
    }
}
/* While dragging, the caret follows the pointer and the text scrolls under it near the edges. */
static void selection_drag(Cu *cu) {
    float x = cu->mouse_x, y = cu->mouse_y;
    State *area = cu->sel_scroller ? cu_state_find(cu, cu->sel_scroller) : NULL;
    if (area) {
        float top = area->rect.y, bottom = top + area->rect.h, margin = 10;
        float over = y < top + margin ? y - top - margin : y > bottom - margin ? y - bottom + margin : 0;
        if (over && area->scroll_max > 0) {
            cu_scroll_by(cu, area->id, CH_CLAMP(over * 0.5f, -40.0f, 40.0f));
            cu_animate(cu, 16);
        }
        y = CH_CLAMP(y, top, bottom - 1);
    }
    CuId id;
    size_t offset;
    if (selection_point(cu, cu->sel_layer, cu->sel_scroller, x, y, &id, &offset)) {
        cu->sel_caret_id = id;
        cu->sel_caret = offset;
    }
}
/* Places the selection among this frame's elements; it ends when its text has gone. */
static void selection_resolve(Cu *cu) {
    cu->sel_first = cu->sel_last = -1;
    if (!cu->sel_anchor_id)
        return;
    int anchor = -1, caret = -1;
    for (int i = 0; i < cu->selectable_count; i++) {
        if (cu->selectables[i].id == cu->sel_anchor_id)
            anchor = i;
        if (cu->selectables[i].id == cu->sel_caret_id)
            caret = i;
    }
    if (anchor < 0 || caret < 0) {
        selection_clear(cu);
        return;
    }
    bool forward = anchor < caret || (anchor == caret && cu->sel_anchor <= cu->sel_caret);
    cu->sel_first = forward ? anchor : caret;
    cu->sel_last = forward ? caret : anchor;
    cu->sel_start = forward ? cu->sel_anchor : cu->sel_caret;
    cu->sel_end = forward ? cu->sel_caret : cu->sel_anchor;
}
/* What Ctrl+A selects: the text last clicked, or else the largest body of text on top. */
static void select_all(Cu *cu) {
    int top = -1, count = 0, floor = modal_floor(cu);
    CuId scroller = 0;
    for (int i = 0; i < cu->selectable_count; i++)
        if (cu->selectables[i].layer >= floor)
            top = CH_MAX(top, cu->selectables[i].layer);
    if (cu->sel_focus && cu->sel_layer == top)
        scroller = cu->sel_scroller;
    else
        for (int i = 0; i < cu->selectable_count; i++) {
            const Selectable *e = &cu->selectables[i];
            int same = 0;
            for (int k = 0; e->layer == top && k < cu->selectable_count; k++)
                same += same_group(&cu->selectables[k], top, e->scroller);
            if (same > count) {
                count = same;
                scroller = e->scroller;
            }
        }
    int first = -1, last = -1;
    for (int i = 0; i < cu->selectable_count; i++)
        if (same_group(&cu->selectables[i], top, scroller)) {
            if (first < 0)
                first = i;
            last = i;
        }
    State *end = last >= 0 ? cu_state_find(cu, cu->selectables[last].id) : NULL;
    if (!end || !end->layout)
        return;
    cu->sel_layer = top;
    cu->sel_scroller = scroller;
    cu->sel_anchor_id = cu->selectables[first].id;
    cu->sel_caret_id = cu->selectables[last].id;
    cu->sel_anchor = 0;
    cu->sel_caret = end->layout->length;
    cu->sel_focus = true;
}
static void press(Cu *cu, const CuEvent *ev) {
    float x = ev->x, y = ev->y;
    cu->mods = ev->mods;
    cu->mouse_down = ev->button == 1;
    /* A click answers the tooltip's question; keep it hidden until the pointer moves on. */
    if (cu->tip_id || cu->hot)
        cu->tip_suppressed = cu->hot;
    cu->tip_id = 0;
    if (ev->button == 1) {
        CuEdge edge = window_edge(cu, x, y);
        if (edge && cu->p->window_begin_resize) {
            cu->p->window_begin_resize(cu->window, edge);
            cu->mouse_down = false;
            return;
        }
    }
    /* Pressing outside a popup closes it (and everything above it) without activating. */
    int target = layer_at(cu, x, y);
    bool inside_popup = target > 0 && cu->prev_layer_kinds[target - 1] == CU_LAYER_POPUP, consumed = false;
    for (int i = cu->prev_layer_count - 1; i >= 0 && i + 1 > target; i--) {
        if (cu->prev_layer_kinds[i] == CU_LAYER_TOOLTIP)
            continue;
        Hit *anchor = cu->prev_layer_anchors[i] ? hit_find(cu, cu->prev_layer_anchors[i]) : NULL;
        if (anchor && contains(anchor->rect, x, y) && cu->prev_layer_kinds[i] == CU_LAYER_POPUP)
            continue;
        dismiss(cu, cu->prev_layers[i]);
        if (cu->prev_layer_kinds[i] != CU_LAYER_POPUP)
            break;
        consumed = !inside_popup;
    }
    if (consumed) {
        cu->pressed = 0;
        cu->mouse_down = false;
        return;
    }
    Hit *h = hit_at(cu, x, y, FIND_INTERACTIVE), *text = h ? NULL : hit_at(cu, x, y, FIND_SELECTABLE);
    cu->pressed = h ? h->id : 0;
    /* Repeated clicks count on buttons, fields and text alike. */
    CuId clicked = h ? h->id : text ? text->id : 0;
    int64_t now = ch_monotonic_us();
    if (clicked && clicked == cu->last_click && now - cu->last_click_time < 400000 &&
        fabsf(x - cu->last_click_x) < 5 && fabsf(y - cu->last_click_y) < 5)
        cu->click_count++;
    else
        cu->click_count = 1;
    cu->last_click = clicked;
    cu->last_click_time = now;
    cu->last_click_x = x;
    cu->last_click_y = y;
    if (ev->button == 3) {
        cu->right_clicked = h ? h->id : 0;
        cu->pressed = 0;
        cu->mouse_down = false;
        return;
    }
    if (ev->button != 1) {
        cu->pressed = 0;
        return;
    }
    if (h && h->drag_window) {
        cu->pressed = 0;
        if (cu->click_count == 2 && cu->p->window_toggle_maximize)
            cu->p->window_toggle_maximize(cu->window);
        else if (cu->p->window_begin_move)
            cu->p->window_begin_move(cu->window);
        cu->mouse_down = false;
        return;
    }
    Hit *area = h ? NULL : hit_at(cu, x, y, FIND_SCROLL);
    if (area) {
        /* Scrollbar thumb */
        State *s = cu_state_find(cu, area->id);
        if (s && s->scroll_max > 0 && x >= area->rect.x + area->rect.w - 10) {
            cu->drag_scrollbar = area->id;
            float track = area->rect.h, thumb = fmaxf(28, track * track / (track + s->scroll_max));
            float pos = (s->scroll / s->scroll_max) * (track - thumb);
            float in = y - area->rect.y;
            cu->drag_offset = in >= pos && in <= pos + thumb ? in - pos : thumb / 2;
            cu->pressed = 0;
            return;
        }
    }
    if (h && h->edit) {
        State *s = cu_state_find(cu, h->id);
        if (s && s->edit) {
            cu->focus = h->id;
            cu->focus_visible = false;
            edit_pointer(cu, s->edit, s->rect, x, y, ev->mods & CU_MOD_SHIFT, cu->click_count);
            cu->drag_edit = h->id;
        }
        selection_clear(cu);
        return;
    }
    /* Pressing a button leaves a text box focused, so typing carries on where it was. */
    State *field = h && h->focusable ? cu_state_find(cu, cu->focus) : NULL;
    if (h && h->focusable && !(field && field->edit)) {
        cu->focus = h->id;
        cu->focus_visible = false;
    }
    if (!h)
        selection_press(cu, text, x, y, ev->mods);
}
static void release(Cu *cu, const CuEvent *ev) {
    if (ev->button == 1 && cu->drag_sel && cu->sel_anchor_id == cu->sel_caret_id && cu->sel_anchor == cu->sel_caret) {
        /* A click on a link, not the start of a selection. */
        State *s = cu_state_find(cu, cu->sel_caret_id);
        const char *link = s && s->layout ? text_layout_link(cu, s->layout, ev->x - s->rect.x, ev->y - s->rect.y) : NULL;
        if (link) {
            free(cu->link_clicked);
            cu->link_clicked = ch_strdup(link);
        }
    }
    if (ev->button == 1) {
        Hit *h = hit_at(cu, ev->x, ev->y, FIND_INTERACTIVE);
        if (cu->pressed && h && h->id == cu->pressed && !h->edit) {
            if (cu->click_count == 2)
                cu->double_clicked = h->id;
            for (int i = 0; i < (int)CH_N_ELEMENTS(cu->clicked); i++)
                if (!cu->clicked[i]) {
                    cu->clicked[i] = h->id;
                    break;
                }
        }
        cu->pressed = 0;
        cu->mouse_down = false;
        cu->drag_edit = cu->drag_scrollbar = 0;
        cu->drag_sel = false;
    }
}
static void scroll(Cu *cu, const CuEvent *ev) {
    /* A message box with more text than it shows scrolls first. */
    Hit *field = hit_at(cu, ev->x, ev->y, FIND_INTERACTIVE);
    State *fs = field && field->edit ? cu_state_find(cu, field->id) : NULL;
    CuEdit *edit = fs ? fs->edit : NULL;
    if (edit && edit->multiline && edit->layout) {
        float max = ((TextLayout *)edit->layout)->h - fs->rect.h;
        if (max > 0.5f) {
            edit->scroll_y = CH_CLAMP(edit->scroll_y + ev->dy, 0, max);
            edit->reveal_caret = false;
            cu_invalidate(cu);
            return;
        }
    }
    Hit *h = hit_at(cu, ev->x, ev->y, FIND_SCROLL);
    if (h)
        cu_scroll_by(cu, h->id, ev->dy);
}
static void key(Cu *cu, const CuEvent *ev) {
    cu->mods = ev->mods;
    /* Escape closes the topmost popup before anything else sees it. */
    if (ev->key == CU_KEY_ESCAPE && !ev->mods) {
        for (int i = cu->prev_layer_count - 1; i >= 0; i--) {
            if (cu->prev_layer_kinds[i] == CU_LAYER_POPUP) {
                dismiss(cu, cu->prev_layers[i]);
                return;
            }
            if (cu->prev_layer_kinds[i] == CU_LAYER_MODAL)
                break;
        }
    }
    /* Keyboard activation of a focused control. */
    Hit *focused = cu->focus ? hit_find(cu, cu->focus) : NULL;
    if (focused && focused->focusable && !focused->edit && !ev->mods &&
        (ev->key == CU_KEY_RETURN || ev->key == ' ')) {
        for (int i = 0; i < (int)CH_N_ELEMENTS(cu->clicked); i++)
            if (!cu->clicked[i]) {
                cu->clicked[i] = cu->focus;
                break;
            }
        return;
    }
    if (cu->key_count < (int)CH_N_ELEMENTS(cu->keys)) {
        cu->keys[cu->key_count].key = ev->key;
        cu->keys[cu->key_count].mods = ev->mods & (CU_MOD_CTRL | CU_MOD_SHIFT | CU_MOD_ALT | CU_MOD_SUPER);
        cu->keys[cu->key_count].text = NULL;
        cu->keys[cu->key_count++].consumed = false;
    }
}
static void clear_keys(Cu *cu) {
    for (int i = 0; i < cu->key_count; i++)
        ch_clear_pointer(&cu->keys[i].text, free);
    cu->key_count = 0;
}
static void process_input(Cu *cu) {
    ch_clear_pointer(&cu->link_clicked, free);
    memset(cu->clicked, 0, sizeof(cu->clicked));
    cu->right_clicked = cu->double_clicked = 0;
    clear_keys(cu);
    cu->dismissed_count = 0;
    cu->scrolled_count = 0;
    for (size_t i = 0; i < cu->events.len; i++) {
        CuEvent *ev = cu->events.data[i];
        switch (ev->type) {
        case CU_EV_PRESS:
            press(cu, ev);
            break;
        case CU_EV_RELEASE:
            release(cu, ev);
            break;
        case CU_EV_SCROLL:
            scroll(cu, ev);
            break;
        case CU_EV_KEY:
            key(cu, ev);
            break;
        case CU_EV_TEXT:
            if (cu->key_count < (int)CH_N_ELEMENTS(cu->keys)) {
                cu->keys[cu->key_count].key = -1;
                cu->keys[cu->key_count].mods = 0;
                cu->keys[cu->key_count].consumed = false;
                cu->keys[cu->key_count++].text = (char *)ch_steal(&ev->text);
            }
            break;
        default:
            break;
        }
        free((char *)ev->text);
    }
    ch_vec_clear(&cu->events);
    ch_vec_init(&cu->events, free);
    /* Drags follow the pointer; a double or triple click keeps the word or text it chose. */
    if (cu->drag_edit && cu->click_count < 2) {
        State *s = cu_state_find(cu, cu->drag_edit);
        if (s && s->edit)
            edit_pointer(cu, s->edit, s->rect, cu->mouse_x, cu->mouse_y, true, 1);
    }
    if (cu->drag_sel && cu->click_count < 2)
        selection_drag(cu);
    if (cu->drag_scrollbar) {
        State *s = cu_state_find(cu, cu->drag_scrollbar);
        if (s && s->scroll_max > 0) {
            float track = s->rect.h, thumb = fmaxf(28, track * track / (track + s->scroll_max));
            float t = (cu->mouse_y - s->rect.y - cu->drag_offset) / fmaxf(1, track - thumb);
            s->scroll_target = CH_CLAMP(t, 0, 1) * s->scroll_max;
            s->scroll_pending = true;
        }
    }
}
bool cu_consume_key(Cu *cu, int key, int mods, bool any_mods) {
    for (int i = 0; i < cu->key_count; i++) {
        if (cu->keys[i].consumed || cu->keys[i].key != key)
            continue;
        if (!any_mods && cu->keys[i].mods != mods)
            continue;
        cu->keys[i].consumed = true;
        return true;
    }
    return false;
}
bool cu_key(Cu *cu, int key, int mods) {
    return cu_consume_key(cu, key, mods, false);
}
bool cu_key_any(Cu *cu, int key) {
    return cu_consume_key(cu, key, 0, true);
}
bool cu_typed(Cu *cu, int ch) {
    for (int i = 0; i < cu->key_count; i++) {
        const char *text = cu->keys[i].text;
        if (!cu->keys[i].consumed && cu->keys[i].key == -1 && text && tolower((unsigned char)text[0]) == ch && !text[1]) {
            cu->keys[i].consumed = true;
            return true;
        }
    }
    return false;
}
int cu_mods(Cu *cu) {
    return cu->mods;
}

/* Interaction queries */
bool cu_hovered(Cu *cu, CuId id) {
    return id && cu->hot == id;
}
bool cu_hovered_within(Cu *cu, CuId id) {
    Hit *h = hit_find(cu, id);
    State *s = h ? NULL : cu_state_find(cu, id);
    CuRect r = h ? h->rect : s ? s->rect : (CuRect){0};
    return cu->mouse_x >= 0 && contains(r, cu->mouse_x, cu->mouse_y) &&
           (h ? h->layer : 0) >= CH_MAX(modal_floor(cu), layer_at(cu, cu->mouse_x, cu->mouse_y));
}
bool cu_pressed(Cu *cu, CuId id) {
    return id && cu->pressed == id && cu->mouse_down && cu->hot == id;
}
bool cu_clicked(Cu *cu, CuId id) {
    for (int i = 0; id && i < (int)CH_N_ELEMENTS(cu->clicked); i++)
        if (cu->clicked[i] == id) {
            cu->clicked[i] = 0;
            return true;
        }
    return false;
}
bool cu_right_clicked(Cu *cu, CuId id) {
    if (id && cu->right_clicked == id) {
        cu->right_clicked = 0;
        return true;
    }
    return false;
}
bool cu_double_clicked(Cu *cu, CuId id) {
    return id && cu->double_clicked == id;
}
CuRect cu_rect(Cu *cu, CuId id) {
    State *s = cu_state_find(cu, id);
    return s ? s->rect : (CuRect){0};
}
bool cu_exists(Cu *cu, CuId id) {
    State *s = cu_state_find(cu, id);
    return s && s->frame >= cu->frame - 1;
}
bool cu_focused(Cu *cu, CuId id) {
    return id && cu->focus == id;
}
bool cu_focus_visible(Cu *cu, CuId id) {
    return cu_focused(cu, id) && cu->focus_visible;
}
void cu_focus(Cu *cu, CuId id) {
    if (id)
        cu_state(cu, id); /* it may first be described in the next frame */
    if (cu->focus != id) {
        cu->focus = id;
        cu->focus_visible = false;
        cu_invalidate(cu);
    }
}
CuId cu_focus_id(Cu *cu) {
    return cu->focus;
}
float cu_mouse_x(Cu *cu) {
    return cu->mouse_x;
}
float cu_mouse_y(Cu *cu) {
    return cu->mouse_y;
}
void cu_scroll_to(Cu *cu, CuId id, float y) {
    State *s = cu_state(cu, id);
    s->scroll_target = y;
    s->scroll_pending = true;
}
void cu_scroll_by(Cu *cu, CuId id, float dy) {
    State *s = cu_state_find(cu, id);
    if (!s)
        return;
    float base = s->scroll_pending ? s->scroll_target : s->scroll;
    if (isinf(base))
        base = s->scroll_max;
    s->scroll_target = CH_CLAMP(base + dy, 0, s->scroll_max);
    s->scroll_pending = true;
    if (cu->scrolled_count < (int)CH_N_ELEMENTS(cu->scrolled))
        cu->scrolled[cu->scrolled_count++] = id;
    cu_invalidate(cu);
}
float cu_scroll_y(Cu *cu, CuId id) {
    State *s = cu_state_find(cu, id);
    return s ? s->scroll : 0;
}
float cu_scroll_target(Cu *cu, CuId id) {
    (void)cu;
    State *s = cu_state_find(cu, id);
    if (!s)
        return 0;
    if (!s->scroll_pending)
        return s->scroll;
    return isinf(s->scroll_target) ? s->scroll_max : s->scroll_target;
}
float cu_scroll_max(Cu *cu, CuId id) {
    State *s = cu_state_find(cu, id);
    return s ? s->scroll_max : 0;
}
bool cu_scrolled_by_user(Cu *cu, CuId id) {
    for (int i = 0; i < cu->scrolled_count; i++)
        if (cu->scrolled[i] == id)
            return true;
    return id == cu->drag_scrollbar && id;
}
bool cu_layer_dismissed(Cu *cu, CuId layer) {
    for (int i = 0; i < cu->dismissed_count; i++)
        if (cu->dismissed[i] == layer)
            return true;
    return false;
}
bool cu_modal_active(Cu *cu) {
    return modal_floor(cu) > 0;
}
void cu_copy(Cu *cu, const char *text) {
    if (text && cu->p->clipboard_set)
        cu->p->clipboard_set(cu->window, text);
}
const char *cu_selection_text(Cu *cu) {
    if (cu->sel_first < 0)
        return NULL;
    ChBuf out;
    ch_buf_init(&out);
    State *previous = NULL;
    bool tight = false;
    for (int i = cu->sel_first; i <= cu->sel_last; i++) {
        if (!same_group(&cu->selectables[i], cu->sel_layer, cu->sel_scroller))
            continue;
        State *s = cu_state_find(cu, cu->selectables[i].id);
        if (!s || !s->layout)
            continue;
        size_t from = i == cu->sel_first ? cu->sel_start : 0, to = i == cu->sel_last ? cu->sel_end : s->layout->length;
        to = CH_MIN(to, s->layout->length);
        from = CH_MIN(from, to);
        /* Cells of a row part with a tab; blocks with the spacing they show. */
        if (previous && out.len) {
            float gap = s->rect.y - previous->rect.y - previous->layout->h;
            bool close = gap <= 6 || (tight && cu->selectables[i].tight);
            ch_buf_append(&out, s->rect.y < previous->rect.y + previous->rect.h - 1 ? "\t" : close ? "\n" : "\n\n");
        }
        ch_buf_append_len(&out, s->layout->text + from, to - from);
        previous = s;
        tight = cu->selectables[i].tight;
    }
    free(cu->sel_text);
    cu->sel_text = ch_buf_steal(&out);
    return *cu->sel_text ? cu->sel_text : NULL;
}
void cu_clear_selection(Cu *cu) {
    selection_clear(cu);
}
bool cu_selecting(Cu *cu) {
    return cu->drag_sel;
}

/* Tree construction */
static El *el_new(Cu *cu, int type, CuId id) {
    El *e = cu_arena(cu, sizeof(El));
    e->type = type;
    El *parent = cu->depth ? cu->stack[cu->depth - 1] : NULL;
    e->parent = parent;
    if (parent) {
        if (parent->last)
            parent->last->next = e;
        else
            parent->first = e;
        parent->last = e;
        parent->child_count++;
        e->layer = parent->layer;
    }
    e->id = id ? id : text_hash(parent ? parent->id : 7, &parent->child_count, sizeof(int));
    if (!e->id)
        e->id = 1;
    return e;
}
void cu_open(Cu *cu, CuId id, const CuBox *box) {
    El *e = el_new(cu, EL_BOX, id);
    if (box)
        e->box = *box;
    if (e->box.tooltip) /* callers may free theirs once the box is described */
        e->box.tooltip = cu_arena_strndup(cu, e->box.tooltip, strlen(e->box.tooltip));
    if (cu->depth < (int)CH_N_ELEMENTS(cu->stack))
        cu->stack[cu->depth++] = e;
}
void cu_close(Cu *cu) {
    if (cu->depth > 1)
        cu->depth--;
}
void cu_spacer(Cu *cu, CuSize w, CuSize h) {
    El *e = el_new(cu, EL_SPACER, 0);
    e->box.w = w;
    e->box.h = h;
}
static void text_element(Cu *cu, CuId id, CuSpan *spans, int count, const CuTextStyle *style, const CuPara *para) {
    El *e = el_new(cu, EL_TEXT, id);
    e->spans = spans;
    e->span_count = count;
    if (style) {
        e->text_style = *style;
        e->text.wrap = style->wrap;
        e->text.ellipsize = style->ellipsize;
        e->text.align = style->align;
        e->text.line_height = style->line_height;
        e->text.max_lines = style->max_lines;
        e->selectable = style->selectable;
        /* CU_SIZE_FIT and an unset size are both zero: only the limits tell CU_FIT_MAX from "not given". */
        bool sized = style->w.kind || style->w.value || style->w.min || style->w.max;
        e->box.w = sized ? style->w : style->wrap ? CU_GROW : CU_FIT;
        if (style->max_width > 0 && !e->box.w.max)
            e->box.w.max = style->max_width;
    }
    if (para) {
        e->text.wrap = para->preformatted ? CU_WRAP_CHAR : CU_WRAP_WORD;
        e->text.indent = para->indent;
        e->text.hanging = para->hanging;
        e->text.line_height = para->line_height;
        e->text.preformatted = para->preformatted;
        e->text.align = para->align;
        e->selectable = para->selectable;
        e->tight = para->tight;
        e->box.w = CU_GROW;
        e->box.pad_b = para->space_after;
    }
}
void cu_text(Cu *cu, CuId id, const char *text, const CuTextStyle *style) {
    CuSpan *span = cu_arena(cu, sizeof(CuSpan));
    size_t n = text ? strlen(text) : 0;
    span->text = cu_arena_strndup(cu, text ? text : "", n);
    span->length = n;
    span->font = style->font;
    span->color = style->color;
    text_element(cu, id, span, 1, style, NULL);
}
void cu_textf(Cu *cu, const CuTextStyle *style, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *s = ch_strdup_vprintf(format, args);
    va_end(args);
    cu_text(cu, 0, s, style);
    free(s);
}
void cu_rich(Cu *cu, CuId id, const CuSpan *spans, int count, const CuPara *para) {
    CuSpan *copy = cu_arena(cu, sizeof(CuSpan) * (size_t)(count ? count : 1));
    for (int i = 0; i < count; i++) {
        copy[i] = spans[i];
        size_t n = spans[i].length == SIZE_MAX ? strlen(spans[i].text) : spans[i].length;
        copy[i].text = cu_arena_strndup(cu, spans[i].text, n);
        copy[i].length = n;
        copy[i].link = spans[i].link ? cu_arena_strndup(cu, spans[i].link, strlen(spans[i].link)) : NULL;
    }
    CuPara p = para ? *para : (CuPara){0};
    text_element(cu, id, copy, count, NULL, &p);
}
void cu_icon(Cu *cu, const char *name, float size, CuColor color) {
    El *e = el_new(cu, EL_ICON, 0);
    e->icon = cu_arena_strndup(cu, name, strlen(name));
    e->icon_size = size;
    e->color = color;
    e->box.w = e->box.h = CU_PX(size);
}
void cu_custom(Cu *cu, CuId id, CuSize w, CuSize h, CuPaintFn paint, void *data) {
    El *e = el_new(cu, EL_CUSTOM, id);
    e->box.w = w;
    e->box.h = h;
    e->paint = paint;
    e->paint_data = data;
}
int cu_edit(Cu *cu, CuId id, CuEdit *edit, const CuEditStyle *style) {
    El *e = el_new(cu, EL_EDIT, id);
    e->edit = edit;
    e->edit_style = *style;
    e->box.w = style->w.kind || style->w.value ? style->w : CU_GROW;
    e->box.interactive = !style->disabled;
    e->box.focusable = !style->disabled;
    e->box.cursor = CU_CURSOR_TEXT;
    if (!edit->initialized)
        cu_edit_init(edit, edit->multiline);
    State *s = cu_state(cu, e->id);
    s->edit = style->disabled ? NULL : edit;
    if (cu->seen_edit_count < (int)CH_N_ELEMENTS(cu->seen_edits))
        cu->seen_edits[cu->seen_edit_count++] = edit;
    int flags = 0;
    if (cu->focus == e->id && !style->disabled)
        edit_handle_input(cu, e, &flags);
    return flags;
}
void cu_layer_open(Cu *cu, CuId id, const CuLayer *layer, const CuBox *box) {
    LayerRec *rec = cu_arena(cu, sizeof(LayerRec));
    rec->id = id;
    rec->layer = *layer;
    El *e = cu_arena(cu, sizeof(El));
    e->type = EL_BOX;
    e->id = id;
    if (box)
        e->box = *box;
    e->layer = (int)cu->layers.len + 1;
    rec->root = e;
    ch_vec_push(&cu->layers, rec);
    if (cu->depth < (int)CH_N_ELEMENTS(cu->stack))
        cu->stack[cu->depth++] = e;
}
void cu_layer_close(Cu *cu) {
    if (cu->depth > 1)
        cu->depth--;
}

/* Layout */
static float clampf(CuSize s, float v) {
    if (s.min > 0 && v < s.min)
        v = s.min;
    if (s.max > 0 && v > s.max)
        v = s.max;
    return v;
}
static bool grows(CuSize s) {
    return s.kind == CU_SIZE_GROW;
}
static void layout_text(Cu *cu, El *e, float width) {
    State *s = cu_state(cu, e->id);
    uint64_t key = 14695981039346656037ull;
    for (int i = 0; i < e->span_count; i++) {
        key = text_hash(key, e->spans[i].text, e->spans[i].length);
        key = text_hash(key, &e->spans[i].font, sizeof(void *));
        key = text_hash(key, &e->spans[i].color, sizeof(CuColor));
        key = text_hash(key, &e->spans[i].bg, sizeof(CuColor));
    }
    key = text_hash(key, &e->text, sizeof(e->text));
    if (!s->layout || s->layout->key != key || fabsf(s->layout->width_key - width) > 0.25f) {
        if (s->layout)
            ch_vec_push(&cu->retired, s->layout);
        s->layout = text_layout_new(cu, e->spans, e->span_count, &e->text, width);
        s->layout->key = key;
        s->layout->width_key = width;
    }
    e->layout = s->layout;
}
static void measure_w(Cu *cu, El *e) {
    float fit = 0, min = 0;
    for (El *c = e->first; c; c = c->next)
        measure_w(cu, c);
    switch (e->type) {
    case EL_TEXT:
        fit = text_natural_width(cu, e->spans, e->span_count, &e->text) + e->text.indent;
        min = e->text.wrap || e->text.ellipsize ? 0 : fit;
        break;
    case EL_ICON:
        fit = min = e->icon_size;
        break;
    case EL_EDIT:
        fit = 60;
        break;
    case EL_SPACER:
    case EL_CUSTOM:
        break;
    default: {
        int n = 0;
        for (El *c = e->first; c; c = c->next, n++) {
            float cf = c->box.w.kind == CU_SIZE_PERCENT ? 0 : c->fit_w, cm = c->box.w.kind == CU_SIZE_PERCENT ? 0 : c->min_w;
            if (e->box.row) {
                fit += cf;
                min += cm;
            } else {
                fit = fmaxf(fit, cf);
                min = fmaxf(min, cm);
            }
        }
        if (e->box.row && n > 1) {
            fit += e->box.gap * (float)(n - 1);
            min += e->box.gap * (float)(n - 1);
        }
        fit += e->box.pad_l + e->box.pad_r;
        min += e->box.pad_l + e->box.pad_r;
    }
    }
    if (e->type == EL_TEXT) {
        fit += e->box.pad_l + e->box.pad_r;
        min += e->box.pad_l + e->box.pad_r;
    }
    if (e->box.w.kind == CU_SIZE_FIXED)
        fit = min = e->box.w.value;
    else if (e->box.w.kind == CU_SIZE_PERCENT)
        fit = min = 0;
    else {
        fit = clampf(e->box.w, fit);
        min = fminf(clampf(e->box.w, min), fit);
    }
    e->fit_w = fit;
    e->min_w = min;
}
static void distribute(float *sizes, const float *mins, const CuSize *kinds, int n, float free_space) {
    if (free_space > 0.01f) {
        for (int round = 0; round < 4 && free_space > 0.01f; round++) {
            int growers = 0;
            for (int i = 0; i < n; i++)
                if (grows(kinds[i]) && (kinds[i].max <= 0 || sizes[i] < kinds[i].max - 0.01f))
                    growers++;
            if (!growers)
                break;
            float share = free_space / (float)growers, used = 0;
            for (int i = 0; i < n; i++) {
                if (!grows(kinds[i]) || (kinds[i].max > 0 && sizes[i] >= kinds[i].max - 0.01f))
                    continue;
                float target = sizes[i] + share;
                if (kinds[i].max > 0 && target > kinds[i].max)
                    target = kinds[i].max;
                used += target - sizes[i];
                sizes[i] = target;
            }
            free_space -= used;
        }
    } else if (free_space < -0.01f) {
        /* Take space from the widest children first, so short labels keep their text
         * while the longest one is ellipsized. */
        float need = -free_space;
        for (int round = 0; round < 2 * n + 2 && need > 0.01f; round++) {
            float top = -1;
            for (int i = 0; i < n; i++)
                if (kinds[i].kind != CU_SIZE_FIXED && sizes[i] > mins[i] + 0.01f)
                    top = fmaxf(top, sizes[i]);
            if (top < 0)
                break;
            /* Lower the widest children together until one reaches its minimum or the next width. */
            float floor = 0;
            int count = 0;
            for (int i = 0; i < n; i++) {
                if (kinds[i].kind == CU_SIZE_FIXED || sizes[i] <= mins[i] + 0.01f)
                    continue;
                if (sizes[i] >= top - 0.01f) {
                    count++;
                    floor = fmaxf(floor, mins[i]);
                } else
                    floor = fmaxf(floor, sizes[i]);
            }
            float step = fminf(top - floor, need / (float)count);
            for (int i = 0; i < n; i++)
                if (kinds[i].kind != CU_SIZE_FIXED && sizes[i] > mins[i] + 0.01f && sizes[i] >= top - 0.01f)
                    sizes[i] -= step;
            need -= step * (float)count;
        }
    }
}
static void assign_w(Cu *cu, El *e, float w) {
    e->w = w;
    if (e->type == EL_TEXT)
        layout_text(cu, e, fmaxf(1, w - e->box.pad_l - e->box.pad_r));
    if (!e->first)
        return;
    float inner = fmaxf(0, w - e->box.pad_l - e->box.pad_r);
    int n = e->child_count;
    float stack_sizes[64], stack_mins[64];
    CuSize stack_kinds[64];
    float *sizes = n <= 64 ? stack_sizes : ch_alloc(sizeof(float) * (size_t)n);
    float *mins = n <= 64 ? stack_mins : ch_alloc(sizeof(float) * (size_t)n);
    CuSize *kinds = n <= 64 ? stack_kinds : ch_alloc(sizeof(CuSize) * (size_t)n);
    int i = 0;
    if (e->box.row) {
        float total = e->box.gap * (float)(n - 1);
        for (El *c = e->first; c; c = c->next, i++) {
            sizes[i] = c->box.w.kind == CU_SIZE_PERCENT ? inner * c->box.w.value : c->fit_w;
            mins[i] = c->box.w.kind == CU_SIZE_PERCENT ? sizes[i] : c->min_w;
            kinds[i] = c->box.w;
            total += sizes[i];
        }
        distribute(sizes, mins, kinds, n, inner - total);
    } else {
        for (El *c = e->first; c; c = c->next, i++) {
            CuSize k = c->box.w;
            float cw = k.kind == CU_SIZE_PERCENT ? inner * k.value : k.kind == CU_SIZE_FIXED ? k.value
                     : grows(k) ? clampf(k, inner) : fminf(c->fit_w, fmaxf(inner, c->min_w));
            sizes[i] = cw;
        }
    }
    i = 0;
    for (El *c = e->first; c; c = c->next, i++)
        assign_w(cu, c, fmaxf(0, sizes[i]));
    if (sizes != stack_sizes) {
        free(sizes);
        free(mins);
        free(kinds);
    }
}
static void measure_h(Cu *cu, El *e) {
    float fit = 0, min = 0;
    for (El *c = e->first; c; c = c->next)
        measure_h(cu, c);
    switch (e->type) {
    case EL_TEXT:
        fit = min = (e->layout ? e->layout->h : 0) + e->box.pad_t + e->box.pad_b;
        break;
    case EL_ICON:
        fit = min = e->icon_size;
        break;
    case EL_EDIT:
        edit_measure(cu, e);
        fit = min = e->fit_h;
        break;
    case EL_SPACER:
    case EL_CUSTOM:
        break;
    default: {
        int n = 0;
        for (El *c = e->first; c; c = c->next, n++) {
            float cf = c->box.h.kind == CU_SIZE_PERCENT ? 0 : c->fit_h, cm = c->box.h.kind == CU_SIZE_PERCENT ? 0 : c->min_h;
            if (!e->box.row) {
                fit += cf;
                min += cm;
            } else {
                fit = fmaxf(fit, cf);
                min = fmaxf(min, cm);
            }
        }
        if (!e->box.row && n > 1) {
            fit += e->box.gap * (float)(n - 1);
            min += e->box.gap * (float)(n - 1);
        }
        fit += e->box.pad_t + e->box.pad_b;
        min += e->box.pad_t + e->box.pad_b;
        if (e->box.scroll_y) {
            e->content_h = fit;
            min = e->box.pad_t + e->box.pad_b;
        }
    }
    }
    if (e->box.h.kind == CU_SIZE_FIXED)
        fit = min = e->box.h.value;
    else if (e->box.h.kind == CU_SIZE_PERCENT)
        fit = min = 0;
    else {
        fit = clampf(e->box.h, fit);
        min = fminf(clampf(e->box.h, min), fit);
    }
    e->fit_h = fit;
    e->min_h = min;
}
static void assign_h(Cu *cu, El *e, float h) {
    e->h = h;
    if (!e->first)
        return;
    float inner = fmaxf(0, h - e->box.pad_t - e->box.pad_b);
    int n = e->child_count;
    float stack_sizes[64], stack_mins[64];
    CuSize stack_kinds[64];
    float *sizes = n <= 64 ? stack_sizes : ch_alloc(sizeof(float) * (size_t)n);
    float *mins = n <= 64 ? stack_mins : ch_alloc(sizeof(float) * (size_t)n);
    CuSize *kinds = n <= 64 ? stack_kinds : ch_alloc(sizeof(CuSize) * (size_t)n);
    int i = 0;
    if (!e->box.row) {
        float total = e->box.gap * (float)(n - 1);
        for (El *c = e->first; c; c = c->next, i++) {
            sizes[i] = c->box.h.kind == CU_SIZE_PERCENT ? inner * c->box.h.value : c->fit_h;
            mins[i] = c->box.h.kind == CU_SIZE_PERCENT ? sizes[i] : c->min_h;
            kinds[i] = c->box.h;
            total += sizes[i];
        }
        float space = inner - total;
        if (e->box.scroll_y && space < 0)
            space = 0;
        distribute(sizes, mins, kinds, n, space);
    } else {
        for (El *c = e->first; c; c = c->next, i++) {
            CuSize k = c->box.h;
            sizes[i] = k.kind == CU_SIZE_PERCENT ? inner * k.value : k.kind == CU_SIZE_FIXED ? k.value
                     : grows(k) ? clampf(k, inner) : c->box.scroll_y ? fminf(c->fit_h, inner) : c->fit_h;
        }
    }
    i = 0;
    for (El *c = e->first; c; c = c->next, i++)
        assign_h(cu, c, fmaxf(0, sizes[i]));
    if (sizes != stack_sizes) {
        free(sizes);
        free(mins);
        free(kinds);
    }
}
static float align_factor(uint8_t a) {
    return a == CU_CENTER ? 0.5f : a == CU_END ? 1.0f : 0.0f;
}
static void add_hit(Cu *cu, El *e, CuRect visible) {
    bool selectable = e->type == EL_TEXT && e->selectable;
    bool wanted = e->box.interactive || e->box.scroll_y || selectable || e->type == EL_EDIT || e->box.drag_window ||
                  e->box.tooltip;
    if (!wanted || e->box.passthrough || visible.w <= 0 || visible.h <= 0)
        return;
    if (cu->next_hit_count == cu->next_hit_cap) {
        cu->next_hit_cap = cu->next_hit_cap ? cu->next_hit_cap * 2 : 512;
        cu->next_hits = ch_realloc(cu->next_hits, sizeof(Hit) * (size_t)cu->next_hit_cap);
    }
    Hit *h = &cu->next_hits[cu->next_hit_count++];
    *h = (Hit){.id = e->id, .rect = visible, .layer = e->layer, .interactive = e->box.interactive,
               .scroll = e->box.scroll_y, .selectable = selectable, .edit = e->type == EL_EDIT,
               .drag_window = e->box.drag_window, .focusable = e->box.focusable, .cursor = e->box.cursor};
    h->tooltip = e->box.tooltip; /* in the arena, copied by cu_open */
}
static void position(Cu *cu, El *e, float x, float y, CuRect clip) {
    e->x = x;
    e->y = y;
    e->clip = clip;
    CuRect rect = {x, y, e->w, e->h};
    State *s = cu_state(cu, e->id);
    s->rect = rect;
    CuRect visible = intersect(clip, rect);
    add_hit(cu, e, visible);
    if (e->type == EL_TEXT && e->selectable && e->layout) {
        for (El *p = e->parent; p && !e->scroller; p = p->parent)
            if (p->box.scroll_y)
                e->scroller = p->id;
        if (cu->selectable_count == cu->selectable_cap) {
            cu->selectable_cap = cu->selectable_cap ? cu->selectable_cap * 2 : 256;
            cu->selectables = ch_realloc(cu->selectables, sizeof(Selectable) * (size_t)cu->selectable_cap);
        }
        cu->selectables[cu->selectable_count++] = (Selectable){e->id, e->scroller, e->layer, e->tight};
        e->sel_index = cu->selectable_count;
    }
    if (e->box.focusable && cu->focusable_count < (int)CH_N_ELEMENTS(cu->focusables)) {
        cu->focusable_layer[cu->focusable_count] = e->layer;
        cu->focusables[cu->focusable_count++] = e->id;
    }
    if (!e->first)
        return;
    CuRect inner_clip = e->box.clip || e->box.scroll_y ? visible : clip;
    float inner_w = fmaxf(0, e->w - e->box.pad_l - e->box.pad_r), inner_h = fmaxf(0, e->h - e->box.pad_t - e->box.pad_b);
    float used = e->box.gap * (float)(e->child_count - 1);
    for (El *c = e->first; c; c = c->next)
        used += e->box.row ? c->w : c->h;
    if (e->box.row) {
        float cursor = x + e->box.pad_l + fmaxf(0, inner_w - used) * align_factor(e->box.align_x);
        for (El *c = e->first; c; c = c->next) {
            float cy = y + e->box.pad_t + fmaxf(0, inner_h - c->h) * align_factor(e->box.align_y);
            position(cu, c, cursor, cy, inner_clip);
            cursor += c->w + e->box.gap;
        }
        return;
    }
    float offset = 0;
    if (e->box.scroll_y) {
        float content = used + e->box.pad_t + e->box.pad_b;
        s->scroll_max = fmaxf(0, content - e->h);
        if (s->scroll_pending) {
            s->scroll = isinf(s->scroll_target) ? s->scroll_max : CH_CLAMP(s->scroll_target, 0, s->scroll_max);
            s->scroll_pending = false;
        }
        s->scroll = CH_CLAMP(s->scroll, 0, s->scroll_max);
        e->scroll = s->scroll;
        e->content_h = content;
        offset = -s->scroll;
    } else
        offset = fmaxf(0, inner_h - used) * align_factor(e->box.align_y);
    float cursor = y + e->box.pad_t + offset;
    for (El *c = e->first; c; c = c->next) {
        float cx = x + e->box.pad_l + fmaxf(0, inner_w - c->w) * align_factor(e->box.align_x);
        position(cu, c, cx, cursor, inner_clip);
        cursor += c->h + e->box.gap;
    }
}
static void layout_root(Cu *cu, El *root, float w, float h, bool fit) {
    measure_w(cu, root);
    float rw = fit ? fminf(root->fit_w, w) : w;
    if (fit && root->box.w.kind == CU_SIZE_FIXED)
        rw = root->box.w.value;
    assign_w(cu, root, rw);
    measure_h(cu, root);
    float rh = fit ? fminf(root->fit_h, h) : h;
    if (fit && root->box.h.kind == CU_SIZE_FIXED)
        rh = fminf(root->box.h.value, h);
    assign_h(cu, root, rh);
}
static void place_layer(Cu *cu, LayerRec *rec) {
    El *r = rec->root;
    CuLayer *l = &rec->layer;
    float margin = 8;
    layout_root(cu, r, cu->width - 2 * margin, cu->height - 2 * margin, true);
    CuRect a = {cu->mouse_x, cu->mouse_y, 0, 0};
    if (l->anchor) {
        State *s = cu_state_find(cu, l->anchor);
        if (s)
            a = s->rect;
    }
    float x = 0, y = 0, w = r->w, h = r->h;
    switch (l->placement) {
    case CU_PLACE_BELOW:
    case CU_PLACE_ABOVE: {
        x = l->align_end ? a.x + a.w - w : a.x;
        bool above = l->placement == CU_PLACE_ABOVE;
        float below_y = a.y + a.h + l->offset, above_y = a.y - l->offset - h;
        if (!above && below_y + h > cu->height - margin && above_y >= margin)
            above = true;
        else if (above && above_y < margin && below_y + h <= cu->height - margin)
            above = false;
        y = above ? above_y : below_y;
        break;
    }
    case CU_PLACE_RIGHT:
    case CU_PLACE_LEFT: {
        float right_x = a.x + a.w + l->offset, left_x = a.x - w - l->offset;
        bool left = l->placement == CU_PLACE_LEFT;
        if (left ? left_x < margin : right_x + w > cu->width - margin)
            left = !left;
        x = left ? left_x : right_x;
        y = l->align_end ? a.y + a.h - h + 5 : a.y - 5;
        break;
    }
    case CU_PLACE_CENTER:
        x = (cu->width - w) / 2;
        y = (cu->height - h) / 2;
        break;
    default:
        x = l->x;
        y = l->y;
        if (x + w > cu->width - margin)
            x = l->x - w;
        if (y + h > cu->height - margin)
            y = l->y - h - 24;
    }
    x = CH_CLAMP(x, margin, fmaxf(margin, cu->width - margin - w));
    y = CH_CLAMP(y, margin, fmaxf(margin, cu->height - margin - h));
    rec->rect = (CuRect){x, y, w, h};
    if (l->kind == CU_LAYER_MODAL && l->scrim) {
        /* The scrim covers the window and blocks everything beneath it. */
        rec->rect = (CuRect){0, 0, cu->width, cu->height};
    }
    position(cu, r, x, y, (CuRect){0, 0, cu->width, cu->height});
}

/* Painting */
static void paint_shadow(Cu *cu, CuCanvas *c, CuRect r, float radius, float size) {
    for (int i = 4; i >= 1; i--) {
        float grow = size * (float)i / 4;
        CuRect s = {r.x - grow, r.y - grow + size * 0.35f, r.w + 2 * grow, r.h + 2 * grow};
        cu->p->fill_rect(c, s, radius + grow, cu_apply_alpha(cu, 0x14000000));
    }
}
static void paint_el(Cu *cu, CuCanvas *c, El *e) {
    CuRect r = {e->x, e->y, e->w, e->h};
    CuRect visible = intersect(r, e->clip);
    if ((visible.w <= 0 || visible.h <= 0) && !e->box.shadow)
        return;
    float saved_alpha = cu->alpha;
    if (e->box.opacity > 0)
        cu->alpha *= e->box.opacity;
    const CuPlatform *p = cu->p;
    if (e->box.shadow)
        paint_shadow(cu, c, r, e->box.radius, e->box.shadow);
    if (e->box.bg >> 24)
        p->fill_rect(c, r, e->box.radius, cu_apply_alpha(cu, e->box.bg));
    if (e->box.border > 0 && (e->box.border_color >> 24)) {
        float b = e->box.border;
        p->stroke_rect(c, (CuRect){r.x + b / 2, r.y + b / 2, r.w - b, r.h - b}, fmaxf(0, e->box.radius - b / 2), b,
                       cu_apply_alpha(cu, e->box.border_color));
    }
    if (e->box.paint) {
        p->save(c);
        e->box.paint(cu, c, r, e->box.paint_data);
        p->restore(c);
    }
    CuColor rule = cu_apply_alpha(cu, e->box.border_color);
    if (e->box.border_b > 0)
        p->fill_rect(c, (CuRect){r.x, r.y + r.h - e->box.border_b, r.w, e->box.border_b}, 0, rule);
    if (e->box.border_t > 0)
        p->fill_rect(c, (CuRect){r.x, r.y, r.w, e->box.border_t}, 0, rule);
    if (e->box.border_r > 0)
        p->fill_rect(c, (CuRect){r.x + r.w - e->box.border_r, r.y, e->box.border_r, r.h}, 0, rule);
    if (e->box.border_l > 0)
        p->fill_rect(c, (CuRect){r.x, r.y, e->box.border_l, r.h}, e->box.border_l / 2, rule);
    switch (e->type) {
    case EL_TEXT:
        if (e->layout) {
            size_t a = 0, b = 0;
            int index = e->sel_index - 1;
            if (index >= 0 && index >= cu->sel_first && index <= cu->sel_last && e->layer == cu->sel_layer &&
                e->scroller == cu->sel_scroller) {
                a = index == cu->sel_first ? cu->sel_start : 0;
                b = index == cu->sel_last ? cu->sel_end : e->layout->length;
            }
            text_layout_paint(cu, c, e->layout, r.x + e->box.pad_l, r.y + e->box.pad_t, a, b, 0x664a76c9);
        }
        break;
    case EL_ICON:
        icon_paint(cu, c, e->icon, r.x, r.y, e->icon_size, cu_apply_alpha(cu, e->color));
        break;
    case EL_CUSTOM:
        if (e->paint) {
            p->save(c);
            e->paint(cu, c, r, e->paint_data);
            p->restore(c);
        }
        break;
    case EL_EDIT:
        edit_paint(cu, c, e);
        break;
    default:
        break;
    }
    if (e->first) {
        bool clipping = e->box.clip || e->box.scroll_y;
        if (clipping) {
            p->save(c);
            p->clip(c, r);
        }
        for (El *child = e->first; child; child = child->next)
            paint_el(cu, c, child);
        if (clipping)
            p->restore(c);
    }
    if (e->box.scroll_y && e->content_h > e->h + 0.5f) {
        State *s = cu_state_find(cu, e->id);
        float track = e->h, thumb = fmaxf(28, track * track / e->content_h);
        float t = s && s->scroll_max > 0 ? s->scroll / s->scroll_max : 0;
        bool active = cu->drag_scrollbar == e->id || cu_hovered_within(cu, e->id);
        if (active)
            p->fill_rect(c, (CuRect){r.x + r.w - 8, r.y + 2 + t * (track - thumb - 4), 5, thumb}, 2.5f,
                         cu_apply_alpha(cu, 0xb0686868));
    }
    cu->alpha = saved_alpha;
}

/* Frame */
static void tooltip_layer(Cu *cu) {
    if (!cu->tip_id || !cu->tip_text || cu->mouse_down || cu->mouse_x < 0)
        return;
    if (cu_now(cu) - cu->tip_since < 550000)
        return;
    CuLayer l = {.kind = CU_LAYER_TOOLTIP, .placement = CU_PLACE_POINT, .x = cu->mouse_x + 4, .y = cu->mouse_y + 22};
    cu_layer_open(cu, cu_id(cu, "#tooltip"), &l,
                  &(CuBox){.w = CU_FIT_MAX(380), CU_PAD2(5, 8), .bg = 0xff303030, .border = 1,
                           .border_color = 0xff444444, .radius = 5, .passthrough = true});
    cu_text(cu, 0, cu->tip_text,
            &(CuTextStyle){.font = cu_font(cu, "Arial, Liberation Sans, sans-serif", 12, 400), .color = 0xffefefef,
                           .wrap = CU_WRAP_WORD, .w = CU_FIT_MAX(364)});
    cu_layer_close(cu);
}
static void build_frame(Cu *cu) {
    arena_reset(cu); /* the last tree is gone, and with it the last uses of retired layouts */
    ch_vec_clear(&cu->retired);
    ch_vec_init(&cu->retired, retired_free);
    ch_vec_clear(&cu->layers);
    ch_vec_init(&cu->layers, NULL);
    cu->depth = 0;
    cu->id_depth = 0;
    cu->next_hit_count = 0;
    cu->focusable_count = 0;
    cu->seen_edit_count = 0;
    cu->frame++;
    El *root = cu_arena(cu, sizeof(El));
    root->type = EL_BOX;
    root->id = cu_id(cu, "#root");
    root->box.w = CU_PX(cu->width);
    root->box.h = CU_PX(cu->height);
    cu->root = root;
    cu->stack[cu->depth++] = root;
    if (cu->build)
        cu->build(cu, cu->build_data);
    cu->depth = 1;
    tooltip_layer(cu);
    /* Keys nothing else wanted act on read-only text, in dialogs as well as the transcript. */
    if (cu_key(cu, 'a', CU_MOD_CTRL))
        select_all(cu);
    if (cu_key(cu, 'c', CU_MOD_CTRL) || cu_key(cu, CU_KEY_INSERT, CU_MOD_CTRL))
        cu_copy(cu, cu_selection_text(cu));
    /* Unconsumed Tab moves keyboard focus among this frame's focusable elements. */
    cu->tab_dir = 0;
    for (int k = 0; k < cu->key_count; k++)
        if (!cu->keys[k].consumed && cu->keys[k].key == CU_KEY_TAB && !(cu->keys[k].mods & ~CU_MOD_SHIFT)) {
            cu->keys[k].consumed = true;
            cu->tab_dir = cu->keys[k].mods & CU_MOD_SHIFT ? -1 : 1;
            break;
        }
}
static void finish_layout(Cu *cu) {
    cu->next_hit_count = 0;
    cu->focusable_count = 0;
    cu->selectable_count = 0;
    layout_root(cu, cu->root, cu->width, cu->height, false);
    position(cu, cu->root, 0, 0, (CuRect){0, 0, cu->width, cu->height});
    cu->prev_layer_count = 0;
    for (size_t i = 0; i < cu->layers.len; i++) {
        LayerRec *rec = cu->layers.data[i];
        place_layer(cu, rec);
        if (cu->prev_layer_count < (int)CH_N_ELEMENTS(cu->prev_layers)) {
            int n = cu->prev_layer_count++;
            cu->prev_layers[n] = rec->id;
            cu->prev_layer_rects[n] = rec->rect;
            cu->prev_layer_kinds[n] = rec->layer.kind;
            cu->prev_layer_anchors[n] = rec->layer.anchor;
        }
    }
    selection_resolve(cu);
    /* Focus does not outlive what held it, such as a field in a dialog that has closed. */
    State *focused = cu->focus ? cu_state_find(cu, cu->focus) : NULL;
    if (focused && cu->frame - focused->frame > 1) {
        cu->focus = 0;
        cu->invalid = true;
    }
    /* Keyboard focus traversal within the topmost modal layer. */
    int floor = 0;
    for (int i = 0; i < cu->prev_layer_count; i++)
        if (cu->prev_layer_kinds[i] == CU_LAYER_MODAL)
            floor = i + 1;
    if (cu->tab_dir) {
        bool back = cu->tab_dir < 0;
        int n = 0, current = -1;
        CuId order[256];
        for (int i = 0; i < cu->focusable_count; i++)
            if (cu->focusable_layer[i] >= floor) {
                if (cu->focusables[i] == cu->focus)
                    current = n;
                order[n++] = cu->focusables[i];
            }
        if (n) {
            int next = current < 0 ? (back ? n - 1 : 0) : (current + (back ? n - 1 : 1)) % n;
            cu->focus = order[next];
            cu->focus_visible = true;
            cu->invalid = true;
        }
        cu->tab_dir = 0;
    }
    /* Swap hit lists: this frame's layout answers the next frame's input. */
    Hit *tmp = cu->hits;
    int tmp_cap = cu->hit_cap;
    cu->hits = cu->next_hits;
    cu->hit_count = cu->next_hit_count;
    cu->hit_cap = cu->next_hit_cap;
    cu->next_hits = tmp;
    cu->next_hit_cap = tmp_cap;
    cu->next_hit_count = 0;
    /* Tooltip strings live in the arena; keep the hits pointing at valid memory until reset. */
}
static bool wake(void *data) {
    Cu *cu = data;
    cu->timer = 0;
    cu->wake_at = 0;
    if (cu->window)
        cu->p->window_redraw(cu->window);
    return false;
}
void cu_paint(Cu *cu, CuCanvas *canvas, int width, int height) {
    cu->width = (float)width;
    cu->height = (float)height;
    cu->now = ch_monotonic_us();
    cu->canvas = canvas;
    cu->painting = true;
    cu->wake_at = 0;
    /* Input acted on while the frame was described can change what was described
     * before it (a chosen item closes its popup), so lay out once more. */
    bool acted = false;
    for (size_t i = 0; i < cu->events.len; i++) {
        CuEventType type = ((CuEvent *)cu->events.data[i])->type;
        acted |= type == CU_EV_PRESS || type == CU_EV_RELEASE || type == CU_EV_KEY || type == CU_EV_TEXT ||
                 type == CU_EV_SCROLL;
    }
    process_input(cu);
    for (int pass = 0; pass < 3; pass++) {
        cu->invalid = pass == 0 && acted;
        build_frame(cu);
        finish_layout(cu);
        /* Tooltips and hover follow the new layout. */
        if (pass == 0) {
            memset(cu->clicked, 0, sizeof(cu->clicked));
            cu->right_clicked = cu->double_clicked = 0;
            clear_keys(cu);
            cu->dismissed_count = 0;
        }
        if (!cu->invalid)
            break;
    }
    cu->alpha = 1;
    cu->p->fill_rect(canvas, (CuRect){0, 0, cu->width, cu->height}, 0, 0xff191919);
    paint_el(cu, canvas, cu->root);
    for (size_t i = 0; i < cu->layers.len; i++) {
        LayerRec *rec = cu->layers.data[i];
        if (rec->layer.kind == CU_LAYER_MODAL && rec->layer.scrim)
            cu->p->fill_rect(canvas, (CuRect){0, 0, cu->width, cu->height}, 0, rec->layer.scrim);
        paint_el(cu, canvas, rec->root);
    }
    cu->painting = false;
    cu->canvas = NULL;
    bool again = cu->invalid;
    update_hover(cu);
    if (cu->frame % 300 == 0)
        state_collect(cu);
    if (again)
        cu->p->window_redraw(cu->window);
    else if (cu->wake_at) {
        if (cu->timer)
            cu->p->timer_remove(cu->timer);
        int64_t delay = (cu->wake_at - ch_monotonic_us()) / 1000;
        cu->timer = cu->p->timer_add((int)CH_MAX(delay, 1), wake, cu);
    }
}

/* Test support: search the last frame, topmost layer first, for text. */
static bool element_text(El *e, char *buf, size_t size) {
    size_t n = 0;
    if (e->type == EL_TEXT)
        for (int i = 0; i < e->span_count && n + 1 < size; i++) {
            size_t k = CH_MIN(e->spans[i].length, size - 1 - n);
            memcpy(buf + n, e->spans[i].text, k);
            n += k;
        }
    else if (e->type == EL_EDIT) {
        const char *s = e->edit && e->edit->text.len ? e->edit->text.str : e->edit_style.placeholder;
        if (s) {
            n = CH_MIN(strlen(s), size - 1);
            memcpy(buf, s, n);
        }
    } else if (e->box.tooltip) {
        n = CH_MIN(strlen(e->box.tooltip), size - 1);
        memcpy(buf, e->box.tooltip, n);
    } else
        return false;
    buf[n] = 0;
    return n > 0;
}
static El *find_in(El *e, const char *text, bool exact) {
    for (El *c = e->last ? e->first : NULL; c; c = c->next) {
        El *hit = find_in(c, text, exact);
        if (hit)
            return hit;
    }
    char buf[1024];
    if (!element_text(e, buf, sizeof(buf)))
        return NULL;
    CuRect visible = intersect((CuRect){e->x, e->y, e->w, e->h}, e->clip);
    if (visible.w < 1 || visible.h < 1)
        return NULL;
    if (exact ? !strcmp(buf, text) : strstr(buf, text) != NULL)
        return e;
    return NULL;
}
static void list_in(El *e, void (*fn)(const char *text, void *data), void *data) {
    for (El *c = e->last ? e->first : NULL; c; c = c->next)
        list_in(c, fn, data);
    char buf[8192];
    CuRect visible = intersect((CuRect){e->x, e->y, e->w, e->h}, e->clip);
    if (visible.w < 1 || visible.h < 1 || !element_text(e, buf, sizeof(buf)))
        return;
    const char *end;
    if (!ch_utf8_validate(buf, -1, &end))
        buf[end - buf] = 0; /* cut at the buffer size, inside a character */
    fn(buf, data);
}
void cu_list_text(Cu *cu, void (*fn)(const char *text, void *data), void *data) {
    if (cu->root)
        list_in(cu->root, fn, data);
    for (size_t i = 0; i < cu->layers.len; i++) {
        LayerRec *rec = cu->layers.data[i];
        if (rec->root && rec->layer.kind != CU_LAYER_TOOLTIP)
            list_in(rec->root, fn, data);
    }
}
bool cu_find_text(Cu *cu, const char *text, CuRect *rect) {
    for (int exact = 1; exact >= 0; exact--) {
        for (int i = (int)cu->layers.len - 1; i >= -1; i--) {
            El *root = i >= 0 ? ((LayerRec *)cu->layers.data[i])->root : cu->root;
            if (!root || (i >= 0 && ((LayerRec *)cu->layers.data[i])->layer.kind == CU_LAYER_TOOLTIP))
                continue;
            El *e = find_in(root, text, exact);
            if (e) {
                *rect = intersect((CuRect){e->x, e->y, e->w, e->h}, e->clip);
                /* Within a paragraph, the words themselves rather than the whole block. */
                const char *found = !exact && e->type == EL_TEXT && e->layout ? strstr(e->layout->text, text) : NULL;
                if (found) {
                    float x, y, h, end_x, end_y, end_h;
                    size_t offset = (size_t)(found - e->layout->text);
                    text_layout_caret(cu, e->layout, offset, &x, &y, &h);
                    text_layout_caret(cu, e->layout, offset + strlen(text), &end_x, &end_y, &end_h);
                    if (end_y == y && end_x > x)
                        *rect = (CuRect){e->x + e->box.pad_l + x, e->y + e->box.pad_t + y, end_x - x, h};
                }
                return true;
            }
        }
    }
    return false;
}
