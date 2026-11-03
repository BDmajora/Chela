/* Isolated test double for libsecret. Only accepts dummy fixture passwords and
 * writes inside the temporary test fixture; never opens a real desktop wallet.
 * Attribute tables arrive as GHashTables from the GLib that Chela loaded, so the
 * few GLib calls needed are declared here instead of including GLib headers. */
#include "../src/core/base.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void *g_hash_table_lookup(void *table, const void *key);
char *g_strdup(const char *s);
static const char *attr(void *table, const char *key) {
    const char *value = g_hash_table_lookup(table, key);
    assert(value);
    return value;
}
static char *item_path(void *attrs) {
    const char *root = getenv("CHELA_FIXTURE");
    assert(root);
    assert(!strcmp(attr(attrs, "xdg:schema"), "io.github.chela.SSH"));
    assert(!strcmp(attr(attrs, "application"), "chela"));
    assert(!strcmp(attr(attrs, "protocol"), "ssh"));
    char *id = ch_strdup_printf("%s/%s/%s", attr(attrs, "host"), attr(attrs, "user"), attr(attrs, "port"));
    char *hash = ch_sha256_hex(id, strlen(id));
    char *dir = ch_build_path(root, "keyring", NULL);
    ch_mkdir_p(dir, 0700);
    char *path = ch_build_path(dir, hash, NULL);
    free(dir);
    free(hash);
    free(id);
    return path;
}
char *secret_password_lookupv_sync(const void *schema, void *attrs, void *cancel, void **error) {
    (void)schema, (void)cancel, (void)error;
    char *path = item_path(attrs), *value = NULL;
    char *result = NULL;
    if (ch_file_get_contents(path, &value, NULL, NULL))
        result = g_strdup(value);
    free(value);
    free(path);
    return result;
}
int secret_password_storev_sync(const void *schema, void *attrs, const char *collection, const char *label,
                                const char *password, void *cancel, void **error) {
    (void)schema, (void)collection, (void)label, (void)cancel, (void)error;
    assert(ch_str_has_prefix(password, "fixture-"));
    char *path = item_path(attrs);
    bool ok = ch_file_set_contents(path, password, -1, 0600, NULL);
    free(path);
    return ok;
}
int secret_password_clearv_sync(const void *schema, void *attrs, void *cancel, void **error) {
    (void)schema, (void)cancel, (void)error;
    char *path = item_path(attrs);
    int ok = unlink(path) == 0;
    free(path);
    return ok;
}
