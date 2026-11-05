/* Popup menus: one menu tree is open at a time; submenus open on hover. */
#include "app.h"
#include <ctype.h>
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
    ch_clear_pointer(&m->target, free);
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
    a->menu.at_pointer = false;
    for (int i = 0; i < 4; i++) {
        a->menu.sub[i] = a->menu.cursor[i] = -1;
        a->menu.count[i] = 0;
    }
    a->menu.depth = 0;
    a->menu.activate_level = -1;
    if (kind == MENU_PROJECT)
        a->last_git_poll = 0; /* the Branch row should list what is there now */
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
/* Right click: at the pointer, and about the thing under it. */
void menu_open_here(App *a, int kind, CuId anchor, const char *target) {
    menu_open(a, kind, anchor, false);
    if (a->menu.kind != kind)
        return;
    a->menu.at_pointer = true;
    a->menu.x = cu_mouse_x(a->cu);
    a->menu.y = cu_mouse_y(a->cu);
    a->menu.target = ch_strdup(target);
}

/* Items */
enum { ITEM_ENABLED = 1, ITEM_SUBMENU = 2, ITEM_OPENS_LEFT = 4 };
static CuId item_id(App *a, bool enabled, bool submenu) {
    Menu *m = &a->menu;
    int level = m->level, index = m->index[level]++;
    if (index < MENU_ITEMS) {
        m->flags[level][index] = (enabled ? ITEM_ENABLED : 0) | (submenu ? ITEM_SUBMENU : 0);
        m->keys[level][index] = 0;
    }
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
    /* Room for a model id whole; past that the middle goes, since ends are what tell names apart. */
    cu_text(cu, 0, title,
            &(CuTextStyle){.font = ui_font(a, 14, 400), .color = color, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_FIT_MAX(380)});
    if (detail)
        cu_text(cu, 0, detail,
                &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .wrap = CU_WRAP_WORD, .w = CU_GROW_MAX(280)});
    cu_close(cu);
    if (trailing && ch_str_has_prefix(trailing, "✓")) {
        cu_icon(cu, "check", 16, C_CHECK);
        trailing += strlen("✓");
    }
    if (trailing && *trailing)
        cu_text(cu, 0, trailing,
                &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_MUTED, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_FIT_MAX(170)});
    if (chevron)
        cu_icon(cu, "chevron-right", 14, C_MUTED);
    cu_close(cu);
}
/* Reaching an item closes the submenu open beside it, unless the pointer is on its way
 * there: Effort's submenu rises past the rows above it, and a diagonal crosses them. */
static void item_leaves_submenu(App *a, CuId id, bool enabled) {
    Menu *m = &a->menu;
    int open = m->sub[m->level];
    if (!enabled || !m->moved || !cu_hovered(a->cu, id) || open < 0)
        return;
    bool towards = open < MENU_ITEMS && (m->flags[m->level][open] & ITEM_OPENS_LEFT) && m->mouse_dx < 0;
    if (!towards)
        m->sub[m->level] = -1;
}
/* One letter or digit at the end of a row is that row's key. */
static void item_key(App *a, const char *trailing) {
    Menu *m = &a->menu;
    int index = m->index[m->level] - 1;
    if (trailing && ch_str_has_prefix(trailing, "✓"))
        trailing += strlen("✓");
    if (index < MENU_ITEMS && trailing && isalnum((unsigned char)*trailing) && !trailing[1])
        m->keys[m->level][index] = (uint8_t)tolower((unsigned char)*trailing);
}
bool menu_item(App *a, const char *icon, const char *title, const char *detail, const char *trailing, bool enabled,
               const char *tooltip) {
    CuId id = item_id(a, enabled, false);
    item_key(a, trailing);
    item_body(a, id, icon, title, detail, trailing, enabled, false, false, tooltip);
    item_leaves_submenu(a, id, enabled);
    if (item_chosen(a, id, enabled)) {
        menu_close(a);
        return true;
    }
    return false;
}
bool menu_item_destructive(App *a, const char *icon, const char *title, const char *trailing, bool enabled) {
    CuId id = item_id(a, enabled, false);
    item_key(a, trailing);
    item_body(a, id, icon, title, NULL, trailing, enabled, true, false, NULL);
    item_leaves_submenu(a, id, enabled);
    if (item_chosen(a, id, enabled)) {
        menu_close(a);
        return true;
    }
    return false;
}
/* A setting in a menu. Choosing it leaves the menu open, so the switch shows what happened. */
bool menu_switch(App *a, const char *title, const char *tooltip, bool on, bool enabled) {
    Cu *cu = a->cu;
    CuId id = item_id(a, enabled, false);
    cu_open(cu, id, &(CuBox){.row = true, .gap = 10, CU_PAD2(5, 10), .radius = 5, .w = CU_GROW, .align_y = CU_CENTER,
                             .bg = item_hot(a, id, enabled) ? C_MENU_HOVER : 0, .interactive = true, .tooltip = tooltip});
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = enabled ? C_TEXT : 0xff969696, .w = CU_GROW});
    ui_switch_mark(a, on, enabled);
    cu_close(cu);
    item_leaves_submenu(a, id, enabled);
    return item_chosen(a, id, enabled);
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
                  &(CuLayer){.kind = CU_LAYER_POPUP, .anchor = anchor, .placement = (uint8_t)placement, .x = m->x, .y = m->y,
                             .align_end = align_end, .offset = placement == CU_PLACE_RIGHT || placement == CU_PLACE_LEFT ? 6 : 4},
                  &(CuBox){CU_PAD(5), .bg = C_MENU, .border = 1, .border_color = C_MENU_BORDER, .radius = 12,
                           .shadow = 14, .w = CU_FIT_MIN(min_width)});
}
static bool sub_begin(App *a, const char *icon, const char *title, const char *trailing, bool enabled,
                      const char *tooltip, bool left) {
    Menu *m = &a->menu;
    int level = m->level, index = m->index[level];
    CuId id = item_id(a, enabled, true);
    if (left && index < MENU_ITEMS)
        m->flags[level][index] |= ITEM_OPENS_LEFT;
    item_body(a, id, icon, title, NULL, trailing, enabled, false, true, tooltip);
    if (enabled && ((m->moved && cu_hovered(a->cu, id)) || cu_clicked(a->cu, id)))
        m->sub[level] = index;
    if (!enabled || m->sub[level] != index || level >= 3)
        return false;
    m->level++;
    m->depth = CH_MAX(m->depth, m->level);
    m->index[m->level] = 0;
    m->sub[m->level] = m->sub[m->level] < -1 ? -1 : m->sub[m->level];
    layer_begin(a, id, left ? CU_PLACE_LEFT : CU_PLACE_RIGHT, left, 214);
    return true;
}
bool menu_sub_begin(App *a, const char *icon, const char *title, const char *trailing, bool enabled,
                    const char *tooltip) {
    return sub_begin(a, icon, title, trailing, enabled, tooltip, false);
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
 * Return activates. A submenu that opens to the left swaps those arrows. Escape is
 * handled by the popup layer. */
void menu_keys(App *a) {
    Menu *m = &a->menu;
    Cu *cu = a->cu;
    if (!m->kind || m->kind == MENU_MODEL_PANEL)
        return;
    int level = CH_CLAMP(m->depth, 0, 3), n = CH_MIN(m->count[level], MENU_ITEMS), *cursor = &m->cursor[level];
    int parent = level > 0 ? m->sub[level - 1] : -1;
    bool in_left = parent >= 0 && parent < MENU_ITEMS && (m->flags[level - 1][parent] & ITEM_OPENS_LEFT);
    bool opens_left = *cursor >= 0 && *cursor < n && (m->flags[level][*cursor] & ITEM_OPENS_LEFT);
    /* By the character typed, so that it does not also land in the composer. */
    for (int i = 0; i < n; i++)
        if (m->keys[level][i] && (m->flags[level][i] & ITEM_ENABLED) && cu_typed(cu, m->keys[level][i])) {
            m->activate_level = level;
            m->activate_index = i;
            m->keyed = true;
            cu_invalidate(cu);
            return;
        }
    if (cu_key(cu, CU_KEY_DOWN, 0))
        *cursor = next_item(m, level, *cursor < 0 ? -1 : *cursor, 1);
    else if (cu_key(cu, CU_KEY_UP, 0))
        *cursor = next_item(m, level, *cursor < 0 ? n : *cursor, -1);
    else if (cu_key(cu, CU_KEY_HOME, 0))
        *cursor = next_item(m, level, -1, 1);
    else if (cu_key(cu, CU_KEY_END, 0))
        *cursor = next_item(m, level, n, -1);
    else if (level > 0 && cu_key(cu, in_left ? CU_KEY_RIGHT : CU_KEY_LEFT, 0)) {
        m->sub[level - 1] = -1;
        *cursor = -1;
    } else if (*cursor >= 0 && *cursor < n &&
               (cu_key(cu, CU_KEY_RIGHT, 0) || cu_key(cu, CU_KEY_RETURN, 0) || (opens_left && cu_key(cu, CU_KEY_LEFT, 0)))) {
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
    m->keyed = true;
    cu_invalidate(cu);
}

/* Menu contents */
static void enable_bypass(App *a, void *data) {
    (void)data;
    a->bypass_enabled = true;
    app_set_mode(a, CH_BYPASS);
}
/* Bypass waits behind Enable: one stray click should not turn every question off. */
static void mode_menu(App *a) {
    menu_heading(a, "Mode");
    const ChMode modes[] = {CH_AUTO, CH_ASK, CH_ACCEPT_EDITS, CH_PLAN};
    const char *details[] = {"Chela handles permission decisions", "Always ask before making changes",
                             "Automatically accept all file edits", "Create a plan before making changes"};
    const char *tooltips[] = {a->config.provider == 5
                                  ? "Reading and editing in this session run. Commands run if they change nothing; "
                                    "the rest ask, because a harness runs outside Chela's sandbox."
                                  : "Edits and sandboxed commands run. Pushing, publishing, deleting folders and "
                                    "anything the sandbox can't hold ask first.",
                              NULL, NULL, NULL};
    for (int i = 0; i < 4; i++) {
        char *key = ch_strdup_printf("%s%d", a->mode == modes[i] ? "✓" : "", i + 1);
        if (menu_item(a, NULL, ch_mode_name(modes[i]), details[i], key, true, tooltips[i]))
            app_set_mode(a, modes[i]);
        free(key);
    }
    bool offered = a->bypass_enabled || a->mode == CH_BYPASS;
    if (menu_item(a, NULL, ch_mode_name(CH_BYPASS), offered ? "Run edits and commands without asking" : NULL,
                  !offered ? "Enable" : a->mode == CH_BYPASS ? "✓5" : "5", true, NULL)) {
        if (offered)
            app_set_mode(a, CH_BYPASS);
        else
            dialog_confirm(a, "Enable Bypass permissions?",
                           "Edits and commands will run without asking, including ones that push, publish or delete. "
                           "Chela's own commands stay inside this session's folders. An ACP harness's do not.",
                           "Enable", true, enable_bypass, NULL);
    }
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
/* Recent project folders, the selected one first. Chats have none. */
ChVec *recent_projects(App *a, size_t limit) {
    ChVec *out = ch_vec_new(free);
    if (*a->project)
        ch_vec_push(out, ch_strdup(a->project));
    ChVec *sessions = ch_store_list(a->store, "", NULL);
    for (size_t i = 0; sessions && i < sessions->len && out->len < limit; i++) {
        ChSession *s = sessions->data[i];
        /* A worktree stands for the repository it was made from. */
        char *project = git_in_worktree(a, s->project) ? git_worktree_origin(a, s->project) : ch_strdup(s->project);
        bool seen = !s->code || !project || !*project;
        for (size_t k = 0; k < out->len && !seen; k++)
            seen = !strcmp(out->data[k], project);
        if (seen)
            free(project);
        else
            ch_vec_push(out, project);
    }
    ch_vec_free(sessions);
    return out;
}
static void attach_menu(App *a) {
    if (menu_item(a, "document-open", "Add files or photos", NULL, "Ctrl+U", true, "Or paste them, or drop them on the window"))
        app_attach_file(a);
    if (!a->code)
        return;
    if (menu_item(a, "folder", "Add folder", NULL, NULL, !a->busy, "Let the agent work in another folder too"))
        app_choose_folder(a);
    if (*a->project && menu_sub_begin(a, "skills", "Skills", NULL, true, NULL)) {
        skills_menu(a);
        menu_sub_end(a);
    }
}
/* Another folder in the session, with the way to take it out again. */
static bool included_row(App *a, size_t index) {
    Cu *cu = a->cu;
    const char *path = a->folders.data[index];
    char *name = ch_path_basename(path);
    cu_open(cu, cu_idn(cu, "included-folder", (int64_t)index),
            &(CuBox){.row = true, .gap = 10, .w = CU_GROW, .align_y = CU_CENTER, .pad_l = 10, .pad_r = 6, .pad_t = 3, .pad_b = 3, .tooltip = path});
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    bool removed = ui_icon_button_sized(a, cu_idn(cu, "remove-folder", (int64_t)index), "window-close", 14, 3, "Remove from this session", !a->busy, false);
    cu_close(cu);
    free(name);
    return removed;
}
/* The current branch, then the others, last committed first; a branch that is checked out
 * in another worktree can't be switched to. The list scrolls: repositories can have hundreds. */
static void branch_menu(App *a) {
    Cu *cu = a->cu;
    Menu *m = &a->menu;
    menu_heading(a, "Switch branch");
    CuId list = cu_id(cu, "branch-list");
    /* Limited in width too: a menu as wide as a long name would open over the row it came from. */
    cu_open(cu, list, &(CuBox){.w = CU_FIT_MAX(320), .h = CU_FIT_MAX(340), .scroll_y = true});
    char *current = ch_strdup(a->git_branch);
    menu_item(a, NULL, current, NULL, "✓", true, current);
    size_t shown = 1, others = 0;
    for (size_t i = 0; i < a->git_branches.len; i++) {
        char *name = ch_strdup(a->git_branches.data[i]), *elsewhere = strchr(name, '\t');
        if (elsewhere)
            *elsewhere++ = 0;
        if (strcmp(name, current) && shown++ < MENU_ITEMS - 1) {
            bool free_to_use = !elsewhere || !*elsewhere;
            char *tooltip = free_to_use ? ch_strdup(name) : ch_strconcat("Checked out in ", elsewhere, NULL);
            if (menu_item(a, NULL, name, NULL, NULL, free_to_use, tooltip))
                git_switch_branch(a, name);
            free(tooltip);
        } else if (strcmp(name, current))
            others++;
        free(name);
    }
    free(current);
    cu_close(cu);
    if (others) {
        char *more = ch_strdup_printf("%zu older branch%s", others, others == 1 ? "" : "es");
        menu_item(a, NULL, more, NULL, NULL, false, NULL);
        free(more);
    }
    /* The arrow keys can go past what the list shows. */
    int cursor = m->cursor[m->level];
    if (m->keyed && cursor >= 0) {
        CuRect item = cu_rect(cu, cu_idn(cu, "#menu-item", m->level * 1000 + cursor)), view = cu_rect(cu, list);
        if (item.y < view.y)
            cu_scroll_by(cu, list, item.y - view.y);
        else if (item.y + item.h > view.y + view.h)
            cu_scroll_by(cu, list, item.y + item.h - view.y - view.h);
    }
}
static void project_menu(App *a) {
    if (menu_item(a, NULL, "No folder", NULL, *a->project ? NULL : "✓", true, "Work without files") && !a->busy) {
        app_set_project(a, "");
        cu_focus(a->cu, cu_id(a->cu, "composer"));
    }
    ChVec *projects = recent_projects(a, 7);
    if (projects->len) {
        menu_separator(a);
        menu_heading(a, "Recent");
    }
    for (size_t i = 0; i < projects->len; i++) {
        const char *path = projects->data[i];
        char *name = ch_path_basename(path);
        bool current = !strcmp(path, a->project);
        if (menu_item(a, NULL, name, NULL, current ? "✓" : NULL, true, path) && !a->busy) {
            if (!ch_file_is_dir(path))
                app_notice(a, "Project folder is missing", path);
            else if (!current)
                app_set_project(a, path);
            cu_focus(a->cu, cu_id(a->cu, "composer"));
        }
        free(name);
    }
    ch_vec_free(projects);
    menu_separator(a);
    if (menu_item(a, "folder-open", "Open folder…", NULL, NULL, true, NULL))
        app_choose_project(a);
    if (!*a->project)
        return;
    menu_separator(a);
    menu_heading(a, "Also included");
    size_t removed = SIZE_MAX;
    for (size_t i = 0; i < a->folders.len; i++)
        if (included_row(a, i))
            removed = i;
    if (removed != SIZE_MAX)
        app_remove_folder(a, removed);
    if (menu_item(a, "list-add", "Add folder…", NULL, NULL, !a->busy, "Let the agent work in another folder too"))
        app_choose_folder(a);
    if (!a->git_branch)
        return;
    menu_separator(a);
    if (menu_sub_begin(a, "git-branch", "Branch", a->git_branch, !a->busy && !a->git_switching, "Switch branch")) {
        branch_menu(a);
        menu_sub_end(a);
    }
}
static void environment_menu(App *a) {
    menu_heading(a, "Environment");
    menu_item(a, "computer", "This computer", "Project files and commands run here", "✓", false, NULL);
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
    if (app_save_config(a, &config, "Could not select model"))
        model_tick(a);
}
static void select_effort(App *a, ChThinking level) {
    if (a->busy)
        return;
    ChConfig config = a->config;
    config.thinking = level;
    app_save_config(a, &config, "Could not save effort");
}
/* Opens to the left: the model button sits at the right of the window. */
static void effort_menu(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(5, 10)});
    ui_label_wrap(a, "More effort can help with hard tasks, but takes longer and shares your response limit.", 12, C_MUTED, 250);
    cu_close(cu);
    ChThinking current = ch_thinking_effective(&a->config);
    bool always_reasons = ch_model_is_gpt_oss(&a->config);
    for (int level = CH_THINK_DEFAULT; level <= CH_THINK_DEEP; level++) {
        if (level == CH_THINK_OFF && always_reasons)
            continue;
        if (menu_item(a, NULL, ch_thinking_name((ChThinking)level), NULL, (int)current == level ? "✓" : NULL, true, NULL))
            select_effort(a, (ChThinking)level);
    }
}
/* Rows are names alone. A line under one is for what stops it being used, or to tell
 * it from a model that shortens to the same name. */
static char *model_note(json_object *models, json_object *model) {
    json_object *status = ch_json_get(model, "status"), *failed = ch_json_get(status, "failed");
    if ((failed && json_object_get_boolean(failed)) || !strcmp(ch_json_str(status, "value"), "loading"))
        return ch_strdup(ch_model_detail(model));
    const char *id = ch_json_str(model, "id");
    char *family = ch_model_family_name(id);
    bool shared = false;
    for (size_t i = 0; i < json_object_array_length(models) && !shared; i++) {
        const char *other = ch_json_str(json_object_array_get_idx(models, i), "id");
        char *name = ch_model_family_name(other);
        shared = strcmp(other, id) && !strcmp(name, family);
        free(name);
    }
    free(family);
    return shared ? ch_model_short_name(id) : NULL;
}
static void model_row(App *a, json_object *models, json_object *model, const char *id, bool current) {
    char *copy = ch_strdup(id), *name = ch_model_family_name(id), *note = model ? model_note(models, model) : NULL;
    if (menu_item(a, NULL, name, note, current ? "✓" : NULL, true, copy))
        select_model(a, copy);
    free(copy);
    free(name);
    free(note);
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
    json_object *models = a->menu.models ? json_object_get(a->menu.models) : NULL, *selected = NULL;
    for (size_t i = 0; models && i < json_object_array_length(models); i++) {
        json_object *model = json_object_array_get_idx(models, i);
        if (!strcmp(ch_json_str(model, "id"), a->config.model))
            selected = model;
    }
    char *current = ch_strdup(a->config.model);
    model_row(a, models, selected, current, true);
    size_t count = 0;
    for (size_t i = 0; models && i < json_object_array_length(models); i++) {
        json_object *model = json_object_array_get_idx(models, i);
        const char *id = ch_json_str(model, "id");
        if (!*id || strlen(id) >= sizeof(a->config.model) || !strcmp(id, current))
            continue;
        model_row(a, models, model, id, false);
        count++;
    }
    free(current);
    json_object_put(models);
    if (a->menu.models_loading)
        menu_item(a, NULL, "Discovering models…", NULL, NULL, false, NULL);
    else if (a->menu.models_error) {
        menu_item(a, NULL, "Could not reach model server", "Check your inference computer", NULL, false, a->menu.models_error);
        if (menu_item(a, "network-server", "Manage connection…", NULL, NULL, true, NULL))
            open_settings(a, 1);
    } else if (!count)
        menu_item(a, NULL, "No other models found", NULL, NULL, false, NULL);
    bool effort = ch_thinking_supported(&a->config), preload = model_control_supported(a);
    if (effort || preload)
        menu_separator(a);
    if (effort && sub_begin(a, NULL, "Effort", ch_thinking_name(ch_thinking_effective(&a->config)), true, NULL, true)) {
        effort_menu(a);
        menu_sub_end(a);
    }
    if (preload && menu_switch(a, "Pre-load model", model_preload_hint(a), a->preload, model_preload_enabled(a)))
        model_preload_set(a, !a->preload);
}
static void archive_one(App *a, const char *id) {
    ChVec *ids = ch_vec_new(free);
    ch_vec_push(ids, ch_strdup(id));
    app_archive_sessions(a, ids);
    ch_vec_free(ids);
}
static void session_menu(App *a) {
    bool pinned = a->id && ch_map_has(&a->pinned, a->id);
    if (menu_item(a, "pin", pinned ? "Unpin" : "Pin", NULL, NULL, a->id != NULL, NULL))
        app_toggle_pin(a, a->id);
    if (menu_item(a, "document-edit", "Rename…", NULL, NULL, a->id != NULL, NULL))
        app_rename_session(a);
    if (menu_item(a, "document-save", "Export conversation…", NULL, NULL, a->id != NULL, NULL))
        app_export(a);
    if (a->code) {
        if (menu_item(a, "view-list", "View project changes", NULL, NULL, *a->project != 0, NULL))
            app_show_changes(a);
        menu_separator(a);
        if (menu_sub_begin(a, "system-users", "Interaction", NULL, true, NULL)) {
            interaction_menu(a);
            menu_sub_end(a);
        }
    }
    menu_separator(a);
    if (menu_item(a, "archive", "Archive", NULL, NULL, a->id != NULL, NULL))
        archive_one(a, a->id);
    if (menu_item_destructive(a, "user-trash", "Delete session…", NULL, a->id != NULL))
        app_delete_session(a);
}
/* Right clicking looks at a session rather than opening it. Only renaming and deleting
 * need it on screen, and they open it when they are chosen. */
static void row_menu(App *a) {
    char *id = ch_strdup(a->menu.target);
    bool pinned = ch_map_has(&a->pinned, id);
    if (menu_item(a, "pin", pinned ? "Unpin" : "Pin", NULL, "P", true, NULL))
        app_toggle_pin(a, id);
    if (menu_item(a, "document-edit", "Rename…", NULL, "R", !a->busy, NULL)) {
        app_select_session(a, id);
        app_rename_session(a);
    }
    if (menu_sub_begin(a, NULL, "Move to", NULL, true, NULL)) {
        if (menu_item(a, NULL, "Recents", NULL, NULL, true, NULL))
            app_move_to_section(a, id, NULL);
        const char *current = ch_map_get(&a->session_sections, id);
        for (size_t i = 0; i < a->sections.len; i++) {
            const char *name = a->sections.data[i];
            char *copy = ch_strdup(name);
            if (menu_item(a, NULL, copy, NULL, !ch_strcmp0(current, name) ? "✓" : NULL, true, NULL))
                app_move_to_section(a, id, copy);
            free(copy);
        }
        menu_separator(a);
        if (menu_item(a, "list-add", "New section…", NULL, NULL, !a->busy, NULL)) {
            app_select_session(a, id);
            app_create_section(a, true);
        }
        menu_sub_end(a);
    }
    menu_separator(a);
    if (menu_item(a, "archive", "Archive", NULL, "A", !a->busy, NULL))
        archive_one(a, id);
    if (menu_item_destructive(a, "user-trash", "Delete…", "D", !a->busy)) {
        app_select_session(a, id);
        app_delete_session(a);
    }
    free(id);
}
/* A folder's heading in the sidebar. */
static void group_menu(App *a) {
    char *project = ch_strdup(a->menu.target), *name = *project ? ch_path_basename(project) : ch_strdup("No folder");
    ChVec *groups = app_project_groups(a), *sessions = app_project_sessions(a, project);
    size_t place = 0, count = sessions->len;
    while (place < groups->len && strcmp(groups->data[place], project))
        place++;
    bool above = place > 0, below = place + 1 < groups->len;
    if (menu_item(a, NULL, "New session", NULL, NULL, !a->busy, NULL)) {
        if (!*project || ch_file_is_dir(project))
            app_set_project(a, project);
        else
            app_notice(a, "Project folder is missing", project);
    }
    if (above || below)
        menu_separator(a);
    if (above && menu_item(a, NULL, "Move up", NULL, NULL, true, NULL))
        app_move_project(a, project, -1);
    if (below && menu_item(a, NULL, "Move down", NULL, NULL, true, NULL))
        app_move_project(a, project, 1);
    menu_separator(a);
    if (menu_item(a, NULL, "Select all", NULL, NULL, true, "Then right click one of them to archive or delete them together"))
        app_select_project(a, project);
    menu_separator(a);
    if (menu_item(a, NULL, "Collapse all", NULL, NULL, sidebar_can_fold(a, true), NULL))
        sidebar_fold_all(a, true);
    if (menu_item(a, NULL, "Expand all", NULL, NULL, sidebar_can_fold(a, false), NULL))
        sidebar_fold_all(a, false);
    menu_separator(a);
    char *archive = ch_strdup_printf("Archive all (%zu)", count), *remove = ch_strdup_printf("Delete all sessions (%zu)", count);
    if (menu_item(a, NULL, archive, NULL, NULL, !a->busy, NULL))
        app_archive_sessions(a, sessions);
    if (menu_item_destructive(a, NULL, remove, NULL, !a->busy))
        app_delete_sessions(a, sessions, name);
    free(archive);
    free(remove);
    ch_vec_free(groups);
    ch_vec_free(sessions);
    free(name);
    free(project);
}
static void selection_menu(App *a) {
    ChVec *ids = app_selected_sessions(a);
    size_t count = ids->len;
    char *archive = ch_strdup_printf("Archive %zu sessions", count), *remove = ch_strdup_printf("Delete %zu sessions…", count);
    if (menu_item(a, "archive", archive, NULL, NULL, !a->busy, NULL))
        app_archive_sessions(a, ids);
    if (menu_item_destructive(a, "user-trash", remove, NULL, !a->busy))
        app_delete_sessions(a, ids, NULL);
    menu_separator(a);
    if (menu_item(a, NULL, "Clear selection", NULL, "Esc", true, NULL))
        app_clear_selection(a);
    free(archive);
    free(remove);
    ch_vec_free(ids);
}
static void filter_apply(App *a, const char *option) {
    if (!strcmp(option, "project") || !strcmp(option, "date") || !strcmp(option, "none") || !strcmp(option, "environment")) {
        a->group_projects = !strcmp(option, "project");
        a->group_dates = !strcmp(option, "date");
        a->group_environment = !strcmp(option, "environment");
    } else if (ch_str_has_prefix(option, "days:"))
        a->activity_days = atoi(option + 5);
    else if (!strcmp(option, "reset")) {
        a->group_projects = a->group_dates = a->group_environment = a->sort_titles = false;
        a->activity_days = 0;
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
    if (menu_sub_begin(a, NULL, "Group by", a->group_dates ? "Date" : "None", true, NULL)) {
        choice(a, "Date", "date", a->group_dates);
        choice(a, "None", "none", !a->group_dates);
        menu_sub_end(a);
    }
    if (menu_sub_begin(a, NULL, "Sort by", a->sort_titles ? "Name" : "Last activity", true, NULL)) {
        choice(a, "Last activity", "recent", !a->sort_titles);
        choice(a, "Name", "title", a->sort_titles);
        menu_sub_end(a);
    }
    menu_separator(a);
    choice(a, "Reset to defaults", "reset", false);
    menu_separator(a);
    if (menu_item(a, NULL, "View all", NULL, NULL, true, NULL))
        open_all_sessions(a);
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
    if (menu_item(a, "folder-open", "Open folder…", NULL, NULL, !a->busy, NULL))
        app_choose_project(a);
    menu_separator(a);
    if (menu_item(a, "sidebar-show", "Toggle sidebar", NULL, "Ctrl+B", true, NULL))
        app_toggle_sidebar(a);
    if (menu_item(a, "preferences-system", "Settings…", NULL, "Ctrl+,", !a->busy, NULL))
        open_settings(a, 0);
    if (menu_item(a, "input-keyboard", "Keyboard shortcuts", NULL, NULL, true, NULL))
        open_shortcuts(a);
    menu_separator(a);
    if (menu_item(a, "help-about", "About Chela", NULL, NULL, true, NULL))
        open_about(a);
    if (menu_item(a, "application-exit", "Quit", NULL, "Ctrl+Q", true, NULL))
        app_quit(a);
}
static void more_menu(App *a) {
    if (menu_sub_begin(a, NULL, "Group and sort", NULL, true, NULL)) {
        if (a->code)
            organizer_menu(a);
        else
            filter_menu(a);
        menu_sub_end(a);
    }
    if (menu_item(a, "input-keyboard", "Keyboard shortcuts", NULL, NULL, true, NULL))
        open_shortcuts(a);
    if (menu_item(a, "help-about", "About Chela", NULL, NULL, true, NULL))
        open_about(a);
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
    m->mouse_dx = cu_mouse_x(a->cu) - m->mouse_x;
    m->mouse_x = cu_mouse_x(a->cu);
    m->mouse_y = cu_mouse_y(a->cu);
    layer_begin(a, m->anchor, m->at_pointer ? CU_PLACE_POINT : CU_PLACE_BELOW, m->align_end,
                kind == MENU_FILTER ? 270 : kind == MENU_MODEL ? 240 : kind == MENU_ORGANIZER ? 214 : 204);
    CuId root = m->layer[0];
    switch (kind) {
    case MENU_APP: app_menu(a); break;
    case MENU_PROJECT: project_menu(a); break;
    case MENU_MODEL: model_menu(a); break;
    case MENU_MODE: mode_menu(a); break;
    case MENU_ATTACH: attach_menu(a); break;
    case MENU_ENVIRONMENT: environment_menu(a); break;
    case MENU_SESSION: session_menu(a); break;
    case MENU_ROW: row_menu(a); break;
    case MENU_GROUP: group_menu(a); break;
    case MENU_SELECTION: selection_menu(a); break;
    case MENU_FILTER: filter_menu(a); break;
    case MENU_ORGANIZER: organizer_menu(a); break;
    case MENU_PROFILE: profile_menu(a); break;
    case MENU_MORE: more_menu(a); break;
    default: break;
    }
    m->count[0] = m->index[0];
    m->activate_level = -1;
    m->keyed = false;
    cu_layer_close(a->cu);
    if (a->menu.kind == kind && cu_layer_dismissed(a->cu, root))
        menu_close(a);
}
