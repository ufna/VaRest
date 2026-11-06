# VaRest 2 M5 — integrated Codex implementation-plan candidate

Status: DOCUMENT-ONLY DRAFT, NOT ACCEPTED. Author: Codex under Root's post-review author assignment, 2026-10-25. Accepted source pin: 7a8eb9fdc6111ce34cd7304d02f408ef6ddbf7ad. This document supersedes the original Kimi candidate as a proposed integrated design only; the original candidate, report and independent review remain immutable evidence.

No tracked plan freeze, source, build graph, UHT/compiler, runtime, server, cook, merge or release permission follows from this draft. Root independently reviews this candidate. Fresh actual accepted WHOLE-I1 reconciliation is mandatory before tracked freeze or M5 source. Current Inspector Task8 working content is excluded from the accepted baseline.

Native retirement section 5 integrates the full revised Codex construction supplied and FULL-read by Root (104fe41d); Root’s repair adjudication (261024cc) supplies binding draft decisions. This author is now an author, not the independent acceptor of this replacement.

## 0. Authority, corrected decisions and acceptance boundaries

Authoritative contracts: core design complete section 5 and relevant threading/body/auth/build rules; docs/dev/cache-offline.md (all 160 accepted lines); OpenAPI section 17 N1–N16 and sections 6/7/14; WebSocket section 19 items 5/7/8, section 11 and SW11; Inspector sections 3.5 and 9; VaRest-ajz / ajz.1 / ajz.2; project AGENTS/CLAUDE and native graph permits. All repository API evidence below refers to the accepted pin, read through git show.

Document evidence:

| Input | SHA256 |
| --- | --- |
| Original m5-plan-candidate.md, 998 lines / 97558 bytes | 9991c2a951615e772bb543f9c318936c7ba73b3471936d51bb026eeb8120d9eb |
| Original m5-plan-report.md, 171 / 14990 | 32c9e9cd8b97a1ab88282b25e50046bfb3d0fd07b281295bb2af2cf5720486cf |
| root-typed-bridge-planning-ruling.md, 29 / 5404 | 69010c2dd94c11416f03d0dbdc920c5f165d94e60a77827d1ea06b1e0a04c891 |
| m5-plan-independent-review.md, 323 / 42597 | 2242c399f9eefd1cf92f984c5bb21b4111e1a49d50a82764e821c025ee31396e |
| codex-retirement-construction.md, 447 / 66103 | 104fe41d788cffdbe54f0a018fcf23c019f9c9420034a5dadc7f4c9e583d50f3 |
| root-codex-repair-adjudication.md, 24 / 7891 | 261024cce47c812b4d3b13a83d28f091b014fd0f767bba4771bbc93816440bf6 |

Root accepted the independent review's C1/I18/M3 as plan repair requirements, then assigned this authoring pass. The following are Root decisions for THIS DRAFT, not unresolved owner questions:

1. Active-only queue replay. Managed durable registrations begin PAUSED with QueueAccount empty; explicit nonempty app account/tenant selection and application activation follow account/environment/current trusted identity readiness. Returning to retained work requires the same protocol. Native code-first mismatch retirement remains unchanged. Name equality never proves identity.
2. UVaRestHub is a public UObject facade. A new GameInstance subsystem host and the engine subsystem host own it with UPROPERTY references. This explicitly reconciles the future core section 5.2 base-class wording; no invalid engine outer for UGameInstanceSubsystem and no fake GI.
3. A new EVaRestEndpointBodyKind has the five asset choices. Accepted EVaRestBodyKind, its None/Stream members, names and numeric codec values remain unchanged.
4. All reflected unsigned NUMERIC IDs/generations use canonical decimal FString. Native integers remain unchanged. Opaque FGuid identity remains FGuid; QueueId and other nonnumeric identifiers retain their existing strings.
5. Saved-relative default storage values are VaRest/Cache and VaRest/Queue. OAuth client-credentials expiry defaults to expires_in. Per-asset RedactedKeys is explicit. DateTime pins are PC_Struct with TBaseStructure<FDateTime>::Get().
6. Ordinary BP/default-options conversion resolves an immutable global default rule snapshot. An explicit owner snapshot overrides it. Different active owners declaring conflicting rules deterministically refuse the affected type, with a diagnostic; there is no arbitrary last writer. Equal declarations and independent unregister are supported.
7. Refresh/token endpoint names mean POST URL path plus trace identity only. Stale preterminal success is Config/DynamicTypeChanged. The protocol action registry lives in VaRestEditor and is link-tested from another module.
8. Dynamic native completion crosses an actual later frame BEFORE Finalize and Completed. Private reflected carriers are tracked on GT and reset before user delivery. A retained public native result shell never owns reflected payload/pins or a last-release GT obligation.
9. Finite admitted-operation count/input/value/work and converter requested/logical-output limits are required. Parse admission estimates, authored/default/native-operation allocations, container capacity/copy effects and global physical peak remain explicitly unproven where no proof exists. Refused caller-driven action/notification costs are outside the admitted-operation count, preserving ordinary asynchronous Config behavior.
10. A cooked worker needs a complete pinned stable closure including Optional and qualified native lifecycle operations; otherwise use the specified GT path or refuse Config before launch. Pins alone are insufficient. Existing compile-time Request<T> is unchanged.
11. Editor undo uses a checked UTransBuffer registration with real delegates and absent-buffer refusal behavior. Schema/rules/target invalidation and separate GT materialization measurements are explicit.
12. A settings-only cook fixture is isolated from BP hard references. Nonzero managed scope/name exists before the first Client Created.
13. Packaged profiles are stable. The engine fallback has a distinct Editor partition. Default PIE partitions include a transient play-session identity plus PIE instance; they do not promise cross-play restart recovery. An explicit stable test profile enables restart tests. Multiple non-PIE GIs in one process must select distinct profiles before client admission.

Asset retry is FVaRestRetryPolicy::Exponential(3), visible in Details. Code-first Make remains no-retry until WithRetry/Retry. UVaRestHttpSettings has no retry field. Custom auth FinishRefresh has exactly Refreshed / Rejected / Failed(kind); held live refresh has no watchdog. First OAuth tokens use SetSession, while ImportSession accepts only the trusted ExportSession schema. ClientCredentials is a separate additive provider.

### 0.1 Mandatory construction qualification gates

G-BP-TRANSFER is owned first by Task 10, before Task 11's full K2 implementation. It qualifies the exact internal preparation/continuation construction, per-invocation cookies, engine-owned output storage and Completed-sink recompile handling. The relevant compiler APIs exist, but existence is not a lifetime proof. The gate is unexecuted. Failure stops this construction and returns to Root for a bounded replan; do not proceed with a default typed Success, a callback-local carrier, or a cached frame/property address.

G-COOKED-CLOSURE is owned by Task 7, with final actual packaged evidence in Task 14. Stable closure, native lifecycle qualification, GT release and shutdown behavior are all required before enabling a reflected cooked worker. The initial safe mode is GT reflected materialization, with immutable JSON/envelope worker parsing. Unqualified native lifecycle operations either use that documented GT mode under its nonreentrant reflection contract or fail Config before allocation. No optimistic worker enablement is inferred from IsNative or a pin count.

G-RETIREMENT is owned by Task 5 against section 5's exact construction. G-I1-RECONCILE precedes all source tasks. Every gate remains open until its actual evidence is reviewed; this planning document executes none of them.

## 1. Module ownership and actual accepted extension seams

Runtime VaRestJson owns JSON/rule/conversion machinery. Runtime VaRestHttp owns asset data, auth configs, Hub facade/hosts, clients, dynamic result and BP actions. It gains a direct PUBLIC Engine dependency for UDataAsset/UObject hosts/async actions, retaining VaRestJson and DeveloperSettings. HTTP stays a private transport dependency. No runtime module links UnrealEd, BlueprintGraph, KismetCompiler, VaRestEditor or WebSockets.

VaRestEditor remains UncookedOnly and owns editor invalidation, K2 nodes, type-ref customization and exported entry-provider registration. Its public provider header requires appropriate public VaRestHttp/CoreUObject/BlueprintGraph dependencies; verify through a separate consumer. New VaRestEditorTools is Type Editor, owning asset factory/actions/menu extension points. It depends on VaRestEditor only for exported menu/entry contracts actually used. HTTP has no WS dependency; W5 supplies protocol classes and may use a separately accepted protocol editor module.

Accepted client config is immutable and Send snapshots it; copies of FVaRestClient share one FClientState. FRequestSpec construction is private by shape. Dynamic requests use the same validated TVaRestRequest/Resolve pipeline. Existing PropertyToJson/JsonToProperty names are reused. The accepted chain remains custom Outer -> Offline -> Cache -> Retry -> AuthGate -> boundary -> limiter -> AuthApply -> custom PerAttempt -> log -> transport.

Existing frozen names/settings/cache/offline/auth/error/query/WS surfaces remain byte-spelled. Public M5 additions below are proposed new freezes only. Existing EVaRestBodyKind numerical values are serialized in queue records and must not change. Rename no existing settings or frozen enum merely to fit new declarations.

## 2. Runtime reflected foundations and exact data model

Task 2 introduces compiling reflected interfaces/helpers before any asset stores them. Implement real GENERATED_BODY/UPROPERTY/UFUNCTION declarations, exported module API macros, categories and Blueprint-compatible types; abbreviated field tables below do not authorize nonreflected or unsupported declarations.

### 2.1 JSON rules and conversion options

EVaRestJsonPresence: Always, OmitWhenDefault, NullWhenDefault, DecodeOnly, Ignore.

FVaRestJsonFieldRule:
- FGuid Field;
- FString Json;
- EVaRestJsonPresence Presence = Always;
- bool bNullable = false;
- bool bBase64 = false.

FVaRestJsonEnumRule:
- FName Enumerator;
- FString Json;
- bool bUnset = false.

FVaRestJsonRules is an exported native registry with GT mutation:
- ReplaceOwnerRules(const UObject* Owner, TConstArrayView<FVaRestJsonTypeRuleRegistration>, TConstArrayView<FString> TypesFolders): publish one batch/generation change.
- RegisterFields(Owner, const UScriptStruct*, Rules), RegisterEnum(Owner, const UEnum*, Rules), RegisterTypesFolder(Owner, ContentFolder) for native callers, each a coherent transaction.
- UnregisterOwner(Owner).
- Snapshot(Owner): immutable explicit-owner view including the captured default fallback.
- DefaultSnapshot(): immutable process default.
- GetGeneration(Owner), GetDefaultGeneration().
- OnRulesChanging()/OnRulesChanged(): narrow GT native notification boundary, so Http can invalidate without a Json-to-Http dependency.

Rule registrations are keyed by owner FObjectKey and type FObjectKey, never raw-address identity. GT resolves type objects and copies plain rule data; immutable snapshots do not dereference owner UObjects from workers. The default snapshot has per-type Resolved / Conflict / Absent state. Normalize rule order by field GUID/enumerator for equality; equal rules from several owners resolve identically. A field/enum declaration conflict refuses conversion for that affected type before walking it, names nonsecret owner/type paths in the diagnostic, and never chooses registration order. Removing one owner leaves equal surviving declarations and cannot unregister another owner.

Explicit owner rules win for its declared types; undeclared nested types use that snapshot's captured default resolution. A default conflict on a nested type remains a refusal unless the explicit owner supplies that type. Snapshot IDs and generations are immutable; cache keys include type serial, Naming, effective RuleSetId. Existing FVaRestJsonNames native renames remain for fields without a GUID rule; a GUID rule has explicit exact-wire precedence. Unregister/edits invalidate caches and dynamic generations before mutable carriers can be reused.

Null FVaRestJsonConvertOptions::Rules now means capture DefaultSnapshot at the top-level conversion entry, once per conversion, not “ignore all registered rules.” Explicit Rules retains that snapshot for all nested calls. Unregistered ordinary types keep accepted behavior. Existing Blueprint JSON nodes automatically take this route without API/owner pins; no Call Endpoint or explicit API load is required.

Field GUID lookup uses FindPropertyGuidFromName; parse the legacy 32-hex property suffix only when ArePropertyGuidsAvailable is false. GUID rules survive rename/recompile without re-registration. Always encodes normally; OmitWhenDefault compares the initialized authored/native default; NullWhenDefault emits null; DecodeOnly skips encode; Ignore skips both. Nullable null retains current/default even for containers; otherwise existing generic container-null semantics remain. Base64 is per byte-array field, after a custom BytesWriter opportunity. Enum mappings precede authored/display name handling; unset omits an object key (null under NullWhenDefault), skips array/map entries with warning, and has no reverse wire match.

Non-Shipping N4 warning is once per type under a currently registered content package folder with no rules. A completely unloaded/unregistered folder is unknowable at runtime; do not claim its missing settings entry alone produces this warning. Asset/editor validation detects a known API missing from Settings.Apis; packaged preload/cook tests establish first-use registration.

FVaRestJsonConvertOptions adds immutable Rules plus MaxOutputBytes, MaxValues and MaxWorkUnits, all zero by default for accepted code-first unlimited behavior. These are requested/logical conversion counts, not a universal allocator/heap cap. Section 7 defines dynamic nonzero limits and exclusions. FVaRestJsonParseOptions adds optional MaxValues and MaxWorkUnits (zero preserves existing calls), checked by parser/builder before adding the counted node/value/work unit. See Task 1 for exact count semantics and hostile-shape tests.

### 2.2 Endpoint/type descriptors

New UENUMs:
- EVaRestTypeKind: None, Json, Struct, String, Bytes, Bool, Int, Int64, Double, Enum, DateTime.
- EVaRestTypeContainer: None, Array, Map.
- EVaRestEndpointBodyKind: Json, Form, Multipart, Binary, Text.
- EVaRestEndpointParamLocation: Path, Query, Header.

FVaRestTypeRef has Kind=None, TObjectPtr<UObject> Type=null, Container=None. Struct requires UScriptStruct; Enum requires UEnum; DateTime maps to FDateTime with no arbitrary Type pointer; all other kinds require null Type. Json means FVaRestJson. None means no typed payload/pin; a newly created HTTP endpoint explicitly defaults ResponseType to Json. Bytes denotes raw byte-array payload, with Container=None; nested byte/container combinations are refused. Map has FString keys and one noncontainer value type. General nested containers are invalid. Parameter descriptors allow only scalar/string/numeric/bool/enum and one array, as N9 requires; root result/body descriptors additionally allow Struct/Json/DateTime and map.

FVaRestEndpointParam:
- FString Name, exact case-sensitive wire spelling;
- FName PinName, unique stable K2 pin identity;
- In, Type, bRequired, FString Default, EVaRestQueryStyle Style=Repeat, FString Description, bRemoved.
Use Name only on the wire. Validate PinName uniqueness and location-aware deduping without changing Name. Declared path arrays and header arrays use comma style; query arrays use Repeat/Csv/Ssv/Pipes. Removed params remain declared/pinned but never sent.

FVaRestHeader and FVaRestQueryParameter are exported BlueprintType USTRUCTs with editable FString Name/Value. Arrays preserve order/repeats. FVaRestApiEnvironment has FName Name, FString BaseUrl, SocketBaseUrl, QueueNamespace; empty QueueNamespace uses canonical environment Name text under the documented naming rule.

FVaRestApiEndpoint has:
- Id: FGuid (assigned once), Name:FName, Verb/Path/Category/Description:FString;
- bDeprecated, DeprecationMessage;
- DefaultQuery, Headers, Params arrays;
- RequestType, BodyKind:EVaRestEndpointBodyKind=Json, BodyContentType:FString (optional selected request media), ResponseType=Json, ErrorType=None;
- AuthMode:EVaRestAuthMode=Required, bIdempotent, bQueueWhenOffline;
- bOverrideRetry + FVaRestRetryPolicy Retry;
- bOverrideTimeouts + float Timeout/ActivityTimeout;
- bOverrideCache + FVaRestCachePolicy Cache;
- bOverrideEnvelope + FVaRestEnvelope Envelope;
- bOverrideNaming + EVaRestJsonNaming Naming;
- bOverridePriority + EVaRestPriority Priority;
- bOverrideMaxJsonBytes + int64 MaxJsonBytes;
- bOverrideStrictDecode + bool bStrictDecode.

BodyContentType is validated and becomes an explicit per-request Content-Type for selected JSON/Text/Binary media. Empty JSON uses asset JsonContentType; Form uses application/x-www-form-urlencoded; Multipart always uses the native computed boundary and rejects an asset boundary override. No asset selector ever casts directly to the native body-kind numeric enum; use a total checked mapping.

FVaRestApiValidationContext is a runtime native collector of localized errors/warnings with object/property paths and stable codes. It has no editor dependency and adapts to FDataValidationContext. UVaRestApiSocketConfig is abstract BlueprintType/EditInlineNew/DefaultToInstanced, exported in Http; Name:FName, Description:FString, virtual GetProtocolTag() and Validate(context). Its instanced array in the API contains no transport behavior. W5 owns concrete connection/message types; SW11 provider-specific channels/routes/pins stay outside Http.

### 2.3 Auth/config and Blueprint metadata interfaces introduced early

Task 2 creates abstract UVaRestAuthConfig with pure virtual CreateAuth() and Validate(context); no working builtin provider is claimed until Task 9. This gives UHT a real complete class for the asset's instanced Auth field. FVaRestClientCredentialsConfig/EVaRestOAuthClientAuthMethod declarations live in the explicit Task 2-owned Public/VaRestClientCredentials.h before Task 9 implements the provider. CreateAuth is a native provider-construction method, not a Blueprint event: it takes no live client and constructs only the provider/weak per-wrapper adapter, invoking no Apply/Refresh/identity/user callback. A later refresh receives the real native FVaRestAuthRefreshContext. This avoids creating an observable partially configured client merely to obtain its initial auth.

FVaRestDecodeIssue is BlueprintType with reflected FString Path/Message. FVaRestResponseData fields:
- FString RequestId: canonical decimal unsigned native ID;
- int32 Status, TArray<FVaRestHeader> Headers, FString EffectiveUrl/ContentType;
- double Elapsed, int32 Attempts, bool bFromCache;
- TArray<uint8> Body, FString SavedFile, TArray<FVaRestDecodeIssue> DecodeWarnings;
- int64 BytesUp/BytesDown (accepted signed byte counters).

FVaRestQueuedResultData: FString QueueId, FName Tag, bool bSuccess, FVaRestResponseData Response, FVaRestError Error. Native FVaRestQueuedResult is unchanged. Copy every raw successful/error body byte and every repeated header line in order; do not replace bytes with a preview or join Set-Cookie. A reflected event adds its own response copy on top of the accepted native const-result copy; this is outside persistence executor accounting and is not copy-free.

FVaRestMultipartPartData: Name/FileName/ContentType/FieldValue:FString, Bytes:TArray<uint8>, FilePath:FString, bField:bool. FVaRestMultipartData holds ordered Parts and checked ToNative. A field uses FieldValue and requires empty Bytes/FilePath/FileName/ContentType; no second field source is accepted. Other parts choose Bytes OR FilePath, never both; conversion uses native AddField/AddBytes/AddFile and preserves streaming/file validation.

FVaRestSessionDataView retains IdentityId:FGuid, AccessToken/RefreshToken:FString, ExpiresAt/Lifetime:double, ordered Headers, and Generation/CredentialGeneration as canonical decimal FString. FVaRestAuthTargetData has Session, ordered Headers/Query, RequestId:FString. UVaRestAuthRefreshContext::GetRefreshId and reflected request/client/scope handle IDs return canonical decimal FString. Conversion helpers strictly parse unsigned decimal with overflow checks, no sign/space/leading zero except “0”; do not narrow a native ID to int64/double. Nonnumeric queue identifiers and GUIDs retain their native semantic forms.

### 2.4 API asset, editing and settings

UVaRestApi derives UDataAsset. It stores Environments, DefaultEnvironment, ShippingEnvironment, Headers, UserAgent, instanced Auth, Retry, Timeout, ActivityTimeout, MaxJsonBytes, Cache, Offline, Envelope, LogLevel, Naming, MaxConcurrent, bStrictDecode, JsonContentType, RedactedKeys, Endpoints, instanced Sockets, JsonTypes, TypesFolder. Constructor sets Retry=Exponential(3), JsonContentType=application/json. JsonTypes entries carry UField Type and field/enum rule arrays; editor import state is opaque.

WITH_EDITORONLY_DATA fields: instanced VisibleAnywhere UAssetImportData and FVaRestApiImportState {Importer:FName, Version:int32, Blob:TArray<uint8>}. Emit normal source-file registry tags. No import/reimport/parser/generator implementation belongs to M5.

Runtime methods: FindEndpoint(Id/Name), FindEnvironment(Name), ResolveEnvironmentName, ResolveEndpointPath, IsDataValid, NotifyEdited, PostLoad, BeginDestroy. URL resolution for refresh/token references accepts only a declared POST endpoint with no unresolved path parameters; use Path and trace name only, never inherited body/query/headers/auth/policies. Command-line environment precedence is per-asset override, global override, nonempty ShippingEnvironment in Shipping, then DefaultEnvironment. Runtime SetEnvironment accepts declared names only.

PostLoad publishes one complete owner rule batch. NotifyEdited first invalidates rule consumers/carriers, publishes one owner generation/batch, validates, then emits native exported FVaRestApiEditorBridge::OnEdited(UVaRestApi*) once. VaRestEditor owns action refresh and loaded-node reconstruction once; Http never calls UnrealEd. BeginDestroy unregisters the owner and invalidates before releasing fields. Editor interactive PostEditChange routes through the same one-batch path; programmatic importers call NotifyEdited once after their batch.

IsDataValid covers duplicate endpoint GUID/name, environment name/default/shipping references, URL/token/header/media validity, path-template syntax and declared/undeclared params, descriptor/type/container validity, refresh/token references, retry/timeouts/cache/offline policy limits, duplicate socket entry names and delegated validation. Known missing Settings.Apis is a warning for rule-bearing assets; invalid runtime configuration fails through normal Config paths, not editor-only checks.

Settings retain config=Engine and all existing keys. Add Naming=CamelCase; CacheRoot=VaRest/Cache; QueueRoot=VaRest/Queue; StorageProfile=Default; Apis soft list; the dynamic limits in section 7. Relative roots resolve under ProjectSavedDir exactly once, absolute values use accepted NormalizeStorageRoot/mobile physical mapping. Tests pin normalized default roots to the accepted Make defaults. No Retry setting.

UVaRestEngineSubsystem::Initialize synchronously loads every nonnull Apis entry and owns UPROPERTY TArray<TObjectPtr<UVaRestApi>> LoadedApis for registration lifetime before GI creation. Null/missing entries warn. The fallback Hub is added only in Task 8, after its concrete class exists; Task 3 loader has no incomplete future Hub construction.

Http registers the actual three-parameter FModifyCookDelegate in editor builds: TConstArrayView<const ITargetPlatform*>, TArray<FName>& ToCook, TArray<FName>& NeverCook. Add deduplicated settings API package names only to ToCook; unregister on module shutdown. Actual isolated settings-only packaged evidence is mandatory; a delegate-array test or hard-referenced BP asset is insufficient.

## 3. Core configuration, traces, body policy and queue compatibility

Add FClientConfig ScopeId=ProcessScopeId, JsonContentType=application/json, immutable JsonRules, and bStrictDecode=false. Add native WithJsonContentType, WithJsonRules, WithStrictDecode and GetScopeId. A managed creation path prepares the complete initial scope/display name/connectivity/storage/auth/config and obtains its native admission/retirement ownership before FClientState::Create publishes anything. Section 5 fixes the exact factory/state seam. Code-first Make keeps scope 0 and accepted defaults; do not implement managed creation as Make followed by WithScopeId, which would already have emitted Created in Process.

Scope identity is immutable for one managed native client state. Do not expose a postcreation scope-move convenience merely to initialize Hub state. The Hub allocates a nonzero mask-independent scope first, emits Opened before clients, and initializes clients with it. Client Created/Destroyed, Started, Marker and late snapshots use the config scope. Scope registry contributes the existing FVaRestTraceScope shape: PIEInstance, bEditorScope, weak GI, debug label. Labels use actual world debug information and are frozen at close; editor fallback is “Editor.” Late native retired handle destruction after close may emit Client Destroyed associated with the retained closed scope; it cannot reopen the scope or admit requests. Snapshot excludes retired live shells as active clients, preserving the closed-scope record and any application-owned inert handles explicitly.

Add Endpoint:FName to FRequestSpec and FExchangeState; builder Endpoint(FName)&& carries it through Send and TracePoints::Describe. Started and replay Started publish it. IDs remain mask-independent; only payload/time/Seq work follows trace detail. Initial scope/client event tests start with capture already attached, so an incorrect early Process event cannot be hidden by a later snapshot/upsert.

FRequestSpec strict decode becomes an optional override. StrictDecode(bool) sets it; Send resolves the override against its immutable config snapshot into Ex.bStrictDecode. Thus a returned native handle from an asset honors asset strictness, and per-endpoint/builder override remains possible. Code-first config false preserves accepted behavior. Naming for struct Body remains build-time as accepted; rules and JSON media used to encode it come from that same ConfigAtBuild snapshot. Activation refuses an already stale asset rule snapshot rather than mixing a current generation token with old rules. Hub rule-change subscriptions publish a fresh client snapshot for later builders.

WithJsonContentType validates JSON-compatible media without CTLs (application/json or +json, optional valid parameters). JSON/struct Body uses ConfigAtBuild JsonContentType; explicit request Header(Content-Type) wins, body type beats a client default. Asset endpoint BodyContentType is an explicit request override; never relabel Form/Multipart/Text/Binary via a JSON default. Struct Body options receive the same JsonRules snapshot; response/error conversion copies it into decode options. Existing client/session/auth Content-Type precedence and auth-refresh bypass are retained.

The queue codec writes version 2 with exactly 19 metadata members, adding endpoint. Reader accepts v1/18 and v2/19 exact allowed key sets, rejects duplicates/missing/extra/mistyped members and enforces existing canonical metadata/lexical/depth/body budgets. Endpoint is an optional-empty plain bounded string (maximum 256 TCHAR; no controls), included in complete persistence guard scanning. Admission stores exact endpoint text; replay restores FName trace identity on GT. V1 replay has an empty endpoint. Every native bodyKind numeric value is unchanged; pin old v1 bytes for None/Json/Text/Form/Multipart/Binary before changing the writer. Older binaries refuse v2 safely; no silent down-version rewrite or record migration occurs.

No new queue implementation is created. Retirements/paused registrations/epoch ownership extend the existing FOfflineQueue and persistence executor. Preserve account/tenant/origin capture at initial Resolve, original body/key bytes, commit selection's irreversible cutoff, truthful actual IO completion, one raw OnQueuedResult per live registration, and at-least-once crash semantics.

## 4. Hub facade, settings loader, identity and explicit replay activation

### 4.1 Facade and hosts

UVaRestHub : UObject is exported BlueprintType. UVaRestGameInstanceSubsystem : UGameInstanceSubsystem has UPROPERTY(Transient) TObjectPtr<UVaRestHub> Hub; Initialize creates it with this host as valid outer. UVaRestEngineSubsystem owns the fallback through a separate UPROPERTY after loading APIs. No AddToRoot and no fake GI. The facade owns wrapper UObjects with reflected references; native FHubRuntime owns clients/config/session/connectivity/dynamic accounting. Native runtime/tickets use weak UObject references and cannot retain the dying facade/host/world/GI.

Get(WorldContext) resolves GI host when available and otherwise the engine fallback. GetForEditor returns the explicit engine fallback. GetClient(Api) is lazy; Api(ApiAsset) returns the same native handle by value for C++/W5. MakeClient(BaseUrl) applies project/Hub defaults and remains no-retry. GetDefaultClient is a lazy code-first Hub client with no named durable queue. GetConnectivity, GetScopeId native uint64, BP GetScopeIdText, IsOnline and ReportOnline are exported. A wrapper exposes its native handle by value.

Each host creates independent connectivity using Make(real clock); the engine fallback has its own connectivity/scope. Never use process-default reachability as GI isolation or reset it for tests. Effective OnConnectivityChanged is the narrow synchronous GT observer forwarded from accepted connectivity. ReportOnline only reports online. Hub OnEnvironmentChanged(Api) follows complete publication; W5 restarts its sockets. OnDeinitializing fires once after closing admissions and before client cancellation; socket consumers close through their own lifecycle.

GI/engine Deinitialize invokes the same native retirement entry, unbinds UObject event adapters and releases UPROPERTY ownership without waiting/pumping. Native retirement control retains exactly admitted native work until its fences settle (section 5), then closes the trace scope; no later user/BP callback targets a dead facade. BeginDestroy is an idempotent backstop if host Deinitialize was skipped. Engine pre-exit uses accepted core suppression/drains plus the native control protocol; it does not call a process-global shutdown merely to retire one Hub.

### 4.2 Durable profile and queue name

Hub SetStorageProfile(FString) is permitted only before the first managed client admission. Validate nonempty bounded nonsecret text and reject controls. Packaged base profile comes from settings, default Default. Every additional non-PIE GI that might address the same API must supply a distinct profile before client creation; accidental incompatible simultaneous same-path joins fail Config/Busy, never silently share different connectivity.

Default PIE profile is base + a transient play-session GUID + PIE instance, length-framed. Generate one play-session identity at PreBeginPIE and reuse it for all GIs of that play, not one identity per request. There is no automatic cross-play recovery promise or silent cleanup of acknowledged files. Explicit UseStableTestProfile/Profile override bypasses the transient part and requires exclusive stable-namespace ownership; the packaged/restart tests use that path. Engine fallback uses a separate Editor suffix and never aliases a game/PIE profile. No runtime ScopeId appears in durable names.

Effective exact queue name, before accepted native SHA256 directory naming, is:
M5 + Field(Api full object path) + Field(storage profile) + Field(stable environment queue namespace) + Field(nonsecret account partition).
Field frames UTF-8 byte length plus colon and exact UTF-8 bytes; the materialized FString round trip and the existing 1024-TCHAR/control/path limits are validated before copying/registering. Empty environment QueueNamespace uses canonical FName comparison spelling so environment FName equality cannot create accidental casing variants across packaged/editor builds. Explicit QueueNamespace and account/profile/path components are exact strings. Credentials/resolved URL/runtime ScopeId are never components.

QueueAccount starts EMPTY. ActivateOfflineQueue refuses Config/QueueAccountRequired until the application explicitly supplies a nonempty stable account/tenant partition; intentional anonymous apps may explicitly choose Anonymous or Default. No implicit Default account domain is activated. Account strings are retention partitions only. Origin/account/tenant/provider identity checks remain native. Changing an origin without changing namespace does not forward old-origin work; the registration/origin rules refuse until an explicitly compatible domain is selected.

### 4.3 Managed paused state and explicit activation

A newly obtained asset wrapper is usable for ordinary HTTP but has managed durable registration in Paused state. A paused target creates no active native queue membership; selected-domain stored count is explicitly not loaded/unavailable until activation/load. No TryReplay, connectivity probe replay or new queue-enabled admission runs until explicit activation. No custom auth GetIdentity/Apply is invoked speculatively to activate. Queue-enabled sends while paused fail ordinary asynchronous Config even online: QueueAccountRequired when the selected app account is empty, otherwise QueueNotActivated.

Wrapper API:
- SetQueueAccount(Account) and SetEnvironment(Environment) stage a new selected domain through the native epoch switch and leave durable replay paused.
- ActivateOfflineQueue(): GT, returns an explicit reflected activation result (Activated, PendingBusy, Refused) with nonsecret detail. It requires an explicit nonempty selected account (otherwise Config/QueueAccountRequired), the selected environment and current credentials/trusted identity to be ready; resolves the native current identity without changing it. A pending busy retry remains paused and repeats admission on later frames only under an explicit pending activation request.
- PauseOfflineQueue(): detaches active replay under section 5 policy; it does not clear credentials or delete files.
- GetOfflineQueueState returns a reflected Paused/PendingBusy/Active/Unavailable state; GetOfflineQueueError returns a nonsecret detail code/text. GetQueuedCount(bool& bLoaded) returns a checked int64 count with bLoaded=false/zero until the selected domain is loaded, rather than borrowing an old drain count. FlushQueue refuses before active admission with the corresponding Config detail and no retained callback.

Activation is an application assertion of readiness, not authentication. Native validates actual scope/origin/provider compatibility. A fresh login with a new opaque identity does not resume a prior session's work merely because the account name matches. The UI/docs show: GetClient -> SetEnvironment/SetQueueAccount -> ImportSession(unchanged trusted export), or restore compatible auth identity -> ActivateOfflineQueue. Ordinary SetSession initial login remains available; applications explicitly decide whether old work should be resumed under trusted continuity or left in its retained partition. No hidden relabel/delete/credential persistence is added.

Environment/account switching resolves/validates first, then fences/detaches the old epoch before publication. Already accepted operations preserve old captured config and their proper cancellation/commit cutoffs; the old epoch becomes permanently inert. Retain old acknowledged files. Publish the entire new config atomically as a paused target with no active queue member; subscribe/read back only its eventual activated registration. The old native cleanup ticket owns exact snapshots independently of the facade's new current registration. Repeated switches coalesce/refuse through bounded native admission, never accumulate unbounded old queues. Only after complete environment publication emit EnvironmentChanged Marker and Hub event. Active-only replay means no hidden background client continues old domains.

Public account/environment setters do not automatically activate. Credential/session replacement, ClearSession, provider replacement or relevant tenant/session-header changes pause managed replay before publishing the changed scope. Same-identity refresh preserves active registration. Setting a trusted unchanged export does not itself start replay; an explicit activation is still required after a paused switch. Code-first FVaRestClient native mismatch behavior is unchanged.

C1 qualification: restart with durable bearer work, delay secure restoration for multiple frames, obtain/use the wrapper, and assert zero replay/retirement/network/queued-result before activation. Returning to a retained account uses the same case. Then restore the exact trusted session and activate; replay keeps the old body/key.

## 5. Native managed admission, retirement, physical-tail fences and control lifetime

### 5.1 Permanent gate, binding lease and independent cleanup

Keep the existing single queue implementation and M4b queue identity/provider/connectivity checks. The complete protocol below is Task 5 native ownership, consumed by Tasks 6–10; no second durable queue or weakened wrong-identity Auth retirement is introduced.

The repaired construction has three separate controls:

1. A permanent shared native client admission gate. Every copied handle observes the same gate. It closes all new logical work, including any-thread sends and builders created before closure.
2. A queue binding lease with a unique registration epoch. Queue-only switch closes that lease, not the permanent client gate. Existing ordinary HTTP operations keep their Send-time configuration; they cannot later enqueue through the explicitly detached binding.
3. An independent module-owned native cleanup service with preadmitted slots. The service retains only native gates, queue/binding/control leases and scope metadata. It does not retain FHubRuntime, Hub, wrapper, World, GameInstance or BP auth UObjects.

Required invariants:

- No rejected post-close Send constructs an exchange, acquires a scope/request trace identity, or posts Enter.
- Every Send admitted before closure owns a counted lease until BOTH its logical terminal/internal delivery tail and every physical dispatch/guard/GT handback tail settle, including an ordinary non-replay request or one still awaiting GT Enter. Completed alone never releases this count.
- No detached queue binding admits new work or selects commit after detach. A commit already selected still completes truthfully.
- A retained replay head is not dispatched by another live member while the old replay operation still has an unsettled physical dispatch/guard or logical terminal handback.
- A durable Retire IO already accepted is never invalidated by changing membership/replay generation.
- A finalized replay terminal outcome published before Completed trace dispatch also wins over retirement reentered from that trace sink. Generation checks cannot turn that accepted success/terminal failure into retain-head cancellation.
- Internal completion bookkeeping runs even when user callbacks, public Flush callbacks or BP delivery are suppressed.
- Registry release depends on actual quiescence and a fence state, never on destruction of external client/config/builder handles.
- Every membership/switch/cleanup/refresh record has a finite owner and bound. There is no append-only RetiredMembers history or one-new-ticket-per-retry design.

### 5.2 Exact native data and admission protocol

The following private VaRestHttp shape is the integrated planning construction. Public declarations remain subject to review/freeze; named finite admission limits are Root decisions.

~~~cpp
struct FNativeSlotHandle
{
    uint16 Index = 0;
    uint64 Generation = 0; // zero invalid; never reused within a slot generation
};

class FClientAdmissionGate final
{
public:
    static constexpr uint64 ClosedBit = uint64(1) << 63;
    static constexpr uint64 CountMask = ClosedBit - 1;
    static constexpr uint32 MaxOwnedOperations = 1024; // managed clients only
    bool TryAcquire(FClientWorkLease& OutLease);       // any thread, CAS
    void ClosePermanently();                          // GT, fetch_or(ClosedBit)
    bool IsClosed() const;                            // acquire load
    uint32 NumOwnedOperations() const;                // acquire load & mask
private:
    std::atomic<uint64> Word{0}; // closed flag AND admitted outstanding count
};

class FClientWorkLease final // private move-only RAII
{
    TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
    bool bHeld = false;
    // Held by FManagedOperationSettlement; release once only at its combined predicate.
    // Atomic decrement, no UObject, GT callback or raw module/service-object lookup.
};

struct FManagedOperationSettlement final
{
    FClientWorkLease AdmittedWork;
    std::atomic<uint32> PendingLogicalTails{0};
    std::atomic<uint32> PendingDispatchSettlements{0};
    std::atomic<bool> bChildAdmissionClosed{false};
    std::atomic<bool> bLogicalTerminalSettled{false};
    std::atomic<bool> bDiscardedAtStop{false};
    std::atomic<bool> bWorkLeaseReleased{false};
    void TryReleaseWorkLease(); // any thread; atomic-only, exactly once
};
struct FManagedOperationRef // native handle into the still-owned fixed control storage
{
    TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
    FNativeSlotHandle Client;
    uint16 OperationIndex = 0;
    uint64 OperationGeneration = 0;
};
// Move-only FManagedLogicalTailLease and FDispatchSettlementLease refer to that record.
// Their destruction records atomic settlement only; neither invokes a UObject/GT service.

struct FQueueMemberKey
{
    uint64 ClientId = 0;
    uint64 RegistrationEpoch = 0;
};
enum class EBindingPhase : uint8 { Active, Detached, Draining, Fenced };
struct FQueueBinding final
{
    const FQueueMemberKey Key;
    EBindingPhase Phase; // GT only
    TWeakPtr<FClientState, ESPMode::ThreadSafe> Client;
    TSharedPtr<FOfflineQueue, ESPMode::ThreadSafe> Queue;
    uint32 PendingAdmissions = 0; // includes load waits through actual cleanup
    uint32 PendingNamespaceIo = 0;
    uint32 OutstandingNativeFlushes = 0;
    bool bExplicitDetach = false;
};
~~~

TryAcquire CASes Word from an open count N to N+1, refusing ClosedBit or N == MaxOwnedOperations. ClosePermanently uses one fetch_or of ClosedBit. The same atomic modification order decides the race: a Send either acquires before Close and is counted, or sees/retries against Closed and is denied. A separate bClosed check followed by a separately incremented pending-Enter counter is prohibited.

Acquire before reading current config, allocating FVaRestExchange, assigning request/trace identity or posting Enter. The lease moves into a preadmitted operation settlement record referenced by the exchange and its native tails, not the builder. An unsent builder therefore reserves no cleanup work. TryReleaseWorkLease releases once only when bLogicalTerminalSettled AND PendingLogicalTails == 0 AND PendingDispatchSettlements == 0, using acquire reads followed by a bWorkLeaseReleased compare-exchange. Dispatch/tail admission is closed before setting logical-terminal settled; no new child is added after observing a releasable zero. A tail counted before closure is inherited/moved or explicitly counted before launch, never incremented after publishing its continuation. The last settling path may decrement the shared client admission count atomically; GT later observes that count. It never calls into a destroyed module or emits Scope Closed itself.

Every managed operation uses this record: ordinary HTTP, health/auth/replay and queue-enabled work. PendingLogicalTails covers Enter, deferred Complete wrappers, decode/finalization handbacks, the Complete/trace/internal observer call stack, and any accepted OnHttpThread user delivery through its return and native handler destruction. A live operation waiting in a stage has bLogicalTerminalSettled=false even if no stack/task tail is currently running. Count each internal task before publication; move a tail instead of inventing a new one where ownership is transferred. Bound simultaneously outstanding logical tails to 64 per managed operation; reserve its completion tail so cancellation never needs a new permit. Refuse an excess nonessential launch before publishing it. Close child admission on GT before terminal acceptance; afterward only transfers of existing counted completion/dispatch tails are allowed, including a previously reserved OnHttpThread delivery. All physical Starts/optional internal tail acquisition check that closed state. Explicitly destroy native delivery handlers/locals before settling their tail rather than relying on lambda capture-member destruction order. Completed/EndWaits/InFlight removal can precede release by many frames. Silent/absent handlers still settle through the same terminal path. At shutdown a discarded task records bDiscardedAtStop and its tail settlement, never a fictitious normal result; only the Stopped protocol may replace terminal publication with explicit discarded settlement. A defensive exchange destructor cannot release while another physical/tail lease is outstanding.

For rejection, generalize the existing DeliverWithoutClient-style asynchronous Config path to take an immutable local failure reason and normal weak owner/callback options. Do not construct an exchange or retain the client to deliver that rejection. Use an internal ClientClosed / ClientCapacity reason; no new public error enum is needed. It must not emit a new Started into an already closed scope. If the captured native control is Stopped/global exit suppression applies, do not post a new refusal delivery through a dead service; the ordinary shutdown suppression policy wins.

For an already-admitted posted Enter, GT rechecks the permanent gate before Resolve/stages. A closed client produces its normal canceled/suppressed terminal path with the original open scope, releases its counted work lease, and never dispatches. Recheck closed/cancellation before limiter grants, refresh/probe creation and final physical Start as defense against reentrant trace/delegate callbacks closing the Hub during entry. Closed setters cannot reopen admission, rejoin a queue or start refresh/probes; internal teardown may still clear session secrets quietly.

Code-first clients keep the existing default behavior unless they explicitly opt into managed lifetime. Install the managed gate, scope and reserved cleanup slot inside a private CreateManaged path before FClientState::Create publishes the client; do not create an observable live client and install its gate afterward.

Accepted evidence: Source/VaRestHttp/Private/VaRestClient.cpp:170–257 constructs/snapshots/posts the exchange; Public/VaRestClient.h:65–66 retains ConfigAtBuild; Public/VaRestClient.h:675–680 and Private/VaRestPipeline.cpp:1444–1458 show CancelAll misses pending Enter; Private/VaRestPipeline.cpp:891–944 currently has only process shutdown/owner/cancel checks.

### 5.3 Preadmitted native service, caps and unwinding

Root adopts these finite record/admission limits for this draft. They do not promise physical heap, RSS or elapsed time:

| Resource | Root planning cap | Admission behavior |
| --- | ---: | --- |
| Managed client cleanup slots, process-wide including draining clients | 256 | Reserve before creating/publishing client; failure returns Config/LifecycleCapacity with no registration |
| Managed client slots per Hub/fallback scope | 64 | Same prepublication refusal |
| Counted operations per managed client, including pending Enter | 1024 | CAS refusal before exchange construction |
| Concurrent unsettled physical dispatches per managed logical operation | 32 | Refuse an excess Start as Config/DispatchCapacity before transport entry; ordinary sequential retry does not consume a lasting history |
| Simultaneously outstanding native logical tails per managed operation | 64 | Completion-tail capacity is reserved; refuse an excess optional task before publication, never allocate a retirement retry list |
| Queue drain records per client slot | 2 inline | One old switch binding plus the active binding if teardown upgrades that switch |
| Concurrent queue switches per native client | 1 | Further distinct switch returns LifecycleBusy without changing current state; identical target is a no-op |
| External RefreshAuth waiters per managed client | 64 | Refuse a new external waiter asynchronously with Failed(Config); do not append it |
| BP auth pending Finish per client | 1 | Existing single-flight; teardown clears its generation and pending Done exactly once |
| Native retirement fence records | At most 512 | Reserved inline in the 256 client slots; no new reservation at Deinitialize |

The service uses fixed slot indices plus monotonically increasing slot generations. A delayed callback validates both before touching a row. It does not retain completed rows, RetiredMembers keys, per-switch results or auth waiter histories. Exhausted generation counters fail closed rather than alias old handles. A slot is not recycled while any unsettled token has authority over its generation; SettleOnce consumes that authority and clears its reference before it can be reused.

~~~cpp
enum class ECleanupMode : uint8 { Live, QueueSwitch, Permanent, Stopped };
enum class EFenceResult : uint8
{
    Pending, Flushed, Failed, NotAdmitted, ShutdownDiscarded
};
struct FMemberDrain
{
    TSharedPtr<FQueueBinding, ESPMode::ThreadSafe> Binding;
    uint64 ThroughAdmission = 0;
    EFenceResult Flush = EFenceResult::Pending;
    bool bRetireIoWasAccepted = false;
    bool bDone = false;
};
struct FClientCleanupSlot
{
    FNativeSlotHandle Handle;
    ECleanupMode Mode;
    TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
    TWeakPtr<FClientState, ESPMode::ThreadSafe> Client;
    TSharedPtr<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope;
    FMemberDrain Drains[2];
    TOptional<FPreparedQueueTarget> PendingActivation; // one latest validated target
    // No UObject pointers with strong ownership and no strong FHubRuntime.
};

TValueOrError<FManagedClientReservation, FName>
FNativeLifetimeService::TryReserveClient(FNativeScopeRef Scope);
bool FClientState::TrySwitchQueue(FPreparedQueueTarget&& Target, FName& OutError);
void FClientState::BeginPermanentRetirement(); // idempotent, cannot fail admission
void FNativeLifetimeService::TickOwned();      // bounded slot scan, GT only
~~~

The service owns admitted native slots independently of caller callbacks. While Live it stores only a weak client pointer; when retiring it keeps the native control leases needed for cleanup, not a wrapper or dead Hub. Native exchanges/IO already own the client/config snapshots they require. A shared native scope object carries ScopeId, label, counters and weak game-instance data; its cleanup does not require calling a UObject.

Reserve two inline drain/fence positions with each client slot so permanent teardown can upgrade an in-progress A-to-B switch: old A already occupies one record and current B, if actually joined, occupies the second. Reject further switches while A is draining. Thus Deinitialize never allocates an unbounded third record and never fails because a new cleanup permit is unavailable.

Failure unwind before publication releases reservation, unjoined target/config, scope-client count and auth setup; no queue registration exists. Once a binding is detached, an IO refusal cannot pretend to restore it or erase selected commits. Record the actual failure, continue native quiescence/fencing, and preserve files. A same-path activation retry uses the single PendingActivation record and one coalesced later-frame tick; QueueBusy alone is retryable, with no new ticket/callback/list entry per attempt. Permanent close clears that target and prevents rejoin.

This is a bound on named native records, not a claim that an arbitrary transport, OS IO or application-owned snapshot finishes within a fixed elapsed time. Delayed admitted work occupies an existing slot; new managed client/switch admission refuses at capacity instead of growing retirement history.

#### Concrete control ownership, tick and module lifetime

Use one TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> created on GT by the HTTP module before managed client publication. It owns one fixed 256-client slot array. TryReserveClient reserves a free client index/generation and allocates that client's fixed 1024 operation-cell array before publishing a client; allocation/admission failure unwinds without registration. Each operation cell is selected before exchange construction, contains its atomic FManagedOperationSettlement and a native GT-release anchor, and is reused only after combined settlement and GT reclamation. Count reservation first, then cell reservation; a cell-refusal rolls back its still-unpublished admission count with no exchange/trace. A non-free but already counted-out cell may conservatively cause capacity refusal until the next GT sweep. No overflow allocation is allowed.

Atomic operation-cell reservation/publication is mandatory, including an any-thread Send paused between client-count acquisition and cell publication. Each fixed operation cell has an atomic packed tag {nonwrapping generation, Free/Claimed/Published/Reclaiming state}; zero generation is invalid. After the client gate CAS succeeds, Send CASes one Free tag to Claimed with acquire-release semantics. The GT never accesses a Claimed cell's non-atomic payload and cannot reclaim the client while that admission count is held. Sender initializes settlement atomics, installs the native release anchor and exact generation, then release-stores Published BEFORE exposing an exchange, Enter/task, callback or token. The GT acquire-loads Published before reading payload. Cell exhaustion rolls back the still-unpublished client count and takes no exchange/request identity; initialization failure publishes an owned canceled/discarded cleanup record if any GT-affine anchor has already been installed, rather than destroying that anchor on the sender thread.

The only reclaim owner is GT. After child admission is closed, combined terminal/tail/dispatch settlement is true, and every token has consumed its authority, GT CASes Published to Reclaiming with acquire-release semantics; it destroys the anchor on GT, resets payload, increments the nonwrapping generation and release-stores the next Free tag. A sender must acquire-CAS that exact Free generation. Token settlement validates generation and is exactly once; no token increments a reclaimed row. Overflow retires a slot closed instead of aliasing an old generation. A callback can retain an inert shared Control after consuming its token, but then has no authority to address the row. Scope/client-slot release waits for BOTH the client admission count and actual cell/anchor reclamation.

After initial Send publication, child-tail and physical-dispatch admission are GT-only. The GT closed check, cap check and count increment have no intervening external callout; publication/Start follows the successful increment and lease installation. Worker callbacks transfer an existing counted lease to their continuation; they do not increment after child admission closes. If real wiring requires another thread to admit children, replace this assumption with one linearized closed-plus-count gate before that wiring can pass review. A split atomic bClosed read followed by an independent increment is prohibited. A late initial sender after Stopped uses its captured Control and existing counted row only, publishes discarded native settlement without GT/user work, and cannot become an off-GT last owner of an anchor. Exit quarantine eligibility includes claimed/unpublished admissions and occupied client counts, not only already published anchors.

The reference graph is: module -> shared Control; Control -> fixed slots -> native gate/scope/bindings/drains and occupied operation cells; occupied cell -> native exchange/config release anchor; exchange and its move-only tail/IO/fence tokens -> shared Control plus exact slot/operation generation. Hub/host/wrapper owns its live client normally but is not on a path back from Control. The managed adapters capture Hub/FHubRuntime/UObjects weakly. Control contains no strong Hub, World, GameInstance, host, FHubRuntime or Blueprint result/auth carrier. Ordinary external native handles may keep their closed gate/control alive, but cannot obtain another occupied slot. The anchor is installed before the exchange/continuation is published, so a late off-GT guard drop cannot become the last owner that destroys GT-affine native config/clock/handler state. Sender writes happen before a release-published cell-ready flag; only GT accesses its non-atomic release anchor afterward. BP carrier cleanup remains with its GT UObject owner and is detached before any exit quarantine.

Startup registers exactly one FTSTicker callback on GT, capturing TWeakPtr<Control>, and stores its handle in the module. TickOwned pins it, rejects Stopped, scans at most 256 clients/512 drains and a fixed budget of 256 operation cells per tick using cursors, applies atomic settlements, advances replay/fences, and destroys ready anchors on GT. A scope closes only after all its occupied operation cells/other native obligations settle, not merely after one scan. Normal runtime reclamation clears anchors, then cells, then completed drain/scope records on GT. It never retains a historical completed list. Off-GT token completion/destruction only updates its preadmitted cell atomics and drops its token reference: no tick registration, task launch, UObject access, queue mutation or module-static lookup. The already registered ticker observes progress; if it has stopped there is no new dispatch.

At normal engine pre-exit, mark Control Stopping (refuse client/slot reservation), close every managed native gate without a callout, retire/cancel native work while the HTTP module still exists, clear GT-only adapters, then execute the existing owned persistence/control stop path. A final bounded GT sweep applies already settled records; it does not wait for arbitrary transports, invoke a general GT drain or manufacture settlement. At ShutdownModule, if needed first mark Stopping and close all managed gates; then mark Stopped before removing the ticker, remove its pre-exit/snapshot hooks, stop owned native services idempotently, and destroy only already quiescent GT anchors. If pre-exit did not run, do not assume the engine HTTP module still exists and issue late transport Cancel through it; use stopped/discarded native settlement and the exit quarantine for unresolved work. That fallback cannot claim successful cancellation. Any late guard/posted task first sees Stopped and records discarded atomic settlement; it must not publish Completed, start user work or enqueue another GT handback through a dead service. Already running user/OnHttpThread callbacks cannot be recalled; their counted tails remain outstanding. All late paths use their captured shared Control, never GetModule/GetService/static singleton lookups.

If unresolved native anchors remain at normal process shutdown, retain that same one preallocated Control with an explicit ExitSelfHold self-reference. This is a deliberately bounded process-exit quarantine of the existing occupied slots, with no new clients, cells, callbacks or records and no claimed successful Scope Closed/Flush. It prevents late off-GT token destruction from destroying GT-affine anchors after the GT/ticker has gone; do not rely on a static destructor to clean it. If all anchors settled, no self-hold is installed: clear on GT and release the module's Control normally. External closed handles may then retain an empty native control safely. This quarantine is forbidden for ordinary Hub teardown, account switches or normal runtime capacity recovery.

Code and state lifetime are distinct: UE5.8 normal UnloadModulesAtShutdown calls ShutdownModule and destroys the module object, but deliberately leaves DLL code mapped. Actual engine Engine/Source/Runtime/Core/Private/Modules/ModuleManager.cpp:1344–1348, :1372–1379 and :1488–1494 establish that boundary; it does not preserve a module member/static owner. FTSTicker::RemoveTicker waits for an executing callback and guarantees none afterward (Engine/Source/Runtime/Core/Public/Containers/Ticker.h:58–66). Set SupportsDynamicReloading=false for this stateful module. Forced live code unload with admitted callbacks is unsupported: no native reference graph can preserve function text after an external caller forcibly unmaps it, and SupportsDynamicReloading is not a veto on direct UnloadModule. A non-exit unload/reload must first prove native quiescence and remove hooks; a bounded engine-exit quarantine is not a live-reload solution. This is the precise remaining lifecycle limitation, not an implicit permission to wait globally or leak per-Hub owners.

### 5.4 Queue-only switch and permanent teardown

#### Queue-only switch

TrySwitchQueue validates the complete target and checks the existing switch slot before changing anything. On failure the old active configuration and registration remain untouched. On success:

1. Mark only the old binding Detached, remove it from future SelectOwner/BroadcastLater, and stop new Enqueue/Prepared commit selection through that exact binding key/epoch.
2. If it owns the active replay, request the retain-head interruption in section 5.5 before canceling that replay handle. Do not CancelAll ordinary HTTP for a queue-only switch.
3. Move the old binding into its pre-reserved native drain record, select its admitted-work watermark after detachment, and arrange owned Flush/fencing through section 5.6. No callback captures the old or current BP wrapper.
4. Publish the complete new environment/account/config with either its complete active binding or an explicit unavailable/paused QueueBusy registration. Use one controlled publication; do not leave intermediate WithBaseUrl/WithOfflineQueue setter publications able to join with mixed configuration. The native retry uses the current validated target and never reopens the old binding.
5. Existing ordinary sent requests retain their Send-time HTTP config. An old QueueWhenOffline exchange that reaches enqueue after explicit detach fails with Config/QueueDetached; it is never relabeled into B. A pre-CAS Prepare is aborted/cleaned. An admission past Committing finishes truthfully. New sends after publication capture B.
6. Count/results/public Flush describe only the current registration, not the native drain of A. Session/event subscriptions survive the switch, subject to the account/identity policy in section 8.

The permanent client gate stays open throughout a queue-only switch. Do not model detach as closing/reopening that gate or as replacing the shared gate object: copied native handles must keep observing one irreversible permanent close decision.

An unsent builder's ConfigAtBuild may retain A for body naming/default content-type, but normal Send still snapshots current native configuration, as M3 specifies. Such a builder may send through current B while the client is open. It may not use retained A as authority to enqueue or bypass permanent close. Already-sent A exchanges are the distinct old-binding case above.

Preserve M4b's ordinary code-first configuration-snapshot behavior. The new explicit managed detach is an additional revocation action; do not globally reinterpret every legacy WithOfflineQueue reconfiguration as cancellation of old complete Send snapshots. Namespace quiescence must also account for any remaining valid legacy captured operations before final release.

#### Permanent Hub/fallback teardown

BeginPermanentRetirement closes the shared gate once. No future ordinary send, queue join, auth refresh or probe can reopen it. The Hub sequence is:

1. Set wrapper bClosing and close ALL managed native gates, with no delegate invocation between those actions. Only then broadcast OnDeinitializing, so a listener borrowing an old native handle cannot send during a native-open window.
2. Explicitly detach active queue bindings and retain-head replay operations; upgrade an in-progress switch in the same native slot. Remove connectivity probe registrations and wrapper-owned native event subscriptions. New public Flush on a permanently closed client refuses; only the already reserved internal cleanup capability may flush detached snapshots.
3. Mark/cancel all admitted native work, including refresh/health/replay requests. The combined admission word includes posted Enter tasks. Suppress application/BP delivery while preserving core terminal observers and selected commit accounting.
4. Cancel/drop managed external refresh subscriptions and the BP bridge's one pending Finish generation without waiting for application re-login. Clear session waiter ownership/secrets quietly after request gates are interrupted; late native auth Done sees a stale refresh id/retired state and is ignored. Do not add a watchdog to an ordinary live Custom auth.
5. Transfer control to the module-owned service and release Hub/wrapper/FHubRuntime strong ownership immediately. Deinitialize never waits on its own UObject lifetime. The service settles native drain records and keeps scope metadata open while any pre-close counted operation can still emit Completed.
6. Emit Scope Closed once only after the combined operation count is zero, all occupied operation cells' GT release anchors are reclaimed, replay/probe/internal IO settlements are complete, the slot's drain records are done, and required last-member namespace fences are released. Existing external native handles/configs may remain alive, but their gate/bindings are closed and they do not own a live Hub.

If old readback is retained during drain, expose it only as old/draining state; it must never alias a newly reopened coordinator's count. Source callbacks already handed to OnHttpThread are past the existing cutoff; use weak/guarded ownership for Hub bridges rather than claiming they can be recalled.

### 5.5 Replay operation, compatible members and owned probe tokens

Do not clear bReplayActive immediately and let a surviving member start Items[0] while the old request is canceling. Retain one explicit active operation:

~~~cpp
enum class EReplayPhase : uint8
{
    Request, CancelRetain, TerminalPublished, RetireIo, Settled
};
enum class EReplayDisposition : uint8 { Unpublished, RetainNetwork, RetainDetach, Retire };
struct FReplayOperation final
{
    uint64 ReplayId = 0; // monotonic per coordinator, never reused
    FQueueMemberKey Owner;
    FString QueueId;
    uint64 Sequence = 0;
    EReplayPhase Phase;
    FVaRestRequestHandle Request;
    FOwnedProbeLease Probe;
    FManagedOperationRef Managed; // shared ordinary/replay settlement, not a second dispatch count
    EReplayDisposition Disposition = EReplayDisposition::Unpublished;
    TOptional<FVaRestQueuedResult> PublishedResult; // one result, never rewritten by later detach
    bool bTerminalAcceptanceStarted = false;
    bool bCoreTerminalSeen = false;
    bool bCompletedTraceReturned = false;
    bool bOwnerDetached = false;
};

void FOfflineQueue::DetachMember(const TSharedRef<FQueueBinding>& Binding,
                                FMemberDrain& OwnedDrain);
void FOfflineQueue::OnReplayCoreTerminal(const TSharedRef<FReplayOperation>& Op,
                                        FVaRestQueuedResult&& Result);
void FOfflineQueue::OnReplayCompletedTraceReturned(const TSharedRef<FReplayOperation>& Op);
void FOfflineQueue::TryAdvanceReplay(const TSharedRef<FReplayOperation>& Op);
void FOfflineQueue::OnRetireIoSettled(const TSharedRef<FReplayOperation>& Op,
                                    bool bActualOk, bool bDelivered);
~~~

Active membership lives only in the existing bounded live Members map, now keyed by binding identity/epoch rather than a historical ClientId alone. A closed binding is retained only by its drain/actual exchanges/IO snapshots. A stale callback validates exact Op identity/ReplayId/QueueId/sequence before affecting the active operation; a reused ClientId alone is not sufficient authority.

Detach branches by phase:

- Request with no published terminal: transition to CancelRetain BEFORE cancel. Remove the departing binding's selection/broadcast eligibility. Complete its owned probe token once. Keep Op active and Items[0] blocked until the core terminal witness and physical/guard handoffs settle. Then release Op without Retire IO; if other compatible live members remain, schedule replay with a fresh ReplayId and their current credentials. Do not treat this expected cancellation as a 10-second connectivity failure.
- Terminal acceptance started / TerminalPublished: mark bOwnerDetached and remove membership, but do not change the accepting/published result to retain, cancel an already finished exchange, or invalidate its operation. Success and other established terminal outcomes still retire normally after physical/GT tails and trace publication settle. A published Connection/Offline disposition retains under its established network rule. This branch includes synchronous retirement from Completed trace dispatch, before ordinary handler delivery or Retire IO admission.
- RetireIo: do not invalidate the accepted IO completion with a new member generation. Mark the member-drain outcome RetireAcceptedIO and allow that exact accepted write to finish. On actual success remove the matching head and charges even if the originating member has left. On failure preserve the actual failure/storage-unavailable semantics. Never report that the head was retained after a successful tombstone.
- Another member owns Request/RetireIo: do not cancel it, clear its retry timer, invalidate its generation or finish its probe. A's detach is not a queue-wide stop while B remains live.
- No active op: removing A does not reset a coordinator-wide backoff/retention rule needed by B. When the last live member leaves, close new coordinator joins, stop its scheduled replay/timer generation and begin last-member draining.

The accepted-retirement completion still notifies each compatible live registration present at the normal subscriber snapshot; omit detached A, preserve B's normal event. With no live members there is no application broadcast or next replay. Suppression of departed-owner delivery must not suppress unrelated compatible members' accepted result.

Core settlement cannot be an ordinary user OnComplete, nor can its only publication hook run after TracePoints::Completed. Use private default-noop hooks, illustratively BeginInternalTerminalAcceptance(), PublishInternalTerminal(const FFinish&) and InternalCompletedTraceReturned(). A private FQueueReplayResultHandler owns the raw result and operation, with Managed installed before Enter or any synchronous Started/completion callout. The ordered Complete seam for every managed dynamic typed operation is:

1. Respect the existing decoding-handler handback and ApiDepth deferral first; own a logical-tail lease throughout the actual completion call stack. A private bFinalizingInternal guard prevents duplicate/reentrant finalization while preserving cancellation flags; it is not terminal acceptance.
2. Invoke the typed handler's GT IResultHandler::Finalize(FFinish&) against its validated current schema/rules/carrier. This may fail conversion or execute native/default/copy code that reenters cancellation, owner teardown or permanent native close. No bFinished, BeginInternalTerminalAcceptance or replay success/disposition is frozen before it returns. Reentrant Cancel must still record its normal flag/reason and interruption; a recursive Complete observes bFinalizingInternal and leaves the authoritative candidate to this outer completion rather than publishing another result.
3. After Finalize, make provisional owner/cancel/gate decisions and perform any required rejected/partial carrier cleanup on GT. Then re-read owner validity, cancellation and the shared permanent gate/suppression state. If that read newly requires discarding a speculative value, clear it and re-read once more after cleanup; no value is reconstructed, so this does not become an unbounded materialization retry. Apply the established final cancellation/owner policy and truthful saved-file/selected-queue-commit exceptions. A finalization success raced by retirement becomes the existing canceled/suppressed result. A finalization failure remains its error unless that established cancellation policy overrides it. Preserve warnings/timing/attempt/result-type semantics; never convert success in a later public callback. The final read after all such callouts is the acceptance linearization point. No reflective copy/default code, destruction or other external callout may occur between that final recheck and the acceptance marker.
4. Close child admission, call BeginInternalTerminalAcceptance, set the private operation marker and Ex.bFinished, then EndWaits. These actions introduce no external callout before the marker. EndWaits can invoke reentrant stage code, but the result is now accepting under the earlier final sample; later retirement records departure without changing it to retain-head cancellation. Decorate final timing/error metadata, set Done/counting/map state, then PublishInternalTerminal(const FFinish&) seals the one queued result and RetainNetwork/RetainDetach/Retire disposition. These private publication hooks perform no IO or user callback and no mutable reflected materialization.
5. Dispatch TracePoints::Completed while the handler/internal observer and logical tail remain owned. A trace sink may now detach/retire the owner, but sees TerminalPublished and cannot overwrite it. After all sinks return, InternalCompletedTraceReturned marks that fact even for silent/absent user delivery. Deliver or suppress normal user callbacks with the existing options; an accepted off-thread delivery carries its counted logical-tail lease through callback return/destruction. Only after the native terminal handoff is complete set bLogicalTerminalSettled; releasing the current tail still cannot count out work with physical or other native tails pending.

Normal public handlers retain their behavior. The typed-bridge finalization must implement this ordering, including the post-Finalize recheck: calling BeginInternalTerminalAcceptance before a potentially reentrant native CopyScriptStruct/default initializer would freeze a success too early. If rejected-value cleanup can itself reenter, finish that cleanup before the final acceptance sample; do not insert such a callout after the sample.

TryAdvanceReplay is GT-only and requires its sealed core/trace flags PLUS the shared managed operation's combined settlement predicate, including zero logical and dispatch tails. It starts at most one Retire IO for a sealed Retire result, or releases the retained head only for its sealed retain disposition. Until that point the same Op stays active; neither detach nor a generation mismatch may clear it. For an accepted Retire IO its actual completion still controls item removal; successor scheduling occurs only after its operation has reached Settled. The all-operation work lease may release only at section 5.2's combined predicate, never merely when the replay hooks register IO; Scope Closed additionally waits for that reserved Op/drain/IO obligation. Keep the operation cell/generation authoritative until the replay control has consumed its settlement reference. An RAII discard witness settles internal accounting without inventing a successful terminal result.

Track physical/GT-tail settlement with a move-only FDispatchSettlementLease attached to every FDispatchTag/FCompletionGuard for EVERY managed operation, not just replay and not the exchange's single TransportCall slot. Acquire against the 32-concurrent-dispatch cap and increment the shared managed record's PendingDispatchSettlements immediately before each Start; install the lease before Start because completion may be synchronous. Move it with the guard into VaRest.AttemptDone and keep it until OnAttemptDone has returned, including its OnOutcome/Complete/reentrant callbacks. Only then decrement once. Guard destruction still sends TransportDropped through the same path during normal running; at Stopped, discard records only its atomic native settlement as specified in section 5.3. Destruction of a discarded posted task likewise releases its native tail once without user work. The preadmitted service GT tick observes the shared combined predicate; replay then adds its FIFO/Retire IO fence, whereas an ordinary managed operation simply completes its normal native release. No new callback allocation is needed to report zero. Never equate Cancel() return, resetting TransportCall/LiveDispatch, logical Completed or ignoring a stale dispatch tag with physical settlement. Count every dispatch, including overlapping/replaced calls and a custom stage's early logical completion. Sequential dispatches reuse capacity; no per-dispatch completed-history set is retained.

The transport contract is the boundary of this physical proof: its completion, or destruction of its uncalled callback, hands the dispatch back (Public/VaRestTransport.h:115–135). It cannot prove that a broken external transport which retains/uses a request after that contract has settled has stopped. A transport that never completes/drops its callback occupies the existing native slot; no timeout releases the head or fabricates settlement. Final implementation must wire every dispatch/guard/task discard edge to the one lease and demonstrate that callbacks cannot outlive their accounting. These are missing native mechanisms in accepted source, not already available helpers.

Probe ownership requires its own generation:

~~~cpp
struct FVaRestProbeToken { uint64 Generation = 0; };
void FVaRestConnectivity::RegisterOwnedProbe(
    uint64 CandidateClientId,
    TFunction<bool(FVaRestProbeToken)>&& Preferred,
    TFunction<bool(FVaRestProbeToken)>&& Fallback = {});
bool FVaRestConnectivity::CompleteOwnedProbe(FVaRestProbeToken Token);
~~~

RunProbe issues the scope token before TryStart and owns one active token. CompleteOwnedProbe changes state only if that exact token is still active, then disarms it once. Convert queue and health probes to this path. A probe candidate client and the actual queue replay owner can be different clients because SelectOwner chooses the oldest registrant; unregistering the candidate must not mistakenly cancel another owner's active replay. UnregisterProbe removes a candidate; only the operation owning the token completes it. A stale A callback after B/new generation starts is ignored instead of clearing B's bProbeRunning. Keep a compatible legacy API if required, but legacy no-argument completion must not clear a newer owned token.

Accepted source evidence: Private/VaRestOffline.cpp:506–597 selects/strongly retains Owner and retires all outcomes except Connection/Offline; :600–612 broadcasts to current live registrations. Private/VaRestPipeline.cpp:1263–1301 establishes bFinished/Done, removes InFlight and emits Completed before :1306–1311 can suppress the entire ordinary handler; :1362–1365 already refuses cancellation of a finished exchange. :96–135 moves the completion guard into the GT handback; :211–221 clears the held call before its late completion; :597–606 replaces the single live-dispatch slot; :968–983 ignores stale dispatches only for live-slot mutation. Private/VaRestExchangeState.h:30–37 and :131–140 contain dispatch tags/current-slot fields, with no aggregate settlement lease today. Private/VaRestConnectivity.cpp:165–189 uses one unversioned bProbeRunning and no-argument ProbeFinished today. These are real Task 5 native changes, not just wrapper flags.

### 5.6 Owned Flush, member completion and final registry fence

Use the existing queue Flush algorithm with a separately owned native settlement, not an application callback capturing a strong client/FHubRuntime:

~~~cpp
struct FNativeQueueFenceToken // move-only, pre-reserved in a service drain record
{
    TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
    FNativeSlotHandle Slot;
    uint8 DrainIndex = 0;
    void SettleOnce(EFenceResult Result); // any thread, records native atomic result only
    // Discard -> ShutdownDiscarded; no user callback or GT work from token destruction.
};
bool FOfflineQueue::FlushOwned(
    const TSharedRef<FQueueBinding>& DetachedSnapshot,
    uint64 ThroughAdmission,
    FNativeQueueFenceToken&& Token);
~~~

FlushOwned is private and uses the same LastAdmission/ticket/serial-barrier implementation as public FlushQueue. The reserved drain row supplies the polling/delivery owner; no new fallible PostPersistencePoll permit or service slot is needed at teardown. Public FlushQueue retains its existing immediate-bool/callback semantics. A user weak-state callback being suppressed does not settle or retain the service row.

To avoid a new anonymous unbounded control queue, the service scans its at most 512 admitted fence records on GT. It starts the existing serial persistence barrier once the detached binding's through-watermark admissions/cleanup and required replay disposition are ready. Worker barrier admission can still fail: return false/record NotAdmitted with no waiter, or record Failed after acceptance, exactly distinguishing those outcomes. Never increment PendingFlushes and then wait for a callback that was not retained. There is no retry-until-success loop for final Flush admission.

Every accepted namespace IO has a native settlement witness. Prepare/Commit/Abort/load/Retire callbacks record actual success/failure; callback destruction at normal refusal/shutdown records discarded settlement atomically in the captured Control. Only the already owned GT callback/tick applies queue/registry changes and GT cleanup; off-GT destruction never posts replacement work or dereferences a raw GT row. After Stopped it remains atomic-only under section 5.3's quarantine rules. Existing Commit/Abort already use control-during-stop; accepted Retire IO also needs a control settlement path so an ordinary callback being suppressed cannot leave its native IO count stuck. This does not authorize new IO after persistence stops. The witness's internal counters do not depend on a weak client or BP owner. Admission ticket ownership remains through actual cleanup.

Member drain readiness is local: detached binding closed, its pre-detach admission tickets/owned IO/owned accepted Flush witnesses settled, and any replay operation it owned settled or its accepted Retire IO completed. It does not wait for unrelated B's new HTTP or infinite stream of B queue operations. If B remains, finish A's drain row and keep the coordinator live with B under the original agreement rules. The global serial barrier can be behind already admitted unrelated persistence, but new unrelated work must not enlarge A's selected watermark indefinitely.

Last-member lifecycle is explicit:

~~~cpp
enum class ECoordinatorPhase : uint8 { Live, Draining, Fenced };
struct FRegistryEntry { uint64 Epoch; TWeakPtr<FOfflineQueue> Queue; };
~~~

When the last active member departs, enter Draining. Same-path Join returns QueueBusy; never create a second writer. Close replay scheduling/admission from explicitly revoked bindings, retain selected commits/accepted Retire IO, and finish all admitted through-watermark work. Any valid legacy captured operation not explicitly revoked by M5 remains accounted until it can no longer add namespace IO; do not break M4b old-complete-snapshot semantics accidentally.

That last sentence needs an actual capture mechanism, not shared_ptr/refcount guessing: add a move-only FQueueCaptureLease to an opted-in exchange and a native closed-plus-count FQueueCaptureGate per binding/coordinator. Snapshot Send's config and acquire its capture lease together while holding the existing client configuration read lock; publish a replacement config/close old capture admission in the corresponding exclusive GT transition. Thus an any-thread Send cannot read A, disappear from accounting while A fences, and later acquire uncounted authority. Do not implement another read-open/increment-later race. An exchange admitted with a legacy complete snapshot keeps that capture until terminal/actual admission cleanup and may enqueue during Draining through that existing capability. Managed explicit detach separately revokes enqueue/pre-CAS authority even for a previously captured exchange. A bare external immutable config/builder reference is not a work lease. No new Send can obtain a capture from an old detached config; ordinary Send still snapshots current client config. Fencing waits for the capture count as well as namespace IO. There is one atomic gate/counter and one move-only lease on actual exchanges, with overflow refusal; no list of historical captures. Exact source wiring must preserve lock ordering: no config lock held over Join, IO, trace or user callback.

Final fence requires: no live members; no permitted captured legacy queue work; no pending load; no active Request/CancelRetain/RetireIo operation or physical/guard tail able to write; no active admission/cleanup; no namespace IO; no accepted Flush waiter; and no scheduled continuation/timer with authority to start more IO. Owned continuations carry the coordinator epoch and become no-ops after Fenced.

Set Fenced BEFORE registry removal, then remove only if both path entry epoch and exact queue pointer still match. Unhook connectivity, clear replay timers/continuations and release retained Items/body charges in the inert old shell. External builders/configs may hold that shell indefinitely but cannot enqueue, select commit, flush, probe or schedule IO. A new Hub may then join the same durable path with a different connectivity object. Old Count is not the new coordinator's count. No persistent files are deleted by fencing.

A failed/refused Flush is an honest failed acknowledgment, not a reason to bypass IO quiescence or erase work. Fencing can release a quiescent failed old coordinator for normal validated recovery; the next opener must use existing recovery checks and must not claim the failed Flush was durable.

Accepted evidence: Private/VaRestOffline.cpp:615–652 defines the current watermark/barrier; :217–259 and :353–420 retain admission ownership. Private/VaRestQueueStore.cpp:362–402 already makes Commit/Abort control callbacks; :404–427 currently makes Retire an ordinary AsyncBool callback. Private/VaRestPersistence.cpp:116–151 bounds existing poll/completion permits, and :173–187 discards ordinary/deferred callbacks at stop. Private/VaRestOffline.cpp:98–105 currently rejects retained old coordinator connectivity; Public/VaRestClient.h:65–66 and Private/VaRestClient.cpp:193–197 explain why shared_ptr destruction is not a fence.

### 5.7 Managed auth waiters and pre-exit convergence

Use a tagged managed waiter entry rather than treating every refresh waiter as application work:

~~~cpp
enum class ERefreshWaiterKind : uint8 { RequestGate, External };
struct FManagedRefreshWaiter
{
    uint64 Id;
    uint64 RefreshGeneration;
    ERefreshWaiterKind Kind;
    FRefreshWaiter Callback;
};
~~~

RequestGate waiters are bounded by the managed operation permits. Bound External waiters to 64, including refresh-ended callbacks awaiting later GT delivery. Overflow never appends to Session.Refresh.Waiters. Live behavior remains asynchronous Failed(Config) on refusal; this is not a watchdog. For managed owners, place accepted deferred external delivery in reserved owner storage rather than creating an untracked retirement-dependent closure per request.

Permanent retirement first cancels parked request gates, then detaches/discards managed external waiter storage and advances the refresh generation. The BP auth adapter takes/drops its one pending Done/context on GT and removes its tracked UObject carrier. Late Done is harmless. Native worker contexts hold the client weakly today; do not replace that with a strong Hub/wrapper capture. Queue-only switch preserves the live session/refresh unless the separately chosen account policy changes it.

Integrate service pre-exit in the same native module path: close all managed gates/detach before process CancelForShutdown; then existing StopAll/StopPersistence settle accepted commit/abort/Retire control witnesses and discard user delivery; finally mark admitted native fence/waiter records ShutdownDiscarded as appropriate. Do not call StopPersistence per Hub and do not pump general GT work. A discarded user Flush callback must not leave a service slot dependent on it.

Ordinary deferred Enter/decode/transport work retains its native admitted lease until the combined logical/delivery/dispatch-tail predicate, including silent or discarded paths; the service does not falsely emit a normal Scope Closed while an ordinary request's old/replaced transport callback or native delivery tail remains. Engine exit can end with the explicit native Stopped/quarantined state in section 5.3 rather than pretending an ordinary runtime convergence happened. No dead Hub UObject is retained while waiting, and no new slots are admitted after stop.

Accepted evidence: Private/VaRestSession.cpp:44–58 currently moves ended waiters into a general later GT task; :504–540 appends/removes waiters; :544–555 starts guarded refresh; :558–625 ignores stale refresh ids. Public/VaRestClient.h:924–929 explicitly permits an indefinitely held auth Done. Private/VaRestHttpModule.cpp:31–36 and Private/VaRestPersistence.cpp:173–187 define current global stop order/owned drain.


### 5.8 Activation integration and exact native ownership paths

A newly created wrapper has complete ordinary HTTP config and a native managed-queue Paused status even while its account is empty; the optional queue target is absent until a nonempty account permits full name/target validation. Resolve of QueueWhenOffline checks that immutable managed status before entering Offline and produces QueueAccountRequired/QueueNotActivated as appropriate; ordinary HTTP remains usable. A paused wrapper with a fully validated selected target still has NO active Members entry or replay authority. They need not open/load a durable coordinator merely for readback. GetQueuedCount reports an explicitly unavailable/not-loaded selected-domain state until actual activation/load; it never returns the old drain's count as the new domain's count. Native TrySwitchQueue accepts a complete prepared target plus the explicit activation intent. A paused target publishes no active binding. Activation after explicit nonempty account and identity readiness consumes the one pending target; QueueBusy remains paused and retries one coalesced later-frame attempt under that explicit intent. The wrapper exposes exactly ActivateOfflineQueue, PauseOfflineQueue and state/detail getters, not two competing Resume aliases. Native code-first Join/snapshot behavior remains unchanged.

Task 5 owns the existing Source/VaRestHttp/Public/VaRestClient.h, VaRestConnectivity.h and VaRestResultHandler.h where the private friend/default-noop seams are declared; it introduces the default-noop IResultHandler::Finalize and internal terminal hooks with ordinary handlers unchanged; Private/VaRestClient.cpp, VaRestClientState.{h,cpp}, VaRestExchangeState.h, VaRestPipeline.cpp, VaRestOfflineInternal.h, VaRestOffline.cpp, VaRestQueueStore.{h,cpp}, VaRestPersistence.{h,cpp}, VaRestConnectivity.cpp, VaRestSession.cpp, VaRestHttpModule.cpp, and existing session/trace private declarations actually needed by those files. Add Private/VaRestNativeLifetime.{h,cpp} for the complete Control/service/slot/operation/tail/drain/fence/gate types and Private/VaRestManagedClient.{h,cpp} for the transactional factory/complete-target publication bridge. These foundations compile with native-only test callers before a Hub UObject/action exists. Task 4 transfers shared-file ownership here; Task 6 adds dynamic completion and Task 8 consumes the finished lifecycle API. Do not constrain Pipeline to scope plumbing or move native settlement into the later UObject task.

CreateManaged takes complete initial config/scope/name and a reserved native slot, constructs the state and installs its permanent gate/initial paused target BEFORE publishing Client Created or returning a handle. Failure unwinds prepublication scope/client counts/auth/config without a join. Store only the weak live client in the service; the Control's counted release anchors and exact snapshots supply required native lifetime after teardown. Public Flush wrappers use weak runtime/action captures; the separately reserved native fence is the cleanup owner.

New native tests: Source/VaRestHttp/Private/Tests/VaRestManagedAdmissionTest.cpp, VaRestManagedRetirementTest.cpp, VaRestReplaySettlementTest.cpp and VaRestNativeLifetimeTest.cpp. Use the actual fake transport/manual persistence/virtual-clock hooks through VaRestHttpTesting.{h,cpp}; additions there expose owned counters/latches only. The private lifecycle API has no arbitrary FRequestSpec/result-handler escape hatch. The transport callback contract and forced live-module-unmapping limitation above are explicit boundaries, not passing results. Section 12 includes every discriminating construction schedule.


## 6. API client and auth behavior

UVaRestApiClient is a UObject owned by its Hub facade while live. Its native backing comes from the bounded managed factory/epoch protocol. API policies include initial complete scope/name/connectivity/storage, asset retry/timeouts/cache/offline/envelope/naming/max concurrency/strict decode/JSON type/rules and per-asset RedactedKeys. WithRedactedKeys appends asset keys to accepted project/provider/session rules and persistence guards without renaming settings.

The instanced asset auth config is DuplicateObject'd into per-wrapper state and its native-only CreateAuth() builds the complete provider before managed native client publication; no Blueprint mutable auth state is shared across PIE clients. Native adapters use weak UObject references and GT event invocation. A native retired control must not keep a dead wrapper/config UObject alive just to settle a refresh; its internal guard/ticket is plain native and stale BP finishes refuse by epoch.

BP functions: SetBearerToken, SetSession, ImportSession, ExportSession, ClearSession, SetHeader/RemoveHeader, SetSessionHeader/RemoveSessionHeader, SetEnvironment/GetEnvironment/GetBaseUrl, SetQueueAccount, ActivateOfflineQueue/PauseOfflineQueue and queue state/error/readback, CancelAll(Tag), ClearCache, FlushQueue, SetApiKey, SetBasicCredentials, SetClientCredentials. Events: OnError, OnSessionChanged, OnSessionExpired, OnQueuedResult. Wrapper error forwarding occurs once for actual primary request errors; typed-output availability has its separate metadata path and cannot fabricate an HTTP failure.

UVaRestFlushQueueAction provides OnFlushed(bool) and OnRefused with nonsecret detail. Public bool false never retains a callback; the action schedules refusal with ordinary asynchronous/frame-boundary behavior. Accepted completion is later-frame for BP. Native retirement settlement does not rely on this user callback. Internal queue/flush completion and shutdown suppression follow section 5, so omitted BP delivery cannot strand cleanup.

Builtins:
- UVaRestBearerAuthConfig -> FVaRestAuth::Bearer.
- UVaRestApiKeyAuthConfig: Placement, Name, optional development Key; SetApiKey replaces the provider/registered secrets through managed scope pause/publication. Empty key retains Config refusal.
- UVaRestBasicAuthConfig: User and optional development Password; runtime setter replaces credentials/provider safely.
- UVaRestBearerRefreshAuthConfig: accepted FVaRestBearerRefreshConfig Config plus RefreshEndpoint:FName. Resolve endpoint to Config.RefreshPath and new native TraceEndpoint metadata only; empty name uses the accepted path. Do not rename RefreshBodyField/OAuthClientId/other frozen fields.
- UVaRestCustomAuthConfig: BlueprintNative Apply, Refresh, pure identity/contract/key/value queries and guarded FinishRefresh.
- UVaRestClientCredentialsAuthConfig: separate FVaRestClientCredentialsConfig and TokenEndpoint with the same URL-only resolution.

FVaRestClientCredentialsConfig fields:
TokenPath, ClientId, ClientSecret, Scope:FString;
ClientAuthMethod:EVaRestOAuthClientAuthMethod {Basic, Form}=Basic;
AccessTokenPath=access_token, ExpiresInPath=expires_in;
RefreshStatuses:TArray<int32>={401}, RefreshLeadSeconds:float=30;
TraceEndpoint:FName.
Validation refuses unusable token URL/path, control-bearing credentials/headers and invalid token paths before wire. Native FVaRestAuth::ClientCredentials(Config) is declared/implemented in Task 9's explicit VaRestAuth.h/.cpp ownership, backed by VaRestClientCredentials.cpp. Token POST uses grant_type=client_credentials, optional scope, and RFC-compatible client Basic auth or form client_id/client_secret. No refresh token is required or invented. Access token/lifetime/default expires_in drive proactive lead-time (at most half known lifetime) and one reactive single-flight refresh under accepted gate rules.

Client identity combines stable client ID, canonical token endpoint and scope; secret/provider replacement follows accepted identity boundaries rather than silently asserting continuity. Implement explicit GetIdentity/GetIdentityContractName/GetAuthName semantics and test replacement versus token rotation. Raw/computed credential forms and protected paths join redaction/persistence guards. Tokens/errors/rejection/connection failure follow accepted native refresh factories and gate semantics; do not rewrite BearerRefresh to support this grant.

SetSession(AccessToken, RefreshToken, ExpiresInSeconds, bSameIdentity) is the first-token entry. ImportSession accepts only ExportSession's validated trusted schema and consistency binding; no token-endpoint JSON reinterpretation. Development asset secrets are plainly labeled; runtime credentials are the normal path.

Custom Apply receives ordered headers/query plus read-only session view and decimal RequestId, returns a checked result. Validate all header/query edits before copying back; preserve repeats. Identity queries are pure GT and never call Apply; absent usable identity bypasses cache and refuses durable activation/admission per accepted rules. Optional stable nonsecret IdentityContractName enables only declared-compatible instances with the same real identity.

UVaRestAuthRefreshContext wraps a value copy of native FVaRestAuthRefreshContext, exposing session/decimal refresh ID and RequestJson. Add native RequestDynamic sibling inside the friend class: take the shared one-time Preset ID and call the validated dynamic builder then MarkAuthRefresh(Preset). The action must use that prepared context builder, not reconstruct an ordinary client request in Activate. Preserve no auth/session headers/park/replay, no envelope/retry/limiter, and the one-ID-only rule across context copies.

EVaRestCustomRefreshOutcome: Refreshed, Rejected, Failed. FinishRefresh takes outcome, new tokens/expiry, FailedKind and message. One guarded pending finish per client/refresh epoch; duplicate/stale finishes warn and do not mutate another session. Dropped Done follows accepted Failed(Connection)/keep-session behavior. A live Custom auth may hold Done indefinitely; requests retain their own deadline/cancel behavior, and managed retirement explicitly disposes its own waiters/guards without a live watchdog. Refreshed/Rejected/Failed(kind) map to existing native factories; Unavailable is never offered as a custom finish result.

Auth fixture tests use the real existing server where possible, adding one strict client-credentials route/selftest. Default RFC snake-case token fields must set known expiry and prove proactive and reactive refresh; custom path and Basic/Form negatives remain distinct. A deliberately held live refresh survives days of virtual time; teardown still settles its owned waiters without waiting for the UI.

## 7. Dynamic requests, frame-terminal ordering, ownership and honest bounds

### 7.1 Validated public shape and native shell

Add VaRestDynamicRequest.h/.cpp and an exported FVaRestDynamicResultConfig:
ResponseType/ErrorType:FVaRestTypeRef; RuleOwner:const UObject* used only by GT activation; optional shared native budget.
FVaRestClient::RequestDynamic(Verb, Path, Config&&) returns TVaRestRequest<FVaRestDynamicResult>. It uses the existing private spec constructor, ordinary builder validation, Resolve and transport. Public config cannot replace IResultHandler or expose FRequestSpec. Auth-refresh context is the existing friend and uses its one-time preset ID. RequestDynamic activation is GT-only; native ordinary Request<T> remains any-thread Send as accepted.

FVaRestDynamicResult is a copyable, retainable native shell. Published data is only immutable/shared native JSON/raw text/byte payload and plain type/outcome/availability metadata. It has no UScriptStruct/UEnum/UObject/FProperty pointer, FInstancedStruct/FInstancedPropertyBag, native clock/config reference or GT release token. Public native callbacks inspect raw JSON/text/bytes plus declared-type validation metadata; callers needing a native T use unchanged Request<T>. Shell copies/destruction may occur on another thread because no reflected destruction or GT permit obligation is inside them. Existing JSON shared-document mutation rules still apply; callers clone before independent/concurrent mutation.

The only reflected Take APIs are private GT preparation-token operations, not methods on a retained shell. A token is valid only while the bridge is synchronously preparing that one action invocation; it cannot be copied into public native callbacks or queued work. Take validates descriptor/generations/current destination, copies immediately, resets its tracked carrier in ON_SCOPE_EXIT and returns validity. After preparation, terminal publication, cancellation or invalidation, token lookup always refuses; native user copies cannot keep/reopen a carrier. Type/rule pins and budget release are owned by the GT pool independently of the shell.

### 7.2 Admitted native GT state and bounded release

FVaRestDynamicRuntime is a GT-owned module-lifetime dynamic control with a finite admitted-slot pool, separate from FNativeLifetimeControl's reflection-free native slots. Each admitted operation reserves one GT state and frame-completion slot before worker launch or retention. The Hub has a per-budget admission limit and a module aggregate limit; default proposed limits are below. A slot owns copied config/decode state, descriptor/bag pins, captured schema/rule/target generations, worker handoff ownership, tracked carriers and exactly-once release. GT registry owns the state strongly; workers/native result shells carry only plain ticket/lease IDs and immutable work payload, never ownership that could destruct reflected state on the last worker release.

Normal GT completion, cancel, stale result, owner loss, shutdown and Take all converge through the same release state. A worker-return ticket remains until its handoff is settled; cancellation can clear editor carriers/pins immediately when the immutable worker does not use them, but cannot release a cooked worker's referenced closure before its worker-tail fence. Native module/control lifetime and late discard use section 5's captured shared-Control/atomic-settlement protocol, with a preadmitted dynamic-work lease alongside managed operation accounting. GT reflected state is not placed in FNativeLifetimeControl's native exit quarantine. In initial GT mode the GT releases all dynamic carriers/pins/actions and turns slots into immutable/discarded native handoff records before stopping; worker completion then settles atomics only. Strong GT roots are released on GT, not by the last worker token. Do not access a destroyed function-static registry from a late worker. On ordinary Hub deinit use nonblocking native settlement; on process shutdown suppress user delivery and drain/discard only owned handoffs under the separately qualified control lifetime. No nested GT pump/general task-graph drain is introduced.

The initial editor and cooked fallback paths materialize reflected values only on GT; their workers parse/envelope only and do not retain mutable reflection pins. Cooked reflected workers remain disabled until G-COOKED-CLOSURE proves lifecycle/thread/release/control shutdown requirements. A failed qualification uses GT mode or explicit preworker Config, not an unsafe fallback.

### 7.3 Actual later-frame native completion

Extend IResultHandler with default-noop Finalize(FFinish&) on GT, default false RequiresGameThreadDelivery(), and a constrained completion-policy query (ordinary handlers unchanged; the new dynamic bridge selects LaterFrame). Dynamic OnHttpThread is Config at Resolve. FClientState routes a dynamic pending finish into its already reserved slot BEFORE bFinished/bDone, removal from InFlight, stats completion or TracePoints::Completed. Even local Config/error/queued outcomes honor the action policy; refusal outside admitted slots uses ordinary asynchronous Config behavior and its separately disclosed caller-driven cost.

The frame control registers OnEndFrame and stamps the current GFrameCounter at readiness. It processes only a finite entry snapshot whose ready frame is strictly older than the current frame. A completion produced inside that drain cannot run in that same drain. Reentrant drain is blocked; no nested pump/sleep. Send and Activate themselves still execute immediately within the actual BP frame, so Started captures the correct owner/function/code offset.

Pending exchange state prevents another stage/attempt from starting after an answer was accepted, while retaining cancellation/owner/native gate accounting. The later-frame turn:
1. Recover the owned finish/handler; resolve owner loss, cancellation, shutdown and managed gate/epoch disposition using native truthful commit cutoffs.
2. Skip materialization for queued acknowledgement, owner-gone/shutdown and user-canceled success. For an ordinary terminal, call Finalize exactly once with a recursion guard.
3. Finalize may execute native constructors/default copies or the constrained internal preparation function. After it returns, make provisional owner/cancel/managed-gate decisions and discard rejected/partial payload as required, preserving selected file/queue commit truth. No terminal acceptance is frozen yet.
4. Reset every remaining private native reflected carrier and end the preparation token on GT. Any prepared BP destination is engine-owned and has no native retained address. Re-read owner/cancel/managed gate AFTER all such callouts/cleanup. If that sample newly invalidates a prepared value, invalidate its plain preparation cookie, clear any remaining native value and re-read once after cleanup; do not reconstruct. The final sample selects the authoritative canceled/silent/error/success outcome.
5. With no reflective copy/destruction or external callout between final sample and marker, close child admission, select native terminal acceptance, then EndWaits under the duplicate/reentrancy guard. Mark done/account once, seal internal replay result before trace dispatch, and emit Completed with final primary outcome/ResultType/warnings/body/decode timing.
6. Deliver immutable native shell/metadata and the action continuation according to section 9. A trace-sink/editor change after step 5 never rewrites this accepted Success into Config; it can make typed output unavailable or suppress owner delivery explicitly.

This reconciles frame-N stale/cancel behavior with IN5. Finalize cannot happen only inside OnSuccess after Completed. Ordinary compile-time handlers keep their existing timing/delivery/worker decode and default-noop finalizer.

### 7.4 Decode, materialization and outcome semantics

Worker Decode uses captured plain kinds and immutable options. JSON results parse owned bytes and apply the existing envelope once. None uses the existing predicate check; Text uses accepted UTF-8 repair and Bytes owns raw bytes, subject to the new dynamic input/logical admission policy. HTTP error JSON still uses ProcessErrorBody under the same finite parser options and preserves raw error response bytes.

Editor Decode never reads mutable types/properties/enums/actions/assets/destination memory. Finalize checks captured schema AND owner/default-rule generations before reflected access. A stale success is Config with Code=DynamicTypeChanged and a retry-after-editor-change message. A valid schema that mismatches JSON is Decode with first path/warnings/body diagnostics as accepted; finite local admission/work/output refusal is Config/DecodeBudget. Typed error conversion never overwrites the primary Http/Server/Auth/etc error; unavailable output is explicit.

Single struct/Json carrier is FInstancedStruct initialized with authored/native defaults. Scalar/array/map uses one-Value FInstancedPropertyBag; DateTime is Struct/FDateTime and FString map keys are explicit. Create descriptors/bag types on GT, including a stable Value descriptor ID, and weak bounded descriptor-cache entries keyed by type serial/container/schema. Do not create a fresh random descriptor GUID/property-bag type per request. An LRU weak cache bounds plugin bookkeeping; live admitted states own strong pins.

JsonToStruct/JsonToProperty runs with the captured rule snapshot and logical limits. Handle empty/204 as default/no payload according to the declared kind; preserve missing-envelope-DataPath failure for typed data. GetResultType is captured on GT from the descriptor, including clear container identity; workers never read a live type name. Decode warnings and original error/body stay intact. DecodeEndAt includes reflected materialization; separate metrics record parse, waiting-for-frame, GT initialization/conversion/copy/reset durations rather than calling the entire interval GT cost.

### 7.5 Finite logical limits and explicit non-guarantees

Initial numerical defaults are proposed for Root's review and must be validated/tuned from measured M5/M7 evidence; they are not RSS/latency promises.

| Setting / account | Proposed default and scope |
| --- | --- |
| DynamicMaxPending | 64 successfully admitted operations per Hub budget |
| DynamicGlobalMaxPending | 256 successfully admitted operations/control slots in the module |
| DynamicMaxInputBytes | 32 MiB retained in-memory input per dynamic result, including Text/Bytes/error body (SaveTo remains streaming) |
| DynamicMaxPendingInputBytes | 128 MiB admitted retained input charge aggregate per budget |
| DynamicParseAdmissionBytes | 128 MiB estimate-account allowance per budget |
| DynamicMaxParseValues | 1,000,000 counted JSON nodes/keys before allocation of the counted item |
| DynamicMaxParseWorkUnits | 8,000,000 lexical/event units (input byte traversal and semantic node/key work), not a universal CPU bound |
| DynamicMaxOutputBytes | 16 MiB converter requested/logical output charge |
| DynamicPendingOutputBytes | 128 MiB converter requested/logical reservations per budget |
| DynamicMaxValues | 1,000,000 conversion scalar/container/default-visit units |
| DynamicMaxWorkUnits | 8,000,000 conversion walk/match/copy logical units |

Zero/unlimited from a code-first setting does not silently disable dynamic safety admission: dynamic settings reject invalid/nonpositive values as configuration. Ordinary code-first converter/parse zero options remain unchanged. Overflow-safe arithmetic precedes reservations and Count*ElementSize growth checks. Reserve input ownership before retaining/copying a body for pending frame work; release/correct charges at transfer/cleanup exactly once.

The old “8*BodyBytes+2 MiB covers actual DOM/workspace” assertion is removed. If retained as an initial parse ADMISSION ESTIMATE, label it as such and record actual requested arena/scratch/input ownership separately. A one-million-zero array has about 2 MB input, >32 MB nodes, and >48 MB nodes/pointer/scratch lower bound on this target; it must be a measurement/negative fixture. The finite MaxParseValues/MaxInputBytes gates are actual count/input controls, not proof that the estimate is a physical cap. Parser MaxWorkUnits counts the declared logical work; hashing/collision CPU and allocator slack are not universally timed/bounded by it.

Converter checks cover requested string/text byte sizes, base64 buffers, element storage, maps/sets and nested values before controlled allocation/growth. Charge/report temporaries and Json deep-copy requested ownership where observable. Authored defaults, native constructors/copy/destructors, container allocator quantization/slack/hash capacity, engine BP destination copies and caller-retained shell values can allocate beyond that logical charge. The draft explicitly does NOT claim a 16 MiB physical typed-allocation ceiling or universal global peak. A large-default {} and arrays of nested defaults must show this difference honestly and exercise whatever descriptor/default preflight refusal is actually implemented; no success claim based solely on a token reservation.

The finite pending count covers SUCCESSFULLY ADMITTED dynamic operations and private carriers. Caller-created UObject actions and ordinary async notifications for refused/invalid requests are not covered by that count; repeatedly refusing in one frame can retain caller-driven notification/action costs. Preserve ordinary async Config delivery, document that remaining global-backlog/peak limit as unproven, and do not claim “no unbounded frame queue” over all callers. No generic GMalloc interception or detached reflection framework is required.

## 8. Editor mutation, cooked qualification and reflection-free public release

### 8.1 Global invalidation and actual hook lifecycle

FVaRestDynamicBridge holds GT tracked state, global SchemaGeneration, default/owner rule generations and target-binding generation. InvalidateAll increments schema/mutation generation, synchronously resets tracked mutable carriers while the old layout is valid and marks pending preterminal success stale. Primary HTTP/envelope errors retain their primary result with typed-body unavailable. Rule-only changes increment the owner/default rule tokens and take the same conservative path. No transitive root-only approximation.

Editor raw owner FDynamicBridgeEditorHooks is registered by FVaRestEditorModule Startup and removed at Shutdown, following the existing no-static-destructor listener pattern. Register:
- FStructureEditorUtils::INotifyOnStructChanged PreChange; PostChange schedules settling. Preserve existing JSON cache resets at both.
- FEnumEditorUtils::INotifyOnEnumChanged via its actual nested FEnumEditorManager listener; PreChange invalidates before enum edits.
- Cast<UTransBuffer>(GEditor->Trans) when available; OnBeforeRedoUndo(const FTransactionContext&) invalidates before deserialization; OnUndo/OnRedo(const FTransactionContext&, bool) schedule settling even after unsuccessful mutation attempts. Retain the actual weak buffer and handles; unbind the same buffer. There is no GetTransactionManager API.
- FCoreUObjectDelegates::OnPackageReloaded at PrePackageLoad/PrePackageFixup; PostBatchPostGC schedules settling.
- GEditor->OnBlueprintPreCompile(UBlueprint*) invalidates target bindings/prepared outputs before owning BP compile/reinstance; compiled/reinstanced notifications schedule settling. This is separate from UDS schema invalidation.
- JsonRules pre-publication and Api NotifyEdited/BeginDestroy invalidation; editor OnEdited refreshes menus/nodes once after the batch.

If GEditor/transaction buffer is absent (commandlet, startup or replacement), editor dynamic MATERIALIZATION remains disabled Config/EditorMutationHooksUnavailable until required hooks are installed. Do not silently accept mutable reflection without pre-undo protection. Recheck and safely replace the weak transaction-buffer binding at appropriate editor initialization/end-frame hooks; missing-buffer tests qualify this behavior. Shutdown clears carriers and disables new materialization before removing listeners. Known automation no-ShutdownModule behavior may intentionally retain the raw owner with no static destructor, but not advertise a completed teardown it never ran.

OnEndFrame settles only on a strictly later frame with no new pre-change since scheduling. All activation/finalization/getters require no mutation in progress and current schema/rule/target tokens. No synchronous worker wait or GT pump. Reentrant BP compile from user callbacks has no plugin-owned reflected carrier left; engine-owned output memory is never addressed from a cached native pointer.

Actual evidence: UDS compiler PreChange at 599 precedes dependent property rewrites at 118–181 and property destruction at 221–231; InstancedStruct Reset destroys through current reflection. TransBuffer's actual delegates are 201–228. EditorEngine exposes Trans at 416 and Blueprint precompile/compiled/reinstanced at 753–772. These APIs justify hook placement, not an unexecuted lifetime claim.

### 8.2 Cooked closure and native operations

GT activation enumerates every root and transitive FStructProperty, array inner, set element, map key/value, FOptionalProperty value, enum/int-property enum reference and bag descriptor. It rejects unsupported/missing property categories before launch rather than silently skipping part of the declared payload. Pin every referenced UScriptStruct/UDS/UEnum/UPropertyBag and rule owner in the GT slot; capture immutable rules, GUID maps/default/owner rule resolution and result-type name. Missing required imported rules under registered folders refuses before wire/worker. Worker sees raw stable qualified reflection pointers only while the GT-owned closure lease is live; it never looks up a live registry or asset.

A native lifecycle qualification registry is explicit and conservative. Known builtin POD/date/Json cases get audited entries. Project/native USTRUCT lifecycle code must declare/qualify initialization, defaults, copy, destruction, nonreflected dependencies and relevant hash/equality operations for the selected thread, with no reflection-mutating reentrancy. IsNative, zero-constructor flag or a strong root alone is not sufficient. A GT-only native type may use GT materialization under its GT lifecycle contract; unknown/unsafe operations fail Config. Existing compile-time Request<T> does not use or change this new qualification registry.

Initial cooked mode is the same safe GT reflected path. Enabling worker materialization is per qualified closure, after counters prove no GT-only constructor/copy/destructor or unsupported Optional dependency escaped validation; owned handback/release/module shutdown is part of the gate. Removing a genuinely required pin must fail a deterministic closure/lifetime check, not crash. A redundant pin counter whose removal does not alter real lifetime is not accepted proof.

G-COOKED-CLOSURE includes a concrete module-stop proof for every referenced closure and worker handoff; if stopping could leave reflected pins/carriers accessible to a worker after GT reflection teardown, worker mode remains disabled. Initial GT fallback destroys all mutable carriers/pins on GT before native exit quarantine, leaving immutable worker payloads only. A worker-produced carrier, only after that gate passes, is transferred through an owned handoff into GT tracked state; no last worker reference owns TStrongObjectPtr pins or a GT-only carrier destructor. Cancel/shutdown cannot discard a referenced closure early. Before any user callback, including Completed sinks, private carriers are consumed/reset on GT, and public shells are inert with respect to reflection. A qualification failure keeps worker mode disabled; it is recorded and does not weaken the GT fallback contract.

## 9. Async actions, typed transfer and callback availability

### 9.1 Action/handle and callback ordering

UVaRestRequestAction derives UBlueprintAsyncActionBase. Its internal factory captures the configured client/builder/body bytes and weak world context, binds internal preparation/continuation delegates, then Activate sends immediately with Owner(WorldContext), Endpoint and the accepted source-location/initiator boundary. RegisterWithGameInstance protects GI actions; the fallback uses the native admitted-action/GT ownership path because RegisterWithGameInstance does nothing without a GI. No request handle is treated as strong lifetime ownership.

UVaRestRequestHandle wraps the weak native handle: Cancel, IsActive, GetIdText (canonical decimal FString). GetId native remains uint64. Queue/file commit cutoffs stay native; cancellation before later-frame dynamic terminal acceptance is still actionable. BeginDestroy/host close cancel/unbind idempotently; action callbacks are weak-owner/suppression checked at final delivery.

Public metadata events:
- OnSuccess(FVaRestResponseData Http);
- OnError(FVaRestError Error);
- OnQueued(FString QueueId);
- OnProgress(signed byte counters/totals);
- OnTypedOutputUnavailable(FVaRestResponseData Http, FVaRestError OriginalError, EVaRestTypedOutputUnavailableReason Reason).
Reasons: TypeChangedAfterCompletion, TargetChangedAfterCompletion, OutputSuperseded, OutputNotBound. It is a delivery-availability surface, not a rewritten native HTTP error/outcome. Exactly one action terminal route is selected; public progress is coalesced and never follows terminal.

Ordinary dynamic action completions/progress/flush notifications cross the selected real later-frame boundary; request capture is immediate. This is not a claim that synchronous auth Apply or the accepted narrow connectivity OnChanged observer becomes deferred. Native session/queued-result semantics preserve accepted ordering; reflected wrappers document their own policy and never suppress required internal settlement.

Simple UVaRestGetJsonAction adds OnSuccessJson(immutable FVaRestJson, Http), with one native plain-JSON Take before broadcasting; multiple listeners share the accepted immutable document convention. GetJson, DownloadFile and UploadFile use the same cancellation/owner/frame policy and accepted SaveTo/BodyFile streaming. They do not invent a named queue for the Hub default. UVaRestHttpLibrary::DescribeError applies native Describe then EscapeForLog; Error.Url is redacted but unescaped until this display helper. Raw response EffectiveUrl remains raw metadata with its existing credential caveat.

### 9.2 Two internal events and explicit post-cutoff availability

The typed K2 node must not leave a native FInstancedStruct local alive across BP or trace callbacks. The chosen construction has a single internal preparation consumer and a separate generated user continuation; it requires G-BP-TRANSFER before full implementation.

Binding is private/BlueprintInternalUseOnly and BindOnce. It accepts fixed native-signature dynamic SINGLECAST delegates created by UK2Node_CreateDelegate targeting two generated functions on the current BP owner: PrepareOutput(Action, RequestIdText) and ContinueOutput(Action, RequestIdText, primary terminal metadata). A third fixed-signature metadata-only availability event is the safe branch when typed continuation is unavailable. No public multicast can request another typed Take. Public metadata/Json delegates can multicast freely because no private reflected carrier survives them.

Preparation is invoked synchronously during Finalize, before native terminal acceptance/Completed:
1. Validate weak target, captured target-binding epoch/class/function signatures and the action's private Preparing phase/cookie.
2. Generated PrepareOutput executes only internal CustomThunk getters and assignments into compiler-owned event-graph storage for Response/ErrorBody/valid flags plus a RequestId cookie. It has NO link to user execution, latent nodes or arbitrary delegates.
3. Each getter uses Stack.MostRecentProperty/Address only during that call; validates exact destination shape plus schema/rule generation; copies immediately and resets the source carrier in ON_SCOPE_EXIT, including failure. No address/FProperty/UFunction from Stack survives the call.
4. An internal ConfirmPrepared records the invocation cookie/validity with plain IDs, then returns. Runtime resets remaining carriers and closes the token before continuing Complete. Failed or missing required preparation is Config before acceptance; optional typed-error failure leaves the primary error and false validity.

The exact shared-storage construction uses compiler-generated event-graph variables/TS_ForcedShared terms for the node's generated callbacks, with deterministic node GUID identity and per-invocation RequestId cookie. Actual UE TemporaryVariable.cpp:41–51 and KismetCompilerMisc.cpp:2929–2937 create EventGraphLocals; bIsPersistent only contributes CPF_SaveGame at KismetCompiler.cpp:1485–1487 and is NOT a lifetime proof. Generated functions/storage must be inspected in the compiled BP by the gate. Two activations of one node cannot silently consume each other's prepared output: reentrant frame drain is blocked and the continuation checks the exact cookie; supersession takes the explicit unavailable path.

After Complete accepts/emits the primary terminal result, a synchronous trace sink may compile/reinstance the target BP or change a type/rule before Deliver. The captured generations/target signature are rechecked AFTER Completed returns. Never cache a raw frame pointer or invoke a stale UFunction to “finish delivery.” If generations/target are still current, resolve the current generated continuation by name/signature on the weak live owner, check cookie and read the engine-owned outputs. Invoke the typed continuation before any public metadata multicast for that success/error route, so an earlier public listener cannot invalidate prepared storage between this check and typed delivery. Once that continuation begins user execution, mark typed delivery consumed exactly once. Subsequent metadata multicast uses only plain metadata and fresh weak-owner checks; it cannot request another Take or emit a second typed availability route. A callback that retires/destroys the owner may suppress remaining delivery under the existing owner policy. If changed, do not read old prepared storage or invoke the typed continuation; emit the metadata-only OnTypedOutputUnavailable route (or suppress if owner/Hub/shutdown is gone). Completed Success remains Success. The original HTTP error is retained on that route if primary outcome was an error; optional typed error-body invalidity never fabricates a second primary error.

For preterminal mutation while waiting in frame N, Finalize selects Config/DynamicTypeChanged before Completed. For postterminal mutation, availability is explicit and there is NO silent default Response followed by OnSuccess. Owner destruction/Hub retirement can suppress action delivery after the accepted cutoff; it cannot rewrite the already accepted native result. The action exposes current terminal metadata/availability for native diagnostics without a reflected carrier.

Once user nodes run, their typed values are engine-owned copies. Native result shells contain no reflected payload. Engine recompilation of already executing Blueprint-owned values remains the engine's lifecycle responsibility; the plugin retains no mutable native layout or stack/property address across that call. Do not claim that persistent-frame storage alone proves all reinstancing cases.

### 9.3 G-BP-TRANSFER — exact first construction gate

Task 10 owns:
- Source/VaRestHttp/Public/VaRestRequestAction.h and Private/VaRestRequestAction.cpp;
- private DynamicBridge preparation/token/availability interfaces;
- Source/VaRestEditor/Private/Tests/VaRestDynamicTransferConstructionTest.cpp;
- Source/VaRestEditor/Private/Tests/VaRestRequestNodeTestUtils.{h,cpp} for a minimal generated graph.

Before Task 11, build a minimal code-authored BP using real CreateDelegate/CustomEvent/assignment/CustomThunk expansion, compile it and inspect the generated UFunctions/properties/storage. Trace real RequestId/cookies, preparation count, native carrier-live/reset count, addresses-retained-after-thunk (must be zero), continuation/unavailable counts, owner target generation, GFrameCounter and release thread. Ensure prepare returns without user exec; reset precedes Completed and user callbacks; one cookie cannot read another activation's value.

Exercise:
- parsed result waits across a real frame; cancel/owner loss/rule edit/UDS edit before acceptance;
- a Completed sink synchronously compiles the target BP, recompiles a dependent UDS/property-bag type, edits rules, retires Hub or destroys target before Deliver;
- user continuation reenters compilation, with native carrier count already zero;
- two overlapping/reentrant activations from the same node and multiple metadata subscribers;
- missing/retyped generated function, absent persistent-frame configuration where applicable, and an owner reinstanced or destroyed during completion;
- native public result copies retained and released on a worker after action cleanup.

Compare accepted trace outcome and user availability: pre-cutoff stale is Config; post-cutoff stale is unchanged primary outcome plus explicit unavailable/suppression. Negative mutants restore same-frame completion, leave a native carrier alive, skip cookie/generation checks or run user exec from prepare; each fails a counter/value invariant without intentionally crashing.

STOP/REPLAN: if generated storage/control flow, reinstance/destruction behavior or cookie separation cannot be proved with actual UE source plus the above fixture, Task 10 does not pass and Task 11 does not start. Root chooses a bounded alternate transfer construction; do not silently fall back to post-Completed typed decode, default-valued typed Success, a detached-reflection framework or compiler scheduling assumptions. This draft contains the gate and intended construction; it does not claim that gate has run.

## 10. K2 surface, asset editing and protocol extensibility

### 10.1 Call Endpoint

UK2Node_VaRestCallEndpoint properties: Api:UVaRestApi, EndpointId:FGuid, EndpointName:FName. Resolve ID first, name fallback/display only. Node details and menu actions expose endpoint selection; a rename keeps GUID binding. Missing API/endpoint/type is a compiler error retaining old pins. Deprecation is a warning.

Inputs: one typed path/query/header param; undeclared path templates are string pins; additional Query map; typed Body for a declared request; advanced Body Json for every JSON body; Tag. Outputs: Then, typed OnSuccess Response + Http, OnError Error plus optional ErrorBody/ErrorBodyValid, OnQueued only when allowed, Handle, and explicit OnTypedOutputUnavailable metadata branch. An OnSuccess typed pin is never silently populated with a default because postterminal delivery failed.

Response/Body pin mapping is exact: Struct/Json -> PC_Struct plus script struct, DateTime -> PC_Struct/FDateTime, Double -> PC_Real/PC_Double, Int/Int64 -> corresponding signed categories, Enum -> accepted engine byte/enum mapping with UEnum, Bytes -> byte array, arrays/maps -> the one supported container with FString map key. Validate descriptors before pin construction and activation. Do not invent a DateTime pin category. Default values use actual K2 validation/import helpers and enum internal identities; errors are compile diagnostics, not silent coercions.

Optional params are advanced, omitted only when unlinked AND still at their autogenerated default. Linked false/zero/empty values are sent. Removed params remain with removed tooltip and are never sent. Use exact FString Name on the wire, never PinName. Query arrays use accepted literal-delimiter AddQueryValues; path array support adds constrained ParamValues that separately percent-encodes elements and inserts literal comma delimiters during validated template substitution. Existing scalar Param remains unchanged; do not pass an already escaped opaque path string to bypass validation. Header arrays join validated values by comma.

Body Json wins when linked, preserving explicit false/zero/null independently of typed default omission. Struct/scalar/array/map JSON bodies use the same captured owner rules and per-request media precedence; runtime property addresses are consumed synchronously at building, never retained across tasks. A flat Form struct uses rules and ordered scalar/repeated-array fields, null as empty; nested objects/container objects refuse Config. Multipart uses the reflected checked wrapper; file parts stream. Binary uses bytes and selected media; Text uses UTF-8/native repair policy and selected media. No request Body selection renames native EVaRestBodyKind.

ReallocatePinsDuringReconstruction retains old unmatched/retyped pins using bOrphanedPin/bSavePinIfOrphaned with explicit tooltip. Linked incompatible types fail compile instead of silently changing values. Existing bRemoved declarations preserve their pins; cleanup/removal is importer/editor scope later. One NotifyEdited causes one action refresh and one affected loaded-node reconstruction pass.

### 10.2 Send Request and simple actions

UK2Node_VaRestSendRequest requires no asset. Client optional -> Hub default, Verb, URL/Path, Query, ordered Headers, wildcard Body (struct/Json/String/bytes), ResponseType picker (default Json), Tag, explicit AuthMode=Required. QueueWhenOffline is offered only with a real activated named registration and still runs native eligibility/verb/origin/commit checks. Outputs include the same typed availability behavior. Runtime response type cannot change a compiled output pin through an arbitrarily linked descriptor; the picker is compile-time node configuration, with compiler validation like GetDataTableRow. Document the chosen static picker semantics.

GetJson/Download/Upload simple async factories require no custom node and expose plain Json/native metadata/progress. They reuse the frame-completion/owner path and real server/temp-file behavior.

### 10.3 Menu registry and external-module link proof

Call Endpoint menu path: VaRest -> Api -> Category split by "|" -> Name. FVaRestApiEditorBridge::OnEdited triggers actual RefreshAssetActions once and reconstructs loaded affected nodes once.

Export FVaRestAssetEntryActionRegistry and IVaRestAssetEntryActionProvider from VaRestEditor public header, with module API macros and direct dependencies. Provider declares protocol tag and returns spawners for an Api/abstract socket entry. Register/Unregister own stable provider handles; module shutdown unregisters without deleting another protocol. Each external protocol owns its concrete K2 identity {Asset, Entry, Channel, Route}, validation, channels and extra pins. Missing provider leaves HTTP actions intact.

A separate opt-in editor test module VaRestM5ExtensionTest (Tools/m5-smoke, excluded from Fab/product) depends on PUBLIC interfaces only and implements a fake protocol config/provider/spawner with channel-specific pins. It must link/run register/unregister tests with no private Http/Editor headers or WS link. Same-module subclass compilation is insufficient export proof. Actual W5/Socket.IO nodes are not implemented here.

### 10.4 VaRestEditorTools and import hooks

New Editor-only module owns UVaRestApiFactory, FVaRestApiAssetTypeActions, category/color/icon/default editing, and a named ToolMenus extension point for later OpenAPI reimport/report/cleanup actions. Core validation stays UVaRestApi::IsDataValid; later importer uses normal UEditorValidatorBase rather than a gratuitous core validator hook.

Actual module needs: Core/CoreUObject/Engine, VaRestJson/Http, UnrealEd, AssetTools, ToolMenus, Slate/SlateCore, PropertyEditor, Kismet/BlueprintGraph and the exported VaRestEditor contract as used. Register/unregister actual actions/customizations/menu handles in Startup/Shutdown. Runtime game builds must not link/load this module or the extension-test module.

Manual UI proof uses a real editor window where required: Create New asset, inline auth instances, visible Exponential(3), settings paths/profile, paused/activation affordance, environments/endpoint/type picker, categories/deprecation/orphan presentation and batch refresh. -nullrhi construction tests do not substitute for that UI evidence. No importer/YAML/reimport/commandlet implementation is pulled into M5.

## 11. Ordered implementation tasks and intermediate build contracts

These are implementation-plan work packages, not a second task tracker; Beads remains durable tracking. Execution is sequential under Root's sole-source/index/native lease. Each task has a compiling tested intermediate state; deferred integration is named explicitly, never a pretend working stub. Reused files pass ownership to the next task only after review. Every edited C++/native version follows section 12's fresh graph permit and separate --no-build runtime rule.

Each critical oracle uses a discriminating restore-the-bug mutant, exact restoration and a rerun. Do not manufacture a crash to show a missing pin. A counter only proves a real ownership invariant if its mutation alters the actual required path; redundant instrumentation is not qualification.

### Task 1 — JSON rules, default resolution and finite logical conversion/parser controls

Own existing:
Source/VaRestJson/Public/VaRestJsonConvert.h, VaRestJson.h;
Private/VaRestJsonConvertPrivate.h, VaRestJsonConvertNames.cpp, VaRestJsonConvertEncode.cpp, VaRestJsonConvertDecode.cpp, VaRestJsonBuilder.h, VaRestJsonReader.cpp and VaRestJson.cpp only where parse options are wired;
Private/VaRestJsonDocument.{h,cpp}/VaRestJsonArena.{h,cpp} only for actual requested-ownership observations or work checks used by the declared bound.
New: Public/VaRestJsonRules.h; Private/VaRestJsonRules.cpp, VaRestJsonBudget.{h,cpp}; Private/Tests/VaRestJsonRulesTest.cpp and VaRestJsonBudgetTest.cpp. Existing converter test fixtures may extend as needed.

Implement section 2.1 entirely without Http/editor references. Introduce a conversion context that counts semantic visits/requested bytes/work before controlled operations; propagate it through nested struct/container/enum/Optional/special Json paths. Parser counts root/container/scalar nodes and keys before adding the counted item; charge fixed input-byte traversal plus declared SAX work, state excluded hash/native/allocator costs. Zero options preserve existing code-first behavior.

Oracles: default-options existing BP/native calls use preloaded-like global registration without passing Rules; explicit owner override, conflicting versus equal owners, independent unregister; GUID rename/recompile and fallback eligibility; every Presence/nullable/base64/unset array/map rule; N4 warning scope; two-owner cache separation; default zero options unchanged; scalar arrays/tiny objects/base64/nested Json/logical-limit failures and exact release. Capture requested DOM/scratch counts for the zero-array counterexample; never report estimate as peak bound. Mutants remove default registry route, select last writer, use FName instead of exact wire or omit one controlled pre-growth check. Full VaRest.Json.* after guarded build.

### Task 2 — Compiling runtime reflected foundations and module dependencies

Own new Http public/private pairs:
VaRestApiTypes, VaRestApiSocketConfig, VaRestAuthConfig (abstract interface only), VaRestClientCredentials (config declarations only), VaRestHttpBlueprintTypes, VaRestApiEditorBridge.
Own Source/VaRestHttp/VaRestHttp.Build.cs for direct public Engine and existing required modules.
New Tests/VaRestApiTypesTest.cpp and VaRestBlueprintMetadataTest.cpp.

Define actual USTRUCT/UENUM/UCLASS exports/property metadata, new EVaRestEndpointBodyKind mapping, all descriptor/param/environment/validation types, lossless decimal-ID adapters, reflected response/queue/header/query/session/auth/multipart data and real checked native conversion helpers. The abstract auth CreateAuth/Validate interface is complete for later UHT references; no builtin config implementation is claimed yet. VaRestApiEditorBridge is a native exported runtime delegate boundary with no UnrealEd dependency.

Oracles: UHT-valid numeric fields, high uint64/uint32 decimal round trips including 0/max and invalid encodings; FGuid/QueueId unchanged; both native body-kind and new endpoint-kind headers compile together; all mappings/nested-descriptor invalid cases/date-time; multipart exclusive sources and file validation; raw 00/01/ff plus repeated Set-Cookie/list headers and error body map exactly. Removing one repeated line or narrowing an ID fails. Runtime module dependency scan has no WS/editor link.

### Task 3 — API asset, settings, startup preload and ModifyCook

Own new Source/VaRestHttp/Public/VaRestApi.h and Private/VaRestApi.cpp; Public/Private VaRestEngineSubsystem (LOADER ONLY);
existing VaRestHttpSettings.{h,cpp}, Private/VaRestHttpModule.cpp;
new Tests/VaRestApiValidationTest.cpp and VaRestHttpSettingsTest.cpp; adapt paths under Source/VaRestHttp/Private/Tests consistently.

Implement section 2.4, referring only to Task 2's complete reflected interfaces. Asset constructor retry/JSON defaults, type/header/refresh/socket validation, editor-only import storage/tag, one-batch rules, NotifyEdited runtime delegate and BeginDestroy. Settings paths/naming/Apis/dynamic-limit declarations exist; no retry field.

Engine loader retains loaded APIs strongly before GI startup. In an actual cook commandlet, skip synchronous gameplay preload to avoid incidentally loading the settings-only proof package; the registered ModifyCook delegate still supplies settings packages. Other editor/game startup retains N7 behavior. This explicit cook-context distinction uses actual IsRunningCookCommandlet and is verified rather than guessed. No fallback Hub field/construction exists until Task 8 adds it; tests here do not pretend one exists.

Oracles: asset Exponential(3) versus native disabled retry; no Settings.Retry property; exact normalized default roots equal existing Saved/VaRest paths; existing settings names unchanged; source tags/import-state editor round trip; one rule generation/native edit notification per batch; socket validation delegation; preload before ordinary conversion; null/missing warnings; actual three-parameter cook callback appends/deduplicates the right package list. Remove preload/default-rule route and first-use conversion fails. Definitive cooked absence proof is Task 14.

### Task 4 — Core scope/endpoint/options plumbing and backward-readable queue v2

Own existing Source/VaRestHttp/Public/VaRestClient.h, VaRestTrace.h, VaRestTransport.h (comment/interface semantics only if needed);
Private/VaRestClient.cpp, VaRestClientState.{h,cpp}, VaRestExchangeState.h, VaRestRequestSpec.h as used, VaRestPipeline.cpp, VaRestTrace.{h,cpp}/VaRestTracePoints.{h,cpp}, VaRestOfflineInternal.h/VaRestOffline.cpp, VaRestQueueStore.{h,cpp}, VaRestUrl.{h,cpp} for checked ParamValues;
new Tests/VaRestTraceM5Test.cpp and targeted existing QueueStore/Replay/Offline/URL tests.

Implement section 3. Scope/name initial config is set in a private test/future managed initializer before Create; Task 5 adds managed admission ownership to that initializer. No public scope-move setter. Add WithStrictDecode optional builder resolution and rules/media options. Add exact endpoint propagation, new path-array element encoding, queue v2 with endpoint guard/budgets and v1 numeric-kind preservation. Do not implement retirement piecemeal here; section 5 is Task 5.

Oracles: first Client Created under capture has correct nonzero scope/display; complete event/snapshot/marker/generation behavior; native asset-like client strict decode works; v1 byte-pinned kinds/endpoint-empty and v2 exact keys survive replay; vendor JSON/body/request-header precedence; path/query delimiter and exact wire case including reserved data; process scope0/default no-retry unchanged. Mutants revert scope/endpoint assignment, enum cast, default strict option or body media use. No Inspector source edits.

### Task 5 — Managed native admission, paused epochs and retirement construction

Own the exact section 5 native construction file set, including client/spec/state/exchange/pipeline, queue/persistence/session/refresh integration and new native control/lease/epoch/ticket files named there. Shared files are sequenced after Task 4. Tests use owned barriers/counters, never a global default reachability reset.

Implement the full transactional managed factory and operation-token accounting, gate seals, pre-entry admissions, paused activation, fresh registration epochs, FIFO/physical replay and accepted terminal/commit cutoffs, bounded ticket/bookkeeping admission, internal settlement independent of user callbacks, quiescent namespace fence and lease-safe control shutdown/off-thread discard. Public code-first behavior changes only through the expressly managed opt-in seams.

No partial “CancelAll then public Flush callback” implementation can pass this task. Add a concrete restore-mutant per gate/epoch/tail/control invariant. Required cases include early logical completion with an outstanding physical callback, any-thread Send/posted Enter, old builder, held guard/dispatch tail, Committing rename, ordinary/silent replay interruption, another compatible live member, reentrant Completed-sink retirement, reused ClientId with a new epoch, same-path takeover with old snapshots, false/suppressed Flush and off-thread late lease drop. Native count/bytes/bookkeeping admit/refuse/coalesce under pressure and drain to baseline.

Task 5 cannot start without the integrated final Root-supplied construction and cannot pass without G-RETIREMENT. Actual signatures/files/oracles are section 5's authority within this integrated draft.

### Task 6 — Dynamic native builder, admitted GT ownership and preterminal frame seam

Own new Http Public/VaRestDynamicRequest.h and Private/VaRestDynamicRequest.cpp, VaRestDynamicResult.{h,cpp}, VaRestDynamicBridge.{h,cpp}, VaRestDynamicBudget.{h,cpp}, VaRestFrameDispatcher.{h,cpp};
existing VaRestResultHandler.h, VaRestClient.h/.cpp, VaRestClientState/ExchangeState and VaRestPipeline.cpp, VaRestDecode.cpp/.h plus HttpTesting hooks.
New Tests/VaRestDynamicResultTest.cpp and VaRestDynamicBudgetTest.cpp.

Implement section 7 using native test consumers, with no future RequestAction UCLASS dependency. Reserve admitted states/frame slots before worker launch. Public shell only owns immutable/plain data. Consume Task 5's default-noop Finalize, add the constrained LaterFrame/delivery policy queries and dynamic override; compile-time handlers remain unchanged. Recheck owner/cancel/managed gate after Finalize before terminal selection. Pending native ownership remains through required internal/physical tails from Task 5.

Apply dynamic MaxInputBytes through the accepted transport MaxBodyBytes/body sink for ALL in-memory result kinds; use the stricter nonzero effective JSON/dynamic limit, while SaveTo remains streaming. Synthetic/custom/cache results are checked before pending bridge retention/parse. A transport/custom stage's already constructed buffer is an excluded upstream allocation, not evidence of a universal ingress heap cap. Oversized HTTP error retains the primary Http result with unavailable body according to accepted cap semantics. Text/Bytes cannot bypass the dynamic input policy.

Oracles: real later frame/cancel/owner/queued/silent ordering; Finalize failure/stale/result type/warnings/timing in Completed; post-Finalize cancellation/gate closure from native lifecycle code; no OnHttpThread dynamic; no private-spec/handler replacement surface; zero editor worker reflection calls; admitted-slot/input/logical work/output refusal/release; arbitrary retained shell last release on worker performs no reflected destruction/GT lookup. Refused notification costs are separately measured/documented, not counted as admitted-state proof.

### Task 7 — Editor pre-change protection and cooked native-operation qualification

Own new Source/VaRestEditor/Private/VaRestDynamicEditorHooks.{h,cpp} and Tests/VaRestDynamicRecompileTest.cpp;
existing VaRestJsonStructListener.{h,cpp}, VaRestEditor.cpp and VaRestEditor.Build.cs;
Http DynamicBridge/DynamicResult after Task 6 plus new VaRestDynamicClosure.{h,cpp} and Tests/VaRestDynamicClosureTest.cpp.
Existing VaRestTests struct fixtures may be reused without putting packaged types in VaRestTests.

Implement section 8 completely. Actual checked UTransBuffer delegates and absent-buffer refusal, struct/enum/undo/package/Blueprint target pre-change invalidation and strict later-frame settling; owner/default-rule-only changes covered. Pre-change resets all GT native carriers before layout destruction. Hook removal disables conversion first. No RequestAction getter integration yet; Task 10 owns it after the class exists.

Implement qualified closure traversal including Optional and native lifecycle classification. GT fallback is the default cooked path. Worker mode remains disabled for unqualified operations/ownership/control shutdown, or refuses explicitly under selected caller policy. Pinning alone is not a worker qualification test.

Oracles: held native carrier before UDS dependent/bag rewrite, enum changes, undo before deserialization, package reload before fixup, absent/replaced transactor, rule-only and target BP precompile, reentrant native callbacks; all known generations and GT reset threads. Safe/nonqualified native constructor/copy/destructor fixtures and Optional dependencies qualify G-COOKED-CLOSURE in simulation; actual packaged proof remains Task 14. No crash-only pin mutant.

### Task 8 — Public Hub facade/hosts, wrapper fundamentals and paused durable UX

Own new Http Public/Private VaRestHub, VaRestGameInstanceSubsystem, VaRestHubRuntime, VaRestApiClient;
existing VaRestEngineSubsystem.{h,cpp} to ADD fallback Hub ownership;
native managed factory integration only through Task 5's defined seam;
new Tests/VaRestHubTest.cpp and VaRestHubQueueTest.cpp.

Implement section 4 using UObject facade and real subsystem hosts. Complete initial scope/name/config/admission before Created. Asset clients/ordinary MakeClient/default client, independent connectivity, storage profiles, paused state, explicit activation/readback, account/environment switch, events and native C++/W5 handle lookup. Native control survives UObject deinit without retaining it. Rule subscriptions refresh later client snapshots while old operations remain anchored.

Only wrapper methods whose types/implementations exist are introduced now. Builtin config classes/setter extension is Task 9; BP Flush/Request actions are Task 10. No unimplemented UFUNCTION returning a future class is added. Test no-auth/native fake config and accepted native bearer provider where needed; real BP custom refresh RequestJson wiring waits for Task 10.

Oracles: engine-only fallback with no GI, two real GI instances, no fake GI; first trace Created and closed-scope snapshot semantics; PIE transient partition versus explicit stable restart profile, required distinct non-PIE profiles and config conflicts; C1 delayed session restoration/activation plus return to retained account; active-only old files, exact bodies/keys/origin and current readback; native retirement after facade destruction, repeated switches/ticket pressure and deinit scope closure. Removing explicit activation or per-Hub connectivity must fail.

### Task 9 — Builtin auth configs, Custom bridge and separate ClientCredentials

Own new/extended Http Public/Private VaRestAuthConfig, VaRestClientCredentials;
existing VaRestAuth.h AND VaRestAuth.cpp for the real ClientCredentials factory AND native refresh RequestDynamic sibling;
VaRestAuthTypes.h native TraceEndpoint only, VaRestBearerRefresh.cpp trace plumbing, VaRestApiClient.h/.cpp for supported credential setters;
new Tests/VaRestAuthConfigTest.cpp and VaRestClientCredentialsTest.cpp;
Tools/testserver/server.py and selftest.py for strict OAuth fixtures only. Private key is filename-only, never inspected.

Implement section 6. Config duplication and pure identity queries; endpoint POST path+trace-only resolution; validated repeated Apply results; three-outcome/stale guarded FinishRefresh; no live-auth watchdog; trusted import/SetSession; separate RFC client-credentials provider with actual default expires_in and complete redaction/persistence forms.

UVaRestAuthRefreshContext exists with native value/GetSession/GetRefreshId now; the reflected RequestJson returning an action is intentionally introduced only by Task 10 when that UCLASS is complete. Task 9 tests native Context.RequestDynamic bypass; it does not claim the future BP action integration already passed.

Oracles: held/dropped/duplicate Done, per-client config instances, stable versus replaced identity, default/custom OAuth fields and proactive/reactive refresh, Basic/Form exact fixture negatives, scope pause on real credential changes, refresh URL/template validation, malformed token-response ImportSession leaves state untouched, no refresh-token clientCredentials requirement. Existing Python suites stay green; mutation removing snake-case expiry must fail proactive refresh.

### Task 10 — Request/flush actions, reflected event wiring and G-BP-TRANSFER

Own new/extended Http Public/Private VaRestRequestAction, VaRestRequestHandle, VaRestFlushQueueAction, VaRestRequestLibrary, VaRestHttpLibrary;
VaRestApiClient.h/.cpp event/action integration; AuthConfig refresh-context RequestJson declaration/implementation;
DynamicBridge/FrameDispatcher integration after Task 7;
new Source/VaRestEditor/Private/VaRestRequestNodeExpansion.{h,cpp} and Tests/VaRestDynamicTransferConstructionTest.cpp, VaRestRequestActionTest.cpp, VaRestRequestNodeTestUtils.{h,cpp};
VaRestEditor.Build.cs direct used compiler dependencies.
Blueprint metadata/conversion foundations from Task 2 are reused, not redefined.

Implement section 9. G-BP-TRANSFER is the first task component and uses the SAME shared expansion helper the full nodes later consume, not a separate imitation. Inspect actual compiled generated functions/storage and prove the preparation cookie/control/availability route. Stop this task and replan if qualification fails. No full Call Endpoint/Send Request feature is claimed yet.

Then wire actual action/handle/metadata queue events/Flush false/accepted/suppressed behavior, simple JSON/file actions, error escaping and BP auth context's prepared builder. Frame policy never postpones Send to capture BP initiator later.

Oracles: all section 9.3 cases and counters; post-cutoff primary Success plus explicit unavailable, never rewritten Config/default Success; owner/Hub silence; retained shell worker release; repeated raw headers/bytes/current registrations; accepted/refused Flush; real GetJson/Download/Upload temp/progress; Context.RequestJson bypass. IN15 real BP stack captured at Send. Same-frame mutation and a retained native carrier mutant fail.

### Task 11 — Complete Call Endpoint / Send Request K2 implementation

Own new Source/VaRestEditor/Private/K2Node_VaRestCallEndpoint.{h,cpp}, K2Node_VaRestSendRequest.{h,cpp}, VaRestTypeRefDetails.{h,cpp};
shared RequestNodeExpansion from Task 10 and VaRestEditor.Build.cs;
new Tests/VaRestRequestNodeTest.cpp and VaRestTypeRefDetailsTest.cpp.

Require accepted G-BP-TRANSFER. Implement all section 10.1/10.2 pins, exact descriptor mapping, real validated action expansion, immediate Send, preparation/continuation/availability, typed error, defaults/Body Json, all BodyKinds, static picker, deprecated/missing/orphan reconstruction and batch refresh.

Oracles: code-built BP compiles/runs struct/scalar/arrays/maps/enum/date-time/text/bytes/no-content and typed error/unavailable; exact sort/Sort wire names with PinName separation; params by location/styles/reserved values/optional linked default/removed behavior; false/zero/null Body Json; form repeats/multipart file/binary/media; renamed endpoint GUID, type-change incompatible links, saved orphan pins; no blank/stub expansion for supported kinds. Mutants send PinName, ignore link presence, drop Body Json or output cookie/availability checks.

### Task 12 — Asset tooling, protocol registry and external consumer module

Own VaRest.uplugin;
new Source/VaRestEditorTools module Build.cs/public module header/private module, asset actions/factory/menus;
new Source/VaRestEditor/Public/VaRestAssetEntryActionRegistry.h and private implementation;
CallEndpoint spawner integration only;
new Tests/VaRestApiAssetToolTest.cpp;
new opt-in Tools/m5-smoke editor extension-test module descriptor/build/source files, excluded from Fab/product.

Implement section 10.3/10.4 with exported API/macros/dependency correctness. Test protocol provider is built in the OTHER module and consumes only public declarations. W5 owns actual protocols; no WS includes/links are introduced into Http or the generic registry.

Oracles: external link/load/register/unregister/channel pins; HTTP actions survive provider absence; Create New/category/color/icon/default editor; validation and one-batch menu/node refresh; runtime target dependency closure excludes EditorTools/extension modules. Complete the real-editor manual UI checklist after separately permitted launch.

### Task 13 — Real server/API/BP integration and lifecycle regression

Own new Source/VaRestTests/Private/VaRestApiIntegrationTest.cpp and VaRestTests.Build.cs;
Tools/testserver server/selftest fixture additions only not already owned/landed by Task 9;
shared IntegrationTest registration only where actually needed.

Reuse /echo, /status, /refresh, /headers/set and cache/offline fixtures. Add typed error route only if existing status route cannot assert schema. Test actual nodes against HTTP, not only fake builder fields.

Oracles: endpoint/send JSON struct/array/scalar, text/binary/error/OnQueued/cache/replay; exact repeated response headers and bytes, idempotency key, all selected media; global/per-asset/Shipping/runtime environment selection; delayed trusted session restore and explicit activation across retained accounts; current count/subscriptions/no hidden old-domain replay; fake/real managed physical-tail teardown with another member. Every intentional Connection/activity-timeout test injects its own connectivity. Owned barriers/counters reach logical terminal before full drain; never reset process default reachability to hide interactions. Lifecycle suite repeats twice; new fixture Python selftests and existing suites run unchanged.

### Task 14 — Isolated settings-only cook and real packaged BP/closure proof

Own opt-in Tools/m5-smoke/VaRestM5Smoke.uplugin;
runtime Source/VaRestM5Smoke Build/module/native startup/smoke entry files;
Config/DefaultVaRestM5Smoke.ini;
Content SM5_Api / SM5_Runner / response/enum/nested assets and SEPARATE SM5_SettingsOnlyApi/settings-only types, authored by editor automation;
an explicit fixture runbook if necessary. Fixture runtime types live here, never VaRestTests. Host descriptor/build/cook commands need separate Root permits; the fixture is excluded from normal/Fab Shipping profiles.

Two independent proofs:
A. BP runner has real Call Endpoint/Send Request nodes referencing SM5_Api; compare exact wire/results and all editor/cooked rule semantics.
B. Settings-only package is referenced solely by Settings.Apis soft path/ModifyCook, not by BP/map/native hard reference, broad CookDir/always-cook, constructor loader or a startup preload during the cook commandlet. Native early smoke uses FindObject/no-load lookup after engine preload in the RUNNING packaged game, before any Call Endpoint/explicit API load; it verifies standalone default-options conversion rules.

Inspect actual asset/cook dependency manifests; if another route still includes B, the negative fixture is invalid and must be repaired, not declared passed. Mutant removing ModifyCook must omit B and fail the early proof; mutant removing normal startup preload must fail the no-load first conversion. Never use an explicit LoadObject to make that test succeed.

After separately inspected/approved cook/package and target graphs: author/save only fixture packages; Linux Development package/runtime against real server; separately authorized monolithic Development/Shipping builds/runs; compare editor/cooked exact renamed/sanitized keys/presence/unset/prettified enums/scalar-array-map/date-time. Assert no editor module/hook dependency and actual closure/native-op mode. Worker mode only if G-COOKED-CLOSURE passes actual cooked operations/release/control; otherwise record GT mode/refusal explicitly. Pin-removal or missing-rule mutants must fail deterministic activation/qualification, not crash.

Record commands/target/config/graph-output closure/cook manifests/server URLs/startup preload ordering/current libraries/native or UBA reuse. Linux does not prove Win64/MSVC, Mac or mobile. O4's imported fixture remains a downstream M7 gate rather than pretending M5 implements import.

### Task 15 — Whole M5 native/runtime/NoPCH/resource/UX qualification

Own native evidence only in the approved M5 workspace plus Tools/run_tests.sh solely if a new explicit opt-in smoke path is required; normal defaults remain unchanged. No source tuning without a new edited-version graph/review.

Run all approved normal/focused/full automation/Python/integration/mutation gates, actual UI and Task 14 smoke. Measure separate GT initialization/conversion/preparation copy/reset cost, parser estimates versus requested ownership and logical budget refusal/cleanup under hostile shapes. Report unproved defaults/native/allocator/caller peak effects explicitly; do not invent RSS/latency claims.

HTTP NoPCH/nonunity uses ONLY VaRestHttp.Build.cs and VaRestTests.Build.cs temporary switches, exact initial bytes/hash capture and unconditional exact restore; fresh graph inspect every action/response/unity/link/output closure. Stop on engine/host/PCH actions without exact new permit. Additional Json/Editor/EditorTools NoPCH needs a separate explicit Root permit; an unexecuted link gate remains open. If explicitly approved compile-only replay fallback is used, save response files and redirect only plugin .o/.d outputs, hash normal libraries, restore normally and run module smoke; label standalone-TU proof, not NoPCH linking.

Normal restore uses a fresh graph/build within its allowed closure, then --no-build smoke. Linux VaRestHost Development/Shipping permits allow only plugin compiles plus named final executable link and its products/receipt, not engine rebuild. Read actual run logs, retain startup CVar0 evidence/known warning noise and record unexecuted gates.

### Task 16 — Final documentation/freeze and independent whole-review handoff

Own AGENTS.md/CLAUDE.md mirrored substantive freeze instructions;
docs/dev/cache-offline.md (managed M5 naming/activation/retirement additions, accepted code-first limits unchanged);
new docs/dev/assets-blueprints.md;
docs/dev/http-thread-completion.md for editor/cooked/frame-terminal exception and availability semantics;
precise core/OpenAPI/WS/Inspector future-contract reconciliation for Hub UObject/hosts, native-vs-endpoint enum, exact wire/PinName, DateTime, multipart, rule route, dynamic editor/cooked/typed availability and provider registry placement;
approved final review evidence, no unrelated draft edits.

Freeze actual accepted declarations/config keys after implementation/review, not a guessed list. Include all new ID representations, activation state/result enums, typed-output availability, native lifecycle APIs and reflected types, preserving frozen native enum/settings spellings. Document no hidden queue replay before readiness, trusted session continuity, active-only retention, profiles/PIE transient behavior, code-first behavior, native shell/GT Take, post-cutoff availability and explicit bound exclusions.

Independent task reviews, one whole-M5 review and one scoped fix wave as required; fresh graph/tests for fixes. Close/update Beads only under active Root/user authority after actual gates. Commit/merge/index/native/source leases follow the current instructions; this draft authoring grants none. Push/tag/Fab remain owner actions. O3/W5/M7 dependencies are named; no downstream milestone is closed by this document.

Handoff records files, accepted baseline/reconciliation, every qualification gate, tests/mutants/restoration, native/UBA reuse and platform limits, issue status and blocked actions. M5 exit is real BP end-to-end plus required N1–N16/lifecycle/native/cook proof, not merely compilation or a coverage table.


## 12. Complete verification contract and requirement map

This section specifies future evidence, not commands executed by the plan author. Compilation does not substitute for UI/cook/control-flow proof; a requirement map does not substitute for a passing oracle. The original review remains evidence and this author is not its replacement independent reviewer.

### 12.1 Per-version graph, runtime and artifact discipline

Before each edited C++ version and each target, generate a FRESH full -WriteOutdatedActions graph under the active Root native permit. Inspect every action/command, recursive response file, unity member, PCH input, link input, output and receipt/product closure before compiling. Editor/NoPCH permits plugin Compile/Link and explicitly permitted metadata only; engine/host Compile/Link/PCH creation stops execution for Root review. -Module, -UsePrecompiled, global unity flags and old graph/recovery evidence are not waivers. Do not alter engine generated files, timestamps, caches or objects to force freshness. Record native/UBA reuse honestly.

Build and runtime/server/cook remain sequential with separately guarded --no-build automation. Read actual log artifacts, test counts and controlled counters rather than relying on stdout or a timeout's absence. Use per-client/test connectivity for deliberate Connection/activity-timeout failures. Do not reset process-default reachability. Release held logical operations before full draining; use owned barriers/counters/quiescence. Existing startup warnings remain recorded. The runner's HomeScreen.EnableHomeScreen=0 ForceDPCVars isolation needs actual startup CVar0 plus the UE source gate; it is not a measurement of all engine network traffic.

Task 15's HTTP NoPCH exact-byte save/restore and fresh normal restore graph are mandatory on every exit. A separately approved compile-only fallback verifies standalone translation units, not NoPCH linking/runtime. Extra module NoPCH, cook/package/host descriptors and final monolithic Development/Shipping links need their explicit permits. Authorized VaRestHost Linux Development/Shipping allows plugin compiles plus the named final executable link/products/receipt, not a general engine build. Linux proof does not prove other release platforms.

For each controlled mutation: save exact starting bytes/hash, apply one actual invariant-breaking change, generate/review a fresh graph if C++ changed, build within permit, run the discriminating oracle, observe its specific failure, restore exact bytes, regenerate/review/build as required, and rerun the restored oracle. Do not leave mutants, generated binary edits or temporary build switches. Byte-preservation checks cover normal libraries where compile-only replay redirects .o/.d. No test-server credential key content is required or permitted.

### 12.2 Native lifecycle controlled schedules and discriminating mutants

Task 5 owns these native schedules; Tasks 6/8/9/10/13–15 add their integration surfaces. Every held resource has an owned counter/latch and an explicit release. A permanently held third-party transport truthfully occupies a bounded slot; no timeout fakes physical settlement.

| Test / controlled schedule | Required oracle | Mutant that must fail |
| --- | --- | --- |
| Any-thread Send pauses at admission while teardown closes | CAS winner is either counted pre-close work or no-exchange async Config; Scope Closed cannot race a late increment | Split open check and counter increment |
| Pre-close Send parked before GT Enter | Retirement count remains nonzero; Enter sends no HTTP after close and releases once | Release lease at Post/Enter instead of terminal, or omit GT gate check |
| Ordinary managed non-replay request completes logically while an old/replaced transport/GT handback is held | Gate count and native scope remain open until logical/delivery AND every dispatch tail settle; no post-close scope event | Attach dispatch accounting only to replay, or release all-operation lease at Completed |
| Ordinary managed OnHttpThread callback held after Completed | Its logical delivery tail keeps the operation counted until callback/handler return; teardown retains no strong Hub | Ignore off-thread delivery tail or count out before callback launch |
| Old builder/native handle used after permanent teardown | No exchange/Started/HTTP/queue join/refresh; asynchronous Config with valid owner suppression | Gate only the BP wrapper |
| Reentrant Started/trace listener closes Hub | No physical Start after closure; counted request settles in original scope | Check gate only at initial Send |
| A-to-B queue switch with ordinary A HTTP active | Ordinary request keeps A snapshot; late opted-in enqueue through detached A refuses; new send uses B | Redirect late enqueue into current B or CancelAll all ordinary HTTP |
| Hundreds of alternating switch calls while A cleanup held | At most one active switch plus two inline upgrade drains; distinct retries fail LifecycleBusy, no histories | Append RetiredMembers/pending callbacks per setter |
| Teardown upgrades held A-to-B switch | Native gate closes and current B detaches using second reserved record; no allocation/admission failure | Allocate teardown ticket only at Deinitialize |
| A owns active replay, compatible B survives; cancel handback/physical release held | Head remains, no B duplicate dispatch until A tails settle, then B resumes with fresh ReplayId | Clear bReplayActive immediately on detach |
| Replay success finalizes, Completed trace sink synchronously retires A, compatible B survives | Before sink runs, terminal result is sealed; A gets no event, success still retires once, B gets normal accepted result; no successor starts before physical/GT tail settles | Publish terminal only in post-trace user callback, or let detach overwrite sealed success with retain |
| Custom stage publishes terminal while old/replaced physical callback is held | Logical Completed alone does not release active Op; every dispatch/guard tail is counted, final zero permits the sealed disposition exactly once | Track only current TransportCall or decrement on Cancel/guard posting |
| EndWaits invokes a stage that synchronously retires its replay owner | Terminal-acceptance marker prevents detach from overwriting the already accepting result before its decorated publication | Add only a post-EndWaits/Completed hook and infer all earlier detach is cancel-retain |
| Typed Finalize/default/copy code reenters Cancel/Hub teardown; rejected-value destructor reenters again | No terminal marker before Finalize; cleanup precedes final owner/cancel/gate sample; Completed/result remain canceled/suppressed with correct warnings/type | Freeze terminal before Finalize, or perform only a pre-Finalize cancellation check |
| Managed stage tries a 33rd overlapping physical dispatch | No excess transport Start; fixed outstanding count and async normal Config result path; sequential retries still reuse the 32 permits | Grow per-dispatch cleanup history or overflow the settlement counter |
| Silent replay/health completion and absent user handler | Internal operation/probe/fence settles once; no user event | Settle only in Handler::Deliver/OnComplete |
| Replay Retire IO accepted, then A leaves with B still live | Actual Retire updates matching item/charges and B gets its normal result; A gets none | Reject accepted IO callback by inactive member generation |
| Last member departs during accepted Retire IO | Report RetireAcceptedIO actual result, no false retained-head claim, no next replay | Revert accepted tombstone or drop its accounting callback |
| Old probe token A finishes after newer token B starts | Old completion does not clear B running state; each token releases once | Use no-argument ProbeFinished for owned probes |
| Probe candidate B selected replay owner A; unregister B | Does not cancel/finish A's still-owned operation merely because candidate was B | Treat candidate ClientId as operation owner |
| A detaches while B owns active replay/backoff | B operation, retry cadence and result subscription survive | Invalidate queue-wide replay generation/timer for every departure |
| Queue admission held before and after commit selection | Before cutoff cleanup is awaited; after cutoff actual rename result survives retirement | Refuse/drop Committing work because binding closed |
| Public Flush accepted then wrapper/weak state gone | Native cleanup owner settles despite suppressed user callback; no strong Hub/FHubRuntime retained | Decrement PendingFlushes only in user callback |
| Final Flush immediate false / worker barrier refusal | No retained callback expected; explicit NotAdmitted/Failed, no fake success, no retry growth | Increment pending on false or treat refusal as Flushed |
| Load/Prepare/Abort/Retire callback discarded at global stop | Native settlement witness releases its fixed row/accounting with correct suppressed result | Depend on ordinary callback invocation |
| Same-path new Hub waits while old IO held | QueueBusy before fence; new different-connectivity coordinator only after actual quiescence | Remove registry entry on member count or handle drop alone |
| External old ConfigAtBuild/config/queue survives reopening | Old shell inert; no late IO/flush; new coordinator unaffected by stale exact-pointer removal | Skip Fenced check or erase registry by path alone |
| Legacy any-thread Send pauses around A config snapshot while its last member switches | Snapshot plus capture admission linearize with replacement; admitted legacy A work is counted and stays valid, later bare old references cannot acquire work | Read A then separately increment a capture counter after registry fencing |
| Shared B keeps enqueueing while A drains | A member fence completes from selected local watermark; B continues | Require whole-coordinator idle for every member drain |
| External RefreshAuth overflow and held BP Finish | Waiter storage bounded, teardown drops owned subscriptions, late Done ignored, no watchdog on live auth | Append unbounded waiters or let Done strongly retain Hub |
| Implicit GetClient followed by delayed trusted import | Queue remains Paused and retained head untouched until explicit activation | Autojoin with empty Bearer at construction |
| Explicit wrong-identity activation | Existing Auth retirement remains observable and no old operation is relabeled | Suppress native identity mismatch to preserve files |
| Slot reclaimed then a stale native token calls back | Generation mismatch is ignored; no mutation of reused client row or double release | Address row by index only |
| Off-GT token settles after module object/ticker shutdown | Captured native Control remains valid; only atomic settlement, no module/static lookup, GT task, user callback or off-GT anchor destruction | Capture raw module/service pointer or let token destruction free the last GT-affine anchor |
| Normal pre-exit with a permanently held transport/posted task | Admission closes; ticker/hooks removed once; unresolved existing rows use one fixed exit quarantine, no fabricated Flush/Scope Closed or global wait | Drop Control at ShutdownModule, append exit records per callback, or time-force settlement |
| All native work quiescent before ShutdownModule | GT releases anchors/cells and removes ticker; no exit self-hold and no further callback can execute | Install unconditional process-lifetime retention for every normal shutdown |
| Two PIE scopes / editor fallback / preload only | Independent Hub connectivity/identity; preload starts no queue replay | Use engine startup loader as shared runtime Hub |

Additional Root-directed atomic-cell schedules: pause a sender after client gate acquisition, after Free->Claimed and before release publication, then race GT close/scan/stop. No GT payload read/reclaim occurs before Published; client/scope cannot close around a counted unpublished admission. Hold one consumed stale token while the cell recycles; its generation cannot mutate the next row. Mutants replacing CAS with a plain Free test, publishing Enter before the anchor/ready flag, decrementing client count before owning cleanup, or reusing a generation must fail counters without a crash. Pause a would-be child at its closed check: only the GT no-callout check/increment path or a linearized child gate is legal; the split check/increment mutant must reproduce refused-after-close admission and fail.

Record caps at occupancy: 256 process/64 per Hub managed clients, 1024 operations/client, 32 physical dispatches and 64 logical tails per operation, two inline drains, one unsettled switch, 64 external refresh waiters and one BP Finish. Capacity refusal preserves ordinary async Config semantics and does not create an extra native record/history; caller-generated refused notification costs remain outside this count. Repeated refused switches cannot change the accepted target or append callbacks. Release resources and show counts/cells/anchors/fences return to baseline under an ordinary live-runtime schedule.

### 12.3 Dynamic bridge, bounds and editor/cook oracles

- Frame/terminal truth: readiness in frame N cannot Finalize/Complete in N. Test Config, success, Decode error, queued acknowledgement, cancellation and owner loss. A schema/rule/target change before acceptance produces Config/DynamicTypeChanged for success; a mutation in Completed leaves accepted outcome unchanged and takes explicit availability/suppression. Reentrant Finalize/default-copy and rejected-value cleanup cancel/retire before the final sample. Completed/OnComplete/action counters agree once with correct ResultType/body/warnings/timing. Mutants moving the marker/Completed ahead of Finalize/cleanup or deferring only BP delivery fail.
- Editor worker instrumentation: no mutable reflection/type/property/enum/asset/action/destination access from new dynamic worker paths. Hold parse result across UDS dependent/bag changes, enum prechange, checked undo/redo before deserialization, package reload before fixup, owner-rule changes and target BP compile. Invalidations reset every tracked carrier on GT before mutation; absent/replaced TransBuffer refuses rather than silently running. Existing compile-time worker tests stay unchanged. A root-only reset or missing owner-generation mutant fails the controlled dependency case.
- G-BP-TRANSFER: Task 10's actual generated UFunctions/property/frame inspection, one preparation consumer, cookie separation, no post-thunk address retention, pre-Completed zero native carrier count, synchronous recompile/reinstance Completed sink, user continuation compile, overlapping node activation and multicast metadata schedules are mandatory. No bIsPersistent or counter-only assertion is substituted for real generated control flow. Native retained shells released off-GT have zero reflected ownership/pins/GT permits. STOP/REPLAN applies before Task 11 if any case is unproved.
- Logical budgets: independent input, aggregate pending input, admitted count, parser node/key and work, converter requested/logical bytes/values/work refusal tests must hit the named check and return Config/DecodeBudget with exact eventual cleanup. Use tiny scalar arrays, tiny nested objects, long keys/strings, base64 expansion, maps/sets, nested FVaRestJson copies and adversarial count overflow. Raise only the fixture-specific parse-value allowance when measuring the million-zero-array lower bound, and report the estimate/requested-ownership discrepancy rather than claiming a physical cap.
- Explicit exclusions: {} with large authored defaults and an array of nested defaults/native copies demonstrate that constructor/default/container/BP-copy physical allocations can exceed logical charge; report which preflight controls actually refuse and which costs remain unproven. Ordinary invalid-request/refused-action notification bursts demonstrate the deliberately narrower admitted-count claim. No universal heap/RSS/CPU/latency claim is made from these tests.
- Cooked: traverse Optional and every nested struct/enum/bag dependency; qualify native initialization/copy/destruction/default/hash/equality and owned shutdown. Unknown/GT-only lifecycle fixtures use explicit GT fallback or Config, never the worker. A missing required rule/pin/qualification entry fails deterministic activation or mode selection. Actual Task 14 package logs record selected mode, first-use ordering, private carrier release and no editor module dependency. Worker mode stays disabled until G-COOKED-CLOSURE passes, including module stop/late-return safety.

### 12.4 Full contract-to-task coverage

N4, N10 and N12 Should items are INCLUDED. All five endpoint body choices, DateTime repair, scalar/array/map response/body/error support and repeated raw metadata are included. This plan supplies extension contracts only; import/reimport/generation/commandlets and W2–W5 transports/clients stay with their owning milestones.

| Contract | Integrated construction and owners | Observable qualification |
| --- | --- | --- |
| Core §5 API asset | §§2/3/6; Tasks 2/3/4/9/12 | Asset auth/retry/timeouts/cache/offline/envelope/naming/concurrency/strict/JSON media/redaction; inline Details; complete endpoint wire/body overrides; no settings retry |
| Core §5.2 Hub | §§3–5; Tasks 4/5/8 | UObject facade with actual GI/engine hosts; independent connectivity/nonzero scopes; lazy same client; lookup/native handle/events; explicit active-only account activation; prepublication scope and no dead UObject retention |
| Core §5 BP HTTP | §§7–10; Tasks 6/7/10/11/13 | Real compiled/executed nodes, exact typed pins/raw metadata, immediate Send initiator, later-frame terminal, progress/cancel/queued/error/availability and streaming/simple actions |
| Core §5 auth/BP extension | §6; Tasks 2/8/9/10/13 | Weak per-client config; Apply ordered validation; identity without Apply; single pending Finish three outcomes; held refresh without watchdog; trusted SetSession/ImportSession distinction |
| Core §5 settings | §2.4; Tasks 3/8/14 | Existing key names retained, Saved-relative roots, naming/Apis/profile/dynamic limits, no retry; startup order and actual isolated cook inclusion |
| N1 field GUID/presence/nullable/base64 | §2.1; Tasks 1/3/6/7/13/14 | GUID survives rename; runtime/default route; all Presence/container/unset/base64 paths; exact wire keys; independent owner generations |
| N2 enum wire/unset | §2.1; Tasks 1/3/7/13/14 | Authored/display rename plus explicit map; unset encode/reverse behavior; enum mutation resets/registration lifetime |
| N3 single-value PropertyToJson/JsonToProperty | §§2.1/7; Tasks 1/6/7/10/11 | Reuse accepted APIs with immutable rules and logical context; scalar/container thunks copy immediately; code-first zero options unchanged |
| N4 missing-rule warning (included) | §2.1; Tasks 1/3/14 | Once/type Non-Shipping warning only under known registered folders; unloaded unknown folder not falsely claimed; Settings validation and first-use proof |
| N5 property-bag scalar/container carrier | §§7–9; Tasks 6/7/10/11/14 | Stable Value descriptor ID, bounded weak cache, scalar/array/map shapes, FString map key; one-shot reset before callbacks |
| N6 asset JsonTypes/TypesFolder lifetime | §§2.1/2.4/8; Tasks 1/3/7/12/14 | Batched registration from PostLoad/NotifyEdited, enum included; default/owner snapshots; independent unregister and deterministic conflict refusal |
| N7 Settings.Apis preload/ModifyCook | §§2.4/11 Task 14; Tasks 3/14 | Normal startup before first standalone conversion; checked actual three-parameter delegate; isolated settings-only package and two independent negative manifests; explicit cook-commandlet preload exception |
| N8 endpoint GUID/category/deprecation | §§2.2/10; Tasks 2/3/11/12/13 | ID-first stable rename, category/menu/picker, deprecation diagnostic, absent endpoint preserves orphan pins/errors |
| N9 params/types/styles/default/removed | §§2.2/10; Tasks 2/4/11/13/14 | Exact FString Name versus PinName; advanced omitted unlinked default only; linked zero/false/empty; removed never sent; element escaping/literal delimiters |
| N10 advanced Body Json (included) | §10; Tasks 11/13/14 | Explicit linked JSON override wins typed body; empty linked JSON handled; non-JSON bodies do not pretend to consume JSON override |
| N11 TypeRef/BodyKinds | §§2.2/7/10; Tasks 2/6/7/10/11/13/14 | New endpoint enum maps unchanged native numeric kinds; Struct/Json/String/Bytes/Bool/Int/Int64/Double/Enum/DateTime plus supported one container; native/media/file semantics |
| N12 typed error (included) | §§7/9/10; Tasks 6/7/10/11/13/14 | ErrorBodyValid and correct typed value when available; primary native error/body/warnings never overwritten by typed error failure; post-cutoff unavailable explicit |
| N13 import storage | §2.4/10; Tasks 2/3/12/14 | Editor-only UAssetImportData/ImportState and tags survive edits/save; excluded runtime/cook payload; no importer implementation |
| N14 base/path joining | §§3/6/10; Tasks 4/9/11/13 | Existing JoinUrl reused; /base + /path concatenates once; no absolute/templated refresh escape; URL-plus-trace refresh semantics |
| N15 auth/redaction/OAuth | §6; Tasks 3/9/10/13 | AuthMode Required/Optional/None; OAuth2Form refresh unchanged; assetRedactedKeys appended; new client_credentials default expires_in, exactBasic/Form request and separate identity |
| N16 one batch edit refresh | §§2.4/8/10; Tasks 3/7/11/12 | One rule publication/NotifyEdited/OnEdited per batch; reconstruct nodes/refresh actions once; no Http UnrealEd dependency |
| M4b full 160-line consumer freeze | §§3–6/9; Tasks 2/4/5/8/9/10/13 | One accepted chain/queue, per-Hub connectivity/scope, explicit account names, full policy bQueueWhenOffline, raw queued results and async Flush immediate-false handling; native mismatch/commit/recovery semantics unchanged |
| Reflected queue/HTTP metadata | §§2.3/6/9; Tasks 2/10/13 | Raw success/error bytes, ordered repeated headers, QueueId/FGuid preserved; all actual unsigned numeric IDs/generations round-trip decimal text without narrowing |
| Inspector §3.5/§9 carries | §§3/7/9; Tasks 4/5/6/8/10/13 | Scope metadata and first Client Created; endpoint ordinary/replay; mask-independent IDs; truthful terminal/final timing; actual initiator captured at Send; accepted whole-I1 prerequisite |
| WS §19 items 5/7/8, §11, SW11 | §§2.2/4/10; Tasks 2/3/8/12 | Abstract instanced exported config validation, SocketBaseUrl, API native lookup/wrapper handle, Hub connectivity/environment/deinit; provider owns channel/route pins/menus; no Http->WS or engine WebSockets dependency |
| Build/cook/release | §§11/12/14; Tasks 14/15/16 | Fresh approved graphs, normal/NoPCH/restore, actual BP/UI, Python/server, monolithic Dev/Ship and isolated package proof; honest platform/UBA/log warnings; independent reviews and owner release actions |

## 13. Complete review-finding resolution matrix

The independent original review's counts are C1/I18/M3. Every row below has a concrete replacement requirement, ordered owner and oracle. “Specified” means repaired at plan level only. Typed transfer and cooked worker have explicit unexecuted qualification gates rather than fabricated resolution evidence. No implementation or independent replacement-plan acceptance is claimed.

| Finding | Exact repair in this integrated candidate | First owner / required evidence | Draft status |
| --- | --- | --- | --- |
| C1 implicit activation loses trusted retained work | §§4.2–4.3/5.8: account initially empty, no active member/replay before explicit nonempty account plus ActivateOfflineQueue; return paused; name never identity | Tasks 5/8/13: delayed trusted restore and retained-account return; explicit wrong-identity activation still native Auth retirement | Specified; unexecuted |
| I1 irreversible shared seal missing | §§5.1–5.4: CAS closed+count shared by copied handles/old builders; posted Enter/Start/setter/auth/probe rechecks; close every gate before deinit callout | Task 5: any-thread CAS winner, pending Enter, Started reentrancy and old handle schedules | Specified; unexecuted |
| I2 native cleanup lifetime/admission | §§5.2–5.3/5.6–5.8: prepublished slot/drains, atomic cells/GT anchors, native Control tick/late discard, owned Flush/IO independent weak public delivery; no Hub retain | Task 5: false/suppressed callback, slot pressure, off-GT late token/Stopped/quarantine controls | Specified; unexecuted |
| I3 replay physical/FIFO/terminal cutoff | §§5.4–5.6/7.3: all-operation tails, dispatch guard leases, CancelRetain before cancel only if unpublished; pre-EndWaits marker and pre-Completed sealed result; accepted Retire IO truth | Task 5 then 6: held physical tail, compatible member, reentrant Completed sink/EndWaits/Finalize schedules | Specified; unexecuted |
| I4 tombstone history/registration reuse | §§5.2–5.6: fresh epoch per binding; no RetiredMembers list; one switch/two drains/finite rows; exact Op/gen/registry pointer fence | Task 5: alternating switches, reused ClientId/new epoch, stale remove/token and takeover counters | Specified; unexecuted |
| I5 default converter rule route | §2.1: process immutable default snapshot at top-level, owner override, conflict refusal, equal-owner and independent unregister, enum/folder semantics | Tasks 1/3/14: plain existing BP/native default conversion before any Hub, conflict/default-route mutants, settings-only cook | Specified; unexecuted |
| I6 global body enum collision/codec break | §§2.2/3/10: separate EVaRestEndpointBodyKind explicit map; native values unchanged v1/v2 | Tasks 2/4: joint include/UHT, pinned old bytes for every native kind and v1 replay | Specified; unexecuted |
| I7 invalid GI subsystem fallback | §4.1: public UObject facade, real GI and engine subsystem hosts/UPROPERTY, explicit future-spec correction | Task 8: real hosts with/without GI, lifecycle/connectivity/scope isolation; no fakeGI | Specified; unexecuted |
| I8 unsupported uint64 BP pins | §§2.3/4/6/9: canonical decimal FString for unsigned numeric IDs/generations; FGuid and existing QueueId strings preserved | Tasks 2/10: UHT and 0/max/invalid round trips across every reflected metadata/session/refresh surface | Specified; unexecuted |
| I9 fictional transaction API | §8.1: checked GEditor->Trans UTransBuffer actual delegates/weak binding; absent/replaced refusal and shutdown disable | Task 7: actual editor registration/undo-before-deserialize/replacement/absence | Specified; unexecuted |
| I10 terminal/frame/BP transfer | §§5.5/7.3/9.2–9.3: actual later frame before Finalize; cleanup/final recheck before marker; carrier reset before trace; explicit unchanged-primary unavailable after cutoff | Tasks 6/10: real G-BP-TRANSFER generated graph and reentrant compile; STOP/REPLAN before Task 11 | Specified construction; G-BP-TRANSFER OPEN |
| I11 false parser ownership bound | §7.5: remove actual 8*N bound; estimate explicitly unproved; finite input/node/key/work gates and measured requested DOM/scratch | Tasks 1/6/15: zero-array lower bound plus independent limit/refusal/cleanup oracles | Specified; unexecuted |
| I12 constructor/default/capacity/backlog overclaim | §§2.1/7.5/12.3: requested/logical limits only, explicit unproved allocation effects and caller-driven refused backlog outside admitted count; no universal heap cap | Tasks 1/6/15: defaults/native-copy/Json-copy/hostile logical amplification/invalid burst measurements | Specified; unexecuted |
| I13 cooked closure/native thread operations | §§8.2/7.2: Optional/full closure plus qualified native operations and stop/release proof; GT fallback initial/default and Config for unsafe types | Tasks 7/14: native lifecycle/Optional fixtures and real packaged selected-mode evidence | Specified fallback; G-COOKED-CLOSURE OPEN for worker |
| I14 retained shell off-GT reflected destruction | §§7.1–7.2/8/9: public shell owns no reflection/pins/GT token; private one-shot GT Take reset; preadmitted GT state/release independent retained copies | Tasks 6/7/10: release public copies on worker, zero carrier before callbacks, shutdown/late handoff | Specified; transfer/release gate unexecuted |
| I15 Saved/Saved path duplication | §2.4: Saved-relative VaRest/Cache and VaRest/Queue defaults resolved once | Task 3: normalized paths equal existing defaults; custom relative/absolute behavior | Specified; unexecuted |
| I16 per-asset redaction omitted | §§2.4/6: explicit asset RedactedKeys appended with provider/session/project keys and persistence guards | Tasks 3/9/13: custom secret in default/session/auth headers/query/body remains guarded/redacted | Specified; unexecuted |
| I17 incomplete task DAG/ownership/build/link | §11: early reflected/auth/editor bridge foundations; native control Task 5; dynamic Task 6; hooks Task 7; Hub Task 8; real BP context action Task 10; shared expansion helper; external VaRestEditor provider module | Each ordered task compiles only existing declarations; Tasks 10/12 actual generated control and external consumer link; no future-type UHT stub | Specified; unexecuted |
| I18 masked ModifyCook negative | Task 14/§12.4: settings-only package separate from BP fixture, cook-only gameplay preload skip, dependency manifest proves sole inclusion route | Tasks 3/14: remove ModifyCook -> package absent; remove runtime preload -> early FindObject/default conversion fails | Specified; unexecuted |
| M1 DateTime category | §§2.2/10: PC_Struct plus FDateTime script struct for scalar/container pins | Tasks 2/11/14: real compile/run DateTime scalar/array/map | Specified; unexecuted |
| M2 new OAuth expiry path | §§2.3/6: new client_credentials expires_in; legacy BearerRefresh frozen expiresIn/fallback unchanged | Task 9: standard default token response sets expiry/proactive lead, plus custom path | Specified; unexecuted |
| M3 first scope event/GT cost | §§3/5.8/7.4: complete scope/name before factory Created, full metadata/closed-shell event contract; separate GT initialization/conversion/copy/reset timing | Tasks 4/5/8/15: capture from first Created, late closed-shell destruction, measured GT cost distinct parse/frame waiting | Specified; unexecuted |

## 14. Mandatory decision and evidence gates before acceptance/release

This replacement is one integrated author proposal incorporating Root decisions, not a stack of alternate designs. Native caps are Root planning choices; dynamic numeric defaults remain tentative review/tuning inputs with honest logical scope. The following gates are explicit work owned by the ordered tasks and program, not requests for the user to answer implementation trivia:

1. Root review plus a fresh independent FULL replacement-plan review; author cannot self-accept. Preserve original artifacts and this review chain. Any new repair changes require exact revised candidate/report hashes and targeted re-review.
2. Fresh actual accepted WHOLE-I1 reconciliation BEFORE tracked freeze/source. Record the actual accepted Inspector commit and reconcile every shared function/type/event/carry; transient Task 8 work is not accepted by proximity. Root sole-source/graph/native/index leases and program milestone ordering remain binding.
3. G-RETIREMENT Task 5: compile native construction and prove atomically admitted all-operation lifetime, every callback/guard/task discard, accepted terminal/commit cutoffs, bounded cell/drain/waiter bookkeeping, last-member exact fence and late lease-safe control. Any missing real edge blocks integration; a wrapper flag/counter-only demo is insufficient.
4. G-BP-TRANSFER Task 10: prove actual compiled shared storage/control flow and the synchronous Completed-sink compile/reinstance schedule. If it fails, stop before Task 11 and replan the transfer; no default typed Success or silent changed error. This gate is explicitly OPEN in this document.
5. G-COOKED-CLOSURE Tasks 7/14: worker mode is disabled until complete Optional/type/rule/native lifecycle/shutdown proof. Initial GT mode is explicit. Unknown native lifecycle can be refused. Do not claim a pin is thread-safety proof or process-exit quarantine solves worker reflection lifetime.
6. G-COOK-PRELOAD Task 14: actual manifest/isolation and early no-load proof with two independent mutants, plus real BP runtime/package outputs. Incidental cook inclusion invalidates the fixture. Native/cook/Dev/Shipping/UI/resource/normal/NoPCH and full regression gates remain separately required, not inferred from this design.
7. Task 16 independent task/whole-M5 reviews/fix wave, exact public/config freeze, documentation and release evidence. Downstream OpenAPI O3/W5/M7/Inspector consumers remain separate; passing M5 does not close them. Owner retains push/tag/Fab/release actions.

No source implementation, native/UHT/compiler probe, graph/build, runtime/server/cook/test, Beads change, git/index/commit/remote or HGS/session action was performed by this authoring pass. The only intended writes are the ignored integrated candidate and its new author report. Tools/testserver/certs/test-server.key remained filename-only and was never read, hashed, copied or diffed.
