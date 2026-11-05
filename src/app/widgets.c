/* Styled controls built from UI primitives, matching the desktop design. */
#include "app.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static CuId open_dropdown;

const char *app_interface_family(App *a) {
    static const char *families[] = {"Anthropic Sans, Arial, Liberation Sans, Helvetica, sans-serif",
                                     "Noto Sans, sans-serif", "DejaVu Sans, sans-serif"};
    return families[CH_CLAMP(a->interface_font, 0, 2)];
}
const char *app_response_family(App *a) {
    return a->response_font ? app_interface_family(a) : "Anthropic Serif, Georgia, Liberation Serif, serif";
}
CuFont *ui_font(App *a, float size, int weight) {
    return cu_font(a->cu, app_interface_family(a), size, weight);
}
CuFont *serif_font(App *a, float size, int weight) {
    return cu_font(a->cu, app_response_family(a), size, weight);
}
CuFont *mono_font(App *a, float size) {
    CuFontDesc d = {.family = "DejaVu Sans Mono, Liberation Mono, monospace", .size = size, .weight = 400,
                    .monospace = true};
    return cu_font_desc(a->cu, &d);
}
void ui_label(App *a, const char *text, float size, CuColor color) {
    cu_text(a->cu, 0, text, &(CuTextStyle){.font = ui_font(a, size, 400), .color = color});
}
void ui_label_wrap(App *a, const char *text, float size, CuColor color, float max_width) {
    cu_text(a->cu, 0, text,
            &(CuTextStyle){.font = ui_font(a, size, 400), .color = color, .wrap = CU_WRAP_WORD,
                           .w = max_width > 0 ? CU_GROW_MAX(max_width) : CU_GROW});
}
void ui_label_ellipsize(App *a, const char *text, float size, CuColor color, int mode, float max_width) {
    cu_text(a->cu, 0, text,
            &(CuTextStyle){.font = ui_font(a, size, 400), .color = color, .ellipsize = (uint8_t)mode,
                           .w = max_width > 0 ? CU_FIT_MAX(max_width) : CU_FIT});
}
static CuColor hover_bg(App *a, CuId id, bool enabled, CuColor base, CuColor hover) {
    if (!enabled)
        return base;
    return cu_pressed(a->cu, id) ? cu_mix(hover, 0xffffffff, 0.06f) : cu_hovered(a->cu, id) ? hover : base;
}
bool ui_icon_button_sized(App *a, CuId id, const char *icon, float icon_size, float pad, const char *tooltip,
                          bool enabled, bool checked) {
    Cu *cu = a->cu;
    CuColor bg = checked ? 0xff3a3a3a : 0;
    cu_open(cu, id,
            &(CuBox){CU_PAD(pad), .radius = 5, .bg = hover_bg(a, id, enabled, bg, checked ? 0xff424242 : C_BUTTON),
                     .interactive = true, .focusable = enabled, .tooltip = tooltip, .opacity = enabled ? 0 : 0.4f,
                     .border = cu_focus_visible(cu, id) ? 1.5f : 0, .border_color = C_FOCUS});
    cu_icon(cu, icon, icon_size, 0xffd4d4d4);
    cu_close(cu);
    return enabled && cu_clicked(cu, id);
}
bool ui_icon_button(App *a, CuId id, const char *icon, const char *tooltip, bool enabled) {
    return ui_icon_button_sized(a, id, icon, 18, 4, tooltip, enabled, false);
}
bool ui_flat_button(App *a, CuId id, const char *icon, const char *text, const char *trailing, float size,
                    CuColor color, const char *tooltip, bool enabled, float max_text) {
    Cu *cu = a->cu;
    cu_open(cu, id,
            &(CuBox){.row = true, CU_PAD2(3, 6), .gap = 6, .radius = 5, .align_y = CU_CENTER,
                     .bg = hover_bg(a, id, enabled, 0, C_BUTTON), .interactive = true, .focusable = enabled,
                     .tooltip = tooltip, .opacity = enabled ? 0 : 0.45f,
                     .border = cu_focus_visible(cu, id) ? 1.5f : 0, .border_color = C_FOCUS});
    if (icon)
        cu_icon(cu, icon, size + 3, 0xffd4d4d4);
    if (text && *text)
        cu_text(cu, 0, text,
                &(CuTextStyle){.font = ui_font(a, size, 400), .color = color, .ellipsize = CU_ELLIPSIZE_MIDDLE,
                               .w = max_text > 0 ? CU_FIT_MAX(max_text) : CU_FIT});
    if (trailing)
        cu_icon(cu, trailing, size - 1, C_MUTED);
    cu_close(cu);
    return enabled && cu_clicked(cu, id);
}
bool ui_button(App *a, CuId id, const char *text, bool enabled, bool primary) {
    Cu *cu = a->cu;
    CuColor bg = primary ? 0xffe8e8e8 : C_BUTTON, hover = primary ? 0xffffffff : 0xff383838;
    cu_open(cu, id,
            &(CuBox){CU_PAD2(6, 12), .radius = 6, .bg = hover_bg(a, id, enabled, bg, hover),
                     .border = cu_focus_visible(cu, id) ? 2 : 1,
                     .border_color = cu_focus_visible(cu, id) ? C_FOCUS : primary ? 0xffe8e8e8 : C_BUTTON_BORDER,
                     .interactive = true, .focusable = enabled, .opacity = enabled ? 0 : 0.45f, .align_x = CU_CENTER});
    cu_text(cu, 0, text, &(CuTextStyle){.font = ui_font(a, 14, primary ? 600 : 400), .color = primary ? 0xff1f1f1f : C_TEXT});
    cu_close(cu);
    return enabled && cu_clicked(cu, id);
}
bool ui_checkbox(App *a, CuId id, const char *text, bool *value, bool enabled) {
    Cu *cu = a->cu;
    cu_open(cu, id,
            &(CuBox){.row = true, .gap = 10, .align_y = CU_CENTER, .interactive = true, .focusable = enabled,
                     .opacity = enabled ? 0 : 0.45f, CU_PAD2(3, 0), .w = CU_GROW});
    cu_open(cu, 0,
            &(CuBox){.w = CU_PX(16), .h = CU_PX(16), .radius = 4, .bg = *value ? C_ACCENT : C_FIELD,
                     .border = cu_focus_visible(cu, id) ? 2 : 1,
                     .border_color = cu_focus_visible(cu, id) ? C_FOCUS : *value ? C_ACCENT : 0xff555555,
                     .align_x = CU_CENTER, .align_y = CU_CENTER});
    if (*value)
        cu_icon(cu, "check", 14, 0xffffffff);
    cu_close(cu);
    if (text)
        cu_text(cu, 0, text,
                &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .wrap = CU_WRAP_WORD});
    cu_close(cu);
    if (enabled && cu_clicked(cu, id)) {
        *value = !*value;
        return true;
    }
    return false;
}
/* The same, sized to sit in a row of flat buttons. */
bool ui_flat_check(App *a, CuId id, const char *text, bool *value, float size, CuColor color, const char *tooltip,
                   bool enabled) {
    Cu *cu = a->cu;
    cu_open(cu, id,
            &(CuBox){.row = true, CU_PAD2(3, 6), .gap = 6, .radius = 5, .align_y = CU_CENTER,
                     .bg = hover_bg(a, id, enabled, 0, C_BUTTON), .interactive = true, .focusable = enabled,
                     .tooltip = tooltip, .opacity = enabled ? 0 : 0.45f,
                     .border = cu_focus_visible(cu, id) ? 1.5f : 0, .border_color = C_FOCUS});
    cu_open(cu, 0,
            &(CuBox){.w = CU_PX(size + 1), .h = CU_PX(size + 1), .radius = 3, .bg = *value ? C_ACCENT : 0, .border = 1,
                     .border_color = *value ? C_ACCENT : 0xff6a6a6a, .align_x = CU_CENTER, .align_y = CU_CENTER});
    if (*value)
        cu_icon(cu, "check", size, 0xffffffff);
    cu_close(cu);
    cu_text(cu, 0, text, &(CuTextStyle){.font = ui_font(a, size, 400), .color = color});
    cu_close(cu);
    if (enabled && cu_clicked(cu, id)) {
        *value = !*value;
        return true;
    }
    return false;
}
static void paint_switch(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    bool on = data != NULL;
    const CuPlatform *p = cu_platform(cu);
    p->fill_rect(c, r, r.h / 2, on ? C_ACCENT : 0xff3a3a3a);
    p->stroke_rect(c, (CuRect){r.x + .5f, r.y + .5f, r.w - 1, r.h - 1}, r.h / 2, 1, on ? C_ACCENT : 0xff4a4a4a);
    float d = r.h - 4;
    p->fill_rect(c, (CuRect){on ? r.x + r.w - d - 2 : r.x + 2, r.y + 2, d, d}, d / 2, 0xffefefef);
}
bool ui_switch(App *a, CuId id, bool *value, bool enabled) {
    Cu *cu = a->cu;
    cu_open(cu, id,
            &(CuBox){.interactive = true, .focusable = enabled, .opacity = enabled ? 0 : 0.5f, .radius = 11,
                     .border = cu_focus_visible(cu, id) ? 2 : 0, .border_color = C_FOCUS});
    cu_custom(cu, 0, CU_PX(38), CU_PX(22), paint_switch, *value ? (void *)1 : NULL);
    cu_close(cu);
    if (enabled && cu_clicked(cu, id)) {
        *value = !*value;
        return true;
    }
    return false;
}
/* The switch without its own click, for a row that takes the click, as a menu row does. */
void ui_switch_mark(App *a, bool on, bool enabled) {
    Cu *cu = a->cu;
    cu_open(cu, 0, &(CuBox){.opacity = enabled ? 0 : 0.5f});
    cu_custom(cu, 0, CU_PX(30), CU_PX(18), paint_switch, on ? (void *)1 : NULL);
    cu_close(cu);
}
bool ui_dropdown(App *a, CuId id, const char *const *choices, int count, int *value, bool enabled, bool flat) {
    Cu *cu = a->cu;
    const char *current = *value >= 0 && *value < count ? choices[*value] : "";
    cu_open(cu, id,
            &(CuBox){.row = true, .gap = 8, .align_y = CU_CENTER, CU_PAD2(5, 8), .radius = 6,
                     .bg = hover_bg(a, id, enabled, flat ? 0 : C_BUTTON, flat ? C_BUTTON : 0xff383838),
                     .border = flat ? (cu_focus_visible(cu, id) ? 1.5f : 0) : 1,
                     .border_color = cu_focus_visible(cu, id) ? C_FOCUS : C_BUTTON_BORDER, .interactive = true,
                     .focusable = enabled, .opacity = enabled ? 0 : 0.45f, .w = flat ? CU_FIT : CU_GROW});
    cu_text(cu, 0, current,
            &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_END,
                           .w = flat ? CU_FIT : CU_GROW});
    cu_icon(cu, "chevron-down", 16, C_TEXT);
    cu_close(cu);
    if (enabled && cu_clicked(cu, id))
        open_dropdown = open_dropdown == id ? 0 : id;
    bool changed = false;
    if (open_dropdown == id) {
        CuId layer = cu_ids(cu, "#dropdown", "");
        cu_layer_open(cu, layer, &(CuLayer){.kind = CU_LAYER_POPUP, .anchor = id, .placement = CU_PLACE_BELOW,
                                            .offset = 4, .align_end = flat},
                      &(CuBox){CU_PAD(5), .bg = C_MENU, .border = 1, .border_color = C_MENU_BORDER, .radius = 10,
                               .shadow = 12, .w = CU_FIT_MIN(flat ? 160 : cu_rect(cu, id).w)});
        for (int i = 0; i < count; i++) {
            CuId item = cu_idn(cu, "#dropdown-item", i);
            cu_open(cu, item,
                    &(CuBox){.row = true, .gap = 10, CU_PAD2(5, 10), .radius = 5, .w = CU_GROW, .align_y = CU_CENTER,
                             .bg = cu_hovered(cu, item) ? C_MENU_HOVER : 0, .interactive = true});
            cu_text(cu, 0, choices[i], &(CuTextStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .w = CU_GROW});
            if (i == *value)
                cu_icon(cu, "check", 16, C_CHECK);
            else
                cu_spacer(cu, CU_PX(16), CU_PX(16));
            cu_close(cu);
            if (cu_clicked(cu, item)) {
                changed = *value != i;
                *value = i;
                open_dropdown = 0;
            }
        }
        cu_layer_close(cu);
        if (cu_layer_dismissed(cu, layer))
            open_dropdown = 0;
    }
    return changed;
}
int ui_entry(App *a, CuId id, CuEdit *edit, const char *placeholder, bool enabled, size_t max_length) {
    Cu *cu = a->cu;
    CuId field = cu_ids(cu, "#field", "");
    (void)field;
    bool focused = cu_focused(cu, id);
    cu_open(cu, 0,
            &(CuBox){CU_PAD2(7, 9), .radius = 6, .bg = enabled ? C_FIELD : 0xff222222, .border = 1,
                     .border_color = focused ? C_FOCUS : C_FIELD_BORDER, .w = CU_GROW, .clip = true});
    int flags = cu_edit(cu, id, edit,
                        &(CuEditStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .placeholder_color = 0xff7a7a7a,
                                       .placeholder = placeholder, .disabled = !enabled, .max_length = max_length});
    cu_close(cu);
    return flags;
}
double ui_spin_value(CuEdit *edit, double fallback) {
    const char *text = cu_edit_text(edit);
    char *end = NULL;
    double v = strtod(text, &end);
    return end && end != text ? v : fallback;
}
void ui_spin_set(CuEdit *edit, double value, int digits) {
    char *text = ch_strdup_printf("%.*f", digits, value);
    cu_edit_set(edit, text);
    free(text);
}
bool ui_spin(App *a, CuId id, CuEdit *edit, double min, double max, double step, int digits, bool enabled) {
    Cu *cu = a->cu;
    double value = ui_spin_value(edit, min);
    bool changed = false;
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .radius = 6, .bg = 0xff292929, .border = 1,
                            .border_color = cu_focused(cu, id) ? C_FOCUS : 0xff404040, .align_y = CU_CENTER});
    cu_open(cu, 0, &(CuBox){CU_PAD2(7, 9), .w = CU_GROW, .clip = true});
    int flags = cu_edit(cu, id, edit, &(CuEditStyle){.font = ui_font(a, 14, 400), .color = C_TEXT, .disabled = !enabled, .max_length = 12});
    cu_close(cu);
    for (int i = 0; i < 2; i++) {
        CuId b = cu_idn(cu, "#spin", (int64_t)(id * 2 + (CuId)i));
        cu_open(cu, b, &(CuBox){CU_PAD2(7, 10), .border_t = 0, .bg = cu_hovered(cu, b) ? 0xff333333 : 0,
                                .interactive = true, .opacity = enabled ? 0 : .4f, .radius = i ? 6 : 0});
        cu_text(cu, 0, i ? "+" : "−", &(CuTextStyle){.font = ui_font(a, 15, 400), .color = C_TEXT});
        cu_close(cu);
        if (enabled && cu_clicked(cu, b)) {
            value = CH_CLAMP(value + (i ? step : -step), min, max);
            ui_spin_set(edit, value, digits);
            changed = true;
        }
    }
    cu_close(cu);
    if ((flags & CU_EDIT_SUBMIT) || (!cu_focused(cu, id) && (value < min || value > max))) {
        ui_spin_set(edit, CH_CLAMP(value, min, max), digits);
        changed = true;
    }
    return changed || (flags & CU_EDIT_CHANGED);
}
typedef struct {
    double fraction;
    CuColor color;
} Bar;
static void paint_bar(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    Bar *bar = data;
    const CuPlatform *pl = cu_platform(cu);
    pl->fill_rect(c, r, r.h / 2, 0xff383838);
    float width = r.w * (float)CH_CLAMP(bar->fraction, 0, 1);
    if (width > 0)
        pl->fill_rect(c, (CuRect){r.x, r.y, fmaxf(width, r.h), r.h}, r.h / 2, bar->color);
}
static void bar_build(App *a, CuId id, double fraction, CuColor color, float width, float height) {
    Bar *bar = cu_frame_alloc(a->cu, sizeof(Bar));
    *bar = (Bar){fraction, color};
    cu_custom(a->cu, id, width > 0 ? CU_PX(width) : CU_GROW, CU_PX(height), paint_bar, bar);
}
/* A bar shows how much is done, and nothing else: no sweep stands in for progress. */
void ui_progress(App *a, CuId id, double fraction, float width) {
    bar_build(a, id, fraction, C_ACCENT, width, 4);
}
/* How full something is, in the colour that says whether to worry. */
void ui_meter(App *a, CuId id, double fraction, CuColor color) {
    bar_build(a, id, fraction, color, 0, 6);
}
static void paint_spinner(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    (void)data;
    const CuPlatform *p = cu_platform(cu);
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, radius = fminf(r.w, r.h) / 2 - 1.5f;
    double start = fmod((double)cu_now(cu) / 1e6 * 2 * M_PI * 1.1, 2 * M_PI);
    p->move_to(c, cx + radius * (float)cos(start), cy + radius * (float)sin(start));
    for (int i = 1; i <= 18; i++) {
        double t = start + i * (M_PI * 1.4 / 18);
        p->line_to(c, cx + radius * (float)cos(t), cy + radius * (float)sin(t));
    }
    p->stroke_path(c, 1.8f, 0xffbdbdbd);
}
void ui_spinner(App *a, float size) {
    cu_custom(a->cu, 0, CU_PX(size), CU_PX(size), paint_spinner, NULL);
    cu_animate(a->cu, 33);
}
void ui_separator(App *a, float margin) {
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, CU_PAD2(margin, 0)});
    cu_open(a->cu, 0, &(CuBox){.w = CU_GROW, .h = CU_PX(1), .bg = 0xff363636});
    cu_close(a->cu);
    cu_close(a->cu);
}
bool ui_expander(App *a, CuId id, const char *title, bool *expanded) {
    Cu *cu = a->cu;
    cu_open(cu, id, &(CuBox){.row = true, .gap = 6, .align_y = CU_CENTER, CU_PAD2(4, 2), .interactive = true,
                             .focusable = true, .radius = 5, .bg = cu_hovered(cu, id) ? C_HOVER : 0});
    cu_icon(cu, *expanded ? "chevron-down" : "chevron-right", 14, C_MUTED);
    cu_text(cu, 0, title, &(CuTextStyle){.font = ui_font(a, 13, 400), .color = 0xffa5a5a5, .ellipsize = CU_ELLIPSIZE_END});
    cu_close(cu);
    if (cu_clicked(cu, id))
        *expanded = !*expanded;
    return *expanded;
}
static void paint_mark(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    (void)data;
    const CuPlatform *p = cu_platform(cu);
    float x = r.x + r.w / 2, y = r.y + r.h / 2, radius = fminf(r.w, r.h) / 2 - 3;
    for (int i = 0; i < 12; i++) {
        double angle = i * M_PI / 6;
        float tip = radius - (i % 3 == 1 ? 2 : 0);
        p->move_to(c, x + (float)cos(angle) * 2, y + (float)sin(angle) * 2);
        p->line_to(c, x + (float)cos(angle) * tip, y + (float)sin(angle) * tip);
        p->stroke_path(c, 2, C_MARK);
    }
}
void ui_mark(App *a, float size) {
    cu_custom(a->cu, 0, CU_PX(size), CU_PX(size + 4), paint_mark, NULL);
}
void ui_avatar(App *a, const char *name) {
    char *initial = ch_utf8_upper_first(name);
    cu_open(a->cu, 0, &(CuBox){.w = CU_PX(20), .h = CU_PX(20), .radius = 10, .bg = C_SELECTED, .align_x = CU_CENTER,
                               .align_y = CU_CENTER});
    cu_text(a->cu, 0, initial, &(CuTextStyle){.font = ui_font(a, 10, 400), .color = C_TEXT});
    cu_close(a->cu);
    free(initial);
}
void ui_mono_text(App *a, CuId id, const char *text, float size, bool selectable) {
    CuSpan span = {.text = text, .length = SIZE_MAX, .font = mono_font(a, size), .color = C_TEXT};
    cu_rich(a->cu, id, &span, 1, &(CuPara){.preformatted = true, .selectable = selectable, .line_height = 1.15f});
}
void ui_text_block(App *a, CuId id, const char *text, float size, CuColor color, bool selectable) {
    cu_text(a->cu, id, text,
            &(CuTextStyle){.font = ui_font(a, size, 400), .color = color, .wrap = CU_WRAP_WORD, .selectable = selectable});
}
