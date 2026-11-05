/* Composer: what you are writing, and the few controls that act on it. Settings live
 * in the Settings dialog; this stays a text box with a plus, a microphone and a send
 * button, and one row underneath that says where the message will go. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

/* Context usage around a dot for the model's state; while loading, the ring is the load. */
typedef struct {
    double fraction;
    CuColor arc, dot;
} Ring;
static void paint_ring(Cu *cu, CuCanvas *c, CuRect r, void *data) {
    Ring *ring = data;
    const CuPlatform *p = cu_platform(cu);
    float cx = r.x + r.w / 2, cy = r.y + r.h / 2, radius = r.w / 2 - 1.2f;
    const int steps = 48;
    for (int pass = 0; pass < 2; pass++) {
        int count = pass ? (int)(steps * CH_CLAMP(ring->fraction, 0, 1) + 0.5) : steps;
        if (!count)
            continue;
        for (int i = 0; i <= count; i++) {
            double angle = -M_PI / 2 + 2 * M_PI * i / steps;
            float x = cx + radius * (float)cos(angle), y = cy + radius * (float)sin(angle);
            if (i)
                p->line_to(c, x, y);
            else
                p->move_to(c, x, y);
        }
        p->stroke_path(c, 2, pass ? ring->arc : 0xff454545);
    }
    if (ring->dot)
        p->fill_rect(c, (CuRect){cx - 2.5f, cy - 2.5f, 5, 5}, 2.5f, ring->dot);
}
static void model_status_ring(App *a) {
    Cu *cu = a->cu;
    if (!model_chip_visible(a))
        return;
    if (a->busy)
        a->load_ready_us = 0; /* once a request starts, its monitor replaces the confirmation */
    const char *state = a->model_state ? a->model_state : "";
    bool loading = !strcmp(state, "loading"), ready = !strcmp(state, "loaded");
    bool failed = !strcmp(state, "failed") || (a->load_error && !ready);
    ModelContext context = model_context(a);
    Ring *ring = cu_frame_alloc(cu, sizeof(Ring));
    *ring = (Ring){0};
    if (loading) {
        char *text = model_eta_text(a, &ring->fraction);
        free(text);
        ring->arc = C_ACCENT;
    } else {
        ring->fraction = context.fraction;
        ring->arc = model_context_color(context.fraction);
        ring->dot = ready ? C_OK : failed ? C_DANGER : 0xff8a8a8a;
    }
    ChBuf tip;
    ch_buf_init(&tip);
    char *chip = model_status_summary(a);
    ch_buf_printf(&tip, "%s\n%s", a->config.model, chip);
    free(chip);
    ch_buf_append(&tip, "\nContext use (Ctrl+Shift+S)");
    CuId id = cu_id(cu, "model-chip");
    /* The number makes context use readable at a glance; while loading, the ring belongs to the load. */
    cu_open(cu, id, &(CuBox){.row = true, .gap = 5, .align_y = CU_CENTER, CU_PAD2(4, 5), .radius = 6,
                             .bg = cu_hovered(cu, id) ? C_BUTTON : 0, .interactive = true,
                             .tooltip = cu_frame_printf(cu, "%s", tip.str)});
    if (!loading && context.used && context.limit) {
        char *percent = context.fraction < 0.01 ? ch_strdup("<1%") : ch_strdup_printf("%.0f%%", context.fraction * 100);
        cu_text(cu, cu_id(cu, "context-percent"), percent,
                &(CuTextStyle){.font = ui_font(a, 12, 400), .color = context.fraction >= 0.8 ? model_context_color(context.fraction) : C_MUTED});
        free(percent);
    }
    cu_custom(cu, 0, CU_PX(15), CU_PX(15), paint_ring, ring);
    cu_close(cu);
    ch_buf_clear(&tip);
    if (cu_clicked(cu, id))
        menu_open(a, MENU_MODEL_PANEL, id, true);
    if (loading)
        cu_animate(cu, 250);
}
static void model_button(App *a) {
    Cu *cu = a->cu;
    CuId model = cu_id(cu, "model-button");
    char *name = ch_model_family_name(a->config.provider == 5 ? "ACP harness" : a->config.model);
    ChThinking thinking = ch_thinking_effective(&a->config);
    bool show = thinking != CH_THINK_DEFAULT && cu_width(cu) >= 900;
    cu_open(cu, model, &(CuBox){.row = true, CU_PAD2(3, 6), .gap = 5, .radius = 5, .align_y = CU_CENTER,
                                .bg = !a->busy && cu_hovered(cu, model) ? C_BUTTON : 0, .interactive = true,
                                .focusable = !a->busy,
                                .tooltip = a->tunnel_error ? a->tunnel_error : cu_frame_printf(cu, "%s\nModel and effort (Ctrl+Shift+L)", a->config.model),
                                .opacity = a->busy ? 0.55f : 0, .border = cu_focus_visible(cu, model) ? 1.5f : 0, .border_color = C_FOCUS});
    cu_text(cu, 0, name, &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_TEXT, .ellipsize = CU_ELLIPSIZE_MIDDLE, .w = CU_FIT_MAX(270)});
    if (show)
        cu_text(cu, 0, ch_thinking_name(thinking), &(CuTextStyle){.font = ui_font(a, 12, 400), .color = C_MUTED});
    cu_close(cu);
    free(name);
    if (!a->busy && cu_clicked(cu, model))
        menu_open(a, MENU_MODEL, model, true);
}
static void footer(App *a, float width) {
    Cu *cu = a->cu;
    bool chat = app_is_chat(a);
    cu_open(cu, cu_id(cu, "composer-footer"),
            &(CuBox){.row = true, .w = CU_GROW, .h = CU_FIT_MIN(30), .gap = 2, .align_y = CU_CENTER, .pad_t = 2, .pad_l = 4, .pad_r = 2});
    if (a->code)
        git_poll(a);
    /* The environment panel says the same things once it is open. */
    bool left = a->code && !app_panel_visible(a);
    if (left) {
        CuId environment = cu_id(cu, "environment-button");
        char *connection = ch_strdup_printf("%s  ·  %s", providers[a->config.provider],
                                            a->config.provider == 5 ? "stdio" : a->config.endpoint);
        if (ui_flat_button(a, environment, "computer", "This computer", NULL, 12, C_TEXT2, connection, !a->busy, 0))
            menu_open(a, MENU_ENVIRONMENT, environment, false);
        free(connection);
        /* "Chela + 2": the working folder and how many others the agent can reach. */
        CuId project = cu_id(cu, "project-button");
        char *base = ch_path_basename(a->project);
        char *label = !*a->project ? ch_strdup("Select folder")
                    : a->folders.len ? ch_strdup_printf("%s + %zu", base, a->folders.len) : ch_strdup(base);
        ChBuf places;
        ch_buf_init(&places);
        ch_buf_append(&places, *a->project ? a->project : "Choose where this session works");
        for (size_t i = 0; i < a->folders.len; i++)
            ch_buf_printf(&places, "\n%s", (const char *)a->folders.data[i]);
        if (ui_flat_button(a, project, "folder", label, NULL, 12, C_TEXT2, places.str, !a->busy, 210))
            menu_open(a, MENU_PROJECT, project, false);
        ch_buf_clear(&places);
        free(label);
        free(base);
        /* Where a session runs is decided before it begins. */
        if (!a->id && a->git_branch && !git_in_worktree(a, a->project))
            ui_flat_check(a, cu_id(cu, "worktree-check"), "Worktree", &a->use_worktree, 12, C_TEXT2,
                          "Run this session in its own git worktree, on a new branch from the last commit", !a->busy);
    } else if (!a->code && width >= 480) {
        cu_open(cu, 0, &(CuBox){.pad_l = 2, .w = CU_GROW});
        cu_text(cu, 0, "Chela can make mistakes. Please check responses.",
                &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xff757575, .ellipsize = CU_ELLIPSIZE_END, .w = CU_GROW});
        cu_close(cu);
    }
    if (a->code || width < 480)
        cu_spacer(cu, CU_GROW, CU_PX(1));
    model_button(a);
    if (a->code) {
        if (!chat) {
            CuId mode = cu_id(cu, "mode-button");
            cu_open(cu, 0, &(CuBox){.radius = 5, .bg = a->menu.kind == MENU_MODE ? C_BUTTON : 0});
            if (ui_flat_button(a, mode, NULL, ch_mode_name(a->mode), NULL, 12, C_TEXT2, "Permission mode (Ctrl+M)", !a->busy, 0))
                menu_open(a, MENU_MODE, mode, true);
            cu_close(cu);
        } else {
            cu_open(cu, 0, &(CuBox){CU_PAD2(0, 6)});
            ui_label(a, "Read only", 12, C_MUTED);
            cu_close(cu);
        }
    }
    model_status_ring(a);
    cu_close(cu);
}
static int text_field(App *a, CuId id, const char *placeholder, float min_height) {
    return cu_edit(a->cu, id, &a->composer,
                   &(CuEditStyle){.font = ui_font(a, 15, 400), .color = C_TEXT, .placeholder_color = 0xff898989,
                                  .placeholder = placeholder, .submit_on_enter = true, .disabled = a->busy,
                                  .min_h = min_height, .max_h = 220, .accept_data = true});
}
void composer_build(App *a) {
    Cu *cu = a->cu;
    float width = content_width(a);
    bool welcome_chat = !a->code && !a->id && !a->live_active && !json_object_array_length(a->messages);
    attachments_take_payloads(a);
    cu_open(cu, cu_id(cu, "composer-area"), &(CuBox){.w = CU_GROW, .align_x = CU_CENTER, .pad_t = 4});
    cu_open(cu, cu_id(cu, "composer-box"), &(CuBox){.w = CU_PX(width), .gap = 5});
    model_card_build(a);
    monitor_build(a);
    CuId composer = cu_id(cu, "composer");
    bool focused = cu_focused(cu, composer), dragging = cu_drag_active(cu);
    if (dragging) {
        cu_open(cu, cu_id(cu, "drop-hint"), &(CuBox){.row = true, .gap = 8, .w = CU_GROW, .align_x = CU_CENTER, .align_y = CU_CENTER,
                                                     CU_PAD2(9, 12), .radius = 10, .bg = 0xff2a2522, .border = 1, .border_color = C_ACCENT});
        cu_icon(cu, "document-open", 16, C_ACCENT);
        cu_text(cu, 0, "Drop to attach", &(CuTextStyle){.font = ui_font(a, 13, 400), .color = C_TEXT});
        cu_close(cu);
    }
    /* Said before sending, and with the way forward, instead of as a failed request. */
    bool refused = app_images_refused(a);
    if (refused) {
        char *text = ch_strconcat("This model can't read images yet. ", ch_vision_help(&a->config), NULL);
        cu_open(cu, cu_id(cu, "image-note"), &(CuBox){.w = CU_GROW, CU_PAD2(8, 12), .radius = 10, .bg = 0xff2a2522, .border = 1, .border_color = 0xff5a4a42});
        ui_label_wrap(a, text, 12, C_TEXT2, 0);
        cu_close(cu);
        free(text);
    }
    cu_open(cu, cu_id(cu, "composer-shell"),
            &(CuBox){.w = CU_GROW, .h = CU_FIT_MIN(44), .bg = C_COMPOSER, .radius = 16, .border = 1,
                     .border_color = dragging ? C_ACCENT : focused ? 0xff555555 : C_COMPOSER_BORDER, CU_PAD2(8, 8)});
    int flags = 0;
    const char *placeholder = a->busy ? "Working…" : a->id ? "Reply" : app_is_chat(a) ? "How can I help you today?"
                                                                                      : "Describe a task or ask a question…";
    /* The first screen gives the message room, with the buttons on a row of their own. */
    if (welcome_chat) {
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_l = 6, .pad_r = 6, .pad_t = 4, .pad_b = 2});
        attachments_row(a);
        flags = text_field(a, composer, placeholder, 44);
        cu_close(cu);
    }
    cu_open(cu, 0, &(CuBox){.row = true, .w = CU_GROW, .gap = 6, .align_y = CU_END});
    CuId attach = cu_id(cu, "attach-button");
    if (ui_icon_button(a, attach, "list-add", "Add files and photos (Ctrl+U)", !a->busy))
        menu_open(a, MENU_ATTACH, attach, false);
    cu_open(cu, 0, &(CuBox){.w = CU_GROW, .pad_t = welcome_chat ? 0 : 5, .pad_b = welcome_chat ? 0 : 5});
    if (!welcome_chat) {
        attachments_row(a);
        flags = text_field(a, composer, placeholder, 0);
    }
    cu_close(cu);
    if (flags & CU_EDIT_SUBMIT)
        app_send(a, false);
    bool has_content = !cu_edit_empty(&a->composer) || a->attachments.len, waiting = a->attach_pending > 0;
    if (!a->worktree_removing && (a->busy || has_content)) {
        CuId send = cu_id(cu, "send");
        bool can = a->busy || (!waiting && !refused);
        cu_open(cu, send, &(CuBox){CU_PAD(4), .radius = 6, .bg = can && cu_hovered(cu, send) ? 0xff444444 : 0, .interactive = true,
                                   .opacity = can ? 0 : 0.45f, .tooltip = a->busy ? "Stop (Esc)" : refused ? "Remove the image to send" : "Send (Enter)"});
        cu_icon(cu, a->busy ? "media-playback-stop" : "send", 18, C_TEXT2);
        cu_close(cu);
        if (can && cu_clicked(cu, send))
            app_send(a, false);
    }
    if (!a->busy && ui_icon_button(a, cu_id(cu, "dictate-button"), "microphone", "Dictate (Ctrl+D)", true))
        dictate(a);
    cu_close(cu);
    cu_close(cu);
    footer(a, width);
    cu_close(cu);
    cu_close(cu);
    float bottom = welcome_chat ? CH_MAX(100, cu_height(cu) * 0.39f) : 1;
    cu_spacer(cu, CU_PX(1), CU_PX(bottom));
    if (!cu_focus_id(cu) && !cu_modal_active(cu) && !a->menu.kind)
        cu_focus(cu, composer);
}
