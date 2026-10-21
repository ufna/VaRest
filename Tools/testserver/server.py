#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""VaRest HTTP test server (core spec 7.1). Python 3.9+ standard library only.

    python3 -I Tools/testserver/server.py [--host 127.0.0.1] [--port 0] [--ready-file PATH] [--parent-pid PID]
                                          [--fixtures-dir DIR]

--port 0 lets the OS choose. Once the server accepts connections it prints "VaRestTestServer <base url>" and,
with --ready-file, writes the base URL to that file (atomically, so a reader never sees half of it). With
--parent-pid it exits when that process is gone or is no longer its parent (a reused PID), so a runner killed
with SIGKILL leaves no server behind. The parent must be the process that started the server: any other PID is
refused at start (exit 2). A server that stops by itself (the parent watch, Ctrl-C) removes its ready file.

Routes. Every response with a body has a Content-Length unless the route says chunked.

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
  ANY  /redirect?to=<path>  302 to that path
  ANY  /headers/set?<Name>=<Value>&...        200 {} with each pair as a response header
  GET  /json/profile        the FVaRestHttpTestProfile fixture (camelCase keys; an id above 2^53; no "score")
  ANY  /slowread?chunk=N&interval_ms=M        reads the request body N bytes at a time, M ms apart (TCP backpressure
                            slows the client's upload, so upload progress can be watched), then {"received": bytes}
  GET  /fixture/<name>      a file of --fixtures-dir (the benchmark's bench_*.json from Tools/gen_bench_json.py), kept
                            in memory after the first read; 404 without the option or the file
  For M4's stage tests (core spec 7.1):
  ANY  /retry-after/<n>?key=<k>&seconds=<s>   the first n calls per key: 503 with Retry-After: s; then 200
  GET  /etag?tag=<t>        ETag "t"; a matching If-None-Match gets 304
  POST /login {"user": "neo", "password": "trinity"} -> {"accessToken", "refreshToken", "expiresIn"}
  POST /refresh {"refreshToken"} -> new tokens; the old refresh token is spent
  ANY  /protected           "Authorization: Bearer <current access token>" -> 200, else 401
  ANY  /flaky?n=<k>&key=<k> the first k calls per key close the connection without a response; then 200
  GET  /envelope/ok, /envelope/err            {"ok": true, "data": {...}} / {"ok": false, "code", "message"}

Plug-ins (WebSocket spec 16.3): each module named in PLUGINS that sits next to this file is imported at start
and may call register_upgrade(token, handler). A request whose Upgrade header names that token goes to
handler(request_handler) instead of a route; the handler answers 101, takes over request_handler.connection,
rfile and wfile, and sets request_handler.close_connection. STATE and its lock are shared with plug-ins.
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
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
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

PROFILE = {
    "id": 9007199254740993,
    "name": "Neo",
    "items": [{"name": "sword", "level": 3}, {"name": "shield", "level": 1}],
}


def register_upgrade(token, handler):
    """WebSocket spec 16.3: ws.py registers "websocket" here"""
    _UPGRADES[token.strip().lower()] = handler


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


def query_int(h, name, default):
    return int(h.query.get(name, [str(default)])[0])


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
    ms = int(match.group(1))
    key = h.query.get("key", [None])[0]
    if key:
        with LOCK:
            entry = STATE["delay"].setdefault(key, {"in_flight": 0, "max_in_flight": 0, "count": 0})
            entry["in_flight"] += 1
            entry["count"] += 1
            entry["max_in_flight"] = max(entry["max_in_flight"], entry["in_flight"])
    try:
        time.sleep(ms / 1000.0)
    finally:
        if key:
            with LOCK:
                STATE["delay"][key]["in_flight"] -= 1
    h.send_json(200, {"delayed_ms": ms})


def route_stats(h, match):
    key = h.query.get("key", [""])[0]
    with LOCK:
        entry = dict(STATE["delay"].get(key, {"in_flight": 0, "max_in_flight": 0, "count": 0}))
        if h.query.get("reset", ["0"])[0] == "1":
            STATE["delay"].pop(key, None)
    h.send_json(200, entry)


def route_drip(h, match):
    total = query_int(h, "bytes", 1024)
    chunks = max(1, query_int(h, "chunks", 4))
    interval = query_int(h, "interval_ms", 100) / 1000.0
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
    body = random.Random(query_int(h, "seed", 1)).randbytes(query_int(h, "bytes", 1048576))
    h.send_bytes(200, body, "application/octet-stream", [("X-Sha256", hashlib.sha256(body).hexdigest())])


def route_large_json(h, match):
    target = query_int(h, "bytes", 1048576)
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
    chunks = query_int(h, "chunks", 8)
    size = query_int(h, "size", 1024)
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
    h.send_bytes(302, b"", "text/plain", [("Location", h.query.get("to", ["/echo"])[0])])


def route_headers_set(h, match):
    h.send_json(200, {}, [(name, value) for name, values in h.query.items() for value in values])


def route_profile(h, match):
    h.send_json(200, PROFILE)


def route_fixture(h, match):
    name = match.group(1)
    path = os.path.join(FIXTURES_DIR, name) if FIXTURES_DIR else None
    with LOCK:
        body = _FIXTURES.get(name)
    if body is None and path and os.path.isfile(path):
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
    chunk = max(1, query_int(h, "chunk", 65536))
    interval = query_int(h, "interval_ms", 100) / 1000.0
    remaining = int(h.headers.get("Content-Length") or 0)
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
    failures = query_int(h, "n", 1)
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
]
ROUTES = [(methods, re.compile(pattern), route) for methods, pattern, route in ROUTES]


class Handler(http.server.BaseHTTPRequestHandler):
    # Keep-alive like a real API server: every response has a Content-Length or is chunked
    protocol_version = "HTTP/1.1"
    server_version = "VaRestTestServer/1"

    def log_message(self, format, *args):
        # The UE log is the record; set VAREST_TESTSERVER_VERBOSE=1 to see requests
        if os.environ.get("VAREST_TESTSERVER_VERBOSE") == "1":
            super().log_message(format, *args)

    def do_GET(self):
        self.dispatch()

    do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = do_GET

    def dispatch(self):
        try:
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
        except (BrokenPipeError, ConnectionResetError):
            # The client gave up (a timeout or cancel test)
            self.close_connection = True

    def read_body(self):
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            chunks = []
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                if size == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    return b"".join(chunks)
                chunks.append(self.rfile.read(size))
                self.rfile.readline()
        length = int(self.headers.get("Content-Length") or 0)
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
        # A client that cancels, times out or drops a keep-alive connection resets it: a test scenario, not an error
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError, TimeoutError)):
            return
        super().handle_error(request, client_address)


def make_server(host="127.0.0.1", port=0):
    return Server((host, port), Handler)


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
    args = parser.parse_args(argv)
    # Review M-13: the watch compares getppid() with this PID, so it must be the parent now; any other PID would stop the
    # server half a second after it started, with nothing said
    if args.parent_pid and os.name == "posix" and os.getppid() != args.parent_pid:
        parser.error("--parent-pid %d is not the process that started this one (%d)" % (args.parent_pid, os.getppid()))

    global FIXTURES_DIR
    FIXTURES_DIR = args.fixtures_dir

    load_plugins()
    server = make_server(args.host, args.port)
    url = "http://%s:%d" % (args.host, server.server_address[1])
    if args.parent_pid:
        threading.Thread(target=watch_parent, args=(args.parent_pid, server), daemon=True).start()
    print("VaRestTestServer " + url, flush=True)
    if args.ready_file:
        write_ready(args.ready_file, url)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
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
