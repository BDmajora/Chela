/* The contract between Chela's toolkit-agnostic UI and a platform backend.
 *
 * Chela draws its own interface. A backend only provides a window, a 2D canvas,
 * text measurement and drawing, input events, the clipboard, timers and a
 * thread-safe wakeup. Backends: native Wayland (wayland.c), headless offscreen
 * rendering for tests (headless.c), and optional Qt 6 (qt.cpp) and GTK 3 (gtk.c)
 * hosts loaded as plugins. A Cocoa or WinUI 3 host implements the same table.
 * All coordinates are logical pixels; the backend applies the output scale. */
#ifndef CHELA_PLATFORM_H
#define CHELA_PLATFORM_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CU_PLATFORM_ABI 2

typedef uint32_t CuColor; /* 0xAARRGGBB */
typedef struct {
    float x, y, w, h;
} CuRect;
typedef struct CuWindow CuWindow;   /* backend window */
typedef struct CuCanvas CuCanvas;   /* backend paint context, valid during paint */
typedef struct CuFont CuFont;       /* backend font, owned and cached by the backend */
typedef struct CuImage CuImage;     /* backend bitmap */
typedef struct {
    const char *family; /* comma-separated preference list, e.g. "Arial, Liberation Sans, sans-serif" */
    float size;         /* logical pixels */
    int weight;         /* 400 regular, 600 semibold, 700 bold */
    bool italic, monospace;
} CuFontDesc;
typedef struct {
    float ascent, descent, height; /* logical pixels; height is the recommended line advance */
} CuFontMetrics;

typedef enum {
    CU_CURSOR_DEFAULT,
    CU_CURSOR_POINTER,
    CU_CURSOR_TEXT,
    CU_CURSOR_MOVE,
    CU_CURSOR_RESIZE_N,
    CU_CURSOR_RESIZE_S,
    CU_CURSOR_RESIZE_E,
    CU_CURSOR_RESIZE_W,
    CU_CURSOR_RESIZE_NE,
    CU_CURSOR_RESIZE_NW,
    CU_CURSOR_RESIZE_SE,
    CU_CURSOR_RESIZE_SW,
} CuCursor;
typedef enum { CU_EDGE_NONE, CU_EDGE_N, CU_EDGE_S, CU_EDGE_E, CU_EDGE_W, CU_EDGE_NE, CU_EDGE_NW, CU_EDGE_SE, CU_EDGE_SW } CuEdge;

/* Keys are Unicode code points for printable keys (letters lower-case) or these values. */
enum {
    CU_KEY_RETURN = 0x110000,
    CU_KEY_ESCAPE,
    CU_KEY_TAB,
    CU_KEY_BACKSPACE,
    CU_KEY_DELETE,
    CU_KEY_LEFT,
    CU_KEY_RIGHT,
    CU_KEY_UP,
    CU_KEY_DOWN,
    CU_KEY_HOME,
    CU_KEY_END,
    CU_KEY_PAGE_UP,
    CU_KEY_PAGE_DOWN,
    CU_KEY_INSERT,
    CU_KEY_F1,
    CU_KEY_F10 = CU_KEY_F1 + 9,
    CU_KEY_F12 = CU_KEY_F1 + 11,
    CU_KEY_MENU,
};
enum { CU_MOD_SHIFT = 1, CU_MOD_CTRL = 2, CU_MOD_ALT = 4, CU_MOD_SUPER = 8 };
typedef enum {
    CU_EV_MOTION,
    CU_EV_PRESS,
    CU_EV_RELEASE,
    CU_EV_SCROLL,
    CU_EV_KEY,
    CU_EV_TEXT,
    CU_EV_PREEDIT,
    CU_EV_LEAVE,
    CU_EV_FOCUS,
    CU_EV_RESIZE,
    CU_EV_CLOSE,
    CU_EV_DRAG, /* files are dragged over the window (focused), or left it (!focused) */
    CU_EV_DROP, /* mime, data and size: what was dropped */
} CuEventType;
typedef struct {
    CuEventType type;
    float x, y;         /* pointer position for pointer events */
    float dx, dy;       /* scroll deltas in logical pixels (positive = down/right) */
    int button;         /* 1 left, 2 middle, 3 right */
    int key, mods;      /* key events; mods also set for pointer events */
    bool repeat, focused;
    int width, height;  /* resize */
    float scale;        /* resize */
    const char *text;   /* text input and preedit (UTF-8), valid during the call */
    const char *mime;   /* drop */
    const void *data;   /* drop, valid during the call */
    size_t size;
} CuEvent;

typedef struct {
    /* Called by the backend; data is the pointer given to window_new. */
    void (*paint)(CuWindow *window, CuCanvas *canvas, int width, int height, void *data);
    void (*event)(CuWindow *window, const CuEvent *event, void *data);
} CuWindowHandler;

typedef struct CuPlatform {
    int abi;
    const char *name;
    bool (*init)(int *argc, char ***argv);
    void (*run)(void);
    void (*quit)(void);
    void (*shutdown)(void);

    /* Windows. client_decorations asks for Chela-drawn title rail; the backend reports
     * through has_client_decorations whether the compositor agreed. The handler is
     * not called before window_new returns: the caller is still setting up. */
    CuWindow *(*window_new)(const char *title, const char *app_id, int width, int height, bool client_decorations,
                            const CuWindowHandler *handler, void *data);
    void (*window_free)(CuWindow *window);
    void (*window_set_title)(CuWindow *window, const char *title);
    void (*window_set_min_size)(CuWindow *window, int width, int height);
    void (*window_redraw)(CuWindow *window);
    void (*window_set_cursor)(CuWindow *window, CuCursor cursor);
    void (*window_minimize)(CuWindow *window);
    void (*window_toggle_maximize)(CuWindow *window);
    bool (*window_is_maximized)(CuWindow *window);
    void (*window_begin_move)(CuWindow *window);
    void (*window_begin_resize)(CuWindow *window, CuEdge edge);
    bool (*window_has_client_decorations)(CuWindow *window);
    void (*window_set_text_input)(CuWindow *window, bool enabled, CuRect caret); /* input method position */

    /* Main loop integration. post may be called from any thread. */
    void (*post)(void (*fn)(void *data), void *data);
    unsigned (*timer_add)(int milliseconds, bool (*fn)(void *data), void *data); /* repeats while fn returns true */
    void (*timer_remove)(unsigned id);

    /* Clipboard. request delivers NULL when no text is available. request_data delivers
     * the most useful form on offer, which may be files or an image (see
     * ch_clipboard_best); mime is NULL when the clipboard is empty. Optional. */
    void (*clipboard_set)(CuWindow *window, const char *text);
    void (*clipboard_request)(CuWindow *window, void (*callback)(const char *text, void *data), void *data);
    void (*clipboard_request_data)(CuWindow *window,
                                   void (*callback)(const char *mime, const void *bytes, size_t size, void *data),
                                   void *data);

    /* Text. Fonts are cached by the backend for the life of the process. */
    CuFont *(*font_get)(const CuFontDesc *desc);
    CuFontMetrics (*font_metrics)(CuFont *font);
    float (*text_width)(CuFont *font, const char *text, size_t length);

    /* Canvas. Paths use the current transform; strokes are round-capped. */
    void (*save)(CuCanvas *c);
    void (*restore)(CuCanvas *c);
    void (*clip)(CuCanvas *c, CuRect r);
    void (*translate)(CuCanvas *c, float x, float y);
    void (*scale)(CuCanvas *c, float sx, float sy);
    void (*fill_rect)(CuCanvas *c, CuRect r, float radius, CuColor color);
    void (*stroke_rect)(CuCanvas *c, CuRect r, float radius, float width, CuColor color);
    void (*move_to)(CuCanvas *c, float x, float y);
    void (*line_to)(CuCanvas *c, float x, float y);
    void (*curve_to)(CuCanvas *c, float x1, float y1, float x2, float y2, float x3, float y3);
    void (*close_path)(CuCanvas *c);
    void (*fill_path)(CuCanvas *c, CuColor color);
    void (*stroke_path)(CuCanvas *c, float width, CuColor color);
    void (*draw_text)(CuCanvas *c, CuFont *font, float x, float baseline, const char *text, size_t length,
                      CuColor color);
    /* Bitmaps of premultiplied ARGB32 pixels; the backend keeps its own copy. */
    CuImage *(*image_new)(const uint32_t *pixels, int width, int height);
    void (*image_free)(CuImage *image);
    void (*draw_image)(CuCanvas *c, CuImage *image, CuRect r, float radius);

    /* Optional native file dialogs; NULL uses Chela's own file browser. The callback
     * receives NULL on cancel. kind: 0 open file, 1 choose folder, 2 save file. */
    void (*file_dialog)(CuWindow *parent, int kind, const char *title, const char *folder, const char *name,
                        void (*callback)(const char *path, void *data), void *data);
    /* Optional headless hook: write the last frame to a PNG. */
    bool (*window_snapshot)(CuWindow *window, const char *path);
} CuPlatform;

/* Built-in backends and the plugin entry point. */
const CuPlatform *cu_platform_wayland(void);
const CuPlatform *cu_platform_headless(void);
typedef const CuPlatform *(*CuPlatformEntry)(void);
#define CU_PLATFORM_ENTRY "chela_platform"
#endif
