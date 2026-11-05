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
static double map_seconds(ChMap *map, const char *model) {
    double *value = ch_map_get(map, model);
    return value ? *value : 0;
}
static void map_store(ChMap *map, const char *model, double value) {
    double *copy = ch_new0(double, 1);
    *copy = value;
    ch_map_set(map, model, copy);
}
static void load_track_begin(App *a, int64_t now) {
    ch_load_begin(&a->load, now, previous_load_seconds(a), map_seconds(&a->load_tail, a->config.model),
                  map_seconds(&a->load_mapped, a->config.model));
}
char *model_eta_text(App *a, double *fraction) {
    ChLoadEstimate e = ch_load_estimate(&a->load, ch_monotonic_us());
    if (fraction)
        *fraction = e.fraction;
    char *eta = ch_load_eta_text(e.eta);
    ChBuf text;
    ch_buf_init(&text);
    if (e.measured || e.eta >= 0)
        ch_buf_printf(&text, "%.0f%%", e.fraction * 100);
    if (e.stalled)
        ch_buf_printf(&text, "%sNo change for a while", text.len ? " · " : "");
    else if (eta)
        ch_buf_printf(&text, "%s%s", text.len ? " · " : "", eta);
    free(eta);
    return ch_buf_steal(&text);
}
char *model_phase_text(App *a) {
    ChLoadEstimate e = ch_load_estimate(&a->load, ch_monotonic_us());
    return ch_load_phase_text(&a->load, &e);
}
/* The running process's own figure first: the server may cap what it was asked for. */
static int context_limit(App *a) {
    return a->model_running.context ? a->model_running.context
         : a->model_listed.context  ? a->model_listed.context
         : a->config.provider == 6  ? a->config.context_size
                                    : 0;
}
int model_vision(App *a) {
    return a->model_running.vision >= 0 ? a->model_running.vision : a->model_listed.vision;
}
static bool reports_tokens(json_object *stats) {
    return ch_json_get(stats, "prompt_total") || ch_json_get(stats, "input_tokens");
}
/* A running request's numbers as they arrive, otherwise those of the last reply. */
ModelContext model_context(App *a) {
    ModelContext c = {.limit = context_limit(a)};
    json_object *stats = a->busy && reports_tokens(a->metrics) ? a->metrics : NULL;
    for (size_t i = json_object_array_length(a->messages); !stats && i-- > 0;)
        stats = ch_json_get(json_object_array_get_idx(a->messages, i), "_chela_stats");
    if (!stats)
        return c;
    /* prompt_total counts the whole prompt; input_tokens can be only the part not already cached. */
    c.prompt = CH_MAX(json_object_get_int64(ch_json_get(stats, "prompt_total")), json_object_get_int64(ch_json_get(stats, "input_tokens")));
    c.reply = json_object_get_int64(ch_json_get(stats, "output_tokens"));
    c.used = c.prompt + c.reply;
    c.fraction = c.limit ? CH_CLAMP((double)c.used / c.limit, 0, 1) : 0;
    return c;
}
CuColor model_context_color(double fraction) {
    return fraction >= 0.95 ? C_METER_RED : fraction >= 0.8 ? C_METER_YELLOW : C_METER_BLUE;
}
static char *context_line(const ModelContext *c) {
    char *used = ch_format_count(c->used), *limit = ch_format_count(c->limit);
    char *text = c->limit ? ch_strdup_printf("%s of %s tokens · %.0f%% used", used, limit, c->fraction * 100)
                          : ch_strdup_printf("%s tokens", used);
    free(used);
    free(limit);
    return text;
}
/* The bar, and the numbers behind it. */
void model_context_build(App *a) {
    Cu *cu = a->cu;
    ModelContext c = model_context(a);
    if (c.limit) {
        ui_meter(a, 0, c.fraction, model_context_color(c.fraction));
        cu_spacer(cu, CU_PX(1), CU_PX(5));
    }
    if (!c.used) {
        char *room = ch_format_count(c.limit), *text = ch_strdup_printf("%s tokens free", room);
        ui_label(a, c.limit ? text : "Fills as the session grows", 12, C_FAINT);
        free(room);
        free(text);
        return;
    }
    char *line = context_line(&c), *prompt = ch_format_count(c.prompt), *reply = ch_format_count(c.reply);
    char *left = ch_format_count(c.limit - c.used);
    char *parts = c.limit ? ch_strdup_printf("Prompt %s · reply %s · %s left", prompt, reply, left)
                          : ch_strdup_printf("Prompt %s · reply %s", prompt, reply);
    ui_label(a, line, 12, C_TEXT2);
    ui_label(a, parts, 12, C_FAINT);
    if (c.fraction >= 0.8)
        ui_label_wrap(a, "Nearly full. A new session starts with an empty context.", 12, model_context_color(c.fraction), 0);
    free(line);
    free(prompt);
    free(reply);
    free(left);
    free(parts);
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
        a->model_listed = ch_model_info_listed(entry);
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
            load_track_begin(a, now);
        }
        a->load_ready_us = 0;
        ch_clear_pointer(&a->load_error, free);
        forget_device(a);
    }
    /* What a process says about itself ends with the process. */
    if (!strcmp(state, "loaded") != !strcmp(previous, "loaded")) {
        a->model_running = (ChModelInfo){.vision = -1};
        a->props_checked = false;
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
                map_store(&a->load_seconds, model, a->last_load_seconds);
                /* What the bar learns from this load: how much really had to be read, and how long the rest took. */
                double tail = ch_load_tail_seconds(&a->load, now);
                if (tail > 0)
                    map_store(&a->load_tail, model, tail);
                if (a->load.mapped_peak > 0)
                    map_store(&a->load_mapped, model, a->load.mapped_peak);
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
    /* Only seeing the model back ends an idle unload: until then a finished request leaves this state out of date, and
     * pre-load would act on it. The model reporting loaded just after Chela asked to unload it is not it coming back. */
    else if (!a->unload_requested)
        a->idle_unloaded = false;
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
enum { MODEL_POLL, MODEL_DEVICE, MODEL_LOAD, MODEL_UNLOAD, MODEL_PROPS };
static void model_job_work(void *data) {
    ModelJob *job = data;
    if (job->kind == MODEL_POLL)
        job->value = ch_models(&job->config, job->cancel, &job->error);
    else if (job->kind == MODEL_DEVICE)
        job->value = ch_inferssd_device_status(&job->config, job->cancel, &job->error);
    else if (job->kind == MODEL_PROPS)
        job->value = ch_model_props(&job->config, job->cancel, &job->error);
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
            ChLoadSample sample;
            if (a->load_started_us && !ch_strcmp0(a->model_state, "loading") &&
                ch_load_sample_parse(ch_json_get(job->value, "load"), &sample))
                ch_load_sample(&a->load, &sample, ch_monotonic_us());
        }
        if (current) {
            free(a->device_error);
            a->device_error = job->error ? ch_strdup(job->error->message) : NULL;
        }
    } else if (job->kind == MODEL_PROPS) {
        a->props_pending = false;
        /* Not every server has /props; the model list and the settings stand in for it. */
        if (current && job->value)
            a->model_running = ch_model_info_props(job->value);
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
    if (kind == MODEL_POLL || kind == MODEL_PROPS) {
        /* Inspect the current API, without waiting for/restarting an SSH tunnel. */
        job->config.ssh_enabled = false;
        job->config.timeout = 4;
        if (kind == MODEL_PROPS) {
            a->props_pending = a->props_checked = true;
        } else {
            a->model_poll_pending = true;
            a->last_model_poll = ch_monotonic_us();
        }
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
    load_track_begin(a, now);
    a->load_observed_late = false;
    a->load_ready_us = 0;
    a->unload_requested = a->idle_unloaded = false;
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
    /* Asked to free the memory, Chela must not load the model straight back. */
    if (user && a->preload) {
        a->preload = false;
        prefs_save(a);
    }
    a->idle_unloaded = !user;
    a->unload_requested = true;
    a->load_started_us = a->load_requested_us = 0;
    ch_clear_pointer(&a->load_error, free);
    model_job(a, MODEL_UNLOAD);
}
/* One switch for whether the model is held ready: on loads it now, off frees the memory now. */
void model_preload_set(App *a, bool on) {
    const char *state = a->model_state ? a->model_state : "";
    a->preload = on;
    a->model_idle_since = ch_monotonic_us();
    a->last_auto_load = 0;
    prefs_save(a);
    if (on && (!strcmp(state, "unloaded") || !strcmp(state, "failed") || !strcmp(state, "sleeping")))
        model_load(a, false);
    else if (!on && (!strcmp(state, "loaded") || !strcmp(state, "loading")))
        model_unload(a, true);
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
    a->unload_requested = a->idle_unloaded = a->props_checked = false;
    a->model_listed = a->model_running = (ChModelInfo){.vision = -1};
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
    bool details = now - a->details_seen_us < CH_USEC_PER_SEC;
    /* During a request, the inference monitor's probe already reports this state. */
    int64_t interval = !strcmp(state, "offline") ? 3 : loading || details ? 2 : 15;
    if (!a->busy && !a->model_poll_pending && now - a->last_model_poll >= interval * CH_USEC_PER_SEC)
        model_job(a, MODEL_POLL);
    /* While loading, these reports are what moves the bar. */
    int64_t device_interval = loading ? 2 : a->busy || details ? 4 : !strcmp(state, "loaded") ? 30 : 0;
    if (a->config.provider == 6 && a->config.ssh_enabled && a->tunnel && device_interval && !a->device_poll_pending &&
        !a->approval_dialog && now - a->last_device_poll >= device_interval * CH_USEC_PER_SEC)
        model_job(a, MODEL_DEVICE);
    /* What the running model really has. llama.cpp servers answer this, loaded or with no router to ask. */
    if ((a->config.provider == 0 || a->config.provider == 6) && !a->busy && !a->props_pending && !a->props_checked &&
        a->model_state && (!strcmp(state, "loaded") || !*state))
        model_job(a, MODEL_PROPS);
    /* Loading and unloading are separate choices. An idle unload stays unloaded until the model is next used. */
    if (!a->busy && !a->model_control_pending && model_control_supported(a)) {
        if (a->preload && !strcmp(state, "unloaded") && !a->unload_requested && !a->idle_unloaded &&
            (!a->last_auto_load || now - a->last_auto_load >= 60 * CH_USEC_PER_SEC))
            model_load(a, true);
        else if (a->idle_unload_seconds > 0 && !strcmp(state, "loaded") && a->model_idle_since && !a->unload_requested &&
                 now - a->model_idle_since >= (int64_t)a->idle_unload_seconds * CH_USEC_PER_SEC)
            model_unload(a, false);
    }
    if (a->load_ready_us && now - a->load_ready_us >= 8 * CH_USEC_PER_SEC)
        a->load_ready_us = 0;
    if (loading || a->load_ready_us || details)
        cu_invalidate(a->cu);
}

/* The model's status, as the composer footer's ring reports it in a tooltip. */
bool model_chip_visible(App *a) {
    return a->config.provider != 5 && (model_control_supported(a) || a->config.provider == 6);
}
char *model_status_summary(App *a) {
    const char *state = a->model_state ? a->model_state : "";
    ChBuf text;
    ch_buf_init(&text);
    if (!strcmp(state, "loading") && a->load_started_us) {
        char *clock = format_clock(ch_monotonic_us() - a->load_started_us), *eta = model_eta_text(a, NULL);
        ch_buf_printf(&text, "Loading %s%s%s", clock, *eta ? " · " : "", eta);
        free(clock);
        free(eta);
    } else if (a->unload_requested && !strcmp(state, "loaded"))
        ch_buf_append(&text, "Unloading…");
    else if (!strcmp(state, "loaded") && *model_backend(a))
        ch_buf_printf(&text, "Ready · %s", model_backend(a));
    else
        ch_buf_append(&text, a->load_error && strcmp(state, "loaded") ? "Load failed" : model_state_name(state));
    json_object *device = model_device(a);
    if (device && *ch_json_str(device, "summary"))
        ch_buf_printf(&text, "\n%s", ch_json_str(device, "summary"));
    char *memory = model_memory_text(a);
    if (memory)
        ch_buf_printf(&text, "\n%s", memory);
    free(memory);
    if (a->last_load_seconds > 0 && !strcmp(state, "loaded"))
        ch_buf_printf(&text, "\nLoaded in %.1fs", a->last_load_seconds);
    ModelContext context = model_context(a);
    if (context.used) {
        char *line = context_line(&context);
        ch_buf_printf(&text, "\nContext: %s", line);
        free(line);
    }
    if (a->model_status_error || a->load_error)
        ch_buf_printf(&text, "\n%s", a->load_error ? a->load_error : a->model_status_error);
    return ch_buf_steal(&text);
}

/* Where a load has got to: a real bar, the percentage and the time left. */
void model_progress_build(App *a) {
    Cu *cu = a->cu;
    double fraction = 0;
    char *eta = model_eta_text(a, &fraction), *phase = model_phase_text(a);
    ui_progress(a, cu_id(cu, "load-progress"), fraction, 0);
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 10, .align_y = CU_CENTER});
    cu_text(cu, 0, phase, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xffaaaaaa, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    if (*eta)
        cu_text(cu, 0, eta, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xffd0d0d0});
    cu_close(cu);
    free(eta);
    free(phase);
    cu_animate(cu, 100);
}
/* The card while a model gets ready; a running request shows the same bar in its monitor. */
void model_card_build(App *a) {
    Cu *cu = a->cu;
    const char *state = a->model_state ? a->model_state : "";
    bool loading = !strcmp(state, "loading");
    bool failed = a->load_error && strcmp(state, "loaded") && strcmp(state, "loading");
    if (!model_chip_visible(a) || a->busy || !((loading && a->load_started_us) || failed))
        return;
    cu_open(cu, cu_id(cu, "load-card"), &(CuBox){.w = CU_GROW, .bg = 0xff232323, .border = 1,
                                                  .border_color = failed ? 0xffba705c : 0xff3a3a3a, .radius = 10,
                                                  CU_PAD2(8, 12), .gap = 6});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER, .w = CU_GROW});
    char *name = ch_model_family_name(a->config.model);
    char *title = loading ? ch_strdup_printf("Loading %s", name) : ch_strdup_printf("%s failed to load", name);
    free(name);
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = failed ? 0xffefa68e : 0xffe1e1e1,
                                         .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
    free(title);
    if (failed && ui_flat_button(a, cu_id(cu, "load-diagnostics"), NULL, "Diagnostics", NULL, 12, C_TEXT2, NULL, true, 0))
        open_diagnostics(a);
    if (ui_flat_button(a, cu_id(cu, "load-action"), NULL, loading ? "Cancel" : "Retry", NULL, 12, C_TEXT, NULL, true, 0)) {
        if (loading)
            model_unload(a, true);
        else
            model_load(a, false);
    }
    if (failed && ui_icon_button_sized(a, cu_id(cu, "load-dismiss"), "window-close", 14, 2, "Dismiss", true, false))
        ch_clear_pointer(&a->load_error, free);
    cu_close(cu);
    if (loading && !strcmp(a->model_state ? a->model_state : "", "loading"))
        model_progress_build(a);
    else if (a->load_error)
        cu_text(cu, 0, a->load_error, &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xffaaaaaa, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    cu_close(cu);
}

static void status_text(App *a, ChBuf *line) {
    const char *state = a->model_state ? a->model_state : "";
    ch_buf_append(line, model_state_name(state));
    if (!strcmp(state, "loading") && a->load_started_us) {
        char *clock = format_clock(ch_monotonic_us() - a->load_started_us), *eta = model_eta_text(a, NULL), *phase = model_phase_text(a);
        ch_buf_printf(line, " · %s%s%s\n%s", clock, *eta ? " · " : "", eta, phase);
        free(clock);
        free(eta);
        free(phase);
    } else if (!strcmp(state, "loaded") && a->last_load_seconds > 0)
        ch_buf_printf(line, " · loaded in %.1fs", a->last_load_seconds);
    else if (!strcmp(state, "unloaded"))
        ch_buf_append(line, a->preload && !a->idle_unloaded ? " · pre-loading it" : " · loads when you send a message");
    else if (!strcmp(state, "sleeping"))
        ch_buf_append(line, " · the server unloaded it while idle; it wakes when you send");
    if (a->load_error && strcmp(state, "loaded"))
        ch_buf_printf(line, "\n%s", a->load_error);
    else if (a->model_status_error)
        ch_buf_printf(line, "\n%s", a->model_status_error);
}
bool model_preload_enabled(App *a) {
    return !a->busy && !a->model_control_pending;
}
const char *model_preload_hint(App *a) {
    return a->preload ? "Ready before your first message. Turning this off frees GPU and RAM now."
                      : "Loads when you send a message. Turning this on loads it now.";
}
/* The pre-load switch in Settings; the model menu has the same one. The label toggles too. */
void model_preload_row(App *a) {
    Cu *cu = a->cu;
    if (!model_control_supported(a))
        return;
    CuId row = cu_id(cu, "preload-row");
    bool enabled = model_preload_enabled(a), on = a->preload;
    cu_open(cu, row, &(CuBox){.row = true, .w = CU_GROW, .h = CU_FIT_MIN(52), .align_y = CU_CENTER, .gap = 16,
                              .border_b = 1, .border_color = 0xff2e2e2e, CU_PAD2(10, 0), .interactive = enabled});
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .gap = 3});
    cu_text(cu, 0, "Pre-load model", &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT});
    ui_label_wrap(a, model_preload_hint(a), 12, C_MUTED, 460);
    cu_close(cu);
    bool toggled = ui_switch(a, cu_id(cu, "preload"), &on, enabled);
    cu_close(cu);
    if (!toggled && enabled && cu_clicked(cu, row)) {
        on = !on;
        toggled = true;
    }
    if (toggled)
        model_preload_set(a, on);
}

static void detail_row(App *a, const char *name, const char *value) {
    Cu *cu = a->cu;
    if (!value || !*value)
        return;
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 12, .w = CU_GROW});
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .w = CU_PX(64)});
    cu_text(cu, 0, value, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = 0xffe1e1e1, .wrap = CU_WRAP_WORD,
                                         .w = CU_GROW, .selectable = true});
    cu_close(cu);
}
/* The specifics of the current model, in Settings → Models: where it runs and what it uses. */
void model_details_build(App *a) {
    Cu *cu = a->cu;
    a->details_seen_us = ch_monotonic_us(); /* keeps these figures current while they are on screen */
    const char *state = a->model_state ? a->model_state : "";
    bool loaded = !strcmp(state, "loaded"), loading = !strcmp(state, "loading");
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 12, .align_y = CU_CENTER});
    char *name = ch_model_short_name(a->config.model);
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 14, 600), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
    free(name);
    if (a->config.provider != 5 && ui_button(a, cu_id(cu, "details-test"), "Test model", !a->busy, false))
        open_model_test(a);
    if (model_control_supported(a)) {
        bool control = !a->busy && !a->model_control_pending;
        const char *label = loading ? "Cancel load" : loaded ? "Unload now" : "Load now";
        if (ui_button(a, cu_id(cu, "details-load"), label, control && strcmp(state, "offline"), false)) {
            if (loaded || loading)
                model_unload(a, true);
            else
                model_load(a, false);
        }
    }
    cu_close(cu);
    ChBuf line;
    ch_buf_init(&line);
    status_text(a, &line);
    json_object *device = model_device(a);
    bool managed = a->config.provider == 6 && a->config.ssh_enabled;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .gap = 6, .pad_t = 8});
    detail_row(a, "Status", line.str);
    ch_buf_clear(&line);
    detail_row(a, "Device", device ? ch_json_str(device, "summary")
                          : managed ? (a->device_error ? a->device_error : "Checking the inference computer…")
                                    : "Reported for SSH-managed InferSSD libraries. This server's API does not say which device it uses.");
    char *memory = model_memory_text(a), *live = model_live_text(a, false);
    detail_row(a, "Memory", memory ? memory : device && loaded ? "The current load log did not report buffer sizes." : NULL);
    detail_row(a, "Now", live);
    free(memory);
    free(live);
    int limit = context_limit(a), vision = model_vision(a);
    char *room = ch_format_count(limit), *context = ch_strconcat(room, " tokens", NULL);
    detail_row(a, "Context", limit ? context : NULL);
    free(room);
    free(context);
    char *refused = ch_strconcat("This model can't read images yet. ", ch_vision_help(&a->config), NULL);
    detail_row(a, "Images", vision > 0 ? "Reads images" : vision == 0 ? refused : NULL);
    free(refused);
    cu_close(cu);
}

/* Context popover, anchored to the ring: the tokens behind its arc, and where to look further. */
void model_panel_build(App *a) {
    Cu *cu = a->cu;
    CuId layer = cu_id(cu, "model-panel");
    cu_layer_open(cu, layer,
                  &(CuLayer){.kind = CU_LAYER_POPUP, .anchor = a->menu.anchor, .placement = CU_PLACE_ABOVE, .align_end = true, .offset = 6},
                  &(CuBox){.w = CU_PX(300), CU_PAD2(14, 16), .gap = 8, .bg = C_MENU, .border = 1,
                           .border_color = C_MENU_BORDER, .radius = 12, .shadow = 16});
    ui_label(a, "Context", 12, C_MUTED);
    cu_open(cu, cu_id(cu, "status-context"), &(CuBox){.w = CU_GROW, .gap = 3}); /* the panel shows one too */
    model_context_build(a);
    cu_close(cu);
    ui_separator(a, 2);
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 6, .align_y = CU_CENTER});
    cu_spacer(cu, CU_GROW, CU_PX(1));
    if (ui_flat_button(a, cu_id(cu, "panel-settings"), NULL, "Settings", NULL, 13, C_TEXT2, "Device, memory, pre-load and idle unload", !a->busy, 0)) {
        menu_close(a);
        open_settings(a, SETTINGS_MODELS);
    }
    if (ui_flat_button(a, cu_id(cu, "panel-diagnostics"), NULL, "Diagnostics", NULL, 13, C_TEXT2, NULL, true, 0)) {
        menu_close(a);
        open_diagnostics(a);
    }
    cu_close(cu);
    cu_layer_close(cu);
    if (a->menu.kind == MENU_MODEL_PANEL && cu_layer_dismissed(cu, layer))
        menu_close(a);
}
