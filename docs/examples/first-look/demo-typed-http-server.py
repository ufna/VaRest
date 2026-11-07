#!/usr/bin/python3
"""Owned loopback-only typed-data demo; not a production server.

Records only these fixed public demo values when the submitted shape matches.
No arbitrary request bodies, URLs, headers or credentials are journalled.
The same handler code was used by the actual Development/Shipping first-look runs.
"""
import argparse
import hashlib
import http.server
import json
import threading
import urllib.parse


def strict_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate demo key')
        result[key] = value
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--receipts', required=True)
    args = parser.parse_args()
    journal = open(args.receipts, 'x', encoding='utf-8')
    lock = threading.Lock()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def setup(self):
            self.request.settimeout(5)
            super().setup()

        def log_message(self, *values):
            pass

        def reply(self, status, value, consumed=None, request_sha256=None):
            body = json.dumps(value, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.close_connection = True
            route = urllib.parse.urlsplit(self.path).path
            if route not in ('/profile', '/matches'):
                route = '<other>'
            row = dict(method=self.command, path=route, status=status,
                       response_bytes=len(body), response_sha256=hashlib.sha256(body).hexdigest())
            if consumed is not None:
                row['consumed_fixed_demo_fields'] = consumed
                row['request_sha256'] = request_sha256
            with lock:
                journal.write(json.dumps(row, separators=(',', ':')) + '\n')
                journal.flush()

        def do_GET(self):
            if urllib.parse.urlsplit(self.path).path == '/profile':
                self.reply(200, dict(id=42, displayName='Alex Demo', level=27,
                                     online=True, guild='Northwind'))
            else:
                self.reply(404, dict(error='unknown demo route'))

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).path != '/matches':
                self.reply(404, dict(error='unknown demo route'))
                return
            try:
                count = int(self.headers.get('Content-Length', '0'))
                if not 0 < count <= 16384:
                    raise ValueError('bounded content required')
                body = self.rfile.read(count)
                if len(body) != count:
                    raise ValueError('truncated request')
                value = json.loads(body.decode('utf-8'), object_pairs_hook=strict_object,
                                   parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite')))
                expected = dict(map='Alex Demo', maxPlayers=27, ranked=True)
                if (not isinstance(value, dict) or value != expected
                        or type(value.get('maxPlayers')) is not int
                        or type(value.get('ranked')) is not bool):
                    self.reply(422, dict(error='typed profile fields did not reach match request'))
                    return
            except (ValueError, UnicodeError):
                self.reply(400, dict(error='invalid bounded demo JSON'))
                return
            self.reply(201, dict(id='50ee371f-8ecf-47df-8b1d-5326a3fa9d90',
                                 status='Waiting', players=value['maxPlayers']),
                       consumed=expected, request_sha256=hashlib.sha256(body).hexdigest())

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 18423), Handler)
    server.daemon_threads = True
    print('TYPED_HTTP_PROOF_READY loopback18423', flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        server.server_close()
        journal.close()


if __name__ == '__main__':
    main()
