"""Desktop checks for the Connections page and the SSH tunnel's lifecycle.

Connections is the SSH model library setup itself; there is no second form. A
stand-in `ssh` forwards the managed tunnel to a loopback model API, so these checks
cover a Chela that is killed, a tunnel orphaned by an older Chela, a second Chela
window, and a port held by another program. Never contacts an inference computer.
"""
import hashlib
import json
import os
from pathlib import Path
import signal
import socket
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from headless import BUILD, ROOT, Harness, wait_for

MODEL = 'fixture-model'
FAKE_SSH = r'''
"""Test stand-in for ssh: forwards -L tunnels and records each tunnel's pid."""
import os, socket, sys, threading
args = sys.argv[1:]
def pipe(source, target):
    try:
        while True:
            data = source.recv(65536)
            if not data:
                break
            target.sendall(data)
    except OSError:
        pass
    for end in (source, target):
        try:
            end.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
if '-L' not in args:
    sys.exit(255)  # helper commands (device status, diagnostics) are not needed here
_, local, _, remote = args[args.index('-L') + 1].split(':')
listener = socket.socket()
listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
listener.bind(('127.0.0.1', int(local)))
listener.listen(16)
with open(os.environ['CHELA_FAKE_TUNNELS'], 'a') as log:
    log.write(f'{os.getpid()}\n')
while True:
    client, _ = listener.accept()
    upstream = socket.create_connection(('127.0.0.1', int(remote)))
    threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
    threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()
'''


class Router(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def send_json(self, data):
        body = json.dumps(data).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        self.server.gets += 1
        self.send_json({'data': [{'id': MODEL, 'status': {'value': 'loaded'}}]})

    def do_POST(self):
        self.rfile.read(int(self.headers.get('Content-Length', 0)))
        self.send_json({'success': True})


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def listening(port):
    try:
        socket.create_connection(('127.0.0.1', port), timeout=.5).close()
        return True
    except OSError:
        return False


def alive(pid):
    try:
        state = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()[0]
    except (FileNotFoundError, IndexError):
        return False
    return state not in 'ZX'


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Router)
    server.gets = 0
    threading.Thread(target=server.serve_forever, daemon=True).start()
    apps, helpers, harness = [], [], None
    log = open(BUILD / 'connections-ui.log', 'w')
    try:
        with tempfile.TemporaryDirectory(prefix='chela-connections-ui-') as folder:
            root = Path(folder)
            harness = Harness(root)
            (root / 'bin').mkdir()
            fake = root / 'bin/ssh'
            fake.write_text(f'#!{sys.executable}\n' + FAKE_SSH)
            fake.chmod(0o700)
            tunnels = root / 'tunnels'
            tunnels.touch()
            local = free_port()
            config = root / 'config/chela/settings.ini'
            config.parent.mkdir(parents=True)
            config.write_text(f'[connection]\nendpoint=http://127.0.0.1:{local}/v1\nmodel={MODEL}\nprovider=6\n'
                              'ssh_host=fixture.test\nssh_user=tester\nssh_port=22\nssh_enabled=true\nssh_remember=false\n'
                              f'remote_port={server.server_port}\nlocal_port={local}\nmodel_dir=~/models\n'
                              'inference_binary=/srv/inferssd-server\ngpu_device=auto\ntools=false\ntimeout=30\n')
            env = {'PATH': f'{root / "bin"}:{os.environ["PATH"]}', 'HOME': str(root),
                   'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_DATA_HOME': str(root / 'data'),
                   'XDG_RUNTIME_DIR': str(root), 'CHELA_FAKE_TUNNELS': str(tunnels), **harness.env}

            def start():
                app = subprocess.Popen([str(ROOT / 'build/chela'), str(root)], cwd=root, env=env, stdout=log, stderr=log)
                apps.append(app)
                return app, harness.window('^Chela$')

            def tunnel_pids():
                return [int(line) for line in tunnels.read_text().split()]

            def quit(app, w):
                w.key('ctrl+q')
                assert app.wait(timeout=10) == 0, (BUILD / 'connections-ui.log').read_text()

            # Connections is the SSH setup form, without a second dialog or manual server fields.
            app, w = start()
            wait_for(lambda: tunnel_pids() and listening(local), what='the tunnel')
            first = tunnel_pids()[-1]
            w.key('ctrl+comma')
            w.wait_visible('^Appearance$')
            w.key('ctrl+2')
            w.wait_visible('^Connect & refresh library$')
            for label in ('^SSH address$', '^Username$', '^Model folder$', '^Advanced$',
                          '^Remember SSH login in my desktop keyring$', '^Forget saved SSH login$'):
                assert w.visible(label), (label, w.texts())
            assert any(text == 'fixture.test' for text in w.texts()), w.texts()
            for removed in ('Server address', 'API key variable', 'ACP command', 'Refresh models', 'Connect with SSH'):
                assert not w.visible(removed), (removed, w.texts())
            w.tap('Advanced')
            w.wait_visible('^Local port$')
            w.shot('ui-connections.png')
            w.key('escape')
            w.wait(lambda: not w.visible('^Connect & refresh library$'), what='settings to close')

            # A Chela that is killed outright takes its tunnel with it.
            app.kill()
            app.wait(timeout=10)
            wait_for(lambda: not alive(first), 10, 'the tunnel to end with Chela')
            wait_for(lambda: not listening(local), 10, 'the local port to be released')

            # A tunnel orphaned by an older Chela holds the port and left a stale control socket.
            # The next Chela ends it, removes the socket and connects through its own tunnel.
            control = root / 'chela-ssh'
            control.mkdir(mode=0o700, exist_ok=True)
            stale = control / hashlib.sha256(b'tester@fixture.test:22').hexdigest()[:16]
            with socket.socket(socket.AF_UNIX) as sock:
                sock.bind(str(stale))
            orphan = subprocess.Popen(
                [str(fake), '-F', '/dev/null', '-T', '-o', 'ControlPersist=no', '-o', f'ControlPath={stale}',
                 '-o', 'ControlMaster=yes', '-p', '22', '-l', 'tester', '-N',
                 '-L', f'127.0.0.1:{local}:127.0.0.1:{server.server_port}', '--', 'fixture.test'],
                env=env, start_new_session=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            helpers.append(orphan)
            wait_for(lambda: listening(local) and orphan.pid in tunnel_pids(), what='the orphaned tunnel')
            before = server.gets
            app, w = start()
            assert orphan.wait(timeout=10) == -signal.SIGTERM, orphan.returncode
            wait_for(lambda: tunnel_pids()[-1] not in (first, orphan.pid) and listening(local), what='a fresh tunnel')
            wait_for(lambda: server.gets > before, what='the model API through the new tunnel')
            assert not stale.exists(), 'The stale control socket must be removed'
            assert not w.visible('in use'), w.texts()

            # A second Chela window explains the conflict and leaves the first window's tunnel alone.
            owner = tunnel_pids()[-1]
            second, w2 = start()
            w2.wait_visible(f'Local port {local} is in use by another Chela window')
            w2.key('ctrl+comma')
            w2.wait_visible('^Appearance$')
            w2.key('ctrl+2')
            w2.wait_visible(f'Local port {local} is in use by another Chela window.*Settings → Connections → Advanced')
            w2.shot('ui-connections-port-in-use.png')
            assert alive(owner) and listening(local)
            w2.key('escape')
            quit(second, w2)
            assert alive(owner)
            quit(app, w)
            wait_for(lambda: not alive(owner), 10, 'the tunnel to stop when Chela quits')

            # Another program on the port is reported and never stopped.
            program = subprocess.Popen([sys.executable, '-c', 'import socket, sys, time\n'
                                        's = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)\n'
                                        's.bind(("127.0.0.1", int(sys.argv[1]))); s.listen(1)\n'
                                        'print("ready", flush=True); time.sleep(60)', str(local)],
                                       stdout=subprocess.PIPE, text=True)
            helpers.append(program)
            assert program.stdout.readline().strip() == 'ready'
            app, w = start()
            w.wait_visible(f'Local port {local} is in use by another program')
            assert program.poll() is None, 'Chela must not stop a program it does not own'
            quit(app, w)
        print('Connections desktop checks passed: SSH setup as the only form, tunnel ending with a killed Chela, '
              'orphaned tunnel and stale socket recovery, second-window and other-program port conflicts.')
    finally:
        for process in apps + helpers:
            if process.poll() is None:
                process.kill()
                process.wait(timeout=5)
        if harness:
            harness.close()
        log.close()
        server.shutdown()


if __name__ == '__main__':
    main()
