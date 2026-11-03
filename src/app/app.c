/* Application state, actions and the root of the interface. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *const providers[7] = {"llama.cpp", "Ollama", "LM Studio", "vLLM", "OpenAI compatible", "ACP harness",
                                  "InferSSD model library"};

/* Background work */
typedef struct {
    App *app;
    void (*work)(void *);
    void (*done)(void *);
    void *data;
} Task;
static void task_done(void *data) {
    Task *t = data;
    t->app->active_jobs--;
    t->done(t->data);
    free(t);
}
static void task_main(void *data) {
    Task *t = data;
    t->work(t->data);
    t->app->p->post(task_done, t);
}
void app_run(App *a, void (*work)(void *), void (*done)(void *), void *data) {
    Task *t = ch_new0(Task, 1);
    *t = (Task){a, work, done, data};
    a->active_jobs++;
    if (!ch_thread_run(task_main, t)) {
        /* Without threads the work runs here; the interface pauses until it finishes. */
        work(data);
        task_done(t);
    }
}
void app_post(App *a, void (*fn)(void *), void *data) {
    a->p->post(fn, data);
}

/* Status and notices */
void app_set_status(App *a, const char *text) {
    if (a->request_started_us) {
        free(a->run_phase);
        a->run_phase = ch_strdup(text);
    }
    free(a->status);
    a->status = ch_strdup(text);
    a->status_visible = true;
    cu_invalidate(a->cu);
}
static const char *idle_status(App *a) {
    bool chat = app_is_chat(a);
    return !a->config.tools ? "Project tools disabled · conversation and attachments only"
         : chat             ? "Chat · can read project files and directories"
         : a->mode == CH_PLAN ? "Agent · Plan · inspect files without changes"
         : a->mode == CH_AUTO ? "Bypass permissions · edits and commands run without approval"
         : a->mode == CH_ACCEPT_EDITS ? "Accept edits · commands still ask for permission"
                                      : "Agent · Ask · review edits and commands before they run";
}
static void hide_status(App *a) {
    if (!a->busy && !a->request_started_us) {
        free(a->status);
        a->status = ch_strdup(idle_status(a));
        a->status_visible = false;
    }
}
bool app_is_chat(App *a) {
    return !a->code || a->interaction == CH_CHAT;
}
bool app_save_config(App *a, const ChConfig *config, const char *failure_title) {
    ChError *error = NULL;
    if (!ch_config_save(config, a->config_path, &error)) {
        app_notice(a, failure_title, error->message);
        ch_clear_error(&error);
        return false;
    }
    a->config = *config;
    cu_invalidate(a->cu);
    return true;
}

/* Sessions */
void app_save_session(App *a) {
    if (!a->id || !json_object_array_length(a->messages))
        return;
    ChSession v = {.id = a->id, .title = a->session_title, .project = a->project, .model = a->config.model,
                   .interaction = a->interaction, .mode = a->mode, .code = a->code};
    ChError *error = NULL;
    if (!ch_store_save(a->store, &v, a->messages, &error)) {
        app_notice(a, "Could not save session", error->message);
        ch_clear_error(&error);
    } else
        for (size_t i = 0; a->rows && i < a->rows->len; i++) {
            ChSession *row = a->rows->data[i];
            if (!strcmp(row->id, a->id)) {
                row->interaction = v.interaction;
                row->mode = v.mode;
                row->code = v.code;
            }
        }
}
static int64_t local_midnight(void) {
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    t.tm_hour = t.tm_min = t.tm_sec = 0;
    return (int64_t)mktime(&t);
}
static int day_key(int64_t us) {
    time_t s = (time_t)(us / CH_USEC_PER_SEC);
    struct tm t;
    localtime_r(&s, &t);
    return (t.tm_year + 1900) * 400 + t.tm_yday;
}
static int compare_sessions(const void *left, const void *right, void *data) {
    App *a = data;
    const ChSession *x = *(ChSession *const *)left, *y = *(ChSession *const *)right;
    int order = a->group_projects ? ch_utf8_collate(x->project, y->project) : 0;
    if (order)
        return order;
    if (a->group_dates && (order = day_key(y->updated) - day_key(x->updated)))
        return order;
    if (a->sort_titles)
        return ch_utf8_collate(x->title, y->title);
    return x->updated > y->updated ? -1 : x->updated < y->updated;
}
void app_refresh_sessions(App *a) {
    ChError *error = NULL;
    ChVec *rows = ch_store_list(a->store, cu_edit_text(&a->search), &error);
    if (!rows) {
        app_set_status(a, error->message);
        ch_clear_error(&error);
        return;
    }
    ch_vec_free(a->rows);
    a->rows = rows;
    int64_t cutoff = a->activity_days == 1 ? local_midnight() : (int64_t)time(NULL) - a->activity_days * 86400;
    for (size_t i = 0; i < rows->len;) {
        ChSession *s = rows->data[i];
        bool visible = (!a->filter_type || s->code == (a->filter_type == 2)) &&
                       (!a->filter_project || !*a->filter_project || !strcmp(a->filter_project, s->project)) &&
                       (!a->activity_days || s->updated / CH_USEC_PER_SEC >= cutoff);
        if (!visible)
            ch_vec_remove(rows, i);
        else
            i++;
    }
    if (a->sort_titles || a->group_projects || a->group_dates)
        ch_vec_sort(rows, compare_sessions, a);
    cu_invalidate(a->cu);
}
static void reset_composer(App *a) {
    cu_edit_set(&a->composer, "");
}
void app_start_new(App *a) {
    if (a->busy)
        return;
    monitor_reset(a);
    ch_clear_pointer(&a->id, free);
    ch_clear_pointer(&a->session_title, free);
    json_object_put(a->messages);
    a->messages = json_object_new_array();
    md_cache_clear(a);
    memset(a->feedback, 0, sizeof(a->feedback));
    ch_clear_pointer(&a->stopped_note, free);
    a->recovery_visible = false;
    reset_composer(a);
    app_set_status(a, "Ready when you are");
    hide_status(a);
    a->follow = true;
    cu_focus(a->cu, cu_id(a->cu, "composer"));
    cu_invalidate(a->cu);
}
void app_select_session(App *a, const char *id) {
    if (a->busy || !id)
        return;
    ChSession *v = NULL;
    for (size_t i = 0; a->rows && i < a->rows->len; i++)
        if (!strcmp(((ChSession *)a->rows->data[i])->id, id))
            v = a->rows->data[i];
    ChError *error = NULL;
    json_object *m = ch_store_load(a->store, id, &error);
    if (!m) {
        app_notice(a, "Could not open session", error->message);
        ch_clear_error(&error);
        return;
    }
    monitor_reset(a);
    json_object_put(a->messages);
    a->messages = m;
    md_cache_clear(a);
    memset(a->feedback, 0, sizeof(a->feedback));
    char *next = ch_strdup(id);
    free(a->id);
    a->id = next;
    if (v) {
        free(a->project);
        a->project = ch_strdup(v->project);
        free(a->session_title);
        a->session_title = ch_strdup(v->title);
        a->code = v->code;
        a->code_interaction = v->interaction;
        a->interaction = v->code ? v->interaction : CH_CHAT;
        a->mode = v->mode;
    } else {
        /* Opened from a list the sidebar filters out; read its metadata directly. */
        ChVec *all = ch_store_list(a->store, "", NULL);
        for (size_t i = 0; all && i < all->len; i++) {
            ChSession *s = all->data[i];
            if (strcmp(s->id, id))
                continue;
            free(a->project);
            a->project = ch_strdup(s->project);
            free(a->session_title);
            a->session_title = ch_strdup(s->title);
            a->code = s->code;
            a->code_interaction = s->interaction;
            a->interaction = s->code ? s->interaction : CH_CHAT;
            a->mode = s->mode;
        }
        ch_vec_free(all);
    }
    ch_clear_pointer(&a->stopped_note, free);
    a->recovery_visible = false;
    a->follow = true;
    app_set_status(a, "Session restored");
    hide_status(a);
    cu_focus(a->cu, cu_id(a->cu, "composer"));
}
void app_navigate(App *a, int direction) {
    if (a->busy || !a->rows || !a->rows->len)
        return;
    int current = -1;
    for (size_t i = 0; i < a->rows->len; i++)
        if (!ch_strcmp0(a->id, ((ChSession *)a->rows->data[i])->id))
            current = (int)i;
    int next = current + direction;
    if (next < 0)
        app_start_new(a);
    else if ((size_t)next < a->rows->len)
        app_select_session(a, ((ChSession *)a->rows->data[next])->id);
}
void app_set_project(App *a, const char *path, bool new_session) {
    char *next = ch_strdup(path);
    if (new_session)
        app_start_new(a);
    free(a->project);
    a->project = next;
    if (!new_session) {
        app_save_session(a);
        app_refresh_sessions(a);
    }
    cu_invalidate(a->cu);
}
void app_set_code(App *a, bool code) {
    if (a->busy || a->code == code)
        return;
    a->code = code;
    a->interaction = code ? a->code_interaction : CH_CHAT;
    app_save_session(a);
    hide_status(a);
    cu_invalidate(a->cu);
}
void app_set_interaction(App *a, ChInteraction interaction) {
    if (a->busy)
        return;
    a->interaction = interaction;
    if (a->code)
        a->code_interaction = interaction;
    app_save_session(a);
    hide_status(a);
}
void app_set_mode(App *a, ChMode mode) {
    if (a->busy)
        return;
    a->mode = mode;
    app_save_session(a);
    hide_status(a);
}
void app_toggle_sidebar(App *a) {
    a->sidebar_hidden = !a->sidebar_hidden;
    cu_invalidate(a->cu);
}
void app_focus_search(App *a) {
    a->sidebar_hidden = false;
    a->search_open = true;
    cu_focus(a->cu, cu_id(a->cu, "session-search"));
}
static void delete_confirmed(App *a, void *data) {
    (void)data;
    ChError *e = NULL;
    if (!a->id)
        return;
    if (ch_store_delete(a->store, a->id, &e)) {
        ch_map_remove(&a->pinned, a->id);
        ch_map_remove(&a->session_sections, a->id);
        app_start_new(a);
        app_refresh_sessions(a);
    } else {
        app_notice(a, "Could not delete session", e->message);
        ch_clear_error(&e);
    }
}
void app_delete_session(App *a) {
    if (a->busy || !a->id)
        return;
    dialog_confirm(a, "Delete this session?", a->session_title, "Delete", true, delete_confirmed, NULL);
}
static void renamed(App *a, const char *text, void *data) {
    (void)data;
    char *title = ch_strstrip(ch_strdup(text));
    if (*title && a->id) {
        free(a->session_title);
        a->session_title = title;
        app_save_session(a);
        app_refresh_sessions(a);
    } else
        free(title);
}
void app_rename_session(App *a) {
    if (a->busy || !a->id)
        return;
    dialog_text_input(a, "Rename session", a->session_title, "Session name", "Save", 160, renamed, NULL);
}
void app_toggle_pin(App *a) {
    if (!a->id || a->busy)
        return;
    if (!ch_map_remove(&a->pinned, a->id))
        ch_map_set(&a->pinned, a->id, NULL);
    a->pinned_collapsed = false;
    prefs_save(a);
    app_refresh_sessions(a);
}
void app_move_to_section(App *a, const char *section) {
    if (!a->id || a->busy)
        return;
    if (section)
        ch_map_set(&a->session_sections, a->id, ch_strdup(section));
    else
        ch_map_remove(&a->session_sections, a->id);
    prefs_save(a);
    app_refresh_sessions(a);
}
static void section_created(App *a, const char *text, void *data) {
    char *name = ch_strstrip(ch_strdup(text));
    bool duplicate = false;
    for (size_t i = 0; i < a->sections.len; i++)
        duplicate |= !strcmp(name, a->sections.data[i]);
    if (*name && data) /* from a session's Move to menu */
        app_move_to_section(a, name);
    if (*name && !duplicate) {
        ch_vec_push(&a->sections, name);
        prefs_save(a);
        cu_invalidate(a->cu);
    } else
        free(name);
}
void app_create_section(App *a, bool move_session) {
    dialog_text_input(a, "New section", "", "Section name", "Create", 60, section_created, move_session ? a : NULL);
}

/* Files */
void app_attach_path(App *a, const char *path) {
    char *text = NULL;
    size_t n = 0;
    ChError *e = NULL;
    if (!ch_file_is_regular(path)) {
        app_notice(a, "Cannot attach file", "Choose a UTF-8 text file.");
        return;
    }
    if (!ch_file_get_contents(path, &text, &n, &e)) {
        app_notice(a, "Could not attach file", e->message);
        ch_clear_error(&e);
        return;
    }
    if (n > 256 * 1024)
        app_notice(a, "File too large", "Text attachments are limited to 256 KiB.");
    else if (!ch_utf8_validate(text, (ptrdiff_t)n, NULL) || memchr(text, 0, n))
        app_notice(a, "Cannot attach file", "Choose a UTF-8 text file.");
    else {
        char *name = ch_path_basename(path), *block = ch_strdup_printf("\n\nAttached file: %s\n```\n%s\n```\n", name, text);
        a->composer.caret = a->composer.anchor = a->composer.text.len;
        cu_edit_insert(&a->composer, block);
        app_set_status(a, "Text attached · included in the next model request");
        cu_focus(a->cu, cu_id(a->cu, "composer"));
        free(name);
        free(block);
    }
    free(text);
}
static void attach_chosen(App *a, const char *path, void *data) {
    (void)data;
    if (path)
        app_attach_path(a, path);
}
void app_attach_file(App *a) {
    if (a->busy)
        return;
    dialog_file(a, 0, "Attach a text file", a->project, NULL, attach_chosen, NULL);
}
static void project_chosen(App *a, const char *path, void *data) {
    (void)data;
    if (path && !a->busy)
        app_set_project(a, path, true);
}
void app_choose_project(App *a) {
    if (a->busy)
        return;
    dialog_file(a, 1, "Choose project folder", a->project, NULL, project_chosen, NULL);
}
static void export_chosen(App *a, const char *path, void *data) {
    (void)data;
    if (!path)
        return;
    ChBuf s;
    ch_buf_init(&s);
    ch_buf_printf(&s, "# %s\n\n", a->session_title ? a->session_title : "Chela conversation");
    for (size_t i = 0; i < json_object_array_length(a->messages); i++) {
        json_object *m = json_object_array_get_idx(a->messages, i);
        if (strcmp(ch_json_str(m, "role"), "system"))
            ch_buf_printf(&s, "## %s\n\n%s\n\n", ch_json_str(m, "role"), ch_json_str(m, "content"));
    }
    ChError *e = NULL;
    if (!ch_file_set_contents(path, s.str, (ptrdiff_t)s.len, 0644, &e)) {
        app_notice(a, "Export failed", e->message);
        ch_clear_error(&e);
    } else
        app_set_status(a, "Conversation exported");
    ch_buf_clear(&s);
}
void app_export(App *a) {
    if (a->busy)
        return;
    dialog_file(a, 2, "Export conversation", a->project, "conversation.md", export_chosen, NULL);
}
void app_show_changes(App *a) {
    if (a->busy)
        return;
    ChError *error = NULL;
    char *out = ch_command(a->project, "git --no-pager diff --no-ext-diff --no-textconv -- .; git status --short", 5,
                           NULL, &error);
    dialog_text_view(a, "Project changes", out ? out : error->message, true, 850, 600);
    free(out);
    ch_clear_error(&error);
}
void app_view_all(App *a) {
    open_all_sessions(a);
}
void app_quit(App *a) {
    stop_dictation(a);
    if (a->busy) {
        a->closing = true;
        app_stop(a);
        app_set_status(a, "Stopping agent before closing…");
        return;
    }
    a->closing = true;
    a->p->quit();
}

/* Requests */
static void flush_events(App *a) {
    ch_mutex_lock(&a->event_mutex);
    char *text = ch_strdup(a->incoming.str);
    ch_buf_truncate(&a->incoming, 0);
    char *status = ch_steal(&a->incoming_status);
    json_object *metrics = ch_steal(&a->incoming_metrics);
    ch_mutex_unlock(&a->event_mutex);
    if (metrics) {
        if (a->metrics && json_object_get_int64(ch_json_get(a->metrics, "request_id")) !=
                              json_object_get_int64(ch_json_get(metrics, "request_id"))) {
            /* A new model call: earlier streamed text is now part of the history. */
            ch_buf_truncate(&a->live, 0);
            a->live_active = false;
        }
        if (a->metrics)
            json_object_put(a->metrics);
        a->metrics = metrics;
        cu_invalidate(a->cu);
    }
    if (*text) {
        ch_buf_append(&a->live, text);
        a->live_active = true;
        cu_invalidate(a->cu);
    }
    if (status)
        app_set_status(a, status);
    free(status);
    free(text);
}
static bool flush_timer(void *data) {
    App *a = data;
    if (a->busy) {
        flush_events(a);
        cu_invalidate(a->cu); /* elapsed time and monitor */
    }
    return true;
}
static void worker_emit(ChEvent type, const char *text, void *data) {
    Job *j = data;
    App *a = j->app;
    ch_mutex_lock(&a->event_mutex);
    if (type == CH_TEXT) {
        ch_buf_append(&a->incoming, text);
        a->last_token_us = ch_monotonic_us();
        free(a->incoming_status);
        a->incoming_status = ch_strdup("Generating response…");
    } else if (type == CH_THOUGHT) {
        a->last_token_us = ch_monotonic_us();
        free(a->incoming_status);
        a->incoming_status = ch_strdup("Thinking…");
    } else if (type == CH_STATUS || type == CH_TOOL) {
        free(a->incoming_status);
        a->incoming_status = ch_strdup(type == CH_TOOL ? "Using project tool…" : text);
    } else if (type == CH_ACTIVITY)
        a->last_network_us = ch_monotonic_us();
    else if (type == CH_METRICS) {
        json_object *metrics = ch_json_parse(text, NULL);
        if (metrics) {
            if (a->incoming_metrics)
                json_object_put(a->incoming_metrics);
            a->incoming_metrics = metrics;
        }
    }
    ch_mutex_unlock(&a->event_mutex);
}
static void show_permission(void *data) {
    Job *j = data;
    App *a = j->app;
    if (ch_cancel_is_cancelled(j->run.cancel) || a->closing) {
        ch_mutex_lock(&j->mutex);
        j->allowed = false;
        j->answered = true;
        ch_cond_signal(&j->condition);
        ch_mutex_unlock(&j->mutex);
        return;
    }
    a->approval_dialog = open_approval(a, j);
}
static bool worker_approve(const char *title, const char *detail, void *data) {
    Job *j = data;
    worker_emit(CH_STATUS, "Waiting for your approval…", j);
    ch_mutex_lock(&j->mutex);
    j->answered = j->allowed = false;
    j->permission_title = ch_strdup(title);
    j->permission_detail = ch_strdup(detail);
    j->app->p->post(show_permission, j);
    while (!j->answered)
        ch_cond_wait(&j->condition, &j->mutex);
    bool allowed = j->allowed;
    ch_clear_pointer(&j->permission_title, free);
    ch_clear_pointer(&j->permission_detail, free);
    ch_mutex_unlock(&j->mutex);
    return allowed;
}
static void worker(void *data) {
    Job *j = data;
    ch_agent_run(&j->run, j->messages, &j->error);
}
static void finished(void *data) {
    Job *j = data;
    App *a = j->app;
    flush_events(a);
    close_approval(a);
    json_object_put(a->messages);
    a->messages = j->messages;
    j->messages = NULL;
    a->busy = false;
    ch_buf_truncate(&a->live, 0);
    a->live_active = false;
    md_cache_clear(a);
    app_save_session(a);
    app_refresh_sessions(a);
    if (j->error) {
        bool cancelled = ch_error_is(j->error, CH_ERR_CANCELLED);
        app_set_status(a, j->error->message);
        free(a->stopped_note);
        a->stopped_note = !cancelled && !a->closing ? ch_strdup(j->error->message) : NULL;
        a->recovery_visible = !cancelled;
        size_t count = json_object_array_length(a->messages);
        a->retry_enabled = j->run.config.provider != 5 && count &&
                           !strcmp(ch_json_str(json_object_array_get_idx(a->messages, count - 1), "role"), "user");
    } else
        app_set_status(a, "Done · saved on this computer");
    monitor_finish(a, j->error);
    model_used(a);
    ch_clear_error(&j->error);
    free(j->run.project);
    ch_cancel_unref(j->run.cancel);
    ch_mutex_clear(&j->mutex);
    ch_cond_clear(&j->condition);
    free(j);
    a->job = NULL;
    ch_clear_pointer(&a->cancel, ch_cancel_unref);
    if (a->closing)
        a->p->quit();
    else
        cu_focus(a->cu, cu_id(a->cu, "composer"));
    cu_invalidate(a->cu);
}
void app_stop(App *a) {
    if (!a->busy)
        return;
    ch_cancel_cancel(a->cancel);
    close_approval(a);
    app_set_status(a, "Stopping…");
}
void app_send(App *a, bool retry) {
    if (a->busy) {
        app_stop(a);
        return;
    }
    size_t count = json_object_array_length(a->messages);
    json_object *last = count ? json_object_array_get_idx(a->messages, count - 1) : NULL;
    if (retry && strcmp(ch_json_str(last, "role"), "user"))
        return;
    char *prompt = ch_strstrip(ch_strdup(retry ? ch_json_str(last, "content") : cu_edit_text(&a->composer)));
    if (!*prompt) {
        free(prompt);
        return;
    }
    if (strlen(prompt) > CHELA_LIMIT) {
        app_notice(a, "Message too large", "Keep each prompt below 4 MiB.");
        free(prompt);
        return;
    }
    ChError *error = NULL;
    if (!ch_config_validate(&a->config, &error)) {
        app_notice(a, "Check your connection settings", error->message);
        ch_clear_error(&error);
        free(prompt);
        open_settings(a, 1);
        return;
    }
    if (!ensure_model_tunnel(a) || !ch_file_is_dir(a->project)) {
        if (!ch_file_is_dir(a->project))
            app_notice(a, "Project folder is missing", "Choose an existing project folder before continuing.");
        free(prompt);
        return;
    }
    if (!a->id) {
        a->id = ch_uuid_random();
        a->session_title = ch_title(prompt);
    }
    /* Interaction and permission mode are independent; Chat always has read-only tools. */
    if (count && !strcmp(ch_json_str(json_object_array_get_idx(a->messages, 0), "role"), "system"))
        json_object_array_del_idx(a->messages, 0, 1);
    ChRun policy = {.config = a->config, .project = a->project, .mode = a->mode,
                    .interaction = a->code ? a->interaction : CH_CHAT};
    json_object *history = json_object_new_array();
    char *system = ch_system_prompt(&policy);
    json_object_array_add(history, ch_message("system", system));
    free(system);
    for (size_t i = 0; i < json_object_array_length(a->messages); i++)
        json_object_array_add(history, json_object_get(json_object_array_get_idx(a->messages, i)));
    json_object_put(a->messages);
    a->messages = history;
    if (!retry) {
        json_object_array_add(a->messages, ch_message("user", prompt));
        reset_composer(a);
    }
    free(prompt);
    md_cache_clear(a);
    ch_clear_pointer(&a->stopped_note, free);
    a->recovery_visible = false;
    app_save_session(a);
    app_refresh_sessions(a);
    ch_buf_truncate(&a->live, 0);
    a->live_active = false;
    a->cancel = ch_cancel_new();
    Job *j = ch_new0(Job, 1);
    j->app = a;
    j->run = (ChRun){.config = a->config, .project = ch_strdup(a->project), .mode = a->mode,
                     .interaction = policy.interaction, .cancel = ch_cancel_ref(a->cancel), .emit = worker_emit,
                     .approve = worker_approve, .data = j};
    json_object_deep_copy(a->messages, &j->messages, NULL);
    ch_mutex_init(&j->mutex);
    ch_cond_init(&j->condition);
    a->job = j;
    a->busy = true;
    a->follow = true;
    monitor_start(a);
    app_set_status(a, "Connecting…");
    app_run(a, worker, finished, j);
}
void app_regenerate(App *a, size_t index) {
    size_t count = json_object_array_length(a->messages);
    if (a->busy || a->config.provider == 5 || !count || index != count - 1 || index < 1)
        return;
    json_object *last = json_object_array_get_idx(a->messages, index);
    if (strcmp(ch_json_str(last, "role"), "assistant") || ch_json_get(last, "tool_calls") ||
        strcmp(ch_json_str(json_object_array_get_idx(a->messages, index - 1), "role"), "user"))
        return;
    /* Only a direct text answer can be regenerated without replaying tools. */
    json_object_array_del_idx(a->messages, index, 1);
    md_cache_clear(a);
    app_send(a, true);
}

/* Shortcuts, checked after the interface has taken its keys. */
static void shortcuts(App *a) {
    Cu *cu = a->cu;
    const int C = CU_MOD_CTRL, CS = CU_MOD_CTRL | CU_MOD_SHIFT;
    if (cu_key(cu, CU_KEY_ESCAPE, 0) && a->busy) {
        app_stop(a);
        return;
    }
    if (cu_modal_active(cu))
        return;
    if (cu_key(cu, 'd', CS))
        open_diagnostics(a);
    else if (cu_key(cu, 's', CS)) {
        if (model_chip_visible(a))
            menu_open(a, MENU_MODEL_PANEL, cu_id(cu, "model-chip"), true);
    } else if (cu_key(cu, 'l', CS) && !a->busy)
        menu_open(a, MENU_MODEL, cu_id(cu, "model-button"), true);
    else if (cu_key(cu, 'p', CS) && !a->busy)
        menu_open(a, MENU_PROJECT, cu_id(cu, a->code ? "project-button" : "attach-button"), false);
    else if (cu_key(cu, 'a', CS) && !a->busy)
        menu_open(a, MENU_ATTACH, cu_id(cu, "attach-button"), false);
    else if (cu_key(cu, 'o', CS) && !a->busy && a->id)
        menu_open(a, MENU_SESSION, cu_id(cu, "session-button"), false);
    else if (cu_key(cu, 'f', CS) && !a->busy)
        menu_open(a, a->code ? MENU_ORGANIZER : MENU_FILTER, cu_id(cu, "filter-button"), false);
    else if (cu_key(cu, 'u', CS) && !a->busy)
        menu_open(a, MENU_PROFILE, cu_id(cu, "profile-button"), false);
    else if (cu_key(cu, 'e', CS) && !a->busy && a->code)
        menu_open(a, MENU_ENVIRONMENT, cu_id(cu, "environment-button"), false);
    else if (cu_key(cu, 'v', CS) && !a->busy)
        menu_open(a, MENU_VOICE, cu_id(cu, "voice-menu-button"), true);
    else if (cu_key(cu, 't', CS) && !a->busy)
        menu_open(a, MENU_THINKING, cu_id(cu, "thinking-button"), false);
    else if (cu_key(cu, 'm', CS)) {
        if (!a->busy && a->code)
            app_set_interaction(a, a->interaction == CH_AGENT ? CH_CHAT : CH_AGENT);
    } else if (cu_key(cu, 'm', C)) {
        if (a->code && !a->busy && a->interaction == CH_AGENT)
            menu_open(a, MENU_MODE, cu_id(cu, "mode-button"), false);
    } else if (cu_key(cu, '1', C) || cu_key(cu, '2', C)) {
        /* handled below */
    } else if (cu_key(cu, 'q', C))
        app_quit(a);
    else if (cu_key(cu, 'n', C))
        app_start_new(a);
    else if (cu_key(cu, 'd', C) && !a->busy)
        dictate(a);
    else if (cu_key(cu, 'u', C) && !a->busy)
        app_attach_file(a);
    else if (cu_key(cu, 'k', C))
        app_focus_search(a);
    else if (cu_key(cu, ',', C))
        open_settings(a, 0);
    else if (cu_key(cu, 'b', C))
        app_toggle_sidebar(a);
    else if (cu_key(cu, CU_KEY_RETURN, C))
        app_send(a, false);
    else if (cu_key(cu, 'c', C)) {
        const char *selection = cu_selection_text(cu);
        if (selection)
            cu_copy(cu, selection);
    }
}
static void tab_shortcuts(App *a) {
    /* Ctrl+1 / Ctrl+2 switch between Chat and Code. */
    Cu *cu = a->cu;
    for (int i = 0; i < 2; i++)
        if (cu_key(cu, '1' + i, CU_MOD_CTRL) && !a->busy) {
            app_set_code(a, i == 1);
            cu_focus(cu, cu_id(cu, "composer"));
        }
}

/* Layout */
static void build(Cu *cu, void *data) {
    App *a = data;
    if (cu_close_requested(cu))
        app_quit(a);
    menu_keys(a);
    cu_open(cu, cu_id(cu, "window"), &(CuBox){.row = true, .w = CU_GROW, .h = CU_GROW, .bg = C_BG});
    if (!a->sidebar_hidden)
        sidebar_build(a);
    cu_open(cu, cu_id(cu, "main"), &(CuBox){.w = CU_GROW, .h = CU_GROW});
    topbar_build(a);
    bool conversation = json_object_array_length(a->messages) > 0 || a->live_active;
    if (conversation)
        chat_build(a);
    else
        welcome_build(a);
    composer_build(a);
    cu_close(cu);
    cu_close(cu);
    menu_build(a);
    dialogs_build(a);
    tab_shortcuts(a);
    shortcuts(a);
    if (a->closing && !a->busy && !a->active_jobs)
        a->p->quit();
}
static bool tick(void *data) {
    App *a = data;
    if (!a->closing)
        model_tick(a);
    return true;
}
void app_install(App *a);
void app_install(App *a) {
    cu_set_build(a->cu, build, a);
    a->tick_timer = a->p->timer_add(1000, tick, a);
    a->flush_timer = a->p->timer_add(33, flush_timer, a);
    hide_status(a);
}
