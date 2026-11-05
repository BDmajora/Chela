/* A user-selected local transcription app supplies text; no microphone data is sent by Chela. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    ChProc *proc;
    ChCancel *cancel;
    char *output;
    bool ok;
    ChError *error;
} Dictation;
static void dictation_work(void *data) {
    Dictation *d = data;
    d->ok = ch_proc_communicate(d->proc, NULL, d->cancel, &d->output, NULL, &d->error);
}
static void dictation_done(void *data) {
    Dictation *d = data;
    App *a = d->app;
    Dialog *listening = dialog_find(a, a->dictation_dialog);
    if (listening)
        dialog_close(a, listening);
    a->dictation_dialog = 0;
    bool cancelled = a->dictation_cancelled || ch_cancel_is_cancelled(d->cancel);
    a->dictation = NULL;
    if (!a->closing) {
        if (!cancelled && d->ok && ch_proc_succeeded(d->proc) && d->output && strlen(d->output) <= 64 * 1024 &&
            ch_utf8_validate(d->output, -1, NULL)) {
            ch_strstrip(d->output);
            cu_edit_insert(&a->composer, d->output);
            cu_focus(a->cu, cu_id(a->cu, "composer"));
            app_set_status(a, *d->output ? "Dictation added · review your draft before sending" : "No speech detected");
        } else if (!cancelled)
            app_notice(a, "Dictation failed", d->error ? d->error->message : "The transcription app must return UTF-8 text, up to 64 KiB.");
    }
    ch_proc_free(d->proc);
    ch_cancel_unref(d->cancel);
    ch_clear_error(&d->error);
    free(d->output);
    free(d);
    cu_invalidate(a->cu);
}
static ChCancel *dictation_cancel;
void stop_dictation(App *a) {
    if (a->dictation) {
        a->dictation_cancelled = true;
        ch_cancel_cancel(dictation_cancel);
    }
}
static bool dictation_timeout(void *data) {
    App *a = data;
    if (a->dictation && ch_monotonic_us() - a->dictation_started >= 120 * CH_USEC_PER_SEC) {
        stop_dictation(a);
        return false;
    }
    return a->dictation != NULL;
}
void dictate(App *a) {
    if (a->busy || a->dictation)
        return;
    if (!a->dictation_command || !*a->dictation_command) {
        open_voice_settings(a);
        return;
    }
    char **argv = NULL;
    ChError *error = NULL;
    if (!ch_shell_parse_argv(a->dictation_command, NULL, &argv, &error)) {
        app_notice(a, "Dictation failed", error->message);
        ch_clear_error(&error);
        return;
    }
    ChSpawn spawn = {.argv = (const char *const *)argv, .cwd = a->code && *a->project ? a->project : NULL,
                     .flags = CH_PROC_STDOUT | CH_PROC_STDERR_SILENCE | CH_PROC_NEW_SESSION | CH_PROC_SEARCH_PATH};
    ChProc *proc = ch_proc_spawn(&spawn, &error);
    ch_strfreev(argv);
    if (!proc) {
        app_notice(a, "Could not start dictation", error->message);
        ch_clear_error(&error);
        return;
    }
    Dictation *d = ch_new0(Dictation, 1);
    d->app = a;
    d->proc = proc;
    d->cancel = ch_cancel_new();
    ch_clear_pointer(&dictation_cancel, ch_cancel_unref);
    dictation_cancel = ch_cancel_ref(d->cancel);
    a->dictation = proc;
    a->dictation_cancelled = false;
    a->dictation_started = ch_monotonic_us();
    a->dictation_dialog = open_listening(a);
    a->p->timer_add(1000, dictation_timeout, a);
    app_run(a, dictation_work, dictation_done, d);
}
