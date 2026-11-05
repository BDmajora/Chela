/* GTK 3 host, loaded as a plugin with --backend=gtk (and used on X11 when no
 * Wayland compositor is running). GTK supplies the window, input, input methods,
 * the clipboard, the main loop and native file dialogs; drawing and text use the
 * same cairo canvas as the native Wayland backend. */
#include "cairo_canvas.h"
#include "../core/base.h"
#include <gtk/gtk.h>
#ifdef GDK_WINDOWING_WAYLAND
#include <gdk/gdkwayland.h>
#endif
#include <math.h>
#include <string.h>

struct CuWindow {
    GtkWidget *window, *area;
    GtkIMContext *im;
    const CuWindowHandler *handler;
    void *data;
    bool csd, starting, preediting;
    guint held_key; /* GTK 3 does not mark repeats */
    guint press_button;
    guint32 press_time;
    double press_root_x, press_root_y;
};
typedef struct {
    unsigned id;
    guint source;
    bool (*fn)(void *);
    void *data;
} Timer;
typedef struct {
    void (*fn)(void *);
    void *data;
} Posted;
typedef struct {
    void (*callback)(const char *, void *);
    void *data;
} Request;
static struct {
    GHashTable *timers; /* id -> Timer */
    unsigned next_timer;
    bool quit_pending;
} gk;

static void send(CuWindow *w, CuEvent ev) {
    if (!w->starting)
        w->handler->event(w, &ev, w->data);
}
static int mods_from(guint state) {
    int mods = 0;
    if (state & GDK_SHIFT_MASK)
        mods |= CU_MOD_SHIFT;
    if (state & GDK_CONTROL_MASK)
        mods |= CU_MOD_CTRL;
    if (state & GDK_MOD1_MASK)
        mods |= CU_MOD_ALT;
    if (state & (GDK_SUPER_MASK | GDK_MOD4_MASK))
        mods |= CU_MOD_SUPER;
    return mods;
}

/* Painting and pointer */
static gboolean on_draw(GtkWidget *widget, cairo_t *cr, CuWindow *w) {
    if (w->starting)
        return TRUE;
    CuCanvas canvas = {cr};
    int width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    w->handler->paint(w, &canvas, width, height, w->data);
    return TRUE;
}
static void on_size_allocate(GtkWidget *widget, GdkRectangle *r, CuWindow *w) {
    send(w, (CuEvent){.type = CU_EV_RESIZE, .width = r->width, .height = r->height,
                      .scale = (float)gtk_widget_get_scale_factor(widget)});
}
static gboolean on_motion(GtkWidget *widget, GdkEventMotion *ev, CuWindow *w) {
    (void)widget;
    send(w, (CuEvent){.type = CU_EV_MOTION, .x = (float)ev->x, .y = (float)ev->y,
                      .mods = mods_from(ev->state)});
    return TRUE;
}
static gboolean on_leave(GtkWidget *widget, GdkEventCrossing *ev, CuWindow *w) {
    (void)widget;
    if (ev->detail != GDK_NOTIFY_INFERIOR)
        send(w, (CuEvent){.type = CU_EV_LEAVE});
    return FALSE;
}
static gboolean on_button(GtkWidget *widget, GdkEventButton *ev, CuWindow *w) {
    /* GTK adds 2BUTTON/3BUTTON events after the presses; the interface counts clicks itself. */
    if (ev->type != GDK_BUTTON_PRESS && ev->type != GDK_BUTTON_RELEASE)
        return TRUE;
    int b = ev->button == 1 ? 1 : ev->button == 2 ? 2 : ev->button == 3 ? 3 : 0;
    if (!b)
        return FALSE;
    if (ev->type == GDK_BUTTON_PRESS) {
        gtk_widget_grab_focus(widget);
        w->press_button = ev->button;
        w->press_time = ev->time;
        w->press_root_x = ev->x_root;
        w->press_root_y = ev->y_root;
    }
    send(w, (CuEvent){.type = ev->type == GDK_BUTTON_PRESS ? CU_EV_PRESS : CU_EV_RELEASE, .x = (float)ev->x,
                      .y = (float)ev->y, .button = b, .mods = mods_from(ev->state)});
    return TRUE;
}
static gboolean on_scroll(GtkWidget *widget, GdkEventScroll *ev, CuWindow *w) {
    (void)widget;
    /* One wheel notch scrolls about three lines, as in the Wayland backend; touchpads
     * report GTK scroll units of 10 pixels. */
    double dx = 0, dy = 0;
    switch (ev->direction) {
    case GDK_SCROLL_UP:
        dy = -56;
        break;
    case GDK_SCROLL_DOWN:
        dy = 56;
        break;
    case GDK_SCROLL_LEFT:
        dx = -56;
        break;
    case GDK_SCROLL_RIGHT:
        dx = 56;
        break;
    case GDK_SCROLL_SMOOTH: {
        GdkDevice *device = gdk_event_get_source_device((GdkEvent *)ev);
        double unit = device && gdk_device_get_source(device) == GDK_SOURCE_TOUCHPAD ? 10 : 56;
        gdk_event_get_scroll_deltas((GdkEvent *)ev, &dx, &dy);
        dx *= unit;
        dy *= unit;
        break;
    }
    }
    if (dx || dy)
        send(w, (CuEvent){.type = CU_EV_SCROLL, .x = (float)ev->x, .y = (float)ev->y, .dx = (float)dx,
                          .dy = (float)dy, .mods = mods_from(ev->state)});
    return TRUE;
}

/* Keyboard and input methods */
static int key_from(guint keyval) {
    switch (keyval) {
    case GDK_KEY_Return:
    case GDK_KEY_KP_Enter:
    case GDK_KEY_ISO_Enter:
        return CU_KEY_RETURN;
    case GDK_KEY_Escape:
        return CU_KEY_ESCAPE;
    case GDK_KEY_Tab:
    case GDK_KEY_KP_Tab:
    case GDK_KEY_ISO_Left_Tab:
        return CU_KEY_TAB;
    case GDK_KEY_BackSpace:
        return CU_KEY_BACKSPACE;
    case GDK_KEY_Delete:
    case GDK_KEY_KP_Delete:
        return CU_KEY_DELETE;
    case GDK_KEY_Left:
    case GDK_KEY_KP_Left:
        return CU_KEY_LEFT;
    case GDK_KEY_Right:
    case GDK_KEY_KP_Right:
        return CU_KEY_RIGHT;
    case GDK_KEY_Up:
    case GDK_KEY_KP_Up:
        return CU_KEY_UP;
    case GDK_KEY_Down:
    case GDK_KEY_KP_Down:
        return CU_KEY_DOWN;
    case GDK_KEY_Home:
    case GDK_KEY_KP_Home:
        return CU_KEY_HOME;
    case GDK_KEY_End:
    case GDK_KEY_KP_End:
        return CU_KEY_END;
    case GDK_KEY_Page_Up:
    case GDK_KEY_KP_Page_Up:
        return CU_KEY_PAGE_UP;
    case GDK_KEY_Page_Down:
    case GDK_KEY_KP_Page_Down:
        return CU_KEY_PAGE_DOWN;
    case GDK_KEY_Insert:
    case GDK_KEY_KP_Insert:
        return CU_KEY_INSERT;
    case GDK_KEY_Menu:
        return CU_KEY_MENU;
    default:
        break;
    }
    if (keyval >= GDK_KEY_F1 && keyval <= GDK_KEY_F12)
        return CU_KEY_F1 + (int)(keyval - GDK_KEY_F1);
    guint32 c = gdk_keyval_to_unicode(gdk_keyval_to_lower(keyval));
    return c >= 0x20 && c != 0x7f ? (int)c : 0;
}
static gboolean on_drag_motion(GtkWidget *widget, GdkDragContext *context, gint x, gint y, guint time, CuWindow *w) {
    (void)widget, (void)context, (void)time;
    CuEvent ev = {.type = CU_EV_DRAG, .focused = true, .x = (float)x, .y = (float)y};
    w->handler->event(w, &ev, w->data);
    return FALSE;
}
static void on_drag_leave(GtkWidget *widget, GdkDragContext *context, guint time, CuWindow *w) {
    (void)widget, (void)context, (void)time;
    CuEvent ev = {.type = CU_EV_DRAG, .focused = false};
    w->handler->event(w, &ev, w->data);
}
static void on_drag_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection,
                             guint info, guint time, CuWindow *w) {
    (void)widget, (void)context, (void)x, (void)y, (void)info, (void)time;
    gint length = 0;
    const guchar *bytes = gtk_selection_data_get_data_with_length(selection, &length);
    char *mime = gdk_atom_name(gtk_selection_data_get_data_type(selection));
    if (bytes && length > 0) {
        CuEvent ev = {.type = CU_EV_DROP, .mime = mime, .data = bytes, .size = (size_t)length};
        w->handler->event(w, &ev, w->data);
    }
    g_free(mime);
}
static gboolean on_key_press(GtkWidget *widget, GdkEventKey *ev, CuWindow *w) {
    (void)widget;
    if (w->preediting && gtk_im_context_filter_keypress(w->im, ev))
        return TRUE;
    int mods = mods_from(ev->state), k = key_from(ev->keyval);
    bool repeat = ev->keyval == w->held_key;
    w->held_key = ev->keyval;
    if (k)
        send(w, (CuEvent){.type = CU_EV_KEY, .key = k, .mods = mods, .repeat = repeat});
    if (gtk_im_context_filter_keypress(w->im, ev))
        return TRUE; /* text arrives through the commit signal */
    guint32 c = gdk_keyval_to_unicode(ev->keyval);
    if (c >= 0x20 && c != 0x7f && !(mods & (CU_MOD_CTRL | CU_MOD_ALT | CU_MOD_SUPER))) {
        char text[5] = {0};
        text[ch_utf8_encode(c, text)] = 0;
        send(w, (CuEvent){.type = CU_EV_TEXT, .text = text});
    }
    return TRUE;
}
static gboolean on_key_release(GtkWidget *widget, GdkEventKey *ev, CuWindow *w) {
    (void)widget;
    if (ev->keyval == w->held_key)
        w->held_key = 0;
    gtk_im_context_filter_keypress(w->im, ev);
    return TRUE;
}
static void on_commit(GtkIMContext *im, const char *text, CuWindow *w) {
    (void)im;
    if (text && *text)
        send(w, (CuEvent){.type = CU_EV_TEXT, .text = text});
}
static void on_preedit_changed(GtkIMContext *im, CuWindow *w) {
    char *text = NULL;
    gtk_im_context_get_preedit_string(im, &text, NULL, NULL);
    w->preediting = text && *text;
    send(w, (CuEvent){.type = CU_EV_PREEDIT, .text = text ? text : ""});
    g_free(text);
}
static gboolean on_focus(GtkWidget *widget, GdkEventFocus *ev, CuWindow *w) {
    (void)widget;
    if (ev->in)
        gtk_im_context_focus_in(w->im);
    else {
        gtk_im_context_focus_out(w->im);
        w->held_key = 0;
    }
    send(w, (CuEvent){.type = CU_EV_FOCUS, .focused = ev->in});
    return FALSE;
}
static gboolean on_delete(GtkWidget *widget, GdkEvent *ev, CuWindow *w) {
    (void)widget, (void)ev;
    send(w, (CuEvent){.type = CU_EV_CLOSE});
    return TRUE; /* the application decides */
}
static void on_realize(GtkWidget *widget, CuWindow *w) {
    gtk_im_context_set_client_window(w->im, gtk_widget_get_window(widget));
}
static void on_window_realize(GtkWidget *widget, CuWindow *w) {
#ifdef GDK_WINDOWING_WAYLAND
    /* GTK only announces client-side decorations for windows it decorates itself, so
     * compositors such as KWin would otherwise add a title bar above Chela's own. */
    GdkWindow *window = gtk_widget_get_window(widget);
    if (w->csd && GDK_IS_WAYLAND_WINDOW(window))
        gdk_wayland_window_announce_csd(window);
#else
    (void)widget, (void)w;
#endif
}

/* Windows */
static bool init(int *argc, char ***argv) {
    gk.timers = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    return gtk_init_check(argc, argv);
}
static CuWindow *window_new(const char *title, const char *app_id, int width, int height, bool csd,
                            const CuWindowHandler *handler, void *data) {
    CuWindow *w = ch_new0(CuWindow, 1);
    w->handler = handler;
    w->data = data;
    w->starting = true;
    const char *mode = ch_getenv("CHELA_DECORATIONS");
    w->csd = csd && !(mode && !strcmp(mode, "server"));
    g_set_prgname(app_id); /* the Wayland app id and X11 class */
    w->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(w->window), title);
    gtk_window_set_icon_name(GTK_WINDOW(w->window), app_id);
    gtk_window_set_default_size(GTK_WINDOW(w->window), width, height);
    gtk_window_set_decorated(GTK_WINDOW(w->window), !w->csd);
    w->area = gtk_drawing_area_new();
    gtk_widget_set_can_focus(w->area, TRUE);
    gtk_widget_add_events(w->area, GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                                       GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_LEAVE_NOTIFY_MASK |
                                       GDK_KEY_PRESS_MASK | GDK_KEY_RELEASE_MASK | GDK_FOCUS_CHANGE_MASK);
    gtk_container_add(GTK_CONTAINER(w->window), w->area);
    w->im = gtk_im_multicontext_new();
    g_signal_connect(w->im, "commit", G_CALLBACK(on_commit), w);
    g_signal_connect(w->im, "preedit-changed", G_CALLBACK(on_preedit_changed), w);
    g_signal_connect(w->area, "realize", G_CALLBACK(on_realize), w);
    g_signal_connect(w->window, "realize", G_CALLBACK(on_window_realize), w);
    g_signal_connect(w->area, "draw", G_CALLBACK(on_draw), w);
    g_signal_connect(w->area, "size-allocate", G_CALLBACK(on_size_allocate), w);
    g_signal_connect(w->area, "motion-notify-event", G_CALLBACK(on_motion), w);
    g_signal_connect(w->area, "leave-notify-event", G_CALLBACK(on_leave), w);
    g_signal_connect(w->area, "button-press-event", G_CALLBACK(on_button), w);
    g_signal_connect(w->area, "button-release-event", G_CALLBACK(on_button), w);
    g_signal_connect(w->area, "scroll-event", G_CALLBACK(on_scroll), w);
    static const GtkTargetEntry drops[] = {{(char *)"text/uri-list", 0, 0}, {(char *)"image/png", 0, 1}, {(char *)"image/jpeg", 0, 2},
                                           {(char *)"text/plain;charset=utf-8", 0, 3}, {(char *)"text/plain", 0, 4}};
    gtk_drag_dest_set(w->area, GTK_DEST_DEFAULT_ALL, drops, G_N_ELEMENTS(drops), GDK_ACTION_COPY);
    g_signal_connect(w->area, "drag-motion", G_CALLBACK(on_drag_motion), w);
    g_signal_connect(w->area, "drag-leave", G_CALLBACK(on_drag_leave), w);
    g_signal_connect(w->area, "drag-data-received", G_CALLBACK(on_drag_received), w);
    g_signal_connect(w->window, "key-press-event", G_CALLBACK(on_key_press), w);
    g_signal_connect(w->window, "key-release-event", G_CALLBACK(on_key_release), w);
    g_signal_connect(w->window, "focus-in-event", G_CALLBACK(on_focus), w);
    g_signal_connect(w->window, "focus-out-event", G_CALLBACK(on_focus), w);
    g_signal_connect(w->window, "delete-event", G_CALLBACK(on_delete), w);
    gtk_widget_show_all(w->window);
    gtk_widget_grab_focus(w->area);
    w->starting = false;
    return w;
}
static void window_free(CuWindow *w) {
    gtk_im_context_set_client_window(w->im, NULL);
    g_object_unref(w->im);
    gtk_widget_destroy(w->window);
    free(w);
}
static void window_set_title(CuWindow *w, const char *title) {
    gtk_window_set_title(GTK_WINDOW(w->window), title);
}
static void window_set_min_size(CuWindow *w, int width, int height) {
    gtk_widget_set_size_request(w->area, width, height);
}
static void window_redraw(CuWindow *w) {
    gtk_widget_queue_draw(w->area);
}
static void window_set_cursor(CuWindow *w, CuCursor cursor) {
    static const char *names[] = {"default",  "pointer",  "text",      "move",      "n-resize",  "s-resize",
                                  "e-resize", "w-resize", "ne-resize", "nw-resize", "se-resize", "sw-resize"};
    GdkWindow *window = gtk_widget_get_window(w->area);
    if (!window || (size_t)cursor >= CH_N_ELEMENTS(names))
        return;
    GdkCursor *c = gdk_cursor_new_from_name(gdk_window_get_display(window), names[cursor]);
    gdk_window_set_cursor(window, c);
    if (c)
        g_object_unref(c);
}
static void window_minimize(CuWindow *w) {
    gtk_window_iconify(GTK_WINDOW(w->window));
}
static bool window_is_maximized(CuWindow *w) {
    return gtk_window_is_maximized(GTK_WINDOW(w->window));
}
static void window_toggle_maximize(CuWindow *w) {
    if (window_is_maximized(w))
        gtk_window_unmaximize(GTK_WINDOW(w->window));
    else
        gtk_window_maximize(GTK_WINDOW(w->window));
}
static void window_begin_move(CuWindow *w) {
    gtk_window_begin_move_drag(GTK_WINDOW(w->window), (gint)w->press_button, (gint)w->press_root_x,
                               (gint)w->press_root_y, w->press_time);
}
static void window_begin_resize(CuWindow *w, CuEdge edge) {
    static const GdkWindowEdge edges[] = {GDK_WINDOW_EDGE_NORTH,      GDK_WINDOW_EDGE_NORTH,
                                          GDK_WINDOW_EDGE_SOUTH,      GDK_WINDOW_EDGE_EAST,
                                          GDK_WINDOW_EDGE_WEST,       GDK_WINDOW_EDGE_NORTH_EAST,
                                          GDK_WINDOW_EDGE_NORTH_WEST, GDK_WINDOW_EDGE_SOUTH_EAST,
                                          GDK_WINDOW_EDGE_SOUTH_WEST};
    if (edge && (size_t)edge < CH_N_ELEMENTS(edges))
        gtk_window_begin_resize_drag(GTK_WINDOW(w->window), edges[edge], (gint)w->press_button,
                                     (gint)w->press_root_x, (gint)w->press_root_y, w->press_time);
}
static bool window_has_client_decorations(CuWindow *w) {
    return w->csd;
}
static void window_set_text_input(CuWindow *w, bool enabled, CuRect caret) {
    if (enabled)
        gtk_im_context_set_cursor_location(w->im,
                                           &(GdkRectangle){(int)caret.x, (int)caret.y, 1, (int)ceilf(caret.h)});
}

/* Main loop */
static gboolean posted_run(gpointer data) {
    Posted *p = data;
    p->fn(p->data);
    return G_SOURCE_REMOVE;
}
static void post(void (*fn)(void *), void *data) {
    Posted *p = g_new(Posted, 1);
    *p = (Posted){fn, data};
    g_idle_add_full(G_PRIORITY_DEFAULT, posted_run, p, g_free); /* thread-safe; always deferred */
}
static gboolean timer_fire(gpointer data) {
    Timer *t = data;
    unsigned id = t->id;
    bool again = t->fn(t->data);
    if (!g_hash_table_contains(gk.timers, GUINT_TO_POINTER(id)))
        return G_SOURCE_REMOVE; /* the callback removed it */
    if (!again)
        g_hash_table_remove(gk.timers, GUINT_TO_POINTER(id));
    return again ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
}
static unsigned timer_add(int ms, bool (*fn)(void *), void *data) {
    Timer *t = g_new0(Timer, 1);
    *t = (Timer){++gk.next_timer, 0, fn, data};
    t->source = g_timeout_add(ms > 0 ? (guint)ms : 0, timer_fire, t);
    g_hash_table_insert(gk.timers, GUINT_TO_POINTER(t->id), t);
    return t->id;
}
static void timer_remove(unsigned id) {
    Timer *t = g_hash_table_lookup(gk.timers, GUINT_TO_POINTER(id));
    if (t) {
        g_source_remove(t->source);
        g_hash_table_remove(gk.timers, GUINT_TO_POINTER(id));
    }
}
static void run(void) {
    if (!gk.quit_pending)
        gtk_main();
    gk.quit_pending = false;
}
static void quit(void) {
    if (gtk_main_level())
        gtk_main_quit();
    else
        gk.quit_pending = true;
}
static void shutdown_backend(void) {
    if (gk.timers) {
        GHashTableIter it;
        gpointer value;
        g_hash_table_iter_init(&it, gk.timers);
        while (g_hash_table_iter_next(&it, NULL, &value))
            g_source_remove(((Timer *)value)->source);
        g_hash_table_destroy(gk.timers);
        gk.timers = NULL;
    }
    cc_shutdown();
}

/* Clipboard */
static void clipboard_set(CuWindow *w, const char *text) {
    (void)w;
    gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), text, -1);
}
static void clipboard_received(GtkClipboard *clipboard, const char *text, gpointer data) {
    (void)clipboard;
    Request *r = data;
    r->callback(text, r->data);
    g_free(r);
}
static void clipboard_request(CuWindow *w, void (*callback)(const char *, void *), void *data) {
    (void)w;
    Request *r = g_new(Request, 1);
    *r = (Request){callback, data};
    gtk_clipboard_request_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), clipboard_received, r);
}

typedef struct {
    void (*callback)(const char *, const void *, size_t, void *);
    void *data;
    char *mime;
} DataRequest;
static void data_received(GtkClipboard *clipboard, GtkSelectionData *selection, gpointer user) {
    (void)clipboard;
    DataRequest *r = user;
    gint length = 0;
    const guchar *bytes = gtk_selection_data_get_data_with_length(selection, &length);
    r->callback(bytes && length > 0 ? r->mime : NULL, bytes, length > 0 ? (size_t)length : 0, r->data);
    g_free(r->mime);
    g_free(r);
}
static void targets_received(GtkClipboard *clipboard, GdkAtom *atoms, gint count, gpointer user) {
    DataRequest *r = user;
    const char **mimes = g_new0(const char *, (gsize)count + 1);
    char **names = g_new0(char *, (gsize)count + 1);
    for (gint i = 0; i < count; i++)
        mimes[i] = names[i] = gdk_atom_name(atoms[i]);
    int best = atoms ? ch_clipboard_best(mimes, count) : -1;
    if (best < 0) {
        r->callback(NULL, NULL, 0, r->data);
        g_free(r);
    } else {
        r->mime = g_strdup(names[best]);
        gtk_clipboard_request_contents(clipboard, atoms[best], data_received, r);
    }
    g_strfreev(names);
    g_free(mimes);
}
static void clipboard_request_data(CuWindow *w, void (*callback)(const char *, const void *, size_t, void *), void *data) {
    (void)w;
    DataRequest *r = g_new0(DataRequest, 1);
    r->callback = callback;
    r->data = data;
    gtk_clipboard_request_targets(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), targets_received, r);
}

/* Native file dialogs (the desktop portal when available) */
static void file_response(GtkNativeDialog *dialog, gint response, gpointer data) {
    Request *r = data;
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    char *path = response == GTK_RESPONSE_ACCEPT ? gtk_file_chooser_get_filename(chooser) : NULL;
    r->callback(path, r->data);
    g_free(path);
    g_free(r);
    g_object_unref(dialog);
}
static void file_dialog(CuWindow *parent, int kind, const char *title, const char *folder, const char *name,
                        void (*callback)(const char *, void *), void *data) {
    static const GtkFileChooserAction actions[] = {GTK_FILE_CHOOSER_ACTION_OPEN,
                                                   GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
                                                   GTK_FILE_CHOOSER_ACTION_SAVE};
    static const char *accept[] = {"_Open", "_Select", "_Save"};
    kind = CH_CLAMP(kind, 0, 2);
    GtkFileChooserNative *dialog =
        gtk_file_chooser_native_new(title, GTK_WINDOW(parent->window), actions[kind], accept[kind], "_Cancel");
    GtkFileChooser *chooser = GTK_FILE_CHOOSER(dialog);
    if (folder && *folder)
        gtk_file_chooser_set_current_folder(chooser, folder);
    if (kind == 2) {
        gtk_file_chooser_set_do_overwrite_confirmation(chooser, TRUE);
        if (name && *name)
            gtk_file_chooser_set_current_name(chooser, name);
    }
    Request *r = g_new(Request, 1);
    *r = (Request){callback, data};
    g_signal_connect(dialog, "response", G_CALLBACK(file_response), r);
    gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(dialog), TRUE);
    gtk_native_dialog_show(GTK_NATIVE_DIALOG(dialog));
}

__attribute__((visibility("default"))) const CuPlatform *chela_platform(void) {
    static CuPlatform p;
    if (!p.abi) {
        p.abi = CU_PLATFORM_ABI;
        p.name = "gtk";
        p.init = init;
        p.run = run;
        p.quit = quit;
        p.shutdown = shutdown_backend;
        p.window_new = window_new;
        p.window_free = window_free;
        p.window_set_title = window_set_title;
        p.window_set_min_size = window_set_min_size;
        p.window_redraw = window_redraw;
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
        p.file_dialog = file_dialog;
        cc_install(&p);
    }
    return &p;
}
