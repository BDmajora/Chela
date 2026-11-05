/* The Code session's environment panel: where this session runs, the branch, how full
 * the context is and what has changed. The git behind it, and behind the folder menu's
 * Branch row and the Worktree box, is here too. All of it runs in the background, so a
 * large repository never slows typing. */
#include "app.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Output with ch_command's closing "[exit N]" line taken off, and true if it was 0.
 * Otherwise the output says why. Quote anything that comes from a repository or a path:
 * a branch name can hold most of the shell's punctuation. */
static bool git_run(const char *cwd, const char *command, int timeout, const ChCancel *cancel, char **output) {
    ChError *error = NULL;
    char *out = ch_command(cwd, command, timeout, cancel, &error);
    if (!out) {
        *output = ch_strdup(error->message);
        ch_clear_error(&error);
        return false;
    }
    char *end = strrchr(out, '\n');
    bool ok = end && !strcmp(end, "\n[exit 0]");
    if (end)
        *end = 0;
    *output = ch_strstrip(out);
    return ok;
}

typedef struct {
    App *app;
    char *project, *branch, *branches, *stat;
    bool list, changes;
} GitProbe;
static void parse_shortstat(App *a, const char *text) {
    a->git_files = a->git_added = a->git_removed = 0;
    for (const char *p = text; *p;) {
        char *end;
        long n = strtol(p, &end, 10);
        if (end == p) {
            p++;
            continue;
        }
        while (*end == ' ')
            end++;
        if (ch_str_has_prefix(end, "file"))
            a->git_files = (int)n;
        else if (ch_str_has_prefix(end, "insertion"))
            a->git_added = (int)n;
        else if (ch_str_has_prefix(end, "deletion"))
            a->git_removed = (int)n;
        p = end;
    }
}
static void git_work(void *data) {
    GitProbe *g = data;
    /* A detached HEAD has no branch to name, so it goes by its commit. */
    if (!git_run(g->project, "b=$(git branch --show-current 2>/dev/null) && echo \"${b:-$(git rev-parse --short HEAD 2>/dev/null)}\"",
                 5, NULL, &g->branch))
        ch_clear_pointer(&g->branch, free);
    /* refname:short would write "heads/x" for a branch that shares its name with a tag. */
    if (g->branch && g->list)
        git_run(g->project, "git for-each-ref --sort=-committerdate --format='%(refname:lstrip=2)%09%(worktreepath)' refs/heads 2>/dev/null",
                5, NULL, &g->branches);
    if (g->changes)
        g->stat = ch_command(g->project, "git --no-pager diff --shortstat HEAD 2>/dev/null", 5, NULL, NULL);
}
static void git_done(void *data) {
    GitProbe *g = data;
    App *a = g->app;
    a->git_pending = false;
    if (!strcmp(g->project, a->project)) {
        free(a->git_branch);
        a->git_branch = ch_steal(&g->branch);
        if (g->list) {
            ch_vec_clear(&a->git_branches);
            ch_vec_init(&a->git_branches, free);
            char **lines = ch_strsplit(g->branches ? g->branches : "", "\n");
            for (size_t i = 0; lines[i]; i++)
                if (*lines[i])
                    ch_vec_push(&a->git_branches, ch_strdup(lines[i]));
            ch_strfreev(lines);
        }
        if (g->changes)
            parse_shortstat(a, g->stat ? g->stat : "");
    }
    free(g->project);
    free(g->branch);
    free(g->branches);
    free(g->stat);
    free(g);
    cu_invalidate(a->cu);
}
void git_poll(App *a) {
    int64_t now = ch_monotonic_us();
    if (!*a->project || a->git_pending || (a->last_git_poll && now - a->last_git_poll < 8 * CH_USEC_PER_SEC))
        return;
    GitProbe *g = ch_new0(GitProbe, 1);
    g->app = a;
    g->project = ch_strdup(a->project);
    g->list = a->menu.kind == MENU_PROJECT;
    g->changes = app_panel_visible(a);
    a->git_pending = true;
    a->last_git_poll = now;
    app_run(a, git_work, git_done, g);
}

/* Switching branch */
typedef struct {
    App *app;
    char *project, *branch, *failure;
    bool refused;
} BranchSwitch;
static void switch_work(void *data) {
    BranchSwitch *s = data;
    /* git carries most changes along and refuses only some, so the rule is ours. It is
     * checked here, right before the switch, because the menu may have been open a while. */
    char *changes = NULL;
    if (!git_run(s->project, "git status --porcelain", 30, NULL, &changes)) {
        s->failure = changes;
        return;
    }
    s->refused = *changes != 0;
    free(changes);
    if (s->refused)
        return;
    char *branch = ch_shell_quote(s->branch), *output = NULL;
    /* Ignored files are the one thing git would otherwise overwrite without asking. */
    char *command = ch_strdup_printf("git switch --quiet --no-overwrite-ignore --end-of-options %s", branch);
    if (!git_run(s->project, command, 30, NULL, &output))
        s->failure = ch_steal(&output);
    free(output);
    free(command);
    free(branch);
}
static void switch_done(void *data) {
    BranchSwitch *s = data;
    App *a = s->app;
    a->git_switching = false;
    a->last_git_poll = 0;
    if (s->refused)
        app_note(a, "Commit or stash your changes before switching branches");
    else if (s->failure)
        app_notice(a, "Could not switch branch", s->failure);
    else {
        if (!strcmp(s->project, a->project)) {
            free(a->git_branch);
            a->git_branch = ch_strdup(s->branch);
        }
        char *note = ch_strconcat("Switched to ", s->branch, NULL);
        app_note(a, note);
        free(note);
    }
    free(s->project);
    free(s->branch);
    free(s->failure);
    free(s);
    cu_invalidate(a->cu);
}
void git_switch_branch(App *a, const char *branch) {
    if (a->busy || a->git_switching || !*a->project)
        return;
    BranchSwitch *s = ch_new0(BranchSwitch, 1);
    s->app = a;
    s->project = ch_strdup(a->project);
    s->branch = ch_strdup(branch);
    a->git_switching = true;
    app_run(a, switch_work, switch_done, s);
}

/* Session worktrees: <data>/chela/worktrees/<id>/<repository>, on the branch chela/<id>.
 * Keeping them out of the repository leaves its status clean, and the last part of the
 * path keeps the name the folder button shows. */
typedef struct {
    App *app;
    ChCancel *cancel;
    char *project, *root, *folder, *failure;
} WorktreeJob;
bool git_in_worktree(App *a, const char *path) {
    size_t n = strlen(a->worktree_root);
    return !strncmp(path, a->worktree_root, n) && path[n] == '/';
}
/* The session's folder may be one inside the worktree. */
static char *worktree_top(App *a, const char *path) {
    const char *id = path + strlen(a->worktree_root) + 1, *name = strchr(id, '/'), *end = name ? strchr(name + 1, '/') : NULL;
    return end ? ch_strndup(path, (size_t)(end - path)) : ch_strdup(path);
}
char *git_worktree_origin(App *a, const char *path) {
    /* The worktree's own .git file says "gitdir: <repository>/.git/worktrees/<name>",
     * relative to the worktree if git is set up to write it that way. */
    char *top = worktree_top(a, path);
    char *file = ch_build_path(top, ".git", NULL), *link = NULL, *origin = NULL;
    size_t size = 0;
    if (ch_file_get_contents(file, &link, &size, NULL) && ch_str_has_prefix(link, "gitdir: ")) {
        char *cut = strstr(link, "/.git/worktrees/");
        if (cut) {
            *cut = 0;
            origin = ch_canonicalize(link + strlen("gitdir: "), top);
        }
    }
    free(link);
    free(file);
    free(top);
    return origin;
}
static void worktree_work(void *data) {
    WorktreeJob *j = data;
    /* The first line is the repository's top, the second where this folder is inside it. */
    char *place = NULL;
    if (!git_run(j->project, "git rev-parse --show-toplevel --show-prefix", 10, j->cancel, &place)) {
        if (!ch_cancel_is_cancelled(j->cancel))
            j->failure = ch_steal(&place);
        free(place);
        return;
    }
    char *inside = strchr(place, '\n');
    if (inside)
        *inside++ = 0;
    char *id = ch_uuid_random(), *name = ch_path_basename(place);
    id[8] = 0;
    char *parent = ch_build_path(j->root, id, NULL), *path = ch_build_path(parent, name, NULL);
    char *branch = ch_strconcat("chela/", id, NULL), *output = NULL;
    char *quoted_parent = ch_shell_quote(parent), *quoted_path = ch_shell_quote(path), *quoted_branch = ch_shell_quote(branch);
    if (ch_mkdir_p(parent, 0700)) {
        j->failure = ch_strdup_printf("%s: %s", parent, strerror(errno));
        goto out;
    }
    char *add = ch_strdup_printf("git worktree add --quiet -b %s %s", quoted_branch, quoted_path);
    bool made = git_run(j->project, add, 600, j->cancel, &output);
    free(add);
    if (made && !ch_cancel_is_cancelled(j->cancel)) {
        /* A folder inside the repository stays that folder, unless nothing in it is committed. */
        char *folder = ch_build_path(path, inside ? inside : "", NULL);
        j->folder = ch_file_is_dir(folder) ? ch_canonicalize(folder, NULL) : ch_strdup(path);
        free(folder);
        goto out;
    }
    if (!ch_cancel_is_cancelled(j->cancel))
        j->failure = ch_steal(&output);
    /* What did not finish is nobody's work yet, and the names are ones made up above. */
    char *undo = ch_strdup_printf("git worktree remove --force %s; rm -rf -- %s; git worktree prune; git branch -D %s",
                                  quoted_path, quoted_parent, quoted_branch);
    free(ch_command(j->project, undo, 30, NULL, NULL));
    free(undo);
out:
    free(quoted_parent);
    free(quoted_path);
    free(quoted_branch);
    free(output);
    free(branch);
    free(path);
    free(parent);
    free(name);
    free(id);
    free(place);
}
static void worktree_done(void *data) {
    WorktreeJob *j = data;
    App *a = j->app;
    a->busy = a->worktree_pending = a->status_visible = false;
    ch_clear_pointer(&a->cancel, ch_cancel_unref);
    if (a->closing)
        a->p->quit();
    else if (j->folder) {
        app_set_project(a, j->folder);
        app_send(a, false);
    } else {
        if (j->failure)
            app_notice(a, "Could not create the worktree", j->failure);
        cu_focus(a->cu, cu_id(a->cu, "composer"));
    }
    ch_cancel_unref(j->cancel);
    free(j->project);
    free(j->root);
    free(j->folder);
    free(j->failure);
    free(j);
    cu_invalidate(a->cu);
}
void git_worktree_create(App *a) {
    WorktreeJob *j = ch_new0(WorktreeJob, 1);
    j->app = a;
    j->project = ch_strdup(a->project);
    j->root = ch_strdup(a->worktree_root);
    a->cancel = ch_cancel_new();
    j->cancel = ch_cancel_ref(a->cancel);
    /* As a request does, this holds the draft and everything else until it is over, and Stop ends it. */
    a->busy = a->worktree_pending = true;
    app_run(a, worktree_work, worktree_done, j);
}

/* Removing one with its session. What decides is whether anything would be lost, so
 * nothing here forces git, and a worktree that cannot go keeps the session as well. */
static const char *const removal_checks[][3] = {
    {"git status --porcelain", "The worktree has uncommitted changes",
     "Nothing was deleted. Commit or discard them, then delete the session again. "
     "To delete only the session, untick “Also remove the worktree”."},
    /* git removes a detached HEAD without a word, and with it the commits no branch holds. */
    {"if head=$(git rev-parse --verify --quiet HEAD); then git rev-list -1 \"$head\" --not --branches --tags --remotes; fi",
     "The worktree has commits on no branch",
     "Nothing was deleted. Put them on a branch, then delete the session again. "
     "To delete only the session, untick “Also remove the worktree”."},
};
typedef struct {
    App *app;
    char *top, *origin, *branch, *failure, *kept;
    const char *const *refused; /* the check that stopped it */
} WorktreeRemoval;
static bool in_folder(const char *path, const char *folder) {
    size_t n = strlen(folder);
    return !strncmp(path, folder, n) && (!path[n] || path[n] == '/');
}
bool git_worktree_removable(App *a) {
    if (!a->code || !git_in_worktree(a, a->project))
        return false;
    char *top = worktree_top(a, a->project), *origin = git_worktree_origin(a, a->project);
    ChVec *sessions = ch_store_list(a->store, "", NULL);
    bool removable = sessions && origin && ch_file_is_dir(origin) && !in_folder(a->launch_project, top);
    for (size_t i = 0; sessions && i < sessions->len; i++) {
        ChSession *s = sessions->data[i];
        if (!strcmp(s->id, a->id))
            continue;
        removable = removable && !in_folder(s->project, top);
        for (size_t k = 0; s->folders && s->folders[k]; k++)
            removable = removable && !in_folder(s->folders[k], top);
    }
    ch_vec_free(sessions);
    free(origin);
    free(top);
    return removable;
}
static void removal_work(void *data) {
    WorktreeRemoval *r = data;
    for (size_t i = 0; i < sizeof removal_checks / sizeof *removal_checks && !r->refused && !r->failure; i++) {
        char *output = NULL;
        if (!git_run(r->top, removal_checks[i][0], 30, NULL, &output))
            r->failure = ch_steal(&output);
        else if (*output)
            r->refused = removal_checks[i];
        free(output);
    }
    if (r->refused || r->failure)
        return;
    char *top = ch_shell_quote(r->top), *branch = ch_shell_quote(r->branch), *output = NULL;
    /* Still no --force: if the folder changed since the check, git refuses it. And nothing
     * can cancel this, as stopping partway would leave a worktree neither there nor gone. */
    char *remove = ch_strdup_printf("git worktree remove %s", top);
    if (git_run(r->origin, remove, 600, NULL, &output)) {
        /* All that is left of it is the folder that kept its name apart from the others'. */
        char *parent = ch_path_dirname(r->top);
        rmdir(parent);
        free(parent);
        /* The branch goes only if every commit on it is also on another branch, a tag or a
         * remote. git branch -d asks something narrower, only about the current branch. */
        char *drop = ch_strdup_printf("if git show-ref --verify --quiet refs/heads/%s; then "
                                      "only=$(git rev-list -1 refs/heads/%s --not --exclude=%s --branches --tags --remotes) && "
                                      "test -z \"$only\" && git branch --quiet -D %s; fi",
                                      branch, branch, branch, branch);
        char *reason = NULL;
        if (!git_run(r->origin, drop, 30, NULL, &reason))
            r->kept = ch_strdup_printf("Kept branch %s: %s", r->branch, *reason ? reason : "its commits are nowhere else");
        free(reason);
        free(drop);
    } else
        r->failure = ch_steal(&output);
    free(output);
    free(remove);
    free(branch);
    free(top);
}
static void removal_done(void *data) {
    WorktreeRemoval *r = data;
    App *a = r->app;
    a->busy = a->worktree_removing = a->status_visible = false;
    if (r->refused)
        app_notice(a, r->refused[1], r->refused[2]);
    else if (r->failure) {
        char *text = ch_strconcat("The session was kept.\n\n", r->failure, NULL);
        app_notice(a, "Could not remove the worktree", text);
        free(text);
    } else {
        app_forget_session(a);
        if (r->kept)
            app_note(a, r->kept);
    }
    if (a->closing)
        a->p->quit();
    free(r->top);
    free(r->origin);
    free(r->branch);
    free(r->failure);
    free(r->kept);
    free(r);
    cu_invalidate(a->cu);
}
void git_worktree_remove(App *a) {
    WorktreeRemoval *r = ch_new0(WorktreeRemoval, 1);
    r->app = a;
    r->top = worktree_top(a, a->project);
    r->origin = git_worktree_origin(a, a->project);
    char *parent = ch_path_dirname(r->top), *id = ch_path_basename(parent);
    r->branch = ch_strconcat("chela/", id, NULL);
    free(id);
    free(parent);
    /* Busy, because the session must not start anything in the folder that is going. */
    a->busy = a->worktree_removing = true;
    app_run(a, removal_work, removal_done, r);
}

bool app_panel_visible(App *a) {
    return a->code && a->panel_open && a->id && cu_width(a->cu) >= 1040;
}
static void heading(App *a, const char *text) {
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .pad_b = 6});
    cu_text(a->cu, 0, text, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED});
    cu_close(a->cu);
}
static void row(App *a, const char *icon, const char *text, const char *detail, const char *tooltip) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 10, .w = CU_GROW, .align_y = CU_CENTER, CU_PAD2(4, 0), .tooltip = tooltip});
    cu_icon(cu, icon, 16, 0xffb5b5b5);
    cu_text(cu, 0, text, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_FIT});
    if (detail)
        cu_text(cu, 0, detail, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_FAINT, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
    cu_close(cu);
}
static void rule(App *a) {
    ui_separator(a, 10);
}
static void folder_row(App *a, const char *path) {
    char *base = ch_path_basename(path), *parent = ch_path_dirname(path);
    const char *home = ch_home_dir();
    char *shown = home && ch_str_has_prefix(parent, home) ? ch_strconcat("~", parent + strlen(home), NULL) : ch_strdup(parent);
    row(a, "folder", base, shown, path);
    free(base);
    free(parent);
    free(shown);
}
/* The other folders the agent can reach in this session. */
static void included(App *a) {
    Cu *cu = a->cu;
    heading(a, "Also included");
    size_t removed = SIZE_MAX;
    for (size_t i = 0; i < a->folders.len; i++) {
        cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .align_y = CU_CENTER, .gap = 4});
        cu_open(cu, 0, &(CuBox){.w = CU_GROW});
        folder_row(a, a->folders.data[i]);
        cu_close(cu);
        if (ui_icon_button_sized(a, cu_idn(cu, "panel-remove-folder", (int64_t)i), "window-close", 14, 3, "Remove from this session", !a->busy, false))
            removed = i;
        cu_close(cu);
    }
    if (removed != SIZE_MAX)
        app_remove_folder(a, removed);
    CuId add = cu_id(cu, "panel-add-folder");
    cu_open(cu, add, &(CuBox){.row = true, .gap = 10, .w = CU_GROW, .align_y = CU_CENTER, CU_PAD2(4, 0), .radius = 6, .interactive = !a->busy,
                              .tooltip = "Let the agent work in another folder too"});
    cu_icon(cu, "list-add", 16, cu_hovered(cu, add) ? C_TEXT : 0xffb5b5b5);
    cu_text(cu, 0, "Add folder…", &(CuTextStyle){.font = ui_font(a, 13, 400), .color = cu_hovered(cu, add) ? C_TEXT : C_TEXT2});
    cu_close(cu);
    if (cu_clicked(cu, add))
        app_choose_folder(a);
}
void panel_build(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, cu_id(cu, "environment-panel"),
            &(CuBox){.w = CU_PX(PANEL_W), .h = CU_GROW, .bg = C_BG, .border_l = 1, .border_color = C_BORDER, CU_PAD2(14, 14), .scroll_y = true});
    heading(a, "Environment");
    row(a, "computer", "This computer", NULL, NULL);
    if (*a->project)
        folder_row(a, a->project);
    else
        row(a, "folder", "No folder", NULL, NULL);
    if (a->git_branch)
        row(a, "git-branch", a->git_branch, NULL, "Current branch");
    rule(a);
    if (*a->project) {
        included(a);
        rule(a);
    }
    heading(a, "Context");
    cu_open(cu, cu_id(cu, "panel-context"), &(CuBox){.w = CU_GROW, .gap = 3});
    model_context_build(a);
    cu_close(cu);
    if (!*a->project) {
        cu_close(cu);
        return;
    }
    rule(a);
    heading(a, "Changes");
    CuId changes = cu_id(cu, "panel-changes");
    bool any = a->git_files > 0;
    cu_open(cu, changes, &(CuBox){.row = true, .gap = 8, .w = CU_GROW, .align_y = CU_CENTER, CU_PAD2(5, 6), .radius = 6,
                                  .bg = any && cu_hovered(cu, changes) ? C_HOVER : 0, .interactive = any,
                                  .tooltip = any ? "View project changes" : NULL});
    cu_icon(cu, "file-diff", 16, 0xffb5b5b5);
    if (any) {
        char *added = ch_strdup_printf("+%d", a->git_added), *removed = ch_strdup_printf("−%d", a->git_removed);
        char *files = ch_strdup_printf("%d file%s", a->git_files, a->git_files == 1 ? "" : "s");
        cu_text(cu, 0, files, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_TEXT2, .w = CU_GROW});
        cu_text(cu, 0, added, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_ADDED});
        cu_text(cu, 0, removed, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_REMOVED});
        free(added);
        free(removed);
        free(files);
    } else
        cu_text(cu, 0, a->git_branch ? "No changes" : "Not a git repository", &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_FAINT});
    cu_close(cu);
    if (any && cu_clicked(cu, changes))
        app_show_changes(a);
    cu_close(cu);
}
