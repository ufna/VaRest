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
        try:
            self.sock.close()
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
            ("126-byte control", lambda c: c.send(ws.OP_PING, b"p" * 126)),
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
            client.send(ws.OP_CONT, b"b" * 600)
            kind, payload = client.message()
            self.assertEqual((kind, close_code(payload)), ("close", 1009))
        finally:
            ws.MAX_MESSAGE = saved

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
