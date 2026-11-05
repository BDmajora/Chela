"""Chat and Code as separate places, the folders and branches of a Code session, and reading a transcript.

Drives the real desktop with the headless backend against a loopback model server
that records every request, so what the model was told and offered is checked along
with what the interface shows: Chat is self-contained, a Code session starts blank
and reaches only its folders, branches switch without losing work, a session can run in
a worktree of its own, which can go with the session but never takes work with it (real
git repositories throughout), and retry, scrolling, text selection and Ctrl+A work.
Uses isolated settings; nothing leaves the machine. Screenshots are saved in build/.
"""
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import sqlite3
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from headless import BUILD, Harness

BINARY = Path(os.environ.get('CHELA_BINARY', BUILD / 'chela'))

PARAGRAPHS = [f"Paragraph number {i} talks about one subject at some length." for i in range(1, 41)]
LONG = "\n\n".join(PARAGRAPHS)
# Wide enough to wrap several times, with a word of code on every line it reaches.
WRAPPED = ' '.join(f'Sentence {i} has a `chip{i}` in it and goes on for a good while longer.' for i in range(1, 7))
MARKDOWN = "Before the list.\n\n- first item\n- second item\n\n```c\nint x;\n```\n\n| A | B |\n|---|---|\n| c | d |\n\nAfter the table."


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
        self.send_json({} if self.path.startswith('/props') else {'data': [{'id': 'fixture'}]})

    def answer(self, text):
        self.send_json({'choices': [{'message': {'role': 'assistant', 'content': text}}]})

    def call(self, name, arguments, id='call-1'):
        calls = [{'id': id, 'type': 'function', 'function': {'name': name, 'arguments': json.dumps(arguments)}}]
        self.send_json({'choices': [{'message': {'role': 'assistant', 'tool_calls': calls}}]})

    def do_POST(self):
        payload = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        messages = payload['messages']
        last = messages[-1]
        prompt = next(m['content'] for m in reversed(messages) if m['role'] == 'user')
        self.server.requests.append(payload)
        if prompt == 'long answer':
            self.answer(LONG)
        elif prompt == 'rich answer':
            self.answer(MARKDOWN)
        elif prompt == 'wrapped answer':
            self.answer(WRAPPED)
        elif prompt == 'tool then failure':
            if last['role'] != 'tool':
                self.call('list_files', {'path': '.'})
            elif self.server.broken:
                self.send_json({'error': {'message': 'Fixture model crashed'}}, 503)
            else:
                self.answer('Recovered after the tool.')
        elif prompt.startswith('script '):
            steps = json.loads(prompt[len('script '):])
            done = sum(m['role'] == 'tool' for m in messages)
            if done < len(steps):
                self.call(steps[done][0], steps[done][1], f'call-{done}')
            else:
                results = [m['content'] for m in messages if m['role'] == 'tool']
                self.server.results = results
                self.answer('Script finished.')
        else:
            self.answer(f'Answer to {prompt}')


class App:
    def __init__(self, temp, harness, port, args=(), first_on_path=None):
        self.temp = temp
        config = temp / 'config/chela'
        config.mkdir(parents=True, exist_ok=True)
        settings = config / 'settings.ini'
        if not settings.exists():
            settings.write_text(f'[connection]\nendpoint=http://127.0.0.1:{port}/v1\nmodel=fixture\nprovider=0\ntools=true\n')
            (config / 'desktop.ini').write_text('[sidebar]\ngroup=none\n')
        path = os.environ.get('PATH', '/usr/bin:/bin')
        env = {'PATH': f'{first_on_path}:{path}' if first_on_path else path, 'HOME': str(temp), 'LANG': 'C.UTF-8',
               'XDG_CONFIG_HOME': str(temp / 'config'), 'XDG_DATA_HOME': str(temp / 'data'),
               'XDG_RUNTIME_DIR': str(temp), **harness.env,
               **{k: v for k, v in os.environ.items() if k.endswith('SAN_OPTIONS')}}
        self.process = subprocess.Popen([str(BINARY), *map(str, args)], cwd=temp, env=env,
                                        stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, text=True)
        self.w = harness.window('^Chela$')
        self.w.do('size 1280 800')
        self.w.do('idle')

    def quit(self):
        self.w.key('ctrl+q')
        assert self.process.wait(timeout=10) == 0, self.process.stderr.read()


def submit(w, text):
    w.type(text)
    w.key('return')


def bypass_permissions(w):
    """Chooses it, enabling it first the one time that is needed."""
    w.key('ctrl+m')
    w.tap('Bypass permissions')
    if w.visible('Enable Bypass permissions'):
        w.tap('Enable')


def clipboard(w):
    return w.do('clipboard-get')[0]


def copied(w):
    """What Ctrl+C puts on the clipboard, which starts out holding a marker."""
    w.do('copy-text __empty__')
    w.key('ctrl+c')
    return clipboard(w)


def sessions(temp):
    with sqlite3.connect(temp / 'data/chela/sessions.db') as conn:
        return conn.execute('SELECT title, project, code, folders, messages FROM sessions s '
                            'JOIN session_options o ON o.id = s.id ORDER BY updated DESC').fetchall()


def chat_is_self_contained(w, server, project):
    server.requests.clear()
    w.key('ctrl+1')
    w.wait_visible('How can I help you today')
    assert not w.visible('Open folder') and not w.visible('Select folder'), w.texts()
    w.key('ctrl+shift+a')
    w.wait_visible('Add files or photos')
    assert not w.visible('Add folder') and not w.visible('Skills') and not w.visible('Add to project'), w.texts()
    w.shot('ui-workspace-chat-menu.png')
    w.key('escape')
    w.key('ctrl+shift+p')
    assert not w.visible('Open folder'), 'Chat has no folder menu'
    submit(w, 'hello chat')
    w.wait_visible('Answer to hello chat')
    request = server.requests[-1]
    assert 'tools' not in request, 'Chat offers no project tools'
    system = request['messages'][0]['content']
    assert 'self-contained' in system and str(project) not in system, system
    w.do('idle')


def tabs_are_separate(w, temp, project):
    # Code knows nothing of the chat, and returns to its own session and draft.
    w.key('ctrl+2')
    w.do('idle')
    assert not w.visible('hello chat'), w.texts()
    submit(w, 'hello code')
    w.wait_visible('Answer to hello code')
    w.type('unsent code draft')
    w.key('ctrl+1')
    w.wait_visible('Answer to hello chat')
    assert not w.visible('hello code') and not w.visible('unsent code draft'), w.texts()
    w.type('unsent chat draft')
    w.key('ctrl+2')
    w.wait_visible('Answer to hello code')
    assert w.visible('unsent code draft') and not w.visible('hello chat') and not w.visible('unsent chat draft'), w.texts()
    w.key('ctrl+a', 'backspace')
    w.key('ctrl+1')
    w.wait_visible('unsent chat draft')
    w.key('ctrl+a', 'backspace')
    w.key('ctrl+k')
    w.type('hello code')
    w.do('idle')
    assert w.texts().count('hello code') == 1, 'Search stays in its own tab: only the search box shows it'
    w.key('escape')
    w.key('ctrl+2')
    w.do('idle')
    saved = {row[0]: row for row in sessions(temp)}
    assert saved['hello chat'][2] == 0 and saved['hello chat'][1] == '', saved['hello chat']
    assert saved['hello code'][2] == 1 and saved['hello code'][1] == str(project), saved['hello code']


def code_starts_blank(w, server):
    w.wait_visible('Open folder')
    assert w.visible('Select folder'), w.texts()
    w.shot('ui-workspace-blank.png')
    w.key('ctrl+shift+a')
    w.wait_visible('Add folder')
    assert not w.visible('Skills'), 'Skills come from a folder'
    w.shot('ui-workspace-plus-menu.png')
    w.key('escape')
    server.requests.clear()
    submit(w, 'no folder task')
    w.wait_visible('Answer to no folder task')
    request = server.requests[-1]
    assert 'tools' not in request, 'No folder means no project tools'
    assert 'No folder is open' in request['messages'][0]['content'], request['messages'][0]
    w.do('idle')
    assert w.visible('No folder'), w.texts()
    w.key('ctrl+n')


def choose_folder(w, path, browsing=None):
    """Goes to a folder in the app's own folder chooser and accepts it."""
    w.wait_visible('Choose folder')
    w.tap(str(browsing or path.parent))  # the path field, which shows the folder being browsed
    w.key('ctrl+a')
    w.type(str(path))
    w.key('return')
    w.do('idle')
    w.tap('Choose folder')
    w.wait(lambda: not w.visible('^Choose folder$'), what='the folder chooser to close')


def add_folder_from_plus(w, path, browsing=None):
    w.key('ctrl+shift+a')
    w.tap('Add folder')
    choose_folder(w, path, browsing)


def folders_reach_only_what_was_added(w, server, temp, project, other, third, outside):
    # The first folder added becomes the working folder; the next ones are included.
    w.do('idle')
    w.tap('Open folder…')
    choose_folder(w, project)
    w.wait_visible(project.name)
    add_folder_from_plus(w, other)
    w.wait_visible(f'{project.name} \\+ 1')
    add_folder_from_plus(w, third)
    w.wait_visible(f'{project.name} \\+ 2')
    w.key('ctrl+shift+p')
    w.wait_visible('Also included')
    assert w.visible('No folder') and w.visible('Recent') and w.visible(other.name) and w.visible(third.name), w.texts()
    w.shot('ui-workspace-folder-menu.png')
    w.key('escape')

    steps = [['write_file', {'path': 'local.txt', 'content': 'in the project'}],
             ['read_file', {'path': str(other / 'shared.h')}],
             ['write_file', {'path': str(other / 'ported.c'), 'content': 'int ported;'}],
             ['list_files', {'path': str(other)}],
             ['run_command', {'command': 'pwd', 'cwd': str(third)}],
             ['read_file', {'path': str(outside / 'secret.txt')}],
             ['write_file', {'path': str(outside / 'written.txt'), 'content': 'no'}],
             ['run_command', {'command': 'pwd', 'cwd': str(outside)}],
             ['read_file', {'path': str(other / '..' / outside.name / 'secret.txt')}],
             ['read_file', {'path': str(other / 'link' / 'secret.txt')}]]
    bypass_permissions(w)
    server.requests.clear()
    submit(w, 'script ' + json.dumps(steps))
    w.wait_visible('Script finished.')
    request = server.requests[0]
    system = request['messages'][0]['content']
    assert str(project) in system and str(other) in system and str(third) in system, system
    assert str(outside) not in system
    assert {t['function']['name'] for t in request['tools']} == {'list_files', 'read_file', 'write_file', 'run_command'}
    r = server.results
    assert (project / 'local.txt').read_text() == 'in the project'
    assert r[1] == '#define SHARED 1\n', r[1]
    assert (other / 'ported.c').read_text() == 'int ported;'
    assert 'shared.h' in r[3] and 'ported.c' in r[3], r[3]
    assert str(third) in r[4], r[4]
    for result in r[5:8]:
        assert result.startswith('Tool error:') and 'outside the folders' in result, result
    assert r[8].startswith('Tool error:'), r[8]
    assert r[9].startswith('Tool error:'), r[9]
    assert not (outside / 'written.txt').exists()

    # The folders belong to the session: they survive a restart and come back with it.
    saved = sessions(temp)[0]
    assert saved[1] == str(project) and json.loads(saved[3]) == [str(other), str(third)], saved[:4]


def folders_come_back(harness, server, temp, project, other, third):
    app = App(temp, harness, server.server_port)
    w = app.w
    w.wait_visible('Select folder')
    w.tap('script ')
    w.wait_visible('Script finished.')
    w.wait_visible('Also included')
    assert w.visible(other.name) and w.visible(third.name), w.texts()
    w.shot('ui-workspace-panel.png')
    w.do('size 900 800')  # the panel folds away; the footer says the same
    w.do('idle')
    w.wait_visible(f'{project.name} \\+ 2')
    w.key('ctrl+shift+p')
    w.wait_visible('Also included')
    w.tap('Remove from this session')
    w.wait_visible(f'{project.name} \\+ 1')
    w.key('escape')
    w.do('size 1280 800')
    server.requests.clear()
    submit(w, 'after removing')
    w.wait_visible('Answer to after removing')
    system = server.requests[-1]['messages'][0]['content']
    assert str(other) not in system and str(third) in system, system
    assert json.loads(sessions(temp)[0][3]) == [str(third)]
    app.quit()


GIT_ENV = {**os.environ, 'GIT_CONFIG_GLOBAL': os.devnull, 'GIT_CONFIG_NOSYSTEM': '1'}
ODD_BRANCH = 'odd;touch${IFS}pwned'  # a shell given this name unquoted would create "pwned"


def git(folder, *args):
    return subprocess.run(['git', *args], cwd=folder, env=GIT_ENV, check=True, capture_output=True, text=True).stdout.strip()


def repository(path):
    path.mkdir()
    git(path, 'init', '--quiet', '--initial-branch=main')
    git(path, 'config', 'user.name', 'Chela Test')
    git(path, 'config', 'user.email', 'test@example.invalid')
    (path / 'tracked.txt').write_text('first\n')
    (path / 'sub').mkdir()
    (path / 'sub/inner.txt').write_text('inner\n')
    git(path, 'add', '.')
    git(path, 'commit', '--quiet', '-m', 'First')
    for branch in ('feature/side', ODD_BRANCH, 'same-name'):
        git(path, 'branch', branch)
    git(path, 'tag', 'same-name')  # "refname:short" would call this branch heads/same-name


def branch_of(repo):
    return git(repo, 'branch', '--show-current')


def session_worktrees(temp):
    return [p for p in (temp / 'data/chela/worktrees').glob('*/*')]


def open_branches(w):
    w.key('ctrl+shift+p')
    w.wait_visible('^Branch$')
    w.tap('Branch')
    w.wait_visible('^Switch branch$')


def close_menus(w):
    w.key('escape', 'escape')
    w.wait(lambda: not w.visible('^Branch$'), what='the folder menu to close')


def switch_to(w, branch):
    open_branches(w)
    w.wait_visible(f'^{re.escape(branch)}$')
    w.tap(branch)


def no_repository_no_controls(w, project):
    w.wait_visible(project.name)
    w.do('wait 300')  # long enough for git to have said this is not a repository
    assert not w.visible('^Worktree$'), w.texts()
    w.key('ctrl+shift+p')
    w.wait_visible('Also included')
    w.do('wait 300')
    assert not w.visible('^Branch$'), w.texts()
    w.key('escape')


def switching_branches(w, temp, repo):
    w.wait_visible('^Worktree$')
    w.key('ctrl+1')
    w.do('idle')
    assert not w.visible('^Worktree$'), 'Chat has no folders, branches or worktrees'
    w.key('ctrl+2')
    w.wait_visible('^Worktree$')
    w.shot('ui-workspace-worktree-box.png')
    open_branches(w)
    w.wait_visible('^feature/side$')
    assert w.texts().count('main') >= 2, 'The row names the current branch, and the list marks it'
    w.shot('ui-workspace-branch-menu.png')
    w.tap('feature/side')
    w.wait_visible('Switched to feature/side')
    assert branch_of(repo) == 'feature/side'
    w.key('ctrl+shift+p')
    w.wait_visible('^feature/side$')
    w.key('escape')

    # What a branch is called never reaches a shell, and a tag of the same name is no trouble.
    switch_to(w, ODD_BRANCH)
    w.wait_visible('Switched to odd;')
    assert branch_of(repo) == ODD_BRANCH and not (repo / 'pwned').exists()
    switch_to(w, 'same-name')
    w.wait_visible('Switched to same-name')
    assert branch_of(repo) == 'same-name'

    # Uncommitted work stops a switch, however git would have treated it.
    tracked = repo / 'tracked.txt'
    tracked.write_text('edited\n')
    switch_to(w, 'main')
    w.wait_visible('Commit or stash your changes')
    w.shot('ui-workspace-branch-refused.png')
    assert branch_of(repo) == 'same-name' and tracked.read_text() == 'edited\n'
    git(repo, 'add', 'tracked.txt')
    w.tap('Dismiss')
    switch_to(w, 'main')
    w.wait_visible('Commit or stash your changes')
    assert branch_of(repo) == 'same-name'
    git(repo, 'reset', '--quiet', '--hard')
    new = repo / 'new.txt'
    new.write_text('not committed\n')
    w.tap('Dismiss')
    switch_to(w, 'main')
    w.wait_visible('Commit or stash your changes')
    assert branch_of(repo) == 'same-name' and new.exists()
    new.unlink()
    w.tap('Dismiss')
    switch_to(w, 'main')
    w.wait_visible('Switched to main')

    # A file git would refuse to replace is shown as git says it, and nothing moves.
    git(repo, 'switch', '--quiet', 'feature/side')
    (repo / 'ignored.txt').write_text('tracked over there\n')
    git(repo, 'add', 'ignored.txt')
    git(repo, 'commit', '--quiet', '-m', 'Second')
    git(repo, 'switch', '--quiet', 'main')
    (repo / '.git/info/exclude').write_text('ignored.txt\n')
    (repo / 'ignored.txt').write_text('ignored here, and precious\n')
    switch_to(w, 'feature/side')
    w.wait_visible('Could not switch branch')
    w.shot('ui-workspace-branch-failed.png')
    w.tap('Close')
    assert branch_of(repo) == 'main' and (repo / 'ignored.txt').read_text() == 'ignored here, and precious\n'
    (repo / 'ignored.txt').unlink()
    (repo / '.git/info/exclude').write_text('')

    # A branch checked out in another worktree can't be taken from it.
    elsewhere = temp / 'checked-out-elsewhere'
    git(repo, 'worktree', 'add', '--quiet', '-b', 'taken', str(elsewhere))
    open_branches(w)
    w.wait_visible('^taken$')
    w.tap('taken')
    w.do('idle')
    assert branch_of(repo) == 'main' and w.visible('^taken$'), 'The row is disabled, so the menu stays open'
    close_menus(w)
    git(repo, 'worktree', 'remove', str(elsewhere))
    git(repo, 'branch', '-D', 'taken')

    # A detached HEAD goes by its commit, and the way back is the same.
    git(repo, 'checkout', '--quiet', '--detach')
    commit = git(repo, 'rev-parse', '--short', 'HEAD')
    w.key('ctrl+shift+p')
    w.wait_visible(f'^{commit}$')
    w.key('escape')
    switch_to(w, 'main')
    w.wait_visible('Switched to main')

    # The keyboard reaches all of it.
    w.key('ctrl+shift+p')
    w.wait_visible('^Branch$')
    w.key('end', 'right')
    w.wait_visible('^Switch branch$')
    w.key('down', 'return')
    w.wait_visible('Switched to ')
    assert branch_of(repo) != 'main'
    git(repo, 'switch', '--quiet', 'main')

    # However long a name is, the list opens beside its row: under the pointer, it would take the click.
    long_name = 'feature/' + 'a-branch-with-a-very-long-name-' * 4 + 'end'
    git(repo, 'branch', long_name)
    w.do('size 860 600')
    open_branches(w)
    w.wait_visible(f'^{re.escape(long_name)}$')
    w.shot('ui-workspace-branch-long.png')
    assert branch_of(repo) == 'main', 'Opening the list chose nothing'
    x, _, width, _ = map(float, w.do(f'place {long_name}')[0].split())
    row, _, _, _ = map(float, w.do('place Branch')[0].split())
    assert width <= 320 and (x + width < row or x > row), (x, width, row)
    close_menus(w)
    git(repo, 'branch', '-D', long_name)
    w.do('size 1280 800')

    # Hundreds of branches: the list scrolls, follows the keyboard, and says what it left out.
    head = git(repo, 'rev-parse', 'HEAD')
    for i in range(80):
        (repo / f'.git/refs/heads/many-{i:03}').write_text(head + '\n')
    order = git(repo, 'for-each-ref', '--sort=-committerdate', '--format=%(refname:lstrip=2)', 'refs/heads').split('\n')
    order.remove('main')
    w.key('ctrl+shift+p')
    w.wait_visible('^Branch$')
    w.key('end', 'right')
    w.wait_visible(f'^{re.escape(order[0])}$')
    first = w.do(f'place {order[0]}')[0]
    w.shot('ui-workspace-branch-many.png')
    w.key('end')
    w.do('idle')
    last = order[61]  # the 64 rows the menu can hold: main, these, and the one saying how many are left
    y = float(w.do(f'place {last}')[0].split()[1])
    assert 0 < y < 800, (last, y)
    assert w.visible(f'^{len(order) - 62} older branches$'), w.texts()
    assert not w.visible(f'^{re.escape(order[0])}$') or w.do(f'place {order[0]}')[0] != first, 'The list moved'
    close_menus(w)
    for i in range(80):
        git(repo, 'branch', '--quiet', '-D', f'many-{i:03}')


def worktree_session(harness, server, temp, repo):
    app = App(temp, harness, server.server_port, [repo])
    w = app.w
    start = git(repo, 'rev-parse', 'HEAD')
    w.tap('Worktree')
    w.shot('ui-workspace-worktree-ticked.png')
    bypass_permissions(w)
    server.requests.clear()
    steps = [['write_file', {'path': 'made-there.txt', 'content': 'in the worktree'}],
             ['run_command', {'command': 'pwd; git branch --show-current; git add made-there.txt && '
                                         'git commit --quiet -m Made && git log --format=%s -1'}]]
    submit(w, 'script ' + json.dumps(steps))
    w.wait_visible('Script finished.')

    # The session lives there, on its own branch, and the repository did not move.
    title, saved, code, folders, _ = sessions(temp)[0]
    folder = Path(saved)
    assert code == 1 and json.loads(folders) == [], (saved, folders)
    assert folder.parent.parent == temp / 'data/chela/worktrees' and folder.name == repo.name, folder
    assert str(folder) in git(repo, 'worktree', 'list')
    system = server.requests[0]['messages'][0]['content']
    assert str(folder) in system and str(repo) not in system, system
    made, branch, committed = server.results[1].split('\n')[:3]
    assert made == str(folder) and branch.startswith('chela/') and committed == 'Made', server.results[1]
    assert (folder / 'made-there.txt').read_text() == 'in the worktree' and not (repo / 'made-there.txt').exists()
    assert branch_of(repo) == 'main' and git(repo, 'rev-parse', 'HEAD') == start
    assert git(repo, 'status', '--porcelain') == '', 'Nothing of it shows in the repository'
    w.wait_visible(branch)
    w.wait_visible(f'{repo.name} · worktree')
    w.do('idle')
    assert not w.visible('^Worktree$'), 'A session that has begun stays where it is'
    w.shot('ui-workspace-worktree-session.png')
    w.do('size 900 800')  # the panel folds away, and the folder menu says the same
    w.key('ctrl+shift+p')
    w.wait_visible(f'^{re.escape(branch)}$')
    w.key('escape')
    w.do('size 1280 800')
    app.quit()

    # It is saved like any other folder: it comes back, and the next message stays in it.
    app = App(temp, harness, server.server_port)
    w = app.w
    w.key('ctrl+shift+p')
    w.wait_visible('^Recent$')
    texts = w.texts()
    assert str(repo) in texts and texts.count(str(folder)) == 1, 'Recent has the repository; only the sidebar has the worktree'
    w.key('escape')
    git(repo, 'worktree', 'repair', '--relative-paths', str(folder))  # as worktree.useRelativePaths would write it
    assert not (folder / '.git').read_text().startswith('gitdir: /')
    w.key('ctrl+shift+p')
    w.wait_visible('^Recent$')
    assert str(repo) in w.texts(), 'A relative link leads to the same place'
    w.key('escape')
    w.tap('script ')
    w.wait_visible('Script finished.')
    w.wait_visible(branch)
    server.requests.clear()
    submit(w, 'still in the worktree')
    w.wait_visible('Answer to still in the worktree')
    assert str(folder) in server.requests[-1]['messages'][0]['content']
    assert sessions(temp)[0][1] == str(folder) and len(session_worktrees(temp)) == 1
    app.quit()

    # From inside the repository, the session is in the same place inside the worktree.
    app = App(temp, harness, server.server_port, [repo / 'sub'])
    w = app.w
    w.wait_visible('^Worktree$')
    w.tap('Worktree')
    submit(w, 'from the subfolder')
    w.wait_visible('Answer to from the subfolder')
    inner = Path(sessions(temp)[0][1])
    assert inner.name == 'sub' and (inner / 'inner.txt').exists() and inner.parent.name == repo.name, inner
    assert len(session_worktrees(temp)) == 2

    # The box stays ticked for the next session, and a failure keeps the draft.
    w.key('ctrl+n')
    w.wait_visible('^Worktree$')
    root = temp / 'data/chela/worktrees'
    root.rename(temp / 'data/chela/worktrees-aside')
    root.write_text('in the way')
    count = len(sessions(temp))
    submit(w, 'cannot begin yet')
    w.wait_visible('Could not create the worktree')
    w.shot('ui-workspace-worktree-failed.png')
    w.tap('Close')
    w.wait_visible('cannot begin yet')
    assert len(sessions(temp)) == count
    root.unlink()
    (temp / 'data/chela/worktrees-aside').rename(root)

    # Stop ends it whenever it is pressed, leaving nothing behind and the draft in place.
    branches = git(repo, 'branch', '--list', 'chela/*')
    hook = repo / '.git/hooks/post-checkout'
    hook.write_text('#!/bin/sh\nsleep 30\n')
    hook.chmod(0o755)
    w.key('return')
    w.wait_visible('Creating worktree')
    w.shot('ui-workspace-worktree-creating.png')
    w.key('escape')
    w.wait(lambda: not w.visible('Creating worktree|Stopping'), what='the worktree to stop')
    w.wait_visible('cannot begin yet')
    assert len(session_worktrees(temp)) == 2 and len(sessions(temp)) == count
    assert git(repo, 'branch', '--list', 'chela/*') == branches
    assert len(git(repo, 'worktree', 'list').split('\n')) == 3
    assert not any(p for p in root.iterdir() if not any(p.iterdir())), 'No empty folders either'

    # Closing Chela does the same, and then quits.
    w.key('return')
    w.wait_visible('Creating worktree')
    app.quit()
    hook.unlink()
    assert len(session_worktrees(temp)) == 2 and git(repo, 'branch', '--list', 'chela/*') == branches


def worktree_commits_in_the_sandbox(harness, server):
    # Temporary folders are writable from any sandbox, so this repository sits beside the build.
    with tempfile.TemporaryDirectory(dir=BUILD, prefix='worktree-') as folder:
        temp = Path(folder).resolve()
        if str(temp).startswith(('/tmp/', '/var/tmp/')):
            print('# built in a temporary folder: the sandbox limits of a worktree are not tested')
            return
        repo = temp / 'repo-beside-the-build'
        repository(repo)
        app = App(temp, harness, server.server_port, [repo])
        w = app.w
        w.wait_visible('^Worktree$')
        w.tap('Worktree')
        bypass_permissions(w)
        out_of_reach = temp / 'out-of-reach.txt'
        steps = [['run_command', {'command': f'touch {shlex.quote(str(out_of_reach))}'}],
                 ['run_command', {'command': 'git commit --quiet --allow-empty -m Made && git log --format=%s -1'}]]
        submit(w, 'script ' + json.dumps(steps))
        w.wait_visible('Script finished.')
        app.quit()
        denied, committed = server.results
        if 'cannot be confined' in denied:
            print('# no Landlock here: the sandbox limits of a worktree are not tested')
            return
        assert 'Permission denied' in denied and not out_of_reach.exists(), denied
        assert committed.startswith('Made'), committed
        branch = next(b for b in git(repo, 'branch', '--format=%(refname:short)').split('\n') if b.startswith('chela/'))
        assert git(repo, 'log', '--format=%s', '-1', branch) == 'Made'
        assert git(repo, 'log', '--format=%s', '-1', 'main') == 'First', 'The commit stayed on the worktree branch'


def begin_in_worktree(w, temp, prompt):
    """A new session in a worktree of its own, with the box ticked: its folder and branch."""
    w.key('ctrl+n')
    w.wait_visible('^Worktree$')
    submit(w, prompt)
    w.wait_visible(f'Answer to {prompt}')
    folder = next(Path(s[1]) for s in sessions(temp) if s[0] == prompt)
    return folder, f'chela/{folder.parent.name}'


def session_titles(temp):
    return [s[0] for s in sessions(temp)]


def local_branches(repo):
    return git(repo, 'branch', '--format=%(refname:short)').split('\n')


def open_delete(w, title):
    w.wait_visible(f'^{re.escape(title)}$')
    w.do(f'rtap {title}')
    w.tap('Delete…')
    w.wait_visible('Delete this session')


def confirm_delete(w):
    w.tap('Delete')
    w.wait(lambda: not w.visible('Delete this session'), what='the confirmation to close')


def delete_session(w, title):
    open_delete(w, title)
    confirm_delete(w)


def not_offered(w, title, why):
    open_delete(w, title)
    assert not w.visible('Also remove the worktree'), why
    w.tap('Cancel')
    w.wait(lambda: not w.visible('Delete this session'), what='the confirmation to close')


def session_gone(w, temp, title):
    w.wait(lambda: title not in session_titles(temp), what=f'{title} to be deleted')


def refused(w, temp, title, folder, branch, reason):
    """Deleting with the box ticked stops at a notice, and leaves all three where they were."""
    delete_session(w, title)
    w.wait_visible(reason)
    w.tap('Close')
    w.wait(lambda: not w.visible(reason), what='the notice to close')
    assert title in session_titles(temp) and folder.is_dir() and branch in local_branches(repo_of(folder))
    w.wait_visible(f'Answer to {title}')


def repo_of(worktree):
    return Path(git(worktree, 'rev-parse', '--path-format=absolute', '--git-common-dir')).parent


def removing_worktrees(harness, server):
    with tempfile.TemporaryDirectory() as folder:
        temp = Path(folder).resolve()
        repo = temp / 'repo-to-clean'
        repository(repo)
        (repo / '.gitignore').write_text('*.env\n')
        git(repo, 'add', '.')
        git(repo, 'commit', '--quiet', '-m', 'Ignore')
        app = App(temp, harness, server.server_port, [repo])
        w = app.w
        w.wait_visible('^Worktree$')
        w.tap('Worktree')

        # The confirmation offers it, ticked, and says what goes. Cancel changes nothing.
        folder, branch = begin_in_worktree(w, temp, 'clean one')
        open_delete(w, 'clean one')
        w.wait_visible('Also remove the worktree')
        w.wait_visible('Files that git ignores go with it')
        w.shot('ui-workspace-worktree-delete.png')
        w.tap('Cancel')
        w.wait(lambda: not w.visible('Delete this session'), what='the confirmation to close')
        assert folder.is_dir() and 'clean one' in session_titles(temp)

        # Nothing of a clean one is left, ignored files included, and the repository is back.
        (folder / 'local.env').write_text('made in the session')
        delete_session(w, 'clean one')
        session_gone(w, temp, 'clean one')
        w.wait_visible('^Worktree$')
        assert not folder.parent.exists(), 'Its own folder under worktrees goes too'
        assert branch not in local_branches(repo) and str(folder) not in git(repo, 'worktree', 'list')
        assert git(repo, 'status', '--porcelain') == ''

        # Unticked, only the session goes.
        folder, branch = begin_in_worktree(w, temp, 'folder stays')
        open_delete(w, 'folder stays')
        w.tap('Also remove the worktree')
        confirm_delete(w)
        session_gone(w, temp, 'folder stays')
        assert folder.is_dir() and branch in local_branches(repo) and str(folder) in git(repo, 'worktree', 'list')

        # Uncommitted work keeps the session and everything else, new files and changes alike.
        folder, branch = begin_in_worktree(w, temp, 'has changes')
        (folder / 'new.txt').write_text('not committed')
        refused(w, temp, 'has changes', folder, branch, 'The worktree has uncommitted changes')
        assert (folder / 'new.txt').read_text() == 'not committed'
        (folder / 'tracked.txt').write_text('changed\n')
        (folder / 'new.txt').unlink()
        open_delete(w, 'has changes')
        confirm_delete(w)
        w.wait_visible('The worktree has uncommitted changes')
        w.shot('ui-workspace-worktree-refused.png')
        w.tap('Close')
        assert (folder / 'tracked.txt').read_text() == 'changed\n'

        # Committed, it is the branch that holds the work: it stays, and can be chosen now.
        git(folder, 'add', '.')
        git(folder, 'commit', '--quiet', '-m', 'Only here')
        only_here = git(folder, 'rev-parse', 'HEAD')
        delete_session(w, 'has changes')
        w.wait_visible(f'Kept branch {re.escape(branch)}')
        w.shot('ui-workspace-worktree-branch-kept.png')
        session_gone(w, temp, 'has changes')
        assert not folder.parent.exists() and git(repo, 'rev-parse', branch) == only_here
        switch_to(w, branch)
        w.wait(lambda: branch_of(repo) == branch, what='the kept branch to be chosen')
        switch_to(w, 'main')
        w.wait(lambda: branch_of(repo) == 'main', what='the repository to return to main')

        # A branch whose commits are on another one goes, though that is not the current branch.
        folder, branch = begin_in_worktree(w, temp, 'merged elsewhere')
        git(folder, 'commit', '--quiet', '--allow-empty', '-m', 'On two branches')
        git(repo, 'branch', 'holds-it', branch)
        delete_session(w, 'merged elsewhere')
        session_gone(w, temp, 'merged elsewhere')
        assert branch not in local_branches(repo) and git(repo, 'log', '--format=%s', '-1', 'holds-it') == 'On two branches'

        # A branch in use elsewhere stays for that reason, and the note does not say otherwise.
        folder, branch = begin_in_worktree(w, temp, 'moved on')
        git(folder, 'switch', '--quiet', '-c', 'moved-on')
        git(repo, 'switch', '--quiet', branch)
        delete_session(w, 'moved on')
        w.wait_visible(f'Kept branch {re.escape(branch)}: .*used by worktree')
        session_gone(w, temp, 'moved on')
        assert not folder.exists() and {branch, 'moved-on'} <= set(local_branches(repo))
        git(repo, 'switch', '--quiet', 'main')

        # git alone would drop these commits, as nothing but the worktree's HEAD holds them.
        folder, branch = begin_in_worktree(w, temp, 'detached')
        git(folder, 'checkout', '--quiet', '--detach')
        git(folder, 'commit', '--quiet', '--allow-empty', '-m', 'On no branch')
        loose = git(folder, 'rev-parse', 'HEAD')
        refused(w, temp, 'detached', folder, branch, 'The worktree has commits on no branch')
        git(folder, 'branch', 'saved-it')
        delete_session(w, 'detached')
        session_gone(w, temp, 'detached')
        assert not folder.exists() and git(repo, 'rev-parse', 'saved-it') == loose

        # What git refuses for a reason of its own keeps everything too, and is passed on.
        folder, branch = begin_in_worktree(w, temp, 'locked')
        git(repo, 'worktree', 'lock', str(folder))
        refused(w, temp, 'locked', folder, branch, 'Could not remove the worktree')
        git(repo, 'worktree', 'unlock', str(folder))
        delete_session(w, 'locked')
        session_gone(w, temp, 'locked')
        assert not folder.exists()

        # A chat says nothing of the worktree the Code tab happens to be in.
        folder, branch = begin_in_worktree(w, temp, 'code side')
        w.key('ctrl+1')
        w.wait_visible('How can I help you today')
        submit(w, 'chat side')
        w.wait_visible('Answer to chat side')
        open_delete(w, 'chat side')
        assert not w.visible('Also remove the worktree'), w.texts()
        confirm_delete(w)
        session_gone(w, temp, 'chat side')
        w.key('ctrl+2')
        assert folder.is_dir()

        # Nor is a worktree offered while another session has it as an extra folder.
        w.wait_visible('^code side$')
        w.key('ctrl+n')
        w.wait_visible('^Worktree$')
        w.tap('Worktree')
        submit(w, 'reaches it')
        w.wait_visible('Answer to reaches it')
        add_folder_from_plus(w, folder, repo)
        w.wait(lambda: str(folder) in json.loads(next(s[3] for s in sessions(temp) if s[0] == 'reaches it')),
               what='the folder to be added')
        not_offered(w, 'code side', 'Another session has it as an extra folder')
        delete_session(w, 'reaches it')
        session_gone(w, temp, 'reaches it')
        delete_session(w, 'code side')
        session_gone(w, temp, 'code side')
        assert not folder.exists() and branch not in local_branches(repo)

        # A folder that has already gone leaves just the session to delete.
        w.wait_visible('^Worktree$')
        w.tap('Worktree')
        folder, branch = begin_in_worktree(w, temp, 'gone by hand')
        shutil.rmtree(folder)
        open_delete(w, 'gone by hand')
        assert not w.visible('Also remove the worktree'), w.texts()
        confirm_delete(w)
        session_gone(w, temp, 'gone by hand')

        # Nor if it goes while the confirmation is open, box ticked.
        folder, branch = begin_in_worktree(w, temp, 'gone meanwhile')
        open_delete(w, 'gone meanwhile')
        w.wait_visible('Also remove the worktree')
        shutil.rmtree(folder)
        confirm_delete(w)
        session_gone(w, temp, 'gone meanwhile')

        # A chat has no worktree, even with no session left to claim the one the Code tab is in,
        # as when a second window has deleted that session.
        folder, branch = begin_in_worktree(w, temp, 'deleted elsewhere')
        w.key('ctrl+1')
        w.wait_visible('How can I help you today')
        submit(w, 'another chat')
        w.wait_visible('Answer to another chat')
        with sqlite3.connect(temp / 'data/chela/sessions.db') as conn:
            conn.execute("DELETE FROM sessions WHERE title = 'deleted elsewhere'")
        open_delete(w, 'another chat')
        assert not w.visible('Also remove the worktree'), w.texts()
        confirm_delete(w)
        session_gone(w, temp, 'another chat')
        assert folder.is_dir() and branch in local_branches(repo)
        app.quit()

        # Two sessions in one worktree: each keeps it from the other. A window opened there
        # keeps it too, and once that is all that stands in the way, it can go.
        app = App(temp, harness, server.server_port, [repo])
        w = app.w
        w.wait_visible('^Worktree$')
        w.tap('Worktree')
        folder, branch = begin_in_worktree(w, temp, 'first there')
        app.quit()
        app = App(temp, harness, server.server_port, [folder])
        w = app.w
        submit(w, 'second there')
        w.wait_visible('Answer to second there')
        assert next(s[1] for s in sessions(temp) if s[0] == 'second there') == str(folder)
        app.quit()
        app = App(temp, harness, server.server_port, [repo])
        w = app.w
        not_offered(w, 'first there', 'Another session runs there')
        not_offered(w, 'second there', 'Another session runs there')
        delete_session(w, 'second there')
        session_gone(w, temp, 'second there')
        assert folder.is_dir()
        app.quit()
        app = App(temp, harness, server.server_port, [folder])
        w = app.w
        not_offered(w, 'first there', 'New sessions of this window start there')
        app.quit()
        app = App(temp, harness, server.server_port, [repo])
        w = app.w
        open_delete(w, 'first there')
        w.wait_visible('Also remove the worktree')
        confirm_delete(w)
        session_gone(w, temp, 'first there')
        assert not folder.exists() and branch not in local_branches(repo)
        app.quit()

        # From a folder inside the worktree, all of the worktree goes.
        app = App(temp, harness, server.server_port, [repo / 'sub'])
        w = app.w
        w.wait_visible('^Worktree$')
        w.tap('Worktree')
        folder, branch = begin_in_worktree(w, temp, 'in a subfolder')
        assert folder.name == 'sub' and (folder / 'inner.txt').exists()
        delete_session(w, 'in a subfolder')
        session_gone(w, temp, 'in a subfolder')
        assert not folder.parent.parent.exists() and branch not in local_branches(repo)
        app.quit()

        # It runs in the background, and neither Stop nor closing leaves it half done.
        tools = temp / 'tools'
        tools.mkdir()
        gate = temp / 'gate'
        (tools / 'git').write_text(f"""#!/bin/sh
if [ "$1 $2" = "worktree remove" ]; then
    while [ ! -e {shlex.quote(str(gate))} ]; do sleep 0.02; done
fi
exec {shlex.quote(shutil.which('git'))} "$@"
""")
        (tools / 'git').chmod(0o755)
        app = App(temp, harness, server.server_port, [repo], first_on_path=tools)
        w = app.w
        w.wait_visible('^Worktree$')
        w.tap('Worktree')
        folder, branch = begin_in_worktree(w, temp, 'takes a while')
        delete_session(w, 'takes a while')
        w.wait_visible('Removing worktree')
        w.shot('ui-workspace-worktree-removing.png')
        w.key('escape')
        w.do('idle')
        assert w.visible('Removing worktree') and not w.visible('Stopping'), w.texts()
        w.type('typed meanwhile')
        w.key('return')
        w.do('idle')
        assert w.visible('Removing worktree') and folder.is_dir() and 'takes a while' in session_titles(temp)
        gate.touch()
        session_gone(w, temp, 'takes a while')
        w.wait(lambda: not w.visible('Removing worktree'), what='the removal to end')
        assert not folder.exists() and branch not in local_branches(repo)
        gate.unlink()
        folder, branch = begin_in_worktree(w, temp, 'closed while it ran')
        delete_session(w, 'closed while it ran')
        w.wait_visible('Removing worktree')
        w.key('ctrl+q')
        w.do('idle')
        assert app.process.poll() is None, 'Chela waits for it'
        gate.touch()
        assert app.process.wait(timeout=10) == 0, app.process.stderr.read()
        assert not folder.exists() and branch not in local_branches(repo)
        assert 'closed while it ran' not in session_titles(temp)


def retry_after_tools(w, server, temp):
    server.broken = True
    submit(w, 'tool then failure')
    w.wait_visible('Fixture model crashed')
    w.wait_visible('Retry message')
    w.shot('ui-workspace-retry.png')
    server.broken = False
    w.tap('Retry message')
    w.wait_visible('Recovered after the tool.')
    assert not w.visible('Retry message')
    messages = json.loads(sessions(temp)[0][4])
    assert [m['role'] for m in messages[-4:]] == ['user', 'assistant', 'tool', 'assistant'], messages
    assert sum(m.get('content') == 'tool then failure' for m in messages) == 1


def retry_after_restart(harness, server, temp, project):
    app = App(temp, harness, server.server_port, [project])
    w = app.w
    server.broken = True
    submit(w, 'tool then failure')
    w.wait_visible('Retry message')
    app.quit()
    app = App(temp, harness, server.server_port, [project])
    w = app.w
    w.tap('tool then failure')
    w.wait_visible('Retry message')
    server.broken = False
    w.tap('Retry message')
    w.wait_visible('Recovered after the tool.')
    app.quit()


def scrolling(w):
    w.key('ctrl+n')
    submit(w, 'long answer')
    w.wait_visible('Paragraph number 40 ')
    w.do('idle')
    assert not w.visible('Paragraph number 1 ')
    w.do('scroll 640 300 -56')  # a single notch is enough to leave the end
    w.do('idle')
    w.do('scroll 640 300 -600')
    w.do('idle')
    assert not w.visible('Paragraph number 40 '), 'The wheel moves the transcript'
    w.do('scroll 640 300 -100000')
    w.do('idle')
    assert w.visible('Paragraph number 1 '), w.texts()
    w.key('pagedown')
    w.do('idle')
    assert not w.visible('Paragraph number 1 '), 'Page Down moves the transcript'
    w.key('pageup')
    w.do('idle')
    assert w.visible('Paragraph number 1 ')
    # The bar on the right drags, and the end of the text can be reached and follows again.
    w.do('press 1014 60')
    w.do('idle')
    w.do('move 1014 700')
    w.do('idle')
    w.do('release 1014 700')
    w.do('idle')
    assert w.visible('Paragraph number 40 '), w.texts()
    w.do('scroll 640 300 100000')
    w.do('idle')
    assert w.visible('Paragraph number 40 ')


def selection(w):
    w.do('scroll 640 300 -100000')
    w.do('idle')
    top = w.do('place Paragraph number 1 ')[0]
    x, y1 = map(float, top.split()[:2])
    y4 = float(w.do('place Paragraph number 4 ')[0].split()[1])
    # Across paragraphs, a drag selects what it passes over.
    w.do(f'press {x + 2} {y1 + 8}')
    w.do(f'move {x + 300} {y4}')
    w.do(f'move {x + 2000} {y4 + 8}')
    w.do(f'release {x + 2000} {y4 + 8}')
    w.shot('ui-workspace-selection.png')
    text = copied(w)
    assert text == '\n\n'.join(PARAGRAPHS[:4]), repr(text)
    # A click clears it, and a double click takes a word.
    w.do(f'click {x + 300} {y1 + 8}')
    w.do('copy-text __empty__')
    w.key('ctrl+c')
    assert clipboard(w) == '__empty__'
    w.do(f'dblclick {x + 12} {y1 + 8}')
    assert copied(w) == 'Paragraph', clipboard(w)
    w.do(f'tripleclick {x + 12} {y1 + 8}')
    assert copied(w) == PARAGRAPHS[0], clipboard(w)
    # Dragging past the edge of the view scrolls it and keeps selecting.
    w.do(f'click {x + 12} {y1 + 8}')
    w.do(f'press {x + 2} {y1 + 8}')
    w.do(f'move {x + 300} 790')
    w.do('wait 700')
    w.do(f'release {x + 300} 790')
    text = copied(w)
    assert text.startswith(PARAGRAPHS[0]) and PARAGRAPHS[20] in text, repr(text[-200:])
    w.do('scroll 640 300 -100000')
    w.do('idle')


def region_pixels(path, x, y, width, height):
    """The RGB rows of part of a screenshot."""
    pixels = subprocess.check_output(['convert', str(path), '-crop', f'{width}x{height}+{x}+{y}', '+repage',
                                      '-alpha', 'off', '-depth', '8', 'rgb:-'])
    return [[tuple(pixels[(row * width + i) * 3:(row * width + i) * 3 + 3]) for i in range(width)] for row in range(height)]


def wrapped_highlight(w):
    """What Ctrl+A paints: each line of a wrapped paragraph end to end, its code included."""
    chip = (0x33, 0x33, 0x33)
    submit(w, 'wrapped answer')
    w.wait_visible('Sentence 6 has a')
    w.do('idle')
    x, y, _, line = (int(float(n)) for n in w.do('place Sentence 1 has a')[0].split()[:4])
    width, height = 600, line * 3  # the first three of its lines, all of them full
    w.shot('ui-workspace-wrapped.png')
    rows = region_pixels(BUILD / 'ui-workspace-wrapped.png', x, y, width, height)
    assert any(chip in row for row in rows), 'the paragraph shows inline code'
    w.do('click 800 700')
    w.key('ctrl+a')
    w.do('idle')
    w.shot('ui-workspace-wrapped-selection.png')
    rows = region_pixels(BUILD / 'ui-workspace-wrapped-selection.png', x, y, width, height)
    painted = [[i for i, (r, _, b) in enumerate(row) if b > r + 30] for row in rows]
    # Every line runs the whole way across: a wrapped line that lost its right edge
    # leaves only a sliver at the left. Letters at the ends of the region may hide a few pixels.
    spans = [row[-1] - row[0] if row else 0 for row in painted]
    assert min(spans) >= width - 12, (width, spans)
    # Inline code is tinted with the text around it, not punched out of the highlight.
    assert not any(chip in row for row in rows), 'selected inline code keeps its unselected colour'
    w.do('click 800 700')


def select_all(w):
    # In an empty message box and after clicking text alike, Ctrl+A means the transcript.
    w.do('click 800 700')
    w.do('copy-text __empty__')
    w.key('ctrl+a')
    w.shot('ui-workspace-select-all.png')
    text = copied(w)
    assert text.startswith('long answer') and text.endswith(PARAGRAPHS[-1]), repr(text[:80])
    # With a draft in the box, it means the draft, until text is clicked.
    w.tap('Reply')
    w.type('a draft to select')
    w.key('ctrl+a')
    assert copied(w) == 'a draft to select', clipboard(w)
    w.key('ctrl+a', 'backspace')
    # The structure of what was selected survives the copy.
    submit(w, 'rich answer')
    w.wait_visible('After the table')
    w.do('idle')
    x, y = map(float, w.do('place Before the list')[0].split()[:2])
    x2, y2 = map(float, w.do('place After the table')[0].split()[:2])
    w.do(f'press {x} {y + 8}')
    w.do(f'move {x + 400} {y2 + 8}')
    w.do(f'release {x + 400} {y2 + 8}')
    text = copied(w)
    assert text == 'Before the list.\n\nfirst item\nsecond item\n\nint x;\n\nA\tB\nc\td\n\nAfter the table.', repr(text)
    # Text in a message box selects by word and whole, without the drag undoing it.
    w.tap('Reply')
    w.type('several words here')
    x, y = map(float, w.do('place several words here')[0].split()[:2])
    w.do(f'dblclick {x + 14} {y + 8}')
    w.do('idle')
    assert copied(w) == 'several', clipboard(w)
    w.do(f'tripleclick {x + 14} {y + 8}')
    w.do('idle')
    assert copied(w) == 'several words here', clipboard(w)
    w.key('ctrl+a', 'backspace')


def composer_scrolls(w):
    w.tap('Reply')
    lines = '\\n'.join(f'draft line {i}' for i in range(1, 31))
    w.do('type ' + lines)
    w.do('idle')
    assert w.visible('draft line 30')
    w.shot('ui-workspace-long-draft.png')
    x, y = map(float, w.do('place draft line')[0].split()[:2])
    w.do(f'scroll {x + 40} {y + 40} -2000')
    w.shot('ui-workspace-long-draft-top.png')
    assert (BUILD / 'ui-workspace-long-draft.png').read_bytes() != (BUILD / 'ui-workspace-long-draft-top.png').read_bytes(), \
        'The wheel scrolls a long draft'
    w.key('ctrl+a', 'backspace')


def settings_of(temp):
    return (temp / 'config/chela/desktop.ini').read_text()


def permission_modes(harness, server, root):
    """The switcher beside the model: Auto decides, Bypass waits behind Enable, and the choice lasts."""
    temp = root / 'modes'
    project = temp / 'work'
    project.mkdir(parents=True)
    app = App(temp, harness, server.server_port, [project])
    w = app.w
    w.wait_visible('^Manual$')
    w.key('ctrl+m')
    for text in ('Auto', 'Chela handles permission decisions', 'Always ask before making changes',
                 'Automatically accept all file edits', 'Create a plan before making changes', 'Bypass permissions', 'Enable'):
        w.wait_visible(f'^{text}$')
    w.shot('ui-mode-menu.png')
    w.key('1')  # chooses Auto, and must not land in the prompt: the script below would not start
    w.wait(lambda: not w.visible('Chela handles permission decisions'), what='the menu to close')
    w.wait_visible('^Auto$')

    # Auto: the plain edit and the read run; the file that matters and the push ask, with the reason.
    steps = [['write_file', {'path': 'made.txt', 'content': 'by auto'}],
             ['run_command', {'command': 'ls'}],
             ['write_file', {'path': '.env', 'content': 'SECRET=1'}],
             ['run_command', {'command': 'touch pushed && git push'}]]
    submit(w, 'script ' + json.dumps(steps))
    w.wait_visible('Allow file change')
    assert w.visible('Auto asks first because this file controls how tools or git behave')
    assert (project / 'made.txt').read_text() == 'by auto'
    w.tap('Decline')
    w.wait_visible('Allow command')
    assert w.visible('Auto asks first because it sends commits to another computer')
    w.shot('ui-mode-auto-asks.png')
    w.tap('Decline')
    w.wait_visible('Script finished.')
    assert not (project / '.env').exists() and not (project / 'pushed').exists()
    with sqlite3.connect(temp / 'data/chela/sessions.db') as conn:
        assert conn.execute('SELECT mode FROM session_options').fetchall() == [(4,)]

    # New sessions start where the last choice left them, across a restart too.
    w.key('ctrl+n')
    w.wait_visible('^Auto$')
    app.quit()
    assert 'mode=4' in settings_of(temp)
    app = App(temp, harness, server.server_port, [project])
    w = app.w
    w.wait_visible('^Auto$')

    # Bypass needs enabling first, once; cancelling leaves everything as it was.
    w.key('ctrl+m')
    w.tap('Bypass permissions')
    w.wait_visible('Enable Bypass permissions')
    w.tap('Cancel')
    w.wait_visible('^Auto$')
    assert 'bypass_enabled=true' not in settings_of(temp)
    w.key('ctrl+m')
    w.tap('Bypass permissions')
    w.tap('Enable')
    w.wait_visible('^Bypass permissions$')
    assert 'bypass_enabled=true' in settings_of(temp)
    w.key('ctrl+m')
    w.wait_visible('^Run edits and commands without asking$')
    assert not w.visible('^Enable$')
    w.key('2')
    w.wait_visible('^Manual$')
    w.key('ctrl+m')
    w.key('5')
    w.wait_visible('^Bypass permissions$')

    # It is a choice for now: the next launch does not start with every question off.
    app.quit()
    app = App(temp, harness, server.server_port, [project])
    w = app.w
    w.wait_visible('^Manual$')
    w.key('ctrl+m')
    w.key('5')  # still enabled, so no second confirmation
    w.wait_visible('^Bypass permissions$')

    # Turned off in Settings, it goes back behind Enable and the session stops using it.
    w.key('ctrl+comma')
    w.wait_visible('Offer the mode that runs edits')
    x, y, width, height = map(float, w.do('place Offer the mode that runs edits and commands without asking.')[0].split())
    right = sum(map(float, w.do('place Voice settings…')[0].split()[::2]))
    w.do(f'click {right - 16} {y}')
    w.wait(lambda: 'bypass_enabled=false' in settings_of(temp), what='Bypass to be turned off')
    w.key('escape')
    w.wait_visible('^Manual$')
    w.key('ctrl+m')
    w.wait_visible('^Enable$')
    w.key('escape')
    app.quit()


def row_positions(w, *titles):
    return [float(w.do(f'place {title}')[0].split()[1]) for title in titles]


def archived_titles(temp):
    with sqlite3.connect(temp / 'data/chela/sessions.db') as conn:
        return {row[0] for row in conn.execute('SELECT title FROM sessions s JOIN session_options o ON o.id = s.id WHERE archived = 1')}


def restore_all(w, temp):
    w.key('ctrl+shift+f')
    w.tap('View all')
    w.wait_visible('^All sessions$')
    w.key('escape')
    for title in sorted(archived_titles(temp)):
        w.key('ctrl+shift+f')
        w.tap('View all')
        w.wait_visible('^Archived$')
        w.tap(title)
        w.wait_visible('^Restore session$')
        w.tap('Restore session')
        w.wait(lambda: title not in archived_titles(temp), what=f'{title} to be restored')
    w.key('ctrl+n')


def sidebar_menus(harness, server, root):
    """Right click on a folder's heading and on a session, and marking several."""
    temp = root / 'sidebar'
    alpha, beta = temp / 'alpha', temp / 'beta'
    alpha.mkdir(parents=True)
    beta.mkdir()
    app = App(temp, harness, server.server_port, [alpha])
    w = app.w
    for title in ('alpha first', 'alpha second'):
        submit(w, title)
        w.wait_visible(f'Answer to {title}')
        w.key('ctrl+n')
    app.quit()
    app = App(temp, harness, server.server_port, [beta])
    w = app.w
    submit(w, 'beta only')
    w.wait_visible('Answer to beta only')
    w.key('ctrl+n')
    w.wait_visible('^beta$')
    first, second = row_positions(w, 'beta', 'alpha')
    assert first < second, 'The folder last used starts on top'

    # The heading's menu is what the reference shows, less the one thing Chela has nothing behind.
    w.do('rtap alpha')
    w.wait_visible(r'^Archive all \(2\)$')
    for text in (r'^Delete all sessions \(2\)$', '^New session$', '^Move up$', '^Select all$', '^Collapse all$', '^Expand all$'):
        assert w.visible(text), text
    assert not w.visible('^Move down$') and not w.visible('Save as environment')
    w.shot('ui-group-menu.png')

    # Moving a folder, which then stays put.
    w.tap('Move up')
    w.wait(lambda: row_positions(w, 'alpha', 'beta')[0] < row_positions(w, 'alpha', 'beta')[1], what='alpha to move up')
    assert str(alpha) in settings_of(temp)
    w.do('rtap alpha')
    w.wait_visible('^Move down$')
    assert not w.visible('^Move up$')
    w.key('escape')

    # Collapse all and Expand all, each offered when there is something to do.
    w.do('rtap alpha')
    w.tap('Collapse all')
    w.wait(lambda: not w.visible('^alpha first$') and not w.visible('^beta only$'), what='every folder to collapse')
    w.do('rtap alpha')
    w.tap('Expand all')
    w.wait(lambda: w.visible('^alpha first$') and w.visible('^beta only$'), what='every folder to expand')

    # New session starts in that folder.
    w.do('rtap beta')
    w.tap('New session')
    def footer_folder():
        texts = w.texts()
        return texts[texts.index('This computer'):]
    w.wait(lambda: 'beta' in footer_folder() and 'alpha' not in footer_folder(), what='a new session in beta')

    # Right clicking a session looks at it: it does not take you there.
    w.tap('beta only')
    w.wait_visible('Answer to beta only')
    w.do('rtap alpha first')
    w.wait_visible('^Rename…$')
    assert w.visible('Answer to beta only')
    w.key('p')
    w.wait_visible('^Pinned$')
    assert w.visible('Answer to beta only') and not w.visible('^Rename…$')
    w.do('rtap alpha first')
    w.tap('Unpin')
    w.wait(lambda: not w.visible('^Pinned$'), what='the session to be unpinned')
    w.do('rtap alpha first')
    w.key('r')  # renaming needs it open, and opens it
    w.wait_visible('^Rename session$')
    w.wait_visible('Answer to alpha first')
    w.key('escape')

    # Marking with Ctrl: no session opens, and the menu speaks for all of them.
    w.key('ctrl+n')
    w.do('ctap alpha first')
    w.do('ctap alpha second')
    assert not w.visible('Answer to')
    w.do('rtap alpha second')
    w.wait_visible('^Archive 2 sessions$')
    assert w.visible('^Delete 2 sessions…$') and w.visible('^Clear selection$')
    w.key('escape')
    w.key('escape')  # the menu, then the marks
    w.do('rtap alpha second')
    w.wait_visible('^Rename…$')
    w.key('escape')

    # Select all and archive them; the sessions stay safe, out of the sidebar.
    w.do('rtap alpha')
    w.tap('Select all')
    w.do('rtap alpha first')
    w.tap('Archive 2 sessions')
    w.wait(lambda: not w.visible('^alpha second$'), what='the sessions to leave the sidebar')
    assert archived_titles(temp) == {'alpha first', 'alpha second'}
    restore_all(w, temp)
    assert not archived_titles(temp)
    w.wait_visible('^alpha second$')

    # One session, by its key, including the one on screen.
    w.tap('alpha second')
    w.wait_visible('Answer to alpha second')
    w.do('rtap alpha second')
    w.wait_visible('^Archive$')
    w.key('a')
    w.wait(lambda: 'alpha second' in archived_titles(temp), what='the session to be archived')
    w.wait(lambda: not w.visible('Answer to alpha second'), what='the archived session to close')
    restore_all(w, temp)

    # Archive all from the heading.
    w.wait_visible('^alpha$')
    w.do('rtap alpha')
    w.tap(r'Archive all (2)')
    w.wait(lambda: archived_titles(temp) == {'alpha first', 'alpha second'}, what='the folder to be archived')
    restore_all(w, temp)

    # Delete all asks, and only then is anything gone.
    w.wait_visible('^alpha$')
    w.do('rtap alpha')
    w.tap('Delete all sessions (2)')
    w.wait_visible(r'^Delete 2 sessions\?$')
    w.shot('ui-group-delete.png')
    w.tap('Cancel')
    assert len(sessions(temp)) == 3
    w.do('rtap alpha')
    w.tap('Delete all sessions (2)')
    w.wait_visible(r'^Delete 2 sessions\?$')
    w.tap('Delete')
    w.wait(lambda: not w.visible('^alpha$'), what='the folder to go')
    assert [row[0] for row in sessions(temp)] == ['beta only']
    app.quit()


def main():
    BUILD.mkdir(exist_ok=True)
    server = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server.requests, server.results, server.broken = [], [], False
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory() as folder:
        temp = Path(folder).resolve()
        project = temp / 'project-one'
        other = temp / 'other-repo'
        third = temp / 'third-repo'
        outside = temp / 'outside'
        for path in (project, other, third, outside):
            path.mkdir()
        (project / 'readme.txt').write_text('project')
        (other / 'shared.h').write_text('#define SHARED 1\n')
        (other / 'link').symlink_to(outside)
        (outside / 'secret.txt').write_text('private')
        repo = temp / 'repo-one'
        repository(repo)
        harness = Harness(folder)
        try:
            # Named on the command line: this window works there, and Chat still ignores it.
            app = App(temp, harness, server.server_port, [project])
            chat_is_self_contained(app.w, server, project)
            tabs_are_separate(app.w, temp, project)
            retry_after_tools(app.w, server, temp)
            scrolling(app.w)
            selection(app.w)
            select_all(app.w)
            wrapped_highlight(app.w)
            composer_scrolls(app.w)
            app.quit()
            retry_after_restart(harness, server, temp, project)

            # Started from a project's own directory with nothing named: blank.
            app = App(temp, harness, server.server_port)
            app.w.key('ctrl+n')
            code_starts_blank(app.w, server)
            folders_reach_only_what_was_added(app.w, server, temp, project, other, third, outside)
            app.quit()
            folders_come_back(harness, server, temp, project, other, third)

            # Real repositories: branches, and sessions in worktrees of their own.
            app = App(temp, harness, server.server_port, [project])
            no_repository_no_controls(app.w, project)
            app.quit()
            app = App(temp, harness, server.server_port, [repo])
            switching_branches(app.w, temp, repo)
            app.quit()
            worktree_session(harness, server, temp, repo)
            worktree_commits_in_the_sandbox(harness, server)
            removing_worktrees(harness, server)

            # Each with a desktop of its own, as they change settings that would carry over.
            permission_modes(harness, server, temp)
            sidebar_menus(harness, server, temp)
        finally:
            harness.close()
    server.shutdown()
    print('Workspace desktop test passed: self-contained chats, separate Chat and Code tabs with their own sessions and '
          'drafts, a blank Code tab, No folder, extra folders reachable only as added and kept with the session, '
          'switching branches without losing work, sessions in their own worktrees that survive a restart and are '
          'removed with the session without losing uncommitted or unmerged work, the permission mode switcher, the '
          'sidebar right click menus with archiving and bulk actions, retry '
          'after tool calls and restart, wheel, page and scrollbar scrolling, selection across blocks, double and '
          'triple clicks, auto-scroll, Ctrl+A and Ctrl+C.')


if __name__ == '__main__':
    main()
