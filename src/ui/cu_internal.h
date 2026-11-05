/* Private structures shared by the UI library's modules. */
#ifndef CHELA_CU_INTERNAL_H
#define CHELA_CU_INTERNAL_H
#include "cu.h"

typedef struct {
    int span;
    size_t start, end; /* byte range in TextLayout.text */
    float x, w;
} TextRun;
typedef struct {
    size_t start, end; /* bytes, excluding the line break */
    float x, y, w, h, ascent;
    int first_run, run_count;
} TextLine;
typedef struct {
    char *text; /* all spans concatenated */
    size_t length;
    CuSpan *spans; /* text pointers rebased into text */
    size_t *span_start;
    int span_count;
    TextLine *lines;
    int line_count;
    TextRun *runs;
    int run_count;
    float w, h, natural_w;
    uint64_t key;
    float width_key;
} TextLayout;
typedef struct {
    float indent, hanging, line_height;
    uint8_t wrap, ellipsize, align;
    bool preformatted, password;
    int max_lines;
} TextOptions;

TextLayout *text_layout_new(Cu *cu, const CuSpan *spans, int count, const TextOptions *options, float max_width);
void text_layout_free(TextLayout *layout);
float text_natural_width(Cu *cu, const CuSpan *spans, int count, const TextOptions *options);
size_t text_layout_hit(Cu *cu, const TextLayout *layout, float x, float y);
const char *text_layout_link(Cu *cu, const TextLayout *layout, float x, float y);
void text_layout_caret(Cu *cu, const TextLayout *layout, size_t offset, float *x, float *y, float *h);
void text_layout_paint(Cu *cu, CuCanvas *c, const TextLayout *layout, float x, float y, size_t sel_a, size_t sel_b,
                       CuColor selection);
size_t text_word_start(const char *text, size_t offset);
size_t text_word_end(const char *text, size_t length, size_t offset);
uint64_t text_hash(uint64_t h, const void *data, size_t n);

enum { EL_BOX, EL_TEXT, EL_ICON, EL_CUSTOM, EL_EDIT, EL_SPACER };
typedef struct El El;
struct El {
    CuId id;
    int type;
    CuBox box;
    El *parent, *first, *last, *next;
    int child_count;
    /* content */
    CuSpan *spans;
    int span_count;
    TextOptions text;
    CuTextStyle text_style;
    bool selectable, tight;
    const char *icon;
    float icon_size;
    CuColor color;
    CuPaintFn paint;
    void *paint_data;
    CuEdit *edit;
    CuEditStyle edit_style;
    /* layout */
    float fit_w, min_w, fit_h, min_h, w, h, x, y;
    float content_h, scroll;
    CuRect clip;
    TextLayout *layout;
    int layer;
    bool has_grow_w, has_grow_h;
    int sel_index; /* 1 + position among this frame's selectable text; 0 when not selectable */
    CuId scroller; /* the scrolling box this selectable text sits in */
};
typedef struct {
    CuId id;
    CuLayer layer;
    El *root;
    CuRect rect;
} LayerRec;
typedef struct {
    CuId id;
    CuRect rect;
    int layer;
    bool interactive, scroll, selectable, edit, drag_window, focusable;
    CuCursor cursor;
    const char *tooltip;
} Hit;
/* Selectable text in reading order. A selection stays within one group: the same
 * layer and the same scrolling box. */
typedef struct {
    CuId id, scroller;
    int layer;
    bool tight;
} Selectable;
typedef struct {
    CuId id;
    int64_t frame;
    CuRect rect;
    float scroll, scroll_max, scroll_target;
    bool scroll_pending;
    TextLayout *layout;
    void *edit;
} State;

struct Cu {
    const CuPlatform *p;
    CuWindow *window;
    void (*build)(Cu *cu, void *data);
    void *build_data;
    float width, height;
    int64_t frame, now;
    /* arena */
    ChVec chunks;
    char *chunk;
    size_t chunk_used, chunk_size;
    /* tree */
    El *root;
    El *stack[256];
    int depth;
    CuId id_stack[64];
    int id_depth;
    ChVec layers; /* LayerRec*, this frame */
    CuId prev_layers[32];
    CuRect prev_layer_rects[32];
    CuLayerKind prev_layer_kinds[32];
    CuId prev_layer_anchors[32];
    int prev_layer_count;
    /* hits from the previous frame */
    Hit *hits, *next_hits;
    int hit_count, hit_cap, next_hit_count, next_hit_cap;
    /* state */
    State **states;
    size_t state_cap, state_count;
    /* input */
    ChVec events; /* CuEvent* with copied text */
    float mouse_x, mouse_y;
    bool mouse_down, window_active, close_requested;
    int mods;
    CuId hot, pressed, clicked[8], right_clicked, double_clicked, last_click;
    int click_count;
    int64_t last_click_time;
    float last_click_x, last_click_y;
    CuId focus, focus_next;
    bool focus_visible;
    int tab_dir;
    struct {
        int key, mods; /* key -1: text input in text */
        bool consumed;
        char *text;
    } keys[64];
    int key_count;
    ChBuf typed;
    char *preedit;
    CuId dismissed[8];
    int dismissed_count;
    CuId scrolled[8];
    int scrolled_count;
    /* drags */
    CuId drag_edit, drag_scrollbar;
    bool drag_sel;
    float drag_offset;
    /* selection in read-only text, which may span many elements */
    CuId sel_anchor_id, sel_caret_id, sel_scroller;
    int sel_layer;
    size_t sel_anchor, sel_caret;
    int sel_first, sel_last;       /* positions in selectables, -1 when nothing is selected */
    size_t sel_start, sel_end;     /* offsets in those two elements */
    bool sel_focus;                /* the last press was on read-only text, so Ctrl+A and Ctrl+C mean it */
    Selectable *selectables;
    int selectable_count, selectable_cap;
    char *sel_text;
    /* tooltip */
    CuId tip_id, tip_suppressed;
    int64_t tip_since;
    char *tip_text;
    /* paste and drop */
    CuEdit *paste_edit;
    CuId paste_focus;
    ChVec payloads;
    bool drag_over;
    /* links */
    char *link_clicked;
    CuId link_press;
    /* focus order this frame */
    CuId focusables[256];
    int focusable_layer[256];
    int focusable_count;
    CuEdit *seen_edits[64];
    int seen_edit_count;
    /* scheduling */
    bool invalid, painting;
    int64_t wake_at;
    unsigned timer;
    CuCursor cursor;
    /* Layouts replaced since the frame began. Elements may still point at them, so
     * they outlive the frame's tree, even if two elements ever share an id. */
    ChVec retired;
    /* caches */
    struct FontEntry {
        CuFontDesc desc;
        char family[160];
        CuFont *font;
        CuFontMetrics metrics;
    } fonts[64];
    int font_count;
    struct WidthEntry {
        uint64_t key;
        CuFont *font;
        float width;
    } *widths;
    size_t width_count;
    CuCanvas *canvas;
    float alpha;
};
void cu_payload_push(Cu *cu, const char *mime, const void *data, size_t size, bool dropped);
void *cu_arena(Cu *cu, size_t size);
char *cu_arena_strndup(Cu *cu, const char *s, size_t n);
State *cu_state(Cu *cu, CuId id);
State *cu_state_find(Cu *cu, CuId id);
CuColor cu_apply_alpha(Cu *cu, CuColor c);
bool cu_consume_key(Cu *cu, int key, int mods, bool any_mods);
void edit_handle_input(Cu *cu, El *el, int *flags);
void edit_measure(Cu *cu, El *el);
void edit_paint(Cu *cu, CuCanvas *c, El *el);
void edit_pointer(Cu *cu, CuEdit *e, CuRect rect, float x, float y, bool extend, int clicks);
void icon_paint(Cu *cu, CuCanvas *c, const char *name, float x, float y, float size, CuColor color);
#endif
