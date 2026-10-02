#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
"""Isolated socket and PTY regressions; no external model or user configuration."""
import contextlib
import fcntl
import http.server
import json
import os
from pathlib import Path
import pty
import select
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import termios
import threading
import time
import unittest

BIN = Path(sys.argv.pop(1)).resolve()


class Mock(http.server.BaseHTTPRequestHandler):
    requests = []
    delay = 0.1
    tool = None

    def log_message(self, *_):
        pass

    def do_POST(self):
        request = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.requests.append(request)
        time.sleep(self.delay)
        call = None
        if self.tool and not any(m.get('role') == 'tool' for m in request.get('messages', [])):
            call = dict(id='call_fixture', type='function', function=dict(name=self.tool[0], arguments=json.dumps(self.tool[1])))
        if request.get('stream'):
            chunks = [dict(id='test', choices=[dict(index=0, delta=dict(role='assistant', content='MOCKDONE'), finish_reason=None)]),
                      dict(id='test', choices=[dict(index=0, delta={}, finish_reason='stop')])]
            if call:
                call['index'] = 0
                chunks = [dict(id='test', choices=[dict(index=0, delta=dict(role='assistant', tool_calls=[call]), finish_reason=None)]),
                          dict(id='test', choices=[dict(index=0, delta={}, finish_reason='tool_calls')])]
            body = ''.join('data: '  + json.dumps(chunk) + '\n\n' for chunk in chunks).encode() + b'data: [DONE]\n\n'
            mime = 'text/event-stream'
        else:
            body = json.dumps(dict(id='test', choices=[dict(index=0, message=dict(role='assistant', content='MOCKDONE'), finish_reason='stop')])).encode()
            mime = 'application/json'
        self.send_response(200)
        self.send_header('Content-Type', mime)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        with contextlib.suppress(BrokenPipeError, ConnectionResetError):
            self.wfile.write(body)


class Wire:
    def __init__(self, path):
        self.socket = socket.socket(socket.AF_UNIX)
        self.socket.settimeout(8)
        self.socket.connect(str(path))
        self.greeting = self.receive()
        self.events = []
        self.sequence = 0

    def send(self, method, params=None):
        self.sequence += 1
        data = json.dumps(dict(id=self.sequence, method=method, params=params or {})).encode()
        self.socket.sendall(f'{len(data):08x}'.encode() + data)
        return self.sequence

    def receive(self):
        def exact(size):
            data = b''
            while len(data) < size:
                part = self.socket.recv(size - len(data))
                if not part:
                    raise EOFError('daemon closed the socket')
                data += part
            return data
        return json.loads(exact(int(exact(8), 16)))

    def reply(self, request_id):
        while True:
            value = self.receive()
            if value.get('id') == request_id:
                return value
            self.events.append(value)

    def call(self, method, params=None):
        return self.reply(self.send(method, params))


class Lifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Mock)
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='qsoc-daemon-test-')
        self.root = Path(self.temp.name)
        self.env = os.environ.copy()
        self.env.update(HOME=str(self.root), QSOC_HOME=str(self.root / 'config/qsoc'),
                        XDG_CONFIG_HOME=str(self.root / 'config'), XDG_RUNTIME_DIR=str(self.root),
                        QT_QPA_PLATFORM='offscreen', TERM='xterm-256color', LANG='C.UTF-8')
        for key in list(self.env):
            if key.lower().endswith('_proxy'):
                self.env.pop(key)
        config = Path(self.env['QSOC_HOME'])
        config.mkdir(parents=True)
        (config / 'qsoc.yml').write_text(f'''llm:
  models:
    mock:
      name: mock
      model: mock-model
      url: http://127.0.0.1:{self.server.server_port}/v1/chat/completions
      key: none
      timeout: 5000
  model: mock
agent:
  predict_input: false
  session_title: false
  away_summary: false
  memory_extract: false
  memory_dream: false
''')
        self.processes = []
        self.clients = []
        Mock.requests = []
        Mock.delay = 0.1
        Mock.tool = None

    def tearDown(self):
        for client in self.clients:
            client.socket.close()
        for process in self.processes:
            if process.poll() is None:
                process.terminate()
        for process in self.processes:
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.temp.cleanup()

    def daemon(self, path=None):
        path = path or self.root / 'daemon.sock'
        process = subprocess.Popen([str(BIN / 'qsoc-agentd'), '-s', str(path)],
                                   env=self.env, cwd=self.root,
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(process)
        deadline = time.monotonic() + 5
        while process.poll() is None and time.monotonic() < deadline:
            probe = socket.socket(socket.AF_UNIX)
            try:
                probe.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                time.sleep(0.02)
            finally:
                probe.close()
        return process, path

    def client(self, path, **options):
        client = Wire(path)
        self.clients.append(client)
        options.setdefault('project_directory', str(self.root))
        self.assertTrue(client.call('open', options)['result']['ok'])
        return client

    def read_pty(self, fd, duration):
        data = b''
        deadline = time.monotonic() + duration
        while time.monotonic() < deadline:
            if select.select([fd], [], [], 0.025)[0]:
                try:
                    data += os.read(fd, 65536)
                except OSError:
                    break
        return data

    def tui(self, *args):
        master, slave = pty.openpty()
        self.addCleanup(os.close, master)
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 32, 100, 0, 0))
        process = subprocess.Popen([str(BIN / 'qsoc'), 'agent', *map(str, args)],
                                   stdin=slave, stdout=slave, stderr=slave,
                                   env=self.env, cwd=self.root, start_new_session=True)
        os.close(slave)
        self.processes.append(process)
        initial = self.read_pty(master, 0.8)
        self.assertIsNone(process.poll(), initial.decode(errors='replace'))
        self.assertIn(b'Ready', initial)
        return process, master

    def test_detach_during_turn_keeps_daemon_alive(self):
        daemon, path = self.daemon()
        client = self.client(path)
        Mock.delay = 0.5
        client.send('turn', {'input': 'disconnect during request'})
        time.sleep(0.1)
        client.socket.close()
        time.sleep(0.8)
        self.assertIsNone(daemon.poll())
        other = self.client(path)
        self.assertIn('result', other.call('status'))

    def test_duplicate_listener_is_refused(self):
        first, path = self.daemon()
        original = self.client(path)
        second, _ = self.daemon(path)
        self.assertNotEqual(second.wait(timeout=5), 0)
        self.assertIsNone(first.poll())
        self.assertIn('result', original.call('status'))
        self.client(path)

    def test_workspace_isolation(self):
        _, path = self.daemon()
        for name in ['A', 'B']:
            (self.root / name).mkdir()
        first = self.client(path, workspace=str(self.root / 'A'))
        self.client(path, workspace=str(self.root / 'B'))
        first.call('command', {'input': '!pwd'})
        output = ''.join(event.get('event', {}).get('text', '') for event in first.events)
        self.assertIn(str(self.root / 'A'), output)
        self.assertNotIn(str(self.root / 'B'), output)

    def test_abort_arrives_during_turn(self):
        daemon, path = self.daemon()
        client = self.client(path)
        Mock.delay = 2
        turn = client.send('turn', {'input': 'slow request'})
        time.sleep(0.1)
        started = time.monotonic()
        client.send('abort')
        result = client.reply(turn)['result']
        self.assertTrue(result['aborted'])
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertIsNone(daemon.poll())

    def test_model_effort_and_help(self):
        _, path = self.daemon()
        client = self.client(path)
        for command in ['/help', '/model mock', '/effort high']:
            reply = client.call('command', {'input': command})
            self.assertTrue(reply['result']['handled'])
        self.assertEqual(reply['result']['snapshot']['effort'], 'high')

    def test_malformed_length_disconnects_only_client(self):
        daemon, path = self.daemon()
        client = self.client(path)
        client.socket.sendall(b'7fffffff')
        self.assertEqual(client.socket.recv(1), b'')
        self.assertIsNone(daemon.poll())
        self.client(path)

    def test_owned_tui_input_and_shutdown(self):
        process, fd = self.tui()
        children_file = Path(f'/proc/{process.pid}/task/{process.pid}/children')
        children = [int(pid) for pid in children_file.read_text().split()]
        self.assertTrue(children, 'default TUI must own a daemon child')
        marker = b'VISIBLE_PROMPT_7391'
        os.write(fd, marker)
        self.assertIn(marker, self.read_pty(fd, 0.3))
        os.write(fd, b'\r')
        self.assertIn(b'MOCKDONE', self.read_pty(fd, 0.8))
        os.write(fd, b'/exit\r')
        process.wait(timeout=5)
        self.assertIn(b'Resume this session with:', self.read_pty(fd, 0.3))
        for pid in children:
            self.assertFalse(Path(f'/proc/{pid}').exists(), f'owned child {pid} survived TUI exit')

    def test_attached_tui_exit_leaves_daemon(self):
        daemon, path = self.daemon()
        process, fd = self.tui('--connect', path)
        os.write(fd, b'/exit\r')
        self.assertEqual(process.wait(timeout=5), 0)
        self.assertIsNone(daemon.poll())
        self.client(path)

    def test_shell_abort_and_deferred_input(self):
        daemon, path = self.daemon()
        client = self.client(path)
        command = client.send('command', {'input': '!sleep 20'})
        time.sleep(0.1)
        queued = client.call('turn', {'input': 'after shell'})
        self.assertTrue(queued['result']['queued'])
        started = time.monotonic()
        client.send('abort')
        self.assertTrue(client.reply(command)['result']['handled'])
        self.assertLess(time.monotonic() - started, 1.5)
        result = client.reply(0)['result']
        self.assertEqual(result['final_text'], 'MOCKDONE')
        self.assertIsNone(daemon.poll())

    def test_menu_nonce_and_abort(self):
        _, path = self.daemon()
        client = self.client(path)
        command = client.send('command', {'input': '/effort'})
        while True:
            event = client.receive().get('event', {})
            if event.get('kind') == 'ask_user':
                break
        nonce = event['json']['request_id']
        self.assertIn('error', client.call('answer', {'request_id': nonce + 1, 'index': 3}))
        client.send('answer', {'request_id': nonce, 'index': 3})
        self.assertEqual(client.reply(command)['result']['snapshot']['effort'], 'high')
        command = client.send('command', {'input': '/model'})
        while client.receive().get('event', {}).get('kind') != 'ask_user':
            pass
        client.send('abort')
        self.assertTrue(client.reply(command)['result']['handled'])

    def test_failed_resume_preserves_session(self):
        _, path = self.daemon()
        client = self.client(path)
        session = client.call('status')['result']['session_id']
        self.assertFalse(client.call('open_session', {'id': 'nonexistent'})['result']['ok'])
        self.assertEqual(client.call('status')['result']['session_id'], session)
        self.assertEqual(client.call('turn', {'input': 'still usable'})['result']['final_text'], 'MOCKDONE')

    def test_compaction_preserves_unextracted_turns_across_restart(self):
        config = Path(self.env['QSOC_HOME']) / 'qsoc.yml'
        config.write_text(config.read_text().replace('memory_extract: false',
            'memory_extract: true\n  memory_extract_cadence: 100'))
        daemon, path = self.daemon()
        client = self.client(path)
        session = client.call('status')['result']['session_id']
        first = 'PENDING_FIRST ' + 'detail ' * 500
        for prompt in [first, 'PENDING_SECOND ' + 'detail ' * 500]:
            self.assertFalse(client.call('turn', {'input': prompt})['result']['error'])
        reply = client.call('command', {'input': '/compact'})
        self.assertTrue(reply['result']['handled'])
        session_file = self.root / '.qsoc/sessions' / (session + '.jsonl')

        def pending():
            records = [json.loads(line) for line in session_file.read_text().splitlines()]
            values = [r['value'] for r in records if r.get('type') == 'meta'
                      and r.get('key') == 'memory_pending']
            self.assertTrue(values, 'runtime must persist the extraction cursor before compaction')
            return json.loads(values[-1])

        self.assertIn(first, [m.get('content') for m in pending()])
        client.socket.close()
        daemon.terminate()
        daemon.wait(timeout=5)
        _, path = self.daemon(path)
        restored = self.client(path, resume_session_id=session)
        for prompt in ['PENDING_THIRD ' + 'detail ' * 500, 'PENDING_FOURTH ' + 'detail ' * 500]:
            self.assertFalse(restored.call('turn', {'input': prompt})['result']['error'])
        restored.call('command', {'input': '/compact'})
        self.assertIn(first, [m.get('content') for m in pending()],
                      'resuming must restore pending extraction as well as its message index')
        self.assertTrue(any('PENDING_THIRD' in m.get('content', '') for m in pending()))

    def test_compaction_defers_commands_until_cancelled(self):
        _, path = self.daemon()
        client = self.client(path)
        for prompt in ['first ' + 'detail ' * 500, 'second ' + 'detail ' * 500]:
            self.assertFalse(client.call('turn', {'input': prompt})['result']['error'])
        Mock.delay = 2
        compact = client.send('command', {'input': '/compact'})
        while client.receive().get('event', {}).get('text') != 'Compacting':
            pass
        queued = client.send('command', {'input': '/rename queued-title'})
        self.assertTrue(client.reply(queued)['result']['queued'])
        client.send('abort')
        self.assertTrue(client.reply(compact)['result']['handled'])
        self.assertTrue(client.reply(0)['result']['handled'])
        sessions = client.call('sessions')['result']['sessions']
        self.assertEqual(sessions[0]['title'], 'queued-title')

    def test_resume_command_switches_and_preserves_saved_session(self):
        _, path = self.daemon()
        client = self.client(path, launch_directory=str(self.root), client_program='qsoc')
        turn = client.call('turn', {'input': 'saved first session'})['result']
        session = turn['snapshot']['session_id']
        self.assertEqual(turn['snapshot']['resume_command'], 'qsoc agent --resume ' + session)
        cleared = client.call('command', {'input': '/clear'})['result']['snapshot']
        self.assertNotEqual(cleared['session_id'], session)
        self.assertEqual(cleared['resume_command'], '')
        resumed = client.call('command', {'input': '/resume ' + session[:8]})['result']['snapshot']
        self.assertEqual(resumed['session_id'], session)
        self.assertTrue(any(m.get('content') == 'saved first session' for m in resumed['messages']))
        self.assertEqual(resumed['resume_command'], 'qsoc agent --resume ' + session)

    def test_completion_stays_in_client_workspace(self):
        _, path = self.daemon()
        (self.root / 'unique-module.sv').write_text('module top; endmodule')
        client = self.client(path)
        items = client.call('complete', {'query': 'unique-mod'})['result']['items']
        self.assertTrue(any('unique-module.sv' in item for item in items), items)

    def test_owned_daemon_exits_when_tui_is_killed(self):
        process, _ = self.tui()
        children = [int(pid) for pid in Path(f'/proc/{process.pid}/task/{process.pid}/children').read_text().split()]
        self.assertTrue(children)
        process.kill()
        process.wait(timeout=5)
        deadline = time.monotonic() + 5
        def alive(pid):
            try:
                return Path(f'/proc/{pid}/stat').read_text().split(') ', 1)[1][0] != 'Z'
            except FileNotFoundError:
                return False
        while any(alive(pid) for pid in children) and time.monotonic() < deadline:
            time.sleep(0.05)
        self.assertFalse(any(alive(pid) for pid in children))

    def test_shutdown_during_turn(self):
        daemon, path = self.daemon()
        client = self.client(path)
        Mock.delay = 2
        client.send('turn', {'input': 'slow'})
        time.sleep(0.1)
        client.call('shutdown')
        self.assertEqual(daemon.wait(timeout=3), 0)

    def test_query_startup_error_is_failure(self):
        result = subprocess.run([str(BIN / 'qsoc'), 'agent', '--resume', 'nonexistent', '-q', 'hello'],
                                cwd=self.root, env=self.env, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(Mock.requests)

    def test_crash_resume_continues_saved_input_once(self):
        daemon, path = self.daemon()
        client = self.client(path)
        session = client.call('status')['result']['session_id']
        Mock.delay = 2
        client.send('turn', {'input': 'recover this unique prompt'})
        deadline = time.monotonic() + 3
        while not Mock.requests and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(Mock.requests)
        daemon.kill()
        daemon.wait(timeout=3)
        client.socket.close()
        Mock.delay = 0.1
        daemon, path = self.daemon(path)
        restored = self.client(path, resume_session_id=session)
        self.assertEqual(restored.reply(0)['result']['final_text'], 'MOCKDONE')
        count = sum(m.get('content') == 'recover this unique prompt' for m in Mock.requests[-1]['messages'])
        self.assertEqual(count, 1, 'resume must not duplicate the staged user input')

    def test_write_tool_records_history_and_diff(self):
        _, path = self.daemon()
        client = self.client(path)
        Mock.tool = ('write_file', {'file_path': str(self.root / 'created.txt'), 'content': 'a new line\n'})
        result = client.call('turn', {'input': 'create the fixture'})['result']
        self.assertFalse(result['error'])
        self.assertEqual((self.root / 'created.txt').read_text(), 'a new line\n')
        command = client.send('command', {'input': '/diff'})
        while True:
            value = client.receive()
            if value.get('id') == command:
                break
            client.events.append(value)
            event = value.get('event', {})
            if event.get('kind') == 'ask_user':
                client.send('answer', {'request_id': event['json']['request_id'], 'index': 0})
        diffs = [e['event'] for e in client.events if e.get('event', {}).get('kind') == 'diff']
        self.assertTrue(diffs, 'file tool must populate the runtime file history')
        self.assertEqual(diffs[-1]['json']['after'], 'a new line\n')

    def test_query_sigint_aborts(self):
        Mock.delay = 3
        query = subprocess.Popen([str(BIN / 'qsoc'), 'agent', '-q', 'cancel me'],
                                 cwd=self.root, env=self.env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.processes.append(query)
        deadline = time.monotonic() + 3
        while not Mock.requests and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertTrue(Mock.requests)
        query.send_signal(signal.SIGINT)
        self.assertEqual(query.wait(timeout=2), 0)

    def test_large_paste_and_history_expand_payload(self):
        process, fd = self.tui()
        pasted = ('PASTE_FIXTURE_' * 100).encode()
        os.write(fd, b'\x1b[200~' + pasted + b'\x1b[201~')
        self.assertIn(b'Pasted text #', self.read_pty(fd, 0.2))
        os.write(fd, b'\r')
        self.assertIn(b'MOCKDONE', self.read_pty(fd, 0.6))
        self.assertTrue(any(m.get('content') == pasted.decode() for m in Mock.requests[-1]['messages']))
        os.write(fd, b'\x1b[A')
        self.assertIn(b'Pasted text #', self.read_pty(fd, 0.2))
        os.write(fd, b'\r')
        self.assertIn(b'MOCKDONE', self.read_pty(fd, 0.6))
        self.assertEqual(sum(m.get('content') == pasted.decode() for m in Mock.requests[-1]['messages']), 2)
        os.write(fd, b'/exit\r')
        self.assertEqual(process.wait(timeout=5), 0)

    def test_config_disables_streaming_in_owned_query(self):
        config = Path(self.env['QSOC_HOME']) / 'qsoc.yml'
        config.write_text(config.read_text().replace('agent:\n', 'agent:\n  stream: false\n'))
        query = subprocess.run([str(BIN / 'qsoc'), 'agent', '-q', 'configured stream'],
                               cwd=self.root, env=self.env, capture_output=True, timeout=10)
        self.assertEqual(query.returncode, 0, query.stderr.decode())
        self.assertIn(b'MOCKDONE', query.stdout)
        self.assertFalse(Mock.requests[-1].get('stream', False))

    def test_plan_approval_returns_to_execution(self):
        _, path = self.daemon()
        client = self.client(path)
        client.call('plan_mode', {'enabled': True})
        turn = client.send('turn', {'input': 'propose a plan'})
        while True:
            value = client.receive()
            event = value.get('event', {})
            if event.get('kind') == 'ask_user' and event.get('json', {}).get('type') == 'plan':
                client.send('answer', {'request_id': event['json']['request_id'], 'approved': True})
                break
        self.assertEqual(client.reply(turn)['result']['final_text'], 'MOCKDONE')
        self.assertEqual(client.reply(0)['result']['final_text'], 'MOCKDONE')
        self.assertEqual(client.call('status')['result']['running'], False)
        self.assertTrue(list(self.root.glob('.qsoc/plans/*.md')))

    def test_query_and_options_over_socket(self):
        _, path = self.daemon()
        query = subprocess.run([str(BIN / 'qsoc'), 'agent', '--connect', str(path),
                                '--no-stream', '--effort', 'high', '-q', 'hello'],
                               cwd=self.root, env=self.env, capture_output=True, timeout=10)
        self.assertEqual(query.returncode, 0, query.stderr.decode())
        self.assertIn(b'MOCKDONE', query.stdout)
        self.assertFalse(Mock.requests[-1].get('stream', False))


if __name__ == '__main__':
    unittest.main()
