/* Composer, its controls and the footer under it. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

static void controls(App *a, float width) {
    Cu *cu = a->cu;
    CuId attach = cu_id(cu, "attach-button");
    if (ui_icon_button(a, attach, "list-add", "Add context and tools (Ctrl+Shift+A)", !a->busy))
        menu_open(a, MENU_ATTACH, attach, false);
    if (ui_icon_button(a, cu_id(cu, "dictate-button"), "microphone", "Dictate (Ctrl+D)", !a->busy))
        dictate(a);
    ui_icon_button(a, cu_id(cu, "voice-button"), "waveform", "Use voice mode · spoken replies are not available", false);
    CuId caret = cu_id(cu, "voice-menu-button");
    if (ui_icon_button_sized(a, caret, "chevron-down", 16, 4, "Voice options (Ctrl+Shift+V)", !a->busy, false))
        menu_open(a, MENU_VOICE, caret, true);
    CuId thinking = cu_id(cu, "thinking-button");
    char *label = ch_strdup_printf("Thinking: %s", ch_thinking_name(ch_thinking_effective(&a->config)));
    char *tip = ch_strdup_printf("%s (Ctrl+Shift+T). Availability depends on the model.", label);
    if (ui_flat_button(a, thinking, "thinking", cu_width(cu) < 1100 || width < 600 ? NULL : label, "chevron-down", 12,
                       C_TEXT2, tip, !a->busy, 0))
        menu_open(a, MENU_THINKING, thinking, false);
    free(label);
    free(tip);
}
static void footer(App *a, float width) {
    Cu *cu = a->cu;
    bool chat = app_is_chat(a);
    cu_open(cu, cu_id(cu, "composer-footer"),
            &(CuBox){.row = true, .w = CU_GROW, .h = CU_FIT_MIN(30), .gap = 2, .align_y = CU_CENTER, .pad_t = 4});
    if (a->code) {
        CuId environment = cu_id(cu, "environment-button");
        char *connection = ch_strdup_printf("%s  ·  %s", providers[a->config.provider],
                                            a->config.provider == 5 ? "stdio" : a->config.endpoint);
        if (ui_flat_button(a, environment, "computer", "Local", "chevron-down", 12, 0xffb5b5b5, connection, !a->busy, 0))
            menu_open(a, MENU_ENVIRONMENT, environment, false);
        free(connection);
        CuId project = cu_id(cu, "project-button");
        char *base = ch_path_basename(a->project);
        if (ui_flat_button(a, project, "folder", base, "chevron-down", 12, 0xffb5b5b5, a->project, !a->busy,
                           cu_width(cu) < 1100 ? 90 : 190))
            menu_open(a, MENU_PROJECT, project, false);
        free(base);
        if (!chat) {
            const char *modes[] = {"Manual", "Plan", "Bypass permissions", "Accept edits"};
            CuId mode = cu_id(cu, "mode-button");
            if (ui_flat_button(a, mode, NULL, modes[a->mode], "chevron-down", 12, C_TEXT2, "Permission mode (Ctrl+M)",
                               !a->busy, 0))
                menu_open(a, MENU_MODE, mode, false);
        } else {
            cu_open(cu, 0, &(CuBox){CU_PAD2(0, 8)});
            ui_label(a, "Read only", 12, C_MUTED);
            cu_close(cu);
        }
    }
    controls(a, width);
    if (!a->code && a->id) {
        cu_open(cu, 0, &(CuBox){.w = CU_GROW, .align_x = CU_CENTER});
        cu_text(cu, 0, "Chela can make mistakes. Please check responses.",
                &(CuTextStyle){.font = ui_font(a, 11, 400), .color = 0xff757575, .ellipsize = CU_ELLIPSIZE_END});
        cu_close(cu);
    } else
        cu_spacer(cu, CU_GROW, CU_PX(1));
    model_chip_build(a, width);
    CuId model = cu_id(cu, "model-button");
    if (ui_flat_button(a, model, NULL, a->config.provider == 5 ? "ACP harness" : a->config.model, "chevron-down", 12,
                       C_TEXT2, a->tunnel_error ? a->tunnel_error : "Select model (Ctrl+Shift+L)", !a->busy,
                       cu_width(cu) < 1100 ? 120 : 230))
        menu_open(a, MENU_MODEL, model, true);
    cu_close(cu);
}
static void recovery(App *a) {
    Cu *cu = a->cu;
    if (!a->recovery_visible)
        return;
    cu_open(cu, 0, &(CuBox){.row = true, .gap = 8, .pad_b = 4});
    if (ui_flat_button(a, cu_id(cu, "recovery-settings"), NULL, "Connection settings", NULL, 13, C_TEXT, NULL, !a->busy, 0))
        open_settings(a, 1);
    if (ui_flat_button(a, cu_id(cu, "recovery-retry"), NULL, "Retry message", NULL, 13, C_TEXT, NULL,
                       !a->busy && a->retry_enabled, 0))
        app_send(a, true);
    cu_close(cu);
}
void composer_build(App *a) {
    Cu *cu = a->cu;
    float width = content_width(a);
    bool welcome_chat = !a->code && !a->id && !a->live_active && !json_object_array_length(a->messages);
    cu_open(cu, cu_id(cu, "composer-area"), &(CuBox){.w = CU_GROW, .align_x = CU_CENTER});
    cu_open(cu, cu_id(cu, "composer-box"), &(CuBox){.w = CU_PX(width), .gap = 4});
    model_card_build(a);
    monitor_build(a);
    recovery(a);
    CuId composer = cu_id(cu, "composer");
    bool focused = cu_focused(cu, composer);
    cu_open(cu, cu_id(cu, "composer-shell"),
            &(CuBox){.row = true, .w = CU_GROW, .gap = 8, .bg = C_COMPOSER, .radius = 14, .border = 1,
                     .border_color = focused ? 0xff555555 : C_COMPOSER_BORDER, CU_PAD2(welcome_chat ? 12 : 10, 14),
                     .align_y = CU_END});
    const char *placeholder = a->id ? "Reply" : app_is_chat(a) ? "How can I help you today?" : "Describe a task or ask a question…";
    int flags = cu_edit(cu, composer, &a->composer,
                        &(CuEditStyle){.font = ui_font(a, 15, 400), .color = C_TEXT, .placeholder_color = 0xff898989,
                                       .placeholder = placeholder, .submit_on_enter = true, .disabled = a->busy,
                                       .min_h = welcome_chat ? 64 : 0, .max_h = 200});
    if (flags & CU_EDIT_SUBMIT)
        app_send(a, false);
    CuId send = cu_id(cu, "send");
    bool can_send = a->busy || !cu_edit_empty(&a->composer);
    cu_open(cu, send, &(CuBox){CU_PAD(3), .radius = 5, .bg = can_send && cu_hovered(cu, send) ? 0xff444444 : 0,
                               .interactive = true, .opacity = can_send ? 0 : 0.45f,
                               .tooltip = a->busy ? "Stop (Esc)" : "Send (Enter)"});
    cu_icon(cu, a->busy ? "media-playback-stop" : "send", 18, a->busy ? C_TEXT : C_MUTED);
    cu_close(cu);
    if (can_send && cu_clicked(cu, send))
        app_send(a, false);
    cu_close(cu);
    footer(a, width);
    cu_close(cu);
    cu_close(cu);
    float bottom = welcome_chat ? CH_MAX(100, cu_height(cu) * 0.39f) : 1;
    cu_spacer(cu, CU_PX(1), CU_PX(bottom));
    if (!cu_focus_id(cu) && !cu_modal_active(cu) && !a->menu.kind)
        cu_focus(cu, composer);
}
