/* Image decoding for attachments. PNG goes through cairo, which the desktop links
 * anyway; JPEG, GIF, BMP, TIFF and WebP use the system's gdk-pixbuf and libwebp when
 * they are installed, loaded at run time so neither is a build dependency. Everything
 * is untrusted input: sizes are checked before pixels are allocated. */
#include "chela.h"
#include <cairo.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIDE 16384
#define MAX_PIXELS (64u * 1024u * 1024u)

static bool size_ok(int width, int height) {
    return width > 0 && height > 0 && width <= MAX_SIDE && height <= MAX_SIDE &&
           (uint64_t)width * (uint64_t)height <= MAX_PIXELS;
}
void ch_image_free(ChImage *image) {
    if (image) {
        free(image->pixels);
        free(image);
    }
}
static ChImage *image_new(int width, int height) {
    ChImage *image = ch_new0(ChImage, 1);
    image->width = width;
    image->height = height;
    image->pixels = ch_alloc((size_t)width * (size_t)height * 4);
    return image;
}
static uint32_t premultiplied(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint32_t pr = (uint32_t)r * a / 255, pg = (uint32_t)g * a / 255, pb = (uint32_t)b * a / 255;
    return (uint32_t)a << 24 | pr << 16 | pg << 8 | pb;
}

typedef struct {
    const unsigned char *data;
    size_t size, position;
} PngInput;
static cairo_status_t png_read(void *closure, unsigned char *out, unsigned int length) {
    PngInput *in = closure;
    if (in->size - in->position < length)
        return CAIRO_STATUS_READ_ERROR;
    memcpy(out, in->data + in->position, length);
    in->position += length;
    return CAIRO_STATUS_SUCCESS;
}
static ChImage *decode_png(const unsigned char *data, size_t size) {
    /* IHDR, to refuse a decompression bomb before decoding it. */
    if (size < 24)
        return NULL;
    uint32_t width = (uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 | (uint32_t)data[18] << 8 | data[19];
    uint32_t height = (uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 | (uint32_t)data[22] << 8 | data[23];
    if (width > MAX_SIDE || height > MAX_SIDE || !size_ok((int)width, (int)height))
        return NULL;
    PngInput in = {data, size, 0};
    cairo_surface_t *surface = cairo_image_surface_create_from_png_stream(png_read, &in);
    ChImage *image = NULL;
    if (cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS) {
        cairo_surface_flush(surface);
        int w = cairo_image_surface_get_width(surface), h = cairo_image_surface_get_height(surface);
        int stride = cairo_image_surface_get_stride(surface);
        cairo_format_t format = cairo_image_surface_get_format(surface);
        const unsigned char *pixels = cairo_image_surface_get_data(surface);
        if (pixels && size_ok(w, h) && (format == CAIRO_FORMAT_ARGB32 || format == CAIRO_FORMAT_RGB24)) {
            image = image_new(w, h);
            for (int y = 0; y < h; y++) {
                const uint32_t *row = (const uint32_t *)(const void *)(pixels + (size_t)y * (size_t)stride);
                for (int x = 0; x < w; x++)
                    image->pixels[(size_t)y * (size_t)w + (size_t)x] = format == CAIRO_FORMAT_RGB24 ? row[x] | 0xff000000u : row[x];
            }
        }
    }
    cairo_surface_destroy(surface);
    return image;
}

static ChImage *decode_webp(const unsigned char *data, size_t size) {
    static void *library;
    static bool tried;
    /* dlopen is thread-safe, and a repeated load of the same library is harmless. */
    if (!tried) {
        library = dlopen("libwebp.so.7", RTLD_NOW | RTLD_LOCAL);
        tried = true;
    }
    if (!library)
        return NULL;
    int (*info)(const uint8_t *, size_t, int *, int *) = (int (*)(const uint8_t *, size_t, int *, int *))dlsym(library, "WebPGetInfo");
    uint8_t *(*decode)(const uint8_t *, size_t, int *, int *) = (uint8_t * (*)(const uint8_t *, size_t, int *, int *)) dlsym(library, "WebPDecodeBGRA");
    void (*release)(void *) = (void (*)(void *))dlsym(library, "WebPFree");
    int width = 0, height = 0;
    if (!info || !decode || !info(data, size, &width, &height) || !size_ok(width, height))
        return NULL;
    uint8_t *pixels = decode(data, size, &width, &height);
    if (!pixels)
        return NULL;
    ChImage *image = NULL;
    if (size_ok(width, height)) {
        image = image_new(width, height);
        size_t count = (size_t)width * (size_t)height;
        for (size_t i = 0; i < count; i++) {
            const uint8_t *p = pixels + i * 4;
            image->pixels[i] = premultiplied(p[2], p[1], p[0], p[3]);
        }
    }
    if (release)
        release(pixels);
    else
        free(pixels);
    return image;
}

/* The small part of gdk-pixbuf's C API that loading a buffer needs. */
typedef struct {
    void *(*loader_new)(void);
    int (*loader_write)(void *, const unsigned char *, size_t, void *);
    int (*loader_close)(void *, void *);
    void *(*loader_pixbuf)(void *);
    void *(*oriented)(void *);
    int (*width)(const void *), (*height)(const void *), (*stride)(const void *), (*channels)(const void *);
    unsigned char *(*pixels)(const void *);
    void (*unref)(void *);
} Pixbuf;
static bool pixbuf_api(Pixbuf *api) {
    void *pixbuf = dlopen("libgdk_pixbuf-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    void *gobject = dlopen("libgobject-2.0.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!pixbuf || !gobject)
        return false;
    void *symbols[] = {dlsym(pixbuf, "gdk_pixbuf_loader_new"), dlsym(pixbuf, "gdk_pixbuf_loader_write"),
                       dlsym(pixbuf, "gdk_pixbuf_loader_close"), dlsym(pixbuf, "gdk_pixbuf_loader_get_pixbuf"),
                       dlsym(pixbuf, "gdk_pixbuf_apply_embedded_orientation"), dlsym(pixbuf, "gdk_pixbuf_get_width"),
                       dlsym(pixbuf, "gdk_pixbuf_get_height"), dlsym(pixbuf, "gdk_pixbuf_get_rowstride"),
                       dlsym(pixbuf, "gdk_pixbuf_get_n_channels"), dlsym(pixbuf, "gdk_pixbuf_get_pixels"),
                       dlsym(gobject, "g_object_unref")};
    for (size_t i = 0; i < CH_N_ELEMENTS(symbols); i++)
        if (!symbols[i])
            return false;
    memcpy(api, symbols, sizeof(symbols));
    return true;
}
static ChImage *decode_pixbuf(const unsigned char *data, size_t size) {
    Pixbuf api;
    if (!pixbuf_api(&api))
        return NULL;
    void *loader = api.loader_new();
    bool wrote = api.loader_write(loader, data, size, NULL);
    bool closed = api.loader_close(loader, NULL);
    void *pixbuf = wrote && closed ? api.loader_pixbuf(loader) : NULL;
    /* Photos store their rotation in EXIF. */
    void *upright = pixbuf ? api.oriented(pixbuf) : NULL;
    ChImage *image = NULL;
    if (upright) {
        int width = api.width(upright), height = api.height(upright), stride = api.stride(upright);
        int channels = api.channels(upright);
        const unsigned char *pixels = api.pixels(upright);
        if (size_ok(width, height) && pixels && (channels == 3 || channels == 4)) {
            image = image_new(width, height);
            for (int y = 0; y < height; y++) {
                const unsigned char *row = pixels + (size_t)y * (size_t)stride;
                for (int x = 0; x < width; x++) {
                    const unsigned char *p = row + (size_t)x * (size_t)channels;
                    image->pixels[(size_t)y * (size_t)width + (size_t)x] = premultiplied(p[0], p[1], p[2], channels == 4 ? p[3] : 255);
                }
            }
        }
        api.unref(upright);
    }
    api.unref(loader);
    return image;
}
ChImage *ch_image_decode(const void *data, size_t size, ChError **error) {
    const char *mime = ch_mime_sniff(data, size);
    ChImage *image = NULL;
    if (mime && !strcmp(mime, "image/png"))
        image = decode_png(data, size);
    else if (mime && !strcmp(mime, "image/webp"))
        image = decode_webp(data, size);
    else if (mime && ch_str_has_prefix(mime, "image/"))
        image = decode_pixbuf(data, size);
    if (!image)
        ch_fail(error, "%s", !mime ? "This is not an image Chela can read."
                             : !strcmp(mime, "image/png") ? "This PNG is damaged or too large."
                                                          : "Chela could not read this image. It may be damaged, too large, or "
                                                            "need gdk-pixbuf or libwebp.");
    return image;
}
/* Downscales by averaging, in premultiplied space so transparent edges stay clean. */
ChImage *ch_image_fit(const ChImage *source, int side) {
    double scale = CH_MIN(1.0, (double)side / CH_MAX(source->width, source->height));
    int width = CH_MAX(1, (int)(source->width * scale)), height = CH_MAX(1, (int)(source->height * scale));
    ChImage *out = image_new(width, height);
    for (int y = 0; y < height; y++) {
        int y0 = (int)((int64_t)y * source->height / height), y1 = CH_MAX(y0 + 1, (int)((int64_t)(y + 1) * source->height / height));
        for (int x = 0; x < width; x++) {
            int x0 = (int)((int64_t)x * source->width / width), x1 = CH_MAX(x0 + 1, (int)((int64_t)(x + 1) * source->width / width));
            uint64_t a = 0, r = 0, g = 0, b = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    uint32_t p = source->pixels[(size_t)sy * (size_t)source->width + (size_t)sx];
                    a += p >> 24, r += p >> 16 & 255, g += p >> 8 & 255, b += p & 255;
                }
            uint64_t n = (uint64_t)(x1 - x0) * (uint64_t)(y1 - y0);
            out->pixels[(size_t)y * (size_t)width + (size_t)x] =
                (uint32_t)(a / n) << 24 | (uint32_t)(r / n) << 16 | (uint32_t)(g / n) << 8 | (uint32_t)(b / n);
        }
    }
    return out;
}
static cairo_status_t png_write(void *closure, const unsigned char *data, unsigned int length) {
    ch_buf_append_len(closure, (const char *)data, length);
    return CAIRO_STATUS_SUCCESS;
}
char *ch_image_png(const ChImage *image, size_t *length) {
    cairo_surface_t *surface = cairo_image_surface_create_for_data((unsigned char *)image->pixels, CAIRO_FORMAT_ARGB32,
                                                                   image->width, image->height, image->width * 4);
    ChBuf out;
    ch_buf_init(&out);
    cairo_status_t status = cairo_surface_write_to_png_stream(surface, png_write, &out);
    cairo_surface_destroy(surface);
    if (status != CAIRO_STATUS_SUCCESS) {
        ch_buf_clear(&out);
        return NULL;
    }
    *length = out.len;
    return ch_buf_steal(&out);
}
