#!/usr/bin/env python3
"""Owned loopback peer for three separately opt-in packaged native closure requests.
No TLS/credentials/external address. Root owns namespaces, startup and termination.
"""
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
import argparse
import json
import time


REPLIES = [b'{"name":"scalar","maybe":{"text":"leaf","number":23},"mode":"One","times":["2026-11-18T00:00:00.000Z"],"labels":["lower","UPPER"],"dates":{"date":"2026-11-18T00:00:00.000Z"}}', b'[{"name":"array0","maybe":{"text":"leaf","number":23},"mode":"One","times":["2026-11-18T00:00:00.000Z"],"labels":["lower","UPPER"],"dates":{"date":"2026-11-18T00:00:00.000Z"}},{"name":"array1","mode":"One","times":["2026-11-18T00:00:00.000Z"],"labels":["lower","UPPER"],"dates":{"date":"2026-11-18T00:00:00.000Z"}}]', b'{"one":{"name":"map0","maybe":{"text":"leaf","number":23},"mode":"One","times":["2026-11-18T00:00:00.000Z"],"labels":["lower","UPPER"],"dates":{"date":"2026-11-18T00:00:00.000Z"}},"two":{"name":"map1","mode":"One","times":["2026-11-18T00:00:00.000Z"],"labels":["lower","UPPER"],"dates":{"date":"2026-11-18T00:00:00.000Z"}}}']

TARGETS = ["/m5/native-closure/" + str(i) for i in range(3)]


class Peer(HTTPServer):
    allow_reuse_address = False

    def __init__(self, port, journal, expected):
        self.journal = journal
        self.expected = expected
        self.next_case = self.requests = 0
        self.failed = False
        super().__init__(("127.0.0.1", port), Handler)
        self.timeout = 0.25

    def record(self, event):
        self.journal.write(json.dumps(event, separators=(",", ":"), sort_keys=True) + "\n")
        self.journal.flush()

    def handle_error(self, request, client_address):
        self.failed = True
        self.record({"event": "handler_io_error"})


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.connection.settimeout(5.0)

    def log_message(self, *args):
        pass

    def send_error(self, code, message=None, explain=None):
        self.server.failed = True
        self.server.record({"event": "protocol_error", "status": code})
        super().send_error(code, message, explain)

    def do_GET(self):
        peer = self.server
        peer.requests += 1
        case = peer.next_case
        reasons = []
        fields = list(self.headers.raw_items())
        if len(fields) > 32 or sum(len(k) + len(v) + 4 for k, v in fields) > 8192:
            reasons.append("header_bound")
        if peer.expected == 0 or case >= 3 or self.path != TARGETS[case]:
            reasons.append("exact_method_target_sequence")
        if self.headers.get_all("Authorization", []):
            reasons.append("unexpected_authorization")
        if self.headers.get_all("Transfer-Encoding", []):
            reasons.append("unexpected_transfer_encoding")
        if self.headers.get_all("Content-Length", []) not in ([], ["0"]):
            reasons.append("GET_body_forbidden")
        accepted = not reasons
        reply = REPLIES[case] if accepted else b'{"fixture":"rejected"}'
        self.send_response(200 if accepted else 400)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(reply)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(reply)
        self.wfile.flush()
        self.close_connection = True
        peer.record({"event": "request", "case": case, "method": "GET", "raw_target": self.path,
                     "actual_request_header_fields": fields, "declared_body": self.headers.get_all("Content-Length", []),
                     "accepted": accepted, "reasons": reasons, "status": 200 if accepted else 400,
                     "response_body_hex": reply.hex()})
        if accepted:
            peer.next_case += 1
        else:
            peer.failed = True


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=18766)
    p.add_argument("--journal", type=Path, required=True)
    p.add_argument("--ready", type=Path, required=True)
    p.add_argument("--stop", type=Path, required=True)
    p.add_argument("--seconds", type=float, default=90.0)
    p.add_argument("--expected", type=int, choices=(0, 3), default=3)
    args = p.parse_args()
    if not 1024 <= args.port <= 65535 or not 0 < args.seconds <= 300:
        p.error("bounded unprivileged loopback port/deadline required")
    with args.journal.open("x", encoding="utf-8") as journal:
        with Peer(args.port, journal, args.expected) as peer:
            ready = {"host": "127.0.0.1", "port": args.port, "cases": 3, "expected_requests": args.expected}
            peer.record(dict(event="ready", **ready))
            with args.ready.open("x", encoding="utf-8") as f:
                json.dump(ready, f)
            deadline = time.monotonic() + args.seconds
            while not args.stop.exists() and time.monotonic() < deadline:
                peer.handle_request()
                if peer.failed or (args.expected == 3 and peer.next_case == 3):
                    break
            passed = not peer.failed and peer.requests == peer.next_case == args.expected
            peer.record({"event": "closed", "passed": passed, "accepted_cases": peer.next_case,
                         "requests": peer.requests, "failed": peer.failed})
            return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
