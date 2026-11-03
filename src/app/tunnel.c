/* SSH is owned by the desktop; the server stays private on the inference PC. */
#include "app.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    App *app;
    ChTunnel *tunnel;
} Ended;
static void tunnel_ended_main(void *data) {
    Ended *e = data;
    App *a = e->app;
    if (a->tunnel == e->tunnel) {
        char *detail = ch_tunnel_detail(e->tunnel);
        ch_tunnel_stop(e->tunnel);
        a->tunnel = NULL;
        if (!a->closing) {
            app_set_status(a, "Inference computer disconnected · open the model picker to reconnect");
            free(a->tunnel_error);
            a->tunnel_error = ch_strdup(*detail ? detail : "Turn on the inference computer, then open the model picker to retry.");
        }
        free(detail);
    }
    free(e);
}
static void tunnel_ended(void *data) {
    /* Runs on the tunnel's reaper thread. */
    Ended *e = data;
    e->app->p->post(tunnel_ended_main, e);
}
void app_track_tunnel(App *a, ChTunnel *t);
void app_track_tunnel(App *a, ChTunnel *t) {
    a->tunnel = t;
    ch_clear_pointer(&a->tunnel_error, free);
    Ended *e = ch_new0(Ended, 1);
    e->app = a;
    e->tunnel = t;
    ch_tunnel_on_exit(t, tunnel_ended, e);
}
void stop_model_tunnel(App *a) {
    if (a->tunnel) {
        ChTunnel *t = ch_steal(&a->tunnel);
        ch_tunnel_stop(t); /* clears the exit callback before killing ssh */
    }
}
bool ensure_model_tunnel(App *a) {
    if (!a->config.ssh_enabled || a->tunnel)
        return true;
    ChError *error = NULL;
    ChTunnel *t = ch_inferssd_tunnel(&a->config, &error);
    if (!t) {
        app_set_status(a, error->message);
        free(a->tunnel_error);
        a->tunnel_error = ch_strdup(error->message);
        ch_clear_error(&error);
        return false;
    }
    app_track_tunnel(a, t);
    app_set_status(a, "Connecting to inference computer…");
    return true;
}
