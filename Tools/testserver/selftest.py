#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Checks Tools/testserver/server.py with a standard-library client before UE tests rely on it.

    python3 -I Tools/testserver/selftest.py [-v]
"""

import base64
import hashlib
import http.client
import json
import os
import shutil
import signal
import socket
import ssl
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


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = server.make_server("127.0.0.1", 0)
        cls.port = cls.httpd.server_address[1]
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def call(self, method, path, body=None, headers=None, chunked=False):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers=headers or {}, encode_chunked=chunked)
            response = connection.getresponse()
            return response.status, response.msg, response.read()
        finally:
            connection.close()

    def call_json(self, method, path, body=None, headers=None, chunked=False):
        status, headers_in, data = self.call(method, path, body, headers, chunked)
        return status, headers_in, (json.loads(data) if data else None)

    def test_echo_json_query_headers(self):
        status, _, echo = self.call_json("POST", "/echo/x?a=1&a=2&b=", b'{"k":"v"}', {"Content-Type": "application/json", "X-Test": "yes"})
        self.assertEqual(status, 200)
        self.assertEqual(echo["method"], "POST")
        self.assertEqual(echo["path"], "/echo/x")
        self.assertEqual(echo["query"], {"a": ["1", "2"], "b": [""]})
        self.assertEqual(echo["headers"]["x-test"], "yes")
        self.assertEqual(echo["body"], '{"k":"v"}')
        self.assertEqual(echo["body_sha256"], hashlib.sha256(b'{"k":"v"}').hexdigest())

    def test_raw_request_line(self):
        # http.client sends the target as given: what the server reports is what was on the wire
        _, _, raw = self.call_json("GET", "/raw/x?p=a|b%7Cc&s=a%20b")
        self.assertEqual(raw["request_line"], "GET /raw/x?p=a|b%7Cc&s=a%20b HTTP/1.1")
        self.assertEqual(raw["target"], "/raw/x?p=a|b%7Cc&s=a%20b")
        self.assertEqual(raw["query"], {"p": ["a|b|c"], "s": ["a b"]})

    def test_echo_form(self):
        _, _, echo = self.call_json("POST", "/echo", b"name=Neo+Anderson&city=Zion+%26+co", {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(echo["form"], {"name": ["Neo Anderson"], "city": ["Zion & co"]})

    def test_echo_multipart_binary_safe(self):
        binary = b"\r\n--\x00\xff\r\n\r\nend"
        body = (b"--B\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\nhi\r\n"
                b"--B\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"b.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n"
                + binary + b"\r\n--B--\r\n")
        _, _, echo = self.call_json("POST", "/echo", body, {"Content-Type": "multipart/form-data; boundary=B"})
        parts = echo["parts"]
        self.assertEqual([part["name"] for part in parts], ["note", "blob"])
        self.assertEqual(parts[0]["text"], "hi")
        self.assertEqual(parts[1]["filename"], "b.bin")
        self.assertEqual(parts[1]["length"], len(binary))
        self.assertEqual(parts[1]["sha256"], hashlib.sha256(binary).hexdigest())

    def test_echo_chunked_request_body(self):
        _, _, echo = self.call_json("PUT", "/echo", iter([b"abc", b"defg"]), {"Content-Type": "text/plain"}, chunked=True)
        self.assertEqual(echo["body"], "abcdefg")

    def test_echo_binary_body(self):
        _, _, echo = self.call_json("POST", "/echo", b"\x00\x01\x02\xff", {"Content-Type": "application/octet-stream"})
        self.assertIsNone(echo["body"])
        self.assertEqual(echo["body_base64"], base64.b64encode(b"\x00\x01\x02\xff").decode("ascii"))

    def test_status(self):
        status, _, value = self.call_json("GET", "/status/404")
        self.assertEqual((status, value), (404, {"status": 404}))
        status, _, data = self.call("GET", "/status/204")
        self.assertEqual((status, data), (204, b""))
        status, headers, _ = self.call("GET", "/status/302?location=/echo")
        self.assertEqual((status, headers["Location"]), (302, "/echo"))
        status, headers, data = self.call("GET", "/status/500?body=oops")
        self.assertEqual((status, headers["Content-Type"], data), (500, "text/plain; charset=utf-8", b"oops"))
        status, headers, data = self.call("HEAD", "/echo")
        self.assertEqual(status, 200)
        self.assertEqual(data, b"")
        self.assertGreater(int(headers["Content-Length"]), 0)

    def test_unknown_route(self):
        status, _, value = self.call_json("GET", "/nope")
        self.assertEqual((status, value["error"]), (404, "no route"))

    def test_delay_counts_concurrency(self):
        key = uuid.uuid4().hex
        threads = [threading.Thread(target=self.call, args=("GET", "/delay/300?key=" + key)) for _ in range(3)]
        started = time.monotonic()
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertLess(time.monotonic() - started, 0.9)
        _, _, stats = self.call_json("GET", "/stats?key=%s&reset=1" % key)
        self.assertEqual((stats["count"], stats["max_in_flight"], stats["in_flight"]), (3, 3, 0))
        _, _, stats = self.call_json("GET", "/stats?key=" + key)
        self.assertEqual(stats["count"], 0)

    def test_drip(self):
        started = time.monotonic()
        status, headers, data = self.call("GET", "/drip?bytes=10&chunks=3&interval_ms=200")
        self.assertEqual((status, data, headers["Content-Length"]), (200, b"x" * 10, "10"))
        self.assertGreaterEqual(time.monotonic() - started, 0.4)

    def test_slowread(self):
        started = time.monotonic()
        status, _, value = self.call_json("POST", "/slowread?chunk=100&interval_ms=50", b"x" * 300, {"Content-Type": "application/octet-stream"})
        self.assertEqual((status, value), (200, {"received": 300}))
        self.assertGreaterEqual(time.monotonic() - started, 0.1)

    def test_large_is_deterministic(self):
        _, headers, first = self.call("GET", "/large?bytes=100000&seed=5")
        _, _, second = self.call("GET", "/large?bytes=100000&seed=5")
        self.assertEqual(len(first), 100000)
        self.assertEqual(first, second)
        self.assertEqual(headers["X-Sha256"], hashlib.sha256(first).hexdigest())

    def test_large_json(self):
        _, headers, items = self.call_json("GET", "/large/json?bytes=50000")
        self.assertEqual(len(items), int(headers["X-Item-Count"]))
        self.assertGreaterEqual(len(json.dumps(items, separators=(",", ":"))), 50000)

    def test_chunked_response(self):
        status, headers, items = self.call_json("GET", "/chunked?chunks=5&size=3")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Transfer-Encoding"], "chunked")
        self.assertEqual(items, ["aaa", "bbb", "ccc", "ddd", "eee"])

    def test_redirect_and_headers(self):
        status, headers, data = self.call("GET", "/redirect?to=/echo/after")
        self.assertEqual((status, headers["Location"]), (302, "/echo/after"))
        # Review I-2: the 302 carries a body of its own, which a client that follows it must not deliver
        self.assertEqual((headers["Content-Type"], json.loads(data)), ("application/json", {"redirect": "body-must-not-leak"}))
        status, headers, _ = self.call("GET", "/headers/set?Set-Cookie=sid%3Dx&X-A=1")
        self.assertEqual((status, headers["Set-Cookie"], headers["X-A"]), (200, "sid=x", "1"))

    def test_profile(self):
        _, _, profile = self.call_json("GET", "/json/profile")
        self.assertEqual(profile["id"], 9007199254740993)
        self.assertNotIn("score", profile)

    def test_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "bench_tiny.json"), "wb") as stream:
                stream.write(b'{"ok":true}')
            server.FIXTURES_DIR = directory
            try:
                status, headers, data = self.call("GET", "/fixture/bench_tiny.json")
                self.assertEqual((status, headers["Content-Type"], data), (200, "application/json", b'{"ok":true}'))
                self.assertEqual(self.call("GET", "/fixture/missing.json")[0], 404)
                self.assertEqual(self.call("GET", "/fixture/..")[0], 404)
            finally:
                server.FIXTURES_DIR = None

    def test_fixture_refuses_links_and_escapes(self):
        # Review M3: only a plain file directly in --fixtures-dir; never through a symlink, "..", an absolute name or a separator
        with tempfile.TemporaryDirectory() as outside, tempfile.TemporaryDirectory() as directory:
            secret = os.path.join(outside, "secret.json")
            with open(secret, "wb") as stream:
                stream.write(b'{"secret":true}')
            with open(os.path.join(directory, "plain.json"), "wb") as stream:
                stream.write(b"{}")
            os.mkdir(os.path.join(directory, "sub"))
            try:
                os.symlink(secret, os.path.join(directory, "link.json"))
            except (OSError, NotImplementedError):
                self.skipTest("no symlinks here")
            server.FIXTURES_DIR = directory
            try:
                self.assertEqual(self.call("GET", "/fixture/link.json")[0], 404)
                self.assertEqual(self.call("GET", "/fixture/sub")[0], 404)
                self.assertEqual(self.call("GET", "/fixture/plain.json")[0], 200)
                for name in ("link.json", "..", ".", "", secret, "sub/../plain.json", "../" + os.path.basename(outside)):
                    self.assertIsNone(server.fixture_path(name), name)
                self.assertEqual(server.fixture_path("plain.json"), os.path.join(os.path.realpath(directory), "plain.json"))
            finally:
                server.FIXTURES_DIR = None

    def raw(self, data):
        """Sends bytes as they are and reads until the server closes"""
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as connection:
            connection.sendall(data)
            received = b""
            while True:
                chunk = connection.recv(65536)
                if not chunk:
                    return received
                received += chunk

    def test_bounds(self):
        # Review M1: a body over MAX_BODY is refused from its declared size alone, before any of it is read
        over = server.MAX_BODY + 1
        answer = self.raw(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % over)
        self.assertTrue(answer.startswith(b"HTTP/1.1 413 "), answer[:80])
        answer = self.raw(b"POST /slowread HTTP/1.1\r\nHost: x\r\nContent-Length: %d\r\n\r\n" % over)
        self.assertTrue(answer.startswith(b"HTTP/1.1 413 "), answer[:80])
        answer = self.raw(b"POST /echo HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n%x\r\n" % over)
        self.assertTrue(answer.startswith(b"HTTP/1.1 413 "), answer[:80])
        answer = self.raw(b"POST /echo HTTP/1.1\r\nHost: x\r\nContent-Length: nope\r\n\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 400 "), answer[:80])
        status, _, value = self.call_json("GET", "/echo?" + "a" * server.MAX_TARGET)
        self.assertEqual((status, value["max"]), (414, server.MAX_TARGET))
        for path in ("/large?bytes=%d" % over, "/large?bytes=x", "/large/json?bytes=-1", "/drip?chunks=0", "/delay/%d" % (server.MAX_MS + 1),
                     "/chunked?chunks=4096&size=65536", "/flaky?n=99999999"):
            status, _, value = self.call_json("GET", path)
            self.assertEqual(status, 400, path)
            self.assertIn(value["error"], ("out of range", "not an integer"), path)
        # A refusal ends its connection; the server goes on serving
        self.assertEqual(self.call("GET", "/echo")[0], 200)

    def test_stats_reset_while_delay_in_flight(self):
        # Review M6: a reset while a /delay with that key sleeps must not cost that request its response
        key = uuid.uuid4().hex
        result = {}

        def delayed():
            try:
                result["status"] = self.call("GET", "/delay/400?key=" + key)[0]
            except (http.client.HTTPException, OSError) as error:
                result["error"] = repr(error)

        thread = threading.Thread(target=delayed)
        thread.start()
        deadline = time.monotonic() + 5
        while self.call_json("GET", "/stats?key=" + key)[2]["in_flight"] != 1 and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(self.call_json("GET", "/stats?reset=1&key=" + key)[2]["in_flight"], 1)
        thread.join()
        self.assertEqual(result, {"status": 200})
        self.assertEqual(self.call_json("GET", "/stats?key=" + key)[2], {"in_flight": 0, "max_in_flight": 0, "count": 0})

    def test_retry_after(self):
        key = uuid.uuid4().hex
        status, headers, _ = self.call("GET", "/retry-after/2?seconds=3&key=" + key)
        self.assertEqual((status, headers["Retry-After"]), (503, "3"))
        self.assertEqual(self.call("GET", "/retry-after/2?key=" + key)[0], 503)
        self.assertEqual(self.call_json("GET", "/retry-after/2?key=" + key)[2], {"attempt": 3})

    def test_etag(self):
        status, headers, _ = self.call("GET", "/etag?tag=v7")
        self.assertEqual((status, headers["ETag"]), (200, '"v7"'))
        status, _, data = self.call("GET", "/etag?tag=v7", headers={"If-None-Match": '"v7"'})
        self.assertEqual((status, data), (304, b""))

    def test_login_refresh_protected(self):
        self.assertEqual(self.call("POST", "/login", b'{"user":"neo","password":"nope"}')[0], 401)
        _, _, tokens = self.call_json("POST", "/login", b'{"user":"neo","password":"trinity"}')
        bearer = {"Authorization": "Bearer " + tokens["accessToken"]}
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 200)
        _, _, renewed = self.call_json("POST", "/refresh", json.dumps({"refreshToken": tokens["refreshToken"]}).encode())
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 401)
        self.assertEqual(self.call("GET", "/protected", headers={"Authorization": "Bearer " + renewed["accessToken"]})[0], 200)
        self.assertEqual(self.call("POST", "/refresh", json.dumps({"refreshToken": tokens["refreshToken"]}).encode())[0], 401)

    def test_flaky(self):
        key = uuid.uuid4().hex
        with self.assertRaises((http.client.RemoteDisconnected, ConnectionError)):
            self.call("GET", "/flaky?n=1&key=" + key)
        self.assertEqual(self.call_json("GET", "/flaky?n=1&key=" + key)[2], {"attempt": 2})

    def test_envelope(self):
        self.assertTrue(self.call_json("GET", "/envelope/ok")[2]["ok"])
        self.assertEqual(self.call_json("GET", "/envelope/err")[2]["code"], "E42")

    def test_expire_keeps_the_refresh_token(self):
        _, _, tokens = self.call_json("POST", "/login", b'{"user":"neo","password":"trinity"}')
        bearer = {"Authorization": "Bearer " + tokens["accessToken"]}
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 200)
        self.assertEqual(self.call("POST", "/expire")[0], 200)
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 401)
        status, _, renewed = self.call_json("POST", "/refresh", json.dumps({"refreshToken": tokens["refreshToken"]}).encode())
        self.assertEqual(status, 200)
        self.assertEqual(self.call("GET", "/protected", headers={"Authorization": "Bearer " + renewed["accessToken"]})[0], 200)

    def test_envelope_gone(self):
        status, _, body = self.call_json("GET", "/envelope/gone")
        self.assertEqual((status, body["code"], body["message"]), (404, "E404", "not here"))

    def test_upgrade_plugin_contract(self):
        # WebSocket spec 16.3: the handler answers 101, takes over the connection and sets close_connection
        def handler(request):
            request.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: x-test\r\nConnection: Upgrade\r\n\r\n")
            line = request.rfile.readline()
            request.wfile.write(b"echo:" + line)
            request.close_connection = True

        server.register_upgrade("x-test", handler)
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as raw:
            raw.sendall(b"GET /anything HTTP/1.1\r\nHost: x\r\nUpgrade: x-test\r\nConnection: Upgrade\r\n\r\nping\n")
            received = b""
            while not received.endswith(b"echo:ping\n"):
                chunk = raw.recv(4096)
                if not chunk:
                    break
                received += chunk
        self.assertTrue(received.startswith(b"HTTP/1.1 101 "))
        self.assertTrue(received.endswith(b"echo:ping\n"))

    def test_plugin_route(self):
        # WebSocket spec 16.3: a plug-in's HTTP route (ws.py serves /ws/journal this way) is matched like a built-in one
        server.register_route(("GET",), r"/plugin/hello", lambda h, match: h.send_json(200, {"hello": h.query.get("who", [""])[0]}))
        status, _, value = self.call_json("GET", "/plugin/hello?who=ws")
        self.assertEqual((status, value), (200, {"hello": "ws"}))
        self.assertEqual(self.call("POST", "/plugin/hello")[0], 404)

    def test_tls_ca_route(self):
        status, headers, data = self.call("GET", "/tls/ca.pem")
        with open(os.path.join(server.CERTS_DIR, "test-ca.pem"), "rb") as stream:
            self.assertEqual((status, headers["Content-Type"], data), (200, "application/x-pem-file", stream.read()))


class TlsTest(unittest.TestCase):
    """The TLS listener (WebSocket spec 16.3): every route over stdlib ssl with the committed test certificate"""

    @classmethod
    def setUpClass(cls):
        cls.httpd = server.make_tls_server("127.0.0.1", 0)
        cls.port = cls.httpd.server_address[1]
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    @staticmethod
    def trusted():
        return ssl.create_default_context(cafile=os.path.join(server.CERTS_DIR, "test-ca.pem"))

    def get(self, context, server_hostname, path="/echo"):
        """One HTTPS GET over a socket to 127.0.0.1 whose certificate is checked for server_hostname"""
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as plain:
            with context.wrap_socket(plain, server_hostname=server_hostname) as tls:
                certificate = tls.getpeercert()
                tls.sendall(("GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n" % (path, server_hostname)).encode("ascii"))
                received = b""
                while True:
                    chunk = tls.recv(65536)
                    if not chunk:
                        return received, certificate
                    received += chunk

    def test_verified_by_name_and_by_ip(self):
        for name in ("localhost", "127.0.0.1"):
            received, _ = self.get(self.trusted(), name)
            self.assertTrue(received.startswith(b"HTTP/1.1 200 "), (name, received[:80]))

    def test_untrusted_is_refused(self):
        with self.assertRaises(ssl.SSLCertVerificationError):
            self.get(ssl.create_default_context(), "localhost")

    def test_certificate_files(self):
        with open(os.path.join(server.CERTS_DIR, "test-server.pem"), encoding="ascii") as stream:
            chain = stream.read()
        # The leaf and then the CA: a client that allows self-signed chains sees SELF_SIGNED_CERT_IN_CHAIN
        self.assertEqual(chain.count("-----BEGIN CERTIFICATE-----"), 2)
        with open(os.path.join(server.CERTS_DIR, "test-ca.pem"), encoding="ascii") as stream:
            self.assertTrue(chain.rstrip().endswith(stream.read().strip()))
        with open(os.path.join(server.CERTS_DIR, "test-server.key"), encoding="ascii") as stream:
            self.assertNotIn("ENCRYPTED", stream.read())
        _, certificate = self.get(self.trusted(), "localhost")
        self.assertGreater(ssl.cert_time_to_seconds(certificate["notAfter"]) - time.time(), 10 * 365 * 86400)
        self.assertIn(("DNS", "localhost"), certificate["subjectAltName"])
        self.assertIn(("IP Address", "127.0.0.1"), certificate["subjectAltName"])

    def test_a_stalled_or_plain_client_does_not_block_others(self):
        # Review focus: the handshake runs on the connection's own thread, bounded by TLS_HANDSHAKE_TIMEOUT
        stalled = socket.create_connection(("127.0.0.1", self.port), timeout=10)
        try:
            self.assertTrue(self.get(self.trusted(), "localhost")[0].startswith(b"HTTP/1.1 200 "))
            with socket.create_connection(("127.0.0.1", self.port), timeout=10) as plain:
                plain.sendall(b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
                plain.recv(65536)
            self.assertTrue(self.get(self.trusted(), "localhost")[0].startswith(b"HTTP/1.1 200 "))
        finally:
            stalled.close()


class LifecycleTest(unittest.TestCase):
    def test_ready_file_and_parent_watch(self):
        # The runner's contract (M3 plan decision 14): a ready file with the URL; exit when the parent -- the process
        # that started the server -- goes. A stand-in parent starts the server and sleeps; the test kills it.
        with tempfile.TemporaryDirectory() as directory:
            ready = os.path.join(directory, "url")
            child_pid_file = os.path.join(directory, "pid")
            starter = ("import os, subprocess, sys, time\n"
                       "child = subprocess.Popen([sys.executable, '-I', sys.argv[1], '--port', '0', '--ready-file', sys.argv[2],"
                       " '--parent-pid', str(os.getpid())], stdout=subprocess.DEVNULL)\n"
                       "open(sys.argv[3], 'w').write(str(child.pid))\n"
                       "time.sleep(60)\n")
            # -I for the stand-in too: with -c, sys.path[0] is the current directory (review M2)
            parent = subprocess.Popen([sys.executable, "-I", "-c", starter, os.path.join(HERE, "server.py"), ready, child_pid_file])
            child_pid = None
            gone = False
            try:
                deadline = time.monotonic() + 10
                while not os.path.exists(ready) and time.monotonic() < deadline:
                    time.sleep(0.05)
                with open(ready, encoding="utf-8") as stream:
                    url = urllib.parse.urlsplit(stream.read().strip())
                connection = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
                connection.request("GET", "/echo")
                self.assertEqual(connection.getresponse().status, 200)
                connection.close()
                with open(child_pid_file, encoding="utf-8") as stream:
                    child_pid = int(stream.read())
                parent.kill()
                parent.wait()
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not gone:
                    try:
                        os.kill(child_pid, 0)
                        time.sleep(0.1)
                    except ProcessLookupError:
                        gone = True
                self.assertTrue(gone, "the server outlived its parent")
                self.assertFalse(os.path.exists(ready), "the ready file outlived the server")
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()
                # A failed check must not leave a server behind: nothing else would stop it, and it holds this run's
                # stderr open (a piped caller would wait for it forever). Only one known to be alive, by its PID.
                if child_pid is None and os.path.exists(child_pid_file):
                    with open(child_pid_file, encoding="utf-8") as stream:
                        child_pid = int(stream.read() or 0) or None
                if child_pid is not None and not gone:
                    try:
                        os.kill(child_pid, getattr(signal, "SIGKILL", signal.SIGTERM))
                    except OSError:
                        pass

    def test_parent_pid_must_be_the_parent(self):
        # Review M-13: a --parent-pid that is not the starting parent is refused at once, not obeyed half a second later
        started = time.monotonic()
        result = subprocess.run([sys.executable, "-I", os.path.join(HERE, "server.py"), "--port", "0", "--parent-pid", "1"],
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=10)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"--parent-pid 1 is not the process that started this one", result.stderr)
        self.assertLess(time.monotonic() - started, 5)


    def test_tls_listener_in_the_ready_file_and_gone_with_the_parent(self):
        # Review focus: the runner killed with both listeners up -- both go, and the ready file with them
        with tempfile.TemporaryDirectory() as directory:
            ready = os.path.join(directory, "url")
            child_pid_file = os.path.join(directory, "pid")
            starter = ("import os, subprocess, sys, time\n"
                       "child = subprocess.Popen([sys.executable, '-I', sys.argv[1], '--port', '0', '--tls-port', '0',"
                       " '--ready-file', sys.argv[2], '--parent-pid', str(os.getpid())], stdout=subprocess.DEVNULL)\n"
                       "open(sys.argv[3], 'w').write(str(child.pid))\n"
                       "time.sleep(60)\n")
            parent = subprocess.Popen([sys.executable, "-I", "-c", starter, os.path.join(HERE, "server.py"), ready, child_pid_file])
            child_pid = None
            gone = False
            try:
                deadline = time.monotonic() + 10
                while not os.path.exists(ready) and time.monotonic() < deadline:
                    time.sleep(0.05)
                with open(ready, encoding="utf-8") as stream:
                    plain_url, tls_url = [urllib.parse.urlsplit(line) for line in stream.read().split()]
                self.assertEqual((plain_url.scheme, tls_url.scheme), ("http", "https"))
                context = ssl.create_default_context(cafile=os.path.join(server.CERTS_DIR, "test-ca.pem"))
                connection = http.client.HTTPSConnection("127.0.0.1", tls_url.port, timeout=10, context=context)
                connection.request("GET", "/echo")
                self.assertEqual(connection.getresponse().status, 200)
                connection.close()
                with open(child_pid_file, encoding="utf-8") as stream:
                    child_pid = int(stream.read())
                parent.kill()
                parent.wait()
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline and not gone:
                    try:
                        os.kill(child_pid, 0)
                        time.sleep(0.1)
                    except ProcessLookupError:
                        gone = True
                self.assertTrue(gone, "the server outlived its parent")
                self.assertFalse(os.path.exists(ready), "the ready file outlived the server")
                with self.assertRaises(ConnectionRefusedError):
                    socket.create_connection(("127.0.0.1", tls_url.port), timeout=5).close()
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()
                if child_pid is None and os.path.exists(child_pid_file):
                    with open(child_pid_file, encoding="utf-8") as stream:
                        child_pid = int(stream.read() or 0) or None
                if child_pid is not None and not gone:
                    try:
                        os.kill(child_pid, getattr(signal, "SIGKILL", signal.SIGTERM))
                    except OSError:
                        pass

    def test_missing_certificate_names_the_file(self):
        # Review M-11: the runner always passes --tls-port, so a missing certificate must say which file, not just fail
        with tempfile.TemporaryDirectory() as directory:
            shutil.copy(os.path.join(HERE, "server.py"), directory)
            result = subprocess.run([sys.executable, "-I", os.path.join(directory, "server.py"), "--port", "0", "--tls-port", "0"],
                                    stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"cannot start the TLS listener", result.stderr)
        self.assertIn(b"test-server", result.stderr)

    def test_tls_port_in_use_says_binding_not_a_directory(self):
        # Review minor: an error without a file name must not be reported against the certs directory
        with socket.socket() as taken:
            taken.bind(("127.0.0.1", 0))
            taken.listen(1)
            result = subprocess.run([sys.executable, "-I", os.path.join(HERE, "server.py"), "--port", "0", "--tls-port",
                                     str(taken.getsockname()[1])], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=20)
        self.assertEqual(result.returncode, 2)
        self.assertIn(b"cannot start the TLS listener: binding 127.0.0.1:", result.stderr)
        self.assertNotIn(b"certs", result.stderr)

    def test_verbose_logs_swallowed_tls_errors(self):
        # Review minor: a plain-HTTP client on the TLS port is a quiet scenario, but VAREST_TESTSERVER_VERBOSE=1 says why
        for verbose in ("1", "0"):
            environment = dict(os.environ, VAREST_TESTSERVER_VERBOSE=verbose)
            with tempfile.TemporaryDirectory() as directory:
                ready = os.path.join(directory, "url")
                process = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "server.py"), "--port", "0", "--tls-port", "0",
                                            "--ready-file", ready], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=environment)
                try:
                    deadline = time.monotonic() + 10
                    while not os.path.exists(ready) and time.monotonic() < deadline:
                        time.sleep(0.05)
                    with open(ready, encoding="utf-8") as stream:
                        port = urllib.parse.urlsplit(stream.read().split()[1]).port
                    with socket.create_connection(("127.0.0.1", port), timeout=5) as plain:
                        plain.sendall(b"GET /echo HTTP/1.1\r\nHost: x\r\n\r\n")
                        try:
                            plain.recv(65536)
                        except OSError:
                            pass
                    time.sleep(0.5)
                finally:
                    process.kill()
                    errors = process.stderr.read()
                    process.wait()
                    process.stderr.close()
            self.assertEqual(b"TLS error from" in errors, verbose == "1", errors)


class PluginTest(unittest.TestCase):
    def test_script_mode_plugin_shares_the_module(self):
        # WebSocket spec 16.3 (review M4): run as a script, server.py is __main__, and a plug-in's "import server" must
        # get that module -- one STATE, one upgrade registry -- not a second copy. A stub ws.py next to a copy checks it.
        stub = ("import json, sys\n"
                "import server\n"
                "def handler(request):\n"
                "    body = json.dumps({'same_module': server is sys.modules['__main__'],\n"
                "                       'serial': server.STATE['tokens']['serial']}).encode()\n"
                "    request.wfile.write(b'HTTP/1.1 101 Switching Protocols\\r\\nUpgrade: x-stub\\r\\nConnection: Upgrade\\r\\n\\r\\n' + body)\n"
                "    request.close_connection = True\n"
                "server.register_upgrade('x-stub', handler)\n")
        with tempfile.TemporaryDirectory() as directory:
            shutil.copy(os.path.join(HERE, "server.py"), directory)
            with open(os.path.join(directory, "ws.py"), "w", encoding="utf-8") as stream:
                stream.write(stub)
            ready = os.path.join(directory, "url")
            child = subprocess.Popen([sys.executable, "-I", os.path.join(directory, "server.py"), "--port", "0", "--ready-file", ready],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                while not os.path.exists(ready) and time.monotonic() < deadline:
                    time.sleep(0.05)
                with open(ready, encoding="utf-8") as stream:
                    url = urllib.parse.urlsplit(stream.read().strip())
                connection = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
                connection.request("POST", "/login", b'{"user":"neo","password":"trinity"}')
                self.assertEqual(connection.getresponse().status, 200)
                connection.close()
                with socket.create_connection((url.hostname, url.port), timeout=10) as raw:
                    raw.sendall(b"GET /anything HTTP/1.1\r\nHost: x\r\nUpgrade: x-stub\r\nConnection: Upgrade\r\n\r\n")
                    received = b""
                    while True:
                        chunk = raw.recv(4096)
                        if not chunk:
                            break
                        received += chunk
                head, _, body = received.partition(b"\r\n\r\n")
                self.assertTrue(head.startswith(b"HTTP/1.1 101 "), received[:80])
                # The plug-in sees the token /login issued through the same process's routes
                self.assertEqual(json.loads(body), {"same_module": True, "serial": 1})
            finally:
                child.kill()
                child.wait()
            self.assertFalse(os.path.exists(os.path.join(directory, "__pycache__")))


if __name__ == "__main__":
    unittest.main()
