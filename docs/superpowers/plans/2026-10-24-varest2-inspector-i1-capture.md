# VaRest 2 — Inspector I1 HTTP capture Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task by task under the controller's dispatches. Unchecked steps are execution instructions; Beads owns status. No implementation, build, runtime, commit or source mutation is authorized by the planning pass itself.

**Goal:** Deliver milestone I1: a bounded, redacted, process-local HTTP capture service with coherent late attachment, immutable snapshots, HTTP phases and links, and statistics that survive history eviction.

**Architecture:** A runtime `VaRestInspector` module consumes the existing `VaRestHttp` trace registry. Its sink admits immutable capture items before copying borrowed event data; one game-thread ticker applies those items to a bounded store. Core remains the only redactor and owner of request/dispatch identity; narrowly specified additive trace fields close the verified response and dispatch gaps before the consumer lands.

**Tech Stack:** Unreal Engine 5.8.1, C++20, `TMpscQueue`, `TRingBuffer`, `FTSTicker`, `IVaRestClock`, the existing fake transport and virtual clock, UE automation. No new third-party library, external service or Python fixture.

**Source pin:** `e7a1bd3fc029cf003e029bd601984a4c7c6d1bee`, the accepted whole-M4/M4b source. All producer and existing test observations below were read with `git show` at that pin. `/w/Unreal/VaRest` is the actual checkout; `/w/Unreal/VaRest-work-d05aec99` is not the implementation checkout. Reconcile intervening accepted changes before execution; do not overwrite concurrent WebSocket work.

**Binding specification:** `docs/superpowers/specs/2026-10-20-varest2-inspector-design.md`, complete revision 1, especially §§1–3, 6.2, 7, 9.1–9.2, 10.1, 10.4 and milestone I1 in §11. The core threading, frozen names and verification contract are in `docs/superpowers/specs/2026-10-18-varest2-core-design.md` §§1, 4.8, 7. `docs/dev/cache-offline.md` owns the accepted M4b consumer semantics. Controller decisions and execution ownership are in `.superpowers/sdd/release-2.0-program.md` and this milestone's ignored workspace.

## Global Constraints

- Copyright every new C++/C# file: `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.` Preserve MIT licensing and the Win64/Mac/Linux/Android/IOS runtime allowlist. LinuxArm64 remains deferred.
- Control plane on the game thread; data plane off it. `OnEvent` can run on a worker only for `Excerpt`. It must not wait for the game thread, touch a UObject, mutate the store, broadcast a delegate, parse a body, redact, perform IO or call back into application code.
- Core is the only source of redaction. Capture only redacted trace fields and shared immutable excerpt bytes. The Inspector must not retain `FVaRestResponse`, `FVaRestError::Body/RawBody`, raw requests, exchange pointers, auth providers or credential sets.
- Keep the thirteen trace kinds and their names. The current payload variant contains nine HTTP alternatives; socket kinds have no payload alternatives yet. Ignore unknown kinds and `Source==Socket` before attempting `TVariant::Get`. Never invent W5 payloads to make I1 compile.
- Preserve `RequestId==FVaRestRequestHandle::GetId()`, actual `Attempt==0` notes, queue GUID strings, current `CacheLookup` versus delivered `bFromCache`, and `Completed.AuthRefreshId` for unsent auth parks. No fabricated network attempts, no late metadata after `Completed`, no asynchronous-disk `CacheStored`.
- Durable replay request-body previews remain suppressed before copying or announcement. Preserve ordinary request/response excerpt policy, core admission and its current/snapshot/applied-secret union. Do not weaken redaction to fit a capture cap.
- Keep v1 paths, reflected names and signatures in `Source/VaRest` and `Source/VaRestEditor` untouched. No HTTP dependency on Inspector and no Inspector dependency on WebSockets, Socket.IO, Slate, editor modules, HAR or the host source module.
- UHT declarations are unconditional. Only native implementation bodies and native capture/store code are controlled by `VAREST_WITH_INSPECTOR`. Its header default uses `#ifndef`, as does the existing core `VAREST_WITH_TRACE_EXCERPTS`. Do not override either globally as a permanent build setting.
- Capture defaults on only in an interactive editor, off in unattended runs and packaged applications. An explicit `-VaRestInspector` or `-VaRestInspector=0` overrides that default. Detail-policy changes are adopted through an explicit capture restart; capacity changes require explicit Clear/recreation at account quiescence as specified in Task11. Existing limits remain active and changed limits visibly pending while old readers retain the account.
- One store and one capture service per process; scope 0 is `Process`. There is no GameInstance isolation or PIE labeling/clear policy in I1.
- Admit count and bytes before acquiring ownership, copying strings, allocating DTO payloads or retaining excerpts. Account for real container capacities and overlapping queue/store/snapshot ownership. No exact RSS, allocator-fragmentation or LLM-total promise; core event/snapshot construction predates the consumer and is a separate upstream transient.
- Existing in-flight rows that were admitted remain pinned. Admission failure is visible and must not become an unbounded side map. Optional detail, metadata, timelines, tombstones, links, client tables and statistics all have finite limits.
- All public native operations, store mutation, `OnChanged` and snapshot creation are GT-only. User observers run only after a consistent batch and outside queue/accounting locks. Reentrant Clear/SetCapture requests are deferred/coalesced until the current notification/enumeration returns; shutdown wins.
- No callback captures a raw module, store or facade pointer. Old generation sinks own only their closed queue and shared accounting. A late registry-list callback may run and destroy its sink on a worker after removal. `RemoveSink` is not quiescence.
- Use `SupportsDynamicReloading()==false`, idempotent PreExit/Shutdown and normal engine-shutdown code lifetime. Arbitrary forced direct DLL unload while foreign code retains sink references is outside the supported contract; no general quiescence claim is made.
- Source timestamps belong to the emitting client's clock. Use them only for that logical request's phases. Excerpt timeout is five seconds on the Inspector's injected `IVaRestClock` after it observes completion; never subtract a client's virtual timestamp from a different clock.
- Preserve unknown history: snapshot `StartedAt` is usable; `StartUtc`, owner/script/source and historical headers can be absent when the request began without capture. Do not reconstruct them from the present clock or inspect a live caller after the fact.
- No file-level `using namespace`; direct includes, generated header last, no MSVC member/name shadowing. Gate dereferences/indexes after nonfatal automation assertions. Test-owned barriers release on every exit before cancellation/drain.
- Build and runtime are sequential. Every changed C++ RED, GREEN, mutation and restoration version requires a fresh normal full action graph and inspection before native execution. Tests run with `--no-build`. No engine/host compilation, new PCH creation, global unity flags, Module/UsePrecompiled isolation claims, generated-file changes, cache/timestamp/object manipulation or historical graph waivers.
- Editor/NoPCH graphs permit only plugin Compile/Link and explicitly inspected receipt metadata. Monolithic gates additionally permit the exact named final VaRestHost executable link and its inspected products, not other host/engine work.
- The controller owns source activation, exact temporary Build.cs scope, model dispatch, index, commit, Beads, shared memory and acceptance. At most five task fix rounds; one complete final review and one final fix wave. No remote operation is part of I1.

## Review Focus

1. **Borrowed oversized event during a stalled drain:** refusal precedes every owned copy/reference; count and byte peaks include reserved writers, queued items, nodes and consumer handoff. A marker does not recursively overflow the queue.
2. **Two physical dispatches with the same attempt number:** ends in both orders and delayed excerpts attach by dispatch identity. An unsent auth/limiter attempt still has no invented row; a terminal without an end is explicitly incomplete.
3. **Cache hit, stale fallback and 304:** final delivered metadata differs from physical wire metadata. Cached entity size is not wire BytesDown; a missing delivered-body preview is visibly unavailable.
4. **Pause/Clear/resume while a worker holds an old registry snapshot:** old items cannot enter the new store, queues cannot outlive their accounting, no raw module callback survives, and no request result is fabricated to close a paused row.
5. **All rows pinned or an observer retaining snapshots:** budgets remain finite, accepted rows survive, new rows/details may be refused visibly, and Clear does not reset credits still held by old owners.
6. **Late excerpt, eviction and immutable snapshots:** completion, pending promises, timeout, retention tombstones and backlinks agree; no mutation of a previously published sealed row, no resurrection by a late excerpt.
7. **Canaries in every captured string:** positive capture contains ordinary data and `***`, while default camelCase keys, cap cuts, auth/session custom names, presigned URLs, URL fields, value echoes and binary descriptors contain no canary.
8. **Source-off and Shipping paths:** no sink/trace work after pause, positive on-control proves counters work, reflected settings still compile, and no UI/export/BP future functionality masks a missing dependency.

## Scope and milestone boundaries

I1 owns the runtime module, native facade, settings, sink/queue, store, immutable snapshots, HTTP records and phases, initiator/decode/envelope retention, links, markers, stats and process scope. The completion snapshot seam is the I1 part of R8; an exporter is not required to prove that a deferred reader sees call-time data.

I2 owns `VaRestInspectorUI`, view model, editor tab/menu, filters, sorting, row text, details navigation, body rendering, clipboard/cURL/Save body and PIE hooks. I3 owns W5 socket records/attempts/frames; I4 owns S4 Socket.IO details. I5 owns HAR and its validator, console commands including the capture cvar, Blueprint library/export action, Stats tab, M5 Hub scopes/labels and desktop game window. I6 owns performance benches, passport and release documentation. The spec's I2 cURL action and I5 Curl exit are reconciled as UI action preparation in I2 and full export validation in I5; neither is I1 work.

`UVaRestInspectorSettings` includes the specified future frame limits and `bHideEngineIoFrames` as inert configuration fields, labeled as used by I3/I4. There is no future module or fake socket implementation. `IVaRestInspector` introduces only the native I1 control/read subset. `ExportHar`, `UVaRestInspectorLibrary`, `UVaRestInspectorExportAction` and their Shipping checks arrive in I5; do not add failing public export stubs prematurely.

## Pinned producer/consumer reconciliation

All paths in this table are relative to the repository and to source pin `e7a1bd3`. These are source observations, not permission to edit every cited file.

| Requirement | Actual producer/interface | I1 consumer and test responsibility |
| --- | --- | --- |
| IN-1 identity/start | `Source/VaRestHttp/Public/VaRestTrace.h`: `FVaRestTraceStarted`; `Private/VaRestTracePoints.cpp:37` `Describe`, `:137` Started | Preserve actual RequestId, ScopeId0, ClientId, flags, QueueReplayOf, initiator, cache policy/history/pending, body descriptor and source start times. Endpoint is currently `NAME_None`; M5 supplies it. |
| IN-2 physical start | `FVaRestTraceAttemptDispatched`: AttemptStart, GapKind/GapSeconds, AuthWait/AuthRefreshId, LimiterWait, final Headers, bConditional; `Private/VaRestPipeline.cpp:568` creates a private dispatch tag | Retain distinct physical dispatches, at most 16 detail rows; additive dispatch id prerequisite below. Do not key solely by Attempt. |
| IN-3 physical end | `TracePoints::AttemptEnded(FExchangeState&, const FVaRestTransportResult&, const FDispatchTag&, bool)` uses the tag's attempt and probe; `Private/VaRestTracePoints.cpp:209` | Preserve status/failure/effective URL/content type/wire counts and the three source timestamps. Stale ends may arrive after a later dispatch, never after logical Completed. |
| IN-4 notes | `FVaRestTraceNote`, six kinds; `TracePoints::Note` stamps real exchange RequestId and Attempt | Retain actual 0 on pre-attempt cache/stale/queued/expiry notes. CacheStored means synchronous retained memory bytes only. |
| IN-5 completion | `FVaRestTraceCompleted`: outcome/cancel/error summary, result type, warning list≤32 plus total, envelope, cache lookup/provenance, AuthRefreshId | Completion is once before user callbacks. Add final delivered metadata from `FFinish` as specified below; do not recover it from last wire status or Error.HttpStatus. |
| IN-6 excerpt | `FVaRestTraceExcerptEvent` owns immutable `TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>`; Request/Response/Frame owner; Skipped None/Backlog/Disabled/Discarded | Ignore Frames in I1. Bodies-disabled sinks ignore all excerpts and promises even if another sink requested them. Retain allocated capacity charge; no byte copy. Add response dispatch id. |
| IN-7 scope | `FVaRestTrace::ProcessScopeId==0`; BuildSnapshot supplies `Label="Process"` | Retain scope0; no game-instance dereference, no synthetic PIE scope. |
| IN-8 clients | `FVaRestTraceClient`, Created/Destroyed boolean, ClientId/ScopeId/DisplayName; name scrubbed or URL-redacted | Bound client metadata retention; historical records retain their own bounded display context. Asset names remain M5. |
| IN-9 markers | Online, Offline, SessionRefreshed/Replaced/Cleared/Expired, EnvironmentChanged enum | Store real marker scope/client/cause/text. EnvironmentChanged has no M4 producer; no synthetic environment event. CaptureOverflow is an Inspector marker kind, not a new core kind. |
| IN-10 ids | `NewRequestId()`/`NewClientId()` are always nonzero; attempt numbering belongs to core | No new HTTP id allocator. QueueId survives process runs; OriginalRequestId==0 across runs prevents false numeric links. |
| IN-11 detail | `GetDetail()` returns `FVaRestTraceDetailSettings`; `FVaRestTrace::Detail()` relaxed atomic; `GetDetailSettings()` merges caps | Metadata always requested while capturing; Headers/Bodies follow settings. `MaxPendingExcerptBytes` is a separate source raw-copy allowance. |
| IN-12 registry | `AddSink(const FSinkRef&)`, `RemoveSink(const FSinkRef&)`, `RefreshDetail()` GT-only; `OnAttached(const FVaRestTraceSnapshot&)` before publication; `OnEvent(const FVaRestTraceEvent&)` borrowed const | Never move from Event. Flatten snapshot entries individually, no second whole-snapshot copy. Immutable registry lists may call removed sink later. |
| IN-13 redaction | Existing FVaRestRedactor and VaRestJson RedactPrefix, current/snapshot/applied union | Inspect every public record string/excerpt in consumer tests. No new redactor, regex scrub or raw fallback. |
| IN-14 excerpt lifecycle | `Private/VaRestTraceExcerpts.h/.cpp`: source admission, request prefix at Send, raw response pipe, decoded response before parse, Backlog/Discarded, runtime Bodies recheck | Discarded resolves its exact promise. Disabled is reserved and not emitted by core. Binary over cap has no hash when the full payload was not seen. Replay request preview remains unavailable. |
| IN-15 initiator | `TVaRestRequest::Send(UE::FSourceLocation)`; Started reads owner/top BP frame only under trace mask | Retain data; no source/BP navigation or new stack walk. Snapshot after trace-off may have no historical initiator. |
| IN-16 logs | Request log correlation already owns `#<RequestId>` | Record id remains identical; no new logging on hot trace path. |
| IN-17 goldens | `Private/Tests/VaRestHttpTraceTest.cpp`, `VaRestHttpTraceGoldenTest.cpp`, `VaRestHttpAuthTest.cpp:4868`, `VaRestHttpTraceCacheOfflineTest.cpp`, `VaRestHttpExcerptTest.cpp:1212`, `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp:134` | Keep producer-owned golden assertions and add consumer assertions against real producer scenarios. Hand-auth and persistence hooks remain in their owning tests, not exposed as production APIs. |
| Testing | `VaRest::Http::Testing::FTestRig`, `Observe`, `MakeOutcome`, `PumpUntil`, `FlushRedactionPipe`, `FScopedPersistenceTest`, `FTraceRecorder`, `FScopedTraceRecorder` | Include `VaRestHttpTesting.h`; a record suite must not accidentally isolate out its own Inspector sink. Cancel held work before full drain. |
| Core counters | `FVaRestTrace::Counters()`; EventsBuilt/RedactorCalls/DelegateBinds/TraceCycles; testing TraceExcerptBytesCopied | Use isolated baseline deltas and on-control; no timing-based zero-cost claim. Inspector stats are a separate aggregator, not core's private FHttpStats. |

Engine mechanisms inspected for this plan: `/w/Unreal/UnrealEngine/Engine/Source/Runtime/Core/Public/Containers/MpscQueue.h:13` is unbounded and allocates a node per Enqueue; `:54–55` exchanges Head before linking Prev->Next and `:63–68` can return temporarily empty. The Inspector supplies admission around two independent lane instances. Its dynamic test holds at the Inspector boundary immediately before Enqueue, because the engine primitive has no internal exchange/link test hook; independence during that exact internal gap is a source-analysis argument, not a claimed injected execution. `Containers/RingBuffer.h:490` PopFront destroys front units; a ring does not impose a cap by itself. `Containers/Ticker.h:35` exposes FTSTicker and its remove contract. `Modules/ModuleInterface.h:53` and `:90` define startup/shutdown and dynamic reloading. `Runtime/Core/Private/Modules/ModuleManager.cpp:1364` retains DLL code at normal engine shutdown; forced unload is a different path. `Runtime/DeveloperSettings/Public/Engine/DeveloperSettings.h:23` provides auto-discovered settings. No engine edit is planned.

## Per-version build and proof protocol

These are separate, sequential operations with an inspection gate between the first and second command. Use a fresh unique evidence filename for every source version, target and configuration; the name below is an example, not a reusable approval.

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/tmp/varest-i1-task-version-editor-actions.json
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
Tools/run_tests.sh --no-build VaRest.Inspector
```

Inspect every action and recursive response file, all unity members and source paths, object/PCH inputs, output paths, link scripts/inputs and receipt products. Host-located plugin UHT/unity products are classified by actual module ownership. An existing engine SharedPCH may be an input; creation of a new one is forbidden. UHT discovery can affect freshness even in the graph step. Stop on a prohibited action and return the exact graph/action to the controller; do not repair engine caches.

For a behavioral RED, add a compiling test against a small valid baseline, or use an exact controller-approved original-bug mutation where the API already exists. An expected compiler error can prove a missing API during initial scaffolding, but it does not replace behavioral RED/restoration evidence. Each critical test below identifies its discriminating failure and positive control. Keep the test unchanged between mutant and restoration. Restore exact original bytes even on failure, then generate and inspect a fresh normal graph before the restored native build and owning test. Never call a guard rejection a compiler failure.

The normal GREEN for each task includes its owning prefix and directly changed producer regressions, not a repeated whole suite without cause. Final whole-suite and macro/strict/monolithic gates are separate tasks. Unchanged Python fixture checks may be attributed to retained evidence; this plan changes no Python file. All native logs retain known unrelated startup warnings separately and the runner's actual `HomeScreen.EnableHomeScreen=0` startup evidence; absence of its old timeout is not proof of zero engine network traffic.

## Controller reconciliation decisions (2026-10-24)

These decisions resolve actual conflicts before implementation; they are not reopened M4b acceptance findings.

1. **Delivered response metadata:** add `Completed.Response` containing actual final status, final redacted headers/effective URL/content type and a body byte-count/storage descriptor. Capture the byte count before decode moves the body. It is separate from wire attempt metadata and byte counters. No new body preview producer: cache/stale/304 delivered previews are `NoProducer` in I1.
2. **Dispatch identity:** add public `uint64 DispatchId` to IN-2, IN-3 and IN-6, forwarding the existing captured `FDispatchTag::Serial`. Keep the private `uint32` generator/range and all attempt numbers. Request/frame excerpts use 0. Public identity is `(RequestId, DispatchId)`; the existing producer range is 1 through `MAX_uint32`, not a new 64-bit generator.
3. **Hard store admission:** `MaxRecords` counts admitted HTTP rows plus markers, including pinned rows. Reserve a minimal active-row allowance before accepting a new row. Never evict admitted in-flight rows; when no evictable space exists, refuse new rows visibly. Later unknown events may retry admission as Partial. Refused operations have no complete-history promise. No unbounded pinned floor or suppressed-id set.
4. **Finite auxiliary domains:** clients, endpoint keys, links, tombstones, timeline and snapshot allocations have explicit global bounds, with one overflow aggregate. Reader/snapshot payload tickets survive through actual payload free, even after eviction/Clear; separate controller tickets and snapshot slots survive through final-weak controller deletion/stored-deleter destruction under the controller-only boundary below.
5. **Lifetime:** retain the shared pending account across all current/retired capture generations. Use normal engine shutdown with dynamic reloading disabled; arbitrary forced unload is unsupported. Use the Inspector receipt clock for excerpt timeout and preserve unknown historical metadata.
6. **Settings lifetime:** queue limits are immutable for a queue-account lifetime; store limits are immutable for a store-account lifetime. A lower setting takes effect only after explicit Clear/recreation, subject to old retained owners releasing their tickets. There is no shrink-under-pins operation.
7. **Staging:** only the native I1 facade/settings surface lands. No I5 BP/export/console implementation or fake success stub.
8. **Controller-only budget boundary (root fix1 amendment):** Inspector-created shared controllers use a retained-owner ledger: each audited controller is charged4096 bytes and its snapshot slot stays held through final-weak controller deletion/stored-deleter destruction. The subsequent hidden engine operator-delete/free tail is excluded. It is at most4096 bytes per already-destroyed controller on the supported paths, but has no aggregate physical bound under arbitrarily suspended external final-release threads. Public UE TSharedPtr exposes no post-controller-free hook. All observable Inspector payload/backing allocations still release credit AFTER actual free. Do not call this a strict physical allocated-capacity/RSS cap or claim hidden-free observation. A stronger physical controller cap needs a separately designed ownership/allocator seam; none is authorized here.

Costs: extra redacted final metadata only while tracing; additional scalar identity forwarding without a new counter; visible capture loss when pinned/retained readers consume capacity; conservative admission may retain fewer records than a nominal count setting. Cached delivered previews remain unavailable, and historical source/UTC data may be unknown. These limits are part of the acceptance record.

## File structure and execution order

Every path below is exact; grouped `.h`/`.cpp` entries mean both literal files. New Inspector paths have prefix `Source/VaRestInspector/`.

| Task | Owned files | Result |
| --- | --- | --- |
| 1 | `Source/VaRestHttp/Public/VaRestTrace.h`, `Public/VaRestResultHandler.h`; `Private/VaRestExchangeState.h`, `Private/VaRestPipeline.cpp`, `Private/VaRestTracePoints.h`, `Private/VaRestTracePoints.cpp`, `Private/VaRestTraceExcerpts.h`, `Private/VaRestTraceExcerpts.cpp`; existing `Private/Tests/VaRestHttpExcerptTest.cpp`, `Private/Tests/VaRestHttpTraceCacheOfflineTest.cpp`, `Private/Tests/VaRestHttpTraceTest.cpp`, `Private/Tests/VaRestHttpTraceGoldenTest.cpp`; Inspector spec and `docs/dev/cache-offline.md` | Additive producer prerequisite and owning regressions |
| 2 | `VaRest.uplugin`; new `VaRestInspector.Build.cs`, `Public/VaRestInspector.h`, `Public/VaRestInspectorRecords.h`, `Public/VaRestInspectorPacketView.h`, `Public/VaRestInspectorStats.h`, `Private/VaRestInspectorPolicy.h`, `Public/VaRestInspectorSettings.h`, `Private/VaRestInspectorSettings.cpp`, `Private/VaRestInspectorModule.cpp`, `Private/Tests/VaRestInspectorModuleTest.cpp` | Independently loadable runtime module/settings/native compiled-out subset |
| 3 | new `Private/VaRestInspectorAllocation.h`, `Private/VaRestInspectorAllocation.cpp`, `Private/VaRestInspectorPacket.h`, `Private/VaRestInspectorPacket.cpp`, `Private/VaRestInspectorQueue.h`, `Private/VaRestInspectorQueue.cpp`, `Private/Tests/VaRestInspectorQueueTest.cpp` | Sized immutable packets, move-only reservations, count/byte-bounded MPSC ownership |
| 4 | modify `Public/VaRestInspectorRecords.h`; new `Private/VaRestInspectorStore.h`, `Private/VaRestInspectorStore.cpp`, `Private/Tests/VaRestInspectorStoreTest.cpp`, `Private/VaRestInspectorBudget.h`, `Private/VaRestInspectorBudget.cpp` | Minimal admitted HTTP rows, identities and snapshot attach |
| 5 | new `Private/VaRestInspectorPhases.h`, `Private/VaRestInspectorPhases.cpp`, `Private/Tests/VaRestInspectorPhasesTest.cpp`; modify Store and Records | Physical dispatch rows and honest phases |
| 6 | modify Store, Records, StoreTest | Completion, delivered response, notes, initiator/decode/envelope |
| 7 | new `Private/VaRestInspectorSnapshot.h`, `Private/VaRestInspectorSnapshot.cpp`, `Private/Tests/VaRestInspectorSnapshotTest.cpp`; modify Store, Records, Module | Excerpt promises, sealing, five-second timeout and immutable snapshots |
| 8 | new `Private/Tests/VaRestInspectorBudgetTest.cpp`; modify Budget, Store, Snapshot, Records | Shared allocation tickets, FIFO/count/budget eviction, pinning and finite auxiliary storage |
| 9 | new `Private/VaRestInspectorLinks.h`, `Private/VaRestInspectorLinks.cpp`, `Private/Tests/VaRestInspectorLinksTest.cpp`; modify Store, Records | Backlinks, queue identity, markers and scope0/client context |
| 10 | modify `Public/VaRestInspectorStats.h`; new `Private/VaRestInspectorStats.cpp`, `Private/Tests/VaRestInspectorStatsTest.cpp`; modify Store | Independently retained bounded statistics |
| 11 | new `Private/VaRestInspectorSink.h`, `Private/VaRestInspectorSink.cpp`, `Private/VaRestInspectorService.h`, `Private/VaRestInspectorService.cpp`, `Private/Tests/VaRestInspectorLifecycleTest.cpp`; modify Module, public facade | Process service, ticker, controls, generation transitions and shutdown |
| 12 | new `Private/Tests/VaRestInspectorTestUtil.h`, `Private/Tests/VaRestInspectorTestUtil.cpp`, `Private/Tests/VaRestInspectorCaptureTest.cpp`; `Source/VaRestTests/Private/VaRestInspectorDecodeTest.cpp`; add one dependency in `Source/VaRestTests/VaRestTests.Build.cs` | Real HTTP producers → Inspector record goldens, reflected decode fixture |
| 13 | new `Private/Tests/VaRestInspectorRedactionTest.cpp`; modify TestUtil | HTTP canary matrix and exact mutation/restoration proofs |
| 14 | new `Private/Tests/VaRestInspectorCostsTest.cpp`; modify ModuleTest/LifecycleTest only as needed for compiled-out assertions | Off/pause counters, detail isolation and macro paths |
| 15 | no permanent source edits; exact temporary owned Build.cs changes only | Strict per-module NoPCH/nonunity and exact normal restoration |
| 16 | no permanent source edits; exact temporary module definitions only if controller permits | Development/Shipping and new excerpts-off/Inspector-off configuration gates |
| 17 | no source edits unless the single final fix wave is dispatched | Full acceptance, coverage/limits report and controller handoff |

Tasks 4–7 use a minimal working budget implementation introduced alongside Store: a fixed account and refusal when full. Task 8 extends it with eviction and accounting coverage; it does not retroactively introduce the first bound. Task 3 includes its own fixed atomic pending account. Likewise Task 2's facade returns valid empty-state values until the service is connected in Task 11; no unfinished pure virtual implementation is linked in an intermediate task.

No existing producer tests are moved. No new production dependency on `VaRestTests`. No test reaches an HTTP private header from the Inspector module. Existing private auth/cache/queue golden fixtures remain core-owned; Task 12 either uses public APIs or replays captured redacted source events from a test observer, without reimplementing stage policy.

## Normative types, bounds and ownership

### Native facade

The public module interface is named `IVaRestInspector`, derives from `IModuleInterface`, and exports the following I1 surface. Public record/stats types are declared in their named headers before the concrete service connects them. `TrySnapshot` fails without partial output if its finite budget cannot admit the snapshot. Payload lasts until last strong release; controller charge and one of four controller admissions last until final-weak stored-deleter destruction. This bounds retained owners; the hidden controller operator-delete/free tail has the explicit exclusion in decision8.

```cpp
#ifndef VAREST_WITH_INSPECTOR
#define VAREST_WITH_INSPECTOR !UE_BUILD_SHIPPING
#endif

class FVaRestInspectorSnapshot;
struct FVaRestInspectorRecordView;
DECLARE_MULTICAST_DELEGATE_OneParam(FVaRestInspectorChanged, uint64);

class VARESTINSPECTOR_API IVaRestInspector : public IModuleInterface
{
public:
    static IVaRestInspector& Get();
    static IVaRestInspector* TryGet();
    static constexpr bool IsCompiledIn() { return VAREST_WITH_INSPECTOR != 0; }
    virtual void SetCapture(bool bEnabled) = 0;
    virtual bool IsCapturing() const = 0;
    virtual void Clear() = 0;
    virtual uint64 GetRevision() const = 0;
    virtual FVaRestInspectorChanged& OnChanged() = 0;
    virtual bool TrySnapshot(
        TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const = 0;
    virtual bool ForEachRecord(
        TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const = 0;
};
```

`Get()` loads `VaRestInspector` only on GT. `TryGet()` never loads it and returns null during/after shutdown. Shipping's concrete module implements SetCapture/Clear as no-ops, IsCapturing false, revision0, an empty delegate and an immutable empty snapshot; `ForEachRecord` returns true with zero calls. Native data-bearing classes are conditionally implemented, while declarations remain compilable. The empty snapshot needs no capture store or ticker. Never let callers hold the module interface across shutdown.

### Finite limits

Public settings carry the spec's defaults. Private safety ceilings are implementation bounds, not new user-facing cvars. Validate once before allocating; invalid nonpositive values use a safe normalized minimum/default, and multiplication uses checked `int64` arithmetic.

| Domain | Default editor / packaged | Hard normalization and accounting |
| --- | --- | --- |
| Store budget | 64 / 8 MiB | 1–1024 MiB; aggregate payload plus retained-owner controller tickets cannot exceed normalized budget; decision8 excludes hidden post-destruction controller free tails |
| Rows (HTTP+markers) | 10000 / 2000 | 1–65536; pre-admit minimal row; markers may be omitted when pinned rows fill count |
| Body / binary / frame excerpt cap | 65536/0/4096; 16384/0/1024 | nonnegative, at most16MiB each; I1 FrameCap0 when reporting sink detail |
| Pending events | 65536 /16384 | 1–262144; includes reserved producers and queued/handing-off items |
| Pending bytes | 16 /4 MiB | 64KiB–256MiB; common account across retired generations; includes packet/nodes/backing/string/excerpt capacity |
| Input metadata work | independent of settings | ≤256 header pairs, ≤32 warnings, ≤16384 TCHAR per string, ≤1MiB copied metadata per item; scalar fallback if any cap is exceeded |
| Per HTTP details | ≤16 physical dispatch slots | each slot identified by DispatchId; total physical count retained separately; older detail can be omitted with explicit count |
| Timeline | ≤128 entries per row | fixed scalar {Kind,Seq,Time,Attempt,DispatchId,detail-state}; no second event/body/string copy |
| Notes | ≤64 per row | FIFO ring of immutable bounded note packets; count omitted notes |
| Retention tombstones |4096 | bounded id ring plus bounded index; no unbounded “seen” map |
| Client metadata |1024 global entries | final overflow client id0 with unknown-label state; never put URL strings in FName |
| Stats groups |1024 global `(scope,client,endpoint)` buckets | at most256 named endpoint keys per client; one process overflow bucket; all allocations charged |
| Backlinks |4096 global edges, ≤64 per retained target | evict oldest edge, count omissions; side table never mutates sealed records |
| Snapshot admissions |4 extant snapshot controllers | plus store budget; return false if count/bytes unavailable; caller-held snapshots retain tickets |
| Startup fixed account | actual allocated capacities | ≤Budget/4; scale private table capacities down under a small budget rather than exceed it |

These are maxima, not eager allocations. A record with 16 full attempts may exceed the “typical metadata ≤1.5KiB” benchmark target; I6 measures it. Do not claim that performance target as a structural bound. Private global table caps are additionally constrained by the byte account, so 1024 histogram groups do not imply they always fit the packaged budget.

### Records and immutable packets

Use explicit native record states:

```cpp
enum class EVaRestInspectorRecordKind : uint8 { Http, Marker };
enum class EVaRestInspectorEndState : uint8
{
    InFlight, Success, Error, Queued, Canceled, CaptureInterrupted
};
enum class EVaRestInspectorDetailState : uint8
{
    Absent, Pending, Present, Backlog, Discarded, TimedOut,
    CaptureStopped, Evicted, NoProducer, MetadataOmitted
};
struct FVaRestInspectorPhase
{
    double Seconds = 0.0;
    bool bKnown = false;
};
struct FVaRestInspectorTimelineEntry
{
    EVaRestTraceKind Kind = EVaRestTraceKind::Started;
    uint64 Seq = 0;
    double Time = 0.0;
    int32 Attempt = 0;
    uint64 DispatchId = 0;
};
struct FVaRestInspectorLink
{
    uint64 FromRequestId = 0;
    uint64 ToRequestId = 0;
    FString QueueId;
    bool bAuth = false;
    bool bTargetEvicted = false;
};
```

`FVaRestInspectorHttpRecord` has: RequestId/ScopeId/ClientId; arrival ordinal/revision; source StartedAt/StartUtc with known bits; immutable started metadata packet; at most16 dispatch detail slots with ids; request excerpt slot and one response excerpt slot per retained dispatch; completed metadata packet; bounded notes/timeline; end state; observed completion time on the Inspector clock; capture-continuity/partial/details-evicted flags; total dispatch count/omitted attempts/notes/timeline counts; wire byte totals; and terminal-known/bSealed bits. Initiator, queue replay, final response and decode/envelope fields are read from the appropriate immutable packet, never duplicated into a second string collection. `FVaRestInspectorMarker` has arrival ordinal, source time, real ScopeId/ClientId/CauseRequestId, core marker kind or Inspector CaptureOverflow tag, immutable text packet, and scalar overflow counts.

`FVaRestInspectorRecordView` is a tagged borrowed reference valid for one visitor invocation. `FVaRestInspectorSnapshot` owns a bounded array of immutable HTTP/marker handles plus a value snapshot of finite stats, links and counters. Its public class declaration lives in Records.h; Snapshot.h is only its private backing implementation. It exposes GetRevision(), ForEachRecord(Visitor), ForEachStats(Visitor), GetLinks() and GetDiagnostics() as read-only snapshot operations, including PendingLimits/effective limits and capture-loss counters in diagnostics. The exact complete public signatures appear in the fix1 contract section, with the stats value forward-declared until Task10; no consumer needs a Private header. Its payload credit survives until payload free; its separate controller credit/slot survives through stored-deleter destruction at final weak release, before the excluded hidden controller free tail (decision8). Sealed HTTP records may be shared; unsealed records are cloned on GT with fresh array/index storage and independently admitted immutable packet-backing copies and shared immutable excerpt references. Sharing a const record pointer while mutating its object is forbidden.

A capture packet contains only a fixed header, a typed scalar payload, offset/length text slices, header/warning slice pairs and one optional immutable excerpt-byte reference. All metadata text occupies one exact-sized backing allocation. No UObject, callback, FString/TArray with hidden growth, trace-event pointer or reference into a snapshot is retained. Two passes over the borrowed event perform sizing then writing; only the second pass runs after admission. The fixed scalar payload is FPacketScalars, a fixed struct interpreted by the validated FPacketHeader.Kind for the nine current HTTP payloads; its complete declaration is below. It carries the same scalar fields/enums/FNames as the trace counterparts without nontrivial union lifetime operations. FName values already owned by core are copied as FNames; never intern captured URL/header/body text.

**Complete field inventory for the packet codec:**

| Payload | Scalar data | Text/array data copied into backing storage |
| --- | --- | --- |
| Started | RequestId/ScopeId/ClientId; Endpoint/Tag/Priority/Flags; OriginalRequestId; CachePolicy/CacheLookup/pending; Body.Bytes/Kind; excerpt promise; StartUtc/CurrentAttempt/StartedAt; ScriptCodeOffset/SourceLine | Verb, Url, PathTemplate, QueueReplayOf.QueueId; all four initiator strings; Headers ordered pairs; Body.ContentType |
| Dispatched | RequestId/DispatchId/Attempt; AttemptStart/GapKind/GapSeconds/AuthWait/AuthRefreshId/LimiterWait; conditional | final Headers ordered pairs |
| Ended | RequestId/DispatchId/Attempt/Status; BytesUp/Down/Failure/FailureDetail; UploadDoneAt/StatusAt/TransportDoneAt; pending | Headers, EffectiveUrl, ContentType |
| Note | RequestId/Attempt/Kind/Reason/Delay/RetryAfter/Bytes | QueueId |
| Completed | RequestId/Outcome/CancelReason; Error.Kind/HttpStatus; ResultType; DecodeStartAt/EndAt; total warning count; Envelope.bApplied; CacheLookup/from-cache; AuthRefreshId; Response.Status/BodyBytes/Storage | Error.Code/Message; each warning Path/Message; all six Envelope strings; final Response.Headers/EffectiveUrl/ContentType |
| Excerpt | Owner/RequestId/DispatchId/Attempt/SocketId/FrameId; Kind/TotalBytes/truncated/redacted/SkipReason | StopReason, Sha256Prefix; shared immutable Bytes retained only after its allocated-capacity admission |
| Scope | Opened/ScopeId/PIEInstance/editor-scope | Label; no GameInstance pointer retained |
| Client | Created/ClientId/ScopeId | DisplayName |
| Marker | Kind/ScopeId/ClientId/CauseRequestId | Text |

The packet reader returns borrowed FStringView/header/warning views into that backing block. When a public snapshot needs an owning string (for example QueueId in a link), reserve prospective string capacity first and charge the resulting `GetAllocatedSize()`; do not materialize all packet strings into a temporary FVaRestTraceEvent. Source scalars are copied field-by-field, not by serializing C++ object padding or casting a nontrivial TVariant to bytes.

Queue admission counts the packet wrapper, its shared-controller allowance, the requested backing capacity including terminators/pair indexes/alignment, the exact `TMpscQueue` node request (observed through a local allocator adapter), and retained excerpt `Bytes->GetAllocatedSize()`. It deliberately charges shared excerpt bytes per queue reference conservatively. Store ownership deduplicates by allocation-owner ticket, never by byte content. Pointer-key dedup entries disappear when their last owner releases, so allocator pointer reuse cannot alias old payloads.

Use a move-only reservation whose destructor releases once; moved-from tokens hold zero. Release queue credit only after its data has been destroyed or transferred to an independently admitted store owner. Dequeue does not itself mean the bytes vanished. Store admission failure drops the packet while its queue ticket still exists, then releases that ticket. Queue and store transient overlap is explicit; no uncharged “one item being applied” exception.

### Task 1: Close the two verified producer seams without changing HTTP behavior

**Files:** the twelve HTTP files listed in the file table, plus `docs/superpowers/specs/2026-10-20-varest2-inspector-design.md` §§3.3, 9.1 IN-2/3/5/6 and `docs/dev/cache-offline.md` “Trace and M5 integration freeze”. These two documentation edits are part of the future implementation task, not this planning lease. Do not edit the core stage policy or accepted M4b reports.

- [ ] **Step 1: Add compiling owning assertions to existing producer fixtures.** Extend `VaRest.Http.Trace.ExcerptOnePerDispatch` to assert positive, different DispatchIds for its two dispatches and correct matching end ids in both existing end orders; keep its distinct `{n:1}` and `{n:2}` bodies. Extend `VaRest.Http.Trace.Golden.StaleAttemptEndQueued` in ExcerptTest with the old dispatch id, and `VaRest.Http.Trace.CancelAtDispatch` there with its synthetic matching end id. In TraceGoldenTest extend the existing Cancel golden with identity preservation through cancellation. Add `VaRest.Http.Trace.DeliveredResponse` cases using existing public fake/cache fixtures: network200, fresh memory hit, disk hit, wire304/delivered200, known-offline stale, attempted stale, local Config refusal, queued success and SaveTo descriptor. Assert cached BodyBytes14 for `plain-response` while BytesDown0, actual final200 versus physical304, and unknown `-1` for unobserved/stream size. Before adding fields, missing-member compile RED is allowed under the normal graph gate; behavioral RED still follows in Step4.

- [ ] **Step 2: Add the exact public fields and smallest private forwarding.** Add these definitions before `FVaRestTraceCompleted` in `VaRestTrace.h` and add `FVaRestTraceResponse Response;` to Completed. No new trace kind or reflected type is needed.

```cpp
enum class EVaRestTraceResponseStorage : uint8 { Unknown, Memory, Stream };
struct FVaRestTraceResponse
{
    int32 Status = 0;
    FVaRestHeaders Headers;
    FString EffectiveUrl;
    FString ContentType;
    int64 BodyBytes = -1;
    EVaRestTraceResponseStorage Storage = EVaRestTraceResponseStorage::Unknown;
};
```

Add `uint64 DispatchId=0;` to `FVaRestTraceAttemptDispatched`, `FVaRestTraceAttemptEnded` and `FVaRestTraceExcerptEvent`. Add `uint64 ExcerptOnDecodeDispatchId=0;` beside the existing exchange decode marker; `uint64 ExcerptDispatchId=0;`, `int64 TraceResponseBodyBytes=-1;` and `EVaRestTraceResponseStorage TraceResponseStorage=Unknown;` to `VaRest::Http::FFinish`; and `uint64 DispatchId=0;` to `Excerpts::FJob`. Keep `FDispatchTag::Serial`, `DispatchCount`, `LiveDispatch` and ScheduleProgress parameter uint32. Forward the captured tag explicitly:

```cpp
// VaRestTracePoints.h and matching definition:
void AttemptDispatched(const FExchangeState& Ex, const FDispatchTag& Tag);
// Transport-stage call immediately after ThisDispatch is formed:
TracePoints::AttemptDispatched(Ex, ThisDispatch);
// In the masked IN-2 producer:
Payload.DispatchId = static_cast<uint64>(Tag.Serial);
// In the masked IN-3 producer:
Payload.DispatchId = static_cast<uint64>(Tag.Serial);
```

Change `PrepareResponse` declaration/definition to `(const FExchangeState&, const FVaRestTransportResult&, int32 Attempt, uint64 DispatchId)` and pass the current tag's id. Change `ExcerptsPrivate::Emit` to include `uint64 DispatchId` after Attempt, assign it into the excerpt payload, and update every call: request jobs0; response pipe jobs captured id; decode Finish.ExcerptDispatchId; Discarded captured marker id. The task's source reconciliation searches all Emit/PrepareResponse call sites; no implicit default argument conceals a forgotten response caller.

`AttemptEnded` assigns both decode marker fields together only for the current dispatch. `OnChainDone` copies both into FFinish before clearing both. `ResolveDiscarded` copies the pair locally, clears the pair even when Bodies is no longer wanted, and emits their captured identity when wanted. Stale ends never clear another dispatch's pending marker. No identity is read from mutable Ex.LiveDispatch inside an asynchronous job.

Before `Finish.Body = MoveTemp(Outcome.Body)` in OnChainDone, capture only cheap scalars under the existing trace mask:

```cpp
if (FVaRestTrace::Detail() != EVaRestTraceDetail::None)
{
    Finish.TraceResponseStorage = Outcome.bBodyInFile
        ? EVaRestTraceResponseStorage::Stream
        : EVaRestTraceResponseStorage::Memory;
    Finish.TraceResponseBodyBytes = Outcome.bBodyInFile
        ? -1 : static_cast<int64>(Outcome.Body.Num());
}
```

Then, after Completed's existing early mask return and redactor acquisition:

```cpp
Payload.Response.Status = Finish.Response.Status;
Payload.Response.BodyBytes = Finish.TraceResponseBodyBytes;
Payload.Response.Storage = Finish.TraceResponseStorage;
Payload.Response.EffectiveUrl = Redactor.RedactUrl(Finish.Response.EffectiveUrl);
Payload.Response.ContentType = Redactor.ScrubText(Finish.Response.ContentType);
if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
{
    Payload.Response.Headers = Redactor.RedactHeaders(Finish.Response.Headers);
}
```

A request that enters decode without a trace sink and gains one later legitimately reports BodyBytes-1. It must not infer size from an emptied decoder body or wire counters. A queued completion has no delivered response. Empty EffectiveUrl from a cache answer stays empty. The response storage enum is separate from `EVaRestBodyKind`, which describes a request construction kind.

- [ ] **Step 3: Add the missing asynchronous identity case.** In `VaRestHttpExcerptTest.cpp`, reuse its owned gate pattern, not the production global pipe stop. A PerAttempt stage proceeds twice sequentially under one Attempt: the first current raw response announces/queues a pipe excerpt held behind a test-owned barrier; the second response reaches the decode path and emits first. Assert equal Attempt values, different DispatchIds, payloads tied to their original ids despite reversed arrival, no cross-resolution and zero owned admissions after release. Also assert Backlog and Discarded carry the promised response DispatchId. Request excerpt DispatchId must remain0.

- [ ] **Step 4: Perform meaningful RED/GREEN/restoration.** Once the code compiles, make three separately approved temporary mutations: stamp every dispatch id with Attempt; derive delivered BodyBytes from Finish.Body at Completed; assign final status from the last wire end. Each must fail its owning assertion (both-order identity, consumed-body/cache hit size, 304 final status). Restore exact bytes, freshly inspect/build and pass owning tests after each. The no-sink and Headers-off controls must still pass; body-copy counters must not increase because of metadata. Run `Tools/run_tests.sh --no-build VaRest.Http.Trace`, `VaRest.Http.TraceSurface`, and the affected excerpt prefix after separately guarded builds. Do not run an unmatched prefix and report success.

- [ ] **Step 5: Amend the exact consumer contract and hand off.** Update the spec table with DispatchId and Completed.Response, source scalar timing and cached-preview unavailability. Preserve the current13 kinds, no metadata after Completed, Note.Attempt0, cache provenance and replay request-preview rules. Record the source/test hashes, actual RED witnesses, restored source identity and owner acceptance before the controller commits. This task is a complete additive HTTP change even before the Inspector module exists.

### Task 2: Add a buildable runtime module, settings and native stubs

**Files:** the descriptor and exact new module/settings/facade/public-shell/policy/test headers listed in the table. The public facade includes only Core/ModuleInterface/Function/SharedPointer and forward declarations until record headers exist. No UI, Settings editor module, host source or BP function library.

- [ ] **Step 1: Add the descriptor and minimal Build.cs.** Insert one Runtime module at Default with the five-platform allowlist; preserve the current descriptor version and unrelated module entries. Use this complete rule:

```csharp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
using UnrealBuildTool;
public class VaRestInspector : ModuleRules
{
    public VaRestInspector(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core", "CoreUObject", "DeveloperSettings", "VaRestHttp"
        });
        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "Engine", "VaRestJson"
        });
    }
}
```

- [ ] **Step 2: Declare unconditional settings.** `FVaRestInspectorLimits` is an unconditional USTRUCT with `UPROPERTY(config, EditAnywhere, Category="Limits")` fields `int32 BudgetMiB`, `MaxRecords`, `MaxBodyBytes`, `MaxBinaryExcerptBytes`, `MaxFrameExcerptBytes`, `MaxFramesPerSocket`, `MaxPendingEvents`, `MaxPendingMiB`. Constructor/default members initialize editor defaults64/10000/65536/0/4096/5000/65536/16. `UVaRestInspectorSettings : UDeveloperSettings` uses `UCLASS(config=Game, defaultconfig, meta=(DisplayName="VaRest Inspector"))`; unconditional config properties are bCaptureInEditor=true, bCaptureInPackaged=false, bCaptureHeaders=true, bCaptureBodies=true, Editor, Packaged, bHideEngineIoFrames=true. Its constructor assigns CategoryName="Plugins", SectionName="VaRestInspector" and packaged limits8/2000/16384/0/1024/1000/16384/4. The generated include is last. Native normalization consumes these values without mutating the CDO.

- [ ] **Step 3: Implement the complete empty service contract and module test.** The concrete module overrides every facade method, initially returning the valid empty behavior described above. It sets SupportsDynamicReloading false. Do not register a sink/ticker yet. Its `Get` uses `FModuleManager::LoadModuleChecked<IVaRestInspector>("VaRestInspector")`, TryGet uses `GetModulePtr` with the shutdown flag. ModuleTest asserts module loaded, setting defaults and packaged defaults, IsCompiledIn matches the macro, IsCapturing false, no capture console object, and zero sink-count change from Clear/empty snapshot. Declare the public snapshot shell and read types in Public/VaRestInspectorRecords.h for this task (revision0, zero-record enumeration, no backing allocations), included by the facade. Public PacketView.h supplies its complete trace-scalar/slice views and Stats.h initially forward-declares the stats value. Task4 supplies live store views; Task7 adds private backing without moving/renaming the public declaration. No private-only public type remains inaccessible to callers.

- [ ] **Step 4: Build RED/GREEN honestly.** A test detecting wrong packaged MaxRecords or an accidental unconditional capture registration is a behavioral RED; temporarily choose the wrong default under the owned settings file, observe exact assertion failure, restore and pass. First graph must account for new module UHT/generated products and its plugin link. If module discovery triggers engine/host compilation or PCH creation, stop with the graph; do not enable global build flags. Run `Tools/run_tests.sh --no-build VaRest.Inspector.Module` only after the permitted normal build. No Shipping runtime claim is made here.

### Task 3: Admit immutable capture packets before copying

**Files:** Allocation.h/.cpp, Packet.h/.cpp, Queue.h/.cpp, QueueTest.cpp; reuse Policy.h and public PacketView.h created Task2. Guard native implementations with `#if VAREST_WITH_INSPECTOR`; tests compile only with WITH_DEV_AUTOMATION_TESTS and exercise the compiled-out branch explicitly in Task14.

**Private contract:** use the complete FPacketCodec and FCaptureQueue declarations in the fix1 contract section: Measure returns EMeasure, Write returns a move-only FPacket, TryPush returns EPush, and the two lane pops return TOptional<FPacket>. Measure never allocates or acquires references. Distinguish Unsupported from ScalarOnly (the measured metadata-too-large fallback) so only the latter records metadata omission/loss and attempts independently reserved scalar retention. Snapshot entries use their source StartedAt and an explicit bSnapshot flag; they do not get a fabricated source Seq. Read counters through the declared account/allocator probes and queue ordinal/loss accessors; there is no alternate untyped GetCounters/TryPop API.

- [ ] **Step 1: Add queue tests before implementation.** Register `VaRest.Inspector.Store.QueueAdmission` with empty/one item/FIFO boundaries, exact-equality count and byte capacity, one-byte-over, huge allocated excerpt capacity with small Num, header array over256, string over16384 TCHAR, aggregate metadata over1MiB and arithmetic overflow. Test a rejected item increases neither OwnedTextCopied nor ExcerptReferencesAcquired. Accepted ordinary fields remain byte-for-byte equal and redacted text remains unchanged. A metadata-oversize Completed falls back to its fixed kind/request/dispatch/outcome/cache/response-scalar descriptor only if that descriptor is independently admitted. Mark MetadataOmitted and count it.

- [ ] **Step 2: Implement checked atomic reservation.** Count is reserved before bytes and rolled back if bytes cannot be reserved; all counters include producers between reservation and publication. Closed generations may finish an already admitted operation only into their own queue. `FPendingTicket` owns a shared account plus its count/byte charge and releases with fetch_sub once on destruction. The account is never Reset while tickets exist. Use this complete bounded primitive:

```cpp
static bool TryReserve(std::atomic<int64>& Used, int64 Limit, int64 Amount)
{
    if (Amount < 0 || Amount > Limit) return false;
    int64 Seen = Used.load(std::memory_order_relaxed);
    do
    {
        if (Seen < 0 || Seen > Limit - Amount) return false;
    }
    while (!Used.compare_exchange_weak(Seen, Seen + Amount,
        std::memory_order_acq_rel, std::memory_order_relaxed));
    return true;
}
static bool AddChecked(int64& Sum, int64 Amount, int64 Limit)
{
    if (Amount < 0 || Sum < 0 || Amount > Limit || Sum > Limit - Amount)
        return false;
    Sum += Amount;
    return true;
}
```

Two-pass packet encoding follows the complete field inventory above. The first pass uses bounded stack descriptors and lengths, no FString formatting or ToText. Size every terminated TCHAR range and index alignment with AddChecked; obtain backing allocation capacity before ownership. The second pass writes exactly the measured layout and constructs borrowed read views only after the backing block is stable. Store the supplied Source/Kind/Seq/Time and local generation/metadata ordinal. A source Header/Body flag being absent prevents retaining that field even if another sink caused the producer to include it.

- [ ] **Step 3: Implement overflow accounting without queue recursion.** Account snapshots expose DroppedCount, DroppedBytes, MetadataOmitted, ExcerptsSkipped and an incremented loss epoch. Over byte limit, an Excerpt attempts a small fixed Backlog packet preserving RequestId/DispatchId/Owner/Attempt/TotalBytes; if that cannot fit, count a dropped excerpt. Over count limit, drop without enqueue. The drain creates one CaptureOverflow summary per observed stall episode directly through store admission; a reserved scalar diagnostics aggregate retains the counts even if a marker row cannot fit. Do not enqueue an overflow event into the overflowing queue.

- [ ] **Step 4: Prove publication and destruction safety.** Add `VaRest.Inspector.Store.QueueConcurrent`: two owned workers, one deliberately held after reservation and one immediately before calling TMpscQueue::Enqueue (test hook local to this queue). A transient empty Dequeue must not reset counters or free the queue. Close, drop the facade reference, release workers, then drop their retained sink-list refs; observe final queue/token destruction on any thread with count/bytes zero. Repeat with three generations sharing one account, proving summed peak≤MaxPendingBytes and no borrowed-event access after OnEvent returns. The owner barrier cleanup executes on every failed assertion; never wait for GT from OnEvent.

- [ ] **Step 5: Behavioral RED/restoration.** Independently remove byte admission (R10), move the text copy before admission, and release the ticket at dequeue before consumer handoff. Each unchanged test must fail a byte peak/copy count/retention assertion. Restore exact bytes and run the guarded normal build plus `VaRest.Inspector.Store.Queue` owning prefix. Include count-only saturation as a control so byte RED cannot pass because count admission masked it.

### Task 4: Admit HTTP rows and apply source snapshots

**Files:** Records.h (modify shell from Task2), Store.h/.cpp, StoreTest.cpp and new Budget.h/.cpp. The budget files begin with a working shared account/ticket implementation; Task8 extends their eviction policy and evidence. Queue/packet APIs remain private to this module.

**Private store API:** `FStore(const FNormalizedLimits&, TSharedRef<IVaRestClock, ESPMode::ThreadSafe>)`; `bool Apply(FPacket&&)`; `void ApplyAttached(const FVaRestTraceSnapshot&, const FCapturePolicy&, TSharedRef<FPendingAccount, ESPMode::ThreadSafe>, FAllocationAdapter&)`; `void BeginGeneration(uint64 Generation, uint64 ConsumedOrdinal)`; `void Clear()`; `uint64 Revision() const`; `bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>&) const`; `bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)>) const`. Construction/application/enumeration/destruction of the mutable store run on GT; allocation tickets and immutable owners may release on any thread. No public method returns mutable records.

- [ ] **Step 1: Write `VaRest.Inspector.Store.IdentityAndAttach`.** Feed Started id41, its snapshot duplicate, then a real event for id41: exactly one row and one arrival ordinal. A snapshot-only id42 is Partial with CurrentAttempt/StartedAt preserved, unknown UTC/initiator when absent, no invented prior phases/headers/excerpt promises. Feed Ended or Completed for unknown id43: admit Partial if capacity permits. Feed Socket source, an unsupported kind and RequestId0 metadata: no HTTP row, no invalid variant access. Scope0 remains Process. Hold MaxRecords live rows and prove row N+1 is visibly refused with no secondary request-id allocation.

- [ ] **Step 2: Define the complete minimum active row and its initial budget.** Use a stable shared allocation owner for each row. Allocate only the base record and empty inline indices initially; later packet/detail ownership is admitted separately. Base admission reserves: allocator-quantized row storage, shared-controller storage, row index slot and arrival-ring slot. Both row count and byte reservation must succeed before inserting either index. On failure release all reservations and increment the finite RefusedRows counter. Every row stores `uint64 BirthMetadataOrdinal=0` and `enum class ELivenessEvidence:uint8 {None,Started,Snapshot}` with an Evidence member; actual Completed is terminal immediately. Real Started birth uses its consumed metadata ordinal and pins. Direct snapshot birth uses LastConsumedMetadataOrdinal at entry and pins from live source presence. Before any new-generation attach walk, BeginGeneration rebases EVERY retained mutable nonterminal row to that generation's consumed ordinal (normally0); this changes only recovery eligibility, never original arrival/history/request identity, terminal truth or source timestamps. Unknown-id Dispatched/Ended/Note/Excerpt births use the current LastConsumedMetadataOrdinal (advanced to this packet's ordinal BEFORE metadata Apply), begin unpinned CaptureInterrupted with unknown terminal details, and increment one GT-local OrphanEpoch after admission. A later real Started or source snapshot presence promotes them to live/pinned, retaining bPartial=true. Reconciliation of an orphan never fabricates a transport outcome. No worker changes OrphanEpoch or a metadata ordinal. Never turn an absent source field into an observed empty value; keep known bits.

- [ ] **Step 3: Implement packet transfer with overlap accounting.** The queue ticket remains live while store owners reserve their charges. Transfer the immutable packet to a store-specific owner only after successful reservation; count both accounts during overlap. Reuse the existing sealed row-owner ticket when sharing it with snapshots; an unsealed snapshot clones its uniquely owned packet backing under independent credit. Packet metadata retained by a row may be refused independently of the base row; keep the scalar result and MetadataOmitted flag only when its already reserved base slot can represent it. Do not allocate a fallback FString, TMap node or TArray temporary on this path.

- [ ] **Step 4: Apply attach synchronously.** OnAttached calls the service's GT-only `ApplyAttached` before the sink is published. Iterate the borrowed snapshot once; admit each client and each Started independently. Use the complete borrowed MeasureStarted/WriteStarted and direct ApplyAttachedStarted handoff below: reserve a minimal base and a common pending ticket before any Started text/header copy, then independently admit store transfer; never construct an owning event or enqueue snapshot metadata. Existing Started metadata is not recopied on duplicate attach. Never retain the source snapshot or its backing arrays. Initial attach can contain arbitrarily many source requests: its pre-existing source allocation and single O(live clients+requests) traversal are explicit upstream exclusions. Inspector-owned copies remain bounded. No OnChanged callback runs during AddSink/OnAttached; mark dirty and notify from the next service batch. A failure in one snapshot item does not roll back unrelated admitted items.

- [ ] **Step 5: Restore-the-bug proof.** Temporarily remove the RequestId lookup before snapshot insertion: unchanged IdentityAndAttach must fail its exactly-one-row assertion. Separately bypass base admission with count capacity already full: the live-row cap assertion must fail. Restore and run the normal guarded build and `VaRest.Inspector.Store.IdentityAndAttach`. Capture baseline/current/peak owned-byte counters without making an RSS claim.

### Task 5: Pair physical dispatches and derive honest HTTP phases

**Files:** Phases.h/.cpp, PhasesTest.cpp, Store.h/.cpp, Records.h. Do not change the HTTP pipeline here.

- [ ] **Step 1: Add `VaRest.Inspector.Capture.Phases` as a table-driven pure-store test.** Include one success, 503/backoff/200, expired-auth401/refresh/replay200, two Attempt1 dispatches ending in each order, pre-dispatch parked cancellation, synthetic cancel-at-dispatch, unknown upload/status timestamps, and a late-attach row missing its dispatch start. For every expected phase assert its known bit as well as duration. Assert wire byte sums include both physical ends, while final delivered status comes only from Completed.Response.

- [ ] **Step 2: Implement the complete phase primitive.** Invalid, absent or inverted endpoints yield unknown, not zero. A genuine equal start/end is known zero. The source `-1` sentinel must remain unknown. Use the same source clock within one request only:

```cpp
static FVaRestInspectorPhase Between(double Begin, double End)
{
    FVaRestInspectorPhase Out;
    if (FMath::IsFinite(Begin) && FMath::IsFinite(End)
        && Begin >= 0.0 && End >= Begin)
    {
        Out.Seconds = End - Begin;
        Out.bKnown = true;
    }
    return Out;
}
```

Total is StartedAt→Completed.Time when StartedAt is known. Let DispatchAt be the Dispatched event.Time, distinct from payload AttemptStart. Auth=AuthWait and Queued=LimiterWait; Prepare=DispatchAt−AttemptStart−AuthWait−LimiterWait when finite/nonnegative. GapKind/GapSeconds belong before their captured dispatch and are never inferred from preceding end arrival. Send=DispatchAt→UploadDoneAt; Wait=UploadDoneAt→StatusAt; Receive=StatusAt→TransportDoneAt; Decode=DecodeStartAt→DecodeEndAt; Deliver=DecodeEndAt→Completed.Time when those endpoints are known. If upload is unknown but status is known, Send is unknown and merged Wait=DispatchAt→StatusAt, explicitly labeled combined Send/Wait. Missing status/transport endpoints stay unknown. Attempt wall-span checks sum Auth+Queued+Prepare+Send+Wait+Receive (or combined Send/Wait) against AttemptStart→TransportDoneAt. Time before the first AttemptStart remains logical outer processing, not a fabricated limiter queue phase. Do not create DNS/connect/SSL/redirect timings from transport-total differences.

- [ ] **Step 3: Key detail by `(RequestId, DispatchId)`.** Attempt remains the producer's retry label. A bounded linear search of at most16 slots is sufficient; no unbounded per-row map. Allocate a slot at Dispatched or an orphan Ended, fill missing half later, and retain raw source times. When the slot cap is reached, count omitted dispatch detail and retain total physical counters; never merge two dispatches because they share Attempt. Response excerpt slots use the same DispatchId. A DispatchId0 response is malformed/unknown and cannot be assigned by Attempt. Request excerpts use DispatchId0 intentionally. Compare bounded ordered dispatched-header views to the previous retained dispatch; when equal, share the immutable header owner instead of storing a second copy. Different headers require fresh admission. Do not deduplicate unrelated metadata by lossy hashes.

- [ ] **Step 4: Keep limiter queue and retry/auth quantities separate.** Queued is the captured LimiterWait; it is not increased by retry backoff, outer logical time or refresh. Refresh gap belongs to the replayer; a peer parked before its first dispatch gets AuthWait instead. Pre-attempt refusal/queueing has Attempt0 notes and no synthetic dispatch row. An unmatched old Ended must not overwrite current response selection. The aggregate completed row keeps final response metadata separately from all physical dispatch records.

- [ ] **Step 5: Kill two precise mutants.** Replace DispatchId matching with Attempt matching: both duplicate-order fixtures must fail pairing/body ownership. Replace `Between`'s unknown branch with known zero: the missing-milestone assertions must fail. Restore byte-for-byte, rebuild through fresh action inspection and run `VaRest.Inspector.Capture.Phases` plus Task1's owning duplicate-dispatch golden prefix. Do not run a full suite at this boundary.

### Task 6: Apply completion, notes and diagnostic metadata

**Files:** Store.h/.cpp, Records.h, StoreTest.cpp.

- [ ] **Step 1: Add `VaRest.Inspector.Store.Completion` and `.Notes`.** Exercise each real Completed outcome/cancel reason, cache-memory/disk/304/stale, queued success, Config error before dispatch, 40 warning inputs represented as32 plus total40, envelope predicate failure, initiator strings/offset/source line, original/replay ids, and all six Note kinds including Attempt0. Deliberately give Error.HttpStatus a different value from Completed.Response.Status to prove delivered metadata uses its dedicated field. A late source metadata event after genuine Completed is rejected/counts a contract violation and cannot mutate sealed contents; legitimate late Excerpt follows Task7.

- [ ] **Step 2: Populate the explicit terminal state and packet owners.** Success/Error/Queued/Canceled map from the real Outcome, not HTTP status alone. Queueing is terminal for this RequestId; the later replay is a new RequestId. Preserve Error.Kind/HttpStatus/Code/Message, result type, warning paths/messages/total, envelope bApplied/paths/expected/actual/code/message, decode times and Completed.AuthRefreshId. Preserve the source final CacheLookup and bFromCache separately from Started and cache notes. No RawBody/Body member of FVaRestError is traversed.

- [ ] **Step 3: Select response metadata without guessing.** The final response panel data comes from Completed.Response, physical attempt panels from Ended. BodyBytes is the delivered scalar or -1; physical BytesDown remains transport accounting. If bFromCache is true, final preview state is NoProducer even when a wire304 physical response excerpt exists. A normal network response can reference its actual selected dispatch excerpt only when that association is known; otherwise final preview is NoProducer/Absent. Never copy a previous attempt's body into a successful later response. Stream descriptors carry no guessed length.

- [ ] **Step 4: Retain bounded diagnostic history.** Notes and timeline use their specified64/128 caps. Timeline stores scalar indices and ownership state, not second text copies. Preserve core Seq and Time even when worker excerpts arrive out of Seq order; insertion order and source order are distinguishable. Evict optional oldest notes/timeline detail within their rings, increment omissions and retain terminal scalars. Never evict a base in-flight row to keep its newest note. Initiator contains already redacted string metadata, never a live UObject/Blueprint pointer.

- [ ] **Step 5: Run targeted behavioral RED.** Use the last wire Ended.Status as final status: the304→200 assertion must fail. Force all Note.Attempt values to1: the pre-attempt queue/cache assertions must fail. Restore exactly and run guarded normal build plus `VaRest.Inspector.Store.Completion`/`.Notes`. Parent reviews the enum mapping against the pinned public declarations; no string-to-enum reconstruction.

### Task 7: Resolve excerpt promises and publish immutable snapshots

**Files:** Snapshot.h/.cpp, SnapshotTest.cpp, Store.h/.cpp, Records.h and Module.cpp; move Task2's complete empty snapshot method/FData definitions out of Module.cpp into Snapshot.cpp and replace their bodies with bounded backing, without duplicate definitions or public renaming.

- [ ] **Step 1: Add `VaRest.Inspector.Store.Sealing` and `.SnapshotIsolation`.** Cover request/response excerpts arriving before and after Completed, Backlog/Discarded, missing promise timeout at4.999 and5.000 observer seconds, no promise, pending runtime Bodies→off, duplicate excerpt, eviction then late excerpt, old generation then Clear, and retained immutable snapshots before/after every mutation. Give source events times1000000 while receipt clock begins0; timeout remains5 seconds after observed completion. A cache final preview stays NoProducer while its physical304 slot may retain its own preview.

- [ ] **Step 2: Store promises by owner and physical identity.** Started.bExcerptPending promises the request slot. Ended.bExcerptPending promises that DispatchId's response slot. An early Excerpt may fill its bounded slot before the corresponding metadata is drained; no unbounded orphan map is permitted. If a base row/dispatch slot cannot be admitted, count/drop the early excerpt. An excerpt-only admitted base starts unpinned CaptureInterrupted and requests bounded local orphan reconciliation even when the last producer-loss epoch was already handled; source presence is required to promote it to pinned. An actual Completed remains the only terminal-result evidence. Excerpt.SkipReason Backlog or Discarded resolves the corresponding promise explicitly. Disabled has no normal runtime producer; when detail was disabled, consumer policy resolves pending promises as CaptureStopped. A dropped excerpt leaves a promise to receipt-clock timeout rather than inventing bytes.

- [ ] **Step 3: Implement the sealing predicate and deadline.** The following predicate is complete; `HasPendingExcerpt` checks the request slot and bounded retained dispatch slots only:

```cpp
static bool CanSeal(const FVaRestInspectorHttpRecord& Row)
{
    return Row.bTerminalKnown && !Row.HasPendingExcerpt();
}
```

On the first actual Completed, set `CompletedObservedAt=Clock->Now()` once. At each bounded expiry batch, if `Now>=CompletedObservedAt+5.0`, mark all still-pending slots TimedOut. A duplicate event never extends the deadline. Use a deadline min-heap indexed by admitted row, at mostMaxRecords entries, one entry per terminal row; remove on seal/eviction/Clear. Examine at most256 expirations/tick; an overdue item may be resolved on a later tick under load, so5 seconds is eligibility, not a hard scheduler latency guarantee. Rows lacking actual completion have no completion deadline. Once CanSeal is true, publish const and never change the record again. Backlinks remain external.

- [ ] **Step 4: Snapshot transaction.** First measure record-handle array, copied mutable rows/detail arrays, copied stats/link views and shared-owner reference costs with checked arithmetic; reserve the complete snapshot charge and one of four snapshot-controller slots (held through final weak release). If any allocation cannot be admitted, return false with Out unchanged. Allocate/populate without callbacks and publish Out only when complete. Sealed rows share immutable owners; unsealed rows clone their mutable header/slots/rings and copy immutable packet backing under fresh credit. Never expose a live mutable row through const-casting. Every observable snapshot payload/backing component releases its ticket after actual free, which may be on a worker. Controller credit/slot instead releases with final-weak stored-deleter destruction; subsequent hidden operator-delete/free is excluded by decision8, not observed by this adapter. ForEachRecord borrows views without building a snapshot and refuses nested enumeration if it cannot honor its single active enumeration contract; requested mutations are deferred until enumeration exits.

- [ ] **Step 5: Demonstrate failure sensitivity.** Seal immediately at Completed: unchanged late-excerpt tests fail. Share an unsealed record object instead of cloning: snapshot isolation fails after a new dispatch. Release snapshot credit on store eviction while an external reader remains: retained-byte assertion fails. Restore each mutant before the next graph/build and run `VaRest.Inspector.Store.Sealing`/`.SnapshotIsolation` unchanged. Ensure test cleanup releases snapshots before checking final account zero.

### Task 8: Complete hard-budget eviction and all auxiliary accounting

**Files:** Budget.h/.cpp (modify, introduced Task4), BudgetTest.cpp (new), Store.h/.cpp, Snapshot.h/.cpp, Records.h.

- [ ] **Step 1: Add `VaRest.Inspector.Store.Budget` with independently small count and byte limits.** Fill pinned rows to MaxRecords, refuse another, complete one, then admit another by FIFO eviction. Separately leave count slack but saturate bytes with allocated-capacity excerpts; admission must fail/evict on bytes. Retain a snapshot across row eviction and Clear: its charged bytes and snapshot slot remain live. Repeat Clear/attach/pause/resume while retaining four old snapshots: the fifth fails and the aggregate ledger never resets. Feed many unique clients/keys/QueueIds/links/markers and prove all auxiliary counts remain at their global limits or below. With an old completed row and a detailed dispatched long poll, pressure evicts the completed row before changing any active detail. Repeat with a retained snapshot keeping completed bytes alive: only eligible never-dispatched pinned headers/excerpts may then be trimmed; dispatched long-poll detail stays intact and admission may fail.

- [ ] **Step 2: Implement allocator-aware accounting.** An allocation request uses FAllocationAdapter::Bound(Request,Alignment), a source-supported conservative upper bound for the audited allocator/build, before admission; generic QuantizeSize alone is not such a guarantee. Verify actual FMemory::GetAllocSize after allocation against that bound. Container storage uses explicitly reserved capacity with no implicit growth outside the accounting adapter. Unsupported size/alignment/family refuses before allocation. An actual capacity exceeding the certified bound is a development invariant/capability failure: free the allocation, disable that capability and report the violated bound; do not conceal a transient breach by reserving a delta afterward. The exact controller≤1024/≤16/4096 proof and calibration are below; every other allocation-size path must likewise have its verified Bound implementation or refuse visibly. Include object storage, shared controllers, owner/index/ring blocks, heap capacities, text terminators/alignment and packet/excerpt capacities. Include the bounded account/diagnostics machinery in the fixed startup charge. Do not call a requested-byte sum “allocated bytes”.

Task3 defines `LLM_DEFINE_TAG(VaRest_Inspector)` in Allocation.cpp and `LLM_DECLARE_TAG(VaRest_Inspector)` in Allocation.h (include HAL/LowLevelMemTracker.h); Task4 Budget.h includes Allocation.h. Thus Task3 allocation code already links before Budget exists; wrap Inspector-owned backing, node, controller, row and snapshot allocations in LLM_SCOPE_BYTAG(VaRest_Inspector). Core-owned excerpts remain under VaRest_Trace and are charged by retained reference capacity here. I6 owns heap/LLM benchmark comparison. Use one store allocation ledger across Clear and reader lifetimes; do not create a fresh independent full budget while old snapshots retain memory. Separate payload/backing charge from shared-controller charge. The complete UE custom-deleter adapter below releases payload only in operator() after destruction/free; controller charge and snapshot slot survive in the stored deleter until its actual destruction at final weak/controller deletion. Never release controller credit at last strong release. Use exact controller type size/alignment, not an unspecified overhead. Every Inspector-created shared owner that can be weakly retained uses this adapter; record read views expose no shared record owner, so clients cannot add a separate unbounded weak domain. Account roots own no child controller and cannot form a self-retaining cycle. The same rule covers the MPSC node/sentinel allocator. Engine allocator metadata/fragmentation, source global objects and the precise decision8 hidden controller free tail remain outside this payload/retained-owner ledger and are reported as such. Queue node/sentinel backing is observable and stays charged until its allocator Free returns; the controller-only exclusion never applies to that backing.

- [ ] **Step 3: Use admission-first eviction.** Determine needed row count/bytes; search the arrival ring from oldest to newest for non-pinned candidates. First evict terminal/sealed/capture-interrupted rows and markers in the one global arrival FIFO, skipping pinned bases. Never mutate a sealed record to trim it; release its store handle as a unit. Only after a finite FIFO pass cannot release the required capacity may the optional-detail fallback visit oldest pinned rows that have NEVER dispatched (bEverDispatched=false, including conservative unknown-history rows excluded from eligibility). Trim only their independently owned header/excerpt allocations, preserving base identity, terminal-reservation scalars and outstanding promise identities; mark details Evicted. Do not trim already-dispatched long polls or expand fallback to their notes/timelines. A mixed flat Started packet containing headers and other metadata cannot be dropped wholesale as “headers”: replace it with an independently pre-admitted compact packet if capacity permits, otherwise skip that header trim and try a separate excerpt or refuse. No over-budget scratch allocation is allowed to make trimming work. Snapshot-held owners may keep all evicted/trimmed bytes charged; then refuse admission visibly. Count-only pressure cannot be relieved by optional-detail trimming. Remove mutable indices, expiry entries and owned optional details; external snapshot owners may keep the bytes charged, so continue searching or refuse if eviction did not release enough. A finite pass over at mostMaxRecords candidates is permitted per new row; ticker work remains bounded by normalizedMaxRecords×256 apply attempts, not an O(1) latency claim. Markers share the arrival count/byte budget and can be omitted while the fixed overflow counters remain visible.

- [ ] **Step 4: Bound every side table globally.** Use fixed-capacity open-addressed indices with load≤0.5 and explicit empty/tombstone state, pre-reserving their exact slot blocks. Ring slots and hash slots are counted separately. Scale private capacities down until startup fixed allocation≤Budget/4; require at least one HTTP row and the diagnostic aggregate, otherwise initialization fails visibly with capture off. Deletion cannot accumulate additional heap tombstones. Retention tombstones use4096 ids maximum and FIFO overwrite; after expiry, a late unknown event may be admitted as Partial, so no eternal dedup promise is claimed. Bound client tables1024, links4096, stats1024 and snapshots4 independently of row count. The single overflow stats bucket is included, not allocated once per refused client. Mutable links cannot keep a record alive through a strong pointer cycle.

- [ ] **Step 5: Calculate and assert finite work/memory.** With MaxRecords≤65536, row index slots≤131072; detailed dispatch slots≤16×admittedRows, timeline slots≤128×admittedRows, notes≤64×admittedRows, with byte admission often limiting these first. Retained snapshot-controller count≤4 across all generations, including expired weak-only snapshots; already-destroyed controllers in the excluded hidden free tail are outside that count, with no aggregate physical bound under arbitrary final-release thread suspension. Global auxiliary maxima are1024 clients+1024 groups+4096 links+4096 tombstone ids; their indices at load0.5 have finite independently charged blocks. Packet sizing touches≤256headers+32warnings and at most1MiB metadata; each string≤16384TCHAR. Queue wrapper count≤262144 and common pending capacity≤256MiB. The native ledger permits at most1GiB store charge at the normalization ceiling; defaults remain64/8MiB. These are counts/capacities, not a claim that all maxima fit simultaneously.

- [ ] **Step 6: Meaningful eviction/accounting mutants.** Evict a pinned oldest row; ignore excerpt allocated capacity; reset the ledger during Clear; omit globally bounded client accounting. Each must fail the unchanged owning assertion, with normal graph/build per version. Restore all source/rules, prove account zero after the last reader release and run `VaRest.Inspector.Store.Budget` plus QueueAdmission. Do not weaken bounds to make the tests pass.

### Task 9: Keep links, markers and process context outside sealed records

**Files:** Links.h/.cpp, LinksTest.cpp, Store.h/.cpp, Records.h.

- [ ] **Step 1: Add `VaRest.Inspector.Store.LinksAndMarkers`.** Complete and seal refresh request10, then observe parked/replayed request11 referencing AuthRefreshId10: request10's record bytes/revision remain unchanged while a new backlink appears. Evict10, retain11: the link is marked target-evicted. Replay queue GUIDq from original12 into new13; clear/restart simulation with OriginalRequestId0 preserves GUID but creates no false cross-process id link. Feed Connectivity/queue-expired/capture-overflow markers with CauseRequestId and real source times. Assert marker/HTTP arrival interleaving and bounded link overwrite.

- [ ] **Step 2: Implement separate finite link ownership.** Key auth edges by real source/targetRequestId; queue edges retain redacted QueueId and optional OriginalRequestId. Store only ids and ticketed string owners, no record pointers. Maintain forward/backlink indices within the4096edge/64target caps; removing an oldest edge updates both indices atomically on GT. If the target row is absent, its link has unknown/evicted availability, not a fabricated record. A bounded tombstone can distinguish known-evicted from never-observed; expiration returns availability to unknown. Record revisions change only for record content, global service revision changes when links change.

- [ ] **Step 3: Retain real context without implementing M5.** Initialize scope0 label Process. Apply existing Scope/Client trace events as metadata only; the current HTTP source reports scope0. Ignore GameInstance pointers and never invent PIE labels. Known client labels update their bounded table; closed metadata can outlive live clients only within that table and budget. For missing/overflow clients, preserve the numeric ClientId on the HTTP row with an unknown label. Do not merge unrelated RequestIds when their client metadata was refused. Inspector CaptureOverflow markers are internally tagged marker rows, not a new EVaRestTraceKind or FVaRestTrace::Emit call.

- [ ] **Step 4: Preserve cap behavior.** When links or marker metadata cannot be admitted, increment omission counters in the fixed diagnostics aggregate. Never mutate a sealed request to append a backlink or embed a growing list. Clear resets links/markers/tombstones and statistics in a single generation transition; externally retained snapshots keep their own values.

- [ ] **Step 5: Behavioral RED/restoration.** Append the backlink to the sealed record: the byte/revision snapshot equality assertion fails. Resolve OriginalRequestId0 by numeric guessing: cross-process link assertion fails. Restore, use a fresh normal action graph and run `VaRest.Inspector.Store.LinksAndMarkers`.

### Task 10: Accumulate bounded statistics independent of retained history

**Files:** Stats.h (public), Stats.cpp, StatsTest.cpp, Store.h/.cpp. Add a private stats header only if needed; that exact new path is `Private/VaRestInspectorStatsPrivate.h` and belongs to this task.

- [ ] **Step 1: Add `VaRest.Inspector.Stats.Http` and `.Bounds`.** Feed known successes/errors/queued/canceled requests, retries/auth replay/cache304/stale, wire byte totals and completed durations. Explicit controls:503/Backoff/200 gives retry1-auth0;401/SessionRefresh/200 with Attempt2 gives retry0-auth1;zero-gap/AuthRefreshId0 SessionRefresh gives auth1;custom same-Attempt redispatch with GapNone gives neither. Assert these again after row eviction. Complete Timeout and Decode errors plus success, queued and canceled controls; verify typed errors-by-kind sum equals actual observed Error outcomes, including the overflow group, and Clear resets each bucket. Evict every history row and verify accumulated totals/histogram remain. Clear and verify reset. Supply endpoint FName, verb+template and verb+host fallback cases; query/path tokens must not create unique fallback keys. Feed257 keys for one client and >1024 global client/key combinations; count sums still match the single overflow aggregate, with bounded key storage and one finite histogram per admitted group.

- [ ] **Step 2: Define public read-only stats values.** `FVaRestInspectorHttpStats` contains observed Started/Completed/Success/Error/Queued/Canceled counts; `FVaRestInspectorErrorCounts ErrorsByKind` (complete typed definition below); physical dispatches/retries/auth-replays; cache-hit/disk/revalidated/stale/queued/replayed counts; wireBytesUp/Down; finite known-duration count/sum/min/max and histogram; partial/incomplete/dropped counts. Distinguish observed counts from inferred complete-request totals. Counters saturate atMAX_uint64 and expose bSaturated; signed byte inputs below0 contribute0 and flag unknown. Only actual Outcome==Error increments Error and exactly one ErrorsByKind bucket selected from Completed.Error.Kind; Canceled/Queued/Success never increment these even if malformed payloads contain a non-None error kind. None with Error or an unknown/future enum value goes to Unknown; all counters saturate independently and set bSaturated. Snapshot copying and the one overflow aggregate include every error-kind bucket. Apply terminal totals once per admitted row's actual Completed; duplicate completion cannot double-count while row/tombstone remains. No eternal dedup beyond bounded history/tombstones is promised after capture loss.

- [ ] **Step 3: Use deterministic keys and fixed histogram bins.** Key scope0+realClientId+Endpoint when Endpoint is non-None; otherwise redacted Verb+PathTemplate; otherwise Verb+redacted URL host. Extract host from the already redacted bounded URL; never intern it as FName. At most256 named groups per client and1024 global groups including the one process overflow aggregate. Failed key allocation routes scalars to that aggregate without retaining the rejected key. Store bounded immutable key strings charged to the store account.

Histogram uses a fixed array of512 uint64 bins plus count/sum/min/max: zero in bin0; positive seconds map `1+floor(log(value/1e-6)/log(1.1))`, clamped1..510, with bin511 for values above represented range/nonfinite excluded from quantiles and counted separately. Quantile returns the geometric midpoint of the selected interval, clamps to observed min/max, and reports unknown when no finite samples. The midpoint multiplicative error of a width1.1 interval is at mostsqrt(1.1)-1≈4.89%; below1µs and upper saturation are explicitly outside that relative-error claim. Keep a finite underflow counter/bin and report its range, rather than claiming5% there. Positive values below1µs go to a dedicated underflow counter rather than silently claiming bin1 precision. Histograms consume4096bytes plus scalars each before key/index costs:1024 groups alone require at least4MiB, so the byte budget can admit fewer than the count cap. P50/P95/P99 are derived, not independently accumulated.

- [ ] **Step 4: Derive physical counts from actual dispatch identities.** Classify each observed distinct physical dispatch once, in this precedence: GapKind==SessionRefresh increments AuthReplays (even GapSeconds0 and AuthRefreshId0); otherwise GapKind==Backoff increments Retries. Do not classify by Attempt progression: real auth replay advances to Attempt2, while custom same-Attempt redispatch without either explicit gap label increments neither. RetryScheduled notes count scheduled-retry diagnostics only, not executed retries: cancellation during a backoff may prevent reissue. If a dispatch lacks explicit classification because its metadata was lost, mark classification unknown instead of deriving it from a note/Attempt. Retained per-row dispatch identities prevent duplicates within bounded history; no unseen retry is invented. Do not treat queued/offline Attempt0 as a network dispatch or include durable queue residence in request latency. Cache hit with zero wire bytes still counts its actual delivered size separately only if a dedicated delivered-size total is exposed; do not add BodyBytes to wireBytesDown. Incomplete capture rows increment incomplete counters and never fake a success/error.

- [ ] **Step 5: Kill history-dependent and unbounded-key mutants.** Recompute stats from retained rows: post-eviction totals fail. Remove the global key cap while keeping256/client: many-client bounds fail. Substitute linear-bin center or wrong logarithm: deterministic percentile relative-error assertions fail for in-range values. Restore exact source and run guarded normal build plus `VaRest.Inspector.Stats`. No UI tab is included.
### Task 11: Connect capture, bounded recovery and supported shutdown

**Files:** Sink.h/.cpp, Service.h/.cpp, LifecycleTest.cpp, Module.cpp, public facade; Queue.h/.cpp for the two-lane integration already required by its contract. All private test hooks compile only with WITH_DEV_AUTOMATION_TESTS. No hook executes application code from the sink.

**Service methods:** `Start(const FCapturePolicy&)`, `Pause()`, `Clear()`, `Tick()`, `Shutdown()`, `ApplyAttached(const FVaRestTraceSnapshot&)`, `RequestControl(EControl)`, `FlushDeferredControl()`. The service owns the mutable store, receipt clock, active sink, ticker/delegate handles and deferred-control state. The sink owns a generation number, immutable policy, closed flag and shared queue/account only. OnAttached temporarily reaches the service through a GT-only weak attachment callback that is removed before registry publication; OnEvent has no service/module/store callback or raw pointer. GetDetail returns the immutable policy with Metadata enabled, Headers/Bodies conditional, FrameCap0 and the configured HTTP excerpt caps.

- [ ] **Step 1: Write `VaRest.Inspector.Capture.Lifecycle` before connecting startup.** Isolate the trace registry with the public scoped isolation helper and restore it LIFO. Verify interactive/unattended/packaged defaults and explicit command-line overrides through a pure policy parser; do not rewrite the process command line in a live suite. Start with one held request: OnAttached creates Partial synchronously but OnChanged waits until the next batch. Pause, complete it while paused, resume: missing row becomes terminal-details-unavailable/CaptureInterrupted, not success/cancel. Clear during capture gives a new Partial snapshot for an actually live request; a held old-list callback cannot repopulate it. Double Start/Pause/Clear/Shutdown are safe and only one sink/ticker exists.

- [ ] **Step 2: Finish the shared queue's two lanes.** Its GT-only metadata lane and worker-capable excerpt lane share the same pending count/byte account, fixed diagnostics and generation closure. They have independent queue heads/tails so a producer held between MPSC head exchange and next-link publication cannot obstruct the metadata watermark. Only successfully published GT metadata gets a monotonically increasing generation-local metadata ordinal; drops have no ordinal. Snapshot items are applied directly and do not advance it. Excerpts retain source Seq but do not affect metadata ordinal. Verify each supported non-Excerpt producer uses GT in the pinned TracePoints/registry/source paths; development checks enforce the contract, and unsupported off-GT metadata is dropped/counted rather than allowed to race the ordinal. No mutex around application callbacks is needed.

Per Tick consume at most192 metadata items and64 excerpt items, including rejected/drop-applied items in these work counts. Either lane may use the other's unused allowance up to256 total, but always offer metadata its192 slots first during recovery. A transient empty excerpt lane must not block metadata or imply the generation is quiescent. Fixed queue wrapper/sentinel allocation for each new generation is itself admitted against the common pending account before Start; if old externally retained generations consume that allowance, Start visibly fails/remains paused. There is no unbounded service-owned retired-generation list. Old sink-list owners keep their own queue alive until their actual last release.

- [ ] **Step 3: Implement fixed-watermark overflow recovery.** Keep the finite scalar state from the corrected scheduler kernel: HandledLossEpoch/HandledOrphanEpoch, TargetLossEpoch/TargetOrphanEpoch, RecoveryWatermark, LastConsumedMetadataOrdinal, OrphanEpoch, bRecoveryPending, LastRecoveryAt. OrphanEpoch is a saturating GT-only scalar advanced when any unknown-id nonterminal row is admitted or an interrupted row receives a new orphan event; it is independent of queue producer loss. The atomic queue loss epoch increments on every actual loss, including dropped Completed and scalar metadata fallback; uint64 saturation becomes a sticky dirty state, with bounded repeated recoveries rather than wrap. Recovery is metadata reconciliation; missing excerpts still use their promise timeout.

When either producer LossEpoch or GT OrphanEpoch is unseen and no recovery is pending, capture both target epochs and `RecoveryWatermark=LastPublishedMetadataOrdinal`. Freeze W: continuously arriving producers never extend it. Drain metadata until LastConsumedMetadataOrdinal≥W; consuming an admitted scalar-fallback descriptor counts as consumption even if store admission fails. Because only successful publication receives an ordinal, a dropped event cannot leave an unfillable ordinal hole. On the first eligible Tick after that condition and `Clock->Now()-LastRecoveryAt>=0.250`, call BuildSnapshot once. Initial LastRecoveryAt is -TNumericLimits<double>::Max() so the first recovery is eligible immediately. At most one recovery snapshot is allowed per Tick.

BuildSnapshot and its complete single walk are synchronous local upstream transients. Increment a store reconciliation stamp; for each source Started mark an already retained open row through the bounded RequestId index and, where appropriate, admit a missing Partial row. Iterate retained mutable/open rows once after the source walk: those not marked and with BirthMetadataOrdinal≤W become CaptureInterrupted with terminal details unavailable and are unpinned. Do not fabricate Completed, status, cancellation, error or duration. Resolve their pending excerpt promises as CaptureStopped/unknown, and leave them unsealed so a later already queued actual Completed can coherently enrich them. A real Completed may then seal normally after its actual promises resolve; never amend a genuinely sealed record.

Release the entire source snapshot before returning. A source request still live at snapshot time stays pinned, even if its earlier metadata was lost. Rows born from metadata afterW are outside this recovery pass; their later lost completion necessarily advances a newer epoch. Only mark the captured pair of epochs handled; if either atomic LossEpoch or GT OrphanEpoch is newer, arm another fixed W even when no further events arrive. Direct attach rows always have explicit birth eligibility; every orphan that arrives after a completed recovery increments OrphanEpoch and therefore schedules another pass without waiting for producer loss. The next recovery waits at least250ms on the injected receipt clock. Coalesce all losses observed before the next arm; no per-loss snapshot or missing-id set. No source BuildSnapshot pagination/query API is added.

This algorithm bounds Inspector-owned state and repeated recovery frequency. It does **not** provide an absolute GT-tick bound: upstream BuildSnapshot allocation and O(live clients+requests) traversal, plus one bounded retained-row scan, remain explicit costs for I6. One source snapshot per affected row is forbidden.

- [ ] **Step 4: Prove overflow order and progress.** Add `VaRest.Inspector.Store.OverflowRecovery` with each deterministic case: (a) Started queued then Completed dropped before any drain; (b) pre-W Started and a post-W dropped Completed; (c) continuously appended successful metadata while W remains fixed; (d) actual Completed queued behind W, snapshot absence first unpins and later actual event enriches; (e) two losses straddling recovery and no subsequent events, virtual clock249ms/250ms proves delayed automatic rearm; (f) worker excerpt publisher held at the Inspector boundary immediately before Enqueue after reservation, while another permitted excerpt publishes and GT metadata reaches W and reconciles; (g) pause/clear/resume while a recovery is armed; (h) all rows pinned and a refused Started, with no suppressed-id growth; (i) scalar-only Completed fallback preserves kind/id/outcome and resolves the correct row; (j) snapshot-born live row followed by dropped Completed; (k) lost Started, accepted Ended-created Partial, dropped Completed; (l) all metadata lost, recovery completes, held excerpt arrives afterward, no later events; (m) genuinely live orphan absent Started but present in source snapshot promotes to pinned, then real completion resolves it. Cases j–l must become/remain unpinned unavailable without invented Success/Error/Canceled; m proves liveness is not discarded indiscriminately. Assert no fabricated transport result, no permanent pin after known absence, at most one snapshot/tick and minimum250ms separation. Use barriers and counters, never sleeps as proof.

- [ ] **Step 5: Implement pause/resume/Clear and limit lifetime.** Pause first closes the current generation's admission, then RemoveSink, then discards its currently drainable queued items; later old callbacks can only touch that closed generation. Existing active rows become capture-interrupted; pending excerpt promises become CaptureStopped. A generation close never invents transport completion. Resume creates a new admitted generation, calls BeginGeneration to rebase every retained mutable row to the new consumed ordinal (normally0) BEFORE live-presence reconciliation, synchronously reconciles its attached source snapshot and republishes one sink. Terminal-known/sealed rows retain their terminal truth and are not rebased or promoted. Old rows still live at resume can continue as Partial; absent formerly open rows are unpinned with unknown terminal details. Actual source metadata after completion remains forbidden, except a legitimately queued real Completed being applied following recovery.

Clear rotates/closes the generation, removes mutable rows/stats/links/tombstones/expiry state, increments service revision, and if recording applies a fresh partial snapshot. Retained external snapshot owners remain charged to the **same store ledger**; pending generation owners remain charged to the **same queue ledger**. Changed store/queue limits are visibly PendingLimits until a subsequent explicit Clear/recreation can release/recreate their account at quiescence. During this explicit operation first release the service-owned empty-store fixed blocks and closed active-generation baseline, then test whether any external strong/weak controller or publisher tickets remain; a permanently allocated empty index must not make quiescence impossible. Releasing an old reader on a worker never automatically changes limits. If old readers prevent quiescence, Clear uses the existing limits and account. Test lower-budget-below-held-bytes, repeated Clear, release, then explicit Clear; preserve high-water and cumulative-loss evidence separately from per-capture cleared counters so recreation cannot hide a cap violation. No impossible live shrink is attempted. Quiescence here means no retained owner or admitted payload/publisher ticket; it does not observe excluded hidden controller free tails. Lower limits never apply while an expired weak controller is retained, but decision8 permits account recreation after stored-deleter destruction even if an external thread is suspended before the hidden operator-delete/free completes.

- [ ] **Step 6: Batch notifications and defer reentrant controls.** `Tick` mutates consistently, then broadcasts OnChanged once if revision changed, outside queue/account locks. During broadcast or ForEachRecord set a scoped dispatch depth. Clear/Pause/Start requests only set bounded pending flags; Shutdown takes precedence, then Clear, then the last requested capture state. On unwinding the outermost dispatch, apply pending controls once; avoid recursive notification, enqueue the resulting dirty notification for a future tick. Tests register observers that remove themselves, take a snapshot, request Clear then Start, and request Shutdown. No visitor or observer executes while indices are half-updated.

- [ ] **Step 7: Wire supported module lifetime.** Startup normalizes settings, creates the service on GT, registers PreExit with a weak service capture, then starts only if policy enables it. Ticker callback captures a thread-safe weak service ref and returns false if it cannot pin. Shutdown sets the module shutdown flag first, removes ticker and PreExit handle, closes/removes the active sink, resolves/discards mutable state, flushes deferred control without broadcasting and releases service ownership. Make it idempotent and set SupportsDynamicReloading false. Normal engine Shutdown keeps module DLL code mapped; a delayed registry-list owner may still destroy its closed sink/queue later on a worker. Do not wait for foreign list references or claim arbitrary forced-unload safety. No asynchronous callback uses module statics that were destroyed by Shutdown.

- [ ] **Step 8: Deterministic lifetime/mutation evidence.** Hold an old registry list and an admitted worker callback; Pause/Clear/Shutdown; release worker and list after facade destruction. Counters prove no post-close copy/admission, no store access, and eventual pending account zero after actual owner release. All failure exits release barriers before waiting/draining. Mutants: hold raw store pointer in sink; reset pending ledger on resume; extend W every tick; remove rearm when no new events; add the faulty Inspector-owned predicate `PendingExcerptReservations.load() == 0` to recovery readiness. The boundary-held excerpt has already incremented that counter, so the unchanged assertion “recovery count advances and absent row unpins before publisher release” fails under that predicate. Always release the barrier in scope cleanup before draining/joining, including failed assertions. Unchanged tests must expose stale writes, bound breach, non-progress or missing recovery respectively. Do not claim that a single-lane mutant is dynamically blocked at the uninstrumented internal engine exchange/link gap; its independence argument remains source analysis. Restore exact files, guarded normal build, then Lifecycle and OverflowRecovery prefixes.

### Task 12: Verify real HTTP producers through the capture service

**Files:** Inspector TestUtil.h/.cpp and CaptureTest.cpp; `Source/VaRestTests/Private/VaRestInspectorDecodeTest.cpp`; add `VaRestInspector` to VaRestTests' private dependencies. No reflection fixture is declared inside an Inspector-private cross-module include. Reuse an existing VaRestTests reflected fixture where compatible; if a new generated fixture is necessary, its exact file is `Source/VaRestTests/Private/VaRestInspectorTestTypes.h` and is owned by this task.

- [ ] **Step 1: Build a failure-safe capture fixture.** It owns FScopedSinkIsolation, FTestRig, a virtual receipt clock independent from the client's clock, a private FService with small normalized limits, explicit retained snapshots, and any test-owned barriers. A helper `DrainCapture()` calls Service.Tick from the test body only, never recursively inside a ticker/GT task. Every callback captures shared outcome state rather than stack references. Destructor releases barriers, cancels live request handles, pumps cancellation, flushes the HTTP redaction pipe, drains capture, closes the service and releases snapshots before checking account balance. Persistence cases use FScopedPersistenceTest and its owned root/drain, never global user cache directories.

- [ ] **Step 2: Add source-to-store goldens under `VaRest.Inspector.Capture.Http`.** Use real FVaRestClient/FVaRestFakeTransport APIs, real core emissions and the real sink. For each request assert the record RequestId equals its returned handle id. Required matrix: success; 503→backoff→200; auth401 refresh/replay and proactive parked peer; canceled at dispatch; cancellation before dispatch; envelope failure; decode warnings; fresh memory and disk cache hit; network304 delivered200; known-offline stale and attempted stale; queued terminal request with no callback re-fire; replay with new id/queue GUID/original link; local pre-attempt refusal; late attachment mid-request and after start with tracing previously off; marker cause link; duplicate same-Attempt ends both orders; sequential same-attempt pipe/decode excerpts distinguished by DispatchId. Existing core tests keep their own stage-policy assertions; this task asserts what the Inspector stores.

Some auth/offline producer seams are deliberately private. For scenarios that cannot be generated through the public fake client configuration without reproducing private policy, reuse recorded redacted event fixtures produced/validated by the owning core golden and feed their trace events through `FVaRestTrace::Emit`; label these **consumer fixtures**, not end-to-end stage tests. Keep at least the normal success/retry/cache/queue public-client flows end-to-end. No test includes VaRestHttp/Private headers from Inspector and no exported test-only auth internals are added merely for this task. Exact parent-reviewed fixture classification is part of the acceptance report.

- [ ] **Step 3: Own reflected decode assertions in VaRestTests.** Exercise a real `As<ReflectedType>` result with one mismatch and a root-envelope condition. Assert core DecodeStartAt/EndAt are forwarded, result type name is retained, warnings carry exact path and count, data path/predicate expected/actual/code/message survive redacted, and decode did not occur in the GT sink/store. The decode fixture includes public Inspector/HTTP testing interfaces; where a private service is needed, test through the real native facade with controlled start/clear and scoped registry restoration. Register `VaRest.Inspector.Capture.Decode` and ensure its module dependency closes entirely within plugin actions/UHT.

- [ ] **Step 4: Keep order/source assertions explicit.** A source fresh cache hit may be Started,Completed only; no required AttemptDispatched/Ended. Queued is not a transport success or a pending future original callback. Async disk persistence emits no late CacheStored. Histories may be Partial after loss/refusal; the golden input without loss must be complete. Ended for a stale dispatch may follow a later Dispatched but precedes Completed; Excerpt may follow Completed. Socket kinds/source are ignored without constructing unavailable payload variants.

- [ ] **Step 5: RED/GREEN with owning behavior.** Temporarily discard real OnAttached rows, substitute Error.HttpStatus for final response status, and route excerpts by Attempt. Each relevant unchanged golden fails. Restore exact bytes and run the targeted Capture prefix plus the specific existing core source goldens touched by Task1. All native versions use new normal action inspection. Do not duplicate the final full suite here.

### Task 13: Prove HTTP redaction across stored and snapshotted surfaces

**Files:** RedactionTest.cpp and TestUtil.h/.cpp only for permanent changes. Any deliberate core redaction mutant is a temporary, root-dispatched exact file/version scope and must be restored before normal development continues.

- [ ] **Step 1: Add `VaRest.Inspector.Redaction.Http`.** Use distinct ≥8byte ordinary canaries in Authorization/Bearer, Cookie/Set-Cookie, custom sensitive header, query parameter, encoded/path text, request JSON string, response JSON string, form/multipart fields, malformed/truncated JSON and known auth/session secret values echoed in errors, decode warnings, envelope fields and effective URL. Include source/default/current/applied secret union rotation, sensitive names and a benign control value. Capture request/response excerpts and headers, then scan every owned packet text range, excerpt byte range, snapshot view, marker, link key and stats key. Secret absence alone is insufficient: assert benign controls and expected `***`/redacted flags so a completely empty capture cannot pass.

- [ ] **Step 2: Exercise options and unavailable content.** Headers=false must retain no header names/values in the Inspector even when another sink requested Headers. Bodies=false retains no byte reference/preview while other sinks request Bodies. Binary cap0 gives descriptor/no raw binary; the pinned HTTP producer remains descriptor-only even with nonzero BinaryCap. Test both values and assert no raw binary acquisition; no new binary-preview producer is authorized. Stream preview is unavailable. Cached final preview remains NoProducer. Durable replay request preview is absent **before copy/announcement**, including deliberately short secret strings below the ordinary8byte boundary; response preview follows ordinary core redaction. Do not turn the replay exclusion into a weaker post-copy scan.

- [ ] **Step 3: Add allocation/copy assertions.** Inspect the existing HTTP TraceExcerptBytesCopied counter around replay, and Inspector OwnedTextCopied/ExcerptReferencesAcquired around denied queue admission. The no-copy promise applies to body preview acquisition, not to unrelated redacted metadata. Force long metadata over the codec bound and prove the scalar fallback retains no truncated raw secret. The sink never calls the redactor itself: the existing RedactorCalls delta must be attributable only to core producers, not doubled by capture.

- [ ] **Step 4: Execute meaningful R1/R2/R11/R13 restoration proofs.** Under separately approved temporary exact file scopes: in `Source/VaRestHttp/Private/VaRestRedactor.cpp:825` pass the custom sensitive header value raw from RedactHeaders (R1); in `Source/VaRestJson/Private/VaRestJsonRedact.cpp:59` remove the RawNumber cut-at-end early stop so a cap-boundary token is written (R2); in that Json file `:290` replace substring matching with exact matching so camelCase refreshToken leaks (R11); in Redactor.cpp `:463` bypass ScrubInPlace so a known bearer value in a path segment leaks (R13). Reconcile exact lines against current accepted source before mutating; preserve the target method/behavior. Each unchanged Inspector test must fail with the target canary/cut-prefix assertion, not a generic build error. Additionally bypass replay request-preview early suppression in `Source/VaRestHttp/Private/VaRestTraceExcerpts.cpp::PrepareRequest` as a separate privacy mutant and require forbidden promise/copy assertions to fail. Record exact modified lines and original hashes before executing. No unrelated auth/queue behavior may be changed to make a mutant reachable. Restore exact hashes after each mutant, inspect a fresh normal graph, rebuild and rerun the same test unchanged. Secrets used here are synthetic test strings, never real credentials.

- [ ] **Step 5: Run the final HTTP redaction subset once.** Run Inspector.Redaction, owning core header/excerpt/replay-redaction goldens and VaRestTests decode coverage after full restoration. Record unavailable/malformed-content behavior explicitly. Exports/cURL/UI clipboard/persistence are not present in I1 and receive no false redaction-pass claim; their sink paths are later-milestone work.

### Task 14: Verify no-capture cost, details isolation and new macro branches

**Files:** CostsTest.cpp, ModuleTest.cpp, LifecycleTest.cpp. No permanent core counter API is added if existing FVaRestTrace::Counters and HTTP testing counters already observe the claim.

- [ ] **Step 1: Add `VaRest.Inspector.Capture.OffCosts`.** With registry isolation and capture disabled, issue1000 fake requests and compare EventsBuilt/RedactorCalls/DelegateBinds/TraceCycles and Inspector allocation/copy counters to the owning zero-trace baseline. Allow other explicitly installed test sinks only in a separate detail-merge case; otherwise their work would invalidate the no-sink proof. Pause has no active sink, no packet copy and no hidden per-frame drain; a ticker retained solely for pending notification/expiry must unregister when no work remains. The compiled-out module creates no service, store, ticker or sink.

- [ ] **Step 2: Add `VaRest.Inspector.Capture.DetailIsolation`.** Install metadata-only Inspector plus a body-enabled recorder and prove Inspector stores no headers/body references outside its own policy while core still serves the recorder. Reverse the policies and remove the recorder: RefreshDetail/default mask correctly drops expensive fields. Test caps, MaxPendingExcerptBytes merge semantics and independent consumer pending limits without changing the core merge algorithm. Removing Inspector cannot remove an unrelated sink or saved isolation list.

- [ ] **Step 3: Prepare conditional tests for the two real new configurations.** Under `VAREST_WITH_TRACE_EXCERPTS=0`, HTTP metadata/Completed.Response/DispatchId capture still works; no Pending promises or raw excerpt owners appear and cache final NoProducer remains explicit. Under `VAREST_WITH_INSPECTOR=0`, UHT settings and native facade link, IsCompiledIn false, SetCapture/Clear are no-ops, revision0, empty snapshot/enumeration, no registered sink/ticker, and no false successful export/BP function. Tests select these branches with preprocessing; they must not silently skip the entire suite. Configuration builds/runs occur in Task16 under the exact guard.

- [ ] **Step 4: Restore-the-bug proof.** Register the sink despite capture disabled and observe EventsBuilt/allocation deltas fail. Retain a body ref despite local Bodies=false and observe reference/byte assertions fail while the other sink control remains intact. Restore exact version, guarded normal build and run OffCosts/DetailIsolation. Microbenchmarks, absolute time targets and LLM baselines remain I6.
### Task 15: Pass strict per-module NoPCH/nonunity and restore normal rules

**Files:** no permanent source changes. Temporary scope is exactly `Source/VaRestHttp/VaRestHttp.Build.cs`, `Source/VaRestInspector/VaRestInspector.Build.cs`, `Source/VaRestTests/VaRestTests.Build.cs`, one module at a time unless the controller explicitly approves a combined closure. VaRestJson rules are included only if a permanent Json fix was accepted in the final implementation; a restored redaction mutant alone is not a permanent change. Host Target.cs, engine rules, global BuildConfiguration and generated files are never edited.

- [ ] **Step 1: Freeze and inventory the final normal version.** Stop owned editors/test runners, record source/rule hashes and git diff, complete all targeted GREEN/restoration evidence. Generate a fresh complete normal editor WriteOutdatedActions graph and inspect it as above. Build only if the closure is exclusively plugin Compile/Link and named metadata; run normal Inspector owning prefixes. Any prohibited PCH/engine/host action is a blocked gate with exact evidence, not permission to alter caches.

- [ ] **Step 2: Strict-build each owned module.** Save exact Build.cs bytes/hash in the ignored task evidence. Add only `PCHUsage = PCHUsageMode.NoPCHs; bUseUnity = false;` to the selected module's constructor. Generate fresh WriteOutdatedActions for this precise version, inspect every rsp/unity/object/link/output, and run the normal Build.sh command only after the guard passes. Other module actions are allowed only when their ownership/necessity is explicitly in the inspected plugin closure; no engine PCH creation or host compilation. A successful link alone does not show each changed translation unit compiled without PCH/unity: inspect compile arguments and source paths. Stop on the first real compile failure and report its owning fix scope to the controller.

- [ ] **Step 3: Restore rules exactly before runtime.** Restore the saved Build.cs bytes even if strict build/test fails; verify hashes/diff and produce another fresh **normal** graph. Run only the allowed normal closure, then `Tools/run_tests.sh --no-build VaRest.Inspector` and directly affected core producer prefixes. Strict artifacts are not accepted as normal runtime evidence. Repeat the sequence per module, without global NoPCH switches or clearing Intermediate/Binaries.

- [ ] **Step 4: Record strict evidence.** Report each module's actual Compile/Link/metadata count, no-PCH/nonunity argument proof, real fixes (if any), guard rejections separately and exact restored hashes. Controller decides any new source fix/review round; this task cannot broaden its lease. Do not infer MSVC/Clang portability from Linux alone. No full suite yet unless a material new concern requires it.

### Task 16: Validate Development, Shipping and meaningful disabled-feature branches

**Files:** no permanent edits. Temporary macro definitions only in the exact consuming plugin Build.cs files approved by the controller, restored after each branch. Do not edit a target/global rule or propagate inconsistent macro values across headers/translation units.

- [ ] **Step 1: Monolithic Linux Development.** Generate a fresh full action graph for `VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject`. Inspect plugin Compile/Link actions plus the exact final host executable link, recursively expanded response inputs, receipt/products and any named `.psym`/`_nodebug` symbol-strip intermediate by its actual link/output role. Permit only that final host Link exception; a host/engine Compile, unrelated link or PCH creation blocks the gate. Build with the same normal target command after approval. No export/runtime-HAR claim is made: I5 owns those functions and its packaged run.

- [ ] **Step 2: Monolithic Linux Shipping.** Repeat the complete graph/input/product inspection for Shipping, including configuration-suffixed symbol products rather than assuming Development filenames. Verify the new module/settings/native facade link with the default VAREST_WITH_INSPECTOR0 and existing excerpts0. The I5 host startup BP/export/console stub check is explicitly deferred because those APIs do not exist in I1 and the host source is outside this closure. The I1 compiled-out native contract is exercised in the editor override below; Shipping is build/link evidence only, clearly labeled. Do not claim Shipping automation ran.

- [ ] **Step 3: Excerpts-off editor branch.** Under a controller-approved exact temporary definition scope covering all relevant consumers, set VAREST_WITH_TRACE_EXCERPTS=0 while Inspector remains enabled. Re-read the existing #ifndef default and ensure a single consistent macro value in all relevant rsp files. Fresh full normal graph→inspection→permitted build; run Module, Capture, Store and Stats branches that remain meaningful plus CostsTest's explicit excerpt-disabled assertions. Tests requiring body bytes select an explicit unavailable branch instead of passing vacuously. Metadata/final response/identity/queue lifetime must still be exercised. Record actual selected tests and counts.

- [ ] **Step 4: Inspector-off editor branch.** Restore the previous macro scope exactly, generate/inspect/build normal as required, then in a separate approved version set VAREST_WITH_INSPECTOR=0 consistently for Inspector and VaRestTests consumers. UHT declarations remain present. Fresh graph→inspection→build; run `VaRest.Inspector.Module` and explicit compiled-out native contract tests, plus existing zero-trace controls. No service/store/ticker/sink is created and Clear/SetCapture/empty snapshot behavior is asserted. Missing expected tests or a zero matched test count is a failure.

- [ ] **Step 5: Restore the final normal configuration.** Restore every temporary Build.cs byte/hash, inspect git diff for only accepted permanent changes, generate/inspect a fresh normal editor graph, build permitted closure and run the normal Inspector prefixes plus source regressions. No macro definition remains in permanent build rules. Preserve each configuration's own log/manifest; never relabel a previous source-version result as the restored final version.

### Task 17: Complete I1 acceptance evidence and hand off

**Files:** only task/acceptance reports in the controller-owned ignored workspace unless a final fix is explicitly dispatched. Repository plan/spec/status changes and any commit remain controller-owned actions. This task does not authorize a remote push or new Beads operation by a worker.

- [ ] **Step 1: Run the final full suite once on the restored normal final source.** Generate/inspect/build if this exact normal version is not already verified after Task16. Run the repository runner's full `VaRest` selection with `--no-build`, owned loopback fixture and owned PID cleanup. Record discovered/passed/failed counts and full log paths. Separate existing startup diagnostics, native failures, harness failures and new Inspector failures. No broad system process kill, no editor during build and no unnecessary rerun of unchanged Python tests.

- [ ] **Step 2: Audit all I1 exits and restoration evidence.** Use the coverage matrix below. Every mandatory R target must have an unchanged-test RED showing the intended behavior, exact restoration hash, fresh normal action-closure evidence and GREEN. Mere source inspection, compile RED, timeout unrelated to the mutant or a test changed between RED/GREEN is insufficient. Account for all unavailable fields and finite-overflow behavior. Report the explicit upstream snapshot exclusion and normal-shutdown-only lifetime boundary; do not conceal them in a performance claim.

- [ ] **Step 3: Request independent whole-plan/implementation review through the controller.** Review complete final diffs, not just the last patch. Review focus includes producer privacy and zero-sink cost, response descriptor before decode, DispatchId through every excerpt path, memory ownership at retained readers, dual-lane generation closure, fixed-watermark recovery, observer/source clocks, stats bounds and all I2–I6 deferrals. Per-task fixes remain bounded to at most five controller-authorized rounds; the whole milestone gets one final reviewed fix wave under the release programme. A new failure that cannot fit those bounds is reported as a blocker, not hidden by relaxing tests.

- [ ] **Step 4: Produce the reviewable final handoff.** Include accepted source pin/final diff, exact source file list, test/strict/monolithic/macro evidence, all restoration hashes, introduced limitations, outstanding issues with owning milestone, and the root's acceptance decision. No worker marks Bead VaRest-zw5.1 complete, commits, updates shared memory or claims acceptance without the controller's dispatch. Release all file/build/runtime leases after cleanup and explicitly report any process or temporary rule still outstanding.

## I1 coverage and deliberate later-milestone boundaries

These are mandatory proof mappings, not claims that tests have already run.

| Requirement / restore target | Owning task and discriminating assertion |
| --- | --- |
| Capture + IN1–17 | Tasks1,4–7,9,11–12: real identity/order, final delivered status, phases, initiator/decode/envelope, source snapshots and process scope |
| R1 capture redaction | Task13: deliberately raw sensitive header reaches a visible nonempty record and unchanged canary scan fails |
| R2 cut-token rule | Task13: cap-boundary JSON token is withheld; deliberately emitting the cut token causes unchanged prefix-canary assertion to fail |
| R3 zero trace cost | Task14: no-sink1000 requests has zero core event/redactor/delegate deltas; unconditional work mutant produces nonzero |
| R4 store byte budget | Task8: byte saturation with count slack; counts-only mutant exceeds the normalized payload/retained-owner charge budget |
| R5 in-flight pinning | Task8: held long poll survives pressure; pinned-eviction mutant removes it and completion loses continuity |
| R6 late attachment | Tasks4,11–12: a held real request is immediately Partial from OnAttached; skipping attachment loses the row/history |
| R8 immutable call-time data | Task7: retain an unsealed snapshot, deliver late excerpt, then read old snapshot; live-sharing mutant exposes later bytes. Export executor is I5 |
| R10 pending byte admission | Task3: stalled drain with count slack rejects before copy/reference; count-only mutant breaches common bytes across both lanes/generations |
| R11 normalized sensitive keys | Task13: camelCase refreshToken/separator variants are redacted; exact-match-only mutant leaks the canary |
| R12 pause unregisters | Tasks11,14: first subsequent frame has no Inspector-induced trace work; flag-only pause mutant still builds events |
| R13 known-secret value scrub | Task13: known bearer value in a path segment is absent; disabling value scrub leaks it, benign path control remains |
| Store/sealing/snapshots | Tasks4,7–9,11: immutable readers, five-second receipt timeout, bounded stats/links/tombstones, Clear and old-generation ownership |
| Stats | Task10: history eviction does not erase totals, global overflow remains finite, in-range log-histogram precision and clear reset |
| Lifecycle/source cost | Task11: held old sink-list refs, worker publication, reentrant controls, bounded epoch recovery; explicit source snapshot traversal exception |
| Macro/UHT/build closure | Tasks2,14–16: unconditional reflected settings, native compiled-out subset, excerpts-off metadata, strict restore and exact monolithic output roles |

I2 owns the HTTP UI/view model/editor tab/menus, initiator navigation, body rendering and copy/Save actions; the specification's I2 copy-as-cURL action remains there, while I5 owns its final Curl exit/export integration. I3 owns W5 socket payloads/rows/frames/snapshots. I4 owns S4 Socket.IO channels/acks. I5 owns HAR/export, console, BP toggle/export, stats tab, coreM5 hub scopes/asset labels and desktop game window, with its packaged export and Shipping host-stub run. I6 owns benchmarks, user documentation and platform passport. No UI/export test is relabeled as an I1 test, and no fake future function is added to make a later exit appear satisfied.

Known I1 limits to carry forward: capture can refuse rows while admitted in-flight rows/readers consume the finite budget; capture loss can produce Partial or terminal-details-unavailable rows; metadata over safety caps becomes visibly omitted; cache-delivered previews have no producer; historical UTC/initiator may be unknown; private uint32 dispatch generation retains its existing practical range; immutable readers can delay changed limits until another explicit Clear; snapshot construction/reconciliation has source-proportional transient allocation/time outside Inspector's ledger; percentile precision has an explicit representable range; normal engine shutdown is supported while arbitrary forced DLL unload is not; controller charges stop at final-weak stored-deleter destruction, with the subsequent hidden free tail excluded (≤4096 bytes per destroyed controller, no aggregate physical bound under arbitrarily suspended final-release threads). None of these authorizes silent raw-data retention, an unbounded fallback map, a new counter generator or an engine/host build workaround.

## Implementation kernels and exact behavioral assertions

These small complete kernels are part of the owning tasks, not a second implementation route. Put them in the named private files (Allocation owns tickets, Packet owns encoding, Queue owns lanes) and use the full field/admission contracts above for their callers. They establish concrete arithmetic, move and timeout behavior before the larger store is connected.

**Task3, Allocation.h/.cpp:** the aggregate count/byte ticket uses the already defined TryReserve primitive and this ownership implementation. The account object itself and queue sentinels are charged by generation creation before publication; an event ticket never owns or resets those baseline charges. Put TryReserve and these types in `VaRest::Inspector::Private`. Counter limits are immutable; account owners never reset Used while tickets remain.

```cpp
struct FPendingAccount
{
    explicit FPendingAccount(int64 InCountLimit, int64 InByteLimit)
        : CountLimit(InCountLimit), ByteLimit(InByteLimit) {}
    const int64 CountLimit;
    const int64 ByteLimit;
    std::atomic<int64> Count{0};
    std::atomic<int64> Bytes{0};
};

class FPendingTicket
{
public:
    FPendingTicket() = default;
    FPendingTicket(const FPendingTicket&) = delete;
    FPendingTicket& operator=(const FPendingTicket&) = delete;
    FPendingTicket(FPendingTicket&& Other) noexcept
        : Account(MoveTemp(Other.Account)), Charge(Other.Charge)
    {
        Other.Charge = 0;
    }
    FPendingTicket& operator=(FPendingTicket&& Other) noexcept
    {
        if (this != &Other)
        {
            Reset();
            Account = MoveTemp(Other.Account);
            Charge = Other.Charge;
            Other.Charge = 0;
        }
        return *this;
    }
    ~FPendingTicket() { Reset(); }

    static TOptional<FPendingTicket> Acquire(
        const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount,
        int64 InCharge)
    {
        if (!TryReserve(InAccount->Count, InAccount->CountLimit, 1)) return {};
        if (!TryReserve(InAccount->Bytes, InAccount->ByteLimit, InCharge))
        {
            InAccount->Count.fetch_sub(1, std::memory_order_acq_rel);
            return {};
        }
        FPendingTicket Ticket;
        Ticket.Account = InAccount;
        Ticket.Charge = InCharge;
        return TOptional<FPendingTicket>(MoveTemp(Ticket));
    }

    void Reset()
    {
        if (Account.IsValid())
        {
            Account->Bytes.fetch_sub(Charge, std::memory_order_acq_rel);
            Account->Count.fetch_sub(1, std::memory_order_acq_rel);
            Charge = 0;
            Account.Reset();
        }
    }
private:
    TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Account;
    int64 Charge = 0;
};
```

The packet backing/excerpt references must be destroyed before its ticket resets. Declare the ticket member before the data-owner members so reverse member destruction releases data first, and explicitly apply that order in move-assignment. A node that becomes TMpscQueue's new sentinel stays covered by the baseline one-sentinel-per-lane charge; the old sentinel freed at dequeue relinquishes the corresponding node capacity. The two lane sentinels and queue objects survive closure until all publishers/last sink-list owners release. Development probes update high-water at successful reservation, before any subsequent enqueue/copy, so a stalled admitted producer is included.

**Task3, QueueTest.cpp:** retain this independent count-slack byte test unchanged during R10. It tests the admission primitive; QueueAdmission additionally tests actual node/backing/excerpt allocator capacities and denied-copy counters as specified above.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorTicketTest,
    "VaRest.Inspector.Store.QueueTicket",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestInspectorTicketTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Inspector::Private;
    auto Account = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8, 100);
    auto First = FPendingTicket::Acquire(Account, 60);
    TestTrue(TEXT("first admitted"), First.IsSet());
    auto Denied = FPendingTicket::Acquire(Account, 41);
    TestFalse(TEXT("byte limit rejects with count slack"), Denied.IsSet());
    TestEqual(TEXT("failed admission rolls back count"), Account->Count.load(), int64(1));
    TestEqual(TEXT("failed admission preserves bytes"), Account->Bytes.load(), int64(60));
    auto Exact = FPendingTicket::Acquire(Account, 40);
    TestTrue(TEXT("exact equality admitted"), Exact.IsSet());
    TestEqual(TEXT("full byte capacity"), Account->Bytes.load(), int64(100));
    if (First.IsSet())
    {
        FPendingTicket Moved(MoveTemp(First.GetValue()));
        First.Reset();
        TestEqual(TEXT("moved owner retains charge"), Account->Bytes.load(), int64(100));
        Moved.Reset();
    }
    TestEqual(TEXT("one retained owner"), Account->Bytes.load(), int64(40));
    Exact.Reset();
    TestEqual(TEXT("all bytes released"), Account->Bytes.load(), int64(0));
    TestEqual(TEXT("all counts released"), Account->Count.load(), int64(0));
    TestFalse(TEXT("negative rejected"), FPendingTicket::Acquire(Account, -1).IsSet());
    TestFalse(TEXT("oversize rejected"), FPendingTicket::Acquire(Account, 101).IsSet());
    return true;
}
```

Includes for this test are `Misc/AutomationTest.h`, `VaRestInspectorQueue.h`, `VaRestInspector.h`; wrap the registration/implementation in `#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR`. Compiled-out contract tests live in ModuleTest and are not hidden by this guard. Run `Tools/run_tests.sh --no-build VaRest.Inspector.Store.QueueTicket` after its permitted build; expect exactly this registered test to pass, and R10's removal of byte reservation to fail the rejection/full-capacity assertions.

**Tasks5/7, Phases.cpp and Snapshot.cpp:** completion deadlines use only the observer clock; the source duration primitive Between above remains separate.

```cpp
static bool ExcerptDeadlineReached(
    const IVaRestClock& ReceiptClock, double CompletedObservedAt,
    bool bActualCompletionObserved)
{
    return bActualCompletionObserved
        && FMath::IsFinite(CompletedObservedAt)
        && ReceiptClock.Now() >= CompletedObservedAt + 5.0;
}
```

The exact timeout assertions are: observer0/source1000000 at actual Completed gives false; observer4.999 gives false; observer5.000 gives true; `bActualCompletionObserved=false` stays false even at observer100. A snapshot-only row never sets the bool. Sealing tests apply these through the real store slot transition, asserting Pending→TimedOut and immutable prior snapshots, rather than only retesting this helper.

**Task11, Service.cpp:** the fixed-watermark scheduler has this complete scalar decision core. The caller drains both lanes within the single256-item allowance, then calls CanRun; it captures/applies/releases one source snapshot when true, and only then calls Complete. Loss epochs never wrap; saturation keeps recovery dirty. This helper contains no source snapshot or row references and requires no allocations.

```cpp
struct FOverflowRecovery
{
    uint64 HandledLoss = 0, HandledOrphan = 0;
    uint64 TargetLoss = 0, TargetOrphan = 0, Watermark = 0;
    double LastRun = -TNumericLimits<double>::Max();
    bool bPending = false;
    void Observe(uint64 Loss, uint64 Orphan, uint64 PublishedMetadata)
    {
        const bool Dirty = Loss != HandledLoss || Orphan != HandledOrphan
            || Loss == MAX_uint64 || Orphan == MAX_uint64;
        if (!bPending && Dirty)
        {
            TargetLoss = Loss;
            TargetOrphan = Orphan;
            Watermark = PublishedMetadata;
            bPending = true;
        }
    }
    bool CanRun(double Now, uint64 ConsumedMetadata) const
    {
        return bPending && ConsumedMetadata >= Watermark
            && FMath::IsFinite(Now) && Now - LastRun >= 0.250;
    }
    void Complete(double Now)
    {
        check(bPending);
        HandledLoss = TargetLoss;
        HandledOrphan = TargetOrphan;
        LastRun = Now;
        bPending = false;
    }
};
```

At each Tick call Observe(LossEpoch, OrphanEpoch, LastPublishedMetadata) before drain and again after applying at most256 packets; then, if CanRun, reconcile once and call Complete. Call Observe again after completion even if no packet arrived. A newly admitted orphan increments OrphanEpoch during GT Apply, after assigning BirthMetadataOrdinal=LastConsumedMetadataOrdinal. The scalar belongs only to the active generation: BeginGeneration rebases all retained mutable rows before the new attach walk, including ordinal-overflow rotation, so an old A:100 can never remain in B:0 eligibility comparisons. Existing pre-W rows and direct snapshot rows are eligible by that scalar; post-W metadata births wait for the already armed next epoch. Orphan events applied after the recovery increment a new local epoch; they remain unpinned until a later source-presence walk. During the walk mark all present mutable rows live/pinned, including orphan origins, and assign current reconciliation stamp; after the walk unpin eligible absent rows. An actual terminal-known row is never promoted by an orphan event. If source absence is learned before an already queued Completed, preserve the unsealed unavailable row for later coherent completion enrichment. Clear/Pause rotate both local scheduling epochs but not allocation ledgers. Newer epochs therefore rearm without extending an existing Watermark. On Clear/Pause discard this generation's scheduler state, reconcile pause/resume as Task11 defines, and never reset the shared pending allocation account. Ordinal overflow is fail-closed: before assigning MAX_uint64+1 close that generation and request a fresh one on GT, preserving the common account; run the SAME BeginGeneration rebase and live-presence attach reconciliation as Resume before publishing the new sink. Do not wrap an ordinal into an older watermark or compare a retained old-generation birth against the new generation's W. A saturated loss epoch may trigger a bounded250ms reconciliation cadence until a new generation, which remains finite and visible.

The exact fixed-watermark assertions are: Observe(loss1,orphan0,published10), then Observe(loss2,orphan1,published1000) leaves Watermark10; CanRun(now0,consumed9) is false and consumed10 true; Complete(0), Observe(loss2,orphan1,published1000) rearms; after handling both, Observe(loss2,orphan2,published1000) rearms solely from a delayed orphan; CanRun(.249,1000) false and CanRun(.250,1000) true. The lifecycle fixture must additionally prove dropped-Completed rows unpin without fabricated outcome, rather than accepting this arithmetic helper alone as the recovery proof.

## Fix1 boundary contracts (normative, part of the existing 17 tasks)

These declarations replace the earlier shorthand method lists. No eighteenth task or alternate implementation is introduced. New exact files are `Private/VaRestInspectorPolicy.h` (Task2), `Public/VaRestInspectorRecords.h` (public shell created Task2, extended Task4), `Public/VaRestInspectorPacketView.h` (Task2 public borrowed packet views), `Public/VaRestInspectorStats.h` (empty read contract created Task2, implemented Task10), `Private/VaRestInspectorAllocation.h/.cpp` (Task3), and `Private/VaRestInspectorBudget.h/.cpp` (Task4). Allocation.h defines FPendingTicket and its account before Packet.h includes it; Queue.h includes Packet.h and Allocation.h. This avoids a cyclic include. `FReservation` is removed: the one event reservation type is `FPendingTicket`. Store packet transfer uses a distinct byte-only `FStoreCredit`, never converts/reset a live pending ticket in place.

### Task2 policy and public read contract; Task3 packet contract

All private declarations below are in `VaRest::Inspector::Private`, except the explicitly public read types. Every header includes its directly used Core/Trace/ArrayView/Function/SharedPointer/atomic types. Policy.h includes VaRestTrace.h and has no UObject dependency. Settings normalization fills this exact value type; FCapturePolicy is immutable once copied into a sink.

```cpp
struct FNormalizedLimits
{
    int64 StoreBytes = 64ll * 1024 * 1024;
    int32 MaxRecords = 10000;
    int32 BodyBytes = 65536, BinaryBytes = 0, FrameBytes = 4096;
    int32 MaxFramesPerSocket = 5000;
    int32 PendingEvents = 65536;
    int64 PendingBytes = 16ll * 1024 * 1024;
    static constexpr int32 MaxHeaders = 256, MaxWarnings = 32;
    static constexpr int32 MaxStringChars = 16384;
    static constexpr int64 MaxMetadataBytes = 1024 * 1024;
    static constexpr int32 DispatchSlots = 16, TimelineSlots = 128, NoteSlots = 64;
    static constexpr int32 Clients = 1024, StatsGroups = 1024, KeysPerClient = 256;
    static constexpr int32 Links = 4096, LinksPerTarget = 64, Tombstones = 4096;
    static constexpr int32 SnapshotControllers = 4;
};
struct FCapturePolicy
{
    FNormalizedLimits Limits;
    bool bHeaders = true;
    bool bBodies = true;
    FVaRestTraceDetailSettings Detail() const
    {
        FVaRestTraceDetailSettings Out;
        Out.Flags = EVaRestTraceDetail::Metadata;
        if (bHeaders) Out.Flags |= EVaRestTraceDetail::Headers;
        if (bBodies) Out.Flags |= EVaRestTraceDetail::Bodies;
        Out.BodyCap = Limits.BodyBytes;
        Out.BinaryCap = Limits.BinaryBytes;
        Out.FrameCap = 0;
        Out.MaxPendingExcerptBytes = Limits.PendingBytes;
        return Out;
    }
};
```

The EText/FTextSlice/FPairSlice/FArraySlice/FPacketHeader/FPacketScalars/FPacketFixed declarations below live in new `Public/VaRestInspectorPacketView.h`, namespace `VaRest::Inspector::View`, created Task2 after Task1 makes the additive core fields available. Packet.h imports these names with explicit using declarations. The remaining layout/codec/queue types stay private. Packet.h has one fixed scalar layout tagged by a validated HTTP Kind, rather than a C++ nontrivial union. Only fields in that kind's inventory are meaningful; defaults never imply observed values. ScalarOnly preserves this same header/scalar identity and sets bMetadataOmitted, with all text/header/warning slices empty and no excerpt reference. There is no fallback serialization of TVariant object representation.

```cpp
enum class EText : uint8
{
    Verb, Url, PathTemplate, QueueId, OwnerPath, ScriptObject, ScriptFunction,
    SourceFile, BodyContentType, EffectiveUrl, ResponseContentType, ErrorCode,
    ErrorMessage, DataPath, PredicatePath, Expected, Actual, EnvelopeCode,
    EnvelopeMessage, StopReason, Sha256Prefix, ScopeLabel, ClientLabel, MarkerText,
    Count
};
struct FTextSlice { uint32 Offset = 0; uint32 Chars = 0; };
struct FPairSlice { FTextSlice First; FTextSlice Second; };
struct FArraySlice { uint32 Offset = 0; uint32 Count = 0; };
struct FPacketHeader
{
    EVaRestTraceKind Kind = EVaRestTraceKind::Started;
    EVaRestTraceSource Source = EVaRestTraceSource::Http;
    uint64 Seq = 0, Generation = 0, MetadataOrdinal = 0;
    double Time = 0;
    bool bSnapshot = false, bMetadataOmitted = false;
};
struct FPacketScalars
{
    uint64 RequestId = 0, DispatchId = 0, ScopeId = 0, ClientId = 0;
    uint64 OriginalRequestId = 0, AuthRefreshId = 0, CauseRequestId = 0;
    uint64 SocketId = 0, FrameId = 0;
    int32 Attempt = 0, CurrentAttempt = 0, Status = 0, ErrorHttpStatus = 0;
    int32 ScriptCodeOffset = INDEX_NONE, SourceLine = 0, PIEInstance = INDEX_NONE;
    int32 DecodeWarningCount = 0;
    int64 BodyBytes = 0, BytesUp = 0, BytesDown = 0, NoteBytes = 0;
    int64 DeliveredBodyBytes = -1, TotalBytes = 0;
    double StartedAt = 0, AttemptStart = 0, GapSeconds = 0, AuthWait = 0;
    double LimiterWait = 0, UploadDoneAt = -1, StatusAt = -1, TransportDoneAt = -1;
    double DecodeStartAt = -1, DecodeEndAt = -1, Delay = 0, RetryAfter = -1;
    FDateTime StartUtc;
    FName Endpoint, Tag, CachePolicy, FailureDetail, ResultType, Reason;
    EVaRestPriority Priority = EVaRestPriority::Normal;
    EVaRestTraceRequestFlags Flags = EVaRestTraceRequestFlags::None;
    EVaRestBodyKind BodyKind = EVaRestBodyKind::None;
    EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
    EVaRestTraceGap GapKind = EVaRestTraceGap::None;
    EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
    EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success;
    EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
    EVaRestErrorKind ErrorKind = EVaRestErrorKind::None;
    EVaRestTraceResponseStorage Storage = EVaRestTraceResponseStorage::Unknown;
    EVaRestTraceNoteKind NoteKind = EVaRestTraceNoteKind::RetryScheduled;
    EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
    EVaRestExcerptKind ExcerptKind = EVaRestExcerptKind::Text;
    EVaRestExcerptSkip Skip = EVaRestExcerptSkip::None;
    EVaRestTraceMarkerKind MarkerKind = EVaRestTraceMarkerKind::Online;
    bool bCacheLookupPending = false, bExcerptPending = false, bConditional = false;
    bool bFromCache = false, bEnvelopeApplied = false, bTruncated = false;
    bool bRedacted = false, bOpened = false, bEditorScope = false, bCreated = false;
};
struct FPacketFixed
{
    FPacketHeader Header;
    FPacketScalars Scalars;
    FTextSlice Text[static_cast<int32>(EText::Count)];
    FArraySlice Headers, Warnings;
};
enum class EMeasure : uint8 { Full, ScalarOnly, Unsupported };
struct FPacketLayout
{
    static constexpr int32 MaxTexts = static_cast<int32>(EText::Count) + 2 * 256 + 2 * 32;
    FPacketFixed Fixed;
    FPairSlice Headers[256];
    FPairSlice Warnings[32];
    FStringView BorrowedText[MaxTexts];
    FTextSlice TextDestination[MaxTexts];
    int32 TextCount = 0;
    SIZE_T BackingRequest = 0, BackingBound = 0;
    int64 ExcerptCapacity = 0, PendingCharge = 0;
    bool bKeepExcerpt = false;
};
struct FAllocationCounters
{
    std::atomic<int64> LiveBacking{0}, PeakBacking{0}, TextBytesCopied{0};
    std::atomic<int64> ExcerptRefsAcquired{0}, OverlapObservations{0};
    std::atomic<int64> LiveControllers{0}, DestroyedControllers{0};
};
class FAllocationAdapter
{
public:
    explicit FAllocationAdapter(FAllocationCounters& InCounters);
    TOptional<SIZE_T> Bound(SIZE_T Request, uint32 Alignment) const;
    void* AllocateReserved(SIZE_T Request, uint32 Alignment, SIZE_T ReservedBound);
    void Free(void* Ptr);
    FAllocationCounters& Counters() const;
    SIZE_T QueueNodeBound() const;
private:
    FAllocationCounters* Probe;
};
class FPacket
{
public:
    FPacket() = default;
    FPacket(FPacket&&) noexcept;
    FPacket& operator=(FPacket&&) noexcept;
    ~FPacket();
    FPacket(const FPacket&) = delete;
    FPacket& operator=(const FPacket&) = delete;
    const FPacketFixed& Fixed() const;
    FStringView Text(EText Field) const;
    TConstArrayView<FPairSlice> Headers() const;
    TConstArrayView<FPairSlice> Warnings() const;
    FStringView Read(FTextSlice Slice) const;
    const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>& Excerpt() const;
    int64 RetainedCapacity() const;
    void Reset();
private:
    friend class FPacketCodec;
    friend class FStorePacketOwner;
    FPendingTicket Pending;
    FAllocationAdapter* Allocator = nullptr;
    void* Backing = nullptr;
    SIZE_T BackingCapacity = 0;
    TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> ExcerptOwner;
};
class FPacketCodec
{
public:
    static EMeasure Measure(const FVaRestTraceEvent& Borrowed,
        const FCapturePolicy& Policy, const FAllocationAdapter& Allocator,
        FPacketLayout& Out);
    static EMeasure MeasureStarted(const FVaRestTraceStarted& Borrowed,
        FPacketHeader Header, const FCapturePolicy& Policy,
        const FAllocationAdapter& Allocator, FPacketLayout& Out);
    static FPacket WriteStarted(const FVaRestTraceStarted& Borrowed,
        const FPacketLayout& Layout, FPendingTicket&& Reservation,
        FAllocationAdapter& Allocator);
    static FPacket Write(const FVaRestTraceEvent& Borrowed,
        const FPacketLayout& Layout, FPendingTicket&& Reservation,
        FAllocationAdapter& Allocator);
private:
    static FPacket WriteLayout(const FPacketLayout& Layout, FPendingTicket&& Reservation,
        FAllocationAdapter& Allocator,
        const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* BorrowedExcerpt);
};
enum class EPush : uint8 { Accepted, ScalarOnly, Dropped, Closed, Unsupported };
class FCaptureQueue
{
public:
    FCaptureQueue(const FCapturePolicy&, uint64 Generation,
        TSharedRef<FPendingAccount, ESPMode::ThreadSafe>, FAllocationAdapter&);
    ~FCaptureQueue();
    EPush TryPush(const FVaRestTraceEvent& Borrowed);
    TOptional<FPacket> TryPopMetadata(); // GT single consumer
    TOptional<FPacket> TryPopExcerpt();  // GT single consumer
    void Close();
    uint64 LastPublishedMetadata() const; // GT
    uint64 LossEpoch() const;
    int64 OutstandingExcerptReservations() const;
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
};
```

FPacketLayout borrows only until Write returns; it never acquires an excerpt ref. Measure validates Kind/variant before field access, fills Fixed field-by-field using the complete inventory, lays out `[FPacketFixed][header pairs][warning pairs][terminated TCHAR spans]` with checked alignment/offset arithmetic and queries the nonallocating adapter bound. MaxTexts=600, so the sizing scratch is fixed stack storage. Body capacity is the borrowed source TArray.GetAllocatedSize; a metadata-only local policy clears promises and bKeepExcerpt. PendingCharge includes actual FPacket-sized queue-node capacity plus BackingBound and retained source excerpt capacity. Unsupported allocator/layout returns Unsupported and a finite diagnostic, never copy-then-admit. Write requires a live exact reservation, writes only measured ranges, then acquires at most one source excerpt ref and increments the matching probe. Read validates each offset/length against allocated backing in development; returned views expire with their packet owner. Header/scalar fields are retained once, not duplicated into heap strings. ScalarOnly uses the same fixed block with empty slices, and its smaller PendingCharge is independently acquired before Write.

The allocator adapter is not a fake allocator: AllocateReserved uses FMemory::Malloc, observes GetAllocSize and increments LiveBacking/PeakBacking by actual capacity; Free obtains capacity before FMemory::Free and decrements after free. It never allocates before the caller's credit. Bound must be a supported upper bound, not generic QuantizeSize faith (see supported allocator contract below); unknown/unsupported returns unset. The queue's static TMpscQueue allocator adapter uses a scoped thread-local reservation context only during Enqueue's synchronous Malloc call, records actual node capacity and clears that context before returning. Free requires no TLS context and may run on a worker. Its exact node request is `sizeof({std::atomic<void*> Next; TTypeCompatibleBytes<FPacket> Value;})` with the real alignment; assert the actual request/alignment seen by TMpscQueue against that mirror at every allocator call. Both sentinels are pre-admitted separately at queue construction. Production adapter/probe lifetime is an intrusive allocation-context root retained by BOTH pending and store accounts, so resetting pending credit after transfer cannot invalidate its raw adapter view. Context owns no account/row/queue, preventing a cycle; every admitted packet and retired queue therefore retains a live allocator through its current credit. The context allocation is included conservatively in both ledgers' fixed startup allowances; tests use stack adapters only when they drain all owners before exit.

Task4's immutable store-packet owner is deliberately a private value with no escaping weak API. It owns its byte credit and the same backing/excerpt, and declares `static bool TryTake(FPacket& InOut, TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator, FStorePacketPtr& Out);`, where FStorePacketPtr is the custom-deleted unique owner defined below. It first acquires enough store credit for wrapper+backing+retained excerpt capacity, allocates its wrapper through the adapter, and records an OverlapObservations probe while BOTH credits are live. Only then move backing/ref into Out, clear InOut's ownership, and reset its pending credit. On refusal InOut and Out remain unchanged. Ordinary FStore::Apply takes `FPacket&&` but binds it as an owned local until TryTake succeeds or its destructor drops it; it cannot release pending credit merely on dequeue. `TUniquePtr` wrapper allocation/deletion is itself adapter-backed with a deleter that frees before releasing wrapper credit. This signature and atomic ownership sequence are fixed in Task4 before consumers land.

Public Records.h defines borrowed read views with no shared record-owner escape:

```cpp
struct FVaRestInspectorPacketView
{
    const VaRest::Inspector::View::FPacketFixed* Fixed = nullptr;
    TConstArrayView<uint8> Backing, Excerpt;
    FStringView Read(VaRest::Inspector::View::FTextSlice S) const
    {
        if (!Fixed || S.Chars == 0) return {};
        const uint64 End = uint64(S.Offset) + uint64(S.Chars) * sizeof(TCHAR);
        check(End <= uint64(Backing.Num()) && S.Offset % alignof(TCHAR) == 0);
        return FStringView(reinterpret_cast<const TCHAR*>(Backing.GetData() + S.Offset), S.Chars);
    }
    FStringView Text(VaRest::Inspector::View::EText Field) const
    {
        return Fixed ? Read(Fixed->Text[static_cast<int32>(Field)]) : FStringView();
    }
    TConstArrayView<VaRest::Inspector::View::FPairSlice> Pairs(
        VaRest::Inspector::View::FArraySlice S) const
    {
        using FPair = VaRest::Inspector::View::FPairSlice;
        if (!Fixed || S.Count == 0) return {};
        check(uint64(S.Offset) + uint64(S.Count) * sizeof(FPair) <= uint64(Backing.Num()));
        check(S.Offset % alignof(FPair) == 0);
        return {reinterpret_cast<const FPair*>(Backing.GetData() + S.Offset), int32(S.Count)};
    }
};
// Byte counters report admitted payload/retained-owner charges, not RSS or
// hidden post-destruction controller free tails; see decision8.
struct FVaRestInspectorDiagnostics
{
    uint64 DroppedEvents = 0, DroppedBytes = 0, RefusedRows = 0;
    uint64 MetadataOmitted = 0, UnsupportedAllocator = 0, InterruptedRows = 0;
    int64 StoreBytes = 0, StorePeakBytes = 0, PendingBytes = 0, PendingPeakBytes = 0;
    int32 SnapshotControllers = 0;
    bool bCapturing = false, bPendingLimits = false;
    int64 EffectiveStoreLimit = 0, RequestedStoreLimit = 0;
};
struct FVaRestInspectorRecordView
{
    EVaRestInspectorRecordKind Kind = EVaRestInspectorRecordKind::Http;
    uint64 RequestId = 0, Revision = 0;
    EVaRestInspectorEndState EndState = EVaRestInspectorEndState::CaptureInterrupted;
    bool bPartial = false, bPinned = false, bSealed = false, bTerminalKnown = false;
    FVaRestInspectorPacketView Started, Completed, Marker;
    TConstArrayView<FVaRestInspectorPacketView> DispatchEvents, Notes, Excerpts;
    TConstArrayView<FVaRestInspectorTimelineEntry> Timeline;
};
class VARESTINSPECTOR_API FVaRestInspectorSnapshot final
{
public:
    FVaRestInspectorSnapshot();
    ~FVaRestInspectorSnapshot();
    FVaRestInspectorSnapshot(const FVaRestInspectorSnapshot&) = delete;
    FVaRestInspectorSnapshot& operator=(const FVaRestInspectorSnapshot&) = delete;
    uint64 GetRevision() const;
    void ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)>) const;
    void ForEachStats(TFunctionRef<void(const FVaRestInspectorHttpStats&)>) const;
    TConstArrayView<FVaRestInspectorLink> GetLinks() const;
    const FVaRestInspectorDiagnostics& GetDiagnostics() const;
private:
    friend class VaRest::Inspector::Private::FStore;
    struct FData;
    TUniquePtr<FData> Data;
};
```

Before this public class, forward-declare the namespaced FStore and FVaRestInspectorHttpStats; define FVaRestInspectorLink and the record enums/timeline as above. Public Records.h includes PacketView.h. DispatchEvents contains the retained Dispatched/Ended packet views, keyed by each Fixed->Scalars.DispatchId; Excerpts carries Request/Response owner and exact DispatchId. No raw/private owner escapes. Every view and its arrays expire at the enclosing record visitor's return. Enumeration constructs at most32 dispatch-event,64 note and17 excerpt view descriptors on the stack (bounded by row caps), then invokes the visitor. These contain no owning strings or references. Task2 defines the public empty snapshot/read types and a no-data FData with all out-of-line methods in Module.cpp; Task7 removes those definitions there as it adds Snapshot.cpp; Task4 extends private backing and real record views; Task7 adds immutable snapshot ownership. Task10 implements the already declared stats visitor. The shell has real empty method bodies in Task2 (revision0, zero callbacks, empty links, fixed diagnostics), so every intermediate module links.

### Tasks7–8: payload-free and retained-controller lifetime adapter

Use the supported public constructor `TSharedPtr<T, ESPMode::ThreadSafe>(Raw, MoveTemp(Deleter))`, inspected at installed `Templates/SharedPointer.h:743–749`. The actual implementation creates `SharedPointerInternals::TReferenceControllerWithDeleter<T,D,ThreadSafe>` at SharedPointerInternals.h:468–470, moves D into its TDeleterHolder once (:354–387), invokes D only for last-strong DestroyObject (:223–232), and destroys D when the last weak count deletes the controller (:305–314). Do not use a private TSharedPtr-layout cast, fabricated allocator seam or MakeShared for externally weak-retainable snapshot/record owners.

The approved allocator bound is deliberately conservative:4096 bytes for an exact controller `sizeof≤1024` and ordinary-new alignment≤16. Ordinary new uses Size≤8?8:__STDCPP_DEFAULT_NEW_ALIGNMENT__ at `Modules/Boilerplate/ModuleBoilerplate.h:46–50`, not simply alignof(T). Allow only the audited engine build/family: Binned2's small-bin Malloc/GetAllocationSize/QuantizeSizeCommon (`MallocBinned2.cpp:570–602`, `MallocBinned2.h:285–305`, `MallocBinnedCommon.h:823–850`), or UE's compiled mimalloc2.0.0 (`ThirdParty/mimalloc/mimalloc.Build.cs:8`). `MallocMimalloc.cpp:124–149` uses mi_malloc_aligned; vendored2.0.0 alloc-aligned.c:17–59 requests at most size+alignment−1, with at most8 padding bytes (mimalloc-types.h:354–363); page-queue.c:56–94 and init.c's size classes round ≤1047 bytes far below4096. `GetAllocationSize` returns mi_malloc_size (:284–288). Adjacent vendored2.1.2 is **not** the selected source and is not implied audited.

The adapter's `FSupportedAllocator` has `enum class EFamily {Unsupported,Binned2,Mimalloc200}; EFamily Family; bool bVerifiedModuleNew=false; bool bVerifiedNoInflatingProxy=false; bool bCalibrationPassed=false;`. Initialize on GT using the exact inspected engine build/config/allocator manifest plus live allocator identity, not merely an unchecked descriptive-name string. The manifest records the source hashes above, module replacement-new flags (`!FORCE_ANSI_ALLOCATOR`, no static-analyzer replacement suppression), pointer size64, ordinary-new alignment≤16, and that the actual proxy chain does not inflate usable allocation size. Native startup/test evidence verifies this against the running binary and named allocator. If the build/allocator chain cannot establish those facts, leave Unsupported and refuse controller creation with one finite UnsupportedAllocator diagnostic. Do not silently treat a new allocator version/wrapper as audited. No flag/allocator change is authorized to make a gate pass.

Calibration allocates a separate same-request/same-ordinary-alignment block through FMemory, observes GetAllocSize≤4096, then frees it; admission for this calibration block is pre-reserved4096 in the fixed startup allowance. This is actual allocator evidence, **not** the actual hidden TSharedPtr controller and not the universal bound by itself. The bound comes from the named source paths/config restrictions. Count actual stored-deleter construction/destruction separately; those probes observe controller lifetime, never the subsequent hidden operator-delete/free. The decision8 tail exclusion is ≤4096 bytes per already-destroyed controller but unbounded in aggregate under arbitrary external final-release thread suspension. Exclude allocator arena/slab metadata/fragmentation from the logical ledger, never call it RSS. Unsupported allocator fallback is admission refusal before controller allocation, not allocating to discover that it was too large.

Budget.h defines an intrusive account root, so tickets do not need recursively allocated shared controllers. Its bootstrap allocation is admitted4096 against the requested store cap before creation and remains part of UsedBytes; no weak root references escape. `TRefCountPtr<FBudgetAccount>` uses the current thread-safe FRefCountedObject from `Templates/RefCounting.h:114–148`; the older FThreadSafeRefCountedObject spelling is deprecated in this UE5.8 tree and is not used. Account destruction owns no child pointers; credits own only the root, so there is no cycle. Explicit Clear regards the account as quiescent only with the service's sole root reference, zero snapshot controllers and UsedBytes==BootstrapBytes after freeing service-owned blocks. It can then replace the root; expired weak snapshot controllers keep it nonquiescent.

```cpp
struct FBudgetAccount : public FRefCountedObject
{
    FBudgetAccount(int64 InLimit, int64 InBootstrap)
        : Limit(InLimit), BootstrapBytes(InBootstrap), UsedBytes(InBootstrap) {}
    const int64 Limit, BootstrapBytes;
    std::atomic<int64> UsedBytes;
    std::atomic<int64> SnapshotControllers{0};
    FAllocationCounters Probe;
};
class FStoreCredit
{
public:
    FStoreCredit() = default;
    FStoreCredit(const FStoreCredit&) = delete;
    FStoreCredit& operator=(const FStoreCredit&) = delete;
    FStoreCredit(FStoreCredit&& Other) noexcept
        : Account(MoveTemp(Other.Account)), Bytes(Other.Bytes), bSlot(Other.bSlot)
    { Other.Bytes = 0; Other.bSlot = false; }
    FStoreCredit& operator=(FStoreCredit&& Other) noexcept
    {
        if (this != &Other)
        {
            Reset(); Account = MoveTemp(Other.Account);
            Bytes = Other.Bytes; bSlot = Other.bSlot;
            Other.Bytes = 0; Other.bSlot = false;
        }
        return *this;
    }
    ~FStoreCredit() { Reset(); }
    static TOptional<FStoreCredit> Acquire(TRefCountPtr<FBudgetAccount> A,
        int64 Charge, bool bSnapshotSlot = false)
    {
        if (!A || Charge < 0) return {};
        if (bSnapshotSlot && !TryReserve(A->SnapshotControllers, 4, 1)) return {};
        if (!TryReserve(A->UsedBytes, A->Limit, Charge))
        {
            if (bSnapshotSlot) A->SnapshotControllers.fetch_sub(1);
            return {};
        }
        FStoreCredit Out;
        Out.Account = MoveTemp(A); Out.Bytes = Charge; Out.bSlot = bSnapshotSlot;
        return TOptional<FStoreCredit>(MoveTemp(Out));
    }
    FBudgetAccount* GetAccount() const { return Account.GetReference(); }
    void Reset()
    {
        if (Account)
        {
            Account->UsedBytes.fetch_sub(Bytes, std::memory_order_acq_rel);
            if (bSlot) Account->SnapshotControllers.fetch_sub(1, std::memory_order_acq_rel);
            Bytes = 0; bSlot = false; Account.SafeRelease();
        }
    }
private:
    TRefCountPtr<FBudgetAccount> Account;
    int64 Bytes = 0;
    bool bSlot = false;
};
template<typename T>
class TInspectorDeleter
{
public:
    TInspectorDeleter(FStoreCredit&& P, FStoreCredit&& C)
        : Payload(MoveTemp(P)), Controller(MoveTemp(C)) {}
    TInspectorDeleter(const TInspectorDeleter&) = delete;
    TInspectorDeleter& operator=(const TInspectorDeleter&) = delete;
    TInspectorDeleter(TInspectorDeleter&& Other) noexcept
        : Payload(MoveTemp(Other.Payload)), Controller(MoveTemp(Other.Controller))
    {
        check(!Other.bStored); // inspected UE constructor makes exactly this one move
        bStored = true;
        Controller.GetAccount()->Probe.LiveControllers.fetch_add(1);
    }
    ~TInspectorDeleter()
    {
        if (bStored)
        {
            FBudgetAccount* A = Controller.GetAccount();
            A->Probe.LiveControllers.fetch_sub(1);
            A->Probe.DestroyedControllers.fetch_add(1);
        }
        // Credit/slot releases with stored-deleter destruction, before hidden
        // operator-delete/free: decision8 explicitly excludes that tail.
    }
    void operator()(T* Object)
    {
        // Payload still retains its account and intrusive allocation context.
        FAllocationAdapter& Adapter = Payload.GetAccount()->Lifetime->Allocator;
        Object->~T();
        Adapter.Free(Object); // observes capacity, frees, then decrements LiveBacking
        Payload.Reset(); // LAST: context survives the post-free bookkeeping
    }
private:
    FStoreCredit Payload, Controller;
    bool bStored = false;
};
template<typename T, typename... Args>
TSharedPtr<T, ESPMode::ThreadSafe> MakeInspectorOwner(
    TRefCountPtr<FBudgetAccount> Account, const FSupportedAllocator& Allocator,
    SIZE_T PayloadBound, bool bSnapshotSlot, Args&&... Arguments)
{
    using D = TInspectorDeleter<T>;
    using C = SharedPointerInternals::TReferenceControllerWithDeleter<T,D,ESPMode::ThreadSafe>;
    if (!Allocator.SupportsController(sizeof(C), alignof(C))) return {};
    auto Control = FStoreCredit::Acquire(Account, 4096, bSnapshotSlot);
    if (!Control.IsSet()) return {};
    auto Payload = FStoreCredit::Acquire(Account, PayloadBound);
    if (!Payload.IsSet()) return {};
    FAllocationAdapter& Adapter = Account->Lifetime->Allocator;
    const auto VerifiedBound = Adapter.Bound(sizeof(T), alignof(T));
    if (!VerifiedBound.IsSet() || VerifiedBound.GetValue() != PayloadBound) return {};
    void* Storage = Adapter.AllocateReserved(sizeof(T), alignof(T), PayloadBound);
    T* Object = new(Storage) T(Forward<Args>(Arguments)...);
    D Deleter(MoveTemp(Payload.GetValue()), MoveTemp(Control.GetValue()));
    return TSharedPtr<T,ESPMode::ThreadSafe>(Object, MoveTemp(Deleter));
}
```

`FSupportedAllocator::SupportsController(Request,Align)` returns true exactly when Family is audited, all three verification/calibration flags are true, Request≤1024, Align≤16, and compiled replacement ordinary-new alignment≤16. It never allocates. PayloadBound is independently obtained from the adapter for sizeof(T)/alignof(T); callers cannot pass an arbitrary unverified bound. The exact factory and deleter both use Account->Lifetime->Allocator: AllocateReserved increments actual LiveBacking, Free obtains capacity before free and decrements afterward while Payload still retains the context. Define these templates only after FAllocationLifetime is complete. No raw shared-payload allocation/free bypass is permitted. The adapter uses LLM_SCOPE_BYTAG; nested snapshot arrays/record clones own separate byte credits, released by T's destructor. Do not combine those nested allocations into the controller charge. Root bootstrap allocation has its own checked source bound and calibration, with all temporary storage counted; no fake controller is constructed to warm a budget. Apply this adapter to the snapshot, immutable shared rows and other Inspector-created shared controllers; source-owned excerpt controllers remain the upstream/reference-capacity contract. Do not give a record shared pointer to public visitors.

`VaRest.Inspector.Store.WeakControllerBudget` holds four TWeakPtr snapshots, resets every strong pointer, confirms each Weak.Pin() is invalid, yet SnapshotControllers==4 and LiveControllers==4 and UsedBytes retains four4096 charges. After last strong release, assert the context adapter's LiveBacking equals its pre-snapshot backing baseline while UsedBytes still includes four4096 controller charges and SnapshotControllers remains4. This independently checks actual allocation/free probe pairing; removing Adapter.Free's decrement must fail that assertion. A fifth TrySnapshot must fail with payload count zero. Repeat Clear and lower limits beneath held bytes: pending stays true, counters/highwater do not reset. Release one weak pointer on an owned worker, join, then assert exactly one stored-deleter destruction/4096 release and one available slot. Release the rest, verify UsedBytes returns to the bootstrap/store baseline, and only a subsequent explicit Clear applies changed limits. Observe actual same-request calibration sizes separately from live controller counts. Mutate only `operator()` to reset Controller early: unchanged fifth-admission and pending-limits assertions fail; restore exactly and rerun. Small-budget byte refusal is tested independently of the four-slot cap. No test claims access to the hidden controller allocation pointer or post-free observation. LiveControllers/DestroyedControllers are stored-deleter lifetime probes, not physical allocation/free probes. Worker join proves that owned test release returned; it does not establish an aggregate physical bound for arbitrarily suspended external release threads.

### Task1: scoped real-response gate and complete sequential fixture

Add this WITH_DEV_AUTOMATION_TESTS-only declaration in existing private VaRestTraceExcerpts.h, namespace VaRest::Http::Excerpts. It selects only one real response job by already captured RequestId+DispatchId. It is not an arbitrary callback or production option. Installation/destruction and FJob::Launch selection are GT-only; workers receive only the already prepared job/gate task. The owning gate must outlive the scope and all launched tasks. Nested scopes restore LIFO.

```cpp
#if WITH_DEV_AUTOMATION_TESTS
class FScopedResponseGate final
{
public:
    FScopedResponseGate(uint64 Request, uint64 Dispatch, FRedactionGate& InGate);
    ~FScopedResponseGate();
    FScopedResponseGate(const FScopedResponseGate&) = delete;
    FScopedResponseGate& operator=(const FScopedResponseGate&) = delete;
private:
    friend struct FJob;
    static FScopedResponseGate* Current;
    FScopedResponseGate* Previous = nullptr;
    uint64 RequestId = 0, DispatchId = 0;
    FRedactionGate* Gate = nullptr;
};
#endif
```

Definitions belong in existing TraceExcerpts.cpp; no extra production file or public HTTP test API. Place the selection at the beginning of the real FJob::Launch, which AttemptEnded calls after emitting its pending promise. Nonmatching/no-route jobs execute the unchanged production LaunchRedaction path. Request jobs never match. The existing LaunchOn(Gate) implementation is reused unchanged.

```cpp
#if WITH_DEV_AUTOMATION_TESTS
VaRest::Http::Excerpts::FScopedResponseGate*
    VaRest::Http::Excerpts::FScopedResponseGate::Current = nullptr;
VaRest::Http::Excerpts::FScopedResponseGate::FScopedResponseGate(
    uint64 Request, uint64 Dispatch, FRedactionGate& InGate)
    : Previous(Current), RequestId(Request), DispatchId(Dispatch), Gate(&InGate)
{
    check(IsInGameThread() && RequestId != 0 && DispatchId != 0);
    Current = this;
}
VaRest::Http::Excerpts::FScopedResponseGate::~FScopedResponseGate()
{
    check(IsInGameThread() && Current == this);
    Current = Previous;
}
#endif

void VaRest::Http::Excerpts::FJob::Launch()
{
#if WITH_DEV_AUTOMATION_TESTS
    check(IsInGameThread());
    const FScopedResponseGate* Route = FScopedResponseGate::Current;
    if (Route && Owner == EVaRestTraceExcerptOwner::Response
        && RequestId == Route->RequestId && DispatchId == Route->DispatchId)
    {
        LaunchOn(*Route->Gate);
        return;
    }
#endif
    ExcerptsPrivate::LaunchJob(*this, [](const TCHAR* Name, TUniqueFunction<void()>&& Body)
    {
        return LaunchRedaction(Name, MoveTemp(Body));
    });
}
```

The following complete test is added in the existing ExcerptTest file/namespace context, reusing its already defined FProceedTwice, First, Json, ResponseExcerpts, HoldPipe and Abandon helpers inspected at e7a1bd3. FProceedTwice::Process holds the first outcome upstream and FirstEnds counts its arrival; calling ProceedAgain **after** FirstEnds makes both dispatch ends current in sequence under Attempt1. The first503 body cannot choose WillExcerptOnDecode, but the second200 FVaRestJson body does. The retry policy never sees the held503, so this is not a retry test. Existing both-end-order hedge cases remain separate and unchanged except for identity assertions.

```cpp
#if VAREST_WITH_TRACE_EXCERPTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMixedExcerptDispatchTest,
    "VaRest.Http.Trace.MixedExcerptDispatchIdentity",
    EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestHttpMixedExcerptDispatchTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Http;
    using namespace VaRest::Http::Testing;
    using namespace VaRest::Http::ExcerptTestPrivate;
    FVaRestTrace::FScopedSinkIsolation Isolation;
    FTestRig Rig;
    Rig.Fake->bCompleteOnWorker = false;
    auto Twice = MakeShared<FProceedTwice, ESPMode::ThreadSafe>();
    Rig.Client.Use(Twice, EVaRestStageSlot::PerAttempt);
    Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
    FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 1024);
    auto Result = MakeOutcome<FVaRestJson>();
    const uint64 Id = Observe(Rig.Client.Get(TEXT("/mixed-excerpt")), Result).Send().GetId();
    auto Start = First<FVaRestTraceAttemptDispatched>(*Scoped.Recorder, Id, EVaRestTraceKind::AttemptDispatched);
    if (!TestTrue(TEXT("real first dispatch"), Start.IsSet())) { Twice->Release(); Abandon(Rig); return false; }
    const uint64 FirstId = Start->DispatchId;
    FRedactionGate Gate;
    auto Release = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
    Gate.Start();
    ON_SCOPE_EXIT
    {
        Release->store(true);
        Gate.Wait(); // release BEFORE waiting, including assertion failures
        Twice->Release();
        Abandon(Rig);
        Gate.Stop();
    };
    HoldPipe(Gate, Release);
    Excerpts::FScopedResponseGate Route(Id, FirstId, Gate);
    Rig.Fake->Complete(0, Json(503, R"({"n":1})"));
    if (!TestTrue(TEXT("first actual current end"), PumpUntil([Twice] { return Twice->FirstEnds->load() == 1; }))) return false;
    TestTrue(TEXT("raw response admitted to owned held pipe"), Excerpts::PendingBytes() > 0);
    TestEqual(TEXT("first excerpt still held"), ResponseExcerpts(*Scoped.Recorder, Id, 1).Num(), 0);
    Twice->ProceedAgain();
    if (!TestEqual(TEXT("second physical dispatch"), Rig.Fake->NumCalls(), 2)) return false;
    TestEqual(TEXT("same retry attempt label"), Rig.Fake->GetCall(1).Request.Attempt, 1);
    Rig.Fake->Complete(1, Json(200, R"({"n":2})"));
    if (!TestTrue(TEXT("decoded second outcome"), Rig.Wait(Result))) return false;
    TArray<FVaRestTraceEvent> Before = ResponseExcerpts(*Scoped.Recorder, Id, 1);
    if (!TestEqual(TEXT("decode excerpt precedes held pipe"), Before.Num(), 1)) return false;
    const auto& Second = Before[0].Payload.Get<FVaRestTraceExcerptEvent>();
    const uint64 SecondId = Second.DispatchId;
    TestTrue(TEXT("distinct actual dispatch identities"), FirstId != 0 && SecondId != 0 && FirstId != SecondId);
    TestEqual(TEXT("decode payload"), Second.Excerpt.ToText(), FString(TEXT("{\"n\":2}")));
    TestTrue(TEXT("decode is not discarded/backlog"), Second.Excerpt.SkipReason == EVaRestExcerptSkip::None);
    Release->store(true);
    Gate.Wait();
    TArray<FVaRestTraceEvent> After = ResponseExcerpts(*Scoped.Recorder, Id, 1);
    if (!TestEqual(TEXT("both actual promises resolved"), After.Num(), 2)) return false;
    const auto& FirstLate = After[1].Payload.Get<FVaRestTraceExcerptEvent>();
    TestEqual(TEXT("late pipe retains first captured identity"), FirstLate.DispatchId, FirstId);
    TestEqual(TEXT("late pipe payload"), FirstLate.Excerpt.ToText(), FString(TEXT("{\"n\":1}")));
    TestTrue(TEXT("late pipe not discarded/backlog"), FirstLate.Excerpt.SkipReason == EVaRestExcerptSkip::None);
    TestEqual(TEXT("source admissions released"), Excerpts::PendingBytes(), int64(0));
    return true;
}
#endif
```

Route destruction precedes scope cleanup, removing selection before cleanup pumps cancellations. Gate.Wait/Stop occur only after Release is set; the gate and probe outlive their callbacks. Existing HoldPipe's watchdog is failure containment, not the ordering witness; the decisive assertion is that only the second dispatch's excerpt is present before explicit release. If native scheduling lacks worker execution (`-nothreading`), this test must report that the required concurrency configuration is unavailable rather than claim its interleaving. Under normal owned test runner workers it must execute, not skip. Add the same exact identity expectations to real Backlog/Discarded producer tests. A mutant stamping dispatch identity from Attempt fails FirstId!=SecondId or late identity; a late lookup of mutable exchange identity fails the late first-id assertion. No manually manufactured FJob substitutes for this regression.

### Task10: exact finite errors-by-kind values

Stats.h defines this value type, copied into every snapshot/stat group including the one overflow aggregate. Its switch is deliberately exhaustive for the pinned enum with an explicit future/invalid fallback; no enum numeric value indexes an unchecked array.

```cpp
struct FVaRestInspectorErrorCounts
{
    uint64 Canceled = 0, Connection = 0, Timeout = 0, Http = 0, Server = 0;
    uint64 Decode = 0, Auth = 0, Offline = 0, Config = 0, Unknown = 0;
    bool bSaturated = false;
    void Add(EVaRestErrorKind Kind)
    {
        uint64* Counter = &Unknown;
        switch (Kind)
        {
        case EVaRestErrorKind::Canceled: Counter = &Canceled; break;
        case EVaRestErrorKind::Connection: Counter = &Connection; break;
        case EVaRestErrorKind::Timeout: Counter = &Timeout; break;
        case EVaRestErrorKind::Http: Counter = &Http; break;
        case EVaRestErrorKind::Server: Counter = &Server; break;
        case EVaRestErrorKind::Decode: Counter = &Decode; break;
        case EVaRestErrorKind::Auth: Counter = &Auth; break;
        case EVaRestErrorKind::Offline: Counter = &Offline; break;
        case EVaRestErrorKind::Config: Counter = &Config; break;
        case EVaRestErrorKind::None: break;
        default: break;
        }
        if (*Counter == MAX_uint64) bSaturated = true;
        else ++*Counter;
    }
};
```

Caller condition is exactly `Completed.Outcome==EVaRestTraceOutcome::Error`; success/queued/canceled terminal classes remain separate. Add an Error/None malformed fixture and a cast future kind to verify Unknown; genuine OutcomeCanceled does not increment ErrorsByKind.Canceled. Sum comparisons apply before saturation; afterward report bSaturated instead of claiming exact arithmetic. Count terminal errors once under the same bounded duplicate-completion contract as other stats. Retry counts use GapKind precedence described in Task10, not this error-kind mapping.

### Tasks3–4: complete accepted/refused packet handoff fixture

Extend the exact Queue.h declaration with `static TOptional<int64> BaselineCharge(const FAllocationAdapter&);` returning the conservative allocation bound for the queue wrapper, PImpl and two sentinel nodes (no event slot), and `bool IsSupported() const`. Constructor reserves that byte-only baseline against its common pending account before allocating; a failed constructor creates no lanes and IsSupported=false. Queue destruction releases its baseline only after both queues are destroyed. FPendingAccount gains `std::atomic<int64> ExcerptReservations{0}` for the scheduling fixture; each admitted excerpt increments before the approved pre-Enqueue hook and decrements only after publication/refusal, independently of the ticket retained for queued bytes. Count remains the number of event reservations, not the two sentinels. The queue holds a byte-only baseline credit, separate from FPendingTicket's one-event reservation. Do not fake count slack by omitting baseline bytes.

Allocation.h also declares the following byte-only common pending baseline type, with the same move/reset behavior as the event ticket but no Count changes: `FPendingBaseline::Acquire(TSharedRef<FPendingAccount,ThreadSafe>, int64 Bytes) -> TOptional<FPendingBaseline>` uses TryReserve(Account.Bytes,ByteLimit,Bytes); destructor fetch_subs Bytes after allocator destruction. Thus event/reserved generations still share one ledger. It is not a new independent budget. Add exact methods `FPendingTicket::Bytes() const -> int64` and `IsValid() const -> bool` for codec development preconditions; rename the ticket's stored `Charge` member only if needed to avoid a method collision.

Store.h defines the actual value-transfer owner and custom unique deleter below. Include Budget.h (FStoreCredit defined there), Packet.h and Templates/UniquePtr.h. The adapter outlives these owners. This implementation reserves store wrapper/backing/reference capacities before relinquishing any pending credit and releases the wrapper's credit only after its own memory is freed. It does not create another shared controller. Store keeps it within its already admitted row; snapshots share/clone row owners using the separate controlled shared-owner adapter above.

```cpp
class FStorePacketOwner;
struct FStorePacketDeleter { void operator()(FStorePacketOwner* Object) const; };
using FStorePacketPtr = TUniquePtr<FStorePacketOwner, FStorePacketDeleter>;
class FStorePacketOwner
{
public:
    static TOptional<int64> Charge(const FPacket& Packet, const FAllocationAdapter& A)
    {
        auto Wrapper = A.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
        if (!Wrapper.IsSet()) return {};
        int64 Total = Packet.RetainedCapacity();
        if (!AddChecked(Total, int64(Wrapper.GetValue()), MAX_int64)) return {};
        return Total;
    }
    static bool TryTake(FPacket& InOut, TRefCountPtr<FBudgetAccount> Budget,
        FAllocationAdapter& Allocator, FStorePacketPtr& Out)
    {
        if (Out.IsValid()) return false;
        auto Needed = Charge(InOut, Allocator);
        if (!Needed.IsSet()) return false;
        auto Reserved = FStoreCredit::Acquire(Budget, Needed.GetValue());
        if (!Reserved.IsSet()) return false;
        auto Wrapper = Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
        check(Wrapper.IsSet());
        void* Storage = Allocator.AllocateReserved(sizeof(FStorePacketOwner),
            alignof(FStorePacketOwner), Wrapper.GetValue());
        if (InOut.Pending.IsValid() && Budget->UsedBytes.load() >= Needed.GetValue())
            Allocator.Counters().OverlapObservations.fetch_add(1);
        FStorePacketOwner* Owner = new(Storage) FStorePacketOwner(
            MoveTemp(Reserved.GetValue()), MoveTemp(InOut), Allocator);
        Owner->Data.Pending.Reset(); // only after independently admitted transfer
        Out.Reset(Owner);
        return true;
    }
    const FPacket& Get() const { return Data; }
private:
    friend struct FStorePacketDeleter;
    FStorePacketOwner(FStoreCredit&& InCredit, FPacket&& InData, FAllocationAdapter& A)
        : Credit(MoveTemp(InCredit)), Data(MoveTemp(InData)), Allocator(&A) {}
    FStoreCredit Credit;
    FPacket Data;
    FAllocationAdapter* Allocator;
};
inline void FStorePacketDeleter::operator()(FStorePacketOwner* Object) const
{
    if (!Object) return;
    FStoreCredit Credit = MoveTemp(Object->Credit);
    FAllocationAdapter* A = Object->Allocator;
    Object->~FStorePacketOwner(); // destroys backing/ref, pending already empty
    A->Free(Object);
    Credit.Reset(); // wrapper storage has now actually been freed
}
```

The one TryTake signature above uses the intrusive account root and custom-deleted FStorePacketPtr throughout Task4. FPacket move construction clears the old Backing/Allocator/capacity and moves both ticket and excerpt pointer; Reset performs ExcerptOwner.Reset, adapter Free(Backing), then Pending.Reset, and nulls all members. Move assignment first Reset()s its previous owned data, then performs that same transfer. `RetainedCapacity()` is backing **bound charged by its ticket** plus source excerpt allocated capacity, excludes the queue node now covered by the queue baseline, and is unchanged by dequeue; actual backing probes separately report GetAllocSize.

The following complete registered test belongs in StoreTest.cpp in Task4. The separate complete QueueAdmission test below belongs wholly to Task3 and depends on no Task4 store type; this Task4 fixture independently adds the overlap/transfer proof against the unchanged codec/queue contract. Bootstrap here is the test account's own pre-admitted allocation; the test retains a root until every packet/owner is destroyed. `MakeBudgetForTest(limit)` is a concrete local helper: assert limit≥4096, allocate one FBudgetAccount using `new FBudgetAccount(limit,4096)` on an audited≤1024byte/≤16alignment root path, immediately observe actual GetAllocSize(root)≤4096, wrap in TRefCountPtr and return; no child/controller ownership is hidden. Production uses the same checked bootstrap factory with refusal instead of a test assertion.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorPacketHandoffTest,
    "VaRest.Inspector.Store.PacketHandoff",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorPacketHandoffTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Inspector::Private;
    FAllocationCounters Probe;
    FAllocationAdapter Allocator(Probe);
    FCapturePolicy Policy;
    FVaRestTraceExcerptEvent Excerpt;
    Excerpt.Owner = EVaRestTraceExcerptOwner::Response;
    Excerpt.RequestId = 77; Excerpt.DispatchId = 5; Excerpt.Attempt = 1;
    Excerpt.Excerpt.StopReason = TEXT("safe capture sentinel");
    auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
    Bytes->Reserve(2048); Bytes->Add(uint8('x'));
    Excerpt.Excerpt.Bytes = Bytes;
    Excerpt.Excerpt.TotalBytes = 1;
    FVaRestTraceEvent Borrowed;
    Borrowed.Kind = EVaRestTraceKind::Excerpt;
    Borrowed.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Excerpt));
    FPacketLayout Layout;
    if (!TestTrue(TEXT("supported full measured layout"),
        FPacketCodec::Measure(Borrowed, Policy, Allocator, Layout) == EMeasure::Full)) return false;
    TestEqual(TEXT("measurement copies no text"), Probe.TextBytesCopied.load(), int64(0));
    TestEqual(TEXT("measurement acquires no byte ref"), Probe.ExcerptRefsAcquired.load(), int64(0));
    auto Base = FCaptureQueue::BaselineCharge(Allocator);
    if (!TestTrue(TEXT("supported queue baseline"), Base.IsSet())) return false;
    const int64 Baseline = Base.GetValue();
    {
        auto DeniedAccount = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8,
            Baseline + Layout.PendingCharge - 1);
        FCaptureQueue Denied(Policy, 1, DeniedAccount, Allocator);
        TestTrue(TEXT("baseline admitted"), Denied.IsSupported());
        // Scalar Backlog fallback is separately admitted and is permitted here.
        const EPush Result = Denied.TryPush(Borrowed);
        TestTrue(TEXT("full excerpt denied"), Result == EPush::ScalarOnly || Result == EPush::Dropped);
        TestEqual(TEXT("denial before excerpt reference"), Probe.ExcerptRefsAcquired.load(), int64(0));
        TestEqual(TEXT("denial before borrowed text copy"), Probe.TextBytesCopied.load(), int64(0));
        Denied.Close();
        while (Denied.TryPopExcerpt().IsSet()) {}
    }
    auto Pending = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8,
        Baseline + Layout.PendingCharge);
    {
        FCaptureQueue Queue(Policy, 2, Pending, Allocator);
        TestTrue(TEXT("queue available"), Queue.IsSupported());
        TestTrue(TEXT("exact byte capacity admitted"), Queue.TryPush(Borrowed) == EPush::Accepted);
        TestEqual(TEXT("one retained source reference"), Probe.ExcerptRefsAcquired.load(), int64(1));
        auto Dequeued = Queue.TryPopExcerpt();
        if (!TestTrue(TEXT("dequeued actual packet"), Dequeued.IsSet())) return false;
        TestEqual(TEXT("dequeued ticket still charged"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
        TestEqual(TEXT("captured identity"), Dequeued->Fixed().Scalars.DispatchId, uint64(5));
        TestEqual(TEXT("captured borrowed text"), FString(Dequeued->Text(EText::StopReason)), FString(TEXT("safe capture sentinel")));
        auto Needed = FStorePacketOwner::Charge(Dequeued.GetValue(), Allocator);
        if (!TestTrue(TEXT("store bound available"), Needed.IsSet())) return false;
        auto Small = MakeBudgetForTest(4096 + Needed.GetValue() - 1);
        FStorePacketPtr Stored;
        TestFalse(TEXT("store refuses before transfer"), FStorePacketOwner::TryTake(
            Dequeued.GetValue(), Small, Allocator, Stored));
        TestEqual(TEXT("refusal preserves pending owner"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
        auto Store = MakeBudgetForTest(4096 + Needed.GetValue());
        TestTrue(TEXT("exact store bound admits overlap transfer"), FStorePacketOwner::TryTake(
            Dequeued.GetValue(), Store, Allocator, Stored));
        TestEqual(TEXT("overlap actually observed"), Probe.OverlapObservations.load(), int64(1));
        TestEqual(TEXT("pending released after transfer"), Pending->Bytes.load(), Baseline);
        TestEqual(TEXT("store remains charged"), Store->UsedBytes.load(), int64(4096) + Needed.GetValue());
        Dequeued.Reset();
        TestTrue(TEXT("stored source byte still alive"), Stored.IsValid() && Stored->Get().Excerpt()->Num() == 1);
        Stored.Reset();
        TestEqual(TEXT("store charge released after actual free"), Store->UsedBytes.load(), int64(4096));
        // A second dequeued owner must retain charge until destruction without handoff.
        TestTrue(TEXT("second admission"), Queue.TryPush(Borrowed) == EPush::Accepted);
        auto Held = Queue.TryPopExcerpt();
        TestTrue(TEXT("second packet held"), Held.IsSet());
        TestEqual(TEXT("held ticket retained"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
        Held.Reset();
        TestEqual(TEXT("destruction releases pending"), Pending->Bytes.load(), Baseline);
        Queue.Close();
    }
    TestEqual(TEXT("queue baseline released last"), Pending->Bytes.load(), int64(0));
    TestEqual(TEXT("all consumer backing freed"), Probe.LiveBacking.load(), int64(0));
    return true;
}
```

Put the helper body immediately before this test:

```cpp
static TRefCountPtr<VaRest::Inspector::Private::FBudgetAccount> MakeBudgetForTest(int64 Limit)
{
    using namespace VaRest::Inspector::Private;
    check(Limit >= 4096 && sizeof(FBudgetAccount) <= 1024 && alignof(FBudgetAccount) <= 16);
    FBudgetAccount* Raw = new FBudgetAccount(Limit, 4096);
    check(FMemory::GetAllocSize(Raw) <= 4096);
    return TRefCountPtr<FBudgetAccount>(Raw);
}
```

Tests run only after allocator support/calibration is established by the native module fixture; unsupported runtime emits an explicit blocked capability diagnostic and fails this required normal-environment test, never silently passes/skips. Synthetic source MakeShared allocation belongs to the test producer, not the Inspector ledger. Probe counters measure the real consumer allocator and real retained-ref acquisition. Count slack=8 isolates byte refusal. The byte-overflow fallback must preserve Kind/RequestId/DispatchId and SkipBacklog without copying StopReason or acquiring the original Bytes; inspect that dequeued fallback in QueueAdmission as well. A mutation copying text/acquiring the ref before reservation fails the zero probe assertions; releasing at dequeue fails held-byte assertions; releasing pending before store acquisition fails the OverlapObservations invariant. The same unchanged test must fail then pass under the exact restored source versions.

### Exact remaining type/lifetime definitions for the boundary

Allocation.h's supported capability object is a value, established once for this audited engine build. It is not a project setting or opt-in override. Task2's module constructs the certificate from compiled platform/build checks, the live `FPlatformMemory::AllocatorToUse` enum (GenericPlatformMemory.h:263–279), matching GMalloc family, and the reviewed allocator-creation chain in UnrealMemory.cpp:347–415. The exact allowed chain is underlying Binned2 or mimalloc2.0.0 plus only the inspected transparent MemoryTrace wrapper and/or MallocPoisonProxy: MemoryTrace.cpp:205/232–239/809–834 forwards original size/alignment and GetAllocationSize, and MallocPoisonProxy.h:43–50/134–141 does likewise. No claim covers arbitrary runtime replacement. Verify/replay/stomp/double-free/frame-profiler/other proxy paths or an unverifiable chain yield no certificate. Fixed build/source/config facts are checked in the Task15/16 action/rsp/runtime evidence; a changed engine hash/config invalidates the certificate instead of reusing it on name alone. MemoryTrace/LLM instrumentation's own global storage is an upstream instrumentation cost, not Inspector payload capacity.

```cpp
struct FSupportedAllocator
{
    enum class EFamily : uint8 { Unsupported, Binned2, Mimalloc200 };
    EFamily Family = EFamily::Unsupported;
    bool bVerifiedModuleNew = false;
    bool bVerifiedNoInflatingProxy = false;
    bool bCalibrationPassed = false;
    bool SupportsController(SIZE_T Request, uint32 TypeAlignment) const
    {
        return Family != EFamily::Unsupported && bVerifiedModuleNew
            && bVerifiedNoInflatingProxy && bCalibrationPassed
            && sizeof(void*) == 8 && Request > 0 && Request <= 1024
            && TypeAlignment <= 16 && __STDCPP_DEFAULT_NEW_ALIGNMENT__ <= 16;
    }
};
class FPendingBaseline
{
public:
    FPendingBaseline() = default;
    FPendingBaseline(const FPendingBaseline&) = delete;
    FPendingBaseline& operator=(const FPendingBaseline&) = delete;
    FPendingBaseline(FPendingBaseline&& Other) noexcept
        : Account(MoveTemp(Other.Account)), Charge(Other.Charge) { Other.Charge = 0; }
    FPendingBaseline& operator=(FPendingBaseline&& Other) noexcept
    {
        if (this != &Other)
        {
            Reset(); Account = MoveTemp(Other.Account);
            Charge = Other.Charge; Other.Charge = 0;
        }
        return *this;
    }
    ~FPendingBaseline() { Reset(); }
    static TOptional<FPendingBaseline> Acquire(
        TSharedRef<FPendingAccount,ESPMode::ThreadSafe> A, int64 Bytes)
    {
        if (!TryReserve(A->Bytes, A->ByteLimit, Bytes)) return {};
        FPendingBaseline Out; Out.Account = A; Out.Charge = Bytes;
        return TOptional<FPendingBaseline>(MoveTemp(Out));
    }
    void Reset()
    {
        if (Account)
        {
            Account->Bytes.fetch_sub(Charge, std::memory_order_acq_rel);
            Charge = 0; Account.Reset();
        }
    }
private:
    TSharedPtr<FPendingAccount,ESPMode::ThreadSafe> Account;
    int64 Charge = 0;
};
```

The pending account/adapter's own one-time root/controller is fixed baseline storage owned across all generations, has no externally exposed weak pointer and no child pointers; include its exact allocation capacity in that common account's bootstrap. It is not recreated on each Clear. FPendingTicket gains the complete inline methods `bool IsValid() const { return Account.IsValid(); }` and `int64 Bytes() const { return Charge; }`. These are the only reservation names used by codec/queue/store. FPendingBaseline releases bytes only, while FPendingTicket releases bytes and one event count.

FPacket's pending ticket is first in member order, but explicit cleanup is used to make allocator and overlap sequencing reviewable. The moved-from object is valid empty state. The code belongs in Packet.cpp:

```cpp
FPacket::FPacket(FPacket&& Other) noexcept
    : Pending(MoveTemp(Other.Pending)), Allocator(Other.Allocator), Backing(Other.Backing),
      BackingCapacity(Other.BackingCapacity), ExcerptOwner(MoveTemp(Other.ExcerptOwner))
{
    Other.Allocator = nullptr; Other.Backing = nullptr; Other.BackingCapacity = 0;
}
FPacket& FPacket::operator=(FPacket&& Other) noexcept
{
    if (this != &Other)
    {
        Reset(); Pending = MoveTemp(Other.Pending);
        Allocator = Other.Allocator; Backing = Other.Backing;
        BackingCapacity = Other.BackingCapacity; ExcerptOwner = MoveTemp(Other.ExcerptOwner);
        Other.Allocator = nullptr; Other.Backing = nullptr; Other.BackingCapacity = 0;
    }
    return *this;
}
FPacket::~FPacket() { Reset(); }
void FPacket::Reset()
{
    ExcerptOwner.Reset();
    if (Backing) { check(Allocator); Allocator->Free(Backing); }
    Backing = nullptr; BackingCapacity = 0; Allocator = nullptr;
    Pending.Reset();
}
const FPacketFixed& FPacket::Fixed() const
{
    check(Backing); return *static_cast<const FPacketFixed*>(Backing);
}
const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>& FPacket::Excerpt() const
{
    return ExcerptOwner;
}
int64 FPacket::RetainedCapacity() const
{
    return int64(BackingCapacity) + (ExcerptOwner ? ExcerptOwner->GetAllocatedSize() : 0);
}
```

Here BackingCapacity is the admitted backing bound, conservatively≥actual; the allocator stores actual size for LiveBacking observation. Owner credit deliberately retains the full bound. Source excerpt capacity is charged per retained reference. To write the packet, Codec::Write checks Reservation.IsValid and Reservation.Bytes()==Layout.PendingCharge, AllocateReserveds Layout.BackingRequest with Layout.BackingBound, placement-constructs FPacketFixed from Layout.Fixed, copies bounded header/warning slice arrays, then copies each measured BorrowedText span/terminator and increments TextBytesCopied by actual copied TCHAR bytes. Only after these operations, when bKeepExcerpt, copy Borrowed.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes and increment ExcerptRefsAcquired. Layout has no mutable pointers to Source. Set FPacket.Pending before publishing; any owned data is destroyed before ticket release on failure. This algorithm uses only the complete declared types and covers every branch without a callback into the borrowed producer.

For Stats.h, the finite read value and key are exactly:

```cpp
struct FVaRestInspectorHttpStats
{
    uint64 ScopeId = 0, ClientId = 0;
    FName Endpoint;
    FString Key; // already redacted; owned/key capacity separately admitted
    bool bOverflow = false, bSaturated = false;
    uint64 Started = 0, Completed = 0, Success = 0, Error = 0, Queued = 0, Canceled = 0;
    uint64 Dispatches = 0, Retries = 0, AuthReplays = 0, RetryScheduled = 0;
    uint64 UnknownDispatchClassification = 0;
    uint64 CacheHit = 0, CacheDisk = 0, Revalidated = 0, Stale = 0, Replayed = 0;
    uint64 WireBytesUp = 0, WireBytesDown = 0, Partial = 0, Incomplete = 0, Dropped = 0;
    FVaRestInspectorErrorCounts ErrorsByKind;
    uint64 Histogram[512] = {};
    uint64 DurationCount = 0, DurationUnderflow = 0, DurationOverflow = 0;
    double DurationSum = 0, DurationMin = 0, DurationMax = 0;
};
```

Snapshot stats copying reserves the copied Key capacities and fixed value storage, so this public value is not an unaccounted FString duplication. Task2 may forward-declare this type and return no stats, while the full definition lands Task10 before actual stats visitors are called. The typed ErrorsByKind lives in the same public header and is copied with the value. Existing historical stats survive record FIFO eviction; only Clear resets them, while lifetime high-water/cap-violation diagnostics remain outside the per-capture stats reset.

Allocation.h declares `struct FAllocationLifetime final : FRefCountedObject { FAllocationCounters Probe; FAllocationAdapter Allocator; FAllocationLifetime() : Allocator(Probe) {} };`. Add `TRefCountPtr<FAllocationLifetime> Lifetime;` to both FPendingAccount and FBudgetAccount, initialized by their production factory before creating any children. The common service creates one context, reserves its conservative actual allocation bound in both fixed bootstrap charges, and gives each account a root reference. Context owns neither account; last child release can destroy the account, then context, on any thread. Test-only stack adapters are allowed solely in fixtures that destroy all packets/accounts before the adapter's scope ends, as PacketHandoff does. The type is defined after FAllocationAdapter; accounts forward-declare it and implement their destructors out of line where complete, so Allocation.h remains acyclic. Task3 creates it with the adapter; Task4 adds the store-account reference.

Before the first Tick of a new generation, the control path must call BeginGeneration(NewId,0) before its attach walk; normal ticks never rebase. The recovery caller's exact order per GT Tick is: Observe(LossEpoch,OrphanEpoch,LastPublishedMetadata); pop metadata within its192-first allowance, set LastConsumedMetadataOrdinal to each successfully published ordinal **before** applying it, and apply/drop it even if store admission fails; pop excerpts within the remaining allowance, creating only unpinned orphan rows and incrementing OrphanEpoch on GT; Observe again; if CanRun, capture one source snapshot, mark live-present rows/admit snapshot-born rows, scan eligible absent rows once, destroy source snapshot, Complete(Now); Observe once more to rearm any new pair; resolve at most256 expiry entries; notify consistent state. The exact faulty scheduling mutation is `if (Recovery.CanRun(Now,LastConsumedMetadataOrdinal) && Queue.OutstandingExcerptReservations()==0)` in place of the correct `if (Recovery.CanRun(...))`. The held publisher has reserved and incremented that counter but has not entered Enqueue; its unchanged test therefore exposes this predicate without pretending to pause the engine queue internally. Scope cleanup releases the publisher before joining/draining on every failure exit.

## Fix2: direct borrowed attachment, independent queue fixture and generation eligibility

These are corrections within Tasks3/4/7/11, not extra tasks or steps. Packet.h/Packet.cpp own the following borrowed Started codec. The ordinary event Measure branch for Started calls MeasureStarted with the actual event header; it does not copy the Started variant. The direct attach caller constructs only a scalar FPacketHeader with bSnapshot=true, Seq=0 and MetadataOrdinal=0. MeasureStarted borrows every FString/header pair, creates no owning event, acquires no reference and allocates nothing. Its direct packet charge excludes a queue node because it never enters a queue; the transient common pending account still reserves one event count until store transfer/destruction. Snapshot policy applies the same caps and scalar-only omission as an ordinary Started.

```cpp
EMeasure FPacketCodec::MeasureStarted(const FVaRestTraceStarted& B,
    FPacketHeader Header, const FCapturePolicy& Policy,
    const FAllocationAdapter& A, FPacketLayout& Out)
{
    Out = FPacketLayout{};
    if (Header.Kind != EVaRestTraceKind::Started ||
        Header.Source != EVaRestTraceSource::Http) return EMeasure::Unsupported;
    Out.Fixed.Header = Header;
    auto& S = Out.Fixed.Scalars;
    S.RequestId = B.RequestId; S.ScopeId = B.ScopeId; S.ClientId = B.ClientId;
    S.OriginalRequestId = B.QueueReplayOf.OriginalRequestId;
    S.Endpoint = B.Endpoint; S.Tag = B.Tag; S.Priority = B.Priority; S.Flags = B.Flags;
    S.CachePolicy = B.CachePolicy; S.CacheLookup = B.CacheLookup;
    S.bCacheLookupPending = B.bCacheLookupPending;
    S.BodyBytes = B.Body.Bytes; S.BodyKind = B.Body.Kind;
    S.bExcerptPending = Policy.bBodies && B.bExcerptPending;
    S.StartUtc = B.StartUtc; S.CurrentAttempt = B.CurrentAttempt; S.StartedAt = B.StartedAt;
    S.ScriptCodeOffset = B.Initiator.ScriptCodeOffset; S.SourceLine = B.Initiator.SourceLine;
    if (Header.bSnapshot) S.Flags |= EVaRestTraceRequestFlags::Partial;
    const FPacketFixed Scalars = Out.Fixed; // scalars/slices only; no owning text
    const int32 PairCount = Policy.bHeaders ? B.Headers.Num() : 0;
    bool bFits = PairCount >= 0 && PairCount <= FNormalizedLimits::MaxHeaders;
    SIZE_T Cursor = Align(sizeof(FPacketFixed), alignof(FPairSlice));
    if (bFits && PairCount)
    {
        Out.Fixed.Headers = {uint32(Cursor), uint32(PairCount)};
        Cursor += SIZE_T(PairCount) * sizeof(FPairSlice);
    }
    Cursor = Align(Cursor, alignof(TCHAR));
    auto AddText = [&](FStringView Text, FTextSlice& Destination)
    {
        if (!bFits) return;
        if (Text.Len() > FNormalizedLimits::MaxStringChars ||
            Out.TextCount >= FPacketLayout::MaxTexts) { bFits = false; return; }
        if (Text.IsEmpty()) { Destination = {}; return; }
        const SIZE_T Bytes = (SIZE_T(Text.Len()) + 1) * sizeof(TCHAR);
        if (Cursor > SIZE_T(FNormalizedLimits::MaxMetadataBytes) ||
            Bytes > SIZE_T(FNormalizedLimits::MaxMetadataBytes) - Cursor)
        { bFits = false; return; }
        Destination = {uint32(Cursor), uint32(Text.Len())};
        Out.BorrowedText[Out.TextCount] = Text;
        Out.TextDestination[Out.TextCount++] = Destination;
        Cursor += Bytes;
    };
    auto AddField = [&](EText Field, const FString& Text)
    { AddText(FStringView(Text), Out.Fixed.Text[static_cast<int32>(Field)]); };
    AddField(EText::Verb, B.Verb); AddField(EText::Url, B.Url);
    AddField(EText::PathTemplate, B.PathTemplate);
    AddField(EText::QueueId, B.QueueReplayOf.QueueId);
    AddField(EText::OwnerPath, B.Initiator.OwnerPath);
    AddField(EText::ScriptObject, B.Initiator.ScriptObject);
    AddField(EText::ScriptFunction, B.Initiator.ScriptFunction);
    AddField(EText::SourceFile, B.Initiator.SourceFile);
    AddField(EText::BodyContentType, B.Body.ContentType);
    if (bFits && Policy.bHeaders)
    {
        int32 Index = 0;
        for (const auto& Pair : B.Headers.GetPairs())
        {
            AddText(FStringView(Pair.Key), Out.Headers[Index].First);
            AddText(FStringView(Pair.Value), Out.Headers[Index].Second);
            ++Index;
            if (!bFits) break;
        }
    }
    if (!bFits)
    {
        Out = FPacketLayout{};
        Out.Fixed = Scalars;
        Out.Fixed.Header.bMetadataOmitted = true;
        Cursor = sizeof(FPacketFixed);
    }
    const auto Bound = A.Bound(Cursor, alignof(FPacketFixed));
    if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64)) return EMeasure::Unsupported;
    Out.BackingRequest = Cursor; Out.BackingBound = Bound.GetValue();
    Out.PendingCharge = int64(Out.BackingBound);
    if (!Header.bSnapshot && !AddChecked(Out.PendingCharge, int64(A.QueueNodeBound()), MAX_int64))
        return EMeasure::Unsupported;
    // Started carries only a promise, never an excerpt byte owner.
    Out.ExcerptCapacity = 0; Out.bKeepExcerpt = false;
    return bFits ? EMeasure::Full : EMeasure::ScalarOnly;
}
FPacket FPacketCodec::WriteLayout(const FPacketLayout& L, FPendingTicket&& Reservation,
    FAllocationAdapter& A,
    const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* BorrowedExcerpt)
{
    check(Reservation.IsValid() && Reservation.Bytes() == L.PendingCharge);
    FPacket Out;
    Out.Pending = MoveTemp(Reservation); // first retained owner, before any allocation
    Out.Allocator = &A;
    Out.BackingCapacity = L.BackingBound;
    Out.Backing = A.AllocateReserved(L.BackingRequest, alignof(FPacketFixed), L.BackingBound);
    new(Out.Backing) FPacketFixed(L.Fixed);
    auto* Bytes = static_cast<uint8*>(Out.Backing);
    if (L.Fixed.Headers.Count)
        FMemory::Memcpy(Bytes + L.Fixed.Headers.Offset, L.Headers,
            SIZE_T(L.Fixed.Headers.Count) * sizeof(FPairSlice));
    if (L.Fixed.Warnings.Count)
        FMemory::Memcpy(Bytes + L.Fixed.Warnings.Offset, L.Warnings,
            SIZE_T(L.Fixed.Warnings.Count) * sizeof(FPairSlice));
    for (int32 Index = 0; Index < L.TextCount; ++Index)
    {
        const FTextSlice Dest = L.TextDestination[Index];
        const SIZE_T Count = SIZE_T(Dest.Chars) * sizeof(TCHAR);
        check(SIZE_T(Dest.Offset) + Count + sizeof(TCHAR) <= L.BackingRequest);
        FMemory::Memcpy(Bytes + Dest.Offset, L.BorrowedText[Index].GetData(), Count);
        *reinterpret_cast<TCHAR*>(Bytes + Dest.Offset + Count) = TCHAR(0);
        A.Counters().TextBytesCopied.fetch_add(int64(Count + sizeof(TCHAR)));
    }
    if (L.bKeepExcerpt)
    {
        check(BorrowedExcerpt && BorrowedExcerpt->IsValid());
        Out.ExcerptOwner = *BorrowedExcerpt;
        A.Counters().ExcerptRefsAcquired.fetch_add(1);
    }
    return Out;
}
FPacket FPacketCodec::WriteStarted(const FVaRestTraceStarted& Borrowed,
    const FPacketLayout& Layout, FPendingTicket&& Reservation, FAllocationAdapter& A)
{
    check(Layout.Fixed.Header.Kind == EVaRestTraceKind::Started);
    check(Layout.Fixed.Scalars.RequestId == Borrowed.RequestId && !Layout.bKeepExcerpt);
    return WriteLayout(Layout, MoveTemp(Reservation), A, nullptr);
}
FPacket FPacketCodec::Write(const FVaRestTraceEvent& Borrowed,
    const FPacketLayout& Layout, FPendingTicket&& Reservation, FAllocationAdapter& A)
{
    const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt = nullptr;
    if (Layout.bKeepExcerpt)
    {
        check(Borrowed.Kind == EVaRestTraceKind::Excerpt);
        Excerpt = &Borrowed.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes;
    }
    return WriteLayout(Layout, MoveTemp(Reservation), A, Excerpt);
}
```

For normal Measure, its Started branch passes the real Seq/Time/Generation/MetadataOrdinal and bSnapshot=false to the method above; packet publication still assigns the actual successful metadata ordinal on GT. No event or string is constructed to call that branch. WriteLayout replaces the earlier prose implementation of Write; every measured kind uses the same reserved allocation/copy/ref path. Unsupported allocator bounds refuse without allocation; scalar-only layouts are independently reserved before writing.

Task4 adds the following exact Store.h declarations. `EAttachedBase` and the helper methods are private; ApplyAttached/BeginGeneration remain the private-module store surface. `PrepareAttachedBase(RequestId, bool& bNeedsStarted)` performs only scalar lookup/base admission: terminal-known/sealed rows return Terminal without promotion; existing mutable rows are marked source-present in the current reconciliation stamp, Partial and pinned; missing rows reserve count/base/index/ring/controller bytes before insertion and start Partial with current consumed ordinal. It never reads/copies a source string. bNeedsStarted is true only when the row has no retained Started packet. `AdoptAttachedPacket` takes an already admitted FStorePacketPtr, installs it in that row's empty Started slot and applies scalar known bits/promise state; it allocates nothing and cannot fail. The ordinary Apply(Started) branch uses this same installed-owner operation after its own admission. `NoteAttachLoss` saturates the fixed attach-loss counter, marks metadata unavailable on the admitted base, and advances GT OrphanEpoch so later reconciliation is coalesced; it does not allocate a missing-id entry. A minimal source-present row can remain admitted if its optional packet fails; other items continue.

```cpp
enum class EAttachedBase : uint8 { Refused, Terminal, Mutable };
// Members of FStore (Budget, Generation, LastConsumedMetadataOrdinal and
// CurrentReconciliationStamp are existing scalar/account state):
EAttachedBase PrepareAttachedBase(uint64 RequestId, bool& bNeedsStarted);
void AdoptAttachedPacket(uint64 RequestId, FStorePacketPtr&& Packet);
void NoteAttachLoss(uint64 RequestId);
bool ApplyAttachedStarted(const FVaRestTraceStarted& Borrowed, const FCapturePolicy& Policy,
    TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator);

bool FStore::ApplyAttachedStarted(const FVaRestTraceStarted& B, const FCapturePolicy& Policy,
    TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator)
{
    check(IsInGameThread());
    bool bNeedsStarted = false;
    const EAttachedBase Base = PrepareAttachedBase(B.RequestId, bNeedsStarted);
    if (Base == EAttachedBase::Refused) return false;
    if (Base == EAttachedBase::Terminal || !bNeedsStarted) return true;
    FPacketHeader Header;
    Header.Kind = EVaRestTraceKind::Started; Header.Source = EVaRestTraceSource::Http;
    Header.bSnapshot = true; Header.Generation = Generation; Header.Time = B.StartedAt;
    // Seq and MetadataOrdinal intentionally remain0: this is not publication.
    FPacketLayout Layout;
    const EMeasure Measured = FPacketCodec::MeasureStarted(B, Header, Policy, Allocator, Layout);
    if (Measured == EMeasure::Unsupported) { NoteAttachLoss(B.RequestId); return false; }
    auto Ticket = FPendingTicket::Acquire(Pending, Layout.PendingCharge);
    if (!Ticket.IsSet()) { NoteAttachLoss(B.RequestId); return false; }
    FPacket Packet = FPacketCodec::WriteStarted(B, Layout, MoveTemp(Ticket.GetValue()), Allocator);
    FStorePacketPtr Stored;
    if (!FStorePacketOwner::TryTake(Packet, Budget, Allocator, Stored))
    { NoteAttachLoss(B.RequestId); return false; } // Packet frees before ticket resets
    AdoptAttachedPacket(B.RequestId, MoveTemp(Stored));
    if (Measured == EMeasure::ScalarOnly) NoteAttachLoss(B.RequestId);
    return true;
}
```

`ApplyAttached(const FVaRestTraceSnapshot& Snapshot, const FCapturePolicy& Policy, TSharedRef<FPendingAccount,ESPMode::ThreadSafe> Pending, FAllocationAdapter& A)` is synchronous GT: start the current reconciliation stamp; apply bounded scalar/client metadata using the existing client admission rules; for each `const FVaRestTraceStarted& B : Snapshot.InFlight` call ApplyAttachedStarted(B,Policy,Pending,A) directly; finish the one retained-row absence walk with the current generation's consumed ordinal as W. No queue method is called. The service's existing one-argument ApplyAttached supplies its immutable policy/common pending account/retained allocator to this store signature before the sink is published; it never retains Snapshot. Recovery uses the same per-entry borrowed helper within its fixed-W walk but finishes with that recovery W, not the attach W. A completed/sealed row is never revived by an anomalous source duplicate. Source stamp marking precedes optional packet copy, so transient packet refusal cannot turn a known live row into false source absence.

Task4's existing StoreTest adds these exact direct-attach controls in its existing test step: a borrowed Started with RequestId77, Url `https://example.test/attached`, one `X-Test: visible` header and StartedAt100; no source Event is built. First measure full at zero TextBytesCopied/ExcerptRefsAcquired. With row count/store space available but common pending byte limit one below Layout.PendingCharge, ApplyAttached leaves an admitted Partial scalar base, copies no text/header, takes no excerpt ref and records one attach loss. With exact pending capacity and sufficient independently admitted store wrapper/backing, the same borrowed entry fills that base synchronously; assert exact Url/header, bSnapshot=true, Seq0/MetadataOrdinal0 and unchanged published ordinal/empty lanes. Reapply the identical attached entry and assert no additional TextBytesCopied, no extra row and no second Started owner; its live-presence stamp still updates. A separately saturated row-count/store-base case refuses before codec copying despite pending slack. Retain the source's original Url/header values throughout and verify unchanged afterward. Clear destroys transferred packet backing and restores pending count/bytes to their prior baseline. This uses the declared direct path, not TryPushSnapshot, and leaves Task4's independent PacketHandoff overlap proof unchanged.

### Complete Task3 QueueAdmission fixture (no Store/Budget dependency)

The following is the full real-event owning test in `Private/Tests/VaRestInspectorQueueTest.cpp`. Include Packet.h, Queue.h, Allocation.h, Misc/AutomationTest.h and Misc/ScopeExit.h directly. Its source-owned test byte array/event construction occurs before Inspector measurement; it deliberately uses MoveTemp into the source event. The Inspector then only borrows that const event. Source ref-count baseline is observed with public GetSharedReferenceCount; accumulated ExcerptRefsAcquired counts admissions, not currently live references. Both checks are required. Stack adapter/probe outlive both queue scopes and every packet; scope exits close/drain on all early returns. There is no Store.h/Budget.h, FStorePacketOwner, FBudgetAccount or MakeBudgetForTest dependency.

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorQueueAdmissionTest,
    "VaRest.Inspector.Store.QueueAdmission",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorQueueAdmissionTest::RunTest(const FString& Parameters)
{
    using namespace VaRest::Inspector::Private;
    FAllocationCounters Probe;
    FAllocationAdapter Allocator(Probe);
    FCapturePolicy Policy;
    auto Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>();
    Bytes->Reserve(2048); Bytes->Add(uint8('x'));
    FVaRestTraceExcerptEvent Excerpt;
    Excerpt.Owner = EVaRestTraceExcerptOwner::Response;
    Excerpt.RequestId = 77; Excerpt.DispatchId = 5; Excerpt.Attempt = 1;
    Excerpt.Excerpt.StopReason = TEXT("safe capture sentinel");
    Excerpt.Excerpt.Bytes = Bytes; Excerpt.Excerpt.TotalBytes = 1;
    FVaRestTraceEvent Source;
    Source.Kind = EVaRestTraceKind::Excerpt;
    Source.Payload = FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Excerpt));
    const FVaRestTraceEvent& Borrowed = Source;
    const int32 SourceRefs = Bytes.GetSharedReferenceCount();
    FPacketLayout Layout;
    if (!TestTrue(TEXT("full borrowed layout"),
        FPacketCodec::Measure(Borrowed, Policy, Allocator, Layout) == EMeasure::Full)) return false;
    TestEqual(TEXT("measure no text copy"), Probe.TextBytesCopied.load(), int64(0));
    TestEqual(TEXT("measure no excerpt acquisition"), Probe.ExcerptRefsAcquired.load(), int64(0));
    TestEqual(TEXT("measure leaves actual refs"), Bytes.GetSharedReferenceCount(), SourceRefs);
    auto Base = FCaptureQueue::BaselineCharge(Allocator);
    if (!TestTrue(TEXT("supported queue baseline"), Base.IsSet())) return false;
    const int64 Baseline = Base.GetValue();
    auto DeniedAccount = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8,
        Baseline + Layout.PendingCharge - 1);
    {
        FCaptureQueue Denied(Policy, 1, DeniedAccount, Allocator);
        ON_SCOPE_EXIT
        {
            Denied.Close();
            while (Denied.TryPopMetadata().IsSet()) {}
            while (Denied.TryPopExcerpt().IsSet()) {}
        };
        if (!TestTrue(TEXT("denied queue baseline available"), Denied.IsSupported())) return false;
        TestEqual(TEXT("count slack before byte refusal"), DeniedAccount->Count.load(), int64(0));
        const EPush Result = Denied.TryPush(Borrowed);
        TestTrue(TEXT("full payload refused by bytes"), Result == EPush::ScalarOnly || Result == EPush::Dropped);
        TestEqual(TEXT("no rejected text copied"), Probe.TextBytesCopied.load(), int64(0));
        TestEqual(TEXT("no rejected excerpt acquired"), Probe.ExcerptRefsAcquired.load(), int64(0));
        TestEqual(TEXT("actual refs unchanged by refusal"), Bytes.GetSharedReferenceCount(), SourceRefs);
    }
    TestEqual(TEXT("denied count cleaned"), DeniedAccount->Count.load(), int64(0));
    TestEqual(TEXT("denied bytes cleaned"), DeniedAccount->Bytes.load(), int64(0));
    TestEqual(TEXT("denied backing freed"), Probe.LiveBacking.load(), int64(0));
    auto Pending = MakeShared<FPendingAccount, ESPMode::ThreadSafe>(8,
        Baseline + Layout.PendingCharge);
    {
        FCaptureQueue Queue(Policy, 2, Pending, Allocator);
        ON_SCOPE_EXIT
        {
            Queue.Close();
            while (Queue.TryPopMetadata().IsSet()) {}
            while (Queue.TryPopExcerpt().IsSet()) {}
        };
        if (!TestTrue(TEXT("exact-capacity queue available"), Queue.IsSupported())) return false;
        if (!TestTrue(TEXT("exact bytes admitted"), Queue.TryPush(Borrowed) == EPush::Accepted)) return false;
        TestEqual(TEXT("one acquired excerpt"), Probe.ExcerptRefsAcquired.load(), int64(1));
        TestEqual(TEXT("actual packet ref"), Bytes.GetSharedReferenceCount(), SourceRefs + 1);
        TestTrue(TEXT("accepted text actually copied"), Probe.TextBytesCopied.load() > 0);
        auto Held = Queue.TryPopExcerpt();
        if (!TestTrue(TEXT("actual dequeued packet"), Held.IsSet())) return false;
        TestEqual(TEXT("dequeued count retained"), Pending->Count.load(), int64(1));
        TestEqual(TEXT("dequeued bytes retained"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
        TestEqual(TEXT("dispatch identity"), Held->Fixed().Scalars.DispatchId, uint64(5));
        TestEqual(TEXT("text survives callback"), FString(Held->Text(EText::StopReason)),
            FString(TEXT("safe capture sentinel")));
        TestTrue(TEXT("source bytes retained"), Held->Excerpt().IsValid() && Held->Excerpt()->Num() == 1);
        Queue.Close();
        TestTrue(TEXT("closed queue cannot admit"), Queue.TryPush(Borrowed) == EPush::Closed);
        TestEqual(TEXT("held charge survives close"), Pending->Bytes.load(), Baseline + Layout.PendingCharge);
        Held.Reset();
        TestEqual(TEXT("payload destruction returns count"), Pending->Count.load(), int64(0));
        TestEqual(TEXT("payload destruction leaves baseline only"), Pending->Bytes.load(), Baseline);
        TestEqual(TEXT("actual ref released"), Bytes.GetSharedReferenceCount(), SourceRefs);
    }
    TestEqual(TEXT("last queue baseline released"), Pending->Bytes.load(), int64(0));
    TestEqual(TEXT("all actual Inspector backing freed"), Probe.LiveBacking.load(), int64(0));
    return true;
}
```

The other Task3 boundary table cases remain in this test's owning file and step. This complete fixture is the required real-event proof, not a request to extract a fragment of Task4's later test. Its unchanged refusal assertions fail if text/ref acquisition moves before reservation; held charge assertions fail if Dequeue resets the packet ticket. Task4 separately proves store/pending overlap with its complete PacketHandoff fixture.

### Task11 current-generation rebase and exact cross-generation witness

`FStore::BeginGeneration(uint64 NewGeneration, uint64 ConsumedOrdinal)` is GT-only and executes once after the old generation closes and before the new sink's synchronous OnAttached live-presence walk. It sets store Generation/LastConsumedMetadataOrdinal to the new values; in one bounded pass over at mostMaxRecords, every retained mutable NON-terminal row gets BirthMetadataOrdinal=ConsumedOrdinal and its current source-presence stamp cleared. Preserve arrival FIFO position, original request identity, history, Partial state, source timestamps and recorded terminal data. Terminal-known or sealed rows are untouched and never promoted by attach. The service resets the generation-local scheduler/OrphanEpoch and ordinal counters before calling BeginGeneration; no old successful queue item is applied afterward. A failed new generation admission leaves capture paused with interrupted unpinned rows; it does not re-enable an old sink. Initial Start and Clear on an empty store follow the same call order. Pause/Resume and ordinal-overflow rotation MUST share this operation, rather than changing generation scalar alone. No registry of prior generations is needed.

The full control order is: close old admission → remove old sink → discard currently drainable old packets → mark retained open rows interrupted/unpinned → admit new generation baseline → reset its local scheduler/counters → BeginGeneration(NewId,0) → AddSink invokes synchronous ApplyAttached and one absence walk → publish new active sink. Already-admitted old workers may still finish into their retained closed old queue, but its packets never enter the new store stream; subsequent old callbacks refuse before copy. Resume source presence pins a retained live Partial; absence leaves it unpinned. Normal per-tick recovery does NOT rebase: it compares only this active generation's already rebased/born ordinals to its fixed W.

Add the following exact table row to Task11 Step4's existing OverflowRecovery test (no new step/test infrastructure):

1. In A, publish/drain99 benign metadata items, then real Started(R) as ordinal100; hold the real request live. Assert BirthMetadataOrdinal100, active pin, and preserve its arrival/history identity for comparison.
2. Pause/Resume B with source snapshot containing R. Assert B published/consumed ordinals0, retained R BirthMetadataOrdinal0, unchanged original arrival/history, Partial=true and pinned. Keep a second real request L live as the presence control, with its retained row likewise rebased.
3. Exhaust B's common pending bytes using an admitted held excerpt reservation AFTER B's baseline/attach has succeeded. Leave count slack. Complete R; both its normal Completed packet and independently measured scalar fallback must be refused. Assert B LossEpoch advanced, PublishedMetadataOrdinal0 and no actual Completed applied. The production source snapshot now omits R and includes L.
4. With the excerpt publisher still held and no further producer events, tick at the eligible injected-clock time. Recovery captures W0, sees R absent and L present, unpins R as CaptureInterrupted/terminal-details-unavailable, keeps L pinned, and marks the loss handled. Assert R has no invented Success/Error/Canceled or completion time. A further quiet tick does not re-pin R. Release the held worker in scope cleanup before any join/drain, including assertion failure; no internal engine queue gap is claimed.
5. Separate enrichment control: retain one real B Completed in the metadata lane behind an already frozen W0 (use the existing deterministic scheduler-step fixture); reconcile source absence first, then consume that actual Completed. It enriches the same unsealed R row with the real terminal metadata and eventually seals by the normal promise rule. It does not create a second row or fabricate a replacement completion. This control allows a successful B publication; it is separate from step3's strict no-publication witness.
6. Repeat the retained-row transition using the ordinal-overflow control path: set only the test-owned scalar ordinal nearMAX_uint64 through the existing service fixture, trigger fail-closed rotation, and assert the SAME BeginGeneration/attach rebase and W0 loss recovery. Never perform2^64 sends. Hold an old registry-list owner, invoke its callback after closure, and assert no new copy/admission/store mutation; a previously admitted old publisher may only drain/destroy within its old queue. No old ordinal is compared to B's W.

Restore-the-bug witness: omit only BeginGeneration's retained-mutable-row ordinal assignment while keeping the rest of the code/test unchanged. A:100 then B:W0 cannot unpin R, so the unchanged step4 assertion fails. Restore exactly and run the existing guarded Lifecycle/OverflowRecovery tests. The still-live and later-real-completion controls prevent a blanket unpin/drop-history workaround. This adds explicit cross-generation meaning to the existing birth scalar without changing fixed-W,250ms/no-new-event rearm or worker responsibilities.
