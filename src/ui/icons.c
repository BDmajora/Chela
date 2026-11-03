/* Outline icons on a 20×20 grid, stroked at 1.5 units with round caps and joins.
 * A small SVG path interpreter (M L H V C S Q A Z, absolute and relative) turns
 * them into canvas paths, so every backend draws identical icons. */
#include "cu_internal.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name, *svg;
} IconDef;
static const IconDef icons[] = {
    {"app-menu", "<path d='M3 4h14M3 10h10M3 16h7'/>"},
    {"go-previous", "<path d='m12 4-6 6 6 6'/>"},
    {"go-next", "<path d='m8 4 6 6-6 6'/>"},
    {"clock", "<circle cx='10' cy='10' r='8'/><path d='M10 5v5l4 2'/>"},
    {"artifacts", "<rect x='2' y='6' width='10' height='12' rx='2'/><path d='m7 6 1-4 10 2-2 12-4-1'/>"},
    {"projects", "<path d='M4 2h12M2 6h16l-2 12H4z'/>"},
    {"window-close", "<path d='m5 5 10 10M5 15 15 5'/>"},
    {"window-minimize", "<path d='M5 10h10'/>"},
    {"window-maximize", "<rect x='5' y='5' width='10' height='10' rx='1'/>"},
    {"window-restore", "<rect x='4' y='7' width='9' height='9' rx='1'/><path d='M7 7V4h9v9h-3'/>"},
    {"sun", "<circle cx='10' cy='10' r='4'/><path d='M10 1v2m0 14v2M1 10h2m14 0h2M3 3l2 2m10 10 2 2M3 17l2-2M15 5l2-2'/>"},
    {"moon", "<path d='M17 12a7 7 0 0 1-9-9 8 8 0 1 0 9 9z'/>"},
    {"pin", "<path d='m7 2 9 9-4 1-3 3-4-4 3-3zM7 13l-5 5'/>"},
    {"share", "<path d='M10 13V2m-4 4 4-4 4 4M4 10H2v8h16v-8h-2'/>"},
    {"speaker", "<path d='M2 7h4l4-4v14l-4-4H2zm11-1a6 6 0 0 1 0 8m3-11a10 10 0 0 1 0 14'/>"},
    {"thumb-up", "<path d='M7 9 10 2h2v6h5l-2 9H7zM2 9h3v8H2z'/>"},
    {"thumb-down", "<path d='m7 11 3 7h2v-6h5l-2-9H7zM2 3h3v8H2z'/>"},
    {"regenerate", "<path d='M17 8a7 7 0 1 0 0 6M17 3v5h-5'/>"},
    {"edit-find", "<circle cx='8.5' cy='8.5' r='5.5'/><path d='m13 13 4.5 4.5'/>"},
    {"sidebar-show", "<rect x='2.5' y='3.5' width='15' height='13' rx='2'/><path d='M8 3.5v13'/>"},
    {"list-add", "<path d='M10 3v14M3 10h14'/>"},
    {"view-more", "<path d='M4 10h.1m5.9 0h.1m5.9 0h.1' stroke-width='2.5'/>"},
    {"user-available", "<path d='M7 3.5h7a3 3 0 0 1 3 3v5a3 3 0 0 1-3 3H9l-4 3v-3a3 3 0 0 1-3-3v-5a3 3 0 0 1 3-3z'/>"},
    {"utilities-terminal", "<path d='m6.5 6-4 4 4 4m7-8 4 4-4 4M11.5 4l-3 12'/>"},
    {"folder", "<path d='M2.5 6V4.5h5l2 2h8v9a1 1 0 0 1-1 1h-13a1 1 0 0 1-1-1zM2.5 8h15'/>"},
    {"preferences-other", "<rect x='3' y='6.5' width='14' height='11' rx='1.5'/><path d='M7 6.5v-3h6v3M3 11h14M9 10v3h2v-3'/>"},
    {"view-sort-descending", "<path d='M5 3v14m-3-3 3 3 3-3M11 5h7m-7 5h5m-5 5h3'/>"},
    {"sliders", "<path d='M5 2v5m0 4v7M15 2v9m0 4v3'/><circle cx='5' cy='9' r='2'/><circle cx='15' cy='13' r='2'/>"},
    {"microphone", "<rect x='7' y='2' width='6' height='10' rx='3'/><path d='M4 8v1a6 6 0 0 0 12 0V8m-6 7v3m-3 0h6'/>"},
    {"waveform", "<path d='M2 8v4M6 4v12M10 2v16M14 5v10M18 8v4'/>"},
    {"thinking", "<path d='M7 14c0-3-3-3-3-7a6 6 0 0 1 12 0c0 4-3 4-3 7M7 15h6M8 18h4M10 4v3m-2 3 2-3 2 3'/>"},
    {"chevron-down", "<path d='m6 8 4 4 4-4'/>"},
    {"chevron-right", "<path d='m8 6 4 4-4 4'/>"},
    {"session-idle", "<circle cx='10' cy='10' r='4' stroke-dasharray='1.2 2.2'/>"},
    {"camera", "<path d='M2 6h4l1.5-3h5L14 6h4v11H2z'/><circle cx='10' cy='11' r='3'/>"},
    {"skills", "<path d='M4 3h12v11H8v4H3V7H1V3h3zm4 11v4h9v-4M4 3v4M7 7h1m2 0h1m2 0h1'/>"},
    {"connectors", "<rect x='2' y='2' width='6' height='6' rx='1'/><rect x='12' y='2' width='6' height='6' rx='1'/><rect x='2' y='12' width='6' height='6' rx='1'/><path d='M15 12v6m-3-3h6'/>"},
    {"system-users", "<circle cx='7' cy='6' r='3'/><path d='M1 17v-2a6 6 0 0 1 12 0v2zm12-8a3 3 0 0 0 0-6m2 9a5 5 0 0 1 4 5'/>"},
    {"plugins", "<path d='m6 2 3 3m5-1 3 3M8 4l8 8-3 3a5 5 0 0 1-7-7zM5 15l-3 3'/>"},
    {"palette", "<path d='M10 2a8 8 0 1 0 0 16h1a2 2 0 0 0 1-3.7 1.5 1.5 0 0 1 1-2.8h2a3 3 0 0 0 3-3C18 5 14 2 10 2z'/><path d='M6 7h.1M10 5h.1M14 7h.1M5 11h.1' stroke-width='2'/>"},
    {"globe", "<circle cx='10' cy='10' r='8'/><ellipse cx='10' cy='10' rx='3.5' ry='8'/><path d='M2 10h16'/>"},
    {"research", "<circle cx='8.5' cy='8.5' r='6'/><path d='m13 13 4.5 4.5M5 9h2l1.5-3 2 5 1-2h2'/>"},
    {"preferences-system", "<path d='m8 2-1 3-3 1 1 3-1 3 3 1 1 3h4l1-3 3-1-1-3 1-3-3-1-1-3z'/><circle cx='10' cy='9' r='2.5'/>"},
    {"media-playback-stop", "<rect x='5' y='5' width='10' height='10' rx='1'/>"},
    {"media-playback-start", "<path d='M6 4v12l10-6z'/>"},
    {"view-list", "<path d='M7 5h10M7 10h10M7 15h10M3 5h.1M3 10h.1M3 15h.1'/>"},
    {"computer", "<rect x='2.5' y='3' width='15' height='11' rx='1.5'/><path d='M10 14v3m-4 0h8'/>"},
    {"network-server", "<rect x='3' y='3' width='14' height='6' rx='1.5'/><rect x='3' y='11' width='14' height='6' rx='1.5'/><path d='M6 6h.1M6 14h.1' stroke-width='2'/>"},
    {"document-open", "<path d='M11 5.5 5.5 11a3 3 0 0 0 4.2 4.2l6.5-6.5a4.5 4.5 0 0 0-6.4-6.4L3.5 8.7m4 1.5 5-5'/>"},
    {"document-edit", "<path d='m4 12 9-9 4 4-9 9-5 1zM11 5l4 4M3 18h14'/>"},
    {"document-save", "<path d='M10 2.5v11m-4-4 4 4 4-4M3 13v4.5h14V13'/>"},
    {"edit-copy", "<rect x='7' y='7' width='10' height='10' rx='1.5'/><path d='M12 7V3H3v9h4'/>"},
    {"go-up", "<path d='M10 16V4m-5 5 5-5 5 5'/>"},
    {"send", "<path d='M10 16V4m-5 5 5-5 5 5'/>"},
    {"user-trash", "<path d='M3 5h14M7 5V2.5h6V5M5 5l1 12h8l1-12M8 8v6m4-6v6'/>"},
    {"input-keyboard", "<rect x='1.5' y='5' width='17' height='10' rx='1.5'/><path d='M5 8h.1M8 8h.1M11 8h.1M14 8h.1M6 12h8' stroke-width='1.8'/>"},
    {"help-about", "<circle cx='10' cy='10' r='8'/><path d='M10 9v5M10 6h.1' stroke-width='1.8'/>"},
    {"application-exit", "<path d='M8 3H4v14h4M13 6l4 4-4 4M17 10H8'/>"},
    {"check", "<path d='m4 10 4 4 8-8'/>"},
    {"warning", "<path d='M10 3 2 17h16zM10 8v4M10 14.5h.1'/>"},
    {"external", "<path d='M11 3h6v6M17 3l-8 8M8 4H3v13h13v-5'/>"},
};

/* Parsed geometry: a list of path commands with coordinates in the 20×20 grid. */
enum { OP_MOVE, OP_LINE, OP_CURVE, OP_CLOSE, OP_STROKE };
typedef struct {
    int op;
    float v[6];
} Op;
typedef struct {
    const char *name;
    Op *ops;
    int count;
} Parsed;
static Parsed *parsed;
static int parsed_count;

typedef struct {
    Op *ops;
    int count, cap;
} OpList;
static void emit(OpList *l, int op, float a, float b, float c, float d, float e, float f) {
    if (l->count == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 64;
        l->ops = ch_realloc(l->ops, sizeof(Op) * (size_t)l->cap);
    }
    l->ops[l->count++] = (Op){op, {a, b, c, d, e, f}};
}
static const char *attr(const char *tag, const char *name, char *out, size_t size) {
    char key[32];
    snprintf(key, sizeof(key), " %s='", name);
    const char *p = strstr(tag, key);
    const char *end = strchr(tag, '>');
    if (!p || (end && p > end))
        return NULL;
    p += strlen(key);
    const char *q = strchr(p, '\'');
    size_t n = q ? (size_t)(q - p) : 0;
    if (n >= size)
        n = size - 1;
    memcpy(out, p, n);
    out[n] = 0;
    return out;
}
static float num(const char *tag, const char *name) {
    char v[64];
    return attr(tag, name, v, sizeof(v)) ? strtof(v, NULL) : 0;
}
static bool number(const char **p, float *out) {
    const char *s = *p;
    while (*s == ' ' || *s == ',' || *s == '\n')
        s++;
    if (!(isdigit((unsigned char)*s) || *s == '-' || *s == '+' || *s == '.'))
        return false;
    const char *start = s;
    if (*s == '-' || *s == '+')
        s++;
    bool dot = false;
    while (isdigit((unsigned char)*s) || (*s == '.' && !dot)) {
        if (*s == '.')
            dot = true;
        s++;
    }
    if (*s == 'e' || *s == 'E') {
        s++;
        if (*s == '-' || *s == '+')
            s++;
        while (isdigit((unsigned char)*s))
            s++;
    }
    char buf[32];
    size_t n = CH_MIN((size_t)(s - start), sizeof(buf) - 1);
    memcpy(buf, start, n);
    buf[n] = 0;
    *out = strtof(buf, NULL);
    *p = s;
    return true;
}
static bool flag(const char **p, float *out) {
    const char *s = *p;
    while (*s == ' ' || *s == ',')
        s++;
    if (*s != '0' && *s != '1')
        return false;
    *out = (float)(*s - '0');
    *p = s + 1;
    return true;
}
/* SVG elliptical arc (endpoint form) as cubic Bézier segments. */
static void arc(OpList *l, float x1, float y1, float rx, float ry, float phi_deg, int large, int sweep, float x2, float y2) {
    if (rx == 0 || ry == 0) {
        emit(l, OP_LINE, x2, y2, 0, 0, 0, 0);
        return;
    }
    double phi = phi_deg * M_PI / 180, cp = cos(phi), sp = sin(phi);
    double dx = (x1 - x2) / 2.0, dy = (y1 - y2) / 2.0;
    double xp = cp * dx + sp * dy, yp = -sp * dx + cp * dy;
    double arx = fabs(rx), ary = fabs(ry);
    double lambda = xp * xp / (arx * arx) + yp * yp / (ary * ary);
    if (lambda > 1) {
        arx *= sqrt(lambda);
        ary *= sqrt(lambda);
    }
    double num_ = arx * arx * ary * ary - arx * arx * yp * yp - ary * ary * xp * xp;
    double den = arx * arx * yp * yp + ary * ary * xp * xp;
    double coef = den > 0 ? sqrt(fmax(0, num_ / den)) : 0;
    if (large == sweep)
        coef = -coef;
    double cxp = coef * arx * yp / ary, cyp = -coef * ary * xp / arx;
    double cx = cp * cxp - sp * cyp + (x1 + x2) / 2.0, cy = sp * cxp + cp * cyp + (y1 + y2) / 2.0;
    double ux = (xp - cxp) / arx, uy = (yp - cyp) / ary, vx = (-xp - cxp) / arx, vy = (-yp - cyp) / ary;
    double start = atan2(uy, ux);
    double delta = atan2(ux * vy - uy * vx, ux * vx + uy * vy);
    if (!sweep && delta > 0)
        delta -= 2 * M_PI;
    else if (sweep && delta < 0)
        delta += 2 * M_PI;
    int segments = (int)ceil(fabs(delta) / (M_PI / 2) - 1e-6);
    if (segments < 1)
        segments = 1;
    double step = delta / segments, t = 4.0 / 3.0 * tan(step / 4);
    for (int i = 0; i < segments; i++) {
        double a1 = start + step * i, a2 = a1 + step;
        double c1 = cos(a1), s1 = sin(a1), c2 = cos(a2), s2 = sin(a2);
        double p1x = c1 - t * s1, p1y = s1 + t * c1, p2x = c2 + t * s2, p2y = s2 - t * c2;
#define TX(px, py) (float)(cx + arx * (px) * cp - ary * (py) * sp)
#define TY(px, py) (float)(cy + arx * (px) * sp + ary * (py) * cp)
        emit(l, OP_CURVE, TX(p1x, p1y), TY(p1x, p1y), TX(p2x, p2y), TY(p2x, p2y), TX(c2, s2), TY(c2, s2));
#undef TX
#undef TY
    }
}
static void parse_path(OpList *l, const char *d) {
    float x = 0, y = 0, sx = 0, sy = 0, cx2 = 0, cy2 = 0;
    char cmd = 0, prev = 0;
    const char *p = d;
    while (*p) {
        while (*p == ' ' || *p == ',')
            p++;
        if (!*p)
            break;
        if (isalpha((unsigned char)*p))
            cmd = *p++;
        else if (!cmd)
            break;
        bool rel = islower((unsigned char)cmd);
        float v[7];
        switch (tolower((unsigned char)cmd)) {
        case 'm':
            if (!number(&p, &v[0]) || !number(&p, &v[1]))
                return;
            x = rel ? x + v[0] : v[0];
            y = rel ? y + v[1] : v[1];
            sx = x, sy = y;
            emit(l, OP_MOVE, x, y, 0, 0, 0, 0);
            cmd = rel ? 'l' : 'L'; /* further pairs are line-to */
            break;
        case 'l':
            if (!number(&p, &v[0]) || !number(&p, &v[1]))
                return;
            x = rel ? x + v[0] : v[0];
            y = rel ? y + v[1] : v[1];
            emit(l, OP_LINE, x, y, 0, 0, 0, 0);
            break;
        case 'h':
            if (!number(&p, &v[0]))
                return;
            x = rel ? x + v[0] : v[0];
            emit(l, OP_LINE, x, y, 0, 0, 0, 0);
            break;
        case 'v':
            if (!number(&p, &v[0]))
                return;
            y = rel ? y + v[0] : v[0];
            emit(l, OP_LINE, x, y, 0, 0, 0, 0);
            break;
        case 'c':
            for (int i = 0; i < 6; i++)
                if (!number(&p, &v[i]))
                    return;
            if (rel)
                for (int i = 0; i < 6; i += 2)
                    v[i] += x, v[i + 1] += y;
            emit(l, OP_CURVE, v[0], v[1], v[2], v[3], v[4], v[5]);
            cx2 = v[2], cy2 = v[3], x = v[4], y = v[5];
            break;
        case 's':
            for (int i = 0; i < 4; i++)
                if (!number(&p, &v[i]))
                    return;
            if (rel)
                for (int i = 0; i < 4; i += 2)
                    v[i] += x, v[i + 1] += y;
            {
                bool smooth = strchr("CcSs", prev) != NULL;
                float c1x = smooth ? 2 * x - cx2 : x, c1y = smooth ? 2 * y - cy2 : y;
                emit(l, OP_CURVE, c1x, c1y, v[0], v[1], v[2], v[3]);
            }
            cx2 = v[0], cy2 = v[1], x = v[2], y = v[3];
            break;
        case 'q':
            for (int i = 0; i < 4; i++)
                if (!number(&p, &v[i]))
                    return;
            if (rel)
                for (int i = 0; i < 4; i += 2)
                    v[i] += x, v[i + 1] += y;
            emit(l, OP_CURVE, x + 2.f / 3 * (v[0] - x), y + 2.f / 3 * (v[1] - y), v[2] + 2.f / 3 * (v[0] - v[2]),
                 v[3] + 2.f / 3 * (v[1] - v[3]), v[2], v[3]);
            x = v[2], y = v[3];
            break;
        case 'a': {
            if (!number(&p, &v[0]) || !number(&p, &v[1]) || !number(&p, &v[2]) || !flag(&p, &v[3]) ||
                !flag(&p, &v[4]) || !number(&p, &v[5]) || !number(&p, &v[6]))
                return;
            float ex = rel ? x + v[5] : v[5], ey = rel ? y + v[6] : v[6];
            arc(l, x, y, v[0], v[1], v[2], (int)v[3], (int)v[4], ex, ey);
            x = ex, y = ey;
            break;
        }
        case 'z':
            emit(l, OP_CLOSE, 0, 0, 0, 0, 0, 0);
            x = sx, y = sy;
            break;
        default:
            return;
        }
        prev = cmd;
    }
}
static void ellipse(OpList *l, float cx, float cy, float rx, float ry, float dash, float gap) {
    if (dash > 0) {
        /* Dashed outline as short arcs around the circle. */
        float circumference = 2 * (float)M_PI * rx;
        int n = (int)(circumference / (dash + gap));
        for (int i = 0; i < n; i++) {
            float a0 = (float)i / (float)n * 2 * (float)M_PI, a1 = a0 + dash / rx;
            emit(l, OP_MOVE, cx + rx * cosf(a0), cy + ry * sinf(a0), 0, 0, 0, 0);
            emit(l, OP_LINE, cx + rx * cosf(a1), cy + ry * sinf(a1), 0, 0, 0, 0);
        }
        return;
    }
    emit(l, OP_MOVE, cx + rx, cy, 0, 0, 0, 0);
    arc(l, cx + rx, cy, rx, ry, 0, 0, 1, cx - rx, cy);
    arc(l, cx - rx, cy, rx, ry, 0, 0, 1, cx + rx, cy);
    emit(l, OP_CLOSE, 0, 0, 0, 0, 0, 0);
}
static void rect(OpList *l, float x, float y, float w, float h, float r) {
    if (r <= 0) {
        emit(l, OP_MOVE, x, y, 0, 0, 0, 0);
        emit(l, OP_LINE, x + w, y, 0, 0, 0, 0);
        emit(l, OP_LINE, x + w, y + h, 0, 0, 0, 0);
        emit(l, OP_LINE, x, y + h, 0, 0, 0, 0);
        emit(l, OP_CLOSE, 0, 0, 0, 0, 0, 0);
        return;
    }
    emit(l, OP_MOVE, x + r, y, 0, 0, 0, 0);
    emit(l, OP_LINE, x + w - r, y, 0, 0, 0, 0);
    arc(l, x + w - r, y, r, r, 0, 0, 1, x + w, y + r);
    emit(l, OP_LINE, x + w, y + h - r, 0, 0, 0, 0);
    arc(l, x + w, y + h - r, r, r, 0, 0, 1, x + w - r, y + h);
    emit(l, OP_LINE, x + r, y + h, 0, 0, 0, 0);
    arc(l, x + r, y + h, r, r, 0, 0, 1, x, y + h - r);
    emit(l, OP_LINE, x, y + r, 0, 0, 0, 0);
    arc(l, x, y + r, r, r, 0, 0, 1, x + r, y);
    emit(l, OP_CLOSE, 0, 0, 0, 0, 0, 0);
}
static Parsed *icon_parse(const char *name) {
    for (int i = 0; i < parsed_count; i++)
        if (!strcmp(parsed[i].name, name))
            return &parsed[i];
    const char *svg = NULL;
    for (size_t i = 0; i < CH_N_ELEMENTS(icons); i++)
        if (!strcmp(icons[i].name, name))
            svg = icons[i].svg;
    if (!svg)
        return NULL;
    OpList l = {0};
    for (const char *tag = strchr(svg, '<'); tag; tag = strchr(tag + 1, '<')) {
        char buf[512];
        float stroke = attr(tag, "stroke-width", buf, sizeof(buf)) ? strtof(buf, NULL) : 1.5f;
        if (!strncmp(tag, "<path", 5) && attr(tag, "d", buf, sizeof(buf)))
            parse_path(&l, buf);
        else if (!strncmp(tag, "<circle", 7)) {
            float dash = 0, gap = 0;
            if (attr(tag, "stroke-dasharray", buf, sizeof(buf)))
                sscanf(buf, "%f %f", &dash, &gap);
            ellipse(&l, num(tag, "cx"), num(tag, "cy"), num(tag, "r"), num(tag, "r"), dash, gap);
        } else if (!strncmp(tag, "<ellipse", 8))
            ellipse(&l, num(tag, "cx"), num(tag, "cy"), num(tag, "rx"), num(tag, "ry"), 0, 0);
        else if (!strncmp(tag, "<rect", 5))
            rect(&l, num(tag, "x"), num(tag, "y"), num(tag, "width"), num(tag, "height"), num(tag, "rx"));
        else
            continue;
        emit(&l, OP_STROKE, stroke, 0, 0, 0, 0, 0);
    }
    parsed = ch_realloc(parsed, sizeof(Parsed) * (size_t)(parsed_count + 1));
    parsed[parsed_count] = (Parsed){name, l.ops, l.count};
    return &parsed[parsed_count++];
}
void icon_paint(Cu *cu, CuCanvas *c, const char *name, float x, float y, float size, CuColor color) {
    Parsed *icon = icon_parse(name);
    const CuPlatform *p = cu->p;
    if (!icon) {
        p->fill_rect(c, (CuRect){x + size * .3f, y + size * .3f, size * .4f, size * .4f}, size * .2f, color);
        return;
    }
    p->save(c);
    p->translate(c, x, y);
    p->scale(c, size / 20, size / 20);
    for (int i = 0; i < icon->count; i++) {
        Op *o = &icon->ops[i];
        switch (o->op) {
        case OP_MOVE:
            p->move_to(c, o->v[0], o->v[1]);
            break;
        case OP_LINE:
            p->line_to(c, o->v[0], o->v[1]);
            break;
        case OP_CURVE:
            p->curve_to(c, o->v[0], o->v[1], o->v[2], o->v[3], o->v[4], o->v[5]);
            break;
        case OP_CLOSE:
            p->close_path(c);
            break;
        case OP_STROKE:
            p->stroke_path(c, o->v[0], color);
            break;
        }
    }
    p->restore(c);
}
void cu_draw_icon(Cu *cu, CuCanvas *c, const char *name, float x, float y, float size, CuColor color) {
    icon_paint(cu, c, name, x, y, size, cu_apply_alpha(cu, color));
}
