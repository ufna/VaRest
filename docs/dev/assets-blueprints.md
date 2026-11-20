# API assets and Blueprint requests

`VaRestHttp` supplies API assets, per-owner clients and Blueprint request actions. An API asset describes a service; a client holds its live environment, credentials and request state. This guide describes the implemented interface. Build, runtime and release qualification remain in the [development passport](../passport/2026-11-04-varest2-development.md).

## Configure an API

Create a `UVaRestApi` asset and configure:

- `Environments`: named `BaseUrl` values, optional `SocketBaseUrl` and `QueueNamespace`.
- `DefaultEnvironment` and, where needed, `ShippingEnvironment`.
- Shared `Headers`, `UserAgent`, `Auth`, retry, timeout, concurrency, cache/offline, envelope, JSON and logging policies.
- `Endpoints`: each endpoint's verb/path, parameters, body, response/error type and optional policy overrides.
- `JsonTypes` and `TypesFolder` for runtime JSON rules. `Sockets` contains protocol-specific entries.

Endpoint `Id` is an `FGuid` assigned at creation. Preserve it when renaming an operation; `Name` is its human-facing lookup name. The asset's retry default is `FVaRestRetryPolicy::Exponential(3)`. Endpoint override toggles select which values replace the asset policy. [Asset declaration](../../Source/VaRestHttp/Public/VaRestApi.h), [endpoint declarations](../../Source/VaRestHttp/Public/VaRestApiTypes.h).

The initial environment selection order is `-VaRestEnv.<AssetName>=...`, then `-VaRestEnv=...`, then `ShippingEnvironment` in Shipping when set, otherwise `DefaultEnvironment`. Selection must name a declared environment. Use the live client's `SetEnvironment` to change it at runtime and handle refusal. An environment switch also changes the durable queue target; it does not authorize replay by itself. [Selection and validation](../../Source/VaRestHttp/Private/VaRestApi.cpp).

List startup API assets in Project Settings → VaRest HTTP → `Apis`. The serialized section/key is:

```ini
[/Script/VaRestHttp.VaRestHttpSettings]
+Apis=/Game/Backend/Api.Api
```

This is an example object path. When replacing an existing array deliberately, Unreal's `!Apis=ClearArray` followed by the desired `+Apis` entries selects the replacement list. There is no `StartupApis` setting. The engine subsystem loads and retains configured assets before game instances in normal startup, so their `PostLoad` rule registration is available to early default-options conversion. Cooking enumerates these packages through VaRest's `ModifyCook` delegate; the cook commandlet intentionally skips the subsystem's runtime preload. Keep `Apis` current after moving/deleting assets: its `Untracked` soft-reference policy does not promise automatic settings fixup. [Settings](../../Source/VaRestHttp/Public/VaRestHttpSettings.h), [startup owner](../../Source/VaRestHttp/Private/VaRestEngineSubsystem.cpp), [cook enumeration](../../Source/VaRestHttp/Private/VaRestHttpModule.cpp).

## Get the owner and client

`UVaRestHub` is a `UObject` facade. `UVaRestGameInstanceSubsystem` owns the Hub of a real GameInstance; `UVaRestEngineSubsystem` owns the independent engine/editor fallback. Both keep their Hub through reflected ownership. No artificial GameInstance is created.

Use `UVaRestHub::Get(WorldContextObject)` for the relevant owner, or `GetForEditor()` for an editor utility. `GetClient(Api)` returns that Hub's API client; `MakeClient(BaseUrl)` and `GetDefaultClient()` provide clients without an API asset. Check for a null/refused client during shutdown or invalid configuration. Different GameInstances have distinct managed scopes and connectivity. A process-scoped native client using default scope 0 does not provide that isolation. [Hub](../../Source/VaRestHttp/Public/VaRestHub.h), [GI host](../../Source/VaRestHttp/Public/VaRestGameInstanceSubsystem.h), [engine host](../../Source/VaRestHttp/Public/VaRestEngineSubsystem.h).

`UVaRestApiClient` holds the selected environment and a per-client duplicate of the asset's auth configuration. Runtime tokens, session headers and account choices belong to this client, never to the shared asset. Its live interface includes `SetEnvironment`, `SetBearerToken`, `SetSession`, `ImportSession`, `ExportSession`, `ClearSession`, credential setters, header setters, `CancelAll` and `ClearCache`. `OnSessionChanged`, `OnSessionExpired`, `OnQueuedResult` and `OnError` are client events. Client `OnError` reports a delivered primary request error before its individual handler; typed-output availability has its own event. [Client declaration](../../Source/VaRestHttp/Public/VaRestApiClient.h).

The Hub retains admitted actions/UObjects only through their logical in-flight lifetime, with bounded admission. A request handle is weak control: keeping `UVaRestRequestHandle` alone does not keep the action or native request alive. Use `Cancel` and `IsActive` on that handle; use `CancelAll(Tag)` for the client's owned group. Let the owning Hub/subsystem perform teardown rather than keeping native aliases beyond its lifetime. [Handle](../../Source/VaRestHttp/Public/VaRestRequestHandle.h), [Hub retention](../../Source/VaRestHttp/Public/VaRestHub.h).

## Readiness, sessions and durable work

A managed API client starts with an empty queue account and no activation request. Loading an API, creating a client, selecting an environment or reporting connectivity does not activate durable replay. Application readiness means that the application has established the intended environment, account/tenant and trusted current identity. There is no implicit application-ready handshake hidden in `GetClient`.

For durable work:

1. Choose a storage profile before the Hub admits its first client.
2. Obtain the API client and select the intended environment.
3. Establish the real current session/credentials, then call `SetQueueAccount` with an explicit nonempty account partition.
4. After application readiness, call `ActivateOfflineQueue` and inspect its result and the live queue state.

`EVaRestOfflineQueueActivation` is `Activated`, `PendingBusy` or `Refused`; `FVaRestOfflineQueueActivationResult` contains `Outcome` and `Detail`. `PendingBusy` retains one accepted activation intent while an older binding drains; it is not an active queue. `EVaRestOfflineQueueState` is `Paused`, `PendingBusy`, `Active` or `Unavailable`. `GetOfflineQueueError` supplies the current detail. `GetQueuedCount(bLoaded)` returns an authoritative count only with `bLoaded=true`; zero while paused, unavailable or loading is not proof of an empty durable store. `PauseOfflineQueue` revokes the active binding and preserves the stored work. [Activation implementation](../../Source/VaRestHttp/Private/VaRestManagedQueue.cpp).

Replay and age/identity retirement require an eligible active owner. A dormant queue is not silently replayed or expired merely because its API asset was loaded. Reopening retained work therefore repeats the same readiness and explicit activation protocol. It remains subject to the original item's age, origin and identity checks when active. Replay is at-least-once across crashes; use a server-enforced idempotency key for effects that must tolerate duplicates. `OnQueued` is durable admission with a `QueueId`, not an HTTP success. `OnQueuedResult` reports later replay retirement. See [cache/offline](cache-offline.md) for admission constraints, storage bounds, asynchronous Flush and recovery semantics. [Active-owner selection](../../Source/VaRestHttp/Private/VaRestOffline.cpp).

The generated durable name frames the complete API asset path, storage profile, environment namespace and account; matching a name does not establish identity. An empty environment `QueueNamespace` uses its complete environment name, including the numeric suffix. `SetStorageProfile` must precede first client admission; simultaneous non-PIE hosts must select distinct stable profiles. Packaged application profiles are stable. The engine/editor fallback uses its own partition. Default PIE profiles include a transient play-session identity and PIE instance, so they do not promise recovery across Play restarts. Choose an explicit stable profile before client creation for an intentional restart test. [Profile and name implementation](../../Source/VaRestHttp/Private/VaRestHubRuntime.cpp), [PIE partition](../../Source/VaRestHttp/Private/VaRestHub.cpp).

Use `SetSession` for new OAuth credentials. `ImportSession` consumes the trusted schema produced by `ExportSession`; a callback's `FVaRestSessionDataView` is not that schema. Persist exported credentials only in application-owned secure storage. Exact token text or a queue account string does not prove continuity. A valid exported binding can preserve identity across restart; `bSameIdentity=true` is a trusted application assertion under its documented live-session rules, not authentication of untrusted input. `EVaRestSessionChange` is `Refreshed`, `Replaced` or `Cleared`. See [identity and queue sharing](cache-offline.md#identity-and-queue-sharing).

Auth configs include `UVaRestBearerAuthConfig`, `UVaRestApiKeyAuthConfig`, `UVaRestBasicAuthConfig`, `UVaRestBearerRefreshAuthConfig` and `UVaRestClientCredentialsAuthConfig`. Use live credential setters for production credentials. `RefreshEndpoint`/`TokenEndpoint` references select a declared POST endpoint's path and trace identity; they do not recursively apply that endpoint's body, response type or request overrides. [Builtin auth declarations](../../Source/VaRestHttp/Public/VaRestBuiltinAuthConfigs.h).

## Blueprint request flow and results

Use **Call Endpoint** for an API endpoint or **Send Request** for an ad hoc request. Connect the world/client inputs, supply the body and parameters, and consume the node's terminal execution route. The generated node owns the preparation/continuation wiring; the `VaRest|Internal` functions on `UVaRestRequestAction` are compiler implementation details, not a replacement manual Blueprint protocol.

The request node exposes `OnSuccess`, `OnError` and `OnTypedOutputUnavailable`, plus `OnQueued`/`QueueId` when queueing is selected. `Http` is `FVaRestResponseData`; `Error` is `FVaRestError`. Typed values use `Response` and `ErrorBody` when those descriptors are present. A non-2xx HTTP response follows the error path. Durable admission follows the queued path instead of fabricating a typed HTTP response. [Node construction](../../Source/VaRestEditor/Private/K2Node_VaRestSendRequest.cpp), [action declaration](../../Source/VaRestHttp/Public/VaRestRequestAction.h).

`FVaRestResponseData` carries `RequestId`, `Status`, ordered `Headers`, `EffectiveUrl`, `ContentType`, `Elapsed`, `Attempts`, `bFromCache`, complete raw `Body`, `SavedFile`, `DecodeWarnings`, `BytesUp` and `BytesDown`. Repeated headers are separate `FVaRestHeader` rows; converting them to a map loses repetitions. Redact `EffectiveUrl` before display or logging because it can contain credentials. Raw body bytes are not a shortened Inspector preview. `FVaRestQueuedResultData` carries `QueueId`, `Tag`, `bSuccess`, `Response` and `Error`. [Reflected data](../../Source/VaRestHttp/Public/VaRestHttpBlueprintTypes.h).

`EVaRestErrorKind` is `None`, `Canceled`, `Connection`, `Timeout`, `Http`, `Server`, `Decode`, `Auth`, `Offline` or `Config`. Inspect `Kind`, code/message and response rather than treating status 0 as an HTTP reply. The spelling is `Canceled`. [Error types](../../Source/VaRestHttp/Public/VaRestHttpTypes.h).

## IDs and typed-output availability

Reflected unsigned numeric identifiers/generations use canonical decimal `FString`, preserving uint64 values without conversion through signed integers or floating point:

| Surface | Representation |
| --- | --- |
| Response/auth-target `RequestId`, handle `GetIdText()` | Decimal `FString` |
| Hub `GetScopeIdText()` | Decimal `FString` |
| Session view `Generation`, `CredentialGeneration` | Decimal `FString` |
| Endpoint `Id`, session `IdentityId`, field rule `Field` | Opaque `FGuid` |
| Durable `QueueId` | Existing opaque `FString` |

`UnsignedIdText`, `TryParseUnsignedId` and `TryParseGeneration` enforce the numeric text contract; signs, whitespace, leading zeros and overflow are refused. The native integer API remains native integer API. Do not parse a decimal ID through a Blueprint float. [ID helpers and reflected fields](../../Source/VaRestHttp/Public/VaRestHttpBlueprintTypes.h).

Reflected materialization and Blueprint transfer run on the game thread. Current Editor conversion requires live mutation/transactor hooks and a settled type/rule generation. Recompile, reinstancing, undo or rule changes invalidate the old typed plan. In cooked Game, rule publication must have finished before a later admission; this supports early conversion without waiting for an arbitrary first EndFrame. Required imported rules and the full dependent type closure must still exist. Unsupported/unqualified native lifecycle types refuse configuration rather than being inferred safe from `IsNative`. [Current availability checks](../../Source/VaRestHttp/Private/VaRestDynamicClosure.cpp), [explicit native GT contract](../../Source/VaRestHttp/Public/VaRestDynamicTypes.h).

No reflected worker-conversion mode is enabled by these APIs. Native JSON/file parsing work and immutable result shells do not confer permission to access reflected values on workers. The ordinary typed HTTP path retains DOM conversion by default; the explicit streaming decoder remains a separate candidate.

A type/target can become invalid after HTTP completion but before delivery of the prepared output. Handle `OnTypedOutputUnavailable` and inspect `EVaRestTypedOutputUnavailableReason`: `TypeChangedAfterCompletion`, `TargetChangedAfterCompletion`, `OutputSuperseded` or `OutputNotBound`. This route retains HTTP metadata and the original error; it is not a fabricated typed success. Public native `EVaRestDynamicAvailability` is plain metadata with `None`, `Available`, `Unavailable`, `TypeChanged` and `BudgetRefused`. Raw JSON/text/bytes remain separate from the private one-shot typed transfer. [Result shell](../../Source/VaRestHttp/Public/VaRestDynamicResult.h), [typed action routes](../../Source/VaRestHttp/Public/VaRestRequestAction.h).

Thread-terminal ordering, later-frame delivery, shutdown exceptions and native shell/GT Take ownership are specified in [HTTP thread and completion](http-thread-completion.md). A successful finite Editor/cooked scenario does not qualify every mutation, worker/native lifecycle, platform or release gate; consult the passport for actual evidence and remaining limits.

## Exact wire names, types and multipart

`FVaRestEndpointParam::Name` is exact, case-sensitive wire text. `PinName` is the stable `FName` Blueprint pin identity; it must not replace the wire spelling. Pin friendly text may show `Name` while reconstruction keeps `PinName`. Parameter defaults are scalar literals or JSON arrays of scalar literals; enum defaults use internal enumerator names. Query arrays support `Repeat`, `Csv`, `Ssv` and `Pipes`; path/header arrays use `Csv`. Removed parameters retain reconstruction pins but are not transmitted. [Parameter contract](../../Source/VaRestHttp/Public/VaRestApiTypes.h), [validation](../../Source/VaRestHttp/Private/VaRestApiTypes.cpp).

`FVaRestTypeRef` selects `None`, `Json`, `Struct`, `String`, `Bytes`, `Bool`, `Int`, `Int64`, `Double`, `Enum` or `DateTime`, and optionally one `Array` or FString-keyed `Map`. It is not an arbitrary nested-container descriptor. `Struct` uses a `UScriptStruct`, `Enum` a `UEnum`; `DateTime` has no authored `Type` object. The DateTime pin is a real `PC_Struct` backed by `TBaseStructure<FDateTime>::Get()`, and JSON encoding uses `FDateTime::ToIso8601()`. `Bytes` already means a byte array. `bJsonValue` applies only to scalar String/Bytes body/response descriptors, selecting a JSON string/base64 value instead of a raw body. Parameter descriptors have their narrower validated scalar/enum-or-array set. [Descriptor](../../Source/VaRestHttp/Public/VaRestApiTypes.h), [pin mapping](../../Source/VaRestEditor/Private/VaRestTypeRefDetails.cpp).

The asset body enum `EVaRestEndpointBodyKind` has exactly `Json`, `Form`, `Multipart`, `Binary`, `Text`. Native `EVaRestBodyKind` is a separate enum with `None`, `Json`, `Text`, `Form`, `Multipart`, `Binary`, `Stream`. Their order differs. Use the checked symbolic `TryGetNativeBodyKind` mapping; never cast by ordinal or rename the native values to match the asset enum. [Mapping](../../Source/VaRestHttp/Private/VaRestApiTypes.cpp), [native body declaration](../../Source/VaRestHttp/Public/VaRestBody.h).

For multipart, pass `FVaRestMultipartData::Parts`, each a `FVaRestMultipartPartData`. Select a field (`bField`/`FieldValue`), bytes, or `FilePath`; do not mix sources. Optional file metadata is `FileName`/`ContentType`. `ToNative` validates the selection and delegates to the real native multipart/file validation. The encoder generates the boundary and multipart Content-Type; an endpoint cannot override them through `BodyContentType` or a Content-Type header. File-backed multipart has the native streamed-body restrictions, including durable queue refusal. [Multipart data and validation](../../Source/VaRestHttp/Private/VaRestHttpBlueprintTypes.cpp), [endpoint validation](../../Source/VaRestHttp/Private/VaRestApi.cpp).

## Rules shared by ordinary and endpoint conversion

An API's `JsonTypes` is runtime data, distinct from editor-only importer state. Field rules use the stable Blueprint field `FGuid` in `FVaRestJsonFieldRule::Field` and exact wire `Json`; a display/property rename does not replace that identity. Presence is `Always`, `OmitWhenDefault`, `NullWhenDefault`, `DecodeOnly` or `Ignore`, with independent `bNullable` and `bBase64` flags. Enum rules use the internal `Enumerator`, exact `Json` text and an explicit physical `bUnset` enumerator. Do not infer an unset value from display text, numeric index zero or a generated MAX entry. The encoder applies explicit unset/presence semantics, including omission of unset container entries. [Rule declarations](../../Source/VaRestJson/Public/VaRestJsonRules.h), [encoding](../../Source/VaRestJson/Private/VaRestJsonConvertEncode.cpp).

`UVaRestApi::PostLoad` registers its batch of rules; destruction unregisters that owner. `NotifyEdited` republishes a complete programmatic batch. Ordinary BP/default-options conversion captures the immutable default rule snapshot when no explicit `FVaRestJsonConvertOptions::Rules` is supplied. Endpoint conversion captures the API owner's snapshot. Equal declarations from independent owners coexist; conflicting declarations refuse the affected type instead of selecting the last writer. An explicit owner snapshot overrides that owner's declarations while preserving captured default resolution for other types. [API registration](../../Source/VaRestHttp/Private/VaRestApi.cpp), [snapshot interface](../../Source/VaRestJson/Public/VaRestJsonRules.h), [conversion options](../../Source/VaRestJson/Public/VaRestJsonConvert.h).

Keep configured APIs/rules and their type dependencies available in cooked content. Editor-only display metadata is not a substitute for runtime rule registration. Dynamic activation refuses missing required imported rules in every build. OpenAPI import/reimport and its save/conflict workflow are described in the [API guide](varest2.md#openapi-import).

## Native code and editor extensions

`FVaRestClient::Make(BaseUrl)` remains the native code-first path. Copies share its client state; builders snapshot configuration when sent. Its default connectivity is process scope 0, retry is opt-in through `WithRetry`/builder `Retry`, and it does not silently acquire an API asset, managed account activation or GI isolation. A Hub-created client is a separate managed route. The asset's Exponential(3) default does not change native Make defaults. Existing `Request<T>` semantics remain separate from dynamic reflected request transfer. [Native client](../../Source/VaRestHttp/Private/VaRestClient.cpp).

Protocol action-provider registration lives in **VaRestEditor**, exported through `IVaRestAssetEntryActionProvider` and `FVaRestAssetEntryActionRegistry`; runtime HTTP does not link an editor registry. A provider owns its concrete `{Asset, Entry, Channel, Route}` node identity, pins and validation. Register with `RegisterProvider`, retain its exact `FDelegateHandle`, and `UnregisterProvider` before module unload outside provider/action-database callbacks. Duplicate/empty/busy registrations are refused. `GatherActions` returns pinned spawners; `RefreshLoadedNodes` belongs to the provider's affected nodes. This editor extension point does not change the runtime body enum or create a second HTTP/queue implementation. [Exported registry](../../Source/VaRestEditor/Public/VaRestAssetEntryActionRegistry.h).
