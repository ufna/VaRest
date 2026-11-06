# Native owner S3 — hidden stop, pump and genuine-join bookkeeping

2026-11-06 · VaRest-81s.21.14 · PLAN ONLY; independent review, Root acceptance and a separate exact source lease required

## 1. Bounded decision and code prelude

Select a dormant S3 source tranche implementing the already-frozen private stop/pump/deferred responsibilities. No consumer, delegate, entry bridge, binder, admission path, resource counter setter, API/table/export or production call route is added. All new external-to-implementation callers remain absent. The full public eight-operation ABI stays unchanged; bootstrap owner/API publication remains unreachable until separately accepted actual admission. Source authoring need not wait for R/A-B/P runtime.

Binding inputs: escrow2 design `0fec35b74f8ca6e9de16b9d7a56aec6c4bb4f0b136b6984e2357fbb022648872`; proof `0dbe5dadb019aeaba9b927a9b63349e20e5f6a48f4ca2f78cb5eff0abed549c2`; v2 `2a60a6754c8713e833537e6b889b70bc9deca424e80530efc9e7468f23feec31`; accepted S2 contract `2a2ca7b9e495948ed2e85f9bf7d6f1618de13e2f4784bb56e5d3073741a91268`; accepted source-site map `d45cd378d5598272133335b086fbaf6e1c4d4f1dc111da460a097b5d4136e9a3`. Immutable S1 types are the historical layout input, not an assertion about B's final S2. No mutable staged S2 source was read while authoring.

This contract resolves one explicit S2 extension: a genuine service-join observation must become durable and invalidate service identity **before** its one descriptor-lock attempt can return BUSY. Canonical join/cleanup can then reconcile later under an existing lock. It does not reinterpret a caller flag as an actual join. All original arena/pin/slot/epoch/resource limits and actual caller/control/lifetime prerequisites remain.

Before S3 code commission, Root must freeze/review actual final S2 delivery, release its lease, reconcile Source and create a new immutable `native-owner-source-s3-001/input.json` with that accepted S2 output-manifest path/full hash/version, this contract hash/decision and exact before size/hash for each leased file. Semantic mismatch with actual S2 requires a reviewed revision; field/helper spelling may be mapped in that finite input without changing this contract. No partial staging bytes are baseline authority, and this document grants no source/build/runtime permission.

The later received immutable S2 delivery manifest is `7fd03a352315387452a8aebe91c07b9f154f953afc2cfb8d6b786c07a91f6163`, source outputs `00632e80bdc89e01f77991c97fdc2decdb27d1197dd24e20be33d4dcdbde6560`; independent A source review is `1519ef4ba76a21b1e3381d08a84debe53771cecda0e6c477436507ed66be9517`. These name frozen source-only evidence, not Root acceptance or actual compiled behavior. The pre-commission decision/pins above remain required.

## 2. Exact prospective paths and surface

All seven prospective files are in `Source/VaRestNativeOwner/Private/NativeOwner/`:

| File | Bounded S3 purpose |
| --- | --- |
| `VaRestNativeOwnerInternal.h` | Append the exact32-byte passive lease mirror below; atomic/layout assertions and same-image declarations only. |
| `VaRestNativeOwner.cpp` | Constant initialization and truthful private mirror framing/pristine checks in the real getter. No arena read, native role call, table read or new export. |
| `VaRestNativeOwnerState.h` | Exact hidden seam declarations and fixed mirror/join semantics; public/slot/owner layout stays otherwise S2. |
| `VaRestNativeOwnerState.cpp` | Real stop/pump/deferred bodies; live-lease ingress guard; attach/detach/poison mirror stores; genuine-join publication and locked reconciliation; role/snapshot updates. |
| `VaRestNativeResolver.cpp` | Existing submit/poll guards honor the join tombstone; poll recognizes newly published Stop/join between observations and reconciles using its existing lock. No new worker/queue/resource architecture. |
| `VaRestNativeOwnerArena.cpp` | Constructor/retire require empty mirror when no lease; retire reconciles genuine join under its existing lock and refuses live mirror/executing pump before any free. Preserve S2 construction/irreversible interval. |
| `VaRestNativeOwnerProducer.h` | Only variant literal/comment and its exact32 digest bytes. |

No Build/module/public header/.uplugin/consumer/test/tool/SDK/generated/config path is selected. GNU Linux x86_64 excluding Android only; other platforms remain closed. Keep local NoPCH/nonunity/Core-only and no consumer imports. Native bodies are substantive hidden locally bound code, retained for compilation with `[[gnu::used]]`; final emission remains an actual artifact gate. No flags/noinline/safeguard workaround or fake passing test. S3 identity literal `VaRest/native-owner/abi2/VaRestNativeOwner/source-s3-bookkeeping/linux-x86_64/2026-11-06` hashes to `c8b46955ecbd12b6f1b4e253fbb100ea4d771db34e45d4e647366b571df17ff2`.

Exact private signatures, unchanged from frozen design:

```c
void NativeRequestStop(VrOwner*, VrSession);
VrStatus NativePumpRegistered(VrOwner*, VrSession, uint32_t registered);
VrStatus NativePumpEnter(VrOwner*, VrSession);
void NativePumpLeave(VrOwner*, VrSession);
void NativeRequestDetachAfterJoin(VrOwner*, VrSession);
```

They are not table members or general ingress. S2's hidden `NativeServiceJoinObserved(VrOwner*,VrSession)` remains VrStatus-returning and makes at most one descriptor-lock CAS; S3 adds durable fact publication before that attempt. Other native outcome/API signatures remain unchanged. No seam creates a thread, joins one, closes a resource, allocates, installs a delegate, polls or releases a loader credit. Genuine external service join is a prerequisite at the actual future creating/join boundary.

## 3. Fixed passive mirror, atomics and lifetime

Append one private `FNativeLeaseIngress3` at descriptor offset256, alignment8:

| Field | Descriptor offset / exact native type |
| --- | --- |
| Owner | 256 / VrOwner* |
| OwnerEpoch | 264 / uint64 |
| ServiceEpoch | 272 / uint64,0 means absent |
| Poisoned | 280 / uint32,0 or1 |
| Reserved | 284 / uint32,always0 |

Mirror size32; descriptor becomes288/alignment8. Existing S2 fields/journal at232..255 and inline160 remain unchanged. All mirror fields are constant-initialized zero/null and **every** read/write uses aligned compiler atomic operations, including payload under the descriptor lock. Assert actual pointer/uint64/uint32 lock-free lowering and offsets. A sequence counter around non-atomic payload is forbidden; no spin/retry loop is introduced. No arena/slot field, public reserved byte or unused1280 census scratch byte is borrowed. The mirror costs32 fixed loader-provided bytes, no extra arena/pool/reference and no callback pointer.

The mirror is a bounded bookkeeping copy of the real current leased owner/session. It is not authority, a pin, an arena validity proof or a stand-alone hazard-pointer protocol. Before any arena dereference, a legitimate call already has continuously protected producer/arena code, an actual live lease and same actual GT serialization with owner creation/retirement. The creating lock/ledger enforces that an entered pump keeps the lease/arena alive. Copied pointer/epochs alone cannot establish those premises. Arbitrary calls, control replacement/fork through an active invocation, signal reentry and direct stale-pointer ingress have no claimed recovery.

Only successful native attach publishes a live mirror, under its existing descriptor lock after all fallible guards/epoch reservation and complete lease initialization: atomic-store Owner/OwnerEpoch, Poisoned0/Reserved0, then release-store ServiceEpoch last. The record exactly matches the creating Owner, nonzero owner epoch and the one leased service epoch. No caller publishes a mirror or chooses identities. Same-image reuse does not allocate/reset it. Live epoch/payload remains unchanged while that lease exists; only Poisoned may become1 concurrently through known-live-owner poison. Poison publication release-stores mirror Poisoned1 before exposing the corresponding live poison transition. It is never reset on a live poisoned lease.

Successful known-safe detach may close the mirror only after service outcome/resources/pump/execution are truly complete: release-store ServiceEpoch0 first, then atomic-clear remaining payload. It clears no executing pump. No delegate/OS/import/callback occurs between those publication stores on GT. It then clears completed native service/lease records under the same existing lock and publishes DORMANT, retaining owner/O/slots/generations. Actual retire/free requires mirror absent and pump executing0; terminal/uncertain arena states cannot retain a live mirror. An incomplete start, known-live poison or deferred join keeps the same live mirror/lease charged. No new attach can overwrite it.

The getter checks only passive framing and coherent absent/live mirror values under its own existing try-lock; a cold/pristine descriptor requires all mirror bytes zero. A live mirror has nonnull current Descriptor.Owner, matching owner pointer, nonzero owner/service epochs, Poisoned<=1 and Reserved0; the getter never follows that pointer or reads a lease. Mirror values add no public flags/diagnostics/API fields. It makes no pthread/getpid call. A new full descriptor288 read-set, data/constant initialization, relocation/pre-init/fini and getter instruction closure audit is required; the earlier656-byte S1 observation is not a certificate for S3. Any later S2 artifact observation is likewise specific to its own bytes.

## 4. Dedicated control/live-lease ingress guard

RequestStop, PumpRegistered, PumpEnter and RequestDetachAfterJoin use a dedicated no-lock guard, never generic ValidateOwner or full mutable bookkeeping. They do not read plain Owner.Phase, Connector counters, Service.Thread/Registered or slot payload. They do not try the descriptor lock. They may be called reentrantly on the established GT while poll owns that lock.

Guard order is fixed: validate pointer value alignment/nonzero and nonzero session shape without dereference; acquire-load ControlBound; reject unbound/unknown; validate immutable established process/control identity, comparing actual getpid before using pthread_equal on the retained **control** pthread_t; then atomically read mirror ServiceEpoch once, Owner/OwnerEpoch/Poisoned/Reserved and once more ServiceEpoch, with no retry. Require the exact current token/session and unchanged nonzero epoch before touching the arena. All payload accesses are atomic; GT-only publication/exclusion plus live-lease premises provide lifetime, not the two epoch reads alone. All native identity imports occur before the final bounded atomic read set; no logging/user callback is inserted.

ControlProcess/ControlThread become immutable after actual binding. The future still-closed binder must publish them before a release-store ControlBound1, and no path may unbind/reset/rewrite them in that mapping/process lifetime. S3 implements no binder. Acquire ControlBound observes that immutable initialization; no integer pthread cast/liveness guess or service-identity comparison is allowed in this guard. A process mismatch refuses before a potentially invalid inherited thread identity is compared.

VrStatus guard results are exact: null/misaligned token or zero session fields INVALID_INPUT; ControlBound0 UNBOUND_CONTROL; unknown framing/bits/inconsistent live mirror ABI_INCOMPATIBLE; getpid failure NATIVE_ERROR; process mismatch FORK_UNESTABLISHED; wrong established control thread WRONG_THREAD; absent/changed/nonmatching mirror/session STALE. The three void seams perform no arena access/mutation on guard failure and return no success acknowledgement; the future adapter's shipping-active GT/lifetime guard must report/refuse its violation. No native logging/diagnostic callback is added. Invalid mirror/input never authorizes a guessed cleanup. Once the guard succeeds, only the stated atomic arena fields may be touched.

## 5. Stop and pump operations

| Seam | Exact substantive action / result |
| --- | --- |
| NativeRequestStop | Dedicated guard, allowing known-live poison/stopped/joined cleanup. Release-store the exact current service epoch into Owner.StopEpoch. No lock, queue, poll, wait or cleanup. Duplicate same-session Stop is idempotent; stale old epochs cannot overwrite a new lease's stop marker because ingress validation precedes the store. Descriptor BUSY does not lose this request. |
| NativePumpRegistered(...,1) | Actual successful AddSP boundary is a precondition, before submit permit publication. Require unpoisoned/unstopped/current lease, no genuine-join fact, PumpExecuting0. Set the Boolean registration with one atomic0→1 CAS; duplicate same real registration with value1/executing0 returns OK without another delegate/count. Stop/join returns STALE; poison POISONED; executing/nested BUSY; unknown Boolean ABI_INCOMPATIBLE. This is not permission for a second delegate or generic setter. |
| NativePumpRegistered(...,0) | Actual disable/removal boundary is a precondition. Validate existing Boolean registration/execution values before release-storing registered0, allowing stopped, genuinely joined or known-live poisoned lease and executing1. Unknown existing values refuse ABI_INCOMPATIBLE rather than normalize into quiescence. It never clears executing, join fact or another resource. Duplicate removal is OK. registered>1 input is INVALID_INPUT. |
| NativePumpEnter | Dedicated guard; require no live poison, exact unstopped epoch, genuine-join fact0 and registered1. Unregistered returns OLD_OWNER_BUSY; stopped/joined STALE; poison POISONED. One atomic Executing0→1 CAS, failure1 BUSY, unknown value ABI_INCOMPATIBLE. After winning, recheck atomic registration/stop/join/poison before success; on refusal clear only its own acquired executing charge and return the corresponding status. No poll/lock/activation-flag reset occurs here. Only OK incurs one matching Leave. |

Owner.StopEpoch, PumpRegistered, PumpExecuting and DeferredDetachAfterJoin are actual aligned acquire/release atomic fields. Known live poison may race Enter; a poison after its final check is observed by the subsequently locked poll, and the entered charge remains balanced. Other full mutable fields are not read under the no-lock ingress. There is one pump Boolean/one turn, not an accumulating counter. No wrap/increment arithmetic or extra reference is introduced.

If registration bookkeeping refuses after a real AddSP succeeded, the creating adapter must disable/remove that actual handle before any detach attempt, retaining its live lease through unwind; refusal does not mean no delegate was obtained. No live atomic flag is cleared through a non-atomic whole-record byte reset while a no-lock reader or balanced invocation can remain. Mirror access is always atomic; ordinary initialization/clearing of other closed records requires the proven quiescence described here.

**NativePumpLeave is a balanced cleanup, not another general ingress.** It is permitted only after successful Enter on the SAME actually verified, pinned GT invocation/session, with no fork/control replacement and continuous producer/arena/live-lease protection through the end of Leave. The future AddSP dispatch's local strong pump reference spans the complete pair. Reentrant Stop/removal cannot destroy that protection because executing1 prevents detach/retire. These preconditions are not established by a caller ID/token packet.

Inside Leave: bounded atomic checks of the stable mirror token/epochs/shape, then exact PumpExecuting1→0 release CAS/store; no retry, descriptor lock, pthread/getpid/native import, poll, detach, resource/WSA/loader operation, allocation or callback. Do not reject balanced Leave merely because Stop, poison, removal or genuine join appeared after Enter; its executing charge must clear. Malformed/mismatched stable fields or a duplicate executing0 call makes no mutation and provides no success acknowledgement; it is outside the valid balanced pair and cannot clear another session's charge. No arena access follows the executing clear, and no reconciliation is attempted by the epilogue. Role/process/fork/lifetime checks belong before Enter; Leave deliberately makes no new OS identity call.

## 6. Genuine join publication before one lock attempt

Retain the existing uint32 DeferredDetachAfterJoin Boolean as a durable **genuine service-join fact plus deferred detach request** for the current live lease. It is not a fake joined bit or “Stop requested” indication. Only the actual creating owner AFTER confirmed native service join may publish it; neither a parser, packet, worker completion marker, deleted wrapper nor ThreadID liveness test supplies that fact. All access is atomic. Values other than0/1 are incompatible and disable service identity use; no code normalizes them into permission.

`NativeRequestDetachAfterJoin` performs the dedicated control/live-lease guard, then release-publishes fact1 with one0→1 CAS (duplicate1 is idempotent). It never takes the descriptor lock or writes canonical ServiceJoined/Registered, counters or phase. It retains the fact even while poll holds the lock. It is allowed under known live poison and does not clear it. It has zero external callers in S3; the actual join boundary remains closed integration work.

S3 changes S2 NativeServiceJoinObserved entry to invoke the same genuine-fact publisher **before** its one descriptor try-lock. If the lock is busy it returns BUSY with fact1 retained. That status means bookkeeping is pending, not that the real join failed or should be repeated. The helper itself never joins. A repeated bookkeeping call for the same still-live fact may make one new lock attempt; actual native join is never repeated. Once the lock is acquired it materializes the fact below and returns OK (also for an already-materialized matching fact), without pretending resources detached. This explicitly replaces S2's duplicate-STALE rule for this hidden outcome while that same lease/fact remains live. After lease closure or epoch change, duplicates are STALE and touch no arena.

NativeServiceNeverCreated remains mutually exclusive with genuine joined. Its existing failed lock attempt leaves the start reservation charged; no “never-created” outcome is converted into joined. The future creating adapter must retain that responsibility until its truthful outcome bookkeeping commits; S3 does not clear it from absence of registration or turn it into a joined flag. Registration refuses any nonzero join fact before deriving/storing another service identity. Contradictory known never-created/joined records poison NATIVE_INVARIANT under the later lock, retaining responsibility rather than deleting one fact.

**Immediate invalidation is normative for EVERY path:** all service/Either role guards test the atomic fact before any pthread_equal on Service.Thread. Any nonzero fact makes that service identity unusable; unknown values refuse. The canonical Registered1/Joined0 bytes may temporarily be historical bookkeeping after an actual join, so a full guard must not treat them as a live pthread or poison the valid pending state merely for being unreconciled. Bound control-role matching remains valid; no control thread is compared with the retired service identity. Submit/copy refuse the joined epoch. Query/poll snapshot service_executing uses `ServiceCallExecuting || (ServiceStarted && !ServiceJoined && fact==0)`; it does not report a known joined thread executing just because canonical stores were deferred. Existing independent execution/reservation/resource obligations remain charged and are not erased by this expression.

## 7. Reconcile under an existing lock, never from Leave

A fixed shared locked helper reads the fact acquire BEFORE service-role comparison/snapshot/quiescence. It makes no OS/thread call and no second lock attempt. For fact1 it validates the exact live owner/lease/mirror/session and materializes precisely one of these genuine outcomes:

| Canonical state | Prepared locked update |
| --- | --- |
| Registered started service, not joined | Set ServiceJoined1, Registered0; retain ServiceStarted1 and lease; start reservation already0. Never compare the historical pthread_t. |
| Real created child returned/refused before registration | Consume ServiceStartReserved1/Started0/Joined0/unregistered into Started1/Joined1/Registered0/StartReserved0; never-created remains0. No invented identity is needed. |
| Matching already joined/unregistered service | No change; fact remains1 until completed lease closure. |
| Incompatible/malformed outcome | Retain fact/responsibility, poison NATIVE_INVARIANT; no identity probe, second join, resource close or new admission. |

Genuine fact1 remains durable after materialization; do not clear it just because ServiceJoined was stored. The subsequent fixed deferred-detach reconciliation may finish only when all original conditions are true: known joined disposition, no start reservation, resource/dependency/wake counts0, pump registered/executing0, ServiceCallExecuting0, and no lease/native poison/uncertainty. It first closes the mirror, then invalidates that service's interests, frees only already-retired known-clean results, clears only completed service/lease and publishes DORMANT under that SAME lock. Running/unjoined DNS slots, O/arena and all owner/service/slot generations remain. Poison can still record its genuine join for truthful snapshots but cannot clear the lease/poison to permit recreation.

Query, attach, poll and retire perform this fixed reconciliation under their normal one acquired lock before their final snapshot/admission/quiescence decision. A query may therefore reconcile already-proven deferred bookkeeping; it does no OS cleanup. Attach may reuse only after real completed detach and must issue a fresh service epoch/mirror. NativeDetachService materializes the join fact first but leaves its requested matching detach to its own normal path, so a call that itself completes detach returns OK; a session already detached by another legitimate control operation is STALE. NativeServiceJoinObserved materializes only and acknowledges the outcome; it does not implicitly certify all resource closure. Service-role operations simply refuse the invalidated epoch before touching retired identity; they do not perform control reconciliation.

During an already-entered pump, executing1 prevents deferred detach even if a reentrant Stop actually joined the service and removed registration. The outer poll records any actual DNS retirement outcome immediately, then checks both StopEpoch and the genuine-join fact between observations/cleanup. A NEW stop/join relative to that turn's entry markers ends further work in that turn; no acquired join result is lost. A service stopped/joined before a later ordinary control call does not disable cleanup forever: later poll may reconcile and retire its known-safe DNS records without another service join. Leave only clears executing1→0; a later valid control operation reconciles under its own existing lock. No engine callback is retained to finish this bookkeeping.

Retire rechecks absent mirror, lease/pump/execution/resources/quiescence before moving O or calling munmap. It cannot free an arena while any balanced Enter still owes Leave, while a start/outcome is pending, or while genuine join fact is not reconciled into real completed detach. Construction likewise requires absent mirror and cannot manufacture one from a prepared scalar/pin tuple. The S2 journal/uncertain-free rules and post-free infallible return path stay unchanged; no S3 helper is inserted after successful arena free.

## 8. Validation, cost and retained gates

After independent acceptance and actual S2 pin freeze, a separate seven-path lease may implement S3. Required source review covers every no-lock read/write set, control/process/pointer-before-arena order, exact atomic lowering/Boolean states, busy Stop persistence, join publication before CAS, all retired-service identity uses, duplicate/pending outcomes, snapshot truth, balanced Leave and detach/retire exclusion. No synthetic live-owner call, invented real join, test enable flag or empty passing test is allowed. New bodies/full eight API stay substantive and emitted; they remain dormant and no consumer can reach them.

Every changed C++ version/target still needs a fresh Root-reviewed full graph before separately authorized plugin-only Compile/Link/receipt work; no SDK/host/engine/PCH/cache/generated/timestamp/object/global-flag workaround. Audit final descriptor288 constant/read/relocation/getter closure, hidden/local code and no consumer import/entry surface. The32-byte mirror and extra code are fixed image cost; owner logical16384, S2 native types, two slots/zero queue/eight dual-stack addresses/hostname255, one lease/owner reference, inline160 and discovery scratch caps are unchanged. No total-process memory or bounded syscall/exit latency claim follows.

Actual GT/control/caller/supplier/bridge authority, original E/P/mapping/catalogue/census/BASE/S, Core gate and module-node lifetime, genuine join/provider/CRT/kernel behavior, AddSP dispatch/lifetime integration, actual connector resource hooks/security/routing and R/A-B/P/platform/Shipping/merged/fork/abnormal-exit gates remain closed. A mirror is never deployment evidence or arbitrary-caller authentication. S3 implements no callback or grant to invoke these hidden seams. Producer update requires restart; reload does not reset owner/slots/poison. No architecture alternative, native pool, resource setter, public packet/table/export, NODELETE/PIN or future-version escape is added.

This task writes only this new canonical candidate and identical exclusive recovery copy/manifest. No C++/Build/tool/SDK/build/UHT/runtime/loader/network/process/Git/HGS/tracker mutation, Tools enumeration/key metadata access, staged S2 read or subagent occurred. Root owns task tracking, acceptance and all subsequent leases.
