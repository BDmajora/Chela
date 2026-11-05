#define _GNU_SOURCE
#include "chela.h"
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static void io_error(ChError **error, const char *what) {
    ch_set_error(error, CH_ERR_IO, errno, "%s: %s", what, strerror(errno));
}
/* Walk directory descriptors instead of joining strings: no symlink traversal or ../ escapes. */
static int parent_fd(const char *root, const char *path, char **leaf, ChError **error) {
    if (!path || !*path || ch_path_is_absolute(path) || strlen(path) > 4096) {
        ch_fail_literal(error, "Use a relative project path");
        return -1;
    }
    char **parts = ch_strsplit(path, "/");
    unsigned n = ch_strv_length(parts);
    for (unsigned i = 0; i < n; i++)
        if (!*parts[i] || !strcmp(parts[i], ".") || !strcmp(parts[i], "..")) {
            ch_fail_literal(error, "Empty, . and .. path components are not allowed");
            ch_strfreev(parts);
            return -1;
        }
    int fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        io_error(error, "Open project");
        ch_strfreev(parts);
        return -1;
    }
    for (unsigned i = 0; i + 1 < n; i++) {
        int next = openat(fd, parts[i], O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(fd);
        fd = next;
        if (fd < 0) {
            io_error(error, "Open project directory (symlinks are excluded)");
            ch_strfreev(parts);
            return -1;
        }
    }
    *leaf = ch_strdup(parts[n - 1]);
    ch_strfreev(parts);
    return fd;
}
char *ch_read_file(const char *root, const char *path, ChError **error) {
    char *leaf = NULL;
    int dir = parent_fd(root, path, &leaf, error);
    if (dir < 0)
        return NULL;
    int fd = openat(dir, leaf, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    close(dir);
    free(leaf);
    if (fd < 0) {
        io_error(error, "Read file");
        return NULL;
    }
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > CHELA_LIMIT) {
        close(fd);
        ch_fail_literal(error, "Only regular text files up to 4 MiB can be read");
        return NULL;
    }
    ChBuf *s = ch_buf_new(NULL);
    char buf[8192];
    ssize_t n;
    bool failed = false;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        if (s->len + (size_t)n > CHELA_LIMIT) {
            ch_fail_literal(error, "File grew past 4 MiB");
            failed = true;
            break;
        }
        ch_buf_append_len(s, buf, n);
    }
    if (n < 0) {
        io_error(error, "Read file");
        failed = true;
    }
    close(fd);
    if (failed || memchr(s->str, 0, s->len) || !ch_utf8_validate(s->str, s->len, NULL)) {
        if (!error || !*error)
            ch_fail_literal(error, "File is not UTF-8 text");
        ch_buf_free(s, false);
        return NULL;
    }
    return ch_buf_free(s, true);
}
bool ch_write_file(const char *root, const char *path, const char *content, ChError **error) {
    if (strlen(content) > CHELA_LIMIT) {
        ch_fail_literal(error, "Write exceeds 4 MiB");
        return false;
    }
    char *leaf = NULL;
    int dir = parent_fd(root, path, &leaf, error);
    if (dir < 0)
        return false;
    struct stat st;
    mode_t mode = 0644;
    if (fstatat(dir, leaf, &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode)) {
            ch_fail_literal(error, "Cannot replace a symlink or special file");
            close(dir);
            free(leaf);
            return false;
        }
        mode = st.st_mode & 0777;
    } else if (errno != ENOENT) {
        io_error(error, "Inspect file");
        close(dir);
        free(leaf);
        return false;
    }
    char *uuid = ch_uuid_random(), *tmp = ch_strconcat(".chela-", uuid, NULL);
    free(uuid);
    int fd = openat(dir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    bool ok = fd >= 0;
    if (!ok)
        io_error(error, "Create temporary file");
    size_t off = 0, len = strlen(content);
    while (ok && off < len) {
        ssize_t n = write(fd, content + off, len - off);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            io_error(error, "Write file");
            ok = false;
        } else
            off += (size_t)n;
    }
    if (ok && (fchmod(fd, mode) || fsync(fd))) {
        io_error(error, "Flush file");
        ok = false;
    }
    if (fd >= 0)
        close(fd);
    if (ok && renameat(dir, tmp, dir, leaf)) {
        io_error(error, "Replace file");
        ok = false;
    }
    if (!ok)
        unlinkat(dir, tmp, 0);
    else
        (void)fsync(dir);
    close(dir);
    free(tmp);
    free(leaf);
    return ok;
}
/* Where a tool's path lands. Relative paths are inside the working folder; absolute ones
 * must be inside it or a folder added to the session. The walk from there refuses
 * symlinks and .. as usual, so a folder cannot be left once entered. */
typedef struct {
    const char *root;
    char *path;
} Place;
static const char *inside(const char *path, const char *root) {
    size_t n = root ? strlen(root) : 0;
    if (!n || strncmp(path, root, n) || (path[n] && path[n] != '/'))
        return NULL;
    return path + n;
}
static bool place(const ChRun *run, const char *path, Place *out, ChError **error) {
    const char *rest = NULL;
    out->root = NULL;
    if (ch_path_is_absolute(path)) {
        const char *best = inside(path, run->project);
        if (best) {
            rest = best;
            out->root = run->project;
        }
        /* With one folder inside another, the deepest one answers. */
        for (size_t i = 0; run->folders && run->folders[i]; i++)
            if ((best = inside(path, run->folders[i])) && (!rest || best > rest)) {
                rest = best;
                out->root = run->folders[i];
            }
        if (!out->root) {
            ch_fail(error, "%s is outside the folders of this session", path);
            return false;
        }
    } else {
        if (!run->project || !*run->project) {
            ch_fail_literal(error, "No folder is open in this session");
            return false;
        }
        out->root = run->project;
        rest = path;
    }
    while (*rest == '/' || (rest[0] == '.' && rest[1] == '/'))
        rest += *rest == '/' ? 1 : 2;
    size_t n = strlen(rest);
    while (n && rest[n - 1] == '/')
        n--;
    out->path = n ? ch_strndup(rest, n) : ch_strdup(".");
    return true;
}
/* Where a harness says it will work. Nothing walks the path for it, so links are followed
 * here. A file yet to be made has no real path: its deepest folder that exists decides. */
bool ch_path_in_session(const ChRun *run, const char *path) {
    Place where;
    if (!path || !*path || !place(run, path, &where, NULL))
        return false;
    char **parts = ch_strsplit(where.path, "/");
    bool climbs = false;
    for (size_t i = 0; parts[i]; i++)
        climbs |= !strcmp(parts[i], "..");
    ch_strfreev(parts);
    char *full = ch_build_path(where.root, where.path, NULL), *root = realpath(where.root, NULL), *real = NULL;
    free(where.path);
    for (char *end; !climbs && !(real = realpath(full, NULL)) && (end = strrchr(full, '/')) && end != full;)
        *end = 0;
    bool found = real && root && inside(real, root);
    free(full);
    free(root);
    free(real);
    return found;
}
static char *command_folder(const ChRun *run, const char *path, ChError **error) {
    Place where;
    if (!place(run, path, &where, error))
        return NULL;
    char *folder = NULL;
    if (!strcmp(where.path, "."))
        folder = ch_strdup(where.root);
    else {
        char *leaf = NULL;
        int parent = parent_fd(where.root, where.path, &leaf, error);
        if (parent >= 0) {
            int fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd < 0)
                io_error(error, "Open command folder (symlinks are excluded)");
            else {
                close(fd);
                folder = ch_build_path(where.root, where.path, NULL);
            }
            close(parent);
            free(leaf);
        }
    }
    free(where.path);
    return folder;
}
static char *list_files(const char *root, const char *path, ChError **error) {
    int fd;
    if (!*path || !strcmp(path, "."))
        fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    else {
        char *leaf = NULL;
        int parent = parent_fd(root, path, &leaf, error);
        if (parent < 0)
            return NULL;
        fd = openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        close(parent);
        free(leaf);
    }
    if (fd < 0) {
        io_error(error, "List directory");
        return NULL;
    }
    DIR *dir = fdopendir(fd);
    if (!dir) {
        close(fd);
        io_error(error, "List directory");
        return NULL;
    }
    ChBuf *s = ch_buf_new(NULL);
    struct dirent *e;
    int count = 0;
    while ((e = readdir(dir)) && count < 1000) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        char *name = ch_utf8_make_valid(e->d_name, -1);
        ch_buf_printf(s, "%s%s\n", name,
                               e->d_type == DT_DIR   ? "/"
                               : e->d_type == DT_LNK ? " [symlink]"
                                                     : "");
        free(name);
        count++;
    }
    if (count == 1000)
        ch_buf_append(s, "[Listing limited to 1000 entries]\n");
    closedir(dir);
    return ch_buf_free(s, true);
}
static char *shell(const char *cwd, const char *command, int timeout, const ChCancel *cancel, int sandbox, ChError **error) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC)) {
        io_error(error, "Create command pipe");
        return NULL;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        int nullfd = open("/dev/null", O_RDONLY);
        if (nullfd >= 0) {
            dup2(nullfd, STDIN_FILENO);
            close(nullfd);
        }
        if (chdir(cwd))
            _exit(126);
        if (sandbox >= 0 && !ch_sandbox_enter(sandbox)) {
            static const char refused[] = "Could not confine this command to the session's folders\n";
            ssize_t written = write(STDOUT_FILENO, refused, sizeof(refused) - 1);
            (void)written;
            _exit(125);
        }
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    close(pipefd[1]);
    if (pid < 0) {
        close(pipefd[0]);
        io_error(error, "Start command");
        return NULL;
    }
    setpgid(pid, pid);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    ChBuf *out = ch_buf_new(NULL);
    int64_t end = ch_monotonic_us() + (int64_t)timeout * CH_USEC_PER_SEC;
    bool eof = false, exited = false, failed = false;
    int status = 0;
    while (!eof || !exited) {
        if (ch_cancel_is_cancelled(cancel) || ch_monotonic_us() > end) {
            ch_set_error_literal(error, ch_cancel_is_cancelled(cancel) ? CH_ERR_CANCELLED : CH_ERR_TIMED_OUT, 0,
                                ch_cancel_is_cancelled(cancel) ? "Stopped" : "Command timed out");
            failed = true;
            break;
        }
        struct pollfd p = {pipefd[0], POLLIN | POLLHUP, 0};
        poll(&p, 1, 50);
        if (p.revents) {
            char buf[8192];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                if (out->len + (size_t)n > CHELA_LIMIT) {
                    ch_fail_literal(error, "Command output exceeded 4 MiB");
                    failed = true;
                    break;
                }
                ch_buf_append_len(out, buf, n);
                if (ch_cancel_is_cancelled(cancel) || ch_monotonic_us() > end)
                    break;
            }
            if (n == 0)
                eof = true;
        }
        if (failed)
            break;
        if (!exited) {
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid)
                exited = true;
        }
    }
    if (failed) {
        kill(-pid, SIGKILL);
        if (!exited)
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
    } else
        kill(-pid, SIGKILL); /* Do not leave detached pipe-owning descendants behind. */
    close(pipefd[0]);
    if (failed) {
        ch_buf_free(out, false);
        return NULL;
    }
    char *valid = ch_utf8_make_valid(out->str, out->len);
    ch_buf_assign(out, valid);
    free(valid);
    if (WIFEXITED(status))
        ch_buf_printf(out, "\n[exit %d]", WEXITSTATUS(status));
    else
        ch_buf_printf(out, "\n[signal %d]", WTERMSIG(status));
    return ch_buf_free(out, true);
}
char *ch_command(const char *cwd, const char *command, int timeout, const ChCancel *cancel, ChError **error) {
    return shell(cwd, command, timeout, cancel, -1, error);
}
json_object *ch_tool_schemas(ChMode mode) {
    static const char *schema =
        "["
        "{\"type\":\"function\",\"function\":{\"name\":\"list_files\",\"description\":\"List a directory. "
        "Paths are relative to the project, or absolute inside a folder included in this session. Symlinks are "
        "not followed.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a UTF-8 file. "
        "Paths are relative to the project, or absolute inside a folder included in this session.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Create or replace a "
        "file with complete UTF-8 content. Paths are relative to the project, or absolute inside a folder "
        "included in this session. Parent directory must exist. Requires approval.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"run_command\",\"description\":\"Run a shell command "
        "with a 30 second timeout, in the project unless cwd names another folder included in this session. "
        "Requires approval. Commands can write only in this session's folders, temporary folders and package "
        "caches, and cannot use sudo.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"},"
        "\"cwd\":{\"type\":\"string\"}},\"required\":[\"command\"]}}}]";
    json_object *a = json_tokener_parse(schema);
    if (mode == CH_PLAN)
        json_object_array_del_idx(a, 2, 2);
    return a;
}
char *ch_tool_execute(ChRun *run, const char *name, const char *arguments, ChError **error) {
    if (ch_cancel_check(run->cancel, error))
        return NULL;
    json_object *a = ch_json_parse(arguments, error);
    if (!a)
        return NULL;
    if (!json_object_is_type(a, json_type_object)) {
        json_object_put(a);
        ch_fail_literal(error, "Tool arguments must be an object");
        return NULL;
    }
    bool read = !strcmp(name, "read_file"), list = !strcmp(name, "list_files"),
             write = !strcmp(name, "write_file"), command = !strcmp(name, "run_command");
    char *out = NULL, *folder = NULL;
    int sandbox = -1;
    ChError *unconfined = NULL;
    Place where = {0};
    const char *path = ch_json_str(a, "path"), *cwd = ch_json_str(a, "cwd");
    if (!read && !list && !write && !command)
        ch_fail(error, "Unknown tool: %s", name);
    else if ((read || list || write) && !json_object_is_type(ch_json_get(a, "path"), json_type_string))
        ch_fail_literal(error, "Tool requires a string path");
    else if (write && !json_object_is_type(ch_json_get(a, "content"), json_type_string))
        ch_fail_literal(error, "write_file requires string content");
    else if (command && !*ch_json_str(a, "command"))
        ch_fail_literal(error, "run_command requires a nonempty command");
    else if ((write || command) && ch_run_read_only(run))
        ch_fail_literal(error, "Chat and Plan modes only permit reading project files");
    else if ((read || list || write) && !place(run, path, &where, error)) {
    } else if (command && !(folder = command_folder(run, *cwd ? cwd : ".", error))) {
    } else if (command && (sandbox = ch_sandbox_open(run, &unconfined)) < 0 && run->mode == CH_BYPASS) {
        /* Nobody is asked in this mode, so it must not lose its limits unnoticed. */
        ch_fail(error, "Commands cannot be confined to this session's folders here: %s. Use Manual mode to approve "
                       "each one.", unconfined->message);
    } else {
        const char *why = NULL;
        bool allowed = write ? ch_may_write(run, where.path, &why)
                     : command ? ch_may_run(run, ch_json_str(a, "command"), sandbox >= 0, &why) : true;
        if (!allowed) {
            char *detail;
            if (write) {
                ChError *local = NULL;
                char *old = ch_read_file(where.root, where.path, &local);
                ch_clear_error(&local);
                detail =
                    ch_strdup_printf("File: %s\n\n--- Current content ---\n%s\n\n+++ Proposed content +++\n%s",
                                    path, old ? old : "[new or unreadable file]", ch_json_str(a, "content"));
                free(old);
            } else {
                char *place_note = *cwd ? ch_strdup_printf("In %s:\n\n", folder) : ch_strdup("");
                detail = ch_strdup_printf("%s%s%s%s%s", unconfined ? "This command will not be confined to the session's folders: " : "",
                                          unconfined ? unconfined->message : "", unconfined ? ".\n\n" : "", place_note,
                                          ch_json_str(a, "command"));
                free(place_note);
            }
            char *question = why ? ch_strdup_printf("Auto asks first because %s.\n\n%s", why, detail) : ch_strdup(detail);
            allowed = run->approve &&
                      run->approve(write ? "Allow file change?" : "Allow command?", question, run->data);
            free(question);
            free(detail);
        }
        if (!allowed)
            ch_fail_literal(error, "User declined this tool call");
        else if (ch_cancel_check(run->cancel, error)) {
        } else if (read)
            out = ch_read_file(where.root, where.path, error);
        else if (list)
            out = list_files(where.root, where.path, error);
        else if (write) {
            if (ch_write_file(where.root, where.path, ch_json_str(a, "content"), error))
                out = ch_strdup("File saved.");
        } else
            out = shell(folder, ch_json_str(a, "command"), 30, run->cancel, sandbox, error);
    }
    if (sandbox >= 0)
        close(sandbox);
    ch_clear_error(&unconfined);
    free(where.path);
    free(folder);
    json_object_put(a);
    return out;
}
