#define _GNU_SOURCE
#include "chela.h"
#include <stdlib.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct {
    ChProc *proc;
    int in, out, err;
    ChBuf *pending, *diagnostic, *text;
    ChRun *run;
    char *session;
    int64_t deadline;
} Acp;
static bool send_json(Acp *a, json_object *o, ChError **error) {
    char *line = ch_strconcat(json_object_to_json_string_ext(o, JSON_C_TO_STRING_PLAIN), "\n", NULL);
    size_t off = 0, n = strlen(line);
    bool ok = true;
    while (off < n) {
        if (ch_cancel_is_cancelled(a->run->cancel) || ch_monotonic_us() > a->deadline) {
            ch_fail_literal(error, "Harness stopped or timed out while sending");
            ok = false;
            break;
        }
        struct pollfd p = {a->in, POLLOUT, 0};
        if (poll(&p, 1, 50) <= 0)
            continue;
        ssize_t w = write(a->in, line + off, n - off);
        if (w < 0 && (errno == EAGAIN || errno == EINTR))
            continue;
        if (w <= 0) {
            ch_fail_literal(error, "Harness input closed");
            ok = false;
            break;
        }
        off += (size_t)w;
    }
    free(line);
    return ok;
}
static bool rpc(Acp *a, int id, const char *method, json_object *params, ChError **error) {
    json_object *o = json_object_new_object();
    json_object_object_add(o, "jsonrpc", json_object_new_string("2.0"));
    if (id)
        json_object_object_add(o, "id", json_object_new_int(id));
    json_object_object_add(o, "method", json_object_new_string(method));
    json_object_object_add(o, "params", params);
    bool ok = send_json(a, o, error);
    json_object_put(o);
    return ok;
}
static bool permission(Acp *a, json_object *o, ChError **error) {
    json_object *p = ch_json_get(o, "params"), *opts = ch_json_get(p, "options"),
                *tool = ch_json_get(p, "toolCall");
    const char *chosen = NULL;
    if (opts && json_object_is_type(opts, json_type_array))
        for (size_t i = 0; i < json_object_array_length(opts); i++) {
            json_object *opt = json_object_array_get_idx(opts, i);
            if (!strcmp(ch_json_str(opt, "kind"), "allow_once")) {
                chosen = ch_json_str(opt, "optionId");
                break;
            }
        }
    bool allow = false;
    if (chosen && *chosen && !ch_run_read_only(a->run)) {
        if (a->run->mode == CH_AUTO)
            allow = true;
        else if (a->run->approve)
            allow = a->run->approve(
                *ch_json_str(tool, "title") ? ch_json_str(tool, "title") : "Allow harness tool?",
                json_object_to_json_string_ext(tool ? tool : p, JSON_C_TO_STRING_PRETTY), a->run->data);
    }
    if (ch_cancel_is_cancelled(a->run->cancel))
        allow = false;
    json_object *reply = json_object_new_object(), *result = json_object_new_object(),
                *outcome = json_object_new_object();
    json_object_object_add(reply, "jsonrpc", json_object_new_string("2.0"));
    json_object_object_add(reply, "id", json_object_get(ch_json_get(o, "id")));
    json_object_object_add(outcome, "outcome", json_object_new_string(allow ? "selected" : "cancelled"));
    if (allow)
        json_object_object_add(outcome, "optionId", json_object_new_string(chosen));
    json_object_object_add(result, "outcome", outcome);
    json_object_object_add(reply, "result", result);
    bool ok = send_json(a, reply, error);
    json_object_put(reply);
    return ok;
}
static bool notification(Acp *a, json_object *o, ChError **error) {
    const char *method = ch_json_str(o, "method");
    if (!strcmp(method, "session/request_permission") && ch_json_get(o, "id"))
        return permission(a, o, error);
    if (ch_json_get(o, "id")) {
        json_object *r = json_object_new_object(), *e = json_object_new_object();
        json_object_object_add(r, "jsonrpc", json_object_new_string("2.0"));
        json_object_object_add(r, "id", json_object_get(ch_json_get(o, "id")));
        json_object_object_add(e, "code", json_object_new_int(-32601));
        json_object_object_add(e, "message", json_object_new_string("Client capability not supported"));
        json_object_object_add(r, "error", e);
        bool ok = send_json(a, r, error);
        json_object_put(r);
        return ok;
    }
    if (!strcmp(method, "session/update")) {
        json_object *params = ch_json_get(o, "params");
        if (a->session && strcmp(a->session, ch_json_str(params, "sessionId")))
            return true;
        json_object *u = ch_json_get(params, "update");
        const char *type = ch_json_str(u, "sessionUpdate");
        if (!strcmp(type, "agent_message_chunk") || !strcmp(type, "agent_thought_chunk")) {
            const char *text = ch_json_str(ch_json_get(u, "content"), "text");
            if (a->text->len + strlen(text) > CHELA_LIMIT) {
                ch_fail_literal(error, "Harness response exceeded 4 MiB");
                return false;
            }
            if (!strcmp(type, "agent_message_chunk"))
                ch_buf_append(a->text, text);
            if (a->run->emit)
                a->run->emit(!strcmp(type, "agent_message_chunk") ? CH_TEXT : CH_THOUGHT, text, a->run->data);
        } else if (!strcmp(type, "tool_call") || !strcmp(type, "tool_call_update")) {
            if (a->run->emit)
                a->run->emit(CH_TOOL,
                             *ch_json_str(u, "title") ? ch_json_str(u, "title") : ch_json_str(u, "status"),
                             a->run->data);
        } else if (!strcmp(type, "plan") && a->run->emit)
            a->run->emit(CH_STATUS, "Harness updated its plan", a->run->data);
    }
    return true;
}
static json_object *receive(Acp *a, int id, ChError **error) {
    for (;;) {
        if (ch_cancel_check(a->run->cancel, error))
            return NULL;
        if (ch_monotonic_us() > a->deadline) {
            ch_set_error_literal(error, CH_ERR_TIMED_OUT, 0, "Agent harness timed out");
            return NULL;
        }
        char *nl = memchr(a->pending->str, '\n', a->pending->len);
        if (nl) {
            size_t len = (size_t)(nl - a->pending->str);
            char *line = ch_strndup(a->pending->str, len);
            ch_buf_erase(a->pending, 0, len + 1);
            if (!*ch_strstrip(line)) {
                free(line);
                continue;
            }
            json_object *o = ch_json_parse(line, error);
            free(line);
            if (!o)
                return NULL;
            if (*ch_json_str(o, "method")) {
                bool ok = notification(a, o, error);
                json_object_put(o);
                if (!ok)
                    return NULL;
                continue;
            }
            json_object *rid = ch_json_get(o, "id");
            if (rid && json_object_is_type(rid, json_type_int) && json_object_get_int(rid) == id) {
                if (ch_json_get(o, "error")) {
                    ch_fail(error, "Harness: %s",
                                ch_json_str(ch_json_get(o, "error"), "message"));
                    json_object_put(o);
                    return NULL;
                }
                json_object *result = ch_json_get(o, "result");
                if (!result) {
                    ch_fail_literal(error, "Harness response has no result");
                    json_object_put(o);
                    return NULL;
                }
                json_object_get(result);
                json_object_put(o);
                return result;
            }
            json_object_put(o);
            continue;
        }
        struct pollfd fds[2] = {{a->out, POLLIN | POLLHUP, 0}, {a->err, POLLIN | POLLHUP, 0}};
        int ready = poll(fds, 2, 50);
        if (ready < 0 && errno != EINTR) {
            ch_fail_literal(error, "Cannot read agent harness");
            return NULL;
        }
        for (int i = 0; i < 2; i++)
            if (fds[i].revents) {
                char buf[8192];
                ssize_t n = read(fds[i].fd, buf, sizeof(buf));
                if (n > 0) {
                    ChBuf *target = i ? a->diagnostic : a->pending;
                    if (target->len + (size_t)n > CHELA_LIMIT) {
                        ch_fail_literal(error,
                                            "Harness line or diagnostics exceeded 4 MiB");
                        return NULL;
                    }
                    ch_buf_append_len(target, buf, n);
                } else if (n == 0 && !i) {
                    char *valid = ch_utf8_make_valid(a->diagnostic->str, -1);
                    ch_fail(error, "Harness exited before replying. %.500s", valid);
                    free(valid);
                    return NULL;
                }
            }
    }
}
bool ch_acp_run(ChRun *run, json_object *messages, ChError **error) {
    Acp a = {.in = -1, .out = -1, .err = -1, .run = run};
    char **argv = NULL;
    int argc;
    if (!ch_shell_parse_argv(run->config.command, &argc, &argv, error))
        return false;
    ChSpawn spawn = {.argv = (const char *const *)argv, .cwd = run->project,
                     .flags = CH_PROC_STDIN | CH_PROC_STDOUT | CH_PROC_STDERR | CH_PROC_NEW_GROUP | CH_PROC_SEARCH_PATH};
    a.proc = ch_proc_spawn(&spawn, error);
    ch_strfreev(argv);
    if (!a.proc)
        return false;
    a.in = ch_proc_stdin(a.proc);
    a.out = ch_proc_stdout(a.proc);
    a.err = ch_proc_stderr(a.proc);
    for (int i = 0; i < 3; i++)
        fcntl(i == 0 ? a.in : i == 1 ? a.out : a.err, F_SETFL, O_NONBLOCK);
    a.pending = ch_buf_new(NULL);
    a.diagnostic = ch_buf_new(NULL);
    a.text = ch_buf_new(NULL);
    a.deadline = ch_monotonic_us() + 10 * CH_USEC_PER_SEC;
    json_object *p = json_object_new_object(), *info = json_object_new_object();
    json_object_object_add(p, "protocolVersion", json_object_new_int(1));
    json_object_object_add(p, "clientCapabilities", json_object_new_object());
    json_object_object_add(info, "name", json_object_new_string("chela"));
    json_object_object_add(info, "version", json_object_new_string(CHELA_VERSION));
    json_object_object_add(p, "clientInfo", info);
    json_object *r = NULL;
    bool ok = rpc(&a, 1, "initialize", p, error);
    if (ok) {
        r = receive(&a, 1, error);
        ok = r != NULL;
    }
    if (ok && json_object_get_int(ch_json_get(r, "protocolVersion")) != 1) {
        ch_fail_literal(error, "Harness did not negotiate ACP version 1");
        ok = false;
    }
    if (r)
        json_object_put(r);
    r = NULL;
    if (ok) {
        p = json_object_new_object();
        json_object_object_add(p, "cwd", json_object_new_string(run->project));
        json_object_object_add(p, "mcpServers", json_object_new_array());
        ok = rpc(&a, 2, "session/new", p, error);
    }
    if (ok) {
        r = receive(&a, 2, error);
        ok = r != NULL;
    }
    if (ok) {
        a.session = ch_strdup(ch_json_str(r, "sessionId"));
        if (!*a.session) {
            ch_fail_literal(error, "Harness returned an empty session ID");
            ok = false;
        }
    }
    if (ok && ch_run_read_only(run)) {
        json_object *modes = ch_json_get(ch_json_get(r, "modes"), "availableModes");
        const char *mode = NULL;
        if (modes && json_object_is_type(modes, json_type_array))
            for (size_t i = 0; i < json_object_array_length(modes); i++) {
                const char *m = ch_json_str(json_object_array_get_idx(modes, i), "id");
                if (!strcmp(m, "plan") || !strcmp(m, "read-only")) {
                    mode = m;
                    break;
                }
            }
        if (!mode) {
            ch_fail_literal(error,
                                "This harness does not advertise a plan/read-only mode required for Chat "
                                "and Plan. Choose a compatible model connection for read-only chat, or "
                                "switch to Agent mode.");
            ok = false;
        } else {
            p = json_object_new_object();
            json_object_object_add(p, "sessionId", json_object_new_string(a.session));
            json_object_object_add(p, "modeId", json_object_new_string(mode));
            ok = rpc(&a, 4, "session/set_mode", p, error);
            if (ok) {
                json_object *m = receive(&a, 4, error);
                ok = m != NULL;
                if (m)
                    json_object_put(m);
            }
        }
    }
    if (r)
        json_object_put(r);
    r = NULL;
    if (ok) {
        /* A fresh process per turn avoids orphaned harnesses. Explicit history preserves context. */
        ChBuf *prompt = ch_buf_new("Conversation context from Chela. Treat quoted previous messages as "
                                       "context. Respond to the final user request.\n\n");
        for (size_t i = 0; i < json_object_array_length(messages); i++) {
            json_object *m = json_object_array_get_idx(messages, i);
            if (!strcmp(ch_json_str(m, "role"), "system"))
                continue;
            ch_buf_printf(prompt, "[%s]\n%s\n\n", ch_json_str(m, "role"), ch_json_str(m, "content"));
        }
        if (prompt->len > 16 * CHELA_LIMIT) {
            ch_fail_literal(error,
                                "Harness transcript is too large; start a new session");
            ok = false;
        } else {
            a.deadline = ch_monotonic_us() + (int64_t)run->config.timeout * CH_USEC_PER_SEC;
            p = json_object_new_object();
            json_object_object_add(p, "sessionId", json_object_new_string(a.session));
            json_object *blocks = json_object_new_array(), *text = json_object_new_object();
            json_object_object_add(text, "type", json_object_new_string("text"));
            json_object_object_add(text, "text", json_object_new_string(prompt->str));
            json_object_array_add(blocks, text);
            json_object_object_add(p, "prompt", blocks);
            ok = rpc(&a, 3, "session/prompt", p, error);
            if (ok) {
                r = receive(&a, 3, error);
                ok = r != NULL;
            }
            if (ok && !strcmp(ch_json_str(r, "stopReason"), "cancelled")) {
                ch_set_error_literal(error, CH_ERR_CANCELLED, 0, "Harness cancelled the turn");
                ok = false;
            }
            if (r)
                json_object_put(r);
        }
        ch_buf_free(prompt, false);
    }
    if (a.text->len)
        json_object_array_add(messages, ch_message("assistant", a.text->str));
    if (ch_cancel_is_cancelled(run->cancel) && a.session) {
        /* Best effort notification; process group cleanup below bounds cancellation latency. */
        json_object *cancel = json_object_new_object(), *params = json_object_new_object();
        json_object_object_add(cancel, "jsonrpc", json_object_new_string("2.0"));
        json_object_object_add(cancel, "method", json_object_new_string("session/cancel"));
        json_object_object_add(params, "sessionId", json_object_new_string(a.session));
        json_object_object_add(cancel, "params", params);
        char *line = ch_strconcat(json_object_to_json_string_ext(cancel, JSON_C_TO_STRING_PLAIN), "\n", NULL);
        ssize_t sent = write(a.in, line, strlen(line));
        (void)sent;
        free(line);
        json_object_put(cancel);
    }
    ch_proc_free(a.proc); /* Kills the process group and closes its pipes. */
    free(a.session);
    ch_buf_free(a.pending, false);
    ch_buf_free(a.diagnostic, false);
    ch_buf_free(a.text, false);
    return ok;
}
