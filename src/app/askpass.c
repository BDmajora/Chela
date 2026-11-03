/* SSH_ASKPASS helper: ssh runs Chela again to ask for a password or a host-key
 * confirmation. The prompt is a small window on the same UI backend, and the
 * answer goes to standard output for ssh. */
#include "app.h"
#include "../platform/headless.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    App app;
    ChAskpass ask;
    CuEdit password;
    bool remember, checking, saving, answered, focused;
    int status;
    char *message, *tip;
    pid_t parent;
} Ask;
static void ask_message(Ask *k, const char *text, const char *tip) {
    free(k->message);
    free(k->tip);
    k->message = ch_strdup(text);
    k->tip = ch_strdup(tip);
}
static void finish(Ask *k, const char *answer) {
    if (answer) {
        printf("%s\n", answer);
        fflush(stdout);
        k->status = 0;
    }
    k->answered = true;
    k->app.p->quit();
}
typedef struct {
    Ask *ask;
    int action; /* 0 lookup, 1 store, 2 forget */
    char *password, *found;
    bool ok;
    ChError *error;
} Keyring;
static void keyring_work(void *data) {
    Keyring *r = data;
    if (r->action == 0)
        r->found = ch_ssh_password_lookup(&r->ask->ask.config, NULL, &r->error);
    else if (r->action == 1)
        r->ok = ch_ssh_password_store(&r->ask->ask.config, r->password, NULL, &r->error);
    else
        r->ok = ch_ssh_password_forget(&r->ask->ask.config, NULL, &r->error);
}
static void mark_attempt(Ask *k) {
    if (k->ask.marker)
        ch_file_set_contents(k->ask.marker, "", 0, 0600, NULL);
}
static void keyring_done(void *data) {
    Keyring *r = data;
    Ask *k = r->ask;
    if (r->action == 0) {
        k->checking = false;
        if (r->found) {
            mark_attempt(k);
            finish(k, r->found);
        } else if (r->error)
            ask_message(k, "Could not check the saved login. Sign in below; your desktop wallet may need to be unlocked.",
                        r->error->message);
    } else if (r->action == 1) {
        k->saving = false;
        if (r->ok) {
            mark_attempt(k);
            finish(k, cu_edit_text(&k->password));
        } else
            ask_message(k, "Could not remember this login. Unlock your desktop wallet and retry, or uncheck Remember login to "
                           "sign in without saving.", r->error ? r->error->message : NULL);
    } else
        k->checking = false;
    ch_secret_free(r->found);
    ch_secret_free(r->password);
    ch_clear_error(&r->error);
    free(r);
    cu_invalidate(k->app.cu);
}
static void keyring(Ask *k, int action, const char *password) {
    Keyring *r = ch_new0(Keyring, 1);
    r->ask = k;
    r->action = action;
    r->password = ch_strdup(password);
    app_run(&k->app, keyring_work, keyring_done, r);
}
static void build(Cu *cu, void *data) {
    Ask *k = data;
    App *a = &k->app;
    if (cu_close_requested(cu))
        finish(k, NULL);
    bool confirm = k->ask.confirm;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .h = CU_GROW, .bg = C_SIDEBAR, CU_PAD2(22, 24), .gap = 14});
    cu_text(cu, 0, confirm ? "Trust inference computer" : "SSH login",
            &(CuTextStyle){.font = ui_font(a, 18, 600), .color = C_TEXT});
    ui_text_block(a, cu_id(cu, "prompt"), k->ask.prompt, 14, C_TEXT2, true);
    if (!confirm) {
        CuId field = cu_id(cu, "password");
        if (!k->focused) {
            cu_focus(cu, field);
            k->focused = true;
        }
        int flags = ui_entry(a, field, &k->password, "Password", !k->checking && !k->saving, 1024);
        ui_checkbox(a, cu_id(cu, "remember"), "Remember login in my desktop keyring", &k->remember,
                    k->ask.can_remember && !k->saving);
        cu_open(cu, cu_id(cu, "status"), &(CuBox){.w = CU_GROW, .tooltip = k->tip});
        ui_label_wrap(a, k->message, 13, C_MUTED, 0);
        cu_close(cu);
        if (flags & CU_EDIT_SUBMIT)
            cu_focus(cu, cu_id(cu, "accept"));
        if ((flags & CU_EDIT_SUBMIT) && !k->saving && !k->checking) {
            if (k->ask.can_remember && k->remember) {
                k->saving = true;
                ask_message(k, "Remembering SSH login… Unlock your desktop wallet if asked.", NULL);
                keyring(k, 1, cu_edit_text(&k->password));
            } else {
                mark_attempt(k);
                finish(k, cu_edit_text(&k->password));
            }
        }
    }
    cu_spacer(cu, CU_PX(1), CU_GROW);
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 8, .align_x = CU_END});
    if (ui_button(a, cu_id(cu, "cancel"), "Cancel", true, false) || cu_key(cu, CU_KEY_ESCAPE, 0))
        finish(k, NULL);
    if (ui_button(a, cu_id(cu, "accept"), confirm ? "Trust & connect" : "Sign in", !k->checking && !k->saving, true)) {
        if (confirm)
            finish(k, "yes");
        else if (k->ask.can_remember && k->remember) {
            k->saving = true;
            ask_message(k, "Remembering SSH login… Unlock your desktop wallet if asked.", NULL);
            keyring(k, 1, cu_edit_text(&k->password));
        } else {
            mark_attempt(k);
            finish(k, cu_edit_text(&k->password));
        }
    }
    cu_close(cu);
    cu_close(cu);
}
static bool parent_check(void *data) {
    Ask *k = data;
    if (getppid() != k->parent)
        finish(k, NULL);
    return !k->answered;
}
static void paint(CuWindow *w, CuCanvas *c, int width, int height, void *data) {
    (void)w;
    cu_paint(((Ask *)data)->app.cu, c, width, height);
}
static void event(CuWindow *w, const CuEvent *e, void *data) {
    (void)w;
    cu_handle_event(((Ask *)data)->app.cu, e);
}
static bool find_text(void *data, const char *text, CuRect *rect) {
    return cu_find_text(((Ask *)data)->app.cu, text, rect);
}
static void list_text(void *data, void (*emit)(const char *text, void *ctx), void *ctx) {
    cu_list_text(((Ask *)data)->app.cu, emit, ctx);
}
int askpass_main(const CuPlatform *p, int argc, char **argv) {
    Ask k = {0};
    if (!ch_askpass_prepare(argc, argv, &k.ask))
        return 1;
    k.status = 1;
    k.parent = getppid();
    k.app.p = p;
    k.remember = k.ask.can_remember;
    ChError *lookup_error = NULL;
    if (k.ask.can_remember && !k.ask.repeated && !k.ask.confirm) {
        /* A saved login answers ssh before any window opens, so reconnecting does not flash a prompt. */
        char *saved = ch_ssh_password_lookup(&k.ask.config, NULL, &lookup_error);
        if (saved) {
            mark_attempt(&k);
            printf("%s\n", saved);
            fflush(stdout);
            ch_secret_free(saved);
            ch_askpass_clear(&k.ask);
            return 0;
        }
    }
    cu_edit_init(&k.password, false);
    k.password.password = true;
    ask_message(&k, k.ask.repeated ? "The previous password was not accepted. Please sign in again."
                    : lookup_error ? "Could not check the saved login. Sign in below; your desktop wallet may need to be unlocked."
                    : k.ask.can_remember ? "Your desktop keyring can remember this SSH login for future connections."
                                         : "This SSH prompt will not be saved. Password caching can be enabled in model library setup.",
                lookup_error ? lookup_error->message : NULL);
    ch_clear_error(&lookup_error);
    static const CuWindowHandler handler = {paint, event};
    k.app.window = p->window_new(k.ask.confirm ? "Trust inference computer · Chela" : "SSH login · Chela", "chela", 540,
                                 k.ask.confirm ? 260 : 330, false, &handler, &k);
    if (!k.app.window)
        return 1;
    k.app.cu = cu_new(p, k.app.window);
    cu_set_build(k.app.cu, build, &k);
    if (!strcmp(p->name, "headless"))
        headless_set_finder(find_text, list_text, &k);
    p->timer_add(250, parent_check, &k);
    if (k.ask.can_remember && k.ask.repeated) {
        /* A repeated prompt means the saved password failed: forget it and ask. */
        k.checking = true;
        keyring(&k, 2, NULL);
    }
    p->run();
    cu_edit_clear(&k.password);
    ch_askpass_clear(&k.ask);
    return k.status;
}
