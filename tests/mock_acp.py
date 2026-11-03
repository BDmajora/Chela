"""Small ACP v1 peer; no model or agent installation is needed."""
import json
import sys
import time

mode = sys.argv[1]
def send(obj):
    print(json.dumps({"jsonrpc": "2.0", **obj}, ensure_ascii=False), flush=True)

for line in sys.stdin:
    req = json.loads(line)
    method = req.get("method")
    if method == "initialize":
        if mode == "exit":
            sys.exit(0)
        if mode == "malformed":
            print("not JSON", flush=True)
            continue
        send({"id": req["id"], "result": {"protocolVersion": 99 if mode == "badversion" else 1, "agentCapabilities": {}}})
    elif method == "session/new":
        result = {"sessionId": "test-session"}
        if mode in ("plan", "chat"):
            result["modes"] = {"currentModeId": "code", "availableModes": [{"id": "plan", "name": "Plan"}]}
        send({"id": req["id"], "result": result})
    elif method == "session/set_mode":
        assert req["params"]["modeId"] == "plan"
        send({"id": req["id"], "result": {}})
    elif method == "session/prompt":
        assert "test request" in req["params"]["prompt"][0]["text"]
        if mode == "hang":
            time.sleep(20)
        if mode in ("permission", "deny", "chat"):
            send({"id": "permission-1", "method": "session/request_permission", "params": {"sessionId": "test-session", "toolCall": {"title": "Write test file", "kind": "edit"}, "options": [{"optionId": "yes", "kind": "allow_once", "name": "Allow once"}]}})
            reply = json.loads(sys.stdin.readline())
            assert reply["id"] == "permission-1"
            outcome = reply["result"]["outcome"]
            assert outcome["outcome"] == ("cancelled" if mode in ("deny", "chat") else "selected")
        text = "declined" if mode == "deny" else "Hello 🦀"
        for chunk in (text[:3], text[3:]):
            send({"method": "session/update", "params": {"sessionId": "test-session", "update": {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": chunk}}}})
        send({"id": req["id"], "result": {"stopReason": "end_turn"}})
