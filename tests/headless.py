"""Drive headless Chela processes over the interactive control socket.

Every Chela process started with this harness's environment uses the headless
backend, connects to the harness, announces its window and then runs one command
at a time (the commands are listed in src/platform/headless.c). The desktop and
the SSH askpass prompts it starts are separate processes, so one harness drives
them all. No display, toolkit or input tools are needed.
"""
import os
from pathlib import Path
import re
import socket
import threading
import time

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / "build"


class Window:
    """One Chela process and its window."""

    def __init__(self, conn, stream, title):
        self.conn, self.stream, self.title = conn, stream, title

    def do(self, command):
        """Runs a command and returns the text lines it reported; raises when it fails."""
        self.stream.write(command + "\n")
        self.stream.flush()
        texts = []
        for line in self.stream:
            line = line.rstrip("\n")
            if line == "@ok":
                return texts
            if line.startswith("@fail "):
                raise AssertionError(f"{self.title}: {line[6:]}")
            if line.startswith("@text "):
                texts.append(line[6:].replace("\\n", "\n").replace("\\\\", "\\"))
        raise AssertionError(f"{self.title}: the process exited during: {command}")

    def texts(self):
        return self.do("dump")

    def visible(self, pattern):
        return any(re.search(pattern, text) for text in self.texts())

    def wait(self, predicate, timeout=20, what="interface state"):
        deadline = time.monotonic() + timeout
        while True:
            value = predicate()
            if value:
                return value
            if time.monotonic() > deadline:
                raise AssertionError(f"{self.title}: timed out waiting for {what}:\n" + "\n".join(self.texts()))
            time.sleep(.05)

    def wait_visible(self, pattern, timeout=20):
        return self.wait(lambda: self.visible(pattern), timeout, repr(pattern))

    def key(self, *names):
        for name in names:
            self.do("key " + name)

    def type(self, text):
        self.do("type " + text.replace("\n", "\\n"))

    def tap(self, text):
        self.do("tap " + text)

    def shot(self, name):
        self.do(f"shot {BUILD / name}")

    def gone(self):
        """Whether the process has exited (its window closed)."""
        try:
            self.do("idle")
            return False
        except (AssertionError, OSError):
            return True

    def close(self):
        try:
            self.conn.close()
        except OSError:
            pass


class Harness:
    def __init__(self, folder):
        self.path = str(Path(folder) / "control.sock")
        self.listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.listener.bind(self.path)
        self.listener.listen(8)
        self.windows, self.claimed = [], set()
        self.ready = threading.Condition()
        threading.Thread(target=self._accept, daemon=True).start()

    @property
    def env(self):
        return {"CHELA_BACKEND": "headless", "CHELA_HEADLESS_CONTROL": self.path}

    def _accept(self):
        while True:
            try:
                conn, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self._hello, args=(conn,), daemon=True).start()

    def _hello(self, conn):
        stream = conn.makefile("rw", encoding="utf-8", errors="replace", newline="\n")
        line = stream.readline()
        if not line.startswith("@window "):
            conn.close()
            return
        with self.ready:
            self.windows.append(Window(conn, stream, line[8:].rstrip("\n")))
            self.ready.notify_all()

    def window(self, pattern, timeout=20):
        """The next unclaimed window whose title matches."""
        deadline = time.monotonic() + timeout
        with self.ready:
            while True:
                for w in self.windows:
                    if id(w) not in self.claimed and re.search(pattern, w.title):
                        self.claimed.add(id(w))
                        return w
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"No window matching {pattern!r} opened; saw {[w.title for w in self.windows]}")
                self.ready.wait(remaining)

    def no_window(self, pattern):
        with self.ready:
            return not any(id(w) not in self.claimed and re.search(pattern, w.title) for w in self.windows)

    def close(self):
        self.listener.close()
        for w in self.windows:
            w.close()
        try:
            os.unlink(self.path)
        except OSError:
            pass


def wait_for(fn, timeout=20, what="a condition"):
    deadline = time.monotonic() + timeout
    while True:
        value = fn()
        if value:
            return value
        if time.monotonic() > deadline:
            raise AssertionError(f"Timed out waiting for {what}")
        time.sleep(.05)
