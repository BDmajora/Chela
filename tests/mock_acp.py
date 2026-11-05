"""Small ACP v1 peer; no model or agent installation is needed."""
import json
import os
import sys
import time

mode = sys.argv[1]
current = "default"
def send(obj):
    print(json.dumps({"jsonrpc": "2.0", **obj}, ensure_ascii=False), flush=True)

def actions():
    """What Auto has to tell apart: (label, toolCall), every one of them a request."""
    here = os.getcwd()
    outside = os.path.dirname(here)
    os.symlink("/etc", "link")
    def edit(path):
        return {"kind": "edit", "title": "Edit", "locations": [{"path": path}]}
    def run(command):
        return {"kind": "execute", "title": "Run", "rawInput": {"command": command}}
    return [("edit-in", edit(here + "/a.txt")),
            ("read-in", {"kind": "read", "title": "Read", "locations": [{"path": "a.txt"}]}),
            ("search", {"kind": "search", "title": "Search"}),
            ("status", run(["git", "status"])),
            ("edit-out", edit(outside + "/elsewhere.txt")),
            ("read-out", {"kind": "read", "title": "Read", "locations": [{"path": "/etc/passwd"}]}),
            ("edit-link", edit(here + "/link/passwd")),
            ("edit-env", edit(here + "/.env")),
            ("edit-climb", edit(here + "/new/../../escape.txt")),
            ("build", run("make -j8")),
            ("push", run("git status && git push")),
            ("edit-blind", {"kind": "edit", "title": "Edit"}),
            ("other", {"kind": "other", "title": "Do something"})]

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
        if mode in ("modes", "refuse-mode"):
            result["modes"] = {"currentModeId": "default", "availableModes": [
                {"id": name, "name": name} for name in ("default", "acceptEdits", "auto", "plan", "bypassPermissions")]}
        send({"id": req["id"], "result": result})
    elif method == "session/set_mode":
        if mode == "refuse-mode":
            send({"id": req["id"], "error": {"code": -32000, "message": "not now"}})
            continue
        if mode in ("plan", "chat"):
            assert req["params"]["modeId"] == "plan"
        current = req["params"]["modeId"]
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
        if mode == "actions":
            answers = []
            for number, (label, tool) in enumerate(actions()):
                send({"id": f"action-{number}", "method": "session/request_permission", "params": {"sessionId": "test-session", "toolCall": tool, "options": [{"optionId": "yes", "kind": "allow_once", "name": "Allow once"}]}})
                outcome = json.loads(sys.stdin.readline())["result"]["outcome"]["outcome"]
                answers.append(f"{label}={'yes' if outcome == 'selected' else 'no'}")
            text = " ".join(answers)
        elif mode == "modes":
            text = f"mode={current}"
        else:
            text = "declined" if mode == "deny" else "Hello 🦀"
        for chunk in (text[:3], text[3:]):
            send({"method": "session/update", "params": {"sessionId": "test-session", "update": {"sessionUpdate": "agent_message_chunk", "content": {"type": "text", "text": chunk}}}})
        send({"id": req["id"], "result": {"stopReason": "end_turn"}})
