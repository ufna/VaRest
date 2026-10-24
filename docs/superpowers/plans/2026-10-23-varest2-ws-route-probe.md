# VaRest 2 WebSocket Immutable Proxy Route Probe Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Measure whether bounded immutable explicit LWS vhosts can implement initial proxy-address fallback without synchronous service-thread DNS, route bypass, TLS identity corruption or SSL_CTX ownership errors.

**Architecture:** Extend only the existing raw FRig with owned explicit-route vhosts selected by `lws_client_connect_info::vhost`. A separate loopback relay gates real server TLS bytes after ClientHello; a parent controller runs every hazardous scenario in its own bounded editor child and retains partial evidence on crash. A validator reports measurements and prerequisite failures separately; root decides production direction after review.

**Tech Stack:** UE 5.8.1 Automation, C++20, pinned libwebsockets 3.0.0 and OpenSSL 1.1.1t through the existing private probe wrapper, Python standard library, IPv4/IPv6 loopback only.

**Spec:** `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` §§1.2,2.1,2.2,16.4; `.superpowers/sdd/release-2.0-program.md`; `.superpowers/sdd/ws-w1-plan-rulings.md` Q5/Q7; B1 passport `docs/passport/2026-10-23-lws30-proxy-probe.md`; root's 2026-10-23 VaRest-81s.12 prerequisite dispatch and weak-symbol instrumentation exception.

**Baseline:** `e8fbf1e4868c43cd5cb81186e352263037bfd4d7`, both HEAD/develop verified. B1 is already accepted C0/I0/M0. No production transport/classifier, DNS worker, engine, library, B1 fixture or B1 passport change belongs to this probe. The remaining-W1b plan stays unfrozen until these measurements are adjudicated.

## Global Constraints

- Engine: UE 5.8 only. Build host: `/w/Unreal/VaRestHost` (plugin symlinked). Build + test: `Tools/run_tests.sh [Filter]` (bare filters are anchored, so `VaRest.WebSocket` never runs `Probe.*` or `Bench.*`); the runner starts `Tools/testserver/server.py` and passes `-VaRestTestServer=<url>` and `-VaRestTestServerTls=<url>`.
- `VaRestWebSocket` dependencies: public `Core`, `CoreUObject`, `VaRestJson`, `VaRestHttp`; private `HTTP`, `SSL`; `AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib")`. The engine module `WebSockets` is **never** a dependency of `VaRestWebSocket` (spec §1.1); `VaRestTests` keeps its include-path-only use of it. No new third-party code.
- **W1 does not depend on M4a code.** Use only what M3 shipped: `VaRest::Http::IsValidHeaderName`, `VaRest::Http::PercentEncode`, `FVaRestRedactor`, `FVaRestClient` (tests), `VaRest::Http::Testing::{PumpUntil, GetTestServerUrl, GetTestServerTlsUrl, FLogCapture, MakeOutcome, Observe}`, `VaRest::Http::FSha256`, `VaRest::Json::{IsValidUtf8, AppendRepairedUtf8, Utf8Length, WriteUtf8}`. Never `VaRestBackoff.h`, `VaRestRetry.h`, `VaRestAuth*.h`, `VaRestEnvelope.h`, `VaRestHttpStats.h`, M4a's LLM tags, `FTraceRecorder`.
- **v1 paths are frozen.** Nothing under `Source/VaRest/` or `Source/VaRestEditor/` changes.
- **Frozen names** (CLAUDE.md): `UVaRestHttpSettings` property names, `FVaRestError`'s fields, `EVaRestErrorKind`, `EVaRestPriority`, `EVaRestLogLevel`, `EVaRestQueryStyle` — W1a touches none of them. Decision 21 lists W1a's names that freeze with W5.
- **Threading rule (core spec §1, verbatim):** "*Control plane on the game thread, data plane off it.* Stage decisions, callbacks, and BP events run on the game thread. Parsing, decoding, cache disk IO, and offline-queue persistence run on `UE::Tasks` background tasks." WebSocket spec §2.1 and §4: every LWS call, assembly, UTF-8 validation and the connect and close deadlines run on the VaRest service thread; commands go in through an MPSC queue followed by `lws_cancel_service`, the only LWS call another thread makes (T13, `libwebsockets.h:3704-3716`); `lws_create_context` and `lws_context_destroy` run on the game thread with the service thread stopped (`LwsWebSocketsManager.cpp:231`, `:298-309`). The transport API is game-thread only except `PopEvents`/`Consumed` (the consumer's one thread) and the getters (any thread). Every function's doc comment names its thread.
- Public types: `FVaRest*`, `EVaRest*`, `IVaRest*`, `TVaRest*`, `UVaRest*`. Internals: namespace `VaRest::WebSocket` (pure code) and `VaRest::WebSocket::Lws` (code that touches libwebsockets); file-local helpers in `namespace VaRest::WebSocket::<FileStem>Private` (or `...::Lws::<FileStem>Private`); tests in `namespace VaRest::WsTest` (VaRestTests) or the file's `...Private` namespace. Never reuse a v1 header name (`VaRest.h`, `VaRestDefines.h`, `VaRestJsonObject.h`, `VaRestJsonValue.h`, `VaRestLibrary.h`, `VaRestRequestJSON.h`, `VaRestSettings.h`, `VaRestSubsystem.h`, `VaRestTypes.h`, `VaRestJsonParser.h`) nor an engine one (`WebSocket.h`, `IWebSocket.h`, `WebSocketsModule.h`).
- **libwebsockets and OpenSSL are included only through `Source/VaRestWebSocket/Private/VaRestLwsInclude.h`** (the `UI` rename, Windows types, third-party warnings); no public header includes it. The engine's `::FEvent` (`HAL/Event.h:21`) never meets a VaRest name: VaRest's are prefixed (`FVaRestSocketEvent`).
- JSON keys are case-sensitive (`ESearchCase::CaseSensitive`, `TestEqualSensitive`); HTTP header names are not (`ESearchCase::IgnoreCase`, explicitly). `FString::operator==` ignores case: compare subprotocols and error strings with `Equals(..., ESearchCase::CaseSensitive)`.
- Unity builds: no file-scope `using namespace`, no anonymous namespaces in `.cpp` files; block-scope `using namespace` inside a function body is fine.
- MSVC-clean by construction: no local, parameter or init-capture named like a member of the enclosing class -- a data member **or a member function** (C4458 fires on both; clang's `-Wshadow`, the only check on this host, reports neither for functions, review spec I-3) -- and none named like a free function of the enclosing namespace (C4459, e.g. `VaRest::WsTest::Request`, review spec M-10). `FVaRestSocketFrame` has members `Kind`, `Storage`, `Offset`, `Length` and functions `Text`, `Binary`, `FromStorage`, `IsValid`, `GetPayload`: its parameters are `InText`, `InBytes`, `InKind`, `InStorage`, `InOffset`, `InLength`. `FConn`'s functions include `Start`, `Send`, `Close`, `Kill`, `Fail`, `Find`, `Release`, `Prepare`: never a local `Start` or `Size` in a member function (Task 8's fragment locals are `FragmentStart`/`FragmentSize`), never `Wsi` (callbacks take `InWsi`). Task 14 Step 9 lists every class's data members and functions for the sweep. Lambda init-captures never reuse an enclosing name, and a local inside a lambda body never takes the name of a local of the function around it (C4456).
- `UE_LOG` format arguments are string literals. Transport log lines name the connection (`ws#<id>`, or `FVaRestSocketConnectRequest::LogName`); they never print a URL (the label is enough), a header value or a payload.
- Tests: `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, inside `#if WITH_DEV_AUTOMATION_TESTS`, flags `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`; unit tests in `Source/VaRestWebSocket/Private/Tests/`, names `VaRest.WebSocket.*`; integration tests in `Source/VaRestTests/Private/WebSocket/`, names `VaRest.Integration.WebSocket.Transport.*` (spec §16.2). An integration test without the runner's server fails, never skips (M3 decision 14). Every WebSocket integration test starts with `ExpectLwsLines(*this)`: VaRest's libwebsockets lines are Warnings, and `bElevateLogWarningsToErrors` defaults to true (`AutomationControllerSettings.cpp:14`).
- **Test hygiene (each cost an M3 fix round):** every test drains what it opens (each connection ends — its terminal event seen — before the test returns; a test transport is shut down in `ON_SCOPE_EXIT`); a test asserting "nothing happened" first waits for the thing that did happen (the terminal event, the server's journal with `wait_ms`), never a fixed sleep; never dereference, `GetValue()` or index after a non-fatal check — write `if (!TestNotNull(...)) { return false; }` first; callbacks never capture stack locals by reference (capture `TSharedRef` state); expected-log patterns are unique per line, with `AddExpectedMessagePlain` / `AddExpectedErrorPlain` and exact counts for VaRest's own lines; a restore-the-bug step breaks the named behaviour, shows the named test failing, restores it, and the task report gives both outcomes; a test must be able to fail (no assertion the code cannot violate).
- **One rule for every test (VaRest-rti, review conc I-3):** never make a test transport while another transport of `VaRestWebSocket` has a wss connection that LWS has not destroyed yet -- an attempt that has ended still runs its handshake until LWS destroys it, so the earlier transport's `LiveConnections` must be back to 0 (shut down, or `WaitIdle`). The transport enforces it by shape too: its first connect makes no context while `TlsHandshakesInFlight()` is not 0 and ends with `Shutdown` instead. Tests run one at a time and shut their transport down before returning, which keeps it.
- **A wss leg resting on a ws-only passport row** (review spec M-5: `NoUpgradeResponse` [hs.no_upgrade_response], the drops [close.drop_*] and the refusals were measured on ws only; empty frames in `Send.Edges` and the second offered subprotocol in `Connect.Protocols` have no row at all): if a leg differs, record what LWS said with an `AddInfo`, add the string's row to Task 5's table, and report it -- never loosen the test. If "the second offered protocol, selected" fails, join the offered protocols with "," instead of ", " (the engine's join, `LwsWebSocket.cpp:791`) and report it.
- Engine claims carry `file:line` citations from `/w/Unreal/UnrealEngine/Engine/Source`; LWS behaviour cites a passport row or a `libwebsockets.h` line (`ThirdParty/libWebSockets/libwebsockets/include/Unix/x86_64-unknown-linux-gnu/libwebsockets.h`). Open every line you cite.
- Never run a `-DisableUnity` or `-CompilerArguments` UBT build (they rebuild the engine). The per-module no-PCH / non-unity check is `PCHUsage = PCHUsageMode.NoPCHs; bUseUnity = false;` set temporarily in the module's Build.cs, dry-run first with `-WriteOutdatedActions=<file>`, reverted line by line after (Task 14).
- No editor may run during a build; kill only by PID (never `pkill -f`).
- New file header: `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.` (C++, C#), `<!-- Copyright ... -->` (Markdown).
- Every commit message ends with `Co-Authored-By: Claude <Model> <noreply@anthropic.com>` naming the model that wrote the commit (for example `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`). Stage only the files your task names (`git add <paths>`): other agents work in this tree. On `index.lock`, wait and retry. Never push.

## Probe-specific constraints and authority

The preceding section is copied verbatim from W1a; these later instructions override conflicting historical examples. Build every changed C++ RED/GREEN/mutant/restored version with a fresh **normal** target `-WriteOutdatedActions` inspection, then native Build.sh, then automation **--no-build**. No engine/host Compile/Link/PCH creation; explicit receipt/metadata output only may be outside plugin. No implicit runner rebuild, global build flags, UsePrecompiled/Module shortcut, generated/cache/timestamp changes. Existing NoPCH evidence is sufficient for this raw prerequisite; do not rerun it unchanged.

Root owns Beads, review gates and commits; commit trailers are literally `Co-Authored-By: Codex <noreply@openai.com>`. Only exact listed files are staged after review and index lease. No remote operation. No owner check-in between approved tasks; send concrete measured conflicts to root. No second LWS context while any previous wss WSI is live. All mixed/held cases use one context.

**Activation sequencing:** M4b is the next complete source milestone. This baseline plan is documentation only until M4b whole-change review finishes; root refreshes runner/test interfaces against the new HEAD before activating the raw probe. No implementation interleaving.

**Narrow raw exception:** Root permits the existing W0 Linux weak named global `openssl_websocket_private_data_index` to be read, never written, around vhost/SSL initialization. This is nonpublic, platform-limited instrumentation, not a production routing/classification API. No offsets, raw addresses, symbol-absence-as-zero, or global-log inference. When absent, index motion is unobserved. Independent public accessor/certificate identity witnesses remain required. The existing private raw include wrapper `VaRestLwsProbeLws.h` remains the only probe LWS/OpenSSL include point.

A negative late-creation result is useful evidence. A crash is recorded as crash with missing final trace; it does not prove index motion without a before/after index witness. Preserve the child exit, flushed phase/SSL identity records, relay journals and acquisition error. Never turn an unsafe run into a passing test or erase its artifacts. Production may prohibit late vhost creation even if precreated-route reuse succeeds.

## Review Focus

1. A CONNECT succeeds while TLS verifies the wrong identity: Task 2 records route binding, SSL/WSI/connection association, leaf verification, Host, SNI and synthetic Authorization isolation; Task 2 rejects absent positive witnesses.
2. A route creation changes LWS's global verification index under an already allocated SSL: Task 2 records before/after indices and SSL-exists witnesses around creation and init separately, with actual ClientHello/server-flight hold; Task 2 preserves a crashed child independently.
3. The first refused peer still has live WSIs when fallback starts: Task 2 gates second physical connect on actual associated DESTROY tokens or a genuinely scrubbed NULL with no token left; Task 2's mutation refuses missing retirement.
4. Provided SSL_CTX is freed early or twice across vhosts: Task 2's OpenSSL ex-data free observer records before-vhost-destroy, after-context-destroy and caller-free, independently of echo; the hazardous case remains isolated.
5. Fixture shutdown or a fake release substitutes for a real held handshake, or IPv6 text silently routes elsewhere: Task 1's gated relay/selftests and Task 2's typed peer/gate/order checks reject these substitutions, retain unsupported IPv6 separately and never claim native platform proof.

## Public API evidence opened by the writer

All below are in `/w/Unreal/UnrealEngine/Engine/Source/ThirdParty/libWebSockets/libwebsockets/include/Unix/x86_64-unknown-linux-gnu/libwebsockets.h`:

| Lines | Contract and consequence |
| --- | --- |
| 2600–2602,2667–2674 | EXPLICIT_VHOSTS creates only context; callers create vhosts |
| 2734–2738 | Vhost proxy address/port fields; strings remain owned by rig |
| 2745–2752 | Context and vhost user ownership are distinct |
| 2765–2769 | Provided SSL_CTX remains caller-owned |
| 3093–3097 | lws_set_proxy after connect is undefined; this plan never calls it |
| 3126–3152 | Explicit vhost creation and destruction public APIs |
| 3439–3451 | ccinfo.vhost selects route/SSL_CTX; pwsi may refer to scrubbed memory on NULL |
| 3545–3570 | Explicit client TLS initialization requires DO_SSL_GLOBAL_INIT, can take provided SSL_CTX |
| 6081–6087 | lws_get_ssl public SSL accessor |

Existing W0 `VaRestLwsProbeTlsTest.cpp:490–542` reads the weak index around serial contexts; this probe applies the permitted read to vhost creation/init, not a new context. Existing `FRig::Shutdown` joins before context destruction and retains FConn storage until rig destruction. Existing B1 trace tokens associate pre-conn0 CREATE with later connection identity and retain raw error pointer presence/length/bytes.

## Files and independent deliverables

| Files | Responsibility |
| --- | --- |
| `Tools/testserver/route_proxy.py`, `route_proxy_selftest.py` | Bounded new relay, TLS hold, IPv6 listener/format observations; inherits B1 lifecycle but never edits B1 |
| `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h`, `.cpp` | Bounded owned vhost vector and route selection, raw index/identity fields |
| `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRouteTest.cpp` | One parameterized raw acquisition plus deterministic bookkeeping unit test |
| `Tools/run_lws_route_probe.py` | Owned child lifecycle and immutable scenario matrix |
| `Tools/lws_route_passport.py` | Typed independent validation, atomic passport render and mutation selftests |
| `docs/passport/2026-10-23-lws30-route-probe.md` | Generated measured rows followed by reviewed root handoff |

## Build/test protocol for every C++ version

From `/w/Unreal/VaRest`:

```bash
mkdir -p /tmp/varest-route-probe
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/tmp/varest-route-probe/actions.json
```

Preserve each fresh graph as `task-N-phase-actions.json`; inspect full sources/produced outputs/commands and nested response/link scripts. All Compile/Link/PCH creation outputs must be plugin-owned; stop engine/host work. Existing engine inputs are allowed. Stable sources/response inputs and completed-action footprint checks bound native replanning; stdout parsing is advisory. With approved graph:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Rig
```

Each task gives its exact filter/controller. RED asserts a missing behavior in compiling code where possible. Mutations restore exact pre-mutation bytes and repeat fresh graph/native build/test. Never run an editor during a build. Child process groups are owned/drained by PID even when the leader exits. Selftests never use external services. No NoPCH or full-suite repetitions without a new concrete concern.

### Task 1: deterministic bounded route relay and fixture selftests

**Files:** Create `Tools/testserver/route_proxy.py`, `Tools/testserver/route_proxy_selftest.py`. B1 `proxy.py` stays byte-identical.

**Interfaces:** Consumes `Proxy.__init__(mode, ports, max_events=512)`, `read_header`, `add`, `wait_idle`, context-manager cleanup and owned handler/socket tracking. Produces `RouteProxy(ports, family=socket.AF_INET, hold_tls=False)`, `.hello_seen`, `.server_held`, `.release_tls`, `.snapshot()` and `.address` (bracketed IPv6 when applicable). Each gate is a threading.Event; stop never counts as release. Maximum first TLS record 18,432 payload bytes, held server flight 16,384 bytes, total relay lifetime 12 s, 16 active handlers/512 events inherited. All upstream targets remain numeric IPv4 fixture ports; IPv6 changes only the proxy's actual TCP listener.

- [ ] **Step 1: write these fixture tests before the implementation.** The complete new selftest file (each timed wait is a bounded assertion on an explicit gate, not sleep-based ordering):

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import pathlib
import socket
import socketserver
import struct
import sys
import threading
import unittest
sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from route_proxy import RouteProxy, Ipv6Unavailable, client_hello_sni
from proxy_selftest import target

def hello(name=b'localhost'):
    names = b'\x00' + struct.pack('!H', len(name)) + name
    extension = struct.pack('!H', len(names)) + names
    extensions = struct.pack('!HH', 0, len(extension)) + extension
    body = b'\x03\x03' + bytes(32) + b'\x00' + b'\x00\x02\x00\x2f' + b'\x01\x00'
    body += struct.pack('!H', len(extensions)) + extensions
    handshake = b'\x01' + len(body).to_bytes(3, 'big') + body
    return b'\x16\x03\x01' + struct.pack('!H', len(handshake)) + handshake

def connect(proxy, port):
    address = proxy.server.server_address
    sock = socket.create_connection((address[0], address[1]), timeout=2)
    sock.sendall(('CONNECT localhost:%d HTTP/1.1\r\nHost: localhost:%d\r\n\r\n' % (port, port)).encode())
    response = bytearray()
    while not response.endswith(b'\r\n\r\n'):
        part = sock.recv(1)
        if not part:
            raise AssertionError('CONNECT ended')
        response += part
    if not response.startswith(b'HTTP/1.1 200 '):
        raise AssertionError(response)
    return sock

class RouteFixtureTest(unittest.TestCase):
    def test_exact_sni_and_malformed_boundaries(self):
        self.assertEqual(client_hello_sni(hello()), 'localhost')
        for data in (b'', hello()[:-1], b'\x16\x03\x01\xff\xff', hello(b'bad\x00name')):
            with self.assertRaises(ValueError):
                client_hello_sni(data)

    def test_hold_real_record_until_explicit_release(self):
        with target() as port, RouteProxy([port], hold_tls=True) as proxy:
            with connect(proxy, port) as sock:
                payload = hello()
                sock.sendall(payload)
                self.assertTrue(proxy.hello_seen.wait(2))
                self.assertTrue(proxy.server_held.wait(2))
                sock.settimeout(.05)
                with self.assertRaises(socket.timeout):
                    sock.recv(1)
                proxy.release_tls.set()
                sock.settimeout(2)
                result = bytearray()
                while len(result) < len(payload):
                    result.extend(sock.recv(len(payload) - len(result)))
                self.assertEqual(bytes(result), payload)
            self.assertTrue(proxy.wait_idle(2))
        kinds = [row['ev'] for row in proxy.snapshot()['events']]
        self.assertLess(kinds.index('client_hello'), kinds.index('tls_hold'))
        self.assertLess(kinds.index('tls_hold'), kinds.index('tls_release'))
        self.assertEqual(kinds.count('tls_release'), 1)

    def test_stop_does_not_release(self):
        with target() as port:
            proxy = RouteProxy([port], hold_tls=True)
            proxy.__enter__()
            sock = connect(proxy, port)
            try:
                sock.sendall(hello())
                self.assertTrue(proxy.server_held.wait(2))
                proxy.__exit__(None, None, None)
                self.assertFalse(proxy.release_tls.is_set())
                self.assertNotIn('tls_release', [e['ev'] for e in proxy.snapshot()['events']])
                self.assertEqual(proxy.snapshot()['active'], 0)
            finally:
                sock.close()
                proxy.__exit__(None, None, None)

    def test_plain_route_is_transparent(self):
        with target() as port, RouteProxy([port]) as proxy:
            with connect(proxy, port) as sock:
                sock.sendall(b'ordinary')
                self.assertEqual(sock.recv(8), b'ordinary')
            self.assertTrue(proxy.wait_idle(2))
            self.assertEqual(proxy.snapshot()['dropped'], 0)

    def test_ipv6_peer_is_distinct(self):
        with target() as port:
            try:
                proxy = RouteProxy([port], family=socket.AF_INET6)
            except Ipv6Unavailable as error:
                self.skipTest('IPv6 loopback unavailable: %s' % error)
            with proxy:
                self.assertTrue(proxy.address.startswith('[::1]:'))
                with connect(proxy, port) as sock:
                    sock.sendall(b'v6')
                    self.assertEqual(sock.recv(2), b'v6')
                self.assertTrue(proxy.wait_idle(2))
                self.assertEqual(proxy.server.server_address[0], '::1')

if __name__ == '__main__':
    unittest.main()
```

Run `python3 -I Tools/testserver/route_proxy_selftest.py`; RED is missing module. Then introduce `client_hello_sni` and RouteProxy but temporarily omit the hold wait; the hold test must fail by receiving a byte before release. This gives a behavior RED beyond import failure.

- [ ] **Step 2: implement the full relay.**

```python
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Raw-probe loopback relay. B1 Proxy remains unchanged."""
import errno
import select
import socket
import struct
import threading
import time
from proxy import Proxy

def client_hello_sni(record):
    def take(data, offset, count):
        if offset < 0 or count < 0 or offset + count > len(data):
            raise ValueError('truncated ClientHello')
        return data[offset:offset + count], offset + count
    if len(record) < 9 or record[0] != 22:
        raise ValueError('not a TLS handshake record')
    length = int.from_bytes(record[3:5], 'big')
    if length > 18432 or length + 5 != len(record):
        raise ValueError('record length')
    data = record[5:]
    if data[0] != 1 or int.from_bytes(data[1:4], 'big') + 4 != len(data):
        raise ValueError('one complete ClientHello required')
    _, pos = take(data, 4, 34)
    session, pos = take(data, pos, 1)
    _, pos = take(data, pos, session[0])
    size, pos = take(data, pos, 2)
    _, pos = take(data, pos, int.from_bytes(size, 'big'))
    size, pos = take(data, pos, 1)
    _, pos = take(data, pos, size[0])
    size, pos = take(data, pos, 2)
    extensions, pos = take(data, pos, int.from_bytes(size, 'big'))
    if pos != len(data):
        raise ValueError('trailing ClientHello bytes')
    cursor = 0
    answer = None
    while cursor < len(extensions):
        head, cursor = take(extensions, cursor, 4)
        kind, count = struct.unpack('!HH', head)
        body, cursor = take(extensions, cursor, count)
        if kind != 0:
            continue
        size, index = take(body, 0, 2)
        if int.from_bytes(size, 'big') + 2 != len(body):
            raise ValueError('name-list length')
        while index < len(body):
            head, index = take(body, index, 3)
            raw, index = take(body, index, int.from_bytes(head[1:], 'big'))
            if head[0] == 0:
                if answer is not None or not raw or any(c < 33 or c > 126 for c in raw):
                    raise ValueError('invalid or duplicate hostname')
                answer = raw.decode('ascii')
    return '' if answer is None else answer

class Ipv6Unavailable(OSError):
    def __init__(self, error):
        super().__init__(error.errno, str(error))
        self.evidence = dict(operation='bind-ipv6-loopback', family='AF_INET6', host='::1', errno=error.errno)

class RouteProxy(Proxy):
    def __init__(self, ports, family=socket.AF_INET, hold_tls=False):
        super().__init__('tunnel', ports)
        self.hold_tls = hold_tls
        self.hello_seen = threading.Event()
        self.server_held = threading.Event()
        self.release_tls = threading.Event()
        self.family = family
        if family == socket.AF_INET6:
            old_server = self.server
            handler = old_server.RequestHandlerClass
            base = type(old_server)
            old_server.server_close()
            class V6Server(base):
                address_family = socket.AF_INET6
            try:
                self.server = V6Server(('::1', 0), handler)
            except OSError as error:
                if error.errno in (errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT, errno.EADDRNOTAVAIL):
                    raise Ipv6Unavailable(error) from error
                raise
            self.address = '[::1]:%d' % self.server.server_address[1]
        elif family != socket.AF_INET:
            self.server.server_close()
            raise ValueError('unsupported family')

    def handle(self, sock):
        with self.lock:
            self.next_id += 1
            ident = self.next_id
        sock.settimeout(1)
        deadline = time.monotonic() + self.lifetime_seconds
        try:
            authority, port, headers = self.read_header(sock)
            self.add(ident, 'connect', authority=authority, headers=headers,
                     peer_host=sock.getsockname()[0], peer_port=sock.getsockname()[1])
            with socket.create_connection(('127.0.0.1', port), timeout=1) as upstream:
                upstream.settimeout(1)
                self.add(ident, 'upstream', host='127.0.0.1', port=port)
                sock.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
                self.add(ident, 'tunnel')
                prefix = bytearray()
                hello_done = False
                released = False
                while not self.stopping.is_set() and time.monotonic() < deadline:
                    readable, _, _ = select.select([sock, upstream], [], [], .02)
                    for source in readable:
                        if self.stopping.is_set():
                            return
                        payload = source.recv(16384)
                        if self.stopping.is_set():
                            return
                        if not payload:
                            self.add(ident, 'peer_eof' if source is sock else 'upstream_eof')
                            return
                        if source is sock and self.hold_tls and not hello_done:
                            prefix.extend(payload)
                            if len(prefix) > 18437:
                                raise ValueError('ClientHello accumulation bound')
                            if len(prefix) < 5:
                                continue
                            needed = 5 + int.from_bytes(prefix[3:5], 'big')
                            if needed > 18437:
                                raise ValueError('TLS record bound')
                            if len(prefix) < needed:
                                continue
                            sni = client_hello_sni(bytes(prefix[:needed]))
                            self.add(ident, 'client_hello', sni=sni, record_bytes=needed)
                            upstream.sendall(prefix)
                            prefix.clear()
                            hello_done = True
                            self.hello_seen.set()
                            continue
                        if source is upstream and self.hold_tls and not released:
                            if not hello_done:
                                raise ValueError('server bytes before ClientHello')
                            self.add(ident, 'tls_hold', bytes=len(payload))
                            self.server_held.set()
                            while not self.release_tls.wait(.02):
                                if self.stopping.is_set() or time.monotonic() >= deadline:
                                    self.add(ident, 'hold_stopped' if self.stopping.is_set() else 'hold_deadline')
                                    return
                            if self.stopping.is_set():
                                self.add(ident, 'hold_stopped')
                                return
                            self.add(ident, 'tls_release', bytes=len(payload))
                            sock.sendall(payload)
                            released = True
                            continue
                        (upstream if source is sock else sock).sendall(payload)
                self.add(ident, 'stopped' if self.stopping.is_set() else 'relay_deadline')
        except (OSError, ValueError, UnicodeError) as error:
            self.add(ident, 'fixture_error', kind=type(error).__name__, detail=str(error))
        finally:
            self.add(ident, 'end')
```

The inherited context exit sets stop before closing sockets and joins non-daemon handlers. The gate releases automatically only on explicit event; its timeout records failure and never `tls_release`. IPv6 uses the same lifecycle and CONNECT whitelist; it does not accept non-loopback or arbitrary upstream DNS.

- [ ] **Step 3: GREEN and deterministic restoration.** Run `python3 -I Tools/testserver/route_proxy_selftest.py` and existing `python3 -I Tools/testserver/proxy_selftest.py`. Mutation 1: bypass `while not self.release_tls.wait(.02)` with `while False`; hold test must fail. Mutation 2: set `release_tls` in `__exit__` before calling base; stop test must fail even if no bytes reach client. Mutation 3: `client_hello_sni` returns `'localhost'` immediately; malformed-boundary test fails. Restore exact bytes after each; all five tests pass or IPv6 is explicitly skipped with errno. A skipped IPv6 selftest becomes unsupported evidence, never a passed IPv6 acquisition.

- [ ] **Step 4: task review and exact-path commit.**

```bash
git add Tools/testserver/route_proxy.py Tools/testserver/route_proxy_selftest.py
git commit -m "test: add bounded TLS-gated route proxy fixture" -m "Co-Authored-By: Codex <noreply@openai.com>"
```
### Task 2: raw immutable-vhost acquisition, TLS identity and ownership witnesses

**Files:** Modify `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h`, `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.cpp`, and Task1 `Tools/testserver/route_proxy.py`; create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRouteTest.cpp`, `Tools/run_lws_route_probe.py`, `Tools/lws_route_passport.py`, `docs/passport/2026-10-23-lws30-route-probe.md`. Existing B1 exporter and production files are unchanged. All raw acquisition/controller/validator/mutation gates complete before this task is reviewed or committed.

**Interfaces:** Consumes landed FRig/recording/WaitFor/Connect/Shutdown and Task 1's external gate. Produces `FRouteSpec`, `FRig::AddRoute(FRouteSpec)`, `FRig::DestroyRoute(int32)`, `FConnectSpec::RouteId`, `FRig::RecordSslIdentity(int32,const TCHAR*)`; `Probe.VaRest.Lws.Route.Attempt` consumes explicit child arguments and writes schema1 trace after joined teardown. Routes are at most8 per rig, not recycled; strings stay owned through context destruction. Every connection still occupies the existing rig's owned FConn list until rig destruction.

- [ ] **Step 1: add the bounded route data model and RED test.** In the probe header before FRigOptions add:

```cpp
struct FRouteSpec
{
    FString Name;
    FString ProxyAddress;
    uint32 ProxyPort = 0;
    bool bClientTls = true;
};
struct FRoute
{
    int32 Id = 0;
    FRouteSpec Spec;
    TArray<ANSICHAR> NameUtf8;
    TArray<ANSICHAR> ProxyUtf8;
    lws_vhost* Vhost = nullptr;
};
/** Any thread, pure: the fixed raw-rig allocation budget. */
inline bool CanAddRoute(int32 Existing) { return Existing >= 0 && Existing < 8; }
```

Add `TFunction<void(FRig&, bool)> OnRouteDestroyStage;` to FRigOptions (synchronous service-thread callback before/after the public destroy call, shared capture only). Add `int32 RouteId = 0;` to FConnectSpec and `bool bExplicitRoutes = false;` to FRigOptions. Add public methods below and private `TArray<TUniquePtr<FRoute>> Routes; int32 NextRouteId = 1; void AddRouteNow(int32, FRouteSpec); FRoute* FindRoute(int32);`. Methods' thread comments are part of their declarations:

```cpp
/** Game thread: allocate a monotone id and enqueue creation; 0 refuses the eight-route cap. */
int32 AddRoute(FRouteSpec InSpec);
/** Game thread: enqueue public vhost destruction; retained route strings survive context destruction. */
void DestroyRoute(int32 InRouteId);
/** Game thread: queue a service-thread SSL/WSI identity witness, never exposes addresses. */
void RecordSslIdentity(int32 ConnId, const TCHAR* Label);
```

A game-thread `int32 RequestedRoutes = 0;` is the admission count, distinct from service-owned Routes.Num(). Add this test to the new RouteTest.cpp within the includes/guard shown later:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRouteBudgetTest, "Probe.VaRest.Lws.Route.Budget",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRouteBudgetTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    for (int32 Index = 0; Index < 8; ++Index) { TestTrue(TEXT("route slot admitted"), CanAddRoute(Index)); }
    TestFalse(TEXT("ninth refused"), CanAddRoute(8));
    TestFalse(TEXT("negative count refused"), CanAddRoute(-1));
    return true;
}
```

Behavior RED: temporarily return true in CanAddRoute; fresh normal graph/build and `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Route.Budget` fails ninth/negative rows. Restore the shown predicate for GREEN.

- [ ] **Step 2: add exact raw FRig implementation.** In its constructor, immediately after `Info.options = Options.ContextOptions;`, add:

```cpp
if (Options.bExplicitRoutes) { Info.options |= LWS_SERVER_OPTION_EXPLICIT_VHOSTS; }
```

For explicit-route tests set `Options.bCommandsInsideCallback = false`; an empty explicit context has no protocol vhost to receive the cancel broadcast. Keep bounded50ms service polling; no blocking waits inside commands. Do not change ordinary rig defaults.

At file scope, under WITH_DEV_AUTOMATION_TESTS, use the same weak declaration as W0:

```cpp
#if PLATFORM_LINUX
extern "C" __attribute__((weak)) int openssl_websocket_private_data_index;
#endif
```

Add these helpers to `namespace VaRest::LwsProbe::VaRestLwsProbePrivate` (not a new anonymous namespace):

```cpp
FVaRestJson IndexObservation()
{
    FVaRestJson Out = FVaRestJson::Object().Set(TEXT("known"), false);
#if PLATFORM_LINUX
    if (&openssl_websocket_private_data_index != nullptr)
    {
        Out.Set(TEXT("known"), true).Set(TEXT("value"), openssl_websocket_private_data_index);
    }
#endif
    return Out;
}
void RoutePhase(FRig& Rig, int32 RouteId, const TCHAR* Phase, bool bValue)
{
    const FVaRestJson Row = FVaRestJson::Object().Set(TEXT("phase"), Phase)
        .Set(TEXT("route"), RouteId).Set(TEXT("value"), bValue)
        .Set(TEXT("index"), IndexObservation()).Set(TEXT("watch"), Rig.WatchedSslIdentity());
    const FString Encoded = Row.ToString();
    Rig.Record(0, Marker, RouteId, Encoded);
    UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-PHASE %s"), *Encoded);
    GLog->Flush();
}
```

Append complete method bodies to Probe.cpp:

```cpp
int32 VaRest::LwsProbe::FRig::AddRoute(FRouteSpec InSpec)
{
    check(IsInGameThread());
    if (!Options.bExplicitRoutes || !CanAddRoute(RequestedRoutes)) { return 0; }
    ++RequestedRoutes;
    const int32 Assigned = NextRouteId++;
    Enqueue([this, Assigned, Moved = MoveTemp(InSpec)]() mutable { AddRouteNow(Assigned, MoveTemp(Moved)); });
    return Assigned;
}
VaRest::LwsProbe::FRoute* VaRest::LwsProbe::FRig::FindRoute(int32 InRouteId)
{
    for (const TUniquePtr<FRoute>& Item : Routes)
    {
        if (Item->Id == InRouteId) { return Item.Get(); }
    }
    return nullptr;
}
void VaRest::LwsProbe::FRig::AddRouteNow(int32 InRouteId, FRouteSpec InSpec)
{
    using namespace VaRestLwsProbePrivate;
    TUniquePtr<FRoute> Made = MakeUnique<FRoute>();
    Made->Id = InRouteId;
    Made->Spec = MoveTemp(InSpec);
    FRoute* Stable = Made.Get();
    Routes.Add(MoveTemp(Made));
    lws_context_creation_info Info = {};
    Info.port = CONTEXT_PORT_NO_LISTEN;
    Info.protocols = Protocols;
    Info.uid = -1;
    Info.gid = -1;
    Info.user = this;
    Info.options = Options.ContextOptions | LWS_SERVER_OPTION_EXPLICIT_VHOSTS;
    Info.max_http_header_data2 = 32 * 1024;
    Info.pt_serv_buf_size = 32 * 1024;
    Info.timeout_secs = Options.TimeoutSecs;
    Info.vhost_name = Utf8Copy(Stable->Spec.Name, Stable->NameUtf8);
    Info.http_proxy_address = Stable->Spec.ProxyAddress.IsEmpty() ? nullptr
        : Utf8Copy(Stable->Spec.ProxyAddress, Stable->ProxyUtf8);
    Info.http_proxy_port = Stable->Spec.ProxyPort;
    RoutePhase(*this, InRouteId, TEXT("create-before"), true);
    Stable->Vhost = lws_create_vhost(Context, &Info);
    RoutePhase(*this, InRouteId, TEXT("create-after"), Stable->Vhost != nullptr);
    if (Stable->Vhost && Stable->Spec.bClientTls)
    {
        Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
        Info.provided_client_ssl_ctx = Options.Tls == ETls::ProvidedContext
            ? Options.ProvidedSslContext : SslModuleContext;
        RoutePhase(*this, InRouteId, TEXT("tls-init-before"), true);
        const int32 Result = lws_init_vhost_client_ssl(&Info, Stable->Vhost);
        RoutePhase(*this, InRouteId, TEXT("tls-init-after"), Result == 0);
        Record(0, Marker, Result, FString::Printf(TEXT("route-tls-result:%d"), InRouteId));
    }
    else { RoutePhase(*this, InRouteId, TEXT("tls-init-skipped"), false); }
    RoutePhase(*this, InRouteId, TEXT("ready"), Stable->Vhost != nullptr);
}
void VaRest::LwsProbe::FRig::DestroyRoute(int32 InRouteId)
{
    check(IsInGameThread());
    Enqueue([this, InRouteId]
    {
        FRoute* Found = FindRoute(InRouteId);
        VaRestLwsProbePrivate::RoutePhase(*this, InRouteId, TEXT("destroy-before"), Found && Found->Vhost);
        if (Options.OnRouteDestroyStage) { Options.OnRouteDestroyStage(*this, true); }
        if (Found && Found->Vhost)
        {
            lws_vhost_destroy(Found->Vhost);
            Found->Vhost = nullptr;
        }
        if (Options.OnRouteDestroyStage) { Options.OnRouteDestroyStage(*this, false); }
        VaRestLwsProbePrivate::RoutePhase(*this, InRouteId, TEXT("destroy-after"), true);
    });
}
FVaRestJson VaRest::LwsProbe::FRig::SslIdentity(FConn& Connection)
{
    // Caller has matched the live owned WSI before entering; no unknown callback pointer is inspected.
    SSL* Ssl = lws_get_ssl(Connection.Wsi);
    if (Connection.ObservedSsl && Connection.ObservedSsl != Ssl)
    { SslTokens.Observe(Connection.ObservedSsl, Connection.Id, true); }
    Connection.ObservedSsl = Ssl;
    const auto WsiIdentity = TraceIdentity.Observe(Connection.Wsi, Connection.Id, false);
    const auto SslIdentityToken = SslTokens.Observe(Ssl, Connection.Id, false);
    lws_vhost* Actual = lws_get_vhost(Connection.Wsi);
    int32 ActualRoute = 0;
    for (const auto& Route : Routes) { if (Route->Vhost && Route->Vhost == Actual) { ActualRoute = Route->Id; } }
    SSL_CTX* Origin = Ssl ? SSL_get_SSL_CTX(Ssl) : nullptr;
    const bool bSupplied = Origin && Origin == Options.ProvidedSslContext;
    bool bIndexKnown = false, bIndexMatches = false;
#if PLATFORM_LINUX
    if (&openssl_websocket_private_data_index != nullptr)
    {
        bIndexKnown = true;
        bIndexMatches = Ssl && openssl_websocket_private_data_index >= 0
            && SSL_get_ex_data(Ssl, openssl_websocket_private_data_index) == Connection.Wsi;
    }
#endif
    return FVaRestJson::Object().Set(TEXT("conn"), Connection.Id)
        .Set(TEXT("route"), Connection.Spec.RouteId).Set(TEXT("actual_route"), ActualRoute)
        .Set(TEXT("wsi_token"), WsiIdentity.Id).Set(TEXT("ssl_token"), SslIdentityToken.Id)
        .Set(TEXT("ssl_exists"), Ssl != nullptr).Set(TEXT("wsi_user"), lws_wsi_user(Connection.Wsi) == &Connection)
        .Set(TEXT("ctx_exists"), Origin != nullptr).Set(TEXT("ctx_supplied"), bSupplied)
        .Set(TEXT("ctx_origin"), !Origin ? TEXT("none") : bSupplied ? TEXT("supplied") : TEXT("other"))
        .Set(TEXT("index_known"), bIndexKnown).Set(TEXT("index_matches_wsi"), bIndexMatches)
        .Set(TEXT("index"), VaRestLwsProbePrivate::IndexObservation());
}
void VaRest::LwsProbe::FRig::RecordSslIdentity(int32 ConnId, const TCHAR* Label)
{
    const FString SavedLabel(Label);
    ExecOn(ConnId, Label, [SavedLabel](FConn& Connection)
    {
        const FString Encoded = Connection.Rig->SslIdentity(Connection).Set(TEXT("phase"), SavedLabel).ToString();
        Connection.Rig->Record(Connection.Id, Marker, 0, Encoded);
        UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-IDENTITY %s"), *Encoded);
        GLog->Flush();
    });
}
```

`SSL_get_ex_data` compares values; it never dereferences the retrieved pointer. If symbol is absent index_matches_wsi=false means unobserved, not a failed match. Preserve `tls-init` return as a measurement; no automatic second initialization or guessing a repair after failure.

In `ConnectNow`, immediately after `Info.context = Context;`, insert:

```cpp
if (Options.bExplicitRoutes)
{
    FRoute* Selected = FindRoute(Spec.RouteId);
    if (!Selected || !Selected->Vhost)
    {
        Conn.bDestroyed = true;
        Record(Conn.Id, ConnectReturned, 0, TEXT("null: route absent"));
        return;
    }
    Info.vhost = Selected->Vhost;
}
```

After `lws_context_destroy(Context); Context = nullptr;` in Shutdown, before any provided context release, add `Routes.Empty();` (all string storage lives until the destruction call returns). Do not individually destroy already destroyed vhosts again. The raw callback dispatch below takes precedence over the existing accessor-based dispatch; ordinary B1 mode remains unchanged.

**Paired operation identity (RP6).** Add private `int32 WatchedConnectionId=0;`, public game-thread `void WatchConnection(int32 ConnId);`, and public service-thread synchronous `FVaRestJson WatchedSslIdentity();`. Every RoutePhase emits its own weak-index observation and live watched identity, rather than a global minimum/maximum across routes. Implement:

```cpp
void VaRest::LwsProbe::FRig::WatchConnection(int32 ConnId)
{
    Enqueue([this, ConnId] { WatchedConnectionId = ConnId; });
}
FVaRestJson VaRest::LwsProbe::FRig::WatchedSslIdentity()
{
    FConn* Found = nullptr;
    {
        FScopeLock Guard(&ConnsLock);
        for (const auto& Item : Conns)
        {
            if (Item->Id == WatchedConnectionId && !Item->bDestroyed && Item->Wsi) { Found = Item.Get(); break; }
        }
    }
    return Found ? SslIdentity(*Found) : FVaRestJson::Object().Set(TEXT("conn"), 0)
        .Set(TEXT("wsi_token"), 0).Set(TEXT("ssl_token"), 0).Set(TEXT("ssl_exists"), false);
}
```

A successfully returned create emits create-after even when it returns null; its init emits tls-init-skipped. A successful create emits both init records even for a returned init failure. A child crash can truncate a pair; parent renders partial and does not fill the absent after record. Separate create/init index movements are attributed only from that operation's own before/after values, with held-original identity on both sides. Missing weak symbol remains unavailable.

**Safe raw callback dispatch (RP3), before existing accessor-based code.** Add service-owned `SSL* ObservedSsl=nullptr;` to FConn (opaque remembered identity, never dereferenced after retirement). Add public synchronous `FVaRestJson SslIdentity(FConn& Connection);`, private `FTraceIdentity SslTokens; int32 HandleRoute(lws*,int32,void*,void*,size_t);`. This callable has no externally retained pointer. `SslIdentity` uses public lws_get_vhost (bundled header3197–3202) and SSL_get_SSL_CTX (ssl.h2001); returned unknown contexts are compared only, never dereferenced/freed.

Add this pure helper to the header in VaRest::LwsProbe and its deterministic test to RouteTest.cpp:

```cpp
inline bool WithOwnedToken(const void* Candidate, TConstArrayView<const void*> Known, TFunctionRef<void()> Inspect)
{
    if (!Candidate || !Known.Contains(Candidate)) { return false; }
    Inspect();
    return true;
}
```

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestRouteUnknownTokenTest, "Probe.VaRest.Lws.Route.UnknownToken",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestRouteUnknownTokenTest::RunTest(const FString& Parameters)
{
    int32 OwnedStorage = 0, UnexpectedStorage = 0, Accesses = 0;
    const void* Known[] = {&OwnedStorage};
    TestFalse(TEXT("unowned token refused before accessor"), VaRest::LwsProbe::WithOwnedToken(
        &UnexpectedStorage, MakeArrayView(Known), [&Accesses] { ++Accesses; }));
    TestEqual(TEXT("no accessor on unowned token"), Accesses, 0);
    TestTrue(TEXT("owned token inspected"), VaRest::LwsProbe::WithOwnedToken(
        &OwnedStorage, MakeArrayView(Known), [&Accesses] { ++Accesses; }));
    TestEqual(TEXT("exactly one owned access"), Accesses, 1);
    return true;
}
```

In Probe.cpp's existing private namespace add `std::atomic<FRig*> RouteCallbackOwner{nullptr};`. Immediately after the constructor's game-thread check, before any LWS context call:

```cpp
if (Options.bExplicitRoutes)
{
    FRig* Expected = nullptr;
    check(VaRestLwsProbePrivate::RouteCallbackOwner.compare_exchange_strong(Expected, this));
}
```

In Shutdown after thread join, context destruction and Routes.Empty, use compare_exchange to clear only this rig (also execute when context creation failed):

```cpp
if (Options.bExplicitRoutes)
{
    FRig* Expected = this;
    VaRestLwsProbePrivate::RouteCallbackOwner.compare_exchange_strong(Expected, nullptr);
}
```

At the **first line** of static FRig::Callback, before lws_get_context:

```cpp
if (FRig* RawOwner = VaRestLwsProbePrivate::RouteCallbackOwner.load())
{
    return RawOwner->HandleRoute(Wsi, static_cast<int32>(Reason), User, In, Len);
}
```

This raw acquisition has one registered FRig; registration spans construction, service callbacks, joined shutdown callbacks and failed construction. Ordinary B1 without bExplicitRoutes uses its existing dispatch unchanged. At the start of FindConn, before lws_wsi_user, add:

```cpp
if (Options.bExplicitRoutes)
{
    FScopeLock Lock(&ConnsLock);
    for (const auto& Each : Conns)
    {
        if (!Each->bDestroyed && Each->Wsi && Each->Wsi == Wsi) { return Each.Get(); }
    }
    return nullptr;
}
```

Complete HandleRoute, with no unknown-WSI accessor:

```cpp
int32 VaRest::LwsProbe::FRig::HandleRoute(lws* InWsi, int32 Reason, void* User, void* In, size_t Len)
{
    // Context/vhost management callbacks carry non-client WSI values; they need no WSI accessor.
    switch (Reason)
    {
    case LWS_CALLBACK_GET_THREAD_ID:
    case LWS_CALLBACK_ADD_POLL_FD:
    case LWS_CALLBACK_DEL_POLL_FD:
    case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
    case LWS_CALLBACK_LOCK_POLL:
    case LWS_CALLBACK_UNLOCK_POLL:
    case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
    case LWS_CALLBACK_PROTOCOL_INIT:
    case LWS_CALLBACK_PROTOCOL_DESTROY:
        return 0;
    case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
        // Documented context-construction argument, not an accessor through an unknown WSI.
        if (Options.OnLoadExtraClientVerifyCerts && User)
        { Options.OnLoadExtraClientVerifyCerts(static_cast<SSL_CTX*>(User)); }
        return 0;
    default: break;
    }
    FConn* Known = FindConn(InWsi, nullptr); // raw branch compares retained pointers only
    const void* Owned[] = {Known ? Known->Wsi : nullptr};
    int32 Answer = Reason == LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION ? 1 : 0;
    const bool bInspected = WithOwnedToken(InWsi, MakeArrayView(Owned), [&]
    {
        if (Reason == LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION)
        {
            SSL* KnownSsl = lws_get_ssl(Known->Wsi);
            if (!KnownSsl || In != KnownSsl || !User)
            {
                const FString Text = FVaRestJson::Object().Set(TEXT("phase"), TEXT("callback-input-mismatch"))
                    .Set(TEXT("conn"), Known->Id).ToString();
                Record(Known->Id, Marker, Reason, Text);
                UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-IDENTITY %s"), *Text);
                GLog->Flush();
                return;
            }
        }
        if (Reason == LWS_CALLBACK_WSI_DESTROY && Known->ObservedSsl)
        {
            SslTokens.Observe(Known->ObservedSsl, Known->Id, true);
            Known->ObservedSsl = nullptr;
        }
        Answer = Handle(InWsi, Reason, User, In, Len);
    });
    if (!bInspected)
    {
        // CREATE may precede pwsi publication. Record its opaque token without treating it as a dereferenceable WSI.
        if (Reason == LWS_CALLBACK_WSI_CREATE || Reason == LWS_CALLBACK_WSI_DESTROY)
        {
            const auto Token = TraceIdentity.Observe(InWsi, ConnectingId, Reason == LWS_CALLBACK_WSI_DESTROY);
            FEvent Event;
            Event.Conn = Token.Conn; Event.WsiToken = Token.Id; Event.Reason = Reason;
            Event.Time = FPlatformTime::Seconds(); Event.Len = static_cast<int64>(Len);
            Append(MoveTemp(Event));
        }
        else
        {
            const FString Text = FVaRestJson::Object().Set(TEXT("phase"), TEXT("callback-unowned"))
                .Set(TEXT("reason"), Reason).Set(TEXT("connecting"), ConnectingId).ToString();
            Record(0, Marker, Reason, Text);
            UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-IDENTITY %s"), *Text);
            GLog->Flush();
        }
    }
    return Answer;
}
```

The existing Handle certificate-store inspection and hook now execute only after known WSI equality and known live SSL/input equality. The documented store argument is read only then. No userdata lookup is used to establish ownership. Unknown bookkeeping records remain raw measured mismatches, never a manufactured TLS success. Library-before-callback crashes remain distinguishable from a refused harness callback. Restore mutation in WithOwnedToken that calls Inspect before membership must fail UnknownToken's zero-access assertion; do not pass an invalid address to a real LWS accessor.

- [ ] **Step 3: implement one raw acquisition test with complete final export.** New RouteTest.cpp uses these includes, helper definitions and body; append the Budget test from Step1 in the same guard. All test APIs return data, not a production-safe verdict.

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsProbe.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "HAL/CriticalSection.h"
#include "../WebSocket/VaRestWsTestUtil.h"

namespace VaRest::LwsProbe::VaRestLwsProbeRouteTestPrivate
{
    struct FOwnership
    {
        std::atomic<int32> Frees{0};
        std::atomic<int32> Stage{0};
        FCriticalSection Lock;
        TArray<FVaRestJson> Destructions;
        void Snapshot(FRig& Rig, int32 InStage, const TCHAR* Label)
        {
            Stage.store(InStage);
            const FString Row = FVaRestJson::Object().Set(TEXT("phase"), TEXT("ownership"))
                .Set(TEXT("stage"), InStage).Set(TEXT("label"), Label).Set(TEXT("frees"), Frees.load()).ToString();
            Rig.Record(0, Marker, InStage, Row);
            UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-OWNERSHIP %s"), *Row);
            GLog->Flush();
        }
        FVaRestJson Export()
        {
            FScopeLock Guard(&Lock);
            FVaRestJson Out = FVaRestJson::Array();
            for (const auto& Row : Destructions) { Out.Add(Row); }
            return Out;
        }
    };
    void ContextFreed(void* Parent, void* Pointer, CRYPTO_EX_DATA* Data, int Index, long ArgL, void* ArgP)
    {
        if (!Pointer) { return; }
        auto* State = static_cast<FOwnership*>(Pointer);
        const FVaRestJson Row = FVaRestJson::Object().Set(TEXT("stage"), State->Stage.load())
            .Set(TEXT("count"), State->Frees.fetch_add(1) + 1);
        {
            FScopeLock Guard(&State->Lock);
            if (State->Destructions.Num() < 8) { State->Destructions.Add(Row); }
        }
        UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-FINAL-CTX-DESTRUCTION %s"), *Row.ToString());
        GLog->Flush();
    }
    FVaRestJson Encode(const FEvent& Item)
    {
        return FVaRestJson::Object().Set(TEXT("seq"), Item.Sequence).Set(TEXT("conn"), Item.Conn)
            .Set(TEXT("wsi"), Item.WsiToken).Set(TEXT("reason"), Item.Reason).Set(TEXT("name"), ReasonName(Item.Reason)).Set(TEXT("time"), Item.Time)
            .Set(TEXT("len"), Item.Len).Set(TEXT("input_present"), Item.bInputPresent)
            .Set(TEXT("hex"), HexOf(Item.Bytes)).Set(TEXT("note"), Item.Note)
            .Set(TEXT("depth"), Item.CertDepth).Set(TEXT("x509_error"), Item.CertError)
            .Set(TEXT("preverify"), Item.Preverify).Set(TEXT("flags"), static_cast<int64>(Item.Flags));
    }
    bool CommandSeen(FRig& Rig, const FString& Label)
    {
        return Rig.WaitFor([Label](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([Label](const FEvent& Item)
            {
                return Item.Reason == CommandRan && Item.Note.Equals(Label, ESearchCase::CaseSensitive);
            });
        }, 3.0);
    }
    void Fence(FRig& Rig, const TCHAR* Label)
    {
        Rig.Exec(Label, [](FRig& Self) {});
    }
    bool Retired(FRig& Rig, int32 ConnId)
    {
        return Rig.WaitFor([ConnId](const TArray<FEvent>& All)
        {
            TSet<int32> Associated;
            for (const FEvent& Item : All) { if (Item.Conn == ConnId && Item.WsiToken) { Associated.Add(Item.WsiToken); } }
            TSet<int32> Destroyed;
            bool bReturnedNull = false;
            for (const FEvent& Item : All)
            {
                if (Associated.Contains(Item.WsiToken) && Item.Reason == LWS_CALLBACK_WSI_DESTROY) { Destroyed.Add(Item.WsiToken); }
                if (Item.Conn == ConnId && Item.Reason == ConnectReturned && Item.Note.StartsWith(TEXT("null"), ESearchCase::CaseSensitive)) { bReturnedNull = true; }
            }
            return Associated.Num() > 0 ? Destroyed.Num() == Associated.Num() : bReturnedNull;
        }, 3.0);
    }
    bool Primary(FRig& Rig, int32 ConnId)
    {
        return Rig.WaitFor([ConnId](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([ConnId](const FEvent& Item)
            {
                return Item.Conn == ConnId && (Item.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED
                    || Item.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR || Item.Reason == LWS_CALLBACK_WSI_DESTROY
                    || (Item.Reason == ConnectReturned && Item.Note.StartsWith(TEXT("null"), ESearchCase::CaseSensitive)));
            });
        }, 5.0);
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsRouteAttemptTest, "Probe.VaRest.Lws.Route.Attempt",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestLwsRouteAttemptTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    using namespace VaRest::LwsProbe::VaRestLwsProbeRouteTestPrivate;
    using VaRest::LwsProbe::FEvent;
    VaRest::WsTest::ExpectLwsLines(*this);
    FString CaseId, Mode, FirstProxy, NextProxy, TracePath, CaPath, Path, GateReady, GateRelease;
    const TCHAR* Cmd = FCommandLine::Get();
    const bool bInputs = FParse::Value(Cmd, TEXT("VaRestRouteCase="), CaseId)
        && FParse::Value(Cmd, TEXT("VaRestRouteMode="), Mode)
        && FParse::Value(Cmd, TEXT("VaRestRouteFirst="), FirstProxy)
        && FParse::Value(Cmd, TEXT("VaRestRouteNext="), NextProxy)
        && FParse::Value(Cmd, TEXT("VaRestRouteTrace="), TracePath)
        && FParse::Value(Cmd, TEXT("VaRestRouteCa="), CaPath)
        && FParse::Value(Cmd, TEXT("VaRestRoutePath="), Path)
        && FParse::Value(Cmd, TEXT("VaRestRouteGateReady="), GateReady)
        && FParse::Value(Cmd, TEXT("VaRestRouteGateRelease="), GateRelease);
    if (!TestTrue(TEXT("route inputs"), bInputs)) { return false; }
    FServer Site;
    if (!FindServer(*this, Site, true)) { return false; }
    TArray<uint8> Pem;
    if (!TestTrue(TEXT("CA bytes"), FFileHelper::LoadFileToArray(Pem, *CaPath))) { return false; }
    const bool bTls = FParse::Param(Cmd, TEXT("VaRestRouteTls"));
    const bool bProvided = FParse::Param(Cmd, TEXT("VaRestRouteProvided"));
    const bool bHeld = Mode.Equals(TEXT("held"), ESearchCase::CaseSensitive)
        || Mode.Equals(TEXT("held-reuse"), ESearchCase::CaseSensitive);
    const bool bLate = Mode.Equals(TEXT("held"), ESearchCase::CaseSensitive);
    const bool bFallback = Mode.Equals(TEXT("fallback"), ESearchCase::CaseSensitive);
    const bool bUntrusted = Mode.Equals(TEXT("untrusted"), ESearchCase::CaseSensitive);
    int32 SeparatePort = 0;
    FParse::Value(Cmd, TEXT("VaRestRouteProxyPort="), SeparatePort);
    auto Ownership = MakeShared<FOwnership, ESPMode::ThreadSafe>();
    SSL_CTX* Provided = nullptr;
    if (bProvided)
    {
        Provided = SSL_CTX_new(TLS_client_method());
        if (!TestNotNull(TEXT("provided context"), Provided)) { return false; }
        const int Slot = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, &ContextFreed);
        if (!TestTrue(TEXT("free observer slot"), Slot >= 0)) { SSL_CTX_free(Provided); return false; }
        if (!TestEqual(TEXT("install free observer"), SSL_CTX_set_ex_data(Provided, Slot, &Ownership.Get()), 1))
        { SSL_CTX_free(Provided); return false; }
        if (!bUntrusted) { AddPemToStore(Provided, Pem); }
    }
    ON_SCOPE_EXIT { if (Provided && Ownership->Frees.load() == 0) { SSL_CTX_free(Provided); } };
    FRigOptions Options;
    Options.bExplicitRoutes = true;
    Options.bCommandsInsideCallback = false;
    Options.ServiceTimeoutMs = 50;
    Options.TimeoutSecs = 5;
    Options.MaxRecordedEvents = 8192;
    Options.Tls = bProvided ? ETls::ProvidedContext : ETls::LwsGlobalInit;
    Options.ProvidedSslContext = Provided;
    Options.OnRouteDestroyStage = [Ownership](FRig& Self, bool bBefore)
    {
        Ownership->Snapshot(Self, bBefore ? 1 : 2, bBefore ? TEXT("vhost-before") : TEXT("vhost-after"));
    };
    if (!bUntrusted)
    {
        Options.OnLoadExtraClientVerifyCerts = [SavedPem = Pem](SSL_CTX* Ctx) { AddPemToStore(Ctx, SavedPem); };
    }
    FRig Rig(Site, MoveTemp(Options));
    if (!TestTrue(TEXT("one raw context ready"), Rig.IsReady())) { return false; }
    auto MakeRoute = [](const TCHAR* Name, const FString& Proxy, uint32 Port)
    {
        FRouteSpec Out;
        Out.Name = Name;
        Out.ProxyAddress = Proxy;
        Out.ProxyPort = Port;
        return Out;
    };
    const int32 FirstRoute = Rig.AddRoute(MakeRoute(TEXT("route-first"), FirstProxy, 0));
    int32 NextRoute = bLate ? 0 : Rig.AddRoute(MakeRoute(TEXT("route-next"), NextProxy, static_cast<uint32>(SeparatePort)));
    Fence(Rig, TEXT("routes-precreated"));
    bool bHarness = CommandSeen(Rig, TEXT("routes-precreated"));
    FConnectSpec Spec;
    Spec.RouteId = Mode.Equals(TEXT("precreated"), ESearchCase::CaseSensitive) ? NextRoute : FirstRoute;
    Spec.Address = TEXT("localhost"); // CONNECT identity, never the proxy's numeric peer
    Spec.Host = FString::Printf(TEXT("localhost:%d"), bTls ? Site.TlsPort : Site.Port);
    Spec.Path = Path;
    Spec.bTls = bTls;
    Spec.AppendHeaders.Add(TEXT("Authorization: Bearer route-probe-marker"));
    Spec.Hook = [](FConn& Connection, lws* InWsi, int32 Reason, void* User, void* In, size_t Len)
    {
        if (Reason != LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION) { return KeepDefault; }
        // First compare the callback WSI against the owned connection. Never inspect an unexpected pointer.
        const bool bKnownWsi = InWsi && InWsi == Connection.Wsi;
        SSL* KnownSsl = bKnownWsi ? lws_get_ssl(InWsi) : nullptr;
        const bool bKnownInput = KnownSsl && In == KnownSsl;
        X509_STORE_CTX* Store = static_cast<X509_STORE_CTX*>(User);
        // Store belongs to this documented callback; only inspect it after the owned WSI/SSL input match.
        const bool bStoreSsl = bKnownInput && Store
            && X509_STORE_CTX_get_ex_data(Store, SSL_get_ex_data_X509_STORE_CTX_idx()) == KnownSsl;
        const bool bUser = bKnownWsi && lws_wsi_user(InWsi) == &Connection;
        const FString Row = Connection.Rig->SslIdentity(Connection).Set(TEXT("phase"), TEXT("verify-identity"))
            .Set(TEXT("known_wsi"), bKnownWsi).Set(TEXT("ssl_input"), bKnownInput)
            .Set(TEXT("store_ssl"), bStoreSsl).Set(TEXT("wsi_user"), bUser)
            .Set(TEXT("preverify"), static_cast<int64>(Len)).ToString();
        Connection.Rig->Record(Connection.Id, Marker, 0, Row);
        UE_LOG(LogVaRestLwsProbe, Display, TEXT("ROUTE-IDENTITY %s"), *Row);
        GLog->Flush();
        return KeepDefault;
    };
    Pending(TEXT("route-first-connect"));
    int32 Active = Rig.Connect(Spec);
    const int32 Initial = Active;
    int32 Neighbor = 0;
    if (bHeld)
    {
        Rig.WatchConnection(Active);
        bHarness = Rig.WaitFor([GateReady](const TArray<FEvent>& All)
        { return IFileManager::Get().FileExists(*GateReady); }, 4.0) && bHarness;
        Rig.RecordSslIdentity(Active, TEXT("ssl-before-route"));
        Fence(Rig, TEXT("identity-before-fence"));
        bHarness = CommandSeen(Rig, TEXT("identity-before-fence")) && bHarness;
        if (bHarness && bLate)
        {
            Pending(TEXT("route-late-create-live-ssl"));
            NextRoute = Rig.AddRoute(MakeRoute(TEXT("route-late"), NextProxy, 0));
        }
        if (bHarness && !bLate)
        {
            FConnectSpec NeighborSpec = Spec;
            NeighborSpec.RouteId = NextRoute;
            NeighborSpec.Path = TEXT("/ws/echo?key=") + CaseId + TEXT("-neighbor");
            Neighbor = Rig.Connect(MoveTemp(NeighborSpec));
            const bool bNeighborPrimary = Primary(Rig, Neighbor);
            Rig.Record(Neighbor, Marker, bNeighborPrimary && Opened(Rig.Events(Neighbor)) ? 1 : 0, TEXT("neighbor-open-held"));
            if (Opened(Rig.Events(Neighbor)))
            {
                Rig.RecordSslIdentity(Neighbor, TEXT("neighbor-ssl-open"));
                Fence(Rig, TEXT("neighbor-ssl-fence"));
                bHarness = CommandSeen(Rig, TEXT("neighbor-ssl-fence")) && bHarness;
                Rig.Close(Neighbor, 1000, TEXT("neighbor-done"));
            }
            const bool bNeighborRetired = Retired(Rig, Neighbor);
            Rig.Record(Neighbor, Marker, bNeighborRetired ? 1 : 0, TEXT("neighbor-retired-held"));
            bHarness = bNeighborRetired && bHarness;
        }
        Fence(Rig, TEXT("late-route-fence"));
        bHarness = CommandSeen(Rig, TEXT("late-route-fence")) && bHarness;
        Rig.RecordSslIdentity(Active, TEXT("ssl-after-route"));
        Fence(Rig, TEXT("identity-after-fence"));
        bHarness = CommandSeen(Rig, TEXT("identity-after-fence")) && bHarness;
        if (bHarness)
        {
            Rig.Record(Active, Marker, 0, TEXT("hold-release-request"));
            GLog->Flush();
            bHarness = FFileHelper::SaveStringToFile(TEXT("release\n"), *GateRelease,
                FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
        }
    }
    const bool bFirstPrimary = Primary(Rig, Active);
    bool bRetiredBeforeFallback = false;
    if (bFallback && !Opened(Rig.Events(Active)))
    {
        bRetiredBeforeFallback = Retired(Rig, Active);
        Rig.Record(Active, Marker, bRetiredBeforeFallback ? 1 : 0, TEXT("fallback-retired"));
        if (bRetiredBeforeFallback)
        {
            Spec.RouteId = NextRoute;
            Active = Rig.Connect(Spec);
        }
    }
    const bool bFinalPrimary = Active == Initial ? bFirstPrimary : Primary(Rig, Active);
    const bool bOpened = Opened(Rig.Events(Active));
    bool bEchoed = false;
    if (bOpened)
    {
        Rig.RecordSslIdentity(Active, TEXT("ssl-open"));
        Fence(Rig, TEXT("ssl-open-fence"));
        bHarness = CommandSeen(Rig, TEXT("ssl-open-fence")) && bHarness;
        Rig.WriteText(Active, TEXT("route-echo"));
        bEchoed = Rig.WaitFor([Active](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([Active](const FEvent& Item)
            {
                return Item.Conn == Active && Item.Reason == LWS_CALLBACK_CLIENT_RECEIVE
                    && EventText(Item).Equals(TEXT("route-echo"), ESearchCase::CaseSensitive);
            });
        }, 2.0);
    }
    Ownership->Snapshot(Rig, 0, TEXT("cleanup-start"));
    if (Mode.Equals(TEXT("destroy-active"), ESearchCase::CaseSensitive))
    {
        Pending(TEXT("route-destroy-active"));
        Rig.DestroyRoute(Spec.RouteId);
        Fence(Rig, TEXT("active-route-destroyed"));
        bHarness = CommandSeen(Rig, TEXT("active-route-destroyed")) && bHarness;
    }
    else
    {
        Ownership->Snapshot(Rig, 1, TEXT("vhost-skipped-before"));
        Ownership->Snapshot(Rig, 2, TEXT("vhost-skipped-after"));
        if (bOpened) { Rig.Close(Active, 1000, TEXT("route-done")); }
    }
    Ownership->Snapshot(Rig, 3, TEXT("retirement-before"));
    const bool bRetired = Retired(Rig, Active);
    Ownership->Snapshot(Rig, 4, TEXT("retirement-after"));
    Ownership->Snapshot(Rig, 5, TEXT("shutdown-before"));
    Rig.Record(0, Marker, 0, TEXT("shutdown-begin"));
    Rig.Shutdown();
    Rig.Record(0, Marker, 0, TEXT("shutdown-end"));
    Ownership->Snapshot(Rig, 6, TEXT("shutdown-after"));
    const int32 FreesAfterContext = Ownership->Frees.load();
    Ownership->Snapshot(Rig, 7, TEXT("caller-before"));
    if (Provided && FreesAfterContext == 0) { SSL_CTX_free(Provided); }
    Provided = nullptr;
    Ownership->Snapshot(Rig, 8, TEXT("caller-after"));
    const int32 FreesAfterCaller = Ownership->Frees.load();
    FVaRestJson Events = FVaRestJson::Array();
    for (const FEvent& Item : Rig.Events()) { Events.Add(Encode(Item)); }
    const FVaRestJson Output = FVaRestJson::Object().Set(TEXT("schema"), 1).Set(TEXT("case"), CaseId)
        .Set(TEXT("mode"), Mode).Set(TEXT("tls"), bTls).Set(TEXT("provided"), bProvided)
        .Set(TEXT("first_route"), FirstRoute).Set(TEXT("next_route"), NextRoute)
        .Set(TEXT("first_connection"), Initial).Set(TEXT("final_connection"), Active)
        .Set(TEXT("neighbor_connection"), Neighbor).Set(TEXT("selected_route"), Spec.RouteId)
        .Set(TEXT("harness_ok"), bHarness).Set(TEXT("first_primary"), bFirstPrimary)
        .Set(TEXT("final_primary"), bFinalPrimary).Set(TEXT("opened"), bOpened).Set(TEXT("echoed"), bEchoed)
        .Set(TEXT("retired_before_fallback"), bRetiredBeforeFallback).Set(TEXT("retired_before_shutdown"), bRetired)
        .Set(TEXT("final_ctx_destructions"), Ownership->Export())
        .Set(TEXT("frees_after_context"), FreesAfterContext).Set(TEXT("frees_after_caller"), FreesAfterCaller)
        .Set(TEXT("shutdown_completed"), true).Set(TEXT("dropped"), Rig.DroppedEventCount()).Set(TEXT("events"), Events);
    TestTrue(TEXT("trace persisted after joined teardown"), FFileHelper::SaveStringToFile(Output.ToString(), *TracePath,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestEqual(TEXT("no recorder loss"), Rig.DroppedEventCount(), 0);
    TestTrue(TEXT("gate/fence acquisition"), bHarness);
    NoteOutcome(*this, TEXT("route open"), bOpened);
    NoteOutcome(*this, TEXT("route echo"), bEchoed);
    // Route/SSL/init/refusal outcomes are measurements. No guessed safety assertion.
    return true;
}
#endif
```

The neighbor uses the separate ungated NextProxy in the same context. The parent gate retains its12second fixture budget and never substitutes stop for release. All identity tokens are bounded integers; raw addresses and secrets are never exported.

- [ ] **Step 4: implement the controller and independent validator before accepting this task.** The remaining steps belong to this same Task2. Its native acquisition, parent controller, validator, passport and named restoring mutations form one independently accepted deliverable; Budget and old rig controls alone do not accept new routing.

**Additional owned files:** Create `Tools/run_lws_route_probe.py`, `Tools/lws_route_passport.py`, `docs/passport/2026-10-23-lws30-route-probe.md`; modify Task1 `Tools/testserver/route_proxy.py` only for the exact inspect_tls extension below. No runner proxy-environment exception is needed.

**Produced artifacts:** immutable matrix, result, trace when completed, flushed editor log, parent proxy/source/target/neighbor journals and SHA256 manifest. Raw acquisition suitability is independent of library positive/negative outcomes.

- [ ] **Step 5: write validator tests and initial RED.** Implement the validator file below first with `validate_case` temporarily returning `[]`; `--selftest` must fail all named malformed/drop/gate/lifecycle tests. Replace that stub with the complete implementation below for GREEN.

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import argparse
import errno
import copy
import hashlib
import json
import math
import os
import pathlib
import tempfile
import unittest

MODES = ('precreated', 'fallback', 'held', 'held-reuse', 'destroy-active', 'untrusted', 'redirect')
CASES = tuple((mode + '-' + scheme + '-' + provider, mode, scheme == 'wss', provider == 'provided', 'v4')
              for mode in MODES for scheme in ('ws', 'wss') for provider in ('generated', 'provided')
              if (scheme == 'wss' or (mode in ('precreated', 'fallback', 'destroy-active', 'redirect') and provider == 'generated')))
CASES += tuple(('ipv6-' + form + '-' + scheme, 'precreated', scheme == 'wss', False, form)
               for form in ('bracket-inline', 'bare-port', 'bracket-port') for scheme in ('ws', 'wss'))

def definition(name):
    for ident, mode, tls, provided, form in CASES:
        if ident == name:
            return dict(case=ident, mode=mode, tls=tls, provided=provided, form=form)
    raise ValueError('unknown immutable case')

def read_object(path):
    value = json.loads(path.read_text(encoding='utf-8'))
    if not isinstance(value, dict):
        raise ValueError('root must be object: ' + path.name)
    return value

def typed_events(trace):
    if not isinstance(trace, dict):
        raise ValueError('trace root')
    rows = trace.get('events')
    if not isinstance(rows, list) or not rows:
        raise ValueError('trace events')
    for row in rows:
        if not isinstance(row, dict):
            raise ValueError('event object')
        for name in ('seq', 'conn', 'wsi', 'reason', 'len', 'depth', 'x509_error', 'preverify', 'flags'):
            if type(row.get(name)) is not int:
                raise ValueError('event integer: ' + name)
        if type(row.get('input_present')) is not bool or not isinstance(row.get('note'), str) or not isinstance(row.get('name'), str):
            raise ValueError('event input/note')
        if not isinstance(row.get('hex'), str) or len(row['hex']) % 2:
            raise ValueError('event hex')
        bytes.fromhex(row['hex'])
        if type(row.get('time')) not in (int, float) or not math.isfinite(row['time']):
            raise ValueError('event time')
    if [e['seq'] for e in rows] != list(range(1, len(rows) + 1)):
        raise ValueError('trace sequence')
    return rows

def notes(rows, phase):
    found = []
    for event in rows:
        if not event['note'].startswith('{'):
            continue
        row = json.loads(event['note'])
        if not isinstance(row, dict):
            raise ValueError('phase note object')
        if row.get('phase') == phase:
            found.append((event['seq'], row))
    return found

def lifecycle_index(rows):
    # A conn0 CREATE may precede association; repeated CREATE does not mean a new token.
    associated = {e['wsi'] for e in rows if e['conn'] > 0 and e['wsi'] > 0}
    intervals, problems = {}, []
    for token in associated:
        events = [e for e in rows if e['wsi'] == token]
        owners = {e['conn'] for e in events if e['conn'] > 0}
        creates = [e for e in events if e['name'] == 'WSI_CREATE']
        destroys = [e for e in events if e['name'] == 'WSI_DESTROY']
        if len(owners) != 1 or not creates or len(destroys) != 1:
            problems.append('token ownership/create/destroy'); continue
        born, dead = min(e['seq'] for e in creates), destroys[0]['seq']
        owner = next(iter(owners))
        linked = min(e['seq'] for e in events if e['conn'] == owner)
        if born >= dead or linked < born or any(e['seq'] >= dead for e in creates):
            problems.append('destroy precedes create/association'); continue
        if any(e['name'] != 'WSI_DESTROY' and not born <= e['seq'] < dead for e in events):
            problems.append('callback outside token lifetime'); continue
        intervals[token] = dict(conn=owner, born=born, linked=linked, dead=dead)
    return intervals, problems

def lifecycle(rows):
    return lifecycle_index(rows)[1]

def live_witness(value, seq, intervals):
    token, conn = value.get('wsi_token'), value.get('conn')
    if type(token) is not int or type(conn) is not int:
        return ['identity lifecycle field type']
    life = intervals.get(token)
    if life is None or life['conn'] != conn:
        return ['identity token not associated with connection']
    if not life['born'] < seq < life['dead']:
        return ['identity outside associated live interval']
    return []

CAPABILITY_ERRNOS = {errno.EAFNOSUPPORT, errno.EPROTONOSUPPORT, errno.EADDRNOTAVAIL}

def immutable(result):
    if not isinstance(result, dict):
        raise ValueError('result object')
    expected = definition(result.get('case'))
    for key in ('mode', 'tls', 'provided', 'form'):
        if type(result.get(key)) is not type(expected[key]) or result[key] != expected[key]:
            raise ValueError('immutable ' + key)
    return expected

def unsupported(result, evidence):
    expected = immutable(result)
    if result.get('unsupported_ipv6') is not True or expected['form'] == 'v4':
        raise ValueError('unsupported requires an IPv6 case')
    if (not isinstance(evidence, dict) or evidence.get('operation') != 'bind-ipv6-loopback'
            or evidence.get('family') != 'AF_INET6' or evidence.get('host') != '::1'
            or type(evidence.get('errno')) is not int or evidence['errno'] not in CAPABILITY_ERRNOS
            or result.get('errno') != evidence['errno'] or result.get('group_drained') is not True):
        raise ValueError('unsupported capability evidence')

def index_state(obs):
    if not isinstance(obs, dict) or type(obs.get('known')) is not bool:
        raise ValueError('index observation type')
    if obs['known']:
        if type(obs.get('value')) is not int:
            raise ValueError('known index value type')
        return obs['value']
    if 'value' in obs:
        raise ValueError('unavailable index invented value')
    return None

def identity(value, tls, supplied, route=None):
    problems = []
    for key in ('conn', 'route', 'actual_route', 'wsi_token', 'ssl_token'):
        if type(value.get(key)) is not int or value[key] < 0:
            problems.append('identity integer ' + key)
    for key in ('ssl_exists', 'wsi_user', 'ctx_exists', 'ctx_supplied', 'index_known', 'index_matches_wsi'):
        if type(value.get(key)) is not bool:
            problems.append('identity bool ' + key)
    if value.get('ctx_origin') not in ('none', 'supplied', 'other'):
        problems.append('identity ctx_origin')
    try:
        index_state(value.get('index'))
    except ValueError as error:
        problems.append(str(error))
    if problems:
        return problems
    if value['conn'] <= 0 or value['wsi_token'] <= 0:
        problems.append('identity lacks owned live token')
    if not value['wsi_user'] or (route is not None and (value['actual_route'] != route or value['route'] != route)):
        problems.append('observed: actual route/user mismatch')
    if tls and (not value['ssl_exists'] or value['ssl_token'] <= 0 or not value['ctx_exists']):
        problems.append('observed: missing live SSL/context')
    if value['index_known'] != value['index']['known']:
        problems.append('identity index availability inconsistent')
    if value['ctx_supplied'] != (value['ctx_origin'] == 'supplied'):
        problems.append('identity origin inconsistent')
    if supplied and tls and not value['ctx_supplied']:
        problems.append('observed: supplied context not used')
    return problems

def provenance(rows, trace):
    """One evidence consumer for validation AND every ownership summary, including non-open TLS."""
    intervals, problems = lifecycle_index(rows)
    valid, matched_certs, matched_opens, ssl_owners = [], {}, {}, {}
    phases = ('ssl-before-route', 'ssl-after-route', 'ssl-open', 'neighbor-ssl-open', 'verify-identity')
    records = [(seq, value, phase) for phase in phases for seq, value in notes(rows, phase)]
    for phase in ('create-before', 'create-after', 'tls-init-before', 'tls-init-after'):
        for seq, value in notes(rows, phase):
            watch = value.get('watch')
            if not isinstance(watch, dict):
                problems.append('operation live watch absent'); continue
            # An explicit no-watch record makes no ownership or live-SSL claim.
            if (type(watch.get('conn')) is int and watch['conn'] == 0
                    and type(watch.get('wsi_token')) is int and watch['wsi_token'] == 0
                    and type(watch.get('ssl_token')) is int and watch['ssl_token'] == 0
                    and watch.get('ssl_exists') is False):
                continue
            records.append((seq, watch, 'watch'))
    for seq, value, phase in sorted(records, key=lambda row: row[0]):
        errors = identity(value, trace['tls'], trace['provided'])
        errors.extend(live_witness(value, seq, intervals))
        route = value.get('route')
        if type(route) is not int or route not in (trace['first_route'], trace['next_route']):
            errors.append('identity undeclared route')
        conn = value.get('conn')
        expected_route = (trace['selected_route'] if conn == trace['final_connection'] else
                          trace['next_route'] if conn == trace['neighbor_connection'] and conn > 0 else
                          trace['first_route'] if conn == trace['first_connection'] else None)
        if expected_route is None or route != expected_route:
            errors.append('identity connection route mismatch')
        if value.get('actual_route') != route:
            errors.append('observed: actual route/user mismatch')
        if (value.get('ctx_exists') != value.get('ssl_exists')
                or value.get('ctx_exists') != (value.get('ctx_origin') in ('supplied', 'other'))):
            errors.append('identity context existence inconsistent')
        if value.get('ssl_exists') is False and value.get('ssl_token') != 0:
            errors.append('identity absent SSL token')
        if value.get('index_matches_wsi') is True and (value.get('index_known') is not True or value.get('ssl_exists') is not True):
            errors.append('identity index match without observation')
        event = rows[seq-1]
        if phase != 'watch' and event['conn'] != value.get('conn'):
            errors.append('identity marker connection mismatch')
        if phase in ('ssl-open', 'neighbor-ssl-open'):
            opens = [e for e in rows if e['conn'] == value.get('conn') and e['name'] == 'ESTABLISHED']
            if len(opens) != 1 or opens[0]['seq'] >= seq or opens[0]['wsi'] != value.get('wsi_token'):
                errors.append('identity actual open association')
            else:
                matched_opens.setdefault(opens[0]['seq'], []).append(seq)
        if phase == 'verify-identity':
            # The native hook records this note after Handle appended the actual certificate callback.
            # Match the latest preceding WSI callback of this connection, then enforce one note per callback.
            previous = [e for e in rows if e['seq'] < seq and e['conn'] == value.get('conn') and e['wsi'] > 0]
            cert = previous[-1] if previous else None
            if (cert is None or cert['name'] != 'PERFORM_SERVER_CERT_VERIFICATION'
                    or cert['wsi'] != value.get('wsi_token') or type(value.get('preverify')) is not int
                    or value['preverify'] != cert['preverify']):
                errors.append('identity actual verification association')
            else:
                matched_certs.setdefault(cert['seq'], []).append(seq)
            if any(value.get(k) is not True for k in ('known_wsi', 'ssl_input', 'store_ssl', 'wsi_user')):
                errors.append('observed: verify association mismatch')
        if value.get('ssl_exists') is True and type(value.get('ssl_token')) is int:
            token = value['ssl_token']
            owner = (value.get('conn'), value.get('wsi_token'), route, value.get('ctx_origin'))
            if token in ssl_owners and ssl_owners[token] != owner:
                errors.append('SSL token ownership/origin changed')
            ssl_owners[token] = owner
        problems.extend(errors)
        if not errors and value.get('ssl_exists') is True:
            valid.append(value)
    for cert in (e for e in rows if e['name'] == 'PERFORM_SERVER_CERT_VERIFICATION'):
        if len(matched_certs.get(cert['seq'], [])) != 1:
            problems.append('certificate callback requires one associated provenance record')
    for opened in (e for e in rows if e['name'] == 'ESTABLISHED'):
        if len(matched_opens.get(opened['seq'], [])) != 1:
            problems.append('open callback requires one associated provenance record')
    # Any malformed/unassociated record invalidates the aggregate ownership claim, even if another note is valid.
    used = not problems and bool(valid) and all(v['ctx_supplied'] and v['ctx_origin'] == 'supplied' for v in valid)
    return dict(errors=problems, supplied_used=used, live_records=len(valid))

def operations(rows):
    result, problems = [], []
    routes = sorted({v.get('route') for _, v in notes(rows, 'create-before') if type(v.get('route')) is int})
    for route in routes:
        created = None
        for op in ('create', 'tls-init'):
            before = [(n, v) for n, v in notes(rows, op + '-before') if v.get('route') == route]
            after = [(n, v) for n, v in notes(rows, op + '-after') if v.get('route') == route]
            skipped = [(n, v) for n, v in notes(rows, 'tls-init-skipped') if v.get('route') == route]
            if op == 'tls-init' and created is False:
                if before or after or len(skipped) != 1:
                    problems.append('failed create requires explicit skipped init')
                result.append(dict(route=route, operation=op, status='skipped', motion='unobserved'))
                continue
            if len(before) != 1 or len(after) != 1 or before[0][0] >= after[0][0]:
                problems.append('incomplete operation %d/%s' % (route, op))
                result.append(dict(route=route, operation=op, status='partial', motion='unobserved'))
                continue
            left, right = before[0][1], after[0][1]
            if type(left.get('value')) is not bool or type(right.get('value')) is not bool:
                problems.append('operation return type')
            try:
                start, end = index_state(left.get('index')), index_state(right.get('index'))
                motion = 'unavailable' if start is None or end is None else 'changed' if start != end else 'unchanged'
            except ValueError as error:
                problems.append(str(error)); motion = 'malformed'
            if op == 'create':
                created = right.get('value')
            result.append(dict(route=route, operation=op, status='returned-success' if right.get('value') is True else 'returned-failure',
                               motion=motion, before=before[0][0], after=after[0][0]))
    if not routes:
        problems.append('no route operation records')
    return result, problems

def ownership(rows, trace, evidence):
    stages = notes(rows, 'ownership')
    problems = []
    if [v.get('stage') for _, v in stages] != list(range(9)):
        problems.append('ownership stage chain')
    labels=['cleanup-start','vhost-before' if trace['mode']=='destroy-active' else 'vhost-skipped-before',
            'vhost-after' if trace['mode']=='destroy-active' else 'vhost-skipped-after','retirement-before',
            'retirement-after','shutdown-before','shutdown-after','caller-before','caller-after']
    if [v.get('label') for _,v in stages] != labels:
        problems.append('ownership stage labels')
    if len(stages)==9:
        begins=[e['seq'] for e in rows if e['note']=='shutdown-begin']
        ends=[e['seq'] for e in rows if e['note']=='shutdown-end']
        if len(begins)!=1 or len(ends)!=1 or not stages[5][0] < begins[0] < ends[0] < stages[6][0]:
            problems.append('joined destruction stage window')
        if trace['mode']=='destroy-active':
            before,after=notes(rows,'destroy-before'),notes(rows,'destroy-after')
            if len(before)!=1 or len(after)!=1 or not before[0][0] < stages[1][0] < stages[2][0] < after[0][0] < stages[3][0]:
                problems.append('vhost destruction stage window')
    counts = [v.get('frees') for _, v in stages]
    if any(type(n) is not int or n < 0 for n in counts) or counts != sorted(counts):
        problems.append('ownership count monotonicity')
    destroyed = trace.get('final_ctx_destructions')
    if not isinstance(destroyed, list) or any(not isinstance(d, dict) or type(d.get('stage')) is not int
            or not 0 <= d['stage'] <= 8 or type(d.get('count')) is not int for d in destroyed):
        return problems + ['final context destruction records'], 'malformed'
    if [d['count'] for d in destroyed] != list(range(1, len(destroyed) + 1)):
        problems.append('final context destruction count chain')
    if [d['stage'] for d in destroyed] != sorted(d['stage'] for d in destroyed):
        problems.append('final context destruction stage order')
    if len(stages) == 9 and not problems:
        if counts[6] != trace['frees_after_context'] or counts[8] != trace['frees_after_caller'] or counts[8] != len(destroyed):
            problems.append('ownership final totals')
        for (_, value) in stages:
            lower = sum(d['stage'] < value['stage'] for d in destroyed)
            upper = sum(d['stage'] <= value['stage'] for d in destroyed)
            if not lower <= value['frees'] <= upper:
                problems.append('ownership snapshot/destruction mismatch')
    used = evidence['supplied_used'] and not problems
    if not trace['provided']:
        label = 'generated context; no supplied ownership claim'
    elif not used:
        label = 'supplied use unestablished'
    elif not destroyed:
        label = 'missing final destruction'
    elif len(destroyed) > 1:
        label = 'multiple final destructions'
    elif destroyed[0]['stage'] < 5:
        label = 'early final destruction at stage %d' % destroyed[0]['stage']
    elif destroyed[0]['stage'] == 5:
        label = 'final destruction during context shutdown'
    elif destroyed[0]['stage'] == 7 and trace['frees_after_context'] == 0:
        label = 'exercised supplied context finally destroyed by caller'
    else:
        label = 'late/unexpected final destruction stage %d' % destroyed[0]['stage']
    return problems, label

def header_values(journal):
    requests = [e for e in journal['events'] if e.get('ev') == 'request']
    headers = []
    for request in requests:
        pairs = request.get('headers')
        if not isinstance(pairs, list) or any(not isinstance(p, list) or len(p) != 2
                or not all(isinstance(v, str) for v in p) for p in pairs):
            raise ValueError('target header shape')
        headers.append({k.lower(): v for k, v in pairs})
    return headers

def peer_proof(result, relay, journal, peer, tls):
    problems = []
    connects = [e for e in relay['events'] if e['ev'] == 'connect']
    hellos = [e for e in relay['events'] if e['ev'] == 'client_hello']
    if result['mode'] == 'redirect':
        connects, hellos = connects[:1], hellos[:1]
    if not connects:
        problems.append('required proxy stimulus absent')
    for event in connects:
        if (event.get('authority') != 'localhost:%d' % result['target_port']
                or event.get('peer_host') != peer['host'] or event.get('peer_port') != peer['port']):
            problems.append('observed: actual proxy peer/CONNECT mismatch')
    if tls and (not hellos or any(e.get('sni') != 'localhost' for e in hellos)):
        problems.append('required named TLS ClientHello absent')
    if journal is not None:
        try:
            headers = header_values(journal)
            if not headers:
                problems.append('target Upgrade witness absent')
            for header in headers:
                if header.get('host') != 'localhost:%d' % result['target_port'] or header.get('authorization') != 'Bearer route-probe-marker':
                    problems.append('observed: target Host/credential mismatch')
        except ValueError as error:
            problems.append(str(error))
    return problems

def connection_proof(rows, conn, route, phase, tls, supplied):
    problems = []
    opened = [e for e in rows if e['conn'] == conn and e['name'] == 'ESTABLISHED']
    witnesses = [(seq, v) for seq, v in notes(rows, phase) if v.get('conn') == conn]
    if len(opened) != 1 or len(witnesses) != 1:
        return ['independent open/SSL witness absent']
    seq, witness = witnesses[0]
    problems.extend(identity(witness, tls, supplied, route))
    if seq <= opened[0]['seq']:
        problems.append('SSL open witness precedes open')
    if tls:
        certs = [e for e in rows if e['conn'] == conn and e['name'] == 'PERFORM_SERVER_CERT_VERIFICATION']
        if not any(e['depth'] == 0 and e['preverify'] == 1 and e['x509_error'] == 0 for e in certs):
            problems.append('verified leaf absent')
        checks = [v for _, v in notes(rows, 'verify-identity') if v.get('conn') == conn]
        if not checks:
            problems.append('verify identity absent')
        for check in checks:
            problems.extend(identity(check, True, supplied, route))
            if any(check.get(k) is not True for k in ('known_wsi', 'ssl_input', 'store_ssl', 'wsi_user')):
                problems.append('observed: verify association mismatch')
            if any(check.get(k) != witness.get(k) for k in ('wsi_token', 'ssl_token', 'actual_route')):
                problems.append('verify/open identity differs')
    return problems

def retirement_stages(rows, conn):
    tokens = {e['wsi'] for e in rows if e['conn'] == conn and e['wsi'] > 0}
    before = notes(rows, 'destroy-before'); after = notes(rows, 'destroy-after')
    shutdown = [e['seq'] for e in rows if e['note'] == 'shutdown-begin']
    result = []
    for token in sorted(tokens):
        endings = [e['seq'] for e in rows if e['wsi'] == token and e['name'] == 'WSI_DESTROY']
        end = endings[0] if len(endings) == 1 else None
        label = 'missing/duplicate'
        if end is not None and shutdown:
            if before and end < before[0][0]: label = 'before destroy request'
            elif after and end <= after[0][0]: label = 'during vhost destroy'
            elif end < shutdown[0]: label = 'during retirement wait'
            else: label = 'only during context shutdown'
        result.append(dict(token=token, destroy_seq=end, stage=label))
    return result

def validate_case(result, trace, relay_a, relay_b, source, target, neighbor):
    try:
        expected = immutable(result)
    except (ValueError, TypeError) as error:
        return [str(error)]
    problems = []
    if result.get('group_drained') is not True: problems.append('group not drained')
    if type(result.get('runner_rc')) is not int: problems.append('runner rc missing')
    if type(result.get('target_port')) is not int or not 1 <= result['target_port'] <= 65535: problems.append('target port')
    if not isinstance(result.get('gate'), dict): problems.append('gate shape')
    if not isinstance(result.get('errors'), list) or result['errors']: problems.append('parent acquisition errors')
    for key in ('peer_a', 'peer_b'):
        peer = result.get(key)
        if not isinstance(peer, dict) or peer.get('host') not in ('127.0.0.1', '::1') or type(peer.get('port')) is not int or not 1 <= peer['port'] <= 65535:
            problems.append('peer endpoint type')
    for relay in (relay_a, relay_b):
        if not isinstance(relay, dict) or not isinstance(relay.get('events'), list):
            problems.append('relay shape'); continue
        if type(relay.get('dropped')) is not int or relay['dropped'] != 0 or relay.get('active') != 0:
            problems.append('relay loss/live')
        for event in relay['events']:
            if not isinstance(event, dict) or not isinstance(event.get('ev'), str):
                problems.append('relay event shape'); continue
            if event['ev'] in ('fixture_error', 'hold_deadline', 'hold_stopped', 'stopped', 'relay_deadline'):
                problems.append('fixture stimulus failed')
            if event['ev'] == 'connect':
                headers = event.get('headers')
                if not isinstance(headers, list) or any(not isinstance(p, list) or len(p) != 2
                        or not all(isinstance(v, str) for v in p) for p in headers):
                    problems.append('CONNECT header shape')
                elif any(p[0].lower() == 'authorization' for p in headers): problems.append('credential in CONNECT')
    for journal in (source, target, neighbor):
        if not isinstance(journal, dict) or not isinstance(journal.get('events'), list):
            problems.append('target journal shape')
        elif type(journal.get('dropped')) is not int or journal['dropped'] != 0 or type(journal.get('connections')) is not int or any(not isinstance(e, dict) for e in journal['events']):
            problems.append('target journal loss/type')
    if problems: return problems
    if trace is None: return ['no final trace; partial/crashed acquisition']
    try: rows = typed_events(trace)
    except (ValueError, TypeError, OverflowError) as error: return [str(error)]
    for key in ('case', 'mode', 'tls', 'provided'):
        if type(trace.get(key)) is not type(expected[key]) or trace[key] != expected[key]: problems.append('trace ' + key)
    for key in ('harness_ok', 'first_primary', 'final_primary', 'opened', 'echoed', 'retired_before_fallback', 'retired_before_shutdown', 'shutdown_completed'):
        if type(trace.get(key)) is not bool: problems.append('trace bool ' + key)
    for key in ('frees_after_context', 'frees_after_caller', 'first_connection', 'final_connection', 'neighbor_connection', 'first_route', 'next_route', 'selected_route'):
        if type(trace.get(key)) is not int: problems.append('trace integer ' + key)
    if trace.get('schema') != 1 or type(trace.get('dropped')) is not int or trace['dropped'] != 0 or trace.get('shutdown_completed') is not True:
        problems.append('trace schema/loss/shutdown')
    if trace.get('harness_ok') is not True or result['runner_rc'] != 0: problems.append('acquisition failed')
    if problems: return problems
    evidence = provenance(rows, trace); problems.extend(evidence['errors'])
    ops, op_errors = operations(rows); problems.extend(op_errors)
    if {op['route'] for op in ops} != {trace['first_route'], trace['next_route']}:
        problems.append('declared route operations missing')
    for phase in ('create-before','create-after','tls-init-before','tls-init-after','tls-init-skipped'):
        for _, value in notes(rows, phase):
            if type(value.get('route')) is not int or value['route'] not in (trace['first_route'],trace['next_route']):
                problems.append('operation route identity')
    owner_errors, owner_label = ownership(rows, trace, evidence); problems.extend(owner_errors)
    if trace['final_primary'] is not True: problems.append('primary observation incomplete')
    expected_selected = trace['next_route'] if expected['mode']=='precreated' or (expected['mode']=='fallback' and trace['retired_before_fallback']) else trace['first_route']
    if trace['selected_route'] != expected_selected:
        problems.append('observed: explicit nondefault route selection mismatch')
    route_refused = any(op['route'] == trace['selected_route'] and op['status'] == 'returned-failure' for op in ops)
    if not route_refused or trace['opened']:
        problems.extend(peer_proof(result, relay_a, source if trace['opened'] or expected['mode'] == 'redirect' else None, result['peer_a'], expected['tls']))
    if trace['opened']:
        if not trace['echoed']: problems.append('positive open lacked echo')
        problems.extend(connection_proof(rows, trace['final_connection'], trace['selected_route'], 'ssl-open', expected['tls'], expected['provided']))
    if expected['mode'] == 'untrusted' and (not route_refused or trace['opened']):
        bad = [e for e in rows if e['name'] == 'PERFORM_SERVER_CERT_VERIFICATION' and e['preverify'] == 0 and e['x509_error'] != 0]
        if not bad: problems.append('untrusted failed-certificate stimulus absent')
        if trace['opened']: problems.append('observed: untrusted peer opened')
    if expected['mode'] == 'redirect' and not route_refused:
        if not any(e.get('ev') == 'response' and e.get('status') == 302 for e in source['events']): problems.append('redirect response stimulus absent')
    if expected['mode'] == 'fallback' and trace['opened']:
        markers = [e for e in rows if e['note'] == 'fallback-retired']
        second = [e for e in rows if e['conn'] == trace['final_connection'] and e['name'] == 'connect>']
        tokens = {e['wsi'] for e in rows if e['conn'] == trace['first_connection'] and e['wsi'] > 0}
        if not markers or markers[0]['len'] != 1 or not second or markers[0]['seq'] >= second[0]['seq'] or not trace['retired_before_fallback']:
            problems.append('fallback before retirement')
        elif any(e['seq'] >= second[0]['seq'] for e in rows if e['wsi'] in tokens and e['name'] == 'WSI_DESTROY'):
            problems.append('fallback before actual WSI destruction')
    if expected['mode'] in ('held', 'held-reuse'):
        kinds = [e['ev'] for e in relay_a['events']]
        if not all(k in kinds for k in ('client_hello', 'tls_hold', 'tls_release')):
            problems.append('missing held TLS stimulus')
        elif not kinds.index('client_hello') < kinds.index('tls_hold') < kinds.index('tls_release'):
            problems.append('TLS stimulus order')
        if result['gate'].get('ready_after_hold') is not True or result['gate'].get('release_after_child') is not True:
            problems.append('parent gate order')
        before, after = notes(rows, 'ssl-before-route'), notes(rows, 'ssl-after-route')
        release = [e['seq'] for e in rows if e['note'] == 'hold-release-request']
        if len(before) != 1 or len(after) != 1 or len(release) != 1 or not before[0][0] < after[0][0] < release[0]:
            problems.append('SSL hold witnesses')
        else:
            for _, witness in before + after:
                problems.extend(identity(witness, True, expected['provided'], trace['first_route']))
            keys = ('conn', 'wsi_token', 'ssl_token', 'route', 'actual_route')
            if before[0][1].get('conn') != trace['first_connection'] or any(before[0][1].get(k) != after[0][1].get(k) for k in keys):
                problems.append('held original identity changed')
            if expected['mode'] == 'held':
                late = [op for op in ops if op['route'] == trace['next_route']]
                for op in late:
                    if op['status'] != 'skipped' and not (before[0][0] < op.get('before', 0) < op.get('after', 0) < after[0][0]):
                        problems.append('late operation outside held SSL window')
                if len(late) != 2: problems.append('late create/init operations missing')
                for phase in ('create-before', 'create-after', 'tls-init-before', 'tls-init-after'):
                    for _, value in notes(rows, phase):
                        if value.get('route') != trace['next_route']: continue
                        watch = value.get('watch')
                        if not isinstance(watch, dict): problems.append('operation live watch absent'); continue
                        problems.extend(identity(watch, True, expected['provided'], trace['first_route']))
                        if any(watch.get(k) != before[0][1].get(k) for k in keys): problems.append('operation watched SSL differs')
            else:
                conn = trace['neighbor_connection']
                problems.extend(connection_proof(rows, conn, trace['next_route'], 'neighbor-ssl-open', True, expected['provided']))
                problems.extend(peer_proof(result, relay_b, neighbor, result['peer_b'], True))
                related = [e for e in rows if e['conn'] == conn]
                ends = [e for e in related if e['name'] == 'WSI_DESTROY']
                opens = [e for e in related if e['name'] == 'ESTABLISHED']
                marker = [e for e in related if e['note'] == 'neighbor-retired-held']
                tokens = {e['wsi'] for e in related if e['wsi'] > 0}
                all_ends = [e for e in rows if e['wsi'] in tokens and e['name'] == 'WSI_DESTROY']
                if not opens or not ends or not marker or marker[0]['len'] != 1 or not all_ends or any(not before[0][0] < e['seq'] < after[0][0] for e in related + all_ends):
                    problems.append('neighbor outside original held window')
                for phase in ('neighbor-ssl-open','verify-identity'):
                    for seq,value in notes(rows,phase):
                        if value.get('conn')==conn and not before[0][0] < seq < after[0][0]:
                            problems.append('neighbor identity outside held window')
    if expected['mode']=='destroy-active':
        beginnings, endings=notes(rows,'destroy-before'),notes(rows,'destroy-after')
        if len(beginnings)!=1 or len(endings)!=1 or beginnings[0][0]>=endings[0][0]:
            problems.append('active vhost destruction stages absent')
        stages=retirement_stages(rows,trace['final_connection'])
        if trace['opened'] and (not stages or any(v['stage']=='before destroy request' for v in stages)):
            problems.append('observed: active teardown had no live target token')
        derived=bool(stages) and all(v['stage']!='only during context shutdown' and v['stage']!='missing/duplicate' for v in stages)
        if stages and trace['retired_before_shutdown'] != derived:
            problems.append('retirement marker disagrees with token stages')
    return problems

def summarize(trace):
    if trace is None: return dict(outcome='partial/crashed; inspect flushed phases', operations=[], ownership='unestablished')
    rows = typed_events(trace)
    ops, errors = operations(rows)
    evidence = provenance(rows, trace)
    owner_errors, label = ownership(rows, trace, evidence)
    return dict(outcome='open+echo' if trace['opened'] and trace['echoed'] else 'did not complete open+echo',
                operations=ops, operation_errors=errors, ownership=label, ownership_errors=owner_errors,
                provenance_errors=evidence['errors'],
                retirements=retirement_stages(rows, trace['final_connection']))

def load_case(folder):
    result = read_object(folder / 'result.json'); immutable(result)
    hashes = result.get('sha256')
    if not isinstance(hashes, dict) or not hashes: raise ValueError('artifact manifest absent')
    required = {'unsupported.json'} if result.get('unsupported_ipv6') is True else {'runner.log', 'editor.log', 'acquisition.json', 'proxy-a.json', 'proxy-b.json', 'source.json', 'target.json', 'neighbor.json'}
    if (folder / 'trace.json').exists(): required.add('trace.json')
    if not required.issubset(hashes): raise ValueError('required artifact missing from manifest')
    for name, digest in hashes.items():
        if pathlib.Path(name).name != name or not isinstance(digest, str): raise ValueError('unsafe artifact path/hash')
        if hashlib.sha256((folder / name).read_bytes()).hexdigest() != digest: raise ValueError('artifact hash mismatch')
    if result.get('unsupported_ipv6') is True:
        unsupported(result, read_object(folder / 'unsupported.json'))
        return result, None, [], dict(outcome='IPv6 capability unavailable', operations=[], ownership='unestablished')
    trace = read_object(folder / 'trace.json') if (folder / 'trace.json').exists() else None
    artifacts = [read_object(folder / name) for name in ('proxy-a.json', 'proxy-b.json', 'source.json', 'target.json', 'neighbor.json')]
    errors = validate_case(result, trace, *artifacts)
    try: summary = summarize(trace)
    except (ValueError, KeyError, TypeError) as error: summary = dict(outcome='malformed trace', error=str(error), operations=[], ownership='unestablished')
    return result, trace, errors, summary

def render(directory, output):
    entries = read_object(directory / 'matrix.json').get('cases')
    if not isinstance(entries, list) or len(entries) != len(CASES): raise ValueError('complete immutable matrix required')
    if {e.get('case') for e in entries if isinstance(e, dict)} != {c[0] for c in CASES}: raise ValueError('matrix identity')
    lines = ['# LWS 3.0 immutable route probe — measured evidence', '',
             'Linux modular editor only. Acquisition completeness, observed differences and production suitability are separate.', '']
    for entry in entries:
        immutable(entry)
        result, trace, errors, summary = load_case(directory / entry['case'])
        if result != entry: raise ValueError('matrix/result disagreement')
        acquisition_errors = [e for e in errors if not e.startswith('observed:')]
        lines += ['## ' + entry['case'], '', 'Acquisition: ' + ('FAILED' if acquisition_errors else 'complete'), '',
                  '```json', json.dumps(dict(validation_errors=errors, summary=summary), sort_keys=True), '```', '']
    lines += ['No production route approval is inferred. Missing private index is unavailable, not unchanged.',
              'Every create/init operation has its own result and index pair; earlier vhosts cannot certify a late operation.',
              'Final SSL_CTX destruction is distinguished from SSL_CTX_free reference decrements.',
              'Root adjudicates negative, unsafe, crashed and unsupported results before B9. W3 and unload-worker obligations remain open.']
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile('w', encoding='utf-8', dir=output.parent, delete=False) as stream:
        temporary = pathlib.Path(stream.name); stream.write('\n'.join(lines) + '\n')
    try: os.replace(temporary, output)
    finally: temporary.unlink(missing_ok=True)

def synthetic(mode='held-reuse', provided=True):
    result = definition(mode + '-wss-' + ('provided' if provided else 'generated'))
    result.update(group_drained=True, runner_rc=0, gate=dict(ready_after_hold=True, release_after_child=True), errors=[],
                  target_port=443, peer_a=dict(host='127.0.0.1', port=5001), peer_b=dict(host='127.0.0.1', port=5002))
    rows = []
    def append(name='marker', conn=0, wsi=0, note='', **extra):
        row = dict(seq=len(rows)+1, conn=conn, wsi=wsi, reason=0, name=name, time=0, len=0, input_present=False,
                   hex='', note=note, depth=-1, x509_error=-1, preverify=-1, flags=0)
        row.update(extra); rows.append(row)
    def ident(conn, route):
        return dict(conn=conn, route=route, actual_route=route, wsi_token=conn+6, ssl_token=conn,
                    ssl_exists=True, wsi_user=True, ctx_exists=True, ctx_supplied=provided,
                    ctx_origin='supplied' if provided else 'other', index_known=True, index_matches_wsi=True,
                    index=dict(known=True, value=5))
    def note(phase, **fields): append(conn=fields.get('conn', 0), note=json.dumps(dict(phase=phase, **fields)))
    def route(number, watch):
        for phase in ('create-before', 'create-after', 'tls-init-before', 'tls-init-after'):
            note(phase, route=number, value=True, index=dict(known=True, value=5), watch=watch)
    empty_watch = dict(conn=0, wsi_token=0, ssl_token=0, ssl_exists=False)
    route(1, empty_watch)
    if mode == 'held-reuse': route(2, empty_watch)
    append('WSI_CREATE', 1, 7); append('connect>', 1)
    note('ssl-before-route', **ident(1, 1))
    if mode == 'held': route(2, ident(1, 1))
    else:
        append('WSI_CREATE', 2, 8); append('connect>', 2)
        append('PERFORM_SERVER_CERT_VERIFICATION', 2, 8, depth=0, preverify=1, x509_error=0)
        note('verify-identity', **ident(2, 2), known_wsi=True, ssl_input=True, store_ssl=True, preverify=1)
        append('ESTABLISHED', 2, 8); note('neighbor-ssl-open', **ident(2, 2))
        append(conn=2, note='neighbor-open-held', len=1)
        append('WSI_DESTROY', 2, 8); append(conn=2, note='neighbor-retired-held', len=1)
    note('ssl-after-route', **ident(1, 1)); append(note='hold-release-request')
    append('PERFORM_SERVER_CERT_VERIFICATION', 1, 7, depth=0, preverify=1, x509_error=0)
    note('verify-identity', **ident(1, 1), known_wsi=True, ssl_input=True, store_ssl=True, preverify=1)
    append('ESTABLISHED', 1, 7); note('ssl-open', **ident(1, 1))
    append('WSI_DESTROY', 1, 7)
    labels=['cleanup-start','vhost-skipped-before','vhost-skipped-after','retirement-before','retirement-after',
            'shutdown-before','shutdown-after','caller-before','caller-after']
    for stage in range(9):
        if stage == 6: append(note='shutdown-end')
        note('ownership', stage=stage, label=labels[stage], frees=int(provided and stage == 8))
        if stage == 5: append(note='shutdown-begin')
    trace = dict(schema=1, case=result['case'], mode=mode, tls=True, provided=provided, events=rows, first_route=1, next_route=2,
                 first_connection=1, final_connection=1, neighbor_connection=2 if mode == 'held-reuse' else 0, selected_route=1,
                 harness_ok=True, first_primary=True, final_primary=True, opened=True, echoed=True, retired_before_fallback=False,
                 retired_before_shutdown=True, shutdown_completed=True, dropped=0, frees_after_context=0,
                 frees_after_caller=int(provided), final_ctx_destructions=[dict(stage=7, count=1)] if provided else [])
    def relay(port, held):
        events = [dict(ev='connect', headers=[], authority='localhost:443', peer_host='127.0.0.1', peer_port=port), dict(ev='client_hello', sni='localhost')]
        if held: events += [dict(ev='tls_hold'), dict(ev='tls_release')]
        return dict(events=events, dropped=0, active=0)
    journal = dict(events=[dict(ev='request', headers=[['Host','localhost:443'],['Authorization','Bearer route-probe-marker']])], dropped=0, connections=1)
    empty = dict(events=[], dropped=0, connections=0)
    return [result, trace, relay(5001, True), relay(5002, False) if mode == 'held-reuse' else dict(events=[], dropped=0, active=0), journal, empty, copy.deepcopy(journal) if mode == 'held-reuse' else copy.deepcopy(empty)]

def change_note(values, phase, edit, first=True):
    for event in values[1]['events']:
        if event['note'].startswith('{'):
            value = json.loads(event['note'])
            if value.get('phase') == phase:
                edit(value); event['note'] = json.dumps(value)
                if first: return

def remove_phase(values, phase):
    values[1]['events'] = [e for e in values[1]['events'] if not e['note'].startswith('{') or json.loads(e['note']).get('phase') != phase]
    for number, event in enumerate(values[1]['events'], 1): event['seq'] = number

def synthetic_untrusted():
    values = synthetic('held')
    values[0].update(definition('untrusted-wss-provided'))
    values[1].update(case=values[0]['case'], mode='untrusted', opened=False, echoed=False)
    for phase in ('ssl-before-route', 'ssl-after-route', 'ssl-open'):
        remove_phase(values, phase)
    rows = values[1]['events']
    rows[:] = [e for e in rows if e['name'] != 'ESTABLISHED' and e['note'] != 'hold-release-request']
    for event in rows:
        if event['name'] == 'PERFORM_SERVER_CERT_VERIFICATION':
            event.update(preverify=0, x509_error=18)
        if event['note'].startswith('{'):
            value = json.loads(event['note'])
            if value.get('phase') == 'verify-identity': value['preverify'] = 0
            if 'watch' in value: value['watch'] = dict(conn=0, wsi_token=0, ssl_token=0, ssl_exists=False)
            event['note'] = json.dumps(value)
    for n, event in enumerate(rows, 1): event['seq'] = n
    values[2]['events'] = [e for e in values[2]['events'] if e['ev'] not in ('tls_hold', 'tls_release')]
    values[4] = dict(events=[], dropped=0, connections=0)
    return values

class ValidatorTest(unittest.TestCase):
    def test_valid_original_and_neighbor(self):
        for mode in ('held', 'held-reuse'):
            self.assertEqual(validate_case(*synthetic(mode)), [])
    def test_neighbor_requires_own_every_witness(self):
        for phase in ('neighbor-ssl-open', 'verify-identity'):
            values = synthetic(); remove_phase(values, phase)
            self.assertTrue(validate_case(*values))
        for field, changed in (('conn',999), ('actual_route',1), ('ssl_token',99), ('ctx_supplied',False)):
            values = synthetic(); change_note(values, 'neighbor-ssl-open', lambda v: v.update({field:changed}))
            self.assertTrue(validate_case(*values))
        for index in (3, 6):
            values = synthetic(); values[index]['events'] = []
            self.assertTrue(validate_case(*values))
        values = synthetic(); values[3]['events'][0]['peer_port'] = 5001
        self.assertTrue(validate_case(*values))
        values = synthetic(); rows=values[1]['events']; opened=next(e for e in rows if e['conn']==2 and e['name']=='ESTABLISHED')
        rows.remove(opened); rows.append(opened)
        for n,e in enumerate(rows,1): e['seq']=n
        self.assertIn('neighbor outside original held window', validate_case(*values))
    def test_operation_pairs_and_original_identity(self):
        for phase in ('create-after','tls-init-before','tls-init-after'):
            values=synthetic('held'); remove_phase(values,phase)
            self.assertTrue(validate_case(*values))
        values=synthetic('held'); change_note(values,'ssl-after-route',lambda v:v.update(ssl_token=77))
        self.assertIn('held original identity changed',validate_case(*values))
        values=synthetic('held'); change_note(values,'tls-init-before',lambda v:v.update(index=dict(known=True)),first=False)
        self.assertIn('known index value type',validate_case(*values))
        for action in ('omit','change'):
            values=synthetic('held')
            for e in values[1]['events']:
                if not e['note'].startswith('{'): continue
                v=json.loads(e['note'])
                if v.get('phase')=='tls-init-before' and v.get('route')==2:
                    if action=='omit': v.pop('watch')
                    else: v['watch']['ssl_token']=999
                    e['note']=json.dumps(v)
            self.assertIn('operation live watch absent' if action=='omit' else 'operation watched SSL differs',validate_case(*values))
    def test_index_motion_attributed_only_to_own_pair(self):
        values=synthetic('held'); change_note(values,'create-after',lambda v:v.update(index=dict(known=True,value=9)))
        ops,_=operations(values[1]['events'])
        self.assertEqual(next(o for o in ops if o['route']==1 and o['operation']=='create')['motion'],'changed')
        self.assertTrue(all(o['motion']=='unchanged' for o in ops if o['route']==2))
        for operation in ('create','tls-init'):
            values=synthetic('held')
            for e in values[1]['events']:
                if e['note'].startswith('{'):
                    v=json.loads(e['note'])
                    if v.get('phase')==operation+'-after' and v.get('route')==2:
                        v['index']['value']=6; e['note']=json.dumps(v)
            ops,_=operations(values[1]['events'])
            self.assertEqual(next(o for o in ops if o['route']==2 and o['operation']==operation)['motion'],'changed')
        values=synthetic('held')
        for phase in ('create-before','create-after','tls-init-before','tls-init-after'):
            change_note(values,phase,lambda v:v.update(index=dict(known=False)),first=False)
        self.assertTrue(all(o['motion']=='unavailable' for o in operations(values[1]['events'])[0]))
    def test_fixture_failure_and_missing_untrusted_stimulus(self):
        values=synthetic(); values[2]['events'].append(dict(ev='fixture_error',kind='ConnectionRefusedError'))
        values[1].update(opened=False,echoed=False)
        self.assertIn('fixture stimulus failed',validate_case(*values))
        for ev in ('hold_deadline','hold_stopped'):
            values=synthetic(); values[3]['events'].append(dict(ev=ev))
            self.assertIn('fixture stimulus failed',validate_case(*values))
        values=synthetic(); values[0].update(definition('untrusted-wss-provided')); values[1].update(case=values[0]['case'],mode='untrusted',opened=False,echoed=False)
        self.assertIn('untrusted failed-certificate stimulus absent',validate_case(*values))
    def test_context_provenance_is_not_free_count(self):
        for factory in (synthetic, synthetic_untrusted):
            original = factory()
            self.assertEqual(validate_case(*original), [])
            self.assertEqual(summarize(original[1])['ownership'], 'exercised supplied context finally destroyed by caller')
            phase = 'verify-identity' if factory == synthetic_untrusted else 'ssl-open'
            for change in ('omit', 'malformed', 'wrong-context', 'missing-context', 'wrong-token', 'wrong-callback', 'omit-record'):
                values = copy.deepcopy(original)
                def edit(value):
                    if change == 'omit': value.pop('ctx_origin')
                    elif change == 'malformed': value['ctx_origin'] = 'forged'
                    elif change == 'wrong-context': value.update(ctx_supplied=False, ctx_origin='other')
                    elif change == 'missing-context': value['ctx_exists'] = False
                    elif change == 'wrong-token': value['wsi_token'] = 999
                    elif change == 'wrong-callback': value['preverify'] = 1
                if change == 'omit-record': remove_phase(values, phase)
                elif change == 'wrong-callback' and factory != synthetic_untrusted:
                    change_note(values, phase, lambda v: v.update(conn=999))
                else: change_note(values, phase, edit)
                self.assertTrue(validate_case(*values), (factory.__name__, change))
                self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished', (factory.__name__, change))
                # Exact restoration is a positive control; no archive is rewritten during actual acquisition.
                values = copy.deepcopy(original)
                self.assertEqual(validate_case(*values), [])
                self.assertEqual(summarize(values[1])['ownership'], 'exercised supplied context finally destroyed by caller')
        values = synthetic_untrusted(); remove_phase(values, 'verify-identity')
        self.assertIn('certificate callback requires one associated provenance record', validate_case(*values))
        self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
    def test_actual_token_lifetime_not_coherent_note_numbers(self):
        values = synthetic('held')
        for event in values[1]['events']:
            if not event['note'].startswith('{'): continue
            value = json.loads(event['note'])
            if value.get('conn') == 1 and 'wsi_token' in value: value['wsi_token'] = 999
            if isinstance(value.get('watch'), dict) and value['watch'].get('conn') == 1:
                value['watch']['wsi_token'] = 999
            event['note'] = json.dumps(value)
        self.assertIn('identity token not associated with connection', validate_case(*values))
        self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
        for where in ('before', 'during-create', 'during-init'):
            values = synthetic('held'); rows = values[1]['events']
            end = next(e for e in rows if e['name'] == 'WSI_DESTROY' and e['wsi'] == 7)
            rows.remove(end)
            phase = {'before':'ssl-before-route', 'during-create':'create-after', 'during-init':'tls-init-after'}[where]
            index = next(i for i,e in enumerate(rows) if e['note'].startswith('{')
                         and json.loads(e['note']).get('phase') == phase
                         and (where == 'before' or json.loads(e['note']).get('route') == 2))
            rows.insert(index, end)
            for n,e in enumerate(rows,1): e['seq'] = n
            self.assertTrue(validate_case(*values), where)
            self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
        for phase in ('ssl-open', 'verify-identity'):
            values = synthetic('held'); rows = values[1]['events']
            name = 'ESTABLISHED' if phase == 'ssl-open' else 'PERFORM_SERVER_CERT_VERIFICATION'
            next(e for e in rows if e['name'] == name)['wsi'] = 999
            self.assertTrue(validate_case(*values), phase)
            self.assertEqual(summarize(values[1])['ownership'], 'supplied use unestablished')
    def test_conn_zero_create_and_repeated_create_remain_valid(self):
        for repeat in (False, True):
            values = synthetic('held'); rows = values[1]['events']
            created = next(e for e in rows if e['name'] == 'WSI_CREATE')
            created['conn'] = 0
            if repeat:
                index = rows.index(created)
                repeated = copy.deepcopy(created); repeated['conn'] = 1
                rows.insert(index+1, repeated)
            # Without repeated CREATE, the certificate callback later supplies independent conn1 association.
            # Its token is stable from conn0 CREATE through DESTROY; notes cannot invent that association.
            for n,e in enumerate(rows,1): e['seq'] = n
            self.assertEqual(validate_case(*values), [])
            self.assertEqual(summarize(values[1])['ownership'], 'exercised supplied context finally destroyed by caller')
    def test_all_emitted_fixture_cutoffs_invalidate_every_relay(self):
        for factory in (lambda: synthetic('held'), synthetic, synthetic_untrusted):
            original = factory()
            self.assertEqual(validate_case(*original), [])
            # The ungated negative row already has real CONNECT, ClientHello and failed verification.
            for relay in (2, 3):
                for name in ('fixture_error', 'hold_deadline', 'hold_stopped', 'stopped', 'relay_deadline'):
                    values = copy.deepcopy(original)
                    values[relay]['events'].append(dict(ev=name))
                    self.assertIn('fixture stimulus failed', validate_case(*values), (factory, relay, name))
            self.assertEqual(validate_case(*original), [])
    def test_final_destruction_stage_chain(self):
        values=synthetic(); remove_phase(values,'ownership')
        self.assertIn('ownership stage chain',validate_case(*values))
        for stage,count in ((1,1),(5,1),(7,2)):
            values=synthetic(); values[1]['final_ctx_destructions']=[dict(stage=stage,count=n) for n in range(1,count+1)]
            values[1]['frees_after_context']=count if stage<6 else 0; values[1]['frees_after_caller']=count
            for e in values[1]['events']:
                if e['note'].startswith('{'):
                    v=json.loads(e['note'])
                    if v.get('phase')=='ownership': v['frees']=count if v['stage']>stage else 0; e['note']=json.dumps(v)
            self.assertEqual(validate_case(*values),[])
            self.assertNotEqual(summarize(values[1])['ownership'],'exercised supplied context finally destroyed by caller')
        values=synthetic(); values[1]['final_ctx_destructions']=[]; values[1]['frees_after_caller']=0
        change_note(values,'ownership',lambda v:v.update(frees=0),first=False)
        self.assertEqual(summarize(values[1])['ownership'],'missing final destruction')
    def test_unsupported_requires_immutable_capability(self):
        evidence=dict(operation='bind-ipv6-loopback',family='AF_INET6',host='::1',errno=errno.EAFNOSUPPORT)
        value=definition('ipv6-bare-port-wss'); value.update(unsupported_ipv6=True,errno=evidence['errno'],group_drained=True)
        unsupported(value,evidence)
        for changed in (dict(case='precreated-wss-generated',mode='precreated',tls=True,provided=False,form='v4'),dict(tls='yes'),dict(mode='held')):
            altered=copy.deepcopy(value); altered.update(changed)
            with self.assertRaises(ValueError): unsupported(altered,evidence)
        altered=copy.deepcopy(evidence); altered['errno']=errno.EMFILE
        value['errno']=errno.EMFILE
        with self.assertRaises(ValueError): unsupported(value,altered)
    def test_lifecycle_and_type_and_output_refusals(self):
        for mutate in ('missing','double','badtype','loss'):
            values=synthetic(); rows=values[1]['events']
            end=next(e for e in rows if e['name']=='WSI_DESTROY')
            if mutate=='missing': rows.remove(end)
            elif mutate=='double': rows.append(copy.deepcopy(end))
            elif mutate=='badtype': rows[0]['wsi']='7'
            else: values[1]['dropped']=1
            for n,e in enumerate(rows,1): e['seq']=n
            self.assertTrue(validate_case(*values))
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); (root/'matrix.json').write_text(json.dumps(dict(cases=[])))
            output=root/'passport.md'; output.write_text('accepted\n')
            with self.assertRaises(ValueError): render(root,output)
            self.assertEqual(output.read_text(),'accepted\n')
    def test_active_retirement_is_not_context_cleanup(self):
        values=synthetic('held'); values[0].update(definition('destroy-active-wss-provided'))
        values[1].update(case=values[0]['case'],mode='destroy-active')
        rows=values[1]['events']; end=next(e for e in rows if e['conn']==1 and e['name']=='WSI_DESTROY')
        rows.remove(end)
        for event in rows:
            if event['note'].startswith('{'):
                v=json.loads(event['note'])
                if v.get('phase')=='ownership' and v.get('stage') in (1,2):
                    v['label']='vhost-before' if v['stage']==1 else 'vhost-after'; event['note']=json.dumps(v)
        for phase,stage in (('destroy-before',1),('destroy-after',3)):
            index=next(i for i,e in enumerate(rows) if e['note'].startswith('{') and json.loads(e['note']).get('phase')=='ownership' and json.loads(e['note']).get('stage')==stage)
            event=copy.deepcopy(end); event.update(conn=0,wsi=0,name='marker',note=json.dumps(dict(phase=phase,route=1,value=True,index=dict(known=False))))
            rows.insert(index,event)
        index=next(i for i,e in enumerate(rows) if e['note'].startswith('{') and json.loads(e['note']).get('phase')=='ownership' and json.loads(e['note']).get('stage')==4)
        rows.insert(index,end)
        for n,e in enumerate(rows,1): e['seq']=n
        self.assertEqual(validate_case(*values),[])
        self.assertEqual(retirement_stages(rows,1)[0]['stage'],'during retirement wait')
        rows.remove(end); rows.append(end); values[1]['retired_before_shutdown']=False
        for n,e in enumerate(rows,1): e['seq']=n
        self.assertEqual(validate_case(*values),[])
        self.assertEqual(retirement_stages(rows,1)[0]['stage'],'only during context shutdown')
    def test_unsupported_render_refusal_preserves_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); entries=[definition(c[0]) for c in CASES]
            bad=entries[0]; bad.update(unsupported_ipv6=True,group_drained=True,errno=errno.EAFNOSUPPORT)
            folder=root/bad['case']; folder.mkdir()
            evidence=dict(operation='bind-ipv6-loopback',family='AF_INET6',host='::1',errno=errno.EAFNOSUPPORT)
            data=json.dumps(evidence).encode(); (folder/'unsupported.json').write_bytes(data)
            bad['sha256']={'unsupported.json':hashlib.sha256(data).hexdigest()}
            (folder/'result.json').write_text(json.dumps(bad)); (root/'matrix.json').write_text(json.dumps(dict(cases=entries)))
            output=root/'passport.md'; output.write_text('accepted\n')
            with self.assertRaises(ValueError): render(root,output)
            self.assertEqual(output.read_text(),'accepted\n')

def main():
    parser=argparse.ArgumentParser(); parser.add_argument('--selftest',action='store_true')
    parser.add_argument('--run',type=pathlib.Path); parser.add_argument('--out',type=pathlib.Path)
    parser.add_argument('--case',type=pathlib.Path); parser.add_argument('--require-positive',action='store_true')
    args=parser.parse_args()
    if args.selftest:
        return int(not unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ValidatorTest)).wasSuccessful())
    if args.case:
        result,trace,errors,summary=load_case(args.case)
        if args.require_positive and (trace is None or not trace.get('opened') or not trace.get('echoed')):
            errors.append('required positive route did not open+echo')
        print(json.dumps(dict(case=result['case'],errors=errors,summary=summary),indent=2))
        return int(bool(errors))
    if not args.run or not args.out: parser.error('--run and --out required')
    render(args.run,args.out); return 0
if __name__=='__main__': raise SystemExit(main())
```

**Round2 evidence contract and RED/GREEN gate.** `lifecycle_index` builds ownership only from actual callback rows with stable WSI tokens. A conn0 CREATE is allowed; association may arrive in a later real callback, but its unique owner must agree with every witness. Repeated CREATE remains legal before the unique DESTROY. Every original, neighbor and operation-watch identity lies strictly inside that token's CREATE→DESTROY interval; a self-consistent invented token cannot create an interval. Open notes bind to the actual ESTABLISHED token. Each verification note binds to the most recent preceding WSI callback of its connection, which must be that certificate callback; every certificate callback, successful or failed, requires exactly one associated note with matching preverify. Native Record markers have conn but wsi0, so marker fields never substitute for actual callback token association.

`provenance` is the sole actual-use decision consumed by both `validate_case` and `summarize`/`ownership`. It validates every claimed live record and watched record, context-origin consistency, declared connection/route, SSL ownership and actual callback association even when no open occurs. An invalid record anywhere in that evidence makes supplied use unestablished. A valid failed-certificate callback can establish actual supplied-context use without claiming handshake success; no SSL/use evidence leaves ownership unestablished. Stage/free records alone cannot establish use. The renderer retains errors and raw observations; it cannot publish an exercised-use label from malformed origins beside those errors.

Both relay journals are sampled before parent context-manager cleanup. Every emitted fixture_error, hold_deadline, hold_stopped, stopped or relay_deadline invalidates acquisition in every mode, including ungated negative measurements. There is no cleanup exception: cleanup after the saved snapshot cannot terminate the stimulus represented by that snapshot. The controller's existing wait_idle/drain failures remain additional acquisition errors.

Implement the new `synthetic_untrusted` fixture and the four round2 test methods before changing the fix1 validator predicates. Run the following command and preserve its nonzero RED output: coherent token999, premature destruction, cutoff journals and missing/malformed non-open origins must expose fix1. Then install the complete predicates above and rerun the same command as GREEN. The positive conn0/repeated-CREATE cases and valid failed-verification row must pass; each malformed copy is discarded and its exact original is checked again. These are deterministic validator controls, not live library experiments or edited measurement archives. Do not execute snippets during plan authorship.

```bash
python3 -I Tools/lws_route_passport.py --selftest
```

No C++ source changes are introduced by round2. The existing source mutation/build/actual acquisition/restoration gates in Step7 still apply unchanged when Task2 is implemented. Unsupported capability, failed-create/skipped-init, missing final trace, crashes, unavailable private indices and library-returned failures retain their distinct outcomes; these validator repairs do not demand unsafe native success.

The exporter includes `ReasonName` from landed `VaRestLwsProbe.cpp:806–840`; validation uses `WSI_CREATE`, `WSI_DESTROY`, `PERFORM_SERVER_CERT_VERIFICATION` and `connect>` exactly. Numeric reason values remain raw evidence but do not drive the validator.

- [ ] **Step 6: implement the controller and execute the full immutable matrix.** New file:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import argparse
import unittest
import contextlib
import hashlib
import json
import os
import pathlib
import shutil
import socket
import subprocess
import sys
import threading
import time
import urllib.parse
import uuid
sys.dont_write_bytecode = True
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from run_lws_proxy_probe import ROOT, target_server, child_environment, direct_json, stop_group
from lws_route_passport import CASES, definition
sys.path.insert(0, str(ROOT / 'Tools/testserver'))
from route_proxy import RouteProxy, Ipv6Unavailable

def save(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')

def manifest(folder):
    return {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in folder.iterdir()
            if p.is_file() and p.name != 'result.json'}

def observe_child(process, folder, proxy, held, ready, release, result, environment):
    limit = time.monotonic() + 120
    try:
        while process.poll() is None and time.monotonic() < limit:
            if held and proxy.server_held.is_set() and not ready.exists():
                save(folder / 'proxy-held.json', proxy.snapshot())
                ready.write_text('actual-server-flight-held\n')
                result['gate']['ready_after_hold'] = True
            if held and release.exists() and not proxy.release_tls.is_set():
                result['gate']['release_after_child'] = ready.exists() and proxy.server_held.is_set()
                save(folder / 'gate-order.json', result['gate'])
                proxy.release_tls.set()
            threading.Event().wait(.02)
        result['runner_rc'] = process.wait(timeout=1) if process.poll() is not None else 124
    finally:
        try:
            stop_group(process)
            result['group_drained'] = True
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            result['errors'].append('cleanup: ' + str(error))
            result['abort_matrix'] = True
        editor = pathlib.Path(environment.get('VAREST_HOST', '/w/Unreal/VaRestHost')) / 'Saved/Logs/VaRestTests.log'
        if editor.exists():
            shutil.copyfile(editor, folder / 'editor.log')
        else:
            result['errors'].append('editor log missing')
            (folder / 'editor.log').write_text('No editor log was available for this child.\n')
        save(folder / 'child-status.json', result)

class ControllerTest(unittest.TestCase):
    def test_failed_child_keeps_status_and_drains(self):
        import tempfile
        with tempfile.TemporaryDirectory() as directory:
            folder = pathlib.Path(directory)
            log = folder / 'Saved/Logs'
            log.mkdir(parents=True)
            (log / 'VaRestTests.log').write_text('PENDING synthetic-controller-exit\n')
            result = dict(runner_rc=-1, group_drained=False, gate={}, errors=[])
            with (folder / 'runner.log').open('wb') as output:
                process = subprocess.Popen([sys.executable, '-c', 'import sys;sys.exit(73)'],
                    stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
                observe_child(process, folder, None, False, folder / 'ready', folder / 'release',
                              result, {'VAREST_HOST': str(folder)})
            self.assertEqual(result['runner_rc'], 73)
            self.assertTrue(result['group_drained'])
            self.assertEqual(result['errors'], [])
            self.assertIn('PENDING synthetic-controller-exit', (folder / 'editor.log').read_text())
            self.assertFalse((folder / 'trace.json').exists())

    def test_failed_drain_preserves_first_and_never_launches_next(self):
        import tempfile
        from unittest.mock import patch
        class Exited:
            def poll(self): return 73
            def wait(self, timeout): return 73
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            launches = []
            def acquire(folder, item):
                launches.append(item[0]); folder.mkdir()
                result = dict(runner_rc=-1, group_drained=False, gate={}, errors=[])
                with patch(__name__ + '.stop_group', side_effect=RuntimeError('synthetic drain refused')):
                    observe_child(Exited(), folder, None, False, folder/'ready', folder/'release', result, {'VAREST_HOST': str(root)})
                save(folder/'proxy-a.json', dict(events=[dict(ev='tls_hold')]))
                save(folder/'result.json', result)
                return result
            with self.assertRaises(AcquisitionAborted):
                acquire_matrix(root, [('first',), ('second',)], acquire)
            self.assertEqual(launches, ['first'])
            self.assertEqual(json.loads((root/'first/result.json').read_text())['runner_rc'], 73)
            self.assertIn('tls_hold', (root/'first/proxy-a.json').read_text())
            self.assertEqual(len(json.loads((root/'matrix.json').read_text())['cases']), 1)

class AcquisitionAborted(RuntimeError):
    pass

def acquire_matrix(folder, items, acquire):
    results = []
    for item in items:
        result = acquire(folder / item[0], item)
        results.append(result)
        save(folder / 'matrix.json', dict(schema=1, baseline='e8fbf1e4868c43cd5cb81186e352263037bfd4d7', cases=results))
        if result.get('group_drained') is not True:
            raise AcquisitionAborted('owned child group drain unproven; no next acquisition')
    return results

def one(folder, item, plain, tls):
    ident, mode, use_tls, provided, form = item
    result = definition(ident)
    result.update(runner_rc=-1, group_drained=False, gate={}, errors=[])
    folder.mkdir()
    ports = [urllib.parse.urlsplit(plain).port, urllib.parse.urlsplit(tls).port]
    source_key, target_key = uuid.uuid4().hex, uuid.uuid4().hex
    path = '/ws/echo?key=' + source_key
    if mode == 'redirect':
        location = '%s://127.0.0.1:%d/ws/echo?key=%s' % ('wss' if use_tls else 'ws', ports[1 if use_tls else 0], target_key)
        path = '/ws/status/302?key=' + source_key + '&location=' + urllib.parse.quote(location, safe='')
    held = mode in ('held', 'held-reuse')
    with contextlib.ExitStack() as stack:
        try:
            proxy_a = stack.enter_context(RouteProxy(ports, family=socket.AF_INET if form == 'v4' else socket.AF_INET6,
                                                    hold_tls=held, inspect_tls=use_tls))
        except Ipv6Unavailable as error:
            result.update(unsupported_ipv6=True, errno=error.errno, detail=str(error), group_drained=True)
            save(folder / 'unsupported.json', error.evidence)
            result['sha256'] = manifest(folder)
            save(folder / 'result.json', result)
            return result
        except OSError as error:
            result.update(group_drained=True, target_port=ports[1 if use_tls else 0],
                          errors=['fixture construction: ' + type(error).__name__ + ': ' + str(error)])
            save(folder / 'acquisition.json', result)
            for name in ('proxy-a.json', 'proxy-b.json', 'source.json', 'target.json', 'neighbor.json'):
                save(folder / name, dict(acquisition_error=result['errors'][0]))
            for name in ('runner.log', 'editor.log'):
                (folder / name).write_text('No child started: fixture construction failed.\n')
            result['sha256'] = manifest(folder)
            save(folder / 'result.json', result)
            return result
        proxy_b = stack.enter_context(RouteProxy(ports, inspect_tls=use_tls))
        # Bind without listening and retain the socket: no other process can claim the refused endpoint during the case.
        refused = stack.enter_context(socket.socket(socket.AF_INET, socket.SOCK_STREAM))
        refused.bind(('127.0.0.1', 0))
        first = '127.0.0.1:%d' % refused.getsockname()[1] if mode in ('fallback', 'precreated') else proxy_a.address
        next_proxy = proxy_a.address if mode in ('fallback', 'precreated') else proxy_b.address
        extra_port = None
        if form == 'bare-port':
            next_proxy, extra_port = '::1', proxy_a.server.server_address[1]
        elif form == 'bracket-port':
            next_proxy, extra_port = '[::1]', proxy_a.server.server_address[1]
        ready = folder / 'gate-ready'
        release = folder / 'gate-release'
        extra = ['-VaRestTestServer=' + plain, '-VaRestTestServerTls=' + tls,
                 '-VaRestRouteCase=' + ident, '-VaRestRouteMode=' + mode,
                 '-VaRestRouteFirst=' + first, '-VaRestRouteNext=' + next_proxy,
                 '-VaRestRouteTrace=' + str(folder / 'trace.json'),
                 '-VaRestRouteCa=' + str(ROOT / 'Tools/testserver/certs/test-ca.pem'),
                 '-VaRestRoutePath=' + path, '-VaRestRouteGateReady=' + str(ready),
                 '-VaRestRouteGateRelease=' + str(release)]
        if extra_port is not None:
            extra.append('-VaRestRouteProxyPort=' + str(extra_port))
        if use_tls:
            extra.append('-VaRestRouteTls')
        if provided:
            extra.append('-VaRestRouteProvided')
        result.update(args=extra, first_proxy=first, next_proxy=next_proxy, source_key=source_key,
                      target_key=target_key, target_port=ports[1 if use_tls else 0],
                      proxy_a_address=proxy_a.address, proxy_b_address=proxy_b.address,
                      peer_a=dict(host=proxy_a.server.server_address[0], port=proxy_a.server.server_address[1]),
                      peer_b=dict(host=proxy_b.server.server_address[0], port=proxy_b.server.server_address[1]))
        save(folder / 'acquisition.json', result)
        environment = child_environment(os.environ, extra)
        with (folder / 'runner.log').open('wb') as stream:
            process = subprocess.Popen(['bash', str(ROOT / 'Tools/run_tests.sh'), '--no-build',
                                        'Probe.VaRest.Lws.Route.Attempt'], cwd=ROOT, env=environment,
                                       stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
            observe_child(process, folder, proxy_a, held, ready, release, result, environment)
            save(folder / 'proxy-before-exit.json', proxy_a.snapshot())
            save(folder / 'proxy-b-before-exit.json', proxy_b.snapshot())
            save(folder / 'result.partial.json', result)
        # A failed drain remains false, is persisted below and aborts the matrix before another launch.
        if not proxy_a.wait_idle(2) or not proxy_b.wait_idle(2):
            result['errors'].append('relay not naturally idle before fixture shutdown')
    save(folder / 'proxy-a.json', proxy_a.snapshot())
    save(folder / 'proxy-b.json', proxy_b.snapshot())
    for name, key in (('source', source_key), ('target', target_key), ('neighbor', ident + '-neighbor')):
        try:
            value = direct_json(plain, '/ws/journal?key=' + key + '&wait_ms=100', raw_path=folder / (name + '.raw'))
        except (OSError, ValueError, RuntimeError) as error:
            value = dict(acquisition_error=str(error))
            result['errors'].append(name + ': ' + str(error))
        save(folder / (name + '.json'), value)
    result['sha256'] = manifest(folder)
    save(folder / 'result.json', result)
    return result

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--only', choices=[c[0] for c in CASES])
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        return int(not unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ControllerTest)).wasSuccessful())
    if args.out is None:
        parser.error('--out required outside selftest')
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    if any(c.isspace() for c in str(args.out)):
        parser.error('output path must be whitespace-free for runner arguments')
    items = [item for item in CASES if not args.only or item[0] == args.only]
    try:
        with target_server(args.out) as (plain, tls):
            results = acquire_matrix(args.out, items, lambda folder, item: one(folder, item, plain, tls))
    except AcquisitionAborted as error:
        print(str(error), file=sys.stderr)
        return 1
    return int(any(r['runner_rc'] != 0 or r['errors'] for r in results if not r.get('unsupported_ipv6')))
if __name__ == '__main__':
    raise SystemExit(main())
```

Task1 RouteProxy needs the following complete signature/member changes in this task (owned exact file added to this task's commit list): change constructor to `def __init__(self, ports, family=socket.AF_INET, hold_tls=False, inspect_tls=False):`, insert `self.inspect_tls = inspect_tls or hold_tls` after assigning hold_tls, and change only the ClientHello accumulation condition to `if source is sock and self.inspect_tls and not hello_done:`. Holding server bytes still requires `self.hold_tls`. This produces SNI witnesses for all TLS positives while keeping ungated routes transparent after the first validated record. Unit selftests remain green; malformed/split-over-multiple-record ClientHello is a fixture acquisition limit, explicitly recorded rather than silently ignored.

**Fresh C++ graph/build precedes the matrix.** Run separately so failure cannot suppress artifact inspection:

```bash
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Route.Budget
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Route.UnknownToken
python3 -I Tools/lws_route_passport.py --selftest
python3 -I Tools/run_lws_route_probe.py --selftest
python3 -I Tools/testserver/route_proxy_selftest.py
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/full-1
python3 -I Tools/lws_route_passport.py --run /w/Unreal/VaRestHost/Saved/RouteProbe/full-1 --out docs/passport/2026-10-23-lws30-route-probe.md
```

A failed raw child makes the acquisition controller nonzero and remains a FAILED/partial passport row. A successfully acquired negative library outcome is a complete row saying it did not open/echo, not a production pass. Render exit0 certifies artifact validation/rendering, **not** complete/safe matrix; report both return codes and row counts. Root reviews failed/unsupported rows before approving any B9 production direction.

- [ ] **Step 7: restore-the-bug and independent acceptance.** Run all validator/fixture tests. Perform four isolated live mutations, preserving failed artifacts and restoring exact bytes after each:

1. Remove route selection `Info.vhost = Selected->Vhost;`; fresh graph/build; collect `--only precreated-wss-generated` to `.../route-selection-mutant`. The named positive proof must cease to have verified open+echo through proxy A; do not merely require a generic test exit. Restore/rebuild/recollect as `route-selection-restored` and require actual peer/Host/SNI/cert/echo controls. If baseline cannot open, mutation is inconclusive and root gets the unmet prerequisite.
2. Omit both SSL-exists witness commands in held path; fresh graph/build; collect `--only held-wss-generated`. Validator must report `SSL hold witnesses`, even if echo works. Restore/rebuild/recollect. This proves successful echo alone cannot certify index/identity safety.
3. Python-only fixture mutation: substitute a `tls_release` marker and early forwarding before child release. The deterministic fixture test fails; a copied acquired case with `gate.release_after_child=false` must also be rejected by validation. Restore exact relay bytes and selftests. Never deliberately move private indices or dereference invalid pointers to manufacture a crash.

4. Supplied→generated substitution: only change AddRouteNow's `Info.provided_client_ssl_ctx` assignment to `nullptr`. Keep Options.ProvidedSslContext retained and its free observer unchanged. Fresh graph/build; collect `precreated-wss-provided`. The trusted generated context may still open+echo, but `ctx_supplied`/`ctx_origin` on ssl-open and certificate witnesses must no longer certify supplied use; the case validator reports `observed: supplied context not used` and ownership becomes unestablished. Restore/rebuild/recollect; require the actual live supplied-context witness, not merely0/1 destruction counts. If the baseline provided path is unsupported, preserve it as inconclusive for this mutant and ask root to adjudicate the unmet production prerequisite.

The nondefault selection baseline precreates route1 against a retained bound/nonlistening numeric endpoint and selects route2 through actual proxyA. Its public actual-vhost identity must be2. Removing explicit selection uses route1, changes the actual peer and cannot preserve the positive proxyA witness. This is independent of a guessed generic exit code.

The corresponding exact collection commands are run with the named source version already built under the common guard:

```bash
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/route-selection-mutant --only precreated-wss-generated
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/route-selection-restored --only precreated-wss-generated
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/ssl-witness-mutant --only held-wss-generated
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/ssl-witness-restored --only held-wss-generated
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/context-origin-mutant --only precreated-wss-provided
python3 -I Tools/run_lws_route_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/context-origin-restored --only precreated-wss-provided
```

Run these case validators after the corresponding acquisitions, preserving return codes separately:

```bash
python3 -I Tools/lws_route_passport.py --case /w/Unreal/VaRestHost/Saved/RouteProbe/route-selection-mutant/precreated-wss-generated --require-positive
python3 -I Tools/lws_route_passport.py --case /w/Unreal/VaRestHost/Saved/RouteProbe/route-selection-restored/precreated-wss-generated --require-positive
python3 -I Tools/lws_route_passport.py --case /w/Unreal/VaRestHost/Saved/RouteProbe/ssl-witness-mutant/held-wss-generated
python3 -I Tools/lws_route_passport.py --case /w/Unreal/VaRestHost/Saved/RouteProbe/ssl-witness-restored/held-wss-generated
python3 -I Tools/lws_route_passport.py --case /w/Unreal/VaRestHost/Saved/RouteProbe/context-origin-mutant/precreated-wss-provided --require-positive
python3 -I Tools/lws_route_passport.py --case /w/Unreal/VaRestHost/Saved/RouteProbe/context-origin-restored/precreated-wss-provided --require-positive
```

The selection mutation/restoration pair is valid only if the restored positive actually passes. The held pair may retain an independently observed unsupported library outcome; root adjudicates it rather than relaxing witness validation.

Two artifact mutations need no UE rebuild: copy full-1 to a new directory and change one trace's dropped counter without changing manifest (hash refusal), then change a trace root to `[]`; render targeting a pre-existing passport must refuse and preserve exact prior bytes. Restore neither by overwriting full-1: use new copies only. Revalidate untouched full-1 afterwards.

The controller's failed-child73 and failed-drain selftests use the same lifecycle functions as acquisition, without entering the immutable measured matrix. Run `python3 -I Tools/run_lws_route_probe.py --selftest`. Mutation sets runner_rc=0 after observation: the exact73 assertion fails. Mutation removes the abort condition in acquire_matrix: the failed-drain test's no-next-launch assertion fails. Restore each exact byte sequence and rerun. A drained crashed child can continue; an unproven drain cannot.

Required existing regressions once after all exact restoration: `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Rig`; `python3 -I Tools/run_lws_proxy_probe.py --out /w/Unreal/VaRestHost/Saved/RouteProbe/b1-final --only explicit-wss`; `Tools/run_tests.sh --no-build VaRest.Integration.WebSocket.Transport.Redirect`; Python route/proxy/ws selftests. Do not rerun the complete B1 22-case matrix without a demonstrated B1 regression.

- [ ] **Step 8: generated passport and manual root handoff, then commit.** Append a handoff naming each actual outcome: whether initial routes can be precreated/reused, whether late creation/init moves the weak index, whether public SSL/WSI identity and verified leaf survive, actual proxy-peer fallback with original CONNECT/Host/SNI, provided SSL_CTX free ordering, active-vhost teardown token closure, and each IPv6 spelling. List failures/crashes/unsupported rows and exact paths; do not claim a failed measurement passed or promote index-absence to portability. Root rules on B9, including any prohibition on late vhost creation. The route probe does not prove asynchronous resolver worker lifetime, normal/abandon module unload contracts, proxy production classification, Windows/Mac/mobile/cooked runtime or W3 delivery bounds.

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.cpp Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRouteTest.cpp Tools/testserver/route_proxy.py Tools/run_lws_route_probe.py Tools/lws_route_passport.py docs/passport/2026-10-23-lws30-route-probe.md
git commit -m "test: record immutable proxy-route and TLS lifetime measurements" -m "Co-Authored-By: Codex <noreply@openai.com>"
```

Root closes `VaRest-81s.12` only after independent task and whole-prerequisite review; then removes its blocker on remaining-plan `VaRest-81s.11`. Parent `VaRest-81s`, proxy `VaRest-2rt`, DNS `VaRest-gi0`, cooked `VaRest-5ia`, metadata/W3 `VaRest-81s.5` remain open unless their actual acceptance separately completes.

## Activation after M4b

Root ruled that M4b executes as the next complete source milestone while these documents are prepared. No source milestone interleaving. Before activating this plan, compare the new reviewed HEAD against baseline only for the raw rig, test runner/helpers, fixture imports and module build dependencies. Reconcile concrete interface changes, regenerate the native action graph, and record actual activation SHA; do not replay stale artifacts or pretend this baseline is unchanged. Frozen v1 and production WebSocket source remain outside this prerequisite.

## Writer self-review and execution boundary

| Requirement | Owning task and evidence |
| --- | --- |
| Bounded route/vhost/string ownership | Task2 eight monotone slots, stable unique-owned route storage through joined destruction; Budget and actual isolated matrix before acceptance |
| Actual proxy-peer address fallback | Task2 bound/nonlistening initial endpoint, all-token retirement before fallback; distinct nondefault selected route and actual endpoint/vhost witness |
| Named CONNECT/Host/SNI and credential separation | Task1 first ClientHello parser; Task2 verified leaf/public SSL association; Task2 exact target/CONNECT assertions |
| Precreated reuse versus late vhost creation | Separate held-reuse/held matrix rows; actual SSL-before/after witnesses; neighbor really connects through precreated second route |
| Verification-index motion | Paired per-route create/init return/index/watch records; movement attributed only within each operation and original live SSL window |
| Provided SSL_CTX ownership | Task2 live SSL_CTX provenance, flushed stages0–8, final-destruction observer and guarded caller free; supplied→generated control |
| Active vhost teardown | destroy-active rows derive each token retirement relative to public destroy and shutdown; late context-only retirement remains explicit |
| IPv6 numeric proxy syntax | Three public address/port forms against ::1; immutable definitions validated first and capability errno limited to actual IPv6 construction |
| Crashed/failed acquisition evidence | Per-case owned child; flushed phases/ownership, parent snapshots/status, failed-child73 and failed-drain/no-next-launch selftests |
| Binding W1/B1 protection | Production and B1 fixture/exporter/passport unchanged; existing serial Rig/B1 explicit-wss/Redirect and Python regressions |

The five Review Focus risks have concrete test bodies in Tasks1–2. Task1 is independently testable Python relay work; Task2 includes raw C++, controller, validator, measured passport and all native/mutation controls before its independent acceptance. No new-route behavior is deferred past a source-task commit. Python blocks were statically parsed as documentation snippets; no fixture/selftest/runtime command was executed during authorship. C++ snippets were checked against the baseline declarations for names/types, namespace/include ownership, callback captures, no nested pumping, unchecked optional/pointer use, and exact file lists. This is not compilation evidence. The copied Global Constraints are preserved verbatim; explicit later overrides settle coauthor/build/ownership conflicts.

No safe production routing conclusion is available yet. The only remaining questions are the stated empirical outputs of this executable prerequisite, owned by root after measurement: whether precreation/reuse and late creation/init preserve the global index and identity, how provided contexts are owned across vhosts, and which IPv6 form works in the pinned library. None is silently selected as production behavior. Complete B9 production planning waits for those measurements and a root ruling.


## Fix round1 — accepted review requirements and self-review

Original frozen baseline: `88a130e89b07b1dc92be8e5844c5e2a784f1b0b446ec389e296397006bc0aac7`. Authority: `.superpowers/sdd/ws-route-probe-plan-review-rulings.md`, RP1–RP10. These repairs change the planned experiment and its acceptance code; they are not runtime findings.

| ID | Concrete repair | Discriminating planned proof |
| --- | --- | --- |
| RP1 | Two sequential tasks; raw acquisition, controller, validator and passport all in Task2 | Full matrix and named actual mutations precede Task2 review/commit; exact final file list includes every deliverable |
| RP2 | Refused default route1, selected nondefault route2 through proxyA; public actual-vhost and peer-port evidence | Omit ccinfo.vhost; named positive selection witness fails; exact restored positive required |
| RP3 | Registered raw rig dispatch before lws_get_context; retained pointer membership before FindConn/accessors/store | UnknownToken calls zero accessors for unowned token; inspect-before-membership mutation fails; ordinary B1 raw mode unchanged |
| RP4 | Neighbor connection id exported; its journal, actual proxyB, Host/SNI, cert, SSL/open identity and all-token retirement consumed within held window | Valid synthetic held-reuse plus omitted/changed/routed-A/after-window refusals; real held-reuse matrix |
| RP5 | Flushed snapshot stages0–8 and bounded final-destruction records; per-token destroy-stage derivation | Early/missing/multiple/context-stage synthetic outcomes differ; marker/token disagreement fails; real active teardown rows |
| RP6 | Per-route create/init pairs, explicit skipped init, original WSI/SSL watch on each side | Missing operation/watch, changed identity, earlier-only motion and separate create/init-only motion tests |
| RP7 | Fixture errors/deadline/stop rejected in every mode; explicit CONNECT/ClientHello/failed-cert/302 stimuli | Broken upstream plus client failure refuses acquisition; untrusted without real bad certificate refused; actual named route/init refusal remains measured |
| RP8 | Immutable definition/type checks before all render branches; narrow IPv6 construction exception/errno | V4 unsupported substitution, changed fields, EMFILE refusal and atomic output-preservation tests |
| RP9 | Persist child status/proxy snapshots/partial result, then abort matrix on unproven drain | Mocked stop_group failure preserves first artifacts and launches no second child; drained exit73 remains retained |
| RP10 | Public SSL_get_SSL_CTX comparison against retained supplied context; unknown origin never dereferenced/freed | Provided→generated mutation can preserve echo while failing supplied-use assertion; omitted/malformed origin tests; ownership gated on actual use |

Self-review checked the two task headings and full exact-path commit lists, public versus private APIs, monotone route/string/token ownership, callback preconditions before each accessor, destruction observer lifetime, neighbor identity/window predicates, unsupported branches before output replacement, and abort-before-next-launch behavior. Safe production direction remains a later root ruling on the actual measurements. Negative/crashed/partial rows are retained; no accepted unsupported result is rewritten into a positive result.


## Fix round2 — residual accepted predicates

Fix1 frozen baseline: `582f27581eeba71b2ec7029c2b1b07e91e8084bd3e8a11686a6451ffd06ee401`, preserved by root in `.superpowers/sdd/ws-route-probe-plan-fix1-baseline.md`. Authority: `.superpowers/sdd/ws-route-probe-plan-fix2-rulings.md`. The round1 table is historical writer disposition; scoped re-review found the following three remaining predicates, repaired here without changing native acquisition or other accepted contracts.

| ID | Complete repair in Task2 | Discriminating controls |
| --- | --- | --- |
| R2-1 / RP6 | Actual-token lifetime index; every identity and operation watch resolves to real unique owner, prior CREATE and later DESTROY; actual open/verification callback association | Coherent notes/watch token999 with actual lifecycle7; DESTROY before held witness or inside create/init; wrong actual open/verify token; positive conn0 CREATE with later association and repeated CREATE |
| R2-2 / RP7 | Both saved relay journals reject every emitted failure/stop/deadline spelling before mode-specific interpretation | All five spellings on each relay in complete held, held-reuse and ungated untrusted rows; negative row already has CONNECT, ClientHello and real failed-cert callback stimulus |
| R2-3 / RP10 | Shared validated provenance feeds validation and ownership summary, including non-open failed verification; missing/malformed/unassociated context evidence cannot establish supplied use | Positive and untrusted non-open omission, malformed/wrong origin, absent context, wrong token/callback and missing note; require ownership unestablished as well as validation error; exact original restoration regains valid exercised-use label |

Self-review: each helper is defined before its consumer; changed function signatures have complete updated callers; native Record marker conn/wsi shape matches synthetic fixtures; conn0/repeated CREATE is accepted while lifecycle rows, not notes, supply unique token ownership. Operation watches and before/after held notes share the same bounded interval. Validation and rendering use the same provenance function, so malformed non-open evidence has no separate permissive summary path. Universal cutoff handling uses actual emitted names and excludes no mode. No raw pointer values, new accessors, forced frees, library patches, production assumptions or fixture edits were added in this repair. The four original Python implementations plus added helper/test bodies are documentation; static parsing is distinct from the future RED/GREEN execution gate. Root commissions scoped re-review of this frozen repair before activation after M4b.
