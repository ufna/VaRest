# VaRest 2 — WebSocket W1b B1: CONNECT proxy evidence — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce reproducible Linux raw-libwebsockets evidence for proxy callback ordering, errors and routing, sufficient for a separately reviewed B2 policy decision.

**Architecture:** A bounded, loopback-only Python CONNECT fixture tunnels to the existing HTTP/TLS WebSocket fixture. A Python parent owns the listeners, fetches target journals directly, and launches one raw-rig attempt per editor child with an explicit environment. The rig records public callbacks before hooks and writes JSON after context destruction; the parent preserves logs, journals and a generated passport without selecting a production classifier or redirect repair.

**Tech Stack:** UE 5.8.1, C++20, engine-bundled libwebsockets 3.0.0/OpenSSL 1.1.1t, UE Automation, Python 3 standard library, existing `Tools/run_tests.sh` and `Tools/testserver/ws.py`.

**Spec:** `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` §§1.2, 2.1–2.2, 16.3–16.4, 17; `.superpowers/sdd/ws-w1b-design-preflight.md` §1; `.superpowers/sdd/release-2.0-program.md` “W1b preflight rulings” (2026-10-23). Read all three. Baseline is W1a final `9884b5b`, not preflight's `fbb5007`. The older `2026-10-22-varest2-ws-w1b-service-policies.md` is scope context: its guessed denial string, state discriminator and `?deny=403` CONNECT mechanism are superseded.

<!-- Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved. -->

The checkboxes below are execution instructions. Beads owns actual task status; root owns VaRest-81s.9 and later implementation beads. This document author ran no implementation, build, selftest or runtime probe. All callback counts, routing outcomes and timings remain measurements to make.

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
- Every commit message names its actual authoring client/model truthfully. For this Codex-authored plan use `Co-Authored-By: Codex <noreply@openai.com>`; do not claim Claude authorship. Stage only the files your task names (`git add <paths>`): other agents work in this tree. On `index.lock`, wait and retry. Never push.

The preceding W1a constraints are copied verbatim except the explicitly corrected authorship rule. Their W1a task numbers describe historical ownership. For B1 the existing raw rig continues to use its private `VaRestLwsProbeLws.h` include wrapper and namespace `VaRest::LwsProbe`; this does not add a production include path or dependency. Raw-probe tests use `Probe.VaRest.Lws.*`. No production source under `Source/VaRestWebSocket` changes. Local implementation commits remain root-coordinated; acquire the controller's exclusive index lease before a task's listed git commands when other agents share this checkout.

B1-specific constraints supersede stale W1a runbook assumptions: inspect a normal-target `-WriteOutdatedActions` result before every build and stop on engine actions. A dry run may invoke UHT and invalidate generated engine artifacts; `-Module`/`-UsePrecompiled` are not isolation guarantees. Never fix engine files, generated includes, timestamps or caches from this plan. No NoPCH gate, cooked run, native Windows/macOS/mobile claim, flow-control/ping/DNS/reply change, or whole-W1 completion belongs to B1.

## Review Focus

1. **A proxy error with null input versus a present zero-length buffer** remains distinguishable without dereferencing null, guessing HTTP status, or logging addresses. Task 2 `ProxyErrorInput` tests null/empty/embedded-NUL/truncation; Task 5 checks live denial/close/silence traces.
2. **A CONNECT authority containing an external host, wrong port, oversized headers, or a stalled header** never opens an arbitrary upstream or retains an unbounded handler/journal. Task 1 selftests exercise each, saturation and shutdown during silence.
3. **The child's `http_proxy` points at a CONNECT-only listener** while journals and CA loading still work. Task 4 selftest poisons the parent environment and proves direct `http.client` access; matrix control requests remain outside the child and do not depend on `no_proxy`.
4. **A tunnel succeeds and the target redirects, including ws→wss**, preserving exact APPEND/WSI_CREATE/cert order and whether synthetic Authorization reached either request. Tasks 3–5 record all callbacks without production vetoes and correlate source/target journals; the prior context is destroyed before another begins.
5. **Partial evidence, a crashed child, or a dropped trace event** cannot masquerade as a complete passport. Tasks 2 and 5 exercise event limits, incomplete matrices, malformed traces and explicit failure artifacts; Task 4 preserves each child log before another run overwrites the runner log.

---

## Design decisions

1. **Fixture and controller separation.** `proxy.py` is a reusable Python object with one immutable mode (`tunnel`, `deny`, `close`, `silent`) per listener. It has no remotely mutable mode and no HTTP forward-proxy support. Only `127.0.0.1` and `localhost` authorities with an explicitly supplied target port are accepted, then connected numerically to `127.0.0.1`. This bounds scope and avoids assuming the WS URL path reaches CONNECT. Cost: fixtures are recreated per case.
2. **Control traffic bypass by construction.** The parent uses `http.client.HTTPConnection('127.0.0.1', port)`; this API does not read proxy environment variables. The child reads a local CA PEM and writes a local trace; it never calls `FetchJournal`/`FetchBytes`. Do not add global environment setters in C++ or rely on curl bypass or LWS `no_proxy` behavior.
3. **Raw evidence, then policy.** APPEND, WSI_CREATE and per-depth certificate callbacks are measured, not expected to have fixed counts. No opaque `struct lws` reads, state-offset accessor, bundled archive patch or log-derived production signal. Static preflight's null/0 denial branch is a prediction, not a runtime result. An explicit post-tunnel TLS rejection and target HTTP 403 are negative controls for B2's possible contextual classifier.
4. **Lifetime and identity.** Pointer values are internal keys only. A stable integer token lasts until WSI_DESTROY; a repeated WSI_CREATE before destruction retains the token and remains a separate event. Connection resolution is recorded independently from the enclosing attempt. A context is shut down and its destruction callbacks captured before the next child/rig. A terminal callback alone is insufficient (VaRest-rti).
5. **Evidence format.** Each case produces `trace.json`, `proxy-a.json`, `proxy-b.json`, `source.json`, `target.json`, `editor.log`, `runner.txt` and `case.json`. Every callback record includes sequence, resolved connection ID, WSI token, reason/name, time, raw len, error pointer presence, bounded error hex, certificate depth/error/preverify, and bounded receive bytes. Full journals contain only fixture-generated data and the synthetic marker `Bearer b1-probe-marker`.
6. **Redirect observation.** The rig appends the synthetic marker at every APPEND, deliberately matching its existing default behavior. Therefore a target marker proves what this raw probe sent, not library header replay. B2 retains no-follow policy; the existing `RedirectReplay` probe remains the distinct test for library replay. B1 does not weaken the production veto.
7. **Routing selection.** Null proxy option means null `http_proxy_address` (environment fallback possible). Explicit option uses the same `host:port` string form as the engine configuration path. Environment rows include cleared `no_proxy` and a separate `no_proxy=*` observation; no bypass behavior is presumed. An explicit proxy and a different environment proxy use distinct successful listeners so journals identify precedence without conflating routing and denial.
8. **Limits.** At most 16 concurrent proxy handlers, 8192 header bytes, 128 header lines, 512 journal events per listener, two-second header deadline, twelve-second connection lifetime, 50 ms polling, one-second upstream connect/write bounds. Limits are assertions, not silent evidence loss: any dropped journal or trace event invalidates measurement. Silent handlers watch EOF and the fixture stop event; shutdown joins them rather than leaving a 30-second sleeper.

## File structure and interfaces

| File | Responsibility |
|---|---|
| `Tools/testserver/proxy.py` (new) | `Proxy(mode, ports, max_events=512)` context manager; `.address`, `.snapshot()`, `.wait_idle(seconds)`; bounded CONNECT relay |
| `Tools/testserver/proxy_selftest.py` (new) | Real socket fixture selftests, no UE dependency |
| `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h/.cpp` | Optional proxy string, callback input capture, token/sequence/limit instrumentation |
| `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTraceTest.cpp` (new) | Deterministic instrumentation tests |
| `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTest.cpp` (new) | One parameterized child attempt, direct/proxy/redirect/TLS controls; JSON export |
| `Tools/run_tests.sh` | Narrow validated child-environment injection; ordinary runner still scrubs proxies |
| `Tools/run_lws_proxy_probe.py` (new) | Matrix controller, direct HTTP journal access, per-case artifacts and process cleanup |
| `Tools/lws_proxy_passport.py` (new) | Validate evidence and emit B1 passport; preserve W0's existing 135-row generator |
| `docs/passport/2026-10-23-lws30-proxy-probe.md` (generated) | Observed rows and explicit B2 decision inputs, Linux modular scope only |

Task order: 1 fixture → 2 recorder → 3 child attempt → 4 matrix → 5 passport and B2 handoff. Each task has its own RED/GREEN/restore cycle. Do not execute later tasks before the preceding reviewer gate.

## Build/test runbook used by Tasks 2–5

Normal recovery was accepted by root on 2026-10-23: the measured 21-action closure refreshed all 51 declared artifact paths with byte-identical hashes, the ordinary target then reported up-to-date, and the complete default no-build suite passed 572/0 (`release-2.0-program.md`, “Normal recovery accepted”). These are the audit executor's results, not this plan author's runs, and the old 21 actions are not a perpetual B1 allowlist. A fresh B1 dry run still gates every C++ RED/GREEN build. A refusal to build on new engine actions is a blocked validation gate, never a passed C++ test.

From `/w/Unreal/VaRest`, with no editor running, prepare a fresh action file and inspect it before any compile:

```bash
mkdir -p /w/Unreal/VaRestHost/Saved/B1Proxy
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/w/Unreal/VaRestHost/Saved/B1Proxy/actions.json
```

Every Compile/Link source and output must belong to the plugin (including its host plugin-specific Intermediate subtree). Ordinary metadata is allowed at host `Binaries/Linux/VaRestHostEditor.target`, host `Binaries/Linux/UnrealEditor.modules`, plugin `Binaries/Linux/UnrealEditor.modules`, and normal UBT bookkeeping; this does not authorize host/engine compilation, engine linking or PCH creation. Any new engine/host Compile/Link action, generated engine input invalidation or unfamiliar action blocks the build and goes to root with exact paths for a bounded normal-build ruling. Review link response files as well as source filenames; existing engine libraries used as link dependencies are expected, rebuilding them is not. Do not assume an empty Compile list makes an engine link safe. After root confirms the saved action set and no other process changed inputs:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Rig
```

Use the same guarded normal build before each C++ test run after edits, including restored mutations. Never let `Tools/run_tests.sh` implicitly build during B1. Existing normal links and the unexecuted NoPCH-link gate are separate; B1 cannot close hqx.2. If the normal build remains blocked, Python tasks can finish, but live B1 evidence and B2 remain blocked.

### Task 1: Bounded loopback CONNECT fixture

**Files:** Create `Tools/testserver/proxy.py`, `Tools/testserver/proxy_selftest.py`.

**Interfaces:** Consumes existing target listeners' numeric ports; produces `Proxy(mode: str, ports: Iterable[int], max_events: int = 512)`, `.address: str`, `.snapshot() -> dict`, `.wait_idle(seconds: float) -> bool`, context-managed start/stop. No server.py route registration or new dependency.

- [ ] **Step 1: Write the failing selftests** in `Tools/testserver/proxy_selftest.py`:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import contextlib
import pathlib
import socket
import socketserver
import sys
import threading
import time
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from proxy import Proxy


class Echo(socketserver.BaseRequestHandler):
    def handle(self):
        self.request.settimeout(2)
        try:
            while True:
                data = self.request.recv(4096)
                if not data:
                    return
                self.request.sendall(data)
        except OSError:
            return


@contextlib.contextmanager
def target():
    with socketserver.ThreadingTCPServer(('127.0.0.1', 0), Echo) as server:
        server.daemon_threads = True
        worker = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .05})
        worker.start()
        try:
            yield server.server_address[1]
        finally:
            server.shutdown()
            worker.join(1)
            if worker.is_alive():
                raise AssertionError('target did not stop')


def dial(proxy):
    host, port = proxy.address.split(':')
    return socket.create_connection((host, int(port)), timeout=3)


def header(sock):
    result = bytearray()
    while not result.endswith(b'\r\n\r\n'):
        part = sock.recv(1)
        if not part:
            break
        result.extend(part)
        if len(result) > 8192:
            raise AssertionError('oversized response')
    return bytes(result)


class ProxyTest(unittest.TestCase):
    def test_tunnel_and_separate_headers(self):
        with target() as port, Proxy('tunnel', [port]) as proxy, dial(proxy) as sock:
            sock.sendall(('CONNECT localhost:%d HTTP/1.1\r\nHost: localhost:%d\r\n\r\n' % (port, port)).encode())
            self.assertIn(b' 200 ', header(sock))
            payload = b'Authorization: Bearer b1-probe-marker\r\n\x00tunnel'
            sock.sendall(payload)
            received = bytearray()
            while len(received) < len(payload):
                received.extend(sock.recv(len(payload) - len(received)))
            self.assertEqual(bytes(received), payload)
            sock.shutdown(socket.SHUT_RDWR)
            self.assertTrue(proxy.wait_idle(1))
            events = proxy.snapshot()['events']
            connects = [e for e in events if e['ev'] == 'connect']
            self.assertEqual(len(connects), 1)
            self.assertEqual(connects[0]['authority'], 'localhost:%d' % port)
            self.assertNotIn('Authorization', str(connects[0]['headers']))
            self.assertEqual(sum(e['ev'] == 'upstream' for e in events), 1)

    def test_denial_close_silence_and_cleanup(self):
        for mode in ('deny', 'close', 'silent'):
            with self.subTest(mode=mode), target() as port:
                proxy = Proxy(mode, [port])
                proxy.__enter__()
                sock = dial(proxy)
                try:
                    sock.sendall(('CONNECT 127.0.0.1:%d HTTP/1.1\r\n\r\n' % port).encode())
                    if mode == 'deny':
                        self.assertIn(b' 403 ', header(sock))
                    elif mode == 'close':
                        self.assertEqual(sock.recv(1), b'')
                    else:
                        sock.settimeout(.15)
                        with self.assertRaises(socket.timeout):
                            sock.recv(1)
                    before = time.monotonic()
                    proxy.__exit__(None, None, None)
                    self.assertLess(time.monotonic() - before, 1)
                    self.assertTrue(proxy.wait_idle(0))
                    self.assertFalse(any(e['ev'] == 'upstream' for e in proxy.snapshot()['events']))
                finally:
                    sock.close()
                    proxy.__exit__(None, None, None)

    def test_bad_authority_header_limit_and_slow_header(self):
        with target() as port, Proxy('tunnel', [port]) as proxy:
            for authority in ('example.org:%d' % port, '127.0.0.1:1', '127.0.0.1:%d?deny=403' % port):
                with dial(proxy) as sock:
                    sock.sendall(('CONNECT %s HTTP/1.1\r\n\r\n' % authority).encode())
                    self.assertIn(b' 400 ', header(sock))
            with dial(proxy) as sock:
                sock.sendall(b'CONNECT ' + b'x' * 8200)
                self.assertIn(b' 400 ', header(sock))
            with dial(proxy) as sock:
                sock.sendall(b'C')
                before = time.monotonic()
                self.assertIn(b' 400 ', header(sock))
                self.assertLess(time.monotonic() - before, 2.8)
            self.assertTrue(proxy.wait_idle(1))
            self.assertFalse(any(e['ev'] == 'upstream' for e in proxy.snapshot()['events']))

    def test_handler_and_journal_bounds(self):
        with target() as port, Proxy('silent', [port], max_events=3) as proxy:
            sockets = []
            try:
                for _ in range(24):
                    sock = dial(proxy)
                    sockets.append(sock)
                    sock.sendall(('CONNECT 127.0.0.1:%d HTTP/1.1\r\n\r\n' % port).encode())
                # Wait on the observable admission/rejection state, not a fixed sleep.
                deadline = time.monotonic() + 2
                while proxy.snapshot()['accepted'] + proxy.snapshot()['rejected'] < 24 and time.monotonic() < deadline:
                    threading.Event().wait(.01)
                state = proxy.snapshot()
                self.assertLessEqual(state['active'], 16)
                self.assertGreater(state['rejected'], 0)
                self.assertLessEqual(len(state['events']), 3)
                self.assertGreater(state['dropped'], 0)
            finally:
                for sock in sockets:
                    sock.close()
            self.assertTrue(proxy.wait_idle(1))


if __name__ == '__main__':
    unittest.main()
```

- [ ] **Step 2: Run RED**: `python3 -I Tools/testserver/proxy_selftest.py -v`. Expected: import failure for the absent `proxy` module. A port permission/setup failure is not the intended RED.

- [ ] **Step 3: Implement** `Tools/testserver/proxy.py`:

```python
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Bounded test-only CONNECT relay. Never resolves a supplied hostname."""
import select
import socket
import socketserver
import threading
import time


class Proxy:
    def __init__(self, mode, ports, max_events=512):
        if mode not in ('tunnel', 'deny', 'close', 'silent'):
            raise ValueError('invalid proxy mode')
        self.mode = mode
        self.ports = frozenset(int(p) for p in ports)
        if not self.ports or any(p <= 0 or p > 65535 for p in self.ports):
            raise ValueError('invalid target ports')
        if not 1 <= max_events <= 512:
            raise ValueError('invalid journal bound')
        self.max_events = max_events
        self.lock = threading.Condition()
        self.stopping = threading.Event()
        self.events = []
        self.dropped = self.active = self.accepted = self.rejected = 0
        self.next_id = 0
        self.slots = threading.BoundedSemaphore(16)
        self.sockets = set()
        owner = self

        class Handler(socketserver.BaseRequestHandler):
            def handle(self):
                owner.handle(self.request)

        class Server(socketserver.ThreadingTCPServer):
            allow_reuse_address = True
            daemon_threads = False
            block_on_close = True

            def process_request(self, request, client_address):
                if not owner.slots.acquire(blocking=False):
                    with owner.lock:
                        owner.rejected += 1
                        owner.lock.notify_all()
                    self.shutdown_request(request)
                    return
                with owner.lock:
                    owner.active += 1
                    owner.accepted += 1
                    owner.sockets.add(request)
                    owner.lock.notify_all()
                try:
                    super().process_request(request, client_address)
                except BaseException:
                    owner.release(request)
                    raise

            def process_request_thread(self, request, client_address):
                try:
                    super().process_request_thread(request, client_address)
                finally:
                    owner.release(request)

        self.server = Server(('127.0.0.1', 0), Handler)
        self.address = '127.0.0.1:%d' % self.server.server_address[1]
        self.worker = None

    def release(self, sock):
        with self.lock:
            self.sockets.discard(sock)
            self.active -= 1
            self.lock.notify_all()
        self.slots.release()

    def add(self, ident, kind, **fields):
        event = dict(id=ident, ev=kind, t=time.monotonic(), **fields)
        with self.lock:
            if len(self.events) < self.max_events:
                self.events.append(event)
            else:
                self.dropped += 1
            self.lock.notify_all()

    def snapshot(self):
        with self.lock:
            return dict(mode=self.mode, address=self.address, events=list(self.events),
                        dropped=self.dropped, active=self.active, accepted=self.accepted, rejected=self.rejected)

    def wait_idle(self, seconds):
        deadline = time.monotonic() + seconds
        with self.lock:
            while self.active and time.monotonic() < deadline:
                self.lock.wait(max(0, deadline - time.monotonic()))
            return self.active == 0

    def __enter__(self):
        self.worker = threading.Thread(target=self.server.serve_forever, kwargs={'poll_interval': .05})
        self.worker.start()
        return self

    def __exit__(self, *_):
        if self.stopping.is_set():
            return
        self.stopping.set()
        if self.worker is not None:
            self.server.shutdown()
            self.worker.join(1)
        with self.lock:
            active_sockets = list(self.sockets)
        for sock in active_sockets:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.server.server_close()  # non-daemon handlers join after shutdown/50-ms polls
        if not self.wait_idle(1) or (self.worker is not None and self.worker.is_alive()):
            raise RuntimeError('proxy cleanup failed')

    def read_header(self, sock):
        data = bytearray()
        deadline = time.monotonic() + 2
        while not data.endswith(b'\r\n\r\n'):
            if self.stopping.is_set() or time.monotonic() >= deadline or len(data) >= 8192:
                raise ValueError('header deadline or size')
            ready, _, _ = select.select([sock], [], [], .05)
            if ready:
                chunk = sock.recv(1)  # preserves any pipelined bytes for the relay
                if not chunk:
                    raise ValueError('header EOF')
                data.extend(chunk)
        lines = bytes(data[:-4]).decode('ascii').split('\r\n')
        if len(lines) > 128:
            raise ValueError('too many headers')
        method, authority, version = lines[0].split(' ')
        if method != 'CONNECT' or version not in ('HTTP/1.0', 'HTTP/1.1'):
            raise ValueError('not CONNECT')
        host, port_text = authority.split(':')
        if host not in ('127.0.0.1', 'localhost') or not port_text.isdecimal():
            raise ValueError('not loopback authority')
        port = int(port_text)
        if port not in self.ports:
            raise ValueError('target port not allowed')
        headers = []
        for line in lines[1:]:
            name, value = line.split(':', 1)
            if not name or any(ord(c) < 33 or ord(c) > 126 for c in name):
                raise ValueError('bad header name')
            if any(ord(c) < 32 and c != '\t' for c in value):
                raise ValueError('bad header value')
            headers.append([name, value.strip()])
        return authority, port, headers

    def handle(self, sock):
        with self.lock:
            self.next_id += 1
            ident = self.next_id
        deadline = time.monotonic() + 12
        sock.settimeout(1)
        try:
            try:
                authority, port, headers = self.read_header(sock)
            except (ValueError, UnicodeError):
                self.add(ident, 'invalid')
                sock.sendall(b'HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                return
            self.add(ident, 'connect', authority=authority, headers=headers)
            if self.mode == 'deny':
                sock.sendall(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                self.add(ident, 'denied', status=403)
                return
            if self.mode == 'close':
                self.add(ident, 'closed_without_response')
                return
            if self.mode == 'silent':
                self.add(ident, 'silent')
                while not self.stopping.is_set() and time.monotonic() < deadline:
                    ready, _, _ = select.select([sock], [], [], .05)
                    if ready and not sock.recv(4096):
                        return
                self.add(ident, 'hold_deadline' if not self.stopping.is_set() else 'stopped')
                return
            with socket.create_connection(('127.0.0.1', port), timeout=1) as upstream:
                self.add(ident, 'upstream', port=port)
                sock.sendall(b'HTTP/1.1 200 Connection Established\r\n\r\n')
                self.add(ident, 'tunnel')
                while not self.stopping.is_set() and time.monotonic() < deadline:
                    readable, _, _ = select.select([sock, upstream], [], [], .05)
                    for source in readable:
                        payload = source.recv(16384)
                        if not payload:
                            return
                        (upstream if source is sock else sock).sendall(payload)
                self.add(ident, 'relay_deadline' if not self.stopping.is_set() else 'stopped')
        except OSError as error:
            self.add(ident, 'io_error', errno=error.errno)
        finally:
            self.add(ident, 'end')
```

- [ ] **Step 4: Run GREEN and mutation**:

```bash
python3 -I Tools/testserver/proxy_selftest.py -v
```

Expected: all four tests pass. Temporarily change `self.mode == 'deny'` to `self.mode == 'never-deny'`, run `python3 -I Tools/testserver/proxy_selftest.py ProxyTest.test_denial_close_silence_and_cleanup -v`, and require a failure because the deny leg returned 200. Restore that exact line and run the whole selftest again. Separately remove `port not in self.ports`'s refusal and require `test_bad_authority_header_limit_and_slow_header` to fail, then restore and rerun. Record both actual failures, not only a test total.

- [ ] **Step 5: Commit reviewed task paths only**:

```bash
git add Tools/testserver/proxy.py Tools/testserver/proxy_selftest.py
git commit -m "test: add bounded loopback CONNECT fixture" -m "Co-Authored-By: Codex <noreply@openai.com>"
```

### Task 2: Raw proxy option and loss-detecting callback recorder

**Files:** Modify `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h`, `.cpp`; create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTraceTest.cpp`.

**Interfaces:** `FRigOptions::ProxyAddress: TOptional<FString>` (unset passes null; set requires nonempty `host:port`); `MaxRecordedEvents: int32` (default `MAX_int32`, B1 uses 4096); `FRig::DroppedEventCount() const -> int32`; `CaptureErrorInput(FEvent&, const void*, size_t)`; `FTraceIdentity::Observe(const void*, int32, bool) -> FTraceIdentity::FToken`; new `FEvent` fields shown below. No transport seam changes.

- [ ] **Step 1: Add deterministic tests**:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsProbe.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestProxyInputTest, "Probe.VaRest.Lws.Rig.ProxyErrorInput",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestProxyInputTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    using VaRest::LwsProbe::FEvent;
    FEvent NullInput, EmptyInput, BinaryInput, LongInput;
    const uint8 Empty = 0;
    const uint8 Binary[] = { 'x', 0, 'y' };
    TArray<uint8> LongBytes;
    LongBytes.Init('a', 300);
    CaptureErrorInput(NullInput, nullptr, 0);
    CaptureErrorInput(EmptyInput, &Empty, 0);
    CaptureErrorInput(BinaryInput, Binary, sizeof(Binary));
    CaptureErrorInput(LongInput, LongBytes.GetData(), LongBytes.Num());
    TestFalse(TEXT("null is absent"), NullInput.bInputPresent);
    TestTrue(TEXT("present empty stays present"), EmptyInput.bInputPresent);
    TestEqual(TEXT("empty has zero length"), EmptyInput.Len, int64(0));
    TestTrue(TEXT("embedded zero survives"), BinaryInput.Bytes == TArray<uint8>({ 'x', 0, 'y' }));
    TestEqual(TEXT("raw length survives cap"), LongInput.Len, int64(300));
    TestEqual(TEXT("kept bytes capped"), LongInput.Bytes.Num(), 256);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestProxyIdentityTest, "Probe.VaRest.Lws.Rig.ProxyIdentity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestProxyIdentityTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    FTraceIdentity Identities;
    const void* Pointer = reinterpret_cast<const void*>(UPTRINT(1)); // never dereferenced
    const auto First = Identities.Observe(Pointer, 7, false);
    const auto Again = Identities.Observe(Pointer, 0, false);
    const auto End = Identities.Observe(Pointer, 0, true);
    const auto Reused = Identities.Observe(Pointer, 8, false);
    TestTrue(TEXT("token is nonzero"), First.Id > 0);
    TestEqual(TEXT("repeat create retains token"), Again.Id, First.Id);
    TestEqual(TEXT("destroy retains token"), End.Id, First.Id);
    TestEqual(TEXT("unresolved callbacks inherit connection"), Again.Conn, 7);
    TestTrue(TEXT("reused address is a new lifetime"), Reused.Id != First.Id);
    TestEqual(TEXT("new lifetime has new owner"), Reused.Conn, 8);
    TestEqual(TEXT("null has no token"), Identities.Observe(nullptr, 0, false).Id, 0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestProxyTraceLimitTest, "Probe.VaRest.Lws.Rig.ProxyTraceLimit",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestProxyTraceLimitTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    FRigOptions Options;
    Options.MaxRecordedEvents = 128;
    FRig Rig(FServer(), MoveTemp(Options));
    if (!TestTrue(TEXT("context exists"), Rig.IsReady())) { return false; }
    for (int32 Index = 0; Index < 130; ++Index) { Rig.Record(0, Marker, Index, TEXT("bounded")); }
    Rig.Shutdown();
    const auto Seen = Rig.Events();
    TestEqual(TEXT("record cap"), Seen.Num(), 128);
    TestTrue(TEXT("loss is explicit"), Rig.DroppedEventCount() >= 2);
    for (int32 Index = 0; Index < Seen.Num(); ++Index)
    {
        TestEqual(TEXT("global ordered sequence"), Seen[Index].Sequence, int64(Index + 1));
    }
    return true;
}
#endif
```

- [ ] **Step 2: Run RED** using the guarded normal build. Expected compile failures name missing `CaptureErrorInput`, `FTraceIdentity` and fields. Restore an unrelated engine gate before proceeding; engine failures do not prove this RED.

- [ ] **Step 3: Add the following declarations** inside `VaRest::LwsProbe` after `FEvent`, and add these fields to `FEvent` (defaults retain existing callers):

```cpp
// Add inside FEvent:
int64 Sequence = 0;
int32 WsiToken = 0;
bool bInputPresent = false;
int32 CertDepth = -1;
int32 CertError = -1;
int32 Preverify = -1;

// Add after FEvent:
/** Service thread (or stopped-context destruction): bounded input capture, no addresses logged. */
void CaptureErrorInput(FEvent& OutEvent, const void* Input, size_t InputLength);

/** Service thread, then game thread only after join. Keys never leave this object. */
struct FTraceIdentity
{
    struct FToken { int32 Id = 0; int32 Conn = 0; };
    TMap<const void*, FToken> Live;
    int32 Next = 1;
    FToken Observe(const void* Pointer, int32 ConnectionId, bool bDestroy)
    {
        if (!Pointer) { return {}; }
        FToken& Token = Live.FindOrAdd(Pointer);
        if (Token.Id == 0) { Token.Id = Next++; }
        if (ConnectionId != 0) { Token.Conn = ConnectionId; }
        const FToken Result = Token;
        if (bDestroy) { Live.Remove(Pointer); }
        return Result;
    }
};

// Add inside FRigOptions:
TOptional<FString> ProxyAddress;
int32 MaxRecordedEvents = MAX_int32;

// Add in FRig public:
/** Any thread: number omitted by the explicit event cap; nonzero invalidates evidence. */
int32 DroppedEventCount() const;

// Add in FRig private:
TArray<ANSICHAR> ProxyUtf8;
FTraceIdentity TraceIdentity;
int32 ConnectingId = 0;
int64 NextSequence = 1;
int32 DroppedEvents = 0;
```

- [ ] **Step 4: Wire the existing rig at these exact anchors**. In the constructor, immediately after `Info.user = this;`, add:

```cpp
check(Options.MaxRecordedEvents > 0);
if (Options.ProxyAddress.IsSet())
{
    check(!Options.ProxyAddress.GetValue().IsEmpty());
    Info.http_proxy_address = VaRestLwsProbePrivate::Utf8Copy(Options.ProxyAddress.GetValue(), ProxyUtf8);
}
```

The public pinned header at `libwebsockets.h:2734–2738` documents the proxy fields; the engine uses the `host:port` string directly at `LwsWebSocketsManager.cpp:196–203`. Keep the UTF-8 array for the rig lifetime. Leave the port member zero, matching that engine path; live explicit controls must establish whether it works for this archive.

In `ConnectNow`, replace only the call line with:

```cpp
ConnectingId = Conn.Id;
lws* Made = lws_client_connect_via_info(&Info);
ConnectingId = 0;
```

In `Handle`, replace `Event.Conn = Conn ? Conn->Id : 0;` with:

```cpp
const FTraceIdentity::FToken Identity = TraceIdentity.Observe(Wsi, Conn ? Conn->Id : ConnectingId,
    Reason == LWS_CALLBACK_WSI_DESTROY);
Event.Conn = Identity.Conn;
Event.WsiToken = Identity.Id;
```

In its `CLIENT_CONNECTION_ERROR` case, replace `Event.Note = Utf8Text(In, Len);` with `CaptureErrorInput(Event, In, Len);`. In its certificate case, after obtaining `Store`, add:

```cpp
Event.CertDepth = X509_STORE_CTX_get_error_depth(Store);
Event.CertError = X509_STORE_CTX_get_error(Store);
Event.Preverify = static_cast<int32>(Len);
```

Replace the header's existing rig lifetime comment “neither opened nor ended” with “not yet destroyed by LWS; stop/join/destroy the earlier rig before creating another context”. All of these instrumentation changes run before `Append` and before `Spec.Hook`/verify handlers. The certificate callback's `len` is preverify_ok, not a byte length (`libwebsockets.h:1015–1044`, read during planning). Add the helper definitions and replace the complete `Append` function with:

```cpp
void VaRest::LwsProbe::CaptureErrorInput(FEvent& OutEvent, const void* Input, size_t InputLength)
{
    OutEvent.bInputPresent = Input != nullptr;
    OutEvent.Len = static_cast<int64>(InputLength);
    OutEvent.Bytes.Reset();
    const int32 Take = static_cast<int32>(FMath::Min<size_t>(InputLength, 256));
    if (Input && Take > 0)
    {
        OutEvent.Bytes.Append(static_cast<const uint8*>(Input), Take);
    }
    OutEvent.Note = Utf8Text(Input, Input ? Take : 0);
}

void VaRest::LwsProbe::FRig::Append(FEvent&& Event)
{
    FScopeLock Lock(&EventsLock);
    Event.Sequence = NextSequence++;
    if (Recorded.Num() >= Options.MaxRecordedEvents)
    {
        ++DroppedEvents;
        return;
    }
    Recorded.Add(MoveTemp(Event));
}

int32 VaRest::LwsProbe::FRig::DroppedEventCount() const
{
    FScopeLock Lock(&EventsLock);
    return DroppedEvents;
}
```

- [ ] **Step 5: Run GREEN and restore tests** after the guarded build:

```bash
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Rig
```

Expected: all existing rig tests and three new tests pass. Change `OutEvent.bInputPresent = Input != nullptr;` to `OutEvent.bInputPresent = InputLength != 0;`, rebuild under the guard, and run `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Rig.ProxyErrorInput`; require present-empty failure. Restore that one line and rerun. Change `if (bDestroy) { Live.Remove(Pointer); }` to `if (false) { Live.Remove(Pointer); }`; require `ProxyIdentity`'s reused-address check to fail, then restore, rebuild and run the whole Rig prefix. Do not proceed with a dropped-event counter that always reports zero.

- [ ] **Step 6: Commit**:

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.cpp Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTraceTest.cpp
git commit -m "test: record proxy callback identity and raw error input" -m "Co-Authored-By: Codex <noreply@openai.com>"
```

### Task 3: One isolated raw attempt with complete trace export

**Files:** Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTest.cpp`.

**Interfaces:** Automation `Probe.VaRest.Lws.Proxy.Attempt`; inputs `-VaRestProxyCase`, `-VaRestProxyTrace`, `-VaRestProxyCa`, `-VaRestProxyPath`, optional `-VaRestTestProxy`, flags `-VaRestProxyTls`, `-VaRestProxyUntrusted`. Existing runner server flags remain required. Output schema 1 below; no HTTP requests and no environment writes. Options use 100 ms service wait, 2 s LWS stage timeout, 8 s primary attempt deadline, bounded close wait then forced context shutdown explicitly marked in output.

- [ ] **Step 1: Register the test with a deliberate failure**:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsProbe.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "Misc/AutomationTest.h"
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProxyAttemptTest, "Probe.VaRest.Lws.Proxy.Attempt",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestLwsProxyAttemptTest::RunTest(const FString& Parameters)
{
    AddError(TEXT("proxy trace exporter is not implemented"));
    return false;
}
#endif
```

- [ ] **Step 2: Guarded build, then RED**: `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Proxy.Attempt`; expected failure is the named missing exporter, not “matched no tests”.

- [ ] **Step 3: Replace the entire new file**:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLwsProbe.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "../WebSocket/VaRestWsTestUtil.h"

namespace VaRest::LwsProbe::VaRestLwsProbeProxyTestPrivate
{
    FVaRestJson Encode(const FEvent& Item)
    {
        return FVaRestJson::Object()
            .Set(TEXT("seq"), Item.Sequence).Set(TEXT("conn"), Item.Conn)
            .Set(TEXT("wsi"), Item.WsiToken).Set(TEXT("reason"), Item.Reason)
            .Set(TEXT("name"), ReasonName(Item.Reason)).Set(TEXT("time"), Item.Time)
            .Set(TEXT("len"), Item.Len).Set(TEXT("input_present"), Item.bInputPresent)
            .Set(TEXT("hex"), HexOf(Item.Bytes)).Set(TEXT("note"), Item.Note)
            .Set(TEXT("depth"), Item.CertDepth).Set(TEXT("x509_error"), Item.CertError)
            .Set(TEXT("preverify"), Item.Preverify);
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProxyAttemptTest, "Probe.VaRest.Lws.Proxy.Attempt",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestLwsProxyAttemptTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::LwsProbe;
    using VaRest::LwsProbe::FEvent;
    VaRest::WsTest::ExpectLwsLines(*this);
    FString CaseId, TraceFile, CaFile, RequestPath, ExplicitProxy;
    const TCHAR* Command = FCommandLine::Get();
    const bool bInputs = FParse::Value(Command, TEXT("VaRestProxyCase="), CaseId)
        && FParse::Value(Command, TEXT("VaRestProxyTrace="), TraceFile)
        && FParse::Value(Command, TEXT("VaRestProxyCa="), CaFile)
        && FParse::Value(Command, TEXT("VaRestProxyPath="), RequestPath);
    if (!TestTrue(TEXT("matrix arguments supplied"), bInputs)) { return false; }
    FServer Site;
    if (!FindServer(*this, Site, true)) { return false; }
    TArray<uint8> Ca;
    if (!TestTrue(TEXT("local test CA readable"), FFileHelper::LoadFileToArray(Ca, *CaFile))) { return false; }
    const bool bTls = FParse::Param(Command, TEXT("VaRestProxyTls"));
    const bool bUntrusted = FParse::Param(Command, TEXT("VaRestProxyUntrusted"));
    const bool bExplicit = FParse::Value(Command, TEXT("VaRestTestProxy="), ExplicitProxy);
    FRigOptions Options;
    Options.ServiceTimeoutMs = 100;
    Options.TimeoutSecs = 2;
    Options.MaxRecordedEvents = 4096;
    // TLS is enabled even on ws because the redirect control may switch schemes.
    Options.Tls = ETls::LwsGlobalInit;
    if (bExplicit) { Options.ProxyAddress = ExplicitProxy; }
    if (!bUntrusted)
    {
        Options.OnLoadExtraClientVerifyCerts = [Pem = MoveTemp(Ca)](SSL_CTX* Context)
        {
            AddPemToStore(Context, Pem);
        };
    }
    const FString ConfiguredProxy = FPlatformHttp::GetConfiguredProxyAddress();
    FRig Rig(Site, MoveTemp(Options));
    if (!TestTrue(TEXT("raw context ready"), Rig.IsReady())) { return false; }
    FConnectSpec Spec;
    Spec.bTls = bTls;
    Spec.Address = TEXT("127.0.0.1");
    // DNS SAN verification succeeds without production IP-literal repair; TCP remains numeric.
    Spec.Host = FString::Printf(TEXT("localhost:%d"), bTls ? Site.TlsPort : Site.Port);
    Spec.Path = RequestPath;
    Spec.AppendHeaders.Add(TEXT("Authorization: Bearer b1-probe-marker"));
    const double Begin = FPlatformTime::Seconds();
    const int32 ConnectionId = Rig.Connect(MoveTemp(Spec));
    const bool bPrimaryCompleted = Rig.WaitFor([ConnectionId](const TArray<FEvent>& All)
    {
        return All.ContainsByPredicate([ConnectionId](const FEvent& Item)
        {
            return Item.Conn == ConnectionId && (Item.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED
                || Item.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR || Item.Reason == LWS_CALLBACK_WSI_DESTROY);
        });
    }, 8.0);
    const double PrimarySeconds = FPlatformTime::Seconds() - Begin;
    const bool bOpened = Opened(Rig.Events(ConnectionId));
    bool bEchoed = false;
    if (bOpened)
    {
        Rig.WriteText(ConnectionId, TEXT("b1-echo"));
        bEchoed = Rig.WaitFor([ConnectionId](const TArray<FEvent>& All)
        {
            return All.ContainsByPredicate([ConnectionId](const FEvent& Item)
            {
                return Item.Conn == ConnectionId && Item.Reason == LWS_CALLBACK_CLIENT_RECEIVE
                    && EventText(Item).Equals(TEXT("b1-echo"), ESearchCase::CaseSensitive);
            });
        }, 2.0);
        Rig.Close(ConnectionId, 1000, TEXT("b1-done"));
    }
    const bool bDestroyedBeforeShutdown = Rig.WaitReason(ConnectionId, LWS_CALLBACK_WSI_DESTROY, 3.0);
    Rig.Shutdown(); // thread joined, context destroyed; only now collect final callbacks
    const auto Seen = Rig.Events(); // include context callbacks and unresolved conn=0 evidence
    FVaRestJson Rows = FVaRestJson::Array();
    for (const FEvent& Item : Seen) { Rows.Add(VaRestLwsProbeProxyTestPrivate::Encode(Item)); }
    const bool bDestroyed = Seen.ContainsByPredicate([ConnectionId](const FEvent& Item)
    {
        return Item.Conn == ConnectionId && Item.Reason == LWS_CALLBACK_WSI_DESTROY;
    });
    const FVaRestJson Trace = FVaRestJson::Object()
        .Set(TEXT("schema"), 1).Set(TEXT("case"), CaseId).Set(TEXT("connection"), ConnectionId)
        .Set(TEXT("configured_proxy_absent"), ConfiguredProxy.IsEmpty())
        .Set(TEXT("explicit_proxy"), bExplicit ? ExplicitProxy : FString())
        .Set(TEXT("tls"), bTls).Set(TEXT("primary_completed"), bPrimaryCompleted)
        .Set(TEXT("primary_seconds"), PrimarySeconds).Set(TEXT("opened"), bOpened)
        .Set(TEXT("echoed"), bEchoed).Set(TEXT("destroyed_before_shutdown"), bDestroyedBeforeShutdown)
        .Set(TEXT("destroyed"), bDestroyed).Set(TEXT("shutdown_completed"), true)
        .Set(TEXT("dropped_events"), Rig.DroppedEventCount())
        .Set(TEXT("lws_version"), UTF8_TO_TCHAR(lws_get_library_version()))
        .Set(TEXT("openssl_version"), UTF8_TO_TCHAR(OpenSSL_version(OPENSSL_VERSION)))
        .Set(TEXT("events"), Rows);
    TestTrue(TEXT("trace saved"), FFileHelper::SaveStringToFile(Trace.ToString(), *TraceFile,
        FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
    TestEqual(TEXT("trace loss"), Rig.DroppedEventCount(), 0);
    TestTrue(TEXT("bounded primary attempt"), bPrimaryCompleted);
    // No assertion about counts, error text, routing or redirect-follow behavior here.
    NoteOutcome(*this, TEXT("opened"), bOpened);
    NoteOutcome(*this, TEXT("echoed"), bEchoed);
    return true;
}
#endif
```

The engine config absence field is deliberately a boolean: an unrelated configured proxy may contain credentials and must not enter artifacts. The generic platform's source at `GenericPlatformHttp.cpp:640–660` reads configured proxy then OS proxy; the raw rig's null option is still independently recorded. Environment rows require absence before claiming engine-equivalent null fallback. If this boolean is false, stop those rows and let root choose an explicit config-isolated child; do not overwrite the user's INI.

- [ ] **Step 4: Guarded build, then GREEN direct controls**. These use the existing runner's own targets, local CA and a fresh output file; no proxy is involved yet:

```bash
VAREST_EXTRA_ARGS='-VaRestProxyCase=direct-ws -VaRestProxyTrace=/w/Unreal/VaRestHost/Saved/B1Proxy/direct-ws.json -VaRestProxyCa=/w/Unreal/VaRest/Tools/testserver/certs/test-ca.pem -VaRestProxyPath=/ws/echo?key=b1-direct-ws' Tools/run_tests.sh --no-build Probe.VaRest.Lws.Proxy.Attempt
VAREST_EXTRA_ARGS='-VaRestProxyCase=direct-wss -VaRestProxyTrace=/w/Unreal/VaRestHost/Saved/B1Proxy/direct-wss.json -VaRestProxyCa=/w/Unreal/VaRest/Tools/testserver/certs/test-ca.pem -VaRestProxyPath=/ws/echo?key=b1-direct-wss -VaRestProxyTls' Tools/run_tests.sh --no-build Probe.VaRest.Lws.Proxy.Attempt
python3 -I - <<'PY'
import json
from pathlib import Path
for name in ('direct-ws', 'direct-wss'):
    row = json.loads((Path('/w/Unreal/VaRestHost/Saved/B1Proxy') / (name + '.json')).read_text())
    assert row['opened'] and row['echoed'] and row['destroyed'] and row['shutdown_completed'], row
    assert row['dropped_events'] == 0
    assert [e['seq'] for e in row['events']] == list(range(1, len(row['events']) + 1))
    assert any(e['name'] == 'WSI_CREATE' for e in row['events'])
    if name == 'direct-wss':
        assert any(e['depth'] >= 0 for e in row['events'])
PY
```

Before relying on exact `name` spelling in the external code, verify `ReasonName` in the baseline: it emits `WSI_CREATE`, `WSI_DESTROY`, `APPEND_HANDSHAKE_HEADER`, `PERFORM_SERVER_CERT_VERIFICATION`, `CONNECTION_ERROR`, `ESTABLISHED`. The validator below uses numeric reasons for comparisons and displays names only.

- [ ] **Step 5: Restore test**. Temporarily change `Spec.Path = RequestPath;` to `Spec.Path = TEXT("/ws/status/403?key=b1-direct-ws");`, rebuild, rerun direct-ws and its JSON assertion. Require the assertion of `opened`/`echoed` to fail even if the automation attempt itself completes successfully; this pins the distinction between a measured negative outcome and a broken positive control. Restore, rebuild, rerun both controls. Then commit:

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTest.cpp
git commit -m "test: export one isolated raw proxy attempt" -m "Co-Authored-By: Codex <noreply@openai.com>"
```

### Task 4: Controlled child matrix and independent parent collection

**Files:** Modify `Tools/run_tests.sh`; create `Tools/run_lws_proxy_probe.py`.

**Interfaces:** `python3 -I Tools/run_lws_proxy_probe.py --out <fresh-directory> [--only <case>]`; `--selftest` runs without UE. Matrix case IDs are defined in the complete code below. `VAREST_LWS_PROXY_ENV=<127.0.0.1:port>` and optional `VAREST_LWS_PROXY_NO_PROXY=*` are accepted by the shell runner only for the exact B1 attempt filter and only with externally managed fixtures. Ordinary calls keep scrubbing all proxy variables. Parent output `case.json` records harness failures separately from raw outcomes. Manifest `acquisition_only=true` and `harness_ok` report collection only; they never certify positive controls. Task 5 must accept the complete matrix before it is usable for B2. Cleanup tests include an exited shell whose editor descendant remains alive in its owned process group.

- [ ] **Step 1: Add the tests first**. Create the controller file initially with the imports, `ControllerTests` class and `main` dispatch from the complete implementation in Step 3, without `direct_json`, `child_environment`, `check_trace` or `run_matrix`. Run `python3 -I Tools/run_lws_proxy_probe.py --selftest`; expected tests fail on those missing helpers. No UE process should start in selftest mode.

- [ ] **Step 2: Add the runner's explicit child environment**. Immediately before `START=$SECONDS`, insert:

```bash
PROXY_CHILD_ENV=(env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY -u all_proxy -u ALL_PROXY -u no_proxy -u NO_PROXY)
if [[ -n "${VAREST_LWS_PROXY_ENV:-}" || -n "${VAREST_LWS_PROXY_NO_PROXY:-}" ]]; then
	[[ "$FILTER" == '^Probe.VaRest.Lws.Proxy.Attempt' && "${VAREST_NO_TESTSERVER:-0}" == 1 ]] || {
		echo 'run_tests: proxy environment is restricted to the isolated B1 attempt' >&2; exit 2;
	}
	[[ "${VAREST_LWS_PROXY_ENV:-}" =~ ^127\.0\.0\.1:([0-9]{1,5})$ ]] || {
		echo 'run_tests: proxy environment must name a loopback listener' >&2; exit 2;
	}
	PROXY_PORT="${BASH_REMATCH[1]}"
	[[ $((10#$PROXY_PORT)) -gt 0 && $((10#$PROXY_PORT)) -lt 65536 ]] || exit 2
	[[ -z "${VAREST_LWS_PROXY_NO_PROXY:-}" || "$VAREST_LWS_PROXY_NO_PROXY" == '*' ]] || exit 2
	PROXY_CHILD_ENV+=("http_proxy=http://${VAREST_LWS_PROXY_ENV}")
	if [[ -n "${VAREST_LWS_PROXY_NO_PROXY:-}" ]]; then
		PROXY_CHILD_ENV+=("no_proxy=$VAREST_LWS_PROXY_NO_PROXY")
	fi
fi
```

In the existing `timeout` invocation replace only

```bash
env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY "$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
```

with

```bash
"${PROXY_CHILD_ENV[@]}" "$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
```

Replace the old “No proxy variables until W1b B1” comment with “Ordinary runs scrub proxy variables; only the exact B1 child may receive a controlled lowercase proxy environment.” This is process creation, not mutation of an active editor's environment. Host/script variables never impersonate `$HOME` or `$CODEX_HOME`.

- [ ] **Step 3: Implement the complete controller** `Tools/run_lws_proxy_probe.py`:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""B1 raw proxy matrix. No build, no production classifier, no HTTP control in UE."""
import argparse
import contextlib
import hashlib
import http.client
import http.server
import json
import os
import pathlib
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.parse
import uuid

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'Tools' / 'testserver'))
from proxy import Proxy

PROXY_VARS = ('http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY',
              'all_proxy', 'ALL_PROXY', 'no_proxy', 'NO_PROXY')
CASES = tuple((mode + '-' + scheme, mode, scheme == 'wss')
              for mode in ('direct', 'explicit', 'deny', 'close', 'silent',
                           'redirect', 'env', 'precedence', 'env-noproxy')
              for scheme in ('ws', 'wss')) + (
                  ('redirect-cross-ws', 'redirect-cross', False),
                  ('untrusted-wss', 'untrusted', True),
                  ('upgrade403-ws', 'upgrade403', False),
                  ('upgrade403-wss', 'upgrade403', True))


def write_json(path, value):
    pathlib.Path(path).write_text(json.dumps(value, indent=2, sort_keys=True) + '\n', encoding='utf-8')


def direct_json(base, path):
    parsed = urllib.parse.urlsplit(base)
    if parsed.scheme != 'http' or parsed.hostname != '127.0.0.1' or not parsed.port:
        raise ValueError('control must be numeric loopback HTTP')
    conn = http.client.HTTPConnection('127.0.0.1', parsed.port, timeout=5)
    try:
        conn.request('GET', path)
        response = conn.getresponse()
        body = response.read(2 * 1024 * 1024 + 1)
        if response.status != 200 or len(body) > 2 * 1024 * 1024:
            raise RuntimeError('invalid control response')
        return json.loads(body)
    finally:
        conn.close()


def child_environment(base, extra, proxy='', no_proxy=''):
    result = {k: v for k, v in base.items() if k not in PROXY_VARS
              and not k.startswith('VAREST_LWS_PROXY_')}
    if any(c.isspace() for arg in extra for c in arg):
        raise ValueError('runner extra arguments require whitespace-free paths')
    result.update(VAREST_NO_TESTSERVER='1', VAREST_TEST_TIMEOUT='90',
                  VAREST_EXTRA_ARGS=' '.join(extra))
    if proxy:
        result['VAREST_LWS_PROXY_ENV'] = proxy
    if no_proxy:
        result['VAREST_LWS_PROXY_NO_PROXY'] = no_proxy
    return result


def check_trace(trace, case_id):
    problems = []
    if trace.get('schema') != 1 or trace.get('case') != case_id:
        problems.append('trace identity/schema')
    events = trace.get('events', [])
    if not events or [e.get('seq') for e in events] != list(range(1, len(events) + 1)):
        problems.append('missing/unordered events')
    if trace.get('dropped_events') != 0:
        problems.append('dropped trace events')
    if not trace.get('shutdown_completed'):
        problems.append('context shutdown incomplete')
    if not trace.get('destroyed'):
        problems.append('attempt destruction not recorded')
    if not trace.get('primary_completed'):
        problems.append('attempt observation deadline')
    for event in events:
        if event.get('reason') == 1:
            if isinstance(event.get('len'), int) and event['len'] < 0:
                problems.append('negative callback length')
            if not isinstance(event.get('input_present'), bool) or not isinstance(event.get('len'), int):
                problems.append('error input metadata missing')
            try:
                raw = bytes.fromhex(event['hex'])
                expected = min(event['len'], 256) if event['input_present'] else 0
                if len(raw) != expected:
                    problems.append('error byte capture differs from length/presence')
            except (KeyError, TypeError, ValueError):
                problems.append('malformed error hex')
    return problems


def group_running(group_id):
    # Linux-only driver: a dead runner can leave its editor alive in the owned group.
    for entry in pathlib.Path('/proc').iterdir():
        if not entry.name.isdecimal():
            continue
        try:
            fields = (entry / 'stat').read_text().rsplit(')', 1)[1].split()
            if int(fields[2]) == group_id and fields[0] not in ('Z', 'X'):
                return True
        except (OSError, IndexError, ValueError):
            continue
    return False


def stop_group(process):
    # Never skip descendants merely because the shell leader has exited.
    for sig, seconds in ((signal.SIGTERM, 3), (signal.SIGKILL, 2)):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            break
        deadline = time.monotonic() + seconds
        while group_running(process.pid) and time.monotonic() < deadline:
            threading.Event().wait(.02)
        if not group_running(process.pid):
            break
    if process.poll() is None:
        process.wait(timeout=2)
    if group_running(process.pid):
        raise RuntimeError('owned editor process group did not drain')


def invoke_runner(env, output):
    with output.open('wb') as stream:
        process = subprocess.Popen(['bash', str(ROOT / 'Tools' / 'run_tests.sh'), '--no-build',
                                    'Probe.VaRest.Lws.Proxy.Attempt'], cwd=ROOT, env=env,
                                   stdout=stream, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            return process.wait(timeout=120)
        except subprocess.TimeoutExpired:
            return 124
        finally:
            stop_group(process)


@contextlib.contextmanager
def target_server(directory):
    ready = directory / 'server.ready'
    with (directory / 'server.log').open('wb') as log:
        process = subprocess.Popen([sys.executable, '-I', str(ROOT / 'Tools' / 'testserver' / 'server.py'),
                                    '--host', '127.0.0.1', '--port', '0', '--tls-port', '0',
                                    '--ready-file', str(ready), '--parent-pid', str(os.getpid())],
                                   stdout=log, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 10
            while not ready.exists() and process.poll() is None and time.monotonic() < deadline:
                threading.Event().wait(.05)
            if not ready.exists():
                raise RuntimeError('target fixture failed; see server.log')
            urls = ready.read_text().splitlines()
            plain = next(u for u in urls if u.startswith('http://'))
            tls = next(u for u in urls if u.startswith('https://'))
            yield plain, tls
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)


def run_case(directory, case, plain, tls):
    case_id, mode, use_tls = case
    directory.mkdir()
    source_key = 'b1-' + uuid.uuid4().hex
    target_key = 'b1-' + uuid.uuid4().hex
    ports = [urllib.parse.urlsplit(plain).port, urllib.parse.urlsplit(tls).port]
    source_path = '/ws/echo?key=' + source_key
    if mode.startswith('redirect'):
        destination = ('wss' if use_tls or mode == 'redirect-cross' else 'ws')
        destination_port = ports[1] if destination == 'wss' else ports[0]
        location = '%s://localhost:%d/ws/echo?key=%s' % (destination, destination_port, target_key)
        # Redirects to localhost can resolve ::1 first on this host. Use numeric TCP authority.
        # Raw TLS redirect hostname checks then legitimately measure the pinned archive's IP behavior.
        # Skip hostname check only on cross/redirect controls; trusted initial wss remains a separate row.
        location = location.replace('://localhost:', '://127.0.0.1:')
        source_path = '/ws/status/302?key=' + source_key + '&location=' + urllib.parse.quote(location, safe='')
    elif mode == 'upgrade403':
        source_path = '/ws/status/403?key=' + source_key
    errors = []
    result = dict(case=case_id, mode=mode, tls=use_tls, source_key=source_key,
                  target_key=target_key, target_port=ports[1] if use_tls else ports[0], control_access='parent-http.client-direct', harness_errors=errors)
    fixture_mode = mode if mode in ('deny', 'close', 'silent') else 'tunnel'
    with Proxy(fixture_mode, ports) as proxy_a, Proxy('tunnel', ports) as proxy_b:
        extra = ['-VaRestTestServer=' + plain, '-VaRestTestServerTls=' + tls,
                 '-VaRestProxyCase=' + case_id, '-VaRestProxyTrace=' + str(directory / 'trace.json'),
                 '-VaRestProxyCa=' + str(ROOT / 'Tools/testserver/certs/test-ca.pem'),
                 '-VaRestProxyPath=' + source_path]
        if use_tls:
            extra.append('-VaRestProxyTls')
        if mode == 'untrusted':
            extra.append('-VaRestProxyUntrusted')
        if mode.startswith('redirect'):
            extra.append('-VaRestProxyRedirectControl')
        if mode not in ('direct', 'env', 'env-noproxy'):
            extra.append('-VaRestTestProxy=' + proxy_a.address)
        environment_proxy = proxy_b.address if mode in ('env', 'precedence', 'env-noproxy') else ''
        env = child_environment(os.environ, extra, environment_proxy, '*' if mode == 'env-noproxy' else '')
        result.update(proxy_a=proxy_a.address, proxy_b=proxy_b.address, env_proxy=environment_proxy,
                      no_proxy='*' if mode == 'env-noproxy' else '', args=extra)
        try:
            result['runner_rc'] = invoke_runner(env, directory / 'runner.txt')
            result['group_drained'] = True
            if result['runner_rc'] != 0:
                errors.append('editor/automation runner failed')
        except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
            result['runner_rc'] = -1
            result['group_drained'] = False
            errors.append('runner setup/cleanup: ' + str(error))
        finally:
            host = pathlib.Path(env.get('VAREST_HOST', '/w/Unreal/VaRestHost'))
            log = host / 'Saved/Logs/VaRestTests.log'
            if log.exists():
                shutil.copyfile(log, directory / 'editor.log')
            else:
                errors.append('editor log missing')
        if not proxy_a.wait_idle(2) or not proxy_b.wait_idle(2):
            errors.append('proxy still active after child exit')
    # Both fixtures closed: no future target request can originate from this attempt.
    write_json(directory / 'proxy-a.json', proxy_a.snapshot())
    write_json(directory / 'proxy-b.json', proxy_b.snapshot())
    for label, key in (('source', source_key), ('target', target_key)):
        try:
            journal = direct_json(plain, '/ws/journal?key=' + key + '&wait_ms=2000')
            write_json(directory / (label + '.json'), journal)
            if journal.get('dropped') != 0:
                errors.append(label + ' journal dropped events')
            if journal.get('connections', 0) and not journal.get('ended'):
                errors.append(label + ' journal not ended')
        except (OSError, ValueError, RuntimeError, http.client.HTTPException) as error:
            errors.append(label + ' control fetch: ' + str(error))
    try:
        trace = json.loads((directory / 'trace.json').read_text())
        errors.extend(check_trace(trace, case_id))
        if mode in ('env', 'precedence', 'env-noproxy') and not trace.get('configured_proxy_absent'):
            errors.append('engine configured proxy not absent; null-fallback claim invalid')
    except (OSError, ValueError) as error:
        errors.append('trace missing/malformed: ' + str(error))
    for proxy in (proxy_a, proxy_b):
        if proxy.snapshot()['dropped']:
            errors.append('proxy journal dropped events')
    write_json(directory / 'case.json', result)
    return result


def run_matrix(out, only):
    out = out.resolve()
    out.mkdir(parents=True, exist_ok=False)  # never overwrite a prior evidence run
    selected = [case for case in CASES if only is None or case[0] == only]
    commit = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip()
    dirty = subprocess.check_output(['git', 'status', '--porcelain'], cwd=ROOT, text=True)
    manifest = dict(schema=1, commit=commit, dirty=dirty, cases=[c[0] for c in selected],
                    scope='Linux modular editor raw rig; no native/cooked runtime claim',
                    acquisition_only=True, results=[])
    write_json(out / 'manifest.json', manifest)
    with target_server(out) as (plain, tls):
        for case in selected:
            record = run_case(out / case[0], case, plain, tls)
            manifest['results'].append(dict(case=case[0], harness_ok=not record['harness_errors']))
            write_json(out / 'manifest.json', manifest)
    hashes = {str(path.relative_to(out)): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in out.rglob('*') if path.is_file()}
    write_json(out / 'sha256.json', hashes)
    return 0 if all(row['harness_ok'] for row in manifest['results']) else 1


class ControllerTests(unittest.TestCase):
    def test_child_environment_is_explicit(self):
        base = dict(http_proxy='secret', HTTPS_PROXY='secret', no_proxy='*',
                    VAREST_LWS_PROXY_ENV='old', VAREST_EXTRA_ARGS='old', PATH='/bin')
        clean = child_environment(base, ['-VaRestProxyCase=x'])
        self.assertFalse(set(PROXY_VARS).intersection(clean))
        self.assertNotIn('VAREST_LWS_PROXY_ENV', clean)
        picked = child_environment(base, ['-VaRestProxyCase=x'], '127.0.0.1:1234', '*')
        self.assertEqual(picked['VAREST_LWS_PROXY_ENV'], '127.0.0.1:1234')
        self.assertEqual(picked['VAREST_LWS_PROXY_NO_PROXY'], '*')
        self.assertEqual(base['http_proxy'], 'secret')
        with self.assertRaises(ValueError):
            child_environment(base, ['-VaRestProxyTrace=/bad path'])

    def test_direct_control_under_poisoned_proxy(self):
        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                body = b'{"control":"direct"}'
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            def log_message(self, *_):
                pass
        with http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler) as server:
            thread = threading.Thread(target=server.serve_forever, kwargs={'poll_interval': .05})
            thread.start()
            try:
                env = dict(os.environ)
                env.update({key: 'http://127.0.0.1:1' for key in PROXY_VARS})
                url = 'http://127.0.0.1:%d' % server.server_port
                result = subprocess.run([sys.executable, '-I', __file__, '--control-url', url],
                                        env=env, capture_output=True, text=True, timeout=5)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(json.loads(result.stdout), {'control': 'direct'})
            finally:
                server.shutdown()
                thread.join(1)

    def test_harness_failure_is_not_negative_outcome(self):
        trace = dict(schema=1, case='deny-ws', events=[dict(seq=1, reason=1,
                     input_present=False, len=0, hex='')], dropped_events=0,
                     primary_completed=True, shutdown_completed=True, destroyed=True, opened=False)
        self.assertEqual(check_trace(trace, 'deny-ws'), [])
        trace['events'][0]['input_present'] = True
        trace['events'][0]['len'] = 4
        self.assertIn('error byte capture differs from length/presence', check_trace(trace, 'deny-ws'))
        trace['dropped_events'] = 1
        self.assertIn('dropped trace events', check_trace(trace, 'deny-ws'))
        trace['case'] = 'other'
        self.assertIn('trace identity/schema', check_trace(trace, 'deny-ws'))

    def test_cleanup_after_runner_leader_exits(self):
        child_code = ('import subprocess,sys; '
                      'p=subprocess.Popen([sys.executable,"-c","import time; time.sleep(60)"],'
                      'stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL); '
                      'print(p.pid,flush=True)')
        process = subprocess.Popen([sys.executable, '-c', child_code], stdout=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            orphan_pid = int(process.stdout.readline())
            self.assertGreater(orphan_pid, 0)
            self.assertEqual(process.wait(timeout=3), 0)
            self.assertTrue(group_running(process.pid))
            stop_group(process)
            self.assertFalse(group_running(process.pid))
        finally:
            stop_group(process)
            process.stdout.close()

    def test_matrix_unique_and_required_controls(self):
        ids = [c[0] for c in CASES]
        self.assertEqual(len(ids), len(set(ids)))
        self.assertTrue({'direct-ws', 'direct-wss', 'explicit-ws', 'explicit-wss',
                         'deny-ws', 'deny-wss', 'close-ws', 'close-wss', 'silent-ws', 'silent-wss',
                         'redirect-cross-ws', 'untrusted-wss', 'upgrade403-wss', 'env-ws',
                         'precedence-wss', 'env-noproxy-wss'}.issubset(ids))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--only', choices=[c[0] for c in CASES])
    parser.add_argument('--selftest', action='store_true')
    parser.add_argument('--control-url')
    args = parser.parse_args()
    if args.selftest:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(ControllerTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.control_url:
        print(json.dumps(direct_json(args.control_url, '/')))
        return 0
    if args.out is None:
        parser.error('--out is required for a matrix')
    return run_matrix(args.out, args.only)


if __name__ == '__main__':
    sys.exit(main())
```

**Redirect control adjustment owned by this task:** In Task 3's child after `Spec.Path = RequestPath;`, add this complete code. It removes the known raw-LWS IP-name mismatch only for the explicitly labelled redirect measurements. It never changes chain verification, and the direct/explicit trusted wss rows still check the DNS SAN:

```cpp
if (FParse::Param(Command, TEXT("VaRestProxyRedirectControl")))
{
    Spec.SslFlags |= LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
}
```

Also add `.Set(TEXT("redirect_hostname_skip"), FParse::Param(Command, TEXT("VaRestProxyRedirectControl")))` to the root trace object before `.Set(TEXT("events"), Rows)`. Record this flag in the passport so nobody confuses redirect ordering with full TLS policy proof. Raw redirects use numeric loopback addresses because the final W1a source still resolves synchronously and tries its first address; B9 is out of scope. The certificate chain still uses the test CA, and B2 must exercise the production IP check/no-follow logic separately.

- [ ] **Step 4: GREEN selftests and shell guard**:

```bash
bash -n Tools/run_tests.sh
python3 -I Tools/run_lws_proxy_probe.py --selftest
VAREST_LWS_PROXY_ENV=127.0.0.1:1234 VAREST_NO_TESTSERVER=1 Tools/run_tests.sh --no-build VaRest.WebSocket.Module
```

Expected final command: exit 2 and “restricted to the isolated B1 attempt”; no editor started. After the guarded build for the redirect flag, run one direct and one explicit row using fresh directories:

```bash
python3 -I Tools/run_lws_proxy_probe.py --out /w/Unreal/VaRestHost/Saved/B1Proxy/task4-direct --only direct-ws
python3 -I Tools/run_lws_proxy_probe.py --out /w/Unreal/VaRestHost/Saved/B1Proxy/task4-explicit --only explicit-wss
```

Inspect `case.json` for empty `harness_errors`; direct must open/echo with no CONNECT; explicit-wss must open/echo and the chosen listener must contain CONNECT/upstream/tunnel, while the source journal contains one Upgrade. These are positive controls; if they fail, preserve their artifacts and fix the harness or report the archive outcome before the full matrix. Do not invent a passing callback count to continue.

- [ ] **Step 5: Restore test**. In `direct_json` temporarily replace `parsed.port` in the `HTTPConnection` construction with `1`; run `--selftest` and require `test_direct_control_under_poisoned_proxy` to fail, then restore and rerun. Measure the baseline environment row first:

```bash
python3 -I Tools/run_lws_proxy_probe.py --out /w/Unreal/VaRestHost/Saved/B1Proxy/task4-env-baseline --only env-ws
python3 -I - <<'PYENV'
import json
from pathlib import Path
p = Path('/w/Unreal/VaRestHost/Saved/B1Proxy/task4-env-baseline/env-ws/proxy-b.json')
assert sum(e['ev'] == 'connect' for e in json.loads(p.read_text())['events']) == 1, 'environment fallback not observed'
PYENV
```

If that assertion fails, preserve the valid observed routing difference and root must resolve it before claiming this restore proof. Otherwise temporarily delete exactly `PROXY_CHILD_ENV+=("http_proxy=http://${VAREST_LWS_PROXY_ENV}")`, then run:

```bash
python3 -I Tools/run_lws_proxy_probe.py --out /w/Unreal/VaRestHost/Saved/B1Proxy/task4-env-mutation --only env-ws
python3 -I - <<'PYENV'
import json
from pathlib import Path
p = Path('/w/Unreal/VaRestHost/Saved/B1Proxy/task4-env-mutation/env-ws/proxy-b.json')
assert sum(e['ev'] == 'connect' for e in json.loads(p.read_text())['events']) == 1, 'controlled child proxy was not applied'
PYENV
```

Require the second assertion to fail, restore the exact shell line, run `bash -n Tools/run_tests.sh`, then rerun into fresh `task4-env-restored` and repeat the assertion against that directory. This tests the child environment seam without depending on Task 5's future generator.

- [ ] **Step 6: Commit**:

```bash
git add Tools/run_tests.sh Tools/run_lws_proxy_probe.py Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeProxyTest.cpp
git commit -m "test: collect isolated proxy matrix and direct control journals" -m "Co-Authored-By: Codex <noreply@openai.com>"
```

### Task 5: Validate the full evidence set and generate the B2 passport

**Files:** Create `Tools/lws_proxy_passport.py`; generate `docs/passport/2026-10-23-lws30-proxy-probe.md`. Read existing `docs/passport/2026-10-22-lws30-probe.md`, final production `VaRestLwsConn.cpp`, `VaRestLwsConnTls.cpp`, `VaRestLwsErrors.cpp`; do not edit them.

**Interfaces:** `python3 -I Tools/lws_proxy_passport.py --run <matrix-directory> --out <passport.md>` returns 0 only for a full, integrity-checked, usable matrix. `--selftest` uses synthetic in-memory rows clearly separated from measurements. The passport holds exact observed counts and an uncollapsed ordered callback list for every attempt; case directories retain complete raw bytes and journals. Callback comparisons use public enum numeric values read from the pinned header: error 1, established 3, APPEND 24, WSI_CREATE 29, WSI_DESTROY 30, certificate 58.

- [ ] **Step 1: Add validator tests first**. Create the imports, `PassportTests` and selftest/main dispatch from Step 3 without `summarize`, `check_cases` or `render`. Run `python3 -I Tools/lws_proxy_passport.py --selftest`; expected failures name the missing functions. No real measurement is asserted by these fabricated test rows.

- [ ] **Step 2: Pin the B2 input table before collecting data**. The generated document must answer all these questions from the rows, with root's policy conclusion kept separate:

| B2 input | B1 evidence required |
|---|---|
| Can direct and explicit-proxy ws/wss open? | open/echo, CONNECT target/count, target Upgrade, separate Authorization journals |
| Does proxying add APPEND or WSI_CREATE before initial upgrade? | ordered sequence + stable WSI token; compare direct vs explicit success |
| When does certificate verification occur? | every depth, preverify, error and sequence position, including trusted wss and cross-scheme redirect |
| What does refusal really pass? | `input_present`, raw len, capped hex, error callback count and full end/destruction ordering |
| Is denial distinguishable from proxy EOF/silence? | ws/wss denial, EOF, silence, elapsed time and callback metadata; fixture mode is ground truth, never inferred by client timing |
| Is a proxied empty error unique to tunnel refusal? | successful CONNECT followed by untrusted TLS and upgrade HTTP403 controls |
| Which endpoint wins? | null option + environment, explicit A + environment B, separate no_proxy=* observation; engine-config-absence flag |
| Is a public discriminator sufficient? | only public callbacks/API observations; if rows coincide, report “not distinguished by recorded public signals” and require a root policy ruling |
| Does B1 prove production behavior? | no: raw rig deliberately follows redirects; B2 must test final no-follow transport, generic refusal detail and stronger pending failures |

- [ ] **Step 3: Implement the validator/generator**:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
import argparse
import copy
import hashlib
import importlib.util
import json
import pathlib
import sys
import unittest

HERE = pathlib.Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('b1_controller', HERE / 'run_lws_proxy_probe.py')
controller = importlib.util.module_from_spec(spec)
spec.loader.exec_module(controller)
EXPECTED = tuple(row[0] for row in controller.CASES)


def check_cases(ids):
    return [] if len(ids) == len(set(ids)) and set(ids) == set(EXPECTED) else ['incomplete/duplicate matrix']


def events_of(journal, kind):
    return [event for event in journal.get('events', []) if event.get('ev') == kind]


def summarize(case, trace, a, b, source, target):
    harness_errors = list(case.get('harness_errors', []))
    harness_errors.extend(controller.check_trace(trace, case['case']))
    if not case.get('group_drained'):
        harness_errors.append('owned editor process group not drained')
    errors = []  # control failures are distinct from collection/cleanup failures
    mode = case['mode']
    a_connect = events_of(a, 'connect')
    b_connect = events_of(b, 'connect')
    connects = a_connect + b_connect
    requests = events_of(source, 'request')
    targets = events_of(target, 'request')
    for journal in (a, b, source, target):
        if journal.get('dropped', 0):
            errors.append('journal loss')
    for connect in connects:
        if not mode.startswith('redirect') and connect['authority'] not in (
                '127.0.0.1:%d' % case['target_port'], 'localhost:%d' % case['target_port']):
            errors.append('CONNECT authority differs from requested target')
        if any(name.lower() == 'authorization' for name, _ in connect['headers']):
            errors.append('target Authorization present in CONNECT')
    positive = mode in ('direct', 'explicit', 'env', 'precedence', 'env-noproxy')
    if positive:
        if not trace.get('opened') or not trace.get('echoed') or len(requests) != 1:
            errors.append('positive open/echo/Upgrade control failed')
        if requests and ['Authorization', 'Bearer b1-probe-marker'] not in requests[0]['headers']:
            # Header names are case-insensitive, values are not.
            if not any(n.lower() == 'authorization' and v == 'Bearer b1-probe-marker'
                       for n, v in requests[0]['headers']):
                errors.append('positive synthetic-header control failed')
    if mode == 'direct' and connects:
        errors.append('direct control used proxy')
    if mode == 'explicit' and (len(a_connect) != 1 or b_connect):
        errors.append('explicit control did not use one A tunnel')
    if mode in ('deny', 'close', 'silent'):
        if len(a_connect) != 1 or b_connect or requests or targets or trace.get('opened'):
            errors.append('negative proxy control reached wrong stage')
    if mode.startswith('redirect'):
        if not any(e.get('status') == 302 for e in events_of(source, 'response')):
            errors.append('source redirect response unobserved')
    if mode in ('untrusted', 'upgrade403'):
        if len(a_connect) != 1 or not events_of(a, 'tunnel') or trace.get('opened'):
            errors.append('post-tunnel negative control invalid')
        if mode == 'upgrade403' and not any(e.get('status') == 403 for e in events_of(source, 'response')):
            errors.append('target upgrade refusal unobserved')
        if mode == 'untrusted' and requests:
            errors.append('untrusted target received upgrade')
    event_rows = trace['events']
    callback_errors = [dict(seq=e['seq'], conn=e['conn'], wsi=e['wsi'],
                            input_present=e['input_present'], length=e['len'], hex=e['hex'])
                       for e in event_rows if e['reason'] == 1]
    certs = [dict(seq=e['seq'], conn=e['conn'], wsi=e['wsi'], depth=e['depth'],
                  preverify=e['preverify'], x509_error=e['x509_error'])
             for e in event_rows if e['reason'] == 58]
    counts = {name: sum(e['reason'] == reason and e['conn'] == trace['connection'] for e in event_rows)
              for name, reason in (('append', 24), ('create', 29), ('destroy', 30), ('cert', 58))}
    routing = 'A+B' if a_connect and b_connect else 'A' if a_connect else 'B' if b_connect else 'direct'
    ordered = [dict(seq=e['seq'], name=e['name'], reason=e['reason'], conn=e['conn'], wsi=e['wsi'])
               for e in event_rows]
    return dict(case=case['case'], opened=trace['opened'], echoed=trace['echoed'],
                route=routing, connect_a=len(a_connect), connect_b=len(b_connect),
                counts=counts, certificates=certs, errors=callback_errors, ordered=ordered,
                primary_seconds=trace['primary_seconds'], destroyed=trace['destroyed'],
                destroyed_before_shutdown=trace['destroyed_before_shutdown'],
                source_requests=len(requests), target_requests=len(targets),
                redirect_hostname_skip=trace.get('redirect_hostname_skip', False),
                source_authorization=[[v for n, v in e['headers'] if n.lower() == 'authorization'] for e in requests],
                target_authorization=[[v for n, v in e['headers'] if n.lower() == 'authorization'] for e in targets],
                harness_errors=harness_errors, control_errors=errors)


def render(manifest, rows, run):
    lines = ['# libwebsockets 3.0 — B1 proxy passport', '',
             '<!-- Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved. -->', '',
             'Generated from a complete raw-rig matrix; values below are observed, not production policy.', '',
             'Source commit: `' + manifest['commit'] + '`. Scope: ' + manifest['scope'] + '.',
             'Evidence directory: `' + str(run) + '`; `sha256.json` hashes every original artifact.',
             'Working-tree state at run start: `' + ('dirty (see manifest)' if manifest['dirty'] else 'clean') + '`.', '',
             '## Results', '',
             '| Case | Open/echo | Route | APPEND/create/destroy/cert | Target requests | Seconds |',
             '|---|---|---|---|---|---|']
    for row in rows:
        counts = row['counts']
        lines.append('| %s | %s/%s | %s | %s/%s/%s/%s | %s | %.6f |' % (
            row['case'], row['opened'], row['echoed'], row['route'], counts['append'], counts['create'],
            counts['destroy'], counts['cert'], row['target_requests'], row['primary_seconds']))
    lines += ['', '## Exact callback and journal observations', '',
              'Each ordered list includes context/unresolved callbacks (conn=0), pseudo-events and repeated callbacks.',
              'Certificate callbacks are per verification operation/depth. A count is not a connection count.',
              'Error hex is bounded to 256 bytes; length is raw callback len, and pointer presence is independent.',
              'Redirect rows explicitly record hostname-check skipping for numeric loopback targets; chain checks remain.',
              'Synthetic Authorization is appended on every raw APPEND; target presence is not proof of automatic replay.', '']
    for row in rows:
        lines += ['### ' + row['case'], '', '```json', json.dumps(row, indent=2, sort_keys=True), '```', '']
    lines += ['## B2 decision inputs', '',
              'Compare direct/explicit ws and wss ordered APPEND, WSI_CREATE and certificate positions before changing any veto.',
              'Compare denial, abrupt close and silence error pointer/length/bytes and terminal/destruction order.',
              'Compare post-tunnel untrusted TLS and target HTTP403 before choosing any contextual empty-error grouping.',
              'Environment rows identify which listener saw CONNECT; no_proxy=* rows are observations, not a promised bypass API.',
              'A denial fixture status is ground truth on the proxy side, not a target HTTP status and not evidence of callback text.',
              'If these public signals coincide, they do not distinguish the cases. A broader proxied pre-upgrade empty-error',
              'ConnectFailed grouping requires an explicit root B2 ruling and preservation of TLS/redirect/cancel/deadline failures.',
              'No production classifier or redirect-veto change was made by B1. Raw redirects were allowed for measurement.',
              'B2 must prove successful production ws/wss proxying and no redirected target request or credential forwarding.', '',
              '## Remaining release gates', '',
              'Native Windows, macOS, Android/iOS and cooked/shared-copy runtime measurements were not run by this Linux editor matrix.',
              'VaRest-rti residual and NoPCH linking remain separate gates; B1 does not close them or the WebSocket parent epic.', '']
    return '\n'.join(lines)


def generate(run, out):
    manifest = json.loads((run / 'manifest.json').read_text())
    problems = check_cases(manifest['cases'])
    if [r['case'] for r in manifest.get('results', [])] != manifest['cases']:
        problems.append('manifest result set incomplete or reordered')
    hashes = json.loads((run / 'sha256.json').read_text())
    required = {'manifest.json', 'server.log'}
    for case_id in manifest['cases']:
        required.update(case_id + '/' + name for name in ('case.json', 'trace.json', 'editor.log', 'runner.txt',
                                                         'proxy-a.json', 'proxy-b.json', 'source.json', 'target.json'))
    if not required.issubset(hashes):
        problems.append('artifact manifest missing required files')
    for name, expected_hash in hashes.items():
        path = (run / name).resolve()
        if not path.is_relative_to(run.resolve()) or not path.is_file():
            problems.append('missing/outside artifact: ' + name)
        elif hashlib.sha256(path.read_bytes()).hexdigest() != expected_hash:
            problems.append('artifact hash mismatch: ' + name)
    rows = []
    for case_id in manifest['cases']:
        directory = run / case_id
        values = [json.loads((directory / (name + '.json')).read_text())
                  for name in ('case', 'trace', 'proxy-a', 'proxy-b', 'source', 'target')]
        row = summarize(*values)
        rows.append(row)
        problems.extend(case_id + ': harness: ' + error for error in row['harness_errors'])
        problems.extend(case_id + ': control: ' + error for error in row['control_errors'])
    if problems:
        print('\n'.join(problems), file=sys.stderr)
        return 1  # do not replace a prior passport with incomplete evidence
    out.write_text(render(manifest, rows, run), encoding='utf-8')
    return 0


class PassportTests(unittest.TestCase):
    def fixture(self):
        event = dict(seq=1, conn=1, wsi=1, reason=1, name='CONNECTION_ERROR',
                     input_present=False, len=0, hex='', depth=-1, preverify=-1, x509_error=-1)
        trace = dict(schema=1, case='deny-ws', connection=1, events=[event], dropped_events=0,
                     primary_completed=True, shutdown_completed=True, opened=False, echoed=False,
                     primary_seconds=.1, destroyed=True, destroyed_before_shutdown=True)
        case = dict(case='deny-ws', mode='deny', target_port=1234, group_drained=True, harness_errors=[])
        a = dict(events=[dict(ev='connect', headers=[], authority='127.0.0.1:1234')], dropped=0)
        empty = dict(events=[], dropped=0)
        return case, trace, a, copy.deepcopy(empty), copy.deepcopy(empty), copy.deepcopy(empty)

    def test_empty_error_is_measurement_not_403_text(self):
        row = summarize(*self.fixture())
        self.assertEqual(row['harness_errors'], [])
        self.assertEqual(row['errors'][0]['input_present'], False)
        self.assertEqual(row['errors'][0]['length'], 0)
        self.assertNotIn('403', json.dumps(row['errors']))
        manifest = dict(commit='synthetic-test', scope='synthetic test only', dirty='')
        text = render(manifest, [row], pathlib.Path('/synthetic'))
        self.assertIn('not production policy', text)
        self.assertIn('Remaining release gates', text)

    def test_missing_or_duplicate_rows_fail(self):
        self.assertEqual(check_cases(list(EXPECTED)), [])
        self.assertTrue(check_cases(list(EXPECTED[:-1])))
        self.assertTrue(check_cases(list(EXPECTED) + [EXPECTED[0]]))

    def test_header_leak_and_false_negative_are_rejected(self):
        data = self.fixture()
        data[2]['events'][0]['headers'] = [['authorization', 'Bearer b1-probe-marker']]
        self.assertIn('target Authorization present in CONNECT', summarize(*data)['control_errors'])
        data[1]['opened'] = True
        self.assertIn('negative proxy control reached wrong stage', summarize(*data)['control_errors'])

    def test_positive_control_and_route_measurement(self):
        data = self.fixture()
        data[0].update(case='explicit-ws', mode='explicit')
        data[1]['case'] = 'explicit-ws'
        self.assertIn('positive open/echo/Upgrade control failed', summarize(*data)['control_errors'])
        data[0].update(case='env-ws', mode='env')
        data[1].update(case='env-ws', opened=True, echoed=True)
        data[4]['events'] = [dict(ev='request', headers=[['Authorization', 'Bearer b1-probe-marker']])]
        before = summarize(*data)
        data[2]['events'].clear()
        after = summarize(*data)
        self.assertEqual(before['route'], 'A')
        self.assertEqual(after['route'], 'direct')
        self.assertEqual(after['harness_errors'], [])
        self.assertEqual(after['control_errors'], [])  # valid routing difference, not lost evidence


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--run', type=pathlib.Path)
    parser.add_argument('--out', type=pathlib.Path)
    parser.add_argument('--selftest', action='store_true')
    args = parser.parse_args()
    if args.selftest:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(PassportTests)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if args.run is None or args.out is None:
        parser.error('--run and --out are required')
    try:
        return generate(args.run.resolve(), args.out)
    except (OSError, ValueError, KeyError, TypeError) as error:
        print('invalid evidence: ' + str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
```

- [ ] **Step 4: Run GREEN selftests and their restore mutation**:

```bash
python3 -I Tools/lws_proxy_passport.py --selftest
```

Temporarily change `check_cases` to `return []`; require `test_missing_or_duplicate_rows_fail` to fail. Restore and rerun. Temporarily remove the authorization-leak append in `summarize`; require `test_header_leak_and_false_negative_are_rejected` to fail, restore, rerun. No fabricated row may be copied into the real passport.

- [ ] **Step 5: Collect the full matrix with validated binaries** after a guarded normal build and Task 4 review:

```bash
python3 -I Tools/testserver/proxy_selftest.py -v
python3 -I Tools/run_lws_proxy_probe.py --selftest
python3 -I Tools/lws_proxy_passport.py --selftest
python3 -I Tools/run_lws_proxy_probe.py --out /w/Unreal/VaRestHost/Saved/B1Proxy/full-1
python3 -I Tools/lws_proxy_passport.py --run /w/Unreal/VaRestHost/Saved/B1Proxy/full-1 --out docs/passport/2026-10-23-lws30-proxy-probe.md
```

Expected: fixture/controller/validator tests pass; 22 cases produce complete artifacts and zero harness errors; generator exits 0 only after all positive controls and negative-stage controls pass. No expected APPEND/cert count, proxy-error string, environmental route or redirect target count is prescribed. If a valid raw outcome differs from anticipated behavior, record it; if a control fails, retain its artifacts and do not generate a “complete” passport.

Run existing regression coverage without modifying production:

```bash
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Rig
Tools/run_tests.sh --no-build Probe.VaRest.Lws.Handshake.RedirectReplay
Tools/run_tests.sh --no-build VaRest.Integration.WebSocket.Transport.Redirect
Tools/run_tests.sh --no-build VaRest.WebSocket
python3 -I Tools/testserver/ws_selftest.py
python3 -I Tools/lws_probe_passport.py --selftest
```

The ordinary runner must still scrub ambient proxies. Full VaRest/M4, cooked runtime and platform gates remain their existing milestone owners, not additional B1 gates.

- [ ] **Step 6: Validate artifact refusal and preservation**. Copy the evidence directory to a fresh mutation directory, remove one error trace or set one `dropped_events` to 1 without changing its hash, and run the generator to a temporary passport. It must exit 1 and leave the existing real passport unchanged. The exact commands below mutate only the copied evidence:

```bash
cp -rf /w/Unreal/VaRestHost/Saved/B1Proxy/full-1 /w/Unreal/VaRestHost/Saved/B1Proxy/full-mutation
python3 -I - <<'PY'
import json
from pathlib import Path
p = Path('/w/Unreal/VaRestHost/Saved/B1Proxy/full-mutation/deny-ws/trace.json')
v = json.loads(p.read_text())
v['dropped_events'] = 1
p.write_text(json.dumps(v))
PY
python3 -I Tools/lws_proxy_passport.py --run /w/Unreal/VaRestHost/Saved/B1Proxy/full-mutation --out /w/Unreal/VaRestHost/Saved/B1Proxy/refused-passport.md
```

Expected exit 1 names hash mismatch and dropped trace events. Verify no `refused-passport.md` was created. Re-run the generator on untouched `full-1` to show the original evidence still passes. Do not update the original hashes to conceal the mutation.

- [ ] **Step 7: Write the B2 decision handoff and commit**. Append to the generated passport a short manually reviewed “Controller handoff” that cites the actual case IDs and sequence positions for: direct-vs-proxy APPEND/WSI_CREATE/cert deltas; denial/close/silence pointer/len behavior; post-tunnel negative controls; environment precedence; redirect source and target journals. For each proposed discriminator name the supported public observation, its counterexamples and limits. If no supported signal distinguishes tunnel denial from another empty failure, state that explicitly and request root's policy ruling before B2 code. Do not classify by duration, read internal offsets or infer HTTP403 from null/0.

This is the only content in the plan intentionally dependent on new measurements. The code above deterministically produces every source datum; the handoff is interpretation of those data, not an unspecified implementation step. Root independently reviews this interpretation and then commissions the complete B2–B9 plan. Preserve production TLS/redirect/cancel/deadline precedence, Q7 engine-following semantics, and no-follow credential isolation as acceptance obligations.

```bash
git add Tools/lws_proxy_passport.py docs/passport/2026-10-23-lws30-proxy-probe.md
git commit -m "docs: record measured LWS proxy prerequisite evidence" -m "Co-Authored-By: Codex <noreply@openai.com>"
```

## Self-review and completion boundary

- Spec/preflight coverage: fixture bounds and shutdown (Task 1); proxy option, ordered callbacks, stable WSI token, raw error input and per-depth certificates (Task 2); final destruction, trusted direct controls and local-CA-only child (Task 3); denial/EOF/silence/redirect/env precedence, direct parent journals and per-child logs (Task 4); honest complete passport and exact B2 inputs (Task 5).
- Review Focus conditions are pinned to named tests/tasks above. No guessed callback count or production classification is an assertion.
- Existing public APIs are unchanged. Every new callable used in a later task is fully defined in an earlier task or in that task's code. Changes use final `9884b5b` anchors, not the old W1a plan's pseudocode.
- The future full W1b plan must carry the preflight's private queue high4096/low2048, finite PopEvents, W3 single admitted batch and independent reply-pressure rulings. They are context, with no implementation here.
- A successful B1 handoff is reviewed fixture/rig/controller code, passing relevant gates, complete archived artifacts, and a truthful passport with root's B2 inputs. It is not W1b/W1 completion, production proxy support, a native-platform result or a cooked shared-copy runtime result. VaRest-81s stays open; root controls all issue changes.
- Execution method is already delegated by the user. Root and an independent plan reviewer review this document before execution; there is no new human approval menu. Build audit rulings must be incorporated before any compiler invocation.
