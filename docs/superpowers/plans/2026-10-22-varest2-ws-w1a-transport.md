# VaRest 2 — WebSocket W1a: the transport (connect, close, send, receive, TLS) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A new Runtime module `VaRestWebSocket` holding VaRest's own WebSocket client transport on the libwebsockets 3.0 the engine bundles: the seam `IVaRestSocketTransport` / `IVaRestSocketConnection`, a scriptable fake for the socket layer's tests, and the LWS implementation — one context and one service thread per process, one ordered event queue per connection, connect with VaRest's own URL and header rules, the close semantics of spec §2.5, in-place fragmented sends with a buffered amount and `Drained`, byte assembly with one UTF-8 check and a size limit, TLS through the engine's roots and pins with VaRest's own IP-literal check, a connect deadline measured on the service thread — green against the test server on ws and wss, beside the engine's `IWebSocket` in the editor.

**Architecture:** The game thread calls the transport; every libwebsockets call runs on one VaRest service thread (`FService`), which drains a command queue inside `LWS_CALLBACK_EVENT_WAIT_CANCELLED` and checks VaRest's own deadlines after each `lws_service` return. Each connection (`FConn`, the LWS userdata) pushes `Open`, `Message`, `Drained`, `Closed` and `Error` events into one SPSC queue it shares with its handle (`FSocketLink`); the consumer pops them, in wire order, and reports the inbound bytes it delivered. Only `lws_cancel_service` is called from another thread (T13); context creation and destruction run on the game thread with the service thread stopped.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), C++20, libwebsockets 3.0.0 and OpenSSL 1.1.1t as the engine bundles and links them (`AddEngineThirdPartyPrivateStaticDependencies`, `WebSockets.Build.cs:118`), the engine `SSL` module, UE Automation tests, `Tools/testserver/ws.py` (Python 3 standard library).

**Spec:** `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` — §17 row W1, §0.3 (T1-T29, the W0 facts), §1.1, §1.2, §2.1-§2.5, §3.4 (the rows the transport's failures feed), §3.5, §4, §15, §16.2, §16.5 items 5, 6, 9, §18, §20 (D21, D24, D29, D30, D32), §22 (SW-4; SW-5 and A2 as W1b and Task 7 below). Evidence: `docs/passport/2026-10-22-lws30-probe.md` (bracketed ids such as `[hs.redirect_veto]` are its rows). Rulings: `.superpowers/sdd/release-2.0-program.md` (R1: own client on LWS 3.0; Q2: no upgrade redirects), `.superpowers/sdd/ws-w0-plan-rulings.md`. Working reference code: `Source/VaRestTests/Private/LwsProbe/` (the W0 probe rig). The rest of W1 is `docs/superpowers/plans/2026-10-22-varest2-ws-w1b-service-policies.md` (outline).

## Why W1 is two plans

Spec §17 row W1 holds about twenty features. Written to this project's density (M3: 17 tasks in ~15k lines, M4a: 24 in ~12k) they are about 22 tasks. The seam: **W1a** is the transport's data path — everything that connects, writes, reads and ends one connection, plus TLS and the connect deadline that the data path needs to end every attempt. **W1b** is the service thread's *policies* on top of a working connection: protocol ping and RTT, the idle deadline and deadline suspension (SW-5 d, e), receive flow control, auto-replies with their priority lane and `SO_SNDBUF` (SW-5 a-c), the proxy checks, and the W1 exit runs (restore-the-bug 10 and 16, the monolithic coexistence run of VaRest-5ia). W1b changes nothing W1a declares; it adds one virtual (`IVaRestSocketConnection::SetIdleDeadline`), two fields (`FVaRestSocketEnd::bWhileReadPaused`, `FVaRestSocketEvent::bAutoReplied`), fields of `FVaRestSocketConnectRequest`, a `Pong` producer, a pause/resume path behind `Consumed`, and a reply lane in front of the outbox (`FConn::OnWriteable`) (review spec M-6). It also takes the address fallback and resolving off the service thread (B9, VaRest-gi0; ruling Q5, review conc I-4). W1b is planned in full when W1a has landed, against the code as it is then (the M4a/M4b precedent).

## Design decisions this plan makes (the spec is silent or says otherwise)

1. **The seam is pull-based.** Decision: `IVaRestSocketConnection::PopEvents` moves the queued events out; an optional `OnEvents` callback, given to `Connect`, runs on the producing thread after events are queued, and only schedules work; the consumer reports delivered inbound bytes with `Consumed(Bytes)`. Why: spec §2.1 makes the event queue SPSC "service thread → pipe" and §7.1 puts the consumer on a `UE::Tasks::FPipe` (W3), so the transport must not decide where events are handled; tests and W2 poll. Cost if wrong: W3 wraps a push-style sink around `OnEvents`.
2. **Exactly one terminal event per connection.** Decision: `Error` for an attempt that never opened (its `End.Code` is 0, spec §14), `Closed` for one that did; nothing after it (the link drops a later event with an Error log line, a transport bug made loud). Why: spec §2.1 lists both without saying which is which; W2's generation guard and classification need one end per attempt. Cost if wrong: none plausible.
3. **When the terminal event is queued.** Decision: when VaRest *kills* a connection (connect deadline, `CloseTimeout`, a pending attempt closed or released) the event is queued at that decision and the connection is detached, while LWS tears TCP down in the background (up to about 1 s later, decision 12); a close handshake reports when the connection ends (at most `CloseTimeout` later). Why: a kill's latency is LWS's (T26), not the deadline's; reporting at the decision makes `ConnectTimeout` exact -- unless the service thread itself is blocked in LWS 3.0's synchronous DNS (`getaddrinfo` inside `lws_client_connect_2`, review conc I-4; W1b B9, VaRest-gi0, resolves off the thread). An attempt VaRest ended sends nothing more: `APPEND_HANDSHAKE_HEADER` and `FILTER_PRE_ESTABLISH` return -1 and the certificate check refuses once its state is `Ended`, and a kill that has not landed one timeout walk after it was armed (`KillRearmSeconds`, 1.25 s) is armed again, because LWS's own stage timers can replace a pending timeout (`lws_generate_client_handshake` calls `lws_set_timeout(wsi, 0, 0)`; review conc I-1, `Close.CancelSendsNothing`). Not on every pass: each `lws_set_timeout` restarts the timeout at the current second, so re-arming every pass postpones the kill whenever the service thread is busy (re-review N-1, `Timeout.KillLatencyBusy`). Cost if wrong: W2 sees an end slightly before the fd is closed.
4. **`bWasClean` for a close VaRest started** means the connection ended before `CloseTimeout` and before LWS's own 5 s close-ack timer: an end by that timer, after our close frame and with no close from the peer, is not clean (review conc I-5: with `CloseTimeout` above about 5 s LWS's timer ends the unanswered close first, `Timeout.CloseLongerThanAck`). Why: LWS 3.0 raises no callback for the peer's reply to our close (`close> > WRITEABLE > CLIENT_CLOSED > WSI_DESTROY`, [close.ours_sequence]), so an echo and a bare TCP end cannot be told apart except by time; the spec (§2.5, D26) already says the server's final close reply is not reported. Cost if wrong: one field's meaning in the docs, and a server that echoes after 4.75 s counted unclean.
5. **Close and release before the open.** Decision: `Close()` on an attempt still connecting kills it and queues `Error{Canceled, By=Client}`; dropping the last handle is `Close(1001, "released")` when open (spec §1.4) and a cancel when pending; a released handle's events are discarded. Why: there is no close frame before the open; spec §3.1 abandons such attempts. Cost if wrong: none.
6. **Frames are moved, not shared, and fragmented in place.** Decision: `FVaRestSocketFrame` owns a `TArray<uint8>` with `Offset >= FrameHeadroom` (16 = `LWS_PRE` on every target, `libwebsockets.h:4773`; a `static_assert` holds it) before the payload (SW-4); fragment *k* is written from `Storage + Offset + k*64 KiB`, its header going into the last 16 bytes of fragment *k-1*, already handed to LWS. Why: LWS masks a client payload in place (RFC 6455 §5.3) and copies the unsent rest of a truncated write into its own buffer (T10, `libwebsockets.h:4910-4932`), so after `lws_write` returns the bytes before the next fragment are free; no copy per fragment. Cost if wrong: one copy per fragment.
7. **The buffered amount is exact to within one fragment.** Decision: bytes are counted at `Send` (game thread) and released after their `lws_write` — at once, or, when `lws_partial_buffered` says LWS still holds part of it, at the next `WRITEABLE` or the next service-loop sweep that sees the flag clear (`libwebsockets.h:6021-6037`: "never set at the start of a writeable callback"). `Drained` follows spec §2.4. Why: `lws_partial_buffered` is a flag, not a count [tx.large_write_result]; with 64 KiB fragments the error is at most one fragment (VaRest-8zj item 5; Task 14 amends §2.4). Cost if wrong: `OnDrained` one fragment late.
8. **One LWS context per process (VaRest-rti).** Decision: VaRest makes its context once per process and keeps it until engine exit (only a fork re-creates it). In modular builds it is made at the first `Connect` — VaRest's LWS copy is its own there [coexist.lws_symbol_owner], re-checked for `VaRestWebSocket` in Task 14. In shared-copy builds (`IS_MONOLITHIC || UE_MERGED_MODULES`) where another LWS-linking engine module (`WebSockets`, `WebSocketServer`, `WebSocketNetworking`, `HttpNetworkReplayStreaming`) is linked, `VaRestWebSocket`'s `StartupModule` first loads the engine's `WebSockets` module (if linked: its one context then exists before any VaRest handshake) and then creates VaRest's context (before game code can start an engine handshake). This refines ruling Q2's letter (review spec M-9): in a shared copy where none of those modules is linked, nobody else makes a context, so VaRest makes its own at the first `Connect`, as in modular builds -- equally safe. A failed start is latched: `FService::Start` never calls `lws_create_context` again after one failure, because a retry could run under an engine wss handshake (review conc I-2). No VaRest context is made while a VaRest wss attempt that LWS has not destroyed exists: a module-wide count (`TlsHandshakesInFlight`, raised by `FTransport::Connect` on the game thread -- where `FService::Start` reads it, so no window opens between a connect and another transport's start (re-review N-2) -- and lowered at `CLIENT_ESTABLISHED`, `WSI_DESTROY`, a refused connect or the `FConn`'s destruction) refuses `FService::Start` while it is not 0 (review conc I-3) -- an attempt that has *ended* still runs its handshake until LWS destroys it. Residual risk, documented in spec §1.2, not claimed closed: `WebSocketServer`, `WebSocketNetworking` and `HttpNetworkReplayStreaming` create contexts when game code starts them, and a module that starts before `VaRestWebSocket` (`PreDefault`, or earlier in `Default`) can already have an engine wss handshake in flight at VaRest's `StartupModule` (review conc M-11); a handshake in flight at such a creation can crash, and VaRest cannot see or serialise those creations (rti note M-6). Test transports make their own contexts, one at a time, each made only after the previous one's connections were destroyed. Why: every `lws_create_context` re-allocates its copy's `openssl_websocket_private_data_index`, which the client verify callback dereferences unchecked [coexist.context_create_reallocates_ssl_index]. Cost if wrong: a monolithic game that links `WebSockets` but never uses it pays one engine context, one VaRest context and the root store at startup (W1b's monolithic run measures it).
9. **IP-literal wss is verified by VaRest (VaRest-sjj).** Decision: in `PERFORM_SERVER_CERT_VERIFICATION`, an error `X509_V_ERR_HOSTNAME_MISMATCH` (62) at depth 0 on an IP-literal host is cleared when `X509_check_ip_asc` finds the IP in the leaf's SAN; OpenSSL's later checks (signatures, dates) still run and still fail on their own errors (they follow `check_id` in OpenSSL 1.1.1's `verify_chain`, and LWS keeps OpenSSL's verdict unless the error is reset, `libwebsockets.h:1015-1044`). A refused certificate is `Failure = TlsVerification` with the X509 error and depth in `Detail`; Task 14 amends §3.4 to make it terminal, captive portals included (ruling Q3: the game reconnects explicitly). VaRest's verdict is a free function, `EvaluateServerCertificate`, so a unit test runs it inside OpenSSL's real verification on leaves made in memory (right IP, wrong IP, DNS-only SAN, untrusted; review spec I-5), and the wrong IP is also tested against the server through the verify callback (`Tls.WrongIp`, review conc M-10). Why: refusing `wss://<ip>` at `Connect` would break servers with IP certificates; LWS 3.0 checks an IP host as a DNS name and always fails it [tls.hostname_ip_literal]. Cost if wrong: about 20 lines in the TLS path to drop.
10. **Redirects are refused at the second `APPEND_HANDSHAKE_HEADER` (VaRest-2rt, ruling Q2).** Decision: the first `APPEND_HANDSHAKE_HEADER` of an attempt appends; any later one appends nothing and returns -1, so the second request never goes out [hs.redirect_veto] and LWS gives no `CONNECTION_ERROR` (the transport reports the end itself); a certificate check on a `ws://` attempt (a redirect to wss, [hs.redirect_to_tls]), or on a wss attempt after its first `APPEND_HANDSHAKE_HEADER` (a redirected wss attempt makes its second TLS handshake before its second `APPEND`), fails it; `FILTER_PRE_ESTABLISH` rejects a second-request response as a second guard. `Failure = Redirect`, `HttpStatus = 0`: LWS 3.0 follows the 3xx before any callback sees the status. Whether LWS replays appended headers by itself is measured by a probe test (Task 10); the count under a configured proxy is W1b's (rti note M-7), and until it is known the test runner starts the editor without the proxy variables (review conc M-12). The certificate-check guard also refuses a TLS 1.2 renegotiation after the request, which OpenSSL 1.1.1 clients accept by default (review conc M-14): rare, and left so. Cost if wrong: a server that relies on an upgrade redirect needs its final URL (ruling Q2's own cost).
11. **libwebsockets' error strings decide a failure; `lws_http_client_http_response` is never called.** Decision: every status VaRest reports comes from the `CONNECTION_ERROR` string ("unauthorized" 401, "forbidden" 403, "not 101, was N"); the full table is Task 5's. Why: the strings carry every status the function returns there [hs.status_in_connection_error.refused], and without a header table it dereferences NULL (T18). Cost if wrong: none.
12. **Kills report at the deadline; the wait is short while one is in flight (VaRest-wcv).** Decision: `lws_service` waits at most 30 s with no connection (commands wake it), 1 s with connections (spec §2.1), and 100 ms while a kill is in flight or LWS holds a partial write; the time from a kill to `WSI_DESTROY` is counted and Task 11 asserts it stays under 1.6 s (`Timeout.KillLatency`). A kill that has not landed 1.25 s after it was armed is armed again, until `WSI_DESTROY` (review conc I-1: LWS's stage timers can overwrite it), never on every pass (re-review N-1: each `lws_set_timeout` restarts the timeout at the current second, `__lws_set_timeout` storing `time()`, so a busy service thread would postpone the kill; `Timeout.KillLatencyBusy` asserts the 1.6 s bound with a busy neighbour). Why: LWS walks its timeouts only when the wall-clock second changed, on a return of `lws_service` (T26), so a 1 s cap alone can land a kill about 2 s late. Cost if wrong: an fd lives a second longer.
13. **VaRest parses the URL itself, on the game thread.** Decision: `ws`, `wss`, and outside Shipping `wss+insecure`; no user info, no fragment; ASCII hosts only (IDN needs punycode, as `VaRest::Http::HasNonAsciiHost` refuses for HTTP); IPv6 in brackets; path and query bytes past ASCII, spaces and controls percent-encoded (RFC 3987 §3.1, as `VaRest::Http::JoinUrl`); `http(s)://` stays the socket layer's sugar (spec §3.2), not the transport's. Why: `lws_parse_uri` writes into its input and takes any scheme (`libwebsockets.h:5800-5819`), and a bad URL must be classified before the service thread runs. Cost if wrong: a URL form some server needs is refused with `BadUrl`.
14. **Header rules.** Decision: names are RFC 7230 tokens; values are VCHAR, space or tab; `Host`, `Upgrade`, `Connection`, `Origin` and `Sec-WebSocket-*` are refused (LWS writes them, or VaRest does from `Origin`/`Protocols`); subprotocols are tokens offered once (case-sensitive), joined with ", " (LWS skips the spaces when it matches the selection); the header block is refused over 32 KiB on the game thread, and checked again against the room LWS reports in `APPEND_HANDSHAKE_HEADER` (T5, [hs.append_header_space]: 32482 bytes for a short path). Why: spec §2.2 and D23; a duplicate `Origin` or `Sec-WebSocket-Protocol` breaks the upgrade. Cost if wrong: an app that set one of them as a header gets `BadHandshake`.
15. **The allowlist is the engine's filter on the scheme and host only.** Decision: `UE::Core::FURLRequestFilter(TEXT("Online.HttpManager"), GEngineIni)` per connect on the game thread, given `scheme://host[:port]` (no path, no query: its refusal line at `LogCore` prints the URL it was given, `URLRequestFilter.cpp:136-140`), `wss+insecure` as `wss`; no bypass key. A project whose `[Online.HttpManager https]` lists domains must add `[Online.HttpManager wss]` too: the filter refuses a scheme it has no section for (`URLRequestFilter.cpp:131-140`). Tests inject the map (decision 19), never the ini. Why: spec §2.2, E7. Cost if wrong: one doc line.
16. **Logging.** Decision: VaRest's LWS copy logs `LLL_ERR | LLL_WARN` only, both as Warnings in `LogVaRestSocket` (`libwebsockets(err): ...`, control characters shown as `?`), as the engine reports its own (`LwsWebSocketsManager.cpp:403-424`); the D30 rule runs at each context start. The transport itself logs Verbose lifecycle lines and Warnings for what it changed (a clamped timeout, a replaced close code, a cut or repaired reason). Basic/Headers/Body logging is W3's (spec §12). The LLM tag is `VaRest_WebSocket`, the module's own: spec §2.1's `VaRest` tag is M4a's, which W1 must not depend on; W5 parents it. Cost if wrong: one renamed tag.
17. **Timeouts.** `ConnectTimeout` above `VaRest::WebSocket::MaxConnectTimeoutSeconds` (60 s, the context's `timeout_secs`, spec §2.2), non-finite or not positive, becomes 60 s with a Warning. LWS itself cuts the TCP connect and the proxy exchange at a fixed 20 s whatever `timeout_secs` says (`lws_client_connect_2`, review conc M-1), so a `ConnectTimeout` in (20, 60] on a stalled TCP connect ends at about 20 s; an empty `CONNECTION_ERROR` is classified by the time the attempt took -- `ConnectTimeout` from 19.5 s on, `Unknown` before (review conc M-2). `CloseTimeout` is always armed; above LWS's fixed 5 s close-ack (T20) it acts as about 5 s, and that end is unclean (decision 4). `MaxMessageBytes` is clamped to [1, 2^31 - 2] (a `TArray` holds the assembly), and the assembly grows from the bytes received, never from a frame's declared length (review conc M-13). Cost if wrong: a Warning's wording.
18. **A send before the open is queued and written after `CLIENT_ESTABLISHED`**, which re-requests `WRITEABLE` (spec §2.4: LWS 3.0 drops a request made during the handshake); our close goes after the data already queued, bounded by `CloseTimeout` from the call; a protocol failure (1007, 1009) drops the outbox first. Cost if wrong: none.
19. **Test transports.** Decision: `VaRest::WebSocket::Testing::MakeLwsTransport` makes a transport with its own context and thread, a trusted extra PEM (the test CA), a TCP address override and an allowlist override. wss tests use `wss://localhost:<tls port>` with TCP to 127.0.0.1 (the DNS-name path through LWS's own name check), and Task 13 adds `wss://127.0.0.1:<tls port>` (decision 9's path). This is ruling Q5's shape: the LWS `address` is 127.0.0.1 and `host`/`Host` is `localhost`, so an IPv6-first resolver cannot make a test flaky. Why: `localhost` resolves to `::1` first on this host (`getent ahosts localhost`) while the listener binds 127.0.0.1 only, and LWS 3.0 connects to the first resolved address only (ruling Q5; recorded by `Probe.VaRest.Lws.Connect.LocalhostAddress`, out of the default suite). The fallback to the next address, and resolving off the service thread (review conc I-4), are W1b B9 (VaRest-gi0, P1). Cost if wrong: none for the product.
20. **The failure enum carries W1b's two deadline failures now** (`HeartbeatTimeout`, `IdleTimeout`), so the `UENUM` changes once. Cost if wrong: two unused enumerators until W1b.
21. **Names that freeze with W5** (they become pins and `FVaRestSocketClose` fields): `EVaRestSocketFrameKind`, `EVaRestSocketCloseBy`, `EVaRestSocketFailure` and their enumerator names (CLAUDE.md freeze line, Task 14). Plain C++ and not frozen: `FVaRestSocketFrame`, `FVaRestSocketEnd`, `FVaRestSocketEvent`, `EVaRestSocketEventType`, `FVaRestSocketConnectRequest`, `IVaRestSocketTransport`, `IVaRestSocketConnection`. Nothing persisted: `UVaRestSocketSettings` is W5's.
22. **Where there is no multithreading** the service is ticked through `FSingleThreadRunnable` by the forkable "thread" the engine makes (`Fork.h:86-102`; the engine's manager does the same, `LwsWebSocketsManager.cpp:341-358`); a forking server's child shuts the default transport down and starts again (`LwsWebSocketsManager.cpp:260-279`). Neither runs on this host: release-pass items.
23. **A peer's close code a peer must not send** (outside 1000-1003, 1007-1014, 3000-4999) becomes 1005 with `Detail` naming the code (spec §2.5); LWS 3.0's own rewrite of 1004-1006, 1012-1016, 2000 and 2999 to 1002 [close.rewritten_codes] is pinned by a test, for W2's VaRest-diy.
24. **The `wss+insecure` path accepts what LWS's flags accept** (`LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK`, spec §2.2) and skips the certificate manager's pin check; other errors (an expired certificate) still fail.
25. **Shutdown writes the close frames already asked for first** (review conc M-3). Decision: `FService::Shutdown` gives the running thread up to `ExitFlushSeconds` (0.25 s) to write every close asked for before the call (`FServiceCounters::UnsentCloses`), then stops it. Why: at engine exit the game instances' shutdown queues the hub's 1001 "shutdown" (spec §1.4; `GameEngine.cpp:1377`, `:1391`) just before `OnEnginePreExit` (`:1396`, `UnrealEngine.cpp:2709`) shuts the transport down; without the wait every server sees 1006 at every game exit. Not where the main thread ticks the service (no real thread). Cost if wrong: up to 0.25 s longer exits with connections closing.
26. **A send after `Close()` is refused at the call** (review conc M-6): the handle sets `FSocketLink::bCloseRequested` and `Send` returns false, as the fake does; before, the service thread dropped the frame silently. Cost if wrong: none.

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

## Review Focus

- **A server that ends a connection with a close nobody can trust** — an empty close frame, a one-byte one, a code ≥ 5000, a reason that is not UTF-8, a code LWS rewrote — must give the app 1005, 1002, 1005, a repaired reason, and 1002 respectively, never garbage (E27) and never a crash. Test owner: Task 9 (`Close.PeerOdd`: the empty, one-byte, 5000 and rewritten 1012 closes; `Close.PeerBadUtf8Reason`; restore-the-bug 5).
- **A text message whose code point straddles a frame boundary or a 64 KiB receive chunk** arrives as one message, byte for byte, never with U+FFFD (E9). Test owner: Task 12 (`Receive.SplitCodePoint`, `Receive.Large`; restore-the-bug 6).
- **An upgrade answered with a 302** — to the same port, an absolute URL, or wss — is never followed: the target never sees a request, so an `Authorization` header never reaches it, and the attempt ends `Redirect`, not a TLS or unknown error. Test owner: Task 10 (`Redirect.SameServer`: relative and absolute, the target's journal checked for the request and the header; `Redirect.OtherScheme`: ws to wss, wss to ws).
- **wss to an IP literal with an IP SAN, to another IP, to an IP with a DNS-only SAN, to a name signed by an untrusted CA, with a pinned key that does not match** — opens; ends `TlsVerification` with X509 error 62; ends with 62; ends naming error 19; ends `TlsVerification` — and a broken check never lets an untrusted chain or another IP's certificate through (ruling Q3). Test owner: Task 13 (`Tls.IpLiteral`, `Tls.WrongIp`, `Tls.Refused`: error 19 by name and by IP, `Tls.PinMismatch`, `Tls.CertificateIp`, and `VaRest.WebSocket.Lws.TlsIpCheck` through OpenSSL's real verification).
- **The app closes or drops its handle while the attempt is still connecting, or while a large message is still going out, or the engine exits right after the hub closes its sockets** — exactly one terminal event (`Canceled`, or `Closed` with its own code), nothing after it, the cancelled attempt's request (and its `Authorization`) never on the wire, the connection gone from the transport, a send after `Close` refused, and an open connection's server sees the 1001 close. Test owners: Task 7 (`Connect.Release`), Task 9 (`Close.Ours`, `Close.CancelSendsNothing`, `Close.ShutdownWritesQueuedClose`), Task 8 (`Send.CloseAfterData`).
- **Headers that do not fit the 32 KiB handshake buffer, or a header value with CR/LF or non-ASCII** — refused as `BadHandshake` before or instead of the upgrade, never truncated or overrun (E19). Test owner: Task 7 (`Connect.HeaderRoom`: over 32 KiB, and within it but past LWS's room; `Connect.BadRequest`: CR/LF and non-ASCII values).
- **A send of several MiB to a server that reads as it can** arrives intact; the buffered amount returns to 0 and `Drained` fires. Test owner: Task 8 (`Send.BufferedAndDrained`).

---

## File structure

```
VaRest.uplugin                                                  + module VaRestWebSocket (T1)
Source/VaRestWebSocket/
  VaRestWebSocket.Build.cs                                      NEW (T1)
  Public/VaRestSocketTypes.h                                    NEW (T2): frame (SW-4), kinds, who closed, failures, end, event, close rules
  Public/VaRestSocketTransport.h                                NEW (T3): IVaRestSocketTransport, IVaRestSocketConnection, FVaRestSocketConnectRequest,
                                                                MaxConnectTimeoutSeconds; GetDefaultTransport (T7)
  Public/VaRestFakeSocketTransport.h                            NEW (T3): FVaRestFakeSocketTransport, FVaRestFakeSocketAttempt
  Public/VaRestSocketTesting.h                                  NEW (T7): MakeLwsTransport, counters, LWS copy checks; CertificateMatchesIp (T13)
  Private/VaRestWebSocketModule.cpp                             NEW (T1); startup policy, pre-exit and fork hooks (T7)
  Private/VaRestWebSocketLog.h                                  NEW (T1): LogVaRestSocket, LLM tag VaRest_WebSocket
  Private/VaRestLwsInclude.h                                    NEW (T1): the include dance
  Private/VaRestSocketTypes.cpp                                 NEW (T2)
  Private/VaRestSocketLink.h/.cpp                               NEW (T3): the per-connection queue and counters
  Private/VaRestFakeSocketTransport.cpp                         NEW (T3)
  Private/VaRestSocketTarget.h/.cpp                             NEW (T4): URL, Host, Origin, subprotocols, headers, allowlist
  Private/VaRestLwsErrors.h/.cpp                                NEW (T5): CONNECTION_ERROR strings
  Private/VaRestLwsService.h/.cpp                               NEW (T6): context, thread, commands, waits, deadlines loop, log level
  Private/VaRestLwsConn.h/.cpp                                  NEW (T7): one connection; handshake, open, errors, ends (T7, T9, T10, T11)
  Private/VaRestLwsConnSend.cpp                                 NEW (T7): Send, Close, WRITEABLE; fragments in place (T8)
  Private/VaRestLwsConnReceive.cpp                              NEW (T12): assembly, UTF-8, size limit
  Private/VaRestLwsConnTls.cpp                                  NEW (T10): certificate verification; VaRest's checks (T13)
  Private/VaRestLwsTransport.h/.cpp                             NEW (T7): FTransport, the handle, the default transport, the startup policy
  Private/VaRestSocketTesting.cpp                               NEW (T7); CertificateMatchesIp (T13)
  Private/Tests/VaRestSocketModuleTest.cpp                      NEW (T1)
  Private/Tests/VaRestSocketTypesTest.cpp                       NEW (T2)
  Private/Tests/VaRestSocketFakeTest.cpp                        NEW (T3)
  Private/Tests/VaRestSocketTargetTest.cpp                      NEW (T4)
  Private/Tests/VaRestLwsErrorsTest.cpp                         NEW (T5)
  Private/Tests/VaRestLwsServiceTest.cpp                        NEW (T6), the startup policy's test included
  Private/Tests/VaRestLwsTlsTest.cpp                            NEW (T13): EvaluateServerCertificate in OpenSSL's verification, in-memory leaves
Source/VaRestTests/VaRestTests.Build.cs                         + VaRestWebSocket (T1); + Projects (T13)
Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.h/.cpp    NEW (T7): site, schemes, transport, events, journal
Source/VaRestTests/Private/WebSocket/VaRestWsConnectTest.cpp    NEW (T7)
Source/VaRestTests/Private/WebSocket/VaRestWsSendTest.cpp       NEW (T8)
Source/VaRestTests/Private/WebSocket/VaRestWsCloseTest.cpp      NEW (T9)
Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp    NEW (T9: CloseTimeout; T11: ConnectTimeout, kill latency)
Source/VaRestTests/Private/WebSocket/VaRestWsRedirectTest.cpp   NEW (T10)
Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRedirectReplayTest.cpp   NEW (T10): does LWS replay appended headers (VaRest-2rt I-1)
Source/VaRestTests/Private/WebSocket/VaRestWsReceiveTest.cpp    NEW (T12)
Source/VaRestTests/Private/WebSocket/VaRestWsTlsTest.cpp        NEW (T13)
Source/VaRestTests/Private/WebSocket/VaRestWsCoexistTest.cpp    NEW (T14): engine IWebSocket beside VaRest, ws and wss; shutdown stress
Tools/run_tests.sh                                              the editor runs without the proxy variables (T7, review conc M-12)
CLAUDE.md                                                       module row, freeze line (T14)
docs/superpowers/specs/2026-10-20-varest2-websocket-design.md   §1.2, §2.1, §2.2, §2.4, §2.5, §3.4, §6.3, §15, §16.2, §16.3, §18, D21, D24, §22.2 (T14)
```

Task order: 1 module · 2 types · 3 seam, link, fake · 4 targets · 5 error strings · 6 service · 7 connect and the write path · 8 fragments, buffered amount, `Drained` · 9 close · 10 redirects · 11 connect deadline · 12 receive · 13 TLS · 14 coexistence and close-out. Tasks 1-6 are pure or LWS-only (no server); from Task 7 on, every integration test runs on ws and wss.

---

### Task 1: The module — Build.cs, the plugin entry, the log category, the LWS include, a link smoke test

The module exists and links libwebsockets, OpenSSL and zlib the way the engine's `WebSockets` module does (`WebSockets.Build.cs:118-119`), without depending on that module (spec §1.1).

**Files:**
- Create: `Source/VaRestWebSocket/VaRestWebSocket.Build.cs`, `Source/VaRestWebSocket/Private/VaRestWebSocketModule.cpp`, `Source/VaRestWebSocket/Private/VaRestWebSocketLog.h`, `Source/VaRestWebSocket/Private/VaRestLwsInclude.h`
- Modify: `VaRest.uplugin` (one module entry), `Source/VaRestTests/VaRestTests.Build.cs` (one dependency)
- Test: `Source/VaRestWebSocket/Private/Tests/VaRestSocketModuleTest.cpp`

**Interfaces:**
- Consumes: nothing.
- Produces: module `VaRestWebSocket` (export macro `VARESTWEBSOCKET_API`); `DECLARE_LOG_CATEGORY_EXTERN(LogVaRestSocket, Log, All)` and `LLM_DECLARE_TAG(VaRest_WebSocket)` in `VaRestWebSocketLog.h`; `VaRestLwsInclude.h`, the only include of `libwebsockets.h` and OpenSSL in the module.

- [ ] **Step 1: Write the failing test** — create `Source/VaRestWebSocket/Private/Tests/VaRestSocketModuleTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsInclude.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketModuleLwsTest, "VaRest.WebSocket.Module.Lws",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketModuleLwsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("the module is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestWebSocket")));
	// T1: libwebsockets 3.0.0 on every target (lws_config.h:34; passport env.lws_version)
	const FString Version = UTF8_TO_TCHAR(lws_get_library_version());
	TestTrue(FString::Printf(TEXT("libwebsockets 3.0.x is linked (got \"%s\")"), *Version), Version.StartsWith(TEXT("3.0."), ESearchCase::CaseSensitive));
	// The engine's OpenSSL (ThirdParty/OpenSSL/OpenSSL.Build.cs:12; passport env.openssl_version)
	const FString OpenSsl = UTF8_TO_TCHAR(OpenSSL_version(OPENSSL_VERSION));
	TestTrue(FString::Printf(TEXT("OpenSSL 1.1.1 is linked (got \"%s\")"), *OpenSsl), OpenSsl.StartsWith(TEXT("OpenSSL 1.1.1"), ESearchCase::CaseSensitive));
	return true;
}

#endif
```

- [ ] **Step 2: Run it to see it fail**

Run: `Tools/run_tests.sh VaRest.WebSocket.Module`
Expected: `run_tests: filter '^VaRest.WebSocket.Module' matched no tests` (exit 3): without a Build.cs, UBT does not build the directory.

- [ ] **Step 3: The Build.cs** — create `Source/VaRestWebSocket/VaRestWebSocket.Build.cs`:

```csharp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

// WebSocket spec 1.1: VaRest's own WebSocket transport on the libwebsockets 3.0 the engine bundles (ruling R1), and from W2
// on the socket, its stages and its Blueprint surface. Dependencies mirror the engine's WebSockets module
// (WebSockets.Build.cs:92-120); that module itself is never one.
public class VaRestWebSocket : ModuleRules
{
	public VaRestWebSocket(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			// PUBLIC: VaRestSocketTypes.h declares UENUMs (W3-W5 hand them to Blueprints)
			"CoreUObject",
			// PUBLIC: W2-W5's socket API takes FVaRestJson and FVaRestError; W1 uses the UTF-8 validator (VaRestJsonUtf8.h)
			"VaRestJson",
			"VaRestHttp",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// FPlatformHttp::GetConfiguredProxyAddress (GenericPlatformHttp.h:193), as LwsWebSocketsManager.cpp:196-203
			"HTTP",
			// The SSL module's context (monolithic and merged builds) and the certificate manager (roots, pinned keys), as
			// WebSockets.Build.cs:119
			"SSL",
		});

		// The engine's own link of libwebsockets (WebSockets.Build.cs:118; ModuleRules.cs:1601-1607 makes them private
		// dependencies): static OpenSSL, libwebsockets and zlib, one copy per module in modular builds (spec 1.2)
		AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL", "libWebSockets", "zlib");
	}
}
```

- [ ] **Step 4: The plugin entry** — in `VaRest.uplugin`, insert after the `VaRestHttp` entry (its closing `},`):

```json
		{
			"Name" : "VaRestWebSocket",
			"Type" : "Runtime",
			"LoadingPhase" : "Default",
			"PlatformAllowList" : [ "Win64", "Mac", "Linux", "Android", "IOS" ]
		},
```

(Spec §1.1: `Type Runtime`, `LoadingPhase Default`, the five platforms. UBT builds every runtime module of an enabled plugin that is valid for the target, `UEBuildTarget.cs:5935-5951`, so an HTTP-only game links it too — ruling R4.)

- [ ] **Step 5: The log category, the include, the module class** — create `Source/VaRestWebSocket/Private/VaRestWebSocketLog.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"
#include "Logging/LogMacros.h"

/** WebSocket spec 12: the socket's log category; the transport's own lines (W1) and VaRest's libwebsockets lines go here */
DECLARE_LOG_CATEGORY_EXTERN(LogVaRestSocket, Log, All);

/**
 * The module's LLM tag (plan decision 16): spec 2.1 names the core's VaRest tag, which M4a defines and W1 must not depend
 * on; W5 parents this one under it
 */
LLM_DECLARE_TAG(VaRest_WebSocket);
```

Create `Source/VaRestWebSocket/Private/VaRestLwsInclude.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

// The one place VaRestWebSocket includes the engine-bundled libwebsockets 3.0 and its OpenSSL, as the engine's WebSockets
// module does (Runtime/Online/WebSockets/Private/Lws/LwsWebSocketsManager.h:14-24) and the W0 probe proved
// (Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeLws.h): Windows types allowed around the headers (libwebsockets.h:52-61
// pulls in winsock2.h there), inside the third-party warning scope. Never included from a public header.
//
// UI is renamed for the headers: OpenSSL's typedef struct ui_st UI (OpenSSL/1.1.1t/include/Unix/openssl/ossl_typ.h:144)
// collides with CoreUObject's namespace UI (UObject/ObjectMacros.h:965), which this module's UENUM headers bring in. The
// engine does the same where it includes OpenSSL next to UObject code (Runtime/Online/BuildPatchServices/Private/Common/Crypto.cpp:6-12).

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#endif

#define UI UI_ST
THIRD_PARTY_INCLUDES_START
#include "libwebsockets.h"
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>
THIRD_PARTY_INCLUDES_END
#undef UI

#if PLATFORM_WINDOWS
#include "Windows/HideWindowsPlatformTypes.h"
#endif
```

Create `Source/VaRestWebSocket/Private/VaRestWebSocketModule.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VaRestWebSocketLog.h"

DEFINE_LOG_CATEGORY(LogVaRestSocket);
LLM_DEFINE_TAG(VaRest_WebSocket);

/** Task 7 adds the startup policy (plan decision 8) and the engine-exit and fork hooks */
class FVaRestWebSocketModule final : public IModuleInterface
{
};

IMPLEMENT_MODULE(FVaRestWebSocketModule, VaRestWebSocket);
```

- [ ] **Step 6: VaRestTests depends on it** — in `Source/VaRestTests/VaRestTests.Build.cs`, in `PrivateDependencyModuleNames`, after `"VaRestHttp",` add:

```csharp
			// WebSocket W1: the transport's integration suite (VaRest.Integration.WebSocket.Transport.*) and its test hooks
			// (VaRestSocketTesting.h)
			"VaRestWebSocket",
```

- [ ] **Step 7: Run it**

Run: `Tools/run_tests.sh VaRest.WebSocket.Module`
Expected: `run_tests: 1 passed, 0 failed`.

- [ ] **Step 8: Restore-the-bug** — delete the `AddEngineThirdPartyPrivateStaticDependencies(...)` line from `VaRestWebSocket.Build.cs`: the build FAILS (`libwebsockets.h` not found, or `undefined symbol: lws_get_library_version` at link) — the module's link is its own, not inherited; restore; `Tools/run_tests.sh VaRest.WebSocket.Module` PASSES.

- [ ] **Step 9: The whole suite still passes**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed` (the count is M4a's total plus this one).

- [ ] **Step 10: Commit**

```bash
git add Source/VaRestWebSocket/VaRestWebSocket.Build.cs Source/VaRestWebSocket/Private/VaRestWebSocketModule.cpp \
  Source/VaRestWebSocket/Private/VaRestWebSocketLog.h Source/VaRestWebSocket/Private/VaRestLwsInclude.h \
  Source/VaRestWebSocket/Private/Tests/VaRestSocketModuleTest.cpp VaRest.uplugin Source/VaRestTests/VaRestTests.Build.cs
git commit -m "ws: W1a task 1 -- module VaRestWebSocket linking the engine-bundled libwebsockets 3.0, OpenSSL and zlib, LogVaRestSocket, the LWS include

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 2: The socket types — frames with a payload offset (SW-4), who closed, failures, ends, events, the close-code rules

Pure types and functions, used by everything after. The three `UENUM`s become Blueprint-visible in W3-W5 and freeze then (plan decision 21).

**Files:**
- Create: `Source/VaRestWebSocket/Public/VaRestSocketTypes.h`, `Source/VaRestWebSocket/Private/VaRestSocketTypes.cpp`
- Test: `Source/VaRestWebSocket/Private/Tests/VaRestSocketTypesTest.cpp`

**Interfaces:**
- Consumes: `VaRest::Json::Utf8Length`, `VaRest::Json::WriteUtf8`, `VaRest::Json::IsValidUtf8`, `VaRest::Json::AppendRepairedUtf8` (`VaRestJsonUtf8.h`).
- Produces:
  - `UENUM(BlueprintType) enum class EVaRestSocketFrameKind : uint8 { Text, Binary }`
  - `UENUM(BlueprintType) enum class EVaRestSocketCloseBy : uint8 { Client, Server, Transport, Heartbeat, Timeout }`
  - `UENUM(BlueprintType) enum class EVaRestSocketFailure : uint8 { None, Canceled, Shutdown, BadUrl, NotAllowed, BadHandshake, ConnectFailed, TlsVerification, ConnectTimeout, Redirect, UpgradeRefused, NoUpgradeResponse, BadUpgradeResponse, ProtocolMismatch, MessageTooBig, InvalidUtf8, HeartbeatTimeout, IdleTimeout, Unknown }`
  - `VaRest::WebSocket::FrameHeadroom = 16`, `VaRest::WebSocket::MaxCloseReasonBytes = 123`
  - `struct VARESTWEBSOCKET_API FVaRestSocketFrame { EVaRestSocketFrameKind Kind; TArray<uint8> Storage; int32 Offset = FrameHeadroom; int32 Length = 0; static FVaRestSocketFrame Text(FUtf8StringView); static FVaRestSocketFrame Text(FStringView); static FVaRestSocketFrame Binary(TConstArrayView<uint8>); static TOptional<FVaRestSocketFrame> FromStorage(EVaRestSocketFrameKind, TArray<uint8>&&, int32 InOffset, int32 InLength); bool IsValid() const; TConstArrayView<uint8> GetPayload() const; }`
  - `struct FVaRestSocketEnd { int32 Code = 0; FString Reason; bool bWasClean = false; EVaRestSocketCloseBy By = Transport; EVaRestSocketFailure Failure = None; int32 HttpStatus = 0; FString Detail; }`
  - `enum class EVaRestSocketEventType : uint8 { Open, Message, Pong, Drained, Closed, Error }`
  - `struct FVaRestSocketEvent { EVaRestSocketEventType Type; EVaRestSocketFrameKind Kind; bool bFinal = true; TArray<uint8> Bytes; FString Protocol; double Seconds = 0.0; FVaRestSocketEnd End; double Time = 0.0; bool IsTerminal() const; }`
  - `bool VaRest::WebSocket::IsValidCloseCode(int32)`, `int32 SanitizeCloseCode(int32 Code, bool& bOutReplaced)`, `TArray<uint8> CloseReasonBytes(FStringView Reason, bool& bOutTruncated)`, `struct FPeerClose { int32 Code = 1005; int32 ReceivedCode = -1; FString Reason; bool bMalformed = false; bool bCodeNormalised = false; bool bReasonRepaired = false; }`, `FPeerClose ParsePeerClose(TConstArrayView<uint8> Payload)` (all `VARESTWEBSOCKET_API`, any thread)
  - `LexToString(EVaRestSocketFailure)`, `LexToString(EVaRestSocketCloseBy)`, `LexToString(EVaRestSocketEventType)`

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestWebSocket/Private/Tests/VaRestSocketTypesTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJsonUtf8.h"
#include "VaRestSocketTypes.h"

namespace VaRest::WebSocket::VaRestSocketTypesTestPrivate
{
	FString PayloadText(const FVaRestSocketFrame& Frame)
	{
		const TConstArrayView<uint8> Payload = Frame.GetPayload();
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Payload.GetData()), Payload.Num());
	}

	bool IsUtf8(const TArray<uint8>& Bytes)
	{
		return VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesFrameTest, "VaRest.WebSocket.Types.Frame",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesFrameTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTypesTestPrivate;
	const FVaRestSocketFrame Hello = FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("hello")));
	TestTrue(TEXT("a text frame is valid"), Hello.IsValid());
	TestTrue(TEXT("... and Text"), Hello.Kind == EVaRestSocketFrameKind::Text);
	TestEqual(TEXT("... its payload starts after the headroom"), Hello.Offset, FrameHeadroom);
	TestEqualSensitive(TEXT("... and is the text"), PayloadText(Hello), FString(TEXT("hello")));

	const FVaRestSocketFrame Cyrillic = FVaRestSocketFrame::Text(FStringView(TEXT("при")));
	TestEqual(TEXT("UTF-16 text becomes UTF-8 (three two-byte letters)"), Cyrillic.Length, 6);
	// An unpaired surrogate becomes U+FFFD, EF BF BD (VaRest::Json::WriteUtf8)
	const TCHAR Lone[] = { static_cast<TCHAR>(0xD800), TEXT('a'), 0 };
	const FVaRestSocketFrame Repaired = FVaRestSocketFrame::Text(FStringView(Lone, 2));
	const TArray<uint8> Expected = { 0xEF, 0xBF, 0xBD, 'a' };
	// TArrayView has no operator== in UE 5.8 (ArrayView.h names one only in comments): compare through TArray (Array.h:871, :1788)
	TestTrue(TEXT("an unpaired surrogate is sent as U+FFFD"), TArray<uint8>(Repaired.GetPayload()) == Expected);

	const uint8 Raw[] = { 0, 1, 2, 255 };
	const FVaRestSocketFrame Bytes = FVaRestSocketFrame::Binary(Raw);
	TestTrue(TEXT("a binary frame is Binary"), Bytes.Kind == EVaRestSocketFrameKind::Binary);
	TestTrue(TEXT("... with its bytes"), TArray<uint8>(Bytes.GetPayload()) == TArray<uint8>(Raw, UE_ARRAY_COUNT(Raw)));

	// SW-4: an encoder's own buffer, a larger gap before the payload
	TArray<uint8> Own;
	Own.Init(0xAA, 40);
	Own.Add('x');
	Own.Add('y');
	TOptional<FVaRestSocketFrame> Gap = FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Own), 40, 2);
	if (!TestTrue(TEXT("an offset past the headroom is taken"), Gap.IsSet()))
	{
		return false;
	}
	TestEqualSensitive(TEXT("... and the payload is what follows it"), PayloadText(Gap.GetValue()), FString(TEXT("xy")));
	TArray<uint8> Short;
	Short.SetNumZeroed(20);
	TestFalse(TEXT("an offset below the headroom is refused"), FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Short), 8, 4).IsSet());
	TArray<uint8> Over;
	Over.SetNumZeroed(20);
	TestFalse(TEXT("a length past the storage is refused"), FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Over), 16, 5).IsSet());
	TestFalse(TEXT("a default frame has no storage and is not valid"), FVaRestSocketFrame().IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesCloseCodesTest, "VaRest.WebSocket.Types.CloseCodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesCloseCodesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	// RFC 6455 7.4.1-7.4.2 and the IANA registry; spec 3.5 and 2.5
	for (const int32 Code : { 1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 3000, 4000, 4999 })
	{
		TestTrue(FString::Printf(TEXT("%d may be sent and received"), Code), IsValidCloseCode(Code));
	}
	for (const int32 Code : { -1, 0, 999, 1004, 1005, 1006, 1015, 1016, 2000, 2999, 5000, 65535 })
	{
		TestFalse(FString::Printf(TEXT("%d may not"), Code), IsValidCloseCode(Code));
	}
	bool bReplaced = true;
	TestEqual(TEXT("4001 is sent as given"), SanitizeCloseCode(4001, bReplaced), 4001);
	TestFalse(TEXT("... unchanged"), bReplaced);
	TestEqual(TEXT("1006 becomes 1000 (spec 3.5)"), SanitizeCloseCode(1006, bReplaced), 1000);
	TestTrue(TEXT("... replaced"), bReplaced);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesCloseReasonTest, "VaRest.WebSocket.Types.CloseReason",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesCloseReasonTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTypesTestPrivate;
	bool bTruncated = true;
	TestEqual(TEXT("a short reason is kept"), CloseReasonBytes(TEXT("bye"), bTruncated).Num(), 3);
	TestFalse(TEXT("... whole"), bTruncated);
	TestEqual(TEXT("200 ASCII characters are cut to 123 bytes"), CloseReasonBytes(FString::ChrN(200, TEXT('a')), bTruncated).Num(), MaxCloseReasonBytes);
	TestTrue(TEXT("... cut"), bTruncated);
	// Review spec I-1: a reason that fits is never cut, whatever its length in UTF-16 units
	TestEqual(TEXT("100 ASCII characters stay 100 bytes"), CloseReasonBytes(FString::ChrN(100, TEXT('a')), bTruncated).Num(), 100);
	TestFalse(TEXT("... and are not reported cut"), bTruncated);
	TestEqual(TEXT("123 ASCII characters stay 123 bytes"), CloseReasonBytes(FString::ChrN(MaxCloseReasonBytes, TEXT('a')), bTruncated).Num(), MaxCloseReasonBytes);
	TestFalse(TEXT("... and are not reported cut"), bTruncated);
	TestEqual(TEXT("124 are cut to 123"), CloseReasonBytes(FString::ChrN(MaxCloseReasonBytes + 1, TEXT('a')), bTruncated).Num(), MaxCloseReasonBytes);
	TestTrue(TEXT("... and reported cut"), bTruncated);
	// 70 two-byte letters, 140 bytes: 123 would split the 62nd, so 61 (122 bytes) are kept
	const TArray<uint8> Cyrillic = CloseReasonBytes(FString::ChrN(70, static_cast<TCHAR>(0x0430)), bTruncated);
	TestEqual(TEXT("a cut never splits a code point"), Cyrillic.Num(), 122);
	TestTrue(TEXT("... so the reason stays UTF-8"), IsUtf8(Cyrillic));
	TestTrue(TEXT("... and is reported cut"), bTruncated);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTypesPeerCloseTest, "VaRest.WebSocket.Types.PeerClose",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTypesPeerCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	// Spec 2.5: length 0 -> 1005 (the engine reads two bytes anyway, E27)
	const FPeerClose Empty = ParsePeerClose(TConstArrayView<uint8>());
	TestEqual(TEXT("an empty close frame is 1005"), Empty.Code, 1005);
	TestEqualSensitive(TEXT("... with no reason"), Empty.Reason, FString());
	const uint8 One[] = { 0x03 };
	const FPeerClose OneByte = ParsePeerClose(One);
	TestEqual(TEXT("a one-byte close frame is 1002"), OneByte.Code, 1002);
	TestTrue(TEXT("... malformed"), OneByte.bMalformed);
	const uint8 Normal[] = { 0x03, 0xE8 };
	TestEqual(TEXT("03 E8 is 1000"), ParsePeerClose(Normal).Code, 1000);
	const uint8 Big[] = { 0x13, 0x88 };
	const FPeerClose Five = ParsePeerClose(Big);
	TestEqual(TEXT("5000, a code a peer must not send, becomes 1005"), Five.Code, 1005);
	TestTrue(TEXT("... normalised"), Five.bCodeNormalised);
	TestEqual(TEXT("... the received code kept"), Five.ReceivedCode, 5000);
	// close.reason_utf8_bytes: 4002 "пока ✓"
	const uint8 Bye[] = { 0x0F, 0xA2, 0xD0, 0xBF, 0xD0, 0xBE, 0xD0, 0xBA, 0xD0, 0xB0, 0x20, 0xE2, 0x9C, 0x93 };
	const FPeerClose Utf8 = ParsePeerClose(Bye);
	TestEqual(TEXT("4002 as sent"), Utf8.Code, 4002);
	TestEqualSensitive(TEXT("... the reason as UTF-8"), Utf8.Reason, FString(TEXT("пока ✓")));
	TestFalse(TEXT("... not repaired"), Utf8.bReasonRepaired);
	const uint8 Bad[] = { 0x03, 0xE8, 0xC3, 0x28 };
	const FPeerClose Repaired = ParsePeerClose(Bad);
	TestEqualSensitive(TEXT("ill-formed reason bytes become U+FFFD"), Repaired.Reason, FString(TEXT("�(")));
	TestTrue(TEXT("... repaired"), Repaired.bReasonRepaired);
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.WebSocket.Types`
Expected: FAIL to compile (`VaRestSocketTypes.h` not found).

- [ ] **Step 3: The header** — create `Source/VaRestWebSocket/Public/VaRestSocketTypes.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestSocketTypes.generated.h"

/** A data message's kind (RFC 6455 5.6). W3's FVaRestSocketMessage carries it to Blueprints: it freezes with W5 */
UENUM(BlueprintType)
enum class EVaRestSocketFrameKind : uint8
{
	Text,
	Binary
};

/**
 * Who ended a connection (WebSocket spec 2.5): the transport knows, so classification never infers it from the code
 * (E14). W2's FVaRestSocketClose::By: it freezes with W5.
 */
UENUM(BlueprintType)
enum class EVaRestSocketCloseBy : uint8
{
	/** The app's Close(), or its last handle released */
	Client,
	/** The server sent the first close frame */
	Server,
	/** A connect failure, an end without a close frame (1006), or VaRest's transport closing for a protocol reason (1007, 1009) */
	Transport,
	/** W1b: no pong and no inbound bytes within PingTimeout, or a stage's idle deadline */
	Heartbeat,
	/** ConnectTimeout */
	Timeout
};

/**
 * Why a connection never opened, or why the transport ended it (WebSocket spec 2.2, 2.3, 3.4, 22 A2). W2 classifies on
 * it (who, code, failure); it freezes with W5.
 */
UENUM(BlueprintType)
enum class EVaRestSocketFailure : uint8
{
	/** A close the app or the server asked for, or a drop (1006): Code and By say which */
	None,
	/** Closed or released before the open */
	Canceled,
	/** The transport shut down (engine exit, a fork) or could not start */
	Shutdown,
	/** Not ws://, wss:// or (not in Shipping) wss+insecure://; no host, a bad port, user info, a fragment, a non-ASCII host */
	BadUrl,
	/** The engine's URL request filter refused it ([Online.HttpManager <scheme>] in the engine ini) */
	NotAllowed,
	/** A header, a subprotocol or the origin VaRest refuses, or headers that do not fit the 32 KiB handshake buffer */
	BadHandshake,
	/** DNS, TCP, a proxy, or a TLS handshake failure other than the certificate check: no HTTP status arrived */
	ConnectFailed,
	/** The server's certificate was refused (the chain, the name or a pinned key); Detail names the X509 error and depth */
	TlsVerification,
	/** ConnectTimeout passed (VaRest's kill), or libwebsockets' own handshake timeout */
	ConnectTimeout,
	/** The upgrade was answered with a redirect, which VaRest never follows (ruling Q2) */
	Redirect,
	/** An HTTP status other than 101 (HttpStatus) */
	UpgradeRefused,
	/** The upgrade went out and the connection ended before any status line (spec 22 A2) */
	NoUpgradeResponse,
	/** A 101 a client must refuse: Sec-WebSocket-Accept, Upgrade or Connection wrong or missing */
	BadUpgradeResponse,
	/** The server selected a subprotocol that was not offered */
	ProtocolMismatch,
	/** An inbound message over MaxMessageBytes: VaRest closed with 1009 */
	MessageTooBig,
	/** A text message that is not UTF-8: VaRest closed with 1007 (RFC 6455 8.1) */
	InvalidUtf8,
	/** W1b: no pong and no inbound bytes within PingTimeout */
	HeartbeatTimeout,
	/** W1b: no inbound data frame within a stage's idle deadline */
	IdleTimeout,
	/** Any other libwebsockets failure */
	Unknown
};

namespace VaRest::WebSocket
{
	/**
	 * Bytes before a frame's payload that the transport writes the frame header into: LWS_PRE, 16 on every target
	 * (libwebsockets.h:4773: _LWS_PAD(4 + 10 + 2) on a 4- or 8-byte pointer); VaRestLwsService.cpp holds it with a static_assert
	 */
	inline constexpr int32 FrameHeadroom = 16;
	/** RFC 6455 5.5: a control frame carries at most 125 bytes, so a close reason at most 123 after its code */
	inline constexpr int32 MaxCloseReasonBytes = 123;
}

/**
 * One outbound message (WebSocket spec 2.4, 22 SW-4): Storage[Offset, Offset + Length) is the payload, and the Offset bytes
 * before it are headroom the transport writes the frame header into, so an encoder can reserve a larger gap and write a
 * variable-length prefix after the body. libwebsockets masks a client payload in place (RFC 6455 5.3) and the transport
 * writes fragment headers into the bytes before each fragment (plan decision 6): a frame is consumed by sending, so move
 * it in. Any thread; a frame is not shared between threads.
 */
struct VARESTWEBSOCKET_API FVaRestSocketFrame
{
	EVaRestSocketFrameKind Kind = EVaRestSocketFrameKind::Text;
	TArray<uint8> Storage;
	int32 Offset = VaRest::WebSocket::FrameHeadroom;
	int32 Length = 0;

	/** UTF-8 text, copied once after FrameHeadroom; not validated (the caller's bytes are the caller's) */
	static FVaRestSocketFrame Text(FUtf8StringView InText);
	/** Text as UTF-8; an unpaired surrogate becomes U+FFFD (VaRest::Json::WriteUtf8). Past 2 GiB: an invalid frame */
	static FVaRestSocketFrame Text(FStringView InText);
	static FVaRestSocketFrame Binary(TConstArrayView<uint8> InBytes);
	/** An encoder's buffer as it is: unset unless InOffset >= FrameHeadroom and InOffset + InLength <= its size */
	static TOptional<FVaRestSocketFrame> FromStorage(EVaRestSocketFrameKind InKind, TArray<uint8>&& InStorage, int32 InOffset, int32 InLength);

	bool IsValid() const
	{
		return Offset >= VaRest::WebSocket::FrameHeadroom && Length >= 0 && static_cast<int64>(Offset) + Length <= Storage.Num();
	}

	TConstArrayView<uint8> GetPayload() const
	{
		return IsValid() ? TConstArrayView<uint8>(Storage.GetData() + Offset, Length) : TConstArrayView<uint8>();
	}
};

/** How a connection ended (WebSocket spec 2.5, 14), carried by its Closed or Error event */
struct FVaRestSocketEnd
{
	/** The close code: the peer's (1005 when it gave none), ours, 1006 for an end without a close frame; 0 when it never opened */
	int32 Code = 0;
	/** The close reason, decoded as UTF-8 (ill-formed bytes as U+FFFD) */
	FString Reason;
	/**
	 * The close handshake ended: the peer's close was echoed, or a close VaRest started ended before CloseTimeout (plan
	 * decision 4: LWS 3.0 raises no callback for the peer's reply, [close.ours_sequence], so an echo and a TCP end look alike)
	 */
	bool bWasClean = false;
	EVaRestSocketCloseBy By = EVaRestSocketCloseBy::Transport;
	EVaRestSocketFailure Failure = EVaRestSocketFailure::None;
	/** UpgradeRefused: the upgrade's HTTP status. 0 elsewhere, a redirect included: LWS 3.0 follows it before any callback sees it */
	int32 HttpStatus = 0;
	/** For logs and messages: libwebsockets' error string, the X509 error, which timeout fired, what VaRest refused */
	FString Detail;
};

enum class EVaRestSocketEventType : uint8
{
	Open,
	Message,
	/** W1b */
	Pong,
	Drained,
	Closed,
	Error
};

/** One item of a connection's ordered event queue (WebSocket spec 2.1, 7.1) */
struct FVaRestSocketEvent
{
	EVaRestSocketEventType Type = EVaRestSocketEventType::Message;
	/** Message */
	EVaRestSocketFrameKind Kind = EVaRestSocketFrameKind::Text;
	/** Message: always true in 2.0 -- the transport queues whole messages (spec 2.1), so a streaming mode needs no format change */
	bool bFinal = true;
	/** Message: the payload, with one byte of slack (Max() > Num()) so FVaRestJson::ParseOwned takes it without a copy (core 2.1) */
	TArray<uint8> Bytes;
	/** Open: the subprotocol the server selected; empty when none */
	FString Protocol;
	/** Open: seconds from the connect to the open; Pong (W1b): the round trip */
	double Seconds = 0.0;
	/** Closed and Error */
	FVaRestSocketEnd End;
	/** FPlatformTime::Seconds() when the transport queued it */
	double Time = 0.0;

	bool IsTerminal() const { return Type == EVaRestSocketEventType::Closed || Type == EVaRestSocketEventType::Error; }
};

namespace VaRest::WebSocket
{
	/** Codes a close frame may carry, sent or received: 1000-1003, 1007-1014, 3000-4999 (RFC 6455 7.4; spec 2.5, 3.5) */
	VARESTWEBSOCKET_API bool IsValidCloseCode(int32 Code);

	/** Spec 3.5: Code when it may be sent, else 1000 (bOutReplaced) */
	VARESTWEBSOCKET_API int32 SanitizeCloseCode(int32 Code, bool& bOutReplaced);

	/** Reason as UTF-8, at most MaxCloseReasonBytes, cut on a code-point boundary (bOutTruncated) */
	VARESTWEBSOCKET_API TArray<uint8> CloseReasonBytes(FStringView Reason, bool& bOutTruncated);

	/** A peer's close, as spec 2.5 reads it */
	struct FPeerClose
	{
		int32 Code = 1005;
		/** The two bytes as received; -1 when the payload had fewer */
		int32 ReceivedCode = -1;
		FString Reason;
		/** One byte: no room for a code (1002) */
		bool bMalformed = false;
		/** A code a peer must not send, reported as 1005 */
		bool bCodeNormalised = false;
		/** The reason was not UTF-8 */
		bool bReasonRepaired = false;
	};

	/** Spec 2.5: 0 bytes -> 1005, 1 byte -> 1002, a code IsValidCloseCode refuses -> 1005, the reason as UTF-8 (ill-formed bytes U+FFFD) */
	VARESTWEBSOCKET_API FPeerClose ParsePeerClose(TConstArrayView<uint8> Payload);
}

VARESTWEBSOCKET_API const TCHAR* LexToString(EVaRestSocketFailure Failure);
VARESTWEBSOCKET_API const TCHAR* LexToString(EVaRestSocketCloseBy By);
VARESTWEBSOCKET_API const TCHAR* LexToString(EVaRestSocketEventType Type);
```

- [ ] **Step 4: The implementation** — create `Source/VaRestWebSocket/Private/VaRestSocketTypes.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTypes.h"

#include "VaRestJsonUtf8.h"

namespace VaRest::WebSocket::VaRestSocketTypesPrivate
{
	/** A frame of Size payload bytes with zeroed headroom; Size must fit an int32 with the headroom */
	FVaRestSocketFrame Allocate(EVaRestSocketFrameKind Kind, int32 Size)
	{
		FVaRestSocketFrame Out;
		Out.Kind = Kind;
		Out.Storage.SetNumUninitialized(VaRest::WebSocket::FrameHeadroom + Size);
		FMemory::Memzero(Out.Storage.GetData(), VaRest::WebSocket::FrameHeadroom);
		Out.Length = Size;
		return Out;
	}

	FString Utf8ToString(const uint8* Data, int32 Num)
	{
		return Num > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Data), Num) : FString();
	}
}

FVaRestSocketFrame FVaRestSocketFrame::Text(FUtf8StringView InText)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	FVaRestSocketFrame Out = Allocate(EVaRestSocketFrameKind::Text, InText.Len());
	if (InText.Len() > 0)
	{
		FMemory::Memcpy(Out.Storage.GetData() + VaRest::WebSocket::FrameHeadroom, InText.GetData(), InText.Len());
	}
	return Out;
}

FVaRestSocketFrame FVaRestSocketFrame::Text(FStringView InText)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	const int64 Bytes = VaRest::Json::Utf8Length(InText);
	if (Bytes > MAX_int32 - VaRest::WebSocket::FrameHeadroom)
	{
		// Past what a TArray holds: an invalid frame, which Send refuses
		FVaRestSocketFrame Invalid;
		Invalid.Length = -1;
		return Invalid;
	}
	FVaRestSocketFrame Out = Allocate(EVaRestSocketFrameKind::Text, static_cast<int32>(Bytes));
	int32 Replaced = 0;
	// Dest is exactly Utf8Length bytes, which WriteUtf8 fills (VaRestJsonUtf8.h)
	const int32 Written = VaRest::Json::WriteUtf8(InText,
		TArrayView<UTF8CHAR>(reinterpret_cast<UTF8CHAR*>(Out.Storage.GetData() + VaRest::WebSocket::FrameHeadroom), static_cast<int32>(Bytes)), Replaced);
	Out.Length = FMath::Max(Written, 0);
	Out.Storage.SetNum(VaRest::WebSocket::FrameHeadroom + Out.Length);
	return Out;
}

FVaRestSocketFrame FVaRestSocketFrame::Binary(TConstArrayView<uint8> InBytes)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	FVaRestSocketFrame Out = Allocate(EVaRestSocketFrameKind::Binary, InBytes.Num());
	if (InBytes.Num() > 0)
	{
		FMemory::Memcpy(Out.Storage.GetData() + VaRest::WebSocket::FrameHeadroom, InBytes.GetData(), InBytes.Num());
	}
	return Out;
}

TOptional<FVaRestSocketFrame> FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind InKind, TArray<uint8>&& InStorage, int32 InOffset, int32 InLength)
{
	if (InOffset < VaRest::WebSocket::FrameHeadroom || InLength < 0 || static_cast<int64>(InOffset) + InLength > InStorage.Num())
	{
		return TOptional<FVaRestSocketFrame>();
	}
	FVaRestSocketFrame Out;
	Out.Kind = InKind;
	Out.Storage = MoveTemp(InStorage);
	Out.Offset = InOffset;
	Out.Length = InLength;
	return Out;
}

bool VaRest::WebSocket::IsValidCloseCode(int32 Code)
{
	// RFC 6455 7.4.1 and 7.4.2 with the IANA registry: 1004-1006 and 1015 are never on the wire, 1016-2999 are reserved,
	// 3000-4999 belong to libraries and applications
	return (Code >= 1000 && Code <= 1003) || (Code >= 1007 && Code <= 1014) || (Code >= 3000 && Code <= 4999);
}

int32 VaRest::WebSocket::SanitizeCloseCode(int32 Code, bool& bOutReplaced)
{
	bOutReplaced = !IsValidCloseCode(Code);
	return bOutReplaced ? 1000 : Code;
}

TArray<uint8> VaRest::WebSocket::CloseReasonBytes(FStringView Reason, bool& bOutTruncated)
{
	// Each UTF-16 unit is at least one UTF-8 byte, so 124 units reach past 123 bytes whenever the reason is longer; a surrogate
	// pair split at unit 124 becomes U+FFFD at byte 123 or later, which the cut below drops (review spec I-1)
	const FStringView Head = Reason.Left(MaxCloseReasonBytes + 1);
	TArray<uint8> Out;
	Out.SetNumUninitialized(static_cast<int32>(VaRest::Json::Utf8Length(Head)));
	int32 Replaced = 0;
	const int32 Written = VaRest::Json::WriteUtf8(Head, TArrayView<UTF8CHAR>(reinterpret_cast<UTF8CHAR*>(Out.GetData()), Out.Num()), Replaced);
	Out.SetNum(FMath::Max(Written, 0));
	// Over the limit only when the converted head is: a reason of more than 124 units is always over it
	bOutTruncated = Out.Num() > MaxCloseReasonBytes;
	if (Out.Num() > MaxCloseReasonBytes)
	{
		// Back to the first byte of the code point the cut falls in: continuation bytes are 10xxxxxx
		int32 Cut = MaxCloseReasonBytes;
		while (Cut > 0 && (Out[Cut] & 0xC0) == 0x80)
		{
			--Cut;
		}
		Out.SetNum(Cut);
	}
	return Out;
}

VaRest::WebSocket::FPeerClose VaRest::WebSocket::ParsePeerClose(TConstArrayView<uint8> Payload)
{
	using namespace VaRest::WebSocket::VaRestSocketTypesPrivate;
	FPeerClose Out;
	if (Payload.Num() == 0)
	{
		// RFC 6455 7.1.5: no status code was received
		Out.Code = 1005;
		return Out;
	}
	if (Payload.Num() == 1)
	{
		Out.Code = 1002;
		Out.bMalformed = true;
		return Out;
	}
	Out.ReceivedCode = (static_cast<int32>(Payload[0]) << 8) | static_cast<int32>(Payload[1]);
	if (IsValidCloseCode(Out.ReceivedCode))
	{
		Out.Code = Out.ReceivedCode;
	}
	else
	{
		Out.Code = 1005;
		Out.bCodeNormalised = true;
	}
	const FUtf8StringView Reason(reinterpret_cast<const UTF8CHAR*>(Payload.GetData() + 2), Payload.Num() - 2);
	if (VaRest::Json::IsValidUtf8(Reason))
	{
		Out.Reason = Utf8ToString(Payload.GetData() + 2, Payload.Num() - 2);
	}
	else
	{
		VaRest::Json::FUtf8Scratch Repaired;
		VaRest::Json::AppendRepairedUtf8(Reason, Repaired);
		Out.Reason = Utf8ToString(reinterpret_cast<const uint8*>(Repaired.GetData()), Repaired.Num());
		Out.bReasonRepaired = true;
	}
	return Out;
}

const TCHAR* LexToString(EVaRestSocketFailure Failure)
{
	switch (Failure)
	{
	case EVaRestSocketFailure::None: return TEXT("None");
	case EVaRestSocketFailure::Canceled: return TEXT("Canceled");
	case EVaRestSocketFailure::Shutdown: return TEXT("Shutdown");
	case EVaRestSocketFailure::BadUrl: return TEXT("BadUrl");
	case EVaRestSocketFailure::NotAllowed: return TEXT("NotAllowed");
	case EVaRestSocketFailure::BadHandshake: return TEXT("BadHandshake");
	case EVaRestSocketFailure::ConnectFailed: return TEXT("ConnectFailed");
	case EVaRestSocketFailure::TlsVerification: return TEXT("TlsVerification");
	case EVaRestSocketFailure::ConnectTimeout: return TEXT("ConnectTimeout");
	case EVaRestSocketFailure::Redirect: return TEXT("Redirect");
	case EVaRestSocketFailure::UpgradeRefused: return TEXT("UpgradeRefused");
	case EVaRestSocketFailure::NoUpgradeResponse: return TEXT("NoUpgradeResponse");
	case EVaRestSocketFailure::BadUpgradeResponse: return TEXT("BadUpgradeResponse");
	case EVaRestSocketFailure::ProtocolMismatch: return TEXT("ProtocolMismatch");
	case EVaRestSocketFailure::MessageTooBig: return TEXT("MessageTooBig");
	case EVaRestSocketFailure::InvalidUtf8: return TEXT("InvalidUtf8");
	case EVaRestSocketFailure::HeartbeatTimeout: return TEXT("HeartbeatTimeout");
	case EVaRestSocketFailure::IdleTimeout: return TEXT("IdleTimeout");
	case EVaRestSocketFailure::Unknown: return TEXT("Unknown");
	default: return TEXT("?");
	}
}

const TCHAR* LexToString(EVaRestSocketCloseBy By)
{
	switch (By)
	{
	case EVaRestSocketCloseBy::Client: return TEXT("Client");
	case EVaRestSocketCloseBy::Server: return TEXT("Server");
	case EVaRestSocketCloseBy::Transport: return TEXT("Transport");
	case EVaRestSocketCloseBy::Heartbeat: return TEXT("Heartbeat");
	case EVaRestSocketCloseBy::Timeout: return TEXT("Timeout");
	default: return TEXT("?");
	}
}

const TCHAR* LexToString(EVaRestSocketEventType Type)
{
	switch (Type)
	{
	case EVaRestSocketEventType::Open: return TEXT("Open");
	case EVaRestSocketEventType::Message: return TEXT("Message");
	case EVaRestSocketEventType::Pong: return TEXT("Pong");
	case EVaRestSocketEventType::Drained: return TEXT("Drained");
	case EVaRestSocketEventType::Closed: return TEXT("Closed");
	case EVaRestSocketEventType::Error: return TEXT("Error");
	default: return TEXT("?");
	}
}
```

- [ ] **Step 5: Run them**

Run: `Tools/run_tests.sh VaRest.WebSocket.Types`
Expected: `run_tests: 4 passed, 0 failed`.

- [ ] **Step 6: Restore-the-bug** (spec §16.5 item 5 at the unit level; the integration half is Task 9) — in `ParsePeerClose`, make the empty payload report what the engine reads there (`Out.Code = 0;` instead of `1005`): `VaRest.WebSocket.Types.PeerClose` FAILS at "an empty close frame is 1005"; restore; PASSES. Then in `CloseReasonBytes` delete the `while` loop: `VaRest.WebSocket.Types.CloseReason` FAILS at "a cut never splits a code point" (123 bytes); restore; PASSES. Then put back the old head, `Reason.Left(64)` (review spec I-1): `Types.CloseReason` FAILS at "100 ASCII characters stay 100 bytes" (64); restore; PASSES.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Public/VaRestSocketTypes.h Source/VaRestWebSocket/Private/VaRestSocketTypes.cpp \
  Source/VaRestWebSocket/Private/Tests/VaRestSocketTypesTest.cpp
git commit -m "ws: W1a task 2 -- frames with a payload offset (SW-4), who closed, failures, ends, events, the close-code and reason rules

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 3: The seam, the event link and the fake transport

`IVaRestSocketTransport` / `IVaRestSocketConnection` (spec §1.3), the per-connection queue the real transport and the fake share (`FSocketLink`, plan decisions 1-2), and `FVaRestFakeSocketTransport` (spec §16.1) for W2-W5's tests.

**Files:**
- Create: `Source/VaRestWebSocket/Public/VaRestSocketTransport.h`, `Source/VaRestWebSocket/Private/VaRestSocketLink.h`, `Source/VaRestWebSocket/Private/VaRestSocketLink.cpp`, `Source/VaRestWebSocket/Public/VaRestFakeSocketTransport.h`, `Source/VaRestWebSocket/Private/VaRestFakeSocketTransport.cpp`
- Test: `Source/VaRestWebSocket/Private/Tests/VaRestSocketFakeTest.cpp`

**Interfaces:**
- Consumes: Task 2's types, `SanitizeCloseCode`, `CloseReasonBytes`.
- Produces:
  - `VaRest::WebSocket::MaxConnectTimeoutSeconds = 60.0`
  - `struct FVaRestSocketConnectRequest { FString Url; TArray<FString> Protocols; TArray<TPair<FString, FString>> Headers; TOptional<FString> Origin; bool bAllowSelfSignedTls = false; double ConnectTimeout = 10.0; double CloseTimeout = 5.0; int64 MaxMessageBytes = 1 MiB; int64 SendHighWater = 256 KiB; FString LogName; }`
  - `class IVaRestSocketConnection { virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) = 0; virtual bool Send(FVaRestSocketFrame&& Frame) = 0; virtual void Close(int32 Code, FStringView Reason) = 0; virtual void Consumed(int64 Bytes) = 0; virtual int64 GetBufferedAmount() const = 0; virtual int64 GetPendingInboundBytes() const = 0; virtual bool HasEnded() const = 0; }`
  - `class IVaRestSocketTransport { virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) = 0; }`
  - `class VaRest::WebSocket::FSocketLink { explicit FSocketLink(TFunction<void()>); void Push(FVaRestSocketEvent&&, bool bNotify = true); void Notify() const; int32 PopAll(TArray<FVaRestSocketEvent>&); void NoteBuffered(int64 HighWater); void ReleaseBuffered(int64 Bytes); std::atomic<int64> BufferedBytes, PendingInboundBytes; std::atomic<bool> bEnded, bReleased, bCloseRequested; }` (producer functions: one thread at a time; `PopAll`: the consumer)
  - `class FVaRestFakeSocketTransport` (`OnConnect`, `NumAttempts`, `GetAttempt`, `GetLast`) and `class FVaRestFakeSocketAttempt` (`Open`, `Message`, `Text`, `Pong`, `ServerClose`, `Drop`, `Fail`, `End`, `Flush`, `bEchoClose`, `GetSent`, `WasCloseRequested`, `GetCloseCode`, `GetCloseReason`, `WasReleased`, `GetConsumed`, `IsOpen`, `HasEnded`), game thread, `WITH_DEV_AUTOMATION_TESTS` only.

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestWebSocket/Private/Tests/VaRestSocketFakeTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestFakeSocketTransport.h"
#include "VaRestHttpTesting.h"

namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate
{
	struct FNotified
	{
		int32 Count = 0;
	};

	FVaRestSocketConnectRequest Request(const TCHAR* Url)
	{
		FVaRestSocketConnectRequest Out;
		Out.Url = Url;
		return Out;
	}

	FString Text(const FVaRestSocketEvent& Event)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Event.Bytes.GetData()), Event.Bytes.Num());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeScriptTest, "VaRest.WebSocket.Fake.Script",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeScriptTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	const TSharedRef<FNotified> Notified = MakeShared<FNotified>();
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(Request(TEXT("ws://fake.test/ws")), [Notified] { ++Notified->Count; });
	if (!TestEqual(TEXT("one attempt"), Fake.NumAttempts(), 1))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the request is kept"), Fake.GetLast().GetRequest().Url, FString(TEXT("ws://fake.test/ws")));
	Fake.GetLast().Open(TEXT("chat.v2"), 0.25).Text(TEXT("hello")).ServerClose(4001, TEXT("bye"));
	TestEqual(TEXT("each script call notified at once"), Notified->Count, 3);
	TestEqual(TEXT("the message counts as pending inbound bytes (spec 2.3)"), Connection->GetPendingInboundBytes(), static_cast<int64>(5));
	TArray<FVaRestSocketEvent> Events;
	TestEqual(TEXT("three events popped"), Connection->PopEvents(Events), 3);
	if (!TestEqual(TEXT("three events"), Events.Num(), 3))
	{
		return false;
	}
	TestTrue(TEXT("Open first, with the protocol"), Events[0].Type == EVaRestSocketEventType::Open && Events[0].Protocol.Equals(TEXT("chat.v2"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("then the message"), Events[1].Type == EVaRestSocketEventType::Message && Text(Events[1]).Equals(TEXT("hello"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("... with a slack byte for ParseOwned"), Events[1].Bytes.Max() > Events[1].Bytes.Num());
	TestTrue(TEXT("then Closed by the server, clean"), Events[2].Type == EVaRestSocketEventType::Closed && Events[2].End.By == EVaRestSocketCloseBy::Server
		&& Events[2].End.Code == 4001 && Events[2].End.bWasClean);
	TestTrue(TEXT("the connection has ended"), Connection->HasEnded());
	Connection->Consumed(5);
	TestEqual(TEXT("Consumed lowers the pending bytes"), Connection->GetPendingInboundBytes(), static_cast<int64>(0));
	TestEqual(TEXT("... and the attempt saw it"), Fake.GetLast().GetConsumed(), static_cast<int64>(5));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeConsumerCallTest, "VaRest.WebSocket.Fake.NeverInsideTheCall",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeConsumerCallTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	const TSharedRef<FNotified> Notified = MakeShared<FNotified>();
	// E7: what Connect (through OnConnect) and Close cause is queued, and notified only from a later game-thread task
	Fake.OnConnect = [](FVaRestFakeSocketAttempt& Attempt) { Attempt.Fail(EVaRestSocketFailure::UpgradeRefused, 403, TEXT("scripted")); };
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Refused = Fake.Connect(Request(TEXT("ws://fake.test/a")), [Notified] { ++Notified->Count; });
	TestEqual(TEXT("not notified inside Connect"), Notified->Count, 0);
	TestTrue(TEXT("notified from a game-thread task"), VaRest::Http::Testing::PumpUntil([Notified] { return Notified->Count == 1; }));
	TArray<FVaRestSocketEvent> Events;
	Refused->PopEvents(Events);
	TestTrue(TEXT("an Error before the open: UpgradeRefused 403, code 0"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Error
		&& Events[0].End.Failure == EVaRestSocketFailure::UpgradeRefused && Events[0].End.HttpStatus == 403 && Events[0].End.Code == 0);

	Fake.OnConnect = nullptr;
	const TSharedRef<FNotified> Closed = MakeShared<FNotified>();
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Pending = Fake.Connect(Request(TEXT("ws://fake.test/b")), [Closed] { ++Closed->Count; });
	Pending->Close(1000, TEXT("never mind"));
	TestEqual(TEXT("a close before the open notifies later"), Closed->Count, 0);
	TestTrue(TEXT("... from a task"), VaRest::Http::Testing::PumpUntil([Closed] { return Closed->Count == 1; }));
	Events.Reset();
	Pending->PopEvents(Events);
	TestTrue(TEXT("a close before the open is Error Canceled, by the client (plan decision 5)"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Error
		&& Events[0].End.Failure == EVaRestSocketFailure::Canceled && Events[0].End.By == EVaRestSocketCloseBy::Client);

	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Open = Fake.Connect(Request(TEXT("ws://fake.test/c")));
	Fake.GetLast().Open();
	Open->Close(1006, TEXT("bad code"));
	TestTrue(TEXT("the close is recorded as it would be sent: 1006 becomes 1000"), Fake.GetLast().WasCloseRequested() && Fake.GetLast().GetCloseCode() == 1000);
	VaRest::Http::Testing::PumpUntil([Open] { return Open->HasEnded(); });
	Events.Reset();
	Open->PopEvents(Events);
	TestTrue(TEXT("an open attempt's close is echoed: Closed by the client, clean, 1000"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Closed
		&& Events[1].End.By == EVaRestSocketCloseBy::Client && Events[1].End.Code == 1000 && Events[1].End.bWasClean);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeBufferedTest, "VaRest.WebSocket.Fake.Buffered",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeBufferedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	FVaRestSocketConnectRequest Req = Request(TEXT("ws://fake.test/d"));
	Req.SendHighWater = 8;
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(MoveTemp(Req));
	Fake.GetLast().Open();
	TestTrue(TEXT("a send is accepted"), Connection->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("0123456789")))));
	TestEqual(TEXT("... and buffered"), Connection->GetBufferedAmount(), static_cast<int64>(10));
	TestEqual(TEXT("... and recorded"), Fake.GetLast().GetSent().Num(), 1);
	Fake.GetLast().Flush(4);
	TestEqual(TEXT("a partial flush leaves the rest"), Connection->GetBufferedAmount(), static_cast<int64>(6));
	Fake.GetLast().Flush();
	TestEqual(TEXT("the full flush empties it"), Connection->GetBufferedAmount(), static_cast<int64>(0));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(TEXT("Drained once, after the high-water mark was reached (spec 2.4)"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Drained);
	TestFalse(TEXT("an invalid frame is refused"), Connection->Send(FVaRestSocketFrame()));
	Fake.GetLast().Drop();
	TestFalse(TEXT("a send after the end is refused"), Connection->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("late")))));
	// Review conc M-6: after Close, before the end, a send is refused at the call
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Closing = Fake.Connect(Request(TEXT("ws://fake.test/c")));
	Fake.GetLast().bEchoClose = false;
	Fake.GetLast().Open();
	Closing->Close(1000, TEXT("bye"));
	TestFalse(TEXT("a send after Close is refused"), Closing->Send(FVaRestSocketFrame::Text(FUtf8StringView(UTF8TEXT("after close")))));
	TestEqual(TEXT("... and not recorded"), Fake.GetLast().GetSent().Num(), 0);
	Fake.GetLast().ServerClose(1000);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketFakeTerminalTest, "VaRest.WebSocket.Fake.TerminalOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketFakeTerminalTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::VaRestSocketFakeTestPrivate;
	FVaRestFakeSocketTransport Fake;
	TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Fake.Connect(Request(TEXT("ws://fake.test/e")));
	Fake.GetLast().Open().Drop();
	// Plan decision 2: nothing after the terminal event
	AddExpectedErrorPlain(TEXT("after the connection's terminal event was dropped"), EAutomationExpectedErrorFlags::Contains, 1);
	Fake.GetLast().Text(TEXT("after the end"));
	TArray<FVaRestSocketEvent> Events;
	Connection->PopEvents(Events);
	TestTrue(TEXT("Open and Closed only"), Events.Num() == 2 && Events[1].Type == EVaRestSocketEventType::Closed && Events[1].End.Code == 1006);
	FVaRestFakeSocketAttempt& Attempt = Fake.GetLast();
	Connection.Reset();
	TestTrue(TEXT("dropping the handle releases the attempt"), Attempt.WasReleased());
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.WebSocket.Fake`
Expected: FAIL to compile (`VaRestFakeSocketTransport.h` not found).

- [ ] **Step 3: The seam** — create `Source/VaRestWebSocket/Public/VaRestSocketTransport.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
#include "VaRestSocketTypes.h"

namespace VaRest::WebSocket
{
	/** The largest ConnectTimeout VaRest accepts: the LWS context's timeout_secs, the backstop of every handshake stage (spec 2.2) */
	inline constexpr double MaxConnectTimeoutSeconds = 60.0;
}

/**
 * One connection attempt (WebSocket spec 2.2; D3: one LWS connection per attempt, its headers fresh each time). The socket
 * layer (W2) fills it from its configuration on the game thread; W1b adds the ping, flow-control and auto-reply fields.
 */
struct FVaRestSocketConnectRequest
{
	/** ws://, wss://, or (not in Shipping) wss+insecure://. http(s):// is the socket layer's sugar (spec 3.2), not the transport's */
	FString Url;
	/** Offered subprotocols: RFC 7230 tokens, each once. The connection binds to VaRest's handler whatever the server selects (T3, T17) */
	TArray<FString> Protocols;
	/** Appended to the upgrade (spec 2.2, D23): names RFC 7230 tokens, values VCHAR, space or tab; never Host, Upgrade, Connection, Origin or Sec-WebSocket-* */
	TArray<TPair<FString, FString>> Headers;
	/** Unset: derived from the URL (https:// for wss, http:// for ws, the host and a non-default port; D29) and sent as given (T4). Set: as given. Set to "": no Origin */
	TOptional<FString> Origin;
	/** Not in Shipping: what wss+insecure:// does for a wss:// URL -- a self-signed chain allowed, the host name not checked (spec 2.2, 15) */
	bool bAllowSelfSignedTls = false;
	/** Seconds from the connect to the open, measured on the service thread (spec 2.2, D32); above MaxConnectTimeoutSeconds it is clamped, with a Warning */
	double ConnectTimeout = 10.0;
	/** Seconds a close VaRest started may take before the connection is killed; LWS's own close-ack wait is a fixed 5 s (T20), so a longer value acts as about 5 s */
	double CloseTimeout = 5.0;
	/** An inbound message over this closes the connection with 1009 (spec 2.3, 15) */
	int64 MaxMessageBytes = 1024 * 1024;
	/** Once the buffered amount has reached this, its next return to zero is reported as Drained (spec 2.4, 6.3) */
	int64 SendHighWater = 256 * 1024;
	/** Log lines name the connection by this (W2: the socket's name and attempt); empty: "ws#<id>" */
	FString LogName;
};

/**
 * One attempt's connection (WebSocket spec 1.3). Its events arrive in one ordered queue (spec 2.1, 7.1): Open first when it
 * opens, then Message and Drained (and W1b's Pong) in wire order, then exactly one terminal event -- Error when it never
 * opened, Closed when it did (plan decision 2). Destroying the last reference releases it (any thread): an open connection
 * is closed with 1001 "released" (spec 1.4), a pending one is cancelled, and its remaining events are discarded.
 */
class IVaRestSocketConnection
{
public:
	virtual ~IVaRestSocketConnection() = default;

	/** The consumer's thread (one at a time): appends the queued events to Out, in order; returns how many */
	virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) = 0;

	/**
	 * Game thread: queues a message (spec 2.4); false when the frame is invalid, the connection has ended, or Close was called
	 * on it (review conc M-6: the refusal is synchronous, never a silent drop). A send before the open goes out after it.
	 */
	virtual bool Send(FVaRestSocketFrame&& Frame) = 0;

	/**
	 * Game thread: our close (spec 3.5: a code outside 1000-1003, 1007-1014, 3000-4999 becomes 1000 and the reason is cut to
	 * 123 bytes, each with a Warning); it goes after the data already queued. Before the open it cancels the attempt.
	 */
	virtual void Close(int32 Code, FStringView Reason) = 0;

	/** The consumer's thread: Bytes of Message events it has delivered (spec 2.3: flow control counts what it has not) */
	virtual void Consumed(int64 Bytes) = 0;

	/** Any thread: bytes Send accepted that the socket has not taken yet, exact to within one 64 KiB fragment (spec 2.4, 6.3; plan decision 7) */
	virtual int64 GetBufferedAmount() const = 0;

	/** Any thread: bytes of queued Message events not yet Consumed */
	virtual int64 GetPendingInboundBytes() const = 0;

	/** Any thread: true once the terminal event is queued */
	virtual bool HasEnded() const = 0;
};

/** The seam (WebSocket spec 1.3): VaRest's libwebsockets transport ships (spec 2); tests use FVaRestFakeSocketTransport */
class IVaRestSocketTransport
{
public:
	virtual ~IVaRestSocketTransport() = default;

	/**
	 * Game thread: starts one attempt. Never calls back inside: a refusal -- a bad URL, the allowlist, a header -- is an Error
	 * event queued like any other (E7). OnEvents, when set, runs after events are queued, on the thread that queued them: the
	 * service thread, or the game thread -- synchronously inside the transport's Shutdown (each connection's Shutdown end),
	 * and from a game-thread task for a refusal or a transport that could not start. It only schedules work and never calls
	 * the connection's game-thread functions. It is destroyed on whichever thread drops the connection's state last (often the
	 * service thread, when LWS destroys the connection), so what it captures must be weak or thread-safe: a strong reference
	 * to the connection's owner makes a cycle (connection -> OnEvents -> owner -> connection) that lasts until the server ends
	 * the connection (review conc M-5).
	 */
	virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) = 0;
};
```

- [ ] **Step 4: The link** — create `Source/VaRestWebSocket/Private/VaRestSocketLink.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "Templates/Function.h"
#include "VaRestSocketTypes.h"

#include <atomic>

namespace VaRest::WebSocket
{
	/**
	 * What a connection's producer (the service thread; the fake on the game thread) and its consumer share: the one ordered
	 * event queue (WebSocket spec 2.1, SPSC), the buffered amount (spec 2.4) and the undelivered inbound bytes (spec 2.3).
	 * The producer queues exactly one terminal event (Closed or Error, plan decision 2); a later event is dropped with an
	 * Error log line. Producer functions run on one thread at a time; PopAll on the consumer's.
	 */
	class FSocketLink final
	{
	public:
		explicit FSocketLink(TFunction<void()> InOnEvents);

		/** Producer: queues Event, stamps its Time, counts a Message's bytes; bNotify false leaves the notification to a later Notify() */
		void Push(FVaRestSocketEvent&& Event, bool bNotify = true);

		/** The thread that queued: runs the consumer's OnEvents, if any */
		void Notify() const;

		/** Consumer: appends the queued events to Out; returns how many */
		int32 PopAll(TArray<FVaRestSocketEvent>& Out);

		/** Producer, after a Send's bytes were added to BufferedBytes: remembers that the high-water mark was reached */
		void NoteBuffered(int64 HighWater);

		/** Producer: Bytes left the buffer (on the socket now); queues Drained when it reaches 0 after the high-water mark (spec 2.4) */
		void ReleaseBuffered(int64 Bytes);

		std::atomic<int64> BufferedBytes{ 0 };
		std::atomic<int64> PendingInboundBytes{ 0 };
		/** Set once the terminal event is in the queue */
		std::atomic<bool> bEnded{ false };
		/** The consumer dropped its handle: Push discards from then on */
		std::atomic<bool> bReleased{ false };
		/** The consumer called Close: its later sends are refused at the call (review conc M-6) */
		std::atomic<bool> bCloseRequested{ false };

	private:
		TQueue<FVaRestSocketEvent, EQueueMode::Spsc> Queue;
		const TFunction<void()> OnEvents;
		/** Producer only */
		bool bTerminalQueued = false;
		bool bHighWaterReached = false;
	};
}
```

Create `Source/VaRestWebSocket/Private/VaRestSocketLink.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketLink.h"

#include "HAL/PlatformTime.h"
#include "VaRestWebSocketLog.h"

VaRest::WebSocket::FSocketLink::FSocketLink(TFunction<void()> InOnEvents)
	: OnEvents(MoveTemp(InOnEvents))
{
}

void VaRest::WebSocket::FSocketLink::Push(FVaRestSocketEvent&& Event, bool bNotify)
{
	if (bTerminalQueued)
	{
		// Plan decision 2, made loud: a producer that queues after the end has a bug
		UE_LOG(LogVaRestSocket, Error, TEXT("an event (%s) after the connection's terminal event was dropped: a transport bug"), LexToString(Event.Type));
		return;
	}
	const bool bTerminal = Event.IsTerminal();
	bTerminalQueued = bTerminal;
	if (bReleased.load())
	{
		bEnded = bEnded.load() || bTerminal;
		return;
	}
	if (Event.Type == EVaRestSocketEventType::Message)
	{
		if (Event.Bytes.Max() == Event.Bytes.Num())
		{
			// The slack byte FVaRestJson::ParseOwned wants (core 2.1)
			Event.Bytes.Reserve(Event.Bytes.Num() + 1);
		}
		PendingInboundBytes.fetch_add(Event.Bytes.Num());
	}
	Event.Time = FPlatformTime::Seconds();
	Queue.Enqueue(MoveTemp(Event));
	if (bTerminal)
	{
		// After the enqueue: HasEnded() true means the terminal event can be popped
		bEnded = true;
	}
	if (bNotify)
	{
		Notify();
	}
}

void VaRest::WebSocket::FSocketLink::Notify() const
{
	if (OnEvents)
	{
		OnEvents();
	}
}

int32 VaRest::WebSocket::FSocketLink::PopAll(TArray<FVaRestSocketEvent>& Out)
{
	int32 Popped = 0;
	FVaRestSocketEvent Event;
	while (Queue.Dequeue(Event))
	{
		Out.Add(MoveTemp(Event));
		++Popped;
	}
	return Popped;
}

void VaRest::WebSocket::FSocketLink::NoteBuffered(int64 HighWater)
{
	if (BufferedBytes.load() >= HighWater)
	{
		bHighWaterReached = true;
	}
}

void VaRest::WebSocket::FSocketLink::ReleaseBuffered(int64 Bytes)
{
	const int64 Left = BufferedBytes.fetch_sub(Bytes) - Bytes;
	if (Left == 0 && bHighWaterReached && !bTerminalQueued)
	{
		bHighWaterReached = false;
		FVaRestSocketEvent Drained;
		Drained.Type = EVaRestSocketEventType::Drained;
		Push(MoveTemp(Drained));
	}
}
```

- [ ] **Step 5: The fake** — create `Source/VaRestWebSocket/Public/VaRestFakeSocketTransport.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketTransport.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

namespace VaRest::WebSocket
{
	class FSocketLink;
}

class FVaRestFakeSocketConnection;

/**
 * One attempt at FVaRestFakeSocketTransport (WebSocket spec 16.1): what the consumer asked -- the request, its sends, its
 * close, what it consumed -- and the server's side, which the test scripts on the game thread at any time: from the test
 * body, from FVaRestFakeSocketTransport::OnConnect, or from inside the consumer's own handlers. The events go into the same
 * ordered queue the real transport fills. A script call notifies at once; one made inside a consumer's call into the
 * connection (Connect, Send, Close) notifies from a later game-thread task, so the fake never calls back inside the
 * consumer's call (E7). It does not check the request (the socket layer's own tests do).
 */
class VARESTWEBSOCKET_API FVaRestFakeSocketAttempt final
{
public:
	FVaRestFakeSocketAttempt(int32 InIndex, FVaRestSocketConnectRequest&& InRequest, TFunction<void()> InOnEvents);

	int32 GetIndex() const { return Index; }
	const FVaRestSocketConnectRequest& GetRequest() const { return Request; }

	// ---- the server's side (game thread); after the terminal event each one is dropped, with an Error log line
	FVaRestFakeSocketAttempt& Open(const FString& InProtocol = FString(), double ConnectSeconds = 0.0);
	FVaRestFakeSocketAttempt& Message(EVaRestSocketFrameKind InKind, TConstArrayView<uint8> InBytes);
	FVaRestFakeSocketAttempt& Text(FStringView InText);
	FVaRestFakeSocketAttempt& Pong(double RttSeconds);
	/** Closed: By Server, clean */
	FVaRestFakeSocketAttempt& ServerClose(int32 Code, const FString& Reason = FString());
	/** Closed: 1006, By Transport, not clean */
	FVaRestFakeSocketAttempt& Drop();
	/** Error before the open: By Timeout for ConnectTimeout, else Transport */
	FVaRestFakeSocketAttempt& Fail(EVaRestSocketFailure Failure, int32 HttpStatus = 0, const FString& Detail = FString());
	/** Any terminal event (Type Closed or Error) */
	FVaRestFakeSocketAttempt& End(EVaRestSocketEventType Type, const FVaRestSocketEnd& InEnd);
	/** Bytes of the consumer's sends (all, by default) reach "the socket": the buffered amount falls, Drained per spec 2.4 */
	FVaRestFakeSocketAttempt& Flush(int64 Bytes = MAX_int64);

	/** true (default): the consumer's Close on an open attempt is answered at once -- Closed, By Client, clean, its code and reason. false: the test ends it */
	bool bEchoClose = true;

	// ---- what the consumer did
	const TArray<FVaRestSocketFrame>& GetSent() const { return Sent; }
	bool WasCloseRequested() const { return bCloseRequested; }
	/** The close as the real transport sends it: the code sanitised, the reason cut (spec 3.5) */
	int32 GetCloseCode() const { return CloseCode; }
	const FString& GetCloseReason() const { return CloseReason; }
	/** Any thread: release and Consumed may come from the consumer's thread (W3's pipe), so both are atomic (review conc M-4) */
	bool WasReleased() const { return bReleased.load(); }
	int64 GetConsumed() const { return ConsumedBytes.load(); }
	bool IsOpen() const { return bOpen && !HasEnded(); }
	bool HasEnded() const;

private:
	friend class FVaRestFakeSocketTransport;
	friend class FVaRestFakeSocketConnection;

	void Push(FVaRestSocketEvent&& Event);
	bool OnSend(FVaRestSocketFrame&& Frame);
	void OnClose(int32 Code, FStringView Reason);
	void OnConsumed(int64 Bytes);
	void OnReleased();

	const int32 Index;
	FVaRestSocketConnectRequest Request;
	TSharedRef<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe> Link;
	/** > 0 while the consumer is inside one of its calls into the connection (or inside Connect) */
	int32 ConsumerDepth = 0;
	bool bOpen = false;
	bool bCloseRequested = false;
	int32 CloseCode = 0;
	FString CloseReason;
	std::atomic<bool> bReleased{ false };
	std::atomic<int64> ConsumedBytes{ 0 };
	TArray<FVaRestSocketFrame> Sent;
};

/** The fake transport of WebSocket spec 16.1, for the socket layer's tests (W2-W5). Game thread. Not for games. */
class VARESTWEBSOCKET_API FVaRestFakeSocketTransport final : public IVaRestSocketTransport
{
public:
	virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) override;

	/** Runs for each attempt as Connect starts it: script it there (Open, Fail, ...) or later through GetAttempt */
	TFunction<void(FVaRestFakeSocketAttempt&)> OnConnect;

	int32 NumAttempts() const { return Attempts.Num(); }
	FVaRestFakeSocketAttempt& GetAttempt(int32 InIndex) const { return *Attempts[InIndex]; }
	FVaRestFakeSocketAttempt& GetLast() const { return *Attempts.Last(); }

private:
	TArray<TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe>> Attempts;
};

#endif // WITH_DEV_AUTOMATION_TESTS
```

Create `Source/VaRestWebSocket/Private/VaRestFakeSocketTransport.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestFakeSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/Async.h"
#include "VaRestSocketLink.h"
#include "VaRestWebSocketLog.h"

/** The consumer's handle on a fake attempt; its destruction is the release */
class FVaRestFakeSocketConnection final : public IVaRestSocketConnection
{
public:
	explicit FVaRestFakeSocketConnection(TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe> InAttempt)
		: Attempt(MoveTemp(InAttempt))
	{
	}

	virtual ~FVaRestFakeSocketConnection() override { Attempt->OnReleased(); }

	virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) override { return Attempt->Link->PopAll(Out); }
	virtual bool Send(FVaRestSocketFrame&& Frame) override { return Attempt->OnSend(MoveTemp(Frame)); }
	virtual void Close(int32 Code, FStringView Reason) override { Attempt->OnClose(Code, Reason); }
	virtual void Consumed(int64 Bytes) override { Attempt->OnConsumed(Bytes); }
	virtual int64 GetBufferedAmount() const override { return FMath::Max<int64>(Attempt->Link->BufferedBytes.load(), 0); }
	virtual int64 GetPendingInboundBytes() const override { return Attempt->Link->PendingInboundBytes.load(); }
	virtual bool HasEnded() const override { return Attempt->HasEnded(); }

private:
	TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe> Attempt;
};

FVaRestFakeSocketAttempt::FVaRestFakeSocketAttempt(int32 InIndex, FVaRestSocketConnectRequest&& InRequest, TFunction<void()> InOnEvents)
	: Index(InIndex)
	, Request(MoveTemp(InRequest))
	, Link(MakeShared<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe>(MoveTemp(InOnEvents)))
{
}

bool FVaRestFakeSocketAttempt::HasEnded() const
{
	return Link->bEnded.load();
}

void FVaRestFakeSocketAttempt::Push(FVaRestSocketEvent&& Event)
{
	check(IsInGameThread());
	const bool bInsideConsumer = ConsumerDepth > 0;
	Link->Push(MoveTemp(Event), !bInsideConsumer);
	if (bInsideConsumer)
	{
		// E7: never inside the consumer's call; a game-thread task notifies (PumpUntil and the engine's tick run it)
		AsyncTask(ENamedThreads::GameThread, [Weak = TWeakPtr<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe>(Link)]
		{
			if (const TSharedPtr<VaRest::WebSocket::FSocketLink, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Notify();
			}
		});
	}
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Open(const FString& InProtocol, double ConnectSeconds)
{
	bOpen = bOpen || !HasEnded();
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Open;
	Event.Protocol = InProtocol;
	Event.Seconds = ConnectSeconds;
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Message(EVaRestSocketFrameKind InKind, TConstArrayView<uint8> InBytes)
{
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Message;
	Event.Kind = InKind;
	Event.Bytes.Reserve(InBytes.Num() + 1);
	Event.Bytes.Append(InBytes.GetData(), InBytes.Num());
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Text(FStringView InText)
{
	const FTCHARToUTF8 Converted(InText.GetData(), InText.Len());
	return Message(EVaRestSocketFrameKind::Text, TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length()));
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Pong(double RttSeconds)
{
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Pong;
	Event.Seconds = RttSeconds;
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::ServerClose(int32 Code, const FString& Reason)
{
	FVaRestSocketEnd Ended;
	Ended.Code = Code;
	Ended.Reason = Reason;
	Ended.bWasClean = true;
	Ended.By = EVaRestSocketCloseBy::Server;
	return End(EVaRestSocketEventType::Closed, Ended);
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Drop()
{
	FVaRestSocketEnd Ended;
	Ended.Code = 1006;
	Ended.By = EVaRestSocketCloseBy::Transport;
	return End(EVaRestSocketEventType::Closed, Ended);
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Fail(EVaRestSocketFailure Failure, int32 HttpStatus, const FString& Detail)
{
	FVaRestSocketEnd Ended;
	Ended.By = Failure == EVaRestSocketFailure::ConnectTimeout ? EVaRestSocketCloseBy::Timeout : EVaRestSocketCloseBy::Transport;
	Ended.Failure = Failure;
	Ended.HttpStatus = HttpStatus;
	Ended.Detail = Detail;
	return End(EVaRestSocketEventType::Error, Ended);
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::End(EVaRestSocketEventType Type, const FVaRestSocketEnd& InEnd)
{
	if (Type != EVaRestSocketEventType::Closed && Type != EVaRestSocketEventType::Error)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("FVaRestFakeSocketAttempt::End takes Closed or Error, not %s"), LexToString(Type));
		return *this;
	}
	FVaRestSocketEvent Event;
	Event.Type = Type;
	Event.End = InEnd;
	Push(MoveTemp(Event));
	return *this;
}

FVaRestFakeSocketAttempt& FVaRestFakeSocketAttempt::Flush(int64 Bytes)
{
	const int64 Take = FMath::Min(Bytes, Link->BufferedBytes.load());
	if (Take > 0)
	{
		Link->ReleaseBuffered(Take);
	}
	return *this;
}

bool FVaRestFakeSocketAttempt::OnSend(FVaRestSocketFrame&& Frame)
{
	check(IsInGameThread());
	// As the real handle: refused after the end and after the consumer's Close (review conc M-6)
	if (!Frame.IsValid() || HasEnded() || bCloseRequested)
	{
		return false;
	}
	++ConsumerDepth;
	Link->BufferedBytes.fetch_add(Frame.Length);
	Link->NoteBuffered(Request.SendHighWater);
	Sent.Add(MoveTemp(Frame));
	--ConsumerDepth;
	return true;
}

void FVaRestFakeSocketAttempt::OnClose(int32 Code, FStringView Reason)
{
	check(IsInGameThread());
	if (HasEnded() || bCloseRequested)
	{
		return;
	}
	++ConsumerDepth;
	bool bReplaced = false;
	bool bTruncated = false;
	CloseCode = VaRest::WebSocket::SanitizeCloseCode(Code, bReplaced);
	const TArray<uint8> Bytes = VaRest::WebSocket::CloseReasonBytes(Reason, bTruncated);
	CloseReason = Bytes.Num() > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()) : FString();
	bCloseRequested = true;
	if (!bOpen)
	{
		FVaRestSocketEnd Ended;
		Ended.By = EVaRestSocketCloseBy::Client;
		Ended.Failure = EVaRestSocketFailure::Canceled;
		Ended.Detail = TEXT("closed before the open");
		End(EVaRestSocketEventType::Error, Ended);
	}
	else if (bEchoClose)
	{
		FVaRestSocketEnd Ended;
		Ended.Code = CloseCode;
		Ended.Reason = CloseReason;
		Ended.bWasClean = true;
		Ended.By = EVaRestSocketCloseBy::Client;
		End(EVaRestSocketEventType::Closed, Ended);
	}
	--ConsumerDepth;
}

void FVaRestFakeSocketAttempt::OnConsumed(int64 Bytes)
{
	ConsumedBytes.fetch_add(Bytes);
	Link->PendingInboundBytes.fetch_sub(Bytes);
}

void FVaRestFakeSocketAttempt::OnReleased()
{
	bReleased = true;
	Link->bReleased = true;
}

TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> FVaRestFakeSocketTransport::Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents)
{
	check(IsInGameThread());
	const TSharedRef<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe> Attempt = MakeShared<FVaRestFakeSocketAttempt, ESPMode::ThreadSafe>(Attempts.Num(), MoveTemp(Request), MoveTemp(OnEvents));
	Attempts.Add(Attempt);
	if (OnConnect)
	{
		// Inside Connect: what OnConnect scripts is notified later (E7)
		++Attempt->ConsumerDepth;
		OnConnect(*Attempt);
		--Attempt->ConsumerDepth;
	}
	return MakeShared<FVaRestFakeSocketConnection, ESPMode::ThreadSafe>(Attempt);
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 6: Run them**

Run: `Tools/run_tests.sh VaRest.WebSocket.Fake`
Expected: `run_tests: 4 passed, 0 failed`.

- [ ] **Step 7: Restore-the-bug** — in `FVaRestFakeSocketAttempt::Push`, notify at once (`Link->Push(MoveTemp(Event), true);` and no task): `VaRest.WebSocket.Fake.NeverInsideTheCall` FAILS at "not notified inside Connect" (E7); restore; PASSES. Then in `FSocketLink::Push` delete the `if (bTerminalQueued)` block: `VaRest.WebSocket.Fake.TerminalOnce` FAILS (three events; the expected Error line never comes); restore; PASSES. Then drop `|| bCloseRequested` from `FVaRestFakeSocketAttempt::OnSend`: `VaRest.WebSocket.Fake.Buffered` FAILS at "a send after Close is refused"; restore; PASSES.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestWebSocket/Public/VaRestSocketTransport.h Source/VaRestWebSocket/Private/VaRestSocketLink.h \
  Source/VaRestWebSocket/Private/VaRestSocketLink.cpp Source/VaRestWebSocket/Public/VaRestFakeSocketTransport.h \
  Source/VaRestWebSocket/Private/VaRestFakeSocketTransport.cpp Source/VaRestWebSocket/Private/Tests/VaRestSocketFakeTest.cpp
git commit -m "ws: W1a task 3 -- IVaRestSocketTransport and IVaRestSocketConnection, the ordered per-connection link, the scriptable fake (spec 16.1)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 4: Connect targets — URL, Host, Origin, subprotocols, headers, the allowlist (pure, game thread)

What a connect request becomes before the service thread sees it (spec §2.2; plan decisions 13-15). Pure and table-tested; Task 7's `FConn::Prepare` calls it on the game thread.

**Files:**
- Create: `Source/VaRestWebSocket/Private/VaRestSocketTarget.h`, `Source/VaRestWebSocket/Private/VaRestSocketTarget.cpp`
- Test: `Source/VaRestWebSocket/Private/Tests/VaRestSocketTargetTest.cpp`

**Interfaces:**
- Consumes: `FVaRestSocketConnectRequest` (Task 3); `VaRest::Http::IsValidHeaderName` (`VaRestUrl.h`, M3).
- Produces:
  - `struct VaRest::WebSocket::FSocketTarget { bool bTls; bool bInsecure; FString Host; bool bHostIsIp; int32 Port; FString HostHeader; FString PathAndQuery; FString Origin; FString Protocols; TArray<uint8> HeaderBlock; FString FilterUrl; }`
  - `struct VaRest::WebSocket::FTargetError { EVaRestSocketFailure Failure; FString Message; }`
  - `VaRest::WebSocket::MaxHeaderBlockBytes = 32 * 1024`
  - `TValueOrError<FSocketTarget, FTargetError> VaRest::WebSocket::PrepareTarget(const FVaRestSocketConnectRequest& Request, bool bShippingRules = UE_BUILD_SHIPPING != 0)` (any thread)
  - `bool VaRest::WebSocket::IsIpv4Literal(FStringView Host)` (any thread)
  - `bool VaRest::WebSocket::IsAllowedByFilter(const FString& FilterUrl, const TArray<TPair<FString, TArray<FString>>>* Allowlist = nullptr)` (game thread when `Allowlist` is null: it reads `GConfig`)

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestWebSocket/Private/Tests/VaRestSocketTargetTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestSocketTarget.h"

namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate
{
	TValueOrError<FSocketTarget, FTargetError> Prepare(const TCHAR* Url, bool bShipping = false)
	{
		FVaRestSocketConnectRequest Request;
		Request.Url = Url;
		return PrepareTarget(Request, bShipping);
	}

	FString BlockText(const FSocketTarget& Target)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Target.HeaderBlock.GetData()), Target.HeaderBlock.Num());
	}

	/**
	 * The value, or null after a failing TestTrue (review spec I-4): GetValue() on an error is a check failure
	 * (ValueOrError.h:156-158, TVariant.h:132-137) that would end the run instead of failing the row. Keep Made alive
	 * while the pointer is used.
	 */
	const FSocketTarget* ValueOf(FAutomationTestBase& Test, const FString& What, const TValueOrError<FSocketTarget, FTargetError>& Made)
	{
		return Test.TestTrue(What, Made.HasValue()) ? &Made.GetValue() : nullptr;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetValidTest, "VaRest.WebSocket.Target.Valid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetValidTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate;
	struct FRow
	{
		const TCHAR* Url;
		const TCHAR* Host;
		int32 Port;
		const TCHAR* HostHeader;
		const TCHAR* Path;
		bool bTls;
		bool bIp;
		const TCHAR* Origin;
		const TCHAR* FilterUrl;
	};
	const FRow Rows[] = {
		{ TEXT("ws://example.com/chat"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/chat"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("wss://example.com"), TEXT("example.com"), 443, TEXT("example.com"), TEXT("/"), true, false, TEXT("https://example.com"), TEXT("wss://example.com") },
		{ TEXT("wss://example.com:8443/a?b=c"), TEXT("example.com"), 8443, TEXT("example.com:8443"), TEXT("/a?b=c"), true, false, TEXT("https://example.com:8443"), TEXT("wss://example.com:8443") },
		{ TEXT("ws://127.0.0.1:41234/ws/echo?key=K"), TEXT("127.0.0.1"), 41234, TEXT("127.0.0.1:41234"), TEXT("/ws/echo?key=K"), false, true, TEXT("http://127.0.0.1:41234"), TEXT("ws://127.0.0.1:41234") },
		{ TEXT("ws://[::1]:8080/x"), TEXT("::1"), 8080, TEXT("[::1]:8080"), TEXT("/x"), false, true, TEXT("http://[::1]:8080"), TEXT("ws://[::1]:8080") },
		{ TEXT("WS://Example.com:80/"), TEXT("Example.com"), 80, TEXT("Example.com"), TEXT("/"), false, false, TEXT("http://Example.com"), TEXT("ws://Example.com") },
		{ TEXT("ws://example.com?x=1"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/?x=1"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("ws://example.com/\u043F?q=a b"), TEXT("example.com"), 80, TEXT("example.com"), TEXT("/%D0%BF?q=a%20b"), false, false, TEXT("http://example.com"), TEXT("ws://example.com") },
		{ TEXT("wss+insecure://127.0.0.1:9/"), TEXT("127.0.0.1"), 9, TEXT("127.0.0.1:9"), TEXT("/"), true, true, TEXT("https://127.0.0.1:9"), TEXT("wss://127.0.0.1:9") },
	};
	for (const FRow& Row : Rows)
	{
		const TValueOrError<FSocketTarget, FTargetError> Made = Prepare(Row.Url);
		if (!TestTrue(FString::Printf(TEXT("%s is taken"), Row.Url), Made.HasValue()))
		{
			continue;
		}
		const FSocketTarget& Target = Made.GetValue();
		TestEqualSensitive(FString::Printf(TEXT("%s: host"), Row.Url), Target.Host, FString(Row.Host));
		TestEqual(FString::Printf(TEXT("%s: port"), Row.Url), Target.Port, Row.Port);
		TestEqualSensitive(FString::Printf(TEXT("%s: Host header"), Row.Url), Target.HostHeader, FString(Row.HostHeader));
		TestEqualSensitive(FString::Printf(TEXT("%s: path and query"), Row.Url), Target.PathAndQuery, FString(Row.Path));
		TestEqual(FString::Printf(TEXT("%s: TLS"), Row.Url), Target.bTls, Row.bTls);
		TestEqual(FString::Printf(TEXT("%s: IP literal"), Row.Url), Target.bHostIsIp, Row.bIp);
		TestEqualSensitive(FString::Printf(TEXT("%s: default Origin (D29)"), Row.Url), Target.Origin, FString(Row.Origin));
		TestEqualSensitive(FString::Printf(TEXT("%s: what the URL filter sees"), Row.Url), Target.FilterUrl, FString(Row.FilterUrl));
	}
	const TValueOrError<FSocketTarget, FTargetError> Insecure = Prepare(TEXT("wss+insecure://127.0.0.1:9/"));
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("wss+insecure is taken"), Insecure))
	{
		TestTrue(TEXT("wss+insecure is insecure outside Shipping"), Target->bInsecure);
	}
	const TValueOrError<FSocketTarget, FTargetError> Checked = Prepare(TEXT("wss://example.com/"));
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("wss is taken"), Checked))
	{
		TestFalse(TEXT("wss is not"), Target->bInsecure);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetInvalidTest, "VaRest.WebSocket.Target.Invalid",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetInvalidTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate;
	const TCHAR* const Refused[] = {
		TEXT("example.com/ws"),             // no scheme
		TEXT("http://example.com/"),        // the socket layer maps http(s), not the transport (spec 3.2)
		TEXT("ftp://example.com/"),
		TEXT("ws://"),                      // no host
		TEXT("ws://:80/"),
		TEXT("ws://user:pw@example.com/"),  // user info
		TEXT("ws://example.com/#frag"),     // a fragment
		TEXT("ws://exa mple.com/"),
		TEXT("ws://\u043F\u0440\u0438\u043C\u0435\u0440.\u0440\u0444/"),   // IDN needs punycode
		TEXT("ws://example.com:0/"),
		TEXT("ws://example.com:65536/"),
		TEXT("ws://example.com:http/"),
		TEXT("ws://example.com:/"),
		TEXT("ws://[::1/"),
		TEXT("ws://[abc]/"),                // brackets hold IPv6 only
		TEXT("ws://[::1]x/"),
	};
	for (const TCHAR* Url : Refused)
	{
		const TValueOrError<FSocketTarget, FTargetError> Made = Prepare(Url);
		TestTrue(FString::Printf(TEXT("%s is refused as BadUrl"), Url), Made.HasError() && Made.GetError().Failure == EVaRestSocketFailure::BadUrl);
	}
	// Spec 2.2, 15: wss+insecure and bAllowSelfSignedTls do nothing in Shipping
	TestTrue(TEXT("wss+insecure is refused under Shipping rules"), Prepare(TEXT("wss+insecure://127.0.0.1:9/"), true).HasError());
	FVaRestSocketConnectRequest SelfSigned;
	SelfSigned.Url = TEXT("wss://127.0.0.1:9/");
	SelfSigned.bAllowSelfSignedTls = true;
	const TValueOrError<FSocketTarget, FTargetError> Development = PrepareTarget(SelfSigned, false);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("bAllowSelfSignedTls is taken outside Shipping"), Development))
	{
		TestTrue(TEXT("bAllowSelfSignedTls makes wss insecure outside Shipping"), Target->bInsecure);
	}
	const TValueOrError<FSocketTarget, FTargetError> Shipping = PrepareTarget(SelfSigned, true);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("bAllowSelfSignedTls is taken under Shipping rules"), Shipping))
	{
		TestFalse(TEXT("... and is ignored under Shipping rules"), Target->bInsecure);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetHandshakeTest, "VaRest.WebSocket.Target.Handshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetHandshakeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::VaRestSocketTargetTestPrivate;
	FVaRestSocketConnectRequest Request;
	Request.Url = TEXT("wss://example.com/");
	// Origin (spec 2.2, D29): given, omitted, refused
	Request.Origin = FString(TEXT("https://game.example"));
	const TValueOrError<FSocketTarget, FTargetError> Given = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("an Origin is taken"), Given))
	{
		TestEqualSensitive(TEXT("an Origin is sent as given"), Target->Origin, FString(TEXT("https://game.example")));
	}
	Request.Origin = FString();
	const TValueOrError<FSocketTarget, FTargetError> Omitted = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("an empty Origin is taken"), Omitted))
	{
		TestTrue(TEXT("an empty Origin omits it"), Target->Origin.IsEmpty());
	}
	Request.Origin = FString(TEXT("https://bad origin"));
	TestTrue(TEXT("an Origin with a space is BadHandshake"), PrepareTarget(Request).HasError() && PrepareTarget(Request).GetError().Failure == EVaRestSocketFailure::BadHandshake);
	Request.Origin.Reset();

	// Subprotocols: tokens, each once, case-sensitive, joined with ", " (plan decision 14)
	Request.Protocols = { TEXT("chat.v2"), TEXT("chat.v1"), TEXT("Chat.v1") };
	const TValueOrError<FSocketTarget, FTargetError> Offered = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("three distinct protocols are taken"), Offered))
	{
		TestEqualSensitive(TEXT("protocols are joined with \", \""), Target->Protocols, FString(TEXT("chat.v2, chat.v1, Chat.v1")));
	}
	Request.Protocols = { TEXT("chat v2") };
	TestTrue(TEXT("a protocol that is not a token is BadHandshake"), PrepareTarget(Request).HasError());
	Request.Protocols = { TEXT("x"), TEXT("x") };
	TestTrue(TEXT("a protocol offered twice is BadHandshake"), PrepareTarget(Request).HasError());
	Request.Protocols.Reset();

	// Headers (spec 2.2, D23)
	Request.Headers = { { TEXT("X-A"), TEXT("1") }, { TEXT("Authorization"), TEXT("Bearer t\tx") } };
	const TValueOrError<FSocketTarget, FTargetError> Lines = PrepareTarget(Request);
	if (const FSocketTarget* Target = ValueOf(*this, TEXT("tab and space in values are taken"), Lines))
	{
		TestEqualSensitive(TEXT("headers become CRLF lines in order"), BlockText(*Target), FString(TEXT("X-A: 1\r\nAuthorization: Bearer t\tx\r\n")));
	}
	const TPair<FString, FString> Bad[] = {
		{ TEXT("Origin"), TEXT("x") },                       // VaRest's own: set Origin
		{ TEXT("sec-websocket-protocol"), TEXT("x") },       // any case
		{ TEXT("Host"), TEXT("x") },
		{ TEXT("X-B"), TEXT("a\r\nX-C: b") },                // header injection
		{ TEXT("X-D"), TEXT("caf\u00E9") },                  // StringCast<ANSICHAR> would mangle it (review item 21)
		{ TEXT("X bad"), TEXT("v") },                        // not a token
	};
	for (const TPair<FString, FString>& Header : Bad)
	{
		Request.Headers = { Header };
		const TValueOrError<FSocketTarget, FTargetError> Made = PrepareTarget(Request);
		TestTrue(FString::Printf(TEXT("header \"%s\" is BadHandshake"), *Header.Key), Made.HasError() && Made.GetError().Failure == EVaRestSocketFailure::BadHandshake);
	}
	Request.Headers = { { TEXT("X-Big"), FString::ChrN(33000, TEXT('a')) } };
	const TValueOrError<FSocketTarget, FTargetError> Big = PrepareTarget(Request);
	TestTrue(TEXT("headers over 32 KiB are BadHandshake before any connect"), Big.HasError() && Big.GetError().Message.Contains(TEXT("32768"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketTargetAllowlistTest, "VaRest.WebSocket.Target.Allowlist",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestSocketTargetAllowlistTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	// URLRequestFilter.cpp:87-165: no map allows everything; a scheme without a section is refused; a section's domains are matched
	const TArray<TPair<FString, TArray<FString>>> None;
	TestTrue(TEXT("an empty filter allows everything"), IsAllowedByFilter(TEXT("ws://127.0.0.1:9"), &None));
	const TArray<TPair<FString, TArray<FString>>> OnlyExample = { { TEXT("ws"), { TEXT("example.com") } } };
	TestFalse(TEXT("a domain not listed is refused"), IsAllowedByFilter(TEXT("ws://127.0.0.1:9"), &OnlyExample));
	TestTrue(TEXT("a listed one is allowed"), IsAllowedByFilter(TEXT("ws://example.com"), &OnlyExample));
	TestFalse(TEXT("a scheme without a section is refused (plan decision 15)"), IsAllowedByFilter(TEXT("wss://example.com"), &OnlyExample));
	TestTrue(TEXT("IPv4 literals are recognised"), IsIpv4Literal(TEXT("127.0.0.1")) && IsIpv4Literal(TEXT("0.0.0.0")) && IsIpv4Literal(TEXT("255.255.255.255")));
	TestFalse(TEXT("... and only they"), IsIpv4Literal(TEXT("256.0.0.1")) || IsIpv4Literal(TEXT("1.2.3")) || IsIpv4Literal(TEXT("1.2.3.4.5"))
		|| IsIpv4Literal(TEXT("a.b.c.d")) || IsIpv4Literal(TEXT("1..2.3")) || IsIpv4Literal(TEXT("1234.1.1.1")));
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.WebSocket.Target`
Expected: FAIL to compile (`VaRestSocketTarget.h` not found).

- [ ] **Step 3: The header** — create `Source/VaRestWebSocket/Private/VaRestSocketTarget.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket
{
	/** What a connect request becomes before the service thread sees it (WebSocket spec 2.2) */
	struct FSocketTarget
	{
		bool bTls = false;
		/** wss+insecure://, or wss:// with bAllowSelfSignedTls; never under Shipping rules (spec 2.2, 15) */
		bool bInsecure = false;
		/** The host as TCP and the TLS name check take it: no brackets, no port */
		FString Host;
		bool bHostIsIp = false;
		int32 Port = 0;
		/** The Host header: the host ([...] for IPv6) and a port that is not the scheme's default (LwsWebSocket.cpp:775-785) */
		FString HostHeader;
		/** From '/' on; bytes past ASCII, spaces and controls percent-encoded (RFC 3987 3.1) */
		FString PathAndQuery;
		/** Empty: no Origin header */
		FString Origin;
		/** The offered subprotocols joined with ", "; empty: none */
		FString Protocols;
		/** "Name: value\r\n" for each header, ASCII */
		TArray<uint8> HeaderBlock;
		/** What the engine's URL filter sees: scheme (wss for wss+insecure) and host, never the path or query (plan decision 15) */
		FString FilterUrl;
	};

	struct FTargetError
	{
		EVaRestSocketFailure Failure = EVaRestSocketFailure::BadUrl;
		/** Never quotes a header value, the path or the query: it reaches logs */
		FString Message;
	};

	/** The most header bytes VaRest appends: the 32 KiB handshake buffer (spec 15), which LWS's own lines share -- the exact room is checked again in APPEND_HANDSHAKE_HEADER */
	inline constexpr int32 MaxHeaderBlockBytes = 32 * 1024;

	/**
	 * Any thread: Request's URL, headers, subprotocols and origin, checked and encoded (spec 2.2; plan decisions 13, 14).
	 * bShippingRules: wss+insecure is refused and bAllowSelfSignedTls ignored, as in a Shipping build.
	 */
	TValueOrError<FSocketTarget, FTargetError> PrepareTarget(const FVaRestSocketConnectRequest& Request, bool bShippingRules = UE_BUILD_SHIPPING != 0);

	/** Four decimal octets, 0-255 each */
	bool IsIpv4Literal(FStringView Host);

	/**
	 * Spec 2.2 (LwsWebSocket.cpp:112-122): the engine's URL request filter (URLRequestFilter.h:28-70) for FilterUrl, from
	 * [Online.HttpManager <scheme>] in GEngineIni -- game thread -- or, for tests, from Allowlist (plan decision 19).
	 */
	bool IsAllowedByFilter(const FString& FilterUrl, const TArray<TPair<FString, TArray<FString>>>* Allowlist = nullptr);
}
```

- [ ] **Step 4: The implementation** — create `Source/VaRestWebSocket/Private/VaRestSocketTarget.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTarget.h"

#include "CoreGlobals.h"
#include "Misc/URLRequestFilter.h"
#include "VaRestUrl.h"

namespace VaRest::WebSocket::VaRestSocketTargetPrivate
{
	/** The headers LWS writes itself (RFC 6455 4.1) or VaRest sets from Origin and Protocols: a duplicate breaks the upgrade */
	const TCHAR* const ReservedHeaders[] = { TEXT("Host"), TEXT("Upgrade"), TEXT("Connection"), TEXT("Origin"), TEXT("Sec-WebSocket-Key"),
		TEXT("Sec-WebSocket-Version"), TEXT("Sec-WebSocket-Protocol"), TEXT("Sec-WebSocket-Extensions"), TEXT("Sec-WebSocket-Accept") };

	TValueOrError<VaRest::WebSocket::FSocketTarget, VaRest::WebSocket::FTargetError> Refuse(EVaRestSocketFailure Failure, FString Message)
	{
		return MakeError(VaRest::WebSocket::FTargetError{ Failure, MoveTemp(Message) });
	}

	/** ASCII letters, digits, '-', '.' and '_': a non-ASCII host needs punycode, as for HTTP (VaRest::Http::HasNonAsciiHost) */
	bool IsHostChar(TCHAR C)
	{
		return (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9'))
			|| C == TEXT('-') || C == TEXT('.') || C == TEXT('_');
	}

	bool IsDigits(FStringView Text)
	{
		if (Text.IsEmpty())
		{
			return false;
		}
		for (const TCHAR C : Text)
		{
			if (C < TEXT('0') || C > TEXT('9'))
			{
				return false;
			}
		}
		return true;
	}

	/** VCHAR, space and tab (spec 2.2, D23) */
	bool IsHeaderValueChar(TCHAR C)
	{
		return C == TEXT('\t') || (C >= 0x20 && C <= 0x7E);
	}

	/** RFC 3987 3.1: each UTF-8 byte at or past 0x80, each space and control, as %XX; everything else -- '%' included -- as written */
	FString EncodePathAndQuery(FStringView Text)
	{
		const FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		FString Out;
		Out.Reserve(Utf8.Length());
		for (int32 Index = 0; Index < Utf8.Length(); ++Index)
		{
			const uint8 Byte = static_cast<uint8>(Utf8.Get()[Index]);
			if (Byte >= 0x80 || Byte <= 0x20 || Byte == 0x7F)
			{
				Out.Appendf(TEXT("%%%02X"), Byte);
			}
			else
			{
				Out.AppendChar(static_cast<TCHAR>(Byte));
			}
		}
		return Out;
	}
}

bool VaRest::WebSocket::IsIpv4Literal(FStringView Host)
{
	int32 Dots = 0;
	int32 Digits = 0;
	int32 Value = 0;
	for (const TCHAR C : Host)
	{
		if (C == TEXT('.'))
		{
			if (Digits == 0)
			{
				return false;
			}
			++Dots;
			Digits = 0;
			Value = 0;
			continue;
		}
		if (C < TEXT('0') || C > TEXT('9') || ++Digits > 3)
		{
			return false;
		}
		Value = Value * 10 + (C - TEXT('0'));
		if (Value > 255)
		{
			return false;
		}
	}
	return Dots == 3 && Digits > 0;
}

TValueOrError<VaRest::WebSocket::FSocketTarget, VaRest::WebSocket::FTargetError> VaRest::WebSocket::PrepareTarget(const FVaRestSocketConnectRequest& Request, bool bShippingRules)
{
	using namespace VaRest::WebSocket::VaRestSocketTargetPrivate;
	FSocketTarget Out;
	const FString& Url = Request.Url;
	const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
	if (SchemeEnd == INDEX_NONE)
	{
		return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the URL has no scheme (ws:// or wss://)"));
	}
	const FString Scheme = Url.Left(SchemeEnd).ToLower();
	bool bInsecureScheme = false;
	if (Scheme.Equals(TEXT("wss"), ESearchCase::CaseSensitive))
	{
		Out.bTls = true;
	}
	else if (Scheme.Equals(TEXT("wss+insecure"), ESearchCase::CaseSensitive) && !bShippingRules)
	{
		Out.bTls = true;
		bInsecureScheme = true;
	}
	else if (!Scheme.Equals(TEXT("ws"), ESearchCase::CaseSensitive))
	{
		return Refuse(EVaRestSocketFailure::BadUrl, bShippingRules ? TEXT("the scheme is not ws or wss") : TEXT("the scheme is not ws, wss or wss+insecure"));
	}
	Out.bInsecure = bInsecureScheme || (Out.bTls && Request.bAllowSelfSignedTls && !bShippingRules);
	const FString Rest = Url.Mid(SchemeEnd + 3);
	int32 Found = INDEX_NONE;
	if (Rest.FindChar(TEXT('#'), Found))
	{
		return Refuse(EVaRestSocketFailure::BadUrl, TEXT("a WebSocket URL has no fragment (#)"));
	}
	int32 AuthorityEnd = Rest.Len();
	for (int32 Index = 0; Index < Rest.Len(); ++Index)
	{
		if (Rest[Index] == TEXT('/') || Rest[Index] == TEXT('?'))
		{
			AuthorityEnd = Index;
			break;
		}
	}
	const FString Authority = Rest.Left(AuthorityEnd);
	if (Authority.FindChar(TEXT('@'), Found))
	{
		return Refuse(EVaRestSocketFailure::BadUrl, TEXT("user info in a WebSocket URL is never sent: put credentials in a header"));
	}
	FString PortText;
	bool bPortGiven = false;
	if (Authority.StartsWith(TEXT("["), ESearchCase::CaseSensitive))
	{
		int32 CloseBracket = INDEX_NONE;
		if (!Authority.FindChar(TEXT(']'), CloseBracket) || CloseBracket < 2)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("an IPv6 host needs [ and ]"));
		}
		Out.Host = Authority.Mid(1, CloseBracket - 1);
		bool bColon = false;
		for (const TCHAR C : Out.Host)
		{
			if (!(FChar::IsHexDigit(C) || C == TEXT(':') || C == TEXT('.')))
			{
				return Refuse(EVaRestSocketFailure::BadUrl, TEXT("an IPv6 host holds only hex digits, ':' and '.'"));
			}
			bColon = bColon || C == TEXT(':');
		}
		if (!bColon)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("brackets hold an IPv6 address"));
		}
		Out.bHostIsIp = true;
		const FString After = Authority.Mid(CloseBracket + 1);
		if (!After.IsEmpty())
		{
			if (!After.StartsWith(TEXT(":"), ESearchCase::CaseSensitive))
			{
				return Refuse(EVaRestSocketFailure::BadUrl, TEXT("only :port may follow an IPv6 host"));
			}
			bPortGiven = true;
			PortText = After.Mid(1);
		}
	}
	else
	{
		int32 Colon = INDEX_NONE;
		if (Authority.FindLastChar(TEXT(':'), Colon))
		{
			Out.Host = Authority.Left(Colon);
			PortText = Authority.Mid(Colon + 1);
			bPortGiven = true;
		}
		else
		{
			Out.Host = Authority;
		}
		if (Out.Host.IsEmpty())
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the URL has no host"));
		}
		for (const TCHAR C : Out.Host)
		{
			if (!IsHostChar(C))
			{
				return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the host holds a character VaRest does not send (a non-ASCII host needs punycode; spaces and controls are refused)"));
			}
		}
		Out.bHostIsIp = IsIpv4Literal(Out.Host);
	}
	const int32 DefaultPort = Out.bTls ? 443 : 80;
	Out.Port = DefaultPort;
	if (bPortGiven)
	{
		if (!IsDigits(PortText) || PortText.Len() > 5)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the port is not a number from 1 to 65535"));
		}
		Out.Port = FCString::Atoi(*PortText);
		if (Out.Port < 1 || Out.Port > 65535)
		{
			return Refuse(EVaRestSocketFailure::BadUrl, TEXT("the port is not a number from 1 to 65535"));
		}
	}
	const FString BracketedHost = Out.Host.Contains(TEXT(":"), ESearchCase::CaseSensitive) ? FString::Printf(TEXT("[%s]"), *Out.Host) : Out.Host;
	// LwsWebSocket.cpp:775-785: the Host header drops the scheme's default port and keeps any other
	Out.HostHeader = Out.Port == DefaultPort ? BracketedHost : FString::Printf(TEXT("%s:%d"), *BracketedHost, Out.Port);
	FString Path = Rest.Mid(AuthorityEnd);
	if (Path.IsEmpty())
	{
		Path = TEXT("/");
	}
	else if (Path[0] == TEXT('?'))
	{
		Path = TEXT("/") + Path;
	}
	Out.PathAndQuery = EncodePathAndQuery(Path);
	// Origin (spec 2.2, D29): derived, given or omitted; sent raw (T4, LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN)
	if (!Request.Origin.IsSet())
	{
		Out.Origin = FString(Out.bTls ? TEXT("https://") : TEXT("http://")) + Out.HostHeader;
	}
	else if (!Request.Origin.GetValue().IsEmpty())
	{
		for (const TCHAR C : Request.Origin.GetValue())
		{
			if (C <= 0x20 || C >= 0x7F)
			{
				return Refuse(EVaRestSocketFailure::BadHandshake, TEXT("the Origin holds a space, a control or a non-ASCII character"));
			}
		}
		Out.Origin = Request.Origin.GetValue();
	}
	// Subprotocols (RFC 6455 4.1: tokens, each offered once; plan decision 14)
	TArray<FString> Offered;
	for (const FString& Protocol : Request.Protocols)
	{
		if (!VaRest::Http::IsValidHeaderName(Protocol))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, TEXT("a subprotocol is not an RFC 7230 token"));
		}
		if (Offered.ContainsByPredicate([&Protocol](const FString& Each) { return Each.Equals(Protocol, ESearchCase::CaseSensitive); }))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("subprotocol %s is offered twice"), *Protocol));
		}
		Offered.Add(Protocol);
	}
	Out.Protocols = FString::Join(Offered, TEXT(", "));
	// Headers (spec 2.2, D23): checked here so the error is immediate; the room is checked again in APPEND_HANDSHAKE_HEADER
	for (const TPair<FString, FString>& Header : Request.Headers)
	{
		if (!VaRest::Http::IsValidHeaderName(Header.Key))
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, TEXT("a header name is not an RFC 7230 token"));
		}
		for (const TCHAR* Reserved : ReservedHeaders)
		{
			if (Header.Key.Equals(Reserved, ESearchCase::IgnoreCase))
			{
				return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("header %s is the transport's own (set Origin or Protocols instead)"), Reserved));
			}
		}
		for (const TCHAR C : Header.Value)
		{
			if (!IsHeaderValueChar(C))
			{
				// The name is a token, so it is safe to print; the value never is
				return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("the value of header %s holds a control or a non-ASCII character"), *Header.Key));
			}
		}
		const FString Line = FString::Printf(TEXT("%s: %s\r\n"), *Header.Key, *Header.Value);
		if (Out.HeaderBlock.Num() + Line.Len() > MaxHeaderBlockBytes)
		{
			return Refuse(EVaRestSocketFailure::BadHandshake, FString::Printf(TEXT("the headers are over %d bytes, the handshake buffer (spec 15)"), MaxHeaderBlockBytes));
		}
		for (const TCHAR C : Line)
		{
			Out.HeaderBlock.Add(static_cast<uint8>(C));
		}
	}
	Out.FilterUrl = FString::Printf(TEXT("%s://%s"), Out.bTls ? TEXT("wss") : TEXT("ws"), *Out.HostHeader);
	return MakeValue(MoveTemp(Out));
}

bool VaRest::WebSocket::IsAllowedByFilter(const FString& FilterUrl, const TArray<TPair<FString, TArray<FString>>>* Allowlist)
{
	if (Allowlist)
	{
		UE::Core::FURLRequestFilter::FRequestMap Map;
		for (const TPair<FString, TArray<FString>>& Entry : *Allowlist)
		{
			Map.Emplace(Entry.Key, Entry.Value);
		}
		return UE::Core::FURLRequestFilter(MoveTemp(Map)).IsRequestAllowed(FilterUrl);
	}
	check(IsInGameThread());
	// Built per connect, as the engine does (LwsWebSocket.cpp:114), so an ini change applies to the next attempt
	return UE::Core::FURLRequestFilter(TEXT("Online.HttpManager"), GEngineIni).IsRequestAllowed(FilterUrl);
}
```

- [ ] **Step 5: Run them**

Run: `Tools/run_tests.sh VaRest.WebSocket.Target`
Expected: `run_tests: 4 passed, 0 failed`.

- [ ] **Step 6: Restore-the-bug** — delete the reserved-header loop in `PrepareTarget`: `VaRest.WebSocket.Target.Handshake` FAILS at "header \"Origin\" is BadHandshake" (and the other two reserved names); restore; PASSES. Then make the value check `C >= 0x20 && C <= 0x7E` drop the tab (`C == TEXT('\t') ||` removed): the test FAILS at "tab and space in values are taken" (the run completes: every `GetValue()` is behind `ValueOf`, review spec I-4); restore; PASSES.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestSocketTarget.h Source/VaRestWebSocket/Private/VaRestSocketTarget.cpp \
  Source/VaRestWebSocket/Private/Tests/VaRestSocketTargetTest.cpp
git commit -m "ws: W1a task 4 -- connect targets: VaRest's URL rules, Host with a non-default port, raw Origin, subprotocol and header checks, the allowlist on scheme and host

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 5: libwebsockets' connection-error strings → failures (pure)

Every client string of the bundled archive that LWS 3.0 can hand `CLIENT_CONNECTION_ERROR` (its `strings`, re-checked by review spec M-3: `set socket opts failed`, `insert wsi failed`, `POLLOUT set failed`, `first service failed` and `HS: SO_SNDBUF failed` are in it too) mapped to a failure and, for the status-line parser's, a status (plan decision 11). Spec §16.1's `ConnectError` asks for every §2.2 string in the parser tests.

**Files:**
- Create: `Source/VaRestWebSocket/Private/VaRestLwsErrors.h`, `Source/VaRestWebSocket/Private/VaRestLwsErrors.cpp`
- Test: `Source/VaRestWebSocket/Private/Tests/VaRestLwsErrorsTest.cpp`

**Interfaces:**
- Consumes: `EVaRestSocketFailure` (Task 2).
- Produces: `struct VaRest::WebSocket::Lws::FConnectionErrorClass { EVaRestSocketFailure Failure = Unknown; int32 HttpStatus = 0; bool bEmpty = false; }` (`bEmpty`: the string was empty after trimming; the connection decides by elapsed time, review conc M-2); `LwsConnectStageSeconds = 20.0`; `FConnectionErrorClass VaRest::WebSocket::Lws::ClassifyEmptyConnectionError(double TookSeconds, FString& OutDetail)` (any thread; re-review: conc-M-2's elapsed branch, table-tested); `FConnectionErrorClass VaRest::WebSocket::Lws::ClassifyConnectionError(FStringView Text)` (any thread; no LWS include needed).

- [ ] **Step 1: Write the failing test** — create `Source/VaRestWebSocket/Private/Tests/VaRestLwsErrorsTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestLwsErrors.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsErrorsTest, "VaRest.WebSocket.Lws.ConnectError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsErrorsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	struct FRow
	{
		const TCHAR* Text;
		EVaRestSocketFailure Failure;
		int32 Status;
	};
	// Spec 2.2's strings and the rest of the archive's (strings lib/Unix/x86_64-unknown-linux-gnu/Release/libwebsockets.a)
	const FRow Rows[] = {
		{ TEXT("HS: ws upgrade unauthorized"), EVaRestSocketFailure::UpgradeRefused, 401 },          // hs.refusal_401
		{ TEXT("HS: ws upgrade forbidden"), EVaRestSocketFailure::UpgradeRefused, 403 },             // hs.refusal_403
		{ TEXT("HS: ws upgrade not 101, was 404"), EVaRestSocketFailure::UpgradeRefused, 404 },      // hs.refusal_404
		{ TEXT("HS: ws upgrade not 101, was 429"), EVaRestSocketFailure::UpgradeRefused, 429 },
		{ TEXT("HS: ws upgrade not 101, was 503"), EVaRestSocketFailure::UpgradeRefused, 503 },
		{ TEXT("HS: ws upgrade not 101, was 302"), EVaRestSocketFailure::Redirect, 302 },            // a 3xx LWS did not follow
		{ TEXT("HS: ws upgrade not 101, was abc"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("HS: Redirect failed"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: Redirect attempted SSL downgrade"), EVaRestSocketFailure::Redirect, 0 },         // hs.redirect_downgrade
		{ TEXT("HS: Redirect code but no Location"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: PROTOCOL malformed"), EVaRestSocketFailure::ProtocolMismatch, 0 },               // hs.unoffered_selected
		{ TEXT("HS: Cannot match protocol"), EVaRestSocketFailure::ProtocolMismatch, 0 },
		{ TEXT("HS: ACCEPT missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Accept hash wrong"), EVaRestSocketFailure::BadUpgradeResponse, 0 },              // hs.bad_accept
		{ TEXT("HS: UPGRADE missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },                // hs.missing_upgrade
		{ TEXT("HS: UPGRADE malformed"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Upgrade to something other than websocket"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: CONNECTION missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },             // hs.missing_connection
		{ TEXT("HS: :status missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI did not parse"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: h2 / ws upgrade unsupported"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed by client filter"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Rejected by filter cb"), EVaRestSocketFailure::BadUpgradeResponse, 0 },          // hs.redirect_veto (VaRest's own refusal wins there)
		{ TEXT("HS: Rejected at CLIENT_ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed at ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("read failed"), EVaRestSocketFailure::NoUpgradeResponse, 0 },                          // hs.no_upgrade_response (22 A2)
		{ TEXT("connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },                           // hs.connect_refused
		{ TEXT("client connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },                    // tls.untrusted_rejected (VaRest's TLS evidence wins there)
		{ TEXT("getaddrinfo failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("socket connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("lws_ssl_client_connect1 failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("bio_create failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("proxy write failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("ERROR reading from proxy socket"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("ERROR proxy: HTTP/1.1 403 Forbidden"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("set socket opts failed"), EVaRestSocketFailure::ConnectFailed, 0 },                   // review spec M-3: the archive's other client strings
		{ TEXT("insert wsi failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("POLLOUT set failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("first service failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("HS: SO_SNDBUF failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT(""), EVaRestSocketFailure::Unknown, 0 },                                               // timeout.default_secs, killed_pending: FConn decides by elapsed time (review conc M-2)
		{ TEXT("Timed out waiting SSL"), EVaRestSocketFailure::ConnectTimeout, 0 },
		{ TEXT("HS: OOM"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("something LWS 3.0 does not say"), EVaRestSocketFailure::Unknown, 0 },
		{ TEXT("Read failed"), EVaRestSocketFailure::Unknown, 0 },                                    // case matters
	};
	for (const FRow& Row : Rows)
	{
		const FConnectionErrorClass Class = ClassifyConnectionError(Row.Text);
		TestTrue(FString::Printf(TEXT("\"%s\" is %s (got %s)"), Row.Text, LexToString(Row.Failure), LexToString(Class.Failure)), Class.Failure == Row.Failure);
		TestEqual(FString::Printf(TEXT("\"%s\": status"), Row.Text), Class.HttpStatus, Row.Status);
	}
	// LWS may count a trailing NUL or newline in len
	const TCHAR WithNul[] = { TEXT('r'), TEXT('e'), TEXT('a'), TEXT('d'), TEXT(' '), TEXT('f'), TEXT('a'), TEXT('i'), TEXT('l'), TEXT('e'), TEXT('d'), 0 };
	TestTrue(TEXT("a trailing NUL is ignored"), ClassifyConnectionError(FStringView(WithNul, 12)).Failure == EVaRestSocketFailure::NoUpgradeResponse);
	TestTrue(TEXT("a trailing newline is ignored"), ClassifyConnectionError(TEXT("connect failed\n")).Failure == EVaRestSocketFailure::ConnectFailed);
	// Review conc M-2: an empty string is LWS's 20 s connect stage, timeout_secs, or a kill VaRest did not make; the
	// classifier only says it was empty
	TestTrue(TEXT("an empty string is flagged empty"), ClassifyConnectionError(TEXT("")).bEmpty);
	const TCHAR JustNul[] = { 0 };
	TestTrue(TEXT("... and so is a lone NUL"), ClassifyConnectionError(FStringView(JustNul, 1)).bEmpty);
	TestFalse(TEXT("a real string is not"), ClassifyConnectionError(TEXT("read failed")).bEmpty);
	// ... and the connection splits it by the time the attempt took (re-review: conc-M-2's elapsed branch)
	struct FEmptyRow
	{
		double Took;
		EVaRestSocketFailure Failure;
		const TCHAR* DetailHas;
	};
	const FEmptyRow EmptyRows[] = {
		{ 0.2, EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },          // a kill VaRest did not make
		{ 19.4, EVaRestSocketFailure::Unknown, TEXT("empty CONNECTION_ERROR") },
		{ 19.5, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },     // LWS's fixed TCP-connect timer (review conc M-1)
		{ 20.1, EVaRestSocketFailure::ConnectTimeout, TEXT("20 s connect stage") },
		{ 60.3, EVaRestSocketFailure::ConnectTimeout, TEXT("timeout_secs") },           // the context's backstop
	};
	for (const FEmptyRow& Row : EmptyRows)
	{
		FString Detail;
		const FConnectionErrorClass Class = ClassifyEmptyConnectionError(Row.Took, Detail);
		TestTrue(FString::Printf(TEXT("an empty string after %.1f s is %s (got %s)"), Row.Took, LexToString(Row.Failure), LexToString(Class.Failure)), Class.Failure == Row.Failure);
		TestTrue(FString::Printf(TEXT("... its detail names it (%s)"), *Detail), Detail.Contains(Row.DetailHas, ESearchCase::CaseSensitive));
	}
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.WebSocket.Lws.ConnectError`
Expected: FAIL to compile (`VaRestLwsErrors.h` not found).

- [ ] **Step 3: The header** — create `Source/VaRestWebSocket/Private/VaRestLwsErrors.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketTypes.h"

namespace VaRest::WebSocket::Lws
{
	/** What a CLIENT_CONNECTION_ERROR string means (plan decision 11) */
	struct FConnectionErrorClass
	{
		EVaRestSocketFailure Failure = EVaRestSocketFailure::Unknown;
		int32 HttpStatus = 0;
		/**
		 * The string was empty after trimming: one of LWS's own stage timers (a fixed 20 s for the TCP connect and the proxy,
		 * timeout_secs for the others; review conc M-1) or a kill VaRest did not make. Failure is Unknown; the connection
		 * decides by the time the attempt took (review conc M-2)
		 */
		bool bEmpty = false;
	};

	/**
	 * Any thread: libwebsockets 3.0's error string for a client that never opened (libwebsockets.h:1255-1299; the strings of
	 * lib/Unix/x86_64-unknown-linux-gnu/Release/libwebsockets.a; passport hs.*, tls.untrusted_rejected, timeout.*), as VaRest
	 * classifies it. The status comes only from the string: lws_http_client_http_response is never called there (T18).
	 * A failure VaRest caused itself (a redirect veto, a refused certificate, its own kill) is the connection's to report: it
	 * overrides what this says.
	 */
	FConnectionErrorClass ClassifyConnectionError(FStringView Text);

	/**
	 * LWS's own timer for the TCP connect and the proxy exchange, fixed in 3.0 whatever timeout_secs says (review conc M-1:
	 * lws_client_connect_2 arms (2 | 8 | 1, 20) at client.c.o 0x572-0x57f, 0x7b6-0x7c3, 0x843-0x850 of the Release archive)
	 */
	inline constexpr double LwsConnectStageSeconds = 20.0;

	/**
	 * Any thread: an empty CONNECTION_ERROR (bEmpty) by the time the attempt took (review conc M-1, M-2; re-review: a pure
	 * function, so the split is table-tested). From LwsConnectStageSeconds - 0.5 on it is LWS's own stage timer --
	 * ConnectTimeout; before, Unknown (a kill VaRest did not make). OutDetail says which, with the time
	 */
	FConnectionErrorClass ClassifyEmptyConnectionError(double TookSeconds, FString& OutDetail);
}
```

- [ ] **Step 4: The table** — create `Source/VaRestWebSocket/Private/VaRestLwsErrors.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsErrors.h"

namespace VaRest::WebSocket::Lws::VaRestLwsErrorsPrivate
{
	struct FRow
	{
		const TCHAR* Text;
		EVaRestSocketFailure Failure;
		int32 Status;
	};

	const FRow Rows[] = {
		// The status-line parser (T7; passport hs.refusal_401, hs.refusal_403): the status is in the string
		{ TEXT("HS: ws upgrade unauthorized"), EVaRestSocketFailure::UpgradeRefused, 401 },
		{ TEXT("HS: ws upgrade forbidden"), EVaRestSocketFailure::UpgradeRefused, 403 },
		// A 3xx LWS could not follow (it follows the rest, plan decision 10) or a downgrade it refuses [hs.redirect_downgrade]
		{ TEXT("HS: Redirect failed"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: Redirect attempted SSL downgrade"), EVaRestSocketFailure::Redirect, 0 },
		{ TEXT("HS: Redirect code but no Location"), EVaRestSocketFailure::Redirect, 0 },
		// A subprotocol the client did not offer [hs.unoffered_selected]
		{ TEXT("HS: PROTOCOL malformed"), EVaRestSocketFailure::ProtocolMismatch, 0 },
		{ TEXT("HS: Cannot match protocol"), EVaRestSocketFailure::ProtocolMismatch, 0 },
		// A 101 a client must refuse [hs.bad_accept, hs.missing_upgrade, hs.missing_connection]
		{ TEXT("HS: ACCEPT missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Accept hash wrong"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: UPGRADE missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: UPGRADE malformed"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Upgrade to something other than websocket"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: CONNECTION missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: :status missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI missing"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: URI did not parse"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: h2 / ws upgrade unsupported"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed by client filter"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		// VaRest's own refusals in FILTER_PRE_ESTABLISH and ESTABLISHED: the connection's own failure overrides these
		{ TEXT("HS: Rejected by filter cb"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: Rejected at CLIENT_ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		{ TEXT("HS: disallowed at ESTABLISHED"), EVaRestSocketFailure::BadUpgradeResponse, 0 },
		// 22 A2: the upgrade went out and the connection closed before any status line [hs.no_upgrade_response]
		{ TEXT("read failed"), EVaRestSocketFailure::NoUpgradeResponse, 0 },
		// No HTTP status at all: TCP [hs.connect_refused], TLS [tls.untrusted_rejected], DNS, the proxy
		{ TEXT("connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("client connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("getaddrinfo failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("socket connect failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("lws_ssl_client_connect1 failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("bio_create failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("proxy write failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("ERROR reading from proxy socket"), EVaRestSocketFailure::ConnectFailed, 0 },
		// The archive's other client strings (review spec M-3): the socket could not be set up or serviced
		{ TEXT("set socket opts failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("insert wsi failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("POLLOUT set failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("first service failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("HS: SO_SNDBUF failed"), EVaRestSocketFailure::ConnectFailed, 0 },
		{ TEXT("Timed out waiting SSL"), EVaRestSocketFailure::ConnectTimeout, 0 },
	};

	const TCHAR* const NotOneOhOne = TEXT("HS: ws upgrade not 101, was ");
	const TCHAR* const ProxyError = TEXT("ERROR proxy: ");
}

VaRest::WebSocket::Lws::FConnectionErrorClass VaRest::WebSocket::Lws::ClassifyConnectionError(FStringView Text)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsErrorsPrivate;
	FStringView Trimmed = Text;
	while (!Trimmed.IsEmpty() && (Trimmed[Trimmed.Len() - 1] == 0 || FChar::IsWhitespace(Trimmed[Trimmed.Len() - 1])))
	{
		Trimmed.LeftChopInline(1);
	}
	FConnectionErrorClass Out;
	if (Trimmed.IsEmpty())
	{
		// LWS's own stage timeouts and a kill while pending give "" [timeout.default_secs, hs.status_in_connection_error.killed_pending]
		Out.bEmpty = true;
		return Out;
	}
	if (Trimmed.StartsWith(NotOneOhOne, ESearchCase::CaseSensitive))
	{
		const FStringView Digits = Trimmed.RightChop(FCString::Strlen(NotOneOhOne));
		int32 Status = 0;
		for (const TCHAR C : Digits)
		{
			if (!FChar::IsDigit(C) || Digits.Len() > 3)
			{
				return Out;
			}
			Status = Status * 10 + (C - TEXT('0'));
		}
		if (Digits.IsEmpty())
		{
			return Out;
		}
		// A 3xx that reached the parser is a redirect LWS did not follow, refused all the same (ruling Q2)
		Out.Failure = Status >= 300 && Status <= 399 ? EVaRestSocketFailure::Redirect : EVaRestSocketFailure::UpgradeRefused;
		Out.HttpStatus = Status;
		return Out;
	}
	if (Trimmed.StartsWith(ProxyError, ESearchCase::CaseSensitive))
	{
		Out.Failure = EVaRestSocketFailure::ConnectFailed;
		return Out;
	}
	for (const FRow& Row : Rows)
	{
		if (Trimmed.Equals(Row.Text, ESearchCase::CaseSensitive))
		{
			Out.Failure = Row.Failure;
			Out.HttpStatus = Row.Status;
			return Out;
		}
	}
	return Out;
}

VaRest::WebSocket::Lws::FConnectionErrorClass VaRest::WebSocket::Lws::ClassifyEmptyConnectionError(double TookSeconds, FString& OutDetail)
{
	// "" is one of LWS's stage timers -- its fixed 20 s for the TCP connect and the proxy, or timeout_secs -- or a kill VaRest
	// did not make. VaRest's own ConnectTimeout (at most timeout_secs) normally fires first, so only the time taken tells them apart
	FConnectionErrorClass Out;
	Out.bEmpty = true;
	if (TookSeconds >= LwsConnectStageSeconds - 0.5)
	{
		Out.Failure = EVaRestSocketFailure::ConnectTimeout;
		OutDetail = FString::Printf(TEXT("libwebsockets' own handshake timeout after %.1f s (its 20 s connect stage, or timeout_secs)"), TookSeconds);
	}
	else
	{
		OutDetail = FString::Printf(TEXT("an empty CONNECTION_ERROR from libwebsockets after %.1f s"), TookSeconds);
	}
	return Out;
}
```

- [ ] **Step 5: Run it**

Run: `Tools/run_tests.sh VaRest.WebSocket.Lws.ConnectError`
Expected: `run_tests: 1 passed, 0 failed`.

- [ ] **Step 6: Restore-the-bug** — delete the `read failed` row: the test FAILS at `"read failed" is NoUpgradeResponse` (22 A2's string); restore; PASSES. Then make the 3xx branch always `UpgradeRefused`: FAILS at `"... was 302" is Redirect`; restore; PASSES. Then delete the `if (Trimmed.IsEmpty())` block: FAILS at "an empty string is flagged empty" (review conc M-2); restore; PASSES. Then make `ClassifyEmptyConnectionError` always `ConnectTimeout` (the old classification): FAILS at "an empty string after 0.2 s is Unknown"; restore; PASSES.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsErrors.h Source/VaRestWebSocket/Private/VaRestLwsErrors.cpp \
  Source/VaRestWebSocket/Private/Tests/VaRestLwsErrorsTest.cpp
git commit -m "ws: W1a task 5 -- every libwebsockets 3.0 client error string mapped to a failure; the status from the string, never lws_http_client_http_response

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 6: The service — one context, one thread, commands, waits, VaRest's deadlines loop, shutdown, the log level

`FService` owns one `lws_context` and the thread that services it (spec §2.1, plan decisions 8, 12, 16, 22). Connections plug in through `IServiceConnection`, so this task is tested with stand-ins and no network; Task 7's `FConn` is the real one.

**Files:**
- Create: `Source/VaRestWebSocket/Private/VaRestLwsService.h`, `Source/VaRestWebSocket/Private/VaRestLwsService.cpp`
- Test: `Source/VaRestWebSocket/Private/Tests/VaRestLwsServiceTest.cpp`

**Interfaces:**
- Consumes: `VaRestLwsInclude.h`, `LogVaRestSocket`, `VaRest_WebSocket` (Task 1); `FrameHeadroom` (Task 2); `VaRest::WebSocket::MaxConnectTimeoutSeconds` (Task 3, `VaRestSocketTransport.h`, which `VaRestLwsService.cpp` includes; re-review A-8).
- Produces:
  - constants in `VaRest::WebSocket::Lws`: `LwsCloseAckSeconds = 5.0`, `KillRearmSeconds = 1.25`, `FragmentBytes = 64 * 1024`, `RxBufferBytes = 64 * 1024`, `HeaderBufferBytes = 32 * 1024`
  - `struct FServiceOptions { int32 IdleWaitMs = 30000; int32 ActiveWaitCapMs = 1000; int32 ShortWaitCapMs = 100; TArray<uint8> ExtraTrustedPem; FString TcpAddressOverride; bool bOverrideAllowlist = false; TArray<TPair<FString, TArray<FString>>> Allowlist; FString ThreadName = TEXT("VaRestLws"); double ExitFlushSeconds = 0.25; bool bFailThreadForTest = false; }`
  - `struct FServiceCounters { std::atomic<int32> LiveConnections, KillsIssued, KillsLanded, CommandsRun, ContextCreations, UnsentCloses; std::atomic<double> LastKillToDestroySeconds{-1}, MaxKillToDestroySeconds{-1}; }`
  - `std::atomic<int32>& TlsHandshakesInFlight()` (any thread; module-wide: VaRest's wss attempts LWS has neither established nor destroyed, review conc I-3)
  - `class IServiceConnection { GetId; Start(FService&); OnCallback(FService&, lws*, int32 Reason, void* User, void* In, size_t Len); CheckDeadlines(FService&, double Now); NextDeadline() const; WantsShortWait() const; IsDestroyed() const; EndForShutdown(); }` (service thread; `EndForShutdown` on the game thread with the thread stopped)
  - `using FCommand = TUniqueFunction<void(FService&)>`
  - `class FService final : public FRunnable, public FSingleThreadRunnable { explicit FService(FServiceOptions); bool Start(); void Shutdown(); bool IsRunning() const; bool IsShuttingDown() const; void Enqueue(FCommand&&); lws_context* GetContext() const; const FServiceOptions& GetOptions() const; ISslCertificateManager* GetCertificates() const; FServiceCounters& GetCounters(); void Adopt(TUniquePtr<IServiceConnection>&&); IServiceConnection* Find(uint64 Id) const; int32 ComputeWaitMs(double Now) const; static int Callback(lws*, lws_callback_reasons, void*, void*, size_t); }` — `Start`/`Shutdown` game thread; `Enqueue`, `IsRunning` any thread; the rest service thread (or the game thread while the thread is not running). `Start` is latched: after one failure it returns false without another `lws_create_context` (review conc I-2); it refuses, unlatched, while `TlsHandshakesInFlight()` is not 0 (review conc I-3). `Shutdown` first gives the running thread up to `ExitFlushSeconds` to write the close frames already asked for (review conc M-3)
  - `void Utf8Copy(const FString&, TArray<ANSICHAR>&)`, `void ApplyLogLevel()` (game thread), `bool ShouldCreateAtStartup(bool bSharedCopy, TFunctionRef<bool(const TCHAR*)> IsLinked)` (any thread)

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestWebSocket/Private/Tests/VaRestLwsServiceTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTLS.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/ScopeLock.h"
#include "VaRestHttpTesting.h"
#include "VaRestLwsService.h"

#include <atomic>

namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate
{
	/** What a stand-in connection saw; shared with the service thread */
	struct FStandInLog
	{
		std::atomic<bool> bStarted{ false };
		std::atomic<int32> Checks{ 0 };
		std::atomic<double> FiredLate{ -1.0 };
		std::atomic<int32> ShutdownEnds{ 0 };
	};

	/** A connection with one deadline and no wsi: the service's loop is all it exercises */
	class FStandIn final : public IServiceConnection
	{
	public:
		FStandIn(uint64 InId, double InDeadline, bool bInShort, TSharedRef<FStandInLog, ESPMode::ThreadSafe> InLog)
			: Id(InId), Deadline(InDeadline), bShort(bInShort), Log(MoveTemp(InLog))
		{
		}

		virtual uint64 GetId() const override { return Id; }
		virtual void Start(FService& Service) override { Log->bStarted = true; }
		virtual int32 OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len) override { return 0; }
		virtual void CheckDeadlines(FService& Service, double Now) override
		{
			++Log->Checks;
			if (!bFired && Now >= Deadline)
			{
				bFired = true;
				Log->FiredLate = Now - Deadline;
				if (bWritesCloseOnFire)
				{
					// Stands in for FConn's close write: the close it was asked for has gone out
					Service.GetCounters().UnsentCloses.fetch_sub(1);
				}
			}
		}
		virtual double NextDeadline() const override { return bFired ? TNumericLimits<double>::Max() : Deadline; }
		virtual bool WantsShortWait() const override { return bShort; }
		virtual bool IsDestroyed() const override { return bFired && bEndOnFire; }
		virtual void EndForShutdown() override { ++Log->ShutdownEnds; }

		bool bEndOnFire = true;
		/** The deadline writes a close FlushCloses waits for (review conc M-3) */
		bool bWritesCloseOnFire = false;

	private:
		const uint64 Id;
		const double Deadline;
		const bool bShort;
		bool bFired = false;
		TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log;
	};

	struct FRan
	{
		FCriticalSection Lock;
		TArray<int32> Order;
		TSet<uint32> Threads;
		std::atomic<int32> Count{ 0 };
		double FirstLatency = -1.0;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceStartStopTest, "VaRest.WebSocket.Lws.Service.StartStop",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceStartStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	TestTrue(TEXT("it runs"), Service->IsRunning());
	TestNotNull(TEXT("with a context"), Service->GetContext());
	TestNotNull(TEXT("and the engine's certificate manager"), Service->GetCertificates());
	Service->Shutdown();
	TestFalse(TEXT("it stops"), Service->IsRunning());
	TestNull(TEXT("its context is destroyed"), Service->GetContext());
	Service->Shutdown();
	TestFalse(TEXT("a second Shutdown changes nothing, and a shut-down service never starts again"), Service->Start());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceCommandsTest, "VaRest.WebSocket.Lws.Service.Commands",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceCommandsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FServiceOptions Options;
	// No connection, so the service waits this long unless lws_cancel_service wakes it (T13, svc.cancel_wake_ms)
	Options.IdleWaitMs = 30000;
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(MoveTemp(Options));
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Service->Shutdown();
	};
	const TSharedRef<FRan, ESPMode::ThreadSafe> Ran = MakeShared<FRan, ESPMode::ThreadSafe>();
	const double Sent = FPlatformTime::Seconds();
	for (int32 Index = 0; Index < 100; ++Index)
	{
		Service->Enqueue([Ran, Index, Sent](FService& Self)
		{
			FScopeLock Guard(&Ran->Lock);
			if (Index == 0)
			{
				Ran->FirstLatency = FPlatformTime::Seconds() - Sent;
			}
			Ran->Order.Add(Index);
			Ran->Threads.Add(FPlatformTLS::GetCurrentThreadId());
			++Ran->Count;
		});
	}
	TestTrue(TEXT("every command ran"), VaRest::Http::Testing::PumpUntil([Ran] { return Ran->Count.load() == 100; }, 5.0));
	FScopeLock Held(&Ran->Lock);
	bool bInOrder = Ran->Order.Num() == 100;
	for (int32 Index = 0; bInOrder && Index < 100; ++Index)
	{
		bInOrder = Ran->Order[Index] == Index;
	}
	TestTrue(TEXT("in the order they were queued"), bInOrder);
	TestEqual(TEXT("on one thread"), Ran->Threads.Num(), 1);
	TestFalse(TEXT("which is not the game thread"), Ran->Threads.Contains(GGameThreadId));
	TestTrue(FString::Printf(TEXT("woken at once, not after the 30 s wait (%.2f ms)"), Ran->FirstLatency * 1000.0), Ran->FirstLatency >= 0.0 && Ran->FirstLatency < 1.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceWaitTest, "VaRest.WebSocket.Lws.Service.Wait",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceWaitTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	// Not started: the game thread owns its state, so ComputeWaitMs can be read directly
	FService Service{ FServiceOptions() };
	TestEqual(TEXT("no connection: the idle wait"), Service.ComputeWaitMs(100.0), 30000);
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	Service.Adopt(MakeUnique<FStandIn>(1, 100.25, false, Log));
	TestTrue(TEXT("Adopt starts the connection"), Log->bStarted.load());
	TestEqual(TEXT("a deadline 250 ms ahead: 250 ms"), Service.ComputeWaitMs(100.0), 250);
	TestEqual(TEXT("one 5 s ahead: the 1 s cap (spec 2.1)"), Service.ComputeWaitMs(95.25), 1000);
	TestEqual(TEXT("a deadline passed: no wait"), Service.ComputeWaitMs(101.0), 0);
	Service.Adopt(MakeUnique<FStandIn>(2, TNumericLimits<double>::Max(), true, Log));
	TestEqual(TEXT("a kill in flight: the 100 ms cap (plan decision 12)"), Service.ComputeWaitMs(95.25), 100);
	TestNotNull(TEXT("Find by id"), Service.Find(2));
	TestNull(TEXT("... and only those"), Service.Find(3));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceDeadlinesTest, "VaRest.WebSocket.Lws.Service.Deadlines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceDeadlinesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Service->Shutdown();
	};
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	const double Deadline = FPlatformTime::Seconds() + 0.3;
	Service->Enqueue([Log, Deadline](FService& Self) { Self.Adopt(MakeUnique<FStandIn>(7, Deadline, false, Log)); });
	TestTrue(TEXT("the deadline is checked after lws_service returns, on time"), VaRest::Http::Testing::PumpUntil([Log] { return Log->FiredLate.load() >= 0.0; }, 5.0));
	// 250 ms, not 100: the service thread runs below normal priority (UnixPlatformProcess.cpp:2623-2625) on a host that may be
	// building (review conc M-8)
	TestTrue(FString::Printf(TEXT("... within 250 ms of it (%.1f ms late)"), Log->FiredLate.load() * 1000.0), Log->FiredLate.load() < 0.25);
	TestTrue(TEXT("a destroyed connection is erased"), VaRest::Http::Testing::PumpUntil([Service] { return Service->GetCounters().LiveConnections.load() == 0; }, 5.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceShutdownTest, "VaRest.WebSocket.Lws.Service.Shutdown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceShutdownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	Service->Enqueue([Log](FService& Self) { Self.Adopt(MakeUnique<FStandIn>(9, TNumericLimits<double>::Max(), false, Log)); });
	TestTrue(TEXT("adopted"), VaRest::Http::Testing::PumpUntil([Log] { return Log->bStarted.load(); }, 5.0));
	Service->Shutdown();
	TestEqual(TEXT("Shutdown ends each connection once (its terminal event)"), Log->ShutdownEnds.load(), 1);
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Late = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Service->Enqueue([Late](FService& Self) { *Late = true; });
	// Review conc M-7: nothing would run a queued command either, so check that it was let go at once -- its capture released
	TestEqual(TEXT("a command after Shutdown is dropped at once (its capture released)"), Late.GetSharedReferenceCount(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceFlushTest, "VaRest.WebSocket.Lws.Service.ShutdownFlushesCloses",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceFlushTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsServiceTestPrivate;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	if (!TestTrue(TEXT("the service starts"), Service->Start()))
	{
		return false;
	}
	// Review conc M-3: a close asked for just before Shutdown (the hub's 1001 at engine exit) is written 50 ms later; Shutdown,
	// called at once, waits for it (ExitFlushSeconds 0.25 by default) instead of stopping the thread first
	const TSharedRef<FStandInLog, ESPMode::ThreadSafe> Log = MakeShared<FStandInLog, ESPMode::ThreadSafe>();
	const double Deadline = FPlatformTime::Seconds() + 0.05;
	Service->Enqueue([Log, Deadline](FService& Self)
	{
		Self.GetCounters().UnsentCloses.fetch_add(1);
		TUniquePtr<FStandIn> Closing = MakeUnique<FStandIn>(11, Deadline, false, Log);
		Closing->bWritesCloseOnFire = true;
		Self.Adopt(MoveTemp(Closing));
	});
	const double Before = FPlatformTime::Seconds();
	Service->Shutdown();
	const double Took = FPlatformTime::Seconds() - Before;
	TestTrue(TEXT("the close went out before the thread stopped"), Log->FiredLate.load() >= 0.0);
	TestTrue(FString::Printf(TEXT("... and Shutdown waited no longer than ExitFlushSeconds (%.0f ms)"), Took * 1000.0), Took < 0.25 + 0.1);
	TestEqual(TEXT("nothing is left unsent"), Service->GetCounters().UnsentCloses.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceStartLatchTest, "VaRest.WebSocket.Lws.Service.StartFailureLatched",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceStartLatchTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	// Review conc I-2: exactly one failure, so exactly one line
	AddExpectedErrorPlain(TEXT("the service thread could not start"), EAutomationExpectedErrorFlags::Contains, 1);
	FServiceOptions Options;
	Options.bFailThreadForTest = true;
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(MoveTemp(Options));
	TestFalse(TEXT("the start fails"), Service->Start());
	TestFalse(TEXT("... and so does the next, at once"), Service->Start());
	TestEqual(TEXT("one lws_create_context, never a second (VaRest-rti)"), Service->GetCounters().ContextCreations.load(), 1);
	TestNull(TEXT("no context is left"), Service->GetContext());
	Service->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceHandshakeGateTest, "VaRest.WebSocket.Lws.Service.RefusesCreateUnderHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceHandshakeGateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedErrorPlain(TEXT("VaRest wss handshake(s) are in flight"), EAutomationExpectedErrorFlags::Contains, 1);
	const TSharedRef<FService, ESPMode::ThreadSafe> Service = MakeShared<FService, ESPMode::ThreadSafe>(FServiceOptions());
	{
		// Review conc I-3: a VaRest wss attempt of another service is in flight
		TlsHandshakesInFlight().fetch_add(1);
		ON_SCOPE_EXIT
		{
			TlsHandshakesInFlight().fetch_sub(1);
		};
		TestFalse(TEXT("no context is made while a wss handshake is in flight"), Service->Start());
		TestEqual(TEXT("... not even tried"), Service->GetCounters().ContextCreations.load(), 0);
	}
	TestTrue(TEXT("once it is done, the same service starts: the refusal is not latched"), Service->Start());
	Service->Shutdown();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceLogLevelTest, "VaRest.WebSocket.Lws.Service.LogLevel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceLogLevelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
#if IS_MONOLITHIC || UE_MERGED_MODULES
	AddInfo(TEXT("a shared LWS copy: the level is the engine's when WebSockets is loaded (D30); checked in W1b's monolithic run"));
#else
	ApplyLogLevel();
	// D30 (spec 1.2): VaRest's own copy logs errors and warnings only [coexist.shared_lws_state] (lwsl_visible, libwebsockets.h:434-443)
	TestTrue(TEXT("errors are visible"), lwsl_visible(LLL_ERR) != 0);
	TestTrue(TEXT("warnings are visible"), lwsl_visible(LLL_WARN) != 0);
	TestEqual(TEXT("notices are not (LWS's default has them, libwebsockets.h:413-414)"), lwsl_visible(LLL_NOTICE), 0);
	TestEqual(TEXT("client lines are not"), lwsl_visible(LLL_CLIENT), 0);
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsServiceStartupPolicyTest, "VaRest.WebSocket.Lws.Service.StartupPolicy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsServiceStartupPolicyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	// Plan decision 8 (VaRest-rti)
	const auto Nothing = [](const TCHAR*) { return false; };
	const auto WebSockets = [](const TCHAR* Name) { return FCString::Strcmp(Name, TEXT("WebSockets")) == 0; };
	const auto Replay = [](const TCHAR* Name) { return FCString::Strcmp(Name, TEXT("HttpNetworkReplayStreaming")) == 0; };
	TestFalse(TEXT("a modular build: lazily, VaRest's copy is its own"), ShouldCreateAtStartup(false, WebSockets));
	TestFalse(TEXT("a shared copy with no other LWS module: lazily"), ShouldCreateAtStartup(true, Nothing));
	TestTrue(TEXT("a shared copy with WebSockets: at startup"), ShouldCreateAtStartup(true, WebSockets));
	TestTrue(TEXT("... or with any other LWS module"), ShouldCreateAtStartup(true, Replay));
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.WebSocket.Lws.Service`
Expected: FAIL to compile (`VaRestLwsService.h` not found).

- [ ] **Step 3: The header** — create `Source/VaRestWebSocket/Private/VaRestLwsService.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Queue.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "Misc/SingleThreadRunnable.h"
#include "Templates/Function.h"
#include "Templates/UniquePtr.h"
#include "VaRestLwsInclude.h"

#include <atomic>

class FRunnableThread;
class ISslCertificateManager;
class ISslManager;

namespace VaRest::WebSocket::Lws
{
	class FService;

	/** LWS's own close-ack wait after our close frame, fixed in 3.0 (T20: ops-ws.c.o rops_handle_POLLOUT_ws 0x103c-0x104a) */
	inline constexpr double LwsCloseAckSeconds = 5.0;
	/**
	 * A pending kill that has not landed this long after it was armed is armed again (review conc I-1, re-review N-1): one
	 * timeout walk -- LWS walks only when the wall-clock second changed (T26) -- plus a margin. Shorter re-arms postpone the kill
	 */
	inline constexpr double KillRearmSeconds = 1.25;
	/** A message goes out in fragments of at most this many bytes (spec 2.4, 15) */
	inline constexpr int32 FragmentBytes = 64 * 1024;
	/** The one protocol's receive buffer, the engine's (LwsWebSocketsManager.cpp:166; spec 2.1) */
	inline constexpr int32 RxBufferBytes = 64 * 1024;
	/** The handshake header buffer, the engine's (LwsWebSocketsManager.cpp:187-190; spec 15) */
	inline constexpr int32 HeaderBufferBytes = 32 * 1024;

	struct FServiceOptions
	{
		/** lws_service's wait with no connection: commands wake it (lws_cancel_service) */
		int32 IdleWaitMs = 30000;
		/** ... with connections: LWS's own timers run only when lws_service returns (spec 2.1, T26) */
		int32 ActiveWaitCapMs = 1000;
		/** ... while a kill is in flight or LWS holds a partial write (plan decision 12, VaRest-wcv) */
		int32 ShortWaitCapMs = 100;
		/** Tests: PEM certificates trusted on top of the engine's roots (the test CA) */
		TArray<uint8> ExtraTrustedPem;
		/** Tests: every connection's TCP goes here; the URL's host stays the Host header and the TLS name (plan decision 19) */
		FString TcpAddressOverride;
		/** Tests: the URL filter's map instead of the engine ini (plan decision 19); read by FConn::Prepare */
		bool bOverrideAllowlist = false;
		TArray<TPair<FString, TArray<FString>>> Allowlist;
		FString ThreadName = TEXT("VaRestLws");
		/**
		 * Shutdown first lets the running thread write the close frames already asked for, at most this long (review conc
		 * M-3): at engine exit the hub's 1001 closes (spec 1.4) are queued by the game instances' shutdown
		 * (GameEngine.cpp:1377, :1391) just before OnEnginePreExit (:1396, UnrealEngine.cpp:2709) shuts the transport down
		 */
		double ExitFlushSeconds = 0.25;
		/** Tests (review conc I-2): Start makes the context, then fails as if the thread could not be made */
		bool bFailThreadForTest = false;
	};

	/** What a test reads (any thread) */
	struct FServiceCounters
	{
		/** Connections the service holds: adopted and not yet destroyed by LWS */
		std::atomic<int32> LiveConnections{ 0 };
		std::atomic<int32> KillsIssued{ 0 };
		std::atomic<int32> KillsLanded{ 0 };
		std::atomic<int32> CommandsRun{ 0 };
		/** lws_create_context calls this service made, failed ones included (review conc I-2) */
		std::atomic<int32> ContextCreations{ 0 };
		/** Open connections whose close was asked for and not written yet: Shutdown's flush waits for 0 (review conc M-3) */
		std::atomic<int32> UnsentCloses{ 0 };
		/** Seconds from lws_set_timeout(LWS_TO_KILL_ASYNC) to WSI_DESTROY; -1 before the first (VaRest-wcv) */
		std::atomic<double> LastKillToDestroySeconds{ -1.0 };
		std::atomic<double> MaxKillToDestroySeconds{ -1.0 };
	};

	/** One connection as the service sees it: FConn (Task 7), or a test's stand-in. Service thread, except EndForShutdown */
	class IServiceConnection
	{
	public:
		virtual ~IServiceConnection() = default;
		virtual uint64 GetId() const = 0;
		/** Right after Adopt: connects, or reports why it cannot */
		virtual void Start(FService& Service) = 0;
		/** A libwebsockets callback the service matched to this connection by its userdata */
		virtual int32 OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len) = 0;
		/** After each return of lws_service, outside any callback */
		virtual void CheckDeadlines(FService& Service, double Now) = 0;
		/** The earliest FPlatformTime::Seconds() at which CheckDeadlines has work; TNumericLimits<double>::Max() for none */
		virtual double NextDeadline() const = 0;
		/** A kill in flight or a partial write held: the wait is capped short (plan decision 12) */
		virtual bool WantsShortWait() const = 0;
		/** LWS is done with it (WSI_DESTROY, a scrubbed connect, or never connected): the service erases it */
		virtual bool IsDestroyed() const = 0;
		/** Shutdown, on the game thread with the service thread stopped: queue the terminal event if there is none yet */
		virtual void EndForShutdown() = 0;
	};

	using FCommand = TUniqueFunction<void(FService&)>;

	/**
	 * One lws_context and the thread that services it (WebSocket spec 2.1). The thread waits in lws_service, runs commands
	 * inside LWS_CALLBACK_EVENT_WAIT_CANCELLED (T13, T24), and checks VaRest's own deadlines after each return. Only
	 * lws_cancel_service is called from another thread; the context is made and destroyed on the game thread with the
	 * thread stopped (LwsWebSocketsManager.cpp:231, :298-309). A service starts once: after Shutdown it never runs again.
	 */
	class FService final : public FRunnable, public FSingleThreadRunnable
	{
	public:
		explicit FService(FServiceOptions InOptions);
		virtual ~FService() override;
		FService(const FService&) = delete;
		FService& operator=(const FService&) = delete;

		/**
		 * Game thread: the context and the thread; false (logged) when either could not be made -- and from then on, without
		 * another lws_create_context (review conc I-2: in a shared LWS copy a retry could run under an engine wss handshake,
		 * VaRest-rti). False, logged and not latched, while TlsHandshakesInFlight() is not 0 (review conc I-3)
		 */
		bool Start();
		/**
		 * Game thread: lets the thread write the close frames already asked for (at most ExitFlushSeconds), stops it, runs the
		 * queued commands, ends every connection (EndForShutdown), destroys the context
		 */
		void Shutdown();
		/** Any thread */
		bool IsRunning() const { return bAccepting.load(); }
		/** Any thread: Shutdown has begun (FConn::Start reports Shutdown then) */
		bool IsShuttingDown() const { return bShuttingDown.load(); }
		/** Any thread: Command runs on the service thread; dropped once the service stops accepting */
		void Enqueue(FCommand&& Command);

		// ---- service thread (or the game thread while the thread does not run)
		lws_context* GetContext() const { return Context; }
		const FServiceOptions& GetOptions() const { return Options; }
		ISslCertificateManager* GetCertificates() const { return Certificates; }
		FServiceCounters& GetCounters() { return Counters; }
		/** Takes the connection, makes its userdata known, and starts it */
		void Adopt(TUniquePtr<IServiceConnection>&& Connection);
		IServiceConnection* Find(uint64 Id) const;
		/** lws_service's next wait (plan decision 12); public for the service's tests */
		int32 ComputeWaitMs(double Now) const;

		/** The protocol's callback (lws_protocols::callback): the service comes from the context's user pointer */
		static int Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len);

	private:
		virtual uint32 Run() override;
		virtual void Stop() override;
		virtual FSingleThreadRunnable* GetSingleThreadInterface() override { return this; }
		/** FSingleThreadRunnable: where there is no multithreading, the main thread ticks the forkable thread (Fork.h:86-91) */
		virtual void Tick() override;
		void ServiceOnce(int32 WaitMs);
		/** Game thread, the thread running: until every close asked for before the call is written, or Seconds pass (review conc M-3) */
		void FlushCloses(double Seconds);
		void DrainCommands();
		void CheckDeadlines(double Now);
		void EraseDestroyed();
		int32 Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len);
		IServiceConnection* FindForCallback(lws* Wsi, void* User, int32 Reason) const;
		void LoadClientCertificates(SSL_CTX* Ctx);
		void AddExtraTrusted(SSL_CTX* Ctx) const;
		void ReleaseSsl();

		FServiceOptions Options;
		lws_context* Context = nullptr;
		lws_protocols Protocols[2] = {};
		FRunnableThread* Thread = nullptr;
		std::atomic<bool> bStopping{ false };
		std::atomic<bool> bAccepting{ false };
		std::atomic<bool> bShuttingDown{ false };
		/** Game thread: a Start failed; Start never tries again (review conc I-2) */
		bool bStartFailed = false;
		/** Held by Enqueue around its check and lws_cancel_service, and by Shutdown when it stops accepting: the context is never destroyed under an Enqueue */
		FCriticalSection EnqueueLock;
		TQueue<FCommand, EQueueMode::Mpsc> Commands;
		TMap<uint64, TUniquePtr<IServiceConnection>> Connections;
		/** The userdata pointers LWS may hand back: a callback whose userdata is not here belongs to no live connection */
		TSet<const IServiceConnection*> Live;
		ISslCertificateManager* Certificates = nullptr;
		ISslManager* SslManager = nullptr;
		SSL_CTX* ProvidedSslCtx = nullptr;
		bool bSslInitialized = false;
		TArray<ANSICHAR> ProxyUtf8;
		FServiceCounters Counters;
	};

	/** Any thread: Text as NUL-terminated UTF-8 in Out, for the char* fields libwebsockets keeps */
	void Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out);

	/** Game thread: the D30 rule for this module's LWS copy (spec 1.2; plan decision 16) */
	void ApplyLogLevel();

	/** Plan decision 8 (VaRest-rti): whether VaRest's context is made at module startup */
	bool ShouldCreateAtStartup(bool bSharedCopy, TFunctionRef<bool(const TCHAR*)> IsLinked);

	/**
	 * Any thread: VaRest's wss attempts, across every service of this module, that LWS has neither established
	 * (CLIENT_ESTABLISHED) nor destroyed (WSI_DESTROY) -- raised by FTransport::Connect on the game thread, where
	 * FService::Start reads it (re-review N-2), lowered once by the FConn (review conc I-3, VaRest-rti).
	 * Every lws_create_context re-allocates this LWS copy's OpenSSL ex-data index that their verify callbacks read
	 * unchecked [coexist.context_create_reallocates_ssl_index], so FService::Start refuses while it is not 0
	 */
	std::atomic<int32>& TlsHandshakesInFlight();
}
```

- [ ] **Step 4: The implementation** — create `Source/VaRestWebSocket/Private/VaRestLwsService.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsService.h"

#include "HAL/PlatformTime.h"
#include "HAL/RunnableThread.h"
#include "Misc/Fork.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "HAL/PlatformProcess.h"
#include "PlatformHttp.h"
#include "Ssl.h"
#include "VaRestSocketTransport.h"
#include "VaRestSocketTypes.h"
#include "VaRestWebSocketLog.h"

// VaRestSocketTransport.h for MaxConnectTimeoutSeconds: this file must compile alone, outside a unity blob (review spec I-2b)
static_assert(VaRest::WebSocket::FrameHeadroom >= LWS_PRE, "FVaRestSocketFrame's headroom must hold libwebsockets' frame header (libwebsockets.h:4773)");

namespace VaRest::WebSocket::Lws::VaRestLwsServicePrivate
{
	/** One libwebsockets line for UE's log: trailing white space dropped, controls shown as '?' (a server's bytes may be in it) */
	FString CleanLine(const char* Line)
	{
		FString Text = Line ? FString(UTF8_TO_TCHAR(Line)) : FString();
		Text.TrimEndInline();
		for (TCHAR& Char : Text)
		{
			if (Char < 0x20 || Char == 0x7F)
			{
				Char = TEXT('?');
			}
		}
		return Text;
	}

	/** VaRest's emitter for its LWS copy: errors and warnings, both Warnings, as the engine reports its own (LwsWebSocketsManager.cpp:405-418) */
	void EmitLwsLine(int Level, const char* Line)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("libwebsockets(%s): %s"), Level == LLL_ERR ? TEXT("err") : TEXT("warn"), *CleanLine(Line));
	}
}

void VaRest::WebSocket::Lws::Utf8Copy(const FString& Text, TArray<ANSICHAR>& Out)
{
	const FTCHARToUTF8 Converted(*Text);
	Out.SetNumUninitialized(Converted.Length() + 1);
	if (Converted.Length() > 0)
	{
		FMemory::Memcpy(Out.GetData(), Converted.Get(), Converted.Length());
	}
	Out[Converted.Length()] = '\0';
}

void VaRest::WebSocket::Lws::ApplyLogLevel()
{
	// D30 (spec 1.2): VaRest's own LWS copy in modular builds, always; the one copy a monolithic or merged build shares, only
	// while the engine's WebSockets module is not loaded. A WebSockets module that loads later sets its own level and emitter
	// (LwsWebSocketsManager.cpp:174-176), and VaRest's lines then go to LogWebSockets: never both in one copy
	constexpr bool bSharedCopy = IS_MONOLITHIC || UE_MERGED_MODULES;
	if (!bSharedCopy || !FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets")))
	{
		lws_set_log_level(LLL_ERR | LLL_WARN, &VaRestLwsServicePrivate::EmitLwsLine);
	}
}

bool VaRest::WebSocket::Lws::ShouldCreateAtStartup(bool bSharedCopy, TFunctionRef<bool(const TCHAR*)> IsLinked)
{
	if (!bSharedCopy)
	{
		return false;
	}
	// The engine modules that link libwebsockets and make their own contexts (spec 1.2)
	for (const TCHAR* Name : { TEXT("WebSockets"), TEXT("WebSocketServer"), TEXT("WebSocketNetworking"), TEXT("HttpNetworkReplayStreaming") })
	{
		if (IsLinked(Name))
		{
			return true;
		}
	}
	return false;
}

std::atomic<int32>& VaRest::WebSocket::Lws::TlsHandshakesInFlight()
{
	static std::atomic<int32> Count{ 0 };
	return Count;
}

VaRest::WebSocket::Lws::FService::FService(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

VaRest::WebSocket::Lws::FService::~FService()
{
	// The owner shuts a running service down on the game thread first (FTransport::Shutdown); this is the backstop
	if (Thread || Context)
	{
		check(IsInGameThread());
		Shutdown();
	}
}

bool VaRest::WebSocket::Lws::FService::Start()
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsServicePrivate;
	check(IsInGameThread());
	if (Context)
	{
		return true;
	}
	if (bShuttingDown.load() || bStartFailed)
	{
		// Review conc I-2: never a second lws_create_context after a failed start -- in a shared LWS copy a retry could run
		// while an engine wss handshake is in flight (VaRest-rti, ruling Q2)
		return false;
	}
	if (const int32 InFlight = TlsHandshakesInFlight().load(); InFlight != 0)
	{
		// Review conc I-3: the creation would re-allocate the ex-data index those handshakes' verify callbacks read
		// [coexist.context_create_reallocates_ssl_index]. Not latched: nothing was created, and the next Start may run
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: no context is made while %d VaRest wss handshake(s) are in flight (VaRest-rti); WebSocket connections end with Shutdown"), InFlight);
		return false;
	}
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	ApplyLogLevel();
	// One protocol, "varest" (spec 2.1). per_session_data_size 0: the connection's userdata is ours (connect_info.userdata),
	// as the engine's (LwsWebSocketsManager.cpp:163-166)
	Protocols[0].name = "varest";
	Protocols[0].callback = &FService::Callback;
	Protocols[0].per_session_data_size = 0;
	Protocols[0].rx_buffer_size = RxBufferBytes;

	lws_context_creation_info Info = {};
	Info.port = CONTEXT_PORT_NO_LISTEN;
	Info.protocols = Protocols;
	Info.uid = -1;
	Info.gid = -1;
	// The engine's options (LwsWebSocketsManager.cpp:184) and JUST_USE_RAW_ORIGIN (T4, libwebsockets.h:2618): .origin as given
	Info.options = LWS_SERVER_OPTION_PEER_CERT_NOT_REQUIRED | LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS | LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN;
	Info.max_http_header_data2 = HeaderBufferBytes;
	Info.pt_serv_buf_size = HeaderBufferBytes;
	// Spec 2.1, 5: VaRest's ping is per connection (W1b); LWS's own is off
	Info.ws_ping_pong_interval = 0;
	// Spec 2.2: context-wide, the backstop of every handshake stage [timeout.configured_secs]; each socket's shorter deadline is VaRest's kill
	Info.timeout_secs = static_cast<unsigned int>(VaRest::WebSocket::MaxConnectTimeoutSeconds);
	// Callback() finds this service through lws_context_user(lws_get_context(wsi)), also inside lws_create_context
	Info.user = this;
	// The engine's proxy setting, command line or ini (LwsWebSocketsManager.cpp:196-203; GenericPlatformHttp.cpp:640-651)
	const FString Proxy = FPlatformHttp::GetConfiguredProxyAddress();
	if (!Proxy.IsEmpty())
	{
		Utf8Copy(Proxy, ProxyUtf8);
		Info.http_proxy_address = ProxyUtf8.GetData();
	}
	FSslModule& Ssl = FModuleManager::LoadModuleChecked<FSslModule>(TEXT("SSL"));
	Certificates = &Ssl.GetCertificateManager();
	SslManager = &Ssl.GetSslManager();
	// The engine's order (LwsWebSocketsManager.cpp:205-225): the SSL module's context where InitializeSsl succeeds -- monolithic
	// and merged builds only (SslManager.cpp:26-55) -- else LWS's own, which raises LOAD_EXTRA_CLIENT_VERIFY_CERTS (T23)
	if (SslManager->InitializeSsl())
	{
		bSslInitialized = true;
		ProvidedSslCtx = SslManager->CreateSslContext(FSslContextCreateOptions());
	}
	if (ProvidedSslCtx)
	{
		// Its roots come from CreateSslContext itself (bAddCertificates, ISslManager.h:46-47) [tls.provided_ctx_load_extra_fired]
		AddExtraTrusted(ProvidedSslCtx);
		Info.provided_client_ssl_ctx = ProvidedSslCtx;
	}
	else
	{
		Info.options |= LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT;
	}
	Context = lws_create_context(&Info);
	Counters.ContextCreations.fetch_add(1);
	if (!Context)
	{
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: lws_create_context failed; WebSocket connections end with Shutdown"));
		ReleaseSsl();
		bStartFailed = true;
		return false;
	}
	bStopping = false;
	{
		FScopeLock Guard(&EnqueueLock);
		bAccepting = true;
	}
	// 128 KiB, below normal, forkable: the engine's service thread (LwsWebSocketsManager.cpp:239-247)
	Thread = Options.bFailThreadForTest ? nullptr
		: FForkProcessHelper::CreateForkableThread(this, *Options.ThreadName, 128 * 1024, TPri_BelowNormal, FPlatformAffinity::GetNoAffinityMask());
	if (!Thread)
	{
		bStartFailed = true;
		UE_LOG(LogVaRestSocket, Error, TEXT("libwebsockets: the service thread could not start; WebSocket connections end with Shutdown"));
		{
			FScopeLock Guard(&EnqueueLock);
			bAccepting = false;
		}
		lws_context_destroy(Context);
		Context = nullptr;
		ReleaseSsl();
		return false;
	}
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets %s: context and thread %s started"), UTF8_TO_TCHAR(lws_get_library_version()), *Options.ThreadName);
	return true;
}

void VaRest::WebSocket::Lws::FService::Shutdown()
{
	check(IsInGameThread());
	// Review conc M-3: the close frames already asked for (the hub's 1001 at engine exit, spec 1.4) go out first, while the
	// thread still runs. Only where it is a real thread: the main thread ticks it otherwise (Fork.h:86-102), and does not here
	if (Thread && bAccepting.load() && Options.ExitFlushSeconds > 0.0 && FPlatformProcess::SupportsMultithreading())
	{
		FlushCloses(Options.ExitFlushSeconds);
	}
	{
		FScopeLock Guard(&EnqueueLock);
		bAccepting = false;
	}
	bShuttingDown = true;
	if (Thread)
	{
		// Kill(true) calls Stop() and waits for Run() to return: the engine's order (LwsWebSocketsManager.cpp:298-303)
		Thread->Kill(true);
		delete Thread;
		Thread = nullptr;
	}
	// The thread is gone: its state is the game thread's now. Queued commands run (a connect among them reports Shutdown)
	DrainCommands();
	for (TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Pair.Value->EndForShutdown();
	}
	if (Context)
	{
		// After the thread, on the game thread (LwsWebSocketsManager.cpp:305-309); the connections' last callbacks
		// (CLIENT_CLOSED, WSI_DESTROY) arrive inside it, here, and each has reported already
		lws_context_destroy(Context);
		Context = nullptr;
	}
	Connections.Empty();
	Live.Empty();
	Counters.LiveConnections = 0;
	ReleaseSsl();
}

void VaRest::WebSocket::Lws::FService::FlushCloses(double Seconds)
{
	// A marker queued after every command so far: once it has run, UnsentCloses counts each close asked for before this call
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> Reached = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	Enqueue([Reached](FService& Self) { *Reached = true; });
	const double Until = FPlatformTime::Seconds() + Seconds;
	while (FPlatformTime::Seconds() < Until && (!Reached->load() || Counters.UnsentCloses.load() > 0))
	{
		FPlatformProcess::SleepNoStats(0.001f);
	}
}

void VaRest::WebSocket::Lws::FService::Enqueue(FCommand&& Command)
{
	FScopeLock Guard(&EnqueueLock);
	if (!bAccepting.load())
	{
		return;
	}
	Commands.Enqueue(MoveTemp(Command));
	// The one LWS call another thread may make (T13, libwebsockets.h:3704-3716): the command runs in EVENT_WAIT_CANCELLED (T24)
	lws_cancel_service(Context);
}

void VaRest::WebSocket::Lws::FService::Adopt(TUniquePtr<IServiceConnection>&& Connection)
{
	IServiceConnection* Raw = Connection.Get();
	Live.Add(Raw);
	Connections.Add(Raw->GetId(), MoveTemp(Connection));
	Counters.LiveConnections = Connections.Num();
	Raw->Start(*this);
}

VaRest::WebSocket::Lws::IServiceConnection* VaRest::WebSocket::Lws::FService::Find(uint64 Id) const
{
	const TUniquePtr<IServiceConnection>* Found = Connections.Find(Id);
	return Found ? Found->Get() : nullptr;
}

int32 VaRest::WebSocket::Lws::FService::ComputeWaitMs(double Now) const
{
	if (Connections.Num() == 0)
	{
		return Options.IdleWaitMs;
	}
	double Next = TNumericLimits<double>::Max();
	bool bShort = false;
	for (const TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Next = FMath::Min(Next, Pair.Value->NextDeadline());
		bShort = bShort || Pair.Value->WantsShortWait();
	}
	const int32 Cap = bShort ? Options.ShortWaitCapMs : Options.ActiveWaitCapMs;
	if (Next == TNumericLimits<double>::Max())
	{
		return Cap;
	}
	// Clamped as a double before the cast; rounded up, so a wait never ends just before its deadline and spins once more
	return FMath::CeilToInt32(FMath::Clamp((Next - Now) * 1000.0, 0.0, static_cast<double>(Cap)));
}

uint32 VaRest::WebSocket::Lws::FService::Run()
{
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	while (!bStopping.load())
	{
		ServiceOnce(ComputeWaitMs(FPlatformTime::Seconds()));
	}
	return 0;
}

void VaRest::WebSocket::Lws::FService::Stop()
{
	bStopping = true;
	if (Context)
	{
		lws_cancel_service(Context);
	}
}

void VaRest::WebSocket::Lws::FService::Tick()
{
	// No wait where the main thread ticks the service: the engine polls with 0 there too (LwsWebSocketsManager.cpp:509)
	if (Context && !bStopping.load())
	{
		ServiceOnce(0);
	}
}

void VaRest::WebSocket::Lws::FService::ServiceOnce(int32 WaitMs)
{
	lws_service(Context, WaitMs);
	// Spec 2.1: VaRest's deadlines after each return, outside any callback, where a kill and a queued event are safe
	CheckDeadlines(FPlatformTime::Seconds());
	EraseDestroyed();
}

void VaRest::WebSocket::Lws::FService::DrainCommands()
{
	FCommand Command;
	while (Commands.Dequeue(Command))
	{
		Command(*this);
		Counters.CommandsRun.fetch_add(1);
	}
}

void VaRest::WebSocket::Lws::FService::CheckDeadlines(double Now)
{
	for (TPair<uint64, TUniquePtr<IServiceConnection>>& Pair : Connections)
	{
		Pair.Value->CheckDeadlines(*this, Now);
	}
}

void VaRest::WebSocket::Lws::FService::EraseDestroyed()
{
	for (auto It = Connections.CreateIterator(); It; ++It)
	{
		if (It->Value->IsDestroyed())
		{
			// LWS calls back no more for it: WSI_DESTROY came, or there never was a wsi
			Live.Remove(It->Value.Get());
			It.RemoveCurrent();
		}
	}
	Counters.LiveConnections = Connections.Num();
}

int VaRest::WebSocket::Lws::FService::Callback(lws* Wsi, lws_callback_reasons Reason, void* User, void* In, size_t Len)
{
	// Also called inside lws_create_context (protocol init, client TLS setup), before Context is set: the service comes from
	// the context's user pointer (lws_context_creation_info::user), as the probe's rig does (VaRestLwsProbe.cpp:492-499)
	lws_context* Owner = Wsi ? lws_get_context(Wsi) : nullptr;
	FService* Self = Owner ? static_cast<FService*>(lws_context_user(Owner)) : nullptr;
	return Self ? Self->Handle(Wsi, static_cast<int32>(Reason), User, In, Len) : 0;
}

int32 VaRest::WebSocket::Lws::FService::Handle(lws* Wsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
		// lws_cancel_service lands here, on the service thread (libwebsockets.h:1551-1556): spec 2.1's command point
		DrainCommands();
		return 0;
	case LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS:
		// user is the SSL_CTX (libwebsockets.h:961-967); raised inside lws_create_context, on the game thread
		LoadClientCertificates(static_cast<SSL_CTX*>(User));
		return 0;
	default:
		break;
	}
	IServiceConnection* Connection = FindForCallback(Wsi, User, Reason);
	return Connection ? Connection->OnCallback(*this, Wsi, Reason, User, In, Len) : 0;
}

VaRest::WebSocket::Lws::IServiceConnection* VaRest::WebSocket::Lws::FService::FindForCallback(lws* Wsi, void* User, int32 Reason) const
{
	// The userdata, never the wsi: LWS 3.0 reuses a freed wsi's address for the next connection (bead VaRest-81s;
	// Probe.VaRest.Lws.Rig.SequentialConnections). PERFORM_SERVER_CERT_VERIFICATION's user is the X509_STORE_CTX, so the
	// userdata comes from lws_wsi_user there, as the engine does (LwsWebSocketsManager.cpp:447-456). T29: the others carry it in user
	if (Reason != LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION && User && Live.Contains(static_cast<const IServiceConnection*>(User)))
	{
		return static_cast<IServiceConnection*>(User);
	}
	void* WsiUser = Wsi ? lws_wsi_user(Wsi) : nullptr;
	return WsiUser && Live.Contains(static_cast<const IServiceConnection*>(WsiUser)) ? static_cast<IServiceConnection*>(WsiUser) : nullptr;
}

void VaRest::WebSocket::Lws::FService::LoadClientCertificates(SSL_CTX* Ctx)
{
	// The engine's roots (LwsWebSocketsManager.cpp:441-446): with LWS's own context they come only from here (T23, tls.load_extra_fired)
	if (Certificates && Ctx)
	{
		Certificates->AddCertificatesToSslContext(Ctx);
	}
	AddExtraTrusted(Ctx);
}

void VaRest::WebSocket::Lws::FService::AddExtraTrusted(SSL_CTX* Ctx) const
{
	if (!Ctx || Options.ExtraTrustedPem.Num() == 0)
	{
		return;
	}
	BIO* Bio = BIO_new_mem_buf(Options.ExtraTrustedPem.GetData(), Options.ExtraTrustedPem.Num());
	if (!Bio)
	{
		return;
	}
	X509_STORE* Store = SSL_CTX_get_cert_store(Ctx);
	int32 Added = 0;
	while (X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr))
	{
		Added += X509_STORE_add_cert(Store, Certificate) == 1 ? 1 : 0;
		X509_free(Certificate);
	}
	// The read that ends the loop leaves "no start line" on OpenSSL's error queue (VaRestLwsProbe.cpp:985-986)
	ERR_clear_error();
	BIO_free(Bio);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("libwebsockets: %d extra trusted certificate(s)"), Added);
}

void VaRest::WebSocket::Lws::FService::ReleaseSsl()
{
	// Modules unload last-loaded first (ModuleManager.cpp:1458): at module shutdown the SSL module, loaded after this one, may be gone
	if (!SslManager || !FModuleManager::Get().IsModuleLoaded(TEXT("SSL")))
	{
		ProvidedSslCtx = nullptr;
		bSslInitialized = false;
		return;
	}
	if (ProvidedSslCtx)
	{
		SslManager->DestroySslContext(ProvidedSslCtx);
		ProvidedSslCtx = nullptr;
	}
	if (bSslInitialized)
	{
		// One ShutdownSsl per successful InitializeSsl (ISslManager.h:58-69)
		SslManager->ShutdownSsl();
		bSslInitialized = false;
	}
}
```

- [ ] **Step 5: Run them**

Run: `Tools/run_tests.sh VaRest.WebSocket.Lws.Service`
Expected: `run_tests: 10 passed, 0 failed` (StartStop, Commands, Wait, Deadlines, Shutdown, ShutdownFlushesCloses, StartFailureLatched, RefusesCreateUnderHandshake, LogLevel, StartupPolicy).

- [ ] **Step 6: Restore-the-bug** — (a) in `Enqueue`, drop `lws_cancel_service(Context);`: `VaRest.WebSocket.Lws.Service.Commands` FAILS — the first command waits out the 30 s idle wait, past `PumpUntil`'s 5 s (the probe's restore found the same, 9.85 s, `ws-w0-progress.md` Task 4); restore; PASSES. (b) In `ServiceOnce`, drop `CheckDeadlines(...)`: `.Deadlines` FAILS ("checked after lws_service returns"); restore; PASSES. (c) In `ComputeWaitMs`, use `Options.ActiveWaitCapMs` whatever `bShort` says: `.Wait` FAILS at "a kill in flight: the 100 ms cap"; restore; PASSES. (d) Review conc M-7: in `Enqueue`, queue the command before the `bAccepting` check (`Commands.Enqueue(MoveTemp(Command));` first, then `if (!bAccepting.load()) { return; }` -- enqueue only, so no `lws_cancel_service` on a null context): `.Shutdown` FAILS at "dropped at once (its capture released)" (2); restore; PASSES. (e) Review conc I-2: delete `|| bStartFailed` from `Start`'s first check: `.StartFailureLatched` FAILS (two contexts, and the error line twice); restore; PASSES. (f) Review conc I-3: delete the `TlsHandshakesInFlight()` check: `.RefusesCreateUnderHandshake` FAILS at "no context is made while a wss handshake is in flight" (no real handshake is in flight, so nothing crashes); restore; PASSES. (g) Review conc M-3: delete the `FlushCloses(...)` call in `Shutdown`: `.ShutdownFlushesCloses` FAILS at "the close went out before the thread stopped"; restore; PASSES.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsService.h Source/VaRestWebSocket/Private/VaRestLwsService.cpp \
  Source/VaRestWebSocket/Private/Tests/VaRestLwsServiceTest.cpp
git commit -m "ws: W1a task 6 -- the LWS service: one context, one forkable thread, commands in EVENT_WAIT_CANCELLED, VaRest's deadlines after each lws_service, the D30 log rule, the startup policy (VaRest-rti)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 7: Connect, handshake, open, refusals — the connection, the transport, the write path, the test helpers

The first real connection: `FConn` connects with Task 4's target, appends the headers in the room LWS reports, reads the selected subprotocol, opens, writes whole messages and our close frame, and reports every pre-open end through Task 5's table — over ws and wss. `FTransport` hands out handles; the default transport and the module's startup policy (plan decision 8) come in here. Fragments are Task 8, the peer's close and `CloseTimeout` Task 9.

**Files:**
- Create: `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp`, `Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp`, `Source/VaRestWebSocket/Private/VaRestLwsTransport.h`, `Source/VaRestWebSocket/Private/VaRestLwsTransport.cpp`, `Source/VaRestWebSocket/Public/VaRestSocketTesting.h`, `Source/VaRestWebSocket/Private/VaRestSocketTesting.cpp`, `Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.h`, `Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.cpp`
- Modify: `Source/VaRestWebSocket/Public/VaRestSocketTransport.h` (`GetDefaultTransport`), `Source/VaRestWebSocket/Private/VaRestWebSocketModule.cpp` (the hooks), `Tools/run_tests.sh` (the editor runs without the proxy variables, review conc M-12)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsConnectTest.cpp`

**Interfaces:**
- Consumes: Tasks 2-6: `FVaRestSocketFrame`, `FVaRestSocketEvent`, `FSocketLink`, `PrepareTarget`, `IsAllowedByFilter`, `ClassifyConnectionError`, `ClassifyEmptyConnectionError`, `LwsConnectStageSeconds` (Task 5), `FService`, `IServiceConnection`, `FServiceOptions`, `FServiceCounters` (`UnsentCloses`, `ContextCreations`), `TlsHandshakesInFlight()`, `KillRearmSeconds`, `Utf8Copy`, `ShouldCreateAtStartup` (Task 6), `SanitizeCloseCode`, `CloseReasonBytes` (Task 2) (re-review A-17).
- Produces:
  - `VARESTWEBSOCKET_API TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> VaRest::WebSocket::GetDefaultTransport()` (any thread; its `Connect` is game thread)
  - `class VaRest::WebSocket::Lws::FConn final : public IServiceConnection` with `Prepare(const FServiceOptions&)` (game thread), `GetLabel()`, the `IServiceConnection` overrides, and the commands `Send(FService&, FVaRestSocketFrame&&)`, `Close(FService&, int32 Code, TArray<uint8>&& Reason, EVaRestSocketCloseBy, EVaRestSocketFailure)`, `Release(FService&)`, `static FConn* Find(FService&, uint64)` (service thread). Private state later tasks extend: `PendingFailure`/`PendingDetail`, `AppendCalls`, `Outbox`, `HeldBytes`, the close fields (`bCloseQueued`, `bCloseSent`, `bCloseCounted`, `CloseSentAt`, `CloseCode`, `CloseReason`, `CloseBy`, `CloseFailure`), `Owner` (the service, set by `Start`), `bTlsCounted`, `KillArmedAt`; public `NeedsTlsCount()`/`MarkTlsCounted()` (game thread, `FTransport::Connect` counts a wss attempt before queuing it, re-review N-2) and a destructor that uncounts; private functions `OnAppendHeaders`, `OnFilterPreEstablish`, `OnEstablished`, `OnWriteable`, `OnConnectionError`, `OnEnded`, `OnDestroyed`, `Kill`, `ReleaseHeld`, `DropOutbox`, `EndPreOpen`, `EndOpen`, `UncountTls`, `UncountClose`. An attempt in state `Ended` refuses `APPEND_HANDSHAKE_HEADER` and `FILTER_PRE_ESTABLISH` (-1) and every later task's certificate check (1); `CheckDeadlines` re-arms an issued kill that has not landed `KillRearmSeconds` after it was armed, before anything else (review conc I-1, re-review N-1) -- Tasks 9 and 11 keep both when they extend these functions.
  - `class VaRest::WebSocket::Lws::FTransport final : public IVaRestSocketTransport { explicit FTransport(FServiceOptions); Connect; bool EnsureStarted(); void Shutdown(); void ResetAfterFork(); const FServiceCounters* GetCounters() const; }`, `FTransport& GetDefault()`, `void OnModuleStartup()`, `void ShutdownDefault()`, `void ResetDefaultAfterFork()` (game thread)
  - `VaRestSocketTesting.h`: `struct FLwsTransportOptions { TArray<uint8> ExtraTrustedPem; FString TcpAddressOverride; bool bOverrideAllowlist; TArray<TPair<FString, TArray<FString>>> Allowlist; int32 ActiveWaitCapMs = 1000; int32 ShortWaitCapMs = 100; double ExitFlushSeconds = 0.25; bool bFailThreadForTest = false; }`, `struct FLwsCounters { int32 LiveConnections, KillsIssued, KillsLanded, ContextCreations, UnsentCloses; double LastKillToDestroySeconds, MaxKillToDestroySeconds; }`, `class ILwsTestTransport : public IVaRestSocketTransport { virtual void Shutdown() = 0; virtual FLwsCounters GetCounters() const = 0; }`, `MakeLwsTransport(FLwsTransportOptions)`, `DescribeLwsCopy()`, `GetLwsCreateContextAddress()`, `DescribeLwsLogLevel()`, `int32 GetTlsHandshakesInFlight()` (any thread), `class FScopedTlsHandshakeInFlight` (raises the module-wide count for its scope, review conc I-3) (all `VARESTWEBSOCKET_API`, game thread unless noted, `WITH_DEV_AUTOMATION_TESTS`)
  - `VaRestWsTestUtil.h` (namespace `VaRest::WsTest`): `FSite`, `FindSite`, `FScheme`, `Schemes`, `NewKey`, `MakeTransport`, `ExpectLwsLines`, `FEvents` (`WaitFor`, `WaitType`, `WaitEnd`, `WaitMessages`, `Find`, `GetEnd`, `GetMessages`, `Count`, `GetAll`, `Pull`, `Describe`, `bAcknowledge`, `Connection`), `Visit`, `IsError`, `FetchJournal`, `FetchBytes`, `JournalEvents`, `JournalHeader`, `JournalCloseCode`, `Request`, `Utf8`, `Sha256Hex`

- [ ] **Step 1: The test helpers** — create `Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"
#include "VaRestSocketTesting.h"
#include "VaRestSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

class FAutomationTestBase;

/** Helpers for VaRest.Integration.WebSocket.Transport.* (WebSocket spec 16.2) against Tools/testserver/ws.py */
namespace VaRest::WsTest
{
	/** The runner's test server: -VaRestTestServer=http://127.0.0.1:<port>, -VaRestTestServerTls=https://127.0.0.1:<port> */
	struct FSite
	{
		FString HttpBase;
		FString Host;
		int32 Port = 0;
		int32 TlsPort = 0;
	};

	/** AddError and false when the runner did not pass the server or (bNeedTls) its TLS listener: an integration test fails, never skips */
	bool FindSite(FAutomationTestBase& Test, FSite& Out, bool bNeedTls = true);

	/** One way to reach the server: ws://127.0.0.1:<port>, or wss://localhost:<tls port> (the name checked against the test CA, TCP to 127.0.0.1) */
	struct FScheme
	{
		const TCHAR* Name = TEXT("ws");
		FString Base;
		bool bTls = false;
	};
	TArray<FScheme> Schemes(const FSite& Site);

	FString NewKey();

	/**
	 * A transport of its own -- context and thread -- with TCP to the server's host (plan decision 19) and, with bTrustTestCa,
	 * the test CA trusted (GET /tls/ca.pem, fetched once). Null, with an AddError, when the CA could not be fetched. Shut it
	 * down before the test returns (ON_SCOPE_EXIT), and never make one while another transport still has a wss attempt that
	 * LWS has not destroyed (its LiveConnections back to 0): an ended attempt still runs its handshake (VaRest-rti; re-review N-5).
	 */
	TSharedPtr<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> MakeTransport(FAutomationTestBase& Test, const FSite& Site, bool bTrustTestCa = true);

	/** VaRest's libwebsockets lines are Warnings (plan decision 16); bElevateLogWarningsToErrors defaults to true (AutomationControllerSettings.cpp:14) */
	void ExpectLwsLines(FAutomationTestBase& Test);

	/** A connection's events, popped as the game thread is pumped; Message bytes are acknowledged (Consumed) as they are popped */
	class FEvents
	{
	public:
		explicit FEvents(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> InConnection);

		/** Pumps the game thread (and the core ticker) until Done holds over the events so far, or Seconds pass */
		bool WaitFor(TFunctionRef<bool(const TArray<FVaRestSocketEvent>&)> Done, double Seconds = 15.0);
		bool WaitType(EVaRestSocketEventType Type, double Seconds = 15.0);
		/** Until the terminal event (Closed or Error) */
		bool WaitEnd(double Seconds = 15.0);
		/** Until Wanted messages, or the end; true when Wanted arrived */
		bool WaitMessages(int32 Wanted, double Seconds = 15.0);
		/** Pops what is queued now, without pumping */
		void Pull();

		const FVaRestSocketEvent* Find(EVaRestSocketEventType Type) const;
		/** The terminal event, or null */
		const FVaRestSocketEvent* GetEnd() const;
		TArray<const FVaRestSocketEvent*> GetMessages() const;
		int32 Count(EVaRestSocketEventType Type) const;
		const TArray<FVaRestSocketEvent>& GetAll() const { return Seen; }
		/** "Open > Message x3 > Closed 1000 by Client" for a test's messages */
		FString Describe() const;

		/** false: the test acknowledges messages itself (flow control tests, W1b) */
		bool bAcknowledge = true;
		TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection;

	private:
		TArray<FVaRestSocketEvent> Seen;
	};

	/** Connects, waits for the open or the end, closes 1000 when it opened, waits for the end: every connection drains */
	TUniquePtr<FEvents> Visit(IVaRestSocketTransport& Transport, FVaRestSocketConnectRequest&& Req, double Seconds = 15.0);

	/** The connection ended with Error and this failure */
	bool IsError(const FEvents& Events, EVaRestSocketFailure Failure);

	FVaRestSocketConnectRequest Request(const FScheme& Scheme, const FString& PathAndQuery);

	/** GET /ws/journal over HTTP; an invalid handle and an AddError when it did not arrive */
	FVaRestJson FetchJournal(FAutomationTestBase& Test, const FSite& Site, const FString& Key, int32 Connections = 1, int32 WaitMs = 5000);
	bool FetchBytes(FAutomationTestBase& Test, const FSite& Site, const FString& Path, TArray<uint8>& Out);
	/** The journal's events of one kind ("ev"), of one connection (1-based) or all (0) */
	TArray<FVaRestJson> JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex = 0);
	/** A request header the server received (name compared ignoring case), or "(absent)" */
	FString JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex = 1);
	/** The code of the first close frame the server received from the client (-1: none; the code is absent for an empty payload), and its reason */
	int32 JournalCloseCode(const FVaRestJson& Journal, FString* OutReason = nullptr, int32 ConnIndex = 1);

	FString Utf8(TConstArrayView<uint8> Bytes);
	FString Sha256Hex(TConstArrayView<uint8> Bytes);
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

Create `Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestWsTestUtil.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Guid.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestSha256.h"

namespace VaRest::WsTest::VaRestWsTestUtilPrivate
{
	/** "http://127.0.0.1:41234" -> host and port (VaRestLwsProbe.cpp:25-45) */
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

	/** The test CA, fetched once per run */
	TArray<uint8>& CachedCa()
	{
		static TArray<uint8> Ca;
		return Ca;
	}

	bool IsOpenOrEnd(const FVaRestSocketEvent& Event)
	{
		return Event.Type == EVaRestSocketEventType::Open || Event.IsTerminal();
	}
}

bool VaRest::WsTest::FindSite(FAutomationTestBase& Test, FSite& Out, bool bNeedTls)
{
	using namespace VaRest::WsTest::VaRestWsTestUtilPrivate;
	Out.HttpBase = VaRest::Http::Testing::GetTestServerUrl();
	if (Out.HttpBase.IsEmpty() || !SplitUrl(Out.HttpBase, TEXT("http"), Out.Host, Out.Port))
	{
		Test.AddError(TEXT("no test server: run Tools/run_tests.sh, which starts Tools/testserver/server.py and passes -VaRestTestServer=<url> and -VaRestTestServerTls=<url>"));
		return false;
	}
	FString TlsHost;
	if (!SplitUrl(VaRest::Http::Testing::GetTestServerTlsUrl(), TEXT("https"), TlsHost, Out.TlsPort))
	{
		Out.TlsPort = 0;
	}
	if (bNeedTls && Out.TlsPort == 0)
	{
		Test.AddError(TEXT("no TLS listener: -VaRestTestServerTls=<url> is missing (Tools/run_tests.sh passes it; Tools/testserver/server.py --tls-port)"));
		return false;
	}
	return true;
}

TArray<VaRest::WsTest::FScheme> VaRest::WsTest::Schemes(const FSite& Site)
{
	TArray<FScheme> Out;
	Out.Add({ TEXT("ws"), FString::Printf(TEXT("ws://%s:%d"), *Site.Host, Site.Port), false });
	// Plan decision 19: the DNS-name path through LWS's own name check; MakeTransport sends the TCP to 127.0.0.1
	Out.Add({ TEXT("wss"), FString::Printf(TEXT("wss://localhost:%d"), Site.TlsPort), true });
	return Out;
}

FString VaRest::WsTest::NewKey()
{
	return FGuid::NewGuid().ToString(EGuidFormats::Digits);
}

TSharedPtr<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> VaRest::WsTest::MakeTransport(FAutomationTestBase& Test, const FSite& Site, bool bTrustTestCa)
{
	using namespace VaRest::WsTest::VaRestWsTestUtilPrivate;
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	if (bTrustTestCa)
	{
		TArray<uint8>& Ca = CachedCa();
		if (Ca.Num() == 0 && !FetchBytes(Test, Site, TEXT("/tls/ca.pem"), Ca))
		{
			return nullptr;
		}
		Options.ExtraTrustedPem = Ca;
	}
	return VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
}

void VaRest::WsTest::ExpectLwsLines(FAutomationTestBase& Test)
{
	Test.AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
}

VaRest::WsTest::FEvents::FEvents(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> InConnection)
	: Connection(MoveTemp(InConnection))
{
}

void VaRest::WsTest::FEvents::Pull()
{
	TArray<FVaRestSocketEvent> Got;
	Connection->PopEvents(Got);
	for (FVaRestSocketEvent& Event : Got)
	{
		if (bAcknowledge && Event.Type == EVaRestSocketEventType::Message)
		{
			Connection->Consumed(Event.Bytes.Num());
		}
		Seen.Add(MoveTemp(Event));
	}
}

bool VaRest::WsTest::FEvents::WaitFor(TFunctionRef<bool(const TArray<FVaRestSocketEvent>&)> Done, double Seconds)
{
	return VaRest::Http::Testing::PumpUntil([this, &Done] { Pull(); return Done(Seen); }, Seconds, true);
}

bool VaRest::WsTest::FEvents::WaitType(EVaRestSocketEventType Type, double Seconds)
{
	return WaitFor([Type](const TArray<FVaRestSocketEvent>& All)
	{
		return All.ContainsByPredicate([Type](const FVaRestSocketEvent& Each) { return Each.Type == Type; });
	}, Seconds);
}

bool VaRest::WsTest::FEvents::WaitEnd(double Seconds)
{
	return WaitFor([](const TArray<FVaRestSocketEvent>& All)
	{
		return All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
	}, Seconds);
}

bool VaRest::WsTest::FEvents::WaitMessages(int32 Wanted, double Seconds)
{
	WaitFor([Wanted](const TArray<FVaRestSocketEvent>& All)
	{
		int32 Messages = 0;
		for (const FVaRestSocketEvent& Each : All)
		{
			if (Each.IsTerminal())
			{
				return true;
			}
			Messages += Each.Type == EVaRestSocketEventType::Message ? 1 : 0;
		}
		return Messages >= Wanted;
	}, Seconds);
	return Count(EVaRestSocketEventType::Message) >= Wanted;
}

const FVaRestSocketEvent* VaRest::WsTest::FEvents::Find(EVaRestSocketEventType Type) const
{
	return Seen.FindByPredicate([Type](const FVaRestSocketEvent& Each) { return Each.Type == Type; });
}

const FVaRestSocketEvent* VaRest::WsTest::FEvents::GetEnd() const
{
	return Seen.FindByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
}

TArray<const FVaRestSocketEvent*> VaRest::WsTest::FEvents::GetMessages() const
{
	TArray<const FVaRestSocketEvent*> Out;
	for (const FVaRestSocketEvent& Each : Seen)
	{
		if (Each.Type == EVaRestSocketEventType::Message)
		{
			Out.Add(&Each);
		}
	}
	return Out;
}

int32 VaRest::WsTest::FEvents::Count(EVaRestSocketEventType Type) const
{
	int32 Out = 0;
	for (const FVaRestSocketEvent& Each : Seen)
	{
		Out += Each.Type == Type ? 1 : 0;
	}
	return Out;
}

FString VaRest::WsTest::FEvents::Describe() const
{
	TArray<FString> Parts;
	int32 Messages = 0;
	for (const FVaRestSocketEvent& Each : Seen)
	{
		if (Each.Type == EVaRestSocketEventType::Message)
		{
			++Messages;
			continue;
		}
		if (Messages > 0)
		{
			Parts.Add(FString::Printf(TEXT("Message x%d"), Messages));
			Messages = 0;
		}
		if (Each.IsTerminal())
		{
			Parts.Add(FString::Printf(TEXT("%s %d by %s, %s%s (%s)"), LexToString(Each.Type), Each.End.Code, LexToString(Each.End.By), LexToString(Each.End.Failure),
				Each.End.bWasClean ? TEXT(", clean") : TEXT(""), *Each.End.Detail));
		}
		else
		{
			Parts.Add(LexToString(Each.Type));
		}
	}
	if (Messages > 0)
	{
		Parts.Add(FString::Printf(TEXT("Message x%d"), Messages));
	}
	return Parts.Num() > 0 ? FString::Join(Parts, TEXT(" > ")) : FString(TEXT("(no events)"));
}

TUniquePtr<VaRest::WsTest::FEvents> VaRest::WsTest::Visit(IVaRestSocketTransport& Transport, FVaRestSocketConnectRequest&& Req, double Seconds)
{
	using namespace VaRest::WsTest::VaRestWsTestUtilPrivate;
	TUniquePtr<FEvents> Events = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
	Events->WaitFor([](const TArray<FVaRestSocketEvent>& All) { return All.ContainsByPredicate(&IsOpenOrEnd); }, Seconds);
	if (Events->Find(EVaRestSocketEventType::Open) && !Events->GetEnd())
	{
		Events->Connection->Close(1000, TEXT("done"));
	}
	Events->WaitEnd(Seconds);
	return Events;
}

bool VaRest::WsTest::IsError(const FEvents& Events, EVaRestSocketFailure Failure)
{
	const FVaRestSocketEvent* End = Events.GetEnd();
	return End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == Failure;
}

FVaRestSocketConnectRequest VaRest::WsTest::Request(const FScheme& Scheme, const FString& PathAndQuery)
{
	FVaRestSocketConnectRequest Out;
	Out.Url = Scheme.Base + PathAndQuery;
	return Out;
}

FVaRestJson VaRest::WsTest::FetchJournal(FAutomationTestBase& Test, const FSite& Site, const FString& Key, int32 Connections, int32 WaitMs)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Client.Get(FString::Printf(TEXT("/ws/journal?key=%s&connections=%d&wait_ms=%d"), *Key, Connections, WaitMs)), Outcome).Send();
	const double Seconds = WaitMs / 1000.0 + 15.0;
	if (!PumpUntil([Outcome] { return Outcome->IsDone(); }, Seconds, true))
	{
		// Drained before returning (VaRestLwsProbe.cpp:59-64): Canceled completes on a later game-thread task
		Handle.Cancel();
		PumpUntil([Outcome, &Handle] { return Outcome->IsDone() && !Handle.IsActive(); }, 5.0, true);
		Test.AddError(FString::Printf(TEXT("the journal of %s did not arrive within %.0f s"), *Key, Seconds));
		return FVaRestJson();
	}
	if (Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("the journal of %s did not arrive: %s"), *Key, *Outcome->Error.Describe()));
		return FVaRestJson();
	}
	return Outcome->Value.GetValue();
}

bool VaRest::WsTest::FetchBytes(FAutomationTestBase& Test, const FSite& Site, const FString& Path, TArray<uint8>& Out)
{
	using namespace VaRest::Http::Testing;
	FVaRestClient Client = FVaRestClient::Make(Site.HttpBase);
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<TArray<uint8>>();
	const FVaRestRequestHandle Handle = Observe(Client.Get<TArray<uint8>>(Path), Outcome).Send();
	if (!PumpUntil([Outcome] { return Outcome->IsDone(); }, 20.0, true))
	{
		Handle.Cancel();
		PumpUntil([Outcome, &Handle] { return Outcome->IsDone() && !Handle.IsActive(); }, 5.0, true);
		Test.AddError(FString::Printf(TEXT("GET %s: no answer within 20 s"), *Path));
		return false;
	}
	if (Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet())
	{
		Test.AddError(FString::Printf(TEXT("GET %s failed: %s"), *Path, *Outcome->Error.Describe()));
		return false;
	}
	Out = Outcome->Value.GetValue();
	return true;
}

TArray<FVaRestJson> VaRest::WsTest::JournalEvents(const FVaRestJson& Journal, const TCHAR* Kind, int32 ConnIndex)
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

FString VaRest::WsTest::JournalHeader(const FVaRestJson& Journal, const TCHAR* Name, int32 ConnIndex)
{
	// RequestEvent, not Request: the name would hide VaRest::WsTest::Request (C4459 is an error, review spec M-10)
	for (const FVaRestJson& RequestEvent : JournalEvents(Journal, TEXT("request"), ConnIndex))
	{
		for (const FVaRestJson& Pair : RequestEvent[TEXT("headers")].Elements())
		{
			// RFC 9110 5.1: header names ignore case
			if (Pair[0].AsString().Equals(Name, ESearchCase::IgnoreCase))
			{
				return Pair[1].AsString();
			}
		}
	}
	return TEXT("(absent)");
}

int32 VaRest::WsTest::JournalCloseCode(const FVaRestJson& Journal, FString* OutReason, int32 ConnIndex)
{
	const TArray<FVaRestJson> Closes = JournalEvents(Journal, TEXT("close"), ConnIndex);
	if (Closes.Num() == 0)
	{
		return -1;
	}
	if (OutReason)
	{
		*OutReason = Closes[0][TEXT("reason")].AsString();
	}
	return Closes[0][TEXT("code")].AsInt(-1);
}

FString VaRest::WsTest::Utf8(TConstArrayView<uint8> Bytes)
{
	return Bytes.Num() > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()) : FString();
}

FString VaRest::WsTest::Sha256Hex(TConstArrayView<uint8> Bytes)
{
	return VaRest::Http::FSha256::Hash(Bytes).ToHex();
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Write the failing tests** — create `Source/VaRestTests/Private/WebSocket/VaRestWsConnectTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: connect, handshake, open and refusal against Tools/testserver/ws.py, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"

#include <atomic>

namespace VaRest::WsTest::VaRestWsConnectTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	struct FSinkLog
	{
		std::atomic<int32> Calls{ 0 };
		std::atomic<int32> OnGameThread{ 0 };
	};

	/** Until the transport holds no connection: LWS destroyed every wsi */
	bool WaitIdle(ILwsTestTransport& Transport, double Seconds = 5.0)
	{
		return VaRest::Http::Testing::PumpUntil([&Transport] { return Transport.GetCounters().LiveConnections == 0; }, Seconds, true);
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN(bNeedTls) \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsConnectTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site, bNeedTls)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectOpenTest, "VaRest.Integration.WebSocket.Transport.Connect.Open", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectOpenTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FEvents Events(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + Key)));
		const bool bOpened = Events.WaitType(EVaRestSocketEventType::Open);
		const FVaRestSocketEvent* Open = Events.Find(EVaRestSocketEventType::Open);
		if (!TestTrue(FString::Printf(TEXT("%s: opened (%s)"), Scheme.Name, *Events.Describe()), bOpened && Open != nullptr))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd();
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: no subprotocol selected"), Scheme.Name), Open->Protocol.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s: the connect time is measured (%.4f s)"), Scheme.Name, Open->Seconds), Open->Seconds > 0.0 && Open->Seconds < 10.0);
		TestFalse(FString::Printf(TEXT("%s: not ended"), Scheme.Name), Events.Connection->HasEnded());
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd());
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: Closed by the client, 4000, clean (%s)"), Scheme.Name, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Closed && End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && End->End.bWasClean);
		TestEqual(FString::Printf(TEXT("%s: one terminal event"), Scheme.Name), Events.Count(EVaRestSocketEventType::Closed) + Events.Count(EVaRestSocketEventType::Error), 1);
		FString Reason;
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqual(FString::Printf(TEXT("%s: the server got our close 4000"), Scheme.Name), JournalCloseCode(Journal, &Reason), 4000);
		TestEqualSensitive(FString::Printf(TEXT("%s: ... with its reason"), Scheme.Name), Reason, FString(TEXT("bye")));
	}
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectHandshakeTest, "VaRest.Integration.WebSocket.Transport.Connect.Handshake", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectHandshakeTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Req.Headers = { { TEXT("X-VaRest-Test"), TEXT("value 1") }, { TEXT("Authorization"), TEXT("Bearer abc") } };
		const TUniquePtr<FEvents> Events = Visit(*Transport, MoveTemp(Req));
		TestNotNull(FString::Printf(TEXT("%s: opened (%s)"), Scheme.Name, *Events->Describe()), Events->Find(EVaRestSocketEventType::Open));
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqualSensitive(FString::Printf(TEXT("%s: a custom header"), Scheme.Name), JournalHeader(Journal, TEXT("X-VaRest-Test")), FString(TEXT("value 1")));
		TestEqualSensitive(FString::Printf(TEXT("%s: Authorization"), Scheme.Name), JournalHeader(Journal, TEXT("Authorization")), FString(TEXT("Bearer abc")));
		const FString Host = Scheme.bTls ? FString::Printf(TEXT("localhost:%d"), Site.TlsPort) : FString::Printf(TEXT("%s:%d"), *Site.Host, Site.Port);
		TestEqualSensitive(FString::Printf(TEXT("%s: Host keeps the non-default port [hs.host_header]"), Scheme.Name), JournalHeader(Journal, TEXT("Host")), Host);
		TestEqualSensitive(FString::Printf(TEXT("%s: the derived Origin, sent raw (D29, T4)"), Scheme.Name), JournalHeader(Journal, TEXT("Origin")),
			FString(Scheme.bTls ? TEXT("https://") : TEXT("http://")) + Host);
		TestEqualSensitive(FString::Printf(TEXT("%s: no subprotocol offered"), Scheme.Name), JournalHeader(Journal, TEXT("Sec-WebSocket-Protocol")), FString(TEXT("(absent)")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectOriginTest, "VaRest.Integration.WebSocket.Transport.Connect.Origin", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectOriginTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Given = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + Given);
		Req.Origin = FString(TEXT("https://game.example:8443"));
		Visit(*Transport, MoveTemp(Req));
		TestEqualSensitive(FString::Printf(TEXT("%s: an Origin as given [hs.raw_origin]"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, Given), TEXT("Origin")),
			FString(TEXT("https://game.example:8443")));
		const FString Omitted = NewKey();
		FVaRestSocketConnectRequest None = Request(Scheme, TEXT("/ws/echo?key=") + Omitted);
		None.Origin = FString();
		Visit(*Transport, MoveTemp(None));
		TestEqualSensitive(FString::Printf(TEXT("%s: an empty Origin omits it [hs.origin_omitted]"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, Omitted), TEXT("Origin")),
			FString(TEXT("(absent)")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectProtocolsTest, "VaRest.Integration.WebSocket.Transport.Connect.Protocols", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectProtocolsTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/protocol?select=chat.v1&key=") + Key);
		Req.Protocols = { TEXT("chat.v2"), TEXT("chat.v1") };
		const TUniquePtr<FEvents> Second = Visit(*Transport, MoveTemp(Req));
		const FVaRestSocketEvent* Open = Second->Find(EVaRestSocketEventType::Open);
		if (TestNotNull(FString::Printf(TEXT("%s: opened (%s)"), Scheme.Name, *Second->Describe()), Open))
		{
			// T17: the selection is read in FILTER_PRE_ESTABLISH; LWS skips the space after the comma when it matches it
			TestEqualSensitive(FString::Printf(TEXT("%s: the second offered protocol, selected"), Scheme.Name), Open->Protocol, FString(TEXT("chat.v1")));
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: offered in order"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, Key), TEXT("Sec-WebSocket-Protocol")),
			FString(TEXT("chat.v2, chat.v1")));
		FVaRestSocketConnectRequest NoneSelected = Request(Scheme, TEXT("/ws/protocol?key=") + NewKey());
		NoneSelected.Protocols = { TEXT("chat.v2") };
		const TUniquePtr<FEvents> Unselected = Visit(*Transport, MoveTemp(NoneSelected));
		const FVaRestSocketEvent* OpenWithout = Unselected->Find(EVaRestSocketEventType::Open);
		TestTrue(FString::Printf(TEXT("%s: a server that selects none still opens, with no protocol [hs.none_selected]"), Scheme.Name), OpenWithout && OpenWithout->Protocol.IsEmpty());
		FVaRestSocketConnectRequest Evil = Request(Scheme, TEXT("/ws/protocol?select=evil&key=") + NewKey());
		Evil.Protocols = { TEXT("chat.v2") };
		const TUniquePtr<FEvents> Mismatch = Visit(*Transport, MoveTemp(Evil));
		TestTrue(FString::Printf(TEXT("%s: a protocol not offered is ProtocolMismatch [hs.unoffered_selected] (%s)"), Scheme.Name, *Mismatch->Describe()),
			IsError(*Mismatch, EVaRestSocketFailure::ProtocolMismatch));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectRefusedTest, "VaRest.Integration.WebSocket.Transport.Connect.Refused", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectRefusedTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const int32 Status : { 401, 403, 404, 429, 500, 503 })
		{
			const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/status/%d?key=%s"), Status, *NewKey())));
			const FVaRestSocketEvent* End = Events->GetEnd();
			TestTrue(FString::Printf(TEXT("%s %d: Error UpgradeRefused with the status, code 0 (%s)"), Scheme.Name, Status, *Events->Describe()),
				End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::UpgradeRefused && End->End.HttpStatus == Status && End->End.Code == 0);
		}
		for (const TCHAR* What : { TEXT("accept"), TEXT("upgrade"), TEXT("connection") })
		{
			const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/broken?what=%s&key=%s"), What, *NewKey())));
			TestTrue(FString::Printf(TEXT("%s: a 101 with a bad %s is BadUpgradeResponse (%s)"), Scheme.Name, What, *Events->Describe()),
				IsError(*Events, EVaRestSocketFailure::BadUpgradeResponse));
		}
		const TUniquePtr<FEvents> Dropped = Visit(*Transport, Request(Scheme, TEXT("/ws/noupgrade?key=") + NewKey()));
		TestTrue(FString::Printf(TEXT("%s: no status line is NoUpgradeResponse (22 A2) (%s)"), Scheme.Name, *Dropped->Describe()),
			IsError(*Dropped, EVaRestSocketFailure::NoUpgradeResponse));
	}
	FVaRestSocketConnectRequest Closed;
	Closed.Url = TEXT("ws://127.0.0.1:1/");
	const TUniquePtr<FEvents> Refused = Visit(*Transport, MoveTemp(Closed));
	TestTrue(FString::Printf(TEXT("a refused TCP connect is ConnectFailed [hs.connect_refused] (%s)"), *Refused->Describe()), IsError(*Refused, EVaRestSocketFailure::ConnectFailed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectBadRequestTest, "VaRest.Integration.WebSocket.Transport.Connect.BadRequest", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectBadRequestTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	const TSharedRef<FSinkLog, ESPMode::ThreadSafe> Log = MakeShared<FSinkLog, ESPMode::ThreadSafe>();
	FVaRestSocketConnectRequest Bad;
	Bad.Url = TEXT("ftp://127.0.0.1/");
	FEvents Events(Transport->Connect(MoveTemp(Bad), [Log] { ++Log->Calls; Log->OnGameThread += IsInGameThread() ? 1 : 0; }));
	TestTrue(TEXT("a bad URL ends"), Events.WaitEnd());
	TestTrue(FString::Printf(TEXT("... as Error BadUrl (%s)"), *Events.Describe()), IsError(Events, EVaRestSocketFailure::BadUrl));
	TestTrue(TEXT("the sink ran"), Log->Calls.load() >= 1);
	// E7: queued like any other event, from the service thread -- so never inside Connect
	TestEqual(TEXT("... never on the game thread, so never inside Connect"), Log->OnGameThread.load(), 0);

	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Injected = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Injected.Headers = { { TEXT("X-B"), TEXT("a\r\nX-C: b") } };
		const TUniquePtr<FEvents> Refused = Visit(*Transport, MoveTemp(Injected));
		TestTrue(FString::Printf(TEXT("%s: CR/LF in a header value is BadHandshake (%s)"), Scheme.Name, *Refused->Describe()), IsError(*Refused, EVaRestSocketFailure::BadHandshake));
		FVaRestSocketConnectRequest Accented = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Accented.Headers = { { TEXT("X-D"), TEXT("café") } };
		TestTrue(FString::Printf(TEXT("%s: a non-ASCII header value is BadHandshake"), Scheme.Name), IsError(*Visit(*Transport, MoveTemp(Accented)), EVaRestSocketFailure::BadHandshake));
		TestEqual(FString::Printf(TEXT("%s: neither reached the server"), Scheme.Name), FetchJournal(*this, Site, Key, 1, 500)[TEXT("connections")].AsInt(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectNotAllowedTest, "VaRest.Integration.WebSocket.Transport.Connect.NotAllowed", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectNotAllowedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Plan decision 19: the filter's map injected, never the host project's ini
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	Options.bOverrideAllowlist = true;
	Options.Allowlist = { { TEXT("ws"), { TEXT("example.com") } }, { TEXT("wss"), { TEXT("example.com") } } };
	const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Filtered = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
	ON_SCOPE_EXIT
	{
		Filtered->Shutdown();
	};
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		const TUniquePtr<FEvents> Events = Visit(*Filtered, Request(Scheme, TEXT("/ws/echo?key=") + Key));
		TestTrue(FString::Printf(TEXT("%s: a host the filter does not list is NotAllowed (%s)"), Scheme.Name, *Events->Describe()), IsError(*Events, EVaRestSocketFailure::NotAllowed));
		TestEqual(FString::Printf(TEXT("%s: and never reached the server"), Scheme.Name), FetchJournal(*this, Site, Key, 1, 500)[TEXT("connections")].AsInt(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectHeaderRoomTest, "VaRest.Integration.WebSocket.Transport.Connect.HeaderRoom", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectHeaderRoomTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Over the 32 KiB block: refused on the game thread, before any connect (plan decision 14)
		const FString TooBigKey = NewKey();
		FVaRestSocketConnectRequest TooBig = Request(Scheme, TEXT("/ws/echo?key=") + TooBigKey);
		TooBig.Headers = { { TEXT("X-Fill"), FString::ChrN(33000, TEXT('a')) } };
		const TUniquePtr<FEvents> Early = Visit(*Transport, MoveTemp(TooBig));
		const FVaRestSocketEvent* EarlyEnd = Early->GetEnd();
		TestTrue(FString::Printf(TEXT("%s: headers over 32 KiB are BadHandshake before the connect (%s)"), Scheme.Name, *Early->Describe()),
			EarlyEnd && EarlyEnd->End.Failure == EVaRestSocketFailure::BadHandshake && EarlyEnd->End.Detail.Contains(TEXT("32768"), ESearchCase::CaseSensitive));
		// Under the block limit, over the room LWS reports (32482 bytes for a short path [hs.append_header_space]): refused in
		// APPEND_HANDSHAKE_HEADER, the whole block or nothing (E19); the request never goes out
		const FString NoRoomKey = NewKey();
		FVaRestSocketConnectRequest NoRoom = Request(Scheme, TEXT("/ws/echo?key=") + NoRoomKey);
		NoRoom.Headers = { { TEXT("X-Fill"), FString::ChrN(32600, TEXT('a')) } };
		const TUniquePtr<FEvents> Late = Visit(*Transport, MoveTemp(NoRoom));
		const FVaRestSocketEvent* LateEnd = Late->GetEnd();
		TestTrue(FString::Printf(TEXT("%s: headers over LWS's room are BadHandshake from APPEND_HANDSHAKE_HEADER (%s)"), Scheme.Name, *Late->Describe()),
			LateEnd && LateEnd->End.Failure == EVaRestSocketFailure::BadHandshake && LateEnd->End.Detail.Contains(TEXT("handshake buffer has"), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("%s: neither request reached the server"), Scheme.Name),
			FetchJournal(*this, Site, TooBigKey, 1, 300)[TEXT("connections")].AsInt() + FetchJournal(*this, Site, NoRoomKey, 1, 300)[TEXT("connections")].AsInt(), 0);
		// A block that fits arrives whole [hs.append_header_fill]
		const FString FitsKey = NewKey();
		FVaRestSocketConnectRequest Fits = Request(Scheme, TEXT("/ws/echo?key=") + FitsKey);
		Fits.Headers = { { TEXT("X-Fill"), FString::ChrN(30000, TEXT('b')) } };
		const TUniquePtr<FEvents> Opened = Visit(*Transport, MoveTemp(Fits));
		TestNotNull(FString::Printf(TEXT("%s: 30000 bytes of header open (%s)"), Scheme.Name, *Opened->Describe()), Opened->Find(EVaRestSocketEventType::Open));
		TestEqual(FString::Printf(TEXT("%s: ... and arrive whole"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, FitsKey), TEXT("X-Fill")).Len(), 30000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectWriteTest, "VaRest.Integration.WebSocket.Transport.Connect.Write", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectWriteTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FEvents Events(Transport->Connect(Request(Scheme, TEXT("/ws/sink?key=") + Key)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd();
			continue;
		}
		const uint8 Raw[] = { 1, 2, 3 };
		TestTrue(FString::Printf(TEXT("%s: a text send is accepted"), Scheme.Name), Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("hello")))));
		TestTrue(FString::Printf(TEXT("%s: a binary send is accepted"), Scheme.Name), Events.Connection->Send(FVaRestSocketFrame::Binary(Raw)));
		Events.Connection->Close(1000, TEXT("done"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		if (!TestEqual(FString::Printf(TEXT("%s: two messages reached the server"), Scheme.Name), Messages.Num(), 2))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: the text first, as sent"), Scheme.Name), Messages[0][TEXT("kind")].AsString().Equals(TEXT("text"), ESearchCase::CaseSensitive)
			&& Messages[0][TEXT("text")].AsString().Equals(TEXT("hello"), ESearchCase::CaseSensitive));
		TestTrue(FString::Printf(TEXT("%s: then the binary"), Scheme.Name), Messages[1][TEXT("kind")].AsString().Equals(TEXT("binary"), ESearchCase::CaseSensitive) && Messages[1][TEXT("len")].AsInt() == 3);
		TestEqual(FString::Printf(TEXT("%s: then our close"), Scheme.Name), JournalCloseCode(Journal), 1000);
		TestEqual(FString::Printf(TEXT("%s: nothing left buffered"), Scheme.Name), Events.Connection->GetBufferedAmount(), static_cast<int64>(0));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectReleaseTest, "VaRest.Integration.WebSocket.Transport.Connect.Release", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectReleaseTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		// An open connection whose last handle goes is closed with 1001 "released" (spec 1.4)
		const FString OpenKey = NewKey();
		TUniquePtr<FEvents> Opened = MakeUnique<FEvents>(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + OpenKey)));
		TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Opened->WaitType(EVaRestSocketEventType::Open));
		Opened.Reset();
		FString Reason;
		const FVaRestJson OpenJournal = FetchJournal(*this, Site, OpenKey);
		TestEqual(FString::Printf(TEXT("%s: the server got 1001"), Scheme.Name), JournalCloseCode(OpenJournal, &Reason), 1001);
		TestEqualSensitive(FString::Printf(TEXT("%s: ... \"released\""), Scheme.Name), Reason, FString(TEXT("released")));
		// A pending one is killed: the server sees its TCP end
		const FString PendingKey = NewKey();
		TUniquePtr<FEvents> Pending = MakeUnique<FEvents>(Transport->Connect(Request(Scheme, TEXT("/ws/hang?ms=20000&key=") + PendingKey)));
		// FetchJournal pumps by itself (PumpUntil is not re-entrant): a plain loop, each fetch waiting up to 200 ms
		bool bSent = false;
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (!bSent && FPlatformTime::Seconds() < Until)
		{
			bSent = FetchJournal(*this, Site, PendingKey, 1, 200)[TEXT("connections")].AsInt() == 1;
		}
		TestTrue(FString::Printf(TEXT("%s: the upgrade reached the server"), Scheme.Name), bSent);
		Pending.Reset();
		TestTrue(FString::Printf(TEXT("%s: the released attempt ended at the server"), Scheme.Name), FetchJournal(*this, Site, PendingKey, 1, 5000)[TEXT("ended")].AsBool());
	}
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	TestTrue(TEXT("the pending ones were killed"), Transport->GetCounters().KillsIssued >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectShutdownTest, "VaRest.Integration.WebSocket.Transport.Connect.Shutdown", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectShutdownTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	const TArray<FScheme> Both = Schemes(Site);
	FEvents Pending(Transport->Connect(Request(Both[0], TEXT("/ws/hang?ms=20000&key=") + NewKey())));
	FEvents Open(Transport->Connect(Request(Both[0], TEXT("/ws/echo?key=") + NewKey())));
	TestTrue(TEXT("one opened"), Open.WaitType(EVaRestSocketEventType::Open));
	Transport->Shutdown();
	Pending.Pull();
	Open.Pull();
	const FVaRestSocketEvent* PendingEnd = Pending.GetEnd();
	const FVaRestSocketEvent* OpenEnd = Open.GetEnd();
	TestTrue(FString::Printf(TEXT("the pending one: Error Shutdown (%s)"), *Pending.Describe()), PendingEnd && PendingEnd->Type == EVaRestSocketEventType::Error && PendingEnd->End.Failure == EVaRestSocketFailure::Shutdown);
	TestTrue(FString::Printf(TEXT("the open one: Closed 1006, Shutdown (%s)"), *Open.Describe()), OpenEnd && OpenEnd->Type == EVaRestSocketEventType::Closed && OpenEnd->End.Failure == EVaRestSocketFailure::Shutdown);
	FEvents After(Transport->Connect(Request(Both[0], TEXT("/ws/echo?key=") + NewKey())));
	TestTrue(TEXT("a connect after Shutdown ends at once"), After.WaitEnd(2.0));
	TestTrue(FString::Printf(TEXT("... with Shutdown (%s)"), *After.Describe()), IsError(After, EVaRestSocketFailure::Shutdown));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectDefaultTest, "VaRest.Integration.WebSocket.Transport.Connect.Default", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectDefaultTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// The process's transport (spec 2.1), ws only: it is never shut down here, so no wss may be in flight on it (VaRest-rti).
	// Its thread then lives until the process ends: an Automation RunTests ...; Quit run calls neither OnEnginePreExit nor
	// ShutdownModule (CLAUDE.md). That is the shape VaRest-x2v suspected for the probe's exit; noted there (review spec M-7)
	const TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> Default = VaRest::WebSocket::GetDefaultTransport();
	TestTrue(TEXT("one instance"), &Default.Get() == &VaRest::WebSocket::GetDefaultTransport().Get());
	FVaRestSocketConnectRequest Req;
	Req.Url = FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *NewKey());
	const TUniquePtr<FEvents> Events = Visit(*Default, MoveTemp(Req));
	TestNotNull(FString::Printf(TEXT("the default transport opens (%s)"), *Events->Describe()), Events->Find(EVaRestSocketEventType::Open));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectStartRefusalsTest, "VaRest.Integration.WebSocket.Transport.Connect.StartRefusals", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectStartRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsConnectTestPrivate;
	using VaRest::WebSocket::Testing::FLwsTransportOptions;
	using VaRest::WebSocket::Testing::MakeLwsTransport;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("the service thread could not start"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("VaRest wss handshake(s) are in flight"), EAutomationExpectedErrorFlags::Contains, 1);
	const FScheme Plain = Schemes(Site)[0];
	{
		// Review conc I-2: a transport whose start failed never makes a second context -- in a shared LWS copy a retry could
		// run under an engine wss handshake (VaRest-rti, ruling Q2)
		FLwsTransportOptions Options;
		Options.TcpAddressOverride = Site.Host;
		Options.bFailThreadForTest = true;
		const TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> Failing = MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT
		{
			Failing->Shutdown();
		};
		FEvents First(Failing->Connect(Request(Plain, TEXT("/ws/echo?key=") + NewKey())));
		FEvents Second(Failing->Connect(Request(Plain, TEXT("/ws/echo?key=") + NewKey())));
		TestTrue(TEXT("the first connect ends"), First.WaitEnd(2.0));
		TestTrue(TEXT("... and so does the second"), Second.WaitEnd(2.0));
		TestTrue(FString::Printf(TEXT("both with Shutdown (%s; %s)"), *First.Describe(), *Second.Describe()),
			IsError(First, EVaRestSocketFailure::Shutdown) && IsError(Second, EVaRestSocketFailure::Shutdown));
		TestEqual(TEXT("one lws_create_context for both connects"), Failing->GetCounters().ContextCreations, 1);
	}
	{
		// Review conc I-3: no context while a VaRest wss handshake is in flight; not latched, so the next connect opens
		FLwsTransportOptions Options;
		Options.TcpAddressOverride = Site.Host;
		const TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> Gated = MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT
		{
			Gated->Shutdown();
		};
		{
			VaRest::WebSocket::Testing::FScopedTlsHandshakeInFlight InFlight;
			FEvents Refused(Gated->Connect(Request(Plain, TEXT("/ws/echo?key=") + NewKey())));
			TestTrue(TEXT("a connect while a wss handshake is in flight ends"), Refused.WaitEnd(2.0));
			TestTrue(FString::Printf(TEXT("... with Shutdown (%s)"), *Refused.Describe()), IsError(Refused, EVaRestSocketFailure::Shutdown));
			TestEqual(TEXT("... and no context made"), Gated->GetCounters().ContextCreations, 0);
		}
		const TUniquePtr<FEvents> Later = Visit(*Gated, Request(Plain, TEXT("/ws/echo?key=") + NewKey()));
		TestNotNull(FString::Printf(TEXT("once none is in flight, the same transport opens (%s)"), *Later->Describe()), Later->Find(EVaRestSocketEventType::Open));
		TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Gated));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectTlsCountedTest, "VaRest.Integration.WebSocket.Transport.Connect.TlsHandshakeCounted", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectTlsCountedTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	using VaRest::WebSocket::Testing::GetTlsHandshakesInFlight;
	const TArray<FScheme> Both = Schemes(Site);
	if (!TestEqual(TEXT("no wss attempt is in flight at the start"), GetTlsHandshakesInFlight(), 0))
	{
		return false;
	}
	// Review conc I-3: a wss attempt counts from its connect until LWS establishes or destroys it. Re-review N-2: from the
	// moment Connect returns, on the game thread, with no pumping
	FEvents Pending(Transport->Connect(Request(Both[1], TEXT("/ws/hang?ms=20000&key=") + NewKey())));
	TestEqual(TEXT("a wss attempt is counted as soon as Connect returns"), GetTlsHandshakesInFlight(), 1);
	TestTrue(TEXT("... and while it is pending"), VaRest::Http::Testing::PumpUntil([] { return GetTlsHandshakesInFlight() == 1; }, 5.0, true));
	const FString PlainKey = NewKey();
	FEvents Plain(Transport->Connect(Request(Both[0], TEXT("/ws/hang?ms=20000&key=") + PlainKey)));
	// Until the ws upgrade reached the server, so its Start has run. FetchJournal pumps by itself (PumpUntil is not
	// re-entrant): a plain loop, each fetch waiting up to 200 ms
	bool bSent = false;
	const double Until = FPlatformTime::Seconds() + 10.0;
	while (!bSent && FPlatformTime::Seconds() < Until)
	{
		bSent = FetchJournal(*this, Site, PlainKey, 1, 200)[TEXT("connections")].AsInt() == 1;
	}
	TestTrue(TEXT("the ws upgrade reached the server"), bSent);
	TestEqual(TEXT("a ws attempt is not counted"), GetTlsHandshakesInFlight(), 1);
	Pending.Connection->Close(1000, TEXT("cancel"));
	Plain.Connection->Close(1000, TEXT("cancel"));
	TestTrue(TEXT("both end"), Pending.WaitEnd() && Plain.WaitEnd());
	// The terminal event comes at the decision; the count only when LWS destroyed the wsi (decision 3)
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	TestEqual(TEXT("... and the count is back to 0"), GetTlsHandshakesInFlight(), 0);
	const TUniquePtr<FEvents> Opened = Visit(*Transport, Request(Both[1], TEXT("/ws/echo?key=") + NewKey()));
	TestNotNull(FString::Printf(TEXT("a wss attempt opens (%s)"), *Opened->Describe()), Opened->Find(EVaRestSocketEventType::Open));
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	TestEqual(TEXT("an opened one leaves the count too"), GetTlsHandshakesInFlight(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectLocalhostTest, "Probe.VaRest.Lws.Connect.LocalhostAddress", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectLocalhostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// A record, out of the default suite (Probe.*, review spec M-1): its outcome is the host resolver's. Ruling Q5: LWS 3.0
	// connects to the first address getaddrinfo returns (spec 2.2); localhost resolves to ::1 first here, the listener is
	// 127.0.0.1 only. The fallback is W1b B9 (VaRest-gi0); every other test sets the TCP address (FServiceOptions::
	// TcpAddressOverride) and keeps localhost as the Host and TLS name. No TCP override here, no CA
	const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Plain = VaRest::WebSocket::Testing::MakeLwsTransport(VaRest::WebSocket::Testing::FLwsTransportOptions());
	ON_SCOPE_EXIT
	{
		Plain->Shutdown();
	};
	FVaRestSocketConnectRequest Req;
	Req.Url = FString::Printf(TEXT("ws://localhost:%d/ws/echo?key=%s"), Site.Port, *NewKey());
	const TUniquePtr<FEvents> Events = Visit(*Plain, MoveTemp(Req));
	AddInfo(FString::Printf(TEXT("ws://localhost against a 127.0.0.1-only listener: %s"), *Events->Describe()));
	TestNotNull(TEXT("it ended either way (drained)"), Events->GetEnd());
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 3: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Connect`
Expected: FAIL to compile (`VaRestSocketTesting.h` not found).

- [ ] **Step 4: The default transport's declaration** — in `Source/VaRestWebSocket/Public/VaRestSocketTransport.h`, append:

```cpp
namespace VaRest::WebSocket
{
	/**
	 * The process's libwebsockets transport (spec 2.1: one context per process; plan decision 8), made at first use and
	 * shut down at engine exit; a connect after that ends with Shutdown. Any thread; its Connect is game thread.
	 */
	VARESTWEBSOCKET_API TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> GetDefaultTransport();
}
```

- [ ] **Step 5: The connection** — create `Source/VaRestWebSocket/Private/VaRestLwsConn.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestLwsService.h"
#include "VaRestSocketLink.h"
#include "VaRestSocketTarget.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket::Lws
{
	enum class EConnState : uint8
	{
		/** Made on the game thread; Start has not run */
		Waiting,
		/** lws_client_connect_via_info was called; no open yet */
		Connecting,
		Open,
		/** The terminal event is queued; LWS may still call back until WSI_DESTROY */
		Ended
	};

	/**
	 * One connection (WebSocket spec 2.1-2.5): libwebsockets' userdata for it, owned by the FService from Adopt on. Made and
	 * prepared on the game thread by FTransport::Connect; every member is the service thread's from Adopt on.
	 */
	class FConn final : public IServiceConnection
	{
	public:
		FConn(uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink, FVaRestSocketConnectRequest&& InRequest);

		/** Any thread: the last owner drops it -- a wss attempt still counted leaves TlsHandshakesInFlight() (re-review N-2: an Adopt command dropped unrun) */
		virtual ~FConn() override;

		/** Game thread, inside Connect: checks and encodes the request (spec 2.2); a failure is reported by Start, from the service thread (E7) */
		void Prepare(const FServiceOptions& Options);
		const FString& GetLabel() const { return Label; }
		/** Game thread, inside Connect: a wss target that Prepare took */
		bool NeedsTlsCount() const { return Target.bTls && PendingFailure == EVaRestSocketFailure::None; }
		/** Game thread, inside Connect, before the Adopt command is queued: raises TlsHandshakesInFlight() for this attempt (re-review N-2) */
		void MarkTlsCounted();

		// ---- IServiceConnection (service thread)
		virtual uint64 GetId() const override { return Id; }
		virtual void Start(FService& Service) override;
		virtual int32 OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len) override;
		virtual void CheckDeadlines(FService& Service, double Now) override;
		virtual double NextDeadline() const override;
		virtual bool WantsShortWait() const override;
		virtual bool IsDestroyed() const override { return bDestroyed; }
		virtual void EndForShutdown() override;

		// ---- commands from the handle (service thread)
		void Send(FService& Service, FVaRestSocketFrame&& Frame);
		void Close(FService& Service, int32 Code, TArray<uint8>&& Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure);
		void Release(FService& Service);

		/** The connection with InId, or null: every connection the transport adopts is an FConn */
		static FConn* Find(FService& Service, uint64 InId);

	private:
		int32 OnAppendHeaders(void* In, size_t Len);
		int32 OnFilterPreEstablish(lws* InWsi);
		int32 OnEstablished(lws* InWsi);
		int32 OnWriteable(lws* InWsi);
		void OnConnectionError(const void* In, size_t Len);
		/** CLIENT_CLOSED, CLOSED_CLIENT_HTTP, and WSI_DESTROY for what did not end before it */
		void OnEnded();
		void OnDestroyed(FService& Service);
		/** lws_set_timeout(LWS_TO_KILL_ASYNC) (T14): the end is reported by the caller, LWS tears TCP down later (plan decision 3) */
		void Kill(FService& Service);
		void ReleaseHeld();
		/** The outbox's bytes leave the buffered amount, with no Drained (an end) */
		void DropOutbox();
		void EndPreOpen(EVaRestSocketFailure Failure, EVaRestSocketCloseBy By, int32 HttpStatus, FString Detail);
		void EndOpen(int32 Code, FString Reason, bool bClean, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure, FString Detail = FString());
		/** Once: this wss attempt leaves TlsHandshakesInFlight() (review conc I-3) */
		void UncountTls();
		/** Once: this connection's close leaves FServiceCounters::UnsentCloses (review conc M-3) */
		void UncountClose();

		const uint64 Id;
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link;
		FVaRestSocketConnectRequest Request;
		FSocketTarget Target;
		FString Label;
		/** A failure VaRest decided -- in Prepare, or during the handshake -- that explains the end better than libwebsockets' string */
		EVaRestSocketFailure PendingFailure = EVaRestSocketFailure::None;
		FString PendingDetail;
		/** UTF-8, NUL-terminated copies of what lws_client_connect_via_info takes: LWS keeps the pointers (libwebsockets.h:3392-3480) */
		TArray<ANSICHAR> AddressUtf8;
		TArray<ANSICHAR> PathUtf8;
		TArray<ANSICHAR> HostUtf8;
		TArray<ANSICHAR> OriginUtf8;
		TArray<ANSICHAR> ProtocolsUtf8;
		lws* Wsi = nullptr;
		/** Set by Start: the service that owns this connection */
		FService* Owner = nullptr;
		EConnState State = EConnState::Waiting;
		bool bDestroyed = false;
		/** In TlsHandshakesInFlight(): a wss attempt from its connect until CLIENT_ESTABLISHED or WSI_DESTROY (review conc I-3) */
		bool bTlsCounted = false;
		double StartedAt = 0.0;
		int32 AppendCalls = 0;
		FString SelectedProtocol;
		bool bKillIssued = false;
		double KillAt = 0.0;
		/** When the pending kill was last armed: Kill sets it, CheckDeadlines' re-arm moves it (re-review N-1) */
		double KillArmedAt = 0.0;
		// ---- the outbox (plan decisions 6, 7, 18)
		TArray<FVaRestSocketFrame> Outbox;
		/** Bytes of a write LWS still holds (lws_partial_buffered); released once it says the write drained */
		int64 HeldBytes = 0;
		// ---- our close (spec 2.5)
		bool bCloseQueued = false;
		bool bCloseSent = false;
		/** In FServiceCounters::UnsentCloses: asked for while open, not written yet (review conc M-3) */
		bool bCloseCounted = false;
		/** When OnWriteable wrote our close frame: an end by LWS's own 5 s close-ack timer after it is not clean (review conc I-5, Task 9) */
		double CloseSentAt = 0.0;
		int32 CloseCode = 0;
		TArray<uint8> CloseReason;
		EVaRestSocketCloseBy CloseBy = EVaRestSocketCloseBy::Client;
		EVaRestSocketFailure CloseFailure = EVaRestSocketFailure::None;
	};
}
```

Create `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "HAL/PlatformTime.h"
#include "VaRestLwsErrors.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate
{
	FString Utf8ToString(const TArray<uint8>& Bytes)
	{
		return Bytes.Num() > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()) : FString();
	}
}

VaRest::WebSocket::Lws::FConn::FConn(uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink, FVaRestSocketConnectRequest&& InRequest)
	: Id(InId)
	, Link(MoveTemp(InLink))
	, Request(MoveTemp(InRequest))
{
	Label = Request.LogName.IsEmpty() ? FString::Printf(TEXT("ws#%llu"), static_cast<unsigned long long>(Id)) : Request.LogName;
}

VaRest::WebSocket::Lws::FConn::~FConn()
{
	UncountTls();
}

void VaRest::WebSocket::Lws::FConn::MarkTlsCounted()
{
	check(IsInGameThread());
	if (!bTlsCounted)
	{
		TlsHandshakesInFlight().fetch_add(1);
		bTlsCounted = true;
	}
}

VaRest::WebSocket::Lws::FConn* VaRest::WebSocket::Lws::FConn::Find(FService& Service, uint64 InId)
{
	return static_cast<FConn*>(Service.Find(InId));
}

void VaRest::WebSocket::Lws::FConn::Prepare(const FServiceOptions& Options)
{
	check(IsInGameThread());
	TValueOrError<FSocketTarget, FTargetError> Made = PrepareTarget(Request);
	if (Made.HasError())
	{
		PendingFailure = Made.GetError().Failure;
		PendingDetail = Made.GetError().Message;
		return;
	}
	Target = Made.StealValue();
	// Spec 2.2 (LwsWebSocket.cpp:112-122): the engine's URL filter, read here on the game thread; its refusal is a queued Error (E7)
	if (!IsAllowedByFilter(Target.FilterUrl, Options.bOverrideAllowlist ? &Options.Allowlist : nullptr))
	{
		PendingFailure = EVaRestSocketFailure::NotAllowed;
		PendingDetail = TEXT("the URL request filter ([Online.HttpManager <scheme>] AllowedDomains in the engine ini) refuses this host");
		return;
	}
	Utf8Copy(Options.TcpAddressOverride.IsEmpty() ? Target.Host : Options.TcpAddressOverride, AddressUtf8);
	Utf8Copy(Target.PathAndQuery, PathUtf8);
	Utf8Copy(Target.HostHeader, HostUtf8);
	if (!Target.Origin.IsEmpty())
	{
		Utf8Copy(Target.Origin, OriginUtf8);
	}
	if (!Target.Protocols.IsEmpty())
	{
		Utf8Copy(Target.Protocols, ProtocolsUtf8);
	}
}

void VaRest::WebSocket::Lws::FConn::Start(FService& Service)
{
	Owner = &Service;
	StartedAt = FPlatformTime::Seconds();
	if (Service.IsShuttingDown())
	{
		EndPreOpen(EVaRestSocketFailure::Shutdown, EVaRestSocketCloseBy::Transport, 0, TEXT("the transport shut down before the connect"));
		UncountTls();
		bDestroyed = true;
		return;
	}
	if (PendingFailure != EVaRestSocketFailure::None)
	{
		EndPreOpen(PendingFailure, EVaRestSocketCloseBy::Transport, 0, PendingDetail);
		bDestroyed = true;
		return;
	}
	lws_client_connect_info Info = {};
	Info.context = Service.GetContext();
	Info.address = AddressUtf8.GetData();
	Info.port = Target.Port;
	Info.ssl_connection = Target.bTls ? LCCSCF_USE_SSL : 0;
	if (Target.bTls && Target.bInsecure)
	{
		// Spec 2.2: a self-signed chain allowed and the host name not checked; ALLOW_SELFSIGNED alone fails an IP host
		// [tls.insecure_ip_host, tls.insecure_skip_hostname]
		Info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	}
	Info.path = PathUtf8.GetData();
	Info.host = HostUtf8.GetData();
	// Sent as given: the context has JUST_USE_RAW_ORIGIN (T4) [hs.raw_origin]; null omits it [hs.origin_omitted]
	Info.origin = OriginUtf8.Num() > 0 ? OriginUtf8.GetData() : nullptr;
	Info.protocol = ProtocolsUtf8.Num() > 0 ? ProtocolsUtf8.GetData() : nullptr;
	// T3, T17: bound to VaRest's handler whatever the server selects, so no protocol needs registering (E17) [hs.unregistered_selected_bound]
	Info.local_protocol_name = "varest";
	Info.ietf_version_or_minus_one = -1;
	Info.userdata = static_cast<IServiceConnection*>(this);
	// The wsi early, so a kill reaches an attempt that has not opened yet (libwebsockets.h:3441-3451)
	Info.pwsi = &Wsi;
	State = EConnState::Connecting;
	// Review conc I-3 (VaRest-rti): a wss attempt was counted by FTransport::Connect on the game thread (MarkTlsCounted,
	// re-review N-2) and stays counted until LWS establishes or destroys it -- the whole upgrade, not only the TLS handshake:
	// conservative, and simple. UncountTls lowers it once
	// Ruling Q5: LWS 3.0 resolves the address itself, synchronously, here on the service thread (getaddrinfo inside
	// lws_client_connect_2, review conc I-4), and tries the first address it returns only; W1b B9 (VaRest-gi0) resolves on
	// UE::Tasks and falls back to the next address
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: connecting"), *Label);
	if (!lws_client_connect_via_info(&Info))
	{
		// NULL: scrubbed (libwebsockets.h:3442-3446), maybe after a CONNECTION_ERROR inside the call that reported already;
		// *pwsi may name freed memory, so it is never touched again
		Wsi = nullptr;
		EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("lws_client_connect_via_info returned NULL"));
		UncountTls();
		bDestroyed = true;
	}
}

int32 VaRest::WebSocket::Lws::FConn::OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		return OnAppendHeaders(In, Len);
	case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
		return OnFilterPreEstablish(InWsi);
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		return OnEstablished(InWsi);
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		OnConnectionError(In, Len);
		return 0;
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		return OnWriteable(InWsi);
	case LWS_CALLBACK_CLIENT_CLOSED:
	case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
		// A client ends with CLIENT_CLOSED, never CLOSED (T29, close.peer_sequence); a handshake VaRest refused, with
		// CLOSED_CLIENT_HTTP and no CONNECTION_ERROR [hs.redirect_veto]
		OnEnded();
		return 0;
	case LWS_CALLBACK_WSI_DESTROY:
		OnDestroyed(Service);
		return 0;
	default:
		return 0;
	}
}

int32 VaRest::WebSocket::Lws::FConn::OnAppendHeaders(void* In, size_t Len)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an attempt VaRest already ended (cancelled, released, timed out) never sends its request, headers
		// and all. The kill may not have landed: LWS's own stage timers overwrite it (lws_generate_client_handshake calls
		// lws_set_timeout(wsi, 0, 0)). A non-zero return here refuses the client before the request goes out [hs.redirect_veto]
		return -1;
	}
	++AppendCalls;
	if (Target.HeaderBlock.Num() == 0)
	{
		return 0;
	}
	// in: a char** to the next free byte; len: the room left (T5, libwebsockets.h:1316-1340) [hs.append_header_space]. The
	// whole block or nothing: never past the room (E19)
	if (static_cast<size_t>(Target.HeaderBlock.Num()) > Len)
	{
		PendingFailure = EVaRestSocketFailure::BadHandshake;
		PendingDetail = FString::Printf(TEXT("the headers need %d bytes and the handshake buffer has %llu left"), Target.HeaderBlock.Num(), static_cast<unsigned long long>(Len));
		// A non-zero return refuses the client, with no CONNECTION_ERROR after it [hs.redirect_veto]: OnEnded reports PendingFailure
		return -1;
	}
	char** Cursor = static_cast<char**>(In);
	FMemory::Memcpy(*Cursor, Target.HeaderBlock.GetData(), Target.HeaderBlock.Num());
	*Cursor += Target.HeaderBlock.Num();
	return 0;
}

int32 VaRest::WebSocket::Lws::FConn::OnFilterPreEstablish(lws* InWsi)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an ended attempt never opens ("HS: Rejected by filter cb")
		return -1;
	}
	// The last point the response headers exist (T6, libwebsockets.h:1301-1307): the subprotocol the server selected (T17)
	// [hs.selected_protocol_in_filter]
	const int Total = lws_hdr_total_length(InWsi, WSI_TOKEN_PROTOCOL);
	if (Total > 0)
	{
		TArray<ANSICHAR> Selected;
		Selected.SetNumZeroed(Total + 1);
		if (lws_hdr_copy(InWsi, Selected.GetData(), Selected.Num(), WSI_TOKEN_PROTOCOL) > 0)
		{
			SelectedProtocol = UTF8_TO_TCHAR(Selected.GetData());
		}
	}
	return 0;
}

int32 VaRest::WebSocket::Lws::FConn::OnEstablished(lws* InWsi)
{
	// The upgrade is done: no verify callback of this attempt runs again (review conc I-3)
	UncountTls();
	if (State == EConnState::Ended)
	{
		// Ended meanwhile (a deadline, a release): the open is refused, and nothing more is reported
		return -1;
	}
	State = EConnState::Open;
	FVaRestSocketEvent Opened;
	Opened.Type = EVaRestSocketEventType::Open;
	Opened.Protocol = SelectedProtocol;
	Opened.Seconds = FPlatformTime::Seconds() - StartedAt;
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: open after %.1f ms (protocol \"%s\")"), *Label, Opened.Seconds * 1000.0, *SelectedProtocol);
	Link->Push(MoveTemp(Opened));
	// LWS 3.0 drops a WRITEABLE request made during the handshake (spec 2.4; Probe.VaRest.Lws.Rig.WriteBeforeOpen,
	// VaRestLwsProbe.cpp:606-613): ask again for what was queued meanwhile (plan decision 18)
	if (Outbox.Num() > 0 || bCloseQueued)
	{
		lws_callback_on_writable(InWsi);
	}
	return 0;
}

void VaRest::WebSocket::Lws::FConn::OnConnectionError(const void* In, size_t Len)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	// in: libwebsockets' error string, len its length; either may be empty (libwebsockets.h:1255-1299)
	const FString Text = In && Len > 0 ? FString::ConstructFromPtrSize(static_cast<const UTF8CHAR*>(In), static_cast<int32>(Len)) : FString();
	if (PendingFailure != EVaRestSocketFailure::None)
	{
		// VaRest's own decision explains this end better than LWS's string (plan decisions 10, 11)
		EndPreOpen(PendingFailure, EVaRestSocketCloseBy::Transport, 0, FString::Printf(TEXT("%s (libwebsockets: \"%s\")"), *PendingDetail, *Text));
		return;
	}
	FConnectionErrorClass Class = ClassifyConnectionError(Text);
	FString Detail = FString::Printf(TEXT("libwebsockets: \"%s\""), *Text);
	if (Class.bEmpty)
	{
		// Review conc M-1, M-2: which of LWS's own timers, or a kill VaRest did not make, by the time taken (Task 5)
		Class = ClassifyEmptyConnectionError(FPlatformTime::Seconds() - StartedAt, Detail);
	}
	const EVaRestSocketCloseBy By = Class.Failure == EVaRestSocketFailure::ConnectTimeout ? EVaRestSocketCloseBy::Timeout : EVaRestSocketCloseBy::Transport;
	EndPreOpen(Class.Failure, By, Class.HttpStatus, MoveTemp(Detail));
}

void VaRest::WebSocket::Lws::FConn::OnEnded()
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate;
	if (State == EConnState::Ended)
	{
		return;
	}
	if (State != EConnState::Open)
	{
		// No CONNECTION_ERROR came: VaRest refused the handshake itself (a non-zero return, hs.redirect_veto), or LWS gave up silently
		const bool bOwn = PendingFailure != EVaRestSocketFailure::None;
		EndPreOpen(bOwn ? PendingFailure : EVaRestSocketFailure::Unknown, EVaRestSocketCloseBy::Transport, 0,
			bOwn ? PendingDetail : FString(TEXT("the connection ended before the open without an error string")));
		return;
	}
	if (bCloseQueued)
	{
		// Our close (spec 2.5): LWS 3.0 raises no callback for the peer's reply [close.ours_sequence], so an end before
		// CloseTimeout counts as clean (plan decision 4); a close that never went out ended unclean
		EndOpen(bCloseSent ? CloseCode : 1006, bCloseSent ? Utf8ToString(CloseReason) : FString(), bCloseSent, CloseBy, CloseFailure);
		return;
	}
	// An end without a close frame (spec 2.5): FIN and RST both give CLIENT_CLOSED [close.drop_reports_client_closed]
	EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None);
}

void VaRest::WebSocket::Lws::FConn::OnDestroyed(FService& Service)
{
	// The connection's last callback: whatever has not reported does now, once
	OnEnded();
	UncountTls();
	UncountClose();
	if (bKillIssued)
	{
		const double Took = FPlatformTime::Seconds() - KillAt;
		FServiceCounters& Counters = Service.GetCounters();
		Counters.KillsLanded.fetch_add(1);
		Counters.LastKillToDestroySeconds = Took;
		if (Took > Counters.MaxKillToDestroySeconds.load())
		{
			Counters.MaxKillToDestroySeconds = Took;
		}
	}
	Wsi = nullptr;
	bDestroyed = true;
}

void VaRest::WebSocket::Lws::FConn::Kill(FService& Service)
{
	if (!Wsi || bKillIssued)
	{
		return;
	}
	bKillIssued = true;
	KillAt = FPlatformTime::Seconds();
	KillArmedAt = KillAt;
	Service.GetCounters().KillsIssued.fetch_add(1);
	// T14, T26: the connection ends at LWS's next timeout walk; the wait stays short until it does (WantsShortWait, VaRest-wcv)
	lws_set_timeout(Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC);
}

void VaRest::WebSocket::Lws::FConn::CheckDeadlines(FService& Service, double Now)
{
	if (bKillIssued && !bDestroyed && Wsi && Now - KillArmedAt >= KillRearmSeconds)
	{
		// Review conc I-1: LWS's own stage timers replace a pending timeout (lws_client_socket_service and
		// lws_generate_client_handshake call lws_set_timeout themselves), so a kill that has not landed one timeout walk
		// after it was armed (T26) is armed again, until WSI_DESTROY. Re-review N-1: not on every pass -- each
		// lws_set_timeout restarts the timeout at the current second (__lws_set_timeout stores time() as
		// pending_timeout_set, libwebsockets.c.o 0x1275-0x1287), so a re-arm made after a second changed and before that
		// second's walk would postpone the kill again and again while the service thread is busy in callbacks. Outside any
		// callback, as libwebsockets.h:4673-4679 allows for LWS_TO_KILL_ASYNC
		lws_set_timeout(Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC);
		KillArmedAt = Now;
	}
	if (bDestroyed || State == EConnState::Ended)
	{
		return;
	}
	if (State == EConnState::Open)
	{
		// LWS calls WRITEABLE only once a held write drained; this sweep releases it even when no WRITEABLE was asked for (plan decision 7)
		ReleaseHeld();
	}
}

double VaRest::WebSocket::Lws::FConn::NextDeadline() const
{
	return TNumericLimits<double>::Max();
}

bool VaRest::WebSocket::Lws::FConn::WantsShortWait() const
{
	return (bKillIssued && !bDestroyed) || HeldBytes > 0;
}

void VaRest::WebSocket::Lws::FConn::EndForShutdown()
{
	// The thread is stopped and the context is destroyed next, on this thread, before any other context can be made: no
	// handshake of this attempt runs again, so it leaves the count here even if lws_context_destroy calls no WSI_DESTROY for it
	UncountTls();
	if (State == EConnState::Open)
	{
		EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::Shutdown, TEXT("the transport shut down"));
	}
	else
	{
		EndPreOpen(EVaRestSocketFailure::Shutdown, EVaRestSocketCloseBy::Transport, 0, TEXT("the transport shut down"));
	}
}

void VaRest::WebSocket::Lws::FConn::Release(FService& Service)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	bool bTruncated = false;
	// Spec 1.4: the last handle is gone -- an open connection is closed with 1001 "released", a pending one cancelled (Close
	// does both); the link discards what follows
	Close(Service, 1001, CloseReasonBytes(TEXT("released"), bTruncated), EVaRestSocketCloseBy::Client, EVaRestSocketFailure::None);
}

void VaRest::WebSocket::Lws::FConn::UncountTls()
{
	if (bTlsCounted)
	{
		bTlsCounted = false;
		TlsHandshakesInFlight().fetch_sub(1);
	}
}

void VaRest::WebSocket::Lws::FConn::UncountClose()
{
	if (bCloseCounted && Owner)
	{
		bCloseCounted = false;
		Owner->GetCounters().UnsentCloses.fetch_sub(1);
	}
}

void VaRest::WebSocket::Lws::FConn::EndPreOpen(EVaRestSocketFailure Failure, EVaRestSocketCloseBy By, int32 HttpStatus, FString Detail)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	State = EConnState::Ended;
	DropOutbox();
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Error;
	Event.End.By = By;
	Event.End.Failure = Failure;
	Event.End.HttpStatus = HttpStatus;
	Event.End.Detail = MoveTemp(Detail);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: never opened: %s, %s"), *Label, LexToString(Failure), *Event.End.Detail);
	Link->Push(MoveTemp(Event));
}

void VaRest::WebSocket::Lws::FConn::EndOpen(int32 Code, FString Reason, bool bClean, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure, FString Detail)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	// Nothing more will be written: Shutdown's flush stops waiting for this close (review conc M-3)
	UncountClose();
	State = EConnState::Ended;
	DropOutbox();
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Closed;
	Event.End.Code = Code;
	Event.End.Reason = MoveTemp(Reason);
	Event.End.bWasClean = bClean;
	Event.End.By = By;
	Event.End.Failure = Failure;
	Event.End.Detail = MoveTemp(Detail);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: closed %d by %s%s"), *Label, Code, LexToString(By), bClean ? TEXT(", clean") : TEXT(""));
	Link->Push(MoveTemp(Event));
}
```

Create `Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp` (whole messages here; Task 8 fragments them):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "HAL/PlatformTime.h"
#include "VaRestWebSocketLog.h"

void VaRest::WebSocket::Lws::FConn::Send(FService& Service, FVaRestSocketFrame&& Frame)
{
	if (State == EConnState::Ended || bCloseQueued)
	{
		// Too late: its bytes leave the buffered amount, and no Drained follows an end
		Link->BufferedBytes.fetch_sub(Frame.Length);
		return;
	}
	Outbox.Add(MoveTemp(Frame));
	Link->NoteBuffered(Request.SendHighWater);
	// Before the open LWS would drop the request (spec 2.4): OnEstablished asks for what is queued (plan decision 18)
	if (State == EConnState::Open)
	{
		lws_callback_on_writable(Wsi);
	}
}

void VaRest::WebSocket::Lws::FConn::Close(FService& Service, int32 Code, TArray<uint8>&& Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure)
{
	if (State == EConnState::Ended || bCloseQueued)
	{
		return;
	}
	if (State != EConnState::Open)
	{
		// No close frame before the open: the attempt is abandoned (spec 3.1; plan decision 5)
		EndPreOpen(EVaRestSocketFailure::Canceled, By, 0, TEXT("closed before the open"));
		Kill(Service);
		return;
	}
	bCloseQueued = true;
	CloseCode = Code;
	CloseReason = MoveTemp(Reason);
	CloseBy = By;
	CloseFailure = Failure;
	// Review conc M-3: Shutdown's flush waits for this close frame (at most ExitFlushSeconds)
	bCloseCounted = true;
	Service.GetCounters().UnsentCloses.fetch_add(1);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: closing with %d"), *Label, Code);
	// After the data already queued (plan decision 18)
	lws_callback_on_writable(Wsi);
}

int32 VaRest::WebSocket::Lws::FConn::OnWriteable(lws* InWsi)
{
	// After our close frame, 0 from every WRITEABLE: LWS then waits for the echo, up to its own 5 s (T20; plan decision 4)
	if (bCloseSent || State != EConnState::Open)
	{
		return 0;
	}
	// LWS calls WRITEABLE only after a truncated write drained (libwebsockets.h:6021-6037)
	ReleaseHeld();
	if (Outbox.Num() > 0)
	{
		FVaRestSocketFrame& Head = Outbox[0];
		const int64 FrameSize = Head.Length;
		const int32 Protocol = Head.Kind == EVaRestSocketFrameKind::Text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY;
		// One lws_write per WRITEABLE: a second in one callback returns -1 and is lost (spec 2.4) [tx.two_writes_one_writeable]
		const int Wrote = lws_write(InWsi, Head.Storage.GetData() + Head.Offset, static_cast<size_t>(FrameSize), static_cast<lws_write_protocol>(Protocol));
		// LWS sent the bytes or copied the rest into its own buffer (T10): the frame can go
		Outbox.RemoveAt(0);
		if (Wrote < 0)
		{
			Link->BufferedBytes.fetch_sub(FrameSize);
			EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None, TEXT("lws_write failed"));
			return -1;
		}
		// A data write returns its payload length, a control frame's counts its header: only < 0 fails (spec 2.4)
		// [tx.partial_after_fragment]. lws_partial_buffered says LWS still holds part of this one [tx.large_write_result]
		if (lws_partial_buffered(InWsi))
		{
			HeldBytes += FrameSize;
		}
		else
		{
			Link->ReleaseBuffered(FrameSize);
		}
		if (Outbox.Num() > 0 || bCloseQueued || HeldBytes > 0)
		{
			lws_callback_on_writable(InWsi);
		}
		return 0;
	}
	if (bCloseQueued)
	{
		// Our close (spec 2.5, T20): its code and reason, -1 from this WRITEABLE and 0 from every one after it, so LWS waits
		// for the peer's echo [close.ours_noecho_wait_ms, close.ours_sequence]
		lws_close_reason(InWsi, static_cast<lws_close_status>(CloseCode), CloseReason.GetData(), static_cast<size_t>(CloseReason.Num()));
		bCloseSent = true;
		// LWS's own 5 s close-ack timer starts with this frame (T20): Task 9's OnEnded reads it (review conc I-5)
		CloseSentAt = FPlatformTime::Seconds();
		UncountClose();
		return -1;
	}
	return 0;
}

void VaRest::WebSocket::Lws::FConn::ReleaseHeld()
{
	if (HeldBytes > 0 && Wsi && !lws_partial_buffered(Wsi))
	{
		const int64 Bytes = HeldBytes;
		HeldBytes = 0;
		Link->ReleaseBuffered(Bytes);
	}
}

void VaRest::WebSocket::Lws::FConn::DropOutbox()
{
	int64 Bytes = HeldBytes;
	for (const FVaRestSocketFrame& Frame : Outbox)
	{
		Bytes += Frame.Length;
	}
	Outbox.Empty();
	HeldBytes = 0;
	if (Bytes > 0)
	{
		Link->BufferedBytes.fetch_sub(Bytes);
	}
}
```

- [ ] **Step 6: The transport** — create `Source/VaRestWebSocket/Private/VaRestLwsTransport.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestLwsService.h"
#include "VaRestSocketTransport.h"

namespace VaRest::WebSocket::Lws
{
	/** IVaRestSocketTransport on one FService: VaRest's own client on the engine-bundled libwebsockets (spec 2, ruling R1). Game thread */
	class FTransport final : public IVaRestSocketTransport
	{
	public:
		explicit FTransport(FServiceOptions InOptions);
		virtual ~FTransport() override;

		virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents = nullptr) override;

		/** The context and the thread, made once (plan decision 8); false when that failed, or after Shutdown */
		bool EnsureStarted();
		/** Every connection ends (Shutdown), the thread stops, the context goes; later connects end with Shutdown */
		void Shutdown();
		/** A forking server's child starts again, with a context and a thread of its own (LwsWebSocketsManager.cpp:260-279) */
		void ResetAfterFork();
		/** The service's counters; null before the first start */
		const FServiceCounters* GetCounters() const;

	private:
		FServiceOptions Options;
		TSharedPtr<FService, ESPMode::ThreadSafe> Service;
		bool bShutDown = false;
		uint64 NextId = 1;
	};

	/** Game thread: the process's transport (plan decision 8) */
	FTransport& GetDefault();
	/** Game thread, StartupModule: plan decision 8's start in a shared LWS copy */
	void OnModuleStartup();
	/** Game thread: OnEnginePreExit and ShutdownModule; nothing when the default transport was never made */
	void ShutdownDefault();
	/** Game thread: OnChildEndFramePostFork */
	void ResetDefaultAfterFork();
}
```

Create `Source/VaRestWebSocket/Private/VaRestLwsTransport.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsTransport.h"

#include "Async/Async.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsConn.h"
#include "VaRestSocketLink.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws::VaRestLwsTransportPrivate
{
	/** Set once GetDefaultTransport made the default transport */
	std::atomic<bool> bDefaultMade{ false };

	/** The consumer's handle on one FConn: commands go to the service thread; the link is shared with it */
	class FHandle final : public IVaRestSocketConnection
	{
	public:
		FHandle(TWeakPtr<FService, ESPMode::ThreadSafe> InService, uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink, FString InLabel)
			: Service(MoveTemp(InService))
			, Id(InId)
			, Link(MoveTemp(InLink))
			, Label(MoveTemp(InLabel))
		{
		}

		virtual ~FHandle() override
		{
			// Any thread (spec 1.4): the events stop at once; the service thread closes or cancels the connection
			Link->bReleased = true;
			Post([ConnId = Id](FService& Self)
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Release(Self);
				}
			});
		}

		virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) override
		{
			return Link->PopAll(Out);
		}

		virtual bool Send(FVaRestSocketFrame&& Frame) override
		{
			check(IsInGameThread());
			// Review conc M-6: after Close the service thread would drop it (FConn::Send, bCloseQueued); say so here instead
			if (!Frame.IsValid() || Link->bEnded.load() || Link->bCloseRequested.load())
			{
				return false;
			}
			// Counted on the caller's side, so the buffered amount includes what the service thread has not seen yet (spec 2.4)
			Link->BufferedBytes.fetch_add(Frame.Length);
			Post([ConnId = Id, Moved = MoveTemp(Frame), Shared = Link](FService& Self) mutable
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Send(Self, MoveTemp(Moved));
				}
				else
				{
					Shared->BufferedBytes.fetch_sub(Moved.Length);
				}
			});
			return true;
		}

		virtual void Close(int32 Code, FStringView Reason) override
		{
			check(IsInGameThread());
			Link->bCloseRequested = true;
			bool bReplaced = false;
			bool bTruncated = false;
			const int32 Sent = SanitizeCloseCode(Code, bReplaced);
			TArray<uint8> Bytes = CloseReasonBytes(Reason, bTruncated);
			if (bReplaced)
			{
				UE_LOG(LogVaRestSocket, Warning, TEXT("%s: close code %d cannot be sent (RFC 6455 7.4); 1000 is sent instead"), *Label, Code);
			}
			if (bTruncated)
			{
				UE_LOG(LogVaRestSocket, Warning, TEXT("%s: the close reason is cut to %d bytes (RFC 6455 5.5)"), *Label, MaxCloseReasonBytes);
			}
			Post([ConnId = Id, Sent, Moved = MoveTemp(Bytes)](FService& Self) mutable
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Close(Self, Sent, MoveTemp(Moved), EVaRestSocketCloseBy::Client, EVaRestSocketFailure::None);
				}
			});
		}

		virtual void Consumed(int64 Bytes) override
		{
			Link->PendingInboundBytes.fetch_sub(Bytes);
		}

		virtual int64 GetBufferedAmount() const override
		{
			return FMath::Max<int64>(Link->BufferedBytes.load(), 0);
		}

		virtual int64 GetPendingInboundBytes() const override
		{
			return Link->PendingInboundBytes.load();
		}

		virtual bool HasEnded() const override
		{
			return Link->bEnded.load();
		}

	private:
		void Post(FCommand&& Command) const
		{
			if (const TSharedPtr<FService, ESPMode::ThreadSafe> Pinned = Service.Pin())
			{
				Pinned->Enqueue(MoveTemp(Command));
			}
		}

		const TWeakPtr<FService, ESPMode::ThreadSafe> Service;
		const uint64 Id;
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link;
		const FString Label;
	};
}

VaRest::WebSocket::Lws::FTransport::FTransport(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

VaRest::WebSocket::Lws::FTransport::~FTransport()
{
	if (Service.IsValid())
	{
		check(IsInGameThread());
		Service->Shutdown();
	}
}

bool VaRest::WebSocket::Lws::FTransport::EnsureStarted()
{
	check(IsInGameThread());
	if (bShutDown)
	{
		return false;
	}
	if (!Service.IsValid())
	{
		Service = MakeShared<FService, ESPMode::ThreadSafe>(Options);
	}
	return Service->IsRunning() || Service->Start();
}

void VaRest::WebSocket::Lws::FTransport::Shutdown()
{
	check(IsInGameThread());
	bShutDown = true;
	if (Service.IsValid())
	{
		Service->Shutdown();
	}
}

void VaRest::WebSocket::Lws::FTransport::ResetAfterFork()
{
	check(IsInGameThread());
	if (Service.IsValid())
	{
		Service->Shutdown();
		Service.Reset();
	}
	bShutDown = false;
}

const VaRest::WebSocket::Lws::FServiceCounters* VaRest::WebSocket::Lws::FTransport::GetCounters() const
{
	return Service.IsValid() ? &Service->GetCounters() : nullptr;
}

TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> VaRest::WebSocket::Lws::FTransport::Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsTransportPrivate;
	check(IsInGameThread());
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	const uint64 ConnId = NextId++;
	const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link = MakeShared<FSocketLink, ESPMode::ThreadSafe>(MoveTemp(OnEvents));
	TUniquePtr<FConn> Conn = MakeUnique<FConn>(ConnId, Link, MoveTemp(Request));
	Conn->Prepare(Options);
	const FString Label = Conn->GetLabel();
	if (!EnsureStarted())
	{
		// No service thread to report from: the Error is queued here and its notification posted, never a call back inside Connect (E7)
		FVaRestSocketEvent Event;
		Event.Type = EVaRestSocketEventType::Error;
		Event.End.Failure = EVaRestSocketFailure::Shutdown;
		Event.End.Detail = TEXT("the WebSocket transport is shut down or could not start");
		Link->Push(MoveTemp(Event), false);
		AsyncTask(ENamedThreads::GameThread, [Link] { Link->Notify(); });
		return MakeShared<FHandle, ESPMode::ThreadSafe>(TWeakPtr<FService, ESPMode::ThreadSafe>(), ConnId, Link, Label);
	}
	if (Conn->NeedsTlsCount())
	{
		// Re-review N-2: counted here, on the game thread, where every FService::Start reads the count, so a transport started
		// right after this Connect -- before the service thread ran the Adopt -- still sees the attempt (VaRest-rti, I-3)
		Conn->MarkTlsCounted();
	}
	Service->Enqueue([Moved = MoveTemp(Conn)](FService& Self) mutable { Self.Adopt(MoveTemp(Moved)); });
	return MakeShared<FHandle, ESPMode::ThreadSafe>(Service, ConnId, Link, Label);
}

TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> VaRest::WebSocket::GetDefaultTransport()
{
	// Leaked on purpose, as VaRest::Http::GetDefaultTransport: nothing of it runs at static destruction; the engine-exit hook shut it down
	static TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe>* const Instance = []
	{
		VaRest::WebSocket::Lws::VaRestLwsTransportPrivate::bDefaultMade = true;
		return new TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe>(
			MakeShared<VaRest::WebSocket::Lws::FTransport, ESPMode::ThreadSafe>(VaRest::WebSocket::Lws::FServiceOptions()));
	}();
	return *Instance;
}

VaRest::WebSocket::Lws::FTransport& VaRest::WebSocket::Lws::GetDefault()
{
	return static_cast<FTransport&>(VaRest::WebSocket::GetDefaultTransport().Get());
}

void VaRest::WebSocket::Lws::OnModuleStartup()
{
	constexpr bool bSharedCopy = IS_MONOLITHIC || UE_MERGED_MODULES;
	// In a monolithic or merged build, ModuleExists lists the statically linked modules (ModuleManager.cpp:268-300)
	const auto IsLinked = [](const TCHAR* Name) { return FModuleManager::Get().ModuleExists(Name); };
	if (!ShouldCreateAtStartup(bSharedCopy, IsLinked))
	{
		return;
	}
	// Plan decision 8 (VaRest-rti): the engine's WebSockets module first -- it makes its one context at startup
	// (WebSocketsModule.cpp:29-43) -- so that context exists before any VaRest handshake; then VaRest's own, before game code
	// can start an engine handshake. Creations by WebSocketServer, WebSocketNetworking and HttpNetworkReplayStreaming later
	// remain a documented residual risk (spec 1.2)
	if (IsLinked(TEXT("WebSockets")))
	{
		FModuleManager::Get().LoadModule(TEXT("WebSockets"));
	}
	GetDefault().EnsureStarted();
}

void VaRest::WebSocket::Lws::ShutdownDefault()
{
	if (VaRestLwsTransportPrivate::bDefaultMade.load())
	{
		GetDefault().Shutdown();
	}
}

void VaRest::WebSocket::Lws::ResetDefaultAfterFork()
{
	if (VaRestLwsTransportPrivate::bDefaultMade.load())
	{
		GetDefault().ResetAfterFork();
	}
	OnModuleStartup();
}
```

- [ ] **Step 7: The module's hooks** — replace the body of `Source/VaRestWebSocket/Private/VaRestWebSocketModule.cpp` with:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/CoreDelegates.h"
#include "Misc/Fork.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsTransport.h"
#include "VaRestWebSocketLog.h"

DEFINE_LOG_CATEGORY(LogVaRestSocket);
LLM_DEFINE_TAG(VaRest_WebSocket);

class FVaRestWebSocketModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Before any module shuts down (CoreDelegates.h:261-262); an automation run's Quit force-exits, so there neither this
		// nor ShutdownModule runs (CLAUDE.md, VaRestHttpModule.cpp)
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&VaRest::WebSocket::Lws::ShutdownDefault);
		if (FForkProcessHelper::IsForkRequested())
		{
			// A forking server's child makes its own context and thread (LwsWebSocketsManager.cpp:260-279; plan decision 22)
			ForkHandle = FCoreDelegates::OnChildEndFramePostFork.AddStatic(&VaRest::WebSocket::Lws::ResetDefaultAfterFork);
		}
		VaRest::WebSocket::Lws::OnModuleStartup();
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnChildEndFramePostFork.Remove(ForkHandle);
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		VaRest::WebSocket::Lws::ShutdownDefault();
	}

private:
	FDelegateHandle PreExitHandle;
	FDelegateHandle ForkHandle;
};

IMPLEMENT_MODULE(FVaRestWebSocketModule, VaRestWebSocket);
```

- [ ] **Step 8: The test hooks** — create `Source/VaRestWebSocket/Public/VaRestSocketTesting.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Test hooks for VaRest's libwebsockets transport (WebSocket spec 16.2). Compiled with WITH_DEV_AUTOMATION_TESTS only: not
 * for games. One rule for every test (VaRest-rti; the W0 probe's decision 16; review conc I-3): never make a transport while
 * another transport of this module has a wss attempt that LWS has not destroyed yet -- an attempt that has ended still runs
 * its handshake until LWS destroys it, so wait for that transport's LiveConnections to reach 0. Each transport makes an LWS
 * context, and every context creation re-allocates this LWS copy's OpenSSL ex-data index under a handshake in flight
 * [coexist.context_create_reallocates_ssl_index]. The rule is enforced by shape too: a transport's first connect makes no
 * context while GetTlsHandshakesInFlight() is not 0, and ends with Shutdown instead.
 */
namespace VaRest::WebSocket::Testing
{
	struct FLwsTransportOptions
	{
		/** PEM certificates trusted on top of the engine's roots: the test CA (Tools/testserver/certs/test-ca.pem, GET /tls/ca.pem) */
		TArray<uint8> ExtraTrustedPem;
		/** Non-empty: every connection's TCP goes here; the URL's host stays the Host header and the TLS name (the probe's FConnectSpec::Address) */
		FString TcpAddressOverride;
		/** true: the URL filter is Allowlist (scheme -> domains, URLRequestFilter.h:16-27), not [Online.HttpManager <scheme>] in the engine ini */
		bool bOverrideAllowlist = false;
		TArray<TPair<FString, TArray<FString>>> Allowlist;
		/** lws_service's cap while connections exist (spec 2.1: 1000 ms), and while a kill is in flight (plan decision 12: 100 ms) */
		int32 ActiveWaitCapMs = 1000;
		int32 ShortWaitCapMs = 100;
		/** Shutdown waits at most this long for the close frames already asked for (review conc M-3) */
		double ExitFlushSeconds = 0.25;
		/** The first connect makes the context, then fails as if the thread could not start (review conc I-2) */
		bool bFailThreadForTest = false;
	};

	/** A snapshot of a transport's counters */
	struct FLwsCounters
	{
		/** Connections LWS has not destroyed yet */
		int32 LiveConnections = 0;
		int32 KillsIssued = 0;
		int32 KillsLanded = 0;
		/** lws_create_context calls, failed ones included (review conc I-2) */
		int32 ContextCreations = 0;
		/** Closes asked for on open connections and not written yet (review conc M-3) */
		int32 UnsentCloses = 0;
		/** Seconds from lws_set_timeout(LWS_TO_KILL_ASYNC) to WSI_DESTROY: the last, and the longest so far; -1 before the first (VaRest-wcv) */
		double LastKillToDestroySeconds = -1.0;
		double MaxKillToDestroySeconds = -1.0;
	};

	class ILwsTestTransport : public IVaRestSocketTransport
	{
	public:
		/** Game thread: ends every connection (each gets a terminal event; Failure Shutdown when it had none), stops the thread, destroys the context. The destructor calls it too */
		virtual void Shutdown() = 0;
		/** Game thread */
		virtual FLwsCounters GetCounters() const = 0;
	};

	/** Game thread: a transport with a context and a service thread of its own, started at its first Connect */
	VARESTWEBSOCKET_API TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> MakeLwsTransport(FLwsTransportOptions Options);

	/** Linux: the file that holds this module's lws_create_context (dladdr); elsewhere "(not on this platform)" (VaRest-5ia M-1) */
	VARESTWEBSOCKET_API FString DescribeLwsCopy();
	/** This module's lws_create_context, to compare with another module's copy */
	VARESTWEBSOCKET_API const void* GetLwsCreateContextAddress();
	/** lwsl_visible for this module's LWS copy: "err=1 warn=2 notice=0 info=0 client=0" (libwebsockets.h:434-443) */
	VARESTWEBSOCKET_API FString DescribeLwsLogLevel();

	/** Any thread: this module's wss attempts that LWS has neither established nor destroyed, across every transport (review conc I-3) */
	VARESTWEBSOCKET_API int32 GetTlsHandshakesInFlight();

	/** Game thread: counts as one wss attempt in flight while it lives, so no transport of this module makes a context (review conc I-3) */
	class VARESTWEBSOCKET_API FScopedTlsHandshakeInFlight
	{
	public:
		FScopedTlsHandshakeInFlight();
		~FScopedTlsHandshakeInFlight();
		FScopedTlsHandshakeInFlight(const FScopedTlsHandshakeInFlight&) = delete;
		FScopedTlsHandshakeInFlight& operator=(const FScopedTlsHandshakeInFlight&) = delete;
	};
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

Create `Source/VaRestWebSocket/Private/VaRestSocketTesting.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketTesting.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/Paths.h"
#include "VaRestLwsInclude.h"
#include "VaRestLwsTransport.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace VaRest::WebSocket::Testing::VaRestSocketTestingPrivate
{
	class FTestTransport final : public ILwsTestTransport
	{
	public:
		explicit FTestTransport(VaRest::WebSocket::Lws::FServiceOptions InOptions)
			: Inner(MoveTemp(InOptions))
		{
		}

		virtual ~FTestTransport() override { Inner.Shutdown(); }

		virtual TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents) override
		{
			return Inner.Connect(MoveTemp(Request), MoveTemp(OnEvents));
		}

		virtual void Shutdown() override { Inner.Shutdown(); }

		virtual FLwsCounters GetCounters() const override
		{
			FLwsCounters Out;
			if (const VaRest::WebSocket::Lws::FServiceCounters* Counters = Inner.GetCounters())
			{
				Out.LiveConnections = Counters->LiveConnections.load();
				Out.KillsIssued = Counters->KillsIssued.load();
				Out.KillsLanded = Counters->KillsLanded.load();
				Out.ContextCreations = Counters->ContextCreations.load();
				Out.UnsentCloses = Counters->UnsentCloses.load();
				Out.LastKillToDestroySeconds = Counters->LastKillToDestroySeconds.load();
				Out.MaxKillToDestroySeconds = Counters->MaxKillToDestroySeconds.load();
			}
			return Out;
		}

	private:
		VaRest::WebSocket::Lws::FTransport Inner;
	};
}

TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> VaRest::WebSocket::Testing::MakeLwsTransport(FLwsTransportOptions Options)
{
	VaRest::WebSocket::Lws::FServiceOptions ServiceOptions;
	ServiceOptions.ExtraTrustedPem = MoveTemp(Options.ExtraTrustedPem);
	ServiceOptions.TcpAddressOverride = MoveTemp(Options.TcpAddressOverride);
	ServiceOptions.bOverrideAllowlist = Options.bOverrideAllowlist;
	ServiceOptions.Allowlist = MoveTemp(Options.Allowlist);
	ServiceOptions.ActiveWaitCapMs = Options.ActiveWaitCapMs;
	ServiceOptions.ShortWaitCapMs = Options.ShortWaitCapMs;
	ServiceOptions.ExitFlushSeconds = Options.ExitFlushSeconds;
	ServiceOptions.bFailThreadForTest = Options.bFailThreadForTest;
	ServiceOptions.ThreadName = TEXT("VaRestLwsTest");
	return MakeShared<VaRestSocketTestingPrivate::FTestTransport, ESPMode::ThreadSafe>(MoveTemp(ServiceOptions));
}

FString VaRest::WebSocket::Testing::DescribeLwsCopy()
{
#if PLATFORM_LINUX
	Dl_info Info = {};
	if (dladdr(reinterpret_cast<void*>(&lws_create_context), &Info) != 0 && Info.dli_fname)
	{
		return FPaths::GetCleanFilename(UTF8_TO_TCHAR(Info.dli_fname));
	}
	return TEXT("(dladdr found no file)");
#else
	return TEXT("(not on this platform)");
#endif
}

const void* VaRest::WebSocket::Testing::GetLwsCreateContextAddress()
{
	return reinterpret_cast<const void*>(&lws_create_context);
}

FString VaRest::WebSocket::Testing::DescribeLwsLogLevel()
{
	return FString::Printf(TEXT("err=%d warn=%d notice=%d info=%d client=%d"), lwsl_visible(LLL_ERR), lwsl_visible(LLL_WARN), lwsl_visible(LLL_NOTICE),
		lwsl_visible(LLL_INFO), lwsl_visible(LLL_CLIENT));
}

int32 VaRest::WebSocket::Testing::GetTlsHandshakesInFlight()
{
	return VaRest::WebSocket::Lws::TlsHandshakesInFlight().load();
}

VaRest::WebSocket::Testing::FScopedTlsHandshakeInFlight::FScopedTlsHandshakeInFlight()
{
	VaRest::WebSocket::Lws::TlsHandshakesInFlight().fetch_add(1);
}

VaRest::WebSocket::Testing::FScopedTlsHandshakeInFlight::~FScopedTlsHandshakeInFlight()
{
	VaRest::WebSocket::Lws::TlsHandshakesInFlight().fetch_sub(1);
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 9: The runner leaves the proxy variables out** (review conc M-12) — with no configured proxy, LWS 3.0 reads `http_proxy` from the environment itself (`context.c.o` imports `getenv`; the archive holds the string `http_proxy`; ruling Q7: VaRest follows the engine there), and the redirect veto's behaviour under a proxy is W1b B1's question: a developer's `http_proxy` must not turn this suite into `Redirect` ends. In `Tools/run_tests.sh`, replace the editor line of the `timeout` command,

```bash
	"$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
```

with

```bash
	env -u http_proxy -u https_proxy -u HTTP_PROXY -u HTTPS_PROXY "$ENGINE/Engine/Binaries/Linux/UnrealEditor-Cmd" "$PROJECT" \
```

and add above the `timeout` line the comment `# No proxy variables until W1b B1 measures LWS 3.0 behind a proxy (review conc M-12): env execs the editor, so the PID file still names it.` (`env` replaces itself with the editor, so `/proc/<pid>/comm` is still `UnrealEditor-Cm`, which the timeout handler checks.)

- [ ] **Step 10: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Connect` then `Tools/run_tests.sh --no-build Probe.VaRest.Lws.Connect.LocalhostAddress`
Expected: `run_tests: 14 passed, 0 failed` (Open, Handshake, Origin, Protocols, Refused, BadRequest, NotAllowed, HeaderRoom, Write, Release, Shutdown, Default, StartRefusals, TlsHandshakeCounted); then `run_tests: 1 passed, 0 failed`. Read the probe's AddInfo line in the log (`grep -n "ws://localhost against" <Host>/Saved/Logs/VaRestTests.log`) and put it in the task report: Task 14 records it on VaRest-gi0 (ruling Q5).

- [ ] **Step 11: Restore-the-bug** — (a) E7: in `FTransport::Connect`, when `Conn` holds a `PendingFailure` after `Prepare`, push its Error right there with `Link->Push(MoveTemp(Event))` (notify at once, on the game thread) and do not enqueue the connection: `Connect.BadRequest` FAILS at "never on the game thread, so never inside Connect"; restore; PASSES. (b) In `FConn::OnFilterPreEstablish`, drop the `lws_hdr_copy` block: `Connect.Protocols` FAILS at "the second offered protocol, selected"; restore; PASSES. (c) In `FConn::Release`, replace the `Close(...)` with `Kill(Service)`: `Connect.Release` FAILS at "the server got 1001" (-1: no close frame); restore; PASSES. (d) Review conc I-2: in `FService::Start` delete `|| bStartFailed`: `Connect.StartRefusals` FAILS at "one lws_create_context for both connects" (2; the error line comes twice); restore; PASSES. (e) Review conc I-3: in `FConn::OnDestroyed` delete `UncountTls();` (and in `~FConn`, its backstop), and run `Connect.TlsHandshakeCounted` alone (a count left at 1 refuses every later context in that run): it FAILS at "the count is back to 0" (the cancelled wss attempt never reached `CLIENT_ESTABLISHED`); restore; PASSES. (f) Review conc M-12, re-review N-4: `http_proxy=http://127.0.0.1:9 no_proxy=127.0.0.1,localhost Tools/run_tests.sh --no-build VaRest.Integration.WebSocket.Transport.Connect.Open` PASSES; take the `env -u ...` back out of `Tools/run_tests.sh` and run the same line: it FAILS at "opened" for ws and wss (LWS takes the proxy from the environment; `no_proxy` keeps the engine's curl -- which honours it -- fetching `/tls/ca.pem` directly, so only libwebsockets can fail the test); restore; PASSES. If the run without `env -u` PASSES, LWS 3.0 does not use the variable on this path: report it, it is a W1b B1 fact. (g) Re-review N-2: move the count back to the service thread (delete the `NeedsTlsCount()` block in `FTransport::Connect` and raise the count at the top of `FConn::Start`): `Connect.TlsHandshakeCounted` FAILS at "counted as soon as Connect returns" (0: the service thread has not run the Adopt yet, nearly always); restore; PASSES.

- [ ] **Step 12: The suite so far**

Run: `Tools/run_tests.sh VaRest.WebSocket` then `Tools/run_tests.sh VaRest.Integration.WebSocket`
Expected: `0 failed` both.

- [ ] **Step 13: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConn.h Source/VaRestWebSocket/Private/VaRestLwsConn.cpp \
  Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp Source/VaRestWebSocket/Private/VaRestLwsTransport.h \
  Source/VaRestWebSocket/Private/VaRestLwsTransport.cpp Source/VaRestWebSocket/Public/VaRestSocketTesting.h \
  Source/VaRestWebSocket/Private/VaRestSocketTesting.cpp Source/VaRestWebSocket/Public/VaRestSocketTransport.h \
  Source/VaRestWebSocket/Private/VaRestWebSocketModule.cpp Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.h \
  Source/VaRestTests/Private/WebSocket/VaRestWsTestUtil.cpp Source/VaRestTests/Private/WebSocket/VaRestWsConnectTest.cpp \
  Tools/run_tests.sh
git commit -m "ws: W1a task 7 -- connect, handshake, open and refusal over ws and wss: headers in LWS's room, raw Origin, the selected subprotocol, every pre-open end classified, whole-message writes and our close, release, the default transport and the startup policy

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 8: Fragments in place, the buffered amount, `Drained`, sends before the open (SW-4)

Messages over 64 KiB go out as `NO_FIN`/`CONTINUATION` fragments written from the frame's own storage (plan decision 6), one per `WRITEABLE`, so W1b's PINGs and the close frame can go between them (spec §2.4, RFC 6455 §5.4); the buffered amount follows plan decision 7.

**Files:**
- Modify: `Source/VaRestWebSocket/Private/VaRestLwsConn.h` (one member), `Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp` (`OnWriteable`, `DropOutbox`)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsSendTest.cpp`

**Interfaces:**
- Consumes: Task 7's `FConn`, `FragmentBytes` (Task 6), `FSocketLink::ReleaseBuffered`/`NoteBuffered` (Task 3), the test helpers.
- Produces: `FConn::NextFragment` (private, `int32`): fragments of `Outbox[0]` already written; W1b's reply lane reads it ("at a message boundary" = `NextFragment == 0`).

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestTests/Private/WebSocket/VaRestWsSendTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: fragmented and large sends, frames with a payload offset (SW-4), the buffered amount and Drained, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsSendTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	struct FFrameSeen
	{
		int32 Op = -1;
		int32 Fin = -1;
		int32 Len = -1;
	};

	/** The client's data frames (op 0, 1, 2) as the server journaled them */
	TArray<FFrameSeen> DataFrames(const FVaRestJson& Journal)
	{
		TArray<FFrameSeen> Out;
		for (const FVaRestJson& Each : JournalEvents(Journal, TEXT("frame")))
		{
			const int32 Op = Each[TEXT("op")].AsInt(-1);
			if (Op >= 0 && Op <= 2)
			{
				Out.Add({ Op, Each[TEXT("fin")].AsInt(-1), Each[TEXT("len")].AsInt(-1) });
			}
		}
		return Out;
	}

	TArray<uint8> Pattern(int32 Size)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Out[Index] = static_cast<uint8>((Index * 7) % 251);
		}
		return Out;
	}

	/** Opens Path on Scheme; false (and drained) when it did not open */
	bool OpenOn(ILwsTestTransport& Transport, const FScheme& Scheme, const FString& Path, TUniquePtr<FEvents>& Out, FVaRestSocketConnectRequest Req = FVaRestSocketConnectRequest())
	{
		Req.Url = Scheme.Base + Path;
		Out = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
		if (Out->WaitType(EVaRestSocketEventType::Open))
		{
			return true;
		}
		Out->WaitEnd();
		return false;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsSendTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendFragmentsTest, "VaRest.Integration.WebSocket.Transport.Send.Fragments", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendFragmentsTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		// 300000 bytes: four 64 KiB fragments and a fifth of 37856 (spec 2.4, 15)
		const FString Text = FString::ChrN(300000, TEXT('a'));
		FVaRestSocketFrame Frame = FVaRestSocketFrame::Text(FStringView(Text));
		const FString Expected = Sha256Hex(Frame.GetPayload());
		Events->Connection->Send(MoveTemp(Frame));
		Events->Connection->Close(1000, TEXT("done"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events->WaitEnd());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FFrameSeen> Frames = DataFrames(Journal);
		if (!TestEqual(FString::Printf(TEXT("%s: five frames on the wire"), Scheme.Name), Frames.Num(), 5))
		{
			continue;
		}
		const int32 Lens[] = { 65536, 65536, 65536, 65536, 37856 };
		for (int32 Index = 0; Index < 5; ++Index)
		{
			TestEqual(FString::Printf(TEXT("%s: frame %d is %s"), Scheme.Name, Index, Index == 0 ? TEXT("TEXT") : TEXT("CONTINUATION")), Frames[Index].Op, Index == 0 ? 1 : 0);
			TestEqual(FString::Printf(TEXT("%s: frame %d's FIN"), Scheme.Name, Index), Frames[Index].Fin, Index == 4 ? 1 : 0);
			TestEqual(FString::Printf(TEXT("%s: frame %d's length"), Scheme.Name, Index), Frames[Index].Len, Lens[Index]);
		}
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		TestTrue(FString::Printf(TEXT("%s: one message, byte for byte"), Scheme.Name), Messages.Num() == 1 && Messages[0][TEXT("sha256")].AsString().Equals(Expected, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendOffsetTest, "VaRest.Integration.WebSocket.Transport.Send.Offset", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendOffsetTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		// SW-4: an encoder that kept a 40-byte gap (0xAA) before its payload
		TArray<uint8> Storage;
		Storage.Init(0xAA, 40);
		for (const TCHAR C : FString(TEXT("offset-ok")))
		{
			Storage.Add(static_cast<uint8>(C));
		}
		TOptional<FVaRestSocketFrame> Frame = FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Storage), 40, 9);
		// A long one too, so a fragment after the first writes its header into the bytes before it (plan decision 6)
		TArray<uint8> Long;
		Long.Init(0xAA, 100);
		Long.Append(Pattern(200000));
		TOptional<FVaRestSocketFrame> LongFrame = FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Binary, MoveTemp(Long), 100, 200000);
		if (!TestTrue(TEXT("both frames are taken"), Frame.IsSet() && LongFrame.IsSet()))
		{
			Events->Connection->Close(1000, TEXT("done"));
			Events->WaitEnd();
			continue;
		}
		const FString LongSha = Sha256Hex(Pattern(200000));
		Events->Connection->Send(MoveTemp(Frame.GetValue()));
		Events->Connection->Send(MoveTemp(LongFrame.GetValue()));
		Events->Connection->Close(1000, TEXT("done"));
		Events->WaitEnd();
		const TArray<FVaRestJson> Messages = JournalEvents(FetchJournal(*this, Site, Key), TEXT("message"));
		if (!TestEqual(FString::Printf(TEXT("%s: two messages"), Scheme.Name), Messages.Num(), 2))
		{
			continue;
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: the payload from its offset, not the gap"), Scheme.Name), Messages[0][TEXT("text")].AsString(), FString(TEXT("offset-ok")));
		TestEqualSensitive(FString::Printf(TEXT("%s: a long frame at an offset, byte for byte"), Scheme.Name), Messages[1][TEXT("sha256")].AsString(), LongSha);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendBufferedTest, "VaRest.Integration.WebSocket.Transport.Send.BufferedAndDrained", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendBufferedTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req;
		Req.SendHighWater = 256 * 1024;
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events, MoveTemp(Req))))
		{
			continue;
		}
		const TArray<uint8> Megabyte = Pattern(1024 * 1024);
		const FString Expected = Sha256Hex(Megabyte);
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Events->Connection->Send(FVaRestSocketFrame::Binary(Megabyte));
		}
		TestTrue(FString::Printf(TEXT("%s: the sends are buffered"), Scheme.Name), Events->Connection->GetBufferedAmount() > 0);
		// Review conc I-6: the service thread may write the first megabyte out before the game thread adds the second, and
		// that early Drained is right (spec 2.4). So the wait is for a Drained with nothing left buffered, not for the first
		const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Events->Connection;
		const bool bSettled = Events->WaitFor([Connection](const TArray<FVaRestSocketEvent>& All)
		{
			return Connection->GetBufferedAmount() == 0 && All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.Type == EVaRestSocketEventType::Drained; });
		}, 30.0);
		TestTrue(FString::Printf(TEXT("%s: Drained after the high-water mark, and nothing buffered then (spec 2.4) (%s)"), Scheme.Name, *Events->Describe()), bSettled);
		TestTrue(FString::Printf(TEXT("%s: at least one Drained"), Scheme.Name), Events->Count(EVaRestSocketEventType::Drained) >= 1);
		Events->Connection->Close(1000, TEXT("done"));
		Events->WaitEnd();
		const TArray<FVaRestJson> Messages = JournalEvents(FetchJournal(*this, Site, Key), TEXT("message"));
		bool bAllIntact = Messages.Num() == 8;
		for (const FVaRestJson& Each : Messages)
		{
			bAllIntact = bAllIntact && Each[TEXT("len")].AsInt() == 1024 * 1024 && Each[TEXT("sha256")].AsString().Equals(Expected, ESearchCase::CaseSensitive);
		}
		TestTrue(FString::Printf(TEXT("%s: eight 1 MiB messages arrived intact (got %d)"), Scheme.Name, Messages.Num()), bAllIntact);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendBeforeOpenTest, "VaRest.Integration.WebSocket.Transport.Send.BeforeOpen", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendBeforeOpenTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		// Plan decision 18: queued before the open and written after it -- LWS drops a WRITEABLE request made during the handshake (spec 2.4)
		FEvents Events(Transport->Connect(Request(Scheme, TEXT("/ws/sink?key=") + Key)));
		TestTrue(FString::Printf(TEXT("%s: a send before the open is accepted"), Scheme.Name), Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("early")))));
		TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open));
		// Review conc M-8: closed and ended first, so the journal's default wait (for the end, ws.py:15) is deterministic
		Events.Connection->Close(1000, TEXT("done"));
		Events.WaitEnd();
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		TestTrue(FString::Printf(TEXT("%s: it went out after the open"), Scheme.Name), Messages.Num() == 1 && Messages[0][TEXT("text")].AsString().Equals(TEXT("early"), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendCloseAfterDataTest, "VaRest.Integration.WebSocket.Transport.Send.CloseAfterData", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendCloseAfterDataTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		Events->Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("one"))));
		Events->Connection->Send(FVaRestSocketFrame::Binary(Pattern(200000)));
		Events->Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("three"))));
		// Plan decision 18: the close goes after what was queued, even a fragmented message
		Events->Connection->Close(4321, TEXT("after"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events->WaitEnd());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TArray<FString> Order;
		for (const FVaRestJson& Each : Journal[TEXT("events")].Elements())
		{
			const FString Kind = Each[TEXT("ev")].AsString();
			if (Kind.Equals(TEXT("message"), ESearchCase::CaseSensitive) || Kind.Equals(TEXT("close"), ESearchCase::CaseSensitive))
			{
				Order.Add(Kind);
			}
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: three messages, then the close"), Scheme.Name), FString::Join(Order, TEXT(",")), FString(TEXT("message,message,message,close")));
		TestEqual(FString::Printf(TEXT("%s: with our code"), Scheme.Name), JournalCloseCode(Journal), 4321);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendEdgesTest, "VaRest.Integration.WebSocket.Transport.Send.Edges", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendEdgesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: an empty text is sent"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Text(FStringView())));
		TestTrue(FString::Printf(TEXT("%s: an empty binary is sent"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Binary(TConstArrayView<uint8>())));
		// Exactly one fragment's worth: one frame, FIN set
		TestTrue(FString::Printf(TEXT("%s: 64 KiB is sent"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Binary(Pattern(64 * 1024))));
		TestFalse(FString::Printf(TEXT("%s: an invalid frame is refused"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame()));
		Events->Connection->Close(1000, TEXT("done"));
		Events->WaitEnd();
		TestFalse(FString::Printf(TEXT("%s: a send after the end is refused"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("late")))));
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		TestTrue(FString::Printf(TEXT("%s: two empty messages, then 64 KiB"), Scheme.Name), Messages.Num() == 3 && Messages[0][TEXT("len")].AsInt() == 0
			&& Messages[1][TEXT("len")].AsInt() == 0 && Messages[2][TEXT("len")].AsInt() == 64 * 1024);
		const TArray<FFrameSeen> Frames = DataFrames(Journal);
		TestTrue(FString::Printf(TEXT("%s: 64 KiB is one final frame"), Scheme.Name), Frames.Num() == 3 && Frames[2].Fin == 1 && Frames[2].Len == 64 * 1024);
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Run them to see what fails**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Send`
Expected: `Send.Fragments` FAILS ("five frames on the wire": Task 7 writes one frame); `Send.Offset`, `Send.BufferedAndDrained`, `Send.BeforeOpen`, `Send.CloseAfterData` and `Send.Edges` may pass already (whole-message writes).

- [ ] **Step 3: The member** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, after `int64 HeldBytes = 0;` add:

```cpp
		/** Fragments of Outbox[0] already written (plan decision 6); 0 at a message boundary */
		int32 NextFragment = 0;
```

- [ ] **Step 4: Fragments in place** — in `Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp`, replace the whole `if (Outbox.Num() > 0) { ... }` block of `FConn::OnWriteable` with:

```cpp
	if (Outbox.Num() > 0)
	{
		FVaRestSocketFrame& Head = Outbox[0];
		// Plan decision 6: fragment k is written from the frame's storage, its header in the 16 bytes before it -- the tail of
		// fragment k-1, which LWS has sent or copied into its own buffer (T10, libwebsockets.h:4910-4932). FragmentBytes each
		// (spec 2.4), so a PING or the close frame can go between them (RFC 6455 5.4) [tx.ping_between_fragments]
		// FragmentStart and FragmentSize, never Start or Size: a local named like a member function of FConn (Start) is
		// C4458, an error on MSVC that clang's -Wshadow does not report (review spec I-3)
		const int64 FragmentStart = static_cast<int64>(NextFragment) * FragmentBytes;
		const int32 FragmentSize = static_cast<int32>(FMath::Min<int64>(FragmentBytes, Head.Length - FragmentStart));
		const bool bLast = FragmentStart + FragmentSize >= Head.Length;
		int32 Protocol = NextFragment == 0 ? (Head.Kind == EVaRestSocketFrameKind::Text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY) : LWS_WRITE_CONTINUATION;
		if (!bLast)
		{
			// T10: NO_FIN, then CONTINUATION, one frame each, as written [tx.no_fin_frames]
			Protocol |= LWS_WRITE_NO_FIN;
		}
		// One lws_write per WRITEABLE: a second in one callback returns -1 and is lost (spec 2.4) [tx.two_writes_one_writeable]
		const int Wrote = lws_write(InWsi, Head.Storage.GetData() + Head.Offset + FragmentStart, static_cast<size_t>(FragmentSize), static_cast<lws_write_protocol>(Protocol));
		if (bLast)
		{
			Outbox.RemoveAt(0);
			NextFragment = 0;
		}
		else
		{
			++NextFragment;
		}
		if (Wrote < 0)
		{
			// This fragment is neither in the outbox nor released: it leaves the count here, the rest in DropOutbox (EndOpen)
			Link->BufferedBytes.fetch_sub(FragmentSize);
			EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None, TEXT("lws_write failed"));
			return -1;
		}
		// A data write returns its payload length, a control frame's counts its header: only < 0 fails (spec 2.4)
		// [tx.partial_after_fragment]. lws_partial_buffered says LWS still holds part of this one [tx.large_write_result]
		if (lws_partial_buffered(InWsi))
		{
			HeldBytes += FragmentSize;
		}
		else
		{
			Link->ReleaseBuffered(FragmentSize);
		}
		if (Outbox.Num() > 0 || bCloseQueued || HeldBytes > 0)
		{
			lws_callback_on_writable(InWsi);
		}
		return 0;
	}
```

and replace `FConn::DropOutbox` with:

```cpp
void VaRest::WebSocket::Lws::FConn::DropOutbox()
{
	int64 Bytes = HeldBytes;
	for (int32 Index = 0; Index < Outbox.Num(); ++Index)
	{
		// The fragments of the head already written were released (or are held, counted above)
		Bytes += Outbox[Index].Length - (Index == 0 ? static_cast<int64>(NextFragment) * FragmentBytes : 0);
	}
	Outbox.Empty();
	NextFragment = 0;
	HeldBytes = 0;
	if (Bytes > 0)
	{
		Link->BufferedBytes.fetch_sub(Bytes);
	}
}
```

- [ ] **Step 5: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Send`
Expected: `run_tests: 6 passed, 0 failed`.

- [ ] **Step 6: Restore-the-bug** — (a) drop `Protocol |= LWS_WRITE_NO_FIN;`: `Send.Fragments` FAILS (the server refuses a CONTINUATION outside a message with 1002, so no message; or the FIN check fails); restore; PASSES. (b) Write from `Head.Storage.GetData() + VaRest::WebSocket::FrameHeadroom + FragmentStart` instead of `+ Head.Offset + FragmentStart`: `Send.Offset` FAILS ("two messages"); restore; PASSES. (c) In `FConn::OnEstablished` (`VaRestLwsConn.cpp`), drop the `lws_callback_on_writable(InWsi)` re-request: `Send.BeforeOpen` FAILS ("the send is Drained once written"); restore; PASSES. (d) Review conc I-6: in `FSocketLink::ReleaseBuffered` (`VaRestSocketLink.cpp`), drop the `Push(MoveTemp(Drained));` line: `Send.BufferedAndDrained` FAILS at "Drained after the high-water mark, and nothing buffered then" (30 s, no Drained); restore; PASSES.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConn.h Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp \
  Source/VaRestTests/Private/WebSocket/VaRestWsSendTest.cpp
git commit -m "ws: W1a task 8 -- 64 KiB fragments written in place from the frame's storage, payload offsets (SW-4), the buffered amount and Drained, sends before the open

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 9: Close — the peer's close, drops, our close and `CloseTimeout`

The peer's close is read by Task 2's rules (1005, 1002, normalised codes, repaired reasons) and echoed; a drop is 1006; our close is bounded by `CloseTimeout` (plan decisions 3, 4, 17). Spec §16.5 item 5 lands here.

**Files:**
- Modify: `Source/VaRestWebSocket/Private/VaRestLwsConn.h` (members, one function), `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp` (`Prepare`, `OnCallback`, `OnPeerClose`, `OnEnded`, `CheckDeadlines`, `NextDeadline`), `Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp` (`Close`)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsCloseTest.cpp`, `Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp`

**Interfaces:**
- Consumes: `ParsePeerClose`, `FPeerClose` (Task 2), `LwsCloseAckSeconds` (Task 6), Task 7's `FConn`.
- Produces: `FConn::OnPeerClose(const void* In, size_t Len)` (private), members `bPeerClosed`, `Peer`, `CloseDeadline`; `NextDeadline` now returns the close deadline while a close is pending (Task 11 adds the connect deadline beside it).

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestTests/Private/WebSocket/VaRestWsCloseTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: empty close -> 1005, close codes and UTF-8 reasons, unclean drop -> 1006, our close, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestUrl.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsCloseTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** The connection's Closed event, or null */
	const FVaRestSocketEvent* Closed(const FEvents& Events)
	{
		const FVaRestSocketEvent* End = Events.GetEnd();
		return End && End->Type == EVaRestSocketEventType::Closed ? End : nullptr;
	}

	/** Until the transport holds no connection: LWS destroyed every wsi */
	bool WaitIdle(ILwsTestTransport& Transport, double Seconds = 5.0)
	{
		return VaRest::Http::Testing::PumpUntil([&Transport] { return Transport.GetCounters().LiveConnections == 0; }, Seconds, true);
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsCloseTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsClosePeerCodesTest, "VaRest.Integration.WebSocket.Transport.Close.PeerCodes", VAREST_WS_TEST_FLAGS)

bool FVaRestWsClosePeerCodesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const int32 Code : { 1000, 1001, 1003, 1011, 4001, 4999 })
		{
			const FString Key = NewKey();
			const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/close/%d?reason=r%d&key=%s"), Code, Code, *Key)));
			const FVaRestSocketEvent* End = Closed(*Events);
			TestTrue(FString::Printf(TEXT("%s %d: Closed by the server, its code and reason, clean (%s)"), Scheme.Name, Code, *Events->Describe()),
				End && End->End.By == EVaRestSocketCloseBy::Server && End->End.Code == Code && End->End.bWasClean
				&& End->End.Reason.Equals(FString::Printf(TEXT("r%d"), Code), ESearchCase::CaseSensitive));
			// RFC 6455 5.5.1: VaRest returns 0 from PEER_INITIATED_CLOSE, so LWS answers [close.echo_on_return_0]
			TestTrue(FString::Printf(TEXT("%s %d: the client answered with a close"), Scheme.Name, Code), JournalCloseCode(FetchJournal(*this, Site, Key)) != -1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsClosePeerOddTest, "VaRest.Integration.WebSocket.Transport.Close.PeerOdd", VAREST_WS_TEST_FLAGS)

bool FVaRestWsClosePeerOddTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Spec 16.5 item 5: an empty close frame is 1005, never two bytes of garbage (E27) [close.empty_len]
		const TUniquePtr<FEvents> Empty = Visit(*Transport, Request(Scheme, TEXT("/ws/close/none?key=") + NewKey()));
		const FVaRestSocketEvent* EmptyEnd = Closed(*Empty);
		TestTrue(FString::Printf(TEXT("%s: an empty close is 1005, clean, no reason (%s)"), Scheme.Name, *Empty->Describe()),
			EmptyEnd && EmptyEnd->End.Code == 1005 && EmptyEnd->End.bWasClean && EmptyEnd->End.Reason.IsEmpty());
		const TUniquePtr<FEvents> One = Visit(*Transport, Request(Scheme, TEXT("/ws/close/onebyte?key=") + NewKey()));
		const FVaRestSocketEvent* OneEnd = Closed(*One);
		TestTrue(FString::Printf(TEXT("%s: a one-byte close is 1002 (%s)"), Scheme.Name, *One->Describe()), OneEnd && OneEnd->End.Code == 1002);
		const TUniquePtr<FEvents> Big = Visit(*Transport, Request(Scheme, TEXT("/ws/close/5000?key=") + NewKey()));
		const FVaRestSocketEvent* BigEnd = Closed(*Big);
		TestTrue(FString::Printf(TEXT("%s: 5000, which a peer must not send, is 1005, the detail naming it (%s)"), Scheme.Name, *Big->Describe()),
			BigEnd && BigEnd->End.Code == 1005 && BigEnd->End.Detail.Contains(TEXT("5000"), ESearchCase::CaseSensitive));
		// Plan decision 23: LWS 3.0 rewrites 1012 to 1002 before VaRest sees it [close.rewritten_codes]; pinned for W2 (VaRest-diy)
		const TUniquePtr<FEvents> Restart = Visit(*Transport, Request(Scheme, TEXT("/ws/close/1012?key=") + NewKey()));
		const FVaRestSocketEvent* RestartEnd = Closed(*Restart);
		TestTrue(FString::Printf(TEXT("%s: a server's 1012 arrives as 1002 on LWS 3.0 (%s)"), Scheme.Name, *Restart->Describe()), RestartEnd && RestartEnd->End.Code == 1002);
		// close.reason_utf8_bytes: "пока ✓", percent-encoded into the query
		const FString Bye = TEXT("пока ✓");
		const TUniquePtr<FEvents> Utf8 = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/close/4002?reason=%s&key=%s"), *VaRest::Http::PercentEncode(Bye), *NewKey())));
		const FVaRestSocketEvent* Utf8End = Closed(*Utf8);
		TestTrue(FString::Printf(TEXT("%s: a UTF-8 reason arrives whole (%s)"), Scheme.Name, *Utf8->Describe()), Utf8End && Utf8End->End.Code == 4002 && Utf8End->End.Reason.Equals(Bye, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsClosePeerBadUtf8Test, "VaRest.Integration.WebSocket.Transport.Close.PeerBadUtf8Reason", VAREST_WS_TEST_FLAGS)

bool FVaRestWsClosePeerBadUtf8Test::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Spec 2.5: ill-formed bytes become U+FFFD, with a warning (one per scheme)
	AddExpectedMessagePlain(TEXT("the server's close reason is not UTF-8"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, TEXT("/ws/badutf8?where=close&key=") + NewKey()));
		const FVaRestSocketEvent* End = Closed(*Events);
		TestTrue(FString::Printf(TEXT("%s: 1000 with the reason repaired (%s)"), Scheme.Name, *Events->Describe()),
			End && End->End.Code == 1000 && End->End.Reason.Equals(TEXT("�("), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseDropTest, "VaRest.Integration.WebSocket.Transport.Close.Drop", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseDropTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const TCHAR* How : { TEXT("0"), TEXT("1") })
		{
			// FIN and RST both give CLIENT_CLOSED with no close frame [close.drop_fin_sequence, close.drop_rst_sequence]
			const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/drop?rst=%s&key=%s"), How, *NewKey())));
			const FVaRestSocketEvent* End = Closed(*Events);
			TestTrue(FString::Printf(TEXT("%s rst=%s: 1006, by the transport, not clean (%s)"), Scheme.Name, How, *Events->Describe()),
				End && End->End.Code == 1006 && End->End.By == EVaRestSocketCloseBy::Transport && !End->End.bWasClean && End->End.Failure == EVaRestSocketFailure::None);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseOursTest, "VaRest.Integration.WebSocket.Transport.Close.Ours", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseOursTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Spec 3.5: a code that may not be sent becomes 1000, a long reason is cut to 123 bytes; one Warning each, per scheme
	AddExpectedMessagePlain(TEXT("close code 1006 cannot be sent"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("the close reason is cut to 123 bytes"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Sanitised = NewKey();
		FEvents Bad(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + Sanitised)));
		if (TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Bad.WaitType(EVaRestSocketEventType::Open)))
		{
			Bad.Connection->Close(1006, TEXT("x"));
			// Review conc M-6: refused at the call, not dropped later on the service thread
			TestFalse(FString::Printf(TEXT("%s: a send after Close is refused"), Scheme.Name), Bad.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("late")))));
		}
		Bad.WaitEnd();
		TestEqual(FString::Printf(TEXT("%s: the server got 1000"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, Sanitised)), 1000);

		const FString LongKey = NewKey();
		FEvents Long(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + LongKey)));
		if (TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Long.WaitType(EVaRestSocketEventType::Open)))
		{
			Long.Connection->Close(1000, FString::ChrN(200, TEXT('r')));
		}
		Long.WaitEnd();
		const TArray<FVaRestJson> Closes = JournalEvents(FetchJournal(*this, Site, LongKey), TEXT("close"));
		TestTrue(FString::Printf(TEXT("%s: the server got a 125-byte close payload (2 + 123)"), Scheme.Name), Closes.Num() >= 1 && Closes[0][TEXT("len")].AsInt() == 125);

		// Before the open: no close frame, the attempt is cancelled (plan decision 5)
		FEvents Pending(Transport->Connect(Request(Scheme, TEXT("/ws/hang?ms=20000&key=") + NewKey())));
		Pending.Connection->Close(1000, TEXT("never mind"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Pending.WaitEnd());
		const FVaRestSocketEvent* PendingEnd = Pending.GetEnd();
		TestTrue(FString::Printf(TEXT("%s: Error Canceled, by the client (%s)"), Scheme.Name, *Pending.Describe()), PendingEnd && PendingEnd->Type == EVaRestSocketEventType::Error
			&& PendingEnd->End.Failure == EVaRestSocketFailure::Canceled && PendingEnd->End.By == EVaRestSocketCloseBy::Client);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseCancelTest, "VaRest.Integration.WebSocket.Transport.Close.CancelSendsNothing", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseCancelTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Review conc I-1: an attempt the game cancels in the tick that started it never sends its upgrade request, headers and
	// all. Its kill lands at LWS's next timeout walk (up to about 1 s, decision 12) and LWS's own stage timers can overwrite
	// it meanwhile, while a loopback TCP connect or TLS handshake finishes in a few ms: the veto in APPEND_HANDSHAKE_HEADER
	// keeps the request off the wire
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Req.Headers = { { TEXT("Authorization"), TEXT("Bearer cancel-test") } };
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		Events.Connection->Close(1000, TEXT("never mind"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd());
		TestTrue(FString::Printf(TEXT("%s: Error Canceled (%s)"), Scheme.Name, *Events.Describe()), IsError(Events, EVaRestSocketFailure::Canceled));
		TestTrue(FString::Printf(TEXT("%s: LWS destroyed it within 2 s"), Scheme.Name), WaitIdle(*Transport, 2.0));
		TestEqual(FString::Printf(TEXT("%s: the server never saw the request"), Scheme.Name), FetchJournal(*this, Site, Key, 1, 1000)[TEXT("connections")].AsInt(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseShutdownFlushTest, "VaRest.Integration.WebSocket.Transport.Close.ShutdownWritesQueuedClose", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseShutdownFlushTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsCloseTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Review conc M-3: at engine exit the hub's 1001 closes (spec 1.4) are queued just before the transport shuts down.
	// Shutdown lets the service thread write them first (ExitFlushSeconds, 0.25 s). The close waits behind 8 MiB: about 128
	// service passes, one 64 KiB fragment each (re-review N-3: 2 MiB was only 1-3 times the about 1 ms a Shutdown that did
	// not wait takes to stop the thread; the server unmasks in one XOR, ws.py:193-199, so the real run stays far inside 0.25 s)
	for (const FScheme& Scheme : Schemes(Site))
	{
		// One transport per scheme, made after the previous one was shut down (VaRest-rti)
		const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Exiting = MakeTransport(*this, Site);
		if (!Exiting.IsValid())
		{
			return false;
		}
		ON_SCOPE_EXIT
		{
			Exiting->Shutdown();
		};
		const FString Key = NewKey();
		FEvents Events(Exiting->Connect(Request(Scheme, TEXT("/ws/sink?key=") + Key)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		TArray<uint8> Payload;
		Payload.SetNumZeroed(8 * 1024 * 1024);
		Events.Connection->Send(FVaRestSocketFrame::Binary(Payload));
		Events.Connection->Close(1001, TEXT("shutdown"));
		Exiting->Shutdown();
		Events.Pull();
		TestNotNull(FString::Printf(TEXT("%s: ended by Shutdown (%s)"), Scheme.Name, *Events.Describe()), Events.GetEnd());
		TestEqual(FString::Printf(TEXT("%s: nothing is left unsent"), Scheme.Name), Exiting->GetCounters().UnsentCloses, 0);
		FString Reason;
		TestEqual(FString::Printf(TEXT("%s: the server got the 1001 asked for just before Shutdown"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, Key), &Reason), 1001);
		TestEqualSensitive(FString::Printf(TEXT("%s: ... with its reason"), Scheme.Name), Reason, FString(TEXT("shutdown")));
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

Create `Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: close against a non-echoing server (CloseTimeout); Task 11 adds the connect timeout and the kill latency

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsTimeoutTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsTimeoutTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutCloseTest, "VaRest.Integration.WebSocket.Transport.Timeout.Close", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutCloseTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/noecho?key=") + Key);
		Req.CloseTimeout = 1.0;
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		const double ClosedAt = FPlatformTime::Seconds();
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd(10.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		const double Took = End->Time - ClosedAt;
		// The server never echoes [close.ours_noecho_wait_ms: LWS alone waits 5526 ms]; CloseTimeout cuts it (plan decisions 3, 17)
		TestTrue(FString::Printf(TEXT("%s: cut by CloseTimeout after %.2f s (1 s asked)"), Scheme.Name, Took), Took >= 0.9 && Took < 1.6);
		TestTrue(FString::Printf(TEXT("%s: Closed by the client, our code, not clean (%s)"), Scheme.Name, *Events.Describe()), End->Type == EVaRestSocketEventType::Closed
			&& End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && !End->End.bWasClean && End->End.Detail.Contains(TEXT("CloseTimeout"), ESearchCase::CaseSensitive));
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqual(FString::Printf(TEXT("%s: the server got our close"), Scheme.Name), JournalCloseCode(Journal), 4000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutCloseAckTest, "VaRest.Integration.WebSocket.Transport.Timeout.CloseLongerThanAck", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutCloseAckTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Review conc I-5: with CloseTimeout above LWS's fixed 5 s close-ack (T20), LWS ends the unanswered close itself, at about
	// 5.5 s [close.ours_noecho_wait_ms] -- and that end is the unanswered close CloseTimeout exists for: not clean
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/noecho?key=") + Key);
		Req.CloseTimeout = 8.0;
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		const double ClosedAt = FPlatformTime::Seconds();
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd(12.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		const double Took = End->Time - ClosedAt;
		TestTrue(FString::Printf(TEXT("%s: ended by LWS's close-ack after %.2f s, before the 8 s CloseTimeout"), Scheme.Name, Took), Took >= 4.9 && Took <= 7.5);
		TestTrue(FString::Printf(TEXT("%s: Closed 4000 by the client, not clean, the detail naming the close-ack (%s)"), Scheme.Name, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Closed && End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && !End->End.bWasClean
			&& End->End.Detail.Contains(TEXT("close-ack"), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("%s: the server got our close"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, Key)), 4000);
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Run them to see them fail**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Close` and `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Timeout`
Expected: `Close.PeerCodes`, `Close.PeerOdd` and `Close.PeerBadUtf8Reason` FAIL (every server close reads 1006: Task 7 does not read `PEER_INITIATED_CLOSE`); `Timeout.Close` FAILS (the end comes from LWS's own close-ack, about 5.5 s); `Timeout.CloseLongerThanAck` FAILS at "not clean" (Task 7 calls every end after our close frame clean); `Close.Drop`, `Close.Ours`, `Close.CancelSendsNothing` and `Close.ShutdownWritesQueuedClose` pass (Task 7 has their code).

- [ ] **Step 3: The members** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, add to the private functions, after `int32 OnWriteable(lws* InWsi);`:

```cpp
		int32 OnPeerClose(const void* In, size_t Len);
```

and at the end of the members, after `EVaRestSocketFailure CloseFailure = EVaRestSocketFailure::None;`:

```cpp
		/** FPlatformTime::Seconds() past which our close is cut (CloseTimeout from the Close call, plan decision 17) */
		double CloseDeadline = 0.0;
		// ---- the peer's close (spec 2.5)
		bool bPeerClosed = false;
		VaRest::WebSocket::FPeerClose Peer;
```

- [ ] **Step 4: `CloseTimeout` is armed at the call** — in `Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp`, in `FConn::Close`, after `CloseFailure = Failure;` add:

```cpp
	// Spec 2.5: CloseTimeout bounds a server that never echoes; above LWS's fixed 5 s close-ack it acts as about 5 s (plan decision 17)
	CloseDeadline = FPlatformTime::Seconds() + Request.CloseTimeout;
```

(`HAL/PlatformTime.h` is already among the file's includes since Task 7, for `CloseSentAt`.)

- [ ] **Step 5: The peer's close, the deadline** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp`:

At the top of `FConn::Prepare`, after `check(IsInGameThread());`, add:

```cpp
	// CloseTimeout (spec 2.5, 15): a value that is not a positive number is LWS's own close-ack wait (plan decision 17)
	if (!FMath::IsFinite(Request.CloseTimeout) || Request.CloseTimeout <= 0.0)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: CloseTimeout %g s is not a positive number; %g s is used"), *Label, Request.CloseTimeout, LwsCloseAckSeconds);
		Request.CloseTimeout = LwsCloseAckSeconds;
	}
```

In `FConn::OnCallback`, add a case before `case LWS_CALLBACK_CLIENT_CLOSED:`:

```cpp
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		return OnPeerClose(In, Len);
```

Add the function, after `FConn::OnConnectionError`:

```cpp
int32 VaRest::WebSocket::Lws::FConn::OnPeerClose(const void* In, size_t Len)
{
	// in: the close frame's payload (T12, libwebsockets.h:1217-1225); LWS 3.0 rewrote some codes to 1002 before this (T19) [close.rewritten_codes]
	Peer = ParsePeerClose(TConstArrayView<uint8>(static_cast<const uint8*>(In), In ? static_cast<int32>(Len) : 0));
	bPeerClosed = true;
	if (Peer.bReasonRepaired)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: the server's close reason is not UTF-8; ill-formed bytes are shown as U+FFFD"), *Label);
	}
	// 0: LWS echoes the close (RFC 6455 5.5.1) [close.echo_on_return_0]; the engine returns 1 (LwsWebSocket.cpp:389-390) [close.return_1_echo]
	return 0;
}
```

In `FConn::OnEnded`, replace everything from `if (bCloseQueued)` to the end of the function with:

```cpp
	if (bCloseQueued)
	{
		// Our close (spec 2.5): LWS 3.0 raises no callback for the peer's reply [close.ours_sequence], so an end before
		// CloseTimeout counts as clean (plan decision 4). The peer's own close, crossing ours before it went out, ends it cleanly
		// too. Review conc I-5: an end by LWS's own close-ack timer -- 5 s after our close frame (T20) -- with no close from the
		// peer is the unanswered close CloseTimeout exists for: not clean. Above about 5 s LWS's timer ends it before VaRest's
		// deadline; at 5 s both can fire in one lws_service return, LWS's first
		const double SinceCloseSent = FPlatformTime::Seconds() - CloseSentAt;
		const bool bAckTimedOut = bCloseSent && !bPeerClosed && SinceCloseSent >= LwsCloseAckSeconds - 0.25;
		const int32 Code = bCloseSent ? CloseCode : (bPeerClosed ? Peer.Code : 1006);
		FString Reason = bCloseSent ? Utf8ToString(CloseReason) : (bPeerClosed ? Peer.Reason : FString());
		// Re-review N-8: worded by what VaRest knows -- an echo arriving just before LWS's timer cannot be seen
		EndOpen(Code, MoveTemp(Reason), (bCloseSent || bPeerClosed) && !bAckTimedOut, CloseBy, CloseFailure,
			bAckTimedOut ? FString::Printf(TEXT("the connection ended %.1f s after our close frame, at or past libwebsockets' 5 s close-ack timer"), SinceCloseSent) : FString());
		return;
	}
	if (bPeerClosed)
	{
		const FString Detail = Peer.bCodeNormalised ? FString::Printf(TEXT("the server sent close code %d, which a peer must not send"), Peer.ReceivedCode)
			: Peer.bMalformed ? FString(TEXT("the server's close payload was one byte")) : FString();
		EndOpen(Peer.Code, Peer.Reason, true, EVaRestSocketCloseBy::Server, EVaRestSocketFailure::None, Detail);
		return;
	}
	// An end without a close frame (spec 2.5): FIN and RST both give CLIENT_CLOSED [close.drop_reports_client_closed]
	EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None);
}
```

Replace `FConn::CheckDeadlines` and `FConn::NextDeadline` with:

```cpp
void VaRest::WebSocket::Lws::FConn::CheckDeadlines(FService& Service, double Now)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate;
	if (bKillIssued && !bDestroyed && Wsi && Now - KillArmedAt >= KillRearmSeconds)
	{
		// Review conc I-1, re-review N-1 (Task 7): a kill that has not landed one timeout walk after it was armed is armed
		// again -- LWS's stage timers can overwrite it -- but never on every pass, which would postpone it under a busy
		// service thread (each lws_set_timeout restarts the timeout at the current second)
		lws_set_timeout(Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC);
		KillArmedAt = Now;
	}
	if (bDestroyed || State == EConnState::Ended)
	{
		return;
	}
	if (State == EConnState::Open && bCloseQueued && Now >= CloseDeadline)
	{
		// Spec 2.5: CloseTimeout cuts the close VaRest started; reported at once, then killed (plan decision 3)
		EndOpen(bCloseSent ? CloseCode : 1006, bCloseSent ? Utf8ToString(CloseReason) : FString(), false, CloseBy, CloseFailure,
			FString::Printf(TEXT("CloseTimeout (%g s) passed without the connection ending"), Request.CloseTimeout));
		Kill(Service);
		return;
	}
	if (State == EConnState::Open)
	{
		// LWS calls WRITEABLE only once a held write drained; this sweep releases it even when no WRITEABLE was asked for (plan decision 7)
		ReleaseHeld();
	}
}

double VaRest::WebSocket::Lws::FConn::NextDeadline() const
{
	double Next = TNumericLimits<double>::Max();
	if (State == EConnState::Open && bCloseQueued)
	{
		Next = CloseDeadline;
	}
	return Next;
}
```

- [ ] **Step 6: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Close` then `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Timeout`
Expected: `run_tests: 7 passed, 0 failed` (PeerCodes, PeerOdd, PeerBadUtf8Reason, Drop, Ours, CancelSendsNothing, ShutdownWritesQueuedClose); `run_tests: 2 passed, 0 failed` (Close, CloseLongerThanAck).

- [ ] **Step 7: Restore-the-bug** — (a) spec §16.5 item 5: in `ParsePeerClose` (`VaRestSocketTypes.cpp`), make the empty payload report `Out.Code = 0;` (what reading two bytes of nothing gives): `Close.PeerOdd` FAILS at "an empty close is 1005"; restore; PASSES. (b) `OnPeerClose` returns 1: `Close.PeerCodes` FAILS at "the client answered with a close" [close.return_1_echo]; restore; PASSES. (c) `FConn::Close` never arms the deadline (`CloseDeadline = TNumericLimits<double>::Max();`): `Timeout.Close` FAILS ("cut by CloseTimeout after 5.5 s"); restore; PASSES. (d) Review conc I-5: in `OnEnded` make `bAckTimedOut` always false: `Timeout.CloseLongerThanAck` FAILS at "not clean"; restore; PASSES. (e) Review conc I-1: in `FConn::OnAppendHeaders` (Task 7) delete the `if (State == EConnState::Ended)` block: `Close.CancelSendsNothing` FAILS at "the server never saw the request" on both legs (1: the loopback connect and the TLS handshake beat the kill's timeout walk; no certificate rule exists before Task 10); restore; PASSES. (f) Review conc M-3: in `FConn::Close` (`VaRestLwsConnSend.cpp`) delete `bCloseCounted = true;` and the `UnsentCloses.fetch_add(1)` line: `Close.ShutdownWritesQueuedClose` FAILS at "the server got the 1001" (-1: the thread stopped with about 128 fragments still to go out); restore; PASSES. (g) Review conc M-6: in `FHandle::Send` delete `|| Link->bCloseRequested.load()`: `Close.Ours` FAILS at "a send after Close is refused"; restore; PASSES.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConn.h Source/VaRestWebSocket/Private/VaRestLwsConn.cpp \
  Source/VaRestWebSocket/Private/VaRestLwsConnSend.cpp Source/VaRestTests/Private/WebSocket/VaRestWsCloseTest.cpp \
  Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp
git commit -m "ws: W1a task 9 -- the peer's close read and echoed (1005, 1002, normalised codes, repaired reasons), drops as 1006, our close bounded by CloseTimeout (restore-the-bug 5)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 10: Redirects are refused (VaRest-2rt, ruling Q2)

LWS 3.0 follows a 3xx to the upgrade by itself [hs.redirect_relative, hs.redirect_absolute] and re-runs `APPEND_HANDSHAKE_HEADER` for the new request [hs.redirect_reappends_headers]; VaRest refuses there, before the second request goes out (plan decision 10). A probe test answers the bead's open question: does LWS replay appended headers by itself?

**Files:**
- Create: `Source/VaRestWebSocket/Private/VaRestLwsConnTls.cpp`, `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRedirectReplayTest.cpp`
- Modify: `Source/VaRestWebSocket/Private/VaRestLwsConn.h` (one function), `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp` (`OnCallback`, `OnAppendHeaders`, `OnFilterPreEstablish`)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsRedirectTest.cpp`

**Interfaces:**
- Consumes: Task 7's `FConn` (`AppendCalls`, `PendingFailure`, `PendingDetail`, `Target.bTls`); the probe rig (`VaRest::LwsProbe::FRig`, `FConnectSpec::Hook`, `FetchJournal`, `JournalHeader`, `UrlValue`, `NewKey`, `Visit`, `NoteOutcome`, `CallSequence`, `KeepDefault`).
- Produces: `int32 FConn::OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk)` (private; service thread) — this task's version refuses a certificate check that belongs to a redirect (on a `ws://` attempt, or after the attempt's request was built); Task 13 adds VaRest's TLS checks to it.

A wss attempt that LWS redirects makes a second TCP and TLS connection before its second `APPEND_HANDSHAKE_HEADER`: the TLS handshake precedes the request it carries, so a certificate check that comes after the attempt's first `APPEND_HANDSHAKE_HEADER` is the redirect's [hs.redirect_to_tls: "1 certificate checks after a plain start", then no second request]. That is where a wss redirect is refused; the second `APPEND_HANDSHAKE_HEADER` guard is the one for ws.

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestTests/Private/WebSocket/VaRestWsRedirectTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// Ruling Q2 (VaRest-2rt): an upgrade redirect is never followed -- to the same port, an absolute URL, wss, or a downgrade

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestUrl.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsRedirectTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** /ws/status/302 to Location, with an Authorization header; checks the attempt failed Redirect and the target saw nothing */
	void ExpectRefused(FAutomationTestBase& Test, ILwsTestTransport& Transport, const FSite& Site, const FScheme& Scheme, const FString& Location, const FString& TargetKey, const TCHAR* What)
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *VaRest::Http::PercentEncode(Location), *Key));
		Req.Headers = { { TEXT("Authorization"), TEXT("Bearer redirect-test-token") } };
		const TUniquePtr<FEvents> Events = Visit(Transport, MoveTemp(Req));
		const FVaRestSocketEvent* End = Events->GetEnd();
		Test.TestTrue(FString::Printf(TEXT("%s %s: Error Redirect, status 0 (plan decision 10) (%s)"), Scheme.Name, What, *Events->Describe()),
			End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::Redirect && End->End.HttpStatus == 0);
		Test.TestEqual(FString::Printf(TEXT("%s %s: the 302 was asked once"), Scheme.Name, What), FetchJournal(Test, Site, Key)[TEXT("connections")].AsInt(), 1);
		// The target never got a request, so the Authorization header never reached it (ruling Q2)
		Test.TestEqual(FString::Printf(TEXT("%s %s: the target saw no request"), Scheme.Name, What), FetchJournal(Test, Site, TargetKey, 1, 1000)[TEXT("connections")].AsInt(), 0);
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsRedirectTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectSameServerTest, "VaRest.Integration.WebSocket.Transport.Redirect.SameServer", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectSameServerTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// hs.redirect_relative: LWS follows a relative Location on its own; the second APPEND_HANDSHAKE_HEADER is refused
		const FString Relative = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, TEXT("/ws/echo?key=") + Relative, Relative, TEXT("relative"));
		// hs.redirect_absolute: the same server by absolute URL. By IP: LWS resolves a Location's host itself, and localhost is ::1
		// first here (plan decision 19), so a name would fail the TCP connect before the redirect could be seen
		const FString Absolute = NewKey();
		ExpectRefused(*this, *Transport, Site, Scheme, FString::Printf(TEXT("%s://%s:%d/ws/echo?key=%s"), Scheme.bTls ? TEXT("wss") : TEXT("ws"), *Site.Host,
			Scheme.bTls ? Site.TlsPort : Site.Port, *Absolute), Absolute, TEXT("absolute"));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsRedirectSchemeTest, "VaRest.Integration.WebSocket.Transport.Redirect.OtherScheme", VAREST_WS_TEST_FLAGS)

bool FVaRestWsRedirectSchemeTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	const TArray<FScheme> Both = Schemes(Site);
	// hs.redirect_to_tls: ws -> wss to another port (by IP, as W0 did); refused at the certificate check of a ws:// attempt
	const FString ToTls = NewKey();
	ExpectRefused(*this, *Transport, Site, Both[0], FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *ToTls), ToTls, TEXT("to wss"));
	// hs.redirect_downgrade: wss -> ws, which LWS refuses itself ("HS: Redirect attempted SSL downgrade")
	const FString Down = NewKey();
	ExpectRefused(*this, *Transport, Site, Both[1], FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *Down), Down, TEXT("downgrade"));
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

Create the probe test `Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRedirectReplayTest.cpp` (outside the default `^VaRest` filter, like every `Probe.*`):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// VaRest-2rt (W0 Task 11 review I-1): the W0 rig appended Authorization on every APPEND_HANDSHAKE_HEADER, so
// hs.redirect_reappends_headers could not tell whether LWS 3.0 replays an appended header on a redirected request by
// itself. This case appends on the first call only and lets LWS follow the redirect: the target's journal answers.
// A record (AddInfo), not a fact row: the passport stays the W0 run's (Tools/lws_probe_passport.py lists its ids).

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeRedirectReplayTest, "Probe.VaRest.Lws.Handshake.RedirectReplay",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeRedirectReplayTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	// The engine's ::FEvent (HAL/Event.h:21) would make the name ambiguous under the directive above
	using VaRest::LwsProbe::FEvent;
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
	const FString Target = NewKey();
	const FString First = NewKey();
	FConnectSpec Spec;
	Spec.Path = FString::Printf(TEXT("/ws/status/302?location=%s&key=%s"), *UrlValue(FString::Printf(TEXT("/ws/echo?key=%s"), *Target)), *First);
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		if (Reason != LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER)
		{
			return KeepDefault;
		}
		int64& Calls = C.Counters.FindOrAdd(FName(TEXT("appends")));
		if (Calls++ > 0)
		{
			// The redirected request: nothing appended, nothing vetoed
			return 0;
		}
		static const char Line[] = "Authorization: Bearer replay-probe-token\r\n";
		const size_t Need = sizeof(Line) - 1;
		if (Need <= Len)
		{
			char** Cursor = static_cast<char**>(In);
			FMemory::Memcpy(*Cursor, Line, Need);
			*Cursor += Need;
		}
		return 0;
	};
	TArray<FEvent> Seen;
	Visit(*this, Rig, MoveTemp(Spec), Seen);
	const FVaRestJson Journal = FetchJournal(*this, Site, Target);
	// Harness: the redirect was followed (hs.redirect_relative), so the question can be answered at all
	if (!TestEqual(TEXT("LWS followed the redirect to the target"), Journal[TEXT("connections")].AsInt(), 1))
	{
		return false;
	}
	const FString Authorization = JournalHeader(Journal, TEXT("Authorization"));
	NoteOutcome(*this, FString::Printf(TEXT("LWS 3.0 replays an appended header on a redirected request by itself (the target received Authorization \"%s\"; the client's callbacks: %s)"),
		*Authorization, *CallSequence(Seen)), !Authorization.Equals(TEXT("(absent)"), ESearchCase::CaseSensitive));
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Run them to see them fail**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Redirect`
Expected: `Redirect.SameServer` FAILS (on ws LWS follows and the attempt opens at the target; on wss the redirected TLS handshake to 127.0.0.1 fails the host-name check, ConnectFailed); `Redirect.OtherScheme`'s ws-to-wss leg FAILS (ConnectFailed, for the same reason).

- [ ] **Step 3: The function** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, after `int32 OnPeerClose(const void* In, size_t Len);` add:

```cpp
		/** PERFORM_SERVER_CERT_VERIFICATION: 0 accepts, 1 refuses (libwebsockets.h:1015-1044) */
		int32 OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk);
```

- [ ] **Step 4: The guards** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp`:

In `FConn::OnCallback`, add before `case LWS_CALLBACK_CLIENT_CLOSED:`:

```cpp
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
		// user is the X509_STORE_CTX, len preverify_ok (libwebsockets.h:1015-1044); the service found this connection by
		// lws_wsi_user, as the engine does (LwsWebSocketsManager.cpp:447-456)
		return OnVerifyServerCertificate(Service, static_cast<X509_STORE_CTX*>(User), static_cast<int32>(Len));
```

In `FConn::OnAppendHeaders`, right after `++AppendCalls;` add:

```cpp
	if (AppendCalls > 1)
	{
		// A second APPEND_HANDSHAKE_HEADER in one attempt: LWS followed a 3xx and builds the request to the new target
		// [hs.redirect_relative, hs.redirect_absolute, hs.redirect_target_visible]. Ruling Q2: nothing is appended -- no auth and
		// no header reaches another target -- and the attempt fails here, before the second request goes out; LWS then gives no
		// CONNECTION_ERROR [hs.redirect_veto], so OnEnded reports it (plan decision 10)
		PendingFailure = EVaRestSocketFailure::Redirect;
		PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow (ruling Q2)");
		return -1;
	}
```

In `FConn::OnFilterPreEstablish`, right after its `if (State == EConnState::Ended) { return -1; }` block (Task 7), add:

```cpp
	if (AppendCalls > 1)
	{
		// The response to a second request: a second guard behind the APPEND veto ("HS: Rejected by filter cb", hs.redirect_veto)
		PendingFailure = EVaRestSocketFailure::Redirect;
		PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow (ruling Q2)");
		return -1;
	}
```

- [ ] **Step 5: The certificate check of a `ws://` attempt** — create `Source/VaRestWebSocket/Private/VaRestLwsConnTls.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

int32 VaRest::WebSocket::Lws::FConn::OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an attempt VaRest already ended finishes no TLS handshake (Task 7's veto in APPEND is the other half)
		return 1;
	}
	if (!Target.bTls || AppendCalls > 0)
	{
		// A redirect's TLS handshake: a ws:// attempt has none of its own, and a wss:// attempt's own comes before its request
		// was built (APPEND_HANDSHAKE_HEADER) -- so LWS is connecting to a redirect's target [hs.redirect_to_tls]. Refused
		// before that handshake ends, so no request reaches the target (ruling Q2, plan decision 10). It also refuses a TLS 1.2
		// renegotiation after the request, which OpenSSL 1.1.1 clients accept by default (review conc M-14): rare, noted in
		// decision 10
		PendingFailure = EVaRestSocketFailure::Redirect;
		PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow (ruling Q2)");
		return 1;
	}
	// OpenSSL's verdict stands (libwebsockets.h:1015-1044); Task 13 adds VaRest's checks
	return 0;
}
```

- [ ] **Step 6: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Redirect`
Expected: `run_tests: 2 passed, 0 failed`. Then the earlier suites, which a certificate rule could break: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Connect` and `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Close` — `0 failed` both (a wss attempt's own certificate check comes before its first `APPEND_HANDSHAKE_HEADER`; a cancelled one's is refused).

Run the probe: `Tools/run_tests.sh Probe.VaRest.Lws.Handshake.RedirectReplay`
Expected: `run_tests: 1 passed, 0 failed`. Copy its `outcome:` line (`grep -n "replays an appended header" <Host>/Saved/Logs/VaRestTests.log`) into the task report: Task 14 writes it to VaRest-2rt.

- [ ] **Step 7: Restore-the-bug** — (a) delete the `if (AppendCalls > 1)` block of `OnAppendHeaders`: `Redirect.SameServer` FAILS on ws ("the target saw no request": the second request went out, with the Authorization header); restore; PASSES. (b) In `OnVerifyServerCertificate`, delete the redirect branch (return 0 always): `Redirect.SameServer` FAILS on wss and `Redirect.OtherScheme` at the ws-to-wss leg (ConnectFailed, not Redirect); restore; PASSES. (After Task 13 lets an IP-literal certificate through, (b) alone would be caught by (a)'s guard instead: run it now.) (c) Review conc I-1: delete both the `if (State == EConnState::Ended)` block of `OnVerifyServerCertificate` and Task 7's in `OnAppendHeaders`: `Close.CancelSendsNothing` FAILS on both legs; put back only the certificate one: the wss leg PASSES (its handshake is refused first), the ws leg still FAILS; restore both; PASSES.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConn.h Source/VaRestWebSocket/Private/VaRestLwsConn.cpp \
  Source/VaRestWebSocket/Private/VaRestLwsConnTls.cpp Source/VaRestTests/Private/WebSocket/VaRestWsRedirectTest.cpp \
  Source/VaRestTests/Private/LwsProbe/VaRestLwsProbeRedirectReplayTest.cpp
git commit -m "ws: W1a task 10 -- upgrade redirects refused: at the second APPEND_HANDSHAKE_HEADER, at a redirect's certificate check, in FILTER_PRE_ESTABLISH (ruling Q2, VaRest-2rt); a probe for LWS's own header replay

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 11: The connect deadline, its clamp, and the kill latency (VaRest-wcv)

`ConnectTimeout` is measured on the service thread from the connect, reported at the deadline and enforced with a kill (spec §2.2, D32, plan decisions 3, 12, 17); the context's `timeout_secs` is the 60 s backstop (Task 6). The kill's latency at the chosen waits is measured here.

**Files:**
- Modify: `Source/VaRestWebSocket/Private/VaRestLwsConn.h` (one member), `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp` (`Prepare`, `Start`, `CheckDeadlines`, `NextDeadline`)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp` (append)

**Interfaces:**
- Consumes: `MaxConnectTimeoutSeconds` (Task 3), `FServiceCounters` kill fields (Task 6), `FConn::Kill` (Task 7), `ILwsTestTransport::GetCounters` (Task 7).
- Produces: `FConn::ConnectDeadline` (private); `NextDeadline` returns it while connecting.

- [ ] **Step 1: Write the failing tests** — in `Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp`, insert before `#undef VAREST_WS_BEGIN`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutConnectTest, "VaRest.Integration.WebSocket.Transport.Timeout.Connect", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutConnectTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/hang?ms=20000&key=") + NewKey());
		Req.ConnectTimeout = 1.0;
		const double Started = FPlatformTime::Seconds();
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		TestTrue(FString::Printf(TEXT("%s: ended within 5 s"), Scheme.Name), Events.WaitEnd(5.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd(30.0);
			continue;
		}
		const double Took = End->Time - Started;
		// Measured on the service thread and reported at the deadline (spec 2.2, D32; plan decision 3), not at LWS's kill
		TestTrue(FString::Printf(TEXT("%s: reported at the deadline, %.3f s after the connect (1 s asked)"), Scheme.Name, Took), Took >= 0.95 && Took < 1.5);
		TestTrue(FString::Printf(TEXT("%s: Error ConnectTimeout, by Timeout, VaRest's own (%s)"), Scheme.Name, *Events.Describe()), End->Type == EVaRestSocketEventType::Error
			&& End->End.Failure == EVaRestSocketFailure::ConnectTimeout && End->End.By == EVaRestSocketCloseBy::Timeout && End->End.Detail.Contains(TEXT("VaRest's ConnectTimeout"), ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("the killed attempts are destroyed"), VaRest::Http::Testing::PumpUntil([&Transport] { return Transport->GetCounters().LiveConnections == 0; }, 5.0, true));
	const VaRest::WebSocket::Testing::FLwsCounters Counters = Transport->GetCounters();
	AddInfo(FString::Printf(TEXT("kill to WSI_DESTROY: last %.1f ms, longest %.1f ms (VaRest-wcv, 100 ms wait cap while a kill is in flight)"),
		Counters.LastKillToDestroySeconds * 1000.0, Counters.MaxKillToDestroySeconds * 1000.0));
	TestTrue(TEXT("both kills landed"), Counters.KillsLanded >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutClampTest, "VaRest.Integration.WebSocket.Transport.Timeout.Clamp", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutClampTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Plan decision 17: over the context's 60 s backstop, or not a positive number: 60 s, with a Warning (two values, two schemes)
	AddExpectedMessagePlain(TEXT("ConnectTimeout 120 s is outside (0, 60]"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("ConnectTimeout 0 s is outside (0, 60]"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const double Asked : { 120.0, 0.0 })
		{
			FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + NewKey());
			Req.ConnectTimeout = Asked;
			const TUniquePtr<FEvents> Events = Visit(*Transport, MoveTemp(Req));
			TestNotNull(FString::Printf(TEXT("%s %g s: still opens"), Scheme.Name, Asked), Events->Find(EVaRestSocketEventType::Open));
		}
		// A slow upgrade inside the deadline opens (/ws/slowupgrade waits before its 101)
		FVaRestSocketConnectRequest Slow = Request(Scheme, TEXT("/ws/slowupgrade?ms=300&key=") + NewKey());
		Slow.ConnectTimeout = 2.0;
		const TUniquePtr<FEvents> SlowEvents = Visit(*Transport, MoveTemp(Slow));
		TestNotNull(FString::Printf(TEXT("%s: a 300 ms upgrade opens within 2 s (%s)"), Scheme.Name, *SlowEvents->Describe()), SlowEvents->Find(EVaRestSocketEventType::Open));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutKillLatencyTest, "VaRest.Integration.WebSocket.Transport.Timeout.KillLatency", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutKillLatencyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// VaRest-wcv: LWS walks its timeouts only when the wall-clock second changed, on a return of lws_service (T26). Measured
	// with the short cap VaRest uses while a kill is in flight (plan decision 12), and, for the record, with the 1 s cap alone
	double Longest[2] = { -1.0, -1.0 };
	const int32 Caps[2] = { 100, 1000 };
	for (int32 Run = 0; Run < 2; ++Run)
	{
		VaRest::WebSocket::Testing::FLwsTransportOptions Options;
		Options.TcpAddressOverride = Site.Host;
		Options.ShortWaitCapMs = Caps[Run];
		const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Measured = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT
		{
			Measured->Shutdown();
		};
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FVaRestSocketConnectRequest Req;
			Req.Url = FString::Printf(TEXT("ws://%s:%d/ws/hang?ms=20000&key=%s"), *Site.Host, Site.Port, *NewKey());
			Req.ConnectTimeout = 0.5;
			FEvents Events(Measured->Connect(MoveTemp(Req)));
			Events.WaitEnd(5.0);
			VaRest::Http::Testing::PumpUntil([&Measured] { return Measured->GetCounters().LiveConnections == 0; }, 5.0, true);
		}
		Longest[Run] = Measured->GetCounters().MaxKillToDestroySeconds;
		AddInfo(FString::Printf(TEXT("wait cap %d ms while a kill is in flight: longest kill to WSI_DESTROY over 4 kills %.1f ms"), Caps[Run], Longest[Run] * 1000.0));
	}
	TestTrue(FString::Printf(TEXT("with VaRest's 100 ms cap a kill lands within 1.6 s (%.1f ms)"), Longest[0] * 1000.0), Longest[0] >= 0.0 && Longest[0] < 1.6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutKillLatencyBusyTest, "VaRest.Integration.WebSocket.Transport.Timeout.KillLatencyBusy", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutKillLatencyBusyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// Re-review N-1: kills beside a busy neighbour on the same service. The neighbour's send to /ws/sink keeps the service
	// thread in WRITEABLE callbacks (each masks and writes 64 KiB) while four attempts time out one after another. A kill
	// re-armed on every pass is stamped with the new second whenever a second boundary falls inside a callback, so the walk of
	// that second keeps the wsi and the kill moves on: past decision 12's 1.6 s. The game thread tops the send up while it
	// waits, so the neighbour stays busy for every kill (32 MiB alone would drain in tens of milliseconds)
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Busy = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
	ON_SCOPE_EXIT
	{
		Busy->Shutdown();
	};
	FVaRestSocketConnectRequest SinkRequest;
	SinkRequest.Url = FString::Printf(TEXT("ws://%s:%d/ws/sink?key=%s"), *Site.Host, Site.Port, *NewKey());
	FEvents Sink(Busy->Connect(MoveTemp(SinkRequest)));
	if (!TestTrue(TEXT("the busy neighbour opened"), Sink.WaitType(EVaRestSocketEventType::Open)))
	{
		Sink.WaitEnd();
		return false;
	}
	TArray<uint8> Megabyte;
	Megabyte.SetNumZeroed(1024 * 1024);
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Neighbour = Sink.Connection;
	int64 SentBytes = 0;
	// Game thread, from the waits below: keeps at least 4 MiB queued, in 1 MiB frames
	const auto TopUp = [&Neighbour, &Megabyte, &SentBytes]
	{
		while (Neighbour->GetBufferedAmount() < 4 * 1024 * 1024 && Neighbour->Send(FVaRestSocketFrame::Binary(Megabyte)))
		{
			SentBytes += Megabyte.Num();
		}
	};
	TopUp();
	int32 TimedOut = 0;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FVaRestSocketConnectRequest Req;
		Req.Url = FString::Printf(TEXT("ws://%s:%d/ws/hang?ms=20000&key=%s"), *Site.Host, Site.Port, *NewKey());
		Req.ConnectTimeout = 0.5;
		FEvents Events(Busy->Connect(MoveTemp(Req)));
		Events.WaitFor([&TopUp](const TArray<FVaRestSocketEvent>& All)
		{
			TopUp();
			return All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
		}, 5.0);
		TimedOut += IsError(Events, EVaRestSocketFailure::ConnectTimeout) ? 1 : 0;
		// Until LWS destroyed the killed one; the neighbour stays
		VaRest::Http::Testing::PumpUntil([&TopUp, &Busy] { TopUp(); return Busy->GetCounters().LiveConnections == 1; }, 5.0, true);
	}
	const VaRest::WebSocket::Testing::FLwsCounters Counters = Busy->GetCounters();
	AddInfo(FString::Printf(TEXT("beside a busy send (%lld MiB queued in all): longest kill to WSI_DESTROY %.1f ms over %d kills"),
		SentBytes / (1024 * 1024), Counters.MaxKillToDestroySeconds * 1000.0, Counters.KillsLanded));
	TestEqual(TEXT("four attempts timed out"), TimedOut, 4);
	TestEqual(TEXT("every kill landed"), Counters.KillsLanded, 4);
	TestTrue(FString::Printf(TEXT("beside a busy neighbour a kill still lands within 1.6 s (%.1f ms)"), Counters.MaxKillToDestroySeconds * 1000.0),
		Counters.MaxKillToDestroySeconds >= 0.0 && Counters.MaxKillToDestroySeconds < 1.6);
	TestTrue(TEXT("the neighbour was still sending at the end"), Neighbour->GetBufferedAmount() > 0 && !Neighbour->HasEnded());
	Neighbour->Close(1000, TEXT("done"));
	Sink.WaitEnd(30.0);
	return true;
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Timeout`
Expected: `Timeout.Connect` FAILS ("ended within 5 s": only the context's 60 s backstop applies); `Timeout.Clamp` FAILS (the two Warnings never come); `Timeout.KillLatency` and `Timeout.KillLatencyBusy` FAIL (no kill is issued: the attempts end after 60 s, past the test's waits); `Timeout.Close` and `Timeout.CloseLongerThanAck` (Task 9) pass (re-review N-9).

- [ ] **Step 3: The member** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, after `double StartedAt = 0.0;` add:

```cpp
		/** FPlatformTime::Seconds() past which the attempt fails ConnectTimeout (spec 2.2, D32) */
		double ConnectDeadline = 0.0;
```

- [ ] **Step 4: The deadline** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp`:

In `FConn::Prepare`, after the `CloseTimeout` check, add:

```cpp
	// ConnectTimeout (spec 2.2): the context's timeout_secs backs up every socket, so a longer value is clamped (plan decision 17)
	if (!FMath::IsFinite(Request.ConnectTimeout) || Request.ConnectTimeout <= 0.0 || Request.ConnectTimeout > MaxConnectTimeoutSeconds)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: ConnectTimeout %g s is outside (0, %g]; %g s is used"), *Label, Request.ConnectTimeout, MaxConnectTimeoutSeconds, MaxConnectTimeoutSeconds);
		Request.ConnectTimeout = MaxConnectTimeoutSeconds;
	}
```

In `FConn::Start`, replace `State = EConnState::Connecting;` with:

```cpp
	State = EConnState::Connecting;
	ConnectDeadline = StartedAt + Request.ConnectTimeout;
```

In `FConn::CheckDeadlines`, after the `if (bDestroyed || State == EConnState::Ended) { return; }` block, add:

```cpp
	if (State == EConnState::Connecting && Now >= ConnectDeadline)
	{
		// Spec 2.2, D32: measured here, so a hitching game thread cannot delay it -- exact unless the service thread is itself
		// blocked in LWS's synchronous DNS (review conc I-4; W1b B9, VaRest-gi0); reported at the deadline and killed (plan
		// decision 3). LWS's own cuts (its 20 s connect stage, timeout_secs) also end in CONNECTION_ERROR "", so the detail
		// says which one fired (review conc M-1, M-2)
		EndPreOpen(EVaRestSocketFailure::ConnectTimeout, EVaRestSocketCloseBy::Timeout, 0, FString::Printf(TEXT("VaRest's ConnectTimeout (%g s) passed"), Request.ConnectTimeout));
		Kill(Service);
		return;
	}
```

In `FConn::NextDeadline`, before `return Next;` add:

```cpp
	if (State == EConnState::Connecting)
	{
		Next = FMath::Min(Next, ConnectDeadline);
	}
```

- [ ] **Step 5: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Timeout`
Expected: `run_tests: 6 passed, 0 failed` (Close, CloseLongerThanAck, Connect, Clamp, KillLatency, KillLatencyBusy). Put both `KillLatency` AddInfo lines and `Timeout.Connect`'s in the task report: Task 14 records them on VaRest-wcv.

- [ ] **Step 6: Restore-the-bug** — (a) in `Start`, `ConnectDeadline = TNumericLimits<double>::Max();`: `Timeout.Connect` FAILS ("ended within 5 s"); restore; PASSES. (b) In `CheckDeadlines`, delete the `EndPreOpen(...)` line of the connect branch (keep `Kill`): the attempt then reports when LWS's kill lands, from its `""` string [timeout.kill_pending_sequence] — `Timeout.Connect` FAILS at "VaRest's own" (and, a kill landing up to about 1 s later, at "reported at the deadline"); restore; PASSES. (c) Re-review N-1: in `FConn::CheckDeadlines` drop `&& Now - KillArmedAt >= KillRearmSeconds` from the re-arm (re-arm on every pass): `Timeout.KillLatencyBusy` FAILS at "beside a busy neighbour a kill still lands within 1.6 s" -- the restore is statistical (a postponement needs a second boundary inside a callback), hence 4 kills; if a run of it passes, run it twice more and report the longest kill each time; restore; PASSES.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConn.h Source/VaRestWebSocket/Private/VaRestLwsConn.cpp \
  Source/VaRestTests/Private/WebSocket/VaRestWsTimeoutTest.cpp
git commit -m "ws: W1a task 11 -- ConnectTimeout on the service thread, reported at the deadline and killed, clamped to the 60 s context backstop; kill latency measured at the chosen waits (VaRest-wcv)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 12: Receive — byte assembly, UTF-8 once, the size limit

Chunks are appended as bytes; a message is complete when the frame is final and nothing of it remains (T8); text is validated once, on the whole message, with VaRestJson's validator; over `MaxMessageBytes` the connection is closed with 1009 (spec §2.3). Spec §16.5 items 6 and 9 land here.

**Files:**
- Create: `Source/VaRestWebSocket/Private/VaRestLwsConnReceive.cpp`
- Modify: `Source/VaRestWebSocket/Private/VaRestLwsConn.h` (members, two functions), `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp` (`Prepare`, `OnCallback`)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsReceiveTest.cpp`

**Interfaces:**
- Consumes: `VaRest::Json::IsValidUtf8` (`VaRestJsonUtf8.h`), `FConn::Close`, `DropOutbox` (Task 7), `CloseReasonBytes` (Task 2).
- Produces: `int32 FConn::OnReceive(FService&, lws*, const void* In, size_t Len)`, `void FConn::Fail(FService&, int32 Code, const TCHAR* Reason, EVaRestSocketFailure)` (private; service thread). W1b's flow control counts what `OnReceive` queues (`FSocketLink::PendingInboundBytes`, Task 3).

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestTests/Private/WebSocket/VaRestWsReceiveTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: fragmented and large text (Cyrillic split inside a code point and at 4096 bytes), binary, interleaved
// order, invalid UTF-8 -> 1007, size limit -> 1009, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestJsonUtf8.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsReceiveTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** The server's "message_sent" event: what /ws/fragment and /ws/large sent (kind, len, sha256) */
	FVaRestJson MessageSent(const FVaRestJson& Journal)
	{
		const TArray<FVaRestJson> Sent = JournalEvents(Journal, TEXT("message_sent"));
		return Sent.Num() > 0 ? Sent[0] : FVaRestJson();
	}

	/** Opens Path, waits for Count messages or the end, closes, waits for the end */
	TUniquePtr<FEvents> Receive(ILwsTestTransport& Transport, const FScheme& Scheme, const FString& Path, int32 Count, FVaRestSocketConnectRequest Req = FVaRestSocketConnectRequest())
	{
		Req.Url = Scheme.Base + Path;
		TUniquePtr<FEvents> Events = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
		Events->WaitMessages(Count, 20.0);
		if (!Events->GetEnd())
		{
			Events->Connection->Close(1000, TEXT("done"));
		}
		Events->WaitEnd();
		return Events;
	}

	/** One message that is the server's message_sent, byte for byte: kind, length, SHA-256 */
	bool SameAsSent(FAutomationTestBase& Test, const TCHAR* What, const FEvents& Events, const FVaRestJson& Journal, EVaRestSocketFrameKind Kind)
	{
		const TArray<const FVaRestSocketEvent*> Messages = Events.GetMessages();
		const FVaRestJson Sent = MessageSent(Journal);
		if (!Test.TestEqual(FString::Printf(TEXT("%s: one message (%s)"), What, *Events.Describe()), Messages.Num(), 1) || !Test.TestTrue(FString::Printf(TEXT("%s: the server said what it sent"), What), Sent.IsValid()))
		{
			return false;
		}
		const FVaRestSocketEvent& Message = *Messages[0];
		bool bSame = Test.TestTrue(FString::Printf(TEXT("%s: its kind"), What), Message.Kind == Kind);
		bSame &= Test.TestEqual(FString::Printf(TEXT("%s: its length"), What), Message.Bytes.Num(), Sent[TEXT("len")].AsInt());
		bSame &= Test.TestEqualSensitive(FString::Printf(TEXT("%s: byte for byte"), What), Sha256Hex(Message.Bytes), Sent[TEXT("sha256")].AsString());
		bSame &= Test.TestTrue(FString::Printf(TEXT("%s: a slack byte for ParseOwned (core 2.1)"), What), Message.Bytes.Max() > Message.Bytes.Num());
		return bSame;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsReceiveTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveSplitTest, "VaRest.Integration.WebSocket.Transport.Receive.SplitCodePoint", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveSplitTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Spec 16.5 item 6, E9: a frame boundary inside a two-byte letter [rx.frames_split_utf8]
		const FString Inside = NewKey();
		const TUniquePtr<FEvents> Split = Receive(*Transport, Scheme, TEXT("/ws/fragment?cyrillic=5120&split_inside=4096&key=") + Inside, 1);
		if (SameAsSent(*this, *FString::Printf(TEXT("%s, cut inside a code point"), Scheme.Name), *Split, FetchJournal(*this, Site, Inside), EVaRestSocketFrameKind::Text))
		{
			TestFalse(FString::Printf(TEXT("%s: no U+FFFD anywhere"), Scheme.Name), Utf8(Split->GetMessages()[0]->Bytes).Contains(TEXT("�"), ESearchCase::CaseSensitive));
		}
		// ... and exactly at 4096 bytes
		const FString At = NewKey();
		const TUniquePtr<FEvents> Exact = Receive(*Transport, Scheme, TEXT("/ws/fragment?cyrillic=8192&at=4096&key=") + At, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, cut at 4096"), Scheme.Name), *Exact, FetchJournal(*this, Site, At), EVaRestSocketFrameKind::Text);
		// A PING between the fragments is answered and the message stays whole (RFC 6455 5.4) [rx.control_inside_fragmented]
		const FString Ping = NewKey();
		const TUniquePtr<FEvents> Between = Receive(*Transport, Scheme, TEXT("/ws/fragment?cyrillic=5120&split_inside=2000&ping_between=1&key=") + Ping, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, a PING between the fragments"), Scheme.Name), *Between, FetchJournal(*this, Site, Ping), EVaRestSocketFrameKind::Text);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveLargeTest, "VaRest.Integration.WebSocket.Transport.Receive.Large", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveLargeTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// One 200000-byte frame of Cyrillic, handed over in 64 KiB chunks [rx.chunk_sizes_single_frame]: code points cut by chunks
		const FString Single = NewKey();
		const TUniquePtr<FEvents> Large = Receive(*Transport, Scheme, TEXT("/ws/large?bytes=200000&ascii=0&key=") + Single, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, one large frame"), Scheme.Name), *Large, FetchJournal(*this, Site, Single), EVaRestSocketFrameKind::Text);
		// Binary in three frames: the kind from the first fragment [rx.binary_flag_on_continuations]
		const FString Bytes = NewKey();
		const TUniquePtr<FEvents> Binary = Receive(*Transport, Scheme, TEXT("/ws/fragment?bytes=10000&binary=1&at=3000,6000&key=") + Bytes, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, binary in three frames"), Scheme.Name), *Binary, FetchJournal(*this, Site, Bytes), EVaRestSocketFrameKind::Binary);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveOrderTest, "VaRest.Integration.WebSocket.Transport.Receive.Order", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveOrderTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Spec 7.1, E12: one ordered queue for both kinds [rx.order_preserved]
		const TUniquePtr<FEvents> Events = Receive(*Transport, Scheme, TEXT("/ws/order?n=400&key=") + NewKey(), 400);
		const TArray<const FVaRestSocketEvent*> Messages = Events->GetMessages();
		if (!TestEqual(FString::Printf(TEXT("%s: 400 messages"), Scheme.Name), Messages.Num(), 400))
		{
			continue;
		}
		int32 Wrong = 0;
		for (int32 Index = 0; Index < 400; ++Index)
		{
			const FVaRestSocketEvent& Each = *Messages[Index];
			if (Index % 2 == 0)
			{
				Wrong += Each.Kind == EVaRestSocketFrameKind::Text && Utf8(Each.Bytes).Equals(FString::Printf(TEXT("t%d"), Index), ESearchCase::CaseSensitive) ? 0 : 1;
			}
			else
			{
				const bool bRight = Each.Kind == EVaRestSocketFrameKind::Binary && Each.Bytes.Num() == 4
					&& ((Each.Bytes[0] << 24) | (Each.Bytes[1] << 16) | (Each.Bytes[2] << 8) | Each.Bytes[3]) == Index;
				Wrong += bRight ? 0 : 1;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: every message in place, text and binary alternating"), Scheme.Name), Wrong, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveInvalidTest, "VaRest.Integration.WebSocket.Transport.Receive.Invalid", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveInvalidTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// RFC 6455 8.1, spec 2.3: invalid UTF-8 in a text message closes with 1007, the message never delivered [rx.invalid_utf8_passthrough]
		const FString BadKey = NewKey();
		const TUniquePtr<FEvents> Bad = Receive(*Transport, Scheme, TEXT("/ws/badutf8?key=") + BadKey, 1);
		const FVaRestSocketEvent* BadEnd = Bad->GetEnd();
		TestEqual(FString::Printf(TEXT("%s: the invalid text is not delivered"), Scheme.Name), Bad->Count(EVaRestSocketEventType::Message), 0);
		TestTrue(FString::Printf(TEXT("%s: Closed 1007 by the transport, InvalidUtf8 (%s)"), Scheme.Name, *Bad->Describe()), BadEnd && BadEnd->Type == EVaRestSocketEventType::Closed
			&& BadEnd->End.Code == 1007 && BadEnd->End.By == EVaRestSocketCloseBy::Transport && BadEnd->End.Failure == EVaRestSocketFailure::InvalidUtf8);
		TestEqual(FString::Printf(TEXT("%s: the server got our 1007"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, BadKey)), 1007);
		// Spec 16.5 item 9: over MaxMessageBytes, closed with 1009 as the chunks arrive, never delivered
		const FString BigKey = NewKey();
		FVaRestSocketConnectRequest Limited;
		Limited.MaxMessageBytes = 65536;
		const TUniquePtr<FEvents> Big = Receive(*Transport, Scheme, TEXT("/ws/large?bytes=200000&key=") + BigKey, 1, MoveTemp(Limited));
		const FVaRestSocketEvent* BigEnd = Big->GetEnd();
		TestEqual(FString::Printf(TEXT("%s: the oversize message is not delivered"), Scheme.Name), Big->Count(EVaRestSocketEventType::Message), 0);
		TestTrue(FString::Printf(TEXT("%s: Closed 1009 by the transport, MessageTooBig (%s)"), Scheme.Name, *Big->Describe()), BigEnd && BigEnd->Type == EVaRestSocketEventType::Closed
			&& BigEnd->End.Code == 1009 && BigEnd->End.By == EVaRestSocketCloseBy::Transport && BigEnd->End.Failure == EVaRestSocketFailure::MessageTooBig);
		TestEqual(FString::Printf(TEXT("%s: the server got our 1009"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, BigKey)), 1009);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveEchoTest, "VaRest.Integration.WebSocket.Transport.Receive.Echo", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveEchoTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		FEvents Events(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + NewKey())));
		Events.bAcknowledge = false;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		const uint8 Raw[] = { 1, 2, 3 };
		const FString Long = FString::ChrN(300000, TEXT('z'));
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("hello"))));
		Events.Connection->Send(FVaRestSocketFrame::Binary(Raw));
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(Long)));
		const bool bAll = Events.WaitMessages(3);
		const TArray<const FVaRestSocketEvent*> Messages = Events.GetMessages();
		if (TestTrue(FString::Printf(TEXT("%s: three echoes (%s)"), Scheme.Name, *Events.Describe()), bAll && Messages.Num() == 3))
		{
			TestTrue(FString::Printf(TEXT("%s: the text"), Scheme.Name), Messages[0]->Kind == EVaRestSocketFrameKind::Text && Utf8(Messages[0]->Bytes).Equals(TEXT("hello"), ESearchCase::CaseSensitive));
			TestTrue(FString::Printf(TEXT("%s: the binary"), Scheme.Name), Messages[1]->Kind == EVaRestSocketFrameKind::Binary && Messages[1]->Bytes == TArray<uint8>(Raw, 3));
			TestTrue(FString::Printf(TEXT("%s: the 300000 bytes, sent in fragments, back whole"), Scheme.Name), Messages[2]->Bytes.Num() == 300000 && Utf8(Messages[2]->Bytes).Equals(Long, ESearchCase::CaseSensitive));
			// Spec 2.3: what is queued and not yet delivered counts; Consumed gives it back
			TestEqual(FString::Printf(TEXT("%s: pending inbound bytes"), Scheme.Name), Events.Connection->GetPendingInboundBytes(), static_cast<int64>(5 + 3 + 300000));
			Events.Connection->Consumed(5 + 3 + 300000);
			TestEqual(FString::Printf(TEXT("%s: ... none after Consumed"), Scheme.Name), Events.Connection->GetPendingInboundBytes(), static_cast<int64>(0));
		}
		Events.Connection->Close(1000, TEXT("done"));
		Events.WaitEnd();
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Run them to see them fail**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Receive`
Expected: all five FAIL (no `Message` event is queued yet).

- [ ] **Step 3: The members** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, after `int32 OnVerifyServerCertificate(...);` add:

```cpp
		int32 OnReceive(FService& Service, lws* InWsi, const void* In, size_t Len);
		/** A protocol failure (1007, 1009): VaRest closes the connection itself, By Transport (spec 2.3) */
		void Fail(FService& Service, int32 Code, const TCHAR* Reason, EVaRestSocketFailure Failure);
```

and at the end of the members:

```cpp
		// ---- receive (spec 2.3)
		TArray<uint8> Assembly;
		EVaRestSocketFrameKind AssemblyKind = EVaRestSocketFrameKind::Text;
		bool bAssembling = false;
```

- [ ] **Step 4: The dispatch and the limit's clamp** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.cpp`, in `FConn::OnCallback` add before `case LWS_CALLBACK_CLIENT_WRITEABLE:`:

```cpp
	case LWS_CALLBACK_CLIENT_RECEIVE:
		// in: a chunk of a data frame, len its size: every frame end and every rx_buffer_size (T8, libwebsockets.h:1345)
		return OnReceive(Service, InWsi, In, Len);
```

In `FConn::Prepare`, after the `ConnectTimeout` check, add:

```cpp
	// A TArray holds the assembly (plan decision 17)
	if (Request.MaxMessageBytes < 1 || Request.MaxMessageBytes > static_cast<int64>(MAX_int32) - 1)
	{
		const int64 Clamped = FMath::Clamp<int64>(Request.MaxMessageBytes, 1, static_cast<int64>(MAX_int32) - 1);
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: MaxMessageBytes %lld is outside [1, %d]; %lld is used"), *Label, static_cast<long long>(Request.MaxMessageBytes), MAX_int32 - 1, static_cast<long long>(Clamped));
		Request.MaxMessageBytes = Clamped;
	}
```

- [ ] **Step 5: The receive path** — create `Source/VaRestWebSocket/Private/VaRestLwsConnReceive.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "VaRestJsonUtf8.h"
#include "VaRestWebSocketLog.h"

int32 VaRest::WebSocket::Lws::FConn::OnReceive(FService& Service, lws* InWsi, const void* In, size_t Len)
{
	// A connection that is closing (ours, or a protocol failure) delivers nothing more
	if (State != EConnState::Open || bCloseQueued)
	{
		return 0;
	}
	if (!bAssembling)
	{
		bAssembling = true;
		// The kind from the first fragment (spec 2.3); LWS repeats the flag on every chunk [rx.binary_flag_on_continuations]
		AssemblyKind = lws_frame_is_binary(InWsi) ? EVaRestSocketFrameKind::Binary : EVaRestSocketFrameKind::Text;
		Assembly.Reset();
	}
	const int64 Remaining = static_cast<int64>(lws_remaining_packet_payload(InWsi));
	// The size limit as chunks arrive (spec 2.3): over it, the message is dropped and the connection closed with 1009
	if (static_cast<int64>(Assembly.Num()) + static_cast<int64>(Len) > Request.MaxMessageBytes)
	{
		Fail(Service, 1009, TEXT("message too big"), EVaRestSocketFailure::MessageTooBig);
		return 0;
	}
	// Bytes, appended (spec 2.3): nothing is converted per chunk, so a code point cut by a chunk or a frame is whole again (E9).
	// Room for what arrived and the slack byte, grown geometrically from the bytes received, so many 64 KiB chunks are not
	// copied once each -- never from the frame's declared length, which would let a server make every connection commit up
	// to MaxMessageBytes by declaring a huge frame and sending one byte (review conc M-13)
	const int64 Need = FMath::Min<int64>(static_cast<int64>(Assembly.Num()) + static_cast<int64>(Len) + 1, Request.MaxMessageBytes + 1);
	if (Assembly.Max() < Need)
	{
		Assembly.Reserve(static_cast<int32>(FMath::Min<int64>(FMath::Max<int64>(Need, static_cast<int64>(Assembly.Max()) * 2), Request.MaxMessageBytes + 1)));
	}
	if (In && Len > 0)
	{
		Assembly.Append(static_cast<const uint8*>(In), static_cast<int32>(Len));
	}
	// Complete when the frame is final and nothing of it remains (T8) [rx.final_flag, rx.remaining_payload]
	if (!lws_is_final_fragment(InWsi) || Remaining != 0)
	{
		return 0;
	}
	bAssembling = false;
	if (AssemblyKind == EVaRestSocketFrameKind::Text
		&& !VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Assembly.GetData()), Assembly.Num())))
	{
		// Once, on the whole message, with VaRestJson's validator (spec 2.3, core 2.2): invalid closes with 1007 (RFC 6455 8.1)
		Fail(Service, 1007, TEXT("invalid UTF-8"), EVaRestSocketFailure::InvalidUtf8);
		return 0;
	}
	FVaRestSocketEvent Message;
	Message.Type = EVaRestSocketEventType::Message;
	Message.Kind = AssemblyKind;
	// The slack byte for FVaRestJson::ParseOwned: reserved above, and ensured by the link (Task 3)
	Message.Bytes = MoveTemp(Assembly);
	Link->Push(MoveTemp(Message));
	Assembly.Reset();
	return 0;
}

void VaRest::WebSocket::Lws::FConn::Fail(FService& Service, int32 Code, const TCHAR* Reason, EVaRestSocketFailure Failure)
{
	// The partial message and what was queued to send are dropped and the close goes next: between two of our fragments if need
	// be, which RFC 6455 5.4 allows a control frame (plan decision 18)
	Assembly.Empty();
	bAssembling = false;
	DropOutbox();
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: %s, closing with %d"), *Label, LexToString(Failure), Code);
	bool bTruncated = false;
	Close(Service, Code, CloseReasonBytes(Reason, bTruncated), EVaRestSocketCloseBy::Transport, Failure);
}
```

- [ ] **Step 6: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Receive`
Expected: `run_tests: 5 passed, 0 failed`.

- [ ] **Step 7: Restore-the-bug** — (a) spec §16.5 item 6: validate each chunk instead of the message — in `OnReceive`, before `Assembly.Append(...)`, add `if (AssemblyKind == EVaRestSocketFrameKind::Text && !VaRest::Json::IsValidUtf8(FUtf8StringView(static_cast<const UTF8CHAR*>(In), static_cast<int32>(Len)))) { Fail(Service, 1007, TEXT("invalid UTF-8"), EVaRestSocketFailure::InvalidUtf8); return 0; }`: `Receive.SplitCodePoint` FAILS (Closed 1007 at the cut); remove it; PASSES. (b) Spec §16.5 item 9: delete the size check: `Receive.Invalid` FAILS ("the oversize message is not delivered": 200000 bytes arrive past a 65536 limit); restore; PASSES. (c) Delete the UTF-8 check: `Receive.Invalid` FAILS ("the invalid text is not delivered"); restore; PASSES.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConn.h Source/VaRestWebSocket/Private/VaRestLwsConn.cpp \
  Source/VaRestWebSocket/Private/VaRestLwsConnReceive.cpp Source/VaRestTests/Private/WebSocket/VaRestWsReceiveTest.cpp
git commit -m "ws: W1a task 12 -- receive: chunks assembled as bytes, UTF-8 checked once on the whole message (1007), MaxMessageBytes as chunks arrive (1009), one ordered queue (restore-the-bug 6, 9)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 13: TLS — the engine's roots and pins, the IP-literal check (VaRest-sjj), insecure, `TlsVerification`

Certificate verification as the engine does it (roots from the certificate manager, its pin check for the host without the port; `LwsWebSocket.cpp:549-572`), plus VaRest's own IP SAN check where LWS 3.0 cannot (plan decision 9, ruling Q3: error 62 only, at depth 0, for an IP-literal host, when `X509_check_ip_asc` matches the leaf), and a distinct `TlsVerification` failure naming the X509 error. VaRest's decision is a free function, `EvaluateServerCertificate`, so a unit test runs it inside OpenSSL's real verification on certificates made in memory (review spec I-5): the DNS-only and wrong-IP cases of ruling Q3 need certificates the test CA, whose key is gone (`Tools/testserver/certs/README.md`), cannot sign. The wrong IP is also tested through the verify callback against the server (review conc M-10).

**Files:**
- Modify: `Source/VaRestWebSocket/Private/VaRestLwsConnTls.cpp` (the full check), `Source/VaRestWebSocket/Private/VaRestLwsConn.h` (two free functions), `Source/VaRestWebSocket/Public/VaRestSocketTesting.h`, `Source/VaRestWebSocket/Private/VaRestSocketTesting.cpp` (`CertificateMatchesIp`), `Source/VaRestTests/VaRestTests.Build.cs` (`Projects`)
- Test: `Source/VaRestTests/Private/WebSocket/VaRestWsTlsTest.cpp`, `Source/VaRestWebSocket/Private/Tests/VaRestLwsTlsTest.cpp` (new: OpenSSL's verification on in-memory certificates)

**Interfaces:**
- Consumes: Task 10's `OnVerifyServerCertificate`; `FService::GetCertificates()` (Task 6); `FSocketTarget::bHostIsIp`, `Host`, `bInsecure` (Task 4).
- Produces: `bool VaRest::WebSocket::Lws::CertificateMatchesIp(X509* Certificate, const FString& Ip)` (any thread); `int32 VaRest::WebSocket::Lws::EvaluateServerCertificate(const FSocketTarget& Target, ISslCertificateManager* Certificates, X509_STORE_CTX* Store, int32 PreverifyOk, FString& OutDetail)` (any thread; LWS's convention, 0 accepts and 1 refuses; `Certificates` null skips the pin check); `VARESTWEBSOCKET_API bool VaRest::WebSocket::Testing::CertificateMatchesIp(TConstArrayView<uint8> Pem, FStringView Ip)`.

- [ ] **Step 1: Write the failing tests** — in `Source/VaRestTests/VaRestTests.Build.cs`, add to `PrivateDependencyModuleNames` after `"VaRestWebSocket",`:

```csharp
			// WebSocket W1: IPluginManager, to read the test server's certificate from the plugin (VaRest.Integration.WebSocket.Transport.Tls.CertificateIp)
			"Projects",
```

Create `Source/VaRestTests/Private/WebSocket/VaRestWsTlsTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 2.2 TLS: the engine's roots and pins, IP-literal hosts (VaRest-sjj), wss+insecure, refusals named

#if WITH_DEV_AUTOMATION_TESTS

#include "Interfaces/IPluginManager.h"
#include "Interfaces/ISslCertificateManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Ssl.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsTlsTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** The runner's own TLS URL form: an IP literal (Tools/run_tests.sh, --host 127.0.0.1) */
	FScheme IpLiteral(const FSite& Site)
	{
		return { TEXT("wss-ip"), FString::Printf(TEXT("wss://%s:%d"), *Site.Host, Site.TlsPort), true };
	}

	const FVaRestSocketEvent* TlsRefusal(const FEvents& Events)
	{
		const FVaRestSocketEvent* End = Events.GetEnd();
		return End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::TlsVerification ? End : nullptr;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsIpLiteralTest, "VaRest.Integration.WebSocket.Transport.Tls.IpLiteral", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsIpLiteralTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
	};
	// VaRest-sjj: the test certificate has IP:127.0.0.1 in its SAN (Tools/testserver/certs/README.md); LWS 3.0 alone refuses it
	// with X509 error 62 [tls.hostname_ip_literal], VaRest's own check lets it through (plan decision 9)
	const FScheme Ip = IpLiteral(Site);
	FEvents Events(Transport->Connect(Request(Ip, TEXT("/ws/echo?key=") + NewKey())));
	const bool bOpened = Events.WaitType(EVaRestSocketEventType::Open);
	if (TestTrue(FString::Printf(TEXT("wss to an IP literal with an IP SAN opens (%s)"), *Events.Describe()), bOpened))
	{
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("over the IP"))));
		TestTrue(TEXT("... and echoes"), Events.WaitMessages(1));
		Events.Connection->Close(1000, TEXT("done"));
	}
	Events.WaitEnd();
	const TUniquePtr<FEvents> Server = Visit(*Transport, Request(Ip, TEXT("/ws/close/4001?key=") + NewKey()));
	const FVaRestSocketEvent* End = Server->GetEnd();
	TestTrue(FString::Printf(TEXT("... and a server close arrives over it (%s)"), *Server->Describe()), End && End->End.Code == 4001 && End->End.By == EVaRestSocketCloseBy::Server);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsRefusedTest, "VaRest.Integration.WebSocket.Transport.Tls.Refused", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// The test CA is not trusted here: only the engine's roots
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Untrusting = MakeTransport(*this, Site, false);
	ON_SCOPE_EXIT
	{
		Untrusting->Shutdown();
	};
	const FString NameKey = NewKey();
	const TUniquePtr<FEvents> ByName = Visit(*Untrusting, Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NameKey));
	const FVaRestSocketEvent* NameEnd = TlsRefusal(*ByName);
	TestTrue(FString::Printf(TEXT("an untrusted chain is TlsVerification, naming X509 error 19 [tls.untrusted_rejected] (%s)"), *ByName->Describe()),
		NameEnd && NameEnd->End.Detail.Contains(TEXT("X509 error 19"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("... and no request reached the server"), FetchJournal(*this, Site, NameKey, 1, 300)[TEXT("connections")].AsInt(), 0);
	// The IP check clears a host-name mismatch only, at the leaf: a chain error comes first and stands (plan decision 9)
	const TUniquePtr<FEvents> ByIp = Visit(*Untrusting, Request(IpLiteral(Site), TEXT("/ws/echo?key=") + NewKey()));
	const FVaRestSocketEvent* IpEnd = TlsRefusal(*ByIp);
	TestTrue(FString::Printf(TEXT("an untrusted chain to an IP with an IP SAN is still refused, error 19 (%s)"), *ByIp->Describe()),
		IpEnd && IpEnd->End.Detail.Contains(TEXT("X509 error 19"), ESearchCase::CaseSensitive));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsWrongIpTest, "VaRest.Integration.WebSocket.Transport.Tls.WrongIp", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsWrongIpTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Ruling Q3, through the verify callback (review conc M-10): wss://10.0.0.1 with TCP to the server (127.0.0.1), whose leaf
	// has IP:127.0.0.1 and IP:::1 only. The chain is trusted (the test CA); the IP is not the host's, so error 62 stands
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
	};
	const FScheme Wrong = { TEXT("wss-wrong-ip"), FString::Printf(TEXT("wss://10.0.0.1:%d"), Site.TlsPort), true };
	const FString Key = NewKey();
	const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Wrong, TEXT("/ws/echo?key=") + Key));
	const FVaRestSocketEvent* End = TlsRefusal(*Events);
	TestTrue(FString::Printf(TEXT("a certificate for another IP is TlsVerification, X509 error 62 at depth 0 (%s)"), *Events->Describe()),
		End && End->End.Detail.Contains(TEXT("X509 error 62 at depth 0"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("... and no request reached the server"), FetchJournal(*this, Site, Key, 1, 300)[TEXT("connections")].AsInt(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsInsecureTest, "VaRest.Integration.WebSocket.Transport.Tls.Insecure", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsInsecureTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Spec 2.2, 15: outside Shipping, wss+insecure and bAllowSelfSignedTls allow the self-signed chain and skip the name check
	// [tls.insecure_skip_hostname]; ALLOW_SELFSIGNED alone would fail the IP host [tls.insecure_ip_host]
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Untrusting = MakeTransport(*this, Site, false);
	ON_SCOPE_EXIT
	{
		Untrusting->Shutdown();
	};
	FVaRestSocketConnectRequest Insecure;
	Insecure.Url = FString::Printf(TEXT("wss+insecure://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey());
	const TUniquePtr<FEvents> Scheme = Visit(*Untrusting, MoveTemp(Insecure));
	TestNotNull(FString::Printf(TEXT("wss+insecure to an IP with an untrusted chain opens (%s)"), *Scheme->Describe()), Scheme->Find(EVaRestSocketEventType::Open));
	FVaRestSocketConnectRequest Allowed = Request(IpLiteral(Site), TEXT("/ws/echo?key=") + NewKey());
	Allowed.bAllowSelfSignedTls = true;
	const TUniquePtr<FEvents> Flag = Visit(*Untrusting, MoveTemp(Allowed));
	TestNotNull(FString::Printf(TEXT("bAllowSelfSignedTls on wss does the same (%s)"), *Flag->Describe()), Flag->Find(EVaRestSocketEventType::Open));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsPinTest, "VaRest.Integration.WebSocket.Transport.Tls.PinMismatch", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsPinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsTlsTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	ISslCertificateManager& Certificates = FSslModule::Get().GetCertificateManager();
	// The host project pins nothing; with pins of its own this test could not clear them back (ClearAllPinnedPublicKeys)
	if (!TestFalse(TEXT("no pinned keys before the test"), Certificates.HasPinnedPublicKeys()))
	{
		return false;
	}
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Certificates.ClearAllPinnedPublicKeys();
		Transport->Shutdown();
	};
	// Spec 2.2: keys pinned on the engine's certificate manager apply for free (ISslCertificateManager.h:38-56); a SHA-256 of
	// 32 zero bytes matches no key
	Certificates.SetPinnedPublicKeys(TEXT("localhost"), TEXT("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA="));
	const TUniquePtr<FEvents> Pinned = Visit(*Transport, Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NewKey()));
	const FVaRestSocketEvent* End = TlsRefusal(*Pinned);
	TestTrue(FString::Printf(TEXT("a pin that does not match is TlsVerification (%s)"), *Pinned->Describe()), End && End->End.Detail.Contains(TEXT("pinned"), ESearchCase::CaseSensitive));
	Certificates.ClearAllPinnedPublicKeys();
	const TUniquePtr<FEvents> Unpinned = Visit(*Transport, Request(Schemes(Site)[1], TEXT("/ws/echo?key=") + NewKey()));
	TestNotNull(FString::Printf(TEXT("without it the same server opens (%s)"), *Unpinned->Describe()), Unpinned->Find(EVaRestSocketEventType::Open));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTlsCertificateIpTest, "VaRest.Integration.WebSocket.Transport.Tls.CertificateIp", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTlsCertificateIpTest::RunTest(const FString& Parameters)
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("VaRest"));
	if (!TestTrue(TEXT("the VaRest plugin"), Plugin.IsValid()))
	{
		return false;
	}
	TArray<uint8> Pem;
	const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Tools/testserver/certs/test-server.pem"));
	if (!TestTrue(FString::Printf(TEXT("the test server's certificate (%s)"), *Path), FFileHelper::LoadFileToArray(Pem, *Path)))
	{
		return false;
	}
	// The leaf's SAN: DNS:localhost, IP:127.0.0.1, IP:::1 (Tools/testserver/certs/README.md)
	TestTrue(TEXT("127.0.0.1 is in the SAN"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("127.0.0.1")));
	TestTrue(TEXT("::1 is"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("::1")));
	TestFalse(TEXT("10.0.0.1 is not"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("10.0.0.1")));
	TestFalse(TEXT("a name is not an IP"), VaRest::WebSocket::Testing::CertificateMatchesIp(Pem, TEXT("localhost")));
	TestFalse(TEXT("no certificate matches nothing"), VaRest::WebSocket::Testing::CertificateMatchesIp(TConstArrayView<uint8>(), TEXT("127.0.0.1")));
	return true;
}

#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Tls`
Expected: FAIL to compile (`VaRest::WebSocket::Testing::CertificateMatchesIp` is not declared).

- [ ] **Step 3: The IP check** — in `Source/VaRestWebSocket/Private/VaRestLwsConn.h`, before `enum class EConnState`, add:

```cpp
	/** Any thread: Ip (no brackets) is one of Certificate's iPAddress SAN entries (VaRest-sjj, plan decision 9) */
	bool CertificateMatchesIp(X509* Certificate, const FString& Ip);

	/**
	 * Any thread: VaRest's verdict on one certificate check of a wss attempt (plan decisions 9, 24; ruling Q3), in LWS's
	 * convention for PERFORM_SERVER_CERT_VERIFICATION (libwebsockets.h:1028-1035): 0 accepts -- with the store's error reset
	 * to X509_V_OK when VaRest overrules OpenSSL -- and 1 refuses, OutDetail then naming why. Certificates null (tests): no pin
	 * check. FConn::OnVerifyServerCertificate calls it after its redirect guard; a unit test calls it from OpenSSL's own
	 * verify callback (review spec I-5)
	 */
	int32 EvaluateServerCertificate(const FSocketTarget& Target, ISslCertificateManager* Certificates, X509_STORE_CTX* Store, int32 PreverifyOk, FString& OutDetail);
```

Replace the whole of `Source/VaRestWebSocket/Private/VaRestLwsConnTls.cpp` with:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "Interfaces/ISslCertificateManager.h"
#include "VaRestWebSocketLog.h"

bool VaRest::WebSocket::Lws::CertificateMatchesIp(X509* Certificate, const FString& Ip)
{
	// X509_check_ip_asc compares the iPAddress SAN entries (OpenSSL 1.1.1t include/openssl/x509v3.h:714): 1 a match, 0 none, < 0 not an address
	return Certificate && X509_check_ip_asc(Certificate, TCHAR_TO_UTF8(*Ip), 0) == 1;
}

int32 VaRest::WebSocket::Lws::EvaluateServerCertificate(const FSocketTarget& Target, ISslCertificateManager* Certificates, X509_STORE_CTX* Store, int32 PreverifyOk, FString& OutDetail)
{
	if (Target.bInsecure)
	{
		// Plan decision 24: what LWS's flags allow decides (a self-signed chain, no name check; spec 2.2); no pin check
		return 0;
	}
	int32 Ok = PreverifyOk;
	const int32 Error = X509_STORE_CTX_get_error(Store);
	const int32 Depth = X509_STORE_CTX_get_error_depth(Store);
	if (!Ok && Error == X509_V_ERR_HOSTNAME_MISMATCH && Depth == 0 && Target.bHostIsIp
		&& CertificateMatchesIp(X509_STORE_CTX_get_current_cert(Store), Target.Host))
	{
		// VaRest-sjj (plan decision 9, ruling Q3): LWS 3.0 checks an IP host as a DNS name and fails it even with that IP in the
		// SAN [tls.hostname_ip_literal]; the SAN's IP entries are checked here instead. Only this error, at the leaf, for an IP
		// host, when the leaf has that IP: a chain error comes before it and stands, a wrong IP or a DNS-only SAN stands, and
		// OpenSSL's later checks (signatures, dates) still run and fail on their own errors. LWS keeps a verdict the callback
		// reset to X509_V_OK and returned 0 for (libwebsockets.h:1028-1035)
		X509_STORE_CTX_set_error(Store, X509_V_OK);
		Ok = 1;
	}
	if (!Ok)
	{
		OutDetail = FString::Printf(TEXT("the server's certificate was refused: X509 error %d at depth %d (%s)"), Error, Depth, UTF8_TO_TCHAR(X509_verify_cert_error_string(Error)));
		return 1;
	}
	// The certificate manager's own check (pinned keys) on what OpenSSL accepted, for the host without its port: the engine's
	// step (LwsWebSocket.cpp:549-572; ISslCertificateManager.h:58-66)
	if (Certificates && !Certificates->VerifySslCertificates(Store, Target.Host))
	{
		OutDetail = FString::Printf(TEXT("the certificate manager refused the chain for %s (a pinned key that does not match)"), *Target.Host);
		return 1;
	}
	return 0;
}

int32 VaRest::WebSocket::Lws::FConn::OnVerifyServerCertificate(FService& Service, X509_STORE_CTX* Store, int32 PreverifyOk)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an attempt VaRest already ended finishes no TLS handshake
		return 1;
	}
	if (!Target.bTls || AppendCalls > 0)
	{
		// A redirect's TLS handshake: a ws:// attempt has none of its own, and a wss:// attempt's own comes before its request
		// was built (APPEND_HANDSHAKE_HEADER) -- so LWS is connecting to a redirect's target [hs.redirect_to_tls]. Refused
		// before that handshake ends, so no request reaches the target (ruling Q2, plan decision 10)
		PendingFailure = EVaRestSocketFailure::Redirect;
		PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow (ruling Q2)");
		return 1;
	}
	FString Detail;
	const int32 Verdict = EvaluateServerCertificate(Target, Service.GetCertificates(), Store, PreverifyOk, Detail);
	if (Verdict != 0)
	{
		// Terminal (ruling Q3, spec 3.4 as Task 14 amends it): W2 never retries it
		PendingFailure = EVaRestSocketFailure::TlsVerification;
		PendingDetail = MoveTemp(Detail);
	}
	return Verdict;
}
```

Create `Source/VaRestWebSocket/Private/Tests/VaRestLwsTlsTest.cpp` (review spec I-5: ruling Q3's cases through OpenSSL's real verification, on certificates made here):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestLwsConn.h"

namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate
{
	/** The target the verify callback evaluates: one verification at a time, on the game thread */
	const FSocketTarget* CurrentTarget = nullptr;

	/**
	 * OpenSSL's verify callback, forwarding to VaRest's decision with LWS 3.0's convention (libwebsockets.h:1028-1035): a
	 * verdict of 1 refuses in any case; 0 accepts only when no error is left (VaRest reset it, or there was none)
	 */
	int VerifyAsLws(int PreverifyOk, X509_STORE_CTX* Store)
	{
		FString Detail;
		if (EvaluateServerCertificate(*CurrentTarget, nullptr, Store, PreverifyOk, Detail) != 0)
		{
			return 0;
		}
		return PreverifyOk || X509_STORE_CTX_get_error(Store) == X509_V_OK ? 1 : 0;
	}

	/** An EC P-256 key; null on failure */
	EVP_PKEY* MakeKey()
	{
		EVP_PKEY* Key = nullptr;
		EVP_PKEY_CTX* Context = EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr);
		if (Context && EVP_PKEY_keygen_init(Context) == 1 && EVP_PKEY_CTX_set_ec_paramgen_curve_nid(Context, NID_X9_62_prime256v1) == 1)
		{
			EVP_PKEY_keygen(Context, &Key);
		}
		EVP_PKEY_CTX_free(Context);
		return Key;
	}

	/** A self-signed leaf for Key, valid for an hour either side of now, its SAN as given ("IP:127.0.0.1", "DNS:localhost"); null on failure */
	X509* MakeLeaf(EVP_PKEY* Key, const char* SubjectAltName)
	{
		X509* Leaf = X509_new();
		if (!Leaf)
		{
			return nullptr;
		}
		X509_set_version(Leaf, 2);
		ASN1_INTEGER_set(X509_get_serialNumber(Leaf), 1);
		X509_gmtime_adj(X509_getm_notBefore(Leaf), -3600);
		X509_gmtime_adj(X509_getm_notAfter(Leaf), 3600);
		X509_set_pubkey(Leaf, Key);
		X509_NAME* Name = X509_get_subject_name(Leaf);
		X509_NAME_add_entry_by_txt(Name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("varest-tls-test"), -1, -1, 0);
		X509_set_issuer_name(Leaf, Name);
		// A null config and context are enough for DNS and IP entries (x509v3.h X509V3_EXT_conf_nid)
		X509_EXTENSION* San = X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name, const_cast<char*>(SubjectAltName));
		const bool bMade = San && X509_add_ext(Leaf, San, -1) == 1 && X509_sign(Leaf, Key, EVP_sha256()) > 0;
		X509_EXTENSION_free(San);
		if (!bMade)
		{
			X509_free(Leaf);
			return nullptr;
		}
		return Leaf;
	}

	struct FOutcome
	{
		bool bVerified = false;
		int32 Error = -1;
		int32 Depth = -1;
	};

	/** OpenSSL's verification of Leaf, Trusted in the store (or nothing), the host name set as LWS 3.0 sets it: an IP host as a DNS name [tls.hostname_ip_literal] */
	FOutcome Verify(X509* Leaf, X509* Trusted, const FSocketTarget& Target)
	{
		FOutcome Out;
		X509_STORE* Store = X509_STORE_new();
		X509_STORE_CTX* Context = X509_STORE_CTX_new();
		if (Store && Context && (!Trusted || X509_STORE_add_cert(Store, Trusted) == 1) && X509_STORE_CTX_init(Context, Store, Leaf, nullptr) == 1)
		{
			X509_VERIFY_PARAM_set1_host(X509_STORE_CTX_get0_param(Context), TCHAR_TO_UTF8(*Target.Host), 0);
			X509_STORE_CTX_set_verify_cb(Context, &VerifyAsLws);
			CurrentTarget = &Target;
			Out.bVerified = X509_verify_cert(Context) == 1;
			CurrentTarget = nullptr;
			Out.Error = X509_STORE_CTX_get_error(Context);
			Out.Depth = X509_STORE_CTX_get_error_depth(Context);
		}
		X509_STORE_CTX_free(Context);
		X509_STORE_free(Store);
		ERR_clear_error();
		return Out;
	}

	FSocketTarget TargetFor(const TCHAR* Host, bool bIp)
	{
		FSocketTarget Target;
		Target.bTls = true;
		Target.Host = Host;
		Target.bHostIsIp = bIp;
		return Target;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsTlsIpCheckTest, "VaRest.WebSocket.Lws.TlsIpCheck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsTlsIpCheckTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsTlsTestPrivate;
	EVP_PKEY* Key = MakeKey();
	X509* IpLeaf = Key ? MakeLeaf(Key, "IP:127.0.0.1") : nullptr;
	X509* WrongIpLeaf = Key ? MakeLeaf(Key, "IP:10.0.0.1") : nullptr;
	X509* DnsOnlyLeaf = Key ? MakeLeaf(Key, "DNS:localhost") : nullptr;
	ON_SCOPE_EXIT
	{
		// Each takes null
		X509_free(IpLeaf);
		X509_free(WrongIpLeaf);
		X509_free(DnsOnlyLeaf);
		EVP_PKEY_free(Key);
	};
	if (!TestTrue(TEXT("the key and the three leaves are made"), IpLeaf && WrongIpLeaf && DnsOnlyLeaf))
	{
		return false;
	}
	const FSocketTarget Ip = TargetFor(TEXT("127.0.0.1"), true);
	// Ruling Q3: accepted only for an IP host, at depth 0, when the leaf has that IP
	const FOutcome Right = Verify(IpLeaf, IpLeaf, Ip);
	TestTrue(FString::Printf(TEXT("a trusted leaf with the host's IP SAN is accepted (error %d)"), Right.Error), Right.bVerified && Right.Error == X509_V_OK);
	const FOutcome WrongIp = Verify(WrongIpLeaf, WrongIpLeaf, Ip);
	TestTrue(FString::Printf(TEXT("a trusted leaf for another IP is refused, error 62 at depth 0 (error %d, depth %d)"), WrongIp.Error, WrongIp.Depth),
		!WrongIp.bVerified && WrongIp.Error == X509_V_ERR_HOSTNAME_MISMATCH && WrongIp.Depth == 0);
	const FOutcome DnsOnly = Verify(DnsOnlyLeaf, DnsOnlyLeaf, Ip);
	TestTrue(FString::Printf(TEXT("a trusted leaf with a DNS-only SAN is refused for an IP host, error 62 (error %d)"), DnsOnly.Error),
		!DnsOnly.bVerified && DnsOnly.Error == X509_V_ERR_HOSTNAME_MISMATCH);
	// The chain error comes first and stands: the IP check clears error 62 and nothing else
	const FOutcome Untrusted = Verify(IpLeaf, nullptr, Ip);
	TestTrue(FString::Printf(TEXT("an untrusted leaf with the host's IP SAN is still refused, error 18 (error %d)"), Untrusted.Error),
		!Untrusted.bVerified && Untrusted.Error == X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT);
	// The rule is for IP hosts only: a name host keeps OpenSSL's name check
	const FOutcome NameOnIpLeaf = Verify(IpLeaf, IpLeaf, TargetFor(TEXT("localhost"), false));
	TestTrue(FString::Printf(TEXT("a name host against an IP-only leaf is refused, error 62 (error %d)"), NameOnIpLeaf.Error),
		!NameOnIpLeaf.bVerified && NameOnIpLeaf.Error == X509_V_ERR_HOSTNAME_MISMATCH);
	const FOutcome Name = Verify(DnsOnlyLeaf, DnsOnlyLeaf, TargetFor(TEXT("localhost"), false));
	TestTrue(FString::Printf(TEXT("a name host against its DNS SAN is accepted (error %d)"), Name.Error), Name.bVerified);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 4: The test hook** — in `Source/VaRestWebSocket/Public/VaRestSocketTesting.h`, before the namespace's closing brace, add:

```cpp
	/** Any thread: the first certificate in Pem has Ip in its iPAddress SAN entries (the check VaRest-sjj's decision adds) */
	VARESTWEBSOCKET_API bool CertificateMatchesIp(TConstArrayView<uint8> Pem, FStringView Ip);
```

In `Source/VaRestWebSocket/Private/VaRestSocketTesting.cpp`, add `#include "VaRestLwsConn.h"` to the includes and, before `#endif // WITH_DEV_AUTOMATION_TESTS`:

```cpp
bool VaRest::WebSocket::Testing::CertificateMatchesIp(TConstArrayView<uint8> Pem, FStringView Ip)
{
	if (Pem.Num() == 0)
	{
		return false;
	}
	BIO* Bio = BIO_new_mem_buf(Pem.GetData(), Pem.Num());
	if (!Bio)
	{
		return false;
	}
	X509* Certificate = PEM_read_bio_X509(Bio, nullptr, nullptr, nullptr);
	BIO_free(Bio);
	ERR_clear_error();
	const bool bMatch = VaRest::WebSocket::Lws::CertificateMatchesIp(Certificate, FString(Ip));
	// X509_free takes null
	X509_free(Certificate);
	return bMatch;
}
```

- [ ] **Step 5: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Tls` then `Tools/run_tests.sh --no-build VaRest.WebSocket.Lws.TlsIpCheck`
Expected: `run_tests: 6 passed, 0 failed` (IpLiteral, Refused, WrongIp, Insecure, PinMismatch, CertificateIp); then `run_tests: 1 passed, 0 failed`. Then the whole transport suite, wss everywhere: `Tools/run_tests.sh VaRest.Integration.WebSocket` — `0 failed`.

- [ ] **Step 6: Restore-the-bug** (in `EvaluateServerCertificate`) — (a) delete the IP-mismatch branch: `Tls.IpLiteral` FAILS (TlsVerification, "X509 error 62 at depth 0") [tls.hostname_ip_literal], and `VaRest.WebSocket.Lws.TlsIpCheck` at "a trusted leaf with the host's IP SAN is accepted"; restore; PASSES. (b) Widen it to any refusal on an IP host (`if (!Ok && Target.bHostIsIp)` alone): `Tls.Refused` FAILS at "an untrusted chain to an IP with an IP SAN is still refused", and `TlsIpCheck` at "an untrusted leaf ... error 18"; restore; PASSES. (c) Ruling Q3: drop the `CertificateMatchesIp(...)` condition (clear error 62 for any IP host at the leaf): `Tls.WrongIp` FAILS (it opens), and `TlsIpCheck` at "another IP is refused" and "a DNS-only SAN is refused for an IP host"; restore; PASSES. (d) Delete the `VerifySslCertificates` block: `Tls.PinMismatch` FAILS (the pinned server opens); restore; PASSES. Close VaRest-sjj only with both legs green (review spec I-5).

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestWebSocket/Private/VaRestLwsConnTls.cpp Source/VaRestWebSocket/Private/VaRestLwsConn.h \
  Source/VaRestWebSocket/Public/VaRestSocketTesting.h Source/VaRestWebSocket/Private/VaRestSocketTesting.cpp \
  Source/VaRestTests/VaRestTests.Build.cs Source/VaRestTests/Private/WebSocket/VaRestWsTlsTest.cpp \
  Source/VaRestWebSocket/Private/Tests/VaRestLwsTlsTest.cpp
git commit -m "ws: W1a task 13 -- TLS: the engine's roots and pin check, VaRest's own IP SAN check for IP-literal hosts (VaRest-sjj), wss+insecure, TlsVerification naming the X509 error

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 14: Editor coexistence, a shutdown stress, the spec corrections, the builds, beads — W1a's close

The engine's `IWebSocket` and VaRest's transport at once in the editor, ws and wss, each on its own LWS copy with its logs in place (spec §1.2, §17 W1 exit "editor coexistence"; VaRest-5ia M-1); a shutdown with connections in every state, repeated (VaRest-x2v's suspect); the spec passages W1a decided or found stale (VaRest-sjj, VaRest-rti, VaRest-2rt, VaRest-ek2, VaRest-8zj); the monolithic builds and the module-level non-unity compile.

**Files:**
- Create: `Source/VaRestTests/Private/WebSocket/VaRestWsCoexistTest.cpp`
- Modify: `CLAUDE.md` (module row, freeze bullet), `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` (the passages in Step 5)
- Temporarily modify, then undo line by line: `Source/VaRestWebSocket/VaRestWebSocket.Build.cs`, `Source/VaRestTests/VaRestTests.Build.cs`

**Interfaces:**
- Consumes: Tasks 1-13; `VaRest::WebSocket::Testing::{DescribeLwsCopy, GetLwsCreateContextAddress, DescribeLwsLogLevel}` (Task 7); the engine's `FWebSocketsModule::CreateWebSocket` through its virtual (include path only, `VaRestTests.Build.cs`).
- Produces: nothing new in code.

- [ ] **Step 1: The tests** — create `Source/VaRestTests/Private/WebSocket/VaRestWsCoexistTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 1.2 and 17 (W1 exit "editor coexistence"): the engine's IWebSocket and VaRest's transport open at once, ws and
// wss; VaRestWebSocket's own LWS copy and log level (VaRest-5ia M-1, D30); the engine's LWS lines still in LogWebSockets. And a
// transport shut down with connections in every state, ten times (VaRest-x2v's suspect: a teardown race)

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "IWebSocket.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"
#include "WebSocketsModule.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace VaRest::WsTest::VaRestWsCoexistTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** One engine IWebSocket and what it reported (its delegates run on the game thread, from the core ticker) */
	struct FEngineSide
	{
		bool bConnected = false;
		bool bEnded = false;
		FString Error;
		TArray<FString> Messages;
	};

	TSharedRef<IWebSocket> OpenEngineSocket(FWebSocketsModule& Module, const FString& Url, const TSharedRef<FEngineSide>& Side)
	{
		// CreateWebSocket is virtual (WebSocketsModule.h:72): no link to the WebSockets module. Connected from the test body,
		// never from a handler (E25)
		TSharedRef<IWebSocket> Socket = Module.CreateWebSocket(Url, FString());
		Socket->OnConnected().AddLambda([Side] { Side->bConnected = true; });
		Socket->OnMessage().AddLambda([Side](const FString& Message) { Side->Messages.Add(Message); });
		Socket->OnConnectionError().AddLambda([Side](const FString& Reason) { Side->Error = Reason; Side->bEnded = true; });
		Socket->OnClosed().AddLambda([Side](int32 Code, const FString& Reason, bool bWasClean) { Side->bEnded = true; });
		Socket->Connect();
		return Socket;
	}

	bool Echoed(const TSharedRef<FEngineSide>& Side, const TCHAR* Text)
	{
		return Side->Messages.ContainsByPredicate([Text](const FString& Each) { return Each.Equals(Text, ESearchCase::CaseSensitive); });
	}

	/** A log category's verbosity as "Log list <cat>" prints it (LogSuppressionInterface.cpp:593-614), the probe's way (VaRestLwsProbeTlsTest.cpp:181-210) */
	FString LogVerbosityOf(const TCHAR* Category)
	{
		FString Out;
#if UE_ALLOW_EXEC_COMMANDS
		if (!GEngine)
		{
			return Out;
		}
		FStringOutputDevice Listing;
		Listing.SetAutoEmitLineTerminator(true);
		GEngine->Exec(nullptr, *FString::Printf(TEXT("Log list %s"), Category), Listing);
		TArray<FString> Lines;
		Listing.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> Tokens;
			Line.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() >= 2 && Tokens[0].Equals(Category, ESearchCase::CaseSensitive))
			{
				Out = Tokens[1];
			}
		}
#endif
		return Out;
	}

	/** An engine socket and a VaRest connection open at once, both echo, both end; false with the reason when any step fails */
	bool BothAtOnce(FWebSocketsModule& Module, ILwsTestTransport& Transport, const FString& EngineUrl, FVaRestSocketConnectRequest&& Req, FString& OutWhy)
	{
		const TSharedRef<FEngineSide> Side = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> Socket = OpenEngineSocket(Module, EngineUrl, Side);
		FEvents Ours(Transport.Connect(MoveTemp(Req)));
		Ours.WaitFor([Side](const TArray<FVaRestSocketEvent>& All)
		{
			const bool bOursSettled = All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.Type == EVaRestSocketEventType::Open || Each.IsTerminal(); });
			return bOursSettled && (Side->bConnected || Side->bEnded);
		}, 20.0);
		const bool bBothOpen = Side->bConnected && Ours.Find(EVaRestSocketEventType::Open) != nullptr;
		bool bEchoed = false;
		if (bBothOpen)
		{
			Socket->Send(TEXT("engine-hello"));
			Ours.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("varest-hello"))));
			bEchoed = Ours.WaitFor([Side](const TArray<FVaRestSocketEvent>& All)
			{
				return Echoed(Side, TEXT("engine-hello")) && All.ContainsByPredicate([](const FVaRestSocketEvent& Each)
				{
					return Each.Type == EVaRestSocketEventType::Message && Utf8(Each.Bytes).Equals(TEXT("varest-hello"), ESearchCase::CaseSensitive);
				});
			}, 20.0);
		}
		// Both drain, whatever happened
		if (Side->bConnected && !Side->bEnded)
		{
			Socket->Close(1000, TEXT("done"));
		}
		if (!Ours.GetEnd())
		{
			Ours.Connection->Close(1000, TEXT("done"));
		}
		const bool bEnded = Ours.WaitFor([Side](const TArray<FVaRestSocketEvent>& All)
		{
			return (Side->bEnded || !Side->bConnected) && All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
		}, 20.0);
		OutWhy = FString::Printf(TEXT("engine: connected=%d ended=%d error=\"%s\" echoed=%d; VaRest: %s; both ended=%d"), Side->bConnected, Side->bEnded, *Side->Error,
			Echoed(Side, TEXT("engine-hello")), *Ours.Describe(), bEnded);
		return bBothOpen && bEchoed && bEnded;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCoexistEngineTest, "VaRest.Integration.WebSocket.Transport.Coexist.EngineWebSocket",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestWsCoexistEngineTest::RunTest(const FString& Parameters)
{
#if WITH_WEBSOCKETS
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsCoexistTestPrivate;
	ExpectLwsLines(*this);
	// The engine's own LWS lines (LwsWebSocketsManager.cpp:403-424) are this test's evidence, not failures
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	VaRest::Http::Testing::FLogCapture Capture;
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
	};
	const TArray<FScheme> Both = Schemes(Site);
	// VaRest's context first (ws only: no wss in flight while the engine module makes its context, VaRest-rti)
	TestNotNull(TEXT("VaRest's transport opens before the engine module loads"), Visit(*Transport, Request(Both[0], TEXT("/ws/echo?key=") + NewKey()))->Find(EVaRestSocketEventType::Open));
	const FString LevelBefore = VaRest::WebSocket::Testing::DescribeLwsLogLevel();
	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	const FString LevelAfter = VaRest::WebSocket::Testing::DescribeLwsLogLevel();
	// D30 (spec 1.2): in a modular build VaRest's copy keeps errors and warnings only; the engine's InitWebSockets set its own
	// copy's level (LwsWebSocketsManager.cpp:174-176) [coexist.shared_lws_state]
	TestEqualSensitive(TEXT("VaRest's LWS log level: errors and warnings (D30)"), LevelAfter, FString(TEXT("err=1 warn=2 notice=0 info=0 client=0")));
	TestEqualSensitive(TEXT("... untouched by the engine's WebSockets module"), LevelAfter, LevelBefore);
#if PLATFORM_LINUX
	// VaRest-5ia M-1: the per-copy claim rests on this measurement, not on RTLD_LOCAL alone (spec 1.2) [coexist.lws_symbol_owner]
	const FString OwnFile = VaRest::WebSocket::Testing::DescribeLwsCopy();
	TestTrue(FString::Printf(TEXT("VaRest's lws_create_context is in VaRestWebSocket's own library (%s)"), *OwnFile), OwnFile.Contains(TEXT("VaRestWebSocket"), ESearchCase::CaseSensitive));
	// The engine module's own path, from its object's vtable pointer (the Itanium ABI puts it first; VaRestLwsProbeTlsTest.cpp:486-503)
	Dl_info ModuleInfo = {};
	void* EngineLib = dladdr(*reinterpret_cast<void**>(&Module), &ModuleInfo) != 0 && ModuleInfo.dli_fname ? dlopen(ModuleInfo.dli_fname, RTLD_LAZY | RTLD_NOLOAD) : nullptr;
	if (TestNotNull(TEXT("the engine's WebSockets library"), EngineLib))
	{
		const void* EngineSymbol = dlsym(EngineLib, "lws_create_context");
		TestTrue(TEXT("... has an lws_create_context of its own, not VaRest's"), EngineSymbol != nullptr && EngineSymbol != VaRest::WebSocket::Testing::GetLwsCreateContextAddress());
		// RTLD_NOLOAD took a reference: give it back
		dlclose(EngineLib);
	}
#endif
	// The engine's leg needs its certificate check off for the test CA and the IP host: the probe's scope-guarded flip
	// (VaRestLwsProbeTlsTest.cpp:521-589, ruling Q4) -- restored on every path out of this block, then checked
	FString WsWhy;
	FString WssWhy;
	bool bWs = false;
	bool bWss = false;
	int32 EngineLwsLines = 0;
	int32 VaRestKeysInEngineCategory = 0;
	// Review spec M-4: VaRest's own emitter writes LogVaRestSocket only, so a count of its lines in LogWebSockets could never
	// fail. What would show VaRest's handshake running through the engine's LWS copy is the engine's Verbose client lines
	// naming a VaRest request: these keys are in VaRest's URLs only
	const FString OursWsKey = NewKey();
	const FString OursWssKey = NewKey();
	{
		bool bWasDisabled = false;
		const bool bHadKey = GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
		const FString OldVerbosity = LogVerbosityOf(TEXT("LogWebSockets"));
		ON_SCOPE_EXIT
		{
			// The engine re-reads the flag only when the key exists (LwsWebSocketsManager.cpp:93, ConfigCacheIni.cpp:5424-5438): the
			// old value goes back first and is read, then an absent key is removed again
			GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
			FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
			if (!bHadKey)
			{
				GConfig->RemoveKey(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), GEngineIni);
			}
#if UE_ALLOW_EXEC_COMMANDS
			if (!OldVerbosity.IsEmpty() && GEngine)
			{
				GEngine->Exec(nullptr, *FString::Printf(TEXT("Log LogWebSockets %s"), *OldVerbosity));
			}
#endif
		};
		GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), true, GEngineIni);
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
#if UE_ALLOW_EXEC_COMMANDS
		if (GEngine && !OldVerbosity.IsEmpty())
		{
			GEngine->Exec(nullptr, TEXT("Log LogWebSockets Verbose"));
		}
#endif
		bWs = BothAtOnce(Module, *Transport, FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *NewKey()), Request(Both[0], TEXT("/ws/echo?key=") + OursWsKey), WsWhy);
		bWss = BothAtOnce(Module, *Transport, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Request(Both[1], TEXT("/ws/echo?key=") + OursWssKey), WssWhy);
		for (const FString& Line : Capture.GetLines())
		{
			const bool bEngineCategory = Line.StartsWith(TEXT("LogWebSockets: "), ESearchCase::CaseSensitive);
			EngineLwsLines += bEngineCategory && Line.Contains(TEXT("Lws("), ESearchCase::CaseSensitive) ? 1 : 0;
			VaRestKeysInEngineCategory += bEngineCategory && (Line.Contains(OursWsKey, ESearchCase::CaseSensitive) || Line.Contains(OursWssKey, ESearchCase::CaseSensitive)) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("LogWebSockets verbosity before: %s"), OldVerbosity.IsEmpty() ? TEXT("(not read)") : *OldVerbosity));
	}
	TestTrue(FString::Printf(TEXT("ws: both open, echo and end at once (%s)"), *WsWhy), bWs);
	TestTrue(FString::Printf(TEXT("wss: both open, echo and end at once (%s)"), *WssWhy), bWss);
	TestEqual(TEXT("the engine's LWS copy never logged a VaRest request (each kept its own copy)"), VaRestKeysInEngineCategory, 0);
	AddInfo(FString::Printf(TEXT("%d libwebsockets lines of the engine's copy in LogWebSockets during the test [coexist.engine_lws_log_routed]"), EngineLwsLines));
	// Harness: the restore took (the probe's own check, VaRestLwsProbeTlsTest.cpp:630-660); -lws-insecure= overrides the ini
	bool bInsecureArg = false;
#if !UE_BUILD_SHIPPING
	const bool bHasInsecureArg = FParse::Bool(FCommandLine::Get(), TEXT("lws-insecure="), bInsecureArg);
#else
	const bool bHasInsecureArg = false;
#endif
	if (!bHasInsecureArg)
	{
		const TSharedRef<FEngineSide> Checked = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> CheckedSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Checked);
		VaRest::Http::Testing::PumpUntil([Checked] { return Checked->bConnected || Checked->bEnded; }, 20.0, true);
		const bool bOpened = Checked->bConnected;
		if (Checked->bConnected && !Checked->bEnded)
		{
			CheckedSocket->Close(1000, TEXT("done"));
		}
		VaRest::Http::Testing::PumpUntil([Checked] { return Checked->bEnded; }, 20.0, true);
		TestFalse(TEXT("the engine's certificate check is on again after the restore"), bOpened);
	}
	else
	{
		// The probe's case (VaRestLwsProbeTlsTest.cpp:667-680): the flag is re-set from the command line on every re-read
		AddInfo(TEXT("the restore check is skipped: -lws-insecure= on the command line (LwsWebSocketsManager.cpp:95-98)"));
	}
	return true;
#else
	AddInfo(TEXT("WITH_WEBSOCKETS is 0 on this platform: nothing to coexist with"));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCoexistShutdownTest, "VaRest.Integration.WebSocket.Transport.Coexist.ShutdownStress",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestWsCoexistShutdownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsCoexistTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	const TArray<FScheme> Both = Schemes(Site);
	for (int32 Round = 0; Round < 10; ++Round)
	{
		// One transport at a time (VaRest-rti): each round's is shut down before the next is made
		const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
		if (!Transport.IsValid())
		{
			return false;
		}
		TArray<TUniquePtr<FEvents>> All;
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[0], TEXT("/ws/hang?ms=20000&key=") + NewKey()))));   // pending
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[0], TEXT("/ws/sink?key=") + NewKey()))));            // open, writing
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[1], TEXT("/ws/echo?key=") + NewKey()))));            // open, TLS
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[0], TEXT("/ws/noecho?key=") + NewKey()))));          // closing
		for (int32 Index = 1; Index < All.Num(); ++Index)
		{
			TestTrue(FString::Printf(TEXT("round %d, connection %d: opened"), Round, Index), All[Index]->WaitType(EVaRestSocketEventType::Open, 10.0));
		}
		TArray<uint8> Big;
		Big.SetNumZeroed(4 * 1024 * 1024);
		All[1]->Connection->Send(FVaRestSocketFrame::Binary(Big));
		All[3]->Connection->Close(1000, TEXT("never echoed"));
		Transport->Shutdown();
		for (int32 Index = 0; Index < All.Num(); ++Index)
		{
			// Shutdown reports every connection before it returns (EndForShutdown): no pumping here. Without that, the
			// connections would still end -- from the callbacks lws_context_destroy makes -- but as Unknown or a transport
			// end, not Shutdown, which is how W2 tells an engine exit from a network failure
			All[Index]->Pull();
			const int32 Terminal = All[Index]->Count(EVaRestSocketEventType::Closed) + All[Index]->Count(EVaRestSocketEventType::Error);
			TestEqual(FString::Printf(TEXT("round %d, connection %d: exactly one terminal event (%s)"), Round, Index, *All[Index]->Describe()), Terminal, 1);
			const FVaRestSocketEvent* End = All[Index]->GetEnd();
			const EVaRestSocketEventType Wanted = Index == 0 ? EVaRestSocketEventType::Error : EVaRestSocketEventType::Closed;
			TestTrue(FString::Printf(TEXT("round %d, connection %d: %s with Failure Shutdown (%s)"), Round, Index, Index == 0 ? TEXT("Error") : TEXT("Closed 1006"), *All[Index]->Describe()),
				End && End->Type == Wanted && End->End.Failure == EVaRestSocketFailure::Shutdown && (Index == 0 || End->End.Code == 1006));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 2: Run them**

Run: `Tools/run_tests.sh VaRest.Integration.WebSocket.Transport.Coexist`
Expected: `run_tests: 2 passed, 0 failed`; the run COMPLETES (a `TEST COMPLETE. EXIT CODE: 0` line — the runner says `run did not complete` otherwise). Run it three times (`--no-build` after the first): VaRest-x2v's exit was intermittent. Report the three outcomes and the `[coexist.engine_lws_log_routed]` count.

- [ ] **Step 3: Restore-the-bug** — in `FConn::EndForShutdown` (`VaRestLwsConn.cpp`) delete the `if`/`else` with its `EndOpen`/`EndPreOpen` calls (keep `UncountTls();`): `Coexist.ShutdownStress` FAILS at "with Failure Shutdown" in round 0 — the connections still end once, from the callbacks `lws_context_destroy` makes inside `FService::Shutdown`, but as `Unknown` (pending) and a `Transport` end with `Failure None` (open); if LWS 3.0 makes no callback there for some state, "exactly one terminal event" fails too, and the report says which. Task 7's `Connect.Shutdown` fails the same way. Restore; PASSES.

- [ ] **Step 4: CLAUDE.md** — in the module table, insert after the row that starts with `` | `VaRestHttp` | `` (M4a's Task 24 rewrote it; keep its text):

```markdown
| `VaRestWebSocket` | Runtime | VaRest's own WebSocket client transport on the engine-bundled libwebsockets 3.0 (W1): `IVaRestSocketTransport` and its LWS implementation -- one context and service thread per process, one ordered event queue per connection, VaRest's handshake rules, in-place fragments, the close rules, TLS with the engine's roots and pins -- and `FVaRestFakeSocketTransport`; W2-W5 add the socket, stages, hub and Blueprint surface. Never depends on the engine's `WebSockets` module |
```

and, in "Conventions & Patterns", insert before the bullet `- **Push back with sources** when a premise is wrong.`:

```markdown
- **The WebSocket enums freeze with W5**: `EVaRestSocketFrameKind`, `EVaRestSocketCloseBy` and `EVaRestSocketFailure`
  (`/Script/VaRestWebSocket`, W1) and their enumerator names become pins and `FVaRestSocketClose` fields there. A rename
  needs a CoreRedirect in the same commit.
```

- [ ] **Step 5: The spec** — in `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md`, make these replacements (each `old` occurs once; `git diff` shows exactly them):

(a) §1.2, VaRest-rti — replace

```
  handshake (read from the disassembly; W0 did not provoke it). In a monolithic build VaRest's
  context shares the copy with `WebSockets`, `WebSocketServer`, `WebSocketNetworking` and
  `HttpNetworkReplayStreaming`, so W1 decides how VaRest's context creation stays out of that window.
```

with

```
  handshake (read from the disassembly; W0 did not provoke it). In a monolithic build VaRest's
  context shares the copy with `WebSockets`, `WebSocketServer`, `WebSocketNetworking` and
  `HttpNetworkReplayStreaming`. W1 (VaRest-rti, ruling Q2): VaRest makes one context per process and
  keeps it to engine exit; in a shared copy where any of those modules is linked, `VaRestWebSocket`'s
  `StartupModule` makes it, after loading the engine's `WebSockets` module, whose one context is made
  at its own startup (`WebSocketsModule.cpp:29-44`), so neither creation crosses the other's
  handshakes. In a shared copy where none of them is linked, nobody else makes a context, and VaRest
  makes its own at the first attempt, as in modular builds. A failed start is never retried (a retry
  could run under an engine handshake), and no VaRest context is made while a VaRest wss attempt
  that LWS has not destroyed exists. Residual risk: `WebSocketServer`, `WebSocketNetworking` and
  `HttpNetworkReplayStreaming` create contexts when game code starts them, and a module that starts
  before `VaRestWebSocket` (`PreDefault`, or earlier in `Default`) can already have an engine wss
  handshake in flight when VaRest's `StartupModule` runs; a VaRest or engine wss handshake in flight
  at such a creation can crash, and VaRest cannot see or serialise those creations.
```

(b) §1.2, VaRest-ek2 — replace

```
* **OpenSSL** follows the engine pattern exactly: static OpenSSL in our module plus an `SSL_CTX`
  from the engine `SSL` module (`LwsWebSocketsManager.cpp:205-225`), as `WebSockets` ships today.
```

with

```
* **OpenSSL** follows the engine pattern (`LwsWebSocketsManager.cpp:205-225`, §2.1): the `SSL`
  module's `SSL_CTX` where `InitializeSsl` succeeds (monolithic and merged builds,
  `SslManager.cpp:26-55`), else LWS's own with `LWS_SERVER_OPTION_DO_SSL_GLOBAL_INIT`, its roots
  added in `LOAD_EXTRA_CLIENT_VERIFY_CERTS` [tls.ssl_module_ctx, tls.load_extra_fired]. In the Linux
  editor the OpenSSL calls bind to the copy in the executable's global scope
  (`libUnrealEditor-DerivedDataCache.so` [env.openssl_version]), not one in our module.
```

(c) §1.2, VaRest-8zj item 4 — replace

```
* W0 verifies it: an engine `IWebSocket` and a VaRest socket open at once (ws and wss) in the editor
  and in a monolithic Development game, with both modules' logs still in place.
```

with

```
* Verified in the editor: by W0's probe [coexist.both_ws, coexist.both_wss,
  coexist.engine_lws_log_routed] and by W1 with `VaRestWebSocket` itself
  (`VaRest.Integration.WebSocket.Transport.Coexist.EngineWebSocket`); the monolithic Development
  game is W1b's run (VaRest-5ia).
```

(d) §2.1 — replace `* **One `lws_context` per process**, created lazily at the first attempt and destroyed at module` with `* **One `lws_context` per process**, created lazily at the first attempt (in a shared LWS copy, at module startup: §1.2) and destroyed at module`; replace `  (`LwsWebSocketsManager.cpp:196-203`). Different: `JUST_USE_RAW_ORIGIN` (T4);` with `  (`LwsWebSocketsManager.cpp:196-203`); with none configured, LWS 3.0 reads `http_proxy` from the environment itself, as the engine's `IWebSocket` does (ruling Q7). Different: `JUST_USE_RAW_ORIGIN` (T4);` (review spec M-2); replace

```
  kill can land up to about 2 s late; W0 measured only the 10 s and 100 ms waits, and W1 measures the
  chosen cap (VaRest-wcv). Where multithreading is unavailable it is polled from a core ticker
  instead (`:109-119`). LLM tag `VaRest`.
```

with

```
  kill can land up to about 2 s late; W0 measured only the 10 s and 100 ms waits. W1 (VaRest-wcv):
  the wait is capped at 100 ms while a kill is in flight, and a kill that has not landed after one
  timeout walk is armed again, because LWS's own stage timers can replace a pending timeout (never on
  every return: each `lws_set_timeout` restarts the timeout at the current second). Where multithreading is
  unavailable the forkable thread is ticked through `FSingleThreadRunnable` instead (`Fork.h:86-102`,
  as `:341-358`). At shutdown the running thread first writes the close frames already asked for, at
  most 0.25 s: the hub's 1001 "shutdown" (§1.4) is queued by the game instances' shutdown just
  before `OnEnginePreExit` (`GameEngine.cpp:1377`, `:1391`, `:1396`). LLM tag `VaRest_WebSocket`
  (W1; W5 parents it under the core's `VaRest`).
```

(review spec M-3, review conc M-3); and replace

```
  `LWS_CALLBACK_EVENT_WAIT_CANCELLED` (T13). Nothing but `lws_cancel_service` is called from
  another thread.
```

with

```
  `LWS_CALLBACK_EVENT_WAIT_CANCELLED` (T13). Nothing but `lws_cancel_service` is called from
  another thread. A send or a close queued before the open is written after it: LWS 3.0 drops a
  WRITEABLE request made during the handshake, so `CLIENT_ESTABLISHED` asks again (§2.4).
```

(e) §2.2 URL — replace `* **URL**: parsed with `lws_parse_uri`; schemes `ws`, `wss`, and outside Shipping `wss+insecure`` with `* **URL**: parsed by VaRest on the game thread (`lws_parse_uri` writes into its input and takes any scheme, `libwebsockets.h:5806-5819`); schemes `ws`, `wss`, and outside Shipping `wss+insecure``.

(f) §2.2 TLS, VaRest-sjj — replace

```
  the certificate carries that IP as a SAN and its CA is trusted [tls.hostname_ip_literal]; W1
  decides what VaRest does with such a URL and how §3.4 classifies the failure (VaRest-sjj).
```

with

```
  the certificate carries that IP as a SAN and its CA is trusted [tls.hostname_ip_literal]. W1
  (VaRest-sjj, ruling Q3): in `PERFORM_SERVER_CERT_VERIFICATION` that error (62) is cleared only
  for an IP-literal host, only at depth 0, and only when `X509_check_ip_asc` finds the IP among the
  leaf's SAN entries; every other error stands (the chain, the dates, another IP, a DNS-only SAN). A
  refused certificate is `EVaRestSocketFailure::TlsVerification`, the X509 error and depth in its
  detail, and terminal (§3.4).
```

(g) §2.2 upgrade refusal (review spec M-3: the clause that still says the status comes from `lws_http_client_http_response`, and the engine's own `Could not initialize connection`, which LWS never says) — replace

```
* **Upgrade refusal**: the status comes from the error string (T7: "not 101, was N";
  "unauthorized" = 401; "forbidden" = 403), and from `lws_http_client_http_response(wsi)` only when
  the error comes from the status-line parser (the strings `HS: ws upgrade …`)
  [hs.status_in_connection_error.refused]; never elsewhere: without a header table it dereferences
  NULL (`client.c.o` `lws_http_client_http_response` 0x1294-0x12b2), and W0 could not reach the
  DNS-failure case [hs.status_in_connection_error.dns_failure]. `Could not initialize connection`,
  `HS: ACCEPT missing`,
```

with

```
* **Upgrade refusal**: the status comes from the error string only (T7: "not 101, was N";
  "unauthorized" = 401; "forbidden" = 403) [hs.status_in_connection_error.refused]. W1 never calls
  `lws_http_client_http_response`: it returns a status only where the string already carries one,
  and without a header table it dereferences NULL (`client.c.o` `lws_http_client_http_response`
  0x1294-0x12b2); W0 could not reach the DNS-failure case [hs.status_in_connection_error.dns_failure].
  A connect `lws_client_connect_via_info` refuses outright (it returns NULL) is VaRest's own
  `ConnectFailed`. `HS: ACCEPT missing`,
```

and replace `  and a kill while pending "" [timeout.default_secs, hs.status_in_connection_error.killed_pending].` with

```
  and a kill while pending "" [timeout.default_secs, hs.status_in_connection_error.killed_pending].
  W1 tells an empty string apart by the time the attempt took: LWS cuts the TCP connect and the
  proxy exchange at a fixed 20 s whatever `timeout_secs` says (`lws_client_connect_2`), and the
  other stages at `timeout_secs` (review conc M-1, M-2).
```

(h) §2.2 redirects, VaRest-2rt — replace

```
  the first callback that can, `APPEND_HANDSHAKE_HEADER`, else `FILTER_PRE_ESTABLISH`; the W1 plan
  names the error kind.
```

with

```
  the first callback that can, `APPEND_HANDSHAKE_HEADER`, else `FILTER_PRE_ESTABLISH`, as
  `EVaRestSocketFailure::Redirect` with `HttpStatus` 0 (LWS follows the 3xx before any callback sees
  its status). A redirect of a wss attempt makes its second TLS handshake before the second
  `APPEND_HANDSHAKE_HEADER`, so a certificate check after the attempt's first one is refused as the
  redirect too.
```

(i) §2.4, VaRest-8zj item 5 — replace

```
* `lws_partial_buffered` is checked after each write; LWS's own buffering of a truncated write is
  counted.
```

with

```
* `lws_partial_buffered` is checked after each write; a write LWS still holds stays counted, whole,
  until LWS says it drained.
```

and replace

```
* **Buffered amount** (bytes accepted by `Send` and not yet handed to the socket) is exact and
  published to the game thread;
```

with

```
* **Buffered amount** (bytes accepted by `Send` and not yet handed to the socket) is exact to within
  one 64 KiB fragment (the last write LWS still holds, which `lws_partial_buffered` flags but does
  not count) and published to the game thread;
```

(j) §3.4, VaRest-sjj and VaRest-2rt — replace the row `| upgrade 3xx (redirect not followed or failed), other 4xx | stop | `Http` (`HttpStatus` set; a 403 message names the Origin sent) |` with

```
| upgrade 3xx (redirect not followed or failed: `Redirect`, `HttpStatus` 0 on LWS 3.0, §2.2), other 4xx | stop | `Http` (`HttpStatus` set for a 4xx; a 403 message names the Origin sent) |
```

and the row `| connect timeout, DNS/TCP/TLS failure, unknown connect error | reconnect | |` with

```
| TLS certificate refused (`TlsVerification`: the chain, the name or a pinned key; VaRest-sjj) | stop | `Connection` (not retryable, captive portals included: the game reconnects explicitly, ruling Q3; the message names the X509 error) |
| connect timeout, DNS/TCP failure, another TLS handshake failure, unknown connect error | reconnect | |
```

(k) §15, VaRest-8zj item (c) and the re-review nit — replace `| `ConnectTimeout`, `ReadyTimeout`, `CloseTimeout` | 10 s, = connect, 5 s | |` with `| `ConnectTimeout`, `ReadyTimeout`, `CloseTimeout` | 10 s, = connect, 5 s | `ConnectTimeout` at most 60 s, the context's `timeout_secs` (§2.2); a `CloseTimeout` above LWS's fixed 5 s close-ack acts as about 5 s, and an end by that timer is not clean (§2.5, T20) |`.

(l) §16.3, VaRest-8zj item (b) — replace `(`Tools/testserver/certs/`, test-only), reached with `bAllowSelfSignedTls` or `wss+insecure`.` with `(`Tools/testserver/certs/`, test-only). The runner's TLS URL is an IP literal; W1's tests reach it as `wss://localhost` (TCP to 127.0.0.1, the test CA trusted), as `wss://127.0.0.1` through VaRest's IP check (§2.2), and with `wss+insecure` or `bAllowSelfSignedTls`, which carry the same flags (§2.2).`

(m) §18, VaRest-8zj item 2 — replace

```
* **LWS 3.0 behaviour we inferred from 4.3** (§0.3 list). Mitigation: W0 probe before W1; the
  fallbacks are written down (protocol registration at context creation, string status parsing,
  1002 handling).
```

with

```
* **LWS 3.0 behaviour is measured on Linux only** (§0.3 T17-T29, the W0 passport). Mitigation: the
  release pass re-runs the probe on Win64 and Mac and W1's transport suite on devices (bead VaRest-b4d).
```

(n) D21, VaRest-8zj item 1 — replace `a one-time warning when the engine's Verbose setting meets query tokens` with `a one-time warning when the engine's Verbose setting meets query tokens in a build where both share one LWS copy (§1.2)`.

(o) D24, VaRest-8zj item (a) — replace the row `| D24 | Subprotocols via `local_protocol_name`, no ini step; fallback: register declared protocols at context creation | E17 removed | if W0 disproves both, code-only subprotocols need a declaration |` with `| D24 | Subprotocols via `local_protocol_name`, no ini step; W0 found no fallback needed (an unregistered selection stays bound, [hs.unregistered_selected_bound]) | E17 removed | none known |`.

(p) §22.2 A2, VaRest-8zj item 3 — replace `default row "reconnect"; W0 pins the LWS 3.0 error string |` with `default row "reconnect"; W0 pinned the LWS 3.0 error string, "read failed" [hs.no_upgrade_response] |`.

(q) §2.2 connect timeout, rulings I-4 and Q5 (review conc I-4, review spec M-1) — already in the spec: the plan revision applied it (the sentences after "so VaRest records which one fired." on LWS 3.0's synchronous `getaddrinfo` on the service thread, its first-address connect, and W1b B9). Check that they are there; change nothing.

(r) §2.5 our close, review conc I-5 — replace `  (T14; the kill's own latency is in §2.1).` with `  (T14; the kill's own latency is in §2.1). An end by LWS's own close-ack timer, after our close frame and with no close from the peer, is reported with `bWasClean = false`, whatever `CloseTimeout` says.`

(s) §6.3, review spec M-3 — replace `The transport reports an exact buffered amount (§2.4).` with `The transport reports the buffered amount, exact to within one 64 KiB fragment (§2.4).`

(t) §16.2, the transport suite's own refusals — replace `fragmented sends, allowlist, upgrade refusal statuses.` with `fragmented sends, allowlist, upgrade refusal statuses; W1 adds a cancelled attempt that sends nothing, a start that failed or was refused under a handshake in flight, and the close frames Shutdown writes first.`

- [ ] **Step 6: The whole suite and the server's own tests**

Run: `Tools/run_tests.sh VaRest` then `python3 -I Tools/testserver/selftest.py` and `python3 -I Tools/testserver/ws_selftest.py`
Expected: `0 failed`; `OK`; `OK`.

- [ ] **Step 7: The monolithic builds** (CLAUDE.md: "check it before a milestone lands"; spec §17 W1 exit "monolithic build")

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Shipping -project=/w/Unreal/VaRestHost/VaRestHost.uproject
```

Expected: both succeed with no warning from `Source/VaRestWebSocket`. Shipping compiles out the fake, the test hooks and `wss+insecure` (`PrepareTarget`'s `bShippingRules` defaults to `UE_BUILD_SHIPPING`). Where the game target links the engine's `WebSockets` module, `VaRestWebSocket` and it share one LWS copy and the link succeeds with one `libwebsockets.a` (review spec M-8: check, do not assume -- `strings -a /w/Unreal/VaRestHost/Binaries/Linux/VaRestHost | grep -c 'Lws(%ls): %ls'` counts the engine module's narrow log format, `LwsWebSocketsManager.cpp:417-421`; 0 means `WebSockets` is not linked and the copy is VaRest's alone; put the count in the task report). W1b's run exercises the startup policy.

- [ ] **Step 8: The per-module non-unity, no-PCH compile** (the M3 ruling: never `-DisableUnity`). First both Build.cs files must be clean: `git diff --quiet -- Source/VaRestWebSocket/VaRestWebSocket.Build.cs Source/VaRestTests/VaRestTests.Build.cs` exits 0 — otherwise another plan's edit is there: stop and report. In each, replace `PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;` with

```csharp
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
```

Dry-run first, so a mistake cannot start an engine rebuild:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject \
  -WriteOutdatedActions=/tmp/varest-w1a-actions.json
python3 -c "import json; a = json.load(open('/tmp/varest-w1a-actions.json')); print(len(a.get('Actions', a)))"
```

Expected: a few hundred actions at most, all under `Plugins/VaRest`. (More than a thousand, or an engine path: stop, undo the two edits, report.) Then build for real (the same command without `-WriteOutdatedActions`), expect success, and undo **only this step's lines** in each Build.cs — `NoPCHs` back to `UseExplicitOrSharedPCHs`, the `bUseUnity = false;` line deleted — never `git checkout --` on a whole file. `git diff --quiet` on both then exits 0. Rebuild once: `Tools/run_tests.sh VaRest.WebSocket.Module`.

- [ ] **Step 9: The MSVC sweep** (only clang builds here; clang's `-Wshadow` does not report a local that hides a member *function*, MSVC's C4458 does, review spec I-3) — read, by hand, every local, parameter and init-capture of each class's member functions against **both** its data members **and** its member functions:
  - `FVaRestSocketFrame` — functions `Text`, `Binary`, `FromStorage`, `IsValid`, `GetPayload`; data `Kind`, `Storage`, `Offset`, `Length`.
  - `FVaRestFakeSocketAttempt` — functions `Open`, `Message`, `Text`, `Pong`, `ServerClose`, `Drop`, `Fail`, `End`, `Flush`, `GetSent`, `GetCloseCode`, `GetCloseReason`, `GetConsumed`, `IsOpen`, `HasEnded`, `Push`, `OnSend`, `OnClose`, `OnConsumed`, `OnReleased`; data `Index`, `Request`, `Link`, `ConsumerDepth`, `bOpen`, `bCloseRequested`, `CloseCode`, `CloseReason`, `bReleased`, `ConsumedBytes`, `Sent`, `bEchoClose`.
  - `FSocketLink` — functions `Push`, `Notify`, `PopAll`, `NoteBuffered`, `ReleaseBuffered`; data `Queue`, `OnEvents`, `BufferedBytes`, `PendingInboundBytes`, `bEnded`, `bReleased`, `bCloseRequested`, `bTerminalQueued`, `bHighWaterReached`.
  - `FService` — functions `Start`, `Shutdown`, `IsRunning`, `IsShuttingDown`, `Enqueue`, `GetContext`, `GetOptions`, `GetCertificates`, `GetCounters`, `Adopt`, `Find`, `ComputeWaitMs`, `Callback`, `Run`, `Stop`, `Tick`, `ServiceOnce`, `FlushCloses`, `DrainCommands`, `CheckDeadlines`, `EraseDestroyed`, `Handle`, `FindForCallback`, `LoadClientCertificates`, `AddExtraTrusted`, `ReleaseSsl`; data `Options`, `Context`, `Protocols`, `Thread`, `bStopping`, `bAccepting`, `bShuttingDown`, `bStartFailed`, `EnqueueLock`, `Commands`, `Connections`, `Live`, `Certificates`, `SslManager`, `ProvidedSslCtx`, `bSslInitialized`, `ProxyUtf8`, `Counters`.
  - `FConn` — functions `~FConn`, `Prepare`, `GetLabel`, `NeedsTlsCount`, `MarkTlsCounted`, `GetId`, `Start`, `OnCallback`, `CheckDeadlines`, `NextDeadline`, `WantsShortWait`, `IsDestroyed`, `EndForShutdown`, `Send`, `Close`, `Release`, `Find`, `OnAppendHeaders`, `OnFilterPreEstablish`, `OnEstablished`, `OnWriteable`, `OnPeerClose`, `OnVerifyServerCertificate`, `OnReceive`, `OnConnectionError`, `OnEnded`, `OnDestroyed`, `Kill`, `Fail`, `ReleaseHeld`, `DropOutbox`, `EndPreOpen`, `EndOpen`, `UncountTls`, `UncountClose`; data `Id`, `Link`, `Request`, `Target`, `Label`, `PendingFailure`, `PendingDetail`, the `*Utf8` arrays, `Wsi`, `Owner`, `State`, `bDestroyed`, `bTlsCounted`, `StartedAt`, `AppendCalls`, `SelectedProtocol`, `bKillIssued`, `KillAt`, `KillArmedAt`, `Outbox`, `HeldBytes`, `NextFragment`, `bCloseQueued`, `bCloseSent`, `bCloseCounted`, `CloseSentAt`, `CloseCode`, `CloseReason`, `CloseBy`, `CloseFailure`, `CloseDeadline`, `ConnectDeadline`, `bPeerClosed`, `Peer`, `Assembly`, `AssemblyKind`, `bAssembling`. (Task 8's `FragmentStart`/`FragmentSize` were `Start`/`Size`: the case this list exists for.)
  - `FTransport` — functions `Connect`, `EnsureStarted`, `Shutdown`, `ResetAfterFork`, `GetCounters`; data `Options`, `Service`, `bShutDown`, `NextId`. The handle — functions `PopEvents`, `Send`, `Close`, `Consumed`, `GetBufferedAmount`, `GetPendingInboundBytes`, `HasEnded`, `Post`; data `Service`, `Id`, `Link`, `Label`.
  - `FEvents` — functions `WaitFor`, `WaitType`, `WaitEnd`, `WaitMessages`, `Pull`, `Find`, `GetEnd`, `GetMessages`, `Count`, `GetAll`, `Describe`; data `Seen`, `Connection`, `bAcknowledge`. And the free functions of `VaRest::WsTest` against locals in that namespace (C4459: `Request`, `Visit`, `Utf8`; review spec M-10).

  Lambda init-captures must not reuse an enclosing name (`[Moved = MoveTemp(Frame)]` is fine; `[Frame = MoveTemp(Frame)]` is C4456/C4458, errors in this build, `CppCompileWarnings.cs:88-90`). Rename what is real and rebuild.

- [ ] **Step 10: Beads** — record what W1a decided and measured (fill the `<...>` from the task reports: the commits, Task 7's `LocalhostAddress` line, Task 10's replay outcome, Task 11's kill latencies):

```bash
bd close VaRest-sjj --reason "W1a task 13 (<hash>): ruling Q3 -- VaRest clears X509 error 62 only for an IP-literal wss host, at depth 0, when X509_check_ip_asc finds the IP in the leaf's SAN; every other error stands; refusals are EVaRestSocketFailure::TlsVerification with the X509 error, terminal; both legs green: VaRest.WebSocket.Lws.TlsIpCheck (OpenSSL's verification on in-memory leaves: right IP, wrong IP, DNS-only SAN, untrusted) and Tls.IpLiteral/Tls.WrongIp/Tls.Refused through the verify callback; spec 2.2 and 3.4 amended in task 14"
bd close VaRest-wcv --reason "W1a task 11 (<hash>): kills report at VaRest's deadline; the wait is capped at 100 ms while a kill is in flight; a kill not landed 1.25 s after it was armed is re-armed (LWS's stage timers can overwrite it; review conc I-1), never every pass (re-review N-1: Timeout.KillLatencyBusy, longest <ms> ms beside a 32 MiB send); an ended attempt vetoes APPEND_HANDSHAKE_HEADER and its certificate check (Close.CancelSendsNothing); kill to WSI_DESTROY longest <ms> ms with the 100 ms cap, <ms> ms with 1 s (Timeout.KillLatency)"
bd close VaRest-ek2 --reason "W1a task 14 (<hash>): spec 1.2's OpenSSL bullet rewritten (SSL-module context in monolithic/merged builds, LWS's own elsewhere; OpenSSL binds globally in the Linux editor)"
bd close VaRest-8zj --reason "W1a task 14 (<hash>): D21, 18, 22.2 A2, 1.2, 2.4 (buffered amount within one fragment), D24, 16.3, 15 (ConnectTimeout max, CloseTimeout), 2.1's cross-reference to 2.4"
bd update VaRest-2rt --notes "W1a task 10 (<hash>): refused at the second APPEND_HANDSHAKE_HEADER (no second request), at a certificate check after the attempt's first APPEND (wss redirects re-handshake first), and in FILTER_PRE_ESTABLISH; Failure Redirect, HttpStatus 0; spec 2.2/3.4 amended. Probe.VaRest.Lws.Handshake.RedirectReplay: <outcome line>. Left for W1b: count APPEND_HANDSHAKE_HEADER under a configured proxy (M-7)."
bd update VaRest-rti --notes "W1a plan decision 8 (<hash>): one context per process; in a shared copy with another LWS module linked, made at VaRestWebSocket's StartupModule after loading WebSockets; a failed start is latched, never retried (review conc I-2, Service.StartFailureLatched, Connect.StartRefusals); no VaRest context is made while a VaRest wss attempt LWS has not destroyed exists (module-wide count, review conc I-3, Service.RefusesCreateUnderHandshake, Connect.TlsHandshakeCounted); residual (WebSocketServer, WebSocketNetworking, HttpNetworkReplayStreaming contexts made later; a module starting before VaRestWebSocket with an engine wss handshake in flight) documented in spec 1.2, not closed. Proof in a monolithic game: W1b's run (VaRest-5ia)."
bd update VaRest-5ia --notes "W1a task 14 (<hash>): editor coexistence with VaRestWebSocket itself, ws and wss, symbol owner re-checked (VaRestWebSocket's own lws_create_context), D30 level kept; the monolithic Development run (cooked host) is W1b's"
bd update VaRest-x2v --notes "W1a task 14: Coexist.ShutdownStress (10 rounds: pending, writing, TLS, closing connections, then Shutdown) <passed 3/3 runs | what failed>; the probe's own exit not reproduced by VaRest's teardown. First suspect if an exit flake recurs (review spec M-7): Connect.Default leaves the default transport's service thread running at process exit, since an Automation RunTests ...; Quit run calls neither OnEnginePreExit nor ShutdownModule"
bd update VaRest-81s --notes "W1a done (<first hash>..<last hash>): the transport's data path, TLS, redirects, connect/close deadlines; W1b (ping/RTT, flow control, idle deadline and suspension, auto-replies and SO_SNDBUF, proxy, monolithic run) next: docs/superpowers/plans/2026-10-22-varest2-ws-w1b-service-policies.md"
```

Ruling Q5 and review spec M-1: the address-fallback bead exists (VaRest-gi0, P1; W1b B9) -- never create a second one. Record the probe's line and the synchronous-DNS finding on it, whatever the line says:

```bash
bd update VaRest-gi0 --notes "W1a (<hash>): Probe.VaRest.Lws.Connect.LocalhostAddress: <line>. Spec 2.2 now documents LWS 3.0's first-address connect and its synchronous getaddrinfo on the service thread (review conc I-4: one slow name stalls every connection's I/O, commands and deadlines, and the exit join; a 3xx Location is resolved the same way before VaRest can refuse it). B9: resolve on UE::Tasks, pass the numeric address as address and the name as host (the TLS name follows host, tls.trusted_ca_verified), try the next address after a failure; test with a resolver hook that sleeps 3 s on one connection while another's CloseTimeout = 1 still ends in [0.9, 1.6) s."
```

- [ ] **Step 11: Commit**

```bash
git add Source/VaRestTests/Private/WebSocket/VaRestWsCoexistTest.cpp CLAUDE.md docs/superpowers/specs/2026-10-20-varest2-websocket-design.md
git commit -m "ws: W1a closes -- editor coexistence with the engine IWebSocket (ws, wss, own LWS copy, logs), a shutdown stress; spec 1.2/2.1/2.2/2.4/2.5/3.4/6.3/15/16.2/16.3/18/D21/D24/22.2 from W1a's decisions and the plan reviews (VaRest-rti, sjj, 2rt, ek2, 8zj, gi0); CLAUDE.md module row and freeze line

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

## After W1a

What W1b and the socket layer take from here, so their plans start from code that exists:

- **W1b** (`docs/superpowers/plans/2026-10-22-varest2-ws-w1b-service-policies.md`) adds to `FVaRestSocketConnectRequest` (`PingInterval`/`PingTimeout`, `MaxPendingInboundBytes`, `AutoReplies`), to `FConn` (a ping deadline and a `Pong` producer in `CheckDeadlines` and `OnCallback`'s `CLIENT_RECEIVE_PONG`; a pause in `OnReceive` above `MaxPendingInboundBytes` and a resume behind `IVaRestSocketConnection::Consumed`, posted as a command; an idle deadline; a reply lane checked in `OnWriteable` when `NextFragment == 0`; `SO_SNDBUF` in `OnEstablished`), to `IVaRestSocketConnection` (`SetIdleDeadline`, one virtual), to `FVaRestSocketEnd` (`bWhileReadPaused`), to `FVaRestSocketEvent` (`bAutoReplied`) and to the test server (a CONNECT proxy). B9 (VaRest-gi0) resolves the host on `UE::Tasks` and passes the address, with the next address tried after a failure (ruling Q5, review conc I-4). `EVaRestSocketFailure::HeartbeatTimeout`/`IdleTimeout` and `EVaRestSocketCloseBy::Heartbeat` exist already, as do the throttled kill re-arm (a heartbeat kill uses it; never re-arm every pass, re-review N-1) and `TlsHandshakesInFlight()` (every new context checks it).
- **W2** builds `FVaRestSocket` on `IVaRestSocketTransport` and tests on `FVaRestFakeSocketTransport`: one attempt per `Connect`, the generation guard over each attempt's events, classification on (`End.By`, `End.Code`, `End.Failure`, `End.HttpStatus`) — spec §3.4 as Task 14 amended it (`TlsVerification` terminal, captive portals included -- ruling Q3; `Redirect` with status 0; LWS 3.0's 1002 rewrite, VaRest-diy; an empty `CONNECTION_ERROR` before 19.5 s is `Unknown`, reconnect). `Close()` takes effect at the call (spec §3.1): the transport's `Closed` for an app close arrives later and is W2's to ignore.
- **W3**'s pipe consumes `PopEvents` on a `UE::Tasks::FPipe` woken by `OnEvents` and acknowledges with `Consumed` after delivery; W3's off-thread sends hand `FVaRestSocketFrame`s (moved, never shared) to the game-thread flush.
- **W5** parents the `VaRest_WebSocket` LLM tag under M4a's `VaRest`, maps `FVaRestSocketEnd` onto `FVaRestSocketClose` and the trace's `FVaRestTraceSocketClose`, and fills the request from `UVaRestSocketSettings` (`bAllowSelfSignedTls`, timeouts, limits).
