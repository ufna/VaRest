# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Bounded test-only CONNECT relay. Never resolves a supplied hostname."""
import select
import socket
import socketserver
import threading
import time


class Proxy:
    def __init__(self, mode, ports, max_events=512):
        if mode not in ('tunnel', 'deny', 'close', 'silent'):
            raise ValueError('invalid proxy mode')
        self.mode = mode
        self.lifetime_seconds = 40.0 if mode == 'silent' else 12.0
        self.ports = frozenset(int(p) for p in ports)
        if not self.ports or any(p <= 0 or p > 65535 for p in self.ports):
            raise ValueError('invalid target ports')
        if not 1 <= max_events <= 512:
            raise ValueError('invalid journal bound')
        self.max_events = max_events
        self.lock = threading.Condition()
        self.stopping = threading.Event()
        self.events = []
        self.dropped = self.active = self.accepted = self.rejected = 0
        self.next_id = 0
        self.slots = threading.BoundedSemaphore(16)
        self.sockets = set()
        owner = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                owner.handle(self.request)

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = False
            block_on_close = True

            def process_request(self, request, client_address):
                if not owner.slots.acquire(blocking=False):
                    with owner.lock:
                        owner.rejected += 1
                        owner.lock.notify_all()
                    self.shutdown_request(request)
                    return
                with owner.lock:
                    owner.active += 1
                    owner.accepted += 1
                    owner.sockets.add(request)
                    owner.lock.notify_all()
                try:
                    super().process_request(request, client_address)
                except BaseException:
                    owner.release(request)
                    raise

            def process_request_thread(self, request, client_address):
                try:
                    super().process_request_thread(request, client_address)
                finally:
                    owner.release(request)

        self.server = Server(('127.0.0.1', 0), Handler)
        self.address = '127.0.0.1:%d' % self.server.server_address[1]
        self.worker = None

    def release(self, sock):
        with self.lock:
            self.sockets.discard(sock)
            self.active -= 1
            self.lock.notify_all()
        self.slots.release()

    def add(self, ident, kind, **fields):
        event = dict(id=ident, ev=kind, t=time.monotonic(), **fields)
        with self.lock:
            if len(self.events) < self.max_events:
                self.events.append(event)
            else:
                self.dropped += 1
            self.lock.notify_all()

    def snapshot(self):
        with self.lock:
            return dict(mode=self.mode, address=self.address, lifetime_seconds=self.lifetime_seconds, events=list(self.events),
                        dropped=self.dropped, active=self.active, accepted=self.accepted, rejected=self.rejected)

    def wait_idle(self, seconds):
        deadline = time.monotonic() + seconds
        with self.lock:
            while self.active and time.monotonic() < deadline:
                self.lock.wait(max(0, deadline - time.monotonic()))
            return self.active == 0

    def __enter__(self):
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .05})
        self.worker.start()
        return self

    def __exit__(self, *_):
        if self.stopping.is_set():
            return
        self.stopping.set()
        if self.worker is not None:
            self.server.shutdown()
            self.worker.join(1)
        with self.lock:
            active_sockets = list(self.sockets)
        for sock in active_sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.server.server_close()  # non-daemon handlers join after shutdown/50-ms polls
        if not self.wait_idle(1) or (self.worker is not None and self.worker.is_alive()):
            raise RuntimeError('proxy cleanup failed')

    def read_header(self, sock):
        data = bytearray()
        deadline = time.monotonic() + 2
        while not data.endswith(b'\r\n\r\n'):
            if self.stopping.is_set() or time.monotonic() >= deadline or len(data) >= 8192:
                raise ValueError('header deadline or size')
            ready, _, _ = select.select([sock], [], [], .05)
            if ready:
                chunk = sock.recv(1)  # preserves any pipelined bytes for the relay
                if not chunk:
                    raise ValueError('header EOF')
                data.extend(chunk)
        lines = bytes(data[:-4]).decode('ascii').split('\r\n')
        if len(lines) > 128:
            raise ValueError('too many headers')
        method, authority, version = lines[0].split(' ')
        if method != 'CONNECT' or version not in ('HTTP/1.0', 'HTTP/1.1'):
            raise ValueError('not CONNECT')
        host, port_text = authority.split(':')
        if host not in ('127.0.0.1', 'localhost') or not port_text.isdecimal():
            raise ValueError('not loopback authority')
        port = int(port_text)
        if port not in self.ports:
            raise ValueError('target port not allowed')
        headers = []
        for line in lines[1:]:
            name, value = line.split(':', 1)
            if not name or any(ord(c) < 33 or ord(c) > 126 for c in name):
                raise ValueError('bad header name')
            if any(ord(c) < 32 and c != '\t' for c in value):
                raise ValueError('bad header value')
            headers.append([name, value.strip()])
        return authority, port, headers

    def handle(self, sock):
        with self.lock:
            self.next_id += 1
            ident = self.next_id
        deadline = time.monotonic() + self.lifetime_seconds
        sock.settimeout(1)
        try:
            try:
                authority, port, headers = self.read_header(sock)
            except (ValueError, UnicodeError):
                self.add(ident, 'invalid')
                sock.sendall(b'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                return
            self.add(ident, 'connect', authority=authority, headers=headers)
            if self.mode == 'deny':
                sock.sendall(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                self.add(ident, 'denied', status=403)
                return
            if self.mode == 'close':
                sock.shutdown(socket.SHUT_RDWR)
                self.add(ident, 'closed_without_response')
                return
            if self.mode == 'silent':
                self.add(ident, 'silent', lifetime_seconds=self.lifetime_seconds)
                while not self.stopping.is_set() and time.monotonic() < deadline:
                    ready, _, _ = select.select([sock], [], [], .05)
                    if ready and not sock.recv(4096):
                        self.add(ident, 'peer_eof')
                        return
                self.add(ident, 'hold_deadline' if not self.stopping.is_set() else 'stopped')
                return
            with socket.create_connection(('127.0.0.1', port), timeout=1) as upstream:
                self.add(ident, 'upstream', port=port)
                sock.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
                self.add(ident, 'tunnel')
                while not self.stopping.is_set() and time.monotonic() < deadline:
                    readable, _, _ = select.select([sock, upstream], [], [], .05)
                    for source in readable:
                        payload = source.recv(16384)
                        if not payload:
                            return
                        (upstream if source is sock else sock).sendall(payload)
                self.add(ident, 'relay_deadline' if not self.stopping.is_set() else 'stopped')
        except OSError as error:
            self.add(ident, 'io_error', errno=error.errno)
        finally:
            self.add(ident, 'end')
