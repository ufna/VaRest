# B9 native owner: producer/deployment revision candidate

2026-11-05 · VaRest-81s.21 · proposal for independent review; no new module/source activation.

## Decision being proposed

Use one dedicated VaRest runtime module, provisionally `VaRestNativeOwner`, as the actual producer of the existing bootstrap1/owner2 native lifetime mechanism. It contains no reflected types, automation registrations, HTTP/WebSocket implementation, user delegates or UE-owned resolver state. `VaRestWebSocket` obtains a real independently owned direct entry credit through its private loader adapter before entering the native API. The same module is the production implementation and the A/B proof producer. No miniature proof-only substitute is accepted.

This explicitly changes the old proof plan's requirements that producers must be complete `VaRestWebSocket` artifacts and that no additional helper/module/dependency may be introduced. Those old requirements remain active until this revision is independently reviewed and Root accepts an exact replacement scope. The conditional ownership protocol, numerical limits and frozen ABI are inherited, not proven by this proposal.

The motivation is concrete: full WebSocket module constructors register UObjects and automation instances in engine-global registries, while direct module-copy retirement does not supply corresponding per-copy cleanup. Normal ModuleManager activation also does not expose original direct E/P history to the current native seam. The dedicated producer separates those reflection lifetimes from resolver code and makes a fresh successful private direct open an actual observable caller-owned operation.

## Compared arrangements

| Arrangement | Current primary evidence and disposition |
| --- | --- |
| Unchanged full WebSocket A/B copies | Actual normal221 INIT_ARRAY/registration calls and installed CoreUObject/automation paths fail the constructor/unload prerequisite. No further duplicate-load experiment is selected. |
| Engine GetAddressInfoAsync | `SocketSubsystem.cpp:113–153` stores a user callback in an auto-deleting engine task and invokes it after resolution. Moving scheduling into the engine does not remove the callback/destructor code-lifetime obligation or provide the required two-slot admission/retirement protocol. No assumption that cancellation stops OS DNS. |
| Legacy GetHostByName/FResolveInfo | Current implementation allocates FResolveInfoAsync with a caller-observed object; its worker keeps Parent and the object owns an FAsyncTask. It returns only the resolved address exposed by the deprecated interface. This does not establish eight unique candidates, bounded post-unload object cleanup or actual worker retirement. It is not selected. |
| Engine/ModuleManager private handle or modified loader | Would change the supplied SDK or depend on private state. Current scope forbids that. No such change is selected. |
| Dedicated actual runtime producer with direct private entry | Proposed. Adds a real package/build dependency and preserves the need to prove its own actual constructors, import/relocation closure, direct entry, self pin and worker retirement. No constructor-free or safe-unload claim is assumed from an empty module class. |

## Build, registration and package boundary

The candidate module is Runtime with LoadingPhase None. Its ordinary module translation unit uses the documented `IMPLEMENT_MODULE(FDefaultModuleImpl, VaRestNativeOwner)` mechanism. No UCLASS/USTRUCT/UENUM, generated reflection registration or `IMPLEMENT_*AUTOMATION_TEST` occurs in the producer. All automation lives in `VaRestTests`. A development-only private bridge may expose controlled proof entry without becoming a production authority packet.

The installed UE5.8 macro is not constructor-free: `ModuleManager.h:955–969` emits `FModuleInitializerEntry`. Its current constructor links a node into Core's global initializer list; its destructor unlinks that exact node (`ModuleManager.cpp:100–136`). FindModule returns the first matching node. Thus A/B proof must audit and attest balanced node lifetime, common GT serialization, absence of unexpected ModuleManager instantiation while copies are present, and complete Core dependency lifetime. Generated per-module boilerplate, allocators, init/fini arrays, TLS and atexit dependencies must also be inspected in actual final artifacts. The native getter/census closure itself may not enter these engine operations.

Proposed rules use an ordinary CPlusPlus plugin module with Core as its only UE dependency. Pure native translation units include their own C/platform headers and keep UE calls outside the native worker/getter closure. Permanent NoPCH/nonunity rules for this new native module are an implementation proposal requiring fresh complete graph review, not a global flag or permission to rebuild Engine PCHs. The ordinary module TU remains part of the real linked producer and is never stripped to make proof pass.

WebSocket must not acquire a link-time import that silently loads/uses the producer as a substitute for direct entry evidence. Its build relationship uses private include access and ordinary dynamic-module staging/build declarations. The plugin descriptor must stage the real module on each supported target. The adapter resolves the declared platform product path before opening, then records the successful actual native open result. ModuleManager's current filename may choose the request path; it is never used as historical handle evidence.

Packaging cost is one additional actual runtime module/artifact and its platform-specific build/deployment coverage. Public Blueprint/HTTP/socket enums and callable APIs do not change. Existing cooked/reflected-worker mode flags stay disabled. No monolithic executable Link, platform success or SDK rebuild is authorized by this proposal.

## Entry, trust and mapping lifetime

For GNU/Linux the adapter performs an actual ordinary absolute token-free `dlopen(P, RTLD_NOW|RTLD_LOCAL)` and retains its returned E continuously through all native entry, unwind and return. It uses the native API directly at the private platform seam rather than assuming a wrapper's flags. Known retained E/file-fd/receipt/mapping association must be established before invoking the owner activation bridge. Native self S still originates inside the producer and follows the inherited one-census-before-S protocol. No borrowed ModuleManager handle, mapping base cast or stack-only guard becomes an owned pin.

The trusted deployment catalogue remains bounded and immutable, with original fd/inode/segment/receipt association. A note or a file hash alone never supplies trust. Unknown lineage, unexpected existing-image selection, file replacement/mapping mismatch, unsupported namespace or provenance ambiguity refuses before native owner admission. The candidate must give an exact finite policy for an unexpected E, including across repeated WebSocket unload/reload; it must not accumulate foreign credits silently or guess a safe final unwind. This is an explicit unresolved review item, not an implicit ownership theorem.

Normal supported deployment treats the native producer artifact as immutable for the process. Updating/hot-patching this producer while loaded requires process restart unless a separately certified catalogue/handoff arrangement proves the new deployment. Reloading the WebSocket module can reuse the unchanged independently retained native producer and cannot reset its native state. The review must reconcile this cost with existing reload requirements and identify any requirement change explicitly.

On successful native admission the same fixed native owner mechanism retains exactly its own S/O and bounded 16KiB arena/two resolver slots. The caller E belongs to the live adapter/service and is accounted separately. After WebSocket Shutdown the helper's late work contains native producer code/data only: no UObject, engine allocator/delegate, WS code pointer or service object. The helper must not attempt to release its own final pin while returning through code that could unmap; retirement/uncertain-release contracts are inherited unchanged.

The adapter still has to bind the actual GT/process/control identity and use private trusted entry; an arbitrary packet, script flag or caller-supplied receipt cannot appoint itself trusted. Windows, Apple, Android, main-executable and merged cases remain separate gates. This proposal does not import GNU namespace/tryjoin semantics to them or cut the required platform scope.

## Service and connector ownership

Resolver slots and thread cleanup live solely in the producer. The WebSocket service owns its existing LWS/SSL/UE resources and must actually join/close them before releasing its corresponding native ConnectorLease. The producer stores finite resource counts/native identities, never borrowed LWS/UE pointers. Moving the owner to a separate module does not permit detaching early, resetting counters, allocating another owner on reload, or forgetting a blocked worker.

The original deadline and ordered candidate/actual-peer fallback include proxy peers, numeric IPv6, Host/CONNECT/TLS/SNI and credential rules. The numeric connector remains a separate prerequisite. No protocol behavior or transport replacement is introduced merely by moving lifetime code.

## Finite replacement scope and acceptance boundary

The proposed source scope is a new module Build.cs/public POD ABI/private native implementation and ordinary module TU, the plugin descriptor, private WS adapter/build declarations, existing VaRestTests proof adapter, and the already planned offline artifact/wrapper/passport tools. Existing Source271 ABI0 records and passive code are reusable; an explicit source move removes the temporary WebSocket export only after its owner-free pre-protocol receipt is recorded. No engine/host/generated/config/cache/object edit is included.

Before source activation, independent review must resolve the unexpected-E credit policy, actual constructor/finalizer/Core dependency boundary, exact dynamic staging without hidden import ownership, trusted deployment catalogue and supported restart/reload contract. Then Root freezes exact paths and a finite replacement of proof-plan sections2/4/6/7. The current frozen documents are preserved as history. A new A/B/default/no-tests set must use the real new producer and undergo its own fresh graph/build/closure/ABI/constructor/provenance checks. Old normal221 cold getter results cannot waive any new artifact gate.

Until that review and Root decision, no new module is authored or loaded. The old full-module A/B path stays stopped. Other authorized feature work continues while this concrete producer arrangement is reviewed.
