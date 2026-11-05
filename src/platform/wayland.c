/* Native Wayland backend: xdg-shell window, shared-memory buffers painted with
 * cairo, xkbcommon keyboard input, clipboard, cursors, fractional scaling and
 * input methods. No toolkit and no GLib. */
#define _GNU_SOURCE
#include "cairo_canvas.h"
#include "../core/base.h"
#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

typedef struct {
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    int width, height;
    bool busy;
    cairo_surface_t *surface;
} Buffer;
struct CuWindow {
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct zxdg_toplevel_decoration_v1 *decoration;
    struct wp_fractional_scale_v1 *fractional;
    struct wp_viewport *viewport;
    struct wl_callback *frame;
    const CuWindowHandler *handler;
    void *data;
    int width, height, pending_width, pending_height;
    double scale; /* fractional when the compositor supports it */
    int buffer_scale;
    bool configured, dirty, maximized, pending_maximized, csd, want_csd, closed, activated;
    bool starting; /* inside window_new: the caller has not finished setting up, so the handler is not called */
    Buffer buffers[2];
};
typedef struct {
    unsigned id;
    int64_t due;
    int interval;
    bool (*fn)(void *);
    void *data;
} Timer;
typedef struct Posted {
    void (*fn)(void *);
    void *data;
    struct Posted *next;
} Posted;
#define PASTE_LIMIT (64u * 1024u * 1024u)
typedef struct {
    struct wl_data_offer *offer;
    char *mimes[32];
    int count;
} Offer;
/* One transfer in progress: a paste of text, a paste of any data, or a drop. */
typedef struct {
    int fd;
    ChBuf data;
    char *mime;
    bool overflow;
    void (*text_callback)(const char *, void *);
    void (*data_callback)(const char *, const void *, size_t, void *);
    void *user;
    Offer drop;
} Paste;
static struct {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    uint32_t compositor_version;
    struct wl_shm *shm;
    struct xdg_wm_base *wm_base;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_keyboard *keyboard;
    struct wl_data_device_manager *data_manager;
    struct wl_data_device *data_device;
    Offer selection, pending, drag;
    uint32_t drag_serial;
    struct wl_surface *drag_surface;
    float drag_x, drag_y;
    struct wl_data_source *source;
    char *source_text;
    struct zxdg_decoration_manager_v1 *decorations;
    struct wp_fractional_scale_manager_v1 *fractional;
    struct wp_viewporter *viewporter;
    struct wp_cursor_shape_manager_v1 *cursor_shapes;
    struct wp_cursor_shape_device_v1 *cursor_device;
    struct zwp_text_input_manager_v3 *text_inputs;
    struct zwp_text_input_v3 *text_input;
    bool text_input_enabled;
    CuRect text_cursor;
    char *ime_commit, *ime_preedit;
    struct wl_output *outputs[8];
    int output_scales[8], output_count;
    struct wl_cursor_theme *cursor_theme;
    struct wl_surface *cursor_surface;
    int cursor_theme_scale;
    CuCursor cursor;
    uint32_t pointer_serial, input_serial, enter_serial;
    CuWindow *pointer_window, *keyboard_window;
    float pointer_x, pointer_y;
    double scroll_dx, scroll_dy;
    int scroll_120_x, scroll_120_y;
    bool scroll_wheel;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_state *compose;
    int mods;
    int repeat_rate, repeat_delay;
    uint32_t repeat_key;
    int64_t repeat_next;
    CuWindow *windows[8];
    int window_count;
    Timer timers[128];
    int timer_count;
    unsigned next_timer;
    int wake_fd;
    ChMutex lock;
    Posted *posted, *posted_tail;
    Paste pastes[4];
    int paste_count;
    bool quit;
} wl;

static void redraw(CuWindow *w);
static void post(void (*fn)(void *), void *data);
static void send(CuWindow *w, CuEvent ev) {
    if (w && w->handler && !w->closed && !w->starting)
        w->handler->event(w, &ev, w->data);
}
static CuWindow *window_for(struct wl_surface *surface) {
    for (int i = 0; i < wl.window_count; i++)
        if (wl.windows[i]->surface == surface)
            return wl.windows[i];
    return NULL;
}

/* Shared-memory buffers */
static void buffer_release(void *data, struct wl_buffer *buffer) {
    (void)buffer;
    ((Buffer *)data)->busy = false;
}
static const struct wl_buffer_listener buffer_listener = {buffer_release};
static void buffer_destroy(Buffer *b) {
    if (b->surface)
        cairo_surface_destroy(b->surface);
    if (b->buffer)
        wl_buffer_destroy(b->buffer);
    if (b->data)
        munmap(b->data, b->size);
    memset(b, 0, sizeof(*b));
}
static bool buffer_create(Buffer *b, int width, int height) {
    int stride = width * 4;
    size_t size = (size_t)stride * (size_t)height;
    int fd = memfd_create("chela-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
        if (fd >= 0)
            close(fd);
        return false;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return false;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(wl.shm, fd, (int32_t)size);
    b->buffer = wl_shm_pool_create_buffer(pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->buffer, &buffer_listener, b);
    b->data = data;
    b->size = size;
    b->width = width;
    b->height = height;
    b->surface = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, width, height, stride);
    return true;
}
static Buffer *next_buffer(CuWindow *w, int width, int height) {
    for (int i = 0; i < 2; i++) {
        Buffer *b = &w->buffers[i];
        if (b->busy)
            continue;
        if (b->buffer && (b->width != width || b->height != height))
            buffer_destroy(b);
        if (!b->buffer && !buffer_create(b, width, height))
            return NULL;
        return b;
    }
    return NULL;
}

/* Painting */
static void frame_done(void *data, struct wl_callback *callback, uint32_t time) {
    (void)time;
    CuWindow *w = data;
    wl_callback_destroy(callback);
    w->frame = NULL;
    if (w->dirty)
        redraw(w);
}
static const struct wl_callback_listener frame_listener = {frame_done};
static void paint(CuWindow *w) {
    if (w->starting)
        return; /* stays dirty; the main loop paints it */
    double scale = w->viewport ? w->scale : w->buffer_scale;
    int bw = (int)lround(w->width * scale), bh = (int)lround(w->height * scale);
    Buffer *b = next_buffer(w, bw, bh);
    if (!b)
        return; /* both buffers are on screen; the release will trigger another frame */
    w->dirty = false;
    cairo_t *cr = cairo_create(b->surface);
    cairo_scale(cr, scale, scale);
    CuCanvas canvas = {cr};
    w->handler->paint(w, &canvas, w->width, w->height, w->data);
    cairo_destroy(cr);
    cairo_surface_flush(b->surface);
    wl_surface_attach(w->surface, b->buffer, 0, 0);
    if (w->viewport) {
        wl_surface_set_buffer_scale(w->surface, 1);
        wp_viewport_set_destination(w->viewport, w->width, w->height);
    } else
        wl_surface_set_buffer_scale(w->surface, w->buffer_scale);
    if (wl.compositor_version >= 4)
        wl_surface_damage_buffer(w->surface, 0, 0, bw, bh);
    else
        wl_surface_damage(w->surface, 0, 0, w->width, w->height);
    w->frame = wl_surface_frame(w->surface);
    wl_callback_add_listener(w->frame, &frame_listener, w);
    b->busy = true;
    wl_surface_commit(w->surface);
}
static void redraw(CuWindow *w) {
    w->dirty = true;
    if (w->configured && !w->frame && !w->closed)
        paint(w);
}

/* xdg-shell */
static void wm_ping(void *data, struct xdg_wm_base *base, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(base, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void surface_configure(void *data, struct xdg_surface *surface, uint32_t serial) {
    CuWindow *w = data;
    xdg_surface_ack_configure(surface, serial);
    int width = w->pending_width > 0 ? w->pending_width : w->width;
    int height = w->pending_height > 0 ? w->pending_height : w->height;
    bool changed = !w->configured || width != w->width || height != w->height || w->maximized != w->pending_maximized;
    w->width = width;
    w->height = height;
    w->maximized = w->pending_maximized;
    w->configured = true;
    if (changed)
        send(w, (CuEvent){.type = CU_EV_RESIZE, .width = width, .height = height, .scale = (float)w->scale});
    w->dirty = true;
    if (!w->frame)
        paint(w);
}
static const struct xdg_surface_listener surface_listener = {surface_configure};
static void toplevel_configure(void *data, struct xdg_toplevel *toplevel, int32_t width, int32_t height,
                               struct wl_array *states) {
    (void)toplevel;
    CuWindow *w = data;
    w->pending_width = width;
    w->pending_height = height;
    w->pending_maximized = false;
    bool activated = false;
    uint32_t *state;
    wl_array_for_each(state, states) {
        if (*state == XDG_TOPLEVEL_STATE_MAXIMIZED || *state == XDG_TOPLEVEL_STATE_FULLSCREEN)
            w->pending_maximized = true;
        if (*state == XDG_TOPLEVEL_STATE_ACTIVATED)
            activated = true;
    }
    if (activated != w->activated) {
        w->activated = activated;
        send(w, (CuEvent){.type = CU_EV_FOCUS, .focused = activated});
    }
}
static void toplevel_close(void *data, struct xdg_toplevel *toplevel) {
    (void)toplevel;
    send(data, (CuEvent){.type = CU_EV_CLOSE});
}
static void toplevel_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h) {
    (void)data, (void)t, (void)w, (void)h;
}
static void toplevel_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *c) {
    (void)data, (void)t, (void)c;
}
static const struct xdg_toplevel_listener toplevel_listener = {toplevel_configure, toplevel_close, toplevel_bounds,
                                                               toplevel_capabilities};
static void decoration_configure(void *data, struct zxdg_toplevel_decoration_v1 *d, uint32_t mode) {
    (void)d;
    CuWindow *w = data;
    w->csd = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE;
    w->dirty = true;
}
static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {decoration_configure};

/* Output scale */
static int integer_scale(CuWindow *w) {
    (void)w;
    int scale = 1;
    for (int i = 0; i < wl.output_count; i++)
        scale = CH_MAX(scale, wl.output_scales[i]);
    return scale;
}
static void fractional_preferred(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale) {
    (void)f;
    CuWindow *w = data;
    double s = scale / 120.0;
    if (fabs(s - w->scale) > 0.001) {
        w->scale = s;
        send(w, (CuEvent){.type = CU_EV_RESIZE, .width = w->width, .height = w->height, .scale = (float)s});
        redraw(w);
    }
}
static const struct wp_fractional_scale_v1_listener fractional_listener = {fractional_preferred};
static void surface_enter(void *data, struct wl_surface *s, struct wl_output *o) {
    (void)s, (void)o;
    CuWindow *w = data;
    if (!w->viewport) {
        w->buffer_scale = integer_scale(w);
        w->scale = w->buffer_scale;
        redraw(w);
    }
}
static void surface_leave(void *data, struct wl_surface *s, struct wl_output *o) {
    (void)data, (void)s, (void)o;
}
static void surface_preferred_scale(void *data, struct wl_surface *s, int32_t factor) {
    (void)s;
    CuWindow *w = data;
    if (!w->viewport && factor != w->buffer_scale) {
        w->buffer_scale = factor;
        w->scale = factor;
        redraw(w);
    }
}
static void surface_preferred_transform(void *data, struct wl_surface *s, uint32_t t) {
    (void)data, (void)s, (void)t;
}
static const struct wl_surface_listener wl_surface_listener = {surface_enter, surface_leave, surface_preferred_scale,
                                                               surface_preferred_transform};
static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph, int32_t sp,
                            const char *make, const char *model, int32_t t) {
    (void)d, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)sp, (void)make, (void)model, (void)t;
}
static void output_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {
    (void)d, (void)o, (void)f, (void)w, (void)h, (void)r;
}
static void output_done(void *d, struct wl_output *o) {
    (void)d, (void)o;
}
static void output_scale(void *d, struct wl_output *o, int32_t factor) {
    (void)d;
    for (int i = 0; i < wl.output_count; i++)
        if (wl.outputs[i] == o)
            wl.output_scales[i] = factor;
}
static void output_name(void *d, struct wl_output *o, const char *n) {
    (void)d, (void)o, (void)n;
}
static void output_description(void *d, struct wl_output *o, const char *n) {
    (void)d, (void)o, (void)n;
}
static const struct wl_output_listener output_listener = {output_geometry, output_mode, output_done, output_scale,
                                                          output_name, output_description};

/* Cursors */
static const char *cursor_names[][2] = {
    {"default", "left_ptr"}, {"pointer", "hand2"},       {"text", "xterm"},          {"move", "fleur"},
    {"n-resize", "top_side"}, {"s-resize", "bottom_side"}, {"e-resize", "right_side"}, {"w-resize", "left_side"},
    {"ne-resize", "top_right_corner"}, {"nw-resize", "top_left_corner"}, {"se-resize", "bottom_right_corner"},
    {"sw-resize", "bottom_left_corner"}};
static const uint32_t cursor_shapes[] = {
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_MOVE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
    WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE};
static void apply_cursor(void) {
    if (!wl.pointer || !wl.enter_serial)
        return;
    if (wl.cursor_device) {
        wp_cursor_shape_device_v1_set_shape(wl.cursor_device, wl.enter_serial, cursor_shapes[wl.cursor]);
        return;
    }
    int scale = wl.pointer_window ? wl.pointer_window->buffer_scale : 1;
    if (!wl.cursor_theme || wl.cursor_theme_scale != scale) {
        if (wl.cursor_theme)
            wl_cursor_theme_destroy(wl.cursor_theme);
        const char *size = ch_getenv("XCURSOR_SIZE");
        int px = size ? atoi(size) : 24;
        wl.cursor_theme = wl_cursor_theme_load(ch_getenv("XCURSOR_THEME"), (px > 0 ? px : 24) * scale, wl.shm);
        wl.cursor_theme_scale = scale;
    }
    if (!wl.cursor_theme)
        return;
    struct wl_cursor *c = wl_cursor_theme_get_cursor(wl.cursor_theme, cursor_names[wl.cursor][0]);
    if (!c)
        c = wl_cursor_theme_get_cursor(wl.cursor_theme, cursor_names[wl.cursor][1]);
    if (!c)
        c = wl_cursor_theme_get_cursor(wl.cursor_theme, "left_ptr");
    if (!c || !c->image_count)
        return;
    struct wl_cursor_image *image = c->images[0];
    struct wl_buffer *buffer = wl_cursor_image_get_buffer(image);
    if (!wl.cursor_surface)
        wl.cursor_surface = wl_compositor_create_surface(wl.compositor);
    wl_pointer_set_cursor(wl.pointer, wl.enter_serial, wl.cursor_surface, (int32_t)image->hotspot_x / scale,
                          (int32_t)image->hotspot_y / scale);
    wl_surface_set_buffer_scale(wl.cursor_surface, scale);
    wl_surface_attach(wl.cursor_surface, buffer, 0, 0);
    wl_surface_damage(wl.cursor_surface, 0, 0, (int32_t)image->width, (int32_t)image->height);
    wl_surface_commit(wl.cursor_surface);
}

/* Pointer */
static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface, wl_fixed_t x,
                          wl_fixed_t y) {
    (void)data, (void)p;
    wl.enter_serial = serial;
    wl.pointer_window = window_for(surface);
    wl.pointer_x = (float)wl_fixed_to_double(x);
    wl.pointer_y = (float)wl_fixed_to_double(y);
    apply_cursor();
    send(wl.pointer_window, (CuEvent){.type = CU_EV_MOTION, .x = wl.pointer_x, .y = wl.pointer_y, .mods = wl.mods});
}
static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface) {
    (void)data, (void)p, (void)serial, (void)surface;
    send(wl.pointer_window, (CuEvent){.type = CU_EV_LEAVE});
    wl.pointer_window = NULL;
}
static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    (void)data, (void)p, (void)time;
    wl.pointer_x = (float)wl_fixed_to_double(x);
    wl.pointer_y = (float)wl_fixed_to_double(y);
    send(wl.pointer_window, (CuEvent){.type = CU_EV_MOTION, .x = wl.pointer_x, .y = wl.pointer_y, .mods = wl.mods});
}
static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    (void)data, (void)p, (void)time;
    wl.pointer_serial = wl.input_serial = serial;
    int b = button == BTN_LEFT ? 1 : button == BTN_MIDDLE ? 2 : button == BTN_RIGHT ? 3 : 0;
    if (!b)
        return;
    send(wl.pointer_window, (CuEvent){.type = state == WL_POINTER_BUTTON_STATE_PRESSED ? CU_EV_PRESS : CU_EV_RELEASE,
                                      .x = wl.pointer_x, .y = wl.pointer_y, .button = b, .mods = wl.mods});
}
static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data, (void)p, (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wl.scroll_dy += wl_fixed_to_double(value);
    else
        wl.scroll_dx += wl_fixed_to_double(value);
}
static void pointer_frame(void *data, struct wl_pointer *p) {
    (void)data, (void)p;
    double dx = wl.scroll_dx, dy = wl.scroll_dy;
    if (wl.scroll_wheel && (wl.scroll_120_y || wl.scroll_120_x)) {
        /* One wheel notch scrolls about three lines. */
        dy = wl.scroll_120_y / 120.0 * 56;
        dx = wl.scroll_120_x / 120.0 * 56;
    } else if (wl.scroll_wheel)
        dy *= 3.5, dx *= 3.5;
    if (dx || dy)
        send(wl.pointer_window, (CuEvent){.type = CU_EV_SCROLL, .x = wl.pointer_x, .y = wl.pointer_y, .dx = (float)dx,
                                          .dy = (float)dy, .mods = wl.mods});
    wl.scroll_dx = wl.scroll_dy = 0;
    wl.scroll_120_x = wl.scroll_120_y = 0;
    wl.scroll_wheel = false;
}
static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source) {
    (void)data, (void)p;
    wl.scroll_wheel = source == WL_POINTER_AXIS_SOURCE_WHEEL;
}
static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis) {
    (void)data, (void)p, (void)time, (void)axis;
}
static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete) {
    (void)data, (void)p, (void)axis, (void)discrete;
}
static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value) {
    (void)data, (void)p;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        wl.scroll_120_y += value;
    else
        wl.scroll_120_x += value;
}
static void pointer_axis_direction(void *data, struct wl_pointer *p, uint32_t axis, uint32_t direction) {
    (void)data, (void)p, (void)axis, (void)direction;
}
static const struct wl_pointer_listener pointer_listener = {
    pointer_enter,         pointer_leave,     pointer_motion,        pointer_button,
    pointer_axis,          pointer_frame,     pointer_axis_source,   pointer_axis_stop,
    pointer_axis_discrete, pointer_axis_value120, pointer_axis_direction};

/* Keyboard */
static int key_from_sym(xkb_keysym_t sym) {
    switch (sym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        return CU_KEY_RETURN;
    case XKB_KEY_Escape:
        return CU_KEY_ESCAPE;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
        return CU_KEY_TAB;
    case XKB_KEY_BackSpace:
        return CU_KEY_BACKSPACE;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
        return CU_KEY_DELETE;
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
        return CU_KEY_LEFT;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
        return CU_KEY_RIGHT;
    case XKB_KEY_Up:
    case XKB_KEY_KP_Up:
        return CU_KEY_UP;
    case XKB_KEY_Down:
    case XKB_KEY_KP_Down:
        return CU_KEY_DOWN;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
        return CU_KEY_HOME;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
        return CU_KEY_END;
    case XKB_KEY_Page_Up:
        return CU_KEY_PAGE_UP;
    case XKB_KEY_Page_Down:
        return CU_KEY_PAGE_DOWN;
    case XKB_KEY_Insert:
        return CU_KEY_INSERT;
    case XKB_KEY_Menu:
        return CU_KEY_MENU;
    default:
        break;
    }
    if (sym >= XKB_KEY_F1 && sym <= XKB_KEY_F12)
        return CU_KEY_F1 + (int)(sym - XKB_KEY_F1);
    uint32_t c = xkb_keysym_to_utf32(xkb_keysym_to_lower(sym));
    return c ? (int)c : 0;
}
static void keyboard_keymap(void *data, struct wl_keyboard *k, uint32_t format, int32_t fd, uint32_t size) {
    (void)data, (void)k;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }
    char *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED)
        return;
    struct xkb_keymap *keymap = xkb_keymap_new_from_string(wl.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(map, size);
    if (!keymap)
        return;
    if (wl.xkb_state)
        xkb_state_unref(wl.xkb_state);
    if (wl.keymap)
        xkb_keymap_unref(wl.keymap);
    wl.keymap = keymap;
    wl.xkb_state = xkb_state_new(keymap);
}
static void keyboard_enter(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys) {
    (void)data, (void)k, (void)keys;
    wl.input_serial = serial;
    wl.keyboard_window = window_for(surface);
}
static void keyboard_leave(void *data, struct wl_keyboard *k, uint32_t serial, struct wl_surface *surface) {
    (void)data, (void)k, (void)serial, (void)surface;
    wl.repeat_key = 0;
    wl.keyboard_window = NULL;
}
static void key_event(uint32_t key, bool repeat) {
    if (!wl.xkb_state || !wl.keyboard_window)
        return;
    xkb_keycode_t code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(wl.xkb_state, code);
    CuWindow *w = wl.keyboard_window;
    int k = key_from_sym(sym);
    char text[64] = {0};
    bool compose_text = false;
    if (wl.compose && !repeat) {
        xkb_compose_state_feed(wl.compose, sym);
        enum xkb_compose_status status = xkb_compose_state_get_status(wl.compose);
        if (status == XKB_COMPOSE_COMPOSING)
            return;
        if (status == XKB_COMPOSE_COMPOSED) {
            xkb_compose_state_get_utf8(wl.compose, text, sizeof(text));
            xkb_compose_state_reset(wl.compose);
            compose_text = true;
        } else if (status == XKB_COMPOSE_CANCELLED) {
            xkb_compose_state_reset(wl.compose);
            return;
        }
    }
    if (!compose_text)
        xkb_state_key_get_utf8(wl.xkb_state, code, text, sizeof(text));
    if (k)
        send(w, (CuEvent){.type = CU_EV_KEY, .key = k, .mods = wl.mods, .repeat = repeat});
    bool printable = (unsigned char)text[0] >= 0x20 && text[0] != 0x7f;
    if (printable && !(wl.mods & (CU_MOD_CTRL | CU_MOD_ALT | CU_MOD_SUPER)))
        send(w, (CuEvent){.type = CU_EV_TEXT, .text = text});
}
static void keyboard_key(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state) {
    (void)data, (void)k, (void)time;
    wl.input_serial = serial;
    if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        key_event(key, false);
        if (wl.keymap && xkb_keymap_key_repeats(wl.keymap, key + 8) && wl.repeat_rate > 0) {
            wl.repeat_key = key;
            wl.repeat_next = ch_monotonic_us() + (int64_t)wl.repeat_delay * 1000;
        }
    } else if (wl.repeat_key == key)
        wl.repeat_key = 0;
}
static void keyboard_modifiers(void *data, struct wl_keyboard *k, uint32_t serial, uint32_t depressed, uint32_t latched,
                               uint32_t locked, uint32_t group) {
    (void)data, (void)k, (void)serial;
    if (!wl.xkb_state)
        return;
    xkb_state_update_mask(wl.xkb_state, depressed, latched, locked, 0, 0, group);
    wl.mods = 0;
    if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
        wl.mods |= CU_MOD_CTRL;
    if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_SHIFT, XKB_STATE_MODS_EFFECTIVE) > 0)
        wl.mods |= CU_MOD_SHIFT;
    if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_ALT, XKB_STATE_MODS_EFFECTIVE) > 0)
        wl.mods |= CU_MOD_ALT;
    if (xkb_state_mod_name_is_active(wl.xkb_state, XKB_MOD_NAME_LOGO, XKB_STATE_MODS_EFFECTIVE) > 0)
        wl.mods |= CU_MOD_SUPER;
}
static void keyboard_repeat(void *data, struct wl_keyboard *k, int32_t rate, int32_t delay) {
    (void)data, (void)k;
    wl.repeat_rate = rate;
    wl.repeat_delay = delay;
}
static const struct wl_keyboard_listener keyboard_listener = {keyboard_keymap, keyboard_enter,     keyboard_leave,
                                                              keyboard_key,    keyboard_modifiers, keyboard_repeat};

/* Input methods (text-input-v3) */
static void ti_enter(void *data, struct zwp_text_input_v3 *t, struct wl_surface *s) {
    (void)data, (void)s;
    zwp_text_input_v3_enable(t);
    zwp_text_input_v3_set_content_type(t, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE, ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
    zwp_text_input_v3_commit(t);
    wl.text_input_enabled = true;
}
static void ti_leave(void *data, struct zwp_text_input_v3 *t, struct wl_surface *s) {
    (void)data, (void)s;
    zwp_text_input_v3_disable(t);
    zwp_text_input_v3_commit(t);
    wl.text_input_enabled = false;
}
static void ti_preedit(void *data, struct zwp_text_input_v3 *t, const char *text, int32_t begin, int32_t end) {
    (void)data, (void)t, (void)begin, (void)end;
    free(wl.ime_preedit);
    wl.ime_preedit = ch_strdup(text ? text : "");
}
static void ti_commit(void *data, struct zwp_text_input_v3 *t, const char *text) {
    (void)data, (void)t;
    free(wl.ime_commit);
    wl.ime_commit = ch_strdup(text);
}
static void ti_delete(void *data, struct zwp_text_input_v3 *t, uint32_t before, uint32_t after) {
    (void)data, (void)t, (void)before, (void)after;
}
static void ti_done(void *data, struct zwp_text_input_v3 *t, uint32_t serial) {
    (void)data, (void)t, (void)serial;
    CuWindow *w = wl.keyboard_window;
    if (wl.ime_commit && *wl.ime_commit)
        send(w, (CuEvent){.type = CU_EV_TEXT, .text = wl.ime_commit});
    send(w, (CuEvent){.type = CU_EV_PREEDIT, .text = wl.ime_preedit ? wl.ime_preedit : ""});
    ch_clear_pointer(&wl.ime_commit, free);
    ch_clear_pointer(&wl.ime_preedit, free);
}
static const struct zwp_text_input_v3_listener text_input_listener = {ti_enter, ti_leave, ti_preedit, ti_commit,
                                                                      ti_delete, ti_done};

/* Clipboard and drag and drop: both arrive as data offers listing their MIME types. */
static void offer_clear(Offer *o) {
    if (o->offer)
        wl_data_offer_destroy(o->offer);
    for (int i = 0; i < o->count; i++)
        free(o->mimes[i]);
    memset(o, 0, sizeof(*o));
}
static void offer_mime(void *data, struct wl_data_offer *offer, const char *mime) {
    (void)data;
    Offer *o = offer == wl.pending.offer ? &wl.pending : offer == wl.selection.offer ? &wl.selection
             : offer == wl.drag.offer    ? &wl.drag     : NULL;
    if (o && o->count < (int)CH_N_ELEMENTS(o->mimes))
        o->mimes[o->count++] = ch_strdup(mime);
}
static void offer_source_actions(void *data, struct wl_data_offer *o, uint32_t a) {
    (void)data, (void)o, (void)a;
}
static void offer_action(void *data, struct wl_data_offer *o, uint32_t a) {
    (void)data, (void)o, (void)a;
}
static const struct wl_data_offer_listener offer_listener = {offer_mime, offer_source_actions, offer_action};
static void device_offer(void *data, struct wl_data_device *d, struct wl_data_offer *offer) {
    (void)data, (void)d;
    offer_clear(&wl.pending);
    wl.pending.offer = offer;
    wl_data_offer_add_listener(offer, &offer_listener, NULL);
}
static const char *offer_best(const Offer *o) {
    int best = ch_clipboard_best((const char *const *)o->mimes, o->count);
    return best >= 0 ? o->mimes[best] : NULL;
}
static void drag_event(bool inside) {
    CuWindow *w = wl.window_count ? wl.windows[0] : NULL;
    for (int i = 0; i < wl.window_count; i++)
        if (wl.windows[i]->surface == wl.drag_surface)
            w = wl.windows[i];
    if (w)
        send(w, (CuEvent){.type = CU_EV_DRAG, .focused = inside, .x = wl.drag_x, .y = wl.drag_y});
}
static void device_enter(void *data, struct wl_data_device *d, uint32_t serial, struct wl_surface *s, wl_fixed_t x,
                         wl_fixed_t y, struct wl_data_offer *offer) {
    (void)data, (void)d;
    offer_clear(&wl.drag);
    if (offer == wl.pending.offer) {
        wl.drag = wl.pending;
        memset(&wl.pending, 0, sizeof(wl.pending));
    }
    wl.drag_serial = serial;
    wl.drag_surface = s;
    wl.drag_x = (float)wl_fixed_to_double(x);
    wl.drag_y = (float)wl_fixed_to_double(y);
    const char *mime = offer_best(&wl.drag);
    if (offer) {
        wl_data_offer_accept(offer, serial, mime);
        if (mime)
            wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    }
    if (mime)
        drag_event(true);
}
static void device_leave(void *data, struct wl_data_device *d) {
    (void)data, (void)d;
    if (offer_best(&wl.drag))
        drag_event(false);
    offer_clear(&wl.drag);
}
static void device_motion(void *data, struct wl_data_device *d, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)data, (void)d, (void)t;
    wl.drag_x = (float)wl_fixed_to_double(x);
    wl.drag_y = (float)wl_fixed_to_double(y);
    if (offer_best(&wl.drag))
        drag_event(true);
}
static int paste_begin(struct wl_data_offer *offer, const char *mime) {
    int fds[2];
    if (wl.paste_count >= (int)CH_N_ELEMENTS(wl.pastes) || pipe2(fds, O_CLOEXEC | O_NONBLOCK))
        return -1;
    wl_data_offer_receive(offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(wl.display);
    int index = wl.paste_count++;
    memset(&wl.pastes[index], 0, sizeof(Paste));
    wl.pastes[index].fd = fds[0];
    wl.pastes[index].mime = ch_strdup(mime);
    ch_buf_init(&wl.pastes[index].data);
    return index;
}
static void device_drop(void *data, struct wl_data_device *d) {
    (void)data, (void)d;
    const char *mime = offer_best(&wl.drag);
    int index = mime ? paste_begin(wl.drag.offer, mime) : -1;
    if (index >= 0) {
        /* The offer stays alive until its bytes have arrived. */
        wl.pastes[index].drop = wl.drag;
        memset(&wl.drag, 0, sizeof(wl.drag));
    }
    drag_event(false);
}
static void device_selection(void *data, struct wl_data_device *d, struct wl_data_offer *offer) {
    (void)data, (void)d;
    offer_clear(&wl.selection);
    if (offer && offer == wl.pending.offer) {
        wl.selection = wl.pending;
        memset(&wl.pending, 0, sizeof(wl.pending));
    } else if (offer)
        wl.selection.offer = offer;
}
static const struct wl_data_device_listener device_listener = {device_offer, device_enter, device_leave,
                                                               device_motion, device_drop, device_selection};
static void source_target(void *data, struct wl_data_source *s, const char *mime) {
    (void)data, (void)s, (void)mime;
}
static void source_send(void *data, struct wl_data_source *s, const char *mime, int32_t fd) {
    (void)data, (void)s, (void)mime;
    const char *text = wl.source_text ? wl.source_text : "";
    size_t n = strlen(text), off = 0;
    while (off < n) {
        ssize_t w = write(fd, text + off, n - off);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    close(fd);
}
static void source_cancelled(void *data, struct wl_data_source *s) {
    (void)data;
    if (wl.source == s) {
        wl.source = NULL;
        ch_clear_pointer(&wl.source_text, free);
    }
    wl_data_source_destroy(s);
}
static void source_dnd(void *data, struct wl_data_source *s) {
    (void)data, (void)s;
}
static void source_finished(void *data, struct wl_data_source *s) {
    (void)data, (void)s;
}
static void source_action(void *data, struct wl_data_source *s, uint32_t a) {
    (void)data, (void)s, (void)a;
}
static const struct wl_data_source_listener source_listener = {source_target, source_send, source_cancelled,
                                                               source_dnd, source_finished, source_action};
static void clipboard_set(CuWindow *w, const char *text) {
    (void)w;
    if (!wl.data_device)
        return;
    struct wl_data_source *source = wl_data_device_manager_create_data_source(wl.data_manager);
    wl_data_source_add_listener(source, &source_listener, NULL);
    const char *mimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "TEXT", "STRING"};
    for (size_t i = 0; i < CH_N_ELEMENTS(mimes); i++)
        wl_data_source_offer(source, mimes[i]);
    free(wl.source_text);
    wl.source_text = ch_strdup(text);
    wl.source = source;
    wl_data_device_set_selection(wl.data_device, source, wl.input_serial);
}
/* The best plain-text type on offer. */
static const char *selection_text(void) {
    const char *best = NULL;
    for (int i = 0; i < wl.selection.count; i++) {
        const char *mime = wl.selection.mimes[i];
        if (!strcmp(mime, "text/plain;charset=utf-8"))
            return mime;
        if (!best && (!strcmp(mime, "text/plain") || !strcmp(mime, "UTF8_STRING") || !strcmp(mime, "TEXT") || !strcmp(mime, "STRING")))
            best = mime;
    }
    return best;
}
static void clipboard_request(CuWindow *w, void (*callback)(const char *, void *), void *data) {
    (void)w;
    if (wl.source && wl.source_text) {
        /* Our own selection: no round trip through the compositor (which would deadlock). */
        callback(wl.source_text, data);
        return;
    }
    const char *mime = wl.selection.offer ? selection_text() : NULL;
    int index = mime ? paste_begin(wl.selection.offer, mime) : -1;
    if (index < 0) {
        callback(NULL, data);
        return;
    }
    wl.pastes[index].text_callback = callback;
    wl.pastes[index].user = data;
}
static void clipboard_request_data(CuWindow *w, void (*callback)(const char *, const void *, size_t, void *), void *data) {
    (void)w;
    if (wl.source && wl.source_text) {
        callback("text/plain", wl.source_text, strlen(wl.source_text), data);
        return;
    }
    const char *mime = wl.selection.offer ? offer_best(&wl.selection) : NULL;
    int index = mime ? paste_begin(wl.selection.offer, mime) : -1;
    if (index < 0) {
        callback(NULL, NULL, 0, data);
        return;
    }
    wl.pastes[index].data_callback = callback;
    wl.pastes[index].user = data;
}
static void paste_read(int index) {
    Paste *p = &wl.pastes[index];
    char chunk[65536];
    for (;;) {
        ssize_t n = read(p->fd, chunk, sizeof(chunk));
        if (n > 0) {
            if (p->data.len < PASTE_LIMIT)
                ch_buf_append_len(&p->data, chunk, (size_t)n);
            else
                p->overflow = true;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return;
        break;
    }
    close(p->fd);
    Paste done = *p;
    memmove(wl.pastes + index, wl.pastes + index + 1, sizeof(Paste) * (size_t)(wl.paste_count - index - 1));
    wl.paste_count--;
    if (done.text_callback) {
        char *text = ch_utf8_make_valid(done.data.str, (ptrdiff_t)done.data.len);
        done.text_callback(text, done.user);
        free(text);
    } else if (done.data_callback) {
        /* A partial image is worse than none. */
        bool whole = !done.overflow && done.data.len;
        done.data_callback(whole ? done.mime : NULL, whole ? done.data.str : NULL, whole ? done.data.len : 0, done.user);
    } else if (done.drop.offer) {
        wl_data_offer_finish(done.drop.offer);
        offer_clear(&done.drop);
        CuWindow *target = wl.window_count ? wl.windows[0] : NULL;
        if (target && !done.overflow && done.data.len)
            send(target, (CuEvent){.type = CU_EV_DROP, .mime = done.mime, .data = done.data.str, .size = done.data.len});
    }
    free(done.mime);
    ch_buf_clear(&done.data);
}

/* Seat and registry */
static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    (void)data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl.pointer) {
        wl.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl.pointer, &pointer_listener, NULL);
        if (wl.cursor_shapes)
            wl.cursor_device = wp_cursor_shape_manager_v1_get_pointer(wl.cursor_shapes, wl.pointer);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl.pointer) {
        if (wl.cursor_device)
            wp_cursor_shape_device_v1_destroy(wl.cursor_device);
        wl.cursor_device = NULL;
        wl_pointer_release(wl.pointer);
        wl.pointer = NULL;
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl.keyboard) {
        wl.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl.keyboard, &keyboard_listener, NULL);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl.keyboard) {
        wl_keyboard_release(wl.keyboard);
        wl.keyboard = NULL;
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data, (void)seat, (void)name;
}
static const struct wl_seat_listener seat_listener = {seat_capabilities, seat_name};
static void registry_global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
    (void)data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        wl.compositor_version = CH_MIN(version, 6u);
        wl.compositor = wl_registry_bind(r, name, &wl_compositor_interface, wl.compositor_version);
    } else if (!strcmp(interface, wl_shm_interface.name))
        wl.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, xdg_wm_base_interface.name)) {
        wl.wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, CH_MIN(version, 5u));
        xdg_wm_base_add_listener(wl.wm_base, &wm_listener, NULL);
    } else if (!strcmp(interface, wl_seat_interface.name) && !wl.seat) {
        wl.seat = wl_registry_bind(r, name, &wl_seat_interface, CH_MIN(version, 8u));
        wl_seat_add_listener(wl.seat, &seat_listener, NULL);
    } else if (!strcmp(interface, wl_data_device_manager_interface.name))
        wl.data_manager = wl_registry_bind(r, name, &wl_data_device_manager_interface, CH_MIN(version, 3u));
    else if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name))
        wl.decorations = wl_registry_bind(r, name, &zxdg_decoration_manager_v1_interface, 1);
    else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name))
        wl.fractional = wl_registry_bind(r, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(interface, wp_viewporter_interface.name))
        wl.viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
    else if (!strcmp(interface, wp_cursor_shape_manager_v1_interface.name))
        wl.cursor_shapes = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
    else if (!strcmp(interface, zwp_text_input_manager_v3_interface.name) && !ch_getenv("CHELA_NO_IME"))
        wl.text_inputs = wl_registry_bind(r, name, &zwp_text_input_manager_v3_interface, 1);
    else if (!strcmp(interface, wl_output_interface.name) && wl.output_count < 8) {
        struct wl_output *o = wl_registry_bind(r, name, &wl_output_interface, CH_MIN(version, 4u));
        wl.outputs[wl.output_count] = o;
        wl.output_scales[wl.output_count++] = 1;
        wl_output_add_listener(o, &output_listener, NULL);
    }
}
static void registry_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data, (void)r, (void)name;
}
static const struct wl_registry_listener registry_listener = {registry_global, registry_remove};

static bool init(int *argc, char ***argv) {
    (void)argc, (void)argv;
    setlocale(LC_CTYPE, "");
    wl.display = wl_display_connect(NULL);
    if (!wl.display)
        return false;
    ch_mutex_init(&wl.lock);
    wl.wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    wl.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    const char *locale = setlocale(LC_CTYPE, NULL);
    struct xkb_compose_table *table =
        xkb_compose_table_new_from_locale(wl.xkb, locale ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
    if (table) {
        wl.compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
        xkb_compose_table_unref(table);
    }
    wl.repeat_rate = 25;
    wl.repeat_delay = 600;
    wl.registry = wl_display_get_registry(wl.display);
    wl_registry_add_listener(wl.registry, &registry_listener, NULL);
    wl_display_roundtrip(wl.display);
    if (!wl.compositor || !wl.shm || !wl.wm_base) {
        fprintf(stderr, "chela: the Wayland compositor lacks wl_compositor, wl_shm or xdg_wm_base\n");
        return false;
    }
    wl_display_roundtrip(wl.display); /* seat capabilities, output scales */
    if (wl.seat && wl.data_manager) {
        wl.data_device = wl_data_device_manager_get_data_device(wl.data_manager, wl.seat);
        wl_data_device_add_listener(wl.data_device, &device_listener, NULL);
    }
    if (wl.seat && wl.text_inputs) {
        wl.text_input = zwp_text_input_manager_v3_get_text_input(wl.text_inputs, wl.seat);
        zwp_text_input_v3_add_listener(wl.text_input, &text_input_listener, NULL);
    }
    return true;
}
static void report_inactive(void *data) {
    for (int i = 0; i < wl.window_count; i++)
        if (wl.windows[i] == data && !wl.windows[i]->activated)
            send(data, (CuEvent){.type = CU_EV_FOCUS, .focused = false});
}
static CuWindow *window_new(const char *title, const char *app_id, int width, int height, bool csd,
                            const CuWindowHandler *handler, void *data) {
    CuWindow *w = ch_new0(CuWindow, 1);
    w->handler = handler;
    w->data = data;
    w->width = width;
    w->height = height;
    w->scale = w->buffer_scale = 1;
    const char *mode = ch_getenv("CHELA_DECORATIONS");
    w->want_csd = csd && !(mode && !strcmp(mode, "server"));
    w->csd = w->want_csd || !wl.decorations;
    w->surface = wl_compositor_create_surface(wl.compositor);
    wl_surface_add_listener(w->surface, &wl_surface_listener, w);
    if (wl.fractional && wl.viewporter) {
        w->fractional = wp_fractional_scale_manager_v1_get_fractional_scale(wl.fractional, w->surface);
        wp_fractional_scale_v1_add_listener(w->fractional, &fractional_listener, w);
        w->viewport = wp_viewporter_get_viewport(wl.viewporter, w->surface);
    } else {
        w->buffer_scale = integer_scale(w);
        w->scale = w->buffer_scale;
    }
    w->xdg_surface = xdg_wm_base_get_xdg_surface(wl.wm_base, w->surface);
    xdg_surface_add_listener(w->xdg_surface, &surface_listener, w);
    w->toplevel = xdg_surface_get_toplevel(w->xdg_surface);
    xdg_toplevel_add_listener(w->toplevel, &toplevel_listener, w);
    xdg_toplevel_set_title(w->toplevel, title);
    xdg_toplevel_set_app_id(w->toplevel, app_id);
    if (wl.decorations) {
        w->decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(wl.decorations, w->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(w->decoration, &decoration_listener, w);
        zxdg_toplevel_decoration_v1_set_mode(w->decoration, w->want_csd ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                                                                        : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    }
    wl_surface_commit(w->surface);
    if (wl.window_count < 8)
        wl.windows[wl.window_count++] = w;
    /* Wait for the first configure (size, decoration mode) without calling the handler. */
    w->starting = true;
    wl_display_roundtrip(wl.display);
    w->starting = false;
    if (!w->activated)
        post(report_inactive, w); /* the UI assumes a new window is active */
    return w;
}
static void window_free(CuWindow *w) {
    for (int i = 0; i < wl.window_count; i++)
        if (wl.windows[i] == w) {
            memmove(wl.windows + i, wl.windows + i + 1, sizeof(CuWindow *) * (size_t)(wl.window_count - i - 1));
            wl.window_count--;
        }
    if (wl.pointer_window == w)
        wl.pointer_window = NULL;
    if (wl.keyboard_window == w)
        wl.keyboard_window = NULL;
    if (w->frame)
        wl_callback_destroy(w->frame);
    for (int i = 0; i < 2; i++)
        buffer_destroy(&w->buffers[i]);
    if (w->decoration)
        zxdg_toplevel_decoration_v1_destroy(w->decoration);
    if (w->viewport)
        wp_viewport_destroy(w->viewport);
    if (w->fractional)
        wp_fractional_scale_v1_destroy(w->fractional);
    xdg_toplevel_destroy(w->toplevel);
    xdg_surface_destroy(w->xdg_surface);
    wl_surface_destroy(w->surface);
    free(w);
}
static void window_set_title(CuWindow *w, const char *title) {
    xdg_toplevel_set_title(w->toplevel, title);
}
static void window_set_min_size(CuWindow *w, int width, int height) {
    xdg_toplevel_set_min_size(w->toplevel, width, height);
}
static void window_set_cursor(CuWindow *w, CuCursor cursor) {
    (void)w;
    if (wl.cursor != cursor) {
        wl.cursor = cursor;
        apply_cursor();
    }
}
static void window_minimize(CuWindow *w) {
    xdg_toplevel_set_minimized(w->toplevel);
}
static void window_toggle_maximize(CuWindow *w) {
    if (w->maximized)
        xdg_toplevel_unset_maximized(w->toplevel);
    else
        xdg_toplevel_set_maximized(w->toplevel);
}
static bool window_is_maximized(CuWindow *w) {
    return w->maximized;
}
static void window_begin_move(CuWindow *w) {
    if (wl.seat)
        xdg_toplevel_move(w->toplevel, wl.seat, wl.pointer_serial);
}
static void window_begin_resize(CuWindow *w, CuEdge edge) {
    static const uint32_t edges[] = {XDG_TOPLEVEL_RESIZE_EDGE_NONE,      XDG_TOPLEVEL_RESIZE_EDGE_TOP,
                                     XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM,    XDG_TOPLEVEL_RESIZE_EDGE_RIGHT,
                                     XDG_TOPLEVEL_RESIZE_EDGE_LEFT,      XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT,
                                     XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT,  XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT,
                                     XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT};
    if (wl.seat && edge)
        xdg_toplevel_resize(w->toplevel, wl.seat, wl.pointer_serial, edges[edge]);
}
static bool window_has_client_decorations(CuWindow *w) {
    return w->csd;
}
static void window_set_text_input(CuWindow *w, bool enabled, CuRect caret) {
    (void)w;
    if (!wl.text_input || !wl.text_input_enabled || !enabled)
        return;
    if (fabsf(caret.x - wl.text_cursor.x) < 0.5f && fabsf(caret.y - wl.text_cursor.y) < 0.5f && caret.h == wl.text_cursor.h)
        return;
    wl.text_cursor = caret;
    zwp_text_input_v3_set_cursor_rectangle(wl.text_input, (int32_t)caret.x, (int32_t)caret.y, 1, (int32_t)caret.h);
    zwp_text_input_v3_commit(wl.text_input);
}

/* Main loop */
static void post(void (*fn)(void *), void *data) {
    Posted *p = ch_new0(Posted, 1);
    p->fn = fn;
    p->data = data;
    ch_mutex_lock(&wl.lock);
    if (wl.posted_tail)
        wl.posted_tail->next = p;
    else
        wl.posted = p;
    wl.posted_tail = p;
    ch_mutex_unlock(&wl.lock);
    uint64_t one = 1;
    (void)!write(wl.wake_fd, &one, sizeof(one));
}
static unsigned timer_add(int ms, bool (*fn)(void *), void *data) {
    if (wl.timer_count >= (int)CH_N_ELEMENTS(wl.timers))
        return 0;
    Timer *t = &wl.timers[wl.timer_count++];
    *t = (Timer){++wl.next_timer, ch_monotonic_us() + (int64_t)ms * 1000, ms, fn, data};
    return t->id;
}
static void timer_remove(unsigned id) {
    for (int i = 0; i < wl.timer_count; i++)
        if (wl.timers[i].id == id) {
            wl.timers[i] = wl.timers[--wl.timer_count];
            return;
        }
}
static void run_posted(void) {
    uint64_t n;
    while (read(wl.wake_fd, &n, sizeof(n)) > 0) {
    }
    ch_mutex_lock(&wl.lock);
    Posted *p = wl.posted;
    wl.posted = wl.posted_tail = NULL;
    ch_mutex_unlock(&wl.lock);
    while (p) {
        Posted *next = p->next;
        p->fn(p->data);
        free(p);
        p = next;
    }
}
static void run_timers(void) {
    int64_t now = ch_monotonic_us();
    for (int i = 0; i < wl.timer_count && !wl.quit; i++) {
        if (wl.timers[i].due > now)
            continue;
        Timer t = wl.timers[i];
        bool again = t.fn(t.data);
        for (int k = 0; k < wl.timer_count; k++)
            if (wl.timers[k].id == t.id) {
                if (again)
                    wl.timers[k].due = ch_monotonic_us() + (int64_t)t.interval * 1000;
                else
                    wl.timers[k] = wl.timers[--wl.timer_count];
                break;
            }
        i = -1;
        now = ch_monotonic_us();
    }
    if (wl.repeat_key && now >= wl.repeat_next) {
        key_event(wl.repeat_key, true);
        wl.repeat_next = now + 1000000 / CH_MAX(wl.repeat_rate, 1);
    }
}
static int next_timeout(void) {
    int64_t now = ch_monotonic_us(), next = now + 1000000;
    for (int i = 0; i < wl.timer_count; i++)
        next = CH_MIN(next, wl.timers[i].due);
    if (wl.repeat_key)
        next = CH_MIN(next, wl.repeat_next);
    return (int)CH_MAX(0, (next - now + 999) / 1000);
}
static void run(void) {
    while (!wl.quit) {
        while (wl_display_prepare_read(wl.display) != 0)
            wl_display_dispatch_pending(wl.display);
        if (wl_display_flush(wl.display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(wl.display);
            break;
        }
        struct pollfd fds[2 + CH_N_ELEMENTS(wl.pastes)];
        fds[0] = (struct pollfd){wl_display_get_fd(wl.display), POLLIN, 0};
        fds[1] = (struct pollfd){wl.wake_fd, POLLIN, 0};
        int n = 2;
        for (int i = 0; i < wl.paste_count; i++)
            fds[n++] = (struct pollfd){wl.pastes[i].fd, POLLIN | POLLHUP, 0};
        int ready = poll(fds, (nfds_t)n, next_timeout());
        if (ready > 0 && (fds[0].revents & POLLIN)) {
            if (wl_display_read_events(wl.display) < 0)
                break;
        } else
            wl_display_cancel_read(wl.display);
        if (fds[0].revents & (POLLERR | POLLHUP))
            break;
        wl_display_dispatch_pending(wl.display);
        if (fds[1].revents & POLLIN)
            run_posted();
        for (int i = n - 1; i >= 2; i--)
            if (fds[i].revents)
                paste_read(i - 2);
        run_timers();
        for (int i = 0; i < wl.window_count; i++) {
            CuWindow *w = wl.windows[i];
            if (w->dirty && w->configured && !w->frame)
                paint(w);
        }
    }
}
static void quit(void) {
    wl.quit = true;
}
static void shutdown_backend(void) {
    while (wl.window_count)
        window_free(wl.windows[0]);
    if (wl.cursor_theme)
        wl_cursor_theme_destroy(wl.cursor_theme);
    if (wl.display) {
        wl_display_flush(wl.display);
        wl_display_disconnect(wl.display);
    }
    cc_shutdown();
}
const CuPlatform *cu_platform_wayland(void) {
    static CuPlatform p;
    if (!p.abi) {
        p.abi = CU_PLATFORM_ABI;
        p.name = "wayland";
        p.init = init;
        p.run = run;
        p.quit = quit;
        p.shutdown = shutdown_backend;
        p.window_new = window_new;
        p.window_free = window_free;
        p.window_set_title = window_set_title;
        p.window_set_min_size = window_set_min_size;
        p.window_redraw = redraw;
        p.window_set_cursor = window_set_cursor;
        p.window_minimize = window_minimize;
        p.window_toggle_maximize = window_toggle_maximize;
        p.window_is_maximized = window_is_maximized;
        p.window_begin_move = window_begin_move;
        p.window_begin_resize = window_begin_resize;
        p.window_has_client_decorations = window_has_client_decorations;
        p.window_set_text_input = window_set_text_input;
        p.post = post;
        p.timer_add = timer_add;
        p.timer_remove = timer_remove;
        p.clipboard_set = clipboard_set;
        p.clipboard_request = clipboard_request;
        p.clipboard_request_data = clipboard_request_data;
        cc_install(&p);
    }
    return &p;
}
