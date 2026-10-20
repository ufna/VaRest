# VaRest 2 — WebSocket design

Status: revision 2, 2026-10-20. Revision 1 followed the adversarial review
`.superpowers/sdd/ws-spec-review.md` and the controller's rulings R1–R5; revision 2 adds the hooks
Socket.IO needs (§22, after `.superpowers/sdd/sio-spec-review.md` and Socket.IO rulings R1–R4) and fixes the
rev 1 passages they contradict in place (marked "rev 2"). Sub-project 2 of 4 (core spec §0.3).
Target: Unreal Engine 5.8.1, branch `v2`. Builds on `2026-10-18-varest2-core-design.md` ("the core");
§ numbers prefixed with *core* refer to it. Appendix A maps every review item to its disposition.

---

## 0. Context

### 0.1 Intent

A game that talks to a WebSocket backend should get, from one asset entry or one C++ statement:
connect, reconnect with backoff, ping-pong and dead-link detection with no setup, a send queue
across reconnects, messages routed by a JSON field to typed handlers, requests and replies in both
directions, auth from the same session as the HTTP API, BP nodes a designer can wire without C++,
and the same logging, redaction and Network Inspector visibility as HTTP. Socket.IO (sub-project 3)
is built on this layer, so it must expose the hooks Engine.IO v4 / Socket.IO v5 need (§8).

What success looks like:

* Designer: add a socket to `DA_GameApi`, declare its messages, then use *Connect Socket*,
  *On chat_message* (typed) and *Emit send_chat* (typed). No C++.
* C++: one statement connects and routes (§9). No reconnect timers, no keepalive counters, no
  `FJsonSerializer` on the game thread.
* Bytes are delivered exactly as sent and in the order sent; a dropped link is noticed (protocol
  ping, on by default), recovered (backoff + jitter) and reported (events, state, logs,
  Inspector); a refusal that will never succeed (403, policy) stops retrying and says why.

### 0.2 Why VaRest does not use the engine's `IWebSocket` (ruling R1)

The review confirmed defects of the engine transport that are silent data corruption or crashes —
not acceptable for the plugin meant to be the standard. Paths are relative to
`Engine/Source/Runtime/Online/WebSockets/`; paths starting with `Runtime/` or `ThirdParty/` are
relative to `Engine/Source/`, and `Config/` to `Engine/`.

| # | Engine behaviour (UE 5.8.1) | Source | Consequence |
| --- | --- | --- | --- |
| E9 | Text is converted from UTF-8 **per receive callback**, which fires at every frame end and every 64 KiB | `Private/Lws/LwsWebSocket.cpp:309-330`; `Private/Lws/LwsWebSocketsManager.cpp:166` | a code point split by a fragment boundary becomes U+FFFD — valid UTF-8, so no parser notices |
| E12 | The game thread drains all text, then raw, then binary queues while the LWS thread keeps filling them | `LwsWebSocket.cpp:599-620`, `:326`, `:335` | text/binary order is lost in both directions |
| E25 | `GameThreadTick` range-fors over `Sockets` and broadcasts events inside it; `Connect()` appends to that array | `LwsWebSocketsManager.cpp:557-560`, `:537`; `LwsWebSocket.cpp:141-142` | connecting from a message handler is UB in Shipping |
| E7 | A non-allowlisted URL fails synchronously inside `Connect()` | `LwsWebSocket.cpp:112-122` | events re-enter the caller |
| E27 | An empty close frame is read as a two-byte code anyway | `LwsWebSocket.cpp:351` vs `:357` | garbage close codes (RFC 6455 says 1005) |
| E14 | Our own close is always reported as 1000/1009; drops can arrive as 1000 unclean | `LwsWebSocket.cpp:392-476` | close classification cannot trust the code |
| E26 | `OnConnected` fires only if the polled state is `Connected` at a tick; messages still drain | `LwsWebSocket.cpp:586-607` | open can be skipped |
| E28 | The peer close reason is widened as ANSI, not UTF-8 | `LwsWebSocket.cpp:359` | mangled reasons |
| E8 | One-producer send queue, unbounded, no drain signal | `Private/Lws/LwsWebSocket.h:353`; `LwsWebSocket.cpp:170-182` | no backpressure |
| E4 | Headers fixed at creation; no per-socket ping, no selected subprotocol, no upgrade status | `Public/IWebSocket.h:6-102`; `LwsWebSocket.cpp:83-95` | |
| E16 | `PingPongInterval` is context-wide, read once, PONG swallowed | `LwsWebSocketsManager.cpp:192-194`, `:438-439` | no per-socket RTT or timeout |
| E17 | Server-selected subprotocols must be registered in `[WebSockets] WebSocketsProtocols` at startup | `Private/WebSocketsModule.cpp:35-44`; `Config/BaseEngine.ini:73-79` | manual ini step |
| E19 | Upgrade headers are appended with `Strncpy(…, Length)`, then the pointer advances by the full length | `LwsWebSocket.cpp:537-547` | buffer overrun on long headers |
| E20 | `Origin` is the bare host, which LWS prefixes with `http://` | `LwsWebSocket.cpp:804`; `LwsWebSocketsManager.cpp:184` (no raw-origin option) | Origin-checking servers refuse |

VaRest therefore ships its **own transport on the libwebsockets the engine already bundles** (§2).
The engine `IWebSocket` backend is **not implemented in 2.0**; the transport seam
(`IVaRestSocketTransport`) keeps one possible later without API change.

### 0.3 What libwebsockets 3.0 gives us (verified in the bundled headers)

Headers: `ThirdParty/libWebSockets/libwebsockets/include/<Platform>/libwebsockets.h` (line numbers
from the `Unix/x86_64-unknown-linux-gnu` copy).

| # | Fact | Source |
| --- | --- | --- |
| T1 | One library for all five targets: version 3.0.0, TLS on, extensions compiled **out** (`LWS_WITHOUT_EXTENSIONS`, so no permessage-deflate), one service thread per context (`LWS_MAX_SMP 1`), client enabled | `lws_config.h:34`, `:47`, `:56`, `:92`, `:135` (same in `Win64/VS2015`, `Mac`, `IOS`, `Android/*`, `Unix/*`) |
| T2 | Prebuilt libs: `lib/{Win64/VS2015,Win64/arm64,Mac,IOS,Android/{ARM64,x64,x86},Unix/{x86_64,aarch64}}/{Debug,Release}`; linked through module `libWebSockets` | `ThirdParty/libWebSockets/libWebSockets.Build.cs:7-18`, `:78-111` |
| T3 | Client connect info: `protocol` (subprotocols sent), **`local_protocol_name`** (binds the connection to our handler independently of what is sent), `origin`, `host`, `ssl_connection` flags, `pwsi`, `userdata` | `libwebsockets.h:3383-3477` |
| T4 | `LWS_SERVER_OPTION_JUST_USE_RAW_ORIGIN`: send `.origin` as given, not `http://` + it | `libwebsockets.h:2618-2623` |
| T5 | `LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER`: `len` is the **remaining** header-buffer space | `libwebsockets.h:1316-1340` |
| T6 | `LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH`: last point where response headers can be read (`lws_hdr_copy`, `WSI_TOKEN_PROTOCOL`) | `libwebsockets.h:1301-1307`, `:3985`, `:4126-4139` |
| T7 | `LWS_CALLBACK_CLIENT_CONNECTION_ERROR` gives an error string; documented strings include `HS: ws upgrade response not 101, was <code>`, `HS: ws upgrade forbidden`; the 3.0 library contains `HS: ws upgrade unauthorized`, `HS: ws upgrade not 101, was %u`. `lws_http_client_http_response(wsi)` returns the last HTTP status but is documented for `ESTABLISHED_CLIENT_HTTP` only | `libwebsockets.h:1255-1299`, `:3596-3607`; `strings` of `lib/Unix/x86_64-unknown-linux-gnu/Release/libwebsockets.a` |
| T8 | Frame data: `LWS_CALLBACK_CLIENT_RECEIVE` with `lws_is_first_fragment`, `lws_is_final_fragment`, `lws_frame_is_binary`, `lws_remaining_packet_payload` | `libwebsockets.h:1345`, `:6003-6049`, `:5214-5232` |
| T9 | Per-connection **PING** (`LWS_WRITE_PING`) and `LWS_CALLBACK_CLIENT_RECEIVE_PONG` | `libwebsockets.h:4799-4800`, `:1349` |
| T10 | Fragmented sends: `LWS_WRITE_NO_FIN` + `LWS_WRITE_CONTINUATION`; LWS buffers a truncated write itself and suppresses WRITEABLE until it drains; `lws_partial_buffered`, `lws_send_pipe_choked` | `libwebsockets.h:4790-4827`, `:4910-4932`, `:5989-5995`, `:6020-6037` |
| T11 | Receive flow control: `lws_rx_flow_control(wsi, 0/1)` | `libwebsockets.h:5185-5197` |
| T12 | Peer close: `LWS_CALLBACK_WS_PEER_INITIATED_CLOSE` gives the optional code + reason; return 0 → LWS echoes the close (RFC 6455 §5.5.1). `lws_close_reason` sets ours. `LWS_CALLBACK_CLIENT_CLOSED` ends a client session | `libwebsockets.h:1217-1225`, `:844`, `:1313` |
| T13 | Thread model: only `lws_cancel_service` may be called from another thread; it raises `LWS_CALLBACK_EVENT_WAIT_CANCELLED` inside the service loop | `libwebsockets.h:3704-3716`, `:1551-1556` |
| T14 | Per-wsi timers (`lws_set_timer_usecs` → `LWS_CALLBACK_TIMER`, supported by the default poll loop) and forced close (`lws_set_timeout(…, LWS_TO_KILL_ASYNC)`) | `libwebsockets.h:4705-4732`, `:1542-1549`, `:4674-4699` |
| T15 | TLS via a provided `SSL_CTX` (`provided_client_ssl_ctx`), `LWS_SERVER_OPTION_DISABLE_OS_CA_CERTS`, verify callbacks `LWS_CALLBACK_OPENSSL_LOAD_EXTRA_CLIENT_VERIFY_CERTS` / `…PERFORM_SERVER_CERT_VERIFICATION`; client proxy `http_proxy_address` | `libwebsockets.h:2765`, `:2582`, `:961`, `:1015`, `:2734` |
| T16 | `lws_set_log_level` and `lws_set_allocator` are process-global | `libwebsockets.h:407-418`, `:5979` |

Inferred from LWS 4.3 source (not available for 3.0) and pinned by the W0 probe (§16.4): whether a
server-selected subprotocol unknown to the context is accepted when `local_protocol_name` pre-binds
the connection; whether `lws_http_client_http_response` is valid inside `CONNECTION_ERROR`; which
received close codes LWS 3.0 rewrites; whether LWS waits for the close echo; LWS's default
`timeout_secs`; whether LWS answers server PINGs (RFC 6455 §5.5.2 requires it).

### 0.4 Clarification to core §0.3

Core §0.3 says there is "nothing for WinHttp" in the engine. True, and moot: no VaRest target used
WinHttp (`WebSockets.Build.cs:7-18`, `:52-89`), and VaRest now owns its per-connection ping (§5).

### 0.5 Non-goals

* An engine-`IWebSocket` backend in 2.0 (§0.2). The seam stays.
* A WebSocket **server**, or a NetDriver over WebSocket (DwtWebSoN does both).
* Compression: the bundled LWS has extensions compiled out (T1).
* Persisting the outgoing queue across app restarts; at-least-once delivery after reconnect
  (needs server cooperation; apps build it with requests, §7.5).
* Socket.IO, STOMP, graphql-ws, Phoenix. Socket.IO is sub-project 3 and **WebSocket-only Engine.IO**
  (socket.io-client's `transports: ['websocket']`: no long-polling, no upgrade). The others fit
  the codec and stage hooks (§8) but do not ship.
* Handler callbacks on worker threads.

---

## 1. Modules and object model

### 1.1 Module

```
VaRestJson          FVaRestJson, struct <-> JSON
   ^
VaRestHttp          + one abstract class UVaRestApiSocketConfig (asset entries, §11)
   ^
VaRestWebSocket     Runtime   transport on libwebsockets, FVaRestSocket, state machine, codec, stages,
                              UVaRestSocketHub, UVaRestSocketClient, socket config classes, async BP actions
   ^
VaRestSocketIO      (sub-project 3)
VaRestEditor        + K2 nodes: On <message>, Emit <message>, Socket Request, Reply (§10)
```

`VaRestWebSocket`: `Type Runtime`, `LoadingPhase Default`, `PlatformAllowList`
Win64/Mac/Linux/Android/IOS. Dependencies mirror the engine's own WebSockets module
(`WebSockets.Build.cs:107-120`): `AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL",
"libWebSockets", "zlib")` and private `SSL` (platform root certificates, pinning), plus `Core`,
`CoreUObject`, `Engine`, `HTTP` (proxy settings), `VaRestJson`, `VaRestHttp`. The engine module
`WebSockets` is **not** a dependency.

**One plugin (ruling R4).** UBT builds every runtime module of an enabled plugin that is valid for
the target (`Programs/UnrealBuildTool/Configuration/UEBuildTarget.cs:5935-5951`), so an HTTP-only
game links `VaRestWebSocket` too. The module split is for dependency hygiene, not link size. Cost:
libwebsockets is ≈ 140–165 KB of code (`size` of the Release archive: 141 KB text on Linux x86-64,
162 KB on Android ARM64); in a monolithic game it is linked once even if the engine's WebSockets
module is also present. OpenSSL and zlib are already linked on Win64/Linux/Android by the engine's
HTTP module (`Runtime/Online/HTTP/HTTP.Build.cs:88-96`, `:183-186`); on Mac/iOS they may add a few MB
unless something else links them (archives: `OpenSSL/1.1.1t/lib/Mac/libcrypto.a` 8.3 MB,
`libssl.a` 1.6 MB, dead-stripped at link). W6 measures the packaged delta per platform.

### 1.2 Two copies of libwebsockets in one process

In modular (editor) builds both the engine's `WebSockets` module and `VaRestWebSocket` link the
static library, so there are two copies. Why it is safe:

* **It is the engine's own practice.** Three more engine modules link the same archive and create
  their own LWS contexts: `WebSocketServer` (`Runtime/Online/WebSocketServer/WebSocketServer.Build.cs:27-28`,
  context at `Private/LwsWebSocketServer.cpp:63-65`), the `WebSocketNetworking` plugin
  (`Plugins/Experimental/WebSocketNetworking/Source/WebSocketNetworking/WebSocketNetworking.Build.cs:25-27`,
  client context at `Private/WebSocket.cpp:47-74`) and `HttpNetworkReplayStreaming`
  (`Runtime/NetworkReplayStreaming/HttpNetworkReplayStreaming/HttpNetworkReplayStreaming.Build.cs:23-27`).
* **Contexts are independent.** All connection state lives in the `lws_context`; VaRest creates
  its own and never touches the engine's.
* **Linux interposition is benign.** `libUnrealEditor-WebSockets.so` exports 185 `lws_*` functions
  (`nm -D --defined-only`, verified on this host), so ELF resolves both modules' calls to whichever
  copy loaded first. Both copies come from the identical archive, so behaviour is identical.
  Windows DLLs and Mac two-level-namespace dylibs keep separate copies. Monolithic builds have one.
* **The only shared state is process-global** (T16): log level/emitter and allocator. VaRest never
  calls `lws_set_allocator`, and calls `lws_set_log_level(LLL_ERR | LLL_WARN, VaRestLwsLog)` only if
  the engine's `WebSockets` module is not loaded; otherwise the engine's routing to `LogWebSockets`
  (`LwsWebSocketsManager.cpp:174-176`) stays. Never both, so no one's logs are hijacked.
* **OpenSSL** follows the engine pattern exactly: static OpenSSL in our module plus an `SSL_CTX`
  from the engine `SSL` module (`LwsWebSocketsManager.cpp:205-225`), as `WebSockets` ships today.
* W0 verifies it: an engine `IWebSocket` and a VaRest socket open at once (ws and wss) in the editor
  and in a monolithic Development game, with both modules' logs still in place.

### 1.3 Object model

| Type | Role |
| --- | --- |
| `FVaRestSocket` | C++ handle (thread-safe `TSharedRef` to `FVaRestSocketState`). Configure, connect, send, subscribe. |
| `FVaRestSocketWeak` | Weak handle for lambda captures (avoids state → handler → handle cycles). |
| `FVaRestSocketChannel` | Router + readiness gate + backlog + pending requests + events. A socket has one default channel; Socket.IO maps namespaces to channels (§8). The socket's own `On`/`Emit`/`Request`/`OnReady` act on the default channel. |
| `IVaRestSocketTransport` / `IVaRestSocketConnection` | Seam: one connection per attempt. Shipped: `FVaRestLwsTransport` (§2). Tests: `FVaRestFakeSocketTransport`. |
| `IVaRestSocketCodec` | Thread-safe: frames ↔ messages (channel, route, data, id, kind, attachments). Default: JSON envelope codec (§7.3), pure. Rev 2 (§22 SW-2): a codec may create a stateful per-connection decoder, used only on the socket's pipe. |
| `IVaRestSocketStage` | Stateful, game-thread middleware: app heartbeat, readiness, protocol control packets (§8). |
| `UVaRestSocketClient` | BP `UObject` around one `FVaRestSocket` (§10). |
| `UVaRestSocketHub` | `UGameInstanceSubsystem` holding named sockets (§1.4). |
| `FVaRestSocketMessage`, `FVaRestSocketOpen`, `FVaRestSocketClose` | BP structs; payloads shared, copies cheap. |

### 1.4 Ownership, lifetime, PIE

* **Unnamed C++ sockets are owned by their handles** (`Hub->Make(Url)`): the last handle closes the
  socket with 1001 "released". Handles use thread-safe reference counting; the state's destructor is
  posted to the game thread, wherever the last handle drops.
* **Named sockets are owned by the hub** (asset entries, `Hub->Make(Name, Url)`) until
  `Hub->Release(Name)` or hub teardown. `.Owner(UObject*)` closes an unnamed socket when the object
  dies; on a named socket it is refused (`ensure`), because every `Socket(GameApi, "Chat")` caller
  shares it.
* **BP `Make Socket`** with a name is a named socket; without one it is owned by its world-context
  object (closed when that object is destroyed), so an unstored return value is neither collected
  under the designer nor leaked.
* `UVaRestSocketHub` is a game-instance subsystem: each PIE client has its own sockets, tokens and
  queues. Editor utilities use `UVaRestSocketHub::GetForEditor()` explicitly (engine-scoped, the
  core §5.2 fallback); there is no static `FVaRestSocket::Make`, so game code cannot reach the
  engine scope by accident.
* Each socket hub pairs with the `UVaRestHub` of its scope (connectivity, the asset's session,
  trace sinks; `Collection.InitializeDependency`). Subsystems are deinitialized in map order, not
  reverse dependency order (`Runtime/Engine/Private/Subsystems/SubsystemCollection.cpp:318-327`), so
  the socket hub closes its sockets on `UVaRestHub::OnDeinitializing` (§19); its own `Deinitialize`
  repeats that without calling into `UVaRestHub`. Closing: 1001 "shutdown", pending requests fail
  `Canceled`, attempts abandoned.

---

## 2. Transport: `FVaRestLwsTransport`

Mirrors the engine's `FLwsWebSocketsManager` glue where it is right (context options, TLS, proxy,
allowlist, thread), and replaces the parts that are wrong (§0.2).

### 2.1 Context and service thread

* **One `lws_context` per process**, created lazily at the first attempt and destroyed at module
  shutdown or `FCoreDelegates::OnEnginePreExit` (`Runtime/Core/Public/Misc/CoreDelegates.h:261-262`)
  — `ShutdownModule` does not run in automation runs. Options as the engine
  (`LwsWebSocketsManager.cpp:178-225`): `CONTEXT_PORT_NO_LISTEN`,
  `PEER_CERT_NOT_REQUIRED | DISABLE_OS_CA_CERTS`, `SSL_CTX` from
  `ISslManager::CreateSslContext` (`Runtime/Online/SSL/Public/Interfaces/ISslManager.h:64-81`),
  `max_http_header_data2 = pt_serv_buf_size = 32 KiB`, `http_proxy_address` from
  `FPlatformHttp::GetConfiguredProxyAddress()` (`LwsWebSocketsManager.cpp:196-203`). Different:
  `JUST_USE_RAW_ORIGIN` (T4); `ws_ping_pong_interval = 0` (our ping is per connection, §5);
  one protocol entry `varest` with `rx_buffer_size` 64 KiB.
* **One service thread** (`FForkProcessHelper::CreateForkableThread`, 128 KiB stack, BelowNormal,
  as `LwsWebSocketsManager.cpp:239-254`; recreated after a fork via
  `FCoreDelegates::OnChildEndFramePostFork`, as `:261-279`). It loops `lws_service(ctx, NextDeadline)`
  and checks per-connection deadlines after each return. Where multithreading is unavailable it is
  polled from a core ticker instead (`:109-119`). LLM tag `VaRest`.
* **Commands** (connect, send, close, pause/resume reading) go from the game thread into an MPSC
  queue followed by `lws_cancel_service`; the service thread drains them in
  `LWS_CALLBACK_EVENT_WAIT_CANCELLED` (T13). Nothing but `lws_cancel_service` is called from
  another thread.
* **Events** go from the service thread into **one ordered queue per socket**: `Open`, `Message`,
  `Pong`, `Drained`, `Closed`, `Error`. A queue item carries type, frame kind, a final flag and
  bytes; in 2.0 the transport queues only complete messages (`bFinal = true`), so a later streaming
  mode needs no format change. Everything the socket learns arrives through this one queue, in
  wire order (§7.1).

### 2.2 Connect

Per attempt, from a `Connect` command:

* **Allowlist**: `UE::Core::FURLRequestFilter(TEXT("Online.HttpManager"), GEngineIni)`, as
  `LwsWebSocket.cpp:112-122`, but the refusal is an ordinary queued `Error` event, never a
  synchronous callback (E7).
* **URL**: parsed with `lws_parse_uri`; schemes `ws`, `wss`, and outside Shipping `wss+insecure`
  (self-signed allowed, as `LwsWebSocket.cpp:733-769`). `Host` keeps a non-default port
  (`:775-785`).
* **Origin**: default derived from the URL (`https://` for wss, `http://` for ws, host and
  non-default port), sent raw (T4); `WithOrigin(Value)` overrides it, `WithOrigin({})` omits it.
* **Subprotocols**: `protocol` = the requested list, `local_protocol_name = "varest"` (T3), so the
  connection is bound to our handler whatever the server selects and no ini registration is needed
  (E17). The selected protocol is read in `FILTER_PRE_ESTABLISH` (T6) into `FVaRestSocketOpen::Protocol`.
  If W0 shows LWS 3.0 still demands a registered name, the context registers every subprotocol named
  in settings and assets at creation, and an unknown code-only name gets a clear `Config` error.
* **Headers**: appended in `APPEND_HANDSHAKE_HEADER` with the remaining length checked per header
  (T5); not enough space → the attempt fails with `Config`, never an overrun (E19). Names must be
  RFC 7230 tokens, values visible ASCII (`StringCast<ANSICHAR>` would mangle anything else, review
  item 21); validation happens on the game thread before the command, so the error is immediate.
* **TLS**: `LCCSCF_USE_SSL`; `LOAD_EXTRA_CLIENT_VERIFY_CERTS` → `AddCertificatesToSslContext`,
  `PERFORM_SERVER_CERT_VERIFICATION` → `VerifySslCertificates(X509_STORE_CTX, Domain)`
  (`Runtime/Online/SSL/Public/Interfaces/ISslCertificateManager.h:28`, `:66`), including the
  manager's user-data remap for that callback (`LwsWebSocketsManager.cpp:440-456`). Pinned keys
  configured on the engine certificate manager (`ISslCertificateManager.h:38-56`) apply for free.
* **Connect timeout** (`ConnectTimeout`, 10 s) is measured on the service thread and enforced with
  `lws_set_timeout(…, LWS_TO_KILL_ASYNC)` (T14), so a hitching game thread cannot cause it.
* **Upgrade refusal**: the status comes from the error string (T7: "not 101, was N";
  "unauthorized" = 401; "forbidden" = 403), and from `lws_http_client_http_response(wsi)` if W0 shows
  it is valid there. `Could not initialize connection`, `HS: ACCEPT missing`, `HS: Accept hash
  wrong`, `HS: UPGRADE missing`, `HS: CONNECTION missing`, `HS: Rejected by filter cb` and the
  redirect strings are all in the parser tests.
* **No upgrade response** (rev 2, §22 A2): the upgrade request was sent and the connection closed
  before any HTTP status line arrived (engine.io's answer to a non-matching path). The transport
  reports it as its own failure reason, `NoUpgradeResponse`; W0 pins the LWS 3.0 error string.

### 2.3 Receive

* `CLIENT_RECEIVE` chunks are **appended as bytes** to the connection's assembly buffer; kind from
  `lws_frame_is_binary` on the first fragment; complete when `lws_is_final_fragment` and
  `lws_remaining_packet_payload == 0` (T8). The buffer reserves one byte of slack so
  `FVaRestJson::ParseOwned` can take it without a copy (core §2.1).
* **Size limit** (`MaxMessageBytes`, 1 MiB) is checked as chunks arrive: over it, the buffer is
  dropped and the connection closed with 1009 ("message too big"), reported `Decode`.
* **Text is validated as UTF-8 once**, on the assembled message, with VaRestJson's byte validator
  (core §2.2). Invalid → close 1007 and `Decode` error, as RFC 6455 §8.1 requires. No per-chunk
  conversion exists anywhere (E9 cannot happen).
* **Flow control**: when a socket's undelivered inbound bytes exceed `MaxPendingInboundBytes`
  (16 MiB) — the game thread is not keeping up, or it is backgrounded — the transport stops reading
  (`lws_rx_flow_control(wsi, 0)`, T11), so TCP pushes back on the server. Reading resumes below half.
  Memory stays bounded without dropping the link.

### 2.4 Send

* A send command carries an encoded payload in a buffer that already has `LWS_PRE` headroom (the
  encoder writes after it), so `lws_write` needs no copy (`libwebsockets.h:4845-4870`).
* Messages over 64 KiB go out as fragments (`LWS_WRITE_NO_FIN`, then `LWS_WRITE_CONTINUATION`, T10),
  one per WRITEABLE callback, so PINGs and the close frame can be interleaved between fragments
  (RFC 6455 §5.4 allows control frames inside a fragmented message) and a large upload does not
  starve the dead-link detector.
* `lws_partial_buffered` is checked after each write; LWS's own buffering of a truncated write is
  counted.
* **Buffered amount** (bytes accepted by `Send` and not yet handed to the socket) is exact and
  published to the game thread; it is the basis of backpressure (§6.3). `Drained` fires when it
  returns to zero after crossing the high-water mark.
* **Auto-replies** (rev 2, §22 SW-5): per-connection rules from the connect command answer an exact
  inbound **text** frame on the service thread. A reply is a data message, so it never enters a
  fragmented message (RFC 6455 §5.4 allows only control frames there); it is written at the next
  message boundary, ahead of every queued data message, is not counted in the buffered amount or
  backpressure, and is never dropped. Connections with rules get `SO_SNDBUF` 256 KiB
  (`lws_get_socket_fd` + `setsockopt`), bounding what already sits in the OS buffer ahead of a reply.
* **Batches** (rev 2, §22 SW-9): the frames of one `SendBatch` are written back to back; only control
  frames and auto-replies can sit between them.

### 2.5 Ping, close, end of connection

* **Protocol ping** (§5): a `LWS_WRITE_PING` with an 8-byte payload (sequence + send time) every
  `PingInterval`; `CLIENT_RECEIVE_PONG` (T9) matches the payload and yields an RTT measured on the
  service thread. No pong **and** no inbound bytes within `PingTimeout` → the connection is killed
  and reported `By = Heartbeat`. Rev 2 (§22 SW-5): this deadline, and a stage's idle deadline
  (`Ctx.SetIdleDeadline`, inbound data frames only), are **suspended while VaRest has paused reading**
  (flow control, §2.3) — a client that stopped reading cannot blame the server; a connection that ends
  meanwhile carries `FVaRestSocketClose::bWhileReadPaused`.
* **Our close**: `lws_close_reason(wsi, Code, Reason)` then a non-zero return from WRITEABLE, as
  `LwsWebSocket.cpp:515-530`; `CloseTimeout` (5 s) bounds a server that never echoes (W0 measures
  whether LWS waits for the echo).
* **Peer close** (T12): length 0 → **1005**, length 1 → 1002 (malformed), else the big-endian code
  and a **UTF-8** reason (invalid bytes become U+FFFD with a warning). Codes a peer must not send
  (<1000, 1004–1006, 1015, 1016–2999, ≥5000) are normalised to 1005. The callback returns 0 so LWS
  echoes the close, as RFC 6455 §5.5.1 requires (the engine returns 1, `LwsWebSocket.cpp:389-390`).
* **Unclean end** (`CLIENT_CLOSED` / `WSI_DESTROY` without a close frame, or a read/write error) →
  1006, `bWasClean = false`.
* Every `Closed` event says who closed (`Client`, `Server`, `Transport`, `Heartbeat`, `Timeout`) —
  the transport knows, so classification never infers it from the code (E14).

---

## 3. Connection (client layer)

### 3.1 State machine

`EVaRestSocketState`: `Disconnected`, `Connecting`, `Connected`, `Reconnecting`. `Connecting` covers
attempts and waits before the first open since `Connect()`; `Reconnecting` covers those after an
open connection was lost. `FVaRestSocketStatus` adds `Attempt`, `bAttemptInFlight`, `WaitReason`
(`Backoff`, `Offline`, `Background`, `Auth`), `NextAttemptIn`, `bReady`.

**`Close()` takes effect at the call.** The state becomes `Disconnected` and `OnDisconnected`
(`By = Client`) fires right after the current dispatch; the close handshake finishes in the
transport as a detached connection, bounded by `CloseTimeout`. There is no public `Closing` state, so
"Close → change URL/token → Connect" works at once (review item 9).

**Handlers never re-enter the transport or the state machine.** `Connect`, `Close`,
`ReconnectNow`, `StopReconnecting` and `Restart` called from inside a dispatch are recorded and run
after the dispatch loop, in the same VaRest tick; attempts always start from VaRest's own tick. The
dispatch loop iterates a stable snapshot; non-Shipping builds `check` a dispatch-depth counter
(ruling R2, review item 1).

| State \ Event | `Connect()` | open | retryable fail / loss | terminal fail / loss | `Close()` | `ReconnectNow()` | `StopReconnecting()` | `Restart(reason)` | logout | offline / online | background / foreground |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Disconnected | → Connecting | — | — | — | no-op | no-op | no-op | no-op | no-op | — | — |
| Connecting (waiting) | no-op | — | — | — | → Disconnected (`Canceled`) | attempt now | → Disconnected (`Canceled`) | attempt now | → Disconnected (`Canceled`) | attempts continue at `MaxDelay`, uncounted (§13) | → wait `Background` / attempt now |
| Connecting (attempt in flight) | no-op | → Connected | wait (OnRetryScheduled) | → Disconnected (GaveUp) | abandon → Disconnected | no-op | abandon → Disconnected | abandon, attempt now | abandon → Disconnected | — | abandon, wait `Background` / — |
| Connected | no-op | — | → Reconnecting (OnDisconnected, `bWillReconnect`) | → Disconnected (OnDisconnected, GaveUp) | → Disconnected | = `Restart` | no-op | → Reconnecting, attempt counter kept | close 1000 "logout" → Disconnected | probe ping on online | close 1001 "background", wait / — |
| Reconnecting | as Connecting rows | → Connected (`bReconnect`) | wait | → Disconnected (GaveUp) | → Disconnected | attempt now | → Disconnected | attempt now | → Disconnected | attempts continue at `MaxDelay`, uncounted | as Connecting rows |

The background column is for the `Disconnect` policy; with `Keep` it does nothing (§13).
`Restart(reason)` is internal: `SetEnvironment` (§11), foreground with `Keep` but a dead link, a
stage. `ReadyTimeout` (§6.2) is a retryable failure. Owner destroyed or hub shutdown = `Close()`.
GaveUp fires for terminal failures and exhausted attempts, never for `Close()`.
`FVaRestSocketOpen` (BlueprintType): `Attempt`, `bReconnect`, `ConnectSeconds`, `Protocol`.
A table-driven test covers every state × event pair.

### 3.2 Connect options (API level)

URL (`ws(s)://`; `http(s)://` mapped to `ws(s)://`, as dawg's `SetURL`; asset paths resolved per
§11), `WithProtocols`, `WithHeader`, `WithQuery`, `WithOrigin`, `OnHandshake(Owner, Fn)` per attempt
on the game thread (edits `FVaRestSocketHandshake {Url, Query, Headers, Protocols, Origin,
Attempt, bReconnect}`; order: config → session/auth (§3.3) → stages → user hook → validation).
Only these, plus auth placement, apply "from the next attempt"; routing, codec, limits and
handlers apply at once.

### 3.3 Auth and session

A socket shares an HTTP client's session with `WithApi(GameApi)` (resolved through the paired hub),
`WithClient(FVaRestClient)`, or automatically for asset sockets.

* **Per attempt**, `IVaRestAuth::Apply` writes into the handshake (§19 item 2); a token
  expiring within 30 s is refreshed first (single-flight, shared with HTTP, core §4.2;
  `WaitReason = Auth`).
* **Placement** `EVaRestSocketAuthPlacement`: `Header` (default, `Authorization: Bearer …`),
  `Query(name = "token")` (browsers cannot set WebSocket headers, so many servers read the query;
  dawg's `/ws/connect?token=`), or `FirstMessage(route = "auth", field = "token")` — sent in the
  handshake lane at open (§6.2), usually with `ReadyWhen`. Cookie-authenticated servers (Django
  Channels, ActionCable) take a `Cookie` session header from the client's session headers.
* **401 on upgrade, or close 4401**: one reactive refresh (single-flight), then an immediate retry
  that is not a backoff step. Refresh failure, or 401 again → terminal `Auth`; the socket raises the
  client's `OnSessionExpired` (§19 item 2).
* **Logout** (`OnSessionCleared`, §19 item 2): close 1000 "logout", no reconnect. A token
  refresh does nothing to an open socket.
* **Session replaced** (rev 2, §22 A1: `OnSessionChanged(Replaced)` — another identity's token via
  `SetBearerToken`/`ImportSession`, or a login while logged in): `Restart` with the stages' closing
  hook (SW-7), so a connection authenticated as the old user does not stay open. A `WithBearer` lambda
  is opaque; code-first apps call `ResetSession()` on a user switch.
* Code-first without a client: `WithBearer(TFunction<FString()>)`, read per attempt.

### 3.4 Reconnect

`FVaRestReconnectPolicy` (BlueprintType):

| Field | Default | Meaning |
| --- | --- | --- |
| `bEnabled` | true | |
| `MaxAttempts` | 0 = unlimited | consecutive failed attempts since the last stable connection |
| `BaseDelay`, `Multiplier`, `MaxDelay` | 0.5 s, 2, 30 s | exponential backoff, the core's formula (core §4.1) |
| `Jitter` | Full | `Full` (uniform 0..d) or `Ratio(r)` (d·(1±r), socket.io-client's style) |
| `StableAfter` | 5 s | the attempt counter resets only after a connection stayed open this long |
| `bStopOnNormalClose` | false | make a server's clean 1000 terminal |
| `TerminalCloseCodes`, `ReconnectCloseCodes` | empty | overrides |

`StableAfter` stops accept-then-close loops. Background waits do not count; offline waits keep
probing (§13). Timers use the core's `IVaRestClock`; jitter uses a per-socket `FRandomStream` that
tests seed.

**Precedence**: the app's `Close()` always stops → the stage's `ShouldReconnect` → `ReconnectIf`
(C++) or a veto from an `OnDisconnected` handler (`StopReconnecting()`, C++ and BP) → the policy's
code sets → the table. Rows are matched on (who closed, code); the transport reports who (§2.5).

| Event | Default | Error kind when terminal |
| --- | --- | --- |
| the app's `Close()` / `StopReconnecting()` | stop | `Canceled` |
| VaRest closed it: ping timeout, inbound over the limit (1009), invalid UTF-8 (1007), `ReadyTimeout` | reconnect | |
| unclean end (1006, `bWasClean = false`) | reconnect | |
| server 1000, clean | **reconnect** (counted; `bStopOnNormalClose` makes it terminal) — ruling R3 | `Server` |
| 1001, 1005, 1011, 1012, 1014, 1015 | reconnect | |
| 1013 Try Again Later, 4429 | reconnect, delay floor 5 s | |
| 1009 from the server (our message was too big) | reconnect (message lost; `OnError`) | |
| 1002, 1003, 1007, 1010 from the server | stop — unless W0 shows LWS 3.0 rewrites codes to 1002, then 1002 reconnects and stops after 3 in a row | `Server` |
| 1008 Policy Violation | stop | `Server` (no session expiry) |
| 4401 | refresh session, then reconnect (§3.3) | `Auth` |
| 4400, 4403, 4404 | stop | `Auth` for 4403, else `Server` |
| upgrade 401 | refresh session, then retry (§3.3) | `Auth` |
| upgrade 408, 429, 5xx | reconnect | |
| upgrade 3xx (redirect not followed or failed), other 4xx | stop | `Http` (`HttpStatus` set; a 403 message names the Origin sent) |
| `Bad URL`, bad scheme, allowlist refusal, handshake validation, subprotocol mismatch | stop | `Config` |
| no upgrade response (rev 2, §22 A2: `NoUpgradeResponse`) | reconnect; the reason travels in `FVaRestSocketClose` and `OnRetryScheduled`, so a protocol stage may make it terminal (Socket.IO: after 3, `Config`) | |
| connect timeout, DNS/TCP/TLS failure, unknown connect error | reconnect | |
| **anything else** | reconnect | |

The 4xxx rows follow the "4000 + HTTP status" convention (graphql-ws and others).

### 3.5 Close

`Close(Code = 1000, Reason = "")`: codes 1000–1003, 1007–1014 and 3000–4999 are sent as given,
anything else is replaced by 1000 with a warning (RFC 6455 §7.4); the reason is truncated to 123
bytes on a UTF-8 boundary. The attempt's generation is bumped at the call: no message handler runs
for that connection afterwards, including entries already in its delivery queue (review item 24).
Codes VaRest itself sends: 1000 "logout"; 1001 "shutdown", "background", "released",
"ping timeout"; 1007 invalid UTF-8; 1009 inbound over the limit.

---

## 4. Threading

| Work | Thread |
| --- | --- |
| Config, `Connect`, `Close`, state machine, timers, stages, handlers, BP events | game thread |
| LWS calls, assembly, UTF-8 validation, ping/pong, connect/close deadlines, auto-replies and idle deadlines (rev 2, SW-5) | VaRest service thread |
| Parse, codec (incl. a per-connection decoder, rev 2 SW-2), typed decode plans | the socket's `UE::Tasks::FPipe` (§7.1) |
| Encode for `Send`/`SendBinary`/`Emit`, and `SendBatch` (rev 2, SW-9) | caller's thread, from an immutable config snapshot (§6.1) |

Cross-thread structures: the transport command queue (MPSC), each socket's event queue (SPSC:
service thread → pipe), each socket's delivery queue (pipe → game thread), the off-thread send
staging queue (MPSC, bounded by `MaxQueuedBytes`), and the published snapshots (routing/codec/naming
for encoders; route → struct types for decoders), replaced copy-on-write on the game thread.
**The core threading rule now holds without exception**: nothing is parsed on the game thread.

---

## 5. Ping-pong and heartbeat

**Protocol ping is on by default for every socket (ruling R3):** `PingInterval` 20 s,
`PingTimeout` 10 s (settings and per socket; 0 disables). Why these values: 20 s sits under the
30–60 s idle cutoffs of common load balancers, reverse proxies and carrier NATs, so the ping also
keeps a quiet link open; 10 s tolerates slow mobile links and a large upload ahead of the ping
(which is interleaved between our own fragments, §2.4). Worst-case detection is 30 s; cost is about
24 bytes on the wire (masked PING with an 8-byte payload, plus the PONG) every 20 s. RFC 6455 §5.5.2
obliges every server to answer, so no
server contract is needed. A ping goes out every interval even on a busy link, so RTT stays fresh:
`GetLatency()` (last), `GetLatencyAverage()` (EWMA, α = 0.2), BP `Get Latency`, a trace event.
Deadlines live on the service thread (§2.5), immune to game-thread hitches.

**App-level heartbeat stays opt-in** (it needs a server contract), for servers that only speak an
app ping, or to measure application-level latency:

```cpp
FVaRestHeartbeat::Text(TEXT("ping"), TEXT("pong"))           // exact payloads
FVaRestHeartbeat::Json(TEXT("ping"), TEXT("pong"))           // {"<RouteField>":"ping"}, pong matched by route
FVaRestHeartbeat::ServerPing(TEXT("ping"), TEXT("pong"))     // server pings, we answer and watch the gap
    .Interval(20.f).Timeout(10.f)
```

`FVaRestHeartbeat` (BlueprintType): `Mode` (`None`, `ClientPing`, `ServerPing`), payloads (text or
route), `Interval`, `Timeout`, `bAnyInboundIsAlive` (true). Text payloads are matched as exact bytes
in the heartbeat stage; JSON ones by route right after the codec (servers add fields such as a
timestamp). Pings and pongs never reach handlers. Its deadlines are checked **after** the tick's
delivery queue drained, and a game-thread stall longer than a deadline re-arms once with a probe
before declaring death (review item 27). Engine.IO's mandatory heartbeat is a stage on this
mechanism (§8). A binary preset is deferred (YAGNI).

LWS's own `ws_ping_pong_interval` is set to 0 in our context; the engine's global
`PingPongInterval` (E16) affects only the engine's sockets.

---

## 6. Sending

### 6.1 API

```cpp
Socket.Send(TEXT("hello"));                                        // text, UTF-8
Socket.Send(FUtf8StringView(Utf8));                                // text, no conversion
Socket.Send(FVaRestJson::Object().Set(TEXT("type"), TEXT("move")));   // JSON text
Socket.Send(FMove{ 1, 2 });                                        // USTRUCT -> JSON text (core §2.4 encode, naming policy)
Socket.SendBinary(MoveTemp(Bytes));
Socket.Emit(TEXT("move"), FMove{ 1, 2 });                          // routed envelope (§7.3)
Socket.Emit(TEXT("move"), FMove{ 1, 2 }, EVaRestSend::Volatile);   // drop instead of queueing
```

* Encoding writes **straight to UTF-8** after the `LWS_PRE` headroom (core §2.4 encode; buffer layout §2.4). `Emit`
  writes envelope and payload in one pass. With no `DataPath`, payload members are merged next to the
  route field; a colliding payload field is skipped with a one-time warning. A scalar or array payload
  without `DataPath` is a compile error for C++ types and a `Config` error for `FVaRestJson`.
* Outbound route field, id field and `DataPath` must be single top-level keys; inbound they may be
  nested paths.
* The `Send` template is constrained to `USTRUCT` types, so `Send(42)` fails with a readable message.
* `EVaRestSend`: `Default`, `Volatile` (drop while not ready, or above the high-water mark),
  `Handshake` (bypasses readiness, §6.2).
* Returns `EVaRestSendResult`: `Sent`, `Queued`, `Dropped`, `Rejected`. Drops and rejections also
  reach `OnError`, at most once per second per socket with a count.
* **Thread contract.** `Send`, `SendBinary` and `Emit` may be called from any thread: they encode
  from the published config snapshot and go through the bounded staging queue, flushed on the next
  game-thread tick (`Volatile` is decided at flush). Rev 2 (§22 SW-9): `SendBatch` may also be called
  from any thread — one staging entry, queued, dropped and written as one unit, optionally pinned to
  (connection generation, channel epoch) so it is dropped at flush if either changed; the send limit
  (`Ctx.SetSendLimit`) is enforced at flush for every queued frame; the codec's `EncodeReply` returns
  a batch through the same path. Everything else — `Request`, `SendFrame`, configuration, lifecycle —
  is game-thread only and `check`s it. On the game thread, a ready socket with nothing queued sends at
  once.

### 6.2 Lanes, readiness, queue

Three lanes, in this order on the wire:

1. **Bypass**: stage `SendBypass` and `EVaRestSend::Handshake` (graphql-ws `connection_init`,
   first-message auth). Goes out whenever the transport is open.
2. **Ready lane**: sends made inside `OnReady` handlers (room joins, subscriptions).
3. **Backlog**: everything else sent while not ready, FIFO.

Sequence per connection: open → `OnConnected` → readiness holds (`ReadyWhen(route)`, stage holds
such as Socket.IO's namespace CONNECT) → ready → `OnReady` (its sends first) → backlog. Without a
hold, ready follows `OnConnected` at once. `ReadyTimeout` (default `ConnectTimeout`, from open) ends
an attempt that never becomes ready as a retryable failure. Readiness, the ready lane and the backlog
are per channel (§1.3).

Backlog limits: `MaxQueued` 256 messages, `MaxQueuedBytes` 1 MiB; overflow policy `DropOldest`
(default), `DropNewest`, `Reject`; a drop is `Offline`. `SendBatch` frames are queued and dropped as
one unit. The backlog survives reconnects, not `Close()` (entries reported `Canceled`).

### 6.3 Backpressure

The transport reports an exact buffered amount (§2.4). `GetBufferedAmount()`; above
`SendHighWater` (256 KiB) `Volatile` sends drop; above `MaxBufferedBytes` (16 MiB) any send is
`Rejected` (`Offline`); `OnDrained` fires when the buffer empties after crossing the high-water
mark. `MaxSendBytes` (16 MiB) bounds one message. Receive-side backpressure is flow control (§2.3).

---

## 7. Receiving

### 7.1 Ordered delivery

Every event of a socket — open, message, pong, drained, closed, error, and events generated by
stages (namespace connect/disconnect) — flows through **one ordered path**: the service thread pushes
it to the socket's event queue; the socket's `UE::Tasks::FPipe` (FIFO, `Runtime/Core/Public/Tasks/Pipe.h:20-27`)
decodes messages and passes every event, decoded or not, to the delivery queue; the game-thread tick
drains that queue in order. So `OnDisconnected` is delivered after every message received before it,
and a small message never overtakes a large one (review item 7). Pipe tasks hold a `TSharedRef` to
the inbound pipeline so the pipe outlives its last task.

Decoding on arrival costs no latency: the game thread would see the message no earlier than its next
tick anyway, and decoding takes microseconds. Results received before a close are delivered, unless
the app called `Close()` (§3.5). The decode executor is injectable (synchronous in tests), so ordering
tests are deterministic. `MaxPendingInboundBytes` (§2.3) bounds the pipe and the delivery queue
together.

### 7.2 Decode

`FVaRestJson::ParseOwned` (zero-copy, core §2.1) → codec (channel, route, data, id, attachments) →
typed decode for each handler type on that route (core §2.4), from the published route → type
snapshot. JSON is parsed only when used: a route or typed handler exists, or `Message.Json()` is
called. Decode follows the core's tolerant rules; `bStrictDecode` turns mismatches into
`OnError(Decode)` and skips the handler.

### 7.3 Routing

`FVaRestSocketRouting` (BlueprintType), read by the default codec:

| Field | Default | Meaning |
| --- | --- | --- |
| `RouteField` | `type` | path of the route value; empty = no routing |
| `Envelope` | whole message | core `FVaRestEnvelope` (core §3.4): `DataPath`, error paths for replies |
| `IdField` | empty | correlation (§7.5); empty = off |
| `IdFormat` | `String` | `String` (`"<8 hex per socket>-<n>"`) or `Number` |
| `ReplyRoute` | empty | if set, only messages with this route resolve requests |
| `bDecodeBinaryAsJson` | false | route binary messages as UTF-8 JSON |

Presets: `ByField(TEXT("type")).DataAt(TEXT("data"))` (dawg's `Type`/`Data` shape); `JsonRpc()`
(`method`/`params`/`id`; a reply is a message **without** `method`, so server requests with `id` +
`method` never resolve ours). Non-JSON text protocols use a custom codec.

* Route values compare by bytes, case-sensitive (the JSON core rule); a non-string route value
  (Discord's `"op": 0`) compares by its canonical text (integers in decimal).
* Per message: `OnMessage` → route handlers (registration order) → `OnUnrouted` if none matched.
  Unrouted values log at Verbose once each, for at most 256 distinct values.

### 7.4 Typed handlers

```cpp
Socket.On(TEXT("chat"), this, [this](const FChatMessage& M) { ... });     // T deduced from the lambda
Socket.On(TEXT("raw"),  this, [this](const FVaRestJson& Data) { ... });
Socket.On(TEXT("any"),  this, [this](const FVaRestSocketMessage& Msg) { ... });
Socket.Off(TEXT("chat"), this);  Socket.OffAll(this);
```

`T`: any `USTRUCT`, `FVaRestJson`, `FString`, or `FVaRestSocketMessage`. The owner is weak; after it
dies its handlers are skipped and removed. A generic lambda cannot be deduced; a `static_assert` says
to name the type.

### 7.5 Requests and replies, both directions

Outbound:

```cpp
Socket.Request<FJoinReply>(TEXT("join"), FJoin{ Room })   // [[nodiscard]] builder, like core §3.2
    .Timeout(5.f).Owner(this)
    .OnSuccess([this](const FJoinReply& R) { ... })
    .OnError([](const FVaRestError& E) { ... })
    .Send();
```

* Needs `IdField`. A reply is an inbound message whose id matches a pending request (and whose route
  equals `ReplyRoute` when set); it is consumed, not routed. Rev 2 (§22 SW-1): when the codec sets
  `FVaRestSocketMessage::Kind`, only `Reply`/`ErrorReply` messages resolve pending requests and only
  `Request` messages reach `OnRequest`; the pending table checks that a reply's channel matches.
* **String ids by default**, prefixed per socket (ids are written at encode time, before queueing):
  numeric counters would collide with the app's own `id` fields. `Number` exists for servers that
  need it, paired with `ReplyRoute` or the `JsonRpc` rule.
* The reply's data and errors follow the routing envelope: failure → `Kind=Server` (core §3.3).
* Timeout (10 s, from `Send()`) → `Timeout`; a request still in the backlog leaves it then. A sent
  request whose connection drops fails at once with `Connection`, `bRetryable`. A late reply is
  dropped (Verbose). Over `MaxPendingRequests` (1024) → `Offline`, `bRetryable`.

Inbound (server → client requests: JSON-RPC calls, Socket.IO acks, "accept match?" prompts):

```cpp
Socket.OnRequest(TEXT("accept_match"), this,
    [this](const FMatchOffer& Offer, TVaRestSocketReply<FMatchAnswer>& Reply) { Reply.Send({ true }); });
```

* `FVaRestSocketMessage::Reply(Payload)` / `ReplyError(Code, Message)`; BP *Reply To Message*.
  The codec's `EncodeReply(Id, Payload, bError)` builds the frame; the default codec echoes the id and
  writes the payload at `DataPath` (JSON-RPC: `result` / `error`).
* At most one reply; a second logs a warning and is dropped. A request handler that returns without
  replying replies nothing (protocols with mandatory replies time out on the server, as they would
  with any client).

---

## 8. Extension points (what Socket.IO builds on)

| Need | Hook |
| --- | --- |
| Raw frame access | `IVaRestSocketStage::OnInbound/OnOutbound(FVaRestSocketFrame&, Ctx)` → `Continue` / `Consumed`; frames are complete messages with kind, shared payload and `Attachments`. `SendFrame` bypasses the codec. |
| Protocol codec | `IVaRestSocketCodec` (thread-safe, runs on the pipe): `Decode(Frame) -> FVaRestSocketMessage {Channel, Route, Data, Id, Kind, Attachments}`; `Encode(Channel, Route, Payload writer, Id) -> frames` (one or a batch); `EncodeReply(Channel, Id, Payload, bError)` → a batch (rev 2, SW-9). Rev 2 (§22 SW-2): `MakeDecoder()` creates a stateful per-connection decoder on the connection's Open event; `Decode` may return `NeedMore` (multi-frame messages) or `ProtocolError` (closed 1002, decoder poisoned for that connection); typed decode is a type-erased plan that may decode several positions of an array `Data`. `Emit`, `On`, `Request`, `OnRequest` work unchanged over any codec. |
| Namespaces | `FVaRestSocketChannel` (§1.3): `Socket.Channel(TEXT("/chat"))` has its own router, readiness, ready lane, backlog, pending requests and `OnConnected/OnReady/OnDisconnected`. The codec names each message's channel. Rev 2 (§22 SW-8): `Ctx.ChannelUp` / `Ctx.ChannelDown(…, WillRetry \| Ended)` drive a channel's lifecycle without closing the socket; `Ctx.HoldInbound(Channel)` queues its inbound messages until its `OnReady` handlers and backlog flush ran. |
| Binary attachments | `FVaRestSocketFrame::Attachments` and `FVaRestSocketMessage::Attachments` (shared immutable buffers). Our transport preserves wire order (§2.1), so a header and the binary frames after it arrive in order; rev 2: the codec's per-connection decoder (§22 SW-2), not a stage, collects the N binary frames into the header's message on the pipe, so typed decode sees them. Typed decode binds `TArray<uint8>` fields to attachments through a VaRestJson decode-option resolver (§19 item 10). |
| Custom heartbeat | a stage returning `true` from `ProvidesHeartbeat()` disables the app heartbeat (protocol ping stays); `Ctx.MarkDead(Reason)`, `Ctx.SendBypass(Frame)`, `Ctx.Timer(Delay, Fn)`. Rev 2 (§22 SW-5): auto-reply rules in the connect command (Engine.IO `2` → `3`) and `Ctx.SetIdleDeadline(Seconds)` (Engine.IO `pingInterval + pingTimeout` from its OPEN), both on the service thread. |
| Handshake | `IVaRestSocketStage::OnHandshake(FVaRestSocketHandshake&)` per attempt (`EIO=4&transport=websocket`). |
| Session | `Ctx.Session()` (current token for the CONNECT auth payload), `Ctx.RequestRefresh()` (CONNECT_ERROR with an expired token). |
| Readiness | `Ctx.HoldReady(Channel)` / `Ctx.SetReady(Channel)`. |
| Closing | rev 2 (§22 SW-7): `OnClosing(Ctx, FVaRestSocketCloseRequest)` runs inside app-initiated closes and session-replacement restarts; its `Ctx.SendBypass` frames precede the close frame. |
| Reconnect and close | `ShouldReconnect(Close) -> TOptional<bool>`; `Ctx.Close(Code, Reason, EVaRestReconnect)` (explicit decision); `OnOpen` with `bReconnect`; `FVaRestReconnectPolicy` with `Jitter = Ratio(0.5)` = socket.io-client defaults. |
| Limits | `Ctx.SetSendLimit(Bytes)` (Engine.IO `maxPayload`). |
| Correlation | the pending-request table (`TVaRestPendingRequests`), reused for acks in both directions; rev 2 (§22 SW-1): replies by `Kind`, a socket-wide `int64` counter for numeric ids, channel checked. |
| Transport | `IVaRestSocketTransport`. |

Note: encoding happens before queueing, so per-connection data cannot be baked into queued frames;
fine for Socket.IO, but a Phoenix-style per-connection `ref` would need a stage to rewrite frames at
flush.

Pipeline:

```
inbound:  LWS → assemble + validate → event queue → pipe: decode (codec) → delivery queue → GT: stages, heartbeat, trace/stats/log → replies → OnMessage → routes → OnUnrouted
outbound: Send/Emit/Request/Reply → encode (caller thread, snapshot) → GT: stages → lanes/backlog (until ready) → log → trace/stats → command queue → LWS
```

Stages run on the game thread in registration order (reverse on outbound). They see decoded frames,
so the Engine.IO codec strips packet types on the pipe and stages act on control packets.

---

## 9. C++ surface

Asset socket, one statement:

```cpp
Chat = UVaRestSocketHub::Get(this)->Socket(GameApi, TEXT("Chat"))
    .On(TEXT("chat_message"), this, [this](const FChatMessage& M) { Lines.Add(M); })
    .OnReady(this, [this](const FVaRestSocketOpen&) { Chat.Emit(TEXT("join"), FJoin{ Room }); })   // re-sent after every reconnect
    .Connect();
```

Code-first, sharing the API session:

```cpp
Feed = UVaRestSocketHub::Get(this)->Make(TEXT("wss://feed.game.io/v1"))
    .WithApi(GameApi)                                     // bearer + refresh + logout (§3.3)
    .WithRouting(FVaRestSocketRouting::ByField(TEXT("op")).DataAt(TEXT("d")))
    .On(TEXT("tick"), this, [this](const FMarketTick& T) { Apply(T); })
    .OnDisconnected(this, [this](const FVaRestSocketClose& C) { ShowBanner(C.bWillReconnect); })
    .Connect();
```

Handle shape:

```cpp
class VARESTWEBSOCKET_API FVaRestSocket
{
public:
    // Configuration (game thread). Connection settings apply from the next attempt, the rest at once.
    FVaRestSocket WithProtocols(TArray<FString>);   FVaRestSocket WithHeader(FString, FString);
    FVaRestSocket WithQuery(FString, FString);      FVaRestSocket WithOrigin(TOptional<FString>);
    FVaRestSocket WithApi(UVaRestApi*);             FVaRestSocket WithClient(FVaRestClient);
    FVaRestSocket WithBearer(TFunction<FString()>); FVaRestSocket WithAuthPlacement(FVaRestSocketAuthPlacement);
    FVaRestSocket WithReconnect(FVaRestReconnectPolicy);  FVaRestSocket NoReconnect();
    FVaRestSocket WithPing(float Interval, float Timeout); FVaRestSocket WithHeartbeat(FVaRestHeartbeat);
    FVaRestSocket WithRouting(FVaRestSocketRouting);  FVaRestSocket WithCodec(TSharedRef<IVaRestSocketCodec>);
    FVaRestSocket Use(TSharedRef<IVaRestSocketStage>);
    FVaRestSocket WithLimits(FVaRestSocketLimits);  FVaRestSocket WithQueue(FVaRestSocketQueuePolicy);
    FVaRestSocket WithNaming(EVaRestJsonNaming);    FVaRestSocket WithLogLevel(EVaRestLogLevel);
    FVaRestSocket ReadyWhen(FString Route);         FVaRestSocket ReconnectIf(TFunction<TOptional<bool>(const FVaRestSocketClose&)>);
    FVaRestSocket OnHandshake(const UObject* Owner, TFunction<void(FVaRestSocketHandshake&)>);
    FVaRestSocket Owner(const UObject* Owner);      // unnamed sockets only
    FVaRestSocketChannel Channel(FStringView Name) const;
    FVaRestSocketWeak AsWeak() const;

    // Events. Owner is weak; each also has an overload without Owner for free functions.
    FVaRestSocket OnConnected(const UObject* Owner, TFunction<void(const FVaRestSocketOpen&)>);
    FVaRestSocket OnReady(const UObject* Owner, TFunction<void(const FVaRestSocketOpen&)>);
    FVaRestSocket OnDisconnected(const UObject* Owner, TFunction<void(const FVaRestSocketClose&)>);
    FVaRestSocket OnRetryScheduled(const UObject* Owner, TFunction<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)>);
    FVaRestSocket OnGaveUp(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSocketClose&)>);
    FVaRestSocket OnStateChanged(const UObject* Owner, TFunction<void(EVaRestSocketState Old, EVaRestSocketState New)>);
    FVaRestSocket OnError(const UObject* Owner, TFunction<void(const FVaRestError&)>);
    FVaRestSocket OnDrained(const UObject* Owner, TFunction<void()>);
    FVaRestSocket OnMessage(const UObject* Owner, TFunction<void(const FVaRestSocketMessage&)>);   // OnUnrouted: same shape
    FVaRestSocket OnBinary(const UObject* Owner, TFunction<void(TConstArrayView<uint8>, const FVaRestSocketMessage&)>);
    template <typename F> FVaRestSocket On(FStringView Route, const UObject* Owner, F&& Handler);
    template <typename F> FVaRestSocket OnRequest(FStringView Route, const UObject* Owner, F&& Handler);
    void Off(FStringView Route, const UObject* Owner);  void OffAll(const UObject* Owner);

    // Lifecycle (game thread).
    FVaRestSocket Connect();  void Close(int32 Code = 1000, FStringView Reason = {});
    void ReconnectNow();      void StopReconnecting();

    // Sending (§6). Send/SendBinary/Emit: any thread. The rest: game thread.
    EVaRestSendResult Send(FStringView Text, EVaRestSend Flags = EVaRestSend::Default);   // + FUtf8StringView, FVaRestJson, USTRUCT
    EVaRestSendResult SendBinary(TArray<uint8>&& Bytes, EVaRestSend Flags = EVaRestSend::Default);
    template <typename T> EVaRestSendResult Emit(FStringView Route, const T& Payload, EVaRestSend Flags = EVaRestSend::Default);
    template <typename TReply = FVaRestJson, typename T> [[nodiscard]] TVaRestSocketRequest<TReply> Request(FStringView Route, const T& Payload);
    EVaRestSendResult SendFrame(FVaRestSocketFrame Frame);  EVaRestSendResult SendBatch(TArray<FVaRestSocketFrame> Frames);  // SendBatch: any thread (rev 2, SW-9)

    // State.
    EVaRestSocketState GetState() const;  FVaRestSocketStatus GetStatus() const;  bool IsConnected() const;  bool IsReady() const;
    FVaRestSocketStats GetStats() const;  double GetLatency() const;  int64 GetBufferedAmount() const;
    int32 GetQueuedCount() const;  void ClearQueue();  FName GetName() const;  FString GetRedactedUrl() const;
};
```

`UVaRestSocketHub`: `Get(WorldContext)`, `GetForEditor()`, `Socket(UVaRestApi*, FName)` (named, lazy),
`Make(Url)` (unnamed, handle-owned), `Make(FName, Url)` (named), `Find(FName)`, `Release(FName)`,
`GetSockets()`, `CloseAll()`. Rev 2 (§22 A3): `Socket`, `Find` and BP *Get Socket* refuse sockets and
entries owned by another protocol module (Socket.IO) with a `Config` error naming its accessor, so raw
`Send` never reaches a Socket.IO connection.

---

## 10. Blueprint surface

### 10.1 `UVaRestSocketClient`

* Functions: Connect, Close (Code, Reason), Reconnect Now, Stop Reconnecting, Send Text, Send Bytes,
  Send Json, Send Struct (wildcard), Emit (Route, wildcard payload, Volatile), Reply To Message,
  Get State, Get Status, Is Connected, Is Ready, Get Latency, Get Buffered Amount, Get Stats,
  Get Queued Count, Clear Queue, Set Header, Remove Header, Set Query Param, Set Url, Set Origin,
  Set Protocols, Set Reconnect Policy, Set Ping, Set Heartbeat, Set Routing, Ready When.
* Events: On Connected, On Ready (Is Reconnect), On Disconnected (Close, Will Reconnect),
  On Retry Scheduled (Attempt, Delay, Close), On Gave Up (Error, Close), On State Changed,
  On Message, On Unrouted, On Error, On Drained.
* Per-attempt values from BP: change headers or query in On Retry Scheduled; the next attempt reads
  them.
* Rev 2 (§22 SW-10): `UVaRestSocketComponent` (`BlueprintSpawnableComponent`) — `Api` + `Entry` (the entry's
  hub-named socket, shared with *Get Socket* and the typed nodes, `bCloseOnEndPlay`) or `Url` with `Scope`
  (`Actor`: unnamed socket closed on `EndPlay`; `GameInstance`: hub-named socket kept across level travel),
  `bAutoConnect`, `BlueprintAssignable` lifecycle events, `GetClient()`. Protocol modules subclass it
  (Socket.IO: `UVaRestSioComponent`).

### 10.2 Library and async nodes

* `UVaRestSocketLibrary`: Get Socket (WorldContext, Api, Name) — also offered by the spawner as
  *Socket ▸ DA_GameApi ▸ Chat*, so the name is never typed; Make Socket (WorldContext, Url,
  Protocols, Headers, optional Name; ownership per §1.4); Describe Socket Close; message accessors;
  Socket Message To Struct (wildcard, CustomThunk); ping, heartbeat and routing presets.
* Async action **Connect Socket**: `On Ready` fires on **every** ready, with `Is Reconnect` — put
  joins and subscriptions there and they are re-sent after each reconnect. `On Failed` (Error, Close)
  fires for any end before the first open: give-up, `Close`, `Stop Reconnecting`, owner gone.

### 10.3 K2 nodes (`VaRestEditor`)

The asset's declared messages (§11) drive typed nodes, offered by the *Call Endpoint* action spawner
(core §5.4): typing "chat" offers *VaRest ▸ DA_GameApi ▸ Chat ▸ On chat_message*. Asset, socket and
route are node properties, refreshed when the asset changes; orphaned pins are kept and flagged.

* **On Socket Message**: exec in (start listening), `Then`, `On Message` (every message), typed
  `Data`, `Message`, and the listen action (`Stop`). Without an asset: Socket pin, Route, `Message
  Type` picker (like *Send Request*). Expands to an async action registered with the game instance
  that stops when its world-context object dies; data arrives decoded into `FInstancedStruct` from
  the pipe, copied out by the *Call Endpoint* CustomThunk getter.
* **On Socket Request** (inbound, `InRequest` messages): typed `Request` output and a `Reply` node
  bound to it.
* **Emit Socket Message**: typed payload from the declaration, or wildcard.
* **Socket Request**: typed request input; On Reply (typed), On Error.

Designer flow: BeginPlay → *Connect Socket* [Chat] → **On Ready** → *Socket Request join* → …;
elsewhere *On chat_message* → widget; a button → *Emit send_chat*; *Get Socket* → bind
*On Retry Scheduled* and *On Disconnected* for a banner.

---

## 11. API asset integration

Core gets one class (ruling R5, review item 18): `UVaRestApi::Sockets` is an array of instanced
`UVaRestApiSocketConfig` objects (abstract, EditInlineNew, in `VaRestHttp`), plus optional
`Environments[].SocketBaseUrl`. `VaRestWebSocket` defines the concrete `UVaRestSocketEndpoint` and all
its structs. Rev 2 (§22 SW-10): the connection fields live in `UVaRestSocketEndpointBase` (`Name`,
`Description`, `Path`, `Query`, `Headers`, `Origin`, `bNoAuth`, overrides for reconnect, protocol ping,
limits, queue and background); `UVaRestSocketEndpoint` adds `Protocols`, routing, `ReadyRoute`,
`Messages`, the WS auth placement and the app-heartbeat override; `VaRestSocketIO`'s
`UVaRestSioEndpoint` subclasses the base, not `UVaRestSocketEndpoint`. Designers press "+" and pick
*VaRest Socket* (or *Socket.IO Socket*).

`UVaRestSocketEndpoint` fields: `Name`, `Description`; `Path` (`/ws/chat`, resolved against
`SocketBaseUrl`, else `BaseUrl` with the scheme mapped; or an absolute URL); `Query`, `Headers`,
`Protocols`, `Origin`; `bNoAuth`, auth placement; routing (§7.3) and `ReadyRoute`; `Messages`
`{Route, Direction (In / Out / Request / InRequest), PayloadType, ReplyType, Description}` — they drive
the typed K2 nodes; overrides for reconnect, ping, heartbeat, limits, queue and `Background` policy.

`IsDataValid` (delegated by the asset to each entry): duplicate names, malformed paths/URLs, duplicate
routes per direction, `Request`/`InRequest` without `IdField`, outbound fields that are not top-level,
header names/values that fail §2.2. `SetEnvironment` restarts open sockets of that asset
(`Restart`, via `UVaRestHub::OnEnvironmentChanged`, §19). The OpenAPI importer does not fill `Sockets`
(OpenAPI has no socket model; AsyncAPI could, later).

---

## 12. Observability

**Logging** (`LogVaRestSocket`; level per socket, default from settings; core cvar `VaRest.LogLevel`):

| Level | Logs |
| --- | --- |
| `Basic` | lifecycle: connecting (redacted URL, attempt), open (ms, protocol), closed (code, reason, clean, by), retry scheduled, gave up |
| `Headers` | + handshake headers (redacted) |
| `Body` | + every frame: direction, kind, bytes, route, excerpt (4 KiB, JSON fields redacted, binary as hex of 64 bytes); compiled out in Shipping |

Redaction is the core's (core §4.5). LWS's own log lines are a process-wide setting (§1.2). When
VaRest sets it, only Error/Warning lines are emitted, to `LogVaRestSocket`. When the engine's
`WebSockets` module set it, every level but parser is emitted to `LogWebSockets`, warnings always and
the rest only at Verbose (`LwsWebSocketsManager.cpp:174-176`, `:403-424`); in builds where both share
one LWS copy, that Verbose output can include VaRest handshake lines. The docs say: a project that
places tokens in the query must not enable `LogWebSockets` Verbose in builds that handle real
credentials; VaRest warns once if it sees that combination.

**Trace tap.** Sockets emit to the core's `IVaRestTraceSink`s with `Source = Socket`, a socket id
and an attempt id: `SocketConnecting`, `SocketOpen`, `SocketFrame` (emitted **after** decode: direction,
kind, bytes, channel, route, request id, excerpt in editor/dev builds only), `SocketClosed`,
`SocketRetryScheduled`, `SocketGaveUp`, `SocketPing` (RTT). Rev 2 (§22 A4): `SocketFrame` also carries a
protocol tag set by the codec (e.g. `eio:ping`, `sio:EVENT`) and an attachment count, and `SocketChannel`
events report a channel going up, down or refused (reason, recovered). The Inspector shows a socket as one
long-lived row with its frame list.

**Amendment (Inspector rev 1, 2026-10-20).** The event names above are superseded by Inspector spec §9.3
(IW-1–IW-6), which is the trace contract for W5. There are four socket events:

* `Socket` (Created / Released).
* `SocketAttempt` (Connecting / Open / Closed). It absorbs `SocketConnecting`, `SocketOpen` and `SocketClosed`.
  `SocketRetryScheduled` becomes `Closed.bWillReconnect` + `Delay`, and `SocketGaveUp` becomes `Closed.bGaveUp`.
* `SocketFrame`, same name, adding `FrameId`, `MsgKind`, `bAutoReplied`, the lane and the times.
  * The inbound excerpt is inline, redacted on the socket's pipe.
  * The outbound excerpt is taken at hand-off and delivered as a core `Excerpt` event, keyed by `FrameId`.
* `SocketNote`: `SendDropped`, `RequestEnded`, `Flow`, `Backpressure`, `Channel` (was `SocketChannel`) and
  `Ping` (was `SocketPing`).

Rules for all four:

* Their payload types are `VaRestHttp` types (`FVaRestTraceSocketClose`, small enums, `FName`s), and WS maps its
  own types onto them.
* All are emitted on the game thread.
* Sockets contribute to the late-attach snapshot (`OnAttached`, Inspector IN-12).
* Auth-placement fields join the socket's redaction keys, and the `FirstMessage` token joins the known-secret
  set (Inspector IN-13).

**Stats.** `stat VaRest` gains: sockets open, attempts, reconnects, frames and bytes in/out, buffered,
queued, average RTT. `FVaRestSocketStats` per socket.

**Console** (not in Shipping): `VaRest.Socket.List`; `VaRest.Socket.Drop <Name>` (kills the
connection as an unclean drop, to test reconnect UI).

---

## 13. Connectivity and app lifecycle

* **Offline.** While the hub is offline, waiting sockets keep attempting at the `MaxDelay` cadence;
  these attempts do not count toward `MaxAttempts`, and each acts as a probe: a socket that opens
  reports online (`ReportOnline`). So a socket-only game state (in a match, no HTTP traffic) cannot
  wait forever behind a hub that never probes (review item 13). Back online: waiting sockets attempt
  after 0–1 s of jitter; open sockets send a protocol ping at once (a network switch kills old TCP
  paths silently).
* **Sockets never report offline.** A down chat server must not queue every HTTP request.
* **Background** (`EVaRestSocketBackground`): `Disconnect` (default on Android/iOS) or `Keep`
  (default elsewhere). `Disconnect`: on `ApplicationWillEnterBackgroundDelegate`
  (`Runtime/Core/Public/Misc/CoreDelegates.h:358`) close 1001 "background" and wait
  (`WaitReason = Background`); on `ApplicationHasEnteredForegroundDelegate` (`:361`) attempt at once.
  The backlog survives. Why: iOS suspends the process, and no game tick runs while backgrounded
  (`Runtime/Core/Private/Containers/BackgroundableTicker.cpp:30-35`, `Runtime/Launch/Private/IOS/LaunchIOS.cpp:662-682`),
  so the server should drop presence at once. With `Keep`, our service thread keeps pinging where the
  OS lets it run (Android), flow control bounds memory (§2.3), and foregrounding sends a ping before
  trusting the link. Lifecycle callbacks are re-dispatched to the game thread if needed.

---

## 14. Errors

Errors reuse `FVaRestError` (core §3.3); close details travel in `FVaRestSocketClose`.

| Situation | `Kind` |
| --- | --- |
| connect failure (DNS, TCP, TLS), connect/ready/ping timeout | `Connection` / `Timeout` |
| upgrade refused | `Http` (`HttpStatus`) |
| refresh failed, 4401 after refresh, 4403 | `Auth` |
| server close codes that stop (§3.4), error reply to a request | `Server` |
| invalid UTF-8, oversize inbound, decode failure | `Decode` |
| bad URL/header/origin/protocol/close code, oversize send, `Emit` misuse | `Config` (never retried) |
| backlog drop/reject, above `MaxBufferedBytes`, too many pending requests | `Offline` |
| `Close()`, `StopReconnecting()`, shutdown, owner gone, released | `Canceled` |

`FVaRestSocketClose` (BlueprintType): `Code` (1005 when the peer gave none, 0 before any open),
`Reason` (UTF-8), `ReasonJson` (parsed when the reason is a JSON object — dawg's servers put
`{Code, Status, Message}` there), `bWasClean`, `By` (`Client`, `Server`, `Transport`, `Heartbeat`,
`Timeout`), `HttpStatus`, `bWillReconnect`, `ReconnectDelay`, `Attempt`.

---

## 15. Limits and defaults

| Setting | Default | Why |
| --- | --- | --- |
| `MaxMessageBytes` (inbound) | 1 MiB | the engine's default; bounds hostile input |
| `MaxPendingInboundBytes` | 16 MiB | flow-control threshold (§2.3) |
| `MaxSendBytes`, `MaxBufferedBytes`, `SendHighWater` | 16 MiB, 16 MiB, 256 KiB | §6.3 |
| `MaxQueued` / `MaxQueuedBytes` | 256 / 1 MiB | §6.2 |
| `PingInterval` / `PingTimeout` | 20 s / 10 s | §5 |
| `MaxPendingRequests`, request timeout | 1024, 10 s | |
| `ConnectTimeout`, `ReadyTimeout`, `CloseTimeout` | 10 s, = connect, 5 s | |
| Handshake header buffer | 32 KiB (context), checked per header | §2.2 |
| Fragment size for sends | 64 KiB | §2.4 |
| Close reason | 123 bytes | RFC 6455 §5.5 |
| Reconnect | 0.5 s × 2 → 30 s, full jitter, unlimited, stable after 5 s | §3.4 |
| App heartbeat (when on) | 20 s / 10 s | §5 |

In code: `FVaRestSocketLimits`, `FVaRestSocketQueuePolicy`, ping fields. `UVaRestSocketSettings`
(DeveloperSettings, *Project Settings ▸ Plugins ▸ VaRest WebSocket*) holds the project defaults,
including `bAllowSelfSignedTls` (ignored in Shipping).

---

## 16. Verification

### 16.1 Unit tests (`VaRest.WebSocket.*`)

On `FVaRestFakeSocketTransport` (scripts each attempt's server side and every transport event,
including events generated inside a dispatch), the core's virtual clock with a seeded RNG, and a
synchronous decode executor:

`State` (every state × event pair, re-entrancy deferral), `Reconnect` (timing, jitter, stable reset,
max attempts, precedence, every classification row), `Handshake` (per-attempt headers/auth/query,
validation, origin), `Send` (lanes, readiness, `ReadyTimeout`, volatile, overflow, batches,
off-thread contract), `Receive` (ordered delivery incl. lifecycle events, close-then-pending-decode),
`Route`, `Request` (both directions), `Close` (codes, truncation, generation bump), `ConnectError`
(every string in §2.2), `Heartbeat` (app heartbeat, hitch re-arm).

### 16.2 Transport tests (`VaRest.Integration.WebSocket.Transport.*`)

`FVaRestLwsTransport` against the test server, ws and wss: handshake (headers, origin, protocols,
selected protocol), fragmented and large text (Cyrillic split inside a code point and at 4096 bytes),
binary, interleaved text/binary order, invalid UTF-8 → 1007, size limit → 1009, empty close → 1005,
close codes and UTF-8 reasons, unclean drop → 1006, ping RTT and silent-server death, connect timeout,
close against a non-echoing server, flow control (slow consumer), backpressure and `Drained`,
fragmented sends, allowlist, upgrade refusal statuses.

### 16.3 Test server (`Tools/testserver/`, Python 3 stdlib)

`Tools/testserver/ws.py`, plugged into the core's HTTP test server: the `ThreadingHTTPServer` handler
sees `Upgrade: websocket`, answers 101, takes over `self.connection`/`self.rfile`, and sets
`close_connection`. One process and port; login/refresh/token state shared. `ws.py` runs alone too;
since `python3 -I` drops the script directory from `sys.path`, it inserts its own directory before
importing siblings. A **TLS listener** uses stdlib `ssl` with a committed self-signed test certificate
(`Tools/testserver/certs/`, test-only), reached with `bAllowSelfSignedTls` or `wss+insecure`.

Hand-written RFC 6455 (≈300 lines): handshake (`Sec-WebSocket-Accept` = base64(SHA-1(key +
`258EAFA5-E914-47DA-95CA-C5AB0DC85B11`)), subprotocol selection, refusal statuses); frames with 7/16/64-bit
lengths, mandatory client masking (big-integer XOR unmask), continuation assembly, control frames,
PING → PONG, close echo, strict UTF-8 → 1007; server frames optionally fragmented at chosen offsets.

Scenarios: `/ws/echo`; `/ws/status/<code>`; `/ws/close/<code>?msg=…` (optional message first);
`/ws/close/none` (empty close frame); `/ws/close/1012`; `/ws/drop`; `/ws/silent` (never answers
anything, including PINGs — for dead-link tests); `/ws/apping`; `/ws/serverping`; `/ws/route`
(pushes, replies by id, error replies, server → client requests); `/ws/fragment?at=…`;
`/ws/large?bytes=N&ascii=0|1`; `/ws/order?n=…` (long alternating text/binary loop); `/ws/headers`
(first message = the handshake seen); `/ws/auth`; `/ws/protocol?select=<p>`; `/ws/flaky?n=`;
`/ws/slowupgrade`; `/ws/noecho` (never echoes our close); `/ws/badutf8`.
`Tools/testserver/ws_selftest.py` checks the server with a stdlib client before UE tests rely on it.

### 16.4 W0 probe: LWS 3.0 specifics

A minimal LWS harness (no VaRest client code) records into `docs/passport/<date>-lws30-probe.md`:
`local_protocol_name` with an unregistered server-selected subprotocol (§2.2); the selected protocol
via `FILTER_PRE_ESTABLISH`; `lws_http_client_http_response` inside `CONNECTION_ERROR` for 401, 403,
404, 503 vs the strings; empty close frame and codes 1004, 1012, 1013, 1014, 2000, 4001, 5000 as
received (rewriting?); close echo timing against `/ws/noecho`; `CLIENT_CLOSED` vs `WSI_DESTROY` order;
auto-PONG to server PINGs; `RECEIVE_PONG` payload echo; receive chunking (fragment boundaries, 64 KiB)
and `NO_FIN` sends; default `timeout_secs` (blackhole address) and `LWS_TO_KILL_ASYNC`; raw origin;
the `APPEND_HANDSHAKE_HEADER` length with a 32 KiB buffer; `rx_flow_control` pause/resume; TLS through
the `SSL` module context on Linux; redirects (3xx); coexistence with an engine `IWebSocket` (editor
and monolithic, ws and wss, both logs intact). The spec is corrected where the probe disagrees.

### 16.5 Restore-the-bug targets

Each seen failing with the behaviour broken, then passing restored:

1. Per-attempt handshake (reuse attempt 1's headers) → refreshed-token test fails.
2. Re-entrancy deferral (run `Connect` inline from `OnMessage`) → the dispatch-depth `check` fires.
3. `StableAfter` (reset on open) → accept-then-close loop shows no growing delay.
4. Generation guard (accept any attempt's events) → a late open from an abandoned attempt flips state.
5. Empty close = 1005 (read two bytes regardless) → `/ws/close/none` reports garbage.
6. Byte assembly (validate or convert per chunk) → split-code-point Cyrillic test corrupts.
7. One ordered path (deliver close outside the queue) → "large decoding, then server close" test sees
   `OnDisconnected` before `game_over`.
8. Ready lane (flush backlog before `OnReady` sends) → order test fails.
9. Inbound size limit (skip the check) → oversize test exceeds the limit.
10. Protocol ping (never arm the deadline) → `/ws/silent` test hangs to its deadline.
11. Request fail-on-disconnect (keep pending) → test waits for the timeout.
12. Offline probing (gate attempts fully) → socket-only reconnect test stalls.
13. Close → Connect (keep a closing state that ignores `Connect`) → button test fails.
14. Handler owner gone (strong capture) → handler runs after destruction.
15. App-heartbeat hitch rule (check before drain) → stalled-game-thread test kills a live link.
16. Flow control (never pause) → slow-consumer test exceeds `MaxPendingInboundBytes`.

### 16.6 Builds and performance

Linux editor, monolithic Development and Shipping; packaged Linux run of the chat sample;
Win64/Mac/Android/iOS — including TLS and platform root certificates — in the release pass (bead
VaRest-b4d). `Bench.VaRest.WebSocket.*`: game-thread cost per small routed message (target < 5 µs),
throughput of 10 000 small messages, memory per idle socket (< 4 KiB of VaRest state), packaged size
delta per platform.

---

## 17. Milestones

| W | Content | Depends on | Exit |
| --- | --- | --- | --- |
| W0 | `ws.py` + TLS listener + selftest; LWS 3.0 probe harness; passport | — | probe recorded; §0.3/§2 corrected where needed |
| W1 | **Transport**: module skeleton and Build.cs, context + service thread, command/event queues, connect (allowlist, URL, origin, protocols, headers, TLS, proxy, timeouts, status), receive (assembly, UTF-8, limits, flow control), send (LWS_PRE encode buffers, fragments, buffered amount), ping/RTT, close semantics; `IVaRestSocketTransport` + fake; rev 2: frame payload offset (SW-4), auto-replies with priority, `SO_SNDBUF`, idle deadline, deadline suspension (SW-5), `NoUpgradeResponse` (A2) | W0; core M3 test harness | transport suite (§16.2) green on Linux ws + wss; editor coexistence; monolithic build; restore-the-bug 5, 6, 9, 10, 16 |
| W2 | Client layer: `FVaRestSocket`, state machine (full table), attempts, generation guard, re-entrancy deferral, reconnect policy + classification, close | W1; **core M4** (clock, timers, backoff) | `State`/`Reconnect`/`Close` green; restore-the-bug 1–4, 13 |
| W3 | Sending (lanes, readiness, backlog, backpressure, thread contract), ordered delivery + decode pipe, app heartbeat, logging/redaction; rev 2: closing hook (SW-7), any-thread pinned `SendBatch` with flush-time send limit (SW-9) | W2 | `Send`/`Receive`/`Heartbeat` green; restore-the-bug 7, 8, 15 |
| W4 | Codec, routing, channels, typed handlers, requests both ways, attachments, stages API (§8); rev 2: message `Kind` + int64 ids (SW-1), codec contract v2 (SW-2), channel lifecycle + inbound hold (SW-8) | W3; VaRestJson items §19 | `Route`/`Request` green; restore-the-bug 11, 14 |
| W5 | `UVaRestSocketHub`, session/auth, connectivity + lifecycle, trace/stats/console, asset entries + validation, `UVaRestSocketClient`, async nodes, K2 + spawner, settings; rev 2: endpoint base + `UVaRestSocketComponent` (SW-10), declarations with a channel (SW-11), session kinds (A1), protocol lookups refused (A3), trace tag + channel events (A4) | W4; **core M5** | BP chat sample end-to-end; two PIE clients isolated; restore-the-bug 12 |
| W6 | Docs (README section; note for v1 users of raw `IWebSocket`), sample, monolithic/Shipping/packaged runs, benchmarks, size measurement | W5 | builds green; numbers in the passport |

Each W gets its own plan in `docs/superpowers/plans/`, written when the previous one lands. W0 and
W1 need no core code beyond the test-server harness, so they can proceed while core M4 is built.

---

## 18. Risks

* **TLS and certificate glue on four platforms we cannot test here.** The glue mirrors the engine's
  shipped code line for line (§2.2), but Win64/Mac/Android/iOS are only verified in the release pass.
  Mitigation: W1 keeps the TLS code a copy of the engine's with citations; a release-pass checklist
  item per platform (ws, wss, pinned key, proxy).
* **LWS 3.0 behaviour we inferred from 4.3** (§0.3 list). Mitigation: W0 probe before W1; the
  fallbacks are written down (protocol registration at context creation, string status parsing,
  1002 handling).
* **Owning a transport** means owning its bugs: one more milestone of work and a thread to maintain.
  Mitigation: the transport is small (one context, one protocol, client only) and fully covered by
  §16.2 against a server we control.
* **Two LWS copies in modular builds** (§1.2). Mitigation: same as four existing engine modules;
  W0 coexistence test; the log-level rule.
* **Mobile lifecycle** varies per OS version. Mitigation: device checks in the release pass; `Keep`
  as an escape hatch.
* **Core interfaces not yet built** (M4/M5). Mitigation: §19, one tight list for those plans.

---

## 19. Minimal additions to core M2–M5

For the core plans (one list; nothing else in core changes):

1. **Clock and backoff (M4):** `IVaRestClock` with timers, the backoff formula and an injectable
   `FRandomStream`, public in `VaRestHttp`.
2. **Auth and session (M4):** `IVaRestAuth::Apply(FVaRestAuthTarget&)` (headers + query) and a public
   single-flight `Refresh()`; client events `OnSessionCleared` (logout, distinct from a refresh) and
   a callable `NotifySessionExpired()`. Rev 2 (§22 A1): `OnSessionChanged(EVaRestSessionChange)` with
   `Refreshed` (same identity, new token), `Replaced` (another identity: `SetBearerToken`/`ImportSession`
   with a different subject, or a login while logged in) and `Cleared` (logout).
3. **Redactor (M3):** public URL, header and JSON-excerpt redaction.
4. **Trace (M4):** `FVaRestTraceEvent` carries `Source` (`Http`, `Socket`) and connection/attempt ids;
   sinks ignore types they do not know.
5. **Hub (M5):** `IsOnline`, `OnConnectivityChanged`, `ReportOnline()`, `OnDeinitializing`,
   `OnEnvironmentChanged(UVaRestApi*)`, and asset → `FVaRestClient` lookup from another module.
6. **Envelope (M4):** public evaluator (`FVaRestEnvelope` applied to an `FVaRestJson` → data or error).
7. **Asset (M5):** `UVaRestApi::Sockets` as instanced `UVaRestApiSocketConfig` (abstract), plus
   `Environments[].SocketBaseUrl`; `IsDataValid` delegates to entries.
8. **Spawner (M5):** the *Call Endpoint* action spawner takes other entry kinds (socket messages).
9. **VaRestJson (M2):** struct encode into an open RapidJSON writer (members only), for one-pass `Emit`.
10. **VaRestJson (M2):** a decode-option resolver for `TArray<uint8>` fields (attachments).
11. **VaRestJson (M2):** export the byte-level UTF-8 validator of core §2.2.
12. **VaRestJson (M2, rev 2, Socket.IO J-1):** an encode-option hook `BytesWriter` for `TArray<uint8>` values
    (the counterpart of item 10), and public writer entries `WriteJsonInternal`, `WriteStructInternal`,
    `WritePropertyInternal` next to item 9's members-only entry; exact signatures in
    `.superpowers/sdd/sio-m2-gap.md`.

---

## 20. Decisions made without the owner

`R` = controller ruling (binding); others are this spec's.

| # | Decision | Why | Cost if wrong |
| --- | --- | --- | --- |
| D1 | `VaRestWebSocket` module; core gets one abstract asset class (R5) | layering; the concrete config lives with its behaviour | none significant |
| D2 | **R1: own transport on the engine-bundled libwebsockets 3.0**, mirroring the engine glue; engine `IWebSocket` backend not in 2.0 | the engine transport corrupts text (E9), reorders (E12), misreports closes (E14, E27) and can crash in Shipping (E25) | one extra milestone; TLS/cert glue to verify on four platforms in the release pass |
| D3 | One LWS connection per attempt, fresh headers each time | per-attempt auth | none |
| D4 | Unnamed sockets owned by handles, named by the hub, BP by world context; no static `Make` | ownership by shape; no PIE leaks | users of named sockets must `Release` explicitly |
| D5 | Asset entries are instanced `UVaRestSocketEndpoint` objects with typed message declarations | one asset per backend; typed BP nodes; Socket.IO subclasses | asset UI one level deeper |
| D6 | Reconnect on, unlimited, 0.5 s × 2 → 30 s, full jitter, stable after 5 s | "it just reconnects" (socket.io-client is unlimited too) | server load during long outages; tune defaults |
| D7 | **R3: server 1000 reconnects by default** (veto in `OnDisconnected`, `bStopOnNormalClose`); classification on (who, code) with a final "reconnect" row | a false stop is silent and permanent; a false reconnect costs one attempt | servers using 1000 for "go away forever" need the option or 4xxx |
| D8 | **R3: protocol ping on by default, 20 s / 10 s**; app heartbeat opt-in | zero-config dead-link detection without a server contract | a few bytes per 20 s per socket; servers that never answer PINGs (RFC violation) get dropped |
| D9 | One ordered event queue per socket; bytes assembled; UTF-8 validated once; invalid → 1007 | exact delivery; RFC 6455 §8.1 | a lenient-server ecosystem sending bad UTF-8 loses those connections |
| D10 | Decode always on the socket's pipe at arrival; no game-thread parse | the core threading rule holds; no added latency (§7.1) | one task per message burst |
| D11 | Three send lanes (bypass, ready, backlog); `ReadyTimeout` | handshake vs join-first protocols; no stuck "connected, never ready" | protocols wanting another order use stages |
| D12 | Real backpressure (buffered amount, high-water, `OnDrained`) and rx flow control | the transport knows exact bytes | none known |
| D13 | Limits per §15 | bounded memory everywhere | big-payload games raise settings |
| D14 | Routing default `type`, whole message as data, byte-exact; numbers by canonical text | most common shape; JSON is case-sensitive | dawg-style `Type` needs one setting |
| D15 | Per-socket string ids; fail on disconnect; inbound requests with at-most-once replies | no id collisions; replies cannot cross connections | servers needing numeric ids set `IdFormat` |
| D16 | Reuse `FVaRestError`; close info in `FVaRestSocketClose` | one error type | none |
| D17 | Offline: keep probing at `MaxDelay`, uncounted; report only "online" | no deadlock without HTTP traffic; chat outages do not take HTTP offline | some extra attempts while truly offline |
| D18 | Mobile: disconnect on background, reconnect on foreground | iOS suspends; presence stays truthful | rejoin cost after short app switches; `Keep` available |
| D19 | Logout closes; token refresh does not reconnect | servers authenticate at handshake | servers re-checking tokens mid-session need a close-code rule |
| D20 | Python stdlib test server shared with HTTP, plus a TLS listener with a committed test cert | real tokens; wss covered | none |
| D21 | Log levels Basic/Headers/Body as §12; LWS library logs Error/Warning when VaRest owns the setting, a one-time warning when the engine's Verbose setting meets query tokens | no flooding; no tokens in library logs by default | frame one-liners only via the Inspector |
| D22 | Off-thread API limited to `Send`/`SendBinary`/`Emit` with config snapshots | a clear contract by shape | worker-thread requests must hop to the game thread |
| D23 | Header validation (RFC 7230 names, visible-ASCII values, per-header space check) | E19 cannot recur; no mangling | servers wanting UTF-8 header values cannot get them |
| D24 | Subprotocols via `local_protocol_name`, no ini step; fallback: register declared protocols at context creation | E17 removed | if W0 disproves both, code-only subprotocols need a declaration |
| D25 | URL sugar `http(s)`→`ws(s)`; paths relative to the environment | one asset, one environment switch | none |
| D26 | `Close()` takes effect at the call; no public `Closing` state | "Close → Connect" works | the server's final close reply is not reported |
| D27 | Channels (`FVaRestSocketChannel`) in 2.0 | Socket.IO namespaces without a retrofit | a small API surface plain sockets ignore |
| D28 | **R4: one plugin**; module split for hygiene, not size | simpler distribution | ≈150 KB of LWS code in HTTP-only games (more on Apple if OpenSSL is not otherwise linked) |
| D29 | Default `Origin` derived from the URL (raw), overridable or omitted | Origin-checking servers work | servers rejecting any Origin from native clients set `WithOrigin({})` |
| D30 | `lws_set_log_level` only when the engine `WebSockets` module is not loaded; never `lws_set_allocator` | process-global LWS state is never hijacked | LWS allocations are not in VaRest's LLM tag |
| D31 | `FirstMessage` auth placement | common "auth as first message" servers | none |
| D32 | Ping deadlines and connect timeouts on the service thread | immune to game-thread hitches | none |

---

## 21. Provenance: DwtWebSoN and dawg-code

Reference copies are in the session scratchpad (`…/scratchpad/ref/DwtWebSoN`, `…/scratchpad/ref/dawg-code`),
not in this repository; ideas reused, not code:

| Taken | From | Changed |
| --- | --- | --- |
| Messages routed by a `Type` field with a `Data` payload; typed per-event registry with tolerant decode | `dawg-code/Dawg/Private/Network/DwSocketClient.cpp:247-288`; `DawgCore/Public/Transactions/DwtAbstractWebTransactionManager.h:86`, `.cpp:42-53` | configurable paths, case-sensitive, decode off the game thread, unrouted logged once per value |
| App keepalive with a server ack; disconnect when an ack is missing | `DwSocketClient.cpp:290-314` | protocol ping by default with RTT and its own timeout; app heartbeat opt-in |
| A handler can veto auto-reconnect (`bool& bProcessed`) | `DwSocketClient.h:73-79`, `.cpp:185-245` | `StopReconnecting()` from `OnDisconnected`, `ReconnectIf`, a classification table |
| Close reason parsed as JSON `{Code, Status, Message}` | `DwSocketClient.cpp:212-228` | `FVaRestSocketClose::ReasonJson` |
| Close-code names | `DwSocketClient.h:15-71` | `Describe Socket Close` |
| `?token=` auth, `http`→`ws` mapping | `DwSocketClient.cpp:27-39`, `:105-107` | configurable placement, token from the shared session per attempt |
| Outbox queued until open, flushed on open | `DwtWebSoN/Source/DwtWebSoN/Public/DwtWebSoNWsppConnection.h:174-213` | bounded, keeps the frame kind (`Flush` re-sends everything as binary, `:209`), lanes |
| Own transport behind an interface | `DwtWebSoNSocket.h:10-12` (`FDwtWebSoNSocket : IWebSocket` over websocketpp) | our seam is `IVaRestSocketTransport`; our backend is the engine-bundled LWS, not a second WebSocket library |

Not taken: fixed 3 s reconnect without backoff or jitter (`DwSocketClient.cpp:13`, `:159-172`);
`Fatal` on a missing token (`:99-103`); ignored upgrade headers and TLS chosen from the subprotocol list
(`DwtWebSoNSocket.cpp:7-18`); `FString(Msg.c_str())` for received UTF-8 (`DwtWebSoNSocket.h:79`);
no reconnect on clean server closes such as 1012 (`DwSocketClient.cpp:205-207`); closing only
`IsConnected()` sockets, which leaves a connecting one to open later (`DwSocketClient.cpp:150-153`).

---

## 22. Revision 2 (2026-10-20): hooks for Socket.IO

Source: `2026-10-20-varest2-socketio-design.md` revision 1 (§18) after its adversarial review
(`.superpowers/sdd/sio-spec-review.md`, §C) and Socket.IO ruling R3 (the reduced set). The SW numbers are
the Socket.IO spec's; SW-3, SW-6 and SW-12 of its first draft are merged, replaced and folded as noted.
Everything here keeps the §4 threading rule (control on the game thread, data off it, nothing parsed on
the game thread) and the §7.1 single ordered path. The rev 1 passages that said otherwise are fixed in
place and marked "rev 2".

### 22.1 Hooks

| # | Hook | Lands in | Tests |
| --- | --- | --- | --- |
| SW-1 | **Message kinds and numeric ids.** `FVaRestSocketMessage::Kind` (`Message`, `Request`, `Reply`, `ErrorReply`, `Control`), set by the codec (the default codec keeps today's rules). Pending requests resolve only from `Reply`/`ErrorReply`; `OnRequest` receives only `Request`. Codecs with numeric ids take them from one socket-wide `int64` counter on the game thread (requests are game-thread only, §6.1); the pending table is keyed by id and checks the reply's channel. (§7.5, §8) | W4 | `Request`: an EVENT-kind message with a pending id is routed, not consumed; channel mismatch dropped |
| SW-2 | **Codec contract v2** (Socket.IO draft SW-2 + SW-3). `IVaRestSocketCodec::MakeDecoder()` → `IVaRestSocketDecoder`, created when the connection's Open event passes through the pipe and destroyed with its Closed event, used only on the pipe. `Decode(Frame)` returns `Message`, `NeedMore` (frame consumed, multi-frame message incomplete) or `ProtocolError(Reason)` — closed 1002 through the normal path, and the decoder is poisoned: it drops the rest of that connection's frames. Typed decode is a type-erased plan registered by the handler adapter; a plan may decode several positions of an array `Data` and bind attachments (VaRestJson `BytesResolver`, §19 item 10). WS's single-`T` handlers are one plan kind. (§1.3, §7.2, §8) | W4 | `Receive`: NeedMore across frames, poisoned decoder, decoder per attempt |
| SW-4 | **Frame payload offset.** `FVaRestSocketFrame` = storage + offset (≥ `LWS_PRE`) + length; the transport writes from the offset, so an encoder can reserve a header gap and write a variable-length prefix after the body. (§2.4) | W1 | transport: frames with offsets > `LWS_PRE` |
| SW-5 | **Service-thread auto-replies and deadlines.** (a) Connect options carry exact-bytes rules for inbound **text** frames (`"2"` → `"3"`), applied on the service thread for that connection; the frame is still delivered, flagged `bAutoReplied`. (b) A reply is a data message: written at the next message boundary ahead of every queued data message, never inside a fragmented message (RFC 6455 §5.4 permits only control frames there), outside buffered amount and backpressure, never dropped. (c) Connections with rules get `SO_SNDBUF` 256 KiB. (d) `Ctx.SetIdleDeadline(Seconds)`: no inbound data frame (text or binary; PONGs do not count) within it → killed, `By = Heartbeat`, measured on the service thread (D32). (e) The idle deadline and the protocol-ping deadline are suspended while VaRest has paused reading (§2.3); a connection that ends meanwhile carries `FVaRestSocketClose::bWhileReadPaused`. (§2.4, §2.5, §5) | W1 | transport: reply during a 16 MiB fragmented send arrives at the next boundary; slow-consumer test does not trip deadlines |
| SW-7 | **Closing hook.** `IVaRestSocketStage::OnClosing(Ctx, const FVaRestSocketCloseRequest&)`, called synchronously by app-initiated closes (`Close()`, logout, owner gone, hub shutdown, `Release`) and by session-replacement restarts (A1); not by drops, background, other `Restart`s or attempts abandoned before open. Frames sent there with `Ctx.SendBypass` precede the close frame, bounded by `CloseTimeout`. (§3.5, §8) | W3 | `Close`: bypass frames before the close frame; none on drop or background |
| SW-8 | **Stage-driven channel lifecycle and inbound hold.** `Ctx.ChannelUp(Channel, FVaRestSocketOpen)` = `SetReady` + the channel's `OnConnected`/`OnReady`. `Ctx.ChannelDown(Channel, FVaRestSocketClose, EVaRestChannelEnd {WillRetry, Ended})` fails the channel's sent requests (`Connection`, retryable) and fires its `OnDisconnected`; `Ended` also cancels its backlog. `Ctx.HoldInbound(Channel)`: inbound messages for a held channel queue in arrival order (bounded; overflow → a stage callback) and are dispatched right after the channel's `OnReady` handlers and backlog flush. (§6.2, §8) | W4 | `Send`/`Receive`: channel down without socket close; held messages after `OnReady` |
| SW-9 | **Any-thread batches.** `SendBatch` callable from any thread: one staging entry, queued, dropped and written as one unit and back to back (only control frames and SW-5 replies between its frames); an optional pin (connection generation, channel epoch) drops the batch at flush if either changed; the send limit (`Ctx.SetSendLimit`) is enforced at flush for every queued frame; `EncodeReply` returns a batch through this path. (§6.1) | W3 | `Send`: off-thread batch contiguity; stale pin dropped; limit re-checked at flush |
| SW-10 | **Endpoint base class and component base.** `UVaRestSocketEndpointBase` (connection fields) ← `UVaRestSocketEndpoint` (`Protocols`, routing, `ReadyRoute`, `Messages`, WS auth placement, app-heartbeat override) and ← protocol entries such as `UVaRestSioEndpoint`; asset-level session wiring reads the placement through a virtual on the base. `UVaRestSocketComponent` (`BlueprintSpawnableComponent`): `Api` + `Entry` (uses the entry's hub-named socket, shared with *Get Socket* and the typed nodes; `bCloseOnEndPlay`) or `Url` with `Scope` (`Actor`: unnamed, closed on `EndPlay`; `GameInstance`: hub-named), `bAutoConnect`, `BlueprintAssignable` lifecycle events, `GetClient()`; protocol modules subclass it. (§10.1, §11) | W5 | asset validation per subclass; component BeginPlay/EndPlay per scope; level travel keeps a `GameInstance` socket |
| SW-11 | **Declarations with a channel; protocol-supplied pins.** K2 identity `{Asset, Entry, Channel, Route}`; spawner path *Asset ▸ Entry ▸ [Channel ▸] Route*; the declaration nodes ask the entry's module (`IVaRestSocketDeclarationProvider`) for extra pins (Socket.IO: `Ack`, args mode, `bOnce`). (§10.3, §19 item 8) | W5 | K2 reconstruction with a channel |

Not adopted: **SW-6** (flow control excluding incomplete messages) — replaced by a configuration invariant
on the protocol side (Socket.IO: `MaxPacketBytes ≤ MaxPendingInboundBytes / 2`), so §2.3's counting is
unchanged. **SW-12** (Socket.IO trace items) — folded into the generic trace work of A4.

### 22.2 Other amendments

| # | Amendment | Where | Lands in |
| --- | --- | --- | --- |
| A1 | Session change kinds `Refreshed` / `Replaced` / `Cleared` (core §4.2 via §19 item 2); `Replaced` restarts open sockets with the closing hook; `ResetSession()` on the socket for opaque `WithBearer` lambdas | §3.3, §19 item 2 | core M4, W5 |
| A2 | Transport failure reason `NoUpgradeResponse` (upgrade sent, closed before any status line); default row "reconnect"; W0 pins the LWS 3.0 error string | §2.2, §3.4, §16.4 | W0, W1 |
| A3 | WS lookups (`Socket`, `Find`, BP *Get Socket*) refuse sockets and entries owned by another protocol module, with a `Config` error naming its accessor | §9 | W5 |
| A4 | Trace: `SocketFrame` gets a codec-set protocol tag and an attachment count; new `SocketChannel` events (up / down / refused, reason, recovered) | §12, §19 item 4 | W5 |
| A5 | VaRestJson for protocol codecs: `BytesWriter` encode hook and writer entries (§19 item 12) | §19 | core M2 |

W0 probe additions (§16.4): the `NoUpgradeResponse` string against a server that closes an upgrade
without answering; `SO_SNDBUF` through `lws_get_socket_fd` on Linux; an auto-reply written between two
large messages.

### 22.3 Decisions (revision 2)

| # | Decision | Why | Cost if wrong |
| --- | --- | --- | --- |
| D33 | The nine hooks above, in the milestones named | Socket.IO needs them; each is small and generic (graphql-ws, Phoenix and JSON-RPC codecs gain too) | W1/W3/W4/W5 grow slightly |
| D34 | Auto-replies jump the queue at message boundaries, outside backpressure; `SO_SNDBUF` 256 KiB on connections with rules | protocol heartbeats must survive large sends; the OS buffer is the other queue | one such connection is limited to ≈ 20 Mbit/s at 100 ms RTT |
| D35 | Client deadlines suspended while VaRest pauses reading | the pause is ours; the server decides | a link that dies during a long pause is noticed when reading resumes |
| D36 | `Replaced` sessions restart open sockets | a socket authenticated as user A must not serve user B | one reconnect per user switch |

## Appendix A. Review disposition

Review: `.superpowers/sdd/ws-spec-review.md`. "R1" = superseded by the own transport.

| # | Sev | Disposition |
| --- | --- | --- |
| 1 | Critical | **R1 + applied**: no engine `IWebSocket`, so E25 cannot occur; the rule stays — handlers never re-enter the transport or state machine, requests run after the dispatch, attempts start from VaRest's tick, non-Shipping depth `check`, restore-the-bug 2 (§3.1). |
| 2 | Important | **R1 + applied**: the transport parses the raw close payload (empty → 1005, normalisation, UTF-8 reason) and reports who closed (§2.5); classification on (who, code) with a final row; W0 probes rewriting, with the 1002 safety rule (§3.4). |
| 3 | Important | **Applied (R3)**: server 1000 reconnects; veto and `bStopOnNormalClose` (§3.4, D7). |
| 4 | Important | **R1**: one ordered queue preserves cross-kind order (§2.1, §7.1); two-FIFO pairing unnecessary. |
| 5 | Important | **R1**: bytes assembled and validated once (§2.3); the fragmented-Cyrillic test stays (§16.2, restore-the-bug 6). |
| 6 | Important | **R1**: open is an event in the same ordered queue, before any message; classification of never-opened attempts uses the same rows (§3.4). |
| 7 | Important | **Applied**: one ordered path through the pipe for messages and lifecycle events; delivery rule before/after close; injectable executor; restore-the-bug 7 (§7.1). |
| 8 | Important | **Applied**: config snapshots, off-thread API limited by shape, thread-safe handles with game-thread destruction, `Volatile` at flush, bounded staging (§6.1, §4, §1.4). |
| 9 | Important | **Applied**: full state × event table, `Restart`, logout, `StopReconnecting` in flight; `Close()` effective at the call (D26) removes the Closing trap (§3.1). |
| 10 | Important | **Applied**: three lanes, `ReadyTimeout`, joins moved to `OnReady` in §9 and §10 (§6.2). |
| 11 | Important | **Applied**: Connect Socket fires on every Ready with `Is Reconnect`; On Failed for any end before first open (§10.2). |
| 12 | Important | **Applied**: no static `Make`; `GetForEditor()`; handle-owned unnamed, hub-owned named, `Owner` refused on named; `FVaRestSocketWeak`; BP ownership by world context (§1.4). |
| 13 | Important | **Applied**: offline attempts at `MaxDelay`, uncounted, acting as probes; restore-the-bug 12 (§13). |
| 14 | Important | **Applied (R3), adapted by R1**: per-connection protocol ping on by default 20 s / 10 s with RTT, on our context — no engine ini hack needed (§5). |
| 15 | Important | **Applied, option (a)**: `FVaRestSocketChannel` (§1.3, §8). |
| 16 | Important | **Applied**: `OnRequest`, `Message.Reply`, codec `EncodeReply`, at-most-once, `InRequest` direction, BP Reply (§7.5, §10, §11). |
| 17 | Important | **Applied**: attachments on frames and messages, multi-frame `Encode`, VaRestJson resolver (§8, §19 item 10). |
| 18 | Important | **Applied (R5)**: one abstract core class; missing items added; one minimal list (§19); W2 gated on core M4, W5 on M5 (§17). |
| 19 | Minor | **Applied (R4)**: one plugin; corrected linking claim; size quantified (§1.1). |
| 20 | Minor | **R1**: raw, scheme-correct, overridable Origin; 403 message names the Origin (§2.2, §3.4). |
| 21 | Minor | **Applied, simpler under R1**: per-header space check against the real remaining length (T5), RFC 7230 names, visible-ASCII values (§2.2). |
| 22 | Minor | **Mostly R1**: VaRest URLs never pass through the engine socket API or `OnWebSocketCreated`; the process-wide LWS log setting is documented, with a one-time warning for query tokens plus `LogWebSockets` Verbose (§12). |
| 23 | Minor | **R1**: reasons decoded as UTF-8 (§2.5). |
| 24 | Minor | **Applied**: generation bump at `Close()`; no handler runs for that connection afterwards (§3.5). |
| 25 | Minor | **Applied**: W0 measures echo timing; we echo peer closes; `CloseTimeout` bounds ours (§2.5, §16.4). |
| 26 | Minor | **Applied**: 3xx and other 4xx stop, 4429 delay floor, 1008 → `Server`, explicit precedence, all strings in parser tests (§3.4, §2.2). |
| 27 | Minor | **Applied, mostly R1**: ping and connect deadlines on the service thread; app heartbeat checked after drain with one re-arm (§5, D32). |
| 28 | Minor | **Applied**: `MaxPendingInboundBytes` via flow control instead of a 1008 close; unrouted cap 256; queued requests leave the backlog at timeout (§2.3, §7.3, §7.5). |
| 29 | Minor | **Applied**: per-socket id prefix; `JsonRpc` "reply = no method"; pending overflow → `Offline` (§7.3, §7.5). |
| 30 | Minor | **Applied**: non-object `Emit` rules; numeric routes by canonical text; next-attempt scope limited to connection settings; trace frame after decode (§6.1, §7.3, §3.2, §12). |
| 31 | Minor | **Applied**: `WithApi`; constrained `Send`; spawner entry for Get Socket; `OnReconnecting` → `OnRetryScheduled`; "bypass send" naming; "once per second" defined; `FirstMessage` auth; cookie note (§9, §6, §10, §3.3). |
| 32 | Minor | **Applied**: per-kind sequences dropped; binary heartbeat preset, `MaxQueueAge` and `WithRouter` deferred (codec covers routers); the engine null transport is gone with R1. |
| 33 | Minor | **Applied**: WebSocket-only Engine.IO stated; stage session access and `RequestRefresh`; `Ctx.Close` with an explicit reconnect decision; `SetSendLimit`; encode-before-queue note (§0.5, §8). |
| 34 | Minor | **Applied**: injectable decode executor; fake engine socket replaced by the fake transport plus real transport tests; wss via a TLS listener; `ws.py` notes (`-I` path insert, handler takeover, close echo); new scenarios; restore-the-bug list extended; provenance files located (§16, §21). |
