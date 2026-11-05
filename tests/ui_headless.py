"""Drive the real desktop with the headless backend: no display, toolkit or input tools.

Each phase runs build/chela --backend=headless with a script of keyboard and
pointer input (see src/platform/headless.c) against isolated settings, a
throwaway project and the loopback model server from integration.py, then checks
what persisted: SQLite sessions, settings files and project files. Later phases
restart the app to check that state survives. Screenshots are saved in build/.
No user settings are touched and nothing leaves the machine.
"""
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
from http.server import ThreadingHTTPServer

from integration import Handler, ROOT, PROPOSED_FILE
from visual_geometry import measure_frame

BUILD = ROOT / "build"
BINARY = Path(os.environ.get("CHELA_BINARY", BUILD / "chela"))


def executable(name):
    result = shutil.which(name)
    if not result:
        raise RuntimeError(f"Install {name} (ImageMagick) to measure screenshots")
    return result


class Desktop:
    def __init__(self, temp, project, port):
        self.temp, self.project = temp, project
        self.config = temp / "config/chela"
        self.db = temp / "data/chela/sessions.db"
        self.config.mkdir(parents=True)
        (self.config / "settings.ini").write_text(
            f"[connection]\nendpoint=http://127.0.0.1:{port}/v1\nmodel=stream\nprovider=0\ntools=true\n")
        transcription = temp / "transcribe.py"
        transcription.write_text("print('second request')\n")
        (self.config / "desktop.ini").write_text(
            f"[sidebar]\ngroup=none\n[voice]\ndictation_command={sys.executable} {transcription}\n")
        self.env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "HOME": str(temp), "LANG": "C.UTF-8",
                    "XDG_CONFIG_HOME": str(temp / "config"), "XDG_DATA_HOME": str(temp / "data"),
                    "XDG_RUNTIME_DIR": str(temp),
                    **{k: v for k, v in os.environ.items() if k.endswith("SAN_OPTIONS")}}
        self.phase = 0

    def run(self, name, script):
        """Runs one app session; `shot NAME` lines save build/NAME."""
        self.phase += 1
        lines = []
        for line in script.strip().splitlines():
            line = line.strip()
            if line.startswith("shot "):
                line = "shot " + str(BUILD / line[5:])
            lines.append(line)
        path = self.temp / f"{self.phase:02d}-{name}.script"
        path.write_text("size 1440 940\nidle\n" + "\n".join(lines) + "\n")
        with open(BUILD / "ui-test.log", "a") as log:
            log.write(f"== phase {self.phase}: {name}\n")
            log.flush()
            result = subprocess.run([str(BINARY), "--backend=headless", "--script", str(path), str(self.project)],
                                    cwd=self.temp, env=self.env, stdout=log, stderr=subprocess.PIPE, text=True,
                                    timeout=180)
            log.write(result.stderr)
        assert result.returncode == 0, f"phase {name} failed:\n{result.stderr}"

    def sessions(self):
        with sqlite3.connect(self.db) as conn:
            rows = conn.execute("SELECT id, title, messages FROM sessions ORDER BY updated DESC").fetchall()
        return [(id, title, json.loads(messages)) for id, title, messages in rows]

    def preferences(self):
        return (self.config / "desktop.ini").read_text()

    def settings(self):
        return (self.config / "settings.ini").read_text()


def last(messages, role):
    return next((m.get("content") for m in reversed(messages) if m.get("role") == role), None)


def main():
    BUILD.mkdir(exist_ok=True)
    (BUILD / "ui-test.log").write_text("")
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="chela-ui-") as temp:
            temp = Path(temp)
            project = temp / "sample-project"
            (project / "src").mkdir(parents=True)
            (project / "src/example.txt").write_text("workspace file contents")
            skill = project / ".claude/skills/review-style"
            skill.mkdir(parents=True)
            (skill / "SKILL.md").write_text("Use short, concrete sentences for this review.")
            desktop = Desktop(temp, project, server.server_port)
            tour(desktop)
            conversation(desktop)
            sidebar(desktop)
            settings(desktop)
            tools(desktop)
            recovery(desktop)
            restart(desktop)
            geometry = {}
            for capture in ["ui-welcome.png", "ui-conversation.png", "ui-chat-rich-narrow.png", "ui-narrow.png",
                            "ui-sidebar-hidden.png"]:
                geometry[capture] = measure_frame(BUILD / capture, executable, sidebar=capture != "ui-sidebar-hidden.png",
                                                  panel=262 if capture == "ui-conversation.png" else 0)
            (BUILD / "visual-geometry.json").write_text(json.dumps(geometry, indent=2) + "\n")
    finally:
        server.shutdown()
    print("Headless desktop test passed: frame geometry, menus and nested submenus by keyboard, model discovery and "
          "switching, permission modes, send, dictation, rename, pin/unpin, sections, grouping and sorting, "
          "search and restore, settings, local skills, workspace Chat tools, approval and denial, connection recovery "
          "and retry, cancel, quit and restart")


def tour(d):
    """Welcome screens, every composer menu and nested submenus, window sizes."""
    gets = Handler.gets
    d.run("tour", """
        expect Open folder…
        shot ui-welcome.png
        key ctrl+m
        expect Bypass permissions
        shot ui-menu-permissions.png
        key home
        key down
        key down
        key return
        expect-not Bypass permissions
        expect Accept edits
        key ctrl+m
        key home
        key return
        expect Auto
        expect-not Manual
        key ctrl+m
        key home
        key down
        key return
        expect Manual
        key ctrl+shift+l
        expect Effort
        expect-not Pre-load model
        expect-not Test selected model
        expect-not Server diagnostics
        expect-not Model settings
        shot ui-menu-model.png
        key end
        key left
        expect Balanced
        shot ui-menu-effort.png
        key right
        expect-not Balanced
        key return
        expect Balanced
        key down
        key down
        key return
        expect-not Balanced
        expect Quick
        key ctrl+shift+l
        expect Effort
        key end
        key left
        key return
        expect-not Quick
        key ctrl+shift+l
        until tool
        hover Effort
        expect Balanced
        hover tool
        expect Balanced
        hover stream
        expect-not Balanced
        key escape
        key ctrl+shift+a
        expect Add files or photos
        shot ui-menu-attach.png
        key home
        key down
        key down
        key right
        expect review-style
        shot ui-menu-skills.png
        key left
        key escape
        expect-not Add files or photos
        key ctrl+shift+v
        shot ui-voice-settings.png
        key escape
        key ctrl+shift+p
        shot ui-menu-projects.png
        key escape
        key ctrl+shift+e
        shot ui-menu-environment.png
        key escape
        key ctrl+shift+l
        until tool
        shot ui-menu-models.png
        tap tool
        expect-not Add files or photos
        key ctrl+shift+l
        until stream
        tap stream
        key ctrl+shift+u
        shot ui-menu-workspace.png
        key escape
        key ctrl+1
        expect How can I help you today?
        shot ui-chat-welcome.png
        key ctrl+shift+a
        shot ui-chat-menu-attach.png
        key escape
        size 980 720
        shot ui-chat-narrow.png
        key ctrl+2
        shot ui-narrow.png
        key ctrl+b
        shot ui-sidebar-hidden.png
        key ctrl+b
        size 1440 940
        idle
    """)
    assert Handler.gets > gets, "Opening the model picker must discover models"
    assert "model=stream" in d.settings()


def conversation(d):
    """Send in Accept edits mode, regenerate, dictate a second session."""
    requests = len(Handler.requests)
    d.run("conversation", """
        key ctrl+n
        key ctrl+m
        key home
        key down
        key down
        key return
        type test request
        key return
        until Reply
        expect Hello 🦀
        key ctrl+m
        key home
        key down
        key return
        shot ui-conversation.png
        key return
        log An empty prompt must not send
        idle
        key ctrl+n
        key ctrl+d
        until second request
        key return
        until Reply
        expect Hello 🦀
        key ctrl+n
        idle
        shot ui-home-sessions.png
    """)
    sessions = d.sessions()
    assert [s[1] for s in sessions] == ["second request", "test request"], sessions
    for _, _, messages in sessions:
        assert last(messages, "assistant") == "Hello 🦀"
        assert sum(m.get("role") == "user" for m in messages) == 1
    assert len(Handler.requests) == requests + 2, "Empty prompts must not send requests"
    with sqlite3.connect(d.db) as conn:
        modes = dict(conn.execute("SELECT s.title, o.mode FROM sessions s JOIN session_options o ON o.id = s.id"))
    assert modes["test request"] == 0 and modes["second request"] == 0, modes


def sidebar(d):
    """Pin, rename, sections, grouping and sorting, search and restore."""
    first = next(s[0] for s in d.sessions() if s[1] == "test request")
    d.run("sidebar", """
        rtap test request
        expect Pin
        shot ui-menu-session-context.png
        tap Pin
        expect Pinned
        shot ui-pinned-session.png
        rtap test request
        tap Unpin
        rtap test request
        tap Rename…
        expect Rename session
        key ctrl+a
        type test request renamed
        shot ui-rename.png
        key return
        until-not Rename session
        expect test request renamed
        tap More
        tap Group and sort
        expect Group by
        key escape
        key escape
        expect-not Keyboard shortcuts
        key ctrl+shift+f
        shot ui-code-organizer.png
        key end
        key return
        expect All sessions
        shot ui-all-sessions.png
        key escape
        key ctrl+shift+f
        key home
        key right
        key down
        key return
        idle
        shot ui-grouped-project.png
        key ctrl+shift+f
        key home
        key down
        key right
        key end
        key return
        idle
        shot ui-sorted-titles.png
        rtap second request
        tap Move to
        tap New section…
        expect New section
        type Reviews
        key return
        until Reviews
        shot ui-custom-section.png
        key ctrl+shift+f
        key home
        key right
        shot ui-code-group.png
        key escape
        key escape
        key ctrl+k
        type renamed
        idle
        key return
        until Hello 🦀
        expect test request renamed
        type follow-up
        key return
        until Reply
    """)
    prefs = d.preferences()
    assert "group=project" in prefs and "sort_titles=true" in prefs, prefs
    second = next(s[0] for s in d.sessions() if s[1] == "second request")
    assert f"pinned={first}" not in prefs, prefs
    assert f"{second}=Reviews" in prefs, prefs
    titles = {s[1]: s for s in d.sessions()}
    assert "test request renamed" in titles, titles
    history = titles["test request renamed"][2]
    assert last(history, "user") == "follow-up"
    assert any(m.get("content") == "test request" for m in Handler.requests[-1]["messages"]), \
        "Restored history must reach the model"


def settings(d):
    """Settings search and sections, a Medium transcript renders 896px wide, a project skill."""
    d.run("settings", """
        key ctrl+shift+f
        key home
        key right
        key end
        key return
        key ctrl+shift+f
        key home
        key down
        key right
        key home
        key return
        key ctrl+comma
        expect Appearance
        shot ui-settings-general.png
        key ctrl+f
        type model
        idle
        shot ui-settings-search.png
        key ctrl+3
        idle
        shot ui-settings-models.png
        key ctrl+1
        tap Narrow
        tap Medium
        key escape
        until-not Appearance
        key ctrl+n
        idle
        shot ui-medium-transcript.png
        key ctrl+shift+a
        key home
        key down
        key down
        key right
        tap review-style
        type review this
        key return
        until Reply
        key ctrl+1
        type visual review conversation
        key return
        until A quieter workspace
        shot ui-chat-rich.png
        size 980 720
        idle
        shot ui-chat-rich-narrow.png
        key ctrl+2
        shot ui-code-rich-narrow.png
        size 1440 940
        idle
        shot ui-code-rich.png
    """)
    prefs = d.preferences()
    assert "group=none" in prefs and "sort_titles=false" in prefs, prefs
    assert "transcript_width=1" in prefs, prefs
    measure_frame(BUILD / "ui-medium-transcript.png", executable, column_limit=896)
    (d.config / "desktop.ini").write_text(prefs.replace("transcript_width=1", "transcript_width=0"))
    skill = next(r for r in Handler.requests if "review this" in (r["messages"][-1].get("content") or ""))
    assert skill["messages"][-1]["content"].startswith("review this"), skill["messages"][-1]
    assert "Use short, concrete sentences" in skill["messages"][-1]["content"], skill["messages"][-1]
    assert any(title == "review this" for _, title, _ in d.sessions()), "Attached skills must not become the title"


def tools(d):
    """Workspace Chat reads files; Agent asks before writing, and a denial changes nothing."""
    d.run("tools", """
        key ctrl+n
        key ctrl+shift+m
        type inspect this project
        key return
        until Read workspace file contents
        shot ui-code-chat.png
        key ctrl+n
        key ctrl+shift+m
        type write a test file, first proposal
        key return
        until Allow once
        key escape
        until Hello 🦀
        type write a test file, approved proposal
        key return
        until Allow once
        shot ui-approval.png
        tap Allow once
        until-not Allow once
        until Reply
    """)
    chat = next(r for r in Handler.requests if last(r["messages"], "user") == "inspect this project")
    assert {t["function"]["name"] for t in chat["tools"]} == {"list_files", "read_file"}
    assert (d.project / "approved.txt").read_text() == PROPOSED_FILE


def recovery(d):
    """A failed request offers settings and a retry that keeps one user turn; Escape stops a request."""
    d.run("recovery", """
        type connection error
        key return
        until Retry message
        shot ui-connection-error.png
        type unsent draft
        tap Retry message
        until-not Retry message
        until Hello 🦀
        tap unsent draft
        key return
        until Reply
        type slow request
        key return
        wait 300
        key escape
        until Reply
        shot ui-final.png
        key ctrl+q
    """)
    history = d.sessions()[0][2]
    assert sum(m.get("content") == "connection error" for m in history) == 1, history
    assert any(m.get("content") == "unsent draft" for m in history)
    assert last(history, "user") == "slow request" and history[-1]["role"] == "user", \
        "A cancelled final turn remains a user message"


def restart(d):
    """Sessions and preferences survive a restart, and a restored Chat keeps its tools."""
    count = len(d.sessions())
    d.run("restart", """
        expect test request renamed
        expect Reviews
        key ctrl+k
        type inspect this
        idle
        key return
        until Read workspace file contents
        type inspect this project after restart
        key return
        until Reply
    """)
    assert len(d.sessions()) == count
    assert len(Handler.requests[-1]["tools"]) == 2, "A restored Chat session keeps read-only tools"


if __name__ == "__main__":
    main()
