#!/usr/bin/python3
"""Local VaRest first-look HTTP demo, not a production backend or Socket.IO server.

GET /profile was exercised by the actual cooked Blueprint demo. No credentials
or request bodies are journalled. Choose a fresh receipt file for each run.
"""
import argparse
import hashlib
import http.server
import json
import threading
import urllib.parse


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--receipts', required=True)
    args = parser.parse_args()
    lock = threading.Lock()
    journal = open(args.receipts, 'x', encoding='utf-8')

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def setup(self):
            self.request.settimeout(5)
            super().setup()

        def log_message(self, fmt, *values):
            pass

        def reply(self, status, value):
            body = json.dumps(value, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
            self.send_response(status)
            self.send_header('Content-Type', 'application/json; charset=utf-8')
            self.send_header('Content-Length', str(len(body)))
            self.send_header('Connection', 'close')
            self.end_headers()
            self.wfile.write(body)
            self.wfile.flush()
            self.close_connection = True
            path = urllib.parse.urlsplit(self.path).path
            if path not in ['/profile', '/inventory', '/matches']:
                path = '<other>'
            with lock:
                journal.write(json.dumps(dict(method=self.command, path=path, status=status,
                                              response_bytes=len(body), response_sha256=hashlib.sha256(body).hexdigest())) + '\n')
                journal.flush()

        def do_GET(self):
            path = urllib.parse.urlsplit(self.path).path
            if path == '/profile':
                self.reply(200, dict(id=42, displayName='Alex Demo', level=27,
                                     online=True, guild='Northwind'))
            elif path == '/inventory':
                self.reply(200, [dict(id=1, name='Explorer Jacket', rarity='Rare', quantity=1),
                                 dict(id=2, name='Health Potion', rarity='Common', quantity=5)])
            else:
                self.reply(404, dict(error='demo route not found'))

        def do_POST(self):
            if urllib.parse.urlsplit(self.path).path != '/matches':
                self.reply(404, dict(error='demo route not found'))
                return
            try:
                count = int(self.headers.get('Content-Length', '0'))
                if not 0 < count <= 1048576:
                    raise ValueError('bounded content required')
                value = json.loads(self.rfile.read(count).decode('utf-8'))
                if not isinstance(value, dict):
                    raise ValueError('object required')
            except (ValueError, UnicodeError):
                self.reply(400, dict(error='invalid demo JSON'))
                return
            self.reply(201, dict(id='50ee371f-8ecf-47df-8b1d-5326a3fa9d90',
                                 status='Waiting', players=1))

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 18423), Handler)
    server.daemon_threads = True
    print('FIRST_LOOK_HTTP_READY loopback18423', flush=True)
    try:
        server.serve_forever(poll_interval=0.2)
    finally:
        server.server_close()
        journal.close()


if __name__ == '__main__':
    main()
