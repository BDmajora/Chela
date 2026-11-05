/* Inference activity above the composer. The interface never estimates tokens
 * from text chunks; token counts and speed come from the server's telemetry. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    ChConfig config;
    ChCancel *cancel;
    int64_t request;
    json_object *models;
    ChError *error;
} HealthProbe;
static void health_work(void *data) {
    HealthProbe *p = data;
    p->models = ch_models(&p->config, p->cancel, &p->error);
}
static void health_done(void *data) {
    HealthProbe *p = data;
    App *a = p->app;
    if (a->health_cancel == p->cancel) {
        a->health_pending = false;
        ch_clear_pointer(&a->health_cancel, ch_cancel_unref);
    }
    bool cancelled = ch_cancel_is_cancelled(p->cancel);
    /* Successful checks also keep the model status chip current during a request. */
    if (!a->closing && p->models && !cancelled)
        model_state_apply(a, p->config.model, p->models, NULL);
    if (!a->closing && a->busy && a->request_started_us == p->request && !cancelled) {
        free(a->health_state);
        free(a->health_error);
        a->health_error = p->error ? ch_strdup(p->error->message) : NULL;
        a->health_state = ch_strdup(p->error ? "Status check failed" : "Server reachable");
        for (size_t i = 0; p->models && i < json_object_array_length(p->models); i++) {
            json_object *model = json_object_array_get_idx(p->models, i), *status = ch_json_get(model, "status");
            if (strcmp(ch_json_str(model, "id"), p->config.model))
                continue;
            const char *state = ch_json_str(status, "value");
            free(a->health_state);
            a->health_state = ch_strdup(json_object_get_boolean(ch_json_get(status, "failed")) ? "Model load failed"
                                        : !strcmp(state, "loading")  ? "Model loading"
                                        : !strcmp(state, "loaded")   ? "Model loaded"
                                        : !strcmp(state, "sleeping") ? "Model sleeping"
                                                                     : "Server reachable");
        }
        cu_invalidate(a->cu);
    }
    if (p->models)
        json_object_put(p->models);
    ch_clear_error(&p->error);
    ch_cancel_unref(p->cancel);
    free(p);
}
static void monitor_probe(App *a) {
    HealthProbe *p = ch_new0(HealthProbe, 1);
    p->app = a;
    p->config = a->config;
    p->config.ssh_enabled = false; /* inspect the current API without waiting for a tunnel */
    p->config.timeout = 3;
    p->request = a->request_started_us;
    p->cancel = ch_cancel_new();
    a->health_cancel = ch_cancel_ref(p->cancel);
    a->health_pending = true;
    a->last_health_poll = ch_monotonic_us();
    app_run(a, health_work, health_done, p);
}
void monitor_reset(App *a) {
    if (a->health_cancel)
        ch_cancel_cancel(a->health_cancel);
    ch_clear_pointer(&a->health_cancel, ch_cancel_unref);
    a->health_pending = false;
    a->request_started_us = a->request_ended_us = a->last_health_poll = 0;
    a->last_network_us = a->last_token_us = 0;
    if (a->metrics)
        json_object_put(a->metrics);
    a->metrics = NULL;
    ch_clear_pointer(&a->run_phase, free);
    ch_clear_pointer(&a->last_request_error, free);
    ch_clear_pointer(&a->health_state, free);
    ch_clear_pointer(&a->health_error, free);
    a->status_visible = false;
}
void monitor_start(App *a) {
    monitor_reset(a);
    a->request_started_us = ch_monotonic_us();
    a->run_phase = ch_strdup("Connecting…");
    a->status_visible = true;
}
void monitor_finish(App *a, const ChError *error) {
    a->request_ended_us = ch_monotonic_us();
    if (a->health_cancel)
        ch_cancel_cancel(a->health_cancel);
    ch_clear_pointer(&a->health_cancel, ch_cancel_unref);
    a->health_pending = false;
    free(a->last_request_error);
    a->last_request_error = error ? ch_strdup(error->message) : NULL;
    /* The transcript now shows the outcome: the numbers under the reply, or the stopped note. */
    a->status_visible = false;
    a->request_started_us = 0;
    a->status_until = 0;
}
/* A passing note, such as a file that could not be attached. */
static void note_build(App *a) {
    Cu *cu = a->cu;
    int64_t now = ch_monotonic_us();
    if (a->status_until && now >= a->status_until) {
        a->status_visible = false;
        a->status_until = 0;
        return;
    }
    cu_open(cu, cu_id(cu, "monitor"), &(CuBox){.row = true, .w = CU_GROW, .gap = 8, .align_y = CU_CENTER, CU_PAD2(2, 10)});
    cu_text(cu, cu_id(cu, "monitor-status"), a->status ? a->status : "",
            &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    if (ui_icon_button_sized(a, cu_id(cu, "monitor-dismiss"), "window-close", 13, 2, "Dismiss", true, false)) {
        a->status_visible = false;
        a->status_until = 0;
    }
    cu_close(cu);
    if (a->status_until)
        cu_animate(cu, 500);
}
/* What the running request is doing. It appears only while one runs. */
void monitor_build(App *a) {
    Cu *cu = a->cu;
    if (a->worktree_pending || a->worktree_removing) {
        cu_open(cu, cu_id(cu, "monitor"), &(CuBox){.row = true, .w = CU_GROW, .gap = 8, .align_y = CU_CENTER, CU_PAD2(2, 10)});
        ui_spinner(a, 13);
        ui_label(a, a->worktree_removing ? "Removing worktree…" : ch_cancel_is_cancelled(a->cancel) ? "Stopping…" : "Creating worktree…",
                 12, 0xffd8d8d8);
        cu_close(cu);
        return;
    }
    if (!a->status_visible)
        return;
    if (!a->busy) {
        note_build(a);
        return;
    }
    int64_t now = ch_monotonic_us();
    ch_mutex_lock(&a->event_mutex);
    int64_t network = a->last_network_us, token = a->last_token_us;
    ch_mutex_unlock(&a->event_mutex);
    if (a->config.provider != 5 && !a->health_pending && now - a->last_health_poll >= 3000000 && !a->approval_dialog)
        monitor_probe(a);
    const char *phase = a->run_phase ? a->run_phase : "Working…";
    const char *kind = ch_json_str(a->metrics, "phase");
    bool model_wait = !*kind || !strcmp(kind, "waiting") || !strcmp(kind, "prefill");
    bool loading = model_wait && !ch_strcmp0(a->health_state, "Model loading") && a->load_started_us && !token;
    if (loading)
        phase = "Loading model…";
    if (!strcmp(kind, "prefill"))
        phase = "Processing prompt…";
    if (!ch_strcmp0(a->health_state, "Model load failed"))
        phase = "Model load failed · open Diagnostics";
    if (ch_cancel_is_cancelled(a->cancel))
        phase = "Stopping…";
    cu_open(cu, cu_id(cu, "monitor"), &(CuBox){.w = CU_GROW, CU_PAD2(2, 10), .gap = 4});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER, .w = CU_GROW});
    ui_spinner(a, 13);
    cu_open(cu, cu_id(cu, "monitor-status"), &(CuBox){.w = CU_GROW, .tooltip = a->last_request_error ? a->last_request_error : phase});
    cu_text(cu, 0, phase, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = 0xffd8d8d8, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    cu_close(cu);
    /* Tokens per second once they arrive; a request that has not produced any shows why. */
    double total = json_object_get_double(ch_json_get(a->metrics, "prompt_total"));
    double processed = json_object_get_double(ch_json_get(a->metrics, "prompt_processed"));
    ChBuf detail;
    ch_buf_init(&detail);
    if (a->metrics && json_object_get_int64(ch_json_get(a->metrics, "output_tokens")) > 0) {
        char *summary = ch_stats_summary(a->metrics);
        ch_buf_append(&detail, summary);
        free(summary);
    } else if (total > 0)
        ch_buf_printf(&detail, "Prompt %.0f / %.0f tokens", processed, total);
    if (!a->approval_dialog && !ch_str_has_prefix(phase, "Using") && !ch_str_has_prefix(phase, "Tool")) {
        int64_t silence = (now - (token ? token : network ? network : a->request_started_us)) / CH_USEC_PER_SEC;
        if (silence >= 15) {
            ch_buf_printf(&detail, "%sNo %s for %llds", detail.len ? " · " : "", token ? "new tokens" : "response data", (long long)silence);
            char *slot = model_slot_text(a, token != 0);
            if (slot)
                ch_buf_printf(&detail, " · %s", slot);
            free(slot);
        }
    }
    if (detail.len) {
        cu_open(cu, cu_id(cu, "monitor-metrics"), &(CuBox){.w = CU_FIT_MAX(380), .tooltip = cu_frame_printf(cu, "%s", detail.str)});
        cu_text(cu, 0, detail.str, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xffaaaaaa, .ellipsize = CU_ELLIPSIZE_END, .w = CU_FIT_MAX(380)});
        cu_close(cu);
    }
    ch_buf_clear(&detail);
    int64_t elapsed = (now - a->request_started_us) / CH_USEC_PER_SEC;
    char clock[32];
    snprintf(clock, sizeof(clock), "%lld:%02lld", (long long)(elapsed / 60), (long long)(elapsed % 60));
    ui_label(a, clock, 11, 0xffaaaaaa);
    cu_animate(cu, 250);
    cu_close(cu);
    if (loading)
        model_progress_build(a);
    else if (!strcmp(kind, "prefill") && total > 0)
        ui_progress(a, cu_id(cu, "monitor-progress"), CH_CLAMP(processed / total, 0, 1), 0);
    cu_close(cu);
}
char *diagnostics_snapshot(App *a) {
    char *summary = ch_stats_summary(a->metrics);
    ch_mutex_lock(&a->event_mutex);
    int64_t network = a->last_network_us, token = a->last_token_us;
    ch_mutex_unlock(&a->event_mutex);
    int64_t now = ch_monotonic_us();
    ChBuf s;
    ch_buf_init(&s);
    ch_buf_printf(&s, "Model: %s\nServer: %s\nPhase: %s\nCurrent model call: %s\n", a->config.model, a->config.endpoint,
                  a->run_phase ? a->run_phase : "Idle", summary);
    free(summary);
    if (a->request_started_us)
        ch_buf_printf(&s, "Elapsed: %.1fs\n", (double)((a->request_ended_us ? a->request_ended_us : now) - a->request_started_us) / 1e6);
    if (network)
        ch_buf_printf(&s, "Last response data: %.1fs ago\n", (double)(now - network) / 1e6);
    if (token)
        ch_buf_printf(&s, "Last generated text/thinking: %.1fs ago\n", (double)(now - token) / 1e6);
    if (ch_json_get(a->metrics, "first_token_ms"))
        ch_buf_printf(&s, "Time to first text/thinking: %.1fs\n", json_object_get_double(ch_json_get(a->metrics, "first_token_ms")) / 1000.0);
    char *slot = a->busy ? model_slot_text(a, token != 0) : NULL;
    if (slot)
        ch_buf_printf(&s, "Server activity: %s\n", slot);
    free(slot);
    json_object *device = model_device(a);
    ch_buf_printf(&s, "Model state: %s%s\n", model_state_name(a->model_state ? a->model_state : ""),
                  a->preload ? " · pre-load on" : " · pre-load off");
    if (a->last_load_seconds > 0)
        ch_buf_printf(&s, "Last load time: %.1fs\n", a->last_load_seconds);
    if (device)
        ch_buf_printf(&s, "Device: %s\n", ch_json_str(device, "summary"));
    char *memory = model_memory_text(a), *live = model_live_text(a, false);
    if (memory)
        ch_buf_printf(&s, "Model memory: %s\n", memory);
    if (live)
        ch_buf_printf(&s, "%s\n", live);
    free(memory);
    free(live);
    if (a->load_error)
        ch_buf_printf(&s, "\nLast load error: %s\n", a->load_error);
    if (a->last_request_error)
        ch_buf_printf(&s, "\nLast request error: %s\n", a->last_request_error);
    if (a->health_error)
        ch_buf_printf(&s, "\nLast status check: %s\n", a->health_error);
    return ch_buf_steal(&s);
}
