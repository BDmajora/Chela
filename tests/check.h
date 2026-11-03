/* Minimal test harness for C tests: no GLib, abort on the first failed check. */
#ifndef CHELA_CHECK_H
#define CHELA_CHECK_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK_FAIL(...)                                                                                      \
    do {                                                                                                     \
        fprintf(stderr, "%s:%d: ", __FILE__, __LINE__);                                                      \
        fprintf(stderr, __VA_ARGS__);                                                                        \
        fputc('\n', stderr);                                                                                 \
        abort();                                                                                             \
    } while (0)
#define CHECK(x)                                                                                             \
    do {                                                                                                     \
        if (!(x))                                                                                            \
            CHECK_FAIL("check failed: %s", #x);                                                              \
    } while (0)
#define CHECK_FALSE(x) CHECK(!(x))
#define CHECK_NULL(x) CHECK((x) == NULL)
#define CHECK_NONNULL(x) CHECK((x) != NULL)
#define CHECK_STR(a, op, b)                                                                                  \
    do {                                                                                                     \
        const char *check_a = (a), *check_b = (b);                                                          \
        int check_r = check_a && check_b ? strcmp(check_a, check_b) : (check_a == check_b ? 0 : check_a ? 1 : -1); \
        if (!(check_r op 0))                                                                                 \
            CHECK_FAIL("“%s” %s “%s” failed: \"%s\" vs \"%s\"", #a, #op, #b, check_a ? check_a : "(null)",   \
                       check_b ? check_b : "(null)");                                                        \
    } while (0)
#define CHECK_INT(a, op, b)                                                                                  \
    do {                                                                                                     \
        long long check_a = (long long)(a), check_b = (long long)(b);                                        \
        if (!(check_a op check_b))                                                                           \
            CHECK_FAIL("%s %s %s failed: %lld vs %lld", #a, #op, #b, check_a, check_b);                      \
    } while (0)
#define CHECK_FLOAT(a, op, b)                                                                                \
    do {                                                                                                     \
        double check_a = (a), check_b = (b);                                                                 \
        if (!(check_a op check_b))                                                                           \
            CHECK_FAIL("%s %s %s failed: %g vs %g", #a, #op, #b, check_a, check_b);                          \
    } while (0)
#define CHECK_NO_ERROR(e)                                                                                    \
    do {                                                                                                     \
        if (e)                                                                                               \
            CHECK_FAIL("unexpected error: %s", (e)->message);                                                \
    } while (0)
#define CHECK_ERROR(e, k)                                                                                    \
    do {                                                                                                     \
        if (!(e) || (e)->kind != (k))                                                                        \
            CHECK_FAIL("expected error kind %d, got %s", (int)(k), (e) ? (e)->message : "no error");        \
    } while (0)

typedef struct {
    const char *name;
    void (*fn)(void);
} CheckTest;
static CheckTest check_tests[64];
static int check_count;
static const char *check_filter;
static inline void check_add(const char *name, void (*fn)(void)) {
    if (check_count < 64)
        check_tests[check_count++] = (CheckTest){name, fn};
}
static inline void check_message(const char *format, const char *value) {
    if (getenv("CHECK_VERBOSE")) {
        fprintf(stderr, "  # ");
        fprintf(stderr, format, value);
        fputc('\n', stderr);
    }
}
static inline int check_run(int argc, char **argv) {
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-p") && i + 1 < argc)
            check_filter = argv[++i];
    int ran = 0;
    for (int i = 0; i < check_count; i++) {
        if (check_filter && strncmp(check_filter, check_tests[i].name, strlen(check_filter)))
            continue;
        printf("# %s\n", check_tests[i].name);
        fflush(stdout);
        check_tests[i].fn();
        printf("ok %d %s\n", ++ran, check_tests[i].name);
    }
    printf("1..%d\n", ran);
    return 0;
}
#endif
