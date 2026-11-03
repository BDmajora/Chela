/* Model residency: whether the selected model is loaded, where it runs, how much
 * memory it uses, and keeping it loaded. State comes from the model router's
 * /v1/models status; device and memory come from the SSH helper's log evidence. */
#include "app.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

const char *model_state_name(const char *state) {
    return !strcmp(state, "loaded")      ? "Ready"
         : !strcmp(state, "loading")     ? "Loading"
         : !strcmp(state, "unloaded")    ? "Not loaded"
         : !strcmp(state, "sleeping")    ? "Sleeping"
         : !strcmp(state, "failed")      ? "Load failed"
         : !strcmp(state, "downloading") ? "Downloading"
         : !strcmp(state, "missing")     ? "Not on server"
         : !strcmp(state, "offline")     ? "Not connected"
                                         : "Checking…";
}
bool model_control_supported(App *a) {
    return a->config.provider == 6 || (a->model_router && a->config.provider != 5);
}
static char *format_mib(double mib) {
    return mib >= 1024 ? ch_strdup_printf("%.1f GiB", mib / 1024.0) : ch_strdup_printf("%.0f MiB", mib);
}
char *format_clock(int64_t microseconds) {
    int64_t seconds = CH_MAX(microseconds, 0) / CH_USEC_PER_SEC;
    return ch_strdup_printf("%lld:%02lld", (long long)(seconds / 60), (long long)(seconds % 60));
}
static double previous_load_seconds(App *a) {
    double *value = ch_map_get(&a->load_seconds, a->config.model);
    return value ? *value : 0;
}
/* Everything shown about device placement comes from the selected model's current load. */
json_object *model_device(App *a) {
    json_object *device = a->device_status;
    if (!device || ch_strcmp0(ch_json_str(device, "model"), a->config.model))
        return NULL;
    /* While a load runs, a confirmed placement describes the previous process. */
    if (!ch_strcmp0(a->model_state, "loading") && json_object_get_boolean(ch_json_get(device, "confirmed")))
        return NULL;
    return device;
}
const char *model_backend(App *a) {
    json_object *device = model_device(a);
    return json_object_get_boolean(ch_json_get(device, "confirmed")) ? ch_json_str(device, "backend") : "";
}
char *model_memory_text(App *a) {
    json_object *memory = ch_json_get(model_device(a), "memory");
    if (!memory)
        return NULL;
    double gpu = json_object_get_double(ch_json_get(memory, "gpu_mib")), ram = json_object_get_double(ch_json_get(memory, "ram_mib"));
    double parts[3] = {0};
    const char *names[] = {"weights", "context", "compute"};
    json_object *devices = ch_json_get(memory, "devices");
    if (devices && json_object_is_type(devices, json_type_object)) {
        json_object_object_foreach(devices, device, values) {
            (void)device;
            for (int i = 0; i < 3; i++)
                parts[i] += json_object_get_double(ch_json_get(values, names[i]));
        }
    }
    char *g = format_mib(gpu), *r = format_mib(ram), *w = format_mib(parts[0]), *c = format_mib(parts[1]), *x = format_mib(parts[2]);
    char *text = ch_strdup_printf("%s on GPU · %s in system RAM\nWeights %s · context cache %s · compute %s", g, r, w, c, x);
    free(g), free(r), free(w), free(c), free(x);
    return text;
}
char *model_live_text(App *a, bool busy_only) {
    json_object *live = ch_json_get(model_device(a), "live"), *gpus = ch_json_get(live, "gpus");
    ChBuf text;
    ch_buf_init(&text);
    for (size_t i = 0; gpus && json_object_is_type(gpus, json_type_array) && i < json_object_array_length(gpus); i++) {
        json_object *gpu = json_object_array_get_idx(gpus, i), *busy = ch_json_get(gpu, "busy_percent");
        if (busy_only) {
            if (busy)
                ch_buf_printf(&text, "%sGPU %d%% busy", text.len ? " · " : "", json_object_get_int(busy));
            continue;
        }
        char *used = format_mib(json_object_get_double(ch_json_get(gpu, "vram_used_mib")));
        char *total = format_mib(json_object_get_double(ch_json_get(gpu, "vram_total_mib")));
        const char *name = *ch_json_str(gpu, "name") ? ch_json_str(gpu, "name") : ch_json_str(gpu, "card");
        bool several = json_object_array_length(gpus) > 1;
        ch_buf_printf(&text, "%s%s%sVRAM %s of %s in use", text.len ? "\n" : "", several ? name : "", several ? ": " : "", used, total);
        if (busy)
            ch_buf_printf(&text, " · %d%% busy", json_object_get_int(busy));
        free(used);
        free(total);
    }
    json_object *process = ch_json_get(live, "process");
    if (!busy_only && process) {
        char *rss = format_mib(json_object_get_double(ch_json_get(process, "rss_mib")));
        ch_buf_printf(&text, "%sModel process RAM %s (includes mapped weights)", text.len ? "\n" : "", rss);
        free(rss);
    }
    if (!text.len) {
        ch_buf_clear(&text);
        return NULL;
    }
    return ch_buf_steal(&text);
}
/* Why a reply is silent, from the model server's own slot monitor. */
char *model_slot_text(App *a, bool streamed) {
    json_object *slot = ch_json_get(ch_json_get(model_device(a), "live"), "slot");
    if (!slot)
        return NULL;
    if (!json_object_get_boolean(ch_json_get(slot, "available")))
        return json_object_get_boolean(ch_json_get(slot, "timed_out"))
                   ? ch_strdup("Server is inside one long model step (it did not answer a status check)") : NULL;
    if (!json_object_get_boolean(ch_json_get(slot, "processing")))
        return ch_strdup("Server reports no request in progress");
    json_object *decoded = ch_json_get(slot, "decoded");
    if (!decoded)
        return ch_strdup("Server is working on the request");
    long long n = (long long)json_object_get_int64(decoded);
    if (!n)
        return ch_strdup("Server is still reading the prompt");
    return ch_strdup_printf(streamed ? "Server has generated %lld tokens" : "Server has generated %lld tokens but has not streamed any yet", n);
}
char *model_eta_text(App *a, double *fraction) {
    double previous = previous_load_seconds(a), elapsed = (double)(ch_monotonic_us() - a->load_started_us) / 1e6;
    if (fraction)
        *fraction = previous > 0 ? CH_CLAMP(elapsed / previous, 0, .97) : -1;
    if (previous <= 0)
        return ch_strdup(a->load_observed_late ? "Started before Chela checked · timing the rest"
                                               : "First timed load of this model · timing it for next time");
    if (previous - elapsed >= 1)
        return ch_strdup_printf("About %.0fs left · last load took %.0fs", previous - elapsed, previous);
    return ch_strdup_printf("Taking longer than last time (%.0fs)", previous);
}
/* A new load replaces the previous placement; never show the old one as current. */
static void forget_device(App *a) {
    if (a->device_status)
        json_object_put(a->device_status);
    a->device_status = NULL;
    a->last_device_poll = 0;
}
void model_state_apply(App *a, const char *model, json_object *models, const ChError *error) {
    if (a->closing || strcmp(model, a->config.model))
        return;
    const char *state = "missing";
    bool router = false;
    ch_clear_pointer(&a->model_status_error, free);
    if (error) {
        state = "offline";
        a->model_status_error = ch_strdup(error->message);
    }
    for (size_t i = 0; models && i < json_object_array_length(models); i++) {
        json_object *entry = json_object_array_get_idx(models, i), *status = ch_json_get(entry, "status");
        router |= status != NULL;
        if (strcmp(ch_json_str(entry, "id"), model))
            continue;
        state = json_object_get_boolean(ch_json_get(status, "failed")) ? "failed"
              : *ch_json_str(status, "value")                         ? ch_json_str(status, "value")
                                                                      : "";
        if (!strcmp(state, "failed") && ch_json_get(status, "exit_code")) {
            free(a->model_status_error);
            a->model_status_error = ch_strdup_printf("The model process exited with code %d. Open Diagnostics for the server log.",
                                                     json_object_get_int(ch_json_get(status, "exit_code")));
        }
    }
    if (models)
        a->model_router = router;
    int64_t now = ch_monotonic_us();
    char *previous = ch_strdup(a->model_state ? a->model_state : "");
    /* A load Chela just requested is reported as unloaded until the router spawns it. */
    if (a->load_requested_us && !strcmp(state, "unloaded") && now - a->load_requested_us < 15 * CH_USEC_PER_SEC)
        state = "loading";
    if (!strcmp(state, "loading") && strcmp(previous, "loading")) {
        if (!a->load_started_us) {
            /* Without a prior idle observation, this load began at an unknown time. */
            a->load_started_us = now;
            a->load_observed_late = strcmp(previous, "unloaded") && strcmp(previous, "sleeping") && strcmp(previous, "failed");
        }
        a->load_ready_us = 0;
        ch_clear_pointer(&a->load_error, free);
        forget_device(a);
    }
    if (!strcmp(state, "loaded") && strcmp(previous, "loaded")) {
        a->model_idle_since = now;
        a->load_requested_us = 0;
        if (a->load_started_us) {
            a->last_load_seconds = (double)(now - a->load_started_us) / 1e6;
            a->load_ready_us = now;
            /* Load times are stored as desktop.ini keys, which cannot hold every model name. */
            size_t n = strlen(model);
            if (!a->load_observed_late && !strpbrk(model, "[]=\r\n") && n && model[0] != ' ' && model[n - 1] != ' ') {
                double *seconds = ch_new0(double, 1);
                *seconds = a->last_load_seconds;
                ch_map_set(&a->load_seconds, model, seconds);
                prefs_save(a);
            }
        }
        a->load_started_us = 0;
        a->last_device_poll = 0;
    }
    if (!strcmp(state, "failed") || (!strcmp(state, "unloaded") && !strcmp(previous, "loading"))) {
        if (a->load_started_us && !a->unload_requested) {
            free(a->load_error);
            a->load_error = ch_strdup(a->model_status_error ? a->model_status_error
                                                            : "The model stopped while loading. Open Diagnostics for the server log.");
        }
        a->load_started_us = a->load_requested_us = 0;
    }
    if (strcmp(state, "loaded") && strcmp(state, "loading"))
        a->unload_requested = !strcmp(state, "offline") && a->unload_requested;
    /* Outside a request, the monitor only carried the connection message; the chip has taken over. */
    if (strcmp(state, "offline") && !a->busy && !a->request_started_us && a->status &&
        !strcmp(a->status, "Connecting to inference computer…"))
        a->status_visible = false;
    char *next = ch_strdup(state);
    free(a->model_state);
    a->model_state = next;
    free(previous);
    cu_invalidate(a->cu);
}

typedef struct {
    App *app;
    ChConfig config;
    ChCancel *cancel;
    int kind;
    json_object *value;
    ChError *error;
} ModelJob;
enum { MODEL_POLL, MODEL_DEVICE, MODEL_LOAD, MODEL_UNLOAD };
static void model_job_work(void *data) {
    ModelJob *job = data;
    if (job->kind == MODEL_POLL)
        job->value = ch_models(&job->config, job->cancel, &job->error);
    else if (job->kind == MODEL_DEVICE)
        job->value = ch_inferssd_device_status(&job->config, job->cancel, &job->error);
    else if (ch_model_control(&job->config, job->kind == MODEL_LOAD, job->cancel, &job->error))
        job->value = json_object_new_boolean(true);
}
static void model_job_done(void *data) {
    ModelJob *job = data;
    App *a = job->app;
    bool current = !a->closing && !strcmp(job->config.model, a->config.model) && !ch_cancel_is_cancelled(job->cancel);
    if (job->kind == MODEL_POLL) {
        a->model_poll_pending = false;
        if (current)
            model_state_apply(a, job->config.model, job->value, job->error);
    } else if (job->kind == MODEL_DEVICE) {
        a->device_poll_pending = false;
        if (current && job->value) {
            if (a->device_status)
                json_object_put(a->device_status);
            a->device_status = json_object_get(job->value);
        }
        if (current) {
            free(a->device_error);
            a->device_error = job->error ? ch_strdup(job->error->message) : NULL;
        }
    } else {
        a->model_control_pending = false;
        if (current && job->error) {
            if (job->kind == MODEL_LOAD) {
                a->load_started_us = a->load_requested_us = 0;
                free(a->load_error);
                a->load_error = ch_strdup(job->error->message);
            } else {
                a->unload_requested = false;
                free(a->model_status_error);
                a->model_status_error = ch_strdup(job->error->message);
            }
        }
        a->last_model_poll = 0;
    }
    if (job->value)
        json_object_put(job->value);
    ch_clear_error(&job->error);
    ch_cancel_unref(job->cancel);
    free(job);
    cu_invalidate(a->cu);
}
static void model_job(App *a, int kind) {
    ModelJob *job = ch_new0(ModelJob, 1);
    job->app = a;
    job->config = a->config;
    job->kind = kind;
    if (kind == MODEL_POLL) {
        /* Inspect the current API, without waiting for/restarting an SSH tunnel. */
        job->config.ssh_enabled = false;
        job->config.timeout = 4;
        a->model_poll_pending = true;
        a->last_model_poll = ch_monotonic_us();
    } else if (kind == MODEL_DEVICE) {
        a->device_poll_pending = true;
        a->last_device_poll = ch_monotonic_us();
    } else {
        job->config.timeout = CH_MAX(job->config.timeout, 60);
        a->model_control_pending = true;
    }
    if (!a->model_cancel)
        a->model_cancel = ch_cancel_new();
    job->cancel = ch_cancel_ref(a->model_cancel);
    app_run(a, model_job_work, model_job_done, job);
}
void model_load(App *a, bool automatic) {
    if (a->model_control_pending || a->busy || !model_control_supported(a) || !ensure_model_tunnel(a))
        return;
    int64_t now = ch_monotonic_us();
    a->load_requested_us = a->load_started_us = now;
    a->load_observed_late = false;
    a->load_ready_us = 0;
    a->unload_requested = false;
    if (automatic)
        a->last_auto_load = now;
    forget_device(a);
    ch_clear_pointer(&a->load_error, free);
    free(a->model_state);
    a->model_state = ch_strdup("loading");
    model_job(a, MODEL_LOAD);
}
void model_unload(App *a, bool user) {
    if (a->model_control_pending || a->busy || !model_control_supported(a))
        return;
    if (user && a->keep_loaded) {
        a->keep_loaded = false;
        prefs_save(a);
    }
    a->unload_requested = true;
    a->load_started_us = a->load_requested_us = 0;
    ch_clear_pointer(&a->load_error, free);
    model_job(a, MODEL_UNLOAD);
}
/* A finished request counts as use for the idle timer. */
void model_used(App *a) {
    a->model_idle_since = ch_monotonic_us();
    a->last_model_poll = a->last_device_poll = 0;
}
void model_selected(App *a) {
    ch_clear_pointer(&a->model_state, free);
    ch_clear_pointer(&a->load_error, free);
    ch_clear_pointer(&a->model_status_error, free);
    forget_device(a);
    a->load_started_us = a->load_requested_us = a->load_ready_us = a->model_idle_since = 0;
    a->last_model_poll = a->last_device_poll = a->last_auto_load = 0;
    a->unload_requested = false;
    free(a->model_status_for);
    a->model_status_for = ch_strdup(a->config.model);
}
void model_tick(App *a) {
    if (a->closing)
        return;
    if (ch_strcmp0(a->model_status_for, a->config.model))
        model_selected(a);
    if (a->config.provider == 5)
        return;
    int64_t now = ch_monotonic_us();
    const char *state = a->model_state ? a->model_state : "";
    bool loading = !strcmp(state, "loading") || a->model_control_pending;
    bool panel = a->menu.kind == MENU_MODEL_PANEL;
    /* During a request, the inference monitor's probe already reports this state. */
    int64_t interval = !strcmp(state, "offline") ? 3 : loading || panel ? 2 : 15;
    if (!a->busy && !a->model_poll_pending && now - a->last_model_poll >= interval * CH_USEC_PER_SEC)
        model_job(a, MODEL_POLL);
    int64_t device_interval = loading || a->busy || panel ? 4 : !strcmp(state, "loaded") ? 30 : 0;
    if (a->config.provider == 6 && a->config.ssh_enabled && a->tunnel && device_interval && !a->device_poll_pending &&
        !a->approval_dialog && now - a->last_device_poll >= device_interval * CH_USEC_PER_SEC)
        model_job(a, MODEL_DEVICE);
    if (!a->busy && !a->model_control_pending && model_control_supported(a)) {
        if (a->keep_loaded && !strcmp(state, "unloaded") && !a->unload_requested &&
            (!a->last_auto_load || now - a->last_auto_load >= 60 * CH_USEC_PER_SEC))
            model_load(a, true);
        else if (!a->keep_loaded && a->idle_unload_seconds > 0 && !strcmp(state, "loaded") && a->model_idle_since &&
                 !a->unload_requested && now - a->model_idle_since >= (int64_t)a->idle_unload_seconds * CH_USEC_PER_SEC)
            model_unload(a, false);
    }
    if (a->load_ready_us && now - a->load_ready_us >= 8 * CH_USEC_PER_SEC)
        a->load_ready_us = 0;
    if (loading || a->load_ready_us || panel)
        cu_invalidate(a->cu);
}

/* Model chip in the composer footer. */
bool model_chip_visible(App *a) {
    return a->config.provider != 5 && (model_control_supported(a) || a->config.provider == 6);
}
static char *chip_text(App *a) {
    const char *state = a->model_state ? a->model_state : "";
    if (!strcmp(state, "loading") && a->load_started_us) {
        char *clock = format_clock(ch_monotonic_us() - a->load_started_us), *text = ch_strdup_printf("Loading %s", clock);
        free(clock);
        return text;
    }
    if (a->unload_requested && !strcmp(state, "loaded"))
        return ch_strdup("Unloading…");
    if (!strcmp(state, "loaded") && *model_backend(a))
        return ch_strdup_printf("Ready · %s", model_backend(a));
    return ch_strdup(a->load_error && strcmp(state, "loaded") ? "Load failed" : model_state_name(state));
}
void model_chip_build(App *a, float content) {
    Cu *cu = a->cu;
    if (!model_chip_visible(a))
        return;
    if (a->busy)
        a->load_ready_us = 0; /* once a request starts, its monitor replaces the confirmation */
    const char *state = a->model_state ? a->model_state : "";
    bool loading = !strcmp(state, "loading");
    char *text = chip_text(a);
    ChBuf tip;
    ch_buf_init(&tip);
    ch_buf_printf(&tip, "%s: %s", a->config.model, text);
    json_object *device = model_device(a);
    if (device)
        ch_buf_printf(&tip, "\n%s", ch_json_str(device, "summary"));
    char *memory = model_memory_text(a);
    if (memory)
        ch_buf_printf(&tip, "\n%s", memory);
    if (a->last_load_seconds > 0 && !strcmp(state, "loaded"))
        ch_buf_printf(&tip, "\nLoaded in %.1fs", a->last_load_seconds);
    if (a->model_status_error || a->load_error)
        ch_buf_printf(&tip, "\n%s", a->load_error ? a->load_error : a->model_status_error);
    ch_buf_append(&tip, "\nModel status (Ctrl+Shift+S)");
    CuId id = cu_id(cu, "model-chip");
    CuColor dot = !strcmp(state, "loaded") ? C_OK : !strcmp(state, "failed") || a->load_error ? C_DANGER : 0xff8a8a8a;
    cu_open(cu, id, &(CuBox){.row = true, .gap = 5, .align_y = CU_CENTER, CU_PAD2(3, 6), .radius = 5,
                             .bg = cu_hovered(cu, id) ? C_BUTTON : 0, .interactive = true,
                             .tooltip = cu_frame_printf(cu, "%s", tip.str)});
    if (loading)
        ui_spinner(a, 10);
    else
        cu_text(cu, 0, "●", &(CuTextStyle){.font = ui_font(a, 9, 400), .color = dot});
    if (content >= 560)
        cu_text(cu, 0, text, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = !strcmp(state, "failed") || a->load_error ? C_DANGER : 0xffb5b5b5});
    cu_close(cu);
    if (cu_clicked(cu, id))
        menu_open(a, MENU_MODEL_PANEL, id, true);
    ch_buf_clear(&tip);
    free(memory);
    free(text);
}

/* The loading card is the "model is getting ready" screen; a running request shows its own monitor. */
void model_card_build(App *a) {
    Cu *cu = a->cu;
    const char *state = a->model_state ? a->model_state : "";
    bool loading = !strcmp(state, "loading");
    bool failed = a->load_error && strcmp(state, "loaded") && strcmp(state, "loading");
    if (!model_chip_visible(a) || a->busy || !((loading && a->load_started_us) || a->load_ready_us || failed))
        return;
    cu_open(cu, cu_id(cu, "load-card"), &(CuBox){.w = CU_GROW, .bg = 0xff232323, .border = 1,
                                                  .border_color = failed ? 0xffba705c : 0xff3a3a3a, .radius = 9,
                                                  CU_PAD2(7, 10), .gap = 5});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER, .w = CU_GROW});
    if (loading)
        ui_spinner(a, 14);
    char *title = loading ? ch_strdup_printf("Loading %s…", a->config.model)
                : failed  ? ch_strdup_printf("%s failed to load", a->config.model)
                          : ch_strdup_printf("%s is ready", a->config.model);
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = failed ? 0xffefa68e : 0xffe1e1e1,
                                         .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
    free(title);
    char *clock = loading ? format_clock(ch_monotonic_us() - a->load_started_us)
                : a->load_ready_us ? ch_strdup_printf("Ready in %.1fs", a->last_load_seconds) : ch_strdup("");
    ui_label(a, clock, 11, 0xffaaaaaa);
    free(clock);
    if (failed && ui_flat_button(a, cu_id(cu, "load-diagnostics"), NULL, "Diagnostics", NULL, 11, C_TEXT, NULL, true, 0))
        open_diagnostics(a);
    if (ui_flat_button(a, cu_id(cu, "load-action"), NULL, loading ? "Cancel" : failed ? "Retry" : "Dismiss", NULL, 11,
                       C_TEXT, NULL, true, 0)) {
        if (loading)
            model_unload(a, true);
        else if (a->load_ready_us)
            a->load_ready_us = 0;
        else
            model_load(a, false);
    }
    if (failed && ui_icon_button_sized(a, cu_id(cu, "load-dismiss"), "window-close", 14, 2, "Dismiss", true, false)) {
        ch_clear_pointer(&a->load_error, free);
        a->load_ready_us = 0;
    }
    cu_close(cu);
    double fraction = -1;
    /* Retry and Dismiss above clear the error during this frame. */
    char *detail = loading ? model_eta_text(a, &fraction) : ch_strdup(failed && a->load_error ? a->load_error : "");
    json_object *device = model_device(a);
    if (!failed && device && *ch_json_str(device, "summary")) {
        char *joined = ch_strdup_printf("%s%s%s", detail, *detail ? " · " : "", ch_json_str(device, "summary"));
        free(detail);
        detail = joined;
    }
    if (loading)
        ui_progress(a, cu_id(cu, "load-progress"), fraction, 0);
    if (*detail)
        cu_text(cu, 0, detail, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xffaaaaaa, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    free(detail);
    cu_close(cu);
    if (loading)
        cu_animate(cu, 500);
}

/* Model status popover, anchored to the chip. */
static void panel_row(App *a, const char *name, const char *value) {
    Cu *cu = a->cu;
    if (!value || !*value)
        return;
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 12, .w = CU_GROW});
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .w = CU_PX(64)});
    cu_text(cu, 0, value, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = 0xffe1e1e1, .wrap = CU_WRAP_WORD,
                                         .w = CU_GROW_MAX(380), .selectable = true});
    cu_close(cu);
}
void model_panel_build(App *a) {
    Cu *cu = a->cu;
    CuId layer = cu_id(cu, "model-panel");
    cu_layer_open(cu, layer,
                  &(CuLayer){.kind = CU_LAYER_POPUP, .anchor = a->menu.anchor, .placement = CU_PLACE_ABOVE, .align_end = true, .offset = 6},
                  &(CuBox){.w = CU_PX(470), CU_PAD2(14, 16), .gap = 8, .bg = C_MENU, .border = 1,
                           .border_color = C_MENU_BORDER, .radius = 12, .shadow = 16});
    const char *state = a->model_state ? a->model_state : "";
    ui_label(a, "Model status", 12, C_MUTED);
    cu_text(cu, 0, a->config.model, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
    ChBuf line;
    ch_buf_init(&line);
    ch_buf_append(&line, model_state_name(state));
    if (!strcmp(state, "loading") && a->load_started_us) {
        char *clock = format_clock(ch_monotonic_us() - a->load_started_us), *eta = model_eta_text(a, NULL);
        ch_buf_printf(&line, " · %s · %s", clock, eta);
        free(clock);
        free(eta);
    } else if (!strcmp(state, "loaded") && a->last_load_seconds > 0)
        ch_buf_printf(&line, " · loaded in %.1fs", a->last_load_seconds);
    else if (!strcmp(state, "unloaded"))
        ch_buf_append(&line, a->keep_loaded ? " · loading it to keep it ready" : " · loads when you send a message");
    else if (!strcmp(state, "sleeping"))
        ch_buf_append(&line, " · the server unloaded it while idle; it wakes when you send");
    if (a->load_error && strcmp(state, "loaded"))
        ch_buf_printf(&line, "\n%s", a->load_error);
    else if (a->model_status_error)
        ch_buf_printf(&line, "\n%s", a->model_status_error);
    ui_label_wrap(a, line.str, 12, C_TEXT2, 0);
    ch_buf_clear(&line);
    json_object *device = model_device(a);
    bool managed = a->config.provider == 6 && a->config.ssh_enabled;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .gap = 6, .pad_t = 2, .pad_b = 2});
    panel_row(a, "Device", device ? ch_json_str(device, "summary")
                         : managed ? (a->device_error ? a->device_error : "Checking the inference computer…")
                                   : "Reported for SSH-managed InferSSD libraries. This server's API does not say which device it uses.");
    char *memory = model_memory_text(a), *live = model_live_text(a, false);
    panel_row(a, "Memory", memory ? memory : device && !strcmp(state, "loaded") ? "The current load log did not report buffer sizes." : NULL);
    panel_row(a, "Now", live);
    free(memory);
    free(live);
    cu_close(cu);
    ui_separator(a, 2);
    bool controllable = model_control_supported(a);
    CuId keep_row = cu_id(cu, "panel-keep-row");
    cu_open(cu, keep_row, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .interactive = controllable});
    cu_text(cu, 0, "Keep model loaded", &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .w = CU_GROW});
    bool keep = a->keep_loaded;
    bool toggled = ui_switch(a, cu_id(cu, "panel-keep"), &keep, controllable);
    cu_close(cu);
    if (!toggled && controllable && cu_clicked(cu, keep_row)) { /* the label toggles too */
        keep = !keep;
        toggled = true;
    }
    if (toggled) {
        a->keep_loaded = keep;
        prefs_save(a);
        a->model_idle_since = ch_monotonic_us();
        a->last_auto_load = 0;
        if (keep && (!strcmp(state, "unloaded") || !strcmp(state, "failed")))
            model_load(a, false);
    }
    int idle_time = a->idle_unload_seconds;
    char *minutes = !idle_time ? NULL : idle_time < 60 ? ch_strdup_printf("%ds", idle_time)
                  : idle_time % 3600 == 0 ? ch_strdup_printf("%d hour%s", idle_time / 3600, idle_time == 3600 ? "" : "s")
                                          : ch_strdup_printf("%d min", idle_time / 60);
    char *keep_text = a->keep_loaded ? ch_strdup("Loads now and stays in memory while Chela is open. Reloads it if the server unloads it.")
                    : idle_time ? ch_strdup_printf("Loads when you send a message. Unloads after %s idle to free GPU and RAM.", minutes)
                                : ch_strdup("Loads when you send a message and stays loaded until you unload it or pick another model.");
    ui_label_wrap(a, keep_text, 12, C_MUTED, 0);
    free(keep_text);
    free(minutes);
    if (!a->keep_loaded) {
        cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .gap = 12});
        cu_text(cu, 0, "Unload when idle", &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .w = CU_GROW});
        const char *idle[] = {"After 5 min", "After 15 min", "After 30 min", "After 1 hour", "Never"};
        const int values[] = {300, 900, 1800, 3600, 0};
        int index = 1;
        for (int i = 0; i < 5; i++)
            if (values[i] == a->idle_unload_seconds)
                index = i;
        if (ui_dropdown(a, cu_id(cu, "panel-idle"), idle, 5, &index, true, false)) {
            a->idle_unload_seconds = values[index];
            a->model_idle_since = ch_monotonic_us();
            prefs_save(a);
        }
        cu_close(cu);
    }
    if (!model_control_supported(a))
        ui_label_wrap(a, "Loading controls need a llama.cpp/InferSSD model router. This server loads models on its own.", 12, C_MUTED, 0);
    bool control = model_control_supported(a) && !a->busy && !a->model_control_pending;
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 6, .pad_t = 4, .align_y = CU_CENTER});
    if (ui_button(a, cu_id(cu, "panel-load"), "Load now",
                  control && strcmp(state, "loaded") && strcmp(state, "loading") && strcmp(state, "offline"), false))
        model_load(a, false);
    if (ui_button(a, cu_id(cu, "panel-unload"), !strcmp(state, "loading") ? "Cancel load" : "Unload",
                  control && (!strcmp(state, "loaded") || !strcmp(state, "loading")), false))
        model_unload(a, true);
    cu_spacer(cu, CU_GROW, CU_PX(1));
    if (ui_flat_button(a, cu_id(cu, "panel-diagnostics"), NULL, "Diagnostics", NULL, 13, C_TEXT, NULL, true, 0)) {
        menu_close(a);
        open_diagnostics(a);
    }
    cu_close(cu);
    cu_layer_close(cu);
    if (a->menu.kind == MENU_MODEL_PANEL && cu_layer_dismissed(cu, layer))
        menu_close(a);
}
