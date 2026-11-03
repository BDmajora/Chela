/* Absolute URI parsing for server addresses (RFC 3986 subset used by the desktop). */
#include "base.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>

void ch_uri_free(ChUri *u) {
    if (u) {
        free(u->scheme);
        free(u->userinfo);
        free(u->host);
        free(u->path);
        free(u->query);
        free(u->fragment);
        free(u);
    }
}
static bool valid_chars(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c <= ' ' || c == 0x7f || c == '"' || c == '<' || c == '>' || c == '\\' || c == '^' || c == '`' ||
            c == '{' || c == '|' || c == '}')
            return false;
        if (c == '%' && (i + 2 >= n + 0 || !isxdigit((unsigned char)s[i + 1]) || !isxdigit((unsigned char)s[i + 2])))
            return false;
    }
    return true;
}
ChUri *ch_uri_parse(const char *text) {
    if (!text)
        return NULL;
    const char *colon = strchr(text, ':');
    if (!colon || colon == text || !isalpha((unsigned char)text[0]))
        return NULL;
    for (const char *p = text; p < colon; p++)
        if (!isalnum((unsigned char)*p) && *p != '+' && *p != '-' && *p != '.')
            return NULL;
    ChUri *u = ch_new0(ChUri, 1);
    u->port = -1;
    u->scheme = ch_ascii_strdown(text, colon - text);
    const char *p = colon + 1;
    if (!valid_chars(p, strlen(p))) {
        ch_uri_free(u);
        return NULL;
    }
    if (p[0] == '/' && p[1] == '/') {
        p += 2;
        size_t auth = strcspn(p, "/?#");
        const char *at = memchr(p, '@', auth), *host = p, *end = p + auth;
        if (at) {
            u->userinfo = ch_strndup(p, (size_t)(at - p));
            host = at + 1;
        }
        const char *port = NULL;
        if (*host == '[') {
            const char *close = memchr(host, ']', (size_t)(end - host));
            if (!close) {
                ch_uri_free(u);
                return NULL;
            }
            u->host = ch_strndup(host + 1, (size_t)(close - host - 1));
            if (close + 1 < end) {
                if (close[1] != ':') {
                    ch_uri_free(u);
                    return NULL;
                }
                port = close + 2;
            }
        } else {
            const char *c = memchr(host, ':', (size_t)(end - host));
            u->host = ch_strndup(host, (size_t)((c ? c : end) - host));
            if (c)
                port = c + 1;
        }
        if (port && port < end) {
            long value = 0;
            for (const char *q = port; q < end; q++) {
                if (!isdigit((unsigned char)*q) || value > 65535) {
                    ch_uri_free(u);
                    return NULL;
                }
                value = value * 10 + (*q - '0');
            }
            if (value > 65535) {
                ch_uri_free(u);
                return NULL;
            }
            u->port = (int)value;
        }
        p = end;
    } else
        u->host = NULL;
    size_t path = strcspn(p, "?#");
    u->path = ch_strndup(p, path);
    p += path;
    if (*p == '?') {
        size_t q = strcspn(p + 1, "#");
        u->query = ch_strndup(p + 1, q);
        p += 1 + q;
    }
    if (*p == '#')
        u->fragment = ch_strdup(p + 1);
    return u;
}
char *ch_uri_join(const char *scheme, const char *userinfo, const char *host, int port, const char *path,
                  const char *query, const char *fragment) {
    ChBuf b;
    ch_buf_init(&b);
    ch_buf_printf(&b, "%s:", scheme);
    if (host) {
        ch_buf_append(&b, "//");
        if (userinfo)
            ch_buf_printf(&b, "%s@", userinfo);
        if (strchr(host, ':'))
            ch_buf_printf(&b, "[%s]", host);
        else
            ch_buf_append(&b, host);
        if (port >= 0)
            ch_buf_printf(&b, ":%d", port);
    }
    ch_buf_append(&b, path ? path : "");
    if (query)
        ch_buf_printf(&b, "?%s", query);
    if (fragment)
        ch_buf_printf(&b, "#%s", fragment);
    return ch_buf_steal(&b);
}
