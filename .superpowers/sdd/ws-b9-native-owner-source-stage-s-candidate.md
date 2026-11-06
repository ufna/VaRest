# Native owner source stage S — S1 candidate and immediate S2 contract

2026-11-06 · VaRest-81s.21.8 · PLAN ONLY; independent review and Root's exact source lease are required.

## Decision

Select **S1: real native records, guards and five owner-operation bodies**, followed immediately by a separately frozen **S2: arena construction, submit, poll and retire**. These are source-authoring tranches in the actual VaRestNativeOwner producer. S1 does not wait for R/A-B/P runtime acceptance. Neither tranche opens those runtime/authority gates. Source275's unrelated W3 diagnostic sequencing is Root's responsibility and does not block this native source plan.

S1 implements query, attach_service, detach_service, copy_result and discard as substantive hidden native functions with normal state/ticket/role/error branches. It publishes no partial API table, creates no owner, and launches no work. The bootstrap's owner/api remain NULL by construction because no S1 path assigns a live owner or API. This is neither a five-function public ABI nor five refusal stubs. Submit/poll/retire are absent in S1, not replaced with permissive/empty functions. S2 completes the real eight-function table and native worker machinery under the internal preconditions below; its construction/registration helpers stay unexposed and uninvoked until separately authorized admission exists.

## 1. Binding inputs and exact amendment

Read in full: design escrow2 `0fec35b74f8ca6e9de16b9d7a56aec6c4bb4f0b136b6984e2357fbb022648872`, proof-plan-fix1 `0dbe5dadb019aeaba9b927a9b63349e20e5f6a48f4ca2f78cb5eff0abed549c2`, producer arrangement v2 `2a60a6754c8713e833537e6b889b70bc9deca424e80530efc9e7468f23feec31`, and all six actual module files. Accepted L001 is an offline passive migration/artifact result; no ownership or loader execution follows. Historical documents/receipts remain unchanged.

Upon independent review and explicit Root acceptance, amend only **source sequencing and exact source paths**: permit dormant native implementation authoring and separately guarded default-artifact builds after L, before the old proof plan's B/C/D runtime order. In particular, the original Phase0/source-before-worker restriction no longer blocks these closed-entry source tranches. All historical E/P, trusted catalogue, census-before-S, BASE, pin/arena, irreversible transfer, actual retirement, host/CRT/finalizer and platform prerequisites still apply before their corresponding activation/runtime. No old runtime phase is marked passed. Acceptance of this candidate alone is not a source/build lease.

The authoritative current six-file inputs are:

| Path under Source/VaRestNativeOwner | SHA256 |
| --- | --- |
| `VaRestNativeOwner.Build.cs` | `fddeb20eb15983a10cbab984735c82d5833ba4c539902a5769d120a3a423e8f1` |
| `Public/VaRestNativeOwnerAbi.h` | `d53d3bbc415c9bc0bc8612f3c26ed7f5088531222b3fb87ed22dbe10daab23c9` |
| `Private/NativeOwner/VaRestNativeOwner.cpp` | `d0f5b133fb38cc572e407d014f603296cffe463eaf7a574e57256de1cf199821` |
| `Private/NativeOwner/VaRestNativeOwnerInternal.h` | `a89d50839eb64ab591a1775cdcc5f603e22ff301356903c90bc3f69bb0a201b1` |
| `Private/NativeOwner/VaRestNativeOwnerProducer.h` | `1ac13242e141ef36f23a90e7d59d54347189b1a519bbe5757f37f5d2f0e0a6f3` |
| `Private/VaRestNativeOwnerModule.cpp` | `0305512c06e3e4f4b7d1f3e2e317941ca7ce08afc84305cdc2639331f444b560` |

Root reconciles these hashes and the current combined Source version when commissioning code. No whole-tree historical checkout or rollback is authorized.

## 2. S1 exact five-path source scope

All following paths are relative to `Source/VaRestNativeOwner/Private/NativeOwner/`.

| Path | Exact S1 change |
| --- | --- |
| `VaRestNativeOwnerInternal.h` | Exact GNU thread type, private descriptor additions and hidden native descriptor/image access declarations described below. Preserve inline160 and every public ABI record/number. |
| `VaRestNativeOwner.cpp` | Keep the real permanent getter and one actual descriptor; replace only its unbound-thread pristine check for the exact native type, validate the new cold private fields, and add hidden same-image descriptor/image access functions used outside the getter. No binder, factory, table publication, census or loader calls. |
| `VaRestNativeOwnerState.h` — NEW | Native owner/slot/service/lease POD definitions, bounds/lock-free assertions and five exact ABI-shaped hidden declarations. No second owner instance/pool, callback, authority packet or activation declaration. |
| `VaRestNativeOwnerState.cpp` — NEW | Real five-operation bodies and local guard/snapshot/ticket/clock/interest/deferred-detach helpers. No resource creation or retirement observation. |
| `VaRestNativeOwnerProducer.h` | Only the S1 identity literal/comment and its32 digest bytes change. |

Build.cs, ordinary module TU, Public ABI header, descriptor/staging declarations, consumers, tests, tools, docs other than this author document, SDK/host/generated/cache/config files are not S1 implementation paths. Keep the ordinary module/Core dependency and permanent local NoPCH/nonunity. Do not restore borrowed getter tests, author a test bridge, load the module, or edit Connect/EnsureStarted/service/pump/Stop hooks.

GNU source is limited to `__linux__ && __x86_64__ && !__ANDROID__`. Native headers are C/POSIX headers and the existing POD ABI; no UE types/includes/macros are used in native operation TUs. Other targets retain closed passive/refusal behavior; no invented portable retirement implementation is emitted.

## 3. S1 owned records and exact identity

The current private uint64 ControlThread is an unbound placeholder, not a pthread_t. For GNU x86_64 replace its type with the actual public `pthread_t`, retaining ControlProcess as a checked uint64 representation of positive getpid(). Compare an established thread only with `pthread_equal(pthread_self(), retained_thread)`. Never cast pthread_t to a chosen integer, test its numeric value to establish ownership, inspect its internals, or infer liveness from it. ControlBound remains the validity discriminator. A fork/process mismatch returns FORK_UNESTABLISHED; wrong established role returns WRONG_THREAD. Native checks do not establish the initial UE GT authority, a thread lifetime or replacement safety.

Freeze the private GNU descriptor layout at existing offsets through inline escrow: Lock0, ActivationDepth4, ActivationGeneration8, ControlProcess16, exact pthread_t ControlThread24, ControlBound32, Reserved36, Owner40, Api48, Escrow56..215. Append InlinePoisonReason at216, Reserved2 at220 and NextOwnerEpoch at224, total232/alignment8. The added reason records later arena-release responsibility without using public reserved bytes; NextOwnerEpoch is a separate monotonic owner-epoch allocator, initially0. ActivationGeneration stays1 initially. Require actual public pthread_t size8/alignment<=8, trivial representation and concrete target offsets; if ordinary GNU target types differ, stop rather than guess. Non-GNU passive compilation must not acquire a native role or expose these operations.

All descriptor bytes remain constant-initialized POD: locks/depth/binding/reasons/counter/pointers/inline escrow zero, generation1. Cold thread checking is a bounded pristine object-byte check while unbound, never pthread_equal or another import inside the getter. No constructor/finalizer initializes, clears or destroys this record. The getter continues copying its existing owner/api fields; S1 supplies no writer that can make them nonnull. It never reads an arena, operation body or relocated operation table. The private extent change requires a new complete getter/storage/relocation audit; L's624-byte receipt is not carried forward as a binary certificate.

`VaRestNativeOwnerState.h` defines exactly one **type**, not an allocated/static owner, for the eventual native arena. Concrete field order follows these records; private sizeof/align/offset values are recorded from the actual compiler, with all records trivial/standard-layout and the total owner <=16384 bytes:

| Record | Ordered native data |
| --- | --- |
| Slot[2] | uint64 generation/service_epoch/deadline_ns; uint32 state/hostname_bytes; hostname[256]; VrResult; uint32 result_status/result_published/interest; uint32 thread_created/thread_joined/close_done/retirement_uncertain/wsa_credit; exact pthread_t; int32 native_error; zero reserved. |
| Service identity | Exact pthread_t; uint32 registered/reserved. Valid only with its current owner/service epoch; no handle or UE object. |
| ConnectorLease | uint64 service_epoch; uint32 leased/service_started/service_joined/poisoned; service_start_reserved/service_never_created/service_call_executing; listener/ingress/pair/buffer/WSI/vhost/context/dependency/wake-descriptor counts; reserved; pump_registered/pump_executing/deferred_detach_after_join. |
| Concrete opaque VrOwner | uint32 phase/poison_reason; uint64 owner_epoch/next_service_epoch/mapping_bytes/stop_epoch; VrPin owner_pin; service identity; exactly two slots; one ConnectorLease. |

No native_connector pointer is included: a WS-created arena cannot be retained as creating-image native ownership. There is no UE/LWS pointer, function/delegate, callback, STL container, per-image reaper or extra resolver pool. Public records and inline160 remain byte-for-byte frozen. Source uses aligned uint32/uint64 with compiler `__atomic` operations, not guessed std::atomic layout. Assert lock-free32/64 lowering; no external atomic runtime is permitted.

Freeze private slot numbers FREE0, RESERVED1, RUNNING2, RETURNED_UNJOINED3, RETIRED_RESULT4, CLEANUP5, POISONED6. Native owner phases use the existing frozen VR_PHASE numbers. Worker-written result and native error are not read before actual retirement; result_published is only an atomic informative marker. All slot state/resource accounting changes hold the descriptor try-lock. Interest, stop epoch and pump flags use their specified acquire/release atomics.

Service start reservation is necessary before an actual thread could exist: attach reserves the lease and sets service_start_reserved=1, service_never_created=0. Detach cannot infer “no thread” from service_started=0 while this reservation remains. Later private creating-code outcomes must establish never-created or real joined state. This closes the early-start/registration race; it is not a new public counter-edit interface.

## 4. Real S1 functions, guards and outputs

Declare/define these exact hidden C++ functions with VR_CALL and the frozen parameter/result types: `NativeQuery2`, `NativeAttachService2`, `NativeDetachService2`, `NativeCopyResult2`, `NativeDiscard2`. Their signatures correspond respectively to the five existing VrOwnerApi2 members. No public export, C entry packet or partial VrOwnerApi2 instance is added. Use `[[gnu::used]]` on these five hidden production bodies solely to retain compilation/emission before a complete table references them. Do not use noinline/optimizer/safeguard switches, new link flags or test-only replacement code. If final Link removes or folds away the substantive bodies, stop with that fact; do not call empty output an implementation.

The getter TU provides hidden `NativeDescriptorForOwnerOperations()` and `NativeImageIdForOwnerOperations()` for same-image implementation use. They grant no admission and are not called by the getter. Keep its descriptor local; no mutable data symbol, pointer or API is exported to a consumer. Hidden operation references bind locally, not through an interposed PLT entry.

Local helper responsibilities are fixed: one-shot descriptor locking; output initialization; descriptor framing/blocker checks; current process/control/service role checks; owner/session/ticket validation; coherent snapshot build; monotonic-clock conversion; interest invalidation; known-clean retired-slot release; live-owner poison; and deferred-detach reconciliation. No helper binds a caller, allocates an arena, acquires a loader reference, creates/joins/detaches/cancels a thread, closes a socket, publishes Owner/Api, or writes a live pin from caller input.

For each operation: validate accessible output/input size/version/alignment/reserved contracts; take exactly one nonrecursive descriptor try-lock (BUSY has no native side effect); apply inline poison/prepared blocker precedence; reject cold ControlBound0 as UNBOUND_CONTROL; check the actual process/role. Compare the opaque owner token with the locked current nonnull Owner and check coherent normal phase/API publication **before dereferencing it**. A caller pointer is never dereferenced to discover whether it is valid. Exactly-one-null publication outside a guarded transitional phase is incompatible. Revalidate epochs/session/ticket and stop state before mutation. No operation resets an activation flag owned by an enclosing transaction or treats a copied tuple as authority.

| Function | Required substantive normal body |
| --- | --- |
| NativeQuery2 | Control role; build the frozen64-byte snapshot from actual known owner/lease/slot bookkeeping under the lock. Occupancy includes every non-FREE reservation. Known live-owner poison can be returned as OK with snapshot.state POISONED; inline/no-arena poison has no callable owner snapshot. No polling or acquisition. |
| NativeAttachService2 | Control role; reconcile only a previously proven complete deferred detach; require ACTIVE/DORMANT compatible owner and no current/uncertain lease. Reserve a fresh nonzero service epoch before resources and return its session; set one pending lease/start reservation. Existing DNS slots/generations/interest responsibilities remain untouched. Occupied slots alone do not forbid attaching once the old connector is truly retired. Wrap poisons/refuses before issuing a session. |
| NativeDetachService2 | Control role and exact session. Require all resource counts/pump flags/service-call execution zero, no pending start reservation, and an actual never-created or joined service disposition. Otherwise OLD_OWNER_BUSY. Invalidate that service's interest/tickets without resetting running/unjoined slots; known-clean RETIRED_RESULT may become FREE. Set DORMANT and clear only the completed lease, retaining owner pin/arena/epochs and both slots. Never poll DNS. |
| NativeCopyResult2 | Current registered service role and exact interested ticket; honor stop_epoch and original monotonic deadline. Only RETIRED_RESULT with known join/cleanup can copy <=368 bytes and free the slot; RUNNING/RETURNED_UNJOINED remain PENDING. Expiry invalidates interest, without reclaiming unretired work. Preserve explicit NATIVE_ERROR/NO_RESULT ticket/result framing and zero unused addresses; ordinary error outputs retain only the frozen header. No deadline restart. |
| NativeDiscard2 | Either matching bound role and exact ticket. Atomically invalidate interest. Only already retired/known-clean result storage can become FREE; every created-but-unjoined/uncertain record remains occupied. Stale input changes nothing. No wait/join/close or completion post. |

A live-owner poison blocks new attach/submit/handoff and is never “fixed” by recreation. Only explicitly known-safe bookkeeping/cleanup may progress; no ambiguous handle is retried. Query may observe it without appointing a new owner. All uint64 generations/epochs refuse wrap. Hostnames, credentials and tokens are not logged.

S1 imports outside the getter may use public `getpid`, `pthread_self`, `pthread_equal`, and `clock_gettime(CLOCK_MONOTONIC)` for these real role/deadline paths. Check tv_sec/tv_nsec and uint64 nanosecond arithmetic without speculative wide-arithmetic helpers. A clock error poisons the known live owner and refuses, never extends time. Native POD clear/copy helpers must have no UE allocator/log/trace/task dependency. S1 has no mmap/munmap/getaddrinfo/freeaddrinfo/pthread_create/tryjoin execution path or source implementation yet.

## 5. Emission, identity, carry and S1 validation

The S1 identity literal is `VaRest/native-owner/abi2/VaRestNativeOwner/source-s1-core/linux-x86_64/2026-11-06`; SHA256 `eced59d9228cd63c0d9f95074175db5ed8304e1fd27183256d7c691f28cd0165`. Encode those32 digest bytes in order. This is a producer variant ID, not authentication or a final artifact hash. The bootstrap name/version and owner2 numbers do not change. No native family note layout is invented by S1. Before eventual admission, the actual family/lineage note, catalogue and original mapping contracts still need their own accepted scope.

A source change normally requires restart of any process containing the previous producer; no hot patch or alias update is selected. Preserve L001 and exact old owner-free lineage. Audit a new artifact's owner-free/closed-entry status from its actual code; do not infer that classification merely from NULL pointers, ID or an old receipt. WS reload later must reuse the same native descriptor/slots/epochs/poison; S does not implement that integration or allow a reset. Terminal native S+at-most-U and the distinct future caller-E/U_E allowance remain unchanged; unused1280 scratch bytes remain unused.

After A's independent plan review, Root may freeze a finite S1 source lease, save exact before bytes and reconcile Source sequencing. Any build is a separate Root action: new full graph for every C++ version/target, inspect all commands/recursive responses/unity/source/link/product closure first; plugin Compile/Link plus explicit receipt metadata only; stop on engine/host Compile/Link or PCH creation. Keep local NoPCH/nonunity and consumers' settings; no flags/cache/generated/object/timestamp workaround. Source/build/runtime are separate statuses.

Require actual default `.so`/sidecars with substantive five bodies, exact native types/layout assertions, complete new getter/read-set and relocation/pre-init/fini audit, hidden symbol/local reference verification, no table/public activation/binder/loader/arena/worker surface, and unchanged consumer nonimport boundary/None staging. Record compiler warnings and native/UBA reuse honestly. Native operation imports are outside the getter and still require future lifetime audit. No API/runtime pass is emitted. Do not create a synthetic live owner, write its tuple into the descriptor, call hidden operations or revive old borrowed tests to claim validation. Runtime behavioral/mutant cases remain the frozen later proof obligations. A failed emission/type/closure fact stops the tranche for Root, not a flag search.

## 6. Immediate S2 source contract proposal — complete backend, still no authority route

This section is ready for **separate native source-contract review**, not parked behind R/P runtime. Its recommended decisions below must be explicitly frozen before S2 code. S1 is independent of that decision. S2's source would change State.h/cpp and Internal.h as necessary within this contract; add only `VaRestNativeOwnerArena.cpp` and `VaRestNativeResolver.cpp` in this same private directory, and change Producer.h identity. Getter changes, if any, are limited to truthful new private phase/reason validation. No Build/public ABI/consumer/private production bridge is included. Root must issue those exact paths separately; this candidate grants none.

S2 adds real `NativeSubmit2`, `NativePollRetirement2`, `NativeRetire2`, completing the existing eight signatures. Only after all eight substantive bodies exist may a hidden immutable constinit VrOwnerApi2 table contain the eight nonnull function addresses in frozen order. Keep it outside the getter's cold read closure; normal loader relocations in this unpublished table are not a pre-init read shortcut. No half-filled table or readiness feature flag is permitted.

### 6.1 Native construction proposal

A hidden, uninvoked `NativeConstructPreparedOwner()` reads only the creating image's actual locked prepared state; no caller packet, bool, ID, receipt or fake handle authorizes it. Its caller must already have completed accepted admission/census and obtained/recorded genuine S. S2 does not supply that caller, bind control, acquire S, discover peers, accept a catalogue or open anything.

Require: immutable established local control/process identity; caller actually owns the existing descriptor lock/activation transaction; PREPARED/known-consumed handoff phase and exact inline160/2; genuine known self pin in its inline slot; Owner/Api NULL; no U, pending old close or terminal poison; and independently reserved nonwrapping NextOwnerEpoch plus image generation. Scalar checks are defensive consistency checks, not proof of historical pin or GT authority. Missing premises refuse before allocation.

Before mmap, validate native page-size query and overflow-safe rounding of16384, all native sizes/capacity and initialization/output locations. Use anonymous private mmap, exactly one owner mapping, and record actual rounded bytes. MAP_FAILED obtains nothing; leave the known S prepared for its existing caller's later once-only unwind under E. No loader release occurs in this helper. A successful mapping is immediately recorded under S in the busy locked descriptor before any later fallible step; all remaining initialization/publication is preprepared native POD work.

**Recommended storage-lifetime decision for review:** use a bounded readonly constinit trivial VrOwner initialization image and native C++20 memcpy implicit-object creation into raw mapped storage, then infallible prepared field stores. It is an immutable construction template, never another mutable owner/slot pool, reservation or arena. Do not use UE/new/delete/placement-new/containers or a destructor to own it. Static assertions cover trivial/standard layout, all fields and actual native type lowering; source/artifact review must accept this precise C++ storage-lifetime method before Arena.cpp is commissioned. If not accepted, stop this one construction decision for Root; do not invent an allocation/placement workaround or hold S1 for runtime proof.

Under the still-held lock, set mapping bytes/owner epoch/service allocator/zero lease and FREE slots; move genuine S logically from inline self_pin to Owner.owner_pin with no extra OS reference; publish the complete table and ACTIVE owner only after coherent initialization. Known obtained storage cannot be treated as “allocation obtained nothing” on later uncertainty. Any uncertain arena cleanup retains known S inline, clears callable publication and preserves POISONED/ARENA_RELEASE in InlinePoisonReason. No U retry or alternate arena follows.

This factory remains hidden and has **zero callers in S2**. No S1/S2 source writes initial ControlBound or creates a prepared self credit from ingress. Therefore legitimate runtime paths still cannot create an owner or publish an API. Keeping implementation dormant is a call-graph property to audit, not a runtime switch, trusted-caller packet or permission to directly invoke the helper.

### 6.2 Native role and service-start proposal

There is one authoritative immutable control identity in the image descriptor. A future authorized creating-image admission path derives getpid/pthread_self after the real UE GT check; it cannot accept caller-chosen IDs, rebind on attach, reset after fork or silently replace a dead control thread. S2 authors no such binder. Thread identity equality does not prove thread lifetime/reuse safety; the host/control lifetime certificate remains later.

Attach reserves the single lease and service_start_reserved before any service could be created. Proposed hidden creating-code outcomes are `NativeServiceNeverCreated(owner,session)`, `NativeRegisterCurrentServiceThread(owner,session)` and `NativeServiceJoinObserved(owner,session)`. They take the existing native lock once and exact epoch; no public API/table entry or consumer export is added. Registration derives the current pthread_t locally in the actual native service-entry path. NeverCreated is called only after creating code proves no thread was created. JoinObserved is called only after its creating owner observes actual service join, including a created thread that returned before registration; it may discharge the start reservation on that known outcome. No thread ID liveness probe or generic boolean statement supplies these observations.

The exact future host/proof call sites, module-code lifetime and cross-module service-entry/export surface remain CLOSED A-B/P. Source may implement these unexposed functions with zero callers; no caller can appoint itself through the bootstrap/eight operations. The service stores only native identity, never a UE thread object or WS callback. Submit/copy require the registered service thread/current session; query/poll/attach/detach/retire require control; discard accepts either matching role. After real service join, control can perform permitted remaining cleanup.

Stop will release-store only the matching stop_epoch and remove its live pump/lease obligations through the separately frozen adapter. It cannot join DNS or release a loader reference. Pump registration/execution/deferred-detach bookkeeping remains charged until removal and callback epilogue. No S2 source simulates an engine frame or installs an engine delegate. P must bind every actual socket/buffer/LWS/dependency/service/pump acquisition and successful close to the one lease. Counter values alone are never resource proof.

### 6.3 Submit and native worker proposal

Under one descriptor lock: validate current role/owner/session/stop/pump_registered1, original nonzero deadline and native clock, hostname1..255+NUL/no embedded NUL/remaining zeros, flags0, port nonzero and one FREE slot. Reserve its generation/record before any per-call resource. Third reservation is CAPACITY, with no pending queue or third create. Never reject the remaining FREE slot merely because the other worker is late. No service recreation clears slots or deadline/generation state.

Initialize worker-owned POD and copy the hostname into its fixed slot before pthread_create. On nonzero create result no joinable thread was obtained; retain only actual obligations through CLEANUP and release that reservation when known clean. On success record the exact public pthread_t and created flag. An early-running/returning child is safe because the record exists first and poll cannot enter before submit unlocks. No detach, cancel, suspended-thread trick, service object or per-worker image pin is introduced.

The worker entry receives only its already-reserved native slot in the protected owner arena. Native getaddrinfo uses AF_UNSPEC, SOCK_STREAM and numeric service, preserving returned order. It copies up to eight unique IPv4/IPv6 candidates, matching family/address/scope/port without padding; malformed/unsupported entries are skipped. No IPv4-only fallback. Decimal service formatting is bounded native arithmetic. Capture native resolver error before another native call; successful zero usable candidates is explicit NO_RESULT. Free the actually obtained successful resolver list with matching freeaddrinfo before release-storing result_published, then return normally through pthread/CRT. No UE allocator/delegate/SocketSubsystem/task/post, escaped resolver pointer, own-arena free or own-pin release exists. NSS/provider memory and latency remain outside plugin bounds and need later actual provider/exit audit.

Public GNU declarations/lowering must compile under the ordinary native module rules. If pthread_tryjoin_np requires a feature-test macro, freeze only a TU-local `_GNU_SOURCE` before native headers in Resolver.cpp; no Build/global/compiler flag or SDK edit follows. Actual headers/function origins/retirement implementation remain later runtime gates; source can implement the specified public call without claiming the current host is already certified.

### 6.4 Poll and retirement proposal

NativePollRetirement2 is control-only, one descriptor try-lock, and at most one zero-time pthread_tryjoin_np observation for each of the two created/unjoined non-uncertain slots. It never waits for DNS and is not called by Stop or a service completion callback. EBUSY retains occupancy. Zero records actual join/TCB cleanup before result consumption; result_published alone never retires. Every other result records uncertainty/poison and prevents retry of that possibly consumed pthread token. Other independently known-safe cleanup may progress without new admission.

Record each actual join result immediately. Observe stop_epoch between observations/cleanup steps; a newly stopped/reentrant teardown prevents another observation in that turn while retaining any unfinished fixed responsibility. Joined/clean but still-interested results become RETIRED_RESULT; stale/stopped/discarded/expired interest becomes FREE only after actual retirement and cleanup. Preserve the old ticket's service/slot generations and original deadline; no result reaches copy_result before join. GNU needs no CloseHandle/WSA balancing, and does not import that assumption into another platform. Clock error poisons rather than restarts time.

NativeRetire2 validates the complete creating owner/session/control/quiescence/output/token contract before freeing anything: both slots truly FREE; service never-created or actually joined; no start reservation, lease/resources/pump/service execution/deferred obligation; exact expected epoch and a distinct already verified/pinned successor under the external admission protocol. It moves O into its own inline slot and prepares all locals/output/stores before munmap.

On successful creating-image munmap, execute only the frozen infallible prepared stores: clear Owner/Api, publish RETIRED, transfer the exact O in VrRetired, release the passive lock and return through the protected epilogue. No freed arena access, new check/re-lock/logging/helper/exception path or own final dlclose follows. Successor durable escrow/external OldClose remain the separately closed admission implementation; SourceS2 does not fake them. Compiler-generated post-free helpers are an artifact rejection.

**Recommended release decision for review:** every nonzero/ambiguous munmap outcome is conservatively treated as uncertain arena disposition. Keep the known pin inline, NULL Owner/Api, POISONED/ARENA_RELEASE, and return no token; never dereference/retry the possibly invalid arena. A later separately justified definitely-retained failure classification is unnecessary for the first source implementation. No new arena or credit is acquired to recover. This is the original allowed uncertainty branch, not a loader-close theorem.

### 6.5 Exact remaining source decisions and integration facts

| Item | Required finite decision/fact; no runtime ordering shortcut |
| --- | --- |
| S2 native record/language method | Freeze the construction-image/memcpy object-lifetime method and actual record/zero/padding assertions; reject it explicitly if language/artifact review cannot support it. This is a source decision, not a demand to run R first. |
| Creation/control entry | Freeze which future creating-image native preparation reserves NextOwnerEpoch/image generation, genuinely owns the lock/S and installs immutable control. No such writer/caller is supplied by S1/S2. P/test authority and mapping proof remain independent. |
| Service-start outcomes | Accept the reservation/never-created/registered/joined protocol above before worker source. The real future service-entry/join call sites remain required before any invocation. No packet or guessed thread identity fills that gap. |
| Connector bounds and hooks | Full fields/quiescence are present. Exact existing numeric connector ceilings and actual creating acquire/close hooks must be supplied/frozen before implementing/wiring resource counter mutation; S2 creates no connector resources and adds no permissive counter setter. This does not block native resolver source with closed invocation. |
| Native wrappers/lowering | Freeze actual pthread/clock/mmap/getaddrinfo types and outcome flags for Source; inspect emitted code on fresh builds. Native runtime/provider/CRT/loader correspondence and synthetic-versus-native case evidence remain later gates. |

Root can approve these native source contracts and commission S2 while R/A-B/P stay CLOSED. If one row remains unsettled, report that specific source decision, continue S1, and resolve it independently; do not substitute a trusted ingress structure, no-op operation, fake join, IPv4-only path or runtime feature flag.

## 7. Preserved acceptance boundary

One normal actually rounded16KiB owner arena, exactly two slots, zero queue, eight normalized candidates, hostname255+NUL, one ConnectorLease, native creating-image cleanup, real thread retirement, inline160, census before S, zero foreign discovery pins, known S plus at most historical U terminal, blocker precedence and all frozen ABI numbers remain unchanged. No final self release, NODELETE/PIN, process registry, alternate pool or callbacks are introduced.

Linux-first source does not remove required Win64/Mac/Linux/Android/IOS release gates. Windows needs its own UCRT/WSA/handle ownership/retirement implementation; Apple discovery/retirement and Android discovery/nonblocking retirement remain separate; monolithic/merged, Shipping, fork/control replacement and abnormal exits remain CLOSED. No POSIX/GNU/private-layout shortcut is imported to them.

S1/S2 source acceptance means substantive native implementation and its exact guarded artifact can be reviewed. It is not an available socket/DNS capability, exercised eight-operation API, safe A/B load, ordinary-frame progress or production activation. Complete full-owner runtime cases/mutants, numeric routing/security/proxy/Host/CONNECT/TLS/SNI and all actual resource hooks remain required. Existing WS behavior is not changed by S1 or S2.

This task wrote only this candidate and its frozen recovery copy/manifest. No C++/tool/SDK/build/UHT/runtime/loader/network/process/Git/tracker/HGS operation or subagent occurred. Root owns tracking, current Source reconciliation, independent A review and any later finite source/build/runtime authority. No commit/push/sync is authorized.
