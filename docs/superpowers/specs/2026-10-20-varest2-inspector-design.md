# VaRest 2 — Network Inspector design

Status: revision 1, 2026-10-20. This revision follows the adversarial review
`.superpowers/sdd/inspector-spec-review.md` and the controller's rulings R1–R6. Appendix A maps every review item
to its disposition. The Inspector is one of the four 2.0 extras (core §0.2).
Target: Unreal Engine 5.8.1, branch `v2`. It builds on three specs:

* `2026-10-18-varest2-core-design.md` ("core");
* `2026-10-20-varest2-websocket-design.md` revision 2 ("WS");
* `2026-10-20-varest2-socketio-design.md` revision 1 ("SIO").

§ numbers prefixed with *core*, *WS* or *SIO* refer to them. Core §8 placed this spec after M4. It is written
earlier so that the trace surface it consumes (§9) goes into the core M3/M4/M5, WS W5 and SIO S4 plans instead
of being retrofitted.

---

## 0. Context

### 0.1 Intent

The owner's goal for VaRest 2 is "a universal web hub/API … maximum web sugar out of the box … if your game
talks to the web, it uses VaRest". The Network Inspector is that hub's equivalent of the browser DevTools
Network panel. It shows every HTTP request, WebSocket and Socket.IO connection VaRest makes. It also shows the
decisions VaRest's own pipeline took along the way (retries, auth refresh, cache, offline queue, envelope,
decode), which no proxy and no browser can see.

What success looks like:

* **Live view.** Press Play and open *Tools ▸ Debug ▸ VaRest Network Inspector*. Every request and socket of
  every PIE instance appears live, with status, duration, size, attempts, cache and offline state, and who
  made the call. Selecting one shows its headers and its body as pretty JSON.
* **Failures explained.** A designer sees why a call failed without asking a programmer. Examples:
  * "401 → waited 180 ms for session refresh #41 → attempt 2 → 200";
  * "200 but `ok` was `false` at `status.ok` → `Kind=Server`";
  * "field `level`: expected number, got string" — and *Open in Blueprint* jumps to the *Call Endpoint* node
    that made the call.
* **Secrets stay out.** They never appear on screen, in an export, or on the clipboard. Redaction happens in
  core, before the data reaches the Inspector (§2.2).
* **Device captures.** A QA build on a phone runs with `-VaRestInspector`. `VaRest.Inspector.Export` writes a
  HAR file, which anyone can open in Chrome DevTools.
* **Cost.** With capture off, a trace point costs one relaxed atomic load. Shipping contains no capture code.
* **v1 projects get it for free.** v1 requests run on the v2 client (core §6), so they appear too.

### 0.2 Engine facts this spec relies on (UE 5.8.1)

Paths are relative to `Engine/Source/`.

| # | Fact | Source |
| --- | --- | --- |
| F1 | Nomad tabs are registered on the global tab manager | `Runtime/Slate/Public/Framework/Docking/TabManager.h:1561-1563` |
| F2 | The editor's *Tools ▸ Debug* group is a workspace-menu category | `Editor/WorkspaceMenuStructure/Public/WorkspaceMenuStructure.h:65-74` (`GetDeveloperToolsDebugCategory` :68) |
| F3 | A game can open a nomad tab in its own window. The engine's Widget Reflector does so through the `WidgetReflector` console command and `TryInvokeTab`. It is an engine module that the engine loads explicitly in non-Shipping builds, so it is a precedent for `TryInvokeTab` in a game, not for module loading (that is F4) | `Developer/SlateReflector/Private/SlateReflectorModule.cpp:107-111`, `:135`; `Runtime/Engine/Private/EngineGlobals.cpp:197-202`; `Runtime/Launch/Private/LaunchEngineLoop.cpp:4552-4555` |
| F4 | `DeveloperTool` plugin modules are built when `bBuildDeveloperTools` is set. It is true by default for editor targets and for Debug/Development games, false for Test/Shipping. They load at runtime when `WITH_UNREAL_DEVELOPER_TOOLS` is set | `Programs/UnrealBuildTool/Configuration/Descriptors/ModuleDescriptor.cs:796-797`; `Programs/UnrealBuildTool/Configuration/Rules/TargetRules.cs:1212-1217`; `Runtime/Projects/Private/ModuleDescriptor.cpp:717-719` |
| F5 | `SListView` generates row widgets only for the visible window, and only from `STableViewBase::Tick`. Items are shared, weak or object pointers. `SHeaderRow` supports sorting and hidden columns | `Runtime/Slate/Public/Widgets/Views/SListView.h:1514`, `:1540-1621`; `Runtime/Slate/Private/Widgets/Views/STableViewBase.cpp:370-411`; `Runtime/Slate/Public/Framework/Views/TableViewTypeTraits.h:120-520`; `Widgets/Views/SHeaderRow.h:168-175`, `:337-339` |
| F6 | The engine HTTP module emits no Unreal Insights events, only CSV stats and cycle counters | `Runtime/Online/HTTP/Private/HttpManager.cpp:92-100`, `:366`, `:496` |
| F7 | The engine's process-wide HTTP hooks are single-bind delegates "intended to be used for analytics" | `Runtime/Online/HTTP/Public/HttpManager.h:40-50`, `:150-163` |
| F8 | A request exposes no DNS/connect/TLS split. Only these are public: `GetElapsedTime`, `OnStatusCodeReceived`, `OnHeaderReceived`, `OnRequestProgress64` ("per tick"). They fire on the thread the request's policy selects. Curl's connect time sits in a private header | `Runtime/Online/HTTP/Public/Interfaces/IHttpRequest.h:14-18`, `:63`, `:72`, `:90`, `:361-381`, `:407-419`; `Runtime/Online/HTTP/Private/GenericPlatform/HttpRequestCommon.h:177` |
| F9 | **The editor always traces.** WITH_EDITOR builds enable the `Default` preset, which includes `region` and `bookmark`, even without `-trace`. Trace counters declared with `TRACE_DECLARE_INT_COUNTER` are a plain, non-atomic `int64` (an atomic variant exists). `TRACE_BOOKMARK` needs a literal format | `Runtime/Core/Private/ProfilingDebugging/TraceAuxiliary.cpp:138`, `:1959-1964`; `Runtime/Core/Public/ProfilingDebugging/CountersTrace.h:246-247`, `:310`; `MiscTrace.h:100-108` |
| F10 | **Under `-nullrhi` Slate neither paints nor ticks.** `Slate.SkipWidgetDrawingInHeadlessMode` defaults to true ("this also skips ticking them"), and `PrivateDrawWindows` returns early when `FApp::CanEverRender()` is false. Widget `Tick` and active timers run only inside `SWidget::Paint`. Slate, the null renderer and fonts do exist (editor token, `FSlateApplication::Create`, null renderer with font services), and engine tests construct widgets and call `SlatePrepass` | `Runtime/Slate/Private/Framework/Application/SlateApplication.cpp:136-141`, `:1437-1443`; `Runtime/Core/Public/Misc/App.h:400-407`; `Runtime/SlateCore/Private/Widgets/SWidget.cpp:1498-1512`; `Runtime/Launch/Private/LaunchEngineLoop.cpp:2292-2296`, `:3108-3119`, `:3342-3347`; `Runtime/SlateNullRenderer/Private/SlateNullRendererModule.cpp:128-150`; `Runtime/SlateCore/Private/Test/SlateAttributeTest.cpp:757-759` |
| F11 | PIE: each world context has `WorldType`, `OwningGameInstance` and `PIEInstance`. `GetDebugStringForWorld` yields "Client N", "Listen Server", "Standalone" or "Editor". The editor fires `PreBeginPIE`/`BeginPIE`/`EndPIE` | `Runtime/Engine/Classes/Engine/Engine.h:357`, `:411`, `:418`; `Runtime/Engine/Classes/Engine/GameInstance.h:294`; `Runtime/Engine/Public/UnrealEngine.h:479`; `Runtime/Engine/Private/UnrealEngine.cpp:709-739`; `Editor/UnrealEd/Public/Editor.h:276-286` |
| F12 | **Initiators can be captured.** Outside Shipping/Test, and in the editor, the Blueprint VM keeps a per-thread script stack; each frame has `Node` (`UFunction`), `Object` and `Code`. The editor maps a code offset to its graph node and can focus it. `UE::FSourceLocation::Current()` gives the file and line of a C++ call site (it is compiled out in Shipping) | `Runtime/Core/Public/Misc/CoreMisc.h:338-344`; `Runtime/CoreUObject/Public/UObject/Script.h:466-516`; `Runtime/CoreUObject/Public/UObject/Stack.h:113-119`; `Editor/UnrealEd/Public/Kismet2/KismetDebugUtilities.h:237`; `Editor/UnrealEd/Public/Kismet2/KismetEditorUtilities.h:442`; `Editor/UnrealEd/Public/SourceCodeNavigation.h:355`; `Runtime/Core/Public/Misc/SourceLocation.h:10-11`, `:107-121` |
| F13 | Clipboard and file dialogs: `FPlatformApplicationMisc::ClipboardCopy`; `IDesktopPlatform::SaveFileDialog` through `FDesktopPlatformModule::Get()` | `Runtime/ApplicationCore/Public/GenericPlatform/GenericPlatformApplicationMisc.h:304`; `Developer/DesktopPlatform/Public/IDesktopPlatform.h:129`; `Developer/DesktopPlatform/Public/DesktopPlatformModule.h:18` |
| F14 | Containers and tasks: `TMpscQueue` (node-based, FIFO, one allocation per item); `TRingBuffer` (indexed, `PopFront`); the core ticker; `UE::Tasks::FPipe` (FIFO). `TConsumeAllMpmcQueue` is a stack, so it is not used | `Runtime/Core/Public/Containers/MpscQueue.h:13-18`, `:47-58`; `RingBuffer.h:133-134`, `:490-506`, `:595-602`; `Ticker.h:35`; `Runtime/Core/Public/Tasks/Pipe.h:20-27`; `ConsumeAllMpmcQueue.h:21-25` |
| F15 | Settings, console, shutdown and LLM: `UDeveloperSettings`; `ISettingsModule::RegisterSettings`; `FAutoConsoleCommandWithWorldArgsAndOutputDevice`; `FCoreDelegates::OnEnginePreExit`; `LLM_DEFINE_TAG`; `FApp::IsUnattended` | `Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h:23`; `Developer/Settings/Public/ISettingsModule.h:49`; `Runtime/Core/Public/HAL/IConsoleManager.h:2467`; `Runtime/Core/Public/Misc/CoreDelegates.h:261-262`; `Runtime/Core/Public/HAL/LowLevelMemTracker.h:437`; `Runtime/Core/Public/Misc/App.h:495` |
| F16 | Text and timers: `SMultiLineEditableTextBox`; `FSyntaxHighlighterTextLayoutMarshaller` (the engine has no JSON highlighter); `STreeView::OnGetChildren`; `SWidget::RegisterActiveTimer` | `Runtime/Slate/Public/Widgets/Input/SMultiLineEditableTextBox.h:39`; `Runtime/Slate/Public/Framework/Text/SyntaxHighlighterTextLayoutMarshaller.h:21`; `Runtime/Slate/Public/Widgets/Views/STreeView.h:92-112`; `Runtime/SlateCore/Public/Widgets/SWidget.h:1816` |

External formats:

* **HAR 1.2.** The W3C draft is the reference every tool follows (`webperfwg.org/specs/HAR/Overview.html`;
  never published, marked abandoned). Its rules:
  * "The send, wait and receive timings are not optional and must have non-negative values";
  * `blocked`, `dns`, `connect` and `ssl` may be `-1`;
  * entry `time` equals the sum of the timings that are not `-1`, with `ssl` counted inside `connect`;
  * "Custom fields and elements MUST start with an underscore";
  * `startedDateTime` is ISO 8601.
* **Chrome DevTools' HAR extensions** (`front_end/models/har/Log.ts`, `front_end/core/sdk/NetworkRequest.ts`):
  * `_resourceType`;
  * `_fromCache` (`"memory"` or `"disk"`);
  * for WebSocket entries, `_webSocketMessages: [{type: "send"|"receive"|"error", time: <epoch seconds>, opcode, data}]`.

### 0.3 Non-goals (permanent for this spec; 2.x candidates are in §13)

* **Traffic that does not go through VaRest.** The engine's only process-wide HTTP hooks are single-bind
  analytics delegates (F7). Binding them would take them away from the project's analytics.
* **Interception**: breakpoints, mocks, edit-and-resend, simulated network conditions.
* **A live UI on mobile.** Phones get capture, console commands and HAR export.

---

## 1. Modules and build configurations

### 1.1 Modules

```
VaRestJson          + VaRest::Json::RedactPrefix (prefix-tolerant redacting re-writer, IN-13)
   ^
VaRestHttp          + trace surface, sink registry, snapshot, redactor, excerpts, scopes (§9)
   ^
VaRestInspector     Runtime        capture sink, bounded queue, record store, stats, HAR export, console,
   ^                               BP capture toggle + export action, C++ facade, settings; stubs in Shipping
VaRestInspectorUI   DeveloperTool  view model, Slate panel, nomad tab, VaRest.Inspector.Open; editor menu and
                                   PIE hooks guarded by GIsEditor
```

**`VaRestInspector`** depends on `Core`, `CoreUObject`, `Engine`, `DeveloperSettings`, `VaRestJson` and
`VaRestHttp`. It does not depend on `VaRestWebSocket` or `VaRestSocketIO`, because socket events arrive as
`VaRestHttp` trace types (IW-1–IW-4).

**`VaRestInspectorUI`** depends on `VaRestInspector`, `Slate`, `SlateCore`, `InputCore`, `ApplicationCore` and
`DesktopPlatform`. When `Target.bBuildEditor` is set it also depends on `UnrealEd`, `WorkspaceMenuStructure`
and `ToolMenus`.

Its menu entry and PIE hooks run only when `GIsEditor`. A `-game` separate-process PIE client has WITH_EDITOR
set but `GIsEditor` false, and gets the game-window behaviour (§4.1).

```json
{ "Name": "VaRestInspector",   "Type": "Runtime",       "LoadingPhase": "Default",
  "PlatformAllowList": [ "Win64", "Mac", "Linux", "Android", "IOS" ] },
{ "Name": "VaRestInspectorUI", "Type": "DeveloperTool", "LoadingPhase": "Default",
  "PlatformAllowList": [ "Win64", "Mac", "Linux" ] }
```

Why this split:

* **The BP nodes must cook for Shipping.** A debug widget that calls *Set Inspector Capture* must still cook
  for Shipping. So the Runtime module exists there, with stubs.
* **The UI must never ship.** It is still wanted in desktop Development games. `DeveloperTool` gives both
  (F4).
* **Game modules must not depend on `VaRestInspectorUI`.** It is absent from Test and Shipping builds. The
  docs say so.

The **view model** (`FVaRestInspectorViewModel`: filters, sort, row text cache, detail models) lives in the UI
module but has no Slate dependency in its interface, so it can be tested without Slate (§10.2).

### 1.2 What ships where

Two macros, each defaulted in a header with `#ifndef` so that a target can override it in `GlobalDefinitions`
without redefining it:

* `VAREST_WITH_TRACE_EXCERPTS` (core, `VaRestHttp`), default `!UE_BUILD_SHIPPING`;
* `VAREST_WITH_INSPECTOR` (`VaRestInspector`), default `!UE_BUILD_SHIPPING`.

A special Shipping test build that wants capture sets both. The docs carry the privacy note of §2.2.

| Configuration | Capture, store, console, HAR export | Live UI |
| --- | --- | --- |
| Editor (incl. PIE) | yes; capture **on** by default, **off** in unattended runs (`FApp::IsUnattended`, F15) | editor tab |
| Debug / Development game, desktop | yes; capture **off** by default | own window via `VaRest.Inspector.Open` |
| Debug / Development game, Android/iOS | yes; capture off by default | — |
| Test | yes; capture off by default | — (no DeveloperTool modules, F4) |
| Shipping | **compiled out**. *Is Inspector Compiled In* returns false, *Set Inspector Capture* does nothing, the export action fails with "compiled out", there are no console objects and no sink | — |

UHT types are declared unconditionally (UHT does not allow reflected members under arbitrary `#if`). Only
function bodies and the native store are conditional.

### 1.3 Object model

| Type | Module | Role |
| --- | --- | --- |
| `IVaRestTraceSink`, `FVaRestTraceEvent`, `FVaRestTraceSnapshot`, `FVaRestTraceExcerpt`, `FVaRestTraceSocketClose` | `VaRestHttp` | the contract of §9 |
| `FVaRestInspectorSink` | Inspector | turns each event into a capture item and pushes it onto the queue; nothing else on the emitting thread |
| `FVaRestInspectorStore` | Inspector | game-thread store: rings, budget, eviction, scopes, back-link table, revision, `OnChanged`; one per process |
| `FVaRestInspectorHttpRecord`, `FVaRestInspectorSocketRecord`, `FVaRestInspectorMarker` | Inspector | §3.3 |
| `FVaRestInspectorStats` | Inspector | aggregates that survive eviction (§3.6) |
| `IVaRestInspector` | Inspector | C++ facade: `SetCapture`, `IsCapturing`, `Clear`, `ExportHar` → `UE::Tasks::TTask<TValueOrError<FString, FString>>` (path or error), `OnChanged`, `ForEachRecord` |
| `UVaRestInspectorLibrary`, `UVaRestInspectorExportAction` | Inspector | BP surface (§6.3) |
| `UVaRestInspectorSettings` | Inspector | *Project Settings ▸ Plugins ▸ VaRest Inspector* (§6.2) |
| `FVaRestInspectorViewModel` | UI | data layer of the panel, without Slate in its interface |
| `SVaRestInspector` | UI | the panel; uses the view model plus injected `IVaRestInspectorClipboard` / `IVaRestInspectorFileDialogs` |

---

## 2. Data source

### 2.1 Flow

```
game thread                                                     game thread (core ticker, once per frame)
  core / WS / SIO trace point                                     drain queue → apply to records → budget,
    └─ mask check (relaxed atomic) ─ 0 → nothing                    eviction → stats → revision++ → OnChanged
    └─ build FVaRestTraceEvent from redacted data                   → view model / console / export
    └─ sink snapshot → Inspector sink → capture item → MPSC queue ─┘
decode task / core redaction pipe / socket pipe
  └─ Excerpt events (redacted) → same sinks → same queue
HTTP thread: emits nothing; it writes timestamps into the exchange (IN-3)
```

Every event except `Excerpt` is emitted on the game thread (§9). There are 13 event kinds in all. A typical
request produces 4 events (`Started`, `AttemptDispatched`, `AttemptEnded`, `Completed`) and at most 2
excerpts.

### 2.2 Redaction happens at capture, in core, once (ruling R1)

**The rule.** A sink never receives a byte that was not redacted.

* Core builds every event from redacted data.
* Raw body bytes stay inside core until the redacted excerpt is published (IN-14).
* The Inspector has no redaction rules of its own. Logs, the trace and HAR files therefore agree, and a fix to
  the rules fixes all of them.

The core redactor (IN-13) replaces the exact-match lists of core §4.5 with these layers:

1. **Normalised key matching**, for JSON keys, form keys, header names, query keys and multipart part names.
   The key is lower-cased and `_`, `-` and `.` are removed. It is then redacted when:
   * it equals a listed name: `sig` or `key` (core §4.5's former query key), and for query keys also `code`
     (OAuth authorization codes); or
   * it **contains** one of `password`, `passwd`, `secret`, `token`, `apikey`, `authorization`, `credential`,
     `signature`, `session`, `cookie`, `privatekey` or `jwt`;
   * unless it is on a short allowlist: `idempotencykey`, `tokentype`.

   So VaRest's own default output, which is camelCase (core §2.4), is covered: `accessToken`, `refreshToken`,
   `clientSecret`, `idToken`, `apiKey`, `sessionToken`. So are presigned URLs (`X-Amz-Signature`,
   `X-Amz-Credential`, `X-Amz-Security-Token`, `X-Goog-Signature`, Azure `sig`), `Authorization`, `Cookie`,
   `Set-Cookie`, `X-Api-Key` and `X-CSRF-Token`.
2. **The auth configuration feeds the rules.** Each client's `BearerRefresh` token paths and its `ApiKey`
   header and query names are added to that client's keys. Session headers set through the session API
   (core §4.2) are added too.
3. **Known-secret value scrub.** The redactor keeps a copy-on-write set of each client's current secret values:
   * bearer and refresh tokens, the API key and Basic credentials;
   * session header values;
   * the WS `FirstMessage` token and the SIO CONNECT payload values.

   After a rotation it also keeps the previous four values for 10 minutes. Every occurrence of 8 bytes or more
   is replaced with `***` in every captured string: URL paths, text bodies, error messages, close reasons and
   frames. This covers what no key rule can. It runs on the redaction pipe (a `memmem` of a few needles over at
   most the cap).
4. **Binary is not captured by default.** A binary body or frame becomes a descriptor: size, content type and
   the first 8 hex digits of its SHA-256. Hex capture is opt-in (`MaxBinaryExcerptBytes`, default 0), because
   protobuf and FlatBuffers store strings verbatim.
5. **Every URL field is URL-redacted**: request URL, effective URL, `Location`, `Content-Location`, the WS
   handshake URL. Userinfo (`user:pass@`) is redacted.

Per payload:

| Payload | Captured as |
| --- | --- |
| JSON (`application/json`, `+json`, or sniffed: `text/*` or no type, first non-space byte `{` or `[`) | `RedactPrefix` over the first `MaxBodyBytes`: a matching key's value of any type, complete or cut, becomes `"***"`; tokens cut by the cap are never written, and neither is a scalar ending exactly at the cap; a parse error ends the excerpt at the last complete token, with the error noted; then the value scrub |
| form | pairs, keys per layer 1–2, then the value scrub |
| multipart | part descriptors (name, filename, type, size); text parts by name per layer 1–2, then the value scrub; binary parts → descriptor |
| other `text/*` | text up to the cap, then the value scrub |
| binary | descriptor (layer 4) |
| streamed (`SaveTo`, file parts) | descriptor; path relative to the project when inside it |
| socket frames | the same, per frame; SIO CONNECT values, `pid` and EIO `sid` → `***` (IS-2) |
| `FVaRestError` | `Url` redacted (core §3.3), `Message` value-scrubbed; `Body`/`RawBody` are not copied (the response excerpt is the body) |

What remains uncovered, and is documented: a secret that is not a known value and sits in a path segment, in a
text body under no key, or in opt-in binary.

### 2.3 Ordering, late attach, late data, sealing

* **Ordering.** Events of one request come from one thread, the game thread, so queue order is causal order.
  Each event carries a `Seq` and a timestamp from core's `IVaRestClock`. These are taken only when the mask is
  non-zero.
* **Late attach (ruling R1, review #1).** Ids are always assigned when an object is created (IN-10). When a
  sink is added (Record after Pause, capture enabled mid-session, or the Inspector module loading after the
  `PreDefault` core modules), core calls `OnAttached(FVaRestTraceSnapshot)` on the game thread. The snapshot
  holds:
  * live scopes and clients;
  * sockets, with their state, current attempt, open-since time and channels;
  * every in-flight request, as `Started{bPartial}` with its verb, redacted URL, template, endpoint, current
    attempt and start time.

  The store builds rows from it. An event with an id the store does not know (an `Excerpt` racing the
  snapshot, for example) creates a "(started before capture)" record instead of being dropped (restore-the-bug
  R6).
* **Late data.** `Started` and `AttemptEnded` say whether an excerpt is coming (`bExcerptPending`). The excerpt
  arrives later: from a background thread (the decode task or the redaction pipe), or, for `SkipReason` `Backlog` and
  `Discarded`, emitted on the game thread. `bExcerptPending` is a promise only to sinks whose detail has Bodies: an
  announced excerpt lapses without an event when no sink wants Bodies any more, and a sink without Bodies ignores
  `bExcerptPending` and any `Excerpt` it receives (M4a Task 21 review M-1; Task 22 review). If an excerpt has not come
  5 s after the record completed (measured on `IVaRestClock`), the record shows "excerpt timed out"; this stays the
  backstop. An excerpt for an evicted record is dropped.
* **Sealing.** A record is sealed when it has completed and nothing is pending. Sealed records are immutable.
  Links that appear later live in a game-thread **back-link table** keyed by id, never in the sealed record:
  * a queue replay finishing minutes later;
  * the requests parked behind a refresh.

  Export reads sealed records by shared pointer, and unsealed ones from a copy taken on the game thread (R8).

---

## 3. Capture

### 3.1 Turning it on; zero cost when off

Capture is controlled by:

* settings (§6.2) and `-VaRestInspector` / `-VaRestInspector=0`;
* the cvar `VaRest.Inspector.Capture 0/1`;
* the UI's Record / Pause button;
* BP *Set Inspector Capture*.

Headers and bodies have their own settings (`bCaptureHeaders`, `bCaptureBodies`, both on).

How "zero cost" is exact:

1. **Detail mask.** Core keeps one atomic summary of all registered sinks' detail (`Metadata`, `Headers`,
   `Bodies`, caps). Every trace point starts with `if (VaRest::Trace::Detail() == 0) return;`, a relaxed load
   and a branch. When it is 0, nothing else happens: no `Seq`, no clock read, no allocation, no copy, no
   redaction.
2. **Delegates.** The transport binds `OnStatusCodeReceived` and `OnRequestProgress64` (F8) only when
   `Metadata` was set at dispatch (IN-11).
3. **Off and Pause unregister the sink.** Pausing is not a flag the sink checks.
4. **Sink list.** It is an immutable shared snapshot swapped on the game thread. Emitters take it only after
   the mask check. After `RemoveSink` returns, a background emitter (a pipe) may still deliver an excerpt, and
   sinks tolerate that (IN-12).
5. **Shipping** has no Inspector sink. Insights emission is deferred (§13), so no second sink keeps the mask
   non-zero.
6. **Proof by counting, not timing.** Core's dev-test counters (events built, redactor calls, delegate binds)
   stay at 0:
   * over 1000 requests with no sink (R3);
   * from the first frame after a pause (R12).

   The tests use the test-only `FVaRestTrace::FScopedSinkIsolation` (IN-12). While it lives, only the sinks
   added inside it are active, so the editor's own Inspector sink cannot distort core's or the Inspector's
   suites.

Capture defaults on in the editor because the usual moment to open the Inspector is right after something went
wrong, and the data must already be there. It defaults off in unattended runs (automation, build machines).

### 3.2 Threads: sink → queue → store

* **The sink** runs on the thread that emitted the event. For the game thread that means building a capture
  item (a POD header with moved-in strings and shared excerpt references), pushing it onto a `TMpscQueue`
  (F14) and returning. For a pipe it does the same with an `Excerpt`.
* **Bounded by count and bytes.** The sink keeps atomic counters of queued items and queued bytes (strings
  plus excerpt bytes). The limits are `MaxPendingEvents` (64 Ki in the editor, 16 Ki packaged) and
  `MaxPendingBytes` (16 MiB / 4 MiB). Over the byte limit, excerpts are replaced by "excerpt skipped: capture
  backlog". Over the count limit, items are dropped. Both are counted, and one `CaptureOverflow` marker per
  stall reports them (R10).
* **Core's raw copies** waiting for redaction have their own admission limit (IN-14). The two limits together
  bound all capture memory outside the store.
* **The store** is owned by the game thread. A core-ticker callback (F14) drains the queue once per frame,
  applies the items, enforces the budget, updates stats, bumps the revision and fires `OnChanged`. Tests drain
  explicitly.

### 3.3 Records

**HTTP record**, one per logical request, keyed by `RequestId` (= `FVaRestRequestHandle::GetId()`, shown as
`#57` in the UI and in core's log lines, IN-16):

| Group | Fields |
| --- | --- |
| identity | `RequestId`, `ScopeId`, `ClientId`, endpoint name, path template, tag, priority, flags (`Idempotent`, `QueueWhenOffline`, `NoAuth`, `AuthRefresh`, `Partial`) |
| initiator | owner object path; the Blueprint frame (object, function, code offset; resolved to a node in the editor, F12); the C++ call site (file, line) |
| links | `QueueReplayOf` (queue id, plus the original request id in the same process run; zero across runs); back-links in the side table (§2.3) |
| request | verb, redacted URL, headers as built, body descriptor + excerpt, cache policy, current pending/lookup state |
| attempts (≤ 16) | number, start, gap before it (kind, seconds), auth wait (+ refresh request id), limiter wait, prepare time, final redacted headers (stored only when they differ from the previous attempt), conditional (revalidation) flag; status, redacted response headers, effective URL, bytes up/down, content type, failure (kind + detail), transport timestamps, response excerpt |
| notes | cache revalidated / stored / served stale, retry scheduled (reason, delay, `Retry-After`), queued (queue id, reason), queue expired |
| outcome | `InFlight`, `Success`, `Error(Kind)`, `Queued`, `Canceled(reason)`; the redacted `FVaRestError`; total duration |
| decode and envelope | result type, decode start/end, up to 32 decode warnings (JSON path + message, core §2.4) and their total, envelope: data path, failed success predicate (path, expected, actual), extracted code and message (core §3.4) |
| timeline | every event of the record with its time, behind the Timing tab |

**Phases (review #8).** Phases come only from explicit durations carried by events. Nothing is inferred from
event gaps.

| Phase | Source | Inside the attempt? |
| --- | --- | --- |
| `Gap` (`Backoff` or `SessionRefresh`) | `AttemptDispatched.GapBefore`: from the previous attempt's end to this attempt's start (a retry delay, or a reactive 401 park) | no: drawn before the attempt, belongs to no HAR entry |
| `Auth` | `AttemptDispatched.AuthWait`: parked in the auth stage inside this attempt (proactive refresh, single-flight wait) | yes |
| `Queued` | `AttemptDispatched.LimiterWait` | yes |
| `Prepare` | dispatch − attempt start − `Auth` − `Queued` (custom per-attempt stages, header building) | yes |
| `Send` | dispatch → `UploadDoneAt`; merged into `Wait` when unknown | yes |
| `Wait` | → `StatusAt` (time to first byte) | yes |
| `Receive` | → `TransportDoneAt` | yes |
| `Decode` | `Completed.DecodeStartAt` → `DecodeEndAt` | after the last attempt |
| `Deliver` | decode end → `Completed` | after the last attempt |

Invariant, checked by the golden tests (IN-17) and the HAR validator (R7): an attempt's in-attempt phases sum to
its wall span, from attempt start to `TransportDoneAt`, within clock rounding.

**Attempts that never left (M4a final review C-I-1).** An attempt that a park before it (a refresh that fails, is
rejected or cleared, the deadline, a cancel) or the limiter's queue ends unsent takes its number like any other: core's
log line, `FVaRestError::Attempts` and a note's `Attempt` count it. It has no `AttemptDispatched`/`AttemptEnded` pair, so
the record has no attempt row for it; the next attempt's `GapBefore` is measured from its end. The refresh such a park
waited on is `Completed.AuthRefreshId`, so the back-link table (§2.3) lists the request under refresh #R even with no
dispatch.

Cache/offline may answer before Retry enters any attempt. Their request-level CacheServedStale,
Queued and QueueExpired notes use Attempt=0 and the real nonzero RequestId; there is no fabricated
attempt row. Queue wait is outer logical time, never limiter/auth time. Replay starts a fresh logical
interval, so time spent in the durable queue is not transport duration. Immediate Started captures
initial cache eligibility/pending state; attach snapshots show current unresolved None/pending or
validated disposition, with the original StartedAt. Repeated Outer passes reset lookup state.
Completion clears unresolved pending to false/None but preserves an established lookup history;
Completed.bFromCache follows the delivered response. A completed queued original is absent from
later in-flight snapshots; its replay is a fresh record linked by QueueId, with original numeric id zero
across process runs. Asynchronous disk writes never reopen a sealed record or emit late CacheStored.

The engine gives no DNS, connect or TLS split (F8), and the timestamps have the engine's delegate cadence. The UI
uses VaRest's own phase names and does not pretend to Chrome's precision.

**Socket record**, one per socket object, across reconnects (WS §12):

* identity: socket id, scope, name, protocol (`ws`, `sio`), redacted URL, entry, initiator;
* lifetime: created and released time, current state;
* attempts (the last 64): gap before (reason, seconds), redacted handshake, result (open: connect ms and
  protocol; failure: reason and status), close (`FVaRestTraceSocketClose`);
* channels and namespaces: state, public id, recovered, last reason;
* frame ring (§3.4);
* requests and acks: id, channel, route, sent time, outcome, latency;
* counters: frames and bytes in/out, drops by reason, reconnects, flow-control pauses, RTT last and average,
  evicted frames.

**Frame:** `FrameId`, attempt, direction, kind, bytes, channel, route, message id and kind, protocol tag,
attachment sizes, `bAutoReplied`. Inbound frames carry the time received at the transport and the time
delivered on the game thread. Outbound frames carry the time of the send call and the hand-off time, plus the
lane. Every frame carries its excerpt.

### 3.4 Memory: caps, eviction

| Limit | Editor | Packaged |
| --- | --- | --- |
| `BudgetMiB` (records, frames, excerpts; the store's own accounting) | 64 | 8 |
| `MaxRecords` (HTTP + markers) | 10 000 | 2 000 |
| `MaxBodyBytes` (text/JSON excerpt) | 64 KiB | 16 KiB |
| `MaxBinaryExcerptBytes` | 0 (descriptor) | 0 |
| `MaxFrameExcerptBytes` | 4 KiB | 1 KiB |
| `MaxFramesPerSocket` | 5 000 | 1 000 |
| `MaxPendingEvents` / `MaxPendingBytes` (§3.2) | 64 Ki / 16 MiB | 16 Ki / 4 MiB |

**Eviction.** One global FIFO of evictable units, in arrival order: a completed HTTP record, a marker, or a
single socket frame. While the store is over budget or over a count, the oldest unit goes.

* **In-flight records are pinned, and they count in the budget.** The limiter queue has no cap (core §4.6). So
  when only pinned records remain over the budget, the details (headers and excerpts) of the oldest records
  that have not yet been dispatched are dropped. Their rows stay, marked "details evicted" (R5 covers the
  pinning).
* **Socket rows** live until their socket is released and they are older than every other unit. Frames go one
  by one: first per socket, then through the global FIFO. The row shows "N older frames evicted".
* **Evicted ids** are remembered (the last 4096), so late excerpts and links say "evicted" rather than
  "unknown".

**LLM.** Core allocates trace data under a `VaRest_Trace` tag (IN-14). The store allocates under
`VaRest_Inspector`. The budget bench compares the store's accounting with the heap delta (§8).

### 3.5 Scopes: PIE instances and hubs

* **A scope is a `UVaRestHub`** (core §5.2), one per game instance, plus the engine-scoped fallback hub
  ("Editor"). A socket hub reports the scope of its paired hub (WS §1.4).
* **Before core M5 lands**, everything is in scope 0, "Process" (IN-7).
* **Labels** come from `GetDebugStringForWorld` (F11) and are computed on the game thread from the scope's game
  instance. This is correct even before a net driver exists, because PIE sets the net mode early
  (`Runtime/Engine/Private/World.cpp:9612-9620`). When several PIE sessions are kept, the label is prefixed
  with the session number ("PIE 3 · Client 1"). It is frozen when the scope closes.
* **UI.** A scope selector, a Scope column, and a tint per PIE client.
* **PIE start** (`PreBeginPIE`, F11) clears closed PIE scopes unless *Preserve log* is on. The Editor scope is
  never cleared by PIE.
* **Console commands** in PIE default to the console world's scope (`-all` overrides).
* **Run under one process** puts all PIE instances in this store. Separate-process PIE clients use their own
  game window (§4.1) or export.

### 3.6 Stats

Aggregates are kept independently of the rings, so they survive eviction. `Clear` resets them.

* **Per scope × client × endpoint key.** The key is the endpoint name, else `VERB path-template`, else
  `VERB host` for absolute URLs without a template (review #11). Concrete paths never become keys. At most 256
  keys per client, then "other". Stats: count, errors by kind, retries, cache hits, bytes up/down, and duration
  p50/p95/max from a fixed log-bucket histogram (constant memory, about 5 % error).
* **Per socket**: attempts, reconnects, frames and bytes in/out, drops, average RTT.

---

## 4. The UI (`VaRestInspectorUI`)

### 4.1 Where

* **Editor.** A nomad tab `VaRestInspector`, "VaRest Network Inspector", in *Tools ▸ Debug* (F1, F2).
* **Desktop Debug/Development games**, including `-game` PIE clients (where `GIsEditor` is false).
  `VaRest.Inspector.Open` invokes the same tab, which opens in its own window, the way `WidgetReflector` does
  (F3). Only core-style brushes are used. The release checklist (§10.3) covers:
  * exclusive fullscreen focus loss;
  * a mouse cursor captured by the game;
  * closing and reopening the window.
* **Mobile, Test, Shipping**: no UI.

### 4.2 Layout

```
[Rec][Pause][Clear][Preserve log] Scope:[All v] [All|HTTP|WS|Socket.IO|Errors|Markers] [filter.......] [Export HAR...]
+------------------------- request list ---------------------------------+------ details (tabs) -----------+
| Status  Method Name              Initiator         Dur.   Size  Att Cache| Overview Request Response Timing|
| 200     GET    GameApi.Profile   WBP_Profile:Load  84 ms  1.2KB  1  hit  |                                 |
| 401→200 POST   GameApi.Buy       ShopComp.cpp:142  412 ms 320 B  2  —    |                                 |
| (queued) POST  GameApi.Score     BP_Match:Finish   —      —      3  —    |                                 |
| -- offline (connection failed on #57) --                                 |                                 |
| 101     WS     Chat              BP_Lobby:Begin    5m12s  ↓84KB  4       |                                 |
+--------------------------------------------------------------------------+---------------------------------+
 1 932 rows (184 shown) · ↓ 3.1 MiB ↑ 220 KiB · 12.4 MiB of 64 · 0 dropped · capturing
```

* The list and the details are split by an `SSplitter`.
* The status bar shows rows, total transferred, budget use, dropped events and capture state.
* The list follows new rows while it is scrolled to the bottom, and stops when the user scrolls up.

### 4.3 Request list

HTTP rows, socket rows and marker rows share one list, in start order. A socket row sits at its creation time,
and its cells update live (WS §12).

| Column | HTTP | Socket | Default |
| --- | --- | --- | --- |
| `#` | request id | socket id | hidden |
| Time | start | created | shown |
| Scope | label | label | shown when there is more than one scope |
| Status | final status; `401→200` when attempts differ; `(failed: Timeout)`, `(pending)`, `(queued)`, `(canceled)` | `Connected`, `Reconnecting (3)`, `Closed 1006`, `Gave up` | shown |
| Method | verb | `WS` / `SIO` | shown |
| Name | endpoint name, else path template, else path | socket name | shown |
| Initiator | Blueprint and function (`WBP_Profile:Load`), else `file:line`, else owner | same | shown |
| URL | redacted URL | redacted URL | hidden |
| Client | client name | entry | hidden |
| Duration | total | connected time | shown |
| Size | ↓ response bytes (↑ in tooltip) | ↓/↑ | shown |
| Attempts | `1`, `3`, `2·auth` | attempts | shown |
| Cache | `—`, `miss`, `hit`, `disk`, `304`, `stale`, `stored` | — | shown |
| Offline | `—`, `queued`, `queued→#88 ok/failed`, `replay of q-12`, `expired` | — | shown |
| Tag | tag | — | hidden |

* **Columns** can be hidden, reordered and resized (F5), and the layout is saved per user (§6.2).
* **Sorting** works on any column. The default is arrival order, and a sorted list does not follow new rows.
* **Row text** is formatted once per row revision, at the 10 Hz refresh, and cached in the view model. Paint
  never formats text.

### 4.4 Filters and search

There are kind chips and a text box. Terms are combined with AND, and `-` negates a term.

| Term | Matches |
| --- | --- |
| free text | substring (case-insensitive) of URL, name, initiator, client, tag |
| `id:57` | request or socket id (matches `#57` in log lines) |
| `status:404`, `status:4xx`, `status:>=500`, `status:0` | final status (`0` = no response) |
| `method:POST` | verb |
| `client:`, `endpoint:`, `initiator:`, `tag:`, `scope:"Client 1"` | those fields |
| `is:error`, `is:running`, `is:cached`, `is:queued`, `is:retried`, `is:auth`, `is:socket`, `is:sio` | flags |
| `larger-than:10k`, `slower-than:500ms` | size, duration |

Filtering runs in the view model on the game thread, debounced by 150 ms while typing. The filter text is saved
per user.

### 4.5 HTTP details

* **Overview**
  * URL, method, final status, outcome and error (`Describe Error`, core §3.3).
  * Client, endpoint, tag, scope, start, duration, size, flags.
  * The request id, with *Copy id*. The log carries the same `#57`.
  * Links: auth refresh, parked requests, queue item and replay. They come from the back-link table and select
    the linked row when clicked.
  * **Initiator.** In the editor, *Open in Blueprint* resolves the BP frame to its node
    (`FindSourceNodeForCodeLocation`) and focuses it (`BringKismetToFocusAttentionOnObject`). *Open source*
    opens the C++ call site (`FSourceCodeNavigation::OpenSourceFile`) (F12). Packaged builds show the function
    and offset as text.
* **Request**: query parameters, headers as dispatched by the selected attempt (a toggle shows them "as
  built"), body viewer.
* **Response**
  * Status, effective URL, headers, body viewer.
  * A **Decode** section: result type, decode time, warnings with their JSON paths ("+N more").
  * An **Envelope** section: data path, the success predicate with its actual value when it failed, and the
    extracted code and message. This answers "why is my struct empty / why `Kind=Server`".
* **Timing**
  * A waterfall: one lane per attempt, with the phases of §3.3. A gap before an attempt is drawn hatched
    (`Backoff`, `SessionRefresh`). Cache and offline notes appear as events.
  * Below it, the record's timeline.
* **Body viewer** (request and response)
  * **Pretty**: `FVaRestJson::Parse` + `ToString(Pretty)` on a background task, cached per excerpt. Highlighted
    by an `FSyntaxHighlighterTextLayoutMarshaller` subclass (F16) up to 64 KiB of text, plain beyond. Display is
    capped at 1 MiB / 20 000 lines.
  * A truncated excerpt is pretty-printed by `RedactPrefix` (IN-13) up to the cut, with "truncated at 64 KiB of
    2.1 MiB".
  * **Tree**: an `STreeView` over `FVaRestJson` handles, children enumerated lazily. Complete excerpts only.
    *Copy value* and *Copy path* are available.
  * **Raw**: as captured.
  * **Hex**: only when binary capture was opted in.
  * Redacted values show as `***` with "redacted at capture: the value never reached the Inspector".
  * *Save body…* writes the excerpt through the save dialog (F13).

### 4.6 Socket details

* **Frames**: a virtualised list of the selected socket's frames.
  * Columns: time, direction, kind, size, channel, route or event, id, protocol tag, attachments, lane, and an
    "auto-pong" badge.
  * Filters: direction, kind, `route:`, `ns:`, text over the route and id, and *hide control frames*.
  * It follows new frames, and the body viewer shows the selected frame.
  * Synthetic rows mark attempt boundaries, closes, dropped-send batches ("37 volatile sends dropped: not
    ready"), and request and ack ends ("ack 13 timed out after 10 s").
* **Connection**
  * State and status (WS §3.1), latency, buffered amount, flow control.
  * An **attempts** table: gap before, start, result, close fields, redacted handshake.
  * A **channels** table (Socket.IO namespaces): state, public id, recovered, last reason.

### 4.7 Socket.IO

A Socket.IO socket is a socket row with method `SIO`. On top of the socket details:

* The Frames pane gains a namespace column. Events show as `event(args…)`, with the args array as the excerpt.
* Each `EmitWithAck` frame is linked to its ACK and shows the latency.
* Binary attachments show as sizes, with no content unless binary capture is opted in.
* `eio:*` frames are hidden by default (SIO §10).

### 4.8 Stats tab

A tab beside the list, showing §3.6 as a sortable table: scope → client → endpoint key, and scope → socket.
*Copy table* copies it as TSV. The tab follows the scope selector, not the text filter.

### 4.9 Actions

| Action | Rules |
| --- | --- |
| Copy as cURL (bash) | from the redacted record (§5.2) |
| Copy URL / request body / response body / value / path / frame / id | redacted data. A truncated copy says so in a toast |
| Save body… | the excerpt, through the save dialog |
| Export HAR… | all rows, the filtered rows, or the selected rows (§5.1). Written on a background task. A toast reports completion and offers *Show in folder* |
| Clear, Pause/Record, Preserve log | §3.1, §3.5 |
| Settings | opens *Project Settings ▸ VaRest Inspector* in the editor |

### 4.10 Refresh model

* The view model listens to `OnChanged` and marks itself dirty. The panel runs one active timer (F16), at
  10 Hz, **only while it is dirty**.
* On each run it appends new matching rows, drops evicted rows from the front, and re-formats the cached row
  text of changed records. A full rebuild happens only when the filter or the sort changes.
* The list's item array holds `TSharedPtr` row models (F5).
* This keeps an idle panel free of per-frame work in a real editor. Under `-nullrhi` neither the timer nor
  the list runs at all (F10). §10 tests the view model directly instead.

---

## 5. Exports

### 5.1 HAR 1.2

The log has `log.version "1.2"` and `log.creator {name: "VaRest", version}`, and no `pages`. Entries are
sorted by `startedDateTime`.

**Entries**

* One entry per network attempt.
* Every logical request has at least one entry. A request without a network attempt gets a **synthetic
  entry**, with `_varest.synthetic: true` and zero timings:
  * a cache hit gets the cached status and `_fromCache: "memory"` or `"disk"`;
  * a request queued offline, or canceled before dispatch, gets status 0 and an `_error`.
* So a HAR file is a complete record of what happened. A gap phase lies between entries and belongs to none.

**`request`**

* Method and the redacted URL.
* `httpVersion: ""`, because the engine does not report it.
* `cookies: []` and the final redacted headers.
* `queryString`, and `postData {mimeType, text}` holding the redacted excerpt (`params` for forms). A binary
  body has no `postData`; `_varest.body` holds its descriptor.
* `headersSize: -1`, and `bodySize` set to the request bytes.

**`response`**

* `status`, which is 0 with an `_error` when there was no response (Chrome's convention).
* `statusText` from the RFC 9110 phrase table; it is synthesized, and the docs say so.
* The redacted headers.
* `content`:
  * `size` is the total bytes;
  * `mimeType`;
  * `text` holds the excerpt;
  * a binary body has a descriptor in `_varest`.
* `redirectURL`, `headersSize: -1`, `bodySize: -1`.

**`timings`**

* `blocked` = `Auth` + `Queued` + `Prepare`, all inside the attempt (§3.3).
* `dns`, `connect` and `ssl` are -1.
* `send`, `wait` and `receive` are each ≥ 0.
* `time` is the sum. The validator checks the sum and the wall-span invariant (R7).

**`_varest`** carries:

* request id, attempt and attempt count;
* scope, client, endpoint, path template and initiator;
* tag, outcome, error kind, the gap before the attempt, retry reason;
* cache and offline state, links;
* decode warnings and envelope outcome;
* the redacted header names, `bodyTruncated` and `bodyTotalBytes`.

**Sockets** (on by default) produce one entry per connection attempt, in Chrome's format:

* `_resourceType: "websocket"`;
* the upgrade request with redacted headers;
* status 101, or the failure status;
* `_webSocketMessages: [{type: "send"|"receive", time: <epoch seconds>, opcode: 1|2, data}]`. A binary frame
  without opt-in has `data: ""` and its descriptor in `_varest.messages[i]`;
* an attempt failure or close becomes a `type: "error"` message with `opcode: -1`.

`_varest` then adds the socket id, the close fields, and per-message channel, route, id, protocol tag and
attachments.

Chrome's import of these entries gets a manual check in the sockets milestone (§10.3).

### 5.2 cURL (bash)

```
# VaRest Inspector · #57, attempt 2 · *** = redacted at capture
curl 'https://api.game.io/v1/orders?lang=en' -X POST \
  -H 'Authorization: ***' -H 'Content-Type: application/json' -H 'Idempotency-Key: 6f1c…' \
  --data-raw '{"item":"sword","qty":1}'
```

Built from the selected attempt's final redacted headers, URL and body:

* headers that curl derives (`Content-Length` and the like) are dropped;
* single quotes are escaped as `'\''`;
* a binary body becomes `--data-binary @body.bin`, with a comment;
* a truncated body adds a comment line;
* the user fills in the `***` values by hand.

---

## 6. Console, settings, BP

### 6.1 Console (wherever `VAREST_WITH_INSPECTOR`)

| Command | Effect |
| --- | --- |
| `VaRest.Inspector.Capture 0/1` | §3.1 |
| `VaRest.Inspector.Clear` | clears the store and the stats |
| `VaRest.Inspector.List [N] [filter…]` | logs the last N rows (default 20), one line each, using the §4.4 grammar |
| `VaRest.Inspector.Show <id>` | logs one record with its headers, excerpts, phases, decode and envelope (all redacted) |
| `VaRest.Inspector.Stats` | logs the §3.6 tables |
| `VaRest.Inspector.Export [path]` | writes a HAR file; by default `Saved/VaRest/Inspector/<UTC>.har`. Logs the path |
| `VaRest.Inspector.Open` | UI module only |

### 6.2 Settings

`UVaRestInspectorSettings` (`config=Game, defaultconfig`) holds:

* `bCaptureInEditor` (true) and `bCaptureInPackaged` (false);
* `bCaptureHeaders` and `bCaptureBodies`;
* the §3.4 limits, as two groups: `Editor` and `Packaged`;
* `bHideEngineIoFrames` (true).

Clear-on-PIE is not a setting. *Preserve log* is the one knob for it, saved per user.

Redaction rules live in core settings (`UVaRestHttpSettings`, IN-13), because they also govern the logs.

Per-user UI state (columns, sort, filter, splitter, *Preserve log*) is saved to `GEditorPerProjectIni`
`[VaRest.Inspector.UI]`. The game window uses defaults and saves nothing.

### 6.3 Blueprint

`UVaRestInspectorLibrary` (category *VaRest|Inspector*) has:

* *Is Inspector Compiled In* (pure);
* *Set Inspector Capture*;
* *Is Inspector Capturing*;
* the async action **Export Inspector HAR** (`Path` = "" for the default, `bAllScopes`), with *On Done (Path)* and
  *On Failed (Message)*.

This is what QA overlays need on devices: a "record" toggle and a "save capture" button. Per-request query
structs are deferred (§13).

---

## 7. Threading

| Work | Thread |
| --- | --- |
| Trace events (all kinds except `Excerpt`), snapshot on attach | game thread |
| Transport timestamps (written into the exchange, not emitted) | HTTP thread |
| Response excerpt redaction (before the in-situ parse) | decode task |
| Request and raw-response excerpt redaction | core redaction `FPipe` |
| Inbound frame excerpts | the socket's pipe (inline in `SocketFrame`) |
| Outbound frame excerpts | core redaction `FPipe` (copied at hand-off) |
| Queue drain, records, budget, stats, `OnChanged`, scope labels | game thread, core ticker |
| View model, UI, console | game thread |
| Pretty print, HAR export | `UE::Tasks` over sealed records and game-thread copies |

Core's threading rule holds. The game thread never parses or redacts a body. At most it copies a request prefix
of up to the cap at `Send`, within the admission limit (IN-14).

---

## 8. Performance budget

| Item | Budget | Measured by |
| --- | --- | --- |
| Trace point with no sink, or paused | one relaxed atomic load + branch; zero events, redactor calls and delegate binds (counted) | R3, R12 |
| Capture per HTTP request, metadata + headers | ≤ 10 µs total on the game thread | dev-only cycle accumulators inside the trace points, reported as µs per request by `Bench.VaRest.Inspector.Overhead` (10 000 fake-transport requests) |
| Request excerpt copy at `Send` | ≤ cap bytes, one `memcpy` | same |
| Excerpt redaction (off thread) | ≤ 1 ms per 64 KiB, value scrub included | `Bench.VaRest.Inspector.Redact` |
| Capture per socket frame (metadata) | ≤ 2 µs on the game thread | `Bench.VaRest.Inspector.Frames` |
| Drain | ≤ 0.2 ms per frame at 200 items | `Bench.VaRest.Inspector.Drain` |
| Record memory without excerpts | ≤ 1.5 KiB per HTTP record, ≤ 96 B per frame | the store's accounting vs. heap delta |
| View model, 10 000 rows | full filter/sort rebuild ≤ 15 ms; append ≤ 1 ms | `Bench.VaRest.Inspector.ViewModel` |
| Body view, 64 KiB JSON | construct + highlight + `SlatePrepass` ≤ 30 ms | `Bench.VaRest.Inspector.BodyView` (works headless: no paint needed, F10) |

---

## 9. What the Inspector needs from core, WS and SIO (ruling R2: the minimal surface)

This table is the contract for the core M3/M4/M5, WS W5 and SIO S4 plans. It replaces the trace kinds of
core §4.8 and WS §12 and the trace paragraph of SIO §10. Thirteen event kinds in all, and **nothing is emitted
from the HTTP thread**.

Fields marked * are redacted (§2.2). Times are `IVaRestClock` seconds. Every event also carries `Kind`, `Seq`
and `Time`, which are taken only when the mask is non-zero.

### 9.1 Core events (`VaRestHttp`)

| # | Event | Fields | Thread | Lands in |
| --- | --- | --- | --- | --- |
| IN-1 | `Started` | `RequestId`, `ScopeId`, `ClientId`, `Verb`, `Url`*, `PathTemplate` (before substitution, no query), `Endpoint` (FName), `Tag`, `Priority`, `Flags` {Idempotent, QueueWhenOffline, NoAuth, AuthRefresh, Partial}, `QueueReplayOf` {QueueId, OriginalRequestId}, `Initiator` {OwnerPath, ScriptObject, ScriptFunction, ScriptCodeOffset, SourceFile, SourceLine}, `CachePolicy`, `CacheLookup` {None, Miss, HitMemory, HitDisk, Stale, Revalidate}, `bCacheLookupPending` (Started initial; snapshots current, never a predicted disk hit), `Headers`* (as built), `Body` {ContentType, Bytes, Kind}, `bExcerptPending`, `StartUtc` | GT | M3; cache fields M4 |
| IN-2 | `AttemptDispatched` | `RequestId`, `Attempt`, `AttemptStart`, `GapBefore` {None, Backoff, SessionRefresh; Seconds}, `AuthWait`, `AuthRefreshId`, `LimiterWait`, `Headers`* (final, after auth and custom per-attempt stages), `bConditional`. Each dispatch is paired with its own `AttemptEnded` while the record is open (§2.3; an end after `Completed` is never reported), which may arrive after a later attempt's `AttemptDispatched` (a stage answered the attempt while its call ran, and the call's end came later), and two dispatches under one attempt number (a PerAttempt stage that proceeds again) report two pairs with that number, of which only the live dispatch's end may announce an excerpt (IN-6) (M4a final review) | GT, immediately before `ProcessRequest` | M3; auth and gap fields M4 |
| IN-3 | `AttemptEnded` | `RequestId`, `Attempt`, `Status` (0 = none), `Headers`*, `EffectiveUrl`*, `ContentType`, `BytesUp`, `BytesDown`, `Failure` {None, Connection, Timeout, Canceled, Other} + `FailureDetail` (FName), `UploadDoneAt`, `StatusAt`, `TransportDoneAt` (plain doubles written into the exchange by the HTTP-thread delegates, which are bound only under IN-11), `bExcerptPending` | GT, when the outcome re-enters the stage chain | M3 |
| IN-4 | `Note` | `RequestId`, `Attempt` (the attempt the note is about: for `RetryScheduled` the one that ended, which may be one a park or the limiter ended unsent, with no attempt row (§3.3); pre-attempt CacheServedStale/Queued/QueueExpired use actual 0 with a real nonzero RequestId, without fabricated attempt rows), one of: `CacheRevalidated`, `CacheStored` {Bytes: synchronous memory retention only; no request Note for asynchronous disk success/refusal}, `CacheServedStale`, `RetryScheduled` {Reason (FName), Delay, RetryAfter}, `Queued` {QueueId, Reason}, `QueueExpired` {QueueId} | GT | M4 |
| IN-5 | `Completed` | `RequestId`, final `CacheLookup`, `bFromCache` (actual delivered response, independent of lookup history), `Outcome` {Success, Error, Queued, Canceled}, `CancelReason` {User, OwnerGone, Shutdown}, `Error` {Kind, HttpStatus, Code, Message*} (no body copy), `ResultType` (FName), `DecodeStartAt`, `DecodeEndAt`, `DecodeWarnings` (≤ 32 {Path, Message*}) + total count, `Envelope` {bApplied, DataPath, FailedPredicate {Path, Expected, Actual*}, Code, Message*}, `AuthRefreshId` (the refresh request the request's last park waited on, 0 = none; set also for a request a park ended before any dispatch: M4a final review C-I-1) | GT, before any callback is dispatched (including `.OnHttpThread()` callbacks) | M3; envelope M4; `AuthRefreshId` M4a |
| IN-6 | `Excerpt` | `Owner` {RequestId, Attempt, Request or Response} or {SocketId, FrameId}, `FVaRestTraceExcerpt` {Bytes (shared, immutable), Kind {Json, Text, Form, Multipart, Binary, Stream, Skipped}, TotalBytes, bTruncated, bRedacted, StopReason, Sha256Prefix, SkipReason {Backlog, Disabled (reserved (W5): core never emits it; M4a final review C-M-7), Discarded}}. `Discarded`: `AttemptEnded` announced the excerpt for the decode task and that attempt's response never reached it (a custom stage replaced the outcome, or a cancel finished the request first); it is emitted on the GT, once, right before the request's next `AttemptEnded` or its `Completed`, so every announced excerpt resolves while a sink wants Bodies (the promise of `bExcerptPending`: see §2.3 "Late data"; M4a Task 21 review M-1; Task 22 review) | decode task or core redaction pipe; `Discarded` on the GT | M4 |
| IN-7 | `Scope` | `Opened`/`Closed`, `ScopeId`, `PIEInstance`, `bEditorScope`, weak game instance (game thread use only) | GT | M5; M3 provides scope 0 "Process" |
| IN-8 | `Client` | `Created`/`Destroyed`, `ClientId`, `ScopeId`, `DisplayName` (asset path, `WithName`, or base URL) | GT | M3; asset names M5 |
| IN-9 | `Marker` | one of: `Online`, `Offline`, `SessionRefreshed`, `SessionReplaced`, `SessionCleared`, `SessionExpired`, `EnvironmentChanged`; `ScopeId`, `ClientId`, `CauseRequestId`, `Text` | GT | M4; environment M5 |

### 9.2 Core rules

| # | Rule | Lands in |
| --- | --- | --- |
| IN-10 | **Ids are always assigned at creation**: request, attempt, scope, client, and (WS) socket, connection attempt, frame. They are cheap counters. Only `Seq`, clock reads and payloads depend on the mask. | M3 |
| IN-11 | **Detail mask.** `IVaRestTraceSink::GetDetail()` returns {Metadata, Headers, Bodies, BodyCap, BinaryCap, FrameCap}; core keeps one atomic summary of all sinks. Every trace point tests it before building anything. The transport binds `OnStatusCodeReceived`/`OnRequestProgress64` only when `Metadata` is set at dispatch. Dev-test counters (events built, redactor calls, delegate binds) and dev-only cycle accumulators. | M3 |
| IN-12 | **Sink registry.** `AddSink`/`RemoveSink` run on the game thread. The sink list is an immutable `TSharedPtr<const TArray<TSharedRef<IVaRestTraceSink, ESPMode::ThreadSafe>>>` swapped on the game thread; emitters take it after the mask check. After `RemoveSink` returns, pipe emitters may still deliver, and sinks tolerate it. `AddSink` calls `OnAttached(const FVaRestTraceSnapshot&)`: scopes, clients, in-flight requests as `Started{Partial}`, and (W5) sockets with state, attempt, open-since and channels. Test-only `FVaRestTrace::FScopedSinkIsolation`. | M3; sockets in the snapshot W5 |
| IN-13 | **Redactor** (replaces core §4.5's lists; logs use it too): normalised key matching with the substring set and allowlist of §2.2, the exact `sig` and `key` and query `code`, the auth configuration's paths and names, session headers, the known-secret value scrub (copy-on-write set, previous values kept for 10 min), URL-field redaction (effective URL, `Location`, `Content-Location`, userinfo), JSON sniffing, form and multipart, binary as a descriptor. Project-wide additions in `UVaRestHttpSettings`. JSON goes through one new public VaRestJson function, `VaRest::Json::RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules&, EVaRestJsonFormat, TArray<uint8>& Out) → {bRedacted, StopReason}`, a SAX re-writer over the existing private reader with the §2.2 cut rules. | M3 (VaRestJson function in M3) |
| IN-14 | **Excerpts.** An ordinary request's prefix is copied at `Send` (up to the cap). Durable replay request-body previews are unavailable: suppress preparation before copying or announcement; Started remains immediate with bExcerptPending=false and no request Excerpt, including a recovered body containing a newly known three-byte secret. The full current+snapshot persistence guard still runs before wire. Ordinary request and response capture policies remain unchanged. A decoded response is redacted on the decode task before `ParseOwned` parses it in situ, with zero copies. An undecoded response (raw, `void`, bytes) has its prefix copied at the game-thread hop and redacted on the core `FPipe`. Raw copies waiting for redaction are bounded by an admission counter (the sink detail sets it; default 16 MiB); over it, `Excerpt{Skipped: Backlog}`. LLM tag `VaRest_Trace`. Compiled under `VAREST_WITH_TRACE_EXCERPTS` (§1.2). | M4 |
| IN-15 | **Initiator.** `Send()` takes `UE::FSourceLocation Where = UE::FSourceLocation::Current()` (F12). The owner's path is read when `.Owner()` was set. On the game thread, under `DO_BLUEPRINT_GUARD`, the top frame of `FBlueprintContextTracker::GetCurrentScriptStack()` (object, function, `Code − Script` offset) is read. All of this happens only when the mask is non-zero. | M3; BP nodes pass through in M5 |
| IN-16 | **Log correlation.** Every core log line about a request includes `#<RequestId>`. | M3 |
| IN-17 | **Core-owned goldens** `VaRest.Http.Trace.*`, which assert event sequences **and phase values**: success; retry with `Retry-After`; 401 → refresh → replay with two parked requests (`GapBefore`, `AuthWait`, `AuthRefreshId`); proactive refresh; memory/disk hit; 304; known-offline and attempted stale; offline queued + replay + expiry; delayed/refused disk persistence after Completed (no late Note, CacheStored only on synchronous memory retention); closed-Outer late handbacks; repeated Outer/read snapshots and cancel/early completion; cancel; decode error with warnings; envelope failure; **attach mid-flight** (one request in flight, one open socket (W5), one SIO namespace (S4); attach; finish → one coherent record each). | M4 (socket parts W5/S4) |

### 9.3 WebSocket (W5)

| # | Event or rule | Fields | Thread |
| --- | --- | --- | --- |
| IW-1 | `Socket` | `Created`/`Released`, `SocketId`, `ScopeId` (paired hub), `Name`, `Protocol` (FName `ws`/`sio`), `Url`*, `Entry` (FName), `Initiator` (IN-15) | GT |
| IW-2 | `SocketAttempt` | `SocketId`, `AttemptId`, then one of: `Connecting` {GapBefore {None, Backoff, Offline, Background, Auth; Seconds}, Url*, HandshakeHeaders*, Protocols}, `Open` {ConnectMs, Protocol}, `Closed` {`FVaRestTraceSocketClose`: Code, Reason*, bClean, By {Client, Server, Transport, Heartbeat, Timeout}, bWillReconnect, Delay, bWhileReadPaused, Failure (FName, e.g. `NoUpgradeResponse`), HttpStatus, bGaveUp, ErrorKind} | GT |
| IW-3 | `SocketFrame` | `SocketId`, `AttemptId`, `FrameId` (assigned at staging for outbound, at assembly for inbound), `Dir`, `Kind` {Text, Binary}, `Bytes`, `Channel`, `Route`, `MsgId`, `MsgKind` {Message, Request, Reply, ErrorReply, Control}, `ProtocolTag` (FName), `AttachmentSizes` (≤ 10), `bAutoReplied`; inbound `ReceivedAt` + the excerpt **inline** (redacted on the socket's pipe before the GT emits); outbound `CalledAt`, `Lane` {Bypass, Ready, Backlog}, `bExcerptPending` (the excerpt is taken at hand-off from the shared payload and redacted on core's pipe → IN-6 by `FrameId`; a frame dropped before hand-off costs nothing) | GT |
| IW-4 | `SocketNote` | `SocketId`, then one of: `SendDropped` {Count, Reason {Volatile, Overflow, Rejected, StalePin, SendLimit}} (aggregated per tick), `RequestEnded` {MsgId, Channel, Route, Outcome {Replied, ErrorReply, Timeout, Connection, Canceled}, Latency}, `Flow` {bPaused, PendingBytes}, `Backpressure` {bHigh}, `Channel` {Name, State {Up, Down, Refused}, PublicId, bRecovered, Reason (FName), Message*}, `Ping` {Rtt} | GT |
| IW-5 | Rules | All types are `VaRestHttp` types; WS maps its own types onto them (`FVaRestSocketClose` → `FVaRestTraceSocketClose`). Sockets contribute to the IN-12 snapshot. Auth-placement fields (`FirstMessage`) are added to the socket's redaction keys, and the token to the known-secret set (IN-13). | — |
| IW-6 | WS §12 amendment | WS §12's `SocketConnecting`, `SocketOpen`, `SocketClosed`, `SocketRetryScheduled` and `SocketGaveUp` become `SocketAttempt` (a retry is `Closed.bWillReconnect` + `Delay`; giving up is `Closed.bGaveUp`). `SocketPing` and `SocketChannel` become `SocketNote`. `SocketFrame` keeps its name with the fields above. No other WS §12 trace event remains. | — |

### 9.4 Socket.IO (S4)

| # | Need |
| --- | --- |
| IS-1 | `ProtocolTag` uses exactly these values: `eio:open`, `eio:close`, `eio:ping`, `eio:noop`, `eio:upgrade`, `sio:CONNECT`, `sio:DISCONNECT`, `sio:EVENT`, `sio:ACK`, `sio:CONNECT_ERROR`, `sio:BINARY_EVENT`, `sio:BINARY_ACK`. `Route` = event name; `Channel` = namespace; `MsgId` = ack id; `AttachmentSizes` set. |
| IS-2 | Redaction: CONNECT payload values, `pid` and the EIO OPEN `sid` become `***` in excerpts and in `SocketNote.Channel` (SIO D28). The CONNECT values join the known-secret set (IN-13). |
| IS-3 | Namespace lifecycle via `SocketNote.Channel` (`Reason` = the `EVaRestSioDisconnectReason` name); acks via `SocketNote.RequestEnded`. |

---

## 10. Verification

### 10.1 Automation, data layer (`VaRest.Inspector.*`, default suite, headless)

The tests run on:

* core's fake transport and virtual clock (core §7.1);
* WS's `FVaRestFakeSocketTransport` (WS §16.1), and the SIO codec over it;
* synchronous executors for the redaction pipe and the export task;
* `FScopedSinkIsolation` and explicit drains.

Suites:

* **`Capture`**: one coherent record per IN-17 sequence (phases, links, back-links, initiator, decode and
  envelope details); attach mid-flight; unknown-id events; Pause → Record.
* **`Redaction`**: canary tests. Unique canary values are planted in every secret position:
  * headers: `Authorization`, a session header, the `ApiKey` header and query names from the auth config;
  * URLs: query `token`, `X-Amz-Signature`, URL userinfo, a `Location` with OAuth `code`;
  * JSON: `password` nested in arrays, camelCase `refreshToken`/`accessToken`/`clientSecret` from a C++
    struct encoded with the default policy, `refresh_token` in a refresh response;
  * other bodies: a form field, a multipart text part;
  * echoes: the bearer token in a URL path segment, in a `text/plain` body and in an error message;
  * binary: a protobuf-encoded password (default settings: descriptor only);
  * sockets: the WS `FirstMessage` token, the SIO CONNECT payload, `pid`, the EIO `sid`.

  Every output is then scanned: store strings and excerpts, view-model row text and detail models, HAR, cURL,
  the clipboard fake, and the `List`/`Show`/`Stats` output. Cut cases: the cap falls inside a sensitive value,
  inside a sensitive key, and inside a number.
* **`Store`**: budget and count eviction; pinning and "details evicted"; frame caps; FIFO order; byte and count
  admission; overflow marker; late excerpt after eviction; excerpt timeout; sealing and snapshot; back-links;
  scopes; clear; PIE clear with and without *Preserve log*.
* **`Stats`**: aggregates, endpoint keys from templates (`/users/{id}` stays one key for 1000 ids),
  percentiles, survival across eviction.
* **`Export`**:
  * the HAR checker: required fields; `send`/`wait`/`receive` ≥ 0; `time` = the sum; phases equal to the wall
    span; ISO 8601; underscore custom fields;
  * entries per attempt; synthetic entries; socket entries.
* **`Curl`**: quoting, binary, truncation.
* **`ViewModel`**: filter grammar (every term of §4.4), kind chips, scope selector, sort, append with filter,
  eviction from the front, live-tail state, row text cache invalidation by revision, detail models (headers
  tables, phase bars, decode and envelope sections, Pretty text and the truncation banner, Tree node children),
  the Frames model (filters, synthetic rows, ack links, `eio:*` default), Stats model, and actions against the
  clipboard and dialog fakes.
* **`Socket`** / **`SocketIO`**: rows across reconnects, attempts, closes, frames with lanes and drops, outbound
  excerpts by `FrameId`, acks and timeouts, channels.

### 10.2 Widget tests: construction only (ruling R3)

Under `-nullrhi`, Slate skips painting, and with it ticking and active timers (F10). Rows are generated only in
`STableViewBase::Tick` (F5). Engine source does show a cvar, `Slate.SkipWidgetDrawingInHeadlessMode` (F10), that
re-enables window drawing. But it does not show that the automation run can create a native window for the panel
under `-nullrhi`. So it is not relied on, and widget tests stop at construction.

`VaRest.Inspector.UI.Construct` (editor context) builds `SVaRestInspector` over a 10 000-record store, the body
view with a 64 KiB JSON body, and the Frames pane. Each is followed by `SlatePrepass`, as the engine's own tests
do (F10). The test asserts no ensure or error, and that the tab spawner delegate returns an `SDockTab`. Widget
behaviour is the manual checklist of §10.3.

### 10.3 Manual release-pass checklist (VaRest-b4d)

* **Editor list**:
  * the Widget Reflector shows about 40 generated rows with 10 000 records (virtualisation);
  * the list follows new rows, stops when scrolled up, and resumes at the bottom;
  * sorting and column hiding work and persist;
  * the 10 Hz refresh does not run while idle (Widget Reflector: no active timer).
* **Details**: every tab, *Open in Blueprint* from a *Call Endpoint* row, *Open source* from a C++ row, copy
  actions, *Save body…*, *Export HAR…*.
* **PIE**: two clients and a listen server produce three scopes; *Preserve log* on and off.
* **Offline**: `VaRest.Http.ForceOffline 1` (core M4 debug cvar, non-Shipping) produces queued rows, the offline
  marker, and replay rows with their back-links after `0`.
* **Sockets**: Frames pane under 60 Hz traffic; Socket.IO acks; the `eio:*` toggle; Chrome DevTools imports the
  HAR and shows the WebSocket messages.
* **Game window** (packaged Linux Development, then Win64/Mac): `VaRest.Inspector.Open`; exclusive fullscreen
  focus loss; a mouse cursor captured by the game; close and reopen.

### 10.4 Restore-the-bug targets

Each is seen failing with the behaviour broken, then passing once restored:

1. **R1** Redaction at capture (raw header handed to sinks) → a canary is found.
2. **R2** Cut rule (write cut tokens) → the cap-boundary canary test fails.
3. **R3** Zero cost (build events, or bind delegates, unconditionally) → the counter test sees non-zero.
4. **R4** Byte budget (enforce counts only) → the budget test exceeds 64 MiB.
5. **R5** Pinning (evict in-flight) → the long-poll record vanishes, and its completion makes an orphan.
6. **R6** Late attach (skip `OnAttached`) → the attach-mid-flight test sees orphan rows.
7. **R7** HAR algebra (count `Auth` inside `Queued` as well) → the wall-span check fails.
8. **R8** Export snapshot (read live unsealed records) → the deferred-executor test sees an excerpt that arrived
   after the export call.
9. **R9** View-model append (append without applying the filter) → the filtered view shows a non-matching row.
10. **R10** Byte admission (count only) → the stalled-drain test exceeds `MaxPendingBytes`.
11. **R11** Normalised keys (exact matching) → the camelCase `refreshToken` canary is found.
12. **R12** Pause = unregister (pause as a flag) → the counter test sees events after the pause frame.
13. **R13** Value scrub (off) → the bearer token in a path segment is found.

### 10.5 Builds and runs

* **Linux targets**: the editor, plus monolithic Development and Shipping game targets.
* **Packaged Linux Development run**:
  * start with `-VaRestInspector` against `Tools/testserver/`;
  * run `VaRest.Inspector.Export`;
  * check the file with `Tools/validate_har.py` (Python 3 stdlib: the §10.1 HAR rules plus a canary scan).
* **Shipping** has no automation and no console input, so the stubs are checked from the host module at
  startup. The host keeps `bUseLoggingInShipping` (`/w/Unreal/VaRestHost/Source/VaRestHost.Target.cs`), and the
  check logs:
  * `IConsoleManager::Get().FindConsoleObject(TEXT("VaRest.Inspector.Capture")) == nullptr`;
  * `Is Inspector Compiled In` is false;
  * the export action fails with "compiled out".
* **Release pass**: Win64, Mac and an Android Development export run.

---

## 11. Milestones (ruling R5: the UI first)

| I | Content | Depends on | Exit |
| --- | --- | --- | --- |
| I1 | **HTTP capture.** `VaRestInspector` skeleton, macros, settings; sink, `OnAttached`, bounded queue (count + bytes), store (rings, budget, eviction, pinning, sealing, back-links), HTTP records with phases, initiator, decode/envelope, markers, stats; scope 0 | core **M3 + M4** (IN-1–IN-17) | `Capture`, `Redaction` (HTTP), `Store`, `Stats` green; R1–R6, R8, R10–R13 |
| I2 | **Editor UI over HTTP.** View model; tab, menu, PIE hooks; toolbar; list (columns, sort, filters, live tail); details (Overview with initiator navigation and id, Request, Response with Decode and Envelope, Timing); body viewer (Pretty, Tree, Raw); copy actions, cURL, *Save body…*; status bar | I1 | `ViewModel` and `UI.Construct` green; R9; `ViewModel` and `BodyView` benches within §8; the editor part of the §10.3 checklist run once |
| I3 | **WebSocket.** Socket records, attempts, frames, notes, snapshot; Frames and Connection panes; socket canaries | I2; **W5** (IW-1–IW-6) | `Socket` green; socket canaries |
| I4 | **Socket.IO.** Namespace column and channels table, ack links, `eio:*` toggle, SIO canaries | I3; **S4** (IS-1–IS-3) | `SocketIO` green |
| I5 | **Exports and extras.** HAR export (HTTP, synthetic, sockets); console; BP capture toggle and export action; Stats tab; hub scopes and labels; desktop game window | I2 (sockets in HAR need I3/I4); **core M5** (IN-7, asset names) | `Export`, `Curl` green; R7; packaged Linux Development run + `validate_har.py`; Shipping stub check; game-window checklist |
| I6 | Benchmarks (§8), docs (a README section: workflow, device capture, privacy and redaction rules), passport | I4, I5 | numbers in `docs/passport/` |

Each I gets its own plan in `docs/superpowers/plans/`, written when its dependencies have landed.

* Hub scopes may move into I2 if core M5 has landed by then.
* I3 and I4 may run in parallel with I5's HTTP-only parts.

---

## 12. Risks

* **The trace surface lands in other teams' milestones.** §9 is small (13 kinds, game-thread emission), but it
  is spread over core M3/M4/M5, W5 and S4. Mitigation:
  * IN-17 makes core own the sequences and phase values as golden tests;
  * IW-6 states the WS §12 amendment explicitly;
  * I1 starts only when M3 and M4 have landed.
* **Redaction completeness.** Normalised keys, auth-fed rules, the value scrub and binary descriptors cover the
  known classes (§2.2). What remains is an unknown secret in a path segment, in an unkeyed text body, or in
  opt-in binary. Exports are shared files. Mitigation:
  * canary tests for every class;
  * the HAR export dialog and the docs say "secrets are redacted by rules; review before sharing";
  * `bCaptureBodies` off is one setting.
* **The value scrub keeps secrets in memory.** It holds a second copy of live tokens inside the redactor.
  Mitigation: copy-on-write sets, cleared on logout, previous values expired after 10 minutes, never logged.
* **Widget behaviour is not automated** (F10). Mitigation:
  * the view model carries the logic and is tested headlessly;
  * the widgets are thin;
  * the §10.3 checklist runs in every release pass.
* **The desktop game window** relies on `TryInvokeTab` in a game (F3) with core-style brushes. Mitigation: the
  checklist. If a platform fails, the editor tab and HAR export are unaffected.
* **Timing precision.** There is no DNS/connect/TLS split (F8). Mitigation: honest phase names, `-1` in HAR,
  and VaRest's own phases (gap, auth, queued, decode, deliver), which explain most game-side delays.
* **Memory with chatty sockets on phones.** Mitigation: off by default in packaged builds, per-socket frame
  rings, a byte-bounded queue and admission, and benchmarks in I6.

---

## 13. Deferred to 2.x (ruling R4)

| Feature | Why deferred |
| --- | --- |
| Lossless capture JSON export **and import** (read-only stores, the device → editor round trip) | HAR with `_varest` already carries VaRest's semantics, and Chrome DevTools opens it. A format should be designed together with its importer, which also needs hardening (size and string caps, text-only rendering) |
| Replay | Unsafe without core support. 2.1 must: keep `NoAuth`, tag and priority; refuse foreign hosts; force `NoRetry` with no auth refresh; never report connectivity; confirm non-idempotent verbs; offer the same `Idempotency-Key` |
| Unreal Insights regions, counters and bookmarks | The editor always traces the `Default` preset (F9), so the sink would keep the mask non-zero. Trace counters are non-atomic. Region names need templates. It is also the least DevTools-like part. 2.1 notes: gate on `FTraceAuxiliary::IsConnected()`/`OnTraceStarted` (`Runtime/Core/Public/ProfilingDebugging/TraceAuxiliary.h:330`, `:393-399`), `TRACE_DECLARE_ATOMIC_INT_COUNTER` with absolute values, literal bookmark formats |
| `body:` search | Needs background search infrastructure. URL, status, method, id and initiator filters cover the common case |
| Socket session export | HAR already contains socket entries |
| BP per-request, frame, socket and stats query structs | A large reflected surface for a rare use. Console + HAR export cover devices |
| Namespaces tab | The namespace column and the channels table cover it |
| Per-limit cvars | Settings suffice. `VaRest.Inspector.Capture` stays |
| PowerShell cURL | bash first; cheap to add |
| Header search, a waterfall column in the list | nice to have |
| Live remote capture (devices, separate processes) | needs a live transport. The 2.0 workflow is the desktop game window or HAR export |
| A mobile live UI | touch UX and screen size are a project of their own |
| Hex capture of binary by default | not redactable. Opt-in only |

---

## 14. Decisions made without the owner

`R` = controller ruling (binding); the others are this spec's.

| # | Decision | Why | Cost if wrong |
| --- | --- | --- | --- |
| R1 | **Late-attaching sinks get a snapshot** (scopes, clients, sockets, in-flight requests). **Redaction defaults** use normalised, case- and separator-insensitive keys with a substring set (covering camelCase output, presigned URLs), auth-fed rules, a known-secret value scrub, and binary as a descriptor. All Important review fixes are applied | records stay coherent after Pause → Record and after late module load; VaRest's own default camelCase output was leaking | a broad substring set over-redacts harmless keys (`sessionCount`), which the allowlist corrects; the value scrub keeps a second copy of tokens in memory |
| R2 | **Minimal trace surface**: 13 kinds, nothing emitted from the HTTP thread, phases from explicit durations (§9) | fewer cross-thread races and allocations; a tight contract for the M3/M4/W5/S4 plans | a phase VaRest later wants (e.g. a DNS split from a future engine API) needs a new field |
| R3 | **UI tests are honest about `-nullrhi`**: the data layer is tested headlessly; widget tests stop at construction + `SlatePrepass`; widget behaviour is a manual checklist | Slate neither paints nor ticks headless (F10) | widget regressions are found in the release pass, not by CI |
| R4 | **2.0 scope cut** per §13 | ship the DevTools core with the rest of 2.0 still ahead | device captures are seen in Chrome until 2.1; no replay, no Insights regions in 2.0 |
| R5 | **Milestone order**: HTTP capture → editor UI → WebSocket → Socket.IO → exports and extras → benches and docs | the visible product lands first, right after the trace surface | HAR export and the game window arrive last |
| R6 | **Initiator** (owner, BP frame → node, C++ call site), **decode and envelope details**, and a **request id shared with log lines** | "who called this", "why is my struct empty" and "which log line is this row" are the first DevTools questions | a few pointer reads per request while capturing; `Send()` gains a defaulted `FSourceLocation` parameter |
| D1 | Two modules: `VaRestInspector` (Runtime, stubs in Shipping) + `VaRestInspectorUI` (DeveloperTool; editor parts guarded by `GIsEditor`) | the BP toggle and export must cook for Shipping; the UI never ships; Development games get a window (F3, F4) | one module boundary to move |
| D2 | `#ifndef` defaults: `VAREST_WITH_INSPECTOR` and `VAREST_WITH_TRACE_EXCERPTS` = `!UE_BUILD_SHIPPING` | the override needs no macro redefinition; QA uses Test builds | Test binaries carry the capture code |
| D3 | Capture on in the editor, off in unattended runs and packaged builds | the data exists when the tab is opened after a bug; CI and devices pay nothing | the editor pays a few µs per request |
| D4 | Zero cost = atomic mask + conditional delegate binding + unregister on pause + snapshot sink list; proven by counters under `FScopedSinkIsolation` | exact and testable | none known |
| D5 | Redaction only in core, at capture; one rule set for logs, trace and exports | consistency | a core rule gap leaks everywhere (§12) |
| D6 | Response excerpts on the decode task before the in-situ parse (zero copy); request prefix copied at `Send`; others on a core pipe; byte admission | the threading rule and bounded memory | an excerpt appears a few ms after its row; under a backlog it is skipped |
| D7 | Limits per §3.4 (binary 0) | bounded on phones; typical API bodies fit | big-payload users raise settings |
| D8 | One global eviction FIFO; in-flight records pinned and in the budget, with evictable details | fair, simple; long polls survive | a chatty socket pushes out HTTP history (per-socket cap limits it) |
| D9 | Game-thread store fed by a `TMpscQueue`, bounded by count and bytes | lock-free capture; a single-threaded store | a stalled game thread drops events (visible marker) |
| D10 | Scope = hub; labels from the world; PIE clears closed scopes unless *Preserve log* (the only knob) | PIE clients never mix | none significant |
| D11 | One list for HTTP, socket and marker rows; frames in the details | WS §12; Chrome's layout | socket-heavy sessions lean on the kind chips |
| D12 | Body viewer Pretty (highlighted ≤ 64 KiB) / Tree / Raw / opt-in Hex; display caps; cached row text | Slate text layout cost | huge bodies are read in Tree or after *Save body…* |
| D13 | Filter grammar of §4.4 with `id:` and `initiator:` | familiar; log correlation | grammar additions later are additive |
| D14 | HAR: one entry per attempt + synthetic entries; sockets as Chrome `_webSocketMessages`; `_varest`; `blocked` = in-attempt auth + limiter + prepare | complete and Chrome-readable | logical gaps are visible only through `_varest` |
| D15 | Endpoint key = name, else `VERB template`, else `VERB host` | bounded stats; no concrete paths as keys | template-less dynamic URLs collapse per host |
| D16 | Stats from the Inspector's own aggregator, log-bucket percentiles | survives eviction; constant memory | ±5 % percentile error |
| D17 | BP surface = compiled-in check, capture toggle, HAR export action | QA overlays on devices | richer in-game debug UIs wait for 2.x |
| D18 | Timing phases only from VaRest's own durations | the engine has no finer data (F8) | users expecting DNS/TLS splits |
| D19 | `eio:*` hidden by default; auto-pongs as a badge | SIO §10; noise | a toggle away |
| D20 | Per-user UI state in the editor ini only | there is no editor ini in games | game-window layouts are not remembered |
| D21 | The value scrub keeps the previous 4 secret values for 10 minutes after rotation | a token echoed in a response just after a refresh is still caught | an older token echoed later is not scrubbed |

---

## Appendix A. Review disposition

Review: `.superpowers/sdd/inspector-spec-review.md`.

| # | Sev | Disposition |
| --- | --- | --- |
| 1 | Critical | **Applied (R1)**: ids always assigned (IN-10); `OnAttached` snapshot (IN-12, IW-5); unknown-id → "(started before capture)" (§2.3); attach-mid-flight golden (IN-17); restore-the-bug R6. |
| 2 | Critical | **Applied (R1)**: normalised keys with substring set and allowlist, auth-fed rules, known-secret value scrub, binary descriptor by default, presigned keys and URL fields (§2.2, IN-13); canaries extended (§10.1); R11, R13. |
| 3 | Important | **Applied (R3)**: F10 corrected; view model split and tested headlessly; widget tests limited to construction + `SlatePrepass`; manual checklist (§10.2–§10.3); R9 replaced by a view-model target. |
| 4 | Important | **Applied**: Insights deferred to 2.x (§13) with the reviewer's 2.1 notes; `FScopedSinkIsolation` (IN-12); unattended default off (§1.2). |
| 5 | Important | **Applied (R2)**: 13 kinds, game-thread emission, transport timestamps carried in `AttemptEnded`, refresh as a flagged request, goldens resized (§9). |
| 6 | Important | **Applied**: conditional delegate binding (IN-11), snapshot sink list and the after-`RemoveSink` rule (IN-12), byte/count counters (§3.2), R12 window defined, delegate binds counted (R3). |
| 7 | Important | **Applied**: byte-bounded queue and core admission counter, "excerpt skipped" (§3.2, IN-14); zero-copy response excerpts on the decode task; R10 asserts bytes. |
| 8 | Important | **Applied**: phases from explicit durations, gaps attributed to the next attempt, `blocked` = auth + limiter + prepare, wall-span invariant (§3.3, §5.1, IN-2, IN-17, R7). |
| 9 | Important | **Applied**: `FVaRestTraceSocketClose` and enums/FNames in `VaRestHttp`; `FrameId`; outbound excerpt at hand-off; stale-pin and send-limit reasons; explicit WS §12 amendment (IW-1–IW-6). |
| 10 | Important | **Deferred (R4)**: Replay moved to 2.x with the reviewer's safety list (§13). |
| 11 | Important | **Applied**: `PathTemplate` in `Started`; endpoint key rule (§3.6, D15); Insights region names moot (deferred). |
| 12 | Important | **Applied (R4)**: the keep/defer split as §13; the ruling's minimum kept (capture + redaction, editor tab with HTTP/WS/SIO, filters, detail panes, cURL, HAR, stats). |
| 13 | Important | **Applied (R5)**: §11 reordered. |
| 14 | Important | **Applied (R6)**: initiator (IN-15, §4.3, §4.5), decode and envelope details (IN-5, §4.5), `#<RequestId>` in logs and `id:` filter (IN-16, §4.4). |
| 15 | Minor | **Applied**: F3 reworded; game-window checklist items (§10.3); `GIsEditor` guards (§1.1). |
| 16 | Minor | **Applied**: `#ifndef` macros, `VAREST_WITH_TRACE_EXCERPTS` (§1.2, IN-14). |
| 17 | Minor | **Applied**: back-link side table (§2.3). |
| 18 | Minor | **Applied**: `bExcerptPending`, timeout on `IVaRestClock`, "excerpt timed out" (§2.3). |
| 19 | Minor | **Applied**: `VaRest_Trace` LLM tag in core; budget checked against the heap delta (§3.4, §8). |
| 20 | Minor | **Applied**: cached row text (§4.3); highlight cap 64 KiB; `BodyView` bench (§8). |
| 21 | Minor | **Applied**: `bClearOnPie` removed; *Preserve log* is the knob (§6.2). |
| 22 | Minor | **Applied**: Shipping check via `FindConsoleObject` from the host module (§10.5). |
| 23 | Minor | **Applied**: dev-only cycle accumulators (IN-11, §8). |
| 24 | Minor | **Applied**: pinned records in the budget, evictable details of undispatched records (§3.4). |
| 25 | Minor | **Applied**: `TMpscQueue` throughout (F14); "SIO D28"; `FVaRestTraceExcerpt` in `VaRestHttp`; `ExportHar` result type; capture JSON placeholder gone (deferred); Chrome import check (§10.3). |
| 26 | Minor | **Partly applied**: status-bar totals and *Save body…* (§4.2, §4.5). Header search and waterfall column deferred (§13). `VaRest.Http.ForceOffline`: **accepted by controller ruling for the M4 plan (non-Shipping only)**. It is a core cvar, not an Inspector requirement, and the §10.3 checklist uses it to exercise offline rows. |
| 27 | Minor | **Deferred with import** (§13): size cap, string caps, text-only rendering recorded as 2.1 requirements. |
