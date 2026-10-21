# VaRest 2 — M4a: retry, auth and session, envelope, trace contract, stats — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the attempt-shaping stages of spec §4 to `VaRestHttp` — retry with backoff and `Retry-After` (§4.1), auth with a single-flight session refresh (§4.2), the response envelope (§3.4) — plus the public pieces the WebSocket spec needs from M4 (§19 items 1, 2, 4, 6), the M4 half of the Inspector trace contract (IN-2 gap fields, IN-4 `Note`, IN-5 `Envelope`, IN-6 `Excerpt`, IN-9 `Marker`, IN-14, the IN-17 goldens that do not need a cache or a queue), `stat VaRest`, and the M3 carries.

**Architecture:** Two new built-in `Outer` stages sit between the custom `Outer` stages and the attempt boundary — `Retry`, then `AuthGate`, which parks a request behind one shared refresh and replays it as a new attempt — and one new built-in `PerAttempt` stage, `AuthApply`, writes credentials into each attempt before the custom `PerAttempt` stages. A stage that holds an exchange waiting (a backoff timer, a park) registers an interrupt that `Cancel`, owner-gone and shutdown call, so every hold ends in exactly one outcome. The envelope is applied on the decode task, next to the JSON parse. Session state (tokens, expiry, session headers) is live per client, on the game thread; its secrets feed the redactor, and its changes are trace markers and client events.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), C++20, `UE::Tasks`, engine HTTP module, UE Automation tests on the fake transport and virtual clock, Python 3 standard library (test server).

**Spec:** `docs/superpowers/specs/2026-10-18-varest2-core-design.md` — §3.4, §4 (chain), §4.1, §4.2, §4.7 (the retry deadline), §4.8 (stats), §7.1 (the `VaRest.Http.*` list for retry timing and `Retry-After`, idempotency, single-flight refresh, envelope), §8 row M4. Also binding: `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` §19 items 1, 2 (with rev 2 §22 A1), 4, 6 and §3.3-§3.4 (what the socket calls); `docs/superpowers/specs/2026-10-20-varest2-inspector-design.md` §2.2, §2.3 and §9 rows IN-2, IN-4, IN-5, IN-6, IN-9, IN-14, IN-17; `docs/superpowers/specs/2026-10-20-varest2-openapi-design.md` §17 N15 (auth mode, `OAuth2Form` refresh) and §7.7. M3 history: `.superpowers/sdd/m3-plan-rulings.md`, `.superpowers/sdd/release-2.0-program.md` ("M3 execution rulings"), `.superpowers/sdd/m3-final-review.md`. M4b (cache, offline queue, connectivity) is outlined in `docs/superpowers/plans/2026-10-21-varest2-m4b-cache-offline.md`.

## Why M4 is two plans

Spec §8 row M4 holds seven features. Written to M3's standard they are about 31 tasks, past the ~20 a plan can carry. The seam: **M4a** is everything that shapes an attempt or a session and everything the WebSocket milestones need from M4 (W2 needs the clock, backoff and random stream; W5 the auth, session and trace contract). **M4b** is the two stages that answer a request without the network or hold it for later — cache and offline queue — and connectivity, which they share. M4b adds two stages in front of `Retry` (spec §4 order: offline queue → cache → retry) and touches nothing M4a builds except to read its auth identity (the cache key) and to replay through its stages. The Inspector's I1 needs both halves.

## Design decisions this plan makes (the specs are silent or say otherwise)

M3's decisions 1-34 (`docs/superpowers/plans/2026-10-21-varest2-m3-http-core.md`) stand. New ones:

1. **A client does not retry until it is given a policy** (`WithRetry`, or a request's `Retry()`). Spec §4.1 gives `FVaRestRetryPolicy`'s defaults, and §3.1 shows `.WithRetry(...)` written out; §5.5's settings ("default ... retry") are M5's, so M5 decides the project default. It also keeps every M3 test valid: a retried 503 or timeout on the virtual clock would wait for an `Advance` no M3 test makes. Cost if wrong: one default flips in M5.
2. **Stage order** (spec §4's diagram, made concrete): custom `Outer` → **`Retry`** → **`AuthGate`** → attempt boundary → **`AuthApply`** → custom `PerAttempt` → log → limiter → transport. M4b inserts offline queue and cache before `Retry`. A session replay goes back through the attempt boundary, so it is a new attempt (Inspector IN-2 `GapBefore = SessionRefresh`), and it is not a retry: `MaxRetries` counts only `Retry`'s re-issues.
3. **A refresh that fails on the network keeps the session.** Spec §4.2 says "If the refresh fails, all parked requests fail with `Kind=Auth` and the client fires `OnSessionExpired`." Pushed back: a dropped Wi-Fi during a refresh would log the player out. Rule: a **rejected** refresh (a 4xx other than 408/429, or a 2xx without an access token) expires the session — tokens dropped, `OnSessionExpired`, parked requests `Kind=Auth`. An **unreachable** one (connection error, timeout, 408, 429, 5xx) changes nothing; parked requests fail with that kind (`Connection` or `Timeout`, retryable), so `Retry` (outside `AuthGate`) and M4b's offline queue can act on them. Cost if wrong: one more outcome to explain in the docs.
4. **`EVaRestAuthMode {Required, IfAvailable, None}` per request** (OpenAPI N15's mode, spec §3.3's "`Auth` (… no credentials)"). `Required` (default): when the client has an auth and it has no credentials (and cannot refresh), the request fails `Kind=Auth` without being sent. `IfAvailable`: sent without. `None` (`NoAuth()`): no credentials, no session headers, no park, no replay. A client with no auth configured sends as it did in M3. Cost if wrong: a game that relied on "send anyway and get a 401" sets `IfAvailable`.
5. **`OnSessionChanged(EVaRestSessionChange)` is the one session event** besides `OnSessionExpired`: WebSocket §19 item 2's `OnSessionCleared` is `OnSessionChanged(Cleared)` (its rev 2, §22 A1, replaced the separate event). A token the game sets (`SetBearerToken`, `SetSession`, `ImportSession`) is `Replaced` unless the caller passes `bSameIdentity = true` (then `Refreshed`); the auth's own refresh is `Refreshed`; `ClearSession` is `Cleared`. VaRest cannot tell identities apart in opaque tokens. Cost if wrong: a code-first game that rotates its own tokens without `bSameIdentity` restarts its sockets (WS D36) on each rotation.
6. **Session state is live, not part of the `Send()` snapshot** (M3 decision 27 covers configuration): an attempt carries the token current when it is dispatched, so a refreshed token reaches a replay and a request queued in the limiter. The auth object itself (`WithAuth`) is configuration and is snapshotted.
7. **Interrupt hook.** A stage that holds an exchange while waiting (a backoff timer, a park behind a refresh) stores `FExchangeState::Interrupt`. `FClientState::Cancel` — used by `Cancel()`, `CancelAll`, owner-gone and shutdown — calls it instead of finishing the request itself, so the waiting stage answers `Canceled` through the chain and every `Outer` stage still gets its one outcome (`IVaRestStage::Process`'s contract). The stage holds itself weakly from the hook (no cycle through the exchange).
8. **Bodies are copied for a re-issue only when one can happen** (M3 decision 12, per exchange): `bMayReissue` = a custom stage that may re-issue, or retry-eligible, or an auth that can refresh. A non-idempotent POST on a client with retry and plain bearer still moves its body into the engine.
9. **The envelope applies to `FVaRestJson`, struct and `void` results** (`void`: the success predicate only), not to raw results (`FString`, `TArray<uint8>`, `FVaRestResponse`) nor to a body in a `SaveTo` file. A missing `DataPath` value is an invalid handle and a success for `FVaRestJson` (spec §2.2: missing is `undefined`), and a `Decode` error naming the path for a struct (a silent default would hide a wrong path). Cost if wrong: a struct endpoint that sometimes omits its data needs `FVaRestJson`.
10. **The retry `Deadline` is an absolute time** (`StartedAt + Deadline`, request start = pipeline entry): no retry starts past it, every attempt's total timeout is cut to the time left, and an attempt dispatched after it (a long limiter wait) fails `Kind=Timeout` (detail `Deadline`, not retryable) without connecting. Spec §4.7: "plus the retry Deadline across attempts". It applies whenever the resolved policy has one, retry-eligible or not.
11. **`Retry-After` replaces the backoff for that step**, capped at `MaxRetryAfter` (60 s, spec §4.1), and only on a status the policy retries. A delay that would end past the deadline means no retry.
12. **Stage-made failures carry a message and a detail the core classifies** (`FVaRestAttemptOutcome::FailureMessage`, public). New details: `AuthRejected`, `NoCredentials`, `SessionCleared` → `Auth`; `HttpDisabled`, `DomainNotAllowed`, `BodyNotSent` → `Config`; `Deadline` → `Timeout`, not retryable; `AuthRefreshUnreachable` keeps the refresh's own failure kind. `ClassifyAttempt` is the one place that maps an outcome to `Kind` and `bRetryable` (the pipeline's error and the retry stage read the same answer).
13. **A body on a verb the transport does not upload is refused, `Kind=Config`** (carry (f), final review M1). `IVaRestTransport::SendsBodyFor(Verb)` (default true) lets the transport say; the `FHttpModule` transport answers POST, PUT, PATCH and DELETE (curl attaches its read function only for those, `CurlHttp.cpp:941-993`; HEAD sets `CURLOPT_NOBODY`, `:975-978`). GET keeps spec §3.2's drop with a Warning. The fake transport mirrors the HTTP one. Refused rather than dropped: `Request("QUERY", ...).Body(...)` without its body is a different request.
14. **Response headers are collected one by one from `OnHeaderReceived`** (carry (f), final review M2): curl folds repeats into one comma-joined value in `GetAllHeaders` (`CurlHttp.cpp:519-530`) but calls the delegate once per header line of the final response (`:509-535`; `bRedirected` skips a followed redirect's headers, `:556-560`). Apple's delegate replays its dictionary (`HttpRequestImpl.cpp:40-60`), already folded by NSURLSession, so there nothing changes.
15. **The engine's `PreCheck` refusals are found before `ProcessRequest`** (carry (f), final review M9): the HTTP transport checks `FHttpModule::IsHttpEnabled()` and `FHttpManager::IsDomainAllowed(Url)` (`HttpRequestCommon.cpp:83-113` refuses on those as `EHttpFailureReason::Other`) and fails the attempt with `HttpDisabled` / `DomainNotAllowed`, `Kind=Config`. The engine's other `Other` stays a retryable `Connection`: curl reports a reset connection or an empty reply that way (only five `CURLcode`s become `ConnectionError`, `CurlHttp.cpp:1318-1330`; the rest fall to `HandleRequestFailed`'s `Other`, `HttpRequestCommon.cpp:291-310`).
16. **`LaunchRedaction(Name, Body)` replaces `GetRedactionPipe()`** (carry (a), review M-10): callers never hold the pipe, so a decode task (IN-14) may launch from any thread. An atomic pipe pointer, an in-use counter and a stop flag; `StopRedactionPipe` raises the flag, waits for launches in progress, empties and deletes the pipe. `FPipe::Launch` itself is thread-safe (`Pipe.h:63-77`, atomics only).
17. **Logs, traces and `FVaRestError::Url` use the client's current redactor** (carry (b), task 14 review M10): a token the auth refreshes while a request is in flight is scrubbed from that request's later lines. The current redactor knows the old token too (the secret set keeps rotated values 10 minutes, Inspector D21). `ClearSession` forgets them (Inspector §12 "cleared on logout").
18. **`FRequestSpec`'s fields are private** (bead VaRest-uq3): friends `TVaRestRequest`, `FVaRestClient` and `FRequestSpecAccess` (defined in a Private header), so no module can hand-build a spec; `SendRequest` stays exported (the inline builder calls it) and forwards to `FRequestSpecAccess::Send`. Chosen over "move every builder check into `Resolve`" because some checks cannot move (a struct body's encode error exists only at `Body()`), and the project prefers enforcement by shape.
19. **Stages still cannot replace a body in 2.0** (carry (g)). M4's built-in stages edit the exchange through `Internals()`; signing stages read `GetBody()` and the new `GetAttemptUrl()`. Compression or encryption would need the length and `Content-Type` reset in one place; no M4 feature needs it.
20. **Request excerpts are copied at `Started`** (after `Resolve`, before the transport stage can move the bytes; IN-14 says "at Send", and for a request sent from a worker `Started` is its first game-thread moment). A 2xx response a decoder will parse is redacted on the decode task from the body itself; every other response body is copied at the game-thread hop and redacted on the pipe. Binary bodies stay descriptors: hex capture (`BinaryCap > 0`, Inspector §2.2 layer 4) is not in M4a (the M3 redactor has no hex mode).
21. **Stats are VaRest's own atomic counters, mirrored to `stat VaRest`.** Engine stats are compiled out in Shipping and cannot be read back by a test; the counters can.
22. **Auth adds the last segment of each token path to the client's redaction keys** (carry (d), M3 review M19): `data.auth.refresh_token` adds `refresh_token`; normalised keys match whole keys (M3 decision 18), so a whole path never would.
23. **`FVaRestAuthRefreshContext::Request<T>()` is how a refresh request is sent**: it bypasses `AuthGate`/`AuthApply`, the envelope and retry, goes `Highest` in the limiter and carries trace flag `AuthRefresh`. A refresh request sent through the client itself would park behind its own refresh. The refresh response is read raw: token paths are against the whole body (`data.accessToken` for a wrapped API).
24. **Replays re-create stream bodies** (carry (e)): every attempt — retry or session replay — is a new engine request with a new `FVaRestSegmentStream`; nothing asks NSURLSession to resend a stream in place, which it cannot (`AppleHttp.cpp:627-634`, `:751-753`, no `needNewBodyStream`).
25. **Trace payloads `Note`, `Excerpt` and `Marker` join `FVaRestTracePayload` in M4**; the socket payloads are W5's. WebSocket §19 item 4 is met by `FVaRestTraceEvent::Source` (M3) plus the request and attempt ids every HTTP payload carries; sinks switch on `Kind` and ignore kinds they do not know (documented on `IVaRestTraceSink::OnEvent`).
26. **Retry jitter draws from one `FRandomStream` per client**, seeded from the client id and `FPlatformTime::Cycles()`; `WithRandomSeed(int32)` makes a test exact. The formula takes the stream as a parameter, so a socket passes its own (WebSocket §3.4).
27. **Names that freeze with M5** (they become asset properties and pins): `EVaRestJitter`, `FVaRestBackoff`, `FVaRestRetryPolicy`, `FVaRestEnvelope`, `EVaRestAuthMode`, `EVaRestApiKeyPlacement`, `EVaRestRefreshStyle`, `EVaRestSessionChange`, `FVaRestBearerRefreshConfig`, and their `UPROPERTY` field names. Task 20 adds them to CLAUDE.md's freeze line.

## Global Constraints

- Engine: UE 5.8 only. Build host: `/w/Unreal/VaRestHost` (plugin symlinked). Build + test: `Tools/run_tests.sh [Filter]` (bare filters are anchored, so `VaRest.Http` never runs `Bench.VaRest.*`); the runner starts `Tools/testserver/server.py` and passes `-VaRestTestServer=<url>`.
- `VaRestHttp` dependencies stay: public `Core`, `CoreUObject`, `VaRestJson`, `DeveloperSettings`; private `HTTP`. No new third-party code.
- **v1 paths are frozen.** Nothing under `Source/VaRest/` or `Source/VaRestEditor/` changes.
- **Frozen names** (CLAUDE.md): `UVaRestHttpSettings` property names (not touched in M4a); `FVaRestError`'s fields, `EVaRestErrorKind`, `EVaRestPriority`, `EVaRestLogLevel`, `EVaRestQueryStyle` freeze with M5 — M4a adds no field and no enumerator to them.
- **Threading rule (spec §1, verbatim):** "*Control plane on the game thread, data plane off it.* Stage decisions, callbacks, and BP events run on the game thread. Parsing, decoding, cache disk IO, and offline-queue persistence run on `UE::Tasks` background tasks. Transport completes on the HTTP thread (`EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread`); the completion only moves bytes and dispatches work. A request may be **sent** from any thread; it is marshalled to the game thread before entering the pipeline." Inspector §7: "The game thread never parses or redacts a body." Session state, auth calls and refresh coordination are game thread.
- Trace points: every one starts with `if (FVaRestTrace::Detail() == EVaRestTraceDetail::None) { return; }` before building anything (IN-11). Sinks receive only redacted data (Inspector §2.2). Every kind but `Excerpt` is emitted on the game thread; nothing from the HTTP thread.
- Public types: `FVaRest*`, `EVaRest*`, `IVaRest*`, `TVaRest*`, `UVaRest*`. Internals: namespace `VaRest::Http`; file-local helpers in `namespace VaRest::Http::<FileStem>Private`. Never reuse a v1 header name (`VaRest.h`, `VaRestDefines.h`, `VaRestJsonObject.h`, `VaRestJsonValue.h`, `VaRestLibrary.h`, `VaRestRequestJSON.h`, `VaRestSettings.h`, `VaRestSubsystem.h`, `VaRestTypes.h`, `VaRestJsonParser.h`).
- JSON keys are case-sensitive (`ESearchCase::CaseSensitive`, `TestEqualSensitive`); HTTP header names are not (`ESearchCase::IgnoreCase`, explicitly).
- Unity builds: no file-scope `using namespace`, no anonymous namespaces in `.cpp` files; block-scope `using namespace` inside a function body is fine.
- MSVC-clean by construction: no local or parameter named like a member (data or function) of the enclosing class (C4458). `TVaRestRequest` gains the methods `Retry`, `NoRetry`, `Envelope`, `NoEnvelope`, `AuthMode`, `NoAuth`, `MarkAuthRefresh`: no parameter or local in that class may use those names (write `InPolicy`, `InEnvelope`, `InMode`). `FVaRestClient` gains `WithRetry`, `WithRandomSeed`, `WithEnvelope`, `WithoutEnvelope`, `WithAuth`, `WithoutAuth`, `WithBearer`, `SetBearerToken`, `SetSession`, `SetSessionHeader`, `RemoveSessionHeader`, `ClearSession`, `GetSession`, `ExportSession`, `ImportSession`, `NeedsAuthRefresh`, `RefreshAuth`, `ApplyAuth`, `NotifySessionExpired`, `OnSessionChanged`, `OnSessionExpired`. Lambda init-captures never reuse an enclosing name, and a local inside a lambda body never takes the name of a local of the function around it (C4456; Task 20's sweep).
- `UE_LOG` format arguments are string literals. Every core log line about a request includes `#<RequestId>` (IN-16). Values in log lines are redacted, then escaped (`EscapeForLog`).
- Tests: `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, inside `#if WITH_DEV_AUTOMATION_TESTS`, flags `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`; unit tests in `Source/VaRestHttp/Private/Tests/`, names `VaRest.Http.*`; integration tests in `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`, names `VaRest.Integration.*`.
- **Test hygiene (each cost an M3 fix round):** every test drains what it sends (pump until done, or cancel and pump until done); a test asserting "nothing happened" first asserts the request finished, and drains with `FTestRig::Drain()` or counters, never with a fixed short wait; never dereference, `GetValue()` or index after a non-fatal check — write `if (!TestTrue(...)) { return false; }` first; callbacks never capture stack locals by reference (capture `TSharedRef` state, `MakeOutcome<T>()`); expected-log patterns are unique per line (the engine counts only the first matching pattern, `AutomationTest.cpp:2692-2714`), with `AddExpectedErrorPlain` / `AddExpectedMessagePlain` and exact counts; a restore-the-bug step breaks the named behaviour, shows the named test failing, restores it, and the task report gives both outcomes; a test must be able to fail (no assertion the code cannot violate).
- `FTestRig` clients log at `None` and have no retry policy and no auth; a test sets what it needs.
- Engine claims carry `file:line` citations from `/w/Unreal/UnrealEngine/Engine/Source`; open every line you cite.
- Never run a `-DisableUnity` or `-CompilerArguments` UBT build (they rebuild the engine). The per-module no-PCH / non-unity check is `PCHUsage = PCHUsageMode.NoPCHs; bUseUnity = false;` set temporarily in `VaRestHttp.Build.cs`, dry-run first with `-WriteOutdatedActions=<file>`, reverted after.
- No editor may run during a build; kill only by PID (never `pkill -f`).
- New file header: `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.`
- Every commit message ends with `Co-Authored-By: Claude <Model> <noreply@anthropic.com>` naming the model that wrote the commit (for example `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`). Stage only the files your task names (`git add <paths>`): other agents work in this tree. Never push.

## Review Focus

- **A POST without `.Idempotent()` on a client with retry, against a 503 or a dropped connection**: sent exactly once; a reasonable person expects no duplicate order. Test owner: Task 7 (`VaRest.Http.Retry.NonIdempotentSentOnce`, restore-the-bug: drop the verb check).
- **Twenty requests hitting 401 together while the refresh endpoint is slow, and then the refresh failing on the network**: one refresh request; each parked request replays once or fails once; the network failure does not log the player out. Test owner: Task 13 (`VaRest.Http.Auth.SingleFlight`, `VaRest.Http.Auth.RefreshUnreachableKeepsSession`).
- **`Cancel()`, the owner's destruction or engine shutdown while a request backs off on a timer or waits behind a refresh**: exactly one `Canceled` (or silence for owner/shutdown), no further attempt, the timer cleared, the request out of `GetInFlightCount()`. Test owners: Task 8 (`VaRest.Http.Retry.StopWhileBackingOff`), Task 13 (`VaRest.Http.Auth.StopWhileParked`).
- **A token refreshed while a request is in flight, echoed back by the server in a header, a body and the URL**: no log line or trace event carries the new or the old token. Test owner: Task 14 (`VaRest.Http.Auth.RefreshedTokenRedacted`).
- **An envelope whose paths miss — no `data`, no `ok`, a body that is an array, an `ok` of `0` or `"false"`**: `Kind=Server` only when the predicate fails (values compare as text, so `true` and `"true"` both pass, documented on `FVaRestEnvelope`), a struct result names the missing path, nothing crashes. Test owner: Task 9 (`VaRest.Http.Envelope.Evaluate`), Task 10 (`VaRest.Http.Envelope.MissingPaths`).

---

## File structure

```
Source/VaRestHttp/
  Public/VaRestClient.h            FRequestSpec private (T1); retry/envelope/auth builder methods (T7, T10, T12); client
                                   retry, envelope, auth and session API (T7, T10, T11, T12, T13); GetAttemptUrl (T12)
  Public/VaRestStage.h             FVaRestAttemptOutcome::FailureMessage (T2); FVaRestExchange::GetAttemptUrl (T12)
  Public/VaRestTransport.h         IVaRestTransport::SendsBodyFor (T2)
  Public/VaRestFakeTransport.h     SendsBodyFor mirror and override (T2)
  Public/VaRestTrace.h             Note / Excerpt / Marker payloads, MaxPendingExcerptBytes, VAREST_WITH_TRACE_EXCERPTS (T4)
  Public/VaRestHttpTesting.h       FTraceRecorder, FScopedTraceRecorder (T4)
  Public/VaRestResultHandler.h     FFinish envelope + excerpt fields, IResultHandler::AppliesEnvelope, CheckEnvelopePredicate (T10, T17)
  Public/VaRestRedactor.h          FVaRestSecretSet::ClearSlots; previous values remember their slot (T11)
  Public/VaRestBackoff.h           NEW (T5): EVaRestJitter, FVaRestBackoff, ParseRetryAfter (WS §19 item 1)
  Public/VaRestRetry.h             NEW (T6): FVaRestRetryPolicy, IsIdempotentVerb
  Public/VaRestEnvelope.h          NEW (T9): FVaRestEnvelope, FVaRestEnvelopeResult (WS §19 item 6)
  Public/VaRestAuthTypes.h         NEW (T11): EVaRestAuthMode, EVaRestApiKeyPlacement, EVaRestRefreshStyle,
                                   EVaRestSessionChange, FVaRestBearerRefreshConfig, FVaRestSessionData
  Public/VaRestAuth.h              NEW (T12): IVaRestAuth, FVaRestAuthTarget, FVaRestAuthRefreshResult,
                                   FVaRestAuthRefreshContext (T13), FVaRestAuth factories (T12, T14) (WS §19 item 2)
  Public/VaRestHttpStats.h         NEW (T16): LLM tags VaRest, VaRest_Trace
  Private/VaRestClientState.h/.cpp FClientConfig fields; Publish's chain; session, random, redactor accessors (T3-T13)
  Private/VaRestExchangeState.h    retry, auth, envelope, excerpt state on the exchange (T7-T16)
  Private/VaRestRequestSpec.h      NEW (T1): FRequestSpecAccess
  Private/VaRestPipeline.cpp       Cancel's interrupt, ClassifyAttempt use, deadline clamp, AttemptUrl (T2, T7, T8, T12)
  Private/VaRestClassify.h/.cpp    NEW (T2): ClassifyAttempt, KindFor moved here
  Private/VaRestHttpTransport.cpp  PreCheck mirror, per-header collection, SendsBodyFor (T2)
  Private/VaRestHttpTransportInternal.h  FHttpTransportCompletion collects headers (T2)
  Private/VaRestRedactionPipe.h, VaRestRedactor.cpp   LaunchRedaction (T3)
  Private/VaRestLogStage.cpp       EscapeControlsForLog (T1), current redactor, AttemptUrl, retry line (T3, T7, T12)
  Private/VaRestTracePoints.h/.cpp current redactor (T3), Note/Marker, gap fields, envelope, flags (T4, T7, T10, T13)
  Private/VaRestBackoff.cpp        NEW (T5)
  Private/VaRestRetry.cpp          NEW (T6): policy, DecideRetry; (T7) the retry stage
  Private/VaRestRetryInternal.h    NEW (T6): FRetryDecision, DecideRetry, MakeRetryStage
  Private/VaRestEnvelope.cpp       NEW (T9): the evaluator
  Private/VaRestSession.h/.cpp     NEW (T11): FSessionState and the session functions of FClientState; (T13) the single-flight refresh
  Private/VaRestAuthInternal.h     NEW (T12): ApplyCredentials, MakeAuthApplyStage; (T13) MakeAuthGateStage
  Private/VaRestAuth.cpp           NEW (T12): Bearer, ApiKey, Basic, the AuthApply stage, the refresh result and context
  Private/VaRestAuthGate.cpp       NEW (T13): the AuthGate stage
  Private/VaRestBearerRefresh.cpp  NEW (T14)
  Private/VaRestHttpStatsInternal.h, VaRestHttpStats.cpp  NEW (T16): FHttpStats, stat VaRest, the LLM tag definitions
  Private/VaRestTraceExcerpts.h/.cpp NEW (T17): IN-14
  Private/VaRestDecode.h/.cpp      ApplyEnvelope; envelope in DecodeJson, DecodeStruct, ProcessErrorBody; CheckEnvelopePredicate (T10)
  Private/Tests/VaRestHttpFidelityTest.cpp   NEW (T2)
  Private/Tests/VaRestHttpTraceM4Test.cpp    NEW (T4)
  Private/Tests/VaRestHttpBackoffTest.cpp    NEW (T5)
  Private/Tests/VaRestHttpRetryTest.cpp      NEW (T6-T8)
  Private/Tests/VaRestHttpEnvelopeTest.cpp   NEW (T9-T10)
  Private/Tests/VaRestHttpSessionTest.cpp    NEW (T11)
  Private/Tests/VaRestHttpAuthTest.cpp       NEW (T12-T15)
  Private/Tests/VaRestHttpStatsTest.cpp      NEW (T16)
  Private/Tests/VaRestHttpExcerptTest.cpp    NEW (T17)
  Private/Tests/VaRestHttpTraceGoldenTest.cpp NEW (T18)
  Private/Tests/VaRestHttpRedactorTest.cpp, VaRestHttpLogTest.cpp   LaunchFromAnyThread, CurrentRedactor (T3); ConfigErrorEscapedOnce (T1)
  Private/Tests/*.cpp (M3)          T1's residual fixes; T3's redaction-pipe callers
Source/VaRestTests/Private/VaRestHttpSurfaceShapeTest.cpp   FRequestSpec shape (T1)
Source/VaRestTests/Private/VaRestIntegrationTest.cpp        Apple gate (T1); RepeatedHeaders (T2); Retry, AuthRefresh, Envelope (T19)
Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp   Envelope.Struct (T10); Trace.Golden.DecodeWarnings (T18)
Tools/testserver/server.py, selftest.py                     POST /expire, GET /envelope/gone (T19)
docs/dev/http-thread-completion.md                          Apple progress note (T1); M4 checks (T20)
CLAUDE.md                                                   freeze line (T20)
```

---
### Task 1: M3 residuals (VaRest-3dj) and the closed request spec (VaRest-uq3)

Six minors from the M3 final re-review, folded here by the M3 ruling, and the `FRequestSpec` back door (plan decision 18). No behaviour of a working program changes: tests stop indexing after a failed check, one test learns to fail, one doc line, one test skips where its premise does not hold, one doc paragraph, one log line escapes once. The spec becomes unreachable from outside VaRestHttp.

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`FRequestSpec` private; `Request()` doc)
- Create: `Source/VaRestHttp/Private/VaRestRequestSpec.h` (`FRequestSpecAccess`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (`SendRequest` → `FRequestSpecAccess::Send`; `DeliverWithoutClient` moves into it)
- Modify: `Source/VaRestHttp/Private/VaRestHttpLog.h`, `Source/VaRestHttp/Private/VaRestLogStage.cpp` (`EscapeControlsForLog`)
- Modify: `Source/VaRestHttp/Private/VaRestLimiter.h:36` (doc)
- Modify: `docs/dev/http-thread-completion.md` (Apple item 4)
- Test: `Source/VaRestTests/Private/VaRestHttpSurfaceShapeTest.cpp`, `Source/VaRestHttp/Private/Tests/VaRestHttpTimeoutProgressTest.cpp`, `Source/VaRestHttp/Private/Tests/VaRestHttpBuilderTest.cpp`, `Source/VaRestHttp/Private/Tests/VaRestHttpLimiterTest.cpp`, `Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp`, `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`

**Interfaces:**
- Consumes: M3 as built (HEAD 2d2ec8d).
- Produces: `VaRest::Http::FRequestSpec` (a class; public destructor only); `struct VaRest::Http::FRequestSpecAccess { static FVaRestRequestHandle Send(TUniquePtr<FRequestSpec>&&, const char*, uint32); static void DeliverWithoutClient(FRequestSpec&); }` in `Private/VaRestRequestSpec.h` — later tasks add fields to `FRequestSpec` and copy them in `FRequestSpecAccess::Send`; `FString VaRest::Http::EscapeControlsForLog(FStringView)`.

- [ ] **Step 1: Write the failing shape test** — append to the `#if defined(__clang__)` block of `Source/VaRestTests/Private/VaRestHttpSurfaceShapeTest.cpp`, and add the two concepts beside the others:

```cpp
	template <typename T>
	concept CReachesSpecFields = requires(T& Spec) { Spec.Verb; };

	template <typename T>
	concept CMakesSpec = requires { new T(TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe>(), FStringView(), FStringView()); };
```

```cpp
	// Bead VaRest-uq3 (plan decision 18): a module cannot hand-build a spec and skip the builder's checks
	static_assert(!CReachesSpecFields<VaRest::Http::FRequestSpec>, "FRequestSpec: its fields are the builder's (bead VaRest-uq3)");
	static_assert(!CMakesSpec<VaRest::Http::FRequestSpec>, "FRequestSpec: only FVaRestClient makes one (bead VaRest-uq3)");
```

- [ ] **Step 2: Build to see it fail**

Run: `/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject`
Expected: FAIL, `static assertion failed ... FRequestSpec: its fields are the builder's (bead VaRest-uq3)` (and the second one).

- [ ] **Step 3: Make the spec private** — in `Source/VaRestHttp/Public/VaRestClient.h` replace the block from `class FVaRestClient;` down to the `SendRequest` declaration (lines 18-66) with:

```cpp
class FVaRestClient;
class FVaRestRequestHandle;
/** Declared here for FRequestSpec's friendship; the definition below carries [[nodiscard]] (allowed on a later declaration) */
template <typename T>
class TVaRestRequest;

namespace VaRest::Http
{
	struct FClientAccess;
	struct FRequestSpecAccess;

	/**
	 * What a TVaRestRequest collects; Send moves it into an exchange. Private by shape (bead VaRest-uq3, plan decision 18):
	 * only the builder, FVaRestClient (which makes one) and VaRestHttp's own code (FRequestSpecAccess, defined in a Private
	 * header) reach its fields, so no module can hand-build a spec that skips the builder's checks. Resolve's checks run
	 * for every request anyway, and the transport stage checks the final headers again (M3 review I3).
	 */
	class VARESTHTTP_API FRequestSpec
	{
	public:
		~FRequestSpec();
		FRequestSpec(const FRequestSpec&) = delete;
		FRequestSpec& operator=(const FRequestSpec&) = delete;

	private:
		template <typename>
		friend class ::TVaRestRequest;
		friend class ::FVaRestClient;
		friend struct FRequestSpecAccess;

		FRequestSpec(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState, FStringView InVerb, FStringView InPath);

		/** Records the first misuse met while building; Send then fails with Kind=Config (decisions 10, 11) */
		void Fail(FString Problem);

		TSharedPtr<FClientState, ESPMode::ThreadSafe> State;
		/** The client's configuration when the builder was made: struct bodies encode with its naming policy in Body() */
		TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> ConfigAtBuild;
		FString Verb;
		FString Path;
		TArray<TPair<FString, FString>> PathParams;
		TArray<TPair<FString, FString>> Query;
		FVaRestHeaders Headers;
		FVaRestRequestBody Body;
		FString BuildError;
		FName Tag;
		EVaRestPriority Priority = EVaRestPriority::Normal;
		TWeakObjectPtr<const UObject> Owner;
		bool bHasOwner = false;
		bool bIdempotent = false;
		bool bKeepBody = false;
		bool bOnHttpThread = false;
		FString SaveTo;
		TOptional<float> Timeout;
		TOptional<float> ActivityTimeout;
		TOptional<int64> MaxJsonBytes;
		EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
		bool bStrictDecode = false;
		/** The current body is a struct, encoded with Naming: Naming() now fails. A later Body() or BodyFile() clears it. */
		bool bStructBodySet = false;
		TUniquePtr<IResultHandler> Handler;
		bool bSent = false;
	};

	/** The inline builder's Send() calls this from the caller's module, so it is exported; it forwards to FRequestSpecAccess::Send */
	VARESTHTTP_API FVaRestRequestHandle SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
}
```

In the same header, `FVaRestClient::Request` now constructs the spec itself (a friend may call the private constructor; `MakeUnique` may not):

```cpp
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const
	{
		TUniquePtr<VaRest::Http::FRequestSpec> NewSpec(new VaRest::Http::FRequestSpec(State, Verb, Path));
		NewSpec->Handler = MakeUnique<VaRest::Http::TResultHandler<T>>();
		return TVaRestRequest<T>(MoveTemp(NewSpec));
	}
```

and its doc comment's first paragraph gains bead VaRest-3dj (3) at the end:

```cpp
	 * Any verb, an RFC 9110 token taken as it is (no trimming): anything else, an empty verb included, fails the request
	 * with Kind=Config and nothing is sent (final review I3). T defaults to FVaRestJson (spec 3.2). An empty client gives
	 * a builder whose Send() logs an error, sends nothing and delivers Kind=Config to OnError and OnComplete from a later
	 * game-thread tick (final review M7; decision 3). The handle that Send() returns is empty: IsActive() is false and
	 * Cancel() does nothing, also while that delivery is still queued (bead VaRest-3dj).
```

- [ ] **Step 4: The access struct** — create `Source/VaRestHttp/Private/VaRestRequestSpec.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClient.h"

namespace VaRest::Http
{
	/**
	 * The one way VaRestHttp's own code reaches FRequestSpec's fields (bead VaRest-uq3). Declared a friend in the public
	 * header and defined only here, in a Private header, so a module outside VaRestHttp cannot use it.
	 */
	struct FRequestSpecAccess
	{
		/** SendRequest's body: the spec into an exchange, the exchange into its client's pipeline (spec 1, 3.1) */
		static FVaRestRequestHandle Send(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
		/** Final review M7: Kind=Config to the callbacks of a builder made on an empty FVaRestClient */
		static void DeliverWithoutClient(FRequestSpec& Spec);
	};
}
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`: add `#include "VaRestRequestSpec.h"`; move `ClientPrivate::DeliverWithoutClient` (lines 26-72) out of `ClientPrivate` and define it as `void VaRest::Http::FRequestSpecAccess::DeliverWithoutClient(FRequestSpec& Spec)` with the same body; rename the definition `FVaRestRequestHandle VaRest::Http::SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)` (line 144) to `FVaRestRequestHandle VaRest::Http::FRequestSpecAccess::Send(...)`, keep its body, and change its `ClientPrivate::DeliverWithoutClient(*Spec);` to `DeliverWithoutClient(*Spec);`. Then add the exported forwarder after it:

```cpp
FVaRestRequestHandle VaRest::Http::SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)
{
	return FRequestSpecAccess::Send(MoveTemp(Spec), SourceFile, SourceLine);
}
```

The constructor, destructor and `Fail` definitions (lines 108-142) are members and compile unchanged.

- [ ] **Step 5: Build and run the builder and pipeline suites**

Run: `Tools/run_tests.sh VaRest.Http.Builder` then `Tools/run_tests.sh --no-build VaRest.Http.Pipeline`
Expected: the build passes (both static_asserts hold); `0 failed` both times.

- [ ] **Step 6: Restore-the-bug for the shape** — move `FString Verb;` above `private:` in `FRequestSpec`, build: the first static_assert fails. Put it back, build: passes. Record both in the task report.

- [ ] **Step 7: 3dj (1) — no index after a failed check** — in `Source/VaRestHttp/Private/Tests/VaRestHttpTimeoutProgressTest.cpp`, `VaRest.Http.Timeout.Applied`, replace the line `TestTrue(TEXT("all reach the transport"), PumpUntil(...));` (line 52) with:

```cpp
	if (!TestTrue(TEXT("all reach the transport"), PumpUntil([&] { return Plain.Fake->NumCalls() == 1 && Rig.Fake->NumCalls() == 5 && Clamped.Fake->NumCalls() == 1; })))
	{
		// Bead VaRest-3dj (1): the checks below index calls; one that never arrived must not be read
		PumpUntil([&] { return Plain.Client.GetInFlightCount() == 0 && Rig.Client.GetInFlightCount() == 0 && Clamped.Client.GetInFlightCount() == 0; });
		return false;
	}
```

- [ ] **Step 8: 3dj (2) — the owner-gone block can fail** — in `Source/VaRestHttp/Private/Tests/VaRestHttpBuilderTest.cpp`, `VaRest.Http.Builder.EmptyClient`, after `PumpUntil([] { return true; });` in the owner-gone block add:

```cpp
	// Bead VaRest-3dj (2): the delivery task ran and dropped the callbacks, and with them their three captures of Silent.
	// Without this, the check below would pass with the task still queued.
	TestEqual(TEXT("owner gone: the delivery task ran and released the callbacks"), Silent.GetSharedReferenceCount(), 1);
```

Restore-the-bug: delete the `PumpUntil([] { return true; });` line; run `Tools/run_tests.sh VaRest.Http.Builder.EmptyClient`: FAIL (the count is 4). Restore; PASS.

- [ ] **Step 9: 3dj (4) — the ensure test states its premise** — in `Source/VaRestHttp/Private/Tests/VaRestHttpLimiterTest.cpp` add `#include "HAL/IConsoleManager.h"`, `#include "Misc/CommandLine.h"`, `#include "Misc/ConfigCacheIni.h"`, `#include "Misc/Parse.h"`, and at the start of the `#if DO_ENSURE` branch of `VaRest.Http.Limiter.DoubleReleaseEnsuresEachTime`:

```cpp
	// Bead VaRest-3dj (4): ensureAlways reports every time only while core.EnsureAlwaysEnabled is on (AssertionMacros.cpp:136-140,
	// :948) and every ensure is handled: [Core.System] HandleEnsurePercent, else -handleensurepercent=, at 100
	// (GenericPlatformMisc.cpp:2058-2068, :2070-2094). Under other settings the count is chance: say so instead of failing.
	{
		IConsoleVariable* Always = IConsoleManager::Get().FindConsoleVariable(TEXT("core.EnsureAlwaysEnabled"));
		float Percent = 100.f;
		if (!GConfig || !GConfig->GetFloat(TEXT("Core.System"), TEXT("HandleEnsurePercent"), Percent, GEngineIni))
		{
			FParse::Value(FCommandLine::Get(), TEXT("handleensurepercent="), Percent);
		}
		if (!Always || !Always->GetBool() || Percent < 100.f)
		{
			AddInfo(TEXT("core.EnsureAlwaysEnabled is off or HandleEnsurePercent is below 100: ensureAlways does not report every failure here; nothing to check"));
			return true;
		}
	}
```

In `Source/VaRestHttp/Private/VaRestLimiter.h`, the `Release()` comment becomes:

```cpp
		/**
		 * A granted attempt ended: exactly once per grant. A Release without one is an ensureAlways, every time (review Minor
		 * 1, P12.1) -- every time under the engine's defaults, core.EnsureAlwaysEnabled on and [Core.System]
		 * HandleEnsurePercent 100 (AssertionMacros.cpp:136-140, :948; GenericPlatformMisc.cpp:2058-2094).
		 */
		void Release();
```

- [ ] **Step 10: 3dj (5) — Apple's last progress call** — in `docs/dev/http-thread-completion.md`, Mac/iOS item 4, append after "Check `VaRest.Integration.Http.UploadProgress` and the trace's upload phase.":

```markdown
   That test's last check, that the final `OnProgress` shows `BytesSent == UploadTotal`, is a curl property: curl's finish
   reports progress once more (`CurlHttp.cpp:1274`), Apple's `FinishRequest` does not (`AppleHttp.cpp:777`), so on Apple the
   last call may show less, and the test skips that check there (bead VaRest-3dj).
```

In `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`, `VaRest.Integration.Http.UploadProgress`, replace the line `TestTrue(TEXT("... ending at the whole body"), ...);` with:

```cpp
#if PLATFORM_APPLE
	// Bead VaRest-3dj (5): curl's finish reports progress once more (CurlHttp.cpp:1274); Apple's FinishRequest does not (AppleHttp.cpp:777)
	AddInfo(TEXT("Apple: the last OnProgress may show less than the whole body (no final progress call); not checked"));
#else
	TestTrue(TEXT("... ending at the whole body"), Progress->Seen.Num() > 0 && Progress->Seen.Last().BytesSent == Size && Progress->Seen.Last().UploadTotal == Size);
#endif
```

- [ ] **Step 11: 3dj (6) — write the failing log test** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp` (before the closing `#endif`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogConfigEscapedOnceTest, "VaRest.Http.Log.ConfigErrorEscapedOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogConfigEscapedOnceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Bead VaRest-3dj (6): Resolve escapes the verb into Error.Message (a game may log the message as it is); the Basic line
	// escaped it again, so "\r" showed as "\\r". A message with a raw CR LF (a missing file's name) must still be escaped.
	FLevelIsolation Isolation;
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> Config error: the verb"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("-> Config error: upload file not found"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	auto Verb = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Request(TEXT("GET\r\nX"), TEXT("/v")), Verb).Send();
	Observe(Rig.Client.Put(TEXT("/f")).BodyFile(TEXT("/no/such/dir\r\nfile.bin")), File).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Verb, File] { return Verb->IsDone() && File->IsDone(); })))
	{
		return false;
	}
	FString VerbLine;
	FString FileLine;
	for (const FString& Line : Capture.GetLines())
	{
		if (Line.Contains(TEXT("Config error: the verb")))
		{
			VerbLine = Line;
		}
		else if (Line.Contains(TEXT("Config error: upload file not found")))
		{
			FileLine = Line;
		}
	}
	TestTrue(TEXT("the verb is escaped once: one backslash before r"), VerbLine.Contains(TEXT("\"GET\\r\\nX\"")) && !VerbLine.Contains(TEXT("\\\\r")));
	TestTrue(TEXT("a raw CR LF in a Config message is still escaped in its line"), FileLine.Contains(TEXT("dir\\r\\nfile.bin"))
		&& !FileLine.Contains(TEXT("\r")) && !FileLine.Contains(TEXT("\n")));
	TestTrue(TEXT("Error.Message keeps its single escape for games that log it"), Verb->Error.Message.Contains(TEXT("\"GET\\r\\nX\"")));
	return true;
}
```

- [ ] **Step 12: Run it to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Log.ConfigErrorEscapedOnce`
Expected: FAIL on "the verb is escaped once" (the line holds `GET\\r\\nX`).

- [ ] **Step 13: Escape once** — in `Source/VaRestHttp/Private/VaRestHttpLog.h` add below `EscapeForLog`:

```cpp
	/**
	 * EscapeForLog without the backslash rule (bead VaRest-3dj): for text that may already hold EscapeForLog's escapes, such
	 * as a Config error message (Resolve escapes the verb into it, because a game may log FVaRestError::Message as it is).
	 * A control character is still escaped, so the line cannot be broken; a literal backslash shows once.
	 */
	FString EscapeControlsForLog(FStringView Text);
```

In `Source/VaRestHttp/Private/VaRestLogStage.cpp`, give `EscapeForLog`'s loop a flag and add the second entry point — replace the definition of `VaRest::Http::EscapeForLog` (lines 260-304) with:

```cpp
namespace VaRest::Http::LogStagePrivate
{
	FString Escape(FStringView Text, bool bBackslash)
	{
		const auto Needs = [bBackslash](TCHAR Char) { return NeedsEscape(Char) && (bBackslash || Char != TEXT('\\')); };
		int32 First = 0;
		while (First < Text.Len() && !Needs(Text[First]))
		{
			++First;
		}
		if (First == Text.Len())
		{
			return FString(Text);
		}
		FString Out;
		Out.Reserve(Text.Len() + 16);
		Out.Append(Text.GetData(), First);
		for (int32 i = First; i < Text.Len(); ++i)
		{
			const TCHAR Char = Text[i];
			if (!Needs(Char))
			{
				Out.AppendChar(Char);
			}
			else if (Char == TEXT('\\'))
			{
				Out += TEXT("\\\\");
			}
			else if (Char == TEXT('\r'))
			{
				Out += TEXT("\\r");
			}
			else if (Char == TEXT('\n'))
			{
				Out += TEXT("\\n");
			}
			else if (static_cast<uint32>(Char) <= 0xFF)
			{
				Out.Appendf(TEXT("\\x%02X"), static_cast<uint32>(Char));
			}
			else
			{
				Out.Appendf(TEXT("\\u%04X"), static_cast<uint32>(Char));
			}
		}
		return Out;
	}
}

FString VaRest::Http::EscapeForLog(FStringView Text)
{
	return LogStagePrivate::Escape(Text, /*bBackslash*/ true);
}

FString VaRest::Http::EscapeControlsForLog(FStringView Text)
{
	return LogStagePrivate::Escape(Text, /*bBackslash*/ false);
}
```

and in `LogConfigError` change `*EscapeForLog(Message)` to `*EscapeControlsForLog(Message)`.

- [ ] **Step 14: Run the log suite and the builder suite**

Run: `Tools/run_tests.sh VaRest.Http.Log` then `Tools/run_tests.sh --no-build VaRest.Http.Builder`
Expected: `0 failed`. Restore-the-bug: put `EscapeForLog(Message)` back in `LogConfigError`; `VaRest.Http.Log.ConfigErrorEscapedOnce` FAILS; restore; PASS.

- [ ] **Step 15: Full suite, then commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestRequestSpec.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/VaRestHttpLog.h Source/VaRestHttp/Private/VaRestLogStage.cpp Source/VaRestHttp/Private/VaRestLimiter.h \
  Source/VaRestHttp/Private/Tests/VaRestHttpTimeoutProgressTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpBuilderTest.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpLimiterTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp \
  Source/VaRestTests/Private/VaRestHttpSurfaceShapeTest.cpp Source/VaRestTests/Private/VaRestIntegrationTest.cpp docs/dev/http-thread-completion.md
git commit -m "http: M4 task 1 -- M3 residuals (VaRest-3dj) and a request spec only the builder reaches (VaRest-uq3)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

Close the beads after the commit: `bd close VaRest-3dj VaRest-uq3 --reason "M4a task 1"`.

---

### Task 2: Engine-transport fidelity — bodies the transport would not send, repeated response headers, PreCheck refusals, one classification (carry (f))

Three things the engine does that M3 documented but did not act on (final review M1, M2, M9), plus the one function that turns an outcome into `Kind` and `bRetryable`, which M4's retry reads (plan decisions 12-15).

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestClassify.h`, `Source/VaRestHttp/Private/VaRestClassify.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestStage.h` (`FVaRestAttemptOutcome::FailureMessage`)
- Modify: `Source/VaRestHttp/Public/VaRestTransport.h` (`IVaRestTransport::SendsBodyFor`, `VaRest::Http::IsBodyVerb`)
- Modify: `Source/VaRestHttp/Public/VaRestFakeTransport.h`, `Source/VaRestHttp/Private/VaRestFakeTransport.cpp` (mirror)
- Modify: `Source/VaRestHttp/Private/VaRestHttpTransportInternal.h`, `Source/VaRestHttp/Private/VaRestHttpTransport.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`KindFor` moves out; `OnChainDone` uses `ClassifyAttempt`; `Resolve` asks the transport)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`Request()` doc), `Source/VaRestHttp/Public/VaRestHttpTypes.h` (`FVaRestHeaders` doc)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpFidelityTest.cpp`, `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`

**Interfaces:**
- Consumes: Task 1.
- Produces: `FString FVaRestAttemptOutcome::FailureMessage` (a stage's human-readable reason; the error's `Message` when set); `virtual bool IVaRestTransport::SendsBodyFor(FStringView Verb) const` (default true); `VARESTHTTP_API bool VaRest::Http::IsBodyVerb(FStringView Verb)` (POST, PUT, PATCH, DELETE, any case); `bool FVaRestFakeTransport::bSendsBodyForAnyVerb = false`; private `struct VaRest::Http::FAttemptClass { EVaRestErrorKind Kind; bool bRetryable; bool IsError() const; }`, `EVaRestErrorKind VaRest::Http::KindFor(EVaRestTransportFailure, FName)`, `FAttemptClass VaRest::Http::ClassifyAttempt(const FVaRestAttemptOutcome&)`; `void FHttpTransportCompletion::AddHeader(const FString&, const FString&)`, `FVaRestHeaders FHttpTransportCompletion::TakeHeaders()`. Failure details recognised from now on (plan decision 12): `AuthRejected`, `NoCredentials`, `SessionCleared` (→ `Auth`); `HttpDisabled`, `DomainNotAllowed`, `BodyNotSent` (→ `Config`); `Deadline` (→ `Timeout`, not retryable).

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestHttp/Private/Tests/VaRestHttpFidelityTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HttpModule.h"
#include "Misc/AutomationTest.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/ScopeExit.h"
#include "VaRestClassify.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpTransportInternal.h"

namespace VaRest::Http::FidelityTestPrivate
{
	FVaRestAttemptOutcome Failed(EVaRestTransportFailure Failure, const TCHAR* Detail)
	{
		FVaRestAttemptOutcome Out;
		Out.Failure = Failure;
		Out.FailureDetail = Detail;
		return Out;
	}

	FVaRestAttemptOutcome Status(int32 Code)
	{
		FVaRestAttemptOutcome Out;
		Out.Status = Code;
		return Out;
	}

	FString Describe(const FAttemptClass& Class)
	{
		return FString::Printf(TEXT("%s%s"), LexToString(Class.Kind), Class.bRetryable ? TEXT("+retry") : TEXT(""));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityClassifyTest, "VaRest.Http.Fidelity.Classify",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityClassifyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::FidelityTestPrivate;
	struct FCase
	{
		FVaRestAttemptOutcome Outcome;
		const TCHAR* Expected;
	};
	FVaRestAttemptOutcome LargeError = Failed(EVaRestTransportFailure::Other, TEXT("ResponseTooLarge"));
	LargeError.Status = 503;
	const FCase Cases[] = {
		{ Status(200), TEXT("None") },
		{ Status(204), TEXT("None") },
		{ Status(404), TEXT("Http") },
		{ Status(503), TEXT("Http+retry") },
		{ Status(429), TEXT("Http+retry") },
		{ Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")), TEXT("Connection+retry") },
		// Plan decision 15: the engine's other Other is a network failure on curl (a reset, an empty reply)
		{ Failed(EVaRestTransportFailure::Other, TEXT("Other")), TEXT("Connection+retry") },
		{ Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")), TEXT("Timeout+retry") },
		{ Failed(EVaRestTransportFailure::Canceled, TEXT("Cancelled")), TEXT("Canceled") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("TransportDropped")), TEXT("Connection") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("InvalidHeader")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("HttpDisabled")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("DomainNotAllowed")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("BodyNotSent")), TEXT("Config") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("ResponseTooLarge")), TEXT("Decode") },
		{ LargeError, TEXT("Http+retry") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("AuthRejected")), TEXT("Auth") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("NoCredentials")), TEXT("Auth") },
		{ Failed(EVaRestTransportFailure::Other, TEXT("SessionCleared")), TEXT("Auth") },
		{ Failed(EVaRestTransportFailure::Timeout, TEXT("Deadline")), TEXT("Timeout") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("status %d, %s %s"), Case.Outcome.Status, LexToString(Case.Outcome.Failure), *Case.Outcome.FailureDetail.ToString()),
			Describe(ClassifyAttempt(Case.Outcome)), FString(Case.Expected));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityBodyVerbTest, "VaRest.Http.Fidelity.BodyOnVerbNotSent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityBodyVerbTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Plan decision 13 (final review M1): curl sends a body only with POST, PUT, PATCH and DELETE (CurlHttp.cpp:941-993);
	// the fake mirrors that, so a body on another verb is a Config error and never reaches the transport
	FTestRig Rig;
	const TCHAR* Refused[] = { TEXT("HEAD"), TEXT("OPTIONS"), TEXT("QUERY"), TEXT("PROPFIND") };
	const TCHAR* Sent[] = { TEXT("POST"), TEXT("PUT"), TEXT("PATCH"), TEXT("DELETE"), TEXT("post") };
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> RefusedOutcomes;
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> SentOutcomes;
	for (const TCHAR* Verb : Refused)
	{
		RefusedOutcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Request(Verb, TEXT("/r")).Body(TEXT("x")), RefusedOutcomes.Last()).Send();
	}
	for (const TCHAR* Verb : Sent)
	{
		SentOutcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Request(Verb, TEXT("/s")).Body(TEXT("x")), SentOutcomes.Last()).Send();
	}
	auto NoBody = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Request(TEXT("OPTIONS"), TEXT("/n")), NoBody).Send();
	const auto AllDone = [&RefusedOutcomes, &SentOutcomes, NoBody]
	{
		for (const auto& One : RefusedOutcomes) { if (!One->IsDone()) { return false; } }
		for (const auto& One : SentOutcomes) { if (!One->IsDone()) { return false; } }
		return NoBody->IsDone();
	};
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone)))
	{
		return false;
	}
	for (int32 Index = 0; Index < RefusedOutcomes.Num(); ++Index)
	{
		const FVaRestError& Error = RefusedOutcomes[Index]->Error;
		TestTrue(FString::Printf(TEXT("%s with a body: Kind Config, not retryable, naming the verb"), Refused[Index]),
			Error.Kind == EVaRestErrorKind::Config && !Error.bRetryable && Error.Message.Contains(Refused[Index]));
	}
	for (int32 Index = 0; Index < SentOutcomes.Num(); ++Index)
	{
		TestEqual(FString::Printf(TEXT("%s with a body: sent"), Sent[Index]), SentOutcomes[Index]->SuccessCalls.load(), 1);
	}
	TestEqual(TEXT("OPTIONS without a body: sent"), NoBody->SuccessCalls.load(), 1);
	TestEqual(TEXT("only the sendable ones reached the transport"), Rig.Fake->NumCalls(), 6);

	// A transport that says it sends any body (a custom one) gets it
	FTestRig Any;
	Any.Fake->bSendsBodyForAnyVerb = true;
	auto Query = MakeOutcome<FVaRestJson>();
	Observe(Any.Client.Request(TEXT("QUERY"), TEXT("/q")).Body(TEXT("x")), Query).Send();
	if (!TestTrue(TEXT("QUERY completes"), Any.Wait(Query)) || !TestEqual(TEXT("... sent through a transport that sends it"), Any.Fake->NumCalls(), 1))
	{
		return false;
	}
	TestEqual(TEXT("... with its body"), Any.Fake->ReadBody(0).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityPreCheckTest, "VaRest.Http.Fidelity.PreCheckRefusal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityPreCheckTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Plan decision 15 (final review M9): with [HTTP] bEnableHttp false the engine's PreCheck refuses the request
	// (HttpRequestCommon.cpp:83-87) and reports EHttpFailureReason::Other, which VaRest read as a retryable Connection
	// error. UpdateConfigs re-reads the switch (HttpModule.cpp:58-68); the real transport, no server needed.
	bool bWasEnabled = true;
	GConfig->GetBool(TEXT("HTTP"), TEXT("bEnableHttp"), bWasEnabled, GEngineIni);
	GConfig->SetBool(TEXT("HTTP"), TEXT("bEnableHttp"), false, GEngineIni);
	FHttpModule::Get().UpdateConfigs();
	ON_SCOPE_EXIT
	{
		GConfig->SetBool(TEXT("HTTP"), TEXT("bEnableHttp"), bWasEnabled, GEngineIni);
		FHttpModule::Get().UpdateConfigs();
	};
	FVaRestClient Client = FVaRestClient::Make(TEXT("http://127.0.0.1:1"));
	Client.WithLogLevel(EVaRestLogLevel::None);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/x")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), PumpUntil([Outcome] { return Outcome->IsDone(); }, 10.0, true)))
	{
		return false;
	}
	TestTrue(TEXT("HTTP disabled: Kind Config, not retryable"), Outcome->Error.Kind == EVaRestErrorKind::Config && !Outcome->Error.bRetryable);
	TestTrue(TEXT("... naming why"), Outcome->Error.Message.Contains(TEXT("HttpDisabled")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFidelityHeaderCollectTest, "VaRest.Http.Fidelity.HeadersOneByOne",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFidelityHeaderCollectTest::RunTest(const FString& Parameters)
{
	// Plan decision 14 (final review M2): the completion keeps each header line the delegate reports, repeats included
	VaRest::Http::FHttpTransportCompletion Completion(FVaRestTransportComplete(), MakeShared<VaRest::Http::FBodySink, ESPMode::ThreadSafe>(0),
		MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>(), nullptr);
	Completion.AddHeader(TEXT("Set-Cookie"), TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT"));
	Completion.AddHeader(TEXT("Content-Type"), TEXT("application/json"));
	Completion.AddHeader(TEXT("Set-Cookie"), TEXT("b=2"));
	const FVaRestHeaders Taken = Completion.TakeHeaders();
	TestEqual(TEXT("three lines, in order"), Taken.Num(), 3);
	int32 Cookies = 0;
	for (const FVaRestHeaders::FPair& Pair : Taken)
	{
		Cookies += Pair.Key.Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase) ? 1 : 0;
	}
	TestEqual(TEXT("both Set-Cookie headers, apart"), Cookies, 2);
	TestEqual(TEXT("taken once"), Completion.TakeHeaders().Num(), 0);
	return true;
}

#endif
```

An empty `FVaRestTransportComplete` is fine there: the completion's destructor claims it and finds nothing to call.

Append to `Source/VaRestTests/Private/VaRestIntegrationTest.cpp` (before the closing `#endif`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationRepeatedHeadersTest, "VaRest.Integration.Http.RepeatedHeaders",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationRepeatedHeadersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// Plan decision 14: curl joins repeats with ", " in GetAllHeaders (CurlHttp.cpp:519-530); an Expires date has a comma,
	// so two cookies could not be split back. Collected from OnHeaderReceived they arrive apart.
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/headers/set")).Query(TEXT("Set-Cookie"), TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT")).Query(TEXT("Set-Cookie"), TEXT("b=2")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Wait(Outcome)) || !Succeeded(*this, TEXT("the request"), Outcome))
	{
		return false;
	}
	TArray<FString> Cookies;
	for (const FVaRestHeaders::FPair& Pair : Outcome->Response.Headers)
	{
		if (Pair.Key.Equals(TEXT("Set-Cookie"), ESearchCase::IgnoreCase))
		{
			Cookies.Add(Pair.Value);
		}
	}
#if PLATFORM_APPLE
	// NSURLSession folds repeats itself (allHeaderFields, AppleHttp.cpp:55); the delegate replays that dictionary
	AddInfo(TEXT("Apple: repeated headers arrive as NSURLSession folded them"));
#else
	if (!TestEqual(TEXT("two Set-Cookie headers"), Cookies.Num(), 2))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the first, its comma intact"), Cookies[0], FString(TEXT("a=1; Expires=Wed, 21 Oct 2026 07:28:00 GMT")));
	TestEqualSensitive(TEXT("the second"), Cookies[1], FString(TEXT("b=2")));
#endif
	return true;
}
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Fidelity`
Expected: FAIL to compile (`VaRestClassify.h` not found; `bSendsBodyForAnyVerb`, `AddHeader`, `TakeHeaders` unknown).

- [ ] **Step 3: The classification** — create `Source/VaRestHttp/Private/VaRestClassify.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** One attempt's outcome as an error (spec 3.3) */
	struct FAttemptClass
	{
		EVaRestErrorKind Kind = EVaRestErrorKind::None;
		/** Spec 4.1's rule, which FVaRestError::bRetryable reports: a connection error, a timeout, or 408/425/429/500/502/503/504 */
		bool bRetryable = false;

		bool IsError() const { return Kind != EVaRestErrorKind::None; }
	};

	/** The kind a transport failure stands for, its detail first (plan decision 12): a local refusal is no network failure */
	EVaRestErrorKind KindFor(EVaRestTransportFailure Failure, FName Detail);

	/**
	 * The one mapping from an attempt's outcome to Kind and bRetryable (plan decision 12). The pipeline's FVaRestError and
	 * the retry stage both read it, so an error that says bRetryable is one the retry stage could have retried.
	 */
	FAttemptClass ClassifyAttempt(const FVaRestAttemptOutcome& Outcome);
}
```

Create `Source/VaRestHttp/Private/VaRestClassify.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClassify.h"

#include <initializer_list>

namespace VaRest::Http::ClassifyPrivate
{
	bool IsOneOf(FName Detail, std::initializer_list<const TCHAR*> Names)
	{
		for (const TCHAR* Name : Names)
		{
			if (Detail == FName(Name))
			{
				return true;
			}
		}
		return false;
	}
}

EVaRestErrorKind VaRest::Http::KindFor(EVaRestTransportFailure Failure, FName Detail)
{
	using namespace ClassifyPrivate;
	// The request itself (M3 reviews M14, N-4, task 7 M-1, I3; plan decisions 13 and 15): sending it again fails again
	if (IsOneOf(Detail, { TEXT("BodyRejected"), TEXT("InvalidHeader"), TEXT("UploadReadFailed"), TEXT("BodyFileMissing"),
		TEXT("HttpDisabled"), TEXT("DomainNotAllowed"), TEXT("BodyNotSent") }))
	{
		return EVaRestErrorKind::Config;
	}
	// What came back could not be kept (M3 decision 20, review M-7)
	if (IsOneOf(Detail, { TEXT("ResponseFileWriteFailed"), TEXT("ResponseBodyRejected"), TEXT("ResponseTooLarge") }))
	{
		return EVaRestErrorKind::Decode;
	}
	// The auth stages' own failures (Tasks 12-13): a rejected refresh, no credentials, a logout while parked
	if (IsOneOf(Detail, { TEXT("AuthRejected"), TEXT("NoCredentials"), TEXT("SessionCleared") }))
	{
		return EVaRestErrorKind::Auth;
	}
	switch (Failure)
	{
	case EVaRestTransportFailure::None: return EVaRestErrorKind::None;
	case EVaRestTransportFailure::Timeout: return EVaRestErrorKind::Timeout;
	case EVaRestTransportFailure::Canceled: return EVaRestErrorKind::Canceled;
	// Connection, and the engine's Other: on curl a reset or an empty reply (CurlHttp.cpp:1318-1330 makes only five
	// CURLcodes a ConnectionError; HandleRequestFailed calls the rest Other, HttpRequestCommon.cpp:291-310), plan decision 15
	default: return EVaRestErrorKind::Connection;
	}
}

VaRest::Http::FAttemptClass VaRest::Http::ClassifyAttempt(const FVaRestAttemptOutcome& Outcome)
{
	static const FName TooLarge(TEXT("ResponseTooLarge"));
	static const FName Dropped(TEXT("TransportDropped"));
	static const FName PastDeadline(TEXT("Deadline"));
	FAttemptClass Out;
	if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge && Outcome.Status > 0 && !IsSuccessStatus(Outcome.Status))
	{
		// M3 reviews M-6, M13: an error response whose body passed the JSON cap is still the HTTP error it was
		Out.Kind = EVaRestErrorKind::Http;
		Out.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
		return Out;
	}
	if (Outcome.IsTransportFailure())
	{
		Out.Kind = KindFor(Outcome.Failure, Outcome.FailureDetail);
		// A dropped completion (engine shutdown, NullHttp) would only be dropped again; a deadline will not come back
		Out.bRetryable = (Out.Kind == EVaRestErrorKind::Connection || Out.Kind == EVaRestErrorKind::Timeout)
			&& Outcome.FailureDetail != Dropped && Outcome.FailureDetail != PastDeadline;
		return Out;
	}
	if (!IsSuccessStatus(Outcome.Status))
	{
		Out.Kind = EVaRestErrorKind::Http;
		Out.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
	}
	return Out;
}
```

- [ ] **Step 4: The outcome's message, the transport's verbs** — in `Source/VaRestHttp/Public/VaRestStage.h`, `FVaRestAttemptOutcome`, after `FName FailureDetail;`:

```cpp
	/** A stage's own reason for a failure it made (an auth refresh rejected, no credentials): the error's Message when set */
	FString FailureMessage;
```

In `Source/VaRestHttp/Public/VaRestTransport.h`, in `IVaRestTransport` after `Start`:

```cpp
	/**
	 * Game thread. False when this transport sends no request body with Verb: Resolve then fails a request with a body on
	 * that verb, Kind=Config, instead of letting it go out without one (plan decision 13). GET is spec 3.2's case and never
	 * reaches this (its body is dropped with a Warning). Default: every verb.
	 */
	virtual bool SendsBodyFor(FStringView Verb) const { return true; }
```

and in `namespace VaRest::Http` beside `GetDefaultTransport`:

```cpp
	/**
	 * The verbs the engine's HTTP transport sends a body with: POST, PUT, PATCH, DELETE, any case (curl upper-cases the verb,
	 * CurlHttp.cpp:321, and attaches its read function only for those four, :941-993; HEAD sets CURLOPT_NOBODY, :975-978)
	 */
	VARESTHTTP_API bool IsBodyVerb(FStringView Verb);
```

Define it at the end of `Source/VaRestHttp/Private/VaRestHttpTransport.cpp` (before `GetDefaultTransport`):

```cpp
bool VaRest::Http::IsBodyVerb(FStringView Verb)
{
	return Verb.Equals(TEXTVIEW("POST"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("PUT"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("PATCH"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("DELETE"), ESearchCase::IgnoreCase);
}
```

and give `FHttpModuleTransport` (same file) the override:

```cpp
		virtual bool SendsBodyFor(FStringView Verb) const override { return IsBodyVerb(Verb); }
```

In `Source/VaRestHttp/Public/VaRestFakeTransport.h`, beside `bCompleteOnWorker`:

```cpp
	/** false (default): like the HTTP transport, a body only with POST, PUT, PATCH and DELETE (plan decision 13); true: any verb */
	bool bSendsBodyForAnyVerb = false;

	virtual bool SendsBodyFor(FStringView Verb) const override { return bSendsBodyForAnyVerb || VaRest::Http::IsBodyVerb(Verb); }
```

- [ ] **Step 5: The HTTP transport** — in `Source/VaRestHttp/Private/VaRestHttpTransportInternal.h` add `#include "Async/Mutex.h"`, `#include "Async/UniqueLock.h"`, `#include "VaRestHttpTypes.h"`, and in `FHttpTransportCompletion`:

```cpp
		/**
		 * Plan decision 14 (final review M2): one header line of the final response, as OnHeaderReceived reports it -- on curl
		 * once per line, repeats apart, a followed redirect's skipped (CurlHttp.cpp:509-535, :556-560); on Apple the folded
		 * dictionary replayed (HttpRequestImpl.cpp:40-60). Any thread: the HTTP thread, or the game thread under the spec 9
		 * fallback, which broadcasts them before it completes (CurlHttp.cpp:1305-1308).
		 */
		void AddHeader(const FString& Name, const FString& Value)
		{
			UE::TUniqueLock<UE::FMutex> Lock(HeadersMutex);
			CollectedHeaders.Add(Name, Value);
		}

		/** What AddHeader collected, once; empty when the delegate never ran (an engine mock response, review M-15) */
		FVaRestHeaders TakeHeaders()
		{
			UE::TUniqueLock<UE::FMutex> Lock(HeadersMutex);
			return MoveTemp(CollectedHeaders);
		}
```

and in its `private:` section:

```cpp
		UE::FMutex HeadersMutex;
		FVaRestHeaders CollectedHeaders;
```

In `Source/VaRestHttp/Private/VaRestHttpTransport.cpp`, `FHttpModuleTransport::Start`:

1. After the `BodyFile` size checks (before `bool bBodyAccepted = true;`):

```cpp
		// Plan decision 15 (final review M9): the engine's PreCheck refuses these (HttpRequestCommon.cpp:83-87, :109-113) and
		// reports EHttpFailureReason::Other, which reads as a retryable network failure; they are the configuration's
		if (!FHttpModule::Get().IsHttpEnabled())
		{
			return FailBeforeConnect(Http, Completion, TEXT("HttpDisabled"));
		}
		if (!FHttpModule::Get().GetHttpManager().IsDomainAllowed(Request.Url))
		{
			return FailBeforeConnect(Http, Completion, TEXT("DomainNotAllowed"));
		}
```

(add `#include "HttpManager.h"` at the top).

2. The header delegate also collects — replace the `OnHeaderReceived().BindLambda` body with:

```cpp
		Http->OnHeaderReceived().BindLambda([Completion](FHttpRequestPtr, const FString& HeaderName, const FString& HeaderValue)
		{
			if (!HeaderValue.IsEmpty() && HeaderName.Equals(TEXT("Content-Encoding"), ESearchCase::IgnoreCase))
			{
				Completion->bEncodedBody = true;
			}
			Completion->AddHeader(HeaderName, HeaderValue);
		});
```

3. In the completion delegate, replace `Result.Headers = FVaRestHeaders::FromLines(Resp->GetAllHeaders());` with:

```cpp
				// Plan decision 14: the lines one by one when the delegate saw them, else the engine's folded list
				Result.Headers = Completion->TakeHeaders();
				if (Result.Headers.IsEmpty())
				{
					Result.Headers = FVaRestHeaders::FromLines(Resp->GetAllHeaders());
				}
```

- [ ] **Step 6: The pipeline** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`: add `#include "VaRestClassify.h"`; delete `PipelinePrivate::KindFor` (lines 49-76); in `FClientState::OnChainDone` replace the four-way `if` (from `static const FName TooLarge` to the end of the `else if (!IsSuccessStatus(Outcome.Status))` block) with:

```cpp
	static const FName TooLarge(TEXT("ResponseTooLarge"));
	// Plan decision 12: one classification for this error and for the retry stage (Task 7)
	const FAttemptClass Class = ClassifyAttempt(Outcome);
	if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge && Class.Kind == EVaRestErrorKind::Http)
	{
		// Reviews M-6, M13: an error response whose body passed the JSON cap is still the HTTP error it was; the body is gone
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FString::Printf(TEXT("%s (its body is over the MaxJsonBytes limit of %lld and was not kept)"),
			*FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status), Ex.MaxJsonBytes);
		Finish.Error.bRetryable = Class.bRetryable;
	}
	else if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge)
	{
		// Decision 20: the transport stopped the body at the cap -- bounded memory on untrusted input
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("the JSON response is over the MaxJsonBytes limit of %lld; the transfer stopped there"), Ex.MaxJsonBytes);
	}
	else if (Outcome.IsTransportFailure())
	{
		Finish.Error.Kind = Class.Kind;
		Finish.Error.Message = Outcome.FailureMessage.IsEmpty() ? FVaRestError::DefaultMessage(Class.Kind, 0) : Outcome.FailureMessage;
		if (!Outcome.FailureDetail.IsNone())
		{
			Finish.Error.Message += FString::Printf(TEXT(" (%s)"), *Outcome.FailureDetail.ToString());
		}
		Finish.Error.bRetryable = Class.bRetryable;
	}
	else if (Class.Kind == EVaRestErrorKind::Http)
	{
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status);
		Finish.Error.bRetryable = Class.bRetryable;
	}
```

(the `static const FName Dropped` line goes: `ClassifyAttempt` owns that rule). In `FClientState::Resolve`, after the GET block (`Exchange.Body = FVaRestRequestBody();` … `}`) add:

```cpp
	if (!Exchange.Body.IsEmpty() && Cfg.Transport.IsValid() && !Cfg.Transport->SendsBodyFor(Exchange.Verb))
	{
		// Plan decision 13 (final review M1): sent without its body it would be another request, and BytesUp would count bytes
		// that never left (VaRestHttpTransport.cpp:398). The verb is a token here (checked above), so it is shown as it is.
		OutError = FString::Printf(TEXT("the client's transport sends no request body with %s (the engine's HTTP transport sends one only with POST, PUT, PATCH and DELETE)"),
			*Exchange.Verb);
		return false;
	}
```

- [ ] **Step 7: The docs** — in `Source/VaRestHttp/Public/VaRestClient.h`, `Request()`'s second paragraph becomes:

```cpp
	 * A body is sent only with POST, PUT, PATCH and DELETE on the engine's curl transport (Linux, Windows, Android): only
	 * those attach curl's read function (CurlHttp.cpp:941-968, :979-988, :995-1001). GET drops its body with a Warning
	 * (spec 3.2). A body on any other verb -- HEAD (CURLOPT_NOBODY, :975-978), OPTIONS, QUERY, PROPFIND -- fails the request
	 * with Kind=Config before anything is sent, on every platform (plan decision 13; IVaRestTransport::SendsBodyFor).
```

In `Source/VaRestHttp/Public/VaRestHttpTypes.h`, the `FVaRestHeaders` paragraph about curl becomes:

```cpp
 * Response headers arrive one per header line (plan decision 14): the HTTP transport collects them from the engine's
 * OnHeaderReceived, so two Set-Cookie headers are two entries. On Mac and iOS NSURLSession folds repeats into one value
 * before the engine sees them (allHeaderFields, AppleHttp.cpp:55). Repeats a stage Add()s to a request go out joined with
 * ", " (VaRestHttpTransport.cpp:139-150), which is wrong for Cookie ("; "): set that one once.
```

and `FVaRestResponse::Headers`'s comment becomes `/** One entry per response header line (see FVaRestHeaders; Apple folds repeats) */`.

- [ ] **Step 8: Run the tests**

Run: `Tools/run_tests.sh VaRest.Http.Fidelity` then `Tools/run_tests.sh --no-build VaRest.Integration.Http.RepeatedHeaders`
Expected: `0 failed` both.

- [ ] **Step 9: Restore-the-bug** — (a) delete the `IsHttpEnabled` check in `Start`: `VaRest.Http.Fidelity.PreCheckRefusal` FAILS (the engine's refusal arrives as a retryable `Connection`); restore, PASS. (b) Make `FVaRestFakeTransport::SendsBodyFor` return true: `VaRest.Http.Fidelity.BodyOnVerbNotSent` FAILS; restore, PASS. (c) Revert step 5.3 (always `FromLines`): `VaRest.Integration.Http.RepeatedHeaders` FAILS (one joined value); restore, PASS. Record all three.

- [ ] **Step 10: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestClassify.h Source/VaRestHttp/Private/VaRestClassify.cpp Source/VaRestHttp/Public/VaRestStage.h \
  Source/VaRestHttp/Public/VaRestTransport.h Source/VaRestHttp/Public/VaRestFakeTransport.h Source/VaRestHttp/Private/VaRestHttpTransportInternal.h \
  Source/VaRestHttp/Private/VaRestHttpTransport.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Public/VaRestClient.h \
  Source/VaRestHttp/Public/VaRestHttpTypes.h Source/VaRestHttp/Private/Tests/VaRestHttpFidelityTest.cpp Source/VaRestTests/Private/VaRestIntegrationTest.cpp
git commit -m "http: M4 task 2 -- engine-transport fidelity: bodies the transport would not send, repeated headers, PreCheck refusals, one classification

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 3: The redaction pipe by shape, and the current redactor (carries (a), (b))

Plan decisions 16 and 17. IN-14's decode-task excerpts (Task 16) launch on the redaction pipe from worker threads, which `GetRedactionPipe()`'s game-thread `check` forbids; and an auth refresh (Task 13) sets a new token while requests are in flight, which the `Send()`-time redactor of those requests does not know.

**Files:**
- Modify: `Source/VaRestHttp/Private/VaRestRedactionPipe.h`, `Source/VaRestHttp/Private/VaRestRedactor.cpp` (`LaunchRedaction`, `WaitForRedactionPipe`)
- Modify: `Source/VaRestHttp/Private/VaRestLogStage.cpp`, `Source/VaRestHttp/Private/VaRestHttpTesting.cpp` (callers)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h` (`GetRedactor()`), `Source/VaRestHttp/Private/VaRestPipeline.cpp`, `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (current redactor)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpRedactorTest.cpp`, `Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp`

**Interfaces:**
- Consumes: Task 1 (`EscapeControlsForLog`).
- Produces: private `bool VaRest::Http::LaunchRedaction(const TCHAR* DebugName, TUniqueFunction<void()>&& Body)` (any thread; false = not launched, Body dropped), `bool VaRest::Http::WaitForRedactionPipe()`; `GetRedactionPipe()` is gone; `TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> FClientState::GetRedactor() const` (any thread).

- [ ] **Step 1: Write the failing tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpRedactorTest.cpp` (add `#include "Tasks/Task.h"` and `#include "VaRestHttpTesting.h"` at the top; it already includes `VaRestRedactionPipe.h`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactorLaunchTest, "VaRest.Http.Redactor.LaunchFromAnyThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactorLaunchTest::RunTest(const FString& Parameters)
{
	// Plan decision 16 (carry (a), review M-10): workers launch while the game thread stops and restarts the pipe. A
	// launch either runs its body or reports false; none is lost and none runs into a deleted pipe.
	struct FCounts
	{
		std::atomic<int32> Ran{ 0 };
		std::atomic<int32> Accepted{ 0 };
		std::atomic<int32> Refused{ 0 };
		std::atomic<int32> WorkersDone{ 0 };
	};
	TSharedRef<FCounts, ESPMode::ThreadSafe> Counts = MakeShared<FCounts, ESPMode::ThreadSafe>();
	constexpr int32 Workers = 8;
	constexpr int32 PerWorker = 2000;
	for (int32 Worker = 0; Worker < Workers; ++Worker)
	{
		UE::Tasks::Launch(TEXT("VaRest.Test.LaunchRedaction"), [Counts]
		{
			for (int32 Index = 0; Index < PerWorker; ++Index)
			{
				if (VaRest::Http::LaunchRedaction(TEXT("VaRest.Test.Body"), [Counts] { ++Counts->Ran; }))
				{
					++Counts->Accepted;
				}
				else
				{
					++Counts->Refused;
				}
			}
			++Counts->WorkersDone;
		});
	}
	VaRest::Http::Testing::PumpUntil([Counts] { return Counts->Accepted.load() + Counts->Refused.load() > Workers * PerWorker / 4; });
	VaRest::Http::StopRedactionPipe();
	VaRest::Http::StartRedactionPipe();
	if (!TestTrue(TEXT("the workers finish"), VaRest::Http::Testing::PumpUntil([Counts] { return Counts->WorkersDone.load() == Workers; }, 30.0)))
	{
		return false;
	}
	VaRest::Http::WaitForRedactionPipe();
	TestEqual(TEXT("every launch reported"), Counts->Accepted.load() + Counts->Refused.load(), Workers * PerWorker);
	TestEqual(TEXT("every accepted body ran, no refused one did"), Counts->Ran.load(), Counts->Accepted.load());
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bAfter = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	TestTrue(TEXT("the restarted pipe takes work"), VaRest::Http::LaunchRedaction(TEXT("VaRest.Test.After"), [bAfter] { *bAfter = true; }));
	VaRest::Http::WaitForRedactionPipe();
	TestTrue(TEXT("... and runs it"), bAfter->load());
	return true;
}
```

Append to `Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCurrentRedactorTest, "VaRest.Http.Log.CurrentRedactor",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCurrentRedactorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	// Plan decision 17 (carry (b), task 14 review M10): a secret the client learns while a request is out -- what an auth
	// refresh does -- is scrubbed from that request's later lines and from its error's URL
	FLevelIsolation Isolation;
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Headers);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FString Late = TEXT("zz-late-secret-123456");
	auto Echoed = MakeOutcome<FVaRestJson>();
	auto Failed = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/late")), Echoed).Send();
	Observe(Rig.Client.Get(TEXT("/err")).Query(TEXT("q"), Late), Failed).Send();
	if (!TestTrue(TEXT("both dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([Echoed, Failed] { return Echoed->IsDone() && Failed->IsDone(); });
		return false;
	}
	const int32 Mark = Capture.GetLines().Num();
	Rig.Client.SetSecretValue(TEXT("Late"), Late);
	FVaRestFakeResponse Echo = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	Echo.Headers.Set(TEXT("X-Echo"), Late);
	Rig.Fake->Complete(0, MoveTemp(Echo));
	Rig.Fake->Complete(1, FVaRestFakeResponse::Json(404, FUtf8StringView(UTF8TEXT("{}"))));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Echoed, Failed] { return Echoed->IsDone() && Failed->IsDone(); })))
	{
		return false;
	}
	const TArray<FString> Lines = Capture.GetLines();
	bool bLeaked = false;
	bool bScrubbedHeader = false;
	for (int32 Index = Mark; Index < Lines.Num(); ++Index)
	{
		bLeaked |= Lines[Index].Contains(Late);
		bScrubbedHeader |= Lines[Index].Contains(TEXT("<   X-Echo: ***"));
	}
	TestFalse(TEXT("no line written after the secret was set holds it"), bLeaked);
	TestTrue(TEXT("... the echoed header shows ***"), bScrubbedHeader);
	TestFalse(TEXT("the error's URL is redacted with the current secrets"), Failed->Error.Url.Contains(Late));
	return true;
}
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Redactor.LaunchFromAnyThread`
Expected: FAIL to compile (`LaunchRedaction` and `WaitForRedactionPipe` unknown). After Step 3 builds, `VaRest.Http.Log.CurrentRedactor` FAILS on "no line written after the secret was set holds it" until Step 5.

- [ ] **Step 3: `LaunchRedaction`** — replace the declarations in `Source/VaRestHttp/Private/VaRestRedactionPipe.h` with:

```cpp
namespace VaRest::Http
{
	/**
	 * Core's redaction pipe (Inspector 7): body excerpts for logs and traces are redacted there, in order, never on the game
	 * thread. Any thread (plan decision 16): false, and Body dropped, before StartRedactionPipe, after StopRedactionPipe, or
	 * while it stops. Callers never hold the pipe, so none can race its delete (review M-10).
	 */
	bool LaunchRedaction(const TCHAR* DebugName, TUniqueFunction<void()>&& Body);
	/** Game thread (StartupModule) */
	void StartRedactionPipe();
	/**
	 * Game thread (ShutdownModule): refuses new launches, waits for launches already inside LaunchRedaction, then for the
	 * queued work, then deletes the pipe -- FPipe's destructor checks it is idle (Pipe.h:40-43)
	 */
	void StopRedactionPipe();
	/** Tests: waits until the pipe is idle; false when there is none */
	bool WaitForRedactionPipe();
}
```

In `Source/VaRestHttp/Private/VaRestRedactor.cpp` (add `#include "HAL/PlatformProcess.h"` and `#include "Misc/ScopeExit.h"`), replace `UE::Tasks::FPipe* Pipe = nullptr;` in `RedactorPrivate` with:

```cpp
	/**
	 * Plan decision 16. Trivially destructible, so safe as statics (review I-4). Sequentially consistent on purpose: a
	 * launcher raises Launching and then reads bStopping, the stopper raises bStopping and then reads Launching, so at
	 * least one sees the other (the launch is refused, or the stopper waits for it).
	 */
	std::atomic<UE::Tasks::FPipe*> Pipe{ nullptr };
	std::atomic<int32> Launching{ 0 };
	std::atomic<bool> bStopping{ true };
```

and replace `GetRedactionPipe`, `StartRedactionPipe` and `StopRedactionPipe` (lines 164-187) with:

```cpp
bool VaRest::Http::LaunchRedaction(const TCHAR* DebugName, TUniqueFunction<void()>&& Body)
{
	using namespace RedactorPrivate;
	// Read first without counting, so a stop under steady launching drains at once instead of waiting for a gap
	if (bStopping.load())
	{
		return false;
	}
	Launching.fetch_add(1);
	ON_SCOPE_EXIT { Launching.fetch_sub(1); };
	UE::Tasks::FPipe* Current = bStopping.load() ? nullptr : Pipe.load();
	if (!Current)
	{
		return false;
	}
	// FPipe::Launch takes any thread (atomics only, Pipe.h:63-77)
	Current->Launch(DebugName, MoveTemp(Body));
	return true;
}

void VaRest::Http::StartRedactionPipe()
{
	using namespace RedactorPrivate;
	check(IsInGameThread());
	check(!Pipe.load());
	Pipe.store(new UE::Tasks::FPipe(TEXT("VaRest.Redaction")));
	bStopping.store(false);
}

void VaRest::Http::StopRedactionPipe()
{
	using namespace RedactorPrivate;
	check(IsInGameThread());
	// ShutdownModule does not run in an `Automation RunTests ...; Quit` session (CLAUDE.md); a pipe left alive then is
	// leaked on purpose, and its destructor's idle check never runs
	bStopping.store(true);
	while (Launching.load() != 0)
	{
		FPlatformProcess::Yield();
	}
	if (UE::Tasks::FPipe* Existing = Pipe.exchange(nullptr))
	{
		Existing->WaitUntilEmpty();
		delete Existing;
	}
}

bool VaRest::Http::WaitForRedactionPipe()
{
	UE::Tasks::FPipe* Current = RedactorPrivate::Pipe.load();
	if (!Current)
	{
		return false;
	}
	Current->WaitUntilEmpty();
	return true;
}
```

In `Source/VaRestHttp/Private/VaRestHttpTesting.cpp`, `FlushRedactionPipe`'s body becomes `VaRest::Http::WaitForRedactionPipe();`.

- [ ] **Step 4: The log stage's launches** — in `Source/VaRestHttp/Private/VaRestLogStage.cpp`:

`LogBodyLater` becomes:

```cpp
	/** Copies at most LogBodyCap bytes here (game thread, one memcpy); redacts and writes on the redaction pipe (Inspector 7) */
	void LogBodyLater(uint64 RequestId, const TCHAR* Direction, const FRedactorRef& Redactor, FString ContentType, TConstArrayView<uint8> Bytes, int64 TotalBytes)
	{
		TArray<uint8> Head(Bytes.GetData(), FMath::Min(Bytes.Num(), LogBodyCap));
		CountCopied(Head.Num());
		LaunchRedaction(TEXT("VaRest.LogBody"), [RequestId, Direction, Redactor, Type = MoveTemp(ContentType), Prefix = MoveTemp(Head), TotalBytes]
		{
			WriteExcerpt(RequestId, Direction, Redactor->RedactBody(Type, Prefix, TotalBytes, LogBodyCap));
		});
	}
```

In `LogRequestBody` delete the `UE::Tasks::FPipe* Pipe = GetRedactionPipe(); if (!Pipe) { return; }` lines and change both `Pipe->Launch(TEXT("VaRest.LogBody"), [...] { ... });` calls to `LaunchRedaction(TEXT("VaRest.LogBody"), [...] { ... });` with the same captures and bodies.

- [ ] **Step 5: The current redactor** — in `Source/VaRestHttp/Private/VaRestClientState.h`, `FClientState`, after `GetConfig()`:

```cpp
		/**
		 * Any thread: the redactor of the configuration published now, with every secret set so far and the rotated ones
		 * still within their 10 minutes (plan decision 17, carry (b)). Logs, traces and FVaRestError::Url use it, not the
		 * Send() snapshot's, so a token refreshed while a request is out is scrubbed from that request's later lines.
		 */
		TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> GetRedactor() const { return GetConfig()->Redactor.ToSharedRef(); }
```

In `Source/VaRestHttp/Private/VaRestLogStage.cpp`:

- `LogOutcome` takes no redactor and no prebuilt URL; it reads the current redactor when the outcome comes back:

```cpp
	void LogOutcome(const FExchangeState& Ex, EVaRestLogLevel Level, double StartedAt, const FVaRestAttemptOutcome& Outcome)
	{
		// Plan decision 17: the redactor of now -- a secret set while the attempt ran (an auth refresh) is known
		const FRedactorRef Redactor = Ex.State->GetRedactor();
		FString Url;
```

  and its body keeps everything else (the `if (Url.IsEmpty()) { Url = ShownUrl(Redactor, Ex.Url); }` branch now always builds it).
- In `FLogStage::Process`, `const FRedactorRef Redactor = Ex.Config->Redactor.ToSharedRef();` becomes `const FRedactorRef Redactor = Ex.State->GetRedactor();`; the request line keeps using `Shown`, which is no longer captured; the continuation becomes:

```cpp
			const double StartedAt = Ex.Config->Clock->Now();
			Next.Proceed(Exchange, [Exchange, Level, StartedAt, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				LogOutcome(Internals(*Exchange), Level, StartedAt, Outcome);
				Done(MoveTemp(Outcome));
			});
```

- In `LogConfigError`, `ShownUrl(Cfg.Redactor.ToSharedRef(), ...)` becomes `ShownUrl(Ex.State->GetRedactor(), ...)`.

In `Source/VaRestHttp/Private/VaRestPipeline.cpp`: in `Resolve`, every `Cfg.Redactor->RedactUrl(` becomes `GetRedactor()->RedactUrl(`; in `Complete`, `Finish.Error.Url = Cfg.Redactor->RedactUrl(...)` becomes `Finish.Error.Url = GetRedactor()->RedactUrl(...)`.

In `Source/VaRestHttp/Private/VaRestTracePoints.cpp`: `Describe` starts with `const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Held = Ex.State->GetRedactor(); const FVaRestRedactor& Redactor = *Held;` in place of `const FVaRestRedactor& Redactor = *Cfg.Redactor;`; `Started`, `AttemptEnded` and `Completed` replace `const FVaRestRedactor& Redactor = *Ex.Config->Redactor;` the same way; `AttemptDispatched` replaces `Ex.Config->Redactor->RedactHeaders(` with `Ex.State->GetRedactor()->RedactHeaders(`.

- [ ] **Step 6: Run them**

Run: `Tools/run_tests.sh VaRest.Http.Redactor` then `Tools/run_tests.sh --no-build VaRest.Http.Log`
Expected: `0 failed` both.

- [ ] **Step 7: Restore-the-bug** — in `LogOutcome` use the redactor the stage held at dispatch (capture `Redactor` in the continuation again and pass it): `VaRest.Http.Log.CurrentRedactor` FAILS ("no line written after the secret was set holds it"); restore; PASS. In `LaunchRedaction` drop the second `bStopping` read (launch on `Pipe.load()` alone): run `VaRest.Http.Redactor.LaunchFromAnyThread` five times; report whether it failed or crashed (the race needs the stop to land between the two reads, so a pass is not proof here; the review checks the reasoning in the comment). Restore.

- [ ] **Step 8: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestRedactionPipe.h Source/VaRestHttp/Private/VaRestRedactor.cpp Source/VaRestHttp/Private/VaRestLogStage.cpp \
  Source/VaRestHttp/Private/VaRestHttpTesting.cpp Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestPipeline.cpp \
  Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Private/Tests/VaRestHttpRedactorTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp
git commit -m "http: M4 task 3 -- redaction launches from any thread by shape; logs, traces and errors use the current redactor

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 4: The M4 trace payloads — `Note`, `Excerpt`, `Marker`; the excerpt switch; a recording sink for tests (Inspector IN-4, IN-6, IN-9; WebSocket §19 item 4)

The payload types every later task emits. Nothing emits them yet except the test.

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestTrace.h`, `Source/VaRestHttp/Private/VaRestTrace.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestTracePoints.h`, `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (`Note`, `Marker`)
- Modify: `Source/VaRestHttp/Public/VaRestHttpTesting.h`, `Source/VaRestHttp/Private/VaRestHttpTesting.cpp` (`FTraceRecorder`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpTraceM4Test.cpp`

**Interfaces:**
- Consumes: Task 3 (`GetRedactor`).
- Produces (public): `VAREST_WITH_TRACE_EXCERPTS` (default `!UE_BUILD_SHIPPING`); `FVaRestTraceDetailSettings::MaxPendingExcerptBytes` (int64, 0 = 16 MiB); `enum class EVaRestTraceNoteKind : uint8 { CacheRevalidated, CacheStored, CacheServedStale, RetryScheduled, Queued, QueueExpired }`; `struct FVaRestTraceNote { uint64 RequestId; int32 Attempt; EVaRestTraceNoteKind Kind; FName Reason; double Delay; double RetryAfter; int64 Bytes; FString QueueId; }`; `enum class EVaRestTraceExcerptOwner : uint8 { Request, Response, Frame }`; `struct FVaRestTraceExcerptEvent { EVaRestTraceExcerptOwner Owner; uint64 RequestId; int32 Attempt; uint64 SocketId; uint64 FrameId; FVaRestTraceExcerpt Excerpt; }`; `enum class EVaRestTraceMarkerKind : uint8 { Online, Offline, SessionRefreshed, SessionReplaced, SessionCleared, SessionExpired, EnvironmentChanged }`; `struct FVaRestTraceMarker { EVaRestTraceMarkerKind Kind; uint64 ScopeId; uint64 ClientId; uint64 CauseRequestId; FString Text; }`; the variant `FVaRestTracePayload` gains the three. Private: `void TracePoints::Note(const FExchangeState&, FVaRestTraceNote&&)`, `void TracePoints::Marker(const FClientState&, EVaRestTraceMarkerKind, uint64 CauseRequestId, FStringView Text)`. Testing: `class VaRest::Http::Testing::FTraceRecorder : public IVaRestTraceSink` with `explicit FTraceRecorder(EVaRestTraceDetail Flags = EVaRestTraceDetail::Metadata)`, `FVaRestTraceDetailSettings Detail`, `TArray<FVaRestTraceEvent> For(uint64 RequestId) const`, `FString KindsOf(uint64 RequestId) const` (excerpts left out), `TArray<FVaRestTraceExcerptEvent> ExcerptsOf(uint64 RequestId) const`, `TArray<FVaRestTraceMarker> MarkersOf(uint64 ClientId) const`, `TArray<FVaRestTraceSnapshot> GetSnapshots() const`, `bool OnlyExcerptsOffGameThread() const`; `uint64 VaRest::Http::Testing::RequestIdOf(const FVaRestTraceEvent&)`; `struct FScopedTraceRecorder { FScopedTraceRecorder(EVaRestTraceDetail); ~FScopedTraceRecorder(); TSharedRef<FTraceRecorder, ESPMode::ThreadSafe> Recorder; }` (an isolation plus a registered recorder, removed on destruction).

- [ ] **Step 1: Write the failing test** — create `Source/VaRestHttp/Private/Tests/VaRestHttpTraceM4Test.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestTracePoints.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceM4PayloadsTest, "VaRest.Http.TraceSurface.M4Payloads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceM4PayloadsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	const uint64 Built = FVaRestTrace::Counters().EventsBuilt.load();
	{
		// IN-11: no sink, nothing built
		FVaRestScopedSinkIsolation Quiet;
		TracePoints::Marker(*FClientAccess::State(Rig.Client), EVaRestTraceMarkerKind::SessionRefreshed, 0, TEXT("x"));
		TestEqual(TEXT("a marker with no sink builds nothing"), FVaRestTrace::Counters().EventsBuilt.load(), Built);
	}
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	FTraceRecorder& Recorder = *Scoped.Recorder;

	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/a")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the recorder sees one request's kinds"), Recorder.KindsOf(Handle.GetId()), FString(TEXT("Started,Dispatched,Ended,Completed")));

	// IN-9: a marker's text is value-scrubbed with the client's current secrets
	Rig.Client.SetSecretValue(TEXT("S"), TEXT("marker-secret-12345"));
	TracePoints::Marker(*FClientAccess::State(Rig.Client), EVaRestTraceMarkerKind::SessionExpired, Handle.GetId(), TEXT("refresh said marker-secret-12345 is bad"));
	const TArray<FVaRestTraceMarker> Markers = Recorder.MarkersOf(Rig.Client.GetId());
	if (!TestEqual(TEXT("one marker"), Markers.Num(), 1))
	{
		return false;
	}
	TestTrue(TEXT("... its kind, client and cause"), Markers[0].Kind == EVaRestTraceMarkerKind::SessionExpired && Markers[0].ClientId == Rig.Client.GetId()
		&& Markers[0].CauseRequestId == Handle.GetId());
	TestTrue(TEXT("... its text scrubbed"), !Markers[0].Text.Contains(TEXT("marker-secret-12345")) && Markers[0].Text.Contains(TEXT("***")));

	// IN-6: an excerpt may come from a background thread; it is recorded and counted as such
	const uint64 Id = Handle.GetId();
	UE::Tasks::Launch(TEXT("VaRest.Test.Excerpt"), [Id]
	{
		FVaRestTraceExcerptEvent Payload;
		Payload.Owner = EVaRestTraceExcerptOwner::Response;
		Payload.RequestId = Id;
		Payload.Attempt = 1;
		Payload.Excerpt.Kind = EVaRestExcerptKind::Skipped;
		Payload.Excerpt.SkipReason = EVaRestExcerptSkip::Backlog;
		FVaRestTrace::Emit(EVaRestTraceKind::Excerpt, 0.0, FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Payload)));
	}).Wait();
	const TArray<FVaRestTraceExcerptEvent> Excerpts = Recorder.ExcerptsOf(Id);
	TestTrue(TEXT("the excerpt arrives, keyed by request"), Excerpts.Num() == 1 && Excerpts[0].Excerpt.SkipReason == EVaRestExcerptSkip::Backlog);
	TestTrue(TEXT("only excerpts came from another thread"), Recorder.OnlyExcerptsOffGameThread());
	TestEqualSensitive(TEXT("KindsOf leaves excerpts out (they race the game thread)"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));

	// IN-4 on the game thread through TracePoints::Note, which stamps the request and attempt
	FVaRestTraceNote Note;
	Note.Kind = EVaRestTraceNoteKind::RetryScheduled;
	Note.Reason = TEXT("Status503");
	Note.Delay = 0.5;
	FVaRestTrace::Emit(EVaRestTraceKind::Note, 0.0, FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(Note)));
	TestTrue(TEXT("a note with no request id is recorded under 0"), Recorder.For(0).ContainsByPredicate([](const FVaRestTraceEvent& Event)
	{
		return Event.Kind == EVaRestTraceKind::Note && Event.Payload.Get<FVaRestTraceNote>().Reason == FName(TEXT("Status503"));
	}));

	// IN-14's admission cap is one of the merged caps
	class FCapSink final : public IVaRestTraceSink
	{
	public:
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Settings;
			Settings.Flags = EVaRestTraceDetail::Metadata;
			Settings.MaxPendingExcerptBytes = 1234;
			return Settings;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent&) override {}
	};
	const TSharedRef<FCapSink, ESPMode::ThreadSafe> Caps = MakeShared<FCapSink, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(Caps);
	TestEqual(TEXT("MaxPendingExcerptBytes merges as the largest"), FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes, static_cast<int64>(1234));
	FVaRestTrace::RemoveSink(Caps);
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.TraceSurface.M4Payloads`
Expected: FAIL to compile (`FScopedTraceRecorder`, `TracePoints::Marker`, `FVaRestTraceExcerptEvent`, `MaxPendingExcerptBytes` unknown).

- [ ] **Step 3: The payloads** — in `Source/VaRestHttp/Public/VaRestTrace.h`:

After the includes:

```cpp
/**
 * Inspector 1.2 / IN-14: body excerpts in the trace. Default on in every build but Shipping; a target may set it in
 * GlobalDefinitions (a Shipping test build that wants capture sets 1). With 0 no body byte is copied for a sink, and
 * Started/AttemptEnded never say an excerpt is pending.
 */
#ifndef VAREST_WITH_TRACE_EXCERPTS
#define VAREST_WITH_TRACE_EXCERPTS !UE_BUILD_SHIPPING
#endif
```

In `FVaRestTraceDetailSettings`, after `FrameCap`:

```cpp
	/** IN-14: bytes of raw body copies that may wait for redaction at once; 0 = 16 MiB. Past it an excerpt is Skipped: Backlog. */
	int64 MaxPendingExcerptBytes = 0;
```

Before `using FVaRestTracePayload = ...` add:

```cpp
/** IN-4 */
enum class EVaRestTraceNoteKind : uint8
{
	CacheRevalidated,
	CacheStored,
	CacheServedStale,
	RetryScheduled,
	Queued,
	QueueExpired
};

/** IN-4: something a stage did to a request, on the game thread. The fields a kind does not use keep their defaults. */
struct FVaRestTraceNote
{
	uint64 RequestId = 0;
	int32 Attempt = 0;
	EVaRestTraceNoteKind Kind = EVaRestTraceNoteKind::RetryScheduled;
	/** RetryScheduled: why ("Status503", "Connection", "Timeout"); Queued (M4b): why it was queued */
	FName Reason;
	/** RetryScheduled: seconds until the next attempt starts */
	double Delay = 0.0;
	/** RetryScheduled: the seconds a Retry-After header asked for; -1 without one */
	double RetryAfter = -1.0;
	/** CacheStored (M4b) */
	int64 Bytes = 0;
	/** Queued, QueueExpired (M4b) */
	FString QueueId;
};

/** IN-6 Owner */
enum class EVaRestTraceExcerptOwner : uint8
{
	Request,
	Response,
	/** A socket frame (W5): SocketId and FrameId are set */
	Frame
};

/** IN-6: a redacted excerpt, from a decode task or the redaction pipe (never the HTTP thread) */
struct FVaRestTraceExcerptEvent
{
	EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
	uint64 RequestId = 0;
	/** The attempt whose response it is; 0 for a request body, taken before the first attempt */
	int32 Attempt = 0;
	/** W5 */
	uint64 SocketId = 0;
	/** W5 */
	uint64 FrameId = 0;
	FVaRestTraceExcerpt Excerpt;
};

/** IN-9 */
enum class EVaRestTraceMarkerKind : uint8
{
	Online,
	Offline,
	SessionRefreshed,
	SessionReplaced,
	SessionCleared,
	SessionExpired,
	/** M5 */
	EnvironmentChanged
};

/** IN-9: a client-level event, on the game thread */
struct FVaRestTraceMarker
{
	EVaRestTraceMarkerKind Kind = EVaRestTraceMarkerKind::SessionRefreshed;
	uint64 ScopeId = 0;
	uint64 ClientId = 0;
	/** The request that caused it (the 401 that started a refresh); 0 when none */
	uint64 CauseRequestId = 0;
	/** Value-scrubbed */
	FString Text;
};
```

and the variant becomes:

```cpp
using FVaRestTracePayload = TVariant<FVaRestTraceStarted, FVaRestTraceAttemptDispatched, FVaRestTraceAttemptEnded, FVaRestTraceNote, FVaRestTraceCompleted,
	FVaRestTraceExcerptEvent, FVaRestTraceScope, FVaRestTraceClient, FVaRestTraceMarker>;
```

`IVaRestTraceSink::OnEvent`'s comment gains a last sentence: `Switch on Event.Kind and ignore kinds you do not know: W5 adds the socket kinds and a later version may add more (WebSocket spec 19 item 4).`

In `Source/VaRestHttp/Private/VaRestTrace.cpp`, `PayloadMatchesKind` gains

```cpp
		case EVaRestTraceKind::Note: return Payload.IsType<FVaRestTraceNote>();
		case EVaRestTraceKind::Excerpt: return Payload.IsType<FVaRestTraceExcerptEvent>();
		case EVaRestTraceKind::Marker: return Payload.IsType<FVaRestTraceMarker>();
```

(its comment now says only the socket kinds are unchecked; so does the one in `Emit`), and `Merge` gains `Merged.MaxPendingExcerptBytes = FMath::Max(Merged.MaxPendingExcerptBytes, One.MaxPendingExcerptBytes);`.

- [ ] **Step 4: The emitters** — in `Source/VaRestHttp/Private/VaRestTracePoints.h`, add `class FClientState;` to the forward declarations and in `namespace TracePoints`:

```cpp
		/** IN-4, game thread: stamps the exchange's request id, attempt and clock */
		void Note(const FExchangeState& Ex, FVaRestTraceNote&& Payload);
		/** IN-9, game thread: Text is value-scrubbed with the client's current redactor */
		void Marker(const FClientState& Client, EVaRestTraceMarkerKind Kind, uint64 CauseRequestId, FStringView Text);
```

In `Source/VaRestHttp/Private/VaRestTracePoints.cpp`:

```cpp
void VaRest::Http::TracePoints::Note(const FExchangeState& Ex, FVaRestTraceNote&& Payload)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	Payload.RequestId = Ex.RequestId;
	Payload.Attempt = Ex.Attempt;
	FVaRestTrace::Emit(EVaRestTraceKind::Note, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceNote>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Marker(const FClientState& Client, EVaRestTraceMarkerKind Kind, uint64 CauseRequestId, FStringView Text)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = Client.GetConfig();
	FVaRestTraceMarker Payload;
	Payload.Kind = Kind;
	Payload.ScopeId = FVaRestTrace::ProcessScopeId;
	Payload.ClientId = Client.GetClientId();
	Payload.CauseRequestId = CauseRequestId;
	Payload.Text = Client.GetRedactor()->ScrubText(Text);
	FVaRestTrace::Emit(EVaRestTraceKind::Marker, Current->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceMarker>(), MoveTemp(Payload)));
}
```

- [ ] **Step 5: The recorder** — in `Source/VaRestHttp/Public/VaRestHttpTesting.h`, inside the `WITH_DEV_AUTOMATION_TESTS` namespace block, add `#include "VaRestTrace.h"` at the top of the file and:

```cpp
	/** The request an event belongs to (0 for client-level kinds, a socket frame's excerpt, and kinds it does not know) */
	VARESTHTTP_API uint64 RequestIdOf(const FVaRestTraceEvent& Event);

	/**
	 * A trace sink for tests, VaRest's and other modules': records every event and snapshot. OnEvent may run on any thread
	 * (excerpts come from tasks); the getters lock.
	 */
	class VARESTHTTP_API FTraceRecorder final : public IVaRestTraceSink
	{
	public:
		explicit FTraceRecorder(EVaRestTraceDetail InFlags = EVaRestTraceDetail::Metadata);

		virtual FVaRestTraceDetailSettings GetDetail() const override { return Detail; }
		virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override;
		virtual void OnEvent(const FVaRestTraceEvent& Event) override;

		/** One request's events, excerpts included, in arrival order */
		TArray<FVaRestTraceEvent> For(uint64 RequestId) const;
		/** "Started,Dispatched,Ended,Note,Completed" for one request, excerpts left out: they arrive from other threads, in no fixed place */
		FString KindsOf(uint64 RequestId) const;
		TArray<FVaRestTraceExcerptEvent> ExcerptsOf(uint64 RequestId) const;
		TArray<FVaRestTraceMarker> MarkersOf(uint64 ClientId) const;
		TArray<FVaRestTraceSnapshot> GetSnapshots() const;
		/** Inspector 9: every kind but Excerpt arrived on the game thread */
		bool OnlyExcerptsOffGameThread() const { return !bNonExcerptOffGameThread.load(); }

		/** Set before AddSink, or call FVaRestTrace::RefreshDetail after a change */
		FVaRestTraceDetailSettings Detail;

	private:
		mutable FCriticalSection Mutex;
		TArray<FVaRestTraceEvent> Events;
		TArray<FVaRestTraceSnapshot> Snapshots;
		std::atomic<bool> bNonExcerptOffGameThread{ false };
	};

	/** An FVaRestScopedSinkIsolation with one FTraceRecorder added inside it, removed when this goes. Game thread. */
	struct VARESTHTTP_API FScopedTraceRecorder
	{
		explicit FScopedTraceRecorder(EVaRestTraceDetail Flags = EVaRestTraceDetail::Metadata, int32 BodyCap = 0);
		~FScopedTraceRecorder();
		FScopedTraceRecorder(const FScopedTraceRecorder&) = delete;
		FScopedTraceRecorder& operator=(const FScopedTraceRecorder&) = delete;

		FVaRestScopedSinkIsolation Isolation;
		TSharedRef<FTraceRecorder, ESPMode::ThreadSafe> Recorder;
	};
```

In `Source/VaRestHttp/Private/VaRestHttpTesting.cpp`, inside `#if WITH_DEV_AUTOMATION_TESTS`:

```cpp
uint64 VaRest::Http::Testing::RequestIdOf(const FVaRestTraceEvent& Event)
{
	switch (Event.Kind)
	{
	case EVaRestTraceKind::Started: return Event.Payload.Get<FVaRestTraceStarted>().RequestId;
	case EVaRestTraceKind::AttemptDispatched: return Event.Payload.Get<FVaRestTraceAttemptDispatched>().RequestId;
	case EVaRestTraceKind::AttemptEnded: return Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId;
	case EVaRestTraceKind::Note: return Event.Payload.Get<FVaRestTraceNote>().RequestId;
	case EVaRestTraceKind::Completed: return Event.Payload.Get<FVaRestTraceCompleted>().RequestId;
	case EVaRestTraceKind::Excerpt:
	{
		const FVaRestTraceExcerptEvent& Excerpt = Event.Payload.Get<FVaRestTraceExcerptEvent>();
		return Excerpt.Owner == EVaRestTraceExcerptOwner::Frame ? 0 : Excerpt.RequestId;
	}
	default: return 0;
	}
}

VaRest::Http::Testing::FTraceRecorder::FTraceRecorder(EVaRestTraceDetail InFlags)
{
	Detail.Flags = InFlags;
}

void VaRest::Http::Testing::FTraceRecorder::OnAttached(const FVaRestTraceSnapshot& Snapshot)
{
	FScopeLock Lock(&Mutex);
	Snapshots.Add(Snapshot);
}

void VaRest::Http::Testing::FTraceRecorder::OnEvent(const FVaRestTraceEvent& Event)
{
	if (Event.Kind != EVaRestTraceKind::Excerpt && !IsInGameThread())
	{
		bNonExcerptOffGameThread = true;
	}
	FScopeLock Lock(&Mutex);
	Events.Add(Event);
}

TArray<FVaRestTraceEvent> VaRest::Http::Testing::FTraceRecorder::For(uint64 RequestId) const
{
	FScopeLock Lock(&Mutex);
	return Events.FilterByPredicate([RequestId](const FVaRestTraceEvent& Event) { return RequestIdOf(Event) == RequestId; });
}

FString VaRest::Http::Testing::FTraceRecorder::KindsOf(uint64 RequestId) const
{
	TArray<FString> Names;
	for (const FVaRestTraceEvent& Event : For(RequestId))
	{
		switch (Event.Kind)
		{
		case EVaRestTraceKind::Started: Names.Add(TEXT("Started")); break;
		case EVaRestTraceKind::AttemptDispatched: Names.Add(TEXT("Dispatched")); break;
		case EVaRestTraceKind::AttemptEnded: Names.Add(TEXT("Ended")); break;
		case EVaRestTraceKind::Note: Names.Add(TEXT("Note")); break;
		case EVaRestTraceKind::Completed: Names.Add(TEXT("Completed")); break;
		default: break;
		}
	}
	return FString::Join(Names, TEXT(","));
}

TArray<FVaRestTraceExcerptEvent> VaRest::Http::Testing::FTraceRecorder::ExcerptsOf(uint64 RequestId) const
{
	TArray<FVaRestTraceExcerptEvent> Out;
	for (const FVaRestTraceEvent& Event : For(RequestId))
	{
		if (Event.Kind == EVaRestTraceKind::Excerpt)
		{
			Out.Add(Event.Payload.Get<FVaRestTraceExcerptEvent>());
		}
	}
	return Out;
}

TArray<FVaRestTraceMarker> VaRest::Http::Testing::FTraceRecorder::MarkersOf(uint64 ClientId) const
{
	FScopeLock Lock(&Mutex);
	TArray<FVaRestTraceMarker> Out;
	for (const FVaRestTraceEvent& Event : Events)
	{
		if (Event.Kind == EVaRestTraceKind::Marker && Event.Payload.Get<FVaRestTraceMarker>().ClientId == ClientId)
		{
			Out.Add(Event.Payload.Get<FVaRestTraceMarker>());
		}
	}
	return Out;
}

TArray<FVaRestTraceSnapshot> VaRest::Http::Testing::FTraceRecorder::GetSnapshots() const
{
	FScopeLock Lock(&Mutex);
	return Snapshots;
}

VaRest::Http::Testing::FScopedTraceRecorder::FScopedTraceRecorder(EVaRestTraceDetail Flags, int32 BodyCap)
	: Recorder(MakeShared<FTraceRecorder, ESPMode::ThreadSafe>(Flags))
{
	Recorder->Detail.BodyCap = BodyCap;
	FVaRestTrace::AddSink(Recorder);
}

VaRest::Http::Testing::FScopedTraceRecorder::~FScopedTraceRecorder()
{
	// Before Isolation's destructor restores the outer list (members die in reverse order)
	FVaRestTrace::RemoveSink(Recorder);
}
```

- [ ] **Step 6: Run it, then the trace suites**

Run: `Tools/run_tests.sh VaRest.Http.TraceSurface` then `Tools/run_tests.sh --no-build VaRest.Http.Trace`
Expected: `0 failed` both.

- [ ] **Step 7: Restore-the-bug** — drop the `ScrubText` in `TracePoints::Marker` (copy `Text` as is): `M4Payloads` FAILS ("its text scrubbed"); restore; PASS. Drop the `MaxPendingExcerptBytes` line in `Merge`: FAILS; restore; PASS.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestTrace.h Source/VaRestHttp/Private/VaRestTrace.cpp Source/VaRestHttp/Private/VaRestTracePoints.h \
  Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestHttpTesting.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpTraceM4Test.cpp
git commit -m "http: M4 task 4 -- trace payloads Note, Excerpt and Marker (IN-4, IN-6, IN-9), the excerpt switch, a recording sink for tests

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 5: The backoff formula, the random stream, `Retry-After` (spec §4.1; WebSocket §19 item 1)

The public pieces a socket's reconnect policy shares with HTTP retry: one formula, a jitter that takes the caller's `FRandomStream` (plan decision 26), and the `Retry-After` parser. `IVaRestClock` with timers is M3's (M3 decision 1).

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestBackoff.h`, `Source/VaRestHttp/Private/VaRestBackoff.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp` (the client's stream)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (`WithRandomSeed`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpBackoffTest.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `UENUM(BlueprintType) enum class EVaRestJitter : uint8 { Full, Ratio, None }`; `USTRUCT(BlueprintType) struct FVaRestBackoff { float BaseDelay = 0.5f; float Multiplier = 2.f; float MaxDelay = 30.f; EVaRestJitter Jitter = Full; float JitterRatio = 0.5f; double GetCeiling(int32 Step) const; double GetDelay(int32 Step, FRandomStream& Random) const; }` (Step 1 = the first wait); `VARESTHTTP_API TOptional<double> VaRest::Http::ParseRetryAfter(FStringView Value, const FDateTime& NowUtc)`; `FRandomStream& FClientState::GetRandom()` (game thread); `FVaRestClient& FVaRestClient::WithRandomSeed(int32 Seed)`.

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestHttp/Private/Tests/VaRestHttpBackoffTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestBackoff.h"

#include <limits>

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffFormulaTest, "VaRest.Http.Backoff.Formula",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffFormulaTest::RunTest(const FString& Parameters)
{
	// Spec 4.1: 0.5 s, x2, at most 30 s
	const FVaRestBackoff Default;
	const double Expected[] = { 0.0, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0, 30.0, 30.0 };
	for (int32 Step = 0; Step < UE_ARRAY_COUNT(Expected); ++Step)
	{
		TestEqual(FString::Printf(TEXT("step %d"), Step), Default.GetCeiling(Step), Expected[Step]);
	}
	TestEqual(TEXT("a negative step waits nothing"), Default.GetCeiling(-3), 0.0);
	TestEqual(TEXT("a huge step stays at the cap (no overflow)"), Default.GetCeiling(100000), 30.0);
	FVaRestBackoff Odd;
	Odd.Multiplier = 0.5f;
	TestEqual(TEXT("a multiplier below 1 counts as 1"), Odd.GetCeiling(5), 0.5);
	Odd = FVaRestBackoff();
	Odd.BaseDelay = 1e30f;
	TestEqual(TEXT("a huge base stays at the cap"), Odd.GetCeiling(1), 30.0);
	Odd = FVaRestBackoff();
	Odd.BaseDelay = std::numeric_limits<float>::quiet_NaN();
	TestEqual(TEXT("a NaN base counts as 0"), Odd.GetCeiling(3), 0.0);
	Odd = FVaRestBackoff();
	Odd.MaxDelay = -1.f;
	TestEqual(TEXT("a negative cap counts as 0"), Odd.GetCeiling(3), 0.0);
	Odd = FVaRestBackoff();
	Odd.BaseDelay = 0.f;
	Odd.Multiplier = std::numeric_limits<float>::infinity();
	TestEqual(TEXT("a zero base with an infinite multiplier is 0, not the cap"), Odd.GetCeiling(4), 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffJitterTest, "VaRest.Http.Backoff.Jitter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffJitterTest::RunTest(const FString& Parameters)
{
	FVaRestBackoff Full;
	FRandomStream Random(1234);
	double Min = 1e9;
	double Max = -1.0;
	double Sum = 0.0;
	constexpr int32 Draws = 4000;
	for (int32 Index = 0; Index < Draws; ++Index)
	{
		const double Delay = Full.GetDelay(3, Random);
		Min = FMath::Min(Min, Delay);
		Max = FMath::Max(Max, Delay);
		Sum += Delay;
	}
	TestTrue(TEXT("full jitter: within [0, d)"), Min >= 0.0 && Max < 2.0);
	TestTrue(TEXT("... spread over it (mean near d/2)"), FMath::Abs(Sum / Draws - 1.0) < 0.1);

	FVaRestBackoff Ratio;
	Ratio.Jitter = EVaRestJitter::Ratio;
	Ratio.JitterRatio = 0.5f;
	Min = 1e9;
	Max = -1.0;
	for (int32 Index = 0; Index < Draws; ++Index)
	{
		const double Delay = Ratio.GetDelay(3, Random);
		Min = FMath::Min(Min, Delay);
		Max = FMath::Max(Max, Delay);
	}
	TestTrue(TEXT("ratio jitter: within d * (1 +/- r)"), Min >= 1.0 && Max <= 3.0 && Max > 2.0 && Min < 2.0);
	TestTrue(TEXT("ratio jitter never passes the cap"), Ratio.GetDelay(20, Random) <= 30.0);

	FVaRestBackoff Exact;
	Exact.Jitter = EVaRestJitter::None;
	TestEqual(TEXT("no jitter: exactly d"), Exact.GetDelay(3, Random), 2.0);

	// Plan decision 26: the caller's stream decides, so a seeded test knows the delay
	FRandomStream A(42);
	FRandomStream B(42);
	bool bSame = true;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		bSame &= Full.GetDelay(Index + 1, A) == Full.GetDelay(Index + 1, B);
	}
	TestTrue(TEXT("one seed, one sequence"), bSame);
	FRandomStream C(42);
	const int32 Before = C.GetCurrentSeed();
	Full.GetDelay(0, C);
	TestEqual(TEXT("step 0 draws nothing"), C.GetCurrentSeed(), Before);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBackoffRetryAfterTest, "VaRest.Http.Backoff.RetryAfter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBackoffRetryAfterTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::ParseRetryAfter;
	const FDateTime Now(2026, 10, 21, 12, 0, 0);
	auto Seconds = [&Now](const TCHAR* Value) { return ParseRetryAfter(Value, Now); };
	TestTrue(TEXT("delay-seconds"), Seconds(TEXT("120")) == TOptional<double>(120.0));
	TestTrue(TEXT("... trimmed"), Seconds(TEXT("  7 ")) == TOptional<double>(7.0));
	TestTrue(TEXT("... zero"), Seconds(TEXT("0")) == TOptional<double>(0.0));
	TestTrue(TEXT("... huge, bounded (no overflow)"), Seconds(TEXT("99999999999999999999999")) == TOptional<double>(1e9));
	TestFalse(TEXT("a negative number is not delay-seconds"), Seconds(TEXT("-1")).IsSet());
	TestFalse(TEXT("nor a fraction"), Seconds(TEXT("1.5")).IsSet());
	TestFalse(TEXT("nor garbage"), Seconds(TEXT("soon")).IsSet());
	TestFalse(TEXT("nor nothing"), Seconds(TEXT("")).IsSet());
	TestTrue(TEXT("an HTTP-date 30 s ahead"), Seconds(TEXT("Wed, 21 Oct 2026 12:00:30 GMT")) == TOptional<double>(30.0));
	TestTrue(TEXT("an HTTP-date in the past is 0"), Seconds(TEXT("Wed, 21 Oct 2026 11:00:00 GMT")) == TOptional<double>(0.0));
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Backoff`
Expected: FAIL to compile (`VaRestBackoff.h` not found).

- [ ] **Step 3: The header** — create `Source/VaRestHttp/Public/VaRestBackoff.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Math/RandomStream.h"
#include "Misc/DateTime.h"

#include "VaRestBackoff.generated.h"

/** How a backoff step's delay is spread (spec 4.1 "full jitter"; WebSocket spec 3.4's Ratio) */
UENUM(BlueprintType)
enum class EVaRestJitter : uint8
{
	/** Uniform in [0, d): spec 4.1's default */
	Full,
	/** d * (1 +/- JitterRatio), uniform, then capped: socket.io-client's style (WebSocket spec 3.4, 8) */
	Ratio,
	/** Exactly d */
	None
};

/**
 * WebSocket spec 19 item 1: the core's backoff formula, for HTTP retry and the socket reconnect policy alike. Step 1 is
 * the wait before the first retry: d = min(MaxDelay, BaseDelay * Multiplier^(Step - 1)), then jittered. BlueprintType
 * because M5's asset and the socket policy embed it: its field names freeze with M5 (plan decision 27).
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestBackoff
{
	GENERATED_BODY()

	/** Seconds before the first retry, before jitter (spec 4.1: 0.5) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float BaseDelay = 0.5f;

	/** Each step multiplies the delay (spec 4.1: 2); below 1 counts as 1 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "1"))
	float Multiplier = 2.f;

	/** No step waits longer, jitter included (spec 4.1: 30) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float MaxDelay = 30.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	EVaRestJitter Jitter = EVaRestJitter::Full;

	/** Ratio jitter only: the spread, 0 to 1 (socket.io-client: 0.5) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0", ClampMax = "1"))
	float JitterRatio = 0.5f;

	/** The un-jittered delay of Step; 0 below step 1. A non-finite or negative field counts as 0 (Multiplier as 1). */
	double GetCeiling(int32 Step) const;

	/**
	 * GetCeiling(Step), jittered with Random -- the caller's stream (plan decision 26): a client's, a socket's, a test's
	 * seeded one. One draw for Full and Ratio, none for None or below step 1. Never below 0, never above MaxDelay.
	 */
	double GetDelay(int32 Step, FRandomStream& Random) const;
};

namespace VaRest::Http
{
	/**
	 * RFC 9110 10.2.3 Retry-After: delay-seconds (digits only, surrounding spaces ignored) or an HTTP-date
	 * (FDateTime::ParseHttpDate, DateTime.h:695-713), as seconds from NowUtc; a date in the past is 0, and seconds stop
	 * growing at 1e9. Unset for anything else: a negative number, a fraction, text. Not capped: the retry policy is
	 * (spec 4.1: 60 s).
	 */
	VARESTHTTP_API TOptional<double> ParseRetryAfter(FStringView Value, const FDateTime& NowUtc);
}
```

- [ ] **Step 4: The formula** — create `Source/VaRestHttp/Private/VaRestBackoff.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBackoff.h"

namespace VaRest::Http::BackoffPrivate
{
	/** A field as a usable number: NaN, an infinity or anything below Floor is Floor */
	double Sane(float Value, double Floor)
	{
		return FMath::IsFinite(Value) ? FMath::Max(static_cast<double>(Value), Floor) : Floor;
	}
}

double FVaRestBackoff::GetCeiling(int32 Step) const
{
	using namespace VaRest::Http::BackoffPrivate;
	const double Base = Sane(BaseDelay, 0.0);
	if (Step < 1 || Base <= 0.0)
	{
		return 0.0;
	}
	const double Cap = Sane(MaxDelay, 0.0);
	const double Factor = FMath::IsFinite(Multiplier) ? FMath::Max(static_cast<double>(Multiplier), 1.0) : 1.0;
	// The exponent is bounded so pow stays defined; any step that large is at the cap long before
	const double Grown = Base * FMath::Pow(Factor, static_cast<double>(FMath::Min(Step - 1, 1023)));
	return FMath::IsFinite(Grown) ? FMath::Min(Grown, Cap) : Cap;
}

double FVaRestBackoff::GetDelay(int32 Step, FRandomStream& Random) const
{
	using namespace VaRest::Http::BackoffPrivate;
	if (Step < 1)
	{
		return 0.0;
	}
	const double Ceiling = GetCeiling(Step);
	switch (Jitter)
	{
	case EVaRestJitter::None:
		return Ceiling;
	case EVaRestJitter::Ratio:
	{
		const double Spread = FMath::IsFinite(JitterRatio) ? FMath::Clamp(static_cast<double>(JitterRatio), 0.0, 1.0) : 0.0;
		const double Scaled = Ceiling * (1.0 + Spread * (2.0 * Random.GetFraction() - 1.0));
		return FMath::Clamp(Scaled, 0.0, Sane(MaxDelay, 0.0));
	}
	default:
		// GetFraction is [0, 1) (RandomStream.h:110-122)
		return Ceiling * Random.GetFraction();
	}
}

TOptional<double> VaRest::Http::ParseRetryAfter(FStringView Value, const FDateTime& NowUtc)
{
	const FStringView Trimmed = Value.TrimStartAndEnd();
	if (Trimmed.IsEmpty())
	{
		return TOptional<double>();
	}
	bool bDigits = true;
	for (const TCHAR Char : Trimmed)
	{
		bDigits &= FChar::IsDigit(Char);
	}
	if (bDigits)
	{
		double Seconds = 0.0;
		for (const TCHAR Char : Trimmed)
		{
			// 31 years: every cap has long taken over, and the sum cannot overflow
			Seconds = FMath::Min(Seconds * 10.0 + static_cast<double>(Char - TEXT('0')), 1e9);
		}
		return Seconds;
	}
	FDateTime When;
	if (FDateTime::ParseHttpDate(FString(Trimmed), When))
	{
		return FMath::Max(0.0, (When - NowUtc).GetTotalSeconds());
	}
	return TOptional<double>();
}
```

- [ ] **Step 5: The client's stream** — in `Source/VaRestHttp/Private/VaRestClientState.h` add `#include "Math/RandomStream.h"`, and in `FClientState`:

```cpp
		/**
		 * Game thread: the stream retry jitter draws from (plan decision 26). Seeded from the client id and the CPU cycle
		 * counter, so clients made in the same frame differ; WithRandomSeed makes a test exact.
		 */
		FRandomStream& GetRandom() { return Random; }
```

and in its private members `FRandomStream Random;`. In `Source/VaRestHttp/Private/VaRestClientState.cpp` the constructor initialises it:

```cpp
VaRest::Http::FClientState::FClientState(uint64 InClientId, FClientConfig&& InConfig)
	: ClientId(InClientId)
	, Config(MakeShared<FClientConfig, ESPMode::ThreadSafe>(MoveTemp(InConfig)))
	, Random(static_cast<int32>(static_cast<uint32>(InClientId * 2654435761ull) ^ FPlatformTime::Cycles()))
{
}
```

(`#include "HAL/PlatformTime.h"`; `Random` is declared after `Config` in the class, keep that order.)

In `Source/VaRestHttp/Public/VaRestClient.h`, configuration section, after `WithClock`:

```cpp
	/** Game thread: seeds the stream retry jitter draws from (plan decision 26), so a test knows each delay */
	FVaRestClient& WithRandomSeed(int32 Seed);
```

and in `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithRandomSeed(int32 Seed)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->GetRandom().Initialize(Seed);
	}
	return *this;
}
```

- [ ] **Step 6: Run them**

Run: `Tools/run_tests.sh VaRest.Http.Backoff`
Expected: `0 failed`.

- [ ] **Step 7: Restore-the-bug** — drop `FMath::Min(..., Cap)` in `GetCeiling` (return `Grown`): `VaRest.Http.Backoff.Formula` FAILS at step 7; restore; PASS. Drop the `Base <= 0.0` early return: FAILS on "a zero base with an infinite multiplier"; restore; PASS.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestBackoff.h Source/VaRestHttp/Private/VaRestBackoff.cpp Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpBackoffTest.cpp
git commit -m "http: M4 task 5 -- the backoff formula with caller-supplied jitter, Retry-After, a seeded stream per client (WS 19 item 1)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 6: The retry policy and the retry decision (spec §4.1)

The policy struct and a pure function that decides, for one outcome, whether to retry and after how long. Task 7 puts it in a stage.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestRetry.h`, `Source/VaRestHttp/Private/VaRestRetryInternal.h`, `Source/VaRestHttp/Private/VaRestRetry.cpp`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp`

**Interfaces:**
- Consumes: Task 2 (`ClassifyAttempt`), Task 5 (`FVaRestBackoff`, `ParseRetryAfter`).
- Produces: `USTRUCT(BlueprintType) struct FVaRestRetryPolicy { int32 MaxRetries = 3; FVaRestBackoff Backoff; TArray<int32> RetryStatuses = {408, 425, 429, 500, 502, 503, 504}; bool bRetryConnectionErrors = true; bool bRetryTimeouts = true; bool bRespectRetryAfter = true; float MaxRetryAfter = 60.f; float Deadline = 0.f; static FVaRestRetryPolicy Disabled(); static FVaRestRetryPolicy Exponential(int32 InMaxRetries = 3); FVaRestRetryPolicy WithDeadline(float Seconds) const; FVaRestRetryPolicy WithBackoff(const FVaRestBackoff&) const; bool IsEnabled() const; }`; `VARESTHTTP_API bool VaRest::Http::IsIdempotentVerb(FStringView Verb)`; private `struct VaRest::Http::FRetryDecision { bool bRetry; double Delay; double RetryAfter; FName Reason; }`, `FRetryDecision VaRest::Http::DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone, double Now, double DeadlineAt, const FDateTime& NowUtc, FRandomStream& Random)`.

- [ ] **Step 1: Write the failing test** — create `Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetry.h"
#include "VaRestRetryInternal.h"

namespace VaRest::Http::RetryTestPrivate
{
	FVaRestAttemptOutcome Status(int32 Code, const TCHAR* RetryAfter = nullptr)
	{
		FVaRestAttemptOutcome Out;
		Out.Status = Code;
		if (RetryAfter)
		{
			Out.Headers.Set(TEXT("Retry-After"), RetryAfter);
		}
		return Out;
	}

	FVaRestAttemptOutcome Failed(EVaRestTransportFailure Failure, const TCHAR* Detail)
	{
		FVaRestAttemptOutcome Out;
		Out.Failure = Failure;
		Out.FailureDetail = Detail;
		return Out;
	}

	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** A policy whose delays a test can read off: no jitter, 0.5 s doubling */
	FVaRestRetryPolicy Exact(int32 MaxRetries = 3)
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(MaxRetries);
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDecideTest, "VaRest.Http.Retry.Decide",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDecideTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::RetryTestPrivate;
	const FDateTime NowUtc(2026, 10, 21, 12, 0, 0);
	const FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(3);
	auto Decide = [&](const FVaRestRetryPolicy& With, const FVaRestAttemptOutcome& Outcome, int32 Done = 0, double Now = 0.0, double DeadlineAt = 0.0)
	{
		FRandomStream Random(7);
		return DecideRetry(With, Outcome, Done, Now, DeadlineAt, NowUtc, Random);
	};

	FRandomStream Same(7);
	const FRetryDecision First = Decide(Policy, Status(503));
	TestTrue(TEXT("503: retried, after the backoff's first delay from the caller's stream"), First.bRetry && First.Delay == Policy.Backoff.GetDelay(1, Same));
	TestTrue(TEXT("... the reason names the status, no Retry-After"), First.Reason == FName(TEXT("Status503")) && First.RetryAfter < 0.0);
	TestFalse(TEXT("after MaxRetries: no more"), Decide(Policy, Status(503), 3).bRetry);
	TestFalse(TEXT("404: not a retried status"), Decide(Policy, Status(404)).bRetry);
	FVaRestRetryPolicy Conflict = Policy;
	Conflict.RetryStatuses.Add(409);
	TestTrue(TEXT("a status the policy adds is retried"), Decide(Conflict, Status(409)).bRetry);
	TestTrue(TEXT("a connection error"), Decide(Policy, Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"))).bRetry);
	TestTrue(TEXT("the engine's Other (a reset, an empty reply on curl)"), Decide(Policy, Failed(EVaRestTransportFailure::Other, TEXT("Other"))).bRetry);
	FVaRestRetryPolicy NoConnection = Policy;
	NoConnection.bRetryConnectionErrors = false;
	TestFalse(TEXT("... unless the policy says no"), Decide(NoConnection, Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"))).bRetry);
	TestTrue(TEXT("a timeout"), Decide(Policy, Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut"))).bRetry);
	FVaRestRetryPolicy NoTimeout = Policy;
	NoTimeout.bRetryTimeouts = false;
	TestFalse(TEXT("... unless the policy says no"), Decide(NoTimeout, Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut"))).bRetry);
	const TCHAR* Never[] = { TEXT("Cancelled"), TEXT("InvalidHeader"), TEXT("TransportDropped"), TEXT("AuthRejected"), TEXT("HttpDisabled"), TEXT("ResponseTooLarge") };
	for (const TCHAR* Detail : Never)
	{
		const EVaRestTransportFailure Failure = FCString::Strcmp(Detail, TEXT("Cancelled")) == 0 ? EVaRestTransportFailure::Canceled : EVaRestTransportFailure::Other;
		TestFalse(FString::Printf(TEXT("never retried: %s"), Detail), Decide(Policy, Failed(Failure, Detail)).bRetry);
	}
	TestFalse(TEXT("never retried: a deadline"), Decide(Policy, Failed(EVaRestTransportFailure::Timeout, TEXT("Deadline"))).bRetry);
	TestFalse(TEXT("a disabled policy"), Decide(FVaRestRetryPolicy::Disabled(), Status(503)).bRetry);

	// Retry-After replaces the backoff (plan decision 11), capped (spec 4.1: 60 s)
	const FRetryDecision Asked = Decide(Policy, Status(503, TEXT("5")));
	TestTrue(TEXT("Retry-After: 5"), Asked.bRetry && Asked.Delay == 5.0 && Asked.RetryAfter == 5.0);
	const FRetryDecision Long = Decide(Policy, Status(429, TEXT("120")));
	TestTrue(TEXT("Retry-After: 120 waits the cap, and says what was asked"), Long.bRetry && Long.Delay == 60.0 && Long.RetryAfter == 120.0);
	const FRetryDecision Dated = Decide(Policy, Status(503, TEXT("Wed, 21 Oct 2026 12:00:30 GMT")));
	TestTrue(TEXT("an HTTP-date"), Dated.bRetry && Dated.Delay == 30.0);
	FRandomStream Again(7);
	const FRetryDecision Garbage = Decide(Policy, Status(503, TEXT("soon")));
	TestTrue(TEXT("garbage: the backoff"), Garbage.bRetry && Garbage.Delay == Policy.Backoff.GetDelay(1, Again) && Garbage.RetryAfter < 0.0);
	FVaRestRetryPolicy Deaf = Exact();
	Deaf.bRespectRetryAfter = false;
	TestTrue(TEXT("a policy that ignores Retry-After"), Decide(Deaf, Status(503, TEXT("5"))).Delay == 0.5);

	// The deadline (plan decision 10): a retry that would start at or past it does not happen
	const FRetryDecision Late = Decide(Exact(), Status(503), 0, 10.0, 10.4);
	TestTrue(TEXT("a retry past the deadline: none, saying why"), !Late.bRetry && Late.Reason == FName(TEXT("Deadline")));
	TestTrue(TEXT("one before it: yes"), Decide(Exact(), Status(503), 0, 10.0, 11.0).bRetry);

	// Spec 4.1's verbs
	for (const TCHAR* Verb : { TEXT("GET"), TEXT("HEAD"), TEXT("OPTIONS"), TEXT("PUT"), TEXT("DELETE"), TEXT("get") })
	{
		TestTrue(FString::Printf(TEXT("%s is idempotent"), Verb), IsIdempotentVerb(Verb));
	}
	for (const TCHAR* Verb : { TEXT("POST"), TEXT("PATCH"), TEXT("QUERY"), TEXT("CONNECT") })
	{
		TestFalse(FString::Printf(TEXT("%s is not (spec 4.1 retries it only with Idempotent())"), Verb), IsIdempotentVerb(Verb));
	}
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Retry.Decide`
Expected: FAIL to compile (`VaRestRetry.h` not found).

- [ ] **Step 3: The policy** — create `Source/VaRestHttp/Public/VaRestRetry.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBackoff.h"

#include "VaRestRetry.generated.h"

/**
 * Spec 4.1. A request is retried only when it is idempotent -- GET, HEAD, OPTIONS, PUT or DELETE, or marked Idempotent()
 * -- and its client or the request itself has an enabled policy: a client without WithRetry() does not retry (plan
 * decision 1). BlueprintType: M5's asset embeds it, and its field names freeze then (plan decision 27).
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestRetryPolicy
{
	GENERATED_BODY()

	/** Attempts after the first (spec 4.1: 3); 0 = no retry. A session replay after a refresh is not a retry (plan decision 2). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	int32 MaxRetries = 3;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FVaRestBackoff Backoff;

	/** The statuses retried (spec 4.1) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<int32> RetryStatuses = { 408, 425, 429, 500, 502, 503, 504 };

	/** No usable response: refused, reset, empty, or no data for the activity timeout */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bRetryConnectionErrors = true;

	/** The attempt's total timeout passed */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bRetryTimeouts = true;

	/** Wait what a Retry-After header asks (seconds or an HTTP-date) instead of the backoff, at most MaxRetryAfter (plan decision 11) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bRespectRetryAfter = true;

	/** Seconds (spec 4.1: 60) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float MaxRetryAfter = 60.f;

	/**
	 * Seconds from the request's start; 0 = none (spec 4.1, 4.7; plan decision 10). No retry starts at or past it, each
	 * attempt's total timeout is cut to the time left, and an attempt that would start past it fails Kind=Timeout without
	 * connecting. Waiting in the limiter's queue counts.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float Deadline = 0.f;

	/** No retries */
	static FVaRestRetryPolicy Disabled();
	/** Spec 4.1's defaults with InMaxRetries retries */
	static FVaRestRetryPolicy Exponential(int32 InMaxRetries = 3);
	FVaRestRetryPolicy WithDeadline(float Seconds) const;
	FVaRestRetryPolicy WithBackoff(const FVaRestBackoff& InBackoff) const;

	bool IsEnabled() const { return MaxRetries > 0; }
};

namespace VaRest::Http
{
	/** Spec 4.1 (RFC 9110 9.2.2): GET, HEAD, OPTIONS, PUT, DELETE, any case. POST and PATCH retry only with Idempotent(). */
	VARESTHTTP_API bool IsIdempotentVerb(FStringView Verb);
}
```

Create `Source/VaRestHttp/Private/VaRestRetryInternal.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestRetry.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** What the retry stage does with one outcome */
	struct FRetryDecision
	{
		bool bRetry = false;
		/** Seconds until the next attempt */
		double Delay = 0.0;
		/** What a Retry-After header asked for, in seconds, before the cap; -1 without one */
		double RetryAfter = -1.0;
		/** For the trace (IN-4 Reason) and the log line: "Status503", "Connection", "Timeout"; why not, when bRetry is false */
		FName Reason;
	};

	/**
	 * Spec 4.1, for one outcome. RetriesDone: retries already made. Now and DeadlineAt are the request's clock (DeadlineAt 0
	 * = none); NowUtc dates a Retry-After HTTP-date. Draws once from Random when it retries by backoff, never otherwise.
	 * Eligibility by verb is the caller's (FExchangeState::bRetryEligible).
	 */
	FRetryDecision DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone, double Now, double DeadlineAt,
		const FDateTime& NowUtc, FRandomStream& Random);

	/** Spec 4's retry stage (Task 7): the first built-in Outer stage */
	FVaRestStageRef MakeRetryStage();
}
```

- [ ] **Step 4: The decision** — create `Source/VaRestHttp/Private/VaRestRetry.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRetryInternal.h"

#include "VaRestClassify.h"

FVaRestRetryPolicy FVaRestRetryPolicy::Disabled()
{
	FVaRestRetryPolicy Policy;
	Policy.MaxRetries = 0;
	return Policy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::Exponential(int32 InMaxRetries)
{
	FVaRestRetryPolicy Policy;
	Policy.MaxRetries = FMath::Max(0, InMaxRetries);
	return Policy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::WithDeadline(float Seconds) const
{
	FVaRestRetryPolicy Copy = *this;
	Copy.Deadline = FMath::IsFinite(Seconds) ? FMath::Max(0.f, Seconds) : 0.f;
	return Copy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::WithBackoff(const FVaRestBackoff& InBackoff) const
{
	FVaRestRetryPolicy Copy = *this;
	Copy.Backoff = InBackoff;
	return Copy;
}

bool VaRest::Http::IsIdempotentVerb(FStringView Verb)
{
	return Verb.Equals(TEXTVIEW("GET"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("HEAD"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("OPTIONS"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("PUT"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("DELETE"), ESearchCase::IgnoreCase);
}

VaRest::Http::FRetryDecision VaRest::Http::DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone,
	double Now, double DeadlineAt, const FDateTime& NowUtc, FRandomStream& Random)
{
	FRetryDecision Out;
	if (!Policy.IsEnabled() || RetriesDone >= Policy.MaxRetries)
	{
		Out.Reason = TEXT("Exhausted");
		return Out;
	}
	// Plan decision 12: the same classification the request's FVaRestError gets
	const FAttemptClass Class = ClassifyAttempt(Outcome);
	bool bEligible = false;
	switch (Class.Kind)
	{
	case EVaRestErrorKind::Connection:
		bEligible = Class.bRetryable && Policy.bRetryConnectionErrors;
		Out.Reason = TEXT("Connection");
		break;
	case EVaRestErrorKind::Timeout:
		bEligible = Class.bRetryable && Policy.bRetryTimeouts;
		Out.Reason = TEXT("Timeout");
		break;
	case EVaRestErrorKind::Http:
		// The policy's list, not FVaRestError::IsRetryableStatus: a game may add 409 or drop 500
		bEligible = Policy.RetryStatuses.Contains(Outcome.Status);
		Out.Reason = FName(*FString::Printf(TEXT("Status%d"), Outcome.Status));
		break;
	default:
		Out.Reason = TEXT("NotRetryable");
		break;
	}
	if (!bEligible)
	{
		return Out;
	}
	TOptional<double> Asked;
	if (Class.Kind == EVaRestErrorKind::Http && Policy.bRespectRetryAfter)
	{
		if (const FString* Header = Outcome.Headers.Find(TEXT("Retry-After")))
		{
			Asked = ParseRetryAfter(*Header, NowUtc);
		}
	}
	double Delay = 0.0;
	if (Asked.IsSet())
	{
		// Plan decision 11: what the server asked, capped (spec 4.1: 60 s)
		Out.RetryAfter = Asked.GetValue();
		const double Cap = FMath::IsFinite(Policy.MaxRetryAfter) ? FMath::Max(0.0, static_cast<double>(Policy.MaxRetryAfter)) : 0.0;
		Delay = FMath::Min(Asked.GetValue(), Cap);
	}
	else
	{
		Delay = Policy.Backoff.GetDelay(RetriesDone + 1, Random);
	}
	if (DeadlineAt > 0.0 && Now + Delay >= DeadlineAt)
	{
		// Plan decision 10: the next attempt would start at or past the deadline
		Out.Reason = TEXT("Deadline");
		return Out;
	}
	Out.bRetry = true;
	Out.Delay = Delay;
	return Out;
}
```

(`MakeRetryStage` is declared now and defined in Task 7; nothing calls it yet, so the link is fine.)

- [ ] **Step 5: Run it**

Run: `Tools/run_tests.sh VaRest.Http.Retry.Decide`
Expected: `0 failed`.

- [ ] **Step 6: Restore-the-bug** — use `FVaRestError::IsRetryableStatus(Outcome.Status)` instead of the policy's list: FAILS on "a status the policy adds is retried"; restore; PASS. Drop the cap (`Delay = Asked.GetValue()`): FAILS on "Retry-After: 120"; restore; PASS.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestRetry.h Source/VaRestHttp/Private/VaRestRetryInternal.h Source/VaRestHttp/Private/VaRestRetry.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp
git commit -m "http: M4 task 6 -- the retry policy and its decision: statuses, connection errors, timeouts, Retry-After, deadline (spec 4.1)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 7: The retry stage — the built-in `Outer` stage, the interrupt hook, the builder and client API, the trace and the log (spec §4.1; IN-2, IN-4)

Plan decisions 1, 2, 7, 8. The stage re-issues an eligible request after the decided delay on the request's clock; the attempt boundary behind it makes each re-issue a new attempt. While it waits it holds the exchange through `FExchangeState::Interrupt`, which `FClientState::Cancel` calls, so `Cancel()`, `CancelAll`, owner-gone and shutdown all reach it. The re-issue goes through the limiter again, behind whoever waits there (spec §4.6; Task 8 tests that).

**Files:**
- Modify: `Source/VaRestHttp/Private/VaRestExchangeState.h` (retry state, `Interrupt`, gap fields; `IsOwnerGone`/`MarkOwnerGone` move here)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`Retry()`, `NoRetry()`, `WithRetry()`; `FRequestSpec::Retry`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (`WithRetry`; `FRequestSpecAccess::Send` copies the policy)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp` (`FClientConfig::Retry`; `Publish` adds the stage)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Resolve`, the transport stage, `Cancel`, `Complete`)
- Modify: `Source/VaRestHttp/Private/VaRestRetry.cpp` (the stage), `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (gap fields)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp`

**Interfaces:**
- Consumes: Tasks 2, 4, 5, 6.
- Produces: `TVaRestRequest<T>::Retry(const FVaRestRetryPolicy& InPolicy) &&`, `TVaRestRequest<T>::NoRetry() &&`; `FVaRestClient& FVaRestClient::WithRetry(const FVaRestRetryPolicy& Policy)`; `FClientConfig::Retry` (default `Disabled()`); on `FExchangeState`: `FVaRestRetryPolicy RetryPolicy`, `TOptional<FVaRestRetryPolicy> RetryOverride`, `bool bRetryEligible`, `bool bMayReissue`, `double DeadlineAt` (0 until Task 8), `int32 RetriesDone`, `TUniqueFunction<void()> Interrupt`, `EVaRestTraceGap NextGap`, `double NextGapSeconds`; `inline bool VaRest::Http::IsOwnerGone(const FExchangeState&)`, `inline void VaRest::Http::MarkOwnerGone(FExchangeState&)` (moved from `PipelinePrivate`). The chain is now: custom Outer → `Retry` → attempt boundary → custom PerAttempt → log → limiter → transport (Tasks 12-13 add the auth stages).

- [ ] **Step 1: Write the failing tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp` (before `#endif`); add to its `RetryTestPrivate` namespace:

```cpp
	int32 CallsTo(const VaRest::Http::Testing::FTestRig& Rig, const TCHAR* Path)
	{
		int32 Count = 0;
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			Count += Rig.Fake->GetCall(Index).Request.Url.EndsWith(Path) ? 1 : 0;
		}
		return Count;
	}

	/** Cancels everything the rig has in flight and pumps until it is all done: the drain of a test that stops early */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}
```

and the tests:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryRecoversTest, "VaRest.Http.Retry.Recovers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryRecoversTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(200, R"({"ok":1})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/r")), Outcome).Send();
	if (!TestTrue(TEXT("the first attempt, then a backoff timer"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestFalse(TEXT("not done while it backs off"), Outcome->IsDone());
	TestEqual(TEXT("... and still in flight"), Rig.Client.GetInFlightCount(), 1);
	Rig.Clock->Advance(0.499);
	Rig.Drain();
	TestEqual(TEXT("no attempt before the delay"), Rig.Fake->NumCalls(), 1);
	Rig.Clock->Advance(0.001);
	if (!TestTrue(TEXT("the second at 0.5 s, then a 1 s backoff"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	if (!TestEqual(TEXT("three calls"), Rig.Fake->NumCalls(), 3))
	{
		return false;
	}
	TestTrue(TEXT("a success after three attempts"), Outcome->SuccessCalls.load() == 1 && Outcome->ErrorCalls.load() == 0 && Outcome->Response.Attempts == 3);
	TestTrue(TEXT("... with the last attempt's body"), Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("ok")).AsInt() == 1);
	TestEqual(TEXT("each attempt numbered"), Rig.Fake->GetCall(2).Request.Attempt, 3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryNonIdempotentTest, "VaRest.Http.Retry.NonIdempotentSentOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryNonIdempotentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Spec 4.1: "Only idempotent verbs (GET, HEAD, OPTIONS, PUT, DELETE) retry by default; POST/PATCH only with .Idempotent()"
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> KeyedCalls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([KeyedCalls](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/drop")))
		{
			return FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError"));
		}
		if (Request.Url.EndsWith(TEXT("/keyed")))
		{
			return ++*KeyedCalls == 1 ? Json(503) : Json(200);
		}
		return Json(503);
	});
	auto Order = MakeOutcome<FVaRestJson>();
	auto Dropped = MakeOutcome<FVaRestJson>();
	auto Patched = MakeOutcome<FVaRestJson>();
	auto Keyed = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/orders")).Body(TEXT("o")), Order).Send();
	Observe(Rig.Client.Post(TEXT("/drop")).Body(TEXT("o")), Dropped).Send();
	Observe(Rig.Client.Patch(TEXT("/patch")).Body(TEXT("o")), Patched).Send();
	Observe(Rig.Client.Post(TEXT("/keyed")).Body(TEXT("o")).Idempotent(), Keyed).Send();
	if (!TestTrue(TEXT("the three non-idempotent ones finish; the keyed one backs off"),
		PumpUntil([&] { return Order->IsDone() && Dropped->IsDone() && Patched->IsDone() && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("POST 503: sent once"), CallsTo(Rig, TEXT("/orders")), 1);
	TestTrue(TEXT("... its error after one attempt (bRetryable states spec 4.1's class, not what was done)"), Order->Error.Kind == EVaRestErrorKind::Http
		&& Order->Error.HttpStatus == 503 && Order->Error.Attempts == 1 && Order->Error.bRetryable);
	TestEqual(TEXT("POST, connection error: sent once"), CallsTo(Rig, TEXT("/drop")), 1);
	TestEqual(TEXT("PATCH 503: sent once"), CallsTo(Rig, TEXT("/patch")), 1);
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("an Idempotent() POST is retried"), Rig.Wait(Keyed)) || !TestEqual(TEXT("... twice in all"), CallsTo(Rig, TEXT("/keyed")), 2))
	{
		Abandon(Rig);
		return false;
	}
	FString Keys[2];
	int32 Found = 0;
	for (int32 Index = 0; Index < Rig.Fake->NumCalls() && Found < 2; ++Index)
	{
		if (Rig.Fake->GetCall(Index).Request.Url.EndsWith(TEXT("/keyed")))
		{
			Keys[Found++] = Rig.Fake->GetCall(Index).Request.Headers.Get(TEXT("Idempotency-Key"));
		}
	}
	TestTrue(TEXT("... with the same Idempotency-Key on both attempts (spec 3.2)"), !Keys[0].IsEmpty() && Keys[0].Equals(Keys[1], ESearchCase::CaseSensitive));
	TestTrue(TEXT("... and succeeds"), Keyed->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryGivesUpTest, "VaRest.Http.Retry.GivesUp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryGivesUpTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/down")), Outcome).Send();
	for (int32 Step = 0; Step < 3; ++Step)
	{
		if (!TestTrue(FString::Printf(TEXT("backoff %d armed"), Step + 1), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Clock->Advance(2.0);
	}
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("one attempt and three retries"), Rig.Fake->NumCalls(), 4);
	TestTrue(TEXT("the last error, after four attempts"), Outcome->Error.Kind == EVaRestErrorKind::Http && Outcome->Error.HttpStatus == 503 && Outcome->Error.Attempts == 4);
	TestEqual(TEXT("no timer left"), Rig.Clock->NumPendingTimers(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryAfterTraceLogTest, "VaRest.Http.Retry.RetryAfterTraceAndLog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryAfterTraceLogTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3)).WithLogLevel(EVaRestLogLevel::Basic);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 503"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	FVaRestFakeResponse Busy = Json(503);
	Busy.Headers.Set(TEXT("Retry-After"), TEXT("2"));
	Rig.Fake->Enqueue(Busy);
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/busy")), Outcome).Send();
	if (!TestTrue(TEXT("backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.999);
	Rig.Drain();
	TestEqual(TEXT("Retry-After: 2 -- nothing at 1.999 s"), Rig.Fake->NumCalls(), 1);
	Rig.Clock->Advance(0.001);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a success on the second attempt"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	const uint64 Id = Handle.GetId();
	TestEqualSensitive(TEXT("IN-4: a Note between the attempts"), Scoped.Recorder->KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Note,Dispatched,Ended,Completed")));
	TArray<FVaRestTraceAttemptDispatched> Dispatched;
	TArray<FVaRestTraceNote> Notes;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Id))
	{
		if (Event.Kind == EVaRestTraceKind::AttemptDispatched)
		{
			Dispatched.Add(Event.Payload.Get<FVaRestTraceAttemptDispatched>());
		}
		else if (Event.Kind == EVaRestTraceKind::Note)
		{
			Notes.Add(Event.Payload.Get<FVaRestTraceNote>());
		}
	}
	if (!TestTrue(TEXT("two dispatches and one note"), Dispatched.Num() == 2 && Notes.Num() == 1))
	{
		return false;
	}
	TestTrue(TEXT("the note: RetryScheduled, why, how long, what was asked, for attempt 1"), Notes[0].Kind == EVaRestTraceNoteKind::RetryScheduled
		&& Notes[0].Reason == FName(TEXT("Status503")) && Notes[0].Delay == 2.0 && Notes[0].RetryAfter == 2.0 && Notes[0].Attempt == 1);
	TestTrue(TEXT("IN-2: the first attempt has no gap"), Dispatched[0].GapKind == EVaRestTraceGap::None && Dispatched[0].GapSeconds == 0.0);
	TestTrue(TEXT("... the second a 2 s backoff"), Dispatched[1].GapKind == EVaRestTraceGap::Backoff && Dispatched[1].GapSeconds == 2.0 && Dispatched[1].Attempt == 2);
	TestTrue(TEXT("the log says so, with the request id (IN-16)"),
		Capture.Contains(FString::Printf(TEXT("#%llu VaRest: Status503; retry 1 of 3 in 2000 ms (Retry-After)"), Id)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetrySeededTest, "VaRest.Http.Retry.SeededJitter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetrySeededTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decision 26: the client's stream, seeded, gives the delay a test can compute
	const FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(1);
	FRandomStream Expected(42);
	const double Delay = Policy.Backoff.GetDelay(1, Expected);
	FTestRig Rig;
	Rig.Client.WithRetry(Policy).WithRandomSeed(42);
	Rig.Fake->Enqueue(Json(503));
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/j")), Outcome).Send();
	if (!TestTrue(TEXT("backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	if (Delay > 1e-5)
	{
		Rig.Clock->Advance(Delay - 1e-6);
		Rig.Drain();
		TestEqual(TEXT("not before the seeded delay"), Rig.Fake->NumCalls(), 1);
	}
	Rig.Clock->Advance(2e-5);
	TestTrue(TEXT("at it"), Rig.Wait(Outcome) && Rig.Fake->NumCalls() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryNotRetriedTest, "VaRest.Http.Retry.NotRetried",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryNotRetriedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	FTestRig Plain;
	Plain.Fake->Enqueue(Json(503));
	auto NoPolicy = MakeOutcome<FVaRestJson>();
	Observe(Plain.Client.Get(TEXT("/p")), NoPolicy).Send();
	TestTrue(TEXT("plan decision 1: a client without WithRetry sends once"), Plain.Wait(NoPolicy) && Plain.Fake->NumCalls() == 1);

	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/text")))
		{
			return FVaRestFakeResponse::Text(200, TEXT("not json"));
		}
		if (Request.Url.EndsWith(TEXT("/manual")))
		{
			return FVaRestFakeResponse::Manual();
		}
		return Json(404);
	});
	auto Missing = MakeOutcome<FVaRestJson>();
	auto NotJson = MakeOutcome<FVaRestJson>();
	auto Canceled = MakeOutcome<FVaRestJson>();
	auto Opted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/missing")), Missing).Send();
	Observe(Rig.Client.Get(TEXT("/text")), NotJson).Send();
	const FVaRestRequestHandle Pending = Observe(Rig.Client.Get(TEXT("/manual")), Canceled).Send();
	Observe(Rig.Client.Get(TEXT("/opted")).NoRetry(), Opted).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 4; });
	Pending.Cancel();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Missing->IsDone() && NotJson->IsDone() && Canceled->IsDone() && Opted->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("404, a decode error, a cancel and a NoRetry() request: one call each"), Rig.Fake->NumCalls(), 4);
	TestEqual(TEXT("... and no backoff armed"), Rig.Clock->NumPendingTimers(), 0);
	TestTrue(TEXT("the kinds are the attempts' own"), Missing->Error.Kind == EVaRestErrorKind::Http && NotJson->Error.Kind == EVaRestErrorKind::Decode
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled && Opted->Error.HttpStatus == 404);
	return true;
}
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Retry`
Expected: FAIL to compile (`WithRetry`, `NoRetry` unknown).

- [ ] **Step 3: The exchange's retry state** — in `Source/VaRestHttp/Private/VaRestExchangeState.h` add `#include "VaRestRetry.h"`, and in `FExchangeState` after `bool bInLimiterQueue = false;`:

```cpp
		// Retry (Tasks 7-8), set by Resolve:
		/** The request's Retry()/NoRetry(), from Send */
		TOptional<FVaRestRetryPolicy> RetryOverride;
		/** That, else the client's (WithRetry) */
		FVaRestRetryPolicy RetryPolicy = FVaRestRetryPolicy::Disabled();
		/** Spec 4.1: an enabled policy, and an idempotent verb or Idempotent() */
		bool bRetryEligible = false;
		/** Plan decision 8: a later attempt can need the body again, so the transport stage copies it */
		bool bMayReissue = false;
		/** Plan decision 10 (Task 8): clock seconds; 0 = none */
		double DeadlineAt = 0.0;
		int32 RetriesDone = 0;

		/**
		 * Plan decision 7. Game thread. Set by a stage while it holds this exchange waiting (a backoff, a park behind a
		 * refresh); FClientState::Cancel takes it and calls it, and the stage answers Canceled through the chain. The stage
		 * clears it when the wait ends. It holds the stage's state weakly, so it makes no cycle through this exchange.
		 */
		TUniqueFunction<void()> Interrupt;

		/** IN-2: what the next AttemptDispatched reports; the transport stage resets both after the dispatch */
		EVaRestTraceGap NextGap = EVaRestTraceGap::None;
		double NextGapSeconds = 0.0;
```

and after `FExchangeAccess`/`Internals` add the owner helpers, moved from `PipelinePrivate` in `VaRestPipeline.cpp` (delete them there; the pipeline's calls resolve to these):

```cpp
	/** IsValid() is false for an object marked as garbage, not only for one collected (WeakObjectPtr.h:290) */
	inline bool IsOwnerGone(const FExchangeState& Ex)
	{
		return Ex.bHasOwner && !Ex.Owner.IsValid();
	}

	inline void MarkOwnerGone(FExchangeState& Ex)
	{
		// Silence wins over an earlier User cancel (review C-2); Shutdown is silent too, and stays
		if (Ex.CancelReason != EVaRestCancelReason::Shutdown)
		{
			Ex.CancelReason = EVaRestCancelReason::OwnerGone;
		}
		Ex.bCancelRequested = true;
	}
```

- [ ] **Step 4: The builder and the client** — in `Source/VaRestHttp/Public/VaRestClient.h` add `#include "VaRestRetry.h"`; in `FRequestSpec`'s fields after `bool bStrictDecode = false;`:

```cpp
		/** Retry() / NoRetry(); unset: the client's policy */
		TOptional<FVaRestRetryPolicy> Retry;
```

in `TVaRestRequest`, after `Priority(...)`:

```cpp
	/**
	 * Spec 4.1: this request's retry policy, replacing the client's (WithRetry). A request is retried only when it is
	 * idempotent: GET, HEAD, OPTIONS, PUT or DELETE, or marked Idempotent(). A retry is a new attempt through the limiter,
	 * behind whoever waits there (spec 4.6), on the request's clock; Cancel() and the owner's destruction stop it while it
	 * waits.
	 */
	TVaRestRequest Retry(const FVaRestRetryPolicy& InPolicy) &&
	{
		Spec->Retry = InPolicy;
		return MoveTemp(*this);
	}

	/** No retry for this request, whatever the client's policy */
	TVaRestRequest NoRetry() &&
	{
		Spec->Retry = FVaRestRetryPolicy::Disabled();
		return MoveTemp(*this);
	}
```

and in `FVaRestClient`'s configuration section:

```cpp
	/**
	 * Spec 4.1: the retry policy of this client's requests (a request's Retry()/NoRetry() replaces it). Without one a client
	 * does not retry (plan decision 1). Game thread; requests already sent keep the policy they started with.
	 */
	FVaRestClient& WithRetry(const FVaRestRetryPolicy& Policy);
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithRetry(const FVaRestRetryPolicy& Policy)
{
	if (State.IsValid())
	{
		State->EditConfig([&Policy](VaRest::Http::FClientConfig& Edited) { Edited.Retry = Policy; });
	}
	return *this;
}
```

and in `FRequestSpecAccess::Send`, after `Ex.bStrictDecode = Spec->bStrictDecode;`: `Ex.RetryOverride = MoveTemp(Spec->Retry);`.

In `Source/VaRestHttp/Private/VaRestClientState.h`, `FClientConfig`, after `MaxJsonBytes`: `FVaRestRetryPolicy Retry = FVaRestRetryPolicy::Disabled();` (add `#include "VaRestRetry.h"`; `VaRestRetryInternal.h` for `MakeRetryStage` in the .cpp).

In `Source/VaRestHttp/Private/VaRestClientState.cpp`, `Publish`, between the custom Outer loop and `NewChain->Add(MakeAttemptBoundaryStage());`:

```cpp
	// Spec 4.1 and plan decision 2: retry is the first built-in Outer stage (M4b puts offline queue and cache in front of it)
	NewChain->Add(MakeRetryStage());
```

(and `#include "VaRestRetryInternal.h"`; the comment above the loop already names the built-in Outer stages).

- [ ] **Step 5: The pipeline** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`#include "VaRestRetryInternal.h"`):

In `Resolve`, before `Exchange.Timeout = ...`:

```cpp
	// Spec 4.1, plan decisions 1 and 8
	Exchange.RetryPolicy = Exchange.RetryOverride.IsSet() ? Exchange.RetryOverride.GetValue() : Cfg.Retry;
	Exchange.bRetryEligible = Exchange.RetryPolicy.IsEnabled() && (Exchange.bIdempotent || IsIdempotentVerb(Exchange.Verb));
	Exchange.bMayReissue = Cfg.bMayReissue || Exchange.bRetryEligible;
```

In `FTransportStage::Process`, `else if (Cfg.bMayReissue)` becomes `else if (Ex.bMayReissue)` (comment: `// A later attempt needs the body again (decision 12, plan decision 8)`), and right after `TracePoints::AttemptDispatched(Ex);`:

```cpp
				// The gap was this attempt's; the next one reports its own (IN-2)
				Ex.NextGap = EVaRestTraceGap::None;
				Ex.NextGapSeconds = 0.0;
```

In `FClientState::Cancel`, between the `if (Ex.TransportCall.IsValid()) { ... return; }` block and the final `Complete(Exchange, MakeFinish(Ex));`:

```cpp
	if (Ex.Interrupt)
	{
		// Plan decision 7: a stage holds it waiting (a backoff, a park behind a refresh); it answers Canceled through the
		// chain, so every Outer stage gets its one outcome. Taken first: answering may let a stage set another.
		TUniqueFunction<void()> Wake = MoveTemp(Ex.Interrupt);
		Ex.Interrupt.Reset();
		Wake();
		return;
	}
```

In `FClientState::Complete`, right after `Ex.Phase = EVaRestExchangePhase::Done;`: `Ex.Interrupt.Reset(); // nothing waits for a finished request`.

In `Source/VaRestHttp/Private/VaRestTracePoints.cpp`, `AttemptDispatched`, after `Payload.LimiterWait = Ex.LimiterWait;`:

```cpp
	Payload.GapKind = Ex.NextGap;
	Payload.GapSeconds = Ex.NextGapSeconds;
```

- [ ] **Step 6: The stage** — append to `Source/VaRestHttp/Private/VaRestRetry.cpp` (`#include "VaRestClientState.h"`, `#include "VaRestExchangeState.h"`, `#include "VaRestHttpLog.h"`, `#include "VaRestTracePoints.h"`):

```cpp
namespace VaRest::Http::RetryPrivate
{
	/** One exchange's way through the retry stage: shared by its attempt continuations and its backoff timer */
	struct FRun : public TSharedFromThis<FRun, ESPMode::ThreadSafe>
	{
		FRun(const FVaRestExchangeRef& InExchange, const FVaRestStageNext& InNext, FVaRestOutcomeCallback&& InDone)
			: Exchange(InExchange)
			, Next(InNext)
			, Done(MoveTemp(InDone))
		{
		}

		/** The stage's one outcome (IVaRestStage::Process): a second call is ignored */
		void Answer(FVaRestAttemptOutcome&& Outcome)
		{
			if (!Done)
			{
				return;
			}
			FVaRestOutcomeCallback Last = MoveTemp(Done);
			Done.Reset();
			Last(MoveTemp(Outcome));
		}

		FVaRestExchangeRef Exchange;
		FVaRestStageNext Next;
		FVaRestOutcomeCallback Done;
		FVaRestTimerHandle Timer;
	};

	using FRunRef = TSharedRef<FRun, ESPMode::ThreadSafe>;

	void LogRetry(const FExchangeState& Ex, const FRetryDecision& Decision)
	{
		if (Ex.LogLevel == EVaRestLogLevel::None || !UE_LOG_ACTIVE(LogVaRestHttp, Log))
		{
			return;
		}
		// IN-16: the request id; the reason is VaRest's own FName, no request text in it
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu VaRest: %s; retry %d of %d in %.0f ms%s"), Ex.RequestId, *Decision.Reason.ToString(), Ex.RetriesDone,
			Ex.RetryPolicy.MaxRetries, Decision.Delay * 1000.0, Decision.RetryAfter >= 0.0 ? TEXT(" (Retry-After)") : TEXT(""));
	}

	void StartAttempt(const FRunRef& Run);

	void OnAttempt(const FRunRef& Run, FVaRestAttemptOutcome&& Outcome)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		// Spec 3.2: "owner gone -> no callbacks, no further retries"; a canceled or finished request is not retried either
		if (Ex.bFinished || Run->Exchange->IsCancelRequested() || IsOwnerGone(Ex))
		{
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		const FRetryDecision Decision = DecideRetry(Ex.RetryPolicy, Outcome, Ex.RetriesDone, Ex.Config->Clock->Now(), Ex.DeadlineAt, FDateTime::UtcNow(),
			Ex.State->GetRandom());
		if (!Decision.bRetry)
		{
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		++Ex.RetriesDone;
		Ex.NextGap = EVaRestTraceGap::Backoff;
		Ex.NextGapSeconds = Decision.Delay;
		LogRetry(Ex, Decision);
		FVaRestTraceNote Note;
		Note.Kind = EVaRestTraceNoteKind::RetryScheduled;
		Note.Reason = Decision.Reason;
		Note.Delay = Decision.Delay;
		Note.RetryAfter = Decision.RetryAfter;
		TracePoints::Note(Ex, MoveTemp(Note));
		// This attempt's outcome ends here: its body is freed with it; a SaveTo temp file of it is removed by the next
		// transport start (FTransportStage), or by the decode task when the wait is canceled
		const TWeakPtr<FRun, ESPMode::ThreadSafe> Weak = Run;
		Ex.Interrupt = [Weak]
		{
			if (const TSharedPtr<FRun, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Internals(*Pinned->Exchange).Config->Clock->ClearTimer(Pinned->Timer);
				Pinned->Answer(FVaRestAttemptOutcome::Canceled());
			}
		};
		Run->Timer = Ex.Config->Clock->SetTimer(Decision.Delay, [Run]
		{
			FExchangeState& Waited = Internals(*Run->Exchange);
			Waited.Interrupt.Reset();
			if (IsOwnerGone(Waited))
			{
				MarkOwnerGone(Waited);
				Run->Answer(FVaRestAttemptOutcome::Canceled());
				return;
			}
			StartAttempt(Run);
		});
	}

	void StartAttempt(const FRunRef& Run)
	{
		Run->Next.Proceed(Run->Exchange, [Run](FVaRestAttemptOutcome&& Outcome) { OnAttempt(Run, MoveTemp(Outcome)); });
	}

	/** Spec 4.1. Outer: it sees each logical request once and re-issues it; the attempt boundary after it numbers the attempts. */
	class FRetryStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Retry"); }

		/** True, but not read: a built-in stage decides per exchange (FExchangeState::bMayReissue, plan decision 8) */
		virtual bool MayReissue() const override { return true; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (!Internals(*Exchange).bRetryEligible)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			StartAttempt(MakeShared<FRun, ESPMode::ThreadSafe>(Exchange, Next, MoveTemp(OnOutcome)));
		}
	};
}

FVaRestStageRef VaRest::Http::MakeRetryStage()
{
	return MakeShared<RetryPrivate::FRetryStage, ESPMode::ThreadSafe>();
}
```

- [ ] **Step 7: Run them, then the pipeline, cancel and limiter suites**

Run: `Tools/run_tests.sh VaRest.Http.Retry` then `Tools/run_tests.sh --no-build VaRest.Http.Pipeline` then `Tools/run_tests.sh --no-build VaRest.Http.Cancel` then `Tools/run_tests.sh --no-build VaRest.Http.Limiter`
Expected: `0 failed` each.

- [ ] **Step 8: Restore-the-bug** — in `Resolve` set `bRetryEligible = Exchange.RetryPolicy.IsEnabled()` (no verb check): `VaRest.Http.Retry.NonIdempotentSentOnce` FAILS ("POST 503: sent once"); restore; PASS. Drop the `Ex.NextGap = EVaRestTraceGap::Backoff;` line: `RetryAfterTraceAndLog` FAILS ("the second a 2 s backoff"); restore; PASS.

- [ ] **Step 9: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp \
  Source/VaRestHttp/Private/VaRestRetry.cpp Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp
git commit -m "http: M4 task 7 -- the retry stage: eligible requests re-issued after backoff or Retry-After, interruptible waits, trace notes and gaps

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 8: Retry against the rest of the pipeline — the deadline, the limiter queue, timeouts, stops while waiting, bodies sent again (spec §4.1, §4.6, §4.7; carries (c), (e))

Plan decision 10 (the deadline) is implemented here; the rest is the interplay M3's lessons list, each with a test that fails when the interplay breaks.

**Files:**
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Resolve` sets `DeadlineAt`; the transport stage cuts the timeout and refuses a late attempt)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp`

**Interfaces:**
- Consumes: Task 7 (`FExchangeState::DeadlineAt`, `Interrupt`, the stage).
- Produces: failure detail `Deadline` (`Kind=Timeout`, not retryable) from the transport stage.

- [ ] **Step 1: Write the failing tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp` (add `#include "HAL/FileManager.h"`, `#include "Misc/FileHelper.h"`, `#include "Misc/Guid.h"`, `#include "Misc/Paths.h"`, `#include "Misc/ScopeExit.h"`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryDeadlineTest, "VaRest.Http.Retry.Deadline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryDeadlineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decision 10: t=0 attempt 1 (503), wait 1 s; t=1 attempt 2 (503), a 2 s wait would end at 3 >= 2.5: no retry
	FTestRig Rig;
	FVaRestRetryPolicy Policy = Exact(5).WithDeadline(2.5f);
	Policy.Backoff.BaseDelay = 1.f;
	Rig.Client.WithRetry(Policy).WithTimeout(10.f);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/d")), Outcome).Send();
	if (!TestTrue(TEXT("first backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("two attempts: the third would start past the deadline"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("the last answer is the error"), Outcome->Error.HttpStatus == 503 && Outcome->Error.Attempts == 2);
	TestTrue(TEXT("each attempt's total timeout is cut to the time left: 2.5 s, then 1.5 s"),
		FMath::IsNearlyEqual(Rig.Fake->GetCall(0).Request.Timeout, 2.5f, 1e-4f) && FMath::IsNearlyEqual(Rig.Fake->GetCall(1).Request.Timeout, 1.5f, 1e-4f));

	// Waiting in the limiter counts (spec 4.7 "across attempts" from the request's start)
	FTestRig Queue;
	Queue.Client.WithMaxConcurrent(1).WithRetry(Exact(3).WithDeadline(1.f));
	Queue.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(200); });
	auto Hold = MakeOutcome<FVaRestJson>();
	auto Late = MakeOutcome<FVaRestJson>();
	Observe(Queue.Client.Get(TEXT("/hold")).NoRetry(), Hold).Send();
	Observe(Queue.Client.Get(TEXT("/late")), Late).Send();
	if (!TestTrue(TEXT("one holds the slot"), PumpUntil([&Queue] { return Queue.Fake->NumCalls() == 1; })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Clock->Advance(1.5);
	Queue.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Hold, Late] { return Hold->IsDone() && Late->IsDone(); })))
	{
		Abandon(Queue);
		return false;
	}
	Queue.Drain();
	TestEqual(TEXT("the late one never reached the transport"), Queue.Fake->NumCalls(), 1);
	TestTrue(TEXT("... it is a Timeout, not retryable, saying why"), Late->Error.Kind == EVaRestErrorKind::Timeout && !Late->Error.bRetryable
		&& Late->Error.Message.Contains(TEXT("Deadline")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryQueueTest, "VaRest.Http.Retry.QueuesBehindWaiting",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryQueueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Carry (c), spec 4.6: "queued retries wait their turn too". A's retry has no delay; B and C wait in the limiter
	// when A fails, so the retry goes behind them: a, b, c, a.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	FVaRestRetryPolicy Instant = Exact(3);
	Instant.Backoff.BaseDelay = 0.f;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")).Retry(Instant), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	if (!TestTrue(TEXT("A holds the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(503));
	if (!TestTrue(TEXT("B gets the slot A freed; A's retry is armed"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.0);
	Rig.Drain();
	TestEqual(TEXT("the retry waits behind C"), Rig.Fake->NumCalls(), 2);
	for (int32 Index = 1; Index < 4; ++Index)
	{
		Rig.Fake->Complete(Index, Json(200));
		if (Index < 3 && !TestTrue(FString::Printf(TEXT("call %d"), Index + 2), PumpUntil([&Rig, Index] { return Rig.Fake->NumCalls() == Index + 2; })))
		{
			Abandon(Rig);
			return false;
		}
	}
	if (!TestTrue(TEXT("all complete"), PumpUntil([A, B, C] { return A->IsDone() && B->IsDone() && C->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TArray<FString> Order;
	for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
	{
		Order.Add(Rig.Fake->GetCall(Index).Request.Url.RightChop(FString(TEXT("https://api.test/")).Len()));
	}
	TestEqualSensitive(TEXT("dispatch order"), FString::Join(Order, TEXT(",")), FString(TEXT("a,b,c,a")));
	TestTrue(TEXT("A succeeds on its retry"), A->SuccessCalls.load() == 1 && A->Response.Attempts == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryTimeoutTest, "VaRest.Http.Retry.TimeoutPerAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Spec 4.7: the timeout is per attempt; without a deadline the request lasts 1 + 0.5 + 1 + 1 + 1 = 4.5 s
	FTestRig Rig;
	Rig.Client.WithTimeout(1.f).WithRetry(Exact(2));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/slow")), Outcome).Send();
	const double Waits[] = { 0.5, 1.0 };
	for (int32 Step = 0; Step < 3; ++Step)
	{
		if (!TestTrue(FString::Printf(TEXT("attempt %d starts"), Step + 1), PumpUntil([&Rig, Step] { return Rig.Fake->NumCalls() == Step + 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Clock->Advance(1.0);
		if (Step < 2)
		{
			if (!TestTrue(FString::Printf(TEXT("attempt %d timed out; backoff armed"), Step + 1), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
			{
				Abandon(Rig);
				return false;
			}
			Rig.Clock->Advance(Waits[Step]);
		}
	}
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a Timeout after three attempts"), Outcome->Error.Kind == EVaRestErrorKind::Timeout && Outcome->Error.Attempts == 3);
	bool bEachOne = true;
	for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
	{
		bEachOne &= Rig.Fake->GetCall(Index).Request.Timeout == 1.f;
	}
	TestTrue(TEXT("every attempt has the whole per-attempt timeout"), bEachOne);
	TestTrue(TEXT("elapsed: three timeouts and two waits"), FMath::IsNearlyEqual(Outcome->Response.Elapsed, 4.5, 1e-6));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryStopTest, "VaRest.Http.Retry.StopWhileBackingOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decision 7: Cancel(), shutdown and the owner's destruction reach a request that waits on a backoff timer
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return Json(503); });
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto Canceled = MakeOutcome<FVaRestJson>();
	auto Owned = MakeOutcome<FVaRestJson>();
	auto ShutDown = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/c")), Canceled).Send();
	Observe(Rig.Client.Get(TEXT("/o")).Owner(Owner), Owned).Send();
	Observe(Rig.Client.Get(TEXT("/s")).Tag(TEXT("Exit")), ShutDown).Send();
	if (!TestTrue(TEXT("all three back off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3 && Rig.Clock->NumPendingTimers() == 3; })))
	{
		Abandon(Rig);
		return false;
	}

	Handle.Cancel();
	if (!TestTrue(TEXT("Cancel() ends the wait"), PumpUntil([Canceled] { return Canceled->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... one OnError(Canceled), one OnComplete"), Canceled->ErrorCalls.load() == 1 && Canceled->CompleteCalls.load() == 1
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... its timer is gone"), Rig.Clock->NumPendingTimers(), 2);

	VaRest::Http::FClientAccess::State(Rig.Client)->CancelAll(TEXT("Exit"), EVaRestCancelReason::Shutdown);
	Rig.Drain();
	TestEqual(TEXT("shutdown: its timer is gone too"), Rig.Clock->NumPendingTimers(), 1);

	Owner->MarkAsGarbage();
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("the owner's request ends when its timer fires"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("no attempt after any stop"), Rig.Fake->NumCalls(), 3);
	TestEqual(TEXT("shutdown and owner gone: no callbacks (spec 3.2, decision 2)"), ShutDown->CompleteCalls.load() + ShutDown->ErrorCalls.load()
		+ Owned->CompleteCalls.load() + Owned->ErrorCalls.load(), 0);
	TestEqual(TEXT("nothing left armed"), Rig.Clock->NumPendingTimers(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRetryBodiesTest, "VaRest.Http.Retry.BodiesResent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRetryBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::RetryTestPrivate;
	// Plan decisions 8 and 24 (carry (e)): every re-issue is a new transport request with the whole body -- a JSON body,
	// a multipart body with a file part, a BodyFile -- and a SaveTo keeps only the final attempt's file
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestRetryTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
	TArray<uint8> Pattern;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Pattern.Add(static_cast<uint8>(Index * 7 + 3));
	}
	const FString PartFile = Dir / TEXT("part.bin");
	const FString UploadFile = Dir / TEXT("upload.bin");
	const FString Destination = Dir / TEXT("download.bin");
	if (!TestTrue(TEXT("fixtures written"), FFileHelper::SaveArrayToFile(Pattern, *PartFile) && FFileHelper::SaveArrayToFile(Pattern, *UploadFile)))
	{
		return false;
	}
	FTestRig Rig;
	Rig.Client.WithRetry(Exact(1));
	TSharedRef<TMap<FString, int32>, ESPMode::ThreadSafe> Seen = MakeShared<TMap<FString, int32>, ESPMode::ThreadSafe>();
	Rig.Fake->SetResponder([Seen](const FVaRestTransportRequest& Request)
	{
		const bool bFirst = ++Seen->FindOrAdd(Request.Url) == 1;
		if (Request.Url.EndsWith(TEXT("/download")))
		{
			return bFirst ? FVaRestFakeResponse::Text(503, TEXT("busy")) : FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 'o', 'k' });
		}
		return bFirst ? Json(503) : Json(200);
	});
	auto Json1 = MakeOutcome<FVaRestJson>();
	auto Parts = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	auto Saved = MakeOutcome<void>();
	Observe(Rig.Client.Put(TEXT("/json")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), Json1).Send();
	Observe(Rig.Client.Put(TEXT("/multipart")).Body(FVaRestMultipart().AddField(TEXT("f"), TEXT("v")).AddFile(TEXT("file"), PartFile)), Parts).Send();
	Observe(Rig.Client.Put(TEXT("/file")).BodyFile(UploadFile), File).Send();
	Observe(Rig.Client.Get<void>(TEXT("/download")).SaveTo(Destination), Saved).Send();
	if (!TestTrue(TEXT("all four back off"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 4; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Json1->IsDone() && Parts->IsDone() && File->IsDone() && Saved->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	if (!TestEqual(TEXT("two calls each"), Rig.Fake->NumCalls(), 8))
	{
		return false;
	}
	for (const TCHAR* Path : { TEXT("/json"), TEXT("/multipart"), TEXT("/file") })
	{
		TArray<int32> Calls;
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			if (Rig.Fake->GetCall(Index).Request.Url.EndsWith(Path))
			{
				Calls.Add(Index);
			}
		}
		if (!TestEqual(FString::Printf(TEXT("%s: two attempts"), Path), Calls.Num(), 2))
		{
			continue;
		}
		const TArray<uint8> First = Rig.Fake->ReadBody(Calls[0]);
		TestTrue(FString::Printf(TEXT("%s: the retry sends the whole body again"), Path), First.Num() > 0 && First == Rig.Fake->ReadBody(Calls[1]));
	}
	TArray<uint8> Downloaded;
	TestTrue(TEXT("SaveTo: the final attempt's body"), Saved->SuccessCalls.load() == 1 && FFileHelper::LoadFileToArray(Downloaded, *Destination)
		&& Downloaded == TArray<uint8>{ 'o', 'k' });
	const auto Leftovers = [&Dir]
	{
		TArray<FString> Parts;
		IFileManager::Get().FindFiles(Parts, *(Dir / TEXT("*.part")), true, false);
		return Parts.Num();
	};
	// The first attempt's temp file is deleted by a UE::Tasks task (FTransportStage): give it the time it needs
	TestTrue(TEXT("... and no attempt's temp file is left"), PumpUntil([&Leftovers] { return Leftovers() == 0; }));
	return true;
}
```

- [ ] **Step 2: Run them to see them fail**

Run: `Tools/run_tests.sh VaRest.Http.Retry`
Expected: `VaRest.Http.Retry.Deadline` FAILS (with no deadline the second 503 arms another backoff, so "completes" times out); the others PASS already (Task 7 built the behaviour; Step 5 shows each of them failing with it broken).

- [ ] **Step 3: The deadline** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`, `Resolve`, after the three retry lines Task 7 added:

```cpp
	// Plan decision 10: an absolute time on the request's clock; Enter stamped StartedAt before Resolve
	const float Deadline = Exchange.RetryPolicy.Deadline;
	Exchange.DeadlineAt = FMath::IsFinite(Deadline) && Deadline > 0.f ? Exchange.StartedAt + Deadline : 0.0;
```

In `FTransportStage::Process`, after the `FindBadHeader` block and before `if (!Ex.SaveTo.IsEmpty())`:

```cpp
			double AttemptTimeout = Ex.Timeout;
			if (Ex.DeadlineAt > 0.0)
			{
				// Plan decision 10 (spec 4.7, "plus the retry Deadline across attempts"): the time left bounds this attempt,
				// and with none left it never starts -- a long wait in the limiter's queue counts
				const double Left = Ex.DeadlineAt - Cfg.Clock->Now();
				if (Left <= 0.0)
				{
					FVaRestAttemptOutcome Late;
					Late.Failure = EVaRestTransportFailure::Timeout;
					Late.FailureDetail = TEXT("Deadline");
					Late.FailureMessage = TEXT("the retry deadline passed before this attempt could start");
					OnOutcome(MoveTemp(Late));
					return;
				}
				AttemptTimeout = AttemptTimeout > 0.0 ? FMath::Min(AttemptTimeout, Left) : Left;
			}
```

and `Request.Timeout = Ex.Timeout;` becomes `Request.Timeout = static_cast<float>(AttemptTimeout);`.

- [ ] **Step 4: Run them**

Run: `Tools/run_tests.sh VaRest.Http.Retry`
Expected: `0 failed`.

- [ ] **Step 5: Restore-the-bug** — (a) delete Task 7's `if (Ex.Interrupt) { ... }` block in `FClientState::Cancel`: `StopWhileBackingOff` FAILS ("its timer is gone": the cancel finishes the request but leaves the backoff armed); restore; PASS. (b) In `FLimiter::Acquire` queue at the front (`Queues[Level].Insert(MoveTemp(Entry), 0)` with the entry built first): `QueuesBehindWaiting` FAILS (order `a,c,b,a` or `a,b,a,c`); restore; PASS. (c) In the transport stage copy only when `Cfg.bMayReissue` (Task 7's per-exchange flag reverted): `BodiesResent` FAILS (the retry sends an empty body); restore; PASS. (d) Remove the `Left <= 0.0` refusal: `Deadline` FAILS ("the late one never reached the transport"); restore; PASS. Record all four.

- [ ] **Step 6: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/Tests/VaRestHttpRetryTest.cpp
git commit -m "http: M4 task 8 -- the retry deadline; retry against the limiter queue, per-attempt timeouts, stops while waiting, bodies sent again

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 9: The envelope and its public evaluator (spec §3.4; WebSocket §19 item 6)

`FVaRestEnvelope` is configuration (M5's asset holds one per API and per endpoint) and a pure function from a parsed body to data or a `Kind=Server` error, which the socket codec also calls on its messages.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestEnvelope.h`, `Source/VaRestHttp/Private/VaRestEnvelope.cpp`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpEnvelopeTest.cpp`

**Interfaces:**
- Consumes: VaRestJson's `FVaRestJson::Path`, `IsString`, `AsString`, `ToString`.
- Produces: `USTRUCT(BlueprintType) struct FVaRestEnvelope { FString DataPath; FString SuccessPath; TArray<FString> SuccessValues; FString ErrorMessagePath; FString ErrorCodePath; bool bParseErrorBodies = false; static FVaRestEnvelope DataAt(FStringView Path); FVaRestEnvelope SuccessWhen(FStringView Path, TArray<FString> Values) const; FVaRestEnvelope ErrorMessageAt(FStringView Path) const; FVaRestEnvelope ErrorCodeAt(FStringView Path) const; FVaRestEnvelope ParseErrorBodies(bool bParse = true) const; bool HasPredicate() const; FVaRestEnvelopeResult Evaluate(const FVaRestJson& Body) const; void ReadErrorFields(const FVaRestJson& Body, FString& OutCode, FString& OutMessage) const; static FString ValueText(const FVaRestJson& Value); }`; `struct FVaRestEnvelopeResult { bool bSuccess = true; FVaRestJson Data; bool bDataFound = true; FString Code, Message, FailedPath, Expected, Actual; FVaRestError ToError(const FVaRestJson& Body, int32 HttpStatus) const; }`.

- [ ] **Step 1: Write the failing test** — create `Source/VaRestHttp/Private/Tests/VaRestHttpEnvelopeTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::EnvelopeTestPrivate
{
	FVaRestJson Parse(const TCHAR* Text)
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(FStringView(Text));
		return Parsed.HasValue() ? Parsed.StealValue() : FVaRestJson();
	}

	FVaRestFakeResponse Json(int32 Code, const char* Text)
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** {"ok": true, "data": ...} with {"code", "message"} on failure: the test server's /envelope routes */
	FVaRestEnvelope OkData()
	{
		return FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeEvaluateTest, "VaRest.Http.Envelope.Evaluate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeEvaluateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::EnvelopeTestPrivate;
	const FVaRestEnvelope Envelope = OkData();

	const FVaRestEnvelopeResult Ok = Envelope.Evaluate(Parse(TEXT(R"({"ok":true,"data":{"id":7}})")));
	TestTrue(TEXT("a success: the data at DataPath"), Ok.bSuccess && Ok.bDataFound && Ok.Data.Get(TEXT("id")).AsInt() == 7);

	const FVaRestEnvelopeResult No = Envelope.Evaluate(Parse(TEXT(R"({"ok":false,"code":"E42","message":"the envelope says no"})")));
	TestTrue(TEXT("a failed predicate"), !No.bSuccess && !No.Data.IsValid());
	TestEqualSensitive(TEXT("... its code"), No.Code, FString(TEXT("E42")));
	TestEqualSensitive(TEXT("... its message"), No.Message, FString(TEXT("the envelope says no")));
	TestEqualSensitive(TEXT("... what failed"), No.FailedPath + TEXT("=") + No.Actual + TEXT(" not ") + No.Expected, FString(TEXT("ok=false not true")));

	const FVaRestError Error = No.ToError(Parse(TEXT(R"({"ok":false})")), 200);
	TestTrue(TEXT("ToError: Kind Server, the 2xx status, not retryable, the body kept"), Error.Kind == EVaRestErrorKind::Server && Error.HttpStatus == 200
		&& !Error.bRetryable && Error.Body.IsValid() && Error.Code == TEXT("E42"));
	const FVaRestEnvelopeResult Bare = FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).Evaluate(Parse(TEXT(R"({"ok":false})")));
	TestTrue(TEXT("ToError without a message path: the fallback names the predicate"),
		Bare.ToError(FVaRestJson(), 200).Message.Contains(TEXT("ok is false, not true")));

	// dawg-style: a numeric code; compared as text, so 200 and "200" both pass
	const FVaRestEnvelope Dawg = FVaRestEnvelope::DataAt(TEXT("Data")).SuccessWhen(TEXT("Code"), { TEXT("200") });
	TestTrue(TEXT("Code 200"), Dawg.Evaluate(Parse(TEXT(R"({"Code":200,"Data":1})"))).bSuccess);
	TestTrue(TEXT("Code \"200\""), Dawg.Evaluate(Parse(TEXT(R"({"Code":"200","Data":1})"))).bSuccess);
	TestFalse(TEXT("Code 500"), Dawg.Evaluate(Parse(TEXT(R"({"Code":500})"))).bSuccess);

	// Review Focus: paths that miss
	const FVaRestEnvelopeResult NoData = Envelope.Evaluate(Parse(TEXT(R"({"ok":true})")));
	TestTrue(TEXT("no data: still a success, the data missing"), NoData.bSuccess && !NoData.bDataFound && !NoData.Data.IsValid());
	const FVaRestEnvelopeResult NoOk = Envelope.Evaluate(Parse(TEXT(R"({"data":1})")));
	TestTrue(TEXT("no predicate value: a failure, the value '(missing)'"), !NoOk.bSuccess && NoOk.Actual == TEXT("(missing)"));
	TestFalse(TEXT("an array body: the predicate misses"), Envelope.Evaluate(Parse(TEXT("[1,2]"))).bSuccess);
	TestFalse(TEXT("ok = 0"), Envelope.Evaluate(Parse(TEXT(R"({"ok":0})"))).bSuccess);
	TestFalse(TEXT("ok = \"false\""), Envelope.Evaluate(Parse(TEXT(R"({"ok":"false"})"))).bSuccess);
	TestFalse(TEXT("an invalid handle"), Envelope.Evaluate(FVaRestJson()).bSuccess);
	const FVaRestEnvelopeResult Plain = FVaRestEnvelope().Evaluate(Parse(TEXT("[1,2]")));
	TestTrue(TEXT("an empty envelope: the whole body"), Plain.bSuccess && Plain.Data.Num() == 2);

	// Path syntax is FVaRestJson::Path's: dotted with brackets, or a JSON pointer
	TestEqual(TEXT("dotted with brackets"), FVaRestEnvelope::DataAt(TEXT("result.items[1]")).Evaluate(Parse(TEXT(R"({"result":{"items":[5,6]}})"))).Data.AsInt(), 6);
	TestEqual(TEXT("a JSON pointer"), FVaRestEnvelope::DataAt(TEXT("/a.b/c")).Evaluate(Parse(TEXT(R"({"a.b":{"c":3}})"))).Data.AsInt(), 3);

	FString Code;
	FString Message;
	FVaRestEnvelope::DataAt(TEXT("")).ErrorCodeAt(TEXT("error.code")).ErrorMessageAt(TEXT("error.message")).ReadErrorFields(
		Parse(TEXT(R"({"error":{"code":42,"message":"gone"}})")), Code, Message);
	TestEqualSensitive(TEXT("ReadErrorFields: a numeric code as text"), Code, FString(TEXT("42")));
	TestEqualSensitive(TEXT("... the message"), Message, FString(TEXT("gone")));
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Envelope`
Expected: FAIL to compile (`VaRestEnvelope.h` not found).

- [ ] **Step 3: The header** — create `Source/VaRestHttp/Public/VaRestEnvelope.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"
#include "VaRestJson.h"

#include "VaRestEnvelope.generated.h"

/** What FVaRestEnvelope::Evaluate found (spec 3.4; WebSocket spec 19 item 6) */
struct VARESTHTTP_API FVaRestEnvelopeResult
{
	/** The predicate held, or there is none */
	bool bSuccess = true;
	/** DataPath's value (the whole body when DataPath is empty); an invalid handle when it is missing or the predicate failed */
	FVaRestJson Data;
	/** False when DataPath names nothing in the body */
	bool bDataFound = true;
	/** A failure's ErrorCodePath and ErrorMessagePath values, as text; empty when absent */
	FString Code;
	FString Message;
	/** A failed predicate: its path, the values that pass joined with '|', and the value found ("(missing)" for none; at most 256 characters) */
	FString FailedPath;
	FString Expected;
	FString Actual;

	/** Kind=Server, not retryable: Code, Message (else a fallback naming the predicate), HttpStatus, and Body as the error body */
	FVaRestError ToError(const FVaRestJson& Body, int32 HttpStatus) const;
};

/**
 * Spec 3.4: where a response's data and its success flag are. Per client (WithEnvelope), replaced per request (Envelope(),
 * NoEnvelope()); M5's asset holds one per API and per endpoint, so it is BlueprintType and its field names freeze then
 * (plan decision 27). Applied to FVaRestJson, struct and void results (plan decision 9); raw results get the body as it came.
 * Paths are FVaRestJson::Path's: "data", "result.items[0]", or a JSON pointer ("/a.b/c"). Values compare as text: a
 * string by its content, anything else by its condensed JSON, so SuccessValues {"200"} accepts 200 and "200", and {"true"}
 * accepts true and "true".
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestEnvelope
{
	GENERATED_BODY()

	/** Where the result is; empty = the whole body */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString DataPath;

	/** A value that says success ("ok", "Code"); empty = no predicate: every 2xx is a success */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString SuccessPath;

	/** SuccessPath's value must be one of these (as text: "true", "200", "OK"); a 2xx whose value is not is Kind=Server */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<FString> SuccessValues;

	/** FVaRestError::Message of a Kind=Server failure, and of a 4xx/5xx with bParseErrorBodies */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ErrorMessagePath;

	/** FVaRestError::Code, the same way */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ErrorCodePath;

	/** Read ErrorMessagePath and ErrorCodePath from 4xx and 5xx JSON bodies too (spec 3.4) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bParseErrorBodies = false;

	static FVaRestEnvelope DataAt(FStringView Path);
	FVaRestEnvelope SuccessWhen(FStringView Path, TArray<FString> Values) const;
	FVaRestEnvelope ErrorMessageAt(FStringView Path) const;
	FVaRestEnvelope ErrorCodeAt(FStringView Path) const;
	FVaRestEnvelope ParseErrorBodies(bool bParse = true) const;

	bool HasPredicate() const { return !SuccessPath.IsEmpty(); }

	/** WebSocket spec 19 item 6: Body through this envelope. Any thread: it only reads Body (spec 2.2's thread contract). */
	FVaRestEnvelopeResult Evaluate(const FVaRestJson& Body) const;

	/** ErrorCodePath's and ErrorMessagePath's values in Body, as text; empty when absent */
	void ReadErrorFields(const FVaRestJson& Body, FString& OutCode, FString& OutMessage) const;

	/** A value as the envelope compares it: a string as itself, anything else as condensed JSON; "" for an invalid handle */
	static FString ValueText(const FVaRestJson& Value);
};
```

- [ ] **Step 4: The evaluator** — create `Source/VaRestHttp/Private/VaRestEnvelope.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestEnvelope.h"

namespace VaRest::Http::EnvelopePrivate
{
	/** A server's value in a message or a trace field: bounded, so a whole object in "ok" does not become a page of text */
	constexpr int32 MaxShownChars = 256;

	FString Shown(FString Text)
	{
		return Text.Len() > MaxShownChars ? Text.Left(MaxShownChars) + TEXT("...") : MoveTemp(Text);
	}
}

FVaRestError FVaRestEnvelopeResult::ToError(const FVaRestJson& Body, int32 HttpStatus) const
{
	FVaRestError Error;
	Error.Kind = EVaRestErrorKind::Server;
	Error.HttpStatus = HttpStatus;
	Error.Code = Code;
	Error.Message = !Message.IsEmpty() ? Message
		: FString::Printf(TEXT("%s (%s is %s, not %s)"), *FVaRestError::DefaultMessage(EVaRestErrorKind::Server, HttpStatus), *FailedPath, *Actual, *Expected);
	Error.Body = Body;
	Error.bRetryable = false;
	return Error;
}

FVaRestEnvelope FVaRestEnvelope::DataAt(FStringView Path)
{
	FVaRestEnvelope Envelope;
	Envelope.DataPath = FString(Path);
	return Envelope;
}

FVaRestEnvelope FVaRestEnvelope::SuccessWhen(FStringView Path, TArray<FString> Values) const
{
	FVaRestEnvelope Copy = *this;
	Copy.SuccessPath = FString(Path);
	Copy.SuccessValues = MoveTemp(Values);
	return Copy;
}

FVaRestEnvelope FVaRestEnvelope::ErrorMessageAt(FStringView Path) const
{
	FVaRestEnvelope Copy = *this;
	Copy.ErrorMessagePath = FString(Path);
	return Copy;
}

FVaRestEnvelope FVaRestEnvelope::ErrorCodeAt(FStringView Path) const
{
	FVaRestEnvelope Copy = *this;
	Copy.ErrorCodePath = FString(Path);
	return Copy;
}

FVaRestEnvelope FVaRestEnvelope::ParseErrorBodies(bool bParse) const
{
	FVaRestEnvelope Copy = *this;
	Copy.bParseErrorBodies = bParse;
	return Copy;
}

FString FVaRestEnvelope::ValueText(const FVaRestJson& Value)
{
	if (!Value.IsValid())
	{
		return FString();
	}
	return Value.IsString() ? Value.AsString() : Value.ToString(EVaRestJsonFormat::Condensed);
}

void FVaRestEnvelope::ReadErrorFields(const FVaRestJson& Body, FString& OutCode, FString& OutMessage) const
{
	using namespace VaRest::Http::EnvelopePrivate;
	OutCode = ErrorCodePath.IsEmpty() ? FString() : Shown(ValueText(Body.Path(ErrorCodePath)));
	OutMessage = ErrorMessagePath.IsEmpty() ? FString() : ValueText(Body.Path(ErrorMessagePath));
}

FVaRestEnvelopeResult FVaRestEnvelope::Evaluate(const FVaRestJson& Body) const
{
	using namespace VaRest::Http::EnvelopePrivate;
	FVaRestEnvelopeResult Out;
	if (HasPredicate())
	{
		const FVaRestJson Found = Body.Path(SuccessPath);
		const FString Actual = ValueText(Found);
		const bool bPass = Found.IsValid() && SuccessValues.ContainsByPredicate([&Actual](const FString& Value) { return Value.Equals(Actual, ESearchCase::CaseSensitive); });
		if (!bPass)
		{
			Out.bSuccess = false;
			Out.bDataFound = false;
			Out.FailedPath = SuccessPath;
			Out.Expected = FString::Join(SuccessValues, TEXT("|"));
			Out.Actual = Found.IsValid() ? Shown(Actual) : FString(TEXT("(missing)"));
			ReadErrorFields(Body, Out.Code, Out.Message);
			return Out;
		}
	}
	if (DataPath.IsEmpty())
	{
		Out.Data = Body;
		Out.bDataFound = Body.IsValid();
		return Out;
	}
	Out.Data = Body.Path(DataPath);
	Out.bDataFound = Out.Data.IsValid();
	return Out;
}
```

- [ ] **Step 5: Run it**

Run: `Tools/run_tests.sh VaRest.Http.Envelope`
Expected: `0 failed`.

- [ ] **Step 6: Restore-the-bug** — compare `Found.ToString()` instead of `ValueText(Found)` (a string keeps its quotes): `VaRest.Http.Envelope.Evaluate` FAILS on `Code "200"`; restore; PASS. Drop `Out.bDataFound = Out.Data.IsValid();`: FAILS on "no data: still a success, the data missing"; restore; PASS.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestEnvelope.h Source/VaRestHttp/Private/VaRestEnvelope.cpp Source/VaRestHttp/Private/Tests/VaRestHttpEnvelopeTest.cpp
git commit -m "http: M4 task 9 -- FVaRestEnvelope and its public evaluator: data path, success predicate, error fields (spec 3.4, WS 19 item 6)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 10: The envelope in the pipeline — client and request options, decode, `Kind=Server`, error bodies, the trace (spec §3.4; IN-5)

Plan decision 9. The envelope runs on the decode task, right after the JSON parse, so it costs no extra parse for `FVaRestJson` and struct results. A `void` result with a predicate gets one copying parse on that task. The outcome goes to `Completed.Envelope`.

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (`Envelope()`, `NoEnvelope()`, `WithEnvelope()`, `WithoutEnvelope()`)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h` (`FClientConfig::Envelope`), `Source/VaRestHttp/Private/VaRestExchangeState.h`
- Modify: `Source/VaRestHttp/Public/VaRestResultHandler.h` (`FFinish` envelope fields, `AppliesEnvelope()`, `CheckEnvelopePredicate`)
- Modify: `Source/VaRestHttp/Private/VaRestDecode.h`, `Source/VaRestHttp/Private/VaRestDecode.cpp` (`ApplyEnvelope`, `DecodeJson`, `DecodeStruct`, `ProcessErrorBody`, `CheckEnvelopePredicate`)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Resolve`, `MakeFinish`, `OnChainDone`), `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (`Completed`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpEnvelopeTest.cpp`, `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp`

**Interfaces:**
- Consumes: Task 9.
- Produces: `TVaRestRequest<T>::Envelope(const FVaRestEnvelope& InEnvelope) &&`, `TVaRestRequest<T>::NoEnvelope() &&`; `FVaRestClient& WithEnvelope(const FVaRestEnvelope& InEnvelope)`, `FVaRestClient& WithoutEnvelope()`; `TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> FClientConfig::Envelope`; on `FExchangeState`: `TOptional<FVaRestEnvelope> EnvelopeOverride`, `bool bNoEnvelope`, `TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope`; on `FFinish`: `Envelope` (same type), `FVaRestTraceEnvelope EnvelopeTrace`, `bool bEnvelopeDataMissing`, `FVaRestJson EnvelopeBody`; `virtual bool IResultHandler::AppliesEnvelope() const`; `VARESTHTTP_API void VaRest::Http::CheckEnvelopePredicate(FFinish&)`; private `bool VaRest::Http::ApplyEnvelope(FFinish&, FVaRestJson& InOut)`.

- [ ] **Step 1: Write the failing tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpEnvelopeTest.cpp` (before `#endif`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopePipelineTest, "VaRest.Http.Envelope.Pipeline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopePipelineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::EnvelopeTestPrivate;
	FTestRig Rig;
	Rig.Client.WithEnvelope(OkData().ErrorCodeAt(TEXT("error.code")).ErrorMessageAt(TEXT("error.message")).ParseErrorBodies());
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		const FString& Url = Request.Url;
		if (Url.EndsWith(TEXT("/no")) || Url.EndsWith(TEXT("/void-no")))
		{
			return Json(200, R"({"ok":false,"error":{"code":"E42","message":"the envelope says no"}})");
		}
		if (Url.EndsWith(TEXT("/gone")))
		{
			return Json(404, R"({"error":{"code":"NF","message":"gone"}})");
		}
		if (Url.EndsWith(TEXT("/text")))
		{
			return FVaRestFakeResponse::Text(200, TEXT("plain"));
		}
		if (Url.EndsWith(TEXT("/empty")))
		{
			FVaRestFakeResponse Empty;
			Empty.Status = 204;
			return Empty;
		}
		return Json(200, R"({"ok":true,"data":{"id":7}})");
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Ok = MakeOutcome<FVaRestJson>();
	auto No = MakeOutcome<FVaRestJson>();
	auto Gone = MakeOutcome<FVaRestJson>();
	auto Raw = MakeOutcome<FVaRestJson>();
	auto Narrow = MakeOutcome<FVaRestJson>();
	auto VoidNo = MakeOutcome<void>();
	auto VoidOk = MakeOutcome<void>();
	auto VoidText = MakeOutcome<void>();
	auto Text = MakeOutcome<FString>();
	auto Empty = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle NoHandle = Observe(Rig.Client.Get(TEXT("/no")), No).Send();
	Observe(Rig.Client.Get(TEXT("/ok")), Ok).Send();
	Observe(Rig.Client.Get(TEXT("/gone")), Gone).Send();
	Observe(Rig.Client.Get(TEXT("/raw")).NoEnvelope(), Raw).Send();
	Observe(Rig.Client.Get(TEXT("/narrow")).Envelope(FVaRestEnvelope::DataAt(TEXT("data.id"))), Narrow).Send();
	const int32 OnBefore = Counters().DecodesOnGameThread.load();
	Observe(Rig.Client.Post<void>(TEXT("/void-no")), VoidNo).Send();
	Observe(Rig.Client.Post<void>(TEXT("/void-ok")), VoidOk).Send();
	Observe(Rig.Client.Post<void>(TEXT("/text")), VoidText).Send();
	Observe(Rig.Client.Get<FString>(TEXT("/ok")), Text).Send();
	Observe(Rig.Client.Get(TEXT("/empty")), Empty).Send();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Ok->IsDone() && No->IsDone() && Gone->IsDone() && Raw->IsDone() && Narrow->IsDone() && VoidNo->IsDone()
		&& VoidOk->IsDone() && VoidText->IsDone() && Text->IsDone() && Empty->IsDone(); })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	TestTrue(TEXT("unwrapped: the data object"), Ok->SuccessCalls.load() == 1 && Ok->Value.IsSet() && Ok->Value->Get(TEXT("id")).AsInt() == 7);
	TestTrue(TEXT("a failed predicate: Kind Server, status 200, not retryable"), No->Error.Kind == EVaRestErrorKind::Server && No->Error.HttpStatus == 200
		&& !No->Error.bRetryable);
	TestTrue(TEXT("... its code and message from the envelope paths"), No->Error.Code == TEXT("E42") && No->Error.Message == TEXT("the envelope says no"));
	TestTrue(TEXT("... the body kept as the error body"), No->Error.Body.IsValid() && !No->Error.Body.Get(TEXT("ok")).AsBool(true));
	TestTrue(TEXT("bParseErrorBodies: a 404's code and message"), Gone->Error.Kind == EVaRestErrorKind::Http && Gone->Error.HttpStatus == 404
		&& Gone->Error.Code == TEXT("NF") && Gone->Error.Message == TEXT("gone"));
	TestTrue(TEXT("NoEnvelope(): the whole body"), Raw->Value.IsSet() && Raw->Value->Get(TEXT("ok")).AsBool());
	TestTrue(TEXT("a request's own envelope"), Narrow->Value.IsSet() && Narrow->Value->AsInt() == 7);
	TestTrue(TEXT("void with a predicate: a failure is Kind Server"), VoidNo->Error.Kind == EVaRestErrorKind::Server && VoidNo->Error.Code == TEXT("E42"));
	TestTrue(TEXT("... a success is a success"), VoidOk->SuccessCalls.load() == 1);
	TestTrue(TEXT("... a body that is not JSON is nothing to judge"), VoidText->SuccessCalls.load() == 1);
	TestEqual(TEXT("... and its parse ran off the game thread"), Counters().DecodesOnGameThread.load() - OnBefore, 0);
	TestTrue(TEXT("an FString result is the body as it came"), Text->Value.IsSet() && Text->Value->Contains(TEXT("\"ok\":true")));
	TestTrue(TEXT("204: a success with no value to unwrap"), Empty->SuccessCalls.load() == 1 && Empty->Value.IsSet() && !Empty->Value->IsValid());

	// IN-5: Completed carries the envelope's outcome
	const TArray<FVaRestTraceEvent> Events = Scoped.Recorder->For(NoHandle.GetId());
	const FVaRestTraceEvent* Completed = Events.FindByPredicate([](const FVaRestTraceEvent& Event) { return Event.Kind == EVaRestTraceKind::Completed; });
	if (!TestNotNull(TEXT("Completed"), Completed))
	{
		return false;
	}
	const FVaRestTraceEnvelope& Seen = Completed->Payload.Get<FVaRestTraceCompleted>().Envelope;
	TestTrue(TEXT("IN-5 Envelope: applied, data path, the failed predicate, code and message"), Seen.bApplied && Seen.DataPath == TEXT("data")
		&& Seen.PredicatePath == TEXT("ok") && Seen.Expected == TEXT("true") && Seen.Actual == TEXT("false") && Seen.Code == TEXT("E42")
		&& Seen.Message == TEXT("the envelope says no"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeMissingTest, "VaRest.Http.Envelope.MissingPaths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeMissingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::EnvelopeTestPrivate;
	// Review Focus: a data path that misses is a missing value for FVaRestJson (spec 2.2), never a crash or a Server error
	FTestRig Rig;
	Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	Rig.Fake->Enqueue(Json(200, "[1,2]"));
	auto NoData = MakeOutcome<FVaRestJson>();
	auto Array = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), NoData).Send();
	Observe(Rig.Client.Get(TEXT("/b")), Array).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([NoData, Array] { return NoData->IsDone() && Array->IsDone(); })))
	{
		return false;
	}
	TestTrue(TEXT("no data: a success with an invalid handle"), NoData->SuccessCalls.load() == 1 && NoData->Value.IsSet() && !NoData->Value->IsValid());
	TestTrue(TEXT("an array body: the same"), Array->SuccessCalls.load() == 1 && Array->Value.IsSet() && !Array->Value->IsValid());
	return true;
}
```

Append to `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp` (before `#endif`; add `#include "VaRestEnvelope.h"`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEnvelopeStructTest, "VaRest.Http.Envelope.Struct",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEnvelopeStructTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true,"data":{"id":5,"name":"neo"}})"));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Wrapped = MakeOutcome<FVaRestHttpTestProfile>();
	auto Missing = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/a")), Wrapped).Send();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/b")), Missing).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Wrapped, Missing] { return Wrapped->IsDone() && Missing->IsDone(); })))
	{
		return false;
	}
	const FVaRestHttpTestProfile Unset;
	TestTrue(TEXT("the struct decoded from the data"), Wrapped->SuccessCalls.load() == 1 && Wrapped->Value.Get(Unset).Id == 5);
	// Plan decision 9: a struct names the missing path instead of decoding to its defaults
	TestTrue(TEXT("no data for a struct: Kind Decode at the data path"), Missing->Error.Kind == EVaRestErrorKind::Decode && Missing->Error.Path == TEXT("data")
		&& Missing->Error.Message.Contains(TEXT("\"data\"")));
	TestTrue(TEXT("... with the body as the error body"), Missing->Error.Body.IsValid() && Missing->Error.Body.Get(TEXT("ok")).AsBool());
	return true;
}
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Envelope`
Expected: FAIL to compile (`WithEnvelope`, `NoEnvelope`, `Envelope` unknown on the builder).

- [ ] **Step 3: Options** — in `Source/VaRestHttp/Public/VaRestClient.h` add `#include "VaRestEnvelope.h"`; `FRequestSpec` fields after `Retry`:

```cpp
		/** Envelope(); unset with bNoEnvelope false: the client's */
		TOptional<FVaRestEnvelope> Envelope;
		/** NoEnvelope() */
		bool bNoEnvelope = false;
```

builder methods after `NoRetry()`:

```cpp
	/** Spec 3.4: this request's envelope, replacing the client's (WithEnvelope). See FVaRestEnvelope for which results it applies to. */
	TVaRestRequest Envelope(const FVaRestEnvelope& InEnvelope) &&
	{
		Spec->Envelope = InEnvelope;
		Spec->bNoEnvelope = false;
		return MoveTemp(*this);
	}

	/** The body as it comes, whatever the client's envelope */
	TVaRestRequest NoEnvelope() &&
	{
		Spec->Envelope.Reset();
		Spec->bNoEnvelope = true;
		return MoveTemp(*this);
	}
```

client methods after `WithRetry`:

```cpp
	/** Spec 3.4: the envelope of this client's responses (a request's Envelope() or NoEnvelope() replaces it). Game thread. */
	FVaRestClient& WithEnvelope(const FVaRestEnvelope& InEnvelope);
	FVaRestClient& WithoutEnvelope();
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithEnvelope(const FVaRestEnvelope& InEnvelope)
{
	if (State.IsValid())
	{
		TSharedRef<const FVaRestEnvelope, ESPMode::ThreadSafe> Shared = MakeShared<const FVaRestEnvelope, ESPMode::ThreadSafe>(InEnvelope);
		State->EditConfig([&Shared](VaRest::Http::FClientConfig& Edited) { Edited.Envelope = Shared; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithoutEnvelope()
{
	if (State.IsValid())
	{
		State->EditConfig([](VaRest::Http::FClientConfig& Edited) { Edited.Envelope.Reset(); });
	}
	return *this;
}
```

and in `FRequestSpecAccess::Send`, after the retry line: `Ex.EnvelopeOverride = MoveTemp(Spec->Envelope); Ex.bNoEnvelope = Spec->bNoEnvelope;`.

`Source/VaRestHttp/Private/VaRestClientState.h`, `FClientConfig`: `TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope;` (`#include "VaRestEnvelope.h"`). `Source/VaRestHttp/Private/VaRestExchangeState.h`, after the retry block:

```cpp
		// Envelope (Task 10), set by Resolve:
		TOptional<FVaRestEnvelope> EnvelopeOverride;
		bool bNoEnvelope = false;
		/** Spec 3.4: the request's, else the client's; null with NoEnvelope() or none configured */
		TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope;
```

- [ ] **Step 4: The finish and the handlers** — in `Source/VaRestHttp/Public/VaRestResultHandler.h` add `#include "VaRestEnvelope.h"` and `#include "VaRestTrace.h"`; `FFinish` gains:

```cpp
		/** Spec 3.4: applied on the decode task (plan decision 9); null = none */
		TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope;
		/** IN-5: what the envelope did, for Completed (unscrubbed here; the trace point scrubs it) */
		FVaRestTraceEnvelope EnvelopeTrace;
		/** The envelope's DataPath named nothing: a struct result fails (plan decision 9) and keeps EnvelopeBody as its error body */
		bool bEnvelopeDataMissing = false;
		FVaRestJson EnvelopeBody;
```

`IResultHandler` gains after `DecodesJson()`:

```cpp
		/** Spec 3.4 / plan decision 9: FVaRestJson and struct results unwrap; void ones check the predicate; raw ones are left alone */
		virtual bool AppliesEnvelope() const = 0;
```

beside `DecodeJson`:

```cpp
	/**
	 * Plan decision 9: a void result's envelope predicate -- a JSON body (by type, or sniffed without one) is parsed (a copy;
	 * void keeps no value) and a failed predicate sets a Kind=Server error. Nothing to judge without a predicate, without a
	 * body, for 204, for a body in a SaveTo file, or for a body that is not JSON. Background thread.
	 */
	VARESTHTTP_API void CheckEnvelopePredicate(FFinish& Finish);
```

`TResultHandler<T>` gains:

```cpp
		virtual bool AppliesEnvelope() const override
		{
			return std::is_same_v<T, FVaRestJson> || bIsStruct;
		}
```

and `TResultHandler<void>` gains `virtual bool AppliesEnvelope() const override { return true; }`, its `Decode` becoming:

```cpp
		virtual void Decode(FFinish& Finish) override
		{
			CheckEnvelopePredicate(Finish);
			ReleaseBody(Finish);
		}
```

- [ ] **Step 5: Decode** — in `Source/VaRestHttp/Private/VaRestDecode.h` declare:

```cpp
	/**
	 * Spec 3.4 on a parsed body (plan decision 9): InOut becomes the data, or the body stays and Finish.Error is a Kind=Server
	 * failure (false). A DataPath that names nothing leaves InOut invalid and sets bEnvelopeDataMissing. Records EnvelopeTrace.
	 * True without an envelope. Background thread.
	 */
	bool ApplyEnvelope(FFinish& Finish, FVaRestJson& InOut);
```

In `Source/VaRestHttp/Private/VaRestDecode.cpp`:

```cpp
bool VaRest::Http::ApplyEnvelope(FFinish& Finish, FVaRestJson& InOut)
{
	if (!Finish.Envelope.IsValid())
	{
		return true;
	}
	const FVaRestEnvelope& Envelope = *Finish.Envelope;
	const FVaRestEnvelopeResult Result = Envelope.Evaluate(InOut);
	FVaRestTraceEnvelope& Trace = Finish.EnvelopeTrace;
	Trace.bApplied = true;
	Trace.DataPath = Envelope.DataPath;
	if (!Result.bSuccess)
	{
		Trace.PredicatePath = Result.FailedPath;
		Trace.Expected = Result.Expected;
		Trace.Actual = Result.Actual;
		Trace.Code = Result.Code;
		Trace.Message = Result.Message;
		// Spec 3.4: "A 2xx response whose predicate fails becomes Kind=Server"
		Finish.Error = Result.ToError(InOut, Finish.Response.Status);
		InOut = FVaRestJson();
		return false;
	}
	if (!Result.bDataFound)
	{
		Finish.bEnvelopeDataMissing = true;
		Finish.EnvelopeBody = InOut;
	}
	InOut = Result.Data;
	return true;
}

void VaRest::Http::CheckEnvelopePredicate(FFinish& Finish)
{
	if (!Finish.Envelope.IsValid() || !Finish.Envelope->HasPredicate() || Finish.Body.Num() == 0 || Finish.Response.Status == 204 || Finish.bBodyInFile)
	{
		return;
	}
	const FString& ContentType = Finish.Response.ContentType;
	if (!IsJsonContentType(ContentType) && !(ContentType.IsEmpty() && DecodePrivate::LooksLikeJson(Finish.Body)))
	{
		return;
	}
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Finish.Body), Finish.ParseOptions);
	if (Parsed.HasError())
	{
		// A void result decodes nothing: a broken body is no envelope failure
		return;
	}
	FVaRestJson Body = Parsed.StealValue();
	ApplyEnvelope(Finish, Body);
}
```

In `DecodeJson`, the last two lines `Out = Parsed.StealValue(); return true;` become:

```cpp
	Out = Parsed.StealValue();
	// Spec 3.4, plan decision 9: on this task, next to the parse
	return ApplyEnvelope(Finish, Out);
```

In `DecodeStruct`, the `if (!Json.IsValid()) { return true; }` block becomes:

```cpp
	if (!Json.IsValid())
	{
		if (Finish.bEnvelopeDataMissing)
		{
			// Plan decision 9: a struct names the missing path instead of decoding to its defaults
			Finish.Error.Kind = EVaRestErrorKind::Decode;
			Finish.Error.Message = FString::Printf(TEXT("the envelope's data path \"%s\" is not in the response"), *Finish.Envelope->DataPath);
			Finish.Error.Path = Finish.Envelope->DataPath;
			Finish.Error.Body = Finish.EnvelopeBody;
			return false;
		}
		return true; // 204 or empty: the struct keeps its defaults (spec 3.2)
	}
```

(`DecodeJson` already returned false for a `Kind=Server` failure, so `DecodeStruct` stops there.) In `ProcessErrorBody`, after the `if (bJson) { ... }` block:

```cpp
	if (Finish.Envelope.IsValid() && Finish.Envelope->bParseErrorBodies && Finish.Error.Body.IsValid())
	{
		// Spec 3.4: "bParseErrorBodies (read message/code from 4xx/5xx bodies)"
		FString Code;
		FString Message;
		Finish.Envelope->ReadErrorFields(Finish.Error.Body, Code, Message);
		if (!Code.IsEmpty())
		{
			Finish.Error.Code = Code;
		}
		if (!Message.IsEmpty())
		{
			Finish.Error.Message = Message;
		}
		Finish.EnvelopeTrace.bApplied = true;
		Finish.EnvelopeTrace.DataPath = Finish.Envelope->DataPath;
		Finish.EnvelopeTrace.Code = Code;
		Finish.EnvelopeTrace.Message = Message;
	}
```

- [ ] **Step 6: The pipeline and the trace** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`:

`Resolve`, after the deadline lines:

```cpp
	// Spec 3.4: the request's own, else the client's; NoEnvelope() none
	if (Exchange.bNoEnvelope)
	{
		Exchange.Envelope.Reset();
	}
	else if (Exchange.EnvelopeOverride.IsSet())
	{
		Exchange.Envelope = MakeShared<const FVaRestEnvelope, ESPMode::ThreadSafe>(Exchange.EnvelopeOverride.GetValue());
	}
	else
	{
		Exchange.Envelope = Cfg.Envelope;
	}
```

`MakeFinish` gains `Finish.Envelope = Exchange.Envelope;`. In `OnChainDone`, the `bTask` computation becomes:

```cpp
	// Plan decision 9: a void result with a predicate is parsed too, on the task, never here
	const bool bEnvelopeCheck = Finish.Envelope.IsValid() && Finish.Envelope->HasPredicate() && Ex.Handler->AppliesEnvelope() && !Finish.bBodyInFile;
	const bool bTask = bDownload
		|| (Finish.Error.IsError() ? Finish.Error.Kind == EVaRestErrorKind::Http && bHasBody : (Ex.Handler->DecodesBody() || bEnvelopeCheck) && bHasBody);
```

In `Source/VaRestHttp/Private/VaRestTracePoints.cpp`, `Completed`, before the `Emit`:

```cpp
	// IN-5 Envelope; Actual, Code and Message are the server's text: scrubbed like every captured string (Inspector 2.2 layer 3)
	Payload.Envelope = Finish.EnvelopeTrace;
	Payload.Envelope.Actual = Redactor.ScrubText(Payload.Envelope.Actual);
	Payload.Envelope.Code = Redactor.ScrubText(Payload.Envelope.Code);
	Payload.Envelope.Message = Redactor.ScrubText(Payload.Envelope.Message);
```

- [ ] **Step 7: Run them, then the decode suites**

Run: `Tools/run_tests.sh VaRest.Http.Envelope` then `Tools/run_tests.sh --no-build VaRest.Http.Decode` then `Tools/run_tests.sh --no-build VaRest.Http.Pipeline`
Expected: `0 failed` each.

- [ ] **Step 8: Restore-the-bug** — drop `|| bEnvelopeCheck` from `bTask`: `VaRest.Http.Envelope.Pipeline` FAILS ("void with a predicate", the predicate is never checked, or "its parse ran off the game thread"); restore; PASS. Drop the `bEnvelopeDataMissing` branch in `DecodeStruct`: `VaRest.Http.Envelope.Struct` FAILS; restore; PASS.

- [ ] **Step 9: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Public/VaRestResultHandler.h Source/VaRestHttp/Private/VaRestDecode.h \
  Source/VaRestHttp/Private/VaRestDecode.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestTracePoints.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpEnvelopeTest.cpp Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp
git commit -m "http: M4 task 10 -- the envelope in the pipeline: unwrap on the decode task, Kind=Server, error bodies, void predicates, IN-5

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 11: The session — tokens, expiry, session headers, events, export and import, secrets (spec §4.2; WebSocket §19 item 2, §22 A1)

Plan decisions 5, 6, 17. A client's session is live state on its `FClientState`, game thread only. Its values are secret slots of the client's redactor (Inspector IN-13 layer 3) and its header names are redaction keys (layer 2); `ClearSession` forgets exactly the session's slots (Inspector §12: "cleared on logout"). No request uses the session yet: Task 12's `AuthApply` stage does.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestAuthTypes.h`, `Source/VaRestHttp/Private/VaRestSession.h`, `Source/VaRestHttp/Private/VaRestSession.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestRedactor.h`, `Source/VaRestHttp/Private/VaRestRedactor.cpp` (`FVaRestSecretSet::ClearSlots`; a previous value remembers its slot)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp` (the session members; `SetSecrets`)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (the public session API; `AppendRedactKeys` moves to `VaRest::Http`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpSessionTest.cpp`

**Interfaces:**
- Consumes: Task 4 (`TracePoints::Marker`).
- Produces (public, `VaRestAuthTypes.h`): `UENUM(BlueprintType) enum class EVaRestAuthMode : uint8 { Required, IfAvailable, None }`; `UENUM(BlueprintType) enum class EVaRestApiKeyPlacement : uint8 { Header, Query }`; `UENUM(BlueprintType) enum class EVaRestRefreshStyle : uint8 { Json, OAuth2Form }`; `UENUM(BlueprintType) enum class EVaRestSessionChange : uint8 { Refreshed, Replaced, Cleared }`; `USTRUCT(BlueprintType) struct FVaRestBearerRefreshConfig` (fields in Step 3; Task 14 uses it); `struct FVaRestSessionData { FString AccessToken; FString RefreshToken; double ExpiresAt; FVaRestHeaders Headers; uint32 Generation; bool HasAccessToken() const; bool HasRefreshToken() const; }`; `using FVaRestSessionChangedEvent = TMulticastDelegate<void(EVaRestSessionChange)>`; `using FVaRestSessionExpiredEvent = TMulticastDelegate<void()>`. On `FVaRestClient`: `void SetBearerToken(FStringView AccessToken, bool bSameIdentity = false)`, `void SetSession(FStringView AccessToken, FStringView RefreshToken, double ExpiresInSeconds = 0.0, bool bSameIdentity = false)`, `void SetSessionHeader(FStringView Name, FStringView Value)`, `void RemoveSessionHeader(FStringView Name)`, `void ClearSession()`, `FVaRestSessionData GetSession() const`, `FString ExportSession() const`, `bool ImportSession(FStringView Json, bool bSameIdentity = false)`, `void NotifySessionExpired()`, `FVaRestSessionChangedEvent& OnSessionChanged()`, `FVaRestSessionExpiredEvent& OnSessionExpired()`. Private on `FClientState`: `const FVaRestSessionData& GetSessionData() const`, `bool UpdateTokens(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds)`, `void AnnounceSession(EVaRestSessionChange, uint64 CauseRequestId, const TCHAR* Text)`, `void SetTokensByGame(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, bool bSameIdentity)`, `void SetSessionHeader(FStringView, FStringView)`, `void RemoveSessionHeader(FStringView)`, `void ClearSession()`, `void ExpireSession(uint64 CauseRequestId, FStringView Why)`, `FString ExportSession() const`, `bool ImportSession(FStringView Json, bool bSameIdentity)`, `void SetSecrets(TConstArrayView<TPair<FName, FString>>)`; `struct VaRest::Http::FSessionState { FVaRestSessionData Data; FVaRestSessionChangedEvent Changed; FVaRestSessionExpiredEvent Expired; }`; `void VaRest::Http::AppendRedactKeys(TArray<FString>& Into, TConstArrayView<FString> Keys)`; `void FVaRestSecretSet::ClearSlots(TFunctionRef<bool(FName Slot)> Matches)`.

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestHttp/Private/Tests/VaRestHttpSessionTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::SessionTestPrivate
{
	using FChanges = TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>;

	FChanges Watch(FVaRestClient& Client)
	{
		FChanges Seen = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
		Client.OnSessionChanged().AddLambda([Seen](EVaRestSessionChange Change) { Seen->Add(Change); });
		return Seen;
	}

	FString Scrub(const FVaRestClient& Client, const TCHAR* Text)
	{
		return VaRest::Http::FClientAccess::State(Client)->GetRedactor()->ScrubText(Text);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionLifecycleTest, "VaRest.Http.Session.Lifecycle",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionLifecycleTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	FTestRig Rig;
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const FChanges Seen = Watch(Rig.Client);
	Rig.Clock->Advance(5.0);
	Rig.Client.SetSession(TEXT("access-one-123456"), TEXT("refresh-one-123456"), 60.0);
	FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("both tokens"), Data.AccessToken == TEXT("access-one-123456") && Data.RefreshToken == TEXT("refresh-one-123456"));
	TestEqual(TEXT("the expiry on the client's clock"), Data.ExpiresAt, 65.0);
	const uint32 FirstGeneration = Data.Generation;
	TestTrue(TEXT("a login is Replaced (plan decision 5)"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Replaced);

	Rig.Client.SetBearerToken(TEXT("access-two-123456"), /*bSameIdentity*/ true);
	Data = Rig.Client.GetSession();
	TestTrue(TEXT("a token of the same identity is Refreshed"), Seen->Num() == 2 && (*Seen)[1] == EVaRestSessionChange::Refreshed);
	TestTrue(TEXT("... the refresh token kept, the expiry unknown, the generation up"), Data.RefreshToken == TEXT("refresh-one-123456")
		&& Data.ExpiresAt == 0.0 && Data.Generation > FirstGeneration);
	Rig.Client.SetBearerToken(TEXT("access-two-123456"));
	TestEqual(TEXT("the same token again: no event"), Seen->Num(), 2);

	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-id-123456"));
	TestEqualSensitive(TEXT("a session header"), Rig.Client.GetSession().Headers.Get(TEXT("X-Session-ID")), FString(TEXT("session-id-123456")));
	TestEqual(TEXT("... no session event for a header"), Seen->Num(), 2);

	Rig.Client.ClearSession();
	Data = Rig.Client.GetSession();
	TestTrue(TEXT("logout: nothing left"), !Data.HasAccessToken() && !Data.HasRefreshToken() && Data.Headers.IsEmpty());
	TestTrue(TEXT("... Cleared"), Seen->Num() == 3 && (*Seen)[2] == EVaRestSessionChange::Cleared);
	Rig.Client.ClearSession();
	TestEqual(TEXT("clearing an empty session: no event"), Seen->Num(), 3);

	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	Rig.Client.SetSession(TEXT("access-three-12345"), TEXT("refresh-three-1234"));
	Rig.Client.NotifySessionExpired();
	Data = Rig.Client.GetSession();
	TestTrue(TEXT("WS 19 item 2 NotifySessionExpired: tokens dropped, OnSessionExpired once"), *Expired == 1 && !Data.HasAccessToken() && !Data.HasRefreshToken());

	const TArray<FVaRestTraceMarker> Markers = Scoped.Recorder->MarkersOf(Rig.Client.GetId());
	TArray<EVaRestTraceMarkerKind> Kinds;
	for (const FVaRestTraceMarker& Marker : Markers)
	{
		Kinds.Add(Marker.Kind);
	}
	const TArray<EVaRestTraceMarkerKind> Expected = { EVaRestTraceMarkerKind::SessionReplaced, EVaRestTraceMarkerKind::SessionRefreshed,
		EVaRestTraceMarkerKind::SessionCleared, EVaRestTraceMarkerKind::SessionReplaced, EVaRestTraceMarkerKind::SessionExpired };
	TestTrue(TEXT("IN-9: one marker per change, in order"), Kinds == Expected);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionSecretsTest, "VaRest.Http.Session.Secrets",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionSecretsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	FTestRig Rig;
	Rig.Client.SetSecretValue(TEXT("Mine"), TEXT("game-own-secret-123"));
	Rig.Client.SetSession(TEXT("access-secret-123456"), TEXT("refresh-secret-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Game-Ticket"), TEXT("ticket-value-123456"));
	TestEqualSensitive(TEXT("IN-13 layer 3: the tokens and the header value are scrubbed"),
		Scrub(Rig.Client, TEXT("a access-secret-123456 b refresh-secret-123456 c ticket-value-123456")), FString(TEXT("a *** b *** c ***")));
	TestTrue(TEXT("layer 2: the header's name is a key"), VaRest::Http::FClientAccess::State(Rig.Client)->GetRedactor()->GetRules().ShouldRedact(FStringView(TEXT("X-Game-Ticket")), EVaRestRedactKeyKind::Header));
	Rig.Client.SetBearerToken(TEXT("access-rotated-1234"), true);
	TestEqualSensitive(TEXT("a rotated token stays known (Inspector D21)"), Scrub(Rig.Client, TEXT("access-secret-123456 access-rotated-1234")), FString(TEXT("*** ***")));
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("logout forgets the session's values (Inspector 12)"), Scrub(Rig.Client, TEXT("access-rotated-1234 ticket-value-123456")),
		FString(TEXT("access-rotated-1234 ticket-value-123456")));
	TestEqualSensitive(TEXT("... and keeps the game's own"), Scrub(Rig.Client, TEXT("game-own-secret-123")), FString(TEXT("***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionExportTest, "VaRest.Http.Session.ExportImport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionExportTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::SessionTestPrivate;
	FTestRig From;
	From.Client.SetSession(TEXT("access-export-12345"), TEXT("refresh-export-1234"), 120.0);
	From.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("session-export-123"));
	const FString Exported = From.Client.ExportSession();
	TestTrue(TEXT("an export holds the tokens (the docs carry the caveat)"), Exported.Contains(TEXT("access-export-12345")) && Exported.Contains(TEXT("\"version\":1")));

	FTestRig To;
	const FChanges Seen = Watch(To.Client);
	if (!TestTrue(TEXT("imported"), To.Client.ImportSession(Exported)))
	{
		return false;
	}
	const FVaRestSessionData Data = To.Client.GetSession();
	TestTrue(TEXT("the same tokens and header"), Data.AccessToken == TEXT("access-export-12345") && Data.RefreshToken == TEXT("refresh-export-1234")
		&& Data.Headers.Get(TEXT("X-Session-ID")) == TEXT("session-export-123"));
	TestTrue(TEXT("the expiry carried through UTC, within a second"), FMath::Abs(Data.ExpiresAt - 120.0) < 1.0);
	TestTrue(TEXT("an import is Replaced"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Replaced);
	TestEqualSensitive(TEXT("... and its values are secrets"), Scrub(To.Client, TEXT("session-export-123")), FString(TEXT("***")));

	FTestRig Bad;
	TestFalse(TEXT("not JSON"), Bad.Client.ImportSession(TEXT("{nope")));
	TestFalse(TEXT("another version"), Bad.Client.ImportSession(TEXT(R"({"version":2,"accessToken":"a"})")));
	TestFalse(TEXT("a token with CR LF"), Bad.Client.ImportSession(TEXT(R"({"version":1,"accessToken":"a\r\nX-Evil: 1"})")));
	TestFalse(TEXT("a bad date"), Bad.Client.ImportSession(TEXT(R"({"version":1,"accessToken":"a","expiresAt":"tomorrow"})")));
	TestFalse(TEXT("nothing changed"), Bad.Client.GetSession().HasAccessToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSessionRefusesTest, "VaRest.Http.Session.RefusesControlCharacters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSessionRefusesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Decision 11's rule for credentials: a token or a session header goes into a header line
	AddExpectedErrorPlain(TEXT("VaRest: SetBearerToken: the token holds"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("VaRest: SetSessionHeader: the name is not a token"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Rig;
	Rig.Client.SetBearerToken(TEXT("abc\r\nX-Evil: 1"));
	Rig.Client.SetSessionHeader(TEXT("Bad Name"), TEXT("v"));
	TestFalse(TEXT("no token set"), Rig.Client.GetSession().HasAccessToken());
	TestTrue(TEXT("no header set"), Rig.Client.GetSession().Headers.IsEmpty());
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Session`
Expected: FAIL to compile (`SetSession`, `GetSession`, `OnSessionChanged` unknown).

- [ ] **Step 3: The types** — create `Source/VaRestHttp/Public/VaRestAuthTypes.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "VaRestHttpTypes.h"

#include "VaRestAuthTypes.generated.h"

/** Spec 4.2 / OpenAPI N15 (plan decision 4): what a request does with its client's auth */
UENUM(BlueprintType)
enum class EVaRestAuthMode : uint8
{
	/** Credentials, session headers, refresh and replay. With an auth that has no credentials (and cannot refresh): Kind=Auth, nothing sent. */
	Required,
	/** The same, but without credentials the request goes out without them */
	IfAvailable,
	/** NoAuth(): no credentials, no session headers, no park, no replay (a login request) */
	None
};

/** Spec 4.2 ApiKey */
UENUM(BlueprintType)
enum class EVaRestApiKeyPlacement : uint8
{
	Header,
	Query
};

/** Spec 4.2 BearerRefresh / OpenAPI N15: how the refresh request carries the refresh token */
UENUM(BlueprintType)
enum class EVaRestRefreshStyle : uint8
{
	/** application/json: { <RefreshTokenField>: "<refresh token>" } */
	Json,
	/** application/x-www-form-urlencoded, RFC 6749 6: grant_type=refresh_token&refresh_token=...[&client_id=...] */
	OAuth2Form
};

/** WebSocket spec 19 item 2 rev 2 (22 A1), plan decision 5 */
UENUM(BlueprintType)
enum class EVaRestSessionChange : uint8
{
	/** The same identity, a new token: the auth refreshed it, or the game said so (bSameIdentity) */
	Refreshed,
	/** Another identity's token: SetBearerToken, SetSession or ImportSession by the game (a login) */
	Replaced,
	/** ClearSession: logout */
	Cleared
};

/**
 * Spec 4.2 BearerRefresh: the refresh endpoint and where the tokens are. BlueprintType: M5's auth config asset holds one,
 * so the field names freeze then (plan decision 27). Paths are FVaRestJson::Path's and read the raw refresh response (no
 * envelope; plan decision 23): a wrapped API writes "data.accessToken".
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestBearerRefreshConfig
{
	GENERATED_BODY()

	/** POSTed to: a path joined to the client's base URL, or an absolute URL */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString RefreshPath = TEXT("/auth/refresh");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	EVaRestRefreshStyle Style = EVaRestRefreshStyle::Json;

	/** Json style: the request body's key for the refresh token (OAuth2Form always sends refresh_token) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString RefreshTokenField = TEXT("refreshToken");

	/** OAuth2Form: client_id, when the server wants one; empty sends none */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ClientId;

	/** The new access token in the response (required) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString AccessTokenPath = TEXT("accessToken");

	/** A new refresh token (optional: the current one is kept when the response has none) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString RefreshTokenPath = TEXT("refreshToken");

	/** Seconds the new access token lives (optional), a number or a numeric string */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ExpiresInPath = TEXT("expiresIn");

	/** A response with one of these refreshes the session and replays the request once (spec 4.2: 401, configurable) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<int32> RefreshStatuses = { 401 };

	/** Refresh this many seconds before a known expiry (spec 4.2: 30) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float RefreshLeadSeconds = 30.f;
};

/** Spec 4.2: a client's session, as FVaRestClient::GetSession copies it out. Game thread. */
struct FVaRestSessionData
{
	FString AccessToken;
	FString RefreshToken;
	/** Client-clock seconds (IVaRestClock) when the access token expires; 0 = unknown */
	double ExpiresAt = 0.0;
	/** dawg's X-Session-ID pattern: sent with every request whose auth mode is not None */
	FVaRestHeaders Headers;
	/** Bumped by every change of credentials: an attempt that applied an older one replays without a refresh (Task 13) */
	uint32 Generation = 0;

	bool HasAccessToken() const { return !AccessToken.IsEmpty(); }
	bool HasRefreshToken() const { return !RefreshToken.IsEmpty(); }
};

/** Game thread */
using FVaRestSessionChangedEvent = TMulticastDelegate<void(EVaRestSessionChange)>;
/** Game thread: a refresh was rejected, or NotifySessionExpired */
using FVaRestSessionExpiredEvent = TMulticastDelegate<void()>;
```

Create `Source/VaRestHttp/Private/VaRestSession.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"

namespace VaRest::Http
{
	/**
	 * Spec 4.2: one client's session, live (plan decision 6). Game thread. FClientState owns it; its session functions
	 * (VaRestSession.cpp) change it, publish its secrets and fire its events. Task 13 adds the refresh in flight.
	 */
	struct FSessionState
	{
		FVaRestSessionData Data;
		FVaRestSessionChangedEvent Changed;
		FVaRestSessionExpiredEvent Expired;
	};

	/** Every secret slot the session takes starts with this, so ClearSession forgets exactly them (plan decision 17) */
	inline const TCHAR* SessionSecretPrefix() { return TEXT("VaRest.Session."); }
}
```

- [ ] **Step 4: Slots that remember** — in `Source/VaRestHttp/Public/VaRestRedactor.h`, `FVaRestSecretSet`: `FPrevious` gains `FName Slot;` after `ExpiresAt`, and after `ClearAll()`:

```cpp
	/** Forgets the current and the previous values of every slot Matches accepts (a logout forgets the session's, plan decision 17) */
	void ClearSlots(TFunctionRef<bool(FName Slot)> Matches);
```

In `Source/VaRestHttp/Private/VaRestRedactor.cpp`, `Set` records the slot (`Previous.Add({ *Old, Now + PreviousLifetime, Slot });`), and:

```cpp
void FVaRestSecretSet::ClearSlots(TFunctionRef<bool(FName Slot)> Matches)
{
	for (auto It = Current.CreateIterator(); It; ++It)
	{
		if (Matches(It.Key()))
		{
			It.RemoveCurrent();
		}
	}
	Previous.RemoveAll([&Matches](const FPrevious& Old) { return Matches(Old.Slot); });
}
```

- [ ] **Step 5: The state** — in `Source/VaRestHttp/Private/VaRestClientState.h` add `#include "VaRestSession.h"`; in `FClientState` public:

```cpp
		/** Game thread: several secret slots, one republish (each SetSecret republishes) */
		void SetSecrets(TConstArrayView<TPair<FName, FString>> Values);

		// ---- Session (spec 4.2, plan decisions 5 and 6; VaRestSession.cpp). Game thread.
		const FVaRestSessionData& GetSessionData() const { return Session.Data; }
		FVaRestSessionChangedEvent& OnSessionChanged() { return Session.Changed; }
		FVaRestSessionExpiredEvent& OnSessionExpired() { return Session.Expired; }
		/** Data and secrets only, no event; RefreshToken null keeps the current one; ExpiresInSeconds 0 = unknown. True when anything changed. */
		bool UpdateTokens(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds);
		/** IN-9 marker and OnSessionChanged, after a change */
		void AnnounceSession(EVaRestSessionChange Change, uint64 CauseRequestId, const TCHAR* Text);
		/** SetBearerToken, SetSession, ImportSession: Replaced, or Refreshed with bSameIdentity (plan decision 5); no event when nothing changed */
		void SetTokensByGame(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, bool bSameIdentity);
		void SetSessionHeader(FStringView Name, FStringView Value);
		void RemoveSessionHeader(FStringView Name);
		/** Logout: tokens, headers and the session's secret slots; OnSessionChanged(Cleared). Nothing happens to an empty session. */
		void ClearSession();
		/** A rejected refresh (Task 13) or NotifySessionExpired: the tokens go, the headers stay; OnSessionExpired */
		void ExpireSession(uint64 CauseRequestId, FStringView Why);
		FString ExportSession() const;
		bool ImportSession(FStringView Json, bool bSameIdentity);
```

private: `FSessionState Session;` and `void PublishSessionSecrets();`. In `namespace VaRest::Http` beside `LogConfigError`:

```cpp
	/** Checks and de-duplicates redaction keys where they enter (task 14 review M4); VaRestClient.cpp */
	void AppendRedactKeys(TArray<FString>& Into, TConstArrayView<FString> Keys);
```

In `Source/VaRestHttp/Private/VaRestClient.cpp` move `ClientPrivate::AppendRedactKeys` (and `SameRedactKey`, which it uses) out of `ClientPrivate`: `SameRedactKey` stays in `ClientPrivate`, and the definition becomes `void VaRest::Http::AppendRedactKeys(TArray<FString>& Into, TConstArrayView<FString> Keys)` with the same body (calling `ClientPrivate::SameRedactKey`); `Make` and `WithRedactedKeys` call `VaRest::Http::AppendRedactKeys`.

In `Source/VaRestHttp/Private/VaRestClientState.cpp`:

```cpp
void VaRest::Http::FClientState::SetSecrets(TConstArrayView<TPair<FName, FString>> Values)
{
	check(IsInGameThread());
	const double Now = GetConfig()->Clock->Now();
	Secrets.Prune(Now);
	for (const TPair<FName, FString>& Value : Values)
	{
		Secrets.Set(Value.Key, Value.Value, Now);
	}
	EditConfig([](FClientConfig&) {});
	ScheduleSecretExpiry();
}
```

Create `Source/VaRestHttp/Private/VaRestSession.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"

#include "VaRestHttpLog.h"
#include "VaRestTracePoints.h"
#include "VaRestUrl.h"

namespace VaRest::Http::SessionPrivate
{
	FName Slot(const TCHAR* Name)
	{
		return FName(*(FString(SessionSecretPrefix()) + Name));
	}

	FName HeaderSlot(FStringView Header)
	{
		return FName(*(FString(SessionSecretPrefix()) + TEXT("Header.") + FString(Header)));
	}
}

void VaRest::Http::FClientState::PublishSessionSecrets()
{
	using namespace SessionPrivate;
	// IN-13 layer 3: an empty value retires a slot (its old value stays known for 10 minutes, Inspector D21)
	TArray<TPair<FName, FString>> Values;
	Values.Emplace(Slot(TEXT("AccessToken")), Session.Data.AccessToken);
	Values.Emplace(Slot(TEXT("RefreshToken")), Session.Data.RefreshToken);
	for (const FVaRestHeaders::FPair& Pair : Session.Data.Headers)
	{
		Values.Emplace(HeaderSlot(Pair.Key), Pair.Value);
	}
	SetSecrets(Values);
}

bool VaRest::Http::FClientState::UpdateTokens(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds)
{
	check(IsInGameThread());
	FVaRestSessionData& Data = Session.Data;
	const double ExpiresAt = FMath::IsFinite(ExpiresInSeconds) && ExpiresInSeconds > 0.0 ? GetConfig()->Clock->Now() + ExpiresInSeconds : 0.0;
	const bool bSame = Data.AccessToken.Equals(AccessToken, ESearchCase::CaseSensitive)
		&& (!RefreshToken || Data.RefreshToken.Equals(*RefreshToken, ESearchCase::CaseSensitive)) && Data.ExpiresAt == ExpiresAt;
	if (bSame)
	{
		return false;
	}
	Data.AccessToken = AccessToken;
	if (RefreshToken)
	{
		Data.RefreshToken = *RefreshToken;
	}
	Data.ExpiresAt = ExpiresAt;
	++Data.Generation;
	PublishSessionSecrets();
	return true;
}

void VaRest::Http::FClientState::AnnounceSession(EVaRestSessionChange Change, uint64 CauseRequestId, const TCHAR* Text)
{
	const EVaRestTraceMarkerKind Kind = Change == EVaRestSessionChange::Refreshed ? EVaRestTraceMarkerKind::SessionRefreshed
		: Change == EVaRestSessionChange::Replaced ? EVaRestTraceMarkerKind::SessionReplaced : EVaRestTraceMarkerKind::SessionCleared;
	TracePoints::Marker(*this, Kind, CauseRequestId, Text);
	// Last: a handler may change the session again (log in again from OnSessionChanged(Cleared))
	Session.Changed.Broadcast(Change);
}

void VaRest::Http::FClientState::SetTokensByGame(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, bool bSameIdentity)
{
	if (UpdateTokens(AccessToken, RefreshToken, ExpiresInSeconds))
	{
		AnnounceSession(bSameIdentity ? EVaRestSessionChange::Refreshed : EVaRestSessionChange::Replaced, 0, TEXT("set by the game"));
	}
}

void VaRest::Http::FClientState::SetSessionHeader(FStringView Name, FStringView Value)
{
	check(IsInGameThread());
	Session.Data.Headers.Set(Name, Value);
	++Session.Data.Generation;
	// IN-13 layer 2: the name is a key wherever it appears (a header, a query, a JSON body that echoes it)
	EditConfig([Name](FClientConfig& Edited) { AppendRedactKeys(Edited.RedactedKeys, TArray<FString>{ FString(Name) }); });
	PublishSessionSecrets();
}

void VaRest::Http::FClientState::RemoveSessionHeader(FStringView Name)
{
	check(IsInGameThread());
	if (!Session.Data.Headers.Remove(Name))
	{
		return;
	}
	++Session.Data.Generation;
	// Retired, not forgotten: its value stays known for 10 minutes (Inspector D21)
	SetSecrets(TArray<TPair<FName, FString>>{ TPair<FName, FString>(SessionPrivate::HeaderSlot(Name), FString()) });
}

void VaRest::Http::FClientState::ClearSession()
{
	check(IsInGameThread());
	FVaRestSessionData& Data = Session.Data;
	if (!Data.HasAccessToken() && !Data.HasRefreshToken() && Data.Headers.IsEmpty())
	{
		return;
	}
	const uint32 Generation = Data.Generation + 1;
	Data = FVaRestSessionData();
	Data.Generation = Generation;
	// Inspector 12, "cleared on logout": the session's values go now, previous ones included; the game's own slots stay
	const FString Prefix = SessionSecretPrefix();
	// FName text keeps the casing it was first made with, so the prefix compares ignoring case
	Secrets.ClearSlots([&Prefix](FName Slot) { return Slot.ToString().StartsWith(Prefix, ESearchCase::IgnoreCase); });
	EditConfig([](FClientConfig&) {});
	ScheduleSecretExpiry();
	AnnounceSession(EVaRestSessionChange::Cleared, 0, TEXT("ClearSession"));
}

void VaRest::Http::FClientState::ExpireSession(uint64 CauseRequestId, FStringView Why)
{
	check(IsInGameThread());
	FVaRestSessionData& Data = Session.Data;
	Data.AccessToken.Reset();
	Data.RefreshToken.Reset();
	Data.ExpiresAt = 0.0;
	++Data.Generation;
	PublishSessionSecrets();
	TracePoints::Marker(*this, EVaRestTraceMarkerKind::SessionExpired, CauseRequestId, Why);
	Session.Expired.Broadcast();
}

FString VaRest::Http::FClientState::ExportSession() const
{
	check(IsInGameThread());
	const FVaRestSessionData& Data = Session.Data;
	FVaRestJson Root = FVaRestJson::Object();
	Root.Set(TEXT("version"), 1);
	Root.Set(TEXT("accessToken"), Data.AccessToken);
	Root.Set(TEXT("refreshToken"), Data.RefreshToken);
	if (Data.ExpiresAt > 0.0)
	{
		// The client's clock is monotonic and per process: an export carries the expiry as a UTC time
		const double Left = Data.ExpiresAt - GetConfig()->Clock->Now();
		Root.Set(TEXT("expiresAt"), (FDateTime::UtcNow() + FTimespan::FromSeconds(Left)).ToIso8601());
	}
	FVaRestJson Headers = FVaRestJson::Array();
	for (const FVaRestHeaders::FPair& Pair : Data.Headers)
	{
		Headers.Add(FVaRestJson::Object().Set(TEXT("name"), Pair.Key).Set(TEXT("value"), Pair.Value));
	}
	Root.Set(TEXT("headers"), Headers);
	return Root.ToString();
}

bool VaRest::Http::FClientState::ImportSession(FStringView Json, bool bSameIdentity)
{
	check(IsInGameThread());
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(Json);
	if (Parsed.HasError())
	{
		return false;
	}
	const FVaRestJson Root = Parsed.StealValue();
	if (!Root.IsObject() || Root[TEXT("version")].AsInt(0) != 1)
	{
		return false;
	}
	const FString Access = Root[TEXT("accessToken")].AsString();
	const FString Refresh = Root[TEXT("refreshToken")].AsString();
	if (!IsValidHeaderValue(Access) || !IsValidHeaderValue(Refresh))
	{
		return false;
	}
	double ExpiresIn = 0.0;
	const FString ExpiresText = Root[TEXT("expiresAt")].AsString();
	if (!ExpiresText.IsEmpty())
	{
		FDateTime When;
		if (!FDateTime::ParseIso8601(*ExpiresText, When))
		{
			return false;
		}
		// Already expired: a moment from now, so the auth refreshes at once (NeedsRefresh), instead of "unknown"
		ExpiresIn = FMath::Max((When - FDateTime::UtcNow()).GetTotalSeconds(), 1e-3);
	}
	FVaRestHeaders Headers;
	for (const FVaRestJson& Entry : Root[TEXT("headers")].Elements())
	{
		const FString Name = Entry[TEXT("name")].AsString();
		const FString Value = Entry[TEXT("value")].AsString();
		if (!IsValidHeaderName(Name) || !IsValidHeaderValue(Value))
		{
			return false;
		}
		Headers.Set(Name, Value);
	}
	TArray<FString> Names;
	for (const FVaRestHeaders::FPair& Pair : Headers)
	{
		Names.Add(Pair.Key);
	}
	Session.Data.Headers = MoveTemp(Headers);
	++Session.Data.Generation;
	EditConfig([&Names](FClientConfig& Edited) { AppendRedactKeys(Edited.RedactedKeys, Names); });
	PublishSessionSecrets();
	SetTokensByGame(Access, &Refresh, ExpiresIn, bSameIdentity);
	return true;
}
```

(add `#include "Misc/DateTime.h"` and `#include "Misc/Timespan.h"`.)

- [ ] **Step 6: The public API** — in `Source/VaRestHttp/Public/VaRestClient.h` add `#include "VaRestAuthTypes.h"` and, after the configuration section:

```cpp
	// ---- session (spec 4.2; game thread). Live state: every attempt uses the session as it is when it starts (plan decision 6).
	/**
	 * The access token (a login, a token from elsewhere). Replaced, or Refreshed with bSameIdentity (plan decision 5); the
	 * refresh token stays, the expiry becomes unknown. An empty token is ClearSession(). A token holding a control character
	 * is refused with an Error log (it would go into a header line).
	 */
	void SetBearerToken(FStringView AccessToken, bool bSameIdentity = false);
	/** A login's result: both tokens and the access token's lifetime in seconds (0 = unknown) */
	void SetSession(FStringView AccessToken, FStringView RefreshToken, double ExpiresInSeconds = 0.0, bool bSameIdentity = false);
	/** dawg's X-Session-ID pattern: sent with every request whose auth mode is not None; its value is a secret, its name a key */
	void SetSessionHeader(FStringView Name, FStringView Value);
	void RemoveSessionHeader(FStringView Name);
	/** Logout: tokens, session headers and their secret values; OnSessionChanged(Cleared) */
	void ClearSession();
	FVaRestSessionData GetSession() const;
	/**
	 * Spec 4.2: the session as JSON, for a game that persists it itself. It holds the tokens in clear: store it like a
	 * password (the platform's keychain), never in a save file others can read.
	 */
	FString ExportSession() const;
	/** False, and nothing changes, when Json is not an ExportSession() result (or holds a control character in a token or a header) */
	bool ImportSession(FStringView Json, bool bSameIdentity = false);
	/** WebSocket spec 19 item 2: a socket's terminal auth failure. The tokens go; OnSessionExpired fires. */
	void NotifySessionExpired();
	/** Game thread. An empty client returns a delegate that never fires. */
	FVaRestSessionChangedEvent& OnSessionChanged();
	FVaRestSessionExpiredEvent& OnSessionExpired();
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
void FVaRestClient::SetBearerToken(FStringView AccessToken, bool bSameIdentity)
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return;
	}
	if (AccessToken.IsEmpty())
	{
		State->ClearSession();
		return;
	}
	if (!VaRest::Http::IsValidHeaderValue(AccessToken))
	{
		// The token is not in the line: it is a secret, and the reason is enough
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetBearerToken: the token holds CR, LF, NUL or another control character; not set"));
		return;
	}
	State->SetTokensByGame(FString(AccessToken), nullptr, 0.0, bSameIdentity);
}

void FVaRestClient::SetSession(FStringView AccessToken, FStringView RefreshToken, double ExpiresInSeconds, bool bSameIdentity)
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return;
	}
	if (!VaRest::Http::IsValidHeaderValue(AccessToken) || !VaRest::Http::IsValidHeaderValue(RefreshToken))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetSession: a token holds CR, LF, NUL or another control character; not set"));
		return;
	}
	const FString Refresh(RefreshToken);
	State->SetTokensByGame(FString(AccessToken), &Refresh, ExpiresInSeconds, bSameIdentity);
}

void FVaRestClient::SetSessionHeader(FStringView Name, FStringView Value)
{
	check(IsInGameThread());
	if (!VaRest::Http::IsValidHeaderName(Name))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetSessionHeader: the name is not a token (\"%s\"); not set"), *VaRest::Http::EscapeForLog(Name));
		return;
	}
	if (!VaRest::Http::IsValidHeaderValue(Value))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: SetSessionHeader: the value of \"%s\" holds a control character; not set"), *VaRest::Http::EscapeForLog(Name));
		return;
	}
	if (State.IsValid())
	{
		State->SetSessionHeader(Name, Value);
	}
}

void FVaRestClient::RemoveSessionHeader(FStringView Name)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->RemoveSessionHeader(Name);
	}
}

void FVaRestClient::ClearSession()
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->ClearSession();
	}
}

FVaRestSessionData FVaRestClient::GetSession() const
{
	check(IsInGameThread());
	return State.IsValid() ? State->GetSessionData() : FVaRestSessionData();
}

FString FVaRestClient::ExportSession() const
{
	return State.IsValid() ? State->ExportSession() : FString();
}

bool FVaRestClient::ImportSession(FStringView Json, bool bSameIdentity)
{
	return State.IsValid() && State->ImportSession(Json, bSameIdentity);
}

void FVaRestClient::NotifySessionExpired()
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->ExpireSession(0, TEXT("NotifySessionExpired"));
	}
}

FVaRestSessionChangedEvent& FVaRestClient::OnSessionChanged()
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		// Leaked, like every singleton here (review I-4)
		static FVaRestSessionChangedEvent* const Never = new FVaRestSessionChangedEvent();
		return *Never;
	}
	return State->OnSessionChanged();
}

FVaRestSessionExpiredEvent& FVaRestClient::OnSessionExpired()
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		static FVaRestSessionExpiredEvent* const Never = new FVaRestSessionExpiredEvent();
		return *Never;
	}
	return State->OnSessionExpired();
}
```

- [ ] **Step 7: Run them, and the redactor and log suites**

Run: `Tools/run_tests.sh VaRest.Http.Session` then `Tools/run_tests.sh --no-build VaRest.Http.Redactor` then `Tools/run_tests.sh --no-build VaRest.Http.Log`
Expected: `0 failed` each.

- [ ] **Step 8: Restore-the-bug** — in `ClearSession` call `Secrets.ClearAll()` instead of `ClearSlots`: `VaRest.Http.Session.Secrets` FAILS ("... and keeps the game's own"); restore; PASS. In `UpdateTokens` skip `PublishSessionSecrets()`: FAILS ("the tokens and the header value are scrubbed" -- the header slot still comes from `SetSessionHeader`, the tokens do not); restore; PASS.

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestAuthTypes.h Source/VaRestHttp/Private/VaRestSession.h Source/VaRestHttp/Private/VaRestSession.cpp \
  Source/VaRestHttp/Public/VaRestRedactor.h Source/VaRestHttp/Private/VaRestRedactor.cpp Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpSessionTest.cpp
git commit -m "http: M4 task 11 -- the session: tokens, expiry, session headers, change and expiry events, export/import, its secrets

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 12: `IVaRestAuth`, the built-in Bearer, ApiKey and Basic, and the `AuthApply` stage (spec §4.2; WebSocket §19 item 2)

Plan decisions 4, 6, 19, 23. The interface the socket shares, three of spec §4.2's four built-ins (BearerRefresh is Task 14), the per-attempt stage that writes session headers and credentials into each attempt, and the request's auth mode. The refresh context is defined here because the interface names it; Task 13 uses it.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestAuth.h`, `Source/VaRestHttp/Private/VaRestAuthInternal.h`, `Source/VaRestHttp/Private/VaRestAuth.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (`AuthMode()`, `NoAuth()`, the private `MarkAuthRefresh()`; `WithAuth`, `WithoutAuth`, `WithBearer`, `ApplyAuth`)
- Modify: `Source/VaRestHttp/Public/VaRestStage.h`, `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`GetAttemptUrl`; the boundary and the transport use it)
- Modify: `Source/VaRestHttp/Private/VaRestExchangeState.h`, `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestLogStage.cpp` (the attempt URL), `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (flags `NoAuth`, `AuthRefresh`; `AuthWait`, `AuthRefreshId`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`

**Interfaces:**
- Consumes: Task 11 (the session, `EVaRestAuthMode`, `AppendRedactKeys`, `SetSecrets`).
- Produces (public, `VaRestAuth.h`): `struct FVaRestAuthTarget { FVaRestAuthTarget(FVaRestHeaders&, TArray<TPair<FString, FString>>&, const FVaRestSessionData&); FVaRestHeaders& Headers; TArray<TPair<FString, FString>>& Query; const FVaRestSessionData& Session; uint64 RequestId = 0; }`; `enum class EVaRestAuthRefreshOutcome : uint8 { Refreshed, Rejected, Unreachable }`; `struct FVaRestAuthRefreshResult { EVaRestAuthRefreshOutcome Outcome; FString AccessToken; FString RefreshToken; double ExpiresInSeconds; EVaRestErrorKind UnreachableKind; FString Message; static Refreshed(FString, FString = {}, double = 0), Rejected(FString), Unreachable(EVaRestErrorKind, FString); }`; `class FVaRestAuthRefreshContext { FVaRestAuthRefreshContext(FVaRestClient, FVaRestSessionData, uint64 RefreshId); template <typename T = FVaRestJson> TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const; const FVaRestSessionData& GetSession() const; uint64 GetRefreshId() const; }`; `class IVaRestAuth { virtual FName GetAuthName() const = 0; virtual bool Apply(FVaRestAuthTarget&) const = 0; virtual bool CanRefresh(const FVaRestSessionData&) const; virtual bool NeedsRefresh(const FVaRestSessionData&, double Now) const; virtual bool IsAuthFailure(int32 Status) const; virtual void Refresh(const FVaRestAuthRefreshContext&, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done); virtual void GetRedactedKeys(TArray<FString>&) const; virtual void GetSecretValues(TArray<TPair<FName, FString>>&) const; }`; `class FVaRestAuth { using FRef = TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>; static FRef Bearer(); static FRef ApiKey(EVaRestApiKeyPlacement, FStringView Name, FStringView Key); static FRef Basic(FStringView User, FStringView Password); }` (Task 14 adds `BearerRefresh`). Builder: `AuthMode(EVaRestAuthMode InMode) &&`, `NoAuth() &&`. Client: `FVaRestClient& WithAuth(TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth)`, `FVaRestClient& WithoutAuth()`, `FVaRestClient& WithBearer(FStringView Token)`, `bool ApplyAuth(FVaRestHeaders& Headers, TArray<TPair<FString, FString>>& Query) const`. `const FString& FVaRestExchange::GetAttemptUrl() const`. Private: `FClientConfig::Auth` (`TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe>`); on `FExchangeState`: `EVaRestAuthMode AuthMode`, `bool bIsAuthRefresh`, `FString AttemptUrl`, `uint32 AppliedGeneration`, `double AuthWait`, `uint64 AuthRefreshId`; `static FVaRestClient FClientAccess::Wrap(TSharedPtr<FClientState, ESPMode::ThreadSafe>)`; in `VaRestAuthInternal.h`: `bool VaRest::Http::ApplyCredentials(const IVaRestAuth* Auth, FVaRestAuthTarget& Target)`, `FVaRestStageRef VaRest::Http::MakeAuthApplyStage()`. The chain: custom Outer → Retry → boundary → **AuthApply** → custom PerAttempt → log → limiter → transport.

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::AuthTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** The index of the Nth call whose URL path is Path (the query ignored), or INDEX_NONE */
	int32 FindCall(const VaRest::Http::Testing::FTestRig& Rig, const TCHAR* Path, int32 Nth = 0)
	{
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			FString Url = Rig.Fake->GetCall(Index).Request.Url;
			int32 Query = INDEX_NONE;
			if (Url.FindChar(TEXT('?'), Query))
			{
				Url.LeftInline(Query);
			}
			if (Url.EndsWith(Path) && Nth-- == 0)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Drains a rig whose test stopped early */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}

	/** Records the URL each PerAttempt pass sees: the final one, auth query included (plan decision 19) */
	class FUrlProbe final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("UrlProbe"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Seen->Add(Exchange->GetAttemptUrl());
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
		TSharedRef<TArray<FString>, ESPMode::ThreadSafe> Seen = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerTest, "VaRest.Http.Auth.Bearer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	Rig.Client.WithBearer(TEXT("bearer-token-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("sid-1234567890"));
	auto Plain = MakeOutcome<FVaRestJson>();
	auto Login = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Plain).Send();
	Observe(Rig.Client.Post(TEXT("/login")).NoAuth().Body(TEXT("x")), Login).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Plain, Login] { return Plain->IsDone() && Login->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	const int32 A = FindCall(Rig, TEXT("/a"));
	const int32 L = FindCall(Rig, TEXT("/login"));
	if (!TestTrue(TEXT("both sent"), A != INDEX_NONE && L != INDEX_NONE))
	{
		return false;
	}
	const FVaRestHeaders& Sent = Rig.Fake->GetCall(A).Request.Headers;
	TestEqualSensitive(TEXT("spec 4.2 Bearer: the session's token"), Sent.Get(TEXT("Authorization")), FString(TEXT("Bearer bearer-token-123456")));
	TestEqualSensitive(TEXT("... and the session header"), Sent.Get(TEXT("X-Session-ID")), FString(TEXT("sid-1234567890")));
	const FVaRestHeaders& Bare = Rig.Fake->GetCall(L).Request.Headers;
	TestTrue(TEXT("NoAuth(): neither"), !Bare.Contains(TEXT("Authorization")) && !Bare.Contains(TEXT("X-Session-ID")));

	Rig.Client.ClearSession();
	auto Required = MakeOutcome<FVaRestJson>();
	auto Optional = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/r")), Required).Send();
	Observe(Rig.Client.Get(TEXT("/o")).AuthMode(EVaRestAuthMode::IfAvailable), Optional).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Required, Optional] { return Required->IsDone() && Optional->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("plan decision 4: Required without credentials is Kind Auth, not retryable, saying what to do"), Required->Error.Kind == EVaRestErrorKind::Auth
		&& !Required->Error.bRetryable && Required->Error.Message.Contains(TEXT("no credentials")));
	TestEqual(TEXT("... and is never sent"), FindCall(Rig, TEXT("/r")), INDEX_NONE);
	const int32 O = FindCall(Rig, TEXT("/o"));
	TestTrue(TEXT("IfAvailable: sent without credentials"), Optional->SuccessCalls.load() == 1 && O != INDEX_NONE
		&& !Rig.Fake->GetCall(O).Request.Headers.Contains(TEXT("Authorization")));

	FTestRig NoAuth;
	auto Unchanged = MakeOutcome<FVaRestJson>();
	Observe(NoAuth.Client.Get(TEXT("/p")), Unchanged).Send();
	TestTrue(TEXT("a client with no auth sends as in M3"), NoAuth.Wait(Unchanged) && Unchanged->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthKeysTest, "VaRest.Http.Auth.ApiKeyAndBasic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthKeysTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	const FString Key = TEXT("api-key-value-1234");
	FTestRig Header;
	Header.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-Api-Key"), Key));
	FTestRig Query;
	const TSharedRef<FUrlProbe, ESPMode::ThreadSafe> Probe = MakeShared<FUrlProbe, ESPMode::ThreadSafe>();
	Query.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT("app_ref"), Key)).Use(Probe, EVaRestStageSlot::PerAttempt)
		.WithLogLevel(EVaRestLogLevel::Headers);
	FTestRig Basic;
	Basic.Client.WithAuth(FVaRestAuth::Basic(TEXT("neo"), TEXT("trinity-password")));
	FLogCapture Capture;
	auto H = MakeOutcome<FVaRestJson>();
	auto Q = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Header.Client.Get(TEXT("/x")), H).Send();
	Observe(Query.Client.Get(TEXT("/x")).Query(TEXT("page"), 2), Q).Send();
	Observe(Basic.Client.Get(TEXT("/x")), B).Send();
	if (!TestTrue(TEXT("all complete"), PumpUntil([H, Q, B] { return H->IsDone() && Q->IsDone() && B->IsDone(); }))
		|| !TestTrue(TEXT("one call each"), Header.Fake->NumCalls() == 1 && Query.Fake->NumCalls() == 1 && Basic.Fake->NumCalls() == 1))
	{
		Abandon(Header);
		Abandon(Query);
		Abandon(Basic);
		return false;
	}
	TestEqualSensitive(TEXT("ApiKey in a header"), Header.Fake->GetCall(0).Request.Headers.Get(TEXT("X-Api-Key")), Key);
	const FString QueryUrl = Query.Fake->GetCall(0).Request.Url;
	TestEqualSensitive(TEXT("ApiKey in the query, after the request's own pairs"), QueryUrl, FString(TEXT("https://api.test/x?page=2&app_ref=api-key-value-1234")));
	TestTrue(TEXT("a PerAttempt stage sees the final URL (GetAttemptUrl)"), Probe->Seen->Num() == 1 && (*Probe->Seen)[0] == QueryUrl);
	TestEqualSensitive(TEXT("Basic: base64 of user:password"), Basic.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),
		FString(TEXT("Basic bmVvOnRyaW5pdHktcGFzc3dvcmQ=")));
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> QueryRedactor = VaRest::Http::FClientAccess::State(Query.Client)->GetRedactor();
	// "app_ref" matches no built-in rule (VaRestJsonRedact.h), so only the auth's added key redacts it
	TestTrue(TEXT("IN-13 layer 2: the query name is a key"), QueryRedactor->GetRules().ShouldRedact(FStringView(TEXT("app_ref")), EVaRestRedactKeyKind::Query));
	TestEqualSensitive(TEXT("layer 3: the key is a secret"), QueryRedactor->ScrubText(Key), FString(TEXT("***")));
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> BasicRedactor = VaRest::Http::FClientAccess::State(Basic.Client)->GetRedactor();
	TestEqualSensitive(TEXT("... so are the Basic credential and the password"), BasicRedactor->ScrubText(TEXT("bmVvOnRyaW5pdHktcGFzc3dvcmQ= trinity-password")),
		FString(TEXT("*** ***")));
	TestFalse(TEXT("no log line holds the key"), Capture.GetLines().ContainsByPredicate([&Key](const FString& Line) { return Line.Contains(Key); }));
	TestTrue(TEXT("... the request line shows it redacted"), Capture.Contains(TEXT("app_ref=***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthApplyTest, "VaRest.Http.Auth.ApplyToHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthApplyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// WebSocket spec 19 item 2 / 3.3: a socket applies the client's session and auth to its handshake
	FTestRig Rig;
	Rig.Client.WithBearer(TEXT("bearer-token-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("sid-1234567890"));
	FVaRestHeaders Headers;
	TArray<TPair<FString, FString>> Query;
	TestTrue(TEXT("applied"), Rig.Client.ApplyAuth(Headers, Query));
	TestTrue(TEXT("... the token and the session header"), Headers.Get(TEXT("Authorization")) == TEXT("Bearer bearer-token-123456")
		&& Headers.Get(TEXT("X-Session-ID")) == TEXT("sid-1234567890") && Query.Num() == 0);
	Rig.Client.ClearSession();
	FVaRestHeaders After;
	TestFalse(TEXT("no credentials: false"), Rig.Client.ApplyAuth(After, Query));
	FTestRig Plain;
	FVaRestHeaders Nothing;
	TestTrue(TEXT("no auth at all: true, nothing written"), Plain.Client.ApplyAuth(Nothing, Query) && Nothing.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthInvalidKeyTest, "VaRest.Http.Auth.InvalidApiKey",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthInvalidKeyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	AddExpectedErrorPlain(TEXT("VaRest: FVaRestAuth::ApiKey: the header name"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("Bad Name"), TEXT("k")));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/x")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("an auth that cannot apply: Kind Auth, nothing sent"), Outcome->Error.Kind == EVaRestErrorKind::Auth && Rig.Fake->NumCalls() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthFlagsTest, "VaRest.Http.Auth.TraceFlags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthFlagsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Client.WithBearer(TEXT("bearer-token-123456"));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto With = MakeOutcome<FVaRestJson>();
	auto Without = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle A = Observe(Rig.Client.Get(TEXT("/a")), With).Send();
	const FVaRestRequestHandle B = Observe(Rig.Client.Get(TEXT("/b")).NoAuth(), Without).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([With, Without] { return With->IsDone() && Without->IsDone(); })))
	{
		return false;
	}
	const auto FlagsOf = [&Scoped](uint64 Id)
	{
		for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::Started)
			{
				return Event.Payload.Get<FVaRestTraceStarted>().Flags;
			}
		}
		return EVaRestTraceRequestFlags::Partial;
	};
	TestFalse(TEXT("IN-1: an authenticated request has no NoAuth flag"), EnumHasAnyFlags(FlagsOf(A.GetId()), EVaRestTraceRequestFlags::NoAuth | EVaRestTraceRequestFlags::Partial));
	TestTrue(TEXT("... NoAuth() has it"), EnumHasAnyFlags(FlagsOf(B.GetId()), EVaRestTraceRequestFlags::NoAuth));
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Auth`
Expected: FAIL to compile (`VaRestAuth.h` not found).

- [ ] **Step 3: The interface** — create `Source/VaRestHttp/Public/VaRestAuth.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestClient.h"

/**
 * What an auth writes into (WebSocket spec 19 item 2): one HTTP attempt, or one WebSocket handshake. Game thread; valid
 * only during IVaRestAuth::Apply.
 */
struct FVaRestAuthTarget
{
	FVaRestAuthTarget(FVaRestHeaders& InHeaders, TArray<TPair<FString, FString>>& InQuery, const FVaRestSessionData& InSession)
		: Headers(InHeaders)
		, Query(InQuery)
		, Session(InSession)
	{
	}

	/** Credential headers go here, with Set: a header of the same name the request set is replaced */
	FVaRestHeaders& Headers;
	/** Pairs appended to the URL, each percent-encoded by the core (an API key in the query) */
	TArray<TPair<FString, FString>>& Query;
	/** The client's session as it is when this attempt starts (plan decision 6) */
	const FVaRestSessionData& Session;
	/** The request (IN-10); 0 for a socket handshake */
	uint64 RequestId = 0;
};

/** How a refresh ended (plan decision 3) */
enum class EVaRestAuthRefreshOutcome : uint8
{
	/** New credentials */
	Refreshed,
	/** The server said no: the session expires, parked requests fail Kind=Auth */
	Rejected,
	/** The server could not be asked (connection, timeout, 408, 429, 5xx): the session stays, parked requests fail with UnreachableKind */
	Unreachable
};

/** What IVaRestAuth::Refresh reports */
struct VARESTHTTP_API FVaRestAuthRefreshResult
{
	EVaRestAuthRefreshOutcome Outcome = EVaRestAuthRefreshOutcome::Rejected;
	FString AccessToken;
	/** Empty: keep the current refresh token (an OAuth2 server need not rotate it) */
	FString RefreshToken;
	/** 0 = unknown */
	double ExpiresInSeconds = 0.0;
	/** Unreachable: Connection or Timeout, the kind parked requests fail with (retryable) */
	EVaRestErrorKind UnreachableKind = EVaRestErrorKind::Connection;
	/** Why, for the error message and the trace marker; never a token */
	FString Message;

	static FVaRestAuthRefreshResult Refreshed(FString InAccessToken, FString InRefreshToken = FString(), double InExpiresInSeconds = 0.0);
	static FVaRestAuthRefreshResult Rejected(FString InMessage);
	static FVaRestAuthRefreshResult Unreachable(EVaRestErrorKind InKind, FString InMessage);
};

/** Handed to IVaRestAuth::Refresh: the client, its session, and the one way to send the refresh request (plan decision 23). Game thread. */
class VARESTHTTP_API FVaRestAuthRefreshContext
{
public:
	FVaRestAuthRefreshContext(FVaRestClient InClient, FVaRestSessionData InSession, uint64 InRefreshId);

	/**
	 * A refresh request on the client: it bypasses the auth stages (no credentials, no park, no replay), the envelope and
	 * retry; it goes Highest in the limiter and carries trace flag AuthRefresh. A refresh sent through the client itself
	 * would park behind its own refresh.
	 */
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const
	{
		return Client.Request<T>(Verb, Path).MarkAuthRefresh();
	}

	/** The session the refresh started from (its refresh token) */
	const FVaRestSessionData& GetSession() const { return Session; }
	/** IN-2 AuthRefreshId */
	uint64 GetRefreshId() const { return RefreshId; }

private:
	FVaRestClient Client;
	FVaRestSessionData Session;
	uint64 RefreshId = 0;
};

/**
 * Spec 4.2 / WebSocket spec 19 item 2: how a client authenticates. Every function runs on the game thread. Apply runs once
 * per attempt (and once per socket handshake) with the session as it is then; the core shares one Refresh among every
 * request that needs it (single-flight, Task 13). Configure with FVaRestClient::WithAuth.
 */
class IVaRestAuth
{
public:
	virtual ~IVaRestAuth() = default;

	/** For logs, errors and traces */
	virtual FName GetAuthName() const = 0;

	/** This auth's credentials into Target; false when it has none (Kind=Auth under EVaRestAuthMode::Required) */
	virtual bool Apply(FVaRestAuthTarget& Target) const = 0;

	/** Refresh() can get new credentials for this session (a refresh token is there) */
	virtual bool CanRefresh(const FVaRestSessionData& Session) const { return false; }

	/** Refresh before the next attempt: no access token yet, or a known expiry within the lead time (spec 4.2: 30 s) */
	virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const { return false; }

	/** A status that means "credentials refused": the request refreshes and replays once (spec 4.2: 401, configurable) */
	virtual bool IsAuthFailure(int32 Status) const { return Status == 401; }

	/**
	 * Gets new credentials. Call Done exactly once, on the game thread (later is fine; a second call is ignored; a call from
	 * another thread is moved to the game thread). Send the refresh request with Context.Request(), never through the client.
	 */
	virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done)
	{
		Done(FVaRestAuthRefreshResult::Rejected(TEXT("this auth cannot refresh")));
	}

	/** IN-13 layer 2: key names this auth puts secrets under (an API key's header or query name, a token path's last segment) */
	virtual void GetRedactedKeys(TArray<FString>& OutKeys) const {}

	/** IN-13 layer 3: secret values this auth holds itself (an API key, Basic credentials); the session's tokens are the core's */
	virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const {}
};

/** Spec 4.2's built-in auths */
class VARESTHTTP_API FVaRestAuth
{
public:
	using FRef = TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>;

	/** Authorization: Bearer <the session's access token> (SetBearerToken, SetSession); WithBearer is the short form */
	static FRef Bearer();
	/**
	 * The key in a header or in the query. A header name that is not an RFC 9110 token, or a key with a control character,
	 * logs an Error, and the auth applies nothing (Kind=Auth under Required).
	 */
	static FRef ApiKey(EVaRestApiKeyPlacement Placement, FStringView Name, FStringView Key);
	/** Authorization: Basic base64(user:password), UTF-8 */
	static FRef Basic(FStringView User, FStringView Password);
};
```

- [ ] **Step 4: The builder's auth options** — in `Source/VaRestHttp/Public/VaRestClient.h`: forward-declare `class IVaRestAuth;` and `class FVaRestAuthRefreshContext;` with the other forward declarations; `FRequestSpec` fields after `bNoEnvelope`:

```cpp
		/** AuthMode() / NoAuth() (plan decision 4) */
		EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
		/** A refresh request (FVaRestAuthRefreshContext::Request; plan decision 23) */
		bool bAuthRefresh = false;
```

`TVaRestRequest` public methods after `NoEnvelope()`:

```cpp
	/** Spec 4.2 / OpenAPI N15 (plan decision 4): Required (the default), IfAvailable, or None */
	TVaRestRequest AuthMode(EVaRestAuthMode InMode) &&
	{
		Spec->AuthMode = InMode;
		return MoveTemp(*this);
	}

	/** Spec 3.2: no credentials, no session headers, no refresh -- a login request */
	TVaRestRequest NoAuth() &&
	{
		return MoveTemp(*this).AuthMode(EVaRestAuthMode::None);
	}
```

and in its `private:` section:

```cpp
	friend class ::FVaRestAuthRefreshContext;

	/** Plan decision 23: a refresh request bypasses the auth stages, the envelope and retry, and goes first in the limiter */
	TVaRestRequest MarkAuthRefresh() &&
	{
		Spec->bAuthRefresh = true;
		Spec->AuthMode = EVaRestAuthMode::None;
		Spec->Envelope.Reset();
		Spec->bNoEnvelope = true;
		Spec->Retry = FVaRestRetryPolicy::Disabled();
		Spec->Priority = EVaRestPriority::Highest;
		return MoveTemp(*this);
	}
```

`FVaRestClient`, after the session section:

```cpp
	// ---- auth (spec 4.2; game thread)
	/** This client's auth: FVaRestAuth::Bearer(), ApiKey(), Basic(), BearerRefresh(), or an IVaRestAuth of your own */
	FVaRestClient& WithAuth(TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth);
	FVaRestClient& WithoutAuth();
	/** Spec 3.1: WithAuth(FVaRestAuth::Bearer()) and SetBearerToken(Token) */
	FVaRestClient& WithBearer(FStringView Token);
	/**
	 * WebSocket spec 19 item 2: the session headers, then this client's auth, into Headers and Query (a socket handshake).
	 * False when the client has an auth and it has no credentials; true with no auth at all.
	 */
	bool ApplyAuth(FVaRestHeaders& Headers, TArray<TPair<FString, FString>>& Query) const;
```

In `Source/VaRestHttp/Private/VaRestClient.cpp` (`#include "VaRestAuth.h"`, `#include "VaRestAuthInternal.h"`): `FRequestSpecAccess::Send` copies `Ex.AuthMode = Spec->AuthMode; Ex.bIsAuthRefresh = Spec->bAuthRefresh;`, and:

```cpp
FVaRestClient& FVaRestClient::WithAuth(TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth)
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return *this;
	}
	TArray<FString> Keys;
	InAuth->GetRedactedKeys(Keys);
	State->EditConfig([&InAuth, &Keys](VaRest::Http::FClientConfig& Edited)
	{
		Edited.Auth = InAuth;
		// IN-13 layer 2: the auth's key names, checked once where they enter (task 14 review M4)
		VaRest::Http::AppendRedactKeys(Edited.RedactedKeys, Keys);
	});
	TArray<TPair<FName, FString>> Values;
	InAuth->GetSecretValues(Values);
	if (Values.Num() > 0)
	{
		State->SetSecrets(Values);
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithoutAuth()
{
	if (State.IsValid())
	{
		State->EditConfig([](VaRest::Http::FClientConfig& Edited) { Edited.Auth.Reset(); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithBearer(FStringView Token)
{
	WithAuth(FVaRestAuth::Bearer());
	SetBearerToken(Token);
	return *this;
}

bool FVaRestClient::ApplyAuth(FVaRestHeaders& Headers, TArray<TPair<FString, FString>>& Query) const
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return true;
	}
	FVaRestAuthTarget Target(Headers, Query, State->GetSessionData());
	return VaRest::Http::ApplyCredentials(State->GetConfig()->Auth.Get(), Target);
}
```

- [ ] **Step 5: The state** — `Source/VaRestHttp/Private/VaRestClientState.h`: `FClientConfig` gains `TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> Auth;` (`#include "VaRestAuth.h"`); `FClientAccess` gains

```cpp
		/** A handle on a state, for the refresh context (Task 13) */
		static FVaRestClient Wrap(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState) { return FVaRestClient(MoveTemp(InState)); }
```

`Source/VaRestHttp/Private/VaRestExchangeState.h`, after the envelope block:

```cpp
		// Auth (Tasks 12-13), from Send:
		EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
		/** The refresh request itself (plan decision 23): the auth stages pass it through */
		bool bIsAuthRefresh = false;
		/** The URL of the current attempt: Url plus the auth's query pairs (AuthApply) */
		FString AttemptUrl;
		/** The session generation the current attempt's credentials came from (AuthGate compares it, Task 13) */
		uint32 AppliedGeneration = 0;
		/** IN-2: seconds the next attempt waited behind a refresh, and that refresh's id; reset after the dispatch */
		double AuthWait = 0.0;
		uint64 AuthRefreshId = 0;
```

`Source/VaRestHttp/Public/VaRestStage.h`, `FVaRestExchange`, after `GetUrl()`:

```cpp
	/** The current attempt's URL: GetUrl() plus an auth's query pairs (an API key), from the auth stage on (plan decision 19) */
	const FString& GetAttemptUrl() const;
```

defined in `VaRestPipeline.cpp` beside the other accessors (`const FString& FVaRestExchange::GetAttemptUrl() const { return Impl->AttemptUrl; }`).

- [ ] **Step 6: The stage and the built-ins** — create `Source/VaRestHttp/Private/VaRestAuthInternal.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuth.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** The session headers, then Auth (it wins on a name both set); true when Auth applied credentials or there is none */
	bool ApplyCredentials(const IVaRestAuth* Auth, FVaRestAuthTarget& Target);

	/** Spec 4.2, per attempt: the first built-in PerAttempt stage, before the custom ones (they sign the final headers) */
	FVaRestStageRef MakeAuthApplyStage();
}
```

Create `Source/VaRestHttp/Private/VaRestAuth.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuthInternal.h"

#include "Misc/Base64.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestUrl.h"

FVaRestAuthRefreshResult FVaRestAuthRefreshResult::Refreshed(FString InAccessToken, FString InRefreshToken, double InExpiresInSeconds)
{
	FVaRestAuthRefreshResult Out;
	Out.Outcome = EVaRestAuthRefreshOutcome::Refreshed;
	Out.AccessToken = MoveTemp(InAccessToken);
	Out.RefreshToken = MoveTemp(InRefreshToken);
	Out.ExpiresInSeconds = InExpiresInSeconds;
	return Out;
}

FVaRestAuthRefreshResult FVaRestAuthRefreshResult::Rejected(FString InMessage)
{
	FVaRestAuthRefreshResult Out;
	Out.Outcome = EVaRestAuthRefreshOutcome::Rejected;
	Out.Message = MoveTemp(InMessage);
	return Out;
}

FVaRestAuthRefreshResult FVaRestAuthRefreshResult::Unreachable(EVaRestErrorKind InKind, FString InMessage)
{
	FVaRestAuthRefreshResult Out;
	Out.Outcome = EVaRestAuthRefreshOutcome::Unreachable;
	Out.UnreachableKind = InKind == EVaRestErrorKind::Timeout ? EVaRestErrorKind::Timeout : EVaRestErrorKind::Connection;
	Out.Message = MoveTemp(InMessage);
	return Out;
}

FVaRestAuthRefreshContext::FVaRestAuthRefreshContext(FVaRestClient InClient, FVaRestSessionData InSession, uint64 InRefreshId)
	: Client(MoveTemp(InClient))
	, Session(MoveTemp(InSession))
	, RefreshId(InRefreshId)
{
}

namespace VaRest::Http::AuthPrivate
{
	class FBearerAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("Bearer"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!Target.Session.HasAccessToken())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
			return true;
		}
	};

	class FApiKeyAuth final : public IVaRestAuth
	{
	public:
		FApiKeyAuth(EVaRestApiKeyPlacement InPlacement, FString InName, FString InKey, bool bInUsable)
			: Placement(InPlacement)
			, Name(MoveTemp(InName))
			, Key(MoveTemp(InKey))
			, bUsable(bInUsable)
		{
		}

		virtual FName GetAuthName() const override { return TEXT("ApiKey"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!bUsable || Key.IsEmpty())
			{
				return false;
			}
			if (Placement == EVaRestApiKeyPlacement::Header)
			{
				Target.Headers.Set(Name, Key);
			}
			else
			{
				Target.Query.Emplace(Name, Key);
			}
			return true;
		}

		virtual void GetRedactedKeys(TArray<FString>& OutKeys) const override
		{
			OutKeys.Add(Name);
		}

		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("VaRest.Auth.ApiKey"), Key);
		}

	private:
		EVaRestApiKeyPlacement Placement;
		FString Name;
		FString Key;
		bool bUsable;
	};

	class FBasicAuth final : public IVaRestAuth
	{
	public:
		FBasicAuth(FString InCredential, FString InPassword)
			: Credential(MoveTemp(InCredential))
			, Password(MoveTemp(InPassword))
		{
		}

		virtual FName GetAuthName() const override { return TEXT("Basic"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Basic ")) + Credential);
			return true;
		}

		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("VaRest.Auth.Basic"), Credential);
			OutValues.Emplace(TEXT("VaRest.Auth.BasicPassword"), Password);
		}

	private:
		/** base64(user:password) */
		FString Credential;
		FString Password;
	};

	/** Spec 4.2, per attempt (plan decisions 4, 6) */
	class FAuthApplyStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("AuthApply"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			if (Ex.AuthMode == EVaRestAuthMode::None || Ex.bIsAuthRefresh)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			const FVaRestSessionData& Session = Ex.State->GetSessionData();
			TArray<TPair<FString, FString>> AuthQuery;
			FVaRestAuthTarget Target(Ex.AttemptHeaders, AuthQuery, Session);
			Target.RequestId = Ex.RequestId;
			const IVaRestAuth* Auth = Ex.Config->Auth.Get();
			if (!ApplyCredentials(Auth, Target) && Ex.AuthMode == EVaRestAuthMode::Required)
			{
				if (Ex.LogLevel != EVaRestLogLevel::None && UE_LOG_ACTIVE(LogVaRestHttp, Warning))
				{
					UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: %s %s -> no credentials (%s); not sent"), Ex.RequestId, *EscapeForLog(Ex.Verb),
						*EscapeForLog(Ex.State->GetRedactor()->RedactUrl(Ex.Url)), *Auth->GetAuthName().ToString());
				}
				FVaRestAttemptOutcome Refused;
				Refused.Failure = EVaRestTransportFailure::Other;
				Refused.FailureDetail = TEXT("NoCredentials");
				Refused.FailureMessage = FString::Printf(TEXT("the client's auth (%s) has no credentials: log in first, or send with AuthMode(IfAvailable) or NoAuth()"),
					*Auth->GetAuthName().ToString());
				OnOutcome(MoveTemp(Refused));
				return;
			}
			Ex.AppliedGeneration = Session.Generation;
			if (AuthQuery.Num() > 0)
			{
				AppendQuery(Ex.AttemptUrl, AuthQuery);
			}
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};
}

bool VaRest::Http::ApplyCredentials(const IVaRestAuth* Auth, FVaRestAuthTarget& Target)
{
	for (const FVaRestHeaders::FPair& Pair : Target.Session.Headers)
	{
		Target.Headers.Set(Pair.Key, Pair.Value);
	}
	return !Auth || Auth->Apply(Target);
}

FVaRestStageRef VaRest::Http::MakeAuthApplyStage()
{
	return MakeShared<AuthPrivate::FAuthApplyStage, ESPMode::ThreadSafe>();
}

FVaRestAuth::FRef FVaRestAuth::Bearer()
{
	return MakeShared<VaRest::Http::AuthPrivate::FBearerAuth, ESPMode::ThreadSafe>();
}

FVaRestAuth::FRef FVaRestAuth::ApiKey(EVaRestApiKeyPlacement Placement, FStringView Name, FStringView Key)
{
	bool bUsable = !Name.IsEmpty();
	if (Placement == EVaRestApiKeyPlacement::Header && !VaRest::Http::IsValidHeaderName(Name))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ApiKey: the header name \"%s\" is not an RFC 9110 token; the auth applies nothing"),
			*VaRest::Http::EscapeForLog(Name));
		bUsable = false;
	}
	else if (!VaRest::Http::IsValidHeaderValue(Key))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ApiKey: the key holds a control character; the auth applies nothing"));
		bUsable = false;
	}
	return MakeShared<VaRest::Http::AuthPrivate::FApiKeyAuth, ESPMode::ThreadSafe>(Placement, FString(Name), FString(Key), bUsable);
}

FVaRestAuth::FRef FVaRestAuth::Basic(FStringView User, FStringView Password)
{
	// RFC 7617 2.1: user-id ":" password, UTF-8 (the charset the RFC recommends), base64
	const FString Pair = FString(User) + TEXT(":") + FString(Password);
	const FTCHARToUTF8 Utf8(*Pair);
	const FString Credential = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<uint32>(Utf8.Length()));
	return MakeShared<VaRest::Http::AuthPrivate::FBasicAuth, ESPMode::ThreadSafe>(Credential, FString(Password));
}
```

- [ ] **Step 7: The chain, the attempt URL, the trace** — `Source/VaRestHttp/Private/VaRestClientState.cpp`, `Publish`: right after `NewChain->Add(MakeAttemptBoundaryStage());` add

```cpp
	// Spec 4.2: credentials per attempt, before the custom PerAttempt stages, which then sign the final headers
	NewChain->Add(MakeAuthApplyStage());
```

(`#include "VaRestAuthInternal.h"`). `Source/VaRestHttp/Private/VaRestPipeline.cpp`: `FAttemptBoundaryStage::Process` gains `Ex.AttemptUrl = Ex.Url;` after the headers copy; in `FTransportStage::Process`, `Request.Url = Ex.Url;` becomes `Request.Url = Ex.AttemptUrl;`, and after the `Ex.NextGap` reset Task 7 added:

```cpp
				Ex.AuthWait = 0.0;
				Ex.AuthRefreshId = 0;
```

`Source/VaRestHttp/Private/VaRestLogStage.cpp`: in `FLogStage::Process` and `LogOutcome`, `ShownUrl(Redactor, Ex.Url)` becomes `ShownUrl(Redactor, Ex.AttemptUrl)` (the URL the attempt went to; an auth query is redacted by its key). `Source/VaRestHttp/Private/VaRestTracePoints.cpp`: in `Describe`, after the `Idempotent` flag:

```cpp
	if (Ex.AuthMode == EVaRestAuthMode::None)
	{
		Out.Flags |= EVaRestTraceRequestFlags::NoAuth;
	}
	if (Ex.bIsAuthRefresh)
	{
		Out.Flags |= EVaRestTraceRequestFlags::AuthRefresh;
	}
```

and in `AttemptDispatched`, after the gap lines: `Payload.AuthWait = Ex.AuthWait; Payload.AuthRefreshId = Ex.AuthRefreshId;`.

- [ ] **Step 8: Run them, then the pipeline, log and trace suites**

Run: `Tools/run_tests.sh VaRest.Http.Auth` then `Tools/run_tests.sh --no-build VaRest.Http.Pipeline` then `Tools/run_tests.sh --no-build VaRest.Http.Log` then `Tools/run_tests.sh --no-build VaRest.Http.Trace`
Expected: `0 failed` each.

- [ ] **Step 9: Restore-the-bug** — in `FAuthApplyStage` skip the `ApplyCredentials` result check (always proceed): `VaRest.Http.Auth.Bearer` FAILS ("Required without credentials is Kind Auth"); restore; PASS. In the transport stage send `Ex.Url` again: `ApiKeyAndBasic` FAILS ("ApiKey in the query"); restore; PASS.

- [ ] **Step 10: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Public/VaRestAuth.h Source/VaRestHttp/Private/VaRestAuthInternal.h Source/VaRestHttp/Private/VaRestAuth.cpp \
  Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Public/VaRestStage.h \
  Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestLogStage.cpp Source/VaRestHttp/Private/VaRestTracePoints.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp
git commit -m "http: M4 task 12 -- IVaRestAuth, Bearer, ApiKey and Basic, the per-attempt auth stage, auth modes (spec 4.2, WS 19 item 2)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 13: The single-flight refresh and the `AuthGate` stage — park, proactive refresh, replay once (spec §4.2; IN-2, IN-9; WebSocket §19 item 2)

Plan decisions 2, 3, 6, 7, 8. Spec §4.2: "the first 401 starts one refresh. Concurrent requests that hit 401 (or start while it runs) park behind it, then replay once each." The refresh is the client's (`FClientState`), shared by HTTP and sockets (`RefreshAuth`). `AuthGate` sits after `Retry` and before the attempt boundary, so a replay is a new attempt with `GapBefore = SessionRefresh`, and a parked request is held through `Interrupt` like a backoff.

**Files:**
- Modify: `Source/VaRestHttp/Private/VaRestSession.h`, `Source/VaRestHttp/Private/VaRestSession.cpp` (the refresh in flight; game changes end it)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp` (`Publish` adds the stage)
- Create: `Source/VaRestHttp/Private/VaRestAuthGate.cpp`; modify `Source/VaRestHttp/Private/VaRestAuthInternal.h`
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Resolve`: an auth that may replay copies the body)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (`NeedsAuthRefresh`, `RefreshAuth`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`

**Interfaces:**
- Consumes: Tasks 7 (`Interrupt`, `NextGap`), 11, 12.
- Produces (private, `VaRestSession.h`): `enum class ERefreshEnd : uint8 { Ready, Rejected, Unreachable, Cleared }`; `struct FRefreshEnded { ERefreshEnd End; uint64 RefreshId; EVaRestErrorKind Kind; FString Message; }`; `using FRefreshWaiter = TUniqueFunction<void(const FRefreshEnded&)>`; `FSessionState::Refresh` (`TUniquePtr<FRefreshInFlight>`), `NextRefreshId`, `NextWaiterId`. On `FClientState`: `bool IsRefreshing() const`, `uint64 NewWaiterId()`, `void JoinRefresh(uint64 WaiterId, const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth, uint64 CauseRequestId, FRefreshWaiter&& OnDone)` (OnDone may run before it returns), `void LeaveRefresh(uint64 WaiterId)`; `FVaRestStageRef VaRest::Http::MakeAuthGateStage()`. Public: `bool FVaRestClient::NeedsAuthRefresh() const`, `void FVaRestClient::RefreshAuth(TUniqueFunction<void(bool bRefreshed)>&& OnDone)`. The chain: custom Outer → Retry → **AuthGate** → boundary → AuthApply → custom PerAttempt → log → limiter → transport.

- [ ] **Step 1: Write the failing tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`; add to `AuthTestPrivate`:

```cpp
	/** A bearer whose refresh the test answers by hand: a refresh endpoint as slow as the test wants */
	class FHandAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("Hand"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!Target.Session.HasAccessToken())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
			return true;
		}

		virtual bool CanRefresh(const FVaRestSessionData& Session) const override { return Session.HasRefreshToken(); }

		virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const override
		{
			return Session.HasRefreshToken() && (!Session.HasAccessToken() || (Session.ExpiresAt > 0.0 && Now >= Session.ExpiresAt - 30.0));
		}

		virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
		{
			++Refreshes;
			LastRefreshId = Context.GetRefreshId();
			Pending = MoveTemp(Done);
		}

		bool IsPending() const { return static_cast<bool>(Pending); }

		void Answer(FVaRestAuthRefreshResult&& Result)
		{
			if (Pending)
			{
				TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Done = MoveTemp(Pending);
				Pending.Reset();
				Done(MoveTemp(Result));
			}
		}

		int32 Refreshes = 0;
		uint64 LastRefreshId = 0;

	private:
		TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Pending;
	};

	/** 200 for the accepted tokens, 401 for anything else */
	void AcceptTokens(VaRest::Http::Testing::FTestRig& Rig, TArray<FString> Accepted)
	{
		Rig.Fake->SetResponder([Accepted](const FVaRestTransportRequest& Request)
		{
			const FString Given = Request.Headers.Get(TEXT("Authorization"));
			return Accepted.ContainsByPredicate([&Given](const FString& Token) { return Given == FString(TEXT("Bearer ")) + Token; }) ? Json(200) : Json(401);
		});
	}

	TSharedRef<FHandAuth, ESPMode::ThreadSafe> HandRig(VaRest::Http::Testing::FTestRig& Rig)
	{
		TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = MakeShared<FHandAuth, ESPMode::ThreadSafe>();
		Rig.Client.WithAuth(Auth);
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		AcceptTokens(Rig, { TEXT("new-access-123456") });
		return Auth;
	}

	TArray<FVaRestTraceAttemptDispatched> DispatchesOf(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		TArray<FVaRestTraceAttemptDispatched> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::AttemptDispatched)
			{
				Out.Add(Event.Payload.Get<FVaRestTraceAttemptDispatched>());
			}
		}
		return Out;
	}
```

and the tests:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthSingleFlightTest, "VaRest.Http.Auth.SingleFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthSingleFlightTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	TArray<uint64> Ids;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Ids.Add(Observe(Rig.Client.Get(FString::Printf(TEXT("/p/%d"), Index)), Outcomes.Last()).Send().GetId());
	}
	if (!TestTrue(TEXT("twenty 401s, one refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 20; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	// Spec 4.2: "or start while it runs" -- these park before their first attempt
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Ids.Add(Observe(Rig.Client.Get(FString::Printf(TEXT("/late/%d"), Index)), Outcomes.Last()).Send().GetId());
	}
	Rig.Drain();
	TestEqual(TEXT("the late ones wait: no call yet"), Rig.Fake->NumCalls(), 20);
	TestEqual(TEXT("one refresh for all of them"), Auth->Refreshes, 1);
	Rig.Clock->Advance(0.25);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), TEXT("new-refresh-123456")));
	const auto AllDone = [&Outcomes] { for (const auto& One : Outcomes) { if (!One->IsDone()) { return false; } } return true; };
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone)))
	{
		Abandon(Rig);
		return false;
	}
	int32 Successes = 0;
	for (int32 Index = 0; Index < Outcomes.Num(); ++Index)
	{
		Successes += Outcomes[Index]->SuccessCalls.load();
		TestEqual(FString::Printf(TEXT("request %d: attempts"), Index), Outcomes[Index]->Response.Attempts, Index < 20 ? 2 : 1);
	}
	TestEqual(TEXT("all succeed"), Successes, 22);
	TestEqual(TEXT("each 401 replayed once, each late one sent once"), Rig.Fake->NumCalls(), 42);
	TestEqual(TEXT("still one refresh"), Auth->Refreshes, 1);
	TestEqualSensitive(TEXT("the session holds the new tokens"), Rig.Client.GetSession().RefreshToken, FString(TEXT("new-refresh-123456")));

	// IN-2: a replay's gap is the refresh; a late request's first attempt waited for it
	const TArray<FVaRestTraceAttemptDispatched> Replayed = DispatchesOf(*Scoped.Recorder, Ids[0]);
	const TArray<FVaRestTraceAttemptDispatched> Late = DispatchesOf(*Scoped.Recorder, Ids[20]);
	if (!TestTrue(TEXT("two dispatches, then one"), Replayed.Num() == 2 && Late.Num() == 1))
	{
		return false;
	}
	TestTrue(TEXT("the replay: GapBefore SessionRefresh 0.25 s, AuthWait, the refresh's id"), Replayed[1].GapKind == EVaRestTraceGap::SessionRefresh
		&& FMath::IsNearlyEqual(Replayed[1].GapSeconds, 0.25) && FMath::IsNearlyEqual(Replayed[1].AuthWait, 0.25) && Replayed[1].AuthRefreshId == Auth->LastRefreshId);
	TestTrue(TEXT("the late one: no gap, AuthWait, the same refresh"), Late[0].GapKind == EVaRestTraceGap::None && FMath::IsNearlyEqual(Late[0].AuthWait, 0.25)
		&& Late[0].AuthRefreshId == Auth->LastRefreshId && Late[0].Attempt == 1);
	const TArray<FVaRestTraceMarker> Markers = Scoped.Recorder->MarkersOf(Rig.Client.GetId());
	TestTrue(TEXT("IN-9: SessionRefreshed, caused by one of the 401s"), Markers.ContainsByPredicate([&Ids](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Ids.Contains(Marker.CauseRequestId);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRejectedTest, "VaRest.Http.Auth.RefreshRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRejectedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("both parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
	if (!TestTrue(TEXT("both complete"), PumpUntil([A, B] { return A->IsDone() && B->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("spec 4.2: parked requests fail Kind Auth, not retryable"), A->Error.Kind == EVaRestErrorKind::Auth && B->Error.Kind == EVaRestErrorKind::Auth
		&& !A->Error.bRetryable && A->Error.Message.Contains(TEXT("rejected")));
	TestEqual(TEXT("... and the client fires OnSessionExpired once"), *Expired, 1);
	TestTrue(TEXT("plan decision 3: a rejected refresh drops the tokens"), !Rig.Client.GetSession().HasAccessToken() && !Rig.Client.GetSession().HasRefreshToken());
	auto After = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), After).Send();
	TestTrue(TEXT("the next request: no credentials, not sent"), Rig.Wait(After) && After->Error.Kind == EVaRestErrorKind::Auth && Rig.Fake->NumCalls() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthUnreachableTest, "VaRest.Http.Auth.RefreshUnreachableKeepsSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthUnreachableTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	auto A = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Unreachable(EVaRestErrorKind::Connection, TEXT("the refresh endpoint did not answer")));
	if (!TestTrue(TEXT("completes"), Rig.Wait(A)))
	{
		Abandon(Rig);
		return false;
	}
	// Plan decision 3: a network blip must not log the player out
	TestTrue(TEXT("the parked request fails as the refresh did: Connection, retryable"), A->Error.Kind == EVaRestErrorKind::Connection && A->Error.bRetryable
		&& A->Error.Message.Contains(TEXT("could not reach")));
	TestTrue(TEXT("the session is kept"), Rig.Client.GetSession().AccessToken == TEXT("old-access-123456") && Rig.Client.GetSession().HasRefreshToken());
	TestEqual(TEXT("no OnSessionExpired"), *Expired, 0);
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("the next 401 starts a new refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain() && Auth->Refreshes == 2))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("... which works"), Rig.Wait(B) && B->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthStopTest, "VaRest.Http.Auth.StopWhileParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto Canceled = MakeOutcome<FVaRestJson>();
	auto Owned = MakeOutcome<FVaRestJson>();
	auto ShutDown = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/c")), Canceled).Send();
	Observe(Rig.Client.Get(TEXT("/o")).Owner(Owner), Owned).Send();
	Observe(Rig.Client.Get(TEXT("/s")).Tag(TEXT("Exit")), ShutDown).Send();
	if (!TestTrue(TEXT("all three parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Handle.Cancel();
	if (!TestTrue(TEXT("Cancel() ends the park"), PumpUntil([Canceled] { return Canceled->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... one OnError(Canceled), one OnComplete"), Canceled->ErrorCalls.load() == 1 && Canceled->CompleteCalls.load() == 1
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled);
	VaRest::Http::FClientAccess::State(Rig.Client)->CancelAll(TEXT("Exit"), EVaRestCancelReason::Shutdown);
	Owner->MarkAsGarbage();
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("nothing left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("no replay after any stop"), Rig.Fake->NumCalls(), 3);
	TestEqual(TEXT("shutdown and owner gone: no callbacks"), ShutDown->CompleteCalls.load() + Owned->CompleteCalls.load() + ShutDown->ErrorCalls.load()
		+ Owned->ErrorCalls.load(), 0);
	TestEqualSensitive(TEXT("the refresh still updates the session"), Rig.Client.GetSession().AccessToken, FString(TEXT("new-access-123456")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthProactiveTest, "VaRest.Http.Auth.Proactive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthProactiveTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	// Spec 4.2: "proactively 30 s before a known expiry"
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
	TestTrue(TEXT("WS 19 item 2: NeedsAuthRefresh"), Rig.Client.NeedsAuthRefresh());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	Rig.Drain();
	if (!TestTrue(TEXT("parked before its first attempt"), Rig.Fake->NumCalls() == 0 && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), FString(), 3600.0));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("one attempt, with the new token"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 1 && Rig.Fake->NumCalls() == 1);
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Handle.GetId());
	TestTrue(TEXT("IN-2: AuthWait 0.5 s, no gap"), Dispatched.Num() == 1 && FMath::IsNearlyEqual(Dispatched[0].AuthWait, 0.5)
		&& Dispatched[0].GapKind == EVaRestTraceGap::None);
	TestFalse(TEXT("an hour left: no refresh due"), Rig.Client.NeedsAuthRefresh());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthReplayOnceTest, "VaRest.Http.Auth.ReplayOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthReplayOnceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	AcceptTokens(Rig, {});
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/never")), Outcome).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("spec 4.2 \"replay once\": the second 401 is the answer"), Outcome->Error.Kind == EVaRestErrorKind::Http && Outcome->Error.HttpStatus == 401
		&& Outcome->Response.Attempts == 2);
	TestEqual(TEXT("... no second refresh"), Auth->Refreshes, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthGameChangesTest, "VaRest.Http.Auth.GameChangesWhileRefreshing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthGameChangesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	AcceptTokens(Rig, { TEXT("game-access-123456"), TEXT("new-access-123456") });
	const auto ParkOne = [&Rig, &Auth](const TCHAR* Path, const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Outcome)
	{
		const int32 Before = Rig.Fake->NumCalls();
		Observe(Rig.Client.Get(Path), Outcome).Send();
		return PumpUntil([&Rig, Before] { return Rig.Fake->NumCalls() == Before + 1; }) && Rig.Drain() && Auth->IsPending();
	};
	// A login by the game while a refresh runs: the parked request goes with the game's token; the refresh's late answer is dropped
	auto A = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("A parked"), ParkOne(TEXT("/a"), A)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetBearerToken(TEXT("game-access-123456"));
	TestTrue(TEXT("A replays with the game's token"), Rig.Wait(A) && A->SuccessCalls.load() == 1);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestEqualSensitive(TEXT("the late refresh changes nothing"), Rig.Client.GetSession().AccessToken, FString(TEXT("game-access-123456")));

	// A logout while a refresh runs: Kind Auth, SessionCleared
	AcceptTokens(Rig, {});
	auto B = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("B parked"), ParkOne(TEXT("/b"), B)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.ClearSession();
	TestTrue(TEXT("B fails Kind Auth, the session cleared"), Rig.Wait(B) && B->Error.Kind == EVaRestErrorKind::Auth && B->Error.Message.Contains(TEXT("SessionCleared")));

	// A socket's NotifySessionExpired while a refresh runs: Kind Auth, rejected
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	auto C = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("C parked"), ParkOne(TEXT("/c"), C)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.NotifySessionExpired();
	TestTrue(TEXT("C fails Kind Auth"), Rig.Wait(C) && C->Error.Kind == EVaRestErrorKind::Auth && C->Error.Message.Contains(TEXT("AuthRejected")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshPublicTest, "VaRest.Http.Auth.RefreshAuthPublic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshPublicTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// WebSocket spec 19 item 2: a socket refreshes through the client, sharing HTTP's refresh
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<TArray<bool>, ESPMode::ThreadSafe> Answers = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
	Rig.Client.RefreshAuth([Answers](bool bRefreshed) { Answers->Add(bRefreshed); });
	Rig.Client.RefreshAuth([Answers](bool bRefreshed) { Answers->Add(bRefreshed); });
	TestEqual(TEXT("nothing inside the call (decision 3)"), Answers->Num(), 0);
	TestEqual(TEXT("one refresh for both"), Auth->Refreshes, 1);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("both told: refreshed"), PumpUntil([Answers] { return Answers->Num() == 2; }) && (*Answers)[0] && (*Answers)[1]);
	FTestRig Plain;
	TSharedRef<TArray<bool>, ESPMode::ThreadSafe> None = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
	Plain.Client.RefreshAuth([None](bool bRefreshed) { None->Add(bRefreshed); });
	TestTrue(TEXT("no auth: false, later"), None->Num() == 0 && PumpUntil([None] { return None->Num() == 1; }) && !(*None)[0]);
	return true;
}
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Auth`
Expected: FAIL to compile (`NeedsAuthRefresh`, `RefreshAuth` unknown).

- [ ] **Step 3: The refresh in flight** — in `Source/VaRestHttp/Private/VaRestSession.h`, before `FSessionState`:

```cpp
	/** How a parked request's wait for a refresh ended (plan decision 3) */
	enum class ERefreshEnd : uint8
	{
		/** New credentials, from the refresh or from the game: go */
		Ready,
		/** The refresh was rejected, or NotifySessionExpired: Kind=Auth */
		Rejected,
		/** The refresh could not reach the server: fail with Kind (Connection or Timeout), retryable */
		Unreachable,
		/** ClearSession (a logout) while waiting: Kind=Auth */
		Cleared
	};

	struct FRefreshEnded
	{
		ERefreshEnd End = ERefreshEnd::Ready;
		uint64 RefreshId = 0;
		EVaRestErrorKind Kind = EVaRestErrorKind::Connection;
		FString Message;
	};

	using FRefreshWaiter = TUniqueFunction<void(const FRefreshEnded&)>;

	/** Spec 4.2's single flight: the refresh running now and who waits for it */
	struct FRefreshInFlight
	{
		uint64 RefreshId = 0;
		/** IN-9 CauseRequestId: the request whose 401 (or due expiry) started it; 0 for RefreshAuth */
		uint64 CauseRequestId = 0;
		TArray<TPair<uint64, FRefreshWaiter>> Waiters;
	};
```

and in `FSessionState`:

```cpp
		/** Null when no refresh runs */
		TUniquePtr<FRefreshInFlight> Refresh;
		uint64 NextRefreshId = 0;
		uint64 NextWaiterId = 0;
```

In `Source/VaRestHttp/Private/VaRestClientState.h`, `FClientState` public, after the session functions:

```cpp
		// ---- Single-flight refresh (spec 4.2; Task 13; VaRestSession.cpp). Game thread.
		bool IsRefreshing() const { return Session.Refresh.IsValid(); }
		uint64 NewWaiterId() { return ++Session.NextWaiterId; }
		/**
		 * Waits for the refresh in flight, or starts one with Auth. OnDone runs once on the game thread -- possibly before this
		 * returns (an auth that answers at once), so a caller sets up what OnDone undoes (FExchangeState::Interrupt) first.
		 */
		void JoinRefresh(uint64 WaiterId, const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth, uint64 CauseRequestId, FRefreshWaiter&& OnDone);
		/** A parked request was canceled: its waiter goes uncalled; the refresh goes on */
		void LeaveRefresh(uint64 WaiterId);
```

private:

```cpp
		void StartRefresh(const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth);
		void FinishRefresh(uint64 RefreshId, FVaRestAuthRefreshResult&& Result);
		/** Ends the refresh in flight without its result: the game changed the session (Ready, Cleared) or it expired (Rejected) */
		void EndRefresh(ERefreshEnd End, const TCHAR* Message);
```

In `Source/VaRestHttp/Private/VaRestSession.cpp` (`#include "Tasks/Task.h"`, `#include "VaRestAuth.h"`):

```cpp
void VaRest::Http::FClientState::JoinRefresh(uint64 WaiterId, const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth, uint64 CauseRequestId, FRefreshWaiter&& OnDone)
{
	check(IsInGameThread());
	const bool bStart = !Session.Refresh.IsValid();
	if (bStart)
	{
		Session.Refresh = MakeUnique<FRefreshInFlight>();
		Session.Refresh->RefreshId = ++Session.NextRefreshId;
		Session.Refresh->CauseRequestId = CauseRequestId;
	}
	Session.Refresh->Waiters.Emplace(WaiterId, MoveTemp(OnDone));
	if (bStart)
	{
		StartRefresh(Auth);
	}
}

void VaRest::Http::FClientState::LeaveRefresh(uint64 WaiterId)
{
	check(IsInGameThread());
	if (Session.Refresh.IsValid())
	{
		Session.Refresh->Waiters.RemoveAll([WaiterId](const TPair<uint64, FRefreshWaiter>& Waiter) { return Waiter.Key == WaiterId; });
	}
}

void VaRest::Http::FClientState::StartRefresh(const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth)
{
	const uint64 RefreshId = Session.Refresh->RefreshId;
	const TWeakPtr<FClientState, ESPMode::ThreadSafe> Weak = AsShared();
	const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bAnswered = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	const FVaRestAuthRefreshContext Context(FClientAccess::Wrap(AsShared()), Session.Data, RefreshId);
	Auth->Refresh(Context, [Weak, RefreshId, bAnswered](FVaRestAuthRefreshResult&& Result)
	{
		// IVaRestAuth::Refresh: Done once; a second call is ignored
		if (bAnswered->exchange(true))
		{
			return;
		}
		TUniqueFunction<void()> Finish = [Weak, RefreshId, Moved = MoveTemp(Result)]() mutable
		{
			if (const TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->FinishRefresh(RefreshId, MoveTemp(Moved));
			}
		};
		if (IsInGameThread())
		{
			Finish();
			return;
		}
		UE::Tasks::Launch(TEXT("VaRest.RefreshDone"), MoveTemp(Finish), UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	});
}

void VaRest::Http::FClientState::FinishRefresh(uint64 RefreshId, FVaRestAuthRefreshResult&& Result)
{
	check(IsInGameThread());
	if (!Session.Refresh.IsValid() || Session.Refresh->RefreshId != RefreshId)
	{
		// Ended without its result: the game set or cleared the session meanwhile (EndRefresh)
		return;
	}
	const TUniquePtr<FRefreshInFlight> Ended = MoveTemp(Session.Refresh);
	FRefreshEnded End;
	End.RefreshId = RefreshId;
	End.Message = Result.Message;
	bool bRefreshed = false;
	bool bExpired = false;
	if (Result.Outcome == EVaRestAuthRefreshOutcome::Refreshed)
	{
		if (!Result.AccessToken.IsEmpty() && IsValidHeaderValue(Result.AccessToken) && IsValidHeaderValue(Result.RefreshToken))
		{
			UpdateTokens(Result.AccessToken, Result.RefreshToken.IsEmpty() ? nullptr : &Result.RefreshToken, Result.ExpiresInSeconds);
			End.End = ERefreshEnd::Ready;
			bRefreshed = true;
		}
		else
		{
			End.End = ERefreshEnd::Rejected;
			End.Message = TEXT("the refresh gave no usable access token");
			bExpired = true;
		}
	}
	else if (Result.Outcome == EVaRestAuthRefreshOutcome::Unreachable)
	{
		// Plan decision 3: the session stays; the parked requests fail as the refresh did, retryable
		End.End = ERefreshEnd::Unreachable;
		End.Kind = Result.UnreachableKind == EVaRestErrorKind::Timeout ? EVaRestErrorKind::Timeout : EVaRestErrorKind::Connection;
	}
	else
	{
		End.End = ERefreshEnd::Rejected;
		bExpired = true;
	}
	if (bExpired)
	{
		Session.Data.AccessToken.Reset();
		Session.Data.RefreshToken.Reset();
		Session.Data.ExpiresAt = 0.0;
		++Session.Data.Generation;
		PublishSessionSecrets();
	}
	// The waiters first, so each replays with the new token (or fails) before a handler of the events below runs
	for (TPair<uint64, FRefreshWaiter>& Waiter : Ended->Waiters)
	{
		Waiter.Value(End);
	}
	if (bRefreshed)
	{
		AnnounceSession(EVaRestSessionChange::Refreshed, Ended->CauseRequestId, TEXT("refreshed by the auth"));
	}
	if (bExpired)
	{
		TracePoints::Marker(*this, EVaRestTraceMarkerKind::SessionExpired, Ended->CauseRequestId, End.Message);
		Session.Expired.Broadcast();
	}
}

void VaRest::Http::FClientState::EndRefresh(ERefreshEnd EndKind, const TCHAR* Message)
{
	if (!Session.Refresh.IsValid())
	{
		return;
	}
	const TUniquePtr<FRefreshInFlight> Ended = MoveTemp(Session.Refresh);
	FRefreshEnded End;
	End.End = EndKind;
	End.RefreshId = Ended->RefreshId;
	End.Message = Message;
	for (TPair<uint64, FRefreshWaiter>& Waiter : Ended->Waiters)
	{
		Waiter.Value(End);
	}
}
```

The game's changes end a refresh in flight. In `SetTokensByGame`, inside `if (UpdateTokens(...))` and before `AnnounceSession`: `EndRefresh(ERefreshEnd::Ready, TEXT("the game set new credentials"));`. In `ClearSession`, before `AnnounceSession(EVaRestSessionChange::Cleared, ...)`: `EndRefresh(ERefreshEnd::Cleared, TEXT("SessionCleared"));` — and in its early return for an empty session, `EndRefresh(ERefreshEnd::Cleared, TEXT("SessionCleared"));` before `return;`. In `ExpireSession`, after `PublishSessionSecrets();`: `EndRefresh(ERefreshEnd::Rejected, TEXT("AuthRejected"));`.

- [ ] **Step 4: The gate** — in `Source/VaRestHttp/Private/VaRestAuthInternal.h` declare:

```cpp
	/** Spec 4.2: the built-in Outer stage after retry -- parks, refreshes, replays once through the attempt boundary */
	FVaRestStageRef MakeAuthGateStage();
```

Create `Source/VaRestHttp/Private/VaRestAuthGate.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuthInternal.h"

#include "VaRestClientState.h"
#include "VaRestExchangeState.h"

namespace VaRest::Http::AuthGatePrivate
{
	/** One exchange's way through the gate */
	struct FRun : public TSharedFromThis<FRun, ESPMode::ThreadSafe>
	{
		FRun(const FVaRestExchangeRef& InExchange, const FVaRestStageNext& InNext, FVaRestOutcomeCallback&& InDone, TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth)
			: Exchange(InExchange)
			, Next(InNext)
			, Done(MoveTemp(InDone))
			, Auth(MoveTemp(InAuth))
		{
		}

		bool IsAnswered() const { return !Done; }

		/** The stage's one outcome; a second call is ignored */
		void Answer(FVaRestAttemptOutcome&& Outcome)
		{
			if (!Done)
			{
				return;
			}
			FVaRestOutcomeCallback Last = MoveTemp(Done);
			Done.Reset();
			Last(MoveTemp(Outcome));
		}

		FVaRestExchangeRef Exchange;
		FVaRestStageNext Next;
		FVaRestOutcomeCallback Done;
		/** The Send() snapshot's auth (plan decision 6: the auth is configuration, the session is not) */
		TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> Auth;
		/** Spec 4.2: "then replay once each" */
		bool bReplayed = false;
	};

	using FRunRef = TSharedRef<FRun, ESPMode::ThreadSafe>;

	FVaRestAttemptOutcome FailureFor(const FRefreshEnded& End)
	{
		FVaRestAttemptOutcome Out;
		switch (End.End)
		{
		case ERefreshEnd::Unreachable:
			// Plan decision 3: the refresh's own kind, retryable, so Retry (outside) and M4b's offline queue can act
			Out.Failure = End.Kind == EVaRestErrorKind::Timeout ? EVaRestTransportFailure::Timeout : EVaRestTransportFailure::Connection;
			Out.FailureDetail = TEXT("AuthRefreshUnreachable");
			Out.FailureMessage = FString::Printf(TEXT("the session refresh could not reach the server: %s"), *End.Message);
			break;
		case ERefreshEnd::Cleared:
			Out.Failure = EVaRestTransportFailure::Other;
			Out.FailureDetail = TEXT("SessionCleared");
			Out.FailureMessage = TEXT("the session was cleared (a logout) while this request waited for a refresh");
			break;
		default:
			Out.Failure = EVaRestTransportFailure::Other;
			Out.FailureDetail = TEXT("AuthRejected");
			Out.FailureMessage = FString::Printf(TEXT("the session refresh was rejected: %s"), *End.Message);
			break;
		}
		return Out;
	}

	void Dispatch(const FRunRef& Run);

	/** Holds Run behind the client's refresh (plan decision 7); bReplay: this follows a refused attempt (IN-2 GapBefore) */
	void Park(const FRunRef& Run, bool bReplay)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		FClientState& State = *Ex.State;
		const uint64 WaiterId = State.NewWaiterId();
		const double ParkedAt = Ex.Config->Clock->Now();
		const TWeakPtr<FRun, ESPMode::ThreadSafe> Weak = Run;
		// Set before JoinRefresh: an auth that answers at once resolves the waiter inside it, and the waiter clears this
		Ex.Interrupt = [Weak, WaiterId]
		{
			if (const TSharedPtr<FRun, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Internals(*Pinned->Exchange).State->LeaveRefresh(WaiterId);
				Pinned->Answer(FVaRestAttemptOutcome::Canceled());
			}
		};
		State.JoinRefresh(WaiterId, Run->Auth, Ex.RequestId, [Run, ParkedAt, bReplay](const FRefreshEnded& End)
		{
			FExchangeState& Back = Internals(*Run->Exchange);
			Back.Interrupt.Reset();
			if (Run->IsAnswered())
			{
				return;
			}
			if (End.End != ERefreshEnd::Ready)
			{
				Run->Answer(FailureFor(End));
				return;
			}
			if (IsOwnerGone(Back))
			{
				MarkOwnerGone(Back);
				Run->Answer(FVaRestAttemptOutcome::Canceled());
				return;
			}
			const double Seconds = Back.Config->Clock->Now() - ParkedAt;
			Back.AuthWait = Seconds;
			Back.AuthRefreshId = End.RefreshId;
			if (bReplay)
			{
				Back.NextGap = EVaRestTraceGap::SessionRefresh;
				Back.NextGapSeconds = Seconds;
			}
			Dispatch(Run);
		});
	}

	void AfterAttempt(const FRunRef& Run, FVaRestAttemptOutcome&& Outcome)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		if (Ex.bFinished || Run->Exchange->IsCancelRequested() || IsOwnerGone(Ex) || Run->bReplayed || Outcome.IsTransportFailure()
			|| !Run->Auth->IsAuthFailure(Outcome.Status))
		{
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		const FVaRestSessionData& Session = Ex.State->GetSessionData();
		// Plan decision 8: a body moved into the engine cannot go again; a BodyFile always can (its path is copied)
		const bool bCanResend = Ex.bMayReissue || Ex.Body.IsEmpty() || !Ex.Body.FilePath.IsEmpty();
		const bool bNewerCredentials = Session.Generation != Ex.AppliedGeneration;
		if (!bCanResend || (!bNewerCredentials && !Ex.State->IsRefreshing() && !Run->Auth->CanRefresh(Session)))
		{
			// Nothing would change: the refusal is the answer
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		Run->bReplayed = true;
		if (bNewerCredentials && !Ex.State->IsRefreshing())
		{
			// Another request's refresh, or the game, changed the credentials since this attempt took its own: replay now
			Ex.NextGap = EVaRestTraceGap::SessionRefresh;
			Ex.NextGapSeconds = 0.0;
			Dispatch(Run);
			return;
		}
		Park(Run, /*bReplay*/ true);
	}

	void Dispatch(const FRunRef& Run)
	{
		Run->Next.Proceed(Run->Exchange, [Run](FVaRestAttemptOutcome&& Outcome) { AfterAttempt(Run, MoveTemp(Outcome)); });
	}

	void BeforeAttempt(const FRunRef& Run)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		FClientState& State = *Ex.State;
		const FVaRestSessionData& Session = State.GetSessionData();
		// Spec 4.2: a refresh in flight parks every request that starts meanwhile; a due expiry refreshes first ("proactively")
		if (State.IsRefreshing() || (Run->Auth->CanRefresh(Session) && Run->Auth->NeedsRefresh(Session, Ex.Config->Clock->Now())))
		{
			Park(Run, /*bReplay*/ false);
			return;
		}
		Dispatch(Run);
	}

	class FAuthGateStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("AuthGate"); }

		/** True, but not read: per exchange (FExchangeState::bMayReissue, plan decision 8) */
		virtual bool MayReissue() const override { return true; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			if (Ex.AuthMode == EVaRestAuthMode::None || Ex.bIsAuthRefresh || !Ex.Config->Auth.IsValid())
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			BeforeAttempt(MakeShared<FRun, ESPMode::ThreadSafe>(Exchange, Next, MoveTemp(OnOutcome), Ex.Config->Auth.ToSharedRef()));
		}
	};
}

FVaRestStageRef VaRest::Http::MakeAuthGateStage()
{
	return MakeShared<AuthGatePrivate::FAuthGateStage, ESPMode::ThreadSafe>();
}
```

- [ ] **Step 5: The chain and the body copy** — `Source/VaRestHttp/Private/VaRestClientState.cpp`, `Publish`: after `NewChain->Add(MakeRetryStage());`:

```cpp
	// Spec 4.2: after retry (a replay is not a retry, plan decision 2), before the boundary (a replay is a new attempt)
	NewChain->Add(MakeAuthGateStage());
```

`Source/VaRestHttp/Private/VaRestPipeline.cpp`, `Resolve`: the `bMayReissue` line becomes

```cpp
	// Plan decision 8: copy the body only when something may send it again -- a custom stage, a retry, or a session replay
	const bool bAuthMayReplay = Cfg.Auth.IsValid() && Exchange.AuthMode != EVaRestAuthMode::None && !Exchange.bIsAuthRefresh
		&& Cfg.Auth->CanRefresh(GetSessionData());
	Exchange.bMayReissue = Cfg.bMayReissue || Exchange.bRetryEligible || bAuthMayReplay;
```

- [ ] **Step 6: The public refresh** — `Source/VaRestHttp/Public/VaRestClient.h`, auth section:

```cpp
	/** WebSocket spec 19 item 2: a refresh is due -- no access token but a refresh token, or a known expiry within the auth's lead time */
	bool NeedsAuthRefresh() const;
	/**
	 * WebSocket spec 19 item 2: refresh the session now, sharing a refresh already in flight (single-flight, spec 4.2).
	 * OnDone(true) when the session has new credentials, false otherwise (no auth, nothing to refresh with, rejected,
	 * unreachable). Never inside this call (decision 3). Game thread.
	 */
	void RefreshAuth(TUniqueFunction<void(bool bRefreshed)>&& OnDone);
```

`Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
bool FVaRestClient::NeedsAuthRefresh() const
{
	check(IsInGameThread());
	if (!State.IsValid())
	{
		return false;
	}
	const TSharedRef<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> Current = State->GetConfig();
	const FVaRestSessionData& Data = State->GetSessionData();
	return Current->Auth.IsValid() && Current->Auth->CanRefresh(Data) && Current->Auth->NeedsRefresh(Data, Current->Clock->Now());
}

void FVaRestClient::RefreshAuth(TUniqueFunction<void(bool bRefreshed)>&& OnDone)
{
	check(IsInGameThread());
	const TSharedRef<TUniqueFunction<void(bool)>, ESPMode::ThreadSafe> Callback = MakeShared<TUniqueFunction<void(bool)>, ESPMode::ThreadSafe>(MoveTemp(OnDone));
	// Decision 3: a later game-thread task, whether the answer is known now or comes from the refresh
	const auto Later = [Callback](bool bRefreshed)
	{
		UE::Tasks::Launch(TEXT("VaRest.RefreshAuth"), [Callback, bRefreshed]
		{
			if (*Callback)
			{
				(*Callback)(bRefreshed);
			}
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	};
	const TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> Auth = State.IsValid() ? State->GetConfig()->Auth : nullptr;
	if (!Auth.IsValid() || !Auth->CanRefresh(State->GetSessionData()))
	{
		Later(false);
		return;
	}
	State->JoinRefresh(State->NewWaiterId(), Auth.ToSharedRef(), 0, [Later](const VaRest::Http::FRefreshEnded& End) { Later(End.End == VaRest::Http::ERefreshEnd::Ready); });
}
```

- [ ] **Step 7: Run them, then the retry, cancel and pipeline suites**

Run: `Tools/run_tests.sh VaRest.Http.Auth` then `Tools/run_tests.sh --no-build VaRest.Http.Retry` then `Tools/run_tests.sh --no-build VaRest.Http.Cancel` then `Tools/run_tests.sh --no-build VaRest.Http.Pipeline`
Expected: `0 failed` each.

- [ ] **Step 8: Restore-the-bug** — (a) in `JoinRefresh` start a refresh for every waiter (drop the `bStart` test): `SingleFlight` FAILS ("one refresh for all of them"); restore; PASS. (b) In `FinishRefresh` treat `Unreachable` like `Rejected`: `RefreshUnreachableKeepsSession` FAILS ("the session is kept"); restore; PASS. (c) In `Park` drop `Ex.Interrupt = ...`: `StopWhileParked` FAILS (Cancel() finishes the request but the replay still runs, or the canceled request's waiter answers twice -- the "no replay after any stop" or "one OnError" check); restore; PASS. (d) Drop `Run->bReplayed = true;`: `ReplayOnce` FAILS (a second refresh); restore; PASS. Record all four.

- [ ] **Step 9: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestSession.h Source/VaRestHttp/Private/VaRestSession.cpp Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestAuthGate.cpp Source/VaRestHttp/Private/VaRestAuthInternal.h \
  Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp
git commit -m "http: M4 task 13 -- single-flight session refresh and the auth gate: park, proactive refresh, replay once, rejected vs unreachable

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 14: `BearerRefresh` — the refresh request, JSON and OAuth2 form styles, token paths, outcomes, redaction (spec §4.2; OpenAPI N15; carry (d))

Plan decisions 3, 22, 23. The built-in refreshing auth: its refresh request goes through the same client (logged, traced, limited) by `FVaRestAuthRefreshContext::Request`, and its response is read raw through the configured token paths.

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestBearerRefresh.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestAuth.h` (`FVaRestAuth::BearerRefresh`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`

**Interfaces:**
- Consumes: Tasks 11-13 (`FVaRestBearerRefreshConfig`, `IVaRestAuth`, the context, the coordinator).
- Produces: `static FVaRestAuth::FRef FVaRestAuth::BearerRefresh(const FVaRestBearerRefreshConfig& Config)`; private `FString VaRest::Http::BearerRefreshPrivate::LastSegment(FString Path)`.

- [ ] **Step 1: Write the failing tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`; add to `AuthTestPrivate`:

```cpp
	FString Text(TConstArrayView<uint8> Bytes)
	{
		const FUTF8ToTCHAR Converted(reinterpret_cast<const ANSICHAR*>(Bytes.GetData()), Bytes.Num());
		return FString(Converted.Length(), Converted.Get());
	}

	/** A refresh endpoint and a protected resource on the fake: the refresh trades OldRefresh for new tokens; the resource wants the new access token */
	void RefreshServer(VaRest::Http::Testing::FTestRig& Rig, FVaRestFakeResponse RefreshAnswer)
	{
		Rig.Fake->SetResponder([RefreshAnswer](const FVaRestTransportRequest& Request)
		{
			if (Request.Url.Contains(TEXT("/auth/")))
			{
				return RefreshAnswer;
			}
			return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200, R"({"ok":true})") : Json(401);
		});
	}

	FVaRestFakeResponse NewTokens()
	{
		return Json(200, R"({"accessToken":"new-access-123456","refreshToken":"new-refresh-123456","expiresIn":60})");
	}
```

and the tests:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerRefreshTest, "VaRest.Http.Auth.BearerRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, NewTokens());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe> Seen = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
	Rig.Client.OnSessionChanged().AddLambda([Seen](EVaRestSessionChange Change) { Seen->Add(Change); });
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/protected")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("401, the refresh, the replay"), Rig.Fake->NumCalls(), 3))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a success on the replay"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2);
	const FVaRestTransportRequest& Refresh = Rig.Fake->GetCall(1).Request;
	TestTrue(TEXT("the refresh: POST to the refresh path"), Refresh.Verb == TEXT("POST") && Refresh.Url == TEXT("https://api.test/auth/refresh"));
	TestEqualSensitive(TEXT("... a JSON body with the refresh token under RefreshTokenField"), Text(Rig.Fake->ReadBody(1)), FString(TEXT(R"({"refreshToken":"old-refresh-123456"})")));
	TestTrue(TEXT("... application/json, no Authorization (it bypasses the auth stages)"), Refresh.Headers.Get(TEXT("Content-Type")) == TEXT("application/json")
		&& !Refresh.Headers.Contains(TEXT("Authorization")));
	TestTrue(TEXT("... first in the limiter (plan decision 23)"), Refresh.Priority == EVaRestPriority::Highest);
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("the session: both new tokens, the expiry from expiresIn"), Data.AccessToken == TEXT("new-access-123456")
		&& Data.RefreshToken == TEXT("new-refresh-123456") && Data.ExpiresAt == 60.0);
	TestTrue(TEXT("OnSessionChanged(Refreshed), once"), Seen->Num() == 1 && (*Seen)[0] == EVaRestSessionChange::Refreshed);
	const uint64 RefreshId = Refresh.RequestId;
	bool bFlagged = false;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(RefreshId))
	{
		bFlagged |= Event.Kind == EVaRestTraceKind::Started && EnumHasAnyFlags(Event.Payload.Get<FVaRestTraceStarted>().Flags, EVaRestTraceRequestFlags::AuthRefresh);
	}
	TestTrue(TEXT("IN-1: the refresh request has flag AuthRefresh"), bFlagged);
	TestTrue(TEXT("IN-9: SessionRefreshed, caused by the 401"), Scoped.Recorder->MarkersOf(Rig.Client.GetId()).ContainsByPredicate([&Handle](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Marker.CauseRequestId == Handle.GetId();
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthOAuthFormTest, "VaRest.Http.Auth.OAuth2Form",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthOAuthFormTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// OpenAPI N15 / 7.7: an oauth2 password or client-credentials flow refreshes with RFC 6749 6
	FVaRestBearerRefreshConfig Config;
	Config.RefreshPath = TEXT("/auth/token");
	Config.Style = EVaRestRefreshStyle::OAuth2Form;
	Config.ClientId = TEXT("game");
	Config.AccessTokenPath = TEXT("access_token");
	Config.RefreshTokenPath = TEXT("refresh_token");
	Config.ExpiresInPath = TEXT("expires_in");
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, Json(200, R"({"access_token":"new-access-123456","token_type":"Bearer","expires_in":"30"})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("three calls"), Rig.Fake->NumCalls(), 3))
	{
		Abandon(Rig);
		return false;
	}
	TestEqualSensitive(TEXT("the form body"), Text(Rig.Fake->ReadBody(1)), FString(TEXT("grant_type=refresh_token&refresh_token=old-refresh-123456&client_id=game")));
	TestEqualSensitive(TEXT("... form-encoded"), Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/x-www-form-urlencoded")));
	const FVaRestSessionData Data = Rig.Client.GetSession();
	TestTrue(TEXT("a response without a refresh token keeps the old one; a numeric string is an expiry"), Data.AccessToken == TEXT("new-access-123456")
		&& Data.RefreshToken == TEXT("old-refresh-123456") && Data.ExpiresAt == 30.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshOutcomesTest, "VaRest.Http.Auth.RefreshOutcomes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshOutcomesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	struct FCase
	{
		const TCHAR* Name;
		FVaRestFakeResponse Answer;
		EVaRestErrorKind Kind;
		bool bExpires;
	};
	const FCase Cases[] = {
		{ TEXT("400: rejected"), Json(400, R"({"error":"invalid_grant"})"), EVaRestErrorKind::Auth, true },
		{ TEXT("503: unreachable"), Json(503), EVaRestErrorKind::Connection, false },
		{ TEXT("429: unreachable"), Json(429), EVaRestErrorKind::Connection, false },
		{ TEXT("no access token: rejected"), Json(200, R"({"refreshToken":"x"})"), EVaRestErrorKind::Auth, true },
		{ TEXT("not JSON: rejected"), FVaRestFakeResponse::Text(200, TEXT("ok")), EVaRestErrorKind::Auth, true },
		{ TEXT("connection error: unreachable"), FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")), EVaRestErrorKind::Connection, false },
		{ TEXT("timeout: unreachable as Timeout"), FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")), EVaRestErrorKind::Timeout, false },
	};
	for (const FCase& Case : Cases)
	{
		FTestRig Rig;
		Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		RefreshServer(Rig, Case.Answer);
		auto Outcome = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: completes"), Case.Name), Rig.Wait(Outcome)))
		{
			Abandon(Rig);
			return false;
		}
		TestEqual(FString::Printf(TEXT("%s: the parked request's kind"), Case.Name), Outcome->Error.Kind, Case.Kind);
		TestEqual(FString::Printf(TEXT("%s: the session expired or kept (plan decision 3)"), Case.Name), !Rig.Client.GetSession().HasRefreshToken(), Case.bExpires);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerProactiveTest, "VaRest.Http.Auth.BearerRefreshProactive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerProactiveTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 10.0);
	RefreshServer(Rig, NewTokens());
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)) || !TestEqual(TEXT("the refresh, then one attempt"), Rig.Fake->NumCalls(), 2))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("spec 4.2: refreshed before the known expiry, no 401 needed"), Rig.Fake->GetCall(0).Request.Url.Contains(TEXT("/auth/refresh"))
		&& Outcome->Response.Attempts == 1 && Outcome->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRedactedTest, "VaRest.Http.Auth.RefreshedTokenRedacted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRedactedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Review Focus: the refreshed token echoed in a header, a text body and a Location URL; the old one in a 401 body
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	FTestRig Rig;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/auth/")))
		{
			return NewTokens();
		}
		if (Request.Headers.Get(TEXT("Authorization")) != TEXT("Bearer new-access-123456"))
		{
			return Json(401, R"({"error":"expired","hint":"old-access-123456"})");
		}
		FVaRestFakeResponse Echo = FVaRestFakeResponse::Text(200, TEXT("your token is new-access-123456"));
		Echo.Headers.Set(TEXT("X-Echo"), TEXT("new-access-123456"));
		Echo.Headers.Set(TEXT("Location"), TEXT("/next?t=new-access-123456"));
		return Echo;
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers);
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 401"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	auto Outcome = MakeOutcome<FString>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get<FString>(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	FlushRedactionPipe();
	const TCHAR* Tokens[] = { TEXT("new-access-123456"), TEXT("old-access-123456"), TEXT("new-refresh-123456"), TEXT("old-refresh-123456") };
	for (const FString& Line : Capture.GetLines())
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Line.Contains(Token))
			{
				AddError(FString::Printf(TEXT("a log line holds %s: %s"), Token, *Line));
			}
		}
	}
	TArray<FString> Traced;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Handle.GetId()))
	{
		if (Event.Kind == EVaRestTraceKind::AttemptEnded)
		{
			const FVaRestTraceAttemptEnded& Ended = Event.Payload.Get<FVaRestTraceAttemptEnded>();
			Traced.Add(Ended.EffectiveUrl);
			for (const FVaRestHeaders::FPair& Pair : Ended.Headers)
			{
				Traced.Add(Pair.Value);
			}
		}
	}
	for (const FString& Value : Traced)
	{
		for (const TCHAR* Token : Tokens)
		{
			if (Value.Contains(Token))
			{
				AddError(FString::Printf(TEXT("a trace field holds %s: %s"), Token, *Value));
			}
		}
	}
	TestTrue(TEXT("the echoed header is in the trace, scrubbed"), Traced.Contains(TEXT("***")));
	TestTrue(TEXT("the body line was written, scrubbed"), Capture.Contains(TEXT("your token is ***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthPathKeysTest, "VaRest.Http.Auth.TokenPathKeys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthPathKeysTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Carry (d), M3 review M19: a token path's last segment is a key; "bearer" matches no built-in rule (VaRestJsonRedact.h)
	FVaRestBearerRefreshConfig Config;
	Config.AccessTokenPath = TEXT("data.auth.bearer");
	Config.RefreshTokenPath = TEXT("/data/renewal");
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	const FVaRestRedactRules& Rules = VaRest::Http::FClientAccess::State(Rig.Client)->GetRedactor()->GetRules();
	TestTrue(TEXT("the dotted path's last segment"), Rules.ShouldRedact(FStringView(TEXT("bearer")), EVaRestRedactKeyKind::Json));
	TestTrue(TEXT("the pointer's last segment"), Rules.ShouldRedact(FStringView(TEXT("renewal")), EVaRestRedactKeyKind::Json));
	TestFalse(TEXT("not the whole path"), Rules.ShouldRedact(FStringView(TEXT("data")), EVaRestRedactKeyKind::Json));
	return true;
}
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Auth`
Expected: FAIL to compile (`FVaRestAuth::BearerRefresh` unknown).

- [ ] **Step 3: The auth** — in `Source/VaRestHttp/Public/VaRestAuth.h`, `FVaRestAuth`:

```cpp
	/**
	 * Spec 4.2 BearerRefresh: the access token as Bearer; on a refresh status (401) or a known expiry within the lead time,
	 * one refresh shared by every request (Task 13): a POST of the refresh token to Config.RefreshPath, the new tokens read
	 * from the raw response at Config's paths. A 4xx answer expires the session; no answer (or 408, 429, 5xx) keeps it
	 * (plan decision 3). Each path's last segment becomes a redaction key (plan decision 22).
	 */
	static FRef BearerRefresh(const FVaRestBearerRefreshConfig& Config);
```

Create `Source/VaRestHttp/Private/VaRestBearerRefresh.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuth.h"

#include "VaRestBody.h"

namespace VaRest::Http::BearerRefreshPrivate
{
	/** Carry (d), M3 review M19: "data.auth.refresh_token" -> "refresh_token", "/a/b" -> "b", "items[3]" -> "items" */
	FString LastSegment(FString Path)
	{
		int32 Bracket = INDEX_NONE;
		while (Path.EndsWith(TEXT("]")) && Path.FindLastChar(TEXT('['), Bracket))
		{
			Path.LeftInline(Bracket);
		}
		for (int32 Index = Path.Len() - 1; Index >= 0; --Index)
		{
			if (Path[Index] == TEXT('.') || Path[Index] == TEXT('/'))
			{
				return Path.Mid(Index + 1);
			}
		}
		return Path;
	}

	double ReadSeconds(const FVaRestJson& Value)
	{
		if (Value.IsNumber())
		{
			return Value.AsDouble();
		}
		if (Value.IsString())
		{
			const FString Number = Value.AsString();
			return Number.IsNumeric() ? FCString::Atod(*Number) : 0.0;
		}
		return 0.0;
	}

	FVaRestAuthRefreshResult ReadRefresh(const FVaRestBearerRefreshConfig& Config, const TVaRestResult<FVaRestJson>& Result)
	{
		if (Result.IsSuccess())
		{
			const FVaRestJson Body = Result.Value.IsSet() ? Result.Value.GetValue() : FVaRestJson();
			const FString Access = Body.Path(Config.AccessTokenPath).AsString();
			if (Access.IsEmpty())
			{
				return FVaRestAuthRefreshResult::Rejected(FString::Printf(TEXT("the refresh response has no access token at \"%s\""), *Config.AccessTokenPath));
			}
			const FString Refresh = Config.RefreshTokenPath.IsEmpty() ? FString() : Body.Path(Config.RefreshTokenPath).AsString();
			const double ExpiresIn = Config.ExpiresInPath.IsEmpty() ? 0.0 : ReadSeconds(Body.Path(Config.ExpiresInPath));
			return FVaRestAuthRefreshResult::Refreshed(Access, Refresh, FMath::IsFinite(ExpiresIn) ? FMath::Max(0.0, ExpiresIn) : 0.0);
		}
		const FVaRestError& Error = Result.Error;
		const FString Why = Error.Kind == EVaRestErrorKind::Http ? FString::Printf(TEXT("the refresh endpoint answered %d"), Error.HttpStatus) : Error.Message;
		// Plan decision 3: only the server's "no" expires the session; a request canceled (CancelAll) or at shutdown is no answer either
		const bool bUnreachable = Error.Kind == EVaRestErrorKind::Connection || Error.Kind == EVaRestErrorKind::Timeout || Error.Kind == EVaRestErrorKind::Canceled
			|| (Error.Kind == EVaRestErrorKind::Http && (Error.HttpStatus == 408 || Error.HttpStatus == 429 || Error.HttpStatus >= 500));
		if (bUnreachable)
		{
			return FVaRestAuthRefreshResult::Unreachable(Error.Kind == EVaRestErrorKind::Timeout ? EVaRestErrorKind::Timeout : EVaRestErrorKind::Connection, Why);
		}
		return FVaRestAuthRefreshResult::Rejected(Why);
	}

	class FBearerRefreshAuth final : public IVaRestAuth
	{
	public:
		explicit FBearerRefreshAuth(const FVaRestBearerRefreshConfig& InConfig)
			: Config(InConfig)
		{
		}

		virtual FName GetAuthName() const override { return TEXT("BearerRefresh"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!Target.Session.HasAccessToken())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
			return true;
		}

		virtual bool CanRefresh(const FVaRestSessionData& Session) const override
		{
			return Session.HasRefreshToken();
		}

		virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const override
		{
			const double Lead = FMath::IsFinite(Config.RefreshLeadSeconds) ? FMath::Max(0.0, static_cast<double>(Config.RefreshLeadSeconds)) : 0.0;
			return Session.HasRefreshToken() && (!Session.HasAccessToken() || (Session.ExpiresAt > 0.0 && Now >= Session.ExpiresAt - Lead));
		}

		virtual bool IsAuthFailure(int32 Status) const override
		{
			return Config.RefreshStatuses.Contains(Status);
		}

		virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
		{
			const FString& Token = Context.GetSession().RefreshToken;
			TVaRestRequest<FVaRestJson> Refreshing = Context.Request<FVaRestJson>(TEXTVIEW("POST"), Config.RefreshPath);
			if (Config.Style == EVaRestRefreshStyle::OAuth2Form)
			{
				// RFC 6749 6
				FVaRestForm Form;
				Form.Add(TEXT("grant_type"), TEXT("refresh_token"));
				Form.Add(TEXT("refresh_token"), Token);
				if (!Config.ClientId.IsEmpty())
				{
					Form.Add(TEXT("client_id"), Config.ClientId);
				}
				Refreshing = MoveTemp(Refreshing).Body(Form);
			}
			else
			{
				Refreshing = MoveTemp(Refreshing).Body(FVaRestJson::Object().Set(Config.RefreshTokenField, Token));
			}
			MoveTemp(Refreshing).OnComplete([Paths = Config, Finish = MoveTemp(Done)](const TVaRestResult<FVaRestJson>& Result) mutable
			{
				Finish(ReadRefresh(Paths, Result));
			}).Send();
		}

		virtual void GetRedactedKeys(TArray<FString>& OutKeys) const override
		{
			// Plan decision 22: added keys match whole normalised keys (M3 decision 18), so the last segment, not the path
			for (const FString& Path : { Config.AccessTokenPath, Config.RefreshTokenPath })
			{
				const FString Segment = LastSegment(Path);
				if (!Segment.IsEmpty())
				{
					OutKeys.Add(Segment);
				}
			}
			if (!Config.RefreshTokenField.IsEmpty())
			{
				OutKeys.Add(Config.RefreshTokenField);
			}
		}

	private:
		FVaRestBearerRefreshConfig Config;
	};
}

FVaRestAuth::FRef FVaRestAuth::BearerRefresh(const FVaRestBearerRefreshConfig& Config)
{
	return MakeShared<VaRest::Http::BearerRefreshPrivate::FBearerRefreshAuth, ESPMode::ThreadSafe>(Config);
}
```

- [ ] **Step 4: Run them**

Run: `Tools/run_tests.sh VaRest.Http.Auth`
Expected: `0 failed`.

- [ ] **Step 5: Restore-the-bug** — (a) in `ReadRefresh` make every failure `Rejected`: `RefreshOutcomes` FAILS on the 503, 429, connection and timeout rows; restore; PASS. (b) In `GetRedactedKeys` add the whole path instead of `LastSegment(Path)`: `TokenPathKeys` FAILS; restore; PASS. (c) In `VaRestSession.cpp`, `UpdateTokens`, comment out `PublishSessionSecrets();`: `RefreshedTokenRedacted` FAILS (the X-Echo header and the body line hold the new token); restore; PASS.

- [ ] **Step 6: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestBearerRefresh.cpp Source/VaRestHttp/Public/VaRestAuth.h Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp
git commit -m "http: M4 task 14 -- BearerRefresh: the refresh request, JSON and OAuth2 form, token paths, rejected vs unreachable, redaction keys

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 15: Auth against the rest of the pipeline — replayed stream bodies, retry around the gate, the refresh first in the limiter (carries (c), (e))

Plan decisions 2, 8, 23, 24. The interplay M3's lessons list, each with a test.

**Files:**
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp`

**Interfaces:**
- Consumes: Tasks 7, 8, 12-14. Produces nothing new; a test that fails here names the task to fix.

- [ ] **Step 1: Write the tests** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp` (add `#include "HAL/FileManager.h"`, `#include "Misc/FileHelper.h"`, `#include "Misc/Guid.h"`, `#include "Misc/Paths.h"`, `#include "Misc/ScopeExit.h"`, `#include "VaRestRetry.h"`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthReplayBodiesTest, "VaRest.Http.Auth.ReplayBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthReplayBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Carry (e), plan decision 24: a session replay is a new transport request with the whole body, stream bodies
	// re-created -- NSURLSession could not resend a stream in place (AppleHttp.cpp:627-634, :751-753)
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestAuthTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
	TArray<uint8> Pattern;
	for (int32 Index = 0; Index < 1000; ++Index)
	{
		Pattern.Add(static_cast<uint8>(Index * 11 + 5));
	}
	const FString PartFile = Dir / TEXT("part.bin");
	const FString UploadFile = Dir / TEXT("upload.bin");
	if (!TestTrue(TEXT("fixtures written"), FFileHelper::SaveArrayToFile(Pattern, *PartFile) && FFileHelper::SaveArrayToFile(Pattern, *UploadFile)))
	{
		return false;
	}
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	RefreshServer(Rig, NewTokens());
	auto Parts = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	auto Posted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("f"), TEXT("v")).AddFile(TEXT("file"), PartFile)), Parts).Send();
	Observe(Rig.Client.Put(TEXT("/file")).BodyFile(UploadFile), File).Send();
	Observe(Rig.Client.Post(TEXT("/json")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), Posted).Send();
	if (!TestTrue(TEXT("all complete"), PumpUntil([Parts, File, Posted] { return Parts->IsDone() && File->IsDone() && Posted->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("all three succeed on their replay"), Parts->SuccessCalls.load() == 1 && File->SuccessCalls.load() == 1 && Posted->SuccessCalls.load() == 1);
	for (const TCHAR* Path : { TEXT("/parts"), TEXT("/file"), TEXT("/json") })
	{
		const int32 First = FindCall(Rig, Path, 0);
		const int32 Second = FindCall(Rig, Path, 1);
		if (!TestTrue(FString::Printf(TEXT("%s: two attempts"), Path), First != INDEX_NONE && Second != INDEX_NONE))
		{
			continue;
		}
		const TArray<uint8> Sent = Rig.Fake->ReadBody(First);
		TestTrue(FString::Printf(TEXT("%s: the replay sends the whole body again"), Path), Sent.Num() > 0 && Sent == Rig.Fake->ReadBody(Second));
	}

	// Plan decision 8: an auth that cannot refresh never replays, so its POST body is moved, not copied
	FTestRig Plain;
	Plain.Client.WithBearer(TEXT("old-access-123456"));
	RefreshServer(Plain, NewTokens());
	auto Once = MakeOutcome<FVaRestJson>();
	Observe(Plain.Client.Post(TEXT("/json")).Body(FVaRestJson::Object().Set(TEXT("a"), 1)), Once).Send();
	TestTrue(TEXT("plain Bearer: the 401 is the answer, one call"), Plain.Wait(Once) && Once->Error.HttpStatus == 401 && Plain.Fake->NumCalls() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRetryAroundTest, "VaRest.Http.Auth.RetryAroundTheGate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRetryAroundTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 2: Retry is outside AuthGate. An unreachable refresh fails the parked request as a Connection error,
	// which Retry retries; the next attempt meets the old token again and starts a new refresh, which works.
	FTestRig Rig;
	FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(2);
	Policy.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Policy).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	TSharedRef<int32, ESPMode::ThreadSafe> Refreshes = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([Refreshes](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.Contains(TEXT("/auth/")))
		{
			return ++*Refreshes == 1 ? Json(503) : NewTokens();
		}
		return Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200) : Json(401);
	});
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	if (!TestTrue(TEXT("the refresh failed; Retry backs off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("the session survived the unreachable refresh"), Rig.Client.GetSession().HasRefreshToken());
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("a success: attempt 1 (401), the retry (401), its replay (200)"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 3);
	TestEqual(TEXT("two refreshes"), *Refreshes, 2);
	TestEqual(TEXT("calls: /p, refresh, /p, refresh, /p"), Rig.Fake->NumCalls(), 5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthJumpsQueueTest, "VaRest.Http.Auth.RefreshJumpsTheQueue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthJumpsQueueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 23 with spec 4.6: one slot; A answers 401 while B and C wait. A's freed slot goes to B (it waited);
	// the refresh, Highest, then goes before C.
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithAuth(FVaRestAuth::BearerRefresh(FVaRestBearerRefreshConfig()));
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")).NoAuth(), B).Send();
	Observe(Rig.Client.Get(TEXT("/c")).NoAuth(), C).Send();
	if (!TestTrue(TEXT("A holds the slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(0, Json(401));
	if (!TestTrue(TEXT("B gets it"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(1, Json(200));
	if (!TestTrue(TEXT("then the refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... before C"), Rig.Fake->GetCall(2).Request.Url.Contains(TEXT("/auth/refresh")));
	Rig.Fake->Complete(2, NewTokens());
	// C, then A's replay: both Normal, FIFO -- C was queued first
	for (int32 Index = 3; Index < 5; ++Index)
	{
		if (!TestTrue(FString::Printf(TEXT("call %d"), Index + 1), PumpUntil([&Rig, Index] { return Rig.Fake->NumCalls() == Index + 1; })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Fake->Complete(Index, Json(200));
	}
	TestTrue(TEXT("all complete"), PumpUntil([A, B, C] { return A->IsDone() && B->IsDone() && C->IsDone(); }));
	TestTrue(TEXT("A succeeds on its replay"), A->SuccessCalls.load() == 1 && A->Response.Attempts == 2);
	return true;
}
```

- [ ] **Step 2: Run them**

Run: `Tools/run_tests.sh VaRest.Http.Auth`
Expected: `0 failed`. If one fails, the behaviour it names is missing from the task named in its comment: fix it there (with that task's restore-the-bug), not by editing the test.

- [ ] **Step 3: Restore-the-bug** — (a) in `Resolve` drop `|| bAuthMayReplay`: `ReplayBodies` FAILS (the multipart and JSON replays send empty bodies); restore; PASS. (b) In `MarkAuthRefresh` drop the `Priority = Highest` line: `RefreshJumpsTheQueue` FAILS (C goes before the refresh); restore; PASS. (c) In `Publish` put `MakeAuthGateStage()` before `MakeRetryStage()`: `RetryAroundTheGate` FAILS (the unreachable refresh is final); restore; PASS.

- [ ] **Step 4: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp
git commit -m "http tests: M4 task 15 -- auth against the pipeline: replayed stream bodies, retry around the gate, the refresh first in the limiter

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 16: `stat VaRest` and the LLM tags (spec §4.8; plan decision 21)

Spec §4.8: "Stats: `stat VaRest` (requests, in-flight, bytes up/down, retries, cache hit ratio) and an LLM tag `VaRest`." The cache hit ratio is M4b's (it has no cache yet). Inspector IN-14 adds the tag `VaRest_Trace` for the trace's copies (Task 17 uses it).

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestHttpStats.h`, `Source/VaRestHttp/Private/VaRestHttpStatsInternal.h`, `Source/VaRestHttp/Private/VaRestHttpStats.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Enter`, `Complete`, `OnAttemptDone`, the decode task), `Source/VaRestHttp/Private/VaRestRetry.cpp` (retries), `Source/VaRestHttp/Private/VaRestClient.cpp` (`FRequestSpecAccess::Send`), `Source/VaRestHttp/Private/VaRestHttpTransport.cpp` (`Start`, the body stream), `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (the trace tag)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpStatsTest.cpp`

**Interfaces:**
- Consumes: Task 7 (the retry stage).
- Produces: public `LLM_DECLARE_TAG_API(VaRest, VARESTHTTP_API)`, `LLM_DECLARE_TAG_API(VaRest_Trace, VARESTHTTP_API)`; private `struct VaRest::Http::FHttpStats { std::atomic<int64> Requests, InFlight, BytesUp, BytesDown, Retries; static FHttpStats& Get(); void RequestStarted(); void RequestFinished(); void Transferred(int64 Up, int64 Down); void Retried(); }`. M4b adds `CacheLookups`, `CacheHits` and the ratio.

- [ ] **Step 1: Write the failing test** — create `Source/VaRestHttp/Private/Tests/VaRestHttpStatsTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStatsCountersTest, "VaRest.Http.Stats.Counters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStatsCountersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(1);
	Policy.Backoff.Jitter = EVaRestJitter::None;
	Rig.Client.WithRetry(Policy);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(503, FUtf8StringView(UTF8TEXT("{}"))));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{\"a\":1}"))));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))));
	// Process-wide counters: earlier tests' late work lands first, then the baseline
	Rig.Drain();
	VaRest::Http::FHttpStats& Stats = VaRest::Http::FHttpStats::Get();
	const int64 Requests = Stats.Requests.load();
	const int64 InFlight = Stats.InFlight.load();
	const int64 Down = Stats.BytesDown.load();
	const int64 Up = Stats.BytesUp.load();
	const int64 Retries = Stats.Retries.load();
	auto Retried = MakeOutcome<FVaRestJson>();
	auto Posted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/r")), Retried).Send();
	if (!TestTrue(TEXT("backing off"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	TestEqual(TEXT("in flight while it waits"), Stats.InFlight.load() - InFlight, static_cast<int64>(1));
	Observe(Rig.Client.Post(TEXT("/p")).Body(TEXT("12345")), Posted).Send();
	// A silent request (its owner is gone at entry): counted in and out like any other
	UObject* Gone = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	Gone->MarkAsGarbage();
	auto Silent = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/gone")).Owner(Gone), Silent).Send();
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("both complete"), PumpUntil([Retried, Posted] { return Retried->IsDone() && Posted->IsDone(); })))
	{
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("three requests (a retry is not a request)"), Stats.Requests.load() - Requests, static_cast<int64>(3));
	TestEqual(TEXT("one retry"), Stats.Retries.load() - Retries, static_cast<int64>(1));
	TestEqual(TEXT("none in flight after"), Stats.InFlight.load(), InFlight);
	TestEqual(TEXT("bytes down: every attempt's body (2 + 7 + 2)"), Stats.BytesDown.load() - Down, static_cast<int64>(11));
	TestEqual(TEXT("bytes up: the POST body"), Stats.BytesUp.load() - Up, static_cast<int64>(5));
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Stats`
Expected: FAIL to compile (`VaRestHttpStatsInternal.h` not found).

- [ ] **Step 3: The tags and the counters** — create `Source/VaRestHttp/Public/VaRestHttpStats.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"

/**
 * Spec 4.8: LLM tag VaRest (requests, bodies, decodes); VaRest_Trace ("VaRest/Trace") for the trace's copies and excerpts
 * (Inspector IN-14). Run with -llm to see them; another module scopes its own work with LLM_SCOPE_BYTAG(VaRest).
 */
LLM_DECLARE_TAG_API(VaRest, VARESTHTTP_API);
LLM_DECLARE_TAG_API(VaRest_Trace, VARESTHTTP_API);
```

Create `Source/VaRestHttp/Private/VaRestHttpStatsInternal.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpStats.h"

#include <atomic>

namespace VaRest::Http
{
	/**
	 * Spec 4.8's stat VaRest (plan decision 21): VaRest's own counters, process-wide, any thread (relaxed atomics), mirrored to
	 * the engine's stats where STATS is on. Engine stats are compiled out in Shipping and a test cannot read them back.
	 */
	struct FHttpStats
	{
		/** Logical requests that entered a pipeline */
		std::atomic<int64> Requests{ 0 };
		std::atomic<int64> InFlight{ 0 };
		std::atomic<int64> BytesUp{ 0 };
		std::atomic<int64> BytesDown{ 0 };
		/** Re-issues by the retry stage (a session replay is not one, plan decision 2) */
		std::atomic<int64> Retries{ 0 };

		static FHttpStats& Get();

		void RequestStarted();
		void RequestFinished();
		/** One attempt's bytes */
		void Transferred(int64 Up, int64 Down);
		void Retried();
	};
}
```

Create `Source/VaRestHttp/Private/VaRestHttpStats.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpStatsInternal.h"

#include "Stats/Stats.h"

LLM_DEFINE_TAG(VaRest);
LLM_DEFINE_TAG(VaRest_Trace, TEXT("Trace"), TEXT("VaRest"));

DECLARE_STATS_GROUP(TEXT("VaRest"), STATGROUP_VaRest, STATCAT_Advanced);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Requests"), STAT_VaRestRequests, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("In flight"), STAT_VaRestInFlight, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Bytes up"), STAT_VaRestBytesUp, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Bytes down"), STAT_VaRestBytesDown, STATGROUP_VaRest);
DECLARE_DWORD_ACCUMULATOR_STAT(TEXT("Retries"), STAT_VaRestRetries, STATGROUP_VaRest);

VaRest::Http::FHttpStats& VaRest::Http::FHttpStats::Get()
{
	// Atomics only: trivially destructible, but leaked like every singleton here (review I-4)
	static FHttpStats* Instance = new FHttpStats();
	return *Instance;
}

void VaRest::Http::FHttpStats::RequestStarted()
{
	Requests.fetch_add(1, std::memory_order_relaxed);
	InFlight.fetch_add(1, std::memory_order_relaxed);
	INC_DWORD_STAT(STAT_VaRestRequests);
	INC_DWORD_STAT(STAT_VaRestInFlight);
}

void VaRest::Http::FHttpStats::RequestFinished()
{
	InFlight.fetch_sub(1, std::memory_order_relaxed);
	DEC_DWORD_STAT(STAT_VaRestInFlight);
}

void VaRest::Http::FHttpStats::Transferred(int64 Up, int64 Down)
{
	BytesUp.fetch_add(Up, std::memory_order_relaxed);
	BytesDown.fetch_add(Down, std::memory_order_relaxed);
	INC_DWORD_STAT_BY(STAT_VaRestBytesUp, Up);
	INC_DWORD_STAT_BY(STAT_VaRestBytesDown, Down);
}

void VaRest::Http::FHttpStats::Retried()
{
	Retries.fetch_add(1, std::memory_order_relaxed);
	INC_DWORD_STAT(STAT_VaRestRetries);
}
```

- [ ] **Step 4: Count and tag** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`#include "VaRestHttpStatsInternal.h"`):
- `FClientState::Enter`: first line after `check(IsInGameThread());` is `LLM_SCOPE_BYTAG(VaRest);`; after `InFlight.Add(Ex.RequestId, Exchange);` add `FHttpStats::Get().RequestStarted();`.
- `FClientState::Complete`: after `InFlight.Remove(Ex.RequestId);` add `FHttpStats::Get().RequestFinished();`.
- `FClientState::OnAttemptDone`: `LLM_SCOPE_BYTAG(VaRest);` first, and after `Ex.TransportCall.Reset();` add `FHttpStats::Get().Transferred(Result.BytesUp, Result.BytesDown);`.
- The decode task lambda in `OnChainDone`: `LLM_SCOPE_BYTAG(VaRest);` as its first line.

In `Source/VaRestHttp/Private/VaRestRetry.cpp`, `OnAttempt`, after `++Ex.RetriesDone;`: `FHttpStats::Get().Retried();` (`#include "VaRestHttpStatsInternal.h"`). In `Source/VaRestHttp/Private/VaRestClient.cpp`, `FRequestSpecAccess::Send` starts with `LLM_SCOPE_BYTAG(VaRest);` (`#include "VaRestHttpStats.h"`). In `Source/VaRestHttp/Private/VaRestHttpTransport.cpp`: `FHttpModuleTransport::Start` and the `SetResponseBodyReceiveStreamDelegateV2` lambda start with `LLM_SCOPE_BYTAG(VaRest);` (`#include "VaRestHttpStats.h"`). In `Source/VaRestHttp/Private/VaRestTracePoints.cpp`, each trace point gets `LLM_SCOPE_BYTAG(VaRest_Trace);` right after its mask check (`#include "VaRestHttpStats.h"`).

- [ ] **Step 5: Run it, then the whole suite**

Run: `Tools/run_tests.sh VaRest.Http.Stats` then `Tools/run_tests.sh --no-build VaRest`
Expected: `0 failed` both.

- [ ] **Step 6: Restore-the-bug** — in `Complete`, move `FHttpStats::Get().RequestFinished();` from after `InFlight.Remove` to after the `if (!Handler.IsValid() || bSilent) { return; }` line: `VaRest.Http.Stats.Counters` FAILS ("none in flight after": the silent request never decrements). Restore; PASS.

- [ ] **Step 7: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestHttpStats.h Source/VaRestHttp/Private/VaRestHttpStatsInternal.h Source/VaRestHttp/Private/VaRestHttpStats.cpp \
  Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestRetry.cpp Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/VaRestHttpTransport.cpp Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Private/Tests/VaRestHttpStatsTest.cpp
git commit -m "http: M4 task 16 -- stat VaRest (requests, in flight, bytes, retries) and the LLM tags VaRest and VaRest_Trace (spec 4.8)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 17: Trace excerpts (Inspector IN-6, IN-14; plan decision 20)

IN-14: "A request's prefix is copied at `Send` (up to the cap). A decoded response is redacted on the decode task before `ParseOwned` parses it in situ, with zero copies. An undecoded response (raw, `void`, bytes) has its prefix copied at the game-thread hop and redacted on the core `FPipe`. Raw copies waiting for redaction are bounded by an admission counter (the sink detail sets it; default 16 MiB); over it, `Excerpt{Skipped: Backlog}`. LLM tag `VaRest_Trace`. Compiled under `VAREST_WITH_TRACE_EXCERPTS`." Logs keep their own excerpts (M3 Task 14); this is the trace's.

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestTraceExcerpts.h`, `Source/VaRestHttp/Private/VaRestTraceExcerpts.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestTracePoints.h`, `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (`Started`, `AttemptEnded`)
- Modify: `Source/VaRestHttp/Private/VaRestExchangeState.h`, `Source/VaRestHttp/Public/VaRestResultHandler.h` (`FFinish` excerpt fields), `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`OnChainDone`, `RunDecode`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpExcerptTest.cpp`

**Interfaces:**
- Consumes: Tasks 3 (`LaunchRedaction`), 4 (`FVaRestTraceExcerptEvent`, `MaxPendingExcerptBytes`), 16 (`VaRest_Trace`).
- Produces: private `namespace VaRest::Http::Excerpts { int64 AdmissionCap(); int64 PendingBytes(); struct FJob { ...; bool IsSet() const; void Launch(); }; FJob PrepareRequest(const FExchangeState&); bool WillExcerptOnDecode(const FExchangeState&, const FVaRestTransportResult&); FJob PrepareResponse(const FExchangeState&, const FVaRestTransportResult&); void EmitOnDecode(const FFinish&); }`; `FExchangeState::ExcerptOnDecodeAttempt`; `FFinish::ExcerptAttempt`, `ExcerptRedactor`, `ExcerptCap`; `TracePoints::AttemptEnded` now takes `FExchangeState&`.

- [ ] **Step 1: Write the failing tests** — create `Source/VaRestHttp/Private/Tests/VaRestHttpExcerptTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestSha256.h"
#include "VaRestTraceExcerpts.h"

namespace VaRest::Http::ExcerptTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text)
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	const FVaRestTraceExcerptEvent* Find(const TArray<FVaRestTraceExcerptEvent>& Excerpts, EVaRestTraceExcerptOwner Owner)
	{
		return Excerpts.FindByPredicate([Owner](const FVaRestTraceExcerptEvent& One) { return One.Owner == Owner; });
	}

	/** The first event of Kind for request Id, its payload copied out (For() returns a copy of the list) */
	template <typename PayloadType>
	TOptional<PayloadType> First(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, EVaRestTraceKind Kind)
	{
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == Kind)
			{
				return Event.Payload.Get<PayloadType>();
			}
		}
		return TOptional<PayloadType>();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptsTest, "VaRest.Http.Trace.Excerpts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestExcerptTest") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Dir, true);
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, false, true); };
	const FString Upload = Dir / TEXT("upload.bin");
	if (!TestTrue(TEXT("fixture written"), FFileHelper::SaveStringToFile(TEXT("abc"), *Upload)))
	{
		return false;
	}
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/login")))
		{
			return Json(200, R"({"accessToken":"tok-abcdef1234","name":"neo"})");
		}
		if (Request.Url.EndsWith(TEXT("/bytes")))
		{
			return FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 1, 2, 3, 4 });
		}
		if (Request.Url.EndsWith(TEXT("/missing")))
		{
			return FVaRestFakeResponse::Text(404, TEXT("no such thing"));
		}
		return Json(200, "{}");
	});
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, /*BodyCap*/ 1024);
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	auto Login = MakeOutcome<FVaRestJson>();
	auto Bytes = MakeOutcome<TArray<uint8>>();
	auto Missing = MakeOutcome<FVaRestJson>();
	auto Parts = MakeOutcome<FVaRestJson>();
	auto File = MakeOutcome<FVaRestJson>();
	auto Empty = MakeOutcome<FVaRestJson>();
	const uint64 LoginId = Observe(Rig.Client.Post(TEXT("/login")).Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("trinity-123"))), Login).Send().GetId();
	const uint64 BytesId = Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/bytes")), Bytes).Send().GetId();
	const uint64 MissingId = Observe(Rig.Client.Get(TEXT("/missing")), Missing).Send().GetId();
	const uint64 PartsId = Observe(Rig.Client.Post(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("secret"), TEXT("shh-123456")).AddField(TEXT("name"), TEXT("neo"))), Parts).Send().GetId();
	const uint64 FileId = Observe(Rig.Client.Put(TEXT("/file")).BodyFile(Upload), File).Send().GetId();
	const uint64 EmptyId = Observe(Rig.Client.Get(TEXT("/empty")), Empty).Send().GetId();
	if (!TestTrue(TEXT("all complete"), PumpUntil([&] { return Login->IsDone() && Bytes->IsDone() && Missing->IsDone() && Parts->IsDone() && File->IsDone() && Empty->IsDone(); })))
	{
		return false;
	}
	FlushRedactionPipe();
	Rig.Drain();

	const TArray<FVaRestTraceExcerptEvent> LoginExcerpts = Recorder.ExcerptsOf(LoginId);
	const FVaRestTraceExcerptEvent* Sent = Find(LoginExcerpts, EVaRestTraceExcerptOwner::Request);
	const FVaRestTraceExcerptEvent* Got = Find(LoginExcerpts, EVaRestTraceExcerptOwner::Response);
	if (!TestTrue(TEXT("IN-6: a request and a response excerpt"), Sent && Got))
	{
		return false;
	}
	TestTrue(TEXT("the request's, before any attempt, redacted"), Sent->Attempt == 0 && Sent->Excerpt.Kind == EVaRestExcerptKind::Json
		&& Sent->Excerpt.ToText().Contains(TEXT("\"password\":\"***\"")) && Sent->Excerpt.ToText().Contains(TEXT("\"user\":\"neo\"")));
	TestTrue(TEXT("the response's, for attempt 1, redacted on the decode task"), Got->Attempt == 1 && Got->Excerpt.ToText().Contains(TEXT("\"accessToken\":\"***\""))
		&& !Got->Excerpt.ToText().Contains(TEXT("tok-abcdef1234")));
	TestTrue(TEXT("... while the decode still read the real body"), Login->Value.IsSet() && Login->Value->Get(TEXT("name")).AsString() == TEXT("neo"));
	const TOptional<FVaRestTraceStarted> LoginStarted = First<FVaRestTraceStarted>(Recorder, LoginId, EVaRestTraceKind::Started);
	TestTrue(TEXT("IN-1: Started says an excerpt is coming"), LoginStarted.IsSet() && LoginStarted->bExcerptPending);
	const TOptional<FVaRestTraceAttemptEnded> LoginEnded = First<FVaRestTraceAttemptEnded>(Recorder, LoginId, EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("IN-3: so does AttemptEnded"), LoginEnded.IsSet() && LoginEnded->bExcerptPending);

	const FVaRestTraceExcerptEvent* Binary = Find(Recorder.ExcerptsOf(BytesId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("raw bytes: a descriptor with the SHA-256 prefix (copied at the hop, the whole body under the cap)"), Binary
		&& Binary->Excerpt.Kind == EVaRestExcerptKind::Binary && Binary->Excerpt.Sha256Prefix == VaRest::Http::FSha256::Hash(TArray<uint8>{ 1, 2, 3, 4 }).ToHex().Left(8));
	const FVaRestTraceExcerptEvent* Error = Find(Recorder.ExcerptsOf(MissingId), EVaRestTraceExcerptOwner::Response);
	TestTrue(TEXT("an error's body: text"), Error && Error->Excerpt.Kind == EVaRestExcerptKind::Text && Error->Excerpt.ToText() == TEXT("no such thing"));
	const FVaRestTraceExcerptEvent* Multipart = Find(Recorder.ExcerptsOf(PartsId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("multipart: parts, a secret field redacted"), Multipart && Multipart->Excerpt.Kind == EVaRestExcerptKind::Multipart
		&& !Multipart->Excerpt.ToText().Contains(TEXT("shh-123456")));
	const FVaRestTraceExcerptEvent* Stream = Find(Recorder.ExcerptsOf(FileId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("a BodyFile: a stream descriptor"), Stream && Stream->Excerpt.Kind == EVaRestExcerptKind::Stream && Stream->Excerpt.TotalBytes == 3);
	const TOptional<FVaRestTraceStarted> EmptyStarted = First<FVaRestTraceStarted>(Recorder, EmptyId, EVaRestTraceKind::Started);
	TestTrue(TEXT("no body: nothing pending"), EmptyStarted.IsSet() && !EmptyStarted->bExcerptPending && Find(Recorder.ExcerptsOf(EmptyId), EVaRestTraceExcerptOwner::Request) == nullptr);
	TestTrue(TEXT("Inspector 9: only excerpts arrive off the game thread"), Recorder.OnlyExcerptsOffGameThread());
	TestEqual(TEXT("every admitted copy released"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptBacklogTest, "VaRest.Http.Trace.ExcerptBacklog",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptBacklogTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	FTestRig Rig;
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Bodies, 4096);
	Scoped.Recorder->Detail.MaxPendingExcerptBytes = 100;
	FVaRestTrace::RefreshDetail();
	auto Big = MakeOutcome<FVaRestJson>();
	auto Small = MakeOutcome<FVaRestJson>();
	const uint64 BigId = Observe(Rig.Client.Post(TEXT("/big")).Body(FString::ChrN(1000, TEXT('x'))), Big).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Big)))
	{
		return false;
	}
	const uint64 SmallId = Observe(Rig.Client.Post(TEXT("/small")).Body(FString::ChrN(50, TEXT('y'))), Small).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Small)))
	{
		return false;
	}
	FlushRedactionPipe();
	const FVaRestTraceExcerptEvent* Skipped = Find(Scoped.Recorder->ExcerptsOf(BigId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("IN-14: past the admission cap, Skipped: Backlog, nothing copied"), Skipped && Skipped->Excerpt.Kind == EVaRestExcerptKind::Skipped
		&& Skipped->Excerpt.SkipReason == EVaRestExcerptSkip::Backlog && Skipped->Excerpt.TotalBytes == 1000);
	const FVaRestTraceExcerptEvent* Admitted = Find(Scoped.Recorder->ExcerptsOf(SmallId), EVaRestTraceExcerptOwner::Request);
	TestTrue(TEXT("under it: the excerpt"), Admitted && Admitted->Excerpt.Kind == EVaRestExcerptKind::Text);
	TestEqual(TEXT("the counter is back at 0"), VaRest::Http::Excerpts::PendingBytes(), static_cast<int64>(0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpExcerptNeedBodiesTest, "VaRest.Http.Trace.ExcerptsNeedBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpExcerptNeedBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::ExcerptTestPrivate;
	// IN-11: without Bodies in the mask no byte is copied for the trace
	FTestRig Rig;
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Post(TEXT("/x")).Body(TEXT("hello")), Outcome).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		return false;
	}
	FlushRedactionPipe();
	const TOptional<FVaRestTraceStarted> Started = First<FVaRestTraceStarted>(*Scoped.Recorder, Id, EVaRestTraceKind::Started);
	TestTrue(TEXT("nothing pending, nothing sent"), Started.IsSet() && !Started->bExcerptPending && Scoped.Recorder->ExcerptsOf(Id).Num() == 0);
	return true;
}

#endif
```

- [ ] **Step 2: Build to see it fail**

Run: `Tools/run_tests.sh VaRest.Http.Trace.Excerpt`
Expected: FAIL to compile (`VaRestTraceExcerpts.h` not found).

- [ ] **Step 3: The excerpt module** — create `Source/VaRestHttp/Private/VaRestTraceExcerpts.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestTrace.h"

struct FVaRestTransportResult;
class FVaRestRedactor;

namespace VaRest::Http
{
	struct FExchangeState;
	struct FFinish;

	/**
	 * Inspector IN-14 (plan decision 20). Nothing here copies or builds anything unless a sink wants Bodies and
	 * VAREST_WITH_TRACE_EXCERPTS is on. LLM tag VaRest_Trace.
	 */
	namespace Excerpts
	{
		/** The sinks' MaxPendingExcerptBytes, else 16 MiB */
		int64 AdmissionCap();
		/** Bytes of raw copies waiting for redaction now (tests) */
		int64 PendingBytes();

		/** An excerpt prepared on the game thread and launched right after the event that announces it (bExcerptPending) */
		struct FJob
		{
			EVaRestTraceExcerptOwner Owner = EVaRestTraceExcerptOwner::Request;
			uint64 RequestId = 0;
			int32 Attempt = 0;
			int64 TotalBytes = 0;
			/** Bytes counted in by the admission counter, counted out once the redaction ran */
			int64 Admitted = 0;
			/** The copy would pass the admission cap: Skipped, Backlog, emitted at once */
			bool bBacklog = false;
			/** IVaRestClock seconds of the hop: the excerpt's Time (the clock itself stays on the game thread, review I-1) */
			double Time = 0.0;
			TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor;
			/** Runs on the redaction pipe */
			TUniqueFunction<FVaRestTraceExcerpt(const FVaRestRedactor&)> Redact;

			bool IsSet() const { return bBacklog || static_cast<bool>(Redact); }
			/** Game thread: the skip at once, or the redaction on the pipe; nothing for an unset job */
			void Launch();
		};

		/** At Started: IN-14's "copied at Send" (after Resolve, before the transport stage can move the bytes) */
		FJob PrepareRequest(const FExchangeState& Ex);
		/** At AttemptEnded: the decode task will excerpt this response from the body itself (a 2xx a decoder parses) */
		bool WillExcerptOnDecode(const FExchangeState& Ex, const FVaRestTransportResult& Result);
		/** At AttemptEnded, every other response: its prefix copied here, redacted on the pipe */
		FJob PrepareResponse(const FExchangeState& Ex, const FVaRestTransportResult& Result);
		/** On the decode task, before the parse rewrites the body in place: no copy (IN-14) */
		void EmitOnDecode(const FFinish& Finish);
	}
}
```

Create `Source/VaRestHttp/Private/VaRestTraceExcerpts.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTraceExcerpts.h"

#include "VaRestClientState.h"
#include "VaRestHttpStats.h"
#include "VaRestRedactionPipe.h"
#include "VaRestResultHandler.h"
#include "VaRestTransport.h"

#include <atomic>

namespace VaRest::Http::ExcerptsPrivate
{
	constexpr int64 DefaultAdmission = 16 * 1024 * 1024;

	/** Trivially destructible: safe as a static (review I-4) */
	std::atomic<int64> Pending{ 0 };

	bool Wanted()
	{
#if VAREST_WITH_TRACE_EXCERPTS
		return FVaRestTrace::Wants(EVaRestTraceDetail::Bodies);
#else
		return false;
#endif
	}

	/** Counts Bytes in when they fit under the cap */
	bool Admit(int64 Bytes)
	{
		const int64 Cap = Excerpts::AdmissionCap();
		int64 Seen = Pending.load();
		do
		{
			if (Seen + Bytes > Cap)
			{
				return false;
			}
		}
		while (!Pending.compare_exchange_weak(Seen, Seen + Bytes));
		return true;
	}

	void Emit(EVaRestTraceExcerptOwner Owner, uint64 RequestId, int32 Attempt, FVaRestTraceExcerpt&& Excerpt, double Time)
	{
		FVaRestTraceExcerptEvent Payload;
		Payload.Owner = Owner;
		Payload.RequestId = RequestId;
		Payload.Attempt = Attempt;
		Payload.Excerpt = MoveTemp(Excerpt);
		FVaRestTrace::Emit(EVaRestTraceKind::Excerpt, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(), MoveTemp(Payload)));
	}

	/** At most Cap bytes copied and admitted, or a backlog skip */
	Excerpts::FJob WithCopy(Excerpts::FJob&& Job, TConstArrayView<uint8> Bytes, int32 Cap, FString ContentType)
	{
		const int32 Take = FMath::Min(Bytes.Num(), FMath::Max(0, Cap));
		if (!Admit(Take))
		{
			Job.bBacklog = true;
			return MoveTemp(Job);
		}
		Job.Admitted = Take;
		Job.Redact = [Prefix = TArray<uint8>(Bytes.GetData(), Take), Type = MoveTemp(ContentType), Total = Job.TotalBytes, Cap](const FVaRestRedactor& Redactor)
		{
			return Redactor.RedactBody(Type, Prefix, Total, Cap);
		};
		return MoveTemp(Job);
	}

	/** The parts with their field bytes cut to Budget in all, as the log copies them (VaRestLogStage.cpp, review M3) */
	TArray<FVaRestMultipartPart> CopyParts(TConstArrayView<FVaRestMultipartPart> Parts, int64 Budget, int64& OutCopied)
	{
		OutCopied = 0;
		TArray<FVaRestMultipartPart> Copies;
		Copies.Reserve(Parts.Num());
		for (const FVaRestMultipartPart& Part : Parts)
		{
			FVaRestMultipartPart& Copied = Copies.AddDefaulted_GetRef();
			Copied.Name = Part.Name;
			Copied.FileName = Part.FileName;
			Copied.ContentType = Part.ContentType;
			Copied.FilePath = Part.FilePath;
			Copied.Size = Part.Size;
			Copied.bField = Part.bField;
			const int32 Take = static_cast<int32>(FMath::Min<int64>(Part.Bytes.Num(), FMath::Max<int64>(0, Budget - OutCopied)));
			Copied.Bytes.Append(Part.Bytes.GetData(), Take);
			OutCopied += Take;
		}
		return Copies;
	}
}

int64 VaRest::Http::Excerpts::AdmissionCap()
{
	const int64 Asked = FVaRestTrace::GetDetailSettings().MaxPendingExcerptBytes;
	return Asked > 0 ? Asked : ExcerptsPrivate::DefaultAdmission;
}

int64 VaRest::Http::Excerpts::PendingBytes()
{
	return ExcerptsPrivate::Pending.load();
}

void VaRest::Http::Excerpts::FJob::Launch()
{
	using namespace ExcerptsPrivate;
	if (bBacklog)
	{
		FVaRestTraceExcerpt Skipped;
		Skipped.Kind = EVaRestExcerptKind::Skipped;
		Skipped.SkipReason = EVaRestExcerptSkip::Backlog;
		Skipped.TotalBytes = TotalBytes;
		Skipped.bTruncated = TotalBytes > 0;
		Emit(Owner, RequestId, Attempt, MoveTemp(Skipped), Time);
		return;
	}
	if (!Redact)
	{
		return;
	}
	const bool bLaunched = LaunchRedaction(TEXT("VaRest.TraceExcerpt"),
		[ForOwner = Owner, Id = RequestId, ForAttempt = Attempt, Bytes = Admitted, At = Time, Redacting = Redactor, Work = MoveTemp(Redact)]() mutable
		{
			LLM_SCOPE_BYTAG(VaRest_Trace);
			FVaRestTraceExcerpt Excerpt = Work(*Redacting);
			Pending.fetch_sub(Bytes);
			Emit(ForOwner, Id, ForAttempt, MoveTemp(Excerpt), At);
		});
	if (!bLaunched)
	{
		// The pipe is stopping (engine exit): the excerpt never comes; the Inspector shows "excerpt timed out" (its 2.3)
		Pending.fetch_sub(Admitted);
	}
}

VaRest::Http::Excerpts::FJob VaRest::Http::Excerpts::PrepareRequest(const FExchangeState& Ex)
{
	using namespace ExcerptsPrivate;
	FJob Job;
	if (!Wanted() || Ex.Body.IsEmpty())
	{
		return Job;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	const FVaRestTraceDetailSettings Detail = FVaRestTrace::GetDetailSettings();
	Job.Owner = EVaRestTraceExcerptOwner::Request;
	Job.RequestId = Ex.RequestId;
	Job.TotalBytes = Ex.Body.ContentLength;
	Job.Time = Ex.Config->Clock->Now();
	Job.Redactor = Ex.State->GetRedactor();
	const FVaRestRequestBody& Body = Ex.Body;
	if (!Body.FilePath.IsEmpty())
	{
		Job.Redact = [Path = Body.FilePath, Type = Body.ContentType, Total = Body.ContentLength](const FVaRestRedactor& Redactor)
		{
			return Redactor.DescribeStream(Path, Type, Total);
		};
		return Job;
	}
	if (Body.Kind == EVaRestBodyKind::Multipart)
	{
		int64 Copied = 0;
		TArray<FVaRestMultipartPart> Parts = CopyParts(Body.PartDescriptors, static_cast<int64>(Detail.BodyCap) + Job.Redactor->GetMaxNeedleBytes(), Copied);
		if (!Admit(Copied))
		{
			Job.bBacklog = true;
			return Job;
		}
		Job.Admitted = Copied;
		Job.Redact = [Described = MoveTemp(Parts), Total = Body.ContentLength, Cap = Detail.BodyCap](const FVaRestRedactor& Redactor)
		{
			return Redactor.DescribeMultipart(Described, Total, Cap);
		};
		return Job;
	}
	// The request's own Content-Type wins (spec 3.2)
	const FString* Type = Ex.RequestHeaders.Find(TEXT("Content-Type"));
	return WithCopy(MoveTemp(Job), Body.Bytes, Detail.BodyCap, Type ? *Type : Body.ContentType);
}

bool VaRest::Http::Excerpts::WillExcerptOnDecode(const FExchangeState& Ex, const FVaRestTransportResult& Result)
{
	return ExcerptsPrivate::Wanted() && Result.Failure == EVaRestTransportFailure::None && IsSuccessStatus(Result.Status) && !Result.bBodyInFile
		&& Result.Body.Num() > 0 && Ex.Handler.IsValid() && Ex.Handler->DecodesBody();
}

VaRest::Http::Excerpts::FJob VaRest::Http::Excerpts::PrepareResponse(const FExchangeState& Ex, const FVaRestTransportResult& Result)
{
	using namespace ExcerptsPrivate;
	FJob Job;
	if (!Wanted() || (!Result.bBodyInFile && Result.Body.Num() == 0))
	{
		return Job;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	Job.Owner = EVaRestTraceExcerptOwner::Response;
	Job.RequestId = Ex.RequestId;
	Job.Attempt = Ex.Attempt;
	Job.TotalBytes = Result.bBodyInFile ? Result.BytesDown : Result.Body.Num();
	Job.Time = Ex.Config->Clock->Now();
	Job.Redactor = Ex.State->GetRedactor();
	if (Result.bBodyInFile)
	{
		// Inspector 2.2: a streamed body is a descriptor; its path is the destination the game named
		Job.Redact = [Path = Ex.SaveTo, Type = Result.ContentType, Total = Job.TotalBytes](const FVaRestRedactor& Redactor)
		{
			return Redactor.DescribeStream(Path, Type, Total);
		};
		return Job;
	}
	return WithCopy(MoveTemp(Job), Result.Body, FVaRestTrace::GetDetailSettings().BodyCap, Result.ContentType);
}

void VaRest::Http::Excerpts::EmitOnDecode(const FFinish& Finish)
{
	if (Finish.ExcerptAttempt <= 0 || !Finish.ExcerptRedactor.IsValid() || Finish.Body.Num() == 0)
	{
		return;
	}
	LLM_SCOPE_BYTAG(VaRest_Trace);
	// The body itself, before ParseOwned unescapes it in place: IN-14's zero copies
	const int32 Take = FMath::Min(Finish.Body.Num(), FMath::Max(0, Finish.ExcerptCap));
	FVaRestTraceExcerpt Excerpt = Finish.ExcerptRedactor->RedactBody(Finish.Response.ContentType, TConstArrayView<uint8>(Finish.Body.GetData(), Take), Finish.Body.Num(),
		Finish.ExcerptCap);
	ExcerptsPrivate::Emit(EVaRestTraceExcerptOwner::Response, Finish.Response.RequestId, Finish.ExcerptAttempt, MoveTemp(Excerpt),
		Finish.Clock.IsValid() ? Finish.Clock->Now() : 0.0);
}
```

- [ ] **Step 4: Wire it in** — `Source/VaRestHttp/Private/VaRestExchangeState.h`, after the auth block: `/** IN-14: the attempt whose 2xx response the decode task excerpts (0 = none) */ int32 ExcerptOnDecodeAttempt = 0;`. `Source/VaRestHttp/Public/VaRestResultHandler.h` (`#include "VaRestRedactor.h"`), `FFinish`:

```cpp
		/** IN-14: the decode task excerpts the body before parsing it -- the attempt (0 = no), the redactor and the cap */
		int32 ExcerptAttempt = 0;
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> ExcerptRedactor;
		int32 ExcerptCap = 0;
```

`Source/VaRestHttp/Private/VaRestTracePoints.h`: `void AttemptEnded(FExchangeState& Ex, const FVaRestTransportResult& Result);` (non-const: it records `ExcerptOnDecodeAttempt`). In `Source/VaRestHttp/Private/VaRestTracePoints.cpp` (`#include "VaRestTraceExcerpts.h"`):

- `Started`: replace the last line (`FVaRestTrace::Emit(EVaRestTraceKind::Started, ...);`) with

```cpp
	// IN-14: the copy is made now, the redaction launched after Started so that the excerpt never comes first
	Excerpts::FJob Job = Excerpts::PrepareRequest(Ex);
	Payload.bExcerptPending = Job.IsSet();
	FVaRestTrace::Emit(EVaRestTraceKind::Started, Ex.StartedAt, FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Payload)));
	Job.Launch();
```

- `AttemptEnded` (signature `FExchangeState& Ex`): replace its last line with

```cpp
	Excerpts::FJob Job;
	if (Excerpts::WillExcerptOnDecode(Ex, Result))
	{
		Ex.ExcerptOnDecodeAttempt = Ex.Attempt;
		Payload.bExcerptPending = true;
	}
	else
	{
		Job = Excerpts::PrepareResponse(Ex, Result);
		Payload.bExcerptPending = Job.IsSet();
	}
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptEnded, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(Payload)));
	Job.Launch();
```

`Source/VaRestHttp/Private/VaRestPipeline.cpp` (`#include "VaRestTraceExcerpts.h"`): in `OnChainDone`, right before `Finish.bTimeDecode = ...`:

```cpp
	if (Ex.ExcerptOnDecodeAttempt > 0 && Ex.ExcerptOnDecodeAttempt == Ex.Attempt && !Finish.Error.IsError())
	{
		// IN-14: this response goes to the decoder; the task excerpts it from the body itself
		Finish.ExcerptAttempt = Ex.Attempt;
		Finish.ExcerptRedactor = GetRedactor();
		Finish.ExcerptCap = FVaRestTrace::GetDetailSettings().BodyCap;
	}
```

and `RunDecode` begins (after its counters block) with `Excerpts::EmitOnDecode(Finish);`.

- [ ] **Step 5: Run them, then the trace and decode suites**

Run: `Tools/run_tests.sh VaRest.Http.Trace` then `Tools/run_tests.sh --no-build VaRest.Http.Decode`
Expected: `0 failed` both.

- [ ] **Step 6: Restore-the-bug** — (a) call `Excerpts::EmitOnDecode(Finish)` after `Handler.Decode(Finish)` in `RunDecode` instead of first: `VaRest.Http.Trace.Excerpts` FAILS (the response excerpt is empty or garbage: the in-situ parse rewrote the buffer, and a JSON result's body was moved into the document); restore; PASS. (b) Make `Admit` always succeed: `ExcerptBacklog` FAILS; restore; PASS. (c) Emit Started after `Job.Launch()`: run `VaRest.Http.Trace.Excerpts` ten times -- order is a race, so report what was seen; the review checks the comment's reasoning. Restore.

- [ ] **Step 7: Full suite, commit**

Run: `Tools/run_tests.sh VaRest`
Expected: `0 failed`.

```bash
git add Source/VaRestHttp/Private/VaRestTraceExcerpts.h Source/VaRestHttp/Private/VaRestTraceExcerpts.cpp Source/VaRestHttp/Private/VaRestTracePoints.h \
  Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Private/VaRestExchangeState.h Source/VaRestHttp/Public/VaRestResultHandler.h \
  Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/Tests/VaRestHttpExcerptTest.cpp
git commit -m "http: M4 task 17 -- trace excerpts: request prefix at Started, decoded responses on the decode task, the rest via the pipe, admission cap (IN-14)

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 18: The IN-17 goldens that M4a owns

Inspector IN-17: "Core-owned goldens `VaRest.Http.Trace.*`, which assert event sequences **and phase values**: success; retry with `Retry-After`; 401 → refresh → replay with two parked requests (`GapBefore`, `AuthWait`, `AuthRefreshId`); proactive refresh; cache hit; 304; offline queued + replay + expiry; cancel; decode error with warnings; envelope failure; attach mid-flight." M3 has success (`VaRest.Http.Trace.Success`). Cache hit, 304 and the offline queue are M4b's. The socket and Socket.IO halves of attach mid-flight are W5's and S4's. Each golden pins the order and the virtual-clock times, so a change in either is a deliberate one.

**Files:**
- Create: `Source/VaRestHttp/Private/Tests/VaRestHttpTraceGoldenTest.cpp`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp` (the two auth goldens, beside `FHandAuth`), `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp` (decode warnings: they need a USTRUCT)

**Interfaces:**
- Consumes: Tasks 4, 7, 10, 13, 17. Produces nothing; a failing golden names the task whose trace point moved.

- [ ] **Step 1: Write the goldens** — create `Source/VaRestHttp/Private/Tests/VaRestHttpTraceGoldenTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::GoldenTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FVaRestRetryPolicy Exact()
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(3);
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}

	/** Every event of Kind for request Id, with its time */
	template <typename PayloadType>
	TArray<TPair<double, PayloadType>> All(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id, EVaRestTraceKind Kind)
	{
		TArray<TPair<double, PayloadType>> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == Kind)
			{
				Out.Emplace(Event.Time, Event.Payload.Get<PayloadType>());
			}
		}
		return Out;
	}

	bool SeqIncreases(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		uint64 Last = 0;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Seq <= Last)
			{
				return false;
			}
			Last = Event.Seq;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenRetryAfterTest, "VaRest.Http.Trace.Golden.RetryAfter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenRetryAfterTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	FVaRestFakeResponse Busy = Json(503);
	Busy.Headers.Set(TEXT("Retry-After"), TEXT("2"));
	Rig.Fake->Enqueue(Busy);
	Rig.Fake->Enqueue(Json(200));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/busy")), Outcome).Send().GetId();
	if (!TestTrue(TEXT("backoff armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([Outcome] { return Outcome->IsDone(); });
		return false;
	}
	Rig.Clock->Advance(2.0);
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		return false;
	}
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("the sequence"), Recorder.KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Note,Dispatched,Ended,Completed")));
	TestTrue(TEXT("Seq increases"), SeqIncreases(Recorder, Id));
	const auto Dispatched = All<FVaRestTraceAttemptDispatched>(Recorder, Id, EVaRestTraceKind::AttemptDispatched);
	const auto Ended = All<FVaRestTraceAttemptEnded>(Recorder, Id, EVaRestTraceKind::AttemptEnded);
	const auto Notes = All<FVaRestTraceNote>(Recorder, Id, EVaRestTraceKind::Note);
	const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
	if (!TestTrue(TEXT("the counts"), Dispatched.Num() == 2 && Ended.Num() == 2 && Notes.Num() == 1 && Completed.Num() == 1))
	{
		return false;
	}
	TestTrue(TEXT("attempt 1 at 0 s, no gap"), Dispatched[0].Value.Attempt == 1 && Dispatched[0].Value.AttemptStart == 0.0 && Dispatched[0].Value.GapKind == EVaRestTraceGap::None);
	TestTrue(TEXT("... ends 503"), Ended[0].Value.Attempt == 1 && Ended[0].Value.Status == 503);
	TestTrue(TEXT("the note at 0 s: RetryScheduled, Status503, 2 s, Retry-After 2"), Notes[0].Key == 0.0 && Notes[0].Value.Kind == EVaRestTraceNoteKind::RetryScheduled
		&& Notes[0].Value.Reason == FName(TEXT("Status503")) && Notes[0].Value.Delay == 2.0 && Notes[0].Value.RetryAfter == 2.0);
	TestTrue(TEXT("attempt 2 at 2 s after a 2 s backoff"), Dispatched[1].Value.Attempt == 2 && Dispatched[1].Value.AttemptStart == 2.0
		&& Dispatched[1].Value.GapKind == EVaRestTraceGap::Backoff && Dispatched[1].Value.GapSeconds == 2.0);
	TestTrue(TEXT("... ends 200"), Ended[1].Value.Status == 200);
	TestTrue(TEXT("Completed at 2 s: a success"), Completed[0].Key == 2.0 && Completed[0].Value.Outcome == EVaRestTraceOutcome::Success);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenCancelTest, "VaRest.Http.Trace.Golden.Cancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/wait")) ? FVaRestFakeResponse::Manual() : Json(503); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto InTransport = MakeOutcome<FVaRestJson>();
	auto BackingOff = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle A = Observe(Rig.Client.Get(TEXT("/wait")), InTransport).Send();
	const FVaRestRequestHandle B = Observe(Rig.Client.Get(TEXT("/busy")), BackingOff).Send();
	if (!TestTrue(TEXT("one in the transport, one backing off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	A.Cancel();
	B.Cancel();
	if (!TestTrue(TEXT("both complete"), PumpUntil([InTransport, BackingOff] { return InTransport->IsDone() && BackingOff->IsDone(); })))
	{
		return false;
	}
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("canceled in the transport"), Recorder.KindsOf(A.GetId()), FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestEqualSensitive(TEXT("canceled while backing off: the note, then Completed"), Recorder.KindsOf(B.GetId()), FString(TEXT("Started,Dispatched,Ended,Note,Completed")));
	const auto EndedA = All<FVaRestTraceAttemptEnded>(Recorder, A.GetId(), EVaRestTraceKind::AttemptEnded);
	TestTrue(TEXT("IN-3: the transport's attempt ended Canceled"), EndedA.Num() == 1 && EndedA[0].Value.Failure == EVaRestTransportFailure::Canceled);
	for (const uint64 Id : { A.GetId(), B.GetId() })
	{
		const auto Completed = All<FVaRestTraceCompleted>(Recorder, Id, EVaRestTraceKind::Completed);
		TestTrue(FString::Printf(TEXT("#%llu: Completed Canceled, reason User"), Id), Completed.Num() == 1 && Completed[0].Value.Outcome == EVaRestTraceOutcome::Canceled
			&& Completed[0].Value.CancelReason == EVaRestCancelReason::User);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenEnvelopeTest, "VaRest.Http.Trace.Golden.EnvelopeFailure",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenEnvelopeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	FTestRig Rig;
	Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message")));
	Rig.Fake->Enqueue(Json(200, R"({"ok":false,"code":"E42","message":"the envelope says no"})"));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/e")), Outcome).Send().GetId();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		return false;
	}
	TestEqualSensitive(TEXT("the sequence"), Scoped.Recorder->KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	const auto Completed = All<FVaRestTraceCompleted>(*Scoped.Recorder, Id, EVaRestTraceKind::Completed);
	if (!TestEqual(TEXT("one Completed"), Completed.Num(), 1))
	{
		return false;
	}
	const FVaRestTraceCompleted& Done = Completed[0].Value;
	TestTrue(TEXT("an error, Kind Server, the envelope's code"), Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Server
		&& Done.Error.HttpStatus == 200 && Done.Error.Code == TEXT("E42"));
	TestTrue(TEXT("IN-5 Envelope: the failed predicate"), Done.Envelope.bApplied && Done.Envelope.DataPath == TEXT("data") && Done.Envelope.PredicatePath == TEXT("ok")
		&& Done.Envelope.Expected == TEXT("true") && Done.Envelope.Actual == TEXT("false"));
	TestTrue(TEXT("decode timed (the mask was on)"), Done.DecodeStartAt >= 0.0 && Done.DecodeEndAt >= Done.DecodeStartAt);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenAttachTest, "VaRest.Http.Trace.Golden.AttachMidFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenAttachTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::GoldenTestPrivate;
	// IN-12 + IN-17: a sink that attaches while one request is in the transport and one backs off gets one coherent
	// record for each: the snapshot's Started{Partial}, then the rest
	FTestRig Rig;
	Rig.Client.WithRetry(Exact());
	TSharedRef<int32, ESPMode::ThreadSafe> BusyCalls = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Fake->SetResponder([BusyCalls](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/wait")))
		{
			return FVaRestFakeResponse::Manual();
		}
		return ++*BusyCalls == 1 ? Json(503) : Json(200);
	});
	auto InTransport = MakeOutcome<FVaRestJson>();
	auto BackingOff = MakeOutcome<FVaRestJson>();
	const uint64 A = Observe(Rig.Client.Get(TEXT("/wait")), InTransport).Send().GetId();
	const uint64 B = Observe(Rig.Client.Get(TEXT("/busy")), BackingOff).Send().GetId();
	if (!TestTrue(TEXT("one in the transport, one backing off"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2 && Rig.Clock->NumPendingTimers() == 1; })))
	{
		Rig.Client.CancelAll();
		PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
		return false;
	}
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	const TArray<FVaRestTraceSnapshot> Snapshots = Scoped.Recorder->GetSnapshots();
	if (!TestEqual(TEXT("one snapshot"), Snapshots.Num(), 1))
	{
		return false;
	}
	const auto Partial = [&Snapshots](uint64 Id) { return Snapshots[0].InFlight.FindByPredicate([Id](const FVaRestTraceStarted& Entry) { return Entry.RequestId == Id; }); };
	const FVaRestTraceStarted* PartialA = Partial(A);
	const FVaRestTraceStarted* PartialB = Partial(B);
	TestTrue(TEXT("both in the snapshot as Partial, at attempt 1"), PartialA && PartialB && EnumHasAnyFlags(PartialA->Flags, EVaRestTraceRequestFlags::Partial)
		&& PartialA->CurrentAttempt == 1 && PartialB->CurrentAttempt == 1);
	Rig.Fake->Complete(0, Json(200));
	Rig.Clock->Advance(0.5);
	if (!TestTrue(TEXT("both complete"), PumpUntil([InTransport, BackingOff] { return InTransport->IsDone() && BackingOff->IsDone(); })))
	{
		return false;
	}
	TestEqualSensitive(TEXT("A: the rest of its record"), Scoped.Recorder->KindsOf(A), FString(TEXT("Ended,Completed")));
	TestEqualSensitive(TEXT("B: its retry and the end"), Scoped.Recorder->KindsOf(B), FString(TEXT("Dispatched,Ended,Completed")));
	const auto Dispatched = All<FVaRestTraceAttemptDispatched>(*Scoped.Recorder, B, EVaRestTraceKind::AttemptDispatched);
	TestTrue(TEXT("... attempt 2 after a 0.5 s backoff"), Dispatched.Num() == 1 && Dispatched[0].Value.Attempt == 2 && Dispatched[0].Value.GapKind == EVaRestTraceGap::Backoff
		&& Dispatched[0].Value.GapSeconds == 0.5);
	return true;
}

#endif
```

Append to `Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp` (it has `FHandAuth`):

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenRefreshTest, "VaRest.Http.Trace.Golden.RefreshParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenRefreshTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// IN-17: 401 -> refresh -> replay with two parked requests (GapBefore, AuthWait, AuthRefreshId)
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto First = MakeOutcome<FVaRestJson>();
	const uint64 A = Observe(Rig.Client.Get(TEXT("/a")), First).Send().GetId();
	if (!TestTrue(TEXT("A parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	auto Second = MakeOutcome<FVaRestJson>();
	auto Third = MakeOutcome<FVaRestJson>();
	const uint64 B = Observe(Rig.Client.Get(TEXT("/b")), Second).Send().GetId();
	const uint64 C = Observe(Rig.Client.Get(TEXT("/c")), Third).Send().GetId();
	Rig.Drain();
	Rig.Clock->Advance(0.4);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("all complete"), PumpUntil([First, Second, Third] { return First->IsDone() && Second->IsDone() && Third->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	const FTraceRecorder& Recorder = *Scoped.Recorder;
	TestEqualSensitive(TEXT("A: 401, then the replay"), Recorder.KindsOf(A), FString(TEXT("Started,Dispatched,Ended,Dispatched,Ended,Completed")));
	TestEqualSensitive(TEXT("B: one attempt"), Recorder.KindsOf(B), FString(TEXT("Started,Dispatched,Ended,Completed")));
	TestEqualSensitive(TEXT("C: one attempt"), Recorder.KindsOf(C), FString(TEXT("Started,Dispatched,Ended,Completed")));
	const TArray<FVaRestTraceAttemptDispatched> ForA = DispatchesOf(Recorder, A);
	if (!TestEqual(TEXT("A dispatched twice"), ForA.Num(), 2))
	{
		return false;
	}
	TestTrue(TEXT("A's replay: attempt 2 at 0.4 s, GapBefore SessionRefresh 0.4 s, AuthWait 0.4 s, the refresh's id"), ForA[1].Attempt == 2 && ForA[1].AttemptStart == 0.4
		&& ForA[1].GapKind == EVaRestTraceGap::SessionRefresh && FMath::IsNearlyEqual(ForA[1].GapSeconds, 0.4) && FMath::IsNearlyEqual(ForA[1].AuthWait, 0.4)
		&& ForA[1].AuthRefreshId == Auth->LastRefreshId);
	for (const uint64 Id : { B, C })
	{
		const TArray<FVaRestTraceAttemptDispatched> Parked = DispatchesOf(Recorder, Id);
		TestTrue(FString::Printf(TEXT("#%llu: attempt 1 at 0.4 s, no gap, AuthWait 0.4 s, the same refresh"), Id), Parked.Num() == 1 && Parked[0].Attempt == 1
			&& Parked[0].AttemptStart == 0.4 && Parked[0].GapKind == EVaRestTraceGap::None && FMath::IsNearlyEqual(Parked[0].AuthWait, 0.4)
			&& Parked[0].AuthRefreshId == Auth->LastRefreshId);
	}
	const TArray<FVaRestTraceMarker> Markers = Scoped.Recorder->MarkersOf(Rig.Client.GetId());
	TestTrue(TEXT("IN-9: SessionRefreshed caused by A"), Markers.ContainsByPredicate([A](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Marker.CauseRequestId == A;
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenProactiveTest, "VaRest.Http.Trace.Golden.ProactiveRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenProactiveTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 10.0);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const uint64 Id = Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send().GetId();
	Rig.Drain();
	Rig.Clock->Advance(0.3);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), FString(), 3600.0));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestEqualSensitive(TEXT("one attempt, after the refresh"), Scoped.Recorder->KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Id);
	TestTrue(TEXT("attempt 1 at 0.3 s, AuthWait 0.3 s, no gap"), Dispatched.Num() == 1 && Dispatched[0].AttemptStart == 0.3
		&& FMath::IsNearlyEqual(Dispatched[0].AuthWait, 0.3) && Dispatched[0].GapKind == EVaRestTraceGap::None && Dispatched[0].AuthRefreshId == Auth->LastRefreshId);
	TestTrue(TEXT("IN-9: SessionRefreshed caused by the request"), Scoped.Recorder->MarkersOf(Rig.Client.GetId()).ContainsByPredicate([Id](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Marker.CauseRequestId == Id;
	}));
	return true;
}
```

Append to `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpGoldenDecodeTest, "VaRest.Http.Trace.Golden.DecodeWarnings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpGoldenDecodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	// IN-17: a decode with warnings (tolerant: a success) and the same body strict (a Decode error)
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, Profile));
	Rig.Fake->Enqueue(Json(200, Profile));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Tolerant = MakeOutcome<FVaRestHttpTestProfile>();
	auto Strict = MakeOutcome<FVaRestHttpTestProfile>();
	const uint64 A = Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/t")), Tolerant).Send().GetId();
	const uint64 B = Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/s")).StrictDecode(), Strict).Send().GetId();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Tolerant, Strict] { return Tolerant->IsDone() && Strict->IsDone(); })))
	{
		return false;
	}
	for (const uint64 Id : { A, B })
	{
		TestEqualSensitive(FString::Printf(TEXT("#%llu: the sequence"), Id), Scoped.Recorder->KindsOf(Id), FString(TEXT("Started,Dispatched,Ended,Completed")));
	}
	TOptional<FVaRestTraceCompleted> First;
	TOptional<FVaRestTraceCompleted> Second;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(A))
	{
		if (Event.Kind == EVaRestTraceKind::Completed)
		{
			First = Event.Payload.Get<FVaRestTraceCompleted>();
		}
	}
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(B))
	{
		if (Event.Kind == EVaRestTraceKind::Completed)
		{
			Second = Event.Payload.Get<FVaRestTraceCompleted>();
		}
	}
	if (!TestTrue(TEXT("both Completed"), First.IsSet() && Second.IsSet()))
	{
		return false;
	}
	TestTrue(TEXT("tolerant: a success with one warning at its path, the result type, decode times"), First->Outcome == EVaRestTraceOutcome::Success
		&& First->DecodeWarningCount == 1 && First->DecodeWarnings.Num() == 1 && First->DecodeWarnings[0].Path == TEXT("items[1].level")
		&& First->ResultType == FName(TEXT("VaRestHttpTestProfile")) && First->DecodeStartAt >= 0.0 && First->DecodeEndAt >= First->DecodeStartAt);
	TestTrue(TEXT("strict: a Decode error"), Second->Outcome == EVaRestTraceOutcome::Error && Second->Error.Kind == EVaRestErrorKind::Decode);
	return true;
}
```

- [ ] **Step 2: Run them**

Run: `Tools/run_tests.sh VaRest.Http.Trace.Golden`
Expected: `0 failed`. A golden that fails shows a trace point in the wrong place or with the wrong value: fix the task that owns it (named in the test's comment), never the golden, unless the spec says the golden is wrong -- then stop and report.

- [ ] **Step 3: Restore-the-bug** — reset `Ex.NextGap` in the retry stage right after setting it (so the gap never reaches the dispatch): `Golden.RetryAfter` and `Golden.AttachMidFlight` FAIL; restore; PASS. In `AuthGate::Park` stop setting `Back.AuthWait`: `Golden.RefreshParked` and `Golden.ProactiveRefresh` FAIL; restore; PASS.

- [ ] **Step 4: Commit**

```bash
git add Source/VaRestHttp/Private/Tests/VaRestHttpTraceGoldenTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpAuthTest.cpp \
  Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp
git commit -m "http tests: M4 task 18 -- IN-17 goldens: Retry-After, refresh with parked requests, proactive refresh, cancel, envelope, decode warnings, attach mid-flight

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 19: The test server and the integration suite for M4a (spec §7.1)

`Tools/testserver/server.py` already has M4's routes (`/retry-after`, `/flaky`, `/login`, `/refresh`, `/protected`, `/envelope/*`; M3 Task 16). It needs two more: one to expire the current access token while keeping the refresh token (a 401 the refresh can fix), and an envelope error with a 4xx status.

**Files:**
- Modify: `Tools/testserver/server.py`, `Tools/testserver/selftest.py`
- Test: `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`

**Interfaces:**
- Consumes: everything above.
- Produces: routes `POST /expire` (forgets the current access token; the refresh token stays valid) and `GET /envelope/gone` (404 `{"ok": false, "code": "E404", "message": "not here"}`).

- [ ] **Step 1: Write the failing selftests** — in `Tools/testserver/selftest.py`, after `test_envelope`:

```python
    def test_expire_keeps_the_refresh_token(self):
        _, _, tokens = self.call_json("POST", "/login", b'{"user":"neo","password":"trinity"}')
        bearer = {"Authorization": "Bearer " + tokens["accessToken"]}
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 200)
        self.assertEqual(self.call("POST", "/expire")[0], 200)
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 401)
        status, _, renewed = self.call_json("POST", "/refresh", json.dumps({"refreshToken": tokens["refreshToken"]}).encode())
        self.assertEqual(status, 200)
        self.assertEqual(self.call("GET", "/protected", headers={"Authorization": "Bearer " + renewed["accessToken"]})[0], 200)

    def test_envelope_gone(self):
        status, _, body = self.call_json("GET", "/envelope/gone")
        self.assertEqual((status, body["code"], body["message"]), (404, "E404", "not here"))
```

- [ ] **Step 2: Run them to see them fail**

Run: `python3 -I Tools/testserver/selftest.py`
Expected: FAIL (404 for `/expire` and `/envelope/gone`).

- [ ] **Step 3: The routes** — in `Tools/testserver/server.py`, after `route_protected`:

```python
def route_expire(h, match):
    """M4: the current access token stops working; the refresh token stays valid (a 401 a session refresh can fix)"""
    with LOCK:
        STATE["tokens"]["access"] = None
    h.send_json(200, {})
```

after `route_envelope_err`:

```python
def route_envelope_gone(h, match):
    h.send_json(404, {"ok": False, "code": "E404", "message": "not here"})
```

in `ROUTES`, after the `/protected` row: `(("POST",), r"/expire", route_expire),`; after the `/envelope/err` row: `(("GET",), r"/envelope/gone", route_envelope_gone),`; and in the module docstring's M4 list:

```
  POST /expire              the current access token stops working; the refresh token stays valid
  GET  /envelope/gone       404 {"ok": false, "code": "E404", "message": "not here"}
```

- [ ] **Step 4: Run the selftests**

Run: `python3 -I Tools/testserver/selftest.py`
Expected: `OK`.

- [ ] **Step 5: Write the integration tests** — append to `Source/VaRestTests/Private/VaRestIntegrationTest.cpp` (before `#endif`; add `#include "VaRestAuth.h"`, `#include "VaRestEnvelope.h"`, `#include "VaRestRetry.h"`):

```cpp
namespace VaRestTestsIntegrationPrivate
{
	/** Short real-clock delays, no jitter */
	FVaRestRetryPolicy QuickRetry(int32 MaxRetries)
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(MaxRetries);
		Policy.Backoff.BaseDelay = 0.1f;
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationRetryTest, "VaRest.Integration.Http.Retry",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationRetryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithRetry(QuickRetry(3));
	const FString Key = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto After = MakeOutcome<FVaRestJson>();
	auto Flaky = MakeOutcome<FVaRestJson>();
	const double Start = FPlatformTime::Seconds();
	Observe(Client.Get(TEXT("/retry-after/2")).Query(TEXT("key"), Key).Query(TEXT("seconds"), TEXT("1")), After).Send();
	// Plan decision 15: curl reports a connection closed without a response as EHttpFailureReason::Other; it is retried
	Observe(Client.Get(TEXT("/flaky")).Query(TEXT("n"), TEXT("2")).Query(TEXT("key"), Key), Flaky).Send();
	if (!TestTrue(TEXT("both complete"), WaitAll(After, Flaky)))
	{
		return false;
	}
	if (Succeeded(*this, TEXT("Retry-After"), After))
	{
		TestEqual(TEXT("... on the third attempt"), After->Value.GetValue()[TEXT("attempt")].AsInt(), 3);
		TestTrue(TEXT("... after two Retry-After: 1 waits"), FPlatformTime::Seconds() - Start >= 2.0);
	}
	if (Succeeded(*this, TEXT("the flaky route"), Flaky))
	{
		// libcurl may resend on its own when a reused keep-alive connection turns out dead before any byte came back (not
		// verified against the bundled libcurl here), so one of the two refusals may never reach VaRest: two or three attempts
		TestTrue(TEXT("... after a VaRest retry"), Flaky->Response.Attempts >= 2 && Flaky->Response.Attempts <= 3);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationAuthTest, "VaRest.Integration.Http.AuthRefresh",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationAuthTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestBearerRefreshConfig Config;
	Config.RefreshPath = TEXT("/refresh");
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithAuth(FVaRestAuth::BearerRefresh(Config));
	auto Login = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/login")).NoAuth().Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("trinity"))), Login).Send();
	if (!TestTrue(TEXT("login completes"), Wait(Login)) || !Succeeded(*this, TEXT("the login"), Login))
	{
		return false;
	}
	const FVaRestJson Tokens = Login->Value.GetValue();
	Client.SetSession(Tokens[TEXT("accessToken")].AsString(), Tokens[TEXT("refreshToken")].AsString(), Tokens[TEXT("expiresIn")].AsDouble());
	auto Expire = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/expire")).NoAuth(), Expire).Send();
	if (!TestTrue(TEXT("expire completes"), Wait(Expire)) || !Succeeded(*this, TEXT("the expiry"), Expire))
	{
		return false;
	}
	auto Protected = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/protected")), Protected).Send();
	if (!TestTrue(TEXT("completes"), Wait(Protected)))
	{
		return false;
	}
	if (Succeeded(*this, TEXT("401, refresh, replay"), Protected))
	{
		TestEqual(TEXT("... in two attempts"), Protected->Response.Attempts, 2);
		TestFalse(TEXT("... with a new access token in the session"), Client.GetSession().AccessToken == Tokens[TEXT("accessToken")].AsString());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationEnvelopeTest, "VaRest.Integration.Http.Envelope",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationEnvelopeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"))
		.ParseErrorBodies());
	auto Ok = MakeOutcome<FVaRestJson>();
	auto Err = MakeOutcome<FVaRestJson>();
	auto Gone = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/envelope/ok")), Ok).Send();
	Observe(Client.Get(TEXT("/envelope/err")), Err).Send();
	Observe(Client.Get(TEXT("/envelope/gone")), Gone).Send();
	if (!TestTrue(TEXT("all complete"), WaitAll(Ok, Err, Gone)))
	{
		return false;
	}
	if (Succeeded(*this, TEXT("the ok envelope"), Ok))
	{
		TestEqual(TEXT("... unwrapped"), Ok->Value.GetValue()[TEXT("id")].AsInt(), 7);
	}
	TestTrue(TEXT("a failed predicate: Kind Server with the envelope's code and message"), Err->Error.Kind == EVaRestErrorKind::Server
		&& Err->Error.Code == TEXT("E42") && Err->Error.Message == TEXT("the envelope says no"));
	TestTrue(TEXT("a 404 envelope: Kind Http, its code read (bParseErrorBodies)"), Gone->Error.Kind == EVaRestErrorKind::Http && Gone->Error.HttpStatus == 404
		&& Gone->Error.Code == TEXT("E404") && Gone->Error.Message == TEXT("not here"));
	return true;
}
```

- [ ] **Step 6: Run the integration suite**

Run: `Tools/run_tests.sh VaRest.Integration`
Expected: `0 failed` (the runner starts the server).

- [ ] **Step 7: Restore-the-bug** — in `ClassifyAttempt` make the engine's `Other` (detail `Other`) non-retryable: `VaRest.Integration.Http.Retry` FAILS on the flaky route (plan decision 15's premise on real curl); restore; PASS.

- [ ] **Step 8: Commit**

```bash
git add Tools/testserver/server.py Tools/testserver/selftest.py Source/VaRestTests/Private/VaRestIntegrationTest.cpp
git commit -m "testserver + integration: M4 task 19 -- /expire and /envelope/gone; retry, Retry-After, flaky, session refresh and envelope against the server

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---
### Task 20: Close M4a — docs, the freeze line, the milestone builds, the non-unity compile, the MSVC sweep, beads

**Files:**
- Modify: `CLAUDE.md` (module row, freeze line), `docs/dev/http-thread-completion.md` (Apple item 5)
- Temporarily modify, then revert: `Source/VaRestHttp/VaRestHttp.Build.cs`, `Source/VaRestTests/VaRestTests.Build.cs`

**Interfaces:**
- Consumes: Tasks 1-19.

- [ ] **Step 1: CLAUDE.md** — in the module table, the `VaRestHttp` row becomes:

```markdown
| `VaRestHttp` | Runtime | `FVaRestClient` and its `[[nodiscard]]` builder over a stage pipeline (retry, auth gate, attempt boundary, auth apply, log/redaction, limiter, transport; M4b adds the offline queue and the cache), HTTP-thread completion with zero-copy JSON decode off the game thread, the envelope, the session and its single-flight refresh, the public redactor, the Inspector trace surface with excerpts, `stat VaRest`, `UVaRestHttpSettings`; M5 adds the `UVaRestApi` asset, `UVaRestHub` and async BP actions |
```

and the freeze paragraph that names `FVaRestError`'s fields gains a sentence:

```markdown
  `EVaRestJitter`, `FVaRestBackoff`, `FVaRestRetryPolicy`, `FVaRestEnvelope`, `EVaRestAuthMode`, `EVaRestApiKeyPlacement`,
  `EVaRestRefreshStyle`, `EVaRestSessionChange` and `FVaRestBearerRefreshConfig` (M4a), with their `UPROPERTY` field names,
  freeze with M5 too: M5's asset stores them.
```

- [ ] **Step 2: The platform note** — in `docs/dev/http-thread-completion.md`, Mac/iOS item 5 ("Stream bodies cannot be resent"), append:

```markdown
   VaRest's own re-issues -- a retry (M4a Task 7) and a session replay after a refresh (Task 13) -- are new engine requests
   with a new stream (M4a plan decision 24), so they are not affected; only a resend NSURLSession attempts by itself is.
   Check `VaRest.Http.Auth.ReplayBodies` and `VaRest.Http.Retry.BodiesResent` on the device.
```

- [ ] **Step 3: The whole suite and the server's own tests**

Run: `Tools/run_tests.sh VaRest` then `python3 -I Tools/testserver/selftest.py`
Expected: `0 failed`; `OK`.

- [ ] **Step 4: The monolithic builds** (CLAUDE.md: "check it before a milestone lands"; spec §7.2)

Run:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Shipping -project=/w/Unreal/VaRestHost/VaRestHost.uproject
```

Expected: both succeed with no warning from `Source/VaRestHttp`. Shipping compiles out `VAREST_WITH_TRACE_EXCERPTS` (Task 17's `Wanted()` returns false), the engine stats (Task 16's `INC_DWORD_STAT` are empty) and the test helpers.

- [ ] **Step 5: The per-module non-unity, no-PCH compile** (the M3 ruling: never `-DisableUnity`, which rebuilds the engine) — in `Source/VaRestHttp/VaRestHttp.Build.cs` replace `PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;` with

```csharp
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
```

and the same in `Source/VaRestTests/VaRestTests.Build.cs`. Dry-run first, so a mistake cannot start an engine rebuild:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHostEditor Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject \
  -WriteOutdatedActions=/tmp/varest-m4a-actions.json
python3 -c "import json; a = json.load(open('/tmp/varest-m4a-actions.json')); print(len(a.get('Actions', a)))"
```

Expected: a few hundred actions at most, all under `Plugins/VaRest`. (More than a thousand, or any engine path: stop, revert the two files, report.) Then build for real (the same command without `-WriteOutdatedActions`), expect success, and **revert both Build.cs files** (`git checkout -- Source/VaRestHttp/VaRestHttp.Build.cs Source/VaRestTests/VaRestTests.Build.cs`), then rebuild once (`Tools/run_tests.sh --no-build` is not enough: run `Tools/run_tests.sh VaRest.Http.Module`) so the tree is back on unity builds.

- [ ] **Step 6: The MSVC sweep** (Global Constraints; only clang builds here). Run (Python: this workstation's `grep` is ugrep, whose `-E` has no back-references):

```bash
python3 - Source/VaRestHttp Source/VaRestTests <<'PY'
import pathlib, re, sys
capture = re.compile(r'\[(?:[^\]]*[ ,])?([A-Za-z_]\w*) = (?:MoveTemp\()?\1\)?[\],]')
member = re.compile(r'^\s+(?:const )?[\w:<>, ]+[ &*](Config|State|Url|Verb|Body|Tag|Priority|Owner|Attempt|InFlight|Limiter|Session|Auth|Envelope|Random|Retry|Interrupt) = ')
declared = re.compile(r'^\s+(?:const )?(?:auto|[A-Z][\w:]*(?:<[^;=()]*>)?)[ &*]+([A-Za-z_]\w*)(?: = |\(|\{| : )')
owners = {"VaRestClient.cpp", "VaRestClientState.cpp", "VaRestPipeline.cpp", "VaRestLogStage.cpp", "VaRestTracePoints.cpp", "VaRestRetry.cpp",
          "VaRestAuth.cpp", "VaRestAuthGate.cpp", "VaRestSession.cpp", "VaRestBearerRefresh.cpp", "VaRestTraceExcerpts.cpp", "VaRestEnvelope.cpp"}
for root in sys.argv[1:]:
    for path in sorted(pathlib.Path(root).rglob("*.[ch]*")):
        lines = path.read_text(encoding="utf-8").splitlines()
        scope_names = {}
        lambda_depth = 0
        for number, line in enumerate(lines, 1):
            if line.startswith("}"):
                scope_names, lambda_depth = {}, 0
            if capture.search(line):
                print("%s:%d: init-capture reuses a name: %s" % (path, number, line.strip()))
            if path.name in owners and member.search(line):
                print("%s:%d: check: a local named like a member? %s" % (path, number, line.strip()))
            match = declared.match(line)
            if match:
                name, indent = match.group(1), len(line) - len(line.lstrip("\t"))
                if lambda_depth > 0 and name in scope_names and scope_names[name] < indent:
                    print("%s:%d: check: a lambda local hides an enclosing local (C4456)? %s" % (path, number, line.strip()))
                scope_names.setdefault(name, indent)
            if re.search(r'\]\s*\([^)]*\)\s*(?:mutable\s*)?(?:->\s*[\w:<>]+\s*)?$|\]\s*(?:mutable\s*)?$', line.rstrip()) or re.search(r'\]\([^)]*\)\s*(?:mutable\s*)?\{\s*$', line):
                lambda_depth += 1
PY
```

Expected: no `init-capture reuses a name` line (`[Result = MoveTemp(Result)]`: C4456/C4458 are errors in this build, `CppCompileWarnings.cs:88-90`). Read every `check:` line: a local named like a member matters only inside a member function of that class (C4458: `FClientState`, `FVaRestClient`, `FVaRestExchange`, `TVaRestRequest`, `FVaRestEnvelope`, `FVaRestBackoff`, `FVaRestRetryPolicy`); a lambda local hiding an enclosing local is C4456. Then read, by hand, each new class's member functions against its member and method names (the script is an approximation): `TVaRestRequest` (`Retry`, `NoRetry`, `Envelope`, `NoEnvelope`, `AuthMode`, `NoAuth`, `MarkAuthRefresh`), `FVaRestClient` (Task 11-13's methods), `FVaRestEnvelope` (`DataAt`, `SuccessWhen`, `ErrorMessageAt`, `ErrorCodeAt`, `ParseErrorBodies` vs the fields), `FVaRestAuthRefreshContext`, `Excerpts::FJob::Launch` (its init-captures are renamed on purpose). Rename what is real and rebuild.

- [ ] **Step 7: Beads** — record what M4a leaves:

```bash
bd create --title="Inspector: binary excerpt hex capture (BinaryCap > 0)" --type=task --priority=3 \
  --description="M4a plan decision 20: FVaRestRedactor::RedactBody has no hex mode, so a binary body is always a descriptor even when a sink sets BinaryCap (Inspector 2.2 layer 4, MaxBinaryExcerptBytes, default 0). Add the opt-in hex capture to the redactor and the excerpt jobs (VaRestTraceExcerpts.cpp) before the Inspector exposes the setting."
bd update VaRest-ajz --notes "M4a carries: (1) plan decision 1 -- clients retry only with WithRetry; M5's UVaRestHttpSettings default retry policy decides the project default. (2) spec 4.2's UVaRestAuthConfig (instanced, EditInlineNew) and the BP Custom auth (native events Apply/Refresh, Finish Refresh) build on IVaRestAuth / FVaRestAuth (VaRestAuth.h); FVaRestBearerRefreshConfig is the BearerRefresh fields; RefreshPath takes a path in M4a, an endpoint name is M5's. (3) OpenAPI N15 AuthMode maps to EVaRestAuthMode (M4a)."
```

Report the new bead's id.

- [ ] **Step 8: Commit**

```bash
git add CLAUDE.md docs/dev/http-thread-completion.md
git commit -m "docs: M4a closes -- module row, freeze line for the M4a Blueprint types, the Apple replay note

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

## After M4a

What M4b and later milestones take from here, so their plans start from code that exists:

- **M4b offline queue and cache** go in `FClientState::Publish` between the custom `Outer` stages and `MakeRetryStage()` (spec §4 order: offline queue, cache, retry). A replayed queue item enters the normal pipeline minus the offline stage (spec §4.4) and gets auth re-applied for free (`AuthApply` reads the live session). The cache key's auth identity is SHA-256 of the applied `Authorization` (spec §4.3): read it from `FExchangeState::AttemptHeaders` after `AuthApply`, so the cache's *lookup* must run per attempt or compute the identity from `ApplyCredentials` up front -- M4b decides (outline item 3). An offline queue item fails "after its retries" (spec §4.4): it sits outside `Retry`, so it sees the final outcome. `FHttpStats` gains `CacheLookups`/`CacheHits` and the ratio; `FVaRestTraceNote` already has the cache and queue kinds; `EVaRestTraceMarkerKind` has `Online`/`Offline`; `FVaRestTraceStarted` has `CachePolicy`, `CacheLookup` and `QueueReplayOf`.
- **W2** (WebSocket): `FVaRestBackoff::GetDelay(Step, Stream)` with the socket's own `FRandomStream` and `EVaRestJitter::Ratio`; `IVaRestClock` timers as M3 built them.
- **W5**: `FVaRestClient::ApplyAuth`, `NeedsAuthRefresh`, `RefreshAuth`, `NotifySessionExpired`, `OnSessionChanged` (`Replaced` restarts sockets, `Cleared` closes them), the session headers; socket payload types join `FVaRestTracePayload` next to M4a's three.
- **M5**: the asset's retry, envelope and auth properties are M4a's USTRUCTs and UENUMs (plan decision 27); the BP Custom auth and `UVaRestAuthConfig` subclasses produce an `IVaRestAuth`; Blueprint events `OnSessionChanged`, `OnSessionExpired` wrap the client's delegates.
- **Inspector I1**: needs M4b too (cache and queue notes, the remaining goldens). Excerpt hex capture is a bead (Task 20).
