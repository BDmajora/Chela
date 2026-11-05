"""Paste, drop, wheel and text selection through the real Wayland backend, in a private
nested compositor.

Starts `kwin_wayland --virtual`, runs Chela in it and owns the clipboard with a test
client, which also presses the keys; a second client drags a file onto the window, and
a third scrolls and selects a transcript with a real pointer and keyboard.
Each case offers what a real application offers.
Skips (exit 0) when KWin or the protocol files are missing. Your session is untouched:
nothing here connects to your desktop's compositor, clipboard or D-Bus session.
"""
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import time

from ui_gallery import ROOT, png

BUILD = ROOT / "build"
CLIENT = BUILD / "wayland-test/paste-client"
DRAG_CLIENT = BUILD / "wayland-test/drag-client"
INPUT_CLIENT = BUILD / "wayland-test/input-client"
SOCKET = "chela-paste-test"


def session(temp, script):
    """Runs a shell script as the nested compositor's session.

    The compositor gets a D-Bus session of its own. On the desktop's bus it would
    register its shortcuts with the desktop's KWin, and take them away on exit: Alt+Tab
    and every other KWin shortcut then stay dead until the next login.
    """
    path = temp / "session.sh"
    bus = temp / "session-bus"
    path.write_text(f"#!/bin/sh\nprintf %s \"$DBUS_SESSION_BUS_ADDRESS\" > '{bus}'\n" + script)
    path.chmod(0o700)
    env = dict(os.environ, KWIN_WAYLAND_NO_PERMISSION_CHECKS="1", HOME=str(temp), XDG_CONFIG_HOME=str(temp / "config"),
               XDG_DATA_HOME=str(temp / "data"), LANG="C.UTF-8")
    env.pop("WAYLAND_SOCKET", None)
    env.pop("DISPLAY", None)
    result = subprocess.run(["timeout", "-k", "5", "60", "dbus-run-session", "--", "kwin_wayland", "--virtual",
                             "--no-global-shortcuts", "--no-kactivities", "--no-lockscreen", "--socket", SOCKET,
                             "--width", "1280", "--height", "800", "--exit-with-session", str(path)],
                            env=env, capture_output=True, text=True, timeout=70)
    assert bus.exists() and bus.read_text() not in ("", os.environ.get("DBUS_SESSION_BUS_ADDRESS")), \
        ("the nested compositor must not share the desktop's session bus", result.stderr[-800:])
    return result


def run_case(name, offers, expect, attachments_folder="data/chela/attachments"):
    with tempfile.TemporaryDirectory(prefix="chela-wayland-") as folder:
        temp = Path(folder)
        (temp / "project").mkdir()
        config = temp / "config/chela"
        config.mkdir(parents=True)
        (config / "settings.ini").write_text("[connection]\nendpoint=http://127.0.0.1:9/v1\nmodel=m\nprovider=0\ntools=false\n")
        (config / "desktop.ini").write_text("[model]\nkeep_loaded=false\n")
        png(temp / "picture.png", 32, 24, (40, 90, 200))
        (temp / "notes.txt").write_text("notes from a file\n")
        (temp / "files.uri").write_text(f"file://{temp}/notes.txt\r\n")
        (temp / "gnome.uri").write_text(f"copy\nfile://{temp}/notes.txt")
        (temp / "text.txt").write_text("pasted words")
        (temp / "link.uri").write_text("https://example.test/page\r\n")
        arguments = " ".join(f"'{mime}' '{temp / source}'" for mime, source in offers)
        result = session(temp, f"""
'{ROOT}/build/chela' --backend=wayland '{temp}/project' > '{temp}/chela.log' 2>&1 &
pid=$!
sleep 2.5
'{CLIENT}' paste {arguments} || echo client-failed > '{temp}/failed'
sleep 1
'{CLIENT}' return
sleep 1
kill $pid
wait $pid
""")
        if (temp / "failed").exists():
            raise AssertionError(f"{name}: test client failed: {result.stderr[-800:]}")
        stored = sorted(p.suffix for p in (temp / attachments_folder).glob("*"))
        db = temp / "data/chela/sessions.db"
        messages = []
        if db.exists():
            with sqlite3.connect(db) as connection:
                rows = connection.execute("SELECT messages FROM sessions").fetchall()
            messages = json.loads(rows[0][0]) if rows else []
        user = next((m for m in messages if m.get("role") == "user"), {})
        assert (stored, user.get("content", "")) == expect, (name, stored, user, (temp / "chela.log").read_text()[-600:])
        print(f"ok {name}")


def run_drag():
    with tempfile.TemporaryDirectory(prefix="chela-wayland-") as folder:
        temp = Path(folder)
        (temp / "project").mkdir()
        config = temp / "config/chela"
        config.mkdir(parents=True)
        (config / "settings.ini").write_text("[connection]\nendpoint=http://127.0.0.1:9/v1\nmodel=m\nprovider=0\ntools=false\n")
        (config / "desktop.ini").write_text("[model]\nkeep_loaded=false\n")
        (temp / "dragged.txt").write_text("dragged from a file manager\n")
        result = session(temp, f"""
'{ROOT}/build/chela' --backend=wayland '{temp}/project' > '{temp}/chela.log' 2>&1 &
pid=$!
sleep 2.5
'{DRAG_CLIENT}' 'file://{temp}/dragged.txt' > '{temp}/drag.log' 2>&1 || echo failed >> '{temp}/drag.log'
sleep 1
kill $pid
wait $pid
""")
        log = (temp / "drag.log").read_text() if (temp / "drag.log").exists() else result.stderr[-600:]
        stored = [p.suffix for p in (temp / "data/chela/attachments").glob("*")]
        assert stored == [".txt"], ("drop", stored, log, (temp / "chela.log").read_text()[-600:])
        assert "drag finished" in log, log
        print("ok file dragged onto the window")


def seed_session(temp, project, answer):
    """A saved session to open: the app adds its own tables and columns to these."""
    (temp / "data/chela").mkdir(parents=True)
    messages = [{"role": "user", "content": "long answer"}, {"role": "assistant", "content": answer}]
    with sqlite3.connect(temp / "data/chela/sessions.db") as connection:
        connection.execute("CREATE TABLE sessions (id TEXT PRIMARY KEY, title TEXT NOT NULL, project TEXT NOT NULL, "
                           "model TEXT NOT NULL, updated INTEGER NOT NULL, messages TEXT NOT NULL)")
        connection.execute("INSERT INTO sessions VALUES ('seed', 'long answer', ?, 'm', 1, ?)",
                           (str(project), json.dumps(messages)))


def run_reading():
    """The wheel reaches the top, a drag selects across paragraphs, Ctrl+A everything.

    What was selected comes back through the real clipboard: pasted into the message
    box and sent, it is stored with the session, where this test reads it. The first
    message can only start at paragraph 1 if the wheel moved a transcript that opened
    at its end.
    """
    paragraphs = [f"Paragraph number {i} talks about one subject at some length." for i in range(1, 41)]
    with tempfile.TemporaryDirectory(prefix="chela-wayland-") as folder:
        temp = Path(folder)
        project = temp / "project"
        project.mkdir()
        config = temp / "config/chela"
        config.mkdir(parents=True)
        (config / "settings.ini").write_text("[connection]\nendpoint=http://127.0.0.1:9/v1\nmodel=m\nprovider=0\ntools=false\n")
        (config / "desktop.ini").write_text("[model]\nkeep_loaded=false\n")
        seed_session(temp, project, "\n\n".join(paragraphs))
        steps = ("click 100 220 wait 700 "                       # open the session
                 "move 700 400 wheel -9000 wait 500 "            # to the top of the transcript
                 "press 380 150 move 500 200 move 700 260 move 800 260 wait 200 release wait 200 "
                 "key ctrl+c wait 200 key ctrl+v wait 400 key return wait 1500 "
                 "move 700 300 key ctrl+a wait 200 key ctrl+c wait 200 key ctrl+v wait 400 key return wait 1500")
        result = session(temp, f"""
'{ROOT}/build/chela' --backend=wayland '{project}' > '{temp}/chela.log' 2>&1 &
pid=$!
sleep 2.5
'{INPUT_CLIENT}' {steps} || echo client-failed > '{temp}/failed'
sleep 0.5
kill $pid
wait $pid
""")
        log = (temp / "chela.log").read_text()[-600:]
        if (temp / "failed").exists():
            raise AssertionError(f"reading: test client failed: {result.stderr[-800:]}")
        with sqlite3.connect(temp / "data/chela/sessions.db") as connection:
            messages = json.loads(connection.execute("SELECT messages FROM sessions").fetchone()[0])
        sent = [m["content"] for m in messages if m.get("role") == "user"][1:]
        assert len(sent) == 2, ("reading", sent, log)
        assert "number 1 talks" in sent[0][:40], sent[0][:120]  # the press lands a little way into the first word
        assert "\n\nParagraph number 2 " in sent[0] and paragraphs[-1] not in sent[0], sent[0][-200:]
        assert sent[1].startswith("long answer") and paragraphs[-1] in sent[1], sent[1][:120]
        print("ok wheel, drag across paragraphs, Ctrl+A and Ctrl+C with a real pointer and keyboard")


def main():
    missing = [p for p in ("kwin_wayland", "dbus-run-session") if not shutil.which(p)]
    if missing or not CLIENT.exists():
        print("Skipped: needs kwin_wayland, dbus-run-session and the test client (make wayland-test builds it)")
        return
    run_case("screenshot on the clipboard", [("image/png", "picture.png")], ([".png"], ""))
    run_case("picture copied from a browser", [("text/html", "text.txt"), ("image/png", "picture.png")], ([".png"], ""))
    run_case("file copied in a file manager", [("text/plain", "text.txt"), ("text/uri-list", "files.uri")], ([".txt"], ""))
    run_case("file copied in GNOME Files", [("UTF8_STRING", "text.txt"), ("x-special/gnome-copied-files", "gnome.uri")], ([".txt"], ""))
    run_case("plain text", [("text/plain;charset=utf-8", "text.txt"), ("text/plain", "text.txt")], ([], "pasted words"))
    run_case("text that is also a picture", [("image/png", "picture.png"), ("text/plain", "text.txt")], ([], "pasted words"))
    run_case("a web link is text", [("text/uri-list", "link.uri"), ("text/plain", "link.uri")], ([], "https://example.test/page"))
    run_drag()
    run_reading()
    print("Wayland paste, drop, scrolling and selection test passed")


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).parent))
    main()
