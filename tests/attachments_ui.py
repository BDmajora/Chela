"""Attachments and links through the real desktop, with the headless backend: pasting and
dropping, what a model server receives, limits and damaged files, and clean-up."""
import base64
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from ui_gallery import ROOT, png

BUILD = ROOT / "build"
BINARY = Path(os.environ.get("CHELA_BINARY", BUILD / "chela"))
OUT = BUILD / "attachments"
requests = []


class Server(BaseHTTPRequestHandler):
    vision = None  # None: the server says nothing about images

    def log_message(self, *_):
        pass

    def do_GET(self):
        entry = {"id": "vision"}
        if self.vision is not None:
            entry["status"] = {"value": "loaded", "args": ["server"] + (["--mmproj", "vision.gguf"] if self.vision else [])}
        body = json.dumps({"data": [entry]}).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        requests.append(data)
        text = "Seen. See [the docs](https://example.test/docs_page) for more."
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        chunk = {"choices": [{"delta": {"content": text}, "finish_reason": "stop"}]}
        self.wfile.write(("data: " + json.dumps(chunk) + "\n\ndata: [DONE]\n\n").encode())


class Desktop:
    def __init__(self, temp, port):
        self.temp = temp
        self.project = temp / "project"
        self.project.mkdir()
        config = temp / "config/chela"
        config.mkdir(parents=True)
        (config / "settings.ini").write_text(f"[connection]\nendpoint=http://127.0.0.1:{port}/v1\nmodel=vision\nprovider=0\ntools=false\n")
        (config / "desktop.ini").write_text("[sidebar]\ngroup=none\n")
        tools = temp / "bin"
        tools.mkdir()
        opener = tools / "xdg-open"
        opener.write_text(f"#!/bin/sh\necho \"$1\" >> '{temp}/opened'\n")
        opener.chmod(0o700)
        self.env = {"PATH": f"{tools}:{os.environ.get('PATH', '/usr/bin:/bin')}", "HOME": str(temp), "LANG": "C.UTF-8",
                    "XDG_CONFIG_HOME": str(temp / "config"), "XDG_DATA_HOME": str(temp / "data"), "XDG_RUNTIME_DIR": str(temp),
                    **{k: v for k, v in os.environ.items() if k.endswith("SAN_OPTIONS")}}
        self.store = temp / "data/chela/attachments"

    def run(self, name, script):
        OUT.mkdir(parents=True, exist_ok=True)
        lines = []
        for line in script.strip().splitlines():
            line = line.strip()
            lines.append("shot " + str(OUT / line[5:]) if line.startswith("shot ") else line)
        path = self.temp / f"{name}.script"
        path.write_text("size 1440 940\nidle\n" + "\n".join(lines) + "\nquit\n")
        result = subprocess.run([str(BINARY), "--backend=headless", "--script", str(path), str(self.project)], cwd=self.temp,
                                env=self.env, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, f"{name}: {result.stderr[:2500]}"

    def sessions(self):
        with sqlite3.connect(self.temp / "data/chela/sessions.db") as db:
            return [(title, json.loads(messages)) for title, messages in
                    db.execute("SELECT title, messages FROM sessions ORDER BY updated DESC")]


def main():
    server = ThreadingHTTPServer(("127.0.0.1", 0), Server)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="chela-attach-") as folder:
            temp = Path(folder)
            d = Desktop(temp, server.server_port)
            png(temp / "photo.png", 320, 200, (30, 110, 190))
            (temp / "notes.md").write_text("# Notes\nfirst line\n")
            (temp / "empty.txt").write_text("")
            (temp / "broken.png").write_bytes(b"\x89PNG\r\n\x1a\n" + b"not really")
            (temp / "huge.txt").write_text("x" * (257 * 1024))
            many = " ".join(str(temp / f"f{i}.txt") for i in range(18))
            for i in range(18):
                (temp / f"f{i}.txt").write_text(f"file {i}\n")

            # A pasted screenshot and a pasted file reach the model, and the transcript shows them.
            d.run("paste", f"""
                copy-data image/png {temp}/photo.png
                key ctrl+v
                wait 400
                idle
                shot pasted.png
                type what is this?
                key return
                until the docs
                until Reply
                shot sent.png
                tap the docs
                wait 200
                drop-files {temp}/notes.md
                wait 300
                idle
                expect notes.md
                type and this file
                key return
                until Reply
            """)
            first, second = requests[-2:]
            parts = first["messages"][-1]["content"]
            assert parts[0] == {"type": "text", "text": "what is this?"}, parts
            url = parts[1]["image_url"]["url"]
            assert url.startswith("data:image/png;base64,") and \
                base64.b64decode(url.split(",", 1)[1]) == (temp / "photo.png").read_bytes(), url[:60]
            assert all(not key.startswith("_chela") for m in first["messages"] for key in m), first["messages"]
            assert "Attached file: notes.md\n```\n# Notes\nfirst line\n\n```" in second["messages"][-1]["content"], second["messages"][-1]
            assert isinstance(second["messages"][1]["content"], list), "Earlier images stay in the conversation"
            assert (temp / "opened").read_text().strip() == "https://example.test/docs_page", "Links open in the browser"
            title, messages = d.sessions()[0]
            assert title == "what is this?", title
            shown = [m for m in messages if m["role"] == "user"]
            assert shown[0]["content"] == "what is this?" and shown[0]["_chela_attachments"][0]["kind"] == "image", shown[0]
            assert len(json.dumps(messages)) < 4000, "Pictures stay out of the session database"

            # A restart shows the picture again, and a retried message keeps it.
            sent_before = len(requests)
            d.run("restart", """
                key ctrl+k
                type what is this
                idle
                key return
                until Seen
                shot restored.png
                tap and this file
                expect Reply
            """)
            assert len(requests) == sent_before

            # What cannot be attached says why, and leaves the draft alone.
            d.run("errors", f"""
                key ctrl+n
                type my draft
                drop-files {temp}/empty.txt
                wait 300
                until is empty
                drop-files {temp}/broken.png
                wait 300
                until damaged
                shot refused.png
                drop-files {temp}/huge.txt
                wait 300
                until 256 KiB
                drop-files /nonexistent/file.txt
                wait 300
                until Cannot read
                drop text/uri-list {temp}/notes.md
                wait 200
                until Only files on this computer
                expect my draft
                key ctrl+n
                drop-files {many}
                wait 800
                until up to 16
                shot limit.png
                expect f15.txt
                expect-not f16.txt
            """)

            # A file can be removed before sending, and a message can be attachments alone.
            sent_before = len(requests)
            d.run("remove", f"""
                key ctrl+n
                drop-files {temp}/notes.md
                wait 300
                expect notes.md
                move 560 820
                idle
                shot hover.png
                click 589 794
                idle
                expect-not notes.md
                key return
                drop-files {temp}/photo.png
                wait 400
                idle
                key return
                until Seen
                until Reply
            """)
            assert len(requests) == sent_before + 1, "Nothing was left to send after removing the only file"
            assert d.sessions()[0][0] == "photo.png", d.sessions()[0][0]
            content = requests[-1]["messages"][-1]["content"]
            assert len(content) == 1 and content[0]["type"] == "image_url", content

            # Deleting a session removes its files, unless another session shows the same one.
            # A model without vision says so before anything is sent, and the draft keeps the picture.
            Server.vision = False
            sent_before = len(requests)
            d.run("no-vision", f"""
                key ctrl+n
                copy-data image/png {temp}/photo.png
                key ctrl+v
                until can't read images yet
                wait 200
                idle
                expect Start the server with its vision file
                shot no-vision.png
                type describe it
                key return
                wait 400
                idle
                expect can't read images yet
                expect describe it
            """)
            assert len(requests) == sent_before, "A request the server would refuse was sent"

            # An earlier image does not stop such a model answering the rest of the session.
            d.run("no-vision-history", """
                key ctrl+k
                type what is this
                idle
                key return
                until Seen
                wait 400
                idle
                type keep going
                key return
                until-not Working…
                until Seen
            """)
            assert len(requests) == sent_before + 1, requests[sent_before:]
            sent = requests[-1]["messages"]
            first = next(m for m in sent if m["role"] == "user")
            assert isinstance(first["content"], str) and "Attached image Pasted image was left out" in first["content"], first
            assert sent[-1]["content"] == "keep going", sent[-1]
            Server.vision = True  # with its vision file in place the same conversation has its picture back
            d.run("vision-restored", """
                key ctrl+k
                type what is this
                idle
                key return
                until Seen
                wait 400
                idle
                type look again
                key return
                until-not Working…
                until Seen
            """)
            first = next(m for m in requests[-1]["messages"] if m["role"] == "user")
            assert isinstance(first["content"], list) and first["content"][-1]["type"] == "image_url", str(first)[:200]
            Server.vision = None

            stored = sorted(p.name for p in d.store.iterdir())
            d.run("delete", """
                rtap photo.png
                tap Delete…
                until Delete this session
                tap Delete
                until-not Delete this session
            """)
            assert sorted(p.name for p in d.store.iterdir()) == stored, "what is this? still shows the picture"
            d.run("delete-last", """
                key ctrl+k
                type what is this
                idle
                key return
                until Seen
                rtap what is this?
                tap Delete…
                until Delete this session
                tap Delete
                until-not Delete this session
            """)
            left = [p for p in d.store.iterdir() if p.suffix == ".png"]
            assert not left, left
    finally:
        server.shutdown()
    print("Attachment desktop test passed: paste and drop, images and files reaching the model, restored transcripts, "
          "links, refused files, the attachment limit, removing, attachment-only messages, models without vision and clean-up")


if __name__ == "__main__":
    main()
