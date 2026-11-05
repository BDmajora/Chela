/* Test client for tests/wayland_paste.py, run inside a nested compositor: plays a
 * list of pointer and keyboard steps through KWin's fake input, as a person would.
 *   click X Y | press X Y | move X Y | release | wheel DY | key ctrl+a | key return | wait MS */
#define _GNU_SOURCE
#include "fake-input-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#define BTN_LEFT 0x110
#define KEY_LEFTCTRL 29
#define KEY_ENTER 28

static struct org_kde_kwin_fake_input *input;
static struct wl_display *display;

static void global(void *data, struct wl_registry *r, uint32_t name, const char *interface, uint32_t version) {
    (void)data, (void)version;
    if (!strcmp(interface, org_kde_kwin_fake_input_interface.name))
        input = wl_registry_bind(r, name, &org_kde_kwin_fake_input_interface, 4);
}
static void global_remove(void *data, struct wl_registry *r, uint32_t name) {
    (void)data, (void)r, (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};
static void settle(int milliseconds) {
    wl_display_roundtrip(display);
    usleep((useconds_t)milliseconds * 1000);
}
static void move_to(int x, int y) {
    org_kde_kwin_fake_input_pointer_motion_absolute(input, wl_fixed_from_int(x), wl_fixed_from_int(y));
    settle(70);
}
static void button(uint32_t state) {
    org_kde_kwin_fake_input_button(input, BTN_LEFT, state);
    settle(70);
}
static void key(int code, int pressed) {
    org_kde_kwin_fake_input_keyboard_key(input, (uint32_t)code, (uint32_t)pressed);
    settle(60);
}
static int letter_code(char c) {
    /* The evdev codes of the letters these tests use. */
    return c == 'a' ? 30 : c == 'c' ? 46 : c == 'v' ? 47 : -1;
}
int main(int argc, char **argv) {
    display = wl_display_connect(NULL);
    if (!display)
        return 2;
    wl_registry_add_listener(wl_display_get_registry(display), &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!input)
        return 2;
    org_kde_kwin_fake_input_authenticate(input, "chela-test", "input test");
    settle(50);
    for (int i = 1; i < argc; i++) {
        const char *step = argv[i];
        if (!strcmp(step, "click") || !strcmp(step, "press") || !strcmp(step, "move")) {
            move_to(atoi(argv[i + 1]), atoi(argv[i + 2]));
            i += 2;
            if (step[0] != 'm')
                button(WL_POINTER_BUTTON_STATE_PRESSED);
            if (step[0] == 'c')
                button(WL_POINTER_BUTTON_STATE_RELEASED);
        } else if (!strcmp(step, "release"))
            button(WL_POINTER_BUTTON_STATE_RELEASED);
        else if (!strcmp(step, "wheel")) {
            org_kde_kwin_fake_input_axis(input, WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(atoi(argv[++i])));
            settle(120);
        } else if (!strcmp(step, "wait"))
            settle(atoi(argv[++i]));
        else if (!strcmp(step, "key")) {
            const char *name = argv[++i];
            if (!strcmp(name, "return")) {
                key(KEY_ENTER, 1);
                key(KEY_ENTER, 0);
            } else if (!strncmp(name, "ctrl+", 5) && letter_code(name[5]) >= 0) {
                key(KEY_LEFTCTRL, 1);
                key(letter_code(name[5]), 1);
                key(letter_code(name[5]), 0);
                key(KEY_LEFTCTRL, 0);
            } else
                return 2;
        } else
            return 2;
    }
    settle(100);
    wl_display_disconnect(display);
    return 0;
}
