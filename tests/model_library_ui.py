"""Drive setup/login/model switching/restart with the real desktop and a fake SSH peer.

The fake transport calls Chela's actual askpass process, which opens its own
headless window that this test answers, and serves a loopback API. It never
contacts an inference computer, changes real settings, or loads weights.
"""
import json
import os
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tempfile
import time

from headless import BUILD, ROOT, Harness, wait_for

FAKE_SSH = r'''
import json, os, pathlib, subprocess, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
root = pathlib.Path(os.environ["CHELA_FIXTURE"])
args = sys.argv[1:]
if "-N" not in args:
    source = sys.stdin.read()
    assert "--models-max" in source and "--models-autoload" in source
    assert "--install" in args[-1]
    assert "--memory-mode 1" in args[-1] and "--context-size 4096" in args[-1], args[-1]
    assert "--gpu-headroom 2048" in args[-1] and "--device 'auto'" in args[-1], args[-1]
    (root / "installed").touch()
    print(json.dumps({"data": [{"id": "First-Q8"}, {"id": "Second-Q6"}]}))
    sys.exit(0)
assert "StrictHostKeyChecking=ask" in args
assert "ControlMaster=yes" in args and "ControlPersist=no" in args
(root / "tunnel-started").touch()
if (root / "ask-trust").exists():
    result = subprocess.run([os.environ["SSH_ASKPASS"], "The authenticity of host '10.0.0.55' cannot be established.\nED25519 key fingerprint is SHA256:FIXTURE.\nAre you sure you want to continue connecting (yes/no)?"], capture_output=True, text=True)
    if result.returncode or result.stdout.strip() != "yes":
        sys.exit(1)
    (root / "ask-trust").unlink()
for attempt in range(2):
    result = subprocess.run([os.environ["SSH_ASKPASS"], "brody@10.0.0.55's password:"], capture_output=True, text=True)
    if result.returncode: sys.exit(1)
    if result.stdout.strip() == "fixture-only": break
else: sys.exit(1)
with (root / "authenticated").open("a") as stream: stream.write("ok\n")
forward = args[args.index("-L") + 1]
port = int(forward.split(":")[1])
class Handler(BaseHTTPRequestHandler):
    loaded = None
    def log_message(self, *_): pass
    def reply(self, data):
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass
    def do_GET(self):
        assert self.path == "/v1/models"
        self.reply({"data": [{"id": name, "status": {"value": "loaded" if self.loaded == name else "unloaded"}} for name in ("First-Q8", "Second-Q6")]})
    def do_POST(self):
        assert self.path == "/v1/chat/completions"
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        type(self).loaded = data["model"]
        with (root / "requests.jsonl").open("a") as stream: stream.write(json.dumps(data) + "\n")
        if (root / "slow-test").exists(): time.sleep(1)
        self.reply({"choices": [{"message": {"role": "assistant", "content": "Reply from " + self.loaded}, "finish_reason": "stop"}]})
ThreadingHTTPServer(("127.0.0.1", port), Handler).serve_forever()
'''


def main():
    app = harness = None
    log = open(BUILD / "model-library-ui.log", "w")
    try:
        with tempfile.TemporaryDirectory(prefix="chela-library-ui-") as folder:
            root = Path(folder)
            harness = Harness(root)
            config = root / "config/chela"
            config.mkdir(parents=True)
            (config / "settings.ini").write_text("[connection]\nprovider=0\nmodel=before-setup\n")
            (root / "bin").mkdir()
            (root / "runtime").mkdir(mode=0o700)
            fake = root / "bin/ssh"
            fake.write_text(f"#!{sys.executable}\n" + FAKE_SSH)
            fake.chmod(0o700)
            (root / "ask-trust").touch()
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            env = {"PATH": str(root / "bin") + ":" + os.environ["PATH"], "HOME": str(root),
                   "XDG_CONFIG_HOME": str(root / "config"), "XDG_DATA_HOME": str(root / "data"),
                   "XDG_RUNTIME_DIR": str(root / "runtime"), "CHELA_FIXTURE": str(root),
                   "LD_LIBRARY_PATH": str(BUILD / "test-keyring") + ":" + os.environ.get("LD_LIBRARY_PATH", ""),
                   **harness.env}
            subprocess.run([str(BUILD / "test_core"), "-p", "/credentials"], env={**env, "CHELA_TEST_KEYRING": "1"},
                           check=True, stdout=subprocess.DEVNULL)

            def start(*args):
                return subprocess.Popen([str(BUILD / "chela"), *args], env=env, cwd=root, stdout=log, stderr=log)

            def settings_text():
                return (config / "settings.ini").read_text()

            def login(shot=None):
                password = harness.window("^SSH login")
                if shot:
                    password.shot(shot)
                password.type("fixture-only")
                password.key("return")
                return password

            def field(w, current, value):
                w.tap(current)
                w.key("ctrl+a")
                w.type(value)

            def model_menu(w, item):
                w.key("ctrl+shift+l")
                w.wait_visible("^Manage connection…$")
                w.tap(item)

            app = start(str(root))
            w = harness.window("^Chela$")
            model_menu(w, "Manage connection…")
            w.wait_visible("^Connect & refresh library$")
            # The address has initial focus; username and folder retain sensible defaults.
            w.type("10.0.0.55")
            w.key("tab", "ctrl+a")
            w.type("brody")
            w.tap("Advanced")
            w.wait_visible("^Server port$")
            w.shot("ui-library-setup.png")
            field(w, "8088", str(port))  # server port, then local port
            field(w, "8088", str(port))
            w.tap("Automatic GPU + RAM")
            w.tap("MoE experts in RAM")
            w.do("scroll 720 470 600")  # the memory fields are below the fold
            texts = w.texts()
            context = texts[texts.index("Context tokens") + 1]
            headroom = texts[texts.index("GPU reserve (MiB)") + 1]
            field(w, context, "4096")
            field(w, headroom, "2048")
            w.shot("ui-library-memory-settings.png")
            w.tap("Connect & refresh library")
            trust = harness.window("^Trust inference computer")
            trust.wait_visible("ED25519 key fingerprint is SHA256:FIXTURE")
            trust.shot("ui-library-trust.png")
            trust.tap("Trust & connect")
            login("ui-library-login.png")
            wait_for(lambda: (root / "installed").exists(), what="the server install")
            wait_for(lambda: "provider=6" in settings_text(), what="the saved InferSSD connection")
            w.wait(lambda: not w.visible("^Connect & refresh library$"), what="setup to close")
            settings = settings_text()
            assert "ssh_host=10.0.0.55" in settings and "fixture-only" not in settings
            assert "ssh_enabled=true" in settings
            assert "ssh_remember=true" in settings
            assert len(list((root / "keyring").iterdir())) == 1
            assert "memory_mode=1" in settings and "context_size=4096" in settings and "gpu_headroom=2048" in settings, settings
            model_menu(w, "Second-Q6")
            wait_for(lambda: "model=Second-Q6" in settings_text(), what="the model switch")
            w.key("ctrl+shift+l")
            w.wait_visible("^First-Q8$")
            w.shot("ui-library-models.png")
            w.key("escape")
            w.key("ctrl+shift+t")
            w.wait_visible("^Quick$")
            w.shot("ui-thinking-options.png")
            w.tap("Quick")
            wait_for(lambda: "thinking=2" in settings_text(), what="the thinking setting")
            w.tap("Describe a task or ask a question…")
            w.type("try the selected model")
            w.key("return")
            wait_for(lambda: (root / "requests.jsonl").exists(), what="the request")
            request = json.loads((root / "requests.jsonl").read_text().splitlines()[-1])
            assert request["model"] == "Second-Q6"
            assert request["reasoning_effort"] == "low"
            assert request["chat_template_kwargs"]["enable_thinking"] is True
            assert request["thinking_budget_tokens"] == 256
            db = root / "data/chela/sessions.db"

            def saved_reply():
                with sqlite3.connect(db) as conn:
                    row = conn.execute("SELECT messages FROM sessions ORDER BY updated DESC LIMIT 1").fetchone()
                return row and json.loads(row[0])[-1].get("content") == "Reply from Second-Q6"
            wait_for(saved_reply, what="the saved reply")
            w.key("ctrl+shift+l")
            w.wait_visible("^Test selected model$")
            w.shot("ui-library-loaded.png")
            w.tap("Test selected model")
            wait_for(lambda: len((root / "requests.jsonl").read_text().splitlines()) == 2, what="the model test")
            w.wait_visible("Reply from Second-Q6")
            w.shot("ui-model-test-result.png")
            w.key("escape")
            with sqlite3.connect(db) as conn:
                messages = json.loads(conn.execute("SELECT messages FROM sessions ORDER BY updated DESC LIMIT 1").fetchone()[0])
            assert len(messages) == 3, messages  # The diagnostic never enters conversation history.
            # Close while a request is still running, then let its callback finish.
            (root / "slow-test").touch()
            model_menu(w, "Test selected model")
            wait_for(lambda: len((root / "requests.jsonl").read_text().splitlines()) == 3, what="the slow test")
            w.key("escape")
            (root / "slow-test").unlink()
            time.sleep(1.1)
            w.do("idle")
            # Long model/project labels plus the new control must fit a narrow window.
            w.do("size 980 720")
            w.shot("ui-thinking-narrow.png")
            w.key("ctrl+q")
            assert app.wait(timeout=10) == 0, (BUILD / "model-library-ui.log").read_text()

            # A cached password reconnects without a login prompt.
            (root / "tunnel-started").unlink()
            app = start(str(root))
            w = harness.window("^Chela$")
            wait_for(lambda: (root / "tunnel-started").exists(), what="the tunnel")
            wait_for(lambda: len((root / "authenticated").read_text().splitlines()) >= 2, what="the cached login")
            w.do("idle")
            assert harness.no_window("^SSH login"), "A cached password should reconnect without a login dialog"
            assert "model=Second-Q6" in settings_text() and "thinking=2" in settings_text()
            w.key("ctrl+q")
            assert app.wait(timeout=10) == 0
            # A rejected cached password must be forgotten and prompt for a replacement.
            next((root / "keyring").iterdir()).write_text("fixture-stale")
            app = start(str(root))
            w = harness.window("^Chela$")
            login()
            wait_for(lambda: len((root / "authenticated").read_text().splitlines()) >= 3, what="the replaced login")
            wait_for(lambda: next((root / "keyring").iterdir()).read_text() == "fixture-only", what="the new password")
            # Settings → Connections is the same setup; saving Code settings
            # must retain the existing login rather than prompt again.
            w.do("idle")
            started = (root / "tunnel-started").stat().st_mtime_ns
            w.key("ctrl+comma")
            w.wait_visible("^Appearance$")
            w.key("ctrl+2")
            w.wait_visible("^Connect & refresh library$")
            assert any(text == "10.0.0.55" for text in w.texts()), w.texts()
            w.key("ctrl+3")
            w.tap("Save settings")
            w.wait(lambda: not w.visible("^Save settings$"), what="settings to close")
            time.sleep(.5)
            assert harness.no_window("^SSH login")
            assert (root / "tunnel-started").stat().st_mtime_ns == started
            # Cancelling a refreshed setup and its SSH prompt must keep saved settings.
            before = settings_text()
            model_menu(w, "Manage connection…")
            w.wait_visible("^Connect & refresh library$")
            w.tap("Forget saved SSH login")
            wait_for(lambda: not list((root / "keyring").iterdir()), what="the forgotten login")
            w.tap("Connect & refresh library")
            # Cancelling authentication returns control promptly and allows a retry.
            password = harness.window("^SSH login")
            password.key("escape")
            wait_for(password.gone, what="the cancelled SSH prompt to close")
            w.wait_visible("^Connect & refresh library$")
            time.sleep(.3)
            w.tap("Connect & refresh library")
            password = harness.window("^SSH login")
            w.key("escape")
            w.wait(lambda: not w.visible("^Connect & refresh library$"), what="setup to close")
            wait_for(password.gone, what="cancelling setup to close its SSH prompt")
            assert settings_text() == before
            w.key("ctrl+q")
            assert app.wait(timeout=10) == 0, (BUILD / "model-library-ui.log").read_text()
            # Direct setup launch shows the form before starting authentication.
            app = start("--setup-models", "10.0.0.55")
            w = harness.window("^Chela$")
            w.wait_visible("^Connect & refresh library$")
            assert any(text == "10.0.0.55" for text in w.texts()), w.texts()
            assert harness.no_window("^SSH login")
            w.shot("ui-library-direct-setup.png")
            w.key("escape")
            harness.window("^SSH login").key("escape")
            w.do("idle")
            w.key("ctrl+q")
            assert app.wait(timeout=10) == 0, (BUILD / "model-library-ui.log").read_text()
            print("Model library desktop test passed: setup, host trust, keyring credential isolation, remembered reconnect without a prompt, stale-password replacement, forgetting a saved login, discovery/selection, thinking persistence, inference diagnostic/cancellation, narrow layout, and cancellation.")
    finally:
        if app and app.poll() is None:
            app.terminate()
            app.wait(timeout=5)
        if harness:
            harness.close()
        log.close()


if __name__ == "__main__":
    main()
