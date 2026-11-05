/* Headless backend: renders offscreen with cairo and drives the interface from a
 * script, for automated tests and reference screenshots without a display.
 *
 * Script lines (CHELA_HEADLESS_SCRIPT=file, or --script file):
 *   size W H | scale S | wait MS | idle | move X Y | click X Y [BUTTON] | dblclick X Y
 *   tripleclick X Y | rclick X Y | press X Y | release X Y | scroll X Y DY | key [ctrl+][shift+][alt+]NAME
 *   type TEXT | tap TEXT (click the element showing TEXT) | rtap TEXT | ctap TEXT (with Ctrl held) | hover TEXT
 *   expect TEXT | expect-not TEXT | until TEXT | until-not TEXT | shot PATH | clipboard TEXT
 *   copy-data MIME PATH (the clipboard then offers that file's bytes as MIME)
 *   copy-text TEXT | drag on|off | drop MIME PATH | drop-files PATH… | dump | log MESSAGE | quit
 *   clipboard-get (reports the clipboard) | place TEXT (reports "X Y W H" of the element showing TEXT)
 * expect checks once the interface is idle; until waits up to 20 s, for work such as
 * a server's reply. A failed expectation exits with status 3.
 *
 * Interactive control (CHELA_HEADLESS_CONTROL=unix socket path): the process connects
 * to a test harness, announces each window as "@window TITLE" and then runs one
 * command per line, answering "@ok" or "@fail MESSAGE"; "dump" first sends each
 * visible text as "@text TEXT" with newlines escaped. Every Chela process started with
 * the variable connects, including SSH askpass prompts. */
#include "cairo_canvas.h"
#include "headless.h"
#include "../core/base.h"
#include <cairo.h>
#include <ctype.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct CuWindow {
    int width, height;
    float scale;
    const CuWindowHandler *handler;
    void *data;
    cairo_surface_t *surface;
    bool dirty, maximized;
    char *title;
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
static struct {
    CuWindow *windows[8];
    int window_count;
    Timer timers[128];
    int timer_count;
    unsigned next_timer;
    ChMutex lock;
    ChCond wake;
    Posted *posted, *posted_tail;
    bool quit;
    int status;
    char *clipboard;
    char *clip_mime, *clip_data; /* a non-text clipboard, from copy-data */
    size_t clip_size;
    char *script;
    HeadlessFinder finder;
    HeadlessLister lister;
    void *finder_data;
    float mouse_x, mouse_y;
    int control; /* harness socket, or -1 */
    ChBuf input;
    char *failure; /* the current interactive command's failure */
} hl;

static void reply(const char *format, ...) CH_PRINTF(1, 2);
static void reply(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *line = ch_strdup_vprintf(format, args);
    va_end(args);
    for (char *p = line; *p; p++)
        if (*p == '\n')
            *p = ' ';
    ChBuf out;
    ch_buf_init(&out);
    ch_buf_printf(&out, "%s\n", line);
    for (size_t sent = 0; sent < out.len;) {
        ssize_t n = write(hl.control, out.str + sent, out.len - sent);
        if (n <= 0)
            break;
        sent += (size_t)n;
    }
    ch_buf_clear(&out);
    free(line);
}
static int connect_control(const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path))
        return -1;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static bool init(int *argc, char ***argv) {
    ch_mutex_init(&hl.lock);
    ch_cond_init(&hl.wake);
    hl.control = -1;
    ch_buf_init(&hl.input);
    const char *script = ch_getenv("CHELA_HEADLESS_SCRIPT");
    for (int i = 1; i < *argc; i++)
        if (!strcmp((*argv)[i], "--script") && i + 1 < *argc) {
            script = (*argv)[i + 1];
            for (int j = i; j + 2 <= *argc; j++)
                (*argv)[j] = (*argv)[j + 2];
            *argc -= 2;
            break;
        }
    if (script) {
        char *text = NULL;
        if (!ch_file_get_contents(script, &text, NULL, NULL)) {
            fprintf(stderr, "chela: cannot read script %s\n", script);
            return false;
        }
        hl.script = text;
    }
    const char *control = ch_getenv("CHELA_HEADLESS_CONTROL");
    if (control && !hl.script && (hl.control = connect_control(control)) < 0) {
        fprintf(stderr, "chela: cannot connect to the test harness at %s\n", control);
        return false;
    }
    return true;
}
void headless_set_finder(HeadlessFinder finder, HeadlessLister lister, void *data) {
    hl.finder = finder;
    hl.lister = lister;
    hl.finder_data = data;
}
static CuWindow *window_new(const char *title, const char *app_id, int width, int height, bool csd,
                            const CuWindowHandler *handler, void *data) {
    (void)app_id, (void)csd;
    CuWindow *w = ch_new0(CuWindow, 1);
    w->width = width;
    w->height = height;
    w->scale = 1;
    const char *scale = ch_getenv("CHELA_HEADLESS_SCALE");
    if (scale)
        w->scale = CH_CLAMP(strtof(scale, NULL), 0.5f, 4.0f);
    w->handler = handler;
    w->data = data;
    w->dirty = true;
    w->title = ch_strdup(title);
    if (hl.window_count < 8)
        hl.windows[hl.window_count++] = w;
    if (hl.control >= 0)
        reply("@window %s", title);
    return w;
}
static void window_free(CuWindow *w) {
    for (int i = 0; i < hl.window_count; i++)
        if (hl.windows[i] == w) {
            memmove(hl.windows + i, hl.windows + i + 1, sizeof(CuWindow *) * (size_t)(hl.window_count - i - 1));
            hl.window_count--;
        }
    if (w->surface)
        cairo_surface_destroy(w->surface);
    free(w->title);
    free(w);
}
static void window_set_title(CuWindow *w, const char *title) {
    free(w->title);
    w->title = ch_strdup(title);
}
static void window_redraw(CuWindow *w) {
    w->dirty = true;
}
static void paint(CuWindow *w) {
    int pw = (int)ceilf((float)w->width * w->scale), ph = (int)ceilf((float)w->height * w->scale);
    if (!w->surface || cairo_image_surface_get_width(w->surface) != pw || cairo_image_surface_get_height(w->surface) != ph) {
        if (w->surface)
            cairo_surface_destroy(w->surface);
        w->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pw, ph);
    }
    cairo_t *cr = cairo_create(w->surface);
    cairo_scale(cr, w->scale, w->scale);
    CuCanvas canvas = {cr};
    w->dirty = false;
    w->handler->paint(w, &canvas, w->width, w->height, w->data);
    cairo_destroy(cr);
    cairo_surface_flush(w->surface);
}
static bool snapshot(CuWindow *w, const char *path) {
    if (w->dirty || !w->surface)
        paint(w);
    return cairo_surface_write_to_png(w->surface, path) == CAIRO_STATUS_SUCCESS;
}
static void post(void (*fn)(void *), void *data) {
    Posted *p = ch_new0(Posted, 1);
    p->fn = fn;
    p->data = data;
    ch_mutex_lock(&hl.lock);
    if (hl.posted_tail)
        hl.posted_tail->next = p;
    else
        hl.posted = p;
    hl.posted_tail = p;
    ch_cond_signal(&hl.wake);
    ch_mutex_unlock(&hl.lock);
}
static unsigned timer_add(int ms, bool (*fn)(void *), void *data) {
    if (hl.timer_count >= (int)CH_N_ELEMENTS(hl.timers))
        return 0;
    Timer *t = &hl.timers[hl.timer_count++];
    *t = (Timer){++hl.next_timer, ch_monotonic_us() + (int64_t)ms * 1000, ms, fn, data};
    return t->id;
}
static void timer_remove(unsigned id) {
    for (int i = 0; i < hl.timer_count; i++)
        if (hl.timers[i].id == id) {
            hl.timers[i] = hl.timers[--hl.timer_count];
            return;
        }
}
static void clipboard_set(CuWindow *w, const char *text) {
    (void)w;
    free(hl.clipboard);
    ch_clear_pointer(&hl.clip_mime, free);
    ch_clear_pointer(&hl.clip_data, free);
    hl.clipboard = ch_strdup(text);
}
static void clipboard_request_data(CuWindow *w, void (*cb)(const char *, const void *, size_t, void *), void *data) {
    (void)w;
    if (hl.clip_mime)
        cb(hl.clip_mime, hl.clip_data, hl.clip_size, data);
    else if (hl.clipboard)
        cb("text/plain", hl.clipboard, strlen(hl.clipboard), data);
    else
        cb(NULL, NULL, 0, data);
}
static void clipboard_request(CuWindow *w, void (*cb)(const char *, void *), void *data) {
    (void)w;
    cb(hl.clipboard, data);
}
static bool maximized(CuWindow *w) {
    return w->maximized;
}
static void toggle_maximize(CuWindow *w) {
    w->maximized = !w->maximized;
}
static void quit(void) {
    hl.quit = true;
}
/* One pass of the loop: posted work, due timers and repaints. Returns whether work ran. */
static bool iterate(int64_t wait_us) {
    bool worked = false;
    ch_mutex_lock(&hl.lock);
    if (!hl.posted && wait_us > 0) {
        int64_t next = ch_monotonic_us() + wait_us;
        for (int i = 0; i < hl.timer_count; i++)
            next = CH_MIN(next, hl.timers[i].due);
        for (int i = 0; i < hl.window_count; i++)
            if (hl.windows[i]->dirty)
                next = ch_monotonic_us();
        int64_t delay = next - ch_monotonic_us();
        if (delay > 0) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            int64_t ns = ts.tv_nsec + delay * 1000;
            ts.tv_sec += ns / 1000000000;
            ts.tv_nsec = ns % 1000000000;
            pthread_cond_timedwait(&hl.wake, &hl.lock, &ts);
        }
    }
    Posted *p = hl.posted;
    hl.posted = hl.posted_tail = NULL;
    ch_mutex_unlock(&hl.lock);
    while (p) {
        Posted *next = p->next;
        p->fn(p->data);
        free(p);
        p = next;
        worked = true;
    }
    int64_t now = ch_monotonic_us();
    for (int i = 0; i < hl.timer_count; i++) {
        Timer t = hl.timers[i];
        if (t.due > now)
            continue;
        bool again = t.fn(t.data);
        worked = true;
        for (int k = 0; k < hl.timer_count; k++)
            if (hl.timers[k].id == t.id) {
                if (again)
                    hl.timers[k].due = now + (int64_t)t.interval * 1000;
                else
                    hl.timers[k] = hl.timers[--hl.timer_count];
                break;
            }
        i = -1; /* the list may have changed */
        now = ch_monotonic_us();
        if (hl.quit)
            break;
    }
    for (int i = 0; i < hl.window_count; i++)
        if (hl.windows[i]->dirty) {
            paint(hl.windows[i]);
            worked = true;
        }
    return worked;
}
static void run_for(int64_t us) {
    int64_t end = ch_monotonic_us() + us;
    while (!hl.quit && ch_monotonic_us() < end)
        iterate(CH_MIN(end - ch_monotonic_us(), 20000));
}
static void settle(void) {
    /* Until nothing happens for 150 ms (timers firing every second still count as idle). */
    int64_t quiet = ch_monotonic_us(), limit = quiet + 15 * CH_USEC_PER_SEC;
    while (!hl.quit && ch_monotonic_us() < limit) {
        bool dirty = false;
        for (int i = 0; i < hl.window_count; i++)
            dirty |= hl.windows[i]->dirty;
        ch_mutex_lock(&hl.lock);
        bool posted = hl.posted != NULL;
        ch_mutex_unlock(&hl.lock);
        if (dirty || posted)
            quiet = ch_monotonic_us();
        iterate(10000);
        if (ch_monotonic_us() - quiet > 150000)
            break;
    }
}
static CuWindow *top(void) {
    return hl.window_count ? hl.windows[hl.window_count - 1] : NULL;
}
static void deliver(CuEvent ev) {
    CuWindow *w = top();
    if (w)
        w->handler->event(w, &ev, w->data);
    iterate(0);
}
static int key_code(const char *name, int *mods) {
    *mods = 0;
    for (;;) {
        if (ch_str_has_prefix(name, "ctrl+"))
            *mods |= CU_MOD_CTRL, name += 5;
        else if (ch_str_has_prefix(name, "shift+"))
            *mods |= CU_MOD_SHIFT, name += 6;
        else if (ch_str_has_prefix(name, "alt+"))
            *mods |= CU_MOD_ALT, name += 4;
        else
            break;
    }
    static const struct {
        const char *name;
        int key;
    } names[] = {{"return", CU_KEY_RETURN}, {"enter", CU_KEY_RETURN}, {"escape", CU_KEY_ESCAPE},
                 {"tab", CU_KEY_TAB}, {"backspace", CU_KEY_BACKSPACE}, {"delete", CU_KEY_DELETE},
                 {"left", CU_KEY_LEFT}, {"right", CU_KEY_RIGHT}, {"up", CU_KEY_UP}, {"down", CU_KEY_DOWN},
                 {"home", CU_KEY_HOME}, {"end", CU_KEY_END}, {"pageup", CU_KEY_PAGE_UP},
                 {"pagedown", CU_KEY_PAGE_DOWN}, {"space", ' '}, {"comma", ','}};
    for (size_t i = 0; i < CH_N_ELEMENTS(names); i++)
        if (!strcmp(name, names[i].name))
            return names[i].key;
    return (int)ch_utf8_get(name, NULL);
}
static void fail(const char *line, const char *why) {
    if (hl.control >= 0) {
        free(hl.failure);
        hl.failure = ch_strdup_printf("%s: %s", why, line);
        return;
    }
    fprintf(stderr, "chela headless: %s: %s\n", why, line);
    hl.status = 3;
    hl.quit = true;
}
static void emit_text(const char *text, void *data) {
    (void)data;
    ChBuf escaped;
    ch_buf_init(&escaped);
    for (const char *p = text; *p; p++)
        if (*p == '\n')
            ch_buf_append(&escaped, "\\n");
        else if (*p == '\\')
            ch_buf_append(&escaped, "\\\\");
        else
            ch_buf_append_c(&escaped, *p);
    if (hl.control >= 0)
        reply("@text %s", escaped.str);
    else
        fprintf(stderr, "# %s\n", escaped.str);
    ch_buf_clear(&escaped);
}
static bool find(const char *text, CuRect *rect) {
    return hl.finder && hl.finder(hl.finder_data, text, rect);
}
static void pointer(CuEventType type, float x, float y, int button) {
    hl.mouse_x = x;
    hl.mouse_y = y;
    deliver((CuEvent){.type = CU_EV_MOTION, .x = x, .y = y});
    if (type != CU_EV_MOTION)
        deliver((CuEvent){.type = type, .x = x, .y = y, .button = button});
}
static void click(float x, float y, int button, int count) {
    for (int i = 0; i < count; i++) {
        pointer(CU_EV_PRESS, x, y, button);
        pointer(CU_EV_RELEASE, x, y, button);
    }
}
static void control_click(float x, float y) {
    pointer(CU_EV_MOTION, x, y, 0);
    deliver((CuEvent){.type = CU_EV_PRESS, .x = x, .y = y, .button = 1, .mods = CU_MOD_CTRL});
    deliver((CuEvent){.type = CU_EV_RELEASE, .x = x, .y = y, .button = 1, .mods = CU_MOD_CTRL});
}
static void step(char *line) {
    char *cmd = line, *rest = strchr(line, ' ');
    if (rest)
        *rest++ = 0;
    else
        rest = "";
    float a = 0, b = 0, c = 0;
    if (!strcmp(cmd, "size") && sscanf(rest, "%f %f", &a, &b) == 2) {
        CuWindow *w = top();
        if (w) {
            w->width = (int)a;
            w->height = (int)b;
            deliver((CuEvent){.type = CU_EV_RESIZE, .width = (int)a, .height = (int)b, .scale = w->scale});
            w->dirty = true;
        }
    } else if (!strcmp(cmd, "scale") && sscanf(rest, "%f", &a) == 1) {
        for (int i = 0; i < hl.window_count; i++) {
            hl.windows[i]->scale = a;
            hl.windows[i]->dirty = true;
        }
    } else if (!strcmp(cmd, "wait"))
        run_for((int64_t)atoi(rest) * 1000);
    else if (!strcmp(cmd, "idle"))
        settle();
    else if (!strcmp(cmd, "move") && sscanf(rest, "%f %f", &a, &b) == 2)
        pointer(CU_EV_MOTION, a, b, 0);
    else if (!strcmp(cmd, "click") && sscanf(rest, "%f %f %f", &a, &b, &c) >= 2)
        click(a, b, c > 0 ? (int)c : 1, 1);
    else if (!strcmp(cmd, "dblclick") && sscanf(rest, "%f %f", &a, &b) == 2)
        click(a, b, 1, 2);
    else if (!strcmp(cmd, "tripleclick") && sscanf(rest, "%f %f", &a, &b) == 2)
        click(a, b, 1, 3);
    else if (!strcmp(cmd, "rclick") && sscanf(rest, "%f %f", &a, &b) == 2)
        click(a, b, 3, 1);
    else if (!strcmp(cmd, "press") && sscanf(rest, "%f %f", &a, &b) == 2)
        pointer(CU_EV_PRESS, a, b, 1);
    else if (!strcmp(cmd, "release") && sscanf(rest, "%f %f", &a, &b) == 2)
        pointer(CU_EV_RELEASE, a, b, 1);
    else if (!strcmp(cmd, "scroll") && sscanf(rest, "%f %f %f", &a, &b, &c) == 3) {
        pointer(CU_EV_MOTION, a, b, 0);
        deliver((CuEvent){.type = CU_EV_SCROLL, .x = a, .y = b, .dy = c});
    } else if (!strcmp(cmd, "key")) {
        int mods;
        int k = key_code(rest, &mods);
        deliver((CuEvent){.type = CU_EV_KEY, .key = k, .mods = mods});
        if (k < 0x110000 && !(mods & (CU_MOD_CTRL | CU_MOD_ALT))) {
            char out[5] = {0};
            out[ch_utf8_encode((uint32_t)k, out)] = 0;
            deliver((CuEvent){.type = CU_EV_TEXT, .text = out});
        }
    } else if (!strcmp(cmd, "type")) {
        for (char *p = rest; *p; p++)
            if (p[0] == '\\' && p[1] == 'n') {
                p[0] = '\n';
                memmove(p + 1, p + 2, strlen(p + 2) + 1);
            }
        deliver((CuEvent){.type = CU_EV_TEXT, .text = rest});
    } else if (!strcmp(cmd, "tap") || !strcmp(cmd, "rtap") || !strcmp(cmd, "ctap") || !strcmp(cmd, "hover")) {
        CuRect r;
        if (!find(rest, &r)) {
            fail(rest, "no element shows this text");
            return;
        }
        float x = r.x + r.w / 2, y = r.y + r.h / 2;
        if (!strcmp(cmd, "hover"))
            pointer(CU_EV_MOTION, x, y, 0);
        else if (cmd[0] == 'c')
            control_click(x, y);
        else
            click(x, y, cmd[0] == 'r' ? 3 : 1, 1);
    } else if (!strcmp(cmd, "expect") || !strcmp(cmd, "expect-not")) {
        settle();
        CuRect r;
        bool found = find(rest, &r);
        char where[96];
        snprintf(where, sizeof(where), "unexpected text is visible at %.0f,%.0f %.0fx%.0f", r.x, r.y, r.w, r.h);
        if (found != !strcmp(cmd, "expect"))
            fail(rest, found ? where : "expected text is not visible");
    } else if (!strcmp(cmd, "until") || !strcmp(cmd, "until-not")) {
        bool want = !strcmp(cmd, "until");
        int64_t limit = ch_monotonic_us() + 20 * CH_USEC_PER_SEC;
        CuRect r;
        while (!hl.quit && find(rest, &r) != want && ch_monotonic_us() < limit)
            iterate(20000);
        if (!hl.quit && find(rest, &r) != want)
            fail(rest, want ? "text did not appear in time" : "text did not go away in time");
    } else if (!strcmp(cmd, "shot")) {
        settle();
        CuWindow *w = top();
        if (!w || !snapshot(w, rest))
            fail(rest, "cannot write screenshot");
    } else if (!strcmp(cmd, "copy-text")) {
        ch_clear_pointer(&hl.clip_mime, free);
        ch_clear_pointer(&hl.clip_data, free);
        free(hl.clipboard);
    free(hl.clip_mime);
    free(hl.clip_data);
    hl.clip_mime = hl.clip_data = NULL;
        hl.clipboard = ch_strdup(rest);
    } else if (!strcmp(cmd, "copy-data") || !strcmp(cmd, "drop")) {
        char *path = strchr(rest, ' '), *bytes = NULL;
        size_t size = 0;
        if (path)
            *path++ = 0;
        if (!path || !ch_file_get_contents(path, &bytes, &size, NULL))
            fail(rest, "needs a MIME type and a readable file");
        else if (!strcmp(cmd, "drop"))
            deliver((CuEvent){.type = CU_EV_DROP, .mime = rest, .data = bytes, .size = size});
        else {
            free(hl.clip_mime);
            free(hl.clip_data);
            hl.clip_mime = ch_strdup(rest);
            hl.clip_data = ch_steal(&bytes);
            hl.clip_size = size;
        }
        free(bytes);
    } else if (!strcmp(cmd, "drop-files")) {
        ChBuf uris;
        ch_buf_init(&uris);
        char **paths = ch_strsplit(rest, " ");
        for (size_t i = 0; paths[i]; i++)
            ch_buf_printf(&uris, "file://%s\r\n", paths[i]);
        ch_strfreev(paths);
        deliver((CuEvent){.type = CU_EV_DROP, .mime = "text/uri-list", .data = uris.str, .size = uris.len});
        ch_buf_clear(&uris);
    } else if (!strcmp(cmd, "drag")) {
        deliver((CuEvent){.type = CU_EV_DRAG, .focused = !strcmp(rest, "on"), .x = hl.mouse_x, .y = hl.mouse_y});
    } else if (!strcmp(cmd, "clipboard")) {
        if (ch_strcmp0(hl.clipboard, rest))
            fail(rest, "clipboard differs");
    } else if (!strcmp(cmd, "clipboard-get")) {
        settle();
        emit_text(hl.clipboard ? hl.clipboard : "", NULL);
    } else if (!strcmp(cmd, "place")) {
        settle();
        CuRect r;
        if (find(rest, &r)) {
            char *where = ch_strdup_printf("%.1f %.1f %.1f %.1f", r.x, r.y, r.w, r.h);
            emit_text(where, NULL);
            free(where);
        } else
            fail(rest, "no element shows this text");
    } else if (!strcmp(cmd, "dump")) {
        settle();
        if (hl.lister)
            hl.lister(hl.finder_data, emit_text, NULL);
    } else if (!strcmp(cmd, "log"))
        fprintf(stderr, "# %s\n", rest);
    else if (!strcmp(cmd, "quit"))
        hl.quit = true;
    else if (*cmd && *cmd != '#')
        fail(cmd, "unknown script command");
}
/* Commands from the harness, while the interface keeps running between them. */
static void run_control(void) {
    while (!hl.quit) {
        struct pollfd pfd = {hl.control, POLLIN, 0};
        if (poll(&pfd, 1, 0) > 0) {
            char chunk[4096];
            ssize_t n = read(hl.control, chunk, sizeof(chunk));
            if (n <= 0)
                break; /* the harness went away */
            ch_buf_append_len(&hl.input, chunk, (size_t)n);
        }
        char *end;
        while (!hl.quit && (end = memchr(hl.input.str, '\n', hl.input.len))) {
            char *line = ch_strndup(hl.input.str, (size_t)(end - hl.input.str));
            ch_buf_erase(&hl.input, 0, (size_t)(end - hl.input.str) + 1);
            char *command = line;
            while (isspace((unsigned char)*command))
                command++;
            step(command);
            iterate(0);
            if (hl.failure)
                reply("@fail %s", hl.failure);
            else
                reply("@ok");
            ch_clear_pointer(&hl.failure, free);
            free(line);
        }
        if (!hl.quit)
            iterate(20000);
    }
}
static void run(void) {
    settle();
    if (hl.control >= 0) {
        run_control();
        return;
    }
    if (hl.script) {
        char *save = NULL;
        for (char *line = strtok_r(hl.script, "\n", &save); line && !hl.quit; line = strtok_r(NULL, "\n", &save)) {
            while (isspace((unsigned char)*line))
                line++;
            step(line);
            iterate(0);
        }
        hl.quit = true;
        return;
    }
    while (!hl.quit)
        iterate(100000);
}
static void shutdown_backend(void) {
    free(hl.clipboard);
    free(hl.clip_mime);
    free(hl.clip_data);
    hl.clip_mime = hl.clip_data = NULL;
    free(hl.script);
    hl.clipboard = hl.script = NULL;
    if (hl.control >= 0) {
        close(hl.control);
        hl.control = -1;
    }
    ch_buf_clear(&hl.input);
    cc_shutdown();
}
int headless_status(void) {
    return hl.status;
}
static bool no_csd(CuWindow *w) {
    (void)w;
    return ch_getenv("CHELA_HEADLESS_CSD") != NULL; /* to review the window buttons */
}
const CuPlatform *cu_platform_headless(void) {
    static CuPlatform p;
    if (!p.abi) {
        p.abi = CU_PLATFORM_ABI;
        p.name = "headless";
        p.init = init;
        p.run = run;
        p.quit = quit;
        p.shutdown = shutdown_backend;
        p.window_new = window_new;
        p.window_free = window_free;
        p.window_set_title = window_set_title;
        p.window_redraw = window_redraw;
        p.window_toggle_maximize = toggle_maximize;
        p.window_is_maximized = maximized;
        p.window_has_client_decorations = no_csd;
        p.post = post;
        p.timer_add = timer_add;
        p.timer_remove = timer_remove;
        p.clipboard_set = clipboard_set;
        p.clipboard_request = clipboard_request;
        p.clipboard_request_data = clipboard_request_data;
        p.window_snapshot = snapshot;
        cc_install(&p);
    }
    return &p;
}
