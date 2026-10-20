# VaRest 2 — Socket.IO design

Status: revision 1, 2026-10-20 (after the adversarial review `.superpowers/sdd/sio-spec-review.md` and the
controller's rulings R1–R4). Sub-project 3 of 4 (core spec §0.3). Appendix A maps every review item to
its disposition.
Target: Unreal Engine 5.8.1, branch `v2`. Builds on `2026-10-18-varest2-core-design.md` ("core") and
`2026-10-20-varest2-websocket-design.md` **revision 2** ("WS"); § numbers prefixed with *core* or *WS*
refer to them. The WS hooks this spec needs are written into WS §22 (SW-1…SW-11), not worked around.

---

## 0. Context

### 0.1 Intent

A game whose backend is a Socket.IO server should get, from one asset entry, one actor component or one
C++ statement: connect, namespaces, typed events, acks in both directions, binary attachments, auth from
the same session as the HTTP API, reconnect with connection state recovery, and the WS layer's logging,
redaction, trace tap and stats. No Socket.IO knowledge beyond "events, namespaces, acks" is needed.

What success looks like:

* Designer: add a *Socket.IO Socket* entry to `DA_GameApi`, declare its events, drop a *VaRest Socket.IO*
  component on an actor (or use *Connect Socket.IO*), then wire **On Connect** → *Emit With Ack join* →
  typed **On Ack**; elsewhere *On chat_message* (typed) and *Emit send_chat*. A server-initiated ack is an
  `Ack` pin wired to *Ack Reply*. No C++.
* C++: one statement per `On` and per `Emit`, JavaScript-shaped:
  `Chat.On(TEXT("move"), this, [this](int32 X, int32 Y) { … })`, `Chat.Emit(TEXT("move"), X, Y)`.
* A wrong server version, a wrong engine path, a refused namespace or an oversize message is reported with
  its cause, never as a silent reconnect loop or a dropped connection.

### 0.2 Protocol facts this spec relies on

Verified 2026-10-20 and re-checked by the review. Sources: **[EIO]**
<https://socket.io/docs/v4/engine-io-protocol/> (same text as `github.com/socketio/engine.io-protocol`
`README.md`); **[SIO]** <https://socket.io/docs/v4/socket-io-protocol/> (=
`github.com/socketio/socket.io-protocol` `Readme.md`); **[CSR]**
<https://socket.io/docs/v4/connection-state-recovery>; **[ref]** the reference implementation at tag
`socket.io@4.8.4`, commit `c239eb5d8945cee40a696a26e4aca00016f9565f` (2026-09-25; it bundles engine.io
6.6.11 and `ws` 8.21), paths relative to `packages/`, permalink form
`https://github.com/socketio/socket.io/blob/c239eb5d8945cee40a696a26e4aca00016f9565f/packages/<path>#L<n>`.
Every cited file was compared byte for byte with `main` on 2026-10-20 (identical), and each cited line
was opened.

| # | Fact | Source |
| --- | --- | --- |
| P1 | Engine.IO packet types: open 0, close 1, ping 2, pong 3, message 4, upgrade 5, noop 6. Over WebSocket each packet is its own frame, `<type>[<data>]`; binary payloads are sent as is, without prefix (the `b` + base64 form is polling-only). | [EIO] §Protocol, §Packet encoding |
| P2 | A WebSocket-only session is `GET <path>?EIO=4&transport=websocket` with no `sid`; the server "MUST respond with an `open` packet" with `sid`, `upgrades` (`[]` on a WebSocket-only session), `pingInterval`, `pingTimeout` (ms). **`maxPayload` exists only since engine.io 6.2.0 / socket.io 4.5.0**; socket.io 3.0–4.4 omit it although they enforce `maxHttpBufferSize` (default 1 MB since engine.io 4.0 / socket.io 3.0). Current defaults: `pingInterval` 25000, `pingTimeout` 20000, `maxHttpBufferSize` 1e6, transports polling + websocket. | [EIO] §Handshake; `socket.io-protocol/test-suite/test-suite.js` (WebSocket "should successfully open a session"); `engine.io/CHANGELOG.md` §6.2.0, §4.0.0-alpha.0; `socket.io/CHANGELOG.md` §4.5.0; [ref] `engine.io/lib/server.ts#L198-L212`, `engine.io/lib/socket.ts#L128-L143` |
| P3 | v4 heartbeat is server-initiated: ping every `pingInterval`, client answers pong within `pingTimeout`; the client "SHOULD consider that the connection is closed" without a ping for `pingInterval + pingTimeout`. A v4 server treats a client ping as an error ("invalid heartbeat direction"). engine.io ≥ 6.6.11 also extends its pong deadline once per cycle on any inbound packet; older servers do not, so VaRest does not rely on it. | [EIO] §Heartbeat; [ref] `engine.io-client/lib/socket.ts#L820-L826`, `engine.io/lib/socket.ts#L172-L188`; `engine.io/CHANGELOG.md` §6.6.11 |
| P4 | For WebSocket the server passes `maxHttpBufferSize` as the `ws` library's `maxPayload`, so one larger client frame ends the connection — whether or not the OPEN advertises it. | [ref] `engine.io/lib/server.ts#L736-L741` |
| P5 | On WebSocket, **every** pre-upgrade refusal is `HTTP/1.1 400 Bad Request` with a `text/html` body: "Transport unknown", "Bad request" (also a failing middleware), "Forbidden" or the `allowRequest` message. `EIO=3` without `allowEIO3` is refused after the 101. A request whose path does not match gets **no response**: the server closes it after 1 s. The engine path is normalised to a trailing slash and prefix-matched, so `/socket.io/?…` matches servers with and without `addTrailingSlash`. | [ref] `engine.io/lib/server.ts#L1062-L1085`, `#L457-L469`, `#L1000-L1017`, `#L251-L259`, `#L975-L977` |
| P6 | Socket.IO packet types: CONNECT 0, DISCONNECT 1, EVENT 2, ACK 3, CONNECT_ERROR 4, BINARY_EVENT 5, BINARY_ACK 6. Format `<type>[<n attachments>-][<namespace>,][<ack id>][JSON]`, namespace only when not `/`, wrapped in an EIO message (`4`). | [SIO] §Packet encoding |
| P7 | v5 has no implicit default namespace: the client "MUST send a CONNECT packet" per namespace; success is CONNECT `{sid}` (`{sid, pid}` with recovery), refusal is CONNECT_ERROR `{message, data?}` — a plain string only to EIO 3 clients. No namespace within `connectTimeout` (default 45 s) → the server closes. | [SIO] §Connection, §History v5; [ref] `socket.io/lib/socket.ts#L509-L519`, `socket.io/lib/namespace.ts#L364-L374`, `socket.io/lib/client.ts#L111-L118`, `socket.io/lib/index.ts#L324` |
| P8 | EVENT data "MUST be a non-empty array"; element 0 is the event name (a non-reserved string, or a number). ACK data is an array, possibly empty. The reserved names — `connect`, `connect_error`, `disconnect`, `disconnecting`, `newListener`, `removeListener` — are defined by the parser, not by [SIO]; an invalid payload makes the receiver close the connection. | [SIO] §Sending; [ref] `socket.io-parser/lib/index.ts#L11-L18`, `#L301-L321` |
| P9 | Binary: placeholders `{"_placeholder":true,"num":n}`, attachments follow as binary frames. The decoder rejects more than 10 attachments by default, a placeholder `num` outside `[0, n)`, text arriving while attachments are expected, and binary arriving when none are. | [SIO] §Examples; [ref] `socket.io-parser/lib/index.ts#L169`, `#L181-L200`, `#L236-L247`, `socket.io-parser/lib/binary.ts#L66-L75` |
| P10 | A packet for a namespace the client has not joined (including DISCONNECT while its CONNECT is pending), or a CONNECT for a joined one, makes the server close the **whole** connection ("invalid state"). DISCONNECT is applied on `process.nextTick`, so a CONNECT for the same namespace read in the same tick still sees it joined. | [ref] `socket.io/lib/client.ts#L279-L296` |
| P11 | Connection state recovery (socket.io ≥ 4.6.0, 2023-02-07): the server appends an offset as the last element of every EVENT that has no ack id and is not volatile — BINARY_EVENTs included, since the type is checked before binary encoding; the client tracks the last string argument as the offset and reconnects with CONNECT `{pid, offset, …auth}`; restore needs both as strings; missed packets are written **before** the CONNECT ack; only transport-level reasons are recoverable, a namespace DISCONNECT is not; defaults 2 min and `skipMiddlewares: true`; "the server must send at least one event"; the Redis (pub/sub) adapter does not support it, Redis Streams and MongoDB (≥ 0.3.0) do; the in-memory adapter keeps sessions per node. | `socket.io/CHANGELOG.md` §4.6.0; [ref] `socket.io-adapter/lib/in-memory-adapter.ts#L479-L497`, `socket.io-client/lib/socket.ts#L638-L645`, `#L770-L781`, `#L832-L836`, `socket.io/lib/namespace.ts#L386-L405`, `socket.io/lib/socket.ts#L39-L46`, `#L166-L179`, `socket.io/lib/index.ts#L331-L334`; [CSR] |
| P12 | Reference client: emits while disconnected are buffered, volatile ones discarded; events received before the namespace CONNECT ack are buffered and dispatched before the `connect` event; on close, only acks registered with an error-first callback (`withError`) fail — plain callbacks are dropped silently; CONNECT_ERROR destroys the socket (no automatic retry); a CONNECT ack without `sid` is reported as "a Socket.IO server in v2.x". Reconnect defaults: 1000 ms × 2 → 5000 ms, randomisation 0.5, unlimited; connect timeout 20000 ms. | [ref] `socket.io-client/lib/socket.ts#L450-L457`, `#L754-L767`, `#L832-L861`, `#L683-L701`, `#L739-L744`, `#L712-L723`; `socket.io-client/lib/manager.ts#L172-L181` |
| P13 | Server ack ids come from one counter per **namespace**, shared by all its sockets, held in a JS number (exact to 2^53 − 1). | [ref] `socket.io/lib/socket.ts#L250`, `socket.io/lib/namespace.ts#L172` |

### 0.3 Scope: WebSocket-only Engine.IO v4, Socket.IO v5 only

**WebSocket transport only** (as WS §0.5 already states): no HTTP long-polling, no upgrade, no
WebTransport. Why: the default server accepts WebSocket-only clients with no configuration (P2);
polling exists for browsers behind WebSocket-hostile proxies, which native games rarely face on
`wss://…:443`; polling would need the HTTP core in a long-poll pattern, an ordered POST/GET pair per
session and **sticky sessions** on multi-node deployments, which WebSocket-only does not need. Cost if
wrong: players behind a proxy that blocks WebSocket cannot connect; the transport seam keeps a polling
transport possible in 2.x.

**Socket.IO v5 over EIO 4 only** (socket.io 3.x and 4.x servers). Socket.IO 2.x (EIO 3) is
**unsupported and detected** (§2.7), with no compatibility option in 2.0. Why: v2 reverses the
heartbeat (P3), connects the default namespace implicitly and sends string CONNECT_ERRORs — a second
protocol and a doubled test matrix for a version superseded by socket.io 3.0 in November 2020 ([SIO]
§History). Version confusion is the most common failure of existing UE Socket.IO plugins (§0.4); one
protocol with a precise mismatch error removes it.

### 0.4 Lessons from existing UE Socket.IO plugins

The popular third-party plugin `getnamo/SocketIOClient-Unreal` (not used, no code copied) wraps
socket.io-client-cpp. The recurring pain points of that approach, and the habits its users bring, are
design requirements here:

| Pain point or habit | VaRest answer |
| --- | --- |
| Which plugin branch talks to which server version (v2 vs v3/v4) | one protocol (v5/EIO 4); a v2 server is detected and named in the error (§2.7) |
| Callbacks from library threads, crashes at PIE end or shutdown | handlers only on the game thread (WS §4), sockets per game instance (WS §1.4), no third-party threads |
| BP events bound by function name; a plugin-specific JSON type | typed event nodes from asset declarations; payloads are `FVaRestJson`/structs, the same types as HTTP |
| "Add the SocketIO Client Component to your actor", auto-connect, plugin-scoped connection | `UVaRestSioComponent` with `bAutoConnect` and an actor or game-instance scope (§8.4) |
| Acks awkward in BP; acks silently lost on disconnect (P12) | `Ack` is a pin; *Emit With Ack* is an async node; **every** ack ends in success, error or timeout (§4.5) |
| A third-party stack (socket.io-client-cpp, asio, websocketpp, its own TLS) per platform | none: the WS layer's transport on the engine-bundled libwebsockets (WS §2) |

### 0.5 Non-goals

* HTTP long-polling, the polling→WebSocket upgrade, WebTransport (§0.3).
* Socket.IO 2.x / EIO 3 servers (§0.3, detected), custom parsers (msgpack, `socket.io-json-parser`):
  the codec seam allows one later.
* A Socket.IO **server**; rooms and broadcast (server concepts, the client has none).
* The `compress` flag: the bundled LWS has no permessage-deflate (WS T1).
* Implicit sharing of one connection by separate `Make` calls (socket.io-client's Manager cache).
  Sharing is explicit: named sockets and namespaces of one socket (§1.3).
* At-least-once delivery beyond `EmitWithAck(...).Retry(N)`; persisting the backlog or the recovery
  `pid` across app restarts.
* `onAnyOutgoing` (the trace tap covers it), an asynchronous `auth` callback (the session's
  single-flight refresh covers it; a synchronous per-CONNECT hook exists, §3.3).

---

## 1. Module and object model

### 1.1 Module

```
VaRestJson          + J-1 and three writer entries (§18.2; M2 Task 7)
   ^
VaRestHttp          + session change kinds (WS §22 A1, core M4)
   ^
VaRestWebSocket     + SW-1, SW-2, SW-4, SW-5, SW-7, SW-8, SW-9, SW-10, SW-11 (WS §22)
   ^
VaRestSocketIO      Runtime   codec (EIO + SIO), stage, FVaRestSio facade, FVaRestSioSocket/Namespace,
                              UVaRestSioClient, UVaRestSioComponent, UVaRestSioEndpoint, UVaRestSioLibrary,
                              async BP actions, settings
VaRestEditor        + Socket.IO K2 nodes on the WS declaration nodes (SW-11)
VaRestEditorTools   + asset entry customisation (Entry dropdown), validation messages
```

`VaRestSocketIO`: `Type Runtime`, `LoadingPhase Default`, `PlatformAllowList`
Win64/Mac/Linux/Android/IOS. Dependencies: `Core`, `CoreUObject`, `Engine`, `VaRestJson`,
`VaRestHttp`, `VaRestWebSocket`. **No third-party code**; Engine.IO and Socket.IO are small text
protocols over the WS transport. Same plugin (WS ruling R4).

### 1.2 Object model

| Type | Role |
| --- | --- |
| `FVaRestSio` | Static facade: `Socket(WorldContext, Api, Entry)`, `Make(WorldContext, Url)`, `Make(WorldContext, Name, Url)`, `Find(WorldContext, Name)`; overloads taking a `UVaRestSocketHub*` (editor: `GetForEditor()`). |
| `FVaRestSioSocket` | C++ handle to one Socket.IO connection. Wraps an `FVaRestSocket` (same ownership, thread-safe refcount) but exposes **only** Socket.IO operations — no raw `Send`/`SendFrame`, so protocol-breaking calls do not compile; WS lookups refuse Socket.IO sockets (WS §22 A3). Forwards namespace members to its default namespace. |
| `FVaRestSioNamespace` | Handle to one namespace = one WS channel (WS §1.3): router, readiness, ready lane, backlog, pending acks, `OnConnect/OnConnectError/OnDisconnect`. Holds the socket strongly, like a socket handle. |
| `FVaRestSioSocketWeak`, `FVaRestSioNamespaceWeak` | Weak handles (`AsWeak()`, `Pin()`) for lambda captures: a lambda holding a strong handle, registered on the same socket, is a cycle and an unnamed socket would never close (WS §1.3). |
| `FVaRestSioEvent` | Inbound event (BlueprintType): namespace, name, args (`FVaRestJson` array), attachments, `Ack`, offset. |
| `FVaRestSioAck` | Reply handle for a server event that requested an ack (BlueprintType, copyable; copies share one state, at most one reply, §4.5). |
| `FVaRestSioEmitWithAck` | `[[nodiscard]]` builder for client → server acks (core §3.2 builder rules). |
| `FVaRestSioEmitter` | Result of `Volatile()`: `Emit`/`EmitArgs` with the volatile flag. |
| `FVaRestSioConnect`, `FVaRestSioDisconnect`, `FVaRestSioConnectError` | BP structs for namespace lifecycle (§3). |
| `FVaRestSioCodec` | `IVaRestSocketCodec`; creates one decoder per connection on its Open event (SW-2): EIO framing, SIO packets, attachments, typed decode plans. |
| `FVaRestSioStage` | `IVaRestSocketStage`: EIO session and heartbeat, namespace state machine, CONNECT/auth, recovery, acks, version and path detection. |
| `UVaRestSioClient` | BP `UObject` around one `FVaRestSioSocket` (§8.1). |
| `UVaRestSioComponent` | Actor component (`BlueprintSpawnableComponent`, subclass of WS `UVaRestSocketComponent`, SW-10): details-panel configuration, auto-connect, assignable events (§8.4). |
| `UVaRestSioEndpoint` | Asset entry (`UVaRestSocketEndpointBase` subclass, SW-10, §9). |
| `UVaRestSioSettings` | DeveloperSettings, *Project Settings ▸ Plugins ▸ VaRest Socket.IO* (§12 defaults). |

### 1.3 Ownership, lifetime, PIE

Exactly WS §1.4, because a Socket.IO socket **is** a WS socket with the Socket.IO codec and stage:
unnamed sockets are owned by their handles (`FVaRestSio::Make(this, Url)`), named ones and asset
entries by the game-instance `UVaRestSocketHub` (`FVaRestSio::Socket(this, GameApi, "Chat")`),
`Owner(UObject*)` closes an unnamed socket when the object dies, BP *Make Socket.IO* without a name is
owned by its world-context object, and the component owns its socket per its scope (§8.4). Each PIE
client has its own hub, connections, sessions and recovery state. There is no static entry without a
world context or a hub, so game code cannot reach the engine scope by accident (WS §1.4).

One `FVaRestSioSocket` is one Engine.IO connection; its namespaces multiplex over it. Two `Make` calls
to the same URL are two connections (§0.5). Capture `AsWeak()` in handlers that refer to their own
socket or namespace.

---

## 2. Protocol layering on the WebSocket layer

### 2.1 Pipeline

```
inbound:  LWS → service thread: assemble, UTF-8 check, auto-reply "2"→"3" (SW-5), idle deadline (SW-5)
          → event queue → pipe: Sio decoder (EIO type → SIO header → JSON → attachments → typed decode plans)
          → delivery queue → GT: Sio stage (OPEN, close packet, namespace state, CONNECT ack/error, hold,
            recovery offset, acks) → WS router: OnAny → On(event) → OnUnhandled | pending acks
outbound: Emit / Ack.Send (any thread), EmitWithAck (GT) → encode (header gap, placeholders, ack id)
          → WS SendBatch (SW-9; Ack.Send pinned to its connection and namespace)
          → GT flush: namespace gate, send limit = maxPayload per frame, lanes: bypass (CONNECT, DISCONNECT)
            / ready (OnConnect sends) / backlog, per namespace
          → command queue → LWS (a packet's frames contiguous; auto-replies jump the queue at message boundaries)
```

Wire sample (`>` client, `<` server):

```
GET /socket.io/?EIO=4&transport=websocket                   Authorization header / query per §6
< 0{"sid":"lv_VI…","upgrades":[],"pingInterval":25000,"pingTimeout":20000,"maxPayload":1000000}
> 40{"token":"eyJ…"}                                        default namespace CONNECT + auth payload
> 40/admin,{"token":"eyJ…","role":"gm"}
< 40{"sid":"wZX3…","pid":"Rp9…"}                           OnConnect("/", Recovered = false)
< 44/admin,{"message":"not allowed","data":{"code":403}}   OnConnectError("/admin")
> 42["join","lobby"]                                        sent in OnConnect (ready lane)
> 42["send_chat",{"text":"hi"}]                             backlog from before the connect
< 42["chat_message",{"from":"neo","text":"hi"},"o17"]       recovery offset appended (P11)
< 2                                                         server ping
> 3                                                         answered on the service thread
> 451-13["upload",{"name":"a.png","data":{"_placeholder":true,"num":0}}]
> <binary frame, 4096 bytes>
< 4313[{"ok":true}]                                         ack 13 → OnSuccess
```

### 2.2 Handshake and URL

* **URL.** `http(s)` → `ws(s)` (WS §3.2). **Engine path** `WithPath(...)`, default `/socket.io/`; a
  trailing slash is always appended, because `/socket.io/?…` matches servers with and without
  `addTrailingSlash` (P5) and python-socketio strips slashes.
* **Code-first `Make(Url)` follows socket.io-client: the URL path is the default namespace.**
  `Make(this, TEXT("https://chat.game.io/admin"))` connects to
  `wss://chat.game.io/socket.io/?EIO=4&transport=websocket` and joins `/admin`. Socket.IO users copy
  `io("https://host/admin")` from server docs, so the same URL must mean the same thing. A server mounted
  under a sub-path sets `WithPath(TEXT("/admin/socket.io/"))`; the wrong-path detection (§2.6) and the
  "Invalid namespace" error (§3.4) both say so. Asset entries have separate fields and no such rule (§9).
* **URL hygiene** (users paste what they see in browser dev tools):
  * a URL path equal to `/socket.io` or `/socket.io/` is taken as the engine path with namespace `/`
    (Info log);
  * `EIO`, `transport` and `sid` in the URL query are removed with a warning; other query keys are kept
    as handshake query;
  * a namespace containing `?` is refused (`Config`: "query in a namespace is the EIO 3 auth style; use
    WithAuth or WithQuery").
* **Query** `EIO=4&transport=websocket` (stage `OnHandshake`, WS §8) + app query + auth query
  placement. **Never a `sid`**: every attempt is a new Engine.IO session (§2.6).
* Headers, Origin, TLS, proxy, allowlist, connect timeout: WS §2.2 unchanged.

### 2.3 Engine.IO session

The first frame after the upgrade must be a text OPEN packet. The stage validates it: JSON object, `sid`
a non-empty string, `pingInterval` and `pingTimeout` positive integers (values under 50 ms are raised to
50 ms; under 1 s logs a warning — the official suite uses 300/200). **`maxPayload` is optional** (P2):
present, it must be a positive number; absent (socket.io 3.0–4.4), VaRest assumes **1 000 000** bytes, the
EIO 4 server default, logs at Verbose, and `WithLimits(MaxOutboundFrameBytes)` may raise it for a server
known to allow more. `upgrades` is ignored. Then: idle deadline = `pingInterval + pingTimeout` (SW-5),
send limit = the effective `maxPayload` (WS `Ctx.SetSendLimit`), and CONNECT for every wanted namespace in
the bypass lane (§3).

| Inbound EIO packet | Handling |
| --- | --- |
| `0` OPEN (first frame) | as above; a second OPEN is a protocol error |
| first frame not `0…` | terminal `Config`: "not an Engine.IO v4 endpoint (first frame: `<excerpt>`)" |
| `2` ping | answered `3` by the transport auto-reply (§2.4); counted and traced, never reaches handlers; a ping a transport delivers without the `bAutoReplied` flag (a transport without SW-5) is answered by the stage |
| `3` pong | unexpected (VaRest never pings at the EIO level); Verbose, ignored |
| `4` message, text | Socket.IO packet (§3–§5) |
| binary frame | an attachment of the packet being assembled (§5.2); none expected → protocol error |
| `1` close | close the transport 1000 and reconnect, like a clean server close (WS R3) |
| `5` upgrade, `6` noop | Verbose, ignored |
| anything else | protocol error |

**Protocol error**: the decoder is poisoned for that connection (its remaining frames are dropped,
SW-2), VaRest closes with 1002, reports `Decode`, **drops every recovery `pid`** (a rejected packet would
otherwise be replayed by recovery on every reconnect, §4.6) and reconnects; three in a row without a
connection reaching `StableAfter` make it terminal (`Decode`).

### 2.4 Heartbeat

* **VaRest never sends an Engine.IO ping.** A v4 server closes on one (P3). The WS app heartbeat is
  disabled by the stage (`ProvidesHeartbeat() = true`, WS §8), and Socket.IO sockets expose no
  app-heartbeat setting: `FVaRestSioSocket` has no `WithHeartbeat`, and the asset entry does not inherit
  the WS heartbeat override (SW-10).
* **Pong on the service thread.** The connect command carries an auto-reply rule (SW-5): a **text** frame
  `2` is answered `3` by the transport without waiting for the game thread, because the server allows only
  `pingTimeout` (20 s default) for the pong and level loads or shader compiles stall the game thread longer.
  The ping is still delivered, flagged `bAutoReplied`, for stats and trace.
* **Pong priority.** `3` is an Engine.IO *data* packet, i.e. a WebSocket **text data frame**, not a WS
  control frame. RFC 6455 §5.4 allows only control frames between the fragments of a fragmented message, so
  the pong can never be placed inside another data message — but it does not have to wait behind the queue
  either: auto-replies are written **at the next message boundary, ahead of every queued data message**
  (SW-5), are not counted in the buffered amount or backpressure, and are never dropped. Socket.IO allows
  this: the pong is consumed by Engine.IO below the Socket.IO parser (P10's `ondecoded` sees only message
  packets), so it may sit between a BINARY_EVENT header and its attachments.
* **Worst-case pong delay** D = (R + K) × 8 / U, where R is the unsent part of the data message being
  written (at most one outbound frame, bounded by the server's `maxPayload`: 1 000 000 B by default), K the
  bytes already in the OS send buffer (capped at 256 KiB with `SO_SNDBUF` on connections that have
  auto-reply rules, SW-5), and U the uplink in bit/s. WS fragmentation (64 KiB, WS §2.4) does not shorten R —
  the pong cannot enter a fragmented message — but keeps WS control frames flowing. With R = 1 000 000 B and
  K = 262 144 B:

  | Server `pingTimeout` | Uplink needed for an on-time pong | D at 2 Mbit/s |
  | --- | --- | --- |
  | 20 000 ms (current default) | ≥ 0.51 Mbit/s | 5.0 s (4× margin) |
  | 5 000 ms (an old default still found in configs) | ≥ 2.0 Mbit/s | 5.0 s (late) |

  When `(maxPayload + 256 KiB) × 8 / pingTimeout` exceeds 1 Mbit/s (a server that raised
  `maxHttpBufferSize` or lowered `pingTimeout`), VaRest logs one warning per connection naming both values.
  Below the needed uplink, a full-size frame in flight can cost the connection; recovery restores the
  session (§4.6).
* **Dead-link deadline on the service thread**: no inbound **data** frame (text or binary; WS PONGs do not
  count) within `pingInterval + pingTimeout` (P3, 45 s with server defaults) kills the connection,
  `By = Heartbeat`, reason *ping timeout* (SW-5). Any inbound data frame counts as alive (the reference
  client resets only on pings, which can only make it fire earlier).
* **Deadlines are suspended while VaRest itself has paused reading** (WS flow control, WS §2.3), for both
  the EIO deadline and the WS protocol ping: a client that stopped reading cannot blame the server. The
  server decides; if the link ends meanwhile, `FVaRestSocketClose::bWhileReadPaused` is set and the log
  says "dropped while the game was not reading" (SW-5).
* **The WS protocol ping stays on** (WS ruling R3, 20 s / 10 s; `WithProtocolPing`): RFC 6455 §5.5.2 obliges
  the server to answer (the Node `ws` library under socket.io does so by default); it is VaRest's only RTT
  source (Engine.IO pings are server-initiated) and detects death in ≤ 30 s. `WithProtocolPing(0, 0)` turns
  it off.

### 2.5 `maxPayload` and frame limits

* **Outbound.** Every frame of a packet (the text header and each attachment) is checked against the
  effective `maxPayload` (advertised, or 1 000 000 when absent, P2/P4) and WS `MaxSendBytes`. Over → the
  emit is **rejected** with `Config` ("event `upload`: frame of 1 400 512 bytes exceeds the server's
  maxPayload 1 000 000"), and the connection stays up. The check runs at the call against the current
  connection's value and again at flush for queued frames (SW-9 enforces the send limit at flush).
* **Inbound.** Socket.IO sockets default to `MaxMessageBytes` 8 MiB per frame (WS default 1 MiB; Socket.IO
  servers send attachments and the reference client has no limit) and `MaxPacketBytes` 16 MiB per packet
  (header + attachments). Over → close 1009 by VaRest, `Decode`, reconnect with the recovery `pid`s
  dropped (§4.6).
* **Reassembly cannot deadlock against flow control** by configuration, not by a hook: an incomplete
  packet's bytes count toward WS `MaxPendingInboundBytes` like any undelivered bytes, and VaRest enforces
  `MaxPacketBytes ≤ MaxPendingInboundBytes / 2` (`Config` at configuration time) with a Socket.IO default
  `MaxPendingInboundBytes` of **32 MiB**. So the pause threshold can never be reached by a single
  incomplete packet waiting for its own remaining attachments.

### 2.6 Reconnect and the Engine.IO session

Each attempt is a fresh Engine.IO session (new `sid`, nothing to resume at the EIO level). Continuity is
Socket.IO's job: connection state recovery per namespace (§4.6). Reconnect timing and the state machine
are WS §3 with Socket.IO defaults: **socket.io-client's** `BaseDelay` 1 s, `Multiplier` 2, `MaxDelay` 5 s,
`Jitter = Ratio(0.5)`, unlimited (P12), plus the WS `StableAfter` 5 s. Why socket.io-client's values:
Socket.IO servers are sized for them (every browser client behaves so); WS's 30 s cap would make a game
reconnect visibly slower than the web client of the same backend. Timeouts: `ConnectTimeout` 20 s
(socket.io-client `timeout`), `ReadyTimeout` 20 s from open (OPEN plus the first namespace CONNECT ack;
server middleware often hits a database). Both, and the per-namespace CONNECT timeout, are game-thread
timers that follow the WS §5 hitch rule: a stall longer than the timeout re-arms them once before they
fire, so connecting in a menu and then loading a level does not cause a spurious reconnect.

Classification rows added to WS §3.4 (the stage's `ShouldReconnect`):

| Event | Default | Error kind when terminal |
| --- | --- | --- |
| no OPEN, or no namespace connected, within `ReadyTimeout` | reconnect | |
| a namespace's CONNECT unanswered for `ReadyTimeout` while another namespace is Connected | **no restart**: that namespace gets `OnConnectError(Timeout, bWillRetry = true)` and keeps waiting, with no re-send and no DISCONNECT (P10); it gets CONNECT again on the next connection | |
| first frame not OPEN, invalid OPEN, v2 server (§2.7) | stop | `Config` |
| **upgrade sent, connection closed without any HTTP response** (WS §22 A2 reason `NoUpgradeResponse`; engine.io's answer to a wrong path, P5) | reconnect; `OnRetryScheduled`'s status carries the hint; 3 in a row → stop | `Config`: "no Engine.IO server at `<redacted URL>`; the engine path (`WithPath` / asset `Path`, default `/socket.io/`) is not the namespace" |
| upgrade 400 (P5) | with a Header or Query session placement: the first 400 is treated like 401 (one single-flight refresh, then retry); otherwise, or again → stop | `Http`, message: "Engine.IO refused the upgrade (400): websocket transport not enabled, refused by allowRequest or middleware (Origin, auth), or bad request" |
| EIO idle deadline (*ping timeout*) | reconnect | |
| EIO `1` close packet | reconnect (as a clean server close) | |
| protocol error (we close 1002), invalid UTF-8 (we close 1007, WS) | reconnect, `pid`s dropped; 3 protocol errors in a row without `StableAfter` → stop | `Decode` |
| inbound over `MaxMessageBytes`/`MaxPacketBytes` (we close 1009) | reconnect, `pid`s dropped | |
| last wanted namespace refused (CONNECT_ERROR, terminal) | stop (`OnGaveUp`) | `Server` / `Auth` / `Config` (§3.4) |
| last wanted namespace kicked (server DISCONNECT) | stop (`OnGaveUp`) | `Server` |

### 2.7 Version detection

A socket.io 2.x server answers an `EIO=4` upgrade with a v3 OPEN and joins the default namespace on its
own (`40` without `sid`). **Any CONNECT ack without `sid`** — requested or not — and **a string
CONNECT_ERROR** (v3/v4 servers send strings only to EIO 3 clients, P7) are terminal `Config`: *"the server
speaks Socket.IO v2 (EIO=3); VaRest supports Socket.IO 3.x and 4.x servers"* (the reference client's own
check, P12). A missing `maxPayload` is **not** a v2 signal (P2: socket.io 3.0–4.4 omit it too).

---

## 3. Namespaces

### 3.1 Model

* Every socket has a **default namespace**: the URL path for `Make(Url)` (§2.2), `DefaultNamespace` for
  asset entries (§9), else `/`. `FVaRestSioSocket`'s `On`/`Emit`/`OnConnect`/… act on it.
* `Socket.Namespace(TEXT("/admin"))` returns the handle (created once, game thread). A namespace is
  **wanted** after its `Connect()`. `Socket.Connect()` = transport + default namespace (always) + asset
  namespaces with `bConnectOnStart`. `Ns.Connect()` on a disconnected socket connects the transport too,
  so `Socket.Namespace(TEXT("/admin")).Connect()` alone joins only `/admin` (`io("/admin")` parity).
* Names must start with `/` and contain neither `,` (the comma ends the namespace in the encoding, P6) nor
  `?` (§2.2); checked at `Namespace()` (`Config` + invalid handle) and in asset validation.
* **Namespace gate**: nothing but CONNECT is ever sent for a namespace before its CONNECT ack, and nothing
  at all after its DISCONNECT. A violation would make the server close the whole connection (P10). Emits
  wait in that namespace's backlog; replies to acks of a namespace that is no longer connected are dropped
  (Verbose).
* **Re-CONNECT after DISCONNECT on the same connection** waits max(WS RTT, 100 ms), so the two frames are
  never read by the server in one tick (P10's `nextTick`); the reference interop gate pins this (§14.4).
* **Inbound packets for a namespace that is not wanted or unknown** are dropped (Verbose; unrequested
  acks: §3.2).
* **Last wanted namespace rule**: when the app or the server ends the last wanted namespace (refused,
  kicked, disconnected), the socket closes (1000 "no namespace") without reconnecting — socket.io-client
  closes the Manager in the same case. Refused/kicked → `OnGaveUp` with the namespace's error.

### 3.2 State machine

`EVaRestSioNamespaceState`: `Disconnected` (not wanted), `Connecting` (wanted; sub-states below),
`Connected`. Handlers never re-enter it; calls from inside a dispatch are deferred (WS §3.1). A
table-driven test covers every cell. *Connecting (not sent)* covers waiting for OPEN, for a proactive token
refresh (§6) and for the re-CONNECT delay (§3.1); timeouts start when the CONNECT is sent.

| State \ Event | `Ns.Connect()` | OPEN | CONNECT ack | CONNECT_ERROR | CONNECT timeout | server DISCONNECT | transport lost | `Ns.Disconnect()` | `Socket.Close()` / logout |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Disconnected | → Connecting: CONNECT as soon as this connection's OPEN was seen (after a pending refresh or re-CONNECT delay, §3.1), on the next OPEN if the socket is connecting or reconnecting, and connect the socket if it is Disconnected | — | *unrequested ack* | Verbose, ignored | — | Verbose, ignored | — | no-op | no-op |
| Connecting (not sent) | no-op | send CONNECT (§3.3) when the refresh and re-CONNECT delay allow | *unrequested ack* | Verbose, ignored | — | Verbose, ignored | stays | → Disconnected | → Disconnected |
| Connecting (sent) | no-op (cancels a pending *leave on ack*) | — | → Connected (§3.6); no `sid` → §2.7; with *leave on ack*: send DISCONNECT → Disconnected, no `OnConnect` | §3.4: refresh + resend, or → Disconnected | another namespace Connected: `OnConnectError(Timeout, bWillRetry)`, keep waiting; none Connected: the connection restarts (§2.6) | → Disconnected (treated as a refusal) | → Connecting (not sent); with *leave on ack* → Disconnected, no packet | → Disconnected for the app; internally *leave on ack* (P10 forbids DISCONNECT before the ack) | → Disconnected, no packet (the close frame follows) |
| Connected | no-op | — | warning, ignored | warning, ignored | — | → Disconnected (`ServerDisconnect`, not wanted) | → Connecting (not sent) with `OnDisconnect(bWillReconnect)`, or → Disconnected if the socket gave up | send DISCONNECT → Disconnected (`ClientDisconnect`) | DISCONNECT in the closing hook (SW-7) → Disconnected (`SocketClosed`) |

*Unrequested ack*: without `sid` → §2.7 (v2 server); with `sid` the server believes we joined, so VaRest
sends DISCONNECT for that namespace and logs a warning.

**Session replaced** (§6) and `ResetSession()`: Connected namespaces get DISCONNECT through the closing
hook, every `pid` is dropped, the connection restarts with the new credentials, and wanted namespaces stay
wanted (reason `SessionReplaced`, §3.5).

Leaving a namespace (any → Disconnected) fails its sent acks (`Connection`, retryable) and, unless the
socket will reconnect, cancels its backlog (`Canceled`) — SW-8.

### 3.3 CONNECT payload

Built per CONNECT on the game thread, in this order (later keys win, as socket.io-client merges
`{pid, offset}` under the app's auth):

1. recovery fields `{pid, offset}` when both are known (§4.6);
2. the namespace's static payload (`WithAuth(FVaRestJson)` or asset `AuthPayload`);
3. the session token at the payload placement's field, when the namespace sends session credentials
   (§6; refreshed first if it expires within 30 s, WS §3.3 — the namespace waits in *Connecting (not
   sent)* meanwhile);
4. the per-CONNECT hook `OnConnectAuth(Owner, Fn(FVaRestJson& Payload, const FVaRestSioConnectContext&))`
   (`Attempt`, `bReconnect`, namespace) — synchronous; it may edit anything.

An empty result sends `0` / `0/admin,` with no payload, like the reference encoder.

### 3.4 Connect errors

CONNECT_ERROR data is an object `{message, data?}` (a string → §2.7). `FVaRestSioConnectError`
(BlueprintType): `Namespace`, `Message`, `Data` (`FVaRestJson`), `bWillRetry`. The `FVaRestError` has
`Message` = `message`, `Body` = `data`, `Code` = `data.code` when it is a string or number. A namespace
CONNECT timeout (§3.2) is reported the same way with `Kind=Timeout`, `bWillRetry = true`.

* **"Invalid namespace"** (the server's fixed text for an unknown namespace) → terminal `Config`, with the
  §2.2 hint about URL path vs engine path.
* **Auth retry (default).** If the namespace sent session credentials and this connect cycle has not
  retried yet, VaRest refreshes once (single-flight, `Ctx.RequestRefresh()`, core §4.2) and tries again:
  * **Payload placement** (the token is in the CONNECT): any refusal qualifies; the new CONNECT goes out
    on the same connection.
  * **Header or Query placement** (the token is in the upgrade): only an auth-looking refusal qualifies —
    `data.code` ∈ {401, 403} (number or string), or `message` containing `unauthori`, `forbidden`, `jwt`,
    `token`, `expired` or `auth` (case-insensitive) — because the retry restarts the connection and every
    namespace with it; the restart is immediate and not a backoff step.

  Refresh fails → terminal `Auth` and the client's `OnSessionExpired` (core §4.2). Refused again after a
  **successful** refresh → terminal `Server`: the token was not the problem, so the session is not expired.
  A wrong guess costs one refresh call, never a logout.
* Otherwise → terminal `Server`. Terminal means: namespace → Disconnected (not wanted), `OnConnectError`
  with `bWillRetry = false`, last-wanted rule (§3.1). Like socket.io-client, VaRest never loops on
  refusals; the app may call `Ns.Connect()` again (BP *Connect Namespace*).
* **Override**: `Ns.ConnectErrorIf(Fn(const FVaRestSioConnectError&) -> EVaRestSioRetry)` with `Default`,
  `RefreshAndRetry`, `Retry` (re-CONNECT after the reconnect policy's delay, counted against
  `MaxAttempts`; for "server busy" refusals), `Stop`.

### 3.5 Disconnect

`FVaRestSioDisconnect` (BlueprintType): `Namespace`, `Reason` (`EVaRestSioDisconnectReason`),
`ReasonText` (socket.io-client's wording), `bWillReconnect`, `Close` (`FVaRestSocketClose` for transport
reasons, including `bWhileReadPaused`).

| Reason | `ReasonText` | Cause | Recoverable by the server (P11) |
| --- | --- | --- | --- |
| `ClientDisconnect` | io client disconnect | `Ns.Disconnect()` | no |
| `SocketClosed` | io client disconnect | `Socket.Close()`, logout, owner gone, released | no |
| `SessionReplaced` | io client disconnect | session replaced (§6) or `ResetSession()`; reconnects at once | no (by design: new user) |
| `ServerDisconnect` | io server disconnect | server DISCONNECT | no |
| `PingTimeout` | ping timeout | EIO deadline or WS protocol ping (`By = Heartbeat`) | yes |
| `ParseError` | parse error | protocol error, invalid UTF-8, oversize inbound | yes, but VaRest drops its `pid`s (§4.6) |
| `TransportClose` | transport close | any other connection end (server close, drop, background, restart) | yes |

* **App close and session replacement** (`Socket.Close()`, logout, owner gone, hub shutdown, `Release`,
  session replaced): the closing hook (SW-7) sends DISCONNECT for every connected namespace before the close
  frame, so the server sees *client namespace disconnect*, does not keep the session, and presence logic
  sees "left", not "dropped".
* **Background, `Restart`, environment change, connection loss**: the transport closes without
  DISCONNECT packets, so a recovery-enabled server keeps the session for the next attempt (except
  `SetEnvironment`, which drops `pid`s, §4.6).

### 3.6 What happens at a CONNECT ack

1. Namespace → Connected; `Sid`, `pid`, `bRecovered` (= the ack's `pid` equals the stored one, P11) set.
2. `OnConnect(FVaRestSioConnect {Namespace, Sid, bRecovered, bReconnect, Attempt})` — the channel's
   `OnReady` (WS §6.2); its emits go out first (ready lane).
3. The namespace's backlog flushes (emits made while not connected).
4. Events received **before** the ack (missed packets of a recovered session, P11) are dispatched in
   arrival order (SW-8 inbound hold).
5. Live events.

Socket.IO-client dispatches held events before `connect` and flushes its buffer before the `connect`
handler's emits (P12). VaRest keeps the WS order on purpose: handlers learn `bRecovered` before the
missed events arrive (to decide whether to reset state), and joins sent in `OnConnect` precede queued chat
(WS D11). The "coming from socket.io-client" docs note both differences.

---

## 4. Events

### 4.1 Arguments model

An event is `[name, arg0, arg1, …]` (P8). VaRest binds **by position**, as JavaScript does:

* A C++ handler's parameters bind `arg0, arg1, …`; a trailing `FVaRestSioAck&` parameter receives the ack;
  or a single `const FVaRestSioEvent&` parameter gets everything.
* **Missing** args get the type's default and a decode warning (strict mode: `OnError(Decode)`, the handler
  is skipped); **extra** args are ignored, as JavaScript ignores extra arguments. This also makes the
  recovery offset (an extra trailing string, P11) harmless to typed handlers. It is **visible** in untyped
  views — `Array` mode, `OnAny`, `FVaRestSioEvent::Args`, *Get Event Arg Count* — exactly as in JavaScript;
  `FVaRestSioEvent::Offset` says when the last argument was taken as the offset.
* Asset declarations and BP use one payload type with `EVaRestSioArgs`: `First` (default; payload =
  `arg0`, the common one-object event), `Positional` (struct field *i* = `arg i`, fields in declaration
  order, `TFieldIterator` + M2's `JsonToProperty` / `WritePropertyInternal`, §18.2), `Array` (payload =
  `FVaRestJson` array of all args).

### 4.2 Emit

```cpp
Chat.Emit(TEXT("send_chat"), FChatOut{ Text });                  // ["send_chat",{"text":"…"}]
Chat.Emit(TEXT("move"), X, Y);                                   // ["move",3,4]
Chat.Emit(TEXT("ready"));                                        // ["ready"]
Chat.Emit(TEXT("upload"), FileName, MoveTemp(PngBytes));         // BINARY_EVENT: ["upload","a.png",{placeholder 0}] + 1 frame
Chat.EmitArgs(TEXT("move"), FMoveArgs{ 3, 4 });                  // positional struct → ["move",3,4]
Chat.Volatile().Emit(TEXT("cursor"), Pos);                       // dropped instead of queued
Admin.Emit(TEXT("kick"), PlayerId);                              // namespace "/admin"
```

| C++ argument | Encoded as |
| --- | --- |
| `FString`, `FStringView`, `const TCHAR*`, `FUtf8StringView`, `FName`, `FText` | string (`FText` culture-invariant, core §2.4) |
| `bool`, integers, `float`, `double` | JSON bool / number (int64 above 2^53 loses precision in JavaScript servers; the docs say so) |
| `FVaRestJson` | as is (`WriteJsonInternal`); a missing handle is `null` (core §2.2 array rule) |
| any `USTRUCT` | `WriteStructInternal` (core §2.4 rules and naming; value structs such as `FDateTime` as their single value); `TArray<uint8>` fields become attachments (§5, J-1) |
| `TArray<T>`, `TSet<T>`, `TMap<FString, T>` | array / object (core §2.4) |
| `TArray<uint8>` (by value or moved) | attachment placeholder (§5) |
| `TOptional<T>` unset, `nullptr` | `null` |

Anything else is a `static_assert` naming the supported types. Rules:

* **Reserved names** (P8) → `Rejected` + `Config` at the call; a sent one would kill the connection.
* Empty event names → `Config`. Numeric names exist inbound only (route compare by canonical text, WS §7.3).
* Returns `EVaRestSendResult` (WS §6.1). Drops and rejections reach `OnError`, rate-limited (WS §6.1).
* **Thread contract**: `Emit`, `EmitArgs`, `Volatile().Emit` and `FVaRestSioAck::Send` may be called from
  any thread; they encode from the published snapshot (naming, limits) and hand one batch to WS `SendBatch`
  (any thread, SW-9). `EmitWithAck`, `Namespace()`, configuration and lifecycle are game-thread only and
  `check` it (WS D22).
* **Volatile**: dropped while the namespace is not connected or above WS `SendHighWater`
  (socket.io-client discards when the transport is not writable, P12).

### 4.3 Buffering while disconnected

Per namespace, the WS backlog (WS §6.2): FIFO, `MaxQueued` 256, `MaxQueuedBytes` **4 MiB** for Socket.IO
sockets (WS default 1 MiB; one packet may carry several 1 MB attachments), `DropOldest`; a packet with
attachments is one entry with its total size, and an entry larger than `MaxQueuedBytes` is rejected at once
with `Offline` (documented). The backlog survives reconnects, not `Close()` (`Canceled`). Unlike
socket.io-client's unbounded buffer, memory is bounded; drops are reported (`Offline`).

### 4.4 Receiving

```cpp
Chat.On(TEXT("chat_message"), this, [this](const FChatMessage& M) { Lines.Add(M); });          // arg0
Chat.On(TEXT("move"),  this, [this](int32 X, int32 Y) { MoveTo(X, Y); });                       // arg0, arg1
Chat.On(TEXT("file"),  this, [this](const FString& Name, const TArray<uint8>& Data) { Save(Name, Data); });  // attachment, no copy
Chat.On(TEXT("invite"), this, [this](const FInvite& I, FVaRestSioAck& Ack) { Ack.Send(bAccept); });
Chat.Once(TEXT("welcome"), this, [this](const FWelcome& W) { ShowMotd(W); });                  // removed after the first call
Chat.OnArgs<FMoveArgs>(TEXT("move"), this, [this](const FMoveArgs& A) { … });                  // positional struct
Chat.OnAny(this, [](const FVaRestSioEvent& E) { UE_LOG(LogTemp, Verbose, TEXT("%s"), *E.Name); });
Chat.OnUnhandled(this, [](const FVaRestSioEvent& E) { … });
Chat.Off(TEXT("move"), this);  Chat.OffAny(this);  Chat.OffAll(this);   // OffAll(nullptr) removes owner-less handlers
```

* Handler parameter types: those of §4.2's table plus `const FVaRestSioEvent&`, `FVaRestSioAck&` (last). A
  generic lambda cannot be deduced; a `static_assert` says to name the types (WS §7.4).
* Decode runs on the socket's pipe through a typed decode plan per handler signature (SW-2), from the
  already-parsed DOM; handlers run on the game thread. Owners are weak (WS §7.4).
* Order per event: `OnAny` → `On`/`Once` handlers in registration order → `OnUnhandled` if none.
  (socket.io-client runs `onAny` listeners first too.)
* `FVaRestSioEvent` (BlueprintType): `Namespace`, `Name`, `Args` (`FVaRestJson` array, placeholders intact,
  offset included when present), `NumAttachments`, `Ack`, `Offset`; C++ `Arg<T>(Index)`, `Attachment(Num)`
  (`TConstArrayView<uint8>`).
* An event that requested an ack and reaches no handler taking `FVaRestSioAck` gets no reply (the server's
  ack times out, as with the reference client); Verbose once per event name.

### 4.5 Acks

**Client → server** (`emitWithAck`):

```cpp
Chat.EmitWithAck(TEXT("join"), FJoin{ Room })
    .Timeout(5.f)                    // default 10 s; 0 = wait forever (socket.io-client's default)
    .ErrorFirst()                    // optional: ack(err, data…) convention; non-null arg0 → error
    .Retry(2)                        // optional: re-emit on Timeout / Connection (server must be idempotent)
    .Owner(this)
    .OnSuccess([this](const FJoinReply& R) { ShowRoom(R); })   // ack args by position, like On
    .OnError([this](const FVaRestError& E) { ShowError(E); })
    .Send();
```

* **Ack ids are `int64`**, accepted up to 2^53 − 1 in both directions (server ids come from a
  per-namespace JS counter, P13); a longer digit run is a protocol error. Outbound ids come from one
  socket-wide counter on the game thread (the server echoes the id together with the namespace; per-namespace
  numbering is a reference-client detail, not protocol). Pending acks live in the WS pending table keyed by
  id with a namespace check (SW-1); shared limit `MaxPendingRequests` 1024 per socket (`Offline` over it).
* Only ACK/BINARY_ACK packets resolve pending acks (SW-1 `Kind`): a server EVENT with the same id never does
  — both counters start at 0, so the collision is the normal case.
* Timeout runs from `Send()`; an ack still in the backlog at timeout leaves it (WS §7.5; socket.io-client
  does the same). Sent and then the connection drops → `Connection`, retryable. Late ack → Verbose drop.
  **Every** ack ends: success, error or timeout (the reference client drops plain-callback acks silently on
  disconnect, P12).
* `ErrorFirst()`: `arg0` not null → `Kind=Server`, `Message` = `arg0.message` or `arg0` as text, `Body` =
  `arg0`; success args start at `arg1`. Without it, every ack is a success.
* `Retry(N)` re-emits with a new id after `Timeout` or `Connection`, at most N times, unordered. The
  reference client's `retries` option serialises its queue; VaRest does not (simpler; documented).

**Server → client**: an EVENT with an id gives the handler an `FVaRestSioAck` (parameter, or
`FVaRestSioEvent::Ack`). `Ack.Send(Args…)` encodes ACK or BINARY_ACK with the same id and namespace through
the WS reply path (`EncodeReply` returning a batch), **pinned** to the event's connection and namespace
(SW-9): if either changed by flush time, the reply is dropped (Verbose) — a reply on a new connection would
be an unknown id at best and an invalid-state close at worst (P10). Callable later and from any thread
(async replies after an HTTP call). At most once: `FVaRestSioAck` holds a
`TSharedPtr<FVaRestSioAckState, ESPMode::ThreadSafe>` (not a `UPROPERTY`) with the atomic "replied" flag, the
weak socket, namespace, id and pin, so every BP or C++ copy shares one state; a second reply logs a warning
and returns `Rejected`. A test replies through two BP copies.

### 4.6 Connection state recovery

On by default (`WithRecovery(false)` disables); it costs nothing against a server without it.

* Per namespace VaRest stores `Pid` (CONNECT ack `pid`) and `LastOffset`. After dispatching an EVENT **or
  BINARY_EVENT** without ack id whose last argument is a string, while `Pid` is set, that string becomes
  `LastOffset` (socket.io-client's rule, P11). The argument is **not** removed (§4.1).
* Reconnect CONNECT carries `{pid, offset}` when both exist (§3.3). The server restores rooms, `data` and
  missed packets, sends the missed events before its ack, and the ack echoes the `pid` → `bRecovered =
  true`; missed events are dispatched after `OnConnect` (§3.6).
* Known ambiguity, same as the reference client: a volatile server event (no offset appended) whose last
  argument is a string is taken as an offset; the server then finds no such packet and starts a fresh
  session (`bRecovered = false`). Recovery can fail, never deliver wrong data; [CSR] says apps must handle
  `recovered = false` anyway (BP: *On Connect* has a `Recovered` pin).
* **`Pid`s are dropped** (the next connect is a fresh session) on: `Socket.Close()`, `Ns.Disconnect()`,
  logout, **session replaced** and `ResetSession()` (§6), `SetEnvironment`, give-up, **every VaRest-initiated
  close caused by inbound content** — 1002 (protocol error), 1007 (invalid UTF-8), 1009 (oversize) — because
  recovery would replay the offending packet on every reconnect, and inbound-hold overflow (§12).
* `pid` is a secret (it lets a client take over a session — recovered sessions skip auth middleware by
  default, P11 `skipMiddlewares: true`): redacted in logs and traces (§10), never persisted, never reused
  across users.
* Multi-node deployments with the in-memory adapter keep sessions per node: a reconnect that lands on
  another node gets `bRecovered = false` (documented, §15).

---

## 5. Binary attachments

### 5.1 Encode

* `TArray<uint8>` arguments, and `TArray<uint8>` values inside structs and containers through the M2
  encode hook `BytesWriter` (J-1), are written as `{"_placeholder":true,"num":k}` (key order as the reference
  encoder) and their bytes collected, in walk order; the packet becomes BINARY_EVENT / BINARY_ACK with `k`
  attachments (P9). Other byte encodings (number array, base64) remain available per call through the core's
  options.
* **One pass, no copy of the JSON**: the encoder reserves a header gap after `LWS_PRE`, sized for `4` + type +
  the largest attachment count + `-` + namespace + `,` + ack id digits; it writes the JSON body, then writes
  the now-known header right-aligned into the gap; the frame starts where the header starts (SW-4: payload
  offset ≥ `LWS_PRE`). Attachment counts change the header length (`459-` vs `4510-`); a test covers 1, 9
  and 10.
* More than `MaxAttachments` (10, the server decoder's default, P9) → `Config` at the call.
* Copies: a moved `TArray<uint8>` argument goes through the WS binary path (one copy into a headroom buffer,
  WS §2.4); a struct field's bytes are copied once by the hook (the struct stays the caller's).

### 5.2 Decode

The per-connection decoder (SW-2), created on the connection's Open event, runs on the socket's pipe:

1. A text EIO message `5<n>-…` / `6<n>-…` starts a packet: `n` is an integer in `[1, MaxAttachments]`, else
   protocol error.
2. The next `n` **binary** frames are its attachments. An EIO control packet (`2` ping) in between is legal
   and handled normally (the parser sees only message packets); a text **message** in between, or a binary
   frame with no packet pending, is a protocol error (P9).
3. When complete: every placeholder `num` must be an integer in `[0, n)` (P9) — else protocol error; unused
   attachments are allowed. Then typed decode plans run, so `TArray<uint8>` parameters and fields bind to
   attachments (M2 `BytesResolver`, WS §19 item 10) before the game thread sees the event.
4. Held bytes are bounded by `MaxPacketBytes` and the §2.5 invariant keeps reassembly clear of flow control.
5. After a protocol error the decoder is poisoned: it drops the rest of that connection's frames (SW-2).

`TArray<uint8>` decode accepts a placeholder, a number array or a base64 string (core options). Copies:
**0** for `const TArray<uint8>&` handler parameters and `Event.Attachment(Num)` (views of the shared
attachment buffer), **1** for a struct field (the resolver fills the field). An `FVaRestJson` view keeps
placeholders as objects.

### 5.3 Ordering

The WS transport keeps wire order across text and binary (WS §2.1, §7.1), so a header and its attachments
arrive in order. Outbound, a packet's frames are queued, dropped and written as one unit (SW-9); only WS
control frames and auto-replies (`3`) can sit between them, which the server's Engine.IO layer handles
below the Socket.IO parser (§2.4).

---

## 6. Auth and session

A Socket.IO socket shares an HTTP client's session exactly as WS §3.3: `WithApi(GameApi)`,
`WithClient(Client)`, automatic for asset entries and components, or `WithBearer(TFunction<FString()>)`
code-first.

| Placement (`FVaRestSioAuthPlacement`) | Where the token goes | Server reads |
| --- | --- | --- |
| `Payload(Field = "token", Prefix = "")` — **default with a session** | CONNECT payload of each namespace with `bSendSession` (default true) | `socket.handshake.auth.token` (the documented Socket.IO way) |
| `Header` | `Authorization: Bearer …` on the upgrade (WS) | `socket.handshake.headers.authorization` |
| `Query(Name = "token")` | upgrade query (WS) | `socket.handshake.query.token` |
| `None` | — | |

Session changes (WS §22 A1: the client distinguishes `Refreshed`, `Replaced`, `Cleared`):

| Change | Socket.IO behaviour | Why |
| --- | --- | --- |
| `Refreshed` (same user, new token) | nothing for connected namespaces; later CONNECTs use the new token | the server checked them at CONNECT (WS D19) |
| `Replaced` (`SetBearerToken`/`ImportSession` for another identity, login while logged in) | DISCONNECT per namespace (closing hook), drop every `pid`, restart with the new credentials (`SessionReplaced`, §3.5) | a recovered session skips auth middleware (P11): user B must never resume user A's session |
| `Cleared` (logout) | DISCONNECT every namespace, close 1000 "logout", drop `pid`s, no reconnect | WS §3.3 |

* A `WithBearer` lambda is opaque: VaRest cannot tell a refresh from a user switch through it, so code-first
  apps call `Socket.ResetSession()` (BP *Reset Session*) when the user changes — the `Replaced` behaviour on
  demand.
* Proactive refresh before each attempt and each CONNECT (token expiring within 30 s), single-flight with HTTP
  (WS §3.3, core §4.2).
* CONNECT_ERROR → one refresh and retry (§3.4); upgrade 401 / close 4401 → WS §3.3; upgrade 400 with a Header
  or Query placement → one refresh and retry (§2.6).

---

## 7. C++ surface

Asset entry, one statement:

```cpp
Chat = FVaRestSio::Socket(this, GameApi, TEXT("Chat"))
    .On(TEXT("chat_message"), this, [this](const FChatMessage& M) { Lines.Add(M); })
    .OnConnect(this, [this](const FVaRestSioConnect& C) { Chat.Emit(TEXT("join"), Room); })   // every (re)connect
    .OnDisconnect(this, [this](const FVaRestSioDisconnect& D) { ShowBanner(D.bWillReconnect); })
    .Connect();
```

Code-first, sharing the API session, two namespaces:

```cpp
Game = FVaRestSio::Make(this, TEXT("https://rt.game.io"))
    .WithApi(GameApi)                                                   // token in the CONNECT payload
    .On(TEXT("tick"), this, [this](const FWorldTick& T) { Apply(T); })
    .Connect();

Admin = Game.Namespace(TEXT("/admin"))
    .WithAuth(FVaRestJson::Object().Set(TEXT("role"), TEXT("gm")))
    .On(TEXT("kick"), this, [this](const FString& PlayerId, const FString& Reason) { Kick(PlayerId, Reason); })
    .OnConnectError(this, [this](const FVaRestError& E, const FVaRestSioConnectError&) { HideAdminUi(); })
    .Connect();
```

Handle shapes (shape, not final signatures):

```cpp
class VARESTSOCKETIO_API FVaRestSioNamespace
{
public:
    // Configuration (game thread)
    FVaRestSioNamespace WithAuth(FVaRestJson Payload);      FVaRestSioNamespace WithSessionAuth(bool bSend);
    FVaRestSioNamespace OnConnectAuth(const UObject* Owner, TFunction<void(FVaRestJson&, const FVaRestSioConnectContext&)>);
    FVaRestSioNamespace ConnectErrorIf(TFunction<EVaRestSioRetry(const FVaRestSioConnectError&)>);

    // Events (Owner weak; overloads without Owner for free functions, removed by OffAll(nullptr))
    FVaRestSioNamespace OnConnect(const UObject* Owner, TFunction<void(const FVaRestSioConnect&)>);
    FVaRestSioNamespace OnConnectError(const UObject* Owner, TFunction<void(const FVaRestError&, const FVaRestSioConnectError&)>);
    FVaRestSioNamespace OnDisconnect(const UObject* Owner, TFunction<void(const FVaRestSioDisconnect&)>);
    template <typename F> FVaRestSioNamespace On(FStringView Event, const UObject* Owner, F&& Handler);
    template <typename F> FVaRestSioNamespace Once(FStringView Event, const UObject* Owner, F&& Handler);
    template <typename TArgs, typename F> FVaRestSioNamespace OnArgs(FStringView Event, const UObject* Owner, F&& Handler);
    FVaRestSioNamespace OnAny(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)>);
    FVaRestSioNamespace OnUnhandled(const UObject* Owner, TFunction<void(const FVaRestSioEvent&)>);
    void Off(FStringView Event, const UObject* Owner);  void OffAny(const UObject* Owner);  void OffAll(const UObject* Owner);

    // Lifecycle (game thread)
    FVaRestSioNamespace Connect();                           void Disconnect();

    // Sending: Emit / EmitArgs / Volatile().Emit from any thread; EmitWithAck on the game thread
    template <typename... A> EVaRestSendResult Emit(FStringView Event, A&&... Args);
    template <typename TArgs> EVaRestSendResult EmitArgs(FStringView Event, const TArgs& Args);
    FVaRestSioEmitter Volatile() const;
    template <typename... A> [[nodiscard]] FVaRestSioEmitWithAck EmitWithAck(FStringView Event, A&&... Args);

    // State
    EVaRestSioNamespaceState GetState() const;  bool IsConnected() const;  FString GetSid() const;
    bool WasRecovered() const;  int32 GetQueuedCount() const;  int32 GetPendingAcks() const;
    FVaRestSioStats GetStats() const;  FStringView GetName() const;  FVaRestSioNamespaceWeak AsWeak() const;
};

// + every FVaRestSioNamespace member, applied to the default namespace and returning FVaRestSioSocket for
// chaining — except Connect (transport + default + start namespaces, §3.1) and Disconnect (use Close).
class VARESTSOCKETIO_API FVaRestSioSocket
{
public:
    FVaRestSioSocket WithPath(FString Path);                  FVaRestSioSocket WithQuery(FString Key, FString Value);
    FVaRestSioSocket WithHeader(FString Name, FString Value); FVaRestSioSocket WithOrigin(TOptional<FString> Origin);
    FVaRestSioSocket WithApi(UVaRestApi* Api);                FVaRestSioSocket WithClient(FVaRestClient Client);
    FVaRestSioSocket WithBearer(TFunction<FString()> Token);  FVaRestSioSocket WithAuthPlacement(FVaRestSioAuthPlacement);
    FVaRestSioSocket WithReconnect(FVaRestReconnectPolicy);   FVaRestSioSocket NoReconnect();
    FVaRestSioSocket WithRecovery(bool bEnabled);             FVaRestSioSocket WithAckTimeout(float Seconds);
    FVaRestSioSocket WithProtocolPing(float Interval, float Timeout); // WS PING frames; the EIO heartbeat is the server's
    FVaRestSioSocket WithLimits(FVaRestSioLimits);            FVaRestSioSocket WithQueue(FVaRestSocketQueuePolicy);
    FVaRestSioSocket WithNaming(EVaRestJsonNaming);           FVaRestSioSocket WithLogLevel(EVaRestLogLevel);
    FVaRestSioSocket WithStrictDecode(bool bStrict);          FVaRestSioSocket Owner(const UObject* Owner);  // unnamed only

    FVaRestSioNamespace Namespace(FStringView Name) const;    FVaRestSioNamespace DefaultNamespace() const;
    FVaRestSioSocketWeak AsWeak() const;

    // Connection-level events (WS §9 semantics)
    FVaRestSioSocket OnRetryScheduled(const UObject*, TFunction<void(const FVaRestSocketStatus&, const FVaRestSocketClose&)>);
    FVaRestSioSocket OnGaveUp(const UObject*, TFunction<void(const FVaRestError&, const FVaRestSocketClose&)>);
    FVaRestSioSocket OnStateChanged(const UObject*, TFunction<void(EVaRestSocketState, EVaRestSocketState)>);
    FVaRestSioSocket OnError(const UObject*, TFunction<void(const FVaRestError&)>);

    // Lifecycle (game thread)
    FVaRestSioSocket Connect();  void Close();  void ReconnectNow();  void StopReconnecting();
    void ResetSession();         // user switch through an opaque WithBearer: DISCONNECT all, drop pids, reconnect (§6)

    // State
    EVaRestSocketState GetState() const;  FVaRestSocketStatus GetStatus() const;  double GetLatency() const;
    FVaRestSioStats GetStats() const;  FName GetName() const;  FString GetRedactedUrl() const;
};
```

`FVaRestSioEmitWithAck`: `Timeout`, `ErrorFirst`, `Retry`, `Owner`, `OnSuccess(F)` (parameters deduced like
`On`), `OnError`, `Send()` → `FVaRestSioAckHandle` (`Cancel()`, `IsActive()`); `[[nodiscard]]`, by-value
methods and the unsent-builder error of core §3.2.

---

## 8. Blueprint surface

### 8.1 `UVaRestSioClient`

Namespaces are a **string parameter** (empty = default namespace), not objects: most games use one
namespace, and events carry the name.

* Functions: Connect, Close, Reconnect Now, Stop Reconnecting, Reset Session, Emit (Event, wildcard Payload,
  Namespace, Volatile), Emit Args (Event, Json array, Namespace, Volatile), Connect Namespace (Name, Auth Json),
  Disconnect Namespace, Is Namespace Connected, Get Sid, Was Recovered, Set Auth Payload (Namespace, Json), Set
  Query Param, Set Header, Get State, Get Status, Get Latency, Get Stats, Get Queued Count, Clear Queue.
* Events: On Connect (`FVaRestSioConnect`), On Connect Error (Error, `FVaRestSioConnectError`), On Disconnect
  (`FVaRestSioDisconnect`), On Event (catch-all `FVaRestSioEvent`), On Retry Scheduled, On Gave Up, On State
  Changed, On Error.

### 8.2 Library and async nodes

* `UVaRestSioLibrary`: Get Socket.IO (WorldContext, Api, Entry) — offered by the spawner as
  *Socket.IO ▸ DA_GameApi ▸ Chat*; Make Socket.IO (WorldContext, Url, **Namespace**, Headers, Auth Json,
  optional Name; ownership per §1.3; an explicit Namespace pin overrides the URL-path rule of §2.2); Ack Reply
  (Ack, wildcard Payload, CustomThunk); Ack Reply Args (Ack, Json array); Is Ack Valid; Event Arg To Struct
  (Event, Index, wildcard; walks the already-parsed DOM, like WS *Socket Message To Struct*); Get Event Arg
  Count; Get Event Attachment (Event, Num); Describe Disconnect.
* Async **Connect Socket.IO** (client or asset entry): `On Connect` fires on **every** connect of the default
  namespace with `Is Reconnect` and `Recovered` — joins go there; `On Failed` (Error, Connect Error) for any
  end before the first connect: refusal, give-up, `Close`, owner gone.
* Async **Emit With Ack** (no asset): Client, Event, Namespace, wildcard Payload, Timeout, Error First,
  `Reply Type` picker (like *Send Request*); `On Ack` (typed Reply, Args Json), `On Error`.

### 8.3 K2 nodes (`VaRestEditor`, on the WS declaration nodes, SW-11)

The entry's declared events drive typed nodes; the spawner offers *VaRest ▸ DA_GameApi ▸ Chat ▸ /admin ▸
On kick* (the namespace level is omitted for the default namespace). Asset, entry, namespace and event are
node properties, refreshed on asset change; orphaned pins kept and flagged (core §5.4).

* **On Socket.IO Event** (`In`, `InWithAck`): exec in (start listening), `Then`, `On Event`, typed `Payload`
  (per `ArgsMode`), `Event` (`FVaRestSioEvent`), **`Ack`** (valid when the server asked for one;
  `InWithAck` declarations show it always), listen handle with `Stop`, and a `bOnce` property (stop after the
  first event). Without an asset: client pin, Event, Namespace, `Payload Type` picker.
* **Emit Socket.IO Event** (`Out`): typed payload from the declaration, or wildcard arg pins with *Add pin*
  (one pin per argument, the *Make Json* mechanism), Volatile.
* **Emit With Ack** (`OutWithAck`): typed payload; `On Ack` with typed `Reply` (`ReplyType`,
  `ReplyArgsMode`, `bErrorFirstReply` from the declaration), `On Error`.

### 8.4 `UVaRestSioComponent`

`BlueprintSpawnableComponent`, subclass of WS `UVaRestSocketComponent` (SW-10), the drop-on-an-actor entry
point users of getnamo's plugin look for.

| Property | Default | Meaning |
| --- | --- | --- |
| `Api` + `Entry` | none | the asset entry; `Entry` is a dropdown of the asset's Socket.IO entries (details customisation in `VaRestEditorTools`). The component then uses **the entry's hub-named socket** — the same one *Get Socket.IO* and the typed nodes use — so it survives level travel like any asset socket |
| `bCloseOnEndPlay` | false | entry mode: close the entry's socket when this actor ends play |
| `Url` + `Namespace` | empty, `/` | code-free alternative when there is no asset |
| `Scope` (URL mode) | `Actor` | `Actor`: the component owns an unnamed socket, closed on `EndPlay`. `GameInstance`: a hub-named socket (`Name`), shared by every component and node using that name, kept across level travel, closed at game-instance shutdown or `Release` — the equivalent of getnamo's plugin-scoped connection |
| `bAutoConnect` | true | connect on `BeginPlay` |
| `Namespaces` | empty | extra namespaces connected with the socket |

`BlueprintAssignable` events: On Connect, On Disconnect, On Connect Error, On Event, On Gave Up (bound in the
details panel like any component event). `GetClient()` returns the `UVaRestSioClient` for every function of
§8.1. The typed K2 nodes have an optional `Client` pin: unconnected, they use the entry's hub-named socket
(the component's socket in entry mode); for a URL-mode component, wire `Get Client`.

Designer flow: drop *VaRest Socket.IO* on the player controller, pick `DA_GameApi ▸ Chat` → **On Connect**
(details panel) → *Emit With Ack join* → **On Ack** (typed); *On chat_message* → widget; *On invite* → `Ack`
→ *Ack Reply* (true); a button → *Emit send_chat*; bind *On Disconnect* for a banner.

---

## 9. API asset integration

`UVaRestSioEndpoint` subclasses `UVaRestSocketEndpointBase` (SW-10), so it inherits the connection fields
(Name, Description, `Path`, Query, Headers, Origin, `bNoAuth`, overrides for reconnect, protocol ping,
limits, queue and background policy) but not WS routing, `Messages`, the WS auth placement or the app
heartbeat override; it declares its own placement (§6). Designers press "+" in `UVaRestApi::Sockets` and
pick *Socket.IO Socket*.

| Field | Default | Meaning |
| --- | --- | --- |
| `Path` | `/socket.io/` | engine path, joined to `SocketBaseUrl` (else `BaseUrl`, scheme mapped) as core URL building (base + path); never a namespace; trailing slash always added |
| `DefaultNamespace` | `/` | the socket's default namespace, always connected by `Socket.Connect()`; must name an entry of `Namespaces` when that list is not empty |
| `Namespaces` | empty (= implicit `/`) | `{Name, bConnectOnStart (true; ignored for the default namespace), AuthPayload (Json text), bSendSession (true), Events}` |
| `Events` (per namespace) | | `{Event, Direction (In / Out / OutWithAck / InWithAck), PayloadType (struct or none = Json), ArgsMode (First), ReplyType, ReplyArgsMode, bErrorFirstReply, Description}` |
| `bRecovery`, `AckTimeout` | true, 10 s | §4.6, §4.5 |
| auth placement | `Payload("token")` | §6 |

`IsDataValid`: namespace names (leading `/`, no `,` or `?`, no duplicates), `DefaultNamespace` present (and a
warning when its `bConnectOnStart` is false, since it is ignored), event names non-empty and not reserved,
duplicates per (namespace, direction), `Positional` needs a struct payload, reply fields only on
`OutWithAck`, `Path` without query. `SetEnvironment` restarts open sockets of the asset and drops recovery
state (WS §11, §4.6).

---

## 10. Observability

**Logging** (`LogVaRestSio`; level per socket, default from settings; core cvar `VaRest.LogLevel`):

| Level | Logs |
| --- | --- |
| `Basic` | WS lifecycle (WS §12) + namespace connect (sid, recovered), connect error (message), disconnect (reason, `bWhileReadPaused`), version, path and protocol errors, the heartbeat-budget warning (§2.4) |
| `Headers` | + handshake (WS) and CONNECT payload **keys** (values always `***`) |
| `Body` | + every packet: direction, EIO/SIO type, namespace, event, ack id, attachment count and sizes, JSON excerpt (4 KiB, redacted); compiled out in Shipping |

Redaction: core §4.5 rules, plus the whole CONNECT payload's values, `pid`, and the EIO OPEN `sid` ("must not
be shared with others parties, as it might lead to session hijacking", [ref] `engine.io/lib/socket.ts#L78-L79`).
Namespace socket ids (`Sid`) are public ([CSR]) and logged.

**Trace tap**: WS socket events (WS §12) with the generic protocol tag and channel events of WS rev 2 (WS §22
A4): `SocketFrame.Protocol` = `eio:ping`, `sio:EVENT`, `sio:BINARY_ACK`, … with `Attachments`; `SocketChannel`
up / down / refused for namespaces with reason and `bRecovered`. The Inspector shows the socket row with a
namespace column; `eio:*` frames are filtered by default.

**Stats**: `FVaRestSioStats` per socket and per namespace: connected namespaces, events in/out, acks pending /
timed out, connect errors, recoveries ok/failed, pings answered (by the auto-reply) and the longest pong delay,
attachments and attachment bytes in/out. `stat VaRest` gains Socket.IO lines (namespaces, events, pending
acks).

**Console** (not Shipping): `VaRest.Sio.List` (sockets, namespaces, state, sid, recovered, pending acks); WS
`VaRest.Socket.Drop <Name>` works on Socket.IO sockets (unclean drop → recovery path).

---

## 11. Errors

`FVaRestError` (core §3.3); lifecycle details travel in the §3 structs.

| Situation | `Kind` |
| --- | --- |
| transport failures (WS §14) | as WS |
| terminal CONNECT_ERROR; server DISCONNECT of the last namespace; error-first ack error | `Server` (`Code` ← `data.code`, `Body` ← `data`) |
| refresh failed after CONNECT_ERROR or upgrade 400 | `Auth` (+ `OnSessionExpired`) |
| upgrade 400 (terminal) | `Http` (status 400, §2.6 message) |
| "Invalid namespace"; not an Engine.IO endpoint; no Engine.IO server at the path (3× no upgrade response); v2 server; reserved or empty event name; bad namespace name; frame over `maxPayload`; over `MaxAttachments`; `MaxPacketBytes` invariant violated | `Config` (never retried) |
| protocol violation; attachment mismatch; oversize inbound; strict typed decode failure | `Decode` |
| ack timeout; namespace CONNECT timeout (`OnConnectError`, will retry); missing OPEN; EIO deadline | `Timeout` |
| sent ack whose connection dropped | `Connection` (`bRetryable`) |
| backlog drop/reject (including one entry over `MaxQueuedBytes`); over `MaxPendingRequests` | `Offline` |
| `Close()`, `Ns.Disconnect()`, owner gone, `Cancel()` | `Canceled` |

---

## 12. Limits and defaults

Socket.IO sockets use WS defaults (WS §15) except where listed.

| Setting | Default | Why |
| --- | --- | --- |
| Engine path | `/socket.io/`, trailing slash always | server default, P5 |
| Handshake query | `EIO=4&transport=websocket` (user `EIO`/`transport`/`sid` stripped) | P2, §2.2 |
| `ConnectTimeout` / `ReadyTimeout` / namespace CONNECT timeout | 20 s / 20 s / 20 s, hitch rule applies | socket.io-client `timeout` (P12); middleware latency |
| Reconnect | 1 s × 2 → 5 s, ±50 %, unlimited, `StableAfter` 5 s | socket.io-client parity (§2.6) + WS stable rule |
| EIO deadline | server's `pingInterval + pingTimeout` (45 s default), data frames only, suspended while reading is paused | P3, §2.4 |
| WS protocol ping | 20 s / 10 s, suspended while reading is paused | WS R3 |
| `SO_SNDBUF` on Socket.IO connections | 256 KiB | bounds the pong delay (§2.4) |
| Outbound per frame | advertised `maxPayload`, or 1 000 000 when absent; ≤ WS `MaxSendBytes` 16 MiB | P2, P4 |
| Ack timeout | 10 s (0 = none) | games need an answer; WS request default |
| Ack ids | `int64`, ≤ 2^53 − 1 | P13 |
| `MaxPendingRequests` (acks, per socket) | 1024 | WS |
| `MaxAttachments` (in and out) | 10 | server decoder default, P9 |
| `MaxMessageBytes` inbound per frame / `MaxPacketBytes` | 8 MiB / 16 MiB | §2.5 |
| `MaxPendingInboundBytes` | 32 MiB; invariant `MaxPacketBytes ≤ MaxPendingInboundBytes / 2` | §2.5 (replaces a hook) |
| Backlog per namespace | 256 / 4 MiB, `DropOldest` | §4.3 |
| Inbound hold before a CONNECT ack, per namespace | 4096 events / 16 MiB; overflow → drop `pid`s, restart (fresh session) | recovery can replay 2 min of a busy room |
| Re-CONNECT after DISCONNECT on one connection | max(WS RTT, 100 ms) | P10 `nextTick` |
| No upgrade response before stop | 3 in a row | §2.6 |
| Protocol errors before stop | 3 in a row without `StableAfter` | §2.3 |
| Namespace / event name | ≤ 256 bytes | sanity bound |
| Recovery | on | §4.6 |

In code: `FVaRestSioLimits`, plus the WS structs. `UVaRestSioSettings` holds project defaults.

---

## 13. Threading

| Work | Thread |
| --- | --- |
| EIO pong auto-reply and its priority, EIO dead-link deadline, deadline suspension | VaRest service thread (SW-5) |
| EIO/SIO parse, attachment assembly, placeholder checks, typed decode plans | the socket's pipe (WS §7.1), decoder per connection (SW-2) |
| OPEN handling, namespace state machine, CONNECT payload and auth, CONNECT_ERROR policy, inbound hold, recovery offsets, ack ids and tables, handlers, BP events | game thread |
| `Emit`, `EmitArgs`, `Volatile().Emit`, `FVaRestSioAck::Send` encode (header gap, placeholders) | caller's thread, from the published snapshot, into WS `SendBatch` (SW-9) |
| `EmitWithAck`, `Namespace()`, configuration, lifecycle | game thread (`check`) |

Snapshots read by encoders: naming policy and limits. The effective `maxPayload` is enforced as the WS send
limit at flush on the game thread (SW-9), so frames queued across connections meet the current server's
value. Nothing is parsed on the game thread (WS §4).

---

## 14. Verification

### 14.1 Unit tests (`VaRest.SocketIO.*`)

On the WS fake transport (WS §16.1), the core virtual clock with a seeded RNG, and the synchronous decode
executor, with `FVaRestSioScriptedServer` — a helper that scripts the server side in protocol terms
(`Server.Open({PingInterval = 300})`, `Server.ExpectText(TEXT("40"))`, `Server.Ack(TEXT("/"), 3, …)`).

* `Codec.Eio`, `Codec.Sio`: **golden vectors** `Tools/testserver/sio_vectors.json` — every encoded example of
  [SIO] §Examples verbatim (`0`, `0/admin,{"sid":…}`, `4{"message":"Not authorized"}`, `2["foo"]`,
  `2/admin,["bar"]`, `51-["baz",{…}]`, `52-/admin,[…]`, `212["foo"]`, `3/admin,13["bar"]`, `61-15["bar",{…}]`,
  `1`, `1/admin,`), the official suite's frames, and `2/admin,9007199254740991["x"]` with its ACK echo — decoded
  and re-encoded byte for byte; invalid inputs from the official suite (`4abc`, `42{}`, `42abc[…]`), a 17-digit
  ack id, placeholder `num` out of range / negative / non-integer, 11 attachments, reserved names, comma or `?`
  in a namespace.
* `Binary`: assembly, ping between header and attachments, text message mid-assembly, stray binary, poisoned
  decoder, header gap with 1 / 9 / 10 attachments, typed `TArray<uint8>` fields and parameters, copy counts,
  the `MaxPacketBytes` invariant.
* `Heartbeat`: auto-reply rule in the connect command (text only), stage fallback for unflagged pings, pong
  ahead of queued data at the next message boundary, deadline = interval + timeout on data frames only,
  suspension while reading is paused, game-thread stall with the server's short `pingTimeout`, no EIO ping
  ever sent, the heartbeat-budget warning.
* `Namespace`: every cell of §3.2, unrequested acks, namespace gate, last-wanted rule, leave-on-ack (also with a
  transport loss), **`/` connected while `/never` times out — `/` stays up and no restart happens**, re-CONNECT
  delay, default namespace from the URL path, URL hygiene, hitch rule on timeouts.
* `Auth`: payload merge order, placements, per-CONNECT hook, refresh-and-retry once (payload: same connection,
  any refusal; header/query: restart, auth-looking refusals only), refresh failure → `Auth` + session expiry,
  refusal after a successful refresh → `Server`, "Invalid namespace" → `Config`, upgrade 400 with a header
  placement, session `Replaced` and `ResetSession()`.
* `Recovery`: offset tracking on EVENT and BINARY_EVENT, `{pid, offset}` only when both known, `bRecovered`,
  held events after `OnConnect`, every `pid` drop of §4.6 (session replaced, 1002, 1007, 1009, hold overflow),
  the volatile ambiguity pinned.
* `Ack`: socket-wide int64 ids, only ACK resolves (a server EVENT id 0 against our pending 0), timeout incl.
  leaving the backlog, connection loss, late ack, `ErrorFirst`, `Retry`, inbound at-most-once through two BP
  copies, reply pin (namespace gone, new connection), BINARY_ACK both ways.
* `Emit`: every §4.2 type, reserved and empty names, `maxPayload` rejection with the connection kept,
  1 000 000 fallback when the OPEN has none, re-check at flush, `MaxAttachments`, volatile, backlog limits per
  namespace incl. one oversize entry, off-thread emit and batch contiguity.
* `Handlers`: positional binding, missing/extra args, strict mode, ack parameter, `Once`, `OffAny`,
  `OffAll(nullptr)`, `OnAny`/`On`/`OnUnhandled` order, owner gone, weak handles.
* `Version`: first frame not OPEN, invalid OPEN, **OPEN without `maxPayload` accepted with the 1 000 000
  fallback and no v2 warning**, CONNECT ack without `sid`, string CONNECT_ERROR, upgrade 400 message, no
  upgrade response ×3 → `Config`.
* `Close`: DISCONNECT per namespace on `Close()`/logout/session replaced, none on background/restart/drop.

### 14.2 Test server (`Tools/testserver/sio.py`, Python 3 stdlib) — the default suite

Built on WS `ws.py` (WS §16.3): registers the upgrade path `/socket.io/` (and `/sub/socket.io/` for path
tests) in the same `ThreadingHTTPServer`, so the `/auth` namespace validates tokens issued by the core server's
`/login` and `/refresh`. Like `ws.py`, it inserts its own directory into `sys.path` (for `python3 -I`) and runs
alone too. Per connection: the `ws.py` reader thread, a lock-guarded writer, a timer thread for server pings.
Expect 1000–1200 lines including `sio_selftest.py`'s stdlib client.

* **Engine.IO v4 server side**: OPEN with configurable `pingInterval`, `pingTimeout`, `maxPayload` (query knobs;
  `--suite` sets the official suite's 300 / 200 / 1e6 / `connectTimeout` 1000; `nomaxpayload=1` omits the field
  as socket.io 3.0–4.4 do); server pings and pong deadline; closes on a client ping ("invalid heartbeat
  direction"); frame over `maxPayload` → close 1009; **`abortUpgrade` fidelity**: every pre-upgrade refusal is
  `400 Bad Request` with a `text/html` body (`Transport unknown`, `Bad request`, `Forbidden`, or a custom
  `allowRequest` message), and `EIO=3` gets the raw 400 bytes after the 101; a non-matching path gets no
  response and is closed after 1 s, as the reference `destroyUpgrade` does; `throttle=<bytes/s>` slows its reader
  (pong-priority test).
* **Socket.IO v5 server side**, enforcing the reference rules VaRest must respect: invalid state (P10, including a
  CONNECT read in the same receive as the DISCONNECT for that namespace), invalid payloads and reserved names,
  attachment limits and placeholder checks (P9), `connectTimeout` (P7), recovery with offsets appended to
  non-ack, non-volatile EVENTs and BINARY_EVENTs, missed packets written before the ack, the
  recoverable-reason list (P11); `norecovery=1` turns recovery off.
* **Namespaces**: `/` and `/custom` (official-suite behaviour: emit `auth` with the handshake auth, `message` →
  `message-back`, `message-with-ack` → ack args), `/auth` (token check; `{"message":"unauthorized","data":{"code":401}}`),
  `/refuse`, `/kick?after=ms`, `/binary` (echo attachments, server binary events, `interleave=1` puts a ping
  between header and attachments), `/asks` (server emits with ack, plain and binary, records replies),
  `/recover` (counter stream, recovery on), `/flood`, `/large`, `/slowconnect?ms=`, `/never` (no CONNECT reply).
  Knobs `eio3sim=1` (v2-style OPEN, unprompted `40`, string CONNECT_ERROR), `badopen=1`.
* **Control events** on any namespace: `["__test", {"op": …}]` with `drop` (unclean TCP close), `close` (WS 1000),
  `eioclose` (send `1`), `stopPing`, `kick`, `emit` (count, args, volatile, binary), `poison` (11 attachments, or a
  bad placeholder, appended to the recovery store), `report` (ack with the server's view: last disconnect reason
  per session, pongs and their delay, packets received).
* `Tools/testserver/sio_selftest.py` checks the server with a stdlib client before UE tests rely on it.
* **Official compliance suite**: `Tools/testserver/run_sio_suite.sh` fetches `socketio/socket.io-protocol` at a
  pinned commit, `npm ci`, starts `sio.py --suite --port 3000` and runs mocha with
  `--grep "HTTP long-polling|upgrade" --invert`. About 23 of its 32 tests apply (handshake, heartbeat, close,
  connect/disconnect for `/` and `/custom`, message and ack echo, binary echo, three invalid-format closes); it
  covers none of recovery, server-initiated acks, middleware CONNECT_ERROR, server DISCONNECT, invalid state,
  `maxPayload` or concurrent namespaces — which is why the interop gate (§14.4) exists. Run when `sio.py`
  changes; result in `docs/passport/`.

### 14.3 Integration (`VaRest.Integration.SocketIO.*`)

Against `sio.py`, ws and wss (WS TLS listener): every namespace and control scenario above, auth refresh end to
end, recovery after `drop` with the missed counter values in order, game-thread stall (1 s at `pingTimeout`
200 ms), an 8 MiB upload in 1 MB frames against `throttle` with `pingTimeout` 200 ms (pong priority),
`maxPayload` with and without `nomaxpayload`, v2 detection, wrong engine path, flood, large attachments, poison
replay.

### 14.4 Reference interop gate (ruling R2) — required for S4 exit and for the release

Differential: the integration scenarios below run against **real Node socket.io**, outside the default suite.

* `Tools/testserver/sio-interop/package.json` pins `"socket.io": "4.8.4"` and npm aliases
  `"socket.io-v45": "npm:socket.io@4.5.4"` (advertises `maxPayload`, no recovery),
  `"socket.io-v44": "npm:socket.io@4.4.1"` (no `maxPayload`) and `"socket.io-v2": "npm:socket.io@2.5.1"`
  (v2 detection); `package-lock.json` is committed. `server.mjs` implements the same namespaces and `__test` ops
  as `sio.py`, selecting the server version with `--server 4.8|4.5|4.4|2`.
* `Tools/run_interop.sh [--server 4.8|4.5|4.4|2|all] [--no-build]`: `npm ci` (socket.io needs Node ≥ 10.2; the dev box has v26.11),
  starts `server.mjs`, then the editor with `-ExecCmds="Automation RunTests Interop.VaRest.SocketIO; Quit"` and
  `-VaRestSioInterop=ws://127.0.0.1:<port>`, and reads results from the run's log (as `Tools/run_tests.sh`
  does). The test prefix `Interop.VaRest.*` is outside the runner's anchored `^VaRest` filter (core §7.3), so the
  default suite never needs Node. Versions (Node, npm, socket.io per alias) go into `docs/passport/`.

Scenarios (each against 4.8.4 unless a version is named):

| # | Scenario | Checks |
| --- | --- | --- |
| I1 | **Recovery** | `connectionStateRecovery` on; N counter events; `__test drop`; the server emits during the outage; reconnect → `bRecovered = true`, missed events once each, in order, after `OnConnect`; a binary event's offset tracked; then `Close()` + connect → `false`; `ResetSession()` → `false`; 4.5.4 → `false` with no error |
| I2 | **Server acks** | server `emitWithAck` plain and with binary args, and `timeout()`; client replies from a typed handler and later from a worker thread through a stored `FVaRestSioAck`; the server's `report` confirms each reply once; a reply after the namespace's DISCONNECT is not sent and the connection survives |
| I3 | **Connect errors** | middleware `next(new Error("unauthorized"))` with `data: {code: 401}` → one refresh → success; refusal after a successful refresh → terminal `Server`; "Invalid namespace" → `Config`; v2.5.1 → the v2 message |
| I4 | **maxPayload** | `maxHttpBufferSize` 1e6: a 900 000-byte frame succeeds, a 1 200 000-byte frame returns `Config` with the connection kept — on 4.8.4 (advertised) and 4.4.1 (not advertised); a 2 MB inbound event is received; with `maxHttpBufferSize` 1e8 the heartbeat-budget warning appears |
| I5 | **Multiple namespaces** | `/`, `/admin` and a never-answering `/slow` concurrently: `/slow` reports `OnConnectError(Timeout)` while `/` stays connected (no restart, connection count 1); `Ns.Disconnect(); Ns.Connect()` back to back keeps the connection (P10 `nextTick`); server DISCONNECT of `/admin` leaves `/` up; no invalid-state close in any server log |
| I6 | **Binary attachments** | client emits with 1, 9 and 10 attachments (header digit boundary) and nested struct fields; BINARY_ACK both ways; server binary events; 11 attachments rejected client-side; Node `Buffer` contents byte-equal |
| I7 | Heartbeat and path | `pingInterval` 300 / `pingTimeout` 200 during a 1 s game-thread stall and during a throttled 8 MiB upload: no disconnect; wrong engine path → `Config` after 3 attempts |

### 14.5 Restore-the-bug targets

Each seen failing with the behaviour broken, then passing restored:

1. Service-thread pong (answer only from the game-thread stage) → stall test: the server drops the link.
2. Inbound hold (dispatch before the ack) → recovery test sees missed events before `OnConnect(bRecovered)`.
3. Namespace gate (emit before the CONNECT ack) → `sio.py` closes the connection (P10).
4. Assembly before typed decode (attach on the game thread) → typed `TArray<uint8>` field arrives empty.
5. `MaxPacketBytes` invariant (allow `MaxPacketBytes` = `MaxPendingInboundBytes`) → large-attachment test stalls until the EIO deadline.
6. Header gap (assume a one-digit count) → the 10-attachment emit corrupts its header; server closes.
7. Offset tracking (never update) → reconnect test reports `bRecovered = false`.
8. `pid` drop on logout (keep it) → the next login resumes the old user's session.
9. One refresh per refusal (no limit) → refusal test counts more than one refresh call.
10. Reserved-name check (skip) → emitting `disconnect` kills the connection instead of `Config`.
11. `maxPayload` check (skip) → server closes 1009 instead of a `Config` error with the link kept.
12. Ack reply pin (reply after the namespace's DISCONNECT) → server closes the connection.
13. No client EIO ping (send `2` on a client timer, as EIO 3 clients do) → server closes ("invalid heartbeat direction").
14. v2 detection (accept a CONNECT ack without `sid`) → test expects `Config`, sees a dead "connected" namespace.
15. DISCONNECT on `Close()` (only close the transport) → `report` shows *transport close*, not *client namespace disconnect*.
16. `pid` drop on our content closes (keep it on 1002, 1007 or 1009) → poison-packet test reconnects until it goes terminal instead of starting fresh.
17. Leave-on-ack (send DISCONNECT while CONNECT is pending) → server closes the whole connection, killing `/`.
18. Message `Kind` (resolve acks from EVENT ids) → a server EVENT with id 0 resolves our pending ack 0.
19. `maxPayload` fallback (16 MiB when absent) → a 2 MB emit to a `nomaxpayload` server closes with 1009.
20. `pid` drop on session replacement (keep it) → `SetBearerToken(userB)` without logout gives `bRecovered = true`.
21. Pong priority (queue the auto-reply behind data) → the throttled-upload test drops with *ping timeout*.
22. Namespace timeout without restart (restart the connection) → the `/never` test's `/` connection count grows.
23. No-response detection (treat it as an unknown connect error) → the wrong-path test loops past 3 attempts.

### 14.6 Builds and performance

Linux editor, monolithic Development and Shipping; packaged Linux run of the Socket.IO chat sample; other
platforms in the release pass (bead VaRest-b4d). `Bench.VaRest.SocketIO.*`: game-thread cost per small event
(target < 5 µs, as WS), `Emit` of a 200-byte struct (≤ 1.2× WS `Emit` of the same payload), a 1 MiB attachment
in and out (1 copy out; 0 copies in for `const TArray<uint8>&` parameters, 1 for struct fields), memory per
idle namespace (< 1 KiB).

---

## 15. Compatibility matrix

| Server | Protocol | Status in 2.0 | How verified |
| --- | --- | --- | --- |
| socket.io 4.6 – 4.8 (Node) | EIO 4 / SIO 5, `maxPayload`, recovery available | **supported, primary target** | `sio.py` (default suite) + interop gate on 4.8.4 |
| socket.io 4.5.x | `maxPayload` advertised, no recovery | supported (recovery inactive) | interop gate on 4.5.4 |
| socket.io 3.0 – 4.4 | no `maxPayload` advertised, 1 MB enforced | supported, 1 000 000 assumed (§2.3) | interop gate on 4.4.1; `nomaxpayload` in `sio.py` |
| socket.io 4.x with `allowEIO3` | server option for old clients | supported (VaRest speaks v4) | n/a |
| multi-node, in-memory adapter | no sticky sessions needed (WebSocket-only) | supported; recovery only when the reconnect reaches the same node | documented |
| Redis (pub/sub) adapter | no recovery ([CSR]) | supported, `bRecovered` always false | documented |
| Redis Streams adapter, MongoDB adapter ≥ 0.3.0 | recovery supported ([CSR]) | supported | documented |
| socket.io 2.x / 1.x, other EIO 3-only servers | EIO 3 / SIO 4 | **unsupported**, detected (§2.7) | `eio3sim` in `sio.py`; interop on 2.5.1 |
| other servers implementing EIO 4 / SIO 5 (e.g. python-socketio 5+, socketioxide) | EIO 4 / SIO 5 | expected to work, not verified | protocol conformance only |
| servers with `transports: ["polling"]` only, or WebTransport only | — | unsupported (§0.3) | upgrade 400 message |
| servers with a custom parser (msgpack) | — | unsupported (§0.5) | protocol error after OPEN |

---

## 16. Milestones

| S | Content | Depends on | Exit |
| --- | --- | --- | --- |
| S0 | `sio.py`, `sio_selftest.py`, `sio_vectors.json`, `run_sio_suite.sh`; `sio-interop/` (`package.json`, lockfile, `server.mjs`) and `Tools/run_interop.sh`; official suite run recorded | W0 (`ws.py`) | selftest green; official suite subset green; `server.mjs` answers the `__test` ops on all four versions (passport) |
| S1 | Module skeleton + Build.cs; codec: EIO framing, SIO encode/decode, per-connection decoder, attachments, placeholders, header gap, limits | S0; W1 (SW-4); W4 (SW-1, SW-2); core M2 (J-1 and the writer entries, §18.2) | `Codec`/`Binary` green; restore-the-bug 4, 5, 6 |
| S2 | Stage: OPEN, heartbeat (SW-5), classification rows incl. no-response and 400 handling, version detection, namespace state machine, closing hook (SW-7), channel lifecycle + inbound hold (SW-8) | S1; W3 | `Heartbeat`/`Namespace`/`Version`/`Close` green; restore-the-bug 1, 3, 13, 14, 15, 17, 19, 21, 22, 23 |
| S3 | Typed variadic handlers, `Once`, `OnAny`/`OnUnhandled`, `Emit`/`EmitArgs`/`Volatile` off-thread (SW-9), backlog, `EmitWithAck` (timeout, error-first, retry), inbound acks, recovery | S2 | `Emit`/`Handlers`/`Ack`/`Recovery` green; restore-the-bug 2, 7, 10, 11, 12, 16, 18 |
| S4 | Auth placements, CONNECT payload, CONNECT_ERROR policy, session change kinds (WS §22 A1); `FVaRestSio` facade + hub; `UVaRestSioEndpoint` (SW-10) + validation; settings; logging/redaction, trace (WS §22 A4), stats, console | S3; W5; core M4 (session kinds), M5 (asset, hub) | `Auth` green; integration suite ws + wss green; **interop gate I1–I7 green on all four server versions**; restore-the-bug 8, 9, 20 |
| S5 | `UVaRestSioClient`, `UVaRestSioComponent` (SW-10 base), library, *Connect Socket.IO* / *Emit With Ack* async actions, K2 nodes + spawner (SW-11) | S4; W5 | BP chat sample (component + typed nodes) end to end against `sio.py`; two PIE clients isolated |
| S6 | Docs (README section, "coming from SocketIOClient-Unreal / socket.io-client" notes incl. D20/D21), sample, monolithic/Shipping/packaged runs, benchmarks; interop gate re-run for the release (VaRest-8e5) | S5; W6 | builds green; interop and benchmark numbers in the passport |

Each S gets its own plan in `docs/superpowers/plans/`, written when the previous one lands. S0 can run right
after W0. The SW items are written into WS revision 2 (WS §22) with their W milestone, so the W plans take them
from the WS spec itself.

---

## 17. Risks

* **Nine WS hooks and four VaRestJson additions must land on time.** They are now in WS §22 with their
  milestones and in `.superpowers/sdd/sio-m2-gap.md` for M2 Task 7; if M2 executes without the gap folded in,
  S1 waits on an M2 follow-up (small, but on the critical path).
* **The interop gate depends on Node and the npm registry.** A required gate can block a release for tooling
  reasons. Mitigation: the lockfile pins every package; the default suite never needs Node; the gate runs at S4
  and before release, not on every change.
* **Pong delay on slow uplinks with large frames** (§2.4): below ~0.5 Mbit/s with default servers, or ~2 Mbit/s
  with a 5 s `pingTimeout`, a full 1 MB frame in flight can cost the connection. Mitigation: priority at
  message boundaries, `SO_SNDBUF` cap, a warning when a server's settings make it likely, recovery.
* **Recovery depends on server configuration and adapter** (off by default on the server, unavailable with the
  Redis pub/sub adapter, node-local with the in-memory adapter, one ambiguity with volatile events). Mitigation:
  client side is on by default and harmless; `bRecovered` is exposed everywhere; docs tell apps to resync on
  `false`.
* **EIO 3-only servers in the wild** are blocked, with a precise message. A v2 codec is possible in 2.x behind
  the same seam if demand appears.
* **Long game-thread stalls** still pause handlers and, with heavy traffic, reading (flow control). The pong goes
  out from the service thread, client deadlines are suspended while reading is paused, and recovery restores
  the session if the server gives up.
* **WebSocket-hostile networks** have no polling fallback. Mitigation: `wss` on 443 passes most proxies; a
  polling transport is possible later behind `IVaRestSocketTransport`.
* **K2 and component complexity** (arg pins, ack pins, namespace level in the spawner, entry dropdown).
  Mitigation: built on the WS declaration nodes and component base (SW-10, SW-11) and the M2 *Make Json* pin
  mechanism; reconstruction test as core §9.

---

## 18. Required changes

### 18.1 WebSocket spec (written into WS revision 2, WS §22)

| # | Change (as adopted in WS §22) | Why (section) | W |
| --- | --- | --- | --- |
| SW-1 | Message `Kind` (`Message`, `Request`, `Reply`, `ErrorReply`, `Control`) set by the codec; pending requests resolve only from replies; a socket-wide `int64` id counter on the game thread for numeric-id codecs; pending table keyed by id with a channel check | a server EVENT id must not resolve our ack (§4.5) | W4 |
| SW-2 | Codec contract v2 (was SW-2 + SW-3): a stateful decoder created per connection on its Open event, used only on the pipe, `Decode` → `Message` / `NeedMore` / `ProtocolError` (closed 1002, decoder poisoned); type-erased typed decode plans that may decode several positions and bind attachments | attachments before typed decode; variadic handlers (§5.2, §4.4) | W4 |
| SW-4 | Frame payload offset ≥ `LWS_PRE` | one-pass encode with a variable-length header (§5.1) | W1 |
| SW-5 | Service-thread auto-reply rules (text frames only) in the connect command, written at the next message boundary ahead of all queued data, outside buffered amount and backpressure, never dropped, frames flagged `bAutoReplied`; `SO_SNDBUF` 256 KiB on connections with rules; `Ctx.SetIdleDeadline` on data frames only; client deadlines (idle and WS protocol ping) suspended while reading is paused, `FVaRestSocketClose::bWhileReadPaused` | pong during stalls and behind large sends (§2.4) | W1 |
| SW-7 | Closing hook `OnClosing(Ctx, Request)` for app-initiated closes and session-replacement restarts; frames sent there precede the close frame | DISCONNECT packets (§3.5) | W3 |
| SW-8 | Stage-driven channel lifecycle (`ChannelUp` / `ChannelDown` with `WillRetry` / `Ended`) and per-channel inbound hold | namespaces end without closing the socket; recovered events precede the ack (§3.2, §3.6) | W4 |
| SW-9 | `SendBatch` callable from any thread, one unit and contiguous on the wire (except control frames and auto-replies), optional pin (connection generation, channel epoch) dropping the batch at flush if it changed; the send limit is enforced at flush for every queued frame; `EncodeReply` returns a batch through the same path | `Emit` and `Ack.Send` from any thread (§4.2, §4.5) | W3 |
| SW-10 | `UVaRestSocketEndpointBase` (connection fields) ← `UVaRestSocketEndpoint` (routing, messages, WS placement, app heartbeat) and ← `UVaRestSioEndpoint`; `UVaRestSocketComponent` base (asset/URL, auto-connect, scope, assignable lifecycle events) | Socket.IO entry and component (§8.4, §9) | W5 |
| SW-11 | Declarations with a channel; spawner path *Asset ▸ Entry ▸ [Channel ▸] Route*; protocol-supplied extra pins | typed Socket.IO nodes (§8.3) | W5 |

Dropped from revision 0 of this spec: **SW-3** (merged into SW-2), **SW-6** (replaced by the configuration
invariant of §2.5), **SW-12** (folded into the general W5 trace work, WS §22 A4).

Other WS revision-2 amendments this spec relies on (WS §22): **A1** session change kinds `Refreshed` /
`Replaced` / `Cleared` (core §4.2 via WS §19 item 2); **A2** transport failure reason `NoUpgradeResponse`; **A3**
WS lookups refuse Socket.IO sockets and entries; **A4** generic channel trace events and a protocol tag.

### 18.2 VaRestJson (core M2)

Checked against the M2 plan (Task 7 encode, Task 8 decode); details and exact signatures in
`.superpowers/sdd/sio-m2-gap.md`, to be folded into M2 Task 7 before it executes.

| # | Need | M2 as planned | Missing |
| --- | --- | --- | --- |
| J-1 | `TArray<uint8>` values inside structs/containers as attachments | no encode-side hook (`bBytesAsBase64` only) | `FVaRestJsonConvertOptions::BytesWriter` (`TFunction<bool(TConstArrayView<uint8>, FVaRestJson& OutReplacement)>`), the encode counterpart of Task 8's `BytesResolver` |
| J-2 decode | positional args (field *i* ← `arg i`) | **covered**: Task 8 `JsonToProperty` over `TFieldIterator<FProperty>` | none |
| J-2 encode | positional args, `FVaRestJson` args and value-struct args written one pass into the packet's writer | `PropertyToJson` builds a DOM, but no public way to write a DOM, a value struct or a property into an open writer (`EmitNode` is private; `WriteStructMembersInternal` refuses value structs) | `WriteJsonInternal`, `WriteStructInternal`, `WritePropertyInternal` (`VaRestJsonWriterInternal.h`, same contract as `WriteStructMembersInternal`) |

No other core changes: redaction additions use the core's per-client list (core §4.5).

---

## 19. Decisions made without the owner

`R` = controller ruling (binding); others are this spec's. Revision 1 marks amended and reversed decisions.

| # | Decision | Why | Cost if wrong |
| --- | --- | --- | --- |
| R1 | Apply the review's Critical and nine Important fixes (optional `maxPayload` with a 1 000 000 fallback; D10 reversed; `pid` dropped on any session/user change and on 1002/1007; wrong engine path terminal and precise; int64 ack ids; actor component); the pong is a data frame that jumps the queue at the next message boundary, with the worst case stated against `pingTimeout` (§2.4) | the review showed a broken server range, a reconnect loop, a cross-user resume and a silent loop | more client logic (timeouts, session kinds, a failure reason), none of it changes the wire |
| R2 | **Interop against real Node socket.io is a required release gate**: `Tools/testserver/sio-interop/` pins socket.io 4.8.x (4.8.4) plus 4.5/4.4/2.x aliases, run by `Tools/run_interop.sh` outside the default suite; scenarios I1–I7 (§14.4) | the official suite misses exactly the risky areas; `sio.py` alone could mirror our misreading | Node/npm tooling on the dev box; a release can be blocked by tooling; S4 exit takes longer |
| R3 | Adopt the reduced nine-item SW set (SW-6 → invariant, SW-1/SW-9 shrunk, SW-12 folded) and amend the WS spec itself (WS §22) | implementers plan from the specs; fewer hooks, same capability | WS rev 2 must stay in sync with this spec; a dropped hook needed later is added in a W plan |
| R4 | J-1/J-2 checked against M2 Tasks 7–8; the gap (J-1 + three writer entries) is written to `.superpowers/sdd/sio-m2-gap.md` for the M2 plan | M2 executes before S1; fixing it later costs a follow-up | four small additions to M2 Task 7 before it runs |
| D1 | `VaRestSocketIO` module on `VaRestWebSocket`, no third-party code | the protocols are small; one transport to maintain | none significant |
| D2 | **WebSocket-only Engine.IO** (no polling, upgrade, WebTransport) | default servers accept it; no sticky sessions; no long-poll HTTP machinery (§0.3) | players behind WebSocket-blocking proxies cannot connect |
| D3 | **Socket.IO v5 / EIO 4 only; v2 detected** (sid-less CONNECT ack, string CONNECT_ERROR) **and refused**, no compat option in 2.0 | v2 is a second protocol, superseded in 2020; version confusion is the #1 complaint about existing plugins | v2-only backends cannot use VaRest Socket.IO until a 2.x codec |
| D4 | Distinct `FVaRestSioSocket`/`FVaRestSioNamespace` handles without raw send; WS lookups refuse Socket.IO sockets | Socket.IO vocabulary; protocol-breaking sends do not compile or resolve | two handle types to learn |
| D5 | Reuse `UVaRestSocketHub` ownership; static facade `FVaRestSio` taking a world context or hub | WS §1.4 rules hold; no second subsystem | none |
| D6 | Namespaces = WS channels; default namespace from the URL path in `Make(Url)` (socket.io-client idiom) with URL hygiene; explicit fields in assets and an explicit Namespace pin in BP | JS examples work as written; pasted dev-tools URLs are repaired | sub-path deployments must set `WithPath` (detected and hinted) |
| D7 | A namespace is wanted after its `Connect()`; the last wanted namespace ending closes the socket without reconnect | socket.io-client parity; no idle connections | an app wanting an idle connection must keep a namespace |
| D8 *(amended)* | Never send EIO pings; pong as a text-only auto-reply on the service thread, ahead of queued data at the next message boundary, outside backpressure, `SO_SNDBUF` 256 KiB; dead-link deadline on data frames only; client deadlines suspended while reading is paused; WS protocol ping stays (R3) | v4 servers close on client pings; stalls and large sends must not cost the session; RTT needs the WS ping | the send-buffer cap limits one connection to ≈ 20 Mbit/s at 100 ms RTT; a server that kills on RFC 6455 PINGs needs `WithProtocolPing(0, 0)` |
| D9 | Reconnect defaults = socket.io-client (1 s × 2 → 5 s, ±50 %, unlimited) + `StableAfter` | servers are sized for it; parity with web clients | more reconnect load during long outages |
| D10 *(reversed)* | `ConnectTimeout`/`ReadyTimeout` 20 s; a later namespace's CONNECT timeout raises `OnConnectError(Timeout, will retry)` and waits, **never restarting a connection that has a connected namespace**; restart only when none is connected | a restart cannot help (the namespace re-asks on the next connection anyway) and would take healthy namespaces down every 21 s | a namespace whose server never answers stays pending until the next reconnect |
| D11 | Positional args; extras ignored, missing defaulted (+warning); `First` default for asset/BP payloads | JavaScript semantics; most events carry one object | strict users must enable strict decode |
| D12 | `TArray<uint8>` means attachment in Socket.IO, nested values included (J-1) | what JS `Buffer` does; zero base64 | servers expecting number arrays set the option |
| D13 | Attachment assembly in a per-connection decoder on the pipe (SW-2) | typed decode sees bytes; nothing parsed on the game thread | WS codec contract grows |
| D14 *(amended)* | Outbound frames checked against `maxPayload` → `Config`, connection kept; the field is optional, **1 000 000 assumed when absent**, raisable by `WithLimits` | the server closes the whole connection over its limit (P4), advertised or not (P2) | a server with a larger unadvertised limit needs one setting |
| D15 | `MaxAttachments` 10 both ways | the server decoder's default (P9) | servers raising it need a setting change |
| D16 | Reserved event names rejected at `Emit`; protocol error inbound | a sent one kills the connection (P8) | none |
| D17 | Ack timeout 10 s by default (socket.io-client: none); `Timeout(0)` = forever | a game should not wait forever silently | slow acks need an explicit timeout |
| D18 | Every sent ack ends (success, error, timeout); sent acks fail on disconnect, queued ones wait; `Retry(N)` unordered; `ErrorFirst()` opt-in | better than the reference client's silent drops (P12) | ordered retries need app logic |
| D19 | Inbound ack at most once (shared state across copies), any thread, pinned to its connection and namespace | late replies on a new connection are invalid (P10) | late replies are lost |
| D20 | Events before the CONNECT ack held and dispatched after `OnConnect` | handlers know `bRecovered` first; differs from socket.io-client, documented | apps ported from JS see `connect` before missed events |
| D21 | `OnConnect` sends before the backlog (WS order) | joins first (WS D11); differs from socket.io-client, documented | apps relying on JS's order must adapt |
| D22 *(amended)* | Recovery on by default; socket.io-client offset rule on EVENT and BINARY_EVENT; args not stripped; `pid`s dropped on close, logout, **session replacement**, env change, give-up, **every content close (1002/1007/1009)** and hold overflow | free when the server lacks it; never a cross-user resume; no poison replay | the volatile ambiguity costs a failed recovery; content errors cost the session state |
| D23 | Default auth placement = CONNECT payload `{"token": …}` when a session is attached | the documented Socket.IO way (`handshake.auth`) | header-reading servers need one setting |
| D24 *(amended)* | CONNECT_ERROR: one single-flight refresh + retry when session credentials were sent — any refusal with the payload placement, only auth-looking refusals with header/query (which restart the connection); never "Invalid namespace"; session expiry only if the refresh failed | no standard auth code; a non-auth refusal must not bounce every namespace | an auth refusal with an unusual message on a header placement is not retried automatically (`ConnectErrorIf` can) |
| D25 | `Close()`, logout and session replacement send DISCONNECT per namespace (SW-7); drops, background and restarts do not | correct server-side reason and recovery semantics (P11) | none |
| D26 *(amended)* | Engine path `/socket.io/`, trailing slash always (no option); never send `sid` | matches servers with and without `addTrailingSlash` (P5) | a server needing a slash-less path cannot be reached (none known) |
| D27 | Inbound limits 8 MiB per frame, 16 MiB per packet; hold 4096 / 16 MiB | attachments are common; memory bounded | big-payload games raise them |
| D28 | CONNECT payload values, `pid`, EIO `sid` always redacted | they are credentials or session keys | debugging auth needs the trace with care |
| D29 | BP namespaces as a string parameter; `Ack` as a pin; arg pins on Emit; `bOnce` on the listen node | fewer objects; acks wired visually | multi-namespace graphs repeat the string (asset nodes hide it) |
| D30 *(amended by R2)* | Default suite on Python stdlib `sio.py` (validated by the official suite); **required** Node interop gate (§14.4) | fast hermetic tests plus an external oracle where the suite is blind | Node needed at S4 and release |
| D31 *(amended)* | Protocol errors: decoder poisoned, close 1002, `pid`s dropped, reconnect, terminal after 3 in a row | transient garbage recovers; poison is not replayed; wrong endpoints stop | a flaky proxy hitting 3 in a row stops early |
| D32 | First frame not OPEN → terminal `Config` | the endpoint is not Engine.IO; retrying hides the cause | a briefly misrouted deploy needs a manual reconnect |
| D33 | No implicit connection sharing between `Make` calls | explicit ownership; named sockets share | two `Make`s to one server open two connections |
| D34 | `UVaRestSioComponent` on a WS component base: entry mode uses the entry's hub-named socket (shared with the typed nodes, survives level travel); URL mode with `Actor` (default) or `GameInstance` scope; auto-connect, assignable events | the entry point getnamo users expect; one socket per entry so component and typed nodes never diverge | an actor-scoped connection to an asset entry needs `bCloseOnEndPlay` |
| D35 | Session change kinds; `Replaced` → DISCONNECT all, drop `pid`s, restart; code-first `ResetSession()` | a recovered session skips auth (P11); an opaque bearer lambda cannot signal a switch | apps with `WithBearer` must call `ResetSession()` on user switch |
| D36 | "Upgrade sent, no HTTP response" is its own reason; 3 in a row → terminal `Config` naming the engine path | engine.io's answer to a wrong path is silence (P5); the common mistake must not loop | a server that is briefly unreachable this way 3 times stops; `ReconnectNow` resumes |
| D37 | Upgrade 400 with a header/query session placement: one refresh, then terminal | engine.io answers `allowRequest`/middleware auth refusals with 400, not 401 (P5) | one wasted refresh for a non-auth 400 |
| D38 | `MaxPacketBytes ≤ MaxPendingInboundBytes / 2` invariant, Socket.IO `MaxPendingInboundBytes` 32 MiB (replaces SW-6) | same memory bound, no decoder API | 16 MiB more worst-case buffering per Socket.IO socket |
| D39 | Ack ids `int64` ≤ 2^53 − 1; one socket-wide outbound counter on the game thread | server counters outgrow int32 (P13); the protocol does not need per-namespace numbering | none |
| D40 | Re-CONNECT after a DISCONNECT of the same namespace on one connection waits max(RTT, 100 ms) | the server applies DISCONNECT on `nextTick` (P10) | a quick leave/rejoin takes one RTT longer |
| D41 | Socket.IO backlog `MaxQueuedBytes` 4 MiB per namespace; an entry over it is rejected `Offline` | packets carry attachments | offline emits over 4 MiB fail at once |

---

## 20. Provenance

**Owner code.** Neither `dawg-code` nor `DwtWebSoN` (session scratchpad `…/scratchpad/ref/`) contains a
Socket.IO or Engine.IO client: a search for `socket.io`, `engine.io`, `EIO=`, `_placeholder` outside
`ThirdParty` finds nothing. Their WebSocket patterns are already absorbed by WS §21. Taken here:

| Taken | From | Changed |
| --- | --- | --- |
| One token applied to HTTP and the realtime socket together | `dawg-code/Dawg/Private/Submodules/Game/DwClientGameSubmodule.cpp:274-282` (`ApplyAuthToken`: set the bearer, then disconnect + reconnect the socket) | the socket reads the shared session per CONNECT (§6); a refresh does not reconnect, a replacement does |
| Event payloads are not always objects | `dawg-code/Dawg/Private/Submodules/Game/DwBaseGameSubmodule.cpp:100-109` (scalar payloads wrapped as `{Data: …}`) | positional typed args bind scalars directly (§4.1) |

Not taken: the condition at `DwBaseGameSubmodule.cpp:103` (`Type != Object || Type != Null`, always true, so
object payloads are duplicated under `Data`); logging the connect URL with the token in its query
(`dawg-code/Dawg/Private/Network/DwSocketClient.cpp:109`, `DwtWebSoN/Source/DwtWebSoN/Private/DwtWebSoNSubsystem.cpp:93`)
— VaRest logs redacted URLs only (core §4.5).

**Reference implementation.** Protocol behaviour was read from the socket.io monorepo at tag `socket.io@4.8.4`
(§0.2) to pin facts the protocol documents leave implicit; no code is copied (VaRest is C++, the reference is
TypeScript). `getnamo/SocketIOClient-Unreal` informed §0.4 and §8.4 only (its README's component workflow).

---

## Appendix A. Review disposition

Review: `.superpowers/sdd/sio-spec-review.md`. "Applied" items are in the sections named.

| # | Sev | Disposition |
| --- | --- | --- |
| 1 | Critical | **Applied (R1)**: `maxPayload` optional, 1 000 000 fallback at Verbose, `WithLimits` override, out of the v2 heuristics; P2 corrected; §15 row 3.0–4.4; `nomaxpayload` knob, `Version` test, interop on 4.4.1, restore-the-bug 19 (§0.2, §2.3, §2.5, §2.7, §14, §15, D14). |
| 2 | Important | **Applied (R1), D10 reversed**: per-namespace timeout → `OnConnectError(Timeout, will retry)`, no re-send, no DISCONNECT, CONNECT on the next connection; restart only with no namespace connected; `Namespace` test and interop I5; restore-the-bug 22 (§2.6, §3.2). |
| 3 | Important | **Applied (R1)**: session change kinds (WS §22 A1); `Replaced` drops every `pid` and restarts with DISCONNECTs; `ResetSession()` for opaque lambdas; restore-the-bug 20 (§3.2, §3.5, §4.6, §6, D35). |
| 4 | Important | **Applied (R1)**: `NoUpgradeResponse` reason (WS §22 A2, W0 pins the LWS string); hint in `OnRetryScheduled`; terminal `Config` after 3; integration test and interop I7; restore-the-bug 23 (§2.6, D36). |
| 5 | Important | **Applied with the controller's correction (R1)**: the pong is a text data frame, so it cannot enter another data message's fragments (control frames may, RFC 6455 §5.4); priority = next message boundary ahead of all queued data, outside backpressure, never dropped, text-only rules, `SO_SNDBUF` cap; worst case D = (R + K) × 8 / U tabulated against `pingTimeout`; throttled-upload test; restore-the-bug 21 (§2.4, SW-5, D8). |
| 6 | Important | **Applied (R1)**: `pid`s dropped on every content close (1002, 1007, 1009); restore-the-bug 16 generalised (§2.3, §2.6, §4.6, D22, D31). |
| 7 | Important | **Applied (R1)**: `int64` ids ≤ 2^53 − 1, longer runs are protocol errors, golden vector `2/admin,9007199254740991["x"]` + ACK (§4.5, §14.1, P13, D39). |
| 8 | Important | **Applied (R2)**: required interop gate with exact scenarios I1–I7 on 4.8.4/4.5.4/4.4.1/2.5.1; official-suite coverage stated honestly (§14.2, §14.4, D30). |
| 9 | Important | **Applied (R3, R4)**: WS revision 2 written (WS §22, contradictions at WS §6.1, §8, §11 and others fixed in place); M2 gap written to `.superpowers/sdd/sio-m2-gap.md` (§18). |
| 10 | Important | **Applied (R1)**: `UVaRestSioComponent` on WS `UVaRestSocketComponent` (SW-10), actor/game-instance scope, assignable events (§8.4, D34). |
| 11 | Minor | **Applied**: a string CONNECT_ERROR goes to v2 detection (§2.7, §3.4). |
| 12 | Minor | **Applied**: 400 message lists the real causes; first 400 with header/query placement refreshes once; `sio.py` emulates `abortUpgrade` and the post-101 EIO 3 abort (§2.6, §14.2, D37). |
| 13 | Minor | **Applied**: `bAddTrailingSlash` removed; slash always appended (§2.2, §9, D26). |
| 14 | Minor | **Applied (R3)**: SW-6 replaced by the `MaxPacketBytes ≤ MaxPendingInboundBytes / 2` invariant, 32 MiB default (§2.5, D38). |
| 15 | Minor | **Applied (R3)**: SW-9 is any-thread `SendBatch` + pin + flush-time send limit; no `OnFlush` (§18.1). |
| 16 | Minor | **Applied (R3)**: SW-1 is `Kind` + one socket-wide int64 counter on the game thread + channel check; atomic counters removed (§4.5, §13). |
| 17 | Minor | **Applied (R4)**: J-2 decode reuses `JsonToProperty`; encode needs the writer entries in the gap file (§18.2). |
| 18 | Minor | **Applied**: client deadlines suspended while reading is paused, `bWhileReadPaused`, EIO deadline on data frames only (§2.4, SW-5). |
| 19 | Minor | **Applied**: (a) reconnecting-socket cell, (b) transport loss during leave-on-ack, (c) unwanted/unknown namespaces dropped, (d) poisoned decoder, (e) default namespace always connects, (f) refresh sub-state and timer start (§3.1, §3.2, §3.3, §5.2, §9). |
| 20 | Minor | **Applied**: header/query placements refresh only on auth-looking refusals (§3.4, D24). |
| 21 | Minor | **Applied**: `AsWeak()` on both handles, weak types, pitfall documented (§1.2, §1.3, §7). |
| 22 | Minor | **Applied**: `Once`, `OffAny`, `OffAll(nullptr)`, `bOnce` on the listen node (§4.4, §8.3). |
| 23 | Minor | **Applied**: re-CONNECT delay max(RTT, 100 ms) unconditionally (cheap), pinned by interop I5 and a `sio.py` same-receive rule (§3.1, §14, D40). |
| 24 | Minor | **Applied**: offsets on EVENT and BINARY_EVENT; visibility in untyped views documented (§4.1, §4.6, P11). |
| 25 | Minor | **Applied**: P12 corrected; listed as an advantage in §0.4 and §4.5 (D18). |
| 26 | Minor | **Applied**: permalinks at tag `socket.io@4.8.4` (`c239eb5d…`), files compared with `main`, lines opened; reserved names attributed to the parser (§0.2). |
| 27 | Minor | **Applied**: `/socket.io` path as engine path, `EIO`/`transport`/`sid` stripped, BP Namespace pin, `?` refused (§2.2, §8.2). |
| 28 | Minor | **Applied**: `WithPing` → `WithProtocolPing` (§2.4, §7). |
| 29 | Minor | **Applied**: copy counts per binding kind (§5.1, §5.2, §14.6). |
| 30 | Minor | **Applied (documented)**: an entry over `MaxQueuedBytes` is rejected `Offline`; Socket.IO default raised to 4 MiB rather than changing the WS backlog rule (§4.3, D41). |
| 31 | Minor | **Applied**: WS hitch rule on `ReadyTimeout` and namespace timeouts (§2.6). |
| 32 | Minor | **Applied**: WS lookups refuse Socket.IO sockets and entries (WS §22 A3; §1.2, D4). |
| 33 | Minor | **Applied**: estimate 1000–1200 lines; knobs `nomaxpayload`, `norecovery`, `poison`, `throttle`, `abortUpgrade` fidelity, same-receive DISCONNECT/CONNECT (§14.2). |
| 34 | Minor | **Applied**: rows for multi-node in-memory adapter and Redis Streams/MongoDB (§15, §4.6). |
| 35 | Minor | **Applied**: (a)–(f) are restore-the-bug 18, 19, 20, 16, 21, 22 (§14.5). |
| 36 | Minor | **Applied**: shared thread-safe ack state, test through two BP copies (§4.5). |
