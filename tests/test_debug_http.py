"""Local HTTP regressions; no external sources or user database are accessed.

Run with --server /path/to/openread_web_server or OPENREAD_WEB_SERVER.
Only Python's standard library is required.
"""

import argparse
import gzip
import http.client
import json
import os
import shutil
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlencode, urlsplit

ROOT = Path(__file__).resolve().parents[1]
SERVER_BINARY = ROOT / 'build/bin/openread_web_server'
RESPONSE_LIMIT = 4 * 1024 * 1024
OVERSIZED_BODY = json.dumps({'books': [], 'padding': 'x' * RESPONSE_LIMIT}).encode()
GZIP_OVERSIZED_BODY = gzip.compress(OVERSIZED_BODY)
CONTENT = '用于端到端验证的本地正文。'


def stop_process(process):
    """Only stop a process created by this test, and always bound the wait."""
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=3)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=3)


def http_request(port, method, path, body=None, timeout=15):
    deadline = time.monotonic() + timeout
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=timeout)
    try:
        connection.request(method, path, body, {'Content-Type': 'application/json'})
        # Retain the socket even if HTTPConnection releases it for Connection: close.
        request_socket = connection.sock
        request_socket.settimeout(max(0.001, deadline - time.monotonic()))
        response = connection.getresponse()
        try:
            chunks = []
            size = 0
            while not response.isclosed():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('HTTP response did not finish before its deadline')
                request_socket.settimeout(remaining)
                chunk = response.read1(64 * 1024)
                if not chunk:
                    break
                size += len(chunk)
                if size > 2 * 1024 * 1024:
                    raise AssertionError('Diagnostic response exceeded the test read limit')
                chunks.append(chunk)
            return response.status, b''.join(chunks).decode(), dict(response.getheaders())
        finally:
            response.close()
    finally:
        connection.close()


class SourceHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(3)

    def do_GET(self):
        parts = urlsplit(self.path).path.strip('/').split('/')
        scenario, endpoint = parts if len(parts) == 2 else ('ok', 'missing')
        base = f'http://127.0.0.1:{self.server.server_port}/{scenario}'
        status = 200
        compressed = False
        if scenario in ('oversized', 'gzip-oversized') and endpoint == 'search':
            compressed = scenario == 'gzip-oversized'
            data = GZIP_OVERSIZED_BODY if compressed else OVERSIZED_BODY
        else:
            if endpoint == 'search':
                body = {'books': [] if scenario == 'empty-search' else [
                    {'name': '测试书', 'url': base + '/book'}]}
                if scenario == 'http-error':
                    status = 503
            elif endpoint == 'book':
                body = {'chapters': [] if scenario == 'empty-catalog' else [
                    {'title': '第一章', 'url': base + '/chapter'}]}
            elif endpoint == 'chapter':
                body = {'text': '' if scenario == 'empty-content' else CONTENT}
                if scenario == 'long-content':
                    body['text'] = '文' * 2000
            else:
                status, body = 404, {'error': 'unknown mock route'}
            data = json.dumps(body, ensure_ascii=False).encode()
        try:
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(data)))
            if compressed:
                self.send_header('Content-Encoding', 'gzip')
            self.end_headers()
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError, TimeoutError):
            # The response limit deliberately makes curl stop reading early.
            pass

    def log_message(self, *_):
        pass


# A separate owned process keeps legacy "kill the port owner" behavior from
# killing this test runner. The port file is written only after bind succeeds.
PORT_OWNER_SCRIPT = r'''
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import sys
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        data = b'openread-test-port-owner'
        self.send_response(200)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)
    def log_message(self, *_):
        pass
server = HTTPServer(('127.0.0.1', 0), Handler)
port_file = Path(sys.argv[1])
ready_file = port_file.with_suffix('.ready')
ready_file.write_text(str(server.server_port))
ready_file.replace(port_file)
server.serve_forever(poll_interval=0.05)
'''


class DebugHttpTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SERVER_BINARY.is_file():
            raise RuntimeError(f'Server binary not found: {SERVER_BINARY}; build it or pass --server')
        cls.source_server = ThreadingHTTPServer(('127.0.0.1', 0), SourceHandler)
        cls.source_server.daemon_threads = True
        cls.source_server.block_on_close = False
        cls.addClassCleanup(cls.source_server.server_close)
        cls.source_thread = threading.Thread(
            target=cls.source_server.serve_forever, kwargs={'poll_interval': 0.05}, daemon=True)
        cls.source_thread.start()
        cls.addClassCleanup(cls.source_thread.join, 3)
        cls.addClassCleanup(cls.source_server.shutdown)
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            cls.port = probe.getsockname()[1]
        cls.server_log = tempfile.TemporaryFile(mode='w+b')
        cls.addClassCleanup(cls.server_log.close)
        cls.process = subprocess.Popen(cls.server_command(cls.port),
                                       stdout=cls.server_log, stderr=cls.server_log)
        cls.addClassCleanup(stop_process, cls.process)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if cls.process.poll() is not None:
                cls.server_log.seek(0)
                raise RuntimeError(f'Server exited with {cls.process.returncode}:\n'
                                   + cls.server_log.read().decode(errors='replace')[-4000:])
            try:
                if cls.request('GET', '/api/health', timeout=0.2)[0] == 200:
                    return
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        raise RuntimeError('Server did not start within 10 seconds')

    @staticmethod
    def server_command(port):
        return [str(SERVER_BINARY), '--host', '127.0.0.1', '--port', str(port),
                '--db', ':memory:', '--web-root', str(ROOT / 'bindings/web/openread/web')]

    @classmethod
    def request(cls, method, path, body=None, timeout=15):
        return http_request(cls.port, method, path, body, timeout)

    def load_source(self, scenario='ok'):
        base = f'http://127.0.0.1:{self.source_server.server_port}/{scenario}'
        source = {
            'bookSourceName': '本地诊断源-' + scenario, 'bookSourceUrl': base,
            'searchUrl': base + '/search?q={{key}}',
            'ruleSearch': {'bookList': '$.books[*]', 'name': '$.name', 'bookUrl': '$.url'},
            'ruleToc': {'chapterList': '$.chapters[*]', 'chapterName': '$.title', 'chapterUrl': '$.url'},
            'ruleContent': {'content': '$.text'},
        }
        status, raw, _ = self.request('POST', '/api/sources/raw', json.dumps({'json': json.dumps(source)}))
        self.assertEqual(status, 200, raw)
        self.assertTrue(json.loads(raw)['ok'], raw)
        return base

    def debug_events(self, scenario='ok'):
        query = urlencode({'source_url': self.load_source(scenario), 'q': '测试'})
        status, stream, headers = self.request('GET', '/api/source/debug?' + query)
        self.assertEqual(status, 200, stream)
        self.assertIn('text/event-stream', headers.get('Content-Type', ''))
        self.assertTrue(stream.endswith('\n\n'), stream)
        events = []
        for frame in stream.strip().split('\n\n'):
            lines = frame.splitlines()
            self.assertEqual(len(lines), 2, frame)
            self.assertTrue(lines[0].startswith('event: '), frame)
            self.assertTrue(lines[1].startswith('data: '), frame)
            events.append((lines[0][7:], json.loads(lines[1][6:])))
        return events

    def assert_stage_failure(self, events, stage):
        names = [name for name, _ in events if name != 'debug_http']
        completed = ['debug_info', 'debug_search', 'debug_catalog', 'debug_content']
        self.assertEqual(names, completed[:completed.index(stage)] + ['debug_error'])
        self.assertEqual(events[-1][0], 'debug_error')
        self.assertEqual(events[-1][1]['stage'], stage)
        self.assertTrue(events[-1][1]['error'])
        self.assertEqual(self.request('GET', '/api/health')[0], 200)

    def test_rss_list_content_failure_and_asset_revalidation(self):
        origin = f'http://127.0.0.1:{self.source_server.server_port}/ok'
        source = {'sourceUrl': origin, 'sourceName': 'RSS fixture', 'singleUrl': False,
                  'sortUrl': origin + '/search', 'ruleArticles': '$.books[*]',
                  'ruleTitle': '$.name', 'ruleLink': '$.url', 'ruleContent': '$.chapters[0].title'}
        status, body, _ = self.request('POST', '/api/rss/import/json', json.dumps({'json': json.dumps(source)}))
        self.assertEqual(status, 200, body)
        status, body, _ = self.request('GET', '/api/rss/articles?' + urlencode({'source_url': origin, 'load': 1}))
        self.assertEqual(json.loads(body)['total'], 1, body)
        article = json.loads(body)['articles'][0]
        status, body, _ = self.request('GET', '/api/rss/article?id=' + str(article['id']))
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)['content'], '第一章')
        self.assertEqual(json.loads(body)['contentError'], '')
        source['ruleContent'] = '$.missing'
        self.request('POST', '/api/rss/import/json', json.dumps({'json': json.dumps(source)}))
        status, body, _ = self.request('GET', '/api/rss/article?id=' + str(article['id']))
        self.assertTrue(json.loads(body)['contentError'], body)
        status, body, _ = self.request('GET', '/api/rss/check/stream')
        self.assertEqual(status, 200, body)
        self.assertEqual(body.count('event: check_done'), 1, body)
        self.assertEqual(body.count('event: check_progress'), 1, body)
        self.assertEqual(self.request('GET', '/api/rss/article?id=999999999')[0], 404)
        self.assertEqual(self.request('GET', '/style.css')[2].get('Cache-Control'), 'no-cache')

    def test_console_result_and_isolation(self):
        status, raw, _ = self.request('POST', '/api/eval', json.dumps({
            'code': "globalThis.saved = 7; console.log('hello'); 42"}))
        self.assertEqual(status, 200, raw)
        result = json.loads(raw)
        self.assertTrue(result['ok'])
        self.assertEqual(result['result'], '42')
        self.assertEqual(result['logs'], ['hello'])
        self.assertIsInstance(result['elapsedMs'], int)
        _, raw, _ = self.request('POST', '/api/eval', json.dumps({'code': 'typeof saved'}))
        self.assertEqual(json.loads(raw)['result'], 'undefined')

    def test_source_debug_fragment_base_and_relative_search(self):
        origin = f'http://127.0.0.1:{self.source_server.server_port}'
        source_url = origin + '#作者标记'
        source = {
            'bookSourceName': '带作者标记的本地源', 'bookSourceUrl': source_url,
            'searchUrl': '/ok/search?q={{key}}',
            'ruleSearch': {'bookList': '$.books[*]', 'name': '$.name', 'bookUrl': '$.url'},
            'ruleToc': {'chapterList': '$.chapters[*]', 'chapterName': '$.title', 'chapterUrl': '$.url'},
            'ruleContent': {'content': '$.text'},
        }
        status, raw, _ = self.request('POST', '/api/sources/raw', json.dumps({'json': json.dumps(source)}))
        self.assertEqual(status, 200, raw)
        status, stream, _ = self.request('GET', '/api/source/debug?' + urlencode({
            'source_url': source_url, 'q': '我'}))
        self.assertEqual(status, 200, stream)
        events = [(frame.splitlines()[0][7:], json.loads(frame.splitlines()[1][6:]))
                  for frame in stream.strip().split('\n\n')]
        self.assertEqual(events[-1][0], 'debug_done', stream)
        self.assertEqual(events[0][1]['sourceUrl'], source_url)
        requests = [data for event, data in events if event == 'debug_http']
        self.assertEqual(len(requests), 3)
        self.assertEqual(requests[0]['url'], origin + '/ok/search?q=%E6%88%91')
        self.assertTrue(all(request['status'] == 200 for request in requests))
        self.assertIn(CONTENT, stream)

    def test_relocated_runtime_uses_adjacent_assets(self):
        runtime_temp = tempfile.TemporaryDirectory()
        self.addCleanup(runtime_temp.cleanup)
        runtime_dir = Path(runtime_temp.name)
        # Copy only the distributable runtime; no build tree or source-tree assets.
        binary = runtime_dir / SERVER_BINARY.name
        shutil.copy2(SERVER_BINARY, binary)
        for pattern in ('*.dylib', '*.so*', '*.dll'):
            for library in SERVER_BINARY.parent.glob(pattern):
                if library.is_file():
                    shutil.copy2(library, runtime_dir / library.name)
        shutil.copytree(SERVER_BINARY.parent / 'web', runtime_dir / 'web')
        marker = 'This asset is only in the relocated runtime.'
        (runtime_dir / 'web/runtime-probe.txt').write_text(marker, encoding='utf-8')
        with socket.socket() as probe:
            probe.bind(('127.0.0.1', 0))
            port = probe.getsockname()[1]
        output = tempfile.TemporaryFile()
        self.addCleanup(output.close)
        environment = {key: value for key, value in os.environ.items()
                       if key not in ('DYLD_LIBRARY_PATH', 'LD_LIBRARY_PATH')}
        process = subprocess.Popen([str(binary), '--port', str(port), '--db', ':memory:'],
                                   cwd=runtime_dir.parent, env=environment,
                                   stdout=output, stderr=output)
        self.addCleanup(stop_process, process)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            if process.poll() is not None:
                output.seek(0)
                self.fail(output.read().decode(errors='replace'))
            try:
                status, body, _ = http_request(port, 'GET', '/runtime-probe.txt', timeout=0.2)
                if status == 200:
                    self.assertEqual(body, marker)
                    break
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(0.05)
        else:
            self.fail('Relocated runtime did not serve its adjacent assets')
        status, body, _ = http_request(port, 'GET', '/debug.js')
        self.assertEqual(status, 200)
        self.assertEqual(body, (runtime_dir / 'web/debug.js').read_text(encoding='utf-8'))

    def test_console_invalid_inputs(self):
        bodies = ['{', '[]', 'null', '{}', '{"code":42}', '{"code":null}', '{"code":""}']
        bodies += [json.dumps({'code': '1', 'timeoutMs': value})
                   for value in (0, -1, 5001, 1.5, '20', True, None, 2 ** 64)]
        for body in bodies:
            with self.subTest(body=body):
                status, raw, _ = self.request('POST', '/api/eval', body)
                self.assertEqual(status, 400, raw)
                self.assertTrue(json.loads(raw)['error'])
        for body in (json.dumps({'code': 'x' * 65537}),
                     json.dumps({'code': '1', 'padding': 'x' * (512 * 1024)})):
            status, raw, _ = self.request('POST', '/api/eval', body)
            self.assertEqual(status, 413, raw)
        self.assertEqual(self.request('GET', '/api/health')[0], 200)

    def test_console_execution_limits_and_recovery(self):
        for code, error in [('while(true){}', 'timed out'),
                            ('throw new Error("expected failure")', 'expected failure')]:
            with self.subTest(code=code):
                status, raw, _ = self.request('POST', '/api/eval', json.dumps({'code': code, 'timeoutMs': 20}))
                self.assertEqual(status, 200, raw)
                result = json.loads(raw)
                self.assertFalse(result['ok'])
                self.assertIn(error, result['error'])
        _, raw, _ = self.request('POST', '/api/eval', json.dumps({
            'code': 'for (let i = 0; i < 110; i++) console.log(i); "x".repeat(70000)'}))
        result = json.loads(raw)
        self.assertTrue(result['ok'], result)
        self.assertTrue(result['logsTruncated'])
        self.assertLessEqual(len(result['logs']), 100)
        self.assertTrue(result['resultTruncated'])
        self.assertLessEqual(len(result['result'].encode()), 65536)
        _, raw, _ = self.request('POST', '/api/eval', json.dumps({'code': '6 * 7'}))
        self.assertEqual(json.loads(raw)['result'], '42')

    def test_source_debug_success_and_stream_completion(self):
        events = self.debug_events()
        names = [name for name, _ in events]
        self.assertEqual([name for name in names if name != 'debug_http'],
                         ['debug_info', 'debug_search', 'debug_catalog', 'debug_content', 'debug_done'])
        requests = [data for name, data in events if name == 'debug_http']
        self.assertEqual([request['stage'] for request in requests],
                         ['debug_search', 'debug_catalog', 'debug_content'])
        for request in requests:
            self.assertEqual(request['method'], 'GET')
            self.assertEqual(request['status'], 200)
            self.assertFalse(request['error'])
            self.assertGreater(request['bytes'], 0)
            self.assertGreaterEqual(request['elapsedMs'], 0)
        content = dict(events)['debug_content']
        self.assertEqual(content['preview'], CONTENT)
        self.assertEqual(content['length'], len(CONTENT.encode()))
        self.assertFalse(content['truncated'])
        self.assertTrue(events[-1][1]['ok'])
        self.assertEqual(events[-1][1]['requests'], 3)

    def test_source_validation_stream_finishes_with_skipped_sources(self):
        self.request('DELETE', '/api/sources/clear')
        source = {'bookSourceUrl': 'https://no-search.test', 'bookSourceName': 'No search rule'}
        self.request('POST', '/api/sources/raw', json.dumps({'json': json.dumps([source])}))
        status, raw, _ = self.request('GET', '/api/sources/validate', timeout=3)
        self.assertEqual(status, 200, raw)
        self.assertEqual(raw.count('event: validate_start'), 1, raw)
        self.assertEqual(raw.count('event: validate_done'), 1, raw)
        self.request('DELETE', '/api/sources/clear')

    def test_source_debug_invalid_inputs(self):
        source_url = self.load_source()
        for path, expected in [('/api/source/debug', 400),
                               ('/api/source/debug?source_url=missing', 404),
                               ('/api/source/debug?source_name=missing', 404)]:
            self.assertEqual(self.request('GET', path)[0], expected)
        for keyword in ('x' * 1025, '文' * 342):
            query = urlencode({'source_url': source_url, 'q': keyword})
            self.assertEqual(self.request('GET', '/api/source/debug?' + query)[0], 400)

    def test_source_debug_failed_stages(self):
        for scenario, stage in [('empty-search', 'debug_search'), ('http-error', 'debug_search'),
                                ('empty-catalog', 'debug_catalog'), ('empty-content', 'debug_content')]:
            with self.subTest(scenario=scenario):
                self.assert_stage_failure(self.debug_events(scenario), stage)

    def test_source_debug_response_and_gzip_limits(self):
        self.assertGreater(len(OVERSIZED_BODY), RESPONSE_LIMIT)
        self.assertLess(len(GZIP_OVERSIZED_BODY), RESPONSE_LIMIT)
        for scenario in ('oversized', 'gzip-oversized'):
            with self.subTest(scenario=scenario):
                events = self.debug_events(scenario)
                self.assert_stage_failure(events, 'debug_search')
                requests = [data for name, data in events if name == 'debug_http']
                self.assertEqual(len(requests), 1)
                self.assertIn('exceeds 4194304 bytes', requests[0]['error'])
                self.assertEqual(requests[0]['status'], 0)
                self.assertLessEqual(requests[0]['bytes'], RESPONSE_LIMIT)
                self.assertIn('exceeds 4194304 bytes', events[-1][1]['error'])

    def test_source_debug_utf8_preview_limit(self):
        content = dict(self.debug_events('long-content'))['debug_content']
        self.assertTrue(content['truncated'])
        self.assertEqual(content['length'], len(('文' * 2000).encode()))
        self.assertLessEqual(len(content['preview'].encode()), 4096)
        self.assertEqual(content['preview'], '文' * (4096 // 3))

    def test_startup_does_not_kill_port_owner(self):
        temp_dir = tempfile.TemporaryDirectory(prefix='openread-port-test-')
        self.addCleanup(temp_dir.cleanup)
        port_file = Path(temp_dir.name) / 'port'
        owner = subprocess.Popen([sys.executable, '-c', PORT_OWNER_SCRIPT, str(port_file)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(stop_process, owner)
        deadline = time.monotonic() + 5
        while not port_file.exists() and time.monotonic() < deadline:
            self.assertIsNone(owner.poll(), 'Port-owner fixture exited before binding')
            time.sleep(0.02)
        self.assertTrue(port_file.exists(), 'Port-owner fixture did not start within 5 seconds')
        occupied_port = int(port_file.read_text())
        self.assertEqual(http_request(occupied_port, 'GET', '/', timeout=2)[1], 'openread-test-port-owner')
        log = tempfile.TemporaryFile(mode='w+b')
        self.addCleanup(log.close)
        contender = subprocess.Popen(self.server_command(occupied_port), stdout=log, stderr=log)
        self.addCleanup(stop_process, contender)
        try:
            contender.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.fail('Server did not reject the occupied port within 5 seconds')
        self.assertNotEqual(contender.returncode, 0, 'Server should fail on an occupied port')
        self.assertIsNone(owner.poll(), 'Server killed the existing port owner')
        self.assertEqual(http_request(occupied_port, 'GET', '/', timeout=2)[1], 'openread-test-port-owner')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__, add_help=False)
    parser.add_argument('--server', default=os.environ.get('OPENREAD_WEB_SERVER', str(SERVER_BINARY)),
                        help='Path to the built openread_web_server executable')
    options, remaining = parser.parse_known_args()
    SERVER_BINARY = Path(options.server).expanduser().resolve()
    unittest.main(argv=[sys.argv[0], *remaining])
