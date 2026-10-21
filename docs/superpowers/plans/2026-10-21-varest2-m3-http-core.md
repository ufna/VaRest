# VaRest 2 — M3: HTTP core Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build module `VaRestHttp`: `FVaRestClient` and its `[[nodiscard]]` request builder over a stage pipeline (logging/redaction, concurrency limiter, transport), every body and result kind of spec §3.2 with JSON decoded zero-copy off the game thread, typed errors, progress, cancellation, timeouts, file upload and download, the public redactor and the trace surface the Inspector needs from M3, plus a fake transport, a virtual clock, a Python test server with a runner lifecycle and the integration suite.

**Architecture:** A request is an `FVaRestExchange` that enters its client's stage chain on the game thread (sent from any thread, marshalled there first), crosses an attempt boundary that M4's retry will sit in front of, and reaches the transport through the log stage and the limiter. `FHttpModule` completes on the HTTP thread (`CompleteOnHttpThread`), and the completion only moves the body (received into our own buffer with one byte of slack) into a game-thread task. Decoding runs on `UE::Tasks`; callbacks and every trace event run on the game thread. Configuration is an immutable snapshot swapped on the game thread, so builders read it from any thread.

**Tech Stack:** UE 5.8.1 (`/w/Unreal/UnrealEngine`), C++20, engine HTTP module (curl on Win64/Linux/Android, NSURLSession on Mac/iOS), `UE::Tasks`, engine RapidJSON through `RapidJsonIncludes.h`, UE Automation tests, Python 3 standard library (test server).

**Spec:** `docs/superpowers/specs/2026-10-18-varest2-core-design.md` — §1 (modules, threading rule), §2.1 (`ParseOwned` slack), §3 (client, builder, results, errors), §4 (pipeline shape; §4.5 logging/redaction, §4.6 limiter, §4.7 timeouts), §5.5, §7.1–§7.2, §8 row M3 ("Http tests green"), §9 (HTTP-thread completion risk). Also binding for M3: `docs/superpowers/specs/2026-10-20-varest2-inspector-design.md` §2.2 and §9 (IN-1, IN-2, IN-3, IN-5, IN-7 scope 0, IN-8, IN-10, IN-11, IN-12, IN-13, IN-15, IN-16), `docs/superpowers/specs/2026-10-20-varest2-websocket-design.md` §19 item 3 and §16.3/§17 (W1 needs this milestone's test-server harness), `docs/superpowers/specs/2026-10-20-varest2-openapi-design.md` N14 (URL join) and the builder half of N9 (repeated query keys append), beads `VaRest-6zo` (slack: curl reserves exactly Content-Length, `CurlHttp.cpp:631-633`), `VaRest-sj2` (seeded key hash), `VaRest-rfd` (its two "by M3" code minors).

## Design decisions this plan makes (the specs are silent)

1. **`IVaRestClock` lands in M3, with timers.** WS §19 item 1 assigns the clock to M4, but M3 needs it now (progress throttling, the virtual clock for timeouts, trace times). M4 adds the backoff formula and the injectable `FRandomStream` on top; nothing it needs is missing.
2. **Explicit `Cancel()` delivers `Kind=Canceled` to `OnError`/`OnComplete`; an owner that is gone and engine shutdown deliver nothing.** Spec §3.2 only says "owner gone → no callbacks"; a caller that cancels usually needs to clear its UI.
3. **Callbacks never run inside `Send()` or `Cancel()`**: a result ready during the call (an invalid URL, a stage that answers synchronously) is delivered from a later game-thread task.
4. **`OnHttpThread()` means callbacks on a `UE::Tasks` worker** after the game-thread `Completed` event, because Inspector IN-5 requires `Completed` on the game thread before any callback.
5. **Responses are received through `SetResponseBodyReceiveStreamDelegateV2` into our own buffer** reserved at `Content-Length + 1`, so `ParseOwned` never reallocates (`IHttpRequest.h:273-284`; the engine's own buffer is reserved at exactly `Content-Length`, `CurlHttp.cpp:631-633`).
6. **A JSON result requires a JSON `Content-Type` (`application/json` or `*+json`); a response with no `Content-Type` is parsed anyway.** Spec §3.2 says non-JSON is a `Decode` error; a missing header is treated as unknown, not as non-JSON.
7. **`FVaRestError` gains `Path`** (the JSON path of a decode error). Spec §3.3's table has no field for it; the brief requires decode errors with paths.
8. **`OnProgress` takes `const FVaRestProgress&`** (`BytesSent`, `BytesReceived`, `UploadTotal`, `DownloadTotal`) instead of `(sent, received, total)`: one total is ambiguous when a request both uploads and downloads.
9. **`SaveTo()` compiles only for `T = void` or `T = FVaRestResponse`** (enforcement by shape): a body written to a file has nothing to decode.
10. **Path parameters: a missing, empty or unknown one is a `Config` error**, so `/users/{id}` never silently becomes `/users/`.
11. **Header names and values are validated (RFC 9110 token; no CR, LF or NUL)**; a bad one is a `Config` error at `Send`, never a transport call (header injection). The final attempt headers are checked again right before the transport, because a custom `PerAttempt` stage edits them after every builder check, and a multipart boundary must be 1-70 RFC 2046 characters (review I3).
12. **The body is moved into `SetContent(TArray<uint8>&&)` unless a stage declares `MayReissue()`**, which makes the transport copy it so a later attempt still has it (M4's retry declares it).
13. **Custom `Outer` stages run first, before the attempt boundary**, so they see every logical request once; M4 places its built-in Outer stages (offline queue, cache, retry) after them.
14. **The test server is owned by the runner**: `Tools/run_tests.sh` starts `Tools/testserver/server.py` on an OS-chosen port, passes `-VaRestTestServer=<url>`, and stops it on exit. An integration test run without that argument **fails** with instructions (a skip would be a false green).
15. **Test types live in a new UncookedOnly module `VaRestTests`**, so HTTP test USTRUCTs never ship in a game's reflection data (`VaRest-rfd`). VaRestJson's existing test types stay where they are in M3 (moving them means moving M1/M2 tests that include private headers); a bead tracks the move.
16. **`UVaRestHttpSettings` lands in M3 with the six fields M3 reads** (log level, redaction keys, max concurrent, total and activity timeout, max JSON response bytes), because IN-13 puts project-wide redaction additions there. M5 adds the rest of spec §5.5.
17. **`RedactPrefix` takes a `bWholeInput` flag** (default false). IN-13's rule "a scalar ending exactly at the cap is never written" is only right for a cut input; a whole body ending in a number must keep it. The rule is applied to numbers only: a string or a literal the reader delivers is complete by construction (its closing quote or last letter was read), so only a number can look complete and be cut. A genuine syntax error in a cut input's last 4 bytes is reported as `Cap` (the excerpt is the same). A leading UTF-8 BOM is skipped, as the parser does.
18. **The redactor's added keys match exactly (after normalisation)**, not as substrings; the built-in substring set stays as specified.
19. **Activity timeouts report `Kind=Connection`**, as the engine does (`HttpRequestCommon.cpp:303-305`; curl's `CURLE_OPERATION_TIMEDOUT` → `ConnectionError`, `CurlHttp.cpp:1320-1329`). Only the total timeout reports `Kind=Timeout`.
20. **JSON response bodies are capped at 32 MiB by default** (controller ruling from the M2 final review, recommendation 4). The arena DOM is about 4.2x its input, so the cap bounds a decode at about 135 MB. It is a setting (`MaxJsonResponseBytes`), a client option (`WithMaxJsonBytes`) and a request option (`MaxJsonBytes`), 0 meaning none, and it reaches `ParseOwned` as `FVaRestJsonParseOptions::MaxInputBytes`, which rejects before allocating. The same options parse JSON error bodies. The HTTP transport applies it while the body arrives (reviews M-6, M13): for a JSON result the body sink fails the transfer once more bytes came (`Kind=Decode`, detail `ResponseTooLarge`, not retryable; an error status stays `Kind=Http` without its body), and no sink reserves more than `max(cap, 8 MiB) + 1` up front, so a lying `Content-Length` costs nothing.
21. **Hash flooding** (controller ruling, recommendation 4b): objects with more than 8 members get an index over an unseeded `CityHash32`, so keys crafted to collide make every index build O(n²) on a server parsing client JSON. M3 is where untrusted network JSON arrives, and seeding is a three-line change, so Task 1 does it (bead VaRest-sj2) instead of waiting for M7.
22. **M3 decodes only into C++ result types** (`T` is a compile-time type, so a Blueprint `UUserDefinedStruct` cannot be one). A decode off the game thread into a Blueprint struct — M5's `FInstancedStruct` carrier for *Call Endpoint* — must keep the struct alive across garbage collection for the whole decode (`TStrongObjectPtr<const UScriptStruct>`, or an `FGCObject` that reports it) and decode into memory the task owns (the `FInstancedStruct` itself, moved back to the game thread), never into memory a Blueprint frame owns (controller ruling, recommendation 4c). `VaRest::Http::DecodeStruct` (Task 8) states that contract for any caller passing a non-native struct, and "After M3" carries it to M5.
23. **A client rename emits `Client{Created}` again for the same id.** IN-8 lists only `Created` and `Destroyed`, but `FVaRestClient::Make(Url).WithName(...)` is the normal order, so the first event names the base URL; a store treats a second `Created` for a known id as an update. The base URL is URL-redacted in the event (it may carry userinfo).
24. **`AttemptEnded` is not emitted after `Completed`.** A cancel can finish a request while the transport's answer is already queued for the game thread; Inspector §2.3 seals a completed record, so the late attempt is dropped from the trace (the log still has its line).
25. **Log lines are `#<id> <verb> <redacted url> -> <status> (<ms>, <down>, <up>)`**, with `>`/`<` lines for headers and bodies at the higher levels (Task 14). A status of 400 or more and a transport failure other than a cancel log as `Warning`; everything else as `Log`. Spec §4.5 fixes the content of each level, not the format or verbosity.
26. **A JSON body parsed in place keeps a 4 KiB diagnostic, not the bytes** (review I5). Spec §3.2's release rule says raw bytes are "kept on error for diagnostics", but `ParseOwned` parses in situ (zero copy, spec §2.1), so after a syntax error or a struct mismatch the bytes are gone. Before the parse the first 4 KiB are copied (one `memcpy` per JSON response); a syntax error keeps them as `RawBody`, a struct mismatch keeps the parsed DOM as `Error.Body`, and `KeepBody()` keeps everything (a copy before the parse). Cost if wrong: diagnostics show only 4 KiB of a broken body unless `KeepBody()`.
27. **The configuration snapshot is taken at `Send()`**, on the sending thread under the state's read lock (spec §3.1 verbatim; reviews I-7, I6, M11). `Enter` fills it only for an exchange made another way. A struct body was already encoded in `Body()` with the naming of that moment.
28. **`Owner()` with `OnHttpThread()` pins the owner for the delivery** (review C-5): the owner is checked on the game thread, then held by a `TStrongObjectPtr` until the worker's callbacks return, which 5.8.1 allows (the release is an atomic decrement with a lock-free root mark, `StrongObjectPtrTemplates.h:47-50`, `:122-131`; `UObjectArray.h:375-386`; `GarbageCollection.cpp:701-719`). Not a `Config` error: a widget may want a decoded payload off the game thread. Cost: the owner outlives its destruction by one callback.
29. **Upload progress goes through a counting archive, only when it is asked for** (review I-2). Under `CompleteOnHttpThread` curl reports progress only on receive and at the finish (`CurlHttp.cpp:453-467`, `:1274`). When `OnProgress` is bound or a trace sink wants metadata, the body goes through `FCountingUpload` and `SetContentFromStream`: curl reads it with its read callback on the HTTP thread, into its own buffer (the same one copy an in-memory body makes), and sends `TotalSize()` as `Content-Length`, not chunked (`CurlHttp.cpp:896-902`, `:952-987`; `HttpRequestPayload.cpp:53-97`). The default path stays `SetContent(TArray&&)`. Cost: one wrapper archive on progress-watched uploads.
30. **A total timeout without an activity timeout also sets the engine's activity timeout to the total** (review I-5). Otherwise the engine's own idle default (`[HTTP] HttpActivityTimeout`, 30 s, `HttpRequestCommon.cpp:733-736`) fires first, as a connection error, and the request's total timeout (spec §4.7) does not govern. Apple's 30 s connection timeout still bounds silence there (documented in `docs/dev/http-thread-completion.md`).
31. **`SaveTo` writes its temp file on the HTTP thread** (review M-11), as the engine's own `SetResponseBodyReceiveStream` to a file archive does. Spec §1's `UE::Tasks` list names cache and offline-queue IO, not download streaming; the rename runs on a worker. Cost if wrong: a slow disk stalls the shared HTTP thread; a bounded pipe can go behind the sink later without an API change.
32. **Deliveries are game-thread tasks** (review M-12): spec §1 says "on the game thread", which they are, but the game thread also runs its task queue between tick groups, so a callback can run mid-frame (v1 delivered from the core ticker). All deliveries go through one helper (`PipelinePrivate::OnGameThread`); M5's Blueprint async actions decide whether to defer to a frame boundary. Cost if wrong: one helper changes in M5.
33. **A base URL may carry a query, not a fragment** (review M22). The path is joined before the base's `?`, and the base query comes first, before the request's own pairs (`https://h/v1?key=k` + `/pets?limit=2` → `https://h/v1/pets?key=k&limit=2`). A base URL with a fragment logs an Error where it is set and makes each request a `Config` error. Cost if wrong: one more join case.
34. **`EVaRestQueryStyle {Repeat, Csv, Ssv, Pipes}` and an array `Query` overload land in M3** (review I7): OpenAPI N9's builder half is "repeated query keys append; the array styles", and OpenAPI codegen and M5 call it. A UENUM, frozen from M5 with the other pin types. Cost if wrong: one small public enum frozen early.

## Global Constraints

- Engine: UE 5.8 only. Build host: `/w/Unreal/VaRestHost` (plugin symlinked). Build + test: `Tools/run_tests.sh [Filter]` (bare filters are anchored, so `VaRest.Http` never runs `Bench.VaRest.*`). From Task 16 on the runner also starts the test server and passes `-VaRestTestServer=<url>`.
- Modules (`VaRest.uplugin`): `VaRestHttp`: `"Type": "Runtime"`, `"LoadingPhase": "PreDefault"`, `"PlatformAllowList": ["Win64","Mac","Linux","Android","IOS"]` (spec §1). `VaRestTests` (Task 10): `"Type": "UncookedOnly"`, `"LoadingPhase": "Default"`, `"PlatformAllowList": ["Win64","Mac","Linux"]`. `VaRestJson` keeps its entry.
- `VaRestHttp` dependencies: public `Core`, `CoreUObject`, `VaRestJson`, `DeveloperSettings`; private `HTTP`. Nothing else, and no new third-party code. RapidJSON only through `#include "RapidJsonIncludes.h"`.
- **v1 paths are frozen.** Nothing under `Source/VaRest/` or `Source/VaRestEditor/` changes in M3.
- **Threading rule (spec §1, verbatim):** "*Control plane on the game thread, data plane off it.* Stage decisions, callbacks, and BP events run on the game thread. Parsing, decoding, cache disk IO, and offline-queue persistence run on `UE::Tasks` background tasks. Transport completes on the HTTP thread (`EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread`); the completion only moves bytes and dispatches work. A request may be **sent** from any thread; it is marshalled to the game thread before entering the pipeline." Inspector §7 adds: "The game thread never parses or redacts a body", and §9: "**nothing is emitted from the HTTP thread**".
- Trace points: every one starts with `if (FVaRestTrace::Detail() == EVaRestTraceDetail::None) { return; }` before building anything (IN-11). Sinks receive only redacted data (Inspector §2.2: "A sink never receives a byte that was not redacted").
- Public types: `FVaRest*`, `EVaRest*`, `IVaRest*`, `TVaRest*`, `UVaRest*`. Internals: namespace `VaRest::Http`; file-local helpers in `namespace VaRest::Http::<FileStem>Private`. **Never reuse a v1 header name** (module include paths are shared): `VaRest.h`, `VaRestDefines.h`, `VaRestJsonObject.h`, `VaRestJsonValue.h`, `VaRestLibrary.h`, `VaRestRequestJSON.h`, `VaRestSettings.h`, `VaRestSubsystem.h`, `VaRestTypes.h`, `VaRestJsonParser.h`.
- **JSON keys are case-sensitive; HTTP header names are not.** JSON keys: byte compares, `ESearchCase::CaseSensitive`, `TestEqualSensitive`. Header names: always `ESearchCase::IgnoreCase`, explicitly.
- Unity builds: no file-scope `using namespace` and no anonymous namespaces in `.cpp` files. Block-scope `using namespace` inside a function body is fine.
- MSVC-clean by construction (only Linux clang builds here): no local or parameter named like a member — data **or function** — of the enclosing class (C4458). Builder methods of `TVaRestRequest` are named `Path`, `Query`, `Header`, `Body`, `BodyFile`, `Tag`, `Priority`, `Owner`, `Timeout`, `Naming`, `SaveTo`, `Idempotent`, `KeepBody`, `OnHttpThread`, `MaxJsonBytes`, `StrictDecode`, `OnSuccess`, `OnError`, `OnComplete`, `OnProgress`, `Send`, `Handler`: no parameter or local in that class may use those names (write `InTag`, `FilePath`, `InPriority`). In `FVaRestExchange`, `FClientState` and `FVaRestClient` member functions, no local is named `Config`, `State`, `Url`, `Verb`, `Body`, `Tag`, `Priority`, `Owner`, `Attempt`, `InFlight` or `Limiter`. No local hides a namespace-scope variable (clang `-Wshadow`).
- Lambda init-captures never reuse an enclosing name (`[Result = MoveTemp(Result)]`), and a local declared inside a lambda body never takes the name of a local of the function around it (`FBodySink& Sink` in a lambda of a function with a `Sink`, review I2): clang's `-Wshadow` lets both pass (the second needs `-Wshadow-uncaptured-local`), but MSVC's shadowing warnings (C4456/C4458, errors in this build: `CppCompileWarnings.cs:88-90`) are not checked here. Give each its own name (`[Moved = MoveTemp(Result)]`, `Received`). Task 17's sweep looks for both.
- `UE_LOG` format arguments are string literals (no `?:` between two `TEXT()`s). Every core log line about a request includes `#<RequestId>` (IN-16).
- Tests: `IMPLEMENT_SIMPLE_AUTOMATION_TEST`, inside `#if WITH_DEV_AUTOMATION_TESTS`, flags `EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter`. Unit tests: `Source/VaRestHttp/Private/Tests/`, names `VaRest.Http.*`; tests that need reflected test types and the integration suite: `Source/VaRestTests/Private/`, names `VaRest.Http.*` / `VaRest.Integration.*`; benchmarks `Bench.VaRest.*` with `EAutomationTestFlags::PerfFilter`. JSON tests stay `VaRest.Json.*` in `Source/VaRestJson/Private/Tests/`.
- **Request callbacks in tests never capture stack locals by reference**: a test that times out returns while a callback is still pending. Capture `TSharedRef` state (`TVaRestTestOutcome`, Task 8). Every test drains what it sends (pump until done, or cancel).
- Pumping: tests wait with `VaRest::Http::Testing::PumpUntil` (Task 6), which runs game-thread tasks from the test body. Automation calls tests from `FEngineLoop::Tick` outside any task (`LaunchEngineLoop.cpp:6036-6045`), so `ProcessThreadUntilIdle`'s recursion guard (`TaskGraph.cpp:700-708`) holds.
- Expected log lines in tests use **`AddExpectedErrorPlain` / `AddExpectedMessagePlain`** with exact occurrence counts (the non-`Plain` forms are regexes, `AutomationTest.h:1782`, `:1854`). Any unexpected `Error` log fails a test. Content checks on log lines use `VaRest::Http::Testing::FLogCapture` (Task 6).
- Comments explain *why* and cite engine source as `file:line`; open every line you cite.
- Restore-the-bug: a task that names a restore-the-bug step shows the named test failing with the behaviour deliberately broken, then passing restored; report both outcomes in the task report.
- No editor may run during a build; kill only by PID (never `pkill -f`).
- New file header: `// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.`
- Every commit message ends with `Co-Authored-By: Claude <Model> <noreply@anthropic.com>`, naming the model that actually wrote the commit (for example `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`). Stage only the files your task names (`git add <paths>`): other agents work in this tree.

## Review Focus

- **A request sent from a worker thread while the game thread edits the client** (edits interleaved with sends): no data race, each request carries one whole configuration (two headers edited together arrive together), and it is the configuration of its `Send()` (decision 27). Test owner: Task 8 (`VaRest.Http.Pipeline.ConfigEditedWhileSending`, restore-the-bug: split the edit).
- **A hundred responses completing at once on worker threads**: every request gets exactly one `OnComplete`, on the game thread. Test owner: Task 8 (`VaRest.Http.Pipeline.ManyConcurrent`).
- **Every `FVaRestClient` handle dropped while requests are in flight**: the requests still finish and call back; the client state dies afterwards. Test owner: Task 8 (`VaRest.Http.Pipeline.ClientDroppedInFlight`).
- **A path parameter holding `/`, `?`, `#`, a space or non-ASCII text, and a base URL with a trailing slash joined to a path without a leading one**: one percent-encoded segment, exactly one `/` at the join. Test owner: Task 3 (`VaRest.Http.Url.PathTemplate`, `VaRest.Http.Url.Join`).
- **A header value containing CR/LF** (from user input): `Kind=Config`, and the transport is never called. Test owner: Task 9 (`VaRest.Http.Builder.HeaderInjection`).
- **A `200` JSON response with an empty body (not `204`)**: `FVaRestJson` gives an invalid handle and a struct its defaults, both as success. Test owner: Task 10 (`VaRest.Http.Decode.EmptyBodies`).
- **`SaveTo` onto an existing file when the download fails**: the existing file is untouched and no `.part` file is left behind. Test owner: Task 10 (`VaRest.Http.Decode.SaveToKeepsOldFileOnError`).
- **A callback that sends a new request and cancels another one from inside itself**: no deadlock, no lost callback. Test owner: Task 11 (`VaRest.Http.Cancel.FromCallback`).
- **An owner destroyed while its request waits in the limiter queue**: never dispatched, no callbacks, and the slot is not leaked. Test owner: Task 12 (`VaRest.Http.Limiter.OwnerGoneWhileQueued`).
- **An `Owner()` request with `OnHttpThread()` whose owner is collected while its callback runs on a worker**: the owner lives until the callbacks return, then the pin is dropped (decision 28). Test owner: Task 10 (`VaRest.Http.Decode.OnHttpThreadPinsOwner`).
- **Secrets in places no key rule names**: a bearer token echoed in a URL path and a `text/plain` body, a `refresh_token` inside a 5xx JSON error body, an OAuth `code` in a `Location` header — none appear in any log line at `Body` level. Test owner: Task 14 (`VaRest.Http.Log.Canaries`).
- **A widget that cancels its request in `EndPlay` and is then destroyed**, or that shows progress with `.Owner(this).OnProgress([this]...)`: no callback of any kind runs into the dead owner. Test owners: Task 11 (`VaRest.Http.Cancel.CancelThenOwnerGone`), Task 13 (`VaRest.Http.Progress.NotAfterOwnerGone`, restore-the-bug in Step 7b).
- **A custom stage (M4's auth, retry) that holds an outcome while `Cancel()` runs, or reports it twice**: exactly one `OnError(Canceled)` and one `OnComplete`, no crash. Test owners: Task 11 (`VaRest.Http.Cancel.WhileOutcomeHeld`), Task 8 (`VaRest.Http.Pipeline.OutcomeTwice`).
- **The engine never calls back** (HTTP module shutdown, `[HTTP] bUseNullHttp`): the request still finishes once and its limiter slot comes back. Test owners: Task 8 (`VaRest.Http.Pipeline.DroppedCompletion`), Task 12 (`VaRest.Http.Limiter.DroppedCompletionFreesSlot`).
- **A server that lies in `Content-Length` or sends a JSON body far over `MaxJsonBytes`**: no gigabyte reserve, the transfer stops at the cap, `Kind=Decode`, not retryable. Test owners: Task 7 (`VaRest.Http.Transport.MemorySinkSlack`, restore-the-bug in Step 8), Task 16 (`VaRest.Integration.Http.ChunkedRedirectCap`).
- **The last reference to a client dropped while a worker or the HTTP thread holds a request**: the client state, its stages and its clock die on the game thread. Test owner: Task 8 (`VaRest.Http.Pipeline.ClientDroppedInFlight`).
- **A relative `Location` carrying a URL in its query** (`/cb?code=…&redirect_uri=https://…`), and a secret percent-encoded or JSON-escaped: redacted like any other. Test owner: Task 5 (`VaRest.Http.Redactor.Url`, `.Headers`, `.Bodies`).
- **A token rotated twice, the second time after the first one's 10 minutes**: no timer spin, nothing left armed. Test owner: Task 14 (`VaRest.Http.Log.SecretRotation`) and Task 5 (`VaRest.Http.Redactor.SecretSet`).
- **Upload progress on curl** (a 16 MiB `POST` with `OnProgress`): `BytesSent` moves during the upload, not only at the end. Test owner: Task 16 (`VaRest.Integration.Http.UploadProgress`).

---

## File structure

```
VaRest.uplugin                                + VaRestHttp (Task 2), VaRestTests (Task 10)
Source/VaRestJson/
  Public/VaRestJsonRedact.h                   NEW (Task 1): FVaRestRedactRules, VaRest::Json::RedactPrefix (IN-13)
  Private/VaRestJsonRedact.cpp                NEW (Task 1): SAX re-writer with the cut rules
  Private/VaRestJsonNode.h                    HashKey seeded per process (Task 1, VaRest-sj2)
  Private/VaRestJsonArena.h, VaRestJsonReader.cpp   VaRest-rfd "by M3" minors (Task 1)
  Private/Tests/VaRestJsonRedactTest.cpp      NEW (Task 1)
Source/VaRestHttp/
  VaRestHttp.Build.cs                         NEW (Task 2)
  Public/VaRestHttpTypes.h                    enums, FVaRestHeaders, FVaRestError, FVaRestProgress, FVaRestResponse, TVaRestResult (Task 2)
  Public/VaRestSha256.h                       VaRest::Http::FSha256 (Task 2)
  Public/VaRestHttpSettings.h                 UVaRestHttpSettings (Task 2)
  Public/VaRestUrl.h                          percent/form encoding, URL join, path templates, query, header checks (Task 3)
  Public/VaRestBody.h                         FVaRestForm, FVaRestMultipart, FVaRestSegmentStream, FVaRestRequestBody (Task 3)
  Public/VaRestTrace.h                        the Inspector §9 contract: kinds, payloads, sinks, registry, isolation, counters (Task 4)
  Public/VaRestRedactor.h                     FVaRestSecretSet, FVaRestRedactor (Task 5)
  Public/VaRestClock.h                        IVaRestClock, FVaRestRealClock, FVaRestVirtualClock (Task 6)
  Public/VaRestTransport.h                    IVaRestTransport and its request/result/probe (Task 6); GetDefaultTransport (Task 7)
  Public/VaRestFakeTransport.h                FVaRestFakeTransport (Task 6)
  Public/VaRestHttpTesting.h                  PumpUntil, FLogCapture, test server URL (Task 6); TVaRestTestOutcome, FVaRestTestRig (Task 8)
  Public/VaRestStage.h                        FVaRestAttemptOutcome, IVaRestStage, FVaRestStageNext, FVaRestExchange (Task 8)
  Public/VaRestResultHandler.h                FFinish, IResultHandler, TResultHandler<T>, decode helpers (Task 8)
  Public/VaRestClient.h                       FVaRestClient, FVaRestRequestHandle, TVaRestRequest<T> (Task 8)
  Private/VaRestHttpModule.cpp, VaRestHttpLog.h                         (Task 2)
  Private/VaRestHttpTypes.cpp, VaRestSha256.cpp, VaRestHttpSettings.cpp  (Task 2)
  Private/VaRestUrl.cpp, VaRestBody.cpp                                  (Task 3)
  Private/VaRestTrace.cpp                                                (Task 4)
  Private/VaRestRedactor.cpp, VaRestRedactionPipe.h                     (Task 5)
  Private/VaRestClock.cpp, VaRestFakeTransport.cpp, VaRestHttpTesting.cpp (Task 6)
  Private/VaRestBodySink.h/.cpp, VaRestHttpTransportInternal.h, VaRestHttpTransport.cpp (Task 7)
  Private/VaRestClientState.h/.cpp, VaRestPipeline.cpp, VaRestDecode.h/.cpp, VaRestClient.cpp (Task 8)
  Private/VaRestLimiter.h/.cpp                                           (Task 12)
  Private/VaRestLogStage.cpp                                             (Task 14)
  Private/VaRestTracePoints.h/.cpp                                       (Task 15)
  Private/Tests/VaRestHttp{Module,Url,TraceSurface,Redactor,Clock,FakeTransport,Transport,Pipeline,Builder,Decode,Cancel,Limiter,TimeoutProgress,Log,Trace}Test.cpp
Source/VaRestTests/                           NEW UncookedOnly module (Task 10)
  VaRestTests.Build.cs, Private/VaRestTestsModule.cpp, Private/VaRestHttpTestTypes.h
  Private/VaRestHttpStructDecodeTest.cpp      (Task 10)
  Private/VaRestIntegrationTest.cpp           (Task 16)
  Private/VaRestHttpRequestPathBench.cpp      (Task 17)
Tools/testserver/server.py, selftest.py       NEW (Task 16)
Tools/run_tests.sh                            test server lifecycle (Task 16)
docs/dev/http-thread-completion.md            NEW (Task 17): per-platform verification for the release pass (spec §9)
docs/passport/2026-10-21-http-request-path.md NEW (Task 17)
CLAUDE.md                                     module table (Task 17)
```

---
### Task 1: VaRestJson — M2 re-review residuals; redaction rules and `RedactPrefix` (IN-13); seeded key hash (VaRest-sj2); rfd minors

IN-13 puts the JSON half of the redactor in VaRestJson: "one new public VaRestJson function, `VaRest::Json::RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules&, EVaRestJsonFormat, TArray<uint8>& Out) → {bRedacted, StopReason}`, a SAX re-writer over the existing private reader with the §2.2 cut rules". `FVaRestRedactRules` (the normalised key matcher of Inspector §2.2 layer 1) lives here too, because `RedactPrefix` takes it; VaRestHttp's redactor (Task 5) uses the same rules for headers, query keys, form fields and part names. The HTTP layer is where untrusted JSON arrives from the network, so the per-process hash seed of VaRest-sj2 lands now.

**Files:**
- Create: `Source/VaRestJson/Public/VaRestJsonRedact.h`, `Source/VaRestJson/Private/VaRestJsonRedact.cpp`
- Modify: `Source/VaRestJson/Private/VaRestJsonNode.h` (`HashKey`, the `FStr::Hash` comment), `Source/VaRestJson/Private/VaRestJsonDocument.cpp` (`GetHashSeed`), `Source/VaRestJson/Private/VaRestJsonArena.h` (class comment), `Source/VaRestJson/Private/VaRestJsonReader.cpp` (one `check`)
- Test: `Source/VaRestJson/Private/Tests/VaRestJsonRedactTest.cpp` (new), `Source/VaRestJson/Private/Tests/VaRestJsonDocumentTest.cpp` (one test appended)
- Modify (Step 1, M2 final re-review residuals, bead VaRest-z0y): `Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp`, `Source/VaRestJson/Public/VaRestJsonWriterInternal.h`, `Source/VaRestJson/Private/Tests/VaRestJsonConvertEncodeTest.cpp`, `docs/superpowers/specs/2026-10-20-varest2-socketio-design.md`

**Interfaces:**
- Consumes: `VaRest::Json::FUtf8Sink`, `FWriterAllocator`, `FCondensedWriter`, `FPrettyWriter` (`VaRestJsonWriterInternal.h`); `VaRest::Json::ConvertForDocument`, `FUtf8Scratch` (`VaRestJsonUtf8.h`); `VaRest::Json::MaxDepth`.
- Produces (public, `VaRestJsonRedact.h`): `enum class EVaRestRedactKeyKind : uint8 { Json, Form, Header, Query, Part }`; `class VARESTJSON_API FVaRestRedactRules { void AddKey(FStringView Key); bool ShouldRedact(FUtf8StringView Key, EVaRestRedactKeyKind Kind) const; bool ShouldRedact(FStringView Key, EVaRestRedactKeyKind Kind) const; static void Normalize(FUtf8StringView Key, TArray<UTF8CHAR, TInlineAllocator<64>>& Out); }`; `enum class EVaRestRedactStop : uint8 { Complete, Cap, ParseError }`; `struct FVaRestRedactResult { bool bRedacted; EVaRestRedactStop StopReason; FString Error; int64 ErrorOffset; }`; `VARESTJSON_API FVaRestRedactResult VaRest::Json::RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules& Rules, EVaRestJsonFormat Format, TArray<uint8>& Out, bool bWholeInput = false)` (appends to `Out`). Private: `uint64 VaRest::Json::GetHashSeed()`; `HashKey` now seeded.

- [ ] **Step 1: The M2 final re-review residuals** (controller ruling; bead VaRest-z0y)

Each is a one-line class of change; they are committed on their own before the redaction work. VaRest-z0y lists five; the fifth (two `SoftObjectPath.cpp` citations "one line off") is not applied: in UE 5.8.1 `SoftObjectPath.cpp:189` is the `if (Path.IsEmpty() || Path.Equals(TEXT("None")…))` test (block 189-193) and `:200` is `if (!Path.StartsWith('/')` (rejection 200-217), so the current `189-193` and `200-217` are right. Close VaRest-z0y with that reason after this step's commit.

1. `Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp` (`VaRest.Json.Api.TextIsCultureInvariant`): `SetCurrentCulture` "will set both the language and locale, as well as clear out any asset group cultures" (`Internationalization.h:46-51`), so restoring the language name alone leaves the locale changed for every later test. Replace

```cpp
	FInternationalization& I18N = FInternationalization::Get();
	const FString Previous = I18N.GetCurrentCulture()->GetName();
	if (!TestTrue(TEXT("the de culture can be set"), I18N.SetCurrentCulture(TEXT("de"))))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		I18N.SetCurrentCulture(Previous);
	};
```

with

```cpp
	FInternationalization& I18N = FInternationalization::Get();
	// SetCurrentCulture sets the language and the locale to one culture and clears the asset-group cultures
	// (Internationalization.h:46-51): restoring the language name alone would leave the locale changed for every later
	// test. Snapshot the whole state and restore it (Internationalization.h:39-44, :158, :163).
	FInternationalization::FCultureStateSnapshot Previous;
	I18N.BackupCultureState(Previous);
	if (!TestTrue(TEXT("the de culture can be set"), I18N.SetCurrentCulture(TEXT("de"))))
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		I18N.RestoreCultureState(Previous);
	};
```

2. `Source/VaRestJson/Public/VaRestJsonWriterInternal.h`, `GetConvertedFieldsInternal`'s comment: recompiling a Blueprint struct deletes its properties while the struct object survives (`Editor/KismetCompiler/Private/UserDefinedStructureCompilerUtils.cpp:225`). Replace

```cpp
	 * struct written as a single value (FVaRestJson, FDateTime, FTimespan, FGuid, soft paths). Any thread; the
	 * properties belong to Struct and live as long as it does.
```

with

```cpp
	 * struct written as a single value (FVaRestJson, FDateTime, FTimespan, FGuid, soft paths). Any thread. The list is
	 * valid until Struct's layout changes: recompiling a Blueprint struct in the editor deletes its properties while the
	 * struct object survives (Editor/KismetCompiler/Private/UserDefinedStructureCompilerUtils.cpp:225). So fetch it for
	 * each use -- it reads the cached field map, which is cheap -- and never cache it.
```

3. `Source/VaRestJson/Private/Tests/VaRestJsonConvertEncodeTest.cpp` (the `WriterEntries` positional block): after a false write the writer is unusable (`VaRestJsonWriterInternal.h`), so the array may be closed only when every write succeeded. Replace

```cpp
	ArgWriter.EndArray();
	TestTrue(TEXT("every converted field writes"), bAllWritten);
```

with

```cpp
	// After a false write the writer is unusable (the contract in VaRestJsonWriterInternal.h): close the array only when
	// every write succeeded, so a field that fails part-way fails this test instead of a check
	if (bAllWritten)
	{
		ArgWriter.EndArray();
	}
	TestTrue(TEXT("every converted field writes"), bAllWritten);
```

4. `docs/superpowers/specs/2026-10-20-varest2-socketio-design.md`, the J-2 decode row of the §18.2 table: in

```
**covered**: Task 8 `JsonToProperty`, over `GetConvertedFieldsInternal(Struct)` (M2 final review I1: not `TFieldIterator<FProperty>`)
```

replace `` `GetConvertedFieldsInternal(Struct)` `` with `` `GetConvertedFieldsInternal(Struct, Options)`, with the same `Options` as the encode side (the naming policy decides which of two colliding fields is kept) ``.

Run `Tools/run_tests.sh VaRest.Json`; expected `0 failed`. Commit, then close the bead:

```bash
git add Source/VaRestJson/Private/Tests/VaRestJsonApiTest.cpp Source/VaRestJson/Public/VaRestJsonWriterInternal.h \
  Source/VaRestJson/Private/Tests/VaRestJsonConvertEncodeTest.cpp docs/superpowers/specs/2026-10-20-varest2-socketio-design.md
git commit -m "json: M2 final re-review residuals -- culture state restore, converted-field lifetime, writer contract in a test, J-2 options

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
bd close VaRest-z0y --reason="Items 1-4 fixed in M3 Task 1 Step 1. Item 5 not applied: SoftObjectPath.cpp:189-193 and :200-217 are already right in UE 5.8.1 (M3 plan review M1)."
```

- [ ] **Step 2: Write the failing tests** — `Source/VaRestJson/Private/Tests/VaRestJsonRedactTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestJsonRedact.h"

namespace VaRest::Json::RedactTestPrivate
{
	FString Redact(FUtf8StringView Input, bool bWhole, FVaRestRedactResult& OutResult,
		EVaRestJsonFormat Format = EVaRestJsonFormat::Condensed, const FVaRestRedactRules& Rules = FVaRestRedactRules())
	{
		TArray<uint8> Out;
		OutResult = VaRest::Json::RedactPrefix(Input, Rules, Format, Out, bWhole);
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Out.GetData()), Out.Num());
	}

	FUtf8StringView V(const char* Text)
	{
		return FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRedactKeysTest, "VaRest.Json.Redact.Keys",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRedactKeysTest::RunTest(const FString& Parameters)
{
	using K = EVaRestRedactKeyKind;
	const FVaRestRedactRules Rules;

	// Inspector spec 2.2 layer 1: lower-cased, '_' '-' '.' removed, then exact names or contained substrings.
	// VaRest's own default output is camelCase (core spec 2.4), so accessToken/clientSecret must be caught.
	const TCHAR* JsonSecrets[] = { TEXT("password"), TEXT("Password"), TEXT("user_password"), TEXT("passwd"), TEXT("accessToken"),
		TEXT("refresh-token"), TEXT("refresh_token"), TEXT("clientSecret"), TEXT("idToken"), TEXT("apiKey"), TEXT("api_key"),
		TEXT("sessionToken"), TEXT("privateKey"), TEXT("jwt"), TEXT("sig"), TEXT("key"), TEXT("KEY"), TEXT("credentials"), TEXT("signature") };
	for (const TCHAR* Key : JsonSecrets)
	{
		TestTrue(FString::Printf(TEXT("JSON key %s is a secret"), Key), Rules.ShouldRedact(FStringView(Key), K::Json));
	}
	const TCHAR* HeaderSecrets[] = { TEXT("Authorization"), TEXT("Proxy-Authorization"), TEXT("Cookie"), TEXT("Set-Cookie"),
		TEXT("X-Api-Key"), TEXT("X-CSRF-Token"), TEXT("X-Session-ID") };
	for (const TCHAR* Key : HeaderSecrets)
	{
		TestTrue(FString::Printf(TEXT("header %s is a secret"), Key), Rules.ShouldRedact(FStringView(Key), K::Header));
	}
	const TCHAR* QuerySecrets[] = { TEXT("token"), TEXT("access_token"), TEXT("X-Amz-Signature"), TEXT("X-Amz-Credential"),
		TEXT("X-Amz-Security-Token"), TEXT("X-Goog-Signature"), TEXT("sig"), TEXT("code") };
	for (const TCHAR* Key : QuerySecrets)
	{
		TestTrue(FString::Printf(TEXT("query key %s is a secret"), Key), Rules.ShouldRedact(FStringView(Key), K::Query));
	}
	const TCHAR* Plain[] = { TEXT("Idempotency-Key"), TEXT("idempotency_key"), TEXT("token_type"), TEXT("tokenType"), TEXT("username"),
		TEXT("id"), TEXT("keyboard"), TEXT("monkey"), TEXT("name"), TEXT("Content-Type") };
	for (const TCHAR* Key : Plain)
	{
		TestFalse(FString::Printf(TEXT("%s is not a secret (JSON)"), Key), Rules.ShouldRedact(FStringView(Key), K::Json));
		TestFalse(FString::Printf(TEXT("%s is not a secret (header)"), Key), Rules.ShouldRedact(FStringView(Key), K::Header));
	}
	TestFalse(TEXT("code is a secret only as a query key (OAuth authorization codes)"), Rules.ShouldRedact(FStringView(TEXT("code")), K::Json));
	TestTrue(TEXT("the substring rule over-redacts on purpose (Inspector ruling R1's stated cost)"), Rules.ShouldRedact(FStringView(TEXT("sessionCount")), K::Json));
	TestTrue(TEXT("UTF-8 keys normalise the same way"), Rules.ShouldRedact(VaRest::Json::RedactTestPrivate::V("ACCESS.TOKEN"), K::Json));

	FVaRestRedactRules Added;
	Added.AddKey(TEXT("player_pin"));
	TestTrue(TEXT("an added key matches after normalisation"), Added.ShouldRedact(FStringView(TEXT("playerPin")), K::Form));
	TestFalse(TEXT("an added key matches exactly, not as a substring"), Added.ShouldRedact(FStringView(TEXT("pin")), K::Form));
	TestFalse(TEXT("... nor inside a longer key"), Added.ShouldRedact(FStringView(TEXT("playerPinHint")), K::Form));
	TestTrue(TEXT("the built-in rules still apply"), Added.ShouldRedact(FStringView(TEXT("password")), K::Form));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRedactWholeTest, "VaRest.Json.Redact.WholeInput",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRedactWholeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::RedactTestPrivate;
	FVaRestRedactResult R;

	const FString Nested = Redact(V(R"({"user":{"name":"neo","password":"canary-1"},"items":[{"token":{"a":[1,2]}},{"keep":true}],"n":5})"), true, R);
	TestEqualSensitive(TEXT("a secret's value of any type becomes \"***\""), Nested,
		FString(TEXT(R"({"user":{"name":"neo","password":"***"},"items":[{"token":"***"},{"keep":true}],"n":5})")));
	TestTrue(TEXT("... reported as redacted"), R.bRedacted);
	TestTrue(TEXT("... and complete"), R.StopReason == EVaRestRedactStop::Complete);

	const FString Numbers = Redact(V(R"({"big":9007199254740993,"f":1.50e+3,"neg":-0})"), true, R);
	TestEqualSensitive(TEXT("numbers are copied as written (raw numbers, no double round trip)"), Numbers, FString(TEXT(R"({"big":9007199254740993,"f":1.50e+3,"neg":-0})")));
	TestFalse(TEXT("nothing to redact"), R.bRedacted);

	const FString Escaped = Redact(V(R"({"password":"x","t":"a\"b\\c\n"})"), true, R);
	TestEqualSensitive(TEXT("keys are matched unescaped; strings are re-escaped"), Escaped, FString(TEXT(R"({"password":"***","t":"a\"b\\c\n"})")));

	TestEqualSensitive(TEXT("a whole input's final number is complete"), Redact(V("42"), true, R), FString(TEXT("42")));
	TestTrue(TEXT("... Complete"), R.StopReason == EVaRestRedactStop::Complete);

	// The parser skips a UTF-8 BOM (VaRestJsonReader.cpp, BomLength); the excerpt does too (review M5)
	TestEqualSensitive(TEXT("a leading BOM is skipped"), Redact(V("\xEF\xBB\xBF{\"password\":\"x\"}"), true, R), FString(TEXT(R"({"password":"***"})")));
	TestTrue(TEXT("... and the excerpt is complete"), R.StopReason == EVaRestRedactStop::Complete);

	const FString Pretty = Redact(V(R"({"a":[1,2],"password":"x"})"), true, R, EVaRestJsonFormat::Pretty);
	TestEqualSensitive(TEXT("pretty: two-space indent, like ToString"), Pretty,
		FString(TEXT("{\n  \"a\": [\n    1,\n    2\n  ],\n  \"password\": \"***\"\n}")));

	const FString Trailing = Redact(V(R"({"a":1,})"), true, R);
	TestEqualSensitive(TEXT("a parse error ends at the last complete token"), Trailing, FString(TEXT(R"({"a":1)")));
	TestTrue(TEXT("... noted as a parse error"), R.StopReason == EVaRestRedactStop::ParseError && !R.Error.IsEmpty() && R.ErrorOffset > 0);

	const FString Garbage = Redact(V(R"({"a":1}x)"), true, R);
	TestEqualSensitive(TEXT("the root is kept before trailing garbage"), Garbage, FString(TEXT(R"({"a":1})")));
	TestTrue(TEXT("... which is a parse error"), R.StopReason == EVaRestRedactStop::ParseError);

	TArray<uint8> BadUtf8 = { '"', 0xC3, 0x28, '"' };
	TArray<uint8> Out;
	R = VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(BadUtf8.GetData()), BadUtf8.Num()), FVaRestRedactRules(), EVaRestJsonFormat::Condensed, Out, true);
	TestTrue(TEXT("invalid UTF-8 is a parse error, never copied"), R.StopReason == EVaRestRedactStop::ParseError && Out.Num() == 0);

	FString Deep;
	for (int32 i = 0; i < 600; ++i)
	{
		Deep.AppendChar(TEXT('['));
	}
	FTCHARToUTF8 DeepUtf8(*Deep);
	Out.Reset();
	R = VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(DeepUtf8.Get()), DeepUtf8.Length()), FVaRestRedactRules(), EVaRestJsonFormat::Condensed, Out, false);
	TestTrue(TEXT("nesting past MaxDepth stops with an error"), R.StopReason == EVaRestRedactStop::ParseError && R.Error.Contains(TEXT("Nesting deeper")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonRedactCutTest, "VaRest.Json.Redact.CutRules",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonRedactCutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Json::RedactTestPrivate;
	FVaRestRedactResult R;

	// Inspector spec 2.2: "a matching key's value of any type, complete or cut, becomes "***"; tokens cut by the cap are
	// never written, and neither is a scalar ending exactly at the cap"
	const FString InValue = Redact(V(R"({"a":1,"password":"canary-2-lo)"), false, R);
	TestEqualSensitive(TEXT("cap inside a secret's value: the value is still ***"), InValue, FString(TEXT(R"({"a":1,"password":"***")")));
	TestFalse(TEXT("... and no canary byte"), InValue.Contains(TEXT("canary")));
	TestTrue(TEXT("... stopped by the cap"), R.StopReason == EVaRestRedactStop::Cap && R.bRedacted);

	const FString InKey = Redact(V(R"({"a":1,"passw)"), false, R);
	TestEqualSensitive(TEXT("cap inside a key: the key is not written"), InKey, FString(TEXT(R"({"a":1)")));
	TestTrue(TEXT("... Cap"), R.StopReason == EVaRestRedactStop::Cap);

	const FString InNumber = Redact(V(R"({"a":123)"), false, R);
	TestEqualSensitive(TEXT("cap inside a number: the number is not written"), InNumber, FString(TEXT(R"({"a":)")));
	TestTrue(TEXT("... Cap"), R.StopReason == EVaRestRedactStop::Cap);

	const FString AtCap = Redact(V(R"({"n":42)"), false, R);
	TestEqualSensitive(TEXT("a number ending exactly at the cap may be the front of a longer one: not written"), AtCap, FString(TEXT(R"({"n":)")));
	TestEqualSensitive(TEXT("a bare number ending at the cap"), Redact(V("42"), false, R), FString());
	TestTrue(TEXT("... Cap"), R.StopReason == EVaRestRedactStop::Cap);

	const FString InString = Redact(V(R"({"a":"xy","b":"unfini)"), false, R);
	TestEqualSensitive(TEXT("a complete string is kept; a cut one is not"), InString, FString(TEXT(R"({"a":"xy","b":)")));

	const FString InLiteral = Redact(V(R"({"a":tr)"), false, R);
	TestEqualSensitive(TEXT("a cut literal is not written"), InLiteral, FString(TEXT(R"({"a":)")));
	TestTrue(TEXT("... Cap, not a parse error"), R.StopReason == EVaRestRedactStop::Cap);

	TArray<uint8> CutChar = { '{', '"', 'a', '"', ':', '"', 0xD0 };
	TArray<uint8> Out;
	R = VaRest::Json::RedactPrefix(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(CutChar.GetData()), CutChar.Num()), FVaRestRedactRules(), EVaRestJsonFormat::Condensed, Out, false);
	TestTrue(TEXT("a cap inside a multi-byte character is a cut, not an encoding error"), R.StopReason == EVaRestRedactStop::Cap);
	return true;
}

#endif
```

Append to `Source/VaRestJson/Private/Tests/VaRestJsonDocumentTest.cpp`, before the final `#endif`:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestJsonDocHashSeededTest, "VaRest.Json.Document.HashSeeded",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestJsonDocHashSeededTest::RunTest(const FString& Parameters)
{
	// VaRest-sj2: with the unseeded CityHash32 and linear probing, keys crafted to collide make every index build O(n^2)
	// (a dedicated server parsing client JSON). The hash is never persisted, so a per-process seed costs nothing.
	const FUtf8StringView Key(UTF8TEXT("player_id"));
	const uint32 Seeded = VaRest::Json::HashKey(Key);
	TestEqual(TEXT("stable within the process"), VaRest::Json::HashKey(Key), Seeded);
	TestNotEqual(TEXT("not the unseeded CityHash32"), Seeded, CityHash32(reinterpret_cast<const char*>(Key.GetData()), static_cast<uint32>(Key.Len())));

	// Indexed objects (more than IndexThreshold members) still find every key
	FString Text = TEXT("{");
	for (int32 i = 0; i < 40; ++i)
	{
		Text += FString::Printf(TEXT("%s\"k%d\":%d"), i ? TEXT(",") : TEXT(""), i, i);
	}
	Text += TEXT("}");
	FVaRestJson J = FVaRestJson::Parse(FStringView(Text)).StealValue();
	TestTrue(TEXT("indexed"), J.GetNodeInternal()->Obj.Index != nullptr);
	bool bAllFound = true;
	for (int32 i = 0; i < 40; ++i)
	{
		bAllFound &= J[*FString::Printf(TEXT("k%d"), i)].AsInt(-1) == i;
	}
	TestTrue(TEXT("every key is found through the seeded index"), bAllFound);
	return true;
}
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: the build fails on `VaRestJsonRedact.h` (no such file). That is the RED for the new API.

- [ ] **Step 4: The public header** — `Source/VaRestJson/Public/VaRestJsonRedact.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

/** Where a key comes from: the exact-name rules differ per kind (Inspector spec 2.2, layer 1) */
enum class EVaRestRedactKeyKind : uint8
{
	Json,
	Form,
	Header,
	Query,
	/** A multipart part name */
	Part
};

/**
 * Which keys hold secrets (Inspector spec 2.2 layer 1 and IN-13; they replace core spec 4.5's lists). A key is normalised --
 * ASCII letters lower-cased, '_', '-' and '.' removed -- and is redacted when it equals an added key; otherwise, unless it
 * is exactly idempotencykey or tokentype, when it equals sig or key (and, for a query key, code), or contains password,
 * passwd, secret, token, apikey, authorization, credential, signature, session, cookie, privatekey or jwt. That covers
 * VaRest's own camelCase output (accessToken, clientSecret), presigned URLs (X-Amz-Signature, X-Goog-Signature, Azure sig)
 * and the usual headers (Authorization, Cookie, Set-Cookie, X-Api-Key, X-CSRF-Token). Build it, then share it: reading is
 * thread-safe, AddKey is not.
 */
class VARESTJSON_API FVaRestRedactRules
{
public:
	/** An exact key, normalised like every key: a client's auth token path or header name, a project addition */
	void AddKey(FStringView Key);

	bool ShouldRedact(FUtf8StringView Key, EVaRestRedactKeyKind Kind) const;
	bool ShouldRedact(FStringView Key, EVaRestRedactKeyKind Kind) const;

	/** ASCII letters lower-cased, '_', '-' and '.' dropped; every other byte kept */
	static void Normalize(FUtf8StringView Key, TArray<UTF8CHAR, TInlineAllocator<64>>& Out);

private:
	bool MatchesNormalized(FUtf8StringView Normalized, EVaRestRedactKeyKind Kind) const;

	TArray<TArray<UTF8CHAR>> AddedKeys;
};

enum class EVaRestRedactStop : uint8
{
	/** The whole input was read */
	Complete,
	/** The input ended mid-token: it is a prefix cut by a size cap */
	Cap,
	/** Invalid JSON (or nesting past VaRest::Json::MaxDepth): the output ends at the last complete token */
	ParseError
};

struct FVaRestRedactResult
{
	bool bRedacted = false;
	EVaRestRedactStop StopReason = EVaRestRedactStop::Complete;
	/** ParseError: what was wrong (RapidJSON's English message) */
	FString Error;
	/** ParseError: the byte offset into the input, else -1 */
	int64 ErrorOffset = -1;
};

namespace VaRest::Json
{
	/**
	 * Re-writes the JSON text Prefix into Out (appended) with every secret's value replaced by "***" (IN-13). Prefix is
	 * the first bytes of a body that may continue past it: a token the end cuts is never written, nor a number that ends
	 * exactly at the end (it may be the front of a longer one), and a secret's value becomes "***" whether it is complete,
	 * cut or nested (Inspector spec 2.2). bWholeInput says Prefix is the whole body, so a final number is complete.
	 * Numbers are copied as written. A parse error ends the output at the last complete token, with the error in the
	 * result. Any thread; the output is a JSON prefix, not always a document (containers the cut left open stay open).
	 */
	VARESTJSON_API FVaRestRedactResult RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules& Rules, EVaRestJsonFormat Format,
		TArray<uint8>& Out, bool bWholeInput = false);
}
```

- [ ] **Step 5: The implementation** — `Source/VaRestJson/Private/VaRestJsonRedact.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestJsonRedact.h"

#include "RapidJsonIncludes.h"
#include "JsonUtils/RapidJsonUtils.h"
#include "String/Find.h"
#include "VaRestJsonUtf8.h"
#include "VaRestJsonWriterInternal.h"

namespace VaRest::Json::RedactPrivate
{
	using FKeyBuffer = TArray<UTF8CHAR, TInlineAllocator<64>>;

	/** Inspector spec 2.2 layer 1: a normalised key containing one of these is a secret */
	const UTF8CHAR* const Substrings[] = { UTF8TEXT("password"), UTF8TEXT("passwd"), UTF8TEXT("secret"), UTF8TEXT("token"),
		UTF8TEXT("apikey"), UTF8TEXT("authorization"), UTF8TEXT("credential"), UTF8TEXT("signature"), UTF8TEXT("session"),
		UTF8TEXT("cookie"), UTF8TEXT("privatekey"), UTF8TEXT("jwt") };

	/** ... unless it is exactly one of these */
	const UTF8CHAR* const Allowed[] = { UTF8TEXT("idempotencykey"), UTF8TEXT("tokentype") };

	bool IsExactly(FUtf8StringView Key, const UTF8CHAR* Name)
	{
		return Key.Equals(FUtf8StringView(Name), ESearchCase::CaseSensitive);
	}

	/**
	 * Same flags as the parser's (VaRestJsonReader.cpp) minus in situ -- Prefix is a view we must not modify -- plus
	 * kParseNumbersAsStringsFlag, so a number is re-written exactly as it was sent. Iterative: hostile depth cannot
	 * overflow the stack. The reader's stack goes through FMemory (JsonUtils/RapidJsonUtils.h:19-53).
	 */
	using FReader = rapidjson::GenericReader<rapidjson::UTF8<>, rapidjson::UTF8<>, UE::Json::Private::FAllocatorImpl>;
	constexpr unsigned Flags = rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag | rapidjson::kParseNumbersAsStringsFlag;
	static_assert((Flags & rapidjson::kParseIterativeFlag) != 0, "The reader must stay iterative: recursive parsing overflows the stack on hostile depth");

	/** SAX handler that mirrors each event into Writer, except a secret's value, which becomes "***" */
	template <typename WriterType>
	class TRedactHandler
	{
	public:
		TRedactHandler(WriterType& InWriter, const FVaRestRedactRules& InRules, const rapidjson::MemoryStream& InStream, bool bInCutAtEnd)
			: Writer(InWriter)
			, Rules(InRules)
			, Stream(InStream)
			, bCutAtEnd(bInCutAtEnd)
		{
		}

		bool Null() { return Scalar([this] { Writer.Null(); }); }
		bool Bool(bool bValue) { return Scalar([this, bValue] { Writer.Bool(bValue); }); }
		// kParseNumbersAsStringsFlag sends every number to RawNumber; these complete the handler concept (reader.h:173-196)
		bool Int(int Value) { return Scalar([this, Value] { Writer.Int(Value); }); }
		bool Uint(unsigned Value) { return Scalar([this, Value] { Writer.Uint(Value); }); }
		bool Int64(int64_t Value) { return Scalar([this, Value] { Writer.Int64(Value); }); }
		bool Uint64(uint64_t Value) { return Scalar([this, Value] { Writer.Uint64(Value); }); }
		bool Double(double Value) { return Scalar([this, Value] { Writer.Double(Value); }); }

		bool RawNumber(const char* Text, rapidjson::SizeType Length, bool)
		{
			// The number's digits are consumed when this fires, so Tell() is where it ends. Ending exactly where a cut input
			// ends, it may be the front of a longer number ("12" of "1234"): never written (Inspector spec 2.2).
			if (bCutAtEnd && Stream.Tell() == Stream.size_)
			{
				bStoppedAtCut = true;
				return false;
			}
			// RawValue, not Writer::RawNumber: that one quotes its text (writer.h:197-202)
			return Scalar([this, Text, Length] { Writer.RawValue(Text, Length, rapidjson::kNumberType); });
		}

		bool String(const char* Text, rapidjson::SizeType Length, bool)
		{
			return Scalar([this, Text, Length] { Writer.String(Text, Length); });
		}

		bool Key(const char* Text, rapidjson::SizeType Length, bool)
		{
			if (SkipLevel != INDEX_NONE)
			{
				return true;
			}
			Writer.Key(Text, Length);
			if (Rules.ShouldRedact(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text), Length), EVaRestRedactKeyKind::Json))
			{
				// Written before the value is read, so a value of any type, complete or cut, never shows (Inspector spec 2.2)
				Writer.String("***", 3);
				bRedacted = true;
				bSkipValue = true;
			}
			return true;
		}

		bool StartObject() { return Open([this] { Writer.StartObject(); }); }
		bool StartArray() { return Open([this] { Writer.StartArray(); }); }
		bool EndObject(rapidjson::SizeType) { return Close([this] { Writer.EndObject(); }); }
		bool EndArray(rapidjson::SizeType) { return Close([this] { Writer.EndArray(); }); }

		bool bRedacted = false;
		bool bStoppedAtCut = false;
		bool bTooDeep = false;

	private:
		template <typename FuncType>
		bool Scalar(FuncType&& Write)
		{
			if (SkipLevel != INDEX_NONE)
			{
				return true;
			}
			if (bSkipValue)
			{
				bSkipValue = false;
				return true;
			}
			Write();
			return true;
		}

		template <typename FuncType>
		bool Open(FuncType&& Write)
		{
			if (Depth >= VaRest::Json::MaxDepth)
			{
				bTooDeep = true;
				return false;
			}
			++Depth;
			if (SkipLevel != INDEX_NONE)
			{
				return true;
			}
			if (bSkipValue)
			{
				// A secret's value is a container: skip everything until it closes
				bSkipValue = false;
				SkipLevel = Depth;
				return true;
			}
			Write();
			return true;
		}

		template <typename FuncType>
		bool Close(FuncType&& Write)
		{
			const int32 Closing = Depth--;
			if (SkipLevel != INDEX_NONE)
			{
				if (Closing == SkipLevel)
				{
					SkipLevel = INDEX_NONE;
				}
				return true;
			}
			Write();
			return true;
		}

		WriterType& Writer;
		const FVaRestRedactRules& Rules;
		const rapidjson::MemoryStream& Stream;
		const bool bCutAtEnd;
		int32 Depth = 0;
		/** Depth of the redacted container being skipped, or INDEX_NONE */
		int32 SkipLevel = INDEX_NONE;
		/** The next value belongs to a redacted key */
		bool bSkipValue = false;
	};
}

void FVaRestRedactRules::Normalize(FUtf8StringView Key, TArray<UTF8CHAR, TInlineAllocator<64>>& Out)
{
	Out.Reset(Key.Len());
	for (const UTF8CHAR Ch : Key)
	{
		if (Ch == '_' || Ch == '-' || Ch == '.')
		{
			continue;
		}
		Out.Add(Ch >= 'A' && Ch <= 'Z' ? static_cast<UTF8CHAR>(Ch - 'A' + 'a') : Ch);
	}
}

void FVaRestRedactRules::AddKey(FStringView Key)
{
	VaRest::Json::FUtf8Scratch Scratch;
	RedactPrivate::FKeyBuffer Normalized;
	Normalize(VaRest::Json::ConvertForDocument(Key, Scratch, /*bWarn*/ false), Normalized);
	if (Normalized.Num() == 0)
	{
		return;
	}
	const FUtf8StringView View(Normalized.GetData(), Normalized.Num());
	for (const TArray<UTF8CHAR>& Existing : AddedKeys)
	{
		if (View.Equals(FUtf8StringView(Existing.GetData(), Existing.Num()), ESearchCase::CaseSensitive))
		{
			return;
		}
	}
	AddedKeys.Emplace(Normalized.GetData(), Normalized.Num());
}

bool FVaRestRedactRules::ShouldRedact(FUtf8StringView Key, EVaRestRedactKeyKind Kind) const
{
	RedactPrivate::FKeyBuffer Normalized;
	Normalize(Key, Normalized);
	return MatchesNormalized(FUtf8StringView(Normalized.GetData(), Normalized.Num()), Kind);
}

bool FVaRestRedactRules::ShouldRedact(FStringView Key, EVaRestRedactKeyKind Kind) const
{
	VaRest::Json::FUtf8Scratch Scratch;
	return ShouldRedact(VaRest::Json::ConvertForDocument(Key, Scratch, /*bWarn*/ false), Kind);
}

bool FVaRestRedactRules::MatchesNormalized(FUtf8StringView Normalized, EVaRestRedactKeyKind Kind) const
{
	using namespace VaRest::Json::RedactPrivate;
	if (Normalized.IsEmpty())
	{
		return false;
	}
	for (const TArray<UTF8CHAR>& Added : AddedKeys)
	{
		if (Normalized.Equals(FUtf8StringView(Added.GetData(), Added.Num()), ESearchCase::CaseSensitive))
		{
			return true;
		}
	}
	for (const UTF8CHAR* Name : Allowed)
	{
		if (IsExactly(Normalized, Name))
		{
			return false;
		}
	}
	if (IsExactly(Normalized, UTF8TEXT("sig")) || IsExactly(Normalized, UTF8TEXT("key")))
	{
		return true;
	}
	if (Kind == EVaRestRedactKeyKind::Query && IsExactly(Normalized, UTF8TEXT("code")))
	{
		return true;
	}
	for (const UTF8CHAR* Part : Substrings)
	{
		if (UE::String::FindFirst(Normalized, FUtf8StringView(Part), ESearchCase::CaseSensitive) != INDEX_NONE)
		{
			return true;
		}
	}
	return false;
}

FVaRestRedactResult VaRest::Json::RedactPrefix(FUtf8StringView Prefix, const FVaRestRedactRules& Rules, EVaRestJsonFormat Format,
	TArray<uint8>& Out, bool bWholeInput)
{
	using namespace RedactPrivate;
	// The document parser skips a UTF-8 BOM (VaRestJsonReader.cpp, BomLength); so does the excerpt, or a body with one
	// would excerpt as "parse error at byte 0". Offsets below stay relative to the input as given.
	const int32 Bom = Prefix.StartsWith(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>("\xEF\xBB\xBF"), 3)) ? 3 : 0;
	Prefix.RightChopInline(Bom);
	// MemoryStream: a bounded byte stream whose Peek() is '\0' at the end (memorystream.h:42-47), so the view needs
	// neither a terminator nor a copy
	rapidjson::MemoryStream Stream(reinterpret_cast<const char*>(Prefix.GetData()), static_cast<size_t>(Prefix.Len()));
	FUtf8Sink Sink{ Out };
	FWriterAllocator WriterStack;
	UE::Json::Private::FAllocatorImpl ReaderStack;
	FReader Reader(&ReaderStack);
	rapidjson::ParseResult Parsed;
	bool bRedacted = false;
	bool bStoppedAtCut = false;
	bool bTooDeep = false;

	auto Run = [&](auto& Writer)
	{
		TRedactHandler<std::decay_t<decltype(Writer)>> Handler(Writer, Rules, Stream, !bWholeInput);
		Parsed = Reader.Parse<Flags>(Stream, Handler);
		bRedacted = Handler.bRedacted;
		bStoppedAtCut = Handler.bStoppedAtCut;
		bTooDeep = Handler.bTooDeep;
	};
	if (Format == EVaRestJsonFormat::Pretty)
	{
		FPrettyWriter Writer(Sink, &WriterStack);
		Writer.SetIndent(' ', 2);
		Run(Writer);
	}
	else
	{
		FCondensedWriter Writer(Sink, &WriterStack);
		Run(Writer);
	}

	FVaRestRedactResult Result;
	Result.bRedacted = bRedacted;
	if (!Parsed.IsError())
	{
		return Result;
	}
	const int64 Offset = static_cast<int64>(Parsed.Offset()) + Bom;
	if (bTooDeep)
	{
		Result.StopReason = EVaRestRedactStop::ParseError;
		Result.Error = FString::Printf(TEXT("Nesting deeper than %d"), VaRest::Json::MaxDepth);
		Result.ErrorOffset = Offset;
		return Result;
	}
	// A cut input runs out mid-token. RapidJSON reports that at the end of the input -- or, inside a multi-byte
	// character, at its first byte, up to 3 bytes before the end (kParseErrorStringInvalidEncoding). So a genuine
	// syntax error within the last 4 bytes of a cut input is reported as Cap too: the excerpt is the same (it ends at
	// the last complete token) and only the stop reason is less precise (review M4).
	if (!bWholeInput && (bStoppedAtCut || Offset - Bom + 4 >= Prefix.Len()))
	{
		Result.StopReason = EVaRestRedactStop::Cap;
		return Result;
	}
	Result.StopReason = EVaRestRedactStop::ParseError;
	Result.Error = FString(rapidjson::GetParseError_En(Parsed.Code()));
	Result.ErrorOffset = Offset;
	return Result;
}
```

- [ ] **Step 6: Seed the key hash (VaRest-sj2)**

In `Source/VaRestJson/Private/VaRestJsonNode.h`, replace

```cpp
		/** CityHash32 of the bytes; set for object keys, 0 otherwise */
		uint32 Hash;
```

with

```cpp
		/** HashKey of the bytes; set for object keys, 0 otherwise */
		uint32 Hash;
```

and replace the `HashKey` function with:

```cpp
	/** This process's hash seed (VaRestJsonDocument.cpp) */
	uint64 GetHashSeed();

	/**
	 * Seeded per process (VaRest-sj2): with a fixed hash and linear probing, keys crafted to collide make every index build
	 * O(n^2), and a dedicated server parses client JSON. The hash is never persisted, so the seed costs nothing.
	 */
	inline uint32 HashKey(FUtf8StringView Key)
	{
		return static_cast<uint32>(CityHash64WithSeed(reinterpret_cast<const char*>(Key.GetData()), static_cast<uint32>(Key.Len()), GetHashSeed()));
	}
```

In `Source/VaRestJson/Private/VaRestJsonDocument.cpp`, after the includes, add:

```cpp
uint64 VaRest::Json::GetHashSeed()
{
	// FGuid::NewGuid takes the platform's random source; a function-local static is initialised once, thread-safely,
	// on first use -- no static-initialisation order with the engine's own startup
	static const uint64 Seed = []
	{
		const FGuid Guid = FGuid::NewGuid();
		return ((static_cast<uint64>(Guid.A) << 32) | Guid.B) ^ ((static_cast<uint64>(Guid.C) << 32) | Guid.D) ^ FPlatformTime::Cycles64();
	}();
	return Seed;
}
```

(add `#include "Misc/Guid.h"` and `#include "HAL/PlatformTime.h"` to its includes if they are not reached through `CoreMinimal.h`).

- [ ] **Step 7: The two VaRest-rfd "by M3" minors**

In `Source/VaRestJson/Private/VaRestJsonArena.h`, replace the class comment

```cpp
	/**
	 * Chunked bump allocator. Memory is released only when the arena dies, and nothing it hands
	 * out ever moves -- that is what lets a FVaRestJson handle hold a raw node pointer while the
	 * node's parent keeps growing.
	 */
```

with

```cpp
	/**
	 * Chunked bump allocator. Memory is released only when the arena dies, and nothing it hands
	 * out ever moves -- that is what lets a FVaRestJson handle hold a raw node pointer while the
	 * node's parent keeps growing. Single writer: allocating is not thread-safe. Only a thread that
	 * mutates the document allocates, and mutation needs exclusive access (spec 2.2 thread contract);
	 * reads never allocate, so concurrent readers are safe.
	 */
```

In `Source/VaRestJson/Private/VaRestJsonReader.cpp`, after `check(Doc.Input.Num() > 0 && Doc.Input.Last() == 0);` add:

```cpp
		check(StartOffset >= 0 && StartOffset < Doc.Input.Num());
```

- [ ] **Step 8: Run and pass**

Run: `Tools/run_tests.sh VaRest.Json`
Expected: `0 failed`, including `VaRest.Json.Redact.Keys`, `VaRest.Json.Redact.WholeInput`, `VaRest.Json.Redact.CutRules` and `VaRest.Json.Document.HashSeeded`; the M1/M2 suite stays green.

- [ ] **Step 9: Restore-the-bug (cut rule, Inspector R2)**

In `TRedactHandler::RawNumber`, temporarily replace the condition with `if (false)`. Run `Tools/run_tests.sh VaRest.Json.Redact`; expected FAIL in `VaRest.Json.Redact.CutRules` ("a number ending exactly at the cap ... not written" shows `{"n":42`). Restore; run again; PASS. Report both runs.

- [ ] **Step 10: Restore-the-bug (normalised keys, Inspector R11)**

In `FVaRestRedactRules::ShouldRedact(FUtf8StringView, ...)`, temporarily call `MatchesNormalized(Key, Kind)` without normalising. Run `Tools/run_tests.sh VaRest.Json.Redact`; expected FAIL in `VaRest.Json.Redact.Keys`: `Password`, `accessToken`, `clientSecret`, `idToken`, `X-Api-Key`, `ACCESS.TOKEN` and the added-key check `playerPin` fail (`refresh_token` still passes: it contains `token` as written). Restore; PASS. Report both runs.

- [ ] **Step 11: Restore-the-bug (seeded hash)**

In `HashKey`, temporarily return `CityHash32(...)` of the bytes. Run `Tools/run_tests.sh VaRest.Json.Document`; expected FAIL in `VaRest.Json.Document.HashSeeded` ("not the unseeded CityHash32"). Restore; PASS.

- [ ] **Step 12: Commit**

```bash
git add Source/VaRestJson/Public/VaRestJsonRedact.h Source/VaRestJson/Private/VaRestJsonRedact.cpp \
  Source/VaRestJson/Private/VaRestJsonNode.h Source/VaRestJson/Private/VaRestJsonDocument.cpp \
  Source/VaRestJson/Private/VaRestJsonArena.h Source/VaRestJson/Private/VaRestJsonReader.cpp \
  Source/VaRestJson/Private/Tests/VaRestJsonRedactTest.cpp Source/VaRestJson/Private/Tests/VaRestJsonDocumentTest.cpp
git commit -m "json: RedactPrefix and FVaRestRedactRules (IN-13); per-process key hash seed (VaRest-sj2); rfd M3 minors

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 2: Module `VaRestHttp` — skeleton, core types, settings, SHA-256

The module the rest of M3 builds in (spec §1: Runtime, `PreDefault`, the five platforms), with the types every later task shares: enums, `FVaRestHeaders` (case-insensitive names, repeats kept), `FVaRestError` (spec §3.3, a `USTRUCT` because M5 hands it to Blueprints), `FVaRestProgress`, `FVaRestResponse` and `TVaRestResult<T>`. `UVaRestHttpSettings` carries the project defaults M3 reads (decision 16). SHA-256 is ours because the engine's `FPlatformMisc::GetSHA256Signature` has no generic implementation — `GenericPlatformMisc.cpp:2019-2024` is `checkf(false, ...)` — and the redactor's binary descriptor (Task 5) and M4's cache key need it.

**Files:**
- Create: `Source/VaRestHttp/VaRestHttp.Build.cs`
- Create: `Source/VaRestHttp/Public/VaRestHttpTypes.h`, `Source/VaRestHttp/Private/VaRestHttpTypes.cpp`
- Create: `Source/VaRestHttp/Public/VaRestSha256.h`, `Source/VaRestHttp/Private/VaRestSha256.cpp`
- Create: `Source/VaRestHttp/Public/VaRestHttpSettings.h`, `Source/VaRestHttp/Private/VaRestHttpSettings.cpp`
- Create: `Source/VaRestHttp/Private/VaRestHttpModule.cpp`, `Source/VaRestHttp/Private/VaRestHttpLog.h`
- Modify: `VaRest.uplugin`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpModuleTest.cpp`

**Interfaces:**
- Consumes: `FVaRestJson`, `FVaRestJsonError` (VaRestJson).
- Produces: `EVaRestPriority { Highest, High, Normal, Low }`, `EVaRestLogLevel { None, Basic, Headers, Body }`, `EVaRestErrorKind { None, Canceled, Connection, Timeout, Http, Server, Decode, Auth, Offline, Config }`, `EVaRestQueryStyle { Repeat, Csv, Ssv, Pipes }` (UENUMs; the last for Task 3, decision 34); `EVaRestStageSlot { Outer, PerAttempt }`, `EVaRestTransportFailure { None, Connection, Timeout, Canceled, Other }`, `EVaRestCancelReason { None, User, OwnerGone, Shutdown }`; `FVaRestHeaders` (`Set`, `Add`, `Remove`, `Find`, `Get`, `Contains`, `Num`, `IsEmpty`, `GetPairs`, `begin/end`, `static FromLines(TConstArrayView<FString>)`); `FVaRestError` (USTRUCT: `Kind`, `HttpStatus`, `Code`, `Message`, `Body`, `RawBody`, `Path`, `Attempts`, `bRetryable`, `Tag`, `Url`; `IsError()`, `Describe()`, `static DefaultMessage(EVaRestErrorKind, int32)`, `static IsRetryableStatus(int32)`); `FVaRestProgress { BytesSent, BytesReceived, UploadTotal, DownloadTotal }`; `FVaRestResponse { RequestId, Status, Headers, EffectiveUrl, ContentType, Elapsed, Attempts, bFromCache, Body, SavedFile, DecodeWarnings, BytesUp, BytesDown }`; `TVaRestResult<T> { TOptional<T> Value; FVaRestError Error; FVaRestResponse Response; IsSuccess(); GetValue(); }` and `TVaRestResult<void>`; `LexToString(EVaRestErrorKind)`, `LexToString(EVaRestTransportFailure)`, `LexToString(EVaRestLogLevel)`; `VaRest::Http::IsJsonContentType(FStringView)`, `VaRest::Http::IsSuccessStatus(int32)`; `VaRest::Http::FSha256` (`Update`, `Finalize`, `static Hash`) and `FSha256Digest::ToHex()`; `UVaRestHttpSettings` (`LogLevel`, `RedactKeys`, `MaxConcurrent`, `Timeout`, `ActivityTimeout`, `int64 MaxJsonResponseBytes = 32 MiB`); log category `LogVaRestHttp` (private header).

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpModuleTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTypes.h"
#include "VaRestSha256.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpModuleLoadsTest, "VaRest.Http.Module.Loads",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpModuleLoadsTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("VaRestHttp is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestHttp")));
	const UVaRestHttpSettings* Settings = GetDefault<UVaRestHttpSettings>();
	TestTrue(TEXT("default log level is Basic (spec 4.5)"), Settings->LogLevel == EVaRestLogLevel::Basic);
	TestEqual(TEXT("default max concurrent is 16 (spec 4.6)"), Settings->MaxConcurrent, 16);
	TestEqual(TEXT("total timeout 0: the engine default"), Settings->Timeout, 0.f);
	TestEqual(TEXT("activity timeout 0: the engine default"), Settings->ActivityTimeout, 0.f);
	TestEqual(TEXT("JSON responses are capped at 32 MiB by default (controller ruling, M2 final review rec. 4)"), Settings->MaxJsonResponseBytes, 32ll * 1024 * 1024);
	TestEqual(TEXT("Project Settings > Plugins"), Settings->GetCategoryName(), FName(TEXT("Plugins")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpHeadersTest, "VaRest.Http.Types.Headers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpHeadersTest::RunTest(const FString& Parameters)
{
	FVaRestHeaders H;
	H.Set(TEXT("Content-Type"), TEXT("text/plain"));
	H.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	H.Add(TEXT("set-cookie"), TEXT("b=2"));
	H.Set(TEXT("X-Trace"), TEXT("1"));
	TestEqual(TEXT("repeated names are kept"), H.Num(), 4);
	TestEqualSensitive(TEXT("lookup ignores case"), H.Get(TEXT("CONTENT-TYPE")), FString(TEXT("text/plain")));
	TestEqualSensitive(TEXT("Find returns the first of a repeated name"), *H.Find(TEXT("Set-Cookie")), FString(TEXT("a=1")));

	H.Set(TEXT("SET-COOKIE"), TEXT("c=3"));
	TestEqual(TEXT("Set replaces every header of that name"), H.Num(), 3);
	TestEqualSensitive(TEXT("... at the first one's position"), H.GetPairs()[1].Value, FString(TEXT("c=3")));
	TestEqualSensitive(TEXT("... with the name as given"), H.GetPairs()[1].Key, FString(TEXT("SET-COOKIE")));

	TestTrue(TEXT("Remove reports a removal"), H.Remove(TEXT("x-trace")));
	TestFalse(TEXT("... and a miss"), H.Remove(TEXT("x-trace")));
	TestTrue(TEXT("Get on a miss is empty"), H.Get(TEXT("nope")).IsEmpty() && H.Find(TEXT("nope")) == nullptr);

	const TArray<FString> Lines = { TEXT("Content-Length: 12"), TEXT("ETag:  \"v1\" "), TEXT("garbage"), TEXT("Set-Cookie: a=1"), TEXT("Set-Cookie: b=2") };
	const FVaRestHeaders Parsed = FVaRestHeaders::FromLines(Lines);
	TestEqual(TEXT("engine lines parse; a line without ':' is skipped"), Parsed.Num(), 4);
	TestEqualSensitive(TEXT("values are trimmed"), Parsed.Get(TEXT("etag")), FString(TEXT("\"v1\"")));
	int32 Cookies = 0;
	for (const FVaRestHeaders::FPair& Pair : Parsed)
	{
		Cookies += FStringView(Pair.Key).Equals(TEXTVIEW("set-cookie"), ESearchCase::IgnoreCase) ? 1 : 0;
	}
	TestEqual(TEXT("both Set-Cookie lines survive"), Cookies, 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpErrorTypesTest, "VaRest.Http.Types.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpErrorTypesTest::RunTest(const FString& Parameters)
{
	const FString NotFound = FVaRestError::DefaultMessage(EVaRestErrorKind::Http, 404);
	TestTrue(TEXT("HTTP message names the status"), NotFound.Contains(TEXT("404")) && NotFound.Contains(TEXT("Not Found")));
	TestFalse(TEXT("every kind but None has a message"), FVaRestError::DefaultMessage(EVaRestErrorKind::Decode, 0).IsEmpty());

	for (int32 Status : { 408, 425, 429, 500, 502, 503, 504 })
	{
		TestTrue(FString::Printf(TEXT("%d is retryable (spec 4.1)"), Status), FVaRestError::IsRetryableStatus(Status));
	}
	for (int32 Status : { 200, 400, 401, 403, 404, 409, 501 })
	{
		TestFalse(FString::Printf(TEXT("%d is not retryable"), Status), FVaRestError::IsRetryableStatus(Status));
	}

	FVaRestError E;
	TestFalse(TEXT("a default error is no error"), E.IsError());
	E.Kind = EVaRestErrorKind::Decode;
	E.Message = TEXT("expected number");
	E.Path = TEXT("items[3].level");
	E.Url = TEXT("https://api.test/x?token=***");
	TestEqualSensitive(TEXT("Describe"), E.Describe(), FString(TEXT("Decode: expected number at items[3].level (https://api.test/x?token=***)")));
	E.Kind = EVaRestErrorKind::Http;
	E.HttpStatus = 503;
	E.Code = TEXT("E503");
	E.Path.Reset();
	E.Url.Reset();
	TestEqualSensitive(TEXT("Describe with status and code"), E.Describe(), FString(TEXT("Http 503 [E503]: expected number")));

	using VaRest::Http::IsJsonContentType;
	TestTrue(TEXT("application/json"), IsJsonContentType(TEXT("application/json")));
	TestTrue(TEXT("case and parameters ignored"), IsJsonContentType(TEXT(" Application/JSON ; charset=utf-8")));
	TestTrue(TEXT("+json (RFC 6839)"), IsJsonContentType(TEXT("application/problem+json")));
	TestFalse(TEXT("text/plain is not JSON"), IsJsonContentType(TEXT("text/plain")));
	TestFalse(TEXT("text/json is not a registered JSON type"), IsJsonContentType(TEXT("text/json")));
	TestFalse(TEXT("empty is unknown, not JSON"), IsJsonContentType(TEXT("")));
	TestTrue(TEXT("2xx"), VaRest::Http::IsSuccessStatus(204) && !VaRest::Http::IsSuccessStatus(304) && !VaRest::Http::IsSuccessStatus(199));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSha256Test, "VaRest.Http.Sha256.Vectors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSha256Test::RunTest(const FString& Parameters)
{
	using VaRest::Http::FSha256;
	auto Hex = [](const char* Text) -> FString
	{
		return FSha256::Hash(TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text))).ToHex();
	};
	// FIPS 180-2 appendix B and the NIST short-message vectors; the 56-byte one fills the first block past the padding point
	TestEqualSensitive(TEXT("empty"), Hex(""), FString(TEXT("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
	TestEqualSensitive(TEXT("abc"), Hex("abc"), FString(TEXT("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
	TestEqualSensitive(TEXT("448 bits"), Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		FString(TEXT("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
	TestEqualSensitive(TEXT("896 bits"), Hex("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu"),
		FString(TEXT("cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1")));

	TArray<uint8> Million;
	Million.Init('a', 1000000);
	TestEqualSensitive(TEXT("a million 'a'"), FSha256::Hash(Million).ToHex(), FString(TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));

	// Incremental updates in uneven pieces give the one-shot digest
	FSha256 Pieces;
	int64 Done = 0;
	int32 Step = 1;
	while (Done < Million.Num())
	{
		const int64 Take = FMath::Min<int64>(Step, Million.Num() - Done);
		Pieces.Update(Million.GetData() + Done, Take);
		Done += Take;
		Step = Step * 7 % 97 + 1;
	}
	TestEqualSensitive(TEXT("incremental"), Pieces.Finalize().ToHex(), FString(TEXT("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `run_tests: filter '^VaRest.Http' matched no tests` (exit 3) — the module does not exist yet, so nothing compiles or registers these tests.

- [ ] **Step 3: Build file and plugin descriptor**

`Source/VaRestHttp/VaRestHttp.Build.cs`:

```csharp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

public class VaRestHttp : ModuleRules
{
	public VaRestHttp(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",

			// PUBLIC: FVaRestError carries an FVaRestJson; bodies and results are FVaRestJson and structs (FVaRestJsonConvert)
			"VaRestJson",

			// PUBLIC: UVaRestHttpSettings derives from UDeveloperSettings (Engine/DeveloperSettings.h)
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			// The default IVaRestTransport is FHttpModule (spec 0.4: UE's HTTP module is the transport). No public
			// header names an engine HTTP type, so a game that only links VaRestHttp's API does not need HTTP's headers.
			"HTTP",
		});
	}
}
```

In `VaRest.uplugin`, insert after the `VaRestJson` module object (keep the file's tab indentation):

```json
		{
			"Name" : "VaRestHttp",
			"Type" : "Runtime",
			"LoadingPhase" : "PreDefault",
			"PlatformAllowList" : [ "Win64", "Mac", "Linux", "Android", "IOS" ]
		},
```

- [ ] **Step 4: Module, log category, settings**

`Source/VaRestHttp/Private/VaRestHttpLog.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Logging/LogMacros.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVaRestHttp, Log, All);
```

`Source/VaRestHttp/Private/VaRestHttpModule.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VaRestHttpLog.h"

DEFINE_LOG_CATEGORY(LogVaRestHttp);

class FVaRestHttpModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
	}

	virtual void ShutdownModule() override
	{
	}
};

IMPLEMENT_MODULE(FVaRestHttpModule, VaRestHttp);
```

`Source/VaRestHttp/Public/VaRestHttpSettings.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestHttpTypes.h"

#include "VaRestHttpSettings.generated.h"

/**
 * Project defaults for every FVaRestClient (spec 5.5), read when a client is made. M3 holds what M3 reads; the rest of
 * spec 5.5 (retry, naming, cache and queue directories) arrives with M5. v1's UVaRestSettings stays in module VaRest.
 */
UCLASS(config = Engine, defaultconfig, meta = (DisplayName = "VaRest HTTP"))
class VARESTHTTP_API UVaRestHttpSettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UVaRestHttpSettings(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

	/** Default log level of every client (spec 4.5); the cvar VaRest.LogLevel overrides it at runtime */
	UPROPERTY(config, EditAnywhere, Category = "Logging")
	EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;

	/**
	 * Extra keys whose values are redacted in logs and traces, on top of the built-in rules (Inspector IN-13). Matched
	 * exactly after normalisation: lower case, '_', '-' and '.' removed (so player_pin also covers playerPin). Read when a
	 * client is made (FVaRestClient::Make): an edit in the editor reaches clients made afterwards (review M20).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Logging")
	TArray<FString> RedactKeys;

	/** Requests one client runs at once; the rest wait in a priority queue (spec 4.6) */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "1"))
	int32 MaxConcurrent = 16;

	/** Seconds for a whole attempt; 0 = the engine's default ([HTTP] HttpTotalTimeout, 0 = none) */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "0"))
	float Timeout = 0.f;

	/**
	 * Seconds without any transfer before an attempt fails. 0 = the total timeout when there is one (decision 30: the
	 * engine's own 30 s idle default would otherwise fire first, as a connection error), else the engine's default
	 * ([HTTP] HttpActivityTimeout, 30).
	 */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "0"))
	float ActivityTimeout = 0.f;

	/**
	 * Largest response body parsed as JSON, in bytes; 0 = no limit. The arena DOM is about 4.2x its input (M2 final
	 * review), so this bounds a decode's memory: 32 MiB of JSON is about 135 MB of DOM. The HTTP transport stops a larger
	 * body as it arrives (Kind=Decode, nothing kept; an error response stays Kind=Http without its body); a transport that
	 * does not, fails it before anything is parsed (FVaRestJsonParseOptions::MaxInputBytes). Per client:
	 * FVaRestClient::WithMaxJsonBytes; per request: MaxJsonBytes().
	 */
	UPROPERTY(config, EditAnywhere, Category = "Requests", meta = (ClampMin = "0"))
	int64 MaxJsonResponseBytes = 32 * 1024 * 1024;
};
```

`Source/VaRestHttp/Private/VaRestHttpSettings.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpSettings.h"

UVaRestHttpSettings::UVaRestHttpSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// UDeveloperSettings reads these protected members (DeveloperSettings.h:63-66)
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("VaRestHttp");
}
```

- [ ] **Step 5: Core types** — `Source/VaRestHttp/Public/VaRestHttpTypes.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

#include "VaRestHttpTypes.generated.h"

/** Limiter queue order (spec 4.6): Highest first, FIFO within one priority */
UENUM(BlueprintType)
enum class EVaRestPriority : uint8
{
	Highest,
	High,
	Normal,
	Low
};

/** Spec 4.5 */
UENUM(BlueprintType)
enum class EVaRestLogLevel : uint8
{
	None,
	/** Verb, redacted URL, status, milliseconds, bytes */
	Basic,
	/** Basic plus the redacted request and response headers */
	Headers,
	/** Headers plus redacted body excerpts of up to 4 KiB; compiled out in Shipping, where it logs as Headers */
	Body
};

/** Spec 3.3 */
UENUM(BlueprintType)
enum class EVaRestErrorKind : uint8
{
	/** No error */
	None,
	Canceled,
	/** No response: the server could not be reached, the connection broke, or no data arrived for the activity timeout */
	Connection,
	/** The total timeout passed */
	Timeout,
	/** A non-2xx status */
	Http,
	/** 2xx, but the envelope says failure (M4) */
	Server,
	/** The response could not be turned into the result type */
	Decode,
	/** Refresh failed or no credentials (M4) */
	Auth,
	/** No connectivity, not queued, or a queued item expired (M4) */
	Offline,
	/** The request is invalid: a bad URL, path parameter, header or body */
	Config
};

/**
 * OpenAPI N9: how an array-valued query parameter is written (decision 34). Repeat: tag=a&tag=b; the others one pair,
 * the values joined before encoding: Csv "a,b", Ssv "a b", Pipes "a|b". A UENUM because M5's nodes take it.
 */
UENUM(BlueprintType)
enum class EVaRestQueryStyle : uint8
{
	Repeat,
	Csv,
	Ssv,
	Pipes
};

/** Where a custom stage goes (spec 4) */
enum class EVaRestStageSlot : uint8
{
	/** Sees each logical request once, before the attempt boundary */
	Outer,
	/** Sees every attempt and its final headers (request signing) */
	PerAttempt
};

/** Why an attempt got no usable response (Inspector IN-3 Failure) */
enum class EVaRestTransportFailure : uint8
{
	None,
	Connection,
	Timeout,
	Canceled,
	Other
};

/** Inspector IN-5 CancelReason */
enum class EVaRestCancelReason : uint8
{
	None,
	/** FVaRestRequestHandle::Cancel or FVaRestClient::CancelAll: the callbacks get Kind=Canceled */
	User,
	/** The request's owner was destroyed: no callbacks (spec 3.2) */
	OwnerGone,
	/** The engine is exiting: no callbacks */
	Shutdown
};

/**
 * HTTP headers in order. Names compare ignoring case (RFC 9110 5.1) and may repeat (Set-Cookie). Not thread-safe: copy
 * it to hand it to another thread.
 */
class VARESTHTTP_API FVaRestHeaders
{
public:
	using FPair = TPair<FString, FString>;

	/** One Name: Value, replacing every header of that name; it takes the first one's position */
	void Set(FStringView Name, FStringView Value);
	/** Another Name: Value after the existing ones */
	void Add(FStringView Name, FStringView Value);
	/** Removes every header of that name; false when there was none */
	bool Remove(FStringView Name);
	/** The first value of Name, or null */
	const FString* Find(FStringView Name) const;
	/** The first value of Name, or "" */
	FString Get(FStringView Name) const;
	bool Contains(FStringView Name) const { return Find(Name) != nullptr; }
	int32 Num() const { return Pairs.Num(); }
	bool IsEmpty() const { return Pairs.Num() == 0; }
	const TArray<FPair>& GetPairs() const { return Pairs; }
	auto begin() const { return Pairs.begin(); }
	auto end() const { return Pairs.end(); }

	/** Parses the engine's "Name: Value" lines (IHttpBase::GetAllHeaders, IHttpBase.h:140-145); a line without ':' is skipped */
	static FVaRestHeaders FromLines(TConstArrayView<FString> Lines);

private:
	TArray<FPair> Pairs;
};

/** Spec 3.3. A USTRUCT because M5 hands it to Blueprints. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	EVaRestErrorKind Kind = EVaRestErrorKind::None;

	/** 0 when there was no response */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int32 HttpStatus = 0;

	/** From the envelope (M4) or the transport; may be empty */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Code;

	/** Human-readable; never empty for an error */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Message;

	/** The parsed error body when it is JSON, else an invalid handle */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FVaRestJson Body;

	/** The first 4 KiB of a body that is not JSON, as text */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString RawBody;

	/** Decode errors: the JSON path of the value that did not fit ("items[3].level") */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int32 Attempts = 0;

	/** A connection error, a timeout, or status 408/425/429/500/502/503/504 (spec 4.1) */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	bool bRetryable = false;

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FName Tag;

	/** The request URL, redacted */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Url;

	bool IsError() const { return Kind != EVaRestErrorKind::None; }

	/** "Http 503 [E503]: message at path (url)", the parts that are set */
	FString Describe() const;

	/** The human-readable fallback for a kind ("HTTP 404 Not Found" for Http) */
	static FString DefaultMessage(EVaRestErrorKind ForKind, int32 Status);

	/** Spec 4.1's retryable statuses: 408, 425, 429, 500, 502, 503, 504 */
	static bool IsRetryableStatus(int32 Status);
};

/** Decision 8: one total per direction */
struct FVaRestProgress
{
	int64 BytesSent = 0;
	int64 BytesReceived = 0;
	/** The request body's size; 0 without one */
	int64 UploadTotal = 0;
	/** The response's Content-Length; -1 when it is unknown (chunked) */
	int64 DownloadTotal = -1;

	bool operator==(const FVaRestProgress& Other) const = default;
};

/** Spec 3.3: the metadata of a response, and its raw bytes per spec 3.2's release rule */
struct FVaRestResponse
{
	uint64 RequestId = 0;
	/** 0 when there was no response */
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** Seconds from entering the pipeline to the result */
	double Elapsed = 0.0;
	int32 Attempts = 0;
	bool bFromCache = false;
	/**
	 * Raw bytes (spec 3.2's release rule, decision 26): kept for raw results, with KeepBody(), and on an error that left
	 * them intact (an HTTP error, a non-JSON Content-Type, a body over MaxJsonBytes refused before parsing). A JSON body
	 * is parsed in place (zero copy), so after a syntax error or a struct mismatch the bytes are gone: the error keeps
	 * the first 4 KiB as RawBody (copied before the parse) and, for a mismatch, the parsed DOM as Body. KeepBody() keeps
	 * all of it (a copy before the parse). Released after a successful decode.
	 */
	TArray<uint8> Body;
	/** SaveTo(): where the body was written */
	FString SavedFile;
	/** What a tolerant struct decode noticed, each with its JSON path (spec 2.4) */
	TArray<FVaRestJsonError> DecodeWarnings;
	int64 BytesUp = 0;
	int64 BytesDown = 0;
};

/** Spec 3.3: a value or an error, plus the response */
template <typename T>
struct TVaRestResult
{
	TOptional<T> Value;
	FVaRestError Error;
	FVaRestResponse Response;

	bool IsSuccess() const { return !Error.IsError(); }
	const T& GetValue() const { return Value.GetValue(); }
};

template <>
struct TVaRestResult<void>
{
	FVaRestError Error;
	FVaRestResponse Response;

	bool IsSuccess() const { return !Error.IsError(); }
};

VARESTHTTP_API const TCHAR* LexToString(EVaRestErrorKind Kind);
VARESTHTTP_API const TCHAR* LexToString(EVaRestTransportFailure Failure);
VARESTHTTP_API const TCHAR* LexToString(EVaRestLogLevel Level);

namespace VaRest::Http
{
	/** application/json, or any type ending in +json (RFC 6839); case, spaces and parameters ignored (spec 3.2) */
	VARESTHTTP_API bool IsJsonContentType(FStringView ContentType);

	inline bool IsSuccessStatus(int32 Status)
	{
		return Status >= 200 && Status <= 299;
	}
}
```

`Source/VaRestHttp/Private/VaRestHttpTypes.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpTypes.h"

#include "Interfaces/IHttpResponse.h"
#include "Misc/StringBuilder.h"

void FVaRestHeaders::Set(FStringView Name, FStringView Value)
{
	int32 First = INDEX_NONE;
	for (int32 i = 0; i < Pairs.Num();)
	{
		if (FStringView(Pairs[i].Key).Equals(Name, ESearchCase::IgnoreCase))
		{
			if (First == INDEX_NONE)
			{
				First = i++;
			}
			else
			{
				Pairs.RemoveAt(i);
			}
			continue;
		}
		++i;
	}
	if (First == INDEX_NONE)
	{
		Pairs.Emplace(FString(Name), FString(Value));
	}
	else
	{
		Pairs[First].Key = FString(Name);
		Pairs[First].Value = FString(Value);
	}
}

void FVaRestHeaders::Add(FStringView Name, FStringView Value)
{
	Pairs.Emplace(FString(Name), FString(Value));
}

bool FVaRestHeaders::Remove(FStringView Name)
{
	return Pairs.RemoveAll([Name](const FPair& Pair) { return FStringView(Pair.Key).Equals(Name, ESearchCase::IgnoreCase); }) > 0;
}

const FString* FVaRestHeaders::Find(FStringView Name) const
{
	for (const FPair& Pair : Pairs)
	{
		if (FStringView(Pair.Key).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &Pair.Value;
		}
	}
	return nullptr;
}

FString FVaRestHeaders::Get(FStringView Name) const
{
	const FString* Value = Find(Name);
	return Value ? *Value : FString();
}

FVaRestHeaders FVaRestHeaders::FromLines(TConstArrayView<FString> Lines)
{
	FVaRestHeaders Out;
	for (const FString& Line : Lines)
	{
		int32 Colon = INDEX_NONE;
		if (!Line.FindChar(TEXT(':'), Colon))
		{
			continue;
		}
		const FStringView View(Line);
		Out.Add(View.Left(Colon).TrimStartAndEnd(), View.RightChop(Colon + 1).TrimStartAndEnd());
	}
	return Out;
}

FString FVaRestError::Describe() const
{
	if (!IsError())
	{
		return TEXT("no error");
	}
	TStringBuilder<512> Text;
	Text << LexToString(Kind);
	if (HttpStatus > 0)
	{
		Text << TEXT(" ") << HttpStatus;
	}
	if (!Code.IsEmpty())
	{
		Text << TEXT(" [") << Code << TEXT("]");
	}
	Text << TEXT(": ") << Message;
	if (!Path.IsEmpty())
	{
		Text << TEXT(" at ") << Path;
	}
	if (!Url.IsEmpty())
	{
		Text << TEXT(" (") << Url << TEXT(")");
	}
	return FString(Text.ToView());
}

FString FVaRestError::DefaultMessage(EVaRestErrorKind ForKind, int32 Status)
{
	switch (ForKind)
	{
	case EVaRestErrorKind::None:
		return FString();
	case EVaRestErrorKind::Canceled:
		return TEXT("the request was canceled");
	case EVaRestErrorKind::Connection:
		return TEXT("the server could not be reached");
	case EVaRestErrorKind::Timeout:
		return TEXT("the request timed out");
	case EVaRestErrorKind::Http:
		// The engine's reason phrases (IHttpResponse.h:52-104); an unlisted status reads "Unknown"
		return FString::Printf(TEXT("HTTP %d %s"), Status, *EHttpResponseCodes::GetDescription(static_cast<EHttpResponseCodes::Type>(Status)).ToString());
	case EVaRestErrorKind::Server:
		return TEXT("the server reported a failure");
	case EVaRestErrorKind::Decode:
		return TEXT("the response could not be decoded");
	case EVaRestErrorKind::Auth:
		return TEXT("authentication failed");
	case EVaRestErrorKind::Offline:
		return TEXT("offline");
	case EVaRestErrorKind::Config:
		return TEXT("the request is invalid");
	}
	return FString();
}

bool FVaRestError::IsRetryableStatus(int32 Status)
{
	switch (Status)
	{
	case 408:
	case 425:
	case 429:
	case 500:
	case 502:
	case 503:
	case 504:
		return true;
	default:
		return false;
	}
}

const TCHAR* LexToString(EVaRestErrorKind Kind)
{
	switch (Kind)
	{
	case EVaRestErrorKind::None: return TEXT("None");
	case EVaRestErrorKind::Canceled: return TEXT("Canceled");
	case EVaRestErrorKind::Connection: return TEXT("Connection");
	case EVaRestErrorKind::Timeout: return TEXT("Timeout");
	case EVaRestErrorKind::Http: return TEXT("Http");
	case EVaRestErrorKind::Server: return TEXT("Server");
	case EVaRestErrorKind::Decode: return TEXT("Decode");
	case EVaRestErrorKind::Auth: return TEXT("Auth");
	case EVaRestErrorKind::Offline: return TEXT("Offline");
	case EVaRestErrorKind::Config: return TEXT("Config");
	}
	return TEXT("Unknown");
}

const TCHAR* LexToString(EVaRestTransportFailure Failure)
{
	switch (Failure)
	{
	case EVaRestTransportFailure::None: return TEXT("None");
	case EVaRestTransportFailure::Connection: return TEXT("Connection");
	case EVaRestTransportFailure::Timeout: return TEXT("Timeout");
	case EVaRestTransportFailure::Canceled: return TEXT("Canceled");
	case EVaRestTransportFailure::Other: return TEXT("Other");
	}
	return TEXT("Unknown");
}

const TCHAR* LexToString(EVaRestLogLevel Level)
{
	switch (Level)
	{
	case EVaRestLogLevel::None: return TEXT("None");
	case EVaRestLogLevel::Basic: return TEXT("Basic");
	case EVaRestLogLevel::Headers: return TEXT("Headers");
	case EVaRestLogLevel::Body: return TEXT("Body");
	}
	return TEXT("Unknown");
}

bool VaRest::Http::IsJsonContentType(FStringView ContentType)
{
	int32 Semicolon = INDEX_NONE;
	FStringView Type = ContentType.FindChar(TEXT(';'), Semicolon) ? ContentType.Left(Semicolon) : ContentType;
	Type = Type.TrimStartAndEnd();
	return Type.Equals(TEXTVIEW("application/json"), ESearchCase::IgnoreCase) || Type.EndsWith(TEXTVIEW("+json"), ESearchCase::IgnoreCase);
}
```

- [ ] **Step 6: SHA-256** — `Source/VaRestHttp/Public/VaRestSha256.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

namespace VaRest::Http
{
	struct FSha256Digest
	{
		uint8 Bytes[32] = {};

		/** 64 lower-case hex digits */
		VARESTHTTP_API FString ToHex() const;
	};

	/**
	 * SHA-256 (FIPS 180-4). Ours because FPlatformMisc::GetSHA256Signature has no generic implementation
	 * (GenericPlatformMisc.cpp:2019-2024 is checkf(false)). Any thread; one instance is not shared between threads.
	 */
	class VARESTHTTP_API FSha256
	{
	public:
		FSha256();
		void Update(const void* Data, int64 Size);
		/** The digest; the object is spent afterwards */
		FSha256Digest Finalize();

		static FSha256Digest Hash(TConstArrayView<uint8> Data);

	private:
		void Transform(const uint8* Block);

		uint32 Words[8];
		uint64 TotalBytes = 0;
		uint8 Buffer[64];
		int32 BufferLen = 0;
	};
}
```

`Source/VaRestHttp/Private/VaRestSha256.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSha256.h"

namespace VaRest::Http::Sha256Private
{
	/** FIPS 180-4 4.2.2: the first 32 bits of the fractional parts of the cube roots of the first 64 primes */
	const uint32 RoundConstants[64] = {
		0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2 };

	inline uint32 RotR(uint32 Value, uint32 Bits)
	{
		return (Value >> Bits) | (Value << (32 - Bits));
	}
}

FString VaRest::Http::FSha256Digest::ToHex() const
{
	static const TCHAR Digits[] = TEXT("0123456789abcdef");
	FString Out;
	Out.Reserve(64);
	for (const uint8 Byte : Bytes)
	{
		Out.AppendChar(Digits[Byte >> 4]);
		Out.AppendChar(Digits[Byte & 15]);
	}
	return Out;
}

VaRest::Http::FSha256::FSha256()
{
	// FIPS 180-4 5.3.3
	Words[0] = 0x6a09e667;
	Words[1] = 0xbb67ae85;
	Words[2] = 0x3c6ef372;
	Words[3] = 0xa54ff53a;
	Words[4] = 0x510e527f;
	Words[5] = 0x9b05688c;
	Words[6] = 0x1f83d9ab;
	Words[7] = 0x5be0cd19;
}

void VaRest::Http::FSha256::Update(const void* Data, int64 Size)
{
	const uint8* In = static_cast<const uint8*>(Data);
	TotalBytes += static_cast<uint64>(Size);
	while (Size > 0)
	{
		const int32 Take = static_cast<int32>(FMath::Min<int64>(64 - BufferLen, Size));
		FMemory::Memcpy(Buffer + BufferLen, In, Take);
		BufferLen += Take;
		In += Take;
		Size -= Take;
		if (BufferLen == 64)
		{
			Transform(Buffer);
			BufferLen = 0;
		}
	}
}

VaRest::Http::FSha256Digest VaRest::Http::FSha256::Finalize()
{
	// FIPS 180-4 5.1.1: 0x80, zeros to 56 mod 64, the bit length big-endian
	const uint64 BitLength = TotalBytes * 8;
	Buffer[BufferLen++] = 0x80;
	if (BufferLen > 56)
	{
		FMemory::Memzero(Buffer + BufferLen, 64 - BufferLen);
		Transform(Buffer);
		BufferLen = 0;
	}
	FMemory::Memzero(Buffer + BufferLen, 56 - BufferLen);
	for (int32 i = 0; i < 8; ++i)
	{
		Buffer[56 + i] = static_cast<uint8>(BitLength >> (56 - 8 * i));
	}
	Transform(Buffer);
	BufferLen = 0;

	FSha256Digest Digest;
	for (int32 i = 0; i < 8; ++i)
	{
		Digest.Bytes[4 * i + 0] = static_cast<uint8>(Words[i] >> 24);
		Digest.Bytes[4 * i + 1] = static_cast<uint8>(Words[i] >> 16);
		Digest.Bytes[4 * i + 2] = static_cast<uint8>(Words[i] >> 8);
		Digest.Bytes[4 * i + 3] = static_cast<uint8>(Words[i]);
	}
	return Digest;
}

VaRest::Http::FSha256Digest VaRest::Http::FSha256::Hash(TConstArrayView<uint8> Data)
{
	FSha256 Hasher;
	Hasher.Update(Data.GetData(), Data.Num());
	return Hasher.Finalize();
}

void VaRest::Http::FSha256::Transform(const uint8* Block)
{
	using namespace Sha256Private;
	uint32 W[64];
	for (int32 t = 0; t < 16; ++t)
	{
		W[t] = (static_cast<uint32>(Block[4 * t]) << 24) | (static_cast<uint32>(Block[4 * t + 1]) << 16)
			| (static_cast<uint32>(Block[4 * t + 2]) << 8) | static_cast<uint32>(Block[4 * t + 3]);
	}
	for (int32 t = 16; t < 64; ++t)
	{
		const uint32 S0 = RotR(W[t - 15], 7) ^ RotR(W[t - 15], 18) ^ (W[t - 15] >> 3);
		const uint32 S1 = RotR(W[t - 2], 17) ^ RotR(W[t - 2], 19) ^ (W[t - 2] >> 10);
		W[t] = W[t - 16] + S0 + W[t - 7] + S1;
	}
	uint32 A = Words[0], B = Words[1], C = Words[2], D = Words[3], E = Words[4], F = Words[5], G = Words[6], H = Words[7];
	for (int32 t = 0; t < 64; ++t)
	{
		const uint32 Sigma1 = RotR(E, 6) ^ RotR(E, 11) ^ RotR(E, 25);
		const uint32 Choose = (E & F) ^ (~E & G);
		const uint32 T1 = H + Sigma1 + Choose + RoundConstants[t] + W[t];
		const uint32 Sigma0 = RotR(A, 2) ^ RotR(A, 13) ^ RotR(A, 22);
		const uint32 Majority = (A & B) ^ (A & C) ^ (B & C);
		const uint32 T2 = Sigma0 + Majority;
		H = G;
		G = F;
		F = E;
		E = D + T1;
		D = C;
		C = B;
		B = A;
		A = T1 + T2;
	}
	Words[0] += A;
	Words[1] += B;
	Words[2] += C;
	Words[3] += D;
	Words[4] += E;
	Words[5] += F;
	Words[6] += G;
	Words[7] += H;
}
```

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`: `VaRest.Http.Module.Loads`, `VaRest.Http.Types.Headers`, `VaRest.Http.Types.Errors`, `VaRest.Http.Sha256.Vectors`. Then `Tools/run_tests.sh --no-build VaRest` stays green.

- [ ] **Step 8: Commit**

```bash
git add VaRest.uplugin Source/VaRestHttp
git commit -m "http: module VaRestHttp -- core types, FVaRestError, FVaRestHeaders, settings, SHA-256

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 3: Request encoding — URL join, path templates, query, header checks, forms, multipart

Spec §3.2: "base URL + path; an absolute URL in `Path` overrides the base. Whitespace trimmed (v1's iOS fix). RFC 3986 encoding for path segments and query values." OpenAPI N14 pins the join: "concatenation with exactly one `/` (`https://h/v1` + `/pets` → `https://h/v1/pets`), never RFC 3986 resolution", and N9's builder half: "repeated query keys append". Bodies: "`FVaRestForm` → `application/x-www-form-urlencoded`; `FVaRestMultipart` → `multipart/form-data`, file parts streamed with `SetContentFromStream`". Everything here is a pure function or a value type, so it is tested without a client.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestUrl.h`, `Source/VaRestHttp/Private/VaRestUrl.cpp`
- Create: `Source/VaRestHttp/Public/VaRestBody.h`, `Source/VaRestHttp/Private/VaRestBody.cpp`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpUrlTest.cpp`

**Interfaces:**
- Consumes: `VaRest::Json::ConvertForDocument`, `AppendRepairedUtf8`, `FUtf8Scratch` (`VaRestJsonUtf8.h`); `LogVaRestHttp`; `EVaRestQueryStyle` (Task 2).
- Produces (`VaRestUrl.h`, namespace `VaRest::Http`, all `VARESTHTTP_API`): `FString PercentEncode(FStringView)`; `FString FormEncode(FStringView)`; `FString PercentDecode(FStringView, bool bPlusIsSpace = false)`; `bool IsAbsoluteHttpUrl(FStringView)`; `FString JoinUrl(FStringView BaseUrl, FStringView Path)`; `TValueOrError<FString, FString> ExpandPathTemplate(FStringView Template, TConstArrayView<TPair<FString, FString>> Params)`; `void AppendQuery(FString& Url, TConstArrayView<TPair<FString, FString>> Query)`; `void AddQueryValues(TArray<TPair<FString, FString>>& Query, FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style)` (decision 34); `bool IsValidHeaderName(FStringView)`; `bool IsValidHeaderValue(FStringView)`. `JoinUrl` keeps a base URL's query (decision 33); `FromMultipart` refuses an invalid boundary (review I3).
- Produces (`VaRestBody.h`): `FVaRestForm` (`Add(Name, Value)` `&`/`&&`, `GetFields()`, `TArray<uint8> Encode() const`); `struct FVaRestMultipartPart { FString Name, FileName, ContentType; TArray<uint8> Bytes; FString FilePath; int64 Size; bool bField; }`; `FVaRestMultipart` (default ctor: random boundary; `explicit FVaRestMultipart(FStringView Boundary)`; `AddField`, `AddBytes`, `AddFile` `&`/`&&`; `GetBoundary`, `GetParts`, `GetContentType`, `HasFiles`, `GetError`; `TArray<FVaRestBodySegment> Encode() const`); `struct FVaRestBodySegment { TArray<uint8> Bytes; FString FilePath; int64 FileSize; int64 Size() const; }`; `class FVaRestSegmentStream final : public FArchive` (`explicit FVaRestSegmentStream(TArray<FVaRestBodySegment>)`); `enum class EVaRestBodyKind : uint8 { None, Json, Text, Form, Multipart, Binary, Stream }`; `struct FVaRestRequestBody { EVaRestBodyKind Kind; FString ContentType; TArray<uint8> Bytes; FString FilePath; TArray<FVaRestBodySegment> Segments; TArray<FVaRestMultipartPart> PartDescriptors; int64 ContentLength; IsEmpty(); IsStreamed(); static FromBytes(TArray<uint8>&&, EVaRestBodyKind, FStringView ContentType); static FromForm(const FVaRestForm&); static TValueOrError<FVaRestRequestBody, FString> FromMultipart(const FVaRestMultipart&); static TValueOrError<FVaRestRequestBody, FString> FromFile(FStringView FilePath, FStringView ContentType); }`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpUrlTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestBody.h"
#include "VaRestUrl.h"

namespace VaRest::Http::UrlTestPrivate
{
	FString AsText(TConstArrayView<uint8> Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	}

	TArray<uint8> Utf8(FStringView Text)
	{
		FTCHARToUTF8 Converted(Text.GetData(), Text.Len());
		return TArray<uint8>(reinterpret_cast<const uint8*>(Converted.Get()), Converted.Length());
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPercentEncodeTest, "VaRest.Http.Url.PercentEncode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPercentEncodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TestEqualSensitive(TEXT("RFC 3986 unreserved characters are kept"), PercentEncode(TEXT("AZaz09-._~")), FString(TEXT("AZaz09-._~")));
	TestEqualSensitive(TEXT("everything else is %XX of its UTF-8"), PercentEncode(TEXT("a b/c?d#e&f=g+h")), FString(TEXT("a%20b%2Fc%3Fd%23e%26f%3Dg%2Bh")));
	TestEqualSensitive(TEXT("two-byte UTF-8"), PercentEncode(TEXT("é")), FString(TEXT("%C3%A9")));
	TestEqualSensitive(TEXT("three-byte UTF-8"), PercentEncode(TEXT("日本")), FString(TEXT("%E6%97%A5%E6%9C%AC")));
	TestEqualSensitive(TEXT("a surrogate pair is one four-byte character"), PercentEncode(TEXT("\U0001F600")), FString(TEXT("%F0%9F%98%80")));
	TestEqualSensitive(TEXT("form encoding: space as '+', '*' kept, '~' encoded (WHATWG)"), FormEncode(TEXT("a b*~")), FString(TEXT("a+b*%7E")));

	TestEqualSensitive(TEXT("decode round trip"), PercentDecode(PercentEncode(TEXT("x y/é"))), FString(TEXT("x y/é")));
	TestEqualSensitive(TEXT("an invalid escape is kept as text"), PercentDecode(TEXT("%zz%4")), FString(TEXT("%zz%4")));
	TestEqualSensitive(TEXT("'+' stays '+' in a URL"), PercentDecode(TEXT("a+b")), FString(TEXT("a+b")));
	TestEqualSensitive(TEXT("... and is a space in a form"), PercentDecode(TEXT("a+b"), true), FString(TEXT("a b")));
	TestEqualSensitive(TEXT("decoded bytes that are not UTF-8 become U+FFFD"), PercentDecode(TEXT("%C3(")), FString(TEXT("�(")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpJoinTest, "VaRest.Http.Url.Join",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpJoinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	struct FCase
	{
		const TCHAR* Base;
		const TCHAR* Path;
		const TCHAR* Expected;
	};
	// OpenAPI N14: exactly one '/' at the join, never RFC 3986 resolution (which would drop /v1 for "/pets")
	const FCase Cases[] = {
		{ TEXT("https://h/v1"), TEXT("/pets"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1/"), TEXT("pets"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1//"), TEXT("//pets"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1/"), TEXT("/pets/"), TEXT("https://h/v1/pets/") },
		{ TEXT("https://h"), TEXT("/"), TEXT("https://h/") },
		{ TEXT("https://h/v1"), TEXT(""), TEXT("https://h/v1") },
		{ TEXT("https://h/v1"), TEXT("https://other/x"), TEXT("https://other/x") },
		{ TEXT("https://h/v1"), TEXT("HTTP://other/x"), TEXT("HTTP://other/x") },
		{ TEXT("  https://h/v1 \t"), TEXT("  /pets \n"), TEXT("https://h/v1/pets") },
		{ TEXT("https://h/v1"), TEXT("?a=1"), TEXT("https://h/v1?a=1") },
		{ TEXT(""), TEXT("/pets"), TEXT("/pets") },
		// Decision 33 (review M22): a base URL's query stays, first
		{ TEXT("https://h/v1?key=k"), TEXT("/pets"), TEXT("https://h/v1/pets?key=k") },
		{ TEXT("https://h/v1/?key=k"), TEXT("/pets?limit=2"), TEXT("https://h/v1/pets?key=k&limit=2") },
		{ TEXT("https://h/v1?key=k"), TEXT(""), TEXT("https://h/v1?key=k") },
		{ TEXT("https://h/v1?key=k"), TEXT("?a=1"), TEXT("https://h/v1?key=k&a=1") },
		{ TEXT("https://h/v1?key=k"), TEXT("/p#top"), TEXT("https://h/v1/p?key=k#top") },
	};
	for (const FCase& Case : Cases)
	{
		TestEqualSensitive(FString::Printf(TEXT("'%s' + '%s'"), Case.Base, Case.Path), JoinUrl(Case.Base, Case.Path), FString(Case.Expected));
	}
	TestTrue(TEXT("http://h is absolute"), IsAbsoluteHttpUrl(TEXT("http://h")));
	TestTrue(TEXT("... in any case"), IsAbsoluteHttpUrl(TEXT("HttpS://h/x")));
	TestFalse(TEXT("a scheme without a host is not"), IsAbsoluteHttpUrl(TEXT("https://")));
	TestFalse(TEXT("... nor an empty host"), IsAbsoluteHttpUrl(TEXT("https:///x")));
	TestFalse(TEXT("other schemes are not http(s)"), IsAbsoluteHttpUrl(TEXT("ftp://h/x")));
	TestFalse(TEXT("a path is not"), IsAbsoluteHttpUrl(TEXT("/users/7")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPathTemplateTest, "VaRest.Http.Url.PathTemplate",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPathTemplateTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using FParams = TArray<TPair<FString, FString>>;

	const FParams Params = { { TEXT("id"), TEXT("a/b?c#d e") }, { TEXT("item"), TEXT("日本") } };
	const TValueOrError<FString, FString> Expanded = ExpandPathTemplate(TEXT("/users/{id}/items/{item}"), Params);
	TestTrue(TEXT("expands"), Expanded.HasValue());
	TestEqualSensitive(TEXT("each value is one percent-encoded segment"), Expanded.GetValue(),
		FString(TEXT("/users/a%2Fb%3Fc%23d%20e/items/%E6%97%A5%E6%9C%AC")));

	const FParams Twice = { { TEXT("id"), TEXT("1") }, { TEXT("id"), TEXT("2") } };
	TestEqualSensitive(TEXT("a placeholder used twice gets the value twice; the last Path() wins"),
		ExpandPathTemplate(TEXT("/{id}/x/{id}"), Twice).GetValue(), FString(TEXT("/2/x/2")));
	TestEqualSensitive(TEXT("no placeholders, no parameters"), ExpandPathTemplate(TEXT("/plain"), FParams()).GetValue(), FString(TEXT("/plain")));

	// Decision 10: every misuse is a Config error, never a silent "/users/"
	const FParams Empty = { { TEXT("id"), TEXT("") } };
	const FParams Unknown = { { TEXT("id"), TEXT("1") }, { TEXT("idd"), TEXT("2") } };
	TestTrue(TEXT("a missing parameter is an error"), ExpandPathTemplate(TEXT("/users/{id}"), FParams()).GetError().Contains(TEXT("{id}")));
	TestTrue(TEXT("an empty parameter is an error"), ExpandPathTemplate(TEXT("/users/{id}"), Empty).GetError().Contains(TEXT("empty")));
	TestTrue(TEXT("a parameter the template lacks is an error (a typo)"), ExpandPathTemplate(TEXT("/users/{id}"), Unknown).GetError().Contains(TEXT("idd")));
	TestTrue(TEXT("an unclosed '{' is an error"), ExpandPathTemplate(TEXT("/users/{id"), FParams()).HasError());
	TestTrue(TEXT("a stray '}' is an error"), ExpandPathTemplate(TEXT("/users/id}"), FParams()).HasError());
	TestTrue(TEXT("'{}' is an error"), ExpandPathTemplate(TEXT("/users/{}"), FParams()).HasError());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpQueryTest, "VaRest.Http.Url.Query",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpQueryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	using FPairs = TArray<TPair<FString, FString>>;
	const FPairs Query = { { TEXT("a"), TEXT("1") }, { TEXT("b"), TEXT("x y&z") }, { TEXT("a"), TEXT("2") } };

	FString Url = TEXT("https://h/x");
	AppendQuery(Url, Query);
	TestEqualSensitive(TEXT("in order, repeats kept (OpenAPI N9), keys and values encoded"), Url, FString(TEXT("https://h/x?a=1&b=x%20y%26z&a=2")));

	Url = TEXT("https://h/x?k=v");
	AppendQuery(Url, FPairs{ { TEXT("a"), TEXT("1") } });
	TestEqualSensitive(TEXT("an existing query continues with '&'"), Url, FString(TEXT("https://h/x?k=v&a=1")));

	Url = TEXT("https://h/x?");
	AppendQuery(Url, FPairs{ { TEXT("a"), TEXT("1") } });
	TestEqualSensitive(TEXT("a trailing '?' is not doubled"), Url, FString(TEXT("https://h/x?a=1")));

	Url = TEXT("https://h/x#frag");
	AppendQuery(Url, FPairs{ { TEXT("a"), TEXT("1") } });
	TestEqualSensitive(TEXT("the query goes before the fragment"), Url, FString(TEXT("https://h/x?a=1#frag")));

	// OpenAPI N9's array styles (review I7, decision 34)
	FPairs Styled;
	const TArray<FString> Tags = { TEXT("a b"), TEXT("c") };
	AddQueryValues(Styled, TEXT("tag"), Tags, EVaRestQueryStyle::Repeat);
	AddQueryValues(Styled, TEXT("csv"), Tags, EVaRestQueryStyle::Csv);
	AddQueryValues(Styled, TEXT("ssv"), Tags, EVaRestQueryStyle::Ssv);
	AddQueryValues(Styled, TEXT("pipes"), Tags, EVaRestQueryStyle::Pipes);
	AddQueryValues(Styled, TEXT("none"), TArray<FString>(), EVaRestQueryStyle::Csv);
	Url = TEXT("https://h/x");
	AppendQuery(Url, Styled);
	TestEqualSensitive(TEXT("Repeat, Csv, Ssv and Pipes; joined before encoding; no values, no pair"), Url,
		FString(TEXT("https://h/x?tag=a%20b&tag=c&csv=a%20b%2Cc&ssv=a%20b%20c&pipes=a%20b%7Cc")));

	Url = TEXT("https://h/x");
	AppendQuery(Url, FPairs());
	TestEqualSensitive(TEXT("no query, no '?'"), Url, FString(TEXT("https://h/x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpHeaderChecksTest, "VaRest.Http.Url.HeaderChecks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpHeaderChecksTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http;
	TestTrue(TEXT("token names"), IsValidHeaderName(TEXT("X-Api-Key")) && IsValidHeaderName(TEXT("!#$%&'*+-.^_`|~09az")));
	TestFalse(TEXT("empty name"), IsValidHeaderName(TEXT("")));
	TestFalse(TEXT("space in a name"), IsValidHeaderName(TEXT("Bad Name")));
	TestFalse(TEXT("colon in a name"), IsValidHeaderName(TEXT("Bad:Name")));
	TestFalse(TEXT("CRLF in a name"), IsValidHeaderName(TEXT("Bad\r\nName")));
	TestFalse(TEXT("non-ASCII name"), IsValidHeaderName(TEXT("ключ")));
	TestTrue(TEXT("ordinary values"), IsValidHeaderValue(TEXT("Bearer abc.def; q=1, é")));
	TestFalse(TEXT("CR in a value"), IsValidHeaderValue(TEXT("a\rb")));
	TestFalse(TEXT("LF in a value (header injection)"), IsValidHeaderValue(TEXT("a\nX-Evil: 1")));
	const TCHAR WithNul[] = { TEXT('a'), TEXT('\0'), TEXT('b') };
	TestFalse(TEXT("NUL in a value"), IsValidHeaderValue(FStringView(WithNul, 3)));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFormTest, "VaRest.Http.Body.Form",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFormTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::UrlTestPrivate;
	const FVaRestForm Form = FVaRestForm().Add(TEXT("name"), TEXT("Neo Anderson")).Add(TEXT("q"), TEXT("a&b=c")).Add(TEXT("к"), TEXT("é"));
	TestEqualSensitive(TEXT("urlencoded pairs"), AsText(Form.Encode()), FString(TEXT("name=Neo+Anderson&q=a%26b%3Dc&%D0%BA=%C3%A9")));

	const FVaRestRequestBody Body = FVaRestRequestBody::FromForm(Form);
	TestTrue(TEXT("form body kind"), Body.Kind == EVaRestBodyKind::Form);
	TestEqualSensitive(TEXT("form content type"), Body.ContentType, FString(TEXT("application/x-www-form-urlencoded")));
	TestEqual(TEXT("content length"), Body.ContentLength, static_cast<int64>(Body.Bytes.Num()));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMultipartTest, "VaRest.Http.Body.Multipart",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMultipartTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::UrlTestPrivate;
	FVaRestMultipart Multipart(TEXT("XyZ"));
	Multipart.AddField(TEXT("title"), TEXT("hi \"there\"")).AddBytes(TEXT("blob"), TEXT("a.bin"), TEXT("application/octet-stream"), { 1, 2, 3 });
	Multipart.AddField(TEXT("we\"ird\r\n"), TEXT("v"));
	TestFalse(TEXT("no file parts"), Multipart.HasFiles());
	TestEqualSensitive(TEXT("content type names the boundary"), Multipart.GetContentType(), FString(TEXT("multipart/form-data; boundary=XyZ")));

	const TValueOrError<FVaRestRequestBody, FString> Body = FVaRestRequestBody::FromMultipart(Multipart);
	TestTrue(TEXT("builds"), Body.HasValue());
	TArray<uint8> Expected = Utf8(TEXT("--XyZ\r\nContent-Disposition: form-data; name=\"title\"\r\n\r\nhi \"there\"\r\n")
		TEXT("--XyZ\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"a.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n"));
	Expected.Append({ 1, 2, 3 });
	// WHATWG multipart/form-data: '"', CR and LF in a name are percent-escaped, so a name cannot end the header
	Expected.Append(Utf8(TEXT("\r\n--XyZ\r\nContent-Disposition: form-data; name=\"we%22ird%0D%0A\"\r\n\r\nv\r\n--XyZ--\r\n")));
	TestTrue(TEXT("in-memory multipart body, byte for byte"), Body.GetValue().Bytes == Expected);
	TestTrue(TEXT("kind"), Body.GetValue().Kind == EVaRestBodyKind::Multipart && !Body.GetValue().IsStreamed());
	TestEqual(TEXT("three part descriptors"), Body.GetValue().PartDescriptors.Num(), 3);
	TestEqual(TEXT("binary descriptors drop their bytes"), Body.GetValue().PartDescriptors[1].Bytes.Num(), 0);
	TestEqual(TEXT("... but keep their size"), Body.GetValue().PartDescriptors[1].Size, static_cast<int64>(3));

	FVaRestMultipart Bad(TEXT("XyZ"));
	Bad.AddBytes(TEXT("f"), TEXT("f.bin"), TEXT("text/plain\r\nX-Evil: 1"), { 1 });
	TestTrue(TEXT("a part content type with CRLF is refused"), FVaRestRequestBody::FromMultipart(Bad).HasError());

	FVaRestMultipart Random;
	TestTrue(TEXT("the default boundary is random and long"), Random.GetBoundary().Len() >= 32 && Random.GetBoundary() != FVaRestMultipart().GetBoundary());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMultipartStreamTest, "VaRest.Http.Body.MultipartStream",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMultipartStreamTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::UrlTestPrivate;
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VaRestTests");
	const FString File = Dir / TEXT("multipart-file.bin");
	TArray<uint8> FileBytes;
	FileBytes.SetNumUninitialized(100000);
	for (int32 i = 0; i < FileBytes.Num(); ++i)
	{
		FileBytes[i] = static_cast<uint8>(i * 31 + 7);
	}
	TestTrue(TEXT("fixture written"), FFileHelper::SaveArrayToFile(FileBytes, *File));

	FVaRestMultipart Multipart(TEXT("B"));
	Multipart.AddField(TEXT("a"), TEXT("1")).AddFile(TEXT("f"), File, TEXT("image/png"));
	TestTrue(TEXT("file parts"), Multipart.HasFiles());
	TestEqualSensitive(TEXT("the file name defaults to the clean file name"), Multipart.GetParts()[1].FileName, FString(TEXT("multipart-file.bin")));

	const TValueOrError<FVaRestRequestBody, FString> Body = FVaRestRequestBody::FromMultipart(Multipart);
	TestTrue(TEXT("builds"), Body.HasValue());
	const FVaRestRequestBody& B = Body.GetValue();
	TestTrue(TEXT("streamed: memory, file, memory"), B.IsStreamed() && B.Segments.Num() == 3 && !B.Segments[1].FilePath.IsEmpty());

	TArray<uint8> Expected = Utf8(TEXT("--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\n1\r\n--B\r\nContent-Disposition: form-data; name=\"f\"; filename=\"multipart-file.bin\"\r\nContent-Type: image/png\r\n\r\n"));
	Expected.Append(FileBytes);
	Expected.Append(Utf8(TEXT("\r\n--B--\r\n")));
	TestEqual(TEXT("content length is the sum of the segments"), B.ContentLength, static_cast<int64>(Expected.Num()));

	FVaRestSegmentStream Stream(B.Segments);
	TestEqual(TEXT("TotalSize"), Stream.TotalSize(), static_cast<int64>(Expected.Num()));
	TArray<uint8> Read;
	Read.SetNumZeroed(Expected.Num());
	int64 Done = 0;
	while (Done < Read.Num())
	{
		// curl reads in its own block sizes and seeks to what it has sent (HttpRequestPayload.cpp:77-96)
		const int64 Take = FMath::Min<int64>(4097, Read.Num() - Done);
		Stream.Seek(Done);
		Stream.Serialize(Read.GetData() + Done, Take);
		Done += Take;
	}
	TestTrue(TEXT("streamed bytes equal the whole body"), Read == Expected && !Stream.IsError());
	Stream.Seek(0);
	TArray<uint8> Again;
	Again.SetNumZeroed(Expected.Num());
	Stream.Serialize(Again.GetData(), Again.Num());
	TestTrue(TEXT("seeking back to 0 reads it again"), Again == Expected);
	Stream.Close();

	FVaRestMultipart Missing(TEXT("B"));
	Missing.AddFile(TEXT("f"), Dir / TEXT("does-not-exist.bin"));
	TestTrue(TEXT("a missing file is an error"), !Missing.GetError().IsEmpty() && FVaRestRequestBody::FromMultipart(Missing).HasError());

	const TValueOrError<FVaRestRequestBody, FString> Upload = FVaRestRequestBody::FromFile(File, TEXT("application/octet-stream"));
	TestTrue(TEXT("a streamed file body"), Upload.HasValue() && Upload.GetValue().Kind == EVaRestBodyKind::Stream && Upload.GetValue().ContentLength == FileBytes.Num());
	TestTrue(TEXT("a missing upload file is an error"), FVaRestRequestBody::FromFile(Dir / TEXT("nope.bin"), TEXT("a/b")).HasError());
	IFileManager::Get().Delete(*File);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails on `VaRestBody.h` / `VaRestUrl.h` (no such file).

- [ ] **Step 3: URL functions** — `Source/VaRestHttp/Public/VaRestUrl.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"
#include "VaRestHttpTypes.h"

namespace VaRest::Http
{
	/** RFC 3986: every byte of Text's UTF-8 but the unreserved set (ALPHA DIGIT - . _ ~) becomes %XX, upper-case (spec 3.2) */
	VARESTHTTP_API FString PercentEncode(FStringView Text);

	/** application/x-www-form-urlencoded (WHATWG URL Standard): ALPHA DIGIT * - . _ kept, space as '+', the rest %XX */
	VARESTHTTP_API FString FormEncode(FStringView Text);

	/** %XX back to bytes, read as UTF-8 (invalid sequences become U+FFFD); an invalid escape stays as text */
	VARESTHTTP_API FString PercentDecode(FStringView Text, bool bPlusIsSpace = false);

	/** "http://" or "https://" (any case) followed by a host */
	VARESTHTTP_API bool IsAbsoluteHttpUrl(FStringView Url);

	/**
	 * BaseUrl + Path with exactly one '/' between them (OpenAPI N14: https://h/v1 + /pets is https://h/v1/pets, never
	 * RFC 3986 resolution). An absolute Path replaces BaseUrl; a Path starting with '?' or '#' is appended as is. Both are
	 * trimmed first (v1's iOS fix, spec 3.2).
	 */
	VARESTHTTP_API FString JoinUrl(FStringView BaseUrl, FStringView Path);

	/**
	 * "/users/{id}" with each {name} replaced by PercentEncode(value): one segment, so '/' in a value is %2F. The last
	 * value given for a name wins. Errors (decision 10): a placeholder without a value, an empty value, a value whose name
	 * the template lacks, '{' without '}', a stray '}', and "{}".
	 */
	VARESTHTTP_API TValueOrError<FString, FString> ExpandPathTemplate(FStringView Template, TConstArrayView<TPair<FString, FString>> Params);

	/** Appends ?k=v&... (or &k=v when Url has a query), each key and value PercentEncode'd, in order, repeats kept (OpenAPI N9), before any '#' */
	VARESTHTTP_API void AppendQuery(FString& Url, TConstArrayView<TPair<FString, FString>> Query);

	/**
	 * OpenAPI N9's array styles (decision 34): adds Values to Query as Style says -- Repeat one pair per value, Csv/Ssv/Pipes
	 * one pair with the values joined by ',', ' ' or '|' (encoded later, by AppendQuery). No values: no pair.
	 */
	VARESTHTTP_API void AddQueryValues(TArray<TPair<FString, FString>>& Query, FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style);

	/** RFC 9110 5.6.2 token: one or more of ALPHA DIGIT !#$%&'*+-.^_`|~ */
	VARESTHTTP_API bool IsValidHeaderName(FStringView Name);

	/** No CR, LF or NUL: a value holding one could end its header and start another (header injection) */
	VARESTHTTP_API bool IsValidHeaderValue(FStringView Value);
}
```

`Source/VaRestHttp/Private/VaRestUrl.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestUrl.h"

#include "VaRestJsonUtf8.h"

#include <initializer_list>

namespace VaRest::Http::UrlPrivate
{
	const TCHAR HexDigits[] = TEXT("0123456789ABCDEF");

	bool IsAlnum(uint32 C)
	{
		return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9');
	}

	/** RFC 3986 2.3 */
	bool IsUnreserved(uint32 C)
	{
		return IsAlnum(C) || C == '-' || C == '.' || C == '_' || C == '~';
	}

	/** WHATWG URL Standard, application/x-www-form-urlencoded byte serializer */
	bool IsFormSafe(uint32 C)
	{
		return IsAlnum(C) || C == '*' || C == '-' || C == '.' || C == '_';
	}

	int32 HexValue(uint32 C)
	{
		if (C >= '0' && C <= '9')
		{
			return static_cast<int32>(C - '0');
		}
		if (C >= 'a' && C <= 'f')
		{
			return static_cast<int32>(C - 'a' + 10);
		}
		if (C >= 'A' && C <= 'F')
		{
			return static_cast<int32>(C - 'A' + 10);
		}
		return -1;
	}

	template <typename PredicateType>
	FString Encode(FStringView Text, PredicateType&& IsSafe, bool bSpaceAsPlus)
	{
		// The UTF-8 bytes are what is encoded; an unpaired surrogate becomes U+FFFD, as everywhere in VaRest (spec 2.2)
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
		FString Out;
		Out.Reserve(Utf8.Len());
		for (const UTF8CHAR Ch : Utf8)
		{
			const uint32 C = static_cast<uint8>(Ch);
			if (IsSafe(C))
			{
				Out.AppendChar(static_cast<TCHAR>(C));
			}
			else if (bSpaceAsPlus && C == ' ')
			{
				Out.AppendChar(TEXT('+'));
			}
			else
			{
				Out.AppendChar(TEXT('%'));
				Out.AppendChar(HexDigits[C >> 4]);
				Out.AppendChar(HexDigits[C & 15]);
			}
		}
		return Out;
	}

	bool IsTokenChar(TCHAR C)
	{
		if (IsAlnum(static_cast<uint32>(C)))
		{
			return true;
		}
		switch (C)
		{
		case TEXT('!'): case TEXT('#'): case TEXT('$'): case TEXT('%'): case TEXT('&'): case TEXT('\''): case TEXT('*'):
		case TEXT('+'): case TEXT('-'): case TEXT('.'): case TEXT('^'): case TEXT('_'): case TEXT('`'): case TEXT('|'): case TEXT('~'):
			return true;
		default:
			return false;
		}
	}
}

FString VaRest::Http::PercentEncode(FStringView Text)
{
	return UrlPrivate::Encode(Text, UrlPrivate::IsUnreserved, false);
}

FString VaRest::Http::FormEncode(FStringView Text)
{
	return UrlPrivate::Encode(Text, UrlPrivate::IsFormSafe, true);
}

FString VaRest::Http::PercentDecode(FStringView Text, bool bPlusIsSpace)
{
	using namespace UrlPrivate;
	VaRest::Json::FUtf8Scratch Scratch;
	const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
	VaRest::Json::FUtf8Scratch Bytes;
	Bytes.Reserve(Utf8.Len());
	for (int32 i = 0; i < Utf8.Len(); ++i)
	{
		const uint32 C = static_cast<uint8>(Utf8[i]);
		if (C == '%' && i + 2 < Utf8.Len())
		{
			const int32 Hi = HexValue(static_cast<uint8>(Utf8[i + 1]));
			const int32 Lo = HexValue(static_cast<uint8>(Utf8[i + 2]));
			if (Hi >= 0 && Lo >= 0)
			{
				Bytes.Add(static_cast<UTF8CHAR>(Hi * 16 + Lo));
				i += 2;
				continue;
			}
		}
		Bytes.Add(bPlusIsSpace && C == '+' ? static_cast<UTF8CHAR>(' ') : Utf8[i]);
	}
	VaRest::Json::FUtf8Scratch Repaired;
	VaRest::Json::AppendRepairedUtf8(FUtf8StringView(Bytes.GetData(), Bytes.Num()), Repaired);
	return FString::ConstructFromPtrSize(Repaired.GetData(), Repaired.Num());
}

bool VaRest::Http::IsAbsoluteHttpUrl(FStringView Url)
{
	const FStringView Trimmed = Url.TrimStartAndEnd();
	for (const FStringView Scheme : { TEXTVIEW("http://"), TEXTVIEW("https://") })
	{
		if (Trimmed.StartsWith(Scheme, ESearchCase::IgnoreCase))
		{
			const FStringView Rest = Trimmed.RightChop(Scheme.Len());
			return !Rest.IsEmpty() && Rest[0] != TEXT('/') && Rest[0] != TEXT('?') && Rest[0] != TEXT('#');
		}
	}
	return false;
}

FString VaRest::Http::JoinUrl(FStringView BaseUrl, FStringView Path)
{
	FStringView Base = BaseUrl.TrimStartAndEnd();
	FStringView Rest = Path.TrimStartAndEnd();
	if (IsAbsoluteHttpUrl(Rest) || Base.IsEmpty())
	{
		return FString(Rest);
	}
	// Decision 33 (review M22): a base URL's query stays, after the joined path and before the request's own pairs
	// ("https://h/v1?key=k" + "/pets?limit=2" -> "https://h/v1/pets?key=k&limit=2"). A base with a fragment is refused
	// before this (Resolve, Kind=Config).
	FStringView BaseQuery;
	int32 BaseQueryAt = INDEX_NONE;
	if (Base.FindChar(TEXT('?'), BaseQueryAt))
	{
		BaseQuery = Base.RightChop(BaseQueryAt + 1);
		Base = Base.Left(BaseQueryAt);
	}
	// The request's own tail: its query and fragment, after its path
	FStringView Tail;
	for (int32 i = 0; i < Rest.Len(); ++i)
	{
		if (Rest[i] == TEXT('?') || Rest[i] == TEXT('#'))
		{
			Tail = Rest.RightChop(i);
			Rest = Rest.Left(i);
			break;
		}
	}
	FString Out;
	Out.Reserve(Base.Len() + 1 + Rest.Len() + 1 + BaseQuery.Len() + Tail.Len());
	if (Rest.IsEmpty())
	{
		Out += Base;
	}
	else
	{
		while (Base.EndsWith(TEXT('/')))
		{
			Base.LeftChopInline(1);
		}
		while (Rest.StartsWith(TEXT('/')))
		{
			Rest.RightChopInline(1);
		}
		Out += Base;
		Out.AppendChar(TEXT('/'));
		Out += Rest;
	}
	if (!BaseQuery.IsEmpty())
	{
		Out.AppendChar(TEXT('?'));
		Out += BaseQuery;
		if (Tail.StartsWith(TEXT('?')))
		{
			Tail.RightChopInline(1);
			if (!Tail.IsEmpty() && Tail[0] != TEXT('#'))
			{
				Out.AppendChar(TEXT('&'));
			}
		}
	}
	Out += Tail;
	return Out;
}

TValueOrError<FString, FString> VaRest::Http::ExpandPathTemplate(FStringView Template, TConstArrayView<TPair<FString, FString>> Params)
{
	TArray<bool, TInlineAllocator<8>> Used;
	Used.SetNumZeroed(Params.Num());
	FString Out;
	Out.Reserve(Template.Len() + 16);
	for (int32 i = 0; i < Template.Len(); ++i)
	{
		const TCHAR C = Template[i];
		if (C == TEXT('}'))
		{
			return MakeError(FString::Printf(TEXT("unmatched '}' in path template \"%s\""), *FString(Template)));
		}
		if (C != TEXT('{'))
		{
			Out.AppendChar(C);
			continue;
		}
		int32 Close = INDEX_NONE;
		for (int32 j = i + 1; j < Template.Len() && Template[j] != TEXT('{'); ++j)
		{
			if (Template[j] == TEXT('}'))
			{
				Close = j;
				break;
			}
		}
		if (Close == INDEX_NONE)
		{
			return MakeError(FString::Printf(TEXT("unclosed '{' in path template \"%s\""), *FString(Template)));
		}
		const FStringView Name = Template.Mid(i + 1, Close - i - 1);
		if (Name.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("empty {} in path template \"%s\""), *FString(Template)));
		}
		int32 Found = INDEX_NONE;
		for (int32 p = 0; p < Params.Num(); ++p)
		{
			if (FStringView(Params[p].Key).Equals(Name, ESearchCase::CaseSensitive))
			{
				Used[p] = true;
				Found = p; // keep going: the last value for a name wins
			}
		}
		if (Found == INDEX_NONE)
		{
			return MakeError(FString::Printf(TEXT("path parameter {%s} has no value"), *FString(Name)));
		}
		if (Params[Found].Value.IsEmpty())
		{
			return MakeError(FString::Printf(TEXT("path parameter {%s} is empty"), *FString(Name)));
		}
		Out += PercentEncode(Params[Found].Value);
		i = Close;
	}
	for (int32 p = 0; p < Params.Num(); ++p)
	{
		if (!Used[p])
		{
			return MakeError(FString::Printf(TEXT("path parameter \"%s\" is not in the path template \"%s\""), *Params[p].Key, *FString(Template)));
		}
	}
	return MakeValue(MoveTemp(Out));
}

void VaRest::Http::AppendQuery(FString& Url, TConstArrayView<TPair<FString, FString>> Query)
{
	if (Query.Num() == 0)
	{
		return;
	}
	FString Fragment;
	int32 Hash = INDEX_NONE;
	if (Url.FindChar(TEXT('#'), Hash))
	{
		Fragment = Url.Mid(Hash);
		Url.LeftInline(Hash);
	}
	int32 Question = INDEX_NONE;
	bool bNeedSeparator = true;
	TCHAR Separator = TEXT('?');
	if (Url.FindChar(TEXT('?'), Question))
	{
		Separator = TEXT('&');
		bNeedSeparator = !(Url.EndsWith(TEXT("?")) || Url.EndsWith(TEXT("&")));
	}
	for (const TPair<FString, FString>& Pair : Query)
	{
		if (bNeedSeparator)
		{
			Url.AppendChar(Separator);
		}
		bNeedSeparator = true;
		Separator = TEXT('&');
		Url += PercentEncode(Pair.Key);
		Url.AppendChar(TEXT('='));
		Url += PercentEncode(Pair.Value);
	}
	Url += Fragment;
}

void VaRest::Http::AddQueryValues(TArray<TPair<FString, FString>>& Query, FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style)
{
	if (Values.Num() == 0)
	{
		return;
	}
	if (Style == EVaRestQueryStyle::Repeat)
	{
		for (const FString& Value : Values)
		{
			Query.Emplace(FString(Name), Value);
		}
		return;
	}
	const TCHAR* Joiner = Style == EVaRestQueryStyle::Csv ? TEXT(",") : Style == EVaRestQueryStyle::Ssv ? TEXT(" ") : TEXT("|");
	Query.Emplace(FString(Name), FString::Join(Values, Joiner));
}

bool VaRest::Http::IsValidHeaderName(FStringView Name)
{
	if (Name.IsEmpty())
	{
		return false;
	}
	for (const TCHAR C : Name)
	{
		if (!UrlPrivate::IsTokenChar(C))
		{
			return false;
		}
	}
	return true;
}

bool VaRest::Http::IsValidHeaderValue(FStringView Value)
{
	for (const TCHAR C : Value)
	{
		if (C == TEXT('\r') || C == TEXT('\n') || C == TEXT('\0'))
		{
			return false;
		}
	}
	return true;
}
```

- [ ] **Step 4: Bodies** — `Source/VaRestHttp/Public/VaRestBody.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/Archive.h"
#include "Templates/ValueOrError.h"

class IFileHandle;

/** application/x-www-form-urlencoded fields, in order (spec 3.2). Ref-qualified like FVaRestJson::Set. */
class VARESTHTTP_API FVaRestForm
{
public:
	FVaRestForm& Add(FStringView Name, FStringView Value) &;
	FVaRestForm Add(FStringView Name, FStringView Value) &&;

	const TArray<TPair<FString, FString>>& GetFields() const { return Fields; }

	/** name=value&... with FormEncode'd names and values, as UTF-8 bytes */
	TArray<uint8> Encode() const;

private:
	TArray<TPair<FString, FString>> Fields;
};

/** One multipart/form-data part. Binary bytes are dropped from the descriptors a request keeps for logs and traces. */
struct FVaRestMultipartPart
{
	FString Name;
	FString FileName;
	FString ContentType;
	/** A field's UTF-8 text, or an in-memory file's bytes */
	TArray<uint8> Bytes;
	/** A file streamed at upload time */
	FString FilePath;
	int64 Size = 0;
	/** A text field: no filename, no Content-Type */
	bool bField = false;
};

/** A run of request body bytes: in memory, or a file read while uploading */
struct FVaRestBodySegment
{
	TArray<uint8> Bytes;
	FString FilePath;
	int64 FileSize = 0;

	int64 Size() const { return FilePath.IsEmpty() ? Bytes.Num() : FileSize; }
};

/**
 * multipart/form-data (spec 3.2). File parts are streamed: their size is read when the part is added (on the calling
 * thread), their bytes while uploading. Names and file names are escaped the WHATWG way ('"', CR, LF as %22, %0D, %0A).
 */
class VARESTHTTP_API FVaRestMultipart
{
public:
	/** A random boundary */
	FVaRestMultipart();
	explicit FVaRestMultipart(FStringView InBoundary);

	FVaRestMultipart& AddField(FStringView Name, FStringView Value) &;
	FVaRestMultipart AddField(FStringView Name, FStringView Value) &&;
	FVaRestMultipart& AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &;
	FVaRestMultipart AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &&;
	/** A missing file makes GetError() non-empty, and sending the body fails with Kind=Config */
	FVaRestMultipart& AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType = TEXTVIEW("application/octet-stream"), FStringView FileName = FStringView()) &;
	FVaRestMultipart AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType = TEXTVIEW("application/octet-stream"), FStringView FileName = FStringView()) &&;

	const FString& GetBoundary() const { return Boundary; }
	const TArray<FVaRestMultipartPart>& GetParts() const { return Parts; }
	/** multipart/form-data; boundary=... */
	FString GetContentType() const;
	bool HasFiles() const;
	/** The first problem met while adding parts, or "" */
	const FString& GetError() const { return Error; }

	/** The encoded body: memory runs merged, one segment per file part */
	TArray<FVaRestBodySegment> Encode() const;

private:
	FString Boundary;
	TArray<FVaRestMultipartPart> Parts;
	FString Error;
};

/**
 * An FArchive over body segments, for IHttpRequest::SetContentFromStream, whose Serialize "will be called from another
 * thread other than the game thread" (IHttpRequest.h:250-257). The engine seeks to what it has sent and reads on
 * (HttpRequestPayload.cpp:77-96). A file that shrank since its part was added reads as zeros, with the error flag set
 * and one warning.
 */
class VARESTHTTP_API FVaRestSegmentStream final : public FArchive
{
public:
	explicit FVaRestSegmentStream(TArray<FVaRestBodySegment> InSegments);
	virtual ~FVaRestSegmentStream() override;

	virtual void Serialize(void* Data, int64 Length) override;
	virtual void Seek(int64 InPos) override;
	virtual int64 Tell() override { return Position; }
	virtual int64 TotalSize() override { return Total; }
	virtual bool Close() override;
	virtual FString GetArchiveName() const override { return TEXT("VaRest multipart body"); }

private:
	bool ReadFileSegment(int32 Index, int64 Offset, uint8* Out, int64 Count);

	TArray<FVaRestBodySegment> Segments;
	int64 Total = 0;
	int64 Position = 0;
	int32 OpenIndex = INDEX_NONE;
	TUniquePtr<IFileHandle> OpenFile;
	bool bWarned = false;
};

/** What a body is, for logs and traces (Inspector IN-1 Body.Kind) */
enum class EVaRestBodyKind : uint8
{
	None,
	Json,
	Text,
	Form,
	Multipart,
	Binary,
	/** A streamed file upload */
	Stream
};

/** A request body ready for the transport */
struct VARESTHTTP_API FVaRestRequestBody
{
	EVaRestBodyKind Kind = EVaRestBodyKind::None;
	/** The default Content-Type; a request's own Content-Type header wins (spec 3.2) */
	FString ContentType;
	/** Json, Text, Form, Binary, and Multipart without file parts */
	TArray<uint8> Bytes;
	/** Stream: the file uploaded with SetContentAsStreamedFile */
	FString FilePath;
	/** Multipart with file parts: uploaded with SetContentFromStream */
	TArray<FVaRestBodySegment> Segments;
	/** Multipart: the parts without their binary bytes (text fields keep theirs), for logs and traces */
	TArray<FVaRestMultipartPart> PartDescriptors;
	int64 ContentLength = 0;

	bool IsEmpty() const { return Kind == EVaRestBodyKind::None; }
	bool IsStreamed() const { return !FilePath.IsEmpty() || Segments.Num() > 0; }

	static FVaRestRequestBody FromBytes(TArray<uint8>&& InBytes, EVaRestBodyKind InKind, FStringView InContentType);
	static FVaRestRequestBody FromForm(const FVaRestForm& Form);
	static TValueOrError<FVaRestRequestBody, FString> FromMultipart(const FVaRestMultipart& Multipart);
	static TValueOrError<FVaRestRequestBody, FString> FromFile(FStringView InFilePath, FStringView InContentType);
};
```

`Source/VaRestHttp/Private/VaRestBody.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBody.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "VaRestHttpLog.h"
#include "VaRestJsonUtf8.h"
#include "VaRestUrl.h"

namespace VaRest::Http::BodyPrivate
{
	void AppendUtf8(TArray<uint8>& Out, FStringView Text)
	{
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
		Out.Append(reinterpret_cast<const uint8*>(Utf8.GetData()), Utf8.Len());
	}

	/** WHATWG HTML, multipart/form-data encoding: '"', CR and LF in a name or file name are percent-escaped */
	FString EscapeQuoted(FStringView Text)
	{
		FString Out;
		Out.Reserve(Text.Len());
		for (const TCHAR C : Text)
		{
			if (C == TEXT('"'))
			{
				Out += TEXT("%22");
			}
			else if (C == TEXT('\r'))
			{
				Out += TEXT("%0D");
			}
			else if (C == TEXT('\n'))
			{
				Out += TEXT("%0A");
			}
			else
			{
				Out.AppendChar(C);
			}
		}
		return Out;
	}
}

FVaRestForm& FVaRestForm::Add(FStringView Name, FStringView Value) &
{
	Fields.Emplace(FString(Name), FString(Value));
	return *this;
}

FVaRestForm FVaRestForm::Add(FStringView Name, FStringView Value) &&
{
	Add(Name, Value);
	return MoveTemp(*this);
}

TArray<uint8> FVaRestForm::Encode() const
{
	FString Joined;
	for (int32 i = 0; i < Fields.Num(); ++i)
	{
		if (i > 0)
		{
			Joined.AppendChar(TEXT('&'));
		}
		Joined += VaRest::Http::FormEncode(Fields[i].Key);
		Joined.AppendChar(TEXT('='));
		Joined += VaRest::Http::FormEncode(Fields[i].Value);
	}
	TArray<uint8> Out;
	VaRest::Http::BodyPrivate::AppendUtf8(Out, Joined); // ASCII after FormEncode
	return Out;
}

FVaRestMultipart::FVaRestMultipart()
	: Boundary(TEXT("----VaRest") + FGuid::NewGuid().ToString(EGuidFormats::Digits))
{
}

FVaRestMultipart::FVaRestMultipart(FStringView InBoundary)
	: Boundary(InBoundary)
{
}

FVaRestMultipart& FVaRestMultipart::AddField(FStringView Name, FStringView Value) &
{
	FVaRestMultipartPart& Part = Parts.AddDefaulted_GetRef();
	Part.Name = FString(Name);
	Part.bField = true;
	VaRest::Http::BodyPrivate::AppendUtf8(Part.Bytes, Value);
	Part.Size = Part.Bytes.Num();
	return *this;
}

FVaRestMultipart FVaRestMultipart::AddField(FStringView Name, FStringView Value) &&
{
	AddField(Name, Value);
	return MoveTemp(*this);
}

FVaRestMultipart& FVaRestMultipart::AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &
{
	if (!VaRest::Http::IsValidHeaderValue(PartContentType) && Error.IsEmpty())
	{
		Error = FString::Printf(TEXT("multipart part \"%s\": the content type holds CR, LF or NUL"), *FString(Name));
	}
	FVaRestMultipartPart& Part = Parts.AddDefaulted_GetRef();
	Part.Name = FString(Name);
	Part.FileName = FString(FileName);
	Part.ContentType = PartContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : FString(PartContentType);
	Part.Size = InBytes.Num();
	Part.Bytes = MoveTemp(InBytes);
	return *this;
}

FVaRestMultipart FVaRestMultipart::AddBytes(FStringView Name, FStringView FileName, FStringView PartContentType, TArray<uint8> InBytes) &&
{
	AddBytes(Name, FileName, PartContentType, MoveTemp(InBytes));
	return MoveTemp(*this);
}

FVaRestMultipart& FVaRestMultipart::AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType, FStringView FileName) &
{
	const FString Path(FilePath);
	const int64 FileSize = IFileManager::Get().FileSize(*Path);
	if (FileSize < 0 && Error.IsEmpty())
	{
		Error = FString::Printf(TEXT("multipart part \"%s\": file not found: %s"), *FString(Name), *Path);
	}
	if (!VaRest::Http::IsValidHeaderValue(PartContentType) && Error.IsEmpty())
	{
		Error = FString::Printf(TEXT("multipart part \"%s\": the content type holds CR, LF or NUL"), *FString(Name));
	}
	FVaRestMultipartPart& Part = Parts.AddDefaulted_GetRef();
	Part.Name = FString(Name);
	Part.FileName = FileName.IsEmpty() ? FPaths::GetCleanFilename(Path) : FString(FileName);
	Part.ContentType = PartContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : FString(PartContentType);
	Part.FilePath = Path;
	Part.Size = FMath::Max<int64>(FileSize, 0);
	return *this;
}

FVaRestMultipart FVaRestMultipart::AddFile(FStringView Name, FStringView FilePath, FStringView PartContentType, FStringView FileName) &&
{
	AddFile(Name, FilePath, PartContentType, FileName);
	return MoveTemp(*this);
}

FString FVaRestMultipart::GetContentType() const
{
	return TEXT("multipart/form-data; boundary=") + Boundary;
}

bool FVaRestMultipart::HasFiles() const
{
	return Parts.ContainsByPredicate([](const FVaRestMultipartPart& Part) { return !Part.FilePath.IsEmpty(); });
}

TArray<FVaRestBodySegment> FVaRestMultipart::Encode() const
{
	using VaRest::Http::BodyPrivate::AppendUtf8;
	using VaRest::Http::BodyPrivate::EscapeQuoted;
	TArray<FVaRestBodySegment> Segments;
	TArray<uint8> Pending;
	for (const FVaRestMultipartPart& Part : Parts)
	{
		AppendUtf8(Pending, TEXT("--"));
		AppendUtf8(Pending, Boundary);
		AppendUtf8(Pending, TEXT("\r\nContent-Disposition: form-data; name=\""));
		AppendUtf8(Pending, EscapeQuoted(Part.Name));
		AppendUtf8(Pending, TEXT("\""));
		if (!Part.bField)
		{
			AppendUtf8(Pending, TEXT("; filename=\""));
			AppendUtf8(Pending, EscapeQuoted(Part.FileName));
			AppendUtf8(Pending, TEXT("\"\r\nContent-Type: "));
			AppendUtf8(Pending, Part.ContentType);
		}
		AppendUtf8(Pending, TEXT("\r\n\r\n"));
		if (Part.FilePath.IsEmpty())
		{
			Pending.Append(Part.Bytes);
		}
		else
		{
			FVaRestBodySegment& Head = Segments.AddDefaulted_GetRef();
			Head.Bytes = MoveTemp(Pending);
			Pending.Reset();
			FVaRestBodySegment& File = Segments.AddDefaulted_GetRef();
			File.FilePath = Part.FilePath;
			File.FileSize = Part.Size;
		}
		AppendUtf8(Pending, TEXT("\r\n"));
	}
	AppendUtf8(Pending, TEXT("--"));
	AppendUtf8(Pending, Boundary);
	AppendUtf8(Pending, TEXT("--\r\n"));
	FVaRestBodySegment& Tail = Segments.AddDefaulted_GetRef();
	Tail.Bytes = MoveTemp(Pending);
	return Segments;
}

FVaRestSegmentStream::FVaRestSegmentStream(TArray<FVaRestBodySegment> InSegments)
	: Segments(MoveTemp(InSegments))
{
	SetIsLoading(true);
	SetIsPersistent(false);
	for (const FVaRestBodySegment& Segment : Segments)
	{
		Total += Segment.Size();
	}
}

FVaRestSegmentStream::~FVaRestSegmentStream() = default;

void FVaRestSegmentStream::Seek(int64 InPos)
{
	Position = FMath::Clamp<int64>(InPos, 0, Total);
}

bool FVaRestSegmentStream::Close()
{
	OpenFile.Reset();
	OpenIndex = INDEX_NONE;
	return !IsError();
}

bool FVaRestSegmentStream::ReadFileSegment(int32 Index, int64 Offset, uint8* Out, int64 Count)
{
	if (OpenIndex != Index)
	{
		OpenFile.Reset(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Segments[Index].FilePath));
		OpenIndex = Index;
	}
	return OpenFile.IsValid() && OpenFile->Seek(Offset) && OpenFile->Read(Out, Count);
}

void FVaRestSegmentStream::Serialize(void* Data, int64 Length)
{
	uint8* Out = static_cast<uint8*>(Data);
	while (Length > 0)
	{
		int64 SegmentStart = 0;
		int32 Index = 0;
		for (; Index < Segments.Num(); ++Index)
		{
			const int64 SegmentSize = Segments[Index].Size();
			if (Position < SegmentStart + SegmentSize)
			{
				break;
			}
			SegmentStart += SegmentSize;
		}
		if (Index >= Segments.Num())
		{
			// Past the end: the caller asked for more than TotalSize()
			FMemory::Memzero(Out, Length);
			SetError();
			return;
		}
		const FVaRestBodySegment& Segment = Segments[Index];
		const int64 Offset = Position - SegmentStart;
		const int64 Count = FMath::Min<int64>(Length, Segment.Size() - Offset);
		if (Segment.FilePath.IsEmpty())
		{
			FMemory::Memcpy(Out, Segment.Bytes.GetData() + Offset, Count);
		}
		else if (!ReadFileSegment(Index, Offset, Out, Count))
		{
			FMemory::Memzero(Out, Count);
			SetError();
			if (!bWarned)
			{
				bWarned = true;
				UE_LOG(LogVaRestHttp, Warning, TEXT("VaRest: multipart file %s could not be read at byte %lld (changed since it was added?); sending zeros"), *Segment.FilePath, Offset);
			}
		}
		Out += Count;
		Position += Count;
		Length -= Count;
	}
}

FVaRestRequestBody FVaRestRequestBody::FromBytes(TArray<uint8>&& InBytes, EVaRestBodyKind InKind, FStringView InContentType)
{
	FVaRestRequestBody Out;
	Out.Kind = InKind;
	Out.ContentType = FString(InContentType);
	Out.ContentLength = InBytes.Num();
	Out.Bytes = MoveTemp(InBytes);
	return Out;
}

FVaRestRequestBody FVaRestRequestBody::FromForm(const FVaRestForm& Form)
{
	return FromBytes(Form.Encode(), EVaRestBodyKind::Form, TEXT("application/x-www-form-urlencoded"));
}

TValueOrError<FVaRestRequestBody, FString> FVaRestRequestBody::FromMultipart(const FVaRestMultipart& Multipart)
{
	if (!Multipart.GetError().IsEmpty())
	{
		return MakeError(Multipart.GetError());
	}
	// Review I3: the boundary goes into the Content-Type header -- 1 to 70 RFC 2046 bchars (2046 5.1.1), so no CR/LF
	// (header injection) and no '"'. The text is not repeated in the message: it may hold control characters.
	const FString& Boundary = Multipart.GetBoundary();
	bool bValidBoundary = Boundary.Len() >= 1 && Boundary.Len() <= 70 && !Boundary.EndsWith(TEXT(" "));
	for (const TCHAR C : Boundary)
	{
		const bool bAlnum = (C >= TEXT('0') && C <= TEXT('9')) || (C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z'));
		bValidBoundary &= bAlnum || FCString::Strchr(TEXT("'()+_,-./:=? "), C) != nullptr;
	}
	if (!bValidBoundary)
	{
		return MakeError(FString(TEXT("the multipart boundary must be 1-70 characters of digits, letters and '()+_,-./:=? (no space at the end; RFC 2046 5.1.1)")));
	}
	FVaRestRequestBody Out;
	Out.Kind = EVaRestBodyKind::Multipart;
	Out.ContentType = Multipart.GetContentType();
	TArray<FVaRestBodySegment> Encoded = Multipart.Encode();
	for (const FVaRestBodySegment& Segment : Encoded)
	{
		Out.ContentLength += Segment.Size();
	}
	if (Multipart.HasFiles())
	{
		Out.Segments = MoveTemp(Encoded);
	}
	else
	{
		Out.Bytes = MoveTemp(Encoded[0].Bytes);
	}
	for (const FVaRestMultipartPart& Part : Multipart.GetParts())
	{
		FVaRestMultipartPart& Descriptor = Out.PartDescriptors.Add_GetRef(Part);
		if (!Part.bField)
		{
			Descriptor.Bytes.Empty();
		}
	}
	return MakeValue(MoveTemp(Out));
}

TValueOrError<FVaRestRequestBody, FString> FVaRestRequestBody::FromFile(FStringView InFilePath, FStringView InContentType)
{
	const FString Path(InFilePath);
	const int64 FileSize = IFileManager::Get().FileSize(*Path);
	if (FileSize < 0)
	{
		return MakeError(FString::Printf(TEXT("upload file not found: %s"), *Path));
	}
	if (!VaRest::Http::IsValidHeaderValue(InContentType))
	{
		return MakeError(FString(TEXT("the upload content type holds CR, LF or NUL")));
	}
	FVaRestRequestBody Out;
	Out.Kind = EVaRestBodyKind::Stream;
	Out.ContentType = InContentType.IsEmpty() ? FString(TEXT("application/octet-stream")) : FString(InContentType);
	Out.FilePath = Path;
	Out.ContentLength = FileSize;
	return MakeValue(MoveTemp(Out));
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including the eight new `VaRest.Http.Url.*` / `VaRest.Http.Body.*` tests.

- [ ] **Step 6: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestUrl.h Source/VaRestHttp/Private/VaRestUrl.cpp \
  Source/VaRestHttp/Public/VaRestBody.h Source/VaRestHttp/Private/VaRestBody.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpUrlTest.cpp
git commit -m "http: request encoding -- URL join (OpenAPI N14), path templates, query, header checks, forms, multipart streams

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 4: The trace surface — kinds, payloads, sinks, detail mask, snapshot, isolation, counters (Inspector §9)

Inspector §9 is "the contract for the core M3/M4/M5, WS W5 and SIO S4 plans". This task builds the parts that hold no request logic: the thirteen kinds (M3 emits five, Task 15), the payload types of the kinds M3 emits, `IVaRestTraceSink`, the registry with its immutable sink list and atomic detail mask (IN-11, IN-12), the late-attach snapshot with scope 0 "Process" (IN-7, IN-12), ids that are always assigned (IN-10), the dev-test counters, and `FScopedSinkIsolation`. Payloads hold no `TSharedRef` to anything internal, so a sink may keep them. `FVaRestTraceExcerpt` is defined here (Inspector §1.3 lists it in `VaRestHttp`) because the redactor (Task 5) builds it for logs now and M4's `Excerpt` event carries it.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestTrace.h`, `Source/VaRestHttp/Private/VaRestTrace.cpp`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpTraceSurfaceTest.cpp`

**Interfaces:**
- Consumes: `FVaRestHeaders`, `EVaRestPriority`, `EVaRestErrorKind`, `EVaRestTransportFailure`, `EVaRestCancelReason` (Task 2); `EVaRestBodyKind` (Task 3).
- Produces: `EVaRestTraceKind` (13 values, Inspector order), `EVaRestTraceSource { Http, Socket }`, `EVaRestTraceDetail { None, Metadata, Headers, Bodies }` (flags), `FVaRestTraceDetailSettings { Flags, BodyCap, BinaryCap, FrameCap }`, `EVaRestExcerptKind`, `EVaRestExcerptSkip`, `FVaRestTraceExcerpt { Bytes, Kind, TotalBytes, bTruncated, bRedacted, StopReason, Sha256Prefix, SkipReason; ToText() }`, `FVaRestTraceInitiator`, `EVaRestTraceRequestFlags` (flags), `EVaRestTraceCacheLookup`, `FVaRestTraceBody`, `FVaRestTraceQueueReplay`, `FVaRestTraceStarted` (+ `CurrentAttempt`, `StartedAt` for snapshots), `EVaRestTraceGap`, `FVaRestTraceAttemptDispatched`, `FVaRestTraceAttemptEnded`, `EVaRestTraceOutcome`, `FVaRestTraceError`, `FVaRestTraceDecodeWarning`, `FVaRestTraceEnvelope`, `FVaRestTraceCompleted` (`MaxDecodeWarnings = 32`), `FVaRestTraceScope`, `FVaRestTraceClient`, `using FVaRestTracePayload = TVariant<FVaRestTraceStarted, FVaRestTraceAttemptDispatched, FVaRestTraceAttemptEnded, FVaRestTraceCompleted, FVaRestTraceScope, FVaRestTraceClient>`, `FVaRestTraceEvent { Kind, Source, Seq, Time, Payload }`, `FVaRestTraceSnapshot { Scopes, Clients, InFlight }`, `IVaRestTraceSink { GetDetail, OnAttached, OnEvent }`, `FVaRestTraceCounters { EventsBuilt, RedactorCalls, DelegateBinds, TraceCycles; Reset() }`, `FVaRestTrace` (static: `Detail()`, `Wants(Flag)`, `GetDetailSettings()`, `AddSink`, `RemoveSink`, `RefreshDetail`, `GetSinks`, `Emit(Kind, Time, Payload&&, Source)`, `OnBuildSnapshot()`, `BuildSnapshot()`, `NewRequestId()`, `NewClientId()`, `Counters()`, `ProcessScopeId = 0`, `using FScopedSinkIsolation = FVaRestScopedSinkIsolation`, `struct FScopedCycles`), `FVaRestScopedSinkIsolation`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpTraceSurfaceTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/ParallelFor.h"
#include "Misc/AutomationTest.h"
#include "VaRestTrace.h"

namespace VaRest::Http::TraceSurfaceTestPrivate
{
	class FSink final : public IVaRestTraceSink
	{
	public:
		explicit FSink(EVaRestTraceDetail InFlags, int32 InBodyCap = 0)
		{
			Detail.Flags = InFlags;
			Detail.BodyCap = InBodyCap;
		}
		virtual FVaRestTraceDetailSettings GetDetail() const override { return Detail; }
		virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override
		{
			++Attached;
			LastSnapshot = Snapshot;
		}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override { Events.Add(Event); }

		FVaRestTraceDetailSettings Detail;
		int32 Attached = 0;
		FVaRestTraceSnapshot LastSnapshot;
		TArray<FVaRestTraceEvent> Events;
	};

	FVaRestTracePayload Started(uint64 RequestId)
	{
		FVaRestTraceStarted Payload;
		Payload.RequestId = RequestId;
		return FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Payload));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceMaskTest, "VaRest.Http.TraceSurface.MaskAndSinks",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceMaskTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	TestTrue(TEXT("isolated: no sink, mask 0"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);

	const uint64 BuiltBefore = FVaRestTrace::Counters().EventsBuilt;
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 1.0, Started(1));
	TestEqual(TEXT("no sink: nothing is built"), FVaRestTrace::Counters().EventsBuilt.load(), BuiltBefore);

	TSharedRef<FSink, ESPMode::ThreadSafe> A = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(A);
	TestEqual(TEXT("OnAttached runs once, from AddSink"), A->Attached, 1);
	TestTrue(TEXT("the snapshot has scope 0 \"Process\" (IN-7)"), A->LastSnapshot.Scopes.Num() == 1
		&& A->LastSnapshot.Scopes[0].ScopeId == FVaRestTrace::ProcessScopeId && A->LastSnapshot.Scopes[0].Label == TEXT("Process"));
	TestTrue(TEXT("mask = the sink's detail"), FVaRestTrace::Detail() == EVaRestTraceDetail::Metadata);

	FVaRestTrace::Emit(EVaRestTraceKind::Started, 2.0, Started(42));
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 3.0, Started(43));
	TestEqual(TEXT("events reach the sink"), A->Events.Num(), 2);
	TestTrue(TEXT("payload kept"), A->Events.Num() == 2 && A->Events[0].Payload.Get<FVaRestTraceStarted>().RequestId == 42);
	TestTrue(TEXT("Seq increases"), A->Events.Num() == 2 && A->Events[1].Seq > A->Events[0].Seq && A->Events[0].Seq > 0);
	TestTrue(TEXT("Time is the caller's clock reading"), A->Events.Num() == 2 && A->Events[1].Time == 3.0);
	TestEqual(TEXT("built events are counted"), FVaRestTrace::Counters().EventsBuilt.load(), BuiltBefore + 2);

	TSharedRef<FSink, ESPMode::ThreadSafe> B = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies, 1024);
	FVaRestTrace::AddSink(B);
	TestTrue(TEXT("mask is the union of every sink's flags"), FVaRestTrace::Detail() == (EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies));
	TestEqual(TEXT("caps are the largest asked for"), FVaRestTrace::GetDetailSettings().BodyCap, 1024);

	FVaRestTrace::RemoveSink(A);
	TestTrue(TEXT("removing a sink recomputes the mask"), FVaRestTrace::Detail() == (EVaRestTraceDetail::Headers | EVaRestTraceDetail::Bodies));
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 4.0, Started(44));
	TestEqual(TEXT("a removed sink gets nothing more"), A->Events.Num(), 2);
	TestEqual(TEXT("the other one does"), B->Events.Num(), 1);

	B->Detail.Flags = EVaRestTraceDetail::Metadata;
	FVaRestTrace::RefreshDetail();
	TestTrue(TEXT("RefreshDetail re-reads GetDetail"), FVaRestTrace::Detail() == EVaRestTraceDetail::Metadata);

	FVaRestTrace::RemoveSink(B);
	TestTrue(TEXT("no sinks, mask 0"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceIsolationTest, "VaRest.Http.TraceSurface.Isolation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceIsolationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Outer; // stands in for whatever the editor had registered
	TSharedRef<FSink, ESPMode::ThreadSafe> EditorSink = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(EditorSink);
	{
		FVaRestTrace::FScopedSinkIsolation Inner;
		TestTrue(TEXT("inside an isolation the outer sink is not active"), FVaRestTrace::Detail() == EVaRestTraceDetail::None);
		FVaRestTrace::Emit(EVaRestTraceKind::Started, 1.0, Started(1));
		TSharedRef<FSink, ESPMode::ThreadSafe> Local = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Headers);
		FVaRestTrace::AddSink(Local);
		FVaRestTrace::Emit(EVaRestTraceKind::Started, 2.0, Started(2));
		TestEqual(TEXT("a sink added inside sees events"), Local->Events.Num(), 1);
	}
	TestEqual(TEXT("the outer sink saw nothing from inside"), EditorSink->Events.Num(), 0);
	TestTrue(TEXT("leaving restores the outer list and its mask"), FVaRestTrace::Detail() == EVaRestTraceDetail::Metadata);
	FVaRestTrace::Emit(EVaRestTraceKind::Started, 3.0, Started(3));
	TestEqual(TEXT("... and the outer sink receives again"), EditorSink->Events.Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceSnapshotTest, "VaRest.Http.TraceSurface.Snapshot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceSnapshotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceSurfaceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FDelegateHandle Contributor = FVaRestTrace::OnBuildSnapshot().AddLambda([](FVaRestTraceSnapshot& Snapshot)
	{
		FVaRestTraceClient& Client = Snapshot.Clients.AddDefaulted_GetRef();
		Client.ClientId = 77;
		Client.DisplayName = TEXT("https://api.test");
		FVaRestTraceStarted& InFlight = Snapshot.InFlight.AddDefaulted_GetRef();
		InFlight.RequestId = 9;
		InFlight.Flags = EVaRestTraceRequestFlags::Partial;
	});
	TSharedRef<FSink, ESPMode::ThreadSafe> Sink = MakeShared<FSink, ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FVaRestTrace::OnBuildSnapshot().Remove(Contributor);
	TestTrue(TEXT("contributors fill the snapshot (IN-12)"), Sink->LastSnapshot.Clients.Num() == 1 && Sink->LastSnapshot.Clients[0].ClientId == 77);
	TestTrue(TEXT("in-flight requests arrive as Started{Partial}"), Sink->LastSnapshot.InFlight.Num() == 1
		&& EnumHasAnyFlags(Sink->LastSnapshot.InFlight[0].Flags, EVaRestTraceRequestFlags::Partial));
	FVaRestTrace::RemoveSink(Sink);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceIdsTest, "VaRest.Http.TraceSurface.Ids",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceIdsTest::RunTest(const FString& Parameters)
{
	TArray<uint64> Ids;
	Ids.SetNumZeroed(1000);
	ParallelFor(Ids.Num(), [&Ids](int32 i) { Ids[i] = FVaRestTrace::NewRequestId(); });
	TSet<uint64> Unique;
	Unique.Append(Ids);
	TestEqual(TEXT("ids from many threads are unique (IN-10: cheap counters)"), Unique.Num(), Ids.Num());
	TestFalse(TEXT("0 is never an id"), Unique.Contains(0));
	TestTrue(TEXT("client ids too"), FVaRestTrace::NewClientId() != FVaRestTrace::NewClientId());

	FVaRestTraceExcerpt Excerpt;
	TestTrue(TEXT("an excerpt without bytes is empty text"), Excerpt.ToText().IsEmpty());
	Excerpt.Bytes = MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(TArray<uint8>{ 'h', 'i' });
	TestEqualSensitive(TEXT("excerpt text"), Excerpt.ToText(), FString(TEXT("hi")));
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails on `VaRestTrace.h` (no such file).

- [ ] **Step 3: The contract** — `Source/VaRestHttp/Public/VaRestTrace.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "HAL/PlatformTime.h"
#include "Misc/DateTime.h"
#include "Misc/TVariant.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"

#include <atomic>

/**
 * Inspector spec 9: thirteen kinds in all, in its order. M3 emits Started, AttemptDispatched, AttemptEnded, Completed and
 * Client, and provides scope 0 through the snapshot; Note, Excerpt and Marker arrive with M4, the socket kinds with W5.
 */
enum class EVaRestTraceKind : uint8
{
	Started,
	AttemptDispatched,
	AttemptEnded,
	Note,
	Completed,
	Excerpt,
	Scope,
	Client,
	Marker,
	Socket,
	SocketAttempt,
	SocketFrame,
	SocketNote
};

/** WebSocket spec 19 item 4: who emitted an event. Sinks ignore payload types they do not know. */
enum class EVaRestTraceSource : uint8
{
	Http,
	Socket
};

/** IN-11: what a sink wants. 0 = nothing: every trace point returns after one relaxed load. */
enum class EVaRestTraceDetail : uint32
{
	None = 0,
	Metadata = 1 << 0,
	Headers = 1 << 1,
	Bodies = 1 << 2
};
ENUM_CLASS_FLAGS(EVaRestTraceDetail);

struct FVaRestTraceDetailSettings
{
	EVaRestTraceDetail Flags = EVaRestTraceDetail::None;
	/** Text and JSON excerpt cap (Inspector 3.4 MaxBodyBytes) */
	int32 BodyCap = 0;
	/** Binary excerpt cap; 0 = a descriptor only (Inspector 3.4) */
	int32 BinaryCap = 0;
	/** Socket frame excerpt cap */
	int32 FrameCap = 0;
};

/** IN-6 */
enum class EVaRestExcerptKind : uint8
{
	Json,
	Text,
	Form,
	Multipart,
	Binary,
	Stream,
	Skipped
};

enum class EVaRestExcerptSkip : uint8
{
	None,
	Backlog,
	Disabled
};

/** IN-6: a redacted body excerpt. FVaRestRedactor builds it (logs use it in M3); M4's Excerpt event carries it. */
struct VARESTHTTP_API FVaRestTraceExcerpt
{
	/** Redacted UTF-8 text: a JSON prefix, form pairs, plain text or a descriptor. Shared and never changed. */
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Bytes;
	EVaRestExcerptKind Kind = EVaRestExcerptKind::Text;
	/** The whole body's size */
	int64 TotalBytes = 0;
	bool bTruncated = false;
	bool bRedacted = false;
	/** "" when complete, "cap" when the cap cut it, else the JSON parse error */
	FString StopReason;
	/** Binary: the first 8 hex digits of the body's SHA-256, when the whole body was seen */
	FString Sha256Prefix;
	EVaRestExcerptSkip SkipReason = EVaRestExcerptSkip::None;

	FString ToText() const;
};

/** IN-15 */
struct FVaRestTraceInitiator
{
	FString OwnerPath;
	FString ScriptObject;
	FString ScriptFunction;
	/** Code - Script of the top Blueprint frame; -1 without one */
	int32 ScriptCodeOffset = INDEX_NONE;
	FString SourceFile;
	int32 SourceLine = 0;
};

enum class EVaRestTraceRequestFlags : uint8
{
	None = 0,
	Idempotent = 1 << 0,
	QueueWhenOffline = 1 << 1,
	NoAuth = 1 << 2,
	AuthRefresh = 1 << 3,
	/** A snapshot entry for a request that started before the sink attached */
	Partial = 1 << 4
};
ENUM_CLASS_FLAGS(EVaRestTraceRequestFlags);

enum class EVaRestTraceCacheLookup : uint8
{
	None,
	Miss,
	HitMemory,
	HitDisk,
	Stale,
	Revalidate
};

struct FVaRestTraceBody
{
	FString ContentType;
	int64 Bytes = 0;
	EVaRestBodyKind Kind = EVaRestBodyKind::None;
};

struct FVaRestTraceQueueReplay
{
	FString QueueId;
	uint64 OriginalRequestId = 0;
};

/** IN-1. Fields marked * in the spec arrive redacted: Url, Headers. */
struct FVaRestTraceStarted
{
	uint64 RequestId = 0;
	uint64 ScopeId = 0;
	uint64 ClientId = 0;
	FString Verb;
	FString Url;
	/** The path before {param} substitution, without the query */
	FString PathTemplate;
	FName Endpoint;
	FName Tag;
	EVaRestPriority Priority = EVaRestPriority::Normal;
	EVaRestTraceRequestFlags Flags = EVaRestTraceRequestFlags::None;
	FVaRestTraceQueueReplay QueueReplayOf;
	FVaRestTraceInitiator Initiator;
	/** M4 */
	FName CachePolicy;
	/** M4 */
	EVaRestTraceCacheLookup CacheLookup = EVaRestTraceCacheLookup::None;
	/** As built, redacted; empty unless a sink wants Headers */
	FVaRestHeaders Headers;
	FVaRestTraceBody Body;
	bool bExcerptPending = false;
	FDateTime StartUtc;
	/** Snapshot entries (Partial): the attempt in progress */
	int32 CurrentAttempt = 0;
	/** Snapshot entries: when the request entered the pipeline (IVaRestClock seconds) */
	double StartedAt = 0.0;
};

enum class EVaRestTraceGap : uint8
{
	None,
	Backoff,
	SessionRefresh
};

/** IN-2: on the game thread, immediately before the transport is started */
struct FVaRestTraceAttemptDispatched
{
	uint64 RequestId = 0;
	int32 Attempt = 0;
	double AttemptStart = 0.0;
	/** M4 */
	EVaRestTraceGap GapKind = EVaRestTraceGap::None;
	double GapSeconds = 0.0;
	/** M4 */
	double AuthWait = 0.0;
	uint64 AuthRefreshId = 0;
	/** Seconds this attempt waited in the limiter queue */
	double LimiterWait = 0.0;
	/** Final (after auth and custom per-attempt stages), redacted; empty unless a sink wants Headers */
	FVaRestHeaders Headers;
	/** M4 (revalidation) */
	bool bConditional = false;
};

/** IN-3: on the game thread, when the outcome re-enters the stage chain. The times were written on the HTTP thread. */
struct FVaRestTraceAttemptEnded
{
	uint64 RequestId = 0;
	int32 Attempt = 0;
	/** 0 = no response */
	int32 Status = 0;
	/** Redacted; empty unless a sink wants Headers */
	FVaRestHeaders Headers;
	/** Redacted */
	FString EffectiveUrl;
	FString ContentType;
	int64 BytesUp = 0;
	int64 BytesDown = 0;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	/** IVaRestClock seconds; -1 when unknown */
	double UploadDoneAt = -1.0;
	double StatusAt = -1.0;
	double TransportDoneAt = -1.0;
	bool bExcerptPending = false;
};

enum class EVaRestTraceOutcome : uint8
{
	Success,
	Error,
	Queued,
	Canceled
};

struct FVaRestTraceError
{
	EVaRestErrorKind Kind = EVaRestErrorKind::None;
	int32 HttpStatus = 0;
	FString Code;
	/** Value-scrubbed */
	FString Message;
};

struct FVaRestTraceDecodeWarning
{
	FString Path;
	FString Message;
};

/** M4 fills it */
struct FVaRestTraceEnvelope
{
	bool bApplied = false;
	FString DataPath;
	FString PredicatePath;
	FString Expected;
	FString Actual;
	FString Code;
	FString Message;
};

/** IN-5: on the game thread, before any callback (OnHttpThread ones included) */
struct FVaRestTraceCompleted
{
	static constexpr int32 MaxDecodeWarnings = 32;

	uint64 RequestId = 0;
	EVaRestTraceOutcome Outcome = EVaRestTraceOutcome::Success;
	EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
	FVaRestTraceError Error;
	FName ResultType;
	/** IVaRestClock seconds; -1 when there was no decode step */
	double DecodeStartAt = -1.0;
	double DecodeEndAt = -1.0;
	/** The first MaxDecodeWarnings, Message scrubbed */
	TArray<FVaRestTraceDecodeWarning> DecodeWarnings;
	int32 DecodeWarningCount = 0;
	FVaRestTraceEnvelope Envelope;
};

/** IN-7. M3 has only scope 0, "Process"; M5's hubs add one per game instance. */
struct FVaRestTraceScope
{
	bool bOpened = true;
	uint64 ScopeId = 0;
	int32 PIEInstance = INDEX_NONE;
	bool bEditorScope = false;
	FString Label;
	/** Game thread use only */
	TWeakObjectPtr<UObject> GameInstance;
};

/** IN-8 */
struct FVaRestTraceClient
{
	bool bCreated = true;
	uint64 ClientId = 0;
	uint64 ScopeId = 0;
	/** The asset path (M5), WithName, or the base URL */
	FString DisplayName;
};

using FVaRestTracePayload = TVariant<FVaRestTraceStarted, FVaRestTraceAttemptDispatched, FVaRestTraceAttemptEnded, FVaRestTraceCompleted, FVaRestTraceScope, FVaRestTraceClient>;

struct FVaRestTraceEvent
{
	EVaRestTraceKind Kind = EVaRestTraceKind::Started;
	EVaRestTraceSource Source = EVaRestTraceSource::Http;
	/** Taken only when the mask is non-zero (Inspector 2.3) */
	uint64 Seq = 0;
	/** IVaRestClock seconds of the emitting client */
	double Time = 0.0;
	FVaRestTracePayload Payload;
};

/** IN-12: what a sink learns when it attaches */
struct FVaRestTraceSnapshot
{
	TArray<FVaRestTraceScope> Scopes;
	TArray<FVaRestTraceClient> Clients;
	/** Every in-flight request as Started{Partial} */
	TArray<FVaRestTraceStarted> InFlight;
};

class IVaRestTraceSink
{
public:
	virtual ~IVaRestTraceSink() = default;

	/** Game thread; read by AddSink and RefreshDetail */
	virtual FVaRestTraceDetailSettings GetDetail() const = 0;

	/** Game thread, from AddSink, before the sink receives any event */
	virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) = 0;

	/**
	 * The emitting thread: the game thread for every kind but Excerpt (a background task); never the HTTP thread
	 * (Inspector 9). Return quickly. A background emitter may still deliver once after RemoveSink returns (IN-12).
	 */
	virtual void OnEvent(const FVaRestTraceEvent& Event) = 0;
};

/** IN-11 dev-test counters: the zero-cost proof counts instead of timing (Inspector 3.1 item 6) */
struct FVaRestTraceCounters
{
	std::atomic<uint64> EventsBuilt{ 0 };
	std::atomic<uint64> RedactorCalls{ 0 };
	/** Transport delegate binds made for tracing or progress (OnStatusCodeReceived, OnRequestProgress64) */
	std::atomic<uint64> DelegateBinds{ 0 };
	/** Dev-only cycle accumulator of the trace points (Inspector 8) */
	std::atomic<uint64> TraceCycles{ 0 };

	void Reset()
	{
		EventsBuilt = 0;
		RedactorCalls = 0;
		DelegateBinds = 0;
		TraceCycles = 0;
	}
};

/** Test-only (IN-12): while one lives, only the sinks added inside it are active. Game thread; nests. */
class VARESTHTTP_API FVaRestScopedSinkIsolation
{
public:
	FVaRestScopedSinkIsolation();
	~FVaRestScopedSinkIsolation();
	FVaRestScopedSinkIsolation(const FVaRestScopedSinkIsolation&) = delete;
	FVaRestScopedSinkIsolation& operator=(const FVaRestScopedSinkIsolation&) = delete;
};

/** The sink registry and the trace points' gate (Inspector 3.1, IN-10 to IN-12) */
class VARESTHTTP_API FVaRestTrace
{
public:
	using FSinkRef = TSharedRef<IVaRestTraceSink, ESPMode::ThreadSafe>;
	using FSinkList = TArray<FSinkRef>;
	using FScopedSinkIsolation = FVaRestScopedSinkIsolation;

	/** IN-7: before M5, everything is in this scope */
	static constexpr uint64 ProcessScopeId = 0;

	/** IN-11: one relaxed load. Every trace point starts with `if (FVaRestTrace::Detail() == EVaRestTraceDetail::None) { return; }`. */
	static EVaRestTraceDetail Detail()
	{
		return static_cast<EVaRestTraceDetail>(DetailMask.load(std::memory_order_relaxed));
	}

	static bool Wants(EVaRestTraceDetail Flag)
	{
		return EnumHasAnyFlags(Detail(), Flag);
	}

	/** The largest caps any sink asked for; read after the mask check */
	static FVaRestTraceDetailSettings GetDetailSettings();

	/** Game thread. Calls Sink->OnAttached(BuildSnapshot()) first (IN-12). */
	static void AddSink(const FSinkRef& Sink);
	/** Game thread */
	static void RemoveSink(const FSinkRef& Sink);
	/** Game thread: a sink's GetDetail() changed */
	static void RefreshDetail();
	/** Any thread: the current immutable list (null when there is none) */
	static TSharedPtr<const FSinkList, ESPMode::ThreadSafe> GetSinks();

	/** Stamps Seq and hands the event to every sink. Game thread for every kind but Excerpt. Callers checked Detail() first. */
	static void Emit(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source = EVaRestTraceSource::Http);

	/** Snapshot contributors (core: clients and in-flight requests; W5: sockets). Game thread. */
	static TMulticastDelegate<void(FVaRestTraceSnapshot&)>& OnBuildSnapshot();
	/** Scope 0, then every contributor. Game thread. */
	static FVaRestTraceSnapshot BuildSnapshot();

	/** IN-10: assigned at creation whatever the mask; any thread; never 0 */
	static uint64 NewRequestId();
	static uint64 NewClientId();

	static FVaRestTraceCounters& Counters();

	/** Adds the cycles it lived to Counters().TraceCycles. Build one inside a trace point, after the mask check. */
	struct FScopedCycles
	{
		uint64 Start = FPlatformTime::Cycles64();
		~FScopedCycles() { Counters().TraceCycles += FPlatformTime::Cycles64() - Start; }
	};

private:
	friend class FVaRestScopedSinkIsolation;

	/**
	 * Game thread. Publishes Next with its mask and caps. The caps are merged before the lock is taken and the old list is
	 * released after it is dropped (review M-8): a sink's GetDetail may call GetDetailSettings or GetSinks, and a sink's
	 * destructor may drop a client whose Client{Destroyed} calls GetSinks -- FRWLock is not recursive.
	 */
	static void Swap(TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Next);

	static std::atomic<uint32> DetailMask;
};
```

- [ ] **Step 4: The registry** — `Source/VaRestHttp/Private/VaRestTrace.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTrace.h"

#include "Misc/ScopeRWLock.h"

std::atomic<uint32> FVaRestTrace::DetailMask{ 0 };

namespace VaRest::Http::TracePrivate
{
	/**
	 * The sink list, the merged caps and the isolation stack. The list itself is immutable once published (IN-12). Leaked
	 * on purpose (review I-4): nothing in VaRestHttp may run at static destruction, where a client state dying late (a
	 * global FVaRestClient) or a sink still registered (ShutdownModule never runs in an automation session) would lock a
	 * destroyed lock or read a freed list.
	 */
	struct FRegistry
	{
		FRWLock Lock;
		TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe> Sinks;
		FVaRestTraceDetailSettings Settings;
		/** Lists saved by FVaRestScopedSinkIsolation, innermost last */
		TArray<TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe>> Saved;
	};

	FRegistry& Registry()
	{
		static FRegistry* Instance = new FRegistry();
		return *Instance;
	}

	// Trivially destructible: safe as statics
	std::atomic<uint64> NextSeq{ 1 };
	std::atomic<uint64> NextRequest{ 1 };
	std::atomic<uint64> NextClient{ 1 };

	/** Outside any lock: GetDetail is the sink's code (review M-8) */
	FVaRestTraceDetailSettings Merge(const FVaRestTrace::FSinkList* List)
	{
		FVaRestTraceDetailSettings Merged;
		if (List)
		{
			for (const FVaRestTrace::FSinkRef& Sink : *List)
			{
				const FVaRestTraceDetailSettings One = Sink->GetDetail();
				Merged.Flags |= One.Flags;
				Merged.BodyCap = FMath::Max(Merged.BodyCap, One.BodyCap);
				Merged.BinaryCap = FMath::Max(Merged.BinaryCap, One.BinaryCap);
				Merged.FrameCap = FMath::Max(Merged.FrameCap, One.FrameCap);
			}
		}
		return Merged;
	}
}

FString FVaRestTraceExcerpt::ToText() const
{
	return Bytes.IsValid() ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes->GetData()), Bytes->Num()) : FString();
}

void FVaRestTrace::Swap(TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Next)
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	const FVaRestTraceDetailSettings Merged = Merge(Next.Get());
	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Old;
	{
		FWriteScopeLock WriteLock(Registry().Lock);
		Old = MoveTemp(Registry().Sinks);
		Registry().Sinks = MoveTemp(Next);
		Registry().Settings = Merged;
		DetailMask.store(static_cast<uint32>(Merged.Flags), std::memory_order_relaxed);
	}
	// Old goes here, after the lock is released
}

FVaRestTraceDetailSettings FVaRestTrace::GetDetailSettings()
{
	using namespace VaRest::Http::TracePrivate;
	FReadScopeLock ReadLock(Registry().Lock);
	return Registry().Settings;
}

void FVaRestTrace::AddSink(const FSinkRef& Sink)
{
	check(IsInGameThread());
	// IN-12: a late sink (Record after Pause, a module loading after the PreDefault core) learns what already exists.
	// A client the snapshot pinned can die only after this function returns: FClientState::ForEachLive releases its pins
	// from a later game-thread task, so that client's Client{Destroyed} reaches this sink after OnAttached (review M-9).
	Sink->OnAttached(BuildSnapshot());
	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Current = GetSinks();
	TSharedRef<FSinkList, ESPMode::ThreadSafe> Next = MakeShared<FSinkList, ESPMode::ThreadSafe>(Current.IsValid() ? *Current : FSinkList());
	Next->AddUnique(Sink);
	Swap(Next);
}

void FVaRestTrace::RemoveSink(const FSinkRef& Sink)
{
	check(IsInGameThread());
	TSharedPtr<const FSinkList, ESPMode::ThreadSafe> Current = GetSinks();
	if (!Current.IsValid())
	{
		return;
	}
	TSharedRef<FSinkList, ESPMode::ThreadSafe> Next = MakeShared<FSinkList, ESPMode::ThreadSafe>(*Current);
	Next->RemoveAll([&Sink](const FSinkRef& Existing) { return Existing == Sink; });
	Current.Reset();
	Swap(Next);
}

void FVaRestTrace::RefreshDetail()
{
	check(IsInGameThread());
	Swap(GetSinks());
}

TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe> FVaRestTrace::GetSinks()
{
	using namespace VaRest::Http::TracePrivate;
	FReadScopeLock ReadLock(Registry().Lock);
	return Registry().Sinks;
}

void FVaRestTrace::Emit(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload, EVaRestTraceSource Source)
{
	const TSharedPtr<const FSinkList, ESPMode::ThreadSafe> List = GetSinks();
	if (!List.IsValid() || List->Num() == 0)
	{
		return;
	}
	FVaRestTraceEvent Event;
	Event.Kind = Kind;
	Event.Source = Source;
	Event.Seq = VaRest::Http::TracePrivate::NextSeq.fetch_add(1, std::memory_order_relaxed);
	Event.Time = Time;
	Event.Payload = MoveTemp(Payload);
	++Counters().EventsBuilt;
	for (const FSinkRef& Sink : *List)
	{
		Sink->OnEvent(Event);
	}
}

TMulticastDelegate<void(FVaRestTraceSnapshot&)>& FVaRestTrace::OnBuildSnapshot()
{
	// Leaked on purpose (review I-4)
	static TMulticastDelegate<void(FVaRestTraceSnapshot&)>* Delegate = new TMulticastDelegate<void(FVaRestTraceSnapshot&)>();
	return *Delegate;
}

FVaRestTraceSnapshot FVaRestTrace::BuildSnapshot()
{
	check(IsInGameThread());
	FVaRestTraceSnapshot Snapshot;
	FVaRestTraceScope& Process = Snapshot.Scopes.AddDefaulted_GetRef();
	Process.ScopeId = ProcessScopeId;
	Process.Label = TEXT("Process");
	OnBuildSnapshot().Broadcast(Snapshot);
	return Snapshot;
}

uint64 FVaRestTrace::NewRequestId()
{
	return VaRest::Http::TracePrivate::NextRequest.fetch_add(1, std::memory_order_relaxed);
}

uint64 FVaRestTrace::NewClientId()
{
	return VaRest::Http::TracePrivate::NextClient.fetch_add(1, std::memory_order_relaxed);
}

FVaRestTraceCounters& FVaRestTrace::Counters()
{
	// Leaked on purpose (review I-4): atomics, but read by trace points that may run late
	static FVaRestTraceCounters* Instance = new FVaRestTraceCounters();
	return *Instance;
}

FVaRestScopedSinkIsolation::FVaRestScopedSinkIsolation()
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	{
		FWriteScopeLock WriteLock(Registry().Lock);
		Registry().Saved.Push(Registry().Sinks);
	}
	FVaRestTrace::Swap(nullptr);
}

FVaRestScopedSinkIsolation::~FVaRestScopedSinkIsolation()
{
	using namespace VaRest::Http::TracePrivate;
	check(IsInGameThread());
	TSharedPtr<const FVaRestTrace::FSinkList, ESPMode::ThreadSafe> Restored;
	{
		FWriteScopeLock WriteLock(Registry().Lock);
		Restored = Registry().Saved.Pop();
	}
	FVaRestTrace::Swap(MoveTemp(Restored));
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.TraceSurface.MaskAndSinks`, `.Isolation`, `.Snapshot`, `.Ids`.

- [ ] **Step 6: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestTrace.h Source/VaRestHttp/Private/VaRestTrace.cpp Source/VaRestHttp/Private/Tests/VaRestHttpTraceSurfaceTest.cpp
git commit -m "http: trace surface (Inspector 9) -- kinds, payloads, sinks, detail mask, snapshot, isolation, ids, counters

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 5: The redactor — URLs, headers, bodies, the known-secret scrub (IN-13, WebSocket §19 item 3)

Inspector §2.2: "**The rule.** A sink never receives a byte that was not redacted", and "Logs, the trace and HAR files therefore agree, and a fix to the rules fixes all of them." This task is that one redactor: layer 1 comes from `FVaRestRedactRules` (Task 1), layer 2's per-client keys are added to the rules a client builds (Task 14), layer 3 is `FVaRestSecretSet` plus the scrub, layer 4 is the binary descriptor, layer 5 is URL redaction. It is public because WebSocket §19 item 3 needs "public URL, header and JSON-excerpt redaction". Bodies are redacted on the core redaction pipe (Inspector §7: "Request and raw-response excerpt redaction | core redaction `FPipe`"), created here.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestRedactor.h`, `Source/VaRestHttp/Private/VaRestRedactor.cpp`, `Source/VaRestHttp/Private/VaRestRedactionPipe.h`
- Modify: `Source/VaRestHttp/Private/VaRestHttpModule.cpp` (start and stop the pipe)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpRedactorTest.cpp`

**Interfaces:**
- Consumes: `FVaRestRedactRules`, `EVaRestRedactKeyKind`, `VaRest::Json::RedactPrefix` (Task 1); `FVaRestHeaders`, `VaRest::Http::IsJsonContentType`, `FSha256` (Task 2); `PercentDecode` (Task 3); `FVaRestMultipartPart` (Task 3); `FVaRestTraceExcerpt`, `EVaRestExcerptKind`, `FVaRestTrace::Counters()` (Task 4).
- Produces: `FVaRestSecretSet` (`MinBytes = 8`, `MaxPrevious = 4`, `PreviousLifetime = 600.0`; `Set(FName Slot, const FString& Value, double Now)`, `ClearAll()`, `TArray<FString> GetActive(double Now) const`, `double NextExpiry(double Now) const`, `void Prune(double Now)`); `FVaRestRedactor` (`FVaRestRedactor()`, `FVaRestRedactor(FVaRestRedactRules, TArray<FString> Secrets)`, `GetRules()`, `GetSecrets()`, `RedactUrl(FStringView)`, `RedactHeaderValue(FStringView Name, FStringView Value)`, `RedactHeaders(const FVaRestHeaders&)`, `ScrubText(FStringView)`, `RedactBody(FStringView ContentType, TConstArrayView<uint8> Prefix, int64 TotalBytes, int32 Cap)`, `DescribeMultipart(TConstArrayView<FVaRestMultipartPart>, int64 TotalBytes, int32 Cap)`, `DescribeStream(FStringView FilePath, FStringView ContentType, int64 TotalBytes)`); private `UE::Tasks::FPipe* VaRest::Http::GetRedactionPipe()`, `StartRedactionPipe()`, `StopRedactionPipe()`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpRedactorTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/Paths.h"
#include "VaRestRedactionPipe.h"
#include "VaRestRedactor.h"
#include "VaRestSha256.h"

namespace VaRest::Http::RedactorTestPrivate
{
	TConstArrayView<uint8> Bytes(const char* Text)
	{
		return TConstArrayView<uint8>(reinterpret_cast<const uint8*>(Text), FCStringAnsi::Strlen(Text));
	}

	const TCHAR* const Bearer = TEXT("canary-bearer-0123456789");
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactUrlTest, "VaRest.Http.Redactor.Url",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactUrlTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });

	// Inspector 2.2 layers 1 and 5: userinfo, secret query keys (presigned URLs, OAuth code), fragments (implicit flow)
	TestEqualSensitive(TEXT("every URL secret class"),
		Redactor.RedactUrl(TEXT("https://user:pa55word@api.test/v1/items?token=canary-1&page=2&X-Amz-Signature=canary-2&code=canary-3#access_token=canary-4&state=ok")),
		FString(TEXT("https://***@api.test/v1/items?token=***&page=2&X-Amz-Signature=***&code=***#access_token=***&state=ok")));
	TestEqualSensitive(TEXT("keys are matched decoded; the written key stays as sent"),
		Redactor.RedactUrl(TEXT("https://h/x?%74oken=canary&a+b=1")), FString(TEXT("https://h/x?%74oken=***&a+b=1")));
	TestEqualSensitive(TEXT("a URL without secrets is unchanged"), Redactor.RedactUrl(TEXT("https://h/x/y?page=2")), FString(TEXT("https://h/x/y?page=2")));
	// Layer 3: a known secret in a path segment, which no key rule names (Inspector restore-the-bug R13)
	TestEqualSensitive(TEXT("a known secret in the path is scrubbed"),
		Redactor.RedactUrl(FString::Printf(TEXT("https://h/u/%s/x"), Bearer)), FString(TEXT("https://h/u/***/x")));
	TestEqualSensitive(TEXT("a relative URL works too"), Redactor.RedactUrl(TEXT("/users?password=x")), FString(TEXT("/users?password=***")));
	// Review C2: a "://" inside the query is not a scheme
	TestEqualSensitive(TEXT("a relative URL carrying a URL in its query"), Redactor.RedactUrl(TEXT("/cb?code=canary-r&redirect_uri=https://app.test/x")),
		FString(TEXT("/cb?code=***&redirect_uri=https://app.test/x")));
	TestEqualSensitive(TEXT("a protocol-relative URL's userinfo"), Redactor.RedactUrl(TEXT("//u:canary-u@h/x")), FString(TEXT("//***@h/x")));
	// Review M6: a secret percent-encoded by Path()/Query() (base64 +/=) is scrubbed in that form too
	const FVaRestRedactor Encoded(FVaRestRedactRules(), { TEXT("canary+b64/value==") });
	TestEqualSensitive(TEXT("a percent-encoded known secret"), Encoded.RedactUrl(TEXT("https://h/u/canary%2Bb64%2Fvalue%3D%3D/x")), FString(TEXT("https://h/u/***/x")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactHeadersTest, "VaRest.Http.Redactor.Headers",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactHeadersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });
	FVaRestHeaders In;
	In.Set(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), Bearer));
	In.Add(TEXT("Cookie"), TEXT("sid=canary-c"));
	In.Add(TEXT("Set-Cookie"), TEXT("sid=canary-s"));
	In.Add(TEXT("X-Api-Key"), TEXT("canary-k"));
	In.Add(TEXT("X-CSRF-Token"), TEXT("canary-x"));
	In.Add(TEXT("Idempotency-Key"), TEXT("3f2c"));
	In.Add(TEXT("Content-Type"), TEXT("application/json"));
	In.Add(TEXT("Location"), TEXT("https://app.test/cb?code=canary-code&state=1"));
	In.Add(TEXT("X-Echo"), FString::Printf(TEXT("seen %s"), Bearer));
	const FVaRestHeaders Out = Redactor.RedactHeaders(In);
	TestEqual(TEXT("names and order are kept"), Out.Num(), In.Num());
	for (const TCHAR* Name : { TEXT("Authorization"), TEXT("Cookie"), TEXT("Set-Cookie"), TEXT("X-Api-Key"), TEXT("X-CSRF-Token") })
	{
		TestEqualSensitive(FString::Printf(TEXT("%s is ***"), Name), Out.Get(Name), FString(TEXT("***")));
	}
	TestEqualSensitive(TEXT("Idempotency-Key is on the allowlist"), Out.Get(TEXT("Idempotency-Key")), FString(TEXT("3f2c")));
	TestEqualSensitive(TEXT("plain headers are kept"), Out.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestEqualSensitive(TEXT("Location is redacted as a URL (layer 5)"), Out.Get(TEXT("Location")), FString(TEXT("https://app.test/cb?code=***&state=1")));
	TestEqualSensitive(TEXT("a known secret in any value is scrubbed (layer 3)"), Out.Get(TEXT("X-Echo")), FString(TEXT("seen ***")));
	FVaRestHeaders Relative;
	Relative.Set(TEXT("Location"), TEXT("/cb?code=canary-rel&next=https://app.test/done"));
	TestEqualSensitive(TEXT("a relative Location with a URL in its query (review C2)"), Redactor.RedactHeaders(Relative).Get(TEXT("Location")),
		FString(TEXT("/cb?code=***&next=https://app.test/done")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactBodiesTest, "VaRest.Http.Redactor.Bodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });
	auto Run = [&Redactor](FStringView Type, const char* Body, int32 Cap = 4096)
	{
		const TConstArrayView<uint8> View = Bytes(Body);
		return Redactor.RedactBody(Type, View, View.Num(), Cap);
	};

	FVaRestTraceExcerpt E = Run(TEXT("application/json; charset=utf-8"), R"({"user":"neo","password":"canary-5"})");
	TestTrue(TEXT("JSON kind"), E.Kind == EVaRestExcerptKind::Json);
	TestEqualSensitive(TEXT("JSON keys redacted"), E.ToText(), FString(TEXT(R"({"user":"neo","password":"***"})")));
	TestTrue(TEXT("... redacted, whole"), E.bRedacted && !E.bTruncated && E.StopReason.IsEmpty() && E.TotalBytes == 36);

	TestTrue(TEXT("+json is JSON"), Run(TEXT("application/problem+json"), R"({"token":"x"})").ToText().Equals(TEXT(R"({"token":"***"})"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("text/* starting with '{' is sniffed as JSON"), Run(TEXT("text/plain"), R"(  {"secret":1})").Kind == EVaRestExcerptKind::Json);
	TestTrue(TEXT("no type starting with '[' is sniffed as JSON"), Run(TEXT(""), R"([{"jwt":"a.b.c"}])").ToText().Equals(TEXT(R"([{"jwt":"***"}])"), ESearchCase::CaseSensitive));

	const FVaRestTraceExcerpt Cut = Run(TEXT("application/json"), R"({"a":"0123456789","password":"canary-cut"})", 22);
	TestTrue(TEXT("a capped JSON body is truncated with reason cap"), Cut.bTruncated && Cut.StopReason == TEXT("cap"));
	TestFalse(TEXT("... and no canary"), Cut.ToText().Contains(TEXT("canary")));

	const FVaRestTraceExcerpt Form = Run(TEXT("application/x-www-form-urlencoded"), "a=1&password=canary-6&b=2");
	TestTrue(TEXT("form kind"), Form.Kind == EVaRestExcerptKind::Form);
	TestEqualSensitive(TEXT("form pairs by key"), Form.ToText(), FString(TEXT("a=1&password=***&b=2")));
	const TConstArrayView<uint8> FormBody = Bytes("a=1&b=22&c=333");
	TestEqualSensitive(TEXT("a capped form drops its cut last pair"),
		Redactor.RedactBody(TEXT("application/x-www-form-urlencoded"), FormBody.Left(10), FormBody.Num(), 10).ToText(), FString(TEXT("a=1&b=22")));

	const FVaRestTraceExcerpt Text = Run(TEXT("text/plain"), "hello canary-bearer-0123456789 world");
	TestEqualSensitive(TEXT("text is value-scrubbed"), Text.ToText(), FString(TEXT("hello *** world")));
	TestTrue(TEXT("... kind Text, redacted"), Text.Kind == EVaRestExcerptKind::Text && Text.bRedacted);
	const TConstArrayView<uint8> Utf8Text = Bytes("ab\xC3\xA9");
	TestEqualSensitive(TEXT("text is cut at a character boundary"), Redactor.RedactBody(TEXT("text/plain"), Utf8Text, 4, 3).ToText(), FString(TEXT("ab")));

	// Layer 4: binary is never copied; protobuf stores strings verbatim
	const uint8 Proto[] = { 0x0A, 0x08, 'p', 'a', 's', 's', 'w', 'o', 'r', 'd', 0x12, 0x02, 0x01, 0x02 };
	const FVaRestTraceExcerpt Binary = Redactor.RedactBody(TEXT("application/x-protobuf"), TConstArrayView<uint8>(Proto, 14), 14, 4096);
	TestTrue(TEXT("binary is a descriptor"), Binary.Kind == EVaRestExcerptKind::Binary && !Binary.ToText().Contains(TEXT("password")));
	TestEqualSensitive(TEXT("... with the SHA-256 prefix of the whole body"), Binary.Sha256Prefix, VaRest::Http::FSha256::Hash(TConstArrayView<uint8>(Proto, 14)).ToHex().Left(8));
	TestTrue(TEXT("... named in the text"), Binary.ToText().Contains(TEXT("14 bytes")) && Binary.ToText().Contains(Binary.Sha256Prefix));
	TestTrue(TEXT("a partial binary body gets no hash"), Redactor.RedactBody(TEXT("image/png"), TConstArrayView<uint8>(Proto, 4), 14, 4096).Sha256Prefix.IsEmpty());
	// Review I4: the protobuf above is valid UTF-8 without NUL; without a Content-Type it must still be a descriptor
	TestTrue(TEXT("untyped non-JSON is binary (layer 4)"), Redactor.RedactBody(TEXT(""), TConstArrayView<uint8>(Proto, 14), 14, 4096).Kind == EVaRestExcerptKind::Binary);
	// Review M5: the parser skips a UTF-8 BOM; the sniffing and the excerpt do too
	TestEqualSensitive(TEXT("a JSON body with a BOM"), Run(TEXT("application/json"), "\xEF\xBB\xBF{\"token\":\"canary-bom\"}").ToText(), FString(TEXT(R"({"token":"***"})")));
	// Review M6: a JSON string escapes '"' and '\\': the known secret is scrubbed in its escaped form too
	const FVaRestRedactor Escaping(FVaRestRedactRules(), { TEXT("can\"ary\\quote") });
	const TConstArrayView<uint8> Echo = Bytes(R"({"echo":"can\"ary\\quote"})");
	TestFalse(TEXT("a JSON-escaped known secret"), Escaping.RedactBody(TEXT("application/json"), Echo, Echo.Num(), 4096).ToText().Contains(TEXT("ary")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRedactPartsTest, "VaRest.Http.Redactor.MultipartAndStream",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRedactPartsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::RedactorTestPrivate;
	const FVaRestRedactor Redactor(FVaRestRedactRules(), { Bearer });
	FVaRestMultipart Multipart(TEXT("B"));
	Multipart.AddField(TEXT("name"), TEXT("neo")).AddField(TEXT("password"), TEXT("canary-7")).AddField(TEXT("note"), FString::Printf(TEXT("t=%s"), Bearer));
	Multipart.AddBytes(TEXT("avatar"), TEXT("a.png"), TEXT("image/png"), TArray<uint8>{ 1, 2, 3 });
	const FVaRestRequestBody Body = FVaRestRequestBody::FromMultipart(Multipart).GetValue();
	const FString Text = Redactor.DescribeMultipart(Body.PartDescriptors, Body.ContentLength, 4096).ToText();
	TestTrue(TEXT("a text part by name"), Text.Contains(TEXT("part \"name\": neo")));
	TestTrue(TEXT("a secret part name (layer 1)"), Text.Contains(TEXT("part \"password\": ***")));
	TestTrue(TEXT("a known secret in a text part (layer 3)"), Text.Contains(TEXT("part \"note\": t=***")));
	TestTrue(TEXT("a binary part as a descriptor"), Text.Contains(TEXT("part \"avatar\" filename=\"a.png\" image/png 3 B")));
	TestFalse(TEXT("no canary"), Text.Contains(TEXT("canary")));

	const FString Inside = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("up.bin"));
	const FVaRestTraceExcerpt Stream = Redactor.DescribeStream(Inside, TEXT("application/zip"), 99);
	TestTrue(TEXT("a streamed upload is a descriptor"), Stream.Kind == EVaRestExcerptKind::Stream);
	TestTrue(TEXT("... with the path relative to the project"), Stream.ToText().Contains(TEXT("Saved/up.bin")) && !Stream.ToText().Contains(FPaths::ConvertRelativePathToFull(FPaths::ProjectDir())));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSecretSetTest, "VaRest.Http.Redactor.SecretSet",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSecretSetTest::RunTest(const FString& Parameters)
{
	FVaRestSecretSet Set;
	Set.Set(TEXT("bearer"), TEXT("token-v1-abcdef"), 0.0);
	Set.Set(TEXT("short"), TEXT("1234567"), 0.0);
	TestEqual(TEXT("values shorter than 8 bytes are not scrubbed (they would hit ordinary text)"), Set.GetActive(0.0).Num(), 1);
	Set.Set(TEXT("bearer"), TEXT("token-v2-abcdef-longer"), 100.0);
	TArray<FString> Active = Set.GetActive(100.0);
	TestEqual(TEXT("after a rotation the old value stays known (Inspector D21)"), Active.Num(), 2);
	TestEqualSensitive(TEXT("longest first, so a value containing another is replaced whole"), Active[0], FString(TEXT("token-v2-abcdef-longer")));
	TestEqual(TEXT("next expiry is 10 minutes after the rotation"), Set.NextExpiry(100.0), 700.0);
	TestEqual(TEXT("... and then it is forgotten"), Set.GetActive(700.0).Num(), 1);
	TestTrue(TEXT("an expired value is no longer pending (review C1)"), Set.NextExpiry(700.0) < 0.0);
	for (int32 i = 0; i < 6; ++i)
	{
		Set.Set(TEXT("bearer"), FString::Printf(TEXT("rotated-token-%d"), i), 200.0 + i);
	}
	TestEqual(TEXT("current + at most 4 previous values"), Set.GetActive(206.0).Num(), 1 + FVaRestSecretSet::MaxPrevious);
	Set.ClearAll();
	TestEqual(TEXT("logout forgets everything"), Set.GetActive(206.0).Num(), 0);
	TestTrue(TEXT("... and nothing expires"), Set.NextExpiry(206.0) < 0.0);

	// Review M9: a process-wide counter -- let an earlier test's body log lines (the redaction pipe) land first
	if (UE::Tasks::FPipe* Pipe = VaRest::Http::GetRedactionPipe())
	{
		Pipe->WaitUntilEmpty();
	}
	const uint64 Before = FVaRestTrace::Counters().RedactorCalls;
	const FVaRestRedactor Redactor;
	(void)Redactor.RedactUrl(TEXT("https://h/x"));
	(void)Redactor.ScrubText(TEXT("x"));
	TestEqual(TEXT("each redactor call is counted (IN-11)"), FVaRestTrace::Counters().RedactorCalls.load(), Before + 2);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails on `VaRestRedactor.h` (no such file).

- [ ] **Step 3: The header** — `Source/VaRestHttp/Public/VaRestRedactor.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonRedact.h"
#include "VaRestTrace.h"

/**
 * A client's known secret values (Inspector 2.2 layer 3, IN-13): the current value per slot (bearer and refresh tokens,
 * the API key, Basic credentials, session headers) and, after a rotation, the previous values for PreviousLifetime
 * seconds, at most MaxPrevious of them (Inspector D21). Game thread; the client copies GetActive() into each redactor it
 * builds, so a published redactor never changes (copy-on-write).
 */
class VARESTHTTP_API FVaRestSecretSet
{
public:
	/** Shorter values are not scrubbed: they would match ordinary text */
	static constexpr int32 MinBytes = 8;
	static constexpr int32 MaxPrevious = 4;
	static constexpr double PreviousLifetime = 600.0;

	/** Sets Slot's value; the value it replaces stays known until Now + PreviousLifetime. An empty Value only retires the old one. */
	void Set(FName Slot, const FString& Value, double Now);
	/** Logout: forgets every value, the previous ones included (Inspector 12: "cleared on logout") */
	void ClearAll();
	/** The values to scrub at Now: current ones and unexpired previous ones of at least MinBytes UTF-8 bytes, longest first */
	TArray<FString> GetActive(double Now) const;
	/**
	 * When the next previous value still pending at Now expires; negative when none will. Values already expired at Now
	 * do not count: a timer re-armed for one would be due at once, forever (review C1).
	 */
	double NextExpiry(double Now) const;
	/** Forgets the previous values expired at Now */
	void Prune(double Now);

private:
	struct FPrevious
	{
		FString Value;
		double ExpiresAt = 0.0;
	};

	TMap<FName, FString> Current;
	TArray<FPrevious> Previous;
};

/**
 * The core redactor (Inspector 2.2, IN-13; WebSocket spec 19 item 3): URLs, headers and body excerpts, for logs and traces
 * alike. Immutable once built: a client publishes a new one when its rules or secrets change, and any thread may use a
 * published one. Bodies are never redacted on the game thread (Inspector 7): RedactBody and Describe* run on a background
 * task (VaRest::Http::GetRedactionPipe). What stays uncovered, as documented there: a secret that is not a known value and
 * sits in a path segment, in a text body under no key, or in opt-in binary.
 */
class VARESTHTTP_API FVaRestRedactor
{
public:
	FVaRestRedactor() = default;
	FVaRestRedactor(FVaRestRedactRules InRules, TArray<FString> InSecrets);

	const FVaRestRedactRules& GetRules() const { return Rules; }
	const TArray<FString>& GetSecrets() const { return Secrets; }

	/** Userinfo becomes ***; values of secret query and fragment keys become *** (keys matched percent-decoded); then the scrub */
	FString RedactUrl(FStringView Url) const;

	/** *** for a secret header name; Location and Content-Location are redacted as URLs; every other value is scrubbed */
	FString RedactHeaderValue(FStringView Name, FStringView Value) const;

	/** Every header through RedactHeaderValue; names and order kept */
	FVaRestHeaders RedactHeaders(const FVaRestHeaders& In) const;

	/** Every known secret value replaced by *** */
	FString ScrubText(FStringView Text) const;

	/**
	 * An excerpt of at most Cap bytes of a body whose first bytes are Prefix and whose size is TotalBytes. JSON (by type,
	 * or sniffed: a text/* type or none, and '{' or '[' first) through VaRest::Json::RedactPrefix; a form pair by pair,
	 * dropping a pair the cap may have cut; other text up to the cap, cut at a character boundary; anything else a
	 * descriptor (size, type, and the SHA-256 prefix when Prefix is the whole body). Then the value scrub.
	 */
	FVaRestTraceExcerpt RedactBody(FStringView ContentType, TConstArrayView<uint8> Prefix, int64 TotalBytes, int32 Cap) const;

	/** A multipart request body: one line per part; a text part's value by its name's rules, then scrubbed; binary as a descriptor */
	FVaRestTraceExcerpt DescribeMultipart(TConstArrayView<FVaRestMultipartPart> Parts, int64 TotalBytes, int32 Cap) const;

	/** A streamed file upload: its path (relative to the project when inside it), size and type */
	FVaRestTraceExcerpt DescribeStream(FStringView FilePath, FStringView ContentType, int64 TotalBytes) const;

private:
	FString RedactUrlUncounted(FStringView Url) const;
	void AppendRedactedPairs(FStringView Pairs, EVaRestRedactKeyKind Kind, FString& Out) const;
	bool ScrubInPlace(FString& Text) const;
	bool ScrubUtf8(TArray<uint8>& Utf8) const;

	FVaRestRedactRules Rules;
	/** Longest first */
	TArray<FString> Secrets;
	TArray<TArray<uint8>> SecretsUtf8;
};
```

`Source/VaRestHttp/Private/VaRestRedactionPipe.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Tasks/Pipe.h"

namespace VaRest::Http
{
	/**
	 * Core's redaction pipe (Inspector 7): body excerpts for logs (and M4's trace excerpts) are redacted there, in order,
	 * never on the game thread. Null before StartupModule and after ShutdownModule; callers skip body excerpts then.
	 * Game thread only (it checks): StopRedactionPipe deletes the pipe, so a launch from another thread could race the
	 * delete (review M-10). M4's decode-task excerpts must make it an atomic with a shutdown flag first.
	 */
	UE::Tasks::FPipe* GetRedactionPipe();
	void StartRedactionPipe();
	/** Waits for the queued work, then deletes the pipe: FPipe's destructor checks it is idle (Pipe.h:40-43) */
	void StopRedactionPipe();
}
```

- [ ] **Step 4: The implementation** — `Source/VaRestHttp/Private/VaRestRedactor.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRedactor.h"

#include "Misc/Paths.h"
#include "VaRestJsonUtf8.h"
#include "VaRestRedactionPipe.h"
#include "VaRestSha256.h"
#include "VaRestUrl.h"

namespace VaRest::Http::RedactorPrivate
{
	UE::Tasks::FPipe* Pipe = nullptr;

	TArray<uint8> ToUtf8(FStringView Text)
	{
		VaRest::Json::FUtf8Scratch Scratch;
		const FUtf8StringView Utf8 = VaRest::Json::ConvertForDocument(Text, Scratch, /*bWarn*/ false);
		return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.GetData()), Utf8.Len());
	}

	FString ToText(TConstArrayView<uint8> Bytes)
	{
		VaRest::Json::FUtf8Scratch Repaired;
		VaRest::Json::AppendRepairedUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()), Repaired);
		return FString::ConstructFromPtrSize(Repaired.GetData(), Repaired.Num());
	}

	/** The media type: before ';', trimmed */
	FStringView MediaType(FStringView ContentType)
	{
		int32 Semicolon = INDEX_NONE;
		return (ContentType.FindChar(TEXT(';'), Semicolon) ? ContentType.Left(Semicolon) : ContentType).TrimStartAndEnd();
	}

	uint8 FirstNonSpace(TConstArrayView<uint8> Bytes)
	{
		// Review M5: a UTF-8 BOM before the JSON is skipped, as the parser does
		if (Bytes.Num() >= 3 && Bytes[0] == 0xEF && Bytes[1] == 0xBB && Bytes[2] == 0xBF)
		{
			Bytes = Bytes.RightChop(3);
		}
		for (const uint8 Byte : Bytes)
		{
			if (Byte != ' ' && Byte != '\t' && Byte != '\r' && Byte != '\n')
			{
				return Byte;
			}
		}
		return 0;
	}

	bool IsTextual(FStringView Type)
	{
		return Type.StartsWith(TEXTVIEW("text/"), ESearchCase::IgnoreCase) || Type.Equals(TEXTVIEW("application/xml"), ESearchCase::IgnoreCase)
			|| Type.EndsWith(TEXTVIEW("+xml"), ESearchCase::IgnoreCase) || Type.Equals(TEXTVIEW("application/javascript"), ESearchCase::IgnoreCase);
	}

	/** Bytes up to the last whole UTF-8 character (a cut excerpt never ends in half a character) */
	int32 WholeCharacters(TConstArrayView<uint8> Bytes)
	{
		int32 End = Bytes.Num();
		int32 Back = 0;
		while (End - Back > 0 && Back < 3 && (Bytes[End - Back - 1] & 0xC0) == 0x80)
		{
			++Back;
		}
		if (End - Back <= 0)
		{
			return End;
		}
		const uint8 Lead = Bytes[End - Back - 1];
		const int32 Need = Lead >= 0xF0 ? 3 : Lead >= 0xE0 ? 2 : Lead >= 0xC0 ? 1 : 0;
		return Need > Back ? End - Back - 1 : End;
	}

	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Share(TArray<uint8>&& Bytes)
	{
		return MakeShared<TArray<uint8>, ESPMode::ThreadSafe>(MoveTemp(Bytes));
	}
}

UE::Tasks::FPipe* VaRest::Http::GetRedactionPipe()
{
	check(IsInGameThread()); // review M-10: only the game thread launches on it
	return RedactorPrivate::Pipe;
}

void VaRest::Http::StartRedactionPipe()
{
	check(!RedactorPrivate::Pipe);
	RedactorPrivate::Pipe = new UE::Tasks::FPipe(TEXT("VaRest.Redaction"));
}

void VaRest::Http::StopRedactionPipe()
{
	// ShutdownModule does not run in an `Automation RunTests ...; Quit` session (CLAUDE.md); a pipe left alive then is
	// leaked on purpose, and its destructor's idle check never runs
	if (UE::Tasks::FPipe* Existing = RedactorPrivate::Pipe)
	{
		RedactorPrivate::Pipe = nullptr;
		Existing->WaitUntilEmpty();
		delete Existing;
	}
}

void FVaRestSecretSet::Set(FName Slot, const FString& Value, double Now)
{
	if (const FString* Old = Current.Find(Slot))
	{
		if (Old->Equals(Value, ESearchCase::CaseSensitive))
		{
			return;
		}
		Previous.Add({ *Old, Now + PreviousLifetime });
		while (Previous.Num() > MaxPrevious)
		{
			Previous.RemoveAt(0);
		}
	}
	if (Value.IsEmpty())
	{
		Current.Remove(Slot);
	}
	else
	{
		Current.Add(Slot, Value);
	}
}

void FVaRestSecretSet::ClearAll()
{
	Current.Reset();
	Previous.Reset();
}

TArray<FString> FVaRestSecretSet::GetActive(double Now) const
{
	TArray<FString> Out;
	auto Consider = [&Out](const FString& Value)
	{
		if (VaRest::Json::Utf8Length(Value) >= MinBytes)
		{
			Out.AddUnique(Value);
		}
	};
	for (const TPair<FName, FString>& Pair : Current)
	{
		Consider(Pair.Value);
	}
	for (const FPrevious& Old : Previous)
	{
		if (Old.ExpiresAt > Now)
		{
			Consider(Old.Value);
		}
	}
	Out.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
	return Out;
}

double FVaRestSecretSet::NextExpiry(double Now) const
{
	double Next = -1.0;
	for (const FPrevious& Old : Previous)
	{
		if (Old.ExpiresAt > Now)
		{
			Next = Next < 0.0 ? Old.ExpiresAt : FMath::Min(Next, Old.ExpiresAt);
		}
	}
	return Next;
}

void FVaRestSecretSet::Prune(double Now)
{
	Previous.RemoveAll([Now](const FPrevious& Old) { return Old.ExpiresAt <= Now; });
}

FVaRestRedactor::FVaRestRedactor(FVaRestRedactRules InRules, TArray<FString> InSecrets)
	: Rules(MoveTemp(InRules))
{
	for (FString& Secret : InSecrets)
	{
		if (VaRest::Json::Utf8Length(Secret) >= FVaRestSecretSet::MinBytes)
		{
			// Review M6: the forms a secret takes on the wire -- percent-encoded by Path()/Query() (base64's + / =) and
			// JSON-escaped in a body ('"', '\') -- are needles too, when they differ
			const FString Percent = VaRest::Http::PercentEncode(Secret);
			FString Escaped = Secret.Replace(TEXT("\\"), TEXT("\\\\")).Replace(TEXT("\""), TEXT("\\\""));
			Secrets.AddUnique(Percent);
			Secrets.AddUnique(MoveTemp(Escaped));
			Secrets.AddUnique(MoveTemp(Secret));
		}
	}
	Secrets.Sort([](const FString& A, const FString& B) { return A.Len() > B.Len(); });
	for (const FString& Secret : Secrets)
	{
		SecretsUtf8.Add(VaRest::Http::RedactorPrivate::ToUtf8(Secret));
	}
}

bool FVaRestRedactor::ScrubInPlace(FString& Text) const
{
	bool bAny = false;
	for (const FString& Secret : Secrets)
	{
		bAny |= Text.ReplaceInline(*Secret, TEXT("***"), ESearchCase::CaseSensitive) > 0;
	}
	return bAny;
}

bool FVaRestRedactor::ScrubUtf8(TArray<uint8>& Utf8) const
{
	// "a memmem of a few needles over at most the cap" (Inspector 2.2 layer 3)
	bool bAny = false;
	for (const TArray<uint8>& Needle : SecretsUtf8)
	{
		if (Needle.Num() == 0 || Needle.Num() > Utf8.Num())
		{
			continue;
		}
		TArray<uint8> Out;
		bool bHit = false;
		int32 i = 0;
		while (i < Utf8.Num())
		{
			if (i + Needle.Num() <= Utf8.Num() && FMemory::Memcmp(Utf8.GetData() + i, Needle.GetData(), Needle.Num()) == 0)
			{
				Out.Append(reinterpret_cast<const uint8*>("***"), 3);
				i += Needle.Num();
				bHit = true;
			}
			else
			{
				Out.Add(Utf8[i++]);
			}
		}
		if (bHit)
		{
			Utf8 = MoveTemp(Out);
			bAny = true;
		}
	}
	return bAny;
}

FString FVaRestRedactor::ScrubText(FStringView Text) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	FString Out(Text);
	ScrubInPlace(Out);
	return Out;
}

void FVaRestRedactor::AppendRedactedPairs(FStringView Pairs, EVaRestRedactKeyKind Kind, FString& Out) const
{
	int32 Start = 0;
	bool bFirst = true;
	while (Start <= Pairs.Len())
	{
		int32 End = Start;
		while (End < Pairs.Len() && Pairs[End] != TEXT('&'))
		{
			++End;
		}
		const FStringView Piece = Pairs.Mid(Start, End - Start);
		if (!bFirst)
		{
			Out.AppendChar(TEXT('&'));
		}
		bFirst = false;
		int32 Equals = INDEX_NONE;
		if (Piece.FindChar(TEXT('='), Equals) && Rules.ShouldRedact(FStringView(VaRest::Http::PercentDecode(Piece.Left(Equals), true)), Kind))
		{
			Out += Piece.Left(Equals);
			Out += TEXT("=***");
		}
		else
		{
			Out += Piece;
		}
		Start = End + 1;
	}
}

FString FVaRestRedactor::RedactUrlUncounted(FStringView Url) const
{
	FString Out;
	Out.Reserve(Url.Len());
	int32 Pos = 0;
	// Review C2: "://" ends a scheme only when everything before it is one (RFC 3986: ALPHA *( ALPHA / DIGIT / "+" /
	// "-" / "." )). Otherwise "/cb?code=SECRET&redirect_uri=https://app/done" would take its whole query for a scheme
	// and copy it verbatim. A leading "//" (protocol-relative) starts an authority too.
	int32 SchemeEnd = INDEX_NONE;
	for (int32 i = 0; i < Url.Len(); ++i)
	{
		const TCHAR C = Url[i];
		if (C == TEXT(':'))
		{
			SchemeEnd = (i > 0 && Url.Mid(i).StartsWith(TEXTVIEW("://"))) ? i : INDEX_NONE;
			break;
		}
		if (!(FChar::IsAlpha(C) || (i > 0 && (FChar::IsDigit(C) || C == TEXT('+') || C == TEXT('-') || C == TEXT('.')))))
		{
			break;
		}
	}
	const int32 AuthorityStart = SchemeEnd != INDEX_NONE ? SchemeEnd + 3 : (Url.StartsWith(TEXTVIEW("//")) ? 2 : INDEX_NONE);
	if (AuthorityStart != INDEX_NONE)
	{
		int32 AuthorityEnd = Url.Len();
		for (int32 i = AuthorityStart; i < Url.Len(); ++i)
		{
			if (Url[i] == TEXT('/') || Url[i] == TEXT('?') || Url[i] == TEXT('#'))
			{
				AuthorityEnd = i;
				break;
			}
		}
		Out += Url.Left(AuthorityStart);
		const FStringView Authority = Url.Mid(AuthorityStart, AuthorityEnd - AuthorityStart);
		int32 At = INDEX_NONE;
		if (Authority.FindLastChar(TEXT('@'), At))
		{
			// Layer 5: userinfo (user:pass@) is redacted whole
			Out += TEXT("***@");
			Out += Authority.RightChop(At + 1);
		}
		else
		{
			Out += Authority;
		}
		Pos = AuthorityEnd;
	}
	int32 QueryStart = INDEX_NONE;
	int32 FragmentStart = INDEX_NONE;
	for (int32 i = Pos; i < Url.Len(); ++i)
	{
		if (Url[i] == TEXT('#'))
		{
			FragmentStart = i;
			break;
		}
		if (Url[i] == TEXT('?') && QueryStart == INDEX_NONE)
		{
			QueryStart = i;
		}
	}
	const int32 PathEnd = QueryStart != INDEX_NONE ? QueryStart : FragmentStart != INDEX_NONE ? FragmentStart : Url.Len();
	Out += Url.Mid(Pos, PathEnd - Pos);
	if (QueryStart != INDEX_NONE)
	{
		const int32 QueryEnd = FragmentStart != INDEX_NONE ? FragmentStart : Url.Len();
		Out.AppendChar(TEXT('?'));
		AppendRedactedPairs(Url.Mid(QueryStart + 1, QueryEnd - QueryStart - 1), EVaRestRedactKeyKind::Query, Out);
	}
	if (FragmentStart != INDEX_NONE)
	{
		// OAuth's implicit flow returns #access_token=...: a fragment is redacted like a query
		Out.AppendChar(TEXT('#'));
		AppendRedactedPairs(Url.RightChop(FragmentStart + 1), EVaRestRedactKeyKind::Query, Out);
	}
	ScrubInPlace(Out);
	return Out;
}

FString FVaRestRedactor::RedactUrl(FStringView Url) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	return RedactUrlUncounted(Url);
}

FString FVaRestRedactor::RedactHeaderValue(FStringView Name, FStringView Value) const
{
	++FVaRestTrace::Counters().RedactorCalls;
	if (Rules.ShouldRedact(Name, EVaRestRedactKeyKind::Header))
	{
		return TEXT("***");
	}
	if (Name.Equals(TEXTVIEW("Location"), ESearchCase::IgnoreCase) || Name.Equals(TEXTVIEW("Content-Location"), ESearchCase::IgnoreCase))
	{
		return RedactUrlUncounted(Value);
	}
	FString Out(Value);
	ScrubInPlace(Out);
	return Out;
}

FVaRestHeaders FVaRestRedactor::RedactHeaders(const FVaRestHeaders& In) const
{
	FVaRestHeaders Out;
	for (const FVaRestHeaders::FPair& Pair : In)
	{
		Out.Add(Pair.Key, RedactHeaderValue(Pair.Key, Pair.Value));
	}
	return Out;
}

FVaRestTraceExcerpt FVaRestRedactor::RedactBody(FStringView ContentType, TConstArrayView<uint8> Prefix, int64 TotalBytes, int32 Cap) const
{
	using namespace VaRest::Http::RedactorPrivate;
	++FVaRestTrace::Counters().RedactorCalls;
	FVaRestTraceExcerpt Excerpt;
	Excerpt.TotalBytes = TotalBytes;
	const int32 Seen = FMath::Min(Prefix.Num(), FMath::Max(Cap, 0));
	const TConstArrayView<uint8> Head = Prefix.Left(Seen);
	const bool bWhole = Seen == TotalBytes;
	const FStringView Type = MediaType(ContentType);
	const uint8 First = FirstNonSpace(Head);
	const bool bSniffJson = (Type.IsEmpty() || Type.StartsWith(TEXTVIEW("text/"), ESearchCase::IgnoreCase)) && (First == '{' || First == '[');
	TArray<uint8> Out;

	if (VaRest::Http::IsJsonContentType(Type) || bSniffJson)
	{
		Excerpt.Kind = EVaRestExcerptKind::Json;
		const FVaRestRedactResult Result = VaRest::Json::RedactPrefix(
			FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Head.GetData()), Head.Num()), Rules, EVaRestJsonFormat::Condensed, Out, bWhole);
		Excerpt.bRedacted = Result.bRedacted;
		Excerpt.bTruncated = !bWhole || Result.StopReason != EVaRestRedactStop::Complete;
		Excerpt.StopReason = Result.StopReason == EVaRestRedactStop::Complete ? FString()
			: Result.StopReason == EVaRestRedactStop::Cap ? FString(TEXT("cap"))
			: FString::Printf(TEXT("parse error at byte %lld: %s"), Result.ErrorOffset, *Result.Error);
	}
	else if (Type.Equals(TEXTVIEW("application/x-www-form-urlencoded"), ESearchCase::IgnoreCase))
	{
		Excerpt.Kind = EVaRestExcerptKind::Form;
		FString Text = ToText(Head);
		if (!bWhole)
		{
			// The last pair may be cut by the cap: a cut token is never written (Inspector 2.2)
			int32 LastAmp = INDEX_NONE;
			if (Text.FindLastChar(TEXT('&'), LastAmp))
			{
				Text.LeftInline(LastAmp);
			}
			else
			{
				Text.Reset();
			}
			Excerpt.bTruncated = true;
			Excerpt.StopReason = TEXT("cap");
		}
		FString Redacted;
		AppendRedactedPairs(Text, EVaRestRedactKeyKind::Form, Redacted);
		Excerpt.bRedacted = !Redacted.Equals(Text, ESearchCase::CaseSensitive);
		Out = ToUtf8(Redacted);
	}
	else if (Type.StartsWith(TEXTVIEW("multipart/"), ESearchCase::IgnoreCase))
	{
		Excerpt.Kind = EVaRestExcerptKind::Multipart;
		Out = ToUtf8(FString::Printf(TEXT("<multipart body, %lld bytes>"), TotalBytes));
	}
	else if (IsTextual(Type))
	{
		Excerpt.Kind = EVaRestExcerptKind::Text;
		const TConstArrayView<uint8> Whole = bWhole ? Head : Head.Left(WholeCharacters(Head));
		Out = ToUtf8(ToText(Whole));
		Excerpt.bTruncated = !bWhole;
		Excerpt.StopReason = bWhole ? FString() : FString(TEXT("cap"));
	}
	else
	{
		// Layer 4: binary is a descriptor. Protobuf and FlatBuffers store strings verbatim, so no byte is copied.
		Excerpt.Kind = EVaRestExcerptKind::Binary;
		if (Prefix.Num() == TotalBytes)
		{
			Excerpt.Sha256Prefix = VaRest::Http::FSha256::Hash(Prefix).ToHex().Left(8);
		}
		Out = ToUtf8(FString::Printf(TEXT("<binary: %lld bytes, %s%s%s>"), TotalBytes,
			Type.IsEmpty() ? TEXT("no content type") : *FString(Type),
			Excerpt.Sha256Prefix.IsEmpty() ? TEXT("") : TEXT(", sha256 "), *Excerpt.Sha256Prefix));
		Excerpt.Bytes = Share(MoveTemp(Out));
		return Excerpt;
	}
	Excerpt.bRedacted |= ScrubUtf8(Out);
	Excerpt.Bytes = Share(MoveTemp(Out));
	return Excerpt;
}

FVaRestTraceExcerpt FVaRestRedactor::DescribeMultipart(TConstArrayView<FVaRestMultipartPart> Parts, int64 TotalBytes, int32 Cap) const
{
	using namespace VaRest::Http::RedactorPrivate;
	++FVaRestTrace::Counters().RedactorCalls;
	FVaRestTraceExcerpt Excerpt;
	Excerpt.Kind = EVaRestExcerptKind::Multipart;
	Excerpt.TotalBytes = TotalBytes;
	FString Text;
	for (const FVaRestMultipartPart& Part : Parts)
	{
		if (!Text.IsEmpty())
		{
			Text.AppendChar(TEXT('\n'));
		}
		if (Part.bField)
		{
			const bool bSecret = Rules.ShouldRedact(FStringView(Part.Name), EVaRestRedactKeyKind::Part);
			Excerpt.bRedacted |= bSecret;
			Text += FString::Printf(TEXT("part \"%s\": %s"), *Part.Name, bSecret ? TEXT("***") : *ToText(Part.Bytes));
		}
		else
		{
			Text += FString::Printf(TEXT("part \"%s\" filename=\"%s\" %s %lld B"), *Part.Name, *Part.FileName, *Part.ContentType, Part.Size);
		}
	}
	Excerpt.bRedacted |= ScrubInPlace(Text);
	TArray<uint8> Out = ToUtf8(Text);
	if (Cap >= 0 && Out.Num() > Cap)
	{
		Out.SetNum(WholeCharacters(TConstArrayView<uint8>(Out).Left(Cap)));
		Excerpt.bTruncated = true;
		Excerpt.StopReason = TEXT("cap");
	}
	Excerpt.Bytes = Share(MoveTemp(Out));
	return Excerpt;
}

FVaRestTraceExcerpt FVaRestRedactor::DescribeStream(FStringView FilePath, FStringView ContentType, int64 TotalBytes) const
{
	using namespace VaRest::Http::RedactorPrivate;
	++FVaRestTrace::Counters().RedactorCalls;
	FString Path = FPaths::ConvertRelativePathToFull(FString(FilePath));
	const FString Project = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
	if (FPaths::IsUnderDirectory(Path, Project))
	{
		// Inspector 2.2: "path relative to the project when inside it" -- a user's home directory is not in the excerpt
		FPaths::MakePathRelativeTo(Path, *Project);
	}
	FString Text = FString::Printf(TEXT("<file upload: %s, %lld bytes, %s>"), *Path, TotalBytes, *FString(ContentType));
	FVaRestTraceExcerpt Excerpt;
	Excerpt.Kind = EVaRestExcerptKind::Stream;
	Excerpt.TotalBytes = TotalBytes;
	Excerpt.bRedacted = ScrubInPlace(Text);
	Excerpt.Bytes = Share(ToUtf8(Text));
	return Excerpt;
}
```

- [ ] **Step 5: The pipe's lifetime** — in `Source/VaRestHttp/Private/VaRestHttpModule.cpp`, add `#include "VaRestRedactionPipe.h"` and make the two bodies:

```cpp
	virtual void StartupModule() override
	{
		VaRest::Http::StartRedactionPipe();
	}

	virtual void ShutdownModule() override
	{
		VaRest::Http::StopRedactionPipe();
	}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including the five `VaRest.Http.Redactor.*` tests.

- [ ] **Step 7: Restore-the-bug (value scrub, Inspector R13)**

Make `FVaRestRedactor::ScrubInPlace` temporarily `return false;` without replacing. Run `Tools/run_tests.sh VaRest.Http.Redactor`; expected FAIL in `VaRest.Http.Redactor.Url` ("a known secret in the path is scrubbed", "a percent-encoded known secret"), `VaRest.Http.Redactor.Headers` ("a known secret in any value") and `VaRest.Http.Redactor.MultipartAndStream` (the `t=***` text field). Restore; PASS. Report both runs.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestRedactor.h Source/VaRestHttp/Private/VaRestRedactor.cpp Source/VaRestHttp/Private/VaRestRedactionPipe.h \
  Source/VaRestHttp/Private/VaRestHttpModule.cpp Source/VaRestHttp/Private/Tests/VaRestHttpRedactorTest.cpp
git commit -m "http: the core redactor (IN-13) -- URLs, headers, bodies, known-secret scrub, secret rotation; redaction pipe

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 6: Clock, transport interface, fake transport, test helpers (spec §4.1, §4.7, §7.1)

Spec §7.1: "`VaRest.Http.*` on the fake transport and virtual clock". §4.1: "Timers run on the core ticker via an injectable clock (`IVaRestClock`), so tests run on virtual time." The clock lands here, with timers (decision 1); M4 adds backoff on top. `IVaRestTransport` is the stage chain's last step (spec §4: "Transport is `IVaRestTransport` (default: `FHttpModule`); tests inject a fake"). The fake mirrors what the real transport does that the core relies on: completion on another thread, a body with one byte of slack, the engine's timeout mapping (a total timeout is `TimedOut`, an activity timeout a connection error, `HttpRequestCommon.cpp:299-305`), progress written into a probe, the response written to a file for `SaveTo`, and delegate binds counted for IN-11. The fake and the virtual clock are public: the Inspector (I1) and WebSocket (W1) tests use them too.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestClock.h`, `Source/VaRestHttp/Private/VaRestClock.cpp`
- Create: `Source/VaRestHttp/Public/VaRestTransport.h`
- Create: `Source/VaRestHttp/Public/VaRestFakeTransport.h`, `Source/VaRestHttp/Private/VaRestFakeTransport.cpp`
- Create: `Source/VaRestHttp/Public/VaRestHttpTesting.h`, `Source/VaRestHttp/Private/VaRestHttpTesting.cpp`
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpClockTest.cpp`, `Source/VaRestHttp/Private/Tests/VaRestHttpFakeTransportTest.cpp`

**Interfaces:**
- Consumes: `FVaRestHeaders`, `EVaRestPriority`, `EVaRestTransportFailure` (Task 2); `FVaRestBodySegment`, `FVaRestSegmentStream` (Task 3); `FVaRestTrace::Counters()` (Task 4); `VaRest::Http::GetRedactionPipe()` (Task 5).
- Produces: `struct FVaRestTimerHandle { uint64 Id; IsValid(); Invalidate(); }`; `uint64 VaRest::Http::NewTimerId()` (process-wide, any thread); `class IVaRestClock { double Now() const; FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback); void ClearTimer(FVaRestTimerHandle&); }`; `FVaRestRealClock` (`static TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Get()`); `FVaRestVirtualClock` (integer nanoseconds inside; `Advance(double)`, `NumPendingTimers()`, `static int64 ToNanos(double)`); `struct FVaRestTransportProbe { atomics BytesSent, BytesReceived, UploadTotal, DownloadTotal (-1), UploadDoneAt (-1), StatusAt (-1) }`; `struct FVaRestTransportRequest { RequestId, Attempt, Verb, Url, Headers, Body, BodyFile, BodySegments, Timeout, ActivityTimeout, Priority, ResponseFile, MaxBodyBytes, bReportProgress, bReportStatusTime, Probe, Clock }`; `struct FVaRestTransportResult { Status, Headers, EffectiveUrl, ContentType, Body, bBodyInFile, Failure, FailureDetail, BytesUp, BytesDown, TransportDoneAt }`; `class IVaRestTransportCall { void Cancel(); }`; `using FVaRestTransportComplete = TUniqueFunction<void(FVaRestTransportResult&&)>`; `class IVaRestTransport { TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&&, FVaRestTransportComplete&&); }`; `struct FVaRestFakeResponse { Status, Headers, Body, EffectiveUrl, Failure, FailureDetail, Delay; static Json(int32, FUtf8StringView), Text(int32, FStringView, FStringView ContentType), Bytes(int32, TArray<uint8>, FStringView ContentType), Failed(EVaRestTransportFailure, FName), Manual() }`; `FVaRestFakeTransport` (`explicit FVaRestFakeTransport(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe>)`, `SetResponder`, `Enqueue`, `bCompleteOnWorker`, `struct FCall { Request, StartedAt, bCompleted, bCanceled, BodyData, BodyNum }`, `NumCalls`, `GetCall`, `NumInFlight`, `NumCanceled`, `ReadBody`, `Complete`, `ReportProgress`, `Drop`; `WITH_DEV_AUTOMATION_TESTS` only); `namespace VaRest::Http::Testing { bool PumpUntil(TFunctionRef<bool()>, double TimeoutSeconds = 5.0, bool bTickCoreTicker = false); void FlushRedactionPipe(); FString GetTestServerUrl(); struct FTestCounters { DecodesOnGameThread, DecodesOffGameThread, ClientStatesDestroyedOffGameThread }; FTestCounters& Counters(); (both !UE_BUILD_SHIPPING); class FLogCapture { Contains, Count, GetLines }; }`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpClockTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestClock.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpVirtualClockTest, "VaRest.Http.Clock.Virtual",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpVirtualClockTest::RunTest(const FString& Parameters)
{
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	TArray<FString> Fired;
	Clock->SetTimer(2.0, [&Fired] { Fired.Add(TEXT("A")); });
	Clock->SetTimer(1.0, [&Fired] { Fired.Add(TEXT("B")); });
	Clock->SetTimer(1.0, [&Fired, &Clock] {
		Fired.Add(TEXT("C"));
		Clock->SetTimer(0.0, [&Fired] { Fired.Add(TEXT("C+0")); }); // due now: fires in this same Advance
	});
	FVaRestTimerHandle Dropped = Clock->SetTimer(1.5, [&Fired] { Fired.Add(TEXT("dropped")); });
	TestEqual(TEXT("nothing fires before Advance"), Fired.Num(), 0);
	Clock->Advance(0.5);
	TestEqual(TEXT("nothing is due at 0.5"), Fired.Num(), 0);
	Clock->ClearTimer(Dropped);
	TestFalse(TEXT("ClearTimer invalidates the handle"), Dropped.IsValid());
	Clock->Advance(0.5);
	TestEqualSensitive(TEXT("due order, ties in creation order, timers set while firing included"), FString::Join(Fired, TEXT(",")), FString(TEXT("B,C,C+0")));
	TestEqual(TEXT("Now is the target time"), Clock->Now(), 1.0);
	Clock->Advance(5.0);
	TestEqualSensitive(TEXT("a cleared timer never fires"), FString::Join(Fired, TEXT(",")), FString(TEXT("B,C,C+0,A")));
	TestEqual(TEXT("Now"), Clock->Now(), 6.0);
	TestEqual(TEXT("nothing pending"), Clock->NumPendingTimers(), 0);

	// Review I1: a tick re-armed at Now() + 0.1 must fall due exactly when 0.1 + 0.1 + 0.05 + 0.05 is reached
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Ticking = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	int32 Ticks = 0;
	TFunction<void()> Arm;
	Arm = [&Arm, &Ticks, &Ticking] { ++Ticks; Ticking->SetTimer(0.1, [&Arm] { Arm(); }); };
	Ticking->SetTimer(0.1, [&Arm] { Arm(); });
	Ticking->Advance(0.1);
	Ticking->Advance(0.1);
	Ticking->Advance(0.05);
	Ticking->Advance(0.05);
	TestEqual(TEXT("three ticks by 0.3: integer nanoseconds, no drift"), Ticks, 3);
	TestEqual(TEXT("... and Now is 0.3 as written"), Ticking->Now(), 0.3);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRealClockTest, "VaRest.Http.Clock.Real",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpRealClockTest::RunTest(const FString& Parameters)
{
	TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Clock = FVaRestRealClock::Get();
	TSharedRef<bool, ESPMode::ThreadSafe> bFired = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedRef<bool, ESPMode::ThreadSafe> bCleared = MakeShared<bool, ESPMode::ThreadSafe>(false);
	const double Start = Clock->Now();
	Clock->SetTimer(0.05, [bFired] { *bFired = IsInGameThread(); });
	FVaRestTimerHandle Never = Clock->SetTimer(0.05, [bCleared] { *bCleared = true; });
	Clock->ClearTimer(Never);
	TestTrue(TEXT("the real clock's timer fires on the game thread, from the core ticker"),
		VaRest::Http::Testing::PumpUntil([bFired] { return *bFired; }, 2.0, /*bTickCoreTicker*/ true));
	TestTrue(TEXT("... after its delay"), Clock->Now() - Start >= 0.05);
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.1, true);
	TestFalse(TEXT("a cleared timer does not fire"), *bCleared);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCaptureTest, "VaRest.Http.Testing.LogCapture",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCaptureTest::RunTest(const FString& Parameters)
{
	VaRest::Http::Testing::FLogCapture Capture;
	UE_LOG(LogVaRestHttp, Log, TEXT("capture-probe game thread"));
	// Not Wait(): a waited task may be retracted and run on the waiting thread (TaskPrivate.cpp:75-100), which is the game thread here
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bLogged = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.Log"), [bLogged] { UE_LOG(LogVaRestHttp, Log, TEXT("capture-probe worker")); *bLogged = true; });
	VaRest::Http::Testing::PumpUntil([bLogged] { return bLogged->load(); });
	TestTrue(TEXT("game-thread lines are captured"), Capture.Contains(TEXT("capture-probe game thread")));
	TestTrue(TEXT("worker lines are captured at once (CanBeUsedOnAnyThread)"), Capture.Contains(TEXT("capture-probe worker")));
	TestEqual(TEXT("Count"), Capture.Count(TEXT("capture-probe")), 2);
	return true;
}

#endif
```

`Source/VaRestHttp/Private/Tests/VaRestHttpFakeTransportTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestFakeTransport.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::FakeTestPrivate
{
	/** Written on a worker, polled on the game thread: bDone is atomic and set last (review M17) */
	struct FDone
	{
		std::atomic<bool> bDone{ false };
		bool bOnGameThread = true;
		FVaRestTransportResult Result;
	};
	using FDoneRef = TSharedRef<FDone, ESPMode::ThreadSafe>;

	FVaRestTransportComplete Into(const FDoneRef& Done)
	{
		return [Done](FVaRestTransportResult&& Result)
		{
			Done->bOnGameThread = IsInGameThread();
			Done->Result = MoveTemp(Result);
			Done->bDone = true;
		};
	}

	FVaRestTransportRequest Get(FStringView Url)
	{
		FVaRestTransportRequest Request;
		Request.Verb = TEXT("GET");
		Request.Url = FString(Url);
		return Request;
	}

	struct FRig
	{
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
		TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake = MakeShared<FVaRestFakeTransport, ESPMode::ThreadSafe>(Clock);
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFakeRespondsTest, "VaRest.Http.Fake.Responds",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFakeRespondsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::FakeTestPrivate;
	FRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(201, FUtf8StringView(UTF8TEXT(R"({"id":7})"))));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return FVaRestFakeResponse::Text(404, Request.Url); });

	FDoneRef First = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request = Get(TEXT("https://api.test/a"));
	Request.Headers.Set(TEXT("X-Test"), TEXT("1"));
	Rig.Fake->Start(MoveTemp(Request), Into(First));
	TestTrue(TEXT("completes"), VaRest::Http::Testing::PumpUntil([First] { return First->bDone.load(); }));
	TestFalse(TEXT("off the game thread, as the HTTP thread completes"), First->bOnGameThread);
	TestEqual(TEXT("the enqueued response first"), First->Result.Status, 201);
	TestEqualSensitive(TEXT("content type from the headers"), First->Result.ContentType, FString(TEXT("application/json")));
	TestTrue(TEXT("the body has one byte of slack for ParseOwned (spec 2.1)"), First->Result.Body.Max() > First->Result.Body.Num());
	TestTrue(TEXT("... and the fake remembers its buffer"), Rig.Fake->GetCall(0).BodyData == First->Result.Body.GetData());
	TestEqualSensitive(TEXT("the effective URL defaults to the request URL"), First->Result.EffectiveUrl, FString(TEXT("https://api.test/a")));
	TestEqualSensitive(TEXT("the call is recorded"), Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("x-test")), FString(TEXT("1")));

	FDoneRef Second = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/b")), Into(Second));
	VaRest::Http::Testing::PumpUntil([Second] { return Second->bDone.load(); });
	TestEqual(TEXT("then the responder"), Second->Result.Status, 404);
	TestEqual(TEXT("two calls"), Rig.Fake->NumCalls(), 2);

	Rig.Fake->bCompleteOnWorker = false;
	FDoneRef Inline = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/c")), Into(Inline));
	TestTrue(TEXT("bCompleteOnWorker = false completes inside Start, on the calling thread"), Inline->bDone && Inline->bOnGameThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFakeTimingTest, "VaRest.Http.Fake.VirtualTime",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFakeTimingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::FakeTestPrivate;
	FRig Rig;
	Rig.Fake->bCompleteOnWorker = false;

	FVaRestFakeResponse Later = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	Later.Delay = 2.0;
	Rig.Fake->Enqueue(Later);
	FDoneRef Delayed = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/delay")), Into(Delayed));
	Rig.Clock->Advance(1.9);
	TestFalse(TEXT("not before its delay"), Delayed->bDone);
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("at its delay"), Delayed->bDone && Delayed->Result.Status == 200);

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Manual = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/manual")), Into(Manual));
	Rig.Clock->Advance(100.0);
	TestFalse(TEXT("a manual call waits for Complete"), Manual->bDone);
	TestEqual(TEXT("one in flight"), Rig.Fake->NumInFlight(), 1);
	Rig.Fake->Complete(1, FVaRestFakeResponse::Text(503, TEXT("busy")));
	TestTrue(TEXT("Complete"), Manual->bDone && Manual->Result.Status == 503);

	// The engine: a total timeout is TimedOut, an activity timeout a connection error (HttpRequestCommon.cpp:299-305)
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Total = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest WithTimeout = Get(TEXT("https://api.test/slow"));
	WithTimeout.Timeout = 1.f;
	Rig.Fake->Start(MoveTemp(WithTimeout), Into(Total));
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("the total timeout"), Total->bDone && Total->Result.Failure == EVaRestTransportFailure::Timeout);
	Rig.Fake->Complete(2, FVaRestFakeResponse::Text(200, TEXT("late")));
	TestEqual(TEXT("a late Complete is ignored"), Total->Result.Status, 0);

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Idle = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest WithActivity = Get(TEXT("https://api.test/drip"));
	WithActivity.ActivityTimeout = 2.f;
	Rig.Fake->Start(MoveTemp(WithActivity), Into(Idle));
	Rig.Clock->Advance(1.5);
	Rig.Fake->ReportProgress(3, 0, 100);
	Rig.Clock->Advance(1.5);
	TestFalse(TEXT("progress resets the activity timeout"), Idle->bDone);
	TestEqual(TEXT("progress lands in the probe"), Rig.Fake->GetCall(3).Request.Probe->BytesReceived.load(), static_cast<int64>(100));
	Rig.Clock->Advance(0.6);
	TestTrue(TEXT("no activity for the activity timeout: a connection failure"), Idle->bDone && Idle->Result.Failure == EVaRestTransportFailure::Connection
		&& Idle->Result.FailureDetail == FName(TEXT("ActivityTimeout")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFakeCancelTest, "VaRest.Http.Fake.CancelAndFiles",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFakeCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::FakeTestPrivate;
	FRig Rig;
	Rig.Fake->bCompleteOnWorker = false;

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Canceled = MakeShared<FDone, ESPMode::ThreadSafe>();
	TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Call = Rig.Fake->Start(Get(TEXT("https://api.test/x")), Into(Canceled));
	Call->Cancel();
	TestTrue(TEXT("Cancel completes with Canceled"), Canceled->bDone && Canceled->Result.Failure == EVaRestTransportFailure::Canceled);
	TestEqual(TEXT("counted"), Rig.Fake->NumCanceled(), 1);
	Call->Cancel();
	TestEqual(TEXT("a second Cancel is a no-op"), Rig.Fake->NumCanceled(), 1);

	const FString File = FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("fake-download.part");
	IFileManager::Get().Delete(*File);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, TArray<uint8>{ 1, 2, 3, 4 }));
	FVaRestTransportRequest Download = Get(TEXT("https://api.test/file"));
	Download.ResponseFile = File;
	FDoneRef Saved = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Download), Into(Saved));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("a ResponseFile request writes the body there"), Saved->Result.bBodyInFile && FFileHelper::LoadFileToArray(OnDisk, *File) && OnDisk.Num() == 4);
	TestEqual(TEXT("... and not into the result"), Saved->Result.Body.Num(), 0);
	IFileManager::Get().Delete(*File);

	FVaRestTransportRequest Upload = Get(TEXT("https://api.test/up"));
	Upload.Verb = TEXT("POST");
	FVaRestBodySegment Head;
	Head.Bytes = { 'a', 'b' };
	FVaRestBodySegment Tail;
	Tail.Bytes = { 'c' };
	Upload.BodySegments = { Head, Tail };
	FDoneRef Up = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Upload), Into(Up));
	TestTrue(TEXT("ReadBody reads segments back"), Rig.Fake->ReadBody(2) == TArray<uint8>({ 'a', 'b', 'c' }));

	const uint64 Binds = FVaRestTrace::Counters().DelegateBinds;
	FVaRestTransportRequest Watched = Get(TEXT("https://api.test/w"));
	Watched.bReportProgress = true;
	Watched.bReportStatusTime = true;
	FDoneRef W = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Watched), Into(W));
	TestEqual(TEXT("progress and status binds are counted like the HTTP transport's (IN-11)"), FVaRestTrace::Counters().DelegateBinds.load(), Binds + 2);
	TestTrue(TEXT("status time is stamped"), Rig.Fake->GetCall(3).Request.Probe->StatusAt.load() >= 0.0 && W->Result.TransportDoneAt >= 0.0);

	// Reviews M-6, M13: past MaxBodyBytes the sink stops the transfer and keeps nothing
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT(R"({"padding":"0123456789"})"))));
	FVaRestTransportRequest Capped = Get(TEXT("https://api.test/big"));
	Capped.MaxBodyBytes = 8;
	FDoneRef Big = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(MoveTemp(Capped), Into(Big));
	TestTrue(TEXT("over MaxBodyBytes: ResponseTooLarge, no body"), Big->bDone && Big->Result.Failure == EVaRestTransportFailure::Other
		&& Big->Result.FailureDetail == FName(TEXT("ResponseTooLarge")) && Big->Result.Body.Num() == 0 && Big->Result.Status == 200);

	// Review I-3: a dropped completion is destroyed uncalled
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	FDoneRef Lost = MakeShared<FDone, ESPMode::ThreadSafe>();
	Rig.Fake->Start(Get(TEXT("https://api.test/lost")), Into(Lost));
	Rig.Fake->Drop(5);
	TestFalse(TEXT("Drop never calls the completion"), Lost->bDone.load());
	TestEqual(TEXT("... and the call is no longer in flight"), Rig.Fake->NumInFlight(), 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails on `VaRestClock.h` / `VaRestFakeTransport.h` / `VaRestHttpTesting.h` (no such file).

- [ ] **Step 3: The clock** — `Source/VaRestHttp/Public/VaRestClock.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Templates/Function.h"

#include <atomic>

/** A timer's id; 0 = none */
struct FVaRestTimerHandle
{
	uint64 Id = 0;

	bool IsValid() const { return Id != 0; }
	void Invalidate() { Id = 0; }
};

namespace VaRest::Http
{
	/**
	 * Any thread: a timer id unique in the process (review M-7). Per-clock counters would let a handle kept across a
	 * WithClock cancel an unrelated timer on the new clock.
	 */
	VARESTHTTP_API uint64 NewTimerId();
}

/**
 * Time and timers for the HTTP core, injectable so tests run on virtual time (spec 4.1). M3 times progress, the fake's
 * delays and timeouts, and trace events with it; M4's retry and backoff use the same clock.
 */
class IVaRestClock
{
public:
	virtual ~IVaRestClock() = default;

	/** Seconds, monotonic. Any thread: the HTTP thread stamps transport times with it (Inspector IN-3). */
	virtual double Now() const = 0;

	/** Game thread. Runs Callback once on the game thread after DelaySeconds; 0 or less: on the next tick. */
	virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) = 0;

	/** Game thread. Cancels a pending timer and invalidates Handle; a fired or invalid handle is a no-op. */
	virtual void ClearTimer(FVaRestTimerHandle& Handle) = 0;
};

/** FPlatformTime::Seconds and one-shot tickers on the core ticker (Ticker.h:35, :56, :66), which the engine ticks on the game thread */
class VARESTHTTP_API FVaRestRealClock final : public IVaRestClock, public TSharedFromThis<FVaRestRealClock, ESPMode::ThreadSafe>
{
public:
	/** The process-wide instance clients use by default */
	static TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Get();

	virtual double Now() const override;
	virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override;
	virtual void ClearTimer(FVaRestTimerHandle& Handle) override;

private:
	/** Game thread */
	TMap<uint64, FTSTicker::FDelegateHandle> Pending;
};

/**
 * Virtual time for tests (spec 7.1): nothing fires until Advance. Time is kept in integer nanoseconds (review I1): in
 * doubles a tick re-armed at Now() + 0.1 falls due at 0.30000000000000004 while 0.1 + 0.1 + 0.05 + 0.05 reaches only 0.3,
 * so a timer due "now" would not fire. M4's backoff tests need the same determinism.
 */
class VARESTHTTP_API FVaRestVirtualClock final : public IVaRestClock
{
public:
	virtual double Now() const override { return static_cast<double>(Nanos.load(std::memory_order_acquire)) / 1e9; }
	virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override;
	virtual void ClearTimer(FVaRestTimerHandle& Handle) override;

	/**
	 * Game thread. Moves time forward by DeltaSeconds, firing each due timer at its due time: in due order, ties in creation
	 * order, including timers that firing ones set when they fall due before the target. Advance(0) fires what is due now.
	 */
	void Advance(double DeltaSeconds);

	int32 NumPendingTimers() const { return Timers.Num(); }

	/** Seconds to whole nanoseconds; negative is 0 */
	static int64 ToNanos(double InSeconds) { return FMath::RoundToInt64(FMath::Max(InSeconds, 0.0) * 1e9); }

private:
	struct FTimer
	{
		uint64 Id = 0;
		int64 Due = 0;
		TUniqueFunction<void()> Callback;
	};

	std::atomic<int64> Nanos{ 0 };
	TArray<FTimer> Timers;
};
```

`Source/VaRestHttp/Private/VaRestClock.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClock.h"

#include "HAL/PlatformTime.h"

uint64 VaRest::Http::NewTimerId()
{
	// Trivially destructible: safe at static destruction (review I-4)
	static std::atomic<uint64> Next{ 1 };
	return Next.fetch_add(1, std::memory_order_relaxed);
}

TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> FVaRestRealClock::Get()
{
	static TSharedRef<FVaRestRealClock, ESPMode::ThreadSafe> Instance = MakeShared<FVaRestRealClock, ESPMode::ThreadSafe>();
	return Instance;
}

double FVaRestRealClock::Now() const
{
	return FPlatformTime::Seconds();
}

FVaRestTimerHandle FVaRestRealClock::SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback)
{
	check(IsInGameThread());
	const uint64 Id = VaRest::Http::NewTimerId();
	TWeakPtr<FVaRestRealClock, ESPMode::ThreadSafe> Weak = AsShared();
	Pending.Add(Id, FTSTicker::GetCoreTicker().AddTicker(TEXT("VaRest.Timer"), static_cast<float>(FMath::Max(DelaySeconds, 0.0)),
		[Weak, Id, Fn = MoveTemp(Callback)](float) mutable
		{
			if (TSharedPtr<FVaRestRealClock, ESPMode::ThreadSafe> Clock = Weak.Pin())
			{
				Clock->Pending.Remove(Id);
			}
			Fn();
			return false; // one shot: the ticker drops it
		}));
	return FVaRestTimerHandle{ Id };
}

void FVaRestRealClock::ClearTimer(FVaRestTimerHandle& Handle)
{
	check(IsInGameThread());
	if (FTSTicker::FDelegateHandle* Ticker = Pending.Find(Handle.Id))
	{
		FTSTicker::RemoveTicker(*Ticker);
		Pending.Remove(Handle.Id);
	}
	Handle.Invalidate();
}

FVaRestTimerHandle FVaRestVirtualClock::SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback)
{
	check(IsInGameThread());
	FTimer& Timer = Timers.AddDefaulted_GetRef();
	Timer.Id = VaRest::Http::NewTimerId();
	Timer.Due = Nanos.load(std::memory_order_acquire) + ToNanos(DelaySeconds);
	Timer.Callback = MoveTemp(Callback);
	return FVaRestTimerHandle{ Timer.Id };
}

void FVaRestVirtualClock::ClearTimer(FVaRestTimerHandle& Handle)
{
	check(IsInGameThread());
	Timers.RemoveAll([Id = Handle.Id](const FTimer& Timer) { return Timer.Id == Id; });
	Handle.Invalidate();
}

void FVaRestVirtualClock::Advance(double DeltaSeconds)
{
	check(IsInGameThread());
	const int64 Target = Nanos.load(std::memory_order_acquire) + ToNanos(DeltaSeconds);
	for (;;)
	{
		int32 Next = INDEX_NONE;
		for (int32 i = 0; i < Timers.Num(); ++i)
		{
			const FTimer& Timer = Timers[i];
			if (Timer.Due <= Target && (Next == INDEX_NONE || Timer.Due < Timers[Next].Due || (Timer.Due == Timers[Next].Due && Timer.Id < Timers[Next].Id)))
			{
				Next = i;
			}
		}
		if (Next == INDEX_NONE)
		{
			break;
		}
		FTimer Fired = MoveTemp(Timers[Next]);
		Timers.RemoveAt(Next);
		Nanos.store(FMath::Max(Nanos.load(std::memory_order_acquire), Fired.Due), std::memory_order_release);
		Fired.Callback();
	}
	Nanos.store(Target, std::memory_order_release);
}
```

- [ ] **Step 4: The transport interface** — `Source/VaRestHttp/Public/VaRestTransport.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBody.h"
#include "VaRestClock.h"
#include "VaRestHttpTypes.h"

#include <atomic>

/**
 * Written from the transport's threads while an attempt runs, read on the game thread: progress (spec 3.2) and the IN-3
 * transport times. Atomics, so neither side needs the other's lock.
 */
struct FVaRestTransportProbe
{
	std::atomic<int64> BytesSent{ 0 };
	std::atomic<int64> BytesReceived{ 0 };
	/** The request body's size, set before Start */
	std::atomic<int64> UploadTotal{ 0 };
	/** The response's Content-Length once known; -1 until then and for chunked responses */
	std::atomic<int64> DownloadTotal{ -1 };
	/** IVaRestClock seconds; -1 = not reached (or not watched) */
	std::atomic<double> UploadDoneAt{ -1.0 };
	std::atomic<double> StatusAt{ -1.0 };
};

/** One attempt, as the transport sees it */
struct FVaRestTransportRequest
{
	uint64 RequestId = 0;
	int32 Attempt = 1;
	FString Verb;
	FString Url;
	FVaRestHeaders Headers;
	/** In-memory body, moved into IHttpRequest::SetContent(TArray<uint8>&&) (spec 2.4) */
	TArray<uint8> Body;
	/** A file streamed with IHttpRequest::SetContentAsStreamedFile */
	FString BodyFile;
	/** Multipart with file parts, streamed with IHttpRequest::SetContentFromStream */
	TArray<FVaRestBodySegment> BodySegments;
	/**
	 * Seconds; 0 = VaRest sets none (spec 4.7). With Timeout > 0 and ActivityTimeout 0 the HTTP transport also sets the
	 * engine's activity timeout to Timeout, so the engine's 30 s idle default never fires first (review I-5).
	 */
	float Timeout = 0.f;
	float ActivityTimeout = 0.f;
	EVaRestPriority Priority = EVaRestPriority::Normal;
	/** Non-empty: stream the response body into this file (SaveTo's temp file) instead of memory */
	FString ResponseFile;
	/**
	 * The result's JSON cap (MaxJsonBytes, decision 20); 0 for results that are not JSON or have no cap. The body sink
	 * stops the transfer once more arrived (Failure Other, detail ResponseTooLarge) and reserves at most
	 * max(MaxBodyBytes, 8 MiB) + 1 up front, so a lying Content-Length costs nothing (reviews M-6, M13).
	 */
	int64 MaxBodyBytes = 0;
	/** Bind progress reporting (a progress callback or trace metadata) */
	bool bReportProgress = false;
	/** Bind the status-code callback and stamp transport times (trace metadata only, IN-11) */
	bool bReportStatusTime = false;
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	/** Stamps the probe's and the result's times */
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
};

/** What one attempt produced */
struct FVaRestTransportResult
{
	/** 0 = no response */
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** The body, with one byte of slack so FVaRestJson::ParseOwned parses it without a copy (spec 2.1) */
	TArray<uint8> Body;
	/** The body went to the request's ResponseFile */
	bool bBodyInFile = false;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	/**
	 * The transport's own reason: LexToString(EHttpFailureReason), "ActivityTimeout", and VaRest's "ResponseTooLarge"
	 * (MaxBodyBytes), "ResponseFileWriteFailed", "ResponseBodyRejected", "BodyRejected", "InvalidHeader",
	 * "TransportDropped" (the transport never called back)
	 */
	FName FailureDetail;
	int64 BytesUp = 0;
	int64 BytesDown = 0;
	/** IVaRestClock seconds; -1 unless bReportStatusTime */
	double TransportDoneAt = -1.0;
};

class IVaRestTransportCall
{
public:
	virtual ~IVaRestTransportCall() = default;

	/** Game thread. Asks the transport to stop; its completion then reports Canceled, or the result it already had. */
	virtual void Cancel() = 0;
};

using FVaRestTransportComplete = TUniqueFunction<void(FVaRestTransportResult&&)>;

/** Spec 4: the chain's last step. FHttpModule by default (Task 7); tests inject FVaRestFakeTransport (spec 7.1). */
class IVaRestTransport
{
public:
	virtual ~IVaRestTransport() = default;

	/**
	 * Game thread. OnComplete runs exactly once, on any thread (the HTTP thread for FHttpModule), possibly before Start
	 * returns; the core moves the result to the game thread itself (spec 1). A transport that destroys OnComplete without
	 * calling it (the engine does at HTTP module shutdown and with NullHttp) still finishes the request: the core wraps
	 * OnComplete so that its destruction reports Failure Other, detail TransportDropped (review I-3).
	 */
	virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) = 0;
};
```

- [ ] **Step 5: The fake** — `Source/VaRestHttp/Public/VaRestFakeTransport.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClock.h"
#include "VaRestTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

/** A scripted response */
struct VARESTHTTP_API FVaRestFakeResponse
{
	int32 Status = 200;
	FVaRestHeaders Headers;
	TArray<uint8> Body;
	/** Empty: the request URL */
	FString EffectiveUrl;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	/** Virtual seconds until it completes; 0 = at once; negative = only when the test calls Complete() */
	double Delay = 0.0;

	static FVaRestFakeResponse Json(int32 InStatus, FUtf8StringView InJson);
	static FVaRestFakeResponse Text(int32 InStatus, FStringView InText, FStringView InContentType = TEXTVIEW("text/plain; charset=utf-8"));
	static FVaRestFakeResponse Bytes(int32 InStatus, TArray<uint8> InBody, FStringView InContentType = TEXTVIEW("application/octet-stream"));
	static FVaRestFakeResponse Failed(EVaRestTransportFailure InFailure, FName InDetail = NAME_None);
	static FVaRestFakeResponse Manual();
};

/**
 * The fake transport of spec 7.1, on a virtual clock. Mirrors the real transport where the core depends on it:
 * completion from a background task (bCompleteOnWorker), a body with one byte of slack, a total timeout as Timeout and an
 * activity timeout as Connection (HttpRequestCommon.cpp:299-305), the probe, ResponseFile, MaxBodyBytes (a body over it is
 * Failure Other, detail ResponseTooLarge, nothing kept), and IN-11's bind counting. Game thread, except completions with
 * bCompleteOnWorker. Compiled with WITH_DEV_AUTOMATION_TESTS only (review M15): it is not for games.
 */
class VARESTHTTP_API FVaRestFakeTransport final : public IVaRestTransport, public TSharedFromThis<FVaRestFakeTransport, ESPMode::ThreadSafe>
{
public:
	explicit FVaRestFakeTransport(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock);

	using FResponder = TFunction<FVaRestFakeResponse(const FVaRestTransportRequest&)>;

	/** Answers every call that Enqueue did not script; default: 200 with an empty JSON object */
	void SetResponder(FResponder InResponder);
	/** The next call's response (first in, first out), before the responder */
	void Enqueue(FVaRestFakeResponse Response);

	/** true (default): completions come from a UE::Tasks task, as the HTTP thread's do; false: inline, inside Start, Complete or Advance */
	bool bCompleteOnWorker = true;

	struct FCall
	{
		FVaRestTransportRequest Request;
		double StartedAt = 0.0;
		bool bCompleted = false;
		bool bCanceled = false;
		/** The completed body's buffer and size (zero-copy tests compare it with the parsed document's strings) */
		const uint8* BodyData = nullptr;
		int32 BodyNum = 0;
	};

	int32 NumCalls() const { return Entries.Num(); }
	const FCall& GetCall(int32 Index) const { return Entries[Index]->Call; }
	int32 NumInFlight() const;
	int32 NumCanceled() const;
	/** What the call sent: its bytes, its upload file, or its multipart segments read through FVaRestSegmentStream */
	TArray<uint8> ReadBody(int32 Index) const;
	/** Completes a Manual() call or one still waiting for its delay; a completed call ignores it */
	void Complete(int32 Index, FVaRestFakeResponse Response);
	/** Writes the call's probe as the HTTP thread would, and counts as activity for its activity timeout */
	void ReportProgress(int32 Index, int64 BytesSent, int64 BytesReceived, int64 DownloadTotal = -1);
	/** Destroys the call's completion without calling it, as the engine does at HTTP module shutdown and with NullHttp (review I-3) */
	void Drop(int32 Index);

	virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) override;

private:
	class FCallHandle;

	struct FEntry
	{
		FCall Call;
		FVaRestTransportComplete OnComplete;
		FVaRestTimerHandle DelayTimer;
		FVaRestTimerHandle TimeoutTimer;
		FVaRestTimerHandle ActivityTimer;
		double LastActivityAt = 0.0;
	};

	void Finish(int32 Index, FVaRestFakeResponse&& Response);
	void CancelCall(int32 Index);
	void ArmActivity(int32 Index);

	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
	FResponder Responder;
	TArray<FVaRestFakeResponse> Queue;
	TArray<TSharedRef<FEntry>> Entries;
};

#endif // WITH_DEV_AUTOMATION_TESTS
```

`Source/VaRestHttp/Private/VaRestFakeTransport.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestFakeTransport.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/FileHelper.h"
#include "Tasks/Task.h"
#include "VaRestTrace.h"

class FVaRestFakeTransport::FCallHandle final : public IVaRestTransportCall
{
public:
	FCallHandle(TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> InTransport, int32 InIndex)
		: Transport(MoveTemp(InTransport))
		, Index(InIndex)
	{
	}

	virtual void Cancel() override
	{
		if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Transport.Pin())
		{
			Pinned->CancelCall(Index);
		}
	}

private:
	TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Transport;
	int32 Index;
};

FVaRestFakeResponse FVaRestFakeResponse::Json(int32 InStatus, FUtf8StringView InJson)
{
	FVaRestFakeResponse Out;
	Out.Status = InStatus;
	Out.Headers.Set(TEXT("Content-Type"), TEXT("application/json"));
	Out.Body.Append(reinterpret_cast<const uint8*>(InJson.GetData()), InJson.Len());
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Text(int32 InStatus, FStringView InText, FStringView InContentType)
{
	FVaRestFakeResponse Out;
	Out.Status = InStatus;
	if (!InContentType.IsEmpty())
	{
		Out.Headers.Set(TEXT("Content-Type"), InContentType);
	}
	FTCHARToUTF8 Utf8(InText.GetData(), InText.Len());
	Out.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Bytes(int32 InStatus, TArray<uint8> InBody, FStringView InContentType)
{
	FVaRestFakeResponse Out;
	Out.Status = InStatus;
	if (!InContentType.IsEmpty())
	{
		Out.Headers.Set(TEXT("Content-Type"), InContentType);
	}
	Out.Body = MoveTemp(InBody);
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Failed(EVaRestTransportFailure InFailure, FName InDetail)
{
	FVaRestFakeResponse Out;
	Out.Status = 0;
	Out.Failure = InFailure;
	Out.FailureDetail = InDetail;
	return Out;
}

FVaRestFakeResponse FVaRestFakeResponse::Manual()
{
	FVaRestFakeResponse Out;
	Out.Delay = -1.0;
	return Out;
}

FVaRestFakeTransport::FVaRestFakeTransport(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock)
	: Clock(MoveTemp(InClock))
{
}

void FVaRestFakeTransport::SetResponder(FResponder InResponder)
{
	Responder = MoveTemp(InResponder);
}

void FVaRestFakeTransport::Enqueue(FVaRestFakeResponse Response)
{
	Queue.Add(MoveTemp(Response));
}

int32 FVaRestFakeTransport::NumInFlight() const
{
	int32 Count = 0;
	for (const TSharedRef<FEntry>& Entry : Entries)
	{
		Count += Entry->Call.bCompleted ? 0 : 1;
	}
	return Count;
}

int32 FVaRestFakeTransport::NumCanceled() const
{
	int32 Count = 0;
	for (const TSharedRef<FEntry>& Entry : Entries)
	{
		Count += Entry->Call.bCanceled ? 1 : 0;
	}
	return Count;
}

TArray<uint8> FVaRestFakeTransport::ReadBody(int32 Index) const
{
	const FVaRestTransportRequest& Request = Entries[Index]->Call.Request;
	TArray<uint8> Out;
	if (!Request.BodyFile.IsEmpty())
	{
		FFileHelper::LoadFileToArray(Out, *Request.BodyFile);
	}
	else if (Request.BodySegments.Num() > 0)
	{
		FVaRestSegmentStream Stream(Request.BodySegments);
		Out.SetNumUninitialized(Stream.TotalSize());
		Stream.Serialize(Out.GetData(), Out.Num());
	}
	else
	{
		Out = Request.Body;
	}
	return Out;
}

TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> FVaRestFakeTransport::Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete)
{
	check(IsInGameThread());
	TSharedRef<FEntry> Entry = MakeShared<FEntry>();
	const int32 Index = Entries.Add(Entry);
	Entry->Call.StartedAt = Clock->Now();
	Entry->LastActivityAt = Entry->Call.StartedAt;
	Entry->OnComplete = MoveTemp(OnComplete);
	Entry->Call.Request = MoveTemp(Request);
	FVaRestTransportRequest& Req = Entry->Call.Request;

	// The HTTP transport binds one engine delegate per flag (IN-11); count the same way
	FVaRestTrace::Counters().DelegateBinds += (Req.bReportProgress ? 1 : 0) + (Req.bReportStatusTime ? 1 : 0);
	if (Req.bReportProgress || Req.bReportStatusTime)
	{
		Req.Probe->UploadDoneAt = Clock->Now(); // the fake uploads instantly
	}

	FVaRestFakeResponse Response;
	if (Queue.Num() > 0)
	{
		Response = MoveTemp(Queue[0]);
		Queue.RemoveAt(0);
	}
	else if (Responder)
	{
		Response = Responder(Req);
	}
	else
	{
		Response = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	}

	TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Weak = AsShared();
	const bool bWaits = Response.Delay < 0.0;
	if (Req.Timeout > 0.f && (bWaits || Response.Delay > Req.Timeout))
	{
		Entry->TimeoutTimer = Clock->SetTimer(Req.Timeout, [Weak, Index]
		{
			if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Finish(Index, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")));
			}
		});
	}
	if (Req.ActivityTimeout > 0.f && (bWaits || Response.Delay > Req.ActivityTimeout))
	{
		ArmActivity(Index);
	}
	if (Response.Delay == 0.0)
	{
		Finish(Index, MoveTemp(Response));
	}
	else if (!bWaits)
	{
		const double Delay = Response.Delay;
		Entry->DelayTimer = Clock->SetTimer(Delay, [Weak, Index, Scripted = MoveTemp(Response)]() mutable
		{
			if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Finish(Index, MoveTemp(Scripted));
			}
		});
	}
	return MakeShared<FCallHandle, ESPMode::ThreadSafe>(Weak, Index);
}

void FVaRestFakeTransport::Complete(int32 Index, FVaRestFakeResponse Response)
{
	check(IsInGameThread());
	Finish(Index, MoveTemp(Response));
}

void FVaRestFakeTransport::ReportProgress(int32 Index, int64 BytesSent, int64 BytesReceived, int64 DownloadTotal)
{
	check(IsInGameThread());
	FEntry& Entry = *Entries[Index];
	Entry.Call.Request.Probe->BytesSent = BytesSent;
	Entry.Call.Request.Probe->BytesReceived = BytesReceived;
	Entry.Call.Request.Probe->DownloadTotal = DownloadTotal;
	Entry.LastActivityAt = Clock->Now();
	if (Entry.Call.Request.ActivityTimeout > 0.f && !Entry.Call.bCompleted)
	{
		ArmActivity(Index);
	}
}

void FVaRestFakeTransport::Drop(int32 Index)
{
	check(IsInGameThread());
	FEntry& Entry = *Entries[Index];
	if (Entry.Call.bCompleted)
	{
		return;
	}
	Entry.Call.bCompleted = true;
	Clock->ClearTimer(Entry.DelayTimer);
	Clock->ClearTimer(Entry.TimeoutTimer);
	Clock->ClearTimer(Entry.ActivityTimer);
	Entry.OnComplete.Reset(); // destroyed uncalled
}

void FVaRestFakeTransport::ArmActivity(int32 Index)
{
	FEntry& Entry = *Entries[Index];
	Clock->ClearTimer(Entry.ActivityTimer);
	const double Due = Entry.LastActivityAt + Entry.Call.Request.ActivityTimeout - Clock->Now();
	TWeakPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Weak = AsShared();
	Entry.ActivityTimer = Clock->SetTimer(Due, [Weak, Index]
	{
		if (TSharedPtr<FVaRestFakeTransport, ESPMode::ThreadSafe> Pinned = Weak.Pin())
		{
			// HttpRequestCommon.cpp:303-305: the engine reports an activity timeout as a connection error
			Pinned->Finish(Index, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ActivityTimeout")));
		}
	});
}

void FVaRestFakeTransport::CancelCall(int32 Index)
{
	FEntry& Entry = *Entries[Index];
	if (Entry.Call.bCompleted)
	{
		return;
	}
	Entry.Call.bCanceled = true;
	Finish(Index, FVaRestFakeResponse::Failed(EVaRestTransportFailure::Canceled, TEXT("Cancelled")));
}

void FVaRestFakeTransport::Finish(int32 Index, FVaRestFakeResponse&& Response)
{
	FEntry& Entry = *Entries[Index];
	if (Entry.Call.bCompleted)
	{
		return;
	}
	Entry.Call.bCompleted = true;
	Clock->ClearTimer(Entry.DelayTimer);
	Clock->ClearTimer(Entry.TimeoutTimer);
	Clock->ClearTimer(Entry.ActivityTimer);

	const FVaRestTransportRequest& Req = Entry.Call.Request;
	FVaRestTransportResult Result;
	Result.Failure = Response.Failure;
	Result.FailureDetail = Response.FailureDetail;
	Result.BytesUp = Req.Probe->UploadTotal;
	if (Response.Failure == EVaRestTransportFailure::None)
	{
		Result.Status = Response.Status;
		Result.Headers = Response.Headers;
		Result.ContentType = Response.Headers.Get(TEXT("Content-Type"));
		Result.EffectiveUrl = Response.EffectiveUrl.IsEmpty() ? Req.Url : Response.EffectiveUrl;
		if (!Req.ResponseFile.IsEmpty())
		{
			// As the HTTP transport's file sink: the file exists only once a byte arrived
			if (Response.Body.Num() > 0)
			{
				FFileHelper::SaveArrayToFile(Response.Body, *Req.ResponseFile);
			}
			Result.bBodyInFile = true;
			Result.BytesDown = Response.Body.Num();
		}
		else if (Req.MaxBodyBytes > 0 && Response.Body.Num() > Req.MaxBodyBytes)
		{
			// As the HTTP transport's sink: the transfer stops past the JSON cap, nothing is kept (reviews M-6, M13)
			Result.Failure = EVaRestTransportFailure::Other;
			Result.FailureDetail = TEXT("ResponseTooLarge");
			Result.BytesDown = Req.MaxBodyBytes + 1;
		}
		else
		{
			// As the HTTP transport's memory sink: one byte of slack for ParseOwned's terminator (spec 2.1)
			Result.Body.Reserve(Response.Body.Num() + 1);
			Result.Body.Append(Response.Body);
			Entry.Call.BodyData = Result.Body.GetData();
			Entry.Call.BodyNum = Result.Body.Num();
			Result.BytesDown = Result.Body.Num();
		}
		if (Req.bReportStatusTime)
		{
			Req.Probe->StatusAt = Clock->Now();
		}
	}
	Req.Probe->BytesReceived = Result.BytesDown;
	if (Req.bReportStatusTime)
	{
		Result.TransportDoneAt = Clock->Now();
	}

	FVaRestTransportComplete Callback = MoveTemp(Entry.OnComplete);
	if (bCompleteOnWorker)
	{
		UE::Tasks::Launch(TEXT("VaRest.FakeTransport.Complete"), [Deliver = MoveTemp(Callback), Moved = MoveTemp(Result)]() mutable
		{
			Deliver(MoveTemp(Moved));
		});
	}
	else
	{
		Callback(MoveTemp(Result));
	}
}

#endif // WITH_DEV_AUTOMATION_TESTS
```

- [ ] **Step 6: Test helpers** — `Source/VaRestHttp/Public/VaRestHttpTesting.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/OutputDevice.h"
#include "Templates/Function.h"

#include <atomic>

#if !UE_BUILD_SHIPPING
namespace VaRest::Http::Testing
{
	/** Where decode steps ran (the decode-off-the-game-thread proof, Task 10). Not in Shipping (review M15). */
	struct FTestCounters
	{
		std::atomic<int32> DecodesOnGameThread{ 0 };
		std::atomic<int32> DecodesOffGameThread{ 0 };
		/** ~FClientState off the game thread (review I-1: it must not happen) */
		std::atomic<int32> ClientStatesDestroyedOffGameThread{ 0 };
	};
	VARESTHTTP_API FTestCounters& Counters();
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
/**
 * Helpers for VaRest's automation tests, and for other modules' (Inspector, WebSocket). Not for game code: compiled with
 * WITH_DEV_AUTOMATION_TESTS only (review M15).
 */
namespace VaRest::Http::Testing
{
	/**
	 * Game thread, from a test body. Runs game-thread tasks until Done() is true or TimeoutSeconds of real time pass;
	 * true when Done() became true. Automation runs tests from FEngineLoop::Tick outside any task
	 * (LaunchEngineLoop.cpp:6036-6045), so ProcessThreadUntilIdle's recursion guard holds (TaskGraph.cpp:700-708); never
	 * call this from inside a game-thread task or a ticker: it checks, because a nested core-ticker Tick trips
	 * Ticker.cpp:102 with a message that does not name the cause (review M-17). bTickCoreTicker also ticks
	 * FTSTicker::GetCoreTicker() (real clock timers, the engine's HTTP manager).
	 */
	VARESTHTTP_API bool PumpUntil(TFunctionRef<bool()> Done, double TimeoutSeconds = 5.0, bool bTickCoreTicker = false);

	/** Waits until the core redaction pipe is idle: body log lines are written there */
	VARESTHTTP_API void FlushRedactionPipe();

	/** -VaRestTestServer=<url>, which Tools/run_tests.sh passes; empty when absent */
	VARESTHTTP_API FString GetTestServerUrl();

	/** Collects every log line written while it lives, from any thread, as "Category: text" */
	class VARESTHTTP_API FLogCapture final : public FOutputDevice
	{
	public:
		FLogCapture();
		virtual ~FLogCapture() override;

		virtual void Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category) override;
		// Without these the redirector buffers other threads' lines until the game thread flushes them (OutputDevice.h:201-212)
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		bool Contains(FStringView Text) const;
		int32 Count(FStringView Text) const;
		TArray<FString> GetLines() const;

	private:
		mutable FCriticalSection Lock;
		TArray<FString> Lines;
	};

	// Task 8 appends TTestOutcome, MakeOutcome, Observe and FTestRig here, inside this block
}
#endif // WITH_DEV_AUTOMATION_TESTS
```

`Source/VaRestHttp/Private/VaRestHttpTesting.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpTesting.h"

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/CommandLine.h"
#include "Misc/OutputDeviceRedirector.h"
#include "Misc/Parse.h"
#include "Misc/ScopeLock.h"
#include "VaRestRedactionPipe.h"

#if !UE_BUILD_SHIPPING
VaRest::Http::Testing::FTestCounters& VaRest::Http::Testing::Counters()
{
	static FTestCounters Instance;
	return Instance;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
bool VaRest::Http::Testing::PumpUntil(TFunctionRef<bool()> Done, double TimeoutSeconds, bool bTickCoreTicker)
{
	check(IsInGameThread());
	// Review M-17: from inside a task, a ticker or a callback that PumpUntil itself runs, the nested Tick would trip
	// Ticker.cpp:102 ("Invalid state") or recurse into the task graph: name the misuse instead
	static bool bPumping = false;
	checkf(!bPumping, TEXT("Testing::PumpUntil is not re-entrant: call it from a test body, never from a callback, a task or a ticker it runs"));
	TGuardValue<bool> Pumping(bPumping, true);
	const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
	double LastTick = FPlatformTime::Seconds();
	for (;;)
	{
		FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		if (bTickCoreTicker)
		{
			const double Now = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(Now - LastTick));
			LastTick = Now;
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
		}
		if (Done())
		{
			return true;
		}
		if (FPlatformTime::Seconds() >= Deadline)
		{
			return false;
		}
		FPlatformProcess::Sleep(0.001f);
	}
}

void VaRest::Http::Testing::FlushRedactionPipe()
{
	if (UE::Tasks::FPipe* Pipe = VaRest::Http::GetRedactionPipe())
	{
		Pipe->WaitUntilEmpty();
	}
}

FString VaRest::Http::Testing::GetTestServerUrl()
{
	FString Url;
	FParse::Value(FCommandLine::Get(), TEXT("VaRestTestServer="), Url);
	return Url;
}

VaRest::Http::Testing::FLogCapture::FLogCapture()
{
	GLog->AddOutputDevice(this);
}

VaRest::Http::Testing::FLogCapture::~FLogCapture()
{
	GLog->RemoveOutputDevice(this);
}

void VaRest::Http::Testing::FLogCapture::Serialize(const TCHAR* V, ELogVerbosity::Type Verbosity, const FName& Category)
{
	FScopeLock ScopeLock(&Lock);
	Lines.Add(FString::Printf(TEXT("%s: %s"), *Category.ToString(), V));
}

bool VaRest::Http::Testing::FLogCapture::Contains(FStringView Text) const
{
	return Count(Text) > 0;
}

int32 VaRest::Http::Testing::FLogCapture::Count(FStringView Text) const
{
	FScopeLock ScopeLock(&Lock);
	int32 Found = 0;
	for (const FString& Line : Lines)
	{
		Found += UE::String::FindFirst(Line, Text, ESearchCase::CaseSensitive) != INDEX_NONE ? 1 : 0;
	}
	return Found;
}

TArray<FString> VaRest::Http::Testing::FLogCapture::GetLines() const
{
	FScopeLock ScopeLock(&Lock);
	return Lines;
}
#endif // WITH_DEV_AUTOMATION_TESTS
```

(add `#include "String/Find.h"` for `UE::String::FindFirst`; `TGuardValue` comes from `Templates/UnrealTemplate.h`, which `CoreMinimal.h` includes).

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.Clock.Virtual`, `VaRest.Http.Clock.Real`, `VaRest.Http.Testing.LogCapture`, `VaRest.Http.Fake.Responds`, `VaRest.Http.Fake.VirtualTime`, `VaRest.Http.Fake.CancelAndFiles`.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestClock.h Source/VaRestHttp/Private/VaRestClock.cpp Source/VaRestHttp/Public/VaRestTransport.h \
  Source/VaRestHttp/Public/VaRestFakeTransport.h Source/VaRestHttp/Private/VaRestFakeTransport.cpp \
  Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestHttpTesting.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpClockTest.cpp Source/VaRestHttp/Private/Tests/VaRestHttpFakeTransportTest.cpp
git commit -m "http: IVaRestClock (real and virtual), IVaRestTransport, the fake transport, test pump and log capture

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 7: The `FHttpModule` transport — HTTP-thread completion, body sinks with slack, file sinks

Spec §1: "Transport completes on the HTTP thread (`EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread`); the completion only moves bytes and dispatches work." Bead VaRest-6zo: "Response bodies must reserve one byte of slack so ParseOwned is zero-copy (curl reserves exactly Content-Length, CurlHttp.cpp:633)". So the response body never lands in the engine's buffer: `SetResponseBodyReceiveStreamDelegateV2` (`IHttpRequest.h:273-284`, "Once set, the data will no longer be cached in response") feeds `FBodySink`, which reserves `Content-Length + 1` on the first chunk (decision 5), capped at `max(MaxBodyBytes, 8 MiB) + 1` because a `Content-Length` is a claim (reviews M-6, M13), and which stops a JSON result's transfer past its cap. The same sink writes `SaveTo`'s temp file instead (spec §3.2: "streams the body to a temp file"); those writes run on the HTTP thread, as the engine's own `SetResponseBodyReceiveStream` to a file archive does (decision 31). The sink has its own lock and is sealed by the completion: on Apple the body arrives on NSURLSession's delegate queue while the completion runs on the HTTP thread, and nothing in the engine orders the two (review C-4). Uploads use `SetContent(TArray<uint8>&&)`, `SetContentAsStreamedFile` and `SetContentFromStream` (`IHttpRequest.h:223-257`); when progress is asked for they go through a counting archive instead, because curl reports no progress while it uploads under `CompleteOnHttpThread` (decision 29). Progress and status delegates are bound only when asked (IN-11). A total timeout without an activity timeout also sets the engine's activity timeout, so the engine's 30 s idle default never fires first (decision 30, review I-5). A completion the engine drops (HTTP module shutdown, NullHttp) still finishes the request (review I-3).

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestBodySink.h`, `Source/VaRestHttp/Private/VaRestBodySink.cpp`, `Source/VaRestHttp/Private/VaRestHttpTransportInternal.h`, `Source/VaRestHttp/Private/VaRestHttpTransport.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestTransport.h` (declare `GetDefaultTransport`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpTransportTest.cpp`

**Interfaces:**
- Consumes: `IVaRestTransport`, `FVaRestTransportRequest` (with `MaxBodyBytes`), `FVaRestTransportResult`, `FVaRestTransportProbe`, `IVaRestTransportCall`, `IVaRestClock` (Task 6); `FVaRestSegmentStream` (Task 3); `FVaRestHeaders::FromLines` (Task 2); `FVaRestTrace::Counters()` (Task 4).
- Produces: `VARESTHTTP_API TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> VaRest::Http::GetDefaultTransport()` (public); private `VaRest::Http::FBodySink` (`FBodySink()`, `explicit FBodySink(int64 MaxBytes)`, `explicit FBodySink(FString FilePath, int64 MaxBytes = 0)`, `bool Write(const void* Data, int64 Length, int64 ContentLength)`, `TArray<uint8> TakeBytes()`, `bool CloseFile()`, `int64 Num() const`, `bool IsFile() const`, `bool HasError() const`, `bool IsTooLarge() const`, `static constexpr int64 ReserveCap = 8 MiB`; tests: `GetDataInternal()`, `GetCapacityInternal()`); private (`VaRestHttpTransportInternal.h`) `float VaRest::Http::EffectiveActivityTimeout(float Total, float Activity)`, `class VaRest::Http::FCountingUpload final : public FArchive` (three constructors: owned bytes, an inner archive, a file path; each with the probe, clock and `bStamp`).

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpTransportTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestBodySink.h"
#include "VaRestClock.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpTransportInternal.h"
#include "VaRestTransport.h"

namespace VaRest::Http::TransportTestPrivate
{
	struct FDone
	{
		std::atomic<bool> bDone{ false };
		std::atomic<int32> Calls{ 0 };
		bool bOnGameThread = true;
		FVaRestTransportResult Result;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpMemorySinkTest, "VaRest.Http.Transport.MemorySinkSlack",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpMemorySinkTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::FBodySink;
	TArray<uint8> Chunk;
	Chunk.Init('x', 256);

	FBodySink Sink;
	Sink.Write(Chunk.GetData(), Chunk.Num(), 1024);
	const uint8* FirstBuffer = Sink.GetDataInternal();
	for (int32 i = 1; i < 4; ++i)
	{
		Sink.Write(Chunk.GetData(), Chunk.Num(), -1); // later chunks: the length is already known
	}
	TestTrue(TEXT("no reallocation after the first chunk"), Sink.GetDataInternal() == FirstBuffer);
	TArray<uint8> Body = Sink.TakeBytes();
	TestEqual(TEXT("the whole body"), Body.Num(), 1024);
	TestTrue(TEXT("one byte of slack: ParseOwned appends its terminator without reallocating (spec 2.1)"), Body.Max() > Body.Num());
	TestTrue(TEXT("TakeBytes moves the buffer it filled"), Body.GetData() == FirstBuffer);
	TestFalse(TEXT("review C-4: TakeBytes seals the sink; a later write fails"), Sink.Write(Chunk.GetData(), 1, -1));

	FBodySink Chunked; // no Content-Length (chunked transfer, or compressed)
	for (int32 i = 0; i < 20; ++i)
	{
		Chunked.Write(Chunk.GetData(), Chunk.Num(), -1);
	}
	TArray<uint8> Unsized = Chunked.TakeBytes();
	TestTrue(TEXT("unknown length still ends with slack"), Unsized.Num() == 20 * 256 && Unsized.Max() > Unsized.Num());

	FBodySink Wrong; // a Content-Length smaller than the body
	Wrong.Write(Chunk.GetData(), Chunk.Num(), 10);
	TArray<uint8> Larger = Wrong.TakeBytes();
	TestTrue(TEXT("a wrong Content-Length still ends with slack"), Larger.Num() == 256 && Larger.Max() > Larger.Num());

	// Review M-6: a Content-Length is a claim; a lying one must not allocate gigabytes up front
	FBodySink Liar;
	Liar.Write(Chunk.GetData(), Chunk.Num(), 2000000000);
	TestTrue(TEXT("a huge Content-Length reserves at most ReserveCap + 1"), Liar.GetCapacityInternal() <= FBodySink::ReserveCap + 1);
	TestTrue(TEXT("... and the body still ends with slack"), Liar.TakeBytes().Max() > Chunk.Num());

	// Review M13: a JSON result's cap stops the transfer instead of receiving the whole body first
	FBodySink Capped(300);
	TestTrue(TEXT("within the cap"), Capped.Write(Chunk.GetData(), Chunk.Num(), 1024));
	TestTrue(TEXT("the reserve follows Content-Length under the 8 MiB cap"), Capped.GetCapacityInternal() == 1025);
	TestFalse(TEXT("past the cap the write fails"), Capped.Write(Chunk.GetData(), Chunk.Num(), -1));
	TestTrue(TEXT("... and says why"), Capped.IsTooLarge() && Capped.HasError());

	FBodySink Empty;
	TArray<uint8> Nothing = Empty.TakeBytes();
	TestTrue(TEXT("an empty body has room for the terminator too"), Nothing.Num() == 0 && Nothing.Max() >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpFileSinkTest, "VaRest.Http.Transport.FileSink",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpFileSinkTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::FBodySink;
	const FString Dir = FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("sink");
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	const FString Path = Dir / TEXT("sub") / TEXT("download.part");
	{
		FBodySink Sink{ FString(Path) };
		TestFalse(TEXT("nothing is created before the first byte"), IFileManager::Get().FileExists(*Path));
		const uint8 A[] = { 1, 2, 3 };
		const uint8 B[] = { 4, 5 };
		TestTrue(TEXT("write"), Sink.Write(A, 3, -1) && Sink.Write(B, 2, -1));
		TestTrue(TEXT("close"), Sink.CloseFile());
		TestTrue(TEXT("a file sink is a file sink"), Sink.IsFile() && Sink.Num() == 5);
		TestFalse(TEXT("CloseFile seals it"), Sink.Write(A, 1, -1));
	}
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the directory is created and the bytes written"), FFileHelper::LoadFileToArray(OnDisk, *Path) && OnDisk == TArray<uint8>({ 1, 2, 3, 4, 5 }));

	// A parent that is a file: the directory cannot be created, so the write fails and the sink says so
	const FString Blocker = Dir / TEXT("blocker");
	FFileHelper::SaveStringToFile(TEXT("x"), *Blocker);
	FBodySink Blocked{ Blocker / TEXT("child.part") };
	const uint8 C[] = { 9 };
	TestFalse(TEXT("an unwritable path fails"), Blocked.Write(C, 1, -1));
	TestTrue(TEXT("... and remembers it"), Blocked.HasError() && !Blocked.CloseFile());
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCountingUploadTest, "VaRest.Http.Transport.CountingUpload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCountingUploadTest::RunTest(const FString& Parameters)
{
	// Review I-2: the archive curl reads a progress-watched upload through
	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock = MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>();
	Clock->Advance(2.0);
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	VaRest::Http::FCountingUpload Upload(TArray<uint8>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }, Probe, Clock, /*bStamp*/ true);
	TestEqual(TEXT("TotalSize is the Content-Length curl sends (CurlHttp.cpp:896-902)"), Upload.TotalSize(), static_cast<int64>(10));
	uint8 Out[10] = {};
	Upload.Serialize(Out, 4);
	TestEqual(TEXT("counted as curl reads"), Probe->BytesSent.load(), static_cast<int64>(4));
	TestTrue(TEXT("not done yet"), Probe->UploadDoneAt.load() < 0.0);
	Upload.Seek(4);
	Upload.Serialize(Out + 4, 6);
	TestEqual(TEXT("all of it"), Probe->BytesSent.load(), static_cast<int64>(10));
	TestEqual(TEXT("UploadDoneAt stamped at the last byte (IN-3)"), Probe->UploadDoneAt.load(), 2.0);
	TestTrue(TEXT("the bytes are the body's"), Out[0] == 1 && Out[9] == 10);
	Upload.Serialize(Out, 1);
	TestTrue(TEXT("reading past the end is an error, not garbage"), Upload.IsError());

	TArray<FVaRestBodySegment> Segments;
	Segments.AddDefaulted_GetRef().Bytes = { 'a', 'b' };
	Segments.AddDefaulted_GetRef().Bytes = { 'c' };
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> SegmentProbe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	VaRest::Http::FCountingUpload Parts(MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Segments)), 3, SegmentProbe, Clock, false);
	uint8 Abc[3] = {};
	Parts.Serialize(Abc, 3);
	TestTrue(TEXT("an inner archive (multipart segments) is read through and counted"), Abc[0] == 'a' && Abc[2] == 'c' && SegmentProbe->BytesSent.load() == 3);
	TestTrue(TEXT("... without stamping when not asked"), SegmentProbe->UploadDoneAt.load() < 0.0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportTimeoutsTest, "VaRest.Http.Transport.ActivityFollowsTotal",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportTimeoutsTest::RunTest(const FString& Parameters)
{
	using VaRest::Http::EffectiveActivityTimeout;
	// Review I-5. IHttpRequest has GetTimeout (IHttpRequest.h:341-347) but no activity getter, so the applied value is
	// the function the transport calls, tested here
	TestEqual(TEXT("a total timeout alone also bounds silence, so the engine's 30 s default cannot fire first"), EffectiveActivityTimeout(60.f, 0.f), 60.f);
	TestEqual(TEXT("an explicit activity timeout wins"), EffectiveActivityTimeout(60.f, 5.f), 5.f);
	TestEqual(TEXT("neither: VaRest sets none (the engine's defaults apply)"), EffectiveActivityTimeout(0.f, 0.f), 0.f);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportThreadTest, "VaRest.Http.Transport.CompletesOffGameThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// curl reports the refused connection as a Warning with its own text; how many lines depends on curl
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = TEXT("http://127.0.0.1:1/"); // port 1: nothing listens, the connection is refused at once
	Request.Timeout = 10.f;
	VaRest::Http::GetDefaultTransport()->Start(MoveTemp(Request), [Done](FVaRestTransportResult&& Result)
	{
		Done->bOnGameThread = IsInGameThread();
		Done->Result = MoveTemp(Result);
		++Done->Calls;
		Done->bDone = true;
	});
	TestTrue(TEXT("completes"), VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, 15.0));
	// Spec 9 risk: HTTP-thread completion per backend. This is the Linux (curl) check; docs/dev/http-thread-completion.md lists the others.
	TestFalse(TEXT("CompleteOnHttpThread: the completion runs on the HTTP thread, not the game thread"), Done->bOnGameThread);
	TestTrue(TEXT("a refused connection is a Connection failure"), Done->Result.Failure == EVaRestTransportFailure::Connection);
	TestEqual(TEXT("no status"), Done->Result.Status, 0);
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.2);
	TestEqual(TEXT("exactly once"), Done->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportRefusedTest, "VaRest.Http.Transport.RefusedRequestCompletesOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportRefusedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// FHttpRequestCommon::PreCheck refuses an empty URL (HttpRequestCommon.cpp:96-101) and still finishes the request,
	// from an HTTP-thread task under CompleteOnHttpThread (HttpRequestCommon.cpp:224-237)
	AddExpectedMessagePlain(TEXT("No URL was specified"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	VaRest::Http::GetDefaultTransport()->Start(MoveTemp(Request), [Done](FVaRestTransportResult&& Result)
	{
		Done->bOnGameThread = IsInGameThread();
		Done->Result = MoveTemp(Result);
		++Done->Calls;
		Done->bDone = true;
	});
	TestTrue(TEXT("a refused request still completes"), VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, 5.0));
	TestTrue(TEXT("... as a failure"), Done->Result.Failure != EVaRestTransportFailure::None);
	VaRest::Http::Testing::PumpUntil([] { return false; }, 0.2);
	TestEqual(TEXT("... exactly once"), Done->Calls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportMockTest, "VaRest.Http.Transport.EngineMockResponse",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportMockTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	// Review M-15: an engine mock response (http.UrlPatternsToMockResponse, HttpManager.cpp:43-48, parsed at :773-839) is
	// appended to the engine's payload, not to the body stream (HttpRequestCommon.cpp:124-160, triggered at :190)
	TGuardConsoleVariable<FString> Mock(TEXT("http.UrlPatternsToMockResponse"), TEXT("mock.varest.test->200++{\"mocked\":true}++Content-Type:application/json"));
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = TEXT("http://mock.varest.test/x");
	VaRest::Http::GetDefaultTransport()->Start(MoveTemp(Request), [Done](FVaRestTransportResult&& Result)
	{
		Done->Result = MoveTemp(Result);
		++Done->Calls;
		Done->bDone = true;
	});
	TestTrue(TEXT("completes"), VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, 5.0));
	const FString Body = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Done->Result.Body.GetData()), Done->Result.Body.Num());
	TestTrue(TEXT("the mocked status"), Done->Result.Status == 200 && Done->Result.Failure == EVaRestTransportFailure::None);
	TestEqualSensitive(TEXT("the engine's payload is taken when the stream got nothing"), Body, FString(TEXT("{\"mocked\":true}")));
	TestTrue(TEXT("... with the slack ParseOwned needs"), Done->Result.Body.Max() > Done->Result.Body.Num());
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails on `VaRestBodySink.h`, `VaRestHttpTransportInternal.h` (no such file) and `VaRest::Http::GetDefaultTransport` (undeclared).

- [ ] **Step 3: The sink** — `Source/VaRestHttp/Private/VaRestBodySink.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Async/Mutex.h"

class IFileHandle;

namespace VaRest::Http
{
	/**
	 * Receives a response body on the transport's thread (IHttpRequest::SetResponseBodyReceiveStreamDelegateV2,
	 * IHttpRequest.h:273-284). In memory it reserves Content-Length + 1 on the first chunk, so FVaRestJson::ParseOwned
	 * appends its terminator without reallocating (spec 2.1) -- the engine's own buffer is reserved at exactly
	 * Content-Length (CurlHttp.cpp:631-633). The up-front reserve is capped at max(MaxBytes, ReserveCap) + 1 and the
	 * buffer grows past it, always keeping Num + 1 <= Max (reviews M-6, M13). With MaxBytes (a JSON result's cap) the
	 * write that passes it fails, which stops the transfer (IHttpRequest.cpp:36-44).
	 *
	 * For SaveTo it writes a file instead, created with its directory on the first byte. Those writes run on the HTTP
	 * thread, as the engine's own SetResponseBodyReceiveStream to a file archive does (decision 31): a slow disk stalls
	 * the shared HTTP thread. A bounded pipe could go behind this class later without an API change.
	 *
	 * Locked (review C-4): on Apple, Write runs on NSURLSession's delegate queue (AppleHttp.cpp:194-206, :884-888) while
	 * the completion reads on the HTTP thread, and the engine orders the two only through a plain property
	 * (AppleHttp.cpp:220-233, :948-952). Write runs inside the engine's stream lock (HttpRequestCommon.cpp:711-723), so the
	 * lock order is engine stream lock -> Mutex, never the reverse. TakeBytes and CloseFile seal the sink: a later Write
	 * fails. Uncontended, the lock costs about 20 ns per curl chunk of 16 KiB or more.
	 */
	class FBodySink
	{
	public:
		/** The up-front reserve without a larger JSON cap: 8 MiB */
		static constexpr int64 ReserveCap = 8 * 1024 * 1024;

		FBodySink() = default;
		/** MaxBytes: the result's JSON cap (0 = none) */
		explicit FBodySink(int64 InMaxBytes);
		explicit FBodySink(FString InFilePath, int64 InMaxBytes = 0);
		~FBodySink();

		/**
		 * Appends Length bytes. ContentLength: the response's, or 0 or less when unknown. False on failure: a file write,
		 * over 2 GiB in memory, past MaxBytes, or after the sink was sealed.
		 */
		bool Write(const void* Data, int64 Length, int64 ContentLength);
		/** Memory: the bytes, with at least one byte of slack. Seals the sink. */
		TArray<uint8> TakeBytes();
		/** File: flushes and closes it; false when any write failed. Seals the sink. */
		bool CloseFile();

		int64 Num() const;
		bool IsFile() const { return !FilePath.IsEmpty(); }
		bool HasError() const;
		/** The body passed MaxBytes */
		bool IsTooLarge() const;
		/** Tests: the memory buffer and its capacity, to prove what is reserved */
		const uint8* GetDataInternal() const;
		int32 GetCapacityInternal() const;

	private:
		mutable UE::FMutex Mutex;
		FString FilePath;
		int64 MaxBytes = 0;
		TUniquePtr<IFileHandle> File;
		TArray<uint8> Bytes;
		int64 Written = 0;
		bool bError = false;
		bool bTooLarge = false;
		bool bSealed = false;
	};
}
```

`Source/VaRestHttp/Private/VaRestBodySink.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBodySink.h"

#include "Async/UniqueLock.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/Paths.h"

VaRest::Http::FBodySink::FBodySink(int64 InMaxBytes)
	: MaxBytes(FMath::Max<int64>(0, InMaxBytes))
{
}

VaRest::Http::FBodySink::FBodySink(FString InFilePath, int64 InMaxBytes)
	: FilePath(MoveTemp(InFilePath))
	, MaxBytes(FMath::Max<int64>(0, InMaxBytes))
{
}

VaRest::Http::FBodySink::~FBodySink() = default;

bool VaRest::Http::FBodySink::Write(const void* Data, int64 Length, int64 ContentLength)
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	if (bError || bSealed)
	{
		return false;
	}
	if (Length <= 0)
	{
		return true;
	}
	const int64 NewNum = Written + Length;
	if (MaxBytes > 0 && NewNum > MaxBytes)
	{
		// Review M13: bounded memory on untrusted input -- the transfer stops here, the request ends ResponseTooLarge
		bError = true;
		bTooLarge = true;
		return false;
	}
	if (IsFile())
	{
		if (!File.IsValid())
		{
			IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
			PlatformFile.CreateDirectoryTree(*FPaths::GetPath(FilePath));
			File.Reset(PlatformFile.OpenWrite(*FilePath));
		}
		if (!File.IsValid() || !File->Write(static_cast<const uint8*>(Data), Length))
		{
			bError = true;
			return false;
		}
		Written = NewNum;
		return true;
	}

	if (NewNum + 1 > MAX_int32)
	{
		// TArray<uint8> holds int32 elements; SaveTo is the way for a larger body
		bError = true;
		return false;
	}
	if (Bytes.Max() == 0 && ContentLength > 0)
	{
		// Content-Length + 1: the terminator ParseOwned appends fits without a reallocation (spec 2.1). Capped, because a
		// Content-Length is a claim: 2000000000 would otherwise allocate 2 GB before a byte arrived (review M-6).
		const int64 Claimed = FMath::Min(ContentLength, FMath::Max(MaxBytes, ReserveCap));
		Bytes.Reserve(static_cast<int32>(FMath::Min<int64>(MAX_int32 - 1, FMath::Max(Claimed, NewNum)) + 1));
	}
	else if (NewNum + 1 > Bytes.Max())
	{
		// Unknown, wrong or capped length (chunked, compressed): grow geometrically, always leaving a byte free
		Bytes.Reserve(static_cast<int32>(FMath::Min<int64>(MAX_int32, FMath::Max<int64>(NewNum + 1, static_cast<int64>(Bytes.Max()) * 2))));
	}
	Bytes.Append(static_cast<const uint8*>(Data), static_cast<int32>(Length));
	Written = NewNum;
	return true;
}

TArray<uint8> VaRest::Http::FBodySink::TakeBytes()
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	bSealed = true;
	if (Bytes.Max() == Bytes.Num())
	{
		Bytes.Reserve(Bytes.Num() + 1);
	}
	return MoveTemp(Bytes);
}

bool VaRest::Http::FBodySink::CloseFile()
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	bSealed = true;
	if (File.IsValid())
	{
		if (!File->Flush())
		{
			bError = true;
		}
		File.Reset();
	}
	return !bError;
}

int64 VaRest::Http::FBodySink::Num() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return Written;
}

bool VaRest::Http::FBodySink::HasError() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return bError;
}

bool VaRest::Http::FBodySink::IsTooLarge() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return bTooLarge;
}

const uint8* VaRest::Http::FBodySink::GetDataInternal() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return Bytes.GetData();
}

int32 VaRest::Http::FBodySink::GetCapacityInternal() const
{
	UE::TUniqueLock<UE::FMutex> Lock(Mutex);
	return Bytes.Max();
}
```

- [ ] **Step 4: The transport** — append to `Source/VaRestHttp/Public/VaRestTransport.h`:

```cpp
namespace VaRest::Http
{
	/** The FHttpModule transport (spec 0.4: "UE's FHttpModule is the transport"); every client uses it unless WithTransport replaces it */
	VARESTHTTP_API TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> GetDefaultTransport();
}
```

`Source/VaRestHttp/Private/VaRestHttpTransportInternal.h` (what the transport's unit tests reach):

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Serialization/Archive.h"
#include "VaRestTransport.h"

namespace VaRest::Http
{
	/**
	 * Decision 30 (review I-5): the activity timeout the transport sets. With a total timeout and no activity timeout the
	 * engine would still apply its own default (GetActivityTimeoutOrDefault -> [HTTP] HttpActivityTimeout = 30,
	 * HttpRequestCommon.cpp:733-736, HttpModule.cpp:98), so .Timeout(60) against a server silent for 40 s would fail at 30 s
	 * as a connection error. Spec 4.7: the request's total timeout governs. 0 = set none.
	 */
	float EffectiveActivityTimeout(float TotalSeconds, float ActivitySeconds);

	/**
	 * Decision 29 (review I-2): an upload that reports progress while it goes. Under CompleteOnHttpThread curl calls the
	 * progress delegate only from the body receive callback and at the finish (CurlHttp.cpp:453-467, :1274), and its
	 * upload callback reports nothing (CurlHttp.cpp:668-683): BytesSent stays 0 for a whole upload and UploadDoneAt would
	 * be stamped at the first response byte. So when progress is asked for (OnProgress, or trace metadata, IN-11) the body
	 * goes through this archive and IHttpRequest::SetContentFromStream.
	 *
	 * Engine facts (UE 5.8.1): curl reads a stream payload through CURLOPT_READFUNCTION -> UploadCallback ->
	 * FRequestPayloadInFileStream::FillOutputBuffer (CurlHttp.cpp:421-429, :668-683, :996-1001; HttpRequestPayload.cpp:77-97),
	 * which serializes straight into curl's buffer on the HTTP thread (IHttpRequest.h:250-257, the note at :252): the same one copy an
	 * in-memory payload makes. Its length is TotalSize() (HttpRequestPayload.cpp:53-56), sent as Content-Length
	 * (CurlHttp.cpp:896-902) and CURLOPT_POSTFIELDSIZE/INFILESIZE (:952, :962, :987): no chunked encoding. A stream is not
	 * seekable (:390) where memory is (:348-349), but curl gets a seek callback only with [HTTP.Curl] bAllowSeekFunction,
	 * false by default (:1051, BaseEngine.ini), so a rewind fails the same way either way. A streamed POST or DELETE needs a
	 * Content-Type (:945, :983); Resolve always sets one for a body (Task 8) and the transport stage refuses an empty one
	 * (Task 8, review I3). Apple sizes a stream from TotalSize() too (AppleHttp.cpp:633, :727) and copies an in-memory body
	 * into NSData anyway (:536, :553).
	 *
	 * One reader at a time (curl's read callback, or NSURLSession's stream thread); the probe's atomics are read on the game
	 * thread. A file is opened on the first read, on the reading thread, never on the game thread.
	 */
	class FCountingUpload final : public FArchive
	{
	public:
		using FProbeRef = TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe>;
		using FClockPtr = TSharedPtr<IVaRestClock, ESPMode::ThreadSafe>;

		FCountingUpload(TArray<uint8>&& InBytes, FProbeRef InProbe, FClockPtr InClock, bool bInStamp);
		FCountingUpload(TSharedRef<FArchive, ESPMode::ThreadSafe> InInner, int64 InTotal, FProbeRef InProbe, FClockPtr InClock, bool bInStamp);
		FCountingUpload(FString InFilePath, int64 InTotal, FProbeRef InProbe, FClockPtr InClock, bool bInStamp);
		virtual ~FCountingUpload() override;

		virtual void Serialize(void* Data, int64 Length) override;
		virtual void Seek(int64 InPos) override;
		virtual int64 Tell() override { return Position; }
		virtual int64 TotalSize() override { return Total; }
		virtual FString GetArchiveName() const override { return TEXT("VaRest upload"); }

	private:
		void Count(int64 Length);

		TArray<uint8> Bytes;
		TSharedPtr<FArchive, ESPMode::ThreadSafe> Inner;
		FString FilePath;
		TUniquePtr<FArchive> File;
		int64 Total = 0;
		int64 Position = 0;
		FProbeRef Probe;
		FClockPtr Clock;
		bool bStamp = false;
	};
}
```

`Source/VaRestHttp/Private/VaRestHttpTransport.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTransport.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "VaRestBodySink.h"
#include "VaRestHttpTransportInternal.h"
#include "VaRestTrace.h"

namespace VaRest::Http::HttpTransportPrivate
{
	EVaRestTransportFailure MapFailure(EHttpFailureReason Reason)
	{
		switch (Reason)
		{
		case EHttpFailureReason::None:
			return EVaRestTransportFailure::None;
		case EHttpFailureReason::ConnectionError:
			// Includes curl's CURLE_OPERATION_TIMEDOUT (CurlHttp.cpp:1320-1329) and an engine activity timeout
			// (HttpRequestCommon.cpp:303-305): decision 19
			return EVaRestTransportFailure::Connection;
		case EHttpFailureReason::Cancelled:
			return EVaRestTransportFailure::Canceled;
		case EHttpFailureReason::TimedOut:
			// The total timeout (HttpRequestCommon.cpp:299-301)
			return EVaRestTransportFailure::Timeout;
		default:
			return EVaRestTransportFailure::Other;
		}
	}

	EHttpRequestPriority MapPriority(EVaRestPriority Priority)
	{
		switch (Priority)
		{
		case EVaRestPriority::Highest: return EHttpRequestPriority::Highest;
		case EVaRestPriority::High: return EHttpRequestPriority::High;
		case EVaRestPriority::Low: return EHttpRequestPriority::Low;
		default: return EHttpRequestPriority::Normal;
		}
	}

	/** Two writers can report BytesSent (the counting archive and the engine's progress delegate): keep the larger */
	void AtomicMax(std::atomic<int64>& Target, int64 Value)
	{
		int64 Seen = Target.load();
		while (Value > Seen && !Target.compare_exchange_weak(Seen, Value))
		{
		}
	}

	/** Shared by the engine delegates of one request; completes exactly once */
	struct FCompletion
	{
		FCompletion(FVaRestTransportComplete&& InOnComplete, TSharedRef<FBodySink, ESPMode::ThreadSafe> InSink,
			TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> InProbe, TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> InClock)
			: OnComplete(MoveTemp(InOnComplete))
			, Sink(MoveTemp(InSink))
			, Probe(MoveTemp(InProbe))
			, Clock(MoveTemp(InClock))
		{
		}

		/**
		 * Review I-3: the engine does not always call back. FHttpManager::Shutdown calls Request->Shutdown() on every
		 * outstanding request (HttpManager.cpp:164-169), which unbinds the delegates (HttpRequestImpl.cpp:32-38); with [HTTP]
		 * bUseNullHttp, CreateRequest returns a request that never completes (HttpModule.cpp:377-380, NullHttp.cpp:142-149).
		 * Either way this object dies with the request: finish it. At shutdown the posted game-thread task never runs, which
		 * is harmless.
		 */
		~FCompletion()
		{
			if (!bDone.exchange(true) && OnComplete)
			{
				FVaRestTransportResult Dropped;
				Dropped.Failure = EVaRestTransportFailure::Other;
				Dropped.FailureDetail = TEXT("TransportDropped");
				OnComplete(MoveTemp(Dropped)); // only posts a game-thread task
			}
		}

		double Now() const
		{
			return Clock.IsValid() ? Clock->Now() : FPlatformTime::Seconds();
		}

		FVaRestTransportComplete OnComplete;
		TSharedRef<FBodySink, ESPMode::ThreadSafe> Sink;
		TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
		std::atomic<bool> bDone{ false };
		/** Stamp transport times (trace metadata, IN-11) */
		bool bStamp = false;
		/** The body went through FCountingUpload: BytesUp is what was sent, not what was meant (review M-18) */
		bool bCountedUpload = false;
		int64 BytesUp = 0;
	};

	class FCall final : public IVaRestTransportCall
	{
	public:
		FCall(TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> InRequest, TWeakPtr<FCompletion, ESPMode::ThreadSafe> InCompletion)
			: Request(MoveTemp(InRequest))
			, Completion(MoveTemp(InCompletion))
		{
		}

		virtual void Cancel() override
		{
			check(IsInGameThread());
			// Review M-5: a request already finished on the HTTP thread is left alone. For one that ended by an abort
			// (a timeout), curl's AbortRequest would run FinishRequestNotInHttpManager a second time (CurlHttp.cpp:1194-1209,
			// HttpRequestCommon.cpp:224-237): the engine's finish, stats and logs twice.
			const TSharedPtr<FCompletion, ESPMode::ThreadSafe> Done = Completion.Pin();
			if (!Done.IsValid() || Done->bDone.load())
			{
				return;
			}
			// The completion delegate then reports EHttpFailureReason::Cancelled (HttpRequestCommon.cpp:295-298)
			if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pinned = Request.Pin())
			{
				Pinned->CancelRequest();
			}
		}

	private:
		TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
		TWeakPtr<FCompletion, ESPMode::ThreadSafe> Completion;
	};

	class FHttpModuleTransport final : public IVaRestTransport
	{
	public:
		virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) override;
	};

	TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> FHttpModuleTransport::Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete)
	{
		check(IsInGameThread());
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
		Http->SetVerb(Request.Verb);
		Http->SetURL(Request.Url);
		for (const FVaRestHeaders::FPair& Pair : Request.Headers)
		{
			// SetHeader replaces; a repeated name is joined with ", " by AppendToHeader (IHttpRequest.h:299-310)
			if (Http->GetHeader(Pair.Key).IsEmpty())
			{
				Http->SetHeader(Pair.Key, Pair.Value);
			}
			else
			{
				Http->AppendToHeader(Pair.Key, Pair.Value);
			}
		}

		const TSharedRef<FBodySink, ESPMode::ThreadSafe> Sink = Request.ResponseFile.IsEmpty()
			? MakeShared<FBodySink, ESPMode::ThreadSafe>(Request.MaxBodyBytes)
			: MakeShared<FBodySink, ESPMode::ThreadSafe>(Request.ResponseFile, Request.MaxBodyBytes);
		const TSharedRef<FCompletion, ESPMode::ThreadSafe> Completion = MakeShared<FCompletion, ESPMode::ThreadSafe>(MoveTemp(OnComplete), Sink, Request.Probe, Request.Clock);
		Completion->bStamp = Request.bReportStatusTime;
		Completion->BytesUp = Request.Probe->UploadTotal.load();

		bool bBodyAccepted = true;
		const int64 UploadTotal = Request.Probe->UploadTotal.load();
		if (Request.bReportProgress && UploadTotal > 0)
		{
			// Decision 29: counted as curl reads it (FCountingUpload). Only here: the default path stays SetContent(TArray&&).
			TSharedPtr<FCountingUpload, ESPMode::ThreadSafe> Counted;
			if (!Request.BodyFile.IsEmpty())
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MoveTemp(Request.BodyFile), UploadTotal, Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			else if (Request.BodySegments.Num() > 0)
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Request.BodySegments)),
					UploadTotal, Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			else
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MoveTemp(Request.Body), Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			Completion->bCountedUpload = true;
			bBodyAccepted = Http->SetContentFromStream(Counted.ToSharedRef());
		}
		else if (!Request.BodyFile.IsEmpty())
		{
			bBodyAccepted = Http->SetContentAsStreamedFile(Request.BodyFile);
		}
		else if (Request.BodySegments.Num() > 0)
		{
			// Serialize runs on the HTTP thread (IHttpRequest.h:250-257); the stream owns its segments
			bBodyAccepted = Http->SetContentFromStream(MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Request.BodySegments)));
		}
		else if (Request.Body.Num() > 0)
		{
			// Spec 2.4: the body produced in one pass is moved, not copied (IHttpRequest.h:225-233)
			Http->SetContent(MoveTemp(Request.Body));
		}
		if (!bBodyAccepted)
		{
			Completion->bDone = true;
			FVaRestTransportResult Result;
			Result.Failure = EVaRestTransportFailure::Other;
			Result.FailureDetail = TEXT("BodyRejected");
			FVaRestTransportComplete Callback = MoveTemp(Completion->OnComplete);
			Callback(MoveTemp(Result));
			return MakeShared<FCall, ESPMode::ThreadSafe>(TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>(Http), Completion);
		}
		if (Request.Timeout > 0.f)
		{
			Http->SetTimeout(Request.Timeout);
		}
		const float Activity = EffectiveActivityTimeout(Request.Timeout, Request.ActivityTimeout);
		if (Activity > 0.f)
		{
			Http->SetActivityTimeout(Activity);
		}
		Http->SetPriority(MapPriority(Request.Priority));
		// Spec 1: complete on the HTTP thread; the completion below only moves bytes and dispatches
		Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);

		const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakHttp = Http;
		Http->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Completion, WeakHttp](void* Ptr, int64& InOutLength)
		{
			int64 ContentLength = -1;
			if (Completion->Sink->Num() == 0)
			{
				// The first chunk: the headers are in (curl creates the response before passing body bytes, CurlHttp.cpp:584)
				if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pinned = WeakHttp.Pin())
				{
					if (const FHttpResponsePtr Response = Pinned->GetResponse())
					{
						// Review M-16: curl decompresses (CURLOPT_ACCEPT_ENCODING "", CurlHttp.cpp:1016-1021), so with a
						// Content-Encoding the Content-Length counts the compressed bytes: the received total is unknown
						const bool bEncoded = !Response->GetHeader(TEXT("Content-Encoding")).IsEmpty();
						ContentLength = !bEncoded && Response->GetContentLength() > 0 ? static_cast<int64>(Response->GetContentLength()) : -1;
					}
				}
				Completion->Probe->DownloadTotal = ContentLength;
			}
			if (!Completion->Sink->Write(Ptr, InOutLength, ContentLength))
			{
				// A short count fails the request (IHttpRequest.cpp:36-44 sets the archive error; IHttpRequest.h:274-275)
				InOutLength = 0;
			}
		}));

		if (Request.bReportProgress)
		{
			++FVaRestTrace::Counters().DelegateBinds;
			Http->OnRequestProgress64().BindLambda([Completion](FHttpRequestPtr, uint64 Sent, uint64 Received)
			{
				FVaRestTransportProbe& Probe = *Completion->Probe;
				AtomicMax(Probe.BytesSent, static_cast<int64>(Sent));
				Probe.BytesReceived = static_cast<int64>(Received);
				if (Completion->bStamp && Probe.UploadDoneAt.load() < 0.0 && static_cast<int64>(Sent) >= Probe.UploadTotal.load())
				{
					Probe.UploadDoneAt = Completion->Now();
				}
			});
		}
		if (Request.bReportStatusTime)
		{
			++FVaRestTrace::Counters().DelegateBinds;
			Http->OnStatusCodeReceived().BindLambda([Completion](FHttpRequestPtr, int32)
			{
				if (Completion->Probe->StatusAt.load() < 0.0)
				{
					Completion->Probe->StatusAt = Completion->Now();
				}
			});
		}

		Http->OnProcessRequestComplete().BindLambda([Completion](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
		{
			// The HTTP thread. Read what the engine holds, take the body from our sink, hand the result on: no parsing here.
			if (Completion->bDone.exchange(true))
			{
				return;
			}
			FVaRestTransportResult Result;
			if (Resp.IsValid())
			{
				Result.Status = Resp->GetResponseCode();
				Result.Headers = FVaRestHeaders::FromLines(Resp->GetAllHeaders());
				Result.EffectiveUrl = Resp->GetEffectiveURL();
				Result.ContentType = Resp->GetContentType();
			}
			// "Received", not "Sink": a lambda local may not hide an enclosing local (MSVC C4456 is an error here, review I2)
			FBodySink& Received = *Completion->Sink;
			if (Resp.IsValid() && Received.Num() == 0 && Resp->GetContent().Num() > 0)
			{
				// Review M-15: engine mock responses append to the engine's payload, not to the stream
				// (HttpRequestCommon.cpp:124-160); take it, through the sink, so the slack and the cap still hold
				const TArray<uint8>& Engine = Resp->GetContent();
				Received.Write(Engine.GetData(), Engine.Num(), Engine.Num());
			}
			if (!bSucceeded || !Resp.IsValid())
			{
				const EHttpFailureReason Reason = Req.IsValid() ? Req->GetFailureReason() : EHttpFailureReason::Other;
				Result.Failure = MapFailure(Reason);
				if (Result.Failure == EVaRestTransportFailure::None)
				{
					Result.Failure = EVaRestTransportFailure::Other;
				}
				Result.FailureDetail = FName(LexToString(Reason));
			}
			if (Received.IsFile())
			{
				Result.bBodyInFile = true;
				if (!Received.CloseFile() && Result.Failure == EVaRestTransportFailure::None)
				{
					Result.Failure = EVaRestTransportFailure::Other;
					Result.FailureDetail = TEXT("ResponseFileWriteFailed");
				}
			}
			else
			{
				Result.Body = Received.TakeBytes();
			}
			if (Received.IsTooLarge())
			{
				// Review M13: the sink stopped the transfer at the result's JSON cap; whatever the engine then reported
				Result.Failure = EVaRestTransportFailure::Other;
				Result.FailureDetail = TEXT("ResponseTooLarge");
				Result.Body.Empty();
			}
			else if (Received.HasError() && Result.Failure == EVaRestTransportFailure::None)
			{
				Result.Failure = EVaRestTransportFailure::Other;
				Result.FailureDetail = TEXT("ResponseBodyRejected");
			}
			// Review M-18: with a counted upload, what was sent; otherwise the body's size
			Result.BytesUp = Completion->bCountedUpload ? Completion->Probe->BytesSent.load() : Completion->BytesUp;
			Result.BytesDown = Received.Num();
			if (Completion->bStamp)
			{
				Result.TransportDoneAt = Completion->Now();
			}
			FVaRestTransportComplete Callback = MoveTemp(Completion->OnComplete);
			Callback(MoveTemp(Result));
		});

		// The completion delegate runs even when ProcessRequest refuses to start ("always called when the request completes
		// or on error", IHttpRequest.h:351): the refusal arrives there as a failure, so the return value adds nothing.
		// When the engine drops it instead, ~FCompletion finishes the request (review I-3).
		Http->ProcessRequest();
		return MakeShared<FCall, ESPMode::ThreadSafe>(WeakHttp, Completion);
	}
}

float VaRest::Http::EffectiveActivityTimeout(float TotalSeconds, float ActivitySeconds)
{
	return ActivitySeconds > 0.f ? ActivitySeconds : FMath::Max(TotalSeconds, 0.f);
}

VaRest::Http::FCountingUpload::FCountingUpload(TArray<uint8>&& InBytes, FProbeRef InProbe, FClockPtr InClock, bool bInStamp)
	: Bytes(MoveTemp(InBytes))
	, Total(Bytes.Num())
	, Probe(MoveTemp(InProbe))
	, Clock(MoveTemp(InClock))
	, bStamp(bInStamp)
{
	SetIsLoading(true);
}

VaRest::Http::FCountingUpload::FCountingUpload(TSharedRef<FArchive, ESPMode::ThreadSafe> InInner, int64 InTotal, FProbeRef InProbe, FClockPtr InClock, bool bInStamp)
	: Inner(MoveTemp(InInner))
	, Total(InTotal)
	, Probe(MoveTemp(InProbe))
	, Clock(MoveTemp(InClock))
	, bStamp(bInStamp)
{
	SetIsLoading(true);
}

VaRest::Http::FCountingUpload::FCountingUpload(FString InFilePath, int64 InTotal, FProbeRef InProbe, FClockPtr InClock, bool bInStamp)
	: FilePath(MoveTemp(InFilePath))
	, Total(InTotal)
	, Probe(MoveTemp(InProbe))
	, Clock(MoveTemp(InClock))
	, bStamp(bInStamp)
{
	SetIsLoading(true);
}

VaRest::Http::FCountingUpload::~FCountingUpload() = default;

void VaRest::Http::FCountingUpload::Serialize(void* Data, int64 Length)
{
	if (Length <= 0)
	{
		return;
	}
	if (Position + Length > Total)
	{
		SetError();
		return;
	}
	if (Inner.IsValid())
	{
		Inner->Seek(Position);
		Inner->Serialize(Data, Length);
		if (Inner->IsError())
		{
			SetError();
			return;
		}
	}
	else if (!FilePath.IsEmpty())
	{
		if (!File.IsValid())
		{
			// On the reading thread (the HTTP thread for curl): never disk IO on the game thread
			File.Reset(IFileManager::Get().CreateFileReader(*FilePath));
		}
		if (!File.IsValid())
		{
			SetError();
			return;
		}
		File->Seek(Position);
		File->Serialize(Data, Length);
		if (File->IsError())
		{
			SetError();
			return;
		}
	}
	else
	{
		FMemory::Memcpy(Data, Bytes.GetData() + Position, static_cast<SIZE_T>(Length));
	}
	Count(Length);
}

void VaRest::Http::FCountingUpload::Seek(int64 InPos)
{
	Position = FMath::Clamp<int64>(InPos, 0, Total);
}

void VaRest::Http::FCountingUpload::Count(int64 Length)
{
	Position += Length;
	HttpTransportPrivate::AtomicMax(Probe->BytesSent, Position);
	if (bStamp && Position == Total && Probe->UploadDoneAt.load() < 0.0)
	{
		Probe->UploadDoneAt = Clock.IsValid() ? Clock->Now() : FPlatformTime::Seconds();
	}
}

TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> VaRest::Http::GetDefaultTransport()
{
	static TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Instance = MakeShared<HttpTransportPrivate::FHttpModuleTransport, ESPMode::ThreadSafe>();
	return Instance;
}
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.Transport.MemorySinkSlack`, `.FileSink`, `.CountingUpload`, `.ActivityFollowsTotal`, `.CompletesOffGameThread`, `.RefusedRequestCompletesOnce`, `.EngineMockResponse`.

- [ ] **Step 6: Restore-the-bug (slack)**

In `FBodySink::Write`, temporarily reserve `Claimed` instead of `Claimed + 1` and make `TakeBytes` return `MoveTemp(Bytes)` without its reserve. Run `Tools/run_tests.sh VaRest.Http.Transport`; expected FAIL in `VaRest.Http.Transport.MemorySinkSlack` ("one byte of slack"). Restore; PASS. Report both runs.

- [ ] **Step 7: Restore-the-bug (completion thread)**

Temporarily change the policy to `EHttpRequestDelegateThreadPolicy::CompleteOnGameThread`. Run `Tools/run_tests.sh VaRest.Http.Transport`; expected FAIL in `VaRest.Http.Transport.CompletesOffGameThread`: a game-thread completion comes from the HTTP manager's tick (`HttpManager.cpp:537-544`), which this `PumpUntil` does not run, so "completes" times out. Restore; PASS. Report both runs.

- [ ] **Step 8: Restore-the-bug (the reserve cap and the JSON cap, reviews M-6, M13)**

In `FBodySink::Write`, temporarily use `const int64 Claimed = ContentLength;` and delete the `MaxBytes > 0 && NewNum > MaxBytes` block. Run `Tools/run_tests.sh VaRest.Http.Transport`; expected FAIL in `VaRest.Http.Transport.MemorySinkSlack` ("a huge Content-Length reserves at most ReserveCap + 1", "past the cap the write fails"). Restore; PASS. Report both runs.

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestTransport.h Source/VaRestHttp/Private/VaRestBodySink.h Source/VaRestHttp/Private/VaRestBodySink.cpp \
  Source/VaRestHttp/Private/VaRestHttpTransportInternal.h Source/VaRestHttp/Private/VaRestHttpTransport.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpTransportTest.cpp
git commit -m "http: FHttpModule transport -- HTTP-thread completion, a locked body sink with slack and caps, counted uploads, dropped completions finish

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 8: `FVaRestClient`, the exchange and the stage pipeline (spec §1, §3.1–§3.3, §4)

The heart of M3. Spec §4: "Each request is an `FVaRestExchange` that flows through a fixed chain. A stage is asynchronous middleware: it may pass the exchange on, observe or alter the outcome, short-circuit (cache hit, queued), or re-issue (retry, auth replay)." This task builds the mechanism with the stages M3 needs — custom `Outer` stages, the attempt boundary, custom `PerAttempt` stages, the transport (the limiter, Task 12, and the log stage, Task 14, slot in before it) — and the life of an exchange:

1. `Send()` on any thread assigns the id (IN-10), snapshots the configuration (spec §3.1: "A request snapshots the configuration at `Send()`"; decision 27), moves the builder into the exchange, and enters on the game thread, directly or through a game-thread task (spec §1).
2. `Enter` resolves the URL and headers, and runs the chain.
3. The transport completes on any thread; the completion only posts a game-thread task (spec §1).
4. `OnChainDone` classifies the outcome. A body to decode goes to a `UE::Tasks` task, with the typed handler moved along, so no other thread touches it meanwhile.
5. `Complete` runs on the game thread, exactly once, never inside `Send()` (decision 3), and delivers. Deliveries are game-thread tasks, which the game thread also runs between tick groups, so a callback can run mid-frame, not only at a frame boundary (decision 32); every delivery goes through one helper, so M5 can change that in one place.

Every `TSharedPtr` that crosses to another thread is moved into the game-thread continuation, never copied, so an exchange and its client state die on the game thread; out-of-line destructors send what is left there as a backstop (review I-1). A transport that destroys its completion uncalled still finishes the request (`TransportDropped`, review I-3).

Configuration is an immutable `FClientConfig` swapped under a lock, so a builder made on a worker reads a consistent copy. In-flight exchanges are owned by the client state and keep it alive (spec §3.2: "in-flight exchanges are owned by the client state, and they keep the client state alive. No `AddToRoot` anywhere"). The typed half of a request is `TResultHandler<T>`; every result kind of spec §3.2 is here: `FVaRestJson` (default), `void`, `FString`, `TArray<uint8>`, `FVaRestResponse`, any `USTRUCT`. The struct path is tested in Task 10, where the test types module arrives.

**Files:**
- Create: `Source/VaRestHttp/Public/VaRestResultHandler.h`, `Source/VaRestHttp/Public/VaRestStage.h`, `Source/VaRestHttp/Public/VaRestClient.h`
- Create: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp`, `Source/VaRestHttp/Private/VaRestPipeline.cpp`, `Source/VaRestHttp/Private/VaRestDecode.h`, `Source/VaRestHttp/Private/VaRestDecode.cpp`, `Source/VaRestHttp/Private/VaRestClient.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestHttpTesting.h`, `Source/VaRestHttp/Private/VaRestHttpTesting.cpp` (outcome recorder and test rig)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpPipelineTest.cpp`

**Interfaces:**
- Consumes: Tasks 2–7 (types, URL functions, bodies, trace ids, redactor, clock, transport, fake, `GetDefaultTransport`); `FVaRestJson::ParseOwned`, `FVaRestJson::Parse(TConstArrayView<uint8>, const FVaRestJsonParseOptions&)`, `FVaRestJson::TryToUtf8`, `FVaRestJsonParseOptions::MaxInputBytes`, `FVaRestJsonConvert::JsonToStruct`, `FVaRestJsonConvertOptions`, `FVaRestJsonDecodeReport`, `VaRest::Json::bHasScriptStruct<T>`, `VaRest::Json::GetScriptStruct<T>()`, `VaRest::Json::AppendRepairedUtf8` (VaRestJson).
- Produces (public):
  - `VaRestResultHandler.h`: `struct VaRest::Http::FFinish { Response, Error, Body, bBodyInFile, TempFile, SaveTo, bKeepBody, DecodeOptions, ParseOptions, bTimeDecode, DecodeStartAt, DecodeEndAt, Clock }`; `class IResultHandler { GetResultType, DecodesBody, DecodesJson, Decode, Deliver, WantsProgress, Progress }`; `DecodeJson(FFinish&, FVaRestJson&)`, `DecodeStruct(FFinish&, const UScriptStruct*, void*)`, `DecodeText(FFinish&, FString&)`, `ReleaseBody(FFinish&)`; `TResultHandler<T>` (+ `<void>`) with `OnSuccess`, `OnError`, `OnComplete`, `OnProgress` (`TUniqueFunction`s).
  - `VaRestStage.h`: `struct FVaRestAttemptOutcome { Status, Headers, EffectiveUrl, ContentType, Body, bBodyInFile, Failure, FailureDetail, BytesUp, BytesDown; static Canceled(); IsTransportFailure() }`; `using FVaRestOutcomeCallback = TUniqueFunction<void(FVaRestAttemptOutcome&&)>`; `using FVaRestStageRef = TSharedRef<IVaRestStage, ESPMode::ThreadSafe>`; `using FVaRestStageChain = TArray<FVaRestStageRef>`; `using FVaRestExchangeRef = TSharedRef<FVaRestExchange, ESPMode::ThreadSafe>`; `class FVaRestStageNext { Proceed(const FVaRestExchangeRef&, FVaRestOutcomeCallback&&) const }`; `class IVaRestStage { GetStageName() const; virtual bool MayReissue() const { return false; } Process(const FVaRestExchangeRef&, const FVaRestStageNext&, FVaRestOutcomeCallback&&) }`; `enum class EVaRestExchangePhase { Created, InChain, Decoding, Done }`; `class FVaRestExchange` (out-of-line destructor; stage accessors `GetRequestId`, `GetClientId`, `GetVerb`, `GetUrl`, `GetPathTemplate`, `GetTag`, `GetPriority`, `IsIdempotent`, `GetRequestHeaders`, `GetAttemptHeaders`, `GetBody`, `GetAttempt`, `IsCancelRequested`, `IsDone`; internal fields listed in the header).
  - `VaRestClient.h`: `struct VaRest::Http::FRequestSpec`; `VaRest::Http::SendRequest(TUniquePtr<FRequestSpec>&&, const char* SourceFile, uint32 SourceLine)`; `FVaRestRequestHandle { GetId(), IsActive() }` (`Cancel()` arrives in Task 11); `template <typename T> class [[nodiscard]] TVaRestRequest` with `Path`, `Query` (string and integral overloads), `Header`, `Body(const FVaRestJson&)`, `Body(FStringView)`, `Body(TArray<uint8>&&)`, `Body(TConstArrayView<uint8>)`, `Tag`, `Priority`, `OnSuccess`, `OnError`, `OnComplete`, `Send(UE::FSourceLocation = Current())`; `FVaRestClient` (`static Make(FStringView BaseUrl)`, `IsValid`, `GetId`, `GetBaseUrl`, `GetInFlightCount`, `WithName`, `WithBaseUrl`, `WithHeader`, `WithoutHeader`, `WithNaming`, `WithTransport`, `WithClock`, `Use(FVaRestStageRef, EVaRestStageSlot)`, `Get/Post/Put/Patch/Delete/Head<T>(Path)`, `Request<T>(Verb, Path)`, `GetStateInternal()`).
  - `VaRestHttpTesting.h` additions: `template <typename T> struct TTestOutcome` (atomics `SuccessCalls`, `ErrorCalls`, `CompleteCalls`, `bAllOnGameThread`; `Value`, `Error`, `Response`; `IsDone()`), `MakeOutcome<T>()`, `Observe(TVaRestRequest<T>&&, const TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe>&)`, `struct FTestRig { Clock, Fake, Client; explicit FTestRig(FStringView BaseUrl = "https://api.test"); Wait(Outcome, Seconds) }`.
  - Private (`VaRestClientState.h`): `struct FClientConfig`, `class FClientState` (`Create`, `GetClientId`, `GetConfig`, `EditConfig`, `Enter`, `OnAttemptDone`, `NumInFlight`, `static ForEachLive`), `MakeAttemptBoundaryStage()`, `MakeTransportStage()`; (`VaRestDecode.h`) `ProcessErrorBody(FFinish&)`, `RawBodyText(TConstArrayView<uint8>)`, `RawBodyCap = 4096`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpPipelineTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::PipelineTestPrivate
{
	using FLog = TSharedRef<TArray<FString>, ESPMode::ThreadSafe>;

	/** Logs what it sees; PerAttempt instances can sign; an Outer one can re-issue or answer by itself */
	class FRecordingStage final : public IVaRestStage
	{
	public:
		enum class EMode : uint8
		{
			Pass,
			Sign,
			Reissue,
			Answer,
			/** Reports the outcome twice (a buggy stage, review C-3) */
			Twice
		};

		FRecordingStage(FString InLabel, FLog InLog, EMode InMode = EMode::Pass)
			: Label(MoveTemp(InLabel))
			, Log(MoveTemp(InLog))
			, Mode(InMode)
		{
		}

		virtual FName GetStageName() const override { return FName(*Label); }
		virtual bool MayReissue() const override { return Mode == EMode::Reissue; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Log->Add(FString::Printf(TEXT("%s>%d"), *Label, Exchange->GetAttempt()));
			if (Mode == EMode::Answer)
			{
				FVaRestAttemptOutcome Cached;
				Cached.Status = 200;
				Cached.ContentType = TEXT("application/json");
				Cached.Headers.Set(TEXT("Content-Type"), TEXT("application/json"));
				Cached.Body = { '{', '"', 'c', '"', ':', '1', '}' };
				OnOutcome(MoveTemp(Cached)); // synchronously, inside Send(): the core must defer the callbacks
				return;
			}
			if (Mode == EMode::Sign)
			{
				Exchange->GetAttemptHeaders().Set(TEXT("X-Signed"), FString::Printf(TEXT("attempt-%d"), Exchange->GetAttempt()));
			}
			FLog Recorder = Log;
			FString Name = Label;
			if (Mode == EMode::Twice)
			{
				Next.Proceed(Exchange, [Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
				{
					FVaRestAttemptOutcome Again = Outcome;
					Done(MoveTemp(Outcome));
					Done(MoveTemp(Again));
				});
				return;
			}
			if (Mode == EMode::Reissue)
			{
				// What M4's retry does: a second trip through the stages after this one, once the first outcome is back
				Next.Proceed(Exchange, [Recorder, Name, Exchange, Next, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& First) mutable
				{
					Recorder->Add(FString::Printf(TEXT("%s<%d"), *Name, First.Status));
					Next.Proceed(Exchange, MoveTemp(Done));
				});
				return;
			}
			Next.Proceed(Exchange, [Recorder, Name, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				Recorder->Add(FString::Printf(TEXT("%s<%d"), *Name, Outcome.Status));
				Done(MoveTemp(Outcome));
			});
		}

	private:
		FString Label;
		FLog Log;
		EMode Mode;
	};

	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineGetJsonTest, "VaRest.Http.Pipeline.GetJson",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineGetJsonTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, R"({"name":"neo","level":42})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/users/{id}")).Path(TEXT("id"), 7).Query(TEXT("lang"), TEXT("en")).Header(TEXT("X-Trace"), TEXT("t1")), Outcome).Send();
	TestTrue(TEXT("the handle has the request id"), Handle.GetId() != 0);
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestEqual(TEXT("one OnSuccess"), Outcome->SuccessCalls.load(), 1);
	TestEqual(TEXT("no OnError"), Outcome->ErrorCalls.load(), 0);
	TestEqual(TEXT("one OnComplete"), Outcome->CompleteCalls.load(), 1);
	TestTrue(TEXT("every callback on the game thread (spec 1)"), Outcome->bAllOnGameThread.load());
	TestTrue(TEXT("the decoded JSON"), Outcome->Value.IsSet() && Outcome->Value->Get(TEXT("level")).AsInt() == 42);
	const FVaRestTransportRequest& Sent = Rig.Fake->GetCall(0).Request;
	TestEqualSensitive(TEXT("verb"), Sent.Verb, FString(TEXT("GET")));
	TestEqualSensitive(TEXT("base URL + expanded path + query"), Sent.Url, FString(TEXT("https://api.test/users/7?lang=en")));
	TestEqualSensitive(TEXT("the request's header"), Sent.Headers.Get(TEXT("X-Trace")), FString(TEXT("t1")));
	TestEqual(TEXT("one attempt"), Outcome->Response.Attempts, 1);
	TestEqual(TEXT("status"), Outcome->Response.Status, 200);
	TestEqual(TEXT("request id in the response"), Outcome->Response.RequestId, Handle.GetId());
	TestEqual(TEXT("a successful decode releases the raw bytes (spec 3.2)"), Outcome->Response.Body.Num(), 0);
	TestFalse(TEXT("the handle is done"), Handle.IsActive());
	TestEqual(TEXT("nothing in flight"), Rig.Client.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineResultTypesTest, "VaRest.Http.Pipeline.ResultTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineResultTypesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("ignored")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 0, 1, 2, 255 }));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("café")));
	FVaRestFakeResponse Raw = FVaRestFakeResponse::Text(201, TEXT("raw"), TEXT("text/x-raw"));
	Raw.Headers.Add(TEXT("Set-Cookie"), TEXT("a=1"));
	Rig.Fake->Enqueue(Raw);

	auto None = MakeOutcome<void>();
	auto Bytes = MakeOutcome<TArray<uint8>>();
	auto Text = MakeOutcome<FString>();
	auto Response = MakeOutcome<FVaRestResponse>();
	Observe(Rig.Client.Delete<void>(TEXT("/a")), None).Send();
	Observe(Rig.Client.Get<TArray<uint8>>(TEXT("/b")), Bytes).Send();
	Observe(Rig.Client.Get<FString>(TEXT("/c")), Text).Send();
	Observe(Rig.Client.Get<FVaRestResponse>(TEXT("/d")), Response).Send();
	TestTrue(TEXT("all complete"), PumpUntil([&] { return None->IsDone() && Bytes->IsDone() && Text->IsDone() && Response->IsDone(); }));

	TestTrue(TEXT("void: success, the body released"), None->SuccessCalls == 1 && None->Response.Body.Num() == 0);
	TestTrue(TEXT("bytes: the body itself"), Bytes->Value.IsSet() && *Bytes->Value == TArray<uint8>({ 0, 1, 2, 255 }));
	TestEqualSensitive(TEXT("FString: UTF-8 decoded"), Text->Value.Get(FString()), FString(TEXT("café")));
	TestTrue(TEXT("FVaRestResponse: the raw response, body kept"), Response->Value.IsSet() && Response->Value->Status == 201
		&& Response->Value->Body.Num() == 3 && Response->Value->Headers.Get(TEXT("set-cookie")) == TEXT("a=1"));
	TestEqualSensitive(TEXT("verbs"), Rig.Fake->GetCall(0).Request.Verb, FString(TEXT("DELETE")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineErrorsTest, "VaRest.Http.Pipeline.Errors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineErrorsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(404, R"({"error":"missing"})"));
	Rig.Fake->Enqueue(Json(503, R"({})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(500, TEXT("<h1>oops</h1>"), TEXT("text/html")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Timeout, TEXT("TimedOut")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("BodyRejected")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("ResponseFileWriteFailed")));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Other, TEXT("TransportDropped")));
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	for (int32 i = 0; i < 8; ++i)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/e%d?token=secret-%d"), i, i)).Tag(TEXT("errs")), Outcomes.Last()).Send();
	}
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { for (const auto& O : Outcomes) { if (!O->IsDone()) { return false; } } return true; }));

	const FVaRestError& NotFound = Outcomes[0]->Error;
	TestTrue(TEXT("404: Kind Http, the status"), NotFound.Kind == EVaRestErrorKind::Http && NotFound.HttpStatus == 404);
	TestEqualSensitive(TEXT("... the JSON error body parsed (spec 3.3)"), NotFound.Body[TEXT("error")].AsString(), FString(TEXT("missing")));
	TestFalse(TEXT("... not retryable"), NotFound.bRetryable);
	TestTrue(TEXT("... a message naming the status"), NotFound.Message.Contains(TEXT("404")));
	TestEqualSensitive(TEXT("... the URL, redacted"), NotFound.Url, FString(TEXT("https://api.test/e0?token=***")));
	TestTrue(TEXT("... tag and attempts"), NotFound.Tag == FName(TEXT("errs")) && NotFound.Attempts == 1);
	TestTrue(TEXT("... the raw body kept on error (spec 3.2)"), Outcomes[0]->Response.Body.Num() > 0);
	TestEqual(TEXT("OnError once"), Outcomes[0]->ErrorCalls.load(), 1);
	TestEqual(TEXT("OnSuccess never"), Outcomes[0]->SuccessCalls.load(), 0);

	TestTrue(TEXT("503 is retryable (spec 4.1)"), Outcomes[1]->Error.bRetryable);
	const FVaRestError& Html = Outcomes[2]->Error;
	TestTrue(TEXT("a non-JSON error body: no Body, RawBody holds the text"), !Html.Body.IsValid() && Html.RawBody.Contains(TEXT("oops")));
	TestTrue(TEXT("no connection: Kind Connection, retryable"), Outcomes[3]->Error.Kind == EVaRestErrorKind::Connection && Outcomes[3]->Error.bRetryable && Outcomes[3]->Error.HttpStatus == 0);
	TestTrue(TEXT("a total timeout: Kind Timeout"), Outcomes[4]->Error.Kind == EVaRestErrorKind::Timeout && Outcomes[4]->Error.bRetryable);
	// Review M14: a local failure is not a network one -- M4's retry must not re-send onto a full disk
	TestTrue(TEXT("a body the engine refused: Kind Config, not retryable, with the detail"), Outcomes[5]->Error.Kind == EVaRestErrorKind::Config
		&& !Outcomes[5]->Error.bRetryable && Outcomes[5]->Error.Message.Contains(TEXT("BodyRejected")));
	TestTrue(TEXT("a file that cannot be written: Kind Decode, not retryable"), Outcomes[6]->Error.Kind == EVaRestErrorKind::Decode && !Outcomes[6]->Error.bRetryable);
	TestTrue(TEXT("a completion the transport dropped: Connection, not retryable (review I-3)"), Outcomes[7]->Error.Kind == EVaRestErrorKind::Connection
		&& !Outcomes[7]->Error.bRetryable && Outcomes[7]->Error.Message.Contains(TEXT("TransportDropped")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineNotInsideSendTest, "VaRest.Http.Pipeline.NeverInsideSend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineNotInsideSendTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = false; // the transport answers inside Start, synchronously
	TSharedRef<bool, ESPMode::ThreadSafe> bInsideSend = MakeShared<bool, ESPMode::ThreadSafe>(false);
	TSharedRef<int32, ESPMode::ThreadSafe> CalledInside = MakeShared<int32, ESPMode::ThreadSafe>(0);
	TSharedRef<int32, ESPMode::ThreadSafe> Called = MakeShared<int32, ESPMode::ThreadSafe>(0);
	auto Probe = [bInsideSend, CalledInside, Called](const auto&)
	{
		++*Called;
		*CalledInside += *bInsideSend ? 1 : 0;
	};

	*bInsideSend = true;
	Rig.Client.Get(TEXT("/inline")).OnComplete(Probe).Send();
	Rig.Client.Get(TEXT("/users/{id}")).OnComplete(Probe).Send(); // a Config error, known inside Send
	*bInsideSend = false;

	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	FTestRig Cached;
	Cached.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("cache"), Log, FRecordingStage::EMode::Answer), EVaRestStageSlot::Outer);
	*bInsideSend = true;
	Cached.Client.Get(TEXT("/cached")).OnComplete(Probe).Send(); // a stage answers inside Send
	*bInsideSend = false;

	TestTrue(TEXT("all three complete"), PumpUntil([Called] { return *Called == 3; }));
	TestEqual(TEXT("decision 3: no callback runs inside Send()"), *CalledInside, 0);
	TestEqual(TEXT("the answering stage kept the transport idle"), Cached.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineFromWorkerTest, "VaRest.Http.Pipeline.SendFromWorker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineFromWorkerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Outcome = MakeOutcome<FVaRestJson>();
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> SentFrom = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0); // 1 worker, 2 game thread
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bActiveOnWorker = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	FVaRestClient Client = Rig.Client;
	// Not Wait(): a waited task may be retracted and run on the waiting thread (TaskPrivate.cpp:75-100)
	UE::Tasks::Launch(TEXT("VaRest.Test.SendFromWorker"), [Client, Outcome, SentFrom, bActiveOnWorker]
	{
		const FVaRestRequestHandle Handle = Observe(Client.Post(TEXT("/from-worker")).Body(TEXT("hi")), Outcome).Send();
		*bActiveOnWorker = Handle.IsActive();
		*SentFrom = IsInGameThread() ? 2 : 1;
	});
	// Review I-6: drain no game-thread task until the worker is done, or the game thread could finish the whole request
	// between its Send() and IsActive(). The worker never needs the game thread, so this cannot deadlock.
	const double SpinUntil = FPlatformTime::Seconds() + 10.0;
	while (SentFrom->load() == 0 && FPlatformTime::Seconds() < SpinUntil)
	{
		FPlatformProcess::Yield();
	}
	TestTrue(TEXT("the worker sent"), SentFrom->load() != 0);
	TestEqual(TEXT("... from a worker thread"), SentFrom->load(), 1);
	TestTrue(TEXT("sent from a worker, the request is active before it reaches the game thread"), bActiveOnWorker->load());
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("it was marshalled to the game thread: callbacks there (spec 1)"), Outcome->bAllOnGameThread.load());
	TestEqual(TEXT("the transport was called"), Rig.Fake->NumCalls(), 1);
	TestTrue(TEXT("with the body"), Rig.Fake->ReadBody(0) == TArray<uint8>({ 'h', 'i' }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineStagesTest, "VaRest.Http.Pipeline.Stages",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineStagesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	Rig.Client.WithHeader(TEXT("X-Client"), TEXT("c")).WithHeader(TEXT("x-mode"), TEXT("client"));
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("outer"), Log), EVaRestStageSlot::Outer);
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("sign"), Log, FRecordingStage::EMode::Sign), EVaRestStageSlot::PerAttempt);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/s")).Header(TEXT("X-Mode"), TEXT("request")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	// Outer before the attempt boundary (attempt 0), PerAttempt after it (attempt 1), outcomes back in reverse
	TestEqualSensitive(TEXT("stage order (spec 4, decision 13)"), FString::Join(*Log, TEXT(",")), FString(TEXT("outer>0,sign>1,sign<200,outer<200")));
	const FVaRestHeaders& Sent = Rig.Fake->GetCall(0).Request.Headers;
	TestEqualSensitive(TEXT("a PerAttempt stage sets final headers (request signing)"), Sent.Get(TEXT("X-Signed")), FString(TEXT("attempt-1")));
	TestEqualSensitive(TEXT("client default headers are sent"), Sent.Get(TEXT("X-Client")), FString(TEXT("c")));
	TestEqualSensitive(TEXT("the request's header wins over the client's, whatever the case"), Sent.Get(TEXT("X-MODE")), FString(TEXT("request")));
	TestEqual(TEXT("... and replaces it"), Sent.GetPairs().FilterByPredicate([](const FVaRestHeaders::FPair& P) { return P.Key.Equals(TEXT("x-mode"), ESearchCase::IgnoreCase); }).Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineReissueTest, "VaRest.Http.Pipeline.Reissue",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineReissueTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("retry"), Log, FRecordingStage::EMode::Reissue), EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(Json(503, "{}"));
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/r")).Body(TArray<uint8>{ 'a', 'b', 'c' }), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestEqual(TEXT("two transport calls"), Rig.Fake->NumCalls(), 2);
	TestTrue(TEXT("attempt numbers count up across the boundary"), Rig.Fake->GetCall(0).Request.Attempt == 1 && Rig.Fake->GetCall(1).Request.Attempt == 2);
	TestTrue(TEXT("a stage that may re-issue keeps the body for the second attempt (decision 12)"),
		Rig.Fake->ReadBody(0) == TArray<uint8>({ 'a', 'b', 'c' }) && Rig.Fake->ReadBody(1) == TArray<uint8>({ 'a', 'b', 'c' }));
	TestTrue(TEXT("the second outcome is the result"), Outcome->SuccessCalls == 1 && Outcome->Response.Attempts == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineDroppedTest, "VaRest.Http.Pipeline.ClientDroppedInFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineDroppedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/slow")), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> WeakState = Rig.Client.GetStateInternal();
	Rig.Client = FVaRestClient(); // the last handle
	TestTrue(TEXT("spec 3.2: the in-flight exchange keeps the client state alive"), WeakState.IsValid());
	const int32 OffBefore = Counters().ClientStatesDestroyedOffGameThread.load();
	// The completion comes from a worker (bCompleteOnWorker) and the JSON is decoded on another: both hand their
	// references over to the game thread instead of dropping the last one where they run (review I-1)
	Rig.Fake->Complete(0, Json(200, R"({"late":true})"));
	TestTrue(TEXT("the request still completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("... and calls back"), Outcome->SuccessCalls == 1);
	PumpUntil([&WeakState] { return !WeakState.IsValid(); }, 1.0);
	TestFalse(TEXT("then the state dies"), WeakState.IsValid());
	TestEqual(TEXT("... on the game thread (spec 1; review I-1)"), Counters().ClientStatesDestroyedOffGameThread.load() - OffBefore, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineManyTest, "VaRest.Http.Pipeline.ManyConcurrent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineManyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig; // completions arrive from worker tasks, all at once
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	for (int32 i = 0; i < 100; ++i)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/m/%d"), i)), Outcomes.Last()).Send();
	}
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { for (const auto& O : Outcomes) { if (!O->IsDone()) { return false; } } return true; }, 10.0));
	PumpUntil([] { return false; }, 0.05);
	int32 Exact = 0;
	bool bGameThread = true;
	for (const auto& O : Outcomes)
	{
		Exact += (O->CompleteCalls == 1 && O->SuccessCalls == 1 && O->ErrorCalls == 0) ? 1 : 0;
		bGameThread &= O->bAllOnGameThread.load();
	}
	TestEqual(TEXT("exactly one OnSuccess and one OnComplete each"), Exact, 100);
	TestTrue(TEXT("all on the game thread"), bGameThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineConfigRaceTest, "VaRest.Http.Pipeline.ConfigEditedWhileSending",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineConfigRaceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	// One edit changes two headers together: a request that ever sees X-A != X-B saw half an edit
	auto EditBoth = [&Rig](int32 Gen)
	{
		Rig.Client.GetStateInternal()->EditConfig([Gen](VaRest::Http::FClientConfig& Edited)
		{
			Edited.DefaultHeaders.Set(TEXT("X-A"), LexToString(Gen));
			Edited.DefaultHeaders.Set(TEXT("X-B"), LexToString(Gen));
		});
	};
	EditBoth(0);
	FVaRestClient Client = Rig.Client;
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Finished = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	for (int32 t = 0; t < 4; ++t)
	{
		UE::Tasks::Launch(TEXT("VaRest.Test.Sender"), [Client, Finished]
		{
			for (int32 i = 0; i < 50; ++i)
			{
				// Spec 3.1: the snapshot is taken here, at Send() on a worker, while the game thread replaces it
				Client.Get(TEXT("/worker")).Send();
			}
			++*Finished;
		});
	}
	for (int32 Gen = 1; Gen <= 200; ++Gen)
	{
		EditBoth(Gen);
		Rig.Client.Get(FString::Printf(TEXT("/gt/%d"), Gen)).Send(); // sent right after edit Gen, on the game thread
		if (Gen % 10 == 0)
		{
			PumpUntil([] { return false; }, 0.001); // let the workers' requests enter between edits
		}
	}
	TestTrue(TEXT("the senders finish"), PumpUntil([Finished] { return Finished->load() == 4; }, 10.0));
	TestTrue(TEXT("all 400 reach the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 400 && Rig.Client.GetInFlightCount() == 0; }, 10.0));
	bool bWhole = true;
	bool bOrdered = true;
	bool bAtSend = true;
	int32 LastGameThreadGen = -1;
	TSet<int32> Seen;
	for (int32 i = 0; i < Rig.Fake->NumCalls(); ++i)
	{
		const FVaRestTransportRequest& Sent = Rig.Fake->GetCall(i).Request;
		int32 A = -1;
		int32 B = -2;
		bWhole &= LexTryParseString(A, *Sent.Headers.Get(TEXT("X-A"))) && LexTryParseString(B, *Sent.Headers.Get(TEXT("X-B"))) && A == B;
		Seen.Add(A);
		int32 SentAt = -1;
		if (Sent.Url.Contains(TEXT("/gt/")) && LexTryParseString(SentAt, *Sent.Url.RightChop(Sent.Url.Find(TEXT("/gt/")) + 4)))
		{
			bOrdered &= A >= LastGameThreadGen;
			bAtSend &= A == SentAt;
			LastGameThreadGen = A;
		}
	}
	TestTrue(TEXT("every request carries one whole configuration (X-A == X-B)"), bWhole);
	TestTrue(TEXT("spec 3.1: a request carries the configuration of its Send(), not of its later Enter"), bAtSend);
	TestTrue(TEXT("... so generations never go back in transport order"), bOrdered);
	TestTrue(TEXT("snapshots were taken at different generations"), Seen.Num() > 1);
	// The data-race half (a worker reading while the game thread swaps) is a crash/TSAN smoke test: it fails only by crashing
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineConfigErrorTest, "VaRest.Http.Pipeline.ConfigErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineConfigErrorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig NoBase(TEXTVIEW(""));
	auto Relative = MakeOutcome<FVaRestJson>();
	Observe(NoBase.Client.Get(TEXT("users/7")), Relative).Send();
	FTestRig Rig;
	auto Missing = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/users/{id}")), Missing).Send();
	TestTrue(TEXT("both complete"), PumpUntil([&] { return Relative->IsDone() && Missing->IsDone(); }));
	TestTrue(TEXT("a relative URL without a base is Kind Config"), Relative->Error.Kind == EVaRestErrorKind::Config && Relative->Error.Message.Contains(TEXT("absolute")));
	TestTrue(TEXT("a missing path parameter is Kind Config, named"), Missing->Error.Kind == EVaRestErrorKind::Config && Missing->Error.Message.Contains(TEXT("{id}")));
	TestEqual(TEXT("the transport never saw them"), NoBase.Fake->NumCalls() + Rig.Fake->NumCalls(), 0);
	TestFalse(TEXT("Config errors are not retryable"), Missing->Error.bRetryable);

	// Review M12: only the path is a template; a brace in the query is the caller's text
	auto Search = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/items/{id}?fields={a,b}")).Path(TEXT("id"), 7), Search).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Search));
	TestTrue(TEXT("a brace in the query is no path parameter"), Search->SuccessCalls == 1
		&& Rig.Fake->GetCall(0).Request.Url.Equals(TEXT("https://api.test/items/7?fields={a,b}"), ESearchCase::CaseSensitive));

	// Decision 33 (review M22): a base URL may carry a query, not a fragment
	AddExpectedErrorPlain(TEXT("has a fragment"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Fragment(TEXTVIEW("https://api.test/v1#x"));
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Fragment.Client.Get(TEXT("/a")), Refused).Send();
	TestTrue(TEXT("completes"), Fragment.Wait(Refused));
	TestTrue(TEXT("a base URL with a fragment: Kind Config, never sent"), Refused->Error.Kind == EVaRestErrorKind::Config && Fragment.Fake->NumCalls() == 0);
	FTestRig Keyed(TEXTVIEW("https://api.test/v1?key=k"));
	auto WithKey = MakeOutcome<FVaRestJson>();
	Observe(Keyed.Client.Get(TEXT("/pets?limit=2")).Query(TEXT("page"), 3), WithKey).Send();
	TestTrue(TEXT("completes"), Keyed.Wait(WithKey));
	TestEqualSensitive(TEXT("the base query first, then the request's own pairs"), Keyed.Fake->GetCall(0).Request.Url,
		FString(TEXT("https://api.test/v1/pets?key=k&limit=2&page=3")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineDroppedCompletionTest, "VaRest.Http.Pipeline.DroppedCompletion",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineDroppedCompletionTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Review I-3: the engine destroys completions uncalled at HTTP module shutdown and with NullHttp
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/lost")), Outcome).Send();
	TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; }));
	Rig.Fake->Drop(0);
	TestTrue(TEXT("the request still finishes"), Rig.Wait(Outcome));
	TestTrue(TEXT("... with one OnError and one OnComplete"), Outcome->ErrorCalls == 1 && Outcome->CompleteCalls == 1 && Outcome->SuccessCalls == 0);
	TestTrue(TEXT("... as a dropped transport call"), Outcome->Error.Kind == EVaRestErrorKind::Connection && Outcome->Error.Message.Contains(TEXT("TransportDropped")));
	TestEqual(TEXT("nothing left in flight"), Rig.Client.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpPipelineOutcomeTwiceTest, "VaRest.Http.Pipeline.OutcomeTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpPipelineOutcomeTwiceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::PipelineTestPrivate;
	// Review C-3: a stage that reports its outcome twice -- the second report is ignored, nothing crashes
	FTestRig Rig;
	FLog Log = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	Rig.Client.Use(MakeShared<FRecordingStage, ESPMode::ThreadSafe>(TEXT("twice"), Log, FRecordingStage::EMode::Twice), EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/twice")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	PumpUntil([] { return false; }, 0.05);
	TestTrue(TEXT("one OnSuccess, one OnComplete"), Outcome->SuccessCalls == 1 && Outcome->CompleteCalls == 1 && Outcome->ErrorCalls == 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails on `VaRestClient.h` (no such file).

- [ ] **Step 3: The typed half** — `Source/VaRestHttp/Public/VaRestResultHandler.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClock.h"
#include "VaRestHttpTypes.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"

#include <type_traits>

namespace VaRest::Http
{
	/** A response on its way to the callbacks: built on the game thread, decoded on a background task, delivered on the game thread (spec 1) */
	struct FFinish
	{
		FVaRestResponse Response;
		/** Kind None: a success so far */
		FVaRestError Error;
		/** The response body with its byte of slack; ParseOwned takes it (spec 2.1) */
		TArray<uint8> Body;
		/** The body went to TempFile (SaveTo) */
		bool bBodyInFile = false;
		FString TempFile;
		FString SaveTo;
		bool bKeepBody = false;
		FVaRestJsonConvertOptions DecodeOptions;
		/** MaxInputBytes = the request's or the client's MaxJsonBytes (decision 20) */
		FVaRestJsonParseOptions ParseOptions;
		/** The trace mask was set at the finish: decode times are recorded (IN-5; IN-10: clock reads follow the mask) */
		bool bTimeDecode = false;
		double DecodeStartAt = -1.0;
		double DecodeEndAt = -1.0;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
	};

	/** The typed half of a request: one result type's decode and callbacks (TResultHandler<T>) */
	class IResultHandler
	{
	public:
		virtual ~IResultHandler() = default;

		/** For traces (Inspector IN-5 ResultType) */
		virtual FName GetResultType() const = 0;
		/** A 2xx body needs decoding off the game thread: JSON, a struct, text */
		virtual bool DecodesBody() const = 0;
		/** The body is parsed as JSON (FVaRestJson, a struct): the transport applies MaxJsonBytes to it (reviews M-6, M13) */
		virtual bool DecodesJson() const = 0;
		/** A 2xx response into the typed value; a failure sets Finish.Error (Kind Decode). A background task when DecodesBody(), else the game thread. */
		virtual void Decode(FFinish& Finish) = 0;
		/** OnSuccess or OnError, then OnComplete. The game thread, or a background task with OnHttpThread(). */
		virtual void Deliver(FFinish& Finish) = 0;
		virtual bool WantsProgress() const = 0;
		/** Game thread */
		virtual void Progress(const FVaRestProgress& Snapshot) = 0;
	};

	/**
	 * The body as JSON. 204 or an empty body is an invalid handle and a success (spec 3.2: "204/empty -> T default"). A
	 * Content-Type that is not JSON is a Decode error (decision 6: a missing one is parsed). ParseOwned takes the body
	 * in situ, zero-copy thanks to its slack (spec 2.1); KeepBody() copies it first. Background thread.
	 */
	VARESTHTTP_API bool DecodeJson(FFinish& Finish, FVaRestJson& Out);

	/**
	 * DecodeJson, then FVaRestJsonConvert::JsonToStruct into Data, an initialised instance of Struct. Warnings land in
	 * Response.DecodeWarnings; a failure is a Decode error with the first issue's JSON path. Background thread. Struct
	 * must stay alive and unchanged until this returns: a native struct always does; a caller passing a Blueprint
	 * UUserDefinedStruct (M5's FInstancedStruct carrier) must hold it across garbage collection for the whole decode
	 * (TStrongObjectPtr or an FGCObject) and decode into memory its task owns (decision 22).
	 */
	VARESTHTTP_API bool DecodeStruct(FFinish& Finish, const UScriptStruct* Struct, void* Data);

	/** UTF-8 to text, an ill-formed sequence as U+FFFD (spec 2.2); the raw bytes released per spec 3.2 */
	VARESTHTTP_API void DecodeText(FFinish& Finish, FString& Out);

	/** Spec 3.2: raw bytes are released after a successful decode, unless KeepBody() asked to keep them */
	VARESTHTTP_API void ReleaseBody(FFinish& Finish);

	template <typename T>
	inline constexpr bool bIsSupportedResult = std::is_same_v<T, FVaRestJson> || std::is_same_v<T, FString> || std::is_same_v<T, TArray<uint8>>
		|| std::is_same_v<T, FVaRestResponse> || VaRest::Json::bHasScriptStruct<T>;

	template <typename T>
	class TResultHandler final : public IResultHandler
	{
		static_assert(bIsSupportedResult<T>, "TVaRestRequest<T>: T is FVaRestJson (the default), void, FString, TArray<uint8>, FVaRestResponse or a USTRUCT (spec 3.2)");
		static constexpr bool bIsStruct = !std::is_same_v<T, FVaRestJson> && VaRest::Json::bHasScriptStruct<T>;

	public:
		TResultHandler()
		{
			if constexpr (bIsStruct)
			{
				Struct = VaRest::Json::GetScriptStruct<T>();
			}
		}

		TUniqueFunction<void(const T&, const FVaRestResponse&)> OnSuccess;
		TUniqueFunction<void(const FVaRestError&)> OnError;
		TUniqueFunction<void(const TVaRestResult<T>&)> OnComplete;
		TUniqueFunction<void(const FVaRestProgress&)> OnProgress;

		virtual FName GetResultType() const override
		{
			if constexpr (std::is_same_v<T, FVaRestJson>)
			{
				return FName(TEXT("Json"));
			}
			else if constexpr (std::is_same_v<T, FString>)
			{
				return FName(TEXT("String"));
			}
			else if constexpr (std::is_same_v<T, TArray<uint8>>)
			{
				return FName(TEXT("Bytes"));
			}
			else if constexpr (std::is_same_v<T, FVaRestResponse>)
			{
				return FName(TEXT("Response"));
			}
			else
			{
				return Struct->GetFName();
			}
		}

		virtual bool DecodesBody() const override
		{
			return !std::is_same_v<T, TArray<uint8>> && !std::is_same_v<T, FVaRestResponse>;
		}

		virtual bool DecodesJson() const override
		{
			return DecodesBody() && !std::is_same_v<T, FString>;
		}

		virtual void Decode(FFinish& Finish) override
		{
			if constexpr (std::is_same_v<T, FVaRestJson>)
			{
				FVaRestJson Json;
				if (DecodeJson(Finish, Json))
				{
					Value.Emplace(MoveTemp(Json));
				}
			}
			else if constexpr (std::is_same_v<T, FString>)
			{
				FString Text;
				DecodeText(Finish, Text);
				Value.Emplace(MoveTemp(Text));
			}
			else if constexpr (std::is_same_v<T, TArray<uint8>>)
			{
				Value.Emplace(MoveTemp(Finish.Body));
			}
			else if constexpr (std::is_same_v<T, FVaRestResponse>)
			{
				FVaRestResponse Raw = Finish.Response;
				Raw.Body = MoveTemp(Finish.Body);
				Value.Emplace(MoveTemp(Raw));
			}
			else
			{
				// Built the way reflection builds it, as FVaRestJsonConvert::FromJson does, so a field the JSON lacks keeps its default
				alignas(T) uint8 Storage[sizeof(T)];
				Struct->InitializeStruct(Storage);
				if (DecodeStruct(Finish, Struct, Storage))
				{
					Value.Emplace(MoveTemp(*reinterpret_cast<T*>(Storage)));
				}
				Struct->DestroyStruct(Storage);
			}
		}

		virtual void Deliver(FFinish& Finish) override
		{
			if (Finish.Error.IsError())
			{
				Value.Reset();
				if (OnError)
				{
					OnError(Finish.Error);
				}
			}
			else if (OnSuccess && Value.IsSet())
			{
				OnSuccess(Value.GetValue(), Finish.Response);
			}
			if (OnComplete)
			{
				TVaRestResult<T> Result;
				Result.Value = MoveTemp(Value);
				Result.Error = Finish.Error;
				Result.Response = MoveTemp(Finish.Response);
				OnComplete(Result);
			}
		}

		virtual bool WantsProgress() const override
		{
			return static_cast<bool>(OnProgress);
		}

		virtual void Progress(const FVaRestProgress& Snapshot) override
		{
			if (OnProgress)
			{
				OnProgress(Snapshot);
			}
		}

	private:
		UScriptStruct* Struct = nullptr;
		TOptional<T> Value;
	};

	template <>
	class TResultHandler<void> final : public IResultHandler
	{
	public:
		TUniqueFunction<void(const FVaRestResponse&)> OnSuccess;
		TUniqueFunction<void(const FVaRestError&)> OnError;
		TUniqueFunction<void(const TVaRestResult<void>&)> OnComplete;
		TUniqueFunction<void(const FVaRestProgress&)> OnProgress;

		virtual FName GetResultType() const override { return NAME_None; }
		virtual bool DecodesBody() const override { return false; }
		virtual bool DecodesJson() const override { return false; }
		virtual void Decode(FFinish& Finish) override { ReleaseBody(Finish); }

		virtual void Deliver(FFinish& Finish) override
		{
			if (Finish.Error.IsError())
			{
				if (OnError)
				{
					OnError(Finish.Error);
				}
			}
			else if (OnSuccess)
			{
				OnSuccess(Finish.Response);
			}
			if (OnComplete)
			{
				TVaRestResult<void> Result;
				Result.Error = Finish.Error;
				Result.Response = MoveTemp(Finish.Response);
				OnComplete(Result);
			}
		}

		virtual bool WantsProgress() const override { return static_cast<bool>(OnProgress); }

		virtual void Progress(const FVaRestProgress& Snapshot) override
		{
			if (OnProgress)
			{
				OnProgress(Snapshot);
			}
		}
	};
}
```

- [ ] **Step 4: Stages and the exchange** — `Source/VaRestHttp/Public/VaRestStage.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestBody.h"
#include "VaRestClock.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonConvert.h"
#include "VaRestResultHandler.h"
#include "VaRestTrace.h"
#include "VaRestTransport.h"

#include <atomic>

class FVaRestExchange;
class IVaRestStage;

namespace VaRest::Http
{
	class FClientState;
	struct FClientConfig;
}

/** What one attempt produced, travelling back up the chain (spec 4) */
struct VARESTHTTP_API FVaRestAttemptOutcome
{
	/** 0 = no response */
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** With one byte of slack for ParseOwned (spec 2.1); empty when the body went to a file */
	TArray<uint8> Body;
	bool bBodyInFile = false;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	int64 BytesUp = 0;
	int64 BytesDown = 0;

	bool IsTransportFailure() const { return Failure != EVaRestTransportFailure::None; }

	/** What Proceed reports for an exchange that is finished or being canceled */
	static FVaRestAttemptOutcome Canceled();
};

using FVaRestOutcomeCallback = TUniqueFunction<void(FVaRestAttemptOutcome&&)>;
using FVaRestStageRef = TSharedRef<IVaRestStage, ESPMode::ThreadSafe>;
using FVaRestStageChain = TArray<FVaRestStageRef>;
using FVaRestExchangeRef = TSharedRef<FVaRestExchange, ESPMode::ThreadSafe>;

/** The rest of the chain after one stage: a cheap value, so a stage may keep it and proceed later (after a timer, a refresh) */
class VARESTHTTP_API FVaRestStageNext
{
public:
	FVaRestStageNext(TSharedRef<const FVaRestStageChain, ESPMode::ThreadSafe> InChain, int32 InIndex)
		: Chain(MoveTemp(InChain))
		, Index(InIndex)
	{
	}

	/** Game thread. Runs the remaining stages for Exchange; OnOutcome receives what they produce. A finished or canceled exchange reports Canceled at once. */
	void Proceed(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& OnOutcome) const;

private:
	TSharedRef<const FVaRestStageChain, ESPMode::ThreadSafe> Chain;
	int32 Index = 0;
};

/** Spec 4: asynchronous middleware. Add one with FVaRestClient::Use(Stage, EVaRestStageSlot). */
class IVaRestStage
{
public:
	virtual ~IVaRestStage() = default;

	virtual FName GetStageName() const = 0;

	/** True when Process may call Next.Proceed more than once (retry, replay): the transport then copies the body instead of moving it (decision 12) */
	virtual bool MayReissue() const { return false; }

	/**
	 * Game thread. Pass the exchange on with Next.Proceed (now or later; each call is another trip through the stages after
	 * this one), or answer it yourself through OnOutcome (a cache hit). Call OnOutcome exactly once. A PerAttempt stage may
	 * edit Exchange->GetAttemptHeaders() before proceeding (request signing).
	 */
	virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) = 0;
};

enum class EVaRestExchangePhase : uint8
{
	/** Sent, not yet on the game thread */
	Created,
	/** In the stage chain */
	InChain,
	/** A background task holds the result handler */
	Decoding,
	Done
};

/**
 * One logical request moving through a client's stages (spec 4). Game thread, except where marked. Stages use the
 * accessors; the fields below them are VaRestHttp's own and not a stable API.
 */
class VARESTHTTP_API FVaRestExchange : public TSharedFromThis<FVaRestExchange, ESPMode::ThreadSafe>
{
public:
	FVaRestExchange() = default;
	/**
	 * Out of line (VaRestPipeline.cpp). Off the game thread it sends its State and Config there to die (review I-1
	 * backstop): with them go the client state, custom stages, the transport and the clock, whose destructors may clear
	 * game-thread timers.
	 */
	~FVaRestExchange();
	FVaRestExchange(const FVaRestExchange&) = delete;
	FVaRestExchange& operator=(const FVaRestExchange&) = delete;

	// ---- for stages
	uint64 GetRequestId() const { return RequestId; }
	uint64 GetClientId() const { return ClientId; }
	const FString& GetVerb() const { return Verb; }
	/** Base URL + expanded path + query */
	const FString& GetUrl() const { return Url; }
	/** The path before {param} substitution, without the query (Inspector IN-1) */
	const FString& GetPathTemplate() const { return PathTemplate; }
	FName GetTag() const { return Tag; }
	EVaRestPriority GetPriority() const { return Priority; }
	bool IsIdempotent() const { return bIdempotent; }
	/** Client defaults, then the request's own */
	const FVaRestHeaders& GetRequestHeaders() const { return RequestHeaders; }
	/** A fresh copy of GetRequestHeaders() for each attempt; PerAttempt stages may edit it */
	FVaRestHeaders& GetAttemptHeaders() { return AttemptHeaders; }
	const FVaRestHeaders& GetAttemptHeaders() const { return AttemptHeaders; }
	const FVaRestRequestBody& GetBody() const { return Body; }
	/** 0 before the attempt boundary, then 1, 2... */
	int32 GetAttempt() const { return Attempt; }
	/** Any thread */
	bool IsCancelRequested() const { return bCancelRequested.load(); }
	/** Any thread: the callbacks have been delivered (or dropped) */
	bool IsDone() const { return bDone.load(); }

	// ---- VaRestHttp internals. Set by Send on the sending thread, read on the game thread after the hop:
	uint64 RequestId = 0;
	uint64 ClientId = 0;
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State;
	FString Verb;
	FString RawPath;
	TArray<TPair<FString, FString>> PathParams;
	TArray<TPair<FString, FString>> Query;
	FVaRestHeaders OwnHeaders;
	FVaRestRequestBody Body;
	FString BuildError;
	FName Tag;
	EVaRestPriority Priority = EVaRestPriority::Normal;
	bool bIdempotent = false;
	bool bKeepBody = false;
	bool bOnHttpThread = false;
	FString SaveTo;
	TOptional<float> TimeoutOverride;
	TOptional<float> ActivityTimeoutOverride;
	TOptional<int64> MaxJsonBytesOverride;
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
	bool bStrictDecode = false;
	TWeakObjectPtr<const UObject> Owner;
	bool bHasOwner = false;
	const char* SourceFile = "";
	uint32 SourceLine = 0;
	FVaRestTraceInitiator Initiator;
	TUniquePtr<VaRest::Http::IResultHandler> Handler;

	// Resolved on entry (game thread):
	TSharedPtr<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> Config;
	FString Url;
	FString PathTemplate;
	FVaRestHeaders RequestHeaders;
	float Timeout = 0.f;
	float ActivityTimeout = 0.f;
	int64 MaxJsonBytes = 0;
	FVaRestJsonConvertOptions DecodeOptions;
	EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
	double StartedAt = 0.0;
	FDateTime StartUtc;

	// The current attempt (game thread):
	int32 Attempt = 0;
	double AttemptStartedAt = 0.0;
	double LimiterWait = 0.0;
	FVaRestHeaders AttemptHeaders;
	FString TempFile;
	TSharedPtr<IVaRestTransportCall, ESPMode::ThreadSafe> TransportCall;
	TSharedPtr<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
	FVaRestTimerHandle ProgressTimer;
	FVaRestProgress LastProgress;
	bool bInLimiterQueue = false;

	// Life (game thread, except the atomics):
	EVaRestExchangePhase Phase = EVaRestExchangePhase::Created;
	EVaRestCancelReason CancelReason = EVaRestCancelReason::None;
	/** Complete ran: nothing else is delivered */
	bool bFinished = false;
	/** Inside Send() or Cancel() on this exchange: Complete defers the callbacks (decision 3) */
	int32 ApiDepth = 0;
	std::atomic<bool> bCancelRequested{ false };
	std::atomic<bool> bDone{ false };
};
```

- [ ] **Step 5: The public client and builder** — `Source/VaRestHttp/Public/VaRestClient.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/SourceLocation.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonConvert.h"
#include "VaRestResultHandler.h"
#include "VaRestUrl.h"
#include "VaRestStage.h"

#include <type_traits>

class FVaRestRequestHandle;

namespace VaRest::Http
{
	/** What a TVaRestRequest collects; Send moves it into an exchange */
	struct VARESTHTTP_API FRequestSpec
	{
		FRequestSpec(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState, FStringView InVerb, FStringView InPath);
		~FRequestSpec();
		FRequestSpec(const FRequestSpec&) = delete;
		FRequestSpec& operator=(const FRequestSpec&) = delete;

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
		bool bStructBodySet = false;
		TUniquePtr<IResultHandler> Handler;
		bool bSent = false;
	};

	VARESTHTTP_API FVaRestRequestHandle SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
}

/** Spec 3.2: weak -- it does not keep the request alive */
class VARESTHTTP_API FVaRestRequestHandle
{
public:
	FVaRestRequestHandle() = default;
	explicit FVaRestRequestHandle(const FVaRestExchangeRef& InExchange);

	/** The request id, also in log lines (#57) and traces (IN-10, IN-16) */
	uint64 GetId() const { return Id; }
	/** Any thread: sent and not finished */
	bool IsActive() const;

private:
	TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Exchange;
	uint64 Id = 0;
};

/**
 * Spec 3.2: one request, built fluently. Every method returns the builder by value (a moved handle, cheap), so the
 * class-level [[nodiscard]] makes a forgotten Send() a compile warning; the methods are && so a builder is used once.
 * Any thread.
 */
template <typename T>
class [[nodiscard]] TVaRestRequest
{
public:
	explicit TVaRestRequest(TUniquePtr<VaRest::Http::FRequestSpec>&& InSpec)
		: Spec(MoveTemp(InSpec))
	{
	}

	TVaRestRequest(TVaRestRequest&&) = default;
	TVaRestRequest& operator=(TVaRestRequest&&) = default;
	TVaRestRequest(const TVaRestRequest&) = delete;
	TVaRestRequest& operator=(const TVaRestRequest&) = delete;

	/** {Name} in the path becomes Value, percent-encoded as one segment (spec 3.2); the last value for a name wins */
	TVaRestRequest Path(FStringView Name, FStringView Value) &&
	{
		Spec->PathParams.Emplace(FString(Name), FString(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
		requires std::is_integral_v<ValueType>
	TVaRestRequest Path(FStringView Name, ValueType Value) &&
	{
		return MoveTemp(*this).Path(Name, FStringView(LexToString(Value)));
	}

	/** Appends Name=Value, percent-encoded; a repeated name is appended again (OpenAPI N9) */
	TVaRestRequest Query(FStringView Name, FStringView Value) &&
	{
		Spec->Query.Emplace(FString(Name), FString(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
		requires std::is_integral_v<ValueType>
	TVaRestRequest Query(FStringView Name, ValueType Value) &&
	{
		return MoveTemp(*this).Query(Name, FStringView(LexToString(Value)));
	}

	/** An array value, written as Style says (OpenAPI N9, decision 34): Repeat (default) tag=a&tag=b; Csv, Ssv, Pipes one joined pair */
	TVaRestRequest Query(FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style = EVaRestQueryStyle::Repeat) &&
	{
		VaRest::Http::AddQueryValues(Spec->Query, Name, Values, Style);
		return MoveTemp(*this);
	}

	/** Sets a header (replacing one of that name, whatever its case); it wins over the client's and over a body's Content-Type */
	TVaRestRequest Header(FStringView Name, FStringView Value) &&
	{
		Spec->Headers.Set(Name, Value);
		return MoveTemp(*this);
	}

	/** application/json; an invalid (missing) handle sends no body, the way JSON.stringify(undefined) is nothing (spec 2.2) */
	TVaRestRequest Body(const FVaRestJson& Json) &&
	{
		if (Json.IsValid())
		{
			TArray<uint8> Bytes;
			const TValueOrError<void, FVaRestJsonError> Written = Json.TryToUtf8(Bytes);
			if (Written.HasError())
			{
				Spec->Fail(FString::Printf(TEXT("the JSON body cannot be written: %s"), *Written.GetError().Message));
			}
			Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Json, TEXT("application/json"));
		}
		return MoveTemp(*this);
	}

	/** text/plain; charset=utf-8 */
	TVaRestRequest Body(FStringView Text) &&
	{
		FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		Spec->Body = FVaRestRequestBody::FromBytes(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), EVaRestBodyKind::Text, TEXT("text/plain; charset=utf-8"));
		return MoveTemp(*this);
	}

	/** application/octet-stream, moved: no copy until the transport (spec 2.4) */
	TVaRestRequest Body(TArray<uint8>&& Bytes) &&
	{
		Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Binary, TEXT("application/octet-stream"));
		return MoveTemp(*this);
	}

	/** application/octet-stream, copied */
	TVaRestRequest Body(TConstArrayView<uint8> Bytes) &&
	{
		return MoveTemp(*this).Body(TArray<uint8>(Bytes.GetData(), Bytes.Num()));
	}

	TVaRestRequest Tag(FName InTag) &&
	{
		Spec->Tag = InTag;
		return MoveTemp(*this);
	}

	/** The limiter's queue order (spec 4.6) */
	TVaRestRequest Priority(EVaRestPriority InPriority) &&
	{
		Spec->Priority = InPriority;
		return MoveTemp(*this);
	}

	/** (const T&) or (const T&, const FVaRestResponse&); for T = void: () or (const FVaRestResponse&) */
	template <typename FuncType>
	TVaRestRequest OnSuccess(FuncType&& Func) &&
	{
		if constexpr (std::is_void_v<T>)
		{
			if constexpr (std::is_invocable_v<FuncType&, const FVaRestResponse&>)
			{
				Handler().OnSuccess = Forward<FuncType>(Func);
			}
			else
			{
				static_assert(std::is_invocable_v<FuncType&>, "OnSuccess of a void request takes () or (const FVaRestResponse&)");
				Handler().OnSuccess = [Callback = Forward<FuncType>(Func)](const FVaRestResponse&) mutable { Callback(); };
			}
		}
		else
		{
			if constexpr (std::is_invocable_v<FuncType&, const T&, const FVaRestResponse&>)
			{
				Handler().OnSuccess = Forward<FuncType>(Func);
			}
			else
			{
				static_assert(std::is_invocable_v<FuncType&, const T&>, "OnSuccess takes (const T&) or (const T&, const FVaRestResponse&)");
				Handler().OnSuccess = [Callback = Forward<FuncType>(Func)](const T& Result, const FVaRestResponse&) mutable { Callback(Result); };
			}
		}
		return MoveTemp(*this);
	}

	/** (const FVaRestError&) */
	template <typename FuncType>
	TVaRestRequest OnError(FuncType&& Func) &&
	{
		Handler().OnError = Forward<FuncType>(Func);
		return MoveTemp(*this);
	}

	/** (const TVaRestResult<T>&), after OnSuccess or OnError */
	template <typename FuncType>
	TVaRestRequest OnComplete(FuncType&& Func) &&
	{
		Handler().OnComplete = Forward<FuncType>(Func);
		return MoveTemp(*this);
	}

	/**
	 * Any thread. The request enters its client's pipeline on the game thread. Callbacks never run inside Send()
	 * (decision 3). Where is the C++ call site for traces (Inspector IN-15); leave it defaulted.
	 */
	FVaRestRequestHandle Send(UE::FSourceLocation Where = UE::FSourceLocation::Current()) &&
	{
		return VaRest::Http::SendRequest(MoveTemp(Spec), Where.GetFileName(), Where.GetLine());
	}

private:
	VaRest::Http::TResultHandler<T>& Handler()
	{
		return static_cast<VaRest::Http::TResultHandler<T>&>(*Spec->Handler);
	}

	TUniquePtr<VaRest::Http::FRequestSpec> Spec;
};

/**
 * Spec 3.1: a cheap handle to one client's state -- base URL, default headers, policies, stages. Copies share it.
 * Configure it on the game thread; requests already sent keep the configuration they started with. Send from any thread.
 */
class VARESTHTTP_API FVaRestClient
{
public:
	/** An empty handle; Make() a real one */
	FVaRestClient() = default;

	/** Game thread. Defaults from UVaRestHttpSettings; the FHttpModule transport; the real clock. */
	static FVaRestClient Make(FStringView BaseUrl);

	bool IsValid() const { return State.IsValid(); }
	uint64 GetId() const;
	FString GetBaseUrl() const;
	/** Game thread */
	int32 GetInFlightCount() const;

	// ---- configuration (game thread)
	FVaRestClient& WithName(FStringView DisplayName);
	FVaRestClient& WithBaseUrl(FStringView BaseUrl);
	FVaRestClient& WithHeader(FStringView Name, FStringView Value);
	FVaRestClient& WithoutHeader(FStringView Name);
	/** How struct bodies name their fields (spec 2.4); decode accepts every spelling regardless */
	FVaRestClient& WithNaming(EVaRestJsonNaming Naming);
	FVaRestClient& WithTransport(TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Transport);
	FVaRestClient& WithClock(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock);
	/** A custom stage (spec 4): Outer sees each logical request once, PerAttempt every attempt with its final headers */
	FVaRestClient& Use(FVaRestStageRef Stage, EVaRestStageSlot Slot);

	// ---- requests (any thread)
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Get(FStringView Path) const { return Request<T>(TEXTVIEW("GET"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Post(FStringView Path) const { return Request<T>(TEXTVIEW("POST"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Put(FStringView Path) const { return Request<T>(TEXTVIEW("PUT"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Patch(FStringView Path) const { return Request<T>(TEXTVIEW("PATCH"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Delete(FStringView Path) const { return Request<T>(TEXTVIEW("DELETE"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Head(FStringView Path) const { return Request<T>(TEXTVIEW("HEAD"), Path); }

	/** Any verb; T defaults to FVaRestJson (spec 3.2). An empty client gives a builder whose Send() logs an error and sends nothing. */
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const
	{
		TUniquePtr<VaRest::Http::FRequestSpec> NewSpec = MakeUnique<VaRest::Http::FRequestSpec>(State, Verb, Path);
		NewSpec->Handler = MakeUnique<VaRest::Http::TResultHandler<T>>();
		return TVaRestRequest<T>(MoveTemp(NewSpec));
	}

	/** VaRestHttp and its tests; not a stable API */
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> GetStateInternal() const { return State; }

private:
	explicit FVaRestClient(TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State;
};
```

- [ ] **Step 6: Client state** — `Source/VaRestHttp/Private/VaRestClientState.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/ScopeRWLock.h"
#include "VaRestClient.h"
#include "VaRestRedactor.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** One immutable configuration snapshot; FClientState swaps a new one in on every edit (spec 3.1) */
	struct FClientConfig
	{
		FString BaseUrl;
		FString DisplayName;
		FVaRestHeaders DefaultHeaders;
		EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
		EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
		int32 MaxConcurrent = 16;
		float Timeout = 0.f;
		float ActivityTimeout = 0.f;
		int64 MaxJsonBytes = 32 * 1024 * 1024;
		/** The project's (UVaRestHttpSettings::RedactKeys) and this client's additions */
		TArray<FString> RedactKeys;
		TArray<TPair<FVaRestStageRef, EVaRestStageSlot>> CustomStages;
		TSharedPtr<IVaRestTransport, ESPMode::ThreadSafe> Transport;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;

		// Derived by FClientState::Publish
		TSharedPtr<const FVaRestStageChain, ESPMode::ThreadSafe> Chain;
		TSharedPtr<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor;
		/** A custom stage may re-issue: the transport copies bodies (decision 12) */
		bool bMayReissue = false;
	};

	class FClientState : public TSharedFromThis<FClientState, ESPMode::ThreadSafe>
	{
	public:
		FClientState(uint64 InClientId, FClientConfig&& InConfig);
		~FClientState();

		/** Builds the derived configuration and registers the client. Game thread. */
		static TSharedRef<FClientState, ESPMode::ThreadSafe> Create(FClientConfig&& InConfig);

		uint64 GetClientId() const { return ClientId; }
		/** Any thread */
		TSharedRef<const FClientConfig, ESPMode::ThreadSafe> GetConfig() const;
		/** Game thread: copy, edit, rebuild the chain and the redactor, swap */
		void EditConfig(TFunctionRef<void(FClientConfig&)> Edit);

		/** Game thread: a sent exchange enters the pipeline */
		void Enter(const FVaRestExchangeRef& Exchange);
		/** Game thread: the transport's result, brought over from the thread that completed it */
		void OnAttemptDone(const FVaRestExchangeRef& Exchange, FVaRestTransportResult&& Result, FVaRestOutcomeCallback&& OnOutcome);
		/** Game thread */
		int32 NumInFlight() const { return InFlight.Num(); }

		/** Every live client (game thread): the trace snapshot, engine shutdown */
		static void ForEachLive(TFunctionRef<void(FClientState&)> Visit);

	private:
		bool Resolve(FVaRestExchange& Exchange, FString& OutError) const;
		void OnChainDone(const FVaRestExchangeRef& Exchange, FVaRestAttemptOutcome&& Outcome);
		void Complete(const FVaRestExchangeRef& Exchange, FFinish&& Finish);
		FFinish MakeFinish(const FVaRestExchange& Exchange) const;
		void Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next);

		const uint64 ClientId;
		mutable FRWLock ConfigLock;
		TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Config;
		/** Spec 3.2: in-flight exchanges are owned here and keep this state alive (each holds its State) */
		TMap<uint64, FVaRestExchangeRef> InFlight;
	};

	/** The chain's built-in stages (VaRestPipeline.cpp) */
	FVaRestStageRef MakeAttemptBoundaryStage();
	FVaRestStageRef MakeTransportStage();
}
```

`Source/VaRestHttp/Private/VaRestClientState.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"

#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::ClientStatePrivate
{
	/** Locked: a client state can die on any thread (a game's own worker may drop the last FVaRestClient) */
	struct FLiveRegistry
	{
		FCriticalSection Lock;
		TArray<TWeakPtr<FClientState, ESPMode::ThreadSafe>> States;
	};

	/**
	 * Leaked on purpose (review I-4): a global FVaRestClient in a game module is destroyed during static destruction,
	 * possibly after this file's statics; its ~FClientState must not lock a destroyed mutex or edit a freed array.
	 */
	FLiveRegistry& Live()
	{
		static FLiveRegistry* Registry = new FLiveRegistry();
		return *Registry;
	}
}

VaRest::Http::FClientState::FClientState(uint64 InClientId, FClientConfig&& InConfig)
	: ClientId(InClientId)
	, Config(MakeShared<FClientConfig, ESPMode::ThreadSafe>(MoveTemp(InConfig)))
{
}

VaRest::Http::FClientState::~FClientState()
{
	if (!IsInGameThread())
	{
#if !UE_BUILD_SHIPPING
		++Testing::Counters().ClientStatesDestroyedOffGameThread;
#endif
		// Review I-1 backstop: the configuration (custom stages, transport, clock) dies on the game thread
		UE::Tasks::Launch(TEXT("VaRest.ReleaseConfig"), [Doomed = Config] {}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
	FScopeLock Lock(&Registry.Lock);
	Registry.States.RemoveAll([this](const TWeakPtr<FClientState, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid() || Weak.HasSameObject(this); });
}

TSharedRef<VaRest::Http::FClientState, ESPMode::ThreadSafe> VaRest::Http::FClientState::Create(FClientConfig&& InConfig)
{
	check(IsInGameThread());
	TSharedRef<FClientState, ESPMode::ThreadSafe> Created = MakeShared<FClientState, ESPMode::ThreadSafe>(FVaRestTrace::NewClientId(), MoveTemp(InConfig));
	Created->EditConfig([](FClientConfig&) {});
	ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
	FScopeLock Lock(&Registry.Lock);
	Registry.States.Add(Created);
	return Created;
}

TSharedRef<const VaRest::Http::FClientConfig, ESPMode::ThreadSafe> VaRest::Http::FClientState::GetConfig() const
{
	FReadScopeLock ReadLock(ConfigLock);
	return Config;
}

void VaRest::Http::FClientState::EditConfig(TFunctionRef<void(FClientConfig&)> Edit)
{
	check(IsInGameThread());
	TSharedRef<FClientConfig, ESPMode::ThreadSafe> Next = MakeShared<FClientConfig, ESPMode::ThreadSafe>(*GetConfig());
	Edit(*Next);
	Publish(Next);
}

void VaRest::Http::FClientState::Publish(const TSharedRef<FClientConfig, ESPMode::ThreadSafe>& Next)
{
	// Spec 4's chain. Custom Outer stages first: they see every logical request once (decision 13); M4's built-in Outer
	// stages (offline queue, cache, retry) go between them and the attempt boundary. Custom PerAttempt stages see the
	// final headers; the built-ins next to the transport (log stage, Task 14; limiter, Task 12) come last.
	TSharedRef<FVaRestStageChain, ESPMode::ThreadSafe> NewChain = MakeShared<FVaRestStageChain, ESPMode::ThreadSafe>();
	bool bReissue = false;
	for (const TPair<FVaRestStageRef, EVaRestStageSlot>& Entry : Next->CustomStages)
	{
		if (Entry.Value == EVaRestStageSlot::Outer)
		{
			NewChain->Add(Entry.Key);
			bReissue |= Entry.Key->MayReissue();
		}
	}
	NewChain->Add(MakeAttemptBoundaryStage());
	for (const TPair<FVaRestStageRef, EVaRestStageSlot>& Entry : Next->CustomStages)
	{
		if (Entry.Value == EVaRestStageSlot::PerAttempt)
		{
			NewChain->Add(Entry.Key);
			bReissue |= Entry.Key->MayReissue();
		}
	}
	NewChain->Add(MakeTransportStage());
	Next->Chain = NewChain;
	Next->bMayReissue = bReissue;

	FVaRestRedactRules Rules;
	for (const FString& Key : Next->RedactKeys)
	{
		Rules.AddKey(Key);
	}
	Next->Redactor = MakeShared<FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), TArray<FString>());

	FWriteScopeLock WriteLock(ConfigLock);
	Config = Next;
}

void VaRest::Http::FClientState::ForEachLive(TFunctionRef<void(FClientState&)> Visit)
{
	check(IsInGameThread());
	TArray<TSharedRef<FClientState, ESPMode::ThreadSafe>> Alive;
	{
		ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
		FScopeLock Lock(&Registry.Lock);
		for (const TWeakPtr<FClientState, ESPMode::ThreadSafe>& Weak : Registry.States)
		{
			if (TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Alive.Add(Pinned.ToSharedRef());
			}
		}
	}
	for (const TSharedRef<FClientState, ESPMode::ThreadSafe>& Client : Alive)
	{
		Visit(*Client);
	}
	// Review M-9: a pin taken here can be a state's last reference. Released here, inside FVaRestTrace::AddSink's
	// snapshot, the state's Client{Destroyed} would go out before the new sink is added, and the sink would keep a ghost
	// client. Released from a later game-thread task, it reaches the sink after OnAttached.
	UE::Tasks::Launch(TEXT("VaRest.ReleaseSnapshotPins"), [Kept = MoveTemp(Alive)] {}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}
```

- [ ] **Step 7: Decode helpers** — `Source/VaRestHttp/Private/VaRestDecode.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestResultHandler.h"

namespace VaRest::Http
{
	/** Spec 3.3: "RawBody | first 4 KiB of a non-JSON error body" */
	inline constexpr int32 RawBodyCap = 4096;

	/** The first RawBodyCap bytes as text, ill-formed UTF-8 as U+FFFD */
	FString RawBodyText(TConstArrayView<uint8> Bytes);

	/**
	 * A non-2xx body (spec 3.3): parsed into Error.Body when it is JSON (by type, or sniffed when there is none), else its
	 * first 4 KiB in Error.RawBody; the raw bytes stay in Response.Body for diagnostics (spec 3.2). Background thread.
	 */
	void ProcessErrorBody(FFinish& Finish);
}
```

`Source/VaRestHttp/Private/VaRestDecode.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDecode.h"

#include "VaRestJsonUtf8.h"

namespace VaRest::Http::DecodePrivate
{
	FString ToText(TConstArrayView<uint8> Bytes)
	{
		VaRest::Json::FUtf8Scratch Repaired;
		VaRest::Json::AppendRepairedUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()), Repaired);
		return FString::ConstructFromPtrSize(Repaired.GetData(), Repaired.Num());
	}

	bool LooksLikeJson(TConstArrayView<uint8> Bytes)
	{
		for (const uint8 Byte : Bytes)
		{
			if (Byte == ' ' || Byte == '\t' || Byte == '\r' || Byte == '\n')
			{
				continue;
			}
			return Byte == '{' || Byte == '[';
		}
		return false;
	}
}

FString VaRest::Http::RawBodyText(TConstArrayView<uint8> Bytes)
{
	return DecodePrivate::ToText(Bytes.Left(FMath::Min(Bytes.Num(), RawBodyCap)));
}

void VaRest::Http::ReleaseBody(FFinish& Finish)
{
	if (Finish.bKeepBody)
	{
		Finish.Response.Body = MoveTemp(Finish.Body);
	}
	else
	{
		Finish.Body.Empty();
	}
}

bool VaRest::Http::DecodeJson(FFinish& Finish, FVaRestJson& Out)
{
	Out = FVaRestJson();
	if (Finish.Body.Num() == 0 || Finish.Response.Status == 204)
	{
		ReleaseBody(Finish);
		return true;
	}
	const FString& ContentType = Finish.Response.ContentType;
	if (!ContentType.IsEmpty() && !IsJsonContentType(ContentType))
	{
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("expected a JSON response, got Content-Type \"%s\""), *ContentType);
		Finish.Error.RawBody = RawBodyText(Finish.Body);
		Finish.Response.Body = MoveTemp(Finish.Body); // kept on error (spec 3.2)
		return false;
	}
	const int64 Cap = Finish.ParseOptions.MaxInputBytes;
	if (Cap > 0 && Finish.Body.Num() > Cap)
	{
		// Decision 20: the arena DOM is about 4.2x its input; refused before anything is parsed, the bytes kept (spec 3.2)
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("the JSON response is %d bytes, over the MaxJsonBytes limit of %lld"), Finish.Body.Num(), Cap);
		Finish.Error.RawBody = RawBodyText(Finish.Body);
		Finish.Response.Body = MoveTemp(Finish.Body);
		return false;
	}
	if (Finish.bKeepBody)
	{
		// A copy: the in-situ parse unescapes strings inside the buffer (spec 2.1)
		Finish.Response.Body = Finish.Body;
	}
	// The diagnostic prefix is copied before the parse rewrites the buffer: at most RawBodyCap bytes
	const TArray<uint8> Diagnostic(Finish.Body.GetData(), FMath::Min(Finish.Body.Num(), RawBodyCap));
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::ParseOwned(MoveTemp(Finish.Body), Finish.ParseOptions);
	if (Parsed.HasError())
	{
		const FVaRestJsonError& JsonError = Parsed.GetError();
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = JsonError.Offset >= 0
			? FString::Printf(TEXT("the response is not valid JSON: %s (byte %lld)"), *JsonError.Message, JsonError.Offset)
			: FString::Printf(TEXT("the response is not valid JSON: %s"), *JsonError.Message);
		Finish.Error.RawBody = RawBodyText(Diagnostic);
		return false;
	}
	Out = Parsed.StealValue();
	return true;
}

bool VaRest::Http::DecodeStruct(FFinish& Finish, const UScriptStruct* Struct, void* Data)
{
	FVaRestJson Json;
	if (!DecodeJson(Finish, Json))
	{
		return false;
	}
	if (!Json.IsValid())
	{
		return true; // 204 or empty: the struct keeps its defaults (spec 3.2)
	}
	FVaRestJsonDecodeReport Report;
	const bool bDecoded = FVaRestJsonConvert::JsonToStruct(Json, Struct, Data, Finish.DecodeOptions, &Report);
	Finish.Response.DecodeWarnings = MoveTemp(Report.Warnings);
	if (bDecoded)
	{
		return true;
	}
	const FVaRestJsonError* First = Report.Errors.Num() > 0 ? &Report.Errors[0] : nullptr;
	Finish.Error.Kind = EVaRestErrorKind::Decode;
	Finish.Error.Message = First ? First->Message : FString::Printf(TEXT("the response is not a JSON object for %s"), *Struct->GetName());
	Finish.Error.Path = First ? First->Path : FString();
	// Valid JSON that does not fit: the parsed body is the diagnostic
	Finish.Error.Body = Json;
	return false;
}

void VaRest::Http::DecodeText(FFinish& Finish, FString& Out)
{
	Out = DecodePrivate::ToText(Finish.Body);
	ReleaseBody(Finish);
}

void VaRest::Http::ProcessErrorBody(FFinish& Finish)
{
	if (Finish.Body.Num() == 0)
	{
		return;
	}
	const FString& ContentType = Finish.Response.ContentType;
	const bool bJson = IsJsonContentType(ContentType) || (ContentType.IsEmpty() && DecodePrivate::LooksLikeJson(Finish.Body));
	if (bJson)
	{
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Finish.Body), Finish.ParseOptions);
		if (Parsed.HasValue())
		{
			Finish.Error.Body = Parsed.StealValue();
		}
	}
	if (!Finish.Error.Body.IsValid())
	{
		Finish.Error.RawBody = RawBodyText(Finish.Body);
	}
	Finish.Response.Body = MoveTemp(Finish.Body);
}
```

- [ ] **Step 8: The pipeline** — `Source/VaRestHttp/Private/VaRestPipeline.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"

#include "Tasks/Task.h"
#include "VaRestDecode.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"
#include "VaRestUrl.h"

namespace VaRest::Http::PipelinePrivate
{
	/**
	 * A game-thread task: UE::Tasks' GameThreadNormalPri is ENamedThreads::GameThread (TaskPrivate.cpp:614-615). Every
	 * delivery goes through here. The game thread runs its named queue at frame boundaries and also inside tick-group and
	 * task waits, so a callback can run between tick groups, mid-frame (decision 32; v1 delivered from the core ticker).
	 * Spec 1 says "on the game thread", which this is; M5's Blueprint async actions decide whether to defer to a frame
	 * boundary, and if so it is this one function that changes.
	 */
	template <typename FuncType>
	void OnGameThread(const TCHAR* DebugName, FuncType&& Func)
	{
		UE::Tasks::Launch(DebugName, Forward<FuncType>(Func), UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}

	/** While a public call (Send, Cancel) runs on an exchange, Complete defers its callbacks (decision 3) */
	struct FApiScope
	{
		explicit FApiScope(FVaRestExchange& InExchange)
			: Exchange(InExchange)
		{
			++Exchange.ApiDepth;
		}
		~FApiScope()
		{
			--Exchange.ApiDepth;
		}
		FVaRestExchange& Exchange;
	};

	EVaRestErrorKind KindFor(EVaRestTransportFailure Failure, FName Detail)
	{
		// Review M14: a local failure is not a network one, and M4's retry reads bRetryable from the kind
		static const FName BodyRejected(TEXT("BodyRejected"));
		static const FName InvalidHeader(TEXT("InvalidHeader"));
		static const FName FileWriteFailed(TEXT("ResponseFileWriteFailed"));
		static const FName BodyTooBig(TEXT("ResponseBodyRejected"));
		static const FName TooLarge(TEXT("ResponseTooLarge"));
		if (Detail == BodyRejected || Detail == InvalidHeader)
		{
			return EVaRestErrorKind::Config;
		}
		if (Detail == FileWriteFailed || Detail == BodyTooBig || Detail == TooLarge)
		{
			return EVaRestErrorKind::Decode;
		}
		switch (Failure)
		{
		case EVaRestTransportFailure::None: return EVaRestErrorKind::None;
		case EVaRestTransportFailure::Timeout: return EVaRestErrorKind::Timeout;
		case EVaRestTransportFailure::Canceled: return EVaRestErrorKind::Canceled;
		default: return EVaRestErrorKind::Connection; // Connection, Other: no usable response
		}
	}

	/**
	 * Review I3: decision 11 holds for the final headers too. The builder checks the request's headers, but a custom
	 * PerAttempt stage edits AttemptHeaders after every builder check. A body also needs a Content-Type: curl checks one
	 * for a streamed POST or DELETE (CurlHttp.cpp:945, :983). Returns the first bad name, or null.
	 */
	const FVaRestHeaders::FPair* FindBadHeader(const FVaRestExchange& Ex)
	{
		for (const FVaRestHeaders::FPair& Pair : Ex.AttemptHeaders)
		{
			if (!IsValidHeaderName(Pair.Key) || !IsValidHeaderValue(Pair.Value))
			{
				return &Pair;
			}
			if (!Ex.Body.IsEmpty() && Pair.Key.Equals(TEXT("Content-Type"), ESearchCase::IgnoreCase) && Pair.Value.TrimStartAndEnd().IsEmpty())
			{
				return &Pair;
			}
		}
		return nullptr;
	}

	/**
	 * Review I-3: the transport's completion, wrapped. It carries the exchange as a TSharedPtr that is moved, never
	 * copied, into the game-thread continuation (review I-1: the last reference is dropped on the game thread). If the
	 * transport destroys it uncalled -- the engine does at HTTP module shutdown and with NullHttp -- its destructor
	 * reports TransportDropped through the same path, so the request still finishes once. A moved-from guard holds
	 * nothing and reports nothing.
	 */
	class FCompletionGuard
	{
	public:
		FCompletionGuard(const FVaRestExchangeRef& InExchange, FVaRestOutcomeCallback&& InOutcome)
			: Held(InExchange)
			, Outcome(MoveTemp(InOutcome))
		{
		}
		FCompletionGuard(FCompletionGuard&&) = default;
		FCompletionGuard& operator=(FCompletionGuard&&) = delete;

		~FCompletionGuard()
		{
			if (Held.IsValid())
			{
				FVaRestTransportResult Dropped;
				Dropped.Failure = EVaRestTransportFailure::Other;
				Dropped.FailureDetail = TEXT("TransportDropped");
				Deliver(MoveTemp(Dropped));
			}
		}

		/** Any thread -- the HTTP thread for FHttpModule. It only moves the result to the game thread (spec 1). */
		void Deliver(FVaRestTransportResult&& Result)
		{
			if (!Held.IsValid())
			{
				return;
			}
			OnGameThread(TEXT("VaRest.AttemptDone"), [Target = MoveTemp(Held), Callback = MoveTemp(Outcome), Moved = MoveTemp(Result)]() mutable
			{
				Target->State->OnAttemptDone(Target.ToSharedRef(), MoveTemp(Moved), MoveTemp(Callback));
			});
		}

	private:
		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Held;
		FVaRestOutcomeCallback Outcome;
	};

	/** Background thread (or the game thread for nothing but the tests' restore-the-bug): spec 1's "parsing, decoding ... run on UE::Tasks" */
	void RunDecode(IResultHandler& Handler, FFinish& Finish)
	{
#if !UE_BUILD_SHIPPING
		if (IsInGameThread())
		{
			++Testing::Counters().DecodesOnGameThread;
		}
		else
		{
			++Testing::Counters().DecodesOffGameThread;
		}
#endif
		if (Finish.bTimeDecode)
		{
			Finish.DecodeStartAt = Finish.Clock->Now();
		}
		if (!Finish.Error.IsError())
		{
			Handler.Decode(Finish);
		}
		else if (Finish.Error.Kind == EVaRestErrorKind::Http)
		{
			ProcessErrorBody(Finish);
		}
		if (Finish.bTimeDecode)
		{
			Finish.DecodeEndAt = Finish.Clock->Now();
		}
	}

	/** Spec 4's boundary between Outer and PerAttempt stages: each pass is a new attempt */
	class FAttemptBoundaryStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("AttemptBoundary"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FVaRestExchange& Ex = *Exchange;
			++Ex.Attempt;
			Ex.AttemptStartedAt = Ex.Config->Clock->Now();
			Ex.AttemptHeaders = Ex.RequestHeaders;
			Ex.LimiterWait = 0.0;
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};

	/** The chain's last stage: hands the attempt to IVaRestTransport */
	class FTransportStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Transport"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FVaRestExchange& Ex = *Exchange;
			const FClientConfig& Cfg = *Ex.Config;
			if (const FVaRestHeaders::FPair* Bad = FindBadHeader(Ex))
			{
				// Review I3: a header a stage set (or an empty Content-Type with a body) never reaches the transport
				UE_LOG(LogVaRestHttp, Error, TEXT("#%llu VaRest: header \"%s\" is not a token, holds CR, LF or NUL, or is an empty Content-Type with a body; the request fails with Kind=Config"),
					Ex.RequestId, *Bad->Key);
				FVaRestAttemptOutcome Refused;
				Refused.Failure = EVaRestTransportFailure::Other;
				Refused.FailureDetail = TEXT("InvalidHeader");
				OnOutcome(MoveTemp(Refused));
				return;
			}
			FVaRestTransportRequest Request;
			Request.RequestId = Ex.RequestId;
			Request.Attempt = Ex.Attempt;
			Request.Verb = Ex.Verb;
			Request.Url = Ex.Url;
			Request.Headers = Ex.AttemptHeaders;
			if (!Ex.Body.FilePath.IsEmpty())
			{
				Request.BodyFile = Ex.Body.FilePath;
			}
			else if (Cfg.bMayReissue)
			{
				// A later attempt needs the body again (decision 12)
				Request.BodySegments = Ex.Body.Segments;
				Request.Body = Ex.Body.Bytes;
			}
			else
			{
				// One attempt: moved all the way into IHttpRequest::SetContent(TArray<uint8>&&) (spec 2.4)
				Request.BodySegments = MoveTemp(Ex.Body.Segments);
				Request.Body = MoveTemp(Ex.Body.Bytes);
			}
			Request.Timeout = Ex.Timeout;
			Request.ActivityTimeout = Ex.ActivityTimeout;
			Request.Priority = Ex.Priority;
			Request.ResponseFile = Ex.TempFile;
			// Reviews M-6, M13: the transport stops a JSON body past its cap instead of receiving all of it first
			Request.MaxBodyBytes = Ex.Handler.IsValid() && Ex.Handler->DecodesJson() ? Ex.MaxJsonBytes : 0;
			Request.Clock = Cfg.Clock;
			Request.Probe->UploadTotal = Ex.Body.ContentLength;
			Ex.Probe = Request.Probe;
			Ex.TransportCall = Cfg.Transport->Start(MoveTemp(Request), [Guard = FCompletionGuard(Exchange, MoveTemp(OnOutcome))](FVaRestTransportResult&& Result) mutable
			{
				Guard.Deliver(MoveTemp(Result));
			});
		}
	};
}

FVaRestAttemptOutcome FVaRestAttemptOutcome::Canceled()
{
	FVaRestAttemptOutcome Out;
	Out.Failure = EVaRestTransportFailure::Canceled;
	Out.FailureDetail = TEXT("Canceled");
	return Out;
}

FVaRestExchange::~FVaRestExchange()
{
	if (!IsInGameThread() && (State.IsValid() || Config.IsValid()))
	{
		// Review I-1 backstop: a pin taken on another thread (IsActive, Cancel) can be the last reference
		VaRest::Http::PipelinePrivate::OnGameThread(TEXT("VaRest.ReleaseExchange"), [DoomedState = MoveTemp(State), DoomedConfig = MoveTemp(Config)] {});
	}
}

void FVaRestStageNext::Proceed(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& OnOutcome) const
{
	check(IsInGameThread());
	if (Exchange->bFinished || Exchange->IsCancelRequested())
	{
		OnOutcome(FVaRestAttemptOutcome::Canceled());
		return;
	}
	checkf(Chain->IsValidIndex(Index), TEXT("FVaRestStageNext: the transport stage is last and never proceeds"));
	(*Chain)[Index]->Process(Exchange, FVaRestStageNext(Chain, Index + 1), MoveTemp(OnOutcome));
}

FVaRestStageRef VaRest::Http::MakeAttemptBoundaryStage()
{
	return MakeShared<PipelinePrivate::FAttemptBoundaryStage, ESPMode::ThreadSafe>();
}

FVaRestStageRef VaRest::Http::MakeTransportStage()
{
	return MakeShared<PipelinePrivate::FTransportStage, ESPMode::ThreadSafe>();
}

VaRest::Http::FFinish VaRest::Http::FClientState::MakeFinish(const FVaRestExchange& Exchange) const
{
	FFinish Finish;
	Finish.Response.RequestId = Exchange.RequestId;
	Finish.Response.Attempts = Exchange.Attempt;
	Finish.SaveTo = Exchange.SaveTo;
	Finish.TempFile = Exchange.TempFile;
	Finish.bKeepBody = Exchange.bKeepBody;
	Finish.DecodeOptions = Exchange.DecodeOptions;
	Finish.ParseOptions.MaxInputBytes = Exchange.MaxJsonBytes;
	Finish.Clock = Exchange.Config->Clock;
	return Finish;
}

bool VaRest::Http::FClientState::Resolve(FVaRestExchange& Exchange, FString& OutError) const
{
	const FClientConfig& Cfg = *Exchange.Config;
	if (Cfg.BaseUrl.Contains(TEXT("#")))
	{
		// Decision 33 (review M22): a base URL may carry a query (kept, before the request's own pairs), not a fragment
		OutError = FString::Printf(TEXT("the base URL \"%s\" has a fragment; a base URL may carry a query but not a fragment"), *Cfg.Redactor->RedactUrl(Cfg.BaseUrl));
		return false;
	}
	int32 QueryAt = INDEX_NONE;
	const bool bHasQuery = Exchange.RawPath.FindChar(TEXT('?'), QueryAt);
	Exchange.PathTemplate = bHasQuery ? Exchange.RawPath.Left(QueryAt) : Exchange.RawPath;

	// The template is expanded before the join, so a brace in the base URL is never touched. Only the path is a template
	// (review M12): a brace in the query is the caller's text and goes out as written.
	TValueOrError<FString, FString> Expanded = ExpandPathTemplate(Exchange.PathTemplate, Exchange.PathParams);
	if (Expanded.HasError())
	{
		OutError = Expanded.StealError();
		return false;
	}
	FString Resolved = JoinUrl(Cfg.BaseUrl, bHasQuery ? Expanded.GetValue() + Exchange.RawPath.Mid(QueryAt) : Expanded.GetValue());
	if (!IsAbsoluteHttpUrl(Resolved))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not an absolute http(s) URL (base URL \"%s\")"), *Cfg.Redactor->RedactUrl(Resolved), *Cfg.Redactor->RedactUrl(Cfg.BaseUrl));
		return false;
	}
	AppendQuery(Resolved, Exchange.Query);
	Exchange.Url = MoveTemp(Resolved);

	// Client defaults, then the request's own: they win whatever the case of the name (spec 3.2: "Every content type can be overridden with Header")
	Exchange.RequestHeaders = Cfg.DefaultHeaders;
	for (const FVaRestHeaders::FPair& Pair : Exchange.OwnHeaders)
	{
		Exchange.RequestHeaders.Set(Pair.Key, Pair.Value);
	}
	if (!Exchange.Body.IsEmpty() && !Exchange.RequestHeaders.Contains(TEXT("Content-Type")))
	{
		Exchange.RequestHeaders.Set(TEXT("Content-Type"), Exchange.Body.ContentType);
	}

	Exchange.Timeout = Exchange.TimeoutOverride.Get(Cfg.Timeout);
	Exchange.ActivityTimeout = Exchange.ActivityTimeoutOverride.Get(Cfg.ActivityTimeout);
	Exchange.MaxJsonBytes = Exchange.MaxJsonBytesOverride.Get(Cfg.MaxJsonBytes);
	Exchange.DecodeOptions.Naming = Exchange.Naming;
	Exchange.DecodeOptions.bStrict = Exchange.bStrictDecode;
	Exchange.LogLevel = Cfg.LogLevel;
	return true;
}

void VaRest::Http::FClientState::Enter(const FVaRestExchangeRef& Exchange)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FVaRestExchange& Ex = *Exchange;
	FApiScope Scope(Ex);
	// Spec 3.1: "A request snapshots the configuration at Send()" -- SendRequest took it (decision 27); an exchange made
	// any other way gets one here
	if (!Ex.Config.IsValid())
	{
		Ex.Config = GetConfig();
	}
	Ex.StartedAt = Ex.Config->Clock->Now();
	Ex.Phase = EVaRestExchangePhase::InChain;
	InFlight.Add(Ex.RequestId, Exchange);

	FString Problem;
	if (!Ex.BuildError.IsEmpty() || !Resolve(Ex, Problem))
	{
		FFinish Finish = MakeFinish(Ex);
		Finish.Error.Kind = EVaRestErrorKind::Config;
		Finish.Error.Message = Ex.BuildError.IsEmpty() ? MoveTemp(Problem) : Ex.BuildError;
		Complete(Exchange, MoveTemp(Finish));
		return;
	}
	FVaRestStageNext(Ex.Config->Chain.ToSharedRef(), 0).Proceed(Exchange, [Exchange](FVaRestAttemptOutcome&& Outcome)
	{
		Exchange->State->OnChainDone(Exchange, MoveTemp(Outcome));
	});
}

void VaRest::Http::FClientState::OnAttemptDone(const FVaRestExchangeRef& Exchange, FVaRestTransportResult&& Result, FVaRestOutcomeCallback&& OnOutcome)
{
	check(IsInGameThread());
	FVaRestExchange& Ex = *Exchange;
	Ex.TransportCall.Reset();
	FVaRestAttemptOutcome Outcome;
	Outcome.Status = Result.Status;
	Outcome.Headers = MoveTemp(Result.Headers);
	Outcome.EffectiveUrl = MoveTemp(Result.EffectiveUrl);
	Outcome.ContentType = MoveTemp(Result.ContentType);
	Outcome.Body = MoveTemp(Result.Body);
	Outcome.bBodyInFile = Result.bBodyInFile;
	Outcome.Failure = Result.Failure;
	Outcome.FailureDetail = Result.FailureDetail;
	Outcome.BytesUp = Result.BytesUp;
	Outcome.BytesDown = Result.BytesDown;
	OnOutcome(MoveTemp(Outcome));
}

void VaRest::Http::FClientState::OnChainDone(const FVaRestExchangeRef& Exchange, FVaRestAttemptOutcome&& Outcome)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FVaRestExchange& Ex = *Exchange;
	if (Ex.bFinished || Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Finished, or a second outcome while the first is being decoded (a stage that reported twice): ignored (review C-3)
		return;
	}
	FFinish Finish = MakeFinish(Ex);
	Finish.Response.Status = Outcome.Status;
	Finish.Response.Headers = MoveTemp(Outcome.Headers);
	Finish.Response.EffectiveUrl = MoveTemp(Outcome.EffectiveUrl);
	Finish.Response.ContentType = MoveTemp(Outcome.ContentType);
	Finish.Response.BytesUp = Outcome.BytesUp;
	Finish.Response.BytesDown = Outcome.BytesDown;
	Finish.Body = MoveTemp(Outcome.Body);
	Finish.bBodyInFile = Outcome.bBodyInFile;

	static const FName TooLarge(TEXT("ResponseTooLarge"));
	static const FName Dropped(TEXT("TransportDropped"));
	if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge && Outcome.Status > 0 && !IsSuccessStatus(Outcome.Status))
	{
		// Reviews M-6, M13: an error response whose body passed the JSON cap is still the HTTP error it was; the body is gone
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FString::Printf(TEXT("%s (its body is over the MaxJsonBytes limit of %lld and was not kept)"),
			*FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status), Ex.MaxJsonBytes);
		Finish.Error.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
	}
	else if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge)
	{
		// Decision 20: the transport stopped the body at the cap -- bounded memory on untrusted input
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("the JSON response is over the MaxJsonBytes limit of %lld; the transfer stopped there"), Ex.MaxJsonBytes);
	}
	else if (Outcome.IsTransportFailure())
	{
		Finish.Error.Kind = KindFor(Outcome.Failure, Outcome.FailureDetail);
		Finish.Error.Message = FVaRestError::DefaultMessage(Finish.Error.Kind, 0);
		if (!Outcome.FailureDetail.IsNone())
		{
			Finish.Error.Message += FString::Printf(TEXT(" (%s)"), *Outcome.FailureDetail.ToString());
		}
		// A dropped completion (engine shutdown, NullHttp) would only be dropped again
		Finish.Error.bRetryable = (Finish.Error.Kind == EVaRestErrorKind::Connection || Finish.Error.Kind == EVaRestErrorKind::Timeout)
			&& Outcome.FailureDetail != Dropped;
	}
	else if (!IsSuccessStatus(Outcome.Status))
	{
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status);
		Finish.Error.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
	}

	const bool bHasBody = Finish.Body.Num() > 0 || Finish.bBodyInFile;
	const bool bTask = Finish.Error.IsError()
		? Finish.Error.Kind == EVaRestErrorKind::Http && bHasBody
		: (Ex.Handler->DecodesBody() && bHasBody) || !Finish.SaveTo.IsEmpty();
	if (!bTask)
	{
		// Nothing to parse (no body, raw bytes, void): the game thread only moves things
		if (!Finish.Error.IsError())
		{
			Ex.Handler->Decode(Finish);
		}
		Complete(Exchange, MoveTemp(Finish));
		return;
	}

	Finish.bTimeDecode = FVaRestTrace::Detail() != EVaRestTraceDetail::None;
	Ex.Phase = EVaRestExchangePhase::Decoding;
	// The handler goes with the task and comes back with the result: nothing else touches it meanwhile
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	// The exchange travels as a TSharedPtr moved into the continuation, so the worker never drops the last reference
	// (review I-1; TSharedRef's move constructor copies, SharedPointer.h:354-361)
	UE::Tasks::Launch(TEXT("VaRest.Decode"), [Held = TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe>(Exchange), Decoder = MoveTemp(Handler), Pending = MoveTemp(Finish)]() mutable
	{
		// Spec 1: "Parsing, decoding, cache disk IO ... run on UE::Tasks background tasks"
		RunDecode(*Decoder, Pending);
		OnGameThread(TEXT("VaRest.Deliver"), [Target = MoveTemp(Held), Delivered = MoveTemp(Decoder), Done = MoveTemp(Pending)]() mutable
		{
			Target->Handler = MoveTemp(Delivered);
			Target->State->Complete(Target.ToSharedRef(), MoveTemp(Done));
		});
	});
}

void VaRest::Http::FClientState::Complete(const FVaRestExchangeRef& Exchange, FFinish&& Finish)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FVaRestExchange& Ex = *Exchange;
	if (Ex.bFinished)
	{
		return;
	}
	if (Ex.Phase == EVaRestExchangePhase::Decoding && !Ex.Handler.IsValid())
	{
		// Review C-3: a decode task holds the handler. It completes the exchange when it returns -- as Canceled when the
		// flag is set -- so finishing here would leave nothing to deliver to and lose every callback.
		return;
	}
	if (Ex.ApiDepth > 0)
	{
		// Decision 3: never inside Send() or Cancel(); the callbacks run from a later game-thread task
		OnGameThread(TEXT("VaRest.Complete"), [Exchange, Pending = MoveTemp(Finish)]() mutable
		{
			Exchange->State->Complete(Exchange, MoveTemp(Pending));
		});
		return;
	}
	Ex.bFinished = true;
	Ex.Phase = EVaRestExchangePhase::Done;

	const FClientConfig& Cfg = *Ex.Config;
	Finish.Response.Elapsed = Cfg.Clock->Now() - Ex.StartedAt;
	Finish.Response.Attempts = Ex.Attempt;
	if (Finish.Error.IsError())
	{
		Finish.Error.Attempts = Ex.Attempt;
		Finish.Error.Tag = Ex.Tag;
		// Spec 3.3: "Url (redacted)"
		Finish.Error.Url = Cfg.Redactor->RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
		if (Finish.Error.Message.IsEmpty())
		{
			Finish.Error.Message = FVaRestError::DefaultMessage(Finish.Error.Kind, Finish.Error.HttpStatus);
		}
	}

	Ex.bDone = true;
	InFlight.Remove(Ex.RequestId);
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	if (Handler.IsValid())
	{
		Handler->Deliver(Finish);
	}
}
```

- [ ] **Step 9: The client, the handle and Send** — `Source/VaRestHttp/Private/VaRestClient.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClient.h"

#include "Tasks/Task.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpSettings.h"
#include "VaRestRedactor.h"
#include "VaRestTrace.h"

namespace VaRest::Http::ClientPrivate
{
	/** Decision 33 (review M22): said once, where the base URL is set; each request then fails with Kind=Config */
	void WarnIfFragment(const FString& BaseUrl)
	{
		if (BaseUrl.Contains(TEXT("#")))
		{
			UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: the base URL \"%s\" has a fragment; a base URL may carry a query but not a fragment, so this client's requests fail with Kind=Config"),
				*FVaRestRedactor().RedactUrl(BaseUrl));
		}
	}
}

VaRest::Http::FRequestSpec::FRequestSpec(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState, FStringView InVerb, FStringView InPath)
	: State(MoveTemp(InState))
	, Verb(InVerb)
	, Path(InPath)
{
	if (State.IsValid())
	{
		// Any thread: the configuration is an immutable snapshot behind a lock
		ConfigAtBuild = State->GetConfig();
		Naming = ConfigAtBuild->Naming;
	}
}

VaRest::Http::FRequestSpec::~FRequestSpec() = default;

void VaRest::Http::FRequestSpec::Fail(FString Problem)
{
	if (BuildError.IsEmpty())
	{
		BuildError = MoveTemp(Problem);
	}
}

FVaRestRequestHandle VaRest::Http::SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine)
{
	check(Spec.IsValid());
	Spec->bSent = true;
	if (!Spec->State.IsValid())
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: Send() on an empty FVaRestClient (make one with FVaRestClient::Make); the %s request is dropped"), *Spec->Verb);
		return FVaRestRequestHandle();
	}
	FVaRestExchangeRef Exchange = MakeShared<FVaRestExchange, ESPMode::ThreadSafe>();
	FVaRestExchange& Ex = *Exchange;
	Ex.RequestId = FVaRestTrace::NewRequestId(); // IN-10: assigned at creation, whatever the trace mask
	Ex.ClientId = Spec->State->GetClientId();
	Ex.State = Spec->State;
	// Spec 3.1, verbatim: "A request snapshots the configuration at Send()" -- here, on the sending thread, under the
	// state's read lock (decision 27). A struct body was encoded in Body() with the naming of that moment.
	Ex.Config = Spec->State->GetConfig();
	Ex.Verb = MoveTemp(Spec->Verb);
	Ex.RawPath = MoveTemp(Spec->Path);
	Ex.PathParams = MoveTemp(Spec->PathParams);
	Ex.Query = MoveTemp(Spec->Query);
	Ex.OwnHeaders = MoveTemp(Spec->Headers);
	Ex.Body = MoveTemp(Spec->Body);
	Ex.BuildError = MoveTemp(Spec->BuildError);
	Ex.Tag = Spec->Tag;
	Ex.Priority = Spec->Priority;
	Ex.bIdempotent = Spec->bIdempotent;
	Ex.bKeepBody = Spec->bKeepBody;
	Ex.bOnHttpThread = Spec->bOnHttpThread;
	Ex.SaveTo = MoveTemp(Spec->SaveTo);
	Ex.TimeoutOverride = Spec->Timeout;
	Ex.ActivityTimeoutOverride = Spec->ActivityTimeout;
	Ex.MaxJsonBytesOverride = Spec->MaxJsonBytes;
	Ex.Naming = Spec->Naming;
	Ex.bStrictDecode = Spec->bStrictDecode;
	Ex.Owner = Spec->Owner;
	Ex.bHasOwner = Spec->bHasOwner;
	Ex.SourceFile = SourceFile;
	Ex.SourceLine = SourceLine;
	Ex.Handler = MoveTemp(Spec->Handler);

	FVaRestRequestHandle Handle(Exchange);
	if (IsInGameThread())
	{
		Ex.State->Enter(Exchange);
	}
	else
	{
		// Spec 1: "A request may be sent from any thread; it is marshalled to the game thread before entering the pipeline"
		UE::Tasks::Launch(TEXT("VaRest.Enter"), [Exchange] { Exchange->State->Enter(Exchange); },
			UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	return Handle;
}

FVaRestRequestHandle::FVaRestRequestHandle(const FVaRestExchangeRef& InExchange)
	: Exchange(InExchange)
	, Id(InExchange->RequestId)
{
}

bool FVaRestRequestHandle::IsActive() const
{
	const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Exchange.Pin();
	return Pinned.IsValid() && !Pinned->IsDone();
}

FVaRestClient FVaRestClient::Make(FStringView BaseUrl)
{
	checkf(IsInGameThread(), TEXT("FVaRestClient::Make runs on the game thread"));
	const UVaRestHttpSettings* Settings = GetDefault<UVaRestHttpSettings>();
	VaRest::Http::FClientConfig Initial;
	Initial.BaseUrl = FString(BaseUrl.TrimStartAndEnd());
	Initial.DisplayName = Initial.BaseUrl;
	Initial.LogLevel = Settings->LogLevel;
	Initial.MaxConcurrent = FMath::Max(1, Settings->MaxConcurrent);
	Initial.Timeout = FMath::Max(0.f, Settings->Timeout);
	Initial.ActivityTimeout = FMath::Max(0.f, Settings->ActivityTimeout);
	Initial.MaxJsonBytes = FMath::Max<int64>(0, Settings->MaxJsonResponseBytes);
	Initial.RedactKeys = Settings->RedactKeys;
	Initial.Transport = VaRest::Http::GetDefaultTransport();
	Initial.Clock = FVaRestRealClock::Get();
	VaRest::Http::ClientPrivate::WarnIfFragment(Initial.BaseUrl);
	return FVaRestClient(VaRest::Http::FClientState::Create(MoveTemp(Initial)));
}

uint64 FVaRestClient::GetId() const
{
	return State.IsValid() ? State->GetClientId() : 0;
}

FString FVaRestClient::GetBaseUrl() const
{
	return State.IsValid() ? State->GetConfig()->BaseUrl : FString();
}

int32 FVaRestClient::GetInFlightCount() const
{
	check(IsInGameThread());
	return State.IsValid() ? State->NumInFlight() : 0;
}

FVaRestClient& FVaRestClient::WithName(FStringView DisplayName)
{
	if (State.IsValid())
	{
		State->EditConfig([DisplayName](VaRest::Http::FClientConfig& Edited) { Edited.DisplayName = FString(DisplayName); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithBaseUrl(FStringView BaseUrl)
{
	if (State.IsValid())
	{
		VaRest::Http::ClientPrivate::WarnIfFragment(FString(BaseUrl.TrimStartAndEnd()));
		State->EditConfig([BaseUrl](VaRest::Http::FClientConfig& Edited) { Edited.BaseUrl = FString(BaseUrl.TrimStartAndEnd()); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithHeader(FStringView Name, FStringView Value)
{
	if (State.IsValid())
	{
		State->EditConfig([Name, Value](VaRest::Http::FClientConfig& Edited) { Edited.DefaultHeaders.Set(Name, Value); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithoutHeader(FStringView Name)
{
	if (State.IsValid())
	{
		State->EditConfig([Name](VaRest::Http::FClientConfig& Edited) { Edited.DefaultHeaders.Remove(Name); });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithNaming(EVaRestJsonNaming Naming)
{
	if (State.IsValid())
	{
		State->EditConfig([Naming](VaRest::Http::FClientConfig& Edited) { Edited.Naming = Naming; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithTransport(TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Transport)
{
	if (State.IsValid())
	{
		State->EditConfig([&Transport](VaRest::Http::FClientConfig& Edited) { Edited.Transport = Transport; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithClock(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock)
{
	if (State.IsValid())
	{
		State->EditConfig([&Clock](VaRest::Http::FClientConfig& Edited) { Edited.Clock = Clock; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::Use(FVaRestStageRef Stage, EVaRestStageSlot Slot)
{
	if (State.IsValid())
	{
		State->EditConfig([&Stage, Slot](VaRest::Http::FClientConfig& Edited) { Edited.CustomStages.Emplace(Stage, Slot); });
	}
	return *this;
}
```

- [ ] **Step 10: The outcome recorder and the test rig** — in `Source/VaRestHttp/Public/VaRestHttpTesting.h`, insert at Task 6's marker comment, inside the `#if WITH_DEV_AUTOMATION_TESTS` block (add `#include "VaRestClient.h"` and `#include "VaRestFakeTransport.h"` to its includes):

```cpp
namespace VaRest::Http::Testing
{
	/** What a request's callbacks did; shared, so a test that times out leaves no dangling reference */
	template <typename T>
	struct TTestOutcome
	{
		std::atomic<int32> SuccessCalls{ 0 };
		std::atomic<int32> ErrorCalls{ 0 };
		std::atomic<int32> CompleteCalls{ 0 };
		std::atomic<bool> bAllOnGameThread{ true };
		TOptional<T> Value;
		FVaRestError Error;
		FVaRestResponse Response;

		bool IsDone() const { return CompleteCalls.load() > 0; }
	};

	template <>
	struct TTestOutcome<void>
	{
		std::atomic<int32> SuccessCalls{ 0 };
		std::atomic<int32> ErrorCalls{ 0 };
		std::atomic<int32> CompleteCalls{ 0 };
		std::atomic<bool> bAllOnGameThread{ true };
		FVaRestError Error;
		FVaRestResponse Response;

		bool IsDone() const { return CompleteCalls.load() > 0; }
	};

	template <typename T>
	TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe> MakeOutcome()
	{
		return MakeShared<TTestOutcome<T>, ESPMode::ThreadSafe>();
	}

	/** Wires OnSuccess, OnError and OnComplete into Outcome. OnComplete is counted last, after the value and error are stored. */
	template <typename T>
	TVaRestRequest<T> Observe(TVaRestRequest<T>&& Request, const TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome)
	{
		auto Tracked = [Outcome](const FVaRestError& Error)
		{
			Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
			Outcome->Error = Error;
			++Outcome->ErrorCalls;
		};
		auto Completed = [Outcome](const TVaRestResult<T>& Result)
		{
			Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
			Outcome->Response = Result.Response;
			++Outcome->CompleteCalls;
		};
		if constexpr (std::is_void_v<T>)
		{
			return MoveTemp(Request)
				.OnSuccess([Outcome](const FVaRestResponse&)
				{
					Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
					++Outcome->SuccessCalls;
				})
				.OnError(MoveTemp(Tracked))
				.OnComplete(MoveTemp(Completed));
		}
		else
		{
			return MoveTemp(Request)
				.OnSuccess([Outcome](const T& Result, const FVaRestResponse&)
				{
					Outcome->bAllOnGameThread = Outcome->bAllOnGameThread.load() && IsInGameThread();
					Outcome->Value = Result;
					++Outcome->SuccessCalls;
				})
				.OnError(MoveTemp(Tracked))
				.OnComplete(MoveTemp(Completed));
		}
	}

	/** A client on the fake transport and the virtual clock (spec 7.1). Game thread. */
	struct VARESTHTTP_API FTestRig
	{
		explicit FTestRig(FStringView BaseUrl = TEXTVIEW("https://api.test"));

		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
		TSharedRef<FVaRestFakeTransport, ESPMode::ThreadSafe> Fake;
		FVaRestClient Client;

		template <typename T>
		bool Wait(const TSharedRef<TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome, double TimeoutSeconds = 5.0) const
		{
			return PumpUntil([&Outcome] { return Outcome->IsDone(); }, TimeoutSeconds);
		}
	};
}
```

In `Source/VaRestHttp/Private/VaRestHttpTesting.cpp`, before its closing `#endif // WITH_DEV_AUTOMATION_TESTS`:

```cpp
VaRest::Http::Testing::FTestRig::FTestRig(FStringView BaseUrl)
	: Clock(MakeShared<FVaRestVirtualClock, ESPMode::ThreadSafe>())
	, Fake(MakeShared<FVaRestFakeTransport, ESPMode::ThreadSafe>(Clock))
	, Client(FVaRestClient::Make(BaseUrl))
{
	Client.WithTransport(Fake).WithClock(Clock);
}
```

- [ ] **Step 11: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including the thirteen `VaRest.Http.Pipeline.*` tests. Then `Tools/run_tests.sh --no-build VaRest` (everything stays green).

- [ ] **Step 12: Restore-the-bug (the game-thread hop)**

In `FCompletionGuard::Deliver`, temporarily call `Held->State->OnAttemptDone(Held.ToSharedRef(), MoveTemp(Result), MoveTemp(Outcome));` directly instead of posting the game-thread task. Run `Tools/run_tests.sh VaRest.Http.Pipeline`; expected FAIL: `VaRest.Http.Pipeline.ManyConcurrent` and `.GetJson` (callbacks off the game thread, or a `check(IsInGameThread())` assertion in `OnAttemptDone` stops the run — either is the failure). Restore; PASS. Report both runs.

- [ ] **Step 13: Restore-the-bug (callbacks never inside Send)**

In `FClientState::Complete`, temporarily delete the `if (Ex.ApiDepth > 0) { ... }` block. Run `Tools/run_tests.sh VaRest.Http.Pipeline`; expected FAIL in `VaRest.Http.Pipeline.NeverInsideSend` ("decision 3: no callback runs inside Send()"). Restore; PASS.

- [ ] **Step 13b: Restore-the-bug (one whole configuration per request, reviews I-7, I6)**

In the test `VaRest.Http.Pipeline.ConfigEditedWhileSending`, temporarily split `EditBoth` into two edits (`Rig.Client.WithHeader(TEXT("X-A"), LexToString(Gen)); Rig.Client.WithHeader(TEXT("X-B"), LexToString(Gen));`). Run `Tools/run_tests.sh VaRest.Http.Pipeline.ConfigEditedWhileSending`; expected FAIL ("every request carries one whole configuration (X-A == X-B)"): with four workers sending 200 requests against 200 edit pairs, a worker's snapshot lands between the two edits. Restore; PASS. Report both runs (if the broken run passes, run it again and report that too: the window is a few microseconds per edit).

- [ ] **Step 14: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestResultHandler.h Source/VaRestHttp/Public/VaRestStage.h Source/VaRestHttp/Public/VaRestClient.h \
  Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp \
  Source/VaRestHttp/Private/VaRestDecode.h Source/VaRestHttp/Private/VaRestDecode.cpp Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Public/VaRestHttpTesting.h Source/VaRestHttp/Private/VaRestHttpTesting.cpp Source/VaRestHttp/Private/Tests/VaRestHttpPipelineTest.cpp
git commit -m "http: FVaRestClient, the exchange and the stage pipeline -- any-thread send, game-thread control plane, decode on UE::Tasks

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 9: Request building — struct, form, multipart and file bodies; header checks; the GET rule; idempotency keys; the unsent-builder error

Spec §3.2: "Bodies: struct/`FVaRestJson` → `application/json` UTF-8; `FString` → `text/plain; charset=utf-8`; bytes → `application/octet-stream`; `FVaRestForm` → `application/x-www-form-urlencoded`; `FVaRestMultipart` → `multipart/form-data`, file parts streamed with `SetContentFromStream`. A body on GET is dropped with a warning (v1 rule). Every content type can be overridden with `Header`." Struct bodies encode "straight to UTF-8 bytes" (spec §2.4) in `Body()`, with the naming policy known at that moment (the request's `Naming()`, else the client's): `Naming()` after a struct body is a misuse and fails the request. `Idempotent()` "adds a stable `Idempotency-Key: <uuid>` across attempts" — generated once, on entry, so a re-issue (M4's retry) sends the same key. "In non-Shipping builds a builder destroyed unsent also logs an error naming the URL" (redacted, as every log line is). Header names and values are checked (decision 11).

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`TVaRestRequest`: `Header` checks, struct/form/multipart bodies, `BodyFile`, `Idempotent`, `Naming`, `StrictDecode`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (`~FRequestSpec`, `WithHeader` checks)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Resolve`: the GET rule, the idempotency key)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpBuilderTest.cpp`

**Interfaces:**
- Consumes: Task 8's `TVaRestRequest`, `FRequestSpec`, `FClientState::Resolve`; `FVaRestRequestBody::FromForm/FromMultipart/FromFile`, `IsValidHeaderName/Value` (Task 3); `FVaRestJsonConvert::StructToUtf8` (VaRestJson).
- Produces (`TVaRestRequest<T>`): `template <typename StructType> requires (VaRest::Json::bHasScriptStruct<StructType> && !std::is_same_v<StructType, FVaRestJson>) Body(const StructType&) &&`; `Body(const FVaRestForm&) &&`; `Body(const FVaRestMultipart&) &&`; `BodyFile(FStringView FilePath, FStringView ContentType = "application/octet-stream") &&`; `Idempotent() &&`; `Naming(EVaRestJsonNaming) &&`; `StrictDecode(bool bStrict = true) &&`; `Header` now validates.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpBuilderTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::BuilderTestPrivate
{
	FString Text(const TArray<uint8>& Bytes)
	{
		return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
	}

	/** Sends Request through Rig and waits; true when it completed */
	template <typename T>
	bool Run(VaRest::Http::Testing::FTestRig& Rig, TVaRestRequest<T>&& Request)
	{
		auto Outcome = VaRest::Http::Testing::MakeOutcome<T>();
		VaRest::Http::Testing::Observe(MoveTemp(Request), Outcome).Send();
		return Rig.Wait(Outcome);
	}

	/** A PerAttempt stage that signs with a value holding CRLF (review I3) */
	class FBadSigner final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("BadSigner"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Exchange->GetAttemptHeaders().Set(TEXT("X-Signed"), TEXT("a\r\nX-Evil: 1"));
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderBodiesTest, "VaRest.Http.Builder.Bodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	VaRest::Http::Testing::FTestRig Rig;
	const FIntPoint Point(3, 4);

	TestTrue(TEXT("json"), Run(Rig, Rig.Client.Post(TEXT("/j")).Body(FVaRestJson::Object().Set(TEXT("a"), 1))));
	TestTrue(TEXT("struct, the client's naming (camelCase by default)"), Run(Rig, Rig.Client.Post(TEXT("/s")).Body(Point)));
	TestTrue(TEXT("struct, the request's naming, set first"), Run(Rig, Rig.Client.Post(TEXT("/n")).Naming(EVaRestJsonNaming::PascalCase).Body(Point)));
	TestTrue(TEXT("text"), Run(Rig, Rig.Client.Put(TEXT("/t")).Body(TEXT("café"))));
	TestTrue(TEXT("bytes"), Run(Rig, Rig.Client.Put(TEXT("/b")).Body(TArray<uint8>{ 1, 2, 3 })));
	TestTrue(TEXT("form"), Run(Rig, Rig.Client.Post(TEXT("/f")).Body(FVaRestForm().Add(TEXT("a b"), TEXT("c&d")))));
	TestTrue(TEXT("multipart"), Run(Rig, Rig.Client.Post(TEXT("/m")).Body(FVaRestMultipart(TEXT("BB")).AddField(TEXT("k"), TEXT("v")))));
	TestTrue(TEXT("a Content-Type header wins over the body's, set before or after"),
		Run(Rig, Rig.Client.Post(TEXT("/o")).Header(TEXT("content-type"), TEXT("application/vnd.game+json")).Body(FVaRestJson::Object())));

	auto Call = [&Rig](int32 Index) -> const FVaRestTransportRequest& { return Rig.Fake->GetCall(Index).Request; };
	TestEqualSensitive(TEXT("json body"), Text(Call(0).Body), FString(TEXT("{\"a\":1}")));
	TestEqualSensitive(TEXT("json type"), Call(0).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestEqualSensitive(TEXT("struct body, camelCase"), Text(Call(1).Body), FString(TEXT("{\"x\":3,\"y\":4}")));
	TestEqualSensitive(TEXT("struct body, PascalCase"), Text(Call(2).Body), FString(TEXT("{\"X\":3,\"Y\":4}")));
	TestEqualSensitive(TEXT("text type"), Call(3).Headers.Get(TEXT("Content-Type")), FString(TEXT("text/plain; charset=utf-8")));
	TestTrue(TEXT("text is UTF-8"), Call(3).Body == TArray<uint8>({ 'c', 'a', 'f', 0xC3, 0xA9 }));
	TestEqualSensitive(TEXT("bytes type"), Call(4).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/octet-stream")));
	TestEqualSensitive(TEXT("form body"), Text(Call(5).Body), FString(TEXT("a+b=c%26d")));
	TestEqualSensitive(TEXT("form type"), Call(5).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/x-www-form-urlencoded")));
	TestEqualSensitive(TEXT("multipart type names the boundary"), Call(6).Headers.Get(TEXT("Content-Type")), FString(TEXT("multipart/form-data; boundary=BB")));
	TestTrue(TEXT("multipart body"), Text(Call(6).Body).StartsWith(TEXT("--BB\r\nContent-Disposition: form-data; name=\"k\"")));
	TestEqualSensitive(TEXT("the request's Content-Type wins"), Call(7).Headers.Get(TEXT("Content-Type")), FString(TEXT("application/vnd.game+json")));
	TestEqual(TEXT("... once"), Call(7).Headers.Num(), 1);

	// Streamed uploads: a file, and multipart with a file part (spec 3.2: "file parts streamed with SetContentFromStream")
	const FString File = FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("builder-upload.bin");
	TArray<uint8> FileBytes;
	FileBytes.Init(7, 5000);
	FFileHelper::SaveArrayToFile(FileBytes, *File);
	TestTrue(TEXT("file"), Run(Rig, Rig.Client.Put(TEXT("/up")).BodyFile(File, TEXT("image/png"))));
	TestTrue(TEXT("multipart with a file"), Run(Rig, Rig.Client.Post(TEXT("/mp")).Body(FVaRestMultipart(TEXT("CC")).AddFile(TEXT("f"), File))));
	TestEqualSensitive(TEXT("a file body streams from the file"), Call(8).BodyFile, File);
	TestTrue(TEXT("... with its bytes"), Rig.Fake->ReadBody(8) == FileBytes);
	TestEqualSensitive(TEXT("... and its type"), Call(8).Headers.Get(TEXT("Content-Type")), FString(TEXT("image/png")));
	TestTrue(TEXT("multipart with a file streams segments"), Call(9).BodySegments.Num() == 3 && Call(9).Body.Num() == 0);
	TestEqual(TEXT("upload sizes reach the probe"), Call(8).Probe->UploadTotal.load(), static_cast<int64>(5000));
	IFileManager::Get().Delete(*File);

	// OpenAPI N9 array styles through the builder (review I7)
	const TArray<FString> Tags = { TEXT("a"), TEXT("b") };
	TestTrue(TEXT("array query"), Run(Rig, Rig.Client.Get(TEXT("/q")).Query(TEXT("tag"), Tags).Query(TEXT("csv"), Tags, EVaRestQueryStyle::Csv)));
	TestEqualSensitive(TEXT("Repeat by default, then Csv"), Call(10).Url, FString(TEXT("https://api.test/q?tag=a&tag=b&csv=a%2Cb")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderMisuseTest, "VaRest.Http.Builder.HeaderInjection",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderMisuseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Injected = MakeOutcome<FVaRestJson>();
	auto BadName = MakeOutcome<FVaRestJson>();
	auto MissingFile = MakeOutcome<FVaRestJson>();
	auto LateNaming = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/h")).Header(TEXT("X-Name"), TEXT("neo\r\nX-Admin: 1")), Injected).Send();
	Observe(Rig.Client.Get(TEXT("/h")).Header(TEXT("Bad Name"), TEXT("v")), BadName).Send();
	Observe(Rig.Client.Put(TEXT("/h")).BodyFile(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("missing.bin")), MissingFile).Send();
	Observe(Rig.Client.Post(TEXT("/h")).Body(FIntPoint(1, 2)).Naming(EVaRestJsonNaming::SnakeCase), LateNaming).Send();
	TestTrue(TEXT("all complete"), PumpUntil([&] { return Injected->IsDone() && BadName->IsDone() && MissingFile->IsDone() && LateNaming->IsDone(); }));
	TestTrue(TEXT("a header value with CRLF is Kind Config (header injection)"), Injected->Error.Kind == EVaRestErrorKind::Config);
	TestTrue(TEXT("an invalid header name is Kind Config"), BadName->Error.Kind == EVaRestErrorKind::Config);
	TestTrue(TEXT("a missing upload file is Kind Config"), MissingFile->Error.Kind == EVaRestErrorKind::Config && MissingFile->Error.Message.Contains(TEXT("missing.bin")));
	TestTrue(TEXT("Naming() after a struct body is Kind Config"), LateNaming->Error.Kind == EVaRestErrorKind::Config && LateNaming->Error.Message.Contains(TEXT("Naming")));
	TestEqual(TEXT("none reached the transport"), Rig.Fake->NumCalls(), 0);

	// Review I3: the multipart boundary goes into Content-Type
	auto BadBoundary = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/h")).Body(FVaRestMultipart(TEXT("b\r\nX-Evil: 1")).AddField(TEXT("a"), TEXT("1"))), BadBoundary).Send();
	TestTrue(TEXT("completes"), Rig.Wait(BadBoundary));
	TestTrue(TEXT("a boundary with CRLF is Kind Config"), BadBoundary->Error.Kind == EVaRestErrorKind::Config && BadBoundary->Error.Message.Contains(TEXT("boundary")));
	TestEqual(TEXT("... and never reached the transport"), Rig.Fake->NumCalls(), 0);

	// Review I3: a PerAttempt stage edits the headers after every builder check; the transport stage checks them again
	AddExpectedErrorPlain(TEXT("holds CR, LF or NUL"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Signed;
	Signed.Client.Use(MakeShared<VaRest::Http::BuilderTestPrivate::FBadSigner, ESPMode::ThreadSafe>(), EVaRestStageSlot::PerAttempt);
	auto Stage = MakeOutcome<FVaRestJson>();
	Observe(Signed.Client.Get(TEXT("/signed")), Stage).Send();
	TestTrue(TEXT("completes"), Signed.Wait(Stage));
	TestTrue(TEXT("a stage's CRLF header value is Kind Config, not retryable"), Stage->Error.Kind == EVaRestErrorKind::Config && !Stage->Error.bRetryable);
	TestEqual(TEXT("... and the transport never saw it"), Signed.Fake->NumCalls(), 0);

	AddExpectedErrorPlain(TEXT("invalid default header"), EAutomationExpectedErrorFlags::Contains, 1);
	Rig.Client.WithHeader(TEXT("X-Bad"), TEXT("a\nb"));
	auto Plain = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/ok")), Plain).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Plain));
	TestFalse(TEXT("an invalid client header is refused, not sent"), Rig.Fake->GetCall(0).Request.Headers.Contains(TEXT("X-Bad")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderGetBodyTest, "VaRest.Http.Builder.GetBodyDropped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderGetBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	VaRest::Http::Testing::FTestRig Rig;
	AddExpectedMessagePlain(TEXT("has a body; it is dropped"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	TestTrue(TEXT("completes"), Run(Rig, Rig.Client.Get(TEXT("/g?token=abc")).Body(FVaRestJson::Object().Set(TEXT("a"), 1))));
	TestEqual(TEXT("spec 3.2: a GET body is dropped (the engine refuses one, HttpRequestCommon.cpp:764-768)"), Rig.Fake->GetCall(0).Request.Body.Num(), 0);
	TestFalse(TEXT("... with its Content-Type"), Rig.Fake->GetCall(0).Request.Headers.Contains(TEXT("Content-Type")));
	TestTrue(TEXT("other verbs keep theirs"), Run(Rig, Rig.Client.Delete(TEXT("/d")).Body(FVaRestJson::Object().Set(TEXT("a"), 1))) && Rig.Fake->GetCall(1).Request.Body.Num() > 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderIdempotentTest, "VaRest.Http.Builder.Idempotent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderIdempotentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::BuilderTestPrivate;
	VaRest::Http::Testing::FTestRig Rig;
	TestTrue(TEXT("idempotent"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Idempotent().Body(FVaRestJson::Object())));
	TestTrue(TEXT("idempotent again"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Idempotent().Body(FVaRestJson::Object())));
	TestTrue(TEXT("plain"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Body(FVaRestJson::Object())));
	TestTrue(TEXT("caller's own key"), Run(Rig, Rig.Client.Post(TEXT("/orders")).Header(TEXT("Idempotency-Key"), TEXT("mine")).Idempotent()));
	const FString First = Rig.Fake->GetCall(0).Request.Headers.Get(TEXT("Idempotency-Key"));
	FGuid Parsed;
	TestTrue(TEXT("Idempotent() adds a UUID key (spec 3.2)"), FGuid::Parse(First, Parsed) && First.Len() == 36);
	TestNotEqual(TEXT("one key per logical request"), Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Idempotency-Key")), First);
	TestFalse(TEXT("no key without Idempotent()"), Rig.Fake->GetCall(2).Request.Headers.Contains(TEXT("Idempotency-Key")));
	TestEqualSensitive(TEXT("a key the caller set is kept"), Rig.Fake->GetCall(3).Request.Headers.Get(TEXT("Idempotency-Key")), FString(TEXT("mine")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpBuilderUnsentTest, "VaRest.Http.Builder.UnsentLogsAnError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpBuilderUnsentTest::RunTest(const FString& Parameters)
{
	VaRest::Http::Testing::FTestRig Rig;
	VaRest::Http::Testing::FLogCapture Capture;
	AddExpectedErrorPlain(TEXT("was built but never sent"), EAutomationExpectedErrorFlags::Contains, 1);
	{
		// [[nodiscard]] already warns at compile time for a temporary (spec 3.2); a named builder dropped unsent logs at run time
		TVaRestRequest<FVaRestJson> Forgotten = Rig.Client.Get(TEXT("/never?token=canary-unsent"));
	}
	{
		TVaRestRequest<FVaRestJson> Sent = Rig.Client.Get(TEXT("/sent"));
		MoveTemp(Sent).Send(); // the moved-from builder logs nothing
	}
	VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0 && Rig.Fake->NumCalls() == 1; });
	TestTrue(TEXT("the error names the URL"), Capture.Contains(TEXT("https://api.test/never?token=***")));
	TestFalse(TEXT("... redacted"), Capture.Contains(TEXT("canary-unsent")));
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http.Builder`
Expected: the build fails: no `TVaRestRequest::Body(const FIntPoint&)`, `BodyFile`, `Idempotent`, `Naming`.

- [ ] **Step 3: The builder** — in `Source/VaRestHttp/Public/VaRestClient.h` (it includes `VaRestUrl.h` since Task 8), replace `TVaRestRequest::Header` with:

```cpp
	/**
	 * Sets a header (replacing one of that name, whatever its case); it wins over the client's and over a body's
	 * Content-Type. A name that is not an RFC 9110 token, or a value holding CR, LF or NUL, fails the request with
	 * Kind=Config and nothing is sent (decision 11: header injection).
	 */
	TVaRestRequest Header(FStringView Name, FStringView Value) &&
	{
		if (!VaRest::Http::IsValidHeaderName(Name))
		{
			Spec->Fail(FString::Printf(TEXT("invalid header name \"%s\""), *FString(Name).ReplaceCharWithEscapedChar()));
		}
		else if (!VaRest::Http::IsValidHeaderValue(Value))
		{
			Spec->Fail(FString::Printf(TEXT("the value of header \"%s\" holds CR, LF or NUL"), *FString(Name)));
		}
		else
		{
			Spec->Headers.Set(Name, Value);
		}
		return MoveTemp(*this);
	}
```

and add after `Body(TConstArrayView<uint8>)`:

```cpp
	/**
	 * A USTRUCT, encoded straight to UTF-8 bytes (spec 2.4) with the naming policy known now: the request's Naming(),
	 * else the client's. Call Naming() first; after a struct body it fails the request.
	 */
	template <typename StructType>
		requires (VaRest::Json::bHasScriptStruct<StructType> && !std::is_same_v<StructType, FVaRestJson>)
	TVaRestRequest Body(const StructType& Struct) &&
	{
		FVaRestJsonConvertOptions Options;
		Options.Naming = Spec->Naming;
		TArray<uint8> Bytes;
		FVaRestJsonError Error;
		if (!FVaRestJsonConvert::StructToUtf8(VaRest::Json::GetScriptStruct<StructType>(), &Struct, Bytes, Options, EVaRestJsonFormat::Condensed, &Error))
		{
			Spec->Fail(FString::Printf(TEXT("the struct body cannot be written: %s"), *Error.Message));
		}
		Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Json, TEXT("application/json"));
		Spec->bStructBodySet = true;
		return MoveTemp(*this);
	}

	/** application/x-www-form-urlencoded */
	TVaRestRequest Body(const FVaRestForm& Form) &&
	{
		Spec->Body = FVaRestRequestBody::FromForm(Form);
		return MoveTemp(*this);
	}

	/** multipart/form-data; file parts stream from disk while uploading */
	TVaRestRequest Body(const FVaRestMultipart& Multipart) &&
	{
		TValueOrError<FVaRestRequestBody, FString> Built = FVaRestRequestBody::FromMultipart(Multipart);
		if (Built.HasError())
		{
			Spec->Fail(Built.StealError());
		}
		else
		{
			Spec->Body = Built.StealValue();
		}
		return MoveTemp(*this);
	}

	/** A file streamed while uploading (IHttpRequest::SetContentAsStreamedFile). Its size is read now; a missing file fails the request. */
	TVaRestRequest BodyFile(FStringView FilePath, FStringView ContentType = TEXTVIEW("application/octet-stream")) &&
	{
		TValueOrError<FVaRestRequestBody, FString> Built = FVaRestRequestBody::FromFile(FilePath, ContentType);
		if (Built.HasError())
		{
			Spec->Fail(Built.StealError());
		}
		else
		{
			Spec->Body = Built.StealValue();
		}
		return MoveTemp(*this);
	}

	/** Spec 3.2: "adds a stable Idempotency-Key: <uuid> across attempts" (M4's retry reads the flag too). A key set with Header is kept. */
	TVaRestRequest Idempotent() &&
	{
		Spec->bIdempotent = true;
		return MoveTemp(*this);
	}

	/** The naming policy of a struct body set after this (spec 2.4: per client, API or request) */
	TVaRestRequest Naming(EVaRestJsonNaming InNaming) &&
	{
		if (Spec->bStructBodySet)
		{
			Spec->Fail(TEXT("Naming() after a struct Body() has no effect: call Naming() first"));
		}
		Spec->Naming = InNaming;
		return MoveTemp(*this);
	}

	/** Spec 2.4: every mismatch while decoding a struct response is an error, not a warning */
	TVaRestRequest StrictDecode(bool bStrict = true) &&
	{
		Spec->bStrictDecode = bStrict;
		return MoveTemp(*this);
	}
```

- [ ] **Step 4: The unsent error and checked client headers** — in `Source/VaRestHttp/Private/VaRestClient.cpp`, add `#include "VaRestUrl.h"`, replace `VaRest::Http::FRequestSpec::~FRequestSpec() = default;` with:

```cpp
VaRest::Http::FRequestSpec::~FRequestSpec()
{
#if !UE_BUILD_SHIPPING
	if (!bSent && ConfigAtBuild.IsValid())
	{
		// Spec 3.2: "In non-Shipping builds a builder destroyed unsent also logs an error naming the URL" -- redacted like every log line
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: a %s request to %s was built but never sent (Send() is missing)"),
			*Verb, *ConfigAtBuild->Redactor->RedactUrl(JoinUrl(ConfigAtBuild->BaseUrl, Path)));
	}
#endif
}
```

and replace `FVaRestClient::WithHeader` with:

```cpp
FVaRestClient& FVaRestClient::WithHeader(FStringView Name, FStringView Value)
{
	if (!VaRest::Http::IsValidHeaderName(Name) || !VaRest::Http::IsValidHeaderValue(Value))
	{
		// Decision 11; no request to fail here, so the header is refused and the error logged
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: invalid default header \"%s\" (not a token, or the value holds CR, LF or NUL); not set"),
			*FString(Name).ReplaceCharWithEscapedChar());
		return *this;
	}
	if (State.IsValid())
	{
		State->EditConfig([Name, Value](VaRest::Http::FClientConfig& Edited) { Edited.DefaultHeaders.Set(Name, Value); });
	}
	return *this;
}
```

- [ ] **Step 5: The GET rule and the idempotency key** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`, add `#include "Misc/Guid.h"`, and in `FClientState::Resolve` replace

```cpp
	if (!Exchange.Body.IsEmpty() && !Exchange.RequestHeaders.Contains(TEXT("Content-Type")))
	{
		Exchange.RequestHeaders.Set(TEXT("Content-Type"), Exchange.Body.ContentType);
	}
```

with

```cpp
	if (!Exchange.Body.IsEmpty() && Exchange.Verb.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		// Spec 3.2: "A body on GET is dropped with a warning (v1 rule)"; the engine would refuse the request
		// (HttpRequestCommon.cpp:764-768: "An HTTP Get request cannot contain a payload")
		UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: GET %s has a body; it is dropped (spec 3.2)"), Exchange.RequestId, *Cfg.Redactor->RedactUrl(Exchange.Url));
		Exchange.Body = FVaRestRequestBody();
	}
	if (!Exchange.Body.IsEmpty() && !Exchange.RequestHeaders.Contains(TEXT("Content-Type")))
	{
		Exchange.RequestHeaders.Set(TEXT("Content-Type"), Exchange.Body.ContentType);
	}
	if (Exchange.bIdempotent && !Exchange.RequestHeaders.Contains(TEXT("Idempotency-Key")))
	{
		// Generated once per logical request, into the request headers every attempt copies: stable across attempts (spec 3.2)
		Exchange.RequestHeaders.Set(TEXT("Idempotency-Key"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.Builder.Bodies`, `.HeaderInjection`, `.GetBodyDropped`, `.Idempotent`, `.UnsentLogsAnError`.

- [ ] **Step 7: Restore-the-bug (header injection)**

In `TVaRestRequest::Header`, temporarily skip the value check (`else if (false)`). Run `Tools/run_tests.sh VaRest.Http.Builder`; expected FAIL in `VaRest.Http.Builder.HeaderInjection` ("a header value with CRLF is Kind Config"; "none reached the transport"). Restore; PASS.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpBuilderTest.cpp
git commit -m "http: request building -- struct, form, multipart and file bodies, header checks, GET rule, idempotency keys, unsent error

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 10: Responses — zero-copy JSON, structs decoded off the game thread, the release rule, size cap, `SaveTo`, `OnHttpThread`, module `VaRestTests`

Spec §3.2: "Response decode by `Content-Type` ... 204/empty → `T` default. Non-JSON when `T` needs JSON → `Decode` error. Raw response bytes are released after a successful decode (memory). They are kept on error for diagnostics, when `T` is raw, or with `.KeepBody()`." and "`SaveTo(path)` (streams the body to a temp file, atomic rename on success), `OnHttpThread()` (opt-in: callbacks on the HTTP/worker thread for heavy consumers)". Spec §2.1: `ParseOwned` "zero-copy when the array has one byte of slack ... which the HTTP layer reserves". Task 8 built the decode path; this task proves its two critical properties with restore-the-bug runs (the body reaches the document without a copy; the parse runs off the game thread), adds the remaining options, and creates the UncookedOnly module `VaRestTests` (decision 15) for tests that need reflected types. The JSON size cap of decision 20 gets its client and request options here.

The rename is atomic where the platform's is: `FUnixPlatformFile::MoveFile` is `rename()` (`UnixPlatformFile.cpp:1092`), which replaces the target; Windows' `MoveFileW` refuses an existing target (`WindowsPlatformFile.cpp:1218`), so there the old file is deleted first (not atomic — `FFileManagerGeneric::Move` does the same, `FileManagerGeneric.cpp:303-323`). A failed or canceled download never touches the destination and deletes its temp file.

**Files:**
- Create: `Source/VaRestTests/VaRestTests.Build.cs`, `Source/VaRestTests/Private/VaRestTestsModule.cpp`, `Source/VaRestTests/Private/VaRestHttpTestTypes.h`
- Modify: `VaRest.uplugin` (module `VaRestTests`)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`KeepBody`, `SaveTo`, `OnHttpThread`, `MaxJsonBytes`; `FVaRestClient::WithMaxJsonBytes`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (`WithMaxJsonBytes`)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`RunDecode`, the temp file in `FTransportStage`, `OnChainDone`, `Complete`)
- Modify: `Source/VaRestHttp/Private/VaRestDecode.h`, `Source/VaRestHttp/Private/VaRestDecode.cpp` (`FinalizeSaveTo`, `LoadFilePrefix`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpDecodeTest.cpp`, `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp`

**Interfaces:**
- Consumes: Task 8's pipeline, `FFinish`, `DecodeJson`, `DecodeStruct`, `ProcessErrorBody`, `RawBodyCap`; `FVaRestFakeTransport::FCall::BodyData/BodyNum`, `Testing::Counters()` (Task 6).
- Produces: `TVaRestRequest<T>::KeepBody() &&`, `SaveTo(FStringView FilePath) && requires (std::is_void_v<T> || std::is_same_v<T, FVaRestResponse>)`, `OnHttpThread() &&`, `MaxJsonBytes(int64) &&`; `FVaRestClient& FVaRestClient::WithMaxJsonBytes(int64)`; private `bool VaRest::Http::FinalizeSaveTo(FFinish&, TFunctionRef<bool()> IsCanceled)`, `void VaRest::Http::LoadFilePrefix(const FString& Path, TArray<uint8>& Out)`; module `VaRestTests` with `FVaRestHttpTestItem { FString Name; int32 Level; }` and `FVaRestHttpTestProfile { int64 Id; FString Name; TArray<FVaRestHttpTestItem> Items; int32 Score = 7; }`.

- [ ] **Step 1: The test module** — `Source/VaRestTests/VaRestTests.Build.cs`

```csharp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

using UnrealBuildTool;

// Tests that need reflected test types, the integration suite and the benchmarks. UncookedOnly: built for the editor
// and uncooked runs only, so its USTRUCTs never reach a game's reflection data (VaRest-rfd, decision 15).
public class VaRestTests : ModuleRules
{
	public VaRestTests(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"VaRestJson",
			"VaRestHttp",
		});
	}
}
```

`Source/VaRestTests/Private/VaRestTestsModule.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, VaRestTests);
```

`Source/VaRestTests/Private/VaRestHttpTestTypes.h`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestHttpTestTypes.generated.h"

USTRUCT()
struct FVaRestHttpTestItem
{
	GENERATED_BODY()

	UPROPERTY()
	FString Name;

	UPROPERTY()
	int32 Level = 0;
};

USTRUCT()
struct FVaRestHttpTestProfile
{
	GENERATED_BODY()

	/** Past 2^53: must stay exact */
	UPROPERTY()
	int64 Id = 0;

	UPROPERTY()
	FString Name;

	UPROPERTY()
	TArray<FVaRestHttpTestItem> Items;

	/** A default the JSON does not set */
	UPROPERTY()
	int32 Score = 7;
};
```

In `VaRest.uplugin`, add a comma after the `VaRestEditor` module object's closing brace and append:

```json
		{
			"Name" : "VaRestTests",
			"Type" : "UncookedOnly",
			"LoadingPhase" : "Default",
			"PlatformAllowList" : [ "Win64", "Mac", "Linux" ]
		}
```

- [ ] **Step 2: Write the failing tests** — `Source/VaRestTests/Private/VaRestHttpStructDecodeTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestHttpTestTypes.h"
#include "VaRestHttpTesting.h"

namespace VaRestTestsStructDecodePrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	const char* const Profile = R"({"id":9007199254740993,"name":"neo","items":[{"name":"a","level":1},{"name":"b","level":"2"}]})";
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStructDecodeTest, "VaRest.Http.Decode.Struct",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStructDecodeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, Profile));
	auto Outcome = MakeOutcome<FVaRestHttpTestProfile>();
	// Review M9: process-wide counters -- let an earlier test's late tasks land before the baseline
	FlushRedactionPipe();
	PumpUntil([] { return false; }, 0.05);
	const int32 OffBefore = Counters().DecodesOffGameThread;
	const int32 OnBefore = Counters().DecodesOnGameThread;
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/me")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("decoded"), Outcome->SuccessCalls == 1 && Outcome->Value.IsSet());
	const FVaRestHttpTestProfile& P = Outcome->Value.GetValue();
	TestEqual(TEXT("int64 past 2^53 stays exact"), P.Id, 9007199254740993ll);
	TestEqualSensitive(TEXT("names match tolerantly (spec 2.4)"), P.Name, FString(TEXT("neo")));
	TestTrue(TEXT("arrays of structs"), P.Items.Num() == 2 && P.Items[1].Level == 2);
	TestEqual(TEXT("a field the JSON lacks keeps its default"), P.Score, 7);
	TestTrue(TEXT("a lenient coercion is a warning with its JSON path (spec 2.4)"),
		Outcome->Response.DecodeWarnings.ContainsByPredicate([](const FVaRestJsonError& W) { return W.Path == TEXT("items[1].level"); }));
	// Spec 1: "Parsing, decoding ... run on UE::Tasks background tasks"
	TestEqual(TEXT("the decode ran off the game thread"), Counters().DecodesOffGameThread - OffBefore, 1);
	TestEqual(TEXT("... and not on it"), Counters().DecodesOnGameThread - OnBefore, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpStructStrictTest, "VaRest.Http.Decode.StructErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpStructStrictTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, Profile));
	Rig.Fake->Enqueue(Json(200, "[1,2]"));
	auto Strict = MakeOutcome<FVaRestHttpTestProfile>();
	auto NotObject = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/me")).StrictDecode(), Strict).Send();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/list")), NotObject).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Strict->IsDone() && NotObject->IsDone(); }));
	TestTrue(TEXT("strict: a mismatch is a Decode error"), Strict->Error.Kind == EVaRestErrorKind::Decode);
	TestEqualSensitive(TEXT("... with the value's JSON path"), Strict->Error.Path, FString(TEXT("items[1].level")));
	TestTrue(TEXT("... and the parsed body"), Strict->Error.Body.IsObject());
	TestFalse(TEXT("... and no value"), Strict->Value.IsSet());
	TestTrue(TEXT("an array for a struct is a Decode error"), NotObject->Error.Kind == EVaRestErrorKind::Decode);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpEmptyBodiesTest, "VaRest.Http.Decode.EmptyBodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpEmptyBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsStructDecodePrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, ""));
	Rig.Fake->Enqueue(Json(200, ""));
	Rig.Fake->Enqueue(Json(204, ""));
	auto JsonEmpty = MakeOutcome<FVaRestJson>();
	auto StructEmpty = MakeOutcome<FVaRestHttpTestProfile>();
	auto NoContent = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Rig.Client.Get(TEXT("/a")), JsonEmpty).Send();
	Observe(Rig.Client.Get<FVaRestHttpTestProfile>(TEXT("/b")), StructEmpty).Send();
	Observe(Rig.Client.Delete<FVaRestHttpTestProfile>(TEXT("/c")), NoContent).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return JsonEmpty->IsDone() && StructEmpty->IsDone() && NoContent->IsDone(); }));
	// Spec 3.2: "204/empty -> T default" -- a 200 with an empty JSON body included
	TestTrue(TEXT("an empty 200 JSON body: success with an invalid handle"), JsonEmpty->SuccessCalls == 1 && JsonEmpty->Value.IsSet() && !JsonEmpty->Value->IsValid());
	TestTrue(TEXT("an empty 200 struct body: success with the defaults"), StructEmpty->SuccessCalls == 1 && StructEmpty->Value.IsSet() && StructEmpty->Value->Score == 7);
	TestTrue(TEXT("204: success with the defaults"), NoContent->SuccessCalls == 1 && NoContent->Value.IsSet());
	return true;
}

#endif
```

`Source/VaRestHttp/Private/Tests/VaRestHttpDecodeTest.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

#include <atomic>

namespace VaRest::Http::DecodeTestPrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FString DownloadDir()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestTests") / TEXT("downloads"));
	}

	int32 CountPartFiles()
	{
		TArray<FString> Parts;
		IFileManager::Get().FindFiles(Parts, *(DownloadDir() / TEXT("*.part")), true, false);
		return Parts.Num();
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpZeroCopyTest, "VaRest.Http.Decode.ZeroCopy",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpZeroCopyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(200, R"({"s":"hello, in situ"})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/z")), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	const FUtf8StringView Parsed = Outcome->Value.IsSet() ? (*Outcome->Value)[TEXT("s")].AsUtf8View() : FUtf8StringView();
	const uint8* Begin = Rig.Fake->GetCall(0).BodyData;
	const uint8* End = Begin + Rig.Fake->GetCall(0).BodyNum;
	const uint8* At = reinterpret_cast<const uint8*>(Parsed.GetData());
	TestEqualSensitive(TEXT("the string"), FString::ConstructFromPtrSize(Parsed.GetData(), Parsed.Len()), FString(TEXT("hello, in situ")));
	// Spec 2.1: ParseOwned steals the body; with its byte of slack the parse is in situ, so the string lies inside the
	// very buffer the transport filled -- no copy anywhere between the HTTP thread and the document
	TestTrue(TEXT("zero-copy: the parsed string points into the transport's buffer"), At >= Begin && At < End);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpContentTypesTest, "VaRest.Http.Decode.ContentTypes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpContentTypesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	FTestRig Rig;
	FVaRestFakeResponse Problem = Json(200, R"({"a":1})");
	Problem.Headers.Set(TEXT("Content-Type"), TEXT("application/problem+json; charset=utf-8"));
	FVaRestFakeResponse Untyped = Json(200, R"({"a":2})");
	Untyped.Headers.Remove(TEXT("Content-Type"));
	Rig.Fake->Enqueue(Problem);
	Rig.Fake->Enqueue(Untyped);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("<html>login</html>"), TEXT("text/html")));
	Rig.Fake->Enqueue(Json(200, R"({"a":)"));
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> O;
	for (int32 i = 0; i < 4; ++i)
	{
		O.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/ct/%d"), i)), O.Last()).Send();
	}
	TestTrue(TEXT("complete"), PumpUntil([&O] { for (const auto& X : O) { if (!X->IsDone()) { return false; } } return true; }));
	TestTrue(TEXT("+json is JSON"), O[0]->Value.IsSet() && (*O[0]->Value)[TEXT("a")].AsInt() == 1);
	TestTrue(TEXT("no Content-Type is parsed (decision 6)"), O[1]->Value.IsSet() && (*O[1]->Value)[TEXT("a")].AsInt() == 2);
	TestTrue(TEXT("text/html for a JSON result is a Decode error (spec 3.2)"), O[2]->Error.Kind == EVaRestErrorKind::Decode && O[2]->Error.Message.Contains(TEXT("text/html")));
	TestTrue(TEXT("... the raw body kept and quoted"), O[2]->Response.Body.Num() > 0 && O[2]->Error.RawBody.Contains(TEXT("login")));
	TestTrue(TEXT("invalid JSON is a Decode error with the parser's message"), O[3]->Error.Kind == EVaRestErrorKind::Decode && O[3]->Error.Message.Contains(TEXT("not valid JSON")));
	TestTrue(TEXT("... and the first bytes as RawBody, from before the in-situ parse"), O[3]->Error.RawBody.Contains(TEXT("{\"a\":")));
	TestEqual(TEXT("decision 26: an in-situ parse error keeps no raw body, only the 4 KiB prefix"), O[3]->Response.Body.Num(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpKeepBodyTest, "VaRest.Http.Decode.KeepBodyAndSizeCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpKeepBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	FTestRig Rig;
	const char* Body = R"({"padding":"0123456789012345678901234567890123456789"})";
	Rig.Fake->SetResponder([Body](const FVaRestTransportRequest& Request)
	{
		return Request.Url.Contains(TEXT("/error")) ? Json(500, Body) : Json(200, Body);
	});
	auto Kept = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/keep")).KeepBody(), Kept).Send();
	TestTrue(TEXT("KeepBody"), Rig.Wait(Kept));
	TestTrue(TEXT("KeepBody keeps the original bytes next to the decoded value (spec 3.2)"),
		Kept->Value.IsSet() && Kept->Response.Body.Num() == FCStringAnsi::Strlen(Body) && FMemory::Memcmp(Kept->Response.Body.GetData(), Body, Kept->Response.Body.Num()) == 0);

	// Decision 20: the arena DOM is about 4.2x its input, so JSON bodies have a size cap
	Rig.Client.WithMaxJsonBytes(16);
	auto Capped = MakeOutcome<FVaRestJson>();
	auto Uncapped = MakeOutcome<FVaRestJson>();
	auto CappedError = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/big")), Capped).Send();
	Observe(Rig.Client.Get(TEXT("/big")).MaxJsonBytes(0), Uncapped).Send();
	Observe(Rig.Client.Get(TEXT("/error")), CappedError).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Capped->IsDone() && Uncapped->IsDone() && CappedError->IsDone(); }));
	TestTrue(TEXT("over the client's cap: a Decode error naming the limit"), Capped->Error.Kind == EVaRestErrorKind::Decode && Capped->Error.Message.Contains(TEXT("MaxJsonBytes")));
	// Reviews M-6, M13: the transport stops the body at the cap (the fake mirrors the HTTP transport's sink)
	TestTrue(TEXT("... the transfer stopped there: nothing kept, not retryable"), Capped->Response.Body.Num() == 0 && !Capped->Error.bRetryable);
	TestEqual(TEXT("... the cap reached the transport"), Rig.Fake->GetCall(1).Request.MaxBodyBytes, static_cast<int64>(16));
	TestTrue(TEXT("a request's MaxJsonBytes(0) lifts it"), Uncapped->Value.IsSet() && Uncapped->Value->IsObject());
	TestTrue(TEXT("an error body over the cap: still the HTTP error, its body not kept"), CappedError->Error.Kind == EVaRestErrorKind::Http
		&& CappedError->Error.HttpStatus == 500 && !CappedError->Error.Body.IsValid() && CappedError->Error.Message.Contains(TEXT("MaxJsonBytes")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOnHttpThreadTest, "VaRest.Http.Decode.OnHttpThread",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOnHttpThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/heavy")).OnHttpThread(), Outcome).Send();
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestFalse(TEXT("OnHttpThread: callbacks off the game thread (decision 4)"), Outcome->bAllOnGameThread.load());
	TestEqual(TEXT("... still exactly once"), Outcome->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOnHttpThreadPinTest, "VaRest.Http.Decode.OnHttpThreadPinsOwner",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOnHttpThreadPinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	// Owner() holds its owner weakly; once the local is cleared, nothing else refers to it, so a GC frees it unless the
	// delivery pins it (decision 28, review C-5)
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	const TWeakObjectPtr<UObject> Weak(Owner);
	struct FState
	{
		std::atomic<bool> bInside{false};
		std::atomic<bool> bRelease{false};
		std::atomic<int32> CompleteCalls{0};
	};
	const TSharedRef<FState, ESPMode::ThreadSafe> State = MakeShared<FState, ESPMode::ThreadSafe>();
	Rig.Client.Get(TEXT("/pinned")).Owner(Owner).OnHttpThread()
		.OnSuccess([State](const FVaRestJson&, const FVaRestResponse&)
		{
			// Where a [this] callback would touch the owner, on a worker: hold here until the game thread has collected
			State->bInside = true;
			const double Until = FPlatformTime::Seconds() + 5.0;
			while (!State->bRelease.load() && FPlatformTime::Seconds() < Until)
			{
				FPlatformProcess::Yield();
			}
		})
		.OnComplete([State](const TVaRestResult<FVaRestJson>&)
		{
			++State->CompleteCalls;
		})
		.Send();
	Owner = nullptr;
	TestTrue(TEXT("the callback runs on a worker"), PumpUntil([State] { return State->bInside.load(); }));
	CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
	TestTrue(TEXT("decision 28: a GC while an OnHttpThread callback runs does not free its owner"), Weak.IsValid());
	State->bRelease = true;
	TestTrue(TEXT("completes"), PumpUntil([State] { return State->CompleteCalls.load() == 1; }));
	// The pin drops on the worker right after the callbacks return (Task 10 Step 6), so poll with a GC each time
	TestTrue(TEXT("... and the pin is dropped after the callbacks (no leak)"), PumpUntil([&Weak]
	{
		CollectGarbage(GARBAGE_COLLECTION_KEEPFLAGS, true);
		return !Weak.IsValid();
	}, 2.0));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToTest, "VaRest.Http.Decode.SaveTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200, { 1, 2, 3, 4, 5 }));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(204, {}));
	const FString File = DownloadDir() / TEXT("pack.bin");
	const FString Empty = DownloadDir() / TEXT("empty.bin");
	auto Saved = MakeOutcome<FVaRestResponse>();
	auto Nothing = MakeOutcome<void>();
	Observe(Rig.Client.Get<FVaRestResponse>(TEXT("/pack")).SaveTo(File), Saved).Send();
	Observe(Rig.Client.Get<void>(TEXT("/empty")).SaveTo(Empty), Nothing).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Saved->IsDone() && Nothing->IsDone(); }));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the body was streamed to the destination"), FFileHelper::LoadFileToArray(OnDisk, *File) && OnDisk == TArray<uint8>({ 1, 2, 3, 4, 5 }));
	TestTrue(TEXT("through a temp file next to it (spec 3.2)"), Rig.Fake->GetCall(0).Request.ResponseFile.StartsWith(File + TEXT(".varest-")));
	TestEqualSensitive(TEXT("the response names the file"), Saved->Value.IsSet() ? Saved->Value->SavedFile : FString(), File);
	TestEqual(TEXT("an empty body still makes the (empty) file"), IFileManager::Get().FileSize(*Empty), static_cast<int64>(0));
	TestEqual(TEXT("no temp file is left"), CountPartFiles(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpSaveToErrorTest, "VaRest.Http.Decode.SaveToKeepsOldFileOnError",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpSaveToErrorTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::DecodeTestPrivate;
	IFileManager::Get().DeleteDirectory(*DownloadDir(), false, true);
	const FString File = DownloadDir() / TEXT("save.dat");
	FFileHelper::SaveStringToFile(TEXT("old"), *File);
	FTestRig Rig;
	Rig.Fake->Enqueue(Json(500, R"({"error":"disk full"})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection, TEXT("ConnectionError")));
	auto Failed = MakeOutcome<void>();
	auto Dropped = MakeOutcome<void>();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(File), Failed).Send();
	Observe(Rig.Client.Get<void>(TEXT("/save")).SaveTo(File), Dropped).Send();
	TestTrue(TEXT("complete"), PumpUntil([&] { return Failed->IsDone() && Dropped->IsDone(); }));
	FString Content;
	FFileHelper::LoadFileToString(Content, *File);
	TestEqualSensitive(TEXT("a failed download never touches the destination"), Content, FString(TEXT("old")));
	TestEqual(TEXT("... and leaves no temp file"), CountPartFiles(), 0);
	TestTrue(TEXT("the error body, read back from the temp file, is parsed"), Failed->Error.Kind == EVaRestErrorKind::Http
		&& Failed->Error.Body[TEXT("error")].AsString().Equals(TEXT("disk full"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("a connection failure is reported"), Dropped->Error.Kind == EVaRestErrorKind::Connection);
	return true;
}

#endif
```

- [ ] **Step 3: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails: no `KeepBody`, `SaveTo`, `OnHttpThread`, `MaxJsonBytes`, `WithMaxJsonBytes`.

- [ ] **Step 4: The options** — in `Source/VaRestHttp/Public/VaRestClient.h`, add `#include "Misc/Paths.h"`, and add to `TVaRestRequest` after `StrictDecode`:

```cpp
	/** Spec 3.2: keep the raw bytes in FVaRestResponse::Body after a successful decode (a copy for JSON: the parse is in situ) */
	TVaRestRequest KeepBody() &&
	{
		Spec->bKeepBody = true;
		return MoveTemp(*this);
	}

	/**
	 * Spec 3.2: streams the body into a temp file next to FilePath and renames it onto FilePath on success; a failed or
	 * canceled download leaves FilePath untouched and deletes the temp file. Only for void and FVaRestResponse results
	 * (decision 9): a body on disk has nothing to decode. The file is written on the HTTP thread, as the engine's own
	 * SetResponseBodyReceiveStream to a file archive does (decision 31); the rename runs on a UE::Tasks worker. A Cancel()
	 * that lands after the rename delivers the success with the saved file: the destination was already replaced, and
	 * success is the truthful outcome (review M-4).
	 */
	TVaRestRequest SaveTo(FStringView FilePath) && requires (std::is_void_v<T> || std::is_same_v<T, FVaRestResponse>)
	{
		if (FilePath.IsEmpty())
		{
			Spec->Fail(TEXT("SaveTo() needs a file path"));
		}
		Spec->SaveTo = FPaths::ConvertRelativePathToFull(FString(FilePath));
		return MoveTemp(*this);
	}

	/**
	 * Spec 3.2: callbacks on a worker thread instead of the game thread, after the game thread has seen the result
	 * (decision 4). With Owner(), the owner is checked on the game thread and then pinned (TStrongObjectPtr) until the
	 * callbacks return, so GC cannot free it under them (decision 28): it may outlive its own destruction by one callback.
	 */
	TVaRestRequest OnHttpThread() &&
	{
		Spec->bOnHttpThread = true;
		return MoveTemp(*this);
	}

	/** The JSON size cap for this request (decision 20); 0 = none */
	TVaRestRequest MaxJsonBytes(int64 Bytes) &&
	{
		Spec->MaxJsonBytes = FMath::Max<int64>(0, Bytes);
		return MoveTemp(*this);
	}
```

and to `FVaRestClient` after `WithClock`:

```cpp
	/** The largest response body parsed as JSON (decision 20); 0 = none. Default: UVaRestHttpSettings::MaxJsonResponseBytes (32 MiB). */
	FVaRestClient& WithMaxJsonBytes(int64 Bytes);
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithMaxJsonBytes(int64 Bytes)
{
	if (State.IsValid())
	{
		State->EditConfig([Bytes](VaRest::Http::FClientConfig& Edited) { Edited.MaxJsonBytes = FMath::Max<int64>(0, Bytes); });
	}
	return *this;
}
```

- [ ] **Step 5: Downloads on disk** — in `Source/VaRestHttp/Private/VaRestDecode.h`, add:

```cpp
	/**
	 * SaveTo's success (spec 3.2): the temp file renamed onto the destination -- rename() on POSIX, which replaces it
	 * atomically (UnixPlatformFile.cpp:1092); Windows' MoveFileW refuses an existing target (WindowsPlatformFile.cpp:1218),
	 * so there the old file is deleted first, as FFileManagerGeneric::Move does (FileManagerGeneric.cpp:303-323). An empty
	 * body never opened the sink, so it becomes an empty file. False, with a Decode error and the temp file deleted, when
	 * the move fails. IsCanceled is read right before the rename (review M-4): when it is true the temp file is deleted,
	 * the destination stays untouched, and false comes back with no error (Complete then delivers Canceled). A cancel
	 * after the rename is too late: the request delivers its success. Background thread.
	 */
	bool FinalizeSaveTo(FFinish& Finish, TFunctionRef<bool()> IsCanceled);

	/** The first RawBodyCap bytes of a file (an error body that went to SaveTo's temp file) */
	void LoadFilePrefix(const FString& Path, TArray<uint8>& Out);
```

In `Source/VaRestHttp/Private/VaRestDecode.cpp`, add the includes `GenericPlatform/GenericPlatformFile.h`, `HAL/PlatformFileManager.h`, `Misc/Paths.h`, and:

```cpp
bool VaRest::Http::FinalizeSaveTo(FFinish& Finish, TFunctionRef<bool()> IsCanceled)
{
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	auto Fail = [&Finish, &PlatformFile](const TCHAR* What)
	{
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("could not %s the download at %s"), What, *Finish.SaveTo);
		PlatformFile.DeleteFile(*Finish.TempFile);
		return false;
	};
	if (!PlatformFile.FileExists(*Finish.TempFile))
	{
		PlatformFile.CreateDirectoryTree(*FPaths::GetPath(Finish.SaveTo));
		TUniquePtr<IFileHandle> EmptyFile(PlatformFile.OpenWrite(*Finish.TempFile));
		if (!EmptyFile.IsValid())
		{
			return Fail(TEXT("create"));
		}
	}
	if (IsCanceled())
	{
		// Review M-4: "a canceled download never touches the destination" -- read as late as it can be
		PlatformFile.DeleteFile(*Finish.TempFile);
		return false;
	}
	if (!PlatformFile.MoveFile(*Finish.SaveTo, *Finish.TempFile))
	{
		const bool bReplaced = PlatformFile.FileExists(*Finish.SaveTo) && PlatformFile.DeleteFile(*Finish.SaveTo) && PlatformFile.MoveFile(*Finish.SaveTo, *Finish.TempFile);
		if (!bReplaced)
		{
			return Fail(TEXT("move"));
		}
	}
	Finish.Response.SavedFile = Finish.SaveTo;
	return true;
}

void VaRest::Http::LoadFilePrefix(const FString& Path, TArray<uint8>& Out)
{
	TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
	if (!File.IsValid())
	{
		return;
	}
	const int64 Count = FMath::Min<int64>(File->Size(), RawBodyCap);
	Out.SetNumUninitialized(static_cast<int32>(Count));
	if (Count > 0 && !File->Read(Out.GetData(), Count))
	{
		Out.Reset();
	}
}
```

- [ ] **Step 6: The pipeline** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`, add `#include "HAL/FileManager.h"`, and:

1. Replace `RunDecode` with:

```cpp
	/**
	 * Background thread (the restore-the-bug runs put it on the game thread): spec 1's "parsing, decoding ... run on
	 * UE::Tasks". Ex is read only through its atomic cancel flag, which any thread may read.
	 */
	void RunDecode(IResultHandler& Handler, FFinish& Finish, const FVaRestExchange& Ex)
	{
#if !UE_BUILD_SHIPPING
		if (IsInGameThread())
		{
			++Testing::Counters().DecodesOnGameThread;
		}
		else
		{
			++Testing::Counters().DecodesOffGameThread;
		}
#endif
		const bool bCanceled = Ex.IsCancelRequested();
		if (Finish.bTimeDecode)
		{
			Finish.DecodeStartAt = Finish.Clock->Now();
		}
		if (!Finish.TempFile.IsEmpty() && (bCanceled || Finish.Error.IsError()))
		{
			// A failed or canceled download: the destination is never touched, the temp file goes
			if (Finish.Error.Kind == EVaRestErrorKind::Http && Finish.bBodyInFile)
			{
				LoadFilePrefix(Finish.TempFile, Finish.Body);
			}
			IFileManager::Get().Delete(*Finish.TempFile, false, false, true);
		}
		if (!Finish.Error.IsError() && !bCanceled)
		{
			// Review M-4: the cancel flag is read again right before the rename
			if (Finish.SaveTo.IsEmpty() || FinalizeSaveTo(Finish, [&Ex] { return Ex.IsCancelRequested(); }))
			{
				Handler.Decode(Finish);
			}
		}
		else if (Finish.Error.Kind == EVaRestErrorKind::Http)
		{
			ProcessErrorBody(Finish);
		}
		if (Finish.bTimeDecode)
		{
			Finish.DecodeEndAt = Finish.Clock->Now();
		}
	}
```

2. In `FTransportStage::Process`, insert before `FVaRestTransportRequest Request;`:

```cpp
			if (!Ex.SaveTo.IsEmpty())
			{
				if (!Ex.TempFile.IsEmpty())
				{
					// An earlier attempt's file; removed off the game thread (spec 1: disk IO on UE::Tasks)
					UE::Tasks::Launch(TEXT("VaRest.DeleteTemp"), [Stale = Ex.TempFile] { IFileManager::Get().Delete(*Stale, false, false, true); });
				}
				// Next to the destination: the same file system, so the final rename is a rename (spec 3.2: "atomic rename on success")
				Ex.TempFile = FString::Printf(TEXT("%s.varest-%llu-%d.part"), *Ex.SaveTo, Ex.RequestId, Ex.Attempt);
			}
```

3. In `FClientState::OnChainDone`, replace

```cpp
	if (Ex.bFinished || Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Finished, or a second outcome while the first is being decoded (a stage that reported twice): ignored (review C-3)
		return;
	}
```

with

```cpp
	if (Ex.bFinished || Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Finished, or a second outcome while the first is being decoded (a stage that reported twice): ignored (review C-3).
		// A finished one's attempt file is removed (a second outcome's file is its decode task's to finish).
		if (Ex.bFinished && !Ex.TempFile.IsEmpty())
		{
			UE::Tasks::Launch(TEXT("VaRest.DeleteTemp"), [Stale = Ex.TempFile] { IFileManager::Get().Delete(*Stale, false, false, true); });
		}
		return;
	}
```

and replace the `bTask` computation with:

```cpp
	const bool bHasBody = Finish.Body.Num() > 0 || Finish.bBodyInFile;
	const bool bTask = !Finish.TempFile.IsEmpty() // a download to finish or clean up: disk IO, never on the game thread
		|| (Finish.Error.IsError() ? Finish.Error.Kind == EVaRestErrorKind::Http && bHasBody : Ex.Handler->DecodesBody() && bHasBody);
```

and in the decode task, call `RunDecode(*Decoder, Pending, *Held);` (Task 8's decode task holds the exchange as `Held`).

4. In `FClientState::Complete`, replace the delivery at its end:

```cpp
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	if (Handler.IsValid())
	{
		Handler->Deliver(Finish);
	}
```

with

```cpp
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	if (!Handler.IsValid())
	{
		return;
	}
	if (Ex.bOnHttpThread)
	{
		// Decision 4: the game thread has seen the result (Completed, Task 15); the callbacks run on a worker.
		// Decision 28 (review C-5): the owner, live here on the game thread, is pinned until the callbacks return -- a
		// worker may not ask a TWeakObjectPtr (WeakObjectPtr.h:290-300), and GC could free the owner under a [this]
		// callback. TStrongObjectPtr takes its reference here (StrongObjectPtrTemplates.h:57-61 -> Reset, :167 AddRef, with
		// the incremental-GC barrier of FUObjectItem::AddRef, UObjectArray.h:347-366) and drops it on the worker, which
		// 5.8.1 allows: ~TStrongObjectPtr -> Reset -> ReleaseUObject (StrongObjectPtrTemplates.h:47-50, :122-131;
		// StrongObjectPtr.cpp:10-16) is an atomic decrement (UObjectArray.h:375-386) whose root-dirtying takes a try-lock
		// or queues lock-free, from any thread (GarbageCollection.cpp:701-719).
		TStrongObjectPtr<const UObject> Pin;
		if (Ex.bHasOwner)
		{
			Pin.Reset(Ex.Owner.Get());
		}
		UE::Tasks::Launch(TEXT("VaRest.DeliverOffGameThread"), [Delivered = MoveTemp(Handler), Done = MoveTemp(Finish), Kept = MoveTemp(Pin)]() mutable
		{
			Delivered->Deliver(Done);
			Kept.Reset(); // after the callbacks, on this worker: legal, see above
		});
		return;
	}
	Handler->Deliver(Finish);
```

and add `#include "UObject/StrongObjectPtr.h"` to `VaRestPipeline.cpp`.

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`: the seven `VaRest.Http.Decode.*` tests in `VaRestHttp` and the three in `VaRestTests` (`Struct`, `StructErrors`, `EmptyBodies`).

- [ ] **Step 8: Restore-the-bug (zero-copy slack, spec 2.1)**

In `FClientState::OnChainDone`, temporarily replace `Finish.Body = MoveTemp(Outcome.Body);` with `Finish.Body = Outcome.Body;` (a copy, which also loses the slack). Run `Tools/run_tests.sh VaRest.Http.Decode`; expected FAIL in `VaRest.Http.Decode.ZeroCopy` ("the parsed string points into the transport's buffer"). Restore; PASS. Report both runs.

- [ ] **Step 9: Restore-the-bug (decode off the game thread, spec 1)**

In `OnChainDone`, temporarily make the decode run inline: replace the `UE::Tasks::Launch(TEXT("VaRest.Decode"), ...)` block with `RunDecode(*Handler, Finish, Ex); Ex.Handler = MoveTemp(Handler); Complete(Exchange, MoveTemp(Finish));`. Run `Tools/run_tests.sh VaRest.Http.Decode`; expected FAIL in `VaRest.Http.Decode.Struct` ("the decode ran off the game thread"). Restore; PASS. Report both runs.

- [ ] **Step 9b: Restore-the-bug (the owner pin, decision 28, review C-5)**

In `Complete`'s `bOnHttpThread` branch, temporarily delete `Pin.Reset(Ex.Owner.Get());`. Run `Tools/run_tests.sh VaRest.Http.Decode.OnHttpThreadPinsOwner`; expected FAIL ("decision 28: a GC while an OnHttpThread callback runs does not free its owner"): nothing holds the owner while the worker is inside its callback, so the game thread's GC frees it. Restore; PASS. Report both runs.

- [ ] **Step 10: Commit**

```bash
git add VaRest.uplugin Source/VaRestTests Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestDecode.h Source/VaRestHttp/Private/VaRestDecode.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpDecodeTest.cpp
git commit -m "http: responses -- zero-copy JSON and struct decode off the game thread, release rule, JSON size cap, SaveTo, OnHttpThread; module VaRestTests

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 11: Cancellation — `Cancel()`, the owner, `CancelAll`, engine shutdown (spec §3.2)

Spec §3.2: "`.Owner(this)` // weak: owner gone -> no callbacks, no further retries" and "`FVaRestRequestHandle`: `Cancel()`, `IsActive()`, `GetId()`. Weak — it does not keep the request alive." Spec §5.3 lists *Cancel All (by tag)* for M5's Blueprint client; its C++ form lands here. The rules (decisions 2 and 3):

- an explicit `Cancel()` delivers `Kind=Canceled` to `OnError`/`OnComplete`, never inside `Cancel()`;
- after `Cancel()` returns on the game thread no `OnSuccess` runs, even when the answer was already on its way;
- an owner that is gone delivers nothing, and no attempt starts for it. It is checked on entry, before each dispatch, when an attempt ends, and at completion, and it wins over an earlier `Cancel()` (a cancel in the owner's `EndPlay` followed by its destruction delivers nothing either, review C-2); a cancel that lands after `SaveTo`'s rename delivers the success (review M-4);
- engine shutdown (`FCoreDelegates::OnEnginePreExit`, `CoreDelegates.h:261-262`, which fires before core modules shut down) cancels every in-flight request and delivers nothing, and a request sent after it completes silently without reaching the HTTP module, which is about to unload (`LaunchEngineLoop.cpp:5182`; review M-14). `ShutdownModule` is not used for this: it does not run in an automation session (CLAUDE.md).

`Cancel()` from any thread sets the flag at once (a decode in flight sees it) and does the rest on the game thread.

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`FVaRestRequestHandle::Cancel`, `TVaRestRequest::Owner`, `FVaRestClient::CancelAll`)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h` (`Cancel`, `CancelAll`, `CancelForShutdown`)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Cancel`, `CancelAll`, owner checks in `Enter`, `FTransportStage`, `OnAttemptDone`, `Complete`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (handle `Cancel`, `CancelAll`)
- Modify: `Source/VaRestHttp/Private/VaRestHttpModule.cpp` (the pre-exit hook)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpCancelTest.cpp`

**Interfaces:**
- Consumes: Task 8's exchange phases, `bFinished`, `ApiDepth`, `bCancelRequested`, `CancelReason`, `TransportCall`, `FApiScope`.
- Produces: `void FVaRestRequestHandle::Cancel() const` (any thread); `TVaRestRequest<T>::Owner(const UObject* InOwner) &&`; `void FVaRestClient::CancelAll(FName Tag = NAME_None)` (game thread); private `FClientState::Cancel(const FVaRestExchangeRef&, EVaRestCancelReason)`, `FClientState::CancelAll(FName Tag, EVaRestCancelReason)`, `static FClientState::CancelForShutdown()`, `static FClientState::IsShuttingDown()`, `static FClientState::ResetShutdownInternal()` (tests); `PipelinePrivate::IsOwnerGone(const FVaRestExchange&)`, `PipelinePrivate::MarkOwnerGone(FVaRestExchange&)`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpCancelTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Tasks/Task.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::CancelTestPrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** An Outer stage that keeps the exchange and resumes only when told */
	class FHoldingStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Hold"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Resume = [Exchange, Next, Done = MoveTemp(OnOutcome)]() mutable { Next.Proceed(Exchange, MoveTemp(Done)); };
		}
		TUniqueFunction<void()> Resume;
	};

	/** An Outer stage that holds the outcome on its way up until the test forwards it (review C-3) */
	class FOutcomeHoldingStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("HoldOutcome"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Next.Proceed(Exchange, [this, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				Kept = MoveTemp(Outcome);
				Forward = [this, Callback = MoveTemp(Done)]() mutable { Callback(MoveTemp(Kept)); };
				bHolding = true;
			});
		}
		FVaRestAttemptOutcome Kept;
		TUniqueFunction<void()> Forward;
		bool bHolding = false;
	};

	UObject* NewOwner()
	{
		// Any UObject will do; a transient settings instance needs no extra class
		return NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	}

	/** Review I-6: run nothing on the game thread until the worker is done; the worker never needs the game thread */
	void SpinUntil(const TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe>& Flag)
	{
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (!Flag->load() && FPlatformTime::Seconds() < Until)
		{
			FPlatformProcess::Yield();
		}
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelInTransportTest, "VaRest.Http.Cancel.InTransport",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelInTransportTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/slow")), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	TestTrue(TEXT("active while in flight"), Handle.IsActive());
	Handle.Cancel();
	TestEqual(TEXT("decision 3: nothing is delivered inside Cancel()"), Outcome->CompleteCalls.load(), 0);
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestTrue(TEXT("decision 2: an explicit Cancel delivers Kind Canceled"), Outcome->ErrorCalls == 1 && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... never a success"), Outcome->SuccessCalls.load(), 0);
	TestEqual(TEXT("the transport call was canceled"), Rig.Fake->NumCanceled(), 1);
	TestFalse(TEXT("inactive afterwards"), Handle.IsActive());
	Handle.Cancel();
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("a Cancel after the end changes nothing"), Outcome->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelRacesTest, "VaRest.Http.Cancel.Races",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelRacesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;

	// The answer is already on its way (queued for the game thread) when Cancel runs
	FTestRig Racing;
	Racing.Fake->bCompleteOnWorker = false;
	Racing.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Raced = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle RacedHandle = Observe(Racing.Client.Get(TEXT("/r")), Raced).Send();
	Racing.Fake->Complete(0, Json(200, R"({"ok":true})"));
	RacedHandle.Cancel();
	TestTrue(TEXT("completes"), Racing.Wait(Raced));
	TestTrue(TEXT("after Cancel() returns, no OnSuccess runs: the answer that raced it is replaced by Canceled"),
		Raced->SuccessCalls == 0 && Raced->Error.Kind == EVaRestErrorKind::Canceled);

	// Canceled from a worker before it reached the game thread: no transport call at all
	FTestRig Early;
	auto Before = MakeOutcome<FVaRestJson>();
	FVaRestClient Client = Early.Client;
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bCanceled = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.CancelEarly"), [Client, Before, bCanceled]
	{
		Observe(Client.Get(TEXT("/never")), Before).Send().Cancel();
		*bCanceled = true;
	});
	SpinUntil(bCanceled); // review I-6: a pump here could run Enter between Send() and Cancel() on the worker
	TestTrue(TEXT("the worker ran"), bCanceled->load());
	TestTrue(TEXT("completes"), Early.Wait(Before));
	TestTrue(TEXT("canceled before entry: Kind Canceled"), Before->Error.Kind == EVaRestErrorKind::Canceled);
	TestEqual(TEXT("... and never sent"), Early.Fake->NumCalls(), 0);

	// Held by a stage that never resumes by itself
	FTestRig Held;
	TSharedRef<FHoldingStage, ESPMode::ThreadSafe> Stage = MakeShared<FHoldingStage, ESPMode::ThreadSafe>();
	Held.Client.Use(Stage, EVaRestStageSlot::Outer);
	auto Parked = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle ParkedHandle = Observe(Held.Client.Get(TEXT("/parked")), Parked).Send();
	ParkedHandle.Cancel();
	TestTrue(TEXT("completes while the stage holds it"), Held.Wait(Parked));
	TestTrue(TEXT("Canceled"), Parked->Error.Kind == EVaRestErrorKind::Canceled);
	{
		// Review M8: moved out first, so the stage keeps no exchange (and no client state) after it ran
		TUniqueFunction<void()> Run = MoveTemp(Stage->Resume);
		Run();
	}
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("a stage that resumes later reaches no transport (Proceed reports Canceled)"), Held.Fake->NumCalls(), 0);
	TestEqual(TEXT("... and nothing is delivered twice"), Parked->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelHeldOutcomeTest, "VaRest.Http.Cancel.WhileOutcomeHeld",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelHeldOutcomeTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	// Review C-3: Cancel() defers its Complete (decision 3); before that task runs, a stage forwards the 200 it held, and
	// the decode task takes the handler. The deferred Complete must leave the exchange to the decode task.
	FTestRig Rig;
	TSharedRef<FOutcomeHoldingStage, ESPMode::ThreadSafe> Stage = MakeShared<FOutcomeHoldingStage, ESPMode::ThreadSafe>();
	Rig.Client.Use(Stage, EVaRestStageSlot::Outer);
	Rig.Fake->Enqueue(Json(200, R"({"ok":true})"));
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/held")), Outcome).Send();
	TestTrue(TEXT("the stage holds the outcome"), PumpUntil([&Stage] { return Stage->bHolding; }));
	Handle.Cancel();
	{
		TUniqueFunction<void()> Run = MoveTemp(Stage->Forward); // synchronously, before any pump
		Run();
	}
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	PumpUntil([] { return false; }, 0.05);
	TestTrue(TEXT("exactly one OnError(Canceled) and one OnComplete"), Outcome->ErrorCalls == 1 && Outcome->CompleteCalls == 1
		&& Outcome->SuccessCalls == 0 && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelThenOwnerGoneTest, "VaRest.Http.Cancel.CancelThenOwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelThenOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	// Review C-2: the usual "cancel my requests in EndPlay", then the owner is destroyed before the Canceled is delivered
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewOwner();
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), Outcome).Send();
	TestTrue(TEXT("dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; }));
	Handle.Cancel();
	Owner->MarkAsGarbage(); // before any pump
	PumpUntil([&Handle] { return !Handle.IsActive(); });
	PumpUntil([] { return false; }, 0.1);
	TestEqual(TEXT("owner gone wins over the earlier Cancel(): no callbacks"), Outcome->SuccessCalls + Outcome->ErrorCalls + Outcome->CompleteCalls, 0);
	TestEqual(TEXT("... and the request is over"), Rig.Client.GetInFlightCount(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpOwnerGoneTest, "VaRest.Http.Cancel.OwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewOwner();
	auto InFlight = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), InFlight).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	Owner->MarkAsGarbage(); // TWeakObjectPtr::IsValid is false for garbage (WeakObjectPtr.h:290)
	Rig.Fake->Complete(0, Json(200, R"({"late":1})"));
	PumpUntil([&Handle] { return !Handle.IsActive(); });
	PumpUntil([] { return false; }, 0.1);
	TestEqual(TEXT("spec 3.2: owner gone -> no callbacks"), InFlight->SuccessCalls + InFlight->ErrorCalls + InFlight->CompleteCalls, 0);
	TestFalse(TEXT("... and the request is over"), Handle.IsActive());
	TestEqual(TEXT("... and nothing is left in flight"), Rig.Client.GetInFlightCount(), 0);

	UObject* Gone = NewOwner();
	Gone->MarkAsGarbage();
	auto Never = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle NeverHandle = Observe(Rig.Client.Get(TEXT("/gone")).Owner(Gone), Never).Send();
	PumpUntil([&NeverHandle] { return !NeverHandle.IsActive(); });
	TestEqual(TEXT("an owner gone before entry: nothing sent"), Rig.Fake->NumCalls(), 1);
	TestEqual(TEXT("... and nothing delivered"), Never->CompleteCalls.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelAllTest, "VaRest.Http.Cancel.AllAndShutdown",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelAllTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A1 = MakeOutcome<FVaRestJson>();
	auto A2 = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a1")).Tag(TEXT("A")), A1).Send();
	Observe(Rig.Client.Get(TEXT("/a2")).Tag(TEXT("A")), A2).Send();
	Observe(Rig.Client.Get(TEXT("/b")).Tag(TEXT("B")), B).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	Rig.Client.CancelAll(TEXT("A"));
	TestTrue(TEXT("the tagged ones complete"), PumpUntil([&] { return A1->IsDone() && A2->IsDone(); }));
	TestTrue(TEXT("as Canceled"), A1->Error.Kind == EVaRestErrorKind::Canceled && A2->Error.Kind == EVaRestErrorKind::Canceled);
	TestFalse(TEXT("the other tag is untouched"), B->IsDone());
	TestEqual(TEXT("... still in flight"), Rig.Client.GetInFlightCount(), 1);

	// Engine exit: every live client's requests stop, and no callback runs into a world that is going away
	VaRest::Http::FClientState::CancelForShutdown();
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("shutdown cancels the transport call"), Rig.Fake->NumCanceled(), 3);
	TestEqual(TEXT("... and delivers nothing"), B->CompleteCalls.load(), 0);

	// Review M-14: after OnEnginePreExit the HTTP module is about to unload; a late Send() never reaches it
	auto Late = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle LateHandle = Observe(Rig.Client.Get(TEXT("/late")), Late).Send();
	PumpUntil([&LateHandle] { return !LateHandle.IsActive(); });
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("a request sent during shutdown is not sent"), Rig.Fake->NumCalls(), 3);
	TestEqual(TEXT("... and delivers nothing"), Late->CompleteCalls.load(), 0);
	VaRest::Http::FClientState::ResetShutdownInternal(); // the editor goes on: later tests send again
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpCancelFromCallbackTest, "VaRest.Http.Cancel.FromCallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpCancelFromCallbackTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::CancelTestPrivate;
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/c")) ? FVaRestFakeResponse::Manual() : Json(200, "{}");
	});
	auto C = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CHandle = Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	auto B = MakeOutcome<FVaRestJson>();
	TSharedRef<bool, ESPMode::ThreadSafe> bADone = MakeShared<bool, ESPMode::ThreadSafe>(false);
	FVaRestClient Client = Rig.Client;
	Rig.Client.Get(TEXT("/a")).OnSuccess([Client, B, CHandle, bADone](const FVaRestJson&)
	{
		// From inside a callback: send another request and cancel a third one (Review Focus)
		Observe(Client.Get(TEXT("/b")), B).Send();
		CHandle.Cancel();
		*bADone = true;
	}).Send();
	TestTrue(TEXT("all three finish"), PumpUntil([&] { return *bADone && B->IsDone() && C->IsDone(); }));
	TestTrue(TEXT("the request sent from the callback succeeds"), B->SuccessCalls == 1);
	TestTrue(TEXT("the one canceled from the callback is Canceled"), C->Error.Kind == EVaRestErrorKind::Canceled);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http.Cancel`
Expected: the build fails: no `FVaRestRequestHandle::Cancel`, `TVaRestRequest::Owner`, `FVaRestClient::CancelAll`, `FClientState::CancelForShutdown`.

- [ ] **Step 3: The public surface** — in `Source/VaRestHttp/Public/VaRestClient.h`:

In `FVaRestRequestHandle`, after `IsActive`:

```cpp
	/**
	 * Any thread. The callbacks then get Kind=Canceled (decision 2), never inside Cancel() (decision 3); after it returns
	 * on the game thread no OnSuccess runs. No-op on a finished request.
	 */
	void Cancel() const;
```

In `TVaRestRequest`, after `Priority`:

```cpp
	/** Spec 3.2: weak. Once the owner is gone there are no callbacks and no further attempts. */
	TVaRestRequest Owner(const UObject* InOwner) &&
	{
		Spec->Owner = InOwner;
		Spec->bHasOwner = InOwner != nullptr;
		return MoveTemp(*this);
	}
```

In `FVaRestClient`, after `GetInFlightCount`:

```cpp
	/** Game thread: cancels every in-flight request with this tag (all of them for NAME_None), like Cancel() on each */
	void CancelAll(FName Tag = NAME_None);
```

- [ ] **Step 4: The state's half** — in `Source/VaRestHttp/Private/VaRestClientState.h`, add to `FClientState`'s public section:

```cpp
		/**
		 * Game thread. The first reason wins, except that a silent one (OwnerGone, Shutdown) overrides User: callbacks
		 * must not run into an owner that is gone or at engine exit, whatever canceled first (review C-2).
		 */
		void Cancel(const FVaRestExchangeRef& Exchange, EVaRestCancelReason Reason);
		/** Game thread: every in-flight exchange with Tag (all for NAME_None) */
		void CancelAll(FName Tag, EVaRestCancelReason Reason);
		/** FCoreDelegates::OnEnginePreExit: every live client's requests, without callbacks; later sends complete silently (review M-14) */
		static void CancelForShutdown();
		/** Any thread */
		static bool IsShuttingDown();
#if WITH_DEV_AUTOMATION_TESTS
		/** Tests: the editor goes on after a simulated shutdown */
		static void ResetShutdownInternal();
#endif
```

In `Source/VaRestHttp/Private/VaRestPipeline.cpp`:

1. Add to `namespace VaRest::Http::PipelinePrivate`, before `RunDecode`:

```cpp
	bool IsOwnerGone(const FVaRestExchange& Ex)
	{
		// IsValid() is false for an object marked as garbage, not only for one collected (WeakObjectPtr.h:290)
		return Ex.bHasOwner && !Ex.Owner.IsValid();
	}

	void MarkOwnerGone(FVaRestExchange& Ex)
	{
		// Silence wins over an earlier User cancel (review C-2); Shutdown is silent too, and stays
		if (Ex.CancelReason != EVaRestCancelReason::Shutdown)
		{
			Ex.CancelReason = EVaRestCancelReason::OwnerGone;
		}
		Ex.bCancelRequested = true;
	}
```

2. At the top of `FTransportStage::Process`, after `FVaRestExchange& Ex = *Exchange;`:

```cpp
			if (IsOwnerGone(Ex))
			{
				// Spec 3.2: "owner gone -> no callbacks, no further retries": no attempt starts for it
				MarkOwnerGone(Ex);
				OnOutcome(FVaRestAttemptOutcome::Canceled());
				return;
			}
```

3. In `FClientState::Enter`, after `InFlight.Add(Ex.RequestId, Exchange);`:

```cpp
	if (IsShuttingDown())
	{
		// Review M-14: after OnEnginePreExit; the HTTP module unloads next (LaunchEngineLoop.cpp:5182)
		Ex.CancelReason = EVaRestCancelReason::Shutdown;
		Ex.bCancelRequested = true;
	}
	if (Ex.IsCancelRequested() || IsOwnerGone(Ex))
	{
		// Canceled before it reached the game thread, its owner is already gone, or the engine is exiting: no transport call
		Complete(Exchange, MakeFinish(Ex));
		return;
	}
```

4. In `FClientState::OnAttemptDone`, after `Ex.TransportCall.Reset();`:

```cpp
	if (IsOwnerGone(Ex))
	{
		// Seen as canceled by every stage the outcome passes on its way up (M4's retry stops here)
		MarkOwnerGone(Ex);
	}
```

5. In `FClientState::Complete`, after the `if (Ex.ApiDepth > 0) { ... }` block and before `Ex.bFinished = true;`:

```cpp
	if (IsOwnerGone(Ex))
	{
		// Spec 3.2: owner gone -> no callbacks, whatever canceled it first (a Cancel() in the owner's EndPlay, review C-2)
		MarkOwnerGone(Ex);
	}
	if (Ex.IsCancelRequested() && Ex.CancelReason == EVaRestCancelReason::None)
	{
		Ex.CancelReason = EVaRestCancelReason::User; // the flag set off the game thread before the reason
	}
	if (Ex.IsCancelRequested() && Finish.Response.SavedFile.IsEmpty())
	{
		// After Cancel() nothing but Canceled is delivered, even an answer that raced it (decision 2). A SaveTo whose
		// rename already replaced the destination delivers its success: the truthful outcome (review M-4).
		FFinish Canceled = MakeFinish(Ex);
		Canceled.Error.Kind = EVaRestErrorKind::Canceled;
		Canceled.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Canceled, 0);
		Finish = MoveTemp(Canceled);
	}
```

and replace

```cpp
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	if (!Handler.IsValid())
	{
		return;
	}
```

with

```cpp
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	// Spec 3.2: owner gone -> no callbacks; nor at engine exit (decision 2)
	const bool bSilent = Ex.CancelReason == EVaRestCancelReason::OwnerGone || Ex.CancelReason == EVaRestCancelReason::Shutdown;
	if (!Handler.IsValid() || bSilent)
	{
		return;
	}
```

6. Append the three functions:

```cpp
void VaRest::Http::FClientState::Cancel(const FVaRestExchangeRef& Exchange, EVaRestCancelReason Reason)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FVaRestExchange& Ex = *Exchange;
	if (Ex.bFinished)
	{
		return;
	}
	// Review C-2: the first reason wins, but silence (OwnerGone, Shutdown) overrides User
	if (Ex.CancelReason == EVaRestCancelReason::None || Reason == EVaRestCancelReason::Shutdown || Reason == EVaRestCancelReason::OwnerGone)
	{
		Ex.CancelReason = Reason;
	}
	Ex.bCancelRequested = true;
	if (Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Created: Enter sees the flag. Decoding: Complete replaces the decoded result when it comes back.
		return;
	}
	FApiScope Scope(Ex);
	if (Ex.TransportCall.IsValid())
	{
		// The transport answers Canceled through the normal path, which releases what the attempt holds
		Ex.TransportCall->Cancel();
		return;
	}
	// Held by a custom stage, or its answer is queued for the game thread: finish now; what arrives later is ignored
	Complete(Exchange, MakeFinish(Ex));
}

void VaRest::Http::FClientState::CancelAll(FName Tag, EVaRestCancelReason Reason)
{
	check(IsInGameThread());
	TArray<FVaRestExchangeRef> Targets;
	for (const TPair<uint64, FVaRestExchangeRef>& Pair : InFlight)
	{
		if (Tag.IsNone() || Pair.Value->GetTag() == Tag)
		{
			Targets.Add(Pair.Value);
		}
	}
	for (const FVaRestExchangeRef& Target : Targets)
	{
		Cancel(Target, Reason);
	}
}

namespace VaRest::Http::PipelinePrivate
{
	/** Trivially destructible: safe as a static (review I-4) */
	std::atomic<bool> bShuttingDown{ false };
}

void VaRest::Http::FClientState::CancelForShutdown()
{
	PipelinePrivate::bShuttingDown = true;
	ForEachLive([](FClientState& Client) { Client.CancelAll(NAME_None, EVaRestCancelReason::Shutdown); });
}

bool VaRest::Http::FClientState::IsShuttingDown()
{
	return PipelinePrivate::bShuttingDown.load();
}

#if WITH_DEV_AUTOMATION_TESTS
void VaRest::Http::FClientState::ResetShutdownInternal()
{
	PipelinePrivate::bShuttingDown = false;
}
#endif
```

- [ ] **Step 5: The handle, the client, the hook** — in `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
void FVaRestRequestHandle::Cancel() const
{
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Exchange.Pin();
	if (!Pinned.IsValid() || Pinned->IsDone())
	{
		return;
	}
	if (IsInGameThread())
	{
		Pinned->State->Cancel(Pinned.ToSharedRef(), EVaRestCancelReason::User);
		return;
	}
	// Any thread: the flag now (a decode in flight sees it), the rest where the control plane lives (spec 1). The pin is
	// moved into the task, so this thread never drops the last reference (review I-1).
	Pinned->bCancelRequested = true;
	UE::Tasks::Launch(TEXT("VaRest.Cancel"), [Target = MoveTemp(Pinned)] { Target->State->Cancel(Target.ToSharedRef(), EVaRestCancelReason::User); },
		UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}

void FVaRestClient::CancelAll(FName Tag)
{
	check(IsInGameThread());
	if (State.IsValid())
	{
		State->CancelAll(Tag, EVaRestCancelReason::User);
	}
}
```

In `Source/VaRestHttp/Private/VaRestHttpModule.cpp`, add `#include "Misc/CoreDelegates.h"` and `#include "VaRestClientState.h"`, a member `FDelegateHandle PreExitHandle;`, and make the bodies:

```cpp
	virtual void StartupModule() override
	{
		VaRest::Http::StartRedactionPipe();
		// Before core modules shut down (CoreDelegates.h:261-262); ShutdownModule does not run in an automation session
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&VaRest::Http::FClientState::CancelForShutdown);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		VaRest::Http::StopRedactionPipe();
	}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.Cancel.InTransport`, `.Races`, `.WhileOutcomeHeld`, `.CancelThenOwnerGone`, `.OwnerGone`, `.AllAndShutdown`, `.FromCallback`.

- [ ] **Step 7: Restore-the-bug (owner gone)**

In `FClientState::Complete`, temporarily set `const bool bSilent = false;`. Run `Tools/run_tests.sh VaRest.Http.Cancel`; expected FAIL in `VaRest.Http.Cancel.OwnerGone` ("owner gone -> no callbacks") and `.AllAndShutdown` ("delivers nothing"). Restore; PASS. Report both runs.

- [ ] **Step 8: Restore-the-bug (an answer racing Cancel)**

In `FClientState::Complete`, temporarily remove the `if (Ex.IsCancelRequested()) { ... Finish = MoveTemp(Canceled); }` block. Run `Tools/run_tests.sh VaRest.Http.Cancel`; expected FAIL in `VaRest.Http.Cancel.Races` ("no OnSuccess runs"). Restore; PASS. Report both runs.

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestPipeline.cpp \
  Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestHttpModule.cpp Source/VaRestHttp/Private/Tests/VaRestHttpCancelTest.cpp
git commit -m "http: cancellation -- Cancel from any thread, owner gone at every checkpoint, CancelAll by tag, engine shutdown

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 12: The concurrency limiter (spec §4.6)

Spec §4.6: "Per-client `MaxConcurrent` (default 16) with a priority queue (Highest → Low, FIFO within a priority). It sits next to the transport, so queued retries wait their turn too." It is a per-attempt stage just before the transport (the log stage of Task 14 goes before it). A slot is taken when the attempt is granted and released when its outcome comes back, before the outcome travels further up, so a stage that re-issues waits in the queue like anyone else. A queued exchange can be canceled out of the queue. An owner that disappeared while queued is caught by the transport stage when the slot is granted, which answers `Canceled` at once and so releases the slot. `LimiterWait` (IN-2) is recorded on the exchange for Task 15.

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestLimiter.h`, `Source/VaRestHttp/Private/VaRestLimiter.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h` (`FLimiter Limiter`, `GetLimiter()`, `MakeLimiterStage()`), `Source/VaRestHttp/Private/VaRestClientState.cpp` (`Publish`: the stage and the capacity)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`FLimiterStage`; `Cancel` takes an exchange out of the queue)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (`WithMaxConcurrent`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpLimiterTest.cpp`

**Interfaces:**
- Consumes: Task 8's chain, `FVaRestStageNext`, `FVaRestExchange::bInLimiterQueue/LimiterWait/Priority`; Task 11's `FClientState::Cancel`.
- Produces: private `VaRest::Http::FLimiter` (`SetCapacity(int32)` (no grants), `Pump(double Now)`, `Acquire(uint64 RequestId, EVaRestPriority, double Now, FGrant&&, FAbort&&)`, `Release(double Now)`, `bool Abort(uint64 RequestId)`, `NumActive()`, `NumQueued()`, `GetCapacity()`; `FGrant = TUniqueFunction<void(double WaitedSeconds)>`, `FAbort = TUniqueFunction<void()>`); `FClientState::GetLimiter()`; `MakeLimiterStage()`; public `FVaRestClient& FVaRestClient::WithMaxConcurrent(int32)`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpLimiterTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::LimiterTestPrivate
{
	FVaRestFakeResponse Ok()
	{
		return FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}")));
	}

	/** Completes the oldest call still open; false when none is */
	bool CompleteOldest(VaRest::Http::Testing::FTestRig& Rig)
	{
		for (int32 i = 0; i < Rig.Fake->NumCalls(); ++i)
		{
			if (!Rig.Fake->GetCall(i).bCompleted)
			{
				Rig.Fake->Complete(i, Ok());
				return true;
			}
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterMaxTest, "VaRest.Http.Limiter.MaxConcurrent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterMaxTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(2);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> O;
	for (int32 i = 0; i < 5; ++i)
	{
		O.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/q/%d"), i)), O.Last()).Send();
	}
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("spec 4.6: at most MaxConcurrent reach the transport"), Rig.Fake->NumCalls(), 2);
	CompleteOldest(Rig);
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	TestEqual(TEXT("a finished attempt frees a slot for the next"), Rig.Fake->NumCalls(), 3);
	auto AllDone = [&O] { for (const auto& X : O) { if (!X->IsDone()) { return false; } } return true; };
	int32 MaxInFlight = 0;
	for (int32 Guard = 0; Guard < 200 && !AllDone(); ++Guard)
	{
		MaxInFlight = FMath::Max(MaxInFlight, Rig.Fake->NumInFlight());
		CompleteOldest(Rig);
		PumpUntil([] { return false; }, 0.01);
	}
	TestTrue(TEXT("all five finish"), AllDone());
	TestEqual(TEXT("each reached the transport once"), Rig.Fake->NumCalls(), 5);
	TestTrue(TEXT("never more than two at once"), MaxInFlight <= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterOrderTest, "VaRest.Http.Limiter.PriorityOrder",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterOrderTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	struct FSend
	{
		const TCHAR* Name;
		EVaRestPriority Priority;
	};
	const FSend Sends[] = { { TEXT("A"), EVaRestPriority::Normal }, { TEXT("B"), EVaRestPriority::Low }, { TEXT("C"), EVaRestPriority::Normal },
		{ TEXT("D"), EVaRestPriority::Highest }, { TEXT("E"), EVaRestPriority::High }, { TEXT("F"), EVaRestPriority::Normal } };
	for (const FSend& Send : Sends)
	{
		Rig.Client.Get(FString::Printf(TEXT("/%s"), Send.Name)).Priority(Send.Priority).Send();
		PumpUntil([] { return false; }, 0.01); // A is granted first; the rest queue in this order
	}
	TArray<FString> Order;
	for (int32 Step = 0; Step < 6; ++Step)
	{
		PumpUntil([&Rig, Step] { return Rig.Fake->NumCalls() > Step; });
		Order.Add(Rig.Fake->GetCall(Step).Request.Url.RightChop(FString(TEXT("https://api.test/")).Len()));
		Rig.Fake->Complete(Step, Ok());
	}
	// Highest -> Low, FIFO within a priority (spec 4.6)
	TestEqualSensitive(TEXT("dispatch order"), FString::Join(Order, TEXT(",")), FString(TEXT("A,D,E,C,F,B")));
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterCancelTest, "VaRest.Http.Limiter.CancelQueued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	const FVaRestRequestHandle BHandle = Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	PumpUntil([] { return false; }, 0.02);
	BHandle.Cancel();
	TestTrue(TEXT("a queued request cancels at once"), Rig.Wait(B));
	TestTrue(TEXT("... as Canceled"), B->Error.Kind == EVaRestErrorKind::Canceled);
	Rig.Fake->Complete(0, Ok());
	TestTrue(TEXT("A finishes"), Rig.Wait(A));
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("the canceled one is never dispatched"), Rig.Fake->NumCalls(), 1);
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	TestTrue(TEXT("and the slot is free for the next"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	Rig.Fake->Complete(1, Ok());
	Rig.Wait(C);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterOwnerTest, "VaRest.Http.Limiter.OwnerGoneWhileQueued",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterOwnerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto A = MakeOutcome<FVaRestJson>();
	auto Queued = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	const FVaRestRequestHandle QueuedHandle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner), Queued).Send();
	PumpUntil([] { return false; }, 0.02);
	Owner->MarkAsGarbage();
	Rig.Fake->Complete(0, Ok());
	TestTrue(TEXT("A finishes"), Rig.Wait(A));
	PumpUntil([&QueuedHandle] { return !QueuedHandle.IsActive(); });
	TestEqual(TEXT("an owner gone while queued: never dispatched"), Rig.Fake->NumCalls(), 1);
	TestEqual(TEXT("... no callbacks"), Queued->CompleteCalls.load(), 0);
	auto C = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), C).Send();
	TestTrue(TEXT("... and its slot was not leaked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	Rig.Fake->Complete(1, Ok());
	Rig.Wait(C);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterCapacityTest, "VaRest.Http.Limiter.CapacityChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterCapacityTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LimiterTestPrivate;
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	for (int32 i = 0; i < 3; ++i)
	{
		Rig.Client.Get(FString::Printf(TEXT("/c/%d"), i)).Send();
	}
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("one at a time"), Rig.Fake->NumCalls(), 1);
	Rig.Client.WithMaxConcurrent(3);
	TestEqual(TEXT("review M-2: nothing is granted inside WithMaxConcurrent()"), Rig.Fake->NumCalls(), 1);
	TestTrue(TEXT("raising MaxConcurrent dispatches the queue on the next game-thread task"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; }));
	Rig.Client.CancelAll();
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLimiterDroppedTest, "VaRest.Http.Limiter.DroppedCompletionFreesSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLimiterDroppedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Review I-3: a completion the transport drops still finishes the request, and its slot comes back
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto First = MakeOutcome<FVaRestJson>();
	auto Second = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/first")), First).Send();
	Observe(Rig.Client.Get(TEXT("/second")), Second).Send();
	PumpUntil([] { return false; }, 0.02);
	TestEqual(TEXT("one slot"), Rig.Fake->NumCalls(), 1);
	Rig.Fake->Drop(0);
	TestTrue(TEXT("the dropped one finishes"), Rig.Wait(First));
	TestTrue(TEXT("... once, with OnError and OnComplete"), First->ErrorCalls == 1 && First->CompleteCalls == 1 && First->Error.Message.Contains(TEXT("TransportDropped")));
	TestTrue(TEXT("the slot is released: the queued one is dispatched"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }));
	Rig.Client.CancelAll();
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http.Limiter`
Expected: the build fails: no `FVaRestClient::WithMaxConcurrent`.

- [ ] **Step 3: The limiter** — `Source/VaRestHttp/Private/VaRestLimiter.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"

namespace VaRest::Http
{
	/**
	 * Spec 4.6: MaxConcurrent slots and a priority queue, Highest first, FIFO within a priority. Strict priority, as the
	 * spec says: under sustained higher-priority load a Low request can wait indefinitely (review M-19). Game thread.
	 */
	class FLimiter
	{
	public:
		using FGrant = TUniqueFunction<void(double /*WaitedSeconds*/)>;
		using FAbort = TUniqueFunction<void()>;

		/** At least 1. Grants nothing by itself: the caller pumps, from a game-thread task (review M-2) */
		void SetCapacity(int32 InCapacity);
		/** Grants queued attempts while slots are free. Not re-entrant: a Release inside a grant only frees its slot (review M-1). */
		void Pump(double Now);
		/** OnGrant now when a slot is free, else when one is; OnAbort instead if Abort() takes it out of the queue */
		void Acquire(uint64 RequestId, EVaRestPriority Priority, double Now, FGrant&& OnGrant, FAbort&& OnAbort);
		/** A granted attempt ended */
		void Release(double Now);
		/** Takes a queued attempt out and runs its OnAbort; false when it is not queued */
		bool Abort(uint64 RequestId);

		int32 NumActive() const { return Active; }
		int32 NumQueued() const;
		int32 GetCapacity() const { return Capacity; }

	private:
		struct FEntry
		{
			uint64 RequestId = 0;
			double EnqueuedAt = 0.0;
			FGrant Grant;
			FAbort OnAbort;
		};

		/** Indexed by EVaRestPriority: Highest = 0 ... Low = 3 */
		TArray<FEntry> Queues[4];
		int32 Active = 0;
		int32 Capacity = 16;
		bool bPumping = false;
	};
}
```

`Source/VaRestHttp/Private/VaRestLimiter.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLimiter.h"

static_assert(static_cast<int32>(EVaRestPriority::Highest) == 0 && static_cast<int32>(EVaRestPriority::Low) == 3, "FLimiter indexes its queues by priority");

void VaRest::Http::FLimiter::SetCapacity(int32 InCapacity)
{
	check(IsInGameThread());
	Capacity = FMath::Max(1, InCapacity);
}

void VaRest::Http::FLimiter::Acquire(uint64 RequestId, EVaRestPriority Priority, double Now, FGrant&& OnGrant, FAbort&& OnAbort)
{
	check(IsInGameThread());
	if (Active < Capacity && NumQueued() == 0)
	{
		++Active;
		OnGrant(0.0);
		return;
	}
	FEntry& Entry = Queues[static_cast<int32>(Priority)].AddDefaulted_GetRef();
	Entry.RequestId = RequestId;
	Entry.EnqueuedAt = Now;
	Entry.Grant = MoveTemp(OnGrant);
	Entry.OnAbort = MoveTemp(OnAbort);
}

void VaRest::Http::FLimiter::Release(double Now)
{
	check(IsInGameThread());
	Active = FMath::Max(0, Active - 1);
	// Review M-1: inside a grant (an exchange canceled at dispatch answers at once) only the slot is freed; the pump
	// already running takes the next entry. Recursing instead grows the stack with every dead entry in the queue.
	if (!bPumping)
	{
		Pump(Now);
	}
}

bool VaRest::Http::FLimiter::Abort(uint64 RequestId)
{
	check(IsInGameThread());
	for (TArray<FEntry>& Queue : Queues)
	{
		const int32 Index = Queue.IndexOfByPredicate([RequestId](const FEntry& Entry) { return Entry.RequestId == RequestId; });
		if (Index != INDEX_NONE)
		{
			FEntry Removed = MoveTemp(Queue[Index]);
			Queue.RemoveAt(Index);
			Removed.OnAbort();
			return true;
		}
	}
	return false;
}

int32 VaRest::Http::FLimiter::NumQueued() const
{
	int32 Count = 0;
	for (const TArray<FEntry>& Queue : Queues)
	{
		Count += Queue.Num();
	}
	return Count;
}

void VaRest::Http::FLimiter::Pump(double Now)
{
	check(IsInGameThread());
	if (bPumping)
	{
		return;
	}
	// A grant may finish at once (a canceled exchange) and Release; that Release only frees the slot (review M-1). Every
	// pass re-reads the queues and holds nothing in them across the call.
	TGuardValue<bool> Pumping(bPumping, true);
	while (Active < Capacity)
	{
		int32 Level = 0;
		while (Level < 4 && Queues[Level].Num() == 0)
		{
			++Level;
		}
		if (Level == 4)
		{
			return;
		}
		FEntry Next = MoveTemp(Queues[Level][0]);
		Queues[Level].RemoveAt(0);
		++Active;
		Next.Grant(Now - Next.EnqueuedAt);
	}
}
```

- [ ] **Step 4: Wire it in** — in `Source/VaRestHttp/Private/VaRestClientState.h`, add `#include "VaRestLimiter.h"`, a public accessor and a member:

```cpp
		/** Game thread */
		FLimiter& GetLimiter() { return Limiter; }
```

```cpp
		/** Spec 4.6 */
		FLimiter Limiter;
```

and next to the other factories: `FVaRestStageRef MakeLimiterStage();`.

In `Source/VaRestHttp/Private/VaRestClientState.cpp`, in `Publish`, replace `NewChain->Add(MakeTransportStage());` with:

```cpp
	NewChain->Add(MakeLimiterStage()); // spec 4.6: "next to the transport, so queued retries wait their turn too"
	NewChain->Add(MakeTransportStage());
```

and at the end of `Publish`, after the write lock's scope (wrap the assignment in its own braces):

```cpp
	{
		FWriteScopeLock WriteLock(ConfigLock);
		Config = Next;
	}
	const int32 Wanted = FMath::Max(1, Next->MaxConcurrent);
	if (Limiter.GetCapacity() != Wanted)
	{
		// Review M-2: only on a change, and the grants from a later game-thread task -- a grant can run a request to its
		// end at once, and Publish runs inside WithHeader(), SetSecretValue() and the like (decision 3)
		Limiter.SetCapacity(Wanted);
		UE::Tasks::Launch(TEXT("VaRest.LimiterPump"), [Weak = TWeakPtr<FClientState, ESPMode::ThreadSafe>(AsShared())]
		{
			if (const TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
			{
				Pinned->Limiter.Pump(Pinned->GetConfig()->Clock->Now());
			}
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
```

In `Source/VaRestHttp/Private/VaRestPipeline.cpp`, add to `PipelinePrivate`:

```cpp
	/** Spec 4.6: per attempt, right before the transport */
	class FLimiterStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Limiter"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FVaRestExchange& Ex = *Exchange;
			// One of the two closures below runs, exactly once: the grant or the abort
			TSharedRef<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Outcome = MakeShared<FVaRestOutcomeCallback, ESPMode::ThreadSafe>(MoveTemp(OnOutcome));
			Ex.bInLimiterQueue = true;
			Ex.State->GetLimiter().Acquire(Ex.RequestId, Ex.Priority, Ex.Config->Clock->Now(),
				[Exchange, Next, Outcome](double Waited)
				{
					Exchange->bInLimiterQueue = false;
					Exchange->LimiterWait = Waited; // Inspector IN-2
					Next.Proceed(Exchange, [Exchange, Outcome](FVaRestAttemptOutcome&& Result)
					{
						// The slot is freed before the outcome goes further up: a stage that re-issues queues again
						Exchange->State->GetLimiter().Release(Exchange->Config->Clock->Now());
						(*Outcome)(MoveTemp(Result));
					});
				},
				[Exchange, Outcome]()
				{
					Exchange->bInLimiterQueue = false;
					(*Outcome)(FVaRestAttemptOutcome::Canceled());
				});
		}
	};
```

and the factory:

```cpp
FVaRestStageRef VaRest::Http::MakeLimiterStage()
{
	return MakeShared<PipelinePrivate::FLimiterStage, ESPMode::ThreadSafe>();
}
```

In `FClientState::Cancel`, insert before `if (Ex.TransportCall.IsValid())`:

```cpp
	if (Ex.bInLimiterQueue && Limiter.Abort(Ex.RequestId))
	{
		// Out of the queue: its abort answers Canceled up the chain, and no slot was held
		return;
	}
```

- [ ] **Step 5: The option** — in `Source/VaRestHttp/Public/VaRestClient.h`, after `WithMaxJsonBytes`:

```cpp
	/** Spec 4.6: requests at once (at least 1); the rest queue by priority. Default: UVaRestHttpSettings::MaxConcurrent (16). */
	FVaRestClient& WithMaxConcurrent(int32 MaxConcurrent);
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithMaxConcurrent(int32 MaxConcurrent)
{
	if (State.IsValid())
	{
		State->EditConfig([MaxConcurrent](VaRest::Http::FClientConfig& Edited) { Edited.MaxConcurrent = FMath::Max(1, MaxConcurrent); });
	}
	return *this;
}
```

- [ ] **Step 6: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including the six `VaRest.Http.Limiter.*` tests; the earlier suites stay green.

- [ ] **Step 7: Restore-the-bug (priority order)**

In `FLimiter::Acquire`, temporarily always enqueue into `Queues[2]` (Normal). Run `Tools/run_tests.sh VaRest.Http.Limiter`; expected FAIL in `VaRest.Http.Limiter.PriorityOrder` (order `A,B,C,D,E,F`). Restore; PASS. Report both runs.

- [ ] **Step 8: Restore-the-bug (slot leak)**

In `FLimiterStage`'s outcome lambda, temporarily skip `Release` when `Result.Failure == EVaRestTransportFailure::Canceled`. Run `Tools/run_tests.sh VaRest.Http.Limiter`; expected FAIL in `VaRest.Http.Limiter.OwnerGoneWhileQueued` ("its slot was not leaked": the transport stage's `Canceled` answer for the gone owner no longer frees the slot). Restore; PASS. Report both runs.

- [ ] **Step 9: Commit**

```bash
git add Source/VaRestHttp/Private/VaRestLimiter.h Source/VaRestHttp/Private/VaRestLimiter.cpp Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Public/VaRestClient.h \
  Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/Tests/VaRestHttpLimiterTest.cpp
git commit -m "http: the concurrency limiter -- MaxConcurrent per client, priority queue, cancel out of the queue

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 13: Timeouts and progress (spec §3.1, §3.2, §4.7)

Spec §4.7: "Per attempt: `SetTimeout` (total) and `SetActivityTimeout`; plus the retry `Deadline` across attempts" (the deadline is M4's). Spec §3.1: `.WithTimeout(/*Total*/ 15.f, /*Activity*/ 5.f)`; §3.2: a request's `Timeout` and "`OnProgress(sent, received, total)` (throttled, game thread)". Resolution already happens in `Resolve` (Task 8: the request's value, else the client's, else 0 = the engine's default) and both transports apply it (Tasks 6, 7), so this task adds the two options and proves the values reach the transport and fire on virtual time. Progress (decision 8): the transport writes byte counts into the probe from its own threads; a clock timer on the game thread reads them every 100 ms and calls `OnProgress` only when they changed, plus once when the attempt ends. The progress delegate is bound only for a request that has `OnProgress` (IN-11; Task 15 adds trace metadata). An owner that disappears mid-download cancels the request at the next progress tick.

**Files:**
- Modify: `Source/VaRestHttp/Public/VaRestClient.h` (`TVaRestRequest::Timeout`, `OnProgress`; `FVaRestClient::WithTimeout`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (`WithTimeout`)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`DeliverProgress`, `ScheduleProgress`; `FTransportStage`, `OnAttemptDone`)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpTimeoutProgressTest.cpp`

**Interfaces:**
- Consumes: `FVaRestTransportProbe`, `IVaRestClock::SetTimer/ClearTimer` (Task 6); `IResultHandler::WantsProgress/Progress` (Task 8); `FClientState::Cancel` (Task 11); `PipelinePrivate::IsOwnerGone`.
- Produces: `TVaRestRequest<T>::Timeout(float TotalSeconds, float ActivitySeconds = 0.f) &&` (a value above 0 replaces the client's); `TVaRestRequest<T>::OnProgress(FuncType&&) &&` (`(const FVaRestProgress&)`); `FVaRestClient& FVaRestClient::WithTimeout(float Total, float Activity = 0.f)`; `PipelinePrivate::ProgressInterval = 0.1`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpTimeoutProgressTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTimeoutAppliedTest, "VaRest.Http.Timeout.Applied",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTimeoutAppliedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Plain;
	Plain.Client.Get(TEXT("/p")).Send();
	FTestRig Rig;
	Rig.Client.WithTimeout(10.f, 3.f);
	Rig.Client.Get(TEXT("/a")).Send();
	Rig.Client.Get(TEXT("/b")).Timeout(2.f).Send();
	Rig.Client.Get(TEXT("/c")).Timeout(2.f, 1.f).Send();
	TestTrue(TEXT("all reach the transport"), PumpUntil([&] { return Plain.Fake->NumCalls() == 1 && Rig.Fake->NumCalls() == 3; }));
	auto Of = [&Rig](int32 i) -> const FVaRestTransportRequest& { return Rig.Fake->GetCall(i).Request; };
	TestTrue(TEXT("no setting: 0, the engine's default (HttpModule.cpp:96-98)"), Plain.Fake->GetCall(0).Request.Timeout == 0.f && Plain.Fake->GetCall(0).Request.ActivityTimeout == 0.f);
	TestTrue(TEXT("the client's timeouts (spec 3.1)"), Of(0).Timeout == 10.f && Of(0).ActivityTimeout == 3.f);
	TestTrue(TEXT("a request's total timeout replaces the client's; its activity timeout stays"), Of(1).Timeout == 2.f && Of(1).ActivityTimeout == 3.f);
	TestTrue(TEXT("... and both"), Of(2).Timeout == 2.f && Of(2).ActivityTimeout == 1.f);
	PumpUntil([&] { return Plain.Client.GetInFlightCount() == 0 && Rig.Client.GetInFlightCount() == 0; });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTimeoutFiresTest, "VaRest.Http.Timeout.Fires",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTimeoutFiresTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	auto Total = MakeOutcome<FVaRestJson>();
	auto Idle = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/slow")).Timeout(1.f), Total).Send();
	Observe(Rig.Client.Get(TEXT("/silent")).Timeout(0.f, 2.f), Idle).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; });
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("the total timeout"), Rig.Wait(Total));
	TestTrue(TEXT("... is Kind Timeout, retryable (spec 4.1)"), Total->Error.Kind == EVaRestErrorKind::Timeout && Total->Error.bRetryable);
	TestFalse(TEXT("the other is still waiting"), Idle->IsDone());
	Rig.Clock->Advance(1.0);
	TestTrue(TEXT("the activity timeout"), Rig.Wait(Idle));
	TestTrue(TEXT("... is Kind Connection, as the engine reports it (decision 19)"), Idle->Error.Kind == EVaRestErrorKind::Connection
		&& Idle->Error.Message.Contains(TEXT("ActivityTimeout")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpProgressTest, "VaRest.Http.Progress.Throttled",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpProgressTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	struct FSeen
	{
		TArray<FVaRestProgress> Calls;
		bool bGameThread = true;
	};
	TSharedRef<FSeen, ESPMode::ThreadSafe> Seen = MakeShared<FSeen, ESPMode::ThreadSafe>();
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/up")).Body(TArray<uint8>{ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 }).OnProgress([Seen](const FVaRestProgress& Progress)
	{
		Seen->bGameThread &= IsInGameThread();
		Seen->Calls.Add(Progress);
	}), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	TestTrue(TEXT("OnProgress binds progress reporting (IN-11)"), Rig.Fake->GetCall(0).Request.bReportProgress);

	Rig.Fake->ReportProgress(0, 4, 0);
	Rig.Fake->ReportProgress(0, 10, 0);
	TestEqual(TEXT("throttled: nothing between ticks"), Seen->Calls.Num(), 0);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("one call per tick, with the latest counts"), Seen->Calls.Num(), 1);
	TestTrue(TEXT("... sent and the upload total"), Seen->Calls.Num() == 1 && Seen->Calls[0].BytesSent == 10 && Seen->Calls[0].UploadTotal == 10);
	Rig.Clock->Advance(0.1);
	TestEqual(TEXT("no change, no call"), Seen->Calls.Num(), 1);
	Rig.Fake->ReportProgress(0, 10, 50, 120);
	Rig.Clock->Advance(0.05);
	TestEqual(TEXT("not before the next tick"), Seen->Calls.Num(), 1);
	Rig.Clock->Advance(0.05);
	TestTrue(TEXT("received and the download total"), Seen->Calls.Num() == 2 && Seen->Calls[1].BytesReceived == 50 && Seen->Calls[1].DownloadTotal == 120);

	FVaRestFakeResponse Body = FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT(R"({"k":"0123456789"})")));
	Rig.Fake->Complete(0, Body);
	TestTrue(TEXT("completes"), Rig.Wait(Outcome));
	TestEqual(TEXT("a last call when the attempt ends"), Seen->Calls.Num(), 3);
	TestTrue(TEXT("... with every byte"), Seen->Calls.Num() == 3 && Seen->Calls[2].BytesReceived == 18);
	TestTrue(TEXT("always on the game thread (spec 3.2)"), Seen->bGameThread);

	Rig.Client.Get(TEXT("/quiet")).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; });
	TestFalse(TEXT("no OnProgress, no progress binding (IN-11)"), Rig.Fake->GetCall(1).Request.bReportProgress);
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });

	// Review M-16: a compressed body's Content-Length counts compressed bytes; past it the total is unknown, never > 100%
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	TSharedRef<FSeen, ESPMode::ThreadSafe> Encoded = MakeShared<FSeen, ESPMode::ThreadSafe>();
	auto Gzip = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/gz")).OnProgress([Encoded](const FVaRestProgress& Progress) { Encoded->Calls.Add(Progress); }), Gzip).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; });
	Rig.Fake->ReportProgress(2, 0, 300, 100);
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("received past the reported total: the total is unknown (-1)"), Encoded->Calls.Num() == 1 && Encoded->Calls[0].DownloadTotal == -1);
	Rig.Client.CancelAll();
	PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpProgressOwnerGoneTest, "VaRest.Http.Progress.NotAfterOwnerGone",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpProgressOwnerGoneTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// Review C-1: the usual .Owner(this).OnProgress([this]...) -- the last progress call must not run into a dead owner
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Calls = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/owned")).Owner(Owner).OnProgress([Calls](const FVaRestProgress&) { ++*Calls; }), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; });
	Rig.Fake->ReportProgress(0, 0, 10, 100);
	Owner->MarkAsGarbage();
	const int32 AtMark = Calls->load();
	Rig.Fake->Complete(0, FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT(R"({"k":1})"))));
	PumpUntil([&Handle] { return !Handle.IsActive(); });
	PumpUntil([] { return false; }, 0.05);
	TestEqual(TEXT("spec 3.2: owner gone -> no callbacks, progress included"), Calls->load() - AtMark, 0);
	TestEqual(TEXT("... and no result callbacks"), Outcome->CompleteCalls.load(), 0);

	// After an explicit Cancel() too (review M-3)
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> AfterCancel = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Canceled = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CancelHandle = Observe(Rig.Client.Get(TEXT("/canceled")).OnProgress([AfterCancel](const FVaRestProgress&) { ++*AfterCancel; }), Canceled).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; });
	CancelHandle.Cancel();
	const int32 AtCancel = AfterCancel->load();
	Rig.Fake->ReportProgress(1, 0, 10, 100);
	Rig.Clock->Advance(0.1);
	TestTrue(TEXT("completes"), Rig.Wait(Canceled));
	TestEqual(TEXT("no progress after Cancel() returned"), AfterCancel->load() - AtCancel, 0);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: the build fails: no `FVaRestClient::WithTimeout`, `TVaRestRequest::Timeout`, `OnProgress`.

- [ ] **Step 3: The options** — in `Source/VaRestHttp/Public/VaRestClient.h`, add to `TVaRestRequest` after `MaxJsonBytes`:

```cpp
	/**
	 * Spec 4.7, per attempt: a value above 0 replaces the client's (0 keeps it). With a total and no activity timeout the
	 * transport bounds silence by the total too, so the engine's 30 s idle default never fires first (decision 30); on Mac
	 * and iOS the engine's connection timeout still caps silence (docs/dev/http-thread-completion.md).
	 */
	TVaRestRequest Timeout(float TotalSeconds, float ActivitySeconds = 0.f) &&
	{
		if (TotalSeconds > 0.f)
		{
			Spec->Timeout = TotalSeconds;
		}
		if (ActivitySeconds > 0.f)
		{
			Spec->ActivityTimeout = ActivitySeconds;
		}
		return MoveTemp(*this);
	}

	/** (const FVaRestProgress&): on the game thread, at most every 100 ms while bytes move, and once when the attempt ends (decision 8) */
	template <typename FuncType>
	TVaRestRequest OnProgress(FuncType&& Func) &&
	{
		Handler().OnProgress = Forward<FuncType>(Func);
		return MoveTemp(*this);
	}
```

and to `FVaRestClient` after `WithMaxConcurrent`:

```cpp
	/**
	 * Spec 3.1 / 4.7: seconds per attempt; 0 = the engine's default ([HTTP] HttpTotalTimeout, HttpActivityTimeout). An
	 * activity of 0 with a total above 0 bounds silence by the total (decision 30).
	 */
	FVaRestClient& WithTimeout(float Total, float Activity = 0.f);
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithTimeout(float Total, float Activity)
{
	if (State.IsValid())
	{
		State->EditConfig([Total, Activity](VaRest::Http::FClientConfig& Edited)
		{
			Edited.Timeout = FMath::Max(0.f, Total);
			Edited.ActivityTimeout = FMath::Max(0.f, Activity);
		});
	}
	return *this;
}
```

- [ ] **Step 4: Progress** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`, add to `PipelinePrivate` (after `MarkOwnerGone`):

```cpp
	/** Decision 8's throttle: the game thread reads the probe this often while an attempt runs */
	constexpr double ProgressInterval = 0.1;

	/** Game thread: calls OnProgress when the probe changed since the last call */
	void DeliverProgress(FVaRestExchange& Ex)
	{
		// No progress for a request being canceled, whose owner is gone, or at shutdown (spec 3.2: no callbacks; review C-1).
		// The last call comes from OnAttemptDone after Task 11's owner check only marks it: without this, OnProgress
		// would run into a destroyed owner.
		if (!Ex.Probe.IsValid() || !Ex.Handler.IsValid() || !Ex.Handler->WantsProgress() || Ex.IsCancelRequested() || IsOwnerGone(Ex))
		{
			return;
		}
		FVaRestProgress Current;
		Current.BytesSent = Ex.Probe->BytesSent.load();
		Current.BytesReceived = Ex.Probe->BytesReceived.load();
		Current.UploadTotal = Ex.Probe->UploadTotal.load();
		Current.DownloadTotal = Ex.Probe->DownloadTotal.load();
		if (Current.DownloadTotal >= 0 && Current.BytesReceived > Current.DownloadTotal)
		{
			// Review M-16: a compressed body's Content-Length counts compressed bytes; a percentage must never pass 100%
			Current.DownloadTotal = -1;
		}
		if (Current == Ex.LastProgress)
		{
			return;
		}
		Ex.LastProgress = Current;
		Ex.Handler->Progress(Current);
	}

	void ScheduleProgress(const FVaRestExchangeRef& Exchange)
	{
		const TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Weak = Exchange;
		Exchange->ProgressTimer = Exchange->Config->Clock->SetTimer(ProgressInterval, [Weak]
		{
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid() || !Pinned->TransportCall.IsValid())
			{
				return;
			}
			if (IsOwnerGone(*Pinned))
			{
				// Spec 3.2: an owner gone mid-download stops it now rather than at the end
				Pinned->State->Cancel(Pinned.ToSharedRef(), EVaRestCancelReason::OwnerGone);
				return;
			}
			DeliverProgress(*Pinned);
			ScheduleProgress(Pinned.ToSharedRef());
		});
	}
```

In `FTransportStage::Process`, before `Ex.TransportCall = Cfg.Transport->Start(...)`, add:

```cpp
			// IN-11: the progress delegate is bound only when someone reads it
			Request.bReportProgress = Ex.Handler.IsValid() && Ex.Handler->WantsProgress();
			Ex.LastProgress = FVaRestProgress();
			const bool bWantsProgress = Request.bReportProgress;
```

and after the `Ex.TransportCall = ...;` statement:

```cpp
			if (bWantsProgress)
			{
				ScheduleProgress(Exchange);
			}
```

In `FClientState::OnAttemptDone`, after `Ex.TransportCall.Reset();` (and the owner check):

```cpp
	Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
	// The final counts, so a caller sees 100% even when the transfer was quicker than one tick (decision 8)
	PipelinePrivate::DeliverProgress(Ex);
```

- [ ] **Step 5: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.Timeout.Applied`, `VaRest.Http.Timeout.Fires`, `VaRest.Http.Progress.Throttled`, `VaRest.Http.Progress.NotAfterOwnerGone`. (`Throttled` needs the virtual clock's integer nanoseconds, Task 6: in doubles its third tick falls due at 0.30000000000000004 while the test reaches 0.3, review I1.)

- [ ] **Step 6: Restore-the-bug (the request's timeout)**

In `FTransportStage::Process`, temporarily set `Request.Timeout = Cfg.Timeout;` instead of `Ex.Timeout`. Run `Tools/run_tests.sh VaRest.Http.Timeout`; expected FAIL in `VaRest.Http.Timeout.Applied` ("a request's total timeout replaces the client's") and `.Fires` (no timeout fires). Restore; PASS. Report both runs.

- [ ] **Step 7: Restore-the-bug (throttle)**

In `DeliverProgress`, temporarily delete the `if (Current == Ex.LastProgress) { return; }` check. Run `Tools/run_tests.sh VaRest.Http.Progress`; expected FAIL in `VaRest.Http.Progress.Throttled` ("no change, no call"). Restore; PASS.

- [ ] **Step 7b: Restore-the-bug (no progress into a gone owner, review C-1)**

In `DeliverProgress`, temporarily remove `|| Ex.IsCancelRequested() || IsOwnerGone(Ex)` from the guard. Run `Tools/run_tests.sh VaRest.Http.Progress`; expected FAIL in `VaRest.Http.Progress.NotAfterOwnerGone` ("owner gone -> no callbacks, progress included"). Restore; PASS. Report both runs.

- [ ] **Step 8: Commit**

```bash
git add Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp \
  Source/VaRestHttp/Private/Tests/VaRestHttpTimeoutProgressTest.cpp
git commit -m "http: timeouts per attempt (client and request) and throttled game-thread progress

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 14: Logging with redaction (spec §4.5, IN-13, IN-16)

Spec §4.5: "`EVaRestLogLevel`: `None`, `Basic` (verb, redacted URL, status, ms, bytes), `Headers`, `Body`. Default `Basic`; overridable per client and by the cvar `VaRest.LogLevel`. Body logging is compiled out in Shipping. Always redacted ... Logs use the same redactor. ... Core log lines about a request include `#<RequestId>` (Inspector IN-16). Logged bodies are truncated to 4 KiB." The log stage is per attempt, after custom `PerAttempt` stages (so it logs the final headers) and before the limiter. URLs and headers are redacted on the game thread; body excerpts are copied there (at most 4 KiB, one `memcpy`) and redacted and written on the redaction pipe, because "the game thread never parses or redacts a body" (Inspector §7). This task also gives the client its layer-2 and layer-3 inputs (IN-13): `WithRedactedKeys` adds exact keys, and `SetSecretValue`/`ClearSecretValues` feed the known-secret scrub. M4's auth calls them with tokens. Each change publishes a new redactor; a timer publishes another when a rotated value's 10 minutes run out (Inspector D21).

Line formats (each starts with `#<id>`):

- `Headers`: the request, `#57 > POST https://h/x?token=*** (attempt 1)`; one line per header, `#57 >   Authorization: ***`.
- `Body`: `#57 > body (json, 120 B): {...}`.
- `Basic`, at the end of the attempt: `#57 POST https://h/x?token=*** -> 201 (12 ms, 340 B down, 120 B up)`. A status of 400 or more, or a transport failure other than a cancel, logs as Warning.
- `Headers` again for the response: `#57 <   Content-Type: application/json`, and `Body`: `#57 < body (json, 340 B): {...}`.

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestLogStage.cpp`
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp` (secrets, the expiry timer, the log stage in the chain, `EffectiveLogLevel`)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Resolve`: the effective level)
- Modify: `Source/VaRestHttp/Public/VaRestClient.h`, `Source/VaRestHttp/Private/VaRestClient.cpp` (`WithLogLevel`, `WithRedactedKeys`, `SetSecretValue`, `ClearSecretValues`)
- Modify: `Source/VaRestHttp/Private/VaRestHttpTesting.cpp` (the rig logs nothing by default)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp`

**Interfaces:**
- Consumes: `FVaRestRedactor`, `FVaRestSecretSet`, `GetRedactionPipe()` (Task 5); `FVaRestTraceExcerpt` (Task 4); the chain (Tasks 8, 12).
- Produces: `FVaRestClient& WithLogLevel(EVaRestLogLevel)`, `FVaRestClient& WithRedactedKeys(TConstArrayView<FString> Keys)`, `void SetSecretValue(FName Slot, const FString& Value)`, `void ClearSecretValues()` (game thread); cvar `VaRest.LogLevel` (-1 = each client's own, 0-3 force); private `VaRest::Http::MakeLogStage()`, `VaRest::Http::EffectiveLogLevel(EVaRestLogLevel)`, `VaRest::Http::LogBodyCap = 4096`, `FClientState::SetSecret`, `FClientState::ClearSecrets`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::LogTestPrivate
{
	FVaRestFakeResponse Json(int32 Status, const char* Text)
	{
		return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	FString Id(const FVaRestRequestHandle& Handle)
	{
		return FString::Printf(TEXT("#%llu "), Handle.GetId());
	}

	/** Sends and waits for the callbacks and the body lines */
	template <typename T>
	FVaRestRequestHandle Run(VaRest::Http::Testing::FTestRig& Rig, TVaRestRequest<T>&& Request)
	{
		auto Outcome = VaRest::Http::Testing::MakeOutcome<T>();
		const FVaRestRequestHandle Handle = VaRest::Http::Testing::Observe(MoveTemp(Request), Outcome).Send();
		Rig.Wait(Outcome);
		VaRest::Http::Testing::FlushRedactionPipe();
		return Handle;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogLevelsTest, "VaRest.Http.Log.Levels",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogLevelsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;

	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	const FVaRestRequestHandle Basic = Run(Rig, Rig.Client.Get(TEXT("/x?token=abc")).Header(TEXT("Authorization"), TEXT("Bearer abc")));
	TestEqual(TEXT("Basic: one line"), Capture.Count(Id(Basic)), 1);
	TestTrue(TEXT("... verb, redacted URL, status (spec 4.5)"), Capture.Contains(Id(Basic) + TEXT("GET https://api.test/x?token=*** -> 200")));

	Rig.Client.WithLogLevel(EVaRestLogLevel::Headers);
	const FVaRestRequestHandle Headers = Run(Rig, Rig.Client.Get(TEXT("/h")).Header(TEXT("Authorization"), TEXT("Bearer abc")).Header(TEXT("X-Trace"), TEXT("t")));
	TestTrue(TEXT("Headers: the request line"), Capture.Contains(Id(Headers) + TEXT("> GET https://api.test/h (attempt 1)")));
	TestTrue(TEXT("... redacted request headers"), Capture.Contains(Id(Headers) + TEXT(">   Authorization: ***")));
	TestTrue(TEXT("... plain ones as they are"), Capture.Contains(Id(Headers) + TEXT(">   X-Trace: t")));
	TestTrue(TEXT("... the response headers"), Capture.Contains(Id(Headers) + TEXT("<   Content-Type: application/json")));
	TestFalse(TEXT("... no bodies"), Capture.Contains(Id(Headers) + TEXT("< body")));

	Rig.Client.WithLogLevel(EVaRestLogLevel::None);
	const FVaRestRequestHandle None = Run(Rig, Rig.Client.Get(TEXT("/n")));
	TestEqual(TEXT("None: nothing"), Capture.Count(Id(None)), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogBodyTest, "VaRest.Http.Log.Body",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	FString Large = TEXT("{\"blob\":\"");
	for (int32 i = 0; i < 10000; ++i)
	{
		Large.AppendChar(TEXT('a' + i % 26));
	}
	Large += TEXT("\"}");
	FTCHARToUTF8 LargeUtf8(*Large);
	Rig.Fake->Enqueue(Json(200, R"({"token":"tok","ok":true})"));
	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(200, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(LargeUtf8.Get()), LargeUtf8.Length())));
	const FVaRestRequestHandle Small = Run(Rig, Rig.Client.Post(TEXT("/login")).Body(FVaRestJson::Object().Set(TEXT("user"), TEXT("neo")).Set(TEXT("password"), TEXT("pw"))));
	TestTrue(TEXT("the request body, JSON keys redacted"), Capture.Contains(Id(Small) + TEXT("> body (json, ")) && Capture.Contains(TEXT("{\"user\":\"neo\",\"password\":\"***\"}")));
	TestTrue(TEXT("the response body, JSON keys redacted"), Capture.Contains(Id(Small) + TEXT("< body (json, ")) && Capture.Contains(TEXT("{\"token\":\"***\",\"ok\":true}")));

	const FVaRestRequestHandle Big = Run(Rig, Rig.Client.Get(TEXT("/big")));
	const TArray<FString> Lines = Capture.GetLines();
	const FString* BodyLine = Lines.FindByPredicate([&Big](const FString& Line) { return Line.Contains(Id(Big) + TEXT("< body")); });
	TestTrue(TEXT("a large body is logged"), BodyLine != nullptr);
	TestTrue(TEXT("... truncated to 4 KiB (spec 4.5)"), BodyLine && BodyLine->Contains(TEXT("truncated")) && BodyLine->Len() < 4096 + 200);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCvarTest, "VaRest.Http.Log.CvarOverride",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCvarTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	FVaRestRequestHandle Silenced;
	{
		// TGuardConsoleVariable restores the previous value when it goes (IConsoleManager.h:1404-1448)
		TGuardConsoleVariable<int32> Guard(TEXT("VaRest.LogLevel"), 0);
		Silenced = Run(Rig, Rig.Client.Get(TEXT("/quiet")));
	}
	TestEqual(TEXT("VaRest.LogLevel 0 silences a Basic client (spec 4.5)"), Capture.Count(Id(Silenced)), 0);
	FVaRestRequestHandle Forced;
	{
		TGuardConsoleVariable<int32> Guard(TEXT("VaRest.LogLevel"), 2);
		Forced = Run(Rig, Rig.Client.Get(TEXT("/loud")));
	}
	TestTrue(TEXT("VaRest.LogLevel 2 raises it to Headers"), Capture.Contains(Id(Forced) + TEXT("> GET")));
	const FVaRestRequestHandle Own = Run(Rig, Rig.Client.Get(TEXT("/own")));
	TestEqual(TEXT("-1 again: the client's own level"), Capture.Count(Id(Own)), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogCanariesTest, "VaRest.Http.Log.Canaries",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogCanariesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("-> 500"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const TArray<FString> Added = { TEXT("player_pin") };
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body).WithRedactedKeys(Added);
	const TCHAR* Bearer = TEXT("canary-bearer-0123456789");
	Rig.Client.SetSecretValue(TEXT("bearer"), Bearer);

	FVaRestFakeResponse Error = Json(500, R"({"refresh_token":"canary-refresh","msg":"bad token canary-bearer-0123456789"})");
	Error.Headers.Add(TEXT("Location"), TEXT("https://app.test/cb?code=canary-code&state=1"));
	Error.Headers.Add(TEXT("Set-Cookie"), TEXT("sid=canary-cookie"));
	Rig.Fake->Enqueue(Error);
	Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200, TEXT("echo canary-bearer-0123456789 echo")));
	Run(Rig, Rig.Client.Post(FString::Printf(TEXT("/u/%s/x?token=canary-query&page=1"), Bearer))
		.Header(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), Bearer))
		.Body(FVaRestJson::Object().Set(TEXT("password"), TEXT("canary-password")).Set(TEXT("playerPin"), TEXT("canary-pin")).Set(TEXT("note"), TEXT("hi"))));
	Run(Rig, Rig.Client.Get<FString>(TEXT("/echo")));

	int32 Leaks = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (Line.Contains(TEXT("canary"), ESearchCase::CaseSensitive))
		{
			++Leaks;
			AddInfo(FString::Printf(TEXT("leaked: %s"), *Line));
		}
	}
	// Every secret class of Inspector 2.2: header, query key, userinfo-free path value, JSON key, an added key,
	// the known-secret scrub in a path, a 5xx JSON body, a Location header, a Set-Cookie, a text/plain body
	TestEqual(TEXT("no canary in any log line at Body level"), Leaks, 0);
	TestTrue(TEXT("... and the lines were written"), Capture.Contains(TEXT("< body (json")) && Capture.Contains(TEXT("< body (text")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogIdTest, "VaRest.Http.Log.RequestIdOnEveryLine",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogIdTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	// Registered before the line is logged: an expected message is matched as it arrives
	AddExpectedMessagePlain(TEXT("-> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	Rig.Client.WithLogLevel(EVaRestLogLevel::Body);
	Rig.Fake->Enqueue(Json(404, R"({"error":"x"})"));
	const FVaRestRequestHandle Handle = Run(Rig, Rig.Client.Post(TEXT("/missing")).Header(TEXT("X-A"), TEXT("1")).Body(TEXT("hello")));
	int32 Lines = 0;
	int32 Tagged = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (Line.StartsWith(TEXT("LogVaRestHttp: ")))
		{
			++Lines;
			Tagged += Line.Contains(Id(Handle)) ? 1 : 0;
		}
	}
	TestTrue(TEXT("several lines at Body level"), Lines >= 6);
	TestEqual(TEXT("IN-16: every core log line about a request includes #<RequestId>"), Tagged, Lines);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpLogRotationTest, "VaRest.Http.Log.SecretRotation",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpLogRotationTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::LogTestPrivate;
	FTestRig Rig;
	FLogCapture Capture;
	Rig.Client.WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-one-abcdefgh"));
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("token-two-abcdefgh"));
	const FVaRestRequestHandle Rotated = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("a rotated-out value is still scrubbed (Inspector D21)"), Capture.Contains(Id(Rotated) + TEXT("GET https://api.test/p/*** ")));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime);
	TestEqual(TEXT("the expiry timer fired once and left nothing armed (review C1)"), Rig.Clock->NumPendingTimers(), 0);
	const FVaRestRequestHandle Expired = Run(Rig, Rig.Client.Get(TEXT("/p/token-one-abcdefgh")));
	TestTrue(TEXT("... until its 10 minutes are up"), Capture.Contains(Id(Expired) + TEXT("GET https://api.test/p/token-one-abcdefgh ")));
	Rig.Client.ClearSecretValues();
	const FVaRestRequestHandle Cleared = Run(Rig, Rig.Client.Get(TEXT("/p/token-two-abcdefgh")));
	TestTrue(TEXT("ClearSecretValues forgets the current value too (logout)"), Capture.Contains(Id(Cleared) + TEXT("GET https://api.test/p/token-two-abcdefgh ")));
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http.Log`
Expected: the build fails: no `WithLogLevel`, `WithRedactedKeys`, `SetSecretValue`, `ClearSecretValues`.

- [ ] **Step 3: The client's surface** — in `Source/VaRestHttp/Public/VaRestClient.h`, add to `FVaRestClient` after `WithTimeout`:

```cpp
	/** Spec 4.5; the cvar VaRest.LogLevel (>= 0) overrides every client's */
	FVaRestClient& WithLogLevel(EVaRestLogLevel Level);
	/** Inspector IN-13 layer 2: more keys whose values are redacted in logs and traces, matched exactly after normalisation */
	FVaRestClient& WithRedactedKeys(TConstArrayView<FString> Keys);
	/**
	 * Inspector IN-13 layer 3: a secret value (token, API key, session id) replaced by *** wherever it appears in a log
	 * line or a trace. Setting a slot again keeps the old value known for 10 minutes (Inspector D21). Values under 8 bytes
	 * are ignored. M4's auth calls this; a game may too. Game thread.
	 */
	void SetSecretValue(FName Slot, const FString& Value);
	/** Logout: forgets every secret value, the rotated ones too. Game thread. */
	void ClearSecretValues();
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`:

```cpp
FVaRestClient& FVaRestClient::WithLogLevel(EVaRestLogLevel Level)
{
	if (State.IsValid())
	{
		State->EditConfig([Level](VaRest::Http::FClientConfig& Edited) { Edited.LogLevel = Level; });
	}
	return *this;
}

FVaRestClient& FVaRestClient::WithRedactedKeys(TConstArrayView<FString> Keys)
{
	if (State.IsValid())
	{
		State->EditConfig([Keys](VaRest::Http::FClientConfig& Edited) { Edited.RedactKeys.Append(Keys.GetData(), Keys.Num()); });
	}
	return *this;
}

void FVaRestClient::SetSecretValue(FName Slot, const FString& Value)
{
	if (State.IsValid())
	{
		State->SetSecret(Slot, Value);
	}
}

void FVaRestClient::ClearSecretValues()
{
	if (State.IsValid())
	{
		State->ClearSecrets();
	}
}
```

- [ ] **Step 4: Secrets in the state** — in `Source/VaRestHttp/Private/VaRestClientState.h`, add to the public section of `FClientState`:

```cpp
		/** Game thread: layer 3's known values (Inspector 2.2); publishes a new redactor */
		void SetSecret(FName Slot, const FString& Value);
		void ClearSecrets();
```

to the private section:

```cpp
		/** Publishes again when the next rotated-out value expires */
		void ScheduleSecretExpiry();

		FVaRestSecretSet Secrets;
		FVaRestTimerHandle SecretExpiryTimer;
```

and next to the factories:

```cpp
	FVaRestStageRef MakeLogStage();
	/** The cvar VaRest.LogLevel when it is 0..3, else the client's level */
	EVaRestLogLevel EffectiveLogLevel(EVaRestLogLevel ClientLevel);
	/** Spec 4.5: "Logged bodies are truncated to 4 KiB" */
	inline constexpr int32 LogBodyCap = 4096;
```

In `Source/VaRestHttp/Private/VaRestClientState.cpp`, in `Publish`, replace the whole line `NewChain->Add(MakeLimiterStage()); // spec 4.6: ...` (Task 12) with:

```cpp
	NewChain->Add(MakeLogStage()); // after custom PerAttempt stages: it logs the final headers
	NewChain->Add(MakeLimiterStage()); // spec 4.6: "next to the transport, so queued retries wait their turn too"
```

replace the redactor line with:

```cpp
	Next->Redactor = MakeShared<FVaRestRedactor, ESPMode::ThreadSafe>(MoveTemp(Rules), Secrets.GetActive(Next->Clock->Now()));
```

and append:

```cpp
void VaRest::Http::FClientState::SetSecret(FName Slot, const FString& Value)
{
	check(IsInGameThread());
	const double Now = GetConfig()->Clock->Now();
	Secrets.Prune(Now); // review C1: expired values never count toward the next expiry
	Secrets.Set(Slot, Value, Now);
	EditConfig([](FClientConfig&) {});
	ScheduleSecretExpiry();
}

void VaRest::Http::FClientState::ClearSecrets()
{
	check(IsInGameThread());
	Secrets.ClearAll();
	GetConfig()->Clock->ClearTimer(SecretExpiryTimer);
	EditConfig([](FClientConfig&) {});
}

void VaRest::Http::FClientState::ScheduleSecretExpiry()
{
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Current = GetConfig();
	Current->Clock->ClearTimer(SecretExpiryTimer);
	// Review C1: prune first, and only a value still pending counts -- an expired one would re-arm a timer due at once,
	// forever (a hang under the virtual clock, a republished configuration every frame under the real one)
	const double Now = Current->Clock->Now();
	Secrets.Prune(Now);
	const double ExpiresAt = Secrets.NextExpiry(Now);
	if (ExpiresAt < 0.0)
	{
		return;
	}
	const TWeakPtr<FClientState, ESPMode::ThreadSafe> Weak = AsShared();
	SecretExpiryTimer = Current->Clock->SetTimer(ExpiresAt - Now, [Weak]
	{
		if (const TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
		{
			Pinned->EditConfig([](FClientConfig&) {}); // a redactor without the expired value
			Pinned->ScheduleSecretExpiry();
		}
	});
}
```

In `Source/VaRestHttp/Private/VaRestPipeline.cpp`, in `Resolve`, replace `Exchange.LogLevel = Cfg.LogLevel;` with `Exchange.LogLevel = EffectiveLogLevel(Cfg.LogLevel);`.

- [ ] **Step 5: The log stage** — `Source/VaRestHttp/Private/VaRestLogStage.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "HAL/IConsoleManager.h"
#include "Tasks/Pipe.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestRedactionPipe.h"

namespace VaRest::Http::LogStagePrivate
{
	TAutoConsoleVariable<int32> CVarLogLevel(
		TEXT("VaRest.LogLevel"),
		-1,
		TEXT("Overrides every VaRest client's log level (spec 4.5): -1 = each client's own, 0 None, 1 Basic, 2 Headers, 3 Body (Body is compiled out in Shipping)."),
		ECVF_Default);

	using FRedactorRef = TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe>;

	const TCHAR* KindName(EVaRestExcerptKind Kind)
	{
		switch (Kind)
		{
		case EVaRestExcerptKind::Json: return TEXT("json");
		case EVaRestExcerptKind::Text: return TEXT("text");
		case EVaRestExcerptKind::Form: return TEXT("form");
		case EVaRestExcerptKind::Multipart: return TEXT("multipart");
		case EVaRestExcerptKind::Binary: return TEXT("binary");
		case EVaRestExcerptKind::Stream: return TEXT("stream");
		default: return TEXT("skipped");
		}
	}

	void LogHeaders(uint64 RequestId, const TCHAR* Direction, const FVaRestHeaders& Redacted)
	{
		for (const FVaRestHeaders::FPair& Pair : Redacted)
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s   %s: %s"), RequestId, Direction, *Pair.Key, *Pair.Value);
		}
	}

	void WriteExcerpt(uint64 RequestId, const TCHAR* Direction, const FVaRestTraceExcerpt& Excerpt)
	{
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s body (%s, %lld B%s): %s"), RequestId, Direction, KindName(Excerpt.Kind), Excerpt.TotalBytes,
			Excerpt.bTruncated ? TEXT(", truncated") : TEXT(""), *Excerpt.ToText());
	}

#if !UE_BUILD_SHIPPING
	/** Copies at most LogBodyCap bytes here (game thread, one memcpy); redacts and writes on the redaction pipe (Inspector 7) */
	void LogBodyLater(uint64 RequestId, const TCHAR* Direction, const FRedactorRef& Redactor, FString ContentType, TConstArrayView<uint8> Bytes, int64 TotalBytes)
	{
		UE::Tasks::FPipe* Pipe = GetRedactionPipe();
		if (!Pipe)
		{
			return;
		}
		TArray<uint8> Head(Bytes.GetData(), FMath::Min(Bytes.Num(), LogBodyCap));
		Pipe->Launch(TEXT("VaRest.LogBody"), [RequestId, Direction, Redactor, Type = MoveTemp(ContentType), Prefix = MoveTemp(Head), TotalBytes]
		{
			WriteExcerpt(RequestId, Direction, Redactor->RedactBody(Type, Prefix, TotalBytes, LogBodyCap));
		});
	}

	void LogRequestBody(const FVaRestExchange& Ex, const FRedactorRef& Redactor)
	{
		const FVaRestRequestBody& Upload = Ex.Body;
		if (Upload.IsEmpty())
		{
			return;
		}
		UE::Tasks::FPipe* Pipe = GetRedactionPipe();
		if (!Pipe)
		{
			return;
		}
		const uint64 RequestId = Ex.RequestId;
		if (!Upload.FilePath.IsEmpty())
		{
			Pipe->Launch(TEXT("VaRest.LogBody"), [RequestId, Redactor, Path = Upload.FilePath, Type = Upload.ContentType, Total = Upload.ContentLength]
			{
				WriteExcerpt(RequestId, TEXT(">"), Redactor->DescribeStream(Path, Type, Total));
			});
		}
		else if (Upload.Kind == EVaRestBodyKind::Multipart)
		{
			Pipe->Launch(TEXT("VaRest.LogBody"), [RequestId, Redactor, Parts = Upload.PartDescriptors, Total = Upload.ContentLength]
			{
				WriteExcerpt(RequestId, TEXT(">"), Redactor->DescribeMultipart(Parts, Total, LogBodyCap));
			});
		}
		else
		{
			const FString* Header = Ex.AttemptHeaders.Find(TEXT("Content-Type"));
			LogBodyLater(RequestId, TEXT(">"), Redactor, Header ? *Header : Upload.ContentType, Upload.Bytes, Upload.ContentLength);
		}
	}
#endif

	void LogOutcome(const FVaRestExchange& Ex, const FRedactorRef& Redactor, const FString& Url, EVaRestLogLevel Level, double StartedAt, const FVaRestAttemptOutcome& Outcome)
	{
		const double Ms = (Ex.Config->Clock->Now() - StartedAt) * 1000.0;
		if (Outcome.Failure == EVaRestTransportFailure::Canceled)
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> canceled (%.0f ms)"), Ex.RequestId, *Ex.Verb, *Url, Ms);
		}
		else if (Outcome.IsTransportFailure())
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %s %s (%.0f ms)"), Ex.RequestId, *Ex.Verb, *Url, LexToString(Outcome.Failure), *Outcome.FailureDetail.ToString(), Ms);
		}
		else if (Outcome.Status >= 400)
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %d (%.0f ms, %lld B down, %lld B up)"), Ex.RequestId, *Ex.Verb, *Url, Outcome.Status, Ms, Outcome.BytesDown, Outcome.BytesUp);
		}
		else
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> %d (%.0f ms, %lld B down, %lld B up)"), Ex.RequestId, *Ex.Verb, *Url, Outcome.Status, Ms, Outcome.BytesDown, Outcome.BytesUp);
		}
		if (Level >= EVaRestLogLevel::Headers && !Outcome.IsTransportFailure())
		{
			LogHeaders(Ex.RequestId, TEXT("<"), Redactor->RedactHeaders(Outcome.Headers));
		}
#if !UE_BUILD_SHIPPING
		if (Level >= EVaRestLogLevel::Body && !Outcome.IsTransportFailure())
		{
			if (Outcome.bBodyInFile)
			{
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu < body (%lld B): written to the SaveTo file"), Ex.RequestId, Outcome.BytesDown);
			}
			else if (Outcome.Body.Num() > 0)
			{
				LogBodyLater(Ex.RequestId, TEXT("<"), Redactor, Outcome.ContentType, Outcome.Body, Outcome.Body.Num());
			}
		}
#endif
	}

	/** Spec 4.5, per attempt: after the custom PerAttempt stages, before the limiter */
	class FLogStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Log"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FVaRestExchange& Ex = *Exchange;
			const EVaRestLogLevel Level = Ex.LogLevel;
			if (Level == EVaRestLogLevel::None)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			const FRedactorRef Redactor = Ex.Config->Redactor.ToSharedRef();
			FString Redacted = Redactor->RedactUrl(Ex.Url);
			if (Level >= EVaRestLogLevel::Headers)
			{
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu > %s %s (attempt %d)"), Ex.RequestId, *Ex.Verb, *Redacted, Ex.Attempt);
				LogHeaders(Ex.RequestId, TEXT(">"), Redactor->RedactHeaders(Ex.AttemptHeaders));
			}
#if !UE_BUILD_SHIPPING
			if (Level >= EVaRestLogLevel::Body)
			{
				LogRequestBody(Ex, Redactor);
			}
#endif
			const double StartedAt = Ex.Config->Clock->Now();
			Next.Proceed(Exchange, [Exchange, Redactor, Line = MoveTemp(Redacted), Level, StartedAt, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				LogOutcome(*Exchange, Redactor, Line, Level, StartedAt, Outcome);
				Done(MoveTemp(Outcome));
			});
		}
	};
}

FVaRestStageRef VaRest::Http::MakeLogStage()
{
	return MakeShared<LogStagePrivate::FLogStage, ESPMode::ThreadSafe>();
}

EVaRestLogLevel VaRest::Http::EffectiveLogLevel(EVaRestLogLevel ClientLevel)
{
	const int32 Forced = LogStagePrivate::CVarLogLevel.GetValueOnAnyThread();
	return Forced < 0 ? ClientLevel : static_cast<EVaRestLogLevel>(FMath::Clamp(Forced, 0, 3));
}
```

- [ ] **Step 6: The rig stays quiet** — in `Source/VaRestHttp/Private/VaRestHttpTesting.cpp`, make the rig's constructor body:

```cpp
	// Logging is a test subject of its own (VaRest.Http.Log.*); elsewhere it would only add noise and warnings
	Client.WithTransport(Fake).WithClock(Clock).WithLogLevel(EVaRestLogLevel::None);
```

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including the six `VaRest.Http.Log.*` tests; the earlier suites stay green.

- [ ] **Step 8: Restore-the-bug (redaction at logging, Inspector R1)**

In `FLogStage::Process`, temporarily log the raw headers: `LogHeaders(Ex.RequestId, TEXT(">"), Ex.AttemptHeaders);`. Run `Tools/run_tests.sh VaRest.Http.Log`; expected FAIL in `VaRest.Http.Log.Canaries` (a `leaked:` line with the Authorization header) and `.Levels` (">   Authorization: ***"). Restore; PASS. Report both runs.

- [ ] **Step 9: Restore-the-bug (request ids, IN-16)**

In `LogHeaders`, temporarily drop the id: `UE_LOG(LogVaRestHttp, Log, TEXT("%s   %s: %s"), Direction, *Pair.Key, *Pair.Value);`. Run `Tools/run_tests.sh VaRest.Http.Log`; expected FAIL in `VaRest.Http.Log.RequestIdOnEveryLine`. Restore; PASS.

- [ ] **Step 10: Commit**

```bash
git add Source/VaRestHttp/Private/VaRestLogStage.cpp Source/VaRestHttp/Private/VaRestClientState.h Source/VaRestHttp/Private/VaRestClientState.cpp \
  Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Public/VaRestClient.h Source/VaRestHttp/Private/VaRestClient.cpp \
  Source/VaRestHttp/Private/VaRestHttpTesting.cpp Source/VaRestHttp/Private/Tests/VaRestHttpLogTest.cpp
git commit -m "http: logging (spec 4.5) -- levels, cvar, #id on every line, redacted URLs/headers, body excerpts on the redaction pipe, secret values

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 15: Trace emission — the four request events, clients, the snapshot, the initiator (Inspector §9: IN-1, 2, 3, 5, 7, 8, 10, 11, 12, 15)

Inspector §9.1 is the contract; Task 4 built the types and the registry, this task fires them. "A typical request produces 4 events (`Started`, `AttemptDispatched`, `AttemptEnded`, `Completed`)" (Inspector §2.1), each on the game thread: `Started` when the request enters the pipeline, after `Resolve` (so a sink sees the URL and headers it goes out with); `AttemptDispatched` "immediately before `ProcessRequest`", in the transport stage; `AttemptEnded` "when the outcome re-enters the stage chain", in `OnAttemptDone`; `Completed` "before any callback is dispatched (including `.OnHttpThread()` callbacks)", in `Complete`. "HTTP thread: emits nothing; it writes timestamps into the exchange (IN-3)": the transport's probe (Task 7) holds them, and the delegates that write them are bound only when a sink wants `Metadata` at dispatch (IN-11). Every trace point starts with the one relaxed load and builds nothing when it is 0; the initiator (IN-15) and `StartUtc` are read inside that gate, because "clock reads and payloads depend on the mask" (IN-10). Clients emit `Client{Created}` at `Make` and `Client{Destroyed}` when the last reference goes; a state can die on a worker, so that event is sent from a game-thread task. A rename (`WithName`) emits `Created` again for the same id (decision 23). The module registers the snapshot contributor: every live client and every in-flight request as `Started{Partial}` (IN-12). M4 adds `Note`, `Excerpt`, `Marker` and the IN-17 goldens; the Blueprint frame of IN-15 gets its test in M5, when Blueprint nodes call `Send` (M3 has no Blueprint caller, and `FBlueprintContextTracker`'s stack is private to the VM).

**Files:**
- Create: `Source/VaRestHttp/Private/VaRestTracePoints.h`, `Source/VaRestHttp/Private/VaRestTracePoints.cpp`
- Modify: `Source/VaRestHttp/Public/VaRestStage.h` (`FVaRestExchange::bSentOnGameThread`)
- Modify: `Source/VaRestHttp/Private/VaRestClient.cpp` (`SendRequest` sets it)
- Modify: `Source/VaRestHttp/Private/VaRestPipeline.cpp` (`Enter`, `FTransportStage::Process`, `OnAttemptDone`, `Complete`)
- Modify: `Source/VaRestHttp/Private/VaRestClientState.h`, `Source/VaRestHttp/Private/VaRestClientState.cpp` (`ForEachInFlight`; the client events)
- Modify: `Source/VaRestHttp/Private/VaRestHttpModule.cpp` (the snapshot contributor)
- Test: `Source/VaRestHttp/Private/Tests/VaRestHttpTraceTest.cpp`

**Interfaces:**
- Consumes: `FVaRestTrace`, the payload structs, `IVaRestTraceSink`, `FVaRestTrace::FScopedSinkIsolation`, `FVaRestTrace::Counters()` (Task 4); `FVaRestRedactor` (Task 5); `FVaRestTransportProbe`, `FVaRestTransportRequest::bReportStatusTime`, `FVaRestTransportResult::TransportDoneAt` (Task 6); the pipeline (Tasks 8, 10, 11, 13).
- Produces: private `VaRest::Http::TracePoints::Started(FVaRestExchange&)`, `AttemptDispatched(const FVaRestExchange&)`, `AttemptEnded(const FVaRestExchange&, const FVaRestTransportResult&)`, `Completed(const FVaRestExchange&, const FFinish&)`, `Client(uint64 ClientId, const FClientConfig&, bool bCreated)`, `ContributeSnapshot(FVaRestTraceSnapshot&)`; `FClientState::ForEachInFlight(TFunctionRef<void(FVaRestExchange&)>) const`; `FVaRestExchange::bSentOnGameThread`.

- [ ] **Step 1: Write the failing tests** — `Source/VaRestHttp/Private/Tests/VaRestHttpTraceTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeLock.h"
#include "UObject/Package.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"
#include "VaRestTrace.h"

namespace VaRest::Http::TraceTestPrivate
{
	uint64 RequestIdOf(const FVaRestTraceEvent& Event)
	{
		switch (Event.Kind)
		{
		case EVaRestTraceKind::Started: return Event.Payload.Get<FVaRestTraceStarted>().RequestId;
		case EVaRestTraceKind::AttemptDispatched: return Event.Payload.Get<FVaRestTraceAttemptDispatched>().RequestId;
		case EVaRestTraceKind::AttemptEnded: return Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId;
		case EVaRestTraceKind::Completed: return Event.Payload.Get<FVaRestTraceCompleted>().RequestId;
		default: return 0;
		}
	}

	FString KindsOf(const TArray<FVaRestTraceEvent>& Events)
	{
		FString Out;
		for (const FVaRestTraceEvent& Event : Events)
		{
			if (!Out.IsEmpty())
			{
				Out += TEXT(",");
			}
			switch (Event.Kind)
			{
			case EVaRestTraceKind::Started: Out += TEXT("Started"); break;
			case EVaRestTraceKind::AttemptDispatched: Out += TEXT("Dispatched"); break;
			case EVaRestTraceKind::AttemptEnded: Out += TEXT("Ended"); break;
			case EVaRestTraceKind::Completed: Out += TEXT("Completed"); break;
			default: Out += TEXT("Other"); break;
			}
		}
		return Out;
	}

	class FRecordingSink final : public IVaRestTraceSink
	{
	public:
		explicit FRecordingSink(EVaRestTraceDetail InFlags)
		{
			Detail.Flags = InFlags;
		}

		virtual FVaRestTraceDetailSettings GetDetail() const override { return Detail; }
		virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override { Snapshots.Add(Snapshot); }
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (!IsInGameThread())
			{
				bAllOnGameThread = false;
			}
			if (Hook)
			{
				Hook(Event);
			}
			FScopeLock Lock(&Mutex);
			Events.Add(Event);
		}

		/** One request's events, in arrival (= Seq) order */
		TArray<FVaRestTraceEvent> For(uint64 RequestId) const
		{
			FScopeLock Lock(&Mutex);
			return Events.FilterByPredicate([RequestId](const FVaRestTraceEvent& Event) { return RequestIdOf(Event) == RequestId; });
		}

		TArray<FVaRestTraceClient> ClientEvents(uint64 ClientId) const
		{
			FScopeLock Lock(&Mutex);
			TArray<FVaRestTraceClient> Out;
			for (const FVaRestTraceEvent& Event : Events)
			{
				if (Event.Kind == EVaRestTraceKind::Client && Event.Payload.Get<FVaRestTraceClient>().ClientId == ClientId)
				{
					Out.Add(Event.Payload.Get<FVaRestTraceClient>());
				}
			}
			return Out;
		}

		FVaRestTraceDetailSettings Detail;
		TArray<FVaRestTraceSnapshot> Snapshots;
		/** Runs inside OnEvent, before the event is stored */
		TFunction<void(const FVaRestTraceEvent&)> Hook;
		std::atomic<bool> bAllOnGameThread{ true };
		mutable FCriticalSection Mutex;
		TArray<FVaRestTraceEvent> Events;
	};

	using FSinkRef = TSharedRef<FRecordingSink, ESPMode::ThreadSafe>;

	FSinkRef MakeSink(EVaRestTraceDetail Flags)
	{
		return MakeShared<FRecordingSink, ESPMode::ThreadSafe>(Flags);
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceSuccessTest, "VaRest.Http.Trace.Success",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceSuccessTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	Rig.Clock->Advance(5.0);

	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Post(TEXT("/items/{id}?token=abc"))
		.Path(TEXT("id"), TEXT("7"))
		.Header(TEXT("Authorization"), TEXT("Bearer abc"))
		.Body(FVaRestJson::Object().Set(TEXT("n"), TEXT("v")))
		.Tag(TEXT("inventory"))
		.Priority(EVaRestPriority::High)
		.Idempotent()
		.Owner(Owner), Outcome).Send();
	Rig.Wait(Outcome);
	FVaRestTrace::RemoveSink(Sink);

	const TArray<FVaRestTraceEvent> Events = Sink->For(Handle.GetId());
	TestEqualSensitive(TEXT("a typical request is four events (Inspector 2.1)"), KindsOf(Events), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Events.Num() != 4)
	{
		return false;
	}
	TestTrue(TEXT("Seq increases"), Events[0].Seq < Events[1].Seq && Events[1].Seq < Events[2].Seq && Events[2].Seq < Events[3].Seq);
	TestTrue(TEXT("every event on the game thread"), Sink->bAllOnGameThread.load());

	const FVaRestTraceStarted& Started = Events[0].Payload.Get<FVaRestTraceStarted>();
	TestEqual(TEXT("IN-1: the client"), Started.ClientId, Rig.Client.GetId());
	TestEqual(TEXT("... scope 0"), Started.ScopeId, FVaRestTrace::ProcessScopeId);
	TestEqualSensitive(TEXT("... the verb"), Started.Verb, FString(TEXT("POST")));
	TestEqualSensitive(TEXT("... the URL, redacted"), Started.Url, FString(TEXT("https://api.test/items/7?token=***")));
	TestEqualSensitive(TEXT("... the template before substitution, without the query"), Started.PathTemplate, FString(TEXT("/items/{id}")));
	TestTrue(TEXT("... tag, priority, the idempotent flag"), Started.Tag == FName(TEXT("inventory")) && Started.Priority == EVaRestPriority::High
		&& EnumHasAnyFlags(Started.Flags, EVaRestTraceRequestFlags::Idempotent));
	TestEqualSensitive(TEXT("... the headers as built, redacted"), Started.Headers.Get(TEXT("Authorization")), FString(TEXT("***")));
	TestTrue(TEXT("... the body's type, size and kind"), Started.Body.Kind == EVaRestBodyKind::Json && Started.Body.Bytes == 9
		&& Started.Body.ContentType.StartsWith(TEXT("application/json")));
	TestTrue(TEXT("IN-15: the C++ call site"), Started.Initiator.SourceFile.Contains(TEXT("VaRestHttpTraceTest.cpp")) && Started.Initiator.SourceLine > 0);
	TestEqualSensitive(TEXT("IN-15: the owner's path"), Started.Initiator.OwnerPath, Owner->GetPathName());
	TestTrue(TEXT("... no Blueprint frame for a C++ caller"), Started.Initiator.ScriptFunction.IsEmpty() && Started.Initiator.ScriptCodeOffset == INDEX_NONE);
	TestTrue(TEXT("... StartUtc is set"), Started.StartUtc.GetTicks() > 0);
	TestEqual(TEXT("Time is the client's clock"), Events[0].Time, 5.0);

	const FVaRestTraceAttemptDispatched& Dispatched = Events[1].Payload.Get<FVaRestTraceAttemptDispatched>();
	TestTrue(TEXT("IN-2: attempt 1, its start, no limiter wait"), Dispatched.Attempt == 1 && Dispatched.AttemptStart == 5.0 && Dispatched.LimiterWait == 0.0);
	TestEqualSensitive(TEXT("... the final headers, redacted"), Dispatched.Headers.Get(TEXT("Authorization")), FString(TEXT("***")));
	TestTrue(TEXT("... with the idempotency key, which is not a secret"), Dispatched.Headers.Contains(TEXT("Idempotency-Key"))
		&& !Dispatched.Headers.Get(TEXT("Idempotency-Key")).Equals(TEXT("***")));

	const FVaRestTraceAttemptEnded& Ended = Events[2].Payload.Get<FVaRestTraceAttemptEnded>();
	TestTrue(TEXT("IN-3: status, sizes, no failure"), Ended.Attempt == 1 && Ended.Status == 200 && Ended.BytesDown == 2 && Ended.Failure == EVaRestTransportFailure::None);
	TestEqualSensitive(TEXT("... the effective URL, redacted"), Ended.EffectiveUrl, FString(TEXT("https://api.test/items/7?token=***")));
	TestEqualSensitive(TEXT("... the response headers"), Ended.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	TestTrue(TEXT("... the transport's timestamps (delegates bound under IN-11)"), Ended.UploadDoneAt >= 0.0 && Ended.StatusAt >= 0.0 && Ended.TransportDoneAt >= 0.0);

	const FVaRestTraceCompleted& Completed = Events[3].Payload.Get<FVaRestTraceCompleted>();
	TestTrue(TEXT("IN-5: success"), Completed.Outcome == EVaRestTraceOutcome::Success && Completed.Error.Kind == EVaRestErrorKind::None);
	TestEqual(TEXT("... the result type"), Completed.ResultType, FName(TEXT("Json")));
	TestTrue(TEXT("... decode times, read because a sink is attached"), Completed.DecodeStartAt >= 0.0 && Completed.DecodeEndAt >= Completed.DecodeStartAt);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceFailuresTest, "VaRest.Http.Trace.Failures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceFailuresTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FTestRig Rig;
	Rig.Client.SetSecretValue(TEXT("bearer"), TEXT("secret-value-123456"));

	Rig.Fake->Enqueue(FVaRestFakeResponse::Json(404, FUtf8StringView(UTF8TEXT("{\"error\":\"nope\"}"))));
	auto NotFound = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Missing = Observe(Rig.Client.Get(TEXT("/missing")), NotFound).Send();
	Rig.Wait(NotFound);

	auto Broken = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Bad = Observe(Rig.Client.Get(TEXT("/u/secret-value-123456/{id")), Broken).Send();
	Rig.Wait(Broken);

	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Stopped = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Canceled = Observe(Rig.Client.Get(TEXT("/slow")), Stopped).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; });
	Canceled.Cancel();
	Rig.Wait(Stopped);
	FVaRestTrace::RemoveSink(Sink);

	const TArray<FVaRestTraceEvent> Http = Sink->For(Missing.GetId());
	TestEqualSensitive(TEXT("an HTTP error is four events"), KindsOf(Http), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Http.Num() == 4)
	{
		const FVaRestTraceCompleted& Done = Http[3].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Error, Http 404}"), Http[2].Payload.Get<FVaRestTraceAttemptEnded>().Status == 404
			&& Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Http && Done.Error.HttpStatus == 404);
	}

	const TArray<FVaRestTraceEvent> Config = Sink->For(Bad.GetId());
	TestEqualSensitive(TEXT("a request that never makes an attempt: Started, Completed"), KindsOf(Config), FString(TEXT("Started,Completed")));
	if (Config.Num() == 2)
	{
		const FVaRestTraceCompleted& Done = Config[1].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... Completed{Error, Config}"), Done.Outcome == EVaRestTraceOutcome::Error && Done.Error.Kind == EVaRestErrorKind::Config);
		TestTrue(TEXT("... its message value-scrubbed (Inspector 2.2: FVaRestError Message)"), Done.Error.Message.Contains(TEXT("***"))
			&& !Done.Error.Message.Contains(TEXT("secret-value-123456")));
		TestFalse(TEXT("... and the URL of Started too"), Config[0].Payload.Get<FVaRestTraceStarted>().Url.Contains(TEXT("secret-value-123456")));
	}

	const TArray<FVaRestTraceEvent> Cancel = Sink->For(Canceled.GetId());
	TestEqualSensitive(TEXT("a cancel in the transport is four events"), KindsOf(Cancel), FString(TEXT("Started,Dispatched,Ended,Completed")));
	if (Cancel.Num() == 4)
	{
		const FVaRestTraceCompleted& Done = Cancel[3].Payload.Get<FVaRestTraceCompleted>();
		TestTrue(TEXT("... AttemptEnded{Canceled}"), Cancel[2].Payload.Get<FVaRestTraceAttemptEnded>().Failure == EVaRestTransportFailure::Canceled);
		TestTrue(TEXT("... Completed{Canceled, User}"), Done.Outcome == EVaRestTraceOutcome::Canceled && Done.CancelReason == EVaRestCancelReason::User);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceZeroCostTest, "VaRest.Http.Trace.ZeroCostWhenOff",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceZeroCostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation; // no sink: the mask is 0
	FTestRig Rig;
	// "TraceCounters", not "Counters": Testing::Counters() is in scope (review M18)
	const FVaRestTraceCounters& TraceCounters = FVaRestTrace::Counters();
	// Review M9: the counters are process-wide -- let an earlier test's late tasks (body log lines on the redaction pipe,
	// decode tasks) land before the baseline
	FlushRedactionPipe();
	PumpUntil([] { return false; }, 0.05);
	FVaRestTrace::Counters().Reset();
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	TArray<FVaRestRequestHandle> Handles;
	// Inspector 3.1 item 6 / R3: "over 1000 requests" (review M10); cheap on the fake
	for (int32 i = 0; i < 1000; ++i)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Handles.Add(Observe(Rig.Client.Get(TEXT("/n?token=x")).Header(TEXT("Authorization"), TEXT("Bearer x")), Outcomes.Last()).Send());
	}
	TestTrue(TEXT("all complete"), PumpUntil([&Outcomes] { return !Outcomes.ContainsByPredicate([](const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Each) { return !Each->IsDone(); }); }, 30.0));
	// Inspector 3.1 item 6: counted, not timed
	TestEqual(TEXT("IN-11: no event is built without a sink"), TraceCounters.EventsBuilt.load(), static_cast<uint64>(0));
	TestEqual(TEXT("... no redactor call"), TraceCounters.RedactorCalls.load(), static_cast<uint64>(0));
	TestEqual(TEXT("... no transport delegate bound"), TraceCounters.DelegateBinds.load(), static_cast<uint64>(0));
	TestEqual(TEXT("... no trace-point cycles"), TraceCounters.TraceCycles.load(), static_cast<uint64>(0));

	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	FVaRestTrace::Counters().Reset();
	auto One = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Traced = Observe(Rig.Client.Get(TEXT("/n")).Header(TEXT("Authorization"), TEXT("Bearer x")), One).Send();
	Rig.Wait(One);
	FVaRestTrace::RemoveSink(Sink);
	// Review M9: only this request's events -- a late Client{Destroyed} from an earlier rig may count in EventsBuilt
	const TArray<FVaRestTraceEvent> Events = Sink->For(Traced.GetId());
	TestEqual(TEXT("with a sink: four events for the request"), Events.Num(), 4);
	TestEqual(TEXT("... the two timing delegates bound for the one attempt (IN-11)"), TraceCounters.DelegateBinds.load(), static_cast<uint64>(2));
	TestTrue(TEXT("Metadata alone builds no headers"), Events.Num() == 4 && Events[0].Payload.Get<FVaRestTraceStarted>().Headers.IsEmpty()
		&& Events[1].Payload.Get<FVaRestTraceAttemptDispatched>().Headers.IsEmpty() && Events[2].Payload.Get<FVaRestTraceAttemptEnded>().Headers.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceLateAttachTest, "VaRest.Http.Trace.LateAttach",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceLateAttachTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/slow?token=abc")), Outcome).Send();
	PumpUntil([&Rig] { return Rig.Fake->NumInFlight() == 1; });

	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	TestEqual(TEXT("one snapshot, at AddSink"), Sink->Snapshots.Num(), 1);
	if (Sink->Snapshots.Num() == 1)
	{
		const FVaRestTraceSnapshot& Snapshot = Sink->Snapshots[0];
		const uint64 ClientId = Rig.Client.GetId();
		const uint64 RequestId = Handle.GetId();
		const FVaRestTraceClient* Client = Snapshot.Clients.FindByPredicate([ClientId](const FVaRestTraceClient& Each) { return Each.ClientId == ClientId; });
		TestTrue(TEXT("IN-12: the live client"), Client && Client->DisplayName == TEXT("https://api.test") && Client->ScopeId == FVaRestTrace::ProcessScopeId);
		const FVaRestTraceStarted* Entry = Snapshot.InFlight.FindByPredicate([RequestId](const FVaRestTraceStarted& Each) { return Each.RequestId == RequestId; });
		TestTrue(TEXT("... the in-flight request as Started{Partial}"), Entry && EnumHasAnyFlags(Entry->Flags, EVaRestTraceRequestFlags::Partial));
		TestTrue(TEXT("... with its verb, redacted URL, template, attempt and start time (Inspector 2.3)"), Entry && Entry->Verb == TEXT("GET")
			&& Entry->Url.Equals(TEXT("https://api.test/slow?token=***"), ESearchCase::CaseSensitive) && Entry->PathTemplate == TEXT("/slow")
			&& Entry->CurrentAttempt == 1 && Entry->StartedAt >= 0.0);
	}
	Rig.Fake->Complete(0, FVaRestFakeResponse::Json(200, FUtf8StringView(UTF8TEXT("{}"))));
	Rig.Wait(Outcome);
	FVaRestTrace::RemoveSink(Sink);
	TestEqualSensitive(TEXT("after the attach, the rest of its events"), KindsOf(Sink->For(Handle.GetId())), FString(TEXT("Ended,Completed")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceThreadTest, "VaRest.Http.Trace.GameThreadOnly",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceThreadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FTestRig Rig;
	Rig.Fake->bCompleteOnWorker = true; // as FHttpModule does with CompleteOnHttpThread
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Callbacks = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> CompletedFirst = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	Sink->Hook = [Callbacks, CompletedFirst](const FVaRestTraceEvent& Event)
	{
		if (Event.Kind == EVaRestTraceKind::Completed && Callbacks->load() == 0)
		{
			++*CompletedFirst;
		}
	};
	FVaRestTrace::AddSink(Sink);

	const FVaRestRequestHandle OnGame = Rig.Client.Get(TEXT("/a")).OnComplete([Callbacks](const TVaRestResult<FVaRestJson>&) { ++*Callbacks; }).Send();
	PumpUntil([Callbacks] { return Callbacks->load() == 1; });
	Callbacks->store(0);
	const FVaRestRequestHandle OffGame = Rig.Client.Get(TEXT("/b")).OnHttpThread().OnComplete([Callbacks](const TVaRestResult<FVaRestJson>&) { ++*Callbacks; }).Send();
	PumpUntil([Callbacks] { return Callbacks->load() == 1; });
	FVaRestTrace::RemoveSink(Sink);

	TestEqual(TEXT("IN-5: Completed before any callback, OnHttpThread ones included"), CompletedFirst->load(), 2);
	TestTrue(TEXT("Inspector 9: nothing is emitted off the game thread, though the transport completes on a worker"), Sink->bAllOnGameThread.load());
	TestEqual(TEXT("four events each"), Sink->For(OnGame.GetId()).Num() + Sink->For(OffGame.GetId()).Num(), 8);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceClientsTest, "VaRest.Http.Trace.Clients",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceClientsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TraceTestPrivate;
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	FVaRestTrace::AddSink(Sink);
	uint64 Id = 0;
	{
		FVaRestClient Client = FVaRestClient::Make(TEXT("https://user:pw@temp.test"));
		Id = Client.GetId();
		Client.WithName(TEXT("Temp"));
		Client.WithName(TEXT("Temp")); // unchanged: no event
	}
	FVaRestTrace::RemoveSink(Sink);
	const TArray<FVaRestTraceClient> Seen = Sink->ClientEvents(Id);
	TestEqual(TEXT("created, renamed, destroyed"), Seen.Num(), 3);
	if (Seen.Num() != 3)
	{
		return false;
	}
	TestTrue(TEXT("IN-8: Created, named by the base URL, redacted"), Seen[0].bCreated && Seen[0].DisplayName.Equals(TEXT("https://***@temp.test"), ESearchCase::CaseSensitive)
		&& Seen[0].ScopeId == FVaRestTrace::ProcessScopeId);
	TestTrue(TEXT("decision 23: a rename is Created again for the same id"), Seen[1].bCreated && Seen[1].DisplayName == TEXT("Temp"));
	TestTrue(TEXT("Destroyed when the last reference goes"), !Seen[2].bCreated);
	TestTrue(TEXT("on the game thread"), Sink->bAllOnGameThread.load());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTraceGhostTest, "VaRest.Http.Trace.SnapshotNoGhost",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTraceGhostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::TraceTestPrivate;
	// Review M-9: the snapshot's walk pins every live state. When a pin is a state's last reference, the state must die
	// after AddSink added the sink, so its Client{Destroyed} reaches it (else the sink keeps a ghost client)
	FVaRestTrace::FScopedSinkIsolation Isolation;
	FVaRestClient Doomed = FVaRestClient::Make(TEXT("https://ghost.test"));
	const uint64 Id = Doomed.GetId();
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Held = Doomed.GetStateInternal();
	const TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Weak = Held;
	Doomed = FVaRestClient();
	VaRest::Http::FClientState::ForEachLive([&Held, Id](VaRest::Http::FClientState& State)
	{
		if (State.GetClientId() == Id)
		{
			Held.Reset(); // the walk's pin is now the last reference
		}
	});
	TestTrue(TEXT("the walk's pin outlives the walk"), Weak.IsValid());
	PumpUntil([&Weak] { return !Weak.IsValid(); }, 1.0);
	TestFalse(TEXT("... and is dropped from a later game-thread task"), Weak.IsValid());

	FVaRestClient Second = FVaRestClient::Make(TEXT("https://ghost2.test"));
	const uint64 SecondId = Second.GetId();
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> SecondHeld = Second.GetStateInternal();
	Second = FVaRestClient();
	const FSinkRef Sink = MakeSink(EVaRestTraceDetail::Metadata);
	// Runs inside the snapshot (contributors are called in reverse order, MulticastDelegateBase.h:299-300, so after the
	// core's walk took its pin only if registered first -- either way the state must not be a ghost)
	const FDelegateHandle Dropper = FVaRestTrace::OnBuildSnapshot().AddLambda([&SecondHeld](FVaRestTraceSnapshot&) { SecondHeld.Reset(); });
	FVaRestTrace::AddSink(Sink);
	FVaRestTrace::OnBuildSnapshot().Remove(Dropper);
	PumpUntil([] { return false; }, 0.05);
	FVaRestTrace::RemoveSink(Sink);
	const bool bInSnapshot = Sink->Snapshots.Num() == 1 && Sink->Snapshots[0].Clients.ContainsByPredicate([SecondId](const FVaRestTraceClient& Each) { return Each.ClientId == SecondId; });
	const bool bDestroyedSeen = Sink->ClientEvents(SecondId).ContainsByPredicate([](const FVaRestTraceClient& Each) { return !Each.bCreated; });
	TestTrue(TEXT("a client the snapshot listed is destroyed for the sink too: no ghost"), !bInSnapshot || bDestroyedSeen);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Http.Trace.`
Expected: it builds (the tests use Task 4's surface and Task 8's state only) and the seven `VaRest.Http.Trace.*` tests FAIL (`SnapshotNoGhost`'s first half passes already: Task 8 defers the pins): the sink receives no request or client events ("a typical request is four events": `""`), and the snapshot has no client.

- [ ] **Step 3: The trace points** — `Source/VaRestHttp/Private/VaRestTracePoints.h`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestTrace.h"

class FVaRestExchange;
struct FVaRestTransportResult;

namespace VaRest::Http
{
	struct FClientConfig;
	struct FFinish;

	/**
	 * Inspector 9.1's trace points. Game thread. Each starts with the one relaxed load of the mask and builds nothing
	 * when it is 0 (IN-11); every string in a payload comes through the client's redactor (Inspector 2.2, ruling R1).
	 */
	namespace TracePoints
	{
		/** IN-1, on entry after Resolve. Reads the initiator (IN-15) and StartUtc into the exchange for the snapshot. */
		void Started(FVaRestExchange& Ex);
		/** IN-2, immediately before the transport starts */
		void AttemptDispatched(const FVaRestExchange& Ex);
		/** IN-3, when the transport's result is back on the game thread */
		void AttemptEnded(const FVaRestExchange& Ex, const FVaRestTransportResult& Result);
		/** IN-5, before any callback */
		void Completed(const FVaRestExchange& Ex, const FFinish& Finish);
		/** IN-8. Any thread; the event is emitted on the game thread. */
		void Client(uint64 ClientId, const FClientConfig& Config, bool bCreated);
		/** IN-12: every live client and in-flight request; the module registers it with FVaRestTrace::OnBuildSnapshot */
		void ContributeSnapshot(FVaRestTraceSnapshot& Snapshot);
	}
}
```

`Source/VaRestHttp/Private/VaRestTracePoints.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTracePoints.h"

#include "Tasks/Task.h"
#include "UObject/Class.h"
#include "UObject/Script.h"
#include "UObject/Stack.h"
#include "VaRestClientState.h"
#include "VaRestTransport.h"
#include "VaRestUrl.h"

namespace VaRest::Http::TracePointsPrivate
{
	/** A name or a base URL; Make uses the base URL, which may carry userinfo (Inspector 2.2 layer 5) */
	FString DisplayNameOf(const FClientConfig& Config)
	{
		if (!Config.Redactor.IsValid())
		{
			return FString();
		}
		return IsAbsoluteHttpUrl(Config.DisplayName) ? Config.Redactor->RedactUrl(Config.DisplayName) : Config.Redactor->ScrubText(Config.DisplayName);
	}

	/** What Started and a snapshot entry share */
	FVaRestTraceStarted Describe(const FVaRestExchange& Ex)
	{
		const FClientConfig& Cfg = *Ex.Config;
		const FVaRestRedactor& Redactor = *Cfg.Redactor;
		FVaRestTraceStarted Out;
		Out.RequestId = Ex.RequestId;
		Out.ScopeId = FVaRestTrace::ProcessScopeId;
		Out.ClientId = Ex.ClientId;
		Out.Verb = Ex.Verb;
		// A request that failed before Resolve finished has no URL: the base and the raw path say what it was
		Out.Url = Redactor.RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
		Out.PathTemplate = Redactor.RedactUrl(Ex.PathTemplate);
		Out.Tag = Ex.Tag;
		Out.Priority = Ex.Priority;
		if (Ex.bIdempotent)
		{
			Out.Flags |= EVaRestTraceRequestFlags::Idempotent;
		}
		Out.Initiator = Ex.Initiator;
		// The request's own Content-Type wins (spec 3.2); the body may already have moved into the transport (Task 8)
		const FString* Type = Ex.RequestHeaders.Find(TEXT("Content-Type"));
		Out.Body.ContentType = Type ? *Type : Ex.Body.ContentType;
		Out.Body.Bytes = Ex.Body.ContentLength;
		Out.Body.Kind = Ex.Body.Kind;
		Out.StartUtc = Ex.StartUtc;
		Out.CurrentAttempt = Ex.Attempt;
		Out.StartedAt = Ex.StartedAt;
		return Out;
	}

#if DO_BLUEPRINT_GUARD
	/**
	 * IN-15: the top Blueprint frame when Send ran inside one. The tracker is per thread (Script.h:470-479) and its stack
	 * holds the frames being executed (Script.h:509-512). Only for a send on the game thread, inside the call.
	 */
	void ReadScriptFrame(FVaRestTraceInitiator& Who)
	{
		const FBlueprintContextTracker* Tracker = FBlueprintContextTracker::TryGet();
		if (!Tracker)
		{
			return;
		}
		const TArrayView<const FFrame* const> Stack = Tracker->GetCurrentScriptStack();
		if (Stack.Num() == 0 || !Stack.Last())
		{
			return;
		}
		const FFrame& Top = *Stack.Last();
		if (Top.Object)
		{
			Who.ScriptObject = Top.Object->GetPathName();
		}
		if (Top.Node)
		{
			Who.ScriptFunction = Top.Node->GetName();
			const uint8* Begin = Top.Node->Script.GetData();
			if (Top.Code && Begin && Top.Code >= Begin && Top.Code <= Begin + Top.Node->Script.Num())
			{
				Who.ScriptCodeOffset = static_cast<int32>(Top.Code - Begin);
			}
		}
	}
#endif

	void EmitOnGameThread(EVaRestTraceKind Kind, double Time, FVaRestTracePayload&& Payload)
	{
		if (IsInGameThread())
		{
			FVaRestTrace::Emit(Kind, Time, MoveTemp(Payload));
			return;
		}
		// Inspector 9: every kind but Excerpt is emitted on the game thread
		UE::Tasks::Launch(TEXT("VaRest.TraceEmit"), [Kind, Time, Pending = MoveTemp(Payload)]() mutable
		{
			FVaRestTrace::Emit(Kind, Time, MoveTemp(Pending));
		}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
}

void VaRest::Http::TracePoints::Started(FVaRestExchange& Ex)
{
	using namespace TracePointsPrivate;
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	// IN-10: clock reads and the initiator follow the mask
	Ex.StartUtc = FDateTime::UtcNow();
	FVaRestTraceInitiator& Who = Ex.Initiator;
	Who.SourceFile = FString(Ex.SourceFile);
	Who.SourceLine = static_cast<int32>(Ex.SourceLine);
	if (Ex.bHasOwner)
	{
		if (const UObject* Owner = Ex.Owner.Get())
		{
			Who.OwnerPath = Owner->GetPathName();
		}
	}
#if DO_BLUEPRINT_GUARD
	if (Ex.bSentOnGameThread)
	{
		ReadScriptFrame(Who);
	}
#endif
	FVaRestTraceStarted Payload = Describe(Ex);
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = Ex.Config->Redactor->RedactHeaders(Ex.RequestHeaders);
	}
	FVaRestTrace::Emit(EVaRestTraceKind::Started, Ex.StartedAt, FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::AttemptDispatched(const FVaRestExchange& Ex)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	FVaRestTraceAttemptDispatched Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.Attempt = Ex.Attempt;
	Payload.AttemptStart = Ex.AttemptStartedAt;
	Payload.LimiterWait = Ex.LimiterWait;
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = Ex.Config->Redactor->RedactHeaders(Ex.AttemptHeaders);
	}
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptDispatched, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptDispatched>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::AttemptEnded(const FVaRestExchange& Ex, const FVaRestTransportResult& Result)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	const FVaRestRedactor& Redactor = *Ex.Config->Redactor;
	FVaRestTraceAttemptEnded Payload;
	Payload.RequestId = Ex.RequestId;
	Payload.Attempt = Ex.Attempt;
	Payload.Status = Result.Status;
	if (FVaRestTrace::Wants(EVaRestTraceDetail::Headers))
	{
		Payload.Headers = Redactor.RedactHeaders(Result.Headers);
	}
	Payload.EffectiveUrl = Redactor.RedactUrl(Result.EffectiveUrl);
	Payload.ContentType = Result.ContentType;
	Payload.BytesUp = Result.BytesUp;
	Payload.BytesDown = Result.BytesDown;
	Payload.Failure = Result.Failure;
	Payload.FailureDetail = Result.FailureDetail;
	if (Ex.Probe.IsValid())
	{
		// Written on the HTTP thread by the delegates bound for this attempt (IN-3); -1 when they were not bound
		Payload.UploadDoneAt = Ex.Probe->UploadDoneAt.load();
		Payload.StatusAt = Ex.Probe->StatusAt.load();
	}
	Payload.TransportDoneAt = Result.TransportDoneAt;
	FVaRestTrace::Emit(EVaRestTraceKind::AttemptEnded, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceAttemptEnded>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Completed(const FVaRestExchange& Ex, const FFinish& Finish)
{
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	const FVaRestRedactor& Redactor = *Ex.Config->Redactor;
	FVaRestTraceCompleted Payload;
	Payload.RequestId = Ex.RequestId;
	if (Finish.Error.Kind == EVaRestErrorKind::Canceled)
	{
		Payload.Outcome = EVaRestTraceOutcome::Canceled;
		Payload.CancelReason = Ex.CancelReason == EVaRestCancelReason::None ? EVaRestCancelReason::User : Ex.CancelReason;
	}
	else if (Finish.Error.IsError())
	{
		Payload.Outcome = EVaRestTraceOutcome::Error;
	}
	if (Finish.Error.IsError())
	{
		// Inspector 2.2: "FVaRestError: Url redacted, Message value-scrubbed; Body/RawBody are not copied"
		Payload.Error.Kind = Finish.Error.Kind;
		Payload.Error.HttpStatus = Finish.Error.HttpStatus;
		Payload.Error.Code = Redactor.ScrubText(Finish.Error.Code);
		Payload.Error.Message = Redactor.ScrubText(Finish.Error.Message);
	}
	Payload.ResultType = Ex.Handler.IsValid() ? Ex.Handler->GetResultType() : NAME_None;
	Payload.DecodeStartAt = Finish.DecodeStartAt;
	Payload.DecodeEndAt = Finish.DecodeEndAt;
	const TArray<FVaRestJsonError>& Warnings = Finish.Response.DecodeWarnings;
	Payload.DecodeWarningCount = Warnings.Num();
	for (int32 Index = 0; Index < FMath::Min(Warnings.Num(), FVaRestTraceCompleted::MaxDecodeWarnings); ++Index)
	{
		FVaRestTraceDecodeWarning& Warning = Payload.DecodeWarnings.AddDefaulted_GetRef();
		Warning.Path = Warnings[Index].Path;
		Warning.Message = Redactor.ScrubText(Warnings[Index].Message);
	}
	FVaRestTrace::Emit(EVaRestTraceKind::Completed, Ex.Config->Clock->Now(), FVaRestTracePayload(TInPlaceType<FVaRestTraceCompleted>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::Client(uint64 ClientId, const FClientConfig& Config, bool bCreated)
{
	using namespace TracePointsPrivate;
	if (FVaRestTrace::Detail() == EVaRestTraceDetail::None)
	{
		return;
	}
	FVaRestTrace::FScopedCycles Cycles;
	FVaRestTraceClient Payload;
	Payload.bCreated = bCreated;
	Payload.ClientId = ClientId;
	Payload.ScopeId = FVaRestTrace::ProcessScopeId;
	Payload.DisplayName = DisplayNameOf(Config);
	const double Time = Config.Clock.IsValid() ? Config.Clock->Now() : 0.0;
	EmitOnGameThread(EVaRestTraceKind::Client, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceClient>(), MoveTemp(Payload)));
}

void VaRest::Http::TracePoints::ContributeSnapshot(FVaRestTraceSnapshot& Snapshot)
{
	using namespace TracePointsPrivate;
	FClientState::ForEachLive([&Snapshot](FClientState& State)
	{
		const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Config = State.GetConfig();
		FVaRestTraceClient& Entry = Snapshot.Clients.AddDefaulted_GetRef();
		Entry.ClientId = State.GetClientId();
		Entry.ScopeId = FVaRestTrace::ProcessScopeId;
		Entry.DisplayName = DisplayNameOf(*Config);
		State.ForEachInFlight([&Snapshot](FVaRestExchange& Ex)
		{
			if (Ex.Phase == EVaRestExchangePhase::Done || !Ex.Config.IsValid())
			{
				return;
			}
			FVaRestTraceStarted Partial = Describe(Ex);
			Partial.Flags |= EVaRestTraceRequestFlags::Partial;
			Snapshot.InFlight.Add(MoveTemp(Partial));
		});
	});
}
```

- [ ] **Step 4: The exchange knows where it was sent from** — in `Source/VaRestHttp/Public/VaRestStage.h`, after `uint32 SourceLine = 0;` in `FVaRestExchange`, add:

```cpp
	/** Send ran on the game thread, so Enter runs inside it and the Blueprint stack is the caller's (IN-15) */
	bool bSentOnGameThread = false;
```

In `Source/VaRestHttp/Private/VaRestClient.cpp`, in `SendRequest`, make the game-thread branch:

```cpp
	if (IsInGameThread())
	{
		Ex.bSentOnGameThread = true;
		Ex.State->Enter(Exchange);
	}
```

- [ ] **Step 5: Wire the request events** — in `Source/VaRestHttp/Private/VaRestPipeline.cpp`, add `#include "VaRestTracePoints.h"`, then:

1. Replace the whole `FClientState::Enter` (its Task 8 body plus Task 11's shutdown and owner checks) with:

```cpp
void VaRest::Http::FClientState::Enter(const FVaRestExchangeRef& Exchange)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FVaRestExchange& Ex = *Exchange;
	FApiScope Scope(Ex);
	// Spec 3.1: "A request snapshots the configuration at Send()" -- SendRequest took it (decision 27); an exchange made
	// any other way gets one here
	if (!Ex.Config.IsValid())
	{
		Ex.Config = GetConfig();
	}
	Ex.StartedAt = Ex.Config->Clock->Now();
	Ex.Phase = EVaRestExchangePhase::InChain;
	InFlight.Add(Ex.RequestId, Exchange);
	if (IsShuttingDown())
	{
		// Review M-14: after OnEnginePreExit; the HTTP module unloads next (LaunchEngineLoop.cpp:5182)
		Ex.CancelReason = EVaRestCancelReason::Shutdown;
		Ex.bCancelRequested = true;
	}
	if (Ex.IsCancelRequested() || IsOwnerGone(Ex))
	{
		// Canceled before it reached the game thread, its owner is already gone, or the engine is exiting: no transport call
		TracePoints::Started(Ex);
		Complete(Exchange, MakeFinish(Ex));
		return;
	}

	FString Problem;
	const bool bResolved = Ex.BuildError.IsEmpty() && Resolve(Ex, Problem);
	// IN-1, after Resolve: a sink sees the URL and the headers the request goes out with
	TracePoints::Started(Ex);
	if (!bResolved)
	{
		FFinish Finish = MakeFinish(Ex);
		Finish.Error.Kind = EVaRestErrorKind::Config;
		Finish.Error.Message = Ex.BuildError.IsEmpty() ? MoveTemp(Problem) : Ex.BuildError;
		Complete(Exchange, MoveTemp(Finish));
		return;
	}
	FVaRestStageNext(Ex.Config->Chain.ToSharedRef(), 0).Proceed(Exchange, [Exchange](FVaRestAttemptOutcome&& Outcome)
	{
		Exchange->State->OnChainDone(Exchange, MoveTemp(Outcome));
	});
}
```

2. In `FTransportStage::Process`, after Task 13's `const bool bWantsProgress = Request.bReportProgress;` and before `Ex.TransportCall = Cfg.Transport->Start(...)`, add:

```cpp
			// IN-11: the transport's timing delegates are bound only when a sink wants metadata at dispatch.
			// UploadDoneAt is stamped by the progress delegate (Task 7); bWantsProgress above keeps the callback timer off.
			if (FVaRestTrace::Wants(EVaRestTraceDetail::Metadata))
			{
				Request.bReportStatusTime = true;
				Request.bReportProgress = true;
			}
			// IN-2: on the game thread, immediately before the transport starts
			TracePoints::AttemptDispatched(Ex);
```

3. In `FClientState::OnAttemptDone`, after `Ex.TransportCall.Reset();` and Task 11's owner check, add:

```cpp
	if (!Ex.bFinished)
	{
		// IN-3, "when the outcome re-enters the stage chain". After Completed (a cancel finished the request while the
		// answer was queued) the record is sealed (Inspector 2.3), so a late attempt is not reported.
		TracePoints::AttemptEnded(Ex, Result);
	}
```

4. In `FClientState::Complete`, insert right before `TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);`:

```cpp
	// IN-5: before any callback, OnHttpThread ones included, and also when none will run (owner gone, shutdown)
	TracePoints::Completed(Ex, Finish);
```

- [ ] **Step 6: Client events and the in-flight walk** — in `Source/VaRestHttp/Private/VaRestClientState.h`, add to the public section of `FClientState`:

```cpp
		/** Game thread: the trace snapshot (IN-12) */
		void ForEachInFlight(TFunctionRef<void(FVaRestExchange&)> Visit) const
		{
			for (const TPair<uint64, FVaRestExchangeRef>& Pair : InFlight)
			{
				Visit(*Pair.Value);
			}
		}
```

In `Source/VaRestHttp/Private/VaRestClientState.cpp`, add `#include "VaRestTracePoints.h"`, make the destructor and `EditConfig`:

```cpp
VaRest::Http::FClientState::~FClientState()
{
	// IN-8. Off the game thread only when a game's own thread dropped the last FVaRestClient (the core hands its
	// references to the game thread, review I-1); the event still goes out on the game thread.
	TracePoints::Client(ClientId, *Config, false);
	if (!IsInGameThread())
	{
#if !UE_BUILD_SHIPPING
		++Testing::Counters().ClientStatesDestroyedOffGameThread;
#endif
		// Review I-1 backstop: the configuration (custom stages, transport, clock) dies on the game thread
		UE::Tasks::Launch(TEXT("VaRest.ReleaseConfig"), [Doomed = Config] {}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	ClientStatePrivate::FLiveRegistry& Registry = ClientStatePrivate::Live();
	FScopeLock Lock(&Registry.Lock);
	Registry.States.RemoveAll([this](const TWeakPtr<FClientState, ESPMode::ThreadSafe>& Weak) { return !Weak.IsValid() || Weak.HasSameObject(this); });
}

void VaRest::Http::FClientState::EditConfig(TFunctionRef<void(FClientConfig&)> Edit)
{
	check(IsInGameThread());
	const TSharedRef<const FClientConfig, ESPMode::ThreadSafe> Previous = GetConfig();
	TSharedRef<FClientConfig, ESPMode::ThreadSafe> Next = MakeShared<FClientConfig, ESPMode::ThreadSafe>(*Previous);
	Edit(*Next);
	Publish(Next);
	if (!Next->DisplayName.Equals(Previous->DisplayName, ESearchCase::CaseSensitive))
	{
		// Decision 23: IN-8 has Created and Destroyed only; a rename is Created again for the same id (a store updates the row)
		TracePoints::Client(ClientId, *Next, true);
	}
}
```

and in `FClientState::Create`, after `Created->EditConfig([](FClientConfig&) {});`:

```cpp
	TracePoints::Client(Created->GetClientId(), *Created->GetConfig(), true); // IN-8
```

- [ ] **Step 7: The snapshot contributor** — in `Source/VaRestHttp/Private/VaRestHttpModule.cpp`, add `#include "VaRestTracePoints.h"`, a member `FDelegateHandle SnapshotHandle;`, and in `StartupModule` (after the pre-exit hook):

```cpp
		// IN-12: a sink that attaches late learns the live clients and in-flight requests
		SnapshotHandle = FVaRestTrace::OnBuildSnapshot().AddStatic(&VaRest::Http::TracePoints::ContributeSnapshot);
```

and first in `ShutdownModule`:

```cpp
		FVaRestTrace::OnBuildSnapshot().Remove(SnapshotHandle);
```

- [ ] **Step 8: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http`
Expected: `0 failed`, including `VaRest.Http.Trace.Success`, `.Failures`, `.ZeroCostWhenOff`, `.LateAttach`, `.GameThreadOnly`, `.Clients`, `.SnapshotNoGhost`; the earlier suites stay green (`VaRest.Http.TraceSurface.*` included).

- [ ] **Step 9: Restore-the-bug (redaction at capture, Inspector R1)**

In `TracePoints::AttemptDispatched`, temporarily use the raw headers: `Payload.Headers = Ex.AttemptHeaders;`. Run `Tools/run_tests.sh VaRest.Http.Trace`; expected FAIL in `VaRest.Http.Trace.Success` ("the final headers, redacted"). Restore; PASS. Report both runs.

- [ ] **Step 10: Restore-the-bug (zero cost when off, IN-11)**

In `TracePoints::Started`, temporarily delete the `if (FVaRestTrace::Detail() == EVaRestTraceDetail::None) { return; }` gate. Run `Tools/run_tests.sh VaRest.Http.Trace`; expected FAIL in `VaRest.Http.Trace.ZeroCostWhenOff` ("no redactor call", "no trace-point cycles"). Restore; PASS. Then in `FTransportStage::Process` temporarily set `Request.bReportStatusTime = true;` unconditionally; expected FAIL in the same test ("no transport delegate bound"). Restore; PASS. Report every run.

- [ ] **Step 11: Restore-the-bug (Completed before callbacks, IN-5)**

In `FClientState::Complete`, temporarily move `TracePoints::Completed(Ex, Finish);` to after the final `Handler->Deliver(Finish);`. Run `Tools/run_tests.sh VaRest.Http.Trace`; expected FAIL in `VaRest.Http.Trace.GameThreadOnly`: the game-thread request's `Completed` now follows its callback ("Completed before any callback"), and the `OnHttpThread` one returns before reaching it ("four events each"). Restore; PASS. Report both runs.

- [ ] **Step 12: Commit**

```bash
git add Source/VaRestHttp/Private/VaRestTracePoints.h Source/VaRestHttp/Private/VaRestTracePoints.cpp Source/VaRestHttp/Public/VaRestStage.h \
  Source/VaRestHttp/Private/VaRestClient.cpp Source/VaRestHttp/Private/VaRestPipeline.cpp Source/VaRestHttp/Private/VaRestClientState.h \
  Source/VaRestHttp/Private/VaRestClientState.cpp Source/VaRestHttp/Private/VaRestHttpModule.cpp Source/VaRestHttp/Private/Tests/VaRestHttpTraceTest.cpp
git commit -m "http: trace emission (Inspector 9) -- Started/AttemptDispatched/AttemptEnded/Completed on the game thread, clients, late-attach snapshot, initiator, zero cost when off

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 16: The test server, its lifecycle in the runner, and the integration suite (spec §7.1; decision 14; WebSocket §16.3)

Spec §7.1: "`VaRest.Integration.*` against `Tools/testserver/` (Python 3 stdlib: echo, status, delay, Retry-After, ETag, login/refresh/protected, flaky, large, chunked, envelope ok/err)." The server gets every route of that list now, M4's included, so M4 adds tests and not server code; each route is pinned by `selftest.py` (WebSocket §16.3 does the same for `ws.py`). WebSocket §16.3 fixes the shape W0 plugs into: "the `ThreadingHTTPServer` handler sees `Upgrade: websocket`, answers 101, takes over `self.connection`/`self.rfile`, and sets `close_connection`. One process and port; login/refresh/token state shared. … since `python3 -I` drops the script directory from `sys.path`, it inserts its own directory before importing siblings." So the server has `register_upgrade`, a shared `STATE` under `LOCK`, and a `PLUGINS` list it imports at start (a plug-in's `import server` must find this module, not a second copy: `load_plugins` maps the name first).

Decision 14 is the lifecycle: `Tools/run_tests.sh` starts the server on a port the OS chooses, waits for its ready file, passes `-VaRestTestServer=<url>` to the editor, and stops it on exit; `--parent-pid` makes the server leave by itself when the runner dies by `SIGKILL`. An integration test without the argument fails with instructions. The suite lives in module `VaRestTests` (Global Constraints) and runs against the real `FHttpModule` transport, the real clock (`PumpUntil(..., true)` ticks the core ticker, which ticks `FTSBackgroundableTicker` (`BackgroundableTicker.cpp:16-29`), where `FHttpManager` is registered (`HttpManager.cpp:104`)), and real threads; its checks are the ones the fake cannot make: byte-exact uploads and downloads through `SetContentFromStream`/`SetContentAsStreamedFile`/the stream delegate, the engine's own timeouts and redirects, chunked bodies, concurrency seen from the server, and the trace and logs with real HTTP-thread completions.

**Files:**
- Create: `Tools/testserver/server.py`, `Tools/testserver/selftest.py`
- Modify: `Tools/run_tests.sh` (the server's lifecycle)
- Test: `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`

**Interfaces:**
- Consumes: the whole client surface (Tasks 8–15); `VaRest::Http::Testing::{GetTestServerUrl, PumpUntil, MakeOutcome, Observe, Counters, FLogCapture, FlushRedactionPipe}`; `FVaRestHttpTestProfile` (Task 10); `VaRest::Http::FSha256` (Task 2).
- Produces: `Tools/testserver/server.py` (`register_upgrade(token, handler)`, `STATE`, `LOCK`, `PLUGINS`, `FIXTURES_DIR`, `make_server(host, port)`, `main`; CLI `--host --port --ready-file --parent-pid --fixtures-dir`; the routes in its docstring, `/fixture/<name>` for Task 17's benchmark, `/slowread` for upload progress), `Tools/testserver/selftest.py`; runner variables `VAREST_NO_TESTSERVER=1` (no server) and the server log `<Host>/Saved/Logs/VaRestTestServer.<runner pid>.log`. W0 adds `ws.py` and the TLS listener (WebSocket §17).

- [ ] **Step 1: Write the failing tests** — `Source/VaRestTests/Private/VaRestIntegrationTest.cpp`

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Tasks/Task.h"
#include "VaRestClient.h"
#include "VaRestHttpTestTypes.h"
#include "VaRestHttpTesting.h"
#include "VaRestSha256.h"
#include "VaRestTrace.h"

namespace VaRestTestsIntegrationPrivate
{
	/** Decision 14: without the runner's server an integration test fails; it never skips (a skip is a false green) */
	bool ServerUrl(FAutomationTestBase& Test, FString& OutUrl)
	{
		OutUrl = VaRest::Http::Testing::GetTestServerUrl();
		if (OutUrl.IsEmpty())
		{
			Test.AddError(TEXT("no test server: run Tools/run_tests.sh, which starts Tools/testserver/server.py and passes -VaRestTestServer=<url> (or start it and pass the argument yourself)"));
			return false;
		}
		return true;
	}

	/** Logging has its own tests; at Basic every 4xx here would add a Warning */
	FVaRestClient MakeClient(const FString& BaseUrl)
	{
		FVaRestClient Made = FVaRestClient::Make(BaseUrl);
		Made.WithLogLevel(EVaRestLogLevel::None);
		return Made;
	}

	/** The real clock's timers and the HTTP manager run on the core ticker, so it is ticked too */
	template <typename T>
	bool Wait(const TSharedRef<VaRest::Http::Testing::TTestOutcome<T>, ESPMode::ThreadSafe>& Outcome, double Seconds = 20.0)
	{
		return VaRest::Http::Testing::PumpUntil([&Outcome] { return Outcome->IsDone(); }, Seconds, true);
	}

	FString Hex(TConstArrayView<uint8> Bytes)
	{
		return VaRest::Http::FSha256::Hash(Bytes).ToHex();
	}

	TArray<uint8> Pattern(int32 Num)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Num);
		for (int32 Index = 0; Index < Num; ++Index)
		{
			Out[Index] = static_cast<uint8>(Index * 31 + 7);
		}
		return Out;
	}

	/** A fresh directory under Saved/ for files a test writes; the test deletes it */
	FString MakeTempDir()
	{
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VaRestIntegration") / FGuid::NewGuid().ToString(EGuidFormats::Digits));
		IFileManager::Get().MakeDirectory(*Dir, true);
		return Dir;
	}

	class FSink final : public IVaRestTraceSink
	{
	public:
		virtual FVaRestTraceDetailSettings GetDetail() const override
		{
			FVaRestTraceDetailSettings Detail;
			Detail.Flags = EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers;
			return Detail;
		}
		virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
		virtual void OnEvent(const FVaRestTraceEvent& Event) override
		{
			if (!IsInGameThread())
			{
				bAllOnGameThread = false;
			}
			FScopeLock Lock(&Mutex);
			Events.Add(Event);
		}

		std::atomic<bool> bAllOnGameThread{ true };
		FCriticalSection Mutex;
		TArray<FVaRestTraceEvent> Events;
	};

	struct FProgressLog
	{
		TArray<FVaRestProgress> Seen;
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationEchoTest, "VaRest.Integration.Http.EchoJson",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationEchoTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	Client.WithHeader(TEXT("X-Client"), TEXT("varest"));
	const int32 OffBefore = Counters().DecodesOffGameThread;
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo/{kind}?a=1"))
		.Path(TEXT("kind"), TEXT("a b/c"))
		.Query(TEXT("a"), TEXT("2"))
		.Header(TEXT("X-Test"), TEXT("yes"))
		.Body(FVaRestJson::Object().Set(TEXT("hello"), TEXT("мир"))), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome));
	TestTrue(TEXT("one OnSuccess, one OnComplete, on the game thread (spec 1)"), Outcome->SuccessCalls == 1 && Outcome->CompleteCalls == 1 && Outcome->bAllOnGameThread);
	if (!Outcome->Value.IsSet())
	{
		return false;
	}
	const FVaRestJson& Echo = Outcome->Value.GetValue();
	TestEqualSensitive(TEXT("the verb"), Echo[TEXT("method")].AsString(), FString(TEXT("POST")));
	TestEqualSensitive(TEXT("a path parameter is one encoded segment"), Echo[TEXT("path")].AsString(), FString(TEXT("/echo/a%20b%2Fc")));
	TestTrue(TEXT("a repeated query key appends (N9)"), Echo[TEXT("query")][TEXT("a")].Num() == 2 && Echo[TEXT("query")][TEXT("a")][1].AsString() == TEXT("2"));
	TestEqualSensitive(TEXT("the request's header"), Echo[TEXT("headers")][TEXT("x-test")].AsString(), FString(TEXT("yes")));
	TestEqualSensitive(TEXT("the client's default header"), Echo[TEXT("headers")][TEXT("x-client")].AsString(), FString(TEXT("varest")));
	TestTrue(TEXT("JSON goes up as application/json"), Echo[TEXT("content_type")].AsString().StartsWith(TEXT("application/json")));
	TestEqualSensitive(TEXT("the body arrives as UTF-8 JSON"), Echo[TEXT("body")].AsString(), FString(TEXT("{\"hello\":\"мир\"}")));
	TestEqual(TEXT("the response"), Outcome->Response.Status, 200);
	TestTrue(TEXT("the decode ran off the game thread"), Counters().DecodesOffGameThread - OffBefore >= 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationBodiesTest, "VaRest.Integration.Http.Bodies",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationBodiesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	const FString Dir = MakeTempDir();
	const TArray<uint8> FileBytes = Pattern(3 * 1024 * 1024);
	const FString FilePath = Dir / TEXT("upload.bin");
	FFileHelper::SaveArrayToFile(FileBytes, *FilePath);
	// Bytes a naive multipart parser would trip on
	const TArray<uint8> Binary = { '\r', '\n', '-', '-', 0, 255, '\r', '\n', '\r', '\n', 'e' };

	FVaRestForm Form;
	Form.Add(TEXT("name"), TEXT("Neo Anderson")).Add(TEXT("city"), TEXT("Zion & co"));
	auto FormOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(Form), FormOutcome).Send();

	FVaRestMultipart Parts;
	Parts.AddField(TEXT("note"), TEXT("hi"));
	Parts.AddBytes(TEXT("blob"), TEXT("b.bin"), TEXT("application/octet-stream"), Binary);
	Parts.AddFile(TEXT("doc"), FilePath);
	auto PartsOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(Parts), PartsOutcome).Send();

	auto FileOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Put(TEXT("/echo")).BodyFile(FilePath), FileOutcome).Send();

	auto TextOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(FStringView(TEXT("привет"))), TextOutcome).Send();

	auto BytesOutcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/echo")).Body(TArray<uint8>{ 0, 1, 2, 255 }), BytesOutcome).Send();

	TestTrue(TEXT("all complete"), Wait(FormOutcome) && Wait(PartsOutcome) && Wait(FileOutcome) && Wait(TextOutcome) && Wait(BytesOutcome));
	if (FormOutcome->Value.IsSet())
	{
		const FVaRestJson FormEcho = FormOutcome->Value.GetValue()[TEXT("form")];
		TestTrue(TEXT("a form arrives decoded"), FormEcho[TEXT("name")][0].AsString() == TEXT("Neo Anderson") && FormEcho[TEXT("city")][0].AsString() == TEXT("Zion & co"));
	}
	if (PartsOutcome->Value.IsSet())
	{
		const FVaRestJson Seen = PartsOutcome->Value.GetValue()[TEXT("parts")];
		TestEqual(TEXT("three parts"), Seen.Num(), 3);
		TestEqualSensitive(TEXT("a text field"), Seen[0][TEXT("text")].AsString(), FString(TEXT("hi")));
		TestEqualSensitive(TEXT("an in-memory file part, byte-exact"), Seen[1][TEXT("sha256")].AsString(), Hex(Binary));
		TestEqualSensitive(TEXT("a streamed file part (SetContentFromStream), byte-exact"), Seen[2][TEXT("sha256")].AsString(), Hex(FileBytes));
		TestEqualSensitive(TEXT("... named after the file"), Seen[2][TEXT("filename")].AsString(), FString(TEXT("upload.bin")));
	}
	if (FileOutcome->Value.IsSet())
	{
		const FVaRestJson& Echo = FileOutcome->Value.GetValue();
		TestTrue(TEXT("a streamed file body (SetContentAsStreamedFile), byte-exact"), Echo[TEXT("body_length")].AsInt64() == FileBytes.Num()
			&& Echo[TEXT("body_sha256")].AsString() == Hex(FileBytes));
	}
	if (TextOutcome->Value.IsSet())
	{
		const FVaRestJson& Echo = TextOutcome->Value.GetValue();
		TestTrue(TEXT("text goes up as UTF-8 text/plain"), Echo[TEXT("body")].AsString() == TEXT("привет") && Echo[TEXT("content_type")].AsString().StartsWith(TEXT("text/plain; charset=utf-8")));
	}
	if (BytesOutcome->Value.IsSet())
	{
		TestEqualSensitive(TEXT("bytes go up as they are"), BytesOutcome->Value.GetValue()[TEXT("body_base64")].AsString(), FString(TEXT("AAEC/w==")));
	}
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationStatusTest, "VaRest.Integration.Http.StatusAndErrors",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationStatusTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// curl reports the refused connection as Warnings with its own text (CurlHttp.cpp:1348)
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FVaRestClient Client = MakeClient(BaseUrl);
	auto NotFound = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/status/404")), NotFound).Send();
	auto Unavailable = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/status/503")), Unavailable).Send();
	auto NoContent = MakeOutcome<FVaRestJson>();
	Observe(Client.Delete(TEXT("/status/204")), NoContent).Send();
	auto Head = MakeOutcome<void>();
	Observe(Client.Head<void>(TEXT("/echo")), Head).Send();
	FVaRestClient Nowhere = MakeClient(TEXT("http://127.0.0.1:1"));
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Nowhere.Get(TEXT("/x")), Refused).Send();
	TestTrue(TEXT("all complete"), Wait(NotFound) && Wait(Unavailable) && Wait(NoContent) && Wait(Head) && Wait(Refused));

	TestTrue(TEXT("404 is an Http error"), NotFound->ErrorCalls == 1 && NotFound->Error.Kind == EVaRestErrorKind::Http && NotFound->Error.HttpStatus == 404);
	TestEqual(TEXT("... with its JSON body parsed (spec 3.3)"), NotFound->Error.Body[TEXT("status")].AsInt(), 404);
	TestFalse(TEXT("... not retryable"), NotFound->Error.bRetryable);
	TestTrue(TEXT("503 is retryable (spec 4.1)"), Unavailable->Error.Kind == EVaRestErrorKind::Http && Unavailable->Error.bRetryable);
	TestTrue(TEXT("204 is a success with an invalid handle (spec 3.2)"), NoContent->SuccessCalls == 1 && NoContent->Value.IsSet() && !NoContent->Value.GetValue().IsValid());
	TestTrue(TEXT("HEAD succeeds without a body"), Head->SuccessCalls == 1 && Head->Response.Status == 200);
	TestTrue(TEXT("a refused connection is a Connection error"), Refused->ErrorCalls == 1 && Refused->Error.Kind == EVaRestErrorKind::Connection && Refused->Error.HttpStatus == 0);
	TestTrue(TEXT("... and retryable"), Refused->Error.bRetryable);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationTimeoutTest, "VaRest.Integration.Http.Timeouts",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationTimeoutTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FVaRestClient Client = MakeClient(BaseUrl);
	const double TotalStart = FPlatformTime::Seconds();
	auto Total = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/delay/3000")).Timeout(0.5f), Total).Send();
	TestTrue(TEXT("completes"), Wait(Total));
	const double TotalElapsed = FPlatformTime::Seconds() - TotalStart;
	TestTrue(TEXT("the total timeout is a Timeout error (spec 4.7)"), Total->Error.Kind == EVaRestErrorKind::Timeout);
	TestTrue(TEXT("... at about its time, not the server's"), TotalElapsed < 2.5);

	const double ActivityStart = FPlatformTime::Seconds();
	auto Activity = MakeOutcome<TArray<uint8>>();
	// One byte now, the next after 4 s: the 1 s activity timeout fires in the gap
	Observe(Client.Get<TArray<uint8>>(TEXT("/drip?bytes=2&chunks=2&interval_ms=4000")).Timeout(0.f, 1.f), Activity).Send();
	TestTrue(TEXT("completes"), Wait(Activity));
	const double ActivityElapsed = FPlatformTime::Seconds() - ActivityStart;
	TestTrue(TEXT("the activity timeout is a Connection error, as the engine reports it (decision 19)"), Activity->Error.Kind == EVaRestErrorKind::Connection);
	TestTrue(TEXT("... before the server's next byte"), ActivityElapsed < 3.5);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationDownloadTest, "VaRest.Integration.Http.LargeDownload",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationDownloadTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	constexpr int32 Size = 8 * 1024 * 1024;
	FVaRestClient Client = MakeClient(BaseUrl);
	TSharedRef<FProgressLog, ESPMode::ThreadSafe> Progress = MakeShared<FProgressLog, ESPMode::ThreadSafe>();
	auto Outcome = MakeOutcome<TArray<uint8>>();
	Observe(Client.Get<TArray<uint8>>(FString::Printf(TEXT("/large?bytes=%d&seed=3"), Size))
		.OnProgress([Progress](const FVaRestProgress& Seen) { Progress->Seen.Add(Seen); }), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome, 60.0));
	if (!Outcome->Value.IsSet())
	{
		return false;
	}
	const TArray<uint8>& Bytes = Outcome->Value.GetValue();
	TestEqual(TEXT("every byte"), Bytes.Num(), Size);
	TestEqualSensitive(TEXT("byte-exact"), Hex(Bytes), Outcome->Response.Headers.Get(TEXT("X-Sha256")));
	TestTrue(TEXT("progress was reported"), Progress->Seen.Num() >= 1);
	bool bMonotonic = true;
	for (int32 Index = 1; Index < Progress->Seen.Num(); ++Index)
	{
		bMonotonic &= Progress->Seen[Index].BytesReceived >= Progress->Seen[Index - 1].BytesReceived;
	}
	TestTrue(TEXT("... never backwards"), bMonotonic);
	TestTrue(TEXT("... ending at the whole body, against Content-Length (decision 8)"), Progress->Seen.Num() >= 1
		&& Progress->Seen.Last().BytesReceived == Size && Progress->Seen.Last().DownloadTotal == Size);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationUploadProgressTest, "VaRest.Integration.Http.UploadProgress",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationUploadProgressTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// Decision 29 (review I-2): under CompleteOnHttpThread curl reports no progress while it uploads; the counting
	// archive does. The server reads 1 MiB per 100 ms, so TCP backpressure keeps the 16 MiB upload going for about 1.5 s.
	constexpr int32 Size = 16 * 1024 * 1024;
	FVaRestClient Client = MakeClient(BaseUrl);
	TSharedRef<FProgressLog, ESPMode::ThreadSafe> Progress = MakeShared<FProgressLog, ESPMode::ThreadSafe>();
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Client.Post(TEXT("/slowread?chunk=1048576&interval_ms=100")).Body(Pattern(Size))
		.OnProgress([Progress](const FVaRestProgress& Seen) { Progress->Seen.Add(Seen); }), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome, 60.0));
	TestTrue(TEXT("the server got every byte"), Outcome->Value.IsSet() && Outcome->Value.GetValue()[TEXT("received")].AsInt64() == Size);
	TestTrue(TEXT("upload progress arrives while the body goes up, not only at the end"),
		Progress->Seen.ContainsByPredicate([](const FVaRestProgress& Seen) { return Seen.BytesSent > 0 && Seen.BytesSent < Size; }));
	TestTrue(TEXT("... ending at the whole body"), Progress->Seen.Num() > 0 && Progress->Seen.Last().BytesSent == Size && Progress->Seen.Last().UploadTotal == Size);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationSaveToTest, "VaRest.Integration.Http.SaveTo",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationSaveToTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	const FString Dir = MakeTempDir();
	const FString Target = Dir / TEXT("download.bin");
	auto Outcome = MakeOutcome<FVaRestResponse>();
	Observe(Client.Get<FVaRestResponse>(TEXT("/large?bytes=4194304&seed=4")).SaveTo(Target), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome, 60.0));
	TArray<uint8> OnDisk;
	TestTrue(TEXT("the file is there"), Outcome->SuccessCalls == 1 && FFileHelper::LoadFileToArray(OnDisk, *Target));
	TestEqual(TEXT("... whole"), OnDisk.Num(), 4194304);
	TestEqualSensitive(TEXT("... byte-exact"), Hex(OnDisk), Outcome->Response.Headers.Get(TEXT("X-Sha256")));
	TestTrue(TEXT("... named in the response"), Outcome->Value.IsSet() && Outcome->Value.GetValue().SavedFile == Target);
	TArray<FString> Leftovers;
	IFileManager::Get().FindFiles(Leftovers, *Dir, TEXT(".part"));
	TestEqual(TEXT("no .part file is left (spec 3.2: atomic rename)"), Leftovers.Num(), 0);
	IFileManager::Get().DeleteDirectory(*Dir, false, true);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationShapesTest, "VaRest.Integration.Http.ChunkedRedirectCap",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationShapesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Chunked = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/chunked?chunks=16&size=4096")), Chunked).Send();
	auto Redirected = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/redirect?to=/echo/after")), Redirected).Send();
	auto Capped = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/large/json?bytes=2097152")).MaxJsonBytes(1024 * 1024), Capped).Send();
	auto Uncapped = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/large/json?bytes=2097152")), Uncapped).Send();
	TestTrue(TEXT("all complete"), Wait(Chunked) && Wait(Redirected) && Wait(Capped) && Wait(Uncapped));

	TestTrue(TEXT("a chunked body without Content-Length decodes"), Chunked->Value.IsSet() && Chunked->Value.GetValue().Num() == 16
		&& Chunked->Value.GetValue()[15].AsString().Len() == 4096);
	TestTrue(TEXT("a redirect is followed (CurlHttp.cpp:183)"), Redirected->Value.IsSet() && Redirected->Value.GetValue()[TEXT("path")].AsString() == TEXT("/echo/after"));
	TestTrue(TEXT("... and the effective URL says where it ended"), Redirected->Response.EffectiveUrl.EndsWith(TEXT("/echo/after")));
	TestTrue(TEXT("a JSON body over MaxJsonBytes is a Decode error (decision 20)"), Capped->Error.Kind == EVaRestErrorKind::Decode && Capped->Error.Message.Contains(TEXT("MaxJsonBytes")));
	TestTrue(TEXT("... stopped by the transport, not retryable, nothing kept (reviews M-6, M13)"), !Capped->Error.bRetryable && Capped->Response.Body.Num() == 0);
	TestTrue(TEXT("... and the same body under the default cap decodes whole"), Uncapped->Value.IsSet()
		&& Uncapped->Value.GetValue().Num() == FCString::Atoi(*Uncapped->Response.Headers.Get(TEXT("X-Item-Count"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationCancelTest, "VaRest.Integration.Http.Cancel",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Client.Get(TEXT("/delay/5000")), Outcome).Send();
	PumpUntil([] { return false; }, 0.2, true);
	const double CancelAt = FPlatformTime::Seconds();
	Handle.Cancel();
	TestTrue(TEXT("completes"), Wait(Outcome));
	TestTrue(TEXT("Canceled, to OnError (decision 2)"), Outcome->ErrorCalls == 1 && Outcome->Error.Kind == EVaRestErrorKind::Canceled);
	TestTrue(TEXT("... at once, not when the server answers"), FPlatformTime::Seconds() - CancelAt < 2.0);
	TestFalse(TEXT("the handle is no longer active"), Handle.IsActive());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationLimiterTest, "VaRest.Integration.Http.Limiter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationLimiterTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	// The server counts the requests it holds at once, per key (Tools/testserver/server.py /delay and /stats)
	auto MaxInFlight = [this, &BaseUrl](int32 Limit) -> int32
	{
		const FString Key = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		FVaRestClient Throttled = MakeClient(BaseUrl);
		Throttled.WithMaxConcurrent(Limit);
		TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
		for (int32 Index = 0; Index < 6; ++Index)
		{
			Outcomes.Add(MakeOutcome<FVaRestJson>());
			Observe(Throttled.Get(FString::Printf(TEXT("/delay/300?key=%s"), *Key)), Outcomes.Last()).Send();
		}
		for (const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Each : Outcomes)
		{
			TestTrue(TEXT("completes"), Wait(Each));
		}
		auto Stats = MakeOutcome<FVaRestJson>();
		Observe(MakeClient(BaseUrl).Get(FString::Printf(TEXT("/stats?key=%s"), *Key)), Stats).Send();
		Wait(Stats);
		return Stats->Value.IsSet() ? Stats->Value.GetValue()[TEXT("max_in_flight")].AsInt() : -1;
	};
	const int32 Limited = MaxInFlight(2);
	TestTrue(TEXT("WithMaxConcurrent(2): never more than two at the server (spec 4.6)"), Limited >= 1 && Limited <= 2);
	// The control: the server's count works, and the engine would have sent more at once (16 per host, CurlHttpManager.cpp:308)
	TestTrue(TEXT("without the limit, more than two at once"), MaxInFlight(16) > 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationWorkerTest, "VaRest.Integration.Http.SendFromWorker",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationWorkerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	auto Outcome = MakeOutcome<FVaRestJson>();
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> bSentOffGameThread = MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false);
	UE::Tasks::Launch(TEXT("VaRest.Test.SendFromWorker"), [Client, Outcome, bSentOffGameThread]
	{
		*bSentOffGameThread = !IsInGameThread();
		VaRest::Http::Testing::Observe(Client.Get(TEXT("/echo/worker")), Outcome).Send();
	});
	TestTrue(TEXT("completes"), Wait(Outcome));
	TestTrue(TEXT("sent from a worker"), bSentOffGameThread->load());
	TestTrue(TEXT("spec 1: marshalled to the game thread; the callbacks run there"), Outcome->SuccessCalls == 1 && Outcome->bAllOnGameThread);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationTraceLogTest, "VaRest.Integration.Http.TraceAndLogs",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationTraceLogTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestTrace::FScopedSinkIsolation Isolation;
	const TSharedRef<FSink, ESPMode::ThreadSafe> Sink = MakeShared<FSink, ESPMode::ThreadSafe>();
	FVaRestTrace::AddSink(Sink);
	FLogCapture Capture;
	FVaRestClient Client = FVaRestClient::Make(BaseUrl);
	Client.WithLogLevel(EVaRestLogLevel::Body);
	Client.SetSecretValue(TEXT("bearer"), TEXT("canary-bearer-0123456789"));

	// The server echoes all of it back: path, query, the Authorization header, inside a JSON body
	auto Echo = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle EchoHandle = Observe(Client.Get(TEXT("/echo/canary-bearer-0123456789?token=canary-query"))
		.Header(TEXT("Authorization"), TEXT("Bearer canary-bearer-0123456789")), Echo).Send();
	auto Cookie = MakeOutcome<FVaRestJson>();
	Observe(Client.Get(TEXT("/headers/set?Set-Cookie=sid%3Dcanary-cookie")), Cookie).Send();
	TestTrue(TEXT("both complete"), Wait(Echo) && Wait(Cookie));
	FlushRedactionPipe();
	FVaRestTrace::RemoveSink(Sink);

	int32 Lines = 0;
	int32 Leaks = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (!Line.StartsWith(TEXT("LogVaRestHttp: ")))
		{
			continue; // the engine's own lines are not ours to redact
		}
		++Lines;
		if (Line.Contains(TEXT("canary"), ESearchCase::CaseSensitive))
		{
			++Leaks;
			AddInfo(FString::Printf(TEXT("leaked: %s"), *Line));
		}
	}
	TestTrue(TEXT("Body-level lines were written"), Lines >= 8);
	TestEqual(TEXT("no secret in any line against a real server (Inspector 2.2)"), Leaks, 0);

	TArray<FVaRestTraceEvent> Events;
	{
		FScopeLock Lock(&Sink->Mutex);
		const uint64 Id = EchoHandle.GetId();
		Events = Sink->Events.FilterByPredicate([Id](const FVaRestTraceEvent& Event)
		{
			return Event.Kind == EVaRestTraceKind::AttemptEnded && Event.Payload.Get<FVaRestTraceAttemptEnded>().RequestId == Id;
		});
	}
	TestTrue(TEXT("Inspector 9: nothing is emitted from the HTTP thread, against the real transport"), Sink->bAllOnGameThread.load());
	TestEqual(TEXT("one AttemptEnded"), Events.Num(), 1);
	if (Events.Num() == 1)
	{
		const FVaRestTraceAttemptEnded& Ended = Events[0].Payload.Get<FVaRestTraceAttemptEnded>();
		TestTrue(TEXT("IN-3: the HTTP-thread timestamps arrived, in order"), Ended.StatusAt >= 0.0 && Ended.TransportDoneAt >= Ended.StatusAt);
		TestFalse(TEXT("... the effective URL redacted"), Ended.EffectiveUrl.Contains(TEXT("canary")));
		TestEqualSensitive(TEXT("... and its response headers"), Ended.Headers.Get(TEXT("Content-Type")), FString(TEXT("application/json")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestIntegrationStructTest, "VaRest.Integration.Http.StructDecode",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestIntegrationStructTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRestTestsIntegrationPrivate;
	FString BaseUrl;
	if (!ServerUrl(*this, BaseUrl))
	{
		return false;
	}
	FVaRestClient Client = MakeClient(BaseUrl);
	const int32 OffBefore = Counters().DecodesOffGameThread;
	auto Outcome = MakeOutcome<FVaRestHttpTestProfile>();
	Observe(Client.Get<FVaRestHttpTestProfile>(TEXT("/json/profile")), Outcome).Send();
	TestTrue(TEXT("completes"), Wait(Outcome));
	if (!Outcome->Value.IsSet())
	{
		return false;
	}
	const FVaRestHttpTestProfile& Profile = Outcome->Value.GetValue();
	TestEqual(TEXT("an int64 past 2^53 stays exact end to end"), Profile.Id, static_cast<int64>(9007199254740993ll));
	TestEqualSensitive(TEXT("camelCase keys onto PascalCase fields (spec 2.4)"), Profile.Name, FString(TEXT("Neo")));
	TestTrue(TEXT("an array of structs"), Profile.Items.Num() == 2 && Profile.Items[0].Name == TEXT("sword") && Profile.Items[0].Level == 3);
	TestEqual(TEXT("a field the server leaves out keeps its default"), Profile.Score, 7);
	TestEqual(TEXT("decoded off the game thread"), Counters().DecodesOffGameThread - OffBefore, 1);
	return true;
}

#endif
```

- [ ] **Step 2: Run the tests to verify they fail**

Run: `Tools/run_tests.sh VaRest.Integration`
Expected: it builds, and all thirteen `VaRest.Integration.Http.*` tests FAIL with "no test server: run Tools/run_tests.sh, which starts Tools/testserver/server.py …" (the runner does not start one yet). This is decision 14's loud failure, seen once on purpose.

- [ ] **Step 3: The server** — `Tools/testserver/server.py`

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""VaRest HTTP test server (core spec 7.1). Python 3.9+ standard library only.

    python3 -I Tools/testserver/server.py [--host 127.0.0.1] [--port 0] [--ready-file PATH] [--parent-pid PID]
                                          [--fixtures-dir DIR]

--port 0 lets the OS choose. Once the server accepts connections it prints "VaRestTestServer <base url>" and,
with --ready-file, writes the base URL to that file (atomically, so a reader never sees half of it). With
--parent-pid it exits when that process is gone or is no longer its parent (a reused PID), so a runner killed
with SIGKILL leaves no server behind. The parent must be the process that started the server.

Routes. Every response with a body has a Content-Length unless the route says chunked.

  ANY  /echo[/<anything>]   JSON: method, path, query {k: [v, ...]}, headers {lower-case name: value}, content_type,
                            body (UTF-8 text or null), body_base64, body_length, body_sha256, plus form {k: [v, ...]}
                            for urlencoded bodies and parts [{name, filename, content_type, length, sha256, text}]
                            for multipart ones. Request bodies may be chunked.
  ANY  /status/<code>       that status, JSON {"status": code} (none for 204/304); ?location=<url> sets Location,
                            ?body=<text> sends it as text/plain instead
  ANY  /delay/<ms>          sleeps, then {"delayed_ms": ms}; ?key=<k> counts the requests in flight per key
  GET  /stats?key=<k>       {"in_flight", "max_in_flight", "count"} of /delay with that key; &reset=1 forgets it
  GET  /drip?bytes=N&chunks=K&interval_ms=M   N bytes of 'x' in K writes, M ms apart, with Content-Length
  GET  /large?bytes=N&seed=S                  N deterministic bytes (application/octet-stream), X-Sha256 header
  GET  /large/json?bytes=N                    a JSON array of objects, at least N bytes; X-Item-Count header
  GET  /chunked?chunks=K&size=M               chunked transfer: a JSON array of K strings of M letters
  ANY  /redirect?to=<path>  302 to that path
  ANY  /headers/set?<Name>=<Value>&...        200 {} with each pair as a response header
  GET  /json/profile        the FVaRestHttpTestProfile fixture (camelCase keys; an id above 2^53; no "score")
  ANY  /slowread?chunk=N&interval_ms=M        reads the request body N bytes at a time, M ms apart (TCP backpressure
                            slows the client's upload, so upload progress can be watched), then {"received": bytes}
  GET  /fixture/<name>      a file of --fixtures-dir (the benchmark's bench_*.json from Tools/gen_bench_json.py), kept
                            in memory after the first read; 404 without the option or the file
  For M4's stage tests (core spec 7.1):
  ANY  /retry-after/<n>?key=<k>&seconds=<s>   the first n calls per key: 503 with Retry-After: s; then 200
  GET  /etag?tag=<t>        ETag "t"; a matching If-None-Match gets 304
  POST /login {"user": "neo", "password": "trinity"} -> {"accessToken", "refreshToken", "expiresIn"}
  POST /refresh {"refreshToken"} -> new tokens; the old refresh token is spent
  ANY  /protected           "Authorization: Bearer <current access token>" -> 200, else 401
  ANY  /flaky?n=<k>&key=<k> the first k calls per key close the connection without a response; then 200
  GET  /envelope/ok, /envelope/err            {"ok": true, "data": {...}} / {"ok": false, "code", "message"}

Plug-ins (WebSocket spec 16.3): each module named in PLUGINS that sits next to this file is imported at start
and may call register_upgrade(token, handler). A request whose Upgrade header names that token goes to
handler(request_handler) instead of a route; the handler answers 101, takes over request_handler.connection,
rfile and wfile, and sets request_handler.close_connection. STATE and its lock are shared with plug-ins.
"""

import argparse
import base64
import hashlib
import http.server
import importlib
import json
import os
import random
import re
import socket
import sys
import threading
import time
import urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGINS = ("ws",)

LOCK = threading.Lock()
STATE = {
    "delay": {},
    "retry": {},
    "flaky": {},
    "tokens": {"access": None, "refresh": None, "serial": 0},
}
_UPGRADES = {}
FIXTURES_DIR = None
_FIXTURES = {}

PROFILE = {
    "id": 9007199254740993,
    "name": "Neo",
    "items": [{"name": "sword", "level": 3}, {"name": "shield", "level": 1}],
}


def register_upgrade(token, handler):
    """WebSocket spec 16.3: ws.py registers "websocket" here"""
    _UPGRADES[token.strip().lower()] = handler


def decode_text(data):
    try:
        return data.decode("utf-8")
    except UnicodeDecodeError:
        return None


def parse_multipart(content_type, body):
    match = re.search(r'boundary="?([^";]+)"?', content_type)
    if not match:
        return []
    delimiter = b"--" + match.group(1).encode("latin-1")
    parts = []
    for chunk in body.split(delimiter)[1:]:
        if chunk.startswith(b"--"):
            break
        if chunk.startswith(b"\r\n"):
            chunk = chunk[2:]
        head, _, payload = chunk.partition(b"\r\n\r\n")
        if payload.endswith(b"\r\n"):
            payload = payload[:-2]
        headers = {}
        for line in head.split(b"\r\n"):
            name, _, value = line.decode("utf-8", "replace").partition(":")
            headers[name.strip().lower()] = value.strip()
        disposition = headers.get("content-disposition", "")
        name = re.search(r'\bname="([^"]*)"', disposition)
        filename = re.search(r'\bfilename="([^"]*)"', disposition)
        parts.append({
            "name": name.group(1) if name else None,
            "filename": filename.group(1) if filename else None,
            "content_type": headers.get("content-type"),
            "length": len(payload),
            "sha256": hashlib.sha256(payload).hexdigest(),
            "text": decode_text(payload) if filename is None else None,
        })
    return parts


def issue_tokens():
    """Under LOCK"""
    tokens = STATE["tokens"]
    tokens["serial"] += 1
    tokens["access"] = "access-%d-%s" % (tokens["serial"], os.urandom(4).hex())
    tokens["refresh"] = "refresh-%d-%s" % (tokens["serial"], os.urandom(4).hex())
    return {"accessToken": tokens["access"], "refreshToken": tokens["refresh"], "expiresIn": 3600}


def count_call(table, key):
    """Takes LOCK: how many calls this key had before this one"""
    with LOCK:
        seen = STATE[table].get(key, 0)
        STATE[table][key] = seen + 1
    return seen


def query_int(h, name, default):
    return int(h.query.get(name, [str(default)])[0])


def route_echo(h, match):
    body = h.body
    content_type = h.headers.get("Content-Type", "")
    out = {
        "method": h.command,
        "path": urllib.parse.urlsplit(h.path).path,
        "query": h.query,
        "headers": {name.lower(): value for name, value in h.headers.items()},
        "content_type": content_type,
        "body": decode_text(body),
        "body_base64": base64.b64encode(body).decode("ascii"),
        "body_length": len(body),
        "body_sha256": hashlib.sha256(body).hexdigest(),
    }
    media = content_type.split(";")[0].strip().lower()
    if media == "application/x-www-form-urlencoded":
        out["form"] = urllib.parse.parse_qs(body.decode("utf-8", "replace"), keep_blank_values=True)
    elif media.startswith("multipart/"):
        out["parts"] = parse_multipart(content_type, body)
    h.send_json(200, out)


def route_status(h, match):
    code = int(match.group(1))
    headers = []
    if "location" in h.query:
        headers.append(("Location", h.query["location"][0]))
    if "body" in h.query:
        h.send_bytes(code, h.query["body"][0].encode("utf-8"), "text/plain; charset=utf-8", headers)
    else:
        h.send_json(code, {"status": code}, headers)


def route_delay(h, match):
    ms = int(match.group(1))
    key = h.query.get("key", [None])[0]
    if key:
        with LOCK:
            entry = STATE["delay"].setdefault(key, {"in_flight": 0, "max_in_flight": 0, "count": 0})
            entry["in_flight"] += 1
            entry["count"] += 1
            entry["max_in_flight"] = max(entry["max_in_flight"], entry["in_flight"])
    try:
        time.sleep(ms / 1000.0)
    finally:
        if key:
            with LOCK:
                STATE["delay"][key]["in_flight"] -= 1
    h.send_json(200, {"delayed_ms": ms})


def route_stats(h, match):
    key = h.query.get("key", [""])[0]
    with LOCK:
        entry = dict(STATE["delay"].get(key, {"in_flight": 0, "max_in_flight": 0, "count": 0}))
        if h.query.get("reset", ["0"])[0] == "1":
            STATE["delay"].pop(key, None)
    h.send_json(200, entry)


def route_drip(h, match):
    total = query_int(h, "bytes", 1024)
    chunks = max(1, query_int(h, "chunks", 4))
    interval = query_int(h, "interval_ms", 100) / 1000.0
    h.send_response(200)
    h.send_header("Content-Type", "application/octet-stream")
    h.send_header("Content-Length", str(total))
    h.end_headers()
    if h.command == "HEAD":
        return
    for index in range(chunks):
        if index > 0:
            time.sleep(interval)
        h.wfile.write(b"x" * (total // chunks + (1 if index < total % chunks else 0)))
        h.wfile.flush()


def route_large(h, match):
    body = random.Random(query_int(h, "seed", 1)).randbytes(query_int(h, "bytes", 1048576))
    h.send_bytes(200, body, "application/octet-stream", [("X-Sha256", hashlib.sha256(body).hexdigest())])


def route_large_json(h, match):
    target = query_int(h, "bytes", 1048576)
    items = []
    size = 2
    while size < target:
        index = len(items)
        text = json.dumps({"id": index, "name": "item-%d" % index, "tags": ["a", "b", "c"], "score": index * 0.5}, separators=(",", ":"))
        items.append(text)
        size += len(text) + 1
    body = ("[" + ",".join(items) + "]").encode("utf-8")
    h.send_bytes(200, body, "application/json", [("X-Item-Count", str(len(items)))])


def route_chunked(h, match):
    chunks = query_int(h, "chunks", 8)
    size = query_int(h, "size", 1024)
    h.send_response(200)
    h.send_header("Content-Type", "application/json")
    h.send_header("Transfer-Encoding", "chunked")
    h.end_headers()
    pieces = [b"["]
    for index in range(chunks):
        pieces.append((b"," if index else b"") + b'"' + bytes([97 + index % 26]) * size + b'"')
    pieces.append(b"]")
    for piece in pieces:
        h.wfile.write(b"%x\r\n%s\r\n" % (len(piece), piece))
    h.wfile.write(b"0\r\n\r\n")


def route_redirect(h, match):
    h.send_bytes(302, b"", "text/plain", [("Location", h.query.get("to", ["/echo"])[0])])


def route_headers_set(h, match):
    h.send_json(200, {}, [(name, value) for name, values in h.query.items() for value in values])


def route_profile(h, match):
    h.send_json(200, PROFILE)


def route_fixture(h, match):
    name = match.group(1)
    path = os.path.join(FIXTURES_DIR, name) if FIXTURES_DIR else None
    with LOCK:
        body = _FIXTURES.get(name)
    if body is None and path and os.path.isfile(path):
        with open(path, "rb") as stream:
            body = stream.read()
        with LOCK:
            _FIXTURES[name] = body
    if body is None:
        h.send_json(404, {"error": "no fixture", "name": name})
        return
    h.send_bytes(200, body, "application/json" if name.endswith(".json") else "application/octet-stream")


def route_slowread(h):
    """Called before the body is read: it reads the body itself, slowly"""
    chunk = max(1, query_int(h, "chunk", 65536))
    interval = query_int(h, "interval_ms", 100) / 1000.0
    remaining = int(h.headers.get("Content-Length") or 0)
    received = 0
    while remaining > 0:
        data = h.rfile.read(min(chunk, remaining))
        if not data:
            break
        received += len(data)
        remaining -= len(data)
        if remaining > 0:
            time.sleep(interval)
    h.send_json(200, {"received": received})


def route_retry_after(h, match):
    failures = int(match.group(1))
    seen = count_call("retry", h.query.get("key", ["default"])[0])
    if seen < failures:
        h.send_json(503, {"attempt": seen + 1}, [("Retry-After", h.query.get("seconds", ["1"])[0])])
    else:
        h.send_json(200, {"attempt": seen + 1})


def route_etag(h, match):
    tag = '"%s"' % h.query.get("tag", ["v1"])[0]
    if h.headers.get("If-None-Match") == tag:
        h.send_bytes(304, b"", headers=[("ETag", tag)])
    else:
        h.send_json(200, {"tag": tag}, [("ETag", tag), ("Cache-Control", "no-cache")])


def read_json(h):
    try:
        value = json.loads(h.body or b"{}")
    except ValueError:
        return {}
    return value if isinstance(value, dict) else {}


def route_login(h, match):
    request = read_json(h)
    if request.get("user") != "neo" or request.get("password") != "trinity":
        h.send_json(401, {"error": "bad credentials"})
        return
    with LOCK:
        tokens = issue_tokens()
    h.send_json(200, tokens)


def route_refresh(h, match):
    request = read_json(h)
    with LOCK:
        current = STATE["tokens"]["refresh"]
        tokens = issue_tokens() if current is not None and request.get("refreshToken") == current else None
    if tokens is None:
        h.send_json(401, {"error": "bad refresh token"})
    else:
        h.send_json(200, tokens)


def route_protected(h, match):
    with LOCK:
        current = STATE["tokens"]["access"]
    if current is not None and h.headers.get("Authorization") == "Bearer " + current:
        h.send_json(200, {"ok": True})
    else:
        h.send_json(401, {"error": "unauthorized"}, [("WWW-Authenticate", "Bearer")])


def route_flaky(h, match):
    failures = query_int(h, "n", 1)
    seen = count_call("flaky", h.query.get("key", ["default"])[0])
    if seen < failures:
        # No status line at all: the client sees the connection end (a Connection error)
        h.close_connection = True
        try:
            h.connection.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        return
    h.send_json(200, {"attempt": seen + 1})


def route_envelope_ok(h, match):
    h.send_json(200, {"ok": True, "data": {"id": 7, "name": "Neo"}})


def route_envelope_err(h, match):
    h.send_json(200, {"ok": False, "code": "E42", "message": "the envelope says no"})


ANY = None
ROUTES = [
    (ANY, r"/echo(?:/.*)?", route_echo),
    (ANY, r"/status/(\d{3})", route_status),
    (ANY, r"/delay/(\d+)", route_delay),
    (("GET",), r"/stats", route_stats),
    (("GET", "HEAD"), r"/drip", route_drip),
    (("GET", "HEAD"), r"/large", route_large),
    (("GET",), r"/large/json", route_large_json),
    (("GET",), r"/chunked", route_chunked),
    (ANY, r"/redirect", route_redirect),
    (ANY, r"/headers/set", route_headers_set),
    (("GET",), r"/json/profile", route_profile),
    (("GET",), r"/fixture/([A-Za-z0-9_.-]+)", route_fixture),
    (ANY, r"/retry-after/(\d+)", route_retry_after),
    (("GET",), r"/etag", route_etag),
    (("POST",), r"/login", route_login),
    (("POST",), r"/refresh", route_refresh),
    (ANY, r"/protected", route_protected),
    (ANY, r"/flaky", route_flaky),
    (("GET",), r"/envelope/ok", route_envelope_ok),
    (("GET",), r"/envelope/err", route_envelope_err),
]
ROUTES = [(methods, re.compile(pattern), route) for methods, pattern, route in ROUTES]


class Handler(http.server.BaseHTTPRequestHandler):
    # Keep-alive like a real API server: every response has a Content-Length or is chunked
    protocol_version = "HTTP/1.1"
    server_version = "VaRestTestServer/1"

    def log_message(self, format, *args):
        # The UE log is the record; set VAREST_TESTSERVER_VERBOSE=1 to see requests
        if os.environ.get("VAREST_TESTSERVER_VERBOSE") == "1":
            super().log_message(format, *args)

    def do_GET(self):
        self.dispatch()

    do_POST = do_PUT = do_PATCH = do_DELETE = do_HEAD = do_OPTIONS = do_GET

    def dispatch(self):
        try:
            upgrade = self.headers.get("Upgrade", "").strip().lower()
            if upgrade in _UPGRADES:
                _UPGRADES[upgrade](self)
                return
            url = urllib.parse.urlsplit(self.path)
            self.query = urllib.parse.parse_qs(url.query, keep_blank_values=True)
            if url.path == "/slowread":
                route_slowread(self)
                return
            self.body = self.read_body()
            for methods, pattern, route in ROUTES:
                match = pattern.fullmatch(url.path)
                if match and (methods is ANY or self.command in methods):
                    route(self, match)
                    return
            self.send_json(404, {"error": "no route", "path": url.path})
        except (BrokenPipeError, ConnectionResetError):
            # The client gave up (a timeout or cancel test)
            self.close_connection = True

    def read_body(self):
        if "chunked" in self.headers.get("Transfer-Encoding", "").lower():
            chunks = []
            while True:
                size = int(self.rfile.readline().split(b";")[0].strip() or b"0", 16)
                if size == 0:
                    while self.rfile.readline() not in (b"\r\n", b"\n", b""):
                        pass
                    return b"".join(chunks)
                chunks.append(self.rfile.read(size))
                self.rfile.readline()
        length = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(length) if length > 0 else b""

    def send_bytes(self, status, body, content_type="application/octet-stream", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        has_body = status not in (204, 304)
        if has_body:
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if has_body and self.command != "HEAD":
            self.wfile.write(body)

    def send_json(self, status, value, headers=()):
        self.send_bytes(status, json.dumps(value, separators=(",", ":")).encode("utf-8"), "application/json", headers)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        # A client that cancels, times out or drops a keep-alive connection resets it: a test scenario, not an error
        if isinstance(sys.exc_info()[1], (ConnectionResetError, BrokenPipeError, TimeoutError)):
            return
        super().handle_error(request, client_address)


def make_server(host="127.0.0.1", port=0):
    return Server((host, port), Handler)


def load_plugins():
    # Run as a script this module is __main__; a plug-in's "import server" must get this module, not a second copy
    # with its own STATE and registry
    sys.modules.setdefault("server", sys.modules[__name__])
    # python3 -I drops the script directory from sys.path (WebSocket spec 16.3)
    if HERE not in sys.path:
        sys.path.insert(0, HERE)
    for name in PLUGINS:
        if os.path.exists(os.path.join(HERE, name + ".py")):
            importlib.import_module(name)


def write_ready(path, url):
    temporary = path + ".tmp"
    with open(temporary, "w", encoding="utf-8") as stream:
        stream.write(url + "\n")
    os.replace(temporary, path)


def watch_parent(pid, server):
    # POSIX only: there os.kill(pid, 0) probes, on Windows it would terminate the process. A PID can be reused, so the
    # server also checks that it is still that process's child: when the parent dies, the server is re-parented.
    if os.name != "posix":
        return
    while True:
        time.sleep(0.5)
        if os.getppid() != pid:
            server.shutdown()
            return
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            server.shutdown()
            return
        except PermissionError:
            pass


def main(argv=None):
    parser = argparse.ArgumentParser(description="VaRest HTTP test server")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--ready-file")
    parser.add_argument("--parent-pid", type=int)
    parser.add_argument("--fixtures-dir")
    args = parser.parse_args(argv)

    global FIXTURES_DIR
    FIXTURES_DIR = args.fixtures_dir

    load_plugins()
    server = make_server(args.host, args.port)
    url = "http://%s:%d" % (args.host, server.server_address[1])
    if args.parent_pid:
        threading.Thread(target=watch_parent, args=(args.parent_pid, server), daemon=True).start()
    print("VaRestTestServer " + url, flush=True)
    if args.ready_file:
        write_ready(args.ready_file, url)
    try:
        server.serve_forever(poll_interval=0.2)
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [ ] **Step 4: Its self-test** — `Tools/testserver/selftest.py`

```python
#!/usr/bin/env python3
# Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
"""Checks Tools/testserver/server.py with a standard-library client before UE tests rely on it.

    python3 -I Tools/testserver/selftest.py [-v]
"""

import base64
import hashlib
import http.client
import json
import os
import socket
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
import server  # noqa: E402


class ServerTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.httpd = server.make_server("127.0.0.1", 0)
        cls.port = cls.httpd.server_address[1]
        cls.thread = threading.Thread(target=cls.httpd.serve_forever, kwargs={"poll_interval": 0.05}, daemon=True)
        cls.thread.start()

    @classmethod
    def tearDownClass(cls):
        cls.httpd.shutdown()
        cls.httpd.server_close()

    def call(self, method, path, body=None, headers=None, chunked=False):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=10)
        try:
            connection.request(method, path, body=body, headers=headers or {}, encode_chunked=chunked)
            response = connection.getresponse()
            return response.status, response.msg, response.read()
        finally:
            connection.close()

    def call_json(self, method, path, body=None, headers=None, chunked=False):
        status, headers_in, data = self.call(method, path, body, headers, chunked)
        return status, headers_in, (json.loads(data) if data else None)

    def test_echo_json_query_headers(self):
        status, _, echo = self.call_json("POST", "/echo/x?a=1&a=2&b=", b'{"k":"v"}', {"Content-Type": "application/json", "X-Test": "yes"})
        self.assertEqual(status, 200)
        self.assertEqual(echo["method"], "POST")
        self.assertEqual(echo["path"], "/echo/x")
        self.assertEqual(echo["query"], {"a": ["1", "2"], "b": [""]})
        self.assertEqual(echo["headers"]["x-test"], "yes")
        self.assertEqual(echo["body"], '{"k":"v"}')
        self.assertEqual(echo["body_sha256"], hashlib.sha256(b'{"k":"v"}').hexdigest())

    def test_echo_form(self):
        _, _, echo = self.call_json("POST", "/echo", b"name=Neo+Anderson&city=Zion+%26+co", {"Content-Type": "application/x-www-form-urlencoded"})
        self.assertEqual(echo["form"], {"name": ["Neo Anderson"], "city": ["Zion & co"]})

    def test_echo_multipart_binary_safe(self):
        binary = b"\r\n--\x00\xff\r\n\r\nend"
        body = (b"--B\r\nContent-Disposition: form-data; name=\"note\"\r\n\r\nhi\r\n"
                b"--B\r\nContent-Disposition: form-data; name=\"blob\"; filename=\"b.bin\"\r\nContent-Type: application/octet-stream\r\n\r\n"
                + binary + b"\r\n--B--\r\n")
        _, _, echo = self.call_json("POST", "/echo", body, {"Content-Type": "multipart/form-data; boundary=B"})
        parts = echo["parts"]
        self.assertEqual([part["name"] for part in parts], ["note", "blob"])
        self.assertEqual(parts[0]["text"], "hi")
        self.assertEqual(parts[1]["filename"], "b.bin")
        self.assertEqual(parts[1]["length"], len(binary))
        self.assertEqual(parts[1]["sha256"], hashlib.sha256(binary).hexdigest())

    def test_echo_chunked_request_body(self):
        _, _, echo = self.call_json("PUT", "/echo", iter([b"abc", b"defg"]), {"Content-Type": "text/plain"}, chunked=True)
        self.assertEqual(echo["body"], "abcdefg")

    def test_echo_binary_body(self):
        _, _, echo = self.call_json("POST", "/echo", b"\x00\x01\x02\xff", {"Content-Type": "application/octet-stream"})
        self.assertIsNone(echo["body"])
        self.assertEqual(echo["body_base64"], base64.b64encode(b"\x00\x01\x02\xff").decode("ascii"))

    def test_status(self):
        status, _, value = self.call_json("GET", "/status/404")
        self.assertEqual((status, value), (404, {"status": 404}))
        status, _, data = self.call("GET", "/status/204")
        self.assertEqual((status, data), (204, b""))
        status, headers, _ = self.call("GET", "/status/302?location=/echo")
        self.assertEqual((status, headers["Location"]), (302, "/echo"))
        status, headers, data = self.call("GET", "/status/500?body=oops")
        self.assertEqual((status, headers["Content-Type"], data), (500, "text/plain; charset=utf-8", b"oops"))
        status, headers, data = self.call("HEAD", "/echo")
        self.assertEqual(status, 200)
        self.assertEqual(data, b"")
        self.assertGreater(int(headers["Content-Length"]), 0)

    def test_unknown_route(self):
        status, _, value = self.call_json("GET", "/nope")
        self.assertEqual((status, value["error"]), (404, "no route"))

    def test_delay_counts_concurrency(self):
        key = uuid.uuid4().hex
        threads = [threading.Thread(target=self.call, args=("GET", "/delay/300?key=" + key)) for _ in range(3)]
        started = time.monotonic()
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertLess(time.monotonic() - started, 0.9)
        _, _, stats = self.call_json("GET", "/stats?key=%s&reset=1" % key)
        self.assertEqual((stats["count"], stats["max_in_flight"], stats["in_flight"]), (3, 3, 0))
        _, _, stats = self.call_json("GET", "/stats?key=" + key)
        self.assertEqual(stats["count"], 0)

    def test_drip(self):
        started = time.monotonic()
        status, headers, data = self.call("GET", "/drip?bytes=10&chunks=3&interval_ms=200")
        self.assertEqual((status, data, headers["Content-Length"]), (200, b"x" * 10, "10"))
        self.assertGreaterEqual(time.monotonic() - started, 0.4)

    def test_slowread(self):
        started = time.monotonic()
        status, _, value = self.call_json("POST", "/slowread?chunk=100&interval_ms=50", b"x" * 300, {"Content-Type": "application/octet-stream"})
        self.assertEqual((status, value), (200, {"received": 300}))
        self.assertGreaterEqual(time.monotonic() - started, 0.1)

    def test_large_is_deterministic(self):
        _, headers, first = self.call("GET", "/large?bytes=100000&seed=5")
        _, _, second = self.call("GET", "/large?bytes=100000&seed=5")
        self.assertEqual(len(first), 100000)
        self.assertEqual(first, second)
        self.assertEqual(headers["X-Sha256"], hashlib.sha256(first).hexdigest())

    def test_large_json(self):
        _, headers, items = self.call_json("GET", "/large/json?bytes=50000")
        self.assertEqual(len(items), int(headers["X-Item-Count"]))
        self.assertGreaterEqual(len(json.dumps(items, separators=(",", ":"))), 50000)

    def test_chunked_response(self):
        status, headers, items = self.call_json("GET", "/chunked?chunks=5&size=3")
        self.assertEqual(status, 200)
        self.assertEqual(headers["Transfer-Encoding"], "chunked")
        self.assertEqual(items, ["aaa", "bbb", "ccc", "ddd", "eee"])

    def test_redirect_and_headers(self):
        status, headers, _ = self.call("GET", "/redirect?to=/echo/after")
        self.assertEqual((status, headers["Location"]), (302, "/echo/after"))
        status, headers, _ = self.call("GET", "/headers/set?Set-Cookie=sid%3Dx&X-A=1")
        self.assertEqual((status, headers["Set-Cookie"], headers["X-A"]), (200, "sid=x", "1"))

    def test_profile(self):
        _, _, profile = self.call_json("GET", "/json/profile")
        self.assertEqual(profile["id"], 9007199254740993)
        self.assertNotIn("score", profile)

    def test_fixture(self):
        with tempfile.TemporaryDirectory() as directory:
            with open(os.path.join(directory, "bench_tiny.json"), "wb") as stream:
                stream.write(b'{"ok":true}')
            server.FIXTURES_DIR = directory
            try:
                status, headers, data = self.call("GET", "/fixture/bench_tiny.json")
                self.assertEqual((status, headers["Content-Type"], data), (200, "application/json", b'{"ok":true}'))
                self.assertEqual(self.call("GET", "/fixture/missing.json")[0], 404)
                self.assertEqual(self.call("GET", "/fixture/..")[0], 404)
            finally:
                server.FIXTURES_DIR = None

    def test_retry_after(self):
        key = uuid.uuid4().hex
        status, headers, _ = self.call("GET", "/retry-after/2?seconds=3&key=" + key)
        self.assertEqual((status, headers["Retry-After"]), (503, "3"))
        self.assertEqual(self.call("GET", "/retry-after/2?key=" + key)[0], 503)
        self.assertEqual(self.call_json("GET", "/retry-after/2?key=" + key)[2], {"attempt": 3})

    def test_etag(self):
        status, headers, _ = self.call("GET", "/etag?tag=v7")
        self.assertEqual((status, headers["ETag"]), (200, '"v7"'))
        status, _, data = self.call("GET", "/etag?tag=v7", headers={"If-None-Match": '"v7"'})
        self.assertEqual((status, data), (304, b""))

    def test_login_refresh_protected(self):
        self.assertEqual(self.call("POST", "/login", b'{"user":"neo","password":"nope"}')[0], 401)
        _, _, tokens = self.call_json("POST", "/login", b'{"user":"neo","password":"trinity"}')
        bearer = {"Authorization": "Bearer " + tokens["accessToken"]}
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 200)
        _, _, renewed = self.call_json("POST", "/refresh", json.dumps({"refreshToken": tokens["refreshToken"]}).encode())
        self.assertEqual(self.call("GET", "/protected", headers=bearer)[0], 401)
        self.assertEqual(self.call("GET", "/protected", headers={"Authorization": "Bearer " + renewed["accessToken"]})[0], 200)
        self.assertEqual(self.call("POST", "/refresh", json.dumps({"refreshToken": tokens["refreshToken"]}).encode())[0], 401)

    def test_flaky(self):
        key = uuid.uuid4().hex
        with self.assertRaises((http.client.RemoteDisconnected, ConnectionError)):
            self.call("GET", "/flaky?n=1&key=" + key)
        self.assertEqual(self.call_json("GET", "/flaky?n=1&key=" + key)[2], {"attempt": 2})

    def test_envelope(self):
        self.assertTrue(self.call_json("GET", "/envelope/ok")[2]["ok"])
        self.assertEqual(self.call_json("GET", "/envelope/err")[2]["code"], "E42")

    def test_upgrade_plugin_contract(self):
        # WebSocket spec 16.3: the handler answers 101, takes over the connection and sets close_connection
        def handler(request):
            request.wfile.write(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: x-test\r\nConnection: Upgrade\r\n\r\n")
            line = request.rfile.readline()
            request.wfile.write(b"echo:" + line)
            request.close_connection = True

        server.register_upgrade("x-test", handler)
        with socket.create_connection(("127.0.0.1", self.port), timeout=10) as raw:
            raw.sendall(b"GET /anything HTTP/1.1\r\nHost: x\r\nUpgrade: x-test\r\nConnection: Upgrade\r\n\r\nping\n")
            received = b""
            while not received.endswith(b"echo:ping\n"):
                chunk = raw.recv(4096)
                if not chunk:
                    break
                received += chunk
        self.assertTrue(received.startswith(b"HTTP/1.1 101 "))
        self.assertTrue(received.endswith(b"echo:ping\n"))


class LifecycleTest(unittest.TestCase):
    def test_ready_file_and_parent_watch(self):
        # The runner's contract (M3 plan decision 14): a ready file with the URL; exit when the parent -- the process
        # that started the server -- goes. A stand-in parent starts the server and sleeps; the test kills it.
        with tempfile.TemporaryDirectory() as directory:
            ready = os.path.join(directory, "url")
            child_pid_file = os.path.join(directory, "pid")
            starter = ("import os, subprocess, sys, time\n"
                       "child = subprocess.Popen([sys.executable, '-I', sys.argv[1], '--port', '0', '--ready-file', sys.argv[2],"
                       " '--parent-pid', str(os.getpid())], stdout=subprocess.DEVNULL)\n"
                       "open(sys.argv[3], 'w').write(str(child.pid))\n"
                       "time.sleep(60)\n")
            parent = subprocess.Popen([sys.executable, "-c", starter, os.path.join(HERE, "server.py"), ready, child_pid_file])
            try:
                deadline = time.monotonic() + 10
                while not os.path.exists(ready) and time.monotonic() < deadline:
                    time.sleep(0.05)
                with open(ready, encoding="utf-8") as stream:
                    url = urllib.parse.urlsplit(stream.read().strip())
                connection = http.client.HTTPConnection(url.hostname, url.port, timeout=10)
                connection.request("GET", "/echo")
                self.assertEqual(connection.getresponse().status, 200)
                connection.close()
                with open(child_pid_file, encoding="utf-8") as stream:
                    child_pid = int(stream.read())
                parent.kill()
                parent.wait()
                deadline = time.monotonic() + 5
                gone = False
                while time.monotonic() < deadline and not gone:
                    try:
                        os.kill(child_pid, 0)
                        time.sleep(0.1)
                    except ProcessLookupError:
                        gone = True
                self.assertTrue(gone, "the server outlived its parent")
            finally:
                if parent.poll() is None:
                    parent.kill()
                    parent.wait()


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 5: Run the self-test**

Run: `python3 -I Tools/testserver/selftest.py -v`
Expected: `Ran 23 tests` … `OK` (about 2 s; Python 3.9 or later for `random.randbytes`).

- [ ] **Step 6: The runner owns the server** — in `Tools/run_tests.sh`:

1. Add to the header comment, after the `VAREST_TEST_TIMEOUT` lines:

```bash
# The runner starts Tools/testserver/server.py on a port the OS picks and passes its URL to the editor as
# -VaRestTestServer=<url> for the VaRest.Integration.* tests (M3 plan decision 14); it stops the server on exit, and the
# server also exits by itself when this script dies (--parent-pid). VAREST_NO_TESTSERVER=1 skips it: integration tests
# then fail, by design. The server's output goes to <Host>/Saved/Logs/VaRestTestServer.<runner pid>.log.
```

2. After the `if [[ $BUILD -eq 1 ]]; then ... fi` block, insert:

```bash
SERVER_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/testserver"
# $$ in the names: two runners at once each get their own (review M-13)
SERVER_URL_FILE="$HOST/Saved/Logs/VaRestTestServer.$$.url"
SERVER_LOG="$HOST/Saved/Logs/VaRestTestServer.$$.log"
SERVER_PID=""
stop_server() {
	# Our own child, by PID (never by name or pattern)
	if [[ -n "$SERVER_PID" ]] && kill -0 "$SERVER_PID" 2>/dev/null; then
		kill "$SERVER_PID" 2>/dev/null || true
		wait "$SERVER_PID" 2>/dev/null || true
	fi
	rm -f "$SERVER_URL_FILE"
}
trap stop_server EXIT
SERVER_ARGS=()
if [[ "${VAREST_NO_TESTSERVER:-0}" != "1" ]]; then
	command -v python3 >/dev/null || { echo "run_tests: python3 not found; the integration tests need it (VAREST_NO_TESTSERVER=1 runs without)" >&2; exit 2; }
	mkdir -p "$(dirname "$SERVER_URL_FILE")"
	rm -f "$SERVER_URL_FILE"
	# --fixtures-dir: the benchmark fixtures of Tools/gen_bench_json.py, for Bench.VaRest.Http.RequestPath (Task 17)
	python3 -I "$SERVER_DIR/server.py" --host 127.0.0.1 --port 0 --ready-file "$SERVER_URL_FILE" --parent-pid $$ \
		--fixtures-dir "$HOST/Saved/VaRestBench" >"$SERVER_LOG" 2>&1 &
	SERVER_PID=$!
	for _ in $(seq 1 100); do
		[[ -s "$SERVER_URL_FILE" ]] && break
		kill -0 "$SERVER_PID" 2>/dev/null || break
		sleep 0.1
	done
	[[ -s "$SERVER_URL_FILE" ]] || { echo "run_tests: the test server did not start -- see $SERVER_LOG" >&2; exit 2; }
	SERVER_ARGS=("-VaRestTestServer=$(head -n 1 "$SERVER_URL_FILE")")
fi
```

3. In the editor command, after `-abslog="$LOG"`, add `${SERVER_ARGS[@]+"${SERVER_ARGS[@]}"}` (the `+` form keeps `set -u` quiet on an empty array in every bash):

```bash
	-unattended -nullrhi -nosplash -nopause -abslog="$LOG" ${SERVER_ARGS[@]+"${SERVER_ARGS[@]}"} ${VAREST_EXTRA_ARGS:-} >/dev/null 2>&1
```

4. In the `if [[ $FAILED -gt 0 ]]; then` block, after the `grep`, add:

```bash
	[[ -n "$SERVER_PID" ]] && echo "run_tests: test server log: $SERVER_LOG"
```

- [ ] **Step 7: Run and pass**

Run: `Tools/run_tests.sh VaRest.Integration`
Expected: `13 passed, 0 failed` (about 17 s: the timeouts, the limiter, the slow upload and the 8 MiB download dominate). Then `Tools/run_tests.sh --no-build VaRest` — everything green, unit and integration. Check that no `server.py` is left running afterwards: `pgrep -x python3` lists none started by the run (look at its arguments with `ps -o args= -p <pid>`; never kill by pattern).

- [ ] **Step 8: Restore-the-bug (the runner's lifecycle, decision 14)**

Run `VAREST_NO_TESTSERVER=1 Tools/run_tests.sh --no-build VaRest.Integration`; expected FAIL: every integration test reports "no test server". Then run the runner and kill it with `kill -9 <runner pid>` while the editor runs; expected: the server exits within a second (`--parent-pid`), the editor is stopped by its own `timeout` wrapper or by hand by PID. Report both.

- [ ] **Step 9: Restore-the-bug (the limiter, seen from the server)**

In `FClientState::Publish`, temporarily remove `NewChain->Add(MakeLimiterStage());`. Run `Tools/run_tests.sh VaRest.Integration.Http.Limiter`; expected FAIL ("never more than two at the server"). Restore; PASS. Report both runs.

- [ ] **Step 10: Commit**

```bash
git add Tools/testserver/server.py Tools/testserver/selftest.py Tools/run_tests.sh Source/VaRestTests/Private/VaRestIntegrationTest.cpp
git commit -m "testserver + integration: Python stdlib server with every spec 7.1 route and the WS plug-in hook, runner-owned lifecycle, VaRest.Integration.Http.*

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

### Task 17: The request-path benchmark against v1, the HTTP-thread fallback and its per-platform checklist, docs, builds, beads (spec §7.2, §7.3, §9)

Spec §7.3 deferred "the v1 request path … to M2/M3, where those pieces exist"; M3 measures it: the same fixture (`bench_1mb.json`, `bench_10mb.json` from `Tools/gen_bench_json.py`, as M1 and M2 used) fetched from the test server by v1's `UVaRestRequestJSON` and by `FVaRestClient`. v1's paths are frozen, so nothing is hooked into it; the benchmark times the game thread from outside, in its own pump loop: the core ticker (the HTTP manager, so v1's game-thread completion and its parse) and the game-thread tasks (v2's hop and callbacks), with the sleeps left out. It reports latency, game-thread milliseconds and the longest single step (the frame hitch). v1 is measured as shipped (it logs the whole body at `Log` on desktop, `VaRestRequestJSON.cpp:510-513`) and with `LogVaRest` off. Spec §9's risk gets its mitigation: a cvar that falls back to game-thread completion (the decode stays on `UE::Tasks`), and `docs/dev/http-thread-completion.md`, the checklist for Win64, Mac, iOS and Android at the release pass, with the engine facts per backend. Then the milestone checks of spec §7.2 and CLAUDE.md: the monolithic Development and Shipping game targets (Shipping compiles body logging out, Task 14), an MSVC-shadowing sweep, the module table, and beads for what M3 leaves.

**Files:**
- Modify: `Source/VaRestHttp/Private/VaRestHttpTransport.cpp` (cvar `VaRest.Http.CompleteOnGameThread`)
- Modify: `Source/VaRestHttp/Private/Tests/VaRestHttpTransportTest.cpp` (`VaRest.Http.Transport.GameThreadFallback`)
- Modify: `Source/VaRestTests/VaRestTests.Build.cs` (private dependency on v1's `VaRest`)
- Create: `Source/VaRestTests/Private/VaRestHttpRequestPathBench.cpp`
- Create: `docs/dev/http-thread-completion.md`, `docs/passport/2026-10-21-http-request-path.md`
- Modify: `CLAUDE.md` (module table, test server)

**Interfaces:**
- Consumes: `VaRest::Http::GetDefaultTransport()` and the `FDone` helper of `VaRestHttpTransportTest.cpp` (Task 7); the test server's `/fixture/<name>` and the runner's `--fixtures-dir` (Task 16); v1's `UVaRestRequestJSON` (`SetVerb`, `SetContentType`, `ProcessURL`, `OnStaticRequestComplete`, `OnStaticRequestFail`, `GetResponseObject`) and `UVaRestJsonObject` (`GetObjectField`, `GetIntegerField`).
- Produces: cvar `VaRest.Http.CompleteOnGameThread` (bool, default false, read per request); `Bench.VaRest.Http.RequestPath` writing `<Host>/Saved/VaRestBench/request-path-results.md`.

- [ ] **Step 1: The fallback's failing test** — append to `Source/VaRestHttp/Private/Tests/VaRestHttpTransportTest.cpp`, before the closing `#endif`, and add `#include "HAL/IConsoleManager.h"` at the top:

```cpp
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpTransportFallbackTest, "VaRest.Http.Transport.GameThreadFallback",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpTransportFallbackTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::TransportTestPrivate;
	AddExpectedMessagePlain(TEXT("libcurl"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	// Spec 9's fallback for a backend that cannot complete on its own thread (docs/dev/http-thread-completion.md)
	TGuardConsoleVariable<bool> Fallback(TEXT("VaRest.Http.CompleteOnGameThread"), true);
	TSharedRef<FDone, ESPMode::ThreadSafe> Done = MakeShared<FDone, ESPMode::ThreadSafe>();
	FVaRestTransportRequest Request;
	Request.Verb = TEXT("GET");
	Request.Url = TEXT("http://127.0.0.1:1/");
	Request.Timeout = 10.f;
	VaRest::Http::GetDefaultTransport()->Start(MoveTemp(Request), [Done](FVaRestTransportResult&& Result)
	{
		Done->bOnGameThread = IsInGameThread();
		Done->Result = MoveTemp(Result);
		++Done->Calls;
		Done->bDone = true;
	});
	// A game-thread completion comes from the HTTP manager's tick (HttpManager.cpp:537-544), so the core ticker is ticked
	TestTrue(TEXT("completes"), VaRest::Http::Testing::PumpUntil([Done] { return Done->bDone.load(); }, 15.0, true));
	TestTrue(TEXT("the fallback completes on the game thread"), Done->bOnGameThread);
	TestTrue(TEXT("... with the same result"), Done->Result.Failure == EVaRestTransportFailure::Connection && Done->Calls.load() == 1);
	return true;
}
```

- [ ] **Step 2: Run it to verify it fails**

Run: `Tools/run_tests.sh VaRest.Http.Transport.GameThreadFallback`
Expected: FAIL: `TGuardConsoleVariable` ensures ("Trying to set console variable \"VaRest.Http.CompleteOnGameThread\" but it doesn't exist", `IConsoleManager.h:1414`) and "the fallback completes on the game thread" fails.

- [ ] **Step 3: The switch** — in `Source/VaRestHttp/Private/VaRestHttpTransport.cpp`, add `#include "HAL/IConsoleManager.h"`, at the top of `namespace VaRest::Http::HttpTransportPrivate`:

```cpp
	TAutoConsoleVariable<bool> CVarCompleteOnGameThread(
		TEXT("VaRest.Http.CompleteOnGameThread"),
		false,
		TEXT("Spec 9 fallback for an HTTP backend that cannot complete requests on its own thread: complete them on the game thread instead. Decoding still runs on UE::Tasks. Read when each request starts; see docs/dev/http-thread-completion.md."),
		ECVF_Default);
```

and replace the policy line in `FHttpModuleTransport::Start`:

```cpp
		// Spec 1: complete on the HTTP thread; the completion below only moves bytes and dispatches. Spec 9's fallback
		// completes on the game thread instead; the completion code is the same either way.
		Http->SetDelegateThreadPolicy(CVarCompleteOnGameThread.GetValueOnAnyThread()
			? EHttpRequestDelegateThreadPolicy::CompleteOnGameThread
			: EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);
```

- [ ] **Step 4: Run and pass**

Run: `Tools/run_tests.sh VaRest.Http.Transport`
Expected: `0 failed`, `GameThreadFallback` included, and `CompletesOffGameThread` still passes (the cvar is back to false after the guard).

- [ ] **Step 5: The benchmark** — in `Source/VaRestTests/VaRestTests.Build.cs`, add `"VaRest",` after `"VaRestHttp",` with the comment `// v1, for Bench.VaRest.Http.RequestPath (its HTTP and Json dependencies are public, VaRest.Build.cs:21-31)`. Then create `Source/VaRestTests/Private/VaRestHttpRequestPathBench.cpp`:

```cpp
// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "Async/TaskGraphInterfaces.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestJsonObject.h"
#include "VaRestRequestJSON.h"
#include "VaRestTypes.h"

namespace VaRestTestsRequestPathBenchPrivate
{
	/** What one request cost, measured from outside: v1's paths are frozen, so nothing is hooked into it */
	struct FRun
	{
		double LatencyMs = 0.0;
		/** Game-thread work in the pump: the core ticker (the HTTP manager, so v1's completion and parse) and game-thread tasks (v2's hop and callbacks) */
		double GameThreadMs = 0.0;
		/** The longest single pump step: the hitch a player would see in that frame */
		double LongestStepMs = 0.0;
		bool bOk = false;
	};

	/** VaRest::Http::Testing::PumpUntil's loop, timing each step's work and leaving the sleeps out */
	void Pump(TFunctionRef<bool()> Done, FRun& Run, double TimeoutSeconds)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
		double LastTick = FPlatformTime::Seconds();
		while (!Done() && FPlatformTime::Seconds() < Deadline)
		{
			const double StepStart = FPlatformTime::Seconds();
			FTSTicker::GetCoreTicker().Tick(static_cast<float>(StepStart - LastTick));
			LastTick = StepStart;
			FTaskGraphInterface::Get().ProcessThreadUntilIdle(ENamedThreads::GameThread);
			const double WorkMs = (FPlatformTime::Seconds() - StepStart) * 1000.0;
			Run.GameThreadMs += WorkMs;
			Run.LongestStepMs = FMath::Max(Run.LongestStepMs, WorkMs);
			FPlatformProcess::Sleep(0.0002f);
		}
	}

	/** v1 as a game uses it: UVaRestRequestJSON, completed on the game thread by the engine's default policy, parsed there */
	FRun RunV1(const FString& FullUrl, int64 ExpectedCount)
	{
		FRun Run;
		UVaRestRequestJSON* Request = NewObject<UVaRestRequestJSON>();
		Request->SetVerb(EVaRestRequestVerb::GET);
		Request->SetContentType(EVaRestRequestContentType::json);
		TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Finished = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
		Request->OnStaticRequestComplete.AddLambda([Finished](UVaRestRequestJSON*) { *Finished = 1; });
		Request->OnStaticRequestFail.AddLambda([Finished](UVaRestRequestJSON*) { *Finished = 2; });
		const double Start = FPlatformTime::Seconds();
		Request->ProcessURL(FullUrl);
		Pump([Finished] { return Finished->load() != 0; }, Run, 120.0);
		Run.LatencyMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		// Checked after the clock stops: GetObjectField makes UObjects
		UVaRestJsonObject* Data = Finished->load() == 1 && Request->GetResponseObject() ? Request->GetResponseObject()->GetObjectField(TEXT("data")) : nullptr;
		Run.bOk = Data && Data->GetIntegerField(TEXT("count")) == ExpectedCount;
		return Run;
	}

	/** v2 as shipped: an FVaRestClient at its default log level, the body parsed zero-copy off the game thread */
	FRun RunV2(const FVaRestClient& Client, const FString& Path, int64 ExpectedCount)
	{
		FRun Run;
		auto Outcome = VaRest::Http::Testing::MakeOutcome<FVaRestJson>();
		const double Start = FPlatformTime::Seconds();
		VaRest::Http::Testing::Observe(Client.Get(Path), Outcome).Send();
		Pump([&Outcome] { return Outcome->IsDone(); }, Run, 120.0);
		Run.LatencyMs = (FPlatformTime::Seconds() - Start) * 1000.0;
		Run.bOk = Outcome->Value.IsSet() && Outcome->Value.GetValue().Path(TEXT("data.count")).AsInt64() == ExpectedCount;
		return Run;
	}

	double Median(TArray<double> Values)
	{
		Values.Sort();
		return Values.Num() == 0 ? 0.0 : Values[Values.Num() / 2];
	}

	struct FRow
	{
		FString Fixture;
		FString Label;
		TArray<FRun> Runs;

		double MedianOf(double FRun::*Field) const
		{
			TArray<double> Values;
			for (const FRun& Each : Runs)
			{
				Values.Add(Each.*Field);
			}
			return Median(MoveTemp(Values));
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpRequestPathBench, "Bench.VaRest.Http.RequestPath",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::PerfFilter)

bool FVaRestHttpRequestPathBench::RunTest(const FString& Parameters)
{
	using namespace VaRestTestsRequestPathBenchPrivate;
	const FString BaseUrl = VaRest::Http::Testing::GetTestServerUrl();
	if (BaseUrl.IsEmpty() || !GEngine)
	{
		AddError(TEXT("no test server: run Tools/run_tests.sh Bench.VaRest.Http, which starts Tools/testserver/server.py with the fixtures"));
		return false;
	}
	const FVaRestClient Client = FVaRestClient::Make(BaseUrl);
	TArray<FRow> Rows;
	struct FFixture
	{
		const TCHAR* Name;
		int32 Runs;
	};
	const FFixture Fixtures[] = { { TEXT("bench_1mb.json"), 9 }, { TEXT("bench_10mb.json"), 5 } };
	for (const FFixture& Fixture : Fixtures)
	{
		const FString LocalPath = FPaths::ProjectSavedDir() / TEXT("VaRestBench") / Fixture.Name;
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *LocalPath))
		{
			AddError(FString::Printf(TEXT("missing %s -- run: python3 Tools/gen_bench_json.py <Host>/Saved/VaRestBench"), *LocalPath));
			return false;
		}
		// The count the parse must reproduce, read once outside any timing
		TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(TConstArrayView<uint8>(Bytes));
		const int64 ExpectedCount = Parsed.HasValue() ? Parsed.GetValue().Path(TEXT("data.count")).AsInt64(-1) : -1;
		TestTrue(FString::Printf(TEXT("%s has data.count"), Fixture.Name), ExpectedCount > 0);
		const FString Path = FString::Printf(TEXT("/fixture/%s"), Fixture.Name);
		const FString FullUrl = BaseUrl + Path;

		// Warm-up, untimed: connections, the server's file cache, first-use allocations
		RunV1(FullUrl, ExpectedCount);
		RunV2(Client, Path, ExpectedCount);

		FRow V1Shipped;
		V1Shipped.Fixture = Fixture.Name;
		V1Shipped.Label = TEXT("v1 UVaRestRequestJSON, as shipped (logs the body on desktop)");
		FRow V1Quiet;
		V1Quiet.Fixture = Fixture.Name;
		V1Quiet.Label = TEXT("v1 UVaRestRequestJSON, LogVaRest off");
		FRow V2;
		V2.Fixture = Fixture.Name;
		V2.Label = TEXT("v2 FVaRestClient -> FVaRestJson (Basic log)");
		for (int32 Index = 0; Index < Fixture.Runs; ++Index)
		{
			// Interleaved, so drift in the machine's state spreads over all three
			V1Shipped.Runs.Add(RunV1(FullUrl, ExpectedCount));
			// The "Log" console command (LogSuppressionInterface.cpp:589-591), reached through UEngine::Exec (UnrealEngine.cpp:5533)
			GEngine->Exec(nullptr, TEXT("Log LogVaRest Off"));
			V1Quiet.Runs.Add(RunV1(FullUrl, ExpectedCount));
			GEngine->Exec(nullptr, TEXT("Log LogVaRest Log"));
			V2.Runs.Add(RunV2(Client, Path, ExpectedCount));
		}
		Rows.Add(MoveTemp(V1Shipped));
		Rows.Add(MoveTemp(V1Quiet));
		Rows.Add(MoveTemp(V2));
	}

	FString Report = FString::Printf(TEXT("# Request path: v1 vs v2 (%s)\n\nMedian of the runs; ms of wall time.\n\n"), *FDateTime::UtcNow().ToIso8601());
	Report += TEXT("| fixture | path | runs | latency ms | game-thread ms | longest step ms |\n|---|---|---|---|---|---|\n");
	bool bAllOk = true;
	for (const FRow& Row : Rows)
	{
		for (const FRun& Each : Row.Runs)
		{
			bAllOk &= Each.bOk;
		}
		Report += FString::Printf(TEXT("| %s | %s | %d | %.2f | %.2f | %.2f |\n"), *Row.Fixture, *Row.Label, Row.Runs.Num(),
			Row.MedianOf(&FRun::LatencyMs), Row.MedianOf(&FRun::GameThreadMs), Row.MedianOf(&FRun::LongestStepMs));
	}
	AddInfo(Report);
	FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("VaRestBench") / TEXT("request-path-results.md")));
	TestTrue(TEXT("every run parsed the fixture whole (data.count matches)"), bAllOk);
	// The threading rule's point (spec 1): v1 parses on the game thread, v2 only moves the result there
	const FRow* V1Big = Rows.FindByPredicate([](const FRow& Row) { return Row.Fixture == TEXT("bench_10mb.json") && Row.Label.Contains(TEXT("LogVaRest off")); });
	const FRow* V2Big = Rows.FindByPredicate([](const FRow& Row) { return Row.Fixture == TEXT("bench_10mb.json") && Row.Label.StartsWith(TEXT("v2")); });
	TestTrue(TEXT("10 MiB: v2 costs the game thread less than v1 even with v1's logging off"), V1Big && V2Big
		&& V2Big->MedianOf(&FRun::GameThreadMs) < V1Big->MedianOf(&FRun::GameThreadMs));
	return true;
}

#endif
```

- [ ] **Step 6: Run the benchmark**

Run, if `<Host>/Saved/VaRestBench/bench_1mb.json` is missing: `python3 Tools/gen_bench_json.py /w/Unreal/VaRestHost/Saved/VaRestBench`. Then three times: `Tools/run_tests.sh Bench.VaRest.Http`.
Expected: `1 passed, 0 failed` each time; `<Host>/Saved/VaRestBench/request-path-results.md` holds the six-row table of the last run (copy it after each run). If "v2 costs the game thread less than v1" fails, stop and report: that contradicts the threading rule's whole point and needs a look before M3 lands.

- [ ] **Step 7: The passport** — create `docs/passport/2026-10-21-http-request-path.md` in the form of `docs/passport/2026-10-19-json-bench.md`:

1. `# HTTP request path: v1 vs v2 (2026-10-21)`.
2. `## Machine and build`: the same two-column table (CPU from `lscpu`, RAM from `/proc/meminfo`, OS from `uname -sr`, engine, configuration `VaRestHostEditor Linux Development` with `UnrealEditor-Cmd -nullrhi`, fixtures with their byte sizes, the command `Tools/run_tests.sh Bench.VaRest.Http`, runs: 9 per path for 1 MiB and 5 for 10 MiB per invocation, three invocations).
3. `## Method`: one paragraph — the server is the local `Tools/testserver/server.py` serving the fixture from memory over loopback; the game thread is timed from outside in the benchmark's own pump (core ticker + game-thread tasks, sleeps excluded); v1 runs with the engine's default `CompleteOnGameThread` policy and parses with `FJsonSerializer` on the game thread (`VaRestRequestJSON.cpp:541-556`, `bUseChunkedParser` off by default), and logs the body at `Log` on desktop (`VaRestRequestJSON.cpp:510-513`); v2 completes on the HTTP thread and parses with `ParseOwned` on a `UE::Tasks` worker; the three paths interleave per run.
4. `## Results`: the six-row table, each cell the median of the three invocations with the three raw values in brackets (as the JSON passport does).
5. `## Reading`: three to five sentences on what the numbers show — game-thread milliseconds and the longest step for v1 as shipped, v1 quiet and v2 at 10 MiB, the share of v1's cost that is its body logging, and latency (which includes the loopback transfer and is not the claim).

- [ ] **Step 8: The per-platform checklist** — create `docs/dev/http-thread-completion.md`:

````markdown
# HTTP-thread completion, per platform (core spec §9)

Core spec §1: "Transport completes on the HTTP thread (`EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread`);
the completion only moves bytes and dispatches work." Spec §9 lists the risk: "**HTTP-thread completion** may
differ per platform backend (curl, Apple, Android). Mitigation: verify per platform in M3. If unsupported, fall
back to game-thread completion with an immediate hop to a background decode task." M3 verified Linux and built
the fallback; this page is the checklist for the other platforms at the release pass.

## What VaRestHttp relies on

1. `OnProcessRequestComplete` runs off the game thread when the policy is `CompleteOnHttpThread`, exactly once,
   also for a request the engine refuses to start.
2. The response body arrives through `SetResponseBodyReceiveStreamDelegateV2` into VaRest's own sink (reserved at
   `Content-Length + 1`, the byte of slack `ParseOwned` needs, capped at `max(MaxJsonBytes, 8 MiB) + 1`), every chunk
   before the completion. The sink has its own lock and is sealed by the completion, so the thread that writes it and
   the thread that takes it need no ordering from the engine.
3. When a trace sink wants metadata (Inspector IN-11) or a caller wants progress, `OnStatusCodeReceived` and
   `OnRequestProgress64` run off the game thread and only write atomics, and the upload goes through VaRest's counting
   archive (`SetContentFromStream`), whose `Serialize` runs on the thread that reads the body (curl's read callback
   on the HTTP thread, NSURLSession's stream thread on Apple) and also only writes atomics.
4. Nothing of VaRest runs on the HTTP thread beyond that: the completion launches a game-thread task (Task 8). A
   `SaveTo` download is written to its temp file on the thread that receives it, as the engine's own file streaming
   does.

## Backends in UE 5.8.1

| Platform | Backend | Where the engine picks it |
| --- | --- | --- |
| Linux | libcurl, `FCurlHttpRequest` | `UnixPlatformHttp.cpp:12-24` |
| Win64 | libcurl, `FCurlHttpRequest` (XCurl only with `bUseXCurl`) | `HTTP.Build.cs:30-37`; `WindowsPlatformHttp.cpp:90-97` |
| Android | libcurl, `FCurlHttpRequest` | `AndroidPlatformHttp.cpp:12-24` |
| Mac, iOS | NSURLSession, `FAppleHttpRequest` | `ApplePlatformHttp.cpp:302-316` |

All four go through the engine's HTTP thread: curl requests are threaded requests, and the Apple request adds itself
as one too (`AppleHttp.cpp:689`). The HTTP thread finishes a `CompleteOnHttpThread` request there and then
(`HttpThread.cpp:452-461`); a request refused before it starts is finished on the HTTP thread as well
(`FinishRequestNotInHttpManager`, `HttpRequestCommon.cpp:224-237`), and the status-code delegate follows the same policy (`HttpRequestCommon.cpp:629-640`).
On Apple the body bytes come from NSURLSession's delegate (`URLSession:dataTask:didReceiveData:`,
`AppleHttp.cpp:194-206`, on the session's serial delegate queue, `ApplePlatformHttp.cpp:265`) into
`PassReceivedDataToStream` (`AppleHttp.cpp:884-888`), so the sink is written on that queue and read at completion on
the HTTP thread, which learns of the completion through a plain property (`AppleHttp.cpp:220-233`, `:948-952`). The
sink synchronizes the delegate queue and the HTTP thread itself (its lock; review C-4), so item 2 holds by
construction; the byte-exact tests below still check it.

Timeouts on Apple: NSURLSession's `timeoutInterval` is the engine's connection timeout (`AppleHttp.cpp:738`, `[HTTP]
HttpConnectionTimeout`, 30 s by default), and it bounds silence whatever activity timeout a request sets. A
`.Timeout(60)` against a server silent for 40 s therefore fails at about 30 s on Mac and iOS, as a connection error.
On curl VaRest sets the engine's activity timeout to the total timeout when no activity timeout is given (decision
30), so there the total governs. A game that needs long silences on Apple raises `HttpConnectionTimeout` in its
`[HTTP]` section.

## Verified

| Platform | Date | How | Result |
| --- | --- | --- | --- |
| Linux (curl) | M3 | `VaRest.Http.Transport.CompletesOffGameThread`, `VaRest.Integration.Http.*` (`Tools/run_tests.sh`) | completes off the game thread; bodies byte-exact |
| Win64 | release pass | the checklist below | |
| Mac | release pass | the checklist below | |
| iOS | release pass | the checklist below | |
| Android | release pass | the checklist below | |

## Checklist for one platform

1. Start the test server where the device can reach it:
   `python3 -I Tools/testserver/server.py --host 0.0.0.0 --port 8642 --fixtures-dir <dir>`.
2. Run on the device or desktop target, with `-VaRestTestServer=http://<host>:8642`:
   `Automation RunTests VaRest.Http.Transport; Automation RunTests VaRest.Integration.Http; Quit`.
3. Required: `VaRest.Http.Transport.CompletesOffGameThread` passes (item 1), and `VaRest.Integration.Http.LargeDownload`,
   `.SaveTo`, `.Bodies` and `.ChunkedRedirectCap` pass (item 2: every byte, in order, with the slack).
   `VaRest.Integration.Http.TraceAndLogs` passes (item 3).
4. Record the row above (date, engine build, device).

## When a platform fails

Set the fallback for that platform only, in its config (for example `Config/Mac/MacEngine.ini`, or a device profile):

```ini
[ConsoleVariables]
VaRest.Http.CompleteOnGameThread=1
```

The transport then asks for `CompleteOnGameThread` (read when each request starts). The completion code is the same;
it runs from the HTTP manager's game-thread tick instead, and still only moves the body into a task, so decoding stays
off the game thread (spec §9's fallback). `VaRest.Http.Transport.GameThreadFallback` covers the switch. File a bead with
the failing test and the platform, and add the row above.
````

- [ ] **Step 9: CLAUDE.md** — in the `Build & Test` code block, after the `Tools/run_tests.sh --no-build VaRest` line, add:

```bash
python3 -I Tools/testserver/selftest.py   # the HTTP test server's own tests (stdlib only)
```

and after the code block, before **Dev-loop hazards**, add the paragraph:

```markdown
`Tools/run_tests.sh` starts `Tools/testserver/server.py` (python3, standard library) on a port the OS picks and passes
`-VaRestTestServer=<url>` for the `VaRest.Integration.*` tests; it stops it on exit and the server leaves by itself if the
runner dies. `VAREST_NO_TESTSERVER=1` runs without it (integration tests then fail, by design). Server log:
`<Host>/Saved/Logs/VaRestTestServer.log`.
```

In the module table, replace the `VaRestHttp` row and add one after `VaRestEditorTools`:

```markdown
| `VaRestHttp` | Runtime | `FVaRestClient` and its `[[nodiscard]]` builder over a stage pipeline (log/redaction, limiter, transport; M4 adds retry, auth, cache, offline), HTTP-thread completion with zero-copy JSON decode off the game thread, the public redactor, the Inspector trace surface, `UVaRestHttpSettings`; M5 adds the `UVaRestApi` asset, `UVaRestHub` and async BP actions |
| `VaRestTests` | UncookedOnly | test-only USTRUCTs, the `VaRest.Integration.*` suite against `Tools/testserver/`, `Bench.VaRest.Http.*`; never in a game target |
```

In `Conventions & Patterns`, after the line that freezes the v2 Blueprint-visible surface, add (review M16):

```markdown
- **`UVaRestHttpSettings` property names are frozen from M3**: they are persisted config keys
  (`[/Script/VaRestHttp.VaRestHttpSettings]` in a project's ini). `FVaRestError`'s fields, `EVaRestErrorKind`,
  `EVaRestPriority`, `EVaRestLogLevel` and `EVaRestQueryStyle` freeze with M5, when they become pin names. A rename
  needs a CoreRedirect (or a config redirect) in the same commit.
```

(`AGENTS.md` has neither section, so nothing is mirrored there.)

- [ ] **Step 10: The milestone builds** (CLAUDE.md: "Monolithic game target -- a different code path; check it before a milestone lands"; spec §7.2: "monolithic Development and Shipping game targets")

Run:

```bash
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Development -project=/w/Unreal/VaRestHost/VaRestHost.uproject
/w/Unreal/UnrealEngine/Engine/Build/BatchFiles/Linux/Build.sh VaRestHost Linux Shipping -project=/w/Unreal/VaRestHost/VaRestHost.uproject
```

Expected: both succeed with no warning from `Source/VaRestHttp` (Shipping compiles out the body excerpts of the log stage and the decode counters' test hooks stay valid). `VaRestTests` is not in either: it is `UncookedOnly`.

- [ ] **Step 11: The MSVC sweep** (Global Constraints; only clang builds here)

Run (Python, because the shell's `grep` on this workstation is ugrep, whose `-E` has no back-references):

```bash
python3 - Source/VaRestHttp Source/VaRestTests Source/VaRestJson/Private/VaRestJsonRedact.cpp <<'PY'
import pathlib, re, sys
capture = re.compile(r'\[(?:[^\]]*[ ,])?([A-Za-z_]\w*) = (?:MoveTemp\()?\1\)?[\],]')
member = re.compile(r'^\s+(?:const )?[\w:<>, ]+[ &*](Config|State|Url|Verb|Body|Tag|Priority|Owner|Attempt|InFlight|Limiter) = ')
declared = re.compile(r'^\s+(?:const )?(?:auto|[A-Z][\w:]*(?:<[^;=()]*>)?)[ &*]+([A-Za-z_]\w*)(?: = |\(|\{| : )')
owners = {"VaRestClient.cpp", "VaRestClientState.cpp", "VaRestPipeline.cpp", "VaRestLogStage.cpp", "VaRestTracePoints.cpp"}
for root in sys.argv[1:]:
    paths = [pathlib.Path(root)] if root.endswith(".cpp") else sorted(pathlib.Path(root).rglob("*.[ch]*"))
    for path in paths:
        lines = path.read_text(encoding="utf-8").splitlines()
        # Review I2 (MSVC C4456): a local declared inside a lambda body with the name of a local of the function around
        # it. Approximation: in each function (a line starting at column 0 that ends a signature, up to the next "}" at
        # column 0), a name declared twice where the second declaration is deeper inside a "[...](...) ... {" block.
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

Expected: no `init-capture reuses a name` line (`[Result = MoveTemp(Result)]`: C4456/C4458 are errors in this build, `CppCompileWarnings.cs:88-90`). A `check: a local named like a member` line matters only inside a member function of `FVaRestExchange`, `FClientState` or `FVaRestClient` (C4458); a free function or another class may use the name. A `check: a lambda local hides an enclosing local` line is the class of review I2 (`FBodySink& Sink` inside a lambda of a function with a `Sink`; clang flags it only with `-Wshadow-uncaptured-local`): read each one. Rename what is real (`Moved`, `Pending`, `Received`) and rebuild.

- [ ] **Step 12: The whole suite**

Run: `Tools/run_tests.sh VaRest` and `python3 -I Tools/testserver/selftest.py`.
Expected: `0 failed` (Json, Http, Integration); `OK`.

- [ ] **Step 13: Beads for what M3 leaves**

```bash
bd create --title="Move VaRestJson test USTRUCTs into VaRestTests" --type=task --priority=3 \
  --description="M3 plan decision 15: test-only USTRUCTs belong in the UncookedOnly module VaRestTests so they never reach a game's reflection data (VaRest-rfd). VaRestJson's existing test types stayed put in M3 because their tests include VaRestJson private headers; move the types and those tests together."
bd create --title="Release pass: HTTP-thread completion and byte-exact bodies on Win64, Mac, iOS, Android" --type=task --priority=2 \
  --description="Core spec 9 risk. Follow docs/dev/http-thread-completion.md on each platform (VaRest.Http.Transport.*, VaRest.Integration.Http.* against Tools/testserver/server.py --host 0.0.0.0), record the row, and set VaRest.Http.CompleteOnGameThread=1 in that platform's config if completion or the body sink fails. Apple writes the body from the NSURLSession delegate queue (AppleHttp.cpp:194-206, 884-888): check LargeDownload/SaveTo there first."
bd create --title="Inspector store: treat Client{Created} for a known id as a rename" --type=task --priority=3 \
  --description="M3 plan decision 23: FVaRestClient::WithName after Make emits Client{bCreated=true} again for the same ClientId (IN-8 has no rename event). The Inspector store must update the row instead of adding one."
```

Report the three ids.

- [ ] **Step 14: Commit**

```bash
git add Source/VaRestHttp/Private/VaRestHttpTransport.cpp Source/VaRestHttp/Private/Tests/VaRestHttpTransportTest.cpp \
  Source/VaRestTests/VaRestTests.Build.cs Source/VaRestTests/Private/VaRestHttpRequestPathBench.cpp \
  docs/dev/http-thread-completion.md docs/passport/2026-10-21-http-request-path.md CLAUDE.md
git commit -m "http: request-path benchmark vs v1 (passport), CompleteOnGameThread fallback and the per-platform checklist (spec 9), docs

Co-Authored-By: Claude <Model> <noreply@anthropic.com>"
```

---

## After M3

What later milestones take from here, so their plans start from code that exists:

- **M4 stages** go in `FClientState::Publish` between the custom `Outer` stages and `MakeAttemptBoundaryStage()` (offline queue, cache, retry; decision 13), and auth before the custom `PerAttempt` stages. Retry declares `MayReissue()` (decision 12) and reads `FVaRestError::bRetryable`, `FVaRestExchange::IsIdempotent()` and the stable `Idempotency-Key` (Task 9). Backoff uses `IVaRestClock::SetTimer` (Task 6) with the injectable `FRandomStream` M4 adds (WebSocket §19 item 1). Auth calls `FVaRestClient::SetSecretValue` for each token (Task 14) and adds its token paths and header/query names to the redactor's keys (IN-13 layer 2). Added keys match whole normalised keys (decision 18), so for a `BearerRefresh` token path such as `data.auth.refresh_token` auth adds the last segment, `refresh_token`, not the path (review M19).
- **M4 trace**: `Note`, `Excerpt` (IN-14: the request prefix copied at `Send`, the response redacted on the decode task before `ParseOwned`, an undecoded one on the redaction pipe, the admission counter), `Marker`, `GapBefore`/`AuthWait`/`AuthRefreshId` in `AttemptDispatched`, the cache fields of `Started`, `Envelope` in `Completed`, and the IN-17 goldens `VaRest.Http.Trace.*` beside Task 15's. `bExcerptPending` stays false until then. `FVaRestTraceEvent::Source` is already there (WebSocket §19 item 4). Decode-task excerpts launch on the redaction pipe off the game thread, so M4 first turns `GetRedactionPipe()` into an atomic pointer with a shutdown flag the launchers check (review M-10; Task 5 checks the game thread until then).
- **M4 envelope**: decode errors already carry `Path`; the envelope's evaluator runs in `RunDecode` (Task 10) before the handler's `Decode`.
- **M5 Blueprint decode** (decision 22): decoding into a `UUserDefinedStruct` off the game thread keeps the struct alive for the whole task (`TStrongObjectPtr<const UScriptStruct>` or an `FGCObject`), decodes into memory the task owns (the `FInstancedStruct`), and fetches converted-field lists per use, never cached across a recompile (`UserDefinedStructureCompilerUtils.cpp:225`). Blueprint nodes then pass their initiator through IN-15 (Task 15 reads the frame when `Send` runs on the game thread; its test lands with the nodes). `FVaRestError`'s fields and `EVaRestErrorKind`'s values become pin names then: freeze them with M5's surface (the CLAUDE.md line Task 17 adds). Blueprint async actions decide whether their delegates fire at a frame boundary or, like the C++ callbacks, from game-thread tasks that can run between tick groups (decision 32, review M-12); all deliveries go through `PipelinePrivate::OnGameThread`, so the change is local.
- **M5 settings**: the rest of spec §5.5 joins `UVaRestHttpSettings` (Task 2); hubs open scopes beyond scope 0 (IN-7) through `FVaRestTrace::OnBuildSnapshot`.
- **W0/W1** (WebSocket §16.3, §17): `ws.py` registers with `server.register_upgrade("websocket", handler)` and shares `STATE`/`LOCK` (Task 16); the TLS listener and `certs/` are W0's. W1 reuses `PumpUntil`, `FLogCapture`, the runner's server lifecycle and the redactor (WebSocket §19 item 3).
- **Known limits carried**: a request canceled while still in phase `Created` (sent from a worker, not yet on the game thread) is not reached by `CancelAll` (it is not in flight yet) and completes canceled only if its own handle was canceled; the body of a non-JSON error response is kept to 4 KiB of text (`RawBodyCap`, Task 8), and a JSON body that failed to parse to its first 4 KiB (decision 26); progress is throttled to 10 Hz on the game thread (Task 13); `SaveTo` writes on the HTTP thread (decision 31: a slow disk stalls it; a bounded pipe can go behind `FBodySink` without an API change); on Mac and iOS a silence longer than `[HTTP] HttpConnectionTimeout` fails the request whatever its timeouts (`docs/dev/http-thread-completion.md`); the limiter's strict priority can starve `Low` under sustained load (spec §4.6, review M-19).
