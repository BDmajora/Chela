#include "chela.h"
#include <string.h>
void ch_sse_init(ChSse *s, ChSseEvent callback, void *user) {
    memset(s, 0, sizeof(*s));
    ch_buf_init(&s->line);
    ch_buf_init(&s->event);
    s->callback = callback;
    s->user = user;
}
static bool dispatch(ChSse *s, ChError **error) {
    if (!s->event.len)
        return true;
    ch_buf_truncate(&s->event, s->event.len - 1);
    bool ok = true;
    if (!strcmp(s->event.str, "[DONE]"))
        s->done = true;
    else if (!s->done)
        ok = s->callback(s->event.str, s->user, error);
    ch_buf_truncate(&s->event, 0);
    return ok;
}
static bool line(ChSse *s, ChError **error) {
    if (s->line.len && s->line.str[s->line.len - 1] == '\r')
        ch_buf_truncate(&s->line, s->line.len - 1);
    bool ok = true;
    if (!s->line.len)
        ok = dispatch(s, error);
    else if (ch_str_has_prefix(s->line.str, "data:")) {
        const char *p = s->line.str + 5;
        if (*p == ' ')
            p++;
        if (s->event.len + strlen(p) > CHELA_LIMIT) {
            ch_fail_literal(error, "Stream event too large");
            ok = false;
        } else {
            ch_buf_append(&s->event, p);
            ch_buf_append_c(&s->event, '\n');
        }
    }
    ch_buf_truncate(&s->line, 0);
    return ok;
}
bool ch_sse_feed(ChSse *s, const char *data, size_t n, ChError **error) {
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            if (!line(s, error))
                return false;
        } else {
            if (s->line.len >= CHELA_LIMIT || !data[i]) {
                ch_fail_literal(error, "Invalid or oversized stream line");
                return false;
            }
            ch_buf_append_c(&s->line, data[i]);
        }
    }
    return true;
}
bool ch_sse_finish(ChSse *s, ChError **error) {
    if (s->line.len && !line(s, error))
        return false;
    return dispatch(s, error);
}
void ch_sse_clear(ChSse *s) {
    ch_buf_clear(&s->line);
    ch_buf_clear(&s->event);
}
