/* Chela UI: a small immediate-mode interface library in C.
 *
 * The application describes its interface every frame from its own state: boxes
 * with flex layout (rows/columns, fit/grow/fixed sizing, padding, gaps,
 * alignment), text, rich text, icons, text editors and custom painting. Layers
 * float above the tree for menus, dialogs and tooltips. Interaction is queried by
 * element id against the previous frame's layout. Rendering and input go through
 * a CuPlatform backend, so the same interface runs on any toolkit or compositor. */
#ifndef CHELA_CU_H
#define CHELA_CU_H
#include "../core/base.h"
#include "platform.h"

typedef struct Cu Cu;
typedef uint64_t CuId;

enum { CU_START, CU_CENTER, CU_END };
enum { CU_SIZE_FIT, CU_SIZE_GROW, CU_SIZE_FIXED, CU_SIZE_PERCENT };
typedef struct {
    uint8_t kind;
    float value, min, max; /* max 0 = unlimited */
} CuSize;
#define CU_FIT ((CuSize){CU_SIZE_FIT, 0, 0, 0})
#define CU_GROW ((CuSize){CU_SIZE_GROW, 0, 0, 0})
#define CU_PX(v) ((CuSize){CU_SIZE_FIXED, (v), 0, 0})
#define CU_PCT(v) ((CuSize){CU_SIZE_PERCENT, (v), 0, 0})
#define CU_FIT_MAX(m) ((CuSize){CU_SIZE_FIT, 0, 0, (m)})
#define CU_FIT_MIN(m) ((CuSize){CU_SIZE_FIT, 0, (m), 0})
#define CU_GROW_MAX(m) ((CuSize){CU_SIZE_GROW, 0, 0, (m)})
#define CU_GROW_MIN(m) ((CuSize){CU_SIZE_GROW, 0, (m), 0})

typedef struct {
    CuSize w, h;
    float pad_l, pad_t, pad_r, pad_b;
    float gap;
    bool row;                /* children left to right (default top to bottom) */
    uint8_t align_x, align_y; /* child alignment within free space */
    CuColor bg, border_color;
    float radius, border;
    float border_b, border_t, border_r, border_l; /* single-side rules */
    bool clip, scroll_y;
    bool interactive;        /* hit-testable: hover, press, click */
    bool focusable;          /* reachable with Tab; Enter/Space activate */
    bool drag_window;        /* pressing moves the window (client-side title rail) */
    bool passthrough;        /* never hit-testable even inside a layer */
    CuCursor cursor;
    const char *tooltip;
    float opacity;           /* 0 = opaque; otherwise multiplies colours in this subtree */
    float shadow;            /* soft drop shadow radius for floating panels */
    void (*paint)(Cu *cu, CuCanvas *canvas, CuRect rect, void *data); /* custom background, under the children */
    void *paint_data;
} CuBox;
#define CU_PAD(v) .pad_l = (v), .pad_t = (v), .pad_r = (v), .pad_b = (v)
#define CU_PAD2(y, x) .pad_l = (x), .pad_t = (y), .pad_r = (x), .pad_b = (y)

enum { CU_WRAP_NONE, CU_WRAP_WORD, CU_WRAP_CHAR };
enum { CU_ELLIPSIZE_NONE, CU_ELLIPSIZE_END, CU_ELLIPSIZE_MIDDLE };
typedef struct {
    CuFont *font;
    CuColor color;
    uint8_t wrap, ellipsize, align;
    float line_height; /* multiple of the font's height; 0 = 1 */
    CuSize w;          /* default: FIT for single lines, GROW for wrapped text */
    bool selectable;
    int max_lines;     /* wrapped text: 0 unlimited */
    float max_width;   /* convenience cap on the natural width */
} CuTextStyle;

typedef struct {
    const char *text;
    size_t length; /* SIZE_MAX = strlen */
    CuFont *font;
    CuColor color, bg;
    bool underline, strike;
    const char *link; /* clicking the span reports this through cu_link_take */
} CuSpan;
typedef struct {
    float indent, hanging, line_height, space_after;
    bool selectable, preformatted; /* preformatted keeps spaces and breaks long lines anywhere */
    bool tight;                    /* copied with single line breaks, as in the rows of a table */
    uint8_t align;
} CuPara;

/* Text editing state owned by the application. */
typedef struct CuEdit {
    ChBuf text;
    size_t caret, anchor;
    bool multiline, password, initialized;
    float scroll_y, scroll_x;
    ChVec undo;
    int64_t blink_start;
    char *preedit;
    /* Last layout, for pointer hit-testing and vertical caret movement. */
    void *layout;
    uint64_t layout_key;
    CuRect rect;
    float desired_x;
    int64_t last_undo;
    int undo_kind;
    bool reveal_caret; /* the caret moved, so scroll it into view */
} CuEdit;
void cu_edit_init(CuEdit *e, bool multiline);
void cu_edit_clear(CuEdit *e);
void cu_edit_set(CuEdit *e, const char *text);
const char *cu_edit_text(const CuEdit *e);
void cu_edit_insert(CuEdit *e, const char *text); /* at the caret, replacing any selection */
bool cu_edit_empty(const CuEdit *e);
typedef struct {
    CuFont *font;
    CuColor color, placeholder_color, selection;
    const char *placeholder;
    CuSize w;
    float min_h, max_h; /* multi-line: grows with content between these */
    bool submit_on_enter, disabled;
    size_t max_length;
    bool accept_data; /* pasted files and images arrive through cu_payload_take */
} CuEditStyle;
enum { CU_EDIT_CHANGED = 1, CU_EDIT_SUBMIT = 2 };

typedef void (*CuPaintFn)(Cu *cu, CuCanvas *canvas, CuRect rect, void *data);

typedef enum { CU_LAYER_POPUP, CU_LAYER_MODAL, CU_LAYER_TOOLTIP } CuLayerKind;
enum { CU_PLACE_BELOW, CU_PLACE_ABOVE, CU_PLACE_RIGHT, CU_PLACE_LEFT, CU_PLACE_CENTER, CU_PLACE_POINT };
typedef struct {
    CuLayerKind kind;
    CuId anchor;          /* element whose rect places the layer */
    uint8_t placement;
    bool align_end;       /* align right edges (below/above) or bottom edges (right) */
    float x, y;           /* CU_PLACE_POINT: absolute position */
    float offset;         /* gap from the anchor */
    CuColor scrim;        /* modal backdrop */
} CuLayer;

/* Context */
Cu *cu_new(const CuPlatform *platform, CuWindow *window);
void cu_free(Cu *cu);
void cu_set_build(Cu *cu, void (*build)(Cu *cu, void *data), void *data);
const CuPlatform *cu_platform(Cu *cu);
CuWindow *cu_window(Cu *cu);
void cu_handle_event(Cu *cu, const CuEvent *event); /* from CuWindowHandler.event */
void cu_paint(Cu *cu, CuCanvas *canvas, int width, int height); /* from CuWindowHandler.paint */
void cu_invalidate(Cu *cu);                 /* rebuild and repaint soon */
void cu_animate(Cu *cu, int milliseconds);  /* request another frame after a delay */
float cu_width(Cu *cu);
float cu_height(Cu *cu);
int64_t cu_now(Cu *cu); /* monotonic microseconds at frame start */
bool cu_window_active(Cu *cu);
bool cu_close_requested(Cu *cu); /* the window manager asked to close; consumes the request */
bool cu_find_text(Cu *cu, const char *text, CuRect *rect); /* testing: a visible element showing text */
void cu_list_text(Cu *cu, void (*fn)(const char *text, void *data), void *data); /* testing: all visible text */

/* Memory valid until the next frame is built (custom paint data, labels). */
void *cu_frame_alloc(Cu *cu, size_t size);
char *cu_frame_printf(Cu *cu, const char *format, ...) CH_PRINTF(2, 3);

/* Ids */
CuId cu_id(Cu *cu, const char *name);
CuId cu_idn(Cu *cu, const char *name, int64_t n);
CuId cu_ids(Cu *cu, const char *name, const char *suffix);
void cu_push_id(Cu *cu, CuId id);
void cu_pop_id(Cu *cu);

/* Tree */
void cu_open(Cu *cu, CuId id, const CuBox *box);
void cu_close(Cu *cu);
void cu_spacer(Cu *cu, CuSize w, CuSize h);
void cu_text(Cu *cu, CuId id, const char *text, const CuTextStyle *style);
void cu_textf(Cu *cu, const CuTextStyle *style, const char *format, ...) CH_PRINTF(3, 4);
void cu_rich(Cu *cu, CuId id, const CuSpan *spans, int count, const CuPara *para);
void cu_icon(Cu *cu, const char *name, float size, CuColor color);
void cu_custom(Cu *cu, CuId id, CuSize w, CuSize h, CuPaintFn paint, void *data);
int cu_edit(Cu *cu, CuId id, CuEdit *edit, const CuEditStyle *style);

/* Layers (menus, dialogs, tooltips) are built after or inside the main tree. */
void cu_layer_open(Cu *cu, CuId id, const CuLayer *layer, const CuBox *box);
void cu_layer_close(Cu *cu);
bool cu_layer_dismissed(Cu *cu, CuId layer); /* pointer pressed outside it (or Escape for popups) */
bool cu_modal_active(Cu *cu);                /* a modal layer was open last frame */

/* Interaction, resolved against the previous frame's layout. */
bool cu_hovered(Cu *cu, CuId id);
bool cu_hovered_within(Cu *cu, CuId id); /* the pointer is anywhere over the element */
bool cu_pressed(Cu *cu, CuId id);
bool cu_clicked(Cu *cu, CuId id);
bool cu_right_clicked(Cu *cu, CuId id);
bool cu_double_clicked(Cu *cu, CuId id);
CuRect cu_rect(Cu *cu, CuId id);
bool cu_exists(Cu *cu, CuId id);
bool cu_focused(Cu *cu, CuId id);
bool cu_focus_visible(Cu *cu, CuId id); /* focused by keyboard: draw a focus ring */
void cu_focus(Cu *cu, CuId id);
CuId cu_focus_id(Cu *cu);
bool cu_key(Cu *cu, int key, int mods); /* consumes a matching key press this frame */
bool cu_key_any(Cu *cu, int key);       /* consumes the key with any modifiers */
bool cu_typed(Cu *cu, int ch);          /* consumes this ASCII character being typed, in either case */
int cu_mods(Cu *cu);                    /* modifiers held at the last input */
float cu_mouse_x(Cu *cu);
float cu_mouse_y(Cu *cu);
void cu_scroll_to(Cu *cu, CuId id, float y); /* use INFINITY for the end */
void cu_scroll_by(Cu *cu, CuId id, float dy); /* as if the user scrolled it */
float cu_scroll_y(Cu *cu, CuId id);
float cu_scroll_target(Cu *cu, CuId id); /* where it is heading: a scroll not yet laid out counts */
float cu_scroll_max(Cu *cu, CuId id);
bool cu_scrolled_by_user(Cu *cu, CuId id); /* the user scrolled this element this frame */
void cu_copy(Cu *cu, const char *text);
char *cu_link_take(Cu *cu); /* a link that was clicked, once; the caller frees it */
/* Files and images pasted or dropped onto the window, oldest first. */
typedef struct {
    char *mime, *data;
    size_t size;
    bool dropped;
} CuPayload;
bool cu_payload_take(Cu *cu, CuPayload *payload);
void cu_payload_clear(CuPayload *payload);
bool cu_drag_active(Cu *cu); /* something droppable is over the window */
/* Selectable text is selected by dragging across as many elements as it takes, by double
 * or triple click, or with Ctrl+A; Ctrl+C copies it. Both work in dialogs too. */
const char *cu_selection_text(Cu *cu); /* the selected text, or NULL */
void cu_clear_selection(Cu *cu);
bool cu_selecting(Cu *cu); /* a selection is being dragged */

/* Fonts and text measurement. */
CuFont *cu_font(Cu *cu, const char *family, float size, int weight);
CuFont *cu_font_desc(Cu *cu, const CuFontDesc *desc);
float cu_text_width(Cu *cu, CuFont *font, const char *text, size_t length);
CuFontMetrics cu_font_metrics(Cu *cu, CuFont *font);

/* Canvas helpers for custom painting. */
void cu_draw_icon(Cu *cu, CuCanvas *c, const char *name, float x, float y, float size, CuColor color);
CuColor cu_mix(CuColor a, CuColor b, float t);
CuColor cu_alpha(CuColor c, float alpha);
#endif
