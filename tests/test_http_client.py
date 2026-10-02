"""Real loopback HTTP/HTTPS regressions for the shared download client.

Uses only the Python standard library and public test certificates. No network,
OpenSSL executable, user database, or operating-system trust changes are needed.
"""

import argparse
from contextlib import contextmanager
import gzip
import http.client
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURES = Path(__file__).resolve().parent / 'fixtures/http_tls'
SERVER_BINARY = ROOT / 'build/bin/ariaread_web_server'
BODY_LIMIT = 32 * 1024 * 1024
SOURCE = json.dumps({'bookSourceName': 'Download fixture',
                     'bookSourceUrl': 'https://example.invalid/test',
                     'searchUrl': '/search?key={{key}}'}).encode()


class DownloadHandler(BaseHTTPRequestHandler):
    def handle(self):
        try:
            super().handle()
        except (ConnectionResetError, BrokenPipeError, ssl.SSLError):
            # Certificate rejection can close the stream before the first
            # HTTP byte reaches this handler.
            pass

    def do_GET(self):
        status = 200
        body = SOURCE
        headers = {}
        if self.path == '/redirect':
            status, body = 302, b''
            headers['Location'] = '/gzip'
        elif self.path == '/loop':
            status, body = 302, b''
            headers['Location'] = '/loop'
        elif self.path == '/gzip':
            body = gzip.compress(SOURCE)
            headers['Content-Encoding'] = 'gzip'
        elif self.path in ('/oversized', '/gzip-oversized'):
            body = b'x' * (BODY_LIMIT + 1)
            if self.path == '/gzip-oversized':
                body = gzip.compress(body)
                headers['Content-Encoding'] = 'gzip'
        elif self.path == '/error':
            status, body = 503, b'unavailable'
        try:
            self.send_response(status)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(body)))
            for name, value in headers.items():
                self.send_header(name, value)
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
            # A response limit deliberately interrupts reading the body.
            pass

    def log_message(self, *_):
        pass


def request(port, method, path, body=None):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=10)
    try:
        connection.request(method, path, body, {'Content-Type': 'application/json'})
        response = connection.getresponse()
        return response.status, response.read(1024 * 1024).decode()
    finally:
        connection.close()


@contextmanager
def application(extra_env=None):
    # The tests own each process and use only an in-memory database. Avoid a
    # developer's proxy or CA settings making a loopback assertion meaningless.
    env = {name: value for name, value in os.environ.items()
           if not name.lower().endswith('_proxy') and name.upper() not in
           ('SSL_CERT_FILE', 'SSL_CERT_DIR', 'CURL_CA_BUNDLE')}
    env['NO_PROXY'] = '*'
    env.update(extra_env or {})
    with socket.socket() as probe:
        probe.bind(('127.0.0.1', 0))
        port = probe.getsockname()[1]
    with tempfile.TemporaryFile(mode='w+b') as log:
        process = subprocess.Popen(
            [str(SERVER_BINARY), '--host', '127.0.0.1', '--port', str(port),
             '--db', ':memory:', '--web-root', str(ROOT / 'bindings/web/ariaread/web')],
            env=env, stdout=log, stderr=log)
        try:
            deadline = time.monotonic() + 10
            while time.monotonic() < deadline:
                if process.poll() is not None:
                    log.seek(0)
                    raise RuntimeError('Server exited: ' + log.read().decode(errors='replace')[-4000:])
                try:
                    if request(port, 'GET', '/api/health')[0] == 200:
                        break
                except (OSError, http.client.HTTPException):
                    pass
                time.sleep(0.03)
            else:
                raise RuntimeError('Server did not become ready within 10 seconds')
            yield port
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=3)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)


class HttpClientTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not SERVER_BINARY.is_file():
            raise RuntimeError(f'Server binary not found: {SERVER_BINARY}')
        cls.http = cls.start_origin()
        cls.https = cls.start_origin(tls=True)

    @classmethod
    def start_origin(cls, tls=False):
        server = ThreadingHTTPServer(('127.0.0.1', 0), DownloadHandler)
        server.daemon_threads = True
        server.block_on_close = False
        cls.addClassCleanup(server.server_close)
        if tls:
            context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            context.load_cert_chain(FIXTURES / 'server.pem', FIXTURES / 'server-key.pem')
            server.socket = context.wrap_socket(server.socket, server_side=True)
        thread = threading.Thread(target=server.serve_forever,
                                  kwargs={'poll_interval': 0.02}, daemon=True)
        thread.start()
        cls.addClassCleanup(thread.join, 3)
        cls.addClassCleanup(server.shutdown)
        return server

    def download(self, port, path='/source', tls=False, hostname='127.0.0.1'):
        origin = self.https if tls else self.http
        url = f'{"https" if tls else "http"}://{hostname}:{origin.server_port}{path}'
        return request(port, 'POST', '/api/sources/url', json.dumps({'url': url}))

    def assert_imported(self, result):
        status, body = result
        self.assertEqual(status, 200, body)
        self.assertEqual(json.loads(body)['loaded'], 1, body)

    def assert_download_failed(self, result):
        status, body = result
        self.assertEqual(status, 500, body)
        self.assertIn('Download failed:', json.loads(body)['error'])

    def test_http_redirect_and_gzip(self):
        for path in ('/source', '/gzip', '/redirect'):
            with self.subTest(path=path):
                with application() as port:
                    self.assert_imported(self.download(port, path))

    def test_http_error_preserves_status(self):
        with application() as port:
            status, body = self.download(port, '/error')
            self.assertEqual(status, 500, body)
            self.assertEqual(json.loads(body)['error'], 'HTTP error 503')

    def test_redirect_loop_is_bounded(self):
        with application() as port:
            self.assert_download_failed(self.download(port, '/loop'))

    def test_decoded_response_limit(self):
        with application() as port:
            for path in ('/oversized', '/gzip-oversized'):
                with self.subTest(path=path):
                    status, body = self.download(port, path)
                    self.assertEqual(status, 500, body)
                    self.assertIn(f'Response body exceeds {BODY_LIMIT} bytes', body)
                    self.assertEqual(request(port, 'GET', '/api/health')[0], 200)

    def test_tls_untrusted_certificate_is_rejected(self):
        with application() as port:
            self.assert_download_failed(self.download(port, tls=True))

    def test_tls_explicit_ca_and_environment_precedence(self):
        with application({'SSL_CERT_FILE': str(FIXTURES / 'ca.pem'),
                          'CURL_CA_BUNDLE': str(FIXTURES / 'wrong-ca.pem')}) as port:
            self.assert_imported(self.download(port, tls=True))

    def test_tls_legacy_curl_ca_bundle(self):
        with application({'CURL_CA_BUNDLE': str(FIXTURES / 'ca.pem')}) as port:
            self.assert_imported(self.download(port, tls=True))

    def test_tls_wrong_explicit_ca_is_rejected(self):
        with application({'SSL_CERT_FILE': str(FIXTURES / 'wrong-ca.pem'),
                          'CURL_CA_BUNDLE': str(FIXTURES / 'ca.pem')}) as port:
            self.assert_download_failed(self.download(port, tls=True))

    def test_tls_hostname_mismatch_is_rejected(self):
        with application({'SSL_CERT_FILE': str(FIXTURES / 'ca.pem')}) as port:
            self.assert_download_failed(self.download(port, tls=True, hostname='localhost'))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--server', type=Path, default=SERVER_BINARY)
    args, unittest_args = parser.parse_known_args()
    SERVER_BINARY = args.server.resolve()
    unittest.main(argv=[__file__, *unittest_args])
