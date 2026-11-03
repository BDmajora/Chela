/* Popup menus: one menu tree is open at a time; submenus open on hover. */
#include "app.h"
#include <dirent.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    ChConfig config;
    unsigned generation;
    json_object *models;
    ChError *error;
} ModelProbe;
static void model_probe_work(void *data) {
    ModelProbe *p = data;
    p->models = ch_models(&p->config, NULL, &p->error);
}
static void model_probe_done(void *data) {
    ModelProbe *p = data;
    App *a = p->app;
    if (!a->closing && a->menu.kind == MENU_MODEL && a->menu.generation == p->generation) {
        a->menu.models = p->models;
        p->models = NULL;
        a->menu.models_error = p->error ? ch_strdup(p->error->message) : NULL;
        a->menu.models_loading = false;
        cu_invalidate(a->cu);
    }
    if (p->models)
        json_object_put(p->models);
    ch_clear_error(&p->error);
    free(p);
}
/* Closing keeps the nesting bookkeeping intact: a menu may close from inside a
 * submenu while the rest of this frame's menu is still being described. */
void menu_close(App *a) {
    Menu *m = &a->menu;
    if (m->models)
        json_object_put(m->models);
    m->models = NULL;
    ch_clear_pointer(&m->models_error, free);
    m->models_loading = false;
    m->kind = MENU_NONE;
    m->generation++;
    cu_invalidate(a->cu);
}
void menu_open(App *a, int kind, CuId anchor, bool align_end) {
    bool same = a->menu.kind == kind && a->menu.anchor == anchor;
    menu_close(a);
    if (same)
        return; /* clicking the anchor again closes its menu */
    a->menu.level = 0;
    a->menu.kind = kind;
    a->menu.anchor = anchor;
    a->menu.align_end = align_end;
    for (int i = 0; i < 4; i++) {
        a->menu.sub[i] = a->menu.cursor[i] = -1;
        a->menu.count[i] = 0;
    }
    a->menu.depth = 0;
    a->menu.activate_level = -1;
    if (kind == MENU_MODEL && a->config.provider != 5 && ensure_model_tunnel(a)) {
        ModelProbe *p = ch_new0(ModelProbe, 1);
        p->app = a;
        p->config = a->config;
        p->config.timeout = 10;
        p->generation = a->menu.generation;
        a->menu.models_loading = true;
        app_run(a, model_probe_work, model_probe_done, p);
    }
}

/* Items */
enum { ITEM_ENABLED = 1, ITEM_SUBMENU = 2 };
static CuId item_id(App *a, bool enabled, bool submenu) {
    Menu *m = &a->menu;
    int level = m->level, index = m->index[level]++;
    if (index < MENU_ITEMS)
        m->flags[level][index] = (enabled ? ITEM_ENABLED : 0) | (submenu ? ITEM_SUBMENU : 0);
    if (enabled && m->cursor[level] == -2)
        m->cursor[level] = index;
    return cu_idn(a->cu, "#menu-item", level * 1000 + index);
}
/* The pointer highlights an item only when it moves, so it does not fight the keyboard. */
static bool item_hot(App *a, CuId id, bool enabled) {
    Menu *m = &a->menu;
    int level = m->level, index = m->index[level] - 1;
    if (enabled && m->moved) {
        if (cu_hovered(a->cu, id))
            m->cursor[level] = index;
        else if (m->cursor[level] == index)
            m->cursor[level] = -1;
    }
    return enabled && (m->cursor[level] == index || m->sub[level] == index);
}
static bool item_chosen(App *a, CuId id, bool enabled) {
    Menu *m = &a->menu;
    bool keyboard = m->activate_level == m->level && m->activate_index == m->index[m->level] - 1;
    return enabled && (cu_clicked(a->cu, id) || keyboard);
}
static void item_body(App *a, CuId id, const char *icon, const char *title, const char *detail, const char *trailing,
                      bool enabled, bool destructive, bool chevron, const char *tooltip) {
    Cu *cu = a->cu;
    bool hot = item_hot(a, id, enabled);
    cu_open(cu, id,
            &(CuBox){.row = true, .gap = 10, CU_PAD2(5, 10), .radius = 5, .w = CU_GROW, .align_y = detail ? CU_START : CU_CENTER,
                     .bg = hot ? C_MENU_HOVER : 0, .interactive = true, .tooltip = tooltip});
    CuColor color = !enabled ? 0xff969696 : destructive ? C_DANGER : C_TEXT;
    if (icon)
        cu_icon(cu, icon, 18, enabled ? (destructive ? C_DANGER : 0xffd4d4d4) : 0xff7a7a7a);
    cu_open(cu, 0, &(CuBox){.gap = 3, .w = CU_GROW});
    cu_text(cu, 0, title,
            &(CuTextStyle){.font = ui_font(a, 14, 400), .color = color, .ellipsize = CU_ELLIPSIZE_END, .w = CU_FIT_MAX(300)});
    if (detail)
        cu_text(cu, 0, detail,
                &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .wrap = CU_WRAP_WORD, .w = CU_GROW_MAX(280)});
    cu_close(cu);
    if (trailing && !strcmp(trailing, "✓"))
        cu_icon(cu, "check", 16, C_CHECK);
    else if (trailing)
        cu_text(cu, 0, trailing, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_MUTED});
    if (chevron)
        cu_icon(cu, "chevron-right", 14, C_MUTED);
    cu_close(cu);
}
bool menu_item(App *a, const char *icon, const char *title, const char *detail, const char *trailing, bool enabled,
               const char *tooltip) {
    CuId id = item_id(a, enabled, false);
    item_body(a, id, icon, title, detail, trailing, enabled, false, false, tooltip);
    if (enabled && a->menu.moved && cu_hovered(a->cu, id))
        a->menu.sub[a->menu.level] = -1;
    if (item_chosen(a, id, enabled)) {
        menu_close(a);
        return true;
    }
    return false;
}
bool menu_item_destructive(App *a, const char *icon, const char *title, const char *trailing, bool enabled) {
    CuId id = item_id(a, enabled, false);
    item_body(a, id, icon, title, NULL, trailing, enabled, true, false, NULL);
    if (enabled && a->menu.moved && cu_hovered(a->cu, id))
        a->menu.sub[a->menu.level] = -1;
    if (item_chosen(a, id, enabled)) {
        menu_close(a);
        return true;
    }
    return false;
}
void menu_heading(App *a, const char *text) {
    cu_open(a->cu, 0, &(CuBox){CU_PAD2(5, 10), .w = CU_GROW});
    cu_text(a->cu, 0, text, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED});
    cu_close(a->cu);
}
void menu_separator(App *a) {
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(5, 8)});
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .h = CU_PX(1), .bg = C_MENU_BORDER});
    cu_close(a->cu);
    cu_close(a->cu);
}
static void layer_begin(App *a, CuId anchor, int placement, bool align_end, float min_width) {
    Menu *m = &a->menu;
    CuId layer = cu_idn(a->cu, "#menu-layer", m->level);
    m->layer[m->level] = layer;
    cu_layer_open(a->cu, layer,
                  &(CuLayer){.kind = CU_LAYER_POPUP, .anchor = anchor, .placement = (uint8_t)placement,
                             .align_end = align_end, .offset = placement == CU_PLACE_RIGHT ? 6 : 4},
                  &(CuBox){CU_PAD(5), .bg = C_MENU, .border = 1, .border_color = C_MENU_BORDER, .radius = 12,
                           .shadow = 14, .w = CU_FIT_MIN(min_width)});
}
bool menu_sub_begin(App *a, const char *icon, const char *title, const char *trailing, bool enabled,
                    const char *tooltip) {
    Menu *m = &a->menu;
    int level = m->level, index = m->index[level];
    CuId id = item_id(a, enabled, true);
    item_body(a, id, icon, title, NULL, trailing, enabled, false, true, tooltip);
    if (enabled && ((m->moved && cu_hovered(a->cu, id)) || cu_clicked(a->cu, id)))
        m->sub[level] = index;
    if (!enabled || m->sub[level] != index || level >= 3)
        return false;
    m->level++;
    m->depth = CH_MAX(m->depth, m->level);
    m->index[m->level] = 0;
    m->sub[m->level] = m->sub[m->level] < -1 ? -1 : m->sub[m->level];
    layer_begin(a, id, CU_PLACE_RIGHT, false, 214);
    return true;
}
void menu_sub_end(App *a) {
    Menu *m = &a->menu;
    cu_layer_close(a->cu);
    if (m->level <= 0)
        return;
    m->count[m->level] = m->index[m->level];
    if (m->kind && cu_layer_dismissed(a->cu, m->layer[m->level])) {
        m->sub[m->level - 1] = -1;
        m->cursor[m->level] = -1;
        cu_invalidate(a->cu); /* this frame still shows the submenu */
    }
    m->level--;
}
static int next_item(const Menu *m, int level, int from, int step) {
    int n = CH_MIN(m->count[level], MENU_ITEMS);
    for (int k = 1; k <= n; k++) {
        int i = ((from + step * k) % n + n) % n;
        if (m->flags[level][i] & ITEM_ENABLED)
            return i;
    }
    return -1;
}
/* Keyboard navigation, before anything else takes the keys: Up/Down/Home/End move
 * within the deepest open menu, Right or Return opens a submenu, Left closes it,
 * Return activates. Escape is handled by the popup layer. */
void menu_keys(App *a) {
    Menu *m = &a->menu;
    Cu *cu = a->cu;
    if (!m->kind || m->kind == MENU_MODEL_PANEL)
        return;
    int level = CH_CLAMP(m->depth, 0, 3), n = CH_MIN(m->count[level], MENU_ITEMS), *cursor = &m->cursor[level];
    if (cu_key(cu, CU_KEY_DOWN, 0))
        *cursor = next_item(m, level, *cursor < 0 ? -1 : *cursor, 1);
    else if (cu_key(cu, CU_KEY_UP, 0))
        *cursor = next_item(m, level, *cursor < 0 ? n : *cursor, -1);
    else if (cu_key(cu, CU_KEY_HOME, 0))
        *cursor = next_item(m, level, -1, 1);
    else if (cu_key(cu, CU_KEY_END, 0))
        *cursor = next_item(m, level, n, -1);
    else if (cu_key(cu, CU_KEY_LEFT, 0) && level > 0) {
        m->sub[level - 1] = -1;
        *cursor = -1;
    } else if (*cursor >= 0 && *cursor < n && (cu_key(cu, CU_KEY_RIGHT, 0) || cu_key(cu, CU_KEY_RETURN, 0))) {
        if (m->flags[level][*cursor] & ITEM_SUBMENU) {
            m->sub[level] = *cursor;
            if (level < 3)
                m->cursor[level + 1] = -2;
        } else {
            m->activate_level = level;
            m->activate_index = *cursor;
        }
    } else
        return;
    cu_invalidate(cu);
}

/* Menu contents */
static void thinking_menu(App *a) {
    menu_heading(a, "Thinking / reasoning");
    bool supported = ch_thinking_supported(&a->config), gpt = ch_model_is_gpt_oss(&a->config);
    const char *details[] = {"Use the model's default behavior", "Skip optional thinking for faster replies",
                             "Shorter reasoning for everyday questions", "More room to work through a question",
                             "More reasoning for difficult tasks; may be slower"};
    for (int i = CH_THINK_DEFAULT; i <= CH_THINK_DEEP; i++) {
        bool enabled = (supported || i == CH_THINK_DEFAULT) && !(gpt && i == CH_THINK_OFF);
        if (menu_item(a, NULL, ch_thinking_name(i), i == CH_THINK_OFF && gpt ? "GPT-OSS always reasons; choose Quick" : details[i],
                      ch_thinking_effective(&a->config) == (ChThinking)i ? "✓" : NULL, enabled, NULL) && !a->busy) {
            ChConfig config = a->config;
            config.thinking = (ChThinking)i;
            app_save_config(a, &config, "Could not save thinking setting");
        }
    }
    menu_separator(a);
    const char *note = !supported ? "Use the server or harness settings for this connection"
                     : a->config.provider == 6 && !gpt ? "Thinking shares your response limit. Availability depends on the model."
                                                       : "Availability depends on the model. Some models only support thinking on or off.";
    menu_item(a, NULL, "Model support", note, NULL, false, NULL);
}
static void mode_menu(App *a) {
    menu_heading(a, "Mode");
    const char *titles[] = {"Manual", "Accept edits", "Plan", "Bypass permissions"};
    const char *descriptions[] = {"Ask before editing files or running commands",
                                  a->config.provider == 5 ? "Harness actions still ask for approval"
                                                          : "Apply file edits; ask before running commands",
                                  "Explore your project without making changes",
                                  "Run edits and commands without approval prompts"};
    const ChMode values[] = {CH_ASK, CH_ACCEPT_EDITS, CH_PLAN, CH_AUTO};
    for (int i = 0; i < 4; i++)
        if (menu_item(a, NULL, titles[i], NULL, a->mode == values[i] ? "✓" : NULL, true, descriptions[i]))
            app_set_mode(a, values[i]);
}
static void interaction_menu(App *a) {
    menu_heading(a, "Interaction");
    if (menu_item(a, "utilities-terminal", "Agent", "Read, edit, and run commands",
                  a->interaction == CH_AGENT ? "✓" : NULL, true, NULL))
        app_set_interaction(a, CH_AGENT);
    if (menu_item(a, "user-available", "Chat", "Ask questions with read-only project access",
                  a->interaction == CH_CHAT ? "✓" : NULL, true, NULL))
        app_set_interaction(a, CH_CHAT);
}
static void attach_context(App *a, const char *path, const char *kind) {
    ChError *error = NULL;
    char *content = ch_read_file(a->project, path, &error);
    if (!content || strlen(content) > 256 * 1024) {
        app_notice(a, "Could not add skill", error ? error->message : "Skill files are limited to 256 KiB.");
        ch_clear_error(&error);
        free(content);
        return;
    }
    char *block = ch_strdup_printf("\n\n%s (%s):\n%s\n", kind ? kind : "Skill instructions", path, content);
    /* Append the block but keep typing before it, so the request (and the session title) come first. */
    size_t end = a->composer.text.len;
    a->composer.caret = a->composer.anchor = end;
    cu_edit_insert(&a->composer, block);
    a->composer.caret = a->composer.anchor = end;
    cu_focus(a->cu, cu_id(a->cu, "composer"));
    free(block);
    free(content);
}
static int compare_names(const void *x, const void *y, void *data) {
    (void)data;
    return ch_utf8_collate(*(char *const *)x, *(char *const *)y);
}
static void skills_menu(App *a) {
    menu_heading(a, "Project skills");
    const char *roots[] = {".chela/skills", ".claude/skills"};
    int count = 0;
    for (size_t i = 0; i < CH_N_ELEMENTS(roots); i++) {
        char *root = ch_build_path(a->project, roots[i], NULL);
        DIR *dir = opendir(root);
        ChVec names;
        ch_vec_init(&names, free);
        for (struct dirent *e; dir && (e = readdir(dir));)
            if (e->d_name[0] != '.')
                ch_vec_push(&names, ch_strdup(e->d_name));
        if (dir)
            closedir(dir);
        ch_vec_sort(&names, compare_names, NULL);
        for (size_t j = 0; j < names.len && count < 30; j++) {
            const char *name = names.data[j];
            char *relative = ch_build_path(roots[i], name, "SKILL.md", NULL);
            char *full = ch_build_path(a->project, relative, NULL);
            if (ch_file_is_regular(full)) {
                if (menu_item(a, "skills", name, NULL, NULL, true, NULL))
                    attach_context(a, relative, NULL);
                count++;
            }
            free(relative);
            free(full);
        }
        ch_vec_clear(&names);
        free(root);
    }
    if (!count)
        menu_item(a, NULL, "No project skills", NULL, NULL, false, NULL);
    menu_separator(a);
    if (menu_item(a, "document-open", "Add instructions from file…", NULL, NULL, true, NULL))
        app_attach_file(a);
}
static void extension_menu(App *a, bool plugins) {
    menu_heading(a, plugins ? "Plugins" : "Connectors");
    menu_item(a, NULL,
              a->config.provider == 5 ? "Managed by your ACP harness" : plugins ? "No plugins connected" : "No connectors connected",
              a->config.provider == 5 ? "Configure extensions in your harness." : "Available through a configured ACP harness.",
              NULL, false, NULL);
}
/* Recent project folders, the selected one first. */
static ChVec *recent_projects(App *a, size_t limit) {
    ChVec *out = ch_vec_new(free);
    ch_vec_push(out, ch_strdup(a->project));
    ChVec *sessions = ch_store_list(a->store, "", NULL);
    for (size_t i = 0; sessions && i < sessions->len && out->len < limit; i++) {
        ChSession *s = sessions->data[i];
        bool seen = false;
        for (size_t k = 0; k < out->len; k++)
            seen |= !strcmp(out->data[k], s->project);
        if (!seen)
            ch_vec_push(out, ch_strdup(s->project));
    }
    ch_vec_free(sessions);
    return out;
}
static void add_to_project_menu(App *a) {
    ChVec *projects = recent_projects(a, 10);
    for (size_t i = 0; i < projects->len; i++) {
        const char *path = projects->data[i];
        char *name = ch_path_basename(path);
        if (menu_item(a, "folder", name, NULL, !strcmp(path, a->project) ? "✓" : NULL, true, path)) {
            if (!ch_file_is_dir(path))
                app_notice(a, "Project folder is missing", path);
            else
                app_set_project(a, path, false);
        }
        free(name);
    }
    ch_vec_free(projects);
}
static void design_menu(App *a) {
    menu_heading(a, "Project design guidelines");
    const char *paths[] = {".chela/DESIGN.md", "DESIGN.md"};
    int count = 0;
    for (size_t i = 0; i < CH_N_ELEMENTS(paths); i++) {
        char *full = ch_build_path(a->project, paths[i], NULL);
        if (ch_file_is_regular(full)) {
            if (menu_item(a, "document-open", paths[i], NULL, NULL, true, NULL))
                attach_context(a, paths[i], "Design guidelines");
            count++;
        }
        free(full);
    }
    if (!count)
        menu_item(a, NULL, "No project design guide", NULL, NULL, false, NULL);
    menu_separator(a);
    if (menu_item(a, "document-open", "Add guidelines from file…", NULL, NULL, true, NULL))
        app_attach_file(a);
}
static void attach_menu(App *a) {
    if (menu_item(a, "document-open", "Add files or photos", NULL, "Ctrl+U", true,
                  "This connection supports UTF-8 text files, up to 256 KiB; image input is not available"))
        app_attach_file(a);
    menu_item(a, "camera", "Take a screenshot", NULL, NULL, false, "Image attachments are not supported by this connection");
    if (!a->code && menu_sub_begin(a, "folder", "Add to project", NULL, true, NULL)) {
        add_to_project_menu(a);
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_sub_begin(a, "skills", "Skills", NULL, true, NULL)) {
        skills_menu(a);
        menu_sub_end(a);
    }
    if (menu_sub_begin(a, "connectors", "Connectors", NULL, true, NULL)) {
        extension_menu(a, false);
        menu_sub_end(a);
    }
    if (!a->code && menu_sub_begin(a, "palette", "Design system", NULL, true, NULL)) {
        design_menu(a);
        menu_sub_end(a);
    }
    if (menu_sub_begin(a, "plugins", "Add plugins", NULL, true, NULL)) {
        extension_menu(a, true);
        menu_sub_end(a);
    }
    if (!a->code) {
        menu_separator(a);
        menu_item(a, "research", "Research", NULL, NULL, false, "Requires a research connector");
        menu_item(a, "globe", "Web search", NULL, NULL, false, "Requires a web search connector");
    }
}
static void voice_menu(App *a) {
    if (menu_item(a, "microphone", "Dictate", NULL, "Ctrl+D", true, NULL))
        dictate(a);
    menu_item(a, "waveform", "Use voice mode", NULL, NULL, false, "Spoken replies are not available yet");
    menu_separator(a);
    if (menu_sub_begin(a, "preferences-system", "Microphone", NULL, true, NULL)) {
        menu_item(a, "microphone", "Transcription app default", NULL, "✓", false, NULL);
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_item(a, NULL, "Voice settings…", NULL, NULL, true, NULL))
        open_voice_settings(a);
}
static void project_menu(App *a) {
    menu_heading(a, "Project folder");
    ChVec *projects = recent_projects(a, 7);
    for (size_t i = 0; i < projects->len; i++) {
        const char *path = projects->data[i];
        char *name = ch_path_basename(path);
        if (menu_item(a, "folder", name, path, i == 0 ? "✓" : NULL, true, NULL) && !a->busy) {
            if (!ch_file_is_dir(path))
                app_notice(a, "Project folder is missing", path);
            else if (strcmp(path, a->project))
                app_set_project(a, path, true);
            cu_focus(a->cu, cu_id(a->cu, "composer"));
        }
        free(name);
    }
    ch_vec_free(projects);
    menu_separator(a);
    if (menu_item(a, "folder", "Open folder…", NULL, NULL, true, NULL))
        app_choose_project(a);
}
static void environment_menu(App *a) {
    menu_heading(a, "Environment");
    menu_item(a, "computer", "Local", "Project files and commands run on this computer", "✓", false, NULL);
    menu_separator(a);
    if (menu_item(a, "network-server", "Model connection…", a->config.provider == 5 ? "External ACP harness" : a->config.endpoint,
                  NULL, true, NULL))
        open_settings(a, 1);
}
static void select_model(App *a, const char *id) {
    if (a->busy)
        return;
    ChConfig config = a->config;
    ch_strlcpy(config.model, id, sizeof(config.model));
    if (app_save_config(a, &config, "Could not select model")) {
        app_set_status(a, "Model selected");
        model_tick(a);
    }
}
static void model_menu(App *a) {
    if (a->config.ssh_enabled && !a->tunnel) {
        menu_heading(a, "Model");
        menu_item(a, NULL, "Inference connection needs attention", a->tunnel_error ? a->tunnel_error : "Reconnect in Settings → Connections",
                  NULL, false, a->tunnel_error);
        if (menu_item(a, "network-server", "Manage connection…", "SSH address, model folder and ports", NULL, true, NULL))
            open_settings(a, 1);
        return;
    }
    menu_heading(a, "Model");
    if (a->config.provider == 5) {
        menu_item(a, NULL, "ACP harness", "The harness manages its own model", "✓", false, NULL);
        menu_separator(a);
        if (menu_item(a, "network-server", "Manage connection…", "Connect or refresh an inference computer", NULL, true, NULL))
            open_settings(a, 1);
        return;
    }
    /* Choosing a model closes the menu, which frees its list while the loop below runs. */
    json_object *models = a->menu.models ? json_object_get(a->menu.models) : NULL;
    const char *current_detail = models ? "Current model" : providers[a->config.provider];
    for (size_t i = 0; models && i < json_object_array_length(models); i++) {
        json_object *model = json_object_array_get_idx(models, i);
        if (!strcmp(ch_json_str(model, "id"), a->config.model) && ch_model_detail(model))
            current_detail = ch_model_detail(model);
    }
    char *current = ch_strdup(a->config.model);
    if (menu_item(a, NULL, current, current_detail, "✓", true, current))
        select_model(a, current);
    size_t count = 0;
    for (size_t i = 0; models && i < json_object_array_length(models); i++) {
        json_object *model = json_object_array_get_idx(models, i);
        const char *id = ch_json_str(model, "id");
        if (!*id || strlen(id) >= sizeof(a->config.model) || !strcmp(id, current))
            continue;
        char *copy = ch_strdup(id);
        if (menu_item(a, NULL, copy, ch_model_detail(model), NULL, true, copy))
            select_model(a, copy);
        free(copy);
        count++;
    }
    free(current);
    json_object_put(models);
    if (a->menu.models_loading)
        menu_item(a, NULL, "Discovering models…", NULL, NULL, false, NULL);
    menu_separator(a);
    if (!a->menu.models_loading && (a->menu.models_error || !count))
        menu_item(a, NULL, a->menu.models_error ? "Could not reach model server" : "No other models found",
                  a->menu.models_error ? "Check your inference computer in Settings → Connections" : NULL, NULL, false,
                  a->menu.models_error);
    if (!a->menu.models_loading && model_chip_visible(a) &&
        menu_item(a, "computer", "Model status…", "Device, memory, load time and keep loaded", "Ctrl+Shift+S", true, NULL))
        menu_open(a, MENU_MODEL_PANEL, cu_id(a->cu, "model-chip"), true);
    if (menu_item(a, "network-server", "Server diagnostics", "Status, device and recent server logs", "Ctrl+Shift+D", true, NULL))
        open_diagnostics(a);
    if (menu_item(a, "media-playback-start", "Test selected model", "Load the model and check a real response", NULL, true, NULL))
        open_model_test(a);
    if (menu_item(a, "network-server", "Manage connection…", "Connect or refresh an inference computer", NULL, true, NULL))
        open_settings(a, 1);
}
static void session_menu(App *a) {
    if (menu_item(a, "document-edit", "Rename…", NULL, NULL, a->id != NULL, NULL))
        app_rename_session(a);
    if (menu_item(a, "document-save", "Export conversation…", NULL, NULL, a->id != NULL, NULL))
        app_export(a);
    if (menu_item(a, "view-list", "View project changes", NULL, NULL, true, NULL))
        app_show_changes(a);
    menu_separator(a);
    if (menu_sub_begin(a, "system-users", "Interaction", NULL, a->code, NULL)) {
        interaction_menu(a);
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_item_destructive(a, "user-trash", "Delete session…", NULL, a->id != NULL))
        app_delete_session(a);
}
static void row_menu(App *a) {
    bool pinned = a->id && ch_map_has(&a->pinned, a->id);
    if (menu_item(a, "pin", pinned ? "Unpin" : "Pin", NULL, "P", a->id != NULL, NULL))
        app_toggle_pin(a);
    if (menu_item(a, "document-edit", "Rename…", NULL, "R", true, NULL))
        app_rename_session(a);
    menu_item(a, "folder", "Add to project", NULL, NULL, false, "The session uses its selected project folder");
    if (menu_sub_begin(a, NULL, "Move to", NULL, true, NULL)) {
        if (menu_item(a, NULL, "Recents", NULL, NULL, true, NULL))
            app_move_to_section(a, NULL);
        const char *current = a->id ? ch_map_get(&a->session_sections, a->id) : NULL;
        for (size_t i = 0; i < a->sections.len; i++) {
            const char *name = a->sections.data[i];
            char *copy = ch_strdup(name);
            if (menu_item(a, NULL, copy, NULL, !ch_strcmp0(current, name) ? "✓" : NULL, true, NULL))
                app_move_to_section(a, copy);
            free(copy);
        }
        menu_separator(a);
        if (menu_item(a, "list-add", "New section…", NULL, NULL, true, NULL))
            app_create_section(a, true);
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_item_destructive(a, "user-trash", "Delete…", "D", true))
        app_delete_session(a);
}
static void filter_apply(App *a, const char *option) {
    if (!strcmp(option, "project") || !strcmp(option, "date") || !strcmp(option, "none") || !strcmp(option, "environment")) {
        a->group_projects = !strcmp(option, "project");
        a->group_dates = !strcmp(option, "date");
        a->group_environment = !strcmp(option, "environment");
    } else if (ch_str_has_prefix(option, "type:"))
        a->filter_type = atoi(option + 5);
    else if (ch_str_has_prefix(option, "days:"))
        a->activity_days = atoi(option + 5);
    else if (ch_str_has_prefix(option, "folder:")) {
        free(a->filter_project);
        a->filter_project = ch_strdup(option + 7);
    } else if (!strcmp(option, "reset")) {
        a->group_projects = a->group_dates = a->group_environment = a->sort_titles = false;
        a->filter_type = a->activity_days = 0;
        ch_clear_pointer(&a->filter_project, free);
    } else
        a->sort_titles = !strcmp(option, "title");
    prefs_save(a);
    app_refresh_sessions(a);
}
static void choice(App *a, const char *title, const char *option, bool selected) {
    char *copy = ch_strdup(option);
    if (menu_item(a, NULL, title, NULL, selected ? "✓" : NULL, true, NULL))
        filter_apply(a, copy);
    free(copy);
}
static void filter_menu(App *a) {
    const char *types[] = {"All", "Chat", "Code"};
    if (menu_sub_begin(a, NULL, "Type", types[a->filter_type], true, NULL)) {
        for (int i = 0; i < 3; i++) {
            char option[16];
            snprintf(option, sizeof(option), "type:%d", i);
            choice(a, types[i], option, a->filter_type == i);
        }
        menu_sub_end(a);
    }
    char *folder = a->filter_project && *a->filter_project ? ch_path_basename(a->filter_project) : ch_strdup("All");
    if (menu_sub_begin(a, NULL, "Project", folder, true, NULL)) {
        choice(a, "All projects", "folder:", !a->filter_project || !*a->filter_project);
        ChVec *sessions = ch_store_list(a->store, "", NULL);
        ChMap seen;
        ch_map_init(&seen, NULL);
        for (size_t i = 0; sessions && i < sessions->len; i++) {
            ChSession *s = sessions->data[i];
            if (ch_map_has(&seen, s->project))
                continue;
            ch_map_set(&seen, s->project, NULL);
            char *name = ch_path_basename(s->project), *option = ch_strconcat("folder:", s->project, NULL);
            choice(a, name, option, !ch_strcmp0(a->filter_project, s->project));
            free(name);
            free(option);
        }
        ch_map_clear(&seen);
        ch_vec_free(sessions);
        menu_sub_end(a);
    }
    free(folder);
    const char *activity[] = {"All time", "Today", "Last 7 days", "Last 30 days"};
    const int days[] = {0, 1, 7, 30};
    int current = a->activity_days == 1 ? 1 : a->activity_days == 7 ? 2 : a->activity_days == 30 ? 3 : 0;
    if (menu_sub_begin(a, NULL, "Last activity", activity[current], true, NULL)) {
        for (int i = 0; i < 4; i++) {
            char option[16];
            snprintf(option, sizeof(option), "days:%d", days[i]);
            choice(a, activity[i], option, i == current);
        }
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_sub_begin(a, NULL, "Group by", a->group_projects ? "Project" : a->group_dates ? "Date" : "None", true, NULL)) {
        choice(a, "Date", "date", a->group_dates);
        choice(a, "Project", "project", a->group_projects);
        choice(a, "None", "none", !a->group_projects && !a->group_dates);
        menu_sub_end(a);
    }
    if (menu_sub_begin(a, NULL, "Sort by", a->sort_titles ? "Name" : "Last activity", true, NULL)) {
        choice(a, "Last activity", "recent", !a->sort_titles);
        choice(a, "Name", "title", a->sort_titles);
        menu_sub_end(a);
    }
    menu_separator(a);
    choice(a, "Reset to defaults", "reset", false);
}
static void organizer_menu(App *a) {
    if (menu_sub_begin(a, NULL, "Group by",
                       a->group_environment ? "Environment" : a->group_projects ? "Folder" : a->group_dates ? "Date" : "None",
                       true, NULL)) {
        choice(a, "Date", "date", a->group_dates);
        choice(a, "Folder", "project", a->group_projects);
        choice(a, "Environment", "environment", a->group_environment);
        choice(a, "None", "none", !a->group_projects && !a->group_dates && !a->group_environment);
        menu_sub_end(a);
    }
    if (menu_sub_begin(a, NULL, "Sort by", a->sort_titles ? "Name" : "Last updated", true, NULL)) {
        choice(a, "Last updated", "recent", !a->sort_titles);
        choice(a, "Name", "title", a->sort_titles);
        menu_sub_end(a);
    }
    if (menu_sub_begin(a, NULL, "Show", NULL, true, NULL)) {
        choice(a, "All sessions", "type:0", !a->filter_type);
        choice(a, "Chats", "type:1", a->filter_type == 1);
        choice(a, "Code sessions", "type:2", a->filter_type == 2);
        menu_separator(a);
        choice(a, "All time", "days:0", !a->activity_days);
        choice(a, "Last 7 days", "days:7", a->activity_days == 7);
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_item(a, NULL, "New section", NULL, NULL, true, NULL))
        app_create_section(a, false);
    menu_separator(a);
    if (menu_item(a, NULL, "View all", NULL, NULL, true, NULL))
        open_all_sessions(a);
}
static void profile_menu(App *a) {
    menu_heading(a, "Local workspace");
    if (menu_item(a, "preferences-system", "Settings…", NULL, "Ctrl+,", !a->busy, NULL))
        open_settings(a, 0);
    if (menu_item(a, "input-keyboard", "Keyboard shortcuts", NULL, NULL, true, NULL))
        open_shortcuts(a);
    menu_separator(a);
    if (menu_item(a, "help-about", "About Chela", NULL, NULL, true, NULL))
        open_about(a);
    menu_separator(a);
    if (menu_item(a, "application-exit", "Quit", NULL, "Ctrl+Q", true, NULL))
        app_quit(a);
}
static void app_menu(App *a) {
    if (menu_item(a, "list-add", "New session", NULL, "Ctrl+N", !a->busy, NULL))
        app_start_new(a);
    if (menu_item(a, "folder", "Open folder…", NULL, NULL, !a->busy, NULL))
        app_choose_project(a);
    menu_separator(a);
    if (menu_item(a, "sidebar-show", "Toggle sidebar", NULL, "Ctrl+B", true, NULL))
        app_toggle_sidebar(a);
    if (menu_item(a, "preferences-system", "Settings…", NULL, "Ctrl+,", !a->busy, NULL))
        open_settings(a, 0);
    if (menu_item(a, NULL, "Keyboard shortcuts", NULL, NULL, true, NULL))
        open_shortcuts(a);
    menu_separator(a);
    if (menu_item(a, NULL, "About Chela", NULL, NULL, true, NULL))
        open_about(a);
    if (menu_item(a, NULL, "Quit", NULL, "Ctrl+Q", true, NULL))
        app_quit(a);
}
static void more_menu(App *a) {
    menu_item(a, "artifacts", "Artifacts", "Artifact previews are not available yet", NULL, false, NULL);
    menu_item(a, "clock", "Scheduled", "Scheduled tasks are not available yet", NULL, false, NULL);
    menu_separator(a);
    if (menu_item(a, NULL, "Keyboard shortcuts", NULL, NULL, true, NULL))
        open_shortcuts(a);
}
void menu_build(App *a) {
    Menu *m = &a->menu;
    if (!m->kind)
        return;
    if (m->kind == MENU_MODEL_PANEL) {
        model_panel_build(a);
        return;
    }
    int kind = m->kind;
    m->level = 0;
    m->index[0] = 0;
    m->depth = 0;
    m->moved = cu_mouse_x(a->cu) != m->mouse_x || cu_mouse_y(a->cu) != m->mouse_y;
    m->mouse_x = cu_mouse_x(a->cu);
    m->mouse_y = cu_mouse_y(a->cu);
    layer_begin(a, m->anchor, CU_PLACE_BELOW, m->align_end, kind == MENU_FILTER ? 270 : kind == MENU_ORGANIZER ? 214 : 204);
    CuId root = m->layer[0];
    switch (kind) {
    case MENU_APP: app_menu(a); break;
    case MENU_PROJECT: project_menu(a); break;
    case MENU_MODEL: model_menu(a); break;
    case MENU_MODE: mode_menu(a); break;
    case MENU_THINKING: thinking_menu(a); break;
    case MENU_ATTACH: attach_menu(a); break;
    case MENU_VOICE: voice_menu(a); break;
    case MENU_ENVIRONMENT: environment_menu(a); break;
    case MENU_SESSION: session_menu(a); break;
    case MENU_ROW: row_menu(a); break;
    case MENU_FILTER: filter_menu(a); break;
    case MENU_ORGANIZER: organizer_menu(a); break;
    case MENU_PROFILE: profile_menu(a); break;
    case MENU_MORE: more_menu(a); break;
    default: break;
    }
    m->count[0] = m->index[0];
    m->activate_level = -1;
    cu_layer_close(a->cu);
    if (a->menu.kind == kind && cu_layer_dismissed(a->cu, root))
        menu_close(a);
}
