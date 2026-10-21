#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""VaRest WebSocket test server (WebSocket spec 16.3). Python 3.9+ standard library only.

    python3 -I Tools/testserver/server.py [server.py options]   loads this module next to it (server.PLUGINS)
    python3 -I Tools/testserver/ws.py [server.py options]       the same server, started from here

A request whose Upgrade header is "websocket" comes here instead of an HTTP route (server.register_upgrade). Every
route takes ?key=<k>: what the server saw on that connection -- the request, every frame both ways, messages, closes,
times -- is kept under k and served as JSON by

  GET /ws/journal?key=<k>[&connections=<n>][&wait_ms=<ms>][&reset=1]
      {"key", "connections", "ended", "dropped", "events": [{"conn", "t", "ev", ...}]}

wait_ms waits until n connections with that key (default 1) have ended; "ended" says whether they had. "conn" numbers
the connections of one key from 1; "t" is seconds since that connection's request was read. Bounds: MAX_KEYS keys
(the oldest is forgotten), MAX_EVENTS events per key (the rest are counted in "dropped"), MAX_MESSAGE bytes per frame
and per message (more is closed with 1009), numeric parameters checked before the 101 (400 JSON, as server.py's
routes).

Echo mode, what a route does after its own part unless it says otherwise: data messages echoed with their kind, PING
answered with a PONG of the same payload, the client's close answered with a close carrying its code, invalid UTF-8 in
a text message closed with 1007, an unmasked frame, a reserved bit, a bad control frame or a broken fragment sequence
closed with 1002.

Routes (GET with Upgrade: websocket):
  /ws/echo[?fragment=N]            echo mode; fragment=N echoes in frames of N bytes
  /ws/status/<code>[?location=u]   refuses the upgrade with that status (200-599) and {"status": code}; Location: u
                                   when given (3xx), WWW-Authenticate: Bearer with 401
"""

import base64
import hashlib
import json
import os
import random
import re
import socket
import struct
import sys
import threading
import time
import urllib.parse

if __name__ == "__main__":
    # Run as a script this module is __main__; server.load_plugins() imports "ws" and must get this module, not a second
    # copy with its own journal and a second registration (the mirror of server.load_plugins' own guard)
    sys.modules.setdefault("ws", sys.modules[__name__])

HERE = os.path.dirname(os.path.abspath(__file__))
# python3 -I drops the script directory from sys.path (WebSocket spec 16.3)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
# No __pycache__ in the source tree (it is not ignored)
sys.dont_write_bytecode = True
import server  # noqa: E402

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA
DATA_OPS = (OP_CONT, OP_TEXT, OP_BINARY)

MAX_MESSAGE = 64 * 1024 * 1024
MAX_KEYS = 256
# Ample for every probe (the busiest, Data.AutoReplyBoundary, journals about 70 frames); 256 keys x 2000 bounds memory
MAX_EVENTS = 2000
# Payloads up to this many bytes are journaled whole (base64), longer ones as a SHA-256
INLINE = 256
# The longest a connection waits on its peer (silent, hang, noecho)
HOLD = 60.0
# How long a close the server started waits for the client's answer
CLOSE_WAIT = 5.0

JOURNAL_LOCK = threading.Condition()
JOURNALS = {}
WS_STATE = {"flaky": {}}


class ProtocolError(Exception):
    """After the 101: the connection is closed with this code"""

    def __init__(self, code, reason):
        super().__init__(reason)
        self.code = code


class Journal:
    """What the server saw on one connection, kept under its ?key= (without one, nothing is kept)"""

    def __init__(self, key):
        self.key = key
        self.t0 = time.monotonic()
        self.index = 0
        if key is None:
            return
        with JOURNAL_LOCK:
            entry = JOURNALS.get(key)
            if entry is None:
                if len(JOURNALS) >= MAX_KEYS:
                    JOURNALS.pop(next(iter(JOURNALS)))
                entry = JOURNALS[key] = {"started": 0, "ended": 0, "dropped": 0, "events": []}
            entry["started"] += 1
            self.index = entry["started"]

    def add(self, ev, **fields):
        if self.key is None:
            return
        event = {"conn": self.index, "t": round(time.monotonic() - self.t0, 4), "ev": ev}
        event.update(fields)
        with JOURNAL_LOCK:
            entry = JOURNALS.get(self.key)
            if entry is None:
                return
            if len(entry["events"]) < MAX_EVENTS:
                entry["events"].append(event)
            else:
                entry["dropped"] += 1

    def end(self):
        self.add("end")
        if self.key is None:
            return
        with JOURNAL_LOCK:
            entry = JOURNALS.get(self.key)
            if entry is not None:
                entry["ended"] += 1
            JOURNAL_LOCK.notify_all()


def inline(payload):
    """A payload for the journal: whole (base64) when short, else its SHA-256"""
    if len(payload) <= INLINE:
        return {"len": len(payload), "b64": base64.b64encode(payload).decode("ascii")}
    return {"len": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}


def message_fields(kind, data):
    fields = {"kind": kind, "len": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if len(data) <= INLINE:
        fields["b64"] = base64.b64encode(data).decode("ascii")
        text = server.decode_text(data) if kind == "text" else None
        if text is not None:
            fields["text"] = text
    return fields


def close_fields(payload):
    fields = {"hex": payload.hex(), "len": len(payload)}
    if len(payload) >= 2:
        fields["code"] = struct.unpack("!H", payload[:2])[0]
        fields["reason"] = payload[2:].decode("utf-8", "replace")
    return fields


def unmask(payload, mask):
    """RFC 6455 5.3 as one big-integer XOR (WebSocket spec 16.3) instead of a loop over every byte"""
    count = len(payload)
    if not count:
        return payload
    key = (mask * (count // 4 + 1))[:count]
    return (int.from_bytes(payload, "big") ^ int.from_bytes(key, "big")).to_bytes(count, "big")


def accept_value(key):
    """RFC 6455 4.2.2: base64(SHA-1(key + GUID))"""
    return base64.b64encode(hashlib.sha1(key.encode("ascii") + GUID).digest()).decode("ascii")


def check_handshake(h):
    """RFC 6455 4.2.1: what a server must see before it answers 101; a refusal is a server.Refused (400 or 426)"""
    if h.command != "GET":
        raise server.Refused(400, "a WebSocket upgrade is a GET", method=h.command)
    if "upgrade" not in [token.strip().lower() for token in h.headers.get("Connection", "").split(",")]:
        raise server.Refused(400, "Connection does not list upgrade")
    if h.headers.get("Sec-WebSocket-Version", "").strip() != "13":
        raise server.Refused(426, "unsupported Sec-WebSocket-Version", version=h.headers.get("Sec-WebSocket-Version"))
    key = h.headers.get("Sec-WebSocket-Key", "").strip()
    try:
        if len(base64.b64decode(key, validate=True)) != 16:
            raise ValueError(key)
    except ValueError:
        raise server.Refused(400, "Sec-WebSocket-Key is not 16 bytes of base64")
    return key


def accept(h, journal, protocol=None):
    """The 101, then the connection is this module's"""
    key = check_handshake(h)
    lines = [b"HTTP/1.1 101 Switching Protocols", b"Upgrade: websocket", b"Connection: Upgrade",
             b"Sec-WebSocket-Accept: " + accept_value(key).encode("ascii")]
    if protocol:
        lines.append(b"Sec-WebSocket-Protocol: " + protocol.encode("latin-1"))
    h.wfile.write(b"\r\n".join(lines) + b"\r\n\r\n")
    journal.add("response", status=101, protocol=protocol)
    return WsConn(h, journal)


class WsConn:
    """One accepted connection: frame IO (server frames unmasked, client frames must be masked) and the receive loop"""

    def __init__(self, h, journal):
        self.sock = h.connection
        self.rfile = h.rfile
        self.wfile = h.wfile
        self.journal = journal
        self.lock = threading.Lock()
        self.sent_close = False
        self.sock.settimeout(HOLD)

    def read_exact(self, count):
        data = self.rfile.read(count) if count else b""
        if data is None or len(data) < count:
            raise EOFError("the client ended the connection")
        return data

    def read_frame(self):
        first, second = self.read_exact(2)
        fin, rsv, op = first >> 7, (first >> 4) & 7, first & 0x0F
        masked, length = second >> 7, second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read_exact(8))[0]
        # Both refused from the header alone: nothing of the payload is read (review M-10)
        if op >= 0x8 and length > 125:
            raise ProtocolError(1002, "a control frame is at most 125 bytes")
        if length > MAX_MESSAGE:
            raise ProtocolError(1009, "a frame over the server's limit")
        mask = self.read_exact(4) if masked else b""
        payload = self.read_exact(length)
        if masked:
            payload = unmask(payload, mask)
        self.journal.add("frame", op=op, fin=fin, rsv=rsv, masked=masked, **inline(payload))
        return fin, rsv, op, masked, payload

    def send_frame(self, op, payload=b"", fin=True, journal=True):
        first = (0x80 if fin else 0) | op
        length = len(payload)
        if length < 126:
            head = struct.pack("!BB", first, length)
        elif length < 65536:
            head = struct.pack("!BBH", first, 126, length)
        else:
            head = struct.pack("!BBQ", first, 127, length)
        with self.lock:
            self.wfile.write(head + payload)
        if journal:
            self.journal.add("sent", op=op, fin=int(fin), **inline(payload))

    def send_message(self, kind, data, fragment=0):
        op = OP_TEXT if kind == "text" else OP_BINARY
        if fragment <= 0 or len(data) <= fragment:
            self.send_frame(op, data)
            return
        for offset in range(0, len(data), fragment):
            self.send_frame(op if offset == 0 else OP_CONT, data[offset:offset + fragment], fin=offset + fragment >= len(data))

    def send_close(self, payload):
        self.sent_close = True
        self.send_frame(OP_CLOSE, payload)

    def fail(self, code, reason):
        self.journal.add("fail", code=code, reason=reason)
        if not self.sent_close:
            self.send_close(struct.pack("!H", code) + reason.encode("utf-8")[:123])
        self.wait_close(CLOSE_WAIT)

    def wait_close(self, seconds):
        """After our close: reads until the client's close (journaled as "close") or EOF, at most seconds"""
        self.sock.settimeout(seconds)
        try:
            while True:
                _, _, op, _, payload = self.read_frame()
                if op == OP_CLOSE:
                    self.journal.add("close", **close_fields(payload))
                    return
        except (EOFError, ProtocolError):
            self.journal.add("eof")
        except socket.timeout:
            self.journal.add("timeout")
        except OSError as error:
            self.journal.add("error", text=repr(error))

    def wait_eof(self, seconds):
        """Reads (and journals) everything until the client ends TCP or seconds pass"""
        self.sock.settimeout(seconds)
        try:
            while True:
                self.read_frame()
        except (EOFError, ProtocolError):
            self.journal.add("eof")
        except socket.timeout:
            self.journal.add("timeout")
        except OSError as error:
            self.journal.add("error", text=repr(error))

    def serve(self, on_message=None, answer_ping=True, echo_close=True, on_frame=None, strict_utf8=True):
        """The receive loop: assembles messages, answers control frames, ends at the client's close or EOF"""
        kind, parts, total = None, [], 0
        try:
            while True:
                fin, rsv, op, masked, payload = self.read_frame()
                if not masked:
                    raise ProtocolError(1002, "client frames must be masked")
                if rsv:
                    raise ProtocolError(1002, "no extension was negotiated")
                if on_frame:
                    on_frame(self, op, fin, payload)
                if op >= 0x8:
                    if not fin:
                        raise ProtocolError(1002, "a control frame is final")
                    if op == OP_PING:
                        if answer_ping:
                            self.send_frame(OP_PONG, payload)
                    elif op == OP_CLOSE:
                        self.journal.add("close", **close_fields(payload))
                        if echo_close and not self.sent_close:
                            # RFC 6455 5.5.1: answer with a close, echoing the status code
                            self.send_close(payload[:2])
                        return "close"
                    elif op != OP_PONG:
                        raise ProtocolError(1002, "an unknown control opcode")
                    continue
                if op == OP_CONT:
                    if kind is None:
                        raise ProtocolError(1002, "a continuation outside a message")
                elif op in (OP_TEXT, OP_BINARY):
                    if kind is not None:
                        raise ProtocolError(1002, "a new message inside a fragmented one")
                    kind = "text" if op == OP_TEXT else "binary"
                else:
                    raise ProtocolError(1002, "an unknown data opcode")
                # Checked before the append: the assembled message never grows past the limit (review M-10)
                if total + len(payload) > MAX_MESSAGE:
                    raise ProtocolError(1009, "a message over the server's limit")
                parts.append(payload)
                total += len(payload)
                if not fin:
                    continue
                data, done = b"".join(parts), kind
                kind, parts, total = None, [], 0
                fields = message_fields(done, data)
                if done == "text" and server.decode_text(data) is None:
                    self.journal.add("message", valid_utf8=False, **fields)
                    if strict_utf8:
                        raise ProtocolError(1007, "invalid UTF-8 in a text message")
                else:
                    self.journal.add("message", **fields)
                if on_message:
                    on_message(self, done, data)
        except ProtocolError as error:
            self.fail(error.code, str(error))
            return "fail"
        except EOFError:
            self.journal.add("eof")
            return "eof"
        except socket.timeout:
            self.journal.add("timeout")
            return "timeout"
        except OSError as error:
            self.journal.add("error", text=repr(error))
            return "error"


def echo(conn, fragment=0, **policy):
    return conn.serve(on_message=lambda c, kind, data: c.send_message(kind, data, fragment), **policy)


def ws_echo(h, match, journal):
    fragment = server.query_int(h, "fragment", 0, MAX_MESSAGE)
    echo(accept(h, journal), fragment)


def ws_status(h, match, journal):
    code = server.path_int(match, "code", 599)
    if code < 200:
        raise server.Refused(400, "out of range", name="code", value=code, min=200, max=599)
    headers = [("Connection", "close")]
    if "location" in h.query:
        headers.append(("Location", h.query["location"][0]))
    if code == 401:
        headers.append(("WWW-Authenticate", "Bearer"))
    journal.add("response", status=code)
    h.send_json(code, {"status": code}, headers)


def route_journal(h, match):
    key = h.query.get("key", [""])[0]
    wait = server.query_int(h, "wait_ms", 0, server.MAX_MS) / 1000.0
    connections = server.query_int(h, "connections", 1, MAX_KEYS, low=1)
    deadline = time.monotonic() + wait
    with JOURNAL_LOCK:
        while True:
            entry = JOURNALS.get(key)
            ended = entry is not None and entry["started"] >= connections and entry["ended"] >= entry["started"]
            remaining = deadline - time.monotonic()
            if ended or remaining <= 0:
                break
            JOURNAL_LOCK.wait(remaining)
        snapshot = {"key": key, "connections": entry["started"] if entry else 0, "ended": ended,
                    "dropped": entry["dropped"] if entry else 0, "events": list(entry["events"]) if entry else []}
        if h.query.get("reset", ["0"])[0] == "1":
            JOURNALS.pop(key, None)
    h.send_json(200, snapshot)


def handle_upgrade(h):
    url = urllib.parse.urlsplit(h.path)
    # server.py's dispatch parses the query after its upgrade check; the routes here read it as its own routes do
    h.query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
    journal = Journal(h.query.get("key", [None])[0])
    journal.add("request", method=h.command, target=h.path, request_line=h.requestline,
                headers=[[name, value] for name, value in h.headers.items()])
    h.close_connection = True
    try:
        for pattern, scenario in SCENARIOS:
            match = pattern.fullmatch(url.path)
            if match:
                scenario(h, match, journal)
                return
        raise server.Refused(404, "no WebSocket route", path=url.path)
    except server.Refused as refused:
        # Raised before any 101: server.py's dispatch answers it as JSON and closes
        journal.add("refused", status=refused.status, error=str(refused))
        raise
    except (BrokenPipeError, ConnectionResetError) as error:
        journal.add("error", text=repr(error))
    finally:
        journal.end()


SCENARIOS = [(re.compile(pattern), scenario) for pattern, scenario in [
    (r"/ws/echo", ws_echo),
    (r"/ws/status/(\d{3})", ws_status),
]]

server.register_upgrade("websocket", handle_upgrade)
server.register_route(("GET",), r"/ws/journal", route_journal)

if __name__ == "__main__":
    sys.exit(server.main())
