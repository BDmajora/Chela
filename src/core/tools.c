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
char *ch_command(const char *cwd, const char *command, int timeout, const ChCancel *cancel, ChError **error) {
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
json_object *ch_tool_schemas(ChMode mode) {
    static const char *schema =
        "["
        "{\"type\":\"function\",\"function\":{\"name\":\"list_files\",\"description\":\"List a project "
        "directory. Paths are relative, no "
        "symlinks.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"description\":\"Read a UTF-8 file "
        "within the "
        "project.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"}},"
        "\"required\":[\"path\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"description\":\"Create or replace a "
        "project file with complete UTF-8 content. Parent directory must exist. Requires "
        "approval.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"path\":{\"type\":\"string\"},"
        "\"content\":{\"type\":\"string\"}},\"required\":[\"path\",\"content\"]}}},"
        "{\"type\":\"function\",\"function\":{\"name\":\"run_command\",\"description\":\"Run a shell command "
        "in the project with a 30 second timeout. Requires approval; shell is not "
        "sandboxed.\",\"parameters\":{\"type\":\"object\",\"properties\":{\"command\":{\"type\":\"string\"}},"
        "\"required\":[\"command\"]}}}]";
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
    char *out = NULL;
    const char *path = ch_json_str(a, "path");
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
    else {
        bool allowed = true;
        if ((write || command) && run->mode != CH_AUTO && !(write && run->mode == CH_ACCEPT_EDITS)) {
            char *detail;
            if (write) {
                ChError *local = NULL;
                char *old = ch_read_file(run->project, path, &local);
                ch_clear_error(&local);
                detail =
                    ch_strdup_printf("File: %s\n\n--- Current content ---\n%s\n\n+++ Proposed content +++\n%s",
                                    path, old ? old : "[new or unreadable file]", ch_json_str(a, "content"));
                free(old);
            } else
                detail = ch_strdup(ch_json_str(a, "command"));
            allowed = run->approve &&
                      run->approve(write ? "Allow file change?" : "Allow command?", detail, run->data);
            free(detail);
        }
        if (!allowed)
            ch_fail_literal(error, "User declined this tool call");
        else if (ch_cancel_check(run->cancel, error)) {
        } else if (read)
            out = ch_read_file(run->project, path, error);
        else if (list)
            out = list_files(run->project, path, error);
        else if (write) {
            if (ch_write_file(run->project, path, ch_json_str(a, "content"), error))
                out = ch_strdup("File saved.");
        } else
            out = ch_command(run->project, ch_json_str(a, "command"), 30, run->cancel, error);
    }
    json_object_put(a);
    return out;
}
