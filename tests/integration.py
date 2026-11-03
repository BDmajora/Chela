"""Exercise the real C HTTP client and agent loop against a local mock server."""
import json
import os
from pathlib import Path
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
PROPOSED_FILE = "# Example\n```c\nint main(void) { return 0; }\n```\n"

class Handler(BaseHTTPRequestHandler):
    requests = []
    gets = 0
    retry_error = True
    def log_message(self, *_):
        pass

    def do_GET(self):
        if self.path == "/v1/library/models":
            self.reply({"data": [{"id": "qwen-q8", "status": {"value": "unloaded"}},
                                 {"id": "qwen-q6", "status": {"value": "loaded"}},
                                 {"id": "qwen-q6"}, {"id": ""}, {"id": "x" * 256},
                                 {"id": "bad\nname"}, {"id": 123}, None]})
            return
        assert self.path == "/v1/models"
        type(self).gets += 1
        time.sleep(getattr(type(self), "discovery_delay", 0))
        self.reply({"data": [{"id": "stream"}, {"id": "tool"}]})

    def reply(self, data, status=200):
        body = json.dumps(data, ensure_ascii=False).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_POST(self):
        if self.path in ("/models/load", "/models/unload", "/library/models/load"):
            self.model_control()
            return
        assert self.path == "/v1/chat/completions"
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        type(self).requests.append(data)
        assert all("_chela_stats" not in message for message in data["messages"]), data
        model = data["model"]
        last_user = next((m["content"] for m in reversed(data["messages"]) if m["role"] == "user"), "")
        if last_user.startswith("thinking-test:"):
            expectations = {
                "default": (None, None), "off": ("none", 0), "quick": ("low", 256),
                "balanced": ("medium", 1024), "deep": ("high", 2048), "small-limit": ("high", 64),
                "gpt-off": ("low", None), "gpt-deep": ("high", None), "llama": ("medium", None),
                "other-provider": (None, None),
            }
            case = last_user.split(":", 1)[1]
            effort, budget = expectations[case]
            assert data.get("reasoning_effort") == effort, data
            assert data.get("thinking_budget_tokens") == budget, data
            if effort:
                assert data["chat_template_kwargs"]["enable_thinking"] is (effort != "none"), data
            else:
                assert "chat_template_kwargs" not in data, data
            assert ("reasoning_format" in data) is (case != "other-provider"), data
            for name in ("timings_per_token", "return_progress"):
                assert data.get(name) is (True if case != "other-provider" else None), data
            assert data.get("stream_options") == ({"include_usage": True} if case != "other-provider" else None), data
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            self.wfile.write((self.chunk({"reasoning_content": "working through "}) +
                             self.chunk({"reasoning_content": "the question"}) +
                             self.chunk({"content": "Ready"}, "stop") + "data: [DONE]\n\n").encode())
            return
        if last_user == "visual review conversation":
            text = ("## A quieter workspace\n\nThe interface now keeps your **conversation** in focus, with the controls close to where you need them.\n\n"
                    "- Choose your project below the message box.\n- Switch models from the row underneath.\n- Use `Ctrl+D` to dictate a draft and review it before sending.\n\n"
                    "### Small details, consistent behavior\n\nYour recent sessions remain in last-activity order. Grouping and sorting are remembered when you return.\n\n"
                    "```c\nconst char *message = \"Hello, Chela\";\nprintf(\"%s\\n\", message);\n```\n\n"
                    "> Next, open the plus menu to add a project skill.\n\nReady when you are.")
            self.reply({"choices": [{"message": {"role": "assistant", "content": text}}]})
            return
        if last_user == "connection error" and type(self).retry_error:
            type(self).retry_error = False
            self.reply({"error": {"message": "Model server unavailable"}}, 503)
            return
        if model == "timeout" or last_user == "slow request":
            time.sleep(4)
            self.reply({"choices": []})
            return
        if model == "http-error":
            self.reply({"error": {"message": "unknown model"}}, 404)
            return
        if model in ("workspace-chat", "chat-deny") or "inspect this project" in last_user:
            assert {t["function"]["name"] for t in data["tools"]} == {"list_files", "read_file"}
            last = data["messages"][-1]
            if model == "chat-deny":
                if last["role"] != "tool":
                    calls = [{"id": "bad-write", "type": "function", "function": {"name": "write_file", "arguments": json.dumps({"path": "forbidden.txt", "content": "bad"})}},
                             {"id": "bad-command", "type": "function", "function": {"name": "run_command", "arguments": json.dumps({"command": "touch forbidden.txt"})}}]
                    self.reply({"choices": [{"message": {"role": "assistant", "tool_calls": calls}}]})
                    return
                assert all("only permit reading" in m["content"] for m in data["messages"][-2:])
                text = "Changes blocked"
            elif last["role"] != "tool" or last.get("name") not in ("list_files", "read_file"):
                self.workspace_tool("list_files", {"path": "."})
                return
            elif last["name"] == "list_files":
                assert "src/" in last["content"]
                self.workspace_tool("read_file", {"path": "src/example.txt"})
                return
            else:
                assert last["content"] == "workspace file contents"
                text = "Read workspace file contents"
            self.reply({"choices": [{"message": {"role": "assistant", "content": text}}]})
            return
        if model == "json":
            self.reply({"choices": [{"message": {"role": "assistant", "content": "Hello 🦀"}, "finish_reason": "stop"}]})
            return
        if model == "metrics-json":
            self.assert_metrics_request(data)
            self.reply({"choices": [{"message": {"role": "assistant", "content": "Measured answer"}, "finish_reason": "stop"}],
                        "timings": {"predicted_n": 8, "prompt_n": 17, "predicted_ms": 640,
                                    "predicted_per_second": 12.5, "prompt_ms": 40},
                        "usage": {"completion_tokens": 10, "prompt_tokens": 19,
                                  "completion_tokens_details": {"reasoning_tokens": 2}}})
            return
        if model in ("blank-json", "reasoning-only-json"):
            message = {"role": "assistant", "content": " \t\n\u2003"}
            if model == "reasoning-only-json":
                message["reasoning_content"] = "Still considering the question"
            self.reply({"choices": [{"message": message, "finish_reason": "stop"}]})
            return
        if model == "metadata-filter":
            previous = next(message for message in data["messages"] if message["role"] == "assistant")
            assert previous["content"] == "Measured answer", data
            assert previous["reasoning_content"] == "Considering carefully", data
            assert data["messages"][-1]["content"] == "Follow-up after reopening", data
            self.reply({"choices": [{"message": {"role": "assistant", "content": "History accepted"}}]})
            return
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream; charset=utf-8")
        if model == "connection-drop":
            self.send_header("Content-Length", "4096")
        self.end_headers()
        if model == "metrics-stream":
            self.assert_metrics_request(data)
            payload = self.packet({"choices": [], "prompt_progress": {"total": 19, "processed": 6}})
            payload += self.chunk({"reasoning_content": "Considering carefully"})
            payload += self.packet({"choices": [{"delta": {"content": "Measured answer"}, "finish_reason": "stop"}],
                                    "timings": {"predicted_n": 8, "prompt_n": 17, "predicted_ms": 640,
                                                "predicted_per_second": 12.5, "prompt_ms": 40}})
            # OpenAI-compatible servers send final usage after the finish choice.
            payload += self.packet({"choices": [], "usage": {"completion_tokens": 10, "prompt_tokens": 19,
                                                            "completion_tokens_details": {"reasoning_tokens": 2}}})
            payload += "data: [DONE]\n\n"
        elif model == "usage-stream":
            self.assert_metrics_request(data)
            payload = self.chunk({"content": "Usage answer"}, "stop")
            payload += self.packet({"choices": [], "usage": {"completion_tokens": 7, "prompt_tokens": 13}})
            payload += "data: [DONE]\n\n"
        elif model in ("whitespace", "reasoning-only", "reasoning-limit"):
            delta = {"content": " \t\n\u2003"}
            if model != "whitespace":
                delta["reasoning_content"] = "Still considering the question"
            payload = self.chunk(delta, "length" if model == "reasoning-limit" else "stop") + "data: [DONE]\n\n"
        elif model == "length-limit":
            payload = self.chunk({"content": "Partial answer"}, "length") + "data: [DONE]\n\n"
        elif model == "connection-drop":
            payload = self.chunk({"content": "Partial before connection lost"})
            self.close_connection = True
        elif model == "malformed":
            payload = "data: {broken}\n\n"
        elif model == "empty":
            payload = "data: [DONE]\n\n"
        elif model == "truncated":
            payload = self.chunk({"content": "partial"})
        elif model == "bad-index":
            payload = self.chunk({"tool_calls": [{"index": -1}]})
        elif model == "loop" or (model in ("multi", "disabled-tools") and data["messages"][-1]["role"] != "tool"):
            calls = [{"index": 0, "id": "call-a", "function": {"name": "read_file", "arguments": '{"path":"missing.txt"}'}}]
            if model == "multi":
                calls.append({"index": 1, "id": "call-b", "function": {"name": "list_files", "arguments": '{"path":"."}'}})
                calls.reverse()  # Stream indexes can arrive out of order.
            if model == "disabled-tools":
                assert "tools" not in data
                calls[0]["function"] = {"name": "write_file", "arguments": '{"path":"forbidden.txt","content":"bad"}'}
            payload = self.chunk({"tool_calls": calls}) + self.chunk({}, "tool_calls") + "data: [DONE]\n\n"
        elif "write a test file" in last_user and data["messages"][-1]["role"] != "tool":
            payload = self.chunk({"tool_calls": [{"index": 0, "id": f"write-{len(type(self).requests)}", "function": {"name": "write_file", "arguments": json.dumps({"path": "approved.txt", "content": PROPOSED_FILE})}}]})
            payload += self.chunk({}, "tool_calls") + "data: [DONE]\n\n"
        elif model == "tool" and data["messages"][-1]["role"] != "tool":
            assert len(data["tools"]) == 4
            payload = self.chunk({"tool_calls": [{"index": 0, "id": "call-1", "function": {"name": "read_file", "arguments": '{"path":'}}]})
            payload += self.chunk({"tool_calls": [{"index": 0, "function": {"arguments": '"missing.txt"}'}}]})
            payload += self.chunk({}, "tool_calls") + "data: [DONE]\n\n"
        else:
            if model == "tool":
                assert data["messages"][-1]["tool_call_id"] == "call-1"
                assert "Tool error" in data["messages"][-1]["content"]
            payload = ":keepalive\r\n\r\n" + self.chunk({"role": "assistant", "content": "Hel"})
            payload += self.chunk({"content": "lo 🦀"}) + self.chunk({}, "stop") + "data: [DONE]\n\n"
        raw = payload.encode()
        try:
            for i in range(0, len(raw), 3):
                self.wfile.write(raw[i:i+3])
                self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass

    def model_control(self):
        """Router load/unload responses, using llama.cpp's error wording."""
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        load = self.path.endswith("/load")
        model = data["model"]
        if self.path.startswith("/library"):
            self.reply({"success": model == "prefixed"})
        elif model == "no-router":
            self.reply({"error": {"code": 404, "message": "File Not Found", "type": "not_found_error"}}, 404)
        elif model == "running" and load:
            self.reply({"error": {"code": 400, "message": "model is already running", "type": "invalid_request_error"}}, 400)
        elif model == "idle" and not load:
            self.reply({"error": {"code": 400, "message": "model is not running", "type": "invalid_request_error"}}, 400)
        elif model == "broken":
            self.reply({"error": {"code": 500, "message": "failed to spawn server instance", "type": "server_error"}}, 500)
        else:
            assert model == "qwen-q8", data
            self.reply({"success": True})

    @staticmethod
    def tool_message(name, arguments):
        return {"role": "assistant", "tool_calls": [{"id": "call-" + name, "type": "function", "function": {"name": name, "arguments": json.dumps(arguments)}}]}

    def workspace_tool(self, name, arguments):
        self.reply({"choices": [{"message": self.tool_message(name, arguments)}]})

    @staticmethod
    def assert_metrics_request(data):
        assert data["timings_per_token"] is True, data
        assert data["return_progress"] is True, data
        assert data["stream_options"] == {"include_usage": True}, data

    @staticmethod
    def packet(data):
        return "data: " + json.dumps(data, ensure_ascii=False) + "\r\n\r\n"

    @staticmethod
    def chunk(delta, finish=None):
        return Handler.packet({"choices": [{"delta": delta, "finish_reason": finish}]})

if __name__ == "__main__":
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    env = {**os.environ, "CHELA_TEST_URL": f"http://127.0.0.1:{server.server_port}/v1", "CHELA_TEST_ROOT": str(ROOT)}
    try:
        subprocess.run([os.environ.get("CHELA_TEST_BINARY", str(ROOT / "build/test_core")), "-p", "/integration"], env=env, check=True, timeout=45)
    finally:
        server.shutdown()
