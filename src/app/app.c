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
    a->status_until = a->busy ? 0 : ch_monotonic_us() + 5 * CH_USEC_PER_SEC;
    cu_invalidate(a->cu);
}
void app_note(App *a, const char *text) {
    app_set_status(a, text);
    a->status_until = ch_monotonic_us() + 8 * CH_USEC_PER_SEC;
}
static void launch_wait(void *data) {
    ch_proc_wait(data, NULL);
    ch_proc_free(data);
}
/* Hands a link or folder to the desktop; the waiting thread reaps the launcher. */
void app_launch(App *a, const char *target) {
    const char *argv[] = {"xdg-open", target, NULL};
    ChSpawn spawn = {.argv = argv, .flags = CH_PROC_SEARCH_PATH | CH_PROC_NEW_SESSION | CH_PROC_STDERR_SILENCE};
    ChError *error = NULL;
    ChProc *proc = ch_proc_spawn(&spawn, &error);
    if (!proc) {
        app_note(a, error->message);
        ch_clear_error(&error);
    } else if (!ch_thread_run(launch_wait, proc))
        ch_proc_free(proc);
}
void app_open_url(App *a, const char *url) {
    if (ch_str_has_prefix(url, "http://") || ch_str_has_prefix(url, "https://") || ch_str_has_prefix(url, "mailto:"))
        app_launch(a, url);
    else
        app_note(a, "Only web links can be opened from a reply.");
}
static const char *idle_status(App *a) {
    return !a->code          ? "Chat · this conversation only"
         : !a->config.tools  ? "Project tools disabled · conversation and attachments only"
         : !*a->project      ? "No folder · choose one to work on files"
         : app_is_chat(a)    ? "Read only · can read files in this session's folders"
         : a->mode == CH_PLAN ? "Agent · Plan · inspect files without changes"
         : a->mode == CH_AUTO ? "Auto · Chela decides what needs your approval"
         : a->mode == CH_BYPASS ? "Bypass permissions · edits and commands run without approval"
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
/* Copies for a session or a request, NULL-terminated. A chat has none. */
static char **session_folders(App *a) {
    size_t n = a->code ? a->folders.len : 0;
    char **out = ch_new0(char *, n + 1);
    for (size_t i = 0; i < n; i++)
        out[i] = ch_strdup(a->folders.data[i]);
    return out;
}
void app_save_session(App *a) {
    if (!a->id || !json_object_array_length(a->messages))
        return;
    char **folders = session_folders(a);
    ChSession v = {.id = a->id, .title = a->session_title, .project = a->code ? a->project : "", .folders = folders,
                   .model = a->config.model, .interaction = a->interaction, .mode = a->mode, .code = a->code};
    ChError *error = NULL;
    bool saved = ch_store_save(a->store, &v, a->messages, &error);
    ch_strfreev(folders);
    if (!saved) {
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
    int order = a->code && a->group_projects ? ch_utf8_collate(x->project, y->project) : 0;
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
        /* Chats and Code sessions never appear in each other's tab. */
        bool visible = s->code == a->code && !s->archived && (!a->activity_days || s->updated / CH_USEC_PER_SEC >= cutoff);
        if (!visible)
            ch_vec_remove(rows, i);
        else
            i++;
    }
    if (a->sort_titles || (a->code && a->group_projects) || a->group_dates)
        ch_vec_sort(rows, compare_sessions, a);
    cu_invalidate(a->cu);
}
static void reset_composer(App *a) {
    cu_edit_set(&a->composer, "");
    attachments_clear(a);
}
static void folders_clear(App *a) {
    ch_vec_clear(&a->folders);
    ch_vec_init(&a->folders, free);
}
/* The panel's git figures belong to the folder they were read from. */
static void project_changed(App *a) {
    ch_clear_pointer(&a->git_branch, free);
    ch_vec_clear(&a->git_branches);
    ch_vec_init(&a->git_branches, free);
    a->git_files = a->git_added = a->git_removed = 0;
    a->last_git_poll = 0;
}
static void leave_session(App *a) {
    monitor_reset(a);
    ch_clear_pointer(&a->id, free);
    ch_clear_pointer(&a->session_title, free);
    json_object_put(a->messages);
    a->messages = json_object_new_array();
    md_cache_clear(a);
    memset(a->feedback, 0, sizeof(a->feedback));
    ch_clear_pointer(&a->stopped_note, free);
    a->recovery_visible = false;
    a->follow = true;
}
void app_start_new(App *a) {
    if (a->busy)
        return;
    leave_session(a);
    reset_composer(a);
    /* Nothing is assumed about where to work, unless the command line said. */
    if (a->code) {
        free(a->project);
        a->project = ch_strdup(a->launch_project);
        folders_clear(a);
        project_changed(a);
        a->mode = a->default_mode;
    }
    app_set_status(a, "Ready when you are");
    hide_status(a);
    cu_focus(a->cu, cu_id(a->cu, "composer"));
    cu_invalidate(a->cu);
}
static void session_apply(App *a, const ChSession *s) {
    free(a->session_title);
    a->session_title = ch_strdup(s->title);
    a->code = s->code;
    if (!s->code) {
        a->interaction = CH_CHAT; /* a chat leaves the Code tab's folder and modes alone */
        return;
    }
    free(a->project);
    a->project = ch_strdup(s->project);
    folders_clear(a);
    for (size_t i = 0; s->folders && s->folders[i]; i++)
        ch_vec_push(&a->folders, ch_strdup(s->folders[i]));
    project_changed(a);
    a->code_interaction = a->interaction = s->interaction;
    a->mode = s->mode;
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
    if (v)
        session_apply(a, v);
    else {
        /* Opened from a list the sidebar filters out; read its metadata directly. */
        ChVec *all = ch_store_list(a->store, "", NULL);
        for (size_t i = 0; all && i < all->len; i++)
            if (!strcmp(((ChSession *)all->data[i])->id, id))
                session_apply(a, all->data[i]);
        ch_vec_free(all);
    }
    ch_clear_pointer(&a->stopped_note, free);
    a->recovery_visible = false;
    /* A session left waiting for its answer can still get one. */
    size_t count = json_object_array_length(a->messages);
    const char *role = count ? ch_json_str(json_object_array_get_idx(a->messages, count - 1), "role") : "";
    if (!strcmp(role, "user") || !strcmp(role, "tool")) {
        a->stopped_note = ch_strdup("The last request did not finish.");
        a->recovery_visible = true;
    }
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
void app_set_project(App *a, const char *path) {
    char *next = ch_strdup(path);
    /* A session stays in its folder, so a different one starts a new session. */
    if (a->id)
        app_start_new(a);
    free(a->project);
    a->project = next;
    if (!*next)
        folders_clear(a);
    for (size_t i = 0; i < a->folders.len; i++)
        if (!strcmp(a->folders.data[i], next))
            ch_vec_remove(&a->folders, i);
    project_changed(a);
    cu_invalidate(a->cu);
}
void app_add_folder(App *a, const char *path) {
    char *real = realpath(path, NULL); /* a link to / is still the whole computer */
    if (!real || !ch_file_is_dir(real)) {
        app_notice(a, "Folder is missing", path);
        free(real);
        return;
    }
    if (!strcmp(real, "/")) {
        app_notice(a, "Choose a folder", "A session can include project folders, not the whole computer.");
        free(real);
        return;
    }
    bool included = !strcmp(real, a->project);
    for (size_t i = 0; i < a->folders.len; i++)
        included |= !strcmp(real, a->folders.data[i]);
    if (included) {
        app_note(a, "That folder is already in this session");
        free(real);
    } else if (!*a->project) {
        /* The first folder is where the session works, even once it has begun. */
        free(a->project);
        a->project = real;
        project_changed(a);
    } else
        ch_vec_push(&a->folders, real);
    app_save_session(a);
    app_refresh_sessions(a);
}
void app_remove_folder(App *a, size_t index) {
    if (a->busy || index >= a->folders.len)
        return;
    ch_vec_remove(&a->folders, index);
    app_save_session(a);
    cu_invalidate(a->cu);
}
void app_set_code(App *a, bool code) {
    if (a->busy || a->code == code)
        return;
    int from = a->code, to = code;
    free(a->tab_session[from]);
    a->tab_session[from] = ch_strdup(a->id);
    free(a->tab_draft[from]);
    a->tab_draft[from] = ch_strdup(cu_edit_text(&a->composer));
    leave_session(a);
    app_clear_selection(a);
    attachments_swap(a, &a->tab_attachments);
    a->code = code;
    a->interaction = code ? a->code_interaction : CH_CHAT;
    cu_edit_set(&a->composer, a->tab_draft[to] ? a->tab_draft[to] : "");
    app_refresh_sessions(a);
    char *id = ch_steal(&a->tab_session[to]);
    if (id)
        app_select_session(a, id);
    free(id);
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
    a->mode = a->default_mode = mode;
    app_save_session(a);
    prefs_save(a);
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
void app_forget_session(App *a) {
    ChError *e = NULL;
    if (ch_store_delete(a->store, a->id, &e)) {
        attachments_forget(a, a->messages);
        ch_map_remove(&a->pinned, a->id);
        ch_map_remove(&a->session_sections, a->id);
        ch_map_remove(&a->selection, a->id);
        app_start_new(a);
        app_refresh_sessions(a);
    } else {
        app_notice(a, "Could not delete session", e->message);
        ch_clear_error(&e);
    }
}
static void delete_confirmed(App *a, void *data) {
    (void)data;
    if (!a->id)
        return;
    /* The dialog may have been open a while. */
    if (a->remove_worktree && git_worktree_removable(a))
        git_worktree_remove(a);
    else
        app_forget_session(a);
}
void app_delete_session(App *a) {
    if (a->busy || !a->id)
        return;
    a->remove_worktree = git_worktree_removable(a);
    if (a->remove_worktree)
        dialog_confirm_option(a, "Delete this session?", a->session_title, "Delete", "Also remove the worktree",
                              "Files that git ignores go with it. If it has uncommitted changes, nothing is deleted. "
                              "Commits that are nowhere else stay on its branch.",
                              &a->remove_worktree, delete_confirmed, NULL);
    else
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
void app_toggle_pin(App *a, const char *id) {
    if (!id)
        return;
    if (!ch_map_remove(&a->pinned, id))
        ch_map_set(&a->pinned, id, NULL);
    a->pinned_collapsed = false;
    prefs_save(a);
    app_refresh_sessions(a);
}
void app_move_to_section(App *a, const char *id, const char *section) {
    if (!id)
        return;
    if (section)
        ch_map_set(&a->session_sections, id, ch_strdup(section));
    else
        ch_map_remove(&a->session_sections, id);
    prefs_save(a);
    app_refresh_sessions(a);
}
static void section_created(App *a, const char *text, void *data) {
    char *name = ch_strstrip(ch_strdup(text));
    bool duplicate = false;
    for (size_t i = 0; i < a->sections.len; i++)
        duplicate |= !strcmp(name, a->sections.data[i]);
    if (*name && data) /* from a session's Move to menu */
        app_move_to_section(a, a->id, name);
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

/* Several sessions at once */
void app_archive_sessions(App *a, ChVec *ids) {
    if (a->busy)
        return;
    bool current = false;
    for (size_t i = 0; i < ids->len; i++) {
        const char *id = ids->data[i];
        ChError *error = NULL;
        if (!ch_store_set_archived(a->store, id, true, &error)) {
            app_notice(a, "Could not archive session", error->message);
            ch_clear_error(&error);
            break;
        }
        ch_map_remove(&a->selection, id);
        current |= !ch_strcmp0(a->id, id);
    }
    if (current)
        app_start_new(a);
    app_refresh_sessions(a);
}
static void doomed_confirmed(App *a, void *data) {
    (void)data;
    ChVec *ids = ch_steal(&a->doomed);
    if (!ids || a->busy) {
        ch_vec_free(ids);
        return;
    }
    bool current = false;
    for (size_t i = 0; i < ids->len; i++) {
        const char *id = ids->data[i];
        ChError *error = NULL;
        json_object *messages = ch_store_load(a->store, id, NULL);
        bool deleted = ch_store_delete(a->store, id, &error);
        if (deleted && messages)
            attachments_forget(a, messages);
        if (messages)
            json_object_put(messages);
        if (!deleted) {
            app_notice(a, "Could not delete session", error->message);
            ch_clear_error(&error);
            break;
        }
        ch_map_remove(&a->pinned, id);
        ch_map_remove(&a->session_sections, id);
        ch_map_remove(&a->selection, id);
        current |= !ch_strcmp0(a->id, id);
    }
    ch_vec_free(ids);
    prefs_save(a);
    if (current)
        app_start_new(a);
    app_refresh_sessions(a);
}
void app_delete_sessions(App *a, ChVec *ids, const char *where) {
    if (a->busy || !ids->len)
        return;
    ch_vec_free(a->doomed);
    a->doomed = ch_vec_new(free);
    for (size_t i = 0; i < ids->len; i++)
        ch_vec_push(a->doomed, ch_strdup(ids->data[i]));
    size_t n = ids->len;
    char *title = ch_strdup_printf("Delete %zu session%s?", n, n == 1 ? "" : "s");
    char *text = ch_strdup_printf("%s%s will be gone for good. Project files and worktrees stay where they are.",
                                  where ? "Every conversation under " : "These conversations", where ? where : "");
    dialog_confirm(a, title, text, "Delete", true, doomed_confirmed, NULL);
    free(title);
    free(text);
}
static bool list_has(const ChVec *list, const char *text) {
    for (size_t i = 0; i < list->len; i++)
        if (!strcmp(list->data[i], text))
            return true;
    return false;
}
/* Rows are in activity order, so the folder last used comes first. A folder that has been
 * moved keeps its place, under any that have not been. */
ChVec *app_project_groups(App *a) {
    ChVec *found = ch_vec_new(free), *out = ch_vec_new(free);
    for (size_t i = 0; a->rows && i < a->rows->len; i++) {
        ChSession *s = a->rows->data[i];
        if (!ch_map_has(&a->pinned, s->id) && !list_has(found, s->project))
            ch_vec_push(found, ch_strdup(s->project));
    }
    for (size_t i = 0; i < found->len; i++)
        if (!list_has(&a->project_order, found->data[i]))
            ch_vec_push(out, ch_strdup(found->data[i]));
    for (size_t i = 0; i < a->project_order.len; i++)
        if (list_has(found, a->project_order.data[i]))
            ch_vec_push(out, ch_strdup(a->project_order.data[i]));
    ch_vec_free(found);
    return out;
}
ChVec *app_project_sessions(App *a, const char *project) {
    ChVec *out = ch_vec_new(free);
    for (size_t i = 0; a->rows && i < a->rows->len; i++) {
        ChSession *s = a->rows->data[i];
        if (!strcmp(s->project, project) && !ch_map_has(&a->pinned, s->id))
            ch_vec_push(out, ch_strdup(s->id));
    }
    return out;
}
void app_move_project(App *a, const char *project, int direction) {
    ChVec *order = app_project_groups(a);
    for (size_t i = 0; i < order->len; i++)
        if (!strcmp(order->data[i], project)) {
            size_t other = i + (size_t)direction;
            if (other < order->len) {
                void *moved = order->data[i];
                order->data[i] = order->data[other];
                order->data[other] = moved;
            }
            break;
        }
    /* Folders hidden for now keep their turn for when they are back. */
    for (size_t i = 0; i < a->project_order.len; i++)
        if (!list_has(order, a->project_order.data[i]))
            ch_vec_push(order, ch_strdup(a->project_order.data[i]));
    ch_vec_clear(&a->project_order);
    a->project_order = *order;
    free(order);
    prefs_save(a);
    cu_invalidate(a->cu);
}
void app_select_project(App *a, const char *project) {
    ChVec *ids = app_project_sessions(a, project);
    for (size_t i = 0; i < ids->len; i++)
        ch_map_set(&a->selection, ids->data[i], NULL);
    ch_vec_free(ids);
    cu_invalidate(a->cu);
}
ChVec *app_selected_sessions(App *a) {
    ChVec *out = ch_vec_new(free);
    for (size_t i = 0; i < a->selection.len; i++)
        ch_vec_push(out, ch_strdup(a->selection.keys[i]));
    return out;
}
void app_clear_selection(App *a) {
    ch_map_clear(&a->selection);
    ch_map_init(&a->selection, NULL);
    cu_invalidate(a->cu);
}

/* Files */
static void attach_chosen(App *a, const char *path, void *data) {
    (void)data;
    if (path)
        attach_path(a, path);
}
void app_attach_file(App *a) {
    if (a->busy)
        return;
    dialog_file(a, 0, "Add files or photos", a->code ? a->project : NULL, NULL, attach_chosen, NULL);
}
static void project_chosen(App *a, const char *path, void *data) {
    (void)data;
    if (path && !a->busy)
        app_set_project(a, path);
}
void app_choose_project(App *a) {
    if (a->busy)
        return;
    dialog_file(a, 1, "Choose project folder", a->project, NULL, project_chosen, NULL);
}
static void folder_chosen(App *a, const char *path, void *data) {
    (void)data;
    if (path && !a->busy)
        app_add_folder(a, path);
}
void app_choose_folder(App *a) {
    if (a->busy)
        return;
    dialog_file(a, 1, "Add folder", a->project, NULL, folder_chosen, NULL);
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
        if (strcmp(ch_json_str(m, "role"), "system")) {
            char *text = ch_message_text(m);
            ch_buf_printf(&s, "## %s\n\n%s\n\n", ch_json_str(m, "role"), text);
            free(text);
        }
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
    dialog_file(a, 2, "Export conversation", a->code ? a->project : NULL, "conversation.md", export_chosen, NULL);
}
void app_show_changes(App *a) {
    if (a->busy || !a->code || !*a->project)
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
    } else
        app_set_status(a, "Done · saved on this computer");
    monitor_finish(a, j->error);
    model_used(a);
    ch_clear_error(&j->error);
    free(j->run.project);
    ch_strfreev(j->run.folders);
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
bool app_images_refused(App *a) {
    if (a->config.provider == 5 || model_vision(a))
        return false;
    for (size_t i = 0; i < a->attachments.len; i++)
        if (((ChAttachment *)a->attachments.data[i])->kind == CH_ATTACH_IMAGE)
            return true;
    return false;
}
void app_send(App *a, bool retry) {
    if (a->busy) {
        app_stop(a);
        return;
    }
    if (!retry && app_images_refused(a))
        return; /* the composer already says why */
    size_t count = json_object_array_length(a->messages);
    /* A retry resends the history as it stands, ending with the request or the last tool result. */
    char *prompt = ch_strstrip(ch_strdup(retry ? "" : cu_edit_text(&a->composer)));
    if (!retry && ((!*prompt && !a->attachments.len) || a->attach_pending)) {
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
    if (!ensure_model_tunnel(a)) {
        /* Say why here too: a retry has nowhere else to show that nothing was sent. */
        if (retry) {
            free(a->stopped_note);
            a->stopped_note = ch_strdup(a->tunnel_error);
        }
        free(prompt);
        return;
    }
    const char *project = a->code ? a->project : "";
    if (*project && !ch_file_is_dir(project)) {
        app_notice(a, "Project folder is missing", "Choose an existing project folder before continuing.");
        free(prompt);
        return;
    }
    /* The worktree comes first, so that the session is saved in the folder it works in. */
    if (!retry && !a->id && a->code && a->use_worktree && a->git_branch && !git_in_worktree(a, a->project)) {
        free(prompt);
        git_worktree_create(a);
        return;
    }
    if (!a->id) {
        a->id = ch_uuid_random();
        a->session_title = ch_title(*prompt ? prompt : ((ChAttachment *)a->attachments.data[0])->name);
    }
    /* Interaction and permission mode are independent; Chat always has read-only tools. */
    if (count && !strcmp(ch_json_str(json_object_array_get_idx(a->messages, 0), "role"), "system"))
        json_object_array_del_idx(a->messages, 0, 1);
    /* A folder that has gone is left out rather than ending the session. */
    char **folders = session_folders(a);
    size_t kept = 0;
    for (size_t i = 0; folders[i]; i++) {
        if (ch_file_is_dir(folders[i]))
            folders[kept++] = folders[i];
        else
            free(folders[i]);
    }
    folders[kept] = NULL;
    ChRun policy = {.config = a->config, .project = (char *)project, .folders = folders, .code = a->code, .mode = a->mode,
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
        json_object *message = ch_message("user", prompt), *attachments = attachments_json(a);
        if (attachments)
            json_object_object_add(message, "_chela_attachments", attachments);
        json_object_array_add(a->messages, message);
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
    j->run = (ChRun){.config = a->config, .project = ch_strdup(project), .folders = folders, .code = a->code, .mode = a->mode,
                     .interaction = policy.interaction, .cancel = ch_cancel_ref(a->cancel), .emit = worker_emit,
                     .approve = worker_approve, .data = j, .text_only = !model_vision(a)};
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
void app_retry(App *a) {
    size_t count = json_object_array_length(a->messages);
    /* Whatever arrived before the failure gives way to a complete answer. */
    if (count && !strcmp(ch_json_str(json_object_array_get_idx(a->messages, count - 1), "role"), "assistant")) {
        json_object_array_del_idx(a->messages, count - 1, 1);
        md_cache_clear(a);
    }
    app_send(a, true);
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
    if (cu_key(cu, CU_KEY_ESCAPE, 0)) {
        if (a->busy) {
            app_stop(a);
            return;
        }
        if (a->selection.len && !cu_modal_active(cu))
            app_clear_selection(a);
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
    else if (cu_key(cu, 'p', CS) && !a->busy && a->code)
        menu_open(a, MENU_PROJECT, cu_id(cu, "project-button"), false);
    else if (cu_key(cu, 'a', CS) && !a->busy)
        menu_open(a, MENU_ATTACH, cu_id(cu, "attach-button"), false);
    else if (cu_key(cu, 'o', CS) && !a->busy && a->id)
        menu_open(a, MENU_SESSION, cu_id(cu, "session-button"), false);
    else if (cu_key(cu, 'f', CS) && !a->busy)
        menu_open(a, a->code ? MENU_ORGANIZER : MENU_FILTER, cu_id(cu, "nav-more"), false);
    else if (cu_key(cu, 'u', CS) && !a->busy)
        menu_open(a, MENU_PROFILE, cu_id(cu, "profile-button"), false);
    else if (cu_key(cu, 'e', CS) && !a->busy && a->code)
        menu_open(a, MENU_ENVIRONMENT, cu_id(cu, "environment-button"), false);
    else if (cu_key(cu, 'v', CS) && !a->busy)
        open_voice_settings(a);
    else if (cu_key(cu, 't', CS) && !a->busy)
        open_settings(a, SETTINGS_MODELS);
    else if (cu_key(cu, 'j', CS) && a->code)
        a->panel_open = !a->panel_open, prefs_save(a);
    else if (cu_key(cu, 'm', CS)) {
        if (!a->busy && a->code)
            app_set_interaction(a, a->interaction == CH_AGENT ? CH_CHAT : CH_AGENT);
    } else if (cu_key(cu, 'm', C)) {
        if (a->code && !a->busy && a->interaction == CH_AGENT)
            menu_open(a, MENU_MODE, cu_id(cu, "mode-button"), true);
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
    cu_open(cu, cu_id(cu, "main-row"), &(CuBox){.row = true, .w = CU_GROW, .h = CU_GROW});
    cu_open(cu, cu_id(cu, "main-column"), &(CuBox){.w = CU_GROW, .h = CU_GROW});
    bool conversation = json_object_array_length(a->messages) > 0 || a->live_active;
    if (conversation)
        chat_build(a);
    else
        welcome_build(a);
    composer_build(a);
    cu_close(cu);
    if (app_panel_visible(a))
        panel_build(a);
    cu_close(cu);
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
