/* Test client for tests/wayland_paste.py: opens a small window, presses the pointer on
 * it through KWin's fake input, and drags a text/uri-list onto the window underneath. */
#define _GNU_SOURCE
#include "fake-input-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct xdg_wm_base *wm_base;
static struct wl_data_device_manager *manager;
static struct org_kde_kwin_fake_input *input;
static struct wl_surface *surface;
static struct xdg_surface *xdg;
static bool configured, entered, finished;
static uint32_t press_serial;
static const char *uri_list;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
    (void)data, (void)version;
    if (!strcmp(interface, "wl_compositor"))
        compositor = wl_registry_bind(r, name, &wl_compositor_interface, 4);
    else if (!strcmp(interface, "wl_shm"))
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 5);
    else if (!strcmp(interface, "xdg_wm_base"))
        wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
    else if (!strcmp(interface, "wl_data_device_manager"))
        manager = wl_registry_bind(r, name, &wl_data_device_manager_interface, 3);
    else if (!strcmp(interface, org_kde_kwin_fake_input_interface.name))
        input = wl_registry_bind(r, name, &org_kde_kwin_fake_input_interface, 4);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data, (void)r, (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(b, serial);
}
static const struct xdg_wm_base_listener wm_listener = {wm_ping};
static void xdg_configure(void *data, struct xdg_surface *s, uint32_t serial) {
    (void)data;
    xdg_surface_ack_configure(s, serial);
    configured = true;
}
static const struct xdg_surface_listener xdg_listener = {xdg_configure};
static void pointer_enter(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *f, wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)p, (void)s, (void)x, (void)y;
    entered = f == surface;
}
static void pointer_leave(void *d, struct wl_pointer *p, uint32_t s, struct wl_surface *f) {
    (void)d, (void)p, (void)s, (void)f;
    entered = false;
}
static void pointer_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
    (void)d, (void)p, (void)t, (void)x, (void)y;
}
static void pointer_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t b, uint32_t state) {
    (void)d, (void)p, (void)t, (void)b;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        press_serial = serial;
}
static void pointer_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {
    (void)d, (void)p, (void)t, (void)a, (void)v;
}
static void pointer_frame(void *d, struct wl_pointer *p) {
    (void)d, (void)p;
}
static void axis_source(void *d, struct wl_pointer *p, uint32_t s) {
    (void)d, (void)p, (void)s;
}
static void axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {
    (void)d, (void)p, (void)t, (void)a;
}
static void axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {
    (void)d, (void)p, (void)a, (void)v;
}
static const struct wl_pointer_listener pointer_listener = {.enter = pointer_enter, .leave = pointer_leave, .motion = pointer_motion,
                                                            .button = pointer_button, .axis = pointer_axis, .frame = pointer_frame,
                                                            .axis_source = axis_source, .axis_stop = axis_stop, .axis_discrete = axis_discrete};
static void source_target(void *d, struct wl_data_source *s, const char *m) {
    (void)d, (void)s, (void)m;
}
static void source_send(void *d, struct wl_data_source *s, const char *mime, int32_t fd) {
    (void)d, (void)s, (void)mime;
    if (write(fd, uri_list, strlen(uri_list)) < 0)
        perror("write");
    close(fd);
}
static void source_cancelled(void *d, struct wl_data_source *s) {
    (void)d, (void)s;
    finished = true;
}
static void source_performed(void *d, struct wl_data_source *s) {
    (void)d, (void)s;
}
static void source_finished(void *d, struct wl_data_source *s) {
    (void)d, (void)s;
    finished = true;
}
static void source_action(void *d, struct wl_data_source *s, uint32_t a) {
    (void)d, (void)s, (void)a;
}
static const struct wl_data_source_listener source_listener = {source_target, source_send, source_cancelled,
                                                               source_performed, source_finished, source_action};
static void serve(struct wl_display *display, int milliseconds) {
    struct timespec start, now;
    clock_gettime(CLOCK_MONOTONIC, &start);
    for (;;) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        int passed = (int)((now.tv_sec - start.tv_sec) * 1000 + (now.tv_nsec - start.tv_nsec) / 1000000);
        if (passed >= milliseconds)
            return;
        wl_display_flush(display);
        struct pollfd p = {wl_display_get_fd(display), POLLIN, 0};
        if (poll(&p, 1, milliseconds - passed) > 0)
            wl_display_dispatch(display);
    }
}
static void move_to(struct wl_display *display, int x, int y) {
    org_kde_kwin_fake_input_pointer_motion_absolute(input, wl_fixed_from_int(x), wl_fixed_from_int(y));
    serve(display, 80);
}
static void button(struct wl_display *display, uint32_t state) {
    org_kde_kwin_fake_input_button(input, BTN_LEFT, state);
    serve(display, 120);
}
int main(int argc, char **argv) {
    if (argc < 2)
        return 2;
    uri_list = argv[1];
    struct wl_display *display = wl_display_connect(NULL);
    if (!display)
        return 2;
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!compositor || !shm || !seat || !wm_base || !manager || !input)
        return 2;
    org_kde_kwin_fake_input_authenticate(input, "chela-test", "drag test");
    xdg_wm_base_add_listener(wm_base, &wm_listener, NULL);
    surface = wl_compositor_create_surface(compositor);
    xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
    xdg_surface_add_listener(xdg, &xdg_listener, NULL);
    struct xdg_toplevel *toplevel = xdg_surface_get_toplevel(xdg);
    xdg_toplevel_set_title(toplevel, "drag source");
    wl_surface_commit(surface);
    while (!configured)
        wl_display_dispatch(display);
    const int side = 160, size = side * side * 4;
    int fd = memfd_create("drag", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, size))
        return 2;
    memset(mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0), 0x80, (size_t)size);
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, side, side, side * 4, WL_SHM_FORMAT_XRGB8888);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage(surface, 0, 0, side, side);
    wl_surface_commit(surface);
    pointer = wl_seat_get_pointer(seat);
    wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    serve(display, 400);
    /* Find our window: the compositor chose where it went. */
    int found_x = -1, found_y = -1;
    for (int y = 60; y < 800 && found_x < 0; y += 70)
        for (int x = 60; x < 1280 && found_x < 0; x += 70) {
            move_to(display, x, y);
            if (entered)
                found_x = x, found_y = y;
        }
    if (found_x < 0) {
        fprintf(stderr, "never saw the pointer on the drag source\n");
        return 3;
    }
    button(display, WL_POINTER_BUTTON_STATE_PRESSED);
    if (!press_serial) {
        fprintf(stderr, "no press serial\n");
        return 3;
    }
    struct wl_data_device *device = wl_data_device_manager_get_data_device(manager, seat);
    struct wl_data_source *source = wl_data_device_manager_create_data_source(manager);
    wl_data_source_add_listener(source, &source_listener, NULL);
    wl_data_source_offer(source, "text/uri-list");
    wl_data_source_set_actions(source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    wl_data_device_start_drag(device, source, surface, NULL, press_serial);
    serve(display, 200);
    /* Far from our own window, over Chela. */
    int target_x = found_x < 640 ? 980 : 300, target_y = 400;
    for (int step = 1; step <= 8; step++)
        move_to(display, found_x + (target_x - found_x) * step / 8, found_y + (target_y - found_y) * step / 8);
    serve(display, 400);
    button(display, WL_POINTER_BUTTON_STATE_RELEASED);
    for (int i = 0; i < 40 && !finished; i++)
        serve(display, 100);
    fprintf(stderr, "drag %s\n", finished ? "finished" : "did not finish");
    wl_display_disconnect(display);
    return 0;
}
