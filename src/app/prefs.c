/* Desktop presentation preferences are separate from model connection settings. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

void prefs_save(App *a) {
    ChKeyFile *k = ch_keyfile_new();
    ch_keyfile_set_string(k, "sidebar", "group",
                          a->group_environment ? "environment" : a->group_projects ? "project"
                          : a->group_dates     ? "date"
                                               : "none");
    ch_keyfile_set_bool(k, "sidebar", "sort_titles", a->sort_titles);
    ch_keyfile_set_int(k, "sidebar", "type", a->filter_type);
    ch_keyfile_set_int(k, "sidebar", "activity_days", a->activity_days);
    ch_keyfile_set_string(k, "sidebar", "project", a->filter_project ? a->filter_project : "");
    ch_keyfile_set_list(k, "sidebar", "pinned", (const char *const *)a->pinned.keys, a->pinned.len);
    ch_keyfile_set_bool(k, "sidebar", "pinned_collapsed", a->pinned_collapsed);
    ch_keyfile_set_bool(k, "sidebar", "recents_collapsed", a->recents_collapsed);
    ch_keyfile_set_list(k, "sidebar", "sections", (const char *const *)a->sections.data, a->sections.len);
    for (size_t i = 0; i < a->session_sections.len; i++)
        ch_keyfile_set_string(k, "session_sections", a->session_sections.keys[i], a->session_sections.values[i]);
    ch_keyfile_set_string(k, "voice", "dictation_command", a->dictation_command ? a->dictation_command : "");
    ch_keyfile_set_int(k, "appearance", "interface_font", a->interface_font);
    ch_keyfile_set_int(k, "appearance", "response_font", a->response_font);
    ch_keyfile_set_int(k, "appearance", "text_size", a->text_size);
    ch_keyfile_set_int(k, "appearance", "transcript_width", a->transcript_width);
    ch_keyfile_set_int(k, "appearance", "motion", a->motion);
    ch_keyfile_set_bool(k, "model", "keep_loaded", a->keep_loaded);
    ch_keyfile_set_int(k, "model", "idle_unload_seconds", a->idle_unload_seconds);
    for (size_t i = 0; i < a->load_seconds.len; i++)
        ch_keyfile_set_double(k, "model_load_seconds", a->load_seconds.keys[i], *(double *)a->load_seconds.values[i]);
    ChError *error = NULL;
    if (!ch_keyfile_save(k, a->preferences_path, &error)) {
        app_notice(a, "Could not save display preferences", error->message);
        ch_clear_error(&error);
    }
    ch_keyfile_free(k);
}
void prefs_load(App *a) {
    ch_map_init(&a->pinned, NULL);
    ch_map_init(&a->session_sections, free);
    ch_map_init(&a->load_seconds, free);
    ch_vec_init(&a->sections, free);
    a->text_size = 1;
    a->keep_loaded = true;
    a->idle_unload_seconds = 900;
    ChKeyFile *k = ch_keyfile_new();
    if (!ch_keyfile_load(k, a->preferences_path, NULL)) {
        ch_keyfile_free(k);
        return;
    }
    char *group = ch_keyfile_get_string(k, "sidebar", "group");
    a->group_projects = !ch_strcmp0(group, "project");
    a->group_dates = !ch_strcmp0(group, "date");
    a->group_environment = !ch_strcmp0(group, "environment");
    free(group);
    a->sort_titles = ch_keyfile_bool_or(k, "sidebar", "sort_titles", false);
    a->filter_type = CH_CLAMP(ch_keyfile_int_or(k, "sidebar", "type", 0), 0, 2);
    int days = ch_keyfile_int_or(k, "sidebar", "activity_days", 0);
    a->activity_days = days == 1 || days == 7 || days == 30 ? days : 0;
    a->filter_project = ch_keyfile_get_string(k, "sidebar", "project");
    a->dictation_command = ch_keyfile_get_string(k, "voice", "dictation_command");
    char **pins = ch_keyfile_get_list(k, "sidebar", "pinned");
    for (size_t i = 0; pins && pins[i]; i++)
        if (*pins[i])
            ch_map_set(&a->pinned, pins[i], NULL);
    ch_strfreev(pins);
    a->pinned_collapsed = ch_keyfile_bool_or(k, "sidebar", "pinned_collapsed", false);
    a->recents_collapsed = ch_keyfile_bool_or(k, "sidebar", "recents_collapsed", false);
    char **sections = ch_keyfile_get_list(k, "sidebar", "sections");
    for (size_t i = 0; sections && sections[i]; i++)
        if (*sections[i])
            ch_vec_push(&a->sections, ch_strdup(sections[i]));
    ch_strfreev(sections);
    char **keys = ch_keyfile_keys(k, "session_sections");
    for (size_t i = 0; keys && keys[i]; i++)
        ch_map_set(&a->session_sections, keys[i], ch_keyfile_get_string(k, "session_sections", keys[i]));
    ch_strfreev(keys);
    a->interface_font = CH_CLAMP(ch_keyfile_int_or(k, "appearance", "interface_font", 0), 0, 2);
    a->response_font = CH_CLAMP(ch_keyfile_int_or(k, "appearance", "response_font", 0), 0, 1);
    a->text_size = CH_CLAMP(ch_keyfile_int_or(k, "appearance", "text_size", 1), 0, 2);
    a->transcript_width = CH_CLAMP(ch_keyfile_int_or(k, "appearance", "transcript_width", 0), 0, 2);
    a->motion = CH_CLAMP(ch_keyfile_int_or(k, "appearance", "motion", 0), 0, 2);
    a->keep_loaded = ch_keyfile_bool_or(k, "model", "keep_loaded", true);
    a->idle_unload_seconds = CH_CLAMP(ch_keyfile_int_or(k, "model", "idle_unload_seconds", 900), 0, 86400);
    char **models = ch_keyfile_keys(k, "model_load_seconds");
    for (size_t i = 0; models && models[i]; i++) {
        double value = 0;
        if (ch_keyfile_get_double(k, "model_load_seconds", models[i], &value, NULL) && value > 0 && value < 86400 &&
            !strpbrk(models[i], "[]=\r\n")) {
            double *seconds = ch_new0(double, 1);
            *seconds = value;
            ch_map_set(&a->load_seconds, models[i], seconds);
        }
    }
    ch_strfreev(models);
    ch_keyfile_free(k);
}
