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
  /ws/close/<code>|none|onebyte?msg=<text>&reason=<text>
                                   optional text msg, then our close (any code, raw; none: empty payload; onebyte: 03),
                                   then up to CLOSE_WAIT for the client's answer (journaled as "close")
  /ws/drop?after_ms=&msg=&rst=0|1  optional msg, then TCP ends without a close frame (FIN, or RST with rst=1)
  /ws/silent                       answers nothing at all (no PONG, no close echo, no data) until the client ends TCP
  /ws/noecho                       echo mode, but the client's close is never answered; waits for the client's TCP end
  /ws/serverping?count=&interval_ms=&size=&unsolicited=0|1
                                   PINGs "ping-<i>" (padded to size with "."), then text "pings-done"; unsolicited=1
                                   first sends a PONG nobody asked for; echo mode
  /ws/fragment?(text=|cyrillic=N|bytes=N)&at=o,..&split_inside=o,..&binary=0|1&ping_between=0|1
                                   one message cut into frames at those offsets (split_inside: the first later offset
                                   inside a UTF-8 sequence), PING "between" after the first frame if asked; echo mode
  /ws/large?bytes=N&ascii=0|1&binary=0|1&frame=F&seed=S
                                   one N-byte message (ASCII, two-byte Cyrillic or seeded bytes) in F-byte frames
  /ws/order?n=K                    K messages alternating text "t<i>" and binary <i> (4 bytes, big-endian)
  /ws/headers                      first message: the handshake as received, as JSON; echo mode
  /ws/protocol?select=<p>          answers Sec-WebSocket-Protocol: <p> whatever was offered (none when empty)
  /ws/auth[?token=<t>]             101 only with /login's current access token (Bearer or token=), else 401
  /ws/flaky?n=K&key=<k>            the first K upgrades per key end TCP before any status line; then echo mode
  /ws/noupgrade                    ends TCP before any status line (WebSocket spec 22 A2: NoUpgradeResponse)
  /ws/hang?ms=<ms>                 never answers the upgrade; holds until the client ends TCP or ms pass
  /ws/slowupgrade?ms=<ms>          waits ms before the 101; echo mode
  /ws/badutf8?where=text|close     invalid UTF-8 (C3 28) in a text message or in our close reason
  /ws/broken?what=accept|upgrade|connection
                                   a 101 a client must refuse: wrong Sec-WebSocket-Accept, or no Upgrade, or no Connection
  /ws/sink?push=<t>&after_frames=N journals everything, echoes nothing; pushes text t once after the N-th data frame
  /ws/flood?bytes=N&chunk=C        N bytes as C-byte binary messages as fast as TCP takes them, then echo mode; the
                                   "flood" event says how long writes blocked (a client that stopped reading)

Not here yet: /ws/route (requests and replies, W4) and /ws/apping (app heartbeat, W3).
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
# The most connections one key's journal can be asked to wait for (same bound as the keys: one probe never needs more)
MAX_CONNECTIONS = MAX_KEYS
# Ample for every probe (the busiest, Data.AutoReplyBoundary, journals about 70 frames); 256 keys x 2000 bounds memory
MAX_EVENTS = 2000
# Payloads up to this many bytes are journaled whole (base64), longer ones as a SHA-256
INLINE = 256
# The longest a connection waits on its peer (silent, hang, noecho)
HOLD = 60.0
# How long a close the server started waits for the client's answer
CLOSE_WAIT = 5.0
# After a framing error the stream is not frames any more: how long we drain it (unparsed) for the client's TCP end
DRAIN = 1.0

JOURNAL_LOCK = threading.Condition()
JOURNALS = {}
WS_STATE = {"flaky": {}}


class ProtocolError(Exception):
    """After the 101: the connection is closed with this code"""

    def __init__(self, code, reason, desynced=False):
        super().__init__(reason)
        self.code = code
        # True when the header was refused and its payload is still unread: the rest of the stream is not frames
        self.desynced = desynced


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

    def read_frame(self, budget=None):
        """One frame. budget: the most a data frame may declare (the message limit minus what the message already holds)"""
        if budget is None:
            budget = MAX_MESSAGE
        first, second = self.read_exact(2)
        fin, rsv, op = first >> 7, (first >> 4) & 7, first & 0x0F
        masked, length = second >> 7, second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read_exact(8))[0]
        # Both refused from the header alone: nothing of the payload is read (review M-10). A control frame is held to
        # 125 bytes above, so the budget only ever limits data frames; it already counts the fragments read so far
        if op >= 0x8 and length > 125:
            raise ProtocolError(1002, "a control frame is at most 125 bytes", desynced=True)
        if op < 0x8 and length > budget:
            raise ProtocolError(1009, "a message over the server's limit", desynced=True)
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

    def fail(self, code, reason, desynced=False):
        self.journal.add("fail", code=code, reason=reason)
        if not self.sent_close:
            self.send_close(struct.pack("!H", code) + reason.encode("utf-8")[:123])
        if desynced:
            self.drain()
        else:
            self.wait_close(CLOSE_WAIT)

    def drain(self):
        """After a header was refused its payload and mask are still in the stream: parsing them as frames would journal
        a close or a PING the client never sent. So: our close is out, we stop sending, and read the rest as bytes,
        for DRAIN seconds at most, to see the client's TCP end"""
        discarded = 0
        try:
            self.sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        deadline = time.monotonic() + DRAIN
        try:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise socket.timeout()
                self.sock.settimeout(remaining)
                chunk = self.rfile.read1(65536)  # read1: what the buffered reader already took counts too
                if not chunk:
                    self.journal.add("eof", discarded=discarded)
                    return
                discarded += len(chunk)
        except socket.timeout:
            self.journal.add("timeout", discarded=discarded)
        except OSError as error:
            self.journal.add("error", text=repr(error), discarded=discarded)

    def wait_close(self, seconds):
        """After our close: reads until the client's close (journaled as "close") or EOF, at most seconds"""
        self.sock.settimeout(seconds)
        try:
            while True:
                _, _, op, _, payload = self.read_frame()
                if op == OP_CLOSE:
                    self.journal.add("close", **close_fields(payload))
                    return
        except EOFError:
            self.journal.add("eof")
        except ProtocolError as error:
            # Not an end of TCP: the client sent a header we refuse (the client may still be there)
            self.journal.add("protocol_error", code=error.code, reason=str(error))
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
        except EOFError:
            self.journal.add("eof")
        except ProtocolError as error:
            # Not an end of TCP: the client sent a header we refuse (the client may still be there)
            self.journal.add("protocol_error", code=error.code, reason=str(error))
        except socket.timeout:
            self.journal.add("timeout")
        except OSError as error:
            self.journal.add("error", text=repr(error))

    def serve(self, on_message=None, answer_ping=True, echo_close=True, on_frame=None, strict_utf8=True):
        """The receive loop: assembles messages, answers control frames, ends at the client's close or EOF"""
        kind, parts, total = None, [], 0
        try:
            while True:
                fin, rsv, op, masked, payload = self.read_frame(MAX_MESSAGE - total)
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
                # No size check here: read_frame refused a frame over MAX_MESSAGE - total at its header, so the assembled
                # message never grows past the limit and nothing big was read first (review M-10)
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
            self.fail(error.code, str(error), error.desynced)
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
    connections = server.query_int(h, "connections", 1, MAX_CONNECTIONS, low=1)
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


CYRILLIC = "Съешь же ещё этих мягких французских булок, да выпей чаю. ".encode("utf-8")


def valid_utf8(data):
    return server.decode_text(data) is not None


def cyrillic(count):
    """count bytes of valid UTF-8, mostly two-byte Cyrillic letters, so that some cut points fall inside a code point"""
    data = (CYRILLIC * (count // len(CYRILLIC) + 1))[:count]
    # The cut may have split the last letter: drop its lead byte and pad with ASCII to the exact length
    while data and not valid_utf8(data):
        data = data[:-1]
    return data + b"x" * (count - len(data))


def parse_offsets(h, data):
    """at=: exact offsets; split_inside=: for each value, the first offset at or after it inside a UTF-8 sequence"""
    offsets = []
    for name in ("at", "split_inside"):
        for part in filter(None, h.query.get(name, [""])[0].split(",")):
            try:
                value = int(part)
            except ValueError:
                raise server.Refused(400, "not an integer", name=name, value=part)
            if name == "split_inside":
                while 0 <= value < len(data) and (data[value] & 0xC0) != 0x80:
                    value += 1
            if not 0 < value < len(data):
                raise server.Refused(400, "an offset must fall inside the message", name=name, value=value, length=len(data))
            offsets.append(value)
    return sorted(set(offsets))


def drop_before_status(h, journal):
    journal.add("dropped_before_status")
    try:
        h.connection.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def ws_close(h, match, journal):
    what = match.group(1)
    if what == "none":
        payload = b""
    elif what == "onebyte":
        payload = b"\x03"
    else:
        payload = struct.pack("!H", server.path_int(match, "code", 65535)) + h.query.get("reason", [""])[0].encode("utf-8")
        if len(payload) > 125:
            raise server.Refused(400, "a close payload is at most 125 bytes (RFC 6455 5.5)", length=len(payload))
    conn = accept(h, journal)
    if "msg" in h.query:
        conn.send_message("text", h.query["msg"][0].encode("utf-8"))
    conn.send_close(payload)
    conn.wait_close(CLOSE_WAIT)


def ws_drop(h, match, journal):
    after = server.query_int(h, "after_ms", 0, server.MAX_MS) / 1000.0
    rst = server.query_int(h, "rst", 0, 1)
    conn = accept(h, journal)
    if "msg" in h.query:
        conn.send_message("text", h.query["msg"][0].encode("utf-8"))
    time.sleep(after)
    if rst:
        # SO_LINGER on with a zero timeout: close() sends RST instead of FIN
        conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        journal.add("rst")
        conn.sock.close()
        return
    journal.add("fin")
    try:
        conn.sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def ws_silent(h, match, journal):
    conn = accept(h, journal)
    if conn.serve(answer_ping=False, echo_close=False) == "close":
        conn.wait_eof(HOLD)


def ws_noecho(h, match, journal):
    conn = accept(h, journal)
    if echo(conn, echo_close=False) == "close":
        conn.wait_eof(HOLD)


def ws_serverping(h, match, journal):
    count = server.query_int(h, "count", 3, 100, low=1)
    interval = server.query_int(h, "interval_ms", 200, server.MAX_MS) / 1000.0
    size = server.query_int(h, "size", 0, 125)
    unsolicited = server.query_int(h, "unsolicited", 0, 1)
    conn = accept(h, journal)

    # Set when the connection ends: the pinger neither outlives it (it waits on this, not sleeps) nor sends after our close
    stop = threading.Event()

    def pinger():
        try:
            if unsolicited:
                conn.send_frame(OP_PONG, b"unsolicited")
            for index in range(count):
                if index and stop.wait(interval):
                    return
                if stop.is_set() or conn.sent_close:
                    return
                conn.send_frame(OP_PING, (b"ping-%d" % index).ljust(size, b"."))
            if stop.wait(interval) or conn.sent_close:
                return
            conn.send_message("text", b"pings-done")
        except OSError:
            pass

    threading.Thread(target=pinger, name="ws-serverping", daemon=True).start()
    try:
        echo(conn)
    finally:
        stop.set()


def ws_fragment(h, match, journal):
    binary = server.query_int(h, "binary", 0, 1)
    if "text" in h.query:
        data = h.query["text"][0].encode("utf-8")
    elif "cyrillic" in h.query:
        data = cyrillic(server.query_int(h, "cyrillic", 4096, MAX_MESSAGE, low=1))
    else:
        data = random.Random(1).randbytes(server.query_int(h, "bytes", 4096, MAX_MESSAGE, low=1))
    offsets = parse_offsets(h, data)
    ping_between = server.query_int(h, "ping_between", 0, 1)
    conn = accept(h, journal)
    journal.add("message_sent", kind="binary" if binary else "text", len=len(data), sha256=hashlib.sha256(data).hexdigest(),
                frames_at=offsets)
    cuts = [0] + offsets + [len(data)]
    for index in range(len(cuts) - 1):
        first, last = index == 0, index == len(cuts) - 2
        op = (OP_BINARY if binary else OP_TEXT) if first else OP_CONT
        conn.send_frame(op, data[cuts[index]:cuts[index + 1]], fin=last)
        if ping_between and first and not last:
            conn.send_frame(OP_PING, b"between")
    echo(conn)


def ws_large(h, match, journal):
    count = server.query_int(h, "bytes", 65536, MAX_MESSAGE, low=1)
    binary = server.query_int(h, "binary", 0, 1)
    ascii_only = server.query_int(h, "ascii", 1, 1)
    frame = server.query_int(h, "frame", 0, MAX_MESSAGE)
    seed = server.query_int(h, "seed", 1, 2 ** 31)
    if binary:
        data = random.Random(seed).randbytes(count)
    elif ascii_only:
        data = (b"abcdefghijklmnopqrstuvwxyz" * (count // 26 + 1))[:count]
    else:
        data = cyrillic(count)
    conn = accept(h, journal)
    journal.add("message_sent", kind="binary" if binary else "text", len=count, sha256=hashlib.sha256(data).hexdigest())
    conn.send_message("binary" if binary else "text", data, frame)
    echo(conn)


def ws_order(h, match, journal):
    count = server.query_int(h, "n", 100, 100000, low=1)
    conn = accept(h, journal)
    for index in range(count):
        if index % 2 == 0:
            conn.send_frame(OP_TEXT, b"t%d" % index, journal=False)
        else:
            conn.send_frame(OP_BINARY, struct.pack("!I", index), journal=False)
    journal.add("order_sent", n=count)
    echo(conn)


def ws_headers(h, match, journal):
    conn = accept(h, journal)
    seen = {"request_line": h.requestline, "headers": [[name, value] for name, value in h.headers.items()],
            "path": urllib.parse.urlsplit(h.path).path, "query": h.query}
    conn.send_message("text", json.dumps(seen, separators=(",", ":")).encode("utf-8"))
    echo(conn)


TOKEN = re.compile(r"[!#$%&'*+\-.^_`|~0-9A-Za-z]+")


def ws_protocol(h, match, journal):
    selected = h.query.get("select", [""])[0]
    # RFC 6455 4.2.2: a subprotocol is one token (RFC 2616 2.2); that also keeps line breaks and non-latin-1 out of the header
    if selected and not TOKEN.fullmatch(selected):
        raise server.Refused(400, "select is not an RFC 6455 token", value=selected)
    echo(accept(h, journal, protocol=selected or None))


def ws_auth(h, match, journal):
    with server.LOCK:
        current = server.STATE["tokens"]["access"]
    offered = h.headers.get("Authorization", "")
    token = h.query.get("token", [None])[0]
    if current is None or (offered != "Bearer " + current and token != current):
        journal.add("response", status=401)
        h.send_json(401, {"error": "unauthorized"}, [("WWW-Authenticate", "Bearer"), ("Connection", "close")])
        return
    echo(accept(h, journal))


def ws_flaky(h, match, journal):
    failures = server.query_int(h, "n", 1, server.MAX_COUNT)
    name = h.query.get("key", ["default"])[0]
    with server.LOCK:
        seen = WS_STATE["flaky"].get(name, 0)
        # Bounded like the journals: the oldest key is forgotten (a request-controlled count)
        if name not in WS_STATE["flaky"] and len(WS_STATE["flaky"]) >= MAX_KEYS:
            WS_STATE["flaky"].pop(next(iter(WS_STATE["flaky"])))
        WS_STATE["flaky"][name] = seen + 1
    if seen < failures:
        drop_before_status(h, journal)
        return
    echo(accept(h, journal))


def ws_noupgrade(h, match, journal):
    drop_before_status(h, journal)


def ws_hang(h, match, journal):
    hold = server.query_int(h, "ms", int(HOLD * 1000), server.MAX_MS) / 1000.0
    h.connection.settimeout(hold)
    try:
        while h.connection.recv(65536):
            pass
        journal.add("eof")
    except socket.timeout:
        journal.add("timeout")
    except OSError:
        journal.add("eof")


def ws_slowupgrade(h, match, journal):
    time.sleep(server.query_int(h, "ms", 1000, server.MAX_MS) / 1000.0)
    echo(accept(h, journal))


def ws_badutf8(h, match, journal):
    where = h.query.get("where", ["text"])[0]
    if where not in ("text", "close"):
        raise server.Refused(400, "where is text or close", value=where)
    conn = accept(h, journal)
    if where == "text":
        conn.send_frame(OP_TEXT, b"ok \xc3\x28")
        echo(conn)
        return
    conn.send_close(struct.pack("!H", 1000) + b"\xc3\x28")
    conn.wait_close(CLOSE_WAIT)


def ws_broken(h, match, journal):
    what = h.query.get("what", ["accept"])[0]
    if what not in ("accept", "upgrade", "connection"):
        raise server.Refused(400, "what is accept, upgrade or connection", value=what)
    key = check_handshake(h)
    lines = [b"HTTP/1.1 101 Switching Protocols"]
    if what != "upgrade":
        lines.append(b"Upgrade: websocket")
    if what != "connection":
        lines.append(b"Connection: Upgrade")
    lines.append(b"Sec-WebSocket-Accept: " + accept_value(key + "x" if what == "accept" else key).encode("ascii"))
    h.wfile.write(b"\r\n".join(lines) + b"\r\n\r\n")
    journal.add("response", status=101, broken=what)
    WsConn(h, journal).wait_eof(CLOSE_WAIT)


def ws_sink(h, match, journal):
    push = h.query.get("push", [None])[0]
    after = server.query_int(h, "after_frames", 1, MAX_EVENTS, low=1)
    conn = accept(h, journal)
    counted = [0]

    def on_frame(connection, op, fin, payload):
        if op in DATA_OPS:
            counted[0] += 1
            if push is not None and counted[0] == after:
                connection.send_frame(OP_TEXT, push.encode("utf-8"))

    conn.serve(on_frame=on_frame)


def ws_flood(h, match, journal):
    total = server.query_int(h, "bytes", 8 * 1024 * 1024, MAX_MESSAGE, low=1)
    chunk = server.query_int(h, "chunk", 65536, MAX_MESSAGE, low=1)
    conn = accept(h, journal)
    block = random.Random(7).randbytes(chunk)
    sent, longest, blocked, started = 0, 0.0, 0, time.monotonic()
    # Before the first write: a write that blocks for good or breaks still leaves evidence of what was attempted
    journal.add("flood_start", bytes=total, chunk=chunk)
    while sent < total:
        piece = block[:min(chunk, total - sent)]
        before = time.monotonic()
        try:
            conn.send_frame(OP_BINARY, piece, journal=False)
        except OSError as error:
            journal.add("flood_error", text=repr(error), bytes=sent, chunk=chunk, longest_write_s=round(longest, 4),
                        writes_over_100ms=blocked, seconds=round(time.monotonic() - started, 4))
            return
        took = time.monotonic() - before
        longest = max(longest, took)
        blocked += 1 if took > 0.1 else 0
        sent += len(piece)
    journal.add("flood", bytes=sent, chunk=chunk, longest_write_s=round(longest, 4), writes_over_100ms=blocked,
                seconds=round(time.monotonic() - started, 4))
    echo(conn)


SCENARIOS = [(re.compile(pattern), scenario) for pattern, scenario in [
    (r"/ws/echo", ws_echo),
    (r"/ws/status/(\d{3})", ws_status),
    (r"/ws/close/(\d{1,5}|none|onebyte)", ws_close),
    (r"/ws/drop", ws_drop),
    (r"/ws/silent", ws_silent),
    (r"/ws/noecho", ws_noecho),
    (r"/ws/serverping", ws_serverping),
    (r"/ws/fragment", ws_fragment),
    (r"/ws/large", ws_large),
    (r"/ws/order", ws_order),
    (r"/ws/headers", ws_headers),
    (r"/ws/protocol", ws_protocol),
    (r"/ws/auth", ws_auth),
    (r"/ws/flaky", ws_flaky),
    (r"/ws/noupgrade", ws_noupgrade),
    (r"/ws/hang", ws_hang),
    (r"/ws/slowupgrade", ws_slowupgrade),
    (r"/ws/badutf8", ws_badutf8),
    (r"/ws/broken", ws_broken),
    (r"/ws/sink", ws_sink),
    (r"/ws/flood", ws_flood),
]]

server.register_upgrade("websocket", handle_upgrade)
server.register_route(("GET",), r"/ws/journal", route_journal)

if __name__ == "__main__":
    sys.exit(server.main())
