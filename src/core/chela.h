#ifndef CHELA_H
#define CHELA_H
#include "base.h"
#include <json-c/json.h>
#include <sqlite3.h>

#define CHELA_VERSION "0.2.0"
#define CHELA_LIMIT (4u * 1024u * 1024u)
/* Keep existing stored mode values stable. */
typedef enum { CH_ASK, CH_PLAN, CH_AUTO, CH_ACCEPT_EDITS } ChMode;
typedef enum { CH_AGENT, CH_CHAT } ChInteraction;
typedef enum { CH_THINK_DEFAULT, CH_THINK_OFF, CH_THINK_QUICK, CH_THINK_BALANCED, CH_THINK_DEEP } ChThinking;
typedef struct {
    char endpoint[1024], model[256], key_env[128], command[2048];
    char ssh_host[256], ssh_user[128], model_dir[1024], inference_binary[1024];
    int ssh_port, remote_port, local_port;
    bool ssh_enabled, ssh_remember;
    char gpu_device[128];
    int memory_mode, context_size, gpu_headroom;
    ChThinking thinking;
    int provider, max_tokens, timeout;
    double temperature;
    bool tools;
} ChConfig;
typedef enum { CH_TEXT, CH_THOUGHT, CH_STATUS, CH_TOOL, CH_METRICS, CH_ACTIVITY } ChEvent;
typedef void (*ChEmit)(ChEvent type, const char *text, void *data);
typedef bool (*ChApprove)(const char *title, const char *detail, void *data);
typedef struct {
    ChConfig config;
    char *project;
    ChMode mode;
    ChInteraction interaction;
    ChCancel *cancel;
    ChEmit emit;
    ChApprove approve;
    void *data;
} ChRun;
bool ch_run_read_only(const ChRun *run);
char *ch_system_prompt(const ChRun *run);
bool ch_thinking_supported(const ChConfig *c);
bool ch_model_is_gpt_oss(const ChConfig *c);
ChThinking ch_thinking_effective(const ChConfig *c);
const char *ch_thinking_name(ChThinking thinking);

const char *ch_json_str(json_object *o, const char *key);
json_object *ch_json_get(json_object *o, const char *key);
json_object *ch_json_parse(const char *text, ChError **error);
json_object *ch_message(const char *role, const char *content);
void ch_config_defaults(ChConfig *c);
bool ch_config_validate(const ChConfig *c, ChError **error);
bool ch_config_load(ChConfig *c, const char *path, ChError **error);
bool ch_config_save(const ChConfig *c, const char *path, ChError **error);
const char *ch_model_detail(json_object *model);
char *ch_endpoint(const char *base, const char *route, ChError **error);
char *ch_connection_url(const ChConfig *c, ChError **error);
char *ch_stats_summary(json_object *stats);
char *ch_title(const char *prompt);
bool ch_private_dir(const char *path, ChError **error);

/* InferSSD model library over SSH. The tunnel owns an ssh process and a reaper. */
typedef struct ChTunnel ChTunnel;
bool ch_inferssd_validate(const ChConfig *c, ChError **error);
json_object *ch_inferssd_install(const ChConfig *c, const ChCancel *cancel, ChError **error);
ChTunnel *ch_inferssd_tunnel(const ChConfig *c, ChError **error);
bool ch_tunnel_exited(ChTunnel *t);
char *ch_tunnel_detail(ChTunnel *t);
void ch_tunnel_on_exit(ChTunnel *t, void (*callback)(void *data), void *data); /* runs on the reaper thread */
void ch_tunnel_stop(ChTunnel *t);
char *ch_inferssd_diagnostics(const ChConfig *c, const ChCancel *cancel, ChError **error);
json_object *ch_inferssd_device_status(const ChConfig *c, const ChCancel *cancel, ChError **error);
bool ch_inferssd_wait(const ChConfig *c, ChTunnel *tunnel, const ChCancel *cancel, ChError **error);
/* Attributes passed to the SSH_ASKPASS helper process. */
typedef struct {
    ChConfig config;
    const char *prompt, *attempt;
    bool confirm, can_remember, repeated;
    char *marker;
} ChAskpass;
bool ch_askpass_prepare(int argc, char **argv, ChAskpass *ask);
void ch_askpass_clear(ChAskpass *ask);

/* Desktop keyring (Secret Service on Linux, loaded at run time). */
char *ch_ssh_password_lookup(const ChConfig *c, const ChCancel *cancel, ChError **error);
bool ch_ssh_password_store(const ChConfig *c, const char *password, const ChCancel *cancel, ChError **error);
bool ch_ssh_password_forget(const ChConfig *c, const ChCancel *cancel, ChError **error);
void ch_secret_free(char *secret);

typedef struct {
    sqlite3 *db;
} ChStore;
typedef struct {
    char *id, *title, *project, *model;
    int64_t updated;
    ChInteraction interaction;
    ChMode mode;
    bool code;
} ChSession;
ChStore *ch_store_open(const char *path, ChError **error);
void ch_store_close(ChStore *s);
bool ch_store_save(ChStore *s, const ChSession *session, json_object *messages, ChError **error);
ChVec *ch_store_list(ChStore *s, const char *search, ChError **error); /* of ChSession */
json_object *ch_store_load(ChStore *s, const char *id, ChError **error);
bool ch_store_delete(ChStore *s, const char *id, ChError **error);
void ch_session_free(void *session);

typedef bool (*ChSseEvent)(const char *data, void *user, ChError **error);
typedef struct {
    ChBuf line, event;
    ChSseEvent callback;
    void *user;
    bool done;
} ChSse;
void ch_sse_init(ChSse *s, ChSseEvent callback, void *user);
bool ch_sse_feed(ChSse *s, const char *data, size_t n, ChError **error);
bool ch_sse_finish(ChSse *s, ChError **error);
void ch_sse_clear(ChSse *s);
json_object *ch_complete(ChRun *run, json_object *messages, ChError **error);
json_object *ch_models(const ChConfig *config, const ChCancel *cancel, ChError **error);
bool ch_model_control(const ChConfig *config, bool load, const ChCancel *cancel, ChError **error);
json_object *ch_tool_schemas(ChMode mode);
char *ch_tool_execute(ChRun *run, const char *name, const char *arguments, ChError **error);
char *ch_read_file(const char *root, const char *path, ChError **error);
bool ch_write_file(const char *root, const char *path, const char *content, ChError **error);
char *ch_command(const char *cwd, const char *command, int timeout, const ChCancel *cancel, ChError **error);
bool ch_agent_run(ChRun *run, json_object *messages, ChError **error);
bool ch_acp_run(ChRun *run, json_object *messages, ChError **error);
#endif
