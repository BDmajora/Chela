/* Command sandbox: Landlock rules under which a shell command, and everything it
 * starts, can write only in the session's folders, temporary folders and package
 * caches. Reading and the network stay open, because builds need the rest of the
 * system; what this prevents is a command damaging anything else on the computer.
 * The kernel enforces it, so it holds however a command is written. */
#define _GNU_SOURCE
#include "chela.h"
#include <fcntl.h>
#include <linux/landlock.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define FILE_WRITES ((__u64)LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_TRUNCATE)
#define TREE_WRITES                                                                                          \
    ((__u64)LANDLOCK_ACCESS_FS_WRITE_FILE | LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE | \
     LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR | LANDLOCK_ACCESS_FS_MAKE_REG |              \
     LANDLOCK_ACCESS_FS_MAKE_SOCK | LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |           \
     LANDLOCK_ACCESS_FS_MAKE_SYM)

/* Under the home folder: downloads and build output that tools share between projects.
 * Their settings and programs, such as ~/.cargo/bin, stay out of reach. */
static const char *const caches[] = {
    ".cache", ".npm", ".yarn/berry", ".pnpm-store", ".local/share/pnpm/store", ".bun/install/cache",
    ".cargo/registry", ".cargo/git", ".cargo/.package-cache", ".cargo/.package-cache-mutate", ".cargo/.global-cache",
    "go/pkg/mod", ".m2/repository", ".gradle/caches", ".gradle/wrapper", ".gradle/daemon", ".gradle/native",
    ".ccache", ".nuget/packages"};

static void allow(int ruleset, __u64 access, const char *path) {
    int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0)
        return; /* not on this computer */
    struct stat st;
    if (!fstat(fd, &st)) {
        struct landlock_path_beneath_attr rule = {.allowed_access = S_ISDIR(st.st_mode) ? access : access & FILE_WRITES,
                                                  .parent_fd = fd};
        syscall(__NR_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0);
    }
    close(fd);
}
static char *read_stripped(const char *folder, const char *name) {
    char *path = ch_build_path(folder, name, NULL), *text = NULL;
    size_t size = 0;
    if (!ch_file_is_regular(path) || !ch_file_get_contents(path, &text, &size, NULL))
        text = NULL;
    free(path);
    return text ? ch_strstrip(text) : NULL;
}
static char *real_path(const char *base, const char *path) {
    char *joined = ch_path_is_absolute(path) ? ch_strdup(path) : ch_build_path(base, path, NULL);
    char *real = realpath(joined, NULL); /* links resolved: what is checked is what gets opened */
    free(joined);
    return real;
}
/* A linked git worktree keeps its index and history in the main repository, so commits
 * have to write there. Commands can rewrite the .git file in their own folder, so it
 * proves nothing alone: the repository must hold a matching record of this worktree,
 * and only a repository is ever opened this way. */
static void allow_linked_repository(int ruleset, __u64 access, const char *folder) {
    char *link = read_stripped(folder, ".git");
    if (!link || !ch_str_has_prefix(link, "gitdir:")) {
        free(link);
        return;
    }
    char *worktree = real_path(folder, ch_strstrip(link + strlen("gitdir:")));
    char *record = worktree ? read_stripped(worktree, "gitdir") : NULL, *listed = record ? real_path(worktree, record) : NULL;
    char *named = worktree ? read_stripped(worktree, "commondir") : NULL, *common = named ? real_path(worktree, named) : NULL;
    char *ours = real_path(folder, ".git"), *expected = common ? ch_strconcat(common, "/worktrees/", NULL) : NULL;
    if (listed && ours && expected && !strcmp(listed, ours) && ch_str_has_prefix(worktree, expected) &&
        ch_str_has_suffix(common, ".git"))
        allow(ruleset, access, common);
    free(link);
    free(worktree);
    free(record);
    free(listed);
    free(named);
    free(common);
    free(ours);
    free(expected);
}
int ch_sandbox_open(const ChRun *run, ChError **error) {
    long abi = ch_getenv("CHELA_TEST_NO_LANDLOCK") ? -1 : syscall(__NR_landlock_create_ruleset, NULL, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi < 1) {
        ch_fail_literal(error, "this system's kernel does not offer Landlock");
        return -1;
    }
    __u64 access = TREE_WRITES | (abi >= 2 ? (__u64)LANDLOCK_ACCESS_FS_REFER : 0) |
                   (abi >= 3 ? (__u64)LANDLOCK_ACCESS_FS_TRUNCATE : 0);
    struct landlock_ruleset_attr attr = {.handled_access_fs = access};
    int ruleset = (int)syscall(__NR_landlock_create_ruleset, &attr, sizeof(attr), 0);
    if (ruleset < 0) {
        ch_fail_literal(error, "this system could not set up Landlock");
        return -1;
    }
    if (run->project && *run->project) {
        allow(ruleset, access, run->project);
        allow_linked_repository(ruleset, access, run->project);
    }
    for (size_t i = 0; run->folders && run->folders[i]; i++) {
        allow(ruleset, access, run->folders[i]);
        allow_linked_repository(ruleset, access, run->folders[i]);
    }
    const char *const system[] = {"/tmp", "/var/tmp", "/dev", "/proc", ch_getenv("TMPDIR"), ch_getenv("XDG_CACHE_HOME")};
    for (size_t i = 0; i < CH_N_ELEMENTS(system); i++)
        if (system[i] && *system[i])
            allow(ruleset, access, system[i]);
    const char *home = ch_home_dir();
    for (size_t i = 0; home && i < CH_N_ELEMENTS(caches); i++) {
        char *path = ch_build_path(home, caches[i], NULL);
        allow(ruleset, access, path);
        free(path);
    }
    return ruleset;
}
/* Between fork and exec: only async-signal-safe calls. no_new_privs is Landlock's
 * price for not being root, and it also keeps sudo from stepping outside the rules. */
bool ch_sandbox_enter(int sandbox) {
    return !prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) && !syscall(__NR_landlock_restrict_self, sandbox, 0);
}
