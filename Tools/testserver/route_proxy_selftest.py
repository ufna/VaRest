#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import errno
import pathlib
import socket
import struct
import sys
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from route_proxy import RouteProxy, Ipv6Unavailable, client_hello_sni
from proxy_selftest import target


def hello(name=b'localhost'):
    names = b'\x00' + struct.pack('!H', len(name)) + name
    extension = struct.pack('!H', len(names)) + names
    extensions = struct.pack('!HH', 0, len(extension)) + extension
    body = b'\x03\x03' + bytes(32) + b'\x00' + b'\x00\x02\x00\x2f' + b'\x01\x00'
    body += struct.pack('!H', len(extensions)) + extensions
    handshake = b'\x01' + len(body).to_bytes(3, 'big') + body
    return b'\x16\x03\x01' + struct.pack('!H', len(handshake)) + handshake


def connect(proxy, port):
    address = proxy.server.server_address
    sock = socket.create_connection((address[0], address[1]), timeout=2)
    try:
        sock.sendall(('CONNECT localhost:%d HTTP/1.1\r\nHost: localhost:%d\r\n\r\n' % (port, port)).encode())
        response = bytearray()
        while not response.endswith(b'\r\n\r\n'):
            part = sock.recv(1)
            if not part:
                raise AssertionError('CONNECT ended')
            response += part
        if not response.startswith(b'HTTP/1.1 200 '):
            raise AssertionError(response)
        return sock
    except BaseException:
        sock.close()
        raise


class RouteFixtureTest(unittest.TestCase):
    def receive_exact(self, sock, count):
        result = bytearray()
        while len(result) < count:
            chunk = sock.recv(count - len(result))
            self.assertTrue(chunk, 'relay closed before payload completed')
            result.extend(chunk)
        return bytes(result)

    def test_exact_sni_and_malformed_boundaries(self):
        self.assertEqual(client_hello_sni(hello()), 'localhost')
        for data in (b'', hello()[:-1], b'\x16\x03\x01\xff\xff', hello(b'bad\x00name')):
            with self.assertRaises(ValueError):
                client_hello_sni(data)
        self.assertEqual(client_hello_sni(hello(b'example.test')), 'example.test')

    def test_hold_real_record_until_explicit_release(self):
        with target() as port, RouteProxy([port], hold_tls=True) as proxy:
            with connect(proxy, port) as sock:
                payload = hello()
                sock.sendall(payload)
                self.assertTrue(proxy.hello_seen.wait(2))
                self.assertTrue(proxy.server_held.wait(2))
                sock.settimeout(.05)
                try:
                    premature = sock.recv(1)
                except socket.timeout:
                    pass
                else:
                    self.assertTrue(premature, 'relay ended before explicit release')
                    self.fail('server bytes arrived before explicit release: %r' % premature)
                proxy.release_tls.set()
                sock.settimeout(2)
                self.assertEqual(self.receive_exact(sock, len(payload)), payload)
            self.assertTrue(proxy.wait_idle(2))
        kinds = [row['ev'] for row in proxy.snapshot()['events']]
        self.assertLess(kinds.index('client_hello'), kinds.index('tls_hold'))
        self.assertLess(kinds.index('tls_hold'), kinds.index('tls_release'))
        self.assertEqual(kinds.count('tls_release'), 1)

    def test_stop_does_not_release(self):
        with target() as port, RouteProxy([port], hold_tls=True) as proxy:
            with connect(proxy, port) as sock:
                sock.sendall(hello())
                self.assertTrue(proxy.server_held.wait(2))
                proxy.__exit__(None, None, None)
                self.assertFalse(proxy.release_tls.is_set())
                self.assertNotIn('tls_release', [e['ev'] for e in proxy.snapshot()['events']])
                self.assertEqual(proxy.snapshot()['active'], 0)

    def test_plain_route_is_transparent(self):
        with target() as port, RouteProxy([port]) as proxy:
            with connect(proxy, port) as sock:
                sock.sendall(b'ordinary')
                self.assertEqual(self.receive_exact(sock, 8), b'ordinary')
            self.assertTrue(proxy.wait_idle(2))
            self.assertEqual(proxy.snapshot()['dropped'], 0)

    def test_ipv6_peer_is_distinct(self):
        with target() as port:
            try:
                proxy = RouteProxy([port], family=socket.AF_INET6)
            except Ipv6Unavailable as error:
                self.skipTest('IPv6 loopback unavailable: %s' % error)
            with proxy:
                self.assertTrue(proxy.address.startswith('[::1]:'))
                with connect(proxy, port) as sock:
                    sock.sendall(b'v6')
                    self.assertEqual(self.receive_exact(sock, 2), b'v6')
                self.assertTrue(proxy.wait_idle(2))
                self.assertEqual(proxy.server.server_address[0], '::1')

    def test_fragmented_hello_and_record_bound(self):
        with target() as port, RouteProxy([port], hold_tls=True) as proxy:
            with connect(proxy, port) as sock:
                payload = hello()
                for byte in payload:
                    sock.sendall(bytes([byte]))
                self.assertTrue(proxy.server_held.wait(2))
                proxy.release_tls.set()
                self.assertEqual(self.receive_exact(sock, len(payload)), payload)
            self.assertTrue(proxy.wait_idle(2))
        with target() as port, RouteProxy([port], hold_tls=True) as proxy:
            with connect(proxy, port) as sock:
                sock.sendall(b'\x16\x03\x01\x48\x01')
                self.assertEqual(sock.recv(1), b'')
            self.assertTrue(proxy.wait_idle(2))
            kinds = [row['ev'] for row in proxy.snapshot()['events']]
            self.assertIn('fixture_error', kinds)
            self.assertNotIn('client_hello', kinds)
            self.assertNotIn('tls_release', kinds)

    def test_hold_deadline_never_releases(self):
        with target() as port, RouteProxy([port], hold_tls=True) as proxy:
            self.assertEqual(proxy.lifetime_seconds, 12.0)
            # Shorten only this unit instance; raw acquisition keeps 12 seconds.
            proxy.lifetime_seconds = .3
            with connect(proxy, port) as sock:
                sock.sendall(hello())
                self.assertTrue(proxy.server_held.wait(2))
                self.assertTrue(proxy.wait_idle(2))
                self.assertEqual(sock.recv(1), b'')
            self.assertFalse(proxy.release_tls.is_set())
            kinds = [row['ev'] for row in proxy.snapshot()['events']]
            self.assertIn('hold_deadline', kinds)
            self.assertNotIn('tls_release', kinds)

    def test_ipv6_unavailable_is_narrow_and_closes_sockets(self):
        real_socket = socket.socket
        for code in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT,
                     errno.EADDRNOTAVAIL, errno.EMFILE, errno.EACCES):
            acquired = []

            class FailingV6Socket(real_socket):
                def __init__(self, *args, **kwargs):
                    super().__init__(*args, **kwargs)
                    acquired.append(self)

                def bind(self, address):
                    if self.family == socket.AF_INET6:
                        raise OSError(code, 'injected IPv6 bind failure')
                    return super().bind(address)

            with self.subTest(errno=code), patch('socket.socket', FailingV6Socket):
                with self.assertRaises(OSError) as caught:
                    RouteProxy([1], family=socket.AF_INET6)
            expected = code in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT, errno.EADDRNOTAVAIL)
            self.assertEqual(isinstance(caught.exception, Ipv6Unavailable), expected)
            self.assertEqual(caught.exception.errno, code)
            if expected:
                self.assertEqual(caught.exception.evidence,
                                 dict(operation='bind-ipv6-loopback', family='AF_INET6',
                                      host='::1', errno=code))
            self.assertTrue(acquired)
            self.assertTrue(all(sock.fileno() == -1 for sock in acquired))


if __name__ == '__main__':
    unittest.main()
