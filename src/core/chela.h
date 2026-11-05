#ifndef CHELA_H
#define CHELA_H
#include "base.h"
#include <json-c/json.h>
#include <sqlite3.h>

#define CHELA_VERSION "0.2.0"
#define CHELA_LIMIT (4u * 1024u * 1024u)
/* Keep existing stored mode values stable. */
typedef enum { CH_ASK, CH_PLAN, CH_BYPASS, CH_ACCEPT_EDITS, CH_AUTO } ChMode;
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
    char *project;  /* the working folder; empty or NULL when there is none */
    char **folders; /* other folders the agent may use, absolute; NULL-terminated, or NULL */
    bool code;      /* a Code session rather than a self-contained chat */
    ChMode mode;
    ChInteraction interaction;
    ChCancel *cancel;
    ChEmit emit;
    ChApprove approve;
    void *data;
    bool text_only; /* this model cannot read images, so attached ones are named instead */
} ChRun;
bool ch_run_read_only(const ChRun *run);
bool ch_run_has_tools(const ChRun *run); /* project tools need a folder to work in */
bool ch_path_in_session(const ChRun *run, const char *path); /* links followed: what is checked is what gets opened */
/* Command sandbox (sandbox.c): -1 when this system cannot confine commands. Enter it in
 * the child before exec; the rules then hold for everything the command starts. */
int ch_sandbox_open(const ChRun *run, ChError **error);
bool ch_sandbox_enter(int sandbox);
/* Permission modes (policy.c). NONE changes nothing; CONFINED is fine inside the sandbox;
 * ASK reaches past what the sandbox protects, or could not be read. */
typedef enum { CH_RISK_NONE, CH_RISK_CONFINED, CH_RISK_ASK } ChRisk;
const char *ch_mode_name(ChMode mode);
ChRisk ch_command_risk(const char *command, const char **why);
bool ch_path_protected(const char *path);
/* Whether an action goes ahead without asking in the run's mode. When it does not, *why
 * finishes "because…" if the mode has a reason, and is NULL if the mode always asks. */
bool ch_may_write(const ChRun *run, const char *path, const char **why);
bool ch_may_run(const ChRun *run, const char *command, bool confined, const char **why);
bool ch_may_harness(const ChRun *run, json_object *tool, const char **why);
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
char *ch_model_short_name(const char *model);
char *ch_model_family_name(const char *model);
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
    char **folders; /* also included in a Code session: NULL-terminated, or NULL */
    int64_t updated;
    ChInteraction interaction;
    ChMode mode;
    bool code;
    bool archived; /* kept, but out of the sidebar */
} ChSession;
ChStore *ch_store_open(const char *path, ChError **error);
void ch_store_close(ChStore *s);
bool ch_store_save(ChStore *s, const ChSession *session, json_object *messages, ChError **error);
ChVec *ch_store_list(ChStore *s, const char *search, ChError **error); /* of ChSession */
json_object *ch_store_load(ChStore *s, const char *id, ChError **error);
bool ch_store_delete(ChStore *s, const char *id, ChError **error);
bool ch_store_set_archived(ChStore *s, const char *id, bool archived, ChError **error);
bool ch_store_mentions(ChStore *s, const char *text); /* any session's messages contain text */
void ch_session_free(void *session);

/* Images (image.c): premultiplied ARGB32 pixels in native byte order. */
typedef struct {
    int width, height;
    uint32_t *pixels;
} ChImage;
ChImage *ch_image_decode(const void *data, size_t size, ChError **error);
ChImage *ch_image_fit(const ChImage *image, int side); /* never enlarges */
char *ch_image_png(const ChImage *image, size_t *length);
void ch_image_free(ChImage *image);

/* Attachments (attach.c). Images and text are copied into a private folder; other
 * files are mentioned by path. */
#define CH_ATTACH_TEXT_LIMIT (256u * 1024u)
typedef enum { CH_ATTACH_TEXT, CH_ATTACH_IMAGE, CH_ATTACH_FILE } ChAttachKind;
typedef struct {
    ChAttachKind kind;
    char *name, *path, *mime;
    size_t size;
    int width, height;
    bool external; /* path is the user's own file, not Chela's copy */
    unsigned order;
} ChAttachment;
ChAttachment *ch_attachment_from_path(const char *path, const char *folder, ChError **error);
ChAttachment *ch_attachment_from_bytes(const void *data, size_t size, const char *name, const char *folder, ChError **error);
void ch_attachment_free(void *attachment);
json_object *ch_attachment_json(const ChAttachment *attachment);
const char *ch_mime_sniff(const void *data, size_t size);
char *ch_base64_encode(const void *data, size_t size);
ChVec *ch_uri_list_parse(const char *text); /* local paths of file: URIs */
json_object *ch_wire_message(json_object *message, bool images); /* what a model server receives */
char *ch_message_text(json_object *message);        /* the same as plain text, for harnesses */

/* Model load progress (load.c): from the server's reports to a bar and an ETA. */
typedef enum { CH_LOAD_STARTING, CH_LOAD_READING, CH_LOAD_UPLOADING, CH_LOAD_CONTEXT, CH_LOAD_WARMING } ChLoadPhase;
typedef struct {
    ChLoadPhase phase;
    double target_mib, mapped_mib, gpu_mib;
    int upload_percent; /* -1 until reported */
} ChLoadSample;
typedef struct {
    int64_t started_us, sample_us, moved_us, reading_end_us;
    double previous_seconds, previous_tail, previous_mapped;
    ChLoadSample last;
    bool measured;
    double rate_mib, eta, shown, mapped_peak;
} ChLoadTrack;
typedef struct {
    double fraction, eta, rate_mib; /* eta < 0: unknown */
    bool measured, stalled, overdue;
} ChLoadEstimate;
void ch_load_begin(ChLoadTrack *track, int64_t now, double previous_seconds, double previous_tail, double previous_mapped);
void ch_load_sample(ChLoadTrack *track, const ChLoadSample *sample, int64_t now);
ChLoadEstimate ch_load_estimate(ChLoadTrack *track, int64_t now);
double ch_load_tail_seconds(const ChLoadTrack *track, int64_t now); /* since reading finished */
bool ch_load_sample_parse(json_object *load, ChLoadSample *sample);
char *ch_load_eta_text(double eta);                                 /* NULL when unknown */
char *ch_load_phase_text(const ChLoadTrack *track, const ChLoadEstimate *estimate);

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
json_object *ch_model_props(const ChConfig *config, const ChCancel *cancel, ChError **error);
/* What a server says about a model: its context in tokens (0: unknown) and whether it reads images (-1: unknown). */
typedef struct {
    int context, vision;
} ChModelInfo;
ChModelInfo ch_model_info_listed(json_object *entry); /* one /v1/models entry: known before the model loads */
ChModelInfo ch_model_info_props(json_object *props);  /* /props of a running model: what it really has */
const char *ch_vision_help(const ChConfig *config);   /* how to give a model that cannot read images the means to */
char *ch_format_count(long long value);               /* 8192 → "8,192" */
json_object *ch_tool_schemas(ChMode mode);
char *ch_tool_execute(ChRun *run, const char *name, const char *arguments, ChError **error);
char *ch_read_file(const char *root, const char *path, ChError **error);
bool ch_write_file(const char *root, const char *path, const char *content, ChError **error);
char *ch_command(const char *cwd, const char *command, int timeout, const ChCancel *cancel, ChError **error);
bool ch_agent_run(ChRun *run, json_object *messages, ChError **error);
bool ch_acp_run(ChRun *run, json_object *messages, ChError **error);
#endif
