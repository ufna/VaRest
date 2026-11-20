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
   `Content-Length + 1`, the byte of slack `ParseOwned` needs; the reserve is capped at `MaxJsonBytes + 1` when the result
   has a JSON cap, else at 8 MiB + 1, `FBodySink::ReserveCap`), every chunk before the completion. The sink has its own
   lock and is sealed by the completion, so the thread that writes it and the thread that takes it need no ordering
   from the engine.
3. When a trace sink wants metadata (Inspector IN-11) or a caller wants progress, `OnStatusCodeReceived` and
   `OnRequestProgress64` run off the game thread and only write atomics. On curl backends a progress-watched upload goes
   through VaRest's counting archive (`SetContentFromStream`), whose `Serialize` runs in curl's read callback on the
   HTTP thread and also only writes atomics. On Mac and iOS it does not: NSURLSession reports upload progress itself
   (`AppleHttp.cpp:161-168`, `:954-962`), and a body stream there could not be resent on a redirect or an auth
   challenge (decision 29, review N-1).
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

Timeouts on Apple: NSURLSession's `timeoutInterval` is the engine's connection timeout (`AppleHttp.cpp:736-738`, `[HTTP]
HttpConnectionTimeout`, 30 s by default, `BaseEngine.ini:40`), and it bounds silence whatever activity timeout a request
sets. A `.Timeout(60)` against a server silent for 40 s therefore fails at about 30 s on Mac and iOS, as a connection
error. On curl VaRest sets the engine's activity timeout to the total timeout when no activity timeout is given
(decision 30, `EffectiveActivityTimeout`), so there the total governs. On Apple that same activity timeout, whenever it
is above `HttpConnectionTimeout`, makes the engine log a Warning on every such request (`AppleHttp.cpp:740-749`):
"HttpConnectionTimeout can't be less than HttpActivityTimeout, otherwise requests may complete unexpectedly with
ConnectionError after %.2f(HttpConnectionTimeout) seconds without activity, instead of intended %.2f(HttpActivityTimeout)
seconds". `.Timeout(60)` without an activity timeout is one such request (task 7 review M-9). A game that needs long
silences on Apple raises `HttpConnectionTimeout` in its `[HTTP]` section, which also silences the Warning.

## Platform notes and residuals

What M3 knows but could not run, in the order the release pass should check it.

**Mac and iOS**

1. **Byte-exact bodies first.** The `FBodySink` mutex and its seal close the Apple body-ordering risk by construction:
   every `Write` on the delegate queue takes the mutex, `TakeBytes`/`CloseFile` on the HTTP thread take it too and seal
   the sink, so a chunk is either in the body or refused, never torn or appended after the take (review C-4, task 7
   review). By construction is not by test: `VaRest.Integration.Http.LargeDownload` and `.SaveTo` (every byte, in order)
   stay first on the Mac and iOS list.
2. **The 30 s silence cap and its Warning**, above. `VaRest.Integration.Http.Timeouts` uses short timeouts; a silence
   beyond 30 s is not in the suite.
3. **The bodyless-response residual.** `UploadReadFailed` (a body file or segment that could not be read; the engine
   sends zeroes and ignores the archive's error, `HttpRequestPayload.cpp:86-96`) is checked at completion, on the
   HTTP thread. On Apple the body stream is read by NSURLSession on a worker of its own (`AppleHttp.cpp:402-421`), so
   the check relies on NSURLSession completing the task after its last body-stream read; nothing in Apple's documentation
   promises that order. A response with a body is ordered anyway: its chunks go through the sink mutex after the upload
   ended. A bodyless response (204, `HEAD`, an empty 200) has no such chunk, so there the order rests on NSURLSession
   alone. Check: an upload of a `BodyFile` that is deleted or truncated while it is sent must fail with
   `UploadReadFailed`, also against `/status/204`.
4. **Upload progress is the engine's.** Counted uploads (`FCountingUpload`) are curl only (`ShouldCountUpload`,
   `PLATFORM_APPLE`). On Apple `OnProgress` and the trace's `BytesSent` come from NSURLSession's `didSendBodyData`
   (`AppleHttp.cpp:161-168`), reported when the HTTP thread ticks the request (`:954-962`). `UploadDoneAt` is stamped
   from that progress only when the request has a body (`ShouldStampUploadFromProgress`; a bodyless request leaves it at
   -1, task 7 review I-1). Check `VaRest.Integration.Http.UploadProgress` and the trace's upload phase.
   That test's last check, that the final `OnProgress` shows `BytesSent == UploadTotal`, is a curl property: curl's finish
   reports progress once more (`CurlHttp.cpp:1274`), Apple's `FinishRequest` does not (`AppleHttp.cpp:777`), so on Apple the
   last call may show less, and the test skips that check there (bead VaRest-3dj).
5. **Stream bodies cannot be resent.** A multipart or `BodyFile` body goes to the engine as a stream
   (`SetContentFromStream`), which NSURLSession sends as `HTTPBodyStream` (`AppleHttp.cpp:627-634`, `:751-753`); the
   engine implements no `needNewBodyStream`, so a 307/308 or an auth challenge that must resend it fails. Check one
   multipart POST to `/status/307?location=/echo`.
   VaRest's own re-issues -- a retry (M4a Task 8) and a session replay after a refresh (Task 17) -- are new engine requests
   with a new stream (M4a plan decision 24), so they are not affected; only a resend NSURLSession attempts by itself is.
   Check `VaRest.Integration.Http.RetryResendsStreamBody` on the device: a multipart `.Idempotent()` POST with a file part
   through `/retry-after/1`, whose second attempt must bring the whole body again -- the route's 200 echoes the bytes it
   received. (`VaRest.Http.Auth.ReplayBodies` and `VaRest.Http.Retry.BodiesResent` run on the fake transport, so they
   cannot see NSURLSession re-read a stream; M4a final review A8.)

**Win64**

1. **`SaveTo` replace is not atomic.** `MoveFileW` without flags does not replace an existing file
   (`WindowsPlatformFile.cpp:1216-1219`), so on Windows only `MoveIntoPlace` moves the old file aside to a unique
   `.bak`, moves the new one in, then deletes the `.bak` (`VaRestDecode.cpp:327-354`, `PLATFORM_WINDOWS` at `:408`;
   documented on `SaveTo`, `VaRestClient.h:348-359`). A crash between the two moves leaves the old file only as that
   `.bak`. Every other platform makes one `rename()`, which replaces atomically. Check `VaRest.Integration.Http.SaveTo`
   and `VaRest.Http.Decode.SaveTo*` on Win64, onto an existing destination.
2. curl as on Linux; the checklist below covers the rest.

**Android**: curl as on Linux; the checklist below covers it.

**Every platform**

1. **A URL with `://` twice warns once, unredacted.** In non-Shipping builds `FHttpManager::IsDomainAllowed` runs
   `CheckNoDuplicatedProtocol` (`HttpManager.cpp:279-297`), which logs the whole URL as a `LogHttp` Warning when `://`
   appears twice (`:286`): an absolute URL written into a path's query (a `Query()` value is percent-encoded and has
   none). The engine's own `PreCheck` makes that call once per request (`HttpRequestCommon.cpp:109`, from `PreProcess`,
   `:184`), with the raw URL -- an ApiKey in the query included. It is the engine's line: the redactor never sees it.
   VaRest's PreCheck mirror hands the filter only `scheme://authority` (`DomainFilterInput`; the filter decides on the
   scheme and the domain alone, `URLRequestFilter.cpp:95-160`), so it adds no second line (task 2 review minor 3).
   `VaRest.Integration.Http.DuplicatedProtocolWarnsOnce` counts 1 per request.

## Verified

| Platform | Date | How | Result |
| --- | --- | --- | --- |
| Linux (curl) | M3 (2026-10-21) | `VaRest.Http.Transport.CompletesOffGameThread`, `.GameThreadFallback`, `VaRest.Integration.Http.*` (`Tools/run_tests.sh`) | completes off the game thread; the fallback completes on it; bodies byte-exact |
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
4. The platform's notes above.
5. Record the row above (date, engine build, device).

## When a platform fails

Set the fallback for that platform only, in its config (for example `Config/Mac/MacEngine.ini`, or a device profile):

```ini
[ConsoleVariables]
VaRest.Http.CompleteOnGameThread=1
```

The transport then asks for `CompleteOnGameThread` (read when each request starts). The completion code is the same;
it runs from the HTTP manager's game-thread tick instead (`HttpManager.cpp:537-544`), and still only moves the body into
a task, so decoding stays off the game thread (spec §9's fallback). `VaRest.Http.Transport.GameThreadFallback` covers the
switch. Under the fallback the engine also moves the header, status-code and progress delegates to the game thread
(`CurlHttp.cpp:532-539`, `:1212-1219`; `AppleHttp.cpp:852-858`, `:871-881`; `HttpRequestCommon.cpp:629-640`), while body chunks still arrive on the
receiving thread. So a response's first chunk can come before VaRest learns of a `Content-Encoding` (review M-16): the
sink then reserves by the compressed `Content-Length` (harmless, it grows) and progress reports that compressed length as
the download total; trace times are those of the game-thread tick. Under the fallback a `SaveTo` download's
`CloseFile()` (the temp file's flush and close, `VaRestHttpTransport.cpp:386-393`), and the file write of an engine mock
response (`:369-375`), run in that completion on the game thread: a small disk IO there, in fallback mode only. A request
the engine refuses before it starts is then finished synchronously inside `ProcessRequest` on the game thread
(`FinishRequestNotInHttpManager`, `HttpRequestCommon.cpp:226-231`), not on the HTTP thread; the pipeline's completion
already hops to a game-thread task, so nothing is delivered inside `Send()`. File a bead with the failing test and the
platform, and add the row above.

## The curl transport may resend a request below VaRest's retry (bead VaRest-rrp)

When a reused keep-alive connection yields zero response bytes, libcurl's `Curl_retry_request` (`lib/transfer.c:690-735`
in the engine's 8.12.1 source; the engine ships 8.15.0) resends the request on a fresh connect, up to `CONN_MAX_RETRIES`
(5), and the retry may pick another pooled connection. It does not look at the method: uploads and POST are resent too,
provided the body can be rewound. This happens inside one transport attempt, below VaRest's retry policy, so it is
invisible to `Response.Attempts` and to the Inspector, and a server may see the request more than once. The only
defence for a non-idempotent call is `.Idempotent()` plus a server-side Idempotency-Key. The engine's
`http.CurlReuseConnectionEnabled=false` (`CurlHttp.cpp:204-207`, `CurlHttpManager.cpp:131-141`) turns reuse off for new
connections but does not evict connections already idle in the pool; `VaRest.Integration.Http.Retry` sends a burst of
requests under it first for that reason.

## M5 dynamic results: GT materialization and frame-terminal completion

The platform policy above controls transport completion. Its background-decode description does not move M5
reflected conversion off the game thread. `FVaRestClient::RequestDynamic(Verb, Path, Config&&)` takes
`FVaRestDynamicResultConfig` and returns `TVaRestRequest<FVaRestDynamicResult>`; include
`VaRestDynamicRequest.h`. Activation is GT-only. Its native work parses/envelopes bytes using captured plain
options; reflected initialization, conversion, preparation and destruction run on GT. Dynamic
`.OnHttpThread()` is refused as Config. Ordinary compile-time `Request<T>` keeps its existing decode and delivery
contract; this new dynamic lifecycle registry does not change it.

### Ordering and terminal exceptions

An admitted dynamic request reserves its budget and completion frame before retaining work. Readiness in frame N
cannot invoke that completion in N: the GT dispatcher uses a finite ready snapshot and requires an older
`GFrameCounter`. A worker only signals readiness; GT stamps the frame. A completion becoming ready during a drain
waits for a later drain.

On that later turn, the pipeline checks cancellation, owner and managed-gate state; finalizes a usable result;
performs the private typed preparation; and resets the private reflected carriers. It samples type/rule validity
and cancellation/owner/gate state again after finalizer, clock and cleanup callouts. Only then does it accept the
terminal outcome, finish accounting and emit `Completed`. User delivery follows `Completed`; moving only the
Blueprint callback to a later frame would not satisfy this ordering.

Admitted local Config/error outcomes, cancellation and queued acknowledgements use the same completion policy.
Queued acknowledgement and canceled/silent outcomes skip reflected materialization; queued delivery reports the
queue ID rather than a typed success. Owner loss, Hub/client retirement or HTTP shutdown can suppress user
delivery. Cancellation preserves a file/queue commit already selected by the native pipeline; it does not turn an
actual committed side effect into an uncommitted one. Refusal before frame admission uses ordinary asynchronous
Config notification, without an admitted frame-slot guarantee or coverage by the admitted-operation count.

### Editor and cooked availability

Editor materialization requires current pre-change/transactor bindings. Struct and enum pre-change, undo/redo
before deserialization, package reload before fixup, target Blueprint precompile, and rule changes invalidate the
GT state before old reflected layouts are used. The editor waits for a strictly later settling frame. Missing or
replaced required editor bindings refuse typed activation with `Config/EditorMutationHooksUnavailable`;
a stale success before terminal acceptance becomes `Config/DynamicTypeChanged`. A valid type whose JSON does
not fit produces Decode diagnostics; finite parser/converter admission refusal uses `Config/DecodeBudget`.
Typed error-body conversion preserves the original primary error and its raw response diagnostics.

Cooked dynamic reflected conversion also uses the GT path. It validates the complete supported transitive closure,
including Optional values, container members, enum references and required imported rules. A project/native struct
needs an explicit GT lifecycle declaration through
`FVaRestDynamicNativeTypes::RegisterGameThreadType(Type, Contract, NativeTypeDependencies)` and corresponding
`UnregisterGameThreadType(Type)`. The declaration covers initialization/defaults, copy, destruction, relevant
hash/equality and nonreflected dependencies; it is a project qualification obligation, not an inference from
`IsNative`, a root pin or zero-constructor flags. It does not enable reflected worker conversion. Cooked reflected
workers remain disabled; the accepted GT fallback is distinct from a qualified worker closure and its shutdown
contract. Callers can select raw JSON/text/bytes without reflected materialization; an admission refusal does not
promise a response body.

### Native shell and the private GT Take

`FVaRestDynamicResult` is a copyable immutable native shell. `GetType()` and `GetErrorType()` return plain
`FVaRestDynamicTypeInfo`; `GetJson()`, `GetErrorJson()`, `GetText()` and `GetBytes()` expose native payloads.
The shell owns no UObject/type/property pointer, reflected carrier, config/clock, budget lease or GT release token.
Copies and final destruction may occur on any thread. The byte view is borrowed for the lifetime of an owning shell
copy. JSON retains its existing shared-document contract: clone before independent or concurrent mutation.
`GetText()` is raw String mode; JSON-value String/Bytes payloads are exposed as JSON and through internal typed
preparation. `HasBody()` describes retained payload, not request success: `TVaRestResult::Error` is authoritative.

There is no public retained-shell getter for reflected output. The generated action expansion uses a noncopyable
private `FDynamicPrepareToken` during one synchronous GT preparation. Its `Take` checks the current descriptor and
destination, copies immediately and resets the carrier even on refusal; no destination address is retained.
Confirmation and the generated continuation's request cookie permit one matching consumer. Preparation ends and
all private carriers are reset before `Completed`. Native shell retention cannot keep or reopen them.
`UVaRestRequestAction::GetNativeResult()` and `TakeNativeResultOnGT()` return the same plain shell, not this
preparation capability. The Blueprint-internal `TakeResponse`/`TakeErrorBody` thunks are not general late getters.

### Availability after the terminal cutoff

The two availability enums have different roles:

| API | Frozen values and meaning |
| --- | --- |
| `FVaRestDynamicTypeInfo::Availability` (`EVaRestDynamicAvailability`) | `None`, `Available`, `Unavailable`, `TypeChanged`, `BudgetRefused`: plain declared-output validation/preparation metadata. `Available` is not a retained reflected value or a promise that a later Blueprint destination is still current. |
| `UVaRestRequestAction::GetTypedOutputUnavailable` / `OnTypedOutputUnavailable` (`EVaRestTypedOutputUnavailableReason`) | `TypeChangedAfterCompletion`, `TargetChangedAfterCompletion`, `OutputSuperseded`, `OutputNotBound`: the accepted result cannot be transferred to that typed action continuation. |

A `Completed` listener may edit a type, recompile the target or retire its owner. After that terminal cutoff the
pipeline does not rewrite an accepted Success into Config. It rechecks the action's current target/type and
one-shot cookie before user execution. A stale typed continuation reports the explicit unavailable reason with
the original HTTP/error metadata when the target can still receive it; owner/Hub/module closure can instead
suppress delivery. It never substitutes a default typed success. Retained native shell metadata is a snapshot,
not a live reflected-availability query. Action metadata/raw getters remain subject to their GT API contract.

### Bounds and lifetime limits

The limits cover admitted count, retained input, parser nodes/work and converter requested/logical output.
They do not prove a universal heap, RSS, CPU or latency ceiling: authored defaults/native lifecycle functions,
allocator/container slack, engine-owned destination copies, upstream buffers and caller-retained shells can add
cost. Caller-created actions and refused asynchronous notifications are outside the admitted-state count.
`SaveTo` remains streaming; ordinary dynamic `SaveTo` requires a live multithreaded native scheduler.

Module-owned GT payloads dispose callbacks, preparations and reflected state on GT. Native worker tickets and
retained public shells are separate from those affine owners. Keep externally held managed client/builders and
affine configuration within their GT retirement contract and release them before HTTP Stop; a weak client identity
only becomes a normal strong client when `PinOnGT` succeeds. This does not promise forced runtime DLL unmapping,
arbitrary global scheduler cancellation or a global task-graph drain. The accepted bounded mapped-exit retention
of unresolved native work is not reflected-worker qualification.

The declarations and ordering above are defined by the current
[dynamic API](../../Source/VaRestHttp/Public/VaRestDynamicRequest.h),
[native shell](../../Source/VaRestHttp/Public/VaRestDynamicResult.h),
[GT lifecycle declaration](../../Source/VaRestHttp/Public/VaRestDynamicTypes.h) and
[action availability API](../../Source/VaRestHttp/Public/VaRestRequestAction.h).
