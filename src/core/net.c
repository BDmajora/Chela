#include "chela.h"
#include <stdlib.h>
#include <ctype.h>
#include <curl/curl.h>
#include <string.h>

typedef struct {
    ChRun *run;
    ChSse sse;
    ChBuf *raw, *text, *thinking;
    json_object *calls;
    json_object *stats;
    ChError *error;
    bool streaming, finished, saw_choice;
    size_t received;
    int64_t started_us, last_activity_emit;
    bool tracking;
} Response;
static void emit(Response *r, ChEvent type, const char *s) {
    if (*s && r->run && r->run->emit)
        r->run->emit(type, s, r->run->data);
}
static void report_stats(Response *r, bool complete) {
    if (!r->tracking)
        return;
    json_object_object_add(r->stats, "request_id", json_object_new_int64(r->started_us));
    json_object_object_add(r->stats, "elapsed_ms", json_object_new_double((ch_monotonic_us() - r->started_us) / 1000.0));
    json_object_object_add(r->stats, "complete", json_object_new_boolean(complete));
    emit(r, CH_METRICS, json_object_to_json_string_ext(r->stats, JSON_C_TO_STRING_PLAIN));
}
static void copy_metric(json_object *to, const char *key, json_object *from, const char *source) {
    json_object *value = ch_json_get(from, source);
    if (value && (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) &&
        json_object_get_double(value) >= 0 && json_object_get_double(value) < 1e12)
        json_object_object_add(to, key, json_object_get(value));
}
/* Two refusals have a next step that the server's wording leaves out; NULL for any other error. */
static char *refusal_advice(const ChConfig *config, json_object *e) {
    if (strstr(ch_json_str(e, "message"), "image input is not supported"))
        return ch_strconcat("This model can't read images yet. ", ch_vision_help(config), NULL);
    if (strcmp(ch_json_str(e, "type"), "exceed_context_size_error"))
        return NULL;
    long long needed = json_object_get_int64(ch_json_get(e, "n_prompt_tokens")), limit = json_object_get_int64(ch_json_get(e, "n_ctx"));
    char *need = ch_format_count(needed), *room = ch_format_count(limit);
    char *text = ch_strdup_printf("This conversation no longer fits: it needs %s tokens and the model has room for %s. "
                                  "Start a new session, or raise Context tokens in Settings → Connections.", need, room);
    free(need);
    free(room);
    return text;
}
static bool consume(const char *data, void *user, ChError **error) {
    Response *r = user;
    json_object *o = ch_json_parse(data, error);
    if (!o)
        return false;
    json_object *e = ch_json_get(o, "error");
    if (e) {
        char *advice = refusal_advice(&r->run->config, e);
        if (advice)
            ch_fail_literal(error, advice);
        else
            ch_fail(error, "Server: %s", ch_json_str(e, "message"));
        free(advice);
        json_object_put(o);
        return false;
    }
    json_object *timings = ch_json_get(o, "timings"), *usage = ch_json_get(o, "usage");
    copy_metric(r->stats, "output_tokens", timings, "predicted_n");
    copy_metric(r->stats, "input_tokens", timings, "prompt_n");
    copy_metric(r->stats, "generation_ms", timings, "predicted_ms");
    copy_metric(r->stats, "tokens_per_second", timings, "predicted_per_second");
    copy_metric(r->stats, "prompt_ms", timings, "prompt_ms");
    copy_metric(r->stats, "output_tokens", usage, "completion_tokens");
    copy_metric(r->stats, "input_tokens", usage, "prompt_tokens");
    copy_metric(r->stats, "reasoning_tokens", ch_json_get(usage, "completion_tokens_details"), "reasoning_tokens");
    json_object *prompt = ch_json_get(o, "prompt_progress");
    if (prompt) {
        copy_metric(r->stats, "prompt_total", prompt, "total");
        copy_metric(r->stats, "prompt_processed", prompt, "processed");
        json_object_object_add(r->stats, "phase", json_object_new_string("prefill"));
    }
    json_object *choices = ch_json_get(o, "choices");
    if (choices && !json_object_is_type(choices, json_type_array)) {
        ch_fail_literal(error, "Invalid choices in server response");
        json_object_put(o);
        return false;
    }
    json_object *c = choices ? json_object_array_get_idx(choices, 0) : NULL;
    json_object *d = ch_json_get(c, "delta");
    if (!d)
        d = ch_json_get(c, "message");
    if (c)
        r->saw_choice = true;
    if (d) {
        const char *s = ch_json_str(d, "content"), *t = ch_json_str(d, "reasoning_content");
        if (!*t)
            t = ch_json_str(d, "reasoning");
        if (r->text->len + strlen(s) > CHELA_LIMIT || r->thinking->len + strlen(t) > CHELA_LIMIT) {
            ch_fail_literal(error, "Model response exceeded limit");
            json_object_put(o);
            return false;
        }
        ch_buf_append(r->text, s);
        emit(r, CH_TEXT, s);
        ch_buf_append(r->thinking, t);
        emit(r, CH_THOUGHT, t);
        if (*s || *t) {
            json_object_object_add(r->stats, "phase", json_object_new_string(*s ? "generating" : "thinking"));
            if (!ch_json_get(r->stats, "first_token_ms"))
                json_object_object_add(r->stats, "first_token_ms",
                    json_object_new_double((ch_monotonic_us() - r->started_us) / 1000.0));
        }
        json_object *calls = ch_json_get(d, "tool_calls");
        if (calls) {
            json_object_object_add(r->stats, "phase", json_object_new_string("tool_call"));
            if (!json_object_is_type(calls, json_type_array) || json_object_array_length(calls) > 64) {
                ch_fail_literal(error, "Invalid tool call array");
                json_object_put(o);
                return false;
            }
            for (size_t i = 0; i < json_object_array_length(calls); i++) {
                json_object *part = json_object_array_get_idx(calls, i), *index = ch_json_get(part, "index");
                int idx = index ? json_object_get_int(index) : (int)i;
                if (idx < 0 || idx >= 64) {
                    ch_fail_literal(error, "Tool call index outside 0–63");
                    json_object_put(o);
                    return false;
                }
                json_object *call = json_object_array_get_idx(r->calls, (size_t)idx);
                if (!call) {
                    call = json_object_new_object();
                    json_object_object_add(call, "type", json_object_new_string("function"));
                    json_object_object_add(call, "function", json_object_new_object());
                    json_object_array_put_idx(r->calls, (size_t)idx, call);
                }
                const char *id = ch_json_str(part, "id");
                if (*id)
                    json_object_object_add(call, "id", json_object_new_string(id));
                json_object *f = ch_json_get(call, "function"), *pf = ch_json_get(part, "function");
                const char *keys[] = {"name", "arguments"};
                for (int k = 0; k < 2; k++) {
                    const char *old = ch_json_str(f, keys[k]), *add = ch_json_str(pf, keys[k]);
                    if (strlen(old) + strlen(add) > CHELA_LIMIT) {
                        ch_fail_literal(error, "Tool arguments too large");
                        json_object_put(o);
                        return false;
                    }
                    char *v = ch_strconcat(old, add, NULL);
                    json_object_object_add(f, keys[k], json_object_new_string(v));
                    free(v);
                }
            }
        }
    }
    if (c && ch_json_get(c, "finish_reason") &&
        json_object_get_type(ch_json_get(c, "finish_reason")) != json_type_null) {
        r->finished = true;
        json_object_object_add(r->stats, "finish_reason", json_object_new_string(ch_json_str(c, "finish_reason")));
    }
    report_stats(r, false);
    json_object_put(o);
    return true;
}
static size_t body(char *data, size_t size, size_t count, void *user) {
    Response *r = user;
    size_t n = size * count;
    if (r->received + n > 16 * CHELA_LIMIT) {
        ch_fail_literal(&r->error, "Server response exceeded 64 MiB");
        return 0;
    }
    r->received += n;
    if (r->tracking && ch_monotonic_us() - r->last_activity_emit >= 200000) {
        emit(r, CH_ACTIVITY, "data");
        r->last_activity_emit = ch_monotonic_us();
    }
    if (r->streaming) {
        if (!ch_sse_feed(&r->sse, data, n, &r->error))
            return 0;
    } else
        ch_buf_append_len(r->raw, data, n);
    return n;
}
static size_t header(char *data, size_t size, size_t count, void *user) {
    Response *r = user;
    size_t n = size * count;
    char *h = ch_ascii_strdown(data, n);
    if (ch_str_has_prefix(h, "content-type:") && strstr(h, "text/event-stream"))
        r->streaming = true;
    if (r->tracking)
        emit(r, CH_ACTIVITY, "headers");
    free(h);
    return n;
}
static int progress(void *user, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    return ch_cancel_is_cancelled(user);
}
/* Router controls live beside, not under, the OpenAI-compatible /v1 API. */
static char *server_route(const char *base, const char *route, ChError **error) {
    char *api = ch_endpoint(base, "models", error);
    if (!api)
        return NULL;
    api[strlen(api) - strlen("/models")] = 0;
    if (ch_str_has_suffix(api, "/v1"))
        api[strlen(api) - strlen("/v1")] = 0;
    char *url = ch_strconcat(api, route, NULL);
    free(api);
    return url;
}
static bool request(ChRun *run, const char *route, json_object *payload, Response *r, ChError **error) {
    if (!ch_inferssd_wait(&run->config, NULL, run->cancel, error))
        return false;
    char *base = ch_connection_url(&run->config, error);
    if (!base)
        return false;
    char *url = route[0] == '/' ? server_route(base, route, error) : ch_endpoint(base, route, error);
    free(base);
    if (!url)
        return false;
    CURL *curl = curl_easy_init();
    if (!curl) {
        free(url);
        ch_fail_literal(error, "Cannot initialize HTTP client");
        return false;
    }
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    const char *key = ch_getenv(run->config.key_env);
    if (key && *key) {
        if (strpbrk(key, "\r\n")) {
            ch_fail_literal(error, "API key contains a newline");
            curl_easy_cleanup(curl);
            curl_slist_free_all(headers);
            free(url);
            return false;
        }
        char *auth = ch_strconcat("Authorization: Bearer ", key, NULL);
        headers = curl_slist_append(headers, auth);
        free(auth);
    }
    char curl_error[CURL_ERROR_SIZE] = {0};
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, (long)run->config.timeout);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, run->config.provider == 6 ? (long)run->config.timeout : 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, curl_error);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, r);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, r);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, run->cancel);
    if (payload)
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS,
                         json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN));
    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = true;
    if (ch_cancel_is_cancelled(run->cancel)) {
        ch_set_error_literal(error, CH_ERR_CANCELLED, 0, "Stopped");
        ok = false;
    } else if (r->error) {
        ch_propagate_error(error, ch_steal(&r->error));
        ok = false;
    } else if (rc != CURLE_OK) {
        ch_set_error(error, rc == CURLE_OPERATION_TIMEDOUT ? CH_ERR_TIMED_OUT : CH_ERR_FAILED, rc, rc == CURLE_OPERATION_TIMEDOUT
                    ? "Request timed out at %s. Open Diagnostics to check whether the server is loading, stalled, or offline. Details: %s"
                    : "Connection to %s failed or was lost. Open Diagnostics to check the server and SSH connection. Details: %s",
                    run->config.endpoint, *curl_error ? curl_error : curl_easy_strerror(rc));
        ok = false;
    } else if (status < 200 || status >= 300) {
        json_object *o = ch_json_parse(r->raw->str, NULL);
        const char *m = ch_json_str(ch_json_get(o, "error"), "message");
        char *advice = refusal_advice(&run->config, ch_json_get(o, "error"));
        if (advice)
            ch_set_error_literal(error, CH_ERR_HTTP, (int)status, advice);
        else
            ch_set_error(error, CH_ERR_HTTP, (int)status, "HTTP %ld%s%.500s", status,
                         *m ? ": " : " — check the server URL, model, and API key", m);
        free(advice);
        if (o)
            json_object_put(o);
        ok = false;
    }
    curl_easy_cleanup(curl);
    curl_slist_free_all(headers);
    free(url);
    return ok;
}
static void response_init(Response *r, ChRun *run) {
    memset(r, 0, sizeof(*r));
    r->run = run;
    r->raw = ch_buf_new(NULL);
    r->text = ch_buf_new(NULL);
    r->thinking = ch_buf_new(NULL);
    r->calls = json_object_new_array();
    r->stats = json_object_new_object();
    r->started_us = ch_monotonic_us();
    ch_sse_init(&r->sse, consume, r);
}
static void response_clear(Response *r) {
    ch_sse_clear(&r->sse);
    ch_buf_free(r->raw, false);
    ch_buf_free(r->text, false);
    ch_buf_free(r->thinking, false);
    json_object_put(r->calls);
    json_object_put(r->stats);
    ch_clear_error(&r->error);
}
json_object *ch_complete(ChRun *run, json_object *messages, ChError **error) {
    Response r;
    response_init(&r, run);
    r.tracking = true;
    json_object_object_add(r.stats, "phase", json_object_new_string("waiting"));
    report_stats(&r, false);
    json_object *p = json_object_new_object();
    json_object_object_add(p, "model", json_object_new_string(run->config.model));
    /* Desktop metadata is stored with messages, never sent to a model; attachments are read in here. */
    json_object *wire = json_object_new_array();
    for (size_t i = 0; i < json_object_array_length(messages); i++)
        json_object_array_add(wire, ch_wire_message(json_object_array_get_idx(messages, i), !run->text_only));
    json_object_object_add(p, "messages", wire);
    json_object_object_add(p, "stream", json_object_new_boolean(true));
    json_object_object_add(p, "max_tokens", json_object_new_int(run->config.max_tokens));
    json_object_object_add(p, "temperature", json_object_new_double(run->config.temperature));
    if (ch_thinking_supported(&run->config)) {
        json_object_object_add(p, "reasoning_format", json_object_new_string("deepseek"));
        json_object_object_add(p, "timings_per_token", json_object_new_boolean(true));
        json_object_object_add(p, "return_progress", json_object_new_boolean(true));
        json_object *options = json_object_new_object();
        json_object_object_add(options, "include_usage", json_object_new_boolean(true));
        json_object_object_add(p, "stream_options", options);
    }
    ChThinking thinking = ch_thinking_effective(&run->config);
    if (thinking != CH_THINK_DEFAULT) {
        const char *levels[] = {NULL, "none", "low", "medium", "high"};
        json_object_object_add(p, "reasoning_effort", json_object_new_string(levels[thinking]));
        json_object *kwargs = json_object_new_object();
        json_object_object_add(kwargs, "enable_thinking", json_object_new_boolean(thinking != CH_THINK_OFF));
        json_object_object_add(p, "chat_template_kwargs", kwargs);
        /* InferSSD's sampler understands budgets; other servers may not. GPT-OSS
         * uses its native low/medium/high template setting instead. */
        if (run->config.provider == 6 && !ch_model_is_gpt_oss(&run->config)) {
            const int budgets[] = {0, 0, 256, 1024, 3072};
            json_object_object_add(p, "thinking_budget_tokens",
                json_object_new_int(CH_MIN(budgets[thinking], run->config.max_tokens / 2)));
        }
    }
    if (ch_run_has_tools(run))
        json_object_object_add(p, "tools", ch_tool_schemas(ch_run_read_only(run) ? CH_PLAN : run->mode));
    bool ok = request(run, "chat/completions", p, &r, error);
    json_object_put(p);
    if (ok)
        ok = r.streaming ? ch_sse_finish(&r.sse, error) : consume(r.raw->str, &r, error);
    if (ok && (!r.saw_choice || (r.streaming && !r.finished && !r.sse.done))) {
        ch_fail_literal(error, "Server closed an incomplete response");
        ok = false;
    }
    json_object *out = NULL;
    if (ok) {
        /* Refuse sparse or unidentifiable tool calls before they enter history. */
        for (size_t i = 0; i < json_object_array_length(r.calls); i++) {
            json_object *c = json_object_array_get_idx(r.calls, i);
            if (!c || !*ch_json_str(c, "id") || !*ch_json_str(ch_json_get(c, "function"), "name")) {
                ch_fail_literal(error, "Incomplete tool call from model");
                ok = false;
                break;
            }
        }
    }
    if (ok && !json_object_array_length(r.calls)) {
        bool visible = false;
        for (const char *s = r.text->str; *s;)
            visible |= !ch_unichar_isspace(ch_utf8_get(s, &s));
        if (!visible) {
            ch_fail_literal(error,
                r.thinking->len ? "The model produced thinking but no final answer. Try Quick/Off thinking or increase Max output tokens; check Diagnostics for server details."
                                : "The model returned no answer or tool calls. Check the model and chat template in Diagnostics, then retry.");
            ok = false;
        }
    }
    json_object_object_add(r.stats, "incomplete", json_object_new_boolean(!ok));
    report_stats(&r, true);
    if (ok) {
        out = ch_message("assistant", r.text->str);
        json_object_object_add(out, "_chela_stats", json_object_get(r.stats));
        if (r.thinking->len)
            json_object_object_add(out, "reasoning_content", json_object_new_string(r.thinking->str));
        if (json_object_array_length(r.calls))
            json_object_object_add(out, "tool_calls", json_object_get(r.calls));
    }
    response_clear(&r);
    return out;
}
json_object *ch_models(const ChConfig *config, const ChCancel *cancel, ChError **error) {
    ChRun run = {.config = *config, .cancel = (ChCancel *)cancel};
    Response r;
    response_init(&r, &run);
    json_object *out = NULL;
    if (request(&run, "models", NULL, &r, error)) {
        json_object *o = ch_json_parse(r.raw->str, error), *data = ch_json_get(o, "data");
        if (data && json_object_is_type(data, json_type_array)) {
            out = json_object_new_array();
            ChMap seen;
            ch_map_init(&seen, NULL);
            for (size_t i = 0; i < json_object_array_length(data); i++) {
                json_object *model = json_object_array_get_idx(data, i);
                const char *id = ch_json_str(model, "id");
                bool valid = *id && strlen(id) < sizeof(config->model);
                for (const char *p = id; *p; p++)
                    valid &= !iscntrl((unsigned char)*p);
                if (valid && !ch_map_has(&seen, id)) {
                    ch_map_set(&seen, id, NULL);
                    json_object_array_add(out, json_object_get(model));
                }
            }
            ch_map_clear(&seen);
        }
        else if (o)
            ch_fail_literal(error, "Server did not return a models array");
        if (o)
            json_object_put(o);
    }
    response_clear(&r);
    return out;
}
/* /props beside the API. autoload=false: asking about a model must never load it. */
json_object *ch_model_props(const ChConfig *config, const ChCancel *cancel, ChError **error) {
    ChRun run = {.config = *config, .cancel = (ChCancel *)cancel};
    Response r;
    response_init(&r, &run);
    json_object *out = NULL;
    CURL *curl = curl_easy_init();
    char *model = curl ? curl_easy_escape(curl, config->model, 0) : NULL;
    char *route = ch_strdup_printf("/props?model=%s&autoload=false", model ? model : "");
    if (request(&run, route, NULL, &r, error))
        out = ch_json_parse(r.raw->str, error);
    free(route);
    curl_free(model);
    if (curl)
        curl_easy_cleanup(curl);
    response_clear(&r);
    return out;
}
bool ch_model_control(const ChConfig *config, bool load, const ChCancel *cancel, ChError **error) {
    ChRun run = {.config = *config, .cancel = (ChCancel *)cancel};
    Response r;
    response_init(&r, &run);
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "model", json_object_new_string(config->model));
    ChError *local = NULL;
    bool ok = request(&run, load ? "/models/load" : "/models/unload", payload, &r, &local);
    /* A repeated request already has the result the user asked for. */
    if (!ok && local && local->kind == CH_ERR_HTTP && local->code >= 400 &&
        (load ? strstr(local->message, "already running") || strstr(local->message, "already loaded")
              : strstr(local->message, "not running") || strstr(local->message, "not loaded"))) {
        ch_clear_error(&local);
        ok = true;
    }
    if (local && local->kind == CH_ERR_HTTP && (local->code == 404 || local->code == 405))
        ch_prefix_error(&local, "This server cannot %s models on request; it needs a llama.cpp/InferSSD model router. ",
                       load ? "load" : "unload");
    if (local)
        ch_propagate_error(error, local);
    json_object_put(payload);
    response_clear(&r);
    return ok;
}
