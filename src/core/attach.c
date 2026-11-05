/* Attachments: images and files added to a message. Their bytes live in a private,
 * content-addressed folder rather than in the session database, so a transcript stays
 * small and searchable; the message keeps a reference under "_chela_attachments" and
 * they are read back when a request is built. */
#include "chela.h"
#include <errno.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BINARY_LIMIT (64u * 1024u * 1024u)
#define IMAGE_LIMIT (32u * 1024u * 1024u)

const char *ch_mime_sniff(const void *data, size_t size) {
    const unsigned char *d = data;
    if (size >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8))
        return "image/png";
    if (size >= 3 && d[0] == 0xff && d[1] == 0xd8 && d[2] == 0xff)
        return "image/jpeg";
    if (size >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6)))
        return "image/gif";
    if (size >= 12 && !memcmp(d, "RIFF", 4) && !memcmp(d + 8, "WEBP", 4))
        return "image/webp";
    if (size >= 2 && d[0] == 'B' && d[1] == 'M')
        return "image/bmp";
    if (size >= 4 && (!memcmp(d, "II*\0", 4) || !memcmp(d, "MM\0*", 4)))
        return "image/tiff";
    if (size >= 5 && !memcmp(d, "%PDF-", 5))
        return "application/pdf";
    return NULL;
}
void ch_attachment_free(void *data) {
    ChAttachment *a = data;
    if (a) {
        free(a->name);
        free(a->path);
        free(a->mime);
        free(a);
    }
}
static char *store_bytes(const char *folder, const void *data, size_t size, const char *extension, ChError **error) {
    if (ch_mkdir_p(folder, 0700)) {
        ch_set_error(error, CH_ERR_IO, errno, "Cannot prepare the attachments folder: %s", strerror(errno));
        return NULL;
    }
    char *digest = ch_sha256_hex(data, size), *name = ch_strconcat(digest, extension, NULL);
    char *path = ch_build_path(folder, name, NULL);
    free(digest);
    free(name);
    /* The same content is the same file, so an existing copy is already right. */
    if (!ch_file_exists(path) && !ch_file_set_contents(path, data, (ptrdiff_t)size, 0600, error))
        ch_clear_pointer(&path, free);
    return path;
}
static bool text_content(const char *data, size_t size) {
    return !memchr(data, 0, size) && ch_utf8_validate(data, (ptrdiff_t)size, NULL);
}
ChAttachment *ch_attachment_from_bytes(const void *data, size_t size, const char *name, const char *folder, ChError **error) {
    if (!size) {
        ch_fail(error, "%s is empty.", name);
        return NULL;
    }
    const char *mime = ch_mime_sniff(data, size);
    ChAttachment *a = ch_new0(ChAttachment, 1);
    a->name = ch_strdup(name);
    a->size = size;
    if (mime && ch_str_has_prefix(mime, "image/")) {
        if (size > IMAGE_LIMIT) {
            ch_fail(error, "%s is larger than the 32 MiB image limit.", name);
            ch_attachment_free(a);
            return NULL;
        }
        ChImage *image = ch_image_decode(data, size, error);
        if (!image) {
            ch_attachment_free(a);
            return NULL;
        }
        a->kind = CH_ATTACH_IMAGE;
        a->width = image->width;
        a->height = image->height;
        /* Model servers read PNG and JPEG; every other format is converted once, here. */
        if (!strcmp(mime, "image/png") || !strcmp(mime, "image/jpeg")) {
            a->mime = ch_strdup(mime);
            a->path = store_bytes(folder, data, size, !strcmp(mime, "image/png") ? ".png" : ".jpg", error);
        } else {
            size_t length = 0;
            char *png = ch_image_png(image, &length);
            a->mime = ch_strdup("image/png");
            if (png) {
                a->size = length;
                a->path = store_bytes(folder, png, length, ".png", error);
            } else
                ch_fail(error, "Could not convert %s.", name);
            free(png);
        }
        ch_image_free(image);
    } else if (text_content(data, size)) {
        if (size > CH_ATTACH_TEXT_LIMIT) {
            ch_fail(error, "%s is larger than the 256 KiB limit for text files.", name);
            ch_attachment_free(a);
            return NULL;
        }
        a->kind = CH_ATTACH_TEXT;
        a->mime = ch_strdup("text/plain");
        a->path = store_bytes(folder, data, size, ".txt", error);
    } else {
        a->kind = CH_ATTACH_FILE;
        a->mime = ch_strdup(mime ? mime : "application/octet-stream");
        a->path = store_bytes(folder, data, size, ".bin", error);
    }
    if (!a->path) {
        ch_attachment_free(a);
        return NULL;
    }
    return a;
}
ChAttachment *ch_attachment_from_path(const char *path, const char *folder, ChError **error) {
    struct stat st;
    if (stat(path, &st)) {
        ch_set_error(error, CH_ERR_IO, errno, "Cannot read %s: %s", path, strerror(errno));
        return NULL;
    }
    char *name = ch_path_basename(path);
    ChAttachment *a = NULL;
    if (S_ISDIR(st.st_mode) || (uint64_t)st.st_size > BINARY_LIMIT) {
        /* Folders and very large files are referred to by path, for tools to open. */
        a = ch_new0(ChAttachment, 1);
        a->kind = CH_ATTACH_FILE;
        a->name = ch_steal(&name);
        a->path = ch_strdup(path);
        a->mime = ch_strdup(S_ISDIR(st.st_mode) ? "inode/directory" : "application/octet-stream");
        a->size = (size_t)st.st_size;
        a->external = true;
    } else if (!S_ISREG(st.st_mode))
        ch_fail(error, "%s is not a file.", name);
    else {
        char *data = NULL;
        size_t size = 0;
        if (ch_file_get_contents(path, &data, &size, error))
            a = ch_attachment_from_bytes(data, size, name, folder, error);
        if (a && a->kind == CH_ATTACH_FILE) {
            /* Binary files stay where they are: nothing in a request reads their bytes. */
            if (a->path)
                unlink(a->path);
            free(a->path);
            a->path = ch_strdup(path);
            a->external = true;
        }
        free(data);
    }
    free(name);
    return a;
}
json_object *ch_attachment_json(const ChAttachment *a) {
    json_object *o = json_object_new_object();
    const char *kinds[] = {"text", "image", "file"};
    json_object_object_add(o, "kind", json_object_new_string(kinds[a->kind]));
    json_object_object_add(o, "name", json_object_new_string(a->name));
    json_object_object_add(o, "path", json_object_new_string(a->path));
    json_object_object_add(o, "mime", json_object_new_string(a->mime));
    json_object_object_add(o, "size", json_object_new_int64((int64_t)a->size));
    if (a->external)
        json_object_object_add(o, "external", json_object_new_boolean(true));
    if (a->kind == CH_ATTACH_IMAGE) {
        json_object_object_add(o, "width", json_object_new_int(a->width));
        json_object_object_add(o, "height", json_object_new_int(a->height));
    }
    return o;
}
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
char *ch_base64_encode(const void *data, size_t size) {
    const unsigned char *d = data;
    char *out = ch_alloc((size + 2) / 3 * 4 + 1), *p = out;
    size_t i = 0;
    for (; i + 2 < size; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (uint32_t)d[i + 1] << 8 | d[i + 2];
        *p++ = b64[v >> 18], *p++ = b64[v >> 12 & 63], *p++ = b64[v >> 6 & 63], *p++ = b64[v & 63];
    }
    if (i < size) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < size ? (uint32_t)d[i + 1] << 8 : 0);
        *p++ = b64[v >> 18], *p++ = b64[v >> 12 & 63];
        *p++ = i + 1 < size ? b64[v >> 6 & 63] : '=';
        *p++ = '=';
    }
    *p = 0;
    return out;
}
/* A fence longer than any run of backticks in the text, so the file cannot close it. */
static char *fence_for(const char *text) {
    size_t longest = 2, run = 0;
    for (; *text; text++) {
        run = *text == '`' ? run + 1 : 0;
        longest = CH_MAX(longest, run);
    }
    char *fence = ch_alloc(longest + 2);
    memset(fence, '`', longest + 1);
    fence[longest + 1] = 0;
    return fence;
}
static void describe(ChBuf *b, json_object *a, bool inline_images) {
    const char *kind = ch_json_str(a, "kind"), *name = ch_json_str(a, "name"), *path = ch_json_str(a, "path");
    if (!strcmp(kind, "image")) {
        if (!inline_images)
            ch_buf_printf(b, "\n\nAttached image: %s (%s)", name, path);
        return;
    }
    if (!strcmp(kind, "file")) {
        ch_buf_printf(b, "\n\nAttached %s, not included in this message: %s (%s)",
                      !strcmp(ch_json_str(a, "mime"), "inode/directory") ? "folder" : "file", name, path);
        return;
    }
    char *text = NULL;
    if (!ch_file_get_contents(path, &text, NULL, NULL)) {
        ch_buf_printf(b, "\n\nAttached file %s is no longer available.", name);
        return;
    }
    char *fence = fence_for(text);
    ch_buf_printf(b, "\n\nAttached file: %s\n%s\n%s\n%s", name, fence, text, fence);
    free(fence);
    free(text);
}
char *ch_message_text(json_object *message) {
    ChBuf b;
    ch_buf_init(&b);
    ch_buf_append(&b, ch_json_str(message, "content"));
    json_object *list = ch_json_get(message, "_chela_attachments");
    for (size_t i = 0; list && i < json_object_array_length(list); i++)
        describe(&b, json_object_array_get_idx(list, i), false);
    return ch_buf_steal(&b);
}
json_object *ch_wire_message(json_object *message, bool images_allowed) {
    json_object *copy = json_object_new_object();
    json_object_object_foreach(message, key, value) {
        if (!ch_str_has_prefix(key, "_chela_"))
            json_object_object_add(copy, key, json_object_get(value));
    }
    json_object *list = ch_json_get(message, "_chela_attachments");
    if (!list || !json_object_array_length(list))
        return copy;
    ChBuf text;
    ch_buf_init(&text);
    ch_buf_append(&text, ch_json_str(message, "content"));
    json_object *images = json_object_new_array();
    for (size_t i = 0; i < json_object_array_length(list); i++) {
        json_object *a = json_object_array_get_idx(list, i);
        if (strcmp(ch_json_str(a, "kind"), "image")) {
            describe(&text, a, true);
            continue;
        }
        /* An image from earlier in a session must not stop a model without vision answering the rest. */
        if (!images_allowed) {
            ch_buf_printf(&text, "\n\nAttached image %s was left out: this model cannot read images.", ch_json_str(a, "name"));
            continue;
        }
        char *data = NULL;
        size_t size = 0;
        if (!ch_file_get_contents(ch_json_str(a, "path"), &data, &size, NULL)) {
            ch_buf_printf(&text, "\n\nAttached image %s is no longer available.", ch_json_str(a, "name"));
            continue;
        }
        char *encoded = ch_base64_encode(data, size);
        char *url = ch_strconcat("data:", ch_json_str(a, "mime"), ";base64,", encoded, NULL);
        json_object *part = json_object_new_object(), *image = json_object_new_object();
        json_object_object_add(image, "url", json_object_new_string(url));
        json_object_object_add(part, "type", json_object_new_string("image_url"));
        json_object_object_add(part, "image_url", image);
        json_object_array_add(images, part);
        free(url);
        free(encoded);
        free(data);
    }
    if (!json_object_array_length(images)) {
        json_object_object_add(copy, "content", json_object_new_string(text.str));
        json_object_put(images);
    } else {
        /* OpenAI's multi-part form: the text first, then each image. */
        json_object *parts = json_object_new_array(), *first = json_object_new_object();
        json_object_object_add(first, "type", json_object_new_string("text"));
        json_object_object_add(first, "text", json_object_new_string(text.str));
        if (text.len)
            json_object_array_add(parts, first);
        else
            json_object_put(first);
        for (size_t i = 0; i < json_object_array_length(images); i++)
            json_object_array_add(parts, json_object_get(json_object_array_get_idx(images, i)));
        json_object_put(images);
        json_object_object_add(copy, "content", parts);
    }
    ch_buf_clear(&text);
    return copy;
}
static int hex_value(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
/* The local path of a file: URI, or NULL for anything else. */
static char *uri_path(const char *uri) {
    if (strncasecmp(uri, "file:", 5))
        return NULL;
    const char *p = uri + 5;
    if (!strncmp(p, "//", 2)) {
        p += 2;
        if (!strncasecmp(p, "localhost/", 10))
            p += 9;
        else if (*p != '/')
            return NULL; /* a file on another host */
    }
    if (*p != '/')
        return NULL;
    ChBuf out;
    ch_buf_init(&out);
    for (; *p && *p != '?' && *p != '#'; p++) {
        if (*p != '%') {
            ch_buf_append_c(&out, *p);
            continue;
        }
        int high = p[1] ? hex_value(p[1]) : -1, low = high >= 0 && p[2] ? hex_value(p[2]) : -1;
        if (low < 0 || (!high && !low)) {
            ch_buf_clear(&out);
            return NULL;
        }
        ch_buf_append_c(&out, (char)(high * 16 + low));
        p += 2;
    }
    return ch_buf_steal(&out);
}
ChVec *ch_uri_list_parse(const char *text) {
    ChVec *paths = ch_vec_new(free);
    char **lines = ch_strsplit(text ? text : "", "\n");
    for (size_t i = 0; lines[i]; i++) {
        char *line = ch_strstrip(lines[i]);
        if (!*line || *line == '#')
            continue;
        char *path = uri_path(line);
        if (path)
            ch_vec_push(paths, path);
    }
    ch_strfreev(lines);
    return paths;
}
