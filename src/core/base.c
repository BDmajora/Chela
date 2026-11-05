#define _GNU_SOURCE
#include "base.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wctype.h>

/* Errors */
static ChError *error_new(ChErrorKind kind, int code, char *message) {
    ChError *e = ch_new0(ChError, 1);
    e->kind = kind;
    e->code = code;
    e->message = message;
    return e;
}
void ch_set_error_literal(ChError **error, ChErrorKind kind, int code, const char *message) {
    if (!error)
        return;
    if (*error) {
        fprintf(stderr, "chela: error overwritten: %s -> %s\n", (*error)->message, message);
        ch_clear_error(error);
    }
    *error = error_new(kind, code, ch_strdup(message));
}
void ch_set_error(ChError **error, ChErrorKind kind, int code, const char *format, ...) {
    if (!error)
        return;
    va_list args;
    va_start(args, format);
    char *message = ch_strdup_vprintf(format, args);
    va_end(args);
    ch_set_error_literal(error, kind, code, message);
    free(message);
}
void ch_prefix_error(ChError **error, const char *format, ...) {
    if (!error || !*error)
        return;
    va_list args;
    va_start(args, format);
    char *prefix = ch_strdup_vprintf(format, args);
    va_end(args);
    char *joined = ch_strconcat(prefix, (*error)->message, NULL);
    free(prefix);
    free((*error)->message);
    (*error)->message = joined;
}
void ch_propagate_error(ChError **dest, ChError *src) {
    if (!src)
        return;
    if (!dest) {
        ch_error_free(src);
        return;
    }
    if (*dest)
        ch_error_free(src);
    else
        *dest = src;
}
void ch_error_free(ChError *error) {
    if (error) {
        free(error->message);
        free(error);
    }
}
void ch_clear_error(ChError **error) {
    if (error && *error) {
        ch_error_free(*error);
        *error = NULL;
    }
}
bool ch_error_is(const ChError *error, ChErrorKind kind) {
    return error && error->kind == kind;
}

/* Memory and strings */
void *ch_alloc(size_t size) {
    void *p = malloc(size ? size : 1);
    if (!p)
        abort();
    return p;
}
void *ch_alloc0(size_t size) {
    void *p = calloc(1, size ? size : 1);
    if (!p)
        abort();
    return p;
}
void *ch_realloc(void *p, size_t size) {
    p = realloc(p, size ? size : 1);
    if (!p)
        abort();
    return p;
}
char *ch_strdup(const char *s) {
    return s ? ch_strndup(s, strlen(s)) : NULL;
}
char *ch_strndup(const char *s, size_t n) {
    if (!s)
        return NULL;
    size_t len = strnlen(s, n);
    char *d = ch_alloc(len + 1);
    memcpy(d, s, len);
    d[len] = 0;
    return d;
}
char *ch_strdup_vprintf(const char *format, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int n = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (n < 0)
        return ch_strdup("");
    char *s = ch_alloc((size_t)n + 1);
    vsnprintf(s, (size_t)n + 1, format, args);
    return s;
}
char *ch_strdup_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *s = ch_strdup_vprintf(format, args);
    va_end(args);
    return s;
}
char *ch_strconcat(const char *first, ...) {
    if (!first)
        return NULL;
    ChBuf b;
    ch_buf_init(&b);
    ch_buf_append(&b, first);
    va_list args;
    va_start(args, first);
    const char *s;
    while ((s = va_arg(args, const char *)))
        ch_buf_append(&b, s);
    va_end(args);
    return ch_buf_steal(&b);
}
size_t ch_strlcpy(char *dest, const char *src, size_t size) {
    size_t n = strlen(src);
    if (size) {
        size_t c = n < size - 1 ? n : size - 1;
        memcpy(dest, src, c);
        dest[c] = 0;
    }
    return n;
}
bool ch_str_has_prefix(const char *s, const char *prefix) {
    return s && strncmp(s, prefix, strlen(prefix)) == 0;
}
bool ch_str_has_suffix(const char *s, const char *suffix) {
    size_t a = strlen(s), b = strlen(suffix);
    return a >= b && memcmp(s + a - b, suffix, b) == 0;
}
char *ch_strstrip(char *s) {
    char *start = s;
    while (*start && isspace((unsigned char)*start))
        start++;
    size_t n = strlen(start);
    while (n && isspace((unsigned char)start[n - 1]))
        n--;
    memmove(s, start, n);
    s[n] = 0;
    return s;
}
char *ch_ascii_strdown(const char *s, ptrdiff_t len) {
    size_t n = len < 0 ? strlen(s) : (size_t)len;
    char *d = ch_strndup(s, n);
    for (char *p = d; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    return d;
}
int ch_ascii_strcasecmp(const char *a, const char *b) {
    for (;; a++, b++) {
        int x = tolower((unsigned char)*a), y = tolower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}
int ch_strcmp0(const char *a, const char *b) {
    if (!a || !b)
        return a == b ? 0 : a ? 1 : -1;
    return strcmp(a, b);
}
char **ch_strsplit(const char *s, const char *delimiter) {
    ChVec parts;
    ch_vec_init(&parts, NULL);
    size_t dl = strlen(delimiter);
    const char *p = s;
    for (const char *q; dl && (q = strstr(p, delimiter)); p = q + dl)
        ch_vec_push(&parts, ch_strndup(p, (size_t)(q - p)));
    ch_vec_push(&parts, ch_strdup(p));
    ch_vec_push(&parts, NULL);
    return (char **)parts.data;
}
void ch_strfreev(char **v) {
    for (size_t i = 0; v && v[i]; i++)
        free(v[i]);
    free(v);
}
size_t ch_strv_length(char **v) {
    size_t n = 0;
    while (v && v[n])
        n++;
    return n;
}

/* Buffers */
static void buf_reserve(ChBuf *b, size_t extra) {
    if (b->len + extra + 1 <= b->cap)
        return;
    size_t cap = b->cap ? b->cap : 64;
    while (cap < b->len + extra + 1)
        cap *= 2;
    b->str = ch_realloc(b->str, cap);
    b->cap = cap;
}
void ch_buf_init(ChBuf *b) {
    b->str = ch_alloc(64);
    b->str[0] = 0;
    b->len = 0;
    b->cap = 64;
}
ChBuf *ch_buf_new(const char *init) {
    ChBuf *b = ch_new0(ChBuf, 1);
    ch_buf_init(b);
    if (init)
        ch_buf_append(b, init);
    return b;
}
void ch_buf_append_len(ChBuf *b, const char *s, size_t n) {
    buf_reserve(b, n);
    memcpy(b->str + b->len, s, n);
    b->len += n;
    b->str[b->len] = 0;
}
void ch_buf_append(ChBuf *b, const char *s) {
    if (s)
        ch_buf_append_len(b, s, strlen(s));
}
void ch_buf_append_c(ChBuf *b, char c) {
    ch_buf_append_len(b, &c, 1);
}
void ch_buf_printf(ChBuf *b, const char *format, ...) {
    va_list args;
    va_start(args, format);
    char *s = ch_strdup_vprintf(format, args);
    va_end(args);
    ch_buf_append(b, s);
    free(s);
}
void ch_buf_truncate(ChBuf *b, size_t len) {
    if (len < b->len) {
        b->len = len;
        b->str[len] = 0;
    }
}
void ch_buf_erase(ChBuf *b, size_t pos, size_t n) {
    if (pos >= b->len)
        return;
    if (n > b->len - pos)
        n = b->len - pos;
    memmove(b->str + pos, b->str + pos + n, b->len - pos - n + 1);
    b->len -= n;
}
void ch_buf_assign(ChBuf *b, const char *s) {
    ch_buf_truncate(b, 0);
    ch_buf_append(b, s);
}
void ch_buf_clear(ChBuf *b) {
    free(b->str);
    b->str = NULL;
    b->len = b->cap = 0;
}
char *ch_buf_steal(ChBuf *b) {
    char *s = b->str;
    b->str = NULL;
    b->len = b->cap = 0;
    return s;
}
char *ch_buf_free(ChBuf *b, bool keep) {
    char *s = keep ? b->str : NULL;
    if (!keep)
        free(b->str);
    free(b);
    return s;
}

/* Vectors */
void ch_vec_init(ChVec *v, void (*free_fn)(void *)) {
    v->data = NULL;
    v->len = v->cap = 0;
    v->free_fn = free_fn;
}
ChVec *ch_vec_new(void (*free_fn)(void *)) {
    ChVec *v = ch_new0(ChVec, 1);
    ch_vec_init(v, free_fn);
    return v;
}
void ch_vec_push(ChVec *v, void *item) {
    ch_vec_insert(v, v->len, item);
}
void ch_vec_insert(ChVec *v, size_t index, void *item) {
    if (v->len + 1 > v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->data = ch_realloc(v->data, v->cap * sizeof(void *));
    }
    if (index > v->len)
        index = v->len;
    memmove(v->data + index + 1, v->data + index, (v->len - index) * sizeof(void *));
    v->data[index] = item;
    v->len++;
}
void *ch_vec_steal(ChVec *v, size_t index) {
    void *item = v->data[index];
    memmove(v->data + index, v->data + index + 1, (v->len - index - 1) * sizeof(void *));
    v->len--;
    return item;
}
void ch_vec_remove(ChVec *v, size_t index) {
    void *item = ch_vec_steal(v, index);
    if (v->free_fn && item)
        v->free_fn(item);
}
void ch_vec_clear(ChVec *v) {
    for (size_t i = 0; v->free_fn && i < v->len; i++)
        if (v->data[i])
            v->free_fn(v->data[i]);
    free(v->data);
    v->data = NULL;
    v->len = v->cap = 0;
}
void ch_vec_free(ChVec *v) {
    if (v) {
        ch_vec_clear(v);
        free(v);
    }
}
void ch_vec_sort(ChVec *v, int (*compare)(const void *, const void *, void *), void *data) {
    /* Insertion into a merged run keeps equal elements in their original order. */
    if (v->len < 2)
        return;
    void **tmp = ch_alloc(v->len * sizeof(void *));
    for (size_t width = 1; width < v->len; width *= 2) {
        for (size_t lo = 0; lo < v->len; lo += 2 * width) {
            size_t mid = CH_MIN(lo + width, v->len), hi = CH_MIN(lo + 2 * width, v->len), i = lo, j = mid, k = lo;
            while (i < mid && j < hi)
                tmp[k++] = compare(&v->data[j], &v->data[i], data) < 0 ? v->data[j++] : v->data[i++];
            while (i < mid)
                tmp[k++] = v->data[i++];
            while (j < hi)
                tmp[k++] = v->data[j++];
        }
        memcpy(v->data, tmp, v->len * sizeof(void *));
    }
    free(tmp);
}

/* Maps */
void ch_map_init(ChMap *m, void (*free_value)(void *)) {
    memset(m, 0, sizeof(*m));
    m->free_value = free_value;
}
void ch_map_clear(ChMap *m) {
    for (size_t i = 0; i < m->len; i++) {
        free(m->keys[i]);
        if (m->free_value && m->values[i])
            m->free_value(m->values[i]);
    }
    free(m->keys);
    free(m->values);
    m->keys = NULL;
    m->values = NULL;
    m->len = m->cap = 0;
}
static ptrdiff_t map_find(const ChMap *m, const char *key) {
    for (size_t i = 0; key && i < m->len; i++)
        if (!strcmp(m->keys[i], key))
            return (ptrdiff_t)i;
    return -1;
}
void *ch_map_get(const ChMap *m, const char *key) {
    ptrdiff_t i = map_find(m, key);
    return i < 0 ? NULL : m->values[i];
}
bool ch_map_has(const ChMap *m, const char *key) {
    return map_find(m, key) >= 0;
}
void ch_map_set(ChMap *m, const char *key, void *value) {
    ptrdiff_t i = map_find(m, key);
    if (i >= 0) {
        if (m->free_value && m->values[i] && m->values[i] != value)
            m->free_value(m->values[i]);
        m->values[i] = value;
        return;
    }
    if (m->len == m->cap) {
        m->cap = m->cap ? m->cap * 2 : 8;
        m->keys = ch_realloc(m->keys, m->cap * sizeof(char *));
        m->values = ch_realloc(m->values, m->cap * sizeof(void *));
    }
    m->keys[m->len] = ch_strdup(key);
    m->values[m->len++] = value;
}
bool ch_map_remove(ChMap *m, const char *key) {
    ptrdiff_t i = map_find(m, key);
    if (i < 0)
        return false;
    free(m->keys[i]);
    if (m->free_value && m->values[i])
        m->free_value(m->values[i]);
    memmove(m->keys + i, m->keys + i + 1, (m->len - (size_t)i - 1) * sizeof(char *));
    memmove(m->values + i, m->values + i + 1, (m->len - (size_t)i - 1) * sizeof(void *));
    m->len--;
    return true;
}

/* UTF-8 */
static int utf8_sequence(const unsigned char *s, size_t avail, uint32_t *out) {
    unsigned char c = s[0];
    int n;
    uint32_t v, min;
    if (c < 0x80) {
        *out = c;
        return 1;
    } else if ((c & 0xe0) == 0xc0) {
        n = 2, v = c & 0x1f, min = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        n = 3, v = c & 0x0f, min = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        n = 4, v = c & 0x07, min = 0x10000;
    } else
        return -1;
    if ((size_t)n > avail)
        return -1;
    for (int i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return -1;
        v = (v << 6) | (s[i] & 0x3f);
    }
    if (v < min || v > 0x10ffff || (v >= 0xd800 && v <= 0xdfff))
        return -1;
    *out = v;
    return n;
}
bool ch_utf8_validate(const char *s, ptrdiff_t len, const char **end) {
    size_t n = len < 0 ? strlen(s) : (size_t)len;
    const unsigned char *p = (const unsigned char *)s, *stop = p + n;
    while (p < stop) {
        uint32_t c;
        int k = *p ? utf8_sequence(p, (size_t)(stop - p), &c) : -1;
        if (k < 0) {
            if (end)
                *end = (const char *)p;
            return false;
        }
        p += k;
    }
    if (end)
        *end = (const char *)p;
    return true;
}
char *ch_utf8_make_valid(const char *s, ptrdiff_t len) {
    size_t n = len < 0 ? strlen(s) : (size_t)len;
    ChBuf b;
    ch_buf_init(&b);
    const unsigned char *p = (const unsigned char *)s, *stop = p + n;
    while (p < stop) {
        uint32_t c;
        int k = *p ? utf8_sequence(p, (size_t)(stop - p), &c) : -1;
        if (k < 0) {
            ch_buf_append(&b, "\xef\xbf\xbd");
            p++;
        } else {
            ch_buf_append_len(&b, (const char *)p, (size_t)k);
            p += k;
        }
    }
    return ch_buf_steal(&b);
}
uint32_t ch_utf8_get(const char *s, const char **next) {
    uint32_t c = 0xfffd;
    int k = utf8_sequence((const unsigned char *)s, 4, &c);
    if (k < 0) {
        c = 0xfffd;
        k = 1;
    }
    if (next)
        *next = s + k;
    return c;
}
const char *ch_utf8_next(const char *s) {
    if (!*s)
        return s;
    s++;
    while ((*s & 0xc0) == 0x80)
        s++;
    return s;
}
const char *ch_utf8_prev(const char *start, const char *s) {
    if (s <= start)
        return start;
    s--;
    while (s > start && (*s & 0xc0) == 0x80)
        s--;
    return s;
}
size_t ch_utf8_strlen(const char *s, ptrdiff_t len) {
    size_t n = 0;
    const char *end = len < 0 ? s + strlen(s) : s + len;
    for (const char *p = s; p < end && *p; p = ch_utf8_next(p))
        n++;
    return n;
}
const char *ch_utf8_offset(const char *s, size_t chars) {
    while (chars-- && *s)
        s = ch_utf8_next(s);
    return s;
}
char *ch_utf8_substring(const char *s, size_t start, size_t end) {
    const char *a = ch_utf8_offset(s, start), *b = ch_utf8_offset(a, end - start);
    return ch_strndup(a, (size_t)(b - a));
}
int ch_utf8_encode(uint32_t c, char out[4]) {
    if (c < 0x80) {
        out[0] = (char)c;
        return 1;
    } else if (c < 0x800) {
        out[0] = (char)(0xc0 | (c >> 6));
        out[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    } else if (c < 0x10000) {
        out[0] = (char)(0xe0 | (c >> 12));
        out[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        out[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (c >> 18));
    out[1] = (char)(0x80 | ((c >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((c >> 6) & 0x3f));
    out[3] = (char)(0x80 | (c & 0x3f));
    return 4;
}
/* Simple case mapping for Latin, Greek, Cyrillic and fullwidth letters; independent of locale. */
static uint32_t fold(uint32_t c) {
    if ((c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) || (c >= 0x391 && c <= 0x3ab && c != 0x3a2) ||
        (c >= 0x410 && c <= 0x42f) || (c >= 0xff21 && c <= 0xff3a))
        return c + 0x20;
    if (c >= 0x400 && c <= 0x40f)
        return c + 0x50;
    if (c == 0x178)
        return 0xff;
    if ((c >= 0x100 && c <= 0x137 && c != 0x130) || (c >= 0x14a && c <= 0x177) || (c >= 0x460 && c <= 0x4ff) ||
        (c >= 0x1e00 && c <= 0x1eff))
        return c | 1;
    if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17e))
        return (c & 1) ? c + 1 : c;
    return c;
}
static uint32_t unfold(uint32_t c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) || (c >= 0x3b1 && c <= 0x3cb && c != 0x3c2) ||
        (c >= 0x430 && c <= 0x44f) || (c >= 0xff41 && c <= 0xff5a))
        return c - 0x20;
    if (c >= 0x450 && c <= 0x45f)
        return c - 0x50;
    if ((c >= 0x100 && c <= 0x137 && c != 0x131) || (c >= 0x14a && c <= 0x177) || (c >= 0x460 && c <= 0x4ff) ||
        (c >= 0x1e00 && c <= 0x1eff))
        return c & ~1u;
    return c;
}
char *ch_utf8_casefold(const char *s) {
    ChBuf b;
    ch_buf_init(&b);
    for (const char *p = s; p && *p;) {
        uint32_t c = ch_utf8_get(p, &p);
        char out[4];
        ch_buf_append_len(&b, out, (size_t)ch_utf8_encode(fold(c), out));
    }
    return ch_buf_steal(&b);
}
char *ch_utf8_upper_first(const char *s) {
    if (!s || !*s)
        return ch_strdup("");
    char out[4];
    int n = ch_utf8_encode(unfold(ch_utf8_get(s, NULL)), out);
    return ch_strndup(out, (size_t)n);
}
int ch_utf8_collate(const char *a, const char *b) {
    char *x = ch_utf8_casefold(a), *y = ch_utf8_casefold(b);
    int r = strcoll(x, y);
    if (!r)
        r = strcmp(a, b);
    free(x);
    free(y);
    return r;
}
bool ch_unichar_isspace(uint32_t c) {
    return c == ' ' || (c >= '\t' && c <= '\r') || c == 0x85 || c == 0xa0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200a) || c == 0x2028 || c == 0x2029 || c == 0x202f || c == 0x205f ||
           c == 0x3000;
}

/* Time and identifiers */
int64_t ch_monotonic_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * CH_USEC_PER_SEC + t.tv_nsec / 1000;
}
int64_t ch_real_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * CH_USEC_PER_SEC + t.tv_nsec / 1000;
}
void ch_usleep(int64_t us) {
    struct timespec t = {(time_t)(us / CH_USEC_PER_SEC), (long)(us % CH_USEC_PER_SEC) * 1000};
    while (nanosleep(&t, &t) && errno == EINTR) {
    }
}
void ch_random_bytes(void *buf, size_t n) {
    unsigned char *p = buf;
    while (n) {
        ssize_t r = getrandom(p, n, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            abort();
        }
        p += r;
        n -= (size_t)r;
    }
}
char *ch_uuid_random(void) {
    unsigned char b[16];
    ch_random_bytes(b, sizeof(b));
    b[6] = (unsigned char)((b[6] & 0x0f) | 0x40);
    b[8] = (unsigned char)((b[8] & 0x3f) | 0x80);
    return ch_strdup_printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", b[0], b[1],
                            b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                            b[15]);
}
bool ch_uuid_valid(const char *s) {
    if (!s || strlen(s) != 36)
        return false;
    for (int i = 0; i < 36; i++) {
        bool dash = i == 8 || i == 13 || i == 18 || i == 23;
        if (dash ? s[i] != '-' : !isxdigit((unsigned char)s[i]))
            return false;
    }
    return true;
}

/* SHA-256 (FIPS 180-4), used for stable private socket names. */
static const uint32_t sha_k[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(uint32_t h[8], const unsigned char *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t t1 = k + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + sha_k[i] + w[i];
        uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        k = g, g = f, f = e, e = d + t1, d = c, c = b, b = a, a = t1 + t2;
    }
    h[0] += a, h[1] += b, h[2] += c, h[3] += d, h[4] += e, h[5] += f, h[6] += g, h[7] += k;
}
char *ch_sha256_hex(const char *data, size_t len) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    size_t full = len / 64 * 64;
    for (size_t i = 0; i < full; i += 64)
        sha_block(h, (const unsigned char *)data + i);
    unsigned char tail[128] = {0};
    size_t rest = len - full;
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    size_t total = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; i++)
        tail[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t i = 0; i < total; i += 64)
        sha_block(h, tail + i);
    char *out = ch_alloc(65);
    for (int i = 0; i < 8; i++)
        snprintf(out + i * 8, 9, "%08x", h[i]);
    return out;
}

/* Paths and files */
char *ch_path_basename(const char *path) {
    if (!path || !*path)
        return ch_strdup(".");
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    if (n == 1 && path[0] == '/')
        return ch_strdup("/");
    size_t start = n;
    while (start && path[start - 1] != '/')
        start--;
    return ch_strndup(path + start, n - start);
}
char *ch_path_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    if (!slash)
        return ch_strdup(".");
    while (slash > path && slash[-1] == '/')
        slash--;
    return slash == path ? ch_strdup("/") : ch_strndup(path, (size_t)(slash - path));
}
char *ch_build_path(const char *first, ...) {
    ChBuf b;
    ch_buf_init(&b);
    va_list args;
    va_start(args, first);
    for (const char *part = first; part; part = va_arg(args, const char *)) {
        if (!*part)
            continue;
        if (b.len && b.str[b.len - 1] != '/' && *part != '/')
            ch_buf_append_c(&b, '/');
        else if (b.len && b.str[b.len - 1] == '/' && *part == '/')
            part++;
        ch_buf_append(&b, part);
    }
    va_end(args);
    return ch_buf_steal(&b);
}
bool ch_path_is_absolute(const char *path) {
    return path && path[0] == '/';
}
char *ch_current_dir(void) {
    char *buf = getcwd(NULL, 0);
    char *out = ch_strdup(buf ? buf : "/");
    free(buf);
    return out;
}
char *ch_canonicalize(const char *path, const char *relative_to) {
    char *base = relative_to ? ch_strdup(relative_to) : ch_current_dir();
    char *joined = ch_path_is_absolute(path) ? ch_strdup(path) : ch_build_path(base, path, NULL);
    free(base);
    char **parts = ch_strsplit(joined, "/");
    free(joined);
    ChVec kept;
    ch_vec_init(&kept, NULL);
    for (size_t i = 0; parts[i]; i++) {
        if (!*parts[i] || !strcmp(parts[i], "."))
            continue;
        if (!strcmp(parts[i], "..")) {
            if (kept.len)
                kept.len--;
            continue;
        }
        ch_vec_push(&kept, parts[i]);
    }
    ChBuf b;
    ch_buf_init(&b);
    for (size_t i = 0; i < kept.len; i++) {
        ch_buf_append_c(&b, '/');
        ch_buf_append(&b, kept.data[i]);
    }
    if (!b.len)
        ch_buf_append_c(&b, '/');
    ch_vec_clear(&kept);
    ch_strfreev(parts);
    return ch_buf_steal(&b);
}
bool ch_file_exists(const char *path) {
    struct stat st;
    return path && lstat(path, &st) == 0;
}
bool ch_file_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}
bool ch_file_is_regular(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}
int ch_mkdir_p(const char *path, int mode) {
    char *p = ch_strdup(path);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            if (mkdir(p, (mode_t)mode) && errno != EEXIST) {
                free(p);
                return -1;
            }
            *s = '/';
        }
    int rc = mkdir(p, (mode_t)mode) && errno != EEXIST ? -1 : 0;
    free(p);
    if (!rc && !ch_file_is_dir(path)) {
        errno = ENOTDIR;
        rc = -1;
    }
    return rc;
}
bool ch_file_get_contents(const char *path, char **contents, size_t *length, ChError **error) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        ch_set_error(error, CH_ERR_IO, errno, "Cannot open %s: %s", path, strerror(errno));
        return false;
    }
    ChBuf b;
    ch_buf_init(&b);
    char chunk[16384];
    ssize_t n;
    while ((n = read(fd, chunk, sizeof(chunk))) != 0) {
        if (n < 0) {
            if (errno == EINTR)
                continue;
            ch_set_error(error, CH_ERR_IO, errno, "Cannot read %s: %s", path, strerror(errno));
            close(fd);
            ch_buf_clear(&b);
            return false;
        }
        ch_buf_append_len(&b, chunk, (size_t)n);
    }
    close(fd);
    if (length)
        *length = b.len;
    *contents = ch_buf_steal(&b);
    return true;
}
bool ch_file_set_contents(const char *path, const char *data, ptrdiff_t length, int mode, ChError **error) {
    size_t n = length < 0 ? strlen(data) : (size_t)length;
    char *dir = ch_path_dirname(path), *name = ch_path_basename(path);
    char *uuid = ch_uuid_random();
    char *tmp = ch_strdup_printf("%s/.%s.%s.tmp", dir, name, uuid);
    free(uuid);
    free(name);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, (mode_t)mode);
    bool ok = fd >= 0;
    size_t off = 0;
    while (ok && off < n) {
        ssize_t w = write(fd, data + off, n - off);
        if (w < 0 && errno == EINTR)
            continue;
        ok = w > 0;
        if (ok)
            off += (size_t)w;
    }
    if (ok)
        ok = fchmod(fd, (mode_t)mode) == 0 && fsync(fd) == 0;
    int saved = errno;
    if (fd >= 0)
        close(fd);
    if (ok && rename(tmp, path)) {
        saved = errno;
        ok = false;
    }
    if (!ok) {
        unlink(tmp);
        ch_set_error(error, CH_ERR_IO, saved, "Cannot write %s: %s", path, strerror(saved));
    } else {
        int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            (void)fsync(dfd);
            close(dfd);
        }
    }
    free(tmp);
    free(dir);
    return ok;
}
char *ch_read_link(const char *path, ChError **error) {
    char buf[4096];
    ssize_t n = readlink(path, buf, sizeof(buf) - 1);
    if (n < 0) {
        ch_set_error(error, CH_ERR_IO, errno, "Cannot read link %s: %s", path, strerror(errno));
        return NULL;
    }
    buf[n] = 0;
    return ch_strdup(buf);
}
const char *ch_getenv(const char *name) {
    return getenv(name);
}
static struct passwd *user_entry(void) {
    static struct passwd entry;
    static char storage[4096];
    static bool loaded;
    static struct passwd *found;
    if (!loaded) {
        getpwuid_r(getuid(), &entry, storage, sizeof(storage), &found);
        loaded = true;
    }
    return found;
}
const char *ch_home_dir(void) {
    const char *home = getenv("HOME");
    if (home && *home)
        return home;
    struct passwd *pw = user_entry();
    return pw && pw->pw_dir ? pw->pw_dir : "/";
}
const char *ch_user_name(void) {
    struct passwd *pw = user_entry();
    if (pw && pw->pw_name)
        return pw->pw_name;
    const char *user = getenv("USER");
    return user ? user : "user";
}
const char *ch_real_name(void) {
    static char name[256];
    struct passwd *pw = user_entry();
    if (!pw || !pw->pw_gecos || !*pw->pw_gecos)
        return ch_user_name();
    ch_strlcpy(name, pw->pw_gecos, sizeof(name));
    char *comma = strchr(name, ',');
    if (comma)
        *comma = 0;
    return *name ? name : ch_user_name();
}
static char *xdg_dir(const char *variable, const char *fallback) {
    const char *value = getenv(variable);
    if (value && ch_path_is_absolute(value))
        return ch_strdup(value);
    return ch_build_path(ch_home_dir(), fallback, NULL);
}
char *ch_user_data_dir(void) {
    return xdg_dir("XDG_DATA_HOME", ".local/share");
}
char *ch_user_config_dir(void) {
    return xdg_dir("XDG_CONFIG_HOME", ".config");
}
char *ch_user_runtime_dir(void) {
    const char *value = getenv("XDG_RUNTIME_DIR");
    if (value && ch_path_is_absolute(value))
        return ch_strdup(value);
    return xdg_dir("XDG_CACHE_HOME", ".cache");
}

/* Shell parsing: words, single quotes, double quotes with \ escapes, backslashes, # comments. */
bool ch_shell_parse_argv(const char *command, int *argc, char ***argv, ChError **error) {
    ChVec words;
    ch_vec_init(&words, free);
    ChBuf word;
    ch_buf_init(&word);
    bool in_word = false;
    const char *p = command ? command : "";
    while (*p) {
        char c = *p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (in_word) {
                ch_vec_push(&words, ch_strdup(word.str));
                ch_buf_truncate(&word, 0);
                in_word = false;
            }
            p++;
        } else if (c == '#' && !in_word) {
            while (*p && *p != '\n')
                p++;
        } else if (c == '\'') {
            const char *end = strchr(p + 1, '\'');
            if (!end)
                goto unterminated;
            ch_buf_append_len(&word, p + 1, (size_t)(end - p - 1));
            in_word = true;
            p = end + 1;
        } else if (c == '"') {
            p++;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1] && strchr("$`\"\\\n", p[1])) {
                    if (p[1] != '\n')
                        ch_buf_append_c(&word, p[1]);
                    p += 2;
                } else
                    ch_buf_append_c(&word, *p++);
            }
            if (*p != '"')
                goto unterminated;
            in_word = true;
            p++;
        } else if (c == '\\') {
            if (!p[1])
                goto unterminated;
            if (p[1] != '\n')
                ch_buf_append_c(&word, p[1]);
            in_word = in_word || p[1] != '\n';
            p += 2;
        } else {
            ch_buf_append_c(&word, c);
            in_word = true;
            p++;
        }
    }
    if (in_word)
        ch_vec_push(&words, ch_strdup(word.str));
    ch_buf_clear(&word);
    if (!words.len) {
        ch_vec_clear(&words);
        ch_fail_literal(error, "Text was empty (or contained only whitespace)");
        return false;
    }
    if (argc)
        *argc = (int)words.len;
    ch_vec_push(&words, NULL);
    if (argv)
        *argv = (char **)words.data;
    else
        ch_strfreev((char **)words.data);
    return true;
unterminated:
    ch_buf_clear(&word);
    ch_vec_clear(&words);
    ch_fail_literal(error, "Text ended before a matching quote or escape was found");
    return false;
}
char *ch_shell_quote(const char *s) {
    ChBuf b;
    ch_buf_init(&b);
    ch_buf_append_c(&b, '\'');
    for (; *s; s++) {
        if (*s == '\'')
            ch_buf_append(&b, "'\\''");
        else
            ch_buf_append_c(&b, *s);
    }
    ch_buf_append_c(&b, '\'');
    return ch_buf_steal(&b);
}

/* Cancellation */
struct ChCancel {
    atomic_int refs;
    atomic_bool cancelled;
};
ChCancel *ch_cancel_new(void) {
    ChCancel *c = ch_new0(ChCancel, 1);
    atomic_init(&c->refs, 1);
    atomic_init(&c->cancelled, false);
    return c;
}
ChCancel *ch_cancel_ref(ChCancel *c) {
    if (c)
        atomic_fetch_add(&c->refs, 1);
    return c;
}
void ch_cancel_unref(ChCancel *c) {
    if (c && atomic_fetch_sub(&c->refs, 1) == 1)
        free(c);
}
void ch_cancel_cancel(ChCancel *c) {
    if (c)
        atomic_store(&c->cancelled, true);
}
bool ch_cancel_is_cancelled(const ChCancel *c) {
    return c && atomic_load(&((ChCancel *)c)->cancelled);
}
bool ch_cancel_check(const ChCancel *c, ChError **error) {
    if (!ch_cancel_is_cancelled(c))
        return false;
    ch_set_error_literal(error, CH_ERR_CANCELLED, 0, "Stopped");
    return true;
}

/* Threads */
typedef struct {
    ChThreadFn fn;
    void *data;
} ThreadStart;
static void *thread_main(void *data) {
    ThreadStart start = *(ThreadStart *)data;
    free(data);
    start.fn(start.data);
    return NULL;
}
bool ch_thread_run(ChThreadFn fn, void *data) {
    ThreadStart *start = ch_new0(ThreadStart, 1);
    start->fn = fn;
    start->data = data;
    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8u << 20);
    int rc = pthread_create(&thread, &attr, thread_main, start);
    pthread_attr_destroy(&attr);
    if (rc) {
        free(start);
        return false;
    }
    return true;
}

/* Which of a clipboard offer's MIME types to read. */
static int image_rank(const char *mime) {
    static const char *const order[] = {"image/png", "image/jpeg", "image/webp", "image/gif", "image/bmp", "image/tiff"};
    for (size_t i = 0; i < CH_N_ELEMENTS(order); i++)
        if (!strcmp(mime, order[i]))
            return (int)i;
    return -1;
}
static bool text_mime(const char *mime) {
    return !strcmp(mime, "text/plain;charset=utf-8") || !strcmp(mime, "text/plain") || !strcmp(mime, "UTF8_STRING") ||
           !strcmp(mime, "TEXT") || !strcmp(mime, "STRING");
}
int ch_clipboard_best(const char *const *mimes, int count) {
    int uri = -1, gnome = -1, text = -1, image = -1, best = 99;
    for (int i = 0; i < count; i++) {
        if (!strcmp(mimes[i], "text/uri-list"))
            uri = i;
        else if (!strcmp(mimes[i], "x-special/gnome-copied-files"))
            gnome = i;
        else if (text_mime(mimes[i]) && (text < 0 || !strcmp(mimes[i], "text/plain;charset=utf-8")))
            text = i;
        else if (image_rank(mimes[i]) >= 0 && image_rank(mimes[i]) < best) {
            best = image_rank(mimes[i]);
            image = i;
        }
    }
    /* Copied files first; then text, because spreadsheets also offer a picture of the
     * selected cells. A picture copied from a browser comes with HTML but no plain text. */
    return uri >= 0 ? uri : gnome >= 0 ? gnome : text >= 0 ? text : image;
}
