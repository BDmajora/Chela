/* Sidebar, window title rail and top bar. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>


static ChMap collapsed_sections;

static bool csd(App *a) {
    return a->p->window_has_client_decorations && a->p->window_has_client_decorations(a->window);
}
static bool nav_button(App *a, const char *name, const char *icon, const char *text, const char *tooltip, bool enabled) {
    Cu *cu = a->cu;
    CuId id = cu_id(cu, name);
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(1, 8)});
    cu_open(cu, id,
            &(CuBox){.row = true, .gap = 10, .align_y = CU_CENTER, CU_PAD2(6, 8), .radius = 5, .w = CU_GROW,
                     .bg = enabled && cu_hovered(cu, id) ? C_BUTTON : 0, .interactive = true, .focusable = enabled,
                     .tooltip = tooltip, .opacity = enabled ? 0 : 0.45f,
                     .border = cu_focus_visible(cu, id) ? 1.5f : 0, .border_color = C_FOCUS});
    cu_icon(cu, icon, 17, 0xffd4d4d4);
    cu_text(cu, 0, text, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT});
    cu_close(cu);
    cu_close(cu);
    return enabled && cu_clicked(cu, id);
}
static void tabs(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.row = true, .bg = 0xff292929, .radius = 6, CU_PAD(1)});
    const char *icons[] = {"user-available", "utilities-terminal"};
    const char *tips[] = {"Chat (Ctrl+1)", "Code (Ctrl+2)"};
    for (int i = 0; i < 2; i++) {
        CuId id = cu_idn(cu, "mode-tab", i);
        bool active = a->code == (i == 1);
        cu_open(cu, id,
                &(CuBox){CU_PAD2(5, 9), .radius = 5,
                         .bg = active ? 0xff3e3e3e : cu_hovered(cu, id) && !a->busy ? 0xff333333 : 0,
                         .border = active ? 1 : 0, .border_color = 0xff4c4c4c, .interactive = true,
                         .tooltip = tips[i], .opacity = a->busy ? 0.5f : 0});
        cu_icon(cu, icons[i], 18, 0xffd4d4d4);
        cu_close(cu);
        if (cu_clicked(cu, id) && !a->busy) {
            app_set_code(a, i == 1);
            cu_focus(cu, cu_id(cu, "composer"));
        }
    }
    cu_close(cu);
}
static void sidebar_rail(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, cu_id(cu, "sidebar-rail"),
            &(CuBox){.row = true, .w = CU_GROW, .h = CU_PX(36), CU_PAD2(3, 8), .gap = 1, .align_y = CU_CENTER,
                     .drag_window = csd(a)});
    CuId menu = cu_id(cu, "app-menu");
    if (ui_icon_button(a, menu, "app-menu", "Application menu", true))
        menu_open(a, MENU_APP, menu, false);
    if (ui_icon_button(a, cu_id(cu, "sidebar-toggle"), "sidebar-show", "Toggle sidebar (Ctrl+B)", true))
        app_toggle_sidebar(a);
    int current = -1;
    for (size_t i = 0; a->rows && i < a->rows->len; i++)
        if (!ch_strcmp0(a->id, ((ChSession *)a->rows->data[i])->id))
            current = (int)i;
    bool back = !a->busy && a->rows && a->rows->len && current < (int)a->rows->len - 1, forward = !a->busy && current >= 0;
    if (ui_icon_button(a, cu_id(cu, "nav-back"), "go-previous", "Previous session", back))
        app_navigate(a, 1);
    if (ui_icon_button(a, cu_id(cu, "nav-forward"), "go-next", "Next session", forward))
        app_navigate(a, -1);
    cu_spacer(cu, CU_GROW, CU_PX(1));
    tabs(a);
    cu_close(cu);
}
static char *date_heading(int64_t updated) {
    time_t t = (time_t)(updated / CH_USEC_PER_SEC);
    struct tm tm;
    if (!localtime_r(&t, &tm))
        return ch_strdup("Unknown date");
    char buf[64];
    strftime(buf, sizeof(buf), "%B %-d, %Y", &tm);
    return ch_strdup(buf);
}
static void session_heading(App *a, const char *text) {
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 20, .pad_t = 10, .pad_b = 6});
    cu_text(a->cu, 0, text, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .ellipsize = CU_ELLIPSIZE_END});
    cu_close(a->cu);
}
/* A session entry; returns true when it was activated. Right click opens its menu. */
static void session_item(App *a, ChSession *s, const char *scope, bool dot, float pad_left) {
    Cu *cu = a->cu;
    CuId id = cu_ids(cu, scope, s->id), more = cu_ids(cu, "row-more", s->id);
    bool selected = !ch_strcmp0(a->id, s->id), hot = cu_hovered(cu, id) || cu_hovered(cu, more);
    bool marked = ch_map_has(&a->selection, s->id);
    hot |= a->menu.kind == MENU_ROW && !ch_strcmp0(a->menu.target, s->id); /* the row its menu is about */
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(1, 8)});
    cu_open(cu, id,
            &(CuBox){.row = true, .gap = 7, .align_y = CU_CENTER, .pad_l = pad_left, .pad_r = 6, .pad_t = 5, .pad_b = 5,
                     .radius = 5, .w = CU_GROW, .h = CU_FIT_MIN(30),
                     .bg = selected || marked ? C_SELECTED : hot && !a->busy ? C_HOVER : 0, .interactive = true,
                     .tooltip = s->title, .opacity = a->busy && !selected ? 0.6f : 0});
    if (dot)
        cu_icon(cu, "session-idle", 14, 0x73d4d4d4);
    cu_text(cu, 0, s->title, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    if (marked)
        cu_icon(cu, "check", 14, C_CHECK);
    if ((hot || selected) && !a->busy) {
        if (ui_icon_button_sized(a, more, "view-more", 16, 1, "Session actions", true, false))
            menu_open_here(a, MENU_ROW, more, s->id);
    }
    cu_close(cu);
    cu_close(cu);
    if (cu_right_clicked(cu, id))
        menu_open_here(a, marked && a->selection.len > 1 ? MENU_SELECTION : MENU_ROW, id, s->id);
    if (a->busy)
        return;
    if (cu_clicked(cu, id)) {
        if (cu_mods(cu) & CU_MOD_CTRL) {
            if (!ch_map_remove(&a->selection, s->id))
                ch_map_set(&a->selection, s->id, NULL);
            cu_invalidate(cu);
        } else {
            app_clear_selection(a);
            app_select_session(a, s->id);
        }
    }
}
static void heading_button(App *a, const char *name, const char *text, bool expanded, bool *toggle) {
    Cu *cu = a->cu;
    CuId id = cu_id(cu, name);
    cu_open(cu, id, &(CuBox){.row = true, .gap = 4, .align_y = CU_CENTER, CU_PAD2(3, 4), .radius = 5,
                             .bg = cu_hovered(cu, id) ? C_HOVER : 0, .interactive = true});
    cu_text(cu, 0, text, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = 0xffa5a5a5});
    cu_icon(cu, expanded ? "chevron-down" : "chevron-right", 12, 0xffa5a5a5);
    cu_close(cu);
    if (cu_clicked(cu, id)) {
        *toggle = !*toggle;
        prefs_save(a);
    }
}
/* What a group of sessions is headed by. A worktree has its repository's name, so it says what it is. */
static char *project_heading(App *a, const char *project) {
    char *base = *project ? ch_path_basename(project) : ch_strdup("No folder");
    if (!git_in_worktree(a, project))
        return base;
    char *heading = ch_strconcat(base, " · worktree", NULL);
    free(base);
    return heading;
}
/* One project's sessions in the Code tab, under a heading that folds them away. */
static void project_group(App *a, const char *project, bool top) {
    Cu *cu = a->cu;
    char *base = project_heading(a, project);
    CuId id = cu_ids(cu, "project-group", project), row = cu_ids(cu, "project-row", project), add = cu_ids(cu, "project-new", project);
    bool collapsed = ch_map_has(&collapsed_sections, project);
    cu_open(cu, row, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .pad_l = 12, .pad_r = 16, .pad_t = 10, .pad_b = 3, .gap = 1,
                              .interactive = true});
    cu_open(cu, id, &(CuBox){.row = true, .gap = 4, .align_y = CU_CENTER, CU_PAD2(3, 4), .radius = 5, .tooltip = project,
                             .bg = cu_hovered(cu, id) ? C_HOVER : 0, .interactive = true});
    cu_text(cu, 0, base, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = 0xffa5a5a5, .ellipsize = CU_ELLIPSIZE_END, .w = CU_FIT_MAX(200)});
    cu_icon(cu, collapsed ? "chevron-right" : "chevron-down", 12, 0xffa5a5a5);
    cu_close(cu);
    cu_spacer(cu, CU_GROW, CU_PX(1));
    if (top || cu_hovered_within(cu, row)) {
        if (ui_icon_button_sized(a, add, "list-add", 16, 3, "New session in this project", !a->busy, false)) {
            if (!*project || ch_file_is_dir(project))
                app_set_project(a, project);
            else
                app_notice(a, "Project folder is missing", project);
        }
    } else
        cu_spacer(cu, CU_PX(22), CU_PX(22));
    cu_close(cu);
    if (cu_clicked(cu, id)) {
        if (collapsed)
            ch_map_remove(&collapsed_sections, project);
        else
            ch_map_set(&collapsed_sections, project, NULL);
    }
    if (cu_right_clicked(cu, id) || cu_right_clicked(cu, row))
        menu_open_here(a, MENU_GROUP, row, project);
    free(base);
    if (collapsed)
        return;
    for (size_t i = 0; i < a->rows->len; i++) {
        ChSession *s = a->rows->data[i];
        if (!strcmp(s->project, project) && !ch_map_has(&a->pinned, s->id))
            session_item(a, s, "row", false, 12);
    }
}
/* Every heading in the sidebar: the folders, and the sections made by hand. */
static void fold_walk(App *a, bool fold, bool apply, bool *can) {
    ChVec *groups = app_project_groups(a);
    size_t total = groups->len + a->sections.len;
    for (size_t i = 0; i < total; i++) {
        const char *name = i < groups->len ? groups->data[i] : a->sections.data[i - groups->len];
        *can |= ch_map_has(&collapsed_sections, name) != fold;
        if (apply && fold)
            ch_map_set(&collapsed_sections, name, NULL);
        else if (apply)
            ch_map_remove(&collapsed_sections, name);
    }
    ch_vec_free(groups);
}
void sidebar_fold_all(App *a, bool fold) {
    bool can = false;
    fold_walk(a, fold, true, &can);
    cu_invalidate(a->cu);
}
bool sidebar_can_fold(App *a, bool fold) {
    bool can = false;
    fold_walk(a, fold, false, &can);
    return can;
}
static void code_history(App *a) {
    Cu *cu = a->cu;
    ChVec *groups = app_project_groups(a);
    for (size_t i = 0; i < groups->len; i++)
        project_group(a, groups->data[i], i == 0);
    bool any = groups->len > 0;
    ch_vec_free(groups);
    if (!any) {
        cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .pad_l = 16, .pad_r = 16, .pad_t = 12, .pad_b = 6});
        cu_text(cu, 0, "Sessions", &(CuTextStyle){.font = ui_font(a, 12, 400), .color = 0xffa5a5a5, .w = CU_GROW});
        cu_close(cu);
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 20, .pad_t = 6, .pad_r = 16});
        ui_label_wrap(a, "Your coding sessions will appear here.", 12, C_FAINT, 0);
        cu_close(cu);
    }
}
static void history(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, cu_id(cu, "history"), &(CuBox){.w = CU_GROW, .h = CU_GROW, .scroll_y = true, .pad_b = 8});
    if (!collapsed_sections.keys)
        ch_map_init(&collapsed_sections, NULL);
    /* Pinned */
    int pinned = 0;
    for (size_t i = 0; a->rows && i < a->rows->len; i++)
        pinned += ch_map_has(&a->pinned, ((ChSession *)a->rows->data[i])->id);
    bool show_pinned = pinned && !a->pinned_collapsed;
    bool collapsed = a->pinned_collapsed;
    if (pinned || !a->code) {
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 12, .pad_t = 18, .pad_b = 2});
        heading_button(a, "pinned-heading", "Pinned", show_pinned, &collapsed);
        if (collapsed != a->pinned_collapsed) {
            a->pinned_collapsed = collapsed;
            prefs_save(a);
        }
        cu_close(cu);
    }
    if (show_pinned)
        for (size_t i = 0; i < a->rows->len; i++) {
            ChSession *s = a->rows->data[i];
            if (ch_map_has(&a->pinned, s->id))
                session_item(a, s, "pinned", false, 26);
        }
    /* Custom sections */
    for (size_t k = 0; k < a->sections.len; k++) {
        const char *name = a->sections.data[k];
        bool expanded = !ch_map_has(&collapsed_sections, name);
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 14, .pad_r = 16, .pad_t = 10, .pad_b = 4});
        bool was = expanded;
        ui_expander(a, cu_ids(cu, "section", name), name, &expanded);
        cu_close(cu);
        if (was != expanded) {
            if (expanded)
                ch_map_remove(&collapsed_sections, name);
            else
                ch_map_set(&collapsed_sections, name, NULL);
        }
        if (!expanded)
            continue;
        for (size_t i = 0; a->rows && i < a->rows->len; i++) {
            ChSession *s = a->rows->data[i];
            if (!ch_map_has(&a->pinned, s->id) && !ch_strcmp0(ch_map_get(&a->session_sections, s->id), name))
                session_item(a, s, "section-row", false, 12);
        }
    }
    /* Recents; in the Code tab, sessions sit under their projects instead. */
    bool grouped_by_project = a->code && !a->group_dates && !a->group_environment && !a->sort_titles;
    if (!grouped_by_project) {
        cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .pad_l = 12, .pad_r = 16, .pad_t = 8, .pad_b = 6, .align_y = CU_CENTER});
        bool rc = a->recents_collapsed;
        heading_button(a, "recents-heading", "Recents", !rc, &rc);
        a->recents_collapsed = rc;
        cu_close(cu);
    }
    if (a->search_open) {
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 12, .pad_r = 12, .pad_b = 6});
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 6, .align_y = CU_CENTER, CU_PAD2(6, 8), .radius = 6, .bg = C_FIELD,
                                .border = 1, .border_color = cu_focused(cu, cu_id(cu, "session-search")) ? C_FOCUS : C_FIELD_BORDER,
                                .w = CU_GROW});
        cu_icon(cu, "edit-find", 16, C_MUTED);
        int flags = cu_edit(cu, cu_id(cu, "session-search"), &a->search,
                            &(CuEditStyle){.font = ui_font(a, 13, 400), .color = C_TEXT, .placeholder_color = 0xff7a7a7a,
                                           .placeholder = "Search sessions…", .max_length = 200});
        if (ui_icon_button_sized(a, cu_id(cu, "search-close"), "window-close", 14, 2, "Close search", true, false)) {
            cu_edit_set(&a->search, "");
            a->search_open = false;
            app_refresh_sessions(a);
        }
        cu_close(cu);
        cu_close(cu);
        if (flags & CU_EDIT_CHANGED)
            app_refresh_sessions(a);
        if (flags & CU_EDIT_SUBMIT) {
            if (a->rows && a->rows->len)
                app_select_session(a, ((ChSession *)a->rows->data[0])->id);
            else
                app_set_status(a, "No matching sessions");
        }
        if (cu_focused(cu, cu_id(cu, "session-search")) && cu_key(cu, CU_KEY_ESCAPE, 0)) {
            cu_edit_set(&a->search, "");
            a->search_open = false;
            app_refresh_sessions(a);
            cu_focus(cu, cu_id(cu, "composer"));
        }
    }
    if (grouped_by_project)
        code_history(a);
    else if (!a->recents_collapsed) {
        const ChSession *previous = NULL;
        bool first = true;
        for (size_t i = 0; a->rows && i < a->rows->len; i++) {
            ChSession *s = a->rows->data[i];
            if (ch_map_has(&a->pinned, s->id) || ch_map_has(&a->session_sections, s->id))
                continue;
            if (a->group_environment && first)
                session_heading(a, "This computer");
            else if (a->group_dates) {
                char *heading = date_heading(s->updated), *prior = previous ? date_heading(previous->updated) : NULL;
                if (!prior || strcmp(prior, heading))
                    session_heading(a, heading);
                free(heading);
                free(prior);
            } else if (a->code && a->group_projects && (!previous || strcmp(previous->project, s->project))) {
                char *base = project_heading(a, s->project);
                session_heading(a, base);
                free(base);
            }
            session_item(a, s, "row", true, 8);
            previous = s;
            first = false;
        }
    }
    cu_close(cu);
}
static void profile(App *a) {
    Cu *cu = a->cu;
    CuId id = cu_id(cu, "profile-button");
    const char *name = ch_real_name();
    if (!name || !*name || !strcmp(name, "Unknown"))
        name = ch_user_name();
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .border_t = 1, .border_color = 0xff373737, CU_PAD2(6, 8), .gap = 2});
    cu_open(cu, id, &(CuBox){.row = true, .gap = 10, .align_y = CU_CENTER, CU_PAD2(6, 8), .w = CU_GROW, .radius = 6,
                             .bg = cu_hovered(cu, id) ? C_HOVER : 0, .interactive = true, .tooltip = "Workspace menu"});
    ui_avatar(a, name);
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    cu_icon(cu, "chevron-down", 12, C_MUTED);
    cu_close(cu);
    if (cu_clicked(cu, id))
        menu_open(a, MENU_PROFILE, id, false);
    if (ui_icon_button(a, cu_id(cu, "profile-settings"), "preferences-system", "Settings (Ctrl+,)", !a->busy))
        open_settings(a, SETTINGS_GENERAL);
    cu_close(cu);
}
void sidebar_build(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, cu_id(cu, "sidebar"),
            &(CuBox){.w = CU_PX(SIDEBAR_W), .h = CU_GROW, .bg = C_SIDEBAR, .border_r = 1, .border_color = C_BORDER});
    sidebar_rail(a);
    cu_spacer(cu, CU_PX(1), CU_PX(6));
    if (nav_button(a, "nav-new", "list-add", "New", a->code ? "New session (Ctrl+N)" : "New chat (Ctrl+N)", !a->busy))
        app_start_new(a);
    if (nav_button(a, "nav-search", "edit-find", "Search", "Search sessions (Ctrl+K)", true))
        app_focus_search(a);
    if (nav_button(a, "nav-customize", "preferences-other", "Customize", "Settings (Ctrl+,)", !a->busy))
        open_settings(a, SETTINGS_GENERAL);
    if (nav_button(a, "nav-more", "view-more", "More", NULL, true))
        menu_open(a, MENU_MORE, cu_id(cu, "nav-more"), false);
    history(a);
    profile(a);
    cu_close(cu);
}

/* Top bar of the main area (also the window title rail with client-side decorations). */
static bool kde_desktop(void) {
    const char *desktop = ch_getenv("XDG_CURRENT_DESKTOP");
    return desktop && strstr(desktop, "KDE");
}
static void window_buttons(App *a) {
    Cu *cu = a->cu;
    if (!csd(a))
        return;
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 2, .align_y = CU_CENTER, .pad_l = 8});
    bool maximized = a->p->window_is_maximized && a->p->window_is_maximized(a->window), kde = kde_desktop();
    const char *minimize = kde ? "chevron-down" : "window-minimize";
    const char *resize = kde ? (maximized ? "window-diamond" : "chevron-up") : maximized ? "window-restore" : "window-maximize";
    if (ui_icon_button_sized(a, cu_id(cu, "win-min"), minimize, 16, 5, "Minimize", true, false) && a->p->window_minimize)
        a->p->window_minimize(a->window);
    if (ui_icon_button_sized(a, cu_id(cu, "win-max"), resize, 16, 5, maximized ? "Restore" : "Maximize", true, false) &&
        a->p->window_toggle_maximize)
        a->p->window_toggle_maximize(a->window);
    if (ui_icon_button_sized(a, cu_id(cu, "win-close"), "window-close", 16, 5, "Close", true, false))
        app_quit(a);
    cu_close(cu);
}
void topbar_build(App *a) {
    Cu *cu = a->cu;
    bool conversation = a->id != NULL;
    cu_open(cu, cu_id(cu, "topbar"),
            &(CuBox){.row = true, .w = CU_GROW, .h = CU_PX(38), .pad_l = 14, .pad_r = 8, .gap = 3, .align_y = CU_CENTER,
                     .border_b = conversation ? 1 : 0, .border_color = C_BORDER, .drag_window = csd(a)});
    if (a->sidebar_hidden && ui_icon_button(a, cu_id(cu, "sidebar-reveal"), "sidebar-show", "Show sidebar (Ctrl+B)", true))
        app_toggle_sidebar(a);
    if (conversation) {
        CuId id = cu_id(cu, "session-button");
        if (ui_flat_button(a, id, NULL, a->session_title ? a->session_title : a->code ? "New session" : "New chat",
                           "chevron-down", 14, C_TEXT, "Session actions (Ctrl+Shift+O)", !a->busy, 420))
            menu_open(a, MENU_SESSION, id, false);
    } else if (a->code) {
        cu_open(cu, 0, &(CuBox){CU_PAD2(0, 6)});
        ui_label(a, "New session", 13, C_MUTED);
        cu_close(cu);
    }
    cu_spacer(cu, CU_GROW, CU_PX(1));
    if (conversation && a->code) {
        bool folder = *a->project != 0;
        CuId panel = cu_id(cu, "panel-button");
        if (ui_icon_button_sized(a, panel, "panel-right", 18, 4, "Environment panel (Ctrl+Shift+J)", cu_width(cu) >= 1040, app_panel_visible(a))) {
            a->panel_open = !a->panel_open;
            prefs_save(a);
        }
        if (folder && ui_icon_button(a, cu_id(cu, "changes-button"), "file-diff", "View project changes", !a->busy))
            app_show_changes(a);
        if (folder && ui_icon_button(a, cu_id(cu, "reveal-button"), "folder", "Open the project folder", true))
            app_launch(a, a->project);
    }
    if (conversation && !a->code &&
        ui_flat_button(a, cu_id(cu, "share-button"), "share", "Share", NULL, 14, C_TEXT,
                       "Save a copy of this conversation to share", !a->busy, 0))
        app_export(a);
    window_buttons(a);
    cu_close(cu);
}
