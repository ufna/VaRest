#!/usr/bin/env python3
"""Owned opt-in M5 wire fixture. No imports from product helpers and no certificate/key paths.
Root supplies fresh exclusive journal/ready outputs and an owned stop path. Single-threaded,
loopback-only, eight accepted requests, bounded headers/body/time, strict failure journal.
"""
import argparse
import hashlib
import json
import socket
import signal
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

WIRE = b'{"ExactFlag":false,"Wire.Score":7,"sanitizedLabel":"alpha","nullCount":null,"nullable":"kept","bytes":"AQID","nullState":null,"mapped":"wire-ONE","pretty":"Pretty State Two","states":["wire-ONE","Pretty State Two"],"stateMap":{"one":"wire-ONE","pretty":"Pretty State Two"},"nested":{"Nested.Flag":true,"Nested.Value":9},"scalars":[1,2,3],"scalarMap":{"x":4,"y":5},"stamp":"2026-10-25T12:34:56.000Z"}'
REPLIES = [b'{"ExactFlag":false,"Wire.Score":7,"sanitizedLabel":"alpha","nullable":null,"bytes":"AQID","mapped":"wire-ONE","pretty":"Pretty State Two","states":["wire-ONE","Pretty State Two"],"stateMap":{"one":"wire-ONE","pretty":"Pretty State Two"},"nested":{"Nested.Flag":true,"Nested.Value":9},"scalars":[1,2,3],"scalarMap":{"x":4,"y":5},"stamp":"2026-10-25T12:34:56.000Z","decodeOnly":"server","ignored":"tamper"}', b'{"ExactFlag":false,"Wire.Score":7,"sanitizedLabel":"alpha","nullable":null,"bytes":"AQID","mapped":"wire-ONE","pretty":"Pretty State Two","states":["wire-ONE","Pretty State Two"],"stateMap":{"one":"wire-ONE","pretty":"Pretty State Two"},"nested":{"Nested.Flag":true,"Nested.Value":9},"scalars":[1,2,3],"scalarMap":{"x":4,"y":5},"stamp":"2026-10-25T12:34:56.000Z","decodeOnly":"server","ignored":"tamper"}', b'[3,4,5]', b'{"left":7,"right":8}', b'"wire-ONE"', b'"2026-10-25T12:34:56.000Z"', b'{"ExactFlag":false,"Wire.Score":7,"sanitizedLabel":"alpha","nullable":null,"bytes":"AQID","mapped":"wire-ONE","pretty":"Pretty State Two","states":["wire-ONE","Pretty State Two"],"stateMap":{"one":"wire-ONE","pretty":"Pretty State Two"},"nested":{"Nested.Flag":true,"Nested.Value":9},"scalars":[1,2,3],"scalarMap":{"x":4,"y":5},"stamp":"2026-10-25T12:34:56.000Z","decodeOnly":"server","ignored":"tamper"}', b'42']

TARGETS = [f"/m5/{'endpoint' if i % 2 == 0 else 'send'}/{i}" for i in range(8)]
TARGETS[0] += "?sort=lower&Sort=UPPER"
MAX_BODY = 16384
MAX_HEADER_BYTES = 8192
MAX_HEADER_FIELDS = 32

class Fixture(HTTPServer):
    allow_reuse_address = False
    def __init__(self, port, journal):
        self.journal = journal
        self.next_case = 0
        self.failed = False
        self.requests = 0
        super().__init__(("127.0.0.1", port), Handler)
        self.timeout = 0.25
    def handle_error(self, request, client_address):
        self.failed = True
        self.record({"event": "handler_io_error"})
    def record(self, event):
        self.journal.write(json.dumps(event, separators=(",", ":"), sort_keys=True) + "\n")
        self.journal.flush()

class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    rbufsize = 8192
    def setup(self):
        super().setup()
        self.connection.settimeout(5.0)
    def log_message(self, fmt, *args):
        pass  # Actual bounded evidence goes to the exclusive owned journal.
    def send_error(self, code, message=None, explain=None):
        self.server.failed = True
        self.server.record({"event": "protocol_error", "status": code})
        super().send_error(code, message, explain)
    def do_POST(self):
        fixture = self.server
        fixture.requests += 1
        case = fixture.next_case
        headers = list(self.headers.raw_items())
        reasons = []
        # The stdlib parser already caps a header line at 64 KiB and count at 100;
        # reject a stricter total/count bound before reading any body.
        if len(headers) > MAX_HEADER_FIELDS or sum(len(k) + len(v) + 4 for k, v in headers) > MAX_HEADER_BYTES:
            reasons.append("header_bound")
        lengths = self.headers.get_all("Content-Length", [])
        if len(lengths) != 1 or not lengths[0].isascii() or not lengths[0].isdigit():
            reasons.append("one_canonical_content_length_required")
            size = 0
        else:
            size = int(lengths[0])
            if str(size) != lengths[0] or not (0 < size <= MAX_BODY):
                reasons.append("body_bound_or_noncanonical_length")
        if self.headers.get_all("Transfer-Encoding", []):
            reasons.append("transfer_encoding_forbidden")
        if self.headers.get_all("X-M5-Dup", []) != ["first, second"]:
            reasons.append("coalesced_ordered_asset_headers")
        if self.headers.get_all("Content-Type", []) != ["application/json"]:
            reasons.append("content_type")
        if self.headers.get_all("Authorization", []):
            reasons.append("unexpected_authorization")
        if case >= len(TARGETS) or self.path != TARGETS[case]:
            reasons.append("exact_method_target_sequence")
        raw = b""
        if not reasons:
            try:
                raw = self.rfile.read(size)
            except (OSError, socket.timeout):
                reasons.append("body_read_timeout")
            if len(raw) != size:
                reasons.append("truncated_body")
            if raw != WIRE:
                reasons.append("exact_raw_body")
        accepted = not reasons
        status = (422 if case == 6 else 200) if accepted else 400
        reply = REPLIES[case] if accepted else b'{"fixture":"wire_rejected"}'
        if accepted:
            fixture.next_case += 1
        else:
            fixture.failed = True
        fixture.record({"event": "request", "index": fixture.requests, "case": case,
                        "method": self.command, "raw_target": self.path,
                        "coalesced_request_headers": self.headers.get_all("X-M5-Dup", []),
                        "actual_request_header_fields": [[k, v] for k, v in headers if k.lower() == "x-m5-dup"],
                        "content_type": self.headers.get_all("Content-Type", []),
                        "declared_body_bytes": size, "actual_body_bytes": len(raw),
                        "raw_body_hex": raw.hex(), "raw_body_sha256": hashlib.sha256(raw).hexdigest(),
                        "accepted": accepted, "reasons": reasons, "status": status,
                        "response_body_hex": reply.hex(), "response_duplicate_headers": ["alpha", "omega"]})
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.send_header("X-M5-Reply", "alpha")
        self.send_header("X-M5-Reply", "omega")
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(reply)
        self.wfile.flush()
        self.close_connection = True
    def do_GET(self):
        self.send_error(405, "POST only")
        self.close_connection = True

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18765)
    parser.add_argument("--journal", required=True, type=Path)
    parser.add_argument("--ready", required=True, type=Path)
    parser.add_argument("--stop", required=True, type=Path)
    parser.add_argument("--seconds", type=int, default=120)
    args = parser.parse_args()
    if not (1024 <= args.port <= 65535 and 1 <= args.seconds <= 300):
        parser.error("port/finite lifetime out of range")
    if len({args.journal.resolve(), args.ready.resolve(), args.stop.resolve()}) != 3:
        parser.error("owned journal/ready/stop paths must differ")
    if args.stop.exists():
        parser.error("owned stop path already exists")
    # Exclusive outputs: never overwrite existing evidence. No TLS, subprocess, external service or worker thread.
    with args.journal.open("x", encoding="utf-8") as journal:
        with Fixture(args.port, journal) as fixture:
            with args.ready.open("x", encoding="utf-8") as ready:
                ready.write(json.dumps({"host": "127.0.0.1", "port": args.port, "cases": 8}) + "\n")
            fixture.record({"event": "ready", "host": "127.0.0.1", "port": args.port, "cases": 8})
            deadline = time.monotonic() + args.seconds
            def request_deadline(signum, frame):
                raise TimeoutError("owned absolute request deadline")
            previous_alarm = signal.signal(signal.SIGALRM, request_deadline)
            try:
                while fixture.next_case < 8 and not fixture.failed and fixture.requests < 16 and not args.stop.exists() and time.monotonic() < deadline:
                    # Linux-only owned main process: absolute bound also covers slow header/body streams.
                    signal.setitimer(signal.ITIMER_REAL, min(5.0, max(0.001, deadline - time.monotonic())))
                    try:
                        fixture.handle_request()
                    except TimeoutError:
                        fixture.failed = True
                        fixture.record({"event": "absolute_request_timeout"})
                    finally:
                        signal.setitimer(signal.ITIMER_REAL, 0)
            finally:
                signal.setitimer(signal.ITIMER_REAL, 0)
                signal.signal(signal.SIGALRM, previous_alarm)
            passed = fixture.next_case == 8 and not fixture.failed
            fixture.record({"event": "closed", "passed": passed, "accepted_cases": fixture.next_case,
                            "requests": fixture.requests, "failed": fixture.failed})
            return 0 if passed else 2

if __name__ == "__main__":
    raise SystemExit(main())
