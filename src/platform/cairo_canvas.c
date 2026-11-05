#include "cairo_canvas.h"
#include "../core/base.h"
#include <cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H
#include <math.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    char *file;
    int index;
    FT_Face ft;
    cairo_font_face_t *face;
    cairo_scaled_font_t *measure; /* at 1 px, identity transform, unhinted metrics */
    bool color;
    /* Colour fonts: GSUB ligature subtables, in lookup order, for emoji sequences. */
    bool gsub_loaded;
    uint8_t *gsub;
    size_t gsub_len;
    struct {
        unsigned lookup;
        size_t offset;
    } *ligatures;
    int ligature_count;
} Face;
typedef struct {
    uint32_t glyph;
    float advance;
    Face *face;
} Advance;
struct CuFont {
    CuFontDesc desc;
    char *family;
    float size;
    FcFontSet *set;
    Face **faces; /* lazily opened, parallel to set */
    FcCharSet **coverage;
    bool *color; /* colour (emoji) fonts, parallel to set */
    int count;
    CuFontMetrics metrics;
    struct {
        uint32_t codepoint;
        int face;
    } map[1024];
    Advance *advances; /* cache keyed by face slot and glyph */
    size_t advance_cap;
};
static FT_Library library;
static Face **faces;
static int face_count;
static CuFont **fonts;
static int font_count;
static cairo_font_options_t *options;

static Face *open_face(const char *file, int index) {
    for (int i = 0; i < face_count; i++)
        if (faces[i]->index == index && !strcmp(faces[i]->file, file))
            return faces[i];
    if (!library && FT_Init_FreeType(&library))
        return NULL;
    FT_Face ft;
    if (FT_New_Face(library, file, index, &ft))
        return NULL;
    Face *f = ch_new0(Face, 1);
    f->file = ch_strdup(file);
    f->index = index;
    f->ft = ft;
    f->color = FT_HAS_COLOR(ft);
    f->face = cairo_ft_font_face_create_for_ft_face(ft, f->color ? FT_LOAD_COLOR : 0);
    cairo_matrix_t font_matrix, ctm;
    cairo_matrix_init_scale(&font_matrix, 1, 1);
    cairo_matrix_init_identity(&ctm);
    f->measure = cairo_scaled_font_create(f->face, &font_matrix, &ctm, options);
    faces = ch_realloc(faces, sizeof(Face *) * (size_t)(face_count + 1));
    faces[face_count++] = f;
    return f;
}
static int fc_weight(int css) {
    return css >= 700 ? FC_WEIGHT_BOLD : css >= 600 ? FC_WEIGHT_DEMIBOLD : css >= 500 ? FC_WEIGHT_MEDIUM
         : css <= 300 ? FC_WEIGHT_LIGHT : FC_WEIGHT_REGULAR;
}
static Face *font_face(CuFont *f, int i) {
    if (i < 0 || i >= f->count)
        return NULL;
    if (!f->faces[i]) {
        FcChar8 *file = NULL;
        int index = 0;
        if (FcPatternGetString(f->set->fonts[i], FC_FILE, 0, &file) != FcResultMatch)
            return NULL;
        FcPatternGetInteger(f->set->fonts[i], FC_INDEX, 0, &index);
        f->faces[i] = open_face((const char *)file, index);
    }
    return f->faces[i];
}
static CuFont *font_get(const CuFontDesc *desc) {
    const char *family = desc->family && *desc->family ? desc->family : "sans-serif";
    for (int i = 0; i < font_count; i++) {
        CuFont *f = fonts[i];
        if (f->size == desc->size && f->desc.weight == desc->weight && f->desc.italic == desc->italic &&
            f->desc.monospace == desc->monospace && !strcmp(f->family, family))
            return f;
    }
    if (!options) {
        FcInit();
        options = cairo_font_options_create();
        cairo_font_options_set_antialias(options, CAIRO_ANTIALIAS_GRAY);
        cairo_font_options_set_hint_style(options, CAIRO_HINT_STYLE_SLIGHT);
        cairo_font_options_set_hint_metrics(options, CAIRO_HINT_METRICS_OFF);
    }
    CuFont *f = ch_new0(CuFont, 1);
    f->desc = *desc;
    f->family = ch_strdup(family);
    f->desc.family = f->family;
    f->size = desc->size;
    FcPattern *pattern = FcPatternCreate();
    char **names = ch_strsplit(family, ",");
    for (int i = 0; names[i]; i++) {
        char *name = ch_strstrip(names[i]);
        size_t n = strlen(name);
        if (n >= 2 && (name[0] == '\'' || name[0] == '"')) {
            name[n - 1] = 0;
            name++;
        }
        if (*name)
            FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)name);
    }
    ch_strfreev(names);
    if (desc->monospace)
        FcPatternAddString(pattern, FC_FAMILY, (const FcChar8 *)"monospace");
    FcPatternAddInteger(pattern, FC_WEIGHT, fc_weight(desc->weight));
    FcPatternAddInteger(pattern, FC_SLANT, desc->italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcPatternAddDouble(pattern, FC_PIXEL_SIZE, desc->size);
    FcConfigSubstitute(NULL, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    f->set = FcFontSort(NULL, pattern, FcTrue, NULL, &result);
    FcPatternDestroy(pattern);
    f->count = f->set ? f->set->nfont : 0; /* every fallback; faces open on first use */
    f->faces = ch_new0(Face *, (size_t)(f->count + 1));
    f->coverage = ch_new0(FcCharSet *, (size_t)(f->count + 1));
    f->color = ch_new0(bool, (size_t)(f->count + 1));
    for (int i = 0; i < f->count; i++) {
        FcBool color = FcFalse;
        FcPatternGetCharSet(f->set->fonts[i], FC_CHARSET, 0, &f->coverage[i]);
        FcPatternGetBool(f->set->fonts[i], FC_COLOR, 0, &color);
        f->color[i] = color;
    }
    for (size_t i = 0; i < CH_N_ELEMENTS(f->map); i++)
        f->map[i].codepoint = UINT32_MAX;
    Face *primary = font_face(f, 0);
    if (primary) {
        cairo_font_extents_t e;
        cairo_scaled_font_extents(primary->measure, &e);
        f->metrics.ascent = (float)(e.ascent * desc->size);
        f->metrics.descent = (float)(e.descent * desc->size);
    } else {
        f->metrics.ascent = desc->size * 0.8f;
        f->metrics.descent = desc->size * 0.2f;
    }
    f->metrics.height = ceilf(f->metrics.ascent + f->metrics.descent);
    fonts = ch_realloc(fonts, sizeof(CuFont *) * (size_t)(font_count + 1));
    fonts[font_count++] = f;
    return f;
}
static CuFontMetrics font_metrics(CuFont *f) {
    return f->metrics;
}
/* Characters drawn as emoji unless followed by U+FE0E (Unicode Emoji_Presentation=Yes). */
static bool emoji_default(uint32_t c) {
    static const uint32_t ranges[][2] = {
        {0x231a, 0x231b}, {0x23e9, 0x23ec}, {0x23f0, 0x23f0}, {0x23f3, 0x23f3}, {0x25fd, 0x25fe}, {0x2614, 0x2615},
        {0x2648, 0x2653}, {0x267f, 0x267f}, {0x2693, 0x2693}, {0x26a1, 0x26a1}, {0x26aa, 0x26ab}, {0x26bd, 0x26be},
        {0x26c4, 0x26c5}, {0x26ce, 0x26ce}, {0x26d4, 0x26d4}, {0x26ea, 0x26ea}, {0x26f2, 0x26f3}, {0x26f5, 0x26f5},
        {0x26fa, 0x26fa}, {0x26fd, 0x26fd}, {0x2705, 0x2705}, {0x270a, 0x270b}, {0x2728, 0x2728}, {0x274c, 0x274c},
        {0x274e, 0x274e}, {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2795, 0x2797}, {0x27b0, 0x27b0}, {0x27bf, 0x27bf},
        {0x2b1b, 0x2b1c}, {0x2b50, 0x2b50}, {0x2b55, 0x2b55}, {0x1f004, 0x1f004}, {0x1f0cf, 0x1f0cf},
        {0x1f18e, 0x1f18e}, {0x1f191, 0x1f19a}, {0x1f1e6, 0x1f1ff}, {0x1f201, 0x1f201}, {0x1f21a, 0x1f21a},
        {0x1f22f, 0x1f22f}, {0x1f232, 0x1f236}, {0x1f238, 0x1f23a}, {0x1f250, 0x1f251}, {0x1f300, 0x1f320},
        {0x1f32d, 0x1f335}, {0x1f337, 0x1f37c}, {0x1f37e, 0x1f393}, {0x1f3a0, 0x1f3ca}, {0x1f3cf, 0x1f3d3},
        {0x1f3e0, 0x1f3f0}, {0x1f3f4, 0x1f3f4}, {0x1f3f8, 0x1f43e}, {0x1f440, 0x1f440}, {0x1f442, 0x1f4fc},
        {0x1f4ff, 0x1f53d}, {0x1f54b, 0x1f54e}, {0x1f550, 0x1f567}, {0x1f57a, 0x1f57a}, {0x1f595, 0x1f596},
        {0x1f5a4, 0x1f5a4}, {0x1f5fb, 0x1f64f}, {0x1f680, 0x1f6c5}, {0x1f6cc, 0x1f6cc}, {0x1f6d0, 0x1f6d2},
        {0x1f6d5, 0x1f6d7}, {0x1f6dc, 0x1f6df}, {0x1f6eb, 0x1f6ec}, {0x1f6f4, 0x1f6fc}, {0x1f7e0, 0x1f7eb},
        {0x1f7f0, 0x1f7f0}, {0x1f90c, 0x1f93a}, {0x1f93c, 0x1f945}, {0x1f947, 0x1f9ff}, {0x1fa70, 0x1faff},
    };
    if (c < ranges[0][0])
        return false;
    for (size_t i = 0; i < CH_N_ELEMENTS(ranges); i++)
        if (c >= ranges[i][0] && c <= ranges[i][1])
            return true;
    return false;
}
/* The face that covers a character, preferring the requested families, and colour
 * fonts for emoji presentation. */
static int face_for(CuFont *f, uint32_t c, bool emoji) {
    uint32_t key = c | (emoji ? 0x80000000u : 0);
    size_t slot = c % CH_N_ELEMENTS(f->map);
    if (f->map[slot].codepoint == key)
        return f->map[slot].face;
    int found = -1;
    for (int pass = emoji ? 0 : 1; pass < 2 && found < 0; pass++)
        for (int i = 0; i < f->count; i++)
            if ((pass || f->color[i]) && f->coverage[i] && FcCharSetHasChar(f->coverage[i], c)) {
                found = i;
                break;
            }
    if (found < 0)
        found = 0;
    f->map[slot].codepoint = key;
    f->map[slot].face = found;
    return found;
}
static float advance(CuFont *f, int slot, uint32_t glyph) {
    if (!f->advance_cap) {
        f->advance_cap = 4096;
        f->advances = ch_new0(Advance, f->advance_cap);
    }
    Face *face = font_face(f, slot);
    if (!face)
        return f->size * 0.5f;
    size_t i = ((size_t)glyph * 31 + (size_t)slot) % f->advance_cap;
    Advance *a = &f->advances[i];
    if (a->face == face && a->glyph == glyph)
        return a->advance;
    cairo_glyph_t g = {glyph, 0, 0};
    cairo_text_extents_t e;
    cairo_scaled_font_glyph_extents(face->measure, &g, 1, &e);
    float adv = (float)e.x_advance * f->size;
    *a = (Advance){glyph, adv, face};
    return adv;
}
/* Emoji sequences (flags, skin tones, ZWJ sequences, keycaps) are ligatures in the
 * colour font's GSUB table. Without a shaping library, apply its ligature lookups
 * to runs drawn with that face. */
static unsigned be16(const Face *f, size_t at) {
    return at + 2 <= f->gsub_len ? (unsigned)(f->gsub[at] << 8 | f->gsub[at + 1]) : 0;
}
static size_t be32(const Face *f, size_t at) {
    return (size_t)be16(f, at) << 16 | be16(f, at + 2);
}
static void load_ligatures(Face *f) {
    f->gsub_loaded = true;
    FT_ULong length = 0;
    FT_ULong tag = FT_MAKE_TAG('G', 'S', 'U', 'B');
    if (FT_Load_Sfnt_Table(f->ft, tag, 0, NULL, &length) || length < 10)
        return;
    f->gsub = ch_alloc(length);
    f->gsub_len = length;
    if (FT_Load_Sfnt_Table(f->ft, tag, 0, f->gsub, &length)) {
        ch_clear_pointer(&f->gsub, free);
        f->gsub_len = 0;
        return;
    }
    size_t list = be16(f, 8);
    unsigned lookups = be16(f, list);
    for (unsigned i = 0; i < lookups; i++) {
        size_t lookup = list + be16(f, list + 2 + 2 * i);
        unsigned type = be16(f, lookup), subtables = be16(f, lookup + 4);
        for (unsigned k = 0; k < subtables; k++) {
            size_t sub = lookup + be16(f, lookup + 6 + 2 * k);
            unsigned kind = type;
            if (type == 7 && be16(f, sub) == 1) { /* extension: 32-bit offset to the real subtable */
                kind = be16(f, sub + 2);
                sub += be32(f, sub + 4);
            }
            if (kind != 4 || be16(f, sub) != 1)
                continue;
            f->ligatures = ch_realloc(f->ligatures, sizeof(*f->ligatures) * (size_t)(f->ligature_count + 1));
            f->ligatures[f->ligature_count].lookup = i;
            f->ligatures[f->ligature_count++].offset = sub;
        }
    }
}
static int coverage_index(const Face *f, size_t table, unsigned glyph) {
    unsigned format = be16(f, table), count = be16(f, table + 2);
    size_t lo = 0, hi = count;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        size_t at = table + 4 + (format == 1 ? 2 : 6) * mid;
        unsigned first = be16(f, at), last = format == 1 ? first : be16(f, at + 2);
        if (glyph < first)
            hi = mid;
        else if (glyph > last)
            lo = mid + 1;
        else
            return format == 1 ? (int)mid : format == 2 ? (int)(be16(f, at + 4) + glyph - first) : -1;
    }
    return -1;
}
typedef struct {
    int slot;
    uint32_t glyph, c;
    float x;
} Glyph;
/* Replaces the preferred ligature starting at g[i]; returns how many glyphs it consumed. */
static int ligature_at(const Face *f, size_t sub, Glyph *g, int n, int i) {
    int index = coverage_index(f, sub + be16(f, sub + 2), g[i].glyph);
    if (index < 0 || index >= (int)be16(f, sub + 4))
        return 0;
    size_t set = sub + be16(f, sub + 6 + 2 * (size_t)index);
    unsigned count = be16(f, set);
    for (unsigned k = 0; k < count; k++) {
        size_t lig = set + be16(f, set + 2 + 2 * k);
        int components = (int)be16(f, lig + 2), c = 1;
        if (components < 2 || i + components > n)
            continue;
        while (c < components && g[i + c].slot == g[i].slot && be16(f, lig + 4 + 2 * (size_t)(c - 1)) == g[i + c].glyph)
            c++;
        if (c == components) {
            g[i].glyph = be16(f, lig);
            return components;
        }
    }
    return 0;
}
static int apply_ligatures(CuFont *f, Glyph *g, int n) {
    for (int start = 0; start < n;) {
        int end = start + 1;
        while (end < n && g[end].slot == g[start].slot)
            end++;
        Face *face = font_face(f, g[start].slot);
        if (face && face->color && end - start > 1) {
            if (!face->gsub_loaded)
                load_ligatures(face);
            for (int l = 0; l < face->ligature_count;) {
                int group = l;
                while (group < face->ligature_count && face->ligatures[group].lookup == face->ligatures[l].lookup)
                    group++;
                for (int i = start; i < end - 1; i++)
                    for (int s = l; s < group; s++) {
                        int used = ligature_at(face, face->ligatures[s].offset, g + start, end - start, i - start);
                        if (used > 1) {
                            memmove(g + i + 1, g + i + used, sizeof(Glyph) * (size_t)(n - i - used));
                            n -= used - 1;
                            end -= used - 1;
                            break;
                        }
                    }
                l = group;
            }
        }
        start = end;
    }
    return n;
}
/* Joiners, variation selectors, keycaps, skin tones and tags that extend an emoji. */
static bool emoji_continues(uint32_t c) {
    return c == 0x200d || c == 0xfe0f || c == 0x20e3 || (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0xe0020 && c <= 0xe007f);
}
static bool invisible(uint32_t c) {
    return c == 0x200b || c == 0x200d || c == 0xfe0e || c == 0xfe0f;
}
static int shape(CuFont *f, const char *text, size_t length, Glyph *out, int cap, float *width) {
    int n = 0;
    const char *p = text, *end = text + length;
    while (p < end && n < cap) {
        uint32_t c = ch_utf8_get(p, &p);
        if (c == '\t')
            c = ' ';
        int prev = n ? out[n - 1].slot : -1;
        bool in_emoji = prev >= 0 && f->color[prev] && emoji_continues(c) && f->coverage[prev] &&
                        FcCharSetHasChar(f->coverage[prev], c);
        if (invisible(c) && !in_emoji)
            continue;
        uint32_t next = p < end ? ch_utf8_get(p, NULL) : 0;
        int slot = in_emoji ? prev : face_for(f, c, next == 0xfe0f || (next != 0xfe0e && emoji_default(c)));
        Face *face = font_face(f, slot);
        out[n++] = (Glyph){slot, face ? FT_Get_Char_Index(face->ft, c) : 0, c, 0};
    }
    n = apply_ligatures(f, out, n);
    float x = 0;
    int kept = 0;
    for (int i = 0; i < n; i++) {
        Glyph g = out[i];
        if (invisible(g.c) && (i == 0 || out[i - 1].slot == g.slot))
            continue; /* a joiner or selector that formed no ligature */
        Face *face = font_face(f, g.slot);
        if (face && kept && out[kept - 1].slot == g.slot && out[kept - 1].glyph && g.glyph && FT_HAS_KERNING(face->ft)) {
            FT_Vector k;
            if (!FT_Get_Kerning(face->ft, out[kept - 1].glyph, g.glyph, FT_KERNING_UNSCALED, &k))
                x += (float)k.x * f->size / (float)face->ft->units_per_EM;
        }
        g.x = x;
        out[kept++] = g;
        x += advance(f, g.slot, g.glyph);
    }
    if (width)
        *width = x;
    return kept;
}
static float text_width(CuFont *f, const char *text, size_t length) {
    Glyph stack[512];
    Glyph *g = length <= 512 ? stack : ch_alloc(sizeof(Glyph) * length);
    float w = 0;
    shape(f, text, length, g, (int)(length <= 512 ? 512 : length), &w);
    if (g != stack)
        free(g);
    return w;
}
static void set_color(cairo_t *cr, CuColor c) {
    cairo_set_source_rgba(cr, ((c >> 16) & 255) / 255.0, ((c >> 8) & 255) / 255.0, (c & 255) / 255.0,
                          (c >> 24) / 255.0);
}
static void draw_text(CuCanvas *c, CuFont *f, float x, float baseline, const char *text, size_t length, CuColor color) {
    Glyph stack[512];
    Glyph *g = length <= 512 ? stack : ch_alloc(sizeof(Glyph) * length);
    int n = shape(f, text, length, g, (int)(length <= 512 ? 512 : length), NULL);
    cairo_t *cr = c->cr;
    set_color(cr, color);
    cairo_glyph_t run[512];
    for (int i = 0; i < n;) {
        int slot = g[i].slot, k = 0;
        Face *face = font_face(f, slot);
        while (i < n && g[i].slot == slot && k < 512) {
            run[k++] = (cairo_glyph_t){g[i].glyph, x + g[i].x, baseline};
            i++;
        }
        if (!face)
            continue;
        cairo_set_font_face(cr, face->face);
        cairo_set_font_size(cr, f->size);
        cairo_set_font_options(cr, options);
        cairo_show_glyphs(cr, run, k);
    }
    if (g != stack)
        free(g);
}

/* Canvas */
static void rounded(cairo_t *cr, CuRect r, float radius) {
    radius = fminf(radius, fminf(r.w, r.h) / 2);
    if (radius <= 0.01f) {
        cairo_rectangle(cr, r.x, r.y, r.w, r.h);
        return;
    }
    cairo_new_sub_path(cr);
    cairo_arc(cr, r.x + r.w - radius, r.y + radius, radius, -M_PI / 2, 0);
    cairo_arc(cr, r.x + r.w - radius, r.y + r.h - radius, radius, 0, M_PI / 2);
    cairo_arc(cr, r.x + radius, r.y + r.h - radius, radius, M_PI / 2, M_PI);
    cairo_arc(cr, r.x + radius, r.y + radius, radius, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}
static void cc_save(CuCanvas *c) {
    cairo_save(c->cr);
}
static void cc_restore(CuCanvas *c) {
    cairo_restore(c->cr);
}
static void cc_clip(CuCanvas *c, CuRect r) {
    cairo_rectangle(c->cr, r.x, r.y, r.w, r.h);
    cairo_clip(c->cr);
}
static void cc_translate(CuCanvas *c, float x, float y) {
    cairo_translate(c->cr, x, y);
}
static void cc_scale(CuCanvas *c, float x, float y) {
    cairo_scale(c->cr, x, y);
}
static void cc_fill_rect(CuCanvas *c, CuRect r, float radius, CuColor color) {
    if (!(color >> 24) || r.w <= 0 || r.h <= 0)
        return;
    rounded(c->cr, r, radius);
    set_color(c->cr, color);
    cairo_fill(c->cr);
}
static void cc_stroke_rect(CuCanvas *c, CuRect r, float radius, float width, CuColor color) {
    if (!(color >> 24))
        return;
    rounded(c->cr, r, radius);
    set_color(c->cr, color);
    cairo_set_line_width(c->cr, width);
    cairo_stroke(c->cr);
}
static void cc_move_to(CuCanvas *c, float x, float y) {
    cairo_move_to(c->cr, x, y);
}
static void cc_line_to(CuCanvas *c, float x, float y) {
    cairo_line_to(c->cr, x, y);
}
static void cc_curve_to(CuCanvas *c, float x1, float y1, float x2, float y2, float x3, float y3) {
    cairo_curve_to(c->cr, x1, y1, x2, y2, x3, y3);
}
static void cc_close_path(CuCanvas *c) {
    cairo_close_path(c->cr);
}
static void cc_fill_path(CuCanvas *c, CuColor color) {
    set_color(c->cr, color);
    cairo_fill(c->cr);
}
static void cc_stroke_path(CuCanvas *c, float width, CuColor color) {
    set_color(c->cr, color);
    cairo_set_line_width(c->cr, width);
    cairo_set_line_cap(c->cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(c->cr, CAIRO_LINE_JOIN_ROUND);
    cairo_stroke(c->cr);
}
struct CuImage {
    cairo_surface_t *surface;
    uint32_t *pixels;
    int width, height;
};
static CuImage *cc_image_new(const uint32_t *pixels, int width, int height) {
    CuImage *image = ch_new0(CuImage, 1);
    size_t bytes = (size_t)width * (size_t)height * 4;
    image->pixels = ch_alloc(bytes);
    memcpy(image->pixels, pixels, bytes);
    image->width = width;
    image->height = height;
    image->surface = cairo_image_surface_create_for_data((unsigned char *)image->pixels, CAIRO_FORMAT_ARGB32, width,
                                                         height, width * 4);
    return image;
}
static void cc_image_free(CuImage *image) {
    if (image) {
        cairo_surface_destroy(image->surface);
        free(image->pixels);
        free(image);
    }
}
static void cc_draw_image(CuCanvas *c, CuImage *image, CuRect r, float radius) {
    cairo_save(c->cr);
    rounded(c->cr, r, radius);
    cairo_clip(c->cr);
    cairo_translate(c->cr, r.x, r.y);
    cairo_scale(c->cr, (double)r.w / image->width, (double)r.h / image->height);
    cairo_set_source_surface(c->cr, image->surface, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(c->cr), CAIRO_FILTER_GOOD);
    cairo_paint(c->cr);
    cairo_restore(c->cr);
}
void cc_install(CuPlatform *p) {
    p->font_get = font_get;
    p->font_metrics = font_metrics;
    p->text_width = text_width;
    p->save = cc_save;
    p->restore = cc_restore;
    p->clip = cc_clip;
    p->translate = cc_translate;
    p->scale = cc_scale;
    p->fill_rect = cc_fill_rect;
    p->stroke_rect = cc_stroke_rect;
    p->move_to = cc_move_to;
    p->line_to = cc_line_to;
    p->curve_to = cc_curve_to;
    p->close_path = cc_close_path;
    p->fill_path = cc_fill_path;
    p->stroke_path = cc_stroke_path;
    p->draw_text = draw_text;
    p->image_new = cc_image_new;
    p->image_free = cc_image_free;
    p->draw_image = cc_draw_image;
}
void cc_shutdown(void) {
    for (int i = 0; i < font_count; i++) {
        FcFontSetDestroy(fonts[i]->set);
        free(fonts[i]->faces);
        free(fonts[i]->coverage);
        free(fonts[i]->color);
        free(fonts[i]->advances);
        free(fonts[i]->family);
        free(fonts[i]);
    }
    free(fonts);
    fonts = NULL;
    font_count = 0;
    for (int i = 0; i < face_count; i++) {
        cairo_scaled_font_destroy(faces[i]->measure);
        cairo_font_face_destroy(faces[i]->face);
        free(faces[i]->gsub);
        free(faces[i]->ligatures);
        free(faces[i]->file);
        free(faces[i]);
    }
    free(faces);
    faces = NULL;
    face_count = 0;
}
