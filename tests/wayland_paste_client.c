/* Test client for tests/wayland_paste.py, run inside a nested compositor: owns the
 * clipboard through ext-data-control with the given MIME types, each backed by a
 * file, then presses the given keys through KWin's fake input. Exits after serving. */
#define _GNU_SOURCE
#include "ext-data-control-v1-client-protocol.h"
#include "fake-input-client-protocol.h"
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define KEY_LEFTCTRL 29
#define KEY_ENTER 28
#define KEY_V 47

static struct wl_seat *seat;
static struct ext_data_control_manager_v1 *manager;
static struct org_kde_kwin_fake_input *input;
static char **pairs;
static int pair_count;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
    (void)data, (void)version;
    if (!strcmp(interface, "wl_seat"))
        seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
    else if (!strcmp(interface, ext_data_control_manager_v1_interface.name))
        manager = wl_registry_bind(r, name, &ext_data_control_manager_v1_interface, 1);
    else if (!strcmp(interface, org_kde_kwin_fake_input_interface.name))
        input = wl_registry_bind(r, name, &org_kde_kwin_fake_input_interface, 4);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data, (void)r, (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void send_file(void *data, struct ext_data_control_source_v1 *source, const char *mime, int32_t fd) {
    (void)data, (void)source;
    for (int i = 0; i + 1 < pair_count; i += 2)
        if (!strcmp(pairs[i], mime)) {
            int file = open(pairs[i + 1], O_RDONLY);
            char chunk[65536];
            ssize_t n;
            while (file >= 0 && (n = read(file, chunk, sizeof(chunk))) > 0)
                if (write(fd, chunk, (size_t)n) < 0)
                    break;
            if (file >= 0)
                close(file);
        }
    close(fd);
}
static void cancelled(void *data, struct ext_data_control_source_v1 *source) {
    (void)data, (void)source;
}
static const struct ext_data_control_source_v1_listener source_listener = {send_file, cancelled};
static void sleep_serving(struct wl_display *display, int milliseconds) {
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
static void key(struct wl_display *display, int code, int pressed) {
    org_kde_kwin_fake_input_keyboard_key(input, (uint32_t)code, (uint32_t)pressed);
    wl_display_flush(display);
    sleep_serving(display, 60);
}
int main(int argc, char **argv) {
    /* wayland_paste_client {paste|return} [MIME FILE]… */
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "no compositor\n");
        return 2;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!seat || !manager || !input) {
        fprintf(stderr, "the compositor lacks ext-data-control or fake input\n");
        return 2;
    }
    org_kde_kwin_fake_input_authenticate(input, "chela-test", "paste test");
    pairs = argv + 2;
    pair_count = argc - 2;
    if (pair_count > 0) {
        struct ext_data_control_source_v1 *source = ext_data_control_manager_v1_create_data_source(manager);
        ext_data_control_source_v1_add_listener(source, &source_listener, NULL);
        for (int i = 0; i + 1 < pair_count; i += 2)
            ext_data_control_source_v1_offer(source, pairs[i]);
        struct ext_data_control_device_v1 *device = ext_data_control_manager_v1_get_data_device(manager, seat);
        ext_data_control_device_v1_set_selection(device, source);
        sleep_serving(display, 300);
    }
    if (!strcmp(argv[1], "paste")) {
        key(display, KEY_LEFTCTRL, 1);
        key(display, KEY_V, 1);
        key(display, KEY_V, 0);
        key(display, KEY_LEFTCTRL, 0);
    } else {
        key(display, KEY_ENTER, 1);
        key(display, KEY_ENTER, 0);
    }
    /* Stay alive to answer the paste. */
    sleep_serving(display, 900);
    wl_display_disconnect(display);
    return 0;
}
