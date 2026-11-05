/* Chela entry point: picks a UI backend and runs the desktop. */
#include "app/app.h"
#include "platform/headless.h"
#include <curl/curl.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void app_install(App *a);

static const CuPlatform *load_plugin(const char *name, char **error) {
    char *exe = ch_read_link("/proc/self/exe", NULL), *dir = exe ? ch_path_dirname(exe) : ch_strdup(".");
    char *file = ch_strdup_printf("chela-%s.so", name);
    char *candidates[] = {ch_build_path(dir, "backends", file, NULL), ch_build_path(dir, "../lib/chela", file, NULL),
                          ch_build_path(dir, file, NULL)};
    const CuPlatform *platform = NULL;
    ChBuf why;
    ch_buf_init(&why);
    for (size_t i = 0; i < CH_N_ELEMENTS(candidates) && !platform; i++) {
        void *module = dlopen(candidates[i], RTLD_NOW | RTLD_LOCAL);
        if (!module) {
            if (ch_file_exists(candidates[i]))
                ch_buf_printf(&why, "%s\n", dlerror());
            continue;
        }
        CuPlatformEntry entry = (CuPlatformEntry)dlsym(module, CU_PLATFORM_ENTRY);
        platform = entry ? entry() : NULL;
        if (platform && platform->abi != CU_PLATFORM_ABI)
            platform = NULL;
    }
    for (size_t i = 0; i < CH_N_ELEMENTS(candidates); i++)
        free(candidates[i]);
    free(file);
    free(dir);
    free(exe);
    if (!platform && error)
        *error = why.len ? ch_buf_steal(&why) : (ch_buf_clear(&why), ch_strdup_printf("the %s backend is not installed", name));
    else
        ch_buf_clear(&why);
    return platform;
}
static const CuPlatform *choose_backend(const char *requested, char **error) {
    if (requested && *requested) {
        if (!strcmp(requested, "wayland"))
            return cu_platform_wayland();
        if (!strcmp(requested, "headless"))
            return cu_platform_headless();
        return load_plugin(requested, error);
    }
    if (ch_getenv("WAYLAND_DISPLAY") || ch_getenv("WAYLAND_SOCKET"))
        return cu_platform_wayland();
    /* Without a Wayland compositor, use a toolkit host if one is installed. */
    const char *fallbacks[] = {"qt", "gtk"};
    for (size_t i = 0; i < CH_N_ELEMENTS(fallbacks); i++) {
        const CuPlatform *p = load_plugin(fallbacks[i], NULL);
        if (p)
            return p;
    }
    *error = ch_strdup("No display found. Run Chela in a Wayland session, or install the Qt or GTK backend for X11.");
    return NULL;
}
static void paint(CuWindow *w, CuCanvas *c, int width, int height, void *data) {
    (void)w;
    cu_paint(((App *)data)->cu, c, width, height);
}
static void event(CuWindow *w, const CuEvent *e, void *data) {
    (void)w;
    cu_handle_event(((App *)data)->cu, e);
}
static bool find_text(void *data, const char *text, CuRect *rect) {
    return cu_find_text(((App *)data)->cu, text, rect);
}
static void list_text(void *data, void (*emit)(const char *text, void *ctx), void *ctx) {
    cu_list_text(((App *)data)->cu, emit, ctx);
}
static int run_desktop(const CuPlatform *p, int argc, char **argv) {
    App *a = ch_new0(App, 1);
    a->p = p;
    a->code = true;
    a->follow = true;
    bool setup = argc > 1 && !strcmp(argv[1], "--setup-models");
    if (setup && argc > 2)
        a->setup_host = ch_strdup(argv[2]);
    ch_mutex_init(&a->event_mutex);
    ch_buf_init(&a->incoming);
    ch_buf_init(&a->live);
    ch_vec_init(&a->dialogs, NULL);
    cu_edit_init(&a->composer, true);
    cu_edit_init(&a->search, false);
    a->messages = json_object_new_array();
    /* Only a folder named on the command line opens with Chela; otherwise the choice is the user's. */
    a->launch_project = !setup && argc > 1 ? ch_canonicalize(argv[1], NULL) : NULL;
    if (!a->launch_project)
        a->launch_project = ch_strdup("");
    a->project = ch_strdup(a->launch_project);
    ch_vec_init(&a->folders, free);
    ch_vec_init(&a->git_branches, free);
    char *data = ch_user_data_dir(), *config = ch_user_config_dir();
    char *data_dir = ch_build_path(data, "chela", NULL), *config_dir = ch_build_path(config, "chela", NULL);
    free(data);
    free(config);
    ChError *error = NULL;
    if (!ch_private_dir(data_dir, &error) || !ch_private_dir(config_dir, &error)) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    a->config_path = ch_build_path(config_dir, "settings.ini", NULL);
    a->preferences_path = ch_build_path(config_dir, "desktop.ini", NULL);
    char *worktrees = ch_build_path(data_dir, "worktrees", NULL);
    a->worktree_root = ch_canonicalize(worktrees, NULL);
    free(worktrees);
    prefs_load(a);
    attachments_init(a);
    char *db = ch_build_path(data_dir, "sessions.db", NULL);
    a->store = ch_store_open(db, &error);
    free(db);
    free(data_dir);
    free(config_dir);
    if (!a->store) {
        fprintf(stderr, "%s\n", error->message);
        return 1;
    }
    char *config_warning = NULL;
    if (!ch_config_load(&a->config, a->config_path, &error)) {
        config_warning = ch_strdup(error->message);
        ch_clear_error(&error);
        ch_config_defaults(&a->config);
    }
    static const CuWindowHandler handler = {paint, event};
    a->window = p->window_new("Chela", "chela", 1440, 940, true, &handler, a);
    if (!a->window) {
        fprintf(stderr, "chela: cannot open a window with the %s backend\n", p->name);
        return 1;
    }
    if (p->window_set_min_size)
        p->window_set_min_size(a->window, 860, 600);
    a->cu = cu_new(p, a->window);
    if (!strcmp(p->name, "headless"))
        headless_set_finder(find_text, list_text, a);
    app_install(a);
    app_refresh_sessions(a);
    a->model_status_for = ch_strdup(a->config.model);
    if (setup) {
        a->setup_at_launch = true;
        open_settings(a, 1);
    }
    else
        ensure_model_tunnel(a);
    if (config_warning) {
        app_notice(a, "Connection settings were reset", config_warning);
        free(config_warning);
    }
    p->run();
    a->closing = true;
    if (a->model_cancel)
        ch_cancel_cancel(a->model_cancel);
    if (a->health_cancel)
        ch_cancel_cancel(a->health_cancel);
    stop_dictation(a);
    stop_model_tunnel(a);
    ch_store_close(a->store);
    /* Workers that are still finishing exit with the process; their results are not needed. */
    return 0;
}
static const char *backend_argument(int *argc, char **argv) {
    const char *backend = ch_getenv("CHELA_BACKEND");
    for (int i = 1; i < *argc; i++)
        if (ch_str_has_prefix(argv[i], "--backend=") || (!strcmp(argv[i], "--backend") && i + 1 < *argc)) {
            bool joined = argv[i][9] == '=';
            backend = joined ? argv[i] + 10 : argv[i + 1];
            int skip = joined ? 1 : 2;
            for (int j = i; j + skip <= *argc; j++)
                argv[j] = argv[j + skip];
            *argc -= skip;
            break;
        }
    return backend;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        puts("Chela " CHELA_VERSION);
        return 0;
    }
    if (argc > 1 && !strcmp(argv[1], "--help")) {
        puts("Chela — native C desktop for local coding agents\n\n"
             "Usage: chela [--backend=NAME] [PROJECT_DIRECTORY]\n       chela --version\n"
             "       chela --setup-models [SSH_HOST]\n\n"
             "Backends: wayland (native, default in a Wayland session), qt, gtk, headless.\n"
             "Configure your model server or ACP harness in Settings (Ctrl+,).\n"
             "Data: $XDG_DATA_HOME/chela, settings: $XDG_CONFIG_HOME/chela.\n"
             "Keyboard: Ctrl+N new session, Ctrl+K search, Enter send, Esc stop.");
        return 0;
    }
    const char *requested = backend_argument(&argc, argv);
    char *error = NULL;
    const CuPlatform *p = choose_backend(requested, &error);
    if (!p) {
        fprintf(stderr, "chela: %s\n", error ? error : "no UI backend available");
        return 1;
    }
    if (!p->init(&argc, &argv)) {
        fprintf(stderr, "chela: the %s backend could not start. See README.md for backends and headless testing.\n", p->name);
        return 1;
    }
    if (ch_getenv("CHELA_SSH_ASKPASS"))
        return askpass_main(p, argc, argv);
    if (curl_global_init(CURL_GLOBAL_DEFAULT)) {
        fputs("Cannot initialize networking\n", stderr);
        return 1;
    }
    int rc = run_desktop(p, argc, argv);
    if (p->shutdown)
        p->shutdown();
    curl_global_cleanup();
    if (!rc && !strcmp(p->name, "headless"))
        rc = headless_status();
    return rc;
}
