# VaRest 2 — M4b cache, offline queue and connectivity Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for execution instructions; Beads owns task status.

**Goal:** Complete core M4 with isolated HTTP caching, durable account-scoped FIFO replay, observable connectivity, and the Inspector cache/offline trace contract.

**Architecture:** Two built-in outer stages precede the landed Retry stage: Offline then Cache. Client state owns a memory cache and subscribes to an injectable connectivity object; immutable worker inputs, a serial persistence executor, and game-thread continuations own disk activity. Named queues share one process-local coordinator, but every persisted operation carries an identity scope and can replay only with that identity's current credentials.

**Tech Stack:** Unreal Engine 5.8, C++20, UE::Tasks, existing VaRestJson/FSha256/FVaRestVirtualClock/FVaRestFakeTransport, engine file APIs, Python standard-library test server. No additional third-party dependency.

**Spec:** `docs/superpowers/specs/2026-10-18-varest2-core-design.md` §§1, 3.1–3.3, 4.3, 4.4, 4.8, 5.2, 5.5, 7.1, 7.2, 8; `docs/superpowers/specs/2026-10-20-varest2-inspector-design.md` §9 IN-1/2/4/5/9/11/12/17; controller rulings in `.superpowers/sdd/release-2.0-program.md`, section “M4b initial planning rulings”. Read those documents with this plan.

## Global Constraints

- “MIT; free; Win64/Mac/Linux/Android/iOS; UE 5.8 only; VaCuus-style dev setup”. LinuxArm64 is deferred separately.
- “Control plane on the game thread, data plane off it.” “Stage decisions, callbacks, and BP events run on the game thread. Parsing, decoding, cache disk IO, and offline-queue persistence run on UE::Tasks background tasks.” Controller-approved exception: queue admission normalizes only bounded metadata on GT through its body-free canonical codec (encoded metadata≤64KiB, depth≤8, lexical units≤2048, temporary logical allocation≤2MiB). Input length/count gates precede copies; no payload parse/scan/encoding or disk IO moves to GT. This establishes restart allocation parity before payload ownership. It costs bounded per-admission GT work, with no latency/frame-time guarantee; M7 measures the cap case.
- “Persisting tokens to disk. ExportSession/ImportSession let the game do it” remains a core non-goal: neither cache nor queue writes credentials on the game's behalf.
- “Only GET/HEAD with status 200 are stored.” Defaults: 8 MiB memory, 64 MiB disk; `Saved/VaRest/Cache`; queue `MaxQueued` 256, `MaxAge` 24 h; connectivity `ProbeInterval` 10 s.
- Preserve frozen v1 paths, names and signatures in `Source/VaRest` and `Source/VaRestEditor`; this plan edits neither. No dependency from W1 transport onto any new M4b API.
- All implementation dispatches include these constraints and the current release-program build/runbook rulings: sequential builds; action-graph inspection before NoPCH/nonunity execution; stop on engine/host actions; no cache/timestamp/object manipulation; restore only owned temporary Build.cs edits. Mirror substantive CLAUDE.md runbook changes in AGENTS.md. Product usage belongs in docs/dev/cache-offline.md.
- Every request receives exactly one outcome through its outer stages. User callbacks never run inside `Send`, `Cancel`, a setter, `FlushQueue`, or a connectivity transition call. Owned silent shutdown completion may settle internal accounting synchronously while suppressing every user callback.
- Accepted persistence waits reserve a separate bounded completion permit before any wait/job; their API-stack completions remain owned and drainable. Permit refusal creates no persistence ownership and keeps the ordinary asynchronous Config completion path outside that drain guarantee.
- Durable replay request-body previews are disabled before excerpt preparation; Started stays immediate with no pending request excerpt. This preserves the existing ordinary/response trace redactor policy.
- Auth refresh requests never enter an offline hold or durable queue. Deadline and cancellation reach every asynchronous cache/queue wait, including Now advancing before timer delivery; handoff rechecks precede cache hits/Notes.
- Snapshot configuration at Send, apply live session credentials when the attempt leaves the limiter, preserve M4a's per-dispatch identity and current/snapshot/applied redaction union.
- No file-level `using namespace`; local variables and parameters never hide a member or a function name under MSVC. Gate every dereference/index after a nonfatal automation assertion.
- Tests wait for monotonic launched/handed-back counters and game-thread quiescence, never a fixed delay. Virtual time controls freshness, probe intervals and queue expiry.
- Every C++ RED, GREEN, mutant and restored source version requires a fresh normal action graph and controller/delegated plugin-only scope review before its native build. Then run automation with `--no-build`. `Tools/run_tests.sh` must never implicitly compile a version in this plan. This applies to every task, not only final NoPCH checks. Stop on engine/host compilation, linking or PCH creation; no reuse of an earlier version's approved graph.
- Beads owns task state. The checkboxes below are execution instructions, not a second tracker. The controller owns issue operations and the release sequence. Implementation is sequential on one checkout; do not run a second build, writer or index mutation concurrently.
- Persistence limits are separate finite accounts:128MiB executor worker/result ownership, bounded caller/coordinator body reservations,10MiB queue metadata, and bounded quarantined cache-control intent. Task8B gives exact codec/recovery peak arithmetic and the≤2MiB body-free metadata normalization account; this is not an RSS promise. JSON guard admission reserves8×body+2MiB and may conservatively refuse below the32MiB record maximum without truncating scans or failing a live response.
- Base inspected: branch `v2`, `9884b5b`, `/w/Unreal/VaRest`. Before implementation re-read the current delta, resolve any changed interfaces, and record that reconciliation. This document-only planning pass does not compile, test, modify code, stage or commit.

## Review Focus

1. **A session export with another account's token pasted into it, or an unchanged import with changed session headers:** no old-account cache hit or queued write; complete validation before mutation, fresh identity on a bad binding, stable identity for refresh and a valid unchanged export. Tasks 1, 7, 8 and 9 pin this, including scope anchoring before any wait and physical A failure after replacement.
2. **Cancellation/deadline/logout during delayed disk read or rename:** one outer outcome and one truthful completion, no late dispatch or resurrected cache entry, no retained client cycle. Tasks 4, 7, 8 and 9 pin this with controllable worker barriers.
3. **Credential material outside normal auth headers (short tokens, percent-encoded query, late JSON field, binary payload):** full persistence guard refuses the operation without changing replay bytes; files and temp files contain no registered credential value. Tasks 3, 5, 8 and 9 pin this, including simultaneous case-distinct secrets and sticky work exhaustion.
4. **Two clients sharing a queue name, owner destroyed during replay, then another account logs in:** one FIFO runner, one event per live subscriber, identity/origin checks before auth, stable idempotency key; no cross-account send or duplicated callback. Tasks 8 and 9 pin this.
5. **Malformed cache metadata, Vary/no-cache/no-store and 304 after an identity change:** a corrupt disk record is a miss, stale data is used only under the specified offline policy, validators never leak into a different identity's attempt, and body/decode limits still hold. Tasks 2, 5, 7, 9 and 10 pin this; Task9 pins refused/failed mandatory maintenance and configured-root clearing.

---

## Decisions and the landed seam

1. The final chain is custom Outer → Offline → Cache → Retry → AuthGate → boundary → limiter → AuthApply → custom PerAttempt → log → transport. `FClientState::Publish` builds it in `Private/VaRestClientState.cpp`. Pipeline entry, completion, cancellation and Resolve are actually in `Private/VaRestPipeline.cpp`, not the state file.
2. Public cache defaults to disabled. `WithCache(Memory())` enables a client memory cache; `.Cache(...)` overrides per request and `.NoCache()` bypasses reads **and** writes. GET and HEAD have separate keys. GET with a body, Range/If-Range, caller conditional headers, SaveTo, refresh, and an unknown custom auth identity bypass caching. Neither a 206 nor a redirect response is stored. HEAD never replaces GET bytes.
3. A request key hashes length-framed UTF-8 fields: domain/version, verb, resolved URL without fragment, identity, and canonical request headers. Conservatively key on **all** built headers plus session headers (case-folded names, case-sensitive values, sorted name/value pairs). This safely implements all `Vary` names known before an attempt and may miss more often. `Vary:*` and Vary on any header absent from the known built/session request headers is uncacheable, including transport-synthesized defaults and auth-only headers. This conservative rule avoids guessing an engine header value. With any custom PerAttempt stage, cache is bypassed unless its author later supplies a separate vary contract; M4b adds no such contract. No raw URL, header value or credential appears in a cache filename or key metadata.
4. The session has an opaque GUID identity, separate from `Generation`/`CredentialGeneration`. A real Replaced mutation, clear or expiry mints it; refresh keeps it. Setting exactly identical session data keeps it. Session header values are separately hashed into cache keys. Queue scope includes those headers too, so changing a header-based tenant cannot redirect old work into a new tenant. Import validates an additive consistency binding before applying the identity; details in Task 1. An unkeyed binding is **not authentication**; caller-owned exported credential state is trusted.
5. Built-in Bearer/BearerRefresh use that session identity only when usable session credentials exist. API key and Basic use a hash of their stable credential material. `NoAuth` has a separate anonymous domain and includes any explicit Authorization/Cookie headers in its request key. A custom auth defaults to no identity: cache bypass, durable queue `Config`. It can opt in through `IVaRestAuth::GetIdentity(const FVaRestSessionData&, FString&) const`; this is a pure GT query, never `Apply` invoked speculatively. A custom implementation promises that the returned opaque string changes with the account and stays stable across its refresh.
6. Before serving a disk result and before storing a response, recompute the identity and canonical request/session-header key. If it differs, ignore the old result. An identity change while a request waits in the limiter also removes conditional headers before dispatch; a 304 under another identity is never joined to old bytes. An auth refresh keeps the identity, so it preserves a valid conditional request.
7. HTTP freshness: strict nonnegative decimal `max-age`; repeated conflicting/invalid ages make lifetime zero. `no-store` forbids storage and removes the matching old entry; `no-cache` allows bytes but requires validation on every use; `must-revalidate` disallows offline stale use. `Expires` minus `Date` (or receive UTC) is fallback; client default TTL is last fallback. `Age` and apparent age reduce freshness. Equality at expiry is stale. Future timestamps never extend lifetime. `private`/`public` have no special meaning in this private client cache.
8. A conditional 304 merges allowed end-to-end response metadata into the cached 200, keeps the cached body, recalculates freshness, and returns status 200 with `bFromCache=true`. It emits the actual wire 304 in AttemptEnded. Never cache Set-Cookie, auth/session headers, hop-by-hop headers, Transfer-Encoding or a stale Content-Length; set GET Content-Length to the retained byte count; HEAD preserves the cached representation length even though its stored body is empty. A 304 with no captured entry remains an ordinary HTTP outcome. A 304 `no-store` may satisfy that request from the captured bytes but removes the stored record.
9. `bServeStaleOnError` covers `Connection`/`Offline` only, and known offline before dispatch. It does not hide timeout, HTTP 4xx/5xx, auth, decode or configuration failures. `no-cache`/`must-revalidate` entries are not served stale. Cache reads and stored bytes remain independent of result type/envelope, decoded afresh with each request's MaxJsonBytes.
10. `ClearCache` invalidates the calling client's memory cache immediately and queues a clear of its configured disk namespace. A monotonically increasing namespace epoch rejects late reads/writes; serial worker order ensures clear cannot be undone by a previous write. Logout changes the identity; it does not delete another account's stored data.
11. `FVaRestConnectivity` is a GT object with a process default scope 0. Each test injects its own instance and clock. M5 creates one per Hub/game instance. A transport Connection failure marks offline; any received HTTP response proves online even if status is 401/500. Local refusals, cancellation and timeouts alone do not flip it. Synthetic cache success does not prove network connectivity.
12. Known-offline nonqueued requests still traverse Cache, allowing fresh/stale hits. If no cache answer is available they fail Offline without a normal attempt. A timed probe bypasses that offline refusal and the durable queue. The queue head is preferred; an optional HealthUrl is used only with no eligible queue head. No queue/health URL means no periodic network request. ForceOffline suppresses all probe dispatch and never marks online from an old response; clearing the cvar reveals the latest observed state with an OverrideRemoved edge, and probes only when that state is offline.
13. A named queue is an explicit durable sharing domain. Its path is `<QueueRoot>/<SHA256(domain, exact name)>`, never a lossy sanitized name. Same name/root means one coordinator and one FIFO, not one per handle. Live registrants must agree on canonical origin, connectivity pointer, limits and identity provider semantics; reject conflicts with Config on queue-enabled sends. Account-scoped names are recommended. Files are not a credential authority: validate the **actual** URL and scope at save, load and replay.
14. Each queue item carries a distinct GUID QueueId, monotonically increasing uint64 sequence (serialized as decimal text, not a JSON double), process-independent original request identity (run GUID + original uint64), UTC creation, absolute URL, verb, safe headers, body kind/type/bytes, idempotency key, tag text, auth mode and opaque identity scope. Request callbacks/owners/result types are never serialized. Public `QueueReplayOf.OriginalRequestId` is 0 after a different process run, preventing false links to reused process-local numeric ids; QueueId remains the durable link.
15. Replay is at-least-once across crashes. A server response followed by a process crash before the durable tombstone can replay once more; preserve the same Idempotency-Key. Idempotent verbs retain their HTTP semantics; `.Idempotent()` makes a stable key before the original attempt. Never claim exactly-once remote execution. In-process replay starts the next item only after the prior file was durably retired.
16. Replay uses the oldest live registered client (lowest ClientId); selection is captured strongly only for the active exchange and weakly otherwise. All currently live same-queue client subscribers receive one `OnQueuedResult` event after durable removal; the subscriber list is snapshotted before dispatch, so reentrant mutations affect the next event. There is one event broadcast per registration, not one per copied FVaRestClient handle. If no clients remain, no replay starts. If the active owner is destroyed, its in-flight exchange keeps the state alive; completion drains normally and future replay selects a live registrant. No callbacks execute under registry iteration or a worker lock.
17. Before replay, scope mismatch ends the FIFO head with Auth/no network and advances; invalid origin/record ends Config/no network and advances. Expired ends Offline/no network and advances. Connection/Offline retains the item and stops. Other final outcomes, including Http/Timeout/Auth/Decode/Config, consume it and fire OnQueuedResult. Replay uses current auth, headers and envelope from the chosen owner but forces raw bytes for the event response and does not invoke the original builder's success/error/complete callbacks.
18. QueueWhenOffline anchors its account/tenant scope once in initial GT Resolve before all stage/load/limiter waits; identity changes refuse further dispatch/admission instead of relabeling the operation. Same-identity refresh remains valid. QueueWhenOffline requires explicit name, idempotent verb or `.Idempotent()`, no streamed/file multipart body, no SaveTo and no OnHttpThread. A 32 MiB per-item persisted-byte bound (before base64), 64 KiB metadata bound, 256-item count, and 64 MiB aggregate queued-body bound prevent unbounded memory/disk use; admission reserves budget before asynchronous write. Limits are configurable in `FVaRestOfflinePolicy` and conflicts are detected for shared names. Invalid opts fail Config at Send even while online. No body is moved away before a possible connection failure is persisted.
19. Durable admission is two-phase: worker prepares and flushes a nonreplayable `.pending` file; GT rechecks identity/origin/owner/deadline and races any-thread cancellation through one shared atomic gate; only the commit winner schedules final rename to `.json`. OnQueued occurs only after rename succeeds. Cancellation/deadline before commit wins promptly and leaves no replayable file; after commit selection filesystem IO is beyond rollback and its actual success/failure is delivered. IsActive becomes false at commit selection, IsDone remains false until delivery. This documented exception applies only to queue acceptance. Shutdown/owner loss suppress callbacks while committed IO drains.
20. `FlushQueue` means flush admitted persistence work to disk, not force network replay. It returns bool admission and takes an optional completion callback. False means no callback (invalid registration, shutdown or bounded waiter/continuation refusal); true completes asynchronously after the serial barrier, subject to shutdown/owner suppression; a pending failed write produces false. It does not run a nested GT pump or block. M5 handles a false admission explicitly before Hub teardown; module PreExit stops admission, cancels live exchanges, finishes worker operations and suppresses user callbacks.
21. Persistence checks are independent of the trace redactor's 8-byte minimum and excerpt cap. Full known-secret byte forms plus structural key checks are applied off GT before writing immutable payloads. Unrecognized application secrets cannot be inferred; the API promises exclusion of configured auth/session fields and registered secrets, and requires applications to register additional payload secrets. Refusing a payload preserves its bytes; it is never redacted into a different operation.
22. All public enums/struct fields in this milestone freeze at M5's Blueprint exposure; their C++ spelling is fixed here. Do not rename landed M4a names. Queue IDs are FString (GUID text), not FName (untrusted persisted text never populates the global name table until bounded Tag validation).

## File structure and ownership

| Files | Responsibility |
| --- | --- |
| `Public/VaRestCache.h`, `Public/VaRestOffline.h`, `Public/VaRestConnectivity.h` | Policy/event types and code-first surfaces |
| `Private/VaRestIdentity.h/.cpp` | Framed hashes, canonical headers/origin, session consistency binding, auth scope |
| `Private/VaRestCachePolicy.h/.cpp` | Eligibility, freshness and 304 metadata merge |
| `Private/VaRestPersistenceGuard.h/.cpp` | Complete bounded persistence validation; raw secret snapshot |
| `Private/VaRestPersistence.h/.cpp` | Serial worker jobs, atomic immutable files, drain/shutdown accounting |
| `Private/VaRestMemoryCache.h/.cpp` | GT byte-accounted memory LRU |
| `Private/VaRestDiskCache.h/.cpp` | Versioned disk records, async index/recovery/LRU/epochs |
| `Private/VaRestCache.cpp` | Cache stage and cancellation/deadline wait |
| `Private/VaRestConnectivity.cpp` | State transitions, cvar, mobile hints, probe scheduling |
| `Private/VaRestQueueStore.h/.cpp` | Validated durable FIFO records and tombstones |
| `Private/VaRestOffline.cpp`, `Private/VaRestOfflineInternal.h` | Registry, admission stage, replay coordinator, callback fanout |
| `Private/VaRestPersistenceWait.h/.cpp` | One reusable interruptible GT wait, never worker-owned exchange |
| `Public/VaRestClient.h`, `Private/VaRestClient.cpp` | Builder/client methods; spec-to-exchange transfer |
| `Private/VaRestClientState.h/.cpp`, `Private/VaRestExchangeState.h`, `Private/VaRestPipeline.cpp` | Configuration, ownership, Resolve, outcome and completion integration |
| `Public/VaRestAuth.h`, `Public/VaRestAuthTypes.h`, `Private/VaRestAuth.cpp`, `Private/VaRestBearerRefresh.cpp`, `Private/VaRestSession.cpp` | Stable identity query and validated session transitions |
| `Public/VaRestStage.h`, `Public/VaRestResultHandler.h`, `Private/VaRestClassify.cpp` | Cache/queued outcome handoff and new local failure details |
| `Public/VaRestTrace.h`, `Private/VaRestTracePoints.cpp`, `Private/VaRestHttpStatsInternal.h`, `Private/VaRestHttpStats.cpp` | Additive final cache disposition, queue traces, ratios |
| `Private/Tests/VaRestHttp{Identity,CachePolicy,PersistenceGuard,Persistence,MemoryCache,DiskCache,Cache,Connectivity,QueueStore,Offline,Replay,PersistenceRegression,TraceCacheOffline}Test.cpp` | Unit/fake/golden proof beside the private interfaces |
| `Public/VaRestHttpTesting.h`, `Private/VaRestHttpTesting.cpp` | Deterministic IO counters/barriers; isolated test connectivity |
| `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`, `Tools/testserver/server.py`, `Tools/testserver/selftest.py` | Actual curl/disk restart integration and stdlib fixtures |
| `docs/dev/cache-offline.md`, core/Inspector specs, `CLAUDE.md`, `AGENTS.md` | User contract, approved spec amendments and freeze/carry list |

Paths without a module prefix in that table are under `Source/VaRestHttp/`. All implementation tasks own their explicitly listed files; no task changes WebSocket transport sources.

## Shared implementation conventions

Each code block belongs to the named file and namespace shown. Add direct includes for every named type/function, preserve copyright headers, and keep UHT `.generated.h` last. New persistence handbacks use the owned-slot/ticker pump defined in Task 4; queue replay deferrals use its bounded GT queue. Existing pipeline GT tasks retain their landed dispatch mechanism. Worker jobs have no extended priority. Never call FTask::Wait from GT to drain disk jobs because it may retract IO onto GT.

Every automation file is guarded by `#if WITH_DEV_AUTOMATION_TESTS`, includes `Misc/AutomationTest.h` (and `Misc/ScopeExit.h` when using ON_SCOPE_EXIT), and uses `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. Test bodies below include their complete assertions; register each named body with `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, using the same C++ class name as its function. Local namespace imports inside `RunTest` are permitted. Every test drains the rig before deleting its private temp directory. `FScopedPersistenceTest` introduced in Task 4 supplies the test root and synchronous observation **only by pumping background handbacks**, never performing production disk IO on GT.


**Per-version normal build gate (all C++ tasks and all mutations):** after editing that exact source version, run the following dry-run, inspect every action/source/output, and obtain the delegated scope gate before the second command. A new edit invalidates the graph approval. Keep the report's source version/diff identifier with its graph. RED can be a missing-symbol compile failure after the allowed native build begins; GREEN must compile before automation.

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/tmp/varest-m4b-normal-actions.json
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
Tools/run_tests.sh --no-build VaRest.Http
```

The three commands are sequential gated actions, not a shell chain. Replace only the final test prefix with the owning task's prefix. The graph must contain only authorized plugin compile/link actions and no engine/host/PCH-creation action. Controller approval of a bounded fallback must name that exact graph. Python fixture-only changes need their Python checks, not a native rebuild. Final monolithic targets each require their own fresh graph/scope gate before building; normal Editor approval does not authorize those graphs.

A task's final commit step is run only by the sequential executor under the controller's index lease. Planning writers do not execute any commit command in this document.

### Task 1: Stable authentication identity and session import consistency

**Files:** Create `Source/VaRestHttp/Private/VaRestIdentity.h/.cpp`, `Private/Tests/VaRestHttpIdentityTest.cpp`; modify `Public/VaRestAuth.h`, `Public/VaRestAuthTypes.h`, `Private/VaRestAuth.cpp`, `Private/VaRestBearerRefresh.cpp`, `Private/VaRestSession.cpp`, `Private/VaRestClientState.h`.

**Interfaces:** Consumes `FSha256::Update/Finalize`, `FVaRestHeaders` iteration/Find/Set, `FVaRestSessionData`, `FClientState::SetTokensByGame/ImportSession/ExpireSessionNow`. Produces:

```cpp
// VaRestIdentity.h, namespace VaRest::Http
FString HashFields(FStringView Domain, TConstArrayView<FString> Fields);
TArray<FString> CanonicalHeaders(const FVaRestHeaders& Headers);
FString SessionBinding(const FVaRestSessionData& Data);
FString SessionScope(const FVaRestSessionData& Data);
bool AuthIdentity(const IVaRestAuth* Auth, EVaRestAuthMode Mode,
    const FVaRestSessionData& Session, FString& OutIdentity);
FString CacheKey(FStringView Verb, FStringView Url, FStringView Identity,
    const FVaRestHeaders& Built, const FVaRestHeaders& SessionHeaders);
// FVaRestSessionData, opaque and not a credential:
FGuid IdentityId = FGuid::NewGuid();
// IVaRestAuth, pure query; false means cache bypass / durable queue refused:
virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const
{
    OutIdentity.Reset();
    return false;
}
```

- [ ] **Step 1: Add the failing identity/import tests.** These assertions are the entire body of `FVaRestHttpIdentityImportTest::RunTest` (`VaRest.Http.Identity.ImportBinding`).

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FTestRig First;
First.Client.WithAuth(FVaRestAuth::Bearer());
First.Client.SetSession(TEXT("account-A-token"), TEXT("account-A-refresh"));
First.Client.SetSessionHeader(TEXT("X-Tenant"), TEXT("tenant-A"));
const FGuid Original = First.Client.GetSession().IdentityId;
const FString Exported = First.Client.ExportSession();
FTestRig Restored;
Restored.Client.WithAuth(FVaRestAuth::Bearer());
if (!TestTrue(TEXT("valid export imports"), Restored.Client.ImportSession(Exported))) return false;
TestTrue(TEXT("restart keeps identity"), Restored.Client.GetSession().IdentityId == Original);
TestTrue(TEXT("repeat import succeeds"), Restored.Client.ImportSession(Exported));
TestTrue(TEXT("unchanged tokens keep restored identity"), Restored.Client.GetSession().IdentityId == Original);
Restored.Client.SetBearerToken(TEXT("account-A-rotated"), true);
TestTrue(TEXT("refresh keeps identity"), Restored.Client.GetSession().IdentityId == Original);
Restored.Client.SetBearerToken(TEXT("account-B-token"));
TestTrue(TEXT("replacement separates"), Restored.Client.GetSession().IdentityId != Original);
const auto Parsed = FVaRestJson::Parse(Exported);
if (!TestTrue(TEXT("export JSON"), Parsed.HasValue())) return false;
FVaRestJson Mixed = Parsed.GetValue().Clone();
Mixed.Set(TEXT("accessToken"), TEXT("account-B-token"));
TestTrue(TEXT("valid credentials, stale binding still imports"), Restored.Client.ImportSession(Mixed.ToString()));
TestTrue(TEXT("stale binding never restores A"), Restored.Client.GetSession().IdentityId != Original);
Mixed.Set(TEXT("accessToken"), 42);
const FVaRestSessionData BeforeInvalid = Restored.Client.GetSession();
TestFalse(TEXT("bad token type rejected"), Restored.Client.ImportSession(Mixed.ToString()));
TestTrue(TEXT("invalid import did not mutate identity"), Restored.Client.GetSession().IdentityId == BeforeInvalid.IdentityId);
TestEqual(TEXT("invalid import did not mutate token"), Restored.Client.GetSession().AccessToken, BeforeInvalid.AccessToken);
const FGuid LiveBeforeTrusted=Restored.Client.GetSession().IdentityId;
TestTrue(TEXT("trusted same identity with different valid export"),Restored.Client.ImportSession(Exported,true));
TestTrue(TEXT("explicit live assertion overrides export ID"),Restored.Client.GetSession().IdentityId==LiveBeforeTrusted);
Restored.Client.ClearSession();
TestTrue(TEXT("same flag without live session restores valid export"),Restored.Client.ImportSession(Exported,true));
TestTrue(TEXT("no-live restores A"),Restored.Client.GetSession().IdentityId==Original);
FVaRestJson ChangedHeaders=Parsed.GetValue().Clone();
ChangedHeaders.Set(TEXT("headers"),FVaRestJson::Array().Add(FVaRestJson::Object().Set(TEXT("name"),TEXT("X-Tenant")).Set(TEXT("value"),TEXT("tenant-B"))));
TestTrue(TEXT("header edit remains valid legacy input"),Restored.Client.ImportSession(ChangedHeaders.ToString()));
TestTrue(TEXT("changed tenant invalidates export binding"),Restored.Client.GetSession().IdentityId!=Original);
FVaRestJson Legacy=Parsed.GetValue().Clone();
Legacy.Remove(TEXT("cacheIdentity")); Legacy.Remove(TEXT("cacheBinding"));
TestTrue(TEXT("old export imports"),Restored.Client.ImportSession(Legacy.ToString()));
TestTrue(TEXT("old export gets fresh namespace"),Restored.Client.GetSession().IdentityId!=Original);
TestTrue(TEXT("framing distinguishes ambiguous concatenation"),
    HashFields(TEXT("test"), TArray<FString>{TEXT("ab"), TEXT("c")}) != HashFields(TEXT("test"), TArray<FString>{TEXT("a"), TEXT("bc")}));
TestTrue(TEXT("domains differ"), HashFields(TEXT("one"), TArray<FString>{TEXT("x")}) != HashFields(TEXT("two"), TArray<FString>{TEXT("x")}));
TestTrue(TEXT("drains"), First.Drain() && Restored.Drain());
return true;
```

`FVaRestJson::Clone()` is the landed deep-copy method; no shared-node mutation of the exported baseline.

- [ ] **Step 2: Run red.** The fresh approved NORMAL native build must fail compilation on the missing identity surface; record that failure. Once that source version builds, run `Tools/run_tests.sh --no-build VaRest.Http.Identity` and record the assertion failures. A --no-build runner never supplies compile evidence.
- [ ] **Step 3: Implement framed hashes and the pure identity contracts.** `VaRestIdentity.cpp` includes `VaRestSha256.h`, `VaRestAuth.h`, `VaRestUrl.h`.

```cpp
FString VaRest::Http::HashFields(FStringView Domain, TConstArrayView<FString> Fields)
{
    FSha256 Hash;
    const auto Feed = [&Hash](FStringView Text)
    {
        const FTCHARToUTF8 Encoded(Text.GetData(), Text.Len());
        const uint64 Length = static_cast<uint64>(Encoded.Length());
        uint8 Size[8];
        for (int32 Index = 0; Index < 8; ++Index) Size[Index] = static_cast<uint8>(Length >> (Index * 8));
        Hash.Update(Size, 8);
        Hash.Update(Encoded.Get(), Encoded.Length());
    };
    Feed(Domain);
    for (const FString& Field : Fields) Feed(Field);
    return Hash.Finalize().ToHex();
}
TArray<FString> VaRest::Http::CanonicalHeaders(const FVaRestHeaders& Headers)
{
    TArray<TPair<FString, FString>> Pairs;
    for (const FVaRestHeaders::FPair& Pair : Headers) Pairs.Emplace(Pair.Key.ToLower(), Pair.Value);
    Pairs.Sort([](const auto& Left, const auto& Right)
    {
        const int32 Compared = Left.Key.Compare(Right.Key, ESearchCase::CaseSensitive);
        return Compared == 0 ? Left.Value.Compare(Right.Value,ESearchCase::CaseSensitive)<0 : Compared < 0;
    });
    TArray<FString> Fields;
    for (const auto& Pair : Pairs) { Fields.Add(Pair.Key); Fields.Add(Pair.Value); }
    return Fields;
}
FString VaRest::Http::SessionBinding(const FVaRestSessionData& Data)
{
    TArray<FString> Fields{Data.IdentityId.ToString(EGuidFormats::DigitsLower), Data.AccessToken, Data.RefreshToken};
    Fields.Append(CanonicalHeaders(Data.Headers));
    return HashFields(TEXT("VaRest/session-export/v1"), Fields);
}
FString VaRest::Http::SessionScope(const FVaRestSessionData& Data)
{
    TArray<FString> Fields{Data.IdentityId.ToString(EGuidFormats::DigitsLower)};
    Fields.Append(CanonicalHeaders(Data.Headers));
    return HashFields(TEXT("VaRest/session-scope/v1"), Fields);
}
bool VaRest::Http::AuthIdentity(const IVaRestAuth* Auth, EVaRestAuthMode Mode,
    const FVaRestSessionData& Session, FString& OutIdentity)
{
    if (Mode == EVaRestAuthMode::None)
    {
        OutIdentity = HashFields(TEXT("VaRest/anonymous/v1"), TArray<FString>{});
        return true;
    }
    if (!Auth)
    {
        OutIdentity = Session.Headers.IsEmpty()
            ? HashFields(TEXT("VaRest/anonymous/v1"), TArray<FString>{}) : SessionScope(Session);
        return true;
    }
    if (!Auth->GetConfigError().IsEmpty()) return false;
    return Auth->GetIdentity(Session, OutIdentity) && !OutIdentity.IsEmpty();
}
FString VaRest::Http::CacheKey(FStringView Verb, FStringView Url, FStringView Identity,
    const FVaRestHeaders& Built, const FVaRestHeaders& SessionHeaders)
{
    FString Fragmentless(Url);
    int32 Fragment = INDEX_NONE;
    if (Fragmentless.FindChar(TEXT('#'), Fragment)) Fragmentless.LeftInline(Fragment);
    FVaRestHeaders Effective = Built;
    for (const auto& Pair : SessionHeaders) Effective.Set(Pair.Key, Pair.Value);
    TArray<FString> Fields{FString(Verb), MoveTemp(Fragmentless), FString(Identity)};
    Fields.Append(CanonicalHeaders(Effective));
    return HashFields(TEXT("VaRest/cache-key/v1"), Fields);
}
```

Built-in overrides: Bearer and BearerRefresh return false when neither token exists, else `OutIdentity = SessionScope(Session); return true;`. Basic returns `HashFields("VaRest/basic/v1", {Credential})`; API key returns `HashFields("VaRest/apikey/v1", {LexToString(static_cast<uint8>(Placement)), Name.ToLower() for Header else Name, Key})`. Include session headers by hashing that built-in identity together with `CanonicalHeaders(Session.Headers)` in `AuthIdentity` after the auth query; do not add session GUID to Basic/API-key scopes or restart reuse would be lost. All false paths reset output; call sites never use output after false.

- [ ] **Step 4: Place identity mutation inside validated session changes.** In SetTokensByGame select the explicit validated import identity before UpdateTokens, or mint in its success branch for a replacement; the exact edits below preserve refresh-waiter ordering. `FinishRefresh` keeps its existing UpdateTokens path and preserves IdentityId. `ClearSession` already assigns a default `FVaRestSessionData` on a real clear, which mints; the empty no-op clear remains a no-op. `ExpireSessionNow` explicitly assigns a new GUID when it clears tokens. `ExportSession` adds:

```cpp
Root.Set(TEXT("cacheIdentity"), Data.IdentityId.ToString(EGuidFormats::DigitsLower));
Root.Set(TEXT("cacheBinding"), SessionBinding(Data));
```

After ImportSession validates every existing field, before `Headers` is moved, calculate `Candidate` from Access/Refresh/Headers. Parse only an exact 32-hex `cacheIdentity`; accept binding only if both are strings, binding has 64 lowercase hex characters, and `SessionBinding(Candidate)` matches case-sensitively. Missing, malformed or mismatching additive metadata sets `Candidate.IdentityId = FGuid::NewGuid()`, while invalid **existing** fields still return false without mutation. Both-empty token import remains logout. Pass the selected identity into SetTokensByGame before it can release refresh waiters, including when UpdateTokens is a no-op. `bSameIdentity=true` with a nonempty live session is an explicit caller assertion and preserves its current identity even when a valid export names a different identity. With no live session, valid binding restores its identity and missing/mismatched binding mints fresh regardless of that flag. Both-empty token import remains logout. The exact insertion below implements this precedence.

The exact import selection block goes after the existing header-validation loop, before the both-empty logout branch (all existing fields are now validated):

```cpp
const bool bLiveIdentity = Session.Data.HasAccessToken() || Session.Data.HasRefreshToken()
    || !Session.Data.Headers.IsEmpty();
FVaRestSessionData Candidate;
Candidate.AccessToken = Access;
Candidate.RefreshToken = Refresh;
Candidate.Headers = Headers;
FGuid ImportedIdentity;
const FVaRestJson IdentityNode = Root[TEXT("cacheIdentity")];
const FVaRestJson BindingNode = Root[TEXT("cacheBinding")];
const FString IdentityText = IdentityNode.IsString() ? IdentityNode.AsString() : FString();
const FString BindingText = BindingNode.IsString() ? BindingNode.AsString() : FString();
bool bValidBinding = false;
if (IdentityText.Len() == 32 && FGuid::ParseExact(IdentityText, EGuidFormats::Digits, ImportedIdentity)
    && ImportedIdentity.IsValid() && BindingText.Len() == 64)
{
    Candidate.IdentityId = ImportedIdentity;
    bValidBinding = SessionBinding(Candidate).Equals(BindingText, ESearchCase::CaseSensitive);
}
const FGuid ChosenIdentity = bSameIdentity && bLiveIdentity ? Session.Data.IdentityId
    : bValidBinding ? ImportedIdentity : FGuid::NewGuid();
```

Extend the private SetTokensByGame declaration/definition with final `const FGuid* IdentityOverride = nullptr` (default only in VaRestClientState.h). At entry, after capturing CredentialsBefore/bNewRefreshToken and before UpdateTokens, insert `if (IdentityOverride) Session.Data.IdentityId = *IdentityOverride;`. Inside the UpdateTokens success branch, before EndRefresh, insert `if (!IdentityOverride && !bSameIdentity) Session.Data.IdentityId = FGuid::NewGuid();`. Replace ImportSession's final call with:

```cpp
SetTokensByGame(Access,&Refresh,ExpiresIn,0.0,bSameIdentity,&ChosenIdentity);
```

All callers reaching that helper already validated the original fields; only import passes the override. Both-empty import returns through ClearSession before this call. The chosen identity is visible before EndRefresh can release parked requests and before queued session events are delivered; unchanged-token imports still update the identity. In ExpireSessionNow add `Data.IdentityId = FGuid::NewGuid();` immediately before tokens are reset. Real clear uses its existing default FVaRestSessionData assignment. Add Private/VaRestClientState.h to this task's explicit files and commit list.

Add these complete override bodies inside the existing private classes:

```cpp
// FBearerAuth and FBearerRefreshAuth, same body in each class:
virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
{
    OutIdentity.Reset();
    if (!Session.HasAccessToken() && !Session.HasRefreshToken()) return false;
    OutIdentity = VaRest::Http::SessionScope(Session);
    return true;
}
// FApiKeyAuth:
virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
{
    OutIdentity.Reset();
    if (!ConfigError.IsEmpty()) return false;
    OutIdentity = VaRest::Http::HashFields(TEXT("VaRest/apikey/v1"), TArray<FString>{
        LexToString(static_cast<uint8>(Placement)),
        Placement == EVaRestApiKeyPlacement::Header ? Name.ToLower() : Name, Key});
    return true;
}
// FBasicAuth:
virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
{
    OutIdentity.Reset();
    if (!ConfigError.IsEmpty()) return false;
    OutIdentity = VaRest::Http::HashFields(TEXT("VaRest/basic/v1"), TArray<FString>{Credential});
    return true;
}
```

Replace AuthIdentity's final `return Auth->GetIdentity(...)` with:

```cpp
FString ProviderIdentity;
if (!Auth->GetIdentity(Session, ProviderIdentity) || ProviderIdentity.IsEmpty())
{
    OutIdentity.Reset();
    return false;
}
TArray<FString> Fields{MoveTemp(ProviderIdentity)};
Fields.Append(CanonicalHeaders(Session.Headers));
OutIdentity = HashFields(TEXT("VaRest/auth-scope/v1"), Fields);
return true;
```

- [ ] **Step 5: Add edge assertions and run green.** Test old export missing both additive fields; malformed GUID/hash; changed headers with unchanged token; different expiry only; NoAuth vs Bearer; Basic/API-key recreated objects; a custom auth with no GetIdentity; clear/expiry and refresh. Each case checks equality/inequality of actual `CacheKey`/`SessionScope`, not just GUID members. Run `Tools/run_tests.sh --no-build VaRest.Http.Identity` then `Tools/run_tests.sh --no-build VaRest.Http.Session`. Restore the bug by removing the binding check: `ImportBinding` must fail without a crash; restore code and rerun.
- [ ] **Step 6: Commit this isolated identity slice.** `git add Source/VaRestHttp/Private/VaRestIdentity.h Source/VaRestHttp/Private/VaRestIdentity.cpp Source/VaRestHttp/Private/Tests/VaRestHttpIdentityTest.cpp Source/VaRestHttp/Public/VaRestAuth.h Source/VaRestHttp/Public/VaRestAuthTypes.h Source/VaRestHttp/Private/VaRestAuth.cpp Source/VaRestHttp/Private/VaRestBearerRefresh.cpp Source/VaRestHttp/Private/VaRestSession.cpp Source/VaRestHttp/Private/VaRestClientState.h && git commit -m "feat(http): add stable cache and replay identity"` (only the sequential executor; inspect staged paths first).

### Task 2: Cache policy, freshness and conservative variation

**Files:** Create `Public/VaRestCache.h`, `Private/VaRestCachePolicy.h/.cpp`, `Private/Tests/VaRestHttpCachePolicyTest.cpp`.

**Interfaces:** Consumes `FVaRestHeaders`, `FDateTime::ParseHttpDate`, Task 1 hashes. Produces the public policy and pure internal metadata functions:

```cpp
// VaRestCache.h: CoreMinimal.h, then VaRestCache.generated.h
UENUM(BlueprintType)
enum class EVaRestCacheMode : uint8 { None, Memory, MemoryAndDisk };
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestCachePolicy
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Cache")
    EVaRestCacheMode Mode = EVaRestCacheMode::None;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Cache")
    int64 MemoryBytes = 8 * 1024 * 1024;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Cache")
    int64 DiskBytes = 64 * 1024 * 1024;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Cache")
    double DefaultTtlSeconds = 0.0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Cache")
    bool bServeStaleOnError = false;
    static FVaRestCachePolicy Memory() { FVaRestCachePolicy Result; Result.Mode = EVaRestCacheMode::Memory; return Result; }
    static FVaRestCachePolicy MemoryAndDisk() { FVaRestCachePolicy Result = Memory(); Result.Mode = EVaRestCacheMode::MemoryAndDisk; return Result; }
};
// VaRestCachePolicy.h, namespace VaRest::Http
struct FCacheMetadata
{
    bool bNoStore = false;
    bool bNoCache = false;
    bool bMustRevalidate = false;
    double Lifetime = 0.0;
    double InitialAge = 0.0;
    FDateTime StoredUtc;
    double StoredAt = 0.0;
    FString ETag;
    FString LastModified;
    TArray<FString> Vary;
    bool IsFresh(double Now) const
    {
        return !bNoCache && !bNoStore && InitialAge + FMath::Max(0.0, Now - StoredAt) < Lifetime;
    }
    bool CanServeStale() const { return !bNoCache && !bNoStore && !bMustRevalidate; }
};
bool ReadDeltaSeconds(FStringView Text, double& OutSeconds);
FCacheMetadata ReadCacheMetadata(const FVaRestHeaders& Headers, double DefaultTtl,
    double Now, FDateTime NowUtc);
bool CacheHeadersEligible(const FVaRestHeaders& Request, const FVaRestHeaders& Response);
FVaRestHeaders MergeNotModified(const FVaRestHeaders& Stored, const FVaRestHeaders& Fresh, int64 BodyBytes, bool bHead);
```

- [ ] **Step 1: Add `VaRest.Http.CachePolicy.Directives` with this body.**

```cpp
using namespace VaRest::Http;
FVaRestHeaders Headers;
Headers.Set(TEXT("Cache-Control"), TEXT("private, max-age=60, no-cache"));
const FDateTime Utc(2026, 10, 23, 12, 0, 0);
auto Meta = ReadCacheMetadata(Headers, 300, 100, Utc);
TestTrue(TEXT("no-cache stores but never fresh"), !Meta.bNoStore && Meta.bNoCache && !Meta.IsFresh(100));
Headers.Set(TEXT("Cache-Control"), TEXT("public, MAX-AGE=60"));
Headers.Set(TEXT("Age"), TEXT("10"));
Meta = ReadCacheMetadata(Headers, 300, 100, Utc);
TestTrue(TEXT("remaining lifetime"), Meta.IsFresh(149.999));
TestFalse(TEXT("boundary expires"), Meta.IsFresh(150));
Headers.Set(TEXT("Cache-Control"), TEXT("max-age=60, max-age=70"));
TestFalse(TEXT("conflicting ages require validation"), ReadCacheMetadata(Headers, 300, 100, Utc).IsFresh(100));
Headers.Set(TEXT("Cache-Control"), TEXT("no-store, max-age=3600"));
TestTrue(TEXT("no-store wins"), ReadCacheMetadata(Headers, 300, 100, Utc).bNoStore);
double Delta = -1;
TestFalse(TEXT("overflow is not immortal"), ReadDeltaSeconds(TEXT("9999999999999999999999999999"), Delta));
TestFalse(TEXT("negative invalid"), ReadDeltaSeconds(TEXT("-1"), Delta));
Headers.Set(TEXT("Vary"), TEXT("*"));
TestFalse(TEXT("wildcard uncacheable"), CacheHeadersEligible(FVaRestHeaders(), Headers));
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.CachePolicy`.
- [ ] **Step 3: Implement parsing with the following numeric core.** Read every Cache-Control header occurrence and comma-separated directive, trim optional whitespace, match directive names case-insensitively, accept quoted decimal max-age only when the entire quote pair is present, and use the minimum of repeated identical valid max-age values (different values set zero). A present invalid max-age never falls back to a positive default. Parsing uses FString keys, never FName from server input.

```cpp
bool VaRest::Http::ReadDeltaSeconds(FStringView Text, double& OutSeconds)
{
    if (Text.IsEmpty()) return false;
    uint64 Value = 0;
    constexpr uint64 Maximum = 315360000ULL; // 10 years; bounded persisted arithmetic
    for (TCHAR Character : Text)
    {
        if (Character < TEXT('0') || Character > TEXT('9')) return false;
        const uint64 Digit = static_cast<uint64>(Character - TEXT('0'));
        if (Value > (Maximum - Digit) / 10) return false;
        Value = Value * 10 + Digit;
    }
    OutSeconds = static_cast<double>(Value);
    return true;
}
```

For `ReadCacheMetadata`: initialize `StoredAt/StoredUtc`; clamp finite DefaultTtl to [0,315360000]; parse Date/Expires using `FDateTime::ParseHttpDate`; calculate `InitialAge=max(valid Age, max(0, NowUtc-Date))`, treating invalid Age as an immediate stale result; lifetime=max-age, otherwise max(0,Expires-(Date or NowUtc)), otherwise default; collect trimmed lower-case Vary names with `IsValidHeaderName`, invalid names imply `bNoStore`. Empty Cache-Control has no directives. `CacheHeadersEligible` rejects request Range/If-Range/If-None-Match/If-Modified-Since, request `no-store`, response Vary `*`, invalid Vary names, Set-Cookie and WWW-Authenticate. The cache stage separately handles request `no-cache`/`max-age=0` by forcing validation.

`MergeNotModified` starts from Stored; for each Fresh pair replace its same-name values unless the name is hop-by-hop (`Connection`, tokens named by Connection, `Keep-Alive`, `Proxy-Authenticate`, `Proxy-Authorization`, `TE`, `Trailer`, `Transfer-Encoding`, `Upgrade`), `Content-Length`, `Set-Cookie`, or a secret/session header. Remove stored Warning values starting with `1`; for GET recalculate Content-Length from stored bytes, while HEAD preserves its stored representation Content-Length (which describes a GET representation, not the empty HEAD body). Use the persistence guard's safe-header filter from Task 3 before disk serialization; memory merge follows the same explicit forbidden-name list.

Add the remaining complete parser/merge bodies to `VaRestCachePolicy.cpp`:

```cpp
namespace VaRest::Http::CachePolicyPrivate
{
    TArray<FString> Tokens(const FVaRestHeaders& Headers, const TCHAR* Header)
    {
        TArray<FString> Result;
        for (const auto& Pair : Headers)
        {
            if (!Pair.Key.Equals(Header, ESearchCase::IgnoreCase)) continue;
            TArray<FString> Parts;
            Pair.Value.ParseIntoArray(Parts, TEXT(","), false);
            for (FString& Part : Parts) Result.Add(Part.TrimStartAndEnd());
        }
        return Result;
    }
    bool HasDirective(const FVaRestHeaders& Headers, const TCHAR* Wanted)
    {
        for (const FString& Directive : Tokens(Headers, TEXT("Cache-Control")))
        {
            FString Name, Value;
            if (!Directive.Split(TEXT("="), &Name, &Value)) Name = Directive;
            if (Name.TrimStartAndEnd().Equals(Wanted, ESearchCase::IgnoreCase)) return true;
        }
        return false;
    }
    bool Forbidden(const FString& Name, const FVaRestHeaders& Headers)
    {
        const FVaRestRedactRules Rules;
        if (Rules.ShouldRedact(Name, EVaRestRedactKeyKind::Header)) return true;
        for (const TCHAR* Item : {TEXT("Connection"),TEXT("Keep-Alive"),TEXT("Proxy-Authenticate"),
            TEXT("Proxy-Authorization"),TEXT("TE"),TEXT("Trailer"),TEXT("Transfer-Encoding"),TEXT("Upgrade"),TEXT("Content-Length")})
            if (Name.Equals(Item, ESearchCase::IgnoreCase)) return true;
        for (const FString& Item : Tokens(Headers,TEXT("Connection")))
            if (Name.Equals(Item, ESearchCase::IgnoreCase)) return true;
        return false;
    }
}
VaRest::Http::FCacheMetadata VaRest::Http::ReadCacheMetadata(const FVaRestHeaders& Headers,
    double DefaultTtl, double Now, FDateTime NowUtc)
{
    FCacheMetadata Result;
    Result.StoredAt = Now;
    Result.StoredUtc = NowUtc;
    Result.Lifetime = FMath::IsFinite(DefaultTtl) ? FMath::Clamp(DefaultTtl,0.0,315360000.0) : 0.0;
    bool bSawAge = false;
    bool bInvalidAge = false;
    double MaxAge = 0.0;
    for (const FString& Directive : CachePolicyPrivate::Tokens(Headers,TEXT("Cache-Control")))
    {
        FString Name, Value;
        if (!Directive.Split(TEXT("="), &Name, &Value)) Name = Directive;
        Name = Name.TrimStartAndEnd().ToLower();
        Value = Value.TrimStartAndEnd();
        if (Name == TEXT("no-store")) Result.bNoStore = true;
        if (Name == TEXT("no-cache")) Result.bNoCache = true;
        if (Name == TEXT("must-revalidate")) Result.bMustRevalidate = true;
        if (Name != TEXT("max-age")) continue;
        if (Value.Len() >= 2 && Value[0] == TEXT('"') && Value[Value.Len()-1] == TEXT('"'))
            Value = Value.Mid(1,Value.Len()-2);
        double Parsed = 0.0;
        if (!ReadDeltaSeconds(Value,Parsed) || (bSawAge && Parsed != MaxAge)) bInvalidAge = true;
        MaxAge = Parsed;
        bSawAge = true;
    }
    FDateTime Date, Expires;
    const bool bDate = FDateTime::ParseHttpDate(Headers.Get(TEXT("Date")),Date);
    const bool bExpires = FDateTime::ParseHttpDate(Headers.Get(TEXT("Expires")),Expires);
    if (bSawAge) Result.Lifetime = bInvalidAge ? 0.0 : MaxAge;
    else if (bExpires) Result.Lifetime = FMath::Clamp((Expires-(bDate ? Date : NowUtc)).GetTotalSeconds(),0.0,315360000.0);
    if (bDate) Result.InitialAge = FMath::Max(0.0,(NowUtc-Date).GetTotalSeconds());
    for (const auto& Pair : Headers)
    {
        if (!Pair.Key.Equals(TEXT("Age"),ESearchCase::IgnoreCase)) continue;
        double Parsed = 0.0;
        if (!ReadDeltaSeconds(Pair.Value.TrimStartAndEnd(),Parsed)) Result.Lifetime = 0.0;
        else Result.InitialAge = FMath::Max(Result.InitialAge,Parsed);
    }
    Result.ETag = Headers.Get(TEXT("ETag"));
    Result.LastModified = Headers.Get(TEXT("Last-Modified"));
    for (FString Name : CachePolicyPrivate::Tokens(Headers,TEXT("Vary")))
    {
        Name.ToLowerInline();
        if (Name == TEXT("*") || !IsValidHeaderName(Name)) Result.bNoStore = true;
        else Result.Vary.AddUnique(MoveTemp(Name));
    }
    return Result;
}
bool VaRest::Http::CacheHeadersEligible(const FVaRestHeaders& Request,const FVaRestHeaders& Response)
{
    for (const TCHAR* Name : {TEXT("Range"),TEXT("If-Range"),TEXT("If-None-Match"),TEXT("If-Modified-Since")})
        if (Request.Contains(Name)) return false;
    if (CachePolicyPrivate::HasDirective(Request,TEXT("no-store"))) return false;
    if (Response.Contains(TEXT("Set-Cookie")) || Response.Contains(TEXT("WWW-Authenticate"))) return false;
    for (const FString& Name : CachePolicyPrivate::Tokens(Response,TEXT("Vary")))
        if (Name == TEXT("*") || !IsValidHeaderName(Name) || !Request.Contains(Name)) return false;
    return true;
}
FVaRestHeaders VaRest::Http::MergeNotModified(const FVaRestHeaders& Stored,
    const FVaRestHeaders& Fresh,int64 BodyBytes,bool bHead)
{
    FVaRestHeaders Result;
    for (const auto& Pair : Stored)
    {
        if (CachePolicyPrivate::Forbidden(Pair.Key,Stored) || CachePolicyPrivate::Forbidden(Pair.Key,Fresh)) continue;
        if (Pair.Key.Equals(TEXT("Warning"),ESearchCase::IgnoreCase) && Pair.Value.TrimStart().StartsWith(TEXT("1"))) continue;
        Result.Add(Pair.Key,Pair.Value);
    }
    TArray<FString> Replaced;
    for (const auto& Pair : Fresh)
    {
        if (CachePolicyPrivate::Forbidden(Pair.Key,Fresh)) continue;
        const FString Lower = Pair.Key.ToLower();
        if (!Replaced.Contains(Lower)) { Result.Remove(Pair.Key); Replaced.Add(Lower); }
        Result.Add(Pair.Key,Pair.Value);
    }
    if (bHead)
    {
        const FString RepresentationLength=Stored.Get(TEXT("Content-Length"));
        if (!RepresentationLength.IsEmpty()) Result.Set(TEXT("Content-Length"),RepresentationLength);
    }
    else Result.Set(TEXT("Content-Length"),LexToString(BodyBytes));
    return Result;
}
```

Add this executable HEAD regression to the policy test body:

```cpp
FVaRestHeaders HeadStored;
HeadStored.Set(TEXT("Content-Length"),TEXT("7123"));
HeadStored.Set(TEXT("ETag"),TEXT("\"head-v1\""));
FVaRestHeaders NotModified; NotModified.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
const auto HeadMerged=MergeNotModified(HeadStored,NotModified,0,true);
TestEqual(TEXT("HEAD describes GET representation length"),HeadMerged.Get(TEXT("Content-Length")),FString(TEXT("7123")));
const auto GetMerged=MergeNotModified(HeadStored,NotModified,3,false);
TestEqual(TEXT("GET retains actual stored body length"),GetMerged.Get(TEXT("Content-Length")),FString(TEXT("3")));
```

Task 7's stage regression sends HEAD with ETag/max-age=0/Content-Length=7123 and zero bytes, then a physical304 and a fresh cached HEAD. Both logical responses retain7123 with empty body; trace wire304 appears only on the second request. Restore-the-bug by assigning stored Body.Num unconditionally; this exact length assertion fails.

- [ ] **Step 4: Expand table-driven tests.** Add concrete rows: empty/default TTL 0→stale; default TTL 5→fresh at 4.999/stale at 5; Expires=Date+20 and Age=5→15 seconds; future Date→age 0; malformed Date+valid Expires uses receive UTC; no-cache plus validator remains stored; must-revalidate denies stale; mixed-case Vary `Accept-Language` yields separate Task 1 keys; same URL with auth A/B differs; identical header insertion orders produce equal keys; URL fragment ignored while query order is preserved. 304 header merge retains ETag unless supplied, replaces Cache-Control, strips stale length/cookie, keeps original bytes. Run the filter green. Mutate `< Lifetime` to `<= Lifetime`, observe boundary failure, restore.
- [ ] **Step 5: Commit:** `git add Source/VaRestHttp/Public/VaRestCache.h Source/VaRestHttp/Private/VaRestCachePolicy.h Source/VaRestHttp/Private/VaRestCachePolicy.cpp Source/VaRestHttp/Private/Tests/VaRestHttpCachePolicyTest.cpp && git commit -m "feat(http): define cache freshness and variation policy"`.

### Task 3: Persistence boundary and canonical origin

**Files:** Create `Private/VaRestPersistenceGuard.h/.cpp`, `Private/Tests/VaRestHttpPersistenceGuardTest.cpp`; modify `Public/VaRestRedactor.h`, `Private/VaRestRedactor.cpp`, `Private/VaRestClientState.h/.cpp`, `Private/VaRestExchangeState.h`, `Private/VaRestClient.cpp`, Task 1 identity files.

**Interfaces:** Consumes raw `FVaRestSecretSet` slots, `IVaRestAuth::GetSecretValues`, `FVaRestRedactRules::ShouldRedact`, `VaRest::Json::RedactPrefix`. Produces:

```cpp
// namespace VaRest::Http
struct FPersistenceGuard
{
    FVaRestRedactRules Rules;
    TArray<TArray<uint8>> Needles;
    bool bRefusePersistence=false;
    int64 NeedleBytes=0;
    mutable bool bWorkExhausted=false;
    mutable int64 RemainingWork=128LL*1024*1024; // one private guard copy per candidate, worker-owned during scans
    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> AbortSignal;
    void AddSecret(FStringView Value);
    void AddKey(FStringView Key);
    void UnionWith(const FPersistenceGuard& Other);
    bool ContainsSecret(TConstArrayView<uint8> Bytes) const;
    bool AllowsBody(EVaRestBodyKind Kind, FStringView ContentType, TConstArrayView<uint8> Bytes) const;
    bool AllowsUrl(FStringView Url) const;
    FVaRestHeaders SafeHeaders(const FVaRestHeaders& Input) const;
};
bool CanonicalOrigin(FStringView Url, FString& OutOrigin);
bool SameOrigin(FStringView Left, FStringView Right);
// FVaRestSecretSet: includes nonempty current values of ANY byte length.
TArray<FString> GetCurrentForPersistence() const;
// FClientConfig: immutable snapshot rebuilt by Publish.
TSharedPtr<const FPersistenceGuard, ESPMode::ThreadSafe> PersistenceGuard;
// FExchangeState: snapshot + current guard union, captured before IO admission.
TSharedPtr<const FPersistenceGuard, ESPMode::ThreadSafe> PersistenceGuard;
TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> PersistenceAbort=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
```

`FClientConfig::PersistenceGuard` is built from all current raw game/session/auth slots, not `GetActive`. Old request snapshots retain their original guard through request completion. At enqueue/cache-write admission union snapshot + applied + current guard; `RememberAppliedRedactor` also accumulates the applied persistence guard, so a rotated credential used after Send remains known. The union method merges needle sets and added key rules; expose a `TArray<FString> ExtraKeys` in the guard, populate it from config/session/auth keys and reconstruct Rules when unioning. Do not infer rule equality by pointer.

- [ ] **Step 1: Add `VaRest.Http.PersistenceGuard.ShortBinaryLate`.**

```cpp
using namespace VaRest::Http;
FPersistenceGuard Guard;
Guard.AddSecret(TEXT("1234567")); // below the log redactor's minimum
TArray<uint8> Body;
Body.Init(static_cast<uint8>('x'), 8192);
const ANSICHAR* Token = "1234567";
Body.Append(reinterpret_cast<const uint8*>(Token), 7);
TestTrue(TEXT("scans beyond excerpt cap"), Guard.ContainsSecret(Body));
TestFalse(TEXT("binary is scanned, not a descriptor"), Guard.AllowsBody(EVaRestBodyKind::Binary, TEXT("application/octet-stream"), Body));
const FTCHARToUTF8 Json(TEXT("{\"padding\":\"safe\",\"password\":\"unknown-value\"}"));
TestFalse(TEXT("secret JSON key rejects unknown value"), Guard.AllowsBody(EVaRestBodyKind::Json, TEXT("application/json"),
    MakeArrayView(reinterpret_cast<const uint8*>(Json.Get()), Json.Length())));
TestFalse(TEXT("query token key"), Guard.AllowsUrl(TEXT("https://a.test/write?access_token=unknown")));
TestFalse(TEXT("userinfo"), Guard.AllowsUrl(TEXT("https://user:pw@a.test/write")));
FString Origin;
TestTrue(TEXT("default port canonical"), CanonicalOrigin(TEXT("HTTPS://A.TEST:443/x"), Origin));
TestEqual(TEXT("canonical result"), Origin, FString(TEXT("https://a.test:443")));
TestFalse(TEXT("scheme is part of origin"), SameOrigin(TEXT("http://a.test/x"), TEXT("https://a.test/x")));
TestFalse(TEXT("port is part of origin"), SameOrigin(TEXT("https://a.test:444/x"), TEXT("https://a.test/x")));
TestFalse(TEXT("userinfo refused even with equal host"), SameOrigin(TEXT("https://a.test"), TEXT("https://u@a.test/x")));
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.PersistenceGuard`.
- [ ] **Step 3: Implement complete-byte detection.** `AddSecret` adds unique raw UTF-8, uppercase/lowercase percent-encoded UTF-8, form-encoded UTF-8, JSON-escaped string contents, and lowercase/uppercase hex UTF-8. Empty values add nothing; no minimum byte count. JSON escaping uses `FVaRestJson::From(FString(Value)).ToString()` then removes the surrounding quotes, preserving the exact interior bytes. Add raw and encoded forms even if equal; `AddUnique` deduplicates arrays.

```cpp
bool VaRest::Http::FPersistenceGuard::ContainsSecret(TConstArrayView<uint8> Bytes) const
{
    if (bRefusePersistence || bWorkExhausted || RemainingWork<=0
        || (AbortSignal.IsValid() && AbortSignal->load(std::memory_order_relaxed))) return true;
    int64 Work=0;
    const auto Tick=[&]()
    {
        ++Work;
        if (RemainingWork<=0) { bWorkExhausted=true; return false; }
        --RemainingWork;
        if ((Work&4095)==0 && AbortSignal.IsValid() && AbortSignal->load(std::memory_order_relaxed))
        { bWorkExhausted=true; return false; }
        return true;
    };
    for (const TArray<uint8>& Needle : Needles)
    {
        if (Needle.IsEmpty() || Needle.Num()>Bytes.Num()) continue;
        TArray<int32> Prefix; Prefix.Init(0,Needle.Num());
        for (int32 Index=1,Matched=0;Index<Needle.Num();++Index)
        {
            while (Matched>0 && Needle[Index]!=Needle[Matched])
            { if (!Tick()) return true; Matched=Prefix[Matched-1]; }
            if (!Tick()) return true;
            if (Needle[Index]==Needle[Matched]) ++Matched;
            Prefix[Index]=Matched;
        }
        for (int32 Index=0,Matched=0;Index<Bytes.Num();++Index)
        {
            while (Matched>0 && Bytes[Index]!=Needle[Matched])
            { if (!Tick()) return true; Matched=Prefix[Matched-1]; }
            if (!Tick()) return true;
            if (Bytes[Index]==Needle[Matched]) ++Matched;
            if (Matched==Needle.Num()) return true;
        }
    }
    return false;
}
bool VaRest::Http::FPersistenceGuard::AllowsBody(EVaRestBodyKind Kind, FStringView ContentType,
    TConstArrayView<uint8> Bytes) const
{
    if (ContainsSecret(Bytes)) return false;
    if (Kind == EVaRestBodyKind::Json || FString(ContentType).Contains(TEXT("json")))
    {
        TArray<uint8> Discarded;
        const auto Result = VaRest::Json::RedactPrefix(
            FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()),
            Rules, EVaRestJsonFormat::Condensed, Discarded, true);
        return !Result.bRedacted && Result.StopReason == EVaRestRedactStop::Complete;
    }
    if (Kind == EVaRestBodyKind::Form)
    {
        for (int32 Start=0;Start<Bytes.Num();)
        {
            int32 End=Start; while (End<Bytes.Num() && Bytes[End]!='&') ++End;
            int32 Equals=Start; while (Equals<End && Bytes[Equals]!='=') ++Equals;
            if (Equals-Start>8192) return false; // bounded key conversion, never copy form values
            const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()+Start),Equals-Start);
            const FString Name(Converted.Length(),Converted.Get());
            if (Rules.ShouldRedact(PercentDecode(Name,true),EVaRestRedactKeyKind::Form)) return false;
            Start=End+1;
        }
    }
    return Kind != EVaRestBodyKind::Stream;
}
```

Add `int64 PersistenceGuardReservation(EVaRestBodyKind Kind,FStringView ContentType,int64 BodyBytes);` to VaRestPersistenceGuard.h and this body to its cpp. It includes the worker's immutable body copy. JSON structural scanning uses the existing full RedactPrefix implementation, whose output/parser/key buffers require a conservative transient allowance; large JSON can therefore refuse persistence before the32MiB item limit while binary remains eligible. This changes persistence admission only, never live-response delivery. Form scanning converts bounded key slices and ignores value copies.

```cpp
int64 VaRest::Http::PersistenceGuardReservation(EVaRestBodyKind Kind,FStringView ContentType,int64 BodyBytes)
{
    const bool bJson=Kind==EVaRestBodyKind::Json || FString(ContentType).Contains(TEXT("json"));
    return BodyBytes*(bJson ? 8LL : 1LL)+2*1024*1024;
}
```

The KMP loop is linear per needle, including repeated-prefix inputs. Limits are64 unique encoded needles,16KiB per needle,64KiB total needle bytes, and128Mi comparison steps across the complete candidate, body plus URL/header/serialized-metadata scans; exceeding any cap refuses persistence rather than concluding safe. At most4096 counted steps pass between atomic cancellation checks. Caps apply during union too; no secret is silently dropped to fit them. Structural parsing is bounded by the item bytes and parser depth. Add at most128 extra key rules/16KiB key text; exceeding that marks bRefusePersistence before Rules construction. ContainsSecret's true result includes refusal/work-limit/cancellation and is intentionally conservative. Every candidate uses a private guard copy; RemainingWork is consumed across all its scans and is never reset between body and metadata. Configuration snapshots are never scanned directly or shared mutably with workers.

Add shared `PersistenceAbort` atomic false to FExchangeState; accepted Cancel, owner/shutdown and a persistence deadline set it true. At IO admission set the copied Guard.AbortSignal=Ex.PersistenceAbort. This immutable worker input owns only an atomic, never the exchange/client/delegate. A successful logical response does not abort its best-effort cache write. Queue commit occurs after scanning and ignores later cancellation per its explicit acceptance cutoff.

The complete `VaRest.Http.PersistenceGuard.SharedPrefixBound` body uses the production work budget directly, so it does not substitute elapsed time for work:

```cpp
using namespace VaRest::Http;
TArray<uint8> Prefix; Prefix.Init(static_cast<uint8>('a'),8191); Prefix.Add(static_cast<uint8>('b'));
TArray<uint8> Body; Body.Init(static_cast<uint8>('a'),256*1024);
FPersistenceGuard Linear;
Linear.Needles.Add(Prefix); // exercise one exact raw KMP needle, independent of encoding expansion
const int64 Before=Linear.RemainingWork;
TestFalse(TEXT("shared prefix absent"),Linear.ContainsSecret(Body));
TestTrue(TEXT("linear comparisons"),Before-Linear.RemainingWork<=2LL*(Body.Num()+Prefix.Num()));
Body.Append(Prefix);
TestTrue(TEXT("late shared-prefix match"),Linear.ContainsSecret(Body));
FPersistenceGuard Budget;
Budget.Needles.Add(TArray<uint8>{static_cast<uint8>('a'),static_cast<uint8>('b')});
Budget.RemainingWork=4096;
TestTrue(TEXT("bounded refusal"),Budget.ContainsSecret(Body));
TestTrue(TEXT("budget not silently reset"),Budget.RemainingWork<=0);
TestTrue(TEXT("metadata cannot reset exhausted body budget"),Budget.ContainsSecret(TArray<uint8>{120}));
FPersistenceGuard Canceled;
Canceled.AbortSignal=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(true);
const int64 Untouched=Canceled.RemainingWork;
TestTrue(TEXT("canceled refuses immediately"),Canceled.ContainsSecret(Body));
TestEqual(TEXT("no scan after cancel"),Canceled.RemainingWork,Untouched);
FPersistenceGuard Many;
for (int32 Index=0;Index<65;++Index) Many.AddSecret(FString::Printf(TEXT("value-%03d"),Index));
TestTrue(TEXT("too many encoded needles refuse, never discard"),Many.bRefusePersistence);
return true;
```

The guard's128Mi comparison limit is shared across all scans of one candidate; tests deliberately lower it to make exhaustion cheap and deterministic. A restore-the-bug matcher that compares every prefix byte at every offset must count each byte comparison against RemainingWork; the linear-comparison assertion then fails on the shared-prefix input. Never replace the bound with a timing assertion.

For in-memory multipart, evaluate all PartDescriptors' names using `Part`, then scan the **entire encoded body** using ContainsSecret. A multipart secret name is refused even if the body doesn't contain a registered value. Do not parse arbitrary binary as text. For textual unknown content types, raw scans apply; undeclared credentials under unknown keys remain outside the guarantee.

`AllowsUrl` first calls CanonicalOrigin (rejects userinfo and malformed authorities); then compares `FVaRestRedactor(Rules, {}).RedactUrl(Url)` with Url to detect secret key structure, independently of value lengths; finally applies ContainsSecret to full UTF-8 URL. `SafeHeaders` omits every header matched by Rules.Header, every header whose value fails raw full-byte scan, and every session header name/standard auth/hop-by-hop name. It preserves Idempotency-Key. A queue send with an explicit request credential header not managed by its configured auth/session is Config/PersistenceCredentials: dropping that header would silently change the operation. Applied auth/session headers are never inputs to serialization; use Ex.RequestHeaders, then filter.

`CanonicalOrigin` parses scheme followed by `://`, cuts authority at first `/ ? #`, rejects whitespace/control/backslash, `%`, `@`, empty host, unbracketed multi-colon host, unmatched IPv6 brackets, empty/nondecimal/out-of-range port, and non-ASCII host. Lowercase ASCII scheme/host, default port 80/443, preserve bracketed IPv6 text, output `scheme://host:port`. Exact canonical string equality implements SameOrigin. Trailing DNS dot remains significant (safe conservative refusal). Do not use landed `GetUrlHost` alone: it drops scheme and port.

The complete identity-origin implementation in `VaRestIdentity.cpp` is:

```cpp
bool VaRest::Http::CanonicalOrigin(FStringView Url,FString& OutOrigin)
{
    OutOrigin.Reset();
    const FString Text(Url);
    int32 Separator = Text.Find(TEXT("://"),ESearchCase::CaseSensitive);
    if (Separator <= 0) return false;
    const FString Scheme = Text.Left(Separator).ToLower();
    if (Scheme != TEXT("http") && Scheme != TEXT("https")) return false;
    const int32 Begin = Separator + 3;
    int32 End = Begin;
    while (End < Text.Len() && Text[End]!=TEXT('/') && Text[End]!=TEXT('?') && Text[End]!=TEXT('#')) ++End;
    FString Authority = Text.Mid(Begin,End-Begin);
    if (Authority.IsEmpty()) return false;
    for (TCHAR Character : Authority)
        if (Character<=32 || Character>=127 || Character==TEXT('@') || Character==TEXT('%') || Character==TEXT('\\')) return false;
    FString Host, PortText;
    if (Authority.StartsWith(TEXT("[")))
    {
        int32 Close = INDEX_NONE;
        if (!Authority.FindChar(TEXT(']'),Close) || Close<=1) return false;
        Host = Authority.Left(Close+1);
        for (int32 Index=1;Index<Close;++Index)
        {
            const TCHAR Character=Authority[Index];
            if (!FChar::IsHexDigit(Character) && Character!=TEXT(':') && Character!=TEXT('.')) return false;
        }
        const FString Tail=Authority.Mid(Close+1);
        if (!Tail.IsEmpty())
        {
            if (!Tail.StartsWith(TEXT(":")) || Tail.Len()==1) return false;
            PortText=Tail.Mid(1);
        }
    }
    else
    {
        int32 Colon=INDEX_NONE;
        if (Authority.FindChar(TEXT(':'),Colon))
        {
            Host=Authority.Left(Colon); PortText=Authority.Mid(Colon+1);
            if (PortText.IsEmpty() || PortText.Contains(TEXT(":"))) return false;
        }
        else Host=Authority;
        for (TCHAR Character : Host)
            if (!FChar::IsAlnum(Character) && Character!=TEXT('.') && Character!=TEXT('-')) return false;
    }
    if (Host.IsEmpty()) return false;
    uint32 Port = Scheme==TEXT("https") ? 443 : 80;
    if (!PortText.IsEmpty())
    {
        Port=0;
        for (TCHAR Character : PortText)
        {
            if (Character<TEXT('0') || Character>TEXT('9')) return false;
            const uint32 Digit=static_cast<uint32>(Character-TEXT('0'));
            if (Port>(65535-Digit)/10) return false;
            Port=Port*10+Digit;
        }
        if (Port==0) return false;
    }
    OutOrigin=Scheme+TEXT("://")+Host.ToLower()+TEXT(":")+LexToString(Port);
    return true;
}
bool VaRest::Http::SameOrigin(FStringView Left,FStringView Right)
{
    FString A,B;
    return CanonicalOrigin(Left,A) && CanonicalOrigin(Right,B) && A==B;
}
```

IPv6 textual alternatives conservatively compare different and therefore refuse sharing; accepting them is not required for correctness. IPv6 syntax validation by actual transport remains in Resolve; the persistence comparison never broadens an authority match.

The remaining guard bodies are:

```cpp
TArray<FString> FVaRestSecretSet::GetCurrentForPersistence() const
{
    TArray<FString> Result;
    for (const auto& Pair : Current) if (!Pair.Value.IsEmpty()) Result.Add(Pair.Value); // byte-array deduplication happens in AddSecret
    return Result;
}
void VaRest::Http::FPersistenceGuard::AddSecret(FStringView Value)
{
    if (Value.IsEmpty()) return;
    if (Value.Len()>8192) { bRefusePersistence=true; return; }
    const auto AddForm=[this](FStringView Form)
    {
        const FTCHARToUTF8 Utf8(Form.GetData(),Form.Len());
        TArray<uint8> Bytes(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
        if (Needles.Contains(Bytes)) return;
        if (Needles.Num()>=64 || Utf8.Length()>16384 || NeedleBytes>65536-Utf8.Length())
        { bRefusePersistence=true; return; }
        NeedleBytes+=Utf8.Length(); Needles.Add(MoveTemp(Bytes));
    };
    AddForm(Value);
    FString Percent=PercentEncode(Value);
    AddForm(Percent);
    for (int32 Index=0;Index+2<Percent.Len();++Index)
        if (Percent[Index]==TEXT('%')) { Percent[Index+1]=FChar::ToLower(Percent[Index+1]); Percent[Index+2]=FChar::ToLower(Percent[Index+2]); Index+=2; }
    AddForm(Percent);
    FString Form=FormEncode(Value);
    AddForm(Form);
    for (int32 Index=0;Index+2<Form.Len();++Index)
        if (Form[Index]==TEXT('%')) { Form[Index+1]=FChar::ToLower(Form[Index+1]); Form[Index+2]=FChar::ToLower(Form[Index+2]); Index+=2; }
    AddForm(Form);
    const FString Quoted=FVaRestJson::From(FString(Value)).ToString();
    if (Quoted.Len()>=2) AddForm(Quoted.Mid(1,Quoted.Len()-2));
    const FTCHARToUTF8 Raw(Value.GetData(),Value.Len());
    FString Hex;
    const TCHAR* Digits=TEXT("0123456789abcdef");
    for (int32 Index=0;Index<Raw.Length();++Index)
    {
        const uint8 Byte=static_cast<uint8>(Raw.Get()[Index]);
        Hex.AppendChar(Digits[Byte>>4]); Hex.AppendChar(Digits[Byte&15]);
    }
    AddForm(Hex); AddForm(Hex.ToUpper());
}
void VaRest::Http::FPersistenceGuard::AddKey(FStringView Key)
{
    if (ExtraKeys.Contains(FString(Key))) return;
    int64 Bytes=0;
    for (const FString& Existing : ExtraKeys) { const FTCHARToUTF8 Utf8(*Existing); Bytes+=Utf8.Length(); }
    const FTCHARToUTF8 Incoming(Key.GetData(),Key.Len());
    if (ExtraKeys.Num()>=128 || Incoming.Length()>16384-Bytes)
    { bRefusePersistence=true; return; }
    if (!FVaRestRedactRules::IsAcceptableKey(Key)) { bRefusePersistence=true; return; }
    ExtraKeys.Add(FString(Key)); Rules.AddKey(Key);
}
void VaRest::Http::FPersistenceGuard::UnionWith(const FPersistenceGuard& Other)
{
    bRefusePersistence|=Other.bRefusePersistence;
    for (const auto& Needle : Other.Needles)
    {
        if (Needles.Contains(Needle)) continue;
        if (Needles.Num()>=64 || NeedleBytes>65536-Needle.Num()) { bRefusePersistence=true; break; }
        NeedleBytes+=Needle.Num(); Needles.Add(Needle);
    }
    for (const FString& Key : Other.ExtraKeys) AddKey(Key);
}
bool VaRest::Http::FPersistenceGuard::AllowsUrl(FStringView Url) const
{
    FString Origin;
    if (!CanonicalOrigin(Url,Origin)) return false;
    const FVaRestRedactor Structural(Rules,TArray<FString>());
    if (!Structural.RedactUrl(Url).Equals(FString(Url),ESearchCase::CaseSensitive)) return false;
    const FTCHARToUTF8 Utf8(Url.GetData(),Url.Len());
    return !ContainsSecret(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()));
}
FVaRestHeaders VaRest::Http::FPersistenceGuard::SafeHeaders(const FVaRestHeaders& Input) const
{
    FVaRestHeaders Result;
    TArray<FString> HopNames{TEXT("connection"),TEXT("keep-alive"),TEXT("proxy-authenticate"),
        TEXT("proxy-authorization"),TEXT("te"),TEXT("trailer"),TEXT("transfer-encoding"),TEXT("upgrade")};
    for (const auto& Pair : Input)
        if (Pair.Key.Equals(TEXT("Connection"),ESearchCase::IgnoreCase))
        {
            TArray<FString> Named; Pair.Value.ParseIntoArray(Named,TEXT(","),false);
            for (const FString& Name : Named) HopNames.AddUnique(Name.TrimStartAndEnd().ToLower());
        }
    for (const auto& Pair : Input)
    {
        if (Rules.ShouldRedact(Pair.Key,EVaRestRedactKeyKind::Header) || HopNames.Contains(Pair.Key.ToLower())) continue;
        const FTCHARToUTF8 Utf8(*Pair.Value,Pair.Value.Len());
        if (ContainsSecret(MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()))) continue;
        Result.Add(Pair.Key,Pair.Value);
    }
    return Result;
}
```

Add `TArray<FString> ExtraKeys;` to FPersistenceGuard. At Publish, after current Secrets changes have been applied and before swapping config, build a new guard:

```cpp
auto Guard=MakeShared<FPersistenceGuard,ESPMode::ThreadSafe>();
for (const FString& Key : Next->RedactedKeys) Guard->AddKey(Key);
for (const FString& Secret : Secrets.GetCurrentForPersistence()) Guard->AddSecret(Secret);
Next->PersistenceGuard=MoveTemp(Guard);
```

At Send assign Ex.PersistenceGuard=Ex.Config->PersistenceGuard. In RememberAppliedRedactor, before its existing early return, create/copy a guard from Ex.PersistenceGuard, UnionWith current config guard, and publish the immutable union back on Ex. At IO admission copy it and UnionWith the latest guard one more time, then assign AbortSignal=Ex.PersistenceAbort. Auth Apply itself remains exactly once per real attempt, never called by a guard.

- [ ] **Step 4: Test captured-secret lifetimes.** Send with token `short-A`, change token to `short-B`, then clear before releasing the persistence barrier; old A/B byte patterns must still be refused by the exchange union, while a new request sees only the current snapshot. Check raw/percent/form/JSON/hex forms, embedded NUL binary, a secret after 4 KiB, secret header aliases, plain nonsecret payload preserved byte-for-byte. Run the guard filter green and restore-the-bug by adding `if (Needle.Num()<8) continue`; the short-token assertion must fail.
- [ ] **Step 5: Commit:** `git add Source/VaRestHttp/Private/VaRestPersistenceGuard.h Source/VaRestHttp/Private/VaRestPersistenceGuard.cpp Source/VaRestHttp/Private/Tests/VaRestHttpPersistenceGuardTest.cpp Source/VaRestHttp/Public/VaRestRedactor.h Source/VaRestHttp/Private/VaRestRedactor.cpp Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestIdentity.h Source/VaRestHttp/Private/VaRestIdentity.cpp && git commit -m "feat(http): enforce credential-free persistence boundaries"`.

### Task 4: Serial background persistence and interruptible waits

**Files:** Create `Private/VaRestPersistence.h/.cpp`, `Private/VaRestPersistenceWait.h/.cpp`, `Private/Tests/VaRestHttpPersistenceTest.cpp`; modify `Public/VaRestHttpTesting.h`, `Private/VaRestHttpTesting.cpp`, `Private/VaRestHttpModule.cpp`, `Private/VaRestExchangeState.h`, `Private/VaRestClientState.h`, `Private/VaRestPipeline.cpp`.

**Interfaces:** Consumes existing `MakeTempFileName`, `MoveIntoPlace`, `PlatformFileOps`, `MakeDeadlineOutcome`, `FExchangeState::Interrupt`, `IVaRestClock`. Produces:

```cpp
// VaRestPersistence.h, namespace VaRest::Http
struct FIoResult
{
    bool bOk = false;
    FString Detail;
    TArray<uint8> Bytes;
    TArray<FString> Names;
};
struct FIoCounters
{
    std::atomic<uint64> Launched{0};
    std::atomic<uint64> HandedBack{0};
    std::atomic<uint64> Delivered{0};
    std::atomic<uint64> OperationsOnGameThread{0};
};
using FIoWork = TUniqueFunction<FIoResult()>;
using FIoDone = TUniqueFunction<void(FIoResult&&)>;
bool LaunchPersistence(FIoWork&& Work, FIoDone&& Done, int64 ReservedBytes=0, bool bControlDuringStop=false); // GT admission only
void StartPersistence();
void PumpPersistence(); // GT only, consumes this executor's slots
bool PersistenceIdle(); // GT only, includes callbacks and deferred continuations
bool PersistenceAccepting(); // GT only
bool PostPersistenceContinuation(TUniqueFunction<void()>&& Callback);
bool PostPersistencePoll(TUniqueFunction<bool()>&& Callback);
uint64 AcquirePersistenceCompletion(); // GT, bounded permit acquired before a persistence wait
void SetPersistenceCompletion(uint64 Ticket,TUniqueFunction<void()>&& Callback);
void ReleasePersistenceCompletion(uint64 Ticket); // GT, idempotent, destroys pending completion on GT
void StopPersistence(); // worker drain + silent internal commit accounting; suppresses user handbacks at pre-exit
FIoCounters& PersistenceCounters();
FIoResult ReadBoundedFile(const FString& Path, int64 MaxBytes);
FIoResult WriteNewFile(const FString& Path, TConstArrayView<uint8> Bytes);
FIoResult DeleteFile(const FString& Path);
FIoResult ListFiles(const FString& Directory);
// VaRestPersistenceWait.h, namespace VaRest::Http
class FPersistenceWait : public TSharedFromThis<FPersistenceWait, ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FPersistenceWait, ESPMode::ThreadSafe> Begin(
        const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done);
    bool IsPending() const { return !bEnded; } // GT only
    bool CanHandoff(); // checks live cancellation/owner/deadline before consuming any result
    void Finish(FVaRestAttemptOutcome&& Outcome);
    // End hold without answering; transfer callback to the next pipeline phase.
    FVaRestOutcomeCallback Resume();
private:
    FPersistenceWait(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done);
    FVaRestOutcomeCallback TakeCompletion();
    void FinishUnchecked(FVaRestAttemptOutcome&& Outcome);
    friend class FQueueAdmissionJob; // Task 8 alone uses TakeCompletion after its accepted commit CAS
    bool InterruptNow();
    void CheckDeadline();
    void Disarm();
    FVaRestExchangeRef HeldExchange;
    FVaRestOutcomeCallback Completion;
    FVaRestTimerHandle Timer;
    bool bEnded = false;
};
```

The worker executor is one process-lifetime UE::Tasks::FPipe with GT-only admission. Workers capture immutable DTOs and paths, never client/exchange/clock/delegates. They publish results into executor-owned slots under a small mutex; no worker queues a GT task. One core-ticker callback consumes only those slots on GT. Callbacks are removed before invocation and no mutex is held during callbacks. Test PumpUntil calls PumpPersistence in its loop, so virtual-clock tests do not need to tick the real core ticker. Stop removes the ticker, closes admission, drains workers and consumes only its own result slots; tagged accepted-commit continuations finish silent accounting, others are destroyed on GT. No general TaskGraph pump, unrelated callback, or persistence wrapper runs during Stop. Queue replay deferrals use the executor's GT-only Deferred queue, which Stop discards. Queued-result events run directly from the already-asynchronous retirement handback after taking a stable subscriber snapshot; no second general GT task is queued.

- [ ] **Step 1: Add a controllable test barrier.** Under WITH_DEV_AUTOMATION_TESTS, define `FPersistenceBarrier { std::atomic<bool> Entered{false}; std::atomic<bool> Released{false}; }`. A scoped test hook, injected per job rather than global, sets Entered on worker and uses `FPlatformProcess::SleepNoStats(0.001f)` until Released. The test calls `PumpUntil(Entered)`, performs cancellation/clock advance/clear, releases, then drains. Test teardown releases unconditionally via `ON_SCOPE_EXIT` before assertions can return.

Register `VaRest.Http.Persistence.WorkerOrdering`: submit write("a"), read, delete, read; callbacks must be [ok, bytes="a", ok, miss], worker count equals 4, GT-operation delta=0 and callback order [1,2,3,4]. Register `VaRest.Http.Persistence.WaitCancelDeadline`: install FOutcomeProbe outside a holding test stage; cancel once and advance past deadline while IO is blocked; after release `Probe.Total(Id)==1`, ErrorCalls==1/Canceled, no fake call; a second request deadline-only yields Timeout and exactly one outer outcome. Code below supplies the wait being tested.

Use these actual per-directory hook declarations in VaRestPersistence.h under WITH_DEV_AUTOMATION_TESTS (all under VaRest::Http::Testing). They are private test controls, never public shipping configuration:

```cpp
struct FPersistenceBarrier
{
    std::atomic<bool> Entered{false},Released{false},TimedOut{false};
};
struct FPersistenceHooks
{
    TSharedPtr<FPersistenceBarrier,ESPMode::ThreadSafe> BeforeRead,BeforeQueueLoad,BeforeCacheWrite,AfterPrepare,BeforeRename;
    bool bFailCommit=false,bFailCacheControl=false; // configured before admission, immutable on worker
};
int64 PersistencePendingBytesForTest();
TSharedPtr<const FPersistenceHooks,ESPMode::ThreadSafe> PersistenceHooksFor(const FString& Directory);
void WaitPersistenceBarrier(const TSharedPtr<FPersistenceBarrier,ESPMode::ThreadSafe>& Barrier);
class FScopedPersistenceHooks
{
public:
    explicit FScopedPersistenceHooks(FString InRoot);
    ~FScopedPersistenceHooks();
    FString Root;
    TSharedRef<FPersistenceHooks,ESPMode::ThreadSafe> Hooks;
};
```

Definitions in VaRestPersistence.cpp:

```cpp
#if WITH_DEV_AUTOMATION_TESTS
namespace VaRest::Http::Testing
{
    TMap<FString,TWeakPtr<FPersistenceHooks,ESPMode::ThreadSafe>>& HookRegistry()
    { static auto* Registry=new TMap<FString,TWeakPtr<FPersistenceHooks,ESPMode::ThreadSafe>>(); return *Registry; }
    FScopedPersistenceHooks::FScopedPersistenceHooks(FString InRoot)
        : Root(MoveTemp(InRoot)),Hooks(MakeShared<FPersistenceHooks,ESPMode::ThreadSafe>())
    { check(IsInGameThread()); HookRegistry().Add(Root,Hooks); }
    FScopedPersistenceHooks::~FScopedPersistenceHooks()
    {
        check(IsInGameThread());
        for (const auto& Barrier : {Hooks->BeforeRead,Hooks->BeforeQueueLoad,Hooks->BeforeCacheWrite,Hooks->AfterPrepare,Hooks->BeforeRename})
            if (Barrier.IsValid()) Barrier->Released=true;
        HookRegistry().Remove(Root);
    }
    TSharedPtr<const FPersistenceHooks,ESPMode::ThreadSafe> PersistenceHooksFor(const FString& Directory)
    {
        check(IsInGameThread());
        for (const auto& Pair : HookRegistry())
            if (Directory==Pair.Key || FPaths::IsUnderDirectory(Directory,Pair.Key)) return Pair.Value.Pin();
        return nullptr;
    }
    void WaitPersistenceBarrier(const TSharedPtr<FPersistenceBarrier,ESPMode::ThreadSafe>& Barrier)
    {
        if (!Barrier.IsValid()) return;
        check(!IsInGameThread()); Barrier->Entered=true;
        const double Deadline=FPlatformTime::Seconds()+30.0;
        while (!Barrier->Released.load())
        {
            if (FPlatformTime::Seconds()>=Deadline) { Barrier->TimedOut=true; return; }
            FPlatformProcess::SleepNoStats(0.001f);
        }
    }
}
#endif
```

At each GT store entry, snapshot `const auto Hooks=Testing::PersistenceHooksFor(Directory)` under WITH_DEV_AUTOMATION_TESTS and capture it in the worker lambda under the same macro. FDiskCache::Find already calls its BeforeRead hook explicitly and FQueueStore::Load its BeforeQueueLoad hook; FQueueStore::Prepare stores WriteNewFile's result, calls AfterPrepare then returns that result; Commit and cache Put already show their exact BeforeRename/BeforeCacheWrite hooks in their bodies; do not insert a second wait. No worker reads HookRegistry or captures a user delegate. A hook object is configured fully before the operation is submitted and is never changed while a worker uses it; only its atomics change. A timed-out hook makes the test fail even if the operation later succeeds.

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Persistence`.
- [ ] **Step 3: Implement file operations.** All these functions assert `!IsInGameThread()` and increment OperationsOnGameThread before a diagnostic ensure if violated. `ReadBoundedFile` opens via `IPlatformFile::OpenRead`, rejects missing/negative/oversized length before allocation, reads exactly Length bytes, refuses short reads. `WriteNewFile` creates the parent directory on worker, opens a unique `MakeTempFileName(Path,0)`, writes bytes, calls `Flush(true)`, closes, then moves to a previously absent final Path; on every failure delete that temp. Queue/cache generations use immutable unique final filenames, avoiding the non-atomic Windows replacement window. Test controls inject prepare/commit failures, and bounded-file tests exercise absent/truncated inputs. The landed FFileOps only wraps move/delete/is-file; do not invent write/flush members on that type. Metadata is bundled with body in the same record, never two independently committed files. `DeleteFile` treats already absent as success; `ListFiles` returns only direct regular files and never follows directory links into another root.

The GT submission core is:

```cpp
bool VaRest::Http::LaunchPersistence(FIoWork&& Work,FIoDone&& Done,int64 ReservedBytes,bool bControlDuringStop)
{
    check(IsInGameThread());
    auto& Runtime=PersistenceRuntime();
    if (!Runtime.bAccepting || ReservedBytes<0 || ReservedBytes>128*1024*1024
        || Runtime.PendingBytes>128*1024*1024-ReservedBytes || Runtime.Callbacks.Num()>=8192) return false;
    Runtime.PendingBytes+=ReservedBytes;
    const uint64 Ticket=++Runtime.NextTicket;
    Runtime.Callbacks.Add(Ticket,FIoContinuation{MoveTemp(Done),bControlDuringStop,ReservedBytes});
    ++Runtime.Counters.Launched;
    Runtime.Pipe.Launch(TEXT("VaRest.Persistence"),[Ticket,Job=MoveTemp(Work)]() mutable
    {
        FIoResult Result=Job(); Job.Reset();
        auto& Current=PersistenceRuntime();
        {
            FScopeLock Lock(&Current.CompletedLock);
            Current.Completed.Emplace(Ticket,MoveTemp(Result));
        }
        ++Current.Counters.HandedBack;
    });
    return true;
}

```

Place the runtime declarations below before LaunchPersistence's definition. StartPersistence is called beside StartRedactionPipe; module PreExit in this task orders CancelForShutdown then StopPersistence; the queue integration later inserts FOfflineQueue::StopAll between them. ShutdownModule repeats StopPersistence idempotently before stopping redaction. There is no queued GT persistence lambda referring to module code after Stop.

The concrete file-operation bodies in VaRestPersistence.cpp include `HAL/PlatformFileManager.h`, `Misc/Paths.h`, `VaRestDecode.h`, and `Misc/ScopeExit.h`:

```cpp
namespace VaRest::Http::PersistencePrivate
{
    void CheckWorker()
    {
        if (IsInGameThread()) ++PersistenceCounters().OperationsOnGameThread;
        check(!IsInGameThread());
    }
    FIoResult Failure(const TCHAR* Detail) { FIoResult Result; Result.Detail=Detail; return Result; }
}
VaRest::Http::FIoResult VaRest::Http::ReadBoundedFile(const FString& Path,int64 MaxBytes)
{
    PersistencePrivate::CheckWorker();
    IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
    if (Platform.IsSymlink(*Path)==ESymlinkResult::Symlink) return PersistencePrivate::Failure(TEXT("Symlink"));
    TUniquePtr<IFileHandle> File(Platform.OpenRead(*Path));
    if (!File.IsValid()) return PersistencePrivate::Failure(TEXT("Missing"));
    const int64 Size=File->Size();
    if (Size<0 || Size>MaxBytes || Size>MAX_int32) return PersistencePrivate::Failure(TEXT("Size"));
    FIoResult Result;
    Result.Bytes.SetNumUninitialized(static_cast<int32>(Size));
    if (Size>0 && !File->Read(Result.Bytes.GetData(),Size)) return PersistencePrivate::Failure(TEXT("Read"));
    Result.bOk=true;
    return Result;
}
VaRest::Http::FIoResult VaRest::Http::WriteNewFile(const FString& Path,TConstArrayView<uint8> Bytes)
{
    PersistencePrivate::CheckWorker();
    IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
    if (Platform.IsSymlink(*FPaths::GetPath(Path))==ESymlinkResult::Symlink
        || Platform.IsSymlink(*Path)==ESymlinkResult::Symlink) return PersistencePrivate::Failure(TEXT("Symlink"));
    if (Platform.FileExists(*Path) || Platform.DirectoryExists(*Path)) return PersistencePrivate::Failure(TEXT("Exists"));
    if (!Platform.CreateDirectoryTree(*FPaths::GetPath(Path))) return PersistencePrivate::Failure(TEXT("Directory"));
    const FString Temporary=MakeTempFileName(Path,0);
    ON_SCOPE_EXIT { Platform.DeleteFile(*Temporary); };
    TUniquePtr<IFileHandle> File(Platform.OpenWrite(*Temporary,false,false));
    if (!File.IsValid()) return PersistencePrivate::Failure(TEXT("Open"));
    if (Bytes.Num()>0 && !File->Write(Bytes.GetData(),Bytes.Num())) return PersistencePrivate::Failure(TEXT("Write"));
    if (!File->Flush(true)) return PersistencePrivate::Failure(TEXT("Flush"));
    File.Reset();
    if (!Platform.MoveFile(*Path,*Temporary)) return PersistencePrivate::Failure(TEXT("Rename"));
    FIoResult Result; Result.bOk=true; return Result;
}
VaRest::Http::FIoResult VaRest::Http::DeleteFile(const FString& Path)
{
    PersistencePrivate::CheckWorker();
    IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
    FIoResult Result;
    Result.bOk=!Platform.FileExists(*Path) || Platform.DeleteFile(*Path);
    if (!Result.bOk) Result.Detail=TEXT("Delete");
    return Result;
}
VaRest::Http::FIoResult VaRest::Http::ListFiles(const FString& Directory)
{
    PersistencePrivate::CheckWorker();
    IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
    FIoResult Result;
    if (Platform.IsSymlink(*Directory)==ESymlinkResult::Symlink) return PersistencePrivate::Failure(TEXT("Symlink"));
    if (!Platform.DirectoryExists(*Directory)) { Result.bOk=true; return Result; }
    int64 NameChars=0,NameBytes=0;
    Result.bOk=Platform.IterateDirectory(*Directory,[&Result,&Platform,&NameChars,&NameBytes](const TCHAR* Path,bool bDirectory)
    {
        if (Result.Names.Num()>=8192) { Result.Detail=TEXT("DirectoryLimit"); return false; }
        if (!bDirectory && Platform.IsSymlink(Path)!=ESymlinkResult::Symlink)
        {
            FString Name=FPaths::GetCleanFilename(Path);
            if (Name.Len()>256 || NameChars+Name.Len()>524288 || NameBytes+Name.GetAllocatedSize()>2*1024*1024)
            { Result.Detail=TEXT("DirectoryLimit"); return false; }
            NameChars+=Name.Len(); NameBytes+=Name.GetAllocatedSize(); Result.Names.Add(MoveTemp(Name));
        }
        return true;
    });
    return Result;
}
```

A known symlink is refused. On platforms reporting IsSymlink=Unimplemented, storage is still supported in application-owned directories; this is not a hostile-filesystem sandbox. Strict generated filenames reject traversal, but no portable `nofollow/openat` guarantee is claimed against a malicious concurrent local actor. A missing directory is an empty store, not an IO error. Fault tests inject platform operations through a private worker adapter, not global filesystem mutation.

For parsed DTO handback, add this template to VaRestPersistence.h; its box contains no engine/user object and is never mutated concurrently:

```cpp
template <typename ResultType>
bool LaunchPersistenceResult(TUniqueFunction<ResultType()>&& Work,
    TUniqueFunction<void(ResultType&&)>&& Done, int64 ReservedBytes=0)
{
    const auto Box=MakeShared<TOptional<ResultType>,ESPMode::ThreadSafe>();
    return LaunchPersistence([Box,Job=MoveTemp(Work)]() mutable
    {
        Box->Emplace(Job());
        FIoResult Status; Status.bOk=true; return Status;
    },[Box,Callback=MoveTemp(Done)](FIoResult&& Status) mutable
    {
        check(Status.bOk && Box->IsSet());
        Callback(MoveTemp(Box->GetValue()));
    },ReservedBytes);
}
```

It is used by disk cache parse and queue Load so JSON parsing stays on worker; GT receives only validated FCacheEntry/FQueueLoad DTOs. Start/stop/counter bodies:

```cpp
namespace VaRest::Http
{
    struct FIoContinuation { FIoDone Done; bool bControlDuringStop=false; int64 Reservation=0; };
    struct FPersistenceRuntime
    {
        UE::Tasks::FPipe Pipe{TEXT("VaRest.Persistence")};
        bool bAccepting=false;
        uint64 NextTicket=0;
        int64 PendingBytes=0; // GT only; includes result slots awaiting GT consumption
        TMap<uint64,FIoContinuation> Callbacks;
        FCriticalSection CompletedLock;
        TArray<TPair<uint64,FIoResult>> Completed;
        TArray<TUniqueFunction<bool()>> Deferred;
        int32 DeferredSlots=0; // includes callbacks temporarily held by PumpOn
        uint64 NextCompletionTicket=0;
        TMap<uint64,TUniqueFunction<void()>> OwnedCompletions; // 8192 separate permits, includes unready waits
        FTSTicker::FDelegateHandle Ticker;
        FIoCounters Counters;
    };
    #if WITH_DEV_AUTOMATION_TESTS
    FPersistenceRuntime* TestRuntime=nullptr; // GT lookup only; worker always captures its admitted Runtime
#endif
    FPersistenceRuntime& PersistenceRuntime()
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (IsInGameThread() && TestRuntime) return *TestRuntime;
#endif
        static FPersistenceRuntime* Instance=new FPersistenceRuntime(); return *Instance;
    }
    FIoCounters& PersistenceCounters() { return PersistenceRuntime().Counters; }
    void PumpPersistence()
    {
        check(IsInGameThread());
        auto& Runtime=PersistenceRuntime();
        TArray<TPair<uint64,FIoResult>> Completed;
        {
            FScopeLock Lock(&Runtime.CompletedLock); Completed=MoveTemp(Runtime.Completed);
        }
        auto Deferred=MoveTemp(Runtime.Deferred); // callbacks posted while pumping wait until next pump
        TArray<uint64> ReadyCompletions;
        if (Runtime.bAccepting)
            for (const auto& Pair : Runtime.OwnedCompletions) if (Pair.Value) ReadyCompletions.Add(Pair.Key);
        ReadyCompletions.Sort(); // admission order; newly ready entries wait until next pump
        for (auto& Slot : Completed)
        {
            auto* Found=Runtime.Callbacks.Find(Slot.Key);
            check(Found);
            FIoContinuation Continuation=MoveTemp(*Found);
            Runtime.Callbacks.Remove(Slot.Key);
            if ((Runtime.bAccepting || Continuation.bControlDuringStop) && Continuation.Done)
                Continuation.Done(MoveTemp(Slot.Value));
            Slot.Value=FIoResult();
            Continuation.Done.Reset(); // discard DTO boxes on GT before releasing their reservation
            Runtime.PendingBytes-=Continuation.Reservation;
            ++Runtime.Counters.Delivered;
        }
        for (uint64 Ticket : ReadyCompletions)
        {
            auto* Found=Runtime.OwnedCompletions.Find(Ticket);
            if (!Found || !Runtime.bAccepting) continue;
            auto Callback=MoveTemp(*Found);
            Runtime.OwnedCompletions.Remove(Ticket);
            if (Callback) Callback(); // no general GT queue; normal Complete releases idempotently
        }
        for (auto& Callback : Deferred)
        {
            if (Runtime.bAccepting && !Callback()) Runtime.Deferred.Add(MoveTemp(Callback));
            else --Runtime.DeferredSlots;
        }
    }
    bool PostPersistencePoll(TUniqueFunction<bool()>&& Callback)
    {
        check(IsInGameThread());
        auto& Runtime=PersistenceRuntime();
        if (!Runtime.bAccepting || Runtime.DeferredSlots>=8192) return false;
        ++Runtime.DeferredSlots; Runtime.Deferred.Add(MoveTemp(Callback)); return true;
    }
    bool PostPersistenceContinuation(TUniqueFunction<void()>&& Callback)
    {
        return PostPersistencePoll([Callback=MoveTemp(Callback)]() mutable
        { Callback(); return true; });
    }
    uint64 AcquirePersistenceCompletion()
    {
        check(IsInGameThread());
        auto& Runtime=PersistenceRuntime();
        if (!Runtime.bAccepting || Runtime.OwnedCompletions.Num()>=8192 || Runtime.NextCompletionTicket==MAX_uint64) return 0;
        const uint64 Ticket=++Runtime.NextCompletionTicket;
        Runtime.OwnedCompletions.Add(Ticket,TUniqueFunction<void()>());
        return Ticket;
    }
    void SetPersistenceCompletion(uint64 Ticket,TUniqueFunction<void()>&& Callback)
    {
        check(IsInGameThread());
        auto& Runtime=PersistenceRuntime();
        auto* Slot=Runtime.OwnedCompletions.Find(Ticket);
        check(Slot && Runtime.bAccepting); // acquired before wait; no fallible second admission
        *Slot=MoveTemp(Callback); // later Cancel may replace the accepted outcome, still asynchronous
    }
    void ReleasePersistenceCompletion(uint64 Ticket)
    {
        check(IsInGameThread());
        if (Ticket) PersistenceRuntime().OwnedCompletions.Remove(Ticket);
    }
    void StartPersistence()
    {
        check(IsInGameThread());
        auto& Runtime=PersistenceRuntime();
        check(!Runtime.Pipe.HasWork() && Runtime.Callbacks.IsEmpty());
        Runtime.bAccepting=true;
        Runtime.Ticker=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([](float)
        { PumpPersistence(); return true; }));
    }
    void StopPersistence()
    {
        check(IsInGameThread());
        auto& Runtime=PersistenceRuntime();
        if (!Runtime.bAccepting) return;
        Runtime.bAccepting=false;
        FTSTicker::GetCoreTicker().RemoveTicker(Runtime.Ticker);
        Runtime.Ticker.Reset();
        Runtime.DeferredSlots-=Runtime.Deferred.Num();
        Runtime.Deferred.Reset();
        Runtime.Pipe.WaitUntilEmpty(); // workers never depend on GT acknowledgement
        PumpPersistence(); // only executor-owned slots; no TaskGraph/ticker/general message pump
        Runtime.OwnedCompletions.Empty(); // canceled exchanges settled first; discard any pending closure, never user delivery
        check(Runtime.Callbacks.IsEmpty() && Runtime.PendingBytes==0);
        check(Runtime.Counters.Delivered.load()==Runtime.Counters.Launched.load());
    }
}

```

Completion permits form a separate bounded8192-entry GT map: acquire before installing a persistence wait, retain through all waits and accepted API deferrals, release on terminal Complete (including cancel/owner/shutdown), and discard remaining closures at Stop after silent cancellation/worker accounting. A permit contains only one closure or an empty reservation; its captured finish/exchange uses the existing request-owned buffer account, not a new payload copy. No job/wait/admission is created if this permit is refused; that early ordinary Config refusal may use landed global asynchronous completion and is explicitly outside accepted-persistence drain claims. The independent IO and continuation caps remain8192 each. Tests fill real permit entries and release them, never overwrite counters.

Completed slots are bounded by the8192 admitted callback slots; deferred replay continuations have a separate8192 bound. Byte reservations remain charged through result consumption/discard, including worker-owned DTO boxes; they are not released when a worker merely publishes a result. Queue Load reserves aggregate body allowance,10MiB retained metadata,8MiB bounded listing/index overhead, and one encoded record plus2MiB decoder/candidate workspace; cache Find reserves its maximum returned body plus metadata. Caps can conservatively refuse unusually large custom policy settings; no operation silently exceeds the executor's128MiB charged-payload budget. Removing a completed callback before invoking it frees its callback slot (its byte reservation remains until callback/result disposal) for the mandatory zero-byte commit/abort continuation, so a prepared item does not lose cleanup solely because ordinary admission filled the map. Test this exact full-map transition. Normal request-owned decode buffers after handoff are outside the persistence pending-byte counter; internal parser/copy overhead is bounded by record caps, not claimed as an exact allocator total.

For the isolated shutdown test, parameterize the four private bodies above: rename LaunchPersistence's implementation to `LaunchOn(FPersistenceRuntime& Runtime, FIoWork&& Work, FIoDone&& Done, int64 ReservedBytes, bool bControlDuringStop)`, remove its singleton lookup, capture `&Runtime` in its worker lambda and use that same runtime for result publication. Rename PumpPersistence/StartPersistence/StopPersistence implementations to PumpOn/StartOn/StopOn with `FPersistenceRuntime& Runtime`; their internal calls use that argument, and StartOn's ticker captures the runtime reference. StopOn removes the ticker before the stack runtime can die. The public wrappers are exactly:

```cpp
bool VaRest::Http::LaunchPersistence(FIoWork&& Work,FIoDone&& Done,int64 ReservedBytes,bool bControlDuringStop)
{ return LaunchOn(PersistenceRuntime(),MoveTemp(Work),MoveTemp(Done),ReservedBytes,bControlDuringStop); }
void VaRest::Http::PumpPersistence() { PumpOn(PersistenceRuntime()); }
bool VaRest::Http::PersistenceAccepting() { check(IsInGameThread()); return PersistenceRuntime().bAccepting; }
bool VaRest::Http::PersistenceIdle()
{
    check(IsInGameThread());
    auto& Runtime=PersistenceRuntime();
    FScopeLock Lock(&Runtime.CompletedLock);
    return Runtime.Callbacks.IsEmpty() && Runtime.Completed.IsEmpty() && Runtime.Deferred.IsEmpty() && Runtime.OwnedCompletions.IsEmpty()
        && Runtime.PendingBytes==0 && Runtime.Counters.Launched.load()==Runtime.Counters.Delivered.load();
}
void VaRest::Http::StartPersistence() { StartOn(PersistenceRuntime()); }
void VaRest::Http::StopPersistence() { StopOn(PersistenceRuntime()); }
```

Under WITH_DEV_AUTOMATION_TESTS add this RAII helper declaration in VaRestPersistence.h and definitions after StartOn/StopOn in its cpp. It redirects only GT admissions/counter queries during one isolated, nonconcurrent test. Worker lambdas already capture their own runtime reference; no worker consults TestRuntime. Production singleton/ticker remain untouched and must be idle on entry.

```cpp
// VaRest::Http::Testing declaration
class FScopedPersistenceRuntime
{
public:
    FScopedPersistenceRuntime();
    ~FScopedPersistenceRuntime();
    void Stop();
private:
    void* Owned=nullptr;
};
int64 VaRest::Http::Testing::PersistencePendingBytesForTest()
{ check(IsInGameThread()); return PersistenceRuntime().PendingBytes; }
VaRest::Http::Testing::FScopedPersistenceRuntime::FScopedPersistenceRuntime()
{
    check(IsInGameThread()); check(!TestRuntime); check(PersistenceIdle());
    auto* Created=new FPersistenceRuntime(); Owned=Created; TestRuntime=Created; StartOn(*Created);
}
void VaRest::Http::Testing::FScopedPersistenceRuntime::Stop()
{ check(IsInGameThread()); StopOn(*static_cast<FPersistenceRuntime*>(Owned)); }
VaRest::Http::Testing::FScopedPersistenceRuntime::~FScopedPersistenceRuntime()
{
    Stop(); check(TestRuntime==Owned); TestRuntime=nullptr;
    delete static_cast<FPersistenceRuntime*>(Owned);
}
```

This is a mechanical parameterization, not a second executor implementation. Private runtime type/helpers precede wrappers in the cpp. Add this test helper there and declare its result/function in VaRestPersistence.h under WITH_DEV_AUTOMATION_TESTS:

```cpp
namespace VaRest::Http::Testing
{
    struct FStopProbe
    {
        bool bOrdinaryCalled=false,bCommitCalled=false,bUnrelatedCalledAtStop=false,bAcceptedAfterStop=false,bClean=false;
        TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> Unrelated=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    };
    FStopProbe RunIsolatedStopProbe()
    {
        FStopProbe Result;
        FPersistenceRuntime Runtime;
        StartOn(Runtime);
        UE::Tasks::Launch(TEXT("VaRest.UnrelatedStopWitness"),[Seen=Result.Unrelated]{ Seen->store(true); },
            UE::Tasks::ETaskPriority::Normal,UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
        LaunchOn(Runtime,[]{ FIoResult Done; Done.bOk=true; return Done; },
            [&Result](FIoResult&&){ Result.bOrdinaryCalled=true; },1,false);
        LaunchOn(Runtime,[]{ FIoResult Done; Done.bOk=true; return Done; },
            [&Result,&Runtime](FIoResult&&)
            {
                Result.bCommitCalled=true;
                Result.bAcceptedAfterStop=LaunchOn(Runtime,[]{ return FIoResult(); },[](FIoResult&&){},0,false);
            },1,true);
        StopOn(Runtime);
        Result.bUnrelatedCalledAtStop=Result.Unrelated->load();
        Result.bClean=Runtime.Callbacks.IsEmpty() && Runtime.Completed.IsEmpty() && Runtime.Deferred.IsEmpty()
            && Runtime.PendingBytes==0 && Runtime.Counters.Delivered.load()==2;
        return Result;
    }
}
```

The complete `VaRest.Http.Persistence.IsolatedStop` body:

```cpp
using namespace VaRest::Http::Testing;
const FStopProbe Probe=RunIsolatedStopProbe();
TestFalse(TEXT("ordinary callbacks suppressed"),Probe.bOrdinaryCalled);
TestTrue(TEXT("accepted commit accounted"),Probe.bCommitCalled);
TestFalse(TEXT("unrelated task not pumped by Stop"),Probe.bUnrelatedCalledAtStop);
TestFalse(TEXT("control continuation cannot admit more IO"),Probe.bAcceptedAfterStop);
TestTrue(TEXT("own slots and reservations drained"),Probe.bClean);
TestTrue(TEXT("ordinary test pump eventually runs witness"),PumpUntil([Seen=Probe.Unrelated]{ return Seen->load(); }));
return true;
```

No module/global shutdown flag is changed by this test. A mutation replacing PumpOn with ProcessThreadUntilIdle makes the unrelated-task assertion fail. A mutation discarding the control continuation makes bCommitCalled fail. Restore each hunk before running the full persistence filter.

The Stop path is only module PreExit/Shutdown or a standalone test executor, not a gameplay API. Module PreExit first CancelForShutdown and tells queue coordinators to stop admitting/replaying; it submits cleanup for already-prepared aborted jobs before StopPersistence closes admission, once the queue integration exists. A committed rename already queued runs to completion. Prepared operations whose ordinary handback is discarded may leave .pending; Load ignores and cleans those next startup. Stop makes no stronger deletion promise. No persistence-handback lambda is ever queued to TaskGraph; the removed ticker and drained slots establish unload safety for this executor.

- [ ] **Step 4: Implement the wait and its exact release ordering.**

```cpp
VaRest::Http::FPersistenceWait::FPersistenceWait(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done)
    : HeldExchange(Exchange), Completion(MoveTemp(Done)) {}
TSharedRef<VaRest::Http::FPersistenceWait, ESPMode::ThreadSafe>
VaRest::Http::FPersistenceWait::Begin(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done)
{
    const TSharedRef<FPersistenceWait, ESPMode::ThreadSafe> Wait =
        MakeShareable(new FPersistenceWait(Exchange, MoveTemp(Done)));
    FExchangeState& Ex = Internals(*Exchange);
    if (Ex.PersistenceCompletionTicket==0)
    {
        Ex.PersistenceCompletionTicket=AcquirePersistenceCompletion();
        if (Ex.PersistenceCompletionTicket==0)
        {
            // No wait/interrupt/job is installed. This remains an ordinary asynchronously refused request.
            FVaRestAttemptOutcome Refused; Refused.Failure=EVaRestTransportFailure::Other;
            Refused.FailureDetail=TEXT("PersistenceStopped"); Refused.FailureMessage=TEXT("persistence completion capacity unavailable");
            Wait->FinishUnchecked(MoveTemp(Refused)); return Wait;
        }
    }
    Ex.bPersistenceOwned=true;
    const TWeakPtr<FPersistenceWait, ESPMode::ThreadSafe> Weak(Wait);
    Ex.Interrupt = [Weak]
    {
        if (const auto Pinned = Weak.Pin()) return Pinned->InterruptNow();
        return false;
    };
    Wait->CheckDeadline();
    return Wait;
}
void VaRest::Http::FPersistenceWait::Disarm()
{
    auto& Ex = Internals(*HeldExchange);
    Ex.Config->Clock->ClearTimer(Timer);
    Ex.Interrupt.Reset();
}
FVaRestOutcomeCallback VaRest::Http::FPersistenceWait::TakeCompletion()
{
    check(IsInGameThread());
    if (bEnded) return {};
    bEnded=true; Disarm();
    return MoveTemp(Completion);
}
void VaRest::Http::FPersistenceWait::FinishUnchecked(FVaRestAttemptOutcome&& Outcome)
{
    auto Callback=TakeCompletion();
    if (Callback) Callback(MoveTemp(Outcome));
}
bool VaRest::Http::FPersistenceWait::CanHandoff()
{
    check(IsInGameThread());
    if (bEnded) return false;
    auto& Ex=Internals(*HeldExchange);
    if (IsOwnerGone(Ex)) MarkOwnerGone(Ex);
    if (Ex.bFinished || Ex.bCompletionQueued || Ex.Phase!=EVaRestExchangePhase::InChain || Ex.IsCancelRequested())
    {
        Ex.PersistenceAbort->store(true,std::memory_order_relaxed);
        FinishUnchecked(FVaRestAttemptOutcome::Canceled()); return false;
    }
    if (Ex.DeadlineAt>0 && Ex.Config->Clock->Now()>=Ex.DeadlineAt-DeadlineTimerSlack)
    {
        Ex.PersistenceAbort->store(true,std::memory_order_relaxed);
        FinishUnchecked(MakeDeadlineOutcome()); return false;
    }
    return true;
}
FVaRestOutcomeCallback VaRest::Http::FPersistenceWait::Resume()
{ return CanHandoff() ? TakeCompletion() : FVaRestOutcomeCallback(); }
void VaRest::Http::FPersistenceWait::Finish(FVaRestAttemptOutcome&& Outcome)
{ if (CanHandoff()) FinishUnchecked(MoveTemp(Outcome)); }
bool VaRest::Http::FPersistenceWait::InterruptNow()
{
    if (bEnded) return false;
    Internals(*HeldExchange).PersistenceAbort->store(true,std::memory_order_relaxed);
    FinishUnchecked(FVaRestAttemptOutcome::Canceled()); return true;
}
void VaRest::Http::FPersistenceWait::CheckDeadline()
{
    if (bEnded) return;
    auto& Ex = Internals(*HeldExchange);
    if (Ex.IsCancelRequested()) { InterruptNow(); return; }
    if (Ex.DeadlineAt <= 0.0) return;
    const double Left = Ex.DeadlineAt - Ex.Config->Clock->Now();
    if (Left <= DeadlineTimerSlack) { Ex.PersistenceAbort->store(true,std::memory_order_relaxed); FinishUnchecked(MakeDeadlineOutcome()); return; }
    const TWeakPtr<FPersistenceWait, ESPMode::ThreadSafe> Weak(AsShared());
    Timer = Ex.Config->Clock->SetTimer(Left, [Weak]
    {
        if (const auto Pinned = Weak.Pin()) Pinned->CheckDeadline();
    });
}
```

**R10/R11 pipeline integration in this task:** add `bool bPersistenceOwned=false; uint64 PersistenceCompletionTicket=0;` to FExchangeState (set by Begin and retained until request disposal), `int32 PendingApiCompletionWrappers=0;` to FClientState for the owned shutdown assertion, and include those exact files in this task. In FClientState::Complete replace the ApiDepth condition with:

```cpp
const bool bOwnedSilentShutdown=Ex.bPersistenceOwned && Ex.CancelReason==EVaRestCancelReason::Shutdown;
if (Ex.ApiDepth>0 && !bOwnedSilentShutdown)
{
    Ex.bCompletionQueued=true;
    EndWaits(Ex);
    if (Ex.bPersistenceOwned)
    {
        check(Ex.PersistenceCompletionTicket!=0);
        SetPersistenceCompletion(Ex.PersistenceCompletionTicket,[Exchange,Pending=MoveTemp(Finish)]() mutable
        { Internals(*Exchange).State->Complete(Exchange,MoveTemp(Pending)); });
        return;
    }
    ++PendingApiCompletionWrappers;
    OnGameThread(TEXT("VaRest.Complete"),[Exchange,Pending=MoveTemp(Finish)]() mutable
    {
        const auto Client=Internals(*Exchange).State;
        --Client->PendingApiCompletionWrappers;
        Client->Complete(Exchange,MoveTemp(Pending));
    });
    return;
}
ReleasePersistenceCompletion(Ex.PersistenceCompletionTicket);
Ex.PersistenceCompletionTicket=0;
```

The release lines go after the complete ApiDepth branch and before IsOwnerGone; existing early Decoding/handler checks remain before that branch. Releasing destroys an earlier owned completion closure even when shutdown is the second caller to Complete. The accepted closure never enters TaskGraph and PendingApiCompletionWrappers counts only the ordinary global branch.

The rest of Complete is unchanged: statistics/in-flight removal/Completed occur, then its existing Shutdown-silent branch destroys handlers without user callbacks. Thus CancelForShutdown may settle an owned cache/prepare wait synchronously inside FApiScope without creating a global completion wrapper; ordinary non-persistence callbacks keep their existing global non-inline deferral, while accepted persistence-owned API completions use their reserved owned slot. This narrowly permits internal silent cleanup, never ordinary user delivery. A committed queue admission still returns from Cancel without fabricated completion; its tagged IO acknowledgement invokes the same normal Complete accounting after rename. A persistence wait that is already decoding remains owned by the landed decode drain; the exchange-level stop proof below targets the new cache-read/prepare/commit holds, not an invented general decode shutdown guarantee.

Begin can end synchronously at an already-passed deadline; callers test `IsPending` before any IO admission. The GT callback map owns the wait until handback; Ex.Interrupt owns it weakly, so no exchange/state cycle. Cancellation releases its Completion promptly but worker-owned bytes may remain until IO finishes; tests assert bounded admission bytes, not instantaneous worker cancellation.

- [ ] **Step 5: Extend FTestRig::Drain.** Its Started sum includes PersistenceCounters.Launched. Settled requires HandedBack==Launched; after pumping GT require Delivered==Launched and a second unchanged Started sample plus PersistenceIdle() before returning. A deferred replay can exist without launching IO yet, so equal worker counters alone are insufficient. This is process-wide IO quiescence because the executor is global; per-client stats comparisons remain per-client. Add `FScopedPersistenceTest` in Public/VaRestHttpTesting.h with `Root=ProjectSavedDir()/"VaRestTests"/NewGuid`, root-directory writes/deletes through LaunchPersistence, a `Drain()` using the same counter loop, and destructor scheduling recursive deletion only after drain. Tests that timeout release barriers and return a failure, not block forever. Stop tests use a standalone executor instance instead of stopping the live module's queue.
The test utility's complete declaration goes in Public/VaRestHttpTesting.h inside WITH_DEV_AUTOMATION_TESTS, because the Integration module also consumes it. Its definitions stay in VaRestHttpTesting.cpp:

```cpp
// namespace VaRest::Http::Testing
struct VARESTHTTP_API FScopedPersistenceTest
{
    FString Root;
    FScopedPersistenceTest();
    ~FScopedPersistenceTest();
    bool Drain(double TimeoutSeconds=5.0) const;
};
VaRest::Http::Testing::FScopedPersistenceTest::FScopedPersistenceTest()
    : Root(FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("VaRestTests"),FGuid::NewGuid().ToString(EGuidFormats::DigitsLower))) {}
bool VaRest::Http::Testing::FScopedPersistenceTest::Drain(double TimeoutSeconds) const
{
    const double Deadline=FPlatformTime::Seconds()+TimeoutSeconds;
    for (;;)
    {
        if (FPlatformTime::Seconds()>=Deadline) return false;
        if (!PumpUntil([]{ const auto& Counts=PersistenceCounters();
            return Counts.Launched.load()==Counts.HandedBack.load(); },FMath::Max(0.0,Deadline-FPlatformTime::Seconds()))) return false;
        const uint64 Before=PersistenceCounters().Launched.load();
        PumpPersistence();
        FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
        if (Before==PersistenceCounters().Launched.load() && Before==PersistenceCounters().Delivered.load() && PersistenceIdle()) return true;
    }
}
VaRest::Http::Testing::FScopedPersistenceTest::~FScopedPersistenceTest()
{
    // Every barrier guard is declared after this fixture and therefore releases before this destructor.
    if (!Drain()) { ensureMsgf(false,TEXT("persistence test did not drain")); return; }
    LaunchPersistence([Directory=Root]
    {
        FIoResult Result;
        Result.bOk=FPlatformFileManager::Get().GetPlatformFile().DeleteDirectoryRecursively(*Directory);
        return Result;
    },[](FIoResult&&){});
    ensure(Drain());
}
```

In Testing::PumpUntil add `VaRest::Http::PumpPersistence();` before its existing ProcessThreadUntilIdle call. This changes only the test pump, never production shutdown. FTestRig::Drain's exact lambda replacements/additions are:

```cpp
const auto Started=[this]
{
    return static_cast<int64>(Fake->NumCompletionsQueued())+Counters().DecodeTasksLaunched.load()
        +static_cast<int64>(PersistenceCounters().Launched.load());
};
const auto Settled=[this]
{
    return Fake->NumCompletionsRun()==Fake->NumCompletionsQueued()
        && Counters().DecodeTasksHandedBack.load()==Counters().DecodeTasksLaunched.load()
        && PersistenceCounters().HandedBack.load()==PersistenceCounters().Launched.load();
};
// Existing final if, after ProcessThreadUntilIdle:
if (Started()==Before && PersistenceCounters().Delivered.load()==PersistenceCounters().Launched.load() && PersistenceIdle()) return true;
```

The deterministic `VaRest.Http.Persistence.WorkerOrdering` body is:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
const FString Path=FPaths::Combine(Disk.Root,TEXT("ordered.json"));
struct FObserved { TArray<int32> Order; TArray<bool> Ok; TArray<uint8> Read; };
const auto Seen=MakeShared<FObserved,ESPMode::ThreadSafe>();
const uint64 Before=PersistenceCounters().OperationsOnGameThread.load();
LaunchPersistence([Path]{ return WriteNewFile(Path,TArray<uint8>{97}); },[Seen](FIoResult&& Result){ Seen->Order.Add(1); Seen->Ok.Add(Result.bOk); });
LaunchPersistence([Path]{ return ReadBoundedFile(Path,1); },[Seen](FIoResult&& Result){ Seen->Order.Add(2); Seen->Ok.Add(Result.bOk); Seen->Read=MoveTemp(Result.Bytes); });
LaunchPersistence([Path]{ return VaRest::Http::DeleteFile(Path); },[Seen](FIoResult&& Result){ Seen->Order.Add(3); Seen->Ok.Add(Result.bOk); });
LaunchPersistence([Path]{ return ReadBoundedFile(Path,1); },[Seen](FIoResult&& Result){ Seen->Order.Add(4); Seen->Ok.Add(Result.bOk); });
if (!TestTrue(TEXT("drain"),Disk.Drain())) return false;
TestTrue(TEXT("serial callback order"),Seen->Order==TArray<int32>({1,2,3,4}));
TestTrue(TEXT("write read delete missing"),Seen->Ok==TArray<bool>({true,true,true,false}));
TestTrue(TEXT("exact bytes"),Seen->Read==TArray<uint8>({97}));
TestEqual(TEXT("no disk operation on GT"),PersistenceCounters().OperationsOnGameThread.load(),Before);
return true;
```

The order assertion requires GT handbacks to be published in the same worker FIFO sequence, which the pipe establishes. Test admission refusals explicitly by setting a test executor's PendingBytes to its cap; the caller must receive false and not wait. Production executor state is never overwritten by a test: parameterize the runtime object used by the private submission implementation, while production uses the process singleton and the scoped test override owns an independent runtime.

- [ ] **Step 6: Run green and mutate.** Run the persistence filter, then cancellation and retry filters. Remove Interrupt installation and observe Outer-probe/deadline test failure; restore. Delay worker handback after counter increment in the mutant and observe Drain ordering test failure; restore. `OperationsOnGameThread` stays unchanged for all production IO operations.
- [ ] **Step 7: Commit:** `git add Source/VaRestHttp/Private/VaRestPersistence.h Source/VaRestHttp/Private/VaRestPersistence.cpp Source/VaRestHttp/Private/VaRestPersistenceWait.h Source/VaRestHttp/Private/VaRestPersistenceWait.cpp Source/VaRestHttp/Private/Tests/VaRestHttpPersistenceTest.cpp Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestHttpTesting.cpp Source/VaRestHttp/Private/VaRestHttpModule.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestPipeline.cpp && git commit -m "feat(http): add serialized asynchronous persistence and bounded waits"`.

### Task 5: Byte-accounted memory and disk cache stores

**Files:** Create `Private/VaRestMemoryCache.h/.cpp`, `Private/VaRestDiskCache.h/.cpp`, `Private/Tests/VaRestHttpMemoryCacheTest.cpp`, `Private/Tests/VaRestHttpDiskCacheTest.cpp`; modify `Public/VaRestStage.h`, `Private/VaRestHttpModule.cpp`.

**Interfaces:** Consumes Task 2 metadata, Task 3 guard and Task 4 persistence. Produces:

```cpp
// VaRestMemoryCache.h, namespace VaRest::Http
struct FCacheEntry
{
    FString Key; // 64 lowercase hexadecimal bytes, never request material
    FVaRestHeaders Headers;
    FString ContentType;
    TArray<uint8> Body;
    FCacheMetadata Metadata;
    uint64 LastUse = 0;
    int64 Charge() const;
    FVaRestAttemptOutcome Answer() const;
};
class FMemoryCache
{
public:
    TSharedPtr<const FCacheEntry, ESPMode::ThreadSafe> Find(const FString& Key);
    void Put(FCacheEntry Entry, int64 Capacity);
    void Remove(const FString& Key);
    void Clear();
    int64 Bytes() const { return UsedBytes; }
    int32 Num() const { return Entries.Num(); }
private:
    TMap<FString,TSharedRef<FCacheEntry,ESPMode::ThreadSafe>> Entries;
    int64 UsedBytes = 0;
    uint64 Sequence = 0;
};
// VaRestDiskCache.h, namespace VaRest::Http
using FCacheReadDone = TUniqueFunction<void(TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>)>;
class FDiskCache : public TSharedFromThis<FDiskCache,ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FDiskCache,ESPMode::ThreadSafe> Open(FString Root, int64 Capacity);
    static void StopControls();
    void Find(FString Key, double Now, FDateTime Utc, FCacheReadDone&& Done);
    void Put(FCacheEntry Entry, FPersistenceGuard Guard);
    void Remove(FString Key);
    void Clear();
    uint64 GetEpoch() const { return Epoch; }
    bool IsQuarantined() const;
private:
    void RequestMaintenance(bool bClear);
    void TryMaintenance();
    bool bMaintenancePending=true,bMaintenanceInFlight=false,bClearRequired=false;
    uint64 ControlSerial=1,WorkerAppliedControlSerial=0; // worker field is pipe-only
    FDiskCache(FString InRoot, int64 InCapacity);
    FString Directory;
    int64 CapacityBytes = 0; // GT requested monotone minimum
    int64 WorkerCapacityBytes = 0; // touched only inside the serialized pipe after construction
    uint64 Epoch = 1;
};
TArray<uint8> EncodeCacheRecord(const FCacheEntry& Entry);
TOptional<FCacheEntry> DecodeCacheRecord(TConstArrayView<uint8> Bytes, FStringView ExpectedKey,
    double Now, FDateTime Utc, int64 MaximumBodyBytes);
```

Private disk state lives solely in the serialized executor, keyed by canonical Directory; GT FDiskCache never reaches worker maps. `Open` schedules a scan, and later jobs are ordered behind it. Root is configurable privately for tests and publicly through `WithStorageDirectories` in Task 6, with defaults in ProjectSavedDir. Cache disk namespace is `HashFields("VaRest/cache-origin/v1", {canonical base origin})`; same origin clients may share disk but memory remains client-owned. Shared root capacity is the monotone minimum requested during one store lifetime. Mandatory maintenance has a coalesced sticky quarantine: no reads/writes are reused until a worker applies it successfully. Refusal/IO failure retains the bounded intent across client release and is retried on later access. Remove conservatively clears the namespace. At most4096 dirty namespaces are retained; exhaustion permanently disables disk reuse in this process (requests still use memory/transport), preventing forgotten invalidations from resurfacing. This is a finite availability fallback, not a power-loss/durable Clear acknowledgement. A smaller registration queues worker capacity reduction and eviction; removing it does not increase capacity. Only complete release of clients, operations and workers followed by reopening permits a larger cap. A reduction may temporarily coexist with already-admitted bytes until its serialized eviction runs; the single bounded in-flight write has the documented temp allowance. Changing directory creates a new store; outstanding Send snapshots retain their old store.

- [ ] **Step 1: Add `VaRest.Http.CacheStore.MemoryLru`.**

```cpp
using namespace VaRest::Http;
FMemoryCache Cache;
FCacheEntry A; A.Key = TEXT("a"); A.Body = {1,2,3};
FCacheEntry B; B.Key = TEXT("b"); B.Body = {4,5,6};
FCacheEntry C; C.Key = TEXT("c"); C.Body = {7,8,9};
const int64 One = A.Charge();
Cache.Put(A, One * 2);
Cache.Put(B, One * 2);
TestTrue(TEXT("A touch"), Cache.Find(TEXT("a")).IsValid());
Cache.Put(C, One * 2);
TestFalse(TEXT("least recently used B evicted"), Cache.Find(TEXT("b")).IsValid());
const auto Found = Cache.Find(TEXT("a"));
if (!TestTrue(TEXT("A survives"), Found.IsValid())) return false;
auto Response = Found->Answer();
Response.Body[0] = 99;
TestEqual(TEXT("answer bytes are independent"), Found->Body[0], static_cast<uint8>(1));
TestTrue(TEXT("charge bounded"), Cache.Bytes() <= One * 2);
Cache.Clear();
TestEqual(TEXT("clear count"), Cache.Num(), 0);
TestEqual(TEXT("clear charge"), Cache.Bytes(), static_cast<int64>(0));
return true;
```

`VaRest.Http.CacheStore.DiskRecovery`: write two version-1 records through executor, force eviction with actual encoded-size budget, reopen a fresh store and verify the recently touched one survives; corrupt body digest/truncate JSON/invalid version/path-like key/oversized base64 record and verify miss+delete. Include a file symlink pointing outside the test root and assert it is ignored, not read or removed through its target.

Register `VaRest.Http.CacheStore.CorruptMetadata` with this complete bounded decoder matrix:

```cpp
using namespace VaRest::Http;
FCacheEntry Entry;
Entry.Key=HashFields(TEXT("test/cache"),TArray<FString>{TEXT("entry")});
Entry.ContentType=TEXT("text/plain"); Entry.Body={97,98,99};
Entry.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
const FDateTime Utc(2026,10,23);
Entry.Metadata=ReadCacheMetadata(Entry.Headers,0,10,Utc);
const auto Bytes=EncodeCacheRecord(Entry);
const auto Good=DecodeCacheRecord(Bytes,Entry.Key,20,Utc,1024);
if (!TestTrue(TEXT("valid baseline"),Good.IsSet())) return false;
TestTrue(TEXT("body survived"),Good->Body==Entry.Body);
const auto Parsed=FVaRestJson::Parse(Bytes);
if (!TestTrue(TEXT("record parses"),Parsed.HasValue())) return false;
for (const TCHAR* Corruption : {TEXT("version"),TEXT("bodySha256"),TEXT("key"),TEXT("noCache"),TEXT("lifetime"),TEXT("headers")})
{
    auto Bad=Parsed.GetValue().Clone();
    if (FCString::Strcmp(Corruption,TEXT("version"))==0) Bad.Set(Corruption,99);
    else if (FCString::Strcmp(Corruption,TEXT("bodySha256"))==0) Bad.Set(Corruption,FString::ChrN(64,TEXT('0')));
    else if (FCString::Strcmp(Corruption,TEXT("key"))==0) Bad.Set(Corruption,TEXT("../escape"));
    else if (FCString::Strcmp(Corruption,TEXT("noCache"))==0) Bad.Set(Corruption,TEXT("false"));
    else if (FCString::Strcmp(Corruption,TEXT("lifetime"))==0) Bad.Set(Corruption,-1);
    else Bad.Set(Corruption,FVaRestJson::Array(FVaRestJson::Array(TEXT("Set-Cookie"),TEXT("private"))));
    TArray<uint8> Encoded;
    if (!TestTrue(TEXT("mutant encodes"),Bad.TryToUtf8(Encoded).HasValue())) return false;
    TestFalse(Corruption,DecodeCacheRecord(Encoded,Entry.Key,20,Utc,1024).IsSet());
}
TArray<uint8> Truncated=Bytes; Truncated.SetNum(Truncated.Num()/2);
TestFalse(TEXT("truncated record"),DecodeCacheRecord(Truncated,Entry.Key,20,Utc,1024).IsSet());
TestFalse(TEXT("body cap enforced"),DecodeCacheRecord(Bytes,Entry.Key,20,Utc,2).IsSet());
Entry.Metadata.StoredUtc=Utc+FTimespan::FromDays(1);
const auto Future=DecodeCacheRecord(EncodeCacheRecord(Entry),Entry.Key,20,Utc,1024);
if (!TestTrue(TEXT("future record decoded conservatively"),Future.IsSet())) return false;
TestFalse(TEXT("clock rollback cannot extend freshness"),Future->Metadata.IsFresh(20));
return true;
```

Register `VaRest.Http.CacheStore.SharedCapacityLifetime` with this body:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
const FString Root=FPaths::Combine(Disk.Root,TEXT("Store"));
FCacheEntry Entry; Entry.Key=HashFields(TEXT("capacity-test"),TArray<FString>{TEXT("entry")});
Entry.Body={1,2,3}; Entry.ContentType=TEXT("application/octet-stream");
Entry.Metadata.StoredUtc=FDateTime::UtcNow(); Entry.Metadata.Lifetime=60;
TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
Store->Put(Entry,FPersistenceGuard());
if (!TestTrue(TEXT("initial write"),Disk.Drain())) return false;
TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Smaller=FDiskCache::Open(Root,1);
if (!TestTrue(TEXT("serialized capacity reduction"),Disk.Drain())) return false;
Smaller.Reset();
TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Larger=FDiskCache::Open(Root,1024*1024);
TestTrue(TEXT("same live store"),Larger==Store);
Store->Put(Entry,FPersistenceGuard());
const auto Found=MakeShared<bool,ESPMode::ThreadSafe>(true);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Found](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Found=Value.IsValid(); });
if (!TestTrue(TEXT("smaller cap still enforced"),Disk.Drain())) return false;
TestFalse(TEXT("releasing smaller registrant does not recover capacity"),*Found);
Store.Reset(); Larger.Reset();
if (!TestTrue(TEXT("all old workers released"),Disk.Drain())) return false;
Store=FDiskCache::Open(Root,1024*1024);
Store->Put(Entry,FPersistenceGuard());
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Found](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Found=Value.IsValid(); });
if (!TestTrue(TEXT("fresh lifetime complete"),Disk.Drain())) return false;
TestTrue(TEXT("full reopen permits larger cap"),*Found);
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.CacheStore`.
- [ ] **Step 3: Implement the memory LRU.**

```cpp
int64 VaRest::Http::FCacheEntry::Charge() const
{
    int64 Count = 1024 + Body.Num() + (Key.Len() + ContentType.Len() + Metadata.ETag.Len() + Metadata.LastModified.Len()) * sizeof(TCHAR);
    for (const auto& Pair : Headers) Count += (Pair.Key.Len() + Pair.Value.Len()) * sizeof(TCHAR) + 64;
    for (const FString& Name : Metadata.Vary) Count += Name.Len() * sizeof(TCHAR) + 32;
    return Count;
}
FVaRestAttemptOutcome VaRest::Http::FCacheEntry::Answer() const
{
    FVaRestAttemptOutcome Result;
    Result.Status = 200;
    Result.Headers = Headers;
    Result.ContentType = ContentType;
    Result.Body = Body;
    Result.Body.Reserve(Result.Body.Num() + 1);
    Result.bFromCache = true; // added to outcome in this task
    return Result;
}
TSharedPtr<const VaRest::Http::FCacheEntry,ESPMode::ThreadSafe>
VaRest::Http::FMemoryCache::Find(const FString& Key)
{
    check(IsInGameThread());
    if (auto* Found = Entries.Find(Key)) { (*Found)->LastUse = ++Sequence; return *Found; }
    return nullptr;
}
void VaRest::Http::FMemoryCache::Remove(const FString& Key)
{
    check(IsInGameThread());
    if (const auto* Found = Entries.Find(Key)) UsedBytes -= (*Found)->Charge();
    Entries.Remove(Key);
}
void VaRest::Http::FMemoryCache::Clear() { check(IsInGameThread()); Entries.Reset(); UsedBytes = 0; }
void VaRest::Http::FMemoryCache::Put(FCacheEntry Entry, int64 Capacity)
{
    check(IsInGameThread());
    Remove(Entry.Key);
    const int64 Cost = Entry.Charge();
    if (Capacity <= 0 || Cost > Capacity) return;
    while (UsedBytes > Capacity - Cost || Entries.Num() >= 4096)
    {
        FString Oldest;
        uint64 OldestUse = MAX_uint64;
        for (const auto& Pair : Entries)
        {
            if (Pair.Value->LastUse < OldestUse) { Oldest = Pair.Key; OldestUse = Pair.Value->LastUse; }
        }
        if (Oldest.IsEmpty()) break;
        Remove(Oldest);
    }
    Entry.LastUse = ++Sequence;
    UsedBytes += Cost;
    const FString StoredKey = Entry.Key;
    Entries.Add(StoredKey, MakeShared<FCacheEntry,ESPMode::ThreadSafe>(MoveTemp(Entry)));
}
```

MemoryBytes is an admission charge covering body and conservative metadata overhead, not an exact allocator/RSS ceiling. Count cap covers empty-body floods. `Answer` must not set BytesDown to cached body size: network accounting is zero for cache hits.

- [ ] **Step 4: Implement the disk record and recovery.** Schema:

```json
{"version":1,"key":"64lowerhex","generation":"GUID32","storedUtc":"2026-10-23T12:00:00Z","usedUtc":"2026-10-23T12:00:00Z","lifetime":60,"initialAge":0,"noCache":false,"mustRevalidate":false,"contentType":"application/json","headers":[["ETag","\"v1\""]],"body":"e30=","bodySha256":"44136fa355b3678a1146ad16f7e8649e94fb4fc21fe77e8310c060f61caaff8a"}
```

Filename `<key>.<generation>.json`; never load a filename referenced inside JSON. Encode with VaRestJson and `FBase64::Encode`, body digest with FSha256; header pairs as arrays preserve repeats. Serialize metadata/bytes in the worker, after guard acceptance, with `TryToUtf8`. Decode requires exact typed fields and version, key equals ExpectedKey, valid GUID/time, finite lifetime/age within 10 years, ≤128 header pairs/64 KiB total header characters, valid HTTP header name/value, allowed content type length ≤1024, body base64 decoded length ≤policy capacity and ≤32 MiB, digest exact. Reject no-store metadata. Reconstruct freshness at process start as `StoredAt=Now`, `InitialAge=persistedAge + max(0,Utc-storedUtc)`; when storedUtc is in the future, force lifetime=0 (do not grant time after a rollback). Store body raw as TArray, never a JSON DOM retained in the cache.

Scan accepts only the strict filename grammar and direct regular files. Remove abandoned `.part` files; retain bounded path/size/mtime index entries, validate complete records when Find reads them, and delete corrupt/superseded records there. When interrupted cleanup leaves multiple valid generations, Find tries most recently touched first (filename breaks ties); cached data remains governed by its own freshness and identity. Enforce actual encoded-byte disk capacity and 4096-record cap using `usedUtc` as LRU order; equal time ties use filename. Read touches its record's OS mtime; use that for restart access order. usedUtc is validated metadata and is not a second authoritative index. Never parse all bodies into memory during scan: bounded records processed one at a time and retain only index `{key,path,size,lastUse}`. `Find` schedules scan/read in pipe, then hands immutable entry to GT, where the captured Epoch must still equal current Epoch. Read failure deletes bad record and reports null, not a user request failure.

`Put` first checks guard, builds one unique immutable file, writes/flushes/renames, publishes the worker index only on success, then retires older generations. Before write reserve actual encoded bytes plus temporary-file occupancy; eviction happens before admission, and if one complete record exceeds CapacityBytes it is not written. Peak storage may include one existing record plus its replacement temp; bound this with an explicit `WorkingBytes <= CapacityBytes + MaxRecordBytes` allowance, reported separately from the retained LRU cap. `Clear` increments GT epoch immediately and enqueues deletion before later puts; late reads miss, and old Put handbacks cannot repopulate GT memory.

Add these complete record encode/decode bodies in VaRestDiskCache.cpp. Add `FGuid Generation=FGuid::NewGuid();` to FCacheEntry so record and filename agree. Includes: VaRestJson.h, Misc/Base64.h, VaRestSha256.h, VaRestUrl.h, VaRestPersistenceGuard.h.

```cpp
namespace VaRest::Http::DiskCachePrivate
{
    bool LowerHex(FStringView Text,int32 Length)
    {
        if (Text.Len()!=Length) return false;
        for (TCHAR Character : Text)
            if (!((Character>=TEXT('0') && Character<=TEXT('9')) || (Character>=TEXT('a') && Character<=TEXT('f')))) return false;
        return true;
    }
    bool HeadersFromJson(const FVaRestJson& Array,FVaRestHeaders& Out)
    {
        if (!Array.IsArray() || Array.Num()>128) return false;
        int64 Characters=0;
        const FVaRestRedactRules Rules;
        for (const auto& Pair : Array.Elements())
        {
            if (!Pair.IsArray() || Pair.Num()!=2 || !Pair[0].IsString() || !Pair[1].IsString()) return false;
            const FString Name=Pair[0].AsString(),Value=Pair[1].AsString();
            Characters+=Name.Len()+Value.Len();
            if (Characters>65536 || !IsValidHeaderName(Name) || !IsValidHeaderValue(Value)
                || Rules.ShouldRedact(Name,EVaRestRedactKeyKind::Header)) return false;
            Out.Add(Name,Value);
        }
        return true;
    }
}
TArray<uint8> VaRest::Http::EncodeCacheRecord(const FCacheEntry& Entry)
{
    FVaRestJson Root=FVaRestJson::Object();
    Root.Set(TEXT("version"),1);
    Root.Set(TEXT("key"),Entry.Key);
    Root.Set(TEXT("generation"),Entry.Generation.ToString(EGuidFormats::DigitsLower));
    Root.Set(TEXT("storedUtc"),Entry.Metadata.StoredUtc.ToIso8601());
    Root.Set(TEXT("usedUtc"),Entry.Metadata.StoredUtc.ToIso8601());
    Root.Set(TEXT("lifetime"),Entry.Metadata.Lifetime);
    Root.Set(TEXT("initialAge"),Entry.Metadata.InitialAge);
    Root.Set(TEXT("noCache"),Entry.Metadata.bNoCache);
    Root.Set(TEXT("mustRevalidate"),Entry.Metadata.bMustRevalidate);
    Root.Set(TEXT("contentType"),Entry.ContentType);
    Root.Set(TEXT("body"),FBase64::Encode(Entry.Body));
    Root.Set(TEXT("bodySha256"),FSha256::Hash(Entry.Body).ToHex());
    FVaRestJson Headers=FVaRestJson::Array();
    for (const auto& Pair : Entry.Headers) Headers.Add(FVaRestJson::Array(Pair.Key,Pair.Value));
    Root.Set(TEXT("headers"),Headers);
    TArray<uint8> Bytes;
    const auto Written=Root.TryToUtf8(Bytes);
    return Written.HasValue() ? MoveTemp(Bytes) : TArray<uint8>();
}
TOptional<VaRest::Http::FCacheEntry> VaRest::Http::DecodeCacheRecord(TConstArrayView<uint8> Bytes,
    FStringView ExpectedKey,double Now,FDateTime Utc,int64 MaximumBodyBytes)
{
    FVaRestJsonParseOptions Options;
    Options.MaxInputBytes=FMath::Min<int64>(MAX_int32,((MaximumBodyBytes+2)/3)*4+262144);
    auto Parsed=FVaRestJson::Parse(Bytes,Options);
    if (Parsed.HasError()) return {};
    const FVaRestJson Root=Parsed.StealValue();
    if (!Root.IsObject() || !Root[TEXT("version")].IsNumber() || Root[TEXT("version")].AsDouble()!=1.0) return {};
    for (const TCHAR* Name : {TEXT("key"),TEXT("generation"),TEXT("storedUtc"),TEXT("usedUtc"),
        TEXT("contentType"),TEXT("body"),TEXT("bodySha256")})
        if (!Root[Name].IsString()) return {};
    if (!Root[TEXT("noCache")].IsBool() || !Root[TEXT("mustRevalidate")].IsBool()) return {};
    const auto Lifetime=Root[TEXT("lifetime")].TryGet<double>();
    const auto Age=Root[TEXT("initialAge")].TryGet<double>();
    if (!Lifetime.IsSet() || !Age.IsSet() || !FMath::IsFinite(*Lifetime) || !FMath::IsFinite(*Age)
        || *Lifetime<0 || *Lifetime>315360000 || *Age<0 || *Age>315360000) return {};
    FCacheEntry Entry;
    Entry.Key=Root[TEXT("key")].AsString();
    if (!DiskCachePrivate::LowerHex(Entry.Key,64) || Entry.Key!=ExpectedKey) return {};
    const FString GenerationText=Root[TEXT("generation")].AsString();
    if (GenerationText.Len()!=32 || !FGuid::ParseExact(GenerationText,EGuidFormats::Digits,Entry.Generation) || !Entry.Generation.IsValid()) return {};
    FDateTime Stored,Used;
    if (!FDateTime::ParseIso8601(*Root[TEXT("storedUtc")].AsString(),Stored)
        || !FDateTime::ParseIso8601(*Root[TEXT("usedUtc")].AsString(),Used)) return {};
    Entry.ContentType=Root[TEXT("contentType")].AsString();
    if (Entry.ContentType.Len()>1024 || !IsValidHeaderValue(Entry.ContentType)
        || !DiskCachePrivate::HeadersFromJson(Root[TEXT("headers")],Entry.Headers)) return {};
    const FString Base64=Root[TEXT("body")].AsString();
    if (Base64.Len()>((MaximumBodyBytes+2)/3)*4 || !FBase64::Decode(Base64,Entry.Body)
        || Entry.Body.Num()>MaximumBodyBytes) return {};
    if (FSha256::Hash(Entry.Body).ToHex()!=Root[TEXT("bodySha256")].AsString()) return {};
    Entry.Metadata=ReadCacheMetadata(Entry.Headers,0,Now,Utc);
    if (Entry.Metadata.bNoStore) return {};
    Entry.Metadata.Lifetime=Stored>Utc ? 0.0 : *Lifetime;
    Entry.Metadata.InitialAge=*Age+FMath::Max(0.0,(Utc-Stored).GetTotalSeconds());
    Entry.Metadata.bNoCache|=Root[TEXT("noCache")].AsBool();
    Entry.Metadata.bMustRevalidate|=Root[TEXT("mustRevalidate")].AsBool();
    Entry.Metadata.StoredAt=Now;
    Entry.Metadata.StoredUtc=Utc;
    return MoveTemp(Entry);
}
```

The initial disk index implementation deliberately rebuilds its **bounded** worker-local index on each operation rather than sharing mutable index state across GT/worker. Startup Open queues one scan/recovery; each subsequent operation scans at most 4096 retained files plus bounded transients. This is an implementation of the rebuildable index, not a second source of truth. M7 measures the cost; an incremental worker index can optimize it later with the same store interface. Use these private helpers:

```cpp
namespace VaRest::Http::DiskCachePrivate
{
    struct FRecord { FString Key,Path; int64 Size=0; FDateTime Accessed; };
    TArray<FRecord> Scan(const FString& Directory)
    {
        TArray<FRecord> Records;
        const FIoResult Listing=ListFiles(Directory);
        if (!Listing.bOk) return Records;
        IPlatformFile& Platform=FPlatformFileManager::Get().GetPlatformFile();
        for (const FString& Name : Listing.Names)
        {
            const FString Path=FPaths::Combine(Directory,Name);
            if (Name.EndsWith(TEXT(".part"))) { VaRest::Http::DeleteFile(Path); continue; }
            if (Name.Len()!=102 || Name.Mid(64,1)!=TEXT(".") || !Name.EndsWith(TEXT(".json"))) continue;
            const FString Key=Name.Left(64),Generation=Name.Mid(65,32);
            if (!LowerHex(Key,64) || !LowerHex(Generation,32)) continue;
            FRecord Record; Record.Key=Key; Record.Path=Path;
            Record.Size=Platform.FileSize(*Path); Record.Accessed=Platform.GetTimeStamp(*Path);
            if (Record.Size<0) continue;
            Records.Add(MoveTemp(Record));
        }
        Records.Sort([](const FRecord& A,const FRecord& B)
        { return A.Accessed==B.Accessed ? A.Path<B.Path : A.Accessed<B.Accessed; });
        return Records;
    }
    void Trim(TArray<FRecord>& Records,int64 Capacity,int64 Incoming=0)
    {
        int64 Bytes=0; for (const auto& Record : Records) Bytes+=Record.Size;
        while (!Records.IsEmpty() && (Bytes>Capacity-Incoming || Records.Num()>=4096))
        {
            if (!VaRest::Http::DeleteFile(Records[0].Path).bOk) break;
            Bytes-=Records[0].Size; Records.RemoveAt(0);
        }
    }
    TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Read(const FString& Directory,
        const FString& Key,double Now,FDateTime Utc,int64 Capacity)
    {
        auto Records=Scan(Directory);
        Trim(Records,Capacity);
        for (int32 Index=Records.Num()-1;Index>=0;--Index)
        {
            if (Records[Index].Key!=Key) continue;
            const auto Loaded=ReadBoundedFile(Records[Index].Path,FMath::Min<int64>(Capacity,48*1024*1024));
            auto Entry=Loaded.bOk ? DecodeCacheRecord(Loaded.Bytes,Key,Now,Utc,FMath::Min<int64>(Capacity,32*1024*1024)) : TOptional<FCacheEntry>();
            if (!Entry.IsSet()) { VaRest::Http::DeleteFile(Records[Index].Path); continue; }
            if (Records[Index].Path!=FPaths::Combine(Directory,Key+TEXT(".")+Entry->Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json")))
            { VaRest::Http::DeleteFile(Records[Index].Path); continue; }
            FPlatformFileManager::Get().GetPlatformFile().SetTimeStamp(*Records[Index].Path,Utc);
            for (const auto& Other : Records)
                if (Other.Key==Key && Other.Path!=Records[Index].Path) VaRest::Http::DeleteFile(Other.Path);
            return MakeShared<FCacheEntry,ESPMode::ThreadSafe>(MoveTemp(*Entry));
        }
        return nullptr;
    }
    void RemoveKey(const FString& Directory,const FString& Key)
    {
        for (const auto& Record : Scan(Directory)) if (Record.Key==Key) VaRest::Http::DeleteFile(Record.Path);
    }
}
```

The filename length is102 =64+1+32+5. On a successful read, schedule deletion of older same-key generations in the same pipe job after choosing a valid newest record. Before reporting a write success, check Trim actually freed sufficient capacity; deletion failure skips the write, never exceeds the configured retained cap.

The GT wrappers capture immutable payloads plus the native disk-store lifetime. Its Directory is immutable, WorkerCapacityBytes is pipe-only, and Epoch/CapacityBytes are GT-only; no worker touches an engine/client/exchange object. Its destructor is thread-neutral. Worker retention keeps the registry entry alive until all old IO finishes, so reopening cannot bypass a previous capacity reduction. GT return ownership is weak:

```cpp
namespace VaRest::Http::DiskCachePrivate
{
    struct FControls
    {
        TMap<FString,TWeakPtr<FDiskCache,ESPMode::ThreadSafe>> Registry;
        TMap<FString,TSharedPtr<FDiskCache,ESPMode::ThreadSafe>> Dirty;
        bool bExhausted=false;
    };
#if WITH_DEV_AUTOMATION_TESTS
    FControls* TestControls=nullptr;
#endif
    FControls& Controls()
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (TestControls) return *TestControls;
#endif
        static auto* Instance=new FControls(); return *Instance;
    }
    TMap<FString,TWeakPtr<FDiskCache,ESPMode::ThreadSafe>>& Registry() { return Controls().Registry; }
    TMap<FString,TSharedPtr<FDiskCache,ESPMode::ThreadSafe>>& Dirty() { return Controls().Dirty; }
    bool& ControlsExhausted() { return Controls().bExhausted; } // GT only, sticky for this process lifetime
    bool RetainIntent(const FString& Root,const TSharedRef<FDiskCache,ESPMode::ThreadSafe>& Store)
    {
        if (Root.Len()>4096 || Root.GetAllocatedSize()>8192*sizeof(TCHAR) || (!Dirty().Contains(Root) && Dirty().Num()>=4096)) { ControlsExhausted()=true; return false; }
        Dirty().Add(Root,Store); return true;
    }
    FIoResult Maintain(const FString& Root,int64 Capacity,bool bClear)
    {
        // A failed listing must never be confused with an empty successful namespace.
        FIoResult Result=ListFiles(Root);
        if (!Result.bOk) return Result;
        if (bClear)
        {
            for (const FString& Name : Result.Names)
            {
                const bool bRecord=Name.Len()==102 && Name.Mid(64,1)==TEXT(".")
                    && LowerHex(Name.Left(64),64) && LowerHex(Name.Mid(65,32),32) && Name.EndsWith(TEXT(".json"));
                if ((bRecord || Name.EndsWith(TEXT(".part"))) && !VaRest::Http::DeleteFile(FPaths::Combine(Root,Name)).bOk)
                    Result.bOk=false;
            }
        }
        else
        {
            auto Records=Scan(Root); Trim(Records,Capacity);
            int64 Remaining=0; for (const auto& Record : Records) Remaining+=Record.Size;
            Result.bOk=Remaining<=Capacity && Records.Num()<4096;
        }
        Result.Names.Reset(); return Result;
    }
}
void VaRest::Http::FDiskCache::StopControls()
{
    check(IsInGameThread()); check(!PersistenceAccepting());
    DiskCachePrivate::ControlsExhausted()=true;
    DiskCachePrivate::Dirty().Reset(); DiskCachePrivate::Registry().Reset();
}
VaRest::Http::FDiskCache::FDiskCache(FString InRoot,int64 InCapacity)
    : Directory(MoveTemp(InRoot)),CapacityBytes(InCapacity),WorkerCapacityBytes(InCapacity) {}
bool VaRest::Http::FDiskCache::IsQuarantined() const
{ check(IsInGameThread()); return bMaintenancePending || DiskCachePrivate::ControlsExhausted(); }
void VaRest::Http::FDiskCache::RequestMaintenance(bool bClear)
{
    check(IsInGameThread());
    ++Epoch; ++ControlSerial; bMaintenancePending=true; bClearRequired|=bClear;
    DiskCachePrivate::RetainIntent(Directory,AsShared());
    TryMaintenance();
}
void VaRest::Http::FDiskCache::TryMaintenance()
{
    check(IsInGameThread());
    if (!bMaintenancePending || bMaintenanceInFlight || DiskCachePrivate::ControlsExhausted() || !PersistenceAccepting()) return;
    if (!DiskCachePrivate::RetainIntent(Directory,AsShared())) return;
    const uint64 Requested=ControlSerial;
    const int64 Desired=CapacityBytes;
    const bool bClear=bClearRequired;
    bMaintenanceInFlight=true;
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    const bool bAccepted=LaunchPersistence([Self=AsShared(),Requested,Desired,bClear
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]
    {
        FIoResult Result;
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid() && Hooks->bFailCacheControl) return Result;
#endif
        Result=DiskCachePrivate::Maintain(Self->Directory,Desired,bClear);
        if (Result.bOk)
        { Self->WorkerCapacityBytes=FMath::Min(Self->WorkerCapacityBytes,Desired); Self->WorkerAppliedControlSerial=Requested; }
        return Result;
    },[Self=AsShared(),Requested](FIoResult&& Result)
    {
        Self->bMaintenanceInFlight=false;
        if (Result.bOk && Self->ControlSerial==Requested)
        {
            Self->bMaintenancePending=false; Self->bClearRequired=false;
            DiskCachePrivate::Dirty().Remove(Self->Directory);
        }
        // No recursive retries on IO failure or full admission. A later access retries while all reuse remains blocked.
    });
    if (!bAccepted) bMaintenanceInFlight=false;
}
TSharedRef<VaRest::Http::FDiskCache,ESPMode::ThreadSafe>
VaRest::Http::FDiskCache::Open(FString Root,int64 Capacity)
{
    check(IsInGameThread());
    auto& Values=DiskCachePrivate::Registry();
    for (auto It=Values.CreateIterator();It;++It) if (!It.Value().IsValid()) It.RemoveCurrent();
    if (auto Existing=Values.FindRef(Root).Pin())
    {
        if (Capacity<Existing->CapacityBytes)
        { Existing->CapacityBytes=Capacity; Existing->RequestMaintenance(false); }
        else Existing->TryMaintenance();
        return Existing.ToSharedRef();
    }
    TSharedRef<FDiskCache,ESPMode::ThreadSafe> Shared=MakeShareable(new FDiskCache(Root,Capacity));
    Values.Add(Root,Shared);
    Shared->TryMaintenance(); // bounded initial trim; preserves valid restart entries
    return Shared;
}
void VaRest::Http::FDiskCache::Find(FString Key,double Now,FDateTime Utc,FCacheReadDone&& Done)
{
    if (IsQuarantined()) TryMaintenance();
    if (DiskCachePrivate::ControlsExhausted() || (bMaintenancePending && !bMaintenanceInFlight)) { Done(nullptr); return; }
    const uint64 RequiredControl=ControlSerial;
    const uint64 ExpectedEpoch=Epoch;
    const int64 ReadCapacity=CapacityBytes; // exact immutable cap used by BOTH the job and its byte reservation
    const TWeakPtr<FDiskCache,ESPMode::ThreadSafe> Weak(AsShared());
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    auto Callback=MakeShared<FCacheReadDone,ESPMode::ThreadSafe>(MoveTemp(Done));
    const bool bAccepted=LaunchPersistenceResult<TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>>(
        [Store=AsShared(),Root=Directory,Key=MoveTemp(Key),Now,Utc,ReadCapacity,RequiredControl
#if WITH_DEV_AUTOMATION_TESTS
            ,Hooks
#endif
        ]
        {
#if WITH_DEV_AUTOMATION_TESTS
            if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeRead);
#endif
            if (Store->WorkerAppliedControlSerial!=RequiredControl) return TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>();
            return DiskCachePrivate::Read(Root,Key,Now,Utc,ReadCapacity);
        },
        [Weak,ExpectedEpoch,Callback](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe>&& Entry) mutable
        {
            const auto Pinned=Weak.Pin();
            if (!Pinned.IsValid() || Pinned->Epoch!=ExpectedEpoch || Pinned->IsQuarantined()) Entry.Reset();
            (*Callback)(MoveTemp(Entry));
        },FMath::Min<int64>(ReadCapacity,32*1024*1024)+65536);
    if (!bAccepted) (*Callback)(nullptr);
}
void VaRest::Http::FDiskCache::Remove(FString Key)
{ RequestMaintenance(true); } // conservative namespace clear; no per-key unbounded tombstone set
void VaRest::Http::FDiskCache::Clear()
{ RequestMaintenance(true); }
void VaRest::Http::FDiskCache::Put(FCacheEntry Entry,FPersistenceGuard Guard)
{
    if (IsQuarantined()) TryMaintenance();
    if (DiskCachePrivate::ControlsExhausted() || (bMaintenancePending && !bMaintenanceInFlight)) return;
    const uint64 RequiredControl=ControlSerial;
    const int64 Reservation=Entry.Body.Num()+((Entry.Body.Num()+2LL)/3)*4+262144;
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    LaunchPersistence([Store=AsShared(),Root=Directory,Value=MoveTemp(Entry),Guard=MoveTemp(Guard),RequiredControl
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]() mutable
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeCacheWrite);
#endif
        const int64 Capacity=Store->WorkerCapacityBytes;
        FIoResult Result;
        if (Store->WorkerAppliedControlSerial!=RequiredControl) return Result;
        if (!Guard.AllowsBody(EVaRestBodyKind::Binary,Value.ContentType,Value.Body)) return Result;
        Value.Headers=Guard.SafeHeaders(Value.Headers);
        const TArray<uint8> Encoded=EncodeCacheRecord(Value);
        if (Encoded.IsEmpty() || Encoded.Num()>Capacity || Guard.ContainsSecret(Encoded)) return Result;
        auto Records=DiskCachePrivate::Scan(Root);
        DiskCachePrivate::Trim(Records,Capacity,Encoded.Num());
        int64 Retained=0; for (const auto& Record : Records) Retained+=Record.Size;
        if (Retained>Capacity-Encoded.Num()) return Result;
        const FString Path=FPaths::Combine(Root,Value.Key+TEXT(".")+Value.Generation.ToString(EGuidFormats::DigitsLower)+TEXT(".json"));
        Result=WriteNewFile(Path,Encoded);
        if (Result.bOk)
            for (const auto& Record : Records)
                if (Record.Key==Value.Key && Record.Path!=Path) VaRest::Http::DeleteFile(Record.Path);
        return Result;
    },[](FIoResult&&){},Reservation);
}
```

Disk Put does not hold the logical request for best-effort cache persistence; its worker admission is byte-bounded by the persistence executor, so successful responses cannot enqueue unlimited cache writes. Add executor `MaxPendingBytes=128MiB` and `LaunchPersistence(..., int64 ReservedBytes=0)`; reject an admission above the cap before copying payload, retain its reservation until GT consumes or discards the result slot, count the immutable body plus encoded upper bound. Queue admissions already have their stricter reservations. No disk cache write is required for a request to succeed; failed cache admission is a skipped store, never an application error.

- [ ] **Step 5: Run green and faults.** Inject read failure, short read, flush failure, move failure, deleted file between index/read, future timestamp, empty-body many-key flood. Assert files are never read/written on GT and corrupt cache never fails the request. Change the epoch comparison to unconditional acceptance and make ClearWhileRead test fail; restore. Change LRU eviction to newest and make MemoryLru fail; restore.
For the cap-fallback test only, declare `FScopedCacheControls` in VaRestDiskCache.h under WITH_DEV_AUTOMATION_TESTS, namespace VaRest::Http::Testing; define it in VaRestDiskCache.cpp after the private FControls type. It owns isolated registries instead of overwriting/resetting production counters or sticky state:

```cpp
class FScopedCacheControls
{
public:
    FScopedCacheControls();
    ~FScopedCacheControls();
private:
    void* Owned=nullptr;
};
VaRest::Http::Testing::FScopedCacheControls::FScopedCacheControls()
{
    check(IsInGameThread()); check(PersistenceIdle()); check(!DiskCachePrivate::TestControls);
    auto* Created=new DiskCachePrivate::FControls(); Owned=Created; DiskCachePrivate::TestControls=Created;
}
VaRest::Http::Testing::FScopedCacheControls::~FScopedCacheControls()
{
    check(IsInGameThread()); check(PersistenceIdle()); check(DiskCachePrivate::TestControls==Owned);
    DiskCachePrivate::TestControls=nullptr; delete static_cast<DiskCachePrivate::FControls*>(Owned);
}
```

**R7 teardown/retained state:** after StopPersistence returns in both the module's PreExit and ShutdownModule paths, call `FDiskCache::StopControls()` idempotently and include VaRestDiskCache.h (the header containing FDiskCache in this task). Add `Source/VaRestHttp/Private/VaRestHttpModule.cpp` to Task5's Files and exact commit paths. Control intent holds at most4096 stores with root≤4096 TCHARs, at most three retained root copies per dirty intent (Directory, Dirty key, Registry key), each limited to4096 TCHARs plus terminator/capacity; charge this separate control budget as4096×3×8192×sizeof(TCHAR), at most384MiB for4-byte TCHAR, plus fixed bounded store/map entries, separately from the executor128MiB DTO allowance. Long roots trigger the same process-wide conservative fallback without retaining the long string in Dirty. Old weak registry entries are pruned on Open; do not accumulate one per historical root. Once cap fallback fires every Open/Find/Put refuses reuse for the process lifetime. StopControls releases strong intent only after admission closes and workers/owned handbacks drain; it acknowledges no durable clear. A later process restart may see old bytes after an unacknowledged failed control.

- [ ] **Step 6: Commit:** `git add Source/VaRestHttp/Private/VaRestMemoryCache.h Source/VaRestHttp/Private/VaRestMemoryCache.cpp Source/VaRestHttp/Private/VaRestDiskCache.h Source/VaRestHttp/Private/VaRestDiskCache.cpp Source/VaRestHttp/Private/Tests/VaRestHttpMemoryCacheTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpDiskCacheTest.cpp Source/VaRestHttp/Public/VaRestStage.h Source/VaRestHttp/Private/VaRestHttpModule.cpp && git commit -m "feat(http): add bounded memory and asynchronous disk caches"`.

### Task 6: Connectivity ownership, offline refusal and scheduled probes

**Files:** Create `Public/VaRestConnectivity.h`, `Private/VaRestConnectivity.cpp`, `Private/Tests/VaRestHttpConnectivityTest.cpp`; modify `Public/VaRestClient.h`, `Private/VaRestClient.cpp`, `Private/VaRestClientState.h/.cpp`, `Private/VaRestPipeline.cpp`, `Private/VaRestClassify.cpp`, `Private/VaRestExchangeState.h`, `Public/VaRestHttpTesting.h`, `Private/VaRestHttpTesting.cpp`.

**Interfaces:** Consumes IVaRestClock and current dispatch outcomes. Produces:

```cpp
using FVaRestConnectivityChangedEvent = TMulticastDelegate<void(bool)>;
class VARESTHTTP_API FVaRestConnectivity : public TSharedFromThis<FVaRestConnectivity,ESPMode::ThreadSafe>
{
public:
    static TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> GetDefault();
    static TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> Make(
        TSharedRef<IVaRestClock,ESPMode::ThreadSafe> Clock);
    bool IsOnline() const;
    void ReportOnline(uint64 CauseRequestId = 0);
    void ReportOffline(uint64 CauseRequestId = 0);
    FVaRestConnectivityChangedEvent& OnChanged() { return Changed; }
    void SetProbeInterval(double Seconds);
    void RegisterProbe(uint64 ClientId, TFunction<bool()>&& TryStart);
    void UnregisterProbe(uint64 ClientId);
    void ProbeFinished();
private:
    explicit FVaRestConnectivity(TSharedRef<IVaRestClock,ESPMode::ThreadSafe> InClock);
    void Transition(bool bNewOnline, uint64 CauseRequestId);
    void ArmProbe();
    void RunProbe();
    TSharedRef<IVaRestClock,ESPMode::ThreadSafe> TimerClock;
    FVaRestTimerHandle ProbeTimer;
    FVaRestConnectivityChangedEvent Changed;
    TMap<uint64,TFunction<bool()>> Probes;
    bool bOnline = true; // latest observed state, independent of ForceOffline
    bool bProbeRunning = false;
    double IntervalSeconds = 10.0;
};
// FVaRestClient:
FVaRestClient& WithConnectivity(TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity);
FVaRestClient& WithHealthUrl(FStringView Url);
FVaRestClient& WithStorageDirectories(FStringView CacheRoot, FStringView QueueRoot);
// namespace VaRest::Http:
FVaRestAttemptOutcome MakeOfflineOutcome();
// FClientConfig:
TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
FString HealthUrl;
FString CacheRoot;
FString QueueRoot;
// FExchangeState + FRequestSpec (private core-only setter):
bool bConnectivityProbe = false;
```

- [ ] **Step 1: Add `VaRest.Http.Connectivity.SharedAndIsolated`.**

```cpp
using namespace VaRest::Http::Testing;
FTestRig Left;
FTestRig Right;
const auto Shared = FVaRestConnectivity::Make(Left.Clock);
Left.Client.WithConnectivity(Shared);
Right.Client.WithConnectivity(Shared);
const auto Isolated = FVaRestConnectivity::Make(Right.Clock);
TSharedRef<TArray<bool>,ESPMode::ThreadSafe> Seen = MakeShared<TArray<bool>,ESPMode::ThreadSafe>();
Shared->OnChanged().AddLambda([Seen](bool bOnline) { Seen->Add(bOnline); });
Shared->ReportOffline();
Shared->ReportOffline();
TestFalse(TEXT("shared state offline"), Shared->IsOnline());
TestTrue(TEXT("other Hub remains online"), Isolated->IsOnline());
TestEqual(TEXT("edge once"), Seen->Num(), 1);
int32 Probes = 0;
Shared->RegisterProbe(Left.Client.GetId(), [&Probes] { ++Probes; return false; });
Left.Clock->Advance(9.999);
TestEqual(TEXT("no early probe"), Probes, 0);
Left.Clock->Advance(0.001);
TestEqual(TEXT("probe at ten seconds"), Probes, 1);
Shared->ReportOnline();
Left.Clock->Advance(20);
TestEqual(TEXT("online stops probing"), Probes, 1);
Shared->UnregisterProbe(Left.Client.GetId());
TestTrue(TEXT("drain"), Left.Drain() && Right.Drain());
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Connectivity`.
- [ ] **Step 3: Implement state transitions and timer mechanics.** `Make` allocates with a private constructor via MakeShareable; GetDefault is a leaked TSharedRef with FVaRestRealClock::Get. Destructor must run on GT (client/config destructors already marshal), clear ProbeTimer and remove cvar registry subscription. Registry holds weak connectivity refs, not a strong cycle. Transition pins AsShared for user callbacks, updates state once, emits per-client Marker Online/Offline with cause (iterate live clients whose config Connectivity matches this pointer), broadcasts Changed, then arms/stops probe after reentrant callback changes are reconciled. A Changed callback may drop the last client or call ReportOffline; queue edges and deliver FIFO as session changes do, rather than recursively broadcasting.

```cpp
void FVaRestConnectivity::ArmProbe()
{
    TimerClock->ClearTimer(ProbeTimer);
    if (IsOnline() || VaRest::Http::ConnectivityPrivate::Forced() || bProbeRunning || Probes.IsEmpty()) return;
    const TWeakPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Weak(AsShared());
    ProbeTimer = TimerClock->SetTimer(IntervalSeconds, [Weak]
    {
        if (const auto Pinned = Weak.Pin()) Pinned->RunProbe();
    });
}
void FVaRestConnectivity::RunProbe()
{
    ProbeTimer.Invalidate();
    if (IsOnline() || bProbeRunning) return;
    if (VaRest::Http::ConnectivityPrivate::Forced()) return;
#if PLATFORM_ANDROID || PLATFORM_IOS
    const ENetworkConnectionType Hint=FPlatformMisc::GetNetworkConnectionType();
    if (Hint==ENetworkConnectionType::None || Hint==ENetworkConnectionType::AirplaneMode) { ArmProbe(); return; }
#endif
    TArray<uint64> Owners;
    Probes.GetKeys(Owners);
    Owners.Sort();
    for (uint64 OwnerId : Owners)
    {
        const TFunction<bool()>* Probe = Probes.Find(OwnerId);
        if (!Probe) continue;
        // Copy before invoking: callback can unregister itself.
        TFunction<bool()> TryStart = *Probe;
        bProbeRunning = true;
        if (TryStart()) return;
        bProbeRunning = false;
    }
    ArmProbe();
}
void FVaRestConnectivity::ProbeFinished()
{
    check(IsInGameThread());
    bProbeRunning = false;
    ArmProbe();
}
```

`ConnectivityPrivate::Forced()` reads the non-Shipping `VaRest.Http.ForceOffline` cvar; Shipping returns false and does not register it. A cvar sink marshals to GT, calls Transition(false) once for every live connectivity when switched on, and on switched-off publishes the latest observed state with reason OverrideRemoved and arms probes only if that state is offline. IsOnline returns `bOnline && !ConnectivityPrivate::Forced()`. Interval must be finite and >0, clamp to [0.1,3600] seconds; default10. Register/unregister arm as needed and always use weak-client callback captures.

Mobile hint: on each offline probe tick, under `PLATFORM_ANDROID || PLATFORM_IOS`, call `FPlatformMisc::GetNetworkConnectionType()`. None/AirplaneMode keep offline and skip that probe; Unknown does not suppress it; WiFi/Cell/etc permit probing but never assert Internet reachability. Engine source checked: `GenericPlatformMisc.h:356` enum and `:1716` default Unknown. No engine dependency is added.

Add these complete connectivity methods. The class declaration adds destructor, `void RefreshOverride();`, `void PublishEffective(uint64,FString);`, `bool bPublishedOnline=true; bool bDelivering=false; TArray<TTuple<bool,uint64,FString>> PendingEdges;`. GetDefault/Make register weak objects in the helper registry. Add `HAL/IConsoleManager.h`, `VaRestClientState.h`, `VaRestTracePoints.h`, `Misc/ScopeLock.h` and Tasks/Task.h.

```cpp
namespace VaRest::Http::ConnectivityPrivate
{
    TArray<TWeakPtr<FVaRestConnectivity,ESPMode::ThreadSafe>>& Live()
    { static auto* Values=new TArray<TWeakPtr<FVaRestConnectivity,ESPMode::ThreadSafe>>(); return *Values; }
#if !UE_BUILD_SHIPPING
    TAutoConsoleVariable<int32> Force(TEXT("VaRest.Http.ForceOffline"),0,
        TEXT("Force effective HTTP connectivity offline; preserve observed state"));
#endif
    bool Forced()
    {
#if !UE_BUILD_SHIPPING
        return Force.GetValueOnGameThread()!=0;
#else
        return false;
#endif
    }
    void PollOverride()
    {
        check(IsInGameThread());
        auto& Values=Live();
        Values.RemoveAll([](const auto& Weak){ return !Weak.IsValid(); });
        const auto Snapshot=Values;
        for (const auto& Weak : Snapshot) if (const auto Pinned=Weak.Pin()) Pinned->RefreshOverride();
    }
}
FVaRestConnectivity::FVaRestConnectivity(TSharedRef<IVaRestClock,ESPMode::ThreadSafe> InClock)
    : TimerClock(MoveTemp(InClock)) {}
FVaRestConnectivity::~FVaRestConnectivity()
{
    check(IsInGameThread()); TimerClock->ClearTimer(ProbeTimer);
}
TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> FVaRestConnectivity::Make(
    TSharedRef<IVaRestClock,ESPMode::ThreadSafe> Clock)
{
    check(IsInGameThread());
    TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> Value=MakeShareable(new FVaRestConnectivity(MoveTemp(Clock)));
    VaRest::Http::ConnectivityPrivate::Live().Add(Value);
    Value->bPublishedOnline=Value->IsOnline();
    return Value;
}
TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> FVaRestConnectivity::GetDefault()
{
    static auto* Value=new TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe>(Make(FVaRestRealClock::Get()));
    return *Value;
}
bool FVaRestConnectivity::IsOnline() const
{
    check(IsInGameThread()); return bOnline && !VaRest::Http::ConnectivityPrivate::Forced();
}
void FVaRestConnectivity::ReportOnline(uint64 CauseRequestId) { Transition(true,CauseRequestId); }
void FVaRestConnectivity::ReportOffline(uint64 CauseRequestId) { Transition(false,CauseRequestId); }
void FVaRestConnectivity::Transition(bool bNewOnline,uint64 CauseRequestId)
{
    check(IsInGameThread());
    bOnline=bNewOnline;
    PublishEffective(CauseRequestId,bNewOnline ? TEXT("ObservedOnline") : TEXT("ObservedOffline"));
}
void FVaRestConnectivity::RefreshOverride()
{
    PublishEffective(0,VaRest::Http::ConnectivityPrivate::Forced() ? TEXT("OverrideEnabled") : TEXT("OverrideRemoved"));
}
void FVaRestConnectivity::PublishEffective(uint64 CauseRequestId,FString Reason)
{
    check(IsInGameThread());
    const auto Keep=AsShared();
    const bool bEffective=IsOnline();
    if (bEffective!=bPublishedOnline)
    {
        bPublishedOnline=bEffective;
        PendingEdges.Emplace(bEffective,CauseRequestId,MoveTemp(Reason));
    }
    if (bDelivering) return;
    TGuardValue<bool> Delivering(bDelivering,true);
    for (int32 Index=0;Index<PendingEdges.Num();++Index)
    {
        const auto Edge=PendingEdges[Index];
        VaRest::Http::FClientState::ForEachLive([this,&Edge](VaRest::Http::FClientState& Client)
        {
            if (Client.GetConfig()->Connectivity.Get()!=this) return;
            VaRest::Http::TracePoints::Marker(Client,Edge.Get<0>() ? EVaRestTraceMarkerKind::Online : EVaRestTraceMarkerKind::Offline,
                Edge.Get<1>(),Edge.Get<2>());
        });
        Changed.Broadcast(Edge.Get<0>());
    }
    const bool bHadEdges=!PendingEdges.IsEmpty();
    PendingEdges.Reset();
    if (bHadEdges) ArmProbe();
}
void FVaRestConnectivity::SetProbeInterval(double Seconds)
{
    check(IsInGameThread());
    IntervalSeconds=FMath::IsFinite(Seconds) ? FMath::Clamp(Seconds,0.1,3600.0) : 10.0;
    ArmProbe();
}
void FVaRestConnectivity::RegisterProbe(uint64 ClientId,TFunction<bool()>&& TryStart)
{
    check(IsInGameThread()); Probes.Add(ClientId,MoveTemp(TryStart)); ArmProbe();
}
void FVaRestConnectivity::UnregisterProbe(uint64 ClientId)
{
    check(IsInGameThread()); Probes.Remove(ClientId); ArmProbe();
}
```

Define the offline outcome factory in namespace VaRest::Http (FVaRestAttemptOutcome itself is a global type):

```cpp
FVaRestAttemptOutcome VaRest::Http::MakeOfflineOutcome()
{
    FVaRestAttemptOutcome Result;
    Result.Failure=EVaRestTransportFailure::Other;
    Result.FailureDetail=TEXT("Offline");
    Result.FailureMessage=TEXT("connectivity is offline");
    return Result;
}
```

ArmProbe/RunProbe use effective connectivity plus the independent Forced predicate. Register one `FAutoConsoleVariableSink` with PollOverride in this cpp for non-Shipping; its sink runs on GT. It iterates weak snapshot and never holds a registry lock during callbacks. Global override is process-wide; scoped observed state remains per object.

Add the real probe-construction method to FRequestSpecAccess (declaration in VaRestRequestSpec.h, implementation in VaRestClient.cpp):

```cpp
FVaRestRequestHandle VaRest::Http::FRequestSpecAccess::SendProbe(
    const TSharedRef<FClientState,ESPMode::ThreadSafe>& State,FStringView Url,TUniqueFunction<void()>&& Done)
{
    TUniquePtr<FRequestSpec> Spec(new FRequestSpec(State,TEXT("HEAD"),Url));
    Spec->AuthMode=EVaRestAuthMode::None;
    Spec->Retry=FVaRestRetryPolicy::Disabled();
    Spec->bConnectivityProbe=true;
    auto Handler=MakeUnique<TResultHandler<void>>();
    Handler->OnComplete=[Callback=MoveTemp(Done)](const TVaRestResult<void>&){ Callback(); };
    Spec->Handler=MoveTemp(Handler);
    return Send(MoveTemp(Spec),__FILE__,__LINE__);
}
```

`FClientState::AttachConnectivity` registers a callback with a weak state, pins it, reads current config, and if HealthUrl nonempty/same-origin calls SendProbe then returns true; Done captures connectivity strongly and calls ProbeFinished. In Task 8 prepend `if (Pinned->Queue.IsValid() && Pinned->Queue->TryReplay(true)) return true;`. FClientState::Create calls AttachConnectivity after live registration. WithConnectivity calls old.UnregisterProbe(ClientId), publishes new config, then AttachConnectivity; WithHealthUrl changes config only (the callback reads current config). Destructor's existing ReleaseConfig lambda adds captured ClientId and calls `Doomed->Connectivity->UnregisterProbe(Id)` on GT; GT destructor branch unregisters directly. No raw state pointer reaches worker/callback.

- [ ] **Step 4: Wire clients and outcomes.** New client config gets default connectivity and default roots; FTestRig adds public `TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;`, initializes it with Make(Clock), and injects it so existing connection-failure tests do not poison each other. WithClock does **not** silently change a shared connectivity clock; docs require an injected connectivity with the desired clock. WithConnectivity unregisters old and registers new weak probe; WithStorageDirectories normalizes absolute roots before joining generated leaf names, rejects roots with unresolved `..`, and republishes a configuration snapshot. In OnAttemptDone only current dispatch reports Connection→offline or real HTTP response→online. After queues exist the probe callback asks queue head first, otherwise starts a `HEAD<void>` request to HealthUrl with NoAuth/NoCache/NoRetry and core-only probe flag; completion always calls ProbeFinished, including cancel/local refusal. HealthUrl must be same-origin and bounded, not an arbitrary redirectable credential request. An unhealthy HTTP status still demonstrates reachability.

`MakeOfflineOutcome` sets Failure=Other, FailureDetail="Offline", generic nonsecret message. KindFor maps Offline and QueueExpired to Offline/nonretryable; it maps PersistenceStopped/QueueWriteFailed/QueueFull/PersistenceCredentials/QueueConfig/QueueOrigin to Config/nonretryable, QueueIdentity to Auth/nonretryable. Queue-disabled normal requests known offline are refused after the cache opportunity. Auth-refresh requests known offline fail immediately Offline, never wait on a disk read; preserve Offline through `FVaRestAuthRefreshResult::Failed` and auth-gate refresh-failure classification by adding the explicit accepted kind and `AuthRefreshOffline` detail. A proactive still-valid-token fallback may attempt the original request only if connectivity now permits it; otherwise it returns Offline to the outer queue.

Concrete client/probe wiring (namespace qualification follows the landed client implementation):

```cpp
void VaRest::Http::FClientState::AttachConnectivity()
{
    const auto Reachability=GetConfig()->Connectivity;
    const TWeakPtr<FClientState,ESPMode::ThreadSafe> Weak(AsShared());
    Reachability->RegisterProbe(ClientId,[Weak]
    {
        const auto Pinned=Weak.Pin();
        if (!Pinned.IsValid() || FClientState::IsShuttingDown()) return false;
        const auto Current=Pinned->GetConfig();
        if (Current->HealthUrl.IsEmpty() || !SameOrigin(Current->BaseUrl,Current->HealthUrl)) return false;
        FRequestSpecAccess::SendProbe(Pinned.ToSharedRef(),Current->HealthUrl,
            [Connectivity=Current->Connectivity]{ Connectivity->ProbeFinished(); });
        return true;
    });
}
FVaRestClient& FVaRestClient::WithConnectivity(TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity)
{
    check(IsInGameThread());
    if (State.IsValid())
    {
        State->GetConfig()->Connectivity->UnregisterProbe(State->GetClientId());
        State->EditConfig([&](VaRest::Http::FClientConfig& Config){ Config.Connectivity=Connectivity; });
        State->AttachConnectivity();
    }
    return *this;
}
FVaRestClient& FVaRestClient::WithHealthUrl(FStringView Url)
{
    check(IsInGameThread());
    if (State.IsValid()) State->EditConfig([Url](VaRest::Http::FClientConfig& Config){ Config.HealthUrl=FString(Url); });
    return *this;
}
FVaRestClient& FVaRestClient::WithStorageDirectories(FStringView CacheRoot,FStringView QueueRoot)
{
    check(IsInGameThread());
    if (!State.IsValid()) return *this;
    FString Cache=FPaths::ConvertRelativePathToFull(FString(CacheRoot));
    FString Queue=FPaths::ConvertRelativePathToFull(FString(QueueRoot));
    FPaths::NormalizeDirectoryName(Cache); FPaths::NormalizeDirectoryName(Queue);
    const bool bValid=!CacheRoot.IsEmpty() && !QueueRoot.IsEmpty()
        && FPaths::CollapseRelativeDirectories(Cache) && FPaths::CollapseRelativeDirectories(Queue)
        && !Cache.Contains(TEXT("/../")) && !Queue.Contains(TEXT("/../"));
    State->EditConfig([&](VaRest::Http::FClientConfig& Config)
    {
        Config.StorageConfigError=bValid ? FString() : FString(TEXT("invalid persistence directory"));
        if (bValid) { Config.CacheRoot=Cache; Config.QueueRoot=Queue; }
    });
    return *this;
}
```

Add StorageConfigError to config and reject it in Resolve when cache disk or queue is requested. Default config initialization in FVaRestClient::Make assigns `Connectivity=FVaRestConnectivity::GetDefault()`, CacheRoot=`FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("VaRest/Cache"))`, QueueRoot=`...TEXT("VaRest/Queue")`; normalize these paths with the same lexical path operations. No filesystem operation runs during configuration.

At Enter, after Started and normal Resolve validation but before starting the chain, add `if (Ex.bIsAuthRefresh && !Ex.Config->Connectivity->IsOnline()) { OnChainDone(Exchange,MakeOfflineOutcome()); return; }`. This independent refresh gate exists before the cache stage is introduced. Ordinary known-offline refusal remains in the next task's cache stage so cache/stale lookup retains precedence.

At the current-dispatch point of OnAttemptDone, after owner marking and before TracePoints::AttemptEnded, insert:

```cpp
if (bCurrent && !Ex.bFinished && !Ex.bCompletionQueued && !Ex.IsCancelRequested() && !IsOwnerGone(Ex))
{
    if (KindFor(Result.Failure,Result.FailureDetail)==EVaRestErrorKind::Connection)
        Ex.Config->Connectivity->ReportOffline(Ex.RequestId);
    else if (Result.Status>0)
        Ex.Config->Connectivity->ReportOnline(Ex.RequestId);
}
```

Do not add this branch to OnChainDone or Complete. Existing current-dispatch test stimuli supply stale tag serials to prove their completion cannot change state. In FTestRig declare Connectivity between Fake and Client and replace its constructor with:

```cpp
VaRest::Http::Testing::FTestRig::FTestRig(FStringView BaseUrl)
    : Clock(MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>())
    , Fake(MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock))
    , Connectivity(FVaRestConnectivity::Make(Clock))
    , Client(FVaRestClient::Make(BaseUrl))
{
    Client.WithTransport(Fake).WithClock(Clock).WithConnectivity(Connectivity).WithLogLevel(EVaRestLogLevel::None);
}
```

The cvar sink is declared after PollOverride, not before its namespace definition:

```cpp
#if !UE_BUILD_SHIPPING
namespace VaRest::Http::ConnectivityPrivate
{
    FAutoConsoleVariableSink Sink(FConsoleCommandDelegate::CreateStatic(&PollOverride));
}
#endif
```

The source ordering places ConnectivityPrivate declarations before ArmProbe/RunProbe so Forced is declared at its first call. Add AttachConnectivity's declaration in FClientState.h, SendProbe's full declaration in VaRestRequestSpec.h, includes of VaRestConnectivity.h to its consumers. In both FVaRestAuthRefreshResult::Failed and FClientState::FinishRefresh's FailedKind normalization, preserve Offline alongside Timeout/Config. In AuthGate's failed-result switch return Other/AuthRefreshOffline for Offline and add `AuthRefreshOffline` to KindFor's Offline/nonretryable details; the exact existing switch case is extended, never coerced into Connection.

- [ ] **Step 5: Verify real behavior.** Add fake tests: Connection first marks offline after retries; 401/500 response marks online; local Config/Cancel/Timeout do not change state; stale late dispatch result cannot mark online; ForceOffline blocks health probes and refresh (queue-head coverage belongs to Task 8); clearing force reveals observed state and schedules a probe only if observed offline; one health probe in flight across two clients; invalid interval never spins; mobile hint adapter injects None/Unknown/WiFi to verify decisions. Event listener can destroy/reconfigure clients and nested transition order remains FIFO. Run Connectivity, Auth and Retry filters. Mutation: remove bProbeRunning guard; simultaneous-owner test must detect >1 probe. Restore and rerun.
- [ ] **Step 6: Commit:** `git add Source/VaRestHttp/Public/VaRestConnectivity.h Source/VaRestHttp/Private/VaRestConnectivity.cpp Source/VaRestHttp/Private/Tests/VaRestHttpConnectivityTest.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestClassify.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestHttpTesting.cpp Source/VaRestHttp/Private/VaRestAuth.cpp Source/VaRestHttp/Private/VaRestAuthGate.cpp && git commit -m "feat(http): add scoped connectivity and bounded offline probes"`.

### Task 7: Cache stage, conditional requests and completion integration

**Files:** Create `Private/VaRestCache.cpp`, `Private/Tests/VaRestHttpCacheTest.cpp`; modify `Public/VaRestClient.h`, `Private/VaRestClient.cpp`, `Private/VaRestClientState.h/.cpp`, `Private/VaRestExchangeState.h`, `Private/VaRestPipeline.cpp`, `Public/VaRestStage.h`, `Private/VaRestAuth.cpp`.

**Interfaces:** Consumes Tasks 1–6 and landed stage API. Produces:

```cpp
// public builder/client
TVaRestRequest Cache(const FVaRestCachePolicy& Policy) &&
{ Spec->Cache = Policy; return MoveTemp(*this); }
TVaRestRequest NoCache() &&
{ Spec->Cache = FVaRestCachePolicy(); return MoveTemp(*this); }
FVaRestClient& WithCache(const FVaRestCachePolicy& Policy);
void ClearCache();
// FRequestSpec: TOptional<FVaRestCachePolicy> Cache;
// FClientConfig: FVaRestCachePolicy Cache;
// FClientState: FMemoryCache MemoryCache; TSharedPtr<FDiskCache,ESPMode::ThreadSafe> DiskCache;
// FExchangeState:
TOptional<FVaRestCachePolicy> CacheOverride;
FVaRestCachePolicy CachePolicy;
FString LookupKey;
FString LookupIdentity;
uint64 CacheEpoch = 0;
bool bCacheLookupPending = false;
EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
bool bCacheConditional = false;
bool bCacheIdentityChanged = false;
// FVaRestAttemptOutcome:
bool bFromCache = false;
// private namespace VaRest::Http
FVaRestStageRef MakeCacheStage();
bool BuildCacheKey(const FExchangeState& Ex, FString& OutKey, FString& OutIdentity);
void CheckConditionalIdentity(FExchangeState& Ex); // AuthApply, after limiter, BEFORE ApplyCredentials
```

`WithCache` validates finite nonnegative TTL and capacities, preserves bad configuration as a deferred Config error in config (same behavior as auth configuration), and republishes. Send copies override; Resolve picks it and sets pending only for an eligible unresolved lookup. The state owns stores across Publish; rebuilding the chain never empties the memory cache. Cache disabled bypasses without stats. Add outcome bFromCache in Task 5 as part of that task's compile unit; Task 7 consumes it. OnChainDone copies `Outcome.bFromCache` into `Finish.Response.bFromCache` before decode. EffectiveUrl for a cache answer is the current resolved URL (never a persisted request URL).

- [ ] **Step 1: Add `VaRest.Http.Cache.RefreshAndAccounts`.**

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FTestRig Rig;
auto Policy = FVaRestCachePolicy::Memory();
Policy.DefaultTtlSeconds = 60;
Rig.Client.WithCache(Policy).WithAuth(FVaRestAuth::Bearer());
Rig.Client.SetBearerToken(TEXT("A-token-1234"));
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("A-body")));
auto First = MakeOutcome<FString>();
Observe(Rig.Client.Get<FString>(TEXT("/me")), First).Send();
if (!TestTrue(TEXT("first"), Rig.Wait(First))) return false;
Rig.Client.SetBearerToken(TEXT("A-token-rotated"), true);
auto Second = MakeOutcome<FString>();
Observe(Rig.Client.Get<FString>(TEXT("/me")), Second).Send();
if (!TestTrue(TEXT("second"), Rig.Wait(Second))) return false;
TestEqual(TEXT("refresh remains cache hit"), Rig.Fake->NumCalls(), 1);
TestTrue(TEXT("response provenance"), Second->Response.bFromCache);
if (!TestTrue(TEXT("value present"), Second->Value.IsSet())) return false;
TestEqual(TEXT("bytes decoded each time"), Second->Value.GetValue(), FString(TEXT("A-body")));
Rig.Client.SetBearerToken(TEXT("B-token-1234"));
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("B-body")));
auto Third = MakeOutcome<FString>();
Observe(Rig.Client.Get<FString>(TEXT("/me")), Third).Send();
if (!TestTrue(TEXT("third"), Rig.Wait(Third))) return false;
TestEqual(TEXT("other account sent"), Rig.Fake->NumCalls(), 2);
TestFalse(TEXT("other account not cached A"), Third->Response.bFromCache);
TestTrue(TEXT("drains"), Rig.Drain());
return true;
```

Add `VaRest.Http.Cache.Revalidate304`: first response Text(200,"original") with max-age=1/ETag `"one"`; advance 1; next scripted status304 with Cache-Control max-age=20; assert second wire request's If-None-Match equals `"one"`, response status200/body original/fromcache, then third request before20 adds no call. Guard `NumCalls()==2` before GetCall(1). `VaRest.Http.Cache.NoCacheNoStore` runs no-cache→304 every time, no-store→two200 calls, request NoCache→no read/write, and non-200/HEAD/GET separation.

Register `VaRest.Http.Cache.Head304` with this body; response headers use the actual fake DTO's mutable Headers field:

```cpp
using namespace VaRest::Http::Testing;
FTestRig Rig;
Rig.Client.WithCache(FVaRestCachePolicy::Memory());
FVaRestFakeResponse First=FVaRestFakeResponse::Text(200,TEXT(""));
First.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=0"));
First.Headers.Set(TEXT("ETag"),TEXT("\"head-v1\""));
First.Headers.Set(TEXT("Content-Length"),TEXT("7123"));
Rig.Fake->Enqueue(MoveTemp(First));
auto A=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/head")),A).Send();
if (!TestTrue(TEXT("initial HEAD"),Rig.Wait(A))) return false;
FVaRestFakeResponse Second=FVaRestFakeResponse::Text(304,TEXT(""));
Second.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
Rig.Fake->Enqueue(MoveTemp(Second));
auto B=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/head")),B).Send();
if (!TestTrue(TEXT("revalidated HEAD"),Rig.Wait(B))) return false;
if (!TestEqual(TEXT("two physical calls"),Rig.Fake->NumCalls(),2)) return false;
TestEqual(TEXT("validator sent"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("If-None-Match")),FString(TEXT("\"head-v1\"")));
TestTrue(TEXT("304 returns cache representation"),B->Response.bFromCache);
TestEqual(TEXT("HEAD length preserved"),B->Response.Headers.Get(TEXT("Content-Length")),FString(TEXT("7123")));
auto C=MakeOutcome<void>(); Observe(Rig.Client.Head<void>(TEXT("/head")),C).Send();
if (!TestTrue(TEXT("fresh HEAD"),Rig.Wait(C))) return false;
TestEqual(TEXT("fresh hit makes no wire call"),Rig.Fake->NumCalls(),2);
TestTrue(TEXT("fresh response from cache"),C->Response.bFromCache);
TestEqual(TEXT("fresh HEAD length preserved"),C->Response.Headers.Get(TEXT("Content-Length")),FString(TEXT("7123")));
TestTrue(TEXT("drained"),Rig.Drain());
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Cache`.
- [ ] **Step 3: Install and implement Cache independently.** In FClientState::Publish, immediately before the existing Retry insertion add `NewChain->Add(MakeCacheStage());` immediately before `NewChain->Add(MakeRetryStage());`. The resulting task-7 chain is custom Outer → Cache → Retry → AuthGate → boundary → limiter → AuthApply → custom PerAttempt → Log → Transport. Put the three private declarations shown above (MakeCacheStage, BuildCacheKey and CheckConditionalIdentity) in namespace VaRest::Http at the start of namespace VaRest::Http in VaRestClientState.h; its existing includes of VaRestExchangeState.h and VaRestStage.h supply both types. Include VaRestClientState.h in VaRestCache.cpp and VaRestAuth.cpp; Publish already belongs to that state implementation. Do not wait for Task 8. Its decision function follows. Its private state is `FCacheOperation { Exchange, Next, Done, Entry, Key, Identity, Epoch, Wait }`. Owned by GT callbacks only; the disk worker gets key/path/guard bytes, not this object. Each operation terminates once. The complete decision sequence is:

```cpp
void FCacheOperation::AfterLookup(TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Candidate, bool bDisk)
{
    FExchangeState& Ex = Internals(*Exchange);
    if (Wait.IsValid() && !Wait->CanHandoff()) return;
    FString CurrentKey, CurrentIdentity;
    if (!BuildCacheKey(Ex, CurrentKey, CurrentIdentity) || CurrentKey != Key || CurrentIdentity != Identity
        || Epoch != Ex.State->GetCacheEpoch())
    {
        Candidate.Reset();
        Ex.bCacheIdentityChanged = true;
    }
    Ex.bCacheLookupPending = false;
    if (Candidate.IsValid() && Ex.Handler.IsValid() && Ex.Handler->DecodesJson()
        && Ex.MaxJsonBytes>0 && Candidate->Body.Num()>Ex.MaxJsonBytes) Candidate.Reset();
    Entry = MoveTemp(Candidate);
    if (Entry.IsValid() && Entry->Metadata.IsFresh(Ex.Config->Clock->Now()) && !bForceValidation)
    {
        Ex.CacheLookup = bDisk ? EVaRestTraceCacheLookup::HitDisk : EVaRestTraceCacheLookup::HitMemory;
        if (!Ex.bCacheHitCounted) { Ex.bCacheHitCounted=true; FHttpStats::CacheHit(Ex.State->GetCounters()); }
        FVaRestAttemptOutcome Answer = Entry->Answer();
        Answer.EffectiveUrl = Ex.Url;
        Complete(MoveTemp(Answer));
        return;
    }
    Ex.CacheLookup = Entry.IsValid() ? EVaRestTraceCacheLookup::Stale : EVaRestTraceCacheLookup::Miss;
    if (!Ex.Config->Connectivity->IsOnline() && !Ex.bConnectivityProbe)
    {
        if (Entry.IsValid() && Ex.CachePolicy.bServeStaleOnError && !bForceValidation && Entry->Metadata.CanServeStale()) ServeStale();
        else Complete(MakeOfflineOutcome());
        return;
    }
    if (Entry.IsValid() && (!Entry->Metadata.ETag.IsEmpty() || !Entry->Metadata.LastModified.IsEmpty()))
    {
        if (!Entry->Metadata.ETag.IsEmpty()) Ex.RequestHeaders.Set(TEXT("If-None-Match"), Entry->Metadata.ETag);
        else Ex.RequestHeaders.Set(TEXT("If-Modified-Since"), Entry->Metadata.LastModified);
        Ex.bCacheConditional = true;
        Ex.CacheLookup = EVaRestTraceCacheLookup::Revalidate;
    }
    Proceed();
}
```

Define `FCacheOperation` with constructor `(FVaRestExchangeRef, FVaRestStageNext, FVaRestOutcomeCallback&&)`, `AfterLookup(...)`, `Proceed()`, `OnResult(FVaRestAttemptOutcome&&)`, `Complete(...)`, `ServeStale()`, and fields above plus `bool bForceValidation=false`. `Complete` uses Wait.Finish if valid else moves Done; `Proceed` gets Wait.Resume (or moves Done), clears Wait, calls Next.Proceed with self-captured callback invoking OnResult. `ServeStale` sets CacheLookup=Stale, emits CacheServedStale, counts a cache hit, takes Answer/EffUrl and completes. Never call `Next.Proceed` while Ex.Interrupt still belongs to the cache wait.

`MakeCacheStage::Process`: reject ineligible → set pending false, Next.Proceed; otherwise BuildCacheKey, count one lookup, capture current cache epoch, try memory, else disk if enabled. For disk install FPersistenceWait before scheduling Find; if scheduling refused answer Config/PersistenceStopped (except shutdown silent cancellation). Disk candidate that exceeds the **current request's** MaxJsonBytes for a JSON decoder is ignored and the request uses transport with its normal cap; it cannot bypass limits via cache. Before every successful store check current key/identity/epoch equality; mutation during transport skips store. Store a copy before decoder consumes Body.

`OnResult`: if status304 and captured Entry and unchanged identity/epoch, merge metadata, emit CacheRevalidated, return Answer with status200; no-store removes both stores, otherwise replace entry. If Connection/Offline and eligible stale, ServeStale. If status200/no failure/eligible metadata, store bytes and emit CacheStored with actual retained bytes; never cache files or credential-bearing response. If 200 no-store delete matching old record. Otherwise forward the exact outcome. Do not replay a 304 under changed identity; expose it as Http, not as cached success.

The operation class and its remaining bodies are defined in `namespace VaRest::Http::CachePrivate`; include VaRestClientState.h, VaRestPersistenceWait.h, VaRestClassify.h, VaRestTracePoints.h, VaRestCachePolicy.h, VaRestConnectivity.h and VaRestHttpStatsInternal.h.

```cpp
class FCacheOperation : public TSharedFromThis<FCacheOperation,ESPMode::ThreadSafe>
{
public:
    FCacheOperation(FVaRestExchangeRef InExchange,FVaRestStageNext InNext,FVaRestOutcomeCallback&& InDone)
        : Exchange(MoveTemp(InExchange)),Next(MoveTemp(InNext)),Done(MoveTemp(InDone)) {}
    void Start();
    void AfterLookup(TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Candidate,bool bDisk);
    void Proceed();
    void OnResult(FVaRestAttemptOutcome&& Outcome);
    void Complete(FVaRestAttemptOutcome&& Outcome);
    void ServeStale();
private:
    FVaRestExchangeRef Exchange;
    FVaRestStageNext Next;
    FVaRestOutcomeCallback Done;
    TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Entry;
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Disk;
    TSharedPtr<FPersistenceWait,ESPMode::ThreadSafe> Wait;
    FString Key,Identity;
    uint64 Epoch=0;
    bool bForceValidation=false;
};
void FCacheOperation::Complete(FVaRestAttemptOutcome&& Outcome)
{
    if (Wait.IsValid()) { Wait->Finish(MoveTemp(Outcome)); Wait.Reset(); return; }
    auto Callback=MoveTemp(Done);
    if (Callback) Callback(MoveTemp(Outcome));
}
void FCacheOperation::Proceed()
{
    if (Wait.IsValid()) { Done=Wait->Resume(); Wait.Reset(); }
    if (!Done) return;
    Next.Proceed(Exchange,[Self=AsShared()](FVaRestAttemptOutcome&& Outcome)
    { Self->OnResult(MoveTemp(Outcome)); });
}
void FCacheOperation::ServeStale()
{
    auto& Ex=Internals(*Exchange);
    Ex.CacheLookup=EVaRestTraceCacheLookup::Stale;
    if (!Ex.bCacheHitCounted) { Ex.bCacheHitCounted=true; FHttpStats::CacheHit(Ex.State->GetCounters()); }
    FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::CacheServedStale;
    TracePoints::Note(Ex,MoveTemp(Note));
    auto Answer=Entry->Answer(); Answer.EffectiveUrl=Ex.Url;
    Complete(MoveTemp(Answer));
}
void FCacheOperation::Start()
{
    auto& Ex=Internals(*Exchange);
    Ex.bCacheLookupPending=false;
    if (!BuildCacheKey(Ex,Key,Identity))
    {
        if (!Ex.Config->Connectivity->IsOnline() && !Ex.bConnectivityProbe)
            Complete(MakeOfflineOutcome());
        else Proceed();
        return;
    }
    Ex.LookupKey=Key; Ex.LookupIdentity=Identity;
    Epoch=Ex.State->GetCacheEpoch();
    if (!Ex.bCacheLookupCounted) { Ex.bCacheLookupCounted=true; FHttpStats::CacheLookup(Ex.State->GetCounters()); }
    for (const auto& Pair : Ex.RequestHeaders)
    {
        if (!Pair.Key.Equals(TEXT("Cache-Control"),ESearchCase::IgnoreCase)) continue;
        const FString Lower=Pair.Value.ToLower();
        bForceValidation|=Lower.Contains(TEXT("no-cache")) || Lower.Contains(TEXT("max-age=0"));
    }
    auto Memory=Ex.State->GetMemoryCache().Find(Key);
    if (Memory.IsValid()) { AfterLookup(MoveTemp(Memory),false); return; }
    if (Ex.CachePolicy.Mode!=EVaRestCacheMode::MemoryAndDisk) { AfterLookup(nullptr,false); return; }
    Ex.bCacheLookupPending=true;
    Disk=Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes);
    Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
    Disk->Find(Key,Ex.Config->Clock->Now(),FDateTime::UtcNow(),
        [Self=AsShared()](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Candidate)
        { Self->AfterLookup(MoveTemp(Candidate),true); });
}
void FCacheOperation::OnResult(FVaRestAttemptOutcome&& Outcome)
{
    auto& Ex=Internals(*Exchange);
    if (Ex.bFinished || Ex.bCompletionQueued || Ex.Phase!=EVaRestExchangePhase::InChain || Ex.IsCancelRequested() || IsOwnerGone(Ex))
    {
        // A real inner outcome still settles its callback once. The already-accepted outer result wins at OnChainDone.
        Complete(MoveTemp(Outcome));
        return;
    }
    FString CurrentKey,CurrentIdentity;
    const bool bSame=!Ex.bCacheIdentityChanged && BuildCacheKey(Ex,CurrentKey,CurrentIdentity)
        && CurrentKey==Key && CurrentIdentity==Identity && Epoch==Ex.State->GetCacheEpoch();
    if (!bSame) { Complete(MoveTemp(Outcome)); return; }
    FVaRestHeaders VaryHeaders=Ex.RequestHeaders;
    if (Ex.bCacheConditional) { VaryHeaders.Remove(TEXT("If-None-Match")); VaryHeaders.Remove(TEXT("If-Modified-Since")); }
    if (Ex.AuthMode!=EVaRestAuthMode::None)
        for (const auto& Header : Ex.State->GetSessionData().Headers) VaryHeaders.Set(Header.Key,Header.Value);
    const auto Kind=ClassifyAttempt(Outcome).Kind;
    if ((Kind==EVaRestErrorKind::Connection || Kind==EVaRestErrorKind::Offline)
        && Entry.IsValid() && Ex.CachePolicy.bServeStaleOnError && !bForceValidation && Entry->Metadata.CanServeStale())
    { ServeStale(); return; }
    if (Outcome.Status==304 && !Outcome.IsTransportFailure() && Entry.IsValid() && Ex.bCacheConditional)
    {
        FCacheEntry Updated=*Entry;
        Updated.Generation=FGuid::NewGuid();
        Updated.Headers=MergeNotModified(Entry->Headers,Outcome.Headers,Entry->Body.Num(),Ex.Verb==TEXT("HEAD"));
        if (Updated.Headers.Contains(TEXT("Content-Type"))) Updated.ContentType=Updated.Headers.Get(TEXT("Content-Type"));
        Updated.Metadata=ReadCacheMetadata(Updated.Headers,Ex.CachePolicy.DefaultTtlSeconds,Ex.Config->Clock->Now(),FDateTime::UtcNow());
        Ex.State->GetMemoryCache().Remove(Key);
        if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk)
            Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Remove(Key);
        if (!Updated.Metadata.bNoStore && CacheHeadersEligible(VaryHeaders,Updated.Headers))
        {
            Ex.State->GetMemoryCache().Put(Updated,Ex.CachePolicy.MemoryBytes);
            if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk)
            {
                FPersistenceGuard Guard=*Ex.PersistenceGuard; Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard);
                Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Put(Updated,MoveTemp(Guard));
            }
        }
        FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::CacheRevalidated;
        TracePoints::Note(Ex,MoveTemp(Note));
        if (!Ex.bCacheHitCounted) { Ex.bCacheHitCounted=true; FHttpStats::CacheHit(Ex.State->GetCounters()); }
        auto Answer=Updated.Answer(); Answer.EffectiveUrl=Ex.Url;
        Answer.BytesUp=Outcome.BytesUp; Answer.BytesDown=Outcome.BytesDown;
        Complete(MoveTemp(Answer)); return;
    }
    if (Outcome.Status==200 && !Outcome.IsTransportFailure() && !Outcome.bBodyInFile)
    {
        const auto Meta=ReadCacheMetadata(Outcome.Headers,Ex.CachePolicy.DefaultTtlSeconds,Ex.Config->Clock->Now(),FDateTime::UtcNow());
        if (Meta.bNoStore)
        {
            Ex.State->GetMemoryCache().Remove(Key);
            if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk) Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Remove(Key);
        }
        else if (CacheHeadersEligible(VaryHeaders,Outcome.Headers))
        {
            FCacheEntry Stored;
            Stored.Key=Key; Stored.Headers=Outcome.Headers; Stored.ContentType=Outcome.ContentType;
            Stored.Body=Outcome.Body; Stored.Metadata=Meta;
            Ex.State->GetMemoryCache().Put(Stored,Ex.CachePolicy.MemoryBytes);
            if (Ex.CachePolicy.Mode==EVaRestCacheMode::MemoryAndDisk)
            {
                FPersistenceGuard Guard=*Ex.PersistenceGuard;
                Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard); Guard.AbortSignal=Ex.PersistenceAbort;
                Ex.State->GetDiskCacheFor(*Ex.Config,Ex.CachePolicy.DiskBytes)->Put(Stored,MoveTemp(Guard));
            }
            if (Ex.State->GetMemoryCache().Find(Key).IsValid())
            {
                FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::CacheStored; Note.Bytes=Stored.Body.Num();
                TracePoints::Note(Ex,MoveTemp(Note));
            }
        }
    }
    Complete(MoveTemp(Outcome));
}
class FCacheStage final : public IVaRestStage
{
public:
    virtual FName GetStageName() const override { return TEXT("Cache"); }
    virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
    {
        auto Operation=MakeShared<FCacheOperation,ESPMode::ThreadSafe>(Exchange,Next,MoveTemp(Done));
        Operation->Start();
    }
};
```

`MakeCacheStage` has the exact body `FVaRestStageRef VaRest::Http::MakeCacheStage() { return MakeShared<CachePrivate::FCacheStage,ESPMode::ThreadSafe>(); }`. Before AfterLookup's freshness branch, reject a candidate when `Ex.Handler->DecodesJson() && Ex.MaxJsonBytes>0 && Candidate->Body.Num()>Ex.MaxJsonBytes`; do this after candidate validity check. Count hits there only behind `!Ex.bCacheHitCounted` and set the flag first. Memory cache hits retain encoded bytes internally; each Answer copies into decode's owned buffer. CacheStored is emitted only when the memory store actually retained the key before Completed. Bytes is retained encoded response bytes, never durable disk bytes. Best-effort disk completion emits no late request Note and never delays successful response delivery. A disk persistence attempt without memory retention therefore has no CacheStored note.

The key builder and conditional gate are:

```cpp
bool VaRest::Http::BuildCacheKey(const FExchangeState& Ex,FString& OutKey,FString& OutIdentity)
{
    OutKey.Reset(); OutIdentity.Reset();
    if (Ex.CachePolicy.Mode==EVaRestCacheMode::None || Ex.bIsAuthRefresh || Ex.bConnectivityProbe
        || (Ex.Verb!=TEXT("GET") && Ex.Verb!=TEXT("HEAD")) || !Ex.Body.IsEmpty() || !Ex.SaveTo.IsEmpty()) return false;
    for (const auto& Stage : Ex.Config->CustomStages) if (Stage.Value==EVaRestStageSlot::PerAttempt) return false;
    FVaRestHeaders Built=Ex.RequestHeaders;
    if (Ex.bCacheConditional) { Built.Remove(TEXT("If-None-Match")); Built.Remove(TEXT("If-Modified-Since")); }
    if (!CacheHeadersEligible(Built,FVaRestHeaders())) return false;
    const auto& Session=Ex.State->GetSessionData();
    if (!AuthIdentity(Ex.Config->Auth.Get(),Ex.AuthMode,Session,OutIdentity)) return false;
    OutKey=CacheKey(Ex.Verb,Ex.Url,OutIdentity,Built,Ex.AuthMode==EVaRestAuthMode::None ? FVaRestHeaders() : Session.Headers);
    return true;
}
void VaRest::Http::CheckConditionalIdentity(FExchangeState& Ex)
{
    if (!Ex.bCacheConditional) return;
    FString CurrentKey,CurrentIdentity;
    if (BuildCacheKey(Ex,CurrentKey,CurrentIdentity) && CurrentKey==Ex.LookupKey && CurrentIdentity==Ex.LookupIdentity) return;
    Ex.RequestHeaders.Remove(TEXT("If-None-Match")); Ex.RequestHeaders.Remove(TEXT("If-Modified-Since"));
    Ex.AttemptHeaders.Remove(TEXT("If-None-Match")); Ex.AttemptHeaders.Remove(TEXT("If-Modified-Since"));
    Ex.bCacheConditional=false; Ex.bCacheIdentityChanged=true;
}
```

Client-state store methods are added to its public private-module interface:

```cpp
FMemoryCache& GetMemoryCache() { check(IsInGameThread()); return MemoryCache; }
uint64 GetCacheEpoch() const { return CacheEpoch; }
TSharedRef<FDiskCache,ESPMode::ThreadSafe> GetDiskCacheFor(const FClientConfig& Cfg,int64 Capacity)
{
    FString Origin;
    CanonicalOrigin(Cfg.BaseUrl,Origin);
    DiskCache=FDiskCache::Open(FPaths::Combine(Cfg.CacheRoot,
        HashFields(TEXT("VaRest/cache-origin/v1"),TArray<FString>{Origin})),Capacity);
    return DiskCache.ToSharedRef();
}
void ClearCacheState()
{
    check(IsInGameThread()); ++CacheEpoch; MemoryCache.Clear();
    const auto Current=GetConfig();
    FString Origin;
    if (Current->StorageConfigError.IsEmpty() && CanonicalOrigin(Current->BaseUrl,Origin))
        GetDiskCacheFor(*Current,Current->Cache.DiskBytes)->Clear();
}
// member: uint64 CacheEpoch=1;
```

The state retains the opened disk store so ClearCache can invalidate pending reads. Request-only Cache overrides pass their effective DiskBytes capacity. ClearCache derives and opens the current configured namespace even before first use; it never clears an old retained pointer after root/origin changes. The independent client CacheEpoch still invalidates prior in-flight reads. WithCache and ClearCache public bodies:

```cpp
FVaRestClient& FVaRestClient::WithCache(const FVaRestCachePolicy& Policy)
{
    if (State.IsValid()) State->EditConfig([Policy](VaRest::Http::FClientConfig& Edited) { Edited.Cache=Policy; });
    return *this;
}
void FVaRestClient::ClearCache() { if (State.IsValid()) State->ClearCacheState(); }
```

Resolve validates Mode in range, finite TTL≥0 and byte caps≥0; invalid cache policy returns false with `OutError="invalid cache policy"`; then sets CachePolicy=CacheOverride.Get(Cfg.Cache). Set bCacheLookupPending based on BuildCacheKey after every prerequisite field is resolved; Started never claims a predicted hit. Add this at the end of Resolve, not before MaxJsonBytes/AuthMode assignment. All client/store access above is GT-only.

- [ ] **Step 4: Protect auth and retry boundaries.** Add `FClientState::GetCacheEpoch() const` and increment on ClearCache; ClearCache clears memory and queues disk clear. In AuthApply before ApplyCredentials, `CheckConditionalIdentity` computes current key ignoring the two cache-injected conditional headers; if identity/key changed, remove only those injected headers, set bCacheConditional=false/bCacheIdentityChanged=true and clear captured entry eligibility. The original request had no caller validators by eligibility, so removal cannot delete caller intent. Retry resets AttemptHeaders from RequestHeaders; CheckConditionalIdentity must remove from **both** maps. On valid identity set `AttemptDispatched.bConditional = Ex.bCacheConditional` in Task 10.

- [ ] **Step 5: Add lifecycle tests.** Disk read held→cancel→release yields Canceled, no wire, one outer outcome. Disk read held→ClearCache→release yields miss/network, no memory resurrection. Disk read held→replace account→release never yields A bytes; network uses B. Limiter occupied→conditional request queued→replace account→release sends no conditional header and caches no A-key response. Refresh token rotation during retry preserves identity/key and allows 304. Unknown custom auth and custom PerAttempt stages bypass cache. GET-with-body/SaveTo/Range/caller validators bypass; fresh cached JSON larger than a lower per-request MaxJsonBytes cannot decode successfully. Each test drains and guards indices. Run Cache, Auth, Retry, Cancel filters green; restore identity comparison removal to make account transition test fail.
- [ ] **Step 6: Commit:** `git add Source/VaRestHttp/Private/VaRestCache.cpp Source/VaRestHttp/Private/Tests/VaRestHttpCacheTest.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Public/VaRestStage.h Source/VaRestHttp/Private/VaRestAuth.cpp Source/VaRestHttp/Private/VaRestHttpStatsInternal.h Source/VaRestHttp/Private/VaRestHttpStats.cpp && git commit -m "feat(http): integrate cache hits revalidation and stale recovery"`.

### Task 8: Durable named queue with scoped admission and FIFO replay

**Build/review boundary:** Sections 8A–8D are one integrated task. Registry, loader, admission and replay mutually call each other; do not link or commit a nonfunctional stub between them. Implement the pure record codec first within this task and run its focused test once the integrated translation units compile. Each red/green section remains a separate short test action; the final commit and review cover the complete queue deliverable.

#### 8A: Policy and shared registry

**Files:** Create `Public/VaRestOffline.h`, `Private/VaRestOfflineInternal.h`, `Private/VaRestOffline.cpp`, `Private/Tests/VaRestHttpOfflineTest.cpp`; modify `Public/VaRestClient.h`, `Private/VaRestClient.cpp`, `Private/VaRestClientState.h/.cpp`, `Private/VaRestExchangeState.h`, `Public/VaRestHttpTesting.h`, `Private/VaRestTraceExcerpts.cpp`.

**Interfaces:** Consumes stable identity/canonical origin/connectivity. Produces:

```cpp
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestOfflinePolicy
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    int32 MaxQueued = 256;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    double MaxAgeSeconds = 86400.0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    int64 MaxItemBytes = 32 * 1024 * 1024;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    int64 MaxBodyBytes = 64 * 1024 * 1024;
};
// FVaRestResponse is a plain C++ type. M5 supplies the reflected event wrapper.
struct VARESTHTTP_API FVaRestQueuedResult
{
    FString QueueId;
    FName Tag;
    bool bSuccess = false;
    FVaRestResponse Response;
    FVaRestError Error;
};
using FVaRestQueuedResultEvent = TMulticastDelegate<void(const FVaRestQueuedResult&)>;
// FVaRestClient:
FVaRestClient& WithOfflineQueue(FStringView Name,
    const FVaRestOfflinePolicy& Policy = FVaRestOfflinePolicy());
int32 GetQueuedCount() const; // committed + reserved admissions, GT; includes active replay head
bool FlushQueue(TUniqueFunction<void(bool)>&& OnFlushed = {});
FVaRestQueuedResultEvent& OnQueuedResult();
// TVaRestRequest<T>:
TVaRestRequest QueueWhenOffline() && { Spec->bQueueWhenOffline = true; return MoveTemp(*this); }
TVaRestRequest OnQueued(TUniqueFunction<void(const FString&)>&& Callback) &&
{ Spec->OnQueued = MoveTemp(Callback); return MoveTemp(*this); }
```

Private coordinator contract:

```cpp
// namespace VaRest::Http
struct FQueueItem
{
    FString QueueId;
    uint64 Sequence = 0;
    FGuid OriginalRun;
    uint64 OriginalRequestId = 0;
    FDateTime CreatedUtc;
    FString Scope;
    FString Verb;
    FString Url;
    FVaRestHeaders Headers;
    FVaRestRequestBody Body;
    FString IdempotencyKey;
    FString Tag;
    TArray<FString> OriginalPartNames; // original admission only; never encoded, recovered items leave empty
    int64 MetadataCharge=0; // ephemeral reserved max(live, canonical recovered); never serialized
    EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
    bool bIdempotent = false;
};
class FQueueAdmissionJob;
struct FQueueAdmissionToken;
class FOfflineQueue : public TSharedFromThis<FOfflineQueue,ESPMode::ThreadSafe>
{
public:
    ~FOfflineQueue();
    static void StopAll();
    void StopForShutdown();
    static TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe> Join(
        const TSharedRef<FClientState,ESPMode::ThreadSafe>& Client,FString& OutConfigError);
    void Leave(uint64 ClientId);
    int32 Count() const;
    bool Reserve(int64 BodyBytes,int64 MetadataBytes,FString& OutError);
    void ReleaseReservation(int64 BodyBytes,int64 MetadataBytes);
    void Enqueue(const FVaRestExchangeRef& Exchange,FVaRestOutcomeCallback&& Done);
    bool TryReplay(bool bProbe);
    bool Flush(TUniqueFunction<void(bool)>&& Done);
#if WITH_DEV_AUTOMATION_TESTS
    int32 NumFlushWaitersForTest() const { check(IsInGameThread()); return FlushWaiters.Num(); }
    int64 MetadataBytesForTest() const { check(IsInGameThread()); return CommittedMetadataBytes+ReservedMetadataBytes; }
#endif
    const FString& GetDirectory() const { return Directory; }
    void AcceptCommitted(FQueueItem Item);
    void ScheduleReplay();
private:
    void Load();
    FDateTime UtcNow() const;
    TSharedPtr<FClientState,ESPMode::ThreadSafe> SelectOwner() const;
    void ReplayFinished(FString QueueId,FVaRestQueuedResult Result,bool bKeep);
    void RetireAndNotify(FString QueueId,FVaRestQueuedResult Result);
    void BroadcastLater(FVaRestQueuedResult Result);
    void FinishAdmission(uint64 Ticket);
    struct FFlushWaiter;
    bool PollFlush(const TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>& Waiter);
    struct FFlushWaiter
    {
        uint64 Through=0;
        TUniqueFunction<void(bool)> Done;
        TOptional<bool> Result;
        bool bBarrierStarted=false;
    };
    FString Directory,Origin,AuthCategory;
    FVaRestOfflinePolicy Policy;
    TWeakPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
    FDelegateHandle ConnectivityChanged;
    TMap<uint64,TWeakPtr<FClientState,ESPMode::ThreadSafe>> Members;
    TSharedPtr<IVaRestClock,ESPMode::ThreadSafe> QueueClock;
    FDateTime UtcOrigin;
    double MonotonicOrigin=0;
    int32 ReservedCount=0;
    int64 ReservedBodyBytes=0,CommittedBodyBytes=0,WaitingBodyBytes=0;
    int64 ReservedMetadataBytes=0,CommittedMetadataBytes=0;
    TArray<FQueueItem> Items;
    TArray<TUniqueFunction<void()>> AfterLoad;
    uint64 NextSequence=1,LastAdmission=0;
    TSet<uint64> ActiveAdmissions;
    TArray<TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>> FlushWaiters;
    bool bStopped=false;
    FVaRestTimerHandle ReplayRetryTimer;
    double ReplayNotBefore=0;
    bool bLoaded=false,bReplayActive=false,bStorageFailed=false,bReplayScheduled=false,bProbeReplay=false,bFlushFailed=false;
    friend class FQueueAdmissionJob;
    friend struct FQueueAdmissionToken;
};
FVaRestStageRef MakeOfflineStage();
FString QueueDirectory(FStringView Root, FStringView Name);
bool QueueScope(const FExchangeState& Ex, FString& OutScope);
bool QueueIdentityStillCurrent(const FExchangeState& Ex);
```

`FClientState` gains config QueueName/OfflinePolicy/QueueConfigError, shared queue pointer, and QueuedResult event. `FRequestSpec` and exchange gain bQueueWhenOffline/OnQueued plus `FVaRestTraceQueueReplay QueueReplayOf` and private replay/probe flags. RequestIdentityScope is declared in VaRestIdentity.h with the signature in section 8D and implemented in section 8A. QueueScope hashes Task 1 auth identity + canonical session headers + **explicit request credential override fingerprint** + AuthMode. NoAuth gets an anonymous scope and safe built request headers; explicit unmanaged auth headers cannot persist (Task 3), so they fail eligibility rather than inheriting a public key. Required auth with no usable identity fails Config before disk admission; it does not silently enqueue as anonymous.

- [ ] **Step 1: Add `VaRest.Http.Offline.RegistryAndReservations`.** Two rigs with same name and shared connectivity/root/origin join one queue. Submit two blocked writes under MaxQueued=1: first reserves, second gets Config QueueFull; GetQueuedCount==1 before first commit. Change second base origin or connectivity then reuse name: Config QueueConfig and no file. Names `a/b`, `a_b`, `../a`, case variants and Unicode must produce distinct 64-hex leaves with no slash; exact repeated name shares. A copied client handle must not register a second event listener. Use Task 4 barrier, release on every exit.

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Offline.Registry`.
- [ ] **Step 3: Implement registration and validation.** Registry is leaked GT `TMap<FString,TWeakPtr<FOfflineQueue>>` keyed by normalized root + generated name hash; expired entries replaced. Join checks exact canonical origin, connectivity pointer, policy fields and auth identity contract category (`None`, built-in kind, custom GetAuthName + identity opt-in); conflicts set QueueConfigError on requesting client, leaving existing members untouched. Do not compare live account identity at registration: FIFO item scope checks handle login changes. Registration uses ClientId once, never copied handles. Changing name/root/base/connectivity leaves prior registration and joins a new one; previous Send snapshots may finish through the old queue and its captured membership.

```cpp
bool VaRest::Http::FOfflineQueue::Reserve(int64 BodyBytes,int64 MetadataBytes,FString& OutError)
{
    check(IsInGameThread());
    if (BodyBytes < 0 || BodyBytes > Policy.MaxItemBytes || BodyBytes > Policy.MaxBodyBytes)
    { OutError = TEXT("QueueFull"); return false; }
    if (MetadataBytes<0 || MetadataBytes>256*1024
        || CommittedMetadataBytes+ReservedMetadataBytes>10*1024*1024-MetadataBytes
        || Items.Num() + ReservedCount >= Policy.MaxQueued
        || CommittedBodyBytes + ReservedBodyBytes > Policy.MaxBodyBytes - BodyBytes)
    { OutError = TEXT("QueueFull"); return false; }
    ++ReservedCount;
    ReservedBodyBytes += BodyBytes; ReservedMetadataBytes+=MetadataBytes;
    return true;
}
void VaRest::Http::FOfflineQueue::ReleaseReservation(int64 BodyBytes,int64 MetadataBytes)
{
    check(IsInGameThread());
    check(ReservedCount > 0 && ReservedBodyBytes >= BodyBytes && ReservedMetadataBytes>=MetadataBytes);
    --ReservedCount;
    ReservedBodyBytes -= BodyBytes; ReservedMetadataBytes-=MetadataBytes;
}
FString VaRest::Http::QueueDirectory(FStringView Root, FStringView Name)
{
    return FPaths::Combine(FString(Root), HashFields(TEXT("VaRest/queue-name/v1"), TArray<FString>{FString(Name)}));
}
```

Policy values validate finite positive MaxAge; MaxQueued [1,4096], MaxItemBytes [1,32MiB], MaxBodyBytes≥MaxItemBytes and ≤1GiB. QueueName requires 1..1024 characters, no control characters; do not normalize user text beyond hashing its exact UTF-8. Encoded metadata max64KiB; retained DTO metadata at most256KiB/item and10MiB aggregate, including active reservations, enforced before Prepare and identically during recovery; QueueMetadataCharge retains the reserved maximum of actual live and canonical recovered FString/TArray allocation (including TCHAR width, terminators and reserved capacity); body-free canonical measurement precedes Reserve and every payload copy. This private finite ceiling can refuse metadata-heavy queues before MaxQueued; no acknowledged new record can exceed its restart DTO budget. Tag max256 TCHAR and no NUL/CTL. File sequences never derive from Tag/URL. Additional encoded-disk budget is `ceil(MaxBodyBytes*4/3)+MaxQueued*64KiB`, with one temp-record allowance capped by `ceil(MaxItemBytes*4/3)+64KiB`; reserve encoded bytes during write too. State clearly this is bounded serialized storage, not an exact filesystem allocation guarantee.

The coordinator declaration above is complete; subsequent signature excerpts identify its uses and must not be inserted twice. Add `FVaRestQueuedResultEvent QueuedResult; TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe> Queue; void RefreshQueueRegistration();` to FClientState. Add `TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe> RegisteredQueue;` to FClientConfig. Send pins `Ex.Config->RegisteredQueue` into Ex.Queue immediately, not at a later Resolve or through a GT-only State member; replacing a registration cannot redirect an already-sent admission.

```cpp
namespace VaRest::Http::OfflinePrivate
{
    TMap<FString,TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe>>& Registry()
    { static auto* Values=new TMap<FString,TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe>>(); return *Values; }
    bool SamePolicy(const FVaRestOfflinePolicy& A,const FVaRestOfflinePolicy& B)
    {
        return A.MaxQueued==B.MaxQueued && A.MaxAgeSeconds==B.MaxAgeSeconds
            && A.MaxItemBytes==B.MaxItemBytes && A.MaxBodyBytes==B.MaxBodyBytes;
    }
    bool ValidPolicy(const FVaRestOfflinePolicy& Policy)
    {
        return Policy.MaxQueued>0 && Policy.MaxQueued<=4096 && FMath::IsFinite(Policy.MaxAgeSeconds)
            && Policy.MaxAgeSeconds>0 && Policy.MaxAgeSeconds<=315360000
            && Policy.MaxItemBytes>0 && Policy.MaxItemBytes<=32*1024*1024
            && Policy.MaxBodyBytes>=Policy.MaxItemBytes && Policy.MaxBodyBytes<=1024LL*1024*1024;
    }
}
bool VaRest::Http::RequestIdentityScope(const FClientConfig& Config,const FVaRestSessionData& Session,
    EVaRestAuthMode Mode,const FVaRestHeaders& BuiltHeaders,FString& OutScope)
{
    FString Identity;
    if (!AuthIdentity(Config.Auth.Get(),Mode,Session,Identity)) { OutScope.Reset(); return false; }
    TArray<FString> Fields{Identity,LexToString(static_cast<uint8>(Mode))};
    // Explicit request credentials cannot inherit the owner's anonymous/old-account scope.
    FVaRestHeaders Credentials;
    const FVaRestRedactRules Rules;
    for (const auto& Pair : BuiltHeaders)
        if (Rules.ShouldRedact(Pair.Key,EVaRestRedactKeyKind::Header)) Credentials.Add(Pair.Key,Pair.Value);
    Fields.Append(CanonicalHeaders(Credentials));
    if (Mode!=EVaRestAuthMode::None) Fields.Append(CanonicalHeaders(Session.Headers));
    OutScope=HashFields(TEXT("VaRest/request-scope/v1"),Fields);
    return true;
}
bool VaRest::Http::QueueScope(const FExchangeState& Ex,FString& OutScope)
{ return RequestIdentityScope(*Ex.Config,Ex.State->GetSessionData(),Ex.AuthMode,Ex.RequestHeaders,OutScope); }
bool VaRest::Http::QueueIdentityStillCurrent(const FExchangeState& Ex)
{
    if (Ex.QueueIdentityScope.IsEmpty()) return false;
    const auto Current=Ex.State->GetConfig();
    if (Ex.AuthMode!=EVaRestAuthMode::None && Current->Auth!=Ex.Config->Auth) return false;
    FString Scope,Origin;
    return CanonicalOrigin(Ex.Url,Origin) && Origin==Ex.QueueAnchorOrigin
        && SameOrigin(Current->BaseUrl,Ex.Url)
        && RequestIdentityScope(*Current,Ex.State->GetSessionData(),Ex.AuthMode,Ex.QueueScopeHeaders,Scope)
        && Scope==Ex.QueueIdentityScope;
}
TSharedPtr<VaRest::Http::FOfflineQueue,ESPMode::ThreadSafe> VaRest::Http::FOfflineQueue::Join(
    const TSharedRef<FClientState,ESPMode::ThreadSafe>& Client,FString& OutConfigError)
{
    check(IsInGameThread()); OutConfigError.Reset();
    const auto Config=Client->GetConfig();
    if (Config->QueueName.IsEmpty()) return nullptr;
    FString Canonical;
    if (!OfflinePrivate::ValidPolicy(Config->OfflinePolicy) || Config->QueueName.Len()>1024
        || !IsValidHeaderValue(Config->QueueName) || !CanonicalOrigin(Config->BaseUrl,Canonical)
        || !Config->Connectivity.IsValid()) { OutConfigError=TEXT("QueueConfig"); return nullptr; }
    const FString Path=QueueDirectory(Config->QueueRoot,Config->QueueName);
    const FString Category=Config->Auth.IsValid() ? Config->Auth->GetAuthName().ToString() : FString(TEXT("None"));
    auto& Registry=OfflinePrivate::Registry();
    auto Queue=Registry.FindRef(Path).Pin();
    if (Queue.IsValid())
    {
        if (Queue->Origin!=Canonical || Queue->Connectivity.Pin()!=Config->Connectivity
            || !OfflinePrivate::SamePolicy(Queue->Policy,Config->OfflinePolicy) || Queue->AuthCategory!=Category)
        { OutConfigError=TEXT("QueueConfig"); return nullptr; }
    }
    else
    {
        Queue=MakeShared<FOfflineQueue,ESPMode::ThreadSafe>();
        Queue->Directory=Path; Queue->Origin=Canonical; Queue->Policy=Config->OfflinePolicy;
        Queue->Connectivity=Config->Connectivity; Queue->AuthCategory=Category;
        Queue->QueueClock=Config->Clock; Queue->MonotonicOrigin=Config->Clock->Now(); Queue->UtcOrigin=FDateTime::UtcNow();
        Registry.Add(Path,Queue);
        Queue->Load();
        const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(Queue);
        Queue->ConnectivityChanged=Config->Connectivity->OnChanged().AddLambda([Weak](bool bOnline)
        { if (bOnline) if (const auto Pinned=Weak.Pin()) Pinned->ScheduleReplay(); });
    }
    Queue->Members.Add(Client->GetClientId(),Client);
    Queue->ScheduleReplay();
    return Queue;
}
void VaRest::Http::FOfflineQueue::Leave(uint64 ClientId) { check(IsInGameThread()); Members.Remove(ClientId); }
int32 VaRest::Http::FOfflineQueue::Count() const { check(IsInGameThread()); return Items.Num()+ReservedCount+AfterLoad.Num(); }
FDateTime VaRest::Http::FOfflineQueue::UtcNow() const
{ return UtcOrigin+FTimespan::FromSeconds(FMath::Max(0.0,QueueClock->Now()-MonotonicOrigin)); }
void VaRest::Http::FOfflineQueue::Load()
{
    FQueueStore::Load(Directory,Origin,Policy,[Self=AsShared()](FQueueLoad&& Loaded)
    {
        Self->Items=MoveTemp(Loaded.Items); Self->NextSequence=Loaded.NextSequence;
        Self->bLoaded=true; Self->bStorageFailed=!Loaded.bOk; Self->CommittedBodyBytes=0; Self->CommittedMetadataBytes=0;
        for (const auto& Item : Self->Items)
        { Self->CommittedBodyBytes+=Item.Body.Bytes.Num(); Self->CommittedMetadataBytes+=QueueMetadataCharge(Item); }
        auto Waiting=MoveTemp(Self->AfterLoad);
        for (auto& Start : Waiting) Start();
        Self->ScheduleReplay();
    });
}
void VaRest::Http::FClientState::RefreshQueueRegistration()
{
    check(IsInGameThread());
    if (Queue.IsValid()) Queue->Leave(ClientId);
    FString Error;
    Queue=FOfflineQueue::Join(AsShared(),Error);
    EditConfig([this,&Error](FClientConfig& Config){ Config.QueueConfigError=Error; Config.RegisteredQueue=Queue; });
}
FVaRestClient& FVaRestClient::WithOfflineQueue(FStringView Name,const FVaRestOfflinePolicy& Policy)
{
    check(IsInGameThread());
    if (State.IsValid())
    {
        State->EventSenderTags.Capture();
        const FString OwnedName(Name);
        State->EditConfig([&](VaRest::Http::FClientConfig& Config){ Config.QueueName=OwnedName; Config.OfflinePolicy=Policy; });
        State->RefreshQueueRegistration();
    }
    return *this;
}
int32 FVaRestClient::GetQueuedCount() const
{ check(IsInGameThread()); return State.IsValid() && State->Queue.IsValid() ? State->Queue->Count() : 0; }
bool FVaRestClient::FlushQueue(TUniqueFunction<void(bool)>&& Done)
{
    check(IsInGameThread());
    if (!State.IsValid() || !State->Queue.IsValid()) return false;
    const TWeakPtr<VaRest::Http::FClientState,ESPMode::ThreadSafe> Weak(State);
    return State->Queue->Flush([Weak,Callback=MoveTemp(Done)](bool bOk) mutable
    {
        if (Weak.IsValid() && !VaRest::Http::FClientState::IsShuttingDown() && Callback) Callback(bOk);
    });
}
FVaRestQueuedResultEvent& FVaRestClient::OnQueuedResult()
{ check(IsInGameThread()); check(State.IsValid()); return State->QueuedResult; }
```

The coordinator destructor and shutdown function are:

```cpp
VaRest::Http::FOfflineQueue::~FOfflineQueue()
{
    check(IsInGameThread());
    if (const auto Reachability=Connectivity.Pin()) Reachability->OnChanged().Remove(ConnectivityChanged);
    if (QueueClock.IsValid()) QueueClock->ClearTimer(ReplayRetryTimer);
}
void VaRest::Http::FOfflineQueue::StopForShutdown()
{
    check(IsInGameThread()); bStorageFailed=true; bStopped=true;
    AfterLoad.Reset(); WaitingBodyBytes=0; FlushWaiters.Reset();
    if (QueueClock.IsValid()) QueueClock->ClearTimer(ReplayRetryTimer);
}
void VaRest::Http::FOfflineQueue::StopAll()
{
    check(IsInGameThread());
    TArray<TSharedPtr<FOfflineQueue,ESPMode::ThreadSafe>> Alive;
    for (const auto& Pair : OfflinePrivate::Registry()) if (const auto Queue=Pair.Value.Pin()) Alive.Add(Queue);
    for (const auto& Queue : Alive) Queue->StopForShutdown();
}
```

The complete declaration above includes the destructor and StopAll. When the queue integration lands, extend module PreExit from CancelForShutdown→StopPersistence→FDiskCache::StopControls to CancelForShutdown→FOfflineQueue::StopAll→StopPersistence→FDiskCache::StopControls. ShutdownModule retains the idempotent StopPersistence→FDiskCache::StopControls sequence added by Task 5 before redaction shutdown. Any prepared job callback owns the coordinator only in the GT continuation map. The worker holds item bytes and paths, so final coordinator release remains on GT.

`AfterLoad` is `TArray<TUniqueFunction<void()>>`, bounded to `Policy.MaxQueued` waiting admissions; each owns an interruptible wait and rejects immediately when the bound is reached. Pre-load waiting bodies have a separate MaxBodyBytes cap (WaitingBodyBytes) before any copy into a record. Load may discover an already full durable queue; the pending originals then fail QueueFull at normal Reserve, never become files. The bounded transient is loaded durable bodies plus at most one MaxBodyBytes of already-owned original request bodies, not a promise of one-policy-budget total process memory. Waiting admissions count in GetQueuedCount by adding AfterLoad.Num(), but do not allocate a sequence until the validated load returns. Use a weak delegate handle and remove it in coordinator destruction: add `FDelegateHandle ConnectivityChanged;` and retain the AddLambda return; destructor removes it on GT. This avoids accumulating dead listener slots when queues are repeatedly opened. Same-name empty registries may reuse a coordinator held by an old exchange, but a member rejoin must still satisfy the origin/connectivity contract. Re-run RefreshQueueRegistration after WithBaseUrl, WithConnectivity, WithStorageDirectories and SetAuth/WithoutAuth; no session-token refresh needs a registry change.

- [ ] **Step 4: Add the builder/Resolve checks.** At Send move callbacks/flags. At Resolve include bQueueWhenOffline in its final bMayReissue assignment (section 8C); never rely on an overwritten Send value. Resolve rejects non-idempotent request without Idempotent, streamed body, SaveTo, OnHttpThread, unnamed/misconfigured queue, absolute different-origin URL, custom auth without identity, and unsafe explicit credential header. Preserve bIdempotent, AuthMode and the generated Idempotency-Key. Known-offline nonqueued requests reach cache; refresh/probe/replay flags bypass durable admission. Queue replay must never recurse into Enqueue.
**R3 original-operation anchor:** add `FString QueueIdentityScope, QueueAnchorOrigin; FVaRestHeaders QueueScopeHeaders;` to FExchangeState only. In FClientState::Resolve, after the URL, AuthMode, body/content-type and generated Idempotency-Key are resolved and queue eligibility validated, before returning to Enter/any stage, insert:

```cpp
if (Exchange.bQueueWhenOffline && Exchange.QueueIdentityScope.IsEmpty())
{
    Exchange.QueueScopeHeaders=Exchange.RequestHeaders;
    if (!CanonicalOrigin(Exchange.Url,Exchange.QueueAnchorOrigin) || !QueueScope(Exchange,Exchange.QueueIdentityScope))
    { OutError=TEXT("queue requires an explicit stable auth identity"); return false; }
}
```

This is the only assignment of the original operation's queue scope. Neither Enqueue nor Load/Prepare may refresh it. A replay sets ReplayIdentityScope from the durable item and uses its existing separate gate. At the beginning of OfflineStage after replay/refresh/probe bypass, check `QueueIdentityStillCurrent(Ex)` for opted requests and return LocalFailure(QueueIdentity) on mismatch. At AuthApply immediately before credentials, add the same check guarded by `Ex.bQueueWhenOffline`; this catches account or tenant replacement while queued at the limiter. Ordinary nonqueued live-auth behavior is unchanged. The initial queue registration-load wait, all retry attempts, and a final failed dispatch retain this anchor. A same-identity refresh and unchanged tenant headers still compare equal.

- [ ] **Step 5: Run green and mutation.** ReservedCount removal must make concurrent admission test fail. Conflicting-origin guard removal must produce a failed no-file/no-call assertion. Validate QueueWhenOffline no-name even online, streamed/multipart-file bodies, non-idempotent POST, NoAuth preserved in item scope, GET/PUT/DELETE idempotency and explicit POST Idempotent key. Run Offline.Registry and Builder filters.

#### 8B: Durable record codec and recovery

**Files:** Create `Private/VaRestQueueStore.h/.cpp`, `Private/Tests/VaRestHttpQueueStoreTest.cpp`; modify `Private/VaRestOfflineInternal.h`, `Private/VaRestOffline.cpp`.

**Interfaces:** Consumes FQueueItem, persistence executor, guard and canonical origin. Produces:

```cpp
// namespace VaRest::Http
struct FQueueLoad
{
    TArray<FQueueItem> Items;
    uint64 NextSequence = 1;
    bool bOk = true,bPolicyExceeded=false;
    int64 PeakBodyBytes=0,PeakMetadataBytes=0;
};
TArray<uint8> CopyQueueAdmissionBytes(TConstArrayView<uint8> Bytes); // declaration belongs to VaRestOfflineInternal.h
int64 QueueMetadataCharge(const FQueueItem& Item);
TOptional<int64> MeasureQueueAdmissionMetadata(const FQueueItem& BodyFreeItem,FStringView Origin,
    const FVaRestOfflinePolicy& Policy);
TArray<uint8> EncodeQueueRecord(const FQueueItem& Item);
TValueOrError<FQueueItem,FString> DecodeQueueRecord(TConstArrayView<uint8> Bytes,
    FStringView ExpectedOrigin, const FVaRestOfflinePolicy& Policy,
    int64 AvailableBody=MAX_int64,bool* bCapacityRefused=nullptr);
class FQueueStore
{
public:
    static void Load(FString Directory, FString Origin, FVaRestOfflinePolicy Policy,
        TUniqueFunction<void(FQueueLoad&&)>&& Done);
    static void Prepare(FString Directory, FString Origin, FVaRestOfflinePolicy Policy, FQueueItem Item, FPersistenceGuard Guard,
        TUniqueFunction<void(bool)>&& Done);
    static void Commit(FString Directory,uint64 Sequence,FString QueueId,TUniqueFunction<void(bool)>&& Done);
    static void Abort(FString Directory,uint64 Sequence,FString QueueId);
    static void Retire(FString Directory, uint64 Sequence, FString QueueId,
        TUniqueFunction<void(bool)>&& Done);
};
FString QueueFileName(uint64 Sequence, FStringView QueueId);
FGuid QueueRunId(); // one random GUID per process, all jobs share it
```

Before adding any filename in the Task4 ListFiles visitor, reject names longer than256 TCHARs and a cumulative filename length above524,288 TCHARs or2MiB actual FString allocation, in addition to its8192-entry limit; set bOk=false and stop enumeration. Generated queue/cache leaf names fit these bounds. This bounds listing/index retained data at8MiB charged allowance; the namespace is app-owned and excessive unrelated files conservatively refuse recovery. Count/body/metadata measurements below describe retained DTO bytes plus one bounded candidate, not exact allocator/RSS overhead. Decode's AvailableBody check happens before base64 allocation; hard format cap remains32MiB independently of a smaller current policy. A candidate may contribute at most256KiB metadata on top of the10MiB retained limit; its allowance stays reserved until handback/discard.

**Exact charged recovery peak (R2):** `B + R + 20MiB`, where B=Policy.MaxBodyBytes, R=`4*ceil(32MiB/3)+64KiB`=44,804,780 bytes. The20MiB is10MiB retained DTO metadata +8MiB filename/index payload and container allowance +2MiB current candidate/metadata parser workspace. B includes all retained decoded bodies **and the current decoded candidate**; AvailableBody prevents allocating another body beyond it. The candidate's≤256KiB metadata is inside the2MiB workspace until moved into the10MiB retained account. The split owns one complete encoded input R and at most64KiB metadata (moved into ParseOwned), never a full-body FString or cloned full-body DOM. The2MiB also covers metadata UTF8/temporary TCHAR conversions, bounded node/member/index/parser scratch (≤2048 lexical units; landed FNode is32 bytes and arena chunks≤256KiB), and small codec bookkeeping. The8MiB covers≤8192 filenames with≤524288 total TCHARs, numeric/id duplicate maps, sorted name DTOs and retired names; move input-independent containers only, and do not introduce a second retained filename/index snapshot. Actual FString/TArray allocated sizes, not character counts, feed QueueMetadataCharge; allocator headers/fragmentation and unrelated caller/request buffers are outside this logical allocation charge and are not claimed as RSS. At the default B=67,108,864, total=132,885,164 bytes, below134,217,728 by1,332,564 bytes. Larger custom B can be conservatively refused by executor admission without callback loss or file deletion. Prepare reserves max(full-guard workspace, twice the immutable body + encoded-record cap +2MiB workspace): the second body belongs to roundtrip schema/hash validation and is destroyed before WriteNewFile. Its metadata-only snapshot copies no raw body. The body codec uses the same split/schema/size rules. JSON guard workspace is conservatively8×body+2MiB; executor admission can refuse a large JSON/formatted-JSON candidate, while a32MiB binary candidate uses2×32MiB+R+2MiB=114,010,796 bytes. This is a finite availability tradeoff, not a silently truncated scan. New admission reserves aggregate metadata before Prepare; accepted commit transfers that charge to committed metadata, and retirement releases it. Transient original request bodies during load remain separately documented/limited by WaitingBodyBytes; they are not worker-return DTOs. During Prepare the coordinator admission job retains at most one body per reserved operation (ReservedBodyBytes≤Policy.MaxBodyBytes); the exchange may retain its original body until terminal delivery. The worker copy is included in the executor reservation above. These request/coordinator accounts are additional to that128MiB worker/result account, not hidden inside its claim.

Load never hands unvalidated text to FName, FPaths, URLs, auth or callbacks. Bad record deletion does not create an event with an attacker-provided QueueId/Tag. A structurally valid record with mismatched current scope is retained for Task 8's Auth result. Expiry occurs on GT after load using queue's clock/UTC anchor, and generates a real bounded result through the coordinator.

- [ ] **Step 1: Add `VaRest.Http.QueueStore.RoundTripAndReject`.**

```cpp
using namespace VaRest::Http;
FQueueItem Item;
Item.QueueId = FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
Item.Sequence = 9007199254740993ULL; // greater than exact double integer range
Item.OriginalRun = QueueRunId();
Item.OriginalRequestId = 123;
Item.CreatedUtc = FDateTime(2026,10,23,12,0,0);
Item.Scope = HashFields(TEXT("test-scope"), TArray<FString>{TEXT("A")});
Item.Verb = TEXT("POST");
Item.Url = TEXT("https://api.test:443/items");
Item.Headers.Set(TEXT("Content-Type"), TEXT("application/octet-stream"));
Item.IdempotencyKey = TEXT("stable-operation-id");
Item.Headers.Set(TEXT("Idempotency-Key"), Item.IdempotencyKey);
Item.Body = FVaRestRequestBody::FromBytes(TArray<uint8>{0,255,1}, EVaRestBodyKind::Binary, TEXT("application/octet-stream"));
Item.Tag = TEXT("Upload");
Item.AuthMode = EVaRestAuthMode::None;
Item.bIdempotent = true;
const auto Encoded = EncodeQueueRecord(Item);
auto Decoded = DecodeQueueRecord(Encoded, TEXT("https://api.test:443"), FVaRestOfflinePolicy());
if (!TestTrue(TEXT("round trip"), Decoded.HasValue())) return false;
const FQueueItem& Read = Decoded.GetValue();
TestEqual(TEXT("uint64 sequence is exact"), Read.Sequence, Item.Sequence);
TestTrue(TEXT("binary bytes exact"), Read.Body.Bytes == Item.Body.Bytes);
TestTrue(TEXT("NoAuth preserved"), Read.AuthMode == EVaRestAuthMode::None);
TestEqual(TEXT("idempotency stable"), Read.IdempotencyKey, Item.IdempotencyKey);
TestTrue(TEXT("wrong origin refuses"), DecodeQueueRecord(Encoded, TEXT("https://other.test:443"), FVaRestOfflinePolicy()).HasError());
TArray<uint8> Truncated = Encoded;
Truncated.SetNum(Encoded.Num() / 2);
TestTrue(TEXT("truncated JSON refuses"), DecodeQueueRecord(Truncated, TEXT("https://api.test:443"), FVaRestOfflinePolicy()).HasError());
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.QueueStore`.
- [ ] **Step 3: Implement the encoder and strict schema.**

The body remains the same canonical JSON base64 string, but the queue codec never materializes it as FString or a body-bearing JSON tree. Place this helper before EncodeQueueRecord in VaRestQueueStore.cpp:

```cpp
namespace VaRest::Http::QueueStorePrivate
{
    struct FRecordParts { int32 BodyStart=0,BodyLength=0; TArray<uint8> Metadata; };
    bool White(uint8 Byte) { return Byte==' ' || Byte=='\t' || Byte=='\r' || Byte=='\n'; }
    bool SplitRecord(TConstArrayView<uint8> Bytes,FRecordParts& Out)
    {
        int32 Depth=0,Units=0,BodyStart=INDEX_NONE,BodyEnd=INDEX_NONE;
        for (int32 Index=0;Index<Bytes.Num();)
        {
            const uint8 Byte=Bytes[Index];
            if (White(Byte)) { ++Index; continue; }
            if (++Units>2048) return false; // strings + nonwhitespace punctuation/primitive bytes
            if (Byte=='"')
            {
                const int32 Start=++Index;
                bool bEscaped=false;
                while (Index<Bytes.Num() && Bytes[Index]!='"')
                {
                    if (Bytes[Index]=='\\') { bEscaped=true; Index+=2; }
                    else ++Index;
                }
                if (Index>=Bytes.Num()) return false;
                const int32 Length=Index-Start; ++Index;
                int32 Colon=Index; while (Colon<Bytes.Num() && White(Bytes[Colon])) ++Colon;
                const bool bBodyKey=Depth==1 && !bEscaped && Length==4
                    && Bytes[Start]=='b' && Bytes[Start+1]=='o' && Bytes[Start+2]=='d' && Bytes[Start+3]=='y'
                    && Colon<Bytes.Num() && Bytes[Colon]==':';
                if (!bBodyKey) continue;
                if (BodyStart!=INDEX_NONE) return false;
                int32 Value=Colon+1; while (Value<Bytes.Num() && White(Bytes[Value])) ++Value;
                if (Value>=Bytes.Num() || Bytes[Value]!='"') return false;
                BodyStart=++Value;
                while (Value<Bytes.Num() && Bytes[Value]!='"')
                {
                    const uint8 Code=Bytes[Value];
                    if (!((Code>='A' && Code<='Z') || (Code>='a' && Code<='z')
                        || (Code>='0' && Code<='9') || Code=='+' || Code=='/' || Code=='=')) return false;
                    ++Value;
                }
                if (Value>=Bytes.Num()) return false;
                BodyEnd=Value; Index=Value+1; continue;
            }
            if (Byte=='{' || Byte=='[') { if (++Depth>8) return false; }
            else if (Byte=='}' || Byte==']') { if (--Depth<0) return false; }
            ++Index;
        }
        if (Depth!=0 || BodyStart==INDEX_NONE || Bytes.Num()-(BodyEnd-BodyStart)>65536) return false;
        Out.BodyStart=BodyStart; Out.BodyLength=BodyEnd-BodyStart;
        Out.Metadata.Reserve(Bytes.Num()-Out.BodyLength+1);
        Out.Metadata.Append(Bytes.GetData(),BodyStart);
        Out.Metadata.Append(Bytes.GetData()+BodyEnd,Bytes.Num()-BodyEnd);
        return true; // strict JSON parser below validates the complete remaining grammar
    }
}
```

Only an unescaped top-level `body` key with an unescaped canonical base64 value is accepted; escaped key/value spellings are conservatively refused. Escaped quotes or strings containing the text body elsewhere never delimit the payload. Schema validation requires exactly the18 known unique top-level keys, so escaped duplicate aliases and extra body fields cannot evade this split. Non-body metadata is≤64KiB, lexical units≤2048 and nesting≤8 before JSON allocation. Base64 alphabet/padding/canonicality are checked below, not assumed from the split.

```cpp
TArray<uint8> VaRest::Http::EncodeQueueRecord(const FQueueItem& Item)
{
    if (Item.Body.Bytes.Num()>32*1024*1024 || QueueMetadataCharge(Item)>256*1024 || Item.Headers.Num()>128) return {};
    FVaRestJson Root = FVaRestJson::Object();
    Root.Set(TEXT("version"), 1);
    Root.Set(TEXT("queueId"), Item.QueueId);
    Root.Set(TEXT("sequence"), LexToString(Item.Sequence));
    Root.Set(TEXT("runId"), Item.OriginalRun.ToString(EGuidFormats::DigitsLower));
    Root.Set(TEXT("originalRequestId"), LexToString(Item.OriginalRequestId));
    Root.Set(TEXT("createdUtc"), Item.CreatedUtc.ToIso8601());
    Root.Set(TEXT("scope"), Item.Scope);
    Root.Set(TEXT("verb"), Item.Verb);
    Root.Set(TEXT("url"), Item.Url);
    Root.Set(TEXT("authMode"), static_cast<int32>(Item.AuthMode));
    Root.Set(TEXT("idempotent"), Item.bIdempotent);
    Root.Set(TEXT("idempotencyKey"), Item.IdempotencyKey);
    Root.Set(TEXT("tag"), Item.Tag);
    Root.Set(TEXT("bodyKind"), static_cast<int32>(Item.Body.Kind));
    Root.Set(TEXT("contentType"), Item.Body.ContentType);
    Root.Set(TEXT("body"), TEXT(""));
    Root.Set(TEXT("bodySha256"), FSha256::Hash(Item.Body.Bytes).ToHex());
    FVaRestJson Headers = FVaRestJson::Array();
    for (const auto& Pair : Item.Headers) Headers.Add(FVaRestJson::Array(Pair.Key, Pair.Value));
    Root.Set(TEXT("headers"), Headers);
    TArray<uint8> Metadata;
    if (Root.TryToUtf8(Metadata).HasError() || Metadata.Num()>65536) return {};
    QueueStorePrivate::FRecordParts Parts;
    if (!QueueStorePrivate::SplitRecord(Metadata,Parts) || Parts.BodyLength!=0) return {};
    const uint32 EncodedLength=FBase64::GetEncodedDataSize(Item.Body.Bytes.Num());
    TArray<uint8> Bytes; Bytes.SetNumUninitialized(Metadata.Num()+EncodedLength);
    FMemory::Memcpy(Bytes.GetData(),Metadata.GetData(),Parts.BodyStart);
    FBase64::Encode(Item.Body.Bytes.GetData(),Item.Body.Bytes.Num(),reinterpret_cast<UTF8CHAR*>(Bytes.GetData()+Parts.BodyStart));
    FMemory::Memcpy(Bytes.GetData()+Parts.BodyStart+EncodedLength,Metadata.GetData()+Parts.BodyStart,Metadata.Num()-Parts.BodyStart);
    return Bytes;
}
FString VaRest::Http::QueueFileName(uint64 Sequence, FStringView QueueId)
{
    return FString::Printf(TEXT("%020llu-%s.json"), Sequence, *FString(QueueId));
}
```

Decode requires version1 object, strings for all string fields, bool for idempotent, integral AuthMode 0..2 and BodyKind only None/Json/Text/Form/Multipart/Binary; no Stream. Strict decimal sequence/original ID parser rejects sign/whitespace/overflow/empty and accepts up to MAX_uint64 (separate from TTL ReadDeltaSeconds). GUIDs exactly32 hex, nonzero sequence, scope64 lowercase hex, URL≤8192 chars and actual same-origin, tag≤256 no control/NUL, verb one of GET/HEAD/PUT/DELETE/OPTIONS/TRACE or bIdempotent=true for POST/PATCH/other valid HTTP token. Any explicit idempotent unsafe verb requires nonempty valid Idempotency-Key equal to the safe headers' field. Headers≤128 and metadata≤64KiB; duplicate request header names refused, name/value validity checked; deny secret/hop-by-hop fields with built-in rules even if current client's guard is absent at load. Body base64 length bounded **before** decode; body size≤MaxItemBytes; digest must match; ContentType must match the header when one exists. Reconstruct ContentLength from actual bytes and leave no FilePath/Segments/PartDescriptors. Recovery validates finite schema, digest/base64, actual URI/origin and header validity/forbidden credential fields, then treats body bytes as opaque. Full JSON/form/key/value/current+snapshot secret scanning runs before Prepare and again in GuardReplay before any wire; a recovered body that now matches a known secret is refused there. Multipart names are checked from bounded original names before save; loaded multipart has the stated raw known-value boundary.

The queue decoder is independent of the cache codec (no private cross-cpp helper dependency). Put these helpers in `namespace VaRest::Http::QueueStorePrivate` in VaRestQueueStore.cpp, and include VaRestJson.h, VaRestSha256.h, VaRestUrl.h, VaRestPersistenceGuard.h, Misc/Base64.h, HAL/PlatformFileManager.h:

```cpp
namespace VaRest::Http::QueueStorePrivate
{
    bool Decimal(FStringView Text,uint64& Out)
    {
        if (Text.IsEmpty()) return false;
        uint64 Value=0;
        for (TCHAR Character : Text)
        {
            if (Character<TEXT('0') || Character>TEXT('9')) return false;
            const uint64 Digit=Character-TEXT('0');
            if (Value>(MAX_uint64-Digit)/10) return false;
            Value=Value*10+Digit;
        }
        Out=Value; return true;
    }
    bool Hex(FStringView Text,int32 Length)
    {
        if (Text.Len()!=Length) return false;
        for (TCHAR Character : Text)
            if (!((Character>=TEXT('0') && Character<=TEXT('9')) || (Character>=TEXT('a') && Character<=TEXT('f')))) return false;
        return true;
    }
    bool Plain(FStringView Text,int32 Limit)
    {
        if (Text.Len()>Limit) return false;
        for (TCHAR Character : Text) if (Character<32 || Character==127) return false;
        return true;
    }
    bool SafeVerb(FStringView Verb)
    {
        for (const TCHAR* Allowed : {TEXT("GET"),TEXT("HEAD"),TEXT("PUT"),TEXT("DELETE"),TEXT("OPTIONS"),TEXT("TRACE")})
            if (Verb==Allowed) return true;
        return false;
    }
    int64 RecordLimit(const FVaRestOfflinePolicy& Policy) { return ((Policy.MaxItemBytes+2)/3)*4+65536; }
    bool FileIdentity(FStringView Name,uint64& Sequence,FString& Id,FStringView Extension)
    {
        const FString Text(Name);
        if (Text.Len()!=53+Extension.Len() || Text[20]!=TEXT('-') || !Text.EndsWith(FString(Extension),ESearchCase::CaseSensitive)) return false;
        Id=Text.Mid(21,32);
        FGuid Parsed;
        return Decimal(Text.Left(20),Sequence) && Sequence!=0 && Hex(Id,32)
            && FGuid::ParseExact(Id,EGuidFormats::Digits,Parsed) && Parsed.IsValid();
    }
}
TValueOrError<VaRest::Http::FQueueItem,FString> VaRest::Http::DecodeQueueRecord(
    TConstArrayView<uint8> Bytes,FStringView ExpectedOrigin,const FVaRestOfflinePolicy& Policy,
    int64 AvailableBody,bool* bCapacityRefused)
{
    if (bCapacityRefused) *bCapacityRefused=false;
    const auto Refuse=[](){ return MakeError(FString(TEXT("QueueRecord"))); };
    if (Bytes.Num()>QueueStorePrivate::RecordLimit(Policy)) return Refuse();
    QueueStorePrivate::FRecordParts Parts;
    if (!QueueStorePrivate::SplitRecord(Bytes,Parts)) return Refuse();
    FVaRestJsonParseOptions Options; Options.MaxInputBytes=65536; Options.MaxDepth=8;
    auto Parsed=FVaRestJson::ParseOwned(MoveTemp(Parts.Metadata),Options);
    if (Parsed.HasError()) return Refuse();
    const auto Root=Parsed.StealValue();
    if (!Root.IsObject() || Root.Num()!=18 || !Root[TEXT("version")].IsNumber() || Root[TEXT("version")].AsDouble()!=1.0) return Refuse();
    const TArray<FString> Allowed{TEXT("version"),TEXT("queueId"),TEXT("sequence"),TEXT("runId"),TEXT("originalRequestId"),
        TEXT("createdUtc"),TEXT("scope"),TEXT("verb"),TEXT("url"),TEXT("authMode"),TEXT("idempotent"),TEXT("idempotencyKey"),
        TEXT("tag"),TEXT("bodyKind"),TEXT("contentType"),TEXT("body"),TEXT("bodySha256"),TEXT("headers")};
    TSet<FString> Seen;
    for (const auto& Member : Root.Members())
    {
        const FString Key=Member.GetKey();
        if (!Allowed.ContainsByPredicate([&Key](const FString& Candidate){ return Candidate.Equals(Key,ESearchCase::CaseSensitive); })
            || Seen.Contains(Key)) return Refuse();
        Seen.Add(Key);
    }
    for (const TCHAR* Field : {TEXT("queueId"),TEXT("sequence"),TEXT("runId"),TEXT("originalRequestId"),
        TEXT("createdUtc"),TEXT("scope"),TEXT("verb"),TEXT("url"),TEXT("idempotencyKey"),TEXT("tag"),
        TEXT("contentType"),TEXT("body"),TEXT("bodySha256")}) if (!Root[Field].IsString()) return Refuse();
    const auto Mode=Root[TEXT("authMode")].TryGet<uint8>();
    const auto Kind=Root[TEXT("bodyKind")].TryGet<uint8>();
    if (!Mode.IsSet() || *Mode>static_cast<uint8>(EVaRestAuthMode::None)
        || !Kind.IsSet() || *Kind>static_cast<uint8>(EVaRestBodyKind::Binary)
        || !Root[TEXT("idempotent")].IsBool()) return Refuse();
    FQueueItem Item;
    Item.QueueId=Root[TEXT("queueId")].AsString();
    FGuid Id;
    if (!QueueStorePrivate::Hex(Item.QueueId,32) || !FGuid::ParseExact(Item.QueueId,EGuidFormats::Digits,Id) || !Id.IsValid()
        || !QueueStorePrivate::Decimal(Root[TEXT("sequence")].AsString(),Item.Sequence) || Item.Sequence==0
        || !QueueStorePrivate::Decimal(Root[TEXT("originalRequestId")].AsString(),Item.OriginalRequestId)) return Refuse();
    const FString Run=Root[TEXT("runId")].AsString();
    if (!QueueStorePrivate::Hex(Run,32) || !FGuid::ParseExact(Run,EGuidFormats::Digits,Item.OriginalRun)
        || !Item.OriginalRun.IsValid() || !FDateTime::ParseIso8601(*Root[TEXT("createdUtc")].AsString(),Item.CreatedUtc)) return Refuse();
    Item.Scope=Root[TEXT("scope")].AsString();
    Item.Verb=Root[TEXT("verb")].AsString(); Item.Url=Root[TEXT("url")].AsString();
    Item.Tag=Root[TEXT("tag")].AsString(); Item.IdempotencyKey=Root[TEXT("idempotencyKey")].AsString();
    Item.AuthMode=static_cast<EVaRestAuthMode>(*Mode); Item.bIdempotent=Root[TEXT("idempotent")].AsBool();
    if (!QueueStorePrivate::Hex(Item.Scope,64) || !QueueStorePrivate::Plain(Item.Tag,256)
        || !QueueStorePrivate::Plain(Item.Url,8192) || !SameOrigin(Item.Url,ExpectedOrigin)
        || !IsValidHeaderName(Item.Verb) || (!QueueStorePrivate::SafeVerb(Item.Verb) && !Item.bIdempotent)
        || !QueueStorePrivate::Plain(Item.IdempotencyKey,4096) || !IsValidHeaderValue(Item.IdempotencyKey)) return Refuse();
    const auto Headers=Root[TEXT("headers")];
    if (!Headers.IsArray() || Headers.Num()>128) return Refuse();
    int64 HeaderChars=0;
    for (const auto& Pair : Headers.Elements())
    {
        if (!Pair.IsArray() || Pair.Num()!=2 || !Pair[0].IsString() || !Pair[1].IsString()) return Refuse();
        const FString Name=Pair[0].AsString(),Value=Pair[1].AsString();
        if (Name.Len()>256 || Value.Len()>8192 || HeaderChars+Name.Len()+Value.Len()>32768
            || !IsValidHeaderName(Name) || !IsValidHeaderValue(Value) || Item.Headers.Contains(Name)) return Refuse();
        HeaderChars+=Name.Len()+Value.Len(); Item.Headers.Add(Name,Value);
    }
    FPersistenceGuard Structural;
    if (Structural.SafeHeaders(Item.Headers).Num()!=Item.Headers.Num() || !Structural.AllowsUrl(Item.Url)) return Refuse();
    if (Item.bIdempotent && !QueueStorePrivate::SafeVerb(Item.Verb)
        && (Item.IdempotencyKey.IsEmpty() || Item.Headers.Get(TEXT("Idempotency-Key"))!=Item.IdempotencyKey)) return Refuse();
    const FString ContentType=Root[TEXT("contentType")].AsString();
    if (!QueueStorePrivate::Plain(ContentType,1024) || !IsValidHeaderValue(ContentType)
        || (Item.Headers.Contains(TEXT("Content-Type")) && Item.Headers.Get(TEXT("Content-Type"))!=ContentType)) return Refuse();
    const UTF8CHAR* Encoded=reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()+Parts.BodyStart);
    const uint32 EncodedLength=Parts.BodyLength;
    if (EncodedLength%4!=0 || EncodedLength>FBase64::GetEncodedDataSize(Policy.MaxItemBytes)) return Refuse();
    const uint32 DecodedLength=FBase64::GetDecodedDataSize(Encoded,EncodedLength);
    if (DecodedLength>AvailableBody)
    { if (bCapacityRefused) *bCapacityRefused=true; return Refuse(); }
    if (DecodedLength>Policy.MaxItemBytes) return Refuse();
    // Strict padding/canonical low bits; the engine decoder alone accepts some noncanonical encodings.
    const auto Digit=[](UTF8CHAR Code)->int32
    {
        if (Code>='A' && Code<='Z') return Code-'A';
        if (Code>='a' && Code<='z') return Code-'a'+26;
        if (Code>='0' && Code<='9') return Code-'0'+52;
        return Code=='+' ? 62 : (Code=='/' ? 63 : -1);
    };
    uint32 Padding=0;
    if (EncodedLength>0 && Encoded[EncodedLength-1]=='=') ++Padding;
    if (EncodedLength>1 && Encoded[EncodedLength-2]=='=') ++Padding;
    for (uint32 Index=0;Index<EncodedLength-Padding;++Index) if (Digit(Encoded[Index])<0) return Refuse();
    if ((Padding==2 && (Digit(Encoded[EncodedLength-3])&15)!=0)
        || (Padding==1 && (Digit(Encoded[EncodedLength-2])&3)!=0)) return Refuse();
    TArray<uint8> Body; Body.SetNumUninitialized(DecodedLength);
    if ((EncodedLength>0 && !FBase64::Decode(Encoded,EncodedLength,Body.GetData()))
        || FSha256::Hash(Body).ToHex()!=Root[TEXT("bodySha256")].AsString()) return Refuse();
    if (*Kind==static_cast<uint8>(EVaRestBodyKind::None) && !Body.IsEmpty()) return Refuse();
    Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(Body),static_cast<EVaRestBodyKind>(*Kind),ContentType);
    return MakeValue(MoveTemp(Item));
}
FGuid VaRest::Http::QueueRunId()
{
    static const FGuid Id=FGuid::NewGuid(); return Id;
}
```

The checked landed enum order is Required=0, IfAvailable=1, None=2. Unknown modes are refused, never coerced to Required.

Queue file path is reconstructed from validated filename sequence/id and must match decoded fields. Never trust an internal path/origin supplied by JSON. Load sorts numeric sequence, detects duplicate QueueId/sequence, rejects every conflicting identity as corruption, and sets NextSequence=max+1; MAX_uint64 exhaustion rejects new writes Config, never wraps. Recovery enforces count/body/metadata caps before retaining each DTO. A smaller reopened policy refuses the complete queue as Config/storage-unavailable and preserves acknowledged records; restore adequate limits and reopen to recover. It never deletes valid overflow or replays a partial prefix. Malformed and duplicate files remain separately handled as corruption. New admissions wait behind Load rather than racing its sequence allocation.

`Prepare`: worker validates entire item (origin/scope/header/body) plus snapshot/current guard, including Tag and all metadata strings, **before writing any .part bytes**; encode then enforce encoded size cap; WriteNewFile writes immutable absent path. Callback is GT and returns false for rejected/written-failed record. No credentials appear even in a failed temp write.

`Retire`: rename `<seq>-<id>.json` to `<seq>-<id>.done` on worker before reporting success; the destination cannot already belong to another item. Existing .done means already retired; it wins over a same-id .json on recovery. Delete .done opportunistically after a successful retirement and no live .json; failed deletion leaves a tombstone recognized on next load. A crash between server outcome and rename may replay (at-least-once), a crash after rename does not. Directory fsync is not portable through the generic API: Flush(true)+rename protects complete-file visibility, not a claim of power-loss transactional filesystem durability.

The complete worker store methods are below. `AsyncBool` keeps the caller's callback in a GT box even when admission fails; every refusal answers false instead of leaving a hold parked.

```cpp
namespace VaRest::Http::QueueStorePrivate
{
    void AsyncBool(FIoWork&& Work,TUniqueFunction<void(bool)>&& Done,int64 Reservation=0,bool bControlDuringStop=false)
    {
        auto Callback=MakeShared<TUniqueFunction<void(bool)>,ESPMode::ThreadSafe>(MoveTemp(Done));
        if (!LaunchPersistence(MoveTemp(Work),[Callback](FIoResult&& Result){ (*Callback)(Result.bOk); },Reservation,bControlDuringStop))
            (*Callback)(false);
    }
    FString PendingPath(const FString& Directory,uint64 Sequence,const FString& Id)
    { return FPaths::ChangeExtension(FPaths::Combine(Directory,QueueFileName(Sequence,Id)),TEXT("pending")); }
}
void VaRest::Http::FQueueStore::Prepare(FString Directory,FString Origin,FVaRestOfflinePolicy Policy,
    FQueueItem Item,FPersistenceGuard Guard,TUniqueFunction<void(bool)>&& Done)
{
    const int64 Reservation=FMath::Max<int64>(
        PersistenceGuardReservation(Item.Body.Kind,Item.Body.ContentType,Item.Body.Bytes.Num()),
        2LL*Item.Body.Bytes.Num()+QueueStorePrivate::RecordLimit(Policy)+2*1024*1024);
    QueueStorePrivate::AsyncBool([Directory=MoveTemp(Directory),Origin=MoveTemp(Origin),Policy,
        Item=MoveTemp(Item),Guard=MoveTemp(Guard)]() mutable
    {
        FIoResult Refused;
        for (const FString& Name : Item.OriginalPartNames)
            if (Guard.Rules.ShouldRedact(Name,EVaRestRedactKeyKind::Part)) return Refused;
        if (!Guard.AllowsUrl(Item.Url) || !Guard.AllowsBody(Item.Body.Kind,Item.Body.ContentType,Item.Body.Bytes)
            || Guard.SafeHeaders(Item.Headers).Num()!=Item.Headers.Num()) return Refused;
        // Scan every serialized metadata value and raw bytes before base64 can hide a known value.
        FQueueItem Metadata;
        Metadata.QueueId=Item.QueueId; Metadata.Sequence=Item.Sequence; Metadata.OriginalRun=Item.OriginalRun;
        Metadata.OriginalRequestId=Item.OriginalRequestId; Metadata.CreatedUtc=Item.CreatedUtc;
        Metadata.Scope=Item.Scope; Metadata.Verb=Item.Verb; Metadata.Url=Item.Url; Metadata.Headers=Item.Headers;
        Metadata.IdempotencyKey=Item.IdempotencyKey; Metadata.Tag=Item.Tag; Metadata.AuthMode=Item.AuthMode;
        Metadata.bIdempotent=Item.bIdempotent;
        Metadata.Body=FVaRestRequestBody::FromBytes(TArray<uint8>(),Item.Body.Kind,Item.Body.ContentType);
        const auto MetaBytes=EncodeQueueRecord(Metadata);
        if (MetaBytes.IsEmpty() || MetaBytes.Num()>65536 || Guard.ContainsSecret(MetaBytes)) return Refused;
        const auto Bytes=EncodeQueueRecord(Item);
        if (Bytes.IsEmpty() || Bytes.Num()>QueueStorePrivate::RecordLimit(Policy)
            || DecodeQueueRecord(Bytes,Origin,Policy).HasError()) return Refused;
        return WriteNewFile(QueueStorePrivate::PendingPath(Directory,Item.Sequence,Item.QueueId),Bytes);
    },MoveTemp(Done),Reservation);
}
void VaRest::Http::FQueueStore::Commit(FString Directory,uint64 Sequence,FString QueueId,
    TUniqueFunction<void(bool)>&& Done)
{
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    QueueStorePrivate::AsyncBool([Directory=MoveTemp(Directory),Sequence,QueueId=MoveTemp(QueueId)
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeRename);
#endif
        auto& Platform=FPlatformFileManager::Get().GetPlatformFile();
        const FString From=QueueStorePrivate::PendingPath(Directory,Sequence,QueueId);
        const FString To=FPaths::Combine(Directory,QueueFileName(Sequence,QueueId));
        FIoResult Result;
#if WITH_DEV_AUTOMATION_TESTS
        const bool bInjectedFailure=Hooks.IsValid() && Hooks->bFailCommit;
#else
        const bool bInjectedFailure=false;
#endif
        Result.bOk=!bInjectedFailure && !Platform.FileExists(*To) && Platform.MoveFile(*To,*From);
        if (!Result.bOk) Platform.DeleteFile(*From);
        return Result;
    },MoveTemp(Done),0,true);
}
void VaRest::Http::FQueueStore::Abort(FString Directory,uint64 Sequence,FString QueueId)
{
    LaunchPersistence([Path=QueueStorePrivate::PendingPath(Directory,Sequence,QueueId)]
        { return VaRest::Http::DeleteFile(Path); },[](FIoResult&&){});
}
void VaRest::Http::FQueueStore::Retire(FString Directory,uint64 Sequence,FString QueueId,
    TUniqueFunction<void(bool)>&& Done)
{
    QueueStorePrivate::AsyncBool([Directory=MoveTemp(Directory),Sequence,QueueId=MoveTemp(QueueId)]
    {
        auto& Platform=FPlatformFileManager::Get().GetPlatformFile();
        const FString From=FPaths::Combine(Directory,QueueFileName(Sequence,QueueId));
        const FString Tombstone=FPaths::ChangeExtension(From,TEXT("done"));
        FIoResult Result;
        Result.bOk=Platform.FileExists(*Tombstone) || (!Platform.FileExists(*From)) || Platform.MoveFile(*Tombstone,*From);
        if (Result.bOk && !Platform.FileExists(*From)) Platform.DeleteFile(*Tombstone);
        return Result;
    },MoveTemp(Done));
}
int64 VaRest::Http::QueueMetadataCharge(const FQueueItem& Item)
{
    int64 Bytes=sizeof(FQueueItem)+Item.QueueId.GetAllocatedSize()+Item.Scope.GetAllocatedSize()
        +Item.Verb.GetAllocatedSize()+Item.Url.GetAllocatedSize()+Item.IdempotencyKey.GetAllocatedSize()
        +Item.Tag.GetAllocatedSize()+Item.Body.ContentType.GetAllocatedSize()+Item.Headers.GetPairs().GetAllocatedSize();
    for (const auto& Header : Item.Headers) Bytes+=Header.Key.GetAllocatedSize()+Header.Value.GetAllocatedSize();
    Bytes+=Item.OriginalPartNames.GetAllocatedSize();
    for (const FString& Name : Item.OriginalPartNames) Bytes+=Name.GetAllocatedSize();
    return FMath::Max(Bytes,Item.MetadataCharge); // copying a DTO can compact arrays; never reduce its reserved charge
}
TOptional<int64> VaRest::Http::MeasureQueueAdmissionMetadata(const FQueueItem& BodyFreeItem,
    FStringView Origin,const FVaRestOfflinePolicy& Policy)
{
    check(BodyFreeItem.Body.Bytes.IsEmpty());
    const int64 LiveCharge=QueueMetadataCharge(BodyFreeItem);
    if (LiveCharge>256*1024) return {};
    const auto Encoded=EncodeQueueRecord(BodyFreeItem);
    if (Encoded.IsEmpty() || Encoded.Num()>65536) return {};
    auto Recovered=DecodeQueueRecord(Encoded,Origin,Policy,0);
    if (Recovered.HasError()) return {};
    const int64 Charge=FMath::Max(LiveCharge,QueueMetadataCharge(Recovered.GetValue()));
    return Charge<=256*1024 ? TOptional<int64>(Charge) : TOptional<int64>();
}
void VaRest::Http::FQueueStore::Load(FString Directory,FString Origin,FVaRestOfflinePolicy Policy,
    TUniqueFunction<void(FQueueLoad&&)>&& Done)
{
    auto Callback=MakeShared<TUniqueFunction<void(FQueueLoad&&)>,ESPMode::ThreadSafe>(MoveTemp(Done));
    FVaRestOfflinePolicy HardPolicy=Policy; HardPolicy.MaxItemBytes=32*1024*1024;
#if WITH_DEV_AUTOMATION_TESTS
    const auto Hooks=Testing::PersistenceHooksFor(Directory);
#endif
    const int64 Reservation=Policy.MaxBodyBytes+20*1024*1024
        +QueueStorePrivate::RecordLimit(HardPolicy);
    if (!LaunchPersistenceResult<FQueueLoad>([Directory=MoveTemp(Directory),Origin=MoveTemp(Origin),Policy,HardPolicy
#if WITH_DEV_AUTOMATION_TESTS
        ,Hooks
#endif
    ]
    {
#if WITH_DEV_AUTOMATION_TESTS
        if (Hooks.IsValid()) Testing::WaitPersistenceBarrier(Hooks->BeforeQueueLoad);
#endif
        FQueueLoad Result;
        auto Listing=ListFiles(Directory);
        if (!Listing.bOk) { Result.bOk=false; return Result; }
        struct FNameRecord { FString Name,Id; uint64 Sequence=0; };
        TArray<FNameRecord> Names;
        TMap<FString,int32> IdCounts; TMap<uint64,int32> SequenceCounts;
        TSet<FString> Retired;
        for (const FString& Name : Listing.Names)
        {
            uint64 Sequence=0; FString Id;
            if (QueueStorePrivate::FileIdentity(Name,Sequence,Id,TEXT(".done")))
            {
                const FString JsonName=QueueFileName(Sequence,Id); Retired.Add(JsonName);
                if (VaRest::Http::DeleteFile(FPaths::Combine(Directory,JsonName)).bOk)
                    VaRest::Http::DeleteFile(FPaths::Combine(Directory,Name));
            }
            else if (Name.EndsWith(TEXT(".pending")) || Name.Contains(TEXT(".part")))
                VaRest::Http::DeleteFile(FPaths::Combine(Directory,Name));
        }
        for (FString& Name : Listing.Names)
        {
            FNameRecord Record;
            if (!QueueStorePrivate::FileIdentity(Name,Record.Sequence,Record.Id,TEXT(".json")) || Retired.Contains(Name)) continue;
            Record.Name=MoveTemp(Name);
            ++IdCounts.FindOrAdd(Record.Id); ++SequenceCounts.FindOrAdd(Record.Sequence);
            Names.Add(MoveTemp(Record));
        }
        Listing.Names.Empty(); // release discarded strings and the original array before reading any payload
        // Remove every conflicting name before any capacity refusal can stop payload recovery.
        Names.RemoveAll([&](const FNameRecord& Name)
        {
            if (IdCounts.FindChecked(Name.Id)<=1 && SequenceCounts.FindChecked(Name.Sequence)<=1) return false;
            Result.bOk=false; VaRest::Http::DeleteFile(FPaths::Combine(Directory,Name.Name)); return true;
        });
        Names.Sort([](const FNameRecord& Left,const FNameRecord& Right){ return Left.Sequence<Right.Sequence; });
        int64 BodyBytes=0,MetadataBytes=0;
        for (const FNameRecord& Name : Names)
        {
            const FString Path=FPaths::Combine(Directory,Name.Name);
            if (Result.Items.Num()>=Policy.MaxQueued)
            { Result.bOk=false; Result.bPolicyExceeded=true; break; }
            auto Read=ReadBoundedFile(Path,QueueStorePrivate::RecordLimit(HardPolicy));
            if (!Read.bOk) { Result.bOk=false; continue; } // IO failure is not proof of corruption
            bool bCapacityRefused=false;
            const int64 Available=FMath::Min<int64>(Policy.MaxItemBytes,Policy.MaxBodyBytes-BodyBytes);
            auto Parsed=DecodeQueueRecord(Read.Bytes,Origin,HardPolicy,Available,&bCapacityRefused);
            if (bCapacityRefused) { Result.bOk=false; Result.bPolicyExceeded=true; break; }
            if (Parsed.HasError() || Parsed.GetValue().Sequence!=Name.Sequence || Parsed.GetValue().QueueId!=Name.Id)
            { Result.bOk=false; VaRest::Http::DeleteFile(Path); continue; }
            FQueueItem Item=Parsed.StealValue();
            const int64 Charge=QueueMetadataCharge(Item);
            Result.PeakBodyBytes=FMath::Max(Result.PeakBodyBytes,BodyBytes+Item.Body.Bytes.Num());
            Result.PeakMetadataBytes=FMath::Max(Result.PeakMetadataBytes,MetadataBytes+Charge);
            if (Charge>256*1024 || MetadataBytes>10*1024*1024-Charge)
            { Result.bOk=false; Result.bPolicyExceeded=true; break; }
            Item.MetadataCharge=Charge;
            BodyBytes+=Item.Body.Bytes.Num(); MetadataBytes+=Charge;
            Result.NextSequence=Item.Sequence==MAX_uint64 ? 0 : Item.Sequence+1;
            Result.Items.Add(MoveTemp(Item));
        }
        if (!Result.bOk) Result.Items.Reset(); // never expose or replay a partial failed recovery
        return Result;
    },[Callback](FQueueLoad&& Result){ (*Callback)(MoveTemp(Result)); },Reservation))
    { FQueueLoad Failed; Failed.bOk=false; (*Callback)(MoveTemp(Failed)); }
}

```

A loader failure sets coordinator `bStorageFailed`, refuses new admissions, and makes Flush report false. This avoids admitting behind an unscanned directory. No partial loaded subset is exposed and no replay starts until the application recreates the queue after correcting the store or restoring adequate configured limits; corrupt input cannot trigger repeated server calls. The duplicate handling deliberately removes all conflicting identities, including byte-identical duplicates under different generated names; a valid identity has exactly one canonical filename.

- [ ] **Step 4: Add recovery/expiry tests.** Temp-only interrupted write is never loaded; committed immutable record survives new coordinator; rename failure retains item and emits no terminal queued event; done+json same id resolves retired; corrupt/malformed record cannot open outside root; forged origin/NoAuth integer/tag length/sequence overflow/base64 cap refused. For UTC expiry use a test-injected `FQueueTime { MonotonicOrigin, UtcOrigin }`: `UtcNow = UtcOrigin + (Clock.Now-MonotonicOrigin)`, real origin captured once. Loaded future-created record is rejected, not given an extra lifetime. Exact MaxAge boundary expires. Do not sleep24h or use FDateTime::UtcNow directly in test decisions.
- [ ] **Step 5: Run green and restore the bug.** Replace string serialization of sequence with a double and prove the >2^53 test fails. Remove actual URL origin check and prove forged-origin test fails. Restore, rerun QueueStore and PersistenceGuard.

#### 8C: Offline stage and two-phase admission

**Files:** Modify `Private/VaRestOffline.cpp`, `Private/VaRestOfflineInternal.h`, `Private/VaRestClientState.cpp`, `Private/VaRestPipeline.cpp`, `Private/VaRestExchangeState.h`, `Public/VaRestStage.h`, `Public/VaRestResultHandler.h`, `Private/Tests/VaRestHttpOfflineTest.cpp`.

**Interfaces:** Consumes queue admission/store, interruptible wait and ClassifyAttempt. Produces `MakeOfflineStage`, queued outcome fields and callback-only queued completion:

```cpp
// FVaRestAttemptOutcome and FFinish:
bool bQueued = false;
FString QueueId;
// FExchangeState:
bool bQueueCommitted = false;
TUniqueFunction<void(const FString&)> OnQueued;
// FOfflineQueue (GT), additional private state:
uint64 NextSequence = 1;
TArray<TUniqueFunction<void()>> AfterLoad;
```

`OnQueued` belongs to the exchange, not every templated result handler; no IResultHandler virtual expansion is needed. OnChainDone sees bQueued before decode/classification, transfers the fields into FFinish and calls normal Complete, preserving lifecycle accounting. Complete sets done/removes in-flight/counters/traces through existing code, then chooses Queued callback branch before Handler.Deliver. It drops the typed handler on GT so its original success/error/complete callbacks never run. A silent owner/shutdown still receives no callbacks. An accepted queue acknowledgement is GT-only because Resolve rejects OnHttpThread with QueueWhenOffline.

- [ ] **Step 1: Add `VaRest.Http.Offline.QueuedOnlyAfterCommit`.**

```cpp
using namespace VaRest::Http::Testing;
FTestRig Rig;
FScopedPersistenceTest Disk;
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")), FPaths::Combine(Disk.Root,TEXT("Queue")));
Rig.Client.WithOfflineQueue(TEXT("account-A"));
Rig.Connectivity->ReportOffline();
const auto Original = MakeOutcome<void>();
const auto Queued = MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
const FVaRestRequestHandle Handle = Observe(Rig.Client.Post<void>(TEXT("/write"))
    .Body(TEXT("safe-body")).Idempotent().QueueWhenOffline(), Original)
    .OnQueued([Queued](const FString& QueueId) { Queued->Add(QueueId); }).Send();
TestEqual(TEXT("not reentrant"), Queued->Num(), 0);
if (!TestTrue(TEXT("persisted acknowledgement"), PumpUntil([Queued] { return Queued->Num() == 1; }))) return false;
TestFalse(TEXT("original handle ended"), Handle.IsActive());
TestEqual(TEXT("no success"), Original->SuccessCalls.load(), 0);
TestEqual(TEXT("no error"), Original->ErrorCalls.load(), 0);
TestEqual(TEXT("no complete"), Original->CompleteCalls.load(), 0);
TestEqual(TEXT("offline consumed no attempts"), Rig.Fake->NumCalls(), 0);
TestEqual(TEXT("queued count"), Rig.Client.GetQueuedCount(), 1);
TestTrue(TEXT("drained"), Rig.Drain());
return true;
```

Add an actual delayed-write variant using Task 4 barrier: before release queued0, count1, handle active; Cancel then release yields exactly one Canceled original completion, queued0, store empty after deletion, count0. A second variant releases and pumps acknowledgement before Cancel; queued1 and stored item remains. OwnerGone variant counts no callbacks while item admission follows the same commit rule. Gate tests use FOutcomeProbe and assert Total1 in each branch.

Register `VaRest.Http.Offline.SecretSnapshotNeverWritten` with this body. These rows test the file boundary, including a secret cleared after Send but before the async worker sees current configuration:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Variant=0;Variant<3;++Variant)
{
    FScopedPersistenceTest Disk;
    FTestRig Rig;
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("guard-boundary"));
    Rig.Connectivity->ReportOffline();
    Rig.Client.SetSecretValue(TEXT("test-secret"),TEXT("s+7"));
    TArray<uint8> Body; Body.Init(120,8192);
    if (Variant==0) { Body.Add(115); Body.Add(43); Body.Add(55); }
    const FString Path=Variant==1 ? TEXT("/write?v=s%2B7") : TEXT("/write");
    const auto Outcome=MakeOutcome<void>();
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Observe(Rig.Client.Post<void>(Path).Body(MoveTemp(Body)).Tag(Variant==2 ? FName(TEXT("s+7")) : NAME_None)
        .Idempotent().QueueWhenOffline(),Outcome).OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    Rig.Client.ClearSecretValues();
    if (!TestTrue(TEXT("guard refusal completes"),Rig.Wait(Outcome))) return false;
    TestTrue(TEXT("persistence refused"),Outcome->Error.Kind==EVaRestErrorKind::Config);
    TestEqual(TEXT("no queued acknowledgement"),*Queued,0);
    TestEqual(TEXT("no network"),Rig.Fake->NumCalls(),0);
    if (!TestTrue(TEXT("cleanup drained"),Rig.Drain())) return false;
    const auto Files=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
    const FString Directory=QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("guard-boundary"));
    LaunchPersistence([Directory]{ return ListFiles(Directory); },[Files](FIoResult&& Result){ *Files=MoveTemp(Result.Names); });
    if (!TestTrue(TEXT("file inspection drained"),Disk.Drain())) return false;
    TestEqual(TEXT("no record, pending or temp written"),Files->Num(),0);
}
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Offline`.
- [ ] **Step 3: Insert the stage and preserve bodies.** Task 7 already installed Cache before Retry. This task inserts MakeOfflineStage immediately before that existing Cache stage; never install a second Cache stage. OfflineStage passes refresh/probe immediately and runs replay guard preflight (refresh offline short-circuit is immediate, never queue). For ordinary non-opted requests it calls Next so Cache can answer offline. For opted requests known offline it calls Queue.Enqueue immediately. While online it calls Next once, intercepts final ClassifyAttempt.Kind==Connection or Offline, then enqueues; other outcomes pass untouched. Cache stale success is success and must not also enqueue. The retry stage stays inside Offline, so a connection attempt is queued only after all configured retries. In Resolve replace its final preservation assignment with `Exchange.bMayReissue = Cfg.bMayReissue || Exchange.bRetryEligible || bAuthMayReplay || Exchange.bQueueWhenOffline;`. This is after queue validation; the earlier Send assignment is not sufficient because Resolve overwrites it. This retains bytes even for NoRetry with nonrefreshable/no auth.

```cpp
// namespace VaRest::Http, before the out-of-line body:
void GuardReplay(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done);
class FOfflineStage final : public IVaRestStage
{
public:
    virtual FName GetStageName() const override { return TEXT("Offline"); }
    virtual void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& OnOutcome) override;
};
void FOfflineStage::Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next,
    FVaRestOutcomeCallback&& OnOutcome)
{
    auto& Ex = Internals(*Exchange);
    if (!Ex.QueueTerminalDetail.IsNone()) { OnOutcome(LocalFailure(Ex.QueueTerminalDetail)); return; }
    if (!Ex.QueueReplayOf.QueueId.IsEmpty()) { GuardReplay(Exchange,Next,MoveTemp(OnOutcome)); return; }
    if (Ex.bIsAuthRefresh || Ex.bConnectivityProbe || !Ex.bQueueWhenOffline)
    {
        Next.Proceed(Exchange, MoveTemp(OnOutcome));
        return;
    }
    if (!QueueIdentityStillCurrent(Ex)) { OnOutcome(LocalFailure(TEXT("QueueIdentity"))); return; }
    const auto Queue = Ex.Queue; // Send/Resolve snapshot, not current client's later queue setting
    if (!Ex.Config->Connectivity->IsOnline())
    {
        Queue->Enqueue(Exchange, MoveTemp(OnOutcome));
        return;
    }
    Next.Proceed(Exchange, [Exchange, Queue, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
    {
        const auto Kind = ClassifyAttempt(Outcome).Kind;
        if (Kind == EVaRestErrorKind::Connection || Kind == EVaRestErrorKind::Offline)
            Queue->Enqueue(Exchange, MoveTemp(Done));
        else Done(MoveTemp(Outcome));
    });
}
```

Durable replay uses an asynchronous preflight guard before the Cache/Auth chain. Replace the replay part of the bypass branch with `GuardReplay(Exchange,Next,MoveTemp(OnOutcome)); return;`; refresh and health probes still bypass immediately. The terminal synthetic detail check precedes both branches. Add these complete bodies beside FOfflineStage:

```cpp
void VaRest::Http::GuardReplay(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done)
{
    auto& Ex=Internals(*Exchange);
    auto Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
    FPersistenceGuard Guard=*Ex.PersistenceGuard; Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard);
    Guard.AbortSignal=Ex.PersistenceAbort;
    const int64 Reservation=PersistenceGuardReservation(Ex.Body.Kind,Ex.Body.ContentType,Ex.Body.Bytes.Num());
    const bool bAdmitted=LaunchPersistence([Guard=MoveTemp(Guard),Body=Ex.Body,Headers=Ex.RequestHeaders,
        Url=Ex.Url,Tag=Ex.Tag.ToString(),Verb=Ex.Verb]() mutable
    {
        FIoResult Result;
        for (const auto& Part : Body.PartDescriptors)
            if (Guard.Rules.ShouldRedact(Part.Name,EVaRestRedactKeyKind::Part)) return Result;
        FVaRestJson Metadata=FVaRestJson::Object();
        Metadata.Set(TEXT("url"),Url); Metadata.Set(TEXT("tag"),Tag); Metadata.Set(TEXT("verb"),Verb);
        Metadata.Set(TEXT("contentType"),Body.ContentType);
        FVaRestJson HeaderJson=FVaRestJson::Array();
        for (const auto& Header : Headers) HeaderJson.Add(FVaRestJson::Array(Header.Key,Header.Value));
        Metadata.Set(TEXT("headers"),HeaderJson);
        TArray<uint8> MetadataBytes;
        Result.bOk=Guard.AllowsUrl(Url) && Guard.AllowsBody(Body.Kind,Body.ContentType,Body.Bytes)
            && Guard.SafeHeaders(Headers).Num()==Headers.Num() && Metadata.TryToUtf8(MetadataBytes).HasValue()
            && MetadataBytes.Num()<=65536 && !Guard.ContainsSecret(MetadataBytes);
        return Result;
    },[Exchange,Next,Wait](FIoResult&& Result) mutable
    {
        if (!Wait->IsPending()) return;
        if (!Result.bOk) { Wait->Finish(LocalFailure(TEXT("PersistenceCredentials"))); return; }
        FVaRestOutcomeCallback Resume=Wait->Resume();
        if (Resume) Next.Proceed(Exchange,MoveTemp(Resume));
    },Reservation);
    if (!bAdmitted) Wait->Finish(LocalFailure(TEXT("PersistenceStopped")));
}
FVaRestStageRef VaRest::Http::MakeOfflineStage()
{ return MakeShared<FOfflineStage,ESPMode::ThreadSafe>(); }
```

Replay admission also checks every multipart descriptor's Name against the guard's Part rules on the worker, before its body scan. Add the loop to Prepare and GuardReplay while the original in-memory descriptors are available. Persist only byte payloads, never file paths or descriptors; a loaded multipart body receives raw registered-value scans, while the structural-key check was made before its original save. Do not claim structural detection of unknown secrets in a forged multipart file lacking descriptors; the storage directory is app-owned, and loaded binary payloads have the documented known-value guarantee.

`FOfflineStage` is a final IVaRestStage whose GetStageName returns "Offline"; no MayReissue override (it does not issue a second network attempt). `Ex.Queue` is a shared pointer pinned from the locked configuration snapshot at Send. Invalid queue config failed Resolve, so no null dereference is reachable.

- [ ] **Step 4: Implement two-phase enqueue and its shared atomic gate.** Add this private enum/field and helper in VaRestExchangeState.h:

```cpp
enum class EQueueAdmission : uint8 { Unused, Open, Aborted, Committing, Finished };
// FExchangeState:
std::atomic<EQueueAdmission> QueueAdmission{EQueueAdmission::Unused};
// namespace VaRest::Http, any thread:
inline bool AcceptUserCancel(FExchangeState& Ex)
{
    EQueueAdmission Seen=Ex.QueueAdmission.load(std::memory_order_acquire);
    for (;;)
    {
        if (Seen==EQueueAdmission::Unused || Seen==EQueueAdmission::Aborted) return true;
        if (Seen==EQueueAdmission::Committing || Seen==EQueueAdmission::Finished) return false;
        if (Ex.QueueAdmission.compare_exchange_weak(Seen,EQueueAdmission::Aborted,std::memory_order_acq_rel)) return true;
    }
}
```

At Send initialize QueueAdmission=Open for bQueueWhenOffline **before publishing the handle**. In FVaRestRequestHandle::Cancel, after pin/done check and before GT/worker branch, call AcceptUserCancel; false returns without setting bCancelRequested. In FClientState::Cancel call it too for ReasonUser (CancelAll can enter there directly); Aborted remains accepted. OwnerGone/Shutdown always set their silent reason, but may not abort a Committing file operation. Deadline and owner checks before commit use the same CAS Open→Aborted, never a plain check/store.

The two-phase store signatures from the record task are:

```cpp
static void Prepare(FString Directory,FString Origin,FVaRestOfflinePolicy Policy,FQueueItem Item,FPersistenceGuard Guard,
    TUniqueFunction<void(bool)>&& Done); // writes <sequence>-<id>.pending, ignored by Load
static void Commit(FString Directory,uint64 Sequence,FString QueueId,
    TUniqueFunction<void(bool)>&& Done); // rename .pending -> .json, absent unique destination
static void Abort(FString Directory,uint64 Sequence,FString QueueId); // delete .pending via pipe
```

Prepare performs full validation before creating any temp, WriteNewFile to the `.pending` path, with the usual unique `.part` and Flush(true). Commit is one worker rename via PlatformFileOps.Move, no content rewrite and no credential handling; failure removes pending and reports false. Load accepts only `.json`, cleans `.pending`/`.part`. Abort never deletes `.json`: only a commit-winner can create that name. The coordinator owns `FQueueAdmissionJob` on GT with `{Exchange,Wait,Item,Queue,ReservedBytes,bPrepared}`; its prepare continuation runs this exact gate sequence:

```cpp
void FQueueAdmissionJob::Prepared(bool bOk)
{
    auto& Ex=Internals(*Exchange);
    if (!bOk)
    {
        Queue->bFlushFailed=true;
        Queue->ReleaseReservation(ReservedBytes,ReservedMetadata);
        if (Wait->IsPending()) Wait->Finish(LocalFailure(TEXT("QueueWriteFailed")));
        return;
    }
    bPrepared=true;
    const auto Current=Ex.State->GetConfig();
    const bool bValidIdentity=QueueIdentityStillCurrent(Ex) && Item.Scope==Ex.QueueIdentityScope;
    const bool bValidOrigin=SameOrigin(Current->BaseUrl,Item.Url);
    const bool bDeadline=Ex.DeadlineAt>0 && Ex.Config->Clock->Now()>=Ex.DeadlineAt-DeadlineTimerSlack;
    if (!Wait->IsPending() || Ex.IsCancelRequested() || IsOwnerGone(Ex) || bDeadline
        || !bValidIdentity || !bValidOrigin || FClientState::IsShuttingDown())
    {
        EQueueAdmission Expected=EQueueAdmission::Open;
        Ex.QueueAdmission.compare_exchange_strong(Expected,EQueueAdmission::Aborted,std::memory_order_acq_rel);
        FQueueStore::Abort(Queue->GetDirectory(),Item.Sequence,Item.QueueId);
        Queue->ReleaseReservation(ReservedBytes,ReservedMetadata);
        if (Wait->IsPending())
        {
            if (bDeadline) Wait->Finish(MakeDeadlineOutcome());
            else if (!bValidIdentity) Wait->Finish(LocalFailure(TEXT("QueueIdentity")));
            else if (!bValidOrigin) Wait->Finish(LocalFailure(TEXT("QueueOrigin")));
            else Wait->Finish(FVaRestAttemptOutcome::Canceled());
        }
        return;
    }
    EQueueAdmission Expected=EQueueAdmission::Open;
    if (!Ex.QueueAdmission.compare_exchange_strong(Expected,EQueueAdmission::Committing,std::memory_order_acq_rel))
    {
        FQueueStore::Abort(Queue->GetDirectory(),Item.Sequence,Item.QueueId);
        Queue->ReleaseReservation(ReservedBytes,ReservedMetadata);
        if (Wait->IsPending()) Wait->Finish(FVaRestAttemptOutcome::Canceled());
        return;
    }
    // The deadline/interrupt are disarmed at the same acceptance point; no yield before Resume.
    FVaRestOutcomeCallback Accepted=Wait->TakeCompletion(); // already past the shared commit CAS; no later deadline override
    FQueueStore::Commit(Queue->GetDirectory(),Item.Sequence,Item.QueueId,
        [Self=AsShared(),Done=MoveTemp(Accepted)](bool bCommitted) mutable
        {
            auto& AcceptedEx=Internals(*Self->Exchange);
            Self->Queue->ReleaseReservation(Self->ReservedBytes,Self->ReservedMetadata);
            if (!bCommitted)
            {
                Self->Queue->bFlushFailed=true;
                Done(LocalFailure(TEXT("QueueWriteFailed")));
                return;
            }
            Self->Queue->AcceptCommitted(Self->Item);
            AcceptedEx.bQueueCommitted=true;
            FVaRestTraceNote Note; Note.Kind=EVaRestTraceNoteKind::Queued;
            Note.QueueId=Self->Item.QueueId; Note.Reason=TEXT("Offline");
            TracePoints::Note(AcceptedEx,MoveTemp(Note));
            FVaRestAttemptOutcome Outcome; Outcome.bQueued=true; Outcome.QueueId=Self->Item.QueueId;
            Done(MoveTemp(Outcome));
            Self->Queue->ScheduleReplay();
        });
}
```

`FQueueAdmissionJob` derives TSharedFromThis, constructor receives the six nondefault fields, exposes Prepared(bool), keeps only GT references, and is captured by the GT callback map. Add `FOfflineQueue::GetDirectory() const` returning Directory, `AcceptCommitted(FQueueItem)` inserting sorted/counting bytes and `friend class FQueueAdmissionJob`; Scope/origin helpers are already defined. `LocalFailure(FName Detail)` is a private inline helper returning Other/Detail with generic `"local queue operation failed"` message. The ordinary FPersistenceWait interrupt stays immediate: cancellation while Prepare runs completes the original promptly; its eventual GT callback schedules Abort and releases the reservation. A process crash can leave only `.pending`, never a replayable canceled record. Admission reservation remains charged until cleanup job is admitted; worker temp bytes are charged by executor independently until deletion completes.

F2-1/F2-3: the body-free measurement runs the same canonical serializer and strict decoder as recovery. The ≤64KiB encoded metadata,≤2048 lexical units, depth8 and ≤256KiB live DTO bounds keep its transient metadata within a separate2MiB GT normalization account; all raw header/url/verb/tag/name length/count gates run before copying them. The encoder receives an empty body and performs no payload encoding. This controller-approved metadata-only parsing exception is recorded in Global Constraints and Task10’s core threading amendment; it never allocates a raw body. That short GT metadata-only operation precedes Reserve. Full payload scans and disk IO stay on workers. MetadataCharge persists in the in-memory DTO through copies, Prepare, commit and retirement, so later compact copies cannot free reserved budget prematurely. Recovered DTOs set their measured charge after decoding; their canonical allocation is at most the value acknowledged for the same record. OriginalPartNames can only make the live charge larger. No format/version or body bytes change. After Reserve, the only fallible operation is the existing Prepare/commit path, which releases exactly the reserved body/metadata on every outcome; allocation failure follows Unreal's fatal allocation behavior, not an invented recoverable branch.

Add `std::atomic<int64> QueueAdmissionBytesCopied{0};` to Testing::FTestCounters in Public/VaRestHttpTesting.h. Declare CopyQueueAdmissionBytes in VaRestOfflineInternal.h and define it in VaRestOffline.cpp; this witnesses the actual admission payload ownership, independently of request-body construction:

```cpp
TArray<uint8> VaRest::Http::CopyQueueAdmissionBytes(TConstArrayView<uint8> Bytes)
{
#if WITH_DEV_AUTOMATION_TESTS
    Testing::Counters().QueueAdmissionBytesCopied.fetch_add(Bytes.Num());
#endif
    return TArray<uint8>(Bytes.GetData(),Bytes.Num());
}
```

Complete the job constructor and enqueue path in VaRestOffline.cpp. The class is in namespace VaRest::Http, before the out-of-line Prepared body; it friends FOfflineQueue. `LocalFailure` belongs to this namespace and has the body below. The hold starts before waiting for Load, so cancel/deadline still work during recovery.

```cpp
FVaRestAttemptOutcome VaRest::Http::LocalFailure(FName Detail)
{
    FVaRestAttemptOutcome Result; Result.Failure=EVaRestTransportFailure::Other;
    Result.FailureDetail=Detail; Result.FailureMessage=TEXT("local queue operation failed"); return Result;
}
class VaRest::Http::FQueueAdmissionJob : public TSharedFromThis<FQueueAdmissionJob,ESPMode::ThreadSafe>
{
public:
    FQueueAdmissionJob(FVaRestExchangeRef InExchange,TSharedRef<FPersistenceWait,ESPMode::ThreadSafe> InWait,
        FQueueItem InItem,TSharedRef<FOfflineQueue,ESPMode::ThreadSafe> InQueue,int64 InReservedBytes,int64 InReservedMetadata)
        : Exchange(MoveTemp(InExchange)),Wait(MoveTemp(InWait)),Item(MoveTemp(InItem)),Queue(MoveTemp(InQueue)),ReservedBytes(InReservedBytes),ReservedMetadata(InReservedMetadata) {}
    void Prepared(bool bOk);
    FVaRestExchangeRef Exchange;
    TSharedRef<FPersistenceWait,ESPMode::ThreadSafe> Wait;
    FQueueItem Item;
    TSharedRef<FOfflineQueue,ESPMode::ThreadSafe> Queue;
    int64 ReservedBytes,ReservedMetadata;
    bool bPrepared=false;
    TSharedPtr<FQueueAdmissionToken,ESPMode::ThreadSafe> Admission;
};
void VaRest::Http::FOfflineQueue::Enqueue(const FVaRestExchangeRef& Exchange,FVaRestOutcomeCallback&& Done)
{
    auto Wait=FPersistenceWait::Begin(Exchange,MoveTemp(Done));
    if (!Wait->IsPending()) return;
    if (LastAdmission==MAX_uint64) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
    const uint64 Ticket=++LastAdmission; ActiveAdmissions.Add(Ticket);
    const auto Admission=MakeShared<FQueueAdmissionToken,ESPMode::ThreadSafe>(AsShared(),Ticket);
    const auto Start=[Self=AsShared(),Exchange,Wait,Admission]
    {
        if (!Wait->CanHandoff()) return;
        auto& Ex=Internals(*Exchange);
        FString Scope,Error;
        if (Self->bStorageFailed || Self->NextSequence==0)
        { Wait->Finish(LocalFailure(TEXT("QueueWriteFailed"))); return; }
        if (!QueueIdentityStillCurrent(Ex)) { Wait->Finish(LocalFailure(TEXT("QueueIdentity"))); return; }
        Scope=Ex.QueueIdentityScope;
        if (!SameOrigin(Self->Origin,Ex.Url) || !SameOrigin(Ex.State->GetConfig()->BaseUrl,Ex.Url))
        { Wait->Finish(LocalFailure(TEXT("QueueOrigin"))); return; }
        FPersistenceGuard Guard=*Ex.PersistenceGuard;
        Guard.UnionWith(*Ex.State->GetConfig()->PersistenceGuard); Guard.AbortSignal=Ex.PersistenceAbort;
        const int64 BodyBytes=Ex.Body.Bytes.Num();
        if (BodyBytes>Self->Policy.MaxItemBytes || BodyBytes>Self->Policy.MaxBodyBytes
            || Ex.RequestHeaders.Num()>128 || Ex.Url.Len()>8192 || Ex.Verb.Len()>8192 || Ex.Tag.GetStringLength()>256)
        { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        int64 HeaderChars=0;
        for (const auto& Header : Ex.RequestHeaders)
        {
            HeaderChars+=Header.Key.Len()+Header.Value.Len();
            if (Header.Key.Len()>256 || Header.Value.Len()>8192 || HeaderChars>32768)
            { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        }
        FQueueItem Item;
        Item.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
        Item.Sequence=Self->NextSequence;
        Self->NextSequence=Self->NextSequence==MAX_uint64 ? 0 : Self->NextSequence+1;
        Item.OriginalRun=QueueRunId(); Item.OriginalRequestId=Ex.RequestId; Item.CreatedUtc=Self->UtcNow();
        Item.Scope=MoveTemp(Scope); Item.Verb=Ex.Verb; Item.Url=Ex.Url; Item.Headers=Ex.RequestHeaders;
        if (Ex.bCacheConditional) { Item.Headers.Remove(TEXT("If-None-Match")); Item.Headers.Remove(TEXT("If-Modified-Since")); }
        if (Ex.Body.PartDescriptors.Num()>128) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        int64 PartChars=0;
        for (const auto& Part : Ex.Body.PartDescriptors)
        {
            PartChars+=Part.Name.Len();
            if (PartChars>16384) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
            Item.OriginalPartNames.Add(Part.Name);
        }
        Item.Body=FVaRestRequestBody::FromBytes(TArray<uint8>(),Ex.Body.Kind,Ex.RequestHeaders.Get(TEXT("Content-Type")));
        Item.IdempotencyKey=Ex.RequestHeaders.Get(TEXT("Idempotency-Key"));
        Item.Tag=Ex.Tag.IsNone() ? FString() : Ex.Tag.ToString(); Item.AuthMode=Ex.AuthMode; Item.bIdempotent=Ex.bIdempotent;
        if (Item.Tag.Len()>256) { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        const auto Measured=MeasureQueueAdmissionMetadata(Item,Self->Origin,Self->Policy);
        if (!Measured.IsSet()) { Wait->Finish(LocalFailure(TEXT("QueueWriteFailed"))); return; }
        const int64 MetadataBytes=Measured.GetValue(); Item.MetadataCharge=MetadataBytes;
        if (!Self->Reserve(BodyBytes,MetadataBytes,Error)) { Wait->Finish(LocalFailure(FName(*Error))); return; }
        // No payload copy occurs before the count/body/metadata reservation succeeds.
        TArray<uint8> OwnedBytes=CopyQueueAdmissionBytes(Ex.Body.Bytes);
        Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(OwnedBytes),Ex.Body.Kind,Item.Body.ContentType);
        const auto Job=MakeShared<FQueueAdmissionJob,ESPMode::ThreadSafe>(Exchange,Wait,MoveTemp(Item),Self,BodyBytes,MetadataBytes);
        FQueueItem WorkerItem=Job->Item; // bounded second body copy, after the same successful reservation
#if WITH_DEV_AUTOMATION_TESTS
        Testing::Counters().QueueAdmissionBytesCopied.fetch_add(BodyBytes);
#endif
        Job->Admission=Admission;
        FQueueStore::Prepare(Self->Directory,Self->Origin,Self->Policy,MoveTemp(WorkerItem),MoveTemp(Guard),
            [Job](bool bOk){ Job->Prepared(bOk); });
    };
    if (bLoaded) Start();
    else if (AfterLoad.Num()>=Policy.MaxQueued) Wait->Finish(LocalFailure(TEXT("QueueFull")));
    else
    {
        const int64 WaitingBytes=Internals(*Exchange).Body.Bytes.Num();
        if (WaitingBytes>Policy.MaxItemBytes || WaitingBodyBytes>Policy.MaxBodyBytes-WaitingBytes)
        { Wait->Finish(LocalFailure(TEXT("QueueFull"))); return; }
        WaitingBodyBytes+=WaitingBytes;
        AfterLoad.Add([Self=AsShared(),Start,WaitingBytes]
        { Self->WaitingBodyBytes-=WaitingBytes; Start(); });
    }
}
void VaRest::Http::FOfflineQueue::AcceptCommitted(FQueueItem Item)
{
    CommittedBodyBytes+=Item.Body.Bytes.Num(); CommittedMetadataBytes+=QueueMetadataCharge(Item); Items.Add(MoveTemp(Item));
    Items.Sort([](const FQueueItem& Left,const FQueueItem& Right){ return Left.Sequence<Right.Sequence; });
}
```

Declare `LocalFailure(FName)` in VaRestOfflineInternal.h; no input string from disk is interned as FName. The only dynamic FName above is Error from Reserve, which returns the fixed QueueFull literal. Declare the job class at namespace scope before qualifying its definition. The queue loader and admission helpers are compiled in the same integrated queue commit; no unfinished method stub is linked.

In Complete, add the following immediately after `VAREST_LLM_SCOPE_SENDER(Ex.SenderTags)` and before the OnHttpThread branch:

```cpp
if (Finish.bQueued)
{
    auto Queued=MoveTemp(Ex.OnQueued);
    if (Queued) Queued(Finish.QueueId);
    return;
}
```

Add `bool bQueued=false; FString QueueId;` to FFinish. In OnChainDone, before the normal typed decode scheduling, a bQueued outcome creates FFinish with bQueued/QueueId, copies response elapsed through the ordinary Complete path, invokes Complete and returns. It never schedules decode of an empty queued body. At the existing `Ex.bDone=true` line, if QueueAdmission!=Unused store Finished with release ordering. The callback branch above occurs after silent cancellation checks and after trace/stats exactly as the existing typed callback branch does.

In FClientState::Cancel insert `if (Reason==EVaRestCancelReason::User && !AcceptUserCancel(Ex)) return;` immediately after its bFinished check. After recording the silent reason and bCancelRequested, insert:

```cpp
Ex.PersistenceAbort->store(true,std::memory_order_relaxed);
if (Ex.QueueAdmission.load(std::memory_order_acquire)==EQueueAdmission::Committing) return;
```

Only OwnerGone/Shutdown can reach this Committing branch because user cancellation was refused at the gate. Accepted queue Commit jobs tag their persistence continuation `bControlDuringStop=true`; StopPersistence drains their rename and runs internal accounting on GT with all user callbacks suppressed by CancelReason. Ordinary disk/prepare callbacks are discarded. Before StopPersistence, after CancelForShutdown, clear each live queue's AfterLoad closures and forbid replay. This releases canceled load-wait captures and prevents a queue→exchange→config→queue cycle. Do not complete a committing exchange with a fabricated canceled result or discard the sole commit accounting callback.

- [ ] **Step 5: Make handle/completion semantics truthful.** IsActive returns false if QueueAdmission==Committing or Finished (otherwise its existing !IsDone). FExchange.IsDone remains bDone; do not mark done early and lose completion. In Complete's cancellation replacement branch exempt QueueAdmission==Committing as well as an already-saved file: a post-cutoff cancellation must not hide rename failure as Canceled. Preserve OwnerGone/Shutdown silent reason regardless of acceptance; all other actual rename outcomes are delivered once. Before normal delivery store QueueAdmission=Finished only for a queue-enabled request; ordinary requests retain Unused. Complete transfers queued fields, emits CompletedQueued, then invokes Ex.OnQueued exactly once under VAREST_LLM_SCOPE_SENDER with a strong Exchange hold and returns without typed Handler.Deliver. All original success/error/complete callbacks are destroyed on GT. Failed rename uses the original error callback normally. Public Cancel/IsActive docs name the commit-handoff cutoff and acknowledge that callbacks may still be pending, exactly as the existing OnHttpThread documentation does for its cutoff.

- [ ] **Step 6: Run green and mutation.** Test retry2 connection errors then queue (three attempts total), auth-refresh network failure queue, refresh forced-offline never waits behind durable storage, queued payload body unchanged after failed transport, persistence error never OnQueued, QueueFull error, cancellation/deadline before commit, custom Outer answering during cache/queue wait, callback destroys client, owner/shutdown silence. Restore-the-bug: fire OnQueued before rename; held-write test fails. Restore and run Offline, Cancel, Auth, Retry filters.

#### 8D: Scoped FIFO replay and flush

**Files:** Modify `Private/VaRestOfflineInternal.h`, `Private/VaRestOffline.cpp`, `Private/VaRestRequestSpec.h`, `Private/VaRestClient.cpp`, `Private/VaRestAuth.cpp`, `Private/VaRestExchangeState.h`; create `Private/Tests/VaRestHttpReplayTest.cpp`.

**Interfaces:** Consumes FQueueStore.Load/Retire, queue policy/time, current client auth/session, existing `TResultHandler<TArray<uint8>>`. Produces:

```cpp
// FRequestSpecAccess (private friend of FRequestSpec):
static FVaRestRequestHandle SendQueued(const TSharedRef<FClientState,ESPMode::ThreadSafe>& State,
    const FQueueItem& Item, bool bProbe, TUniqueFunction<void(const TVaRestResult<TArray<uint8>>&)>&& Done);
// FOfflineQueue private methods:
// ScheduleReplay is public in the complete coordinator declaration.
void ReplayFinished(FString QueueId, FVaRestQueuedResult Result, bool bKeep);
void RetireAndNotify(FString QueueId, FVaRestQueuedResult Result);
void BroadcastLater(FVaRestQueuedResult Result);
TSharedPtr<FClientState,ESPMode::ThreadSafe> SelectOwner() const;
// FExchangeState: immutable persisted auth scope for an active replay.
FString ReplayIdentityScope;
// shared identity helper, namespace VaRest::Http
bool RequestIdentityScope(const FClientConfig& Config, const FVaRestSessionData& Session,
    EVaRestAuthMode Mode, const FVaRestHeaders& BuiltHeaders, FString& OutScope);
```

RequestIdentityScope never includes rotating Bearer values themselves; use Task 1 identity. It includes manual credential headers by digest for cache identity, while durable admission rejects unmanaged credential headers. It includes AuthMode and canonical session headers when mode!=None. QueueScope is the Ex wrapper over this helper. Replays recheck it at selection **and at AuthApply after the limiter**, so a login change while a replay waits cannot send a different account's credentials.

- [ ] **Step 1: Add `VaRest.Http.Replay.IdentityAndFifo`.**

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig;
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")), FPaths::Combine(Disk.Root,TEXT("Queue")));
Rig.Client.WithAuth(FVaRestAuth::Bearer()).WithOfflineQueue(TEXT("orders-account-A"));
Rig.Client.SetBearerToken(TEXT("A-old-token"));
Rig.Connectivity->ReportOffline();
const auto Queued = MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
for (int32 Number : {1,2})
{
    Rig.Client.Post<void>(TEXT("/orders")).Body(LexToString(Number)).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString& Id) { Queued->Add(Id); }).Send();
}
if (!TestTrue(TEXT("two durable items"), PumpUntil([Queued] { return Queued->Num()==2; }))) return false;
const auto Results = MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Result) { Results->Add(Result); });
Rig.Client.SetBearerToken(TEXT("A-new-token"), true);
Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
Rig.Connectivity->ReportOnline();
if (!TestTrue(TEXT("first starts"), PumpUntil([&Rig] { return Rig.Fake->NumCalls()==1; }))) return false;
TestEqual(TEXT("current auth applied"), Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")), FString(TEXT("Bearer A-new-token")));
TestTrue(TEXT("one at a time"), Rig.Drain());
TestEqual(TEXT("second waits"), Rig.Fake->NumCalls(), 1);
Rig.Fake->Complete(0, FVaRestFakeResponse::Text(200, TEXT("ok-one")));
if (!TestTrue(TEXT("second starts after durable removal"), PumpUntil([&Rig] { return Rig.Fake->NumCalls()==2; }))) return false;
Rig.Fake->Complete(1, FVaRestFakeResponse::Text(200, TEXT("ok-two")));
if (!TestTrue(TEXT("two results"), PumpUntil([Results] { return Results->Num()==2; }))) return false;
TestEqual(TEXT("first result id"), (*Results)[0].QueueId, (*Queued)[0]);
TestEqual(TEXT("second result id"), (*Results)[1].QueueId, (*Queued)[1]);
TestEqual(TEXT("drained queue"), Rig.Client.GetQueuedCount(), 0);
TestTrue(TEXT("drained workers"), Rig.Drain());
return true;
```

Register `VaRest.Http.Replay.AccountAndTenantReplacement` with this body:

```cpp
using namespace VaRest::Http::Testing;
for (bool bTenantOnly : {false,true})
{
    FScopedPersistenceTest Disk;
    FTestRig Rig;
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithAuth(FVaRestAuth::Bearer()).WithOfflineQueue(TEXT("identity-refusal"));
    Rig.Client.SetBearerToken(TEXT("account-A-token"));
    Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("tenant-A"));
    Rig.Connectivity->ReportOffline();
    const auto Queued=MakeShared<FString,ESPMode::ThreadSafe>();
    const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
    Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Result){ Results->Add(Result); });
    Rig.Client.Post<void>(TEXT("/write")).Body(TEXT("safe")).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString& Id){ *Queued=Id; }).Send();
    if (!TestTrue(TEXT("accepted before identity mutation"),PumpUntil([Queued]{ return !Queued->IsEmpty(); }))) return false;
    if (bTenantOnly) Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("tenant-B"));
    else Rig.Client.SetBearerToken(TEXT("account-B-token"));
    Rig.Connectivity->ReportOnline();
    if (!TestTrue(TEXT("terminal result"),PumpUntil([Results]{ return Results->Num()==1; }))) return false;
    TestTrue(TEXT("identity mismatch is Auth"),(*Results)[0].Error.Kind==EVaRestErrorKind::Auth);
    TestEqual(TEXT("zero cross-account wire calls"),Rig.Fake->NumCalls(),0);
    TestEqual(TEXT("durably retired before event"),Rig.Client.GetQueuedCount(),0);
    TestTrue(TEXT("drained"),Rig.Drain());
}
return true;
```

The NoAuth row uses a configured Bearer owner but .NoAuth() on the original; replay must carry no Authorization/session headers even after the unused owner token changes. The limiter-held identity row changes the session after selection and before AuthApply; it must produce Auth without sending the queued operation.

Register `VaRest.Http.Replay.SharedOwnerFailover` with this complete body:

```cpp
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig First,Second;
const auto Shared=First.Connectivity;
for (FVaRestClient* Client : {&First.Client,&Second.Client})
    Client->WithConnectivity(Shared).WithClock(First.Clock)
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("shared-orders"));
Shared->ReportOffline();
const auto Queued=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
const auto FirstEvents=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
const auto SecondEvents=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
First.Client.OnQueuedResult().AddLambda([FirstEvents](const FVaRestQueuedResult&){ ++*FirstEvents; });
Second.Client.OnQueuedResult().AddLambda([SecondEvents](const FVaRestQueuedResult& Result){ SecondEvents->Add(Result.QueueId); });
for (const TCHAR* Body : {TEXT("one"),TEXT("two")})
    First.Client.Post<void>(TEXT("/write")).Body(Body).Idempotent().QueueWhenOffline()
        .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
if (!TestTrue(TEXT("both queued"),PumpUntil([Queued]{ return Queued->load()==2; }))) return false;
First.Fake->Enqueue(FVaRestFakeResponse::Manual()); Second.Fake->Enqueue(FVaRestFakeResponse::Manual());
Shared->ReportOnline();
if (!TestTrue(TEXT("first owner selected"),PumpUntil([&First]{ return First.Fake->NumCalls()==1; }))) return false;
TestEqual(TEXT("second owner idle"),Second.Fake->NumCalls(),0);
First.Client=FVaRestClient(); // active exchange owns its state until its result finishes
First.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("one")));
if (!TestTrue(TEXT("next owner selected after retirement"),PumpUntil([&Second]{ return Second.Fake->NumCalls()==1; }))) return false;
Second.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("two")));
if (!TestTrue(TEXT("surviving subscriber sees both"),PumpUntil([SecondEvents]{ return SecondEvents->Num()==2; }))) return false;
TestTrue(TEXT("different durable ids"),!(*SecondEvents)[0].Equals((*SecondEvents)[1],ESearchCase::CaseSensitive));
TestEqual(TEXT("departed subscriber is not resurrected"),FirstEvents->load(),0);
TestEqual(TEXT("first wire exactly once"),First.Fake->NumCalls(),1);
TestEqual(TEXT("second wire exactly once"),Second.Fake->NumCalls(),1);
TestEqual(TEXT("empty shared queue"),Second.Client.GetQueuedCount(),0);
TestTrue(TEXT("drained"),First.Drain() && Second.Drain());
return true;
```

The separate live-subscriber row retains First.Client and expects two events on each state while one owner sends both. The already-specified A→B row uses SetBearerToken without bSameIdentity and asserts Auth/no wire before advancing FIFO. These rows distinguish event-sharing, owner retention and identity isolation instead of conflating them in one count.

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Replay`.
- [ ] **Step 3: Implement private replay construction.**

```cpp
FVaRestRequestHandle VaRest::Http::FRequestSpecAccess::SendQueued(
    const TSharedRef<FClientState,ESPMode::ThreadSafe>& State, const FQueueItem& Item, bool bProbe,
    TUniqueFunction<void(const TVaRestResult<TArray<uint8>>&)>&& Done,FName TerminalDetail)
{
    TUniquePtr<FRequestSpec> Spec(new FRequestSpec(State, Item.Verb, Item.Url));
    Spec->QueueTerminalDetail=TerminalDetail;
    Spec->Headers = Item.Headers;
    Spec->Body = Item.Body;
    Spec->AuthMode = Item.AuthMode;
    Spec->bIdempotent = Item.bIdempotent;
    Spec->bKeepBody = true;
    Spec->Tag = Item.Tag.IsEmpty() ? NAME_None : FName(*Item.Tag);
    Spec->bQueueWhenOffline = false;
    Spec->bConnectivityProbe = bProbe;
    Spec->QueueReplayOf.QueueId = Item.QueueId;
    Spec->QueueReplayOf.OriginalRequestId = Item.OriginalRun == QueueRunId() ? Item.OriginalRequestId : 0;
    Spec->ReplayIdentityScope = Item.Scope;
    auto Handler = MakeUnique<TResultHandler<TArray<uint8>>>();
    Handler->OnComplete = MoveTemp(Done);
    Spec->Handler = MoveTemp(Handler);
    return Send(MoveTemp(Spec), __FILE__, __LINE__);
}
```

**Accepted O1 trace carry, effective in this task:** in VaRestTraceExcerpts.cpp, at the start of Excerpts::PrepareRequest immediately after `FJob Job;`, insert the following before Wanted/body checks, redactor acquisition, copies or job creation:

```cpp
if (!Ex.QueueReplayOf.QueueId.IsEmpty()) return Job;
```

Durable replay request-body previews are deliberately unavailable. Started metadata remains immediate and its unchanged `Payload.bExcerptPending=Job.IsSet()` is false, so no request excerpt is announced or left unresolved. Ordinary request and all response excerpt behavior remains unchanged, including the global MinBytes=8 policy. The full persistence guard still runs before replay wire. This suppression is needed even for a replay later refused by GuardReplay: Started occurs earlier. Task9 pins a three-byte newly known Text secret with Bodies capture; Task10 records the same policy in IN-14/goldens. Include VaRestTraceExcerpts.cpp in this integrated commit, not a later trace-only fix.

Add QueueReplayOf, ReplayIdentityScope, bConnectivityProbe and QueueTerminalDetail to FRequestSpec and move them in Send to exchange. Original Headers contain an idempotency key; Send's existing auto-generation must not overwrite it. Per-current-configuration defaults can add safe nonauth headers, but current defaults must not change request Content-Type over the saved explicit Content-Type. Raw result type prevents new envelope interpretation of an old operation's body; current envelope remains in trace only if the chosen raw handler applies it (landed raw handler does not).

`TryReplay` checks loaded/notactive/notshutdown, chooses lowest-id valid member, observes online or bProbe, then removes expired heads and identity/origin-invalid heads through RetireAndNotify one at a time. It must **not** loop synchronously over256 expired heads invoking user callbacks. Set bReplayActive before any external/auth query or Send. For a network head call SendQueued, capture coordinator strongly only in that exchange callback, keep current owner alive until callback returns. Completion builds FVaRestQueuedResult from raw Result, bSuccess=!Error.IsError, tag/id from captured item. Connection/Offline calls ReplayFinished(...,bKeep=true): retain head, clear active, leave observed connectivity unchanged, call ProbeFinished when needed, stop until a future timer/online edge. Any other result calls RetireAndNotify; on successful durable retire update Items/CommittedBodyBytes, clearactive, schedule BroadcastLater then ScheduleReplay. On retire failure retain head, stop runner and remember flush failure; do not issue another network replay in a hot loop.

`BroadcastLater` captures a TArray of strong live client states (sorted ClientId) **at the broadcast task's start**, then calls each state's QueuedResult.Broadcast once outside registry iteration. The shared queue is an event-sharing domain: clients sharing a name may observe its results; account-scoped names are necessary for separate event visibility. Event handlers may enqueue, clear sessions, leave the name or destroy client handles. A weak registered state remains live while an active exchange owns it; after that exchange releases, old owner disappears and next runner selects another. Tests observe no duplicate send/event during this handoff.

Complete the replay coordinator with these bodies in VaRestOffline.cpp. `SendQueued` takes the optional TerminalDetail parameter in this task (Task 10 adds its trace note); add it to the earlier declaration and definition now and set `Spec->QueueTerminalDetail=TerminalDetail`. The terminal detail is carried into Ex by Send. OfflineStage's first branch answers LocalFailure(QueueTerminalDetail) before any replay/cache/auth bypass. This makes expiry/scope/origin results travel through the ordinary logical accounting and error construction.

```cpp
TSharedPtr<VaRest::Http::FClientState,ESPMode::ThreadSafe> VaRest::Http::FOfflineQueue::SelectOwner() const
{
    TArray<uint64> Ids; Members.GetKeys(Ids); Ids.Sort();
    for (uint64 Id : Ids)
        if (const auto Candidate=Members.FindChecked(Id).Pin())
            if (Candidate->Queue.Get()==this) return Candidate;
    return nullptr;
}
void VaRest::Http::FOfflineQueue::ScheduleReplay()
{
    if (bReplayScheduled || !bLoaded || bStorageFailed || Items.IsEmpty() || FClientState::IsShuttingDown()) return;
    bReplayScheduled=true;
    const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(AsShared());
    if (!PostPersistenceContinuation([Weak]
    {
        if (const auto Self=Weak.Pin()) { Self->bReplayScheduled=false; Self->TryReplay(false); }
    })) { bReplayScheduled=false; bStorageFailed=true; }
}
bool VaRest::Http::FOfflineQueue::TryReplay(bool bProbe)
{
    check(IsInGameThread());
    if (!bLoaded || bStorageFailed || bReplayActive || Items.IsEmpty() || FClientState::IsShuttingDown()
        || QueueClock->Now()<ReplayNotBefore) return false;
    const auto Reachability=Connectivity.Pin();
    const auto Owner=SelectOwner();
    if (!Reachability.IsValid() || !Owner.IsValid() || (!bProbe && !Reachability->IsOnline())) return false;
    bReplayActive=true; bProbeReplay=bProbe;
    const FQueueItem Item=Items[0];
    FName Terminal;
    const auto Current=Owner->GetConfig();
    FString Scope;
    const double Age=(UtcNow()-Item.CreatedUtc).GetTotalSeconds();
    if (Age<0 || Age>=Policy.MaxAgeSeconds) Terminal=TEXT("QueueExpired");
    else if (!SameOrigin(Current->BaseUrl,Item.Url)) Terminal=TEXT("QueueOrigin");
    else if (!RequestIdentityScope(*Current,Owner->GetSessionData(),Item.AuthMode,Item.Headers,Scope) || Scope!=Item.Scope)
        Terminal=TEXT("QueueIdentity");
    FRequestSpecAccess::SendQueued(Owner.ToSharedRef(),Item,bProbe,
        [Self=AsShared(),Owner,Item,Terminal](const TVaRestResult<TArray<uint8>>& Reply)
        {
            FVaRestQueuedResult Result; Result.QueueId=Item.QueueId;
            Result.Tag=Item.Tag.IsEmpty() ? NAME_None : FName(*Item.Tag);
            Result.bSuccess=!Reply.Error.IsError(); Result.Response=Reply.Response; Result.Error=Reply.Error;
            const bool bKeep=Reply.Error.Kind==EVaRestErrorKind::Connection || Reply.Error.Kind==EVaRestErrorKind::Offline;
            // Expiry is terminal Offline; a network Offline must remain pending.
            Self->ReplayFinished(Item.QueueId,MoveTemp(Result),bKeep && Terminal!=TEXT("QueueExpired"));
        },Terminal);
    return true;
}
void VaRest::Http::FOfflineQueue::ReplayFinished(FString QueueId,FVaRestQueuedResult Result,bool bKeep)
{
    if (bKeep)
    {
        bReplayActive=false;
        const bool bWasProbe=bProbeReplay; bProbeReplay=false;
        if (const auto Reachability=Connectivity.Pin())
        {
            if (bWasProbe) Reachability->ProbeFinished();
        }
        // Logical Connection is not physical evidence. Pause this queue, not shared reachability.
        ReplayNotBefore=QueueClock->Now()+10.0;
        QueueClock->ClearTimer(ReplayRetryTimer);
        const TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Weak(AsShared());
        if (!bStopped && !FClientState::IsShuttingDown())
            ReplayRetryTimer=QueueClock->SetTimer(10.0,[Weak]
            { if (const auto Self=Weak.Pin()) Self->ScheduleReplay(); });
        return;
    }
    RetireAndNotify(MoveTemp(QueueId),MoveTemp(Result));
}
void VaRest::Http::FOfflineQueue::RetireAndNotify(FString QueueId,FVaRestQueuedResult Result)
{
    if (Items.IsEmpty() || Items[0].QueueId!=QueueId) { bStorageFailed=true; bReplayActive=false; return; }
    FQueueStore::Retire(Directory,Items[0].Sequence,QueueId,
        [Self=AsShared(),QueueId=MoveTemp(QueueId),Result=MoveTemp(Result)](bool bOk) mutable
        {
            if (bOk && !Self->Items.IsEmpty() && Self->Items[0].QueueId==QueueId)
            {
                Self->CommittedBodyBytes-=Self->Items[0].Body.Bytes.Num();
                Self->CommittedMetadataBytes-=QueueMetadataCharge(Self->Items[0]); Self->Items.RemoveAt(0);
            }
            else Self->bStorageFailed=true;
            Self->bReplayActive=false;
            const bool bWasProbe=Self->bProbeReplay; Self->bProbeReplay=false;
            if (bWasProbe)
                if (const auto Reachability=Self->Connectivity.Pin()) Reachability->ProbeFinished();
            if (bOk) { Self->BroadcastLater(MoveTemp(Result)); Self->ScheduleReplay(); }
        });
}
void VaRest::Http::FOfflineQueue::BroadcastLater(FVaRestQueuedResult Result)
{
    // Called only from the asynchronous retirement handback: already later than Send/network completion.
    if (FClientState::IsShuttingDown()) return;
    const auto Keep=AsShared();
    TArray<uint64> Ids; Members.GetKeys(Ids); Ids.Sort();
    TArray<TSharedPtr<FClientState,ESPMode::ThreadSafe>> Owners;
    for (uint64 Id : Ids)
        if (const auto Member=Members.FindChecked(Id).Pin())
            if (Member->Queue.Get()==this) Owners.Add(Member);
    for (const auto& Member : Owners)
    { VAREST_LLM_SCOPE_SENDER(Member->EventSenderTags); Member->QueuedResult.Broadcast(Result); }
}

bool VaRest::Http::FOfflineQueue::Flush(TUniqueFunction<void(bool)>&& Done)
{
    check(IsInGameThread());
    if (bStopped || FClientState::IsShuttingDown() || FlushWaiters.Num()>=4096) return false;
    auto Waiter=MakeShared<FFlushWaiter,ESPMode::ThreadSafe>();
    Waiter->Through=LastAdmission;
    Waiter->Done=MoveTemp(Done);
    // Reserve the delivery slot before retaining a waiter. It remains charged until delivery/discard.
    if (!PostPersistencePoll([Self=AsShared(),Waiter]{ return Self->PollFlush(Waiter); })) return false;
    FlushWaiters.Add(Waiter);
    return true;
}
void VaRest::Http::FOfflineQueue::FinishAdmission(uint64 Ticket)
{
    check(IsInGameThread()); ActiveAdmissions.Remove(Ticket);
    // PollFlush owns readiness and delivery. Never invoke a flush callback on an API stack.
}
bool VaRest::Http::FOfflineQueue::PollFlush(const TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>& Waiter)
{
    if (bStopped || FClientState::IsShuttingDown()) { FlushWaiters.Remove(Waiter); return true; }
    if (Waiter->Result.IsSet())
    {
        const bool bOk=Waiter->Result.GetValue();
        auto Callback=MoveTemp(Waiter->Done);
        FlushWaiters.Remove(Waiter);
        if (Callback) Callback(bOk);
        return true;
    }
    if (!bLoaded || Waiter->bBarrierStarted) return false;
    for (uint64 Ticket : ActiveAdmissions) if (Ticket<=Waiter->Through) return false;
    Waiter->bBarrierStarted=true;
    if (!LaunchPersistence([]{ FIoResult Result; Result.bOk=true; return Result; },
        [Self=AsShared(),Waiter](FIoResult&& Result)
        { Waiter->Result=Result.bOk && !Self->bStorageFailed && !Self->bFlushFailed; }))
        Waiter->Result=false;
    return false; // both refusal and worker success are delivered by a subsequent owned poll
}

```

The completed result does not synthesize online from logical success: only OnAttemptDone updates observed connectivity. A terminal local expiry during a probe calls ProbeFinished and leaves connectivity offline, so the next head waits for the next probe tick. Set bProbeReplay=false before invoking ProbeFinished to tolerate reentrant listener activity. BroadcastLater must run under sender tags captured with each member registration; add `FLlmSenderTags EventSenderTags` to FClientState, capture in WithOfflineQueue under the existing sender-tag API, and scope each event invocation with `VAREST_LLM_SCOPE_SENDER(Member->EventSenderTags)`.

At AuthApply immediately before applying credentials, insert:

```cpp
if (Ex.bQueueWhenOffline && !QueueIdentityStillCurrent(Ex))
{ OnOutcome(LocalFailure(TEXT("QueueIdentity"))); return; }
if (!Ex.ReplayIdentityScope.IsEmpty())
{
    FString CurrentScope;
    const auto Current=Ex.State->GetConfig();
    if ((Ex.AuthMode!=EVaRestAuthMode::None && Current->Auth!=Ex.Config->Auth) || !SameOrigin(Current->BaseUrl,Ex.Url)
        || !RequestIdentityScope(*Current,Ex.State->GetSessionData(),Ex.AuthMode,Ex.RequestHeaders,CurrentScope)
        || CurrentScope!=Ex.ReplayIdentityScope)
    {
        OnOutcome(LocalFailure(TEXT("QueueIdentity")));
        return;
    }
}
```

The identity check reads the effective per-request AuthMode; None deliberately ignores the owner's configured auth and session. For None, omit the `Current->Auth!=Ex.Config->Auth` check so an unrelated owner login does not invalidate anonymous replay. This pointer check is only a conservative refusal of a changed auth provider for an authenticated in-flight replay; refreshing its live session preserves the provider and identity. Declare RequestIdentityScope in VaRestIdentity.h alongside AuthIdentity; its exact signature appears in this task and its body precedes QueueScope.

Flush tracks logical admission watermarks, not only already-submitted pipe jobs. Otherwise a Flush called while Prepare is running could finish its barrier before GT has submitted Commit. Use the same coordinator fields/methods declared in8A (do not duplicate them), and add the admission token before FQueueAdmissionJob's definition:

```cpp
// FOfflineQueue fields/methods:
struct FFlushWaiter
    {
        uint64 Through=0;
        TUniqueFunction<void(bool)> Done;
        TOptional<bool> Result;
        bool bBarrierStarted=false;
    };
uint64 LastAdmission=0;
TSet<uint64> ActiveAdmissions;
TArray<TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>> FlushWaiters;
bool bFlushFailed=false;
void FinishAdmission(uint64 Ticket);
bool PollFlush(const TSharedRef<FFlushWaiter,ESPMode::ThreadSafe>& Waiter);
friend struct FQueueAdmissionToken;
// namespace VaRest::Http:
struct FQueueAdmissionToken
{
    TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Owner;
    uint64 Ticket;
    FQueueAdmissionToken(const TSharedRef<FOfflineQueue,ESPMode::ThreadSafe>& InOwner,uint64 InTicket)
        : Owner(InOwner),Ticket(InTicket) {}
    ~FQueueAdmissionToken()
    { check(IsInGameThread()); if (const auto Queue=Owner.Pin()) Queue->FinishAdmission(Ticket); }
};
```

Enqueue's Start closure and then the admission job own this token only on GT. Every early rejection, canceled load wait, prepare failure, abort or commit acknowledgement eventually releases it once. Flush returns false without retaining/invoking Done if registration/shutdown/waiter/continuation admission fails. A true return only promises an asynchronous result, with shutdown/owner suppression. Flush captures LastAdmission and waits only for tickets≤that watermark; requests admitted afterward cannot indefinitely postpone it. Once those finish, its pipe barrier also waits for their admitted abort/removal jobs. It does not wait for future network replays. bFlushFailed records refused/failed admissions for truthful false reporting; bStorageFailed additionally blocks replay after loader/retirement failure. Shutdown discards flush delegates without invoking users.

Register `VaRest.Http.Offline.FlushAdmissionBounds` with this complete body:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig;
FScopedPersistenceHooks Hooks(Disk.Root);
Hooks.Hooks->bFailCommit=true;
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithOfflineQueue(TEXT("flush-bounds"));
Rig.Connectivity->ReportOffline();
if (!TestTrue(TEXT("load complete"),Rig.Drain())) return false;
const auto Calls=MakeShared<int32,ESPMode::ThreadSafe>(0);
const auto Failed=MakeShared<int32,ESPMode::ThreadSafe>(0);
for (int32 Index=0;Index<4096;++Index)
    if (!TestTrue(TEXT("bounded waiter admitted"),Rig.Client.FlushQueue([Calls,Failed](bool bOk){ ++*Calls; if (!bOk) ++*Failed; }))) return false;
const auto RefusedCallback=MakeShared<int32,ESPMode::ThreadSafe>(0);
TestFalse(TEXT("overflow refused"),Rig.Client.FlushQueue([RefusedCallback](bool){ ++*RefusedCallback; }));
TestEqual(TEXT("accepted callbacks never inline"),*Calls,0);
if (!TestTrue(TEXT("flushes drained"),Rig.Drain())) return false;
TestEqual(TEXT("all accepted succeeded asynchronously"),*Calls,4096);
TestEqual(TEXT("no failure before IO"),*Failed,0);
TestEqual(TEXT("false admission has no callback"),*RefusedCallback,0);
const auto Original=MakeOutcome<void>();
Observe(Rig.Client.Post<void>(TEXT("/save")).Body(TEXT("safe")).Idempotent().QueueWhenOffline(),Original).Send();
if (!TestTrue(TEXT("injected rename failure delivered"),Rig.Wait(Original))) return false;
TestTrue(TEXT("actual error, not queued"),Original->Error.Kind==EVaRestErrorKind::Config);
const auto Io=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
if (!TestTrue(TEXT("postfailure flush admitted"),Rig.Client.FlushQueue([Io](bool bOk){ *Io=bOk; }))) return false;
TestFalse(TEXT("failed IO callback not inline"),Io->IsSet());
if (!TestTrue(TEXT("failed IO flush drained"),Rig.Drain())) return false;
if (!TestTrue(TEXT("IO result supplied"),Io->IsSet())) return false;
TestFalse(TEXT("accepted request returns IO failure"),Io->GetValue());
const auto Dropped=MakeShared<int32,ESPMode::ThreadSafe>(0);
if (!TestTrue(TEXT("owner flush admitted"),Rig.Client.FlushQueue([Dropped](bool){ ++*Dropped; }))) return false;
Rig.Client=FVaRestClient();
if (!TestTrue(TEXT("owner flush drained"),Disk.Drain())) return false;
TestEqual(TEXT("expired owner suppresses callback"),*Dropped,0);
return true;
```

The exchange-level shutdown tests in Task 9 additionally pin shutdown suppression: Flush uses ordinary continuations, never bControlDuringStop. The public overflow test above bounds waiters; a second private-runtime row fills Deferred to8192 and calls the same PostPersistencePoll admission used by Flush, asserting false and no retained callback. No production counter is overwritten.

`VaRest.Http.Offline.FlushAdmissionCutoff` holds AfterPrepare, sends an opted request, immediately calls FlushQueue and verifies neither OnQueued nor flush completion occurs. Release the hook: OnQueued occurs after rename, flush occurs afterward, file is replayable. The test records event order in a shared array and asserts [queued-first, flushed-first]. Later admissions are excluded from the logical watermark, although physical FIFO jobs already ahead of the eventual barrier can delay it; no instantaneous flush or OS-IO preemption is promised. A mutant using only a pipe no-op acknowledges the first flush before queued-first and must fail.

- [ ] **Step 4: Implement flush and restart.** Flush enqueues a serial no-op barrier behind all admitted writes/retirements and calls Done on GT with accumulated failure status; it does not mark connectivity online or start replay. WithOfflineQueue schedules Load immediately then ScheduleReplay if online. A disk-restart test flushes, drops client/coordinator, imports **the previously exported A session** into a new client with the same name/root, attaches event listener, lets Load finish and observes one replay with current restored auth. QueueId and idempotency key remain identical, and prior original request callbacks are absent. Different session export causes Auth terminal result/no wire. A separate process-run identity injection sets original run different and verifies QueueReplayOf.OriginalRequestId==0.

- [ ] **Step 5: Pin one-owner/probe/failure cases.** Two compatible clients share name; both observe one result but fake transport chosen only from lowest-id owner. Drop first public handle while active: that request completes once, then second live owner sends next. Both callbacks enqueue recursively: FIFO of already committed items remains ahead of new items. Connection keeps head and suppresses second; after10s one probe replays head, preserving idempotency key; successful probe resumes queue and flips online. 401 refresh reuses scope/current token, while refresh failed Offline retains item. MaxAge exactly expires with Offline note and no wire. Replayed NoAuth stays anonymous after current owner logs in. Restore-the-bug by removing scope check at AuthApply: blocked-replay-login test must fail; restore. Run Replay, Offline, Auth and Connectivity green.


- [ ] **Final integrated commit:** `git add Source/VaRestHttp/Public/VaRestOffline.h Source/VaRestHttp/Private/VaRestOfflineInternal.h Source/VaRestHttp/Private/VaRestOffline.cpp Source/VaRestHttp/Private/Tests/VaRestHttpOfflineTest.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Private/VaRestQueueStore.h Source/VaRestHttp/Private/VaRestQueueStore.cpp Source/VaRestHttp/Private/Tests/VaRestHttpQueueStoreTest.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Public/VaRestStage.h Source/VaRestHttp/Public/VaRestResultHandler.h Source/VaRestHttp/Private/VaRestRequestSpec.h Source/VaRestHttp/Private/VaRestAuth.cpp Source/VaRestHttp/Private/Tests/VaRestHttpReplayTest.cpp Source/VaRestHttp/Private/VaRestIdentity.h Source/VaRestHttp/Private/VaRestIdentity.cpp Source/VaRestHttp/Private/VaRestHttpModule.cpp Source/VaRestHttp/Private/VaRestClassify.cpp Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestTraceExcerpts.cpp && git commit -m "feat(http): add durable scoped offline admission and FIFO replay"`. Run QueueStore, Offline, Replay, Cancel, Retry and Auth filters before this review boundary.

### Task 9: Cancellation, shutdown, ownership and persistence fault matrix

**Files:** Create `Private/Tests/VaRestHttpPersistenceRegressionTest.cpp`; modify `Private/Tests/VaRestHttpPersistenceTest.cpp`, `Private/Tests/VaRestHttpCacheTest.cpp`, `Private/Tests/VaRestHttpOfflineTest.cpp`, `Private/Tests/VaRestHttpReplayTest.cpp`, and the minimal owning production files when a pinned invariant fails.

**Interfaces:** Consumes Task 4 barrier/IO/completion-permit counters, Task8 admission-copy witness/canonical metadata charge, FOutcomeProbe, actual client state counters and all prior surfaces. Produces deterministic regression tests; no new public surface.

- [ ] **Step 1: Write `VaRest.Http.Offline.Interleavings` as a table of explicit cases.** Each row constructs a new isolated rig/root/probe, installs the named hook, invokes the action, releases the hook, drains and compares these expectations:

| Case / hook | Action while held | Original terminal | File after drain | Wire / queued events |
| --- | --- | --- | --- | --- |
| CacheReadStarted | Handle.Cancel | Canceled1 | old cache retained | 0 / 0 |
| CacheReadStarted | deadline Advance | Timeout1 | old cache retained | 0 / 0 |
| CacheReadStarted | ClearCache | success from new wire | new generation only | 1 / 0 |
| CacheReadStarted | session replace | success from new identity wire | never reused old key | 1 / 0 |
| QueuePrepareStarted | Handle.Cancel | Canceled1 | no replayable item | 0 / 0 |
| QueuePrepareStarted | deadline Advance | Timeout1 | no replayable item | 0 / 0 |
| QueuePrepareStarted | session replace | Auth1 | no replayable item | 0 / 0 |
| QueueCommitStarted | Handle.Cancel | commit result once | one if rename succeeded | 0 / OnQueued1 on success |
| QueueRetireStarted | online edge twice | no original callbacks | gone after success | next head starts once |
| QueueRetireStarted | owner drops handle | no original callbacks | gone after success | subscribers once |
| QueuePrepareStarted | simulated shutdown | silent | only ignored pending file; restart cleanup | 0 / 0 |
| QueueCommitStarted | simulated shutdown | silent | committed item recoverable | 0 / 0 |

Use explicit row structs rather than assertions relying on a fixed delay:

```cpp
struct FExpectedEnd
{
    int32 Success = 0;
    int32 Error = 0;
    int32 Queued = 0;
    int32 Calls = 0;
    EVaRestErrorKind Kind = EVaRestErrorKind::None;
};
const auto AssertEnd = [this](const TCHAR* Label, const FExpectedEnd& Expected,
    const TSharedRef<VaRest::Http::Testing::TTestOutcome<void>,ESPMode::ThreadSafe>& Outcome,
    int32 QueuedCalls, int32 WireCalls)
{
    TestEqual(Label, Outcome->SuccessCalls.load(), Expected.Success);
    TestEqual(TEXT("error count"), Outcome->ErrorCalls.load(), Expected.Error);
    TestEqual(TEXT("queued count"), QueuedCalls, Expected.Queued);
    TestEqual(TEXT("wire count"), WireCalls, Expected.Calls);
    if (Expected.Error) TestTrue(TEXT("error kind"), Outcome->Error.Kind == Expected.Kind);
};
```

Register the two concrete two-phase rows as `VaRest.Http.Offline.CancelAroundCommit`. Both use actual files and public handles; each test iteration has its own queue root/connectivity:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (bool bAfterGate : {false,true})
{
    FScopedPersistenceTest Disk;
    FTestRig Rig;
    const auto Probe=MakeShared<FOutcomeProbe,ESPMode::ThreadSafe>();
    Rig.Client.Use(Probe,EVaRestStageSlot::Outer);
    FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (bAfterGate) Hooks.Hooks->BeforeRename=Barrier; else Hooks.Hooks->AfterPrepare=Barrier;
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("cancel-cutoff"));
    Rig.Connectivity->ReportOffline();
    if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
    const auto Outcome=MakeOutcome<void>();
    const auto Queued=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
    const auto Handle=Observe(Rig.Client.Post<void>(TEXT("/save")).Body(TEXT("payload"))
        .Idempotent().QueueWhenOffline(),Outcome).OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (!TestTrue(TEXT("worker reached exact barrier"),PumpUntil([Barrier]{ return Barrier->Entered.load(); }))) return false;
    TestFalse(TEXT("completion waits for IO"),Handle.IsDone());
    TestEqual(TEXT("active reflects gate"),Handle.IsActive(),!bAfterGate);
    Handle.Cancel();
    if (!bAfterGate)
    {
        if (!TestTrue(TEXT("pregate cancel completes promptly"),Rig.Wait(Outcome))) return false;
        TestTrue(TEXT("pregate canceled"),Outcome->Error.Kind==EVaRestErrorKind::Canceled);
    }
    Barrier->Released=true;
    if (!TestTrue(TEXT("drain all stages"),Rig.Drain())) return false;
    TestFalse(TEXT("barrier never timed out"),Barrier->TimedOut.load());
    TestTrue(TEXT("done after actual completion"),Handle.IsDone());
    TestEqual(TEXT("exactly one outer outcome"),Probe->Total(Handle.GetId()),1);
    TestEqual(TEXT("queued only after committed rename"),Queued->load(),bAfterGate ? 1 : 0);
    TestEqual(TEXT("ordinary callback exactly on canceled side"),Outcome->CompleteCalls.load(),bAfterGate ? 0 : 1);
    TestEqual(TEXT("no transport"),Rig.Fake->NumCalls(),0);
    const auto Files=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
    const FString Directory=QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("cancel-cutoff"));
    LaunchPersistence([Directory]{ return ListFiles(Directory); },[Files](FIoResult&& Result){ *Files=MoveTemp(Result.Names); });
    if (!TestTrue(TEXT("inspection drained"),Disk.Drain())) return false;
    int32 Replayable=0;
    for (const FString& Name : *Files) if (Name.EndsWith(TEXT(".json"))) ++Replayable;
    TestEqual(TEXT("cancel cannot resurrect on restart"),Replayable,bAfterGate ? 1 : 0);
}
return true;
```

Add the deadline variant by replacing only Handle.Cancel with virtual-clock Advance past an explicit RetryPolicy.Deadline, and assert Timeout before the gate versus queued after it. Add the any-thread race variant by launching a worker that calls Handle.Cancel while AfterPrepare is held, wait for that worker's atomic Returned flag on GT, then release AfterPrepare: the shared CAS must choose Aborted, error Canceled, no `.json`. The paired post-gate worker cancel uses BeforeRename and must produce queued. These substitutions define separate named bodies, not a nondeterministic race stress test.

#### Fix round1 executable regression suite (R1–R13)

Create `Source/VaRestHttp/Private/Tests/VaRestHttpPersistenceRegressionTest.cpp` in Task9. Add it to this task's Files and exact commit list. Include VaRestHttpTesting.h, VaRestClientState.h, VaRestExchangeState.h, VaRestPersistence.h, VaRestDiskCache.h, VaRestQueueStore.h, VaRestOfflineInternal.h, VaRestCachePolicy.h, VaRestIdentity.h, VaRestJson.h, HAL/PlatformFileManager.h, Tasks/Task.h and Misc/AutomationTest.h. Use the registration convention above, with one body per named test below. Helpers live in `VaRest::Http::Testing::PersistenceRegression`, never file-level using directives. All source versions, including each deliberately restored mutant, receive the fresh NORMAL graph/build gate before `Tools/run_tests.sh --no-build VaRest.Http.PersistenceRegression`. These tests require Task8, so earlier task filters must not accidentally include this new file before Task9.

Complete shared fixtures:

```cpp
namespace VaRest::Http::Testing::PersistenceRegression
{
    FQueueItem SafeItem(uint64 Sequence,int32 BodyBytes=1024)
    {
        FQueueItem Item; Item.QueueId=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
        Item.Sequence=Sequence; Item.OriginalRun=QueueRunId(); Item.OriginalRequestId=Sequence;
        Item.CreatedUtc=FDateTime::UtcNow(); Item.Scope=HashFields(TEXT("regression"),TArray<FString>{TEXT("anonymous")});
        Item.Verb=TEXT("PUT"); Item.Url=TEXT("https://api.test/items"); Item.AuthMode=EVaRestAuthMode::None;
        TArray<uint8> Bytes; Bytes.Init(71,BodyBytes);
        Item.Body=FVaRestRequestBody::FromBytes(MoveTemp(Bytes),EVaRestBodyKind::Binary,TEXT("application/octet-stream"));
        Item.Headers.Set(TEXT("Content-Type"),Item.Body.ContentType);
        return Item;
    }
    FCacheEntry SafeEntry()
    {
        FCacheEntry Entry; Entry.Key=HashFields(TEXT("regression/cache"),TArray<FString>{TEXT("entry")});
        Entry.Body={71,72,73}; Entry.ContentType=TEXT("application/octet-stream");
        Entry.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=600"));
        Entry.Metadata=ReadCacheMetadata(Entry.Headers,0,0,FDateTime::UtcNow());
        return Entry;
    }
    class FDelayedTimerClock final : public IVaRestClock
    {
    public:
        double Now() const override { return Seconds.load(); }
        FVaRestTimerHandle SetTimer(double Delay,TUniqueFunction<void()>&& Callback) override
        { return Timers.SetTimer(Delay,MoveTemp(Callback)); }
        void ClearTimer(FVaRestTimerHandle& Handle) override { Timers.ClearTimer(Handle); }
        void AdvanceNowOnly(double Delta) { Seconds.store(Seconds.load()+Delta); }
        int32 Pending() const { return Timers.NumPendingTimers(); }
    private:
        std::atomic<double> Seconds{0};
        FVaRestVirtualClock Timers;
    };
    class FEarlyOuter final : public IVaRestStage,public TSharedFromThis<FEarlyOuter,ESPMode::ThreadSafe>
    {
    public:
        FName GetStageName() const override { return TEXT("Regression.EarlyOuter"); }
        void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
        {
            Pending=MoveTemp(Done);
            Next.Proceed(Exchange,[Self=AsShared()](FVaRestAttemptOutcome&& Outcome)
            {
                ++Self->InnerCalls;
                if (Self->Pending) { auto Finish=MoveTemp(Self->Pending); Finish(MoveTemp(Outcome)); }
            });
        }
        void FinishNow()
        { check(Pending); auto Finish=MoveTemp(Pending); FVaRestAttemptOutcome Answer; Answer.Status=204; Finish(MoveTemp(Answer)); }
        int32 InnerCalls=0;
    private:
        FVaRestOutcomeCallback Pending;
    };
}
```

`VaRest.Http.PersistenceRegression.CaseDistinctSecrets` (R1): both raw secrets coexist in different slots; each is independently refused by both actual persistence boundaries. Memory retention is permitted for successful responses; neither disk nor queue may retain them.

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig;
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithCache(FVaRestCachePolicy::MemoryAndDisk()).WithOfflineQueue(TEXT("secret-case"));
Rig.Client.SetSecretValue(TEXT("case-one"),TEXT("AbCdEfSecret"));
Rig.Client.SetSecretValue(TEXT("case-two"),TEXT("aBcDeFsECRET"));
if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
for (const TCHAR* Secret : {TEXT("AbCdEfSecret"),TEXT("aBcDeFsECRET")})
{
    Rig.Connectivity->ReportOnline();
    Rig.Client.ClearCache(); if (!TestTrue(TEXT("clear drained"),Rig.Drain())) return false;
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,Secret));
    const auto Reply=MakeOutcome<FString>();
    Observe(Rig.Client.Get<FString>(TEXT("/secret-response")),Reply).Send();
    if (!TestTrue(TEXT("response completes"),Rig.Wait(Reply))) return false;
    TestEqual(TEXT("live response preserved"),Reply->SuccessCalls.load(),1);
    Rig.Connectivity->ReportOffline();
    const auto Rejected=MakeOutcome<void>();
    Observe(Rig.Client.Put<void>(TEXT("/items")).Body(Secret).QueueWhenOffline(),Rejected).Send();
    if (!TestTrue(TEXT("queue refuses secret"),Rig.Wait(Rejected))) return false;
    TestTrue(TEXT("persistence failure"),Rejected->Error.Kind==EVaRestErrorKind::Config);
}
if (!TestTrue(TEXT("writes drained"),Rig.Drain())) return false;
const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
LaunchPersistence([Root=Disk.Root]
{
    FIoResult Result; Result.bOk=true;
    FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*Root,
        [&Result](const TCHAR* Path,bool bDirectory)
        { if (!bDirectory) Result.Names.Add(FString(Path)); return true; });
    return Result;
},[Files](FIoResult&& Result){ *Files=Result.Names.Num(); });
if (!TestTrue(TEXT("inspection drained"),Disk.Drain())) return false;
TestEqual(TEXT("neither case variant wrote even a temporary file"),*Files,0);
return true;
```

The secret occurs only in the response body, so URL rejection cannot mask this regression. Mutant: restore AddUnique for the raw-secret union; at least one row must leave a file or admit the queued body.

`VaRest.Http.PersistenceRegression.QueueCodecSplit` verifies the bounded split rather than treating its canonical format assumption as trusted input:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing::PersistenceRegression;
auto Item=SafeItem(1,1); Item.Body.Bytes[0]=77;
Item.Tag=TEXT("quoted \\\"body\\\":\\\"TQ==\\\" text");
const auto Encoded=EncodeQueueRecord(Item);
auto Good=DecodeQueueRecord(Encoded,TEXT("https://api.test"),FVaRestOfflinePolicy());
if (!TestTrue(TEXT("quoted metadata cannot delimit body"),Good.HasValue())) return false;
TestTrue(TEXT("bytes survive split"),Good.GetValue().Body.Bytes==Item.Body.Bytes);
const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Encoded.GetData()),Encoded.Num());
const FString Text(Converted.Length(),Converted.Get());
for (const TCHAR* Replacement : {
    TEXT("\"body\":\"TQ==\",\"body\":\"\""),
    TEXT("\"bo\\u0064y\":\"TQ==\""),
    TEXT("\"body\":\"\\u0054Q==\""),
    TEXT("\"body\":\"TQ=A\""),
    TEXT("\"body\":\"TR==\""),
    TEXT("\"body\":\"TQ=\""),
    TEXT("\"body\":\"TQ==\",\"bo\\u0064y\":\"\"")})
{
    const FString Bad=Text.Replace(TEXT("\"body\":\"TQ==\""),Replacement,ESearchCase::CaseSensitive);
    if (!TestFalse(TEXT("fixture replacement happened"),Bad.Equals(Text,ESearchCase::CaseSensitive))) return false;
    const FTCHARToUTF8 Utf8(*Bad);
    TestTrue(TEXT("ambiguous or noncanonical body refused"),DecodeQueueRecord(
        MakeArrayView(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length()),TEXT("https://api.test"),FVaRestOfflinePolicy()).HasError());
}
for (int32 Size : {0,1,2,3,1024*1024})
{
    auto Roundtrip=SafeItem(2,Size); const auto Bytes=EncodeQueueRecord(Roundtrip);
    auto Decoded=DecodeQueueRecord(Bytes,TEXT("https://api.test"),FVaRestOfflinePolicy());
    if (!TestTrue(TEXT("canonical lengths decode"),Decoded.HasValue())) return false;
    TestTrue(TEXT("canonical body exact"),Decoded.GetValue().Body.Bytes==Roundtrip.Body.Bytes);
}
return true;
```

The literal escaped-key/value rows intentionally carry a JSON backslash-u spelling, not a C++ Unicode escape. Add excessive metadata and >2048 lexical-unit rows by encoding a valid metadata root with an extra8192-element array; Decode must refuse before constructing the metadata DOM. SplitRecord's scan itself owns no dynamic buffer until metadata length/unit/depth checks pass. Restore the bug by replacing the split with AsString/whole-root Clone and record charged-workspace assertions at the maximum binary row during implementation; no elapsed-time/RSS threshold substitutes for the owned-buffer accounting.

`VaRest.Http.PersistenceRegression.RecoveryBounds` (R2) checks retained peak and preservation, not just final count:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
const FString Root=FPaths::Combine(Disk.Root,TEXT("records"));
const auto Written=MakeShared<int32,ESPMode::ThreadSafe>(0);
for (uint64 Sequence=1;Sequence<=4;++Sequence)
{
    const auto Item=SafeItem(Sequence);
    LaunchPersistence([Root,Item]{ return WriteNewFile(FPaths::Combine(Root,QueueFileName(Item.Sequence,Item.QueueId)),EncodeQueueRecord(Item)); },
        [Written](FIoResult&& Result){ if (Result.bOk) ++*Written; });
}
if (!TestTrue(TEXT("fixture writes drained"),Disk.Drain())) return false;
if (!TestEqual(TEXT("four valid acknowledged-format records"),*Written,4)) return false;
for (bool bCountLimit : {false,true})
{
    FVaRestOfflinePolicy Lower; Lower.MaxItemBytes=1024; Lower.MaxBodyBytes=bCountLimit ? 4096 : 2048; Lower.MaxQueued=bCountLimit ? 2 : 256;
    const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
    FQueueStore::Load(Root,TEXT("https://api.test"),Lower,[Loaded](FQueueLoad&& Value){ *Loaded=MoveTemp(Value); });
    if (!TestTrue(TEXT("bounded recovery drained"),Disk.Drain())) return false;
    TestFalse(TEXT("incompatible policy refused"),Loaded->bOk);
    TestTrue(TEXT("policy reason distinguished"),Loaded->bPolicyExceeded);
    TestEqual(TEXT("no partial replay window"),Loaded->Items.Num(),0);
    TestTrue(TEXT("peak retained bodies bounded before append"),Loaded->PeakBodyBytes<=Lower.MaxBodyBytes);
    TestTrue(TEXT("metadata including candidate bounded"),Loaded->PeakMetadataBytes<=10*1024*1024+256*1024);
}
FVaRestOfflinePolicy Adequate; Adequate.MaxItemBytes=1024; Adequate.MaxBodyBytes=4096;
const auto Reopened=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
FQueueStore::Load(Root,TEXT("https://api.test"),Adequate,[Reopened](FQueueLoad&& Value){ *Reopened=MoveTemp(Value); });
if (!TestTrue(TEXT("adequate recovery drained"),Disk.Drain())) return false;
if (!TestTrue(TEXT("valid files were preserved"),Reopened->bOk)) return false;
if (!TestEqual(TEXT("all acknowledged operations survive"),Reopened->Items.Num(),4)) return false;
for (int32 Index=0;Index<4;++Index) TestEqual(TEXT("FIFO preserved"),Reopened->Items[Index].Sequence,uint64(Index+1));
return true;
```

`VaRest.Http.PersistenceRegression.RecoveryMetadata` separately proves the metadata bound and unavailable-store integration:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
const FString Root=QueueDirectory(Disk.Root,TEXT("metadata"));
const auto Written=MakeShared<int32,ESPMode::ThreadSafe>(0);
for (uint64 Sequence=1;Sequence<=300;++Sequence)
{
    auto Item=SafeItem(Sequence,0);
    for (int32 Header=0;Header<4;++Header)
        Item.Headers.Set(FString::Printf(TEXT("X-Safe-%d"),Header),FString::ChrN(8000,TEXT('z')));
    LaunchPersistence([Root,Item]{ return WriteNewFile(FPaths::Combine(Root,QueueFileName(Item.Sequence,Item.QueueId)),EncodeQueueRecord(Item)); },
        [Written](FIoResult&& Result){ if (Result.bOk) ++*Written; },256*1024);
}
if (!TestTrue(TEXT("metadata fixture drained"),Disk.Drain())) return false;
if (!TestEqual(TEXT("complete bounded fixtures"),*Written,300)) return false;
FVaRestOfflinePolicy Policy; Policy.MaxQueued=512; Policy.MaxItemBytes=1; Policy.MaxBodyBytes=1;
const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
FQueueStore::Load(Root,TEXT("https://api.test"),Policy,[Loaded](FQueueLoad&& Value){ *Loaded=MoveTemp(Value); });
if (!TestTrue(TEXT("metadata recovery drained"),Disk.Drain())) return false;
TestFalse(TEXT("metadata excess refused"),Loaded->bOk); TestTrue(TEXT("capacity reason"),Loaded->bPolicyExceeded);
TestEqual(TEXT("failed prefix never exposed"),Loaded->Items.Num(),0);
TestEqual(TEXT("no body material"),Loaded->PeakBodyBytes,int64(0));
TestTrue(TEXT("bounded metadata peak"),Loaded->PeakMetadataBytes<=10*1024*1024+256*1024);
const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
LaunchPersistence([Root]{ return ListFiles(Root); },[Files](FIoResult&& Result){ *Files=Result.Names.Num(); });
if (!TestTrue(TEXT("preservation inspection"),Disk.Drain())) return false;
TestEqual(TEXT("all valid records preserved"),*Files,300);
FTestRig Rig; Rig.Connectivity->ReportOffline();
Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root)
    .WithOfflineQueue(TEXT("metadata"),Policy);
if (!TestTrue(TEXT("coordinator load"),Rig.Drain())) return false;
auto Reply=MakeOutcome<void>(); Observe(Rig.Client.Put<void>(TEXT("/items")).QueueWhenOffline(),Reply).Send();
if (!TestTrue(TEXT("unavailable store refuses admission"),Rig.Wait(Reply))) return false;
TestTrue(TEXT("Config result"),Reply->Error.Kind==EVaRestErrorKind::Config);
Rig.Connectivity->ReportOnline();
if (!TestTrue(TEXT("no partial replay"),Rig.Drain())) return false;
TestEqual(TEXT("no wire for unavailable recovery"),Rig.Fake->NumCalls(),0);
return true;
```

This fixture represents preexisting metadata beyond the new hard ceiling; it is preserved for explicit application migration, never silently truncated. New production admission prevents creating that state. Add the following complete `VaRest.Http.PersistenceRegression.MetadataReservations` body; pin ReservedMetadataBytes across held Prepare with the existing RegistryAndReservations barrier case:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FTestRig Rig;
Rig.Connectivity->ReportOffline();
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root)
    .WithOfflineQueue(TEXT("metadata-reservations"));
if (!TestTrue(TEXT("empty load complete"),Rig.Drain())) return false;
const auto Queue=FClientAccess::State(Rig.Client)->Queue;
if (!TestTrue(TEXT("registered"),Queue.IsValid())) return false;
FString Failure;
int32 Accepted=0;
for (int32 Index=0;Index<40;++Index)
{
    if (!Queue->Reserve(0,256*1024,Failure)) break;
    ++Accepted;
}
TestEqual(TEXT("ten MiB reserved"),Accepted,40);
const bool bOverflow=Queue->Reserve(0,256*1024,Failure);
TestFalse(TEXT("metadata overflow refused"),bOverflow);
TestEqual(TEXT("fixed refusal"),Failure,FString(TEXT("QueueFull")));
if (bOverflow) Queue->ReleaseReservation(0,256*1024);
for (int32 Index=0;Index<Accepted;++Index) Queue->ReleaseReservation(0,256*1024);
const bool bAfterRelease=Queue->Reserve(0,256*1024,Failure);
TestTrue(TEXT("capacity released"),bAfterRelease);
if (bAfterRelease) Queue->ReleaseReservation(0,256*1024);
TestEqual(TEXT("no stranded reservation"),Queue->Count(),0);
return true;
```

 Body-policy recovery retains the separate adequate-policy reopen proof above. Removing incremental metadata admission/retention must fail these assertions.

`VaRest.Http.PersistenceRegression.MetadataRestartParity` (F2-1) uses production admission, fills the last metadata gap with smaller records, releases all owners, reopens with identical policy, and retires/reuses capacity:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
const FString CacheRoot=FPaths::Combine(Disk.Root,TEXT("cache")),QueueRoot=FPaths::Combine(Disk.Root,TEXT("queue"));
TWeakPtr<FOfflineQueue,ESPMode::ThreadSafe> Previous;
int32 Accepted=0; int64 AcknowledgedCharge=0;
{
    FTestRig Rig; Rig.Connectivity->ReportOffline();
    Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("parity"));
    if (!TestTrue(TEXT("production load complete"),Rig.Drain())) return false;
    const auto Queue=FClientAccess::State(Rig.Client)->Queue; Previous=Queue;
    for (int32 ValueLength : {6000,64})
    {
        for (int32 Header=0;Header<5;++Header)
            Rig.Client.WithHeader(FString::Printf(TEXT("X-Pair-%d"),Header),FString::ChrN(ValueLength,TEXT('v')));
        bool bRefused=false;
        for (int32 Attempt=0;Attempt<257;++Attempt)
        {
            auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<bool,ESPMode::ThreadSafe>(false);
            Observe(Rig.Client.Put<void>(TEXT("/items")).NoAuth().QueueWhenOffline(),Reply)
                .OnQueued([Queued](const FString&){ *Queued=true; }).Send();
            if (!TestTrue(TEXT("admission settles"),PumpUntil([Queued,Reply]{return *Queued || Reply->CompleteCalls.load()!=0;}))) return false;
            if (!*Queued)
            {
                TestTrue(TEXT("finite metadata refusal"),Reply->Error.Kind==EVaRestErrorKind::Config);
                bRefused=true; break;
            }
            ++Accepted;
        }
        if (!TestTrue(TEXT("finite ceiling reached"),bRefused)) return false;
    }
    if (!TestTrue(TEXT("all acknowledged writes drained"),Rig.Drain())) return false;
    TestTrue(TEXT("metadata rather than count ceiling"),Accepted>40 && Accepted<256);
    AcknowledgedCharge=Queue->MetadataBytesForTest();
    TestTrue(TEXT("near ten MiB ceiling"),AcknowledgedCharge>10*1024*1024-16384);
    TestTrue(TEXT("within ceiling"),AcknowledgedCharge<=10*1024*1024);
    TestEqual(TEXT("every acknowledgement retained"),Rig.Client.GetQueuedCount(),Accepted);
}
if (!TestFalse(TEXT("full coordinator release"),Previous.IsValid())) return false;
FTestRig Restored; Restored.Connectivity->ReportOffline();
Restored.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("parity"));
if (!TestTrue(TEXT("same-policy recovery"),Restored.Drain())) return false;
TestEqual(TEXT("all acknowledged records available"),Restored.Client.GetQueuedCount(),Accepted);
const auto Queue=FClientAccess::State(Restored.Client)->Queue;
if (!TestTrue(TEXT("recovered coordinator"),Queue.IsValid())) return false;
TestTrue(TEXT("recovery does not grow acknowledged charge"),Queue->MetadataBytesForTest()<=AcknowledgedCharge);
for (int32 Index=0;Index<Accepted;++Index) Restored.Fake->Enqueue(FVaRestFakeResponse::Text(204,TEXT("")));
Restored.Connectivity->ReportOnline();
if (!TestTrue(TEXT("recovered records retire"),PumpUntil([&Restored]{return Restored.Client.GetQueuedCount()==0;}))) return false;
if (!TestTrue(TEXT("retirement drained"),Restored.Drain())) return false;
TestEqual(TEXT("retirement releases canonical charge"),Queue->MetadataBytesForTest(),int64(0));
Restored.Connectivity->ReportOffline();
const auto Again=MakeShared<bool,ESPMode::ThreadSafe>(false);
Restored.Client.Put<void>(TEXT("/again")).NoAuth().QueueWhenOffline().OnQueued([Again](const FString&){*Again=true;}).Send();
if (!TestTrue(TEXT("released capacity reusable"),PumpUntil([Again]{return *Again;}))) return false;
return TestTrue(TEXT("final drain"),Restored.Drain());
```

Mutant: reserve only the live compact DTO charge instead of max(live, recovered). The near-ceiling same-policy recovery assertion must fail; preserve five header pairs and the smaller final records so the test does not accidentally leave a large unused gap.

`VaRest.Http.PersistenceRegression.RejectedAdmissionCopiesNothing` (F2-2/F2-3) covers loaded oversized/full queues and a loading oversized queue; construction of the caller's request is outside this admission counter:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Scenario=0;Scenario<3;++Scenario)
{
    FScopedPersistenceTest Disk; FTestRig Rig; FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (Scenario==2) Hooks.Hooks->BeforeQueueLoad=Gate;
    FVaRestOfflinePolicy Policy; Policy.MaxQueued=1; Policy.MaxItemBytes=16; Policy.MaxBodyBytes=16;
    Rig.Connectivity->ReportOffline();
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("copies"),Policy);
    if (Scenario!=2 && !TestTrue(TEXT("loaded"),Rig.Drain())) return false;
    if (Scenario==1)
    {
        const auto Seed=MakeShared<bool,ESPMode::ThreadSafe>(false);
        Rig.Client.Put<void>(TEXT("/seed")).Body(TEXT("small")).NoAuth().QueueWhenOffline()
            .OnQueued([Seed](const FString&){*Seed=true;}).Send();
        if (!TestTrue(TEXT("full queue seeded"),PumpUntil([Seed]{return *Seed;}))) return false;
        if (!TestTrue(TEXT("seed drained"),Rig.Drain())) return false;
    }
    const int64 Before=Testing::Counters().QueueAdmissionBytesCopied.load();
    TArray<uint8> Bytes; Bytes.Init('x',Scenario==1 ? 8 : 1024*1024);
    auto Reply=MakeOutcome<void>();
    Observe(Rig.Client.Put<void>(TEXT("/rejected")).Body(MoveTemp(Bytes)).NoAuth().QueueWhenOffline(),Reply).Send();
    if (!TestTrue(TEXT("rejected asynchronously"),Rig.Wait(Reply))) return false;
    TestTrue(TEXT("bounded refusal"),Reply->Error.Kind==EVaRestErrorKind::Config);
    TestEqual(TEXT("zero extra admission payload copy"),Testing::Counters().QueueAdmissionBytesCopied.load(),Before);
    TestEqual(TEXT("no replay wire"),Rig.Fake->NumCalls(),0);
    Gate->Released=true;
    if (!TestTrue(TEXT("drained after refusal"),Rig.Drain())) return false;
    TestEqual(TEXT("no stranded admission"),Rig.Client.GetQueuedCount(),Scenario==1 ? 1 : 0);
}
return true;
```

`VaRest.Http.PersistenceRegression.RecoveredKnownSecret` pins the recovery/guard boundary:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
const FString CacheRoot=FPaths::Combine(Disk.Root,TEXT("cache")),QueueRoot=FPaths::Combine(Disk.Root,TEXT("queue"));
{
    FTestRig Original; Original.Connectivity->ReportOffline();
    Original.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("restored"));
    if (!TestTrue(TEXT("original load"),Original.Drain())) return false;
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Original.Client.Put<void>(TEXT("/items")).Body(TEXT("q7!")).NoAuth().QueueWhenOffline()
        .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (!TestTrue(TEXT("original durable acknowledgement"),PumpUntil([Queued]{ return *Queued==1; }))) return false;
    if (!TestTrue(TEXT("original drained"),Original.Drain())) return false;
}
FLogCapture Logs; FTestRig Restored; FScopedTraceRecorder Trace(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies);
Restored.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheRoot,QueueRoot).WithLogLevel(EVaRestLogLevel::Body);
Restored.Client.SetSecretValue(TEXT("late-known"),TEXT("q7!"));
const auto Event=MakeShared<TOptional<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
Restored.Client.OnQueuedResult().AddLambda([Event](const FVaRestQueuedResult& Value){ *Event=Value; });
Restored.Client.WithOfflineQueue(TEXT("restored"));
if (!TestTrue(TEXT("restored item classified"),PumpUntil([Event]{ return Event->IsSet(); }))) return false;
if (!TestTrue(TEXT("all restored work drained"),Restored.Drain())) return false;
TestFalse(TEXT("known secret not replayed"),Event->GetValue().bSuccess);
TestTrue(TEXT("guard refusal is Config"),Event->GetValue().Error.Kind==EVaRestErrorKind::Config);
TestEqual(TEXT("no wire before full guard"),Restored.Fake->NumCalls(),0);
TestFalse(TEXT("callback error has no payload"),Event->GetValue().Error.Message.Contains(TEXT("q7!")));
FlushRedactionPipe(); TestFalse(TEXT("body logs redacted"),Logs.Contains(TEXT("q7!")));
const uint64 ReplayId=Event->GetValue().Response.RequestId;
const auto Events=Trace.Recorder->For(ReplayId);
if (!TestTrue(TEXT("replay metadata present"),Events.Num()>=2)) return false;
if (!TestTrue(TEXT("Started remains first"),Events[0].Kind==EVaRestTraceKind::Started)) return false;
TestFalse(TEXT("no request excerpt promised"),Events[0].Payload.Get<FVaRestTraceStarted>().bExcerptPending);
TestEqual(TEXT("refused replay announces no attempt"),Trace.Recorder->KindsOf(ReplayId),FString(TEXT("Started,Completed")));
for (const auto& Excerpt : Trace.Recorder->ExcerptsOf(ReplayId))
{
    TestTrue(TEXT("no replay request excerpt"),Excerpt.Owner!=EVaRestTraceExcerptOwner::Request);
    TestFalse(TEXT("no short secret disclosure"),Excerpt.Excerpt.ToText().Contains(TEXT("q7!")));
}
return true;
```

`VaRest.Http.PersistenceRegression.QueueAnchor` (R3) uses three deterministic hold locations:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Mutation=0;Mutation<3;++Mutation)
for (int32 Hold=0;Hold<3;++Hold)
{
    FScopedPersistenceTest Disk; FTestRig Rig; FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (Hold==1) Hooks.Hooks->BeforeQueueLoad=Barrier;
    Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithBearer(TEXT("A-live-token"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithOfflineQueue(TEXT("anchor")).WithMaxConcurrent(1);
    Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("A"));
    if (Hold==1) Rig.Connectivity->ReportOffline();
    else if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
    TSharedPtr<TTestOutcome<void>,ESPMode::ThreadSafe> Blocker;
    if (Hold==2)
    {
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); Blocker=MakeOutcome<void>();
        Observe(Rig.Client.Get<void>(TEXT("/blocker")).NoRetry(),Blocker.ToSharedRef()).Send();
        if (!TestEqual(TEXT("limiter occupied"),Rig.Fake->NumCalls(),1)) return false;
    }
    if (Hold==0) Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
    const auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe-body")).NoRetry().QueueWhenOffline(),Reply).OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (Hold==1 && !TestTrue(TEXT("initial load held"),PumpUntil([Barrier]{ return Barrier->Entered.load(); }))) return false;
    if (Hold==0)
    {
        if (!TestEqual(TEXT("A original dispatched"),Rig.Fake->NumCalls(),1)) return false;
        TestEqual(TEXT("A credentials on original"),Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-live-token")));
    }
    if (Mutation==0) Rig.Client.SetBearerToken(TEXT("B-live-token"));
    if (Mutation==1) Rig.Client.SetSessionHeader(TEXT("X-Tenant"),TEXT("B"));
    if (Mutation==2) Rig.Client.SetBearerToken(TEXT("A-refreshed-token"),true);
    if (Hold==0) Rig.Fake->Complete(0,FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
    if (Hold==1) Barrier->Released=true;
    if (Hold==2) Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("")));
    if (!TestTrue(TEXT("held operation settles"),PumpUntil([Reply,Queued]{ return Reply->CompleteCalls.load()+*Queued==1; }))) return false;
    if (Mutation<2)
    {
        TestTrue(TEXT("replacement rejects original identity"),Reply->Error.Kind==EVaRestErrorKind::Auth);
        TestEqual(TEXT("no B wire"),Rig.Fake->NumCalls(),Hold==1 ? 0 : 1);
        TestEqual(TEXT("no relabeled operation"),Rig.Client.GetQueuedCount(),0);
    }
    else if (Hold<2)
    { TestEqual(TEXT("same-identity refresh may queue"),*Queued,1); TestEqual(TEXT("one item"),Rig.Client.GetQueuedCount(),1); }
    else
    {
        TestEqual(TEXT("same-identity refresh may dispatch"),Reply->SuccessCalls.load(),1);
        if (!TestEqual(TEXT("one new dispatch"),Rig.Fake->NumCalls(),2)) return false;
        TestEqual(TEXT("live refreshed credentials"),Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")),FString(TEXT("Bearer A-refreshed-token")));
    }
    if (!TestTrue(TEXT("holds drained"),Rig.Drain())) return false;
}
return true;
```

Mutation2 is the paired same-identity control. Ordinary nonqueued controls retain landed live-auth behavior. Restoring scope calculation at Enqueue admits the wrong account/tenant in Hold0/1; omitting AuthApply's anchor gate sends B in Hold2.

`VaRest.Http.PersistenceRegression.QueueBodyAfterNoRetry` (R4):

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk; FTestRig Rig;
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithOfflineQueue(TEXT("body"));
if (!TestTrue(TEXT("load drained"),Rig.Drain())) return false;
Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
Rig.Client.Put<void>(TEXT("/items")).Body(TEXT("exact-payload")).NoAuth().NoRetry().QueueWhenOffline()
    .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
if (!TestTrue(TEXT("queue ack"),PumpUntil([Queued]{ return *Queued==1; }))) return false;
if (!TestEqual(TEXT("one failed physical attempt"),Rig.Fake->NumCalls(),1)) return false;
const TArray<uint8> Physical=Rig.Fake->ReadBody(0);
TestTrue(TEXT("original body nonempty"),!Physical.IsEmpty());
const auto Loaded=MakeShared<FQueueLoad,ESPMode::ThreadSafe>();
FQueueStore::Load(QueueDirectory(FPaths::Combine(Disk.Root,TEXT("Queue")),TEXT("body")),TEXT("https://api.test"),
    FVaRestOfflinePolicy(),[Loaded](FQueueLoad&& Value){ *Loaded=MoveTemp(Value); });
if (!TestTrue(TEXT("record read"),Disk.Drain())) return false;
if (!TestEqual(TEXT("one durable item"),Loaded->Items.Num(),1)) return false;
TestTrue(TEXT("durable bytes exactly original"),Loaded->Items[0].Body.Bytes==Physical);
TestTrue(TEXT("NoAuth persisted"),Loaded->Items[0].AuthMode==EVaRestAuthMode::None);
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT(""))); Rig.Connectivity->ReportOnline();
if (!TestTrue(TEXT("replay drained"),Rig.Drain())) return false;
if (!TestEqual(TEXT("one replay"),Rig.Fake->NumCalls(),2)) return false;
TestTrue(TEXT("replay bytes exactly original"),Rig.Fake->ReadBody(1)==Physical);
TestFalse(TEXT("anonymous replay"),Rig.Fake->GetCall(1).Request.Headers.Contains(TEXT("Authorization")));
return true;
```

`VaRest.Http.PersistenceRegression.ClearConfiguredNamespace` (R6):

```cpp
using namespace VaRest::Http::Testing;
for (bool bPreviouslyTouchedA : {false,true})
{
    FScopedPersistenceTest Disk;
    const FString CacheA=FPaths::Combine(Disk.Root,TEXT("A")),CacheB=FPaths::Combine(Disk.Root,TEXT("B"));
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0; Policy.DefaultTtlSeconds=600;
    {
        FTestRig Seed;
        Seed.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(CacheB,FPaths::Combine(Disk.Root,TEXT("Q"))).WithCache(Policy);
        Seed.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("old-B")));
        auto Reply=MakeOutcome<FString>(); Observe(Seed.Client.Get<FString>(TEXT("/entry")),Reply).Send();
        if (!TestTrue(TEXT("seed request"),Seed.Wait(Reply)) || !TestTrue(TEXT("seed disk"),Seed.Drain())) return false;
    }
    FTestRig Rig;
    Rig.Client.WithBaseUrl(TEXT("https://api.test")).WithStorageDirectories(bPreviouslyTouchedA ? CacheA : CacheB,
        FPaths::Combine(Disk.Root,TEXT("Q"))).WithCache(Policy);
    if (bPreviouslyTouchedA)
    {
        Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("old-A")));
        auto Reply=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),Reply).Send();
        if (!TestTrue(TEXT("touch A"),Rig.Wait(Reply)) || !TestTrue(TEXT("A disk"),Rig.Drain())) return false;
        Rig.Client.WithStorageDirectories(CacheB,FPaths::Combine(Disk.Root,TEXT("Q")));
    }
    Rig.Client.ClearCache(); // B has never been looked up by this client
    if (!TestTrue(TEXT("configured clear applied"),Rig.Drain())) return false;
    const int32 Before=Rig.Fake->NumCalls();
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("new-B")));
    auto Reply=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),Reply).Send();
    if (!TestTrue(TEXT("new request"),Rig.Wait(Reply))) return false;
    TestEqual(TEXT("B requires wire"),Rig.Fake->NumCalls(),Before+1);
    TestFalse(TEXT("B old bytes unavailable"),Reply->Response.bFromCache);
    if (!TestTrue(TEXT("value present"),Reply->Value.IsSet())) return false;
    TestEqual(TEXT("new value"),Reply->Value.GetValue(),FString(TEXT("new-B")));
    if (!TestTrue(TEXT("B writes drained"),Rig.Drain())) return false;
    if (bPreviouslyTouchedA)
    {
        Rig.Client.WithStorageDirectories(CacheA,FPaths::Combine(Disk.Root,TEXT("Q")));
        auto A=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),A).Send();
        if (!TestTrue(TEXT("A lookup"),Rig.Wait(A))) return false;
        TestTrue(TEXT("unrelated A namespace preserved"),A->Response.bFromCache);
        if (!TestTrue(TEXT("A value"),A->Value.IsSet())) return false;
        TestEqual(TEXT("A bytes preserved"),A->Value.GetValue(),FString(TEXT("old-A")));
    }
    if (!TestTrue(TEXT("final drain"),Rig.Drain())) return false;
}
return true;
```

`VaRest.Http.PersistenceRegression.MandatoryCacheControls` (R7) uses real bounded admission. No runtime counter is assigned to simulate saturation:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
for (int32 Operation=0;Operation<3;++Operation) // clear, no-store Remove, smaller capacity
{
    FScopedPersistenceTest Disk;
    const FString Root=FPaths::Combine(Disk.Root,TEXT("store"));
    const FCacheEntry Entry=SafeEntry();
    TSharedPtr<FDiskCache,ESPMode::ThreadSafe> Store=FDiskCache::Open(Root,1024*1024);
    Store->Put(Entry,FPersistenceGuard());
    if (!TestTrue(TEXT("seed store"),Disk.Drain())) return false;
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    LaunchPersistence([Gate]{ WaitPersistenceBarrier(Gate); FIoResult Result; Result.bOk=true; return Result; },[](FIoResult&&){});
    for (int32 Index=1;Index<8192;++Index)
        if (!TestTrue(TEXT("fill real slots"),LaunchPersistence([]{ FIoResult Result; Result.bOk=true; return Result; },[](FIoResult&&){})))
        { Gate->Released=true; return false; }
    if (Operation==0) Store->Clear();
    if (Operation==1) Store->Remove(Entry.Key);
    if (Operation==2) Store=FDiskCache::Open(Root,1);
    TestTrue(TEXT("refused mandatory operation quarantined"),Store->IsQuarantined());
    Store.Reset(); Store=FDiskCache::Open(Root,1024*1024);
    TestTrue(TEXT("release/reopen retains intent"),Store->IsQuarantined());
    const auto Hit=MakeShared<bool,ESPMode::ThreadSafe>(true);
    Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Hit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Hit=Value.IsValid(); });
    TestFalse(TEXT("refused control cannot expose prior bytes"),*Hit);
    Store->Put(Entry,FPersistenceGuard());
    Gate->Released=true;
    if (!TestTrue(TEXT("saturation drained"),Disk.Drain())) return false;
    {
        FScopedPersistenceHooks Hooks(Disk.Root); Hooks.Hooks->bFailCacheControl=true;
        Store=FDiskCache::Open(Root,1024*1024); // retry fails actual IO result
        if (!TestTrue(TEXT("failed maintenance drained"),Disk.Drain())) return false;
        TestTrue(TEXT("IO failure remains quarantined"),Store->IsQuarantined());
    }
    Store.Reset(); Store=FDiskCache::Open(Root,1024*1024);
    if (!TestTrue(TEXT("later successful retry"),Disk.Drain())) return false;
    TestFalse(TEXT("successful control lifts quarantine"),Store->IsQuarantined());
    Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Hit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Hit=Value.IsValid(); });
    if (!TestTrue(TEXT("postcontrol lookup"),Disk.Drain())) return false;
    TestFalse(TEXT("invalidated bytes never resurrect"),*Hit);
}
return true;
```

`VaRest.Http.PersistenceRegression.CapacityReadCharge` (R7):

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
const FString Root=FPaths::Combine(Disk.Root,TEXT("store"));
auto Store=FDiskCache::Open(Root,1024*1024); auto Entry=SafeEntry(); Entry.Body.Init(71,128*1024);
Store->Put(Entry,FPersistenceGuard());
if (!TestTrue(TEXT("seed persisted"),Disk.Drain())) return false;
FScopedPersistenceHooks Hooks(Disk.Root); auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Gate;
auto OldHit=MakeShared<bool,ESPMode::ThreadSafe>(true),NewHit=MakeShared<bool,ESPMode::ThreadSafe>(true);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[OldHit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *OldHit=Value.IsValid(); });
if (!TestTrue(TEXT("old read held"),PumpUntil([Gate]{ return Gate->Entered.load(); }))) return false;
FDiskCache::Open(Root,1);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[NewHit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *NewHit=Value.IsValid(); });
TestEqual(TEXT("old and new immutable read caps remain charged"),PersistencePendingBytesForTest(),int64(1024*1024+65536+1+65536));
Gate->Released=true;
if (!TestTrue(TEXT("serialized reads/control drained"),Disk.Drain())) return false;
TestFalse(TEXT("old epoch discarded"),*OldHit); TestFalse(TEXT("new small-cap read cannot reuse old allowance"),*NewHit);
TestEqual(TEXT("all resident DTO charge released"),PersistencePendingBytesForTest(),int64(0));
return true;
```

`VaRest.Http.PersistenceRegression.ControlCapFallback` (R7):

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk;
FScopedCacheControls Controls;
const FString Original=FPaths::Combine(Disk.Root,TEXT("original"));
auto Store=FDiskCache::Open(Original,1024*1024); const auto Entry=SafeEntry(); Store->Put(Entry,FPersistenceGuard());
if (!TestTrue(TEXT("baseline persisted"),Disk.Drain())) return false;
const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
LaunchPersistence([Gate]{ WaitPersistenceBarrier(Gate); return FIoResult(); },[](FIoResult&&){});
for (int32 Index=0;Index<4097;++Index)
    FDiskCache::Open(FPaths::Combine(Disk.Root,FString::Printf(TEXT("pending-%d"),Index)),1024);
TestTrue(TEXT("cap disables even previously healthy namespace"),Store->IsQuarantined());
Store->Put(Entry,FPersistenceGuard());
const auto Hit=MakeShared<bool,ESPMode::ThreadSafe>(true);
Store->Find(Entry.Key,0,FDateTime::UtcNow(),[Hit](TSharedPtr<const FCacheEntry,ESPMode::ThreadSafe> Value){ *Hit=Value.IsValid(); });
TestFalse(TEXT("process fallback is a miss"),*Hit);
Gate->Released=true;
if (!TestTrue(TEXT("already admitted controls drain"),Disk.Drain())) return false;
TestTrue(TEXT("fallback does not reset when earlier controls succeed"),FDiskCache::Open(Original,1024*1024)->IsQuarantined());
return true;
```

The isolated control registry is destroyed after Drain and before Disk cleanup. A restoring mutant that drops intent on refusal fails reopen, removing the IO-success check lifts quarantine early, and bypassing ControlsExhausted in Find fails fallback. CapacityReadCharge separately pins the old/new immutable reservations across that serialized control.

For R8 add this helper beside FEarlyOuter:

```cpp
class FLogicalReplayFailure final : public IVaRestStage
{
public:
    FName GetStageName() const override { return TEXT("Regression.LogicalReplayFailure"); }
    void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
    {
        if (VaRest::Http::Internals(*Exchange).QueueReplayOf.QueueId.IsEmpty())
        { Next.Proceed(Exchange,MoveTemp(Done)); return; }
        ++Entered; Pending=MoveTemp(Done);
        if (!bHold) Fail();
    }
    void Fail()
    {
        check(Pending); auto Finish=MoveTemp(Pending);
        FVaRestAttemptOutcome Result; Result.Failure=EVaRestTransportFailure::Connection; Finish(MoveTemp(Result));
    }
    bool bHold=false;
    int32 Entered=0;
private:
    FVaRestOutcomeCallback Pending;
};
```

`VaRest.Http.PersistenceRegression.PhysicalReachabilityOnly`:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
for (bool bDelayed : {false,true})
{
    FScopedPersistenceTest Disk; FTestRig Rig;
    Rig.Client.WithBaseUrl(TEXT("https://api.test"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")))
        .WithOfflineQueue(TEXT("reachability"));
    const auto Stage=MakeShared<FLogicalReplayFailure,ESPMode::ThreadSafe>(); Stage->bHold=bDelayed;
    Rig.Client.Use(Stage,EVaRestStageSlot::Outer); Rig.Connectivity->ReportOffline();
    if (!TestTrue(TEXT("initial load"),Rig.Drain())) return false;
    const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    Rig.Client.Put<void>(TEXT("/items")).Body(TEXT("safe")).QueueWhenOffline()
        .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    if (!TestTrue(TEXT("durable item"),PumpUntil([Queued]{ return *Queued==1; }))) return false;
    Rig.Connectivity->ReportOnline();
    if (!TestTrue(TEXT("replay entered"),PumpUntil([Stage]{ return Stage->Entered==1; }))) return false;
    if (bDelayed)
    {
        Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
        auto Reach=MakeOutcome<void>(); Observe(Rig.Client.Get<void>(TEXT("/reachable")).NoRetry(),Reach).Send();
        if (!TestEqual(TEXT("real attempt dispatched"),Rig.Fake->NumCalls(),1)) return false;
        Rig.Connectivity->ReportOffline();
        Rig.Fake->Complete(0,FVaRestFakeResponse::Text(503,TEXT("")));
        if (!TestTrue(TEXT("physical503 processed"),Rig.Wait(Reach))) return false;
        TestTrue(TEXT("physical HTTP restored online"),Rig.Connectivity->IsOnline());
        Stage->bHold=false; Stage->Fail();
    }
    if (!TestTrue(TEXT("logical failure settled"),Rig.Drain())) return false;
    TestTrue(TEXT("logical Connection cannot overwrite reachability"),Rig.Connectivity->IsOnline());
    TestEqual(TEXT("head retained"),Rig.Client.GetQueuedCount(),1);
    TestEqual(TEXT("synthetic replay never sent"),Rig.Fake->NumCalls(),bDelayed ? 1 : 0);
    TestEqual(TEXT("no immediate retry loop"),Stage->Entered,1);
    Rig.Clock->Advance(9);
    if (!TestTrue(TEXT("before retry tick drained"),Rig.Drain())) return false;
    TestEqual(TEXT("bounded queue-local pause"),Stage->Entered,1);
    Rig.Clock->Advance(1);
    if (!TestTrue(TEXT("retry tick drained"),Rig.Drain())) return false;
    TestEqual(TEXT("one later local retry"),Stage->Entered,2);
}
return true;
```

Mutant: restore ReplayFinished.ReportOffline; the online assertion fails in both rows. Remove ReplayNotBefore/timer and explicitly ScheduleReplay twice before advancing time: the no-immediate-retry assertion fails. The timer belongs to the queue, is cleared at shutdown/destruction, and never changes FVaRestConnectivity. Physical401/404/503, cache/stale/synthetic200 and ForceOffline tests remain in Task6/7's existing matrix.

`VaRest.Http.PersistenceRegression.DeadlineBeforeDiskHandoff` (R10):

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
for (bool bStale : {false,true})
{
    FScopedPersistenceTest Disk; FTestRig Rig;
    auto Clock=MakeShared<FDelayedTimerClock,ESPMode::ThreadSafe>();
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0;
    Policy.DefaultTtlSeconds=bStale ? 0 : 600; Policy.bServeStaleOnError=true;
    Rig.Client.WithClock(Clock).WithBaseUrl(TEXT("https://api.test"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue"))).WithCache(Policy);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("cached")));
    auto Seed=MakeOutcome<FString>(); Observe(Rig.Client.Get<FString>(TEXT("/entry")),Seed).Send();
    if (!TestTrue(TEXT("seed response"),Rig.Wait(Seed)) || !TestTrue(TEXT("seed IO"),Rig.Drain())) return false;
    FScopedPersistenceHooks Hooks(Disk.Root);
    auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>(); Hooks.Hooks->BeforeRead=Gate;
    if (bStale) Rig.Connectivity->ReportOffline();
    FVaRestRetryPolicy Deadline; Deadline.MaxRetries=0; Deadline.Deadline=1;
    auto Reply=MakeOutcome<FString>();
    Observe(Rig.Client.Get<FString>(TEXT("/entry")).Retry(Deadline),Reply).Send();
    if (!TestTrue(TEXT("read held"),PumpUntil([Gate]{ return Gate->Entered.load(); }))) return false;
    Clock->AdvanceNowOnly(2); // no timer dispatch: reproduces wall-clock/ticker lag
    Gate->Released=true;
    if (!TestTrue(TEXT("handback settles"),Rig.Wait(Reply))) return false;
    TestTrue(TEXT("deadline wins over fresh/stale bytes"),Reply->Error.Kind==EVaRestErrorKind::Timeout);
    TestEqual(TEXT("one completion"),Reply->CompleteCalls.load(),1);
    TestEqual(TEXT("no extra wire"),Rig.Fake->NumCalls(),1);
    if (!TestTrue(TEXT("deadline drain"),Rig.Drain())) return false;
    TestEqual(TEXT("wait timer disarmed"),Clock->Pending(),0);
}
return true;
```

`VaRest.Http.PersistenceRegression.ExchangeShutdown` (R11) tests the inherited FApiScope/Complete route using actual exchanges and an isolated executor:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (int32 Hold=0;Hold<3;++Hold) // disk read, prepared uncommitted item, accepted rename
{
    FScopedPersistenceTest Disk; // destructor runs after scoped executor restores production admission
    FScopedPersistenceRuntime Runtime;
    FTestRig Rig; FScopedPersistenceHooks Hooks(Disk.Root);
    auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    if (Hold==0) Hooks.Hooks->BeforeRead=Gate;
    if (Hold==1) Hooks.Hooks->AfterPrepare=Gate;
    if (Hold==2) Hooks.Hooks->BeforeRename=Gate;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk(); Policy.MemoryBytes=0;
    Rig.Client.WithBaseUrl(TEXT("https://api.test"))
        .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue"))).WithCache(Policy);
    if (Hold!=0) { Rig.Client.WithOfflineQueue(TEXT("shutdown")); Rig.Connectivity->ReportOffline(); }
    if (!TestTrue(TEXT("setup drained"),Rig.Drain())) return false;
    auto Reply=MakeOutcome<void>(); const auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
    const auto Handle=[&]
    {
        if (Hold==0) return Observe(Rig.Client.Get<void>(TEXT("/entry")),Reply).Send();
        return Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).QueueWhenOffline(),Reply)
            .OnQueued([Queued](const FString&){ ++*Queued; }).Send();
    }();
    if (!TestTrue(TEXT("exchange at owned hold"),PumpUntil([Gate]{ return Gate->Entered.load(); }))) return false;
    const auto State=FClientAccess::State(Rig.Client);
    if (!TestTrue(TEXT("state retained"),State.IsValid())) return false;
    const auto Unrelated=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
    UE::Tasks::Launch(TEXT("VaRest.UnrelatedExchangeShutdown"),[Unrelated]{ Unrelated->store(true); },
        UE::Tasks::ETaskPriority::Normal,UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
    State->CancelAll(NAME_None,EVaRestCancelReason::Shutdown); // same per-client path CancelForShutdown uses
    if (State->Queue.IsValid()) State->Queue->StopForShutdown();
    Gate->Released=true;
    Runtime.Stop(); // drains only owned results, including the accepted commit acknowledgement
    TestFalse(TEXT("no unrelated GT pump"),Unrelated->load());
    TestEqual(TEXT("no deferred global Complete wrapper"),State->PendingApiCompletionWrappers,0);
    TestEqual(TEXT("all owned exchange accounting settled"),State->NumInFlight(),0);
    TestTrue(TEXT("handle done after actual IO for commit"),Handle.IsDone());
    TestEqual(TEXT("ordinary callback suppressed"),Reply->CompleteCalls.load(),0);
    TestEqual(TEXT("queued callback suppressed"),*Queued,0);
    TestTrue(TEXT("owned slots and DTO bytes released"),PersistenceIdle());
    // Only after all Stop assertions may normal automation pump its witness.
    if (!TestTrue(TEXT("witness cleanup"),PumpUntil([Unrelated]{ return Unrelated->load(); }))) return false;
}
return true;
```

Mutant: restore unconditional ApiDepth deferral. The cache/prepare rows must fail PendingApiCompletionWrappers==0 and NumInFlight==0 before any general task pump. Removing bControlDuringStop must fail the committed row's accounting. The test does not set/reset the production global shutdown flag or stop the live module singleton.

`VaRest.Http.PersistenceRegression.AcceptedCompletionShutdown` (F2-4) adds the exact completed-on-Send-stack case; unlike the held-IO rows, Prepare refuses synchronously while every real IO callback slot is occupied:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FScopedPersistenceRuntime Runtime;
TWeakPtr<FClientState,ESPMode::ThreadSafe> WeakState;
const auto Unrelated=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
{
    FTestRig Rig; Rig.Connectivity->ReportOffline();
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("accepted-stop"));
    if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
    const auto State=FClientAccess::State(Rig.Client); WeakState=State;
    const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    ON_SCOPE_EXIT { Gate->Released=true; };
    if (!TestTrue(TEXT("blocking job admitted"),LaunchPersistence([Gate]{WaitPersistenceBarrier(Gate);return FIoResult();},[](FIoResult&&){}))) return false;
    for (int32 Index=1;Index<8192;++Index)
        if (!TestTrue(TEXT("real callback slot filled"),LaunchPersistence([]{return FIoResult();},[](FIoResult&&){}))) return false;
    auto Reply=MakeOutcome<void>();
    const auto Handle=Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).NoAuth().QueueWhenOffline(),Reply).Send();
    TestEqual(TEXT("accepted failure remains non-inline"),Reply->CompleteCalls.load(),0);
    TestEqual(TEXT("no global wrapper even before Stop"),State->PendingApiCompletionWrappers,0);
    TestEqual(TEXT("exchange pending owned completion"),State->NumInFlight(),1);
    UE::Tasks::Launch(TEXT("VaRest.AcceptedStopWitness"),[Unrelated]{Unrelated->store(true);},
        UE::Tasks::ETaskPriority::Normal,UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
    // No PumpUntil, TaskGraph or core ticker between Send and Stop.
    State->CancelAll(NAME_None,EVaRestCancelReason::Shutdown);
    State->Queue->StopForShutdown(); Gate->Released=true;
    Runtime.Stop();
    TestFalse(TEXT("unrelated GT witness untouched"),Unrelated->load());
    TestEqual(TEXT("zero global wrappers after Stop"),State->PendingApiCompletionWrappers,0);
    TestEqual(TEXT("zero in-flight exchanges"),State->NumInFlight(),0);
    TestEqual(TEXT("no user delivery"),Reply->CompleteCalls.load(),0);
    TestTrue(TEXT("handle silently terminal"),Handle.IsDone());
    TestTrue(TEXT("owned completion and IO slots released"),PersistenceIdle());
}
TestFalse(TEXT("no completion-owned client retention"),WeakState.IsValid());
TestFalse(TEXT("still no unrelated pump"),Unrelated->load());
return TestTrue(TEXT("witness cleanup after assertions"),PumpUntil([Unrelated]{return Unrelated->load();}));
```

Restore-the-bug: route persistence-owned ApiDepth completion through OnGameThread again. This exact test must fail both the wrapper and post-scope weak-retention assertions; zero InFlight alone is insufficient. The existing ordinary request non-inline suite and FlushFallbackNotInline remain required.

`VaRest.Http.PersistenceRegression.CompletionPermitRefusal` pins the controller-approved boundary: an exhausted completion-permit cap creates no persistence wait/job/admission and uses the ordinary non-inline Config completion path.

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk; FScopedPersistenceRuntime Runtime; FTestRig Rig;
Rig.Connectivity->ReportOffline();
Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),Disk.Root).WithOfflineQueue(TEXT("permit-cap"));
if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
TArray<uint64> Permits;
ON_SCOPE_EXIT { for (uint64 Ticket : Permits) ReleasePersistenceCompletion(Ticket); };
for (int32 Index=0;Index<8192;++Index)
{
    const uint64 Ticket=AcquirePersistenceCompletion();
    if (!TestTrue(TEXT("real bounded permit"),Ticket!=0)) return false;
    Permits.Add(Ticket);
}
TestEqual(TEXT("no unbounded completion lane"),AcquirePersistenceCompletion(),uint64(0));
const uint64 Before=PersistenceCounters().Launched.load();
const int64 Copied=Testing::Counters().QueueAdmissionBytesCopied.load();
auto Reply=MakeOutcome<void>();
Observe(Rig.Client.Put<void>(TEXT("/operation")).Body(TEXT("safe")).NoAuth().QueueWhenOffline(),Reply).Send();
TestEqual(TEXT("ordinary refusal not inline"),Reply->CompleteCalls.load(),0);
TestEqual(TEXT("no persistence job on refused permit"),PersistenceCounters().Launched.load(),Before);
TestEqual(TEXT("no payload ownership"),Testing::Counters().QueueAdmissionBytesCopied.load(),Copied);
TestEqual(TEXT("no queue reservation"),Rig.Client.GetQueuedCount(),0);
TestEqual(TEXT("ordinary completion wrapper is explicit exception"),FClientAccess::State(Rig.Client)->PendingApiCompletionWrappers,1);
for (uint64 Ticket : Permits) ReleasePersistenceCompletion(Ticket);
Permits.Reset();
if (!TestTrue(TEXT("ordinary refusal delivered later"),Rig.Wait(Reply))) return false;
TestTrue(TEXT("Config refusal"),Reply->Error.Kind==EVaRestErrorKind::Config);
TestEqual(TEXT("one completion"),Reply->CompleteCalls.load(),1);
const uint64 Reused=AcquirePersistenceCompletion();
TestTrue(TEXT("released permits reusable"),Reused!=0); ReleasePersistenceCompletion(Reused);
if (!TestTrue(TEXT("ordinary wrapper drained"),Rig.Drain())) return false;
TestEqual(TEXT("no wrapper remains"),FClientAccess::State(Rig.Client)->PendingApiCompletionWrappers,0);
Runtime.Stop(); return TestTrue(TEXT("all owned accounts empty"),PersistenceIdle());
```

`VaRest.Http.PersistenceRegression.FlushFallbackNotInline` (R12):

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk; FTestRig Rig;
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")))
    .WithOfflineQueue(TEXT("flush-stack"));
Rig.Connectivity->ReportOffline();
if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
const auto Depth=MakeShared<int32,ESPMode::ThreadSafe>(0);
const auto SeenDepth=MakeShared<int32,ESPMode::ThreadSafe>(-1);
const auto Result=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
if (!TestTrue(TEXT("flush admitted with owned delivery slot"),Rig.Client.FlushQueue([Depth,SeenDepth,Result](bool bOk)
    { *SeenDepth=*Depth; *Result=bOk; }))) return false;
const auto Gate=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
LaunchPersistence([Gate]{ WaitPersistenceBarrier(Gate); return FIoResult(); },[](FIoResult&&){});
for (int32 Index=1;Index<8192;++Index)
    if (!TestTrue(TEXT("saturate IO slots"),LaunchPersistence([]{ return FIoResult(); },[](FIoResult&&){})))
    { Gate->Released=true; return false; }
*Depth=1;
const auto Original=MakeOutcome<void>();
Observe(Rig.Client.Put<void>(TEXT("/items")).Body(TEXT("safe")).QueueWhenOffline(),Original).Send();
TestFalse(TEXT("finish-admission refusal did not call another user's flush"),Result->IsSet());
*Depth=0;
PumpPersistence(); // retained poll observes barrier refusal, stores false, does not deliver inline
TestFalse(TEXT("first readiness poll retains result slot"),Result->IsSet());
PumpPersistence();
if (!TestTrue(TEXT("next owned pump delivers refusal"),Result->IsSet())) { Gate->Released=true; return false; }
TestFalse(TEXT("truthful failed barrier"),Result->GetValue());
TestEqual(TEXT("delivery is off Send stack"),*SeenDepth,0);
Gate->Released=true;
if (!TestTrue(TEXT("cleanup drained"),Rig.Drain())) return false;
return true;
```

Keep FlushAdmissionBounds' separate false/no-callback waiter overflow and accepted IO-failure rows. The independent continuation-cap test is `VaRest.Http.PersistenceRegression.FlushContinuationBound`:

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FScopedPersistenceTest Disk;
FScopedPersistenceRuntime Runtime;
FTestRig Rig;
Rig.Client.WithBaseUrl(TEXT("https://api.test"))
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")))
    .WithOfflineQueue(TEXT("continuation-bound"));
if (!TestTrue(TEXT("queue loaded"),Rig.Drain())) return false;
for (int32 Index=0;Index<8192;++Index)
    if (!TestTrue(TEXT("owned poll slot admitted"),PostPersistencePoll([]{return false;}))) return false;
const auto Calls=MakeShared<int32,ESPMode::ThreadSafe>(0);
TestFalse(TEXT("flush admission refuses full continuation queue"),Rig.Client.FlushQueue([Calls](bool){ ++*Calls; }));
const auto State=FClientAccess::State(Rig.Client);
if (!TestTrue(TEXT("registered state"),State.IsValid() && State->Queue.IsValid())) return false;
TestEqual(TEXT("no stranded waiter"),State->Queue->NumFlushWaitersForTest(),0);
State->Queue->StopForShutdown(); Runtime.Stop();
TestEqual(TEXT("false admission has no later callback"),*Calls,0);
TestTrue(TEXT("all reserved delivery slots discarded"),PersistenceIdle());
return true;
```


`VaRest.Http.PersistenceRegression.ClosedCacheHandback` (R13):

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
using namespace VaRest::Http::Testing::PersistenceRegression;
FScopedPersistenceTest Disk; FTestRig Rig; FScopedTraceRecorder Trace;
Rig.Client.WithCache(FVaRestCachePolicy::MemoryAndDisk())
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("cache")),FPaths::Combine(Disk.Root,TEXT("queue")));
const auto Outer=MakeShared<FEarlyOuter,ESPMode::ThreadSafe>(); Rig.Client.Use(Outer,EVaRestStageSlot::Outer);
Rig.Fake->bCompleteOnWorker=false; Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
const auto Reply=MakeOutcome<void>(); const auto Handle=Observe(Rig.Client.Get<void>(TEXT("/late")),Reply).Send();
if (!TestTrue(TEXT("disk miss dispatched"),PumpUntil([&Rig]{ return Rig.Fake->NumCalls()==1; }))) return false;
Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("late cache bytes")));
// Transport's GT handback is queued. Answer through Outer before pumping it.
Outer->FinishNow();
if (!TestTrue(TEXT("outer answer completed"),Handle.IsDone())) return false;
const FString Before=Trace.Recorder->KindsOf(Handle.GetId());
if (!TestTrue(TEXT("late completion drained"),Rig.Drain())) return false;
TestEqual(TEXT("no metadata event after Completed"),Trace.Recorder->KindsOf(Handle.GetId()),Before);
TestEqual(TEXT("one client completion"),Reply->CompleteCalls.load(),1);
const auto State=FClientAccess::State(Rig.Client);
if (!TestTrue(TEXT("state exists"),State.IsValid())) return false;
TestEqual(TEXT("closed request did not resurrect memory entry"),State->MemoryCache.Num(),0);
TestEqual(TEXT("inner once callback forwarded or discarded once"),Outer->InnerCalls,1);
const auto Files=MakeShared<int32,ESPMode::ThreadSafe>(0);
LaunchPersistence([Root=Disk.Root]
{
    FIoResult Result; Result.bOk=true;
    FPlatformFileManager::Get().GetPlatformFile().IterateDirectoryRecursively(*Root,
        [&Result](const TCHAR* Path,bool bDirectory)
        { if (!bDirectory && FString(Path).EndsWith(TEXT(".json"))) Result.Names.Add(FString(Path)); return true; });
    return Result;
},[Files](FIoResult&& Result){ *Files=Result.Names.Num(); });
if (!TestTrue(TEXT("disk inspection"),Disk.Drain())) return false;
TestEqual(TEXT("closed request created no disk record"),*Files,0);
return true;
```

The event stream is metadata-only, so delayed Excerpt events cannot hide a late CacheStored Note. Extend the existing Task10 IN-17 golden with this ordering: Started,AttemptStarted,AttemptDispatched,Completed from Outer; exact landed attempt-end placement follows the ordinary orphan handler, and releasing late200 must add **no request metadata after Completed**. The persisted disk inspection must likewise contain no cache record from this closed exchange. Mutant: remove CacheOnResult terminal guard, causing retained memory or a late CacheStored Note.

- [ ] **Step 2: Run red on deliberately restored faults.** Before touching production during this task, prove each previously implemented behavior by one isolated temporary mutant: omit wait interrupt; remove epoch compare; publish queue before atomic commit; omit reservation; remove scope check; start second runner while active. Run only the discriminating filter, record an assertion failure rather than crash/hang, then restore exact hunk. This establishes the tests are meaningful even when the normal suite starts green.
- [ ] **Step 3: Add retained-owner and shutdown proofs.** Weak state expires after completion+drain when all public handles drop; off-GT destructor counter unchanged; no pending timers in test connectivity; no GT file operations; no request remains in FClientState::NumInFlight or counter after cancellation/queued completion. A custom Outer stage that answers while its Next is held must cause exactly one client completion and must interrupt pending IO hold (its own probe may observe the canceled inner result once). No callback/worker owns a raw client pointer. Standalone persistence executor Stop with pending work must finish worker bytes, suppress callbacks and reject post-stop launch; no real module stop inside the shared automation process.
- [ ] **Step 4: Run combined filters green.** `Tools/run_tests.sh --no-build VaRest.Http` includes the existing M4a retry/auth/session/trace suite. Do not broaden to whole plugin until these pass. Record each mutant filename/hunk/test/failure/result in the execution ledger, then confirm no mutant remains with `git diff --check` and source review.
- [ ] **Step 5: Commit:** `git add Source/VaRestHttp/Private/Tests/VaRestHttpPersistenceTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpCacheTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpOfflineTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpReplayTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpPersistenceRegressionTest.cpp && git commit -m "test(http): pin cache and queue lifecycle interleavings"`.

### Task 10: Cache statistics, truthful trace payloads and IN-17 goldens

**Files:** Modify `Private/VaRestHttpStatsInternal.h`, `Private/VaRestHttpStats.cpp`, `Public/VaRestTrace.h`, `Private/VaRestTracePoints.cpp`, `Private/VaRestPipeline.cpp`, `Private/VaRestCache.cpp`, `Private/VaRestOffline.cpp`, `Private/VaRestRequestSpec.h`, `Private/VaRestClient.cpp`; create `Private/Tests/VaRestHttpTraceCacheOfflineTest.cpp`; modify `Private/Tests/VaRestHttpStatsTest.cpp`; amend core and Inspector specs in this implementation task.

**Interfaces:** Consumes actual cache/queue outcomes and FTraceRecorder. Produces additive IN-1/IN-5 fields without changing thirteen event kinds:

```cpp
// FHttpCounters:
std::atomic<int64> CacheLookups{0};
std::atomic<int64> CacheHits{0};
// FHttpStats:
static void CacheLookup(FHttpCounters& Client);
static void CacheHit(FHttpCounters& Client);
static double CacheRatio(const FHttpCounters& Client);
// FVaRestTraceStarted:
bool bCacheLookupPending = false;
// FVaRestTraceCompleted:
EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
bool bFromCache = false;
// FExchangeState:
bool bCacheLookupCounted = false;
bool bCacheHitCounted = false;
```

**Dependency:** Add the minimal CacheLookup/CacheHit counters and methods in Task 7's implementation commit so its code links; this task adds stat display, ratio verification and final trace integration. No temporary no-op stats implementation.

- [ ] **Step 1: Write `VaRest.Http.Trace.CacheHitGolden`.**

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
FTestRig Rig;
FScopedTraceRecorder Trace;
auto Policy = FVaRestCachePolicy::Memory();
Policy.DefaultTtlSeconds = 60;
Rig.Client.WithCache(Policy);
Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("plain-response")));
auto First = MakeOutcome<FString>();
const auto FirstHandle = Observe(Rig.Client.Get<FString>(TEXT("/cached")), First).Send();
if (!TestTrue(TEXT("populate"), Rig.Wait(First))) return false;
auto Hit = MakeOutcome<FString>();
const auto HitHandle = Observe(Rig.Client.Get<FString>(TEXT("/cached")), Hit).Send();
if (!TestTrue(TEXT("cache hit"), Rig.Wait(Hit))) return false;
TestEqual(TEXT("no fabricated attempt"), Trace.Recorder->KindsOf(HitHandle.GetId()), FString(TEXT("Started,Completed")));
const auto Events = Trace.Recorder->For(HitHandle.GetId());
if (!TestEqual(TEXT("two metadata events"), Events.Num(), 2)) return false;
if (!TestTrue(TEXT("payload kinds"),Events[0].Kind==EVaRestTraceKind::Started && Events[1].Kind==EVaRestTraceKind::Completed)) return false;
const auto& Started = Events[0].Payload.Get<FVaRestTraceStarted>();
const auto& Ended = Events[1].Payload.Get<FVaRestTraceCompleted>();
TestTrue(TEXT("start truthfully pending"), Started.bCacheLookupPending && Started.CacheLookup==EVaRestTraceCacheLookup::None);
TestTrue(TEXT("final provenance"), Ended.bFromCache && Ended.CacheLookup==EVaRestTraceCacheLookup::HitMemory);
TestEqual(TEXT("zero attempts"), Hit->Response.Attempts, 0);
TestEqual(TEXT("no wire bytes on hit"), Hit->Response.BytesDown, static_cast<int64>(0));
FString Why;
TestTrue(TEXT("phase invariant"), PhasesHold(*Trace.Recorder, FirstHandle.GetId(), Why));
const auto& Counters = FClientAccess::State(Rig.Client)->GetCounters();
TestEqual(TEXT("two lookups"), Counters.CacheLookups.load(), static_cast<int64>(2));
TestEqual(TEXT("one hit"), Counters.CacheHits.load(), static_cast<int64>(1));
TestEqual(TEXT("ratio"), FHttpStats::CacheRatio(Counters), 0.5);
TestTrue(TEXT("drain"), Rig.Drain());
return true;
```

Register `VaRest.Http.Trace.CacheDiskWriteAfterCompleted` with this complete body. DiskBytes=1 deterministically refuses the encoded record; the other row permits persistence. MemoryBytes=0 exercises absence of memory retention without introducing a public disk-only mode.

```cpp
using namespace VaRest::Http;
using namespace VaRest::Http::Testing;
for (bool bRefuseDisk : {false,true})
{
    FScopedPersistenceTest Disk;
    FTestRig Rig;
    FScopedPersistenceHooks Hooks(Disk.Root);
    const auto Barrier=MakeShared<FPersistenceBarrier,ESPMode::ThreadSafe>();
    Hooks.Hooks->BeforeCacheWrite=Barrier;
    FScopedTraceRecorder Trace;
    auto Policy=FVaRestCachePolicy::MemoryAndDisk();
    Policy.MemoryBytes=0; Policy.DiskBytes=bRefuseDisk ? 1 : 64*1024*1024; Policy.DefaultTtlSeconds=60;
    Rig.Client.WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
        .WithCache(Policy);
    Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("safe response")));
    const auto Outcome=MakeOutcome<FString>();
    const auto Handle=Observe(Rig.Client.Get<FString>(TEXT("/best-effort")),Outcome).Send();
    if (!TestTrue(TEXT("response completes before disk release"),Rig.Wait(Outcome))) return false;
    if (!TestTrue(TEXT("disk write reached barrier"),PumpUntil([Barrier]{ return Barrier->Entered.load(); }))) return false;
    TestEqual(TEXT("response succeeds even when disk refuses"),Outcome->SuccessCalls.load(),1);
    TestTrue(TEXT("response handle finished"),Handle.IsDone());
    TestEqual(TEXT("no predicted store note"),Trace.Recorder->KindsOf(Handle.GetId()),FString(TEXT("Started,Dispatched,Ended,Completed")));
    const int32 EventsBefore=Trace.Recorder->For(Handle.GetId()).Num();
    Barrier->Released=true;
    if (!TestTrue(TEXT("disk work drained"),Rig.Drain())) return false;
    TestFalse(TEXT("barrier did not time out"),Barrier->TimedOut.load());
    TestEqual(TEXT("closed request receives no late Note"),Trace.Recorder->For(Handle.GetId()).Num(),EventsBefore);
    TestEqual(TEXT("one application result"),Outcome->CompleteCalls.load(),1);
}
return true;
```

- [ ] **Step 2: Run red:** `Tools/run_tests.sh --no-build VaRest.Http.Trace.Cache`.
- [ ] **Step 3: Implement counters and payload population.**

```cpp
double VaRest::Http::FHttpStats::CacheRatio(const FHttpCounters& Client)
{
    const int64 Lookups = Client.CacheLookups.load(std::memory_order_relaxed);
    const int64 Hits = Client.CacheHits.load(std::memory_order_relaxed);
    return Lookups > 0 ? FMath::Clamp(static_cast<double>(Hits) / Lookups, 0.0, 1.0) : 0.0;
}
void VaRest::Http::FHttpStats::CacheLookup(FHttpCounters& Client)
{
    for (FHttpCounters* Set : {&Global(), &Client}) Set->CacheLookups.fetch_add(1, std::memory_order_relaxed);
    INC_DWORD_STAT(STAT_VaRestCacheLookups);
    SET_FLOAT_STAT(STAT_VaRestCacheHitRatio, static_cast<float>(CacheRatio(Global())));
}
void VaRest::Http::FHttpStats::CacheHit(FHttpCounters& Client)
{
    for (FHttpCounters* Set : {&Global(), &Client}) Set->CacheHits.fetch_add(1, std::memory_order_relaxed);
    INC_DWORD_STAT(STAT_VaRestCacheHits);
    SET_FLOAT_STAT(STAT_VaRestCacheHitRatio, static_cast<float>(CacheRatio(Global())));
}
```

Declare two DWORD accumulator stats and one float accumulator stat in VaRestHttpStats.cpp. Cache lookup counts eligible logical requests once; hit counts served memory/disk/revalidated/stale once. Guard using exchange booleans; custom stage repeated Proceed must not make ratio exceed1. Hits do not increment transport BytesDown. LLM_SCOPE_BYTAG(VaRest) covers store buffers/workers, and existing VAREST_LLM_SCOPE_SENDER covers OnQueued/OnQueuedResult user callback allocations. Store bytes and queue bodies must not be charged to VaRest_Trace.

Keep `TracePoints::Started` at Enter exactly where it captures BP initiator/StartUtc. Task8 suppresses durable replay request-body excerpt preparation before any announcement; ordinary request excerpts remain unchanged. Document in IN-14 that replay request-body previews are unavailable, and pin Started.bExcerptPending=false with no request Excerpt in the three-byte recovered-secret golden. Describe fills CachePolicy with None/Memory/MemoryAndDisk; CacheLookup pending None when unresolved, bCacheLookupPending true only eligible; QueueWhenOffline flag; QueueReplayOf. Snapshot calls Describe and therefore reports **current** pending/lookup state; it never predicts HitDisk before bytes validated. Completed copies final CacheLookup and `Finish.Response.bFromCache`; a canceled pending read ends pending=false/lookupNone. AttemptDispatched.bConditional reflects only cache-injected still-valid validators. Queued uses Finish.bQueued to select OutcomeQueued. All notes use TracePoints.Note after the action's GT commit while the exchange still accepts metadata; no worker emits metadata. Existing note fields contain no raw key or payload.

- [ ] **Step 4: Give expired/restored queue events a real trace identity.** Task 8 already defines core-only QueueTerminalDetail and SendQueued's optional TerminalDetail. This task adds the QueueExpired note at that existing seam; do not redeclare fields or change the signature. For expiry, origin/scope refusal, construct a synthetic replay through SendQueued with `QueueExpired`, `QueueOrigin`, `QueueIdentity` respectively. OfflineStage, before its replay bypass, sees nonempty terminal detail, emits QueueExpired note for expiry only, then returns Other/detail without cache/auth/transport. This produces Started→Note→Completed(Error Offline) for expiry and Started→Completed(Error Auth/Config) for refused replay. The current numeric RequestId is fresh; QueueReplayOf supplies durable QueueId and same-run original link. RetireAndNotify owns the final OnQueuedResult; original callbacks remain absent. No invented request0 note, and no AttemptDispatched for unsent expiry.

- [ ] **Step 5: Add the required golden sequences and exact fields.**

| Test | Metadata order (per logical request) | Values pinned |
| --- | --- | --- |
| `VaRest.Http.PersistenceRegression.RecoveredKnownSecret` | Started,Completed | newly known three-byte Text secret; Bodies capture; no wire; Started request excerpt pendingfalse; no request Excerpt/no disclosure |
| `VaRest.Http.Trace.CacheDiskGolden` | Started,Completed | initial pending; finalHitDisk/fromcache; attempts0; decode fresh result; safe snapshot |
| `VaRest.Http.Trace.Cache304Golden` | Started,Dispatched,Ended,Note,Completed | dispatch conditionaltrue; wire304; CacheRevalidated; finalRevalidate/fromcache; original body; phases nonnegative |
| `VaRest.Http.Trace.CacheStaleGolden` | Started,Note,Completed when known offline | CacheServedStale, finalStale/fromcache; no fabricated Online |
| `VaRest.Http.Trace.OfflineQueuedGolden` | Started,Note,Completed | QueueWhenOffline; Queued note has id/reason; CompletedQueued; attempts0; original complete0 |
| `VaRest.Http.Trace.OfflineReplayGolden` | Started,Dispatched,Ended,Completed | new requestid; QueueReplayOf exact queueid/originalid; QueueWhenOffline false; current auth redacted |
| `VaRest.Http.Trace.OfflineExpiryGolden` | Started,Note,Completed | synthetic replayid; QueueExpired queueid; ErrorOffline; attempt0; OnQueuedResult1 |
| `VaRest.Http.Trace.CacheAttachWhileRead` | snapshot partial, then Completed | one coherent request; current pending=true; same original StartedAt; final disposition truthful |
| `VaRest.Http.Trace.QueueAttachWhilePrepare` | snapshot partial, then Note,Completed | original request alive; no attempt; later Queued row coherent |

Each golden asserts exactly the expected note kind, callback counts, ids, fromcache, flags and phase values, not only event names. Use `PhasesHold` and explicit dispatch/ended/response durations; queue wait is outer logical duration and never inserted into an attempt's limiter/auth phase. Replay uses a new logical start so days in queue do not appear as transport duration. Attach after original queued completion sees no historical in-flight request; replay later creates a new record linked by QueueId (Inspector may display an unknown original).

- [ ] **Step 6: Amend the authoritative spec text.** Core4.3 replaces rotating applied-Authorization hash with stable auth identity plus request variation, session import binding trust and refresh preservation. Core4.4 gains identity-scoped replay, two-phase admission/cancel cutoff, shared named queues, same-origin validation and explicit nonsecret persistence guarantee. Core5.2/threading states the narrow GT metadata normalization exception: body-free only, encoded metadata≤64KiB/depth8/2048 lexical units, temporary logical allocation≤2MiB, pre-copy length/count rejection, no payload parse/scan/encoding or disk IO. Explain its canonical allocation parity purpose and bounded per-admission GT cost; M7 measures the cap case and no frame-time guarantee is implied. IN-14 documents durable replay request-body preview suppression before copy/announcement, with immediate Started and pendingfalse; ordinary/response excerpts keep their existing policy. Inspector IN-4 and IN-17 define CacheStored as synchronous memory retention only and require no late Note for asynchronous disk success/failure. Inspector IN-1 adds bCacheLookupPending and states Started initial vs snapshot current; IN-5 adds final CacheLookup/bFromCache; keep13kinds. Document original numeric ID=0 across process-run changes. These are approved controller refinements, not silent implementation drift.
- [ ] **Step 7: Run green and mutation.** Run `Tools/run_tests.sh --no-build VaRest.Http.Trace`, Stats filter, then `VAREST_EXTRA_ARGS=-llm Tools/run_tests.sh --no-build VaRest.Http.Stats.LlmTags`. Add a no-sinks proof: cache/queue requests do not increase EventsBuilt merely because stores operate. Mutate Completed final disposition to None, observe golden failure; remove bConditional, observe 304 failure; drop queue link, observe replay failure. Restore and rerun.
- [ ] **Step 8: Commit:** `git add Source/VaRestHttp/Private/VaRestHttpStatsInternal.h Source/VaRestHttp/Private/VaRestHttpStats.cpp Source/VaRestHttp/Public/VaRestTrace.h Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestCache.cpp Source/VaRestHttp/Private/VaRestOffline.cpp Source/VaRestHttp/Private/VaRestRequestSpec.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/Tests/VaRestHttpTraceCacheOfflineTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpStatsTest.cpp docs/superpowers/specs/2026-10-18-varest2-core-design.md docs/superpowers/specs/2026-10-20-varest2-inspector-design.md && git commit -m "feat(http): expose cache and queue trace goldens and statistics"`.

### Task 11: HTTP fixtures and real-transport cache/restart integration

**Files:** Modify `Tools/testserver/server.py`, `Tools/testserver/selftest.py`, `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`.

**Interfaces:** Consumes existing `/etag`, `Handler.send_json/send_bytes`, testserver LOCK/state, FVaRestClient and TestServer URL helper. Produces `/cache/value` (GET/HEAD) and `/queue/write` (POST), with per-test UUID keys and stable idempotency handling; no production modules depend on fixtures.

- [ ] **Step 1: Write the Python fixture test.** In ServerTest:

```python
def test_cache_and_queue_contract(self):
    key = uuid.uuid4().hex
    path = "/cache/value?key=" + key
    status, headers, data = self.call("GET", path)
    self.assertEqual(status, 200)
    self.assertEqual(headers.get("Cache-Control"), "max-age=1")
    self.assertEqual(headers.get("ETag"), '"' + key + '"')
    status, headers, empty = self.call("GET", path, headers={"If-None-Match": '"' + key + '"'})
    self.assertEqual(status, 304)
    self.assertEqual(empty, b"")
    self.assertEqual(headers.get("Cache-Control"), "max-age=60")
    status, head_headers, head_body = self.call("HEAD", path)
    self.assertEqual(status, 200)
    self.assertEqual(head_body, b"")
    self.assertEqual(int(head_headers.get("Content-Length")), len(data))
    operation = uuid.uuid4().hex
    body = b'{"value":7}'
    request_headers = {"Content-Type": "application/json", "Idempotency-Key": operation}
    first = self.call_json("POST", "/queue/write", body, request_headers)
    second = self.call_json("POST", "/queue/write", body, request_headers)
    self.assertEqual(first[0], 200)
    self.assertEqual(second[2], first[2])
    self.assertEqual(first[2]["applied"], 1)
```

- [ ] **Step 2: Run red:** `python3 -I Tools/testserver/selftest.py -v` (new routes404).
- [ ] **Step 3: Add bounded fixture routes.**

```python
QUEUE_WRITES = {}

def route_cache_value(h, match):
    key = h.query.get("key", [""])[0]
    if not re.fullmatch(r"[0-9a-f]{32}", key):
        h.send_json(400, {"error": "key"})
        return
    etag = '"' + key + '"'
    if h.headers.get("If-None-Match") == etag:
        h.send_bytes(304, b"", headers=[("ETag", etag), ("Cache-Control", "max-age=60")])
        return
    h.send_json(200, {"value": key}, [("ETag", etag), ("Cache-Control", "max-age=1")])

def route_queue_write(h, match):
    operation = h.headers.get("Idempotency-Key", "")
    if not operation or len(operation) > 128:
        h.send_json(400, {"error": "idempotency"})
        return
    digest = hashlib.sha256(h.body or b"").hexdigest()
    with LOCK:
        previous = QUEUE_WRITES.get(operation)
        if previous is None:
            if len(QUEUE_WRITES) >= 4096:
                QUEUE_WRITES.clear()
            previous = {"applied": 1, "bodySha256": digest}
            QUEUE_WRITES[operation] = previous
        conflict = previous["bodySha256"] != digest
        result = dict(previous)
    h.send_json(409 if conflict else 200, result)
```

The inspected server already imports hashlib and socket; keep those existing imports. Add routes before ROUTES compilation: `(("GET","HEAD"),r"/cache/value",route_cache_value)` and `(("POST",),r"/queue/write",route_queue_write)`. Existing send_bytes suppresses HEAD body; verify Content-Length semantics with a Python HEAD assertion. Keep fixture state locked and bounded. Do not mutate existing `/etag` behavior: it is useful for always-revalidate no-cache tests.

- [ ] **Step 4: Add real transport integration tests.**

Register `VaRest.Integration.Cache.Revalidate` with this complete body. Include VaRestCache.h, VaRestConnectivity.h and VaRestClock.h; all other helpers below are already public testing APIs or the existing VaRestTestsIntegrationPrivate helpers.

```cpp
using namespace VaRest::Http::Testing;
using namespace VaRestTestsIntegrationPrivate;
FString BaseUrl;
if (!ServerUrl(*this,BaseUrl)) return false;
FScopedPersistenceTest Disk;
FScopedTraceRecorder Trace;
const auto Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
const auto Connectivity=FVaRestConnectivity::Make(Clock);
const FString CacheRoot=FPaths::Combine(Disk.Root,TEXT("Cache"));
const FString QueueRoot=FPaths::Combine(Disk.Root,TEXT("Queue"));
const FString Path=TEXT("/cache/value?key=")+FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
FVaRestClient Client=MakeClient(BaseUrl);
Client.WithClock(Clock).WithConnectivity(Connectivity).WithStorageDirectories(CacheRoot,QueueRoot)
    .WithAuth(FVaRestAuth::Bearer()).WithCache(FVaRestCachePolicy::MemoryAndDisk());
Client.SetBearerToken(TEXT("integration-owned-session"));
const FString Exported=Client.ExportSession();
auto First=MakeOutcome<FVaRestJson>();
Observe(Client.Get(Path).NoRetry(),First).Send();
if (!TestTrue(TEXT("first complete"),Wait(First)) || !Succeeded(*this,TEXT("first"),First)) return false;
TestFalse(TEXT("first used HTTP"),First->Response.bFromCache);
Clock->Advance(1.0);
auto Validated=MakeOutcome<FVaRestJson>();
const auto ValidateHandle=Observe(Client.Get(Path).NoRetry(),Validated).Send();
if (!TestTrue(TEXT("validation complete"),Wait(Validated)) || !Succeeded(*this,TEXT("validation"),Validated)) return false;
int32 Wire304=0;
for (const auto& Event : Trace.Recorder->For(ValidateHandle.GetId()))
    if (Event.Kind==EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().Status==304) ++Wire304;
TestEqual(TEXT("actual HTTP304"),Wire304,1);
TestEqual(TEXT("logical status200"),Validated->Response.Status,200);
TestTrue(TEXT("joined cached bytes"),Validated->Response.bFromCache);
auto Hit=MakeOutcome<FVaRestJson>();
const auto HitHandle=Observe(Client.Get(Path).NoRetry(),Hit).Send();
if (!TestTrue(TEXT("memory hit complete"),Wait(Hit)) || !Succeeded(*this,TEXT("memory hit"),Hit)) return false;
TestEqual(TEXT("no fabricated wire attempt"),Trace.Recorder->KindsOf(HitHandle.GetId()),FString(TEXT("Started,Completed")));
if (!TestTrue(TEXT("disk persistence finished"),Disk.Drain())) return false;
Client=FVaRestClient();
FVaRestClient Restarted=MakeClient(BaseUrl);
Restarted.WithClock(Clock).WithConnectivity(Connectivity).WithStorageDirectories(CacheRoot,QueueRoot)
    .WithAuth(FVaRestAuth::Bearer()).WithCache(FVaRestCachePolicy::MemoryAndDisk());
if (!TestTrue(TEXT("restore trusted export"),Restarted.ImportSession(Exported))) return false;
auto Restored=MakeOutcome<FVaRestJson>();
const auto RestoredHandle=Observe(Restarted.Get(Path).NoRetry(),Restored).Send();
if (!TestTrue(TEXT("restart hit complete"),Wait(Restored)) || !Succeeded(*this,TEXT("restart hit"),Restored)) return false;
const auto Events=Trace.Recorder->For(RestoredHandle.GetId());
if (!TestEqual(TEXT("restart has no transport"),Events.Num(),2)) return false;
if (!TestTrue(TEXT("completed kind"),Events.Last().Kind==EVaRestTraceKind::Completed)) return false;
TestTrue(TEXT("validated disk hit"),Events.Last().Payload.Get<FVaRestTraceCompleted>().CacheLookup==EVaRestTraceCacheLookup::HitDisk);
auto Head=MakeOutcome<void>();
Observe(Restarted.Head<void>(Path).NoRetry(),Head).Send();
if (!TestTrue(TEXT("HEAD complete"),Wait(Head)) || !Succeeded(*this,TEXT("HEAD"),Head)) return false;
TestEqual(TEXT("HEAD has distinct key"),Head->Response.Attempts,1);
for (int32 Index=0;Index<2;++Index)
{
    auto NoCache=MakeOutcome<FString>();
    Observe(Restarted.Get<FString>(TEXT("/etag")).NoRetry(),NoCache).Send();
    if (!TestTrue(TEXT("no-cache complete"),Wait(NoCache)) || !Succeeded(*this,TEXT("no-cache"),NoCache)) return false;
    TestEqual(TEXT("no-cache always reaches HTTP"),NoCache->Response.Attempts,1);
}
TestTrue(TEXT("final drain"),Disk.Drain());
return true;
```

Register `VaRest.Integration.Offline.RestartReplay` with this body. It restores the same trusted session export but creates a new client/coordinator. Keep metadata-only trace capture; no fixture returns credential values.

```cpp
using namespace VaRest::Http::Testing;
using namespace VaRestTestsIntegrationPrivate;
FString BaseUrl;
if (!ServerUrl(*this,BaseUrl)) return false;
FScopedPersistenceTest Disk;
const auto Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
const auto Connectivity=FVaRestConnectivity::Make(Clock);
Connectivity->ReportOffline();
const FString CacheRoot=FPaths::Combine(Disk.Root,TEXT("Cache"));
const FString QueueRoot=FPaths::Combine(Disk.Root,TEXT("Queue"));
const FString Operation=FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
const FString QueueName=TEXT("restart-account-A-")+Operation;
FVaRestClient Client=MakeClient(BaseUrl);
Client.WithClock(Clock).WithConnectivity(Connectivity).WithStorageDirectories(CacheRoot,QueueRoot)
    .WithAuth(FVaRestAuth::Bearer());
Client.SetBearerToken(TEXT("restart-account-A-token"));
const FString Exported=Client.ExportSession();
Client.WithOfflineQueue(QueueName);
const auto QueueId=MakeShared<FString,ESPMode::ThreadSafe>();
const auto Original=MakeOutcome<void>();
Observe(Client.Post<void>(TEXT("/queue/write")).Body(FVaRestJson::Object().Set(TEXT("value"),7))
    .Header(TEXT("Idempotency-Key"),Operation).Idempotent().NoRetry().QueueWhenOffline(),Original)
    .OnQueued([QueueId](const FString& Id){ *QueueId=Id; }).Send();
if (!TestTrue(TEXT("durable queued"),PumpUntil([QueueId]{ return !QueueId->IsEmpty(); },20,true))) return false;
const auto Flushed=MakeShared<TOptional<bool>,ESPMode::ThreadSafe>();
if (!TestTrue(TEXT("flush admitted"),Client.FlushQueue([Flushed](bool bOk){ *Flushed=bOk; }))) return false;
TestFalse(TEXT("flush not inline"),Flushed->IsSet());
if (!TestTrue(TEXT("flush finished"),PumpUntil([Flushed]{ return Flushed->IsSet(); },20,true))) return false;
TestTrue(TEXT("flush succeeded"),Flushed->GetValue());
TestEqual(TEXT("queued skips original callbacks"),Original->CompleteCalls.load(),0);
Client=FVaRestClient();
if (!TestTrue(TEXT("old references drained"),Disk.Drain())) return false;
FVaRestClient Restarted=MakeClient(BaseUrl);
Restarted.WithClock(Clock).WithConnectivity(Connectivity).WithStorageDirectories(CacheRoot,QueueRoot)
    .WithAuth(FVaRestAuth::Bearer());
if (!TestTrue(TEXT("restored same account"),Restarted.ImportSession(Exported))) return false;
const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
Restarted.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Result){ Results->Add(Result); });
Restarted.WithOfflineQueue(QueueName);
Connectivity->ReportOnline();
if (!TestTrue(TEXT("replayed"),PumpUntil([Results]{ return !Results->IsEmpty(); },20,true))) return false;
if (!TestEqual(TEXT("one result"),Results->Num(),1)) return false;
TestTrue(TEXT("replay succeeded"),(*Results)[0].bSuccess);
TestEqual(TEXT("durable queue id"),(*Results)[0].QueueId,*QueueId);
TestEqual(TEXT("retired before event"),Restarted.GetQueuedCount(),0);
const auto Reply=FVaRestJson::Parse((*Results)[0].Response.Body);
if (!TestTrue(TEXT("raw reply parses"),Reply.HasValue())) return false;
const FTCHARToUTF8 Expected(TEXT("{\"value\":7}"));
const FString Digest=VaRest::Http::FSha256::Hash(MakeArrayView(reinterpret_cast<const uint8*>(Expected.Get()),Expected.Length())).ToHex();
TestEqual(TEXT("replay body unchanged"),Reply.GetValue()[TEXT("bodySha256")].AsString(),Digest);
auto Duplicate=MakeOutcome<FVaRestJson>();
Observe(Restarted.Post(TEXT("/queue/write")).Body(FVaRestJson::Object().Set(TEXT("value"),7))
    .Header(TEXT("Idempotency-Key"),Operation).Idempotent().NoRetry(),Duplicate).Send();
if (!TestTrue(TEXT("duplicate complete"),Wait(Duplicate)) || !Succeeded(*this,TEXT("duplicate"),Duplicate)) return false;
TestEqual(TEXT("fixture applies key once"),Duplicate->Value.GetValue()[TEXT("applied")].AsInt(),1);
TestTrue(TEXT("final drain"),Disk.Drain());
TestEqual(TEXT("no late duplicate event"),Results->Num(),1);
return true;
```

`VaRest.Integration.Offline.ConnectionThenProbe` uses this exact same-origin, fail-once route (the inspected server already imports socket):

```python
QUEUE_FAILURES = set()

def route_queue_flaky(h, match):
    key = match.group(1)
    with LOCK:
        first = key not in QUEUE_FAILURES
        if first:
            if len(QUEUE_FAILURES) >= 4096:
                h.send_json(503, {"error": "fixture-cap"})
                return
            QUEUE_FAILURES.add(key)
    if first:
        h.close_connection = True
        h.connection.shutdown(socket.SHUT_RDWR)
        h.connection.close()
        return
    route_queue_write(h, match)
```

Register POST `r"/queue/flaky/([0-9a-f]{32})"` in the same route table as `/queue/write`. First call closes without a response; every subsequent call reaches the idempotency fixture. The test uses NoRetry, observes Connection→OnQueued, advances the injected connectivity clock10 seconds, then sees one successful queued result. The stored URL/port never changes. Add a Python selftest that expects http.client.RemoteDisconnected for the first POST, then200/applied1 on the second and unchanged result on the third. No fixture mode-control endpoint or existing-route guess is needed.

The complete fail-once integration body (`VaRest.Integration.Offline.ConnectionThenProbe`) is:

```cpp
using namespace VaRest::Http::Testing;
using namespace VaRestTestsIntegrationPrivate;
FString BaseUrl;
if (!ServerUrl(*this,BaseUrl)) return false;
FScopedPersistenceTest Disk;
const auto Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
const auto Connectivity=FVaRestConnectivity::Make(Clock);
FVaRestClient Client=MakeClient(BaseUrl);
Client.WithClock(Clock).WithConnectivity(Connectivity)
    .WithStorageDirectories(FPaths::Combine(Disk.Root,TEXT("Cache")),FPaths::Combine(Disk.Root,TEXT("Queue")))
    .WithOfflineQueue(TEXT("fail-once"));
const FString Path=TEXT("/queue/flaky/")+FGuid::NewGuid().ToString(EGuidFormats::DigitsLower);
const auto Queued=MakeShared<FString,ESPMode::ThreadSafe>();
const auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& Result){ Results->Add(Result); });
Client.Post<void>(Path).Body(FVaRestJson::Object().Set(TEXT("value"),7)).Idempotent().NoRetry()
    .QueueWhenOffline().OnQueued([Queued](const FString& Id){ *Queued=Id; }).Send();
if (!TestTrue(TEXT("connection failure becomes durable queue"),PumpUntil([Queued]{ return !Queued->IsEmpty(); },20,true))) return false;
TestFalse(TEXT("physical failure observed"),Connectivity->IsOnline());
TestTrue(TEXT("not replayed without probe"),Results->IsEmpty());
Clock->Advance(9.999);
TestTrue(TEXT("probe not early"),Results->IsEmpty());
Clock->Advance(0.001);
if (!TestTrue(TEXT("probe completed"),PumpUntil([Results]{ return !Results->IsEmpty(); },20,true))) return false;
if (!TestEqual(TEXT("one queued result"),Results->Num(),1)) return false;
TestTrue(TEXT("physical probe succeeded"),(*Results)[0].bSuccess && Connectivity->IsOnline());
TestEqual(TEXT("same durable item"),(*Results)[0].QueueId,*Queued);
TestEqual(TEXT("consumed"),Client.GetQueuedCount(),0);
TestTrue(TEXT("drain"),Disk.Drain());
return true;
```

Add the exact Python fail-once test and third ROUTES tuple:

```python
def test_queue_flaky_once(self):
    path = "/queue/flaky/" + uuid.uuid4().hex
    body = b'{"value":7}'
    headers = {"Idempotency-Key": uuid.uuid4().hex, "Content-Type": "application/json"}
    with self.assertRaises(http.client.RemoteDisconnected):
        self.call("POST", path, body, headers)
    first = self.call_json("POST", path, body, headers)
    second = self.call_json("POST", path, body, headers)
    self.assertEqual(first[0], 200)
    self.assertEqual(first[2]["applied"], 1)
    self.assertEqual(second[2], first[2])

# Inside ROUTES, before its compiled expansion:
(("POST",), r"/queue/flaky/([0-9a-f]{32})", route_queue_flaky),
```

- [ ] **Step 5: Run green.** `python3 -I Tools/testserver/selftest.py -v`; `Tools/run_tests.sh --no-build VaRest.Integration.Cache`; `Tools/run_tests.sh --no-build VaRest.Integration.Offline`; `Tools/run_tests.sh --no-build VaRest.Integration` (all existing HTTP+WebSocket fixtures remain stable). Use GUID routes to avoid cross-test state. Preserve each runner's own log before the next overwrites it if recording detailed evidence.
- [ ] **Step 6: Commit:** `git add Tools/testserver/server.py Tools/testserver/selftest.py Source/VaRestTests/Private/VaRestIntegrationTest.cpp && git commit -m "test(http): verify cache revalidation and durable replay against HTTP"`.

### Task 12: Public documentation, milestone gates and M5 carries

**Files:** Create `docs/dev/cache-offline.md`; modify `CLAUDE.md` and mirrored `AGENTS.md` for developer/runbook guidance, core/Inspector specs only to reconcile approved Task 10 changes; update the controller's execution ledger/report. Beads operations remain controller-owned.

**Interfaces:** Consumes completed implementation and test evidence; produces reviewed user documentation and milestone evidence, no new runtime behavior.

- [ ] **Step 1: Write a copyable code-first example.**

```cpp
auto Connectivity = FVaRestConnectivity::GetDefault();
FVaRestCachePolicy CachePolicy = FVaRestCachePolicy::MemoryAndDisk();
CachePolicy.DefaultTtlSeconds = 30;
CachePolicy.bServeStaleOnError = true;
FVaRestClient Api = FVaRestClient::Make(TEXT("https://api.example.com"));
Api.WithConnectivity(Connectivity)
   .WithAuth(FVaRestAuth::Bearer())
   .WithCache(CachePolicy)
   .WithOfflineQueue(TEXT("GameApi.Account42"));
Api.SetBearerToken(TEXT("token supplied by the game"));
Api.OnQueuedResult().AddLambda([](const FVaRestQueuedResult& Result)
{
    // Associate Result.QueueId with the game's operation record.
});
Api.Post<void>(TEXT("/orders"))
   .Body(FVaRestJson::Object().Set(TEXT("productId"), 42))
   .Idempotent().QueueWhenOffline()
   .OnQueued([](const FString& QueueId) { /* save the nonsecret operation id */ })
   .Send();
```

Document the accepted-persistence completion permit and early ordinary Config-refusal exception; conservative canonical metadata charging before any admission payload copy; and the explicit absence of durable replay request-body previews. Document WithCache/Cache/NoCache and TTL/default/caps; no-cache/no-store/must-revalidate; stable auth identity and trusted ExportSession binding; persisted cache bytes decoded per request; exclusions and Vary conservatism; credential-free known-secret boundary and application responsibility; queue account naming/event-sharing domain; same-origin only; original callbacks vs OnQueued and OnQueuedResult; AuthMode preservation; at-least-once crash replay and idempotency; admission cancellation cutoff; FlushQueue bool admission, asynchronous result and shutdown suppression; memory-only CacheStored note semantics; shared/default vs injected connectivity; ForceOffline non-Shipping; root changes and caps; corruption behavior and retryable storage failures. Never place real token example values in persisted fixtures.

- [ ] **Step 2: Record the freeze and carries.** Freeze `EVaRestCacheMode`, `FVaRestCachePolicy` Mode/MemoryBytes/DiskBytes/DefaultTtlSeconds/bServeStaleOnError; `FVaRestOfflinePolicy` MaxQueued/MaxAgeSeconds/MaxItemBytes/MaxBodyBytes; `FVaRestQueuedResult` QueueId/Tag/bSuccess/Response/Error; builder/client names used above. M5 owns reflected hub/config wrappers, asset default queue name = asset path plus explicit account isolation policy, bQueueWhenOffline endpoint flag, per-Hub connectivity ScopeId, settings directory exposure, and Deinitialize cancel+asynchronous queue flush lifecycle including false admission handling and a reflected queued-result wrapper around the plain C++ response type. Core process default scope0 is not a claim of GameInstance isolation. M5 must retain runtime readback and OnQueuedResult event semantics, not create a second queue implementation.

Document these repaired contracts in docs/dev/cache-offline.md and the corresponding core sections in Step2: a QueueWhenOffline operation anchors identity/tenant at its initial Resolve and refuses account/tenant replacement even before any queue write; same-identity token refresh stays supported. ClearCache targets the current configured root/origin even before first lookup. Required cache invalidation/capacity maintenance may quarantine a namespace and retry on later access; bounded intent exhaustion disables disk cache reuse for this process, and a failed/refused clear is never a durable restart acknowledgement. Queue recovery refuses an incompatible smaller policy without deleting acknowledged records; it exposes/replays no partial prefix. New admissions also enforce the10MiB retained metadata ceiling, with count/body/encoded budgets remaining independent. FlushQueue(true) reserves asynchronous delivery through success and IO-refusal paths; false retains no callback. Cache/stale/synthetic outcomes never update automatic reachability. Silent owned persistence shutdown may complete internal accounting synchronously, with user callbacks suppressed; ordinary request API callbacks remain deferred. Add the whole-candidate sticky secret-work refusal and its persistence-only cost, not an implication that live response handling fails. Explain the conservative JSON guard reservation (8×body+2MiB), which can refuse persistence before the nominal 32MiB item maximum; the scan is never truncated. The128MiB executor charge is a logical bound for owned worker/results, not an RSS limit; caller/exchange body ownership and the separately bounded cache-maintenance intent remain distinct.

- [ ] **Step 3: Run final normal gates sequentially.**

```bash
python3 -I Tools/testserver/selftest.py
python3 -I Tools/testserver/ws_selftest.py
Tools/run_tests.sh --no-build VaRest
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/tmp/varest-m4b-development-actions.json
# Pause for the delegated plugin-only graph gate; then build this exact source version.
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Shipping -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/tmp/varest-m4b-shipping-actions.json
# Pause for the delegated plugin-only graph gate; then build this exact source version.
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Shipping -project=/w/Unreal/VaRestHost/VaRestHost.uproject
```

Expected: every matched test green, both monolithic targets succeed, no new plugin compiler warning. Run a separate excerpts-off build only if the new trace payload code adds a macro-conditioned path; verify queue/cache do not require Inspector capture or body excerpts. Never run a second UE build concurrently.

- [ ] **Step 4: Perform guarded NoPCH/nonunity verification.** Temporarily change only VaRestHttp.Build.cs and VaRestTests.Build.cs to `PCHUsageMode.NoPCHs; bUseUnity=false`; snapshot exact original content. Dry-run:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject -WriteOutdatedActions=/tmp/varest-m4b-actions.json
```

Inspect the complete action JSON source paths, command arguments and produced outputs; every Compile/Link must be plugin-owned. UHT can alter generated engine freshness even during dry-run; this is not described as read-only. Stop on engine/host actions and report exact graph for controller ruling. No global DisableUnity/CompilerArguments flags, engine source edits, generated-file deletion, cache/timestamp manipulation or hand reusing objects. If a controller accepts compile-only fallback, record standalone-TU proof separately from NoPCH link/run; do not call one the other. Restore exactly the temporary Build.cs changes, re-run a normal dry-run with the same guard, then normal module smoke. Use the actual current release-program rulings; W1's past bounded recovery does not authorize a different M4b graph.

- [ ] **Step 5: MSVC and source-shape review.** Run `rg -n '^using namespace|\b(auto|int32|uint64|FString) (State|Config|Body|Headers|Name|Key|Count|Result|Callback)\b' Source/VaRestHttp` as a candidate list, then inspect introduced scopes against member/function names. Do not treat regex matches as compiler findings. Confirm each new translation unit directly includes everything it uses, UHT generated header is last, no untrusted FName construction before bounds, no unguarded `.GetValue()`/array index after a failed nonfatal assertion. Win64/Mac/mobile builds remain the release hardware pass; do not claim Linux proves MSVC compilation.

- [ ] **Step 6: Verify exit and hand off.** Check core8 M4 “stage tests green, restore-the-bug done” for M4a plus this milestone; evidence lists every restored bug and exact failing assertion then final pass. Controller records open residuals as Beads, closes only the appropriate M4b child and M4 parent when both are landed, and performs authorized local merge. Push/tag/Fab upload remain owner actions. Hand off changed files, test/build counts, any platform limits and M5 carries.
- [ ] **Step 7: Commit docs after checks.** `git add docs/dev/cache-offline.md docs/superpowers/specs/2026-10-18-varest2-core-design.md docs/superpowers/specs/2026-10-20-varest2-inspector-design.md CLAUDE.md AGENTS.md && git commit -m "docs(http): document cache queue and connectivity contracts"`. The controller manages the milestone merge; no push in this plan.
