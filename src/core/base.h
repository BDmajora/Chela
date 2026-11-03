/* Portable C11 + POSIX foundation for Chela. No GLib, no GUI toolkit.
 * Platform-specific pieces (processes, sockets, directories) live behind these
 * functions so a Windows or macOS port replaces implementations, not callers. */
#ifndef CHELA_BASE_H
#define CHELA_BASE_H
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define CH_N_ELEMENTS(a) (sizeof(a) / sizeof((a)[0]))
#define CH_MIN(a, b) ((a) < (b) ? (a) : (b))
#define CH_MAX(a, b) ((a) > (b) ? (a) : (b))
#define CH_CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define CH_USEC_PER_SEC INT64_C(1000000)
#if defined(__GNUC__)
#define CH_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define CH_PRINTF(f, a)
#endif

/* Errors: a kind for control flow plus a human-readable message. */
typedef enum {
    CH_ERR_FAILED = 1,
    CH_ERR_CANCELLED,
    CH_ERR_TIMED_OUT,
    CH_ERR_HTTP, /* code is the HTTP status */
    CH_ERR_IO,   /* code is errno */
    CH_ERR_DB,   /* code is the SQLite result */
} ChErrorKind;
typedef struct {
    ChErrorKind kind;
    int code;
    char *message;
} ChError;
void ch_set_error(ChError **error, ChErrorKind kind, int code, const char *format, ...) CH_PRINTF(4, 5);
void ch_set_error_literal(ChError **error, ChErrorKind kind, int code, const char *message);
void ch_prefix_error(ChError **error, const char *format, ...) CH_PRINTF(2, 3);
void ch_propagate_error(ChError **dest, ChError *src);
void ch_error_free(ChError *error);
void ch_clear_error(ChError **error);
bool ch_error_is(const ChError *error, ChErrorKind kind);
#define ch_fail(error, ...) ch_set_error((error), CH_ERR_FAILED, 1, __VA_ARGS__)
#define ch_fail_literal(error, message) ch_set_error_literal((error), CH_ERR_FAILED, 1, (message))

/* Memory and strings. Allocation failure aborts, like the rest of the desktop. */
void *ch_alloc(size_t size);
void *ch_alloc0(size_t size);
void *ch_realloc(void *p, size_t size);
#define ch_new0(type, n) ((type *)ch_alloc0(sizeof(type) * (n)))
char *ch_strdup(const char *s);
char *ch_strndup(const char *s, size_t n);
char *ch_strdup_printf(const char *format, ...) CH_PRINTF(1, 2);
char *ch_strdup_vprintf(const char *format, va_list args);
char *ch_strconcat(const char *first, ...); /* NULL-terminated */
size_t ch_strlcpy(char *dest, const char *src, size_t size);
bool ch_str_has_prefix(const char *s, const char *prefix);
bool ch_str_has_suffix(const char *s, const char *suffix);
char *ch_strstrip(char *s); /* in place; returns s */
char *ch_ascii_strdown(const char *s, ptrdiff_t len);
int ch_ascii_strcasecmp(const char *a, const char *b);
int ch_strcmp0(const char *a, const char *b);
char **ch_strsplit(const char *s, const char *delimiter); /* NULL-terminated */
void ch_strfreev(char **v);
size_t ch_strv_length(char **v);
#define ch_clear_pointer(pp, destroy)                                                                        \
    do {                                                                                                     \
        if (*(pp)) {                                                                                         \
            destroy(*(pp));                                                                                  \
            *(pp) = NULL;                                                                                    \
        }                                                                                                    \
    } while (0)
static inline void *ch_steal(void *pp) {
    void **p = pp, *v = *p;
    *p = NULL;
    return v;
}

/* Growable string buffer. */
typedef struct {
    char *str;
    size_t len, cap;
} ChBuf;
void ch_buf_init(ChBuf *b);
ChBuf *ch_buf_new(const char *init);
void ch_buf_append(ChBuf *b, const char *s);
void ch_buf_append_len(ChBuf *b, const char *s, size_t n);
void ch_buf_append_c(ChBuf *b, char c);
void ch_buf_printf(ChBuf *b, const char *format, ...) CH_PRINTF(2, 3);
void ch_buf_truncate(ChBuf *b, size_t len);
void ch_buf_erase(ChBuf *b, size_t pos, size_t n);
void ch_buf_assign(ChBuf *b, const char *s);
void ch_buf_clear(ChBuf *b);           /* frees storage of an init()ed buffer */
char *ch_buf_free(ChBuf *b, bool keep); /* for ch_buf_new(); returns the string when keep */
char *ch_buf_steal(ChBuf *b);          /* for init()ed buffers */

/* Pointer array with an optional element destructor. */
typedef struct {
    void **data;
    size_t len, cap;
    void (*free_fn)(void *);
} ChVec;
ChVec *ch_vec_new(void (*free_fn)(void *));
void ch_vec_init(ChVec *v, void (*free_fn)(void *));
void ch_vec_push(ChVec *v, void *item);
void ch_vec_insert(ChVec *v, size_t index, void *item);
void ch_vec_remove(ChVec *v, size_t index); /* frees the element */
void *ch_vec_steal(ChVec *v, size_t index);
void ch_vec_clear(ChVec *v);
void ch_vec_free(ChVec *v);
void ch_vec_sort(ChVec *v, int (*compare)(const void *a, const void *b, void *data), void *data); /* stable */
#define ch_vec_at(v, i) ((v)->data[i])

/* String-keyed map with insertion order (small sets: pins, sections, timings). */
typedef struct {
    char **keys;
    void **values;
    size_t len, cap;
    void (*free_value)(void *);
} ChMap;
void ch_map_init(ChMap *m, void (*free_value)(void *));
void ch_map_clear(ChMap *m);
void *ch_map_get(const ChMap *m, const char *key);
bool ch_map_has(const ChMap *m, const char *key);
void ch_map_set(ChMap *m, const char *key, void *value); /* replaces and frees the old value */
bool ch_map_remove(ChMap *m, const char *key);

/* UTF-8 */
bool ch_utf8_validate(const char *s, ptrdiff_t len, const char **end);
char *ch_utf8_make_valid(const char *s, ptrdiff_t len);
uint32_t ch_utf8_get(const char *s, const char **next); /* decodes one valid character */
const char *ch_utf8_next(const char *s);
const char *ch_utf8_prev(const char *start, const char *s);
size_t ch_utf8_strlen(const char *s, ptrdiff_t len);
const char *ch_utf8_offset(const char *s, size_t chars);
char *ch_utf8_substring(const char *s, size_t start, size_t end);
int ch_utf8_encode(uint32_t c, char out[4]);
char *ch_utf8_casefold(const char *s);
char *ch_utf8_upper_first(const char *s); /* first character, upper-cased */
int ch_utf8_collate(const char *a, const char *b);
bool ch_unichar_isspace(uint32_t c);

/* Time, identifiers, hashing. */
int64_t ch_monotonic_us(void);
int64_t ch_real_us(void);
void ch_usleep(int64_t us);
char *ch_uuid_random(void);
bool ch_uuid_valid(const char *s);
void ch_random_bytes(void *buf, size_t n);
char *ch_sha256_hex(const char *data, size_t len);

/* Files, paths and user directories. */
char *ch_path_basename(const char *path);
char *ch_path_dirname(const char *path);
char *ch_build_path(const char *first, ...); /* NULL-terminated */
bool ch_path_is_absolute(const char *path);
char *ch_canonicalize(const char *path, const char *relative_to);
bool ch_file_exists(const char *path);
bool ch_file_is_dir(const char *path);
bool ch_file_is_regular(const char *path);
int ch_mkdir_p(const char *path, int mode);
bool ch_file_get_contents(const char *path, char **contents, size_t *length, ChError **error);
bool ch_file_set_contents(const char *path, const char *data, ptrdiff_t length, int mode, ChError **error);
char *ch_read_link(const char *path, ChError **error);
const char *ch_home_dir(void);
const char *ch_user_name(void);
const char *ch_real_name(void);
char *ch_user_data_dir(void);
char *ch_user_config_dir(void);
char *ch_user_runtime_dir(void);
char *ch_current_dir(void);
const char *ch_getenv(const char *name);

/* Shell-style argument parsing and quoting (no shell is run). */
bool ch_shell_parse_argv(const char *command, int *argc, char ***argv, ChError **error);
char *ch_shell_quote(const char *s);

/* Cancellation tokens shared between the UI and workers. */
typedef struct ChCancel ChCancel;
ChCancel *ch_cancel_new(void);
ChCancel *ch_cancel_ref(ChCancel *c);
void ch_cancel_unref(ChCancel *c);
void ch_cancel_cancel(ChCancel *c);
bool ch_cancel_is_cancelled(const ChCancel *c);
bool ch_cancel_check(const ChCancel *c, ChError **error); /* sets "Stopped" when cancelled */

/* Threads. */
typedef void (*ChThreadFn)(void *data);
bool ch_thread_run(ChThreadFn fn, void *data); /* detached */
typedef pthread_mutex_t ChMutex;
typedef pthread_cond_t ChCond;
#define ch_mutex_init(m) pthread_mutex_init((m), NULL)
#define ch_mutex_clear(m) pthread_mutex_destroy(m)
#define ch_mutex_lock(m) pthread_mutex_lock(m)
#define ch_mutex_unlock(m) pthread_mutex_unlock(m)
#define ch_cond_init(c) pthread_cond_init((c), NULL)
#define ch_cond_clear(c) pthread_cond_destroy(c)
#define ch_cond_wait(c, m) pthread_cond_wait((c), (m))
#define ch_cond_signal(c) pthread_cond_signal(c)

/* Subprocesses. */
enum {
    CH_PROC_STDIN = 1 << 0,
    CH_PROC_STDOUT = 1 << 1,
    CH_PROC_STDERR = 1 << 2,
    CH_PROC_STDERR_SILENCE = 1 << 3,
    CH_PROC_STDERR_MERGE = 1 << 4,
    CH_PROC_NEW_GROUP = 1 << 5,
    CH_PROC_NEW_SESSION = 1 << 6,
    CH_PROC_SEARCH_PATH = 1 << 7,
    /* SIGTERM the child when the spawning thread (or the whole process) exits. */
    CH_PROC_DIE_WITH_PARENT = 1 << 8,
};
typedef struct ChProc ChProc;
typedef struct {
    const char *const *argv;
    const char *const *env; /* "NAME=value" overrides, NULL-terminated */
    const char *cwd;
    int flags;
} ChSpawn;
ChProc *ch_proc_spawn(const ChSpawn *spawn, ChError **error);
pid_t ch_proc_pid(const ChProc *p);
int ch_proc_stdin(const ChProc *p);
int ch_proc_stdout(const ChProc *p);
int ch_proc_stderr(const ChProc *p);
/* Writes input, reads stdout/stderr to EOF, waits. Cancellation kills the process group. */
bool ch_proc_communicate(ChProc *p, const char *input, const ChCancel *cancel, char **out, char **err,
                         ChError **error);
bool ch_proc_wait(ChProc *p, int *status);
bool ch_proc_try_wait(ChProc *p, int *status); /* true when exited */
bool ch_proc_succeeded(const ChProc *p);
void ch_proc_kill(ChProc *p); /* SIGKILL to the process and its group */
void ch_proc_free(ChProc *p); /* kills and reaps when still running */

/* INI files compatible with the desktop key-file format used by earlier releases. */
typedef struct ChKeyFile ChKeyFile;
ChKeyFile *ch_keyfile_new(void);
void ch_keyfile_free(ChKeyFile *k);
bool ch_keyfile_load(ChKeyFile *k, const char *path, ChError **error);
bool ch_keyfile_parse(ChKeyFile *k, const char *text, ChError **error);
char *ch_keyfile_to_data(const ChKeyFile *k, size_t *length);
bool ch_keyfile_save(const ChKeyFile *k, const char *path, ChError **error); /* atomic, 0600 */
bool ch_keyfile_has(const ChKeyFile *k, const char *group, const char *key);
char *ch_keyfile_get_string(const ChKeyFile *k, const char *group, const char *key);
bool ch_keyfile_get_int(const ChKeyFile *k, const char *group, const char *key, int *value, ChError **error);
bool ch_keyfile_get_double(const ChKeyFile *k, const char *group, const char *key, double *value, ChError **error);
bool ch_keyfile_get_bool(const ChKeyFile *k, const char *group, const char *key, bool *value, ChError **error);
char **ch_keyfile_get_list(const ChKeyFile *k, const char *group, const char *key);
char **ch_keyfile_keys(const ChKeyFile *k, const char *group);
void ch_keyfile_set_string(ChKeyFile *k, const char *group, const char *key, const char *value);
void ch_keyfile_set_int(ChKeyFile *k, const char *group, const char *key, int value);
void ch_keyfile_set_double(ChKeyFile *k, const char *group, const char *key, double value);
void ch_keyfile_set_bool(ChKeyFile *k, const char *group, const char *key, bool value);
void ch_keyfile_set_list(ChKeyFile *k, const char *group, const char *key, const char *const *values, size_t n);
/* Convenience readers with defaults (no error reporting). */
int ch_keyfile_int_or(const ChKeyFile *k, const char *group, const char *key, int fallback);
bool ch_keyfile_bool_or(const ChKeyFile *k, const char *group, const char *key, bool fallback);

/* URIs: scheme://[userinfo@]host[:port][path][?query][#fragment]. */
typedef struct {
    char *scheme, *userinfo, *host, *path, *query, *fragment;
    int port; /* -1 when absent */
} ChUri;
ChUri *ch_uri_parse(const char *text);
void ch_uri_free(ChUri *u);
char *ch_uri_join(const char *scheme, const char *userinfo, const char *host, int port, const char *path,
                  const char *query, const char *fragment);
#endif
