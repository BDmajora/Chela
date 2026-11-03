#define _GNU_SOURCE
#include "chela.h"
#include "inferssd_library.h"
#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

bool ch_inferssd_validate(const ChConfig *c, ChError **error) {
    bool valid = c->provider == 6 && *c->ssh_host && *c->ssh_user && c->ssh_host[0] != '-' && c->ssh_user[0] != '-' &&
                 c->ssh_port > 0 && c->ssh_port <= 65535 && c->remote_port > 0 && c->remote_port <= 65535 &&
                 c->local_port > 0 && c->local_port <= 65535 && *c->model_dir && *c->inference_binary;
    for (const char *p = c->ssh_host; *p; p++)
        valid &= isalnum((unsigned char)*p) || strchr(".:-_", *p) != NULL;
    for (const char *p = c->ssh_user; *p; p++)
        valid &= isalnum((unsigned char)*p) || strchr("._-", *p) != NULL;
    valid &= !strpbrk(c->model_dir, "\r\n") && !strpbrk(c->inference_binary, "\r\n");
    char *expected = ch_strdup_printf("http://127.0.0.1:%d/v1", c->local_port);
    valid &= !strcmp(c->endpoint, expected);
    free(expected);
    if (!valid)
        ch_fail_literal(error, "Enter a host/IP, SSH username, valid ports, model folder, and server executable. "
                               "The SSH connection uses a local 127.0.0.1 server URL.");
    return valid;
}

/* ssh asks Chela itself (SSH_ASKPASS) for passwords and host-key confirmations. Each
 * attempt has a marker file so a repeated prompt can tell the last password failed. */
typedef struct {
    ChVec args, env;
    char *marker, *socket;
} SshCommand;
static void ssh_command_clear(SshCommand *s) {
    ch_vec_clear(&s->args);
    ch_vec_clear(&s->env);
    if (s->marker)
        unlink(s->marker);
    free(s->marker);
    free(s->socket);
}
static bool ssh_command(const ChConfig *c, bool master, SshCommand *s, ChError **error) {
    memset(s, 0, sizeof(*s));
    ch_vec_init(&s->args, free);
    ch_vec_init(&s->env, free);
    if (!ch_inferssd_validate(c, error))
        return false;
    char *runtime = ch_user_runtime_dir(), *dir = ch_build_path(runtime, "chela-ssh", NULL);
    free(runtime);
    struct stat st;
    if (mkdir(dir, 0700) && (lstat(dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid() || (st.st_mode & 077))) {
        ch_fail_literal(error, "Cannot prepare a private SSH connection directory.");
        free(dir);
        return false;
    }
    char *identity = ch_strdup_printf("%s@%s:%d", c->ssh_user, c->ssh_host, c->ssh_port);
    char *digest = ch_sha256_hex(identity, strlen(identity));
    digest[16] = 0;
    s->socket = ch_build_path(dir, digest, NULL);
    char *control = ch_strconcat("ControlPath=", s->socket, NULL);
    free(digest);
    free(identity);
    if (strlen(control) > 110) {
        ch_fail_literal(error, "The runtime folder path is too long for an SSH socket.");
        free(control);
        free(dir);
        return false;
    }
    char *exe = ch_read_link("/proc/self/exe", error);
    if (!exe) {
        free(control);
        free(dir);
        return false;
    }
    char *attempt = ch_uuid_random();
    s->marker = ch_build_path(dir, attempt, NULL);
    free(dir);
    ch_vec_push(&s->env, ch_strconcat("SSH_ASKPASS=", exe, NULL));
    ch_vec_push(&s->env, ch_strdup("SSH_ASKPASS_REQUIRE=force"));
    ch_vec_push(&s->env, ch_strdup("CHELA_SSH_ASKPASS=1"));
    ch_vec_push(&s->env, ch_strconcat("CHELA_SSH_HOST=", c->ssh_host, NULL));
    ch_vec_push(&s->env, ch_strconcat("CHELA_SSH_USER=", c->ssh_user, NULL));
    ch_vec_push(&s->env, ch_strdup_printf("CHELA_SSH_PORT=%d", c->ssh_port));
    ch_vec_push(&s->env, ch_strdup(c->ssh_remember ? "CHELA_SSH_REMEMBER=1" : "CHELA_SSH_REMEMBER=0"));
    ch_vec_push(&s->env, ch_strconcat("CHELA_SSH_ATTEMPT=", attempt, NULL));
    ch_vec_push(&s->env, NULL);
    free(attempt);
    free(exe);
    const char *base[] = {"ssh", "-F", "/dev/null", "-T", "-o", "ConnectTimeout=10", "-o", "ConnectionAttempts=1",
                          "-o", "ServerAliveInterval=15", "-o", "ServerAliveCountMax=2", "-o",
                          "StrictHostKeyChecking=ask", "-o", "ControlPersist=no", "-o", "ExitOnForwardFailure=yes",
                          "-o", "NumberOfPasswordPrompts=2"};
    for (size_t i = 0; i < CH_N_ELEMENTS(base); i++)
        ch_vec_push(&s->args, ch_strdup(base[i]));
    ch_vec_push(&s->args, ch_strdup("-o"));
    ch_vec_push(&s->args, control);
    ch_vec_push(&s->args, ch_strdup("-o"));
    ch_vec_push(&s->args, ch_strdup(master ? "ControlMaster=yes" : "ControlMaster=no"));
    ch_vec_push(&s->args, ch_strdup("-p"));
    ch_vec_push(&s->args, ch_strdup_printf("%d", c->ssh_port));
    ch_vec_push(&s->args, ch_strdup("-l"));
    ch_vec_push(&s->args, ch_strdup(c->ssh_user));
    return true;
}
/* ssh runs in its own session, so it never receives the terminal's Ctrl+C or hangup.
 * It ends with the thread that starts it instead: call this only from a thread that
 * waits for ssh to exit. */
static ChProc *ssh_start(SshCommand *s, int flags, ChError **error) {
    ch_vec_push(&s->args, NULL);
    ChSpawn spawn = {.argv = (const char *const *)s->args.data, .env = (const char *const *)s->env.data,
                     .flags = flags | CH_PROC_SEARCH_PATH | CH_PROC_NEW_SESSION | CH_PROC_DIE_WITH_PARENT};
    return ch_proc_spawn(&spawn, error);
}
/* Runs one helper command over SSH with the embedded library script on stdin. */
static char *ssh_run(const ChConfig *c, bool batch, const char *command, const char *input, const ChCancel *cancel,
                     const char *failure, int detail_limit, ChError **error) {
    SshCommand s;
    if (!ssh_command(c, false, &s, error)) {
        ssh_command_clear(&s);
        return NULL;
    }
    if (batch) {
        ch_vec_push(&s.args, ch_strdup("-o"));
        ch_vec_push(&s.args, ch_strdup("BatchMode=yes"));
    }
    ch_vec_push(&s.args, ch_strdup("--"));
    ch_vec_push(&s.args, ch_strdup(c->ssh_host));
    ch_vec_push(&s.args, ch_strdup(command));
    ChProc *p = ssh_start(&s, (input ? CH_PROC_STDIN : 0) | CH_PROC_STDOUT | CH_PROC_STDERR, error);
    char *out = NULL, *err = NULL;
    bool ok = p && ch_proc_communicate(p, input, cancel, &out, &err, error);
    if (ok && !ch_proc_succeeded(p)) {
        ch_fail(error, "%s%.*s", failure, detail_limit, err && *err ? err : "");
        ok = false;
    }
    free(err);
    ch_proc_free(p);
    ssh_command_clear(&s);
    if (!ok)
        ch_clear_pointer(&out, free);
    return out;
}
json_object *ch_inferssd_install(const ChConfig *c, const ChCancel *cancel, ChError **error) {
    /* stdin is the embedded helper, never a password or a shell program. */
    const char *bootstrap =
        "import os,pathlib,runpy,sys,tempfile; "
        "d=pathlib.Path(os.environ.get('XDG_DATA_HOME',str(pathlib.Path.home()/'.local/share')))/'chela/"
        "inferssd'; "
        "d.mkdir(parents=True,exist_ok=True,mode=448); "
        "fd,t=tempfile.mkstemp(dir=d,suffix='.py'); "
        "f=os.fdopen(fd,'w'); f.write(sys.stdin.read()); f.close(); "
        "p=d/'inferssd-library.py'; os.replace(t,p); runpy.run_path(str(p),run_name='__main__')";
    char *quoted = ch_shell_quote(bootstrap), *server = ch_shell_quote(c->inference_binary),
         *folder = ch_shell_quote(c->model_dir), *device = ch_shell_quote(c->gpu_device);
    char *command = ch_strdup_printf("python3 -c %s --install --server %s --model-dir %s --port %d "
                                     "--memory-mode %d --context-size %d --gpu-headroom %d --device %s",
                                     quoted, server, folder, c->remote_port, c->memory_mode, c->context_size,
                                     c->gpu_headroom, device);
    free(quoted);
    free(server);
    free(folder);
    free(device);
    ChError *local = NULL;
    char *out = ssh_run(c, false, command, inferssd_library, cancel, "Could not set up the inference computer: ", 4000,
                        &local);
    free(command);
    if (!out && local && !ch_error_is(local, CH_ERR_CANCELLED) && !strcmp(local->message, "Could not set up the inference computer: ")) {
        ch_clear_error(&local);
        ch_fail_literal(&local, "Could not set up the inference computer: Check the computer is on and your SSH login is correct.");
    }
    ch_propagate_error(error, local);
    json_object *models = out ? ch_json_parse(out, error) : NULL;
    if (models && !json_object_is_type(ch_json_get(models, "data"), json_type_array)) {
        ch_fail_literal(error, "Setup did not return a model library.");
        json_object_put(models);
        models = NULL;
    }
    free(out);
    return models;
}
json_object *ch_inferssd_device_status(const ChConfig *c, const ChCancel *cancel, ChError **error) {
    /* Run the current embedded helper without installing or changing any file.
     * BatchMode reuses a connection/key and never opens an authentication UI. */
    char *script = ch_shell_quote("import sys; exec(compile(sys.stdin.read(), '<chela-device-status>', 'exec'))");
    char *model = ch_shell_quote(c->model);
    char *command = ch_strdup_printf("python3 -c %s --device-status --model %s", script, model);
    free(script);
    free(model);
    char *out = ssh_run(c, true, command, inferssd_library, cancel, "Could not check the inference device over SSH. ",
                        2000, error);
    free(command);
    json_object *status = out ? ch_json_parse(out, error) : NULL;
    if (status && (!json_object_is_type(status, json_type_object) || !*ch_json_str(status, "summary"))) {
        ch_fail_literal(error, "The inference computer did not return device status.");
        json_object_put(status);
        status = NULL;
    }
    free(out);
    return status;
}
char *ch_inferssd_diagnostics(const ChConfig *c, const ChCancel *cancel, ChError **error) {
    /* Reuse the SSH connection or keys. Diagnostics never open a password prompt. */
    const char *script =
        "import subprocess; "
        "a=subprocess.run(['systemctl','--user','show','chela-inferssd.service',"
        "'--property=ActiveState,SubState,Result,ExecMainStatus,NRestarts'],capture_output=True,text=True,timeout=5); "
        "b=subprocess.run(['journalctl','--user','-u','chela-inferssd.service','-n','60','--no-pager','-o','short-iso'],"
        "capture_output=True,text=True,timeout=5); "
        "print('Service state\\n'+(a.stdout+a.stderr)[-6000:]+'\\nRecent server log\\n'+(b.stdout+b.stderr)[-24000:])";
    char *quoted = ch_shell_quote(script), *command = ch_strdup_printf("python3 -c %s", quoted);
    free(quoted);
    char *out = ssh_run(c, true, command, NULL, cancel,
                        "Could not read the server log over SSH. Reconnect SSH, then refresh diagnostics. ", 2000, error);
    free(command);
    return out;
}

struct ChTunnel {
    ChProc *proc;
    SshCommand command;
    ChMutex lock;
    ChCond done_cond;
    ChBuf detail;
    ChError *start_error;
    bool started, exited, done;
    void (*on_exit)(void *data);
    void *on_exit_data;
};
static void tunnel_reaper(void *data) {
    ChTunnel *t = data;
    /* This thread lives exactly as long as ssh, so a crashed or killed Chela takes
     * the tunnel with it instead of leaving it holding the local port. */
    ChError *error = NULL;
    ChProc *proc = ssh_start(&t->command, CH_PROC_STDERR, &error);
    ch_mutex_lock(&t->lock);
    t->proc = proc;
    t->start_error = error;
    t->started = true;
    ch_cond_signal(&t->done_cond);
    ch_mutex_unlock(&t->lock);
    if (!proc)
        return;
    int fd = ch_proc_stderr(proc);
    char chunk[4096];
    for (;;) {
        ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n > 0) {
            ch_mutex_lock(&t->lock);
            if (t->detail.len < 64 * 1024)
                ch_buf_append_len(&t->detail, chunk, (size_t)n);
            ch_mutex_unlock(&t->lock);
        } else if (n == 0 || errno != EINTR)
            break;
    }
    ch_proc_wait(proc, NULL);
    ch_mutex_lock(&t->lock);
    t->exited = true;
    void (*callback)(void *) = t->on_exit;
    void *callback_data = t->on_exit_data;
    ch_mutex_unlock(&t->lock);
    if (callback)
        callback(callback_data);
    ch_mutex_lock(&t->lock);
    t->done = true;
    ch_cond_signal(&t->done_cond);
    ch_mutex_unlock(&t->lock);
}
static bool local_port_free(int port, int *err) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    bool ok = sock >= 0 && !setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) &&
              !bind(sock, (struct sockaddr *)&address, sizeof(address));
    *err = errno;
    if (sock >= 0)
        close(sock);
    return ok;
}
/* ssh cannot replace a control socket left by a killed master and then runs without
 * one, so the BatchMode helpers (device status, diagnostics) could not reuse the login. */
static void remove_stale_socket(const char *path) {
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un address = {.sun_family = AF_UNIX};
    ch_strlcpy(address.sun_path, path, sizeof(address.sun_path));
    if (sock >= 0 && connect(sock, (struct sockaddr *)&address, sizeof(address)) && errno == ECONNREFUSED)
        unlink(path);
    if (sock >= 0)
        close(sock);
}
static pid_t parent_of(long pid) {
    char *path = ch_strdup_printf("/proc/%ld/stat", pid), *line = NULL;
    pid_t parent = 0;
    if (ch_file_get_contents(path, &line, NULL, NULL)) {
        const char *name_end = strrchr(line, ')');
        int ppid;
        if (name_end && sscanf(name_end + 1, " %*c %d", &ppid) == 1)
            parent = ppid;
    }
    free(line);
    free(path);
    return parent;
}
static bool chela_process(pid_t pid) {
    char *path = ch_strdup_printf("/proc/%d/exe", (int)pid), *exe = ch_read_link(path, NULL);
    char *base = exe ? ch_path_basename(exe) : NULL;
    bool chela = base && (!strcmp(base, "chela") || !strcmp(base, "chela (deleted)"));
    free(base);
    free(exe);
    free(path);
    return chela;
}
/* A tunnel left behind by an older Chela that crashed or was killed keeps the local
 * port. It names a socket in Chela's private control folder and forwards this port,
 * so end it, unless a running Chela still owns it. Returns how many were ended. */
static int end_orphaned_tunnels(const SshCommand *s, int local_port, bool *owned) {
    const char *slash = strrchr(s->socket, '/');
    char *control = ch_strdup_printf("ControlPath=%.*s/", (int)(slash - s->socket), s->socket);
    char *forward = ch_strdup_printf("127.0.0.1:%d:", local_port);
    int ended = 0;
    DIR *proc = opendir("/proc");
    for (struct dirent *entry; proc && (entry = readdir(proc));) {
        char *end;
        long pid = strtol(entry->d_name, &end, 10);
        if (*end || pid <= 0 || pid == getpid())
            continue;
        char *dir = ch_strdup_printf("/proc/%ld", pid), *path = ch_build_path(dir, "cmdline", NULL), *args = NULL;
        size_t length = 0;
        struct stat st;
        bool socket_arg = false, master = false, detached = false, forwards = false, after_l = false;
        if (!stat(dir, &st) && st.st_uid == getuid() && ch_file_get_contents(path, &args, &length, NULL))
            for (size_t i = 0; i < length; i += strlen(args + i) + 1) {
                const char *arg = args + i;
                socket_arg |= ch_str_has_prefix(arg, control);
                master |= !strcmp(arg, "ControlMaster=yes");
                detached |= !strcmp(arg, "-N");
                forwards |= after_l && ch_str_has_prefix(arg, forward);
                after_l = !strcmp(arg, "-L");
            }
        if (socket_arg && master && detached && forwards) {
            if (chela_process(parent_of(pid)))
                *owned = true;
            else if (!kill((pid_t)pid, SIGTERM))
                ended++;
        }
        free(args);
        free(path);
        free(dir);
    }
    if (proc)
        closedir(proc);
    free(forward);
    free(control);
    return ended;
}
static void tunnel_free(ChTunnel *t) {
    ch_proc_free(t->proc);
    remove_stale_socket(t->command.socket); /* ch_proc_kill gives ssh no chance to remove it */
    ssh_command_clear(&t->command);
    ch_buf_clear(&t->detail);
    ch_clear_error(&t->start_error);
    ch_mutex_clear(&t->lock);
    ch_cond_clear(&t->done_cond);
    free(t);
}
ChTunnel *ch_inferssd_tunnel(const ChConfig *c, ChError **error) {
    ChTunnel *t = ch_new0(ChTunnel, 1);
    if (!ssh_command(c, true, &t->command, error)) {
        ssh_command_clear(&t->command);
        free(t);
        return NULL;
    }
    /* Never silently connect to another application already using this port. */
    int saved = 0;
    bool owned = false, available = local_port_free(c->local_port, &saved);
    if (!available && saved == EADDRINUSE && end_orphaned_tunnels(&t->command, c->local_port, &owned))
        for (int64_t deadline = ch_monotonic_us() + 3 * CH_USEC_PER_SEC;
             !(available = local_port_free(c->local_port, &saved)) && ch_monotonic_us() < deadline;)
            ch_usleep(50000);
    if (!available) {
        if (owned)
            ch_fail(error, "Local port %d is in use by another Chela window. Close that window, or change Local port "
                           "in Settings → Connections → Advanced.", c->local_port);
        else if (saved == EADDRINUSE)
            ch_fail(error, "Local port %d is in use by another program. Close it, or change Local port in Settings → "
                           "Connections → Advanced.", c->local_port);
        else
            ch_fail(error, "Local port %d is unavailable (%s). Change Local port in Settings → Connections → Advanced.",
                    c->local_port, strerror(saved));
        ssh_command_clear(&t->command);
        free(t);
        return NULL;
    }
    remove_stale_socket(t->command.socket);
    ch_vec_push(&t->command.args, ch_strdup("-N"));
    ch_vec_push(&t->command.args, ch_strdup("-L"));
    ch_vec_push(&t->command.args, ch_strdup_printf("127.0.0.1:%d:127.0.0.1:%d", c->local_port, c->remote_port));
    ch_vec_push(&t->command.args, ch_strdup("--"));
    ch_vec_push(&t->command.args, ch_strdup(c->ssh_host));
    ch_mutex_init(&t->lock);
    ch_cond_init(&t->done_cond);
    ch_buf_init(&t->detail);
    if (!ch_thread_run(tunnel_reaper, t)) {
        ch_fail_literal(error, "Cannot watch the SSH connection.");
        tunnel_free(t);
        return NULL;
    }
    ch_mutex_lock(&t->lock);
    while (!t->started)
        ch_cond_wait(&t->done_cond, &t->lock);
    ch_mutex_unlock(&t->lock);
    if (!t->proc) {
        ch_propagate_error(error, ch_steal(&t->start_error));
        tunnel_free(t);
        return NULL;
    }
    return t;
}
bool ch_tunnel_exited(ChTunnel *t) {
    ch_mutex_lock(&t->lock);
    bool exited = t->exited;
    ch_mutex_unlock(&t->lock);
    return exited;
}
char *ch_tunnel_detail(ChTunnel *t) {
    ch_mutex_lock(&t->lock);
    char *detail = ch_utf8_make_valid(t->detail.str, (ptrdiff_t)t->detail.len);
    ch_mutex_unlock(&t->lock);
    return ch_strstrip(detail);
}
void ch_tunnel_on_exit(ChTunnel *t, void (*callback)(void *data), void *data) {
    ch_mutex_lock(&t->lock);
    t->on_exit = callback;
    t->on_exit_data = data;
    bool exited = t->exited;
    ch_mutex_unlock(&t->lock);
    if (exited && callback)
        callback(data);
}
void ch_tunnel_stop(ChTunnel *t) {
    if (!t)
        return;
    ch_mutex_lock(&t->lock);
    t->on_exit = NULL;
    ch_mutex_unlock(&t->lock);
    ch_proc_kill(t->proc);
    ch_mutex_lock(&t->lock);
    while (!t->done)
        ch_cond_wait(&t->done_cond, &t->lock);
    ch_mutex_unlock(&t->lock);
    tunnel_free(t);
}
static bool port_open(int port) {
    int sock = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (sock < 0)
        return false;
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    bool ok = connect(sock, (struct sockaddr *)&address, sizeof(address)) == 0;
    if (!ok && errno == EINPROGRESS) {
        struct pollfd p = {sock, POLLOUT, 0};
        int err = 0;
        socklen_t len = sizeof(err);
        ok = poll(&p, 1, 1000) == 1 && !getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &len) && !err;
    }
    close(sock);
    return ok;
}
bool ch_inferssd_wait(const ChConfig *c, ChTunnel *tunnel, const ChCancel *cancel, ChError **error) {
    if (!c->ssh_enabled)
        return true;
    int64_t deadline = ch_monotonic_us() + 90 * CH_USEC_PER_SEC;
    bool ok = false, failed = false;
    do {
        if (ch_cancel_is_cancelled(cancel))
            break;
        if (tunnel && ch_tunnel_exited(tunnel)) {
            char *detail = ch_tunnel_detail(tunnel);
            ch_fail(error, "SSH connection ended. Check that the computer is on and your login is correct. %.2000s",
                    detail);
            free(detail);
            failed = true;
            break;
        }
        if (port_open(c->local_port)) {
            ok = true;
            break;
        }
        ch_usleep(100000);
    } while (ch_monotonic_us() < deadline);
    if (!ok && !failed && !ch_cancel_check(cancel, error))
        ch_fail_literal(error, "Could not connect to the inference computer. Turn it on, check your SSH login, then "
                               "reopen the model picker to retry or use Model library setup.");
    return ok;
}

/* SSH_ASKPASS helper state, shared by every UI backend's prompt window. */
bool ch_askpass_prepare(int argc, char **argv, ChAskpass *ask) {
    memset(ask, 0, sizeof(*ask));
    if (argc < 2)
        return false;
    ask->prompt = argv[1];
    ask->confirm = !ch_strcmp0(ch_getenv("SSH_ASKPASS_PROMPT"), "confirm") || strstr(argv[1], "yes/no");
    ch_config_defaults(&ask->config);
    const char *host = ch_getenv("CHELA_SSH_HOST"), *user = ch_getenv("CHELA_SSH_USER"),
               *port = ch_getenv("CHELA_SSH_PORT");
    ch_strlcpy(ask->config.ssh_host, host ? host : "", sizeof(ask->config.ssh_host));
    ch_strlcpy(ask->config.ssh_user, user ? user : "", sizeof(ask->config.ssh_user));
    ask->config.ssh_port = (int)strtol(port ? port : "22", NULL, 10);
    char *lower = ch_ascii_strdown(argv[1], -1);
    ask->attempt = ch_getenv("CHELA_SSH_ATTEMPT");
    ask->can_remember = !ask->confirm && strstr(lower, "password:") && !strstr(lower, "new password") &&
                        !ch_strcmp0(ch_getenv("CHELA_SSH_REMEMBER"), "1") && *ask->config.ssh_host &&
                        *ask->config.ssh_user && ask->config.ssh_port > 0 && ask->config.ssh_port <= 65535 &&
                        ch_uuid_valid(ask->attempt);
    free(lower);
    if (ask->can_remember) {
        char *runtime = ch_user_runtime_dir();
        ask->marker = ch_build_path(runtime, "chela-ssh", ask->attempt, NULL);
        free(runtime);
        ask->repeated = ch_file_exists(ask->marker);
    }
    return true;
}
void ch_askpass_clear(ChAskpass *ask) {
    free(ask->marker);
    ask->marker = NULL;
}
