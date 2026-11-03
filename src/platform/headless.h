#ifndef CHELA_HEADLESS_H
#define CHELA_HEADLESS_H
#include "../ui/platform.h"
/* How the backend looks inside the interface: the on-screen rectangle of the element
 * showing text (for "tap"), and every visible text (for "dump"). */
typedef bool (*HeadlessFinder)(void *data, const char *text, CuRect *rect);
typedef void (*HeadlessLister)(void *data, void (*emit)(const char *text, void *ctx), void *ctx);
void headless_set_finder(HeadlessFinder finder, HeadlessLister lister, void *data);
int headless_status(void);
#endif
