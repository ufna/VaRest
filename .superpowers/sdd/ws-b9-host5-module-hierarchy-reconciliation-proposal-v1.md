# Host5 module hierarchy reconciliation — bounded proposal v1

Task unreal/T-yew4ke. STATIC PROPOSAL ONLY. This does not amend the adopted contract, grant a source write, execute UBT, qualify Game availability or authorize native mode. Canonical Source320/code319, original Host/Launch sources and unexecuted normal251 remain unchanged.

## Actual failed boundary

The real fresh Source317 VaRestHost Linux Development Modular invocation returned UBT6 before exporting a graph or executing Compile/Link: Engine Launch cannot reference Project VaRestHost. UEBuildTarget.ValidateModule1624–1635 validates the references returned by GetDependencies(true,true); UEBuildModule414–428 includes public/private linked, include-only and dynamic dependencies. RulesScope.Contains38–47 permits only the same or ancestor scope. Launch's private dependency on the project is therefore incompatible with the current contract, not an unavailable optional header or a C++ body error.

Moving that dependency to PrivateIncludePathModuleNames or DynamicallyLoadedModuleNames would still be rejected. An untracked absolute project include, manually supplied VARESTHOST_API/import definition, classification change or hierarchy allowlist would hide or bypass the dependency instead of fixing it. None is proposed. The nine after payloads remain preserved in patch283960b8; Root restored every unbuilt canonical path after the STOP.

## Proposed ownership of the boundary

Introduce exactly ONE new Engine-owned header, proposed DATA-only path:

`/w/Unreal/UnrealEngine/Engine/Source/Runtime/Launch/Public/LaunchVaRestHostControl1.h`

Do not probe or create that future path under this proposal. Amend the original nine-path implementation scope to ten named paths by adding only that header. No new module, target, DSO, Core/ModuleManager/Loop edit, registry, callback installer, source-classification override or validation exception.

The header would depend only on existing Core headers: CoreTypes, Modules/ModuleManager (for FDefaultGameModuleImpl), and Templates/SharedPointer. It must not include any project/WS header or import a project symbol. All added declarations remain behind each module's identical selected-profile private VAREST_HOST_NATIVE_FRAME1 guard; no global definition is introduced.

The header declares an ordinary abstract module interface, `IVaRestLaunchHostControl1 : public FDefaultGameModuleImpl`, and an Engine-owned four-field result POD, `FVaRestLaunchHostStep1 { uint32 Size, Version, Outcome; int32 ActualStatus; }`. This is a NEW Host/Launch control boundary, not the native producer ABI or the WS revision2 interface. Proposed field values use existing operation outcome spellings0..3 and full raw status; no authority, readiness or exit certificate. Concrete size/alignment/static assertions and emitted ABI require later real compilation; no source-derived object/stack size is claimed.

Use a SINGLE inheritance chain: the selected project FVaRestHostModule derives from this interface, which already derives from FDefaultGameModuleImpl. Do not separately derive from another IModuleInterface base. Existing FDefaultGameModuleImpl supplies the original IsGameModule behavior; no override of Core's IModuleInterface is edited.

The proposed interface has seven additional virtual functions, with no exported out-of-line Engine implementation or factory:

| Function | Exact responsibility |
| --- | --- |
| `TSharedPtr<void, ESPMode::ThreadSafe> CopySessionOwner1() const` | Copy the ONE real typed Startup-created NativeSession shared pointer through the converting shared-pointer constructor; no allocation, adoption or new Session. |
| `FVaRestLaunchHostStep1 StartAfterEngineInit1()` | Call that same private Session's existing Start exactly once when actual source flow permits; copy its four result fields. |
| `FVaRestLaunchHostStep1 FrameAfterEngineTick1()` | Call the same Session's existing frame operation once at the direct post-whole-Tick edge. |
| `FVaRestLaunchHostStep1 RequestFinish1()` | Forward its existing stop/first-finish behavior, including reentrant cutoff. |
| `FVaRestLaunchHostStep1 AdvanceFinish1()` | Forward existing permitted pending progress; no first/repeated Kill shortcut. |
| `bool IsFinishRequested1() const` | Existing GT-owned source history of that same Session, not an exit permission. |
| `bool HasAttemptedStart1() const` | Existing actual issued-call history, not H/native admission. |

Concrete Host module methods implement every function. They do not take an arbitrary context pointer or turn data into an owner. The four returned fields are copied without replacing malformed/terminal statuses, resetting19 or manufacturing Complete0; the private Session's existing Observe/history/finish implementation remains the authority for those operation results. Missing owner before any issued Start is a truthful local refusal, never a discharge of an existing duty.

The opaque `TSharedPtr<void>` is ONLY a strong ownership copy of the actual typed NativeSession. Never dereference, cast, reconstruct, adopt a raw pointer or pass it back as authority. Actual SharedPointer.h817–824 copies both pointer and shared controller for convertible types;1119–1123 constrains dereference separately. The Host uses its existing typed MakeShared/control block, so deletion still belongs to the concrete Host type and requires its real code/return lifetime. No void-object allocation or TSharedRef<void> conversion is proposed. Compatibility is a bounded primary-declaration inference, not a compiler result.

All operation wrappers use the exact same private NativeSession member that CopySessionOwner1 copies. Startup creates it once; qualified flow must exclude replacement, re-Startup, unload and all module/code invalidation through the whole native interval. An opaque reference or module name does not establish those facts. Module lifetime/code/provider protection and actual same-instance association remain mandatory H obligations.

## Correct UBT direction and actual runtime dispatch

| Side | UBT/include relation | Runtime operation |
| --- | --- | --- |
| Launch Engine module | Own header + existing Core dependencies; REMOVE private Host dependency and absolute project include | GetModulePtr of the already normally loaded VaRestHost through the Engine-owned interface. No LoadModule. |
| Project Host module | Selected-profile PrivateIncludePathModuleNames Launch, to consume the Engine-owned header ONLY; retains public WS dependency | Its real module implements the seven methods and owns the existing one Session. |
| Private Session | Existing public WS types and revision2 interface, unchanged nonvirtual concrete Session implementation | Existing bind/Start/frame/stop/finish/advance and real transport/member/local pins. |

Project->Engine include-only is the permitted direction under RulesScope, and ModuleRules1261–1264 explicitly supports header access without linking. UEBuildModule607–617 uses include-only compile environment propagation. There is no Launch->Host declared dependency and no project header/symbol imported by Launch. Header-only abstract declarations are not an Engine-owned helper implementation or DSO. A future fresh full graph must independently check the complete resulting relation, any cycle and all genuine installed Game prerequisites; this proposal does not predict that graph.

GetModulePtr365–368 is a static cast of the real IModuleInterface pointer. That cast authenticates nothing. Exact deployment, the single inheritance class definition, the genuinely normally loaded module instance, virtual table/return ABI and all original H/source/code premises must be qualified before native invocation. No ABI negotiation by module name/Size/Version or fake module/context/plan is introduced.

## Launch lexical interval and retained duty

After whole successful EngineInit and all original physical GT/real-mode/nonembedded/no-exit guards, Launch looks up the borrowed module interface and copies its real Session owner. A null module/owner refuses before any Start. The copied owner is declared BEFORE the cleanup guard; the same lexical owner and protected borrowed module remain available through every call, pending control turn, cleanup backstop and full return. Do not substitute late rediscovery or a global slot.

The actual Launch driver encloses the Start call before dispatch. It calls the module's frame operation immediately AFTER the whole EngineTick returns, then retains the existing active/closing/first-finish/advance logic. The Host module wrappers add no delegate/ticker/timer/catch-up poll. Native mode OFF performs no lookup/copy/control/native dispatch; the selected Startup metadata allocation remains real. Original WS terminal19, malformed results, partial Start, reentrant cutoff, false/unknown joins, full Enter/poll/Leave/return and WS-complete/global-unestablished retention remain intact.

The cleanup guard cannot return or call EngineExit with issued duty. Holding an opaque owner does not authorize engine teardown. Source-H/global-exit/late DNS/producer O/Core/provider/allocator/whole4608-frame prerequisites remain OPEN. In particular, unchanged BeginExitIfRequested and benchmark/time-limit RequestExit within Tick still need actual exclusion/domination. No Loop/Core change or runtime bypass is added.

Existing callbacks are not equivalent implementations: OnFEngineLoopInitComplete is inside FEngineLoop::Init at4951, with actual work until return4975. OnEndFrame is inside Tick at6127, before subsequent frame-counter/render/stat work. Moving the driver there would lose the required full-return edge; PreExit/Shutdown callbacks cannot supply a nonreturning normal-flow veto. This proposal preserves the original edge instead of weakening it.

## Explicit amendment and qualification needed

Before implementation, a NEW bounded owner decision must replace the old contract's mandatory Launch PRIVATE Host dependency/project-private include/free Host Acquire with the Engine-owned header/interface lookup described here. Permit the one new header and seven module virtuals/Host wrappers, including the new Engine result type and opaque ownership copy. Preserve the original nine source paths, exact target predicate and all other restrictions. This text is not that decision.

Additional actual costs include seven module virtual slots, wrapper/result-copy frames, opaque shared-reference copies/releases and concrete control-block destruction/return paths. Session remains nonvirtual with its original actual state/operations; no extra Session, control block, thread, queue, native allocation, registry or polling is proposed. Do not claim zero cost, reuse old object/stack measurements or subtract these effects from the full simultaneous frame budget.

After an authorized implementation and independent source reviews: fresh FULL Game graph, real compile/link closure and private-C Game DATA binding; STOP on missing prerequisites, unexpected dependency or ungranted Engine/PCH work. Then actual module/symbol/import/virtual ABI, same-owner/lifetime/full-return/stack/provider/H qualification. Native flag remains uninvoked until all those gates pass. No prediction of binary bytes, native behavior, successful exit or release acceptance.

The separate Editor restoration proposal f9aaf4d1 still requires its own explicit permission; this static architectural proposal does not authorize normal251 or any SDK/native operation. Current UTF8 vectors remain unbuilt/unrun. Goal and acceptance scope remain unchanged.
