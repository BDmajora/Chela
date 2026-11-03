"""Verify inference activity in the real desktop against a controlled loopback API.

The fixture server holds each stage (loading, prefill, thinking, generation) until
the interface has shown the previous one. Uses the headless backend and isolated
settings; no inference hardware or SSH credentials are accessed. Screenshots are
saved in build/.
"""
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from headless import BUILD, ROOT, Harness, wait_for


class Handler(BaseHTTPRequestHandler):
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
        try:
            if self.path == '/props':
                self.send_json({})
                return
            assert self.path == '/v1/models', self.path
            self.server.gets += 1
            self.send_json({'data': [{'id': 'monitor-model', 'status': {'value': self.server.model_state}}]})
        except (BrokenPipeError, ConnectionResetError):
            pass

    def event(self, payload):
        self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
        self.wfile.flush()

    def chunk(self, delta, finish=None, **extra):
        self.event({'choices': [{'delta': delta, 'finish_reason': finish}], **extra})

    def do_POST(self):
        try:
            assert self.path == '/v1/chat/completions', self.path
            payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
            prompt = next(m['content'] for m in reversed(payload['messages']) if m['role'] == 'user')
            self.server.requests.append(payload)
            attempt = self.server.attempts.get(prompt, 0)
            self.server.attempts[prompt] = attempt + 1
            if prompt == 'http failure':
                self.send_json({'error': {'message': 'Fixture model process exited'}}, 503)
                return
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.end_headers()
            if prompt == 'show the stages':
                self.server.model_state = 'loading'
                self.server.started.set()
                assert self.server.prefill.wait(20), 'Prefill was never released'
                self.server.model_state = 'loaded'
                self.event({'prompt_progress': {'total': 20, 'processed': 10}})
                assert self.server.thinking.wait(20), 'Thinking was never released'
                self.chunk({'reasoning_content': 'Checking the answer.'},
                           timings={'predicted_n': 8, 'prompt_n': 20, 'predicted_per_second': 32.0})
                assert self.server.generating.wait(20), 'Generation was never released'
                self.chunk({'content': 'The monitored answer.'},
                           timings={'predicted_n': 12, 'prompt_n': 20, 'predicted_ms': 300.0,
                                    'predicted_per_second': 40.0})
                assert self.server.complete.wait(20), 'Completion was never released'
                self.chunk({}, 'stop', timings={'predicted_n': 12, 'prompt_n': 20, 'predicted_ms': 300.0,
                                                'predicted_per_second': 40.0},
                           usage={'completion_tokens': 12, 'prompt_tokens': 20,
                                  'completion_tokens_details': {'reasoning_tokens': 8}})
            elif prompt == 'empty answer' and attempt == 0:
                self.chunk({'content': ''}, 'stop')
            elif prompt == 'reasoning only':
                self.chunk({'reasoning_content': 'Still thinking.'}, 'length')
            elif prompt == 'disconnect':
                self.chunk({'content': 'A partial answer'})
                return  # A clean HTTP EOF without a finish marker is still an incomplete model reply.
            else:
                self.chunk({'content': 'Recovered successfully.'}, 'stop')
            self.wfile.write(b'data: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.gets, server.model_state, server.requests, server.attempts = 0, 'unloaded', [], {}
    for name in ('started', 'prefill', 'thinking', 'generating', 'complete'):
        setattr(server, name, threading.Event())
    threading.Thread(target=server.serve_forever, daemon=True).start()
    app = harness = None
    log_path = BUILD / 'monitor-ui.log'
    try:
        with tempfile.TemporaryDirectory(prefix='chela-monitor-ui-') as folder:
            root = Path(folder)
            harness = Harness(root)
            config = root / 'config/chela/settings.ini'
            config.parent.mkdir(parents=True)
            config.write_text(f'[connection]\nendpoint=http://127.0.0.1:{server.server_port}/v1\n'
                              'model=monitor-model\nprovider=0\ntools=false\ntimeout=30\n')
            # This test drives loading through a request; model_status_ui.py covers keep-loaded.
            (config.parent / 'desktop.ini').write_text('[model]\nkeep_loaded=false\nidle_unload_seconds=0\n')
            env = {'PATH': os.environ['PATH'], 'HOME': str(root), 'XDG_CONFIG_HOME': str(root / 'config'),
                   'XDG_DATA_HOME': str(root / 'data'), 'XDG_RUNTIME_DIR': str(root), **harness.env}
            with log_path.open('w') as log:
                app = subprocess.Popen([str(ROOT / 'build/chela'), str(root)], cwd=root, env=env,
                                       stdout=log, stderr=log)
                w = harness.window('^Chela$')
                db = root / 'data/chela/sessions.db'

                def saved_answer(expected):
                    if not db.exists():
                        return False
                    with sqlite3.connect(db) as conn:
                        rows = conn.execute('SELECT messages FROM sessions ORDER BY updated DESC').fetchall()
                    if not rows:
                        return False
                    messages = json.loads(rows[0][0])
                    return messages[-1] if messages[-1].get('content') == expected else False

                def submit(text):
                    w.type(text)
                    w.key('return')

                def count(text):
                    return sum(text in line for line in w.texts())

                w.key('ctrl+1')  # Chat layout, matching the user's report.
                submit('show the stages')
                wait_for(server.started.is_set, what='the request')
                w.wait_visible('Loading model')
                w.wait_visible(r'^0:0[1-9]$')
                assert not saved_answer('The monitored answer.')
                w.shot('ui-monitor-loading.png')

                w.key('ctrl+shift+d')
                w.wait_visible('API: reachable')
                assert w.visible('Model: monitor-model') and w.visible('Elapsed:'), w.texts()
                w.shot('ui-monitor-diagnostics-active.png')
                w.key('escape')
                w.wait(lambda: not w.visible('API: reachable'), what='diagnostics to close')

                server.prefill.set()
                w.wait_visible('Processing prompt')
                w.wait_visible(r'Prompt 10 / 20 tokens')
                w.shot('ui-monitor-prefill.png')
                server.thinking.set()
                w.wait_visible('Thinking')
                w.wait_visible(r'32\.0 tok/s')
                w.shot('ui-monitor-thinking.png')
                server.generating.set()
                w.wait_visible('Generating response')
                w.wait_visible(r'40\.0 tok/s')
                assert w.visible('The monitored answer.')
                w.shot('ui-monitor-generating.png')
                server.complete.set()
                answer = wait_for(lambda: saved_answer('The monitored answer.'), what='the saved answer')
                assert answer['_chela_stats']['output_tokens'] == 12, answer
                assert answer['_chela_stats']['tokens_per_second'] == 40, answer
                assert answer['_chela_stats']['reasoning_tokens'] == 8, answer
                assert answer['_chela_stats']['complete'] is True, answer
                w.wait_visible('^Completed$')
                assert count('40.0 tok/s') >= 2, w.texts()
                w.shot('ui-monitor-completed.png')
                assert server.requests[0]['timings_per_token'] is True
                assert server.requests[0]['return_progress'] is True
                assert server.requests[0]['stream_options']['include_usage'] is True

                submit('empty answer')
                w.wait_visible('The model returned no answer or tool calls')
                assert w.visible('Request failed') and w.visible('Retry message'), w.texts()
                w.shot('ui-monitor-empty-error.png')
                w.tap('Retry message')
                wait_for(lambda: saved_answer('Recovered successfully.'), what='the retried answer')
                w.wait_visible('^Completed$')
                assert server.attempts['empty answer'] == 2
                assert all('_chela_stats' not in message for request in server.requests
                           for message in request['messages']), server.requests

                w.tap('Reply')
                submit('reasoning only')
                w.wait_visible('The model produced thinking but no final answer')
                assert w.visible('Retry message')
                w.shot('ui-monitor-thinking-only-error.png')
                w.tap('Reply')
                submit('disconnect')
                w.wait_visible('Server closed an incomplete response')
                assert w.visible('Request failed') and w.visible('Retry message')
                w.shot('ui-monitor-disconnected.png')
                w.tap('Reply')
                submit('http failure')
                w.wait_visible('Fixture model process exited')
                assert w.visible('Request failed') and w.visible('Retry message')
                w.shot('ui-monitor-http-error.png')
                w.key('ctrl+shift+d')
                w.wait_visible('Last request error: HTTP 503')
                assert w.visible('API: reachable')
                w.shot('ui-monitor-diagnostics-error.png')
                w.key('escape', 'ctrl+q')
                assert app.wait(timeout=10) == 0, log_path.read_text()
        print('Inference desktop checks passed: loading/elapsed, live prompt progress, thinking/generation, '
              'server token speed, persisted transcript metrics, active diagnostics, empty/reasoning-only '
              'failures, retry, truncated streams and HTTP errors.')
    finally:
        for name in ('prefill', 'thinking', 'generating', 'complete'):
            getattr(server, name).set()
        if app and app.poll() is None:
            app.terminate()
            app.wait(timeout=5)
        if harness:
            harness.close()
        server.shutdown()


if __name__ == '__main__':
    main()
