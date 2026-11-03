/* Desktop keyring through the Secret Service (KWallet, GNOME Keyring, KeePassXC).
 * libsecret is loaded on first use, so Chela neither links nor needs its headers.
 * These are the public libsecret v1 password APIs; no wallet files are opened. */
#include "chela.h"
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t domain;
    int code;
    char *message;
} SecretError; /* GError's public layout */
typedef unsigned (*HashFn)(const void *);
typedef int (*EqualFn)(const void *, const void *);
static struct {
    void *secret, *glib;
    char *(*lookup)(const void *, void *, void *, SecretError **);
    int (*store)(const void *, void *, const char *, const char *, const char *, void *, SecretError **);
    int (*clear)(const void *, void *, void *, SecretError **);
    void *(*table_new)(HashFn, EqualFn, void (*)(void *), void (*)(void *));
    void (*table_insert)(void *, void *, void *);
    void (*table_unref)(void *);
    HashFn str_hash;
    EqualFn str_equal;
    void (*error_free)(SecretError *);
    void (*g_free)(void *);
    char *(*g_strdup)(const char *);
} api;
static pthread_once_t api_once = PTHREAD_ONCE_INIT;
static void load_api(void) {
    api.secret = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
    api.glib = dlopen("libglib-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!api.secret || !api.glib)
        return;
    api.lookup = (char *(*)(const void *, void *, void *, SecretError **))dlsym(api.secret, "secret_password_lookupv_sync");
    api.store = (int (*)(const void *, void *, const char *, const char *, const char *, void *, SecretError **))dlsym(
        api.secret, "secret_password_storev_sync");
    api.clear = (int (*)(const void *, void *, void *, SecretError **))dlsym(api.secret, "secret_password_clearv_sync");
    api.table_new = (void *(*)(HashFn, EqualFn, void (*)(void *), void (*)(void *)))dlsym(api.glib, "g_hash_table_new_full");
    api.table_insert = (void (*)(void *, void *, void *))dlsym(api.glib, "g_hash_table_insert");
    api.table_unref = (void (*)(void *))dlsym(api.glib, "g_hash_table_unref");
    api.str_hash = (HashFn)dlsym(api.glib, "g_str_hash");
    api.str_equal = (EqualFn)dlsym(api.glib, "g_str_equal");
    api.error_free = (void (*)(SecretError *))dlsym(api.glib, "g_error_free");
    api.g_free = (void (*)(void *))dlsym(api.glib, "g_free");
    api.g_strdup = (char *(*)(const char *))dlsym(api.glib, "g_strdup");
}
static bool secret_api(ChError **error) {
    pthread_once(&api_once, load_api);
    if (api.lookup && api.store && api.clear && api.table_new && api.table_insert && api.table_unref && api.str_hash &&
        api.str_equal && api.error_free && api.g_free && api.g_strdup)
        return true;
    ch_fail_literal(error, "The desktop keyring is unavailable. Install libsecret and enable your desktop's password "
                           "wallet, or turn off Remember login.");
    return false;
}
static void put(void *table, const char *key, const char *value) {
    api.table_insert(table, api.g_strdup(key), api.g_strdup(value));
}
static void *attributes(const ChConfig *c) {
    void *values = api.table_new(api.str_hash, api.str_equal, api.g_free, api.g_free);
    char *host = ch_ascii_strdown(c->ssh_host, -1), *port = ch_strdup_printf("%d", c->ssh_port);
    put(values, "xdg:schema", "io.github.chela.SSH");
    put(values, "application", "chela");
    put(values, "protocol", "ssh");
    put(values, "host", host);
    put(values, "user", c->ssh_user);
    put(values, "port", port);
    free(host);
    free(port);
    return values;
}
static bool take_error(SecretError *e, ChError **error) {
    if (!e)
        return false;
    ch_fail_literal(error, e->message ? e->message : "Keyring error");
    api.error_free(e);
    return true;
}
void ch_secret_free(char *secret) {
    if (secret) {
        volatile char *p = secret;
        size_t length = strlen(secret);
        while (length--)
            *p++ = 0;
        free(secret);
    }
}
char *ch_ssh_password_lookup(const ChConfig *c, const ChCancel *cancel, ChError **error) {
    if (ch_cancel_check(cancel, error) || !secret_api(error))
        return NULL;
    void *values = attributes(c);
    SecretError *e = NULL;
    char *found = api.lookup(NULL, values, NULL, &e);
    api.table_unref(values);
    char *password = found ? ch_strdup(found) : NULL;
    if (found) {
        volatile char *p = found;
        for (size_t n = strlen(found); n--;)
            *p++ = 0;
        api.g_free(found);
    }
    take_error(e, error);
    if (ch_cancel_check(cancel, error))
        ch_clear_pointer(&password, ch_secret_free);
    return password;
}
bool ch_ssh_password_store(const ChConfig *c, const char *password, const ChCancel *cancel, ChError **error) {
    if (ch_cancel_check(cancel, error) || !secret_api(error))
        return false;
    void *values = attributes(c);
    char *title = ch_strdup_printf("Chela SSH · %s@%s:%d", c->ssh_user, c->ssh_host, c->ssh_port);
    SecretError *e = NULL;
    bool ok = api.store(NULL, values, NULL, title, password, NULL, &e) != 0;
    free(title);
    api.table_unref(values);
    if (take_error(e, error))
        ok = false;
    return ok;
}
bool ch_ssh_password_forget(const ChConfig *c, const ChCancel *cancel, ChError **error) {
    if (ch_cancel_check(cancel, error) || !secret_api(error))
        return false;
    void *values = attributes(c);
    SecretError *e = NULL;
    api.clear(NULL, values, NULL, &e);
    api.table_unref(values);
    return !take_error(e, error);
}
