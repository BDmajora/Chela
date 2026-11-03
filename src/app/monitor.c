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
    const char *phase = error ? ch_error_is(error, CH_ERR_CANCELLED) ? "Stopped" : "Request failed · open Diagnostics"
                      : !strcmp(ch_json_str(a->metrics, "finish_reason"), "length") ? "Output limit reached"
                                                                                    : "Completed";
    app_set_status(a, phase);
}
void monitor_build(App *a) {
    Cu *cu = a->cu;
    if (!a->status_visible)
        return;
    int64_t now = ch_monotonic_us();
    bool request = a->request_started_us != 0;
    bool failed = request && a->last_request_error && a->request_ended_us;
    ch_mutex_lock(&a->event_mutex);
    int64_t network = a->last_network_us, token = a->last_token_us;
    ch_mutex_unlock(&a->event_mutex);
    if (a->busy && a->config.provider != 5 && !a->health_pending && now - a->last_health_poll >= 3000000 &&
        !a->approval_dialog)
        monitor_probe(a);
    const char *phase = request ? (a->run_phase ? a->run_phase : "Working…") : a->status ? a->status : "";
    const char *kind = ch_json_str(a->metrics, "phase");
    bool model_wait = !*kind || !strcmp(kind, "waiting") || !strcmp(kind, "prefill");
    if (a->busy && model_wait && !ch_strcmp0(a->health_state, "Model loading"))
        phase = "Loading model…";
    if (a->busy && !strcmp(kind, "prefill"))
        phase = "Processing prompt…";
    if (a->busy && !ch_strcmp0(a->health_state, "Model load failed"))
        phase = "Model load failed · open Diagnostics";
    if (a->busy && ch_cancel_is_cancelled(a->cancel))
        phase = "Stopping…";
    cu_open(cu, cu_id(cu, "monitor"), &(CuBox){.w = CU_GROW, .bg = 0xff232323, .border = 1,
                                               .border_color = failed && !ch_strcmp0(phase, "Request failed · open Diagnostics") ? 0xffba705c : 0xff3a3a3a,
                                               .radius = 9, CU_PAD2(7, 10), .gap = 5});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER, .w = CU_GROW});
    if (a->busy)
        ui_spinner(a, 14);
    bool error_phase = failed && strcmp(phase, "Stopped");
    cu_open(cu, cu_id(cu, "monitor-status"), &(CuBox){.w = CU_GROW, .tooltip = a->last_request_error ? a->last_request_error : phase});
    cu_text(cu, 0, phase, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = error_phase ? 0xffefa68e : 0xffe1e1e1,
                                         .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    cu_close(cu);
    if (request) {
        int64_t elapsed = ((a->request_ended_us ? a->request_ended_us : now) - a->request_started_us) / CH_USEC_PER_SEC;
        char clock[32];
        snprintf(clock, sizeof(clock), "%lld:%02lld", (long long)(elapsed / 60), (long long)(elapsed % 60));
        ui_label(a, clock, 11, 0xffaaaaaa);
        if (a->busy)
            cu_animate(cu, 250);
    }
    if (ui_flat_button(a, cu_id(cu, "monitor-diagnostics"), NULL, "Diagnostics", NULL, 11, C_TEXT,
                       "Server status, device and recent logs (Ctrl+Shift+D)", true, 0))
        open_diagnostics(a);
    if (!a->busy && ui_icon_button_sized(a, cu_id(cu, "monitor-dismiss"), "window-close", 13, 2, "Dismiss", true, false))
        a->status_visible = false;
    cu_close(cu);
    if (request) {
        double total = json_object_get_double(ch_json_get(a->metrics, "prompt_total"));
        double processed = json_object_get_double(ch_json_get(a->metrics, "prompt_processed"));
        ChBuf detail;
        ch_buf_init(&detail);
        /* "0 output tokens · speed not reported" says nothing while the prompt is still being read. */
        if (a->metrics && (json_object_get_int64(ch_json_get(a->metrics, "output_tokens")) > 0 || !a->busy)) {
            char *summary = ch_stats_summary(a->metrics);
            ch_buf_append(&detail, summary);
            free(summary);
        } else if (a->busy && total > 0)
            ch_buf_printf(&detail, "Prompt %.0f / %.0f tokens · waiting for the first token", processed, total);
        else
            ch_buf_append(&detail, a->config.provider == 5 ? "Token speed is not reported by this harness" : "Waiting for token counts and speed");
        /* Prompt progress or tokens show the model is running, whatever the last status check said. */
        bool running = token || !strcmp(kind, "prefill") || !strcmp(kind, "generating");
        if (a->busy && a->health_state && !(running && !strcmp(a->health_state, "Model loading")))
            ch_buf_printf(&detail, " · %s", a->health_state);
        if (a->busy && !ch_strcmp0(a->health_state, "Model loading") && a->load_started_us) {
            char *eta = model_eta_text(a, NULL);
            ch_buf_printf(&detail, " · %s", eta);
            free(eta);
        }
        json_object *device = model_device(a);
        if (*model_backend(a)) {
            json_object *used = ch_json_get(device, "actual_devices");
            const char *name = used && json_object_is_type(used, json_type_array) && json_object_array_length(used)
                                   ? ch_json_str(json_object_array_get_idx(used, 0), "name") : "";
            ch_buf_printf(&detail, " · on %s%s%s%s", model_backend(a), *name ? " (" : "", name, *name ? ")" : "");
        }
        char *busy = a->busy ? model_live_text(a, true) : NULL;
        if (busy)
            ch_buf_printf(&detail, " · %s", busy);
        free(busy);
        if (a->busy && !a->approval_dialog && !ch_str_has_prefix(phase, "Using") && !ch_str_has_prefix(phase, "Tool")) {
            int64_t silence = (now - (token ? token : network ? network : a->request_started_us)) / CH_USEC_PER_SEC;
            if (silence >= 15) {
                ch_buf_printf(&detail, " · No %s for %llds", token ? "new tokens" : "response data", (long long)silence);
                char *slot = model_slot_text(a, token != 0);
                if (slot)
                    ch_buf_printf(&detail, " · %s", slot);
                free(slot);
            }
        }
        cu_open(cu, cu_id(cu, "monitor-metrics"), &(CuBox){.w = CU_GROW, .tooltip = cu_frame_printf(cu, "%s", detail.str)});
        cu_text(cu, 0, detail.str, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xffaaaaaa, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
        cu_close(cu);
        ch_buf_clear(&detail);
        bool prefill = a->busy && !strcmp(kind, "prefill");
        if (a->busy && (prefill || !token))
            ui_progress(a, cu_id(cu, "monitor-progress"), prefill && total > 0 ? CH_CLAMP(processed / total, 0, 1) : -1, 0);
    }
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
                  a->keep_loaded ? " · keep loaded on" : " · keep loaded off");
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
