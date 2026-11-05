"""Verify model loading, pre-load, idle unload, device and memory status and context use in the real desktop.

A fixture model router serves the API. A stand-in `ssh` forwards the managed
tunnel to it and answers the helper's device-status command with canned readings,
so the SSH-managed InferSSD path runs without hardware or credentials. Uses the
headless backend and isolated settings. Screenshots go to build/.
"""
from collections import Counter
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
BAR_COLOURS = {'track': bytes.fromhex('383838'), 'blue': bytes.fromhex('72a4ec'),
               'yellow': bytes.fromhex('e6c04e'), 'red': bytes.fromhex('e5534b')}
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
                  'load': {'phase': 'reading', 'file_mib': 40000, 'target_mib': 20480, 'mapped_mib': 5120, 'gpu_mib': 0},
                  'live': {'gpus': [{'card': 'card1', 'vram_used_mib': 9000, 'vram_total_mib': 16368, 'busy_percent': 3}]}}
UPLOADING_DEVICE = dict(LOADING_DEVICE, load={'phase': 'uploading', 'file_mib': 40000, 'target_mib': 20480,
                                              'mapped_mib': 20480, 'gpu_mib': 15360, 'upload_percent': 40})
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
        server = self.server
        if self.path.startswith('/props'):
            assert 'autoload=false' in self.path, self.path  # asking about a model never loads it
            if server.state != 'loaded':
                self.send_json({'error': {'code': 400, 'message': 'model is not running'}}, 400)
                return
            # The running process has less context than it was asked for, as when capped by the model.
            self.send_json({'default_generation_settings': {'n_ctx': 4096}, 'modalities': {'vision': True}})
            return
        assert self.path == '/v1/models', self.path
        status = {'value': server.state, 'args': ['server', '--ctx-size', '8192', '--mmproj', '/models/vision.gguf']}
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
        if server.state == 'unloaded':
            server.state = 'loaded'  # the router loads a model for a request
        assert server.state == 'loaded', server.state
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        for delta, finish in (({'content': 'Status checked.'}, None), ({}, 'stop')):
            event = {'choices': [{'delta': delta, 'finish_reason': finish}],
                     'timings': {'predicted_n': 3, 'prompt_n': 9, 'predicted_per_second': 51.0}}
            self.wfile.write(('data: ' + json.dumps(event) + '\n\n').encode())
        # prompt_n above is only what the prompt cache missed; usage has the whole prompt.
        usage = {'choices': [], 'usage': {'prompt_tokens': server.prompt_tokens, 'completion_tokens': 409}}
        self.wfile.write(('data: ' + json.dumps(usage) + '\n\n').encode())
        self.wfile.write(b'data: [DONE]\n\n')
        self.wfile.flush()


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Router)
    server.state, server.controls, server.fail_next, server.release = 'unloaded', [], False, threading.Event()
    server.prompt_tokens = 1229
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
            # Saved by an older version: keep-loaded ignored the idle time, so it must not start to apply now.
            preferences.write_text('[model]\nkeep_loaded=true\nidle_unload_seconds=4\n')
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

                def context_panel():
                    w.key('ctrl+shift+s')
                    w.wait_visible('^Diagnostics$')

                def model_menu():
                    w.key('ctrl+shift+l')
                    w.wait_visible('^Pre-load model')

                def bar_colours(name):
                    """Solid pixels of each meter colour in a screenshot: a bar has hundreds, the ring a few dozen."""
                    w.shot(name)
                    rgb = subprocess.check_output(['convert', str(ROOT / 'build' / name), '-alpha', 'off', '-depth', '8', 'rgb:-'])
                    counts = Counter(rgb[i:i + 3] for i in range(0, len(rgb), 3))
                    return {colour: counts[value] for colour, value in BAR_COLOURS.items()}

                def open_settings():
                    w.key('ctrl+shift+t')
                    w.wait_visible('^Unload when idle')

                # Pre-load (default) loads the model at startup, with a real bar: bytes read, not a sweep.
                wait_for(lambda: server.controls == ['/models/load'], 20, 'the startup load')
                w.wait_visible(f'^Loading {MODEL}$', 10)
                assert w.visible('Waiting for the server') or w.visible('Reading weights'), w.texts()
                w.wait_visible(r'Reading weights · 5\.0 GiB of 20\.0 GiB', 10)
                w.wait_visible(r'^\d+%', 6)
                w.shot('ui-model-loading.png')
                device.write_text(json.dumps(dict(LOADING_DEVICE, load=dict(LOADING_DEVICE['load'], mapped_mib=10240))))
                w.wait_visible(r'Reading weights · 10\.0 GiB of 20\.0 GiB · \d+ MiB/s', 8)
                w.wait_visible(r'^\d+% · (\d+m )?\d+s left$', 6)  # two readings give a rate, and so an ETA
                device.write_text(json.dumps(UPLOADING_DEVICE))
                w.wait_visible(r'Copying 15\.0 GiB to the GPU · 40%', 8)
                w.shot('ui-model-loading-gpu.png')

                device.write_text(json.dumps(LOADED_DEVICE))
                time.sleep(1.2)
                server.release.set()
                w.wait(lambda: not w.visible('^Loading '), 10, 'the load card to close')
                assert float(preference(re.escape(MODEL))) >= 1, preferences.read_text()
                wait_for(lambda: preference(re.escape(MODEL)) and 'model_load_tail' in preferences.read_text(), 6, 'the learned load tail')
                learned = re.search(r'\[model_load_mapped\]\n' + re.escape(MODEL) + r'=(\S+)', preferences.read_text())
                assert learned and float(learned[1]) == 20480, preferences.read_text()
                w.shot('ui-model-ready.png')

                assert preference('preload') == 'true' and preference('idle_unload_seconds') == '0', preferences.read_text()
                assert preference('keep_loaded') is None, preferences.read_text()

                # The ring's popover is about context; the model's state and the switch are elsewhere.
                context_panel()
                assert w.visible('^Context$') and w.visible('tokens free'), w.texts()
                assert not w.visible('^Model status') and not w.visible('^Ready'), w.texts()
                assert not w.visible('^Pre-load model'), 'the switch lives in the model menu'
                assert not w.visible('^Load now$') and not w.visible('^Unload'), w.texts()
                assert not w.visible('^Device$') and not w.visible('^Memory$'), 'Device and memory live in Settings'
                bars = bar_colours('ui-model-status.png')
                assert bars['track'] > 1200 and bars['blue'] < 100, bars  # an empty bar, before any reply
                w.key('escape')

                # One switch beside Effort replaces the two buttons.
                model_menu()
                assert w.visible('^Effort$'), w.texts()
                w.shot('ui-model-menu.png')
                w.key('escape')

                # The specifics are in Settings, with the context the running process reports.
                open_settings()
                w.wait_visible(r'^Ready · loaded in', 10)
                w.wait_visible('15.4 GiB on GPU · 5.0 GiB in system RAM', 10)
                assert w.visible('Weights 19.5 GiB · context cache 576 MiB · compute 321 MiB'), w.texts()
                assert w.visible('VRAM 15.4 GiB of 16.0 GiB in use · 87% busy'), w.texts()
                assert w.visible('Model process RAM 5.1 GiB'), w.texts()
                assert w.visible('GPU \\+ CPU — AMD Radeon RX 6800 \\(ROCm0\\) · 24/25 layers on GPU')
                w.wait_visible('^4,096 tokens$', 10)
                assert w.visible('^Reads images$'), w.texts()
                assert w.visible('^Pre-load model') and w.visible('^Never$'), w.texts()
                w.shot('ui-settings-model.png')
                w.key('escape')

                # Switching it off frees the memory now, and the model stays unloaded. The menu stays open.
                model_menu()
                w.tap('Pre-load model')
                wait_for(lambda: server.controls[-1:] == ['/models/unload'], what='the unload')
                wait_for(lambda: preference('preload') == 'false', what='preload=false')
                assert w.visible('^Pre-load model'), w.texts()
                w.shot('ui-model-menu-off.png')
                w.key('escape')
                open_settings()
                w.wait_visible('^Not loaded', 6)
                w.key('escape')
                time.sleep(2.5)
                assert server.controls == ['/models/load', '/models/unload'], server.controls

                # Switching it back on, from the keyboard, loads once, and the last load supplies the estimate at once.
                model_menu()
                w.key('end')
                w.key('return')
                wait_for(lambda: server.controls[-1:] == ['/models/load'], what='the reload')
                assert preference('preload') == 'true', preferences.read_text()
                assert server.controls == ['/models/load', '/models/unload', '/models/load'], server.controls
                w.key('escape')
                w.wait_visible(f'^Loading {MODEL}$', 6)
                w.wait_visible(r'^\d+% · ', 6)
                # The previous load's placement is not presented as the new load's.
                assert not any('layers on GPU' in text for text in w.texts()), w.texts()
                device.write_text(json.dumps(LOADING_DEVICE))
                w.wait_visible(r'Reading weights · 5\.0 GiB of 20\.0 GiB', 8)
                w.shot('ui-model-reloading.png')
                device.write_text(json.dumps(LOADED_DEVICE))
                server.release.set()
                w.wait(lambda: not w.visible('^Loading '), 10, 'the load card to close')

                # Unload now, in Settings, also turns pre-load off, so Chela does not reload the model.
                open_settings()
                w.wait_visible('^Unload now$', 6)
                w.tap('Unload now')
                wait_for(lambda: server.controls[-1:] == ['/models/unload'], what='the manual unload')
                wait_for(lambda: preference('preload') == 'false', what='preload=false')
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
                w.shot('ui-model-failed.png')
                w.tap('Retry')
                wait_for(lambda: server.controls.count('/models/load') == 4, what='the retried load')
                server.release.set()
                w.wait(lambda: not w.visible('failed to load') and not w.visible('^Loading '), 10, 'the retry')

                # A reply shows its speed, and how full the context is at a glance, in the server's numbers.
                wait_for(lambda: server.state == 'loaded', what='the model to be ready')
                w.tap('Describe a task or ask a question…')
                w.type('where are you running')
                w.key('return')
                w.wait_visible('Status checked.', 10)
                w.wait_visible(r'51\.0 tok/s', 10)
                w.wait_visible('^40%$', 10)  # 1,229 + 409 of the running model's 4,096
                w.shot('ui-model-request-device.png')
                assert 'device-status' in ssh_log.read_text()
                context_panel()
                w.wait_visible('1,638 of 4,096 tokens · 40% used', 6)
                assert w.visible('Prompt 1,229 · reply 409 · 2,458 left'), w.texts()
                assert not w.visible('Nearly full'), w.texts()
                bars = bar_colours('ui-model-context.png')
                assert bars['blue'] > 500 and not bars['yellow'] and not bars['red'], bars
                w.key('escape')

                # Close to the limit it says so.
                server.prompt_tokens = 3400
                w.tap('Reply')
                w.type('and now')
                w.key('return')
                w.wait_visible('^93%$', 10)
                context_panel()
                w.wait_visible('Nearly full', 6)
                bars = bar_colours('ui-model-context-full.png')
                assert bars['yellow'] > 1000 and bars['blue'] < 100 and bars['red'] < 100, bars
                w.key('escape')

                # At the very end it turns red.
                server.prompt_tokens = 3600
                w.tap('Reply')
                w.type('one more')
                w.key('return')
                w.wait_visible('^98%$', 10)
                context_panel()
                w.wait_visible('Nearly full', 6)
                bars = bar_colours('ui-model-context-critical.png')
                assert bars['red'] > 1000 and bars['blue'] < 100 and bars['yellow'] < 100, bars
                w.key('escape')
                w.key('ctrl+q')
                assert app.wait(timeout=10) == 0, log_path.read_text()

            # Idle unloading no longer depends on pre-load being off, and pre-load does not fight it.
            server.controls.clear()
            server.state = 'loaded'
            server.prompt_tokens = 100
            device.write_text(json.dumps(LOADED_DEVICE))
            preferences.write_text('[model]\npreload=true\nidle_unload_seconds=3\n')
            with log_path.open('a') as log:
                app = subprocess.Popen([str(ROOT / 'build/chela'), str(root)], cwd=root, env=env, stdout=log, stderr=log)
                w = harness.window('^Chela$')
                wait_for(lambda: server.controls == ['/models/unload'], 12, 'the idle unload')
                time.sleep(2.5)
                assert server.controls == ['/models/unload'], server.controls
                # The next message brings the model back; after that it may idle out again.
                w.tap('Describe a task or ask a question…')
                w.type('are you back')
                w.key('return')
                w.wait_visible('Status checked.', 10)
                wait_for(lambda: server.controls.count('/models/unload') == 2, 12, 'the second idle unload')
                assert '/models/load' not in server.controls, server.controls
                w.key('ctrl+q')
                assert app.wait(timeout=10) == 0, log_path.read_text()
        print('Model status desktop checks passed: pre-load at startup and its switch, migration from keep-loaded, measured '
              'load progress and ETA, device, memory and live GPU readings in Settings, context use in the server\'s numbers, '
              'idle unload independent of pre-load, load estimate, manual unload, failed load and retry.')
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
