/* Canvas and text for backends that render with cairo (Wayland, headless, GTK).
 * Text uses FreeType faces chosen by fontconfig with per-character fallback; no
 * Pango or GLib. */
#ifndef CHELA_CAIRO_CANVAS_H
#define CHELA_CAIRO_CANVAS_H
#include "../ui/platform.h"
#include <cairo.h>

struct CuCanvas {
    cairo_t *cr;
};
void cc_install(CuPlatform *p); /* fills the font and canvas entries */
void cc_shutdown(void);
#endif
