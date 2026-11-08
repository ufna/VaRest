#!/usr/bin/env python3
"""One-shot local peer for the ordinary VaRest public cooked socket demo.

Run with a fresh --receipts path. Listens on IPv4 loopback18445 only, accepts
one real RFC6455 client and only the fixed public demo text, echoes its bytes,
echoes close1000 and records actual EOF. It logs no arbitrary bodies/headers.
No TLS or external service; this does not claim WSS/provider qualification.
"""
import argparse
import base64
import hashlib
import json
import socket
import struct

MARKER = b'"VaRest cooked WebSocket demo"'
GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
MAX_FRAME = 65536


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--receipts", required=True)
    args = parser.parse_args()
    # Exclusive creation refuses any occupied file; no existing data is read.
    with open(args.receipts, "x", encoding="utf-8") as journal:
        def record(event, **fields):
            journal.write(json.dumps(dict(event=event, **fields), separators=(",", ":")) + "\n")
            journal.flush()

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
            listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            listener.bind(("127.0.0.1", 18445))
            listener.listen(1)
            listener.settimeout(45)
            print("PUBLIC_SOCKET_DEMO_READY loopback18445", flush=True)
            try:
                connection, address = listener.accept()
                with connection:
                    connection.settimeout(12)
                    pending = bytearray()

                    def receive(count):
                        while len(pending) < count:
                            chunk = connection.recv(min(4096, count - len(pending)))
                            if not chunk:
                                raise ValueError("premature peer EOF")
                            pending.extend(chunk)
                        value = bytes(pending[:count])
                        del pending[:count]
                        return value

                    while b"\r\n\r\n" not in pending:
                        if len(pending) >= 8192:
                            raise ValueError("upgrade exceeds demo header bound")
                        chunk = connection.recv(min(1024, 8192 - len(pending)))
                        if not chunk:
                            raise ValueError("upgrade ended early")
                        pending.extend(chunk)
                    end = pending.index(b"\r\n\r\n")
                    request = bytes(pending[:end])
                    del pending[:end + 4]
                    lines = request.split(b"\r\n")
                    if not lines or lines[0] != b"GET /echo HTTP/1.1":
                        raise ValueError("unexpected demo request line")
                    headers = {}
                    for line in lines[1:]:
                        key, separator, value = line.partition(b":")
                        if not separator:
                            raise ValueError("malformed upgrade header")
                        key = key.strip().lower()
                        if not key or key in headers:
                            raise ValueError("duplicate or empty upgrade header")
                        headers[key] = value.strip()
                    if headers.get(b"host") != b"localhost:18445":
                        raise ValueError("original demo Host was not preserved")
                    if headers.get(b"upgrade", b"").lower() != b"websocket":
                        raise ValueError("missing WebSocket upgrade")
                    if b"upgrade" not in {v.strip().lower() for v in headers.get(b"connection", b"").split(b",")}:
                        raise ValueError("missing Connection upgrade token")
                    if headers.get(b"sec-websocket-version") != b"13":
                        raise ValueError("unexpected WebSocket version")
                    key = headers.get(b"sec-websocket-key", b"")
                    if len(base64.b64decode(key, validate=True)) != 16:
                        raise ValueError("invalid WebSocket key")
                    accept = base64.b64encode(hashlib.sha1(key + GUID).digest())
                    connection.sendall(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: " + accept + b"\r\n\r\n")
                    record("upgrade", method="GET", path="/echo", host="localhost:18445", status=101)

                    def frame():
                        first, second = receive(2)
                        if not first & 128 or first & 112 or not second & 128:
                            raise ValueError("demo expects final unextended masked client frames")
                        opcode, length = first & 15, second & 127
                        if length == 126:
                            length = struct.unpack("!H", receive(2))[0]
                            if length < 126:
                                raise ValueError("noncanonical short extended length")
                        elif length == 127:
                            length = struct.unpack("!Q", receive(8))[0]
                            if length <= 65535:
                                raise ValueError("noncanonical long extended length")
                        if length > MAX_FRAME or (opcode >= 8 and length > 125):
                            raise ValueError("frame exceeds demo bound")
                        mask, raw = receive(4), receive(length)
                        return opcode, bytes(value ^ mask[index & 3] for index, value in enumerate(raw))

                    def send(opcode, payload):
                        length = len(payload)
                        if length <= 125:
                            header = bytes([128 | opcode, length])
                        elif length <= 65535:
                            header = bytes([128 | opcode, 126]) + struct.pack("!H", length)
                        else:
                            raise ValueError("outbound frame exceeds demo bound")
                        connection.sendall(header + payload)

                    def next_data():
                        for _ in range(16):
                            opcode, payload = frame()
                            if opcode == 9:
                                send(10, payload)
                            elif opcode != 10:
                                return opcode, payload
                        raise ValueError("too many control frames")

                    opcode, payload = next_data()
                    if opcode != 1 or payload != MARKER:
                        # Never retain/log/echo an unknown request body or header.
                        raise ValueError("unexpected fixed public demo text")
                    record("message", opcode=1, masked=True, text=payload.decode("ascii"), bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
                    send(1, payload)
                    record("echo_sent", bytes=len(payload), sha256=hashlib.sha256(payload).hexdigest())
                    opcode, payload = next_data()
                    expected_close = struct.pack("!H", 1000) + b"demo complete"
                    if opcode != 8 or payload != expected_close:
                        raise ValueError("expected actual close1000/demo complete")
                    record("close_received", code=1000, reason="demo complete", masked=True)
                    send(8, payload)
                    record("close_sent", code=1000)
                    if pending or connection.recv(1):
                        raise ValueError("unexpected bytes after close")
                    record("eof", normal=True)
                    print("PUBLIC_SOCKET_DEMO_COMPLETE", flush=True)
            except Exception as exc:
                # Exception classes are safe diagnostics; arbitrary input is never logged.
                record("error", category=type(exc).__name__)
                raise


if __name__ == "__main__":
    main()
