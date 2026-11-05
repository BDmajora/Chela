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
    /* Window and navigation */
    {"app-menu", "<path d='M3.5 5h13M3.5 10h10M3.5 15h7'/>"},
    {"sidebar-show", "<rect x='2.5' y='4' width='15' height='12' rx='2.5'/><path d='M6 8v4'/>"},
    {"panel-right", "<rect x='2.5' y='3.5' width='15' height='13' rx='2.5'/><path d='M5.5 7h5M5.5 10h5M5.5 13h3M13 3.5v13'/>"},
    {"go-previous", "<path d='m12 5-5 5 5 5'/>"},
    {"go-next", "<path d='m8 5 5 5-5 5'/>"},
    {"go-up", "<path d='M10 16V4m-5 5 5-5 5 5'/>"},
    {"chevron-down", "<path d='m6 8 4 4 4-4'/>"},
    {"chevron-right", "<path d='m8 6 4 4-4 4'/>"},
    {"chevron-up", "<path d='m6 12 4-4 4 4'/>"},
    {"window-close", "<path d='m5 5 10 10M5 15 15 5'/>"},
    {"window-minimize", "<path d='M5 10h10'/>"},
    {"window-maximize", "<rect x='5' y='5' width='10' height='10' rx='1'/>"},
    {"window-restore", "<rect x='4' y='7' width='9' height='9' rx='1'/><path d='M7 7V4h9v9h-3'/>"},
    {"window-diamond", "<path d='m10 4.5 5.5 5.5-5.5 5.5L4.5 10z'/>"},
    {"list-add", "<path d='M10 3.5v13M3.5 10h13'/>"},
    {"edit-find", "<circle cx='8.5' cy='8.5' r='5.5'/><path d='m12.7 12.7 4.3 4.3'/>"},
    {"view-more", "<path d='M4 10h.1m5.9 0h.1m5.9 0h.1' stroke-width='2.4'/>"},
    {"view-list", "<path d='M7 5h10M7 10h10M7 15h10M3 5h.1M3 10h.1M3 15h.1'/>"},
    {"session-idle", "<circle cx='10' cy='10' r='4' stroke-dasharray='1.2 2.2'/>"},
    /* Sections */
    {"user-available", "<path d='M6 3.5h8a3.5 3.5 0 0 1 3.5 3.5v4a3.5 3.5 0 0 1-3.5 3.5H9.5L6 17.5v-3a3.5 3.5 0 0 1-3.5-3.5V7A3.5 3.5 0 0 1 6 3.5z'/>"},
    {"utilities-terminal", "<path d='m6.5 6-4 4 4 4m7-8 4 4-4 4M11.5 4l-3 12'/>"},
    {"projects", "<path d='M4.5 3h11M2.5 7h15l-1.4 9a1.5 1.5 0 0 1-1.5 1.3H5.4A1.5 1.5 0 0 1 3.9 16z'/>"},
    {"preferences-other", "<rect x='2.5' y='6.5' width='15' height='10.5' rx='2'/><path d='M7 6.5V5a2 2 0 0 1 2-2h2a2 2 0 0 1 2 2v1.5'/>"},
    {"preferences-system", "<circle cx='10' cy='10' r='2.6'/><path d='M8.6 2.2h2.8l.4 2.1 1.5.9 2-.8 1.4 2.4-1.6 1.4v1.7l1.6 1.4-1.4 2.4-2-.8-1.5.9-.4 2.1H8.6l-.4-2.1-1.5-.9-2 .8-1.4-2.4 1.6-1.4V8.6L3.3 7.2l1.4-2.4 2 .8 1.5-.9z'/>"},
    {"pin", "<path d='m7 2.5 10.5 10.5-3.7.9-3.3 3.3-5.7-5.7 3.3-3.3zM6.8 13.2 2.5 17.5'/>"},
    {"archive", "<path d='M3 4.5h14v3H3zM4.5 7.5V16h11V7.5M8 11h4'/>"},
    {"share", "<path d='M10 12.5V2.5m-3.5 3.5L10 2.5 13.5 6M6 9H4a1.5 1.5 0 0 0-1.5 1.5v5A1.5 1.5 0 0 0 4 17h12a1.5 1.5 0 0 0 1.5-1.5v-5A1.5 1.5 0 0 0 16 9h-2'/>"},
    /* Messages */
    {"thumb-up", "<path d='M6.5 9 9.8 2.6c1.2 0 2.2 1 2.2 2.2V8h4.1c1.2 0 2 1.1 1.8 2.2l-1.2 5.9a1.8 1.8 0 0 1-1.8 1.4H6.5zM6.5 9h-3a1 1 0 0 0-1 1v6.5a1 1 0 0 0 1 1h3'/>"},
    {"thumb-down", "<path d='M13.5 11l-3.3 6.4c-1.2 0-2.2-1-2.2-2.2V12H3.9c-1.2 0-2-1.1-1.8-2.2l1.2-5.9a1.8 1.8 0 0 1 1.8-1.4h8.4zM13.5 11h3a1 1 0 0 0 1-1V3.5a1 1 0 0 0-1-1h-3'/>"},
    {"regenerate", "<path d='M17 10a7 7 0 1 1-2.1-5L17 7M17 3v4h-4'/>"},
    {"edit-copy", "<rect x='7' y='7' width='10.5' height='10.5' rx='2'/><path d='M13 7V4.5a2 2 0 0 0-2-2H4.5a2 2 0 0 0-2 2V11a2 2 0 0 0 2 2H7'/>"},
    {"check", "<path d='m4 10.5 4 4 8-9'/>"},
    {"warning", "<path d='M10 3 1.8 17h16.4zM10 8.5v4M10 14.8h.1'/>"},
    {"help-about", "<circle cx='10' cy='10' r='7.5'/><path d='M10 9v5M10 6.2h.1' stroke-width='1.8'/>"},
    {"task-open", "<rect x='3.5' y='3.5' width='13' height='13' rx='3'/>"},
    {"task-done", "<rect x='3.5' y='3.5' width='13' height='13' rx='3'/><path d='m6.5 10.3 2.5 2.5 4.5-5.3'/>"},
    /* Composer */
    {"microphone", "<rect x='7' y='2.5' width='6' height='10' rx='3'/><path d='M4 9.5a6 6 0 0 0 12 0M10 15.5v2.5'/>"},
    {"send", "<path d='M16.5 4v5a3 3 0 0 1-3 3H4m4-4-4 4 4 4'/>"},
    {"media-playback-stop", "<rect x='5' y='5' width='10' height='10' rx='2'/>"},
    {"media-playback-start", "<path d='M6.5 4v12l9.5-6z'/>"},
    {"thinking", "<path d='M7 13.5c0-1.6-2.5-2.8-2.5-6a5.5 5.5 0 0 1 11 0c0 3.2-2.5 4.4-2.5 6zM7.5 16h5M8.5 18.5h3'/>"},
    {"image", "<rect x='2.5' y='3.5' width='15' height='13' rx='2'/><circle cx='7' cy='8' r='1.5'/><path d='m17.5 12.5-3.8-3.8L5.5 16.5'/>"},
    {"file", "<path d='M5 2.5h6.5l4 4V17a.5.5 0 0 1-.5.5H5a.5.5 0 0 1-.5-.5V3a.5.5 0 0 1 .5-.5zM11.5 2.5v4h4'/>"},
    {"file-text", "<path d='M5 2.5h6.5l4 4V17a.5.5 0 0 1-.5.5H5a.5.5 0 0 1-.5-.5V3a.5.5 0 0 1 .5-.5zM11.5 2.5v4h4M7.5 10.5h5M7.5 13.5h5'/>"},
    /* Projects and changes */
    {"folder", "<path d='M2.5 5.5A1.5 1.5 0 0 1 4 4h3.6l2 2H16a1.5 1.5 0 0 1 1.5 1.5v7A1.5 1.5 0 0 1 16 16H4a1.5 1.5 0 0 1-1.5-1.5z'/>"},
    {"folder-open", "<path d='M2.5 14.5v-9A1.5 1.5 0 0 1 4 4h3.6l2 2H15a1.5 1.5 0 0 1 1.5 1.5V9M2.5 14.5 4.3 10a1.5 1.5 0 0 1 1.4-1h11.1a1 1 0 0 1 .9 1.4l-1.5 4.7a1.5 1.5 0 0 1-1.4.9H4a1.5 1.5 0 0 1-1.5-1.5z'/>"},
    {"computer", "<rect x='4' y='4.5' width='12' height='8.5' rx='1.2'/><path d='M2 16h16'/>"},
    {"network-server", "<rect x='3' y='3' width='14' height='6' rx='1.5'/><rect x='3' y='11' width='14' height='6' rx='1.5'/><path d='M6 6h.1M6 14h.1' stroke-width='2'/>"},
    {"git-branch", "<circle cx='5.5' cy='4.5' r='2'/><circle cx='5.5' cy='15.5' r='2'/><circle cx='14.5' cy='6.5' r='2'/><path d='M5.5 6.5v7M14.5 8.5c0 3-3 3.2-6.5 3.8-1.4.2-2.5 1-2.5 1.2'/>"},
    {"file-diff", "<rect x='3.5' y='2.5' width='13' height='15' rx='2'/><path d='M10 5.8v5M7.5 8.3h5M7.5 14h5'/>"},
    /* Menus */
    {"skills", "<path d='M10 2.5 12 8l5.5 2-5.5 2-2 5.5L8 12l-5.5-2L8 8z'/>"},
    {"system-users", "<circle cx='7.5' cy='6.5' r='3'/><path d='M2 17v-1a5.5 5.5 0 0 1 11 0v1M13 3.8a3 3 0 0 1 0 5.4M15.5 12.5A5.5 5.5 0 0 1 18 16.5v.5'/>"},
    {"palette", "<path d='M10 2a8 8 0 1 0 0 16h1a2 2 0 0 0 1-3.7 1.5 1.5 0 0 1 1-2.8h2a3 3 0 0 0 3-3C18 5 14 2 10 2z'/><path d='M6 7h.1M10 5h.1M14 7h.1M5 11h.1' stroke-width='2'/>"},
    {"document-open", "<path d='M16.5 9.5 10 16a4 4 0 0 1-5.7-5.7l6.7-6.7a2.7 2.7 0 0 1 3.8 3.8l-6.6 6.6a1.3 1.3 0 0 1-1.9-1.9L12.5 6'/>"},
    {"document-edit", "<path d='m4 12.5 9-9 3.5 3.5-9 9L3 17zM11 5.5 14.5 9'/>"},
    {"document-save", "<path d='M10 2.5v11m-4-4 4 4 4-4M3 14v2a1.5 1.5 0 0 0 1.5 1.5h11A1.5 1.5 0 0 0 17 16v-2'/>"},
    {"user-trash", "<path d='M3 5h14M7.5 5V3.5A1 1 0 0 1 8.5 2.5h3a1 1 0 0 1 1 1V5M5 5l.8 11.1A1.5 1.5 0 0 0 7.3 17.5h5.4a1.5 1.5 0 0 0 1.5-1.4L15 5M8.5 8.5v5.5m3-5.5v5.5'/>"},
    {"input-keyboard", "<rect x='1.5' y='4.5' width='17' height='11' rx='2'/><path d='M5 8h.1M8 8h.1M11 8h.1M14 8h.1M6 12h8' stroke-width='1.8'/>"},
    {"application-exit", "<path d='M8 3H4.5A1.5 1.5 0 0 0 3 4.5v11A1.5 1.5 0 0 0 4.5 17H8M13 6l4 4-4 4M17 10H8'/>"},
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
    const IconDef *def = NULL;
    for (size_t i = 0; i < CH_N_ELEMENTS(icons); i++)
        if (!strcmp(icons[i].name, name))
            def = &icons[i];
    if (!def)
        return NULL;
    const char *svg = def->svg;
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
    /* The table's own name: the caller's lives only until the next frame. */
    parsed[parsed_count] = (Parsed){def->name, l.ops, l.count};
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
