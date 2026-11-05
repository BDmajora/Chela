/* Attachments: pasted and dropped data, the tiles above the composer and in the
 * transcript, and the thumbnails they show. Reading and decoding run in the
 * background, so a large photo never holds up typing. */
#include "app.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MAX_ATTACHMENTS 16
#define TILE 92.0f
#define THUMB_SIDE 512

typedef struct {
    CuImage *image;
    int width, height;
    bool loading;
} Thumb;
typedef struct {
    App *app;
    char *key, *path;
    int side;
    ChImage *result;
} ThumbJob;
static void thumb_free(void *data) {
    (void)data; /* images are released in attachments_free, which knows the platform */
}
static void thumb_work(void *data) {
    ThumbJob *j = data;
    char *bytes = NULL;
    size_t size = 0;
    if (ch_file_get_contents(j->path, &bytes, &size, NULL)) {
        ChImage *image = ch_image_decode(bytes, size, NULL);
        if (image) {
            j->result = ch_image_fit(image, j->side);
            ch_image_free(image);
        }
    }
    free(bytes);
}
static void thumb_done(void *data) {
    ThumbJob *j = data;
    App *a = j->app;
    Thumb *t = ch_map_get(&a->thumbs, j->key);
    if (t) {
        t->loading = false;
        if (j->result && a->p->image_new) {
            t->image = a->p->image_new(j->result->pixels, j->result->width, j->result->height);
            t->width = j->result->width;
            t->height = j->result->height;
        }
    }
    ch_image_free(j->result);
    free(j->key);
    free(j->path);
    free(j);
    cu_invalidate(a->cu);
}
CuImage *thumb_image(App *a, const char *path, int side, int *width, int *height) {
    char *key = ch_strdup_printf("%d:%s", side, path);
    Thumb *t = ch_map_get(&a->thumbs, key);
    if (!t && !a->closing) {
        t = ch_new0(Thumb, 1);
        t->loading = true;
        ch_map_set(&a->thumbs, key, t);
        ThumbJob *j = ch_new0(ThumbJob, 1);
        *j = (ThumbJob){a, ch_strdup(key), ch_strdup(path), side, NULL};
        app_run(a, thumb_work, thumb_done, j);
    }
    free(key);
    if (!t || !t->image)
        return NULL;
    if (width)
        *width = t->width;
    if (height)
        *height = t->height;
    return t->image;
}

void attachments_init(App *a) {
    ch_vec_init(&a->attachments, ch_attachment_free);
    ch_vec_init(&a->tab_attachments, ch_attachment_free);
    ch_map_init(&a->thumbs, thumb_free);
    char *data = ch_user_data_dir();
    a->attachment_folder = ch_build_path(data, "chela", "attachments", NULL);
    free(data);
}
void attachments_clear(App *a) {
    ch_vec_clear(&a->attachments);
    ch_vec_init(&a->attachments, ch_attachment_free);
    a->attach_generation++; /* whatever is still being read belonged to this draft */
    a->attach_pending = 0;
}
void attachments_swap(App *a, ChVec *other) {
    ChVec current = a->attachments;
    a->attachments = *other;
    *other = current;
    a->attach_generation++;
    a->attach_pending = 0;
}
void attachments_free(App *a) {
    ch_vec_clear(&a->attachments);
    ch_vec_clear(&a->tab_attachments);
    for (size_t i = 0; i < a->thumbs.len; i++) {
        Thumb *t = a->thumbs.values[i];
        if (t->image && a->p->image_free)
            a->p->image_free(t->image);
        free(t);
    }
    ch_map_clear(&a->thumbs);
    free(a->attachment_folder);
}

typedef struct {
    App *app;
    unsigned generation, order;
    char *path, *data, *name, *folder;
    size_t size;
    ChAttachment *result;
    ChError *error;
} AttachJob;
static void attach_work(void *data) {
    AttachJob *j = data;
    j->result = j->path ? ch_attachment_from_path(j->path, j->folder, &j->error)
                        : ch_attachment_from_bytes(j->data, j->size, j->name, j->folder, &j->error);
}
static void attach_done(void *data) {
    AttachJob *j = data;
    App *a = j->app;
    if (j->generation == a->attach_generation && !a->closing) {
        a->attach_pending--;
        if (j->result) {
            /* Reading finishes in any order; tiles stay in the order they were added. */
            j->result->order = j->order;
            size_t at = a->attachments.len;
            while (at && ((ChAttachment *)a->attachments.data[at - 1])->order > j->order)
                at--;
            ch_vec_insert(&a->attachments, at, j->result);
            j->result = NULL;
        } else if (j->error)
            app_note(a, j->error->message);
    }
    ch_attachment_free(j->result);
    ch_clear_error(&j->error);
    free(j->path);
    free(j->data);
    free(j->name);
    free(j->folder);
    free(j);
    cu_invalidate(a->cu);
}
static bool attach_allowed(App *a) {
    if (a->busy) {
        app_note(a, "Wait for the reply to finish before adding files.");
        return false;
    }
    if (a->attachments.len + (size_t)a->attach_pending >= MAX_ATTACHMENTS) {
        app_note(a, "A message can hold up to 16 attachments.");
        return false;
    }
    return true;
}
static void attach_start(App *a, AttachJob *j) {
    j->app = a;
    j->generation = a->attach_generation;
    j->folder = ch_strdup(a->attachment_folder);
    j->order = ++a->attach_order;
    a->attach_pending++;
    app_run(a, attach_work, attach_done, j);
    cu_invalidate(a->cu);
}
void attach_path(App *a, const char *path) {
    if (!attach_allowed(a))
        return;
    AttachJob *j = ch_new0(AttachJob, 1);
    j->path = ch_strdup(path);
    attach_start(a, j);
}
void attachments_take_payloads(App *a) {
    CuPayload p;
    while (cu_payload_take(a->cu, &p)) {
        bool uris = !strcmp(p.mime, "text/uri-list") || !strcmp(p.mime, "x-special/gnome-copied-files");
        if (uris) {
            ChVec *paths = ch_uri_list_parse(p.data);
            for (size_t i = 0; i < paths->len; i++)
                attach_path(a, paths->data[i]);
            if (!paths->len)
                app_note(a, "Only files on this computer can be attached.");
            ch_vec_free(paths);
        } else if (ch_str_has_prefix(p.mime, "text/") || !strcmp(p.mime, "UTF8_STRING")) {
            /* Text dropped from another window lands in the draft, as if typed. */
            cu_edit_insert(&a->composer, p.data);
            cu_focus(a->cu, cu_id(a->cu, "composer"));
        } else if (attach_allowed(a)) {
            AttachJob *j = ch_new0(AttachJob, 1);
            j->data = ch_steal(&p.data);
            j->size = p.size;
            j->name = ch_strdup(ch_str_has_prefix(p.mime, "image/") ? "Pasted image" : "Pasted file");
            attach_start(a, j);
        }
        cu_payload_clear(&p);
    }
}
json_object *attachments_json(App *a) {
    if (!a->attachments.len)
        return NULL;
    json_object *list = json_object_new_array();
    for (size_t i = 0; i < a->attachments.len; i++)
        json_object_array_add(list, ch_attachment_json(a->attachments.data[i]));
    return list;
}
void attachments_forget(App *a, json_object *messages) {
    size_t prefix = strlen(a->attachment_folder);
    for (size_t i = 0; i < json_object_array_length(messages); i++) {
        json_object *list = ch_json_get(json_object_array_get_idx(messages, i), "_chela_attachments");
        for (size_t k = 0; list && k < json_object_array_length(list); k++) {
            const char *path = ch_json_str(json_object_array_get_idx(list, k), "path");
            /* Only Chela's own copies, and only when no other session shows the same file. */
            if (strncmp(path, a->attachment_folder, prefix) || path[prefix] != '/')
                continue;
            if (!ch_store_mentions(a->store, path + prefix + 1))
                unlink(path);
        }
    }
}

/* Tiles */
typedef struct {
    CuImage *image;
    int width, height;
} TilePaint;
static void paint_tile(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    TilePaint *t = data;
    const CuPlatform *p = cu_platform(cu);
    if (!t->image || !p->draw_image)
        return;
    /* The whole image, centred, like a preview: never cropped. */
    float scale = fminf(r.w / (float)t->width, r.h / (float)t->height);
    float w = (float)t->width * scale, h = (float)t->height * scale;
    p->draw_image(c, t->image, (CuRect){r.x + (r.w - w) / 2, r.y + (r.h - h) / 2, w, h}, 8);
}
static char *size_text(size_t size) {
    double kib = (double)size / 1024;
    return kib < 1 ? ch_strdup_printf("%zu B", size) : kib < 1024 ? ch_strdup_printf("%.0f KB", kib)
                                                                  : ch_strdup_printf("%.1f MB", kib / 1024);
}
static char *extension_of(const char *name) {
    const char *dot = strrchr(name, '.');
    char *upper = ch_strdup(dot && dot[1] && strlen(dot + 1) <= 5 ? dot + 1 : "FILE");
    for (char *p = upper; *p; p++)
        if (*p >= 'a' && *p <= 'z')
            *p = (char)(*p - 'a' + 'A');
    return upper;
}
/* A file's name and type, for anything that has no picture. */
static void file_face(App *a, const char *name, const char *kind, size_t size, bool folder) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .h = CU_GROW, .gap = 3});
    cu_icon(cu, folder ? "folder" : !strcmp(kind, "text") ? "file-text" : "file", 18, 0xffb9b9b9);
    cu_spacer(cu, CU_PX(1), CU_GROW);
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_TEXT, .wrap = CU_WRAP_CHAR, .max_lines = 2, .w = CU_GROW});
    char *extension = extension_of(name), *amount = size_text(size);
    char *line = folder ? ch_strdup("Folder") : ch_strdup_printf("%s · %s", extension, amount);
    cu_text(cu, 0, line, &(CuTextStyle){.font = ui_font(a, 10, 400), .color = C_FAINT, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
    free(line);
    free(extension);
    free(amount);
    cu_close(cu);
}
static void remove_button(App *a, CuId id, bool *removed) {
    Cu *cu = a->cu;
    cu_open(cu, id, &(CuBox){CU_PAD(3), .radius = 11, .bg = cu_hovered(cu, id) ? 0xff555555 : 0xdd2a2a2a, .interactive = true,
                             .border = 1, .border_color = 0xff555555, .tooltip = "Remove"});
    cu_icon(cu, "window-close", 12, C_TEXT);
    cu_close(cu);
    if (cu_clicked(cu, id))
        *removed = true;
}
void attachments_row(App *a) {
    Cu *cu = a->cu;
    if (!a->attachments.len && !a->attach_pending)
        return;
    /* Tiles wrap by whole rows: as many as fit the composer's text column. */
    float available = content_width(a) - 96;
    size_t per_row = (size_t)CH_MAX(available / (TILE + 8), 1), count = a->attachments.len, shown = 0;
    cu_open(cu, cu_id(cu, "attachments"), &(CuBox){.w = CU_GROW, .gap = 8, .pad_b = 8, .pad_t = 2});
    while (shown < count || (!shown && a->attach_pending)) {
        cu_open(cu, 0, &(CuBox){.row = true, .gap = 8});
        size_t end = CH_MIN(shown + per_row, count);
        for (size_t i = shown; i < end; i++) {
            ChAttachment *item = a->attachments.data[i];
            cu_push_id(cu, (CuId)(uintptr_t)item);
            CuId id = cu_id(cu, "attachment"), remove = cu_id(cu, "attachment-remove");
            TilePaint *paint = cu_frame_alloc(cu, sizeof(TilePaint));
            *paint = (TilePaint){0};
            if (item->kind == CH_ATTACH_IMAGE)
                paint->image = thumb_image(a, item->path, THUMB_SIDE, &paint->width, &paint->height);
            bool removed = false;
            cu_open(cu, id, &(CuBox){.w = CU_PX(TILE), .h = CU_PX(TILE), .radius = 10, .bg = 0xff1f1f1f, .border = 1,
                                     .border_color = cu_hovered_within(cu, id) ? 0xff666666 : 0xff484848, .interactive = true,
                                     .tooltip = item->name, .paint = item->kind == CH_ATTACH_IMAGE ? paint_tile : NULL,
                                     .paint_data = paint, CU_PAD(item->kind == CH_ATTACH_IMAGE ? 4 : 8),
                                     .align_x = item->kind == CH_ATTACH_IMAGE ? CU_END : CU_START});
            if (item->kind == CH_ATTACH_IMAGE) {
                if (cu_hovered_within(cu, id))
                    remove_button(a, remove, &removed);
            } else {
                cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .h = CU_GROW});
                file_face(a, item->name, item->kind == CH_ATTACH_TEXT ? "text" : "file", item->size,
                          !strcmp(item->mime, "inode/directory"));
                if (cu_hovered_within(cu, id))
                    remove_button(a, remove, &removed);
                cu_close(cu);
            }
            cu_close(cu);
            if (cu_clicked(cu, id) && item->kind == CH_ATTACH_IMAGE && !removed)
                open_image(a, item->path, item->name);
            cu_pop_id(cu);
            if (removed) {
                ch_vec_remove(&a->attachments, i);
                cu_invalidate(cu);
                end = i; /* the rest of this row moved; the next frame lays it out */
                count = a->attachments.len;
                shown = count;
                break;
            }
        }
        if (shown <= end && end == count && a->attach_pending) {
            cu_open(cu, 0, &(CuBox){.w = CU_PX(TILE), .h = CU_PX(TILE), .radius = 10, .bg = 0xff1f1f1f, .border = 1,
                                    .border_color = 0xff484848, .align_x = CU_CENTER, .align_y = CU_CENTER});
            ui_spinner(a, 16);
            cu_close(cu);
        }
        cu_close(cu);
        shown = end;
        if (shown >= count)
            break;
    }
    cu_close(cu);
}
void attachment_sent(App *a, CuId id, json_object *attachment) {
    Cu *cu = a->cu;
    const char *kind = ch_json_str(attachment, "kind"), *path = ch_json_str(attachment, "path"), *name = ch_json_str(attachment, "name");
    if (!strcmp(kind, "image")) {
        TilePaint *paint = cu_frame_alloc(cu, sizeof(TilePaint));
        *paint = (TilePaint){0};
        paint->image = thumb_image(a, path, THUMB_SIDE, &paint->width, &paint->height);
        double width = json_object_get_double(ch_json_get(attachment, "width")), height = json_object_get_double(ch_json_get(attachment, "height"));
        float h = 150, w = height > 0 ? CH_CLAMP((float)(width / height) * h, 90, 280) : 150;
        cu_open(cu, id, &(CuBox){.w = CU_PX(w), .h = CU_PX(h), .radius = 10, .bg = 0xff1f1f1f, .border = 1, .border_color = 0xff3a3a3a,
                                 .interactive = true, .tooltip = name, .paint = paint_tile, .paint_data = paint, CU_PAD(2)});
        cu_close(cu);
        if (cu_clicked(cu, id))
            open_image(a, path, name);
        return;
    }
    cu_open(cu, id, &(CuBox){.w = CU_PX(TILE + 28), .h = CU_PX(TILE), .radius = 10, .bg = 0xff1f1f1f, .border = 1, .border_color = 0xff3a3a3a,
                             CU_PAD(8), .tooltip = name});
    file_face(a, name, kind, (size_t)json_object_get_int64(ch_json_get(attachment, "size")), !strcmp(ch_json_str(attachment, "mime"), "inode/directory"));
    cu_close(cu);
}
