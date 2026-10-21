#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""VaRest HTTP test server (core spec 7.1). Python 3.9+ standard library only.

    python3 -I Tools/testserver/server.py [--host 127.0.0.1] [--port 0] [--tls-port P] [--ready-file PATH]
                                          [--parent-pid PID] [--fixtures-dir DIR]

--port 0 lets the OS choose. Once the server accepts connections it prints "VaRestTestServer <base url>" and,
with --ready-file, writes the base URL to that file (atomically, so a reader never sees half of it). --tls-port also
serves every route and plug-in over TLS (stdlib ssl, the test certificate in certs/: localhost, 127.0.0.1 and ::1,
signed by the throwaway test CA certs/test-ca.pem; WebSocket spec 16.3): it prints "VaRestTestServerTls <https url>" and
the ready file holds that URL on a second line. With --parent-pid it exits when that process is gone or is no longer
its parent (a reused PID), so a runner killed with SIGKILL leaves no server behind. The parent must be the process
that started the server: any other PID is refused at start (exit 2). A server that stops by itself (the parent
watch, Ctrl-C) removes its ready file.

Routes. Every response with a body has a Content-Length unless the route says chunked. Bounds (review M1): a request
body over MAX_BODY (64 MiB) gets 413, a request-target over MAX_TARGET (16 KiB) 414, and a numeric parameter that is
not an integer or is out of its range 400 ({"error", "name", ...}); each closes the connection.

  ANY  /echo[/<anything>]   JSON: method, path, query {k: [v, ...]}, headers {lower-case name: value}, content_type,
                            body (UTF-8 text or null), body_base64, body_length, body_sha256, plus form {k: [v, ...]}
                            for urlencoded bodies and parts [{name, filename, content_type, length, sha256, text}]
                            for multipart ones. Request bodies may be chunked.
  ANY  /raw[/<anything>]    JSON: request_line (as read off the socket, Latin-1), target (the request-target, not decoded),
                            query {k: [v, ...]}: what the transport put on the wire, for the URL-encoding checks
  ANY  /status/<code>       that status, JSON {"status": code} (none for 204/304); ?location=<url> sets Location,
                            ?body=<text> sends it as text/plain instead
  ANY  /delay/<ms>          sleeps, then {"delayed_ms": ms}; ?key=<k> counts the requests in flight per key
  GET  /stats?key=<k>       {"in_flight", "max_in_flight", "count"} of /delay with that key; &reset=1 forgets it
  GET  /drip?bytes=N&chunks=K&interval_ms=M   N bytes of 'x' in K writes, M ms apart, with Content-Length
  GET  /large?bytes=N&seed=S                  N deterministic bytes (application/octet-stream), X-Sha256 header
  GET  /large/json?bytes=N                    a JSON array of objects, at least N bytes; X-Item-Count header
  GET  /chunked?chunks=K&size=M               chunked transfer: a JSON array of K strings of M letters
  ANY  /redirect?to=<path>  302 to that path, with a JSON body of its own ({"redirect": "body-must-not-leak"}) that a
                            client following the redirect must never deliver
  ANY  /headers/set?<Name>=<Value>&...        200 {} with each pair as a response header
  GET  /json/profile        the FVaRestHttpTestProfile fixture (camelCase keys; an id above 2^53; no "score")
  ANY  /slowread?chunk=N&interval_ms=M        reads the request body N bytes at a time, M ms apart (TCP backpressure
                            slows the client's upload, so upload progress can be watched), then {"received": bytes}
  GET  /fixture/<name>      a file of --fixtures-dir (the benchmark's bench_*.json from Tools/gen_bench_json.py), kept
                            in memory after the first read; 404 without the option or the file, and for ".", "..", an
                            absolute name, a symlink or anything whose real path is not directly in that directory
  For M4's stage tests (core spec 7.1):
  ANY  /retry-after/<n>?key=<k>&seconds=<s>   the first n calls per key: 503 with Retry-After: s; then 200
  GET  /etag?tag=<t>        ETag "t"; a matching If-None-Match gets 304
  POST /login {"user": "neo", "password": "trinity"} -> {"accessToken", "refreshToken", "expiresIn"}
  POST /refresh {"refreshToken"} -> new tokens; the old refresh token is spent
  ANY  /protected           "Authorization: Bearer <current access token>" -> 200, else 401
  ANY  /flaky?n=<k>&key=<k> the first k calls per key close the connection without a response; then 200
  GET  /envelope/ok, /envelope/err            {"ok": true, "data": {...}} / {"ok": false, "code", "message"}
  GET  /tls/ca.pem          the test CA, for a client that verifies the TLS listener instead of allowing self-signed

Plug-ins (WebSocket spec 16.3): each module named in PLUGINS that sits next to this file is imported at start
and may call register_upgrade(token, handler) and register_route(methods, pattern, route). A request whose Upgrade
header names that token goes to handler(request_handler) instead of a route; the handler answers 101, takes over
request_handler.connection, rfile and wfile, and sets request_handler.close_connection. A registered route is matched
before the built-in ones. STATE and its lock are shared with plug-ins.
"""

import argparse
import base64
import hashlib
import http.server
import importlib
import json
import os
import random
import re
import socket
try:
    import ssl
except ImportError:
    ssl = None
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
# WebSocket spec 16.3: the TLS listener's test certificate (certs/make_certs.sh; test-only, its CA's key is gone)
CERTS_DIR = os.path.join(HERE, "certs")
# A TLS client that never finishes its handshake is dropped after this, on its own thread (Handler.setup)
TLS_HANDSHAKE_TIMEOUT = 10.0
PLUGINS = ("ws",)

LOCK = threading.Lock()
STATE = {
    "delay": {},
    "retry": {},
    "flaky": {},
    "tokens": {"access": None, "refresh": None, "serial": 0},
}
_UPGRADES = {}
FIXTURES_DIR = None
_FIXTURES = {}

# Review M1 (task 16): bounds, so a wrong request costs a 4xx and not the run's memory or time. The largest body a test
# sends is UploadProgress's 16 MiB; the largest one generated, LargeDownload's 8 MiB.
MAX_BODY = 64 * 1024 * 1024
MAX_TARGET = 16 * 1024
MAX_MS = 60 * 1000
MAX_COUNT = 1 << 16

REDIRECT_BODY = {"redirect": "body-must-not-leak"}

PROFILE = {
    "id": 9007199254740993,
    "name": "Neo",
    "items": [{"name": "sword", "level": 3}, {"name": "shield", "level": 1}],
}


class Refused(Exception):
    """A request the server will not serve: dispatch answers it with this status and a JSON body, and closes"""

    def __init__(self, status, message, **fields):
        super().__init__(message)
        self.status = status
        self.fields = dict(fields, error=message)


def register_upgrade(token, handler):
    """WebSocket spec 16.3: ws.py registers "websocket" here"""
    _UPGRADES[token.strip().lower()] = handler


def register_route(methods, pattern, route):
    """WebSocket spec 16.3: an HTTP route of a plug-in (ws.py: /ws/journal), matched before the built-in routes; methods
    is a tuple of verbs or ANY, route(h, match) as the built-in ones"""
    ROUTES.insert(0, (methods, re.compile(pattern), route))


def decode_text(data):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def parse_multipart(content_type, body):
    match = re.search(r'boundary="?([^";]+)"?', content_type)
    if not match:
        return []
    delimiter = b"--" + match.group(1).encode("latin-1")
    parts = []
    for chunk in body.split(delimiter)[1:]:
        if chunk.startswith(b"--"):
            break
        if chunk.startswith(b"\r\n"):
            chunk = chunk[2:]
        head, _, payload = chunk.partition(b"\r\n\r\n")
        if payload.endswith(b"\r\n"):
            payload = payload[:-2]
        headers = {}
        for line in head.split(b"\r\n"):
            name, _, value = line.decode("utf-8", "replace").partition(":")
            headers[name.strip().lower()] = value.strip()
        disposition = headers.get("content-disposition", "")
        name = re.search(r'\bname="([^"]*)"', disposition)
        filename = re.search(r'\bfilename="([^"]*)"', disposition)
        parts.append({
            "name": name.group(1) if name else None,
            "filename": filename.group(1) if filename else None,
            "content_type": headers.get("content-type"),
            "length": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "text": decode_text(payload) if filename is None else None,
        })
    return parts


def issue_tokens():
    """Under LOCK"""
    tokens = STATE["tokens"]
    tokens["serial"] += 1
    tokens["access"] = "access-%d-%s" % (tokens["serial"], os.urandom(4).hex())
    tokens["refresh"] = "refresh-%d-%s" % (tokens["serial"], os.urandom(4).hex())
    return {"accessToken": tokens["access"], "refreshToken": tokens["refresh"], "expiresIn": 3600}


def count_call(table, key):
    """Takes LOCK: how many calls this key had before this one"""
    with LOCK:
        seen = STATE[table].get(key, 0)
        STATE[table][key] = seen + 1
    return seen


def query_int(h, name, default, high, low=0):
    text = h.query.get(name, [str(default)])[0]
    try:
        value = int(text)
    except ValueError:
        raise Refused(400, "not an integer", name=name, value=text)
    if not low <= value <= high:
        raise Refused(400, "out of range", name=name, value=value, min=low, max=high)
    return value


def path_int(match, name, high):
    value = int(match.group(1))
    if value > high:
        raise Refused(400, "out of range", name=name, value=value, max=high)
    return value


def route_echo(h, match):
    body = h.body
    content_type = h.headers.get("Content-Type", "")
    out = {
        "method": h.command,
        "path": urllib.parse.urlsplit(h.path).path,
        "query": h.query,
        "headers": {name.lower(): value for name, value in h.headers.items()},
        "content_type": content_type,
        "body": decode_text(body),
        "body_base64": base64.b64encode(body).decode("ascii"),
        "body_length": len(body),
        "body_sha256": hashlib.sha256(body).hexdigest(),
    }
    media = content_type.split(";")[0].strip().lower()
    if media == "application/x-www-form-urlencoded":
        out["form"] = urllib.parse.parse_qs(body.decode("utf-8", "replace"), keep_blank_values=True)
    elif media.startswith("multipart/"):
        out["parts"] = parse_multipart(content_type, body)
    h.send_json(200, out)


def route_raw(h, match):
    h.send_json(200, {"request_line": h.requestline, "target": h.path, "query": h.query})


def route_status(h, match):
    code = int(match.group(1))
    headers = []
    if "location" in h.query:
        headers.append(("Location", h.query["location"][0]))
    if "body" in h.query:
        h.send_bytes(code, h.query["body"][0].encode("utf-8"), "text/plain; charset=utf-8", headers)
    else:
        h.send_json(code, {"status": code}, headers)


def route_delay(h, match):
    ms = path_int(match, "ms", MAX_MS)
    key = h.query.get("key", [None])[0]
    entry = None
    if key:
        with LOCK:
            entry = STATE["delay"].setdefault(key, {"in_flight": 0, "max_in_flight": 0, "count": 0})
            entry["in_flight"] += 1
            entry["count"] += 1
            entry["max_in_flight"] = max(entry["max_in_flight"], entry["in_flight"])
    try:
        time.sleep(ms / 1000.0)
    finally:
        if entry is not None:
            # Review M6: the entry this call counted in, not a lookup by key -- /stats?reset=1 may have dropped it meanwhile
            # (a lookup raised KeyError and lost the response) or a later call may have made a new one (it would be off by one)
            with LOCK:
                entry["in_flight"] -= 1
    h.send_json(200, {"delayed_ms": ms})


def route_stats(h, match):
    key = h.query.get("key", [""])[0]
    with LOCK:
        entry = dict(STATE["delay"].get(key, {"in_flight": 0, "max_in_flight": 0, "count": 0}))
        if h.query.get("reset", ["0"])[0] == "1":
            STATE["delay"].pop(key, None)
    h.send_json(200, entry)


def route_drip(h, match):
    total = query_int(h, "bytes", 1024, MAX_BODY)
    chunks = query_int(h, "chunks", 4, MAX_COUNT, low=1)
    interval = query_int(h, "interval_ms", 100, MAX_MS) / 1000.0
    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(total))
    h.end_headers()
    if h.command == "HEAD":
        return
    for index in range(chunks):
        if index > 0:
            time.sleep(interval)
        h.wfile.write(b"x" * (total // chunks + (1 if index < total % chunks else 0)))
        h.wfile.flush()


def route_large(h, match):
    seed = query_int(h, "seed", 1, 2 ** 63 - 1, low=-(2 ** 63))
    body = random.Random(seed).randbytes(query_int(h, "bytes", 1048576, MAX_BODY))
    h.send_bytes(200, body, "application/octet-stream", [("X-Sha256", hashlib.sha256(body).hexdigest())])


def route_large_json(h, match):
    target = query_int(h, "bytes", 1048576, MAX_BODY)
    items = []
    size = 2
    while size < target:
        index = len(items)
        text = json.dumps({"id": index, "name": "item-%d" % index, "tags": ["a", "b", "c"], "score": index * 0.5}, separators=(",", ":"))
        items.append(text)
        size += len(text) + 1
    body = ("[" + ",".join(items) + "]").encode("utf-8")
    h.send_bytes(200, body, "application/json", [("X-Item-Count", str(len(items)))])


def route_chunked(h, match):
    chunks = query_int(h, "chunks", 8, MAX_COUNT)
    size = query_int(h, "size", 1024, MAX_BODY)
    if chunks * size > MAX_BODY:
        raise Refused(400, "out of range", name="chunks*size", value=chunks * size, max=MAX_BODY)
    h.send_response(200)
    h.send_header("Content-Type", "application/json")
    h.send_header("Transfer-Encoding", "chunked")
    h.end_headers()
    pieces = [b"["]
    for index in range(chunks):
        pieces.append((b"," if index else b"") + b'"' + bytes([97 + index % 26]) * size + b'"')
    pieces.append(b"]")
    for piece in pieces:
        h.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
    h.wfile.write(b"0\r\n\r\n")


def route_redirect(h, match):
    # Review I-2 (task 16): a body of its own, so a transport or sink that kept a followed redirect's bytes breaks the
    # final JSON instead of passing unseen (m3-plan-review-concurrency.md:427-428: libcurl discards it)
    h.send_json(302, REDIRECT_BODY, [("Location", h.query.get("to", ["/echo"])[0])])


def route_headers_set(h, match):
    h.send_json(200, {}, [(name, value) for name, values in h.query.items() for value in values])


def route_profile(h, match):
    h.send_json(200, PROFILE)


def fixture_path(name):
    """The file /fixture/<name> may serve, or None (review M3): a plain file directly in FIXTURES_DIR, never through a
    symlink, "..", an absolute name or a separator"""
    if not FIXTURES_DIR or name in ("", ".", "..") or os.path.isabs(name) or "/" in name or os.sep in name:
        return None
    if os.altsep and os.altsep in name:
        return None
    root = os.path.realpath(FIXTURES_DIR)
    path = os.path.join(root, name)
    if os.path.islink(path) or os.path.dirname(os.path.realpath(path)) != root or not os.path.isfile(path):
        return None
    return path


def route_fixture(h, match):
    name = match.group(1)
    with LOCK:
        body = _FIXTURES.get(name)
    path = fixture_path(name) if body is None else None
    if path:
        with open(path, "rb") as stream:
            body = stream.read()
        with LOCK:
            _FIXTURES[name] = body
    if body is None:
        h.send_json(404, {"error": "no fixture", "name": name})
        return
    h.send_bytes(200, body, "application/json" if name.endswith(".json") else "application/octet-stream")


def route_slowread(h):
    """Called before the body is read: it reads the body itself, slowly"""
    chunk = query_int(h, "chunk", 65536, MAX_BODY, low=1)
    interval = query_int(h, "interval_ms", 100, MAX_MS) / 1000.0
    remaining = h.content_length()
    received = 0
    while remaining > 0:
        data = h.rfile.read(min(chunk, remaining))
        if not data:
            break
        received += len(data)
        remaining -= len(data)
        if remaining > 0:
            time.sleep(interval)
    h.send_json(200, {"received": received})


def route_retry_after(h, match):
    failures = int(match.group(1))
    seen = count_call("retry", h.query.get("key", ["default"])[0])
    if seen < failures:
        h.send_json(503, {"attempt": seen + 1}, [("Retry-After", h.query.get("seconds", ["1"])[0])])
    else:
        h.send_json(200, {"attempt": seen + 1})


def route_etag(h, match):
    tag = '"%s"' % h.query.get("tag", ["v1"])[0]
    if h.headers.get("If-None-Match") == tag:
        h.send_bytes(304, b"", headers=[("ETag", tag)])
    else:
        h.send_json(200, {"tag": tag}, [("ETag", tag), ("Cache-Control", "no-cache")])


def read_json(h):
    try:
        value = json.loads(h.body or b"{}")
    except ValueError:
        return {}
    return value if isinstance(value, dict) else {}


def route_login(h, match):
    request = read_json(h)
    if request.get("user") != "neo" or request.get("password") != "trinity":
        h.send_json(401, {"error": "bad credentials"})
        return
    with LOCK:
        tokens = issue_tokens()
    h.send_json(200, tokens)


def route_refresh(h, match):
    request = read_json(h)
    with LOCK:
        current = STATE["tokens"]["refresh"]
        tokens = issue_tokens() if current is not None and request.get("refreshToken") == current else None
    if tokens is None:
        h.send_json(401, {"error": "bad refresh token"})
    else:
        h.send_json(200, tokens)


def route_protected(h, match):
    with LOCK:
        current = STATE["tokens"]["access"]
    if current is not None and h.headers.get("Authorization") == "Bearer " + current:
        h.send_json(200, {"ok": True})
    else:
        h.send_json(401, {"error": "unauthorized"}, [("WWW-Authenticate", "Bearer")])


def route_flaky(h, match):
    failures = query_int(h, "n", 1, MAX_COUNT)
    seen = count_call("flaky", h.query.get("key", ["default"])[0])
    if seen < failures:
        # No status line at all: the client sees the connection end (a Connection error)
        h.close_connection = True
        try:
            h.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        return
    h.send_json(200, {"attempt": seen + 1})


def route_envelope_ok(h, match):
    h.send_json(200, {"ok": True, "data": {"id": 7, "name": "Neo"}})


def route_envelope_err(h, match):
    h.send_json(200, {"ok": False, "code": "E42", "message": "the envelope says no"})


def route_tls_ca(h, match):
    """The test CA (certs/test-ca.pem): a client that wants the TLS listener verified, not only allowed, trusts this one"""
    with open(os.path.join(CERTS_DIR, "test-ca.pem"), "rb") as stream:
        h.send_bytes(200, stream.read(), "application/x-pem-file")


ANY = None
ROUTES = [
    (ANY, r"/echo(?:/.*)?", route_echo),
    (ANY, r"/raw(?:/.*)?", route_raw),
    (ANY, r"/status/(\d{3})", route_status),
    (ANY, r"/delay/(\d+)", route_delay),
    (("GET",), r"/stats", route_stats),
    (("GET", "HEAD"), r"/drip", route_drip),
    (("GET", "HEAD"), r"/large", route_large),
    (("GET",), r"/large/json", route_large_json),
    (("GET",), r"/chunked", route_chunked),
    (ANY, r"/redirect", route_redirect),
    (ANY, r"/headers/set", route_headers_set),
    (("GET",), r"/json/profile", route_profile),
    (("GET",), r"/fixture/([A-Za-z0-9_.-]+)", route_fixture),
    (ANY, r"/retry-after/(\d+)", route_retry_after),
    (("GET",), r"/etag", route_etag),
    (("POST",), r"/login", route_login),
    (("POST",), r"/refresh", route_refresh),
    (ANY, r"/protected", route_protected),
    (ANY, r"/flaky", route_flaky),
    (("GET",), r"/envelope/ok", route_envelope_ok),
    (("GET",), r"/envelope/err", route_envelope_err),
    (("GET",), r"/tls/ca.pem", route_tls_ca),
]
ROUTES = [(methods, re.compile(pattern), route) for methods, pattern, route in ROUTES]


class Handler(http.server.BaseHTTPRequestHandler):
    # Keep-alive like a real API server: every response has a Content-Length or is chunked
    protocol_version = "HTTP/1.1"
    server_version = "VaRestTestServer/1"

    def setup(self):
        # The TLS handshake runs here, on this connection's thread, not in the accept loop: a client that never finishes
        # it must not stall the others (TlsServer.get_request wraps without shaking hands)
        if ssl is not None and isinstance(self.request, ssl.SSLSocket):
            self.request.settimeout(TLS_HANDSHAKE_TIMEOUT)
            self.request.do_handshake()
            self.request.settimeout(None)
        super().setup()

    def log_message(self, format, *args):
        # The UE log is the record; set VAREST_TESTSERVER_VERBOSE=1 to see requests
        if os.environ.get("VAREST_TESTSERVER_VERBOSE") == "1":
            super().log_message(format, *args)

    def do_GET(self):
        self.dispatch()

    do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = do_GET

    def dispatch(self):
        try:
            if len(self.path) > MAX_TARGET:
                raise Refused(414, "the request-target is over the server's limit", length=len(self.path), max=MAX_TARGET)
            upgrade = self.headers.get("Upgrade", "").strip().lower()
            if upgrade in _UPGRADES:
                _UPGRADES[upgrade](self)
                return
            url = urllib.parse.urlsplit(self.path)
            self.query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
            if url.path == "/slowread":
                route_slowread(self)
                return
            self.body = self.read_body()
            for methods, pattern, route in ROUTES:
                match = pattern.fullmatch(url.path)
                if match and (methods is ANY or self.command in methods):
                    route(self, match)
                    return
            self.send_json(404, {"error": "no route", "path": url.path})
        except Refused as refused:
            # Before any byte of a response; a body may be left unread, so the connection ends with this answer
            self.send_json(refused.status, refused.fields, [("Connection", "close")])
            self.close_connection = True
        except (BrokenPipeError, ConnectionResetError):
            # The client gave up (a timeout or cancel test)
            self.close_connection = True

    def content_length(self):
        text = self.headers.get("Content-Length")
        if not text:
            return 0
        try:
            length = int(text)
        except ValueError:
            raise Refused(400, "bad Content-Length", value=text)
        if length < 0:
            raise Refused(400, "bad Content-Length", value=text)
        if length > MAX_BODY:
            raise Refused(413, "the body is over the server's limit", length=length, max=MAX_BODY)
        return length

    def read_body(self):
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            chunks = []
            total = 0
            while True:
                line = self.rfile.readline(1024).split(b";")[0].strip()
                try:
                    size = int(line or b"0", 16)
                except ValueError:
                    raise Refused(400, "bad chunk size", value=line.decode("latin-1"))
                # Checked before the chunk is read: the declared size alone is refused
                if size < 0 or total + size > MAX_BODY:
                    raise Refused(413, "the body is over the server's limit", length=total + size, max=MAX_BODY)
                if size == 0:
                    while self.rfile.readline(1024) not in (b"\r\n", b"\n", b""):
                        pass
                    return b"".join(chunks)
                chunks.append(self.rfile.read(size))
                total += size
                self.rfile.readline(1024)
        length = self.content_length()
        return self.rfile.read(length) if length > 0 else b""

    def send_bytes(self, status, body, content_type="application/octet-stream", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        has_body = status not in (204, 304)
        if has_body:
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if has_body and self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, status, value, headers=()):
        self.send_bytes(status, json.dumps(value, separators=(",", ":")).encode("utf-8"), "application/json", headers)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True
    # socketserver's default listen backlog is 5 (TCPServer.request_queue_size): a test that opens a dozen connections at
    # once would see the surplus SYNs dropped and retried a second later, a delay no test asked for
    request_queue_size = 64

    def handle_error(self, request, client_address):
        # A client that cancels, times out or drops a keep-alive connection resets it, and a TLS client may refuse our
        # test certificate or speak plain HTTP to the TLS port: test scenarios, not errors
        error = sys.exc_info()[1]
        if ssl is not None and isinstance(error, ssl.SSLError):
            # Swallowed, but a TLS client library that fails against this listener (the libwebsockets probe) needs the
            # reason: VAREST_TESTSERVER_VERBOSE=1 puts it in the server log
            if os.environ.get("VAREST_TESTSERVER_VERBOSE") == "1":
                print("VaRestTestServer: TLS error from %s:%s: reason=%s: %s" % (client_address[0], client_address[1], getattr(error, "reason", None), error), file=sys.stderr, flush=True)
            return
        if isinstance(error, (ConnectionResetError, BrokenPipeError, TimeoutError, socket.timeout)):
            return
        super().handle_error(request, client_address)


def make_server(host="127.0.0.1", port=0):
    return Server((host, port), Handler)


class TlsServer(Server):
    """The TLS listener (WebSocket spec 16.3): the same routes and plug-ins, over stdlib ssl"""

    def __init__(self, address, handler, context):
        self.tls_context = context
        super().__init__(address, handler)

    def get_request(self):
        connection, address = self.socket.accept()
        return self.tls_context.wrap_socket(connection, server_side=True, do_handshake_on_connect=False), address


def make_tls_context():
    if ssl is None:
        raise OSError("this Python has no ssl module")
    chain, key = os.path.join(CERTS_DIR, "test-server.pem"), os.path.join(CERTS_DIR, "test-server.key")
    # load_cert_chain's FileNotFoundError carries no file name: check first, so the message names the missing file
    for path in (chain, key):
        if not os.path.isfile(path):
            raise FileNotFoundError(2, "no such file", path)
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(chain, key)
    return context


def make_tls_server(host="127.0.0.1", port=0):
    return TlsServer((host, port), Handler, make_tls_context())


def load_plugins():
    # Run as a script this module is __main__; a plug-in's "import server" must get this module, not a second copy
    # with its own STATE and registry
    sys.modules.setdefault("server", sys.modules[__name__])
    # python3 -I drops the script directory from sys.path (WebSocket spec 16.3)
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    # No __pycache__ in the source tree (it is not ignored)
    sys.dont_write_bytecode = True
    for name in PLUGINS:
        if os.path.exists(os.path.join(HERE, name + ".py")):
            importlib.import_module(name)


def write_ready(path, url):
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        stream.write(url + "\n")
    os.replace(temporary, path)


def watch_parent(pid, server):
    # POSIX only: there os.kill(pid, 0) probes, on Windows it would terminate the process. A PID can be reused, so the
    # server also checks that it is still that process's child: when the parent dies, the server is re-parented.
    if os.name != "posix":
        return
    while True:
        time.sleep(0.5)
        if os.getppid() != pid:
            server.shutdown()
            return
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            server.shutdown()
            return
        except PermissionError:
            pass


def main(argv=None):
    parser = argparse.ArgumentParser(description="VaRest HTTP test server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--ready-file")
    parser.add_argument("--parent-pid", type=int)
    parser.add_argument("--fixtures-dir")
    parser.add_argument("--tls-port", type=int, help="also serve every route with TLS on this port (0: the OS picks)")
    args = parser.parse_args(argv)
    # Review M-13: the watch compares getppid() with this PID, so it must be the parent now; any other PID would stop the
    # server half a second after it started, with nothing said
    if args.parent_pid and os.name == "posix" and os.getppid() != args.parent_pid:
        parser.error("--parent-pid %d is not the process that started this one (%d)" % (args.parent_pid, os.getppid()))

    global FIXTURES_DIR
    FIXTURES_DIR = args.fixtures_dir

    load_plugins()
    server = make_server(args.host, args.port)
    urls = ["http://%s:%d" % (args.host, server.server_address[1])]
    extras = []
    if args.tls_port is not None:
        try:
            extras.append(make_tls_server(args.host, args.tls_port))
        except OSError as error:
            # Review M-11: the runner only says "did not start -- see <log>"; this line names what failed. Only an error
            # that carries a file name gets one (a missing certificate); a bind failure or a certificate that does not
            # load (ssl.SSLError is an OSError) says which step instead of pointing at the directory
            where = getattr(error, "filename", None) or ("loading the certificate chain" if ssl is not None and isinstance(error, ssl.SSLError) else "binding %s:%d" % (args.host, args.tls_port))
            print("VaRestTestServer: cannot start the TLS listener: %s: %s" % (where, error), file=sys.stderr, flush=True)
            server.server_close()
            return 2
        urls.append("https://%s:%d" % (args.host, extras[0].server_address[1]))
    if args.parent_pid:
        threading.Thread(target=watch_parent, args=(args.parent_pid, server), daemon=True).start()
    print("VaRestTestServer " + urls[0], flush=True)
    if extras:
        print("VaRestTestServerTls " + urls[1], flush=True)
    for extra in extras:
        threading.Thread(target=extra.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True).start()
    if args.ready_file:
        write_ready(args.ready_file, "\n".join(urls))
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        # The parent watch stops the plain listener; the TLS one goes with it
        for extra in extras:
            extra.shutdown()
            extra.server_close()
        server.server_close()
        # The parent watch ends here when the runner was killed and its own cleanup never ran: a ready file must not
        # name a server that is gone
        if args.ready_file:
            try:
                os.remove(args.ready_file)
            except OSError:
                pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
