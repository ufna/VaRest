# VaRest 2 — WebSocket W0: test server, TLS listener and the libwebsockets 3.0 probe — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `Tools/testserver/ws.py` (a hand-written RFC 6455 server inside the HTTP test server), its TLS listener and selftests, and a probe that drives the engine-bundled libwebsockets 3.0 directly against it. The probe records every WebSocket spec §16.4 item as a fact in `docs/passport/<date>-lws30-probe.md`, and the plan ends by correcting spec §0.3/§2 where the probe contradicts them (and, per ruling Q3, §1.2, D30 and §3.4's 1002 row where its facts are unambiguous).

**Architecture:** `ws.py` plugs into `server.py`'s upgrade hook. One process serves the plain port and a TLS port, shares the login token state, and journals what it saw for each `?key=` so that `GET /ws/journal` can return it. The probe tests (`Probe.VaRest.Lws.*`, in the UncookedOnly `VaRestTests` module) each own one `lws_context` built the way spec §2.1 plans the transport: one protocol `varest`, a service thread blocked in `lws_service` and woken by `lws_cancel_service`, and game-thread commands run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED`. The rig records every callback in order. Each test reads the server's side from the journal over HTTP and logs one JSON `FACT` line per item. A stdlib script turns the run logs into the passport.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), C++20, libwebsockets 3.0.0 and OpenSSL 1.1.1t as the engine bundles them (`Engine/Source/ThirdParty/libWebSockets`, `ThirdParty/OpenSSL/1.1.1t`), the engine `SSL` and `WebSockets` modules, UE Automation tests, Python 3.9+ standard library, and the `openssl` command line once, to make the test certificates.

**Revision 1 (2026-10-21):** applies the controller's rulings and the pre-execution review (`.superpowers/sdd/ws-w0-plan-rulings.md`, `ws-w0-plan-review.md`); the per-finding map is `.superpowers/sdd/ws-w0-plan-revision-report.md`.

**Spec:** `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` — §0.3 (what LWS 3.0 gives; the "inferred from 4.3" list), §1.2 (two LWS copies), §2 (the transport), §16.3 (test server), §16.4 (the W0 probe checklist), §17 row W0 ("`ws.py` + TLS listener + selftest; LWS 3.0 probe harness; passport"; exit "probe recorded; §0.3/§2 corrected where needed"), §18 (risks), §20 D20/D24/D30, §22.2 A2 and the "W0 probe additions" under it. Program rulings: own client on the engine-bundled libwebsockets 3.0, not `IWebSocket` (R1, 2026-10-20); `.superpowers/sdd/release-2.0-program.md:140` (W0 runs before M4 on the one checkout).

Engine paths below are relative to `/w/Unreal/UnrealEngine/Engine/Source/` unless they start with `Programs/` (also under `Engine/Source/`) or `Config/` (under `Engine/`). `libwebsockets.h` and `lws_config.h` mean `ThirdParty/libWebSockets/libwebsockets/include/Unix/x86_64-unknown-linux-gnu/<file>`; `LwsWebSocketsManager.cpp`, `LwsWebSocket.cpp` and `WebSocketsModule.cpp` are under `Runtime/Online/WebSockets/Private/` (the first two in `Lws/`); `SslManager.cpp` and `SslCertificateManager.cpp` under `Runtime/Online/SSL/Private/`.

## Design decisions this plan makes (the spec is silent)

1. **The probe lives in `VaRestTests`, not in a new module.** That module is UncookedOnly, so UBT never builds it for a game target: `ModuleDescriptor.cs:792-793` (`Programs/UnrealBuildTool/Configuration/Descriptors/`) builds `UncookedOnly` only when `!bBuildRequiresCookedData`, and a Game target sets that flag (`Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:1190-1193`). It also already has the integration harness. Cost if wrong: W1 moves four files.
2. **The probe tests are named `Probe.VaRest.Lws.*`, and the one that may crash the process `Probe.VaRest.Risky.Lws.*`.** `Tools/run_tests.sh` anchors a bare filter at the start (`^VaRest`), so neither group runs in the default suite. They take minutes, several facts depend on the host (DNS, routes), and one call may end the process. Cost: someone has to run them on purpose after an engine upgrade. The passport says how.
3. **A probe test passes when it recorded, not when the library agrees with the spec.** Each item is logged as `LogVaRestLwsProbe: Display: FACT {json}` with `id`, `verdict` (`agrees`, `differs`, `recorded`, `unverified`), `observed`, `spec` and `evidence`. Library outcomes (a connection that opened or not, ended or not, echoed or not) are recorded with `NoteOutcome` (an `AddInfo`) and the fact, never asserted (review M-2 ruling). `TestTrue` is kept for harness invariants only: the server was found and answered (its journal arrived), the rig's context and service thread exist, the rig's own self-tests (`Probe.VaRest.Lws.Rig.*`), and the precondition connection to `/ws/echo` that every other fact of a test depends on. W0's job is to record, and a contradiction is a finding. `Tools/lws_probe_passport.py` checks that the run logged every id in its `EXPECTED` list. Cost: a library change shows up as a changed passport, not as a red test.
4. **Test certificates are a throwaway CA and a leaf, made once by `Tools/testserver/certs/make_certs.sh` and committed, with names that explain a scanner hit (ruling Q5): `test-ca.pem`, `test-server.pem` (leaf + CA), `test-server.key`, plus `Tools/testserver/certs/README.md`.** The CA's private key is deleted as the script exits, so nothing else can ever be signed by it. Why: Python's standard library cannot make X.509 certificates. Committed files mean no host needs `openssl` at test time (spec D20 already says "a committed test cert"). Sending the CA in the chain makes `LCCSCF_ALLOW_SELFSIGNED` see `X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN`, which LWS 3.0 can accept ("accepting self-signed certificate (verify_callback)" is a string in `lib/Unix/x86_64-unknown-linux-gnu/Release/libwebsockets.a`), while trusting `test-ca.pem` exercises real verification. SAN: `DNS:localhost, IP:127.0.0.1, IP:::1`. Validity: 36500 days. Cost: a secret scanner may flag `test-server.key` (it signs nothing and protects nothing; the README says so), and the files must be regenerated before 2126. The release package excluding `Tools/` is the controller's note on bead 8e5, not a step of this plan.
5. **The TLS listener is a second port of the same server process.** It serves the same routes and plug-ins and shares `STATE`. Its URL is the second line of the ready file and reaches the editor as `-VaRestTestServerTls=<url>`, read by `VaRest::Http::Testing::GetTestServerTlsUrl()`, which sits next to `GetTestServerUrl()` so W1 can reuse it. Cost: one more runner argument.
6. **The server-side evidence comes from `GET /ws/journal`, fetched with `FVaRestClient`.** The probe never reads its evidence through the library under test. Cost: none.
7. **`ws.py` in W0 covers every transport scenario that §16.2 and §16.4 need and leaves out `/ws/route` (W4) and `/ws/apping` (W3).** Those two encode codec and heartbeat contracts that W0 does not design. `/ws/auth` is included, because it is ten lines over the shared tokens. Cost: one small server task each in the W3 and W4 plans.
8. **Monolithic coexistence is not run in W0.** No VaRest runtime module links LWS before W1, and `VaRestTests` is never in a game target (decision 1). It is recorded as an `unverified` fact. W1's exit row already reads "editor coexistence; monolithic build". Cost if wrong: none.
9. **Two service-wait shapes, chosen per fact (review C-1).** LWS checks its own deadlines (`lws_set_timeout`, the handshake and close-ack timeouts, a `LWS_TO_KILL_ASYNC`) only when `lws_service` returns; `poll()` gets the caller's wait, capped only by the hrtimer list (`lws-plat-unix.c.o` `_lws_plat_service_tsi`: `__lws_hrtimer_service` at 0x314, then `poll` at 0x378; the timeout checks run in `lws_service_fd_tsi` after it, with second granularity; opened with `objdump -dr` on the archive member, as review C-1 did). So facts about **LWS's own timers** (`timeout.default_secs`, `timeout.configured_secs`, both blackholes, `close.ours_noecho_wait_ms`, `ping.idle_survives_timeout_secs`) use a 100 ms wait (`FRigOptions::ServiceTimeoutMs = 100`). Facts about **the service shape** (the command path, the cancel wake, timers, the kill latencies) keep a 10 s wait woken by `lws_cancel_service`, and each kill latency has a 100 ms control row (`svc.kill_async_ms_short_wait`, `timeout.kill_pending_ms_short_wait`), so the passport shows how much of a delay is the wait. Spec §2.1 plans `lws_service(ctx, NextDeadline)`, which is neither, and Task 11's E3 is written from the pair. `Probe.VaRest.Lws.Service.Commands` also runs a control round trip with commands drained between `lws_service` calls, the engine's way (`LwsWebSocketsManager.cpp:480-517`).
10. **Calls that may crash run in their own invocation, after a `PENDING` line, one case per invocation (review C-2).** One such call is `lws_http_client_http_response` inside `CONNECTION_ERROR`, which is documented for `ESTABLISHED_CLIENT_HTTP` only (`libwebsockets.h:3595-3607`) and dereferences NULL when the connection has no header table (`client.c.o` `lws_http_client_http_response` 0x1294-0x12b2: with `[rdi+0x8]` and `[rdi+0x120]` both null it reads `[0+0x3a4]`; opened with `objdump -dr` on the archive member). Six tests, `Probe.VaRest.Risky.Lws.StatusInConnectionError.<Case>` (`Refused`, `ConnectRefused`, `DnsFailure`, `NoUpgrade`, `TlsVerifyFailure`, `KilledPending`), each with its own id, `PENDING` line and `Tools/run_tests.sh` run, because a crash ends the run. A `PENDING` id with no `FACT` becomes `crashed` in the passport, so the crash itself is recorded. Cost: seven runs instead of one.
11. **The coexistence test turns on the engine's `[LwsWebSocket] bDisableCertValidation` for its own duration and restores it (ruling Q4).** The engine reads that key again on any config change (`LwsWebSocketsManager.cpp:91-93`, `WebSocketsModule.cpp:47`, `:68-74`). The flag does not change the engine's TLS flags: `wss` still gets only `LCCSCF_ALLOW_SELFSIGNED` (`LwsWebSocket.cpp:741-743`). It works because the engine's verify callback then sets `X509_V_OK` and accepts (`LwsWebSocket.cpp:554-568`), which also passes the host-name check an IP URL would fail (review M-9). Ruling Q4's conditions hold by shape: the restore is an `ON_SCOPE_EXIT` declared right after the old value is read and before the flag is set, so it runs on every exit path; a key that was absent is removed (`GConfig->RemoveKey`), not written as `False`; and while the flag is on, the test connects only to the local test server (`Site.Host`, the runner's `127.0.0.1`). Cost: the engine's certificate check is off, process-wide, for a few seconds of a test run.
12. **The probe reaches `IWebSocket` through `FWebSocketsModule`'s virtuals only (`PrivateIncludePathModuleNames`, no link), so `VaRestTests` keeps its own static libwebsockets.** That copy situation is the one `VaRestWebSocket` will have, because spec §1.1 says `WebSockets` "is **not** a dependency". The probe uses `FModuleManager::LoadModuleChecked<FWebSocketsModule>`; the static `FWebSocketsModule::Get()` needs the import library, so it is never called. Cost: none.
13. **The monolithic SSL-module context path is stood in for by a context the probe makes itself (`Probe.VaRest.Lws.Tls.ProvidedContext`).** In modular builds `FSslManager::InitializeSsl()` returns false and `CreateSslContext` returns null (`SslManager.cpp:26-55`, `:76-131`), so an editor cannot see the engine's provided-context path any other way. Cost: that context lacks the engine's root certificates. The probe adds the test CA instead.
14. **Spec corrections follow a fixed table, fact by fact (Task 11).** Per ruling Q3, the table also has rows for §1.2 (two copies, their log state, the TLS index), D30 in §20, and §3.4's 1002 row. Each row is applied only when its fact is unambiguous; a fact that is ambiguous or `unverified` becomes a bead instead.
15. **Upgrade redirects are refused (ruling Q2).** VaRest never follows a 3xx to the upgrade, as browsers do not: the WHATWG WebSockets standard fetches with redirect mode "error", and RFC 6455 §4.1 only permits following. W0 records what LWS 3.0 does (whether it follows, whether a callback can see the new target, whether a non-zero return vetoes it, which `Host` the second request carries), so Task 11 can write §2.2's rule from evidence, and the P1 bead carries the same facts either way.
16. **The rig never creates a context while another rig of the same LWS copy has a TLS handshake in flight (review I-3).** Every `lws_create_context` re-allocates the copy's process-global `openssl_websocket_private_data_index` (`ssl.c.o` `lws_context_init_ssl_library` stores it on both the `DO_SSL_GLOBAL_INIT` path and the other one), and `OpenSSL_client_verify_callback` reads it through the GOT and dereferences `SSL_get_ex_data`'s result without a null check (`openssl-client.c.o` 0x1c8-0x1d6: `mov 0x78(%rax),%rcx`; `ssl.c.o` stores at 0x1f4 and 0x23a; opened with `objdump -dr`, as review I-3 did). Tests create their rigs before connecting and keep one TLS rig alive at a time. The hazard itself is recorded as `coexist.context_create_reallocates_ssl_index`.

## Global Constraints

- Engine UE 5.8.1. Build host `/w/Unreal/VaRestHost` (its `Plugins/VaRest` is a symlink to this checkout). Build and test with `Tools/run_tests.sh [--no-build] <Filter>`; the runner starts the test server. **Never** plan or run `-DisableUnity` or `-CompilerArguments`: both rebuild the engine.
- No editor may run during a build (it holds the module `.so` files). Kill only by PID, never `pkill -f`.
- Results come from the run's log file (`/w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log`), never from stdout. The runner deletes that file at the start of each run, so copy it before the next run when a later step needs it.
- Python: standard library only, run as `python3 -I`, Python 3.9+ (`socket.timeout`, not a bare `TimeoutError`, catches socket timeouts on 3.9). Every script that imports a sibling inserts its own directory into `sys.path` and sets `sys.dont_write_bytecode = True` (no `__pycache__` in the tree).
- Test server rules (M3 review M1/M3/M-13): every request-controlled size and count is bounded and checked before work; nothing is read from disk under a request-controlled name (`/tls/ca.pem` is one fixed file); the server exits when the runner dies (`--parent-pid`, unchanged); refusals before the 101 are HTTP 4xx JSON and close the connection.
- The certificates in `Tools/testserver/certs/` (`test-ca.pem`, `test-server.pem`, `test-server.key`, `README.md`) are test-only: a throwaway CA whose key no longer exists, a leaf for `localhost`/`127.0.0.1`/`::1`, and no key of any real service. Never point a non-test client at that CA.
- Modules: `VaRestTests` stays `"Type": "UncookedOnly"`, `"PlatformAllowList": ["Win64","Mac","Linux"]`. Nothing under `Source/VaRest/`, `Source/VaRestEditor/` or `Source/VaRestJson/` changes. `VaRestHttp` gains one testing helper (`GetTestServerTlsUrl`) and nothing else. W0 adds no VaRest WebSocket client code (spec §16.4: "A minimal LWS harness (no VaRest client code)").
- libwebsockets and OpenSSL headers are included only through `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeLws.h`, the engine's way (`Runtime/Online/WebSockets/Private/Lws/LwsWebSocketsManager.h:14-24`). UBT defines `NDEBUG=1` without the debug CRT (`Programs/UnrealBuildTool/Configuration/UEBuildPlatform.cs:1338-1345`), so `lws_config.h:3-7` does not define `_DEBUG`.
- The probe never calls `lws_set_log_level` or `lws_set_allocator`. Both are process-global per library copy (spec T16, D30).
- Library outcomes are facts, not assertions (decision 3): record them with `NoteOutcome` and `Fact`; `TestTrue`/`TestNotNull` only for harness invariants.
- Never create an `FRig` while another `FRig` has a TLS connection that has not opened or ended (decision 16).
- Rig threading: LWS is called only on the rig's service thread. The exceptions are `lws_cancel_service` (any thread, `libwebsockets.h:3704-3716`), `lws_create_context` on the game thread before the thread starts, and `lws_context_destroy` on the game thread after the thread has stopped, the engine's order (`LwsWebSocketsManager.cpp:298-309`). Hooks and commands run on the service thread and talk to the test only through `FRig::Record`. The game thread reads only the rig's event list.
- Lessons from M3. Every test drains what it opens: `WaitEnded` for every connection, or the rig's teardown. Completion is asserted before silence. Nothing is indexed or dereferenced after a non-fatal check (guard in the same expression or return). Callbacks never capture test stack locals by reference (shared state is a `TSharedRef`, rig state lives in `FConn`). Expected-log patterns name one source each: `"Lws("` matches only the engine's routed LWS lines (`LwsWebSocketsManager.cpp:415-422`).
- JSON strings compare case-sensitively (`Equals(..., ESearchCase::CaseSensitive)`; `FString ==` ignores case). HTTP header names compare case-insensitively.
- A probe item that cannot run on this host is a fact with verdict `unverified` and text `"unverified here; release-pass item (<platform>)"`. It is never guessed.
- Engine claims carry `file:line`, and every cited line is opened. Comments explain *why*.
- New files start with `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.` (C++) or `# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.` (Python, shell).
- Unity builds: no anonymous namespaces and no file-scope `using namespace` in `.cpp` files. File-local helpers go in `namespace VaRest::LwsProbe::<FileStem>Private`. A block-scope `using namespace` inside a function is fine.
- MSVC-clean by construction: no local or parameter named like a member (data or function) of the enclosing class (C4458). `FRig`'s members are listed in Task 4's Interfaces. No lambda parameter or lambda-body local reuses a name of the enclosing function (`All`, `Each` are kept for lambda parameters). No arithmetic between two different enumeration types (C++20 deprecation).
- `UE_LOG` format strings are literals.
- Commits: stage only the files the task names (`git add <paths>`), because other agents work in this tree. Messages end with `Co-Authored-By: Claude <Model> <noreply@anthropic.com>` naming the model that wrote the commit (for example `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`). On `index.lock`, wait and retry. Never push.

## Review Focus

The five conditions most likely to bite that no task's main tests exercise; each line's test is added to the owning task.

- **A probe test whose connection never ends** (the server holds it, LWS never times it out): the test fails at its own deadline, and the rig's teardown still returns within seconds and ends the connection. Owner: Task 4, `Probe.VaRest.Lws.Rig.TeardownWithOpenConnection` (restore-the-bug: `Stop()` without `lws_cancel_service`).
- **LWS reusing a freed `wsi` address for the next connection**: events stay with the connection they belong to, because the rig matches the userdata first and a live `wsi` only second. Owner: Task 4, `Probe.VaRest.Lws.Rig.SequentialConnections`.
- **A hostile client** (a 2^63 frame length, a 126-byte control frame, a fragmented control frame, a continuation with no message, a message inside a message, a reserved bit, a reserved opcode, a message over the limit): closed with 1002 or 1009 before anything big is read, and the server goes on serving. Owner: Task 1, `test_protocol_errors_close_1002_and_the_server_goes_on`, `test_lengths_over_the_limit_close_1009`.
- **The runner killed while both listeners are up**: both listeners exit and the ready file goes with them. Owner: Task 3, `LifecycleTest.test_tls_listener_in_the_ready_file_and_gone_with_the_parent`.
- **A TLS client that never finishes its handshake, or that speaks plain HTTP to the TLS port**: other TLS connections are still served, because the handshake runs on the connection's own thread with a timeout. Owner: Task 3, `TlsTest.test_a_stalled_or_plain_client_does_not_block_others`.

---

## File Structure

```
Tools/testserver/
  ws.py                         NEW (Task 1, routes in Task 2): RFC 6455 server, journal, /ws/journal
  ws_selftest.py                NEW (Task 1, Task 2, Task 3): stdlib client checks of ws.py, incl. over TLS
  server.py                     register_route (Task 1); TLS listener, Handler.setup, /tls/ca.pem, --tls-port (Task 3)
  selftest.py                   plug-in route (Task 1); TlsTest, TLS lifecycle, /tls/ca.pem (Task 3)
  certs/make_certs.sh           NEW (Task 3): makes the three files below; run once
  certs/test-ca.pem, test-server.pem, test-server.key   NEW (Task 3): generated, committed, test-only
  certs/README.md               NEW (Task 3): what these are, why the key is harmless, how to regenerate
Tools/run_tests.sh              --tls-port 0, -VaRestTestServerTls=<url> (Task 3)
Tools/lws_probe_passport.py     NEW (Task 10): FACT lines -> passport, EXPECTED ids, --selftest
Source/VaRestHttp/Public/VaRestHttpTesting.h, Private/VaRestHttpTesting.cpp   GetTestServerTlsUrl (Task 4)
Source/VaRestTests/VaRestTests.Build.cs                                     SSL, WebSockets headers, LWS/OpenSSL/zlib (Task 4)
Source/VaRestTests/Private/VaRestTestsModule.cpp                            module class: WebSockets loaded at startup? (Task 4)
Source/VaRestTests/Private/LwsProbe/
  VaRestLwsProbeLws.h           NEW (Task 4): the one place LWS/OpenSSL headers are included
  VaRestLwsProbe.h/.cpp         NEW (Task 4): FRig, FConn, events, facts, journal/HTTP helpers
  VaRestLwsProbeRigTest.cpp     NEW (Task 4): Probe.VaRest.Lws.Rig.*
  VaRestLwsProbeServiceTest.cpp NEW (Task 5): Probe.VaRest.Lws.Service.*, .Timeout.*
  VaRestLwsProbeHandshakeTest.cpp NEW (Task 6): Probe.VaRest.Lws.Handshake.*, Probe.VaRest.Risky.Lws.StatusInConnectionError.*
  VaRestLwsProbeCloseTest.cpp   NEW (Task 7): Probe.VaRest.Lws.Close.*, .Ping.*
  VaRestLwsProbeDataTest.cpp    NEW (Task 8): Probe.VaRest.Lws.Data.*
  VaRestLwsProbeTlsTest.cpp     NEW (Task 9): Probe.VaRest.Lws.Tls.*, .Coexist.*
docs/passport/<run date>-lws30-probe.md                                    NEW (Task 10): generated
docs/superpowers/specs/2026-10-20-varest2-websocket-design.md              §0.3, §2; §1.2, §3.4's table, §20 D30 when their facts are unambiguous (Task 11)
CLAUDE.md                                                                   Build & Test: ws_selftest, the probe runs (Task 11, ruling Q1)
```

Task order: 1 → 2 → 3 (server side) → 4 (rig) → 5–9 (probe groups, any order after 4) → 10 → 11.

---
### Task 1: `ws.py` core — handshake, frames, echo, refusals, the journal

**Files:**
- Create: `Tools/testserver/ws.py`
- Create: `Tools/testserver/ws_selftest.py`
- Modify: `Tools/testserver/server.py` (add `register_route`, docstring "Plug-ins" paragraph)
- Modify: `Tools/testserver/selftest.py` (one test in `ServerTest`)

**Interfaces:**
- Consumes (server.py, as it is at HEAD): `server.register_upgrade(token, handler)`, `server.Refused(status, message, **fields)` (caught by `Handler.dispatch` and answered as JSON, connection closed), `server.query_int(h, name, default, high, low=0)`, `server.path_int(match, name, high)`, `server.decode_text(data)`, `server.LOCK`, `server.STATE`, `server.MAX_MS`, `server.MAX_COUNT`, `server.make_server(host, port)`, `server.main(argv=None)`, `server.PLUGINS = ("ws",)` (already loads a `ws.py` next to it).
- Produces (Task 2, 3 and the probe rely on these):
  - `server.register_route(methods, pattern, route)`: a plug-in's HTTP route, matched before the built-in ones; `route(h, match)`.
  - `ws.accept_value(key) -> str`, `ws.unmask(payload, mask) -> bytes`, `ws.check_handshake(h) -> key`, `ws.accept(h, journal, protocol=None) -> WsConn`.
  - `ws.Journal(key)` with `.add(ev, **fields)` and `.end()`; module globals `JOURNAL_LOCK` (a `threading.Condition`), `JOURNALS`, `MAX_KEYS`, `MAX_EVENTS`, `MAX_MESSAGE`, `INLINE`, `HOLD`, `CLOSE_WAIT`, `WS_STATE`.
  - `ws.WsConn` with `.send_frame(op, payload=b"", fin=True, journal=True)`, `.send_message(kind, data, fragment=0)`, `.send_close(payload)`, `.serve(on_message=None, answer_ping=True, echo_close=True, on_frame=None, strict_utf8=True) -> "close"|"eof"|"timeout"|"error"|"fail"`, `.wait_close(seconds)`, `.wait_eof(seconds)`, `.fail(code, reason)`, `.sock`; `ws.echo(conn, fragment=0, **policy)`.
  - `ws.message_fields(kind, data)`, `ws.close_fields(payload)`, `ws.inline(payload)`, `ws.ProtocolError(code, reason)`.
  - `ws.SCENARIOS`: a list of `(compiled pattern, scenario(h, match, journal))`; Task 2 replaces it.
  - Opcodes `ws.OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG`, `ws.DATA_OPS`.
  - Journal JSON (`GET /ws/journal?key=<k>[&connections=<n>][&wait_ms=<ms>][&reset=1]`): `{"key", "connections", "ended", "dropped", "events": [{"conn", "t", "ev", ...}]}`. Event kinds: `request` (method, target, request_line, headers `[[name, value], ...]` in order and case), `response` (status, protocol), `frame` (op, fin, rsv, masked, len, b64 or sha256), `sent` (op, fin, len, b64 or sha256), `message` (kind, len, sha256, b64 and text when short, valid_utf8=false when invalid), `close` (hex, len, code, reason: the client's close as received), `fail` (code, reason), `eof`, `timeout`, `error` (text), `refused` (status, error), `end`.
  - `ws_selftest.py`: `Client(port, path, headers=(), protocols=(), tls=None, key=None, version="13", timeout=10.0)` with `.status`, `.headers` (lower-case names), `.body`, `.accepted`, `.send(op, payload=b"", fin=True, mask=True)`, `.recv() -> (fin, op, payload)`, `.message() -> (kind, data)` where kind is `"text"`, `"binary"`, `"close"`, `"ping"` or `"pong"`, `.close(code=1000, reason=b"")`, `.shutdown()`; `raw(port, data) -> bytes`; `start(make) -> server`; `WsTestBase` with `connect(path, **kwargs)`, `journal(key, connections=1, wait_ms=5000)`, `ended(key, connections=1)`, `of(journal, ev, conn=None)`.

- [ ] **Step 1: Write the failing server.py test for plug-in routes**

In `Tools/testserver/selftest.py`, add to `class ServerTest`, after `test_upgrade_plugin_contract`:

```python
    def test_plugin_route(self):
        # WebSocket spec 16.3: a plug-in's HTTP route (ws.py serves /ws/journal this way) is matched like a built-in one
        server.register_route(("GET",), r"/plugin/hello", lambda h, match: h.send_json(200, {"hello": h.query.get("who", [""])[0]}))
        status, _, value = self.call_json("GET", "/plugin/hello?who=ws")
        self.assertEqual((status, value), (200, {"hello": "ws"}))
        self.assertEqual(self.call("POST", "/plugin/hello")[0], 404)
```

- [ ] **Step 2: Run it to see it fail**

Run: `python3 -I Tools/testserver/selftest.py ServerTest.test_plugin_route -v`
Expected: ERROR, `AttributeError: module 'server' has no attribute 'register_route'`.

- [ ] **Step 3: Add `register_route` to server.py**

In `Tools/testserver/server.py`, directly below `def register_upgrade(token, handler): ...`, add:

```python
def register_route(methods, pattern, route):
    """WebSocket spec 16.3: an HTTP route of a plug-in (ws.py: /ws/journal), matched before the built-in routes; methods
    is a tuple of verbs or ANY, route(h, match) as the built-in ones"""
    ROUTES.insert(0, (methods, re.compile(pattern), route))
```

Replace the docstring's last paragraph (the one starting `Plug-ins (WebSocket spec 16.3):`) with:

```
Plug-ins (WebSocket spec 16.3): each module named in PLUGINS that sits next to this file is imported at start
and may call register_upgrade(token, handler) and register_route(methods, pattern, route). A request whose Upgrade
header names that token goes to handler(request_handler) instead of a route; the handler answers 101, takes over
request_handler.connection, rfile and wfile, and sets request_handler.close_connection. A registered route is matched
before the built-in ones. STATE and its lock are shared with plug-ins.
```

- [ ] **Step 4: Run it to see it pass**

Run: `python3 -I Tools/testserver/selftest.py -v`
Expected: every test passes, `test_plugin_route` included.

- [ ] **Step 5: Write the failing ws.py selftest**

Create `Tools/testserver/ws_selftest.py`:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Checks Tools/testserver/ws.py with a standard-library RFC 6455 client before UE tests rely on it (WebSocket spec 16.3).

    python3 -I Tools/testserver/ws_selftest.py [-v]
"""

import base64
import hashlib
import http.client
import json
import os
import random
import socket
import ssl
import struct
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
import ws  # noqa: E402


class Client:
    """A stdlib RFC 6455 client, enough to check ws.py: it masks every frame unless told not to, as a client must"""

    def __init__(self, port, path, headers=(), protocols=(), tls=None, key=None, version="13", timeout=10.0):
        self.raw = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock = tls.wrap_socket(self.raw, server_hostname="localhost") if tls else self.raw
        self.key = key if key is not None else base64.b64encode(os.urandom(16)).decode("ascii")
        lines = ["GET %s HTTP/1.1" % path, "Host: 127.0.0.1:%d" % port, "Upgrade: websocket", "Connection: Upgrade",
                 "Sec-WebSocket-Key: " + self.key, "Sec-WebSocket-Version: " + version]
        if protocols:
            lines.append("Sec-WebSocket-Protocol: " + ", ".join(protocols))
        lines += ["%s: %s" % pair for pair in headers]
        self.sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("latin-1"))
        self.rfile = self.sock.makefile("rb")
        status_line = self.rfile.readline(65536)
        # 0: the server ended the connection without a status line
        self.status = int(status_line.split()[1]) if status_line.startswith(b"HTTP/") else 0
        self.headers = {}
        while status_line:
            line = self.rfile.readline(65536)
            if line in (b"\r\n", b"\n", b""):
                break
            name, _, value = line.decode("latin-1").partition(":")
            self.headers[name.strip().lower()] = value.strip()
        self.body = b""
        if self.status not in (0, 101):
            length = int(self.headers.get("content-length", "0"))
            self.body = self.rfile.read(length) if length else b""

    @property
    def accepted(self):
        return self.status == 101 and self.headers.get("sec-websocket-accept") == ws.accept_value(self.key)

    def send(self, op, payload=b"", fin=True, mask=True):
        first = (0x80 if fin else 0) | op
        flag = 0x80 if mask else 0
        length = len(payload)
        if length < 126:
            head = struct.pack("!BB", first, flag | length)
        elif length < 65536:
            head = struct.pack("!BBH", first, flag | 126, length)
        else:
            head = struct.pack("!BBQ", first, flag | 127, length)
        if mask:
            key = os.urandom(4)
            head += key
            payload = ws.unmask(payload, key)  # XOR with the mask is its own inverse
        self.sock.sendall(head + payload)

    def read(self, count):
        data = self.rfile.read(count) if count else b""
        if len(data) < count:
            raise EOFError("the server closed the connection")
        return data

    def recv(self):
        """One frame: (fin, op, payload)"""
        first, second = self.read(2)
        if second & 0x80:
            raise AssertionError("a server frame must not be masked (RFC 6455 5.1)")
        length = second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read(8))[0]
        return first >> 7, first & 0x0F, self.read(length)

    def message(self):
        """The next data message as (kind, bytes), or the next control frame as ("close"|"ping"|"pong", payload)"""
        parts, kind = [], None
        while True:
            fin, op, payload = self.recv()
            if op >= 0x8:
                return {ws.OP_CLOSE: "close", ws.OP_PING: "ping", ws.OP_PONG: "pong"}[op], payload
            if op != ws.OP_CONT:
                kind = "text" if op == ws.OP_TEXT else "binary"
            parts.append(payload)
            if fin:
                return kind, b"".join(parts)

    def close(self, code=1000, reason=b""):
        self.send(ws.OP_CLOSE, struct.pack("!H", code) + reason)

    def shutdown(self):
        try:
            self.sock.close()
        except OSError:
            pass


def raw(port, data):
    """Sends bytes as they are and reads until the server closes"""
    with socket.create_connection(("127.0.0.1", port), timeout=10) as connection:
        connection.sendall(data)
        received = b""
        while True:
            chunk = connection.recv(65536)
            if not chunk:
                return received
            received += chunk


def start(make):
    httpd = make("127.0.0.1", 0)
    threading.Thread(target=httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True).start()
    return httpd


def close_code(payload):
    return struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else None


class WsTestBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = start(server.make_server)
        cls.port = cls.httpd.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def connect(self, path, **kwargs):
        client = Client(self.port, path, **kwargs)
        self.addCleanup(client.shutdown)
        return client

    def journal(self, key, connections=1, wait_ms=5000):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=30)
        try:
            connection.request("GET", "/ws/journal?key=%s&connections=%d&wait_ms=%d" % (key, connections, wait_ms))
            return json.loads(connection.getresponse().read())
        finally:
            connection.close()

    def ended(self, key, connections=1):
        """The journal once that many connections with the key have ended: completion, asserted"""
        journal = self.journal(key, connections)
        self.assertTrue(journal["ended"], journal)
        return journal

    @staticmethod
    def of(journal, ev, conn=None):
        return [event for event in journal["events"] if event["ev"] == ev and (conn is None or event["conn"] == conn)]


class CoreTest(WsTestBase):
    def test_accept_value_matches_the_rfc_example(self):
        # RFC 6455 1.3: this key and this answer
        self.assertEqual(ws.accept_value("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")

    def test_echo(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/echo?key=" + key)
        self.assertTrue(client.accepted, client.headers)
        client.send(ws.OP_TEXT, "привет".encode("utf-8"))
        self.assertEqual(client.message(), ("text", "привет".encode("utf-8")))
        client.send(ws.OP_BINARY, b"\x00\xff")
        self.assertEqual(client.message(), ("binary", b"\x00\xff"))
        # A fragmented message with a PING between its fragments (RFC 6455 5.4 allows control frames there)
        client.send(ws.OP_TEXT, b"ab", fin=False)
        client.send(ws.OP_PING, b"in-between")
        client.send(ws.OP_CONT, b"cd", fin=False)
        client.send(ws.OP_CONT, b"ef")
        self.assertEqual(client.message(), ("pong", b"in-between"))
        self.assertEqual(client.message(), ("text", b"abcdef"))
        client.close(1000, b"bye")
        self.assertEqual(client.message(), ("close", struct.pack("!H", 1000)))
        journal = self.ended(key)
        self.assertEqual([event["kind"] for event in self.of(journal, "message")], ["text", "binary", "text"])
        self.assertEqual(self.of(journal, "message")[0]["text"], "привет")
        self.assertEqual((self.of(journal, "close")[0]["code"], self.of(journal, "close")[0]["reason"]), (1000, "bye"))
        self.assertEqual(self.of(journal, "request")[0]["target"], "/ws/echo?key=" + key)
        self.assertEqual(self.of(journal, "response")[0]["status"], 101)

    def test_echo_in_frames(self):
        client = self.connect("/ws/echo?fragment=4")
        client.send(ws.OP_TEXT, b"0123456789")
        self.assertEqual([client.recv() for _ in range(3)], [(0, ws.OP_TEXT, b"0123"), (0, ws.OP_CONT, b"4567"), (1, ws.OP_CONT, b"89")])

    def test_status_refusals(self):
        refused = self.connect("/ws/status/403")
        self.assertEqual((refused.status, json.loads(refused.body)), (403, {"status": 403}))
        moved = self.connect("/ws/status/302?location=/ws/echo")
        self.assertEqual((moved.status, moved.headers["location"]), (302, "/ws/echo"))
        self.assertEqual(self.connect("/ws/status/401").headers["www-authenticate"], "Bearer")
        self.assertEqual(self.connect("/ws/status/101").status, 400)

    def test_handshake_checked(self):
        self.assertEqual(self.connect("/ws/echo", version="12").status, 426)
        self.assertEqual(self.connect("/ws/echo", key="short").status, 400)
        self.assertEqual(self.connect("/ws/nope").status, 404)
        self.assertEqual(self.connect("/ws/echo?fragment=x").status, 400)
        upgrade = (b"Upgrade: websocket\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n")
        answer = raw(self.port, b"POST /ws/echo HTTP/1.1\r\nHost: x\r\nConnection: Upgrade\r\nContent-Length: 0\r\n" + upgrade + b"\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 400 "), answer[:80])
        answer = raw(self.port, b"GET /ws/echo HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n" + upgrade + b"\r\n")
        self.assertTrue(answer.startswith(b"HTTP/1.1 400 "), answer[:80])

    def test_protocol_errors_close_1002_and_the_server_goes_on(self):
        # Review focus: what a hostile or broken client sends is closed with 1002 (RFC 6455 5.2, 5.4, 5.5, 7.4.1)
        cases = [
            ("unmasked", lambda c: c.send(ws.OP_TEXT, b"x", mask=False)),
            ("126-byte control", lambda c: c.send(ws.OP_PING, b"p" * 126)),
            ("fragmented control", lambda c: c.send(ws.OP_PING, b"p", fin=False)),
            ("continuation first", lambda c: c.send(ws.OP_CONT, b"x")),
            ("message in a message", lambda c: (c.send(ws.OP_TEXT, b"a", fin=False), c.send(ws.OP_TEXT, b"b"))),
            ("reserved bit", lambda c: c.sock.sendall(bytes([0x80 | 0x40 | ws.OP_TEXT, 0x80]) + b"\x00\x00\x00\x00")),
            ("reserved opcode", lambda c: c.send(0x3, b"x")),
        ]
        for name, misbehave in cases:
            client = self.connect("/ws/echo")
            misbehave(client)
            kind, payload = client.message()
            self.assertEqual((kind, close_code(payload)), ("close", 1002), name)
        client = self.connect("/ws/echo")
        client.send(ws.OP_TEXT, b"still here")
        self.assertEqual(client.message(), ("text", b"still here"))

    def test_lengths_over_the_limit_close_1009(self):
        # Review focus: a 2^63 length is refused from the header alone, before any payload is read
        client = self.connect("/ws/echo")
        client.sock.sendall(bytes([0x80 | ws.OP_BINARY, 0x80 | 127]) + struct.pack("!Q", 2 ** 63) + b"\x00\x00\x00\x00")
        kind, payload = client.message()
        self.assertEqual((kind, close_code(payload)), ("close", 1009))
        saved = ws.MAX_MESSAGE
        ws.MAX_MESSAGE = 1000
        try:
            client = self.connect("/ws/echo")
            client.send(ws.OP_TEXT, b"a" * 600, fin=False)
            client.send(ws.OP_CONT, b"b" * 600)
            kind, payload = client.message()
            self.assertEqual((kind, close_code(payload)), ("close", 1009))
        finally:
            ws.MAX_MESSAGE = saved

    def test_invalid_utf8_closes_1007(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/echo?key=" + key)
        client.send(ws.OP_TEXT, b"\xc3\x28")
        kind, payload = client.message()
        self.assertEqual((kind, close_code(payload)), ("close", 1007))
        client.close(1007)
        self.assertFalse(self.of(self.ended(key), "message")[0]["valid_utf8"])

    def test_journal_waits_and_is_bounded(self):
        started = time.monotonic()
        journal = self.journal(uuid.uuid4().hex, wait_ms=300)
        self.assertEqual((journal["ended"], journal["connections"], journal["events"]), (False, 0, []))
        self.assertGreaterEqual(time.monotonic() - started, 0.25)
        # Review focus: the oldest key goes, and events past the cap are counted, not kept
        saved = (ws.MAX_KEYS, ws.MAX_EVENTS)
        ws.MAX_KEYS, ws.MAX_EVENTS = 2, 3
        try:
            with ws.JOURNAL_LOCK:
                ws.JOURNALS.clear()
            first = ws.Journal("first")
            for index in range(5):
                first.add("note", index=index)
            with ws.JOURNAL_LOCK:
                self.assertEqual((len(ws.JOURNALS["first"]["events"]), ws.JOURNALS["first"]["dropped"]), (3, 2))
            ws.Journal("second")
            ws.Journal("third")
            with ws.JOURNAL_LOCK:
                self.assertEqual(list(ws.JOURNALS), ["second", "third"])
            first.add("late")  # its key is gone: nothing kept, nothing raised
            first.end()
        finally:
            ws.MAX_KEYS, ws.MAX_EVENTS = saved

    def test_connections_of_one_key(self):
        key = uuid.uuid4().hex
        for _ in range(2):
            client = self.connect("/ws/echo?key=" + key)
            client.close()
            client.message()
        journal = self.ended(key, connections=2)
        self.assertEqual(journal["connections"], 2)
        self.assertEqual(sorted({event["conn"] for event in journal["events"]}), [1, 2])
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        connection.request("GET", "/ws/journal?key=%s&reset=1" % key)
        connection.getresponse().read()
        connection.close()
        self.assertEqual(self.journal(key, wait_ms=0)["connections"], 0)


class ScriptModeTest(unittest.TestCase):
    """ws.py loaded by server.py, and started by itself: one module either way (WebSocket spec 16.3)"""

    def run_server(self, script):
        with tempfile.TemporaryDirectory() as directory:
            ready = os.path.join(directory, "url")
            child = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, script), "--port", "0", "--ready-file", ready],
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                deadline = time.monotonic() + 10
                while not os.path.exists(ready) and time.monotonic() < deadline and child.poll() is None:
                    time.sleep(0.05)
                with open(ready, encoding="utf-8") as stream:
                    port = urllib.parse.urlsplit(stream.readline().strip()).port
                key = uuid.uuid4().hex
                client = Client(port, "/ws/echo?key=" + key)
                client.send(ws.OP_TEXT, b"script mode")
                self.assertEqual(client.message(), ("text", b"script mode"))
                client.close()
                client.message()
                client.shutdown()
                connection = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
                connection.request("GET", "/ws/journal?key=%s&wait_ms=3000" % key)
                journal = json.loads(connection.getresponse().read())
                connection.close()
                # The upgrade handler and the journal route are one module: the journal knows the connection
                self.assertTrue(journal["ended"], journal)
            finally:
                child.kill()
                child.wait()
        self.assertFalse(os.path.exists(os.path.join(HERE, "__pycache__")))

    def test_loaded_by_server_py(self):
        self.run_server("server.py")

    def test_started_as_ws_py(self):
        self.run_server("ws.py")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 6: Run it to see it fail**

Run: `python3 -I Tools/testserver/ws_selftest.py -v`
Expected: ERROR at import, `ModuleNotFoundError: No module named 'ws'`.

- [ ] **Step 7: Write ws.py**

Create `Tools/testserver/ws.py`:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""VaRest WebSocket test server (WebSocket spec 16.3). Python 3.9+ standard library only.

    python3 -I Tools/testserver/server.py [server.py options]   loads this module next to it (server.PLUGINS)
    python3 -I Tools/testserver/ws.py [server.py options]       the same server, started from here

A request whose Upgrade header is "websocket" comes here instead of an HTTP route (server.register_upgrade). Every
route takes ?key=<k>: what the server saw on that connection -- the request, every frame both ways, messages, closes,
times -- is kept under k and served as JSON by

  GET /ws/journal?key=<k>[&connections=<n>][&wait_ms=<ms>][&reset=1]
      {"key", "connections", "ended", "dropped", "events": [{"conn", "t", "ev", ...}]}

wait_ms waits until n connections with that key (default 1) have ended; "ended" says whether they had. "conn" numbers
the connections of one key from 1; "t" is seconds since that connection's request was read. Bounds: MAX_KEYS keys
(the oldest is forgotten), MAX_EVENTS events per key (the rest are counted in "dropped"), MAX_MESSAGE bytes per frame
and per message (more is closed with 1009), numeric parameters checked before the 101 (400 JSON, as server.py's
routes).

Echo mode, what a route does after its own part unless it says otherwise: data messages echoed with their kind, PING
answered with a PONG of the same payload, the client's close answered with a close carrying its code, invalid UTF-8 in
a text message closed with 1007, an unmasked frame, a reserved bit, a bad control frame or a broken fragment sequence
closed with 1002.

Routes (GET with Upgrade: websocket):
  /ws/echo[?fragment=N]            echo mode; fragment=N echoes in frames of N bytes
  /ws/status/<code>[?location=u]   refuses the upgrade with that status (200-599) and {"status": code}; Location: u
                                   when given (3xx), WWW-Authenticate: Bearer with 401
"""

import base64
import hashlib
import json
import os
import random
import re
import socket
import struct
import sys
import threading
import time
import urllib.parse

if __name__ == "__main__":
    # Run as a script this module is __main__; server.load_plugins() imports "ws" and must get this module, not a second
    # copy with its own journal and a second registration (the mirror of server.load_plugins' own guard)
    sys.modules.setdefault("ws", sys.modules[__name__])

HERE = os.path.dirname(os.path.abspath(__file__))
# python3 -I drops the script directory from sys.path (WebSocket spec 16.3)
if HERE not in sys.path:
    sys.path.insert(0, HERE)
# No __pycache__ in the source tree (it is not ignored)
sys.dont_write_bytecode = True
import server  # noqa: E402

GUID = b"258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
OP_CONT, OP_TEXT, OP_BINARY, OP_CLOSE, OP_PING, OP_PONG = 0x0, 0x1, 0x2, 0x8, 0x9, 0xA
DATA_OPS = (OP_CONT, OP_TEXT, OP_BINARY)

MAX_MESSAGE = 64 * 1024 * 1024
MAX_KEYS = 256
# Ample for every probe (the busiest, Data.AutoReplyBoundary, journals about 70 frames); 256 keys x 2000 bounds memory
MAX_EVENTS = 2000
# Payloads up to this many bytes are journaled whole (base64), longer ones as a SHA-256
INLINE = 256
# The longest a connection waits on its peer (silent, hang, noecho)
HOLD = 60.0
# How long a close the server started waits for the client's answer
CLOSE_WAIT = 5.0

JOURNAL_LOCK = threading.Condition()
JOURNALS = {}
WS_STATE = {"flaky": {}}


class ProtocolError(Exception):
    """After the 101: the connection is closed with this code"""

    def __init__(self, code, reason):
        super().__init__(reason)
        self.code = code


class Journal:
    """What the server saw on one connection, kept under its ?key= (without one, nothing is kept)"""

    def __init__(self, key):
        self.key = key
        self.t0 = time.monotonic()
        self.index = 0
        if key is None:
            return
        with JOURNAL_LOCK:
            entry = JOURNALS.get(key)
            if entry is None:
                if len(JOURNALS) >= MAX_KEYS:
                    JOURNALS.pop(next(iter(JOURNALS)))
                entry = JOURNALS[key] = {"started": 0, "ended": 0, "dropped": 0, "events": []}
            entry["started"] += 1
            self.index = entry["started"]

    def add(self, ev, **fields):
        if self.key is None:
            return
        event = {"conn": self.index, "t": round(time.monotonic() - self.t0, 4), "ev": ev}
        event.update(fields)
        with JOURNAL_LOCK:
            entry = JOURNALS.get(self.key)
            if entry is None:
                return
            if len(entry["events"]) < MAX_EVENTS:
                entry["events"].append(event)
            else:
                entry["dropped"] += 1

    def end(self):
        self.add("end")
        if self.key is None:
            return
        with JOURNAL_LOCK:
            entry = JOURNALS.get(self.key)
            if entry is not None:
                entry["ended"] += 1
            JOURNAL_LOCK.notify_all()


def inline(payload):
    """A payload for the journal: whole (base64) when short, else its SHA-256"""
    if len(payload) <= INLINE:
        return {"len": len(payload), "b64": base64.b64encode(payload).decode("ascii")}
    return {"len": len(payload), "sha256": hashlib.sha256(payload).hexdigest()}


def message_fields(kind, data):
    fields = {"kind": kind, "len": len(data), "sha256": hashlib.sha256(data).hexdigest()}
    if len(data) <= INLINE:
        fields["b64"] = base64.b64encode(data).decode("ascii")
        text = server.decode_text(data) if kind == "text" else None
        if text is not None:
            fields["text"] = text
    return fields


def close_fields(payload):
    fields = {"hex": payload.hex(), "len": len(payload)}
    if len(payload) >= 2:
        fields["code"] = struct.unpack("!H", payload[:2])[0]
        fields["reason"] = payload[2:].decode("utf-8", "replace")
    return fields


def unmask(payload, mask):
    """RFC 6455 5.3 as one big-integer XOR (WebSocket spec 16.3) instead of a loop over every byte"""
    count = len(payload)
    if not count:
        return payload
    key = (mask * (count // 4 + 1))[:count]
    return (int.from_bytes(payload, "big") ^ int.from_bytes(key, "big")).to_bytes(count, "big")


def accept_value(key):
    """RFC 6455 4.2.2: base64(SHA-1(key + GUID))"""
    return base64.b64encode(hashlib.sha1(key.encode("ascii") + GUID).digest()).decode("ascii")


def check_handshake(h):
    """RFC 6455 4.2.1: what a server must see before it answers 101; a refusal is a server.Refused (400 or 426)"""
    if h.command != "GET":
        raise server.Refused(400, "a WebSocket upgrade is a GET", method=h.command)
    if "upgrade" not in [token.strip().lower() for token in h.headers.get("Connection", "").split(",")]:
        raise server.Refused(400, "Connection does not list upgrade")
    if h.headers.get("Sec-WebSocket-Version", "").strip() != "13":
        raise server.Refused(426, "unsupported Sec-WebSocket-Version", version=h.headers.get("Sec-WebSocket-Version"))
    key = h.headers.get("Sec-WebSocket-Key", "").strip()
    try:
        if len(base64.b64decode(key, validate=True)) != 16:
            raise ValueError(key)
    except ValueError:
        raise server.Refused(400, "Sec-WebSocket-Key is not 16 bytes of base64")
    return key


def accept(h, journal, protocol=None):
    """The 101, then the connection is this module's"""
    key = check_handshake(h)
    lines = [b"HTTP/1.1 101 Switching Protocols", b"Upgrade: websocket", b"Connection: Upgrade",
             b"Sec-WebSocket-Accept: " + accept_value(key).encode("ascii")]
    if protocol:
        lines.append(b"Sec-WebSocket-Protocol: " + protocol.encode("latin-1"))
    h.wfile.write(b"\r\n".join(lines) + b"\r\n\r\n")
    journal.add("response", status=101, protocol=protocol)
    return WsConn(h, journal)


class WsConn:
    """One accepted connection: frame IO (server frames unmasked, client frames must be masked) and the receive loop"""

    def __init__(self, h, journal):
        self.sock = h.connection
        self.rfile = h.rfile
        self.wfile = h.wfile
        self.journal = journal
        self.lock = threading.Lock()
        self.sent_close = False
        self.sock.settimeout(HOLD)

    def read_exact(self, count):
        data = self.rfile.read(count) if count else b""
        if data is None or len(data) < count:
            raise EOFError("the client ended the connection")
        return data

    def read_frame(self):
        first, second = self.read_exact(2)
        fin, rsv, op = first >> 7, (first >> 4) & 7, first & 0x0F
        masked, length = second >> 7, second & 0x7F
        if length == 126:
            length = struct.unpack("!H", self.read_exact(2))[0]
        elif length == 127:
            length = struct.unpack("!Q", self.read_exact(8))[0]
        # Both refused from the header alone: nothing of the payload is read (review M-10)
        if op >= 0x8 and length > 125:
            raise ProtocolError(1002, "a control frame is at most 125 bytes")
        if length > MAX_MESSAGE:
            raise ProtocolError(1009, "a frame over the server's limit")
        mask = self.read_exact(4) if masked else b""
        payload = self.read_exact(length)
        if masked:
            payload = unmask(payload, mask)
        self.journal.add("frame", op=op, fin=fin, rsv=rsv, masked=masked, **inline(payload))
        return fin, rsv, op, masked, payload

    def send_frame(self, op, payload=b"", fin=True, journal=True):
        first = (0x80 if fin else 0) | op
        length = len(payload)
        if length < 126:
            head = struct.pack("!BB", first, length)
        elif length < 65536:
            head = struct.pack("!BBH", first, 126, length)
        else:
            head = struct.pack("!BBQ", first, 127, length)
        with self.lock:
            self.wfile.write(head + payload)
        if journal:
            self.journal.add("sent", op=op, fin=int(fin), **inline(payload))

    def send_message(self, kind, data, fragment=0):
        op = OP_TEXT if kind == "text" else OP_BINARY
        if fragment <= 0 or len(data) <= fragment:
            self.send_frame(op, data)
            return
        for offset in range(0, len(data), fragment):
            self.send_frame(op if offset == 0 else OP_CONT, data[offset:offset + fragment], fin=offset + fragment >= len(data))

    def send_close(self, payload):
        self.sent_close = True
        self.send_frame(OP_CLOSE, payload)

    def fail(self, code, reason):
        self.journal.add("fail", code=code, reason=reason)
        if not self.sent_close:
            self.send_close(struct.pack("!H", code) + reason.encode("utf-8")[:123])
        self.wait_close(CLOSE_WAIT)

    def wait_close(self, seconds):
        """After our close: reads until the client's close (journaled as "close") or EOF, at most seconds"""
        self.sock.settimeout(seconds)
        try:
            while True:
                _, _, op, _, payload = self.read_frame()
                if op == OP_CLOSE:
                    self.journal.add("close", **close_fields(payload))
                    return
        except (EOFError, ProtocolError):
            self.journal.add("eof")
        except socket.timeout:
            self.journal.add("timeout")
        except OSError as error:
            self.journal.add("error", text=repr(error))

    def wait_eof(self, seconds):
        """Reads (and journals) everything until the client ends TCP or seconds pass"""
        self.sock.settimeout(seconds)
        try:
            while True:
                self.read_frame()
        except (EOFError, ProtocolError):
            self.journal.add("eof")
        except socket.timeout:
            self.journal.add("timeout")
        except OSError as error:
            self.journal.add("error", text=repr(error))

    def serve(self, on_message=None, answer_ping=True, echo_close=True, on_frame=None, strict_utf8=True):
        """The receive loop: assembles messages, answers control frames, ends at the client's close or EOF"""
        kind, parts, total = None, [], 0
        try:
            while True:
                fin, rsv, op, masked, payload = self.read_frame()
                if not masked:
                    raise ProtocolError(1002, "client frames must be masked")
                if rsv:
                    raise ProtocolError(1002, "no extension was negotiated")
                if on_frame:
                    on_frame(self, op, fin, payload)
                if op >= 0x8:
                    if not fin:
                        raise ProtocolError(1002, "a control frame is final")
                    if op == OP_PING:
                        if answer_ping:
                            self.send_frame(OP_PONG, payload)
                    elif op == OP_CLOSE:
                        self.journal.add("close", **close_fields(payload))
                        if echo_close and not self.sent_close:
                            # RFC 6455 5.5.1: answer with a close, echoing the status code
                            self.send_close(payload[:2])
                        return "close"
                    elif op != OP_PONG:
                        raise ProtocolError(1002, "an unknown control opcode")
                    continue
                if op == OP_CONT:
                    if kind is None:
                        raise ProtocolError(1002, "a continuation outside a message")
                elif op in (OP_TEXT, OP_BINARY):
                    if kind is not None:
                        raise ProtocolError(1002, "a new message inside a fragmented one")
                    kind = "text" if op == OP_TEXT else "binary"
                else:
                    raise ProtocolError(1002, "an unknown data opcode")
                # Checked before the append: the assembled message never grows past the limit (review M-10)
                if total + len(payload) > MAX_MESSAGE:
                    raise ProtocolError(1009, "a message over the server's limit")
                parts.append(payload)
                total += len(payload)
                if not fin:
                    continue
                data, done = b"".join(parts), kind
                kind, parts, total = None, [], 0
                fields = message_fields(done, data)
                if done == "text" and server.decode_text(data) is None:
                    self.journal.add("message", valid_utf8=False, **fields)
                    if strict_utf8:
                        raise ProtocolError(1007, "invalid UTF-8 in a text message")
                else:
                    self.journal.add("message", **fields)
                if on_message:
                    on_message(self, done, data)
        except ProtocolError as error:
            self.fail(error.code, str(error))
            return "fail"
        except EOFError:
            self.journal.add("eof")
            return "eof"
        except socket.timeout:
            self.journal.add("timeout")
            return "timeout"
        except OSError as error:
            self.journal.add("error", text=repr(error))
            return "error"


def echo(conn, fragment=0, **policy):
    return conn.serve(on_message=lambda c, kind, data: c.send_message(kind, data, fragment), **policy)


def ws_echo(h, match, journal):
    fragment = server.query_int(h, "fragment", 0, MAX_MESSAGE)
    echo(accept(h, journal), fragment)


def ws_status(h, match, journal):
    code = server.path_int(match, "code", 599)
    if code < 200:
        raise server.Refused(400, "out of range", name="code", value=code, min=200, max=599)
    headers = [("Connection", "close")]
    if "location" in h.query:
        headers.append(("Location", h.query["location"][0]))
    if code == 401:
        headers.append(("WWW-Authenticate", "Bearer"))
    journal.add("response", status=code)
    h.send_json(code, {"status": code}, headers)


def route_journal(h, match):
    key = h.query.get("key", [""])[0]
    wait = server.query_int(h, "wait_ms", 0, server.MAX_MS) / 1000.0
    connections = server.query_int(h, "connections", 1, MAX_KEYS, low=1)
    deadline = time.monotonic() + wait
    with JOURNAL_LOCK:
        while True:
            entry = JOURNALS.get(key)
            ended = entry is not None and entry["started"] >= connections and entry["ended"] >= entry["started"]
            remaining = deadline - time.monotonic()
            if ended or remaining <= 0:
                break
            JOURNAL_LOCK.wait(remaining)
        snapshot = {"key": key, "connections": entry["started"] if entry else 0, "ended": ended,
                    "dropped": entry["dropped"] if entry else 0, "events": list(entry["events"]) if entry else []}
        if h.query.get("reset", ["0"])[0] == "1":
            JOURNALS.pop(key, None)
    h.send_json(200, snapshot)


def handle_upgrade(h):
    url = urllib.parse.urlsplit(h.path)
    # server.py's dispatch parses the query after its upgrade check; the routes here read it as its own routes do
    h.query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
    journal = Journal(h.query.get("key", [None])[0])
    journal.add("request", method=h.command, target=h.path, request_line=h.requestline,
                headers=[[name, value] for name, value in h.headers.items()])
    h.close_connection = True
    try:
        for pattern, scenario in SCENARIOS:
            match = pattern.fullmatch(url.path)
            if match:
                scenario(h, match, journal)
                return
        raise server.Refused(404, "no WebSocket route", path=url.path)
    except server.Refused as refused:
        # Raised before any 101: server.py's dispatch answers it as JSON and closes
        journal.add("refused", status=refused.status, error=str(refused))
        raise
    except (BrokenPipeError, ConnectionResetError) as error:
        journal.add("error", text=repr(error))
    finally:
        journal.end()


SCENARIOS = [(re.compile(pattern), scenario) for pattern, scenario in [
    (r"/ws/echo", ws_echo),
    (r"/ws/status/(\d{3})", ws_status),
]]

server.register_upgrade("websocket", handle_upgrade)
server.register_route(("GET",), r"/ws/journal", route_journal)

if __name__ == "__main__":
    sys.exit(server.main())
```

- [ ] **Step 8: Run the selftests to see them pass**

Run: `python3 -I Tools/testserver/ws_selftest.py -v && python3 -I Tools/testserver/selftest.py -v`
Expected: every test passes in both; `git status --short Tools/testserver` shows no `__pycache__`.

Restore-the-bug: in `WsConn.read_frame`, move the `if length > MAX_MESSAGE` check below `payload = self.read_exact(length)`. `test_lengths_over_the_limit_close_1009` must fail: the server waits for 2^63 bytes and the client's `message()` times out. Restore the check above the read and see it pass. Report both outcomes.

- [ ] **Step 9: Commit**

```bash
git add Tools/testserver/ws.py Tools/testserver/ws_selftest.py Tools/testserver/server.py Tools/testserver/selftest.py
git commit -m "testserver: ws.py core (WS W0) -- RFC 6455 handshake, frames, echo, refusals, the per-key journal and /ws/journal

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 2: `ws.py` scenario routes

**Files:**
- Modify: `Tools/testserver/ws.py` (new scenario functions above `SCENARIOS`, the `SCENARIOS` list, the docstring's route list)
- Modify: `Tools/testserver/ws_selftest.py` (new `ScenarioTest` class above `class ScriptModeTest`)

**Interfaces:**
- Consumes (Task 1): `accept`, `check_handshake`, `accept_value`, `WsConn` (`send_frame`, `send_message`, `send_close`, `serve`, `wait_close`, `wait_eof`, `sock`), `echo`, `Journal.add`, `server.query_int`, `server.path_int`, `server.Refused`, `server.LOCK`, `server.STATE`, `server.MAX_MS`, `server.MAX_COUNT`, the ws_selftest helpers (`WsTestBase`, `Client`, `close_code`).
- Produces (the probe's contract with the server; Tasks 5–9 use these exact paths and journal events):

| Route | Behaviour | Journal events it adds |
| --- | --- | --- |
| `/ws/close/<code>\|none\|onebyte?msg=&reason=` | optional text `msg`, then a close (code 0–65535 raw, `reason` UTF-8, payload ≤ 125; `none`: empty payload; `onebyte`: `03`), then waits `CLOSE_WAIT` for the client's close | `sent` op 8; the client's answer as `close` |
| `/ws/drop?after_ms=&msg=&rst=0\|1` | optional `msg`, waits, then FIN (or RST with `rst=1`), no close frame | `fin` or `rst` |
| `/ws/silent` | accepts, then answers nothing: no PONG, no close echo, no data; reads until the client ends TCP | frames, `close`, `eof` |
| `/ws/noecho` | echo mode, except the client's close is never answered; waits for the client's TCP end | `close`, then `eof` (times show how long the client waited) |
| `/ws/serverping?count=&interval_ms=&size=&unsolicited=0\|1` | another thread sends `count` PINGs `ping-<i>` padded with `.` to `size` bytes, `interval_ms` apart, then text `pings-done`; `unsolicited=1` first sends a PONG `unsolicited`; echo mode | `sent` op 9 / op 10; the client's PONGs as `frame` op 10 |
| `/ws/fragment?(text=\|cyrillic=N\|bytes=N)&at=o1,o2&split_inside=o1,o2&binary=0\|1&ping_between=0\|1` | one message cut into frames at `at` offsets and, for each `split_inside` value, at the first later offset inside a UTF-8 sequence; `ping_between=1` sends PING `between` after the first frame; echo mode | `message_sent` (kind, len, sha256, frames_at) |
| `/ws/large?bytes=N&ascii=0\|1&binary=0\|1&frame=F&seed=S` | one N-byte message (ASCII letters, two-byte Cyrillic UTF-8, or seeded random bytes) in frames of F bytes (0: one frame); echo mode | `message_sent` |
| `/ws/order?n=K` | K messages alternating text `t<i>` and binary `<i>` as 4 big-endian bytes; echo mode | `order_sent` |
| `/ws/headers` | first message: the handshake as JSON (`request_line`, `headers` `[[name, value]...]` in order and case, `path`, `query`); echo mode | |
| `/ws/protocol?select=<p>` | answers `Sec-WebSocket-Protocol: <p>` whatever was offered (none when empty); echo mode | `response` with protocol |
| `/ws/auth[?token=]` | 101 only with `/login`'s current access token (Bearer or `?token=`), else 401 + `WWW-Authenticate: Bearer`; echo mode | |
| `/ws/flaky?n=K&key=k` | the first K upgrades per key end TCP before any status line; then echo mode | `dropped_before_status` |
| `/ws/noupgrade` | reads the request, ends TCP before any status line (spec §22 A2) | `dropped_before_status` |
| `/ws/hang?ms=` | reads the request, never answers, holds until the client ends TCP or `ms` (default 60000) | `eof` or `timeout` |
| `/ws/slowupgrade?ms=` | waits `ms` before the 101; echo mode | |
| `/ws/badutf8?where=text\|close` | text `ok C3 28`, or a close 1000 with reason `C3 28`; then echo mode / waits for the answer | |
| `/ws/broken?what=accept\|upgrade\|connection` | a 101 with a wrong `Sec-WebSocket-Accept`, or without `Upgrade`, or without `Connection`; holds `CLOSE_WAIT` | `response` with broken |
| `/ws/sink?push=<t>&after_frames=N` | journals everything, echoes nothing; after the N-th data frame (default 1) pushes text `<t>` once; PINGs and the close are answered | |
| `/ws/flood?bytes=N&chunk=C` | N bytes as binary messages of C bytes, as fast as TCP takes them; then echo mode | `flood` (bytes, chunk, longest_write_s, writes_over_100ms, seconds) |

- [ ] **Step 1: Write the failing scenario tests**

In `Tools/testserver/ws_selftest.py`, insert above `class ScriptModeTest`:

```python
class ScenarioTest(WsTestBase):
    def test_close_with_code_message_and_answer(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/close/4001?msg=hi&reason=bye&key=" + key)
        self.assertEqual(client.message(), ("text", b"hi"))
        self.assertEqual(client.message(), ("close", struct.pack("!H", 4001) + b"bye"))
        client.close(4001)
        journal = self.ended(key)
        self.assertEqual(self.of(journal, "close")[0]["code"], 4001)
        self.assertEqual(len([event for event in self.of(journal, "sent") if event["op"] == ws.OP_CLOSE]), 1)

    def test_close_payload_shapes(self):
        self.assertEqual(self.connect("/ws/close/none").message(), ("close", b""))
        self.assertEqual(self.connect("/ws/close/onebyte").message(), ("close", b"\x03"))
        self.assertEqual(self.connect("/ws/close/1005").message(), ("close", struct.pack("!H", 1005)))
        self.assertEqual(self.connect("/ws/close/65535").message(), ("close", struct.pack("!H", 65535)))
        self.assertEqual(self.connect("/ws/close/70000").status, 400)
        self.assertEqual(self.connect("/ws/close/1000?reason=" + "r" * 124).status, 400)

    def test_drop_without_a_close_frame(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/drop?after_ms=50&msg=bye&key=" + key)
        self.assertEqual(client.message(), ("text", b"bye"))
        with self.assertRaises(EOFError):
            client.message()
        self.assertEqual(len(self.of(self.ended(key), "fin")), 1)
        client = self.connect("/ws/drop?rst=1")
        with self.assertRaises((EOFError, ConnectionResetError)):
            client.message()

    def test_silent_answers_nothing(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/silent?key=" + key)
        client.send(ws.OP_PING, b"anyone?")
        client.send(ws.OP_TEXT, b"hello?")
        # Completion before silence: the server has read both frames, and nothing came back
        deadline = time.monotonic() + 5
        while len(self.of(self.journal(key, wait_ms=0), "frame")) < 2 and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(len(self.of(self.journal(key, wait_ms=0), "frame")), 2)
        client.sock.settimeout(0.5)
        with self.assertRaises((socket.timeout, TimeoutError)):
            client.recv()

    def test_noecho_never_answers_the_close(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/noecho?key=" + key)
        client.send(ws.OP_TEXT, b"x")
        self.assertEqual(client.message(), ("text", b"x"))
        client.close(1000)
        deadline = time.monotonic() + 5
        while not self.of(self.journal(key, wait_ms=0), "close") and time.monotonic() < deadline:
            time.sleep(0.02)
        self.assertEqual(len(self.of(self.journal(key, wait_ms=0), "close")), 1)
        client.sock.settimeout(0.5)
        with self.assertRaises((socket.timeout, TimeoutError)):
            client.recv()
        client.shutdown()
        journal = self.ended(key)
        self.assertLess(self.of(journal, "close")[0]["t"], self.of(journal, "eof")[0]["t"])

    def test_serverping(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/serverping?count=2&interval_ms=50&size=20&unsolicited=1&key=" + key)
        self.assertEqual(client.message(), ("pong", b"unsolicited"))
        for index in range(2):
            kind, payload = client.message()
            self.assertEqual((kind, payload), ("ping", (b"ping-%d" % index).ljust(20, b".")))
            client.send(ws.OP_PONG, payload)
        self.assertEqual(client.message(), ("text", b"pings-done"))
        client.close()
        journal = self.ended(key)
        self.assertEqual(len([event for event in self.of(journal, "frame") if event["op"] == ws.OP_PONG]), 2)

    def test_fragment_offsets(self):
        key = uuid.uuid4().hex
        text = "Привет".encode("utf-8")
        client = self.connect("/ws/fragment?at=3,7&text=%s&key=%s" % (urllib.parse.quote("Привет"), key))
        self.assertEqual([client.recv() for _ in range(3)], [(0, ws.OP_TEXT, text[:3]), (0, ws.OP_CONT, text[3:7]), (1, ws.OP_CONT, text[7:])])
        client = self.connect("/ws/fragment?cyrillic=100&split_inside=10&ping_between=1&key=" + key)
        first = client.recv()
        self.assertEqual(client.recv(), (1, ws.OP_PING, b"between"))
        rest = client.recv()
        self.assertEqual((first[0], first[1], rest[0], rest[1]), (0, ws.OP_TEXT, 1, ws.OP_CONT))
        # The cut lands inside a two-byte letter: the second frame starts with a continuation byte
        self.assertEqual(rest[2][0] & 0xC0, 0x80)
        self.assertEqual(len((first[2] + rest[2]).decode("utf-8").encode("utf-8")), 100)
        sent = self.of(self.journal(key, connections=2, wait_ms=0), "message_sent", conn=2)[0]
        self.assertEqual(sent["frames_at"], [len(first[2])])
        self.assertEqual(self.connect("/ws/fragment?text=abc&at=5").status, 400)

    def test_large(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/large?bytes=100000&ascii=0&frame=4096&key=" + key)
        frames = [client.recv()]
        while not frames[-1][0]:
            frames.append(client.recv())
        self.assertEqual(len(frames), 25)
        data = b"".join(frame[2] for frame in frames)
        self.assertEqual(len(data), 100000)
        data.decode("utf-8")
        self.assertEqual(self.of(self.journal(key, wait_ms=0), "message_sent")[0]["sha256"], hashlib.sha256(data).hexdigest())
        self.assertEqual(self.connect("/ws/large?bytes=1000&binary=1&seed=3").message(), ("binary", random.Random(3).randbytes(1000)))
        self.assertEqual(self.connect("/ws/large?bytes=%d" % (ws.MAX_MESSAGE + 1)).status, 400)

    def test_order(self):
        client = self.connect("/ws/order?n=6")
        self.assertEqual([client.message() for _ in range(6)],
                         [("text", b"t0"), ("binary", struct.pack("!I", 1)), ("text", b"t2"), ("binary", struct.pack("!I", 3)),
                          ("text", b"t4"), ("binary", struct.pack("!I", 5))])

    def test_headers(self):
        client = self.connect("/ws/headers?x=1", headers=[("X-Probe", "Value With Case")])
        kind, data = client.message()
        seen = json.loads(data)
        self.assertEqual(kind, "text")
        self.assertIn(["X-Probe", "Value With Case"], seen["headers"])
        self.assertEqual((seen["path"], seen["query"]), ("/ws/headers", {"x": ["1"]}))

    def test_protocol_selection(self):
        chosen = self.connect("/ws/protocol?select=chat.v2", protocols=("chat.v1",))
        self.assertEqual((chosen.status, chosen.headers.get("sec-websocket-protocol")), (101, "chat.v2"))
        self.assertNotIn("sec-websocket-protocol", self.connect("/ws/protocol?select=", protocols=("chat.v1",)).headers)
        self.assertEqual(self.connect("/ws/protocol?select=a%0D%0Ab").status, 400)

    def test_auth_uses_the_http_tokens(self):
        refused = self.connect("/ws/auth")
        self.assertEqual((refused.status, refused.headers.get("www-authenticate")), (401, "Bearer"))
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        connection.request("POST", "/login", b'{"user":"neo","password":"trinity"}')
        tokens = json.loads(connection.getresponse().read())
        connection.close()
        self.assertTrue(self.connect("/ws/auth", headers=[("Authorization", "Bearer " + tokens["accessToken"])]).accepted)
        self.assertTrue(self.connect("/ws/auth?token=" + tokens["accessToken"]).accepted)

    def test_flaky_noupgrade_hang_slowupgrade(self):
        key = uuid.uuid4().hex
        self.assertEqual(self.connect("/ws/flaky?n=1&key=" + key).status, 0)
        self.assertTrue(self.connect("/ws/flaky?n=1&key=" + key).accepted)
        self.assertEqual(self.connect("/ws/noupgrade").status, 0)
        started = time.monotonic()
        self.assertEqual(self.connect("/ws/hang?ms=300").status, 0)
        self.assertGreaterEqual(time.monotonic() - started, 0.25)
        started = time.monotonic()
        self.assertTrue(self.connect("/ws/slowupgrade?ms=300").accepted)
        self.assertGreaterEqual(time.monotonic() - started, 0.25)

    def test_badutf8(self):
        self.assertEqual(self.connect("/ws/badutf8?where=text").recv(), (1, ws.OP_TEXT, b"ok \xc3\x28"))
        self.assertEqual(self.connect("/ws/badutf8?where=close").message(), ("close", struct.pack("!H", 1000) + b"\xc3\x28"))
        self.assertEqual(self.connect("/ws/badutf8?where=nope").status, 400)

    def test_broken_handshakes(self):
        wrong = self.connect("/ws/broken?what=accept")
        self.assertEqual((wrong.status, wrong.accepted), (101, False))
        self.assertNotIn("upgrade", self.connect("/ws/broken?what=upgrade").headers)
        self.assertNotIn("connection", self.connect("/ws/broken?what=connection").headers)

    def test_sink_pushes_and_echoes_nothing(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/sink?push=2&after_frames=1&key=" + key)
        client.send(ws.OP_TEXT, b"a")
        self.assertEqual(client.message(), ("text", b"2"))
        client.send(ws.OP_TEXT, b"b")
        client.send(ws.OP_PING, b"p")
        # The PONG is the next frame: "b" was not echoed before it
        self.assertEqual(client.message(), ("pong", b"p"))
        client.close()
        self.assertEqual(client.message()[0], "close")
        self.assertEqual([event.get("text") for event in self.of(self.ended(key), "message")], ["a", "b"])

    def test_flood(self):
        key = uuid.uuid4().hex
        client = self.connect("/ws/flood?bytes=262144&chunk=65536&key=" + key)
        self.assertEqual([(kind, len(data)) for kind, data in (client.message() for _ in range(4))], [("binary", 65536)] * 4)
        client.close()
        self.assertEqual(client.message()[0], "close")
        self.assertEqual(self.of(self.ended(key), "flood")[0]["bytes"], 262144)
```

- [ ] **Step 2: Run them to see them fail**

Run: `python3 -I Tools/testserver/ws_selftest.py ScenarioTest -v`
Expected: every `ScenarioTest` test fails. The new routes answer 404 (`no WebSocket route`), so `message()` raises `EOFError` or a status check sees 404.

- [ ] **Step 3: Add the scenarios to ws.py**

In `Tools/testserver/ws.py`, insert directly above the line `SCENARIOS = [(re.compile(pattern), scenario) for pattern, scenario in [`:

```python
CYRILLIC = "Съешь же ещё этих мягких французских булок, да выпей чаю. ".encode("utf-8")


def valid_utf8(data):
    return server.decode_text(data) is not None


def cyrillic(count):
    """count bytes of valid UTF-8, mostly two-byte Cyrillic letters, so that some cut points fall inside a code point"""
    data = (CYRILLIC * (count // len(CYRILLIC) + 1))[:count]
    # The cut may have split the last letter: drop its lead byte and pad with ASCII to the exact length
    while data and not valid_utf8(data):
        data = data[:-1]
    return data + b"x" * (count - len(data))


def parse_offsets(h, data):
    """at=: exact offsets; split_inside=: for each value, the first offset at or after it inside a UTF-8 sequence"""
    offsets = []
    for name in ("at", "split_inside"):
        for part in filter(None, h.query.get(name, [""])[0].split(",")):
            try:
                value = int(part)
            except ValueError:
                raise server.Refused(400, "not an integer", name=name, value=part)
            if name == "split_inside":
                while 0 <= value < len(data) and (data[value] & 0xC0) != 0x80:
                    value += 1
            if not 0 < value < len(data):
                raise server.Refused(400, "an offset must fall inside the message", name=name, value=value, length=len(data))
            offsets.append(value)
    return sorted(set(offsets))


def drop_before_status(h, journal):
    journal.add("dropped_before_status")
    try:
        h.connection.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def ws_close(h, match, journal):
    what = match.group(1)
    if what == "none":
        payload = b""
    elif what == "onebyte":
        payload = b"\x03"
    else:
        payload = struct.pack("!H", server.path_int(match, "code", 65535)) + h.query.get("reason", [""])[0].encode("utf-8")
        if len(payload) > 125:
            raise server.Refused(400, "a close payload is at most 125 bytes (RFC 6455 5.5)", length=len(payload))
    conn = accept(h, journal)
    if "msg" in h.query:
        conn.send_message("text", h.query["msg"][0].encode("utf-8"))
    conn.send_close(payload)
    conn.wait_close(CLOSE_WAIT)


def ws_drop(h, match, journal):
    after = server.query_int(h, "after_ms", 0, server.MAX_MS) / 1000.0
    rst = server.query_int(h, "rst", 0, 1)
    conn = accept(h, journal)
    if "msg" in h.query:
        conn.send_message("text", h.query["msg"][0].encode("utf-8"))
    time.sleep(after)
    if rst:
        # SO_LINGER on with a zero timeout: close() sends RST instead of FIN
        conn.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        journal.add("rst")
        conn.sock.close()
        return
    journal.add("fin")
    try:
        conn.sock.shutdown(socket.SHUT_RDWR)
    except OSError:
        pass


def ws_silent(h, match, journal):
    conn = accept(h, journal)
    if conn.serve(answer_ping=False, echo_close=False) == "close":
        conn.wait_eof(HOLD)


def ws_noecho(h, match, journal):
    conn = accept(h, journal)
    if echo(conn, echo_close=False) == "close":
        conn.wait_eof(HOLD)


def ws_serverping(h, match, journal):
    count = server.query_int(h, "count", 3, 100, low=1)
    interval = server.query_int(h, "interval_ms", 200, server.MAX_MS) / 1000.0
    size = server.query_int(h, "size", 0, 125)
    unsolicited = server.query_int(h, "unsolicited", 0, 1)
    conn = accept(h, journal)

    def pinger():
        try:
            if unsolicited:
                conn.send_frame(OP_PONG, b"unsolicited")
            for index in range(count):
                if index:
                    time.sleep(interval)
                conn.send_frame(OP_PING, (b"ping-%d" % index).ljust(size, b"."))
            time.sleep(interval)
            conn.send_message("text", b"pings-done")
        except OSError:
            pass

    threading.Thread(target=pinger, daemon=True).start()
    echo(conn)


def ws_fragment(h, match, journal):
    binary = server.query_int(h, "binary", 0, 1)
    if "text" in h.query:
        data = h.query["text"][0].encode("utf-8")
    elif "cyrillic" in h.query:
        data = cyrillic(server.query_int(h, "cyrillic", 4096, MAX_MESSAGE, low=1))
    else:
        data = random.Random(1).randbytes(server.query_int(h, "bytes", 4096, MAX_MESSAGE, low=1))
    offsets = parse_offsets(h, data)
    ping_between = server.query_int(h, "ping_between", 0, 1)
    conn = accept(h, journal)
    journal.add("message_sent", kind="binary" if binary else "text", len=len(data), sha256=hashlib.sha256(data).hexdigest(),
                frames_at=offsets)
    cuts = [0] + offsets + [len(data)]
    for index in range(len(cuts) - 1):
        first, last = index == 0, index == len(cuts) - 2
        op = (OP_BINARY if binary else OP_TEXT) if first else OP_CONT
        conn.send_frame(op, data[cuts[index]:cuts[index + 1]], fin=last)
        if ping_between and first and not last:
            conn.send_frame(OP_PING, b"between")
    echo(conn)


def ws_large(h, match, journal):
    count = server.query_int(h, "bytes", 65536, MAX_MESSAGE, low=1)
    binary = server.query_int(h, "binary", 0, 1)
    ascii_only = server.query_int(h, "ascii", 1, 1)
    frame = server.query_int(h, "frame", 0, MAX_MESSAGE)
    seed = server.query_int(h, "seed", 1, 2 ** 31)
    if binary:
        data = random.Random(seed).randbytes(count)
    elif ascii_only:
        data = (b"abcdefghijklmnopqrstuvwxyz" * (count // 26 + 1))[:count]
    else:
        data = cyrillic(count)
    conn = accept(h, journal)
    journal.add("message_sent", kind="binary" if binary else "text", len=count, sha256=hashlib.sha256(data).hexdigest())
    conn.send_message("binary" if binary else "text", data, frame)
    echo(conn)


def ws_order(h, match, journal):
    count = server.query_int(h, "n", 100, 100000, low=1)
    conn = accept(h, journal)
    for index in range(count):
        if index % 2 == 0:
            conn.send_frame(OP_TEXT, b"t%d" % index, journal=False)
        else:
            conn.send_frame(OP_BINARY, struct.pack("!I", index), journal=False)
    journal.add("order_sent", n=count)
    echo(conn)


def ws_headers(h, match, journal):
    conn = accept(h, journal)
    seen = {"request_line": h.requestline, "headers": [[name, value] for name, value in h.headers.items()],
            "path": urllib.parse.urlsplit(h.path).path, "query": h.query}
    conn.send_message("text", json.dumps(seen, separators=(",", ":")).encode("utf-8"))
    echo(conn)


def ws_protocol(h, match, journal):
    selected = h.query.get("select", [""])[0]
    if "\r" in selected or "\n" in selected:
        raise server.Refused(400, "select holds a line break")
    echo(accept(h, journal, protocol=selected or None))


def ws_auth(h, match, journal):
    with server.LOCK:
        current = server.STATE["tokens"]["access"]
    offered = h.headers.get("Authorization", "")
    token = h.query.get("token", [None])[0]
    if current is None or (offered != "Bearer " + current and token != current):
        journal.add("response", status=401)
        h.send_json(401, {"error": "unauthorized"}, [("WWW-Authenticate", "Bearer"), ("Connection", "close")])
        return
    echo(accept(h, journal))


def ws_flaky(h, match, journal):
    failures = server.query_int(h, "n", 1, server.MAX_COUNT)
    name = h.query.get("key", ["default"])[0]
    with server.LOCK:
        seen = WS_STATE["flaky"].get(name, 0)
        WS_STATE["flaky"][name] = seen + 1
    if seen < failures:
        drop_before_status(h, journal)
        return
    echo(accept(h, journal))


def ws_noupgrade(h, match, journal):
    drop_before_status(h, journal)


def ws_hang(h, match, journal):
    hold = server.query_int(h, "ms", int(HOLD * 1000), server.MAX_MS) / 1000.0
    h.connection.settimeout(hold)
    try:
        while h.connection.recv(65536):
            pass
        journal.add("eof")
    except socket.timeout:
        journal.add("timeout")
    except OSError:
        journal.add("eof")


def ws_slowupgrade(h, match, journal):
    time.sleep(server.query_int(h, "ms", 1000, server.MAX_MS) / 1000.0)
    echo(accept(h, journal))


def ws_badutf8(h, match, journal):
    where = h.query.get("where", ["text"])[0]
    if where not in ("text", "close"):
        raise server.Refused(400, "where is text or close", value=where)
    conn = accept(h, journal)
    if where == "text":
        conn.send_frame(OP_TEXT, b"ok \xc3\x28")
        echo(conn)
        return
    conn.send_close(struct.pack("!H", 1000) + b"\xc3\x28")
    conn.wait_close(CLOSE_WAIT)


def ws_broken(h, match, journal):
    what = h.query.get("what", ["accept"])[0]
    if what not in ("accept", "upgrade", "connection"):
        raise server.Refused(400, "what is accept, upgrade or connection", value=what)
    key = check_handshake(h)
    lines = [b"HTTP/1.1 101 Switching Protocols"]
    if what != "upgrade":
        lines.append(b"Upgrade: websocket")
    if what != "connection":
        lines.append(b"Connection: Upgrade")
    lines.append(b"Sec-WebSocket-Accept: " + accept_value(key + "x" if what == "accept" else key).encode("ascii"))
    h.wfile.write(b"\r\n".join(lines) + b"\r\n\r\n")
    journal.add("response", status=101, broken=what)
    WsConn(h, journal).wait_eof(CLOSE_WAIT)


def ws_sink(h, match, journal):
    push = h.query.get("push", [None])[0]
    after = server.query_int(h, "after_frames", 1, MAX_EVENTS, low=1)
    conn = accept(h, journal)
    counted = [0]

    def on_frame(connection, op, fin, payload):
        if op in DATA_OPS:
            counted[0] += 1
            if push is not None and counted[0] == after:
                connection.send_frame(OP_TEXT, push.encode("utf-8"))

    conn.serve(on_frame=on_frame)


def ws_flood(h, match, journal):
    total = server.query_int(h, "bytes", 8 * 1024 * 1024, MAX_MESSAGE, low=1)
    chunk = server.query_int(h, "chunk", 65536, MAX_MESSAGE, low=1)
    conn = accept(h, journal)
    block = random.Random(7).randbytes(chunk)
    sent, longest, blocked, started = 0, 0.0, 0, time.monotonic()
    while sent < total:
        piece = block[:min(chunk, total - sent)]
        before = time.monotonic()
        conn.send_frame(OP_BINARY, piece, journal=False)
        took = time.monotonic() - before
        longest = max(longest, took)
        blocked += 1 if took > 0.1 else 0
        sent += len(piece)
    journal.add("flood", bytes=sent, chunk=chunk, longest_write_s=round(longest, 4), writes_over_100ms=blocked,
                seconds=round(time.monotonic() - started, 4))
    echo(conn)


```

Replace the whole `SCENARIOS = ...` statement with:

```python
SCENARIOS = [(re.compile(pattern), scenario) for pattern, scenario in [
    (r"/ws/echo", ws_echo),
    (r"/ws/status/(\d{3})", ws_status),
    (r"/ws/close/(\d{1,5}|none|onebyte)", ws_close),
    (r"/ws/drop", ws_drop),
    (r"/ws/silent", ws_silent),
    (r"/ws/noecho", ws_noecho),
    (r"/ws/serverping", ws_serverping),
    (r"/ws/fragment", ws_fragment),
    (r"/ws/large", ws_large),
    (r"/ws/order", ws_order),
    (r"/ws/headers", ws_headers),
    (r"/ws/protocol", ws_protocol),
    (r"/ws/auth", ws_auth),
    (r"/ws/flaky", ws_flaky),
    (r"/ws/noupgrade", ws_noupgrade),
    (r"/ws/hang", ws_hang),
    (r"/ws/slowupgrade", ws_slowupgrade),
    (r"/ws/badutf8", ws_badutf8),
    (r"/ws/broken", ws_broken),
    (r"/ws/sink", ws_sink),
    (r"/ws/flood", ws_flood),
]]
```

In the module docstring, append after the `/ws/status/<code>` entry (same indentation as the existing lines):

```
  /ws/close/<code>|none|onebyte?msg=<text>&reason=<text>
                                   optional text msg, then our close (any code, raw; none: empty payload; onebyte: 03),
                                   then up to CLOSE_WAIT for the client's answer (journaled as "close")
  /ws/drop?after_ms=&msg=&rst=0|1  optional msg, then TCP ends without a close frame (FIN, or RST with rst=1)
  /ws/silent                       answers nothing at all (no PONG, no close echo, no data) until the client ends TCP
  /ws/noecho                       echo mode, but the client's close is never answered; waits for the client's TCP end
  /ws/serverping?count=&interval_ms=&size=&unsolicited=0|1
                                   PINGs "ping-<i>" (padded to size with "."), then text "pings-done"; unsolicited=1
                                   first sends a PONG nobody asked for; echo mode
  /ws/fragment?(text=|cyrillic=N|bytes=N)&at=o,..&split_inside=o,..&binary=0|1&ping_between=0|1
                                   one message cut into frames at those offsets (split_inside: the first later offset
                                   inside a UTF-8 sequence), PING "between" after the first frame if asked; echo mode
  /ws/large?bytes=N&ascii=0|1&binary=0|1&frame=F&seed=S
                                   one N-byte message (ASCII, two-byte Cyrillic or seeded bytes) in F-byte frames
  /ws/order?n=K                    K messages alternating text "t<i>" and binary <i> (4 bytes, big-endian)
  /ws/headers                      first message: the handshake as received, as JSON; echo mode
  /ws/protocol?select=<p>          answers Sec-WebSocket-Protocol: <p> whatever was offered (none when empty)
  /ws/auth[?token=<t>]             101 only with /login's current access token (Bearer or token=), else 401
  /ws/flaky?n=K&key=<k>            the first K upgrades per key end TCP before any status line; then echo mode
  /ws/noupgrade                    ends TCP before any status line (WebSocket spec 22 A2: NoUpgradeResponse)
  /ws/hang?ms=<ms>                 never answers the upgrade; holds until the client ends TCP or ms pass
  /ws/slowupgrade?ms=<ms>          waits ms before the 101; echo mode
  /ws/badutf8?where=text|close     invalid UTF-8 (C3 28) in a text message or in our close reason
  /ws/broken?what=accept|upgrade|connection
                                   a 101 a client must refuse: wrong Sec-WebSocket-Accept, or no Upgrade, or no Connection
  /ws/sink?push=<t>&after_frames=N journals everything, echoes nothing; pushes text t once after the N-th data frame
  /ws/flood?bytes=N&chunk=C        N bytes as C-byte binary messages as fast as TCP takes them, then echo mode; the
                                   "flood" event says how long writes blocked (a client that stopped reading)

Not here yet: /ws/route (requests and replies, W4) and /ws/apping (app heartbeat, W3).
```

- [ ] **Step 4: Run the selftests to see them pass**

Run: `python3 -I Tools/testserver/ws_selftest.py -v && python3 -I Tools/testserver/selftest.py`
Expected: every test passes.

Restore-the-bug: in `ws_noecho`, replace `echo(conn, echo_close=False)` with `echo(conn)`. `test_noecho_never_answers_the_close` must fail: the client's `recv()` gets the echoed close instead of timing out. Restore and see it pass. Report both outcomes.

- [ ] **Step 5: Commit**

```bash
git add Tools/testserver/ws.py Tools/testserver/ws_selftest.py
git commit -m "testserver: ws.py scenario routes (WS W0) -- close shapes, drops, silent/noecho, server pings, fragments, large, order, headers, protocol, auth, flaky/noupgrade/hang, bad UTF-8, broken 101s, sink, flood

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 3: TLS listener, test certificates, runner

**Files:**
- Create: `Tools/testserver/certs/make_certs.sh`, `Tools/testserver/certs/README.md`
- Create (generated by the script, then committed): `Tools/testserver/certs/test-ca.pem`, `Tools/testserver/certs/test-server.pem`, `Tools/testserver/certs/test-server.key`
- Modify: `Tools/testserver/server.py` (imports, constants, `Handler.setup`, `Server.handle_error`, `TlsServer`, `make_tls_context`, `make_tls_server`, `route_tls_ca`, `ROUTES`, `main`, docstring)
- Modify: `Tools/testserver/selftest.py` (`import ssl`, a `ServerTest` test, `class TlsTest`, a `LifecycleTest` test)
- Modify: `Tools/testserver/ws_selftest.py` (`class WssTest`)
- Modify: `Tools/run_tests.sh` (`--tls-port 0`, `-VaRestTestServerTls=<url>`, header comment)

**Interfaces:**
- Consumes (Task 1): `ws_selftest.Client(..., tls=context)`, `ws_selftest.start`.
- Produces:
  - `server.CERTS_DIR`, `server.TLS_HANDSHAKE_TIMEOUT`, `server.make_tls_context()`, `server.make_tls_server(host="127.0.0.1", port=0)` (a `Server` whose sockets are TLS), `GET /tls/ca.pem` (the test CA, `application/x-pem-file`) on both listeners.
  - `python3 -I server.py ... --tls-port P`: also listens with TLS on P (0: the OS picks); prints `VaRestTestServerTls <https url>`; the ready file holds two lines, the http base URL and then the https base URL.
  - `Tools/run_tests.sh` passes `-VaRestTestServer=http://127.0.0.1:<port>` and `-VaRestTestServerTls=https://127.0.0.1:<tlsport>`.
  - Certificates (names chosen so a secret-scanner hit explains itself, ruling Q5): `test-ca.pem` (the test CA, `CN=VaRest Test CA (not trusted anywhere)`, `CA:TRUE, pathlen:0`), `test-server.pem` (the leaf for `DNS:localhost, IP:127.0.0.1, IP:::1`, then `test-ca.pem`: the chain the listener sends), `test-server.key` (the leaf's RSA 2048 key, unencrypted). All valid for 36500 days. `README.md` says what they are.
  - A listener whose certificate files cannot be loaded makes `server.py` print `VaRestTestServer: cannot start the TLS listener: <file>: <error>` and exit 2 (review M-11), so the runner's "did not start -- see <log>" names the file.

- [ ] **Step 1: Write the failing TLS selftests**

In `Tools/testserver/selftest.py`, add `import ssl` to the imports (alphabetical, after `import socket`). Add to `class ServerTest`:

```python
    def test_tls_ca_route(self):
        status, headers, data = self.call("GET", "/tls/ca.pem")
        with open(os.path.join(server.CERTS_DIR, "test-ca.pem"), "rb") as stream:
            self.assertEqual((status, headers["Content-Type"], data), (200, "application/x-pem-file", stream.read()))
```

Insert above `class LifecycleTest`:

```python
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
```

In `class LifecycleTest`, add:

```python
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
```

In `Tools/testserver/ws_selftest.py`, insert above `class ScriptModeTest`:

```python
class WssTest(unittest.TestCase):
    """ws.py over the TLS listener, verified against the test CA"""

    @classmethod
    def setUpClass(cls):
        cls.httpd = start(server.make_tls_server)
        cls.port = cls.httpd.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def test_wss_echo_verified(self):
        context = ssl.create_default_context(cafile=os.path.join(server.CERTS_DIR, "test-ca.pem"))
        client = Client(self.port, "/ws/echo", tls=context)
        self.addCleanup(client.shutdown)
        self.assertTrue(client.accepted, client.headers)
        client.send(ws.OP_TEXT, b"over tls")
        self.assertEqual(client.message(), ("text", b"over tls"))
        client.close()
        self.assertEqual(client.message()[0], "close")
```

- [ ] **Step 2: Run them to see them fail**

Run: `python3 -I Tools/testserver/selftest.py TlsTest -v`
Expected: ERROR in `setUpClass`, `AttributeError: module 'server' has no attribute 'make_tls_server'`.

- [ ] **Step 3: Write the certificate script and make the certificates**

Create `Tools/testserver/certs/make_certs.sh` and `chmod +x` it:

```bash
#!/usr/bin/env bash
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
# Makes the TLS listener's test certificates (WebSocket spec 16.3). Run once; the output is committed:
#   test-ca.pem      the throwaway test CA. Its private key is made in a temporary directory and deleted when this
#                    script exits, so nothing else can ever be signed by it.
#   test-server.pem  the listener's certificate (localhost, 127.0.0.1, ::1), then test-ca.pem: the chain it sends
#   test-server.key  the listener's private key (RSA 2048, unencrypted). A test key: it protects and signs nothing.
# Python's standard library cannot make certificates, so this needs the openssl command line (1.1.1 or later); the
# tests themselves do not. 36500 days: the suite must not expire under itself; regenerate before 2126.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
DAYS=36500

openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days "$DAYS" \
	-subj "/CN=VaRest Test CA (not trusted anywhere)" \
	-addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
	-addext "keyUsage=critical,keyCertSign,cRLSign" \
	-keyout "$WORK/ca.key" -out test-ca.pem

openssl req -newkey rsa:2048 -nodes -sha256 -subj "/CN=localhost" -keyout test-server.key -out "$WORK/server.csr"
cat > "$WORK/server.ext" <<'EOF'
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1
EOF
openssl x509 -req -sha256 -days "$DAYS" -in "$WORK/server.csr" -CA test-ca.pem -CAkey "$WORK/ca.key" \
	-CAcreateserial -CAserial "$WORK/ca.srl" -extfile "$WORK/server.ext" -out "$WORK/server.crt"

cat "$WORK/server.crt" test-ca.pem > test-server.pem
chmod 0644 test-ca.pem test-server.pem test-server.key
openssl verify -CAfile test-ca.pem "$WORK/server.crt"
```

Run: `Tools/testserver/certs/make_certs.sh`
Expected: the last line is `.../server.crt: OK`. `ls Tools/testserver/certs` shows `make_certs.sh test-ca.pem test-server.key test-server.pem`, and no key of the CA.

Create `Tools/testserver/certs/README.md` (ruling Q5):

```markdown
# Test certificates for Tools/testserver (test-only)

These files let the test server's TLS listener (`server.py --tls-port`) serve `https://` and `wss://` to the VaRest
automation tests (WebSocket spec 16.3). They protect nothing and must never be trusted outside these tests.

| File | What it is |
| --- | --- |
| `test-ca.pem` | A throwaway CA, `CN=VaRest Test CA (not trusted anywhere)`. Its private key was made in a temporary directory and destroyed right after it signed the leaf, so this CA can never issue anything else. |
| `test-server.pem` | The listener's certificate (SAN `DNS:localhost`, `IP:127.0.0.1`, `IP:::1`), followed by `test-ca.pem`: the chain the listener sends. |
| `test-server.key` | The listener's private key (RSA 2048, unencrypted). It is committed on purpose: a secret scanner flags it, and it is a test key that serves only `localhost`. |

All three are valid for 36500 days. To regenerate them (needs the `openssl` command line, 1.1.1 or later; the tests do
not): run `Tools/testserver/certs/make_certs.sh`, then commit the three files. Tests that verify the listener trust
`test-ca.pem` (`GET /tls/ca.pem` serves it); tests that only need TLS allow self-signed chains.
```

- [ ] **Step 4: Add the TLS listener to server.py**

In `Tools/testserver/server.py`:

After `import socket`, add (a Python built without `ssl` still serves plain HTTP, and `--tls-port` then says why it cannot, review M-11):

```python
try:
    import ssl
except ImportError:
    ssl = None
```

Below `HERE = ...` add:

```python
# WebSocket spec 16.3: the TLS listener's test certificate (certs/make_certs.sh; test-only, its CA's key is gone)
CERTS_DIR = os.path.join(HERE, "certs")
# A TLS client that never finishes its handshake is dropped after this, on its own thread (Handler.setup)
TLS_HANDSHAKE_TIMEOUT = 10.0
```

Below `def route_envelope_err(h, match): ...` add:

```python
def route_tls_ca(h, match):
    """The test CA (certs/test-ca.pem): a client that wants the TLS listener verified, not only allowed, trusts this one"""
    with open(os.path.join(CERTS_DIR, "test-ca.pem"), "rb") as stream:
        h.send_bytes(200, stream.read(), "application/x-pem-file")
```

In `ROUTES`, after the `/envelope/err` entry, add:

```python
    (("GET",), r"/tls/ca.pem", route_tls_ca),
```

In `class Handler`, add as its first method (after `server_version`):

```python
    def setup(self):
        # The TLS handshake runs here, on this connection's thread, not in the accept loop: a client that never finishes
        # it must not stall the others (TlsServer.get_request wraps without shaking hands)
        if ssl is not None and isinstance(self.request, ssl.SSLSocket):
            self.request.settimeout(TLS_HANDSHAKE_TIMEOUT)
            self.request.do_handshake()
            self.request.settimeout(None)
        super().setup()
```

In `class Server`, change `handle_error`'s check to also ignore TLS failures:

```python
    def handle_error(self, request, client_address):
        # A client that cancels, times out or drops a keep-alive connection resets it, and a TLS client may refuse our
        # test certificate or speak plain HTTP to the TLS port: test scenarios, not errors
        quiet = (ConnectionResetError, BrokenPipeError, TimeoutError, socket.timeout) + ((ssl.SSLError,) if ssl is not None else ())
        if isinstance(sys.exc_info()[1], quiet):
            return
        super().handle_error(request, client_address)
```

Below `class Server`, add:

```python
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
```

In `main()`, add the option after `--fixtures-dir`:

```python
    parser.add_argument("--tls-port", type=int, help="also serve every route with TLS on this port (0: the OS picks)")
```

and replace everything from `load_plugins()` to the end of `main()` with:

```python
    load_plugins()
    server = make_server(args.host, args.port)
    urls = ["http://%s:%d" % (args.host, server.server_address[1])]
    extras = []
    if args.tls_port is not None:
        try:
            extras.append(make_tls_server(args.host, args.tls_port))
        except OSError as error:
            # Review M-11: the runner only says "did not start -- see <log>"; this line names the file that failed
            # (ssl.SSLError is an OSError: a certificate that does not load lands here too)
            print("VaRestTestServer: cannot start the TLS listener: %s: %s" % (getattr(error, "filename", None) or CERTS_DIR, error), file=sys.stderr, flush=True)
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
```

In the module docstring, replace the usage line and the paragraph after it with:

```
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
```

and add to the route list, after the `/envelope/ok` line:

```
  GET  /tls/ca.pem          the test CA, for a client that verifies the TLS listener instead of allowing self-signed
```

- [ ] **Step 5: Run the selftests to see them pass**

Run: `python3 -I Tools/testserver/selftest.py -v && python3 -I Tools/testserver/ws_selftest.py -v`
Expected: every test passes, `TlsTest` and `WssTest` included.

Restore-the-bug: change `TlsServer.get_request` to `return self.tls_context.wrap_socket(connection, server_side=True), address`, which shakes hands in the accept loop. `test_a_stalled_or_plain_client_does_not_block_others` must fail: the stalled client holds the accept loop, so the next verified `get` times out after 10 s. The test's `finally` closes the stalled socket, which ends that handshake, so `tearDownClass` still returns. Restore `do_handshake_on_connect=False` and see the test pass. Report both outcomes.

- [ ] **Step 6: Pass the TLS URL to the editor**

In `Tools/run_tests.sh`, change the server start line to:

```bash
	python3 -I "$SERVER_DIR/server.py" --host 127.0.0.1 --port 0 --tls-port 0 --ready-file "$SERVER_URL_FILE" --parent-pid $$ \
		--fixtures-dir "$HOST/Saved/VaRestBench" >"$SERVER_LOG" 2>&1 &
```

and replace `SERVER_ARGS=("-VaRestTestServer=$(head -n 1 "$SERVER_URL_FILE")")` with:

```bash
	SERVER_ARGS=("-VaRestTestServer=$(head -n 1 "$SERVER_URL_FILE")")
	# WebSocket W0: the TLS listener's base URL is the ready file's second line (VaRest::Http::Testing::GetTestServerTlsUrl)
	TLS_URL="$(sed -n 2p "$SERVER_URL_FILE")"
	if [[ -n "$TLS_URL" ]]; then
		SERVER_ARGS+=("-VaRestTestServerTls=$TLS_URL")
	fi
```

In the header comment, replace the sentence `The runner starts Tools/testserver/server.py on a port the OS picks and passes its URL to the editor as -VaRestTestServer=<url> for the VaRest.Integration.* tests (M3 plan decision 14);` with:

```bash
# The runner starts Tools/testserver/server.py on ports the OS picks (plain and TLS) and passes its URLs to the editor as
# -VaRestTestServer=<url> and -VaRestTestServerTls=<url> for the VaRest.Integration.* and Probe.VaRest.* tests (M3 plan
# decision 14, WebSocket W0 plan decision 5);
```

- [ ] **Step 7: Check the runner end to end**

Run: `Tools/run_tests.sh VaRest.Integration`
Expected: the M3 integration suite passes as before (`run_tests: N passed, 0 failed`). `grep -c "VaRestTestServerTls=https://127.0.0.1:" /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints at least 1: the editor's command line echoes it into the log.

- [ ] **Step 8: Commit**

```bash
git add Tools/testserver/certs/make_certs.sh Tools/testserver/certs/README.md Tools/testserver/certs/test-ca.pem \
	Tools/testserver/certs/test-server.pem Tools/testserver/certs/test-server.key \
	Tools/testserver/server.py Tools/testserver/selftest.py Tools/testserver/ws_selftest.py Tools/run_tests.sh
git commit -m "testserver: TLS listener (WS W0) -- committed throwaway test CA and leaf, --tls-port, /tls/ca.pem, handshake off the accept loop; runner passes -VaRestTestServerTls

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 4: The probe rig — module wiring, `FRig`, facts, journal helpers

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestHttpTesting.h`, `Source/VaRestHttp/Private/VaRestHttpTesting.cpp` (`GetTestServerTlsUrl`)
- Modify: `Source/VaRestTests/VaRestTests.Build.cs`
- Modify: `Source/VaRestTests/Private/VaRestTestsModule.cpp` (a module class that samples `IsModuleLoaded("WebSockets")` at startup, review I-4)
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeLws.h`
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h`
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.cpp`
- Test: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRigTest.cpp`

**Interfaces:**
- Consumes: `VaRest::Http::Testing::PumpUntil(TFunctionRef<bool()>, double, bool bTickCoreTicker)`, `GetTestServerUrl()`, `FLogCapture`, `MakeOutcome<T>()`, `Observe(...)` (`VaRestHttpTesting.h`); `FVaRestClient::Make`, `.WithLogLevel(EVaRestLogLevel::None)`, `.Get<T>(Path)` (`VaRestClient.h`); `FVaRestJson` (`Object()`, `Set`, `ToString`, `Parse`, `operator[]`, `Elements()`, `AsString`, `AsInt`, `AsInt64`, `AsBool`, `Has`); `VaRest::Http::FSha256::Hash(...).ToHex()` (`VaRestSha256.h`); the Task 1–3 server and journal.
- Produces (every probe task uses these exact names; all in `namespace VaRest::LwsProbe`, inside `#if WITH_DEV_AUTOMATION_TESTS`, plus the log category):
  - `DECLARE_LOG_CATEGORY_EXTERN(LogVaRestLwsProbe, Log, All)`.
  - `enum EPseudoReason : int32 { ConnectCalling = -1, ConnectReturned = -2, CommandRan = -3, WriteDone = -4, CloseRequested = -5, HeadersAppended = -6, Marker = -7 }`.
  - `enum EEventFlags : uint32 { FirstFragment, FinalFragment, BinaryFrame, UserIsConnection, WsiUserIsConnection }` (bits 0–4).
  - `struct FEvent { int32 Conn; int32 Reason; double Time; int64 Len; uint32 Flags; int64 Remaining; TArray<uint8> Bytes; FString Note; }`.
  - `struct FServer { FString HttpBase; FString Host; int32 Port; int32 TlsPort; }`; `bool FindServer(FAutomationTestBase&, FServer&, bool bNeedTls = false)`.
  - `enum class EVerdict : uint8 { Agrees, Differs, Recorded, Unverified }`; `EVerdict AgreesIf(bool)`; `void Fact(FAutomationTestBase&, const FString& Id, EVerdict, const FString& Observed, const FString& Spec, const FString& Evidence)`; `void Pending(const FString& Id)`; `const TCHAR* YesNo(bool)`; `bool NoteOutcome(FAutomationTestBase&, const FString& What, bool bOutcome)` (a library outcome as an `AddInfo`, never a failure; returns `bOutcome`; review M-2 ruling).
  - `extern bool GWebSocketsLoadedAtModuleStartup` (defined in `VaRestTestsModule.cpp`, set in `StartupModule`).
  - `constexpr int32 KeepDefault`; `using FHook = TFunction<int32(FConn& Conn, lws* Wsi, int32 Reason, void* User, void* In, size_t Len)>`.
  - `struct FConnectSpec { FString Path = "/ws/echo"; bool bTls; FString Address; int32 Port; FString Host; TOptional<FString> Origin; FString Protocols; FString LocalProtocolName = "varest"; int32 SslFlags; TArray<FString> AppendHeaders; int32 PeerCloseReturn; FHook Hook; }`.
  - `struct FWrite { TArray<uint8> Buffer; int32 Protocol; }` (`Buffer` = `LWS_PRE` bytes of headroom + payload).
  - `struct FConn { FRig* Rig; int32 Id; lws* Wsi; bool bDestroyed; FConnectSpec Spec; TArray<ANSICHAR> PathUtf8, AddressUtf8, HostUtf8, OriginUtf8, ProtocolsUtf8, LocalNameUtf8; TArray<FWrite> Outbox; int32 CloseCode; TArray<uint8> CloseReason; TMap<FName, int64> Counters; }`.
  - `enum class ETls : uint8 { None, LwsGlobalInit, SslModule, ProvidedContext }`; `struct FRigOptions { int32 ServiceTimeoutMs = 10000; uint32 ContextOptions = PEER_CERT_NOT_REQUIRED | DISABLE_OS_CA_CERTS | JUST_USE_RAW_ORIGIN; ETls Tls = None; SSL_CTX* ProvidedSslContext; uint32 TimeoutSecs = 0; uint32 RxBufferSize = 65536; bool bCommandsInsideCallback = true; TFunction<void(SSL_CTX*)> OnLoadExtraClientVerifyCerts; TFunction<int32(FConn&, X509_STORE_CTX*, int64 PreverifyOk)> OnVerifyServerCert; }`.
  - `class FRig final : public FRunnable`: `FRig(const FServer&, FRigOptions = {})`, `~FRig()`, `bool IsReady() const`, `bool UsedSslModuleContext() const`, `const FServer& GetServer() const`, `void Shutdown()`; game thread: `int32 Connect(FConnectSpec)`, `void Exec(const TCHAR* Label, TFunction<void(FRig&)>)`, `void ExecOn(int32 ConnId, const TCHAR* Label, TFunction<void(FConn&)>)`, `void Write(int32 ConnId, int32 Protocol, TArray<uint8> Payload)`, `void WriteText(int32 ConnId, const FString& Text, int32 Protocol = LWS_WRITE_TEXT)`, `void Close(int32 ConnId, int32 Code, const FString& Reason)`, `bool WaitFor(TFunctionRef<bool(const TArray<FEvent>&)>, double Seconds = 15.0)`, `bool WaitReason(int32 ConnId, int32 Reason, double Seconds = 15.0)`, `bool WaitEnded(int32 ConnId, double Seconds = 15.0)`, `TArray<FEvent> Events(int32 ConnId = 0) const`, `bool Find(int32 ConnId, int32 Reason, FEvent& Out, const TCHAR* NotePrefix = nullptr) const`; any thread: `void Record(int32 ConnId, int32 Reason, int64 Len, const FString& Note)`; service thread: `int32 WriteNow(FConn&, lws* Wsi, int32 Protocol, TConstArrayView<uint8> Payload, const TCHAR* Label)`; `static int Callback(lws*, lws_callback_reasons, void*, void*, size_t)`. Members (MSVC C4458: no local may take these names): `Server, Options, Context, Protocols, Thread, bStopping, Commands, ConnsLock, Conns, EventsLock, Recorded, CopiedBytes, SslModuleContext, bSslInitialized, bSslModuleContext, NextId` and the functions above plus `Run, Stop, Enqueue, DrainCommands, Handle, ConnectNow, FindConn, AppendHeaders, WriteNext, Append, CopyBytes`.
  - Helpers (game thread): `FString NewKey()`, `FString Utf8Text(const void*, size_t)`, `FString EventText(const FEvent&)`, `TArray<uint8> Utf8Bytes(const FString&)`, `FString HexOf(TConstArrayView<uint8>)`, `FString ReasonName(int32)`, `FString CallSequence(const TArray<FEvent>&)`, `TArray<FEvent> OfReason(const TArray<FEvent>&, int32 Reason)`, `bool Opened(const TArray<FEvent>&)`, `FString ErrorOf(const TArray<FEvent>&)`, `FString ConnectResult(const TArray<FEvent>&)`, `bool Visit(FAutomationTestBase&, FRig&, FConnectSpec, TArray<FEvent>& Out, double Seconds = 20.0)` (a connection that never ends is noted, not failed), `bool WaitBytes(FRig&, int32 ConnId, int64 Bytes, double Seconds = 15.0)`, `FVaRestJson FetchJournal(FAutomationTestBase&, const FServer&, const FString& Key, int32 Connections = 1, int32 WaitMs = 5000)`, `TArray<FVaRestJson> JournalEvents(const FVaRestJson&, const TCHAR* Kind, int32 ConnIndex = 0)`, `FString JournalHeader(const FVaRestJson&, const TCHAR* Name, int32 ConnIndex = 1)`, `TArray<uint8> JournalBytes(const FVaRestJson& Event)`, `bool FetchBytes(FAutomationTestBase&, const FServer&, const FString& Path, TArray<uint8>& Out)`, `int32 AddPemToStore(SSL_CTX*, TConstArrayView<uint8> Pem)`, `FString UrlValue(const FString&)`, `bool IsMessageStart(int32 Protocol)`.
  - `VaRest::Http::Testing::GetTestServerTlsUrl()` (VaRestHttp).

- [ ] **Step 1: Write the failing rig tests and wire the module**

In `Source/VaRestTests/VaRestTests.Build.cs`, replace the constructor body with:

```csharp
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"VaRestJson",
			"VaRestHttp",
			"VaRest", // v1, for Bench.VaRest.Http.RequestPath (its HTTP and Json dependencies are public, VaRest.Build.cs:21-31)
			// WebSocket W0 probe: the SSL module's context and certificate manager, which the engine's transport uses
			// (WebSockets.Build.cs:118-119)
			"SSL",
		});

		// WebSocket W0 probe (Probe.VaRest.Lws.*): the engine's IWebSocket for the coexistence check, reached only through
		// FWebSocketsModule's virtuals. Include paths, no link: this module keeps its own static libwebsockets, as
		// VaRestWebSocket will (WebSocket spec 1.1: the engine module WebSockets is not a dependency).
		PrivateIncludePathModuleNames.Add("WebSockets");

		// WebSocket W0 probe: raw libwebsockets 3.0 as the engine bundles it, linked the way the engine's WebSockets module
		// links it (WebSockets.Build.cs:118; ModuleRules.cs:1601-1607 adds them as private dependencies). Test-only: this
		// module is UncookedOnly, never in a game target (ModuleDescriptor.cs:792-793, TargetRules.cs:1190-1193).
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib");
```

Replace `Source/VaRestTests/Private/VaRestTestsModule.cpp` with:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"

namespace VaRest::LwsProbe
{
	/**
	 * Whether the engine's WebSockets module was loaded when this module started, before any test ran (WS W0 review I-4):
	 * automation runs tests in name order, so Probe.VaRest.Lws.Coexist.* (which loads WebSockets) runs before
	 * Probe.VaRest.Lws.Service.Environment, which reports this value. WebSocket spec D30 branches on it.
	 */
	bool GWebSocketsLoadedAtModuleStartup = false;
}

class FVaRestTestsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		VaRest::LwsProbe::GWebSocketsLoadedAtModuleStartup = FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets"));
	}
};

IMPLEMENT_MODULE(FVaRestTestsModule, VaRestTests);
```

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRigTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// The rig's own checks (Probe.VaRest.Lws.Rig.*): before any fact is believed, the harness that records it must work

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigEchoTest, "Probe.VaRest.Lws.Rig.Echo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigEchoTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context and service thread exist"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/echo?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	Rig.WriteText(Conn, TEXT("hello"));
	TestTrue(TEXT("the echo arrives"), Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([Conn](const FEvent& Each)
		{
			return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("hello"), ESearchCase::CaseSensitive);
		});
	}));
	Rig.Close(Conn, 1000, TEXT("bye"));
	if (!TestTrue(TEXT("the connection ends"), Rig.WaitEnded(Conn)))
	{
		return false;
	}
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal has ended"), Journal[TEXT("ended")].AsBool()))
	{
		return false;
	}
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	TestTrue(TEXT("the server got hello"), Messages.Num() == 1 && Messages[0][TEXT("text")].AsString().Equals(TEXT("hello"), ESearchCase::CaseSensitive));
	const TArray<FVaRestJson> Closes = JournalEvents(Journal, TEXT("close"));
	TestTrue(TEXT("the server got our 1000 bye"), Closes.Num() == 1 && Closes[0][TEXT("code")].AsInt() == 1000
		&& Closes[0][TEXT("reason")].AsString().Equals(TEXT("bye"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigFactLineTest, "Probe.VaRest.Lws.Rig.FactLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigFactLineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The contract with Tools/lws_probe_passport.py: one JSON object per FACT line, whatever the observed text holds
	VaRest::Http::Testing::FLogCapture Capture;
	Fact(*this, TEXT("rig.fact_line"), EVerdict::Recorded, TEXT("a \"quoted\" | piped value"), TEXT("none"), TEXT("the rig's own check"));
	const TArray<FString> Lines = Capture.GetLines();
	const FString* Line = Lines.FindByPredicate([](const FString& Each) { return Each.StartsWith(TEXT("LogVaRestLwsProbe: FACT {")); });
	if (!TestNotNull(TEXT("one FACT line"), Line))
	{
		return false;
	}
	FString Json;
	Line->Split(TEXT("FACT "), nullptr, &Json);
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Json));
	if (!TestTrue(TEXT("the line is JSON"), Parsed.HasValue()))
	{
		return false;
	}
	const FVaRestJson& Value = Parsed.GetValue();
	TestTrue(TEXT("id, verdict and observed survive"), Value[TEXT("id")].AsString().Equals(TEXT("rig.fact_line"), ESearchCase::CaseSensitive)
		&& Value[TEXT("verdict")].AsString().Equals(TEXT("recorded"), ESearchCase::CaseSensitive)
		&& Value[TEXT("observed")].AsString().Equals(TEXT("a \"quoted\" | piped value"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("the test is named"), Value[TEXT("test")].AsString().Contains(TEXT("Probe.VaRest.Lws.Rig.FactLine")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigSequentialTest, "Probe.VaRest.Lws.Rig.SequentialConnections",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigSequentialTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// Review focus: LWS may hand a freed wsi's address to the next connection; the rig matches the userdata first
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	TArray<int32> Ids;
	for (int32 Round = 0; Round < 5; ++Round)
	{
		FConnectSpec Spec;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Ids.Add(Conn);
		if (!TestTrue(FString::Printf(TEXT("round %d opens"), Round), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
		{
			return false;
		}
		const FString Hello = FString::Printf(TEXT("round %d"), Round);
		Rig.WriteText(Conn, Hello);
		TestTrue(FString::Printf(TEXT("round %d echoes"), Round), Rig.WaitFor([Conn, Hello](const TArray<FEvent>& All)
		{
			return All.ContainsByPredicate([Conn, &Hello](const FEvent& Each)
			{
				return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(Hello, ESearchCase::CaseSensitive);
			});
		}));
		Rig.Close(Conn, 1000, TEXT("next"));
		TestTrue(FString::Printf(TEXT("round %d ends"), Round), Rig.WaitEnded(Conn));
	}
	const TArray<FEvent> Everything = Rig.Events();
	for (const int32 Id : Ids)
	{
		const int32 Opens = Everything.FilterByPredicate([Id](const FEvent& Each) { return Each.Conn == Id && Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; }).Num();
		const int32 Ends = Everything.FilterByPredicate([Id](const FEvent& Each) { return Each.Conn == Id && Each.Reason == LWS_CALLBACK_WSI_DESTROY; }).Num();
		TestTrue(FString::Printf(TEXT("connection %d: one ESTABLISHED and one WSI_DESTROY (saw %d and %d)"), Id, Opens, Ends), Opens == 1 && Ends == 1);
	}
	TestFalse(TEXT("no open or receive is left without its connection"), Everything.ContainsByPredicate([](const FEvent& Each)
	{
		return Each.Conn == 0 && (Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRigTeardownTest, "Probe.VaRest.Lws.Rig.TeardownWithOpenConnection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRigTeardownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// Review focus: a probe whose connection never ends still gets its rig torn down, promptly
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/silent?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// The service thread parks in its 10 s wait (FRigOptions::ServiceTimeoutMs) before the teardown starts
	FPlatformProcess::Sleep(0.2f);
	const double Before = FPlatformTime::Seconds();
	Rig.Shutdown();
	const double Took = FPlatformTime::Seconds() - Before;
	TestTrue(FString::Printf(TEXT("the teardown returns within 5 s with a connection open (took %.2f s)"), Took), Took < 5.0);
	FEvent Destroyed;
	TestTrue(TEXT("the open connection ended inside the teardown"), Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed) && Destroyed.Time >= Before);
	TestTrue(TEXT("the server saw the connection end"), FetchJournal(*this, Site, Key)[TEXT("ended")].AsBool());
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Rig`
Expected: `run_tests: build failed`, with `fatal error: 'VaRestLwsProbe.h' file not found` in the build output.

- [ ] **Step 3: Add the TLS URL helper to VaRestHttpTesting**

In `Source/VaRestHttp/Public/VaRestHttpTesting.h`, below `VARESTHTTP_API FString GetTestServerUrl();`, add:

```cpp
	/** -VaRestTestServerTls=<url>, the test server's TLS listener (WebSocket W0), which Tools/run_tests.sh passes; empty when absent */
	VARESTHTTP_API FString GetTestServerTlsUrl();
```

In `Source/VaRestHttp/Private/VaRestHttpTesting.cpp`, below `GetTestServerUrl()`'s definition, add:

```cpp
FString VaRest::Http::Testing::GetTestServerTlsUrl()
{
	// "VaRestTestServerTls=" and "VaRestTestServer=" are not substrings of each other, so neither lookup sees the other
	FString Url;
	FParse::Value(FCommandLine::Get(), TEXT("VaRestTestServerTls="), Url);
	return Url;
}
```

- [ ] **Step 4: Write the LWS include header**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeLws.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// The one place the probe includes the engine-bundled libwebsockets 3.0 and its OpenSSL, the way the engine's WebSockets
// module does (Runtime/Online/WebSockets/Private/Lws/LwsWebSocketsManager.h:14-24): Windows types allowed around the
// headers (libwebsockets.h:52-61 pulls in winsock2.h there), inside the third-party warning scope. libwebsockets.h
// includes openssl/ssl.h itself (libwebsockets.h:221); the others are for the probe's own TLS checks.

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

THIRD_PARTY_INCLUDES_START
#include "libwebsockets.h"
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
THIRD_PARTY_INCLUDES_END

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
```

- [ ] **Step 5: Write the rig's header**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// WebSocket spec 16.4 (W0): a minimal harness that drives the engine-bundled libwebsockets 3.0 directly -- no VaRest
// client code -- against Tools/testserver/ws.py, and logs what it sees as FACT lines for Tools/lws_probe_passport.py.
// One FRig per test: one lws_context with one protocol "varest", one service thread that waits in lws_service and is
// woken by lws_cancel_service, game-thread commands run inside LWS_CALLBACK_EVENT_WAIT_CANCELLED (the shape of WebSocket
// spec 2.1), and every callback recorded in order. Test-only: VaRestTests is UncookedOnly, never in a game target.
//
// One constraint for every test (WS W0 decision 16, review I-3): never create an FRig while another FRig has a TLS
// connection that has neither opened nor ended. Each lws_create_context re-allocates this LWS copy's process-global
// openssl_websocket_private_data_index (ssl.c.o lws_context_init_ssl_library, stores at 0x1f4 and 0x23a), and the
// client verify callback dereferences what SSL_get_ex_data returns under it without a null check (openssl-client.c.o
// 0x1c8-0x1d6), so a handshake in flight across a context creation crashes.

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "VaRestJson.h"
#include "VaRestLwsProbeLws.h"

#include <atomic>

class FAutomationTestBase;
class FRunnableThread;

DECLARE_LOG_CATEGORY_EXTERN(LogVaRestLwsProbe, Log, All);

#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::LwsProbe
{
	/** Rig actions recorded next to LWS's callbacks: negative, so never an lws_callback_reasons value (0-75, 1000) */
	enum EPseudoReason : int32
	{
		ConnectCalling = -1,  // just before lws_client_connect_via_info
		ConnectReturned = -2, // Note: "wsi" or "null"
		CommandRan = -3,      // Note: the command's label, or "<label>: no wsi" when its connection was gone
		WriteDone = -4,       // Len: payload bytes; Note: "<label> ret=<lws_write> partial=<lws_partial_buffered>"
		CloseRequested = -5,  // Note: "code=<c>": lws_close_reason, then -1 from WRITEABLE
		HeadersAppended = -6, // Len: bytes written in APPEND_HANDSHAKE_HEADER; Note: "all" or "stopped: no space"
		Marker = -7,          // a hook's own note: Len and Note are the hook's
	};

	enum EEventFlags : uint32
	{
		FirstFragment = 1u << 0,       // lws_is_first_fragment, on CLIENT_RECEIVE
		FinalFragment = 1u << 1,       // lws_is_final_fragment
		BinaryFrame = 1u << 2,         // lws_frame_is_binary
		UserIsConnection = 1u << 3,    // the callback's user argument was this connection's userdata
		WsiUserIsConnection = 1u << 4, // lws_wsi_user(wsi) was this connection's userdata
	};

	/** One callback, or one rig action, in service-thread order */
	struct FEvent
	{
		int32 Conn = 0;       // the rig's connection id; 0 when the callback belongs to no connection
		int32 Reason = 0;     // lws_callback_reasons or EPseudoReason
		double Time = 0.0;    // FPlatformTime::Seconds() when recorded
		int64 Len = 0;        // the callback's len, or the pseudo event's number
		uint32 Flags = 0;     // EEventFlags
		int64 Remaining = -1; // lws_remaining_packet_payload, on CLIENT_RECEIVE
		TArray<uint8> Bytes;  // a copy of in for reasons that carry bytes (at most 64 KiB each, 64 MiB per rig)
		FString Note;
	};

	/** The runner's test server: -VaRestTestServer=http://host:port and -VaRestTestServerTls=https://host:port */
	struct FServer
	{
		FString HttpBase;
		FString Host;
		int32 Port = 0;
		int32 TlsPort = 0; // 0: no TLS listener
	};

	/** AddError and false when the runner did not pass the server (or its TLS listener, when bNeedTls) */
	bool FindServer(FAutomationTestBase& Test, FServer& Out, bool bNeedTls = false);

	/** A fact's verdict: what the spec says (or inferred) against what LWS 3.0 did; Recorded: a measurement the spec does not claim */
	enum class EVerdict : uint8
	{
		Agrees,
		Differs,
		Recorded,
		Unverified,
	};

	inline EVerdict AgreesIf(bool bAgrees)
	{
		return bAgrees ? EVerdict::Agrees : EVerdict::Differs;
	}

	inline const TCHAR* YesNo(bool bValue)
	{
		return bValue ? TEXT("yes") : TEXT("no");
	}

	/** One probe result: a FACT line (JSON) in the log for the passport script, and an AddInfo in the test's report */
	void Fact(FAutomationTestBase& Test, const FString& Id, EVerdict Verdict, const FString& Observed, const FString& Spec, const FString& Evidence);

	/** Logged and flushed right before a call that may end the process: an id with PENDING and no FACT is "crashed" */
	void Pending(const FString& Id);

	/**
	 * A library outcome (opened, ended, echoed...), recorded as an AddInfo and never a failure: the facts carry it
	 * (WS W0 decision 3, review M-2 ruling). Returns bOutcome, so the caller can still branch on it.
	 */
	bool NoteOutcome(FAutomationTestBase& Test, const FString& What, bool bOutcome);

	/** Set by FVaRestTestsModule::StartupModule (VaRestTestsModule.cpp) */
	extern bool GWebSocketsLoadedAtModuleStartup;

	struct FConn;
	class FRig;

	/** A hook returns this to let the rig's default handling of the callback run */
	constexpr int32 KeepDefault = MIN_int32;

	/** Service thread, after the rig recorded the callback; any other return value is the callback's */
	using FHook = TFunction<int32(FConn& Conn, lws* Wsi, int32 Reason, void* User, void* In, size_t Len)>;

	/** What one connection does; set on the game thread before Connect, read on the service thread */
	struct FConnectSpec
	{
		FString Path = TEXT("/ws/echo");   // request target, query included
		bool bTls = false;                  // LCCSCF_USE_SSL, to the server's TLS port unless Port says otherwise
		FString Address;                    // where TCP goes; empty: the server's host
		int32 Port = 0;                     // 0: the server's port (its TLS port when bTls)
		FString Host;                       // the Host header (and the name TLS checks); empty: "<Address>:<Port>"
		TOptional<FString> Origin;          // unset: no origin
		FString Protocols;                  // the offered subprotocols, as LWS takes them (comma separated); empty: none
		FString LocalProtocolName = TEXT("varest"); // local_protocol_name (libwebsockets.h:3455-3461); empty: NULL
		int32 SslFlags = 0;                 // LCCSCF_* added to LCCSCF_USE_SSL
		TArray<FString> AppendHeaders;      // "Name: value" lines, CRLF added, written in APPEND_HANDSHAKE_HEADER when they fit
		int32 PeerCloseReturn = 0;          // WS_PEER_INITIATED_CLOSE's return: 0 lets LWS echo (libwebsockets.h:1217-1225)
		FHook Hook;
	};

	/** One queued lws_write: LWS_PRE bytes of headroom, then the payload (libwebsockets.h:4846-4877) */
	struct FWrite
	{
		TArray<uint8> Buffer;
		int32 Protocol = 0; // lws_write_protocol, with LWS_WRITE_NO_FIN when it is not a message's last fragment
	};

	/** One connection's state; the service thread owns everything but Rig, Id and Spec */
	struct FConn
	{
		FRig* Rig = nullptr;
		int32 Id = 0;
		lws* Wsi = nullptr;   // from connect_info.pwsi, cleared at WSI_DESTROY
		bool bDestroyed = false;
		FConnectSpec Spec;
		/** UTF-8 copies of the strings handed to lws_client_connect_via_info, alive as long as the connection */
		TArray<ANSICHAR> PathUtf8, AddressUtf8, HostUtf8, OriginUtf8, ProtocolsUtf8, LocalNameUtf8;
		TArray<FWrite> Outbox;
		int32 CloseCode = 0;
		TArray<uint8> CloseReason;
		/** Scratch for hooks */
		TMap<FName, int64> Counters;
	};

	enum class ETls : uint8
	{
		None,            // no TLS in the context
		LwsGlobalInit,   // LWS initialises OpenSSL and makes its own SSL_CTX (LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT)
		SslModule,       // the engine's order: the SSL module's SSL_CTX when InitializeSsl succeeds, else LwsGlobalInit (LwsWebSocketsManager.cpp:205-225)
		ProvidedContext, // FRigOptions::ProvidedSslContext as provided_client_ssl_ctx; the caller frees it after the rig
	};

	struct FRigOptions
	{
		/** lws_service's wait; the production shape is a long wait woken by lws_cancel_service (WebSocket spec 2.1) */
		int32 ServiceTimeoutMs = 10000;
		/** The engine's options (LwsWebSocketsManager.cpp:184) plus JUST_USE_RAW_ORIGIN (WebSocket spec 2.1) */
		uint32 ContextOptions = LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED | LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
		ETls Tls = ETls::None;
		SSL_CTX* ProvidedSslContext = nullptr;
		/** lws_context_creation_info::timeout_secs (libwebsockets.h:2791-2795); 0: LWS's default */
		uint32 TimeoutSecs = 0;
		/** The one protocol's rx_buffer_size; 64 KiB as the engine's (LwsWebSocketsManager.cpp:166) */
		uint32 RxBufferSize = 65536;
		/** true: commands run inside LWS_CALLBACK_EVENT_WAIT_CANCELLED (WebSocket spec 2.1); false: between lws_service calls */
		bool bCommandsInsideCallback = true;
		/** Service thread (or inside lws_create_context): LOAD_EXTRA_CLIENT_VERIFY_CERTS, whose user is the SSL_CTX* (libwebsockets.h:961-967) */
		TFunction<void(SSL_CTX* Ctx)> OnLoadExtraClientVerifyCerts;
		/** Service thread: PERFORM_SERVER_CERT_VERIFICATION; its return is the callback's (0 keeps OpenSSL's verdict, libwebsockets.h:1015-1044) */
		TFunction<int32(FConn& Conn, X509_STORE_CTX* Store, int64 PreverifyOk)> OnVerifyServerCert;
	};

	class FRig final : public FRunnable
	{
	public:
		/** Game thread: creates the context and starts the service thread; IsReady() says whether both worked */
		explicit FRig(const FServer& InServer, FRigOptions InOptions = FRigOptions());
		virtual ~FRig() override;
		FRig(const FRig&) = delete;
		FRig& operator=(const FRig&) = delete;

		bool IsReady() const { return Context != nullptr && Thread != nullptr; }
		bool UsedSslModuleContext() const { return bSslModuleContext; }
		const FServer& GetServer() const { return Server; }

		/** Game thread: stops the service thread, then destroys the context (open connections end inside); the events stay readable */
		void Shutdown();

		/** Game thread: a new connection, connected on the service thread; returns its id (> 0) */
		int32 Connect(FConnectSpec InSpec);
		/** Game thread: Command runs on the service thread, recorded as CommandRan with Label */
		void Exec(const TCHAR* Label, TFunction<void(FRig&)> Command);
		/** ... with the connection; recorded as "<Label>: no wsi" (and not run) when the connection is gone */
		void ExecOn(int32 ConnId, const TCHAR* Label, TFunction<void(FConn&)> Command);
		/** Queues one lws_write; WRITEABLE writes one per callback, as WebSocket spec 2.4 plans */
		void Write(int32 ConnId, int32 Protocol, TArray<uint8> Payload);
		void WriteText(int32 ConnId, const FString& Text, int32 Protocol = LWS_WRITE_TEXT);
		/** The engine's close: lws_close_reason, then -1 from WRITEABLE once the outbox is empty (LwsWebSocket.cpp:515-530) */
		void Close(int32 ConnId, int32 Code, const FString& Reason);

		/** Game thread: pumps (game-thread tasks and the core ticker) until Done holds over the events, or Seconds pass */
		bool WaitFor(TFunctionRef<bool(const TArray<FEvent>&)> Done, double Seconds = 15.0);
		bool WaitReason(int32 ConnId, int32 Reason, double Seconds = 15.0);
		/** WSI_DESTROY, or a connect that returned null */
		bool WaitEnded(int32 ConnId, double Seconds = 15.0);
		/** A copy of the events; 0: all of them */
		TArray<FEvent> Events(int32 ConnId = 0) const;
		/** The first event of ConnId with Reason (and a Note starting with NotePrefix) */
		bool Find(int32 ConnId, int32 Reason, FEvent& Out, const TCHAR* NotePrefix = nullptr) const;

		/** Any thread: a pseudo event */
		void Record(int32 ConnId, int32 Reason, int64 Len, const FString& Note);
		/** Service thread, inside a callback of Wsi: one lws_write now, recorded as WriteDone with Label */
		int32 WriteNow(FConn& Conn, lws* Wsi, int32 Protocol, TConstArrayView<uint8> Payload, const TCHAR* Label);

		/** The protocol's callback (lws_protocols::callback) */
		static int Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len);

	private:
		virtual uint32 Run() override;
		virtual void Stop() override;
		void Enqueue(TFunction<void()> Command);
		void DrainCommands();
		int32 Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len);
		void ConnectNow(FConn& Conn);
		FConn* FindConn(lws* Wsi, void* User);
		int32 AppendHeaders(FConn& Conn, void* In, size_t Len);
		int32 WriteNext(FConn& Conn, lws* Wsi);
		void Append(FEvent&& Event);
		void CopyBytes(FEvent& Event, const void* In, size_t Len);

		FServer Server;
		FRigOptions Options;
		lws_context* Context = nullptr;
		lws_protocols Protocols[2] = {};
		FRunnableThread* Thread = nullptr;
		std::atomic<bool> bStopping{ false };
		TQueue<TFunction<void()>, EQueueMode::Mpsc> Commands;
		mutable FCriticalSection ConnsLock;
		TArray<TUniquePtr<FConn>> Conns;
		mutable FCriticalSection EventsLock;
		TArray<FEvent> Recorded;
		int64 CopiedBytes = 0;
		SSL_CTX* SslModuleContext = nullptr;
		bool bSslInitialized = false;
		bool bSslModuleContext = false;
		std::atomic<int32> NextId{ 1 };
	};

	// ---- helpers (game thread)

	FString NewKey();
	FString Utf8Text(const void* Data, size_t Num);
	FString EventText(const FEvent& Event);
	TArray<uint8> Utf8Bytes(const FString& Source);
	FString HexOf(TConstArrayView<uint8> Bytes);
	FString ReasonName(int32 Reason);
	/** Reason names joined with " > ", runs collapsed ("RECEIVE x4"); rig bookkeeping left out */
	FString CallSequence(const TArray<FEvent>& Events);
	TArray<FEvent> OfReason(const TArray<FEvent>& Events, int32 Reason);
	bool Opened(const TArray<FEvent>& Events);
	/** CONNECTION_ERROR's string, or "(no CONNECTION_ERROR: <sequence>)" */
	FString ErrorOf(const TArray<FEvent>& Events);
	/** What lws_client_connect_via_info returned: "wsi", "null" or "(not called)" */
	FString ConnectResult(const TArray<FEvent>& Events);
	/** Connects, waits for the open or the end, closes 1000 if it opened, waits for the end; a connection that never ends is noted (NoteOutcome), and false returned */
	bool Visit(FAutomationTestBase& Test, FRig& Rig, FConnectSpec Spec, TArray<FEvent>& Out, double Seconds = 20.0);
	/** Until the connection's CLIENT_RECEIVE lengths add up to Bytes */
	bool WaitBytes(FRig& Rig, int32 ConnId, int64 Bytes, double Seconds = 15.0);
	/** GET /ws/journal over HTTP (FVaRestClient); an invalid handle and an AddError on failure */
	FVaRestJson FetchJournal(FAutomationTestBase& Test, const FServer& Site, const FString& Key, int32 Connections = 1, int32 WaitMs = 5000);
	/** The journal's events of one kind ("ev"), of one connection (1-based) or all (0) */
	TArray<FVaRestJson> JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex = 0);
	/** A request header the server received (name compared case-insensitively), or "(absent)" */
	FString JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex = 1);
	/** An event's "b64" payload */
	TArray<uint8> JournalBytes(const FVaRestJson& Event);
	bool FetchBytes(FAutomationTestBase& Test, const FServer& Site, const FString& Path, TArray<uint8>& Out);
	/** Adds every PEM certificate in Pem to the context's trust store; the number added */
	int32 AddPemToStore(SSL_CTX* Ctx, TConstArrayView<uint8> Pem);
	/** Percent-encodes a query value */
	FString UrlValue(const FString& Value);
	/** LWS_WRITE_TEXT or LWS_WRITE_BINARY, without the flags: the first frame of a message */
	bool IsMessageStart(int32 Protocol);
}
#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 6: Write the rig**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsProbe.h"

DEFINE_LOG_CATEGORY(LogVaRestLwsProbe);

#if WITH_DEV_AUTOMATION_TESTS

#include "GenericPlatform/GenericPlatformHttp.h"
#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/Guid.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "Ssl.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"

namespace VaRest::LwsProbe::VaRestLwsProbePrivate
{
	/** "http://127.0.0.1:41234" -> host and port */
	bool SplitUrl(const FString& Url, const TCHAR* Scheme, FString& OutHost, int32& OutPort)
	{
		const FString Prefix = FString(Scheme) + TEXT("://");
		if (!Url.StartsWith(Prefix, ESearchCase::IgnoreCase))
		{
			return false;
		}
		FString Authority = Url.Mid(Prefix.Len());
		int32 Slash = INDEX_NONE;
		if (Authority.FindChar(TEXT('/'), Slash))
		{
			Authority.LeftInline(Slash);
		}
		FString PortText;
		if (!Authority.Split(TEXT(":"), &OutHost, &PortText, ESearchCase::CaseSensitive, ESearchDir::FromEnd) || !PortText.IsNumeric())
		{
			return false;
		}
		OutPort = FCString::Atoi(*PortText);
		return !OutHost.IsEmpty() && OutPort > 0 && OutPort < 65536;
	}

	/** In the order of EVerdict */
	const TCHAR* const VerdictNames[] = { TEXT("agrees"), TEXT("differs"), TEXT("recorded"), TEXT("unverified") };

	/** Copies kept of callback bytes: enough for every probe's evidence, bounded against a flood */
	constexpr int64 MaxBytesPerEvent = 64 * 1024;
	constexpr int64 MaxBytesPerRig = 64 * 1024 * 1024;

	/** A UTF-8 copy for lws_client_connect_via_info, kept in Out */
	const char* Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out)
	{
		const FTCHARToUTF8 Converted(*Text);
		Out.SetNumUninitialized(Converted.Length() + 1);
		FMemory::Memcpy(Out.GetData(), Converted.Get(), Converted.Length());
		Out[Converted.Length()] = '\0';
		return Out.GetData();
	}
}

bool VaRest::LwsProbe::FindServer(FAutomationTestBase& Test, FServer& Out, bool bNeedTls)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	Out.HttpBase = VaRest::Http::Testing::GetTestServerUrl();
	if (Out.HttpBase.IsEmpty() || !SplitUrl(Out.HttpBase, TEXT("http"), Out.Host, Out.Port))
	{
		Test.AddError(TEXT("no test server: run Tools/run_tests.sh Probe.VaRest.Lws, which starts Tools/testserver/server.py and passes -VaRestTestServer=<url> and -VaRestTestServerTls=<url>"));
		return false;
	}
	FString TlsHost;
	if (!SplitUrl(VaRest::Http::Testing::GetTestServerTlsUrl(), TEXT("https"), TlsHost, Out.TlsPort))
	{
		Out.TlsPort = 0;
	}
	if (bNeedTls && Out.TlsPort == 0)
	{
		Test.AddError(TEXT("no TLS listener: -VaRestTestServerTls=<url> is missing (Tools/run_tests.sh passes it, Tools/testserver/server.py --tls-port)"));
		return false;
	}
	return true;
}

void VaRest::LwsProbe::Fact(FAutomationTestBase& Test, const FString& Id, EVerdict Verdict, const FString& Observed, const FString& Spec, const FString& Evidence)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	const FString Name = VerdictNames[static_cast<int32>(Verdict)];
	const FString Line = FVaRestJson::Object()
		.Set(TEXT("id"), Id)
		.Set(TEXT("verdict"), Name)
		.Set(TEXT("observed"), Observed)
		.Set(TEXT("spec"), Spec)
		.Set(TEXT("evidence"), Evidence)
		.Set(TEXT("test"), Test.GetTestFullName())
		.ToString();
	// Display: in the run's log file at the default verbosity, where Tools/lws_probe_passport.py reads it
	UE_LOG(LogVaRestLwsProbe, Display, TEXT("FACT %s"), *Line);
	Test.AddInfo(FString::Printf(TEXT("%s [%s]: %s"), *Id, *Name, *Observed));
}

void VaRest::LwsProbe::Pending(const FString& Id)
{
	UE_LOG(LogVaRestLwsProbe, Display, TEXT("PENDING %s"), *FVaRestJson::Object().Set(TEXT("id"), Id).ToString());
	// The next call may be the last thing the process does: this line must reach the file first
	GLog->Flush();
}

bool VaRest::LwsProbe::NoteOutcome(FAutomationTestBase& Test, const FString& What, bool bOutcome)
{
	Test.AddInfo(FString::Printf(TEXT("outcome: %s: %s"), *What, YesNo(bOutcome)));
	return bOutcome;
}

// ---- FRig

VaRest::LwsProbe::FRig::FRig(const FServer& InServer, FRigOptions InOptions)
	: Server(InServer)
	, Options(MoveTemp(InOptions))
{
	check(IsInGameThread());
	// One protocol, "varest" (WebSocket spec 2.1). per_session_data_size 0: the connection's userdata is ours
	// (connect_info.userdata), as the engine's (LwsWebSocketsManager.cpp:163-166, LwsWebSocket.cpp:807)
	Protocols[0].name = "varest";
	Protocols[0].callback = &FRig::Callback;
	Protocols[0].per_session_data_size = 0;
	Protocols[0].rx_buffer_size = Options.RxBufferSize;

	lws_context_creation_info Info = {};
	Info.port = CONTEXT_PORT_NO_LISTEN;
	Info.protocols = Protocols;
	Info.uid = -1;
	Info.gid = -1;
	Info.options = Options.ContextOptions;
	// The engine's 32 KiB (LwsWebSocketsManager.cpp:187-190): the handshake header buffer (WebSocket spec 15)
	Info.max_http_header_data2 = 32 * 1024;
	Info.pt_serv_buf_size = 32 * 1024;
	// WebSocket spec 2.1: our ping is per connection, LWS's own is off
	Info.ws_ping_pong_interval = 0;
	Info.timeout_secs = Options.TimeoutSecs;
	// Callback() finds the rig through lws_context_user(lws_get_context(wsi))
	Info.user = this;
	switch (Options.Tls)
	{
	case ETls::None:
		break;
	case ETls::LwsGlobalInit:
		Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		break;
	case ETls::SslModule:
	{
		// The engine's order (LwsWebSocketsManager.cpp:205-225). InitializeSsl succeeds only in monolithic builds
		// (SslManager.cpp:26-55); elsewhere CreateSslContext returns null (:76-131) and LWS makes its own context
		ISslManager& SslManager = FModuleManager::LoadModuleChecked<FSslModule>(TEXT("SSL")).GetSslManager();
		if (SslManager.InitializeSsl())
		{
			bSslInitialized = true;
			SslModuleContext = SslManager.CreateSslContext(FSslContextCreateOptions());
		}
		if (SslModuleContext)
		{
			bSslModuleContext = true;
			Info.provided_client_ssl_ctx = SslModuleContext;
		}
		else
		{
			Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
		}
		break;
	}
	case ETls::ProvidedContext:
		// "Libwebsockets no longer is responsible for freeing the context" (libwebsockets.h:2765-2769)
		Info.provided_client_ssl_ctx = Options.ProvidedSslContext;
		break;
	}
	Context = lws_create_context(&Info);
	if (!Context)
	{
		return;
	}
	// 128 KiB, below normal: the engine's service thread (LwsWebSocketsManager.cpp:239-247)
	Thread = FRunnableThread::Create(this, TEXT("VaRestLwsProbe"), 128 * 1024, TPri_BelowNormal);
}

VaRest::LwsProbe::FRig::~FRig()
{
	Shutdown();
}

void VaRest::LwsProbe::FRig::Shutdown()
{
	check(IsInGameThread());
	if (Thread)
	{
		// Kill(true) calls Stop() and waits for Run() to return: the engine's order (LwsWebSocketsManager.cpp:298-303)
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	if (Context)
	{
		// After the thread, on the game thread (LwsWebSocketsManager.cpp:305-309); the callbacks of connections still open
		// (CLOSED, WSI_DESTROY) arrive inside this call and are recorded like any other
		lws_context_destroy(Context);
		Context = nullptr;
	}
	if (SslModuleContext)
	{
		FModuleManager::GetModuleChecked<FSslModule>(TEXT("SSL")).GetSslManager().DestroySslContext(SslModuleContext);
		SslModuleContext = nullptr;
	}
	if (bSslInitialized)
	{
		// One ShutdownSsl per successful InitializeSsl (ISslManager.h:58-69)
		FModuleManager::GetModuleChecked<FSslModule>(TEXT("SSL")).GetSslManager().ShutdownSsl();
		bSslInitialized = false;
	}
}

uint32 VaRest::LwsProbe::FRig::Run()
{
	while (!bStopping.load())
	{
		if (!Options.bCommandsInsideCallback)
		{
			// The engine's place: before lws_service, on the same thread (LwsWebSocketsManager.cpp:485-509)
			DrainCommands();
		}
		lws_service(Context, Options.ServiceTimeoutMs);
	}
	return 0;
}

void VaRest::LwsProbe::FRig::Stop()
{
	bStopping = true;
	// The one LWS call another thread may make (libwebsockets.h:3704-3716): it ends the service thread's wait
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::LwsProbe::FRig::Enqueue(TFunction<void()> Command)
{
	Commands.Enqueue(MoveTemp(Command));
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::LwsProbe::FRig::DrainCommands()
{
	TFunction<void()> Command;
	while (Commands.Dequeue(Command))
	{
		Command();
	}
}

int32 VaRest::LwsProbe::FRig::Connect(FConnectSpec InSpec)
{
	check(IsInGameThread());
	TUniquePtr<FConn> Made = MakeUnique<FConn>();
	Made->Rig = this;
	Made->Id = NextId++;
	Made->Spec = MoveTemp(InSpec);
	FConn* Raw = Made.Get();
	{
		FScopeLock Lock(&ConnsLock);
		// Never removed before the rig dies: commands and callbacks may keep Raw
		Conns.Add(MoveTemp(Made));
	}
	Enqueue([this, Raw] { ConnectNow(*Raw); });
	return Raw->Id;
}

void VaRest::LwsProbe::FRig::ConnectNow(FConn& Conn)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	const FConnectSpec& Spec = Conn.Spec;
	const FString Address = Spec.Address.IsEmpty() ? Server.Host : Spec.Address;
	const int32 Port = Spec.Port != 0 ? Spec.Port : (Spec.bTls ? Server.TlsPort : Server.Port);
	const FString Host = Spec.Host.IsEmpty() ? FString::Printf(TEXT("%s:%d"), *Address, Port) : Spec.Host;

	lws_client_connect_info Info = {};
	Info.context = Context;
	Info.address = Utf8Copy(Address, Conn.AddressUtf8);
	Info.port = Port;
	Info.ssl_connection = Spec.bTls ? (LCCSCF_USE_SSL | Spec.SslFlags) : Spec.SslFlags;
	Info.path = Utf8Copy(Spec.Path, Conn.PathUtf8);
	Info.host = Utf8Copy(Host, Conn.HostUtf8);
	Info.origin = Spec.Origin.IsSet() ? Utf8Copy(Spec.Origin.GetValue(), Conn.OriginUtf8) : nullptr;
	Info.protocol = Spec.Protocols.IsEmpty() ? nullptr : Utf8Copy(Spec.Protocols, Conn.ProtocolsUtf8);
	Info.local_protocol_name = Spec.LocalProtocolName.IsEmpty() ? nullptr : Utf8Copy(Spec.LocalProtocolName, Conn.LocalNameUtf8);
	Info.ietf_version_or_minus_one = -1;
	Info.userdata = &Conn;
	// The wsi early, so a command (a kill) can reach a connection that has not opened yet (libwebsockets.h:3441-3451)
	Info.pwsi = &Conn.Wsi;
	Record(Conn.Id, ConnectCalling, 0, Host);
	lws* Made = lws_client_connect_via_info(&Info);
	Record(Conn.Id, ConnectReturned, 0, Made ? TEXT("wsi") : TEXT("null"));
}

void VaRest::LwsProbe::FRig::Exec(const TCHAR* Label, TFunction<void(FRig&)> Command)
{
	const FString Name(Label);
	Enqueue([this, Name, Moved = MoveTemp(Command)]() mutable
	{
		Record(0, CommandRan, 0, Name);
		Moved(*this);
	});
}

void VaRest::LwsProbe::FRig::ExecOn(int32 ConnId, const TCHAR* Label, TFunction<void(FConn&)> Command)
{
	FConn* Target = nullptr;
	{
		FScopeLock Lock(&ConnsLock);
		for (const TUniquePtr<FConn>& Each : Conns)
		{
			if (Each->Id == ConnId)
			{
				Target = Each.Get();
				break;
			}
		}
	}
	const FString Name(Label);
	Enqueue([this, Target, ConnId, Name, Moved = MoveTemp(Command)]() mutable
	{
		if (!Target || !Target->Wsi)
		{
			Record(ConnId, CommandRan, 0, Name + TEXT(": no wsi"));
			return;
		}
		Record(ConnId, CommandRan, 0, Name);
		Moved(*Target);
	});
}

void VaRest::LwsProbe::FRig::Write(int32 ConnId, int32 Protocol, TArray<uint8> Payload)
{
	FWrite Item;
	Item.Protocol = Protocol;
	Item.Buffer.SetNumZeroed(LWS_PRE);
	Item.Buffer.Append(Payload);
	ExecOn(ConnId, TEXT("write"), [Moved = MoveTemp(Item)](FConn& Conn) mutable
	{
		Conn.Outbox.Add(MoveTemp(Moved));
		lws_callback_on_writable(Conn.Wsi);
	});
}

void VaRest::LwsProbe::FRig::WriteText(int32 ConnId, const FString& Text, int32 Protocol)
{
	Write(ConnId, Protocol, Utf8Bytes(Text));
}

void VaRest::LwsProbe::FRig::Close(int32 ConnId, int32 Code, const FString& Reason)
{
	ExecOn(ConnId, TEXT("close"), [Code, Moved = Utf8Bytes(Reason)](FConn& Conn) mutable
	{
		Conn.CloseCode = Code;
		Conn.CloseReason = MoveTemp(Moved);
		lws_callback_on_writable(Conn.Wsi);
	});
}

int32 VaRest::LwsProbe::FRig::WriteNow(FConn& Conn, lws* Wsi, int32 Protocol, TConstArrayView<uint8> Payload, const TCHAR* Label)
{
	TArray<uint8> Buffer;
	Buffer.SetNumZeroed(LWS_PRE + Payload.Num());
	if (Payload.Num() > 0)
	{
		FMemory::Memcpy(Buffer.GetData() + LWS_PRE, Payload.GetData(), Payload.Num());
	}
	// A truncated write is buffered by LWS itself (libwebsockets.h:4914-4930), so Buffer may go when this returns
	const int Wrote = lws_write(Wsi, Buffer.GetData() + LWS_PRE, static_cast<size_t>(Payload.Num()), static_cast<lws_write_protocol>(Protocol));
	Record(Conn.Id, WriteDone, Payload.Num(), FString::Printf(TEXT("%s ret=%d partial=%d"), Label, Wrote, lws_partial_buffered(Wsi)));
	return Wrote;
}

int32 VaRest::LwsProbe::FRig::WriteNext(FConn& Conn, lws* Wsi)
{
	if (Conn.Outbox.Num() == 0)
	{
		if (Conn.CloseCode != 0)
		{
			// The engine's close (LwsWebSocket.cpp:515-530): the close frame's code and reason, then a non-zero return
			lws_close_reason(Wsi, static_cast<lws_close_status>(Conn.CloseCode), Conn.CloseReason.GetData(), static_cast<size_t>(Conn.CloseReason.Num()));
			Record(Conn.Id, CloseRequested, Conn.CloseReason.Num(), FString::Printf(TEXT("code=%d"), Conn.CloseCode));
			return -1;
		}
		return 0;
	}
	FWrite Next = MoveTemp(Conn.Outbox[0]);
	Conn.Outbox.RemoveAt(0);
	const int32 Size = Next.Buffer.Num() - LWS_PRE;
	const int Wrote = lws_write(Wsi, Next.Buffer.GetData() + LWS_PRE, static_cast<size_t>(Size), static_cast<lws_write_protocol>(Next.Protocol));
	Record(Conn.Id, WriteDone, Size, FString::Printf(TEXT("outbox ret=%d partial=%d"), Wrote, lws_partial_buffered(Wsi)));
	if (Wrote < 0)
	{
		return -1;
	}
	if (Conn.Outbox.Num() > 0 || Conn.CloseCode != 0)
	{
		lws_callback_on_writable(Wsi);
	}
	return 0;
}

int32 VaRest::LwsProbe::FRig::AppendHeaders(FConn& Conn, void* In, size_t Len)
{
	// in: a char** to the next free byte; len: the space left (libwebsockets.h:1316-1340). Each line is checked against
	// what is left, the check the engine skips (LwsWebSocket.cpp:537-547, WebSocket spec E19)
	char** Cursor = static_cast<char**>(In);
	int64 Written = 0;
	bool bAll = true;
	for (const FString& Line : Conn.Spec.AppendHeaders)
	{
		const FTCHARToUTF8 Converted(*Line);
		const int64 Need = Converted.Length() + 2;
		if (Written + Need > static_cast<int64>(Len))
		{
			bAll = false;
			break;
		}
		FMemory::Memcpy(*Cursor, Converted.Get(), Converted.Length());
		(*Cursor)[Converted.Length()] = '\r';
		(*Cursor)[Converted.Length() + 1] = '\n';
		*Cursor += Need;
		Written += Need;
	}
	Record(Conn.Id, HeadersAppended, Written, bAll ? TEXT("all") : TEXT("stopped: no space"));
	return 0;
}

VaRest::LwsProbe::FConn* VaRest::LwsProbe::FRig::FindConn(lws* Wsi, void* User)
{
	// The userdata first: a freed wsi's address may be reused by the next connection, a FConn's never is
	void* WsiUser = Wsi ? lws_wsi_user(Wsi) : nullptr;
	FScopeLock Lock(&ConnsLock);
	for (const TUniquePtr<FConn>& Each : Conns)
	{
		FConn* Candidate = Each.Get();
		if (Candidate == User || Candidate == WsiUser || (Wsi && !Candidate->bDestroyed && Candidate->Wsi == Wsi))
		{
			return Candidate;
		}
	}
	return nullptr;
}

int VaRest::LwsProbe::FRig::Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len)
{
	// Also called inside lws_create_context (protocol init, client TLS setup), before FRig::Context is set: the rig comes
	// from the context's user pointer (lws_context_creation_info::user), not from the member
	lws_context* Owner = Wsi ? lws_get_context(Wsi) : nullptr;
	FRig* Rig = Owner ? static_cast<FRig*>(lws_context_user(Owner)) : nullptr;
	return Rig ? Rig->Handle(Wsi, static_cast<int32>(Reason), User, In, Len) : 0;
}

int32 VaRest::LwsProbe::FRig::Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	// Poll bookkeeping LWS sends with its internal loop too (libwebsockets.h:1396-1449): not evidence
	case LWS_CALLBACK_GET_THREAD_ID:
	case LWS_CALLBACK_ADD_POLL_FD:
	case LWS_CALLBACK_DEL_POLL_FD:
	case LWS_CALLBACK_CHANGE_MODE_POLL_FD:
	case LWS_CALLBACK_LOCK_POLL:
	case LWS_CALLBACK_UNLOCK_POLL:
		return 0;
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		// lws_cancel_service lands here, on the service thread (libwebsockets.h:1551-1556): WebSocket spec 2.1's command point
		if (Options.bCommandsInsideCallback)
		{
			DrainCommands();
		}
		return 0;
	default:
		break;
	}

	FConn* Conn = FindConn(Wsi, User);
	FEvent Event;
	Event.Conn = Conn ? Conn->Id : 0;
	Event.Reason = Reason;
	Event.Time = FPlatformTime::Seconds();
	Event.Len = static_cast<int64>(Len);
	if (Conn)
	{
		Event.Flags |= User == Conn ? UserIsConnection : 0u;
		Event.Flags |= Wsi && lws_wsi_user(Wsi) == Conn ? WsiUserIsConnection : 0u;
	}
	switch (Reason)
	{
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		// in: an error string of len bytes, when not null (libwebsockets.h:1255-1299)
		Event.Note = Utf8Text(In, Len);
		break;
	case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
	{
		// The last point the response headers exist (libwebsockets.h:1301-1307): the selected subprotocol and the status.
		// lws_http_client_http_response is safe here because the header table exists by definition in this callback
		// (T6); without one it dereferences NULL (client.c.o 0x1294-0x12b2), which is why the CONNECTION_ERROR cases
		// are probed one per run (Probe.VaRest.Risky.Lws.StatusInConnectionError.*)
		char Selected[256] = {};
		const int Total = lws_hdr_total_length(Wsi, WSI_TOKEN_PROTOCOL);
		if (Total > 0 && Total < static_cast<int>(sizeof(Selected)))
		{
			lws_hdr_copy(Wsi, Selected, static_cast<int>(sizeof(Selected)), WSI_TOKEN_PROTOCOL);
		}
		Event.Note = FString::Printf(TEXT("protocol=%s status=%u"), *Utf8Text(Selected, FCStringAnsi::Strlen(Selected)), lws_http_client_http_response(Wsi));
		break;
	}
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
	{
		const lws_protocols* Bound = lws_get_protocol(Wsi);
		const FString BoundName = Bound && Bound->name ? Utf8Text(Bound->name, FCStringAnsi::Strlen(Bound->name)) : FString(TEXT("(none)"));
		Event.Note = TEXT("bound=") + BoundName;
		break;
	}
	case LWS_CALLBACK_CLIENT_RECEIVE:
		Event.Flags |= lws_is_first_fragment(Wsi) ? FirstFragment : 0u;
		Event.Flags |= lws_is_final_fragment(Wsi) ? FinalFragment : 0u;
		Event.Flags |= lws_frame_is_binary(Wsi) ? BinaryFrame : 0u;
		Event.Remaining = static_cast<int64>(lws_remaining_packet_payload(Wsi));
		CopyBytes(Event, In, Len);
		break;
	case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		CopyBytes(Event, In, Len);
		break;
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
	{
		// user: the X509_STORE_CTX*, in: the SSL*, len: preverify_ok (libwebsockets.h:1015-1044)
		X509_STORE_CTX* Store = static_cast<X509_STORE_CTX*>(User);
		Event.Note = FString::Printf(TEXT("preverify=%d error=%d depth=%d"), static_cast<int32>(Len), X509_STORE_CTX_get_error(Store), X509_STORE_CTX_get_error_depth(Store));
		break;
	}
	case LWS_CALLBACK_WSI_DESTROY:
		if (Conn)
		{
			Conn->Wsi = nullptr;
			Conn->bDestroyed = true;
		}
		break;
	default:
		break;
	}
	Append(MoveTemp(Event));

	if (Conn && Conn->Spec.Hook)
	{
		const int32 Hooked = Conn->Spec.Hook(*Conn, Wsi, Reason, User, In, Len);
		if (Hooked != KeepDefault)
		{
			return Hooked;
		}
	}

	switch (Reason)
	{
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		return Conn ? AppendHeaders(*Conn, In, Len) : 0;
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		return Conn ? WriteNext(*Conn, Wsi) : 0;
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		// 0 lets LWS echo the close (libwebsockets.h:1217-1225); the engine returns 1 (LwsWebSocket.cpp:389-390)
		return Conn ? Conn->Spec.PeerCloseReturn : 0;
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		if (Options.OnLoadExtraClientVerifyCerts)
		{
			Options.OnLoadExtraClientVerifyCerts(static_cast<SSL_CTX*>(User));
		}
		return 0;
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
		// 0 keeps OpenSSL's verdict, 1 fails the certificate (libwebsockets.h:1029-1037)
		return Options.OnVerifyServerCert && Conn ? Options.OnVerifyServerCert(*Conn, static_cast<X509_STORE_CTX*>(User), static_cast<int64>(Len)) : 0;
	default:
		return 0;
	}
}

void VaRest::LwsProbe::FRig::CopyBytes(FEvent& Event, const void* In, size_t Len)
{
	using namespace VaRest::LwsProbe::VaRestLwsProbePrivate;
	if (!In || Len == 0)
	{
		return;
	}
	const int64 Take = FMath::Min<int64>(static_cast<int64>(Len), MaxBytesPerEvent);
	FScopeLock Lock(&EventsLock);
	if (CopiedBytes + Take > MaxBytesPerRig)
	{
		Event.Note += TEXT(" [bytes not kept: over the rig's 64 MiB]");
		return;
	}
	CopiedBytes += Take;
	Event.Bytes.Append(static_cast<const uint8*>(In), static_cast<int32>(Take));
	if (Take < static_cast<int64>(Len))
	{
		Event.Note += FString::Printf(TEXT(" [first %lld of %lld bytes kept]"), Take, static_cast<int64>(Len));
	}
}

void VaRest::LwsProbe::FRig::Append(FEvent&& Event)
{
	FScopeLock Lock(&EventsLock);
	Recorded.Add(MoveTemp(Event));
}

void VaRest::LwsProbe::FRig::Record(int32 ConnId, int32 Reason, int64 Len, const FString& Note)
{
	FEvent Event;
	Event.Conn = ConnId;
	Event.Reason = Reason;
	Event.Time = FPlatformTime::Seconds();
	Event.Len = Len;
	Event.Note = Note;
	Append(MoveTemp(Event));
}

bool VaRest::LwsProbe::FRig::WaitFor(TFunctionRef<bool(const TArray<FEvent>&)> Done, double Seconds)
{
	// The core ticker too: the HTTP client and the engine's IWebSocket deliver from it (LwsWebSocketsManager.cpp:257-258,
	// BackgroundableTicker.cpp:18-29)
	return VaRest::Http::Testing::PumpUntil([this, &Done]
	{
		FScopeLock Lock(&EventsLock);
		return Done(Recorded);
	}, Seconds, true);
}

bool VaRest::LwsProbe::FRig::WaitReason(int32 ConnId, int32 Reason, double Seconds)
{
	return WaitFor([ConnId, Reason](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnId, Reason](const FEvent& Each) { return Each.Conn == ConnId && Each.Reason == Reason; });
	}, Seconds);
}

bool VaRest::LwsProbe::FRig::WaitEnded(int32 ConnId, double Seconds)
{
	return WaitFor([ConnId](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnId](const FEvent& Each)
		{
			return Each.Conn == ConnId && (Each.Reason == LWS_CALLBACK_WSI_DESTROY || (Each.Reason == ConnectReturned && Each.Note == TEXT("null")));
		});
	}, Seconds);
}

TArray<VaRest::LwsProbe::FEvent> VaRest::LwsProbe::FRig::Events(int32 ConnId) const
{
	FScopeLock Lock(&EventsLock);
	if (ConnId == 0)
	{
		return Recorded;
	}
	return Recorded.FilterByPredicate([ConnId](const FEvent& Each) { return Each.Conn == ConnId; });
}

bool VaRest::LwsProbe::FRig::Find(int32 ConnId, int32 Reason, FEvent& Out, const TCHAR* NotePrefix) const
{
	FScopeLock Lock(&EventsLock);
	for (const FEvent& Each : Recorded)
	{
		if (Each.Conn == ConnId && Each.Reason == Reason && (!NotePrefix || Each.Note.StartsWith(NotePrefix, ESearchCase::CaseSensitive)))
		{
			Out = Each;
			return true;
		}
	}
	return false;
}

// ---- helpers

FString VaRest::LwsProbe::NewKey()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

FString VaRest::LwsProbe::Utf8Text(const void* Data, size_t Num)
{
	if (!Data || Num == 0)
	{
		return FString();
	}
	const FUTF8ToTCHAR Converted(static_cast<const ANSICHAR*>(Data), static_cast<int32>(Num));
	return FString::ConstructFromPtrSize(Converted.Get(), Converted.Length());
}

FString VaRest::LwsProbe::EventText(const FEvent& Event)
{
	return Utf8Text(Event.Bytes.GetData(), static_cast<size_t>(Event.Bytes.Num()));
}

TArray<uint8> VaRest::LwsProbe::Utf8Bytes(const FString& Source)
{
	const FTCHARToUTF8 Converted(*Source);
	return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
}

FString VaRest::LwsProbe::HexOf(TConstArrayView<uint8> Bytes)
{
	static const TCHAR Digits[] = TEXT("0123456789abcdef");
	FString Out;
	Out.Reserve(Bytes.Num() * 2);
	for (const uint8 Byte : Bytes)
	{
		Out.AppendChar(Digits[Byte >> 4]);
		Out.AppendChar(Digits[Byte & 15]);
	}
	return Out;
}

FString VaRest::LwsProbe::ReasonName(int32 Reason)
{
	switch (Reason)
	{
	case ConnectCalling: return TEXT("connect>");
	case ConnectReturned: return TEXT("connect<");
	case CommandRan: return TEXT("command");
	case WriteDone: return TEXT("write");
	case CloseRequested: return TEXT("close>");
	case HeadersAppended: return TEXT("headers+");
	case Marker: return TEXT("marker");
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR: return TEXT("CONNECTION_ERROR");
	case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH: return TEXT("FILTER_PRE_ESTABLISH");
	case LWS_CALLBACK_CLIENT_ESTABLISHED: return TEXT("ESTABLISHED");
	case LWS_CALLBACK_CLOSED: return TEXT("CLOSED");
	case LWS_CALLBACK_CLIENT_RECEIVE: return TEXT("RECEIVE");
	case LWS_CALLBACK_CLIENT_RECEIVE_PONG: return TEXT("RECEIVE_PONG");
	case LWS_CALLBACK_CLIENT_WRITEABLE: return TEXT("WRITEABLE");
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS: return TEXT("LOAD_EXTRA_CLIENT_VERIFY_CERTS");
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER: return TEXT("APPEND_HANDSHAKE_HEADER");
	case LWS_CALLBACK_PROTOCOL_INIT: return TEXT("PROTOCOL_INIT");
	case LWS_CALLBACK_PROTOCOL_DESTROY: return TEXT("PROTOCOL_DESTROY");
	case LWS_CALLBACK_WSI_CREATE: return TEXT("WSI_CREATE");
	case LWS_CALLBACK_WSI_DESTROY: return TEXT("WSI_DESTROY");
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE: return TEXT("PEER_INITIATED_CLOSE");
	case LWS_CALLBACK_ESTABLISHED_CLIENT_HTTP: return TEXT("ESTABLISHED_CLIENT_HTTP");
	case LWS_CALLBACK_CLOSED_CLIENT_HTTP: return TEXT("CLOSED_CLIENT_HTTP");
	case LWS_CALLBACK_HTTP_BIND_PROTOCOL: return TEXT("HTTP_BIND_PROTOCOL");
	case LWS_CALLBACK_HTTP_DROP_PROTOCOL: return TEXT("HTTP_DROP_PROTOCOL");
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION: return TEXT("PERFORM_SERVER_CERT_VERIFICATION");
	case LWS_CALLBACK_TIMER: return TEXT("TIMER");
	case LWS_CALLBACK_CLIENT_CLOSED: return TEXT("CLIENT_CLOSED");
	default: return FString::Printf(TEXT("reason %d"), Reason);
	}
}

FString VaRest::LwsProbe::CallSequence(const TArray<FEvent>& Events)
{
	TArray<FString> Parts;
	int32 Last = MIN_int32;
	int32 Repeats = 0;
	const auto Flush = [&Parts, &Last, &Repeats]
	{
		if (Repeats > 0)
		{
			Parts.Add(Repeats > 1 ? FString::Printf(TEXT("%s x%d"), *ReasonName(Last), Repeats) : ReasonName(Last));
		}
	};
	for (const FEvent& Each : Events)
	{
		// The rig's own bookkeeping is not a callback
		if (Each.Reason == CommandRan || Each.Reason == ConnectCalling || Each.Reason == HeadersAppended || Each.Reason == Marker)
		{
			continue;
		}
		if (Each.Reason == Last)
		{
			++Repeats;
			continue;
		}
		Flush();
		Last = Each.Reason;
		Repeats = 1;
	}
	Flush();
	return FString::Join(Parts, TEXT(" > "));
}

TArray<VaRest::LwsProbe::FEvent> VaRest::LwsProbe::OfReason(const TArray<FEvent>& Events, int32 Reason)
{
	return Events.FilterByPredicate([Reason](const FEvent& Each) { return Each.Reason == Reason; });
}

bool VaRest::LwsProbe::Opened(const TArray<FEvent>& Events)
{
	return Events.ContainsByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; });
}

FString VaRest::LwsProbe::ErrorOf(const TArray<FEvent>& Events)
{
	const FEvent* Error = Events.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR; });
	return Error ? Error->Note : FString::Printf(TEXT("(no CONNECTION_ERROR: %s)"), *CallSequence(Events));
}

FString VaRest::LwsProbe::ConnectResult(const TArray<FEvent>& Events)
{
	const FEvent* Returned = Events.FindByPredicate([](const FEvent& Each) { return Each.Reason == ConnectReturned; });
	return Returned ? Returned->Note : FString(TEXT("(not called)"));
}

bool VaRest::LwsProbe::Visit(FAutomationTestBase& Test, FRig& Rig, FConnectSpec Spec, TArray<FEvent>& Out, double Seconds)
{
	const int32 ConnId = Rig.Connect(MoveTemp(Spec));
	Rig.WaitFor([ConnId](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([ConnId](const FEvent& Each)
		{
			return Each.Conn == ConnId && (Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_WSI_DESTROY
				|| (Each.Reason == ConnectReturned && Each.Note == TEXT("null")));
		});
	}, Seconds);
	if (Opened(Rig.Events(ConnId)))
	{
		Rig.Close(ConnId, 1000, TEXT("probe done"));
	}
	const bool bEnded = Rig.WaitEnded(ConnId, Seconds);
	Out = Rig.Events(ConnId);
	NoteOutcome(Test, FString::Printf(TEXT("connection %d ended within %.0f s (%s)"), ConnId, Seconds, *CallSequence(Out)), bEnded);
	return bEnded;
}

bool VaRest::LwsProbe::WaitBytes(FRig& Rig, int32 ConnId, int64 Bytes, double Seconds)
{
	return Rig.WaitFor([ConnId, Bytes](const TArray<FEvent>& All)
	{
		int64 Got = 0;
		for (const FEvent& Each : All)
		{
			Got += Each.Conn == ConnId && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE ? Each.Len : 0;
		}
		return Got >= Bytes;
	}, Seconds);
}

FVaRestJson VaRest::LwsProbe::FetchJournal(FAutomationTestBase& Test, const FServer& Site, const FString& Key, int32 Connections, int32 WaitMs)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(FString::Printf(TEXT("/ws/journal?key=%s&connections=%d&wait_ms=%d"), *Key, Connections, WaitMs)), Outcome).Send();
	const bool bDone = PumpUntil([Outcome] { return Outcome->IsDone(); }, WaitMs / 1000.0 + 15.0, true);
	if (!bDone || Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("the journal of %s did not arrive: %s"), *Key, bDone ? *Outcome->Error.Describe() : TEXT("no answer")));
		return FVaRestJson();
	}
	return Outcome->Value.GetValue();
}

TArray<FVaRestJson> VaRest::LwsProbe::JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex)
{
	TArray<FVaRestJson> Out;
	for (const FVaRestJson& Event : Journal[TEXT("events")].Elements())
	{
		if (Event[TEXT("ev")].AsString().Equals(Kind, ESearchCase::CaseSensitive) && (ConnIndex == 0 || Event[TEXT("conn")].AsInt() == ConnIndex))
		{
			Out.Add(Event);
		}
	}
	return Out;
}

FString VaRest::LwsProbe::JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex)
{
	for (const FVaRestJson& Request : JournalEvents(Journal, TEXT("request"), ConnIndex))
	{
		for (const FVaRestJson& Pair : Request[TEXT("headers")].Elements())
		{
			// HTTP header names are case-insensitive (RFC 9110 5.1)
			if (Pair[0].AsString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pair[1].AsString();
			}
		}
	}
	return TEXT("(absent)");
}

TArray<uint8> VaRest::LwsProbe::JournalBytes(const FVaRestJson& Event)
{
	TArray<uint8> Out;
	FBase64::Decode(Event[TEXT("b64")].AsString(), Out);
	return Out;
}

bool VaRest::LwsProbe::FetchBytes(FAutomationTestBase& Test, const FServer& Site, const FString& Path, TArray<uint8>& Out)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<TArray<uint8>>();
	Observe(Client.Get<TArray<uint8>>(Path), Outcome).Send();
	const bool bDone = PumpUntil([Outcome] { return Outcome->IsDone(); }, 20.0, true);
	if (!bDone || Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("GET %s failed: %s"), *Path, bDone ? *Outcome->Error.Describe() : TEXT("no answer within 20 s")));
		return false;
	}
	Out = Outcome->Value.GetValue();
	return true;
}

int32 VaRest::LwsProbe::AddPemToStore(SSL_CTX* Ctx, TConstArrayView<uint8> Pem)
{
	BIO* Bio = BIO_new_mem_buf(Pem.GetData(), Pem.Num());
	if (!Bio)
	{
		return 0;
	}
	X509_STORE* Store = SSL_CTX_get_cert_store(Ctx);
	int32 Added = 0;
	while (X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr))
	{
		Added += X509_STORE_add_cert(Store, Certificate) == 1 ? 1 : 0;
		X509_free(Certificate);
	}
	// The read that ends the loop leaves "no start line" on OpenSSL's error queue
	ERR_clear_error();
	BIO_free(Bio);
	return Added;
}

FString VaRest::LwsProbe::UrlValue(const FString& Value)
{
	return FGenericPlatformHttp::UrlEncode(Value);
}

bool VaRest::LwsProbe::IsMessageStart(int32 Protocol)
{
	// Without LWS_WRITE_NO_FIN (0x40) and LWS_WRITE_H2_STREAM_END (0x80), libwebsockets.h:4819-4827
	const int32 Kind = Protocol & 0x3F;
	return Kind == LWS_WRITE_TEXT || Kind == LWS_WRITE_BINARY;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 7: Build and run the rig tests**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Rig`
Expected: `run_tests: 4 passed, 0 failed`.

If `Rig.Echo`, `Rig.SequentialConnections` and `Rig.TeardownWithOpenConnection` fail to open, echo or close while `Rig.FactLine` passes, the inside-callback command path may be what fails (review M-13, re-review N-2: those three all go through `Enqueue` → `DrainCommands`, only `Rig.FactLine` does not): set `FRigOptions::bCommandsInsideCallback`'s default to `false` in `VaRestLwsProbe.h` (commands then drain between `lws_service` calls, the engine's way), re-run, and keep it if they pass; note it in the task report. Task 5's `svc.commands_*` rows record both placements either way, and Task 11's E2 reads them.

Restore-the-bug: in `FRig::Stop()`, comment out the `lws_cancel_service(Context);` call. `Probe.VaRest.Lws.Rig.TeardownWithOpenConnection` must fail with "the teardown returns within 5 s ... (took ~10 s)", because the service thread sits out its 10 s wait. Restore the line and see it pass. Report both outcomes.

- [ ] **Step 8: Check the default suite and the monolithic build are untouched**

Run: `Tools/run_tests.sh --no-build VaRest`
Expected: the default suite passes as before, and no probe ran: `grep -c 'Test Completed. Result={[A-Za-z]*} Name={[^}]*} Path={Probe\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `0`.

Run: `/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject`
Expected: `Result: Succeeded`. VaRestTests and its libwebsockets copy are not part of the game target (decision 1).

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestHttpTesting.cpp Source/VaRestTests/VaRestTests.Build.cs \
	Source/VaRestTests/Private/VaRestTestsModule.cpp \
	Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeLws.h Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h \
	Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.cpp Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRigTest.cpp
git commit -m "tests: libwebsockets 3.0 probe rig (WS W0) -- one context per test with the spec's service shape, ordered callback journal, facts as JSON log lines, server journal and TLS helpers

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 5: Probe — environment, service thread, timers and kills, handshake timeouts

**Files:**
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeServiceTest.cpp`

**Interfaces:**
- Consumes (Task 4): `FRig` (`Connect`, `Exec`, `ExecOn`, `WriteText`, `Close`, `WaitFor`, `WaitReason`, `WaitEnded`, `Events`, `Find`), `FConnectSpec`, `FRigOptions` (`bCommandsInsideCallback`, `TimeoutSecs`, `ServiceTimeoutMs`), `FindServer`, `Fact`, `AgreesIf`, `EVerdict`, `YesNo`, `NoteOutcome`, `GWebSocketsLoadedAtModuleStartup`, `NewKey`, `EventText`, `Utf8Text`, `CallSequence`, `ErrorOf`, `FetchJournal`, `JournalEvents`, pseudo reasons `ConnectCalling`, `ConnectReturned`, `CommandRan`; server routes `/ws/echo`, `/ws/hang`.
- Produces facts (ids exactly as in `Tools/lws_probe_passport.py` `EXPECTED`): `env.lws_version`, `env.openssl_version`, `env.build`, `env.websockets_loaded_at_start`, `release.win64`, `release.mac`, `release.android_ios`, `coexist.monolithic`, `svc.commands_inside_wait_cancelled`, `svc.commands_between_service_calls`, `svc.cancel_wake_ms`, `svc.timer_latency_ms`, `svc.kill_async_ms`, `svc.kill_async_ms_short_wait`, `svc.kill_async_sequence`, `svc.kill_async_close_frame`, `timeout.default_secs`, `timeout.configured_secs`, `timeout.kill_pending_ms`, `timeout.kill_pending_ms_short_wait`, `timeout.kill_pending_sequence`, `timeout.tcp_connect_blackhole`, `timeout.default_secs_blackhole`.
- Tests: `Probe.VaRest.Lws.Service.Environment`, `.Service.Commands`, `.Service.TimerAndKill`, `Probe.VaRest.Lws.Timeout.DefaultHandshake`, `.Timeout.ConfiguredHandshake`, `.Timeout.KillPending`, `.Timeout.Blackhole`.
- Service waits (decision 9, review C-1): the timeout tests use 100 ms (`ShortWaitMs`), because they ask what LWS's own timers do. The command, wake and timer facts use the rig's 10 s, because they ask what the service shape does. Each kill latency is taken with both waits.

- [ ] **Step 1: Run the filter to see it match nothing**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Service`
Expected: exit 3, `run_tests: filter '^Probe.VaRest.Lws.Service' matched no tests`.

- [ ] **Step 2: Write the probe tests**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeServiceTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): the environment, the service thread's command path, timers and forced closes, and the
// handshake timeouts. Two service waits (WS W0 decision 9, review C-1): LWS checks its own deadlines only when
// lws_service returns (lws-plat-unix.c.o _lws_plat_service_tsi: the hrtimer list caps poll()'s wait, nothing else
// does), so facts about LWS's timers take a 100 ms wait, and facts about the service shape the rig's 10 s one

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformProperties.h"
#include "HAL/PlatformTime.h"
#include "Misc/App.h"
#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate
{
	/** The wait for facts about LWS's own timers: short enough that a deadline is noticed within 0.1 s of passing */
	constexpr int32 ShortWaitMs = 100;
	/** The rig's default, spec 2.1's long wait woken by lws_cancel_service */
	constexpr int32 LongWaitMs = 10000;

	struct FRoundTrip
	{
		bool bEstablished = false;
		bool bEchoed = false;
		bool bEnded = false;
		double WakeMs = -1.0;
	};

	/** Connect, a wake while the service thread waits, an echo, a close: every step goes through the command queue */
	FRoundTrip RoundTrip(const FServer& Site, bool bInsideCallback)
	{
		FRoundTrip Out;
		FRigOptions Options;
		Options.bCommandsInsideCallback = bInsideCallback;
		FRig Rig(Site, MoveTemp(Options));
		if (!Rig.IsReady())
		{
			return Out;
		}
		FConnectSpec Spec;
		Spec.Path = TEXT("/ws/echo?key=") + NewKey();
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Out.bEstablished = Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED);
		if (Out.bEstablished)
		{
			// The service thread parks in its 10 s wait (FRigOptions::ServiceTimeoutMs) before the wake is timed
			FPlatformProcess::Sleep(0.2f);
			const double Queued = FPlatformTime::Seconds();
			Rig.Exec(TEXT("wake"), [](FRig&) {});
			FEvent Ran;
			if (Rig.WaitReason(0, CommandRan) && Rig.Find(0, CommandRan, Ran, TEXT("wake")))
			{
				Out.WakeMs = (Ran.Time - Queued) * 1000.0;
			}
			Rig.WriteText(Conn, TEXT("round trip"));
			Out.bEchoed = Rig.WaitFor([Conn](const TArray<FEvent>& All)
			{
				return All.ContainsByPredicate([Conn](const FEvent& Each)
				{
					return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("round trip"), ESearchCase::CaseSensitive);
				});
			});
			Rig.Close(Conn, 1000, TEXT("done"));
		}
		Out.bEnded = Rig.WaitEnded(Conn);
		return Out;
	}

	FString Describe(const FRoundTrip& Trip)
	{
		return FString::Printf(TEXT("established=%s echoed=%s ended=%s, wake %.1f ms"), YesNo(Trip.bEstablished), YesNo(Trip.bEchoed), YesNo(Trip.bEnded), Trip.WakeMs);
	}

	struct FHang
	{
		bool bEnded = false;
		double Seconds = -1.0;
		FString Error;
		FString Sequence;
	};

	/**
	 * A connection whose TCP connects and whose upgrade is never answered (/ws/hang), or a TCP connect to Address, timed
	 * from the connect call to its end, with the short wait: these ask what LWS's own deadline is (review C-1)
	 */
	FHang Hang(const FServer& Site, uint32 TimeoutSecs, const FString& Address, int32 Port, double WaitSeconds)
	{
		FHang Out;
		FRigOptions Options;
		Options.TimeoutSecs = TimeoutSecs;
		Options.ServiceTimeoutMs = ShortWaitMs;
		FRig Rig(Site, MoveTemp(Options));
		if (!Rig.IsReady())
		{
			return Out;
		}
		FConnectSpec Spec;
		Spec.Path = TEXT("/ws/hang?ms=60000&key=") + NewKey();
		Spec.Address = Address;
		Spec.Port = Port;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Out.bEnded = Rig.WaitEnded(Conn, WaitSeconds);
		const TArray<FEvent> Seen = Rig.Events(Conn);
		FEvent Start;
		FEvent End;
		const bool bStarted = Rig.Find(Conn, ConnectCalling, Start);
		if (Out.bEnded && bStarted && (Rig.Find(Conn, LWS_CALLBACK_CLIENT_CONNECTION_ERROR, End) || Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, End)))
		{
			Out.Seconds = End.Time - Start.Time;
		}
		Out.Error = ErrorOf(Seen);
		Out.Sequence = CallSequence(Seen);
		return Out;
	}

	FString Describe(const FHang& Run, double WaitSeconds)
	{
		return Run.bEnded ? FString::Printf(TEXT("%.1f s, then \"%s\" (%s)"), Run.Seconds, *Run.Error, *Run.Sequence)
			: FString::Printf(TEXT("still pending after %.0f s (%s)"), WaitSeconds, *Run.Sequence);
	}

	/** A blackhole result, saying so when the host has no blackhole (the connect failed at once) */
	FString DescribeBlackhole(const FHang& Run, double WaitSeconds)
	{
		return Run.bEnded && Run.Seconds >= 0.0 && Run.Seconds < 0.5
			? FString::Printf(TEXT("no blackhole on this host (failed at once): %s"), *Describe(Run, WaitSeconds))
			: Describe(Run, WaitSeconds);
	}

	struct FKill
	{
		bool bReady = false;
		bool bEnded = false;
		double Ms = -1.0;
		FString Sequence;
		FString Key;
		FString KillNote = TEXT("not run");
	};

	/**
	 * A connection to Route killed with lws_set_timeout(LWS_TO_KILL_ASYNC) from a command -- open (bWhenOpen, /ws/echo)
	 * or pending with its upgrade unanswered (/ws/hang) -- timed from the command to WSI_DESTROY under one service wait
	 */
	FKill Kill(const FServer& Site, const FString& Route, int32 ServiceTimeoutMs, bool bWhenOpen)
	{
		FKill Out;
		FRigOptions Options;
		Options.ServiceTimeoutMs = ServiceTimeoutMs;
		FRig Rig(Site, MoveTemp(Options));
		if (!Rig.IsReady())
		{
			return Out;
		}
		Out.Key = NewKey();
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("%s%skey=%s"), *Route, Route.Contains(TEXT("?")) ? TEXT("&") : TEXT("?"), *Out.Key);
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		Out.bReady = bWhenOpen ? Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED) : Rig.WaitReason(Conn, ConnectReturned);
		if (!Out.bReady)
		{
			Rig.WaitEnded(Conn);
			return Out;
		}
		// Open: the service thread parks in its wait. Pending: the upgrade is out and unanswered by now
		FPlatformProcess::Sleep(bWhenOpen ? 0.2f : 1.0f);
		Rig.ExecOn(Conn, TEXT("kill"), [](FConn& C) { lws_set_timeout(C.Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC); });
		Out.bEnded = Rig.WaitEnded(Conn, 25.0);
		FEvent Killed;
		FEvent Destroyed;
		if (Rig.Find(Conn, CommandRan, Killed, TEXT("kill")))
		{
			Out.KillNote = Killed.Note;
		}
		// Review M-12: the sequence and the latency only from a kill that ran (a "kill: no wsi" never reached LWS)
		if (Out.bEnded && Out.KillNote.Equals(TEXT("kill"), ESearchCase::CaseSensitive) && Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed))
		{
			Out.Ms = (Destroyed.Time - Killed.Time) * 1000.0;
			Out.Sequence = CallSequence(Rig.Events(Conn).FilterByPredicate([&Killed](const FEvent& Each) { return Each.Time >= Killed.Time && Each.Reason != CommandRan; }));
		}
		return Out;
	}

	FString Describe(const FKill& Run, int32 ServiceTimeoutMs)
	{
		return Run.Ms >= 0.0
			? FString::Printf(TEXT("%.1f ms from lws_set_timeout(LWS_TO_KILL_ASYNC) to WSI_DESTROY, lws_service waiting up to %d ms"), Run.Ms, ServiceTimeoutMs)
			: FString::Printf(TEXT("not measured: ready %s, kill %s, ended %s (wait %d ms)"), YesNo(Run.bReady), *Run.KillNote, YesNo(Run.bEnded), ServiceTimeoutMs);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeEnvironmentTest, "Probe.VaRest.Lws.Service.Environment",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeEnvironmentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	const char* LwsVersion = lws_get_library_version();
	const FString Version = Utf8Text(LwsVersion, FCStringAnsi::Strlen(LwsVersion));
	Fact(*this, TEXT("env.lws_version"), AgreesIf(Version.StartsWith(TEXT("3.0.0"))), Version,
		TEXT("T1: libwebsockets 3.0.0 on every target (lws_config.h:34)"), TEXT("lws_get_library_version() in VaRestTests' copy"));
	const char* SslVersion = OpenSSL_version(OPENSSL_VERSION);
	// Review M-5: VaRestTests links OpenSSL statically, but on Linux its calls may bind to the global-scope copy (the
	// HTTP and SSL modules export it): which object the function resolves to goes in the evidence
	FString SslOwner = TEXT("which object provides it is not looked up on this platform");
#if PLATFORM_LINUX
	Dl_info SslInfo = {};
	SslOwner = dladdr(reinterpret_cast<void*>(&OpenSSL_version), &SslInfo) != 0 && SslInfo.dli_fname
		? FString::Printf(TEXT("OpenSSL_version resolves to %s"), *FPaths::GetCleanFilename(UTF8_TO_TCHAR(SslInfo.dli_fname)))
		: FString(TEXT("dladdr found no object for OpenSSL_version"));
#endif
	Fact(*this, TEXT("env.openssl_version"), EVerdict::Recorded, Utf8Text(SslVersion, FCStringAnsi::Strlen(SslVersion)),
		TEXT("none (the engine bundles OpenSSL 1.1.1t, ThirdParty/OpenSSL/OpenSSL.Build.cs:12)"),
		FString::Printf(TEXT("OpenSSL_version(OPENSSL_VERSION) as VaRestTests' calls resolve it; %s"), *SslOwner));
	const FString Platform = ANSI_TO_TCHAR(FPlatformProperties::PlatformName());
	Fact(*this, TEXT("env.build"), EVerdict::Recorded,
		FString::Printf(TEXT("%s, %s, %s"), *Platform, IS_MONOLITHIC ? TEXT("monolithic") : TEXT("modular"), LexToString(FApp::GetBuildConfiguration())),
		TEXT("none"), TEXT("FPlatformProperties::PlatformName, IS_MONOLITHIC, FApp::GetBuildConfiguration"));
	// Review I-4: sampled in FVaRestTestsModule::StartupModule, before any test (tests run in name order, and Coexist,
	// which loads WebSockets, runs before this one)
	Fact(*this, TEXT("env.websockets_loaded_at_start"), EVerdict::Recorded,
		FString::Printf(TEXT("%s at VaRestTests' startup (%s when this test ran)"), YesNo(GWebSocketsLoadedAtModuleStartup), YesNo(FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))),
		TEXT("WebSocket spec D30 sets the LWS log level only when the engine's WebSockets module is not loaded"),
		TEXT("FModuleManager::IsModuleLoaded(\"WebSockets\") in FVaRestTestsModule::StartupModule (LoadingPhase Default), and again now"));

	const FString ReleasePass = TEXT("WebSocket spec 16.6: Win64, Mac, Android and iOS in the release pass (bead VaRest-b4d)");
	const FString Here = FString::Printf(TEXT("this run: %s"), *Platform);
	Fact(*this, TEXT("release.win64"), PLATFORM_WINDOWS ? EVerdict::Recorded : EVerdict::Unverified,
		PLATFORM_WINDOWS ? FString(TEXT("this is the Win64 run: see the other rows"))
			: FString(TEXT("unverified here; release-pass item (Win64): run Tools/run_tests.sh Probe.VaRest.Lws and each Probe.VaRest.Risky case there")),
		ReleasePass, Here);
	Fact(*this, TEXT("release.mac"), PLATFORM_MAC ? EVerdict::Recorded : EVerdict::Unverified,
		PLATFORM_MAC ? FString(TEXT("this is the Mac run: see the other rows"))
			: FString(TEXT("unverified here; release-pass item (Mac): run Tools/run_tests.sh Probe.VaRest.Lws and each Probe.VaRest.Risky case there")),
		ReleasePass, Here);
	Fact(*this, TEXT("release.android_ios"), EVerdict::Unverified,
		TEXT("unverified here; release-pass item (Android, iOS): the probe cannot run there (VaRestTests: PlatformAllowList Win64, Mac, Linux); W1's transport suite on devices covers them"),
		ReleasePass, TEXT("VaRest.uplugin"));
	Fact(*this, TEXT("coexist.monolithic"), EVerdict::Unverified,
		TEXT("not run in W0: no VaRest runtime module links LWS before W1, and VaRestTests (UncookedOnly) is never in a game target (ModuleDescriptor.cs:792-793, TargetRules.cs:1190-1193); W1 exit item"),
		TEXT("WebSocket spec 1.2 and 16.4: an engine IWebSocket and a VaRest socket at once in a monolithic Development game"), TEXT("UBT module host types"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeCommandsTest, "Probe.VaRest.Lws.Service.Commands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeCommandsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	const FRoundTrip Inside = RoundTrip(Site, true);
	const FRoundTrip Between = RoundTrip(Site, false);
	NoteOutcome(*this, TEXT("both round trips ended"), Inside.bEnded && Between.bEnded);
	Fact(*this, TEXT("svc.commands_inside_wait_cancelled"), AgreesIf(Inside.bEstablished && Inside.bEchoed && Inside.bEnded), Describe(Inside),
		TEXT("WebSocket spec 2.1: the service thread drains commands (connect, send, close) in LWS_CALLBACK_EVENT_WAIT_CANCELLED"),
		TEXT("lws_client_connect_via_info, lws_callback_on_writable and the close all called inside EVENT_WAIT_CANCELLED"));
	Fact(*this, TEXT("svc.commands_between_service_calls"), EVerdict::Recorded, Describe(Between),
		TEXT("none: the engine starts sockets between lws_service calls (LwsWebSocketsManager.cpp:485-509)"),
		TEXT("the same round trip with the commands drained before each lws_service"));
	Fact(*this, TEXT("svc.cancel_wake_ms"), AgreesIf(Inside.WakeMs >= 0.0 && Inside.WakeMs < 100.0), FString::Printf(TEXT("%.1f ms"), Inside.WakeMs),
		TEXT("T13: lws_cancel_service wakes the service loop (libwebsockets.h:3704-3716)"),
		TEXT("a command queued while lws_service waited (10 s), timed until it ran in EVENT_WAIT_CANCELLED"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTimerAndKillTest, "Probe.VaRest.Lws.Service.TimerAndKill",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTimerAndKillTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	{
		// A per-wsi timer, armed from a command while lws_service would wait 10 s (libwebsockets.h:4705-4732): the
		// hrtimer list caps poll()'s wait, so this one is about the shape and keeps the long wait
		FRig Rig(Site);
		if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return false;
		}
		FConnectSpec Spec;
		Spec.Path = TEXT("/ws/echo?key=") + NewKey();
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		if (!TestTrue(TEXT("the echo connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
		{
			return false;
		}
		FPlatformProcess::Sleep(0.2f);
		Rig.ExecOn(Conn, TEXT("timer"), [](FConn& C) { lws_set_timer_usecs(C.Wsi, 200 * 1000); });
		const bool bTimer = Rig.WaitReason(Conn, LWS_CALLBACK_TIMER, 15.0);
		FEvent Armed;
		FEvent Fired;
		const bool bTimed = bTimer && Rig.Find(Conn, CommandRan, Armed, TEXT("timer")) && Rig.Find(Conn, LWS_CALLBACK_TIMER, Fired);
		const double TimerMs = bTimed ? (Fired.Time - Armed.Time) * 1000.0 : -1.0;
		Fact(*this, TEXT("svc.timer_latency_ms"), AgreesIf(bTimed && TimerMs < 1000.0),
			bTimed ? FString::Printf(TEXT("%.1f ms for a 200 ms timer, lws_service waiting up to 10 s"), TimerMs) : FString(TEXT("no LWS_CALLBACK_TIMER within 15 s")),
			TEXT("T14: lws_set_timer_usecs -> LWS_CALLBACK_TIMER, supported by the default poll loop (libwebsockets.h:4705-4732)"),
			TEXT("lws_set_timer_usecs(wsi, 200000) from a command on an open connection"));
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(*this, TEXT("the timer connection ended"), Rig.WaitEnded(Conn));
	}

	// A forced close from a command, "killed at the next timeout check" (libwebsockets.h:4673-4679): with the long wait
	// (the shape) and the short one (the control that shows how much of the delay is the wait, review C-1)
	const FKill Long = Kill(Site, TEXT("/ws/echo"), LongWaitMs, true);
	const FKill Short = Kill(Site, TEXT("/ws/echo"), ShortWaitMs, true);
	const FString KillSpec = TEXT("T14: forced close with lws_set_timeout(..., LWS_TO_KILL_ASYNC); WebSocket spec 2.1 loops lws_service(ctx, NextDeadline)");
	Fact(*this, TEXT("svc.kill_async_ms"), EVerdict::Recorded, Describe(Long, LongWaitMs), KillSpec, TEXT("a kill from a command on an open echo connection"));
	Fact(*this, TEXT("svc.kill_async_ms_short_wait"), EVerdict::Recorded, Describe(Short, ShortWaitMs), KillSpec,
		TEXT("the control for svc.kill_async_ms: the same kill with a 100 ms wait"));
	Fact(*this, TEXT("svc.kill_async_sequence"), EVerdict::Recorded, Long.Sequence.IsEmpty() ? FString(TEXT("not measured")) : Long.Sequence,
		TEXT("none"), TEXT("the callbacks after the kill command (the long-wait connection)"));
	FString CloseFrame = TEXT("not measured: the killed connection did not run");
	if (!Long.Key.IsEmpty())
	{
		const FVaRestJson Journal = FetchJournal(*this, Site, Long.Key);
		if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
		{
			return false;
		}
		CloseFrame = !Journal[TEXT("ended")].AsBool() ? FString(TEXT("the server still held the connection 5 s after the kill"))
			: JournalEvents(Journal, TEXT("close")).Num() > 0 ? FString(TEXT("a close frame reached the server"))
			: FString(TEXT("no close frame: the TCP connection just ended"));
	}
	Fact(*this, TEXT("svc.kill_async_close_frame"), EVerdict::Recorded, CloseFrame, TEXT("none"), TEXT("the server's journal of the killed connection"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeDefaultHandshakeTimeoutTest, "Probe.VaRest.Lws.Timeout.DefaultHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeDefaultHandshakeTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	const FHang Run = Hang(Site, 0, FString(), 0, 45.0);
	NoteOutcome(*this, TEXT("the unanswered upgrade ended within 45 s"), Run.bEnded);
	Fact(*this, TEXT("timeout.default_secs"), EVerdict::Recorded, Describe(Run, 45.0),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): LWS's default timeout_secs"),
		TEXT("timeout_secs = 0; /ws/hang accepts TCP, reads the upgrade, never answers; lws_service waiting up to 100 ms"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeConfiguredHandshakeTimeoutTest, "Probe.VaRest.Lws.Timeout.ConfiguredHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeConfiguredHandshakeTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	const FHang Run = Hang(Site, 3, FString(), 0, 20.0);
	NoteOutcome(*this, TEXT("the unanswered upgrade ended within 20 s"), Run.bEnded);
	Fact(*this, TEXT("timeout.configured_secs"), AgreesIf(Run.bEnded && Run.Seconds >= 2.5 && Run.Seconds <= 5.0), Describe(Run, 20.0),
		TEXT("lws_context_creation_info::timeout_secs bounds the handshake (libwebsockets.h:2791-2795; it is context-wide)"),
		TEXT("timeout_secs = 3; /ws/hang; lws_service waiting up to 100 ms"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeKillPendingTest, "Probe.VaRest.Lws.Timeout.KillPending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeKillPendingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// How WebSocket spec 2.2 enforces ConnectTimeout: a kill 1 s after the upgrade went out to a server that never
	// answers, timeout_secs left at LWS's default, with the long wait (the shape) and the short one (the control)
	const FKill Long = Kill(Site, TEXT("/ws/hang?ms=60000"), LongWaitMs, false);
	const FKill Short = Kill(Site, TEXT("/ws/hang?ms=60000"), ShortWaitMs, false);
	const FString KillSpec = TEXT("WebSocket spec 2.2: ConnectTimeout enforced with lws_set_timeout(..., LWS_TO_KILL_ASYNC) (T14)");
	Fact(*this, TEXT("timeout.kill_pending_ms"), EVerdict::Recorded, Describe(Long, LongWaitMs), KillSpec, TEXT("a pending attempt to /ws/hang, killed from a command"));
	Fact(*this, TEXT("timeout.kill_pending_ms_short_wait"), EVerdict::Recorded, Describe(Short, ShortWaitMs), KillSpec,
		TEXT("the control for timeout.kill_pending_ms: the same kill with a 100 ms wait"));
	Fact(*this, TEXT("timeout.kill_pending_sequence"), EVerdict::Recorded, Long.Sequence.IsEmpty() ? FString(TEXT("not measured")) : Long.Sequence,
		TEXT("none: the engine reads WSI_DESTROY before an open as a timeout (LwsWebSocket.cpp:392-405)"), TEXT("the callbacks after the kill (the long-wait attempt)"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeBlackholeTest, "Probe.VaRest.Lws.Timeout.Blackhole",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeBlackholeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeServiceTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// 192.0.2.1 is TEST-NET-1 (RFC 5737): never assigned, so a SYN to it usually goes unanswered. A host without a route
	// fails at once instead, and the facts say so.
	const FHang Configured = Hang(Site, 5, TEXT("192.0.2.1"), 80, 40.0);
	NoteOutcome(*this, TEXT("the connect to TEST-NET-1 with timeout_secs 5 ended within 40 s"), Configured.bEnded);
	Fact(*this, TEXT("timeout.tcp_connect_blackhole"), EVerdict::Recorded, DescribeBlackhole(Configured, 40.0),
		TEXT("WebSocket spec 2.2: whether a set timeout_secs bounds a TCP connect that is never answered"),
		TEXT("timeout_secs = 5; TCP to 192.0.2.1:80; lws_service waiting up to 100 ms"));
	// WebSocket spec 16.4: "default timeout_secs (blackhole address)" -- LWS's default against the kernel's SYN retries
	const FHang Default = Hang(Site, 0, TEXT("192.0.2.1"), 80, 60.0);
	NoteOutcome(*this, TEXT("the connect to TEST-NET-1 with LWS's default timeout ended within 60 s"), Default.bEnded);
	Fact(*this, TEXT("timeout.default_secs_blackhole"), EVerdict::Recorded, DescribeBlackhole(Default, 60.0),
		TEXT("WebSocket spec 0.3 (inferred from 4.3) and 16.4: LWS's default timeout_secs, on a blackhole address"),
		TEXT("timeout_secs = 0; TCP to 192.0.2.1:80; lws_service waiting up to 100 ms"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build and run the group**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Service && cp /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-service.log && Tools/run_tests.sh --no-build Probe.VaRest.Lws.Timeout`
Expected: `run_tests: 3 passed, 0 failed`, then `run_tests: 4 passed, 0 failed` (the timeout group takes up to three minutes). A test fails only on a harness invariant (decision 3): the server was not found, a rig did not start, or the precondition echo connection did not open.

Check the facts: `grep -c 'LogVaRestLwsProbe: Display: FACT {"id":"\(env\|release\|coexist\|svc\)\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-service.log` prints `16`, and `grep -c 'LogVaRestLwsProbe: Display: FACT {"id":"timeout\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `7`.

Control: open the `svc.commands_inside_wait_cancelled` and `svc.commands_between_service_calls` lines. At least one of them shows `established=yes echoed=yes ended=yes`; if neither does, the rig is broken, so stop and fix it before recording anything. If only the "between" mode works, set `FRigOptions::bCommandsInsideCallback`'s default to `false` in `VaRestLwsProbe.h` (the probe then measures everything the engine's way, Task 4 Step 7), re-run Task 4's rig tests, and note it in the task report: it is a finding for Task 11 (E2).

Second control (C-1): `svc.kill_async_ms_short_wait` should read well under the long-wait row. If both are large, the delay is LWS's own, not the wait, and Task 11's E3 must not be applied.

- [ ] **Step 4: Commit**

If Step 3 flipped `bCommandsInsideCallback`'s default, also stage the header: add `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbe.h` to the `git add` line below (re-review N-2).

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeServiceTest.cpp
git commit -m "tests: LWS 3.0 probe -- environment, command path, timers, forced closes with short-wait controls, handshake and blackhole timeouts (WS W0)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 6: Probe — handshake: subprotocols, origin, headers, refusals, redirects

**Files:**
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeHandshakeTest.cpp`

**Interfaces:**
- Consumes (Task 4): `FRig`, `FRigOptions` (`ContextOptions`, `Tls`, `ServiceTimeoutMs`, `OnLoadExtraClientVerifyCerts`), `ETls::LwsGlobalInit`, `FConnectSpec` (`Protocols`, `LocalProtocolName`, `Origin`, `AppendHeaders`, `Address`, `Port`, `Host`, `bTls`, `SslFlags`, `Hook`), `FHook`, `FConn::Counters`, `Visit`, `Opened`, `ErrorOf`, `ConnectResult`, `CallSequence`, `OfReason`, `ReasonName`, `Utf8Text`, `FetchJournal`, `JournalEvents`, `JournalHeader`, `FetchBytes`, `AddPemToStore`, `UrlValue`, `Fact`, `Pending`, `AgreesIf`, `YesNo`, `NoteOutcome`, `KeepDefault`, `Marker`, `ConnectReturned`, flags `UserIsConnection`, `WsiUserIsConnection`; server routes `/ws/protocol`, `/ws/echo`, `/ws/status/<code>`, `/ws/noupgrade`, `/ws/broken`, `/ws/hang`, `/tls/ca.pem`.
- Produces facts: `hs.unregistered_selected_bound`, `hs.selected_protocol_in_filter`, `hs.unregistered_selected_unbound`, `hs.unoffered_selected`, `hs.none_selected`, `hs.offered_header`, `hs.raw_origin`, `hs.default_origin`, `hs.origin_omitted`, `hs.append_header_space`, `hs.append_header_user`, `hs.append_header_fill`, `hs.host_header`, `hs.refusal_401`, `hs.refusal_403`, `hs.refusal_404`, `hs.refusal_429`, `hs.refusal_500`, `hs.refusal_503`, `hs.refusal_sequence`, `hs.callback_user_by_reason`, `hs.no_upgrade_response`, `hs.bad_accept`, `hs.missing_upgrade`, `hs.missing_connection`, `hs.connect_refused`, `hs.dns_failure`, `hs.redirect_relative`, `hs.redirect_absolute`, `hs.redirect_reappends_headers`, `hs.redirect_to_tls`, `hs.redirect_downgrade`, `hs.redirect_target_visible`, `hs.redirect_veto`, `hs.redirect_host_header`; and, one per risky test, `hs.status_in_connection_error.refused`, `.connect_refused`, `.dns_failure`, `.no_upgrade`, `.tls_verify_failure`, `.killed_pending`.
- Tests: `Probe.VaRest.Lws.Handshake.Subprotocols`, `.Handshake.OriginAndHeaders`, `.Handshake.Refusals`, `.Handshake.Redirects`, and six risky tests `Probe.VaRest.Risky.Lws.StatusInConnectionError.{Refused,ConnectRefused,DnsFailure,NoUpgrade,TlsVerifyFailure,KilledPending}`, each run on its own (decision 10, review C-2: a crash ends the run, so one case per invocation).

- [ ] **Step 1: Run the filter to see it match nothing**

Run: `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Handshake`
Expected: exit 3, `matched no tests`.

- [ ] **Step 2: Write the probe tests**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeHandshakeTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): what LWS 3.0 does with subprotocols, the Origin, appended headers, refused and broken
// upgrades and redirects (WebSocket spec 2.2). Library outcomes are facts, never failed assertions (decision 3, review
// M-2): a test fails only when the harness did not run.

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "Misc/AutomationTest.h"

namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate
{
	/** ESTABLISHED's note ("bound=<protocol>"), or empty */
	FString BoundNote(const TArray<FEvent>& Seen)
	{
		const FEvent* Open = Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; });
		return Open ? Open->Note : FString();
	}

	FString FilterNote(const TArray<FEvent>& Seen)
	{
		const FEvent* Filter = Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH; });
		return Filter ? Filter->Note : FString(TEXT("(no FILTER_PRE_ESTABLISH)"));
	}

	/** "established, bound=<p>" or "refused: <error>" */
	FString Outcome(const TArray<FEvent>& Seen)
	{
		return Opened(Seen) ? FString::Printf(TEXT("established, %s"), *BoundNote(Seen)) : FString::Printf(TEXT("refused: %s"), *ErrorOf(Seen));
	}

	/** The notes of the hook's marker events that start with Prefix */
	TArray<FString> Marks(const TArray<FEvent>& Seen, const TCHAR* Prefix)
	{
		TArray<FString> Out;
		for (const FEvent& Each : Seen)
		{
			if (Each.Reason == Marker && Each.Note.StartsWith(Prefix, ESearchCase::CaseSensitive))
			{
				Out.Add(Each.Note);
			}
		}
		return Out;
	}

	/**
	 * Whether the redirect was followed (review M-1): a second request at the server, or at the client a second
	 * APPEND_HANDSHAKE_HEADER, or -- when the original connection was plain -- a certificate check or a TLS error. A
	 * followed wss redirect can fail its TLS before any request reaches the server: the review (M-1) reads
	 * lws_client_reset as keeping only the scheme's SSL bit, so SslFlags (ALLOW_SELFSIGNED, the host-name skip) do not
	 * carry over, and the name checked becomes the bare address
	 */
	FString Redirected(const TArray<FEvent>& Seen, const FVaRestJson& Journal, bool bOriginalTls)
	{
		const TArray<FVaRestJson> Second = JournalEvents(Journal, TEXT("request"), 2);
		const int32 Appends = OfReason(Seen, LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER).Num();
		const int32 Verifies = bOriginalTls ? 0 : OfReason(Seen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION).Num();
		const FString Error = ErrorOf(Seen);
		// FString::Contains ignores case by default
		const bool bTlsError = !bOriginalTls && (Error.Contains(TEXT("ssl")) || Error.Contains(TEXT("tls")) || Error.Contains(TEXT("cert")));
		const FString Client = FString::Printf(TEXT("APPEND_HANDSHAKE_HEADER %d times, %d certificate checks after a plain start; the client %s"), Appends, Verifies, *Outcome(Seen));
		if (Second.Num() > 0)
		{
			return FString::Printf(TEXT("followed: second request %s; %s"), *Second[0][TEXT("target")].AsString(), *Client);
		}
		if (Appends > 1 || Verifies > 0 || bTlsError)
		{
			return FString::Printf(TEXT("followed, and failed before a second request reached the server; %s"), *Client);
		}
		return FString::Printf(TEXT("not followed (no second request, no second attempt at the client); %s"), *Client);
	}

	FString RedirectPath(const FString& Location, const FString& Key)
	{
		return FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *UrlValue(Location), *Key);
	}

	/** One client stash token, as a callback reads it with lws_hdr_copy (libwebsockets.h:4071-4073, :4111, :4139) */
	FString StashToken(lws* Wsi, lws_token_indexes Token)
	{
		char Buffer[512] = {};
		const int Total = lws_hdr_total_length(Wsi, Token);
		if (Total <= 0 || Total >= static_cast<int>(sizeof(Buffer)))
		{
			return FString::Printf(TEXT("(%d bytes)"), Total);
		}
		lws_hdr_copy(Wsi, Buffer, static_cast<int>(sizeof(Buffer)), Token);
		return Utf8Text(Buffer, FCStringAnsi::Strlen(Buffer));
	}

	/**
	 * Records what each APPEND_HANDSHAKE_HEADER can see of the target (review I-5) as a marker "append <n>: ...", and
	 * optionally vetoes (ruling Q2: a redirected connection fails at the first callback that can): returns 1 in the
	 * VetoAppend-th APPEND_HANDSHAKE_HEADER (0: never; libwebsockets.h:1316-1343 shows a non-zero return failing the
	 * handshake), or in FILTER_PRE_ESTABLISH once a second APPEND_HANDSHAKE_HEADER ran (bVetoFilter)
	 */
	FHook TargetHook(int32 VetoAppend, bool bVetoFilter)
	{
		return [VetoAppend, bVetoFilter](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
		{
			const FName AppendsKey(TEXT("appends"));
			if (Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER && Wsi)
			{
				int64& Appends = C.Counters.FindOrAdd(AppendsKey);
				++Appends;
				C.Rig->Record(C.Id, Marker, Appends, FString::Printf(TEXT("append %lld: host=%s uri=%s peer=%s"), Appends,
					*StashToken(Wsi, _WSI_TOKEN_CLIENT_HOST), *StashToken(Wsi, _WSI_TOKEN_CLIENT_URI), *StashToken(Wsi, _WSI_TOKEN_CLIENT_PEER_ADDRESS)));
				if (Appends == VetoAppend)
				{
					C.Rig->Record(C.Id, Marker, Appends, TEXT("veto in APPEND_HANDSHAKE_HEADER"));
					return 1;
				}
			}
			if (Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH && bVetoFilter && C.Counters.FindRef(AppendsKey) >= 2)
			{
				C.Rig->Record(C.Id, Marker, 0, TEXT("veto in FILTER_PRE_ESTABLISH"));
				return 1;
			}
			return KeepDefault;
		};
	}

	/** A veto run: where the hook vetoed, what the client saw, and whether a second request reached the server */
	FString Vetoed(const TArray<FEvent>& Seen, const FVaRestJson& Journal)
	{
		const TArray<FString> Vetoes = Marks(Seen, TEXT("veto "));
		if (Vetoes.Num() == 0)
		{
			return FString::Printf(TEXT("not exercised: the hook never vetoed (the redirect was not followed); the client %s"), *Outcome(Seen));
		}
		return FString::Printf(TEXT("%s: the client %s; a second request %s the server"), *Vetoes[0], *Outcome(Seen),
			JournalEvents(Journal, TEXT("request"), 2).Num() > 0 ? TEXT("reached") : TEXT("did not reach"));
	}

	/** The Host header of a journal's ConnIndex-th request, or "(no request)" */
	FString HostOf(const FVaRestJson& Journal, int32 ConnIndex)
	{
		if (JournalEvents(Journal, TEXT("request"), ConnIndex).Num() == 0)
		{
			return TEXT("(no request)");
		}
		return TEXT("Host: ") + JournalHeader(Journal, TEXT("Host"), ConnIndex);
	}

	/** For each reason, whether user and lws_wsi_user(wsi) were the connection's userdata (the first event of that reason) */
	FString UserByReason(const TArray<FEvent>& Seen)
	{
		const int32 Reasons[] = {
			LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER, LWS_CALLBACK_CLIENT_CONNECTION_ERROR, LWS_CALLBACK_CLIENT_ESTABLISHED,
			LWS_CALLBACK_CLOSED, LWS_CALLBACK_CLIENT_CLOSED, LWS_CALLBACK_WSI_DESTROY,
		};
		TArray<FString> Parts;
		for (const int32 Reason : Reasons)
		{
			const FEvent* First = Seen.FindByPredicate([Reason](const FEvent& Each) { return Each.Reason == Reason; });
			Parts.Add(First
				? FString::Printf(TEXT("%s user=%s wsi_user=%s"), *ReasonName(Reason), YesNo((First->Flags & UserIsConnection) != 0), YesNo((First->Flags & WsiUserIsConnection) != 0))
				: FString::Printf(TEXT("%s not seen"), *ReasonName(Reason)));
		}
		return FString::Join(Parts, TEXT("; "));
	}

	/** The claim every StatusInConnectionError case tests */
	const TCHAR* const StatusSpec = TEXT("WebSocket spec 0.3 (inferred from 4.3) and 2.2: lws_http_client_http_response(wsi) gives the upgrade's status inside CONNECTION_ERROR (documented for ESTABLISHED_CLIENT_HTTP only, libwebsockets.h:3595-3607)");

	/**
	 * Calls lws_http_client_http_response(wsi) in CONNECTION_ERROR and records what it returned. Without a header table
	 * it dereferences NULL (client.c.o 0x1294-0x12b2, review C-2): if the process ends here, the PENDING line makes the
	 * passport say so, and the case's own run (decision 10) keeps every other case's fact
	 */
	FHook StatusHook()
	{
		return [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
		{
			if (Reason == LWS_CALLBACK_CLIENT_CONNECTION_ERROR && Wsi)
			{
				C.Rig->Record(C.Id, Marker, static_cast<int64>(lws_http_client_http_response(Wsi)), TEXT("status in CONNECTION_ERROR"));
			}
			return KeepDefault;
		};
	}

	/** What the call returned inside CONNECTION_ERROR, or -1 when CONNECTION_ERROR never came */
	int64 StatusMarked(const TArray<FEvent>& Seen)
	{
		const FEvent* Marked = Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker; });
		return Marked ? Marked->Len : -1;
	}

	/** One case: agrees when the call returned Expected (0: no status line was parsed); unverified when CONNECTION_ERROR never came */
	void StatusFact(FAutomationTestBase& Test, const TCHAR* Id, const TArray<FEvent>& Seen, int64 Expected, const TCHAR* Evidence)
	{
		const int64 Got = StatusMarked(Seen);
		const FString Observed = Got < 0
			? FString::Printf(TEXT("CONNECTION_ERROR never came (%s)"), *CallSequence(Seen))
			: FString::Printf(TEXT("returned %lld inside CONNECTION_ERROR \"%s\"; the process survived"), Got, *ErrorOf(Seen));
		Fact(Test, Id, Got < 0 ? EVerdict::Unverified : AgreesIf(Got == Expected), Observed, StatusSpec,
			FString::Printf(TEXT("%s; expected %lld"), Evidence, Expected));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeSubprotocolsTest, "Probe.VaRest.Lws.Handshake.Subprotocols",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeSubprotocolsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// Only "varest" is registered (FRig's protocol table); the server selects names the context does not know
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyBound = NewKey();
	FConnectSpec Bound;
	Bound.Path = TEXT("/ws/protocol?select=chat.v2&key=") + KeyBound;
	Bound.Protocols = TEXT("chat.v2, chat.v1");
	FConnectSpec Unbound = Bound;
	Unbound.Path = TEXT("/ws/protocol?select=chat.v2&key=") + NewKey();
	Unbound.LocalProtocolName.Reset();
	FConnectSpec Unoffered;
	Unoffered.Path = TEXT("/ws/protocol?select=evil&key=") + NewKey();
	Unoffered.Protocols = TEXT("chat.v2");
	FConnectSpec NoneSelected;
	NoneSelected.Path = TEXT("/ws/protocol?select=&key=") + NewKey();
	NoneSelected.Protocols = TEXT("chat.v2");
	TArray<FEvent> BoundSeen;
	TArray<FEvent> UnboundSeen;
	TArray<FEvent> UnofferedSeen;
	TArray<FEvent> NoneSeen;
	// A connection that never ends is noted by Visit, not failed (review M-2)
	Visit(*this, Rig, Bound, BoundSeen);
	Visit(*this, Rig, Unbound, UnboundSeen);
	Visit(*this, Rig, Unoffered, UnofferedSeen);
	Visit(*this, Rig, NoneSelected, NoneSeen);

	Fact(*this, TEXT("hs.unregistered_selected_bound"), AgreesIf(Opened(BoundSeen) && BoundNote(BoundSeen).Equals(TEXT("bound=varest"), ESearchCase::CaseSensitive)),
		Outcome(BoundSeen),
		TEXT("WebSocket spec 0.3 (inferred from 4.3) and 2.2: a server-selected subprotocol unknown to the context is accepted when local_protocol_name pre-binds the connection"),
		TEXT("offered \"chat.v2, chat.v1\", the server selected chat.v2, only \"varest\" registered, local_protocol_name = \"varest\""));
	const FString Filter = FilterNote(BoundSeen);
	Fact(*this, TEXT("hs.selected_protocol_in_filter"), AgreesIf(Filter.StartsWith(TEXT("protocol=chat.v2 "), ESearchCase::CaseSensitive)), Filter,
		TEXT("T6: the selected protocol is read with lws_hdr_copy(WSI_TOKEN_PROTOCOL) in CLIENT_FILTER_PRE_ESTABLISH"),
		TEXT("the bound connection's FILTER_PRE_ESTABLISH"));
	Fact(*this, TEXT("hs.unregistered_selected_unbound"), EVerdict::Recorded, Outcome(UnboundSeen),
		TEXT("none: the engine's way, .protocol both binds and offers (LwsWebSocket.cpp:805)"), TEXT("the same, local_protocol_name = NULL"));
	Fact(*this, TEXT("hs.unoffered_selected"), EVerdict::Recorded, Outcome(UnofferedSeen),
		TEXT("none: RFC 6455 4.1 says the client fails a connection whose selected protocol it did not offer"), TEXT("offered chat.v2, the server selected evil"));
	Fact(*this, TEXT("hs.none_selected"), EVerdict::Recorded, Outcome(NoneSeen),
		TEXT("none"), TEXT("offered chat.v2, the server selected none"));
	const FVaRestJson Journal = FetchJournal(*this, Site, KeyBound);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	Fact(*this, TEXT("hs.offered_header"), EVerdict::Recorded, TEXT("Sec-WebSocket-Protocol: ") + JournalHeader(Journal, TEXT("Sec-WebSocket-Protocol")),
		TEXT("T3: .protocol is the list of protocols offered"), TEXT("what the server received for .protocol = \"chat.v2, chat.v1\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeOriginAndHeadersTest, "Probe.VaRest.Lws.Handshake.OriginAndHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeOriginAndHeadersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Raw(Site);
	FRigOptions PlainOptions;
	PlainOptions.ContextOptions &= ~static_cast<uint32>(LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN);
	FRig Plain(Site, MoveTemp(PlainOptions));
	if (!TestTrue(TEXT("both probe contexts exist"), Raw.IsReady() && Plain.IsReady()))
	{
		return false;
	}
	const FString KeyRaw = NewKey();
	const FString KeyPlain = NewKey();
	const FString KeyNone = NewKey();
	const FString KeyFill = NewKey();
	FConnectSpec RawSpec;
	RawSpec.Path = TEXT("/ws/echo?key=") + KeyRaw;
	RawSpec.Origin = FString(TEXT("https://game.example:8443"));
	FConnectSpec PlainSpec;
	PlainSpec.Path = TEXT("/ws/echo?key=") + KeyPlain;
	PlainSpec.Origin = FString(TEXT("game.example"));
	FConnectSpec NoneSpec;
	NoneSpec.Path = TEXT("/ws/echo?key=") + KeyNone;
	TArray<FEvent> RawSeen;
	TArray<FEvent> PlainSeen;
	TArray<FEvent> NoneSeen;
	Visit(*this, Raw, RawSpec, RawSeen);
	Visit(*this, Plain, PlainSpec, PlainSeen);
	Visit(*this, Raw, NoneSpec, NoneSeen);

	// The space APPEND_HANDSHAKE_HEADER reports with the context's 32 KiB header buffer, then a header that fills it but 512 bytes
	const TArray<FEvent> Appends = OfReason(RawSeen, LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER);
	const int64 Space = Appends.Num() > 0 ? Appends[0].Len : -1;
	const uint32 AppendFlags = Appends.Num() > 0 ? Appends[0].Flags : 0u;
	const int32 FillBytes = Space > 1024 ? static_cast<int32>(Space - 512) : 0;
	FConnectSpec FillSpec;
	FillSpec.Path = TEXT("/ws/echo?key=") + KeyFill;
	if (FillBytes > 0)
	{
		FillSpec.AppendHeaders.Add(TEXT("X-Fill: ") + FString::ChrN(FillBytes, TEXT('a')));
	}
	FillSpec.AppendHeaders.Add(TEXT("Authorization: Bearer probe-fill-token"));
	TArray<FEvent> FillSeen;
	Visit(*this, Raw, FillSpec, FillSeen);
	const FVaRestJson RawJournal = FetchJournal(*this, Site, KeyRaw);
	const FVaRestJson PlainJournal = FetchJournal(*this, Site, KeyPlain);
	const FVaRestJson NoneJournal = FetchJournal(*this, Site, KeyNone);
	const FVaRestJson FillJournal = FetchJournal(*this, Site, KeyFill);
	if (!TestTrue(TEXT("every server journal arrived"), RawJournal.IsValid() && PlainJournal.IsValid() && NoneJournal.IsValid() && FillJournal.IsValid()))
	{
		return false;
	}

	const FString RawOrigin = JournalHeader(RawJournal, TEXT("Origin"));
	Fact(*this, TEXT("hs.raw_origin"), AgreesIf(RawOrigin.Equals(TEXT("https://game.example:8443"), ESearchCase::CaseSensitive)), TEXT("Origin: ") + RawOrigin,
		TEXT("T4: with LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN, .origin is sent as given (libwebsockets.h:2618-2624)"),
		TEXT("a context with the option; .origin = \"https://game.example:8443\""));
	Fact(*this, TEXT("hs.default_origin"), EVerdict::Recorded, TEXT("Origin: ") + JournalHeader(PlainJournal, TEXT("Origin")),
		TEXT("WebSocket spec E20: without the option LWS prefixes http:// (the engine sets no raw-origin option, LwsWebSocketsManager.cpp:184)"),
		TEXT("a context without the option; .origin = \"game.example\""));
	Fact(*this, TEXT("hs.origin_omitted"), EVerdict::Recorded, TEXT("Origin: ") + JournalHeader(NoneJournal, TEXT("Origin")),
		TEXT("WebSocket spec D29: WithOrigin({}) omits it"), TEXT(".origin = NULL"));
	Fact(*this, TEXT("hs.append_header_space"), AgreesIf(Space > 30000), FString::Printf(TEXT("len = %lld bytes"), Space),
		TEXT("T5 and WebSocket spec 15: len is the space left in the 32 KiB handshake header buffer"),
		TEXT("APPEND_HANDSHAKE_HEADER with max_http_header_data2 = pt_serv_buf_size = 32 KiB"));
	Fact(*this, TEXT("hs.append_header_user"), EVerdict::Recorded,
		FString::Printf(TEXT("the user argument %s the connection's userdata; lws_wsi_user(wsi) %s"),
			(AppendFlags & UserIsConnection) != 0 ? TEXT("is") : TEXT("is not"), (AppendFlags & WsiUserIsConnection) != 0 ? TEXT("is") : TEXT("is not")),
		TEXT("libwebsockets.h:1316-1343: user is NULL and the callback goes to protocols[0]; the engine takes its socket from user there (LwsWebSocketsManager.cpp:434, :470, :477)"),
		TEXT("the first APPEND_HANDSHAKE_HEADER of a connection"));
	const FString Fill = JournalHeader(FillJournal, TEXT("X-Fill"));
	const FString Bearer = JournalHeader(FillJournal, TEXT("Authorization"));
	const bool bFilled = FillBytes > 0 && Fill.Len() == FillBytes && Bearer.Equals(TEXT("Bearer probe-fill-token"), ESearchCase::CaseSensitive) && Opened(FillSeen);
	Fact(*this, TEXT("hs.append_header_fill"), AgreesIf(bFilled),
		FString::Printf(TEXT("an X-Fill value of %d bytes arrived as %d, Authorization \"%s\"; %s"), FillBytes, Fill.Len(), *Bearer, *Outcome(FillSeen)),
		TEXT("WebSocket spec 2.2: headers appended in APPEND_HANDSHAKE_HEADER, the remaining length checked per header (T5)"),
		TEXT("two headers filling all but 512 bytes of the reported len"));
	Fact(*this, TEXT("hs.host_header"), EVerdict::Recorded, TEXT("Host: ") + JournalHeader(RawJournal, TEXT("Host")),
		TEXT("WebSocket spec 2.2: Host keeps a non-default port"), TEXT(".host = \"<address>:<port>\""));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRefusalsTest, "Probe.VaRest.Lws.Handshake.Refusals",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	struct FRefusal
	{
		int32 Code;
		const TCHAR* Expected;
	};
	const FRefusal Cases[] = {
		{ 401, TEXT("HS: ws upgrade unauthorized") },
		{ 403, TEXT("HS: ws upgrade forbidden") },
		{ 404, TEXT("HS: ws upgrade not 101, was 404") },
		{ 429, TEXT("HS: ws upgrade not 101, was 429") },
		{ 500, TEXT("HS: ws upgrade not 101, was 500") },
		{ 503, TEXT("HS: ws upgrade not 101, was 503") },
	};
	TArray<FEvent> RefusedSeen403;
	for (const FRefusal& Case : Cases)
	{
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/status/%d?key=%s"), Case.Code, *NewKey());
		TArray<FEvent> Seen;
		Visit(*this, Rig, MoveTemp(Spec), Seen);
		const FString Error = ErrorOf(Seen);
		Fact(*this, FString::Printf(TEXT("hs.refusal_%d"), Case.Code), AgreesIf(Error.Equals(Case.Expected, ESearchCase::CaseSensitive)), Error,
			FString::Printf(TEXT("T7: \"%s\" (the strings in libwebsockets.a)"), Case.Expected), FString::Printf(TEXT("the upgrade answered %d"), Case.Code));
		if (Case.Code == 403)
		{
			RefusedSeen403 = Seen;
		}
	}
	Fact(*this, TEXT("hs.refusal_sequence"), EVerdict::Recorded, CallSequence(RefusedSeen403), TEXT("none"), TEXT("the callbacks of the 403 attempt"));

	// Review M-14: which callbacks carry the connection in user. The engine takes its socket from user for every
	// connection reason it handles, WSI_DESTROY included (LwsWebSocketsManager.cpp:434, :458-461, :463-477), except the
	// certificate check, where user is the X509_STORE_CTX and the socket comes from lws_wsi_user (:447-456)
	FConnectSpec Echo;
	Echo.Path = TEXT("/ws/echo?key=") + NewKey();
	TArray<FEvent> EchoSeen;
	Visit(*this, Rig, MoveTemp(Echo), EchoSeen);
	TArray<FEvent> Both = RefusedSeen403;
	Both.Append(EchoSeen);
	Fact(*this, TEXT("hs.callback_user_by_reason"), EVerdict::Recorded, UserByReason(Both),
		TEXT("none: libwebsockets.h:1316-1343 says user is NULL in APPEND_HANDSHAKE_HEADER; W1 finds its socket per reason from this row"),
		TEXT("the first event of each reason across the 403 attempt and an echo connection closed with 1000"));

	struct FBroken
	{
		const TCHAR* Id;
		const TCHAR* Path;
		const TCHAR* Expected;
	};
	const FBroken Brokens[] = {
		{ TEXT("hs.bad_accept"), TEXT("/ws/broken?what=accept"), TEXT("HS: Accept hash wrong") },
		{ TEXT("hs.missing_upgrade"), TEXT("/ws/broken?what=upgrade"), TEXT("HS: UPGRADE missing") },
		{ TEXT("hs.missing_connection"), TEXT("/ws/broken?what=connection"), TEXT("HS: CONNECTION missing") },
	};
	for (const FBroken& Case : Brokens)
	{
		FConnectSpec Spec;
		Spec.Path = Case.Path;
		TArray<FEvent> Seen;
		Visit(*this, Rig, MoveTemp(Spec), Seen);
		const FString Error = ErrorOf(Seen);
		Fact(*this, Case.Id, AgreesIf(Error.Equals(Case.Expected, ESearchCase::CaseSensitive)), Error,
			FString::Printf(TEXT("WebSocket spec 2.2: \"%s\" is among the parser tests' strings"), Case.Expected), Case.Path);
	}

	FConnectSpec NoUpgrade;
	NoUpgrade.Path = TEXT("/ws/noupgrade");
	TArray<FEvent> NoUpgradeSeen;
	Visit(*this, Rig, MoveTemp(NoUpgrade), NoUpgradeSeen);
	Fact(*this, TEXT("hs.no_upgrade_response"), EVerdict::Recorded,
		FString::Printf(TEXT("\"%s\" (connect returned %s)"), *ErrorOf(NoUpgradeSeen), *ConnectResult(NoUpgradeSeen)),
		TEXT("WebSocket spec 2.2 and 22 A2: NoUpgradeResponse, upgrade sent and the connection closed before any status line; W0 pins the string"),
		TEXT("/ws/noupgrade reads the request and ends TCP"));

	FConnectSpec Refused;
	Refused.Address = TEXT("127.0.0.1");
	Refused.Port = 1;
	TArray<FEvent> ConnectRefusedSeen;
	Visit(*this, Rig, MoveTemp(Refused), ConnectRefusedSeen);
	Fact(*this, TEXT("hs.connect_refused"), EVerdict::Recorded,
		FString::Printf(TEXT("\"%s\" (connect returned %s)"), *ErrorOf(ConnectRefusedSeen), *ConnectResult(ConnectRefusedSeen)),
		TEXT("WebSocket spec 2.2: \"Could not initialize connection\" is the engine's own text for a null connect (LwsWebSocket.cpp:813-824)"),
		TEXT("TCP to 127.0.0.1:1, where nothing listens"));

	FConnectSpec Unknown;
	Unknown.Address = TEXT("nonexistent.invalid");
	Unknown.Port = 80;
	Unknown.Host = TEXT("nonexistent.invalid");
	TArray<FEvent> UnknownSeen;
	Visit(*this, Rig, MoveTemp(Unknown), UnknownSeen);
	Fact(*this, TEXT("hs.dns_failure"), EVerdict::Recorded,
		FString::Printf(TEXT("\"%s\" (connect returned %s)"), *ErrorOf(UnknownSeen), *ConnectResult(UnknownSeen)),
		TEXT("WebSocket spec 3.4: a DNS failure reconnects"), TEXT("nonexistent.invalid (RFC 6761: never resolves)"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRedirectsTest, "Probe.VaRest.Lws.Handshake.Redirects",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRedirectsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	TArray<uint8> Ca;
	if (!FetchBytes(*this, Site, TEXT("/tls/ca.pem"), Ca))
	{
		return false;
	}
	// TLS in the context, trusting the test CA, so a redirect to wss can be followed and verified
	FRigOptions Options;
	Options.Tls = ETls::LwsGlobalInit;
	Options.OnLoadExtraClientVerifyCerts = [Ca](SSL_CTX* Ctx) { AddPemToStore(Ctx, Ca); };
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyRelative = NewKey();
	const FString KeyAbsolute = NewKey();
	const FString KeyToTls = NewKey();
	const FString KeyDown = NewKey();
	const FString KeyVetoAppend = NewKey();
	const FString KeyVetoFilter = NewKey();
	FConnectSpec Relative;
	Relative.Path = RedirectPath(TEXT("/ws/echo?key=") + KeyRelative, KeyRelative);
	Relative.AppendHeaders.Add(TEXT("Authorization: Bearer redirect-probe-token"));
	Relative.Hook = TargetHook(0, false);
	FConnectSpec Absolute;
	Absolute.Path = RedirectPath(FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *KeyAbsolute), KeyAbsolute);
	Absolute.Hook = TargetHook(0, false);
	FConnectSpec ToTls;
	ToTls.Path = RedirectPath(FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *KeyToTls), KeyToTls);
	ToTls.Hook = TargetHook(0, false);
	FConnectSpec Down;
	Down.bTls = true;
	Down.SslFlags = LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	Down.Path = RedirectPath(FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *KeyDown), KeyDown);
	// Ruling Q2's two veto points, on the relative redirect: the redirected request's APPEND_HANDSHAKE_HEADER, and its FILTER_PRE_ESTABLISH
	FConnectSpec VetoAppend;
	VetoAppend.Path = RedirectPath(TEXT("/ws/echo?key=") + KeyVetoAppend, KeyVetoAppend);
	VetoAppend.Hook = TargetHook(2, false);
	FConnectSpec VetoFilter;
	VetoFilter.Path = RedirectPath(TEXT("/ws/echo?key=") + KeyVetoFilter, KeyVetoFilter);
	VetoFilter.Hook = TargetHook(0, true);
	TArray<FEvent> RelativeSeen;
	TArray<FEvent> AbsoluteSeen;
	TArray<FEvent> ToTlsSeen;
	TArray<FEvent> DownSeen;
	TArray<FEvent> VetoAppendSeen;
	TArray<FEvent> VetoFilterSeen;
	Visit(*this, Rig, Relative, RelativeSeen);
	Visit(*this, Rig, Absolute, AbsoluteSeen);
	Visit(*this, Rig, ToTls, ToTlsSeen);
	Visit(*this, Rig, Down, DownSeen);
	Visit(*this, Rig, VetoAppend, VetoAppendSeen);
	Visit(*this, Rig, VetoFilter, VetoFilterSeen);
	// Every client connection has ended or been noted: a second request that has not arrived within 3 s is not coming
	const FVaRestJson RelativeJournal = FetchJournal(*this, Site, KeyRelative, 2, 3000);
	const FVaRestJson AbsoluteJournal = FetchJournal(*this, Site, KeyAbsolute, 2, 3000);
	const FVaRestJson ToTlsJournal = FetchJournal(*this, Site, KeyToTls, 2, 3000);
	const FVaRestJson DownJournal = FetchJournal(*this, Site, KeyDown, 2, 3000);
	const FVaRestJson VetoAppendJournal = FetchJournal(*this, Site, KeyVetoAppend, 2, 3000);
	const FVaRestJson VetoFilterJournal = FetchJournal(*this, Site, KeyVetoFilter, 2, 3000);
	if (!TestTrue(TEXT("every server journal arrived"), RelativeJournal.IsValid() && AbsoluteJournal.IsValid() && ToTlsJournal.IsValid()
		&& DownJournal.IsValid() && VetoAppendJournal.IsValid() && VetoFilterJournal.IsValid()))
	{
		return false;
	}
	const FString RedirectSpec = TEXT("WebSocket spec 3.4 row \"upgrade 3xx (redirect not followed or failed)\" and ruling Q2 (2026-10-21: VaRest never follows an upgrade redirect); libwebsockets.a holds \"HS: Redirect failed\" and lws_client_reset");
	Fact(*this, TEXT("hs.redirect_relative"), EVerdict::Recorded, Redirected(RelativeSeen, RelativeJournal, false), RedirectSpec,
		TEXT("302 with Location: /ws/echo?key=<k>"));
	Fact(*this, TEXT("hs.redirect_absolute"), EVerdict::Recorded, Redirected(AbsoluteSeen, AbsoluteJournal, false), RedirectSpec,
		TEXT("302 with Location: ws://<host>:<port>/ws/echo?key=<k>"));
	Fact(*this, TEXT("hs.redirect_reappends_headers"), EVerdict::Recorded,
		FString::Printf(TEXT("APPEND_HANDSHAKE_HEADER ran %d times; Authorization on the second request: %s"),
			OfReason(RelativeSeen, LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER).Num(), *JournalHeader(RelativeJournal, TEXT("Authorization"), 2)),
		TEXT("WebSocket spec 3.3: auth headers go on every attempt; ruling Q2: they must never reach another host"),
		TEXT("the relative redirect, with Authorization appended"));
	Fact(*this, TEXT("hs.redirect_to_tls"), EVerdict::Recorded, Redirected(ToTlsSeen, ToTlsJournal, false), RedirectSpec,
		TEXT("ws:// answered 302 with Location: wss://<host>:<tls port>/ws/echo, the test CA trusted; followed is decided from the client's events too (review M-1)"));
	Fact(*this, TEXT("hs.redirect_downgrade"), EVerdict::Recorded, Redirected(DownSeen, DownJournal, true),
		TEXT("none: libwebsockets.a holds \"HS: Redirect attempted SSL downgrade\""), TEXT("wss:// answered 302 with Location: ws://<host>:<port>/ws/echo"));

	// Review I-5: what W1 needs to write ruling Q2's rule from evidence
	const TArray<FString> RelativeTargets = Marks(RelativeSeen, TEXT("append "));
	const TArray<FString> AbsoluteTargets = Marks(AbsoluteSeen, TEXT("append "));
	const TArray<FString> ToTlsTargets = Marks(ToTlsSeen, TEXT("append "));
	Fact(*this, TEXT("hs.redirect_target_visible"), EVerdict::Recorded,
		FString::Printf(TEXT("relative: [%s]; absolute: [%s]; to wss: [%s]"), *FString::Join(RelativeTargets, TEXT(" | ")),
			*FString::Join(AbsoluteTargets, TEXT(" | ")), *FString::Join(ToTlsTargets, TEXT(" | "))),
		TEXT("ruling Q2: the appended-header callback adds nothing once the target differs from the original scheme+host+port; libwebsockets.h:4071-4073 lists the client stash tokens"),
		TEXT("lws_hdr_copy of _WSI_TOKEN_CLIENT_HOST, _URI and _PEER_ADDRESS in every APPEND_HANDSHAKE_HEADER; the plain redirects stay on the test server's host and port (one listener), so a changed uri shows whether the tokens follow a redirect"));
	const bool bAppendVetoed = Marks(VetoAppendSeen, TEXT("veto ")).Num() > 0;
	const bool bFilterVetoed = Marks(VetoFilterSeen, TEXT("veto ")).Num() > 0;
	Fact(*this, TEXT("hs.redirect_veto"),
		bAppendVetoed && bFilterVetoed ? AgreesIf(!Opened(VetoAppendSeen) && !Opened(VetoFilterSeen)) : EVerdict::Recorded,
		FString::Printf(TEXT("in APPEND_HANDSHAKE_HEADER: %s; in FILTER_PRE_ESTABLISH: %s"), *Vetoed(VetoAppendSeen, VetoAppendJournal), *Vetoed(VetoFilterSeen, VetoFilterJournal)),
		TEXT("ruling Q2: the socket fails a redirected connection at the first callback that can (FILTER_PRE_ESTABLISH or establish) with a redirect error; a non-zero return fails a handshake (libwebsockets.h:1301-1307, :1316-1343)"),
		TEXT("the relative redirect, the hook returning 1 in the second APPEND_HANDSHAKE_HEADER, and separately in FILTER_PRE_ESTABLISH after a second APPEND_HANDSHAKE_HEADER; the error string and the server's second request recorded"));
	Fact(*this, TEXT("hs.redirect_host_header"), EVerdict::Recorded,
		FString::Printf(TEXT("relative: %s (first request %s); absolute: %s (first request %s)"), *HostOf(RelativeJournal, 2), *HostOf(RelativeJournal, 1),
			*HostOf(AbsoluteJournal, 2), *HostOf(AbsoluteJournal, 1)),
		TEXT("WebSocket spec 2.2: Host keeps a non-default port; the review (I-5) reads lws_client_reset as given the address as host, dropping the port"),
		TEXT("the Host header of each redirect's first and second request at the server"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorRefusedTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.Refused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	// The status-line parser's errors: the header table is still attached when CONNECTION_ERROR comes
	Pending(TEXT("hs.status_in_connection_error.refused"));
	const int32 Codes[] = { 401, 403, 404, 503 };
	TArray<FString> Parts;
	bool bAllMatch = true;
	bool bAllCame = true;
	for (const int32 Code : Codes)
	{
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/status/%d?key=%s"), Code, *NewKey());
		Spec.Hook = StatusHook();
		TArray<FEvent> Seen;
		Visit(*this, Rig, MoveTemp(Spec), Seen);
		const int64 Got = StatusMarked(Seen);
		bAllMatch = bAllMatch && Got == Code;
		bAllCame = bAllCame && Got >= 0;
		Parts.Add(FString::Printf(TEXT("%d -> %lld"), Code, Got));
	}
	Fact(*this, TEXT("hs.status_in_connection_error.refused"), bAllCame ? AgreesIf(bAllMatch) : EVerdict::Unverified,
		FString::Printf(TEXT("%s; the process survived"), *FString::Join(Parts, TEXT(", "))), StatusSpec,
		TEXT("upgrades refused with 401, 403, 404, 503 (status-line parser errors); -1: CONNECTION_ERROR never came"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorConnectRefusedTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.ConnectRefused",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorConnectRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.connect_refused"));
	FConnectSpec Spec;
	Spec.Address = TEXT("127.0.0.1");
	Spec.Port = 1;
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	StatusFact(*this, TEXT("hs.status_in_connection_error.connect_refused"), Seen, 0, TEXT("TCP to 127.0.0.1:1, where nothing listens: no response at all"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorDnsFailureTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.DnsFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorDnsFailureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.dns_failure"));
	FConnectSpec Spec;
	Spec.Address = TEXT("nonexistent.invalid");
	Spec.Port = 80;
	Spec.Host = TEXT("nonexistent.invalid");
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	StatusFact(*this, TEXT("hs.status_in_connection_error.dns_failure"), Seen, 0, TEXT("nonexistent.invalid (RFC 6761: never resolves): no TCP, no response"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorNoUpgradeTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.NoUpgrade",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorNoUpgradeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.no_upgrade"));
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/noupgrade");
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	StatusFact(*this, TEXT("hs.status_in_connection_error.no_upgrade"), Seen, 0, TEXT("/ws/noupgrade reads the request and ends TCP before any status line"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorTlsVerifyFailureTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.TlsVerifyFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorTlsVerifyFailureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	// TLS in the context and no trust anchor at all (the rig disables the OS CAs and loads no extra one): the test CA is
	// untrusted, so the TLS handshake fails before any HTTP
	FRigOptions Options;
	Options.Tls = ETls::LwsGlobalInit;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.tls_verify_failure"));
	FConnectSpec Spec;
	Spec.bTls = true;
	Spec.Path = TEXT("/ws/echo?key=") + NewKey();
	Spec.Hook = StatusHook();
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	StatusFact(*this, TEXT("hs.status_in_connection_error.tls_verify_failure"), Seen, 0, TEXT("wss to the TLS listener with its CA untrusted: the certificate check fails, no response"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeStatusInErrorKilledPendingTest, "Probe.VaRest.Risky.Lws.StatusInConnectionError.KilledPending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeStatusInErrorKilledPendingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeHandshakeTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// The short wait, so the kill lands within a second (review C-1)
	FRigOptions Options;
	Options.ServiceTimeoutMs = 100;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Pending(TEXT("hs.status_in_connection_error.killed_pending"));
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/hang?ms=60000&key=") + NewKey();
	Spec.Hook = StatusHook();
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (Rig.WaitReason(Conn, ConnectReturned))
	{
		// The upgrade is out and unanswered by now: WebSocket spec 2.2's ConnectTimeout kill
		FPlatformProcess::Sleep(1.0f);
		Rig.ExecOn(Conn, TEXT("kill"), [](FConn& C) { lws_set_timeout(C.Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC); });
	}
	NoteOutcome(*this, TEXT("the killed attempt ended within 25 s"), Rig.WaitEnded(Conn, 25.0));
	StatusFact(*this, TEXT("hs.status_in_connection_error.killed_pending"), Rig.Events(Conn), 0,
		TEXT("an attempt to /ws/hang killed with lws_set_timeout(LWS_TO_KILL_ASYNC) 1 s after the upgrade went out: no response"));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build and run the group**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Handshake`
Expected: `run_tests: 4 passed, 0 failed`; `grep -c 'LogVaRestLwsProbe: Display: FACT {"id":"hs\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `35`. A test fails only on a harness invariant (decision 3): the server or a journal did not answer, or a rig did not start.

Controls: `hs.raw_origin` and `hs.default_origin` must differ. If they are the same string, the probe did not tell the two contexts apart: stop and find out why before recording. `hs.refusal_403`'s observed text must name 403 somehow ("forbidden" or "403"). If it does not, `ErrorOf` is reading the wrong event. `hs.redirect_target_visible` must list at least one `append 1:` entry per redirect; if it lists none, the hook did not run, so fix the rig before recording.

- [ ] **Step 4: Run each risky case on its own**

Run:

```bash
for CASE in Refused ConnectRefused DnsFailure NoUpgrade TlsVerifyFailure KilledPending; do
	Tools/run_tests.sh --no-build "Probe.VaRest.Risky.Lws.StatusInConnectionError.$CASE"
	cp /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log "/w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-risky-$CASE.log"
done
```

Expected, per case: either `run_tests: 1 passed, 0 failed` with one `FACT {"id":"hs.status_in_connection_error.<case>"` line, or `run_tests: run did not complete (crash?)` with a `PENDING {"id":"hs.status_in_connection_error.<case>"}` line and no FACT (`<case>` is `refused`, `connect_refused`, `dns_failure`, `no_upgrade`, `tls_verify_failure`, `killed_pending`). Both are results, so record which one each case gave. A crash is a valid result for this probe: do not "fix" it. The six logs are passed to the passport in Task 10.

- [ ] **Step 5: Commit**

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeHandshakeTest.cpp
git commit -m "tests: LWS 3.0 probe -- subprotocol binding, origin, appended headers, refusal and broken-upgrade strings, redirects and their veto points, status in CONNECTION_ERROR per case (WS W0)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 7: Probe — close semantics, ping and pong

**Files:**
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeCloseTest.cpp`

**Interfaces:**
- Consumes (Task 4): `FRig` (`Connect`, `Write`, `WriteText`, `Close`, `WaitFor`, `WaitReason`, `WaitEnded`, `Events`, `Find`), `FRigOptions` (`TimeoutSecs`, `ServiceTimeoutMs`), `FConnectSpec::PeerCloseReturn`, `CallSequence`, `OfReason`, `Opened`, `EventText`, `Utf8Bytes`, `HexOf`, `FetchJournal`, `JournalEvents`, `JournalBytes`, `UrlValue`, `Fact`, `AgreesIf`, `YesNo`, `NoteOutcome`, pseudo reasons `CloseRequested`, `WriteDone`; server routes `/ws/close/<code>|none|onebyte`, `/ws/echo`, `/ws/noecho`, `/ws/drop`, `/ws/serverping`, `/ws/silent`.
- Produces facts: `close.rewritten_codes`, `close.echo_on_return_0`, `close.echo_payload`, `close.reason_utf8_bytes`, `close.peer_sequence`, `close.empty_len`, `close.empty_echo`, `close.one_byte`, `close.return_1_echo`, `close.ours_echoed_ms`, `close.ours_noecho_wait_ms`, `close.ours_sequence`, `close.ours_long_reason`, `close.drop_fin_sequence`, `close.drop_rst_sequence`, `close.drop_reports_client_closed`, `ping.auto_pong`, `ping.auto_pong_latency_ms`, `ping.client_sees_server_ping`, `ping.pong_payload_echo`, `ping.rtt_ms`, `ping.max_payload_125`, `ping.unsolicited_pong_delivered`, `ping.silent_no_pong`, `ping.idle_survives_timeout_secs`.
- Tests: `Probe.VaRest.Lws.Close.PeerCodes`, `.Close.EmptyAndShort`, `.Close.PeerReturnNonZero`, `.Close.Ours`, `.Close.Drops`, `Probe.VaRest.Lws.Ping.ServerPings`, `.Ping.ClientPing`, `.Ping.Silent`.

- [ ] **Step 1: Run the filter to see it match nothing**

Run: `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Close`
Expected: exit 3, `matched no tests`.

- [ ] **Step 2: Write the probe tests**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeCloseTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): what LWS 3.0 hands over and sends when a connection closes -- the peer's codes, empty and
// short close frames, our own close against an echoing and a non-echoing server, drops -- and how it pings and pongs
// (WebSocket spec 2.5). Whether a connection ended is a library outcome, noted and never failed (decision 3, review M-2).
// Close.Ours and Ping.Silent ask what LWS's own timers do, so their rigs wait 100 ms in lws_service (review C-1: LWS
// checks its deadlines only when lws_service returns)

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate
{
	/** The PEER_INITIATED_CLOSE event of a connection, or nullptr */
	const FEvent* PeerClose(const TArray<FEvent>& Seen)
	{
		return Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_WS_PEER_INITIATED_CLOSE; });
	}

	/** The big-endian code LWS handed over, -1 when the payload has none */
	int32 ReceivedCode(const FEvent* Close)
	{
		return Close && Close->Bytes.Num() >= 2 ? (static_cast<int32>(Close->Bytes[0]) << 8) | Close->Bytes[1] : -1;
	}

	/** The client's close as the server received it ("close" in its journal): "<hex>" or "none" */
	FString ClientCloseHex(const FVaRestJson& Journal)
	{
		const TArray<FVaRestJson> Closes = JournalEvents(Journal, TEXT("close"));
		return Closes.Num() > 0 ? FString::Printf(TEXT("\"%s\""), *Closes[0][TEXT("hex")].AsString()) : FString(TEXT("none"));
	}

	/** Connects and waits until the connection ends by itself (the server closes it); false when it does not */
	bool RunToEnd(FRig& Rig, const FString& Path, TArray<FEvent>& Out, int32 PeerCloseReturn = 0, double Seconds = 20.0)
	{
		FConnectSpec Spec;
		Spec.Path = Path;
		Spec.PeerCloseReturn = PeerCloseReturn;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		const bool bEnded = Rig.WaitEnded(Conn, Seconds);
		Out = Rig.Events(Conn);
		return bEnded;
	}

	/** The callbacks from ESTABLISHED on */
	FString AfterOpen(const TArray<FEvent>& Seen)
	{
		const int32 Open = Seen.IndexOfByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; });
		return CallSequence(Open == INDEX_NONE ? Seen : TArray<FEvent>(Seen.GetData() + Open, Seen.Num() - Open));
	}

	/** A close we start on an open connection, timed from CloseRequested to WSI_DESTROY */
	struct FOurClose
	{
		bool bEnded = false;
		double Ms = -1.0;
		FString Sequence;
	};

	FOurClose CloseOurs(FRig& Rig, const FString& Path, int32 Code, const FString& Reason, double Seconds)
	{
		FOurClose Out;
		FConnectSpec Spec;
		Spec.Path = Path;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		if (!Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED))
		{
			Rig.WaitEnded(Conn);
			return Out;
		}
		Rig.Close(Conn, Code, Reason);
		Out.bEnded = Rig.WaitEnded(Conn, Seconds);
		FEvent Requested;
		FEvent Destroyed;
		if (Out.bEnded && Rig.Find(Conn, CloseRequested, Requested) && Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed))
		{
			Out.Ms = (Destroyed.Time - Requested.Time) * 1000.0;
			Out.Sequence = CallSequence(Rig.Events(Conn).FilterByPredicate([&Requested](const FEvent& Each) { return Each.Time >= Requested.Time; }));
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbePeerCodesTest, "Probe.VaRest.Lws.Close.PeerCodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbePeerCodesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	// Every code a server may send and several it must not (RFC 6455 7.4): which ones does LWS 3.0 hand over unchanged?
	const int32 Codes[] = { 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016,
		2000, 2999, 3000, 4001, 4999, 5000, 65535 };
	struct FPeer
	{
		int32 Code = 0;
		int32 Conn = 0;
		FString Key;
	};
	TArray<FPeer> Peers;
	for (const int32 Code : Codes)
	{
		FPeer& Peer = Peers.AddDefaulted_GetRef();
		Peer.Code = Code;
		Peer.Key = NewKey();
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/close/%d?reason=bye-%d&key=%s"), Code, Code, *Peer.Key);
		Peer.Conn = Rig.Connect(MoveTemp(Spec));
	}
	const FString Utf8Reason = TEXT("пока ✓");
	const FString Utf8Key = NewKey();
	FConnectSpec Utf8Spec;
	Utf8Spec.Path = FString::Printf(TEXT("/ws/close/4002?reason=%s&key=%s"), *UrlValue(Utf8Reason), *Utf8Key);
	const int32 Utf8Conn = Rig.Connect(MoveTemp(Utf8Spec));
	// Each one ends: the server closes after the client's answer or after CLOSE_WAIT
	bool bAll = Rig.WaitEnded(Utf8Conn, 20.0);
	for (const FPeer& Peer : Peers)
	{
		bAll = Rig.WaitEnded(Peer.Conn, 20.0) && bAll;
	}
	NoteOutcome(*this, TEXT("every peer-closed connection ended within 20 s"), bAll);

	TArray<FString> Rewritten;
	TArray<FString> EchoNotes;
	bool bAllEchoed = true;
	FString PeerSequence;
	for (const FPeer& Peer : Peers)
	{
		const TArray<FEvent> Seen = Rig.Events(Peer.Conn);
		const int32 Received = ReceivedCode(PeerClose(Seen));
		if (Received != Peer.Code)
		{
			Rewritten.Add(Received < 0 ? FString::Printf(TEXT("%d -> no code (%s)"), Peer.Code, *AfterOpen(Seen)) : FString::Printf(TEXT("%d -> %d"), Peer.Code, Received));
		}
		const FVaRestJson Journal = FetchJournal(*this, Site, Peer.Key);
		const TArray<FVaRestJson> Answers = JournalEvents(Journal, TEXT("close"));
		if (Answers.Num() == 0)
		{
			bAllEchoed = false;
			EchoNotes.Add(FString::Printf(TEXT("%d: no answer"), Peer.Code));
		}
		else if (!Answers[0].Has(TEXT("code")) || Answers[0][TEXT("code")].AsInt() != Peer.Code)
		{
			EchoNotes.Add(FString::Printf(TEXT("%d: answered %s"), Peer.Code, *ClientCloseHex(Journal)));
		}
		if (Peer.Code == 1000)
		{
			PeerSequence = AfterOpen(Seen);
		}
	}
	Fact(*this, TEXT("close.rewritten_codes"), EVerdict::Recorded,
		Rewritten.Num() == 0 ? FString(TEXT("none: every code arrived as sent")) : FString::Join(Rewritten, TEXT(", ")),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): which received close codes LWS 3.0 rewrites; 2.5 normalises codes a peer must not send"),
		TEXT("the server closed with each code (raw); the first two bytes of PEER_INITIATED_CLOSE's in"));
	Fact(*this, TEXT("close.echo_on_return_0"), AgreesIf(bAllEchoed),
		bAllEchoed ? FString(TEXT("every server close was answered with a close")) : FString::Join(EchoNotes.FilterByPredicate([](const FString& Each) { return Each.EndsWith(TEXT("no answer")); }), TEXT(", ")),
		TEXT("T12: PEER_INITIATED_CLOSE returning 0 makes LWS echo the close (libwebsockets.h:1217-1225)"),
		TEXT("the server's journal of each connection"));
	Fact(*this, TEXT("close.echo_payload"), EVerdict::Recorded,
		EchoNotes.Num() == 0 ? FString(TEXT("every answer carried the code the server sent")) : FString::Join(EchoNotes, TEXT(", ")),
		TEXT("RFC 6455 5.5.1: the answer typically echoes the status code"), TEXT("the server's journal: the client's close payload"));

	const TArray<FEvent> Utf8Seen = Rig.Events(Utf8Conn);
	const FEvent* Utf8Close = PeerClose(Utf8Seen);
	const TArray<uint8> Expected = Utf8Bytes(Utf8Reason);
	const bool bUtf8Intact = Utf8Close && Utf8Close->Bytes.Num() == Expected.Num() + 2
		&& FMemory::Memcmp(Utf8Close->Bytes.GetData() + 2, Expected.GetData(), Expected.Num()) == 0;
	Fact(*this, TEXT("close.reason_utf8_bytes"), AgreesIf(bUtf8Intact),
		Utf8Close ? FString::Printf(TEXT("reason bytes %s"), *HexOf(TConstArrayView<uint8>(Utf8Close->Bytes).RightChop(2))) : FString(TEXT("no PEER_INITIATED_CLOSE")),
		TEXT("WebSocket spec 2.5: the reason is decoded as UTF-8 by VaRest, so LWS must hand the bytes over unchanged"),
		FString::Printf(TEXT("close 4002 with reason \"%s\" (%s)"), *Utf8Reason, *HexOf(Expected)));
	Fact(*this, TEXT("close.peer_sequence"), EVerdict::Recorded, PeerSequence,
		TEXT("T12: CLIENT_CLOSED ends a client session (libwebsockets.h:1313-1314)"), TEXT("the 1000 connection, from ESTABLISHED on"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeEmptyCloseTest, "Probe.VaRest.Lws.Close.EmptyAndShort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeEmptyCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyNone = NewKey();
	const FString KeyOne = NewKey();
	TArray<FEvent> NoneSeen;
	TArray<FEvent> OneSeen;
	bool bAll = RunToEnd(Rig, TEXT("/ws/close/none?key=") + KeyNone, NoneSeen);
	bAll = RunToEnd(Rig, TEXT("/ws/close/onebyte?key=") + KeyOne, OneSeen) && bAll;
	NoteOutcome(*this, TEXT("both connections ended within 20 s"), bAll);
	const FVaRestJson NoneJournal = FetchJournal(*this, Site, KeyNone);
	const FVaRestJson OneJournal = FetchJournal(*this, Site, KeyOne);
	const FEvent* NoneClose = PeerClose(NoneSeen);
	Fact(*this, TEXT("close.empty_len"), AgreesIf(NoneClose && NoneClose->Len == 0),
		NoneClose ? FString::Printf(TEXT("PEER_INITIATED_CLOSE len=%lld"), NoneClose->Len) : FString::Printf(TEXT("no PEER_INITIATED_CLOSE: %s"), *AfterOpen(NoneSeen)),
		TEXT("WebSocket spec 2.5: length 0 -> 1005 (the engine reads two bytes anyway, E27: LwsWebSocket.cpp:351)"),
		TEXT("/ws/close/none: a close frame with an empty payload"));
	Fact(*this, TEXT("close.empty_echo"), EVerdict::Recorded, TEXT("the client answered ") + ClientCloseHex(NoneJournal),
		TEXT("none"), TEXT("the server's journal: the client's close payload"));
	const FEvent* OneClose = PeerClose(OneSeen);
	Fact(*this, TEXT("close.one_byte"), EVerdict::Recorded,
		FString::Printf(TEXT("%s; the client answered %s"),
			OneClose ? *FString::Printf(TEXT("PEER_INITIATED_CLOSE len=%lld bytes %s"), OneClose->Len, *HexOf(OneClose->Bytes)) : *FString::Printf(TEXT("no PEER_INITIATED_CLOSE (%s)"), *AfterOpen(OneSeen)),
			*ClientCloseHex(OneJournal)),
		TEXT("WebSocket spec 2.5: length 1 -> 1002 (malformed)"), TEXT("/ws/close/onebyte: a close frame with the payload 03"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbePeerReturnNonZeroTest, "Probe.VaRest.Lws.Close.PeerReturnNonZero",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbePeerReturnNonZeroTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	TArray<FEvent> Seen;
	NoteOutcome(*this, TEXT("the connection ended within 20 s"), RunToEnd(Rig, TEXT("/ws/close/1000?reason=bye&key=") + Key, Seen, 1));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	Fact(*this, TEXT("close.return_1_echo"), EVerdict::Recorded,
		FString::Printf(TEXT("the client answered %s; %s"), *ClientCloseHex(Journal), *AfterOpen(Seen)),
		TEXT("libwebsockets.h:1217-1225: nonzero -> LWS just closes; the engine returns 1 (LwsWebSocket.cpp:389-390)"),
		TEXT("PEER_INITIATED_CLOSE returning 1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeOurCloseTest, "Probe.VaRest.Lws.Close.Ours",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeOurCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// The close-ack wait is LWS's own timer: the short service wait, so the value is LWS's and not the rig's (review C-1)
	FRigOptions Options;
	Options.ServiceTimeoutMs = 100;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyEcho = NewKey();
	const FString KeyNoEcho = NewKey();
	const FString KeyLong = NewKey();
	const FOurClose Echoed = CloseOurs(Rig, TEXT("/ws/echo?key=") + KeyEcho, 1000, TEXT("bye"), 30.0);
	const FOurClose Unanswered = CloseOurs(Rig, TEXT("/ws/noecho?key=") + KeyNoEcho, 1000, TEXT("bye"), 45.0);
	const FOurClose Long = CloseOurs(Rig, TEXT("/ws/echo?key=") + KeyLong, 4000, FString::ChrN(200, TEXT('r')), 30.0);
	NoteOutcome(*this, TEXT("our three closes ended"), Echoed.bEnded && Unanswered.bEnded && Long.bEnded);
	const FVaRestJson EchoJournal = FetchJournal(*this, Site, KeyEcho);
	const FVaRestJson NoEchoJournal = FetchJournal(*this, Site, KeyNoEcho);
	const FVaRestJson LongJournal = FetchJournal(*this, Site, KeyLong);
	Fact(*this, TEXT("close.ours_echoed_ms"), EVerdict::Recorded,
		FString::Printf(TEXT("%.1f ms from our close to WSI_DESTROY against an echoing server"), Echoed.Ms),
		TEXT("none"), TEXT("lws_close_reason + -1 from WRITEABLE on /ws/echo; lws_service waiting up to 100 ms"));
	const TArray<FVaRestJson> ServerSaw = JournalEvents(NoEchoJournal, TEXT("close"));
	const TArray<FVaRestJson> ServerEof = JournalEvents(NoEchoJournal, TEXT("eof"));
	const FString ServerSide = ServerSaw.Num() > 0 && ServerEof.Num() > 0
		? FString::Printf(TEXT("the server saw our close, then TCP end %.2f s later"), ServerEof[0][TEXT("t")].AsDouble() - ServerSaw[0][TEXT("t")].AsDouble())
		: FString(TEXT("the server journal lacks the close or the TCP end"));
	Fact(*this, TEXT("close.ours_noecho_wait_ms"), EVerdict::Recorded,
		Unanswered.bEnded ? FString::Printf(TEXT("%.1f ms from our close to WSI_DESTROY against a server that never answers; %s"), Unanswered.Ms, *ServerSide)
			: FString(TEXT("did not end within 45 s")),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): whether LWS waits for the close echo; 2.5 bounds it with CloseTimeout (5 s)"),
		TEXT("our close on /ws/noecho, timeout_secs left at LWS's default; lws_service waiting up to 100 ms"));
	Fact(*this, TEXT("close.ours_sequence"), EVerdict::Recorded, Echoed.Sequence, TEXT("none"), TEXT("callbacks after our close on /ws/echo"));
	const TArray<FVaRestJson> LongSaw = JournalEvents(LongJournal, TEXT("close"));
	const TArray<FVaRestJson> LongFail = JournalEvents(LongJournal, TEXT("fail"));
	Fact(*this, TEXT("close.ours_long_reason"), EVerdict::Recorded,
		LongSaw.Num() > 0 ? FString::Printf(TEXT("the server got a %lld-byte close payload"), LongSaw[0][TEXT("len")].AsInt64())
			: LongFail.Num() > 0 ? FString::Printf(TEXT("the server failed the frame: %s"), *LongFail[0][TEXT("reason")].AsString())
			: FString(TEXT("no close reached the server")),
		TEXT("WebSocket spec 15: close reasons are cut to 123 bytes by VaRest; lws_close_reason takes \"up to 124 bytes\" (libwebsockets.h:832-845)"),
		TEXT("lws_close_reason with a 200-byte reason"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeDropsTest, "Probe.VaRest.Lws.Close.Drops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeDropsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	TArray<FEvent> FinSeen;
	TArray<FEvent> RstSeen;
	bool bAll = RunToEnd(Rig, TEXT("/ws/drop?after_ms=200&key=") + NewKey(), FinSeen);
	bAll = RunToEnd(Rig, TEXT("/ws/drop?after_ms=200&rst=1&key=") + NewKey(), RstSeen) && bAll;
	NoteOutcome(*this, TEXT("both dropped connections ended within 20 s"), bAll);
	const TCHAR* const UncleanSpec = TEXT("WebSocket spec 2.5: an unclean end (CLIENT_CLOSED / WSI_DESTROY without a close frame) -> 1006");
	Fact(*this, TEXT("close.drop_fin_sequence"), EVerdict::Recorded, AfterOpen(FinSeen), UncleanSpec, TEXT("/ws/drop: TCP FIN, no close frame"));
	Fact(*this, TEXT("close.drop_rst_sequence"), EVerdict::Recorded, AfterOpen(RstSeen), UncleanSpec, TEXT("/ws/drop?rst=1: TCP RST"));
	const bool bFinClosed = OfReason(FinSeen, LWS_CALLBACK_CLIENT_CLOSED).Num() > 0;
	const bool bRstClosed = OfReason(RstSeen, LWS_CALLBACK_CLIENT_CLOSED).Num() > 0;
	Fact(*this, TEXT("close.drop_reports_client_closed"), EVerdict::Recorded,
		FString::Printf(TEXT("CLIENT_CLOSED on FIN: %s, on RST: %s; PEER_INITIATED_CLOSE on either: %s"), YesNo(bFinClosed), YesNo(bRstClosed),
			YesNo(PeerClose(FinSeen) != nullptr || PeerClose(RstSeen) != nullptr)),
		UncleanSpec, TEXT("the two drops above"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeServerPingsTest, "Probe.VaRest.Lws.Ping.ServerPings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeServerPingsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/serverping?count=3&interval_ms=200&key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	// Completion: the server's "pings-done" text comes after its last PING (Task 2's route)
	const bool bDone = Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([Conn](const FEvent& Each)
		{
			return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("pings-done"), ESearchCase::CaseSensitive);
		});
	}, 15.0);
	NoteOutcome(*this, TEXT("the server's pings-done arrived"), bDone);
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	int32 Answered = 0;
	double Slowest = 0.0;
	const TArray<FVaRestJson> Frames = JournalEvents(Journal, TEXT("frame"));
	for (const FVaRestJson& Sent : JournalEvents(Journal, TEXT("sent")))
	{
		if (Sent[TEXT("op")].AsInt() != 9)
		{
			continue;
		}
		const FString Payload = Sent[TEXT("b64")].AsString();
		const FVaRestJson* Pong = Frames.FindByPredicate([&Payload, &Sent](const FVaRestJson& Each)
		{
			return Each[TEXT("op")].AsInt() == 10 && Each[TEXT("b64")].AsString().Equals(Payload, ESearchCase::CaseSensitive)
				&& Each[TEXT("t")].AsDouble() >= Sent[TEXT("t")].AsDouble();
		});
		if (Pong)
		{
			++Answered;
			Slowest = FMath::Max(Slowest, (*Pong)[TEXT("t")].AsDouble() - Sent[TEXT("t")].AsDouble());
		}
	}
	Fact(*this, TEXT("ping.auto_pong"), AgreesIf(Answered == 3), FString::Printf(TEXT("%d of 3 server PINGs answered with a PONG of the same payload"), Answered),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): LWS answers server PINGs (RFC 6455 5.5.2 requires it)"),
		TEXT("/ws/serverping: three PINGs 200 ms apart; the server's journal"));
	Fact(*this, TEXT("ping.auto_pong_latency_ms"), EVerdict::Recorded, FString::Printf(TEXT("slowest PONG %.1f ms after its PING"), Slowest * 1000.0),
		TEXT("none"), TEXT("the server's journal times"));
	const TArray<FEvent> Seen = Rig.Events(Conn);
	TArray<FString> Other;
	for (const FEvent& Each : Seen)
	{
		const bool bExpected = Each.Reason < 0 || Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE || Each.Reason == LWS_CALLBACK_CLIENT_WRITEABLE
			|| Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH
			|| Each.Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER || Each.Reason == LWS_CALLBACK_WSI_CREATE
			|| Each.Reason == LWS_CALLBACK_WSI_DESTROY || Each.Reason == LWS_CALLBACK_CLIENT_CLOSED || Each.Reason == LWS_CALLBACK_CLOSED;
		if (!bExpected)
		{
			Other.AddUnique(ReasonName(Each.Reason));
		}
	}
	Fact(*this, TEXT("ping.client_sees_server_ping"), EVerdict::Recorded,
		Other.Num() == 0 ? FString(TEXT("no callback for the server's PINGs")) : FString::Printf(TEXT("other callbacks: %s"), *FString::Join(Other, TEXT(", "))),
		TEXT("none"), TEXT("the connection's callbacks other than the usual ones"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeClientPingTest, "Probe.VaRest.Lws.Ping.ClientPing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeClientPingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/echo?key=") + NewKey();
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// WebSocket spec 2.5's ping: 8 bytes (sequence + send time); then the largest a control frame may carry
	const TArray<uint8> Eight = { 1, 2, 3, 4, 5, 6, 7, 8 };
	TArray<uint8> Largest;
	Largest.Init(static_cast<uint8>('p'), 125);
	Rig.Write(Conn, LWS_WRITE_PING, Eight);
	const auto PongCount = [Conn](const TArray<FEvent>& All, int32 Count)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE_PONG; }).Num() >= Count;
	};
	const bool bFirst = Rig.WaitFor([&PongCount](const TArray<FEvent>& All) { return PongCount(All, 1); });
	Rig.Write(Conn, LWS_WRITE_PING, Largest);
	const bool bSecond = Rig.WaitFor([&PongCount](const TArray<FEvent>& All) { return PongCount(All, 2); });
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const TArray<FEvent> Pongs = OfReason(Rig.Events(Conn), LWS_CALLBACK_CLIENT_RECEIVE_PONG);
	const TArray<FEvent> Writes = OfReason(Rig.Events(Conn), WriteDone);
	const bool bEchoed = bFirst && Pongs.Num() >= 1 && Pongs[0].Bytes == Eight;
	Fact(*this, TEXT("ping.pong_payload_echo"), AgreesIf(bEchoed),
		Pongs.Num() >= 1 ? FString::Printf(TEXT("CLIENT_RECEIVE_PONG len=%lld bytes %s"), Pongs[0].Len, *HexOf(Pongs[0].Bytes)) : FString(TEXT("no CLIENT_RECEIVE_PONG")),
		TEXT("T9: LWS_WRITE_PING, and CLIENT_RECEIVE_PONG hands over the payload WebSocket spec 2.5 matches"),
		TEXT("an 8-byte PING 0102030405060708 to /ws/echo"));
	Fact(*this, TEXT("ping.rtt_ms"), EVerdict::Recorded,
		bEchoed && Writes.Num() >= 1 ? FString::Printf(TEXT("%.2f ms from lws_write(PING) to CLIENT_RECEIVE_PONG on loopback"), (Pongs[0].Time - Writes[0].Time) * 1000.0) : FString(TEXT("not measured")),
		TEXT("WebSocket spec 2.5: RTT measured on the service thread"), TEXT("the first ping"));
	Fact(*this, TEXT("ping.max_payload_125"), EVerdict::Recorded,
		bSecond && Pongs.Num() >= 2 ? FString::Printf(TEXT("a 125-byte PING came back as %lld bytes, %s"), Pongs[1].Len, Pongs[1].Bytes == Largest ? TEXT("unchanged") : TEXT("changed"))
			: FString(TEXT("no second CLIENT_RECEIVE_PONG")),
		TEXT("RFC 6455 5.5: a control frame carries at most 125 bytes"), TEXT("a 125-byte PING to /ws/echo"));

	FConnectSpec Unsolicited;
	Unsolicited.Path = TEXT("/ws/serverping?count=1&interval_ms=100&unsolicited=1&key=") + NewKey();
	const int32 Other = Rig.Connect(MoveTemp(Unsolicited));
	const bool bDone = Rig.WaitFor([Other](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([Other](const FEvent& Each)
		{
			return Each.Conn == Other && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("pings-done"), ESearchCase::CaseSensitive);
		});
	});
	Rig.Close(Other, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the unsolicited-pong connection ended after our close"), Rig.WaitEnded(Other));
	const TArray<FEvent> OtherPongs = OfReason(Rig.Events(Other), LWS_CALLBACK_CLIENT_RECEIVE_PONG);
	Fact(*this, TEXT("ping.unsolicited_pong_delivered"), EVerdict::Recorded,
		bDone ? FString::Printf(TEXT("%d CLIENT_RECEIVE_PONG for an unsolicited PONG \"unsolicited\"%s"), OtherPongs.Num(),
			OtherPongs.Num() > 0 && EventText(OtherPongs[0]).Equals(TEXT("unsolicited"), ESearchCase::CaseSensitive) ? TEXT(", payload intact") : TEXT(""))
			: FString(TEXT("pings-done never arrived")),
		TEXT("RFC 6455 5.5.3: an unsolicited PONG is a heartbeat; WebSocket spec 2.5 counts inbound bytes as alive"),
		TEXT("/ws/serverping?unsolicited=1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeSilentTest, "Probe.VaRest.Lws.Ping.Silent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeSilentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// A short timeout_secs: if LWS ends idle connections by itself, it does so inside this test. The short service wait
	// and a window past timeout_secs + the wait + 5 s, so an idle kill is seen inside it (review C-1)
	constexpr uint32 SilentTimeoutSecs = 3;
	constexpr int32 SilentWaitMs = 100;
	const double Window = SilentTimeoutSecs + SilentWaitMs / 1000.0 + 5.0;
	FRigOptions Options;
	Options.TimeoutSecs = SilentTimeoutSecs;
	Options.ServiceTimeoutMs = SilentWaitMs;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/silent?key=") + NewKey();
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	const double OpenedAt = FPlatformTime::Seconds();
	Rig.Write(Conn, LWS_WRITE_PING, Utf8Bytes(TEXT("dead?")));
	// Completion before silence: our PING has left (WriteDone), then 3 s without a PONG
	NoteOutcome(*this, TEXT("our PING was written"), Rig.WaitReason(Conn, WriteDone));
	const bool bPong = Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_RECEIVE_PONG, 3.0);
	Fact(*this, TEXT("ping.silent_no_pong"), AgreesIf(!bPong), bPong ? FString(TEXT("a PONG arrived")) : FString(TEXT("no PONG within 3 s")),
		TEXT("the control for WebSocket spec 2.5's dead-link detection: /ws/silent never answers"), TEXT("one PING to /ws/silent"));
	const double Left = FMath::Max(0.0, Window - (FPlatformTime::Seconds() - OpenedAt));
	const bool bDied = Rig.WaitEnded(Conn, Left);
	FEvent Destroyed;
	Fact(*this, TEXT("ping.idle_survives_timeout_secs"), AgreesIf(!bDied),
		bDied && Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed)
			? FString::Printf(TEXT("LWS ended the idle connection %.1f s after it opened: %s"), Destroyed.Time - OpenedAt, *CallSequence(Rig.Events(Conn)))
			: FString::Printf(TEXT("still open %.1f s after it opened, with timeout_secs = %u and lws_service waiting up to %d ms"), Window, SilentTimeoutSecs, SilentWaitMs),
		TEXT("WebSocket spec 2.1: ws_ping_pong_interval = 0, so LWS neither pings nor drops an idle connection; VaRest's own ping decides (2.5)"),
		TEXT("an open connection to /ws/silent, nothing sent after one PING"));
	if (!bDied)
	{
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(*this, TEXT("the silent connection ended after our close"), Rig.WaitEnded(Conn, 40.0));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build and run the groups**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Close && cp /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-close.log && Tools/run_tests.sh --no-build Probe.VaRest.Lws.Ping`
Expected: `run_tests: 5 passed, 0 failed`, then `run_tests: 3 passed, 0 failed`. A test fails only on a harness invariant (decision 3): the server or a journal did not answer, a rig did not start, or a precondition connection (`/ws/echo`, `/ws/silent`) did not open. `grep -c 'FACT {"id":"close\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-close.log` prints `16`; `grep -c 'FACT {"id":"ping\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `9`.

Control: `close.echo_on_return_0` (return 0) and `close.return_1_echo` (return 1) must not both say the client answered. If they do, the hook return value is not reaching LWS: check `FRig::Handle`'s `WS_PEER_INITIATED_CLOSE` branch before recording.

- [ ] **Step 4: Commit**

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeCloseTest.cpp
git commit -m "tests: LWS 3.0 probe -- peer close codes and echoes, empty and one-byte closes, our close vs a non-echoing server, drops, server and client pings, silent server (WS W0)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 8: Probe — receive chunking, flow control, fragmented and large sends, auto-reply at a boundary, SO_SNDBUF

**Files:**
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeDataTest.cpp`

**Interfaces:**
- Consumes (Task 4): `FRig` (`Connect`, `ExecOn`, `Write`, `WriteNow`, `Close`, `Record`, `WaitFor`, `WaitReason`, `WaitEnded`, `Events`, `Find`), `FConn` (`Outbox`, `Counters`, `Rig`, `Id`, `Wsi`), `FWrite`, `FConnectSpec::Hook`, `KeepDefault`, `Marker`, `WriteDone`, `IsMessageStart`, `WaitBytes`, `OfReason`, `HexOf`, `EventText`, `Utf8Bytes`, `CallSequence`, `FetchJournal`, `JournalEvents`, `JournalBytes`, `Fact`, `AgreesIf`, `YesNo`, `NoteOutcome`, flags `FirstFragment`, `FinalFragment`, `BinaryFrame`, `ETls::LwsGlobalInit`; `VaRest::Http::FSha256` (`VaRestSha256.h`); server routes `/ws/fragment`, `/ws/large`, `/ws/order`, `/ws/badutf8`, `/ws/flood`, `/ws/sink`, `/ws/echo`.
- Produces facts: `rx.frames_split_utf8`, `rx.final_flag`, `rx.first_flag`, `rx.bytes_intact`, `rx.chunk_sizes_single_frame`, `rx.remaining_payload`, `rx.binary_flag_on_continuations`, `rx.control_inside_fragmented`, `rx.order_preserved`, `rx.invalid_utf8_passthrough`, `rx.flow_pause_stops_callbacks`, `rx.flow_resume_from_command`, `rx.flow_resume_form`, `rx.flow_backpressure_reached_server`, `tx.no_fin_frames`, `tx.ping_between_fragments`, `tx.message_intact`, `tx.partial_after_fragment`, `tx.large_write_result`, `tx.large_write_frames`, `tx.large_write_intact`, `tx.two_writes_one_writeable`, `tx.autoreply_between_messages`, `tx.autoreply_delay_ms`, `tx.so_sndbuf_ws`, `tx.so_sndbuf_wss`.
- Tests: `Probe.VaRest.Lws.Data.ReceiveChunking`, `.Data.FlowControl`, `.Data.FragmentedSend`, `.Data.LargeWrite`, `.Data.TwoWritesPerWriteable`, `.Data.AutoReplyBoundary`, `.Data.SoSndbuf`.

- [ ] **Step 1: Run the filter to see it match nothing**

Run: `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Data`
Expected: exit 3, `matched no tests`.

- [ ] **Step 2: Write the probe tests**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeDataTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): how LWS 3.0 hands received frames over (chunks, flags, remaining bytes, order), pauses and
// resumes reading, writes fragments, large buffers and two frames per WRITEABLE, lets a reply in between two messages,
// and takes SO_SNDBUF on its socket (WebSocket spec 2.3, 2.4)

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "VaRestSha256.h"

#if PLATFORM_LINUX || PLATFORM_MAC
#include <sys/socket.h>
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate
{
	constexpr int32 AutoReplyFragmentBytes = 64 * 1024;
	constexpr int32 AutoReplyFragments = 32;

	/** The connection's CLIENT_RECEIVE events */
	TArray<FEvent> Chunks(FRig& Rig, int32 Conn)
	{
		return OfReason(Rig.Events(Conn), LWS_CALLBACK_CLIENT_RECEIVE);
	}

	TArray<uint8> Assemble(const TArray<FEvent>& Received)
	{
		TArray<uint8> Out;
		for (const FEvent& Each : Received)
		{
			Out.Append(Each.Bytes);
		}
		return Out;
	}

	/** "len[flags,remaining]" per chunk: F first, L final, B binary */
	FString Describe(const TArray<FEvent>& Received)
	{
		TArray<FString> Parts;
		for (const FEvent& Each : Received)
		{
			Parts.Add(FString::Printf(TEXT("%lld[%s%s%s,%lld]"), Each.Len, (Each.Flags & FirstFragment) != 0 ? TEXT("F") : TEXT(""),
				(Each.Flags & FinalFragment) != 0 ? TEXT("L") : TEXT(""), (Each.Flags & BinaryFrame) != 0 ? TEXT("B") : TEXT(""), Each.Remaining));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	/** Indexes of the chunks with a flag set */
	FString Where(const TArray<FEvent>& Received, uint32 Flag)
	{
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Received.Num(); ++Index)
		{
			if ((Received[Index].Flags & Flag) != 0)
			{
				Parts.Add(FString::FromInt(Index));
			}
		}
		return FString::Printf(TEXT("[%s] of %d"), *FString::Join(Parts, TEXT(",")), Received.Num());
	}

	FString Sha(TConstArrayView<uint8> Bytes)
	{
		return VaRest::Http::FSha256::Hash(Bytes).ToHex();
	}

	/** Connect, wait until Bytes arrived, close, wait for the end; what happened is noted, never failed (review M-2) */
	void Receive(FAutomationTestBase& Test, FRig& Rig, const FString& Path, int64 Bytes, int32& OutConn)
	{
		FConnectSpec Spec;
		Spec.Path = Path;
		OutConn = Rig.Connect(MoveTemp(Spec));
		const bool bGot = WaitBytes(Rig, OutConn, Bytes, 20.0);
		Rig.Close(OutConn, 1000, TEXT("done"));
		const bool bEnded = Rig.WaitEnded(OutConn, 30.0);
		NoteOutcome(Test, FString::Printf(TEXT("%s: %lld bytes %s, connection %s"), *Path, Bytes, bGot ? TEXT("arrived") : TEXT("did not arrive"), bEnded ? TEXT("ended") : TEXT("did not end")), bGot && bEnded);
	}

	/** The client's frames as the server saw them, "op/fin/len" (close frames left out) */
	FString ClientFrames(const FVaRestJson& Journal, bool bWithControl)
	{
		TArray<FString> Parts;
		for (const FVaRestJson& Frame : JournalEvents(Journal, TEXT("frame")))
		{
			const int32 Op = Frame[TEXT("op")].AsInt();
			if (Op == 8 || (!bWithControl && Op >= 8))
			{
				continue;
			}
			Parts.Add(FString::Printf(TEXT("%d/%d/%lld"), Op, Frame[TEXT("fin")].AsInt(), Frame[TEXT("len")].AsInt64()));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	/** One text message of Fragments frames of FragmentBytes bytes of Byte, appended to an outbox (service thread) */
	void QueueTextMessage(TArray<FWrite>& Outbox, uint8 Byte, int32 FragmentBytes, int32 Fragments)
	{
		for (int32 Index = 0; Index < Fragments; ++Index)
		{
			FWrite& Item = Outbox.AddDefaulted_GetRef();
			Item.Protocol = (Index == 0 ? static_cast<int32>(LWS_WRITE_TEXT) : static_cast<int32>(LWS_WRITE_CONTINUATION))
				| (Index == Fragments - 1 ? 0 : static_cast<int32>(LWS_WRITE_NO_FIN));
			Item.Buffer.SetNumZeroed(LWS_PRE);
			Item.Buffer.AddUninitialized(FragmentBytes);
			FMemory::Memset(Item.Buffer.GetData() + LWS_PRE, Byte, FragmentBytes);
		}
	}

	TArray<uint8> Filled(int32 Num, uint8 Byte)
	{
		TArray<uint8> Out;
		Out.Init(Byte, Num);
		return Out;
	}

	/** The flood's size: more than loopback socket buffers hold even with raised tcp_wmem/tcp_rmem (review M-6) */
	constexpr int64 FloodBytes = 32 * 1024 * 1024;

	struct FFlood
	{
		bool bRan = false;
		int64 PauseReturned = MIN_int64;
		int64 AfterPause = 0;
		int32 AfterPauseCalls = 0;
		int64 ResumeReturned = MIN_int64;
		bool bResumed = false;
		bool bFloodLogged = false;
		double Longest = -1.0;
		int32 SlowWrites = 0;
	};

	/**
	 * One round on its own rig: a flood paused with lws_rx_flow_control(wsi, 0) inside its first CLIENT_RECEIVE (where
	 * WebSocket spec 2.3 counts undelivered bytes), 2 s of watching, then lws_rx_flow_control(wsi, ResumeArg) from a
	 * command. bRan is false only when the harness failed (no rig, or the flood's first chunk never came)
	 */
	FFlood Flood(FAutomationTestBase& Test, const FServer& Site, int32 ResumeArg)
	{
		FFlood Out;
		FRig Rig(Site);
		if (!Test.TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return Out;
		}
		const FString Key = NewKey();
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/flood?bytes=%lld&chunk=65536&key=%s"), FloodBytes, *Key);
		Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
		{
			const FName PausedKey(TEXT("paused"));
			if (Reason == LWS_CALLBACK_CLIENT_RECEIVE && C.Counters.FindRef(PausedKey) == 0)
			{
				C.Counters.Add(PausedKey, 1);
				C.Rig->Record(C.Id, Marker, lws_rx_flow_control(Wsi, 0), TEXT("paused"));
			}
			return KeepDefault;
		};
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		if (!Test.TestTrue(TEXT("the flood's first chunk arrives"), Rig.WaitReason(Conn, Marker)))
		{
			return Out;
		}
		Out.bRan = true;
		FEvent Paused;
		Rig.Find(Conn, Marker, Paused, TEXT("paused"));
		Out.PauseReturned = Paused.Len;
		// Completion before silence: the pause is recorded; now the window in which no more should arrive
		FPlatformProcess::Sleep(2.0f);
		for (const FEvent& Each : Rig.Events(Conn))
		{
			if (Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && Each.Time > Paused.Time)
			{
				Out.AfterPause += Each.Len;
				++Out.AfterPauseCalls;
			}
		}
		Rig.ExecOn(Conn, TEXT("resume"), [ResumeArg](FConn& C) { C.Rig->Record(C.Id, Marker, lws_rx_flow_control(C.Wsi, ResumeArg), TEXT("resumed")); });
		Out.bResumed = WaitBytes(Rig, Conn, FloodBytes, 20.0);
		FEvent Resumed;
		if (Rig.Find(Conn, Marker, Resumed, TEXT("resumed")))
		{
			Out.ResumeReturned = Resumed.Len;
		}
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(Test, TEXT("the flood connection ended after our close"), Rig.WaitEnded(Conn, 30.0));
		const FVaRestJson Journal = FetchJournal(Test, Site, Key);
		const TArray<FVaRestJson> Events = JournalEvents(Journal, TEXT("flood"));
		if (Events.Num() > 0)
		{
			Out.bFloodLogged = true;
			Out.Longest = Events[0][TEXT("longest_write_s")].AsDouble();
			Out.SlowWrites = Events[0][TEXT("writes_over_100ms")].AsInt();
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeReceiveChunkingTest, "Probe.VaRest.Lws.Data.ReceiveChunking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeReceiveChunkingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}

	// A. 5 KiB of Cyrillic in frames cut inside a code point at about 4096 (review item 5, E9)
	const FString KeyCut = NewKey();
	int32 Cut = 0;
	Receive(*this, Rig, TEXT("/ws/fragment?cyrillic=5120&split_inside=4096&key=") + KeyCut, 5120, Cut);
	// B. one 200 000-byte frame
	int32 Single = 0;
	Receive(*this, Rig, TEXT("/ws/large?bytes=200000&ascii=1&key=") + NewKey(), 200000, Single);
	// C. a binary message in three frames
	int32 Binary = 0;
	Receive(*this, Rig, TEXT("/ws/fragment?bytes=10000&binary=1&at=3000,6000&key=") + NewKey(), 10000, Binary);
	// D. a PING from the server between two fragments of a text message
	const FString KeyPing = NewKey();
	int32 Pinged = 0;
	Receive(*this, Rig, TEXT("/ws/fragment?cyrillic=5120&split_inside=2000&ping_between=1&key=") + KeyPing, 5120, Pinged);
	// E. 400 small messages, text and binary in turn
	constexpr int32 OrderCount = 400;
	int64 OrderBytes = 0;
	for (int32 Index = 0; Index < OrderCount; ++Index)
	{
		OrderBytes += Index % 2 == 0 ? FString::Printf(TEXT("t%d"), Index).Len() : 4;
	}
	int32 Order = 0;
	Receive(*this, Rig, FString::Printf(TEXT("/ws/order?n=%d&key=%s"), OrderCount, *NewKey()), OrderBytes, Order);
	// F. invalid UTF-8 in a text message
	int32 Bad = 0;
	Receive(*this, Rig, TEXT("/ws/badutf8?where=text&key=") + NewKey(), 5, Bad);

	const FVaRestJson CutJournal = FetchJournal(*this, Site, KeyCut);
	const TArray<FVaRestJson> CutSent = JournalEvents(CutJournal, TEXT("message_sent"));
	const TArray<FEvent> CutChunks = Chunks(Rig, Cut);
	const TArray<uint8> CutBytes = Assemble(CutChunks);
	TArray<FString> InsideCodePoint;
	int64 Offset = 0;
	for (int32 Index = 0; Index + 1 < CutChunks.Num(); ++Index)
	{
		Offset += CutChunks[Index].Len;
		if (Offset < CutBytes.Num() && (CutBytes[static_cast<int32>(Offset)] & 0xC0) == 0x80)
		{
			InsideCodePoint.Add(FString::Printf(TEXT("%lld"), Offset));
		}
	}
	const FString CutAt = CutSent.Num() > 0 ? CutSent[0][TEXT("frames_at")].ToString() : FString(TEXT("?"));
	Fact(*this, TEXT("rx.frames_split_utf8"), EVerdict::Recorded,
		FString::Printf(TEXT("frames cut at %s; chunks %s; chunk ends inside a code point at [%s]"), *CutAt, *Describe(CutChunks), *FString::Join(InsideCodePoint, TEXT(","))),
		TEXT("WebSocket spec 2.3 and E9: one CLIENT_RECEIVE per frame end or full rx buffer; VaRest assembles bytes, so a split code point is harmless"),
		TEXT("/ws/fragment?cyrillic=5120&split_inside=4096"));
	const bool bOnlyLastFinal = CutChunks.Num() > 0 && Where(CutChunks, FinalFragment).Equals(FString::Printf(TEXT("[%d] of %d"), CutChunks.Num() - 1, CutChunks.Num()), ESearchCase::CaseSensitive);
	const int32 Completions = CutChunks.FilterByPredicate([](const FEvent& Each) { return (Each.Flags & FinalFragment) != 0 && Each.Remaining == 0; }).Num();
	Fact(*this, TEXT("rx.final_flag"), AgreesIf(bOnlyLastFinal && Completions == 1),
		FString::Printf(TEXT("lws_is_final_fragment on chunks %s; final with remaining 0 on %d chunk(s)"), *Where(CutChunks, FinalFragment), Completions),
		TEXT("WebSocket spec 2.3: a message is complete when lws_is_final_fragment and lws_remaining_packet_payload == 0 (T8)"),
		TEXT("the same message, two frames"));
	Fact(*this, TEXT("rx.first_flag"), EVerdict::Recorded, FString::Printf(TEXT("lws_is_first_fragment on chunks %s"), *Where(CutChunks, FirstFragment)),
		TEXT("none"), TEXT("the same message"));
	Fact(*this, TEXT("rx.bytes_intact"), AgreesIf(CutSent.Num() > 0 && Sha(CutBytes).Equals(CutSent[0][TEXT("sha256")].AsString(), ESearchCase::CaseSensitive)),
		FString::Printf(TEXT("%d bytes assembled, sha256 %s"), CutBytes.Num(), *Sha(CutBytes)),
		TEXT("WebSocket spec 2.3: chunks appended as bytes give the message exactly"), TEXT("against the server's message_sent sha256"));

	const TArray<FEvent> SingleChunks = Chunks(Rig, Single);
	bool bRemainingFalls = SingleChunks.Num() > 0 && SingleChunks.Last().Remaining == 0;
	for (int32 Index = 0; Index + 1 < SingleChunks.Num(); ++Index)
	{
		bRemainingFalls = bRemainingFalls && SingleChunks[Index].Remaining > 0 && (SingleChunks[Index].Flags & FinalFragment) == 0;
	}
	Fact(*this, TEXT("rx.chunk_sizes_single_frame"), EVerdict::Recorded, Describe(SingleChunks),
		TEXT("E9: a callback at every frame end and every 64 KiB (rx_buffer_size 65536)"), TEXT("/ws/large?bytes=200000: one frame"));
	Fact(*this, TEXT("rx.remaining_payload"), AgreesIf(bRemainingFalls),
		bRemainingFalls ? FString(TEXT("lws_remaining_packet_payload > 0 and no final flag until the last chunk, 0 there"))
			: FString::Printf(TEXT("not so: %s"), *Describe(SingleChunks)),
		TEXT("WebSocket spec 2.3: complete when final and remaining == 0 (T8; libwebsockets.h:5212-5232)"), TEXT("the same frame"));
	Fact(*this, TEXT("rx.binary_flag_on_continuations"), EVerdict::Recorded,
		FString::Printf(TEXT("lws_frame_is_binary on chunks %s"), *Where(Chunks(Rig, Binary), BinaryFrame)),
		TEXT("WebSocket spec 2.3: the kind comes from the first fragment"), TEXT("/ws/fragment?bytes=10000&binary=1&at=3000,6000"));

	const FVaRestJson PingJournal = FetchJournal(*this, Site, KeyPing);
	const TArray<FVaRestJson> PingSent = JournalEvents(PingJournal, TEXT("message_sent"));
	const TArray<uint8> PingBytes = Assemble(Chunks(Rig, Pinged));
	const bool bPongBetween = JournalEvents(PingJournal, TEXT("frame")).ContainsByPredicate([](const FVaRestJson& Each)
	{
		return Each[TEXT("op")].AsInt() == 10 && JournalBytes(Each) == Utf8Bytes(TEXT("between"));
	});
	const bool bPingIntact = PingSent.Num() > 0 && Sha(PingBytes).Equals(PingSent[0][TEXT("sha256")].AsString(), ESearchCase::CaseSensitive);
	Fact(*this, TEXT("rx.control_inside_fragmented"), AgreesIf(bPongBetween && bPingIntact),
		FString::Printf(TEXT("PONG for the PING between fragments: %s; message intact: %s"), YesNo(bPongBetween), YesNo(bPingIntact)),
		TEXT("RFC 6455 5.4: control frames may sit between fragments; WebSocket spec 2.3 assembles across them"),
		TEXT("/ws/fragment?cyrillic=5120&split_inside=2000&ping_between=1"));

	const TArray<FEvent> OrderChunks = Chunks(Rig, Order);
	int32 Mismatches = 0;
	int32 Position = 0;
	for (const FEvent& Each : OrderChunks)
	{
		const bool bWantBinary = Position % 2 == 1;
		const bool bMatches = bWantBinary
			? (Each.Flags & BinaryFrame) != 0 && Each.Bytes.Num() == 4
				&& ((static_cast<uint32>(Each.Bytes[0]) << 24) | (static_cast<uint32>(Each.Bytes[1]) << 16) | (static_cast<uint32>(Each.Bytes[2]) << 8) | Each.Bytes[3]) == static_cast<uint32>(Position)
			: (Each.Flags & BinaryFrame) == 0 && EventText(Each).Equals(FString::Printf(TEXT("t%d"), Position), ESearchCase::CaseSensitive);
		Mismatches += bMatches ? 0 : 1;
		++Position;
	}
	Fact(*this, TEXT("rx.order_preserved"), AgreesIf(Mismatches == 0 && OrderChunks.Num() == OrderCount),
		FString::Printf(TEXT("%d messages, %d out of place or wrong"), OrderChunks.Num(), Mismatches),
		TEXT("WebSocket spec 7.1: one ordered path; review item 4: count inversions between text and binary"), TEXT("/ws/order?n=400"));
	const TArray<uint8> BadBytes = Assemble(Chunks(Rig, Bad));
	const TArray<uint8> BadSent = { 0x6f, 0x6b, 0x20, 0xc3, 0x28 };
	Fact(*this, TEXT("rx.invalid_utf8_passthrough"), EVerdict::Recorded,
		FString::Printf(TEXT("delivered %s (%s); %s"), *HexOf(BadBytes), BadBytes == BadSent ? TEXT("unchanged") : TEXT("changed"), *CallSequence(Rig.Events(Bad))),
		TEXT("WebSocket spec 2.3: VaRest validates UTF-8 once on the assembled message, so LWS must pass the bytes through"),
		TEXT("/ws/badutf8: text 6f6b20c328"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeFlowControlTest, "Probe.VaRest.Lws.Data.FlowControl",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeFlowControlTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// Round 1: the bool form, as the engine and WebSocket spec 2.3 write it. Round 2, its own rig and flood: the explicit
	// form for a change made outside the connection's callback (libwebsockets.h:5192-5194). lws_rx_flow_control turns a
	// bool into APPLIES | USER_BOOL | (enable << 13), and only APPLIES_ENABLE_BIT clears the bitmap (libwebsockets.c.o
	// 0x2570-0x25b6, review I-2), so the explicit resume is APPLIES_ENABLE, not APPLIES_DISABLE (libwebsockets.h:5164-5173)
	const FFlood BoolRound = Flood(*this, Site, 1);
	if (!BoolRound.bRan)
	{
		return false;
	}
	const FFlood NowRound = Flood(*this, Site, LWS_RXFLOW_REASON_APPLIES_ENABLE | LWS_RXFLOW_REASON_USER_BOOL | LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
	if (!NowRound.bRan)
	{
		return false;
	}
	Fact(*this, TEXT("rx.flow_pause_stops_callbacks"), AgreesIf(BoolRound.AfterPause <= 256 * 1024),
		FString::Printf(TEXT("%lld bytes in %d CLIENT_RECEIVE calls in the 2 s after lws_rx_flow_control(wsi, 0) (returned %lld); round 2: %lld bytes"),
			BoolRound.AfterPause, BoolRound.AfterPauseCalls, BoolRound.PauseReturned, NowRound.AfterPause),
		TEXT("T11 and WebSocket spec 2.3: lws_rx_flow_control(wsi, 0) stops reading, so TCP pushes back"),
		FString::Printf(TEXT("/ws/flood: %lld bytes as 64 KiB binary messages; paused inside the first CLIENT_RECEIVE"), FloodBytes));
	Fact(*this, TEXT("rx.flow_resume_from_command"), AgreesIf(BoolRound.bResumed || NowRound.bResumed),
		FString::Printf(TEXT("all %lld bytes arrived after a resume from a command: bool form %s, explicit form %s"), FloodBytes, YesNo(BoolRound.bResumed), YesNo(NowRound.bResumed)),
		TEXT("WebSocket spec 2.3: reading resumes (below half) from the service thread"), TEXT("the resume as a command, i.e. from EVENT_WAIT_CANCELLED, 2 s after the pause"));
	Fact(*this, TEXT("rx.flow_resume_form"), EVerdict::Recorded,
		FString::Printf(TEXT("lws_rx_flow_control(wsi, 1): %s (returned %lld); lws_rx_flow_control(wsi, APPLIES_ENABLE | USER_BOOL | FLAG_PROCESS_NOW): %s (returned %lld)"),
			BoolRound.bResumed ? TEXT("resumed reading") : TEXT("did not resume within 20 s"), BoolRound.ResumeReturned,
			NowRound.bResumed ? TEXT("resumed reading") : TEXT("did not resume within 20 s"), NowRound.ResumeReturned),
		TEXT("none"), TEXT("two rounds, each its own rig and flood; the return values are lws_rx_flow_control's"));
	Fact(*this, TEXT("rx.flow_backpressure_reached_server"), AgreesIf(BoolRound.Longest >= 1.0),
		BoolRound.bFloodLogged ? FString::Printf(TEXT("the server's longest write blocked %.2f s; %d writes blocked over 100 ms"), BoolRound.Longest, BoolRound.SlowWrites)
			: FString(TEXT("no flood event in the journal")),
		TEXT("WebSocket spec 2.3: with reading paused, TCP pushes back on the server"),
		FString::Printf(TEXT("the server's flood event, round 1 (%lld bytes: more than loopback socket buffers hold, review M-6)"), FloodBytes));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeFragmentedSendTest, "Probe.VaRest.Lws.Data.FragmentedSend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeFragmentedSendTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/sink?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// One 200 000-byte text message in four fragments, one per WRITEABLE (WebSocket spec 2.4), a PING after the second
	Rig.Write(Conn, LWS_WRITE_TEXT | LWS_WRITE_NO_FIN, Filled(50000, 'a'));
	Rig.Write(Conn, LWS_WRITE_CONTINUATION | LWS_WRITE_NO_FIN, Filled(50000, 'b'));
	Rig.Write(Conn, LWS_WRITE_PING, Utf8Bytes(TEXT("between")));
	Rig.Write(Conn, LWS_WRITE_CONTINUATION | LWS_WRITE_NO_FIN, Filled(50000, 'c'));
	Rig.Write(Conn, LWS_WRITE_CONTINUATION, Filled(50000, 'd'));
	NoteOutcome(*this, TEXT("all five writes were done"), Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == WriteDone; }).Num() >= 5;
	}));
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	const FString Data = ClientFrames(Journal, false);
	const FString WithPing = ClientFrames(Journal, true);
	Fact(*this, TEXT("tx.no_fin_frames"), AgreesIf(Data.Equals(TEXT("1/0/50000, 0/0/50000, 0/0/50000, 0/1/50000"), ESearchCase::CaseSensitive)), Data,
		TEXT("T10: LWS_WRITE_NO_FIN and LWS_WRITE_CONTINUATION give one frame each, as written (op/fin/len)"), TEXT("four fragments of 50 000 bytes"));
	Fact(*this, TEXT("tx.ping_between_fragments"), AgreesIf(WithPing.Equals(TEXT("1/0/50000, 0/0/50000, 9/1/7, 0/0/50000, 0/1/50000"), ESearchCase::CaseSensitive)), WithPing,
		TEXT("WebSocket spec 2.4: PINGs and the close frame can be written between fragments (RFC 6455 5.4)"), TEXT("a PING queued after the second fragment"));
	TArray<uint8> Whole = Filled(50000, 'a');
	Whole.Append(Filled(50000, 'b'));
	Whole.Append(Filled(50000, 'c'));
	Whole.Append(Filled(50000, 'd'));
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	Fact(*this, TEXT("tx.message_intact"), AgreesIf(Messages.Num() == 1 && Messages[0][TEXT("sha256")].AsString().Equals(Sha(Whole), ESearchCase::CaseSensitive)),
		FString::Printf(TEXT("%d message(s) at the server"), Messages.Num()), TEXT("none"), TEXT("the server's message sha256 against ours"));
	TArray<FString> Notes;
	for (const FEvent& Each : OfReason(Rig.Events(Conn), WriteDone))
	{
		Notes.Add(Each.Note);
	}
	Fact(*this, TEXT("tx.partial_after_fragment"), EVerdict::Recorded, FString::Join(Notes, TEXT("; ")),
		TEXT("T10 and WebSocket spec 2.4: lws_partial_buffered is checked after each write"), TEXT("the five writes"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeLargeWriteTest, "Probe.VaRest.Lws.Data.LargeWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeLargeWriteTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/sink?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// One lws_write of 1 MiB: more than rx_buffer_size, which caps one send when tx_packet_size is 0 (libwebsockets.h:2346-2353)
	const TArray<uint8> Big = Filled(1024 * 1024, 'x');
	Rig.Write(Conn, LWS_WRITE_TEXT, Big);
	NoteOutcome(*this, TEXT("the write was done"), Rig.WaitReason(Conn, WriteDone));
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn, 30.0));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	FEvent Written;
	Rig.Find(Conn, WriteDone, Written);
	Fact(*this, TEXT("tx.large_write_result"), EVerdict::Recorded, FString::Printf(TEXT("lws_write(1 MiB): %s"), *Written.Note),
		TEXT("T10: LWS buffers a truncated write itself and suppresses WRITEABLE until it drains (libwebsockets.h:4914-4930)"), TEXT("one 1 MiB text write"));
	Fact(*this, TEXT("tx.large_write_frames"), EVerdict::Recorded, ClientFrames(Journal, false), TEXT("none"), TEXT("the server's frames (op/fin/len)"));
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	Fact(*this, TEXT("tx.large_write_intact"), AgreesIf(Messages.Num() == 1 && Messages[0][TEXT("sha256")].AsString().Equals(Sha(Big), ESearchCase::CaseSensitive)),
		FString::Printf(TEXT("%d message(s) at the server"), Messages.Num()), TEXT("none"), TEXT("the server's message sha256 against ours"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTwoWritesTest, "Probe.VaRest.Lws.Data.TwoWritesPerWriteable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTwoWritesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/sink?key=") + Key;
	// "libwebsockets-3.0 only allows us to send once per LWS_CALLBACK_*_WRITABLE event" (LwsWebSocket.cpp:195): is it so?
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		const FName Double(TEXT("double"));
		if (Reason == LWS_CALLBACK_CLIENT_WRITEABLE && C.Counters.FindRef(Double) == 0)
		{
			C.Counters.Add(Double, 1);
			C.Rig->WriteNow(C, Wsi, LWS_WRITE_PING, Utf8Bytes(TEXT("one")), TEXT("first"));
			C.Rig->WriteNow(C, Wsi, LWS_WRITE_TEXT, Utf8Bytes(TEXT("two")), TEXT("second"));
			return 0;
		}
		return KeepDefault;
	};
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	Rig.ExecOn(Conn, TEXT("kick"), [](FConn& C) { lws_callback_on_writable(C.Wsi); });
	NoteOutcome(*this, TEXT("both writes ran"), Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == WriteDone; }).Num() >= 2;
	}));
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	TArray<FString> Notes;
	for (const FEvent& Each : OfReason(Rig.Events(Conn), WriteDone))
	{
		Notes.Add(Each.Note);
	}
	const bool bPing = JournalEvents(Journal, TEXT("frame")).ContainsByPredicate([](const FVaRestJson& Each) { return Each[TEXT("op")].AsInt() == 9 && JournalBytes(Each) == Utf8Bytes(TEXT("one")); });
	const bool bText = JournalEvents(Journal, TEXT("message")).ContainsByPredicate([](const FVaRestJson& Each) { return Each[TEXT("text")].AsString().Equals(TEXT("two"), ESearchCase::CaseSensitive); });
	Fact(*this, TEXT("tx.two_writes_one_writeable"), EVerdict::Recorded,
		FString::Printf(TEXT("PING arrived: %s, text arrived: %s; %s"), YesNo(bPing), YesNo(bText), *FString::Join(Notes, TEXT("; "))),
		TEXT("WebSocket spec 2.4 writes one frame per WRITEABLE; the engine's comment says 3.0 allows only one (LwsWebSocket.cpp:195)"),
		TEXT("a PING and a text written in the same WRITEABLE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeAutoReplyTest, "Probe.VaRest.Lws.Data.AutoReplyBoundary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeAutoReplyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	// The server pushes "2" after our first data frame (a heartbeat, as Engine.IO's); we answer "3" at the next message
	// boundary, never inside message A, and message B goes in behind the answer (WebSocket spec 2.4 rev 2, 22 SW-5)
	Spec.Path = TEXT("/ws/sink?push=2&after_frames=1&key=") + Key;
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		const FName Reply(TEXT("reply"));
		if (Reason == LWS_CALLBACK_CLIENT_RECEIVE && Len == 1 && static_cast<const uint8*>(In)[0] == '2' && C.Counters.FindRef(Reply) == 0)
		{
			C.Counters.Add(Reply, 1);
			// Len: the fragments of A still queued when the push arrived
			C.Rig->Record(C.Id, Marker, C.Outbox.Num(), TEXT("push"));
			lws_callback_on_writable(Wsi);
			return KeepDefault;
		}
		if (Reason == LWS_CALLBACK_CLIENT_WRITEABLE && C.Counters.FindRef(Reply) == 1)
		{
			if (C.Outbox.Num() == 0 || IsMessageStart(C.Outbox[0].Protocol))
			{
				C.Counters.Add(Reply, 2);
				const uint8 Three = '3';
				C.Rig->WriteNow(C, Wsi, LWS_WRITE_TEXT, TConstArrayView<uint8>(&Three, 1), TEXT("reply"));
				QueueTextMessage(C.Outbox, 'B', AutoReplyFragmentBytes, AutoReplyFragments);
				lws_callback_on_writable(Wsi);
				return 0;
			}
			// Review I-1: inside A the default write below sends the next fragment, and FRig::WriteNext re-arms only while
			// its outbox is non-empty; one more WRITEABLE, so this check runs again after A's last fragment
			lws_callback_on_writable(Wsi);
		}
		return KeepDefault;
	};
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	Rig.ExecOn(Conn, TEXT("queue A"), [](FConn& C)
	{
		QueueTextMessage(C.Outbox, 'A', AutoReplyFragmentBytes, AutoReplyFragments);
		lws_callback_on_writable(C.Wsi);
	});
	const int32 Expected = AutoReplyFragments * 2 + 1;
	const bool bWritten = Rig.WaitFor([Conn, Expected](const TArray<FEvent>& All)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == WriteDone; }).Num() >= Expected;
	}, 30.0);
	NoteOutcome(*this, TEXT("A, the reply and B were all written"), bWritten);
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn, 30.0));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	TArray<FString> Order;
	for (const FVaRestJson& Each : Messages)
	{
		Order.Add(Each.Has(TEXT("text")) ? FString::Printf(TEXT("\"%s\""), *Each[TEXT("text")].AsString()) : FString::Printf(TEXT("%lld bytes"), Each[TEXT("len")].AsInt64()));
	}
	const int64 MessageBytes = static_cast<int64>(AutoReplyFragmentBytes) * AutoReplyFragments;
	const bool bBetween = Messages.Num() == 3 && Messages[0][TEXT("len")].AsInt64() == MessageBytes
		&& Messages[1][TEXT("text")].AsString().Equals(TEXT("3"), ESearchCase::CaseSensitive) && Messages[2][TEXT("len")].AsInt64() == MessageBytes;
	FEvent Push;
	FEvent Replied;
	const bool bPushed = Rig.Find(Conn, Marker, Push, TEXT("push"));
	const bool bTimed = bPushed && Rig.Find(Conn, WriteDone, Replied, TEXT("reply"));
	// Review I-1: the boundary is exercised only when the push lands while fragments of A are still queued
	const bool bExercised = bPushed && Push.Len > 0;
	const FString AtServer = FString::Printf(TEXT("messages at the server: %s"), *FString::Join(Order, TEXT(", ")));
	Fact(*this, TEXT("tx.autoreply_between_messages"), bExercised ? AgreesIf(bBetween) : EVerdict::Recorded,
		bExercised ? FString::Printf(TEXT("%s; the push arrived with %lld of A's %d fragments still queued"), *AtServer, Push.Len, AutoReplyFragments)
			: FString::Printf(TEXT("%s; push arrived after A (%s): boundary not exercised"), *AtServer, bPushed ? TEXT("no fragment of A queued") : TEXT("no push seen")),
		TEXT("WebSocket spec 2.4 rev 2 (22 SW-5): a reply is written at the next message boundary, never inside a fragmented message"),
		TEXT("A and B: 2 MiB text in 32 fragments each; the reply \"3\" to the server's \"2\"; the boundary rule is the rig's own (the hook writes the reply only when the outbox head starts a message), so the row checks LWS's write order and the delay"));
	Fact(*this, TEXT("tx.autoreply_delay_ms"), EVerdict::Recorded,
		bTimed ? FString::Printf(TEXT("%.1f ms from the push to the reply's lws_write"), (Replied.Time - Push.Time) * 1000.0) : FString(TEXT("not measured")),
		TEXT("none"), TEXT("bounded by the fragments of A still queued"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeSoSndbufTest, "Probe.VaRest.Lws.Data.SoSndbuf",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeSoSndbufTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	const TCHAR* const SndbufSpec = TEXT("WebSocket spec 2.4 rev 2 (22 SW-5): connections with auto-reply rules get SO_SNDBUF 256 KiB through lws_get_socket_fd + setsockopt");
#if PLATFORM_LINUX || PLATFORM_MAC
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	FRigOptions Options;
	Options.Tls = ETls::LwsGlobalInit;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FHook SetSndbuf = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		if (Reason == LWS_CALLBACK_CLIENT_ESTABLISHED)
		{
			const lws_sockfd_type Fd = lws_get_socket_fd(Wsi);
			int Asked = 256 * 1024;
			const int Set = setsockopt(Fd, SOL_SOCKET, SO_SNDBUF, &Asked, sizeof(Asked));
			int Got = 0;
			socklen_t GotLen = sizeof(Got);
			getsockopt(Fd, SOL_SOCKET, SO_SNDBUF, &Got, &GotLen);
			C.Rig->Record(C.Id, Marker, Got, FString::Printf(TEXT("setsockopt(SO_SNDBUF, 262144) returned %d on fd %d; getsockopt reports %d"), Set, static_cast<int32>(Fd), Got));
		}
		return KeepDefault;
	};
	FConnectSpec Plain;
	Plain.Hook = SetSndbuf;
	FConnectSpec Tls;
	Tls.bTls = true;
	Tls.SslFlags = LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	Tls.Hook = SetSndbuf;
	TArray<FEvent> PlainSeen;
	TArray<FEvent> TlsSeen;
	Visit(*this, Rig, Plain, PlainSeen);
	Visit(*this, Rig, Tls, TlsSeen);
	const FEvent* PlainMark = PlainSeen.FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker; });
	const FEvent* TlsMark = TlsSeen.FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker; });
	Fact(*this, TEXT("tx.so_sndbuf_ws"), EVerdict::Recorded, PlainMark ? PlainMark->Note : FString::Printf(TEXT("never opened: %s"), *ErrorOf(PlainSeen)), SndbufSpec, TEXT("ws://, in CLIENT_ESTABLISHED"));
	Fact(*this, TEXT("tx.so_sndbuf_wss"), EVerdict::Recorded, TlsMark ? TlsMark->Note : FString::Printf(TEXT("never opened: %s"), *ErrorOf(TlsSeen)), SndbufSpec, TEXT("wss:// (the TCP socket under TLS), in CLIENT_ESTABLISHED"));
	return true;
#else
	Fact(*this, TEXT("tx.so_sndbuf_ws"), EVerdict::Unverified, TEXT("unverified here; release-pass item (Win64): setsockopt takes a SOCKET and a char* there"), SndbufSpec, TEXT("this platform"));
	Fact(*this, TEXT("tx.so_sndbuf_wss"), EVerdict::Unverified, TEXT("unverified here; release-pass item (Win64)"), SndbufSpec, TEXT("this platform"));
	return true;
#endif
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build and run the group**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Data`
Expected: `run_tests: 7 passed, 0 failed` (a test fails only on a harness invariant, decision 3); `grep -c 'FACT {"id":"\(rx\|tx\)\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `26`.

Controls: `rx.bytes_intact` must agree. If it does not, the rig's byte copies are wrong (`FRig::CopyBytes`), and no other `rx.` row can be trusted until that is fixed. `tx.message_intact` must agree too, or the outbox is mangling buffers.

- [ ] **Step 4: Commit**

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeDataTest.cpp
git commit -m "tests: LWS 3.0 probe -- receive chunks and flags, order, flow control pause/resume, NO_FIN sends, large and double writes, reply between messages, SO_SNDBUF (WS W0)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 9: Probe — TLS paths and coexistence with the engine's WebSockets module

**Files:**
- Create: `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeTlsTest.cpp`

**Interfaces:**
- Consumes (Task 4): `FRig` (`UsedSslModuleContext`, `Connect`, `WriteText`, `Close`, `WaitFor`, `WaitEnded`, `Events`, `Record`, `GetServer`), `FRigOptions` (`Tls`, `ProvidedSslContext`, `OnLoadExtraClientVerifyCerts`, `OnVerifyServerCert`), `ETls`, `FConn`, `FConnectSpec` (`bTls`, `SslFlags`, `Address`, `Host`, `Hook`), `Visit`, `Opened`, `ErrorOf`, `EventText`, `OfReason`, `FetchBytes`, `AddPemToStore`, `Fact`, `AgreesIf`, `YesNo`, `Marker`, `ConnectReturned`, `KeepDefault`; `openssl_websocket_private_data_index` (libwebsockets.a `ssl.c.o`, Linux only, decision 16); engine: `FSslModule::Get()` (`SSL_API`), `ISslCertificateManager::AddCertificatesToSslContext`, `VerifySslCertificates(X509_STORE_CTX*, const FString&)` (`Runtime/Online/SSL/Public/Interfaces/ISslCertificateManager.h:28`, `:66`), `FWebSocketsModule` via `FModuleManager::LoadModuleChecked` (virtual `CreateWebSocket`, `WebSocketsModule.h:72`), `IWebSocket` (`Connect`, `Send`, `Close`, `OnConnected`, `OnMessage`, `OnConnectionError`, `OnClosed`; `IWebSocket.h:16-70`); `VaRest::Http::Testing::FLogCapture`.
- Produces facts: `tls.ssl_module_ctx`, `tls.load_extra_fired`, `tls.insecure_localhost`, `tls.insecure_ip_host`, `tls.insecure_skip_hostname`, `tls.version`, `tls.untrusted_rejected`, `tls.trusted_ca_verified`, `tls.hostname_with_port`, `tls.hostname_ip_literal`, `tls.verify_callback_calls`, `tls.provided_ctx_connects`, `tls.provided_ctx_load_extra_fired`, `coexist.shared_lws_state`, `coexist.lws_symbol_owner`, `coexist.both_ws`, `coexist.both_wss`, `coexist.engine_after_probe_teardown`, `coexist.engine_lws_log_routed`, `coexist.context_create_reallocates_ssl_index`.
- Tests: `Probe.VaRest.Lws.Tls.EnginePath`, `.Tls.Verification`, `.Tls.ProvidedContext`, `Probe.VaRest.Lws.Coexist.EngineWebSocket`.

- [ ] **Step 1: Run the filter to see it match nothing**

Run: `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Tls`
Expected: exit 3, `matched no tests`.

- [ ] **Step 2: Write the probe tests**

Create `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeTlsTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): TLS through the engine's glue (the SSL module's context or LWS's own, the certificate
// manager's roots and verify step), host-name checks against the test certificate, a provided SSL_CTX (the monolithic
// path's mechanics), and an engine IWebSocket next to a probe context, ws and wss (WebSocket spec 1.2, 2.1, 2.2)

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "IWebSocket.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "Ssl.h"
#include "VaRestHttpTesting.h"
#include "WebSocketsModule.h"

#if PLATFORM_LINUX
#include <dlfcn.h>

// libwebsockets.a's ssl.c.o defines this as a common symbol (nm: C) and declares it in no header; lws_context_init_ssl_library
// stores a fresh CRYPTO_get_ex_new_index into it on every lws_create_context (WS W0 decision 16, review I-3)
extern "C" int openssl_websocket_private_data_index;
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate
{
	/** The domain the engine verifies against: the Host header (or address) without its port */
	FString DomainOf(const FConn& Conn)
	{
		const FString Host = !Conn.Spec.Host.IsEmpty() ? Conn.Spec.Host : !Conn.Spec.Address.IsEmpty() ? Conn.Spec.Address : Conn.Rig->GetServer().Host;
		int32 Colon = INDEX_NONE;
		return Host.FindChar(TEXT(':'), Colon) ? Host.Left(Colon) : Host;
	}

	/**
	 * The engine's verify step (LwsWebSocket.cpp:549-572, bDisableCertValidation off): a chain OpenSSL accepted still
	 * passes the certificate manager's check (pinned keys) for the URL's domain; anything else fails
	 */
	int32 EngineVerify(FConn& Conn, X509_STORE_CTX* Store, int64 PreverifyOk)
	{
		int32 Ok = static_cast<int32>(PreverifyOk);
		const FString Domain = DomainOf(Conn);
		if (Ok == 1 && !FSslModule::Get().GetCertificateManager().VerifySslCertificates(Store, Domain))
		{
			Ok = 0;
		}
		Conn.Rig->Record(Conn.Id, Marker, Ok, FString::Printf(TEXT("engine verify domain=%s ok=%d"), *Domain, Ok));
		return Ok == 1 ? 0 : 1;
	}

	/** The engine's roots, as LwsWebSocketsManager.cpp:441-446 adds them */
	void EngineRoots(SSL_CTX* Ctx)
	{
		FSslModule::Get().GetCertificateManager().AddCertificatesToSslContext(Ctx);
	}

	/** On ESTABLISHED: the negotiated TLS version, as a Marker */
	int32 RecordTlsVersion(FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
	{
		if (Reason == LWS_CALLBACK_CLIENT_ESTABLISHED)
		{
			SSL* Ssl = lws_get_ssl(Wsi);
			const char* Version = Ssl ? SSL_get_version(Ssl) : "none";
			C.Rig->Record(C.Id, Marker, 0, FString::Printf(TEXT("tls=%s"), *Utf8Text(Version, FCStringAnsi::Strlen(Version))));
		}
		return KeepDefault;
	}

	/** A wss connection to the TLS listener: TCP to 127.0.0.1, Host (and the checked name) as given */
	FConnectSpec Tls(const FServer& Site, const FString& HostName, int32 Flags)
	{
		FConnectSpec Spec;
		Spec.bTls = true;
		Spec.Address = Site.Host;
		Spec.Host = FString::Printf(TEXT("%s:%d"), *HostName, Site.TlsPort);
		Spec.SslFlags = Flags;
		Spec.Path = TEXT("/ws/echo?key=") + NewKey();
		Spec.Hook = &RecordTlsVersion;
		return Spec;
	}

	/** "established (tls=...)" or "refused: ...", with the verify callback's notes */
	FString TlsOutcome(const TArray<FEvent>& Seen)
	{
		TArray<FString> Notes;
		for (const FEvent& Each : Seen)
		{
			if (Each.Reason == LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION || Each.Reason == Marker)
			{
				Notes.Add(Each.Note);
			}
		}
		return FString::Printf(TEXT("%s [%s]"), Opened(Seen) ? TEXT("established") : *FString::Printf(TEXT("refused: %s"), *ErrorOf(Seen)),
			*FString::Join(Notes, TEXT("; ")));
	}

	bool LoadExtraFired(FRig& Rig)
	{
		return Rig.Events().ContainsByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS; });
	}

	/** One engine IWebSocket and what it reported (delegates run on the game thread, from the core ticker) */
	struct FEngineSide
	{
		bool bConnected = false;
		bool bEnded = false;
		FString Error;
		TArray<FString> Messages;
	};

	TSharedRef<IWebSocket> OpenEngineSocket(FWebSocketsModule& Module, const FString& Url, const TSharedRef<FEngineSide>& Side)
	{
		// CreateWebSocket is virtual (WebSocketsModule.h:72): no link to the WebSockets module (decision 12). Connect from the
		// test body, never from a handler (WebSocket spec E25)
		TSharedRef<IWebSocket> Socket = Module.CreateWebSocket(Url, FString());
		Socket->OnConnected().AddLambda([Side] { Side->bConnected = true; });
		Socket->OnMessage().AddLambda([Side](const FString& Message) { Side->Messages.Add(Message); });
		Socket->OnConnectionError().AddLambda([Side](const FString& Reason) { Side->Error = Reason; Side->bEnded = true; });
		Socket->OnClosed().AddLambda([Side](int32 Code, const FString& Reason, bool bWasClean) { Side->bEnded = true; });
		Socket->Connect();
		return Socket;
	}

	bool EngineEchoed(const TSharedRef<FEngineSide>& Side, const FString& Text)
	{
		return Side->Messages.ContainsByPredicate([&Text](const FString& Each) { return Each.Equals(Text, ESearchCase::CaseSensitive); });
	}

	/** Both open at once, both echo, both close and end; false when any step fails */
	bool BothEcho(FRig& Rig, int32 Conn, const TSharedRef<IWebSocket>& Socket, const TSharedRef<FEngineSide>& Side, FString& OutWhy)
	{
		Rig.WaitFor([Conn, Side](const TArray<FEvent>& All)
		{
			const bool bProbeSettled = All.ContainsByPredicate([Conn](const FEvent& Each)
			{
				return Each.Conn == Conn && (Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_WSI_DESTROY);
			});
			return bProbeSettled && (Side->bConnected || Side->bEnded);
		}, 20.0);
		const bool bBothOpen = Side->bConnected && Opened(Rig.Events(Conn));
		bool bEchoed = false;
		if (bBothOpen)
		{
			Socket->Send(TEXT("engine-hello"));
			Rig.WriteText(Conn, TEXT("probe-hello"));
			bEchoed = Rig.WaitFor([Conn, Side](const TArray<FEvent>& All)
			{
				return EngineEchoed(Side, TEXT("engine-hello")) && All.ContainsByPredicate([Conn](const FEvent& Each)
				{
					return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("probe-hello"), ESearchCase::CaseSensitive);
				});
			});
		}
		// Both drain, whatever happened
		if (Side->bConnected && !Side->bEnded)
		{
			Socket->Close(1000, TEXT("done"));
		}
		Rig.Close(Conn, 1000, TEXT("done"));
		const bool bEnded = Rig.WaitFor([Conn, Side](const TArray<FEvent>& All)
		{
			return (Side->bEnded || !Side->bConnected) && All.ContainsByPredicate([Conn](const FEvent& Each)
			{
				return Each.Conn == Conn && (Each.Reason == LWS_CALLBACK_WSI_DESTROY || (Each.Reason == ConnectReturned && Each.Note == TEXT("null")));
			});
		}, 20.0);
		OutWhy = FString::Printf(TEXT("engine: connected=%s ended=%s error=\"%s\" echoed=%s; probe: opened=%s; both ended=%s"),
			YesNo(Side->bConnected), YesNo(Side->bEnded), *Side->Error, YesNo(EngineEchoed(Side, TEXT("engine-hello"))), YesNo(Opened(Rig.Events(Conn))), YesNo(bEnded));
		return bBothOpen && bEchoed && bEnded;
	}

	/**
	 * A log category's current verbosity as "Log list <cat>" prints it ("%-40s  %-12s", LogSuppressionInterface.cpp:593-614),
	 * or empty when it is not listed or exec commands are compiled out
	 */
	FString LogVerbosityOf(const TCHAR* Category)
	{
		FString Out;
#if UE_ALLOW_EXEC_COMMANDS
		FStringOutputDevice Listing;
		Listing.SetAutoEmitLineTerminator(true);
		GEngine->Exec(nullptr, *FString::Printf(TEXT("Log list %s"), Category), Listing);
		TArray<FString> Lines;
		Listing.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> Tokens;
			Line.ParseIntoArrayWS(Tokens);
			// "Log list" matches substrings: take only the category itself
			if (Tokens.Num() >= 2 && Tokens[0].Equals(Category, ESearchCase::CaseSensitive))
			{
				Out = Tokens[1];
			}
		}
#endif
		return Out;
	}

	/** lwsl_visible reads this module's copy of LWS's log level (libwebsockets.h:434-443) */
	FString LogBits()
	{
		return FString::Printf(TEXT("err=%d warn=%d notice=%d info=%d client=%d"), lwsl_visible(LLL_ERR), lwsl_visible(LLL_WARN), lwsl_visible(LLL_NOTICE),
			lwsl_visible(LLL_INFO), lwsl_visible(LLL_CLIENT));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTlsEnginePathTest, "Probe.VaRest.Lws.Tls.EnginePath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTlsEnginePathTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	// OpenSSL's verify errors may reach the engine's LWS log routing when the copy is shared (coexist.shared_lws_state)
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FRigOptions Options;
	Options.Tls = ETls::SslModule;
	Options.OnLoadExtraClientVerifyCerts = &EngineRoots;
	Options.OnVerifyServerCert = &EngineVerify;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	Fact(*this, TEXT("tls.ssl_module_ctx"), AgreesIf(Rig.UsedSslModuleContext()),
		Rig.UsedSslModuleContext() ? FString(TEXT("the SSL module's SSL_CTX (ISslManager::CreateSslContext) was provided"))
			: FString::Printf(TEXT("no SSL module context in this %s build: InitializeSsl() is false, CreateSslContext null (SslManager.cpp:26-55, :76-131); LWS made its own (DO_SSL_GLOBAL_INIT)"), IS_MONOLITHIC ? TEXT("monolithic") : TEXT("modular")),
		TEXT("WebSocket spec 2.1: SSL_CTX from ISslManager::CreateSslContext (ISslManager.h:64-81)"),
		TEXT("FRigOptions::Tls = SslModule: the engine's order, LwsWebSocketsManager.cpp:205-225"));
	Fact(*this, TEXT("tls.load_extra_fired"), AgreesIf(LoadExtraFired(Rig)), YesNo(LoadExtraFired(Rig)),
		TEXT("WebSocket spec 2.2: LOAD_EXTRA_CLIENT_VERIFY_CERTS -> AddCertificatesToSslContext"),
		TEXT("the context's callbacks during lws_create_context"));
	TArray<FEvent> LocalSeen;
	TArray<FEvent> IpSeen;
	TArray<FEvent> SkipSeen;
	// A refused handshake, or a connection that never ends, is a fact (Visit notes it), not a failure (review M-2)
	Visit(*this, Rig, Tls(Site, TEXT("localhost"), LCCSCF_ALLOW_SELFSIGNED), LocalSeen);
	Visit(*this, Rig, Tls(Site, Site.Host, LCCSCF_ALLOW_SELFSIGNED), IpSeen);
	Visit(*this, Rig, Tls(Site, Site.Host, LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK), SkipSeen);
	const FString InsecureSpec = TEXT("WebSocket spec 2.2: wss+insecure means self-signed allowed (LCCSCF_ALLOW_SELFSIGNED, as LwsWebSocket.cpp:747-750); 16.3: the test server is reached that way");
	Fact(*this, TEXT("tls.insecure_localhost"), EVerdict::Recorded, TlsOutcome(LocalSeen), InsecureSpec,
		TEXT("LCCSCF_ALLOW_SELFSIGNED, Host localhost:<port>, the chain leaf + test CA, the engine's roots and verify step"));
	Fact(*this, TEXT("tls.insecure_ip_host"), EVerdict::Recorded, TlsOutcome(IpSeen), InsecureSpec,
		TEXT("LCCSCF_ALLOW_SELFSIGNED, Host 127.0.0.1:<port>: the engine's wss+insecure on an IP URL"));
	Fact(*this, TEXT("tls.insecure_skip_hostname"), EVerdict::Recorded, TlsOutcome(SkipSeen), InsecureSpec,
		TEXT("LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK, Host 127.0.0.1:<port>"));
	FString Version = TEXT("no TLS connection opened");
	for (const TArray<FEvent>* Seen : { &LocalSeen, &IpSeen, &SkipSeen })
	{
		const FEvent* Mark = Seen->FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker && Each.Note.StartsWith(TEXT("tls=")); });
		if (Mark)
		{
			Version = Mark->Note;
			break;
		}
	}
	Fact(*this, TEXT("tls.version"), EVerdict::Recorded, Version, TEXT("none (ESslTlsProtocol stops at TLSv1_2, ISslManager.h:14-33; OpenSSL 1.1.1 also speaks 1.3)"),
		TEXT("SSL_get_version(lws_get_ssl(wsi)) at ESTABLISHED"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTlsVerificationTest, "Probe.VaRest.Lws.Tls.Verification",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTlsVerificationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TArray<uint8> Ca;
	if (!FetchBytes(*this, Site, TEXT("/tls/ca.pem"), Ca))
	{
		return false;
	}
	// Untrusted: the engine's roots only, and full verification (LCCSCF_USE_SSL, nothing allowed)
	FRigOptions Untrusting;
	Untrusting.Tls = ETls::SslModule;
	Untrusting.OnLoadExtraClientVerifyCerts = &EngineRoots;
	Untrusting.OnVerifyServerCert = &EngineVerify;
	TArray<FEvent> UntrustedSeen;
	// One TLS rig alive at a time (decision 16)
	{
		FRig Rig(Site, MoveTemp(Untrusting));
		if (!TestTrue(TEXT("the untrusting context exists"), Rig.IsReady()))
		{
			return false;
		}
		Visit(*this, Rig, Tls(Site, TEXT("localhost"), 0), UntrustedSeen);
	}
	// Trusted: the same, with the test CA in the store
	FRigOptions Trusting;
	Trusting.Tls = ETls::SslModule;
	Trusting.OnLoadExtraClientVerifyCerts = [Ca](SSL_CTX* Ctx)
	{
		EngineRoots(Ctx);
		AddPemToStore(Ctx, Ca);
	};
	Trusting.OnVerifyServerCert = &EngineVerify;
	TArray<FEvent> TrustedSeen;
	TArray<FEvent> IpSeen;
	{
		FRig Rig(Site, MoveTemp(Trusting));
		if (!TestTrue(TEXT("the trusting context exists"), Rig.IsReady()))
		{
			return false;
		}
		Visit(*this, Rig, Tls(Site, TEXT("localhost"), 0), TrustedSeen);
		Visit(*this, Rig, Tls(Site, Site.Host, 0), IpSeen);
	}
	Fact(*this, TEXT("tls.untrusted_rejected"), AgreesIf(!Opened(UntrustedSeen)), TlsOutcome(UntrustedSeen),
		TEXT("WebSocket spec 2.2: wss verifies the chain (engine roots, VerifySslCertificates); a chain from an unknown CA is refused"),
		TEXT("LCCSCF_USE_SSL only, the test CA not trusted, Host localhost:<port>"));
	Fact(*this, TEXT("tls.trusted_ca_verified"), AgreesIf(Opened(TrustedSeen)), TlsOutcome(TrustedSeen),
		TEXT("WebSocket spec 2.2: a chain to a trusted root, then the certificate manager's check, opens"),
		TEXT("LCCSCF_USE_SSL only, the test CA added in LOAD_EXTRA_CLIENT_VERIFY_CERTS, Host localhost:<port>"));
	Fact(*this, TEXT("tls.hostname_with_port"), EVerdict::Recorded,
		Opened(TrustedSeen) ? FString(TEXT("Host \"localhost:<port>\" passed the name check: LWS checks the host without its port"))
			: FString::Printf(TEXT("the trusted connection did not open: %s"), *ErrorOf(TrustedSeen)),
		TEXT("WebSocket spec 2.2: Host keeps a non-default port (LwsWebSocket.cpp:775-785), and TLS must still check the name"),
		TEXT("the trusted connection above"));
	Fact(*this, TEXT("tls.hostname_ip_literal"), EVerdict::Recorded, TlsOutcome(IpSeen),
		TEXT("none: the certificate has IP:127.0.0.1 in its SAN; X509_VERIFY_PARAM_set1_host compares DNS names"),
		TEXT("LCCSCF_USE_SSL only, the test CA trusted, Host 127.0.0.1:<port>"));
	TArray<FString> Calls;
	for (const FEvent& Each : OfReason(TrustedSeen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION))
	{
		Calls.Add(Each.Note);
	}
	for (const FEvent& Each : OfReason(UntrustedSeen, LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION))
	{
		Calls.Add(TEXT("untrusted: ") + Each.Note);
	}
	Fact(*this, TEXT("tls.verify_callback_calls"), EVerdict::Recorded, Calls.Num() > 0 ? FString::Join(Calls, TEXT("; ")) : FString(TEXT("PERFORM_SERVER_CERT_VERIFICATION never called")),
		TEXT("WebSocket spec 2.2: PERFORM_SERVER_CERT_VERIFICATION -> VerifySslCertificates, with the user-data remap (LwsWebSocketsManager.cpp:447-456)"),
		TEXT("the trusted and the untrusted connection"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTlsProvidedContextTest, "Probe.VaRest.Lws.Tls.ProvidedContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTlsProvidedContextTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TArray<uint8> Ca;
	if (!FetchBytes(*this, Site, TEXT("/tls/ca.pem"), Ca))
	{
		return false;
	}
	// Decision 13: the mechanics of the monolithic path (a context made before lws_create_context and provided, no
	// DO_SSL_GLOBAL_INIT, LwsWebSocketsManager.cpp:210-215), with the test CA instead of the engine's roots
	SSL_CTX* Provided = SSL_CTX_new(TLS_client_method());
	if (!TestNotNull(TEXT("an SSL_CTX"), Provided))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		// After the rig: LWS does not free a provided context (libwebsockets.h:2765-2769)
		SSL_CTX_free(Provided);
	};
	TestEqual(TEXT("the test CA is in the provided context's store"), AddPemToStore(Provided, Ca), 1);
	TArray<FEvent> Seen;
	bool bLoadExtra = false;
	{
		FRigOptions Options;
		Options.Tls = ETls::ProvidedContext;
		Options.ProvidedSslContext = Provided;
		FRig Rig(Site, MoveTemp(Options));
		if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return false;
		}
		bLoadExtra = LoadExtraFired(Rig);
		Visit(*this, Rig, Tls(Site, TEXT("localhost"), 0), Seen);
	}
	Fact(*this, TEXT("tls.provided_ctx_connects"), EVerdict::Recorded, TlsOutcome(Seen),
		TEXT("T15: TLS through a provided SSL_CTX (provided_client_ssl_ctx, libwebsockets.h:2764-2772); WebSocket spec 2.1's monolithic path"),
		TEXT("SSL_CTX_new(TLS_client_method()) + the test CA, no DO_SSL_GLOBAL_INIT, LCCSCF_USE_SSL, Host localhost:<port>"));
	Fact(*this, TEXT("tls.provided_ctx_load_extra_fired"), EVerdict::Recorded, YesNo(bLoadExtra),
		TEXT("WebSocket spec 2.2 relies on LOAD_EXTRA_CLIENT_VERIFY_CERTS for the engine's roots; with a provided context they come from CreateSslContext itself (bAddCertificates, ISslManager.h:46-47, SslManager.cpp:114-117)"),
		TEXT("the provided-context rig's callbacks during lws_create_context"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeCoexistTest, "Probe.VaRest.Lws.Coexist.EngineWebSocket",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeCoexistTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeTlsTestPrivate;
#if WITH_WEBSOCKETS
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	// The engine's LWS lines (LwsWebSocketsManager.cpp:403-424) are this test's evidence, not failures
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	VaRest::Http::Testing::FLogCapture Capture;

	// 1. Process-global LWS state: one copy or two (WebSocket spec 1.2, T16)
	const bool bLoadedBefore = FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets"));
	const FString Before = LogBits();
	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	const FString After = LogBits();
	// The engine's InitWebSockets enables LLL_CLIENT (LwsWebSocketsManager.cpp:176); LWS's default is err|warn|notice
	// (libwebsockets.h:407-418). Seen here, the engine's call reached this module's copy.
	const bool bShared = lwsl_visible(LLL_CLIENT) != 0;
	Fact(*this, TEXT("coexist.shared_lws_state"), AgreesIf(bShared),
		FString::Printf(TEXT("%s: this module's log state before loading WebSockets %s, after %s (WebSockets loaded before this test: %s)"),
			bShared ? TEXT("shared with the engine's copy") : TEXT("separate from the engine's copy"), *Before, *After, YesNo(bLoadedBefore)),
		TEXT("WebSocket spec 1.2: on Linux both modules' lws_* calls resolve to whichever copy loaded first, so process-global state is shared"),
		TEXT("lwsl_visible in VaRestTests' copy around FWebSocketsModule's InitWebSockets; UE dlopens modules RTLD_LOCAL (UnixPlatformProcess.cpp:107-148)"));
#if PLATFORM_LINUX
	FString OwnerFile = TEXT("unknown");
	Dl_info Info = {};
	if (dladdr(reinterpret_cast<void*>(&lws_create_context), &Info) != 0 && Info.dli_fname)
	{
		OwnerFile = FPaths::GetCleanFilename(UTF8_TO_TCHAR(Info.dli_fname));
	}
	// Review M-4: the engine module's own path, from its object's vtable pointer (the Itanium ABI puts it first, in the
	// module's .so), not a bare soname that depends on the search path
	FString EngineCopy = TEXT("the WebSockets module's object was not found in any loaded file");
	Dl_info ModuleInfo = {};
	void* EngineLib = dladdr(*reinterpret_cast<void**>(&Module), &ModuleInfo) != 0 && ModuleInfo.dli_fname
		? dlopen(ModuleInfo.dli_fname, RTLD_LAZY | RTLD_NOLOAD) : nullptr;
	if (EngineLib)
	{
		void* EngineSymbol = dlsym(EngineLib, "lws_create_context");
		EngineCopy = !EngineSymbol ? TEXT("the engine module exports no lws_create_context")
			: EngineSymbol == reinterpret_cast<void*>(&lws_create_context) ? TEXT("the engine module's lws_create_context is the same function")
			: TEXT("the engine module has its own copy at another address");
		// RTLD_NOLOAD took a reference: give it back
		dlclose(EngineLib);
	}
	const FString EngineFile = ModuleInfo.dli_fname ? FPaths::GetCleanFilename(UTF8_TO_TCHAR(ModuleInfo.dli_fname)) : FString(TEXT("(unknown)"));
	Fact(*this, TEXT("coexist.lws_symbol_owner"), EVerdict::Recorded,
		FString::Printf(TEXT("this module's lws_create_context is in %s; the WebSockets module is %s: %s"), *OwnerFile, *EngineFile, *EngineCopy),
		TEXT("WebSocket spec 1.2: libUnrealEditor-WebSockets.so exports 185 lws_* functions"),
		TEXT("dladdr of lws_create_context; dladdr of the FWebSocketsModule object's vtable, then dlopen(that path, RTLD_NOLOAD) + dlsym"));
#else
	Fact(*this, TEXT("coexist.lws_symbol_owner"), EVerdict::Unverified,
		TEXT("unverified here; release-pass item (Win64, Mac): separate DLL / two-level-namespace copies by construction"),
		TEXT("WebSocket spec 1.2"), TEXT("this platform"));
#endif

	// 1b. Review I-3: does creating a context re-allocate the OpenSSL ex-data index this LWS copy's verify callback reads?
	// Rigs made one after another, no TLS connection in flight (decision 16)
#if PLATFORM_LINUX
	const int32 IndexBefore = openssl_websocket_private_data_index;
	int32 IndexAfterPlain = IndexBefore;
	int32 IndexAfterTls = IndexBefore;
	{
		FRig PlainRig(Site);
		if (!TestTrue(TEXT("the plain probe context exists"), PlainRig.IsReady()))
		{
			return false;
		}
		IndexAfterPlain = openssl_websocket_private_data_index;
	}
	{
		FRigOptions TlsOptions;
		TlsOptions.Tls = ETls::LwsGlobalInit;
		FRig TlsRig(Site, MoveTemp(TlsOptions));
		if (!TestTrue(TEXT("the TLS probe context exists"), TlsRig.IsReady()))
		{
			return false;
		}
		IndexAfterTls = openssl_websocket_private_data_index;
	}
	Dl_info IndexInfo = {};
	const FString IndexFile = dladdr(reinterpret_cast<void*>(&openssl_websocket_private_data_index), &IndexInfo) != 0 && IndexInfo.dli_fname
		? FPaths::GetCleanFilename(UTF8_TO_TCHAR(IndexInfo.dli_fname)) : FString(TEXT("(unknown)"));
	Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), AgreesIf(IndexBefore == IndexAfterPlain && IndexAfterPlain == IndexAfterTls),
		FString::Printf(TEXT("openssl_websocket_private_data_index (in %s) was %d, %d after a plain context, %d after a TLS context (DO_SSL_GLOBAL_INIT)"),
			*IndexFile, IndexBefore, IndexAfterPlain, IndexAfterTls),
		TEXT("WebSocket spec 1.2: \"Contexts are independent. All connection state lives in the lws_context\""),
		TEXT("the global read around two lws_create_context calls in VaRestTests' copy; statically, lws_context_init_ssl_library stores a new CRYPTO_get_ex_new_index on both paths (ssl.c.o 0x1f4, 0x23a) and OpenSSL_client_verify_callback dereferences SSL_get_ex_data under it without a null check (openssl-client.c.o 0x1c8-0x1d6): a TLS handshake in flight across a context creation in the same copy reads NULL"));
#else
	Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), EVerdict::Unverified,
		TEXT("unverified here (static evidence only); release-pass item (Win64, Mac): the global is read on Linux only"),
		TEXT("WebSocket spec 1.2: \"Contexts are independent. All connection state lives in the lws_context\""),
		TEXT("ssl.c.o 0x1f4, 0x23a and openssl-client.c.o 0x1c8-0x1d6 (review I-3)"));
#endif

	// Decision 11 (ruling Q4): the engine's leg skips its certificate check for this test only, and connects only to the
	// local test server while it does. The flag leaves wss at LCCSCF_ALLOW_SELFSIGNED (LwsWebSocket.cpp:741-743); the
	// verify callback then sets X509_V_OK and accepts (:554-568), which is what lets an IP URL past the host-name check
	// (review M-9). The engine reads the key again on a config change (LwsWebSocketsManager.cpp:91-93, WebSocketsModule.cpp:47, :68-74).
	// The restore is armed before anything changes, so every exit path runs it (review M-3)
	bool bWasDisabled = false;
	const bool bHadKey = GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
	const FString OldVerbosity = LogVerbosityOf(TEXT("LogWebSockets"));
	ON_SCOPE_EXIT
	{
		if (bHadKey)
		{
			GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
		}
		else
		{
			// Absent before: removed, never written into the host's ini as False
			GConfig->RemoveKey(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), GEngineIni);
		}
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
#if UE_ALLOW_EXEC_COMMANDS
		if (!OldVerbosity.IsEmpty())
		{
			// The verbosity read above, not LogWebSockets' declared one
			GEngine->Exec(nullptr, *FString::Printf(TEXT("Log LogWebSockets %s"), *OldVerbosity));
		}
#endif
	};
	GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), true, GEngineIni);
	FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
#if UE_ALLOW_EXEC_COMMANDS
	// UEngine::Exec exists where exec commands are allowed (Engine.h:2434-2438, Misc/Exec.h:11-15); "Log" is handled by
	// the log suppression's Exec_Runtime (LogSuppressionInterface.cpp:589-591)
	GEngine->Exec(nullptr, TEXT("Log LogWebSockets Verbose"));
#endif

	// 2. ws and wss, an engine socket and a probe connection open at once
	FString WsWhy;
	FString WssWhy;
	bool bWs = false;
	bool bWss = false;
	{
		FRigOptions Options;
		Options.Tls = ETls::LwsGlobalInit;
		FRig Rig(Site, MoveTemp(Options));
		if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return false;
		}
		const TSharedRef<FEngineSide> EngineWs = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> EngineWsSocket = OpenEngineSocket(Module, FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *NewKey()), EngineWs);
		FConnectSpec Plain;
		Plain.Path = TEXT("/ws/echo?key=") + NewKey();
		bWs = BothEcho(Rig, Rig.Connect(MoveTemp(Plain)), EngineWsSocket, EngineWs, WsWhy);

		const TSharedRef<FEngineSide> EngineWss = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> EngineWssSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), EngineWss);
		bWss = BothEcho(Rig, Rig.Connect(Tls(Site, TEXT("localhost"), LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK)), EngineWssSocket, EngineWss, WssWhy);
	}
	Fact(*this, TEXT("coexist.both_ws"), AgreesIf(bWs), WsWhy,
		TEXT("WebSocket spec 1.2: an engine IWebSocket and a VaRest-owned LWS context work at once (ws)"), TEXT("one engine socket and one probe connection to /ws/echo at once"));
	Fact(*this, TEXT("coexist.both_wss"), AgreesIf(bWss), WssWhy,
		TEXT("WebSocket spec 1.2: the same over TLS (wss)"), TEXT("the same to the TLS listener; the probe context with DO_SSL_GLOBAL_INIT"));

	// 3. The probe's context, TLS included, is gone (lws_context_destroy with DO_SSL_GLOBAL_INIT): the engine still opens wss
	const TSharedRef<FEngineSide> Later = MakeShared<FEngineSide>();
	const TSharedRef<IWebSocket> LaterSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Later);
	VaRest::Http::Testing::PumpUntil([Later] { return Later->bConnected || Later->bEnded; }, 20.0, true);
	bool bLaterEchoed = false;
	if (Later->bConnected)
	{
		LaterSocket->Send(TEXT("after"));
		bLaterEchoed = VaRest::Http::Testing::PumpUntil([Later] { return EngineEchoed(Later, TEXT("after")); }, 20.0, true);
		LaterSocket->Close(1000, TEXT("done"));
	}
	const bool bLaterEnded = VaRest::Http::Testing::PumpUntil([Later] { return Later->bEnded || !Later->bConnected; }, 20.0, true);
	Fact(*this, TEXT("coexist.engine_after_probe_teardown"), AgreesIf(Later->bConnected && bLaterEchoed && bLaterEnded),
		FString::Printf(TEXT("engine wss after the probe's context was destroyed: connected=%s echoed=%s ended=%s error=\"%s\""),
			YesNo(Later->bConnected), YesNo(bLaterEchoed), YesNo(bLaterEnded), *Later->Error),
		TEXT("WebSocket spec 1.2: VaRest's context teardown leaves the engine's TLS working"), TEXT("a new engine wss socket after the rig's Shutdown"));

	// 4. The engine's LWS lines still reach LogWebSockets through its own emitter
	int32 LwsLines = 0;
	for (const FString& Line : Capture.GetLines())
	{
		LwsLines += Line.StartsWith(TEXT("LogWebSockets: ")) && Line.Contains(TEXT("Lws(")) ? 1 : 0;
	}
	Fact(*this, TEXT("coexist.engine_lws_log_routed"), EVerdict::Recorded,
		FString::Printf(TEXT("%d libwebsockets lines in LogWebSockets during this test (Verbose on)"), LwsLines),
		TEXT("WebSocket spec 1.2 and D30: the engine's routing to LogWebSockets (LwsWebSocketsManager.cpp:174-176) stays; VaRest never calls lws_set_log_level when WebSockets is loaded"),
		TEXT("FLogCapture over the engine sockets above"));
	// A leg that did not echo is a fact (coexist.both_ws / both_wss say which side), not a failure (review M-2)
	return true;
#else
	Fact(*this, TEXT("coexist.shared_lws_state"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0 on this platform"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.lws_symbol_owner"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.both_ws"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.both_wss"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.engine_after_probe_teardown"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.engine_lws_log_routed"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	Fact(*this, TEXT("coexist.context_create_reallocates_ssl_index"), EVerdict::Unverified, TEXT("unverified here: WITH_WEBSOCKETS is 0"), TEXT("WebSocket spec 1.2"), TEXT("this platform"));
	return true;
#endif
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build and run the group**

Run: `Tools/run_tests.sh Probe.VaRest.Lws.Tls && cp /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-tls.log && Tools/run_tests.sh --no-build Probe.VaRest.Lws.Coexist`
Expected: `run_tests: 3 passed, 0 failed` (a `Tls.*` test may fail only on a harness error; a refused handshake is a fact, not a failure), then `run_tests: 1 passed, 0 failed`. After the coexist run, check the host's ini was left alone: `grep -n bDisableCertValidation /w/Unreal/VaRestHost/Saved/Config/*/Engine.ini /w/Unreal/VaRestHost/Config/DefaultEngine.ini` shows nothing the test wrote (ruling Q4). `grep -c 'FACT {"id":"tls\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-tls.log` prints `13`; `grep -c 'FACT {"id":"coexist\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `7`.

`Coexist.EngineWebSocket` fails only on a harness invariant (decision 3): the server, a rig or the module did not start. A leg that did not echo is a fact: read the `coexist.both_ws` and `coexist.both_wss` rows (they say which side failed) before deciding whether it is the harness or the libraries. If the engine leg failed with `Invalid Domain`, the host project has an `[Online.HttpManager]` allowlist: add `127.0.0.1` to it in `/w/Unreal/VaRestHost/Config/DefaultEngine.ini` (outside this repository, a test-host setting) and re-run.

Controls: `tls.untrusted_rejected` must agree, because LWS must refuse a CA it does not trust. If it differs, stop: the probe's verify mirror would be accepting anything, and no `tls.` row is evidence until that is understood; file a P0 bead for it in Task 11.

- [ ] **Step 4: Commit**

```bash
git add Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeTlsTest.cpp
git commit -m "tests: LWS 3.0 probe -- TLS through the engine's glue, host-name checks, a provided SSL_CTX, engine IWebSocket coexistence ws/wss and log routing (WS W0)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 10: The passport — script, the full probe run, `docs/passport/<date>-lws30-probe.md`

**Files:**
- Create: `Tools/lws_probe_passport.py`
- Create (generated): `docs/passport/<run date>-lws30-probe.md`, where `<run date>` is `date +%F` on the day of the run

**Interfaces:**
- Consumes: the FACT/PENDING lines of Tasks 5–9 (`LogVaRestLwsProbe: Display: FACT {json}`, `... PENDING {json}`) in the run logs; `Engine/Build/Build.version`.
- Produces: `python3 -I Tools/lws_probe_passport.py --log <file> [<file> ...] [--log ...] --out <md> [--engine <dir>]` (exit 0 when every `EXPECTED` id was recorded, 1 with the missing ids listed otherwise) and `--selftest`. The passport sections are, in order: "Machine and build", "How the probe works", "Summary", "Missing facts" (only when ids are missing), "Differs from the spec", "Crashed", "Results" (one table per id prefix), "Re-run". Task 11 reads the "Differs" list and the `observed` column.

- [ ] **Step 1: Run the selftest to see it fail**

Run: `python3 -I Tools/lws_probe_passport.py --selftest`
Expected: `python3: can't open file '.../Tools/lws_probe_passport.py': [Errno 2] No such file or directory`.

- [ ] **Step 2: Write the script**

Create `Tools/lws_probe_passport.py`:

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Writes the libwebsockets 3.0 probe passport (WebSocket spec 16.4, milestone W0) from the FACT lines of probe runs.

    python3 -I Tools/lws_probe_passport.py --log <log> [<log> ...] --out docs/passport/<date>-lws30-probe.md
    python3 -I Tools/lws_probe_passport.py --selftest

The probe tests (Source/VaRestTests/Private/LwsProbe/) log one line per fact,
    LogVaRestLwsProbe: Display: FACT {"id", "verdict", "observed", "spec", "evidence", "test"}
and "PENDING {"id"}" right before a call that may end the process: a PENDING id with no FACT becomes "crashed" (the
Probe.VaRest.Risky.* cases run one per process, so each has its own log). Every
id in EXPECTED must be present, or the exit status is 1 and the missing ones are listed, so a probe that stopped
recording cannot pass unnoticed. Ids starting with "rig." are the harness's own checks and are left out.
"""

import argparse
import datetime
import json
import os
import platform
import re
import subprocess
import sys
import tempfile
import unittest

EXPECTED = (
    "env.lws_version", "env.openssl_version", "env.build", "env.websockets_loaded_at_start",
    "release.win64", "release.mac", "release.android_ios",
    "svc.commands_inside_wait_cancelled", "svc.commands_between_service_calls", "svc.cancel_wake_ms",
    "svc.timer_latency_ms", "svc.kill_async_ms", "svc.kill_async_ms_short_wait", "svc.kill_async_sequence",
    "svc.kill_async_close_frame",
    "timeout.default_secs", "timeout.configured_secs", "timeout.kill_pending_ms", "timeout.kill_pending_ms_short_wait",
    "timeout.kill_pending_sequence", "timeout.tcp_connect_blackhole", "timeout.default_secs_blackhole",
    "hs.unregistered_selected_bound", "hs.selected_protocol_in_filter", "hs.unregistered_selected_unbound",
    "hs.unoffered_selected", "hs.none_selected", "hs.offered_header",
    "hs.raw_origin", "hs.default_origin", "hs.origin_omitted", "hs.append_header_space", "hs.append_header_user",
    "hs.append_header_fill", "hs.host_header",
    "hs.refusal_401", "hs.refusal_403", "hs.refusal_404", "hs.refusal_429", "hs.refusal_500", "hs.refusal_503",
    "hs.refusal_sequence", "hs.callback_user_by_reason", "hs.no_upgrade_response", "hs.bad_accept",
    "hs.missing_upgrade", "hs.missing_connection", "hs.connect_refused", "hs.dns_failure",
    "hs.redirect_relative", "hs.redirect_absolute", "hs.redirect_reappends_headers", "hs.redirect_to_tls",
    "hs.redirect_downgrade", "hs.redirect_target_visible", "hs.redirect_veto", "hs.redirect_host_header",
    "hs.status_in_connection_error.refused", "hs.status_in_connection_error.connect_refused",
    "hs.status_in_connection_error.dns_failure", "hs.status_in_connection_error.no_upgrade",
    "hs.status_in_connection_error.tls_verify_failure", "hs.status_in_connection_error.killed_pending",
    "close.rewritten_codes", "close.echo_on_return_0", "close.echo_payload", "close.reason_utf8_bytes",
    "close.peer_sequence", "close.empty_len", "close.empty_echo", "close.one_byte", "close.return_1_echo",
    "close.ours_echoed_ms", "close.ours_noecho_wait_ms", "close.ours_sequence", "close.ours_long_reason",
    "close.drop_fin_sequence", "close.drop_rst_sequence", "close.drop_reports_client_closed",
    "ping.auto_pong", "ping.auto_pong_latency_ms", "ping.client_sees_server_ping", "ping.pong_payload_echo",
    "ping.rtt_ms", "ping.max_payload_125", "ping.unsolicited_pong_delivered", "ping.silent_no_pong",
    "ping.idle_survives_timeout_secs",
    "rx.frames_split_utf8", "rx.final_flag", "rx.first_flag", "rx.bytes_intact", "rx.chunk_sizes_single_frame",
    "rx.remaining_payload", "rx.binary_flag_on_continuations", "rx.control_inside_fragmented", "rx.order_preserved",
    "rx.invalid_utf8_passthrough", "rx.flow_pause_stops_callbacks", "rx.flow_resume_from_command",
    "rx.flow_resume_form", "rx.flow_backpressure_reached_server",
    "tx.no_fin_frames", "tx.ping_between_fragments", "tx.message_intact", "tx.partial_after_fragment",
    "tx.large_write_result", "tx.large_write_frames", "tx.large_write_intact", "tx.two_writes_one_writeable",
    "tx.autoreply_between_messages", "tx.autoreply_delay_ms", "tx.so_sndbuf_ws", "tx.so_sndbuf_wss",
    "tls.ssl_module_ctx", "tls.load_extra_fired", "tls.insecure_localhost", "tls.insecure_ip_host",
    "tls.insecure_skip_hostname", "tls.version", "tls.untrusted_rejected", "tls.trusted_ca_verified",
    "tls.hostname_with_port", "tls.hostname_ip_literal", "tls.verify_callback_calls", "tls.provided_ctx_connects",
    "tls.provided_ctx_load_extra_fired",
    "coexist.monolithic", "coexist.shared_lws_state", "coexist.lws_symbol_owner", "coexist.both_ws",
    "coexist.both_wss", "coexist.engine_after_probe_teardown", "coexist.engine_lws_log_routed",
    "coexist.context_create_reallocates_ssl_index",
)
# Probe.VaRest.Risky.Lws.StatusInConnectionError.<case>: one run each (WS W0 decision 10); the id is the snake-case name
RISKY_CASES = ("Refused", "ConnectRefused", "DnsFailure", "NoUpgrade", "TlsVerifyFailure", "KilledPending")
LOGS = "/w/Unreal/VaRestHost/Saved/Logs"
VERDICTS = ("differs", "crashed", "agrees", "recorded", "unverified")
LINE = re.compile(r"LogVaRestLwsProbe: Display: (FACT|PENDING) (\{.*\})\s*$")
SECTIONS = (
    ("env", "Environment"),
    ("release", "Not run here: release-pass items"),
    ("svc", "Service thread and commands"),
    ("timeout", "Timeouts"),
    ("hs", "Handshake"),
    ("close", "Close"),
    ("ping", "Ping and pong"),
    ("rx", "Receive"),
    ("tx", "Send"),
    ("tls", "TLS"),
    ("coexist", "Coexistence with the engine's WebSockets module"),
)
COMMANDS_INSIDE = (
    "One service thread per context, waiting up to 10 s in `lws_service` and woken by `lws_cancel_service`; game-thread "
    "commands (connect, write, close, kill, flow control) run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED` "
    "(`svc.commands_*` compares that with the engine's between-calls placement).")
# Task 4 Step 7 / Task 5 Step 3 switch the rig to the engine's placement when the inside one does not work
COMMANDS_BETWEEN = (
    "One service thread per context, waiting up to 10 s in `lws_service` and woken by `lws_cancel_service`; game-thread "
    "commands (connect, write, close, kill, flow control) are drained between `lws_service` calls, the engine's way: "
    "inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED` they did not work in this run (`svc.commands_inside_wait_cancelled` "
    "differs), so the rig's default was switched.")
# None: the command placement, chosen per run by method()
METHOD = (
    "One `lws_context` per test with WebSocket spec 2.1's shape: one protocol `varest` (`rx_buffer_size` 64 KiB), "
    "`CONTEXT_PORT_NO_LISTEN`, `PEER_CERT_NOT_REQUIRED | DISABLE_OS_CA_CERTS | JUST_USE_RAW_ORIGIN`, 32 KiB header "
    "buffers, `ws_ping_pong_interval = 0`.",
    None,
    "Facts about LWS's own timers (handshake, close-ack and idle timeouts, the blackholes) wait at most 100 ms instead: "
    "LWS checks its deadlines only when `lws_service` returns. Each kill latency is measured with both waits "
    "(`*_short_wait` rows).",
    "Library outcomes (a connection that opened, ended or echoed) are facts, never failed tests; a test fails only when "
    "the harness did not run. Calls that may end the process (`Probe.VaRest.Risky.*`) run one case per process after "
    "a `PENDING` line.",
    "Every callback is recorded in order with its length, flags and a copy of its bytes; the server side comes from "
    "`ws.py`'s journal (`GET /ws/journal`) over HTTP, never through the library under test.",
    "TLS against the test server's committed throwaway CA (`Tools/testserver/certs/`).",
    "The probe never calls `lws_set_log_level` or `lws_set_allocator`.",
)


def read_facts(paths):
    facts, pending = {}, {}
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as stream:
            for line in stream:
                match = LINE.search(line)
                if not match:
                    continue
                record = json.loads(match.group(2))
                key = record.get("id", "")
                if not key or key.startswith("rig."):
                    continue
                (facts if match.group(1) == "FACT" else pending)[key] = record
    for key, record in pending.items():
        if key not in facts:
            facts[key] = {"id": key, "verdict": "crashed", "test": record.get("test", ""), "spec": "", "evidence": "",
                          "observed": "the process ended inside the probed call (a PENDING line and no FACT)"}
    return facts


def cell(value):
    return str(value).replace("\\", "\\\\").replace("|", "\\|").replace("\r", " ").replace("\n", " ")


def engine_version(engine):
    try:
        with open(os.path.join(engine, "Engine", "Build", "Build.version"), encoding="utf-8") as stream:
            build = json.load(stream)
        return "%d.%d.%d (%s)" % (build["MajorVersion"], build["MinorVersion"], build["PatchVersion"], build.get("BranchName", ""))
    except (OSError, ValueError, KeyError):
        return "unknown"


def cpu_name():
    try:
        with open("/proc/cpuinfo", encoding="utf-8") as stream:
            for line in stream:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def plugin_commit():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    try:
        result = subprocess.run(["git", "-C", root, "rev-parse", "--short", "HEAD"], capture_output=True, text=True, timeout=10)
        return result.stdout.strip() or "unknown"
    except (OSError, subprocess.SubprocessError):
        return "unknown"


def method(facts):
    """METHOD with the command placement this run used (re-review N-2)"""
    between = facts.get("svc.commands_inside_wait_cancelled", {}).get("verdict") == "differs"
    return [(COMMANDS_BETWEEN if between else COMMANDS_INSIDE) if text is None else text for text in METHOD]


def render(facts, logs, machine, today, missing):
    def observed(key):
        return facts.get(key, {}).get("observed", "not recorded")

    lines = ["# libwebsockets 3.0 probe (%s)" % today, "",
             "WebSocket spec §16.4, milestone W0: libwebsockets as the engine bundles it, driven directly by "
             "`Probe.VaRest.Lws.*` and `Probe.VaRest.Risky.Lws.*` (`Source/VaRestTests/Private/LwsProbe/`) against "
             "`Tools/testserver/ws.py`. One row per fact. **spec** is what the WebSocket spec says, or what it inferred "
             "from libwebsockets 4.3; **verdict** compares it with what 3.0 did: agrees, differs, recorded (a measurement "
             "the spec makes no claim about), unverified (could not run on this host), crashed (the process ended inside "
             "the probed call). Generated by `Tools/lws_probe_passport.py`: re-run it instead of editing this file.",
             "", "## Machine and build", "", "| | |", "|---|---|"]
    for name, value in machine:
        lines.append("| %s | %s |" % (name, cell(value)))
    lines.append("| libwebsockets | %s |" % cell(observed("env.lws_version")))
    lines.append("| OpenSSL (as VaRestTests' calls resolve) | %s |" % cell(observed("env.openssl_version")))
    lines.append("| Build | %s |" % cell(observed("env.build")))
    lines.append("| Logs | %s |" % cell(", ".join(os.path.basename(path) for path in logs)))
    lines += ["", "## How the probe works", ""] + ["- " + text for text in method(facts)]

    counts = {verdict: 0 for verdict in VERDICTS}
    for record in facts.values():
        counts[record.get("verdict", "recorded")] = counts.get(record.get("verdict", "recorded"), 0) + 1
    lines += ["", "## Summary", "", "| verdict | facts |", "|---|---|"]
    lines += ["| %s | %d |" % (verdict, counts.get(verdict, 0)) for verdict in VERDICTS]
    lines.append("| expected ids missing | %d |" % len(missing))
    if missing:
        lines += ["", "## Missing facts", ""] + ["- `%s`" % key for key in missing]

    for title, verdict in (("Differs from the spec", "differs"), ("Crashed", "crashed")):
        chosen = sorted(key for key, record in facts.items() if record.get("verdict") == verdict)
        lines += ["", "## " + title, ""]
        lines += ["- `%s`: %s (spec: %s)" % (key, cell(facts[key].get("observed", "")), cell(facts[key].get("spec", ""))) for key in chosen] or ["none"]

    lines += ["", "## Results"]
    for prefix, title in SECTIONS:
        keys = [key for key in EXPECTED if key.split(".", 1)[0] == prefix and key in facts]
        keys += sorted(key for key in facts if key.split(".", 1)[0] == prefix and key not in EXPECTED)
        lines += ["", "### " + title, "", "| id | verdict | observed | spec | evidence |", "|---|---|---|---|---|"]
        for key in keys:
            record = facts[key]
            lines.append("| `%s` | %s | %s | %s | %s |" % (key, record.get("verdict", ""), cell(record.get("observed", "")),
                                                           cell(record.get("spec", "")), cell(record.get("evidence", ""))))
    lines += ["", "## Re-run", "", "```",
              "Tools/run_tests.sh Probe.VaRest.Lws; cp %s/VaRestTests.log %s/VaRestProbe-main.log" % (LOGS, LOGS),
              "for CASE in %s; do" % " ".join(RISKY_CASES),
              "\tTools/run_tests.sh --no-build \"Probe.VaRest.Risky.Lws.StatusInConnectionError.$CASE\"",
              "\tcp %s/VaRestTests.log \"%s/VaRestProbe-risky-$CASE.log\"" % (LOGS, LOGS),
              "done",
              "python3 -I Tools/lws_probe_passport.py --log %s/VaRestProbe-main.log %s/VaRestProbe-risky-*.log \\" % (LOGS, LOGS),
              "\t--out docs/passport/$(date +%F)-lws30-probe.md",
              "```", ""]
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description="libwebsockets 3.0 probe passport")
    parser.add_argument("--log", action="extend", nargs="+", default=[], help="run logs with FACT lines (one or more; repeatable)")
    parser.add_argument("--out", help="the passport to write")
    parser.add_argument("--engine", default=os.environ.get("VAREST_ENGINE", "/w/Unreal/UnrealEngine"))
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args(argv)
    if args.selftest:
        return 0 if unittest.main(module=__name__, argv=[sys.argv[0]], exit=False).result.wasSuccessful() else 1
    if not args.log or not args.out:
        parser.error("--log and --out are required")
    facts = read_facts(args.log)
    missing = [key for key in EXPECTED if key not in facts]
    machine = [("Date", datetime.date.today().isoformat()), ("OS", platform.platform()), ("CPU", cpu_name()),
               ("Engine", engine_version(args.engine)), ("Plugin commit", plugin_commit())]
    text = render(facts, args.log, machine, datetime.date.today().isoformat(), missing)
    temporary = args.out + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        stream.write(text)
    os.replace(temporary, args.out)
    unknown = sorted(set(facts) - set(EXPECTED))
    if unknown:
        print("facts not in EXPECTED (kept in the passport): " + ", ".join(unknown))
    if missing:
        print("missing facts: " + ", ".join(missing), file=sys.stderr)
        return 1
    print("passport: %d facts -> %s" % (len(facts), args.out))
    return 0


class SelfTest(unittest.TestCase):
    def write_log(self, directory, lines):
        path = os.path.join(directory, "run.log")
        with open(path, "w", encoding="utf-8") as stream:
            stream.write("\n".join(lines) + "\n")
        return path

    def fact(self, key, verdict="agrees", observed="ok"):
        return "[2026.10.21-12.00.00:000][  0]LogVaRestLwsProbe: Display: FACT " + json.dumps(
            {"id": key, "verdict": verdict, "observed": observed, "spec": "s", "evidence": "e", "test": "t"})

    def test_facts_pending_and_rig_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [
                self.fact("hs.raw_origin", "differs", "Origin: http://x | y"),
                "[...]LogVaRestLwsProbe: Display: PENDING " + json.dumps({"id": "hs.status_in_connection_error.no_upgrade"}),
                self.fact("rig.fact_line"),
                "LogSomethingElse: Display: FACT {not ours}",
            ])
            facts = read_facts([log])
        self.assertEqual(sorted(facts), ["hs.raw_origin", "hs.status_in_connection_error.no_upgrade"])
        self.assertEqual(facts["hs.status_in_connection_error.no_upgrade"]["verdict"], "crashed")
        text = render(facts, [log], [("Date", "2026-10-21")], "2026-10-21", ["env.lws_version"])
        self.assertIn("| `hs.raw_origin` | differs | Origin: http://x \\| y |", text)
        self.assertIn("## Missing facts", text)
        self.assertIn("- `hs.status_in_connection_error.no_upgrade`: the process ended inside the probed call", text)

    def test_a_pending_line_with_its_fact_is_not_a_crash(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [
                "[...]LogVaRestLwsProbe: Display: PENDING " + json.dumps({"id": "hs.status_in_connection_error.refused"}),
                self.fact("hs.status_in_connection_error.refused", "differs", "401 -> 0"),
            ])
            facts = read_facts([log])
        self.assertEqual(facts["hs.status_in_connection_error.refused"]["verdict"], "differs")

    def test_missing_ids_fail_the_run(self):
        with tempfile.TemporaryDirectory() as directory:
            log = self.write_log(directory, [self.fact("env.lws_version")])
            out = os.path.join(directory, "passport.md")
            self.assertEqual(main(["--log", log, "--out", out]), 1)
            with open(out, encoding="utf-8") as stream:
                self.assertIn("`svc.cancel_wake_ms`", stream.read())
            # One --log with several files (the per-case risky logs): every id from either file counts
            first = os.path.join(directory, "first.log")
            second = os.path.join(directory, "second.log")
            half = len(EXPECTED) // 2
            with open(first, "w", encoding="utf-8") as stream:
                stream.write("\n".join(self.fact(key) for key in EXPECTED[:half]) + "\n")
            with open(second, "w", encoding="utf-8") as stream:
                stream.write("\n".join(self.fact(key) for key in EXPECTED[half:]) + "\n")
            self.assertEqual(main(["--log", first, second, "--out", out]), 0)

    def test_expected_ids_are_unique_and_sectioned(self):
        self.assertEqual(len(EXPECTED), len(set(EXPECTED)))
        prefixes = {prefix for prefix, _ in SECTIONS}
        self.assertEqual(sorted({key.split(".", 1)[0] for key in EXPECTED} - prefixes), [])

    def test_method_follows_the_command_placement(self):
        inside = render({}, [], [], "2026-10-21", [])
        self.assertIn("run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED`", inside)
        flipped = {"svc.commands_inside_wait_cancelled": {"verdict": "differs", "observed": "established=no"}}
        between = render(flipped, [], [], "2026-10-21", [])
        self.assertIn("drained between `lws_service` calls", between)
        self.assertNotIn("run inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED`", between)

    def test_every_risky_case_has_its_id(self):
        ids = ["hs.status_in_connection_error." + re.sub(r"(?<!^)(?=[A-Z])", "_", case).lower() for case in RISKY_CASES]
        self.assertEqual([key for key in ids if key not in EXPECTED], [])
        self.assertEqual(sorted(key for key in EXPECTED if key.startswith("hs.status_in_connection_error.")), sorted(ids))
        text = render({}, [], [], "2026-10-21", [])
        self.assertIn("Probe.VaRest.Risky.Lws.StatusInConnectionError.$CASE", text)


if __name__ == "__main__":
    sys.dont_write_bytecode = True
    sys.exit(main())
```

- [ ] **Step 3: Run the selftest to see it pass**

Run: `python3 -I Tools/lws_probe_passport.py --selftest`
Expected: `Ran 6 tests ... OK`, exit 0.

- [ ] **Step 4: Run the whole probe**

No editor may be running. Run:

```bash
Tools/run_tests.sh Probe.VaRest.Lws; cp /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-main.log
for CASE in Refused ConnectRefused DnsFailure NoUpgrade TlsVerifyFailure KilledPending; do
	Tools/run_tests.sh --no-build "Probe.VaRest.Risky.Lws.StatusInConnectionError.$CASE"
	cp /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log "/w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-risky-$CASE.log"
done
```

Expected: the first prints `run_tests: 34 passed, 0 failed`. That count is the four rig tests and 30 probe tests: `Service` 3, `Timeout` 4, `Handshake` 4, `Close` 5, `Ping` 3, `Data` 7, `Tls` 3, `Coexist` 1. It takes several minutes. A failed test is a harness failure and nothing else (decision 3, review M-2 ruling): the test server, a journal, a rig or a precondition connection did not work, and every library outcome is a fact and an `outcome:` info line instead. Fix the harness and repeat the run. Each risky case prints `1 passed` or `run did not complete (crash?)`, and both are results (Task 6 Step 4).

- [ ] **Step 5: Generate the passport**

Run:

```bash
python3 -I Tools/lws_probe_passport.py --log /w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-main.log \
	/w/Unreal/VaRestHost/Saved/Logs/VaRestProbe-risky-*.log --out docs/passport/$(date +%F)-lws30-probe.md
```

Expected: `passport: 135 facts -> docs/passport/<date>-lws30-probe.md`, exit 0 (a risky case that crashed counts as its `crashed` fact). On exit 1 the missing ids are printed: find which test did not log them (it failed before its `Fact` calls), fix the harness, and re-run Step 4.

Read the generated file end to end. Check that its "Summary" counts add up to 135 and that every "Differs" entry has an `observed` text that a reader can act on without the log. Note in the task report every row whose verdict surprised you.

- [ ] **Step 6: Commit**

```bash
git add Tools/lws_probe_passport.py docs/passport/*-lws30-probe.md
git commit -m "passport: libwebsockets 3.0 probe (WS W0) -- every spec 16.4 item recorded as a fact, generated from the probe run

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 11: Spec corrections (§0.3, §2, and per ruling Q3 §1.2, D30, §3.4's 1002 row), CLAUDE.md, beads, final verification

W0's exit row: "probe recorded; §0.3/§2 corrected where needed". This task edits `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` only as the tables below say: §0.3 (Step 1), §2 (Step 2) and, per ruling Q3, §1.2, D30 in §20 and §3.4's 1002 row (Step 3, rows S1–S4). Each row is applied only when its condition holds on an unambiguous fact. A fact that is ambiguous or `unverified` for a row becomes a bead (Step 6), and so does a finding that contradicts any other section, and so does every `differs` that no row consumed (Step 6's last bead, re-review N-4). Every inserted citation names the passport row, written `[<id>]`, and the passport file `docs/passport/<date>-lws30-probe.md` (the date of Task 10's run). "Observed" below means the passport's `observed` cell for that id, copied (shortened to its first clause when it runs over a line). The task also adds the probe's commands to `CLAUDE.md` (Step 5, ruling Q1).

**Files:**
- Modify: `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` (§0.3, §1.2, §2, §3.4's table, the D30 row in §20; nothing else)
- Modify: `CLAUDE.md` (Build & Test block: three lines)

**Interfaces:**
- Consumes: the Task 10 passport (`## Differs from the spec`, `## Crashed`, the `observed` cells).
- Produces: the corrected spec; three `CLAUDE.md` lines; beads for ambiguous, unverified and out-of-scope findings.

- [ ] **Step 1: Replace §0.3's "inferred" paragraph with the pinned facts**

Find this paragraph (spec §0.3, after the T16 row):

```
Inferred from LWS 4.3 source (not available for 3.0) and pinned by the W0 probe (§16.4): whether a
server-selected subprotocol unknown to the context is accepted when `local_protocol_name` pre-binds
the connection; whether `lws_http_client_http_response` is valid inside `CONNECTION_ERROR`; which
received close codes LWS 3.0 rewrites; whether LWS waits for the close echo; LWS's default
`timeout_secs`; whether LWS answers server PINGs (RFC 6455 §5.5.2 requires it).
```

Replace it with the paragraph and table below. Each `Fact` cell is the text for the verdict the passport shows; drop the alternatives. Keep the row even when the verdict agrees, because the point is that it is now measured.

```
Pinned by the W0 probe on Linux (`docs/passport/<date>-lws30-probe.md`; passport rows in brackets), where the
list above was inferred from LWS 4.3. Win64 and Mac re-run the probe in the release pass; Android and iOS cannot
run it (W1's transport suite on devices covers them). Timeouts were measured with `lws_service` waiting at most
100 ms, so they are LWS's own and not the wait's.

| # | Fact (LWS 3.0, measured) | Passport |
| --- | --- | --- |
```

Then one row per line, in this order. `CEIC` below abbreviates the six ids `hs.status_in_connection_error.{refused,connect_refused,dns_failure,no_upgrade,tls_verify_failure,killed_pending}`.

| Row | Verdict | Fact cell |
| --- | --- | --- |
| T17 | `hs.unregistered_selected_bound` agrees | A server-selected subprotocol the context does not register is accepted when `local_protocol_name` pre-binds the connection, which stays bound to `varest`; `FILTER_PRE_ESTABLISH` reads the selected name: <`hs.selected_protocol_in_filter` observed> |
| T17 | ... differs, observed starts with "refused" | A server-selected subprotocol the context does not register is refused even when `local_protocol_name` pre-binds the connection: <observed> |
| T17 | ... differs, observed starts with "established" | A server-selected subprotocol the context does not register is accepted, but the connection is bound to another protocol than `local_protocol_name` names: <observed> |
| T18 | all six CEIC agree | `lws_http_client_http_response(wsi)` is safe inside `CONNECTION_ERROR` in every case probed (refused upgrade, connect refused, DNS failure, no upgrade response, TLS verify failure, a kill while pending): the status after a status line, 0 otherwise. On a connection without a header table it dereferences NULL (`client.c.o` `lws_http_client_http_response` 0x1294-0x12b2), so only these cases are known safe |
| T18 | `.refused` agrees, another CEIC case crashed, differs or is unverified | Inside `CONNECTION_ERROR`, `lws_http_client_http_response(wsi)` returns the status only when the status-line parser failed (<`.refused` observed>); <for each other case: "<case>: crashed" \| "<case>: returned N" \| "<case>: not reached">. Without a header table it dereferences NULL (`client.c.o` `lws_http_client_http_response` 0x1294-0x12b2), so it is never called elsewhere |
| T18 | `.refused` differs, crashed or is unverified | `lws_http_client_http_response(wsi)` does not give the status inside `CONNECTION_ERROR` (<`.refused` observed>): the error strings are the only source |
| T19 | `close.rewritten_codes` (recorded) | Received close codes: <observed>. Returning 0 from `WS_PEER_INITIATED_CLOSE` echoes the close: <`close.echo_on_return_0` observed> |
| T20 | `close.ours_noecho_wait_ms` (recorded) | After our close, against a server that never echoes: <observed> |
| T21 | `timeout.default_secs` (recorded) | Default `timeout_secs`, an unanswered upgrade: <observed>; a connect to a blackhole address: <`timeout.default_secs_blackhole` observed>. A set `timeout_secs` bounds the handshake: <`timeout.configured_secs` observed>, and a TCP connect: <`timeout.tcp_connect_blackhole` observed> |
| T22 | `ping.auto_pong` agrees | LWS answers server PINGs with a PONG of the same payload, and raises no callback for them (<`ping.client_sees_server_ping` observed>) |
| T22 | ... differs | LWS does not answer server PINGs: <observed> |
| T23 | `tls.ssl_module_ctx` (any) | In modular builds the SSL module gives no `SSL_CTX` (`InitializeSsl()` is false, `Runtime/Online/SSL/Private/SslManager.cpp:26-55`, `:76-131`); LWS makes its own (`LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT`) and raises `LOAD_EXTRA_CLIENT_VERIFY_CERTS`: <`tls.load_extra_fired` observed>. A provided `SSL_CTX` (the monolithic path's mechanics): <`tls.provided_ctx_connects` observed>; `LOAD_EXTRA_CLIENT_VERIFY_CERTS` then: <`tls.provided_ctx_load_extra_fired` observed> |
| T24 | `svc.commands_inside_wait_cancelled` (any) | Connect, write and close from inside `EVENT_WAIT_CANCELLED`: <observed>; `lws_cancel_service` ends a 10 s wait in <`svc.cancel_wake_ms` observed> |
| T25 | `hs.redirect_relative` (recorded) | A 3xx upgrade answer: <observed>; to wss: <`hs.redirect_to_tls` observed>; on the redirected request: <`hs.redirect_reappends_headers` observed>, the callback sees <`hs.redirect_target_visible` observed>, a non-zero return gives <`hs.redirect_veto` observed>, and `Host` is <`hs.redirect_host_header` observed>; a redirect from wss to ws: <`hs.redirect_downgrade` observed> |
| T26 | `svc.kill_async_ms` (recorded) | `LWS_TO_KILL_ASYNC` on an open connection: <observed> (with a 100 ms wait: <`svc.kill_async_ms_short_wait` observed>); on a pending attempt: <`timeout.kill_pending_ms` observed> (with a 100 ms wait: <`timeout.kill_pending_ms_short_wait` observed>; <`timeout.kill_pending_sequence` observed>) |
| T27 | `rx.final_flag` (any) | Receive: <observed>; one frame larger than `rx_buffer_size`: <`rx.chunk_sizes_single_frame` observed> |
| T28 | `rx.flow_pause_stops_callbacks` (any) | Flow control: <observed>; resume from a command: <`rx.flow_resume_form` observed> |
| T29 | `hs.callback_user_by_reason` (recorded) | Which callbacks carry the connection's userdata in `user` (and in `lws_wsi_user(wsi)`): <observed> |

The Passport column holds the bracketed ids each row used, e.g. `[hs.unregistered_selected_bound, hs.selected_protocol_in_filter]`; for T18, the CEIC ids it names.

Also in the T table above: for every row whose claim a passport row contradicts (verdict `differs`, and the `spec` cell names that T row), append to that row's Fact cell ` — W0: <observed> [<id>]`. The T rows a fact can contradict are T1 (`env.lws_version`), T4 (`hs.raw_origin`), T5 (`hs.append_header_space`), T6 (`hs.selected_protocol_in_filter`), T7 (`hs.refusal_*`), T9 (`ping.pong_payload_echo`), T10 (`tx.no_fin_frames`), T11 (`rx.flow_pause_stops_callbacks`), T12 (`close.echo_on_return_0`), T13 (`svc.cancel_wake_ms`) and T14 (`svc.timer_latency_ms`).

- [ ] **Step 2: Correct §2 where the passport contradicts it**

Apply each row whose condition holds; leave the passage as it is otherwise. The "Find" texts are exact (line breaks as in the spec).

| # | Condition | Find (in §2) | Replace with |
| --- | --- | --- | --- |
| E1 | always (from the engine source; `tls.ssl_module_ctx` confirms it in the editor) | `` `PEER_CERT_NOT_REQUIRED \| DISABLE_OS_CA_CERTS`, `SSL_CTX` from`` ⏎ `` `ISslManager::CreateSslContext` (`Runtime/Online/SSL/Public/Interfaces/ISslManager.h:64-81`), `` | `` `PEER_CERT_NOT_REQUIRED \| DISABLE_OS_CA_CERTS`; the `SSL_CTX` from `ISslManager::CreateSslContext` (`Runtime/Online/SSL/Public/Interfaces/ISslManager.h:64-81`) where `InitializeSsl` succeeds, which is in monolithic builds only (`Runtime/Online/SSL/Private/SslManager.cpp:26-55`, `:76-131`); elsewhere, every editor included, LWS makes its own with `LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT` and `LOAD_EXTRA_CLIENT_VERIFY_CERTS` adds the engine's roots, as the engine does (`LwsWebSocketsManager.cpp:205-225`, `:441-446`) [tls.ssl_module_ctx, tls.load_extra_fired], `` |
| E1b | `tls.load_extra_fired` differs | the text E1 inserted, from `and LOAD_EXTRA_CLIENT_VERIFY_CERTS adds` to `[tls.ssl_module_ctx, tls.load_extra_fired]` | `` but LWS 3.0 raises no `LOAD_EXTRA_CLIENT_VERIFY_CERTS` for it [tls.load_extra_fired]: VaRest then makes the context itself (`SSL_CTX_new(TLS_client_method())`, `AddCertificatesToSslContext`) and provides it, which works [tls.provided_ctx_connects] `` (if `tls.provided_ctx_connects` shows it does not work, file the P0 bead of Step 6 instead of this edit) |
| E2 | `svc.commands_inside_wait_cancelled` differs and `svc.commands_between_service_calls` shows a working round trip | `` the service thread drains them in`` ⏎ `` `LWS_CALLBACK_EVENT_WAIT_CANCELLED` (T13). `` | `` the service thread drains them between `lws_service` calls, as the engine does (`LwsWebSocketsManager.cpp:485-509`): from inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED` they do not work on LWS 3.0 [svc.commands_inside_wait_cancelled]; `lws_cancel_service` ends the wait (T13). `` |
| E3 | `svc.kill_async_ms` observed over 1500 ms and `svc.kill_async_ms_short_wait` observed under 1500 ms (the delay is the wait, decision 9) | `` and checks per-connection deadlines after each return. `` | `` and checks per-connection deadlines after each return. LWS's own timers (a `LWS_TO_KILL_ASYNC`, the close-ack and handshake timeouts) run only when `lws_service` returns, which a long wait postpones (a kill took <`svc.kill_async_ms` observed> with a 10 s wait, <`svc.kill_async_ms_short_wait` observed> with 100 ms [svc.kill_async_ms, svc.kill_async_ms_short_wait]), so the wait is capped at 1 s. `` |
| E4 | `hs.unregistered_selected_bound` agrees | `` The selected protocol is read in `FILTER_PRE_ESTABLISH` (T6) into `FVaRestSocketOpen::Protocol`. `` ⏎ `` If W0 shows LWS 3.0 still demands a registered name, the context registers every subprotocol named `` ⏎ `` in settings and assets at creation, and an unknown code-only name gets a clear `Config` error. `` | `` The selected protocol is read in `FILTER_PRE_ESTABLISH` (T6) into `FVaRestSocketOpen::Protocol`. LWS 3.0 accepts a selected name the context does not register [hs.unregistered_selected_bound, hs.selected_protocol_in_filter]. `` |
| E4b | `hs.unregistered_selected_bound` differs and its observed starts with "refused" | the same Find text | `` The selected protocol is read in `FILTER_PRE_ESTABLISH` (T6) into `FVaRestSocketOpen::Protocol`. LWS 3.0 still demands a registered name (<observed> [hs.unregistered_selected_bound]), so the context registers every subprotocol named in settings and assets at creation, and an unknown code-only name gets a clear `Config` error. `` (observed starting with "established": the connection opened bound to another protocol; no edit, a bead in Step 6) |
| E4c | `hs.selected_protocol_in_filter` differs | `` The selected protocol is read in `FILTER_PRE_ESTABLISH` (T6) into `FVaRestSocketOpen::Protocol`. `` (as E4/E4b left it) | `` LWS 3.0 does not give the selected protocol in `FILTER_PRE_ESTABLISH` (<observed> [hs.selected_protocol_in_filter]): `FVaRestSocketOpen::Protocol` is the protocol the connection is bound to, `lws_get_protocol(wsi)->name`. `` |
| E5 | `hs.raw_origin` differs | `` non-default port), sent raw (T4); `` | `` non-default port); LWS 3.0 does not send it raw even with `JUST_USE_RAW_ORIGIN` (<observed> [hs.raw_origin]), so W1 appends `Origin` itself in `APPEND_HANDSHAKE_HEADER` and passes no `.origin`; `` |
| E6 | `hs.append_header_user` shows the user argument is not the connection's userdata | `` (T5); not enough space → the attempt fails with `Config`, never an overrun (E19). `` | `` (T5), the connection found through `lws_wsi_user(wsi)`: the callback's `user` is not it there (<observed> [hs.append_header_user, hs.callback_user_by_reason]); not enough space → the attempt fails with `Config`, never an overrun (E19). `` |
| E7 | `hs.append_header_fill` differs | `` with the remaining length checked per header `` (the end of the **Headers** bullet's first line) | `` with the remaining length checked per header, kept 1 KiB below what LWS 3.0 reports, which did not all fit (<observed> [hs.append_header_fill]), `` |
| E8 | `tls.insecure_ip_host` refused and `tls.insecure_skip_hostname` established | `` and outside Shipping `wss+insecure` `` ⏎ `` (self-signed allowed, as `LwsWebSocket.cpp:733-769`). `` | `` and outside Shipping `wss+insecure` (self-signed allowed and the host name not checked, `LCCSCF_ALLOW_SELFSIGNED \| LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK`: with `ALLOW_SELFSIGNED` alone, as the engine sets it at `LwsWebSocket.cpp:747-750`, an IP URL fails the host-name check [tls.insecure_ip_host, tls.insecure_skip_hostname]). `` |
| E9 | `timeout.kill_pending_ms` over 1500 ms, or `timeout.kill_pending_sequence` without `WSI_DESTROY` | `` `lws_set_timeout(…, LWS_TO_KILL_ASYNC)` (T14), so a hitching game thread cannot cause it. `` | `` `lws_set_timeout(…, LWS_TO_KILL_ASYNC)` (T14), so a hitching game thread cannot cause it; a pending attempt ends <`timeout.kill_pending_ms` observed> after the kill with a 10 s wait, <`timeout.kill_pending_ms_short_wait` observed> with 100 ms (<`timeout.kill_pending_sequence` observed>) [timeout.kill_pending_ms, timeout.kill_pending_ms_short_wait]. `` |
| E9b | `timeout.configured_secs` agrees, and `ping.idle_survives_timeout_secs` agrees (E25 not applied: a `timeout_secs` that also ends idle open connections must not be lowered to `ConnectTimeout`) | the **Connect timeout** bullet's first sentence (as E9 left it, or the original) | insert after it: `` The context's `timeout_secs` is set to the default `ConnectTimeout` as a backstop [timeout.configured_secs]: it is the vhost's, so context-wide for VaRest's one context ("various processes involving network roundtrips", `libwebsockets.h:2791-2795`), and it backs only the default; a socket with its own `ConnectTimeout` relies on the kill. `` |
| E10 | all six CEIC agree | `` and from `lws_http_client_http_response(wsi)` if W0 shows `` ⏎ `` it is valid there. `` | `` and from `lws_http_client_http_response(wsi)`, which is valid there in every case W0 probed: refused upgrade, connect refused, DNS failure, no upgrade response, TLS verify failure, a kill while pending [hs.status_in_connection_error.refused, …connect_refused, …dns_failure, …no_upgrade, …tls_verify_failure, …killed_pending]; it dereferences NULL on a connection without a header table (`client.c.o` 0x1294-0x12b2), so W1 calls it only in these cases. `` |
| E10c | `.refused` agrees, and another CEIC case crashed, differs or is unverified | the same Find text | `` and from `lws_http_client_http_response(wsi)` only when the error comes from the status-line parser (the strings `HS: ws upgrade …`) [hs.status_in_connection_error.refused]; never elsewhere: without a header table it dereferences NULL (`client.c.o` `lws_http_client_http_response` 0x1294-0x12b2) [<the CEIC ids that crashed, differ or are unverified>]. `` |
| E10b | `.refused` differs, crashed or is unverified | the same Find text | `` and not from `lws_http_client_http_response(wsi)`, which is <not valid there (differs) \| fatal there (crashed) \| not reached in W0 (unverified)> [hs.status_in_connection_error.refused]: the strings are the only source. `` |
| E11 | any `hs.refusal_*` differs | `` (T7: "not 101, was N"; `` ⏎ `` "unauthorized" = 401; "forbidden" = 403) `` | `` (T7, as LWS 3.0 sends them [hs.refusal_401, hs.refusal_403, hs.refusal_404]: 401 <observed>, 403 <observed>, other codes <hs.refusal_404 observed with 404 replaced by N>) `` |
| E12 | always | `` reports it as its own failure reason, `NoUpgradeResponse`; W0 pins the LWS 3.0 error string. `` | `` reports it as its own failure reason, `NoUpgradeResponse`; LWS 3.0 reports it as <`hs.no_upgrade_response` observed> [hs.no_upgrade_response]. `` |
| E13 | any of `hs.redirect_relative`, `hs.redirect_absolute`, `hs.redirect_to_tls` observed starts with "followed", and `hs.redirect_veto` agrees | (insert a new bullet right after the **No upgrade response** bullet) | `` * **Redirects** are refused (ruling Q2): VaRest never follows a 3xx to the upgrade, as browsers do not (the WHATWG WebSockets standard fetches with redirect mode "error"; RFC 6455 §4.1 only permits following). LWS 3.0 follows one itself [hs.redirect_relative, hs.redirect_absolute, hs.redirect_to_tls] and runs `APPEND_HANDSHAKE_HEADER` again for the new request (<`hs.redirect_reappends_headers` observed>), where the callback sees <`hs.redirect_target_visible` observed> [hs.redirect_target_visible] and the request carries <`hs.redirect_host_header` observed> [hs.redirect_host_header]. So the appended-header callback adds nothing (no auth, no custom headers) once the connection's target differs from the original scheme+host+port, which the second `APPEND_HANDSHAKE_HEADER` of an attempt marks, and the socket fails the connection at the first callback that can (`APPEND_HANDSHAKE_HEADER`, else `FILTER_PRE_ESTABLISH`: a non-zero return gives <`hs.redirect_veto` observed> [hs.redirect_veto]) with a redirect error; the W1 plan names the error kind. A redirect from wss to ws is <`hs.redirect_downgrade` observed> [hs.redirect_downgrade]. `` |
| E13c | E13's "followed" condition holds and `hs.redirect_veto` differs (a non-zero return did not fail it) | (insert the same place) | E13's bullet with its sentence from `and the socket fails the connection` replaced by: `` and the socket fails the connection: a non-zero return from those callbacks does not (<`hs.redirect_veto` observed> [hs.redirect_veto]), so it kills the attempt with `lws_set_timeout(…, LWS_TO_KILL_ASYNC)` (T14) from the second `APPEND_HANDSHAKE_HEADER`, reported as a redirect error; the W1 plan names the error kind. `` |
| E13d | E13's "followed" condition holds and `hs.redirect_veto` is `recorded` (a veto run was not exercised: the relative redirect was not followed, or the redirected request failed before `FILTER_PRE_ESTABLISH`) | (insert the same place) | E13's bullet with its sentence from `and the socket fails the connection` replaced by: `` and the socket fails the connection by killing the attempt with `lws_set_timeout(…, LWS_TO_KILL_ASYNC)` (T14) from the second `APPEND_HANDSHAKE_HEADER`, reported as a redirect error (the veto points were not exercised in W0: <`hs.redirect_veto` observed> [hs.redirect_veto]); the W1 plan names the error kind. `` |
| E13b | none of the three starts with "followed" | (insert the same place) | `` * **Redirects** are refused (ruling Q2: VaRest never follows a 3xx to the upgrade, as browsers do not). LWS 3.0 does not follow one either (<`hs.redirect_relative` observed> [hs.redirect_relative, hs.redirect_absolute, hs.redirect_to_tls]); it is an upgrade refusal, §3.4's "upgrade 3xx" row. `` |
| E14 | `rx.final_flag` or `rx.remaining_payload` differs | `` complete when `lws_is_final_fragment` and `` ⏎ `` `lws_remaining_packet_payload == 0` (T8). `` | `` complete as LWS 3.0 marks it (<`rx.final_flag` observed>; <`rx.remaining_payload` observed> [rx.final_flag, rx.remaining_payload]): W1's rule is the observed one, pinned by the transport suite's fragment tests. `` |
| E15 | `rx.flow_resume_form` shows the explicit form resumed reading and the bool form did not | `` Reading resumes below half. `` | `` Reading resumes below half, with `lws_rx_flow_control(wsi, LWS_RXFLOW_REASON_APPLIES_ENABLE \| LWS_RXFLOW_REASON_USER_BOOL \| LWS_RXFLOW_REASON_FLAG_PROCESS_NOW)`: the resume runs outside the connection's own callback (`libwebsockets.h:5192-5194`), where the bool form does not resume it on LWS 3.0 [rx.flow_resume_form]. `` (neither form resumed, `rx.flow_resume_from_command` differs: no edit, the P1 bead of Step 6) |
| E15b | `rx.flow_pause_stops_callbacks` differs | `` the transport stops reading`` ⏎ `` (`lws_rx_flow_control(wsi, 0)`, T11), `` | `` the transport stops reading (`lws_rx_flow_control(wsi, 0)`, T11; LWS 3.0 still delivers <observed> after it [rx.flow_pause_stops_callbacks], which the count includes), `` |
| E16 | `tx.ping_between_fragments` differs | `` one per WRITEABLE callback, so PINGs and the close frame can be interleaved between fragments `` | `` one per WRITEABLE callback; LWS 3.0 writes a PING queued between them as <`tx.ping_between_fragments` observed> [tx.ping_between_fragments], so a PING goes only between whole messages `` |
| E16b | `tx.no_fin_frames` differs | `` * Messages over 64 KiB go out as fragments (`LWS_WRITE_NO_FIN`, then `LWS_WRITE_CONTINUATION`, T10), `` | `` * Messages over 64 KiB go out as fragments (`LWS_WRITE_NO_FIN`, then `LWS_WRITE_CONTINUATION`, T10; on LWS 3.0 four such writes reach the server as <`tx.no_fin_frames` observed> [tx.no_fin_frames], which W1's transport suite pins), `` |
| E17 | `tx.autoreply_between_messages` differs | `` it is written at the next`` ⏎ `` message boundary, ahead of every queued data message, `` | `` it is written at the next message boundary, ahead of every queued data message (on LWS 3.0: <observed> [tx.autoreply_between_messages]), `` |
| E18 | always | `` Connections with rules get `SO_SNDBUF` 256 KiB`` ⏎ `` (`lws_get_socket_fd` + `setsockopt`), `` | `` Connections with rules get `SO_SNDBUF` 256 KiB (`lws_get_socket_fd` + `setsockopt`; on Linux: <`tx.so_sndbuf_ws` observed> [tx.so_sndbuf_ws, tx.so_sndbuf_wss]), `` |
| E19 | `ping.pong_payload_echo` differs | `` `CLIENT_RECEIVE_PONG` (T9) matches the payload and yields an RTT `` | `` `CLIENT_RECEIVE_PONG` (T9) does not hand the payload back on LWS 3.0 (<observed> [ping.pong_payload_echo]), so PONGs are matched by order and yield an RTT `` |
| E20 | always | `` `CloseTimeout` (5 s) bounds a server that never echoes (W0 measures`` ⏎ `` whether LWS waits for the echo). `` | `` `CloseTimeout` (5 s) bounds a server that never echoes (LWS 3.0: <`close.ours_noecho_wait_ms` observed> [close.ours_noecho_wait_ms]). `` |
| E21 | `close.empty_len` differs | `` **Peer close** (T12): length 0 → **1005**, `` | `` **Peer close** (T12): an empty close frame arrives as <observed> [close.empty_len] and reads as **1005**, `` |
| E22 | `close.rewritten_codes` observed is not "none: ..." | `` Codes a peer must not send`` ⏎ `` (<1000, 1004–1006, 1015, 1016–2999, ≥5000) are normalised to 1005. `` | `` Codes a peer must not send (<1000, 1004–1006, 1015, 1016–2999, ≥5000) are normalised to 1005. LWS 3.0 rewrites some codes before the callback (<observed> [close.rewritten_codes]), so a received 1002 may be one of those. `` |
| E23 | `close.echo_on_return_0` differs | `` The callback returns 0 so LWS`` ⏎ `` echoes the close, as RFC 6455 §5.5.1 requires `` | `` LWS 3.0 does not echo even when the callback returns 0 (<observed> [close.echo_on_return_0]), so VaRest answers with `lws_close_reason` and a non-zero return, as RFC 6455 §5.5.1 requires `` |
| E24 | always | `` * **Unclean end** (`CLIENT_CLOSED` / `WSI_DESTROY` without a close frame, or a read/write error) →`` ⏎ `` 1006, `bWasClean = false`. `` | `` * **Unclean end** (`CLIENT_CLOSED` / `WSI_DESTROY` without a close frame, or a read/write error) → 1006, `bWasClean = false`. On LWS 3.0 a dropped connection gives <`close.drop_reports_client_closed` observed> [close.drop_fin_sequence, close.drop_rst_sequence]. `` |
| E25 | `ping.idle_survives_timeout_secs` differs | §2.1: `` `JUST_USE_RAW_ORIGIN` (T4); `ws_ping_pong_interval = 0` (our ping is per connection, §5); `` | `` `JUST_USE_RAW_ORIGIN` (T4); `ws_ping_pong_interval = 0` (our ping is per connection, §5), although LWS 3.0 still ends an idle open connection by itself (with `timeout_secs` = 3: <observed> [ping.idle_survives_timeout_secs]), so the context does not lower `timeout_secs` to `ConnectTimeout` (no connect backstop) and W1's transport suite pins what ends an idle connection under LWS's default against `PingInterval` (bead); `` |

In the tables of Steps 2 and 3, `⏎` stands for the line break and two-space indent of the spec's text as it is now; a `\|` is a literal `|`. After each edit, rewrap the bullet to the spec's width (about 100 columns).

- [ ] **Step 3: Correct §1.2, D30 and §3.4's 1002 row where the passport is unambiguous (ruling Q3)**

| # | Condition | Find | Replace with |
| --- | --- | --- | --- |
| S1 | `coexist.lws_symbol_owner` observed says "has its own copy at another address" and `coexist.shared_lws_state` differs | §1.2: `` * **Linux interposition is benign.** `libUnrealEditor-WebSockets.so` exports 185 `lws_*` functions`` ⏎ `` (`nm -D --defined-only`, verified on this host), so ELF resolves both modules' calls to whichever`` ⏎ `` copy loaded first. Both copies come from the identical archive, so behaviour is identical.`` ⏎ `` Windows DLLs and Mac two-level-namespace dylibs keep separate copies. Monolithic builds have one. `` | `` * **Each module keeps its own copy, on Linux too.** `libUnrealEditor-WebSockets.so` exports 185 `lws_*` functions (`nm -D --defined-only`), but UE loads modules with `RTLD_LOCAL` (`Runtime/Core/Private/Unix/UnixPlatformProcess.cpp:107-148`), so each module's calls stay in its own copy (<`coexist.lws_symbol_owner` observed> [coexist.lws_symbol_owner, coexist.shared_lws_state]). Both copies come from the identical archive, so behaviour is identical. Windows DLLs and Mac two-level-namespace dylibs keep separate copies too. Monolithic builds have one. `` |
| S2 | S1's condition | §1.2: `` * **The only shared state is process-global** (T16): log level/emitter and allocator. VaRest never`` ⏎ `` calls `lws_set_allocator`, and calls `lws_set_log_level(LLL_ERR \| LLL_WARN, VaRestLwsLog)` only if`` ⏎ `` the engine's `WebSockets` module is not loaded; otherwise the engine's routing to `LogWebSockets` `` ⏎ `` (`LwsWebSocketsManager.cpp:174-176`) stays. Never both, so no one's logs are hijacked. `` | `` * **Process-global state is per copy** (T16): log level/emitter and allocator. VaRest never calls `lws_set_allocator`. In modular builds its copy's log state is its own [coexist.shared_lws_state], so there VaRest always calls `lws_set_log_level(LLL_ERR \| LLL_WARN, VaRestLwsLog)`, and the engine's routing to `LogWebSockets` (`LwsWebSocketsManager.cpp:174-176`) stays in the engine's copy (<`coexist.engine_lws_log_routed` observed> [coexist.engine_lws_log_routed]). In a monolithic build the copy is shared, so there VaRest calls it only if the engine's `WebSockets` module is not loaded (in the editor, at VaRestTests' startup: <`env.websockets_loaded_at_start` observed> [env.websockets_loaded_at_start]). Never both in one copy, so no one's logs are hijacked. `` |
| S2b | S1's condition | §20: `` \| D30 \| `lws_set_log_level` only when the engine `WebSockets` module is not loaded; never `lws_set_allocator` \| `` | `` \| D30 \| `lws_set_log_level` on VaRest's own copy (modular builds) always, and on the one shared copy of a monolithic build only when the engine `WebSockets` module is not loaded (§1.2, [coexist.shared_lws_state]); never `lws_set_allocator` \| `` |
| S3 | `close.rewritten_codes` observed is "none: every code arrived as sent" | §3.4: `` \| 1002, 1003, 1007, 1010 from the server \| stop — unless W0 shows LWS 3.0 rewrites codes to 1002, then 1002 reconnects and stops after 3 in a row \| `Server` \| `` | `` \| 1002, 1003, 1007, 1010 from the server \| stop (LWS 3.0 hands every received code over unchanged [close.rewritten_codes]) \| `Server` \| `` |
| S3b | `close.rewritten_codes` observed lists rewrites, at least one of them `-> 1002`, and none `-> no code` | the same Find text | two rows: `` \| 1003, 1007, 1010 from the server \| stop \| `Server` \| `` and `` \| 1002 from the server \| reconnect; stop after 3 in a row (LWS 3.0 rewrites <the codes listed with "-> 1002"> to 1002 [close.rewritten_codes]) \| `Server` \| `` |
| S3c | `close.rewritten_codes` observed lists rewrites, none `-> 1002` and none `-> no code` | the same Find text | `` \| 1002, 1003, 1007, 1010 from the server \| stop (LWS 3.0 rewrites only <observed>, none to 1002 [close.rewritten_codes]) \| `Server` \| `` |
| S4 | `coexist.context_create_reallocates_ssl_index` differs | §1.2: `` * **Contexts are independent.** All connection state lives in the `lws_context`; VaRest creates`` ⏎ `` its own and never touches the engine's. `` | `` * **Contexts are independent, except for one TLS global.** All connection state lives in the `lws_context`; VaRest creates its own and never touches the engine's. But every `lws_create_context` re-allocates its LWS copy's process-global `openssl_websocket_private_data_index`, which the client verify callback dereferences without a null check (<observed> [coexist.context_create_reallocates_ssl_index]): a context created while another context of the same copy has a TLS handshake in flight crashes that handshake. In a monolithic build VaRest's context shares the copy with `WebSockets`, `WebSocketServer`, `WebSocketNetworking` and `HttpNetworkReplayStreaming`, so W1 decides how VaRest's context creation stays out of that window. `` |

When a row's facts disagree with each other (S1/S2: one coexist row says one copy, the other two) or are `unverified`, or `close.rewritten_codes` lists a `-> no code` entry, leave that passage and file Step 6's bead for it.

- [ ] **Step 4: Check the edits stayed inside the allowed ranges**

Run:

```bash
git diff -U0 docs/superpowers/specs/2026-10-20-varest2-websocket-design.md | python3 -I -c '
import re, sys
lines = open("docs/superpowers/specs/2026-10-20-varest2-websocket-design.md", encoding="utf-8").read().split("\n")
def at(prefix):
    return next(index + 1 for index, line in enumerate(lines) if line.startswith(prefix))
# §0.3, §1.2, §2, §3.4 and the D30 row (ruling Q3), as line ranges of the edited file: [first, next heading)
allowed = [(at("### 0.3 "), at("### 0.4 ")), (at("### 1.2 "), at("### 1.3 ")), (at("## 2. "), at("## 3. ")),
           (at("### 3.4 "), at("### 3.5 ")), (at("| D30 |"), at("| D30 |") + 1)]
outside = []
for hunk in sys.stdin:
    match = re.match(r"@@ -\d+(?:,\d+)? \+(\d+)(?:,(\d+))? @@", hunk)
    if match:
        first = int(match.group(1))
        last = first + max(int(match.group(2) or 1), 1) - 1
        if not any(low <= first and last < high for low, high in allowed):
            outside.append(hunk.strip())
print("\n".join(outside) or "every hunk is inside §0.3, §1.2, §2, §3.4 or the D30 row")
sys.exit(1 if outside else 0)
'
```

Expected: `every hunk is inside §0.3, §1.2, §2, §3.4 or the D30 row`, exit 0. A hunk it prints is a mistake: undo it.

- [ ] **Step 5: Add the probe's commands to CLAUDE.md (ruling Q1)**

In `CLAUDE.md`'s Build & Test block, replace the line

```
python3 -I Tools/testserver/selftest.py   # the HTTP test server's own tests (stdlib only)
```

with

```
python3 -I Tools/testserver/selftest.py   # the HTTP test server's own tests (stdlib only)
python3 -I Tools/testserver/ws_selftest.py  # the WebSocket test server's own tests (ws.py, the TLS listener; stdlib only)
Tools/run_tests.sh Probe.VaRest.Lws         # LWS 3.0 probe (WS W0): minutes long, host-dependent, outside the default ^VaRest filter
Tools/run_tests.sh --no-build Probe.VaRest.Risky.Lws.StatusInConnectionError.Refused  # may crash: one case per run; also ConnectRefused, DnsFailure, NoUpgrade, TlsVerifyFailure, KilledPending
```

Then `grep -n 'ws_selftest\|Probe.VaRest' CLAUDE.md` prints the three new lines and nothing else changed (`git diff --stat CLAUDE.md`: `1 file changed, 3 insertions(+)`).

- [ ] **Step 6: File beads**

Always (ruling Q2: written with the probe facts whatever LWS does):

```bash
bd create "WS W1: refuse WebSocket upgrade redirects (ruling Q2), from the W0 redirect facts" -t task -p 1 \
	--deps discovered-from:VaRest-81s -d "Ruling Q2 (2026-10-21): VaRest does not follow upgrade redirects; a 3xx fails the connection (browser behaviour: the WHATWG WebSockets standard fetches with redirect mode error; RFC 6455 4.1 only permits following). Auth headers must never reach another host, and refusing is simpler to explain than a same-origin exception. Passport docs/passport/<date>-lws30-probe.md: hs.redirect_relative <observed>; hs.redirect_to_tls <observed>; hs.redirect_reappends_headers <observed>; hs.redirect_target_visible <observed>; hs.redirect_veto <observed>; hs.redirect_host_header <observed>. Spec 2.2 (corrected in W0, E13/E13b/E13c) states the rule. W1: when LWS follows, the appended-header callback adds nothing once the target differs from the original scheme+host+port and the socket fails the connection at the first callback that can, with a redirect error whose kind the W1 plan names; the transport suite covers /ws/status/302 to the same and to another port."
bd create "WS W1: monolithic coexistence and the SSL-module TLS context in a monolithic game (W0 could not run them)" -t task -p 2 \
	--deps discovered-from:VaRest-81s -d "Passport rows coexist.monolithic, tls.ssl_module_ctx: VaRestTests is UncookedOnly (ModuleDescriptor.cs:792-793); the first Runtime module linking LWS is W1's VaRestWebSocket. W1 exit: engine IWebSocket and a VaRest socket at once, ws and wss, in VaRestHost Linux Development, both logs intact."
bd update VaRest-b4d --append-notes "WS W0: on Win64 and Mac run Tools/run_tests.sh Probe.VaRest.Lws, then each Probe.VaRest.Risky.Lws.StatusInConnectionError.<Case> in its own run (Refused, ConnectRefused, DnsFailure, NoUpgrade, TlsVerifyFailure, KilledPending), then Tools/lws_probe_passport.py with all seven logs; diff the passport against docs/passport/<date>-lws30-probe.md (rows release.*)."
```

When `coexist.context_create_reallocates_ssl_index` differs or is `unverified` (review I-3):

```bash
bd create "WS W1: LWS context creation re-allocates the OpenSSL ex-data index -- one LWS context per process copy, or context creation serialised against handshakes" -t task -p 1 \
	--deps discovered-from:VaRest-81s -d "Passport row coexist.context_create_reallocates_ssl_index: <observed>. Every lws_create_context stores a new CRYPTO_get_ex_new_index into the copy's openssl_websocket_private_data_index (ssl.c.o 0x1f4, 0x23a), and OpenSSL_client_verify_callback dereferences SSL_get_ex_data under it without a null check (openssl-client.c.o 0x1c8-0x1d6): a TLS handshake in flight across a context creation in the same copy crashes. In a monolithic game one copy serves WebSockets, WebSocketServer, WebSocketNetworking, HttpNetworkReplayStreaming and VaRest's lazily created context (spec 2.1). W1 designs which: one LWS context per process copy, or context creation serialised against handshakes; the W1 monolithic coexistence test opens an engine wss while VaRest's context is created."
```

When S1/S2 could not be applied because the coexist rows disagree or are `unverified`:

```bash
bd create "WS spec §1.2/D30: LWS copies and their log state on Linux are ambiguous in the W0 passport" -t task -p 2 \
	--deps discovered-from:VaRest-81s -d "Passport rows coexist.shared_lws_state <observed>; coexist.lws_symbol_owner <observed>; env.websockets_loaded_at_start <observed>. UE dlopens modules RTLD_LOCAL (Runtime/Core/Private/Unix/UnixPlatformProcess.cpp:107-148). Decide whether VaRest's copy is its own and correct §1.2's interposition and log-level bullets and D30."
```

When none of S3/S3b/S3c applied (`close.rewritten_codes` has a `-> no code` entry):

```bash
bd create "WS spec §3.4: resolve the 1002 row from the W0 passport (close.rewritten_codes is ambiguous)" -t task -p 2 \
	--deps discovered-from:VaRest-81s -d "Passport docs/passport/<date>-lws30-probe.md row close.rewritten_codes: <observed>. Some codes never reached WS_PEER_INITIATED_CLOSE, so whether LWS 3.0 rewrites them to 1002 is open; §3.4's 1002/1003/1007/1010 row stays conditional until W1's transport suite pins it."
```

When `hs.unregistered_selected_bound` differs with an observed that starts with "established" (opened, bound to another protocol):

```bash
bd create "WS W1: LWS 3.0 binds a connection with an unregistered selected subprotocol to another protocol than local_protocol_name" -t task -p 2 \
	--deps discovered-from:VaRest-81s -d "Passport row hs.unregistered_selected_bound: <observed>. Spec 2.2's subprotocol paragraph (E4) was left as it was; W1 decides how the transport finds its handler (lws_get_protocol(wsi), or registering every subprotocol)."
```

When `svc.kill_async_ms` and `svc.kill_async_ms_short_wait` are both over 1500 ms (E3 not applied: the delay is LWS's own):

```bash
bd create "WS W1: LWS_TO_KILL_ASYNC takes over 1.5 s even with a 100 ms service wait (W0 passport)" -t task -p 2 \
	--deps discovered-from:VaRest-81s -d "Passport rows svc.kill_async_ms <observed>, svc.kill_async_ms_short_wait <observed>, timeout.kill_pending_ms_short_wait <observed>. Spec 2.2's ConnectTimeout and 2.5's ping-timeout kill rely on it; W1 measures and bounds it (close the socket fd, or an earlier kill)."
```

When `rx.flow_resume_from_command` differs (neither resume form worked):

```bash
bd create "WS W1: reading cannot be resumed from a command on LWS 3.0 (W0 passport rx.flow_resume_form)" -t bug -p 1 \
	--deps discovered-from:VaRest-81s -d "Passport rows rx.flow_resume_from_command <observed>, rx.flow_resume_form <observed>. Spec 2.3's flow control resumes from the service thread outside the connection's callback; W1 must find a working resume (e.g. lws_callback_on_writable then resume inside the callback) before flow control ships."
```

When `tls.untrusted_rejected` differs, or E1b's condition holds and `tls.provided_ctx_connects` shows no connection:

```bash
bd create "WS: LWS 3.0 TLS verification does not behave as the engine glue assumes (W0 passport tls.*)" -t bug -p 0 \
	--deps discovered-from:VaRest-81s -d "Passport rows tls.untrusted_rejected, tls.load_extra_fired, tls.provided_ctx_connects: <observed>. W1 must not ship wss until this is understood."
```

When E25 applied (`ping.idle_survives_timeout_secs` differs):

```bash
bd create "WS W1: LWS 3.0 ends idle open connections by itself despite ws_ping_pong_interval = 0 (W0 passport)" -t task -p 2 \
	--deps discovered-from:VaRest-81s -d "Passport row ping.idle_survives_timeout_secs: <observed> (timeout_secs = 3, a silent server, one unanswered PING). Spec 2.1 (E25) now says the context keeps LWS's default timeout_secs; W1's transport suite measures what ends an idle connection under that default and keeps PingInterval and PingTimeout below it."
```

Last, the catch-all (re-review N-4): list the passport's differing ids with

```bash
sed -n '/^## Differs from the spec/,/^## Crashed/p' docs/passport/<date>-lws30-probe.md | grep -o '^- `[^`]*`'
```

and file one bead for every id there that no applied T, E or S row names in its condition and no bead above quotes (likely candidates: `coexist.both_ws`, `coexist.both_wss`, `coexist.engine_after_probe_teardown` against §1.2's "W0 verifies it" bullet; `tls.trusted_ca_verified`, `rx.order_preserved`, `rx.control_inside_fragmented`, `close.reason_utf8_bytes`, `tx.large_write_intact`). Priority P1 for `coexist.*` and `tls.*` ids, P2 for the rest:

```bash
bd create "WS W1: <id> differs from the spec (W0 passport)" -t task -p <1 for coexist.* and tls.*, else 2> \
	--deps discovered-from:VaRest-81s -d "Passport docs/passport/<date>-lws30-probe.md row <id>: observed <observed>; spec <the row's spec cell>. No Task 11 row of W0 consumed it; W1 decides what the spec and the transport do about it."
```

Each `<observed>` is the passport's cell for that id.

- [ ] **Step 7: Final verification**

Run, one after the other (no editor running):

```bash
python3 -I Tools/testserver/selftest.py && python3 -I Tools/testserver/ws_selftest.py && python3 -I Tools/lws_probe_passport.py --selftest
Tools/run_tests.sh VaRest
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
git status --short
```

Expected: all three selftests OK. The default suite passes with the same count as before W0 plus zero; probes never run in it (`grep -c 'Path={Probe\.' /w/Unreal/VaRestHost/Saved/Logs/VaRestTests.log` prints `0`). The monolithic game builds (`Result: Succeeded`). `git status` shows the spec and `CLAUDE.md` as modified, plus `.superpowers/` and other agents' files: no `__pycache__`, no `*.tmp`, no generated certificate scratch.

- [ ] **Step 8: Commit the spec corrections and the CLAUDE.md lines**

```bash
git add docs/superpowers/specs/2026-10-20-varest2-websocket-design.md CLAUDE.md
git commit -m "spec: WebSocket §0.3/§1.2/§2/§3.4/D30 corrected from the W0 libwebsockets 3.0 probe (passport <date>-lws30-probe); CLAUDE.md: probe commands

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

## Self-review

**1. Spec coverage** (§17 W0 row, §16.3, §16.4, §22.2 W0 additions, the brief, the rulings):

| Requirement | Task |
| --- | --- |
| `ws.py`: stdlib, `python3 -I`, plugged into server.py's hook, one process and port, shared tokens, runs alone, hand-written RFC 6455 (Accept, subprotocols, refusals, 7/16/64-bit lengths, masking with big-integer XOR, continuations, control frames, PING→PONG, close echo, strict UTF-8 → 1007, server fragments at chosen offsets) | 1 |
| §16.3 scenarios: echo, status, close/<code>?msg, close/none, close/1012, drop, silent, serverping, fragment?at, large?bytes&ascii, order, headers, auth, protocol?select, flaky, slowupgrade, noecho, badutf8 | 1, 2 (`/ws/route`, `/ws/apping` deferred to W4/W3, decision 7) |
| `ws_selftest.py` | 1, 2, 3 |
| TLS listener with a committed self-signed test certificate (`test-ca.pem`, `test-server.pem`, `test-server.key`, README; ruling Q5), reached with `wss+insecure` (ALLOW_SELFSIGNED) or the CA | 3 |
| Bounded inputs (control frames ≤ 125 bytes before reading, message totals before appending, 2 000 journal events per key), no traversal, server leaves with the runner, a missing certificate named on stderr | 1 (bounds tests), 3 (TLS lifecycle) |
| §16.4: `local_protocol_name` with an unregistered selection; selected protocol in `FILTER_PRE_ESTABLISH` | 6 |
| `lws_http_client_http_response` in `CONNECTION_ERROR` for 401/403/404/503 vs the strings, and in the cases without a status line | 6 (six risky cases, one per run, decision 10; `hs.refusal_*`) |
| empty close, codes 1004/1012/1013/1014/2000/4001/5000 as received | 7 |
| close echo timing against `/ws/noecho`; `CLIENT_CLOSED` vs `WSI_DESTROY` order | 7 (100 ms service wait, decision 9) |
| auto-PONG to server PINGs; `RECEIVE_PONG` payload echo | 7 |
| receive chunking (fragment boundaries, 64 KiB); `NO_FIN` sends | 8 |
| default `timeout_secs` (blackhole) and `LWS_TO_KILL_ASYNC` | 5 (`timeout.default_secs_blackhole`; kills with 10 s and 100 ms waits) |
| raw origin; `APPEND_HANDSHAKE_HEADER` length with 32 KiB | 6 |
| `rx_flow_control` pause/resume | 8 (both resume forms, each its own round) |
| TLS through the SSL module context on Linux | 9 (`tls.ssl_module_ctx`: null in modular builds, so it records the fallback and stands in a provided context, decision 13) |
| redirects (3xx), and the facts ruling Q2's rule needs | 6 (followed or not from both sides, target visibility, veto, `Host`) |
| coexistence with an engine `IWebSocket` (editor, ws and wss, both logs intact); the TLS index hazard | 9; monolithic: `unverified`, W1 (decision 8, bead in Task 11); `coexist.context_create_reallocates_ssl_index` (decision 16) |
| §22.2: `NoUpgradeResponse` string, `SO_SNDBUF` via `lws_get_socket_fd`, an auto-reply between two large messages | 6, 8, 8 |
| Passport `docs/passport/<date>-lws30-probe.md` | 10 |
| §0.3/§2 corrected where the probe disagrees, as a separate commit; §1.2, D30 and §3.4's 1002 row when unambiguous (ruling Q3) | 11 (Steps 1–4) |
| The probe commands in CLAUDE.md (ruling Q1) | 11 (Step 5) |
| `bDisableCertValidation` scoped and restored on every path, local server only (ruling Q4) | 9 (decision 11) |
| Platform items that cannot run here: "unverified here; release-pass item" | 5 (`release.*`, `coexist.monolithic`), 8 (`tx.so_sndbuf_*` off Linux/Mac), 9 (`coexist.lws_symbol_owner`, `coexist.context_create_reallocates_ssl_index` off Linux) |

Not covered, on purpose: the client proxy (`http_proxy_address`). §16.4 does not list it, and the release pass owns it per platform (§18). It is in the writer report. Not done either: a risky probe that creates a second rig in the middle of a TLS handshake (review I-3's optional item); the static evidence and the index fact decide the bead, and a crash probe would add only the crash.

**2. Placeholder scan.** No TBD or TODO. The angle-bracket values in Task 11 (`<observed>`, `<date>`) are runtime results that the plan cannot know: each one names the exact passport cell it is copied from. The only conditional plan text is the fallback in Task 4 Step 7 and Task 5 Step 3 (if the inside-callback command path does not work, flip the rig default), which states its trigger and its edit.

**3. Type consistency.** Checked across Tasks 4–9: `FRig::Find(ConnId, Reason, Out, NotePrefix)` matches by prefix, so `TEXT("kill")` also finds `"kill: no wsi"`, and Task 5's `Kill` checks `Note.Equals("kill")` before it measures (review M-12). `WriteNow` notes start with their label (`reply`, `first`), and `WriteNext` notes with `outbox`. `FHook`'s signature is the same in every test (`FConn&, lws*, int32, void*, void*, size_t`); Task 6's `TargetHook` and `StatusHook` return one. Hook markers are told apart by their note prefixes (`append `, `veto `, `status `, `paused`, `resumed`, `push`). `NoteOutcome` replaces every assertion on a library outcome; `TestTrue` stays on the server, the journals, the rigs and the precondition connections. The `ETls` values are used as declared. Every fact id a test logs appears in `EXPECTED` (135), and every `EXPECTED` id is logged by exactly one test (counted per prefix in Steps 3 of Tasks 5–9: 16+7, 35+6, 16+9, 26, 13+7). Tests: 4 rig, 30 probe, 6 risky; all 40 names start with `Probe.`.

**4. Review Focus.** Five lines, each with its test in the owning task: Task 4 `Rig.TeardownWithOpenConnection` (restore-the-bug) and `Rig.SequentialConnections`; Task 1 `test_protocol_errors_close_1002_and_the_server_goes_on` and `test_lengths_over_the_limit_close_1009`; Task 3 `test_tls_listener_in_the_ready_file_and_gone_with_the_parent` and `test_a_stalled_or_plain_client_does_not_block_others` (restore-the-bug).
