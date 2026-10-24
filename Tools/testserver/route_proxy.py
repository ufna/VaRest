# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Raw-probe loopback relay. B1 Proxy remains unchanged."""
import errno
import select
import socket
import struct
import threading
import time
from proxy import Proxy


def client_hello_sni(record):
    def take(data, offset, count):
        if offset < 0 or count < 0 or offset + count > len(data):
            raise ValueError('truncated ClientHello')
        return data[offset:offset + count], offset + count
    if len(record) < 9 or record[0] != 22:
        raise ValueError('not a TLS handshake record')
    length = int.from_bytes(record[3:5], 'big')
    if length > 18432 or length + 5 != len(record):
        raise ValueError('record length')
    data = record[5:]
    if data[0] != 1 or int.from_bytes(data[1:4], 'big') + 4 != len(data):
        raise ValueError('one complete ClientHello required')
    _, pos = take(data, 4, 34)
    session, pos = take(data, pos, 1)
    _, pos = take(data, pos, session[0])
    size, pos = take(data, pos, 2)
    _, pos = take(data, pos, int.from_bytes(size, 'big'))
    size, pos = take(data, pos, 1)
    _, pos = take(data, pos, size[0])
    size, pos = take(data, pos, 2)
    extensions, pos = take(data, pos, int.from_bytes(size, 'big'))
    if pos != len(data):
        raise ValueError('trailing ClientHello bytes')
    cursor = 0
    answer = None
    while cursor < len(extensions):
        head, cursor = take(extensions, cursor, 4)
        kind, count = struct.unpack('!HH', head)
        body, cursor = take(extensions, cursor, count)
        if kind != 0:
            continue
        size, index = take(body, 0, 2)
        if int.from_bytes(size, 'big') + 2 != len(body):
            raise ValueError('name-list length')
        while index < len(body):
            head, index = take(body, index, 3)
            raw, index = take(body, index, int.from_bytes(head[1:], 'big'))
            if head[0] == 0:
                if answer is not None or not raw or any(c < 33 or c > 126 for c in raw):
                    raise ValueError('invalid or duplicate hostname')
                answer = raw.decode('ascii')
    return '' if answer is None else answer


class Ipv6Unavailable(OSError):
    def __init__(self, error):
        super().__init__(error.errno, str(error))
        self.evidence = dict(operation='bind-ipv6-loopback', family='AF_INET6', host='::1', errno=error.errno)


class RouteProxy(Proxy):
    def __init__(self, ports, family=socket.AF_INET, hold_tls=False, inspect_tls=False):
        super().__init__('tunnel', ports)
        self.hold_tls = hold_tls
        self.inspect_tls = inspect_tls or hold_tls
        self.hello_seen = threading.Event()
        self.server_held = threading.Event()
        self.release_tls = threading.Event()
        self.family = family
        if family == socket.AF_INET6:
            old_server = self.server
            handler = old_server.RequestHandlerClass
            base = type(old_server)
            old_server.server_close()

            class V6Server(base):
                address_family = socket.AF_INET6

            try:
                self.server = V6Server(('::1', 0), handler)
            except OSError as error:
                if error.errno in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT, errno.EADDRNOTAVAIL):
                    raise Ipv6Unavailable(error) from error
                raise
            self.address = '[::1]:%d' % self.server.server_address[1]
        elif family != socket.AF_INET:
            self.server.server_close()
            raise ValueError('unsupported family')

    def handle(self, sock):
        with self.lock:
            self.next_id += 1
            ident = self.next_id
        deadline = time.monotonic() + self.lifetime_seconds
        operation = 'read-connect-header'

        def send(destination, payload):
            nonlocal operation
            operation = 'send-client' if destination is sock else 'send-upstream'
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('relay lifetime expired')
            destination.settimeout(min(1, remaining))
            destination.sendall(payload)

        sock.settimeout(1)
        try:
            authority, port, headers = self.read_header(sock)
            self.add(ident, 'connect', authority=authority, headers=headers,
                     peer_host=sock.getsockname()[0], peer_port=sock.getsockname()[1])
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                self.add(ident, 'relay_deadline')
                return
            operation = 'connect-upstream'
            with socket.create_connection(('127.0.0.1', port), timeout=min(1, remaining)) as upstream:
                upstream.settimeout(1)
                self.add(ident, 'upstream', host='127.0.0.1', port=port)
                send(sock, b'HTTP/1.1 200 Connection Established\r\n\r\n')
                self.add(ident, 'tunnel')
                prefix = bytearray()
                hello_done = False
                released = False
                while not self.stopping.is_set() and time.monotonic() < deadline:
                    readable, _, _ = select.select(
                        [sock, upstream], [], [], min(.02, max(0, deadline - time.monotonic())))
                    for source in readable:
                        if self.stopping.is_set():
                            self.add(ident, 'stopped')
                            return
                        remaining = deadline - time.monotonic()
                        if remaining <= 0:
                            self.add(ident, 'relay_deadline')
                            return
                        # Read only the first record while parsing it. Coalesced
                        # later records stay in the socket rather than consuming
                        # the bounded ClientHello accumulation buffer.
                        count = 16384
                        if source is sock and self.inspect_tls and not hello_done:
                            count = (5 if len(prefix) < 5 else
                                     5 + int.from_bytes(prefix[3:5], 'big')) - len(prefix)
                        source.settimeout(min(1, remaining))
                        operation = 'recv-client' if source is sock else 'recv-upstream'
                        try:
                            payload = source.recv(count)
                        except ConnectionResetError as error:
                            if source is not sock or error.errno != errno.ECONNRESET or not (self.inspect_tls and hello_done):
                                raise
                            if self.stopping.is_set():
                                self.add(ident, 'stopped')
                            elif time.monotonic() >= deadline:
                                self.add(ident, 'relay_deadline')
                            else:
                                self.add(ident, 'peer_reset', operation='recv-client', errno=error.errno,
                                         phase='after-forwarded-client-hello')
                            return
                        if self.stopping.is_set():
                            self.add(ident, 'stopped')
                            return
                        if not payload:
                            self.add(ident, 'peer_eof' if source is sock else 'upstream_eof')
                            return
                        if source is sock and self.inspect_tls and not hello_done:
                            prefix.extend(payload)
                            if len(prefix) > 18437:
                                raise ValueError('ClientHello accumulation bound')
                            if len(prefix) < 5:
                                continue
                            needed = 5 + int.from_bytes(prefix[3:5], 'big')
                            if needed > 18437:
                                raise ValueError('TLS record bound')
                            if len(prefix) < needed:
                                continue
                            sni = client_hello_sni(bytes(prefix[:needed]))
                            self.add(ident, 'client_hello', sni=sni, record_bytes=needed)
                            send(upstream, prefix)
                            prefix.clear()
                            hello_done = True
                            self.hello_seen.set()
                            continue
                        if source is upstream and self.hold_tls and not released:
                            if not hello_done:
                                raise ValueError('server bytes before ClientHello')
                            self.add(ident, 'tls_hold', bytes=len(payload))
                            self.server_held.set()
                            while not self.release_tls.wait(.02):
                                if self.stopping.is_set() or time.monotonic() >= deadline:
                                    self.add(ident, 'hold_stopped' if self.stopping.is_set() else 'hold_deadline')
                                    return
                            if self.stopping.is_set():
                                self.add(ident, 'hold_stopped')
                                return
                            if time.monotonic() >= deadline:
                                self.add(ident, 'hold_deadline')
                                return
                            self.add(ident, 'tls_release', bytes=len(payload))
                            send(sock, payload)
                            released = True
                            continue
                        send(upstream if source is sock else sock, payload)
                self.add(ident, 'stopped' if self.stopping.is_set() else 'relay_deadline')
        except (OSError, ValueError, UnicodeError) as error:
            self.add(ident, 'fixture_error', error_kind=type(error).__name__, detail=str(error),
                     operation=operation, errno=getattr(error, 'errno', None))
        finally:
            self.add(ident, 'end')
