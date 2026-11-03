/* Key files: [group] sections of key=value lines, # comments, \s \n \t \r \\ escapes
 * and ;-separated lists. Reads and writes files produced by earlier Chela releases. */
#include "base.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *key, *value; /* value is stored escaped, exactly as written in the file */
} Entry;
typedef struct {
    char *name;
    ChVec entries;
} Group;
struct ChKeyFile {
    ChVec groups;
};
static void entry_free(void *data) {
    Entry *e = data;
    free(e->key);
    free(e->value);
    free(e);
}
static void group_free(void *data) {
    Group *g = data;
    free(g->name);
    ch_vec_clear(&g->entries);
    free(g);
}
ChKeyFile *ch_keyfile_new(void) {
    ChKeyFile *k = ch_new0(ChKeyFile, 1);
    ch_vec_init(&k->groups, group_free);
    return k;
}
void ch_keyfile_free(ChKeyFile *k) {
    if (k) {
        ch_vec_clear(&k->groups);
        free(k);
    }
}
static Group *find_group(const ChKeyFile *k, const char *name, bool create) {
    for (size_t i = 0; i < k->groups.len; i++) {
        Group *g = k->groups.data[i];
        if (!strcmp(g->name, name))
            return g;
    }
    if (!create)
        return NULL;
    Group *g = ch_new0(Group, 1);
    g->name = ch_strdup(name);
    ch_vec_init(&g->entries, entry_free);
    ch_vec_push((ChVec *)&k->groups, g);
    return g;
}
static Entry *find_entry(const ChKeyFile *k, const char *group, const char *key) {
    Group *g = find_group(k, group, false);
    for (size_t i = 0; g && i < g->entries.len; i++) {
        Entry *e = g->entries.data[i];
        if (!strcmp(e->key, key))
            return e;
    }
    return NULL;
}
static void set_raw(ChKeyFile *k, const char *group, const char *key, char *escaped) {
    Group *g = find_group(k, group, true);
    for (size_t i = 0; i < g->entries.len; i++) {
        Entry *e = g->entries.data[i];
        if (!strcmp(e->key, key)) {
            free(e->value);
            e->value = escaped;
            return;
        }
    }
    Entry *e = ch_new0(Entry, 1);
    e->key = ch_strdup(key);
    e->value = escaped;
    ch_vec_push(&g->entries, e);
}
bool ch_keyfile_parse(ChKeyFile *k, const char *text, ChError **error) {
    if (!ch_utf8_validate(text, -1, NULL)) {
        ch_fail_literal(error, "Key file contains invalid UTF-8");
        return false;
    }
    char **lines = ch_strsplit(text, "\n");
    Group *current = NULL;
    bool ok = true;
    for (size_t i = 0; lines[i] && ok; i++) {
        char *line = lines[i];
        size_t n = strlen(line);
        if (n && line[n - 1] == '\r')
            line[--n] = 0;
        const char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (!*s || *s == '#')
            continue;
        if (*s == '[') {
            const char *end = strchr(s, ']');
            if (!end) {
                ch_fail(error, "Key file contains an invalid group on line %zu", i + 1);
                ok = false;
                break;
            }
            char *name = ch_strndup(s + 1, (size_t)(end - s - 1));
            current = find_group(k, name, true);
            free(name);
            continue;
        }
        const char *eq = strchr(s, '=');
        if (!eq || !current) {
            ch_fail(error, "Key file contains line “%s” which is not a key-value pair, group, or comment", line);
            ok = false;
            break;
        }
        char *key = ch_strndup(s, (size_t)(eq - s));
        ch_strstrip(key);
        const char *value = eq + 1;
        while (*value == ' ' || *value == '\t')
            value++;
        if (*key && !strchr(key, '['))
            set_raw(k, current->name, key, ch_strdup(value));
        free(key);
    }
    ch_strfreev(lines);
    return ok;
}
bool ch_keyfile_load(ChKeyFile *k, const char *path, ChError **error) {
    char *text = NULL;
    if (!ch_file_get_contents(path, &text, NULL, error))
        return false;
    bool ok = ch_keyfile_parse(k, text, error);
    free(text);
    return ok;
}
char *ch_keyfile_to_data(const ChKeyFile *k, size_t *length) {
    ChBuf b;
    ch_buf_init(&b);
    for (size_t i = 0; i < k->groups.len; i++) {
        Group *g = k->groups.data[i];
        if (i)
            ch_buf_append_c(&b, '\n');
        ch_buf_printf(&b, "[%s]\n", g->name);
        for (size_t j = 0; j < g->entries.len; j++) {
            Entry *e = g->entries.data[j];
            ch_buf_printf(&b, "%s=%s\n", e->key, e->value);
        }
    }
    if (length)
        *length = b.len;
    return ch_buf_steal(&b);
}
bool ch_keyfile_save(const ChKeyFile *k, const char *path, ChError **error) {
    size_t n;
    char *text = ch_keyfile_to_data(k, &n);
    bool ok = ch_file_set_contents(path, text, (ptrdiff_t)n, 0600, error);
    free(text);
    return ok;
}
static char *escape(const char *s, bool list) {
    ChBuf b;
    ch_buf_init(&b);
    for (const char *p = s; *p; p++) {
        switch (*p) {
        case ' ':
            ch_buf_append(&b, p == s ? "\\s" : " ");
            break;
        case '\n':
            ch_buf_append(&b, "\\n");
            break;
        case '\t':
            ch_buf_append(&b, "\\t");
            break;
        case '\r':
            ch_buf_append(&b, "\\r");
            break;
        case '\\':
            ch_buf_append(&b, "\\\\");
            break;
        case ';':
            ch_buf_append(&b, list ? "\\;" : ";");
            break;
        default:
            ch_buf_append_c(&b, *p);
        }
    }
    return ch_buf_steal(&b);
}
/* Unescapes one value; for lists stops at an unescaped ';' and reports where. */
static char *unescape(const char *s, bool list, const char **end) {
    ChBuf b;
    ch_buf_init(&b);
    const char *p = s;
    for (; *p; p++) {
        if (list && *p == ';')
            break;
        if (*p != '\\' || !p[1]) {
            ch_buf_append_c(&b, *p);
            continue;
        }
        p++;
        char c = *p == 's' ? ' ' : *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
        if (c == *p && c != '\\' && c != ';')
            ch_buf_append_c(&b, '\\');
        ch_buf_append_c(&b, c);
    }
    if (end)
        *end = p;
    return ch_buf_steal(&b);
}
bool ch_keyfile_has(const ChKeyFile *k, const char *group, const char *key) {
    return find_entry(k, group, key) != NULL;
}
char *ch_keyfile_get_string(const ChKeyFile *k, const char *group, const char *key) {
    Entry *e = find_entry(k, group, key);
    return e ? unescape(e->value, false, NULL) : NULL;
}
static bool missing(const Entry *e, const char *group, const char *key, ChError **error) {
    if (e)
        return false;
    ch_fail(error, "Key file does not have key “%s” in group “%s”", key, group);
    return true;
}
bool ch_keyfile_get_int(const ChKeyFile *k, const char *group, const char *key, int *value, ChError **error) {
    Entry *e = find_entry(k, group, key);
    if (missing(e, group, key, error))
        return false;
    char *text = unescape(e->value, false, NULL), *end = NULL;
    ch_strstrip(text);
    errno = 0;
    long v = strtol(text, &end, 10);
    bool ok = *text && end && !*end && !errno && v >= INT32_MIN && v <= INT32_MAX;
    if (ok)
        *value = (int)v;
    else
        ch_fail(error, "Value “%s” cannot be interpreted as a number.", text);
    free(text);
    return ok;
}
bool ch_keyfile_get_double(const ChKeyFile *k, const char *group, const char *key, double *value, ChError **error) {
    Entry *e = find_entry(k, group, key);
    if (missing(e, group, key, error))
        return false;
    char *text = unescape(e->value, false, NULL), *end = NULL;
    ch_strstrip(text);
    double v = strtod(text, &end);
    bool ok = *text && end && !*end && isfinite(v);
    if (ok)
        *value = v;
    else
        ch_fail(error, "Value “%s” cannot be interpreted as a float number.", text);
    free(text);
    return ok;
}
bool ch_keyfile_get_bool(const ChKeyFile *k, const char *group, const char *key, bool *value, ChError **error) {
    Entry *e = find_entry(k, group, key);
    if (missing(e, group, key, error))
        return false;
    char *text = unescape(e->value, false, NULL);
    ch_strstrip(text);
    bool ok = true;
    if (!strcmp(text, "true") || !strcmp(text, "1"))
        *value = true;
    else if (!strcmp(text, "false") || !strcmp(text, "0"))
        *value = false;
    else {
        ch_fail(error, "Value “%s” cannot be interpreted as a boolean.", text);
        ok = false;
    }
    free(text);
    return ok;
}
char **ch_keyfile_get_list(const ChKeyFile *k, const char *group, const char *key) {
    Entry *e = find_entry(k, group, key);
    if (!e)
        return NULL;
    ChVec items;
    ch_vec_init(&items, NULL);
    const char *p = e->value;
    while (*p) {
        const char *end;
        ch_vec_push(&items, unescape(p, true, &end));
        p = *end == ';' ? end + 1 : end;
    }
    ch_vec_push(&items, NULL);
    return (char **)items.data;
}
char **ch_keyfile_keys(const ChKeyFile *k, const char *group) {
    Group *g = find_group(k, group, false);
    ChVec keys;
    ch_vec_init(&keys, NULL);
    for (size_t i = 0; g && i < g->entries.len; i++)
        ch_vec_push(&keys, ch_strdup(((Entry *)g->entries.data[i])->key));
    ch_vec_push(&keys, NULL);
    return (char **)keys.data;
}
void ch_keyfile_set_string(ChKeyFile *k, const char *group, const char *key, const char *value) {
    set_raw(k, group, key, escape(value ? value : "", false));
}
void ch_keyfile_set_int(ChKeyFile *k, const char *group, const char *key, int value) {
    set_raw(k, group, key, ch_strdup_printf("%d", value));
}
void ch_keyfile_set_double(ChKeyFile *k, const char *group, const char *key, double value) {
    /* Shortest text that reads back as the same double. */
    char text[64];
    for (int precision = 1; precision <= 17; precision++) {
        snprintf(text, sizeof(text), "%.*g", precision, value);
        if (strtod(text, NULL) == value)
            break;
    }
    set_raw(k, group, key, ch_strdup(text));
}
void ch_keyfile_set_bool(ChKeyFile *k, const char *group, const char *key, bool value) {
    set_raw(k, group, key, ch_strdup(value ? "true" : "false"));
}
void ch_keyfile_set_list(ChKeyFile *k, const char *group, const char *key, const char *const *values, size_t n) {
    ChBuf b;
    ch_buf_init(&b);
    for (size_t i = 0; i < n; i++) {
        char *item = escape(values[i], true);
        ch_buf_append(&b, item);
        ch_buf_append_c(&b, ';');
        free(item);
    }
    set_raw(k, group, key, ch_buf_steal(&b));
}
int ch_keyfile_int_or(const ChKeyFile *k, const char *group, const char *key, int fallback) {
    int value = fallback;
    if (!ch_keyfile_get_int(k, group, key, &value, NULL))
        value = fallback;
    return value;
}
bool ch_keyfile_bool_or(const ChKeyFile *k, const char *group, const char *key, bool fallback) {
    bool value = fallback;
    if (!ch_keyfile_get_bool(k, group, key, &value, NULL))
        value = fallback;
    return value;
}
