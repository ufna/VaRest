#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Checks Tools/testserver/ws.py with a standard-library RFC 6455 client before UE tests rely on it (WebSocket spec 16.3).

    python3 -I Tools/testserver/ws_selftest.py [-v]
"""

import base64
import hashlib
import http.client
import json
import os
import random
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
# python3 -I drops the script directory from sys.path (WebSocket spec 16.3)
sys.path.insert(0, HERE)
# No __pycache__ in the source tree (it is not ignored)
sys.dont_write_bytecode = True
import server  # noqa: E402
import ws  # noqa: E402


class Client:
    """A stdlib RFC 6455 client, enough to check ws.py: it masks every frame unless told not to, as a client must"""

    def __init__(self, port, path, headers=(), protocols=(), tls=None, key=None, version="13", timeout=10.0):
        self.raw = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock = tls.wrap_socket(self.raw, server_hostname="localhost") if tls else self.raw
        self.key = key if key is not None else base64.b64encode(os.urandom(16)).decode("ascii")
        lines = ["GET %s HTTP/1.1" % path, "Host: 127.0.0.1:%d" % port, "Upgrade: websocket", "Connection: Upgrade",
                 "Sec-WebSocket-Key: " + self.key, "Sec-WebSocket-Version: " + version]
        if protocols:
            lines.append("Sec-WebSocket-Protocol: " + ", ".join(protocols))
        lines += ["%s: %s" % pair for pair in headers]
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        self.rfile = self.sock.makefile("rb")
        status_line = self.rfile.readline(65536)
        # 0: the server ended the connection without a status line
        self.status = int(status_line.split()[1]) if status_line.startswith(b"HTTP/") else 0
        self.headers = {}
        while status_line:
            line = self.rfile.readline(65536)
            if line in (b"\r\n", b"\n", b""):
                break
            name, _, value = line.decode("latin-1").partition(":")
            self.headers[name.strip().lower()] = value.strip()
        self.body = b""
        if self.status not in (0, 101):
            length = int(self.headers.get("content-length", "0"))
            self.body = self.rfile.read(length) if length else b""

    @property
    def accepted(self):
        return self.status == 101 and self.headers.get("sec-websocket-accept") == ws.accept_value(self.key)

    def send(self, op, payload=b"", fin=True, mask=True):
        first = (0x80 if fin else 0) | op
        flag = 0x80 if mask else 0
        length = len(payload)
        if length < 126:
            head = struct.pack("!BB", first, flag | length)
        elif length < 65536:
            head = struct.pack("!BBH", first, flag | 126, length)
        else:
            head = struct.pack("!BBQ", first, flag | 127, length)
        if mask:
            key = os.urandom(4)
            head += key
            payload = ws.unmask(payload, key)  # XOR with the mask is its own inverse
        self.sock.sendall(head + payload)

    def read(self, count):
        data = self.rfile.read(count) if count else b""
        if len(data) < count:
            raise EOFError("the server closed the connection")
        return data

    def recv(self):
        """One frame: (fin, op, payload)"""
        first, second = self.read(2)
        if second & 0x80:
            raise AssertionError("a server frame must not be masked (RFC 6455 5.1)")
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read(8))[0]
        return first >> 7, first & 0x0F, self.read(length)

    def message(self):
        """The next data message as (kind, bytes), or the next control frame as ("close"|"ping"|"pong", payload)"""
        parts, kind = [], None
        while True:
            fin, op, payload = self.recv()
            if op >= 0x8:
                return {ws.OP_CLOSE: "close", ws.OP_PING: "ping", ws.OP_PONG: "pong"}[op], payload
            if op != ws.OP_CONT:
                kind = "text" if op == ws.OP_TEXT else "binary"
            parts.append(payload)
            if fin:
                return kind, b"".join(parts)

    def close(self, code=1000, reason=b""):
        self.send(ws.OP_CLOSE, struct.pack("!H", code) + reason)

    def shutdown(self):
        """Ends TCP now: close() alone leaves the descriptor open while rfile (makefile) still references the socket"""
        for end in (lambda: self.raw.shutdown(socket.SHUT_RDWR), self.rfile.close, self.sock.close, self.raw.close):
            try:
                end()
            except OSError:
                pass


def raw(port, data):
    """Sends bytes as they are and reads until the server closes"""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        connection.sendall(data)
        received = b""
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                return received
            received += chunk


def start(make):
    httpd = make("127.0.0.1", 0)
    threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
    return httpd


def close_code(payload):
    return struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else None


class WsTestBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = start(server.make_server)
        cls.port = cls.httpd.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def connect(self, path, **kwargs):
        client = Client(self.port, path, **kwargs)
        self.addCleanup(client.shutdown)
        return client

    def journal(self, key, connections=1, wait_ms=5000):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            connection.request("GET", "/ws/journal?key=%s&connections=%d&wait_ms=%d" % (key, connections, wait_ms))
            return json.loads(connection.getresponse().read())
        finally:
            connection.close()

    def ended(self, key, connections=1):
        """The journal once that many connections with the key have ended: completion, asserted"""
        journal = self.journal(key, connections)
        self.assertTrue(journal["ended"], journal)
        return journal

    @staticmethod
    def of(journal, ev, conn=None):
        return [event for event in journal["events"] if event["ev"] == ev and (conn is None or event["conn"] == conn)]


class CoreTest(WsTestBase):
    def test_accept_value_matches_the_rfc_example(self):
        # RFC 6455 1.3: this key and this answer
        self.assertEqual(ws.accept_value("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

    def test_echo(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/echo?key=" + key)
        self.assertTrue(client.accepted, client.headers)
        client.send(ws.OP_TEXT, "привет".encode("utf-8"))
        self.assertEqual(client.message(), ("text", "привет".encode("utf-8")))
        client.send(ws.OP_BINARY, b"\x00\xff")
        self.assertEqual(client.message(), ("binary", b"\x00\xff"))
        # A fragmented message with a PING between its fragments (RFC 6455 5.4 allows control frames there)
        client.send(ws.OP_TEXT, b"ab", fin=False)
        client.send(ws.OP_PING, b"in-between")
        client.send(ws.OP_CONT, b"cd", fin=False)
        client.send(ws.OP_CONT, b"ef")
        self.assertEqual(client.message(), ("pong", b"in-between"))
        self.assertEqual(client.message(), ("text", b"abcdef"))
        client.close(1000, b"bye")
        self.assertEqual(client.message(), ("close", struct.pack("!H", 1000)))
        journal = self.ended(key)
        self.assertEqual([event["kind"] for event in self.of(journal, "message")], ["text", "binary", "text"])
        self.assertEqual(self.of(journal, "message")[0]["text"], "привет")
        self.assertEqual((self.of(journal, "close")[0]["code"], self.of(journal, "close")[0]["reason"]), (1000, "bye"))
        self.assertEqual(self.of(journal, "request")[0]["target"], "/ws/echo?key=" + key)
        self.assertEqual(self.of(journal, "response")[0]["status"], 101)

    def test_echo_in_frames(self):
        client = self.connect("/ws/echo?fragment=4")
        client.send(ws.OP_TEXT, b"0123456789")
        self.assertEqual([client.recv() for _ in range(3)], [(0, ws.OP_TEXT, b"0123"), (0, ws.OP_CONT, b"4567"), (1, ws.OP_CONT, b"89")])

    def test_status_refusals(self):
        refused = self.connect("/ws/status/403")
        self.assertEqual((refused.status, json.loads(refused.body)), (403, {"status": 403}))
        moved = self.connect("/ws/status/302?location=/ws/echo")
        self.assertEqual((moved.status, moved.headers["location"]), (302, "/ws/echo"))
        self.assertEqual(self.connect("/ws/status/401").headers["www-authenticate"], "Bearer")
        self.assertEqual(self.connect("/ws/status/101").status, 400)

    def test_status_redirect_codes(self):
        # Task 10 review M-2: the transport suite asks every redirect status LWS may follow
        for code in (301, 302, 303, 307, 308):
            moved = self.connect("/ws/status/%d?location=%s" % (code, urllib.parse.quote("/ws/echo?key=k", safe="")))
            self.assertEqual((moved.status, moved.headers.get("location"), json.loads(moved.body)),
                             (code, "/ws/echo?key=k", {"status": code}), code)

    def test_handshake_checked(self):
        self.assertEqual(self.connect("/ws/echo", version="12").status, 426)
        self.assertEqual(self.connect("/ws/echo", key="short").status, 400)
        self.assertEqual(self.connect("/ws/nope").status, 404)
        self.assertEqual(self.connect("/ws/echo?fragment=x").status, 400)
        upgrade = (b"Upgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n")
        answer = raw(self.port, b"POST /ws/echo HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nContent-Length: 0\r\n" + upgrade + b"\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 400 "), answer[:80])
        answer = raw(self.port, b"GET /ws/echo HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n" + upgrade + b"\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 400 "), answer[:80])

    def test_protocol_errors_close_1002_and_the_server_goes_on(self):
        # Review focus: what a hostile or broken client sends is closed with 1002 (RFC 6455 5.2, 5.4, 5.5, 7.4.1)
        cases = [
            ("unmasked", lambda c: c.send(ws.OP_TEXT, b"x", mask=False)),
            # Header alone, no payload sent: the server must refuse from the header, not after reading 126 bytes
            ("126-byte control", lambda c: c.sock.sendall(bytes([0x80 | ws.OP_PING, 0x80 | 126]) + struct.pack("!H", 126))),
            ("fragmented control", lambda c: c.send(ws.OP_PING, b"p", fin=False)),
            ("continuation first", lambda c: c.send(ws.OP_CONT, b"x")),
            ("message in a message", lambda c: (c.send(ws.OP_TEXT, b"a", fin=False), c.send(ws.OP_TEXT, b"b"))),
            ("reserved bit", lambda c: c.sock.sendall(bytes([0x80 | 0x40 | ws.OP_TEXT, 0x80]) + b"\x00\x00\x00\x00")),
            ("reserved opcode", lambda c: c.send(0x3, b"x")),
        ]
        for name, misbehave in cases:
            client = self.connect("/ws/echo")
            misbehave(client)
            kind, payload = client.message()
            self.assertEqual((kind, close_code(payload)), ("close", 1002), name)
        client = self.connect("/ws/echo")
        client.send(ws.OP_TEXT, b"still here")
        self.assertEqual(client.message(), ("text", b"still here"))

    def test_lengths_over_the_limit_close_1009(self):
        # Review focus: a 2^63 length is refused from the header alone, before any payload is read
        client = self.connect("/ws/echo")
        client.sock.sendall(bytes([0x80 | ws.OP_BINARY, 0x80 | 127]) + struct.pack("!Q", 2 ** 63) + b"\x00\x00\x00\x00")
        kind, payload = client.message()
        self.assertEqual((kind, close_code(payload)), ("close", 1009))
        saved = ws.MAX_MESSAGE
        ws.MAX_MESSAGE = 1000
        try:
            client = self.connect("/ws/echo")
            client.send(ws.OP_TEXT, b"a" * 600, fin=False)
            # The second frame's header declares 600 more bytes (1200 > 1000) and its payload is never sent
            client.sock.sendall(bytes([0x80 | ws.OP_CONT, 0x80 | 126]) + struct.pack("!H", 600))
            kind, payload = client.message()
            self.assertEqual((kind, close_code(payload)), ("close", 1009))
        finally:
            ws.MAX_MESSAGE = saved

    def test_header_error_is_not_followed_by_parsing_the_rest(self):
        # After a refused header the bytes behind it are not frames. These ones spell a masked CLOSE: the journal must
        # not show a "close" the client never sent, and must say how the connection really ended
        key = uuid.uuid4().hex
        client = self.connect("/ws/echo?key=" + key)
        client.sock.sendall(bytes([0x80 | ws.OP_PING, 0x80 | 126]) + struct.pack("!H", 126) + bytes([0x88, 0x80]) + b"\x00" * 4)
        kind, payload = client.message()
        self.assertEqual((kind, close_code(payload)), ("close", 1002))
        started = time.monotonic()
        client.shutdown()
        journal = self.ended(key)
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual(self.of(journal, "close"), [])
        self.assertEqual([event["ev"] for event in journal["events"] if event["ev"] in ("frame", "eof", "timeout")], ["eof"])
        self.assertEqual(self.of(journal, "eof")[0]["discarded"], 6)
        # A client that does not end TCP: the drain is bounded and says "timeout"
        saved = ws.DRAIN
        ws.DRAIN = 0.3
        try:
            key = uuid.uuid4().hex
            client = self.connect("/ws/echo?key=" + key)
            client.sock.sendall(bytes([0x80 | ws.OP_PING, 0x80 | 126]) + struct.pack("!H", 126))
            client.message()
            self.assertEqual([event["ev"] for event in self.of(self.ended(key), "timeout")], ["timeout"])
        finally:
            ws.DRAIN = saved

    def test_invalid_utf8_closes_1007(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/echo?key=" + key)
        client.send(ws.OP_TEXT, b"\xc3\x28")
        kind, payload = client.message()
        self.assertEqual((kind, close_code(payload)), ("close", 1007))
        client.close(1007)
        self.assertFalse(self.of(self.ended(key), "message")[0]["valid_utf8"])

    def test_journal_waits_and_is_bounded(self):
        started = time.monotonic()
        journal = self.journal(uuid.uuid4().hex, wait_ms=300)
        self.assertEqual((journal["ended"], journal["connections"], journal["events"]), (False, 0, []))
        self.assertGreaterEqual(time.monotonic() - started, 0.25)
        # Review focus: the oldest key goes, and events past the cap are counted, not kept
        saved = (ws.MAX_KEYS, ws.MAX_EVENTS)
        ws.MAX_KEYS, ws.MAX_EVENTS = 2, 3
        try:
            with ws.JOURNAL_LOCK:
                ws.JOURNALS.clear()
            first = ws.Journal("first")
            for index in range(5):
                first.add("note", index=index)
            with ws.JOURNAL_LOCK:
                self.assertEqual((len(ws.JOURNALS["first"]["events"]), ws.JOURNALS["first"]["dropped"]), (3, 2))
            ws.Journal("second")
            ws.Journal("third")
            with ws.JOURNAL_LOCK:
                self.assertEqual(list(ws.JOURNALS), ["second", "third"])
            first.add("late")  # its key is gone: nothing kept, nothing raised
            first.end()
        finally:
            ws.MAX_KEYS, ws.MAX_EVENTS = saved

    def test_connections_of_one_key(self):
        key = uuid.uuid4().hex
        for _ in range(2):
            client = self.connect("/ws/echo?key=" + key)
            client.close()
            client.message()
        journal = self.ended(key, connections=2)
        self.assertEqual(journal["connections"], 2)
        self.assertEqual(sorted({event["conn"] for event in journal["events"]}), [1, 2])
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        connection.request("GET", "/ws/journal?key=%s&reset=1" % key)
        connection.getresponse().read()
        connection.close()
        self.assertEqual(self.journal(key, wait_ms=0)["connections"], 0)


class ScenarioTest(WsTestBase):
    def test_close_with_code_message_and_answer(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/close/4001?msg=hi&reason=bye&key=" + key)
        self.assertEqual(client.message(), ("text", b"hi"))
        self.assertEqual(client.message(), ("close", struct.pack("!H", 4001) + b"bye"))
        client.close(4001)
        journal = self.ended(key)
        self.assertEqual(self.of(journal, "close")[0]["code"], 4001)
        self.assertEqual(len([event for event in self.of(journal, "sent") if event["op"] == ws.OP_CLOSE]), 1)

    def test_close_payload_shapes(self):
        self.assertEqual(self.connect("/ws/close/none").message(), ("close", b""))
        self.assertEqual(self.connect("/ws/close/onebyte").message(), ("close", b"\x03"))
        self.assertEqual(self.connect("/ws/close/1005").message(), ("close", struct.pack("!H", 1005)))
        self.assertEqual(self.connect("/ws/close/65535").message(), ("close", struct.pack("!H", 65535)))
        self.assertEqual(self.connect("/ws/close/70000").status, 400)
        self.assertEqual(self.connect("/ws/close/1000?reason=" + "r" * 124).status, 400)

    def test_drop_without_a_close_frame(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/drop?after_ms=50&msg=bye&key=" + key)
        self.assertEqual(client.message(), ("text", b"bye"))
        with self.assertRaises(EOFError):
            client.message()
        self.assertEqual(len(self.of(self.ended(key), "fin")), 1)
        # Raw recv on the socket: FIN reads as b"", only RST raises ECONNRESET
        client = self.connect("/ws/drop?rst=1&key=" + key + "r")
        with self.assertRaises(ConnectionResetError):
            client.sock.recv(1)
        self.assertEqual(len(self.of(self.ended(key + "r"), "rst")), 1)
        client = self.connect("/ws/drop?rst=0")
        self.assertEqual(client.sock.recv(1), b"")

    def test_drop_after_the_first_message(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/drop?on_message=1&key=" + key)
        self.assertTrue(client.accepted)
        # No drop before a message: a PING is still answered
        client.send(ws.OP_PING, b"still there?")
        self.assertEqual(client.message(), ("pong", b"still there?"))
        client.send(ws.OP_TEXT, b"go")
        with self.assertRaises(EOFError):
            client.message()
        journal = self.ended(key)
        self.assertEqual([e["ev"] for e in journal["events"] if e["ev"] in ("message", "fin", "rst")], ["message", "fin"])
        self.assertEqual([e["op"] for e in self.of(journal, "sent")], [ws.OP_PONG])
        key = uuid.uuid4().hex
        client = self.connect("/ws/drop?on_message=1&rst=1&key=" + key)
        client.send(ws.OP_BINARY, b"\x01")
        with self.assertRaises(ConnectionResetError):
            client.sock.recv(1)
        self.assertEqual([e["ev"] for e in self.ended(key)["events"] if e["ev"] in ("message", "fin", "rst")], ["message", "rst"])
        # The client's close before any message: answered, and nothing is dropped
        key = uuid.uuid4().hex
        client = self.connect("/ws/drop?on_message=1&key=" + key)
        client.close(1000)
        self.assertEqual(client.message(), ("close", struct.pack("!H", 1000)))
        self.assertEqual([e["ev"] for e in self.ended(key)["events"] if e["ev"] in ("fin", "rst")], [])
        self.assertEqual(self.connect("/ws/drop?on_message=2").status, 400)

    def test_silent_answers_nothing(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/silent?key=" + key)
        client.send(ws.OP_PING, b"anyone?")
        client.send(ws.OP_TEXT, b"hello?")
        # Completion before silence: the server has read both frames, and nothing came back
        deadline = time.monotonic() + 5
        while len(self.of(self.journal(key, wait_ms=0), "frame")) < 2 and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(len(self.of(self.journal(key, wait_ms=0), "frame")), 2)
        client.sock.settimeout(0.5)
        with self.assertRaises((socket.timeout, TimeoutError)):
            client.recv()

    def test_noecho_never_answers_the_close(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/noecho?key=" + key)
        client.send(ws.OP_TEXT, b"x")
        self.assertEqual(client.message(), ("text", b"x"))
        client.close(1000)
        deadline = time.monotonic() + 5
        while not self.of(self.journal(key, wait_ms=0), "close") and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(len(self.of(self.journal(key, wait_ms=0), "close")), 1)
        client.sock.settimeout(0.5)
        with self.assertRaises((socket.timeout, TimeoutError)):
            client.recv()
        client.shutdown()
        journal = self.ended(key)
        self.assertLess(self.of(journal, "close")[0]["t"], self.of(journal, "eof")[0]["t"])

    def test_noecho_answers_late_when_asked(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/noecho?echo_after_ms=300&key=" + key)
        client.send(ws.OP_TEXT, b"x")
        self.assertEqual(client.message(), ("text", b"x"))
        sent = time.monotonic()
        client.close(4000, b"bye")
        self.assertEqual(client.message(), ("close", struct.pack("!H", 4000)))
        self.assertGreaterEqual(time.monotonic() - sent, 0.3)
        client.shutdown()
        journal = self.ended(key)
        closes = [e for e in self.of(journal, "sent") if e["op"] == ws.OP_CLOSE]
        self.assertEqual(len(closes), 1)
        self.assertGreaterEqual(closes[0]["t"] - self.of(journal, "close")[0]["t"], 0.3)
        self.assertEqual(self.connect("/ws/noecho?echo_after_ms=%d" % (server.MAX_MS + 1)).status, 400)
        self.assertEqual(self.connect("/ws/noecho?echo_after_ms=soon").status, 400)

    def test_serverping(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/serverping?count=2&interval_ms=50&size=20&unsolicited=1&key=" + key)
        self.assertEqual(client.message(), ("pong", b"unsolicited"))
        for index in range(2):
            kind, payload = client.message()
            self.assertEqual((kind, payload), ("ping", (b"ping-%d" % index).ljust(20, b".")))
            client.send(ws.OP_PONG, payload)
        self.assertEqual(client.message(), ("text", b"pings-done"))
        client.close()
        journal = self.ended(key)
        self.assertEqual(len([event for event in self.of(journal, "frame") if event["op"] == ws.OP_PONG]), 2)

    def test_fragment_offsets(self):
        key = uuid.uuid4().hex
        text = "Привет".encode("utf-8")
        client = self.connect("/ws/fragment?at=3,7&text=%s&key=%s" % (urllib.parse.quote("Привет"), key))
        self.assertEqual([client.recv() for _ in range(3)], [(0, ws.OP_TEXT, text[:3]), (0, ws.OP_CONT, text[3:7]), (1, ws.OP_CONT, text[7:])])
        client = self.connect("/ws/fragment?cyrillic=100&split_inside=10&ping_between=1&key=" + key)
        first = client.recv()
        self.assertEqual(client.recv(), (1, ws.OP_PING, b"between"))
        rest = client.recv()
        self.assertEqual((first[0], first[1], rest[0], rest[1]), (0, ws.OP_TEXT, 1, ws.OP_CONT))
        # The cut lands inside a two-byte letter: the second frame starts with a continuation byte
        self.assertEqual(rest[2][0] & 0xC0, 0x80)
        self.assertEqual(len((first[2] + rest[2]).decode("utf-8").encode("utf-8")), 100)
        sent = self.of(self.journal(key, connections=2, wait_ms=0), "message_sent", conn=2)[0]
        self.assertEqual(sent["frames_at"], [len(first[2])])
        self.assertEqual(self.connect("/ws/fragment?text=abc&at=5").status, 400)

    def test_fragment_empty_edges(self):
        for binary in (0, 1):
            for first, final in ((1, 0), (0, 1), (1, 1)):
                with self.subTest(binary=binary, empty_first=first, empty_final=final):
                    key = uuid.uuid4().hex
                    client = self.connect("/ws/fragment?text=abc&binary=%d&empty_first=%d&empty_final=%d&key=%s"
                                          % (binary, first, final, key))
                    expected = []
                    opcode = ws.OP_BINARY if binary else ws.OP_TEXT
                    if first:
                        expected.append((0, opcode, b""))
                    expected.append((0 if final else 1, ws.OP_CONT if first else opcode, b"abc"))
                    if final:
                        expected.append((1, ws.OP_CONT, b""))
                    for frame in expected:
                        self.assertEqual(client.recv(), frame)
                    client.close()
                    self.assertEqual(len(self.of(self.ended(key), "message_sent")), 1)
        # Boundary offsets stay invalid; the new flags alone allow the two empty edges.
        for query in ("at=0", "at=3", "empty_first=2", "empty_first=-1", "empty_final=x", "empty_final=2"):
            with self.subTest(query=query):
                self.assertEqual(self.connect("/ws/fragment?text=abc&" + query).status, 400)

    def test_large(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/large?bytes=100000&ascii=0&frame=4096&key=" + key)
        frames = [client.recv()]
        while not frames[-1][0]:
            frames.append(client.recv())
        self.assertEqual(len(frames), 25)
        data = b"".join(frame[2] for frame in frames)
        self.assertEqual(len(data), 100000)
        data.decode("utf-8")
        self.assertEqual(self.of(self.journal(key, wait_ms=0), "message_sent")[0]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(self.connect("/ws/large?bytes=1000&binary=1&seed=3").message(), ("binary", random.Random(3).randbytes(1000)))
        self.assertEqual(self.connect("/ws/large?bytes=%d" % (ws.MAX_MESSAGE + 1)).status, 400)

    def test_order(self):
        client = self.connect("/ws/order?n=6")
        self.assertEqual([client.message() for _ in range(6)],
                         [("text", b"t0"), ("binary", struct.pack("!I", 1)), ("text", b"t2"), ("binary", struct.pack("!I", 3)),
                          ("text", b"t4"), ("binary", struct.pack("!I", 5))])

    def test_headers(self):
        client = self.connect("/ws/headers?x=1", headers=[("X-Probe", "Value With Case")])
        kind, data = client.message()
        seen = json.loads(data)
        self.assertEqual(kind, "text")
        self.assertIn(["X-Probe", "Value With Case"], seen["headers"])
        self.assertEqual((seen["path"], seen["query"]), ("/ws/headers", {"x": ["1"]}))

    def test_protocol_selection(self):
        chosen = self.connect("/ws/protocol?select=chat.v2", protocols=("chat.v1",))
        self.assertEqual((chosen.status, chosen.headers.get("sec-websocket-protocol")), (101, "chat.v2"))
        self.assertNotIn("sec-websocket-protocol", self.connect("/ws/protocol?select=", protocols=("chat.v1",)).headers)
        self.assertEqual(self.connect("/ws/protocol?select=a%0D%0Ab").status, 400)

    def test_auth_uses_the_http_tokens(self):
        refused = self.connect("/ws/auth")
        self.assertEqual((refused.status, refused.headers.get("www-authenticate")), (401, "Bearer"))
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        connection.request("POST", "/login", b'{"user":"neo","password":"trinity"}')
        tokens = json.loads(connection.getresponse().read())
        connection.close()
        self.assertTrue(self.connect("/ws/auth", headers=[("Authorization", "Bearer " + tokens["accessToken"])]).accepted)
        self.assertTrue(self.connect("/ws/auth?token=" + tokens["accessToken"]).accepted)

    def test_flaky_noupgrade_hang_slowupgrade(self):
        key = uuid.uuid4().hex
        self.assertEqual(self.connect("/ws/flaky?n=1&key=" + key).status, 0)
        self.assertTrue(self.connect("/ws/flaky?n=1&key=" + key).accepted)
        self.assertEqual(self.connect("/ws/noupgrade").status, 0)
        started = time.monotonic()
        self.assertEqual(self.connect("/ws/hang?ms=300").status, 0)
        self.assertGreaterEqual(time.monotonic() - started, 0.25)
        started = time.monotonic()
        self.assertTrue(self.connect("/ws/slowupgrade?ms=300").accepted)
        self.assertGreaterEqual(time.monotonic() - started, 0.25)

    def test_badutf8(self):
        self.assertEqual(self.connect("/ws/badutf8?where=text").recv(), (1, ws.OP_TEXT, b"ok \xc3\x28"))
        self.assertEqual(self.connect("/ws/badutf8?where=close").message(), ("close", struct.pack("!H", 1000) + b"\xc3\x28"))
        self.assertEqual(self.connect("/ws/badutf8?where=nope").status, 400)

    def test_broken_handshakes(self):
        wrong = self.connect("/ws/broken?what=accept")
        self.assertEqual((wrong.status, wrong.accepted), (101, False))
        self.assertNotIn("upgrade", self.connect("/ws/broken?what=upgrade").headers)
        self.assertNotIn("connection", self.connect("/ws/broken?what=connection").headers)

    def test_sink_pushes_and_echoes_nothing(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/sink?push=2&after_frames=1&key=" + key)
        client.send(ws.OP_TEXT, b"a")
        self.assertEqual(client.message(), ("text", b"2"))
        client.send(ws.OP_TEXT, b"b")
        client.send(ws.OP_PING, b"p")
        # The PONG is the next frame: "b" was not echoed before it
        self.assertEqual(client.message(), ("pong", b"p"))
        client.close()
        self.assertEqual(client.message()[0], "close")
        self.assertEqual([event.get("text") for event in self.of(self.ended(key), "message")], ["a", "b"])

    def test_protocol_error_after_our_close_is_not_an_eof(self):
        # The client's bytes after our close declare a 126-byte PING and send none of it: the journal must say so, not "eof"
        bad_header = bytes([0x80 | ws.OP_PING, 0x80 | 126]) + struct.pack("!H", 126)
        for route in ("/ws/close/1000", "/ws/silent", "/ws/noecho"):
            key = uuid.uuid4().hex
            client = self.connect(route + "?key=" + key)
            if route == "/ws/close/1000":
                self.assertEqual(client.message()[0], "close")
            else:
                client.close()
                time.sleep(0.1)
            client.sock.sendall(bad_header)
            journal = self.ended(key)
            self.assertEqual(self.of(journal, "eof"), [], route)
            self.assertEqual([event["code"] for event in self.of(journal, "protocol_error")], [1002], route)
            client.shutdown()

    def test_serverping_stops_with_the_connection(self):
        # The pinger sleeps interval_ms between PINGs; it must leave with the connection, not after count x interval
        key = uuid.uuid4().hex
        client = self.connect("/ws/serverping?count=10&interval_ms=5000&size=4&key=" + key)
        self.assertEqual(client.message()[0], "ping")
        client.close()
        self.assertEqual(client.message()[0], "close")
        self.ended(key)
        deadline = time.monotonic() + 1
        while time.monotonic() < deadline and any(thread.name == "ws-serverping" and thread.is_alive() for thread in threading.enumerate()):
            time.sleep(0.02)
        self.assertEqual([thread for thread in threading.enumerate() if thread.name == "ws-serverping" and thread.is_alive()], [])

    def test_flaky_keys_are_bounded(self):
        saved = ws.MAX_KEYS
        ws.MAX_KEYS = 2
        try:
            with ws.JOURNAL_LOCK:
                ws.WS_STATE["flaky"].clear()
            for name in ("a", "b", "c"):
                self.connect("/ws/flaky?n=1&key=" + uuid.uuid4().hex + name + "&flakykey=" + name)
            self.assertLessEqual(len(ws.WS_STATE["flaky"]), 2)
        finally:
            ws.MAX_KEYS = saved

    def test_status_location_must_be_visible_ascii(self):
        # W0 final review M-8: no header injection, no non-latin-1, no unbounded value; the probes' locations still pass
        for bad in ("a%0D%0AX-Injected:%201", "%0A", "%E2%82%AC", "a%20b", "%09", "", "a" * (ws.MAX_LOCATION + 1)):
            refused = self.connect("/ws/status/302?location=" + bad)
            self.assertEqual(refused.status, 400, bad[:40])
            self.assertNotIn("x-injected", refused.headers)
        for good in ("/ws/echo?key=k1", "ws://127.0.0.1:1/ws/echo?key=k2", "wss://localhost:2/ws/echo", "a" * ws.MAX_LOCATION):
            moved = self.connect("/ws/status/302?location=" + urllib.parse.quote(good, safe=""))
            self.assertEqual((moved.status, moved.headers.get("location")), (302, good), good[:40])

    def test_protocol_select_must_be_a_token(self):
        for bad in ("%E2%82%AC", "a%20b", "a%2Cb", "a%0D%0Ab"):
            self.assertEqual(self.connect("/ws/protocol?select=" + bad).status, 400, bad)
        self.assertEqual(self.connect("/ws/protocol?select=chat.v2%2B1").headers.get("sec-websocket-protocol"), "chat.v2+1")

    def test_flood_journals_a_start_and_a_failed_write(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/flood?bytes=%d&chunk=65536&key=%s" % (ws.MAX_MESSAGE, key))
        client.message()
        client.shutdown()
        journal = self.ended(key)
        self.assertEqual(len(self.of(journal, "flood_start")), 1)
        failed = self.of(journal, "flood_error")[0]
        self.assertGreater(failed["bytes"], 0)
        self.assertLess(failed["bytes"], ws.MAX_MESSAGE)
        self.assertEqual(self.of(journal, "flood"), [])

    def test_flood(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/flood?bytes=262144&chunk=65536&key=" + key)
        self.assertEqual([(kind, len(data)) for kind, data in (client.message() for _ in range(4))], [("binary", 65536)] * 4)
        client.close()
        self.assertEqual(client.message()[0], "close")
        self.assertEqual(self.of(self.ended(key), "flood")[0]["bytes"], 262144)


class WssTest(unittest.TestCase):
    """ws.py over the TLS listener, verified against the test CA"""

    @classmethod
    def setUpClass(cls):
        cls.httpd = start(server.make_tls_server)
        cls.port = cls.httpd.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def test_wss_echo_verified(self):
        context = ssl.create_default_context(cafile=os.path.join(server.CERTS_DIR, "test-ca.pem"))
        client = Client(self.port, "/ws/echo", tls=context)
        self.addCleanup(client.shutdown)
        self.assertTrue(client.accepted, client.headers)
        client.send(ws.OP_TEXT, b"over tls")
        self.assertEqual(client.message(), ("text", b"over tls"))
        client.close()
        self.assertEqual(client.message()[0], "close")

    def trusted(self):
        return ssl.create_default_context(cafile=os.path.join(server.CERTS_DIR, "test-ca.pem"))

    def connect(self, path):
        client = Client(self.port, path, tls=self.trusted())
        self.addCleanup(client.shutdown)
        return client

    def journal(self, key):
        context = self.trusted()
        connection = http.client.HTTPSConnection("localhost", self.port, timeout=30, context=context)
        try:
            connection.request("GET", "/ws/journal?key=%s&connections=1&wait_ms=5000" % key)
            journal = json.loads(connection.getresponse().read())
        finally:
            connection.close()
        self.assertTrue(journal["ended"], journal)
        return journal

    def test_routes_that_shut_the_socket_down_over_tls(self):
        # Python's SSLSocket.shutdown drops the SSL object: the drop, close and drain paths of ws.py must still end
        # the connection and journal what they did, as over plain TCP (they call shutdown(), then read)
        key = uuid.uuid4().hex
        client = self.connect("/ws/drop?msg=bye&key=" + key)
        self.assertEqual(client.message(), ("text", b"bye"))
        with self.assertRaises((EOFError, ssl.SSLError, OSError)):
            client.message()
        self.assertEqual([e["ev"] for e in self.journal(key)["events"] if e["ev"] in ("fin", "rst")], ["fin"])
        key = uuid.uuid4().hex
        client = self.connect("/ws/close/1000?key=" + key)
        self.assertEqual(client.message()[0], "close")
        client.close()
        self.assertEqual([e["ev"] for e in self.journal(key)["events"] if e["ev"] == "close"], ["close"])
        # A refused header: the rest of the stream is drained as bytes, to the client's end
        key = uuid.uuid4().hex
        client = self.connect("/ws/echo?key=" + key)
        client.sock.sendall(bytes([0x80 | ws.OP_PING, 0x80 | 126]) + struct.pack("!H", 126) + bytes([0x88, 0x80]) + b"\x00" * 4)
        self.assertEqual(close_code(client.message()[1]), 1002)
        client.shutdown()
        events = self.journal(key)["events"]
        self.assertEqual([e["ev"] for e in events if e["ev"] in ("close", "eof", "timeout", "error")], ["eof"])
        self.assertEqual([e for e in events if e["ev"] == "eof"][0]["discarded"], 6)


class ScriptModeTest(unittest.TestCase):
    """ws.py loaded by server.py, and started by itself: one module either way (WebSocket spec 16.3)"""

    def run_server(self, script):
        with tempfile.TemporaryDirectory() as directory:
            ready = os.path.join(directory, "url")
            child = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, script), "--port", "0", "--ready-file", ready],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                while not os.path.exists(ready) and time.monotonic() < deadline and child.poll() is None:
                    time.sleep(0.05)
                with open(ready, encoding="utf-8") as stream:
                    port = urllib.parse.urlsplit(stream.readline().strip()).port
                key = uuid.uuid4().hex
                client = Client(port, "/ws/echo?key=" + key)
                client.send(ws.OP_TEXT, b"script mode")
                self.assertEqual(client.message(), ("text", b"script mode"))
                client.close()
                client.message()
                client.shutdown()
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                connection.request("GET", "/ws/journal?key=%s&wait_ms=3000" % key)
                journal = json.loads(connection.getresponse().read())
                connection.close()
                # The upgrade handler and the journal route are one module: the journal knows the connection
                self.assertTrue(journal["ended"], journal)
            finally:
                child.kill()
                child.wait()
        self.assertFalse(os.path.exists(os.path.join(HERE, "__pycache__")))

    def test_loaded_by_server_py(self):
        self.run_server("server.py")

    def test_started_as_ws_py(self):
        self.run_server("ws.py")


if __name__ == "__main__":
    unittest.main()
