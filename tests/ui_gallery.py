"""Screenshots of every main screen, for reviewing the interface against its references.

Runs build/chela with the headless backend against a local model server that replies
with rich markdown, and writes build/gallery/*.png. Nothing leaves the machine.
    python3 tests/ui_gallery.py [name-filter]
"""
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import threading
import time
import zlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get("CHELA_BINARY", ROOT / "build/chela"))
OUT = ROOT / "build/gallery"
RICH = """# Heading one

A paragraph that wraps across several lines to check the measure and line height of body text. It has **bold**, *italic*, ***both***, ~~struck~~, `inline code`, and a [link to the docs](https://example.test/docs).

## Heading two

Line one of a soft-wrapped paragraph
continues here on the same paragraph.

### Lists

- First item with `code`
- Second item
  - Nested item
    - Deeply nested item
- [ ] An open task
- [x] A finished task

1. Numbered one
2. Numbered two
   - A child under two
3. Numbered three

```python
def greet(name: str) -> str:
    # A short function
    return f"Hello, {name}!"
```

> A quote that says something worth keeping,
> over two lines.
>
> > And a nested reply.

| Model | Size | Speed |
|:------|-----:|:-----:|
| Qwen3.8 Flash | 58.4 GiB | 21 tok/s |
| OpenAI-20B | 12 GiB | **64 tok/s** |

---

```
plain block without a language
```

Done. 👋
"""
STRESS = """A very long URL in text: https://example.test/a/really/long/path/that/keeps/going/and/going/without/any/spaces/at/all/until/it/must/break/somewhere/in/the/line?with=query&and=more

`a_very_long_inline_code_identifier_that_is_wider_than_a_narrow_window_could_possibly_show_on_one_line_without_wrapping`

| Column one | Column two | Column three | Column four | Column five | Column six | Column seven | Column eight |
|---|---|---|---|---|---|---|---|
| Some cell text that is long | More text in this cell here | Short | Another fairly long value | 12345678901234567890 | x | y | z |
| [link](https://example.test) | `code` | **bold** | *it* | | | | last |

```text
a line of code that is much much much longer than the column is wide so that it has to wrap somewhere rather than overflow the box
```

1. An item with a long sentence that wraps across several lines in a narrow window so the hanging indent can be checked properly here.
   ```sh
   make test
   ```
   And a paragraph that continues the same item after its code.

   - Nested bullet with a long sentence that wraps across several lines in a narrow window to check nested hanging indents.

> A quote with a long sentence that wraps across several lines in a narrow window so its bar runs the whole height.

**Bold at the start.** Then `code`, then *italics*, then ~~gone~~, then [a link](https://example.test/x), all on a single line that wraps.
"""


class Server(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send_json(self, data):
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        self.send_json({"data": [{"id": "Qwen3.8-Flash-Next-GSQ-RCO-IQ1_M-ccb740ad"},
                                 {"id": "OpenAI-20B-NEO-CODE-DI-Uncensored-Q8_0-ecda383c"},
                                 {"id": "gemma-4-26B-A4B-heretic-APEX-I-Quality-fb467308"}]})

    def do_POST(self):
        data = json.loads(self.rfile.read(int(self.headers["Content-Length"])))
        last = data["messages"][-1]
        content = last.get("content")
        text = content if isinstance(content, str) else " ".join(p.get("text", "") for p in content)
        if last.get("role") == "tool":
            reply = {"content": "I checked the project. Everything looks **fine**."}
        elif "tools" in text:
            calls = [{"id": f"c{i}", "type": "function", "function": {"name": name, "arguments": json.dumps(args)}}
                     for i, (name, args) in enumerate([("list_files", {"path": "."}), ("read_file", {"path": "src/a.txt"})])]
            reply = {"tool_calls": calls}
        elif "stress" in text:
            reply = {"content": STRESS}
        elif "hello" in text:
            reply = {"content": "Hello! 👋 How can I help you today?"}
        else:
            reply = {"content": RICH}
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        chunk = {"choices": [{"delta": reply, "finish_reason": None}]}
        done = {"choices": [{"delta": {}, "finish_reason": "stop"}],
                "timings": {"predicted_n": 49, "prompt_n": 900, "predicted_per_second": 20.4}}
        self.wfile.write(("data: " + json.dumps(chunk) + "\n\n" + "data: " + json.dumps(done) + "\n\ndata: [DONE]\n\n").encode())


def png(path, width, height, rgb):
    raw = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    def part(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + part(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)) +
                     part(b"IDAT", zlib.compress(raw)) + part(b"IEND", b""))


class Gallery:
    def __init__(self, temp, port):
        self.temp = temp
        self.project = temp / "Chela"
        (self.project / "src").mkdir(parents=True)
        (self.project / "src/a.txt").write_text("contents\n")
        subprocess.run("git init -q . && git add . && git -c user.email=t@t -c user.name=t commit -qm x && echo more >> src/a.txt",
                       shell=True, cwd=self.project, check=True)
        self.config = temp / "config/chela"
        self.config.mkdir(parents=True)
        (self.config / "settings.ini").write_text(f"[connection]\nendpoint=http://127.0.0.1:{port}/v1\n"
                                                  "model=Qwen3.8-Flash-Next-GSQ-RCO-IQ1_M-ccb740ad\nprovider=0\ntools=true\nthinking=2\n")
        (self.config / "desktop.ini").write_text("[sidebar]\ngroup=none\n")
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(temp), "LANG": "C.UTF-8",
                    "XDG_CONFIG_HOME": str(temp / "config"), "XDG_DATA_HOME": str(temp / "data"),
                    "XDG_RUNTIME_DIR": str(temp), "XDG_CURRENT_DESKTOP": "KDE", "CHELA_HEADLESS_CSD": "1",
                    **{k: v for k, v in os.environ.items() if k.endswith("SAN_OPTIONS")}}
        png(temp / "photo.png", 640, 400, (70, 120, 170))
        png(temp / "tall.png", 200, 500, (170, 110, 80))
        (temp / "notes.md").write_text("# Notes\n")
        (temp / "tool.bin").write_bytes(b"\xff\x00\x01")

    def run(self, name, script, size=(1440, 940)):
        path = self.temp / f"{name}.script"
        text = f"size {size[0]} {size[1]}\nidle\n" + "\n".join(
            ("shot " + str(OUT / line.strip()[5:]) if line.strip().startswith("shot ") else line.strip())
            for line in script.strip().splitlines()) + "\nquit\n"
        path.write_text(text)
        result = subprocess.run([str(BINARY), "--backend=headless", "--script", str(path), str(self.project)],
                                cwd=self.temp, env=self.env, capture_output=True, text=True, timeout=120)
        if result.returncode:
            print(f"{name} failed:\n{result.stderr}", file=sys.stderr)
            sys.exit(1)


SCENES = {
    "welcome": ("""
        shot code-welcome.png
        key ctrl+1
        idle
        shot chat-welcome.png
    """, (1440, 940)),
    "chat": ("""
        key ctrl+1
        type hello world
        key return
        until Hello!
        idle
        shot chat-hello.png
        type show me everything
        key return
        until plain block without
        idle
        shot chat-rich.png
    """, (1440, 940)),
    "code": ("""
        type show me everything
        key return
        until plain block without
        idle
        shot code-rich.png
        type run the tools
        key return
        until looks
        idle
        shot code-tools.png
        tap Read 1 file
        idle
        shot code-tools-open.png
    """, (1440, 940)),
    "attach": ("""
        drag on
        idle
        shot attach-drag.png
        drag off
        drop-files {temp}/photo.png {temp}/tall.png {temp}/notes.md {temp}/tool.bin
        wait 500
        idle
        type hello, what is in these?
        shot attach-composer.png
        key return
        until How can I help
        idle
        shot attach-sent.png
    """, (1440, 940)),
    "settings": ("""
        key ctrl+comma
        idle
        shot settings-general.png
        key ctrl+3
        idle
        shot settings-models.png
        key ctrl+2
        idle
        shot settings-connections.png
        key escape
        key ctrl+shift+a
        idle
        shot menu-attach.png
        key escape
        key ctrl+shift+l
        wait 300
        idle
        shot menu-model.png
    """, (1440, 940)),
    "picker": ("""
        key ctrl+shift+l
        until Effort
        idle
        shot picker-model.png
        tap Effort
        until Balanced
        idle
        shot picker-effort.png
        key escape
        key escape
        key ctrl+m
        until Accept edits
        idle
        shot picker-permissions.png
        tap Bypass permissions
        idle
        shot picker-bypass-confirm.png
        key escape
        key escape
        key ctrl+shift+s
        idle
        shot picker-status.png
    """, (1440, 940)),
    "sidebar": ("""
        type hello one
        key return
        until Hello!
        idle
        key ctrl+n
        type hello two
        key return
        until-not Connecting
        idle
        shot sidebar-two.png
        rtap hello one
        idle
        shot sidebar-row-menu.png
        key escape
        rtap Chela
        idle
        shot sidebar-group-menu.png
        tap Select all
        idle
        rtap hello one
        idle
        shot sidebar-selection-menu.png
    """, (1440, 940)),
    "stress": ("""
        key ctrl+1
        type stress test
        key return
        until single line
        idle
        shot stress-wide.png
        size 760 900
        idle
        scroll 400 400 -4000
        idle
        shot stress-narrow.png
    """, (1440, 940)),
    "narrow": ("""
        type hello there
        key return
        until Hello!
        idle
        shot narrow.png
    """, (900, 640)),
}


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    wanted = sys.argv[1] if len(sys.argv) > 1 else ""
    server = ThreadingHTTPServer(("127.0.0.1", 0), Server)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for name, (script, size) in SCENES.items():
            if wanted and wanted not in name:
                continue
            with tempfile.TemporaryDirectory(prefix="chela-gallery-") as folder:
                temp = Path(folder)
                gallery = Gallery(temp, server.server_port)
                gallery.run(name, script.replace("{temp}", str(temp)), size)
    finally:
        server.shutdown()
    print("Wrote", OUT)


if __name__ == "__main__":
    main()
