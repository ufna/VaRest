# HTTP cache, durable offline queue and connectivity

VaRest 2 caching and durable queueing are opt-in. Configure clients and send requests on the game thread. Keep the client alive for future queue replay and result subscriptions; a request handle is weak and does not provide that lifetime.

## Configure a client

```cpp
#include "VaRestAuth.h"
#include "VaRestCache.h"
#include "VaRestClient.h"
#include "VaRestConnectivity.h"
#include "VaRestJson.h"
#include "VaRestOffline.h"

auto Connectivity = FVaRestConnectivity::GetDefault();
FVaRestCachePolicy CachePolicy = FVaRestCachePolicy::MemoryAndDisk();
CachePolicy.DefaultTtlSeconds = 30;
CachePolicy.bServeStaleOnError = true;
FVaRestClient Api = FVaRestClient::Make(TEXT("https://api.example.com"));
Api.WithConnectivity(Connectivity)
   .WithAuth(FVaRestAuth::Bearer())
   .WithCache(CachePolicy)
   .WithOfflineQueue(TEXT("GameApi.Account42"));
Api.SetBearerToken(TEXT("token supplied by the game")); // Replace from your login flow.
Api.OnQueuedResult().AddLambda([](const FVaRestQueuedResult& Result)
{
    // Associate Result.QueueId with your nonsecret operation record.
    // Result.Response.Body contains the raw replay response bytes.
});
Api.Post<void>(TEXT("/orders"))
   .Body(FVaRestJson::Object().Set(TEXT("productId"), 42))
   .Idempotent().QueueWhenOffline()
   .OnQueued([](const FString& QueueId) { /* save the operation id */ })
   .Send();
```

The example shows configuration inside an application-owned lifetime; retain `Api` beyond this startup block. The placeholder is not a credential to ship. A fresh bearer replacement does not restore a previous session identity merely because the token text or queue name matches. For same-account restart continuity, restore an unchanged trusted `ExportSession()` result using `ImportSession()`. The game owns secure storage of exported credentials; the queue does not persist them for you.

## Cache policy and freshness

`WithCache(Policy)` sets the client policy; builder `.Cache(Policy)` overrides it for one request. `.NoCache()` bypasses both lookup and storage. `Memory()` and `MemoryAndDisk()` return policy values. Defaults are mode `None`, 8 MiB memory, 64 MiB disk, fallback TTL 0 seconds and stale fallback disabled. Default disk root is `Saved/VaRest/Cache`. Memory and disk use bounded LRU retention; limits can reduce hit rate. A shared disk-store instance retains the smallest capacity ever requested during its lifetime. Increasing a later policy does not increase that instance's cap until all holders and work release it and it reopens.

Disk-cache records have a 12 MiB encoded ceiling. A default-policy disk read reserves 112 MiB in the logical persistence account: `8 × min(effective retained encoded capacity, 12 MiB) + 16 MiB`. The shared store's retained capacity can be smaller than a later policy requests. Conservative record/workspace checks can refuse persistence below nominal payload or capacity limits, reducing concurrent disk admissions and cache hits and causing extra network requests under contention. These cache-specific limits are separate from the queue's 32 MiB raw item and 64 MiB aggregate body limits; best-effort cache refusal does not fail a successful live response.

Only eligible GET/HEAD status-200 representations are retained. GET and HEAD use different keys; HEAD never replaces GET bytes. Request bodies, Range/If-Range, caller conditional headers, SaveTo, refresh, connectivity probes and custom PerAttempt stages bypass caching. Redirect responses and 206 are not stored. Auth without a usable stable identity bypasses caching. Both original built headers and the actual session Set-overlay must be eligible; an overlay cannot erase a caller's no-store restriction. NoAuth is independent of session headers and the configured provider.

Keys hash framed verb, resolved URL without fragment, stable identity and all known built/session request headers. Authenticated identity also includes auth mode and canonical provider/contract semantics; undeclared providers use their live-instance namespace. Names are case-folded; values are case-sensitive. Key filenames/metadata contain no raw URL, credential or header value. `Vary:*`, and Vary naming a header absent from those known inputs, refuse retention. This includes transport defaults and auth-only headers whose values are not known there. There is no custom-stage vary contract in M4b.

Request `no-cache` and numeric `max-age=0` (including quoted zero) require validation online and forbid fresh/stale delivery offline. Only actual directive tokens count; text inside quoted extension values does not. Original and applied session headers are both checked.

Strict nonnegative `max-age` takes precedence; invalid or conflicting repeated ages give zero lifetime. `Expires` relative to `Date` (or receive UTC) is the next source, and `DefaultTtlSeconds` is the last fallback. `Age` and apparent age reduce remaining freshness. Equality at expiry is stale; future timestamps do not extend freshness. `private`/`public` add no shared-proxy behavior to this private client cache.

- `no-store` forbids retention and invalidates an old matching entry.
- `no-cache` allows retained bytes but requires validation on every use.
- `must-revalidate` prevents offline stale use.
- `bServeStaleOnError` permits Connection/Offline fallback, including a known-offline lookup. It does not hide Timeout, HTTP 4xx/5xx, Auth, Decode or Config failures, or override no-cache/must-revalidate.

An unretainable replacement 200 or a failed best-effort write can leave an older retained generation available under its own freshness and opted-in stale policy. Mandatory no-store/Vary:* invalidation, explicit clear and capacity controls still apply.

A conditional wire 304 can return the captured representation as logical status 200 with `bFromCache=true`; its actual attempt still records 304. Allowed end-to-end metadata merges, excluding protected auth/session fields, Set-Cookie, hop-by-hop headers, Transfer-Encoding and stale Content-Length. GET length reflects retained bytes; HEAD preserves representation length. A 304 without a captured entry is an ordinary HTTP outcome.

A 304 does **not** always renew TTL. A response without Cache-Control/Date/Age/Expires preserves parsed policy and its original age anchor, potentially requiring validation immediately again. Updates with unavailable inherited Cache-Control provenance or ambiguous/default-dependent policy can satisfy the current request while refusing future retention. Incomplete or invalid new age information retains a conservative old-age floor; entirely valid explicit Date/Age updates can replace it. A no-store 304 may satisfy the current request from captured bytes while invalidating stored copies. These conservative rules can cause extra validation or network misses.

Stored bytes are decoded afresh for the request's result type, envelope and MaxJsonBytes limit. Identity/key are checked again before disk delivery and storage; account replacement strips old cache validators before dispatch. An old lookup may complete with fresh network bytes while declining retention after a generation/account change. A new resolved request can retain normally.

`ClearCache()` invalidates client memory immediately and schedules clearing the **currently configured** disk root/origin, even before its first lookup. Epochs and serial work prevent late writes/reads from undoing it. Mandatory invalidation and capacity maintenance can quarantine a namespace and retry on later access. Exhausting the bounded maintenance-intent account disables disk reuse for this process. A refused or failed clear is not a durable restart acknowledgment. Logout changes identity; it does not delete every account's stored data.

An optional disk read refused under ordinary executor contention becomes a miss while persistence still accepts work, allowing live HTTP (or Offline on a known-offline miss). Stopped persistence and completion-permit refusal remain Config. Mandatory queue work cannot silently become an optional miss. These bounds provide no fairness guarantee.

## Identity and queue sharing

Bearer/BearerRefresh use an opaque session identity distinct from token generation. Refresh and exactly identical session data retain it; real replacement, clear and expiry change it. Tenant/session-header changes also change request scope. API key and Basic identity derive from stable credential material. NoAuth has an independent anonymous domain, with explicit request Authorization/Cookie still included in scope/key checks.

Export/import uses an additive consistency binding to validate identity continuity before mutation. A missing/mismatched binding normally creates fresh identity. An explicit `bSameIdentity=true` import with a nonempty live session is a trusted caller assertion and preserves that live identity; without a live session, valid exported binding is needed for continuity. The unkeyed binding is not authentication. Never edit or accept another party's exported credentials as trusted state.

Custom `IVaRestAuth::GetIdentity(Session, OutIdentity)` is a pure GT query, never speculative `Apply`. Return a usable opaque identity that changes with account and remains stable through refresh. The default false result bypasses cache and refuses durable queueing with Config. The optional pure GT `GetIdentityContractName()` defaults to `NAME_None`: an identity-capable custom provider then shares cache and queue semantics only with that same live instance. A weak registry assigns a random GUID namespace, not a raw pointer. Replacing it or restarting misses old cache entries and ends old queued records with the normal Auth terminal result. Old cache keys can become unreachable without migrating queue records. To support compatible instances/restarts, declare matching nonsecret stable contract and auth-name semantics **and** supply the same real current identity. Only FName provider/contract labels are canonicalized to FName equality; opaque identities, header values, exact queue names and Linux roots retain their own case semantics.

A queue name is an explicit durable sharing and event domain. Its directory is `Saved/VaRest/Queue/<SHA256(domain, exact name)>` by default. Use account-scoped names, and decide how login/logout should retain or retire old work. Same root/name shares one FIFO coordinator. Live registrants must agree on canonical origin, connectivity object, policy and provider semantics. Conflicts refuse queue-enabled sends with Config. Reconfiguration publishes either a complete registration or an unavailable Config gap; it never mixes an old coordinator with new settings. Sends already holding an old complete snapshot keep it. During the gap, current `GetQueuedCount()` does not expose the old count and `FlushQueue()` refuses.

Queue-enabled operations capture original account/tenant/origin scope at initial Resolve, before stage, load or limiter waits. Replacement refuses dispatch/admission with Auth instead of relabeling the operation. Same-identity refresh is supported. Actual URLs must remain same-origin at save, load and replay; a queue is not a cross-origin forwarding mechanism.

## Admission, replay and callbacks

`.QueueWhenOffline()` requires a named queue, an idempotent HTTP verb or `.Idempotent()`, no streamed/file multipart body, no SaveTo and no OnHttpThread. Invalid combinations fail Config even online. Refresh never queues. Known-offline opted-in requests queue immediately; otherwise a final Connection/Offline outcome may queue after its normal retry cycle. Original body bytes remain available. `.Idempotent()` creates a stable Idempotency-Key before the original attempt; the server must enforce it for operations where duplicate effects matter.

Admission prepares and flushes a nonreplayable `.pending` file on a worker. GT then rechecks scope, owner and actual deadline and races any-thread cancellation through an atomic commit gate. Before that gate cancellation/deadline wins promptly and leaves no replayable item. After commit selection the rename is beyond rollback: `IsActive()` becomes false while completion remains pending. Actual rename success delivers `OnQueued(QueueId)`; actual failure follows ordinary error completion. The public handle has `GetId()`, `IsActive()` and `Cancel()`, with no public completion getter.

Successful queue acknowledgment invokes OnQueued, not the original typed success/error/complete callbacks. Committed work remains accounted even if an Outer stage already finished; a sealed request receives no late Queued Note or callback. A postcutoff cancellation bit alone does not suppress an otherwise valid acknowledgment. Owner loss/shutdown suppress user delivery while committed IO drains.

Replay selects the oldest live compatible client, retaining it only for the active exchange. It uses current credentials after scope checks, preserves AuthMode/key/body and bypasses cache reads/writes even for GET/HEAD. Original callbacks/owners/result types are not serialized. One `OnQueuedResult` event per live registration (not per copied handle) follows durable retirement; the subscriber snapshot makes reentrant subscription changes apply to later events. Keep a live registration for replay and subscriptions.

`FVaRestQueuedResult` contains QueueId, Tag, bSuccess, Response and Error. Successful raw replay bytes are in `Response.Body`; an error retains its existing response body. The current const-result adapter makes one owned response-buffer copy, outside the persistence executor's account. It is not a copy-free or total-memory guarantee.

A scope mismatch retires the head as Auth; invalid origin/record as Config; expiry as Offline, without network. Connection/Offline retains the head and pauses replay; other final outcomes consume it. One FIFO runner starts the next item only after prior retirement. Reentrant admissions during load remain bounded and ordered behind older waiters. Load-to-admission handoff uses a bounded owned continuation only after the Load reservation releases; refusal makes storage unavailable without exposing partial recovery.

Replay is **at-least-once across crashes**. A crash after server success but before durable retirement can send the same operation again with the same key. No exactly-once remote execution is promised. QueueId is the durable link; `QueueReplayOf.OriginalRequestId` is zero in another process run rather than aliasing a reused numeric request id.

## Limits, persistence and recovery

| Policy / account | Default or bound |
| --- | --- |
| `FVaRestOfflinePolicy::MaxQueued` | 256 committed/reserved items; loading waits are also bounded and visible in count |
| `MaxAgeSeconds` | 86400 seconds (24 hours) |
| `MaxItemBytes` | 32 MiB body before base64; also the supported per-item maximum |
| `MaxBodyBytes` | 64 MiB aggregate queued bodies |
| Canonical encoded metadata | 64 KiB per record; depth 8; 2048 lexical units |
| Canonical retained metadata | 10 MiB queue account, charging max(live, recovered) representation for restart parity |
| Metadata normalization workspace | At most 2 MiB logical temporary allowance; body-free GT work |
| Persistence executor worker/results | 128 MiB logical reservation account |
| JSON guard reservation | 8 × body bytes + 2 MiB; may refuse below the nominal 32 MiB item maximum |

Length/count gates and canonical metadata normalization precede admission payload copies; count/body/metadata reservations precede ownership. Encoded storage/temp occupancy is separately bounded, not equal to body size. Caller/exchange buffers, coordinator bodies and bounded cache-maintenance intent are separate accounts. The 128 MiB limit is not an RSS, allocator-fragmentation or frame-time promise. Increasing policy values cannot bypass executor/codec limits. An incompatible smaller recovery policy refuses with acknowledged files preserved and no partial prefix exposed; reopen under compatible limits after all old holders release.

Persistence checks the entire candidate body and metadata off GT, including every in-memory multipart descriptor name plus the whole encoded body. It excludes configured auth/session fields and registered known-secret byte forms independently of trace minimum length/excerpt caps. Whole-candidate work/capacity exhaustion is sticky and refuses persistence; it never truncates the scan or silently forgets a configured secret. Cache refusal does not fail a successful live response; mandatory queue refusal does fail admission. Short known-secret byte/hex forms can conservatively collide with otherwise benign encoded metadata or hashes. Applications must register additional secrets; unknown application secrets cannot be inferred. A nonempty protected key that normalizes to no usable matching rule (for example, separator-only text) refuses persistence conservatively. Protected-key additions can outlive header removal/session clear; recovery can require a fresh client/config without that unusable rule. Refusal never redacts bytes into a different replay operation.

`WithStorageDirectories(CacheRoot, QueueRoot)` selects app-owned writable roots. Mobile writable physical mapping occurs before lexical normalization; status and IO use the same target. Invalid used storage configuration fails conservatively, without making an unused root break ordinary HTTP. Linux registry equality **and hashing** distinguish case-sensitive roots. Typed Missing differs from access/IO errors. Corrupt cache records are misses; queue recovery validates schema, origin and budgets and preserves acknowledged work when recovery is refused. Pending files are nonreplayable and cleaned during load; failed cleanup exposes no partial queue.

For queue storage, the configured `QueueRoot` is limited to 4096 TCHAR before namespace copying. The generated queue namespace is also limited to 4096 TCHAR, with allocated `FString` storage at most 8192 TCHAR units. Exceeding these guards refuses registration and queue-enabled work as Config. OS/native path validity and length restrictions apply separately and can be stricter.

This includes failure to remove a partially written temporary `.part` before Prepare returns: handles are closed and actual cleanup is resolved on the worker. Known-clean validation/admission/write failures keep storage usable while Flush still reports the failed write.

Cleanup failure/refusal marks storage unavailable and Flush failed before releasing bounded accounting. Prompt logical cancellation can precede physical cleanup and count release. A known-clean failed commit still fails Flush but need not disable future admissions. The verified recovery route repairs permissions, drains work, fully releases the failed coordinator, then reopens it for normal load cleanup. Disabling/re-enabling a single client only accomplishes this when no other client or old snapshot retains that coordinator; there is no public reset framework.

Generated leaf names and known-symlink refusal protect ordinary app-owned paths. Linux uses actual lstat-aware checks for tested leaf/root links. This is not a hostile-filesystem sandbox against ancestor aliases or concurrent replacement; other-platform alias/symlink and device storage behavior need native validation. Flush plus atomic rename establishes complete-file visibility; the portable filesystem interface does not guarantee directory fsync or power-loss durability. Client replacement tests are not process-kill/restart tests.

## Flush, shutdown and connectivity

`bool FlushQueue(TUniqueFunction<void(bool)>&& OnFlushed = {})` flushes admitted persistence, not network replay. Handle the immediate bool separately from the callback's IO result: false retains no callback (invalid registration, shutdown or bounded delivery/waiter refusal); true reserves asynchronous delivery and completes after the admitted-work watermark and serial barrier, including accepted IO-refusal failure paths. Owner loss/shutdown can suppress that user callback. It never blocks or pumps a nested GT loop.

Accepted persistence waits reserve bounded completion ownership before jobs/waits, so shutdown can drain their own results/continuations. Early permit refusal creates no persistence ownership and uses the ordinary asynchronous Config completion path outside that owned-drain guarantee. Silent owned shutdown may settle internal accounting synchronously with user callbacks suppressed; ordinary request callbacks remain deferred. Future Hub teardown must cancel in-flight work, request asynchronous flush and explicitly handle false admission before releasing its lifetime.

`GetDefault()` shares process scope 0. For independent scopes use `FVaRestConnectivity::Make(Clock)` and inject it with `WithConnectivity`; changing a client's clock alone does not retime connectivity. Process scope 0 is not GameInstance isolation. A current actual physical Connection failure observes offline; any received HTTP response establishes online, including error status, even if body timeout later makes the logical result fail. Closed/orphaned dispatches, cache/stale/synthetic results, local refusals and status-zero timeouts do not establish reachability. The discovering request preserves Connection and its retry cycle; later ordinary known-offline requests consult cache, then return Offline on miss.

Offline probes default to 10 seconds and prefer eligible queue heads across every owner in the shared scope before considering an optional same-origin `WithHealthUrl` HEAD. Without either there is no periodic request. Non-Shipping `VaRest.Http.ForceOffline` suppresses probes and masks effective state; physical observations remain separate. Removing it reveals the latest observation and only probes if that state is offline. It does not guarantee a fresh network check.

`OnChanged()` is a narrow **synchronous GT observer** exception: state changes first; nested edges queue FIFO without recursive notification; final state reconciles probes. Request outcomes, OnQueued, OnQueuedResult and accepted Flush callbacks remain asynchronous relative to Send, Cancel, setters and connectivity transitions.

## Trace and M5 integration freeze

Started stays immediate, with actual cache policy and `bCacheLookupPending` during unresolved reads; snapshots show current state and Completed carries final CacheLookup and bFromCache independently. Cache hits have no transport attempt. Queued/stale/expiry notes before an attempt use the real RequestId and Attempt0, never an invented attempt. CacheStored means actual synchronous memory retention before Completed; best-effort disk writes neither delay successful delivery nor emit late request Notes. A disk-only stored response may have no CacheStored Note.

`Completed.Response` adds final delivered status, separately gated/redacted headers, redacted effective URL/content type and a scalar body descriptor. BodyBytes is captured under the trace mask before decode consumes the body; it is independent of wire BytesDown. A wire304 may deliver200 with cached entity bytes and zero wire bytes. Memory describes a captured in-memory entity; Stream keeps unknown size (-1). A late sink that missed the scalar capture, a queued acknowledgement or a local completion with no response keeps unknown storage/size; absent response status is0. Empty cached EffectiveUrl remains empty. Cache/stale/304 delivered previews have no producer and remain unavailable; physical304 excerpts are not a substitute. No extra body copy is introduced.

HTTP starts/ends/response excerpts carry public uint64 DispatchId from the existing captured uint32 dispatch serial (1..MAX_uint32). Pair physical events by RequestId+DispatchId, independently of Attempt and delivery order. Backlog/Discarded retain their promised response id; request/frame excerpts use0. The private generator and thirteen trace kinds remain unchanged.

Durable replay request-body previews are disabled in O(1) before copy/preparation/announcement, including short secrets. Ordinary request and response excerpt/redactor behavior is unchanged. Cache/queue correctness does not require Inspector capture or body excerpts.

The M5 consumer freeze preserves these exact names and fields:

| Type / surface | Frozen spelling |
| --- | --- |
| `EVaRestCacheMode` | `None`, `Memory`, `MemoryAndDisk` |
| `FVaRestCachePolicy` | `Mode: EVaRestCacheMode`, `MemoryBytes: int64`, `DiskBytes: int64`, `DefaultTtlSeconds: double`, `bServeStaleOnError: bool`; `Memory()`, `MemoryAndDisk()` |
| `FVaRestOfflinePolicy` | `MaxQueued: int32`, `MaxAgeSeconds: double`, `MaxItemBytes: int64`, `MaxBodyBytes: int64` |
| `FVaRestQueuedResult` (plain C++) | `QueueId: FString`, `Tag: FName`, `bSuccess: bool`, `Response: FVaRestResponse`, `Error: FVaRestError` |
| Builder | `Body`, `Cache`, `NoCache`, `QueueWhenOffline`, `OnQueued`, `Idempotent`, `Send` |
| Client | `Make`, `Post`, `WithAuth`, `SetBearerToken`, `WithCache`, `ClearCache`, `WithOfflineQueue`, `GetQueuedCount`, `FlushQueue`, `OnQueuedResult`, `WithConnectivity`, `WithHealthUrl`, `WithStorageDirectories` |
| Optional auth contract | `GetIdentity`, `GetIdentityContractName`, existing `GetAuthName` semantics |

M5 supplies reflected Hub/config wrappers, a reflected queued-result wrapper around the plain C++ response, asset-path default queue naming with explicit account isolation policy, endpoint `bQueueWhenOffline`, per-Hub connectivity/ScopeId and settings directory exposure. It preserves runtime readback and OnQueuedResult semantics, and implements Deinitialize cancellation plus asynchronous flush with false-admission handling. It consumes this one queue implementation.
