// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "CoreGlobals.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Modules/ModuleManager.h"
#include "UObject/UObjectGlobals.h"
#include "VaRestInspector.h"
#include "VaRestInspectorPolicy.h"
#include "VaRestInspectorSettings.h"
#include "VaRestTrace.h"
#if VAREST_WITH_INSPECTOR
#include "VaRestInspectorService.h"
#endif

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private::ModuleTesting
{
struct FModuleObservations { int32 Calls=0,Records=0; bool PartialPinned=false; };
struct FPublicPolicyState
{
 FPublicPolicyState(FAutomationTestBase& T,IVaRestInspector& I,TSharedRef<FModuleObservations,ESPMode::ThreadSafe> O,FDelegateHandle H)
  :Test(T),Inspector(I),InitialObserved(MoveTemp(O)),InitialObserver(H) {}
 FAutomationTestBase& Test; IVaRestInspector& Inspector;
 TSharedRef<FModuleObservations,ESPMode::ThreadSafe> InitialObserved;
 FDelegateHandle InitialObserver; int32 InitialCalls=0;
 UVaRestInspectorSettings* Settings=nullptr;
 FVaRestInspectorLimits OriginalEditor,OriginalPackaged;
 bool OriginalHeaders=false,OriginalBodies=false,OriginalCapture=false,Saved=false,Cleaned=false;
 TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
 TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Held;
 FVaRestTraceStarted Started; FDelegateHandle Contributor,Observer;
 uint64 InitialFrame=0; uint32 Polls=0; int32 Calls=0;
 bool DeferredStable=false;
 int64 RequestedStore=0;
 ~FPublicPolicyState() { Cleanup(); }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  Inspector.OnChanged().Remove(InitialObserver); Inspector.OnChanged().Remove(Observer);
  if (!Saved) return;
  FVaRestTrace::OnBuildSnapshot().Remove(Contributor);
  Held.Reset(); Inspector.SetCapture(false);
  Settings->Editor=OriginalEditor; Settings->Packaged=OriginalPackaged;
  Settings->bCaptureHeaders=OriginalHeaders; Settings->bCaptureBodies=OriginalBodies;
  // No retained test owner remains. Explicit Clear restores original account
  // capacities before restoring the previous registry and requested capture.
  Inspector.Clear(); Isolation.Reset(); Inspector.SetCapture(OriginalCapture);
 }
 FVaRestTraceDetailSettings Detail() const
 {
  const auto Sinks=FVaRestTrace::GetSinks();
  Test.TestEqual(TEXT("public policy uses exactly one actual generation sink"),Sinks?Sinks->Num():0,1);
  return Sinks && Sinks->Num()==1 ? (*Sinks)[0]->GetDetail() : FVaRestTraceDetailSettings{};
 }
 bool Begin(const TSharedRef<FPublicPolicyState,ESPMode::ThreadSafe>& Self)
 {
  Settings=GetMutableDefault<UVaRestInspectorSettings>();
  if (!Test.TestNotNull(TEXT("public settings fixture has actual CDO"),Settings)) return false;
  OriginalEditor=Settings->Editor; OriginalPackaged=Settings->Packaged;
  OriginalHeaders=Settings->bCaptureHeaders; OriginalBodies=Settings->bCaptureBodies;
  OriginalCapture=Inspector.IsCapturing(); InitialCalls=InitialObserved->Calls; Saved=true;
  Inspector.SetCapture(false); Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
  Started.RequestId=FVaRestTrace::NewRequestId(); Started.ClientId=FVaRestTrace::NewClientId();
  Started.Verb=TEXT("GET"); Started.Url=TEXT("https://capture.invalid/public-policy");
  Started.StartedAt=10;
  TWeakPtr<FPublicPolicyState,ESPMode::ThreadSafe> Weak=Self;
  Contributor=FVaRestTrace::OnBuildSnapshot().AddLambda([Weak](FVaRestTraceSnapshot& Out) {
   if (auto S=Weak.Pin()) Out.InFlight.Add(S->Started);
  });
  Inspector.SetCapture(true);
  if (!Test.TestTrue(TEXT("public settings baseline genuinely captures"),Inspector.IsCapturing()) ||
   !Test.TestTrue(TEXT("public settings holds genuine immutable reader"),Inspector.TrySnapshot(Held))) return false;
  const int64 OldCap=Held->GetDiagnostics().EffectiveStoreLimit;
  auto& Requested=GIsEditor?Settings->Editor:Settings->Packaged;
  Requested.BudgetMiB=OldCap>1024ll*1024 ? int32(OldCap/(2*1024ll*1024)) : 2;
  Requested.MaxRecords=FMath::Max(512,Requested.MaxRecords/2);
  Requested.MaxPendingEvents=FMath::Max(512,Requested.MaxPendingEvents/2);
  Requested.MaxPendingMiB=FMath::Max(1,Requested.MaxPendingMiB/2);
  Requested.MaxBodyBytes=37; Requested.MaxBinaryExcerptBytes=19;
  Settings->bCaptureHeaders=false; Settings->bCaptureBodies=true;
  RequestedStore=NormalizeLimits(Requested).StoreBytes;
  Inspector.SetCapture(false); Inspector.SetCapture(true);
  const auto D=Detail();
  Test.TestEqual(TEXT("public restart adopts actual BodyCap"),D.BodyCap,37);
  Test.TestEqual(TEXT("public restart adopts actual BinaryCap"),D.BinaryCap,19);
  Test.TestFalse(TEXT("public restart adopts actual header flag"),EnumHasAnyFlags(D.Flags,EVaRestTraceDetail::Headers));
  Test.TestTrue(TEXT("public restart retains actual body flag"),EnumHasAnyFlags(D.Flags,EVaRestTraceDetail::Bodies));
  Inspector.Clear();
  {
   TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> View;
   if (!Test.TestTrue(TEXT("public pending-limit snapshot admitted"),Inspector.TrySnapshot(View))) return false;
   const auto Diag=View->GetDiagnostics();
   Test.TestTrue(TEXT("public capacity changes visibly pending while reader retained"),Diag.bPendingLimits);
   Test.TestEqual(TEXT("public pending change keeps actual old account cap"),Diag.EffectiveStoreLimit,OldCap);
   Test.TestEqual(TEXT("public requested cap comes from changed normalized CDO"),Diag.RequestedStoreLimit,RequestedStore);
  }
  Held.Reset(); Inspector.Clear();
  {
   TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> View;
   if (!Test.TestTrue(TEXT("public post-Clear snapshot admitted"),Inspector.TrySnapshot(View))) return false;
   Test.TestFalse(TEXT("public explicit Clear commits after reader release"),View->GetDiagnostics().bPendingLimits);
   Test.TestEqual(TEXT("public committed account cap matches requested CDO"),View->GetDiagnostics().EffectiveStoreLimit,RequestedStore);
  }
  Observer=Inspector.OnChanged().AddLambda([Weak](uint64) {
   if (auto S=Weak.Pin())
   {
    ++S->Calls;
    if (S->Calls!=1) return;
    auto& P=GIsEditor?S->Settings->Editor:S->Settings->Packaged;
    P.MaxBodyBytes=73; P.MaxBinaryExcerptBytes=41; S->Settings->bCaptureHeaders=true;
    const auto Before=S->Detail();
    // Both operations are real public controls issued from the actual public
    // observer frame. The full policy must survive until deferred Clear/Start.
    S->Inspector.Clear(); S->Inspector.SetCapture(true);
    const auto During=S->Detail();
    S->DeferredStable=Before.BodyCap==37 && During.BodyCap==37 && S->Inspector.IsCapturing();
   }
  });
  InitialFrame=GFrameCounter;
  Test.TestEqual(TEXT("public Changed is never synchronous with control calls"),Calls,0);
  return true;
 }
};
class FCheckPublicPolicy final:public IAutomationLatentCommand
{
public:
 explicit FCheckPublicPolicy(TSharedRef<FPublicPolicyState,ESPMode::ThreadSafe> S):State(MoveTemp(S)) {}
 bool Update() override
 {
  if (!Started)
  {
   Started=true; if (!State->Begin(State)) { State->Cleanup(); return true; }
   return false;
  }
  if ((State->Calls==0 || GFrameCounter<=State->InitialFrame) && ++State->Polls<128) return false;
  auto& T=State->Test;
  T.TestTrue(TEXT("public OnChanged actually delivered across a normal engine frame"),State->Calls>0 && GFrameCounter>State->InitialFrame);
  T.TestTrue(TEXT("pre-first-use public observer actually receives later normal-frame notification"),State->InitialObserved->Calls>State->InitialCalls);
  T.TestTrue(TEXT("public observer controls preserve current generation until return"),State->DeferredStable);
  const auto D=State->Detail();
  T.TestEqual(TEXT("deferred public Start retains requested BodyCap"),D.BodyCap,73);
  T.TestEqual(TEXT("deferred public Start retains requested BinaryCap"),D.BinaryCap,41);
  T.TestTrue(TEXT("deferred public Start retains requested header flag"),EnumHasAnyFlags(D.Flags,EVaRestTraceDetail::Headers));
  State->Cleanup(); return true;
 }
private:
 TSharedRef<FPublicPolicyState,ESPMode::ThreadSafe> State; bool Started=false;
};
}
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorModuleTest, "VaRest.Inspector.Module",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestInspectorModuleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("Inspector is loaded"), FModuleManager::Get().IsModuleLoaded(TEXT("VaRestInspector")));
	IVaRestInspector* Inspector = IVaRestInspector::TryGet();
	if (!TestNotNull(TEXT("TryGet finds the loaded Inspector"), Inspector))
		return false;
	TestTrue(TEXT("Get returns the same module"), &IVaRestInspector::Get() == Inspector);
	TestFalse(TEXT("dynamic reloading is disabled"), Inspector->SupportsDynamicReloading());
	TestEqual(TEXT("compiled-in flag matches the macro"), IVaRestInspector::IsCompiledIn(), VAREST_WITH_INSPECTOR != 0);

	const UVaRestInspectorSettings* Settings = GetDefault<UVaRestInspectorSettings>();
	if (!TestNotNull(TEXT("settings exist"), Settings))
		return false;
	TestEqual(TEXT("settings category"), Settings->GetCategoryName(), FName(TEXT("Plugins")));
	TestEqual(TEXT("settings section"), Settings->GetSectionName(), FName(TEXT("VaRestInspector")));
	TestTrue(TEXT("editor capture default"), Settings->bCaptureInEditor);
	TestFalse(TEXT("packaged capture default"), Settings->bCaptureInPackaged);
	TestTrue(TEXT("headers default"), Settings->bCaptureHeaders);
	TestTrue(TEXT("bodies default"), Settings->bCaptureBodies);
	TestTrue(TEXT("engine IO frames hidden by default (future I3/I4)"), Settings->bHideEngineIoFrames);
	TestEqual(TEXT("editor budget"), Settings->Editor.BudgetMiB, 64);
	TestEqual(TEXT("editor records"), Settings->Editor.MaxRecords, 10000);
	TestEqual(TEXT("editor body cap"), Settings->Editor.MaxBodyBytes, 65536);
	TestEqual(TEXT("editor binary cap"), Settings->Editor.MaxBinaryExcerptBytes, 0);
	TestEqual(TEXT("editor frame cap"), Settings->Editor.MaxFrameExcerptBytes, 4096);
	TestEqual(TEXT("editor frames"), Settings->Editor.MaxFramesPerSocket, 5000);
	TestEqual(TEXT("editor pending events"), Settings->Editor.MaxPendingEvents, 65536);
	TestEqual(TEXT("editor pending MiB"), Settings->Editor.MaxPendingMiB, 16);
	TestEqual(TEXT("packaged budget"), Settings->Packaged.BudgetMiB, 8);
	TestEqual(TEXT("packaged MaxRecords"), Settings->Packaged.MaxRecords, 2000);
	TestEqual(TEXT("packaged body cap"), Settings->Packaged.MaxBodyBytes, 16384);
	TestEqual(TEXT("packaged binary cap"), Settings->Packaged.MaxBinaryExcerptBytes, 0);
	TestEqual(TEXT("packaged frame cap"), Settings->Packaged.MaxFrameExcerptBytes, 1024);
	TestEqual(TEXT("packaged frames"), Settings->Packaged.MaxFramesPerSocket, 1000);
	TestEqual(TEXT("packaged pending events"), Settings->Packaged.MaxPendingEvents, 16384);
	TestEqual(TEXT("packaged pending MiB"), Settings->Packaged.MaxPendingMiB, 4);

	const auto Limits = VaRest::Inspector::Private::NormalizeLimits(Settings->Packaged);
	TestEqual(TEXT("normalized packaged byte budget"), Limits.StoreBytes, 8ll * 1024 * 1024);
	TestEqual(TEXT("normalization preserves records"), Limits.MaxRecords, Settings->Packaged.MaxRecords);
	FVaRestInspectorLimits Invalid;
	Invalid.BudgetMiB = MIN_int32;
	Invalid.MaxRecords = MAX_int32;
	Invalid.MaxBodyBytes = -1;
	Invalid.MaxBinaryExcerptBytes = MAX_int32;
	Invalid.MaxFrameExcerptBytes = MAX_int32;
	Invalid.MaxFramesPerSocket = 0;
	Invalid.MaxPendingEvents = 0;
	Invalid.MaxPendingMiB = MAX_int32;
	const auto Bounded = VaRest::Inspector::Private::NormalizeLimits(Invalid);
	TestEqual(TEXT("budget minimum"), Bounded.StoreBytes, 1024ll * 1024);
	TestEqual(TEXT("record ceiling"), Bounded.MaxRecords, 65536);
	TestEqual(TEXT("negative body cap"), Bounded.BodyBytes, 0);
	TestEqual(TEXT("binary ceiling"), Bounded.BinaryBytes, 16 * 1024 * 1024);
	TestEqual(TEXT("frame ceiling"), Bounded.FrameBytes, 16 * 1024 * 1024);
	TestEqual(TEXT("frame count minimum"), Bounded.MaxFramesPerSocket, 1);
	TestEqual(TEXT("pending count minimum"), Bounded.PendingEvents, 1);
	TestEqual(TEXT("pending byte ceiling"), Bounded.PendingBytes, 256ll * 1024 * 1024);
	TestEqual(TEXT("normalization leaves input unchanged"), Invalid.BudgetMiB, MIN_int32);
	Invalid.MaxPendingMiB = 0;
	TestEqual(TEXT("pending byte minimum"), VaRest::Inspector::Private::NormalizeLimits(Invalid).PendingBytes, 64ll * 1024);
	VaRest::Inspector::Private::FCapturePolicy Policy;
	Policy.Limits = Limits;
	const auto Detail = Policy.Detail();
	TestTrue(TEXT("metadata requested"), EnumHasAnyFlags(Detail.Flags, EVaRestTraceDetail::Metadata));
	TestEqual(TEXT("I1 requests no socket frames"), Detail.FrameCap, 0);
	const auto Support = VaRest::Inspector::Private::GetSupportedAllocator();
	TestFalse(TEXT("bootstrap capability remains uncalibrated"), Support.bCalibrationPassed);
	TestFalse(TEXT("uncalibrated capability refuses controllers"), Support.SupportsController(64, 8));

	TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Snapshot;
	int32 Records = 0;
#if VAREST_WITH_INSPECTOR
	using namespace VaRest::Inspector::Private;
	// These are value parser cases. The live process command line is immutable.
	TestTrue(TEXT("interactive policy default"), FService::InitialCapturePolicy(true, false, true, false, TEXT("")));
	TestFalse(TEXT("unattended policy default"), FService::InitialCapturePolicy(true, true, true, false, TEXT("")));
	TestFalse(TEXT("packaged policy default"), FService::InitialCapturePolicy(false, false, true, false, TEXT("")));
	TestFalse(TEXT("explicit policy zero"), FService::InitialCapturePolicy(true, false, true, true, TEXT("-VaRestInspector=0")));
	TestTrue(TEXT("explicit policy one"), FService::InitialCapturePolicy(false, true, false, false, TEXT("-VaRestInspector")));
	FVaRestInspectorChanged* const ChangedIdentity = &Inspector->OnChanged();
	auto Observed = MakeShared<ModuleTesting::FModuleObservations, ESPMode::ThreadSafe>();
	const auto ObserverHandle = Inspector->OnChanged().AddLambda([Observed](uint64) { ++Observed->Calls; });
 bool ObserverTransferred=false;
	const bool WasCapturing = Inspector->IsCapturing();
	const bool NeverStarted = !WasCapturing && Inspector->GetRevision() == 0;
	if (NeverStarted)
	{
		Snapshot = MakeShared<FVaRestInspectorSnapshot, ESPMode::ThreadSafe>();
		const auto* Sentinel = Snapshot.Get();
		TestFalse(TEXT("meaningful compiled-in initial-off snapshot refuses"), Inspector->TrySnapshot(Snapshot));
		TestTrue(TEXT("initial-off refusal preserves nonnull Out"), Snapshot.Get() == Sentinel);
		Snapshot.Reset();
		TestTrue(TEXT("initial-off enumeration succeeds"), Inspector->ForEachRecord([&Records](const auto&) { ++Records; }));
		TestEqual(TEXT("initial-off has no records"), Records, 0);
		Inspector->Clear();
		TestEqual(TEXT("initial-off Clear does not materialize a root"), Inspector->GetRevision(), uint64(0));
	}
	Inspector->SetCapture(false);
	// Restore requested global capture AFTER the nested registry isolation ends.
	ON_SCOPE_EXIT { Snapshot.Reset(); if (!ObserverTransferred) Inspector->OnChanged().Remove(ObserverHandle); Inspector->SetCapture(WasCapturing); };
	{
		FVaRestTrace::FScopedSinkIsolation Isolation;
		struct FSource { FVaRestTraceStarted Started; bool Live = true; };
		auto Source = MakeShared<FSource, ESPMode::ThreadSafe>();
		Source->Started.RequestId = FVaRestTrace::NewRequestId();
		Source->Started.ClientId = FVaRestTrace::NewClientId();
		Source->Started.Verb = TEXT("GET"); Source->Started.Url = TEXT("https://capture.invalid/module");
		Source->Started.Flags = EVaRestTraceRequestFlags::Partial;
		const FDelegateHandle Contributor = FVaRestTrace::OnBuildSnapshot().AddLambda([Source](FVaRestTraceSnapshot& Out) {
			if (Source->Live) Out.InFlight.Add(Source->Started);
		});
		ON_SCOPE_EXIT { Snapshot.Reset(); Inspector->SetCapture(false); FVaRestTrace::OnBuildSnapshot().Remove(Contributor); };
		Inspector->SetCapture(true); // Same fresh checked first-use route as ordinary product use.
		if (!TestTrue(TEXT("compiled-in qualified service really captures"), Inspector->IsCapturing())) return false;
		TestTrue(TEXT("Changed identity survives lazy first use"), &Inspector->OnChanged() == ChangedIdentity);
		TestTrue(TEXT("pre-first-use observer binding survives lazy service construction"), Inspector->OnChanged().IsBound());
		const auto Active = FVaRestTrace::GetSinks();
		TestEqual(TEXT("real service registers one generation sink"), Active ? Active->Num() : 0, 1);
		const uint64 StartedRevision = Inspector->GetRevision();
		TestTrue(TEXT("real attach advances revision"), StartedRevision > 0);
		Observed->Records = 0;
		TestTrue(TEXT("real facade enumeration succeeds"), Inspector->ForEachRecord([Source, Observed](const auto& Row) {
			if (Row.RequestId != Source->Started.RequestId) return;
			++Observed->Records; Observed->PartialPinned = Row.bPartial && Row.bPinned;
		}));
		TestEqual(TEXT("real facade returns actual attached identity"), Observed->Records, 1);
		TestTrue(TEXT("synchronous late attach is Partial/pinned"), Observed->PartialPinned);
		if (!TestTrue(TEXT("qualified immutable snapshot succeeds"), Inspector->TrySnapshot(Snapshot)) || !TestTrue(TEXT("real snapshot exists"), Snapshot.IsValid())) return false;
		const auto& D = Snapshot->GetDiagnostics();
		TestTrue(TEXT("production snapshot/store backing is charged"), D.StoreBytes > 0 && D.StoreBytes <= D.EffectiveStoreLimit);
		TestTrue(TEXT("production common pending backing is charged"), D.PendingBytes > 0 && D.PendingBytes <= D.PendingPeakBytes);
		TestTrue(TEXT("snapshot identifies live capture"), D.bCapturing);
		const uint64 ImmutableRevision = Snapshot->GetRevision();
		Inspector->Clear();
		TestTrue(TEXT("real Clear advances revision"), Inspector->GetRevision() > StartedRevision);
		TestEqual(TEXT("held snapshot is immutable across Clear"), Snapshot->GetRevision(), ImmutableRevision);
		Snapshot.Reset();
		Inspector->SetCapture(false); Inspector->SetCapture(false);
		TestFalse(TEXT("real Pause is idempotent"), Inspector->IsCapturing());
		Source->Live = false;
		Inspector->SetCapture(true);
		TestTrue(TEXT("real Resume creates a new capture generation"), Inspector->IsCapturing());
		TestTrue(TEXT("Changed identity survives Clear/Pause/Resume"), &Inspector->OnChanged() == ChangedIdentity);
	}
#else
	const auto Before = FVaRestTrace::GetSinks();
	const int32 SinkCount = Before ? Before->Num() : 0;
	TestFalse(TEXT("compiled-out shell is inactive"), Inspector->IsCapturing());
	Inspector->SetCapture(true); Inspector->Clear(); Inspector->SetCapture(false);
	TestFalse(TEXT("compiled-out controls remain inactive"), Inspector->IsCapturing());
	TestEqual(TEXT("compiled-out revision"), Inspector->GetRevision(), uint64(0));
	TestFalse(TEXT("compiled-out changed delegate empty"), Inspector->OnChanged().IsBound());
	TestTrue(TEXT("compiled-out enumeration succeeds"), Inspector->ForEachRecord([&Records](const FVaRestInspectorRecordView&) { ++Records; }));
	TestEqual(TEXT("compiled-out facade has no records"), Records, 0);
	if (!TestTrue(TEXT("empty snapshot succeeds"), Inspector->TrySnapshot(Snapshot)) ||
		!TestTrue(TEXT("snapshot exists"), Snapshot.IsValid()))
		return false;
	TestEqual(TEXT("snapshot revision"), Snapshot->GetRevision(), uint64(0));
	Snapshot->ForEachRecord([&Records](const FVaRestInspectorRecordView&) { ++Records; });
	int32 Stats = 0;
	Snapshot->ForEachStats([&Stats](const FVaRestInspectorHttpStats&) { ++Stats; });
	TestEqual(TEXT("no snapshot records"), Records, 0);
	TestEqual(TEXT("no snapshot stats"), Stats, 0);
	TestEqual(TEXT("no links"), Snapshot->GetLinks().Num(), 0);
	const auto& Diagnostics = Snapshot->GetDiagnostics();
	TestTrue(TEXT("snapshot diagnostics are stable"), &Diagnostics == &Snapshot->GetDiagnostics());
	TestEqual(TEXT("no dropped events"), Diagnostics.DroppedEvents, uint64(0));
	TestEqual(TEXT("no dropped bytes"), Diagnostics.DroppedBytes, uint64(0));
	TestEqual(TEXT("no refused rows"), Diagnostics.RefusedRows, uint64(0));
	TestEqual(TEXT("no omitted metadata"), Diagnostics.MetadataOmitted, uint64(0));
	TestEqual(TEXT("no allocator refusals before capture"), Diagnostics.UnsupportedAllocator, uint64(0));
	TestEqual(TEXT("no interrupted rows"), Diagnostics.InterruptedRows, uint64(0));
	TestEqual(TEXT("no store allocation"), Diagnostics.StoreBytes, int64(0));
	TestEqual(TEXT("no store peak"), Diagnostics.StorePeakBytes, int64(0));
	TestEqual(TEXT("no pending peak"), Diagnostics.PendingPeakBytes, int64(0));
	TestEqual(TEXT("no effective limit before capture"), Diagnostics.EffectiveStoreLimit, int64(0));
	TestEqual(TEXT("no requested limit before capture"), Diagnostics.RequestedStoreLimit, int64(0));
	TestEqual(TEXT("no pending allocation"), Diagnostics.PendingBytes, int64(0));
	TestEqual(TEXT("no snapshot backing controllers"), Diagnostics.SnapshotControllers, 0);
	TestFalse(TEXT("diagnostics inactive"), Diagnostics.bCapturing);
	TestFalse(TEXT("no pending limits"), Diagnostics.bPendingLimits);
	const auto After = FVaRestTrace::GetSinks();
	TestEqual(TEXT("compiled-out controls and snapshot register no sink"), After ? After->Num() : 0, SinkCount);
#endif
#if VAREST_WITH_INSPECTOR
 AddCommand(new VaRest::Inspector::Private::ModuleTesting::FCheckPublicPolicy(
  MakeShared<VaRest::Inspector::Private::ModuleTesting::FPublicPolicyState,ESPMode::ThreadSafe>(*this,*Inspector,Observed,ObserverHandle)));
 ObserverTransferred=true;
#endif
	TestNull(TEXT("capture console object is absent"), IConsoleManager::Get().FindConsoleObject(TEXT("VaRest.Inspector.Capture")));
	return true;
}

#endif
