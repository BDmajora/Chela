/* Modal sheets drawn inside the window: a stack of dialogs, each with its own
 * state and build function. Work they start runs in the background and finds its
 * dialog again by serial number, so closing a dialog never races a worker. */
#include "app.h"
#include <dirent.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

Dialog *dialog_push(App *a, Dialog *d) {
    d->serial = ++a->dialog_serial;
    ch_vec_push(&a->dialogs, d);
    menu_close(a);
    cu_invalidate(a->cu);
    return d;
}
Dialog *dialog_find(App *a, unsigned serial) {
    for (size_t i = 0; serial && i < a->dialogs.len; i++) {
        Dialog *d = a->dialogs.data[i];
        if (d->serial == serial && !d->closed)
            return d;
    }
    return NULL;
}
void dialog_close(App *a, Dialog *d) {
    if (d)
        d->closed = true;
    cu_invalidate(a->cu);
}
static Dialog *top_dialog(App *a) {
    for (size_t i = a->dialogs.len; i-- > 0;) {
        Dialog *d = a->dialogs.data[i];
        if (!d->closed)
            return d;
    }
    return NULL;
}
void dialogs_build(App *a) {
    /* A dialog closed during last frame's build is freed only now: that frame's layout
     * still read its text fields. */
    for (size_t i = 0; i < a->dialogs.len;) {
        Dialog *d = a->dialogs.data[i];
        if (d->closed) {
            ch_vec_steal(&a->dialogs, i);
            if (d->destroy)
                d->destroy(d);
            free(d);
        } else
            i++;
    }
    for (size_t i = 0; i < a->dialogs.len; i++) {
        Dialog *d = a->dialogs.data[i];
        if (!d->closed)
            d->build(a, d);
    }
}
void dialogs_free(App *a) {
    for (size_t i = 0; i < a->dialogs.len; i++) {
        Dialog *d = a->dialogs.data[i];
        if (d->destroy)
            d->destroy(d);
        free(d);
    }
    ch_vec_clear(&a->dialogs);
}
bool dialog_frame_begin(App *a, Dialog *d, const char *title, float width, float height) {
    Cu *cu = a->cu;
    CuId layer = cu_idn(cu, "#dialog", d->serial);
    float w = CH_MIN(width, cu_width(cu) - 48), h = height > 0 ? CH_MIN(height, cu_height(cu) - 48) : 0;
    cu_layer_open(cu, layer, &(CuLayer){.kind = CU_LAYER_MODAL, .placement = CU_PLACE_CENTER, .scrim = C_SCRIM},
                  &(CuBox){.w = CU_PX(w), .h = h > 0 ? CU_PX(h) : CU_FIT_MAX(cu_height(cu) - 48), .bg = C_SIDEBAR,
                           .border = 1, .border_color = 0xff383838, .radius = 12, .shadow = 24, .clip = true});
    cu_push_id(cu, layer);
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_PX(46), .pad_l = 18, .pad_r = 10, .align_y = CU_CENTER,
                            .border_b = 1, .border_color = 0xff383838});
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 16, 600), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    bool cancel = ui_icon_button_sized(a, cu_id(cu, "dialog-close"), "window-close", 18, 5, "Close", true, false);
    cu_close(cu);
    if (top_dialog(a) == d && cu_key(cu, CU_KEY_ESCAPE, 0))
        cancel = true;
    return cancel;
}
void dialog_frame_end(App *a) {
    cu_pop_id(a->cu);
    cu_layer_close(a->cu);
}
static void body_begin(App *a, bool scroll) {
    cu_open(a->cu, cu_id(a->cu, "dialog-body"),
            &(CuBox){.w = CU_GROW, .h = scroll ? CU_GROW : CU_FIT, .scroll_y = scroll, CU_PAD2(20, 24), .gap = 12});
}
static void body_end(App *a) {
    cu_close(a->cu);
}
bool dialog_footer_buttons(App *a, Dialog *d, const char *cancel, const char *accept, bool accept_enabled, int *choice) {
    (void)d;
    Cu *cu = a->cu;
    bool clicked = false;
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 8, .align_x = CU_END, .pad_l = 16, .pad_r = 16, .pad_t = 4, .pad_b = 16});
    if (cancel && ui_button(a, cu_id(cu, "footer-cancel"), cancel, true, false)) {
        *choice = 0;
        clicked = true;
    }
    if (accept && ui_button(a, cu_id(cu, "footer-accept"), accept, accept_enabled, true)) {
        *choice = 1;
        clicked = true;
    }
    cu_close(cu);
    return clicked;
}

/* Notices */
typedef struct {
    Dialog base;
    char *title, *text;
} Notice;
static void notice_destroy(Dialog *d) {
    Notice *n = (Notice *)d;
    free(n->title);
    free(n->text);
}
static void notice_build(App *a, Dialog *d) {
    Notice *n = (Notice *)d;
    bool cancel = dialog_frame_begin(a, d, n->title, 520, 0);
    body_begin(a, false);
    ui_text_block(a, cu_id(a->cu, "notice-text"), n->text, 14, C_TEXT2, true);
    body_end(a);
    int choice;
    if (dialog_footer_buttons(a, d, NULL, "Close", true, &choice) || cancel || cu_key(a->cu, CU_KEY_RETURN, 0))
        dialog_close(a, d);
    dialog_frame_end(a);
}
void app_notice(App *a, const char *title, const char *text) {
    Notice *n = ch_new0(Notice, 1);
    n->base.build = notice_build;
    n->base.destroy = notice_destroy;
    n->title = ch_strdup(title);
    n->text = ch_strdup(text ? text : "");
    dialog_push(a, &n->base);
}
void open_shortcuts(App *a) {
    app_notice(a, "Keyboard shortcuts",
               "Ctrl+N     New session\nCtrl+K     Search sessions\nCtrl+B     Toggle sidebar\n"
               "Ctrl+,       Settings\nCtrl+M     Permission mode, then 1 to 5\nCtrl+Shift+M     Switch Agent / Chat\n"
               "Ctrl+Shift+L     Model menu\nCtrl+Shift+P     Project menu\nCtrl+Shift+A     Attach menu\n"
               "Ctrl+Shift+O     Session menu\nCtrl+Shift+F     Group and sort\n"
               "Ctrl+Shift+E     Environment menu\nCtrl+Shift+U     Workspace menu\nCtrl+1 / Ctrl+2     Chat / Code\n"
               "Ctrl+U     Attach file\nCtrl+D     Dictate\nCtrl+Shift+V     Voice options\n"
               "Ctrl+Shift+S     Context use\nCtrl+Shift+D     Server diagnostics\n"
               "Ctrl+A     Select all\nCtrl+C     Copy selection\nCtrl+click     Mark sessions in the sidebar\n"
               "Page Up / Page Down     Scroll the transcript\n"
               "Enter     Send message\nShift+Enter     New line\nEsc     Stop current request\nCtrl+Q     Quit");
}
void open_about(App *a) {
    char *text = ch_strdup_printf("A native workspace for your coding models.\n\n"
                                  "Connect a local or remote model server, or an ACP harness.\n"
                                  "Interface backend: %s.\n"
                                  "Independent software. Not affiliated with Anthropic.",
                                  a->p->name);
    app_notice(a, "Chela " CHELA_VERSION, text);
    free(text);
}

/* Confirmation */
typedef struct {
    Dialog base;
    char *title, *text, *accept, *option, *detail;
    bool destructive, *checked;
    void (*callback)(App *, void *);
    void *data;
} Confirm;
static void confirm_destroy(Dialog *d) {
    Confirm *c = (Confirm *)d;
    free(c->title);
    free(c->text);
    free(c->accept);
    free(c->option);
    free(c->detail);
}
static void confirm_build(App *a, Dialog *d) {
    Confirm *c = (Confirm *)d;
    bool cancel = dialog_frame_begin(a, d, c->title, 460, 0);
    body_begin(a, false);
    ui_text_block(a, cu_id(a->cu, "confirm-text"), c->text, 14, C_TEXT2, false);
    if (c->option) {
        ui_checkbox(a, cu_id(a->cu, "confirm-option"), c->option, c->checked, true);
        cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 26});
        ui_text_block(a, cu_id(a->cu, "confirm-detail"), c->detail, 12, C_MUTED, false);
        cu_close(a->cu);
    }
    body_end(a);
    int choice = -1;
    dialog_footer_buttons(a, d, "Cancel", c->accept, true, &choice);
    if (choice == 1) {
        dialog_close(a, d);
        c->callback(a, c->data);
    } else if (choice == 0 || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
static Confirm *confirm_new(const char *title, const char *text, const char *accept, bool destructive,
                            void (*callback)(App *, void *), void *data) {
    Confirm *c = ch_new0(Confirm, 1);
    c->base.build = confirm_build;
    c->base.destroy = confirm_destroy;
    c->title = ch_strdup(title);
    c->text = ch_strdup(text ? text : "");
    c->accept = ch_strdup(accept);
    c->destructive = destructive;
    c->callback = callback;
    c->data = data;
    return c;
}
void dialog_confirm(App *a, const char *title, const char *text, const char *accept, bool destructive,
                    void (*callback)(App *, void *), void *data) {
    dialog_push(a, &confirm_new(title, text, accept, destructive, callback, data)->base);
}
/* A destructive confirmation with one choice that goes with it, left in *checked. */
void dialog_confirm_option(App *a, const char *title, const char *text, const char *accept, const char *option,
                           const char *detail, bool *checked, void (*callback)(App *, void *), void *data) {
    Confirm *c = confirm_new(title, text, accept, true, callback, data);
    c->option = ch_strdup(option);
    c->detail = ch_strdup(detail);
    c->checked = checked;
    dialog_push(a, &c->base);
}

/* Single-line text input */
typedef struct {
    Dialog base;
    char *title, *placeholder, *accept;
    CuEdit edit;
    size_t max_length;
    bool focused;
    void (*callback)(App *, const char *, void *);
    void *data;
} TextInput;
static void input_destroy(Dialog *d) {
    TextInput *t = (TextInput *)d;
    free(t->title);
    free(t->placeholder);
    free(t->accept);
    cu_edit_clear(&t->edit);
}
static void input_build(App *a, Dialog *d) {
    TextInput *t = (TextInput *)d;
    Cu *cu = a->cu;
    bool cancel = dialog_frame_begin(a, d, t->title, 440, 0);
    body_begin(a, false);
    CuId entry = cu_id(cu, "input-entry");
    if (!t->focused) {
        cu_focus(cu, entry);
        t->edit.anchor = 0;
        t->edit.caret = t->edit.text.len;
        t->focused = true;
    }
    int flags = ui_entry(a, entry, &t->edit, t->placeholder, true, t->max_length);
    body_end(a);
    int choice = -1;
    dialog_footer_buttons(a, d, "Cancel", t->accept, true, &choice);
    if (choice == 1 || (flags & CU_EDIT_SUBMIT)) {
        dialog_close(a, d);
        t->callback(a, cu_edit_text(&t->edit), t->data);
    } else if (choice == 0 || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void dialog_text_input(App *a, const char *title, const char *initial, const char *placeholder, const char *accept,
                       size_t max_length, void (*callback)(App *, const char *, void *), void *data) {
    TextInput *t = ch_new0(TextInput, 1);
    t->base.build = input_build;
    t->base.destroy = input_destroy;
    t->title = ch_strdup(title);
    t->placeholder = ch_strdup(placeholder);
    t->accept = ch_strdup(accept);
    t->max_length = max_length;
    t->callback = callback;
    t->data = data;
    cu_edit_init(&t->edit, false);
    cu_edit_set(&t->edit, initial ? initial : "");
    dialog_push(a, &t->base);
}

/* Read-only text */
typedef struct {
    Dialog base;
    char *title, *text;
    bool mono;
    float width, height;
} TextView;
static void view_destroy(Dialog *d) {
    TextView *v = (TextView *)d;
    free(v->title);
    free(v->text);
}
static void view_build(App *a, Dialog *d) {
    TextView *v = (TextView *)d;
    bool cancel = dialog_frame_begin(a, d, v->title, v->width, v->height);
    body_begin(a, true);
    if (v->mono)
        ui_mono_text(a, cu_id(a->cu, "view-text"), v->text, 13, true);
    else
        ui_text_block(a, cu_id(a->cu, "view-text"), v->text, 14, C_TEXT2, true);
    body_end(a);
    int choice;
    if (dialog_footer_buttons(a, d, NULL, "Close", true, &choice) || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void dialog_text_view(App *a, const char *title, const char *text, bool monospace, float width, float height) {
    TextView *v = ch_new0(TextView, 1);
    v->base.build = view_build;
    v->base.destroy = view_destroy;
    v->title = ch_strdup(title);
    v->text = ch_strdup(text && *text ? text : "No changes.");
    v->mono = monospace;
    v->width = width;
    v->height = height;
    dialog_push(a, &v->base);
}

/* Files: the platform's native dialog when it has one, otherwise Chela's own browser. */
typedef struct {
    char *name;
    bool dir;
} FileEntry;
static void entry_free(void *data) {
    FileEntry *e = data;
    free(e->name);
    free(e);
}
typedef struct {
    Dialog base;
    int kind;
    char *title, *dir;
    ChVec entries;
    int selected;
    CuEdit name, path;
    bool hidden;
    void (*callback)(App *, const char *, void *);
    void *data;
} FileDialog;
static int compare_entries(const void *x, const void *y, void *data) {
    (void)data;
    const FileEntry *a = *(FileEntry *const *)x, *b = *(FileEntry *const *)y;
    if (a->dir != b->dir)
        return a->dir ? -1 : 1;
    return ch_utf8_collate(a->name, b->name);
}
static void file_load(FileDialog *f, const char *dir) {
    char *next = ch_canonicalize(dir, NULL);
    free(f->dir);
    f->dir = next;
    ch_vec_clear(&f->entries);
    ch_vec_init(&f->entries, entry_free);
    f->selected = -1;
    cu_edit_set(&f->path, f->dir);
    DIR *d = opendir(f->dir);
    for (struct dirent *e; d && (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || (!f->hidden && e->d_name[0] == '.'))
            continue;
        char *full = ch_build_path(f->dir, e->d_name, NULL);
        bool is_dir = ch_file_is_dir(full);
        free(full);
        if (f->kind == 1 && !is_dir)
            continue;
        FileEntry *entry = ch_new0(FileEntry, 1);
        entry->name = ch_utf8_make_valid(e->d_name, -1);
        entry->dir = is_dir;
        ch_vec_push(&f->entries, entry);
    }
    if (d)
        closedir(d);
    ch_vec_sort(&f->entries, compare_entries, NULL);
}
static void file_destroy(Dialog *d) {
    FileDialog *f = (FileDialog *)d;
    free(f->title);
    free(f->dir);
    ch_vec_clear(&f->entries);
    cu_edit_clear(&f->name);
    cu_edit_clear(&f->path);
}
typedef struct {
    void (*callback)(App *, const char *, void *);
    void *data;
    char *path;
} Overwrite;
static void overwrite_confirmed(App *a, void *data) {
    Overwrite *o = data;
    o->callback(a, o->path, o->data);
    free(o->path);
    free(o);
}
static void file_accept(App *a, FileDialog *f, const char *path) {
    if (f->kind == 2 && ch_file_exists(path)) {
        /* The file dialog closes now; keep what the confirmation needs. */
        Overwrite *o = ch_new0(Overwrite, 1);
        o->callback = f->callback;
        o->data = f->data;
        o->path = ch_strdup(path);
        dialog_close(a, &f->base);
        char *text = ch_strdup_printf("“%s” already exists. Replacing it will overwrite its contents.", path);
        dialog_confirm(a, "Replace existing file?", text, "Replace", true, overwrite_confirmed, o);
        free(text);
        return;
    }
    dialog_close(a, &f->base);
    f->callback(a, path, f->data);
}
static void file_build(App *a, Dialog *d) {
    FileDialog *f = (FileDialog *)d;
    Cu *cu = a->cu;
    bool cancel = dialog_frame_begin(a, d, f->title, 720, 560);
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 6, .align_y = CU_CENTER, .pad_l = 20, .pad_r = 20, .pad_t = 14});
    char *parent = ch_path_dirname(f->dir);
    if (ui_icon_button(a, cu_id(cu, "file-up"), "go-up", "Parent folder", strcmp(parent, f->dir) != 0))
        file_load(f, parent);
    free(parent);
    if (ui_icon_button(a, cu_id(cu, "file-home"), "computer", "Home folder", true))
        file_load(f, ch_home_dir());
    if (ui_entry(a, cu_id(cu, "file-path"), &f->path, "Folder", true, 4096) & CU_EDIT_SUBMIT) {
        if (ch_file_is_dir(cu_edit_text(&f->path)))
            file_load(f, cu_edit_text(&f->path));
        else if (f->kind == 0 && ch_file_is_regular(cu_edit_text(&f->path))) {
            char *path = ch_strdup(cu_edit_text(&f->path));
            file_accept(a, f, path);
            free(path);
            cu_close(cu);
            dialog_frame_end(a);
            return;
        }
    }
    bool hidden = f->hidden;
    if (ui_icon_button_sized(a, cu_id(cu, "file-hidden"), "view-more", 18, 4, "Show hidden files", true, hidden)) {
        f->hidden = !f->hidden;
        char *dir = ch_strdup(f->dir);
        file_load(f, dir);
        free(dir);
    }
    cu_close(cu);
    cu_open(cu, cu_id(cu, "file-list"), &(CuBox){.w = CU_GROW, .h = CU_GROW, .scroll_y = true, .pad_l = 16, .pad_r = 16, .pad_t = 10, .pad_b = 6});
    char *open_dir = NULL, *accept = NULL;
    if (!f->entries.len)
        ui_label(a, f->kind == 1 ? "No folders here." : "This folder is empty.", 13, C_FAINT);
    for (size_t i = 0; i < f->entries.len; i++) {
        FileEntry *e = f->entries.data[i];
        CuId id = cu_idn(cu, "file-row", (int64_t)i);
        bool selected = f->selected == (int)i;
        cu_open(cu, id, &(CuBox){.row = true, .gap = 10, .align_y = CU_CENTER, CU_PAD2(6, 8), .radius = 5, .w = CU_GROW,
                                 .bg = selected ? C_SELECTED : cu_hovered(cu, id) ? C_HOVER : 0, .interactive = true});
        cu_icon(cu, e->dir ? "folder" : "document-open", 16, 0xffd4d4d4);
        cu_text(cu, 0, e->name, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
        cu_close(cu);
        if (cu_double_clicked(cu, id)) {
            if (e->dir)
                open_dir = ch_build_path(f->dir, e->name, NULL);
            else if (f->kind == 0)
                accept = ch_build_path(f->dir, e->name, NULL);
        } else if (cu_clicked(cu, id)) {
            f->selected = (int)i;
            if (f->kind == 2 && !e->dir)
                cu_edit_set(&f->name, e->name);
        }
    }
    cu_close(cu);
    if (f->kind == 2) {
        cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 10, .align_y = CU_CENTER, .pad_l = 20, .pad_r = 20, .pad_t = 6});
        ui_label(a, "Name", 14, C_MUTED);
        if (ui_entry(a, cu_id(cu, "file-name"), &f->name, "File name", true, 255) & CU_EDIT_SUBMIT && !cu_edit_empty(&f->name))
            accept = ch_build_path(f->dir, cu_edit_text(&f->name), NULL);
        cu_close(cu);
    }
    int choice = -1;
    const char *label = f->kind == 1 ? "Choose folder" : f->kind == 2 ? "Save" : "Open";
    FileEntry *sel = f->selected >= 0 && f->selected < (int)f->entries.len ? f->entries.data[f->selected] : NULL;
    bool can = f->kind == 1 || (f->kind == 0 && sel) || (f->kind == 2 && !cu_edit_empty(&f->name));
    dialog_footer_buttons(a, d, "Cancel", label, can, &choice);
    if (choice == 1 && !accept && !open_dir) {
        if (f->kind == 1)
            accept = sel ? ch_build_path(f->dir, sel->name, NULL) : ch_strdup(f->dir);
        else if (f->kind == 0 && sel)
            sel->dir ? (void)(open_dir = ch_build_path(f->dir, sel->name, NULL)) : (void)(accept = ch_build_path(f->dir, sel->name, NULL));
        else if (f->kind == 2)
            accept = ch_build_path(f->dir, cu_edit_text(&f->name), NULL);
    }
    if (open_dir) {
        file_load(f, open_dir);
        free(open_dir);
    } else if (accept)
        file_accept(a, f, accept);
    else if (choice == 0 || cancel) {
        dialog_close(a, d);
        f->callback(a, NULL, f->data);
    }
    free(accept);
    dialog_frame_end(a);
}
typedef struct {
    App *app;
    void (*callback)(App *, const char *, void *);
    void *data;
} Native;
typedef struct {
    Native *native;
    char *path;
} NativeResult;
static void native_main(void *data) {
    NativeResult *r = data;
    r->native->callback(r->native->app, r->path, r->native->data);
    cu_invalidate(r->native->app->cu);
    free(r->path);
    free(r->native);
    free(r);
}
static void native_done(const char *path, void *data) {
    NativeResult *r = ch_new0(NativeResult, 1);
    r->native = data;
    r->path = ch_strdup(path);
    r->native->app->p->post(native_main, r);
}
void dialog_file(App *a, int kind, const char *title, const char *folder, const char *name,
                 void (*callback)(App *, const char *, void *), void *data) {
    if (a->p->file_dialog && !ch_getenv("CHELA_BUILTIN_FILE_DIALOG")) {
        Native *n = ch_new0(Native, 1);
        *n = (Native){a, callback, data};
        a->p->file_dialog(a->window, kind, title, folder, name, native_done, n);
        return;
    }
    FileDialog *f = ch_new0(FileDialog, 1);
    f->base.build = file_build;
    f->base.destroy = file_destroy;
    f->kind = kind;
    f->title = ch_strdup(title);
    f->callback = callback;
    f->data = data;
    cu_edit_init(&f->name, false);
    cu_edit_init(&f->path, false);
    cu_edit_set(&f->name, name ? name : "");
    file_load(f, folder && ch_file_is_dir(folder) ? folder : ch_home_dir());
    dialog_push(a, &f->base);
}

/* Settings */
/* Settings. Connections is the SSH model library setup: the only way Chela connects. */
typedef struct {
    Dialog base;
    int page;
    CuEdit search, tokens, temperature, timeout;
    CuEdit host, user, folder, binary, ssh_port, remote_port, local_port, device, context, headroom;
    int memory;
    bool tools, remember, advanced, connecting, forgetting, focused;
    bool at_launch; /* chela --setup-models: cancelling continues with the saved connection */
    char *result, *result_tip;
    ChCancel *cancel;
} Settings;
static void settings_destroy(Dialog *d) {
    Settings *s = (Settings *)d;
    CuEdit *edits[] = {&s->search, &s->tokens, &s->temperature, &s->timeout, &s->host, &s->user, &s->folder,
                       &s->binary, &s->ssh_port, &s->remote_port, &s->local_port, &s->device, &s->context, &s->headroom};
    for (size_t i = 0; i < CH_N_ELEMENTS(edits); i++)
        cu_edit_clear(edits[i]);
    free(s->result);
    free(s->result_tip);
    if (s->cancel) {
        ch_cancel_cancel(s->cancel);
        ch_cancel_unref(s->cancel);
    }
}
static void set_result(Settings *s, const char *text, const char *tip) {
    free(s->result);
    free(s->result_tip);
    s->result = ch_strdup(text);
    s->result_tip = ch_strdup(tip);
}
static void field_row(App *a, const char *label) {
    cu_open(a->cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 20, .align_y = CU_CENTER});
    cu_text(a->cu, 0, label, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_MUTED, .w = CU_PX(150)});
}
static void field_end(App *a) {
    cu_close(a->cu);
}
static void appearance_row(App *a, const char *title, const char *const *choices, int count, int *preference) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_FIT_MIN(52), .align_y = CU_CENTER, .border_b = 1,
                            .border_color = 0xff2e2e2e, CU_PAD2(10, 0)});
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .w = CU_GROW});
    if (ui_dropdown(a, cu_ids(cu, "appearance", title), choices, count, preference, true, true)) {
        prefs_save(a);
        md_cache_clear(a);
    }
    cu_close(cu);
}
static void setting_row(App *a, const char *title, const char *detail, CuId *row);
static void general_page(App *a) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD(24)});
    cu_text(cu, 0, "General", &(CuTextStyle){.font = ui_font(a, 22, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(18));
    cu_text(cu, 0, "Appearance", &(CuTextStyle){.font = ui_font(a, 14, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(10));
    const char *interfaces[] = {"Default sans", "Noto Sans", "DejaVu Sans"};
    const char *responses[] = {"Default serif", "Match interface"};
    const char *sizes[] = {"Small", "Medium", "Large"};
    const char *widths[] = {"Narrow", "Medium", "Wide"};
    appearance_row(a, "Interface font", interfaces, 3, &a->interface_font);
    appearance_row(a, "Response font", responses, 2, &a->response_font);
    appearance_row(a, "Transcript text size", sizes, 3, &a->text_size);
    appearance_row(a, "Transcript width", widths, 3, &a->transcript_width);
    cu_spacer(cu, CU_PX(1), CU_PX(40));
    cu_text(cu, 0, "Voice", &(CuTextStyle){.font = ui_font(a, 14, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(14));
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_FIT_MIN(52), .align_y = CU_CENTER, .border_b = 1,
                            .border_color = 0xff2e2e2e, CU_PAD2(10, 0)});
    cu_text(cu, 0, "Dictation", &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .w = CU_GROW});
    if (ui_flat_button(a, cu_id(cu, "voice-settings"), NULL, "Voice settings…", NULL, 14, C_TEXT, NULL, true, 0))
        open_voice_settings(a);
    cu_close(cu);
    cu_spacer(cu, CU_PX(1), CU_PX(40));
    cu_text(cu, 0, "Permissions", &(CuTextStyle){.font = ui_font(a, 14, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(14));
    setting_row(a, "Bypass permissions", "Offer the mode that runs edits and commands without asking.", NULL);
    if (ui_switch(a, cu_id(cu, "bypass-enabled"), &a->bypass_enabled, true)) {
        prefs_save(a);
        if (!a->bypass_enabled && a->mode == CH_BYPASS)
            app_set_mode(a, CH_ASK);
    }
    cu_close(cu);
    cu_close(cu);
}
typedef struct {
    App *app;
    unsigned serial;
    ChConfig config;
    ChCancel *cancel;
    ChTunnel *tunnel;
    json_object *models;
    ChError *error;
} LibraryJob;
static void library_work(void *data) {
    LibraryJob *j = data;
    j->tunnel = ch_inferssd_tunnel(&j->config, &j->error);
    json_object *inventory = NULL;
    if (j->tunnel && ch_inferssd_wait(&j->config, j->tunnel, j->cancel, &j->error))
        inventory = ch_inferssd_install(&j->config, j->cancel, &j->error);
    if (inventory) {
        json_object_put(inventory);
        j->models = ch_models(&j->config, j->cancel, &j->error);
    }
}
void app_track_tunnel(App *a, ChTunnel *t);
static void library_done(void *data) {
    LibraryJob *j = data;
    App *a = j->app;
    Settings *s = (Settings *)dialog_find(a, j->serial);
    if (s && !a->closing) {
        bool connected = false;
        if (j->models && json_object_array_length(j->models)) {
            bool found = false;
            for (size_t i = 0; i < json_object_array_length(j->models); i++)
                found |= !strcmp(j->config.model, ch_json_str(json_object_array_get_idx(j->models, i), "id"));
            if (!found)
                ch_strlcpy(j->config.model, ch_json_str(json_object_array_get_idx(j->models, 0), "id"), sizeof(j->config.model));
            ch_clear_error(&j->error);
            if (ch_config_save(&j->config, a->config_path, &j->error)) {
                a->config = j->config;
                app_track_tunnel(a, ch_steal(&j->tunnel));
                char *text = ch_strdup_printf("Model library connected · %zu models · choose one from the model picker",
                                              json_object_array_length(j->models));
                app_set_status(a, text);
                free(text);
                connected = true;
            }
        }
        if (connected)
            dialog_close(a, &s->base);
        else {
            set_result(s, j->error ? j->error->message : "No models found.", j->error ? j->error->message : NULL);
            s->connecting = false;
        }
    }
    if (j->tunnel)
        ch_tunnel_stop(j->tunnel);
    if (j->models)
        json_object_put(j->models);
    ch_clear_error(&j->error);
    ch_cancel_unref(j->cancel);
    free(j);
    cu_invalidate(a->cu);
}
static void connect_library(App *a, Settings *s) {
    ChConfig config = a->config;
    config.provider = 6;
    config.ssh_enabled = true;
    config.ssh_remember = s->remember;
    config.timeout = CH_MAX(config.timeout, 900);
    ch_strlcpy(config.ssh_host, cu_edit_text(&s->host), sizeof(config.ssh_host));
    ch_strlcpy(config.ssh_user, cu_edit_text(&s->user), sizeof(config.ssh_user));
    ch_strlcpy(config.model_dir, cu_edit_text(&s->folder), sizeof(config.model_dir));
    ch_strlcpy(config.inference_binary, cu_edit_text(&s->binary), sizeof(config.inference_binary));
    config.ssh_port = (int)ui_spin_value(&s->ssh_port, config.ssh_port);
    config.remote_port = (int)ui_spin_value(&s->remote_port, config.remote_port);
    config.local_port = (int)ui_spin_value(&s->local_port, config.local_port);
    config.memory_mode = s->memory;
    config.context_size = (int)ui_spin_value(&s->context, config.context_size);
    config.gpu_headroom = (int)ui_spin_value(&s->headroom, config.gpu_headroom);
    ch_strlcpy(config.gpu_device, cu_edit_text(&s->device), sizeof(config.gpu_device));
    snprintf(config.endpoint, sizeof(config.endpoint), "http://127.0.0.1:%d/v1", config.local_port);
    ChError *error = NULL;
    if (!ch_config_validate(&config, &error)) {
        set_result(s, error->message, NULL);
        ch_clear_error(&error);
        return;
    }
    stop_model_tunnel(a);
    LibraryJob *j = ch_new0(LibraryJob, 1);
    j->app = a;
    j->serial = s->base.serial;
    j->config = config;
    if (s->cancel)
        ch_cancel_unref(s->cancel);
    s->cancel = ch_cancel_new();
    j->cancel = ch_cancel_ref(s->cancel);
    s->connecting = true;
    set_result(s, "Signing in and preparing your model library… Follow the SSH login prompt. This can take a minute. "
                  "Cancel stops setup.", NULL);
    app_run(a, library_work, library_done, j);
}
typedef struct {
    App *app;
    unsigned serial;
    ChConfig config;
    bool ok;
    ChError *error;
} Forget;
static void forget_work(void *data) {
    Forget *f = data;
    f->ok = ch_ssh_password_forget(&f->config, NULL, &f->error);
}
static void forget_done(void *data) {
    Forget *f = data;
    Settings *s = (Settings *)dialog_find(f->app, f->serial);
    if (s) {
        set_result(s, f->ok ? "Saved SSH login removed. The next connection will ask you to sign in."
                            : f->error ? f->error->message : "Could not remove the saved login.", NULL);
        s->forgetting = false;
    }
    cu_invalidate(f->app->cu);
    ch_clear_error(&f->error);
    free(f);
}
static void forget_login(App *a, Settings *s) {
    Forget *f = ch_new0(Forget, 1);
    f->app = a;
    f->serial = s->base.serial;
    ch_config_defaults(&f->config);
    ch_strlcpy(f->config.ssh_host, cu_edit_text(&s->host), sizeof(f->config.ssh_host));
    ch_strlcpy(f->config.ssh_user, cu_edit_text(&s->user), sizeof(f->config.ssh_user));
    f->config.ssh_port = (int)ui_spin_value(&s->ssh_port, 22);
    if (!*f->config.ssh_host || !*f->config.ssh_user) {
        set_result(s, "Enter the SSH address and username whose saved login you want to remove.", NULL);
        free(f);
        return;
    }
    s->forgetting = true;
    app_run(a, forget_work, forget_done, f);
}
static void library_field(App *a, const char *label, CuId id, CuEdit *edit, const char *placeholder, size_t max, bool enabled) {
    field_row(a, label);
    ui_entry(a, id, edit, placeholder, enabled, max);
    field_end(a);
}
static void library_spin(App *a, const char *label, CuId id, CuEdit *edit, double min, double max, double step, bool enabled) {
    field_row(a, label);
    ui_spin(a, id, edit, min, max, step, 0, enabled);
    field_end(a);
}
static void connections_page(App *a, Settings *s) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD(24), .gap = 14});
    cu_text(cu, 0, "Connections", &(CuTextStyle){.font = ui_font(a, 22, 600), .color = C_TEXT});
    ui_label_wrap(a, "Turn on your inference computer first. Chela signs in through SSH, finds your downloaded GGUF models, "
                     "and reconnects when you open the app.", 13, C_MUTED, 560);
    bool on = !s->connecting;
    CuId host = cu_id(cu, "lib-host");
    if (!s->focused) {
        cu_focus(cu, host);
        s->focused = true;
    }
    library_field(a, "SSH address", host, &s->host, "10.0.0.55", 255, on);
    library_field(a, "Username", cu_id(cu, "lib-user"), &s->user, "", 127, on);
    library_field(a, "Model folder", cu_id(cu, "lib-folder"), &s->folder, "~/models", 1023, on);
    ui_expander(a, cu_id(cu, "lib-advanced"), "Advanced", &s->advanced);
    if (s->advanced) {
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .gap = 12, .pad_l = 4});
        library_field(a, "Server executable", cu_id(cu, "lib-binary"), &s->binary, "", 1023, on);
        library_spin(a, "SSH port", cu_id(cu, "lib-ssh-port"), &s->ssh_port, 1, 65535, 1, on);
        library_spin(a, "Server port", cu_id(cu, "lib-remote-port"), &s->remote_port, 1, 65535, 1, on);
        library_spin(a, "Local port", cu_id(cu, "lib-local-port"), &s->local_port, 1, 65535, 1, on);
        field_row(a, "Memory use");
        const char *modes[] = {"Automatic GPU + RAM", "MoE experts in RAM", "CPU / RAM only", "InferSSD expert cache"};
        cu_open(cu, cu_id(cu, "lib-memory-field"), &(CuBox){.w = CU_GROW,
            .tooltip = "Automatic fits weights to GPU memory and keeps the rest in RAM. MoE experts in RAM keeps expert calculations on the CPU. CPU / RAM only works without a GPU. InferSSD expert cache runs Qwen3.8-Flash-Next, DeepSeek-V4.1 and Muse through InferSSD's RAM and GPU expert caches (other models load as Automatic); it needs a server built with the expert tier."});
        ui_dropdown(a, cu_id(cu, "lib-memory"), modes, 4, &s->memory, on, false);
        cu_close(cu);
        field_end(a);
        cu_open(cu, cu_id(cu, "lib-device-field"), &(CuBox){.w = CU_GROW,
            .tooltip = "auto selects the first GPU reported by your server; or enter a device such as ROCm0 or Vulkan0."});
        library_field(a, "GPU device", cu_id(cu, "lib-device"), &s->device, "auto", 127, on);
        cu_close(cu);
        library_spin(a, "Context tokens", cu_id(cu, "lib-context"), &s->context, 512, 131072, 512, on);
        library_spin(a, "GPU reserve (MiB)", cu_id(cu, "lib-headroom"), &s->headroom, 256, 65536, 256, on);
        cu_close(cu);
    }
    ui_checkbox(a, cu_id(cu, "lib-remember"), "Remember SSH login in my desktop keyring", &s->remember, on);
    cu_open(cu, 0, &(CuBox){.row = true});
    if (ui_button(a, cu_id(cu, "lib-forget"), "Forget saved SSH login", on && !s->forgetting, false))
        forget_login(a, s);
    cu_close(cu);
    cu_open(cu, cu_id(cu, "lib-result"), &(CuBox){.w = CU_GROW, .tooltip = s->result_tip});
    cu_text(cu, 0, s->result, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_MUTED, .wrap = CU_WRAP_WORD,
                                              .max_lines = 6, .w = CU_GROW_MAX(600)});
    cu_close(cu);
    if (s->connecting) {
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER});
        ui_spinner(a, 16);
        ui_label(a, "Working…", 13, C_MUTED);
        cu_close(cu);
    }
    cu_close(cu);
}
static void setting_row(App *a, const char *title, const char *detail, CuId *row) {
    Cu *cu = a->cu;
    cu_open(cu, row ? *row : 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_FIT_MIN(52), .align_y = CU_CENTER, .border_b = 1, .gap = 16,
                                         .border_color = 0xff2e2e2e, CU_PAD2(10, 0), .interactive = row != NULL});
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .gap = 3});
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT});
    if (detail)
        ui_label_wrap(a, detail, 12, C_MUTED, 460);
    cu_close(cu);
}
static void models_page(App *a, Settings *s) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD(24), .gap = 4});
    cu_text(cu, 0, "Models", &(CuTextStyle){.font = ui_font(a, 22, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(10));
    bool supported = ch_thinking_supported(&a->config), gpt = ch_model_is_gpt_oss(&a->config);
    setting_row(a, "Effort", !supported ? "Set this in your server or harness for this connection."
                             : gpt ? "GPT-OSS always reasons, so Off is the same as Quick."
                                   : "More effort can help with hard tasks, but takes longer and shares your response limit.", NULL);
    const char *levels[] = {"Model default", "Off", "Quick", "Balanced", "Deep"};
    int level = (int)a->config.thinking;
    cu_open(cu, 0, &(CuBox){.w = CU_PX(180)});
    if (ui_dropdown(a, cu_id(cu, "thinking-level"), levels, 5, &level, supported, false)) {
        ChConfig config = a->config;
        config.thinking = (ChThinking)level;
        app_save_config(a, &config, "Could not save effort");
    }
    cu_close(cu);
    cu_close(cu);
    /* Whether to load ahead of time and when to free the memory are separate choices. */
    model_preload_row(a);
    if (model_control_supported(a)) {
        setting_row(a, "Unload when idle", "Frees GPU and RAM while you are not using the model. The next message loads it again.", NULL);
        const char *idle[] = {"After 5 min", "After 15 min", "After 30 min", "After 1 hour", "Never"};
        const int values[] = {300, 900, 1800, 3600, 0};
        int index = 4;
        for (int i = 0; i < 5; i++)
            if (values[i] == a->idle_unload_seconds)
                index = i;
        cu_open(cu, 0, &(CuBox){.w = CU_PX(180)});
        if (ui_dropdown(a, cu_id(cu, "idle-unload"), idle, 5, &index, true, false)) {
            a->idle_unload_seconds = values[index];
            a->model_idle_since = ch_monotonic_us();
            prefs_save(a);
        }
        cu_close(cu);
        cu_close(cu);
    }
    cu_spacer(cu, CU_PX(1), CU_PX(26));
    cu_text(cu, 0, "Current model", &(CuTextStyle){.font = ui_font(a, 14, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(10));
    model_details_build(a);
    cu_spacer(cu, CU_PX(1), CU_PX(26));
    cu_text(cu, 0, "Requests", &(CuTextStyle){.font = ui_font(a, 14, 600), .color = C_TEXT});
    cu_spacer(cu, CU_PX(1), CU_PX(6));
    ui_label_wrap(a, "Used with your direct model connection. External harnesses use their own settings.", 13, C_MUTED, 460);
    cu_spacer(cu, CU_PX(1), CU_PX(8));
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .gap = 14});
    field_row(a, "Max output tokens");
    ui_spin(a, cu_id(cu, "tokens"), &s->tokens, 1, 131072, 256, 0, true);
    field_end(a);
    field_row(a, "Temperature");
    ui_spin(a, cu_id(cu, "temperature"), &s->temperature, 0, 2, 0.1, 1, true);
    field_end(a);
    field_row(a, "Turn timeout (sec)");
    ui_spin(a, cu_id(cu, "timeout"), &s->timeout, 1, 3600, 30, 0, true);
    field_end(a);
    ui_checkbox(a, cu_id(cu, "tools"), "Enable project tools (Chat reads; Agent can edit and run commands)", &s->tools, true);
    cu_close(cu);
    cu_close(cu);
}
static void settings_build(App *a, Dialog *d) {
    Settings *s = (Settings *)d;
    Cu *cu = a->cu;
    bool cancel = dialog_frame_begin(a, d, "Settings", 1024, cu_height(cu) - 72);
    if (top_dialog(a) == d) {
        if (cu_key(cu, 'f', CU_MOD_CTRL))
            cu_focus(cu, cu_id(cu, "settings-search"));
        for (int i = 0; i < 3; i++)
            if (cu_key(cu, '1' + i, CU_MOD_CTRL)) {
                cu_edit_set(&s->search, "");
                s->page = i;
            }
    }
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_GROW});
    cu_open(cu, 0, &(CuBox){.w = CU_PX(212), .h = CU_GROW, .bg = C_BG, .border_r = 1, .border_color = C_BORDER, .pad_t = 16});
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 16, .pad_r = 16});
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 6, .align_y = CU_CENTER, CU_PAD2(4, 8), .radius = 6, .bg = C_FIELD,
                            .border = 1, .border_color = cu_focused(cu, cu_id(cu, "settings-search")) ? C_FOCUS : C_FIELD_BORDER, .w = CU_GROW});
    cu_icon(cu, "edit-find", 16, C_MUTED);
    int search_flags = cu_edit(cu, cu_id(cu, "settings-search"), &s->search,
                               &(CuEditStyle){.font = ui_font(a, 13, 400), .color = C_TEXT, .placeholder_color = 0xff7a7a7a,
                                              .placeholder = "Search", .max_length = 80});
    cu_close(cu);
    cu_close(cu);
    cu_open(cu, 0, &(CuBox){.pad_l = 18, .pad_t = 24, .pad_b = 8});
    ui_label(a, "Preferences", 12, C_MUTED);
    cu_close(cu);
    const char *titles[] = {"General", "Connections", "Models"};
    const char *icons[] = {"preferences-system", "network-server", "thinking"};
    char *query = ch_utf8_casefold(cu_edit_text(&s->search));
    int first = -1;
    for (int i = 0; i < 3; i++) {
        char *title = ch_utf8_casefold(titles[i]);
        bool match = strstr(title, query) != NULL;
        free(title);
        if (!match)
            continue;
        if (first < 0)
            first = i;
        CuId id = cu_idn(cu, "settings-page", i);
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(1, 16)});
        cu_open(cu, id, &(CuBox){.row = true, .gap = 12, .align_y = CU_CENTER, CU_PAD2(8, 10), .radius = 6, .w = CU_GROW,
                                 .bg = s->page == i ? C_SELECTED : cu_hovered(cu, id) ? C_HOVER : 0, .interactive = true});
        cu_icon(cu, icons[i], 18, 0xffd4d4d4);
        cu_text(cu, 0, titles[i], &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT});
        cu_close(cu);
        cu_close(cu);
        if (cu_clicked(cu, id))
            s->page = i;
    }
    free(query);
    if ((search_flags & CU_EDIT_SUBMIT) && first >= 0)
        s->page = first;
    cu_close(cu);
    cu_open(cu, cu_id(cu, "settings-content"), &(CuBox){.w = CU_GROW, .h = CU_GROW, .scroll_y = true});
    if (s->page == 0)
        general_page(a);
    else if (s->page == 1)
        connections_page(a, s);
    else
        models_page(a, s);
    cu_close(cu);
    cu_close(cu);
    int choice = -1;
    if (s->page == SETTINGS_CONNECTIONS)
        dialog_footer_buttons(a, d, "Cancel", "Connect & refresh library", !s->connecting, &choice);
    else if (s->page == SETTINGS_MODELS)
        dialog_footer_buttons(a, d, "Cancel", "Save settings", true, &choice);
    if (choice == 1 && s->page == 1)
        connect_library(a, s);
    else if (choice == 1) {
        ChConfig c = a->config;
        c.max_tokens = (int)CH_CLAMP(ui_spin_value(&s->tokens, c.max_tokens), 1, 131072);
        c.temperature = CH_CLAMP(ui_spin_value(&s->temperature, c.temperature), 0, 2);
        c.timeout = (int)CH_CLAMP(ui_spin_value(&s->timeout, c.timeout), 1, 3600);
        c.tools = s->tools;
        if (app_save_config(a, &c, "Could not save settings")) {
            app_set_status(a, "Settings saved");
            choice = 0;
        }
    }
    if (choice == 0 || cancel) {
        /* A setup still signing in has already stopped the old connection; the next model use reconnects. */
        if (s->at_launch && !s->connecting)
            ensure_model_tunnel(a);
        dialog_close(a, d);
    }
    dialog_frame_end(a);
}
void open_settings(App *a, int page) {
    if (a->busy)
        return;
    for (size_t i = 0; i < a->dialogs.len; i++)
        if (((Dialog *)a->dialogs.data[i])->build == settings_build && !((Dialog *)a->dialogs.data[i])->closed) {
            ((Settings *)a->dialogs.data[i])->page = page;
            return;
        }
    Settings *s = ch_new0(Settings, 1);
    s->base.build = settings_build;
    s->base.destroy = settings_destroy;
    s->page = page;
    CuEdit *edits[] = {&s->search, &s->tokens, &s->temperature, &s->timeout, &s->host, &s->user, &s->folder,
                       &s->binary, &s->ssh_port, &s->remote_port, &s->local_port, &s->device, &s->context, &s->headroom};
    for (size_t i = 0; i < CH_N_ELEMENTS(edits); i++)
        cu_edit_init(edits[i], false);
    const ChConfig *c = &a->config;
    ui_spin_set(&s->tokens, c->max_tokens, 0);
    ui_spin_set(&s->temperature, c->temperature, 1);
    ui_spin_set(&s->timeout, c->timeout, 0);
    s->tools = c->tools;
    cu_edit_set(&s->host, a->setup_host ? a->setup_host : c->ssh_host);
    ch_clear_pointer(&a->setup_host, free);
    s->at_launch = a->setup_at_launch;
    a->setup_at_launch = false;
    cu_edit_set(&s->user, c->ssh_user);
    cu_edit_set(&s->folder, c->model_dir);
    cu_edit_set(&s->binary, c->inference_binary);
    ui_spin_set(&s->ssh_port, c->ssh_port, 0);
    ui_spin_set(&s->remote_port, c->remote_port, 0);
    ui_spin_set(&s->local_port, c->local_port, 0);
    cu_edit_set(&s->device, c->gpu_device);
    ui_spin_set(&s->context, c->context_size, 0);
    ui_spin_set(&s->headroom, c->gpu_headroom, 0);
    s->memory = c->memory_mode;
    s->remember = c->ssh_remember;
    /* The reason the saved connection failed is more useful here than the general advice. */
    set_result(s, a->tunnel_error ? a->tunnel_error
                                  : "Sign in once using your password or an SSH key. Remember login stores the password in "
                                    "your desktop keyring for future connections. Setup loads one model at a time using "
                                    "GPU + RAM. Advanced includes RAM experts and CPU-only modes. After connecting, use "
                                    "Test selected model in the model picker.", NULL);
    dialog_push(a, &s->base);
}

/* A picture at full size, or as large as the window allows. */
typedef struct {
    Dialog base;
    char *path, *name;
} ImageView;
static void image_view_destroy(Dialog *d) {
    ImageView *v = (ImageView *)d;
    free(v->path);
    free(v->name);
}
typedef struct {
    CuImage *image;
    float width, height;
} ViewPaint;
static void paint_view(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    ViewPaint *v = data;
    float scale = fminf(fminf(r.w / v->width, r.h / v->height), 1);
    float w = v->width * scale, h = v->height * scale;
    cu_platform(cu)->draw_image(c, v->image, (CuRect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h}, 6);
}
static void image_view_build(App *a, Dialog *d) {
    ImageView *v = (ImageView *)d;
    Cu *cu = a->cu;
    bool cancel = dialog_frame_begin(a, d, v->name, cu_width(cu) - 96, cu_height(cu) - 96);
    int width = 0, height = 0;
    CuImage *image = thumb_image(a, v->path, 2400, &width, &height);
    cu_open(cu, cu_id(cu, "image-view"), &(CuBox){.w = CU_GROW, .h = CU_GROW, CU_PAD(12), .align_x = CU_CENTER, .align_y = CU_CENTER});
    if (image) {
        ViewPaint *paint = cu_frame_alloc(cu, sizeof(ViewPaint));
        *paint = (ViewPaint){image, (float)width, (float)height};
        cu_custom(cu, 0, CU_GROW, CU_GROW, paint_view, paint);
    } else if (!ch_file_exists(v->path))
        ui_label(a, "This file is no longer available.", 13, C_MUTED);
    else
        ui_spinner(a, 20);
    cu_close(cu);
    if (cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void open_image(App *a, const char *path, const char *name) {
    ImageView *v = ch_new0(ImageView, 1);
    v->base.build = image_view_build;
    v->base.destroy = image_view_destroy;
    v->path = ch_strdup(path);
    v->name = ch_strdup(name);
    dialog_push(a, &v->base);
}

/* Diagnostics */
typedef struct {
    Dialog base;
    char *text;
    bool loading;
    unsigned generation;
} Diagnostics;
typedef struct {
    App *app;
    unsigned serial, generation;
    ChConfig config;
    ChCancel *cancel;
    char *report;
} DiagnosticsJob;
static void diagnostics_work(void *data) {
    DiagnosticsJob *j = data;
    ChBuf report;
    ch_buf_init(&report);
    ch_buf_append(&report, j->report);
    if (j->config.provider != 5) {
        ChConfig check = j->config;
        check.ssh_enabled = false;
        check.timeout = 3;
        ChError *error = NULL;
        json_object *models = ch_models(&check, j->cancel, &error);
        if (models) {
            ch_buf_printf(&report, "\nAPI: reachable (%zu models)\n", json_object_array_length(models));
            for (size_t i = 0; i < json_object_array_length(models); i++) {
                json_object *model = json_object_array_get_idx(models, i);
                if (strcmp(ch_json_str(model, "id"), check.model))
                    continue;
                ch_buf_printf(&report, "Selected model: %s\n", ch_model_detail(model) ? ch_model_detail(model) : "listed; no loading status reported");
                json_object *status = ch_json_get(model, "status");
                if (ch_json_get(status, "exit_code"))
                    ch_buf_printf(&report, "Model process exit code: %d\n", json_object_get_int(ch_json_get(status, "exit_code")));
            }
            json_object_put(models);
        } else
            ch_buf_printf(&report, "\nAPI check failed: %s\n", error ? error->message : "No response");
        ch_clear_error(&error);
        if (j->config.ssh_enabled && !ch_cancel_is_cancelled(j->cancel)) {
            char *logs = ch_inferssd_diagnostics(&j->config, j->cancel, &error);
            ch_buf_printf(&report, "\n%s\n", logs ? logs : error ? error->message : "No server log returned");
            free(logs);
            ch_clear_error(&error);
        } else if (!j->config.ssh_enabled)
            ch_buf_append(&report, "\nServer logs: available through a managed SSH connection. For this direct connection, inspect the server's own console/log.\n");
    } else
        ch_buf_append(&report, "\nACP harness: inspect the harness output for its provider's server status.\n");
    free(j->report);
    j->report = ch_buf_steal(&report);
}
static void diagnostics_done(void *data) {
    DiagnosticsJob *j = data;
    Diagnostics *d = (Diagnostics *)dialog_find(j->app, j->serial);
    if (d && d->generation == j->generation) {
        free(d->text);
        d->text = ch_steal(&j->report);
        d->loading = false;
    }
    free(j->report);
    ch_cancel_unref(j->cancel);
    cu_invalidate(j->app->cu);
    free(j);
}
static void diagnostics_refresh(App *a, Diagnostics *d) {
    DiagnosticsJob *j = ch_new0(DiagnosticsJob, 1);
    j->app = a;
    j->serial = d->base.serial;
    j->generation = ++d->generation;
    j->config = a->config;
    j->cancel = ch_cancel_new();
    j->report = diagnostics_snapshot(a);
    free(d->text);
    d->text = ch_strdup(j->report);
    d->loading = true;
    app_run(a, diagnostics_work, diagnostics_done, j);
}
static void diagnostics_destroy(Dialog *d) {
    free(((Diagnostics *)d)->text);
}
static void diagnostics_build(App *a, Dialog *d) {
    Diagnostics *g = (Diagnostics *)d;
    bool cancel = dialog_frame_begin(a, d, "Server diagnostics", 840, 580);
    body_begin(a, true);
    ui_mono_text(a, cu_id(a->cu, "diagnostics-text"), g->text, 12.5f, true);
    if (g->loading) {
        cu_open(a->cu, 0, &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER});
        ui_spinner(a, 14);
        ui_label(a, "Checking server status…", 13, C_MUTED);
        cu_close(a->cu);
    }
    body_end(a);
    int choice = -1;
    dialog_footer_buttons(a, d, "Close", "Refresh", !g->loading, &choice);
    if (choice == 1)
        diagnostics_refresh(a, g);
    else if (choice == 0 || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void open_diagnostics(App *a) {
    Diagnostics *d = ch_new0(Diagnostics, 1);
    d->base.build = diagnostics_build;
    d->base.destroy = diagnostics_destroy;
    dialog_push(a, &d->base);
    diagnostics_refresh(a, d);
}

/* Model test */
typedef struct {
    Dialog base;
    char *text;
    bool done;
    ChCancel *cancel;
} ModelTest;
typedef struct {
    App *app;
    unsigned serial;
    ChRun run;
    char *text;
} ModelTestJob;
static void test_work(void *data) {
    ModelTestJob *j = data;
    json_object *messages = json_object_new_array();
    json_object_array_add(messages, ch_message("user", "Reply with one short sentence confirming you are ready."));
    ChError *error = NULL;
    json_object *answer = ch_complete(&j->run, messages, &error);
    json_object_put(messages);
    if (answer && *ch_json_str(answer, "content")) {
        char *reply = ch_utf8_substring(ch_json_str(answer, "content"), 0, CH_MIN(600, ch_utf8_strlen(ch_json_str(answer, "content"), -1)));
        j->text = ch_strdup_printf("Model loaded and replied:\n\n%s\n\nCheck the server log to confirm GPU offloading and memory use.", reply);
        free(reply);
    } else
        j->text = ch_strdup_printf("Model test failed: %s", error ? error->message
                                   : "The model returned no answer within the test's output limit. Try Quick thinking or check the server logs.");
    if (answer)
        json_object_put(answer);
    ch_clear_error(&error);
}
static void test_done(void *data) {
    ModelTestJob *j = data;
    ModelTest *t = (ModelTest *)dialog_find(j->app, j->serial);
    if (t) {
        free(t->text);
        t->text = ch_steal(&j->text);
        t->done = true;
    }
    free(j->text);
    ch_cancel_unref(j->run.cancel);
    cu_invalidate(j->app->cu);
    free(j);
}
static void test_destroy(Dialog *d) {
    ModelTest *t = (ModelTest *)d;
    free(t->text);
    ch_cancel_cancel(t->cancel);
    ch_cancel_unref(t->cancel);
}
static void test_build(App *a, Dialog *d) {
    ModelTest *t = (ModelTest *)d;
    bool cancel = dialog_frame_begin(a, d, "Test selected model", 580, 0);
    body_begin(a, false);
    if (!t->done) {
        cu_open(a->cu, 0, &(CuBox){.row = true, .gap = 10, .align_y = CU_CENTER});
        ui_spinner(a, 16);
        cu_close(a->cu);
    }
    ui_text_block(a, cu_id(a->cu, "test-text"), t->text, 14, C_TEXT2, true);
    body_end(a);
    int choice;
    if (dialog_footer_buttons(a, d, t->done ? "Close" : "Cancel", NULL, false, &choice) || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void open_model_test(App *a) {
    if (a->busy || a->config.provider == 5 || !ensure_model_tunnel(a))
        return;
    ModelTest *t = ch_new0(ModelTest, 1);
    t->base.build = test_build;
    t->base.destroy = test_destroy;
    t->text = ch_strdup("Loading the selected model and checking a real response…\nThe first load can take several minutes.");
    t->cancel = ch_cancel_new();
    dialog_push(a, &t->base);
    ModelTestJob *j = ch_new0(ModelTestJob, 1);
    j->app = a;
    j->serial = t->base.serial;
    j->run.config = a->config;
    j->run.config.tools = false;
    j->run.config.max_tokens = 512;
    j->run.config.thinking = CH_THINK_QUICK;
    j->run.cancel = ch_cancel_ref(t->cancel);
    app_run(a, test_work, test_done, j);
}

/* Voice */
typedef struct {
    Dialog base;
    CuEdit edit;
    bool focused;
} Voice;
static void voice_destroy(Dialog *d) {
    cu_edit_clear(&((Voice *)d)->edit);
}
static void voice_build(App *a, Dialog *d) {
    Voice *v = (Voice *)d;
    Cu *cu = a->cu;
    bool cancel = dialog_frame_begin(a, d, "Voice settings", 560, 0);
    body_begin(a, false);
    cu_text(cu, 0, "Dictation", &(CuTextStyle){.font = ui_font(a, 22, 600), .color = C_TEXT});
    ui_label_wrap(a, "Choose a local transcription app that records your microphone and returns text. Dictation adds its "
                     "transcript to your draft for review before sending.", 13, C_MUTED, 500);
    ui_label(a, "Transcription command", 14, C_TEXT);
    CuId entry = cu_id(cu, "voice-command");
    if (!v->focused) {
        cu_focus(cu, entry);
        v->focused = true;
    }
    int flags = ui_entry(a, entry, &v->edit, "/path/to/your-transcription-app", true, 2047);
    ui_label_wrap(a, "The command runs directly, without a shell. It must print UTF-8 text to standard output. Choose the "
                     "microphone in your transcription app. Spoken replies are not available yet.", 13, C_MUTED, 500);
    body_end(a);
    int choice = -1;
    dialog_footer_buttons(a, d, "Cancel", "Save", true, &choice);
    if (choice == 1 || (flags & CU_EDIT_SUBMIT)) {
        const char *command = cu_edit_text(&v->edit);
        char **argv = NULL;
        ChError *error = NULL;
        if (*command && !ch_shell_parse_argv(command, NULL, &argv, &error)) {
            app_notice(a, "Check the transcription command", error->message);
            ch_clear_error(&error);
        } else {
            ch_strfreev(argv);
            free(a->dictation_command);
            a->dictation_command = ch_strdup(command);
            prefs_save(a);
            dialog_close(a, d);
        }
    } else if (choice == 0 || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void open_voice_settings(App *a) {
    if (a->busy || a->dictation)
        return;
    Voice *v = ch_new0(Voice, 1);
    v->base.build = voice_build;
    v->base.destroy = voice_destroy;
    cu_edit_init(&v->edit, false);
    cu_edit_set(&v->edit, a->dictation_command ? a->dictation_command : "");
    dialog_push(a, &v->base);
}
static void listening_build(App *a, Dialog *d) {
    bool cancel = dialog_frame_begin(a, d, "Listening…", 460, 0);
    body_begin(a, false);
    cu_open(a->cu, 0, &(CuBox){.row = true, .gap = 10, .align_y = CU_CENTER});
    ui_spinner(a, 16);
    ui_label_wrap(a, "Finish recording in your transcription app. Your words will appear in the message box.", 14, C_TEXT2, 380);
    cu_close(a->cu);
    body_end(a);
    int choice;
    if (dialog_footer_buttons(a, d, "Stop dictation", NULL, false, &choice) || cancel) {
        stop_dictation(a);
        dialog_close(a, d);
    }
    dialog_frame_end(a);
}
unsigned open_listening(App *a) {
    Dialog *d = ch_new0(Dialog, 1);
    d->build = listening_build;
    return dialog_push(a, d)->serial;
}

/* Tool approval */
typedef struct {
    Dialog base;
    Job *job;
    char *title, *detail;
} Approval;
static void approval_answer(App *a, Approval *p, bool allowed) {
    Job *j = p->job;
    if (j) {
        ch_mutex_lock(&j->mutex);
        j->allowed = allowed && !ch_cancel_is_cancelled(j->run.cancel);
        j->answered = true;
        ch_cond_signal(&j->condition);
        ch_mutex_unlock(&j->mutex);
        p->job = NULL;
    }
    a->approval_dialog = 0;
    dialog_close(a, &p->base);
}
static void approval_destroy(Dialog *d) {
    Approval *p = (Approval *)d;
    free(p->title);
    free(p->detail);
}
static void approval_build(App *a, Dialog *d) {
    Approval *p = (Approval *)d;
    bool cancel = dialog_frame_begin(a, d, p->title, 800, 580);
    body_begin(a, true);
    ui_label(a, "Review this action before allowing it in your project.", 13, C_MUTED);
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .bg = 0xff1a1a1a, .border = 1, .border_color = 0xff333333, .radius = 8, CU_PAD2(10, 12)});
    ui_mono_text(a, cu_id(a->cu, "approval-detail"), p->detail, 12.5f, true);
    cu_close(a->cu);
    body_end(a);
    int choice = -1;
    dialog_footer_buttons(a, d, "Decline", "Allow once", true, &choice);
    if (choice == 1)
        approval_answer(a, p, true);
    else if (choice == 0 || cancel)
        approval_answer(a, p, false);
    dialog_frame_end(a);
}
unsigned open_approval(App *a, Job *job) {
    Approval *p = ch_new0(Approval, 1);
    p->base.build = approval_build;
    p->base.destroy = approval_destroy;
    p->job = job;
    p->title = ch_strdup(job->permission_title);
    p->detail = ch_strdup(job->permission_detail);
    return dialog_push(a, &p->base)->serial;
}
void close_approval(App *a) {
    Approval *p = (Approval *)dialog_find(a, a->approval_dialog);
    if (p)
        approval_answer(a, p, false);
}

/* All sessions */
typedef struct {
    Dialog base;
    ChVec *sessions;
    int selected;
} AllSessions;
static void all_destroy(Dialog *d) {
    ch_vec_free(((AllSessions *)d)->sessions);
}
static void open_from_all(App *a, const ChSession *session) {
    char *copy = ch_strdup(session->id);
    ChError *error = NULL;
    if (session->archived && !ch_store_set_archived(a->store, copy, false, &error)) {
        app_notice(a, "Could not restore session", error->message);
        ch_clear_error(&error);
    }
    a->activity_days = 0;
    cu_edit_set(&a->search, "");
    app_refresh_sessions(a);
    app_select_session(a, copy);
    free(copy);
}
static void all_build(App *a, Dialog *d) {
    AllSessions *s = (AllSessions *)d;
    Cu *cu = a->cu;
    bool cancel = dialog_frame_begin(a, d, "All sessions", 800, 560);
    body_begin(a, true);
    const ChSession *open = NULL;
    for (size_t i = 0; s->sessions && i < s->sessions->len; i++) {
        ChSession *v = s->sessions->data[i];
        CuId id = cu_ids(cu, "all-row", v->id);
        cu_open(cu, id, &(CuBox){.w = CU_GROW, .gap = 5, CU_PAD2(8, 10), .radius = 6, .interactive = true,
                                 .bg = s->selected == (int)i ? C_SELECTED : cu_hovered(cu, id) ? C_HOVER : 0});
        cu_text(cu, 0, v->title, &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
        if (*v->project)
            cu_text(cu, 0, v->project, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_GROW});
        if (v->archived)
            cu_text(cu, 0, "Archived", &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_FAINT});
        cu_close(cu);
        if (cu_double_clicked(cu, id))
            open = v;
        else if (cu_clicked(cu, id))
            s->selected = (int)i;
    }
    if (!s->sessions || !s->sessions->len)
        ui_label(a, "No sessions yet.", 13, C_FAINT);
    body_end(a);
    int choice = -1;
    const ChSession *chosen = s->selected >= 0 ? s->sessions->data[s->selected] : NULL;
    dialog_footer_buttons(a, d, "Close", chosen && chosen->archived ? "Restore session" : "Open session", chosen && !a->busy, &choice);
    if (choice == 1 && !open)
        open = chosen;
    if (open && !a->busy) {
        dialog_close(a, d);
        open_from_all(a, open);
    } else if (choice == 0 || cancel)
        dialog_close(a, d);
    dialog_frame_end(a);
}
void open_all_sessions(App *a) {
    AllSessions *s = ch_new0(AllSessions, 1);
    s->base.build = all_build;
    s->base.destroy = all_destroy;
    s->sessions = ch_store_list(a->store, "", NULL);
    /* The list of the tab it was opened from. */
    for (size_t i = 0; s->sessions && i < s->sessions->len;) {
        ChSession *v = s->sessions->data[i];
        if (v->code != a->code)
            ch_vec_remove(s->sessions, i);
        else
            i++;
    }
    s->selected = -1;
    dialog_push(a, &s->base);
}
