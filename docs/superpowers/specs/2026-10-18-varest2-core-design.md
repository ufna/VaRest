# VaRest 2 — Core (JSON + HTTP) design

Status: approved for planning, 2026-10-18. Sub-project 1 of 4 (see §0.3).
Target: Unreal Engine 5.8, branch `v2` off `develop`.

---

## 0. Context

### 0.1 Intent

VaRest 1.x was the de-facto REST/JSON plugin for UE4/UE5 (1.1k stars, 331 forks) and was archived
in 2024. VaRest 2 brings it back as **the** web hub for Unreal: if your game talks to the web, it uses
VaRest. Free, MIT, Fab + GitHub.

What success looks like:

* A designer configures a backend **without C++**: an API asset, a node per endpoint, typed
  results, sane errors.
* A C++ programmer writes one readable statement per request — no "setup snot".
* Retries, timeouts, auth refresh, caching, offline queueing, and log redaction are **on the
  shelf**, not homework.
* JSON is measurably faster and lighter than `FJsonSerializer`/`FJsonObject` (gate in §7.3).
* Existing VaRest 1.x projects open and keep working; a migration guide shows the way out.

### 0.2 Decisions already made (owner, 2026-10-18)

| Topic | Decision |
| --- | --- |
| v1 compatibility | **Option C**: v2 is a new clean API; v1 classes stay as deprecated shims on the new core; removed in v3; migration guide ships with 2.0. |
| Scope assumptions | MIT; free; Win64/Mac/Linux/Android/iOS; UE 5.8 only; VaCuus-style dev setup; WebRTC = data channels only. |
| 2.0 extras | Network Inspector, OpenAPI/Swagger import, HTTP cache, offline queue — all in 2.0. Cache + offline queue are core pipeline stages (this spec); Inspector and OpenAPI import get their own specs. |
| Configuration | **Asset + code**: an API asset is the main path; the same client can be built from C++ or BP without an asset. |
| JSON core | **Approach 1**: fast core on the engine's RapidJSON (`Runtime/Json/Public/JsonUtils/RapidJsonUtils.h`, ThirdParty/RapidJSON 1.1.0), behind a benchmark gate (§7.3). |

### 0.3 The four sub-projects

1. **Core: JSON + HTTP** — this document.
2. **WebSocket** — reconnect/backoff, app-level heartbeat (UE 5.8 `IWebSocket` has no per-socket
   ping: only the global `[WebSockets.LibWebSockets] PingPongInterval` for LWS,
   `LwsWebSocketsManager.cpp:192-194`, nothing for WinHttp), message routing, BP events.
3. **Socket.IO** — native Engine.IO v4 / Socket.IO v5 over (2).
4. **WebRTC data channels** — last; possibly 2.x.

Network Inspector and OpenAPI import are separate specs that build on the extension points defined
here (§4.8 trace tap, §5.1 API asset).

### 0.4 Non-goals for the core

* Cookie jar. Apple's `NSURLSession` uses the shared system cookie storage on its own, so a VaRest jar would
  behave differently per platform. Sessions are token- and header-based (§4.2).
* Persisting tokens to disk. `ExportSession`/`ImportSession` let the game do it (§4.2).
* HTTP/3, server push, a custom transport. UE's `FHttpModule` is the transport.
* Latent BP nodes for v2 (async action nodes replace them; v1 latent `ApplyURL` stays in legacy).

---

## 1. Architecture and modules

```
VaRestJson          Runtime       FVaRestJson, arena DOM on RapidJSON, struct <-> JSON, BP JSON library
   ^
VaRestHttp          Runtime       FVaRestClient, request pipeline + stages, UVaRestApi asset,
   ^                              UVaRestHub, UVaRestApiClient, async BP actions, settings
VaRest              Runtime       v1 API, unchanged paths, reimplemented as deprecated shims (removed in v3)
VaRestEditor        UncookedOnly  K2 nodes: v1 Make/Break Json (unchanged) + v2 Make Json, wildcard
                                  Get/Set, Call Endpoint, Send Request
VaRestEditorTools   Editor        asset definition (category, colour, icon), validation, menus
--- later specs ---
VaRestInspector · VaRestOpenApi · VaRestWebSocket · VaRestSocketIO · VaRestWebRtc
```

**Why v1 keeps the `VaRest` module name.** v1 C++ users depend on module `VaRest` and include
`VaRestJsonObject.h` etc.; v1 BP graphs reference `/Script/VaRest.*` and `/Script/VaRestEditor.*`.
Leaving v1 exactly where it is means **zero CoreRedirects for v1 types**, no C++ include breakage,
and no fragile delegate-signature redirects. The new core gets new module names, so nothing collides.
(This revises the in-chat sketch of section 1, which moved v1 to `VaRestLegacy` with redirects. That
would have broken v1 C++ code: a `VaRest` → `VaRestLegacy` dependency would be circular.)

**Why JSON is its own module.** WebSocket and Socket.IO need JSON but not HTTP; projects that only
want JSON for saves or configs link only `VaRestJson`.

**Object model.**

* `FVaRestJson` — 24-byte handle to a node in a shared, arena-allocated document (§2).
* `FVaRestClient` — cheap C++ handle (`TSharedRef` to the client state): base URL, policies, stage
  chain, session (§3).
* `UVaRestApi` — data asset: environments, auth, policies, envelope, endpoints (§5.1).
* `UVaRestApiClient` — BP-facing `UObject` around one `FVaRestClient`, with assignable events (§5.3).
* `UVaRestHub` — owns `UVaRestApiClient`s, connectivity state and offline queues.
  **GameInstance-scoped**, so two PIE clients never share a token; an engine-scoped fallback
  instance serves editor utilities and other contexts with no game instance (§5.2).

**Plugin descriptor.** `"VersionName": "2.0.0"`, `"EngineVersion": "5.8.0"`, `Category "Network"`,
`PlatformAllowList` Win64/Mac/Linux/Android/IOS on runtime modules. All three runtime modules load
`PreDefault` (v1 did, other modules may parse JSON early, and `VaRest` depends on both new modules, so
they must not start later than it does).

**Threading rule (applies everywhere).** *Control plane on the game thread, data plane off it.*
Stage decisions, callbacks, and BP events run on the game thread. Parsing, decoding, cache disk IO,
and offline-queue persistence run on `UE::Tasks` background tasks. Transport completes on the
HTTP thread (`EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread`); the completion only moves
bytes and dispatches work. A request may be **sent** from any thread; it is marshalled to the game
thread before entering the pipeline.

---

## 2. JSON layer (`VaRestJson`)

### 2.1 Storage

RapidJSON is used for its **reader (SAX) and writer** — SIMD whitespace skipping, fast number
parsing, Grisu shortest round-trip doubles. Include it through the engine's
`RapidJsonIncludes.h` so the engine's macro configuration (`RAPIDJSON_ASSERT` → `check`,
`RAPIDJSON_SSE42`/`RAPIDJSON_NEON`) is identical. Use `kParseIterativeFlag` so hostile input
cannot overflow the stack.

The DOM is **ours**, not `rapidjson::GenericDocument`. Reason: RapidJSON stores object members
and array elements in contiguous, reallocating arrays. A handle to a child would dangle the moment
its parent grows. Our DOM gives handle stability by construction:

* `FVaRestJsonDocument` owns a chunked bump arena (first chunk sized from the input length when
  parsing; growth chunks up to 64 KiB). Nodes never move once allocated.
* A node is `{type tag, payload}`: bool; number as `int64` / `uint64` / `double` (integral literals
  that fit keep integer precision — 64-bit IDs survive); string as UTF-8 bytes + length in the arena;
  array as an arena vector of `Node*`; object as an arena vector of `{key UTF-8, Node*}` in
  **insertion order**.
* Growing a member/element vector allocates a new vector in the arena (geometric growth), and the
  child nodes stay where they are. Removed nodes stay in the arena until the document dies.
  `Clone()` rebuilds into a fresh document when that matters (an in-place compaction would move
  nodes and invalidate other handles, so there is none).
* **Parsing is in situ on a buffer the document owns**, so parsed strings and keys point into
  that buffer with **zero per-string copies** (`kParseInsituFlag` over an owned, NUL-terminated
  UTF-8 buffer). `ParseOwned(TArray<uint8>&&)` steals the HTTP response body — zero-copy when the
  array has one byte of slack for the terminator, which the HTTP layer reserves; the view overloads
  copy the input once. Strings created by mutation live in the arena. (Not for SIMD:
  `kParseValidateEncodingFlag`, which untrusted input needs, disables RapidJSON's SIMD string
  scanner, and a SIMD whitespace skipper also exists for bounded `MemoryStream`s — reader.h:514,
  :1006, verified in Task 6's review.) A raw NUL inside the input is an error, not an early end.
* Objects with more than 8 members get a hash index (open addressing, in the arena) built **at parse
  time or on mutation**, never on read, so concurrent readers never write.
* Duplicate keys in input are preserved; lookup returns the **last** occurrence (`JSON.parse`
  semantics).
* NaN/Inf cannot be represented in JSON: serialised as `null` with a warning.
* Limits (configurable per parse): max depth 512, max input size unlimited by default.

`FVaRestJson` = `TSharedPtr<FVaRestJsonDocument, ESPMode::ThreadSafe>` + `Node*`. An invalid handle
has type `None`.

### 2.2 Semantics

* **Reference semantics**, like v1 and Epic's `FJsonObjectWrapper`: copying a handle aliases the
  same node; `Clone()` deep-copies into a new document. Documented prominently because it differs
  from ordinary BP structs.
* Assigning a value from another document deep-copies it into the target document (a node has one
  parent). An optimisation for adopting a whole sole-owned temporary document by splicing arena
  chunks is allowed but not required.
* **Thread contract:** any number of threads may read a document no one mutates; mutation needs
  exclusive access; handles are safe to copy across threads; a parsed document is handed off between
  threads by ownership transfer (that is how HTTP responses reach the game thread).

### 2.3 C++ API (shape, not final signatures)

```cpp
TValueOrError<FVaRestJson, FVaRestJsonError> R = FVaRestJson::Parse(Utf8Bytes); // also FUtf8StringView, FStringView
FVaRestJson Body = FVaRestJson::Object()
    .Set(TEXT("name"), TEXT("Neo"))
    .Set(TEXT("level"), 42)
    .Set(TEXT("tags"), FVaRestJson::Array({ TEXT("a"), TEXT("b") }));

int32 Level = Json[TEXT("level")].AsInt(/*Default*/ 0);      // operator[] on missing key -> None handle
TOptional<FProfile> P = Json[TEXT("profile")].TryGet<FProfile>();
FVaRestJson Name = Json.Path(TEXT("user.items[3].name"));    // also JSON Pointer: "/user/items/3/name"
for (const auto& [Key, Value] : Json.Members()) { ... }
for (const FVaRestJson& Item : Json.Elements()) { ... }
FString Text = Json.ToString(EVaRestJsonFormat::Pretty);    // 2-space indent; also ToUtf8(TArray<uint8>&)
```

* `FVaRestJsonError`: message, byte offset, line, column, JSON path (for decode errors).
* `TryGet<T>` / `AsX(Default)` support: `bool`, all integer types, `float`, `double`, `FString`,
  `FName`, `FText`, `FVaRestJson`, any `USTRUCT`, `TArray<T>`, `TMap<FString, T>`.
* Key overloads take `FUtf8StringView` too, so hot C++ paths avoid TCHAR→UTF-8 conversion.
* Path syntax: dotted with brackets (`a.b[0].c`); a path starting with `/` is RFC 6901 JSON Pointer
  (for keys containing dots).
* Bridges: `ToJsonValue()` / `ToJsonObject()` / `FromJsonValue()` / `FromJsonObject()` for engine
  interop and the v1 shims.

### 2.4 Struct ↔ JSON (`FVaRestJsonConvert`)

* Walks `FProperty` reflection with a **per-struct field map cached** by `(UScriptStruct*, naming policy)`.
  The cache is invalidated when a BP struct is recompiled or reinstanced in the editor.
* **Field names.** C++ properties use the property name. For BP structs (`UUserDefinedStruct`) we always
  use the GUID-suffix-stripping algorithm the engine uses in cooked builds
  (`Runtime/CoreUObject/Private/StructUtils/UserDefinedStruct.cpp:300-306`), **never** the
  editor-only friendly name (`:289-298`). That way the same JSON comes out in PIE and in a packaged game.
* **Encode naming policy** (per client / API / request): `AsIs`, `camelCase` (default), `snake_case`,
  `PascalCase`.
* **Decode is tolerant by default**: exact match → case-insensitive → policy-normalised
  (`user_id` ↔ `UserId` ↔ `userId`). Unknown fields are ignored. Missing fields keep defaults.
  `null` keeps defaults for structs, clears containers. Lenient coercions (`"42"` → int, `1` → bool)
  unless `bStrict`. Every mismatch is recorded as a warning with its JSON path; strict mode turns them into errors.
* C++ per-field renames: `FVaRestJsonNames::Register<FMyStruct>({{ "UserId", "user_id" }})`.
  `UPROPERTY` meta is editor-only data, so meta-based names cannot work in cooked builds and are not offered.
* Types: bool, all ints, float/double, FString, FName, FText (culture-invariant string), enums (name
  string by default, number by option), nested structs, TArray, TSet, TMap (string/name/int/enum
  keys → object keys), `FVaRestJson` fields (raw passthrough), FDateTime (ISO 8601), FTimespan
  (seconds, number), FGuid (`DigitsWithHyphens`), soft object/class paths (string), `TArray<uint8>`
  (number array; base64 by option), TOptional (missing/null ↔ unset). `UObject*` properties are
  skipped with a warning.
* Encode writes **straight to UTF-8 bytes** with the RapidJSON writer — no DOM, no FString — so a
  request body is produced in one pass and moved into `SetContent(TArray<uint8>&&)`.
* Decode in 2.0 goes bytes → arena DOM → struct walk. A streaming **SAX → struct** decoder (no DOM)
  is part of the performance milestone (§8, M7). It becomes the default for typed responses when it beats
  DOM+walk by ≥1.5× on the 10 MB fixture. The DOM path stays for envelopes with predicates and for
  `FVaRestJson` fields.

### 2.5 Blueprint surface (`UVaRestJsonLibrary` + K2 nodes)

* Parse (string → Json, success, error), To String (pretty flag), Load/Save file (UTF-8).
* Make Object / Make Array / From Bool / Number / String / Null; Get Type; Is Valid; Is Null.
* **Wildcard `Get Field` / `Set Field` / `Get Path` / `Array Get` / `Array Add`** (`CustomThunk`, like
  `JsonBlueprintUtilities`): one node adapts to whatever is connected — int, string, struct, array
  of structs. `Get*` returns a `Found` bool.
* Typed convenience getters with defaults (pure): Get String / Int / Int64 / Number / Bool /
  Object / Array (Key, Default).
* Object: Keys, Has, Remove, Merge; Array: Length, Remove At; Clone; Equals (deep).
* **Struct To Json / Json To Struct** (wildcard), Struct To Json String.
* **`Make Json` K2 node** (v2, in `VaRestEditor`): add named pins; each pin is a wildcard.
* `FVaRestJson` struct ops: `ExportTextItem`/`ImportTextItem` (a pin default can be typed as JSON
  text), `Identical` (deep equality), `Serialize` (SaveGame: compact UTF-8).

---

## 3. HTTP core (`VaRestHttp`)

### 3.1 Client

```cpp
FVaRestClient Api = FVaRestClient::Make(TEXT("https://api.game.io"))
    .WithBearer(Token)
    .WithRetry(FVaRestRetryPolicy::Exponential(/*MaxRetries*/ 3))
    .WithTimeout(/*Total*/ 15.f, /*Activity*/ 5.f)
    .WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).ErrorMessageAt(TEXT("error.message")))
    .WithCache(FVaRestCachePolicy::MemoryAndDisk())
    .WithOfflineQueue(TEXT("GameApi"))
    .WithNaming(EVaRestJsonNaming::SnakeCase)
    .WithMaxConcurrent(8);
```

Configuration is mutable at runtime (tokens change), on the game thread. A request snapshots the
configuration at `Send()`. Custom stages: `.Use(Stage, EVaRestStageSlot)` (§4).

### 3.2 Request builder

```cpp
FVaRestRequestHandle H = Api.Get<FProfile>(TEXT("/users/{id}"))
    .Path(TEXT("id"), UserId)                 // percent-encoded path segment
    .Query(TEXT("lang"), TEXT("en"))
    .Owner(this)                              // weak: owner gone -> no callbacks, no further retries
    .OnSuccess([this](const FProfile& P) { ... })        // also (const T&, const FVaRestResponse&)
    .OnError([](const FVaRestError& E) { ... })
    .Send();

Api.Post(TEXT("/events")).Body(FVaRestJson::Object().Set(TEXT("type"), TEXT("kill"))).Send();
Api.Post<FOrder>(TEXT("/orders")).Body(NewOrder).Idempotent().QueueWhenOffline().Send();
```

* Verbs: `Get Post Put Patch Delete Head`, `Request<T>(Verb, Path)`. `T` defaults to `FVaRestJson`;
  `void` (no payload), `FString`, `TArray<uint8>`, and `FVaRestResponse` (raw) are allowed.
* The builder is `[[nodiscard]]` and its methods return it **by value** (a moved handle, cheap), so
  the class-level attribute applies and forgetting `Send()` is a compile warning. In non-Shipping builds
  a builder destroyed unsent also logs an error naming the URL.
* Options: `Header`, `Body(struct | FVaRestJson | FString | TArray<uint8> | FVaRestForm | FVaRestMultipart)`,
  `Timeout`, `Priority`, `Retry(policy)`/`NoRetry()`, `Idempotent()` (adds a stable
  `Idempotency-Key: <uuid>` across attempts), `Cache(policy)`/`NoCache()`, `QueueWhenOffline()`,
  `NoAuth()`, `Tag(FName)`, `OnComplete(TVaRestResult<T>)`, `OnProgress(sent, received, total)`
  (throttled, game thread), `OnQueued(queue id)`, `SaveTo(path)` (streams the body to a temp file,
  atomic rename on success), `OnHttpThread()` (opt-in: callbacks on the HTTP/worker thread for
  heavy consumers).
* URL building: base URL + path; an absolute URL in `Path` overrides the base. Whitespace trimmed
  (v1's iOS fix). RFC 3986 encoding for path segments and query values.
* Bodies: struct/`FVaRestJson` → `application/json` UTF-8; `FString` → `text/plain; charset=utf-8`;
  bytes → `application/octet-stream`; `FVaRestForm` → `application/x-www-form-urlencoded`;
  `FVaRestMultipart` → `multipart/form-data`, file parts streamed with `SetContentFromStream`.
  A body on GET is dropped with a warning (v1 rule). Every content type can be overridden with `Header`.
* Response decode by `Content-Type`: `application/json` or `+json` → JSON → envelope → `T`.
  204/empty → `T` default. Non-JSON when `T` needs JSON → `Decode` error.
* Raw response bytes are released after a successful decode (memory). They are kept on error for
  diagnostics, when `T` is raw, or with `.KeepBody()`.
* `FVaRestRequestHandle`: `Cancel()`, `IsActive()`, `GetId()`. Weak — it does not keep the request alive.
* Lifetime: in-flight exchanges are owned by the client state, and they keep the client state alive.
  No `AddToRoot` anywhere.

### 3.3 Results and errors

`TVaRestResult<T>`: value or `FVaRestError`, plus `FVaRestResponse` metadata.

`FVaRestResponse`: status, headers (case-insensitive lookup), effective URL, elapsed, attempts,
`bFromCache`, body bytes (per the release rule above).

`FVaRestError` (USTRUCT, BlueprintType):

| Field | Meaning |
| --- | --- |
| `Kind` | `Canceled`, `Connection`, `Timeout`, `Http`, `Server` (envelope says failure), `Decode`, `Auth` (refresh failed / no credentials), `Offline` (no connectivity, not queued, or queued item expired), `Config` (bad URL, invalid request) |
| `HttpStatus` | 0 when no response |
| `Code`, `Message` | from the envelope or transport; human-readable fallback always present |
| `Body` | parsed error body if JSON (else invalid handle) |
| `RawBody` | first 4 KiB of a non-JSON error body |
| `Attempts`, `bRetryable`, `Tag`, `Url` (redacted) | diagnostics |

BP helpers: `Describe Error`, `Is Http Status`, `Switch on EVaRestErrorKind`.

### 3.4 Envelope

`FVaRestEnvelope` (per API, overridable per endpoint/request):
`DataPath` (empty = whole body), optional `SuccessPath` + `SuccessValues` (e.g. `ok` ∈ {`true`},
or dawg-style `Code` ∈ {`200`}), `ErrorMessagePath`, `ErrorCodePath`, `bParseErrorBodies` (read
message/code from 4xx/5xx bodies). A 2xx response whose predicate fails becomes `Kind=Server`.

---

## 4. Pipeline stages

Each request is an `FVaRestExchange` that flows through a fixed chain. A stage is asynchronous
middleware: it may pass the exchange on, observe or alter the outcome, short-circuit (cache hit,
queued), or re-issue (retry, auth replay).

```
Trace tap → Offline queue → Cache → Retry ┐→ Auth → [custom per-attempt] → Log/redact → Limiter → Transport
                                          └────────────── once per attempt ─────────────────────┘
```

Slots for custom stages: `Outer` (sees each logical request once) and `PerAttempt` (sees final
headers including auth — for request signing). Transport is `IVaRestTransport` (default:
`FHttpModule`); tests inject a fake (§7.1).

### 4.1 Retry

`FVaRestRetryPolicy`: `MaxRetries` 3, `BaseDelay` 0.5 s, `Multiplier` 2, `MaxDelay` 30 s, full
jitter, retry on connection error, timeout, and statuses {408, 425, 429, 500, 502, 503, 504};
respect `Retry-After` (seconds or HTTP date, capped at 60 s); optional total `Deadline`.
**Only idempotent verbs** (GET, HEAD, OPTIONS, PUT, DELETE) retry by default; POST/PATCH only with
`.Idempotent()`. Timers run on the core ticker via an injectable clock (`IVaRestClock`), so tests run
on virtual time.

### 4.2 Auth and session

`IVaRestAuth` runtime interface, configured from `UVaRestAuthConfig` (instanced, EditInlineNew
subclasses in the asset):

* `Bearer` — token set at runtime (`SetBearerToken`).
* `BearerRefresh` — refresh endpoint (endpoint name or path), JSON paths for `access_token`,
  `refresh_token`, `expires_in`. Refreshes **proactively** 30 s before a known expiry, and **reactively**
  on 401 (statuses configurable).
* `ApiKey` (header or query), `Basic`.
* `Custom` — C++ `IVaRestAuth`, or BP subclass with native events `Apply` and `Refresh`
  (finished by calling `Finish Refresh(bSuccess)`).

**Single-flight refresh**: the first 401 starts one refresh. Concurrent requests that hit 401 (or
start while it runs) park behind it, then replay once each. If the refresh fails, all parked requests
fail with `Kind=Auth` and the client fires `OnSessionExpired`. The refresh request and `.NoAuth()`
requests bypass the auth stage.

Session state per client: tokens, expiry, session headers (dawg's `X-Session-ID` pattern);
`OnSessionChanged`, `ClearSession()` (logout), `ExportSession()`/`ImportSession()` (JSON, for games
that persist sessions themselves; the docs carry the security caveat).

### 4.3 Cache

`FVaRestCachePolicy`: `None` | `Memory` | `MemoryAndDisk`; 8 MiB memory and 64 MiB disk LRU caps;
`Saved/VaRest/Cache`; respects `Cache-Control` (`max-age`, `no-store`, `no-cache`); optional default
TTL; `bServeStaleOnError` (serve stale when offline). Only GET/HEAD with status 200 are stored.
Revalidation sends `If-None-Match`/`If-Modified-Since`; a 304 refreshes metadata and serves the
cached body. **The cache key includes a hash of the auth identity** (SHA-256 of the applied
`Authorization` value), so after logout/login one user never sees another's cached data. Bytes are
cached, not decoded values; disk IO is on background tasks. `ClearCache()`.

### 4.4 Offline queue and connectivity

* Per-request opt-in `.QueueWhenOffline()` (or endpoint flag). Eligible only if idempotent or
  `.Idempotent()`; streamed/multipart-file bodies are not queueable (config error).
* When a request fails with `Connection` after its retries, or when the Hub already knows it is
  offline, the request is persisted (verb, resolved URL, non-auth headers, body bytes,
  idempotency key, tag, created time) under `Saved/VaRest/Queue/<client id>`. The original
  completion fires **`OnQueued(QueueId)`** (BP: `On Queued` pin), not success or error.
* Replay is FIFO per client, one at a time (preserves order). Auth is re-applied, and the request goes through the normal
  pipeline minus the offline stage. The final outcome is delivered **only** through the client event
  `OnQueuedResult(QueueId, Tag, bSuccess, Response, Error)`. This works the same after an app
  restart, when the original callbacks no longer exist.
* Client identity: the API asset path, or the explicit name in `.WithOfflineQueue(Name)` for
  code-first clients. Limits: `MaxQueued` 256, `MaxAge` 24 h (expired → `OnQueuedResult` with
  `Kind=Offline`).
* Connectivity (Hub-wide): offline after a transport connection failure; online after any success or
  a probe success (`ProbeInterval` 10 s; probe = head of a queue or an optional `HealthUrl`); on
  mobile also uses `FPlatformMisc::GetNetworkConnectionType()` as a hint. Fires
  `OnConnectivityChanged`. While offline, `QueueWhenOffline` requests queue immediately without
  wasting attempts.

### 4.5 Logging and redaction

`EVaRestLogLevel`: `None`, `Basic` (verb, redacted URL, status, ms, bytes), `Headers`, `Body`.
Default `Basic`; overridable per client and by the cvar `VaRest.LogLevel`. Body logging is compiled
out in Shipping. Always redacted (replaced by `***`):

* headers `Authorization`, `Proxy-Authorization`, `Cookie`, `Set-Cookie`, `X-Api-Key`;
* query params `token`, `access_token`, `api_key`, `key`, `password`;
* JSON body fields `password`, `token`, `access_token`, `refresh_token`, `secret`, `client_secret`;
* plus per-client additions.

Logged bodies are truncated to 4 KiB. (v1 logged every response body on desktop regardless of
`bExtendedLog`: `VaRestRequestJSON.cpp:510`.)

### 4.6 Concurrency limiter

Per-client `MaxConcurrent` (default 16) with a priority queue (Highest → Low, FIFO within a
priority). It sits next to the transport, so queued retries wait their turn too.

### 4.7 Timeouts

Per attempt: `SetTimeout` (total) and `SetActivityTimeout`; plus the retry `Deadline` across attempts.

### 4.8 Trace tap and stats

The outermost stage emits `FVaRestTraceEvent`s (Started, AttemptStarted, Response, RetryScheduled,
CacheHit, Queued, Completed) with **already-redacted** copies and body excerpts (editor/dev builds
only, size-capped) to registered `IVaRestTraceSink`s. The Network Inspector spec consumes this.
Stats: `stat VaRest` (requests, in-flight, bytes up/down, retries, cache hit ratio) and an LLM tag
`VaRest`.

---

## 5. Assets, Hub, and the Blueprint/C++ surface

### 5.1 `UVaRestApi` (data asset)

* `Environments` (`{Name, BaseUrl}`), `DefaultEnvironment`, optional `ShippingEnvironment`;
  command line `-VaRestEnv=<Name>` or `-VaRestEnv.<AssetName>=<Name>`; runtime `SetEnvironment`.
* Default headers, user agent; `Auth` (instanced `UVaRestAuthConfig`); retry, timeouts, cache,
  offline, envelope, log level, naming policy, max concurrent.
* `Endpoints`: `{Name, Verb, Path ("/users/{id}"), DefaultQuery, Headers, RequestType
  (UScriptStruct*, none = FVaRestJson/no body), ResponseType (none = FVaRestJson), bNoAuth,
  bIdempotent, bQueueWhenOffline, per-endpoint overrides (bOverrideX + struct), Description}`.
  The OpenAPI importer fills this list.
* `IsDataValid`: duplicate names, malformed path templates, invalid URLs, missing environments.

### 5.2 `UVaRestHub`

GameInstance subsystem; `UVaRestHub::Get(WorldContext)` falls back to an engine-scoped instance
when there is no game instance. `GetClient(UVaRestApi*)` (lazy), `MakeClient(BaseUrl)`,
`IsOnline()`, `OnConnectivityChanged`. On `Deinitialize` it cancels in-flight requests and flushes
offline queues to disk. C++: `UVaRestHub::Get(this)->Api(GameApi)` returns the asset's
`FVaRestClient` handle (by value; handles are cheap).

### 5.3 `UVaRestApiClient` (BP)

Functions: Set Bearer Token, Clear Session, Set/Remove Header, Set Environment, Get Base Url,
Cancel All (by tag), Clear Cache, Get Queued Count, Flush Queue. Events: `OnError`,
`OnSessionChanged`, `OnSessionExpired`, `OnQueuedResult`.

### 5.4 Blueprint nodes

* **`Call Endpoint`** (K2, `VaRestEditor`). The Api asset and endpoint are **node properties**, set by
  the action-menu spawner: typing "Login" in the BP context menu offers *VaRest ▸ DA_GameApi ▸
  Login*. The actions refresh when the asset changes (`FBlueprintActionDatabase::RefreshAssetActions`),
  and they are editable in the details panel.
  * Inputs: one per `{path param}`, Query map, typed `Body` (RequestType), Tag.
  * Outputs: `Then`, `On Success` + typed `Response` + `Http` (status/headers); `On Error` + `Error`;
    `On Queued` (only if the endpoint allows offline); `Handle`.
  * **Implementation.** The node expands to an internal async action that decodes into an `FInstancedStruct`
    **off the game thread** (it knows `ResponseType` at runtime). On success it copies that into the
    typed output via a CustomThunk getter, the same way the engine's instanced-struct getter does.
  * Pins rebuild on `ReconstructNode`. Pins orphaned by an asset change are kept and flagged, never
    silently dropped.
* **`Send Request`** (K2, no asset): Client (optional; default client of the Hub), Verb, URL/Path, Query, Headers,
  wildcard `Body` (struct / Json / String / bytes), `Response Type` picker (default `FVaRestJson`) that
  retypes the `Response` output (like *Get Data Table Row*). Same outputs as `Call Endpoint`.
* Simple async actions (no custom K2): `VaRest Get Json (URL)`, `Download File (URL, Path)` with
  progress, `Upload File`.
* JSON nodes: §2.5.

### 5.5 Settings

`UVaRestHttpSettings` (DeveloperSettings, *Project Settings ▸ Plugins ▸ VaRest*): default log
level, retry, timeouts, naming policy, max concurrent, cache/queue directories. v1's
`UVaRestSettings` stays in module `VaRest`, with its own config section.

---

## 6. v1 compatibility (module `VaRest`) and migration

* Every v1 header, class, struct, enum, delegate and `UFUNCTION` signature stays, at the same path. BP graphs and
  C++ code compile unchanged.
* **v1 JSON classes keep `FJsonObject`/`FJsonValue` storage.** The v1 C++ API hands out
  `TSharedRef<FJsonObject>& GetRootObject()` (`VaRestJsonObject.h:28`), and v1 BP semantics share
  nested objects by reference. Both hold only if the storage stays. New bridge functions:
  `ToVaRestJson()` / `FromVaRestJson()` and BP nodes `To VaRest Json` / `To Legacy Json Object`
  for incremental migration.
* The custom parser `VaRestJsonParser` (1150 lines) is deleted. `DeserializeFromUTF8Bytes` and
  `DecodeJson` use the v2 parser and convert. `bUseChunkedParser` is ignored, with a warning.
* `UVaRestRequestJSON` runs on a v2 `FVaRestClient` underneath: raw body and content type built the
  v1 way, raw response bytes back into the v1 fields. It gains redaction, trace tap, and correct
  lifetime. Requests are kept alive by the Hub's in-flight set instead of `AddToRoot` (`VaRestRequestJSON.cpp:305`).
  `CallURL`, latent `ApplyURL`, events, tags, and headers behave as before.
* Deprecation: every v1 `UFUNCTION` gets `DeprecatedFunction` + a `DeprecationMessage` naming its v2
  replacement. Deprecated nodes still compile and run, and are hidden from the action menu for new graphs.
  C++ `UE_DEPRECATED` markers wait until 2.1, to avoid warning floods in 2.0.
* `docs/MIGRATION.md`: concept map (Construct Json Request → Send Request / Call Endpoint; Construct
  Json Object → Make Json; Get*Field → Get Field; CallURL → Send Request; ApplyURL → async node),
  node-by-node table, C++ table, behaviour changes (logging, lifetime, parser), removal timeline (v3).
* **Compat fixture.** BP assets using every v1 node, authored with v1 built on UE 5.8, stored in the
  repo (not packaged). They must load, compile, and run against the local test server under v2.

---

## 7. Verification

### 7.1 Automation tests (`VaRest.*`)

* `VaRest.Json.*`: round trips, number edges (int64/uint64/-0/1e308/denormals), UTF-8/escapes/surrogates,
  malformed input with exact positions, depth limit, duplicate keys, path access, mutation and handle
  stability (grow a parent while child handles are held), reference vs. clone semantics, bridges.
* `VaRest.Json.Convert.*`: every supported property kind, naming policies, tolerant/strict decode,
  BP struct names (the authored-name algorithm), warnings with paths.
* `VaRest.Http.*` on the fake transport and virtual clock: builder/URL encoding, bodies, decode by content
  type, envelope, errors, retry timing and `Retry-After`, idempotency rules, single-flight refresh,
  cache keying (auth identity), revalidation, offline persistence/replay/expiry, redaction, limiter
  order, owner-gone behaviour, cancel.
* `VaRest.Legacy.*`: v1 behaviour on the shims.
* `VaRest.Integration.*` against `Tools/testserver/` (Python 3 stdlib: echo, status, delay,
  Retry-After, ETag, login/refresh/protected, flaky, large, chunked, envelope ok/err).
* **Restore-the-bug standard** (VaCuus convention): each critical behaviour test must be seen failing
  with the behaviour deliberately broken, then passing once it is restored.

### 7.2 Builds and runs

Linux editor + monolithic Development and Shipping game targets on the host. A packaged Linux run
decodes BP-struct responses (proves cooked field names). Win64 and Mac builds before release.
Android and iOS compile checks before release.

### 7.3 Benchmark gate (first implementation milestone)

`VaRest.Bench.*` (perf filter, not in the default suite), deterministic fixtures from
`Tools/gen_bench_json.py` (1 MiB and 10 MiB realistic API payloads). Measured: parse time (median of
N), peak memory, typed decode, serialize. Compared against `FJsonSerializer` + `FJsonObject`,
`FJsonObjectConverter`, and the v1 request path. Results are recorded in `docs/passport/`.
**Gate:** if the VaRest DOM is not clearly ahead (≥2× faster parse **or** ≥2× lower peak memory on
the 10 MiB fixture, and not worse on the other), fall back to an `FJsonValue`-backed
`FVaRestJson`. API and BP stay the same, and the decision is recorded.

**Gate result (2026-10-19):** PASS — see docs/passport/2026-10-19-json-bench.md. Measured: parse time and *retained* memory (peak was not measured; the engine path's peak is larger than its retained figure, so the memory comparison holds a fortiori). `FJsonObjectConverter`, typed decode and the v1 request path are deferred to M2/M3.

---

## 8. Milestones

| M | Content | Exit |
| --- | --- | --- |
| M0 | Branch, host project, CLAUDE.md/AGENTS.md, beads, v1 builds on 5.8 (tag `v1-on-5.8`) | host builds; v1 loads on 5.8 |
| M1 | JSON arena DOM, parse/serialise, C++ API, bridges, **benchmark gate** | Json tests green; gate decided and recorded |
| M2 | Struct ↔ JSON, BP JSON library, wildcard nodes, Make Json K2 | Convert tests green; BP smoke |
| M3 | HTTP client, builder, transport, threading, bodies, errors, progress, file up/down, logging/redaction, limiter, fake transport, test server | Http tests green |
| M4 | Retry, auth/session, envelope, cache, offline queue + connectivity, trace tap, stats | stage tests green, restore-the-bug done |
| M5 | API asset, Hub, UVaRestApiClient, Call Endpoint / Send Request / simple nodes, settings, asset tooling | BP end-to-end against test server |
| M6 | v1 compat fixture authored from tag `v1-on-5.8`, v1 shims on the new core, fixture verification, MIGRATION.md | fixture loads/compiles/runs |
| M7 | SAX→struct decoder (per §2.4 rule), benchmark passport, monolithic/Shipping/cooked runs, README | passport published; builds green |

Each milestone gets its own implementation plan in `docs/superpowers/plans/`, written when the
previous one lands, so each plan is grounded in code that already exists. The Network Inspector spec
is written after M4 (it consumes the trace tap); the OpenAPI import spec after M5 (it fills the
API asset).

## 9. Risks

* **HTTP-thread completion** may differ per platform backend (curl, Apple, Android). Mitigation:
  verify per platform in M3. If unsupported, fall back to game-thread completion with an immediate
  hop to a background decode task.
* **K2 node complexity** (`Call Endpoint` reconstruction on asset edits). Mitigation: pins are a
  pure function of `(asset, endpoint)`; orphan pins kept; a dedicated reconstruction test.
* **Arena growth under heavy mutation.** Mitigation: `Clone()`; the docs recommend building bodies
  from structs.
* **Legacy fixture authoring** needs v1 to build on 5.8. Mitigation: the minimal port is part of M0.
  If v1 cannot be made to build, author the fixture on 5.4 and resave on 5.8.
