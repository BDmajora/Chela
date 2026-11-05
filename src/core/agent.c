#include "chela.h"
#include <stdlib.h>
#include <string.h>
bool ch_agent_run(ChRun *run, json_object *messages, ChError **error) {
    if (run->config.provider == 5) {
        if (!run->config.tools) {
            ch_fail_literal(error,
                                "ACP harnesses control their own tools. Enable project tools, or choose a "
                                "compatible model connection for chat with tools disabled.");
            return false;
        }
        return ch_acp_run(run, messages, error);
    }
    for (int round = 0; round < 24; round++) {
        if (ch_cancel_check(run->cancel, error))
            return false;
        if (run->emit)
            run->emit(CH_STATUS, round ? "Continuing…" : run->config.provider == 6
                ? "Connecting · selected model will load if needed…" : "Connecting to model…", run->data);
        json_object *answer = ch_complete(run, messages, error);
        if (!answer)
            return false;
        json_object_array_add(messages, answer);
        json_object *calls = ch_json_get(answer, "tool_calls");
        if (!calls || !json_object_array_length(calls))
            return true;
        /* Complete every tool result even when a user stops partway through a batch. */
        for (size_t i = 0; i < json_object_array_length(calls); i++) {
            json_object *c = json_object_array_get_idx(calls, i), *f = ch_json_get(c, "function");
            const char *name = ch_json_str(f, "name");
            if (run->emit)
                run->emit(CH_TOOL, name, run->data);
            ChError *local = NULL;
            char *result = NULL;
            if (!ch_run_has_tools(run))
                ch_fail_literal(&local, "Tools are disabled");
            else
                result = ch_tool_execute(run, name, ch_json_str(f, "arguments"), &local);
            char *text = result ? ch_strdup(result)
                                : ch_strdup_printf("Tool error: %s", local ? local->message : "No output");
            json_object *m = ch_message("tool", text);
            json_object_object_add(m, "tool_call_id", json_object_new_string(ch_json_str(c, "id")));
            json_object_object_add(m, "name", json_object_new_string(name));
            json_object_array_add(messages, m);
            if (run->emit)
                run->emit(CH_STATUS, local ? "Tool declined or failed" : "Tool completed", run->data);
            free(text);
            free(result);
            ch_clear_error(&local);
        }
    }
    ch_fail_literal(error, "Stopped after 24 tool rounds. Send a follow-up to continue.");
    return false;
}
