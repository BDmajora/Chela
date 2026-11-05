/* Chela desktop application: state, theme and shared helpers. The interface is
 * rebuilt every frame from this state by the toolkit-agnostic UI library. */
#ifndef CHELA_APP_H
#define CHELA_APP_H
#include "../core/chela.h"
#include "../ui/cu.h"
#include <math.h>
#include <stdio.h>

/* October 2026 desktop surfaces, in logical pixels at 100% scale. */
#define SIDEBAR_W 280 /* the reference frame: a full-height 280px sidebar */
#define PANEL_W 262   /* the Code session's environment panel */
#define C_BG 0xff191919
#define C_SIDEBAR 0xff1f1f1f
#define C_BORDER 0xff333333
#define C_TEXT 0xffefefef
#define C_TEXT2 0xffc9c9c9
#define C_MUTED 0xff9e9e9e
#define C_FAINT 0xff858585
#define C_HOVER 0xff2b2b2b
#define C_SELECTED 0xff383838
#define C_BUTTON 0xff303030
#define C_BUTTON_BORDER 0xff414141
#define C_FIELD 0xff262626
#define C_FIELD_BORDER 0xff383838
#define C_FOCUS 0xff777777
#define C_COMPOSER 0xff303030
#define C_COMPOSER_BORDER 0xff424242
#define C_MENU 0xff242424
#define C_MENU_BORDER 0xff424242
#define C_MENU_HOVER 0xff373737
#define C_ACCENT 0xffc47a5d
#define C_MARK 0xffd97550
#define C_CHECK 0xff72a4ec
#define C_DANGER 0xffe39182
#define C_OK 0xff7dbf7f
#define C_METER_BLUE C_CHECK
#define C_METER_YELLOW 0xffe6c04e
#define C_METER_RED 0xffe5534b
#define C_CODE_BG 0xff242424
#define C_INLINE_CODE 0xff333333
#define C_CODE_BORDER 0xff353535
#define C_QUOTE 0xff4f4f4f
#define C_LINK 0xff8fb7f0
#define C_ADDED 0xff7dbf7f
#define C_REMOVED 0xffe0746a
#define C_SCRIM 0x80000000

extern const char *const providers[7];

typedef struct App App;
typedef struct Dialog Dialog;
struct Dialog {
    void (*build)(App *a, Dialog *d);
    void (*destroy)(Dialog *d);
    unsigned serial;
    bool closed;
};

enum {
    MENU_NONE,
    MENU_APP,
    MENU_PROJECT,
    MENU_MODEL,
    MENU_MODE,
    MENU_ATTACH,
    MENU_ENVIRONMENT,
    MENU_SESSION,
    MENU_ROW,
    MENU_FILTER,
    MENU_ORGANIZER,
    MENU_PROFILE,
    MENU_MORE,
    MENU_MODEL_PANEL,
    MENU_GROUP,
    MENU_SELECTION,
};
#define MENU_ITEMS 64
typedef struct {
    int kind;
    CuId anchor;
    bool align_end, above;
    /* A context menu opens where the pointer is, and is about one session or folder. */
    bool at_pointer;
    float x, y;
    char *target;
    int sub[4];
    int level, index[4];
    CuId layer[4];
    /* Keyboard navigation: the highlighted item per level (-1 none, -2 the first enabled
     * one once built), and last frame's items, deepest open level and pointer. */
    int cursor[4], count[4], depth, activate_level, activate_index;
    uint8_t flags[4][MENU_ITEMS], keys[4][MENU_ITEMS]; /* keys: the letter or digit a row shows, which chooses it */
    float mouse_x, mouse_y, mouse_dx;
    bool moved, keyed; /* keyed: a key moved the highlight this frame */
    /* Model discovery for the model picker. */
    json_object *models;
    char *models_error;
    bool models_loading;
    unsigned generation;
} Menu;

typedef struct {
    App *app;
    ChRun run;
    json_object *messages;
    ChError *error;
    ChMutex mutex;
    ChCond condition;
    bool answered, allowed;
    char *permission_title, *permission_detail;
} Job;

struct App {
    Cu *cu;
    const CuPlatform *p;
    CuWindow *window;
    ChConfig config;
    ChStore *store;
    char *config_path, *preferences_path, *id, *session_title, *setup_host;
    char *project; /* the Code tab's working folder; empty when there is none. Chat never uses it. */
    char *launch_project; /* named on the command line: new sessions start there, otherwise blank */
    ChVec folders; /* folders also included in the Code session, as absolute paths */
    /* Chat and Code keep their own place: the session open there and what was being written. */
    char *tab_session[2], *tab_draft[2];
    ChVec tab_attachments; /* the other tab's */
    bool setup_at_launch; /* the next model library setup was opened by --setup-models */
    char *dictation_command;
    ChVec *rows; /* visible sessions in display order */
    json_object *messages;
    CuEdit composer, search;
    bool search_open;
    bool busy, code, closing, follow, sidebar_hidden;
    bool group_projects, group_dates, group_environment, sort_titles;
    int activity_days;
    ChInteraction code_interaction, interaction;
    ChMode mode, default_mode;  /* default_mode: the last one chosen, which new sessions start in */
    bool bypass_enabled;        /* Bypass permissions has been turned on, so it is on offer */
    ChMap pinned, session_sections, load_seconds; /* id → NULL, id → section name, model → double* */
    ChMap selection;            /* sessions marked in the sidebar, id → NULL */
    ChVec sections;
    ChVec project_order;        /* folders moved in the sidebar, first to last; the rest go above them */
    ChVec *doomed;              /* sessions whose deletion is waiting to be confirmed */
    bool pinned_collapsed, recents_collapsed;
    int interface_font, response_font, text_size, transcript_width;
    /* Status and recovery */
    char *status, *stopped_note; /* stopped_note: why the last request ended, shown under the transcript */
    bool status_visible, recovery_visible;
    /* Request */
    Job *job;
    ChCancel *cancel;
    ChMutex event_mutex;
    ChBuf incoming, live;
    bool live_active;
    char *incoming_status;
    json_object *incoming_metrics, *metrics;
    char *run_phase, *last_request_error, *health_state, *health_error;
    int64_t request_started_us, request_ended_us, last_network_us, last_token_us, last_health_poll;
    ChCancel *health_cancel;
    bool health_pending;
    /* Model residency (model.c) */
    char *model_state, *model_status_for, *model_status_error, *load_error, *device_error;
    ChLoadTrack load;
    ChMap load_tail, load_mapped; /* model → double*, from its last load */
    json_object *device_status;
    ChCancel *model_cancel;
    int idle_unload_seconds;
    double last_load_seconds;
    bool preload, model_router, model_poll_pending, device_poll_pending, model_control_pending;
    bool unload_requested, load_observed_late, idle_unloaded, props_pending, props_checked;
    ChModelInfo model_listed, model_running; /* from the model list; from /props of the running model */
    int64_t load_started_us, load_requested_us, load_ready_us, model_idle_since, last_model_poll, last_device_poll,
        last_auto_load, details_seen_us;
    /* SSH tunnel and dictation */
    ChTunnel *tunnel;
    char *tunnel_error;
    ChProc *dictation;
    bool dictation_cancelled;
    unsigned dictation_dialog;
    int64_t dictation_started;
    /* UI */
    Menu menu;
    ChVec dialogs;
    unsigned dialog_serial;
    unsigned approval_dialog;
    int active_jobs;
    unsigned tick_timer, flush_timer;
    uint8_t feedback[2048];
    struct MdCache *md;
    size_t md_count;
    CuId copied_id; /* what was just copied, to show a check for a moment */
    int64_t copied_at;
    /* Attachments of the message being written (attach_ui.c) */
    ChVec attachments;
    char *attachment_folder;
    int attach_pending;
    unsigned attach_generation, attach_order;
    ChMap thumbs;
    int64_t status_until; /* a passing note hides itself at this time */
    /* Code session panel and git (panel.c) */
    bool panel_open, git_pending, git_switching;
    char *git_branch;   /* the current branch, or a detached HEAD's commit; NULL outside a repository */
    ChVec git_branches; /* "name\tworktree" per local branch, last committed first; read for the folder menu */
    int git_added, git_removed, git_files;
    int64_t last_git_poll;
    /* The Worktree box: new Code sessions in a repository get a worktree of their own. */
    bool use_worktree, worktree_pending;
    /* Deleting a session: the box in its confirmation, and the removal once it runs. */
    bool remove_worktree, worktree_removing;
    char *worktree_root;
};

/* app.c */
void app_set_status(App *a, const char *text);
void app_notice(App *a, const char *title, const char *text);
void app_refresh_sessions(App *a);
void app_start_new(App *a);
void app_select_session(App *a, const char *id);
void app_save_session(App *a);
void app_send(App *a, bool retry);
void app_retry(App *a); /* picks a stopped request up again from where it failed */
bool app_images_refused(App *a); /* the draft has images this model cannot read */
void app_stop(App *a);
void app_set_project(App *a, const char *path); /* a session in that folder; "" for no folder */
void app_add_folder(App *a, const char *path);  /* lets the agent into another folder as well */
void app_remove_folder(App *a, size_t index);
void app_choose_folder(App *a);
void app_set_code(App *a, bool code);
void app_set_interaction(App *a, ChInteraction interaction);
void app_set_mode(App *a, ChMode mode);
bool app_save_config(App *a, const ChConfig *config, const char *failure_title);
void app_quit(App *a);
void app_toggle_sidebar(App *a);
void app_focus_search(App *a);
void app_navigate(App *a, int direction);
void app_attach_file(App *a);
void app_note(App *a, const char *text); /* a short message above the composer that hides itself */
void app_open_url(App *a, const char *url);
void app_launch(App *a, const char *target);
bool app_panel_visible(App *a);
void app_export(App *a);
void app_choose_project(App *a);
void app_delete_session(App *a);
void app_forget_session(App *a); /* deletes the current session straight away */
void app_rename_session(App *a);
void app_toggle_pin(App *a, const char *id);
void app_move_to_section(App *a, const char *id, const char *section);
void app_create_section(App *a, bool move_session);
/* Several sessions at once, from the sidebar. ids is the caller's. */
void app_archive_sessions(App *a, ChVec *ids);
void app_delete_sessions(App *a, ChVec *ids, const char *where); /* asks first */
ChVec *app_project_groups(App *a); /* the folders headed in the sidebar, top first */
ChVec *app_project_sessions(App *a, const char *project); /* the ids listed under one */
void app_move_project(App *a, const char *project, int direction);
void app_select_project(App *a, const char *project);
ChVec *app_selected_sessions(App *a); /* the ids marked */
void app_clear_selection(App *a);
void app_view_all(App *a);
void app_show_changes(App *a);
void app_regenerate(App *a, size_t index);
bool app_is_chat(App *a);
const char *app_interface_family(App *a);
const char *app_response_family(App *a);
/* Background work: work runs on a thread, done on the UI thread. */
void app_run(App *a, void (*work)(void *data), void (*done)(void *data), void *data);
void app_post(App *a, void (*fn)(void *data), void *data);

/* prefs.c */
void prefs_load(App *a);
void prefs_save(App *a);

/* widgets.c: styled controls. Most return true when activated this frame. */
CuFont *ui_font(App *a, float size, int weight);
CuFont *serif_font(App *a, float size, int weight);
CuFont *mono_font(App *a, float size);
void ui_label(App *a, const char *text, float size, CuColor color);
void ui_label_wrap(App *a, const char *text, float size, CuColor color, float max_width);
void ui_label_ellipsize(App *a, const char *text, float size, CuColor color, int mode, float max_width);
bool ui_icon_button(App *a, CuId id, const char *icon, const char *tooltip, bool enabled);
bool ui_icon_button_sized(App *a, CuId id, const char *icon, float icon_size, float pad, const char *tooltip,
                          bool enabled, bool checked);
bool ui_flat_button(App *a, CuId id, const char *icon, const char *text, const char *trailing, float size,
                    CuColor color, const char *tooltip, bool enabled, float max_text);
bool ui_button(App *a, CuId id, const char *text, bool enabled, bool primary);
bool ui_checkbox(App *a, CuId id, const char *text, bool *value, bool enabled);
bool ui_flat_check(App *a, CuId id, const char *text, bool *value, float size, CuColor color, const char *tooltip,
                   bool enabled);
bool ui_switch(App *a, CuId id, bool *value, bool enabled);
void ui_switch_mark(App *a, bool on, bool enabled);
bool ui_dropdown(App *a, CuId id, const char *const *choices, int count, int *value, bool enabled, bool flat);
bool ui_spin(App *a, CuId id, CuEdit *edit, double min, double max, double step, int digits, bool enabled);
int ui_entry(App *a, CuId id, CuEdit *edit, const char *placeholder, bool enabled, size_t max_length);
void ui_progress(App *a, CuId id, double fraction, float width);
void ui_meter(App *a, CuId id, double fraction, CuColor color);
void ui_spinner(App *a, float size);
void ui_separator(App *a, float margin);
bool ui_expander(App *a, CuId id, const char *title, bool *expanded);
void ui_mark(App *a, float size);
void ui_avatar(App *a, const char *name);
void ui_mono_text(App *a, CuId id, const char *text, float size, bool selectable);
void ui_text_block(App *a, CuId id, const char *text, float size, CuColor color, bool selectable);
double ui_spin_value(CuEdit *edit, double fallback);
void ui_spin_set(CuEdit *edit, double value, int digits);

/* menus.c */
void menu_open(App *a, int kind, CuId anchor, bool align_end);
void menu_open_here(App *a, int kind, CuId anchor, const char *target);
void menu_close(App *a);
void menu_build(App *a);
void menu_keys(App *a);
bool menu_item(App *a, const char *icon, const char *title, const char *detail, const char *trailing, bool enabled,
               const char *tooltip);
bool menu_item_destructive(App *a, const char *icon, const char *title, const char *trailing, bool enabled);
bool menu_switch(App *a, const char *title, const char *tooltip, bool on, bool enabled);
bool menu_sub_begin(App *a, const char *icon, const char *title, const char *trailing, bool enabled, const char *tooltip);
void menu_sub_end(App *a);
void menu_heading(App *a, const char *text);
void menu_separator(App *a);

/* dialogs.c */
Dialog *dialog_push(App *a, Dialog *d);
Dialog *dialog_find(App *a, unsigned serial);
void dialog_close(App *a, Dialog *d);
void dialogs_build(App *a);
void dialogs_free(App *a);
bool dialog_frame_begin(App *a, Dialog *d, const char *title, float width, float height);
void dialog_frame_end(App *a);
bool dialog_footer_buttons(App *a, Dialog *d, const char *cancel, const char *accept, bool accept_enabled, int *choice);
void dialog_confirm_option(App *a, const char *title, const char *text, const char *accept, const char *option,
                           const char *detail, bool *checked, void (*callback)(App *, void *), void *data);
void dialog_confirm(App *a, const char *title, const char *text, const char *accept, bool destructive,
                    void (*callback)(App *a, void *data), void *data);
void dialog_text_input(App *a, const char *title, const char *initial, const char *placeholder, const char *accept,
                       size_t max_length, void (*callback)(App *a, const char *text, void *data), void *data);
void dialog_text_view(App *a, const char *title, const char *text, bool monospace, float width, float height);
void dialog_file(App *a, int kind, const char *title, const char *folder, const char *name,
                 void (*callback)(App *a, const char *path, void *data), void *data);
void open_settings(App *a, int page);
void open_diagnostics(App *a);
void open_model_test(App *a);
void open_voice_settings(App *a);
void open_all_sessions(App *a);
void open_shortcuts(App *a);
void open_about(App *a);
void open_image(App *a, const char *path, const char *name);
unsigned open_approval(App *a, Job *job);
void close_approval(App *a);
unsigned open_listening(App *a);

/* Settings pages, in open_settings order. */
enum { SETTINGS_GENERAL, SETTINGS_CONNECTIONS, SETTINGS_MODELS };

/* attach_ui.c: files and images on the message being written and in the transcript. */
void attachments_init(App *a);
void attachments_clear(App *a);
void attachments_swap(App *a, ChVec *other); /* the draft of the other tab comes back */
void attachments_free(App *a);
void attachments_take_payloads(App *a); /* pasted and dropped data */
void attach_path(App *a, const char *path);
void attachments_row(App *a);
void attachment_sent(App *a, CuId id, json_object *attachment);
json_object *attachments_json(App *a);
void attachments_forget(App *a, json_object *messages); /* after a session is deleted */
CuImage *thumb_image(App *a, const char *path, int side, int *width, int *height);

/* sidebar.c, chat.c, composer.c, panel.c */
void sidebar_build(App *a);
void sidebar_fold_all(App *a, bool fold);
bool sidebar_can_fold(App *a, bool fold); /* something is left to fold, or to unfold */
void topbar_build(App *a);
void chat_build(App *a);
void welcome_build(App *a);
void composer_build(App *a);
float content_width(App *a);
void md_cache_clear(App *a);
void panel_build(App *a);
ChVec *recent_projects(App *a, size_t limit);
void git_poll(App *a);                                /* keeps the git figures current, off the UI thread */
void git_switch_branch(App *a, const char *branch);   /* refused while there are uncommitted changes */
void git_worktree_create(App *a);                     /* then sends the draft from the new worktree */
bool git_in_worktree(App *a, const char *path);       /* in a worktree Chela made for a session */
char *git_worktree_origin(App *a, const char *path);  /* the repository of that worktree; NULL if it has gone */
bool git_worktree_removable(App *a);                  /* this session's worktree is there, and only this session uses it */
void git_worktree_remove(App *a);                     /* then deletes the session, unless the worktree holds work */

/* model.c */
void model_tick(App *a);
void model_selected(App *a);
void model_used(App *a);
void model_state_apply(App *a, const char *model, json_object *models, const ChError *error);
void model_load(App *a, bool automatic);
void model_unload(App *a, bool user);
void model_preload_set(App *a, bool on);
bool model_preload_enabled(App *a);
const char *model_preload_hint(App *a);
void model_preload_row(App *a);
void model_details_build(App *a);
bool model_chip_visible(App *a);
char *model_status_summary(App *a);
void model_card_build(App *a);
void model_panel_build(App *a);
const char *model_state_name(const char *state);
json_object *model_device(App *a);
const char *model_backend(App *a);
char *model_memory_text(App *a);
char *model_live_text(App *a, bool busy_only);
char *model_slot_text(App *a, bool streamed);
char *model_eta_text(App *a, double *fraction); /* "42% · 1m 12s left", with the bar's position */
char *model_phase_text(App *a);
void model_progress_build(App *a);
/* How full the context is, in the server's numbers. limit is 0 when the server does not say. */
typedef struct {
    long long prompt, reply, used;
    int limit;
    double fraction;
} ModelContext;
ModelContext model_context(App *a);
void model_context_build(App *a);
CuColor model_context_color(double fraction);
int model_vision(App *a); /* 1 reads images, 0 does not, -1 unknown */
char *format_clock(int64_t microseconds);
bool model_control_supported(App *a);

/* monitor.c */
void monitor_start(App *a);
void monitor_reset(App *a);
void monitor_finish(App *a, const ChError *error);
void monitor_build(App *a);
char *diagnostics_snapshot(App *a);

/* tunnel.c and voice.c */
bool ensure_model_tunnel(App *a);
void stop_model_tunnel(App *a);
void dictate(App *a);
void stop_dictation(App *a);

/* askpass.c: the SSH_ASKPASS helper process. */
int askpass_main(const CuPlatform *p, int argc, char **argv);
#endif
