"""Verify model loading, keep-loaded, device and memory status in the real desktop.

A fixture model router serves the API. A stand-in `ssh` forwards the managed
tunnel to it and answers the helper's device-status command with canned readings,
so the SSH-managed InferSSD path runs without hardware or credentials. Uses the
headless backend and isolated settings. Screenshots go to build/.
"""
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from headless import BUILD, ROOT, Harness, wait_for

MODEL = 'status-model'
FAKE_SSH = r'''#!/usr/bin/env python3
"""Test stand-in for ssh: forwards -L tunnels and answers Chela's helper commands."""
import os, socket, sys, threading
args = sys.argv[1:]
command = args[-1] if args else ''
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
if '-L' in args:
    _, local, _, remote = args[args.index('-L') + 1].split(':')
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(('127.0.0.1', int(local)))
    listener.listen(16)
    while True:
        client, _ = listener.accept()
        upstream = socket.create_connection(('127.0.0.1', int(remote)))
        threading.Thread(target=pipe, args=(client, upstream), daemon=True).start()
        threading.Thread(target=pipe, args=(upstream, client), daemon=True).start()
elif '--device-status' in command:
    sys.stdin.read()
    with open(os.environ['CHELA_FAKE_SSH_LOG'], 'a') as log:
        log.write('device-status\n')
    sys.stdout.write(open(os.environ['CHELA_FAKE_DEVICE']).read())
elif 'systemctl' in command:
    print('Service state\nActiveState=active\n\nRecent server log\nfixture router log')
else:
    sys.exit(255)
'''
LOADING_DEVICE = {'model': MODEL, 'confirmed': False,
                  'summary': 'Loading model · 20/25 layers assigned to GPU; load not complete',
                  'live': {'gpus': [{'card': 'card1', 'vram_used_mib': 9000, 'vram_total_mib': 16368, 'busy_percent': 3}]}}
LOADED_DEVICE = {'model': MODEL, 'confirmed': True, 'backend': 'GPU + CPU',
                 'summary': 'GPU + CPU — AMD Radeon RX 6800 (ROCm0) · 24/25 layers on GPU',
                 'actual_devices': [{'id': 'ROCm0', 'name': 'AMD Radeon RX 6800'}],
                 'memory': {'gpu_mib': 15812.5, 'ram_mib': 5084.5, 'devices': {
                     'ROCm0': {'weights': 15000.0, 'context': 512.0, 'compute': 300.5},
                     'CPU_Mapped': {'weights': 5000.0, 'context': 0.0, 'compute': 0.0},
                     'CPU': {'weights': 0.0, 'context': 64.0, 'compute': 20.5}}},
                 'live': {'gpus': [{'card': 'card1', 'vram_used_mib': 15800, 'vram_total_mib': 16368, 'busy_percent': 87}],
                          'process': {'pid': 4242, 'rss_mib': 5200}}}


class Router(BaseHTTPRequestHandler):
    """llama.cpp router subset: model status, load/unload and a streamed reply."""
    def log_message(self, *_):
        pass

    def send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        assert self.path == '/v1/models', self.path
        server = self.server
        status = {'value': server.state}
        if server.state == 'failed':
            status = {'value': 'unloaded', 'failed': True, 'exit_code': 1}
        self.send_json({'data': [{'id': MODEL, 'status': status}, {'id': 'other-model', 'status': {'value': 'unloaded'}}]})

    def do_POST(self):
        server = self.server
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        if self.path in ('/models/load', '/models/unload'):
            assert payload == {'model': MODEL}, payload
            server.controls.append(self.path)
            if self.path == '/models/load':
                server.state = 'loading'
                release = server.release = threading.Event()
                fail = server.fail_next
                server.fail_next = False

                def finish():
                    release.wait(60)
                    if server.state == 'loading':
                        server.state = 'failed' if fail else 'loaded'
                threading.Thread(target=finish, daemon=True).start()
            else:
                server.state = 'unloaded'
                server.release.set()
            self.send_json({'success': True})
            return
        assert self.path == '/v1/chat/completions', self.path
        assert server.state == 'loaded', server.state
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        for delta, finish in (({'content': 'Status checked.'}, None), ({}, 'stop')):
            event = {'choices': [{'delta': delta, 'finish_reason': finish}],
                     'timings': {'predicted_n': 3, 'prompt_n': 9, 'predicted_per_second': 51.0}}
            self.wfile.write(('data: ' + json.dumps(event) + '\n\n').encode())
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Router)
    server.state, server.controls, server.fail_next, server.release = 'unloaded', [], False, threading.Event()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    app = harness = None
    log_path = BUILD / 'model-status-ui.log'
    try:
        with tempfile.TemporaryDirectory(prefix='chela-model-status-ui-') as folder:
            root = Path(folder)
            harness = Harness(root)
            bin_dir = root / 'bin'
            bin_dir.mkdir()
            (bin_dir / 'ssh').write_text(f'#!{sys.executable}\n' + FAKE_SSH.split('\n', 1)[1])
            (bin_dir / 'ssh').chmod(0o700)
            device = root / 'device.json'
            device.write_text(json.dumps(LOADING_DEVICE))
            ssh_log = root / 'ssh.log'
            local = free_port()
            config = root / 'config/chela/settings.ini'
            config.parent.mkdir(parents=True)
            config.write_text(f'[connection]\nendpoint=http://127.0.0.1:{local}/v1\nmodel={MODEL}\nprovider=6\n'
                              'ssh_host=fixture.test\nssh_user=tester\nssh_port=22\nssh_enabled=true\nssh_remember=false\n'
                              f'remote_port={server.server_port}\nlocal_port={local}\nmodel_dir=~/models\n'
                              'inference_binary=/srv/inferssd-server\ngpu_device=auto\ntools=false\ntimeout=30\n')
            preferences = config.parent / 'desktop.ini'
            # Keep-loaded is the default; a short idle time exercises idle unloading when it is off.
            preferences.write_text('[model]\nidle_unload_seconds=4\n')
            env = {'PATH': f'{bin_dir}:{os.environ["PATH"]}', 'HOME': str(root),
                   'XDG_CONFIG_HOME': str(root / 'config'), 'XDG_DATA_HOME': str(root / 'data'),
                   'XDG_RUNTIME_DIR': str(root), 'CHELA_FAKE_DEVICE': str(device),
                   'CHELA_FAKE_SSH_LOG': str(ssh_log), **harness.env}
            with log_path.open('w') as log:
                app = subprocess.Popen([str(ROOT / 'build/chela'), str(root)], cwd=root, env=env,
                                       stdout=log, stderr=log)
                w = harness.window('^Chela$')

                def preference(key):
                    match = re.search(rf'^{key}=(.*)$', preferences.read_text(), re.M)
                    return match and match[1]

                # Keep-loaded (default) loads the model at startup and shows the load timer.
                wait_for(lambda: server.controls == ['/models/load'], 20, 'the startup load')
                w.wait_visible(r'^Loading 0:0[1-9]', 10)
                w.wait_visible(f'Loading {MODEL}…')
                assert w.visible('First timed load of this model'), w.texts()
                w.wait_visible('20/25 layers assigned to GPU', 10)
                w.shot('ui-model-loading.png')

                device.write_text(json.dumps(LOADED_DEVICE))
                time.sleep(1.2)
                server.release.set()
                w.wait_visible(r'Ready in \d+\.\ds', 10)
                w.wait_visible('^Ready · GPU \\+ CPU', 10)
                w.shot('ui-model-ready.png')
                assert float(preference(re.escape(MODEL))) >= 1, preferences.read_text()

                w.key('ctrl+shift+s')
                w.wait_visible('^Model status')
                w.wait_visible('15.4 GiB on GPU · 5.0 GiB in system RAM', 10)
                assert w.visible('Weights 19.5 GiB · context cache 576 MiB · compute 321 MiB'), w.texts()
                assert w.visible('VRAM 15.4 GiB of 16.0 GiB in use · 87% busy'), w.texts()
                assert w.visible('Model process RAM 5.1 GiB'), w.texts()
                assert w.visible('GPU \\+ CPU — AMD Radeon RX 6800 \\(ROCm0\\) · 24/25 layers on GPU')
                assert w.visible('Loads now and stays in memory')
                w.shot('ui-model-status.png')

                # Turning keep-loaded off unloads after the idle time; it does not reload.
                w.tap('Keep model loaded')
                wait_for(lambda: preference('keep_loaded') == 'false', what='keep_loaded=false')
                w.wait_visible('Unloads after 4s idle')
                wait_for(lambda: server.controls[-1:] == ['/models/unload'], 12, 'the idle unload')
                w.wait_visible('^Not loaded', 6)
                time.sleep(2.5)
                assert server.controls == ['/models/load', '/models/unload'], server.controls

                # Turning it back on loads immediately, with an estimate from the previous load.
                w.tap('Keep model loaded')
                wait_for(lambda: server.controls[-1:] == ['/models/load'], what='the reload')
                w.wait_visible('last load took', 6)
                # The previous load's placement is not presented as the new load's.
                assert not any('layers on GPU' in text and 'last load took' in text for text in w.texts()), w.texts()
                device.write_text(json.dumps(LOADING_DEVICE))
                w.wait_visible('last load took.*20/25 layers assigned to GPU', 8)
                w.shot('ui-model-reloading.png')
                device.write_text(json.dumps(LOADED_DEVICE))
                server.release.set()
                w.wait_visible('^Ready · GPU \\+ CPU', 10)

                # A manual unload also turns keep-loaded off, so Chela does not reload the model.
                w.tap('Unload')
                wait_for(lambda: server.controls[-1:] == ['/models/unload'], what='the manual unload')
                wait_for(lambda: preference('keep_loaded') == 'false', what='keep_loaded=false')
                w.wait_visible('^Not loaded', 6)
                time.sleep(2.5)
                assert server.controls.count('/models/load') == 2, server.controls

                # A failed load is reported with the server's exit code and can be retried.
                server.fail_next = True
                w.tap('Load now')
                wait_for(lambda: server.controls.count('/models/load') == 3, what='the third load')
                server.release.set()
                w.key('escape')
                w.wait_visible(f'{MODEL} failed to load', 10)
                assert w.visible('exited with code 1'), w.texts()
                assert w.visible('^Load failed'), w.texts()
                w.shot('ui-model-failed.png')
                w.tap('Retry')
                wait_for(lambda: server.controls.count('/models/load') == 4, what='the retried load')
                server.release.set()
                w.wait_visible('^Ready · GPU \\+ CPU', 10)

                # The request monitor names the device the reply ran on. (Retry kept keyboard focus.)
                w.tap('Describe a task or ask a question…')
                w.type('where are you running')
                w.key('return')
                w.wait_visible('Status checked.', 10)
                w.wait_visible('^Completed', 10)
                w.wait_visible('51.0 tok/s.*on GPU \\+ CPU \\(AMD Radeon RX 6800\\)', 6)
                w.shot('ui-model-request-device.png')
                assert 'device-status' in ssh_log.read_text()
                w.key('ctrl+q')
                assert app.wait(timeout=10) == 0, log_path.read_text()
        print('Model status desktop checks passed: keep-loaded autoload, load timer and ready time, device, '
              'memory and live GPU readings, idle unload, load estimate, manual unload, failed load and retry, '
              'and device in the request monitor.')
    finally:
        server.release.set()
        if app and app.poll() is None:
            app.terminate()
            app.wait(timeout=5)
        if harness:
            harness.close()
        server.shutdown()


if __name__ == '__main__':
    main()
