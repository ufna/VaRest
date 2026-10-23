#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import contextlib
import pathlib
import select
import socket
import socketserver
import sys
import threading
import time
import unittest
from types import SimpleNamespace
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.dont_write_bytecode = True
from proxy import Proxy


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(2)
        try:
            while True:
                data = self.request.recv(4096)
                if not data:
                    return
                self.request.sendall(data)
        except OSError:
            return


@contextlib.contextmanager
def target():
    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Echo) as server:
        server.daemon_threads = False
        worker = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .05})
        worker.start()
        try:
            yield server.server_address[1]
        finally:
            server.shutdown()
            worker.join(1)
            if worker.is_alive():
                raise AssertionError('target did not stop')


def dial(proxy):
    host, port = proxy.address.split(':')
    return socket.create_connection((host, int(port)), timeout=3)


def header(sock):
    result = bytearray()
    while not result.endswith(b'\r\n\r\n'):
        part = sock.recv(1)
        if not part:
            break
        result.extend(part)
        if len(result) > 8192:
            raise AssertionError('oversized response')
    return bytes(result)


class ProxyTest(unittest.TestCase):
    def test_tunnel_and_separate_headers(self):
        with target() as port, Proxy('tunnel', [port]) as proxy, dial(proxy) as sock:
            sock.sendall(('CONNECT localhost:%d HTTP/1.1\r\nHost: localhost:%d\r\n\r\n' % (port, port)).encode())
            self.assertIn(b' 200 ', header(sock))
            payload = b'Authorization: Bearer b1-probe-marker\r\n\x00tunnel'
            sock.sendall(payload)
            received = bytearray()
            while len(received) < len(payload):
                chunk = sock.recv(len(payload) - len(received))
                self.assertTrue(chunk, 'tunnel closed before echo completed')
                received.extend(chunk)
            self.assertEqual(bytes(received), payload)
            sock.shutdown(socket.SHUT_RDWR)
            self.assertTrue(proxy.wait_idle(1))
            events = proxy.snapshot()['events']
            connects = [e for e in events if e['ev'] == 'connect']
            self.assertEqual(len(connects), 1)
            self.assertEqual(connects[0]['authority'], 'localhost:%d' % port)
            self.assertNotIn('Authorization', str(connects[0]['headers']))
            self.assertEqual(sum(e['ev'] == 'upstream' for e in events), 1)

    def test_denial_close_silence_and_cleanup(self):
        for mode in ('deny', 'close', 'silent'):
            with self.subTest(mode=mode), target() as port, Proxy(mode, [port]) as proxy, dial(proxy) as sock:
                try:
                    sock.sendall(('CONNECT 127.0.0.1:%d HTTP/1.1\r\n\r\n' % port).encode())
                    if mode == 'deny':
                        self.assertIn(b' 403 ', header(sock))
                    elif mode == 'close':
                        self.assertEqual(sock.recv(1), b'')
                    else:
                        sock.settimeout(.15)
                        with self.assertRaises(socket.timeout):
                            sock.recv(1)
                    before = time.monotonic()
                    proxy.__exit__(None, None, None)
                    self.assertLess(time.monotonic() - before, 1)
                    self.assertTrue(proxy.wait_idle(0))
                    state = proxy.snapshot()
                    self.assertEqual(state['lifetime_seconds'], 40.0 if mode == 'silent' else 12.0)
                    witness = {'deny': 'denied', 'close': 'closed_without_response', 'silent': 'silent'}[mode]
                    self.assertTrue(any(e['ev'] == witness for e in state['events']))
                    if mode == 'deny':
                        self.assertTrue(any(e['ev'] == 'denied' and e['status'] == 403 for e in state['events']))
                    self.assertFalse(any(e['ev'] == 'upstream' for e in proxy.snapshot()['events']))
                finally:
                    sock.close()
                    proxy.__exit__(None, None, None)

    def test_silent_shutdown_is_not_peer_eof(self):
        for phase in ('before_select', 'inside_recv'):
            with self.subTest(phase=phase), Proxy('silent', [1]) as proxy:
                entered = threading.Event()
                resume = threading.Event()
                errors = []
                closer = None
                real_handle = proxy.handle

                def gate():
                    entered.set()
                    if not resume.wait(3):
                        errors.append('handler gate timed out')

                class GatedSocket:
                    def __init__(self, raw):
                        self.raw = raw

                    def __getattr__(self, name):
                        return getattr(self.raw, name)

                    def recv(self, size):
                        # Only the silent read uses 4096; header reads use 1.
                        if phase == 'inside_recv' and size == 4096:
                            gate()
                        return self.raw.recv(size)

                def gated_select(*args):
                    if phase == 'before_select' and any(
                            e['ev'] == 'silent' for e in proxy.snapshot()['events']):
                        gate()
                    return select.select(*args)

                def stop():
                    try:
                        proxy.__exit__(None, None, None)
                    except BaseException as error:
                        errors.append(repr(error))

                with patch.object(proxy, 'handle', lambda raw: real_handle(GatedSocket(raw))), \
                        patch('proxy.select', SimpleNamespace(select=gated_select)), dial(proxy) as sock:
                    try:
                        sock.sendall(b'CONNECT 127.0.0.1:1 HTTP/1.1\r\n\r\n')
                        if phase == 'inside_recv':
                            # Real EOF makes select ready. Pause within recv so
                            # stop begins AFTER the post-select check, before
                            # the real read. An overlapping stop must dominate.
                            sock.shutdown(socket.SHUT_WR)
                        self.assertTrue(entered.wait(2), 'silent handler did not reach gate')
                        closer = threading.Thread(target=stop)
                        closer.start()
                        # Wait for real fixture socket shutdown, not a delay.
                        # The client is still open (fully open in before_select).
                        self.assertEqual(sock.recv(1), b'')
                    finally:
                        resume.set()
                        if closer is not None:
                            closer.join(3)
                    if closer is not None:
                        self.assertFalse(closer.is_alive(), 'fixture stop did not join')
                    self.assertEqual(errors, [])
                    self.assertTrue(proxy.wait_idle(0))
                    self.assertEqual([e['ev'] for e in proxy.snapshot()['events']],
                                     ['connect', 'silent', 'stopped', 'end'])

    def test_silent_client_eof_before_fixture_stop(self):
        with Proxy('silent', [1]) as proxy, dial(proxy) as sock:
            sock.sendall(b'CONNECT 127.0.0.1:1 HTTP/1.1\r\n\r\n')
            with proxy.lock:
                self.assertTrue(proxy.lock.wait_for(
                    lambda: any(e['ev'] == 'silent' for e in proxy.events), timeout=2))
            sock.shutdown(socket.SHUT_RDWR)
            self.assertTrue(proxy.wait_idle(1))
            self.assertFalse(proxy.stopping.is_set())
            events = proxy.snapshot()['events']
            self.assertEqual([e['ev'] for e in events], ['connect', 'silent', 'peer_eof', 'end'])
            self.assertEqual(len({e['id'] for e in events}), 1)

    def test_bad_authority_header_limit_and_slow_header(self):
        with target() as port, Proxy('tunnel', [port]) as proxy:
            for authority in ('example.org:%d' % port, '127.0.0.1:1', '127.0.0.1:%d?deny=403' % port):
                with dial(proxy) as sock:
                    sock.sendall(('CONNECT %s HTTP/1.1\r\n\r\n' % authority).encode())
                    self.assertIn(b' 400 ', header(sock))
            with dial(proxy) as sock:
                sock.sendall(b'CONNECT ' + b'x' * 8200)
                self.assertIn(b' 400 ', header(sock))
            with dial(proxy) as sock:
                sock.sendall(('CONNECT localhost:%d HTTP/1.1\r\n' % port).encode() + b'X: y\r\n' * 128 + b'\r\n')
                self.assertIn(b' 400 ', header(sock))
            with dial(proxy) as sock:
                sock.sendall(b'C')
                before = time.monotonic()
                self.assertIn(b' 400 ', header(sock))
                self.assertLess(time.monotonic() - before, 2.8)
            self.assertTrue(proxy.wait_idle(1))
            self.assertFalse(any(e['ev'] == 'upstream' for e in proxy.snapshot()['events']))

    def test_handler_and_journal_bounds(self):
        with target() as port, Proxy('silent', [port], max_events=3) as proxy:
            sockets = []
            try:
                for _ in range(24):
                    sock = dial(proxy)
                    sockets.append(sock)
                    sock.sendall(('CONNECT 127.0.0.1:%d HTTP/1.1\r\n\r\n' % port).encode())
                # Wait on the observable admission/rejection state, not a fixed sleep.
                deadline = time.monotonic() + 2
                while proxy.snapshot()['accepted'] + proxy.snapshot()['rejected'] < 24 and time.monotonic() < deadline:
                    threading.Event().wait(.01)
                state = proxy.snapshot()
                self.assertLessEqual(state['active'], 16)
                self.assertGreater(state['rejected'], 0)
                self.assertLessEqual(len(state['events']), 3)
                self.assertGreater(state['dropped'], 0)
            finally:
                for sock in sockets:
                    sock.close()
            self.assertTrue(proxy.wait_idle(1))


if __name__ == '__main__':
    unittest.main()
