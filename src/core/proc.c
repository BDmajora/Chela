#define _GNU_SOURCE
#include "base.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

struct ChProc {
    pid_t pid;
    int in, out, err;
    bool group, reaped;
    int status;
    ChMutex lock;
};
pid_t ch_proc_pid(const ChProc *p) {
    return p->pid;
}
int ch_proc_stdin(const ChProc *p) {
    return p->in;
}
int ch_proc_stdout(const ChProc *p) {
    return p->out;
}
int ch_proc_stderr(const ChProc *p) {
    return p->err;
}
static char **merged_environment(const char *const *overrides) {
    ChVec env;
    ch_vec_init(&env, NULL);
    for (char **e = environ; e && *e; e++) {
        bool replaced = false;
        for (const char *const *o = overrides; o && *o && !replaced; o++) {
            const char *eq = strchr(*o, '=');
            size_t n = eq ? (size_t)(eq - *o) : strlen(*o);
            replaced = !strncmp(*e, *o, n) && (*e)[n] == '=';
        }
        if (!replaced)
            ch_vec_push(&env, *e);
    }
    for (const char *const *o = overrides; o && *o; o++)
        ch_vec_push(&env, (void *)*o);
    ch_vec_push(&env, NULL);
    return (char **)env.data;
}
static void close_pair(int fds[2]) {
    for (int i = 0; i < 2; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}
ChProc *ch_proc_spawn(const ChSpawn *spawn, ChError **error) {
    int in[2] = {-1, -1}, out[2] = {-1, -1}, err[2] = {-1, -1}, status[2] = {-1, -1};
    bool ok = pipe2(status, O_CLOEXEC) == 0;
    if (ok && (spawn->flags & CH_PROC_STDIN))
        ok = pipe2(in, O_CLOEXEC) == 0;
    if (ok && (spawn->flags & CH_PROC_STDOUT))
        ok = pipe2(out, O_CLOEXEC) == 0;
    if (ok && (spawn->flags & CH_PROC_STDERR))
        ok = pipe2(err, O_CLOEXEC) == 0;
    if (!ok) {
        ch_set_error(error, CH_ERR_IO, errno, "Cannot create process pipes: %s", strerror(errno));
        close_pair(in), close_pair(out), close_pair(err), close_pair(status);
        return NULL;
    }
    char **env = spawn->env ? merged_environment(spawn->env) : environ;
    pid_t parent = getpid(), pid = fork();
    if (pid == 0) {
#ifdef __linux__
        if (spawn->flags & CH_PROC_DIE_WITH_PARENT) {
            prctl(PR_SET_PDEATHSIG, SIGTERM);
            if (getppid() != parent) /* the parent already exited */
                _exit(127);
        }
#else
        (void)parent;
#endif
        if (spawn->flags & CH_PROC_NEW_SESSION)
            setsid();
        else if (spawn->flags & CH_PROC_NEW_GROUP)
            setpgid(0, 0);
        signal(SIGPIPE, SIG_DFL);
        int null = open("/dev/null", O_RDWR | O_CLOEXEC);
        dup2(in[0] >= 0 ? in[0] : null, STDIN_FILENO);
        if (out[1] >= 0)
            dup2(out[1], STDOUT_FILENO);
        if (err[1] >= 0)
            dup2(err[1], STDERR_FILENO);
        else if (spawn->flags & CH_PROC_STDERR_MERGE)
            dup2(STDOUT_FILENO, STDERR_FILENO);
        else if (spawn->flags & CH_PROC_STDERR_SILENCE)
            dup2(null, STDERR_FILENO);
        if (spawn->cwd && chdir(spawn->cwd)) {
            int e = errno;
            (void)!write(status[1], &e, sizeof(e));
            _exit(127);
        }
        if (spawn->flags & CH_PROC_SEARCH_PATH)
            execvpe(spawn->argv[0], (char *const *)spawn->argv, env);
        else
            execve(spawn->argv[0], (char *const *)spawn->argv, env);
        int e = errno;
        (void)!write(status[1], &e, sizeof(e));
        _exit(127);
    }
    if (env != environ)
        free(env);
    close(status[1]);
    status[1] = -1;
    if (in[0] >= 0)
        close(in[0]);
    if (out[1] >= 0)
        close(out[1]);
    if (err[1] >= 0)
        close(err[1]);
    if (pid < 0) {
        ch_set_error(error, CH_ERR_IO, errno, "Cannot start %s: %s", spawn->argv[0], strerror(errno));
        close_pair(status);
        if (in[1] >= 0)
            close(in[1]);
        if (out[0] >= 0)
            close(out[0]);
        if (err[0] >= 0)
            close(err[0]);
        return NULL;
    }
    if (spawn->flags & (CH_PROC_NEW_GROUP | CH_PROC_NEW_SESSION))
        setpgid(pid, pid);
    int child_errno = 0;
    ssize_t n;
    while ((n = read(status[0], &child_errno, sizeof(child_errno))) < 0 && errno == EINTR) {
    }
    close(status[0]);
    if (n == (ssize_t)sizeof(child_errno)) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
        ch_set_error(error, CH_ERR_IO, child_errno, "Failed to execute child process “%s” (%s)", spawn->argv[0],
                     strerror(child_errno));
        if (in[1] >= 0)
            close(in[1]);
        if (out[0] >= 0)
            close(out[0]);
        if (err[0] >= 0)
            close(err[0]);
        return NULL;
    }
    ChProc *p = ch_new0(ChProc, 1);
    p->pid = pid;
    p->in = in[1];
    p->out = out[0];
    p->err = err[0];
    p->group = (spawn->flags & (CH_PROC_NEW_GROUP | CH_PROC_NEW_SESSION)) != 0;
    ch_mutex_init(&p->lock);
    return p;
}
bool ch_proc_try_wait(ChProc *p, int *status) {
    ch_mutex_lock(&p->lock);
    if (!p->reaped) {
        int s;
        pid_t w = waitpid(p->pid, &s, WNOHANG);
        if (w == p->pid) {
            p->reaped = true;
            p->status = s;
        }
    }
    bool done = p->reaped;
    if (done && status)
        *status = p->status;
    ch_mutex_unlock(&p->lock);
    return done;
}
bool ch_proc_wait(ChProc *p, int *status) {
    ch_mutex_lock(&p->lock);
    while (!p->reaped) {
        int s;
        pid_t w = waitpid(p->pid, &s, 0);
        if (w == p->pid) {
            p->reaped = true;
            p->status = s;
        } else if (w < 0 && errno != EINTR)
            break;
    }
    bool done = p->reaped;
    if (done && status)
        *status = p->status;
    ch_mutex_unlock(&p->lock);
    return done;
}
bool ch_proc_succeeded(const ChProc *p) {
    return p->reaped && WIFEXITED(p->status) && WEXITSTATUS(p->status) == 0;
}
void ch_proc_kill(ChProc *p) {
    if (!p)
        return;
    ch_mutex_lock(&p->lock);
    if (!p->reaped) {
        if (p->group)
            kill(-p->pid, SIGKILL);
        kill(p->pid, SIGKILL);
    }
    ch_mutex_unlock(&p->lock);
}
void ch_proc_free(ChProc *p) {
    if (!p)
        return;
    /* Do not leave detached pipe-owning descendants behind. */
    if (p->group)
        kill(-p->pid, SIGKILL);
    if (!ch_proc_try_wait(p, NULL)) {
        ch_proc_kill(p);
        ch_proc_wait(p, NULL);
    }
    if (p->in >= 0)
        close(p->in);
    if (p->out >= 0)
        close(p->out);
    if (p->err >= 0)
        close(p->err);
    ch_mutex_clear(&p->lock);
    free(p);
}
bool ch_proc_communicate(ChProc *p, const char *input, const ChCancel *cancel, char **out, char **err,
                         ChError **error) {
    ChBuf o, e;
    ch_buf_init(&o);
    ch_buf_init(&e);
    size_t written = 0, total = input ? strlen(input) : 0;
    int fds[3] = {p->in, p->out, p->err};
    if (fds[0] >= 0 && !total) {
        close(fds[0]);
        fds[0] = p->in = -1;
    }
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0)
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
    bool ok = true;
    while (fds[0] >= 0 || fds[1] >= 0 || fds[2] >= 0) {
        if (ch_cancel_is_cancelled(cancel)) {
            ch_set_error_literal(error, CH_ERR_CANCELLED, 0, "Operation was cancelled");
            ok = false;
            break;
        }
        struct pollfd poll_fds[3];
        int map[3], n = 0;
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0) {
                poll_fds[n] = (struct pollfd){fds[i], i == 0 ? POLLOUT : POLLIN, 0};
                map[n++] = i;
            }
        int ready = poll(poll_fds, (nfds_t)n, 100);
        if (ready < 0 && errno != EINTR) {
            ch_set_error(error, CH_ERR_IO, errno, "Cannot read process output: %s", strerror(errno));
            ok = false;
            break;
        }
        for (int k = 0; k < n && ready > 0; k++) {
            int i = map[k];
            if (!poll_fds[k].revents)
                continue;
            if (i == 0) {
                ssize_t w = write(fds[0], input + written, total - written);
                if (w > 0)
                    written += (size_t)w;
                if ((w < 0 && errno != EAGAIN && errno != EINTR) || written == total) {
                    close(fds[0]);
                    fds[0] = p->in = -1;
                }
                continue;
            }
            char chunk[16384];
            ssize_t r = read(fds[i], chunk, sizeof(chunk));
            if (r > 0)
                ch_buf_append_len(i == 1 ? &o : &e, chunk, (size_t)r);
            else if (r == 0 || (errno != EAGAIN && errno != EINTR)) {
                close(fds[i]);
                fds[i] = -1;
                if (i == 1)
                    p->out = -1;
                else
                    p->err = -1;
            }
        }
    }
    if (!ok) {
        ch_proc_kill(p);
        ch_proc_wait(p, NULL);
        ch_buf_clear(&o);
        ch_buf_clear(&e);
        return false;
    }
    while (!ch_proc_try_wait(p, NULL)) {
        if (ch_cancel_is_cancelled(cancel)) {
            ch_proc_kill(p);
            ch_proc_wait(p, NULL);
            ch_buf_clear(&o);
            ch_buf_clear(&e);
            ch_set_error_literal(error, CH_ERR_CANCELLED, 0, "Operation was cancelled");
            return false;
        }
        ch_usleep(10000);
    }
    if (out)
        *out = ch_buf_steal(&o);
    else
        ch_buf_clear(&o);
    if (err)
        *err = ch_buf_steal(&e);
    else
        ch_buf_clear(&e);
    return true;
}
