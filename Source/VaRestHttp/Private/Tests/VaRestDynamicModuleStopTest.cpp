// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS
#include "VaRestModuleStopTest.h"
#include "VaRestDynamicLifecycleTestTypes.h"
#include "VaRestDynamicRequestAccess.h"
#include "VaRestDynamicBridge.h"
#include "VaRestManagedCompletion.h"
#include "HAL/PlatformTLS.h"
#include "VaRestDynamicTypes.h"
#include "VaRestClientState.h"
#include "VaRestNativeLifetimeService.h"
#include "VaRestNativeOperationStore.h"
#include "VaRestNativeScope.h"
#include "VaRestHubRuntime.h"
#include "VaRestHttpTesting.h"
#include "VaRestClock.h"
#include "VaRestTrace.h"
#include "VaRestJsonRules.h"
#include "CoreGlobals.h"
#include "Features/IModularFeatures.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Tasks/Task.h"
#include "UObject/Class.h"
#include "UObject/ObjectRedirector.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/PropertyOptional.h"
#include "UObject/UnrealType.h"

namespace VaRest::Http::ModuleStopFixture
{
using namespace VaRest::ModuleStopTest;
using FProbe=TSharedPtr<INativeProbe,ESPMode::ThreadSafe>;
using FState=TSharedPtr<FDynamicTypedState,ESPMode::ThreadSafe>;

bool SelectedConfiguration(FString& Case,FString& Trace)
{
 if (!FParse::Param(FCommandLine::Get(),TEXT("VaRestM5ModuleStopProof")) ||
  !FParse::Value(FCommandLine::Get(),TEXT("VaRestM5StopCase="),Case) ||
  !FParse::Value(FCommandLine::Get(),TEXT("VaRestM5StopTrace="),Trace)) return false;
 return (Case==TEXT("NativeOptionalScalar") || Case==TEXT("NativeOptionalArray") ||
  Case==TEXT("NativeOptionalMap") || Case==TEXT("EnumOwnerMap")) &&
  (Trace==TEXT("None") || Trace==TEXT("Timings"));
}

// An owned real sink enables Metadata, the actual flag which selects bTimeDecode. It requests
// no bodies/headers/excerpts and owns no UObject, clock, callback or shutdown service.
class FTimingSink final : public IVaRestTraceSink
{
public:
 FVaRestTraceDetailSettings GetDetail() const override
 { FVaRestTraceDetailSettings S;S.Flags=EVaRestTraceDetail::Metadata;return S; }
 void OnAttached(const FVaRestTraceSnapshot&) override {}
 void OnEvent(const FVaRestTraceEvent&) override {}
};

// Snapshot membership only on GT. Return plain values and destroy the actual immutable sink
// list snapshot here; no native probe/worker retains the list or an extra sink owner.
struct FTraceSelectionObservation
{
 bool MembershipMatches=false,MaskMatches=false;
 int32 SinkCount=-1;
 uint32 Mask=0;
};
FTraceSelectionObservation ObserveTraceSelectionOnGT(bool Timing,const FTimingSink* OwnedSink)
{
 check(IsInGameThread());
 const auto Sinks=FVaRestTrace::GetSinks();
 FTraceSelectionObservation O;
 O.SinkCount=Sinks ? Sinks->Num() : 0;
 O.Mask=static_cast<uint32>(FVaRestTrace::Detail());
 O.MaskMatches=O.Mask==static_cast<uint32>(Timing ? EVaRestTraceDetail::Metadata : EVaRestTraceDetail::None);
 O.MembershipMatches=Timing ? (OwnedSink && O.SinkCount==1 && &(*Sinks)[0].Get()==OwnedSink) :
  (!OwnedSink && O.SinkCount==0);
 return O;
}

// This is the caller's real injected clock for each fixture client. Now follows the public
// ANY-THREAD contract and delegates to the actual atomic virtual clock; timer methods remain GT.
// No probe owns this clock. Its ordinary/managed original config/Frame keeps its genuine ownership.
class FObservedClock final : public IVaRestClock
{
public:
 explicit FObservedClock(TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> InClock) : Clock(MoveTemp(InClock)) {}
 ~FObservedClock() override { check(IsInGameThread()); }
 double Now() const override
 {
  (IsInGameThread() ? GTCalls : WorkerCalls).fetch_add(1,std::memory_order_relaxed);
  return Clock->Now();
 }
 FVaRestTimerHandle SetTimer(double Delay,TUniqueFunction<void()>&& Callback) override
 { check(IsInGameThread());return Clock->SetTimer(Delay,MoveTemp(Callback)); }
 void ClearTimer(FVaRestTimerHandle& Handle) override
 { check(IsInGameThread());Clock->ClearTimer(Handle); }
 int64 OnGT() const { return GTCalls.load(std::memory_order_acquire); }
 int64 OnWorker() const { return WorkerCalls.load(std::memory_order_acquire); }
private:
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
 mutable std::atomic<int64> GTCalls{0},WorkerCalls{0};
};

// The observer's entire ownership graph is GTK only. Native probes never retain it, its
// weak type/owner observations, completion/frame observers, clients, clocks or transports.
struct FGTObserver
{
 TUniquePtr<Testing::FTestRig> Ordinary,Managed;
 TSharedPtr<FObservedClock,ESPMode::ThreadSafe> OrdinaryClock,ManagedClock;
 TSharedPtr<FTimingSink,ESPMode::ThreadSafe> TimingSink;
 TArray<TWeakPtr<FDynamicTypedState,ESPMode::ThreadSafe>> Types;
 TArray<TSharedPtr<FDynamicCompletion,ESPMode::ThreadSafe>> Completions;
 TSharedPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Control;
 TSharedPtr<FClientAdmissionGate,ESPMode::ThreadSafe> Gate;
 FNativeOperationStore* Operations=nullptr; // accessed on GT under the captured real Control
 int32 Baseline=0;
 bool Timing=false;
 ~FGTObserver()
 {
  check(IsInGameThread());
  if (TimingSink) { FVaRestTrace::RemoveSink(TimingSink.ToSharedRef());TimingSink.Reset(); }
 }
 FGTObservation ObserveOnGT()
 {
  check(IsInGameThread()); FGTObservation O;
  // Observe actual membership/mask at real OnPreExit, before this observer removes its own
  // sink in its GT destructor. A foreign None sink must fail even when the merged mask agrees.
  const auto Trace=ObserveTraceSelectionOnGT(Timing,TimingSink.Get());
  O.TraceMembershipMatches=Trace.MembershipMatches;O.TraceMaskMatches=Trace.MaskMatches;
  O.TraceSinkCount=Trace.SinkCount;O.TraceMask=Trace.Mask;
  O.FramesStopped=Completions.Num()==2;
  for (const auto& Completion:Completions) O.FramesStopped &= Completion && Completion->IsStopped();
  O.OwnedReflectionRetired=Types.Num()==2;
  for (const auto& Weak:Types)
  {
   const auto State=Weak.Pin();
   // Inspect this actual tracked state's storage, never a copied pin-count predicate.
   // Expired also proves no reflected state was retained by the real worker.
   O.OwnedReflectionRetired &= !State || State->OwnedReflectionRetiredForStopTestOnGT();
  }
  O.Carriers=Testing::LiveDynamicCarriersOnGT(); O.CarrierBaseline=Baseline;
  O.ControlStopped=Control && Control->IsStopped();
  O.ExitRetention=Control && Control->IsExitRetentionRequested();
  O.GateClosed=Gate && Gate->IsClosed();
  O.CountedOperations=Gate ? Gate->NumOwnedOperations() : 0;
  O.OccupiedClients=Control ? Control->ClientSlotsOnGT().NumOccupied() : 0;
  O.OccupiedCells=Operations ? Operations->NumOccupied() : 0;
  O.PendingInputs=Control ? Control->NumPendingInputs() : 0;
  O.AllNativeOperationsOnGT=DynamicLifecycleTest::ObserveOnGT().WrongThread==0;
  O.OrdinaryClockWorkerCalls=OrdinaryClock ? OrdinaryClock->OnWorker() : -1;
  O.ManagedClockWorkerCalls=ManagedClock ? ManagedClock->OnWorker() : -1;
  DynamicLifecycleTest::EndObservationOnGT();
  return O; // destructor and this callback are destroyed by witness BEFORE late gate release
 }
};

struct FDestination
{
 TUniquePtr<FVaRestDynamicLifecycleDestinations> Native;
 FInstancedPropertyBag Bag;
 TStrongObjectPtr<const UPropertyBag> BagType;
 const FProperty* Property=nullptr;
 void* Address=nullptr;
 bool Taken=false,Confirmed=false;
 ~FDestination() { check(IsInGameThread()); }
 bool Initialize(EVaRestTypeContainer Container,UEnum* Enum)
 {
  if (Enum)
  {
   FPropertyBagPropertyDesc Desc(TEXT("Value"),EPropertyBagContainerType::Map,EPropertyBagPropertyType::Enum,
    Enum,CPF_Edit,EPropertyBagPropertyType::String);
   Desc.ID=FGuid(0x56a27e31,0x393a4a1c,0x80f10938,0xab123456);
   BagType.Reset(UPropertyBag::GetOrCreateFromDescs(MakeArrayView(&Desc,1)));
   if (!BagType.IsValid()) return false;
   Bag.InitializeFromBagStruct(BagType.Get());
   const auto* Found=Bag.FindPropertyDescByName(TEXT("Value"));
   Property=Found ? Found->CachedProperty : nullptr;
   Address=Property ? Property->ContainerPtrToValuePtr<void>(Bag.GetMutableValue().GetMemory()) : nullptr;
  }
  else
  {
   Native=MakeUnique<FVaRestDynamicLifecycleDestinations>();
   switch (Container)
   {
    case EVaRestTypeContainer::None:
     Property=FindFProperty<FStructProperty>(FVaRestDynamicLifecycleDestinations::StaticStruct(),GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleDestinations,Scalar));
     Address=&Native->Scalar;break;
    case EVaRestTypeContainer::Array:
     Property=FindFProperty<FArrayProperty>(FVaRestDynamicLifecycleDestinations::StaticStruct(),GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleDestinations,Array));
     Address=&Native->Array;break;
    case EVaRestTypeContainer::Map:
     Property=FindFProperty<FMapProperty>(FVaRestDynamicLifecycleDestinations::StaticStruct(),GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleDestinations,Map));
     Address=&Native->Map;break;
   }
  }
  return Property && Address;
 }
 void Prepare(FDynamicPrepareToken& Token)
 {
  check(IsInGameThread());
  Taken=!Token.IsError() && Token.Take(Property,Address);
  Confirmed=Taken && Token.ConfirmPrepared();
 }
 bool Verify(EVaRestTypeContainer Container,const UObject* Owner)
 {
  check(IsInGameThread());
  if (!Taken || !Confirmed) return false;
  if (Bag.IsValid())
  {
   FVaRestJsonConvertOptions Options; Options.Rules=FVaRestJsonRules::Snapshot(Owner);
   return FVaRestJsonConvert::PropertyToJson(Property,Address,Options).ToString()==TEXT("{\"one\":\"stop-entry\"}");
  }
  const FVaRestDynamicLifecycleRoot* Value=nullptr;
  switch (Container)
  {
   case EVaRestTypeContainer::None:Value=&Native->Scalar;break;
   case EVaRestTypeContainer::Array:if (Native->Array.Num()==1) Value=&Native->Array[0];break;
   case EVaRestTypeContainer::Map:if (Native->Map.Num()==1) Value=Native->Map.Find(TEXT("one"));break;
  }
  if (!Value || Value->Name!=TEXT("stop-native") || !Value->Maybe.IsSet() ||
   Value->Maybe->Text!=TEXT("nested") || Value->Maybe->Number!=23) return false;
  FVaRestDynamicLifecycleRoot Copy(*Value);
  FVaRestDynamicLifecycleRoot::StaticStruct()->CopyScriptStruct(&Copy,Value);
  return FVaRestDynamicLifecycleRoot::StaticStruct()->CompareScriptStruct(&Copy,Value,0) &&
   FVaRestDynamicLifecycleRoot::StaticStruct()->GetStructTypeHash(&Copy)==FVaRestDynamicLifecycleRoot::StaticStruct()->GetStructTypeHash(Value);
 }
};

class FRun final : public IAutomationLatentCommand
{
public:
 FRun(FAutomationTestBase& InTest,FString InCase,bool InTiming,IWitness& InWitness,FProbe InProbe)
  : Test(InTest),Case(MoveTemp(InCase)),Timing(InTiming),Witness(InWitness),Probe(MoveTemp(InProbe)),Deadline(FPlatformTime::Seconds()+35.0),
   Observer(MakeUnique<FGTObserver>()),Outcome(Testing::MakeOutcome<FVaRestDynamicResult>()),Release(MakeShared<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe>())
 {
  Observer->Timing=Timing;
  Observer->Baseline=Testing::LiveDynamicCarriersOnGT();
  LaunchBaseline=Testing::Counters().DecodeTasksLaunched.load();
  if (Timing)
  {
   Observer->TimingSink=MakeShared<FTimingSink,ESPMode::ThreadSafe>();
   FVaRestTrace::AddSink(Observer->TimingSink.ToSharedRef());
  }
  if (Case==TEXT("NativeOptionalArray")) Container=EVaRestTypeContainer::Array;
  if (Case==TEXT("NativeOptionalMap") || Case==TEXT("EnumOwnerMap")) Container=EVaRestTypeContainer::Map;
 }
 ~FRun() override
 {
  check(IsInGameThread());
  if (!Armed)
  {
   Probe->FailOnGT();
   if (RegisteredRoot) FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct());
   if (RegisteredLeaf) FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct());
   if (Owner.IsValid()) FVaRestJsonRules::UnregisterOwner(Owner.Get());
   if (Observing) DynamicLifecycleTest::EndObservationOnGT();
  }
 }
 bool Update() override
 {
  check(IsInGameThread());
  const auto Trace=ObserveTraceSelectionOnGT(Timing,Observer->TimingSink.Get());
  if (!Trace.MembershipMatches || !Trace.MaskMatches)
   return Fail(TEXT("selected trace variant changed or actual sink membership differs from the fixture-owned selection"));
  if (FPlatformTime::Seconds()>=Deadline || Probe->FailedOnGT()) return Fail(TEXT("module-stop setup/held native worker deadline or evidence failure"));
  if (Phase==0)
  {
   FString Code; if (!Intrinsic(Code))
   {
    if (Code==TEXT("EditorMutationHooksUnavailable")) return Fail(TEXT("real required Editor mutation/transactor hooks are missing"));
    return false; // only real engine frames settle admission; never reset frames or mutation hooks
   }
   const auto* Optional=FindFProperty<FOptionalProperty>(FVaRestDynamicLifecycleRoot::StaticStruct(),GET_MEMBER_NAME_CHECKED(FVaRestDynamicLifecycleRoot,Maybe));
   const auto* Leaf=Optional ? CastField<FStructProperty>(Optional->GetValueProperty()) : nullptr;
   if (!Leaf || Leaf->Struct!=FVaRestDynamicLifecycleLeaf::StaticStruct()) return Fail(TEXT("actual Optional leaf was not generated"));
   UnknownRefused=RefusedOptional();
   if (!UnknownRefused) return Fail(TEXT("unknown actual native Optional root was not refused before worker allocation"));
   RegisteredRoot=FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct(),TEXT("VaRest.Test.GT.Optional.Root.v1"));
   if (!RegisteredRoot) return Fail(TEXT("native root lifecycle contract registration failed"));
   Advance();return false;
  }
  if (Phase==1)
  {
   FString Code;if (GFrameCounter<=PublicationFrame || !Intrinsic(Code)) return false;
   LeafRefused=RefusedOptional();
   if (!LeafRefused) return Fail(TEXT("native dependency reached only through Optional was not refused before launch"));
   RegisteredLeaf=FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct(),TEXT("VaRest.Test.GT.Optional.Leaf.v1"));
   if (!RegisteredLeaf) return Fail(TEXT("native Optional leaf registration failed"));
   if (Case==TEXT("EnumOwnerMap") && !CreateEnumAndRules()) return Fail(TEXT("actual unique enum/RuleOwner fixture creation failed"));
   DynamicLifecycleTest::BeginObservationOnGT();Observing=true;
   Advance();return false;
  }
  if (Phase==2)
  {
   FString Code;if (GFrameCounter<=PublicationFrame || !Intrinsic(Code)) return false;
   Observer->Ordinary=MakeUnique<Testing::FTestRig>(TEXTVIEW("https://module-stop.test"));
   Observer->Ordinary->Fake->bCompleteOnWorker=false;
   Observer->OrdinaryClock=MakeShared<FObservedClock,ESPMode::ThreadSafe>(Observer->Ordinary->Clock);
   Observer->Ordinary->Client.WithClock(Observer->OrdinaryClock.ToSharedRef());
   Destination=MakeShared<FDestination,ESPMode::ThreadSafe>();
   if (!Destination->Initialize(Container,Enum.Get())) return Fail(TEXT("real generated/native destination was not created"));
   auto Request=Observer->Ordinary->Client.RequestDynamic(TEXTVIEW("GET"),TEXTVIEW("/preflight"),Config());
   FString Error;if (!FDynamicRequestAccess::ValidateOnGT(Request,Error))
   { FDynamicRequestAccess::DiscardOnGT(Request); return Fail(*Error); }
   if (!FDynamicRequestAccess::SetPreparationOnGT(Request,[Owned=Destination](FDynamicPrepareToken& Token){ Owned->Prepare(Token); }))
   { FDynamicRequestAccess::DiscardOnGT(Request); return Fail(TEXT("real positive private preparation did not bind")); }
   Observer->Ordinary->Fake->Enqueue(FVaRestFakeResponse::Text(200,Body(),TEXTVIEW("application/json")));
   Handle=Testing::Observe(MoveTemp(Request),Outcome).Send();
   if (!Handle.GetId()) return Fail(TEXT("real preflight Send did not allocate request identity"));
   Phase=3;return false;
  }
  if (Phase==3)
  {
   if (!Outcome->IsDone()) return false;
   if (Outcome->Error.IsError() || !Outcome->Value.IsSet() || !Destination->Verify(Container,Owner.Get()) ||
    Outcome->Value->GetType().Availability!=EVaRestDynamicAvailability::Available ||
    Testing::LiveDynamicCarriersOnGT()!=Observer->Baseline) return Fail(TEXT("actual typed positive transfer/reset did not produce its selected value"));
   if (Outcome->Value->GetNativeOwnerCountForTesting()!=1) return false; // owned shell quiescence, no task pump/drain
   if (!Outcome->Value->AttachReleaseProbeForTesting(Release)) return Fail(TEXT("sole real public shell release observer refused"));
   Destination.Reset(); // actual engine-owned destination storage dies on GTK before the late experiment
   auto Shell=MoveTemp(Outcome->Value.GetValue());Outcome->Value.Reset();
   UE::Tasks::Launch(TEXT("VaRest.M5.Stop.PreflightShellRelease"),[Shell=MoveTemp(Shell)]() mutable { Shell=FVaRestDynamicResult(); });
   Materialized=true;Phase=4;return false;
  }
  if (Phase==4)
  {
   if (!Release->bDestroyed.load(std::memory_order_acquire)) return false;
   if (Release->bDestroyedOnGameThread.load(std::memory_order_relaxed)) return Fail(TEXT("sole real result shell was not finally destroyed on a worker"));
   Observer->Managed=MakeUnique<Testing::FTestRig>(TEXTVIEW("https://module-stop.test"));
   Observer->Managed->Fake->bCompleteOnWorker=false;
   Observer->ManagedClock=MakeShared<FObservedClock,ESPMode::ThreadSafe>(Observer->Managed->Clock);
   FVaRestTraceScope ScopeInfo;ScopeInfo.Label=TEXT("M5ModuleStopFixture");
   auto Scope=FNativeScopeLifetime::Open(MoveTemp(ScopeInfo),Observer->Managed->Clock->Now());
   if (!Scope) return Fail(TEXT("actual native scope admission refused"));
   auto Initial=FClientAccess::InitialConfig(TEXTVIEW("https://module-stop.test"));
   Initial.Transport=Observer->Managed->Fake;Initial.Clock=Observer->ManagedClock;
   Initial.Connectivity=Observer->Managed->Connectivity;Initial.LogLevel=EVaRestLogLevel::None;
   FName Error;Observer->Managed->Client=FClientAccess::CreateManaged(MoveTemp(Initial),Scope.ToSharedRef(),Error);
   const auto State=FClientAccess::State(Observer->Managed->Client);
   if (!State || !State->GetManagedLifetime().IsValid()) return Fail(TEXT("actual module-owned CreateManaged refused"));
   const auto& Registration=State->GetManagedLifetime();
   Observer->Control=Registration.Control;Observer->Gate=Registration.Gate;Observer->Operations=Registration.Operations;
   if (Observer->Control!=NativeControlForNewClientOnGT()) return Fail(TEXT("managed factory did not use the current module service Control"));
   Probe->SetupOnGT(UnknownRefused,LeafRefused,Materialized,true);
   if (!SendHeld(*Observer->Ordinary,TEXTVIEW("/held-ordinary")) || !SendHeld(*Observer->Managed,TEXTVIEW("/held-managed")))
    return Fail(TEXT("one real RequestDynamic worker route did not admit its typed state"));
   // Request configs are consumed, destination is gone. These are the LAST external strong enum/owner roots.
   Enum.Reset();Owner.Reset();
   Phase=5;return false;
  }
  if (!Probe->BothHeldOnGT()) return false;
  if (Testing::LiveDynamicCarriersOnGT()!=Observer->Baseline || DynamicLifecycleTest::ObserveOnGT().WrongThread!=0)
   return Fail(TEXT("native parser workers retained/accessed reflected materialization"));
  if (Case!=TEXT("EnumOwnerMap"))
  {
   const auto Seen=DynamicLifecycleTest::ObserveOnGT();
   for (const auto Op:{DynamicLifecycleTest::EOperation::DefaultConstruct,DynamicLifecycleTest::EOperation::CopyConstruct,
    DynamicLifecycleTest::EOperation::Assign,DynamicLifecycleTest::EOperation::Destroy,
    DynamicLifecycleTest::EOperation::Equal,DynamicLifecycleTest::EOperation::Hash})
    if (Seen.Operations[static_cast<uint8>(Op)]<=0) return Fail(TEXT("selected native positive did not execute every lifecycle operation category"));
  }
  if (!Probe->ObserveClockOnGT(Timing,static_cast<uint32>(FVaRestTrace::Detail()),
   static_cast<uint64>(reinterpret_cast<UPTRINT>(Observer->OrdinaryClock.Get())),
   static_cast<uint64>(reinterpret_cast<UPTRINT>(Observer->ManagedClock.Get())),
   Observer->OrdinaryClock->OnGT(),Observer->OrdinaryClock->OnWorker(),
   Observer->ManagedClock->OnGT(),Observer->ManagedClock->OnWorker()))
   return Fail(TEXT("actual selected timing flags/original clock identities/any-thread Now observations did not agree"));
  if (!Witness.SetGTObserverOnGT([Owned=MoveTemp(Observer)] { return Owned->ObserveOnGT(); }))
   return Fail(TEXT("ordinary witness refused the separately owned GT observer"));
  if (!Probe->ArmOnGT()) return Fail(TEXT("native witness did not flush the armed-only marker"));
  Armed=true;Observing=false;
  Test.AddInfo(TEXT("ARMED ONLY. This automation Success is not module-stop acceptance. The SAME Automation command must continue with ;SoftQuit. External evidence and nonforced exit0 decide."));
  return true;
 }
private:
 bool Fail(const TCHAR* Message) { Test.AddError(Message);Probe->FailOnGT();return true; }
 void Advance() { PublicationFrame=GFrameCounter;++Phase; }
 FState Intrinsic(FString& Code)
 {
  FVaRestDynamicResultConfig C;C.ResponseType.Kind=EVaRestTypeKind::Struct;C.ResponseType.Type=FVaRestJson::StaticStruct();
  FString Error;return FDynamicTypedState::CreateOnGT(C,Error,&Code);
 }
 bool RefusedOptional()
 {
  FVaRestDynamicResultConfig C;C.ResponseType.Kind=EVaRestTypeKind::Struct;C.ResponseType.Type=FVaRestDynamicLifecycleRoot::StaticStruct();
  FString Error;auto State=FDynamicTypedState::CreateOnGT(C,Error);
  return !State && Error.Contains(TEXT("qualified game-thread lifecycle contract")) &&
   Testing::LiveDynamicCarriersOnGT()==Observer->Baseline && Testing::Counters().DecodeTasksLaunched.load()==LaunchBaseline;
 }
 bool CreateEnumAndRules()
 {
  const FName Name=MakeUniqueObjectName(GetTransientPackage(),UEnum::StaticClass(),TEXT("VaRestM5StopEnum"));
  Enum.Reset(NewObject<UEnum>(GetTransientPackage(),Name,RF_Transient));
  if (UObjectRedirector::StaticClass()->HasAnyClassFlags(CLASS_Abstract)) return false;
  Owner.Reset(NewObject<UObjectRedirector>(GetTransientPackage(),NAME_None,RF_Transient));
  if (!Enum.IsValid() || !Owner.IsValid()) return false;
  const FName Entry(*(Name.ToString()+TEXT("::Entry")));
  TArray<TPair<FName,int64>> Values;Values.Emplace(Entry,int64(0));
  if (!Enum->SetEnums(Values,UEnum::ECppForm::EnumClass,UEnum::EUnderlyingType::uint8,EEnumFlags::None,UEnum::EAddMaxKeyIfMissing::Yes)) return false;
  FVaRestJsonEnumRule Rule;Rule.Enumerator=Entry;Rule.Json=TEXT("stop-entry");
  return FVaRestJsonRules::RegisterEnum(Owner.Get(),Enum.Get(),MakeArrayView(&Rule,1));
 }
 FVaRestDynamicResultConfig Config() const
 {
  FVaRestDynamicResultConfig C;C.ResponseType.Container=Container;C.RuleOwner=Owner.Get();
  C.ResponseType.Kind=Enum.IsValid() ? EVaRestTypeKind::Enum : EVaRestTypeKind::Struct;
  C.ResponseType.Type=Enum.IsValid() ? static_cast<UObject*>(Enum.Get()) : static_cast<UObject*>(FVaRestDynamicLifecycleRoot::StaticStruct());
  return C;
 }
 FString Body() const
 {
  if (Enum.IsValid()) return TEXT("{\"one\":\"stop-entry\"}");
  const FString Value=TEXT("{\"name\":\"stop-native\",\"maybe\":{\"text\":\"nested\",\"number\":23}}");
  if (Container==EVaRestTypeContainer::Array) return TEXT("[")+Value+TEXT("]");
  if (Container==EVaRestTypeContainer::Map) return TEXT("{\"one\":")+Value+TEXT("}");
  return Value;
 }
 bool SendHeld(Testing::FTestRig& Rig,FStringView Path)
 {
  auto Request=Rig.Client.RequestDynamic(TEXTVIEW("GET"),Path,Config());
  FString Error;if (!FDynamicRequestAccess::ValidateOnGT(Request,Error)) { FDynamicRequestAccess::DiscardOnGT(Request);return false; }
  if (!FDynamicRequestAccess::BindModuleStopProbeOnGT(Request,Probe.ToSharedRef())) { FDynamicRequestAccess::DiscardOnGT(Request);return false; }
  Observer->Types.Add(FDynamicRequestAccess::GetTypesOnGT(Request));
  const auto Completion=FDynamicRequestAccess::CompletionForStopTestOnGT(Request);
  if (!Completion) { FDynamicRequestAccess::DiscardOnGT(Request);return false; }
  Observer->Completions.Add(Completion);
  Rig.Fake->Enqueue(FVaRestFakeResponse::Text(200,Body(),TEXTVIEW("application/json")));
  const auto Sent=MoveTemp(Request).OnComplete([Native=Probe](const TVaRestResult<FVaRestDynamicResult>&) { Native->UserDeliveryOnGT(); }).Send();
  return Sent.GetId()!=0;
 }
 FAutomationTestBase& Test;FString Case;const bool Timing;IWitness& Witness;FProbe Probe;
 const double Deadline;
 TUniquePtr<FGTObserver> Observer;
 TSharedRef<Testing::TTestOutcome<FVaRestDynamicResult>,ESPMode::ThreadSafe> Outcome;
 TSharedRef<FVaRestDynamicReleaseProbe,ESPMode::ThreadSafe> Release;
 TSharedPtr<FDestination,ESPMode::ThreadSafe> Destination;
 TStrongObjectPtr<UEnum> Enum;TStrongObjectPtr<UObject> Owner;
 FVaRestRequestHandle Handle;
 EVaRestTypeContainer Container=EVaRestTypeContainer::None;
 uint64 PublicationFrame=0;int32 LaunchBaseline=0,Phase=0;
 bool RegisteredRoot=false,RegisteredLeaf=false,UnknownRefused=false,LeafRefused=false;
 bool Observing=false,Materialized=false,Armed=false;
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestDynamicModuleStopTest,"VaRest.Http.Dynamic.ModuleStopArm",
 (WITH_EDITOR ? EAutomationTestFlags::EditorContext : (EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext)) | EAutomationTestFlags::EngineFilter)
void FVaRestDynamicModuleStopTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
 FString Case,Trace;
 // No discoverable arm row in ordinary product/test processes. One case/trace pair per fresh process.
 if (VaRest::Http::ModuleStopFixture::SelectedConfiguration(Case,Trace))
 { Names.Add(Case+TEXT(".Trace")+Trace);Commands.Add(Case+TEXT("|")+Trace); }
}
bool FVaRestDynamicModuleStopTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Http::ModuleStopFixture;
 FString Case,Trace;
 if (!SelectedConfiguration(Case,Trace) || Parameters!=Case+TEXT("|")+Trace) { AddError(TEXT("module-stop arm requires explicit dedicated fresh-process case and trace variant"));return false; }
 if (!FPlatformProcess::SupportsMultithreading()) { AddError(TEXT("module-stop experiment requires both genuine native worker routes"));return false; }
 const auto InitialTrace=ObserveTraceSelectionOnGT(false,nullptr);
 if (!InitialTrace.MembershipMatches || !InitialTrace.MaskMatches) { AddError(TEXT("dedicated trace qualification requires an actually empty sink registry and None mask; Timings installs only its own real Metadata sink"));return false; }
 const auto Witnesses=IModularFeatures::Get().GetModularFeatureImplementations<IWitness>(FeatureName());
 if (Witnesses.Num()!=1 || !Witnesses[0]->StartupPrecededHttpOnGT()) { AddError(TEXT("exactly one real earlier-started ordinary-plugin witness is required"));return false; }
 const auto Probe=Witnesses[0]->CreateProbeOnGT(Case+TEXT(".Trace")+Trace);
 if (!Probe) { AddError(TEXT("native early-opened evidence channel/current module witness setup failed"));return false; }
 ADD_LATENT_AUTOMATION_COMMAND(FRun(*this,Case,Trace==TEXT("Timings"),*Witnesses[0],Probe));
 return true;
}

namespace VaRest::Http::HeldRetirementFixture
{
using namespace VaRest::ModuleStopTest;
constexpr int32 EventCount = static_cast<int32>(EPublication::ManagedReturnCaptureAbandoned) + 1;

// The worker and Frame retain only this plain native observation state, never the runner/clock/destination.
class FProbe final : public INativeProbe, public IManagedFrameRetirementObserver
{
public:
 void HoldAfterNativeParse(FWorkerIdentity Id,bool Parsed,bool Timing,uint64 Clock,uint32 Thread,bool OnGT) override
 {
  if (OnGT || !Parsed || Timing || !Clock || !Thread || Id.Route!=ERoute::Managed || Id.Generation!=1 || !Id.NativeToken || Entered.exchange(true,std::memory_order_acq_rel))
  { Failed.store(true,std::memory_order_release);return; }
  Identity=Id;ClockAddress=Clock;
  Held.store(true,std::memory_order_release); // immutable identity/clock values published to GT
  const double Limit=FPlatformTime::Seconds()+15.0;
  while (!Released.load(std::memory_order_acquire) && FPlatformTime::Seconds()<Limit) FPlatformProcess::SleepNoStats(0.001f);
  if (!Released.load(std::memory_order_acquire)) Failed.store(true,std::memory_order_release);
 }
 void Published(FWorkerIdentity Id,EPublication Event,uint32 Thread,bool OnGT,bool Stopped) override
 {
  if (!Matches(Id) || !Thread || Stopped || Event==EPublication::OrdinaryReady) Failed.store(true,std::memory_order_release);
  const int32 Index=static_cast<int32>(Event);
  if (Index<0 || Index>=EventCount) { Failed.store(true,std::memory_order_release);return; }
  if ((Event==EPublication::ManagedReturnExecuted || Event==EPublication::ManagedReturnCompleted) && !OnGT)
   Failed.store(true,std::memory_order_release);
  Events[Index].fetch_add(1,std::memory_order_release);
 }
 void DataDisposed(FWorkerIdentity Id,uint32 Thread,bool OnGT) override
 { RecordFrame(Id,Thread,OnGT,Disposed); }
 void FrameDestroyed(FWorkerIdentity Id,uint32 Thread,bool OnGT) override
 { RecordFrame(Id,Thread,OnGT,Destroyed); }
 void ReflectedMaterializationOnGT() override { RecordGT(Materializations); }
 void UserDeliveryOnGT() override { RecordGT(Deliveries); }
 bool BothHeldOnGT() const override { check(IsInGameThread());return Held.load(std::memory_order_acquire); }
 bool FailedOnGT() const override { check(IsInGameThread());return Failed.load(std::memory_order_acquire); }
 bool ArmOnGT() override { FailOnGT();return false; } // this fixture never arms a shutdown witness
 void SetupOnGT(bool,bool,bool,bool) override { FailOnGT(); }
 bool ObserveClockOnGT(bool,uint32,uint64,uint64,int64,int64,int64,int64) override { FailOnGT();return false; }
 void FailOnGT() override { check(IsInGameThread());Failed.store(true,std::memory_order_release);Release(); }
 void Release() { Released.store(true,std::memory_order_release); }
 FWorkerIdentity IdentityOnGT() const { check(IsInGameThread() && Held.load(std::memory_order_acquire));return Identity; }
 uint64 ClockOnGT() const { check(IsInGameThread() && Held.load(std::memory_order_acquire));return ClockAddress; }
 int32 Count(EPublication Event) const { return Events[static_cast<int32>(Event)].load(std::memory_order_acquire); }
 void PublicResult(bool Good) { CallbackOK.store(Good,std::memory_order_release);RecordGT(Callbacks); }
 void CaptureDestroyed(bool Preparation) { RecordGT(Preparation ? PreparationDestroyed : CallbackDestroyed); }
 void ClockDestroyed() { RecordGT(ClocksDestroyed); }
 std::atomic<int32> Disposed{0},Destroyed{0},Materializations{0},Deliveries{0},Callbacks{0};
 std::atomic<int32> PreparationDestroyed{0},CallbackDestroyed{0},ClocksDestroyed{0},WrongThread{0};
 std::atomic<bool> CallbackOK{false};
private:
 bool Matches(FWorkerIdentity Id) const
 {
  if (!Held.load(std::memory_order_acquire)) return false;
  return Id.Route==Identity.Route && Id.Request==Identity.Request && Id.Client==Identity.Client &&
   Id.Generation==Identity.Generation && Id.Cell==Identity.Cell && Id.CellGeneration==Identity.CellGeneration && Id.NativeToken==Identity.NativeToken;
 }
 void RecordGT(std::atomic<int32>& Counter)
 { if (!IsInGameThread()) WrongThread.fetch_add(1,std::memory_order_relaxed);Counter.fetch_add(1,std::memory_order_release); }
 void RecordFrame(FWorkerIdentity Id,uint32 Thread,bool OnGT,std::atomic<int32>& Counter)
 {
  if (!Matches(Id) || !Thread || !OnGT) Failed.store(true,std::memory_order_release);
  RecordGT(Counter);
 }
 FWorkerIdentity Identity;
 uint64 ClockAddress=0;
 std::atomic<int32> Events[EventCount]{};
 std::atomic<bool> Entered{false},Held{false},Released{false},Failed{false};
};
struct FCapture
{
 TSharedRef<FProbe,ESPMode::ThreadSafe> Probe;
 bool Preparation;
 FCapture(TSharedRef<FProbe,ESPMode::ThreadSafe> InProbe,bool InPreparation) : Probe(MoveTemp(InProbe)),Preparation(InPreparation) {}
 ~FCapture() { Probe->CaptureDestroyed(Preparation); }
};
class FClock final : public IVaRestClock
{
public:
 FClock(TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> InClock,TSharedRef<FProbe,ESPMode::ThreadSafe> InProbe)
  : Clock(MoveTemp(InClock)),Probe(MoveTemp(InProbe)) {}
 ~FClock() override { Probe->ClockDestroyed(); }
 double Now() const override { return Clock->Now(); }
 FVaRestTimerHandle SetTimer(double Delay,TUniqueFunction<void()>&& Callback) override
 { check(IsInGameThread());return Clock->SetTimer(Delay,MoveTemp(Callback)); }
 void ClearTimer(FVaRestTimerHandle& Handle) override { check(IsInGameThread());Clock->ClearTimer(Handle); }
private:
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
 TSharedRef<FProbe,ESPMode::ThreadSafe> Probe;
};

class FRun final : public IAutomationLatentCommand
{
public:
 explicit FRun(FAutomationTestBase& InTest) : Test(InTest),Probe(MakeShared<FProbe,ESPMode::ThreadSafe>()),Deadline(FPlatformTime::Seconds()+30.0),CarrierBaseline(Testing::LiveDynamicCarriersOnGT()) {}
 ~FRun() override { Probe->Release();Destination.Reset();DropClient();CleanupTypes(); }
 bool Update() override
 {
  check(IsInGameThread());
  if (Cleaning) return CleanupStep();
  if (FPlatformTime::Seconds()>=Deadline || Probe->FailedOnGT())
   return Fail(*FString::Printf(TEXT("held-worker retirement deadline or lifecycle failure: phase=%d held=%d callback=%d return=%d/%d capture=%d access=%d disposed=%d frame=%d viewReleased=%d work=%u inputs=%llu"),
    Phase,Probe->BothHeldOnGT()?1:0,Probe->Callbacks.load(),Probe->Count(EPublication::ManagedReturnExecuted),Probe->Count(EPublication::ManagedReturnCompleted),
    Probe->Count(EPublication::ManagedReturnCaptureRan),Probe->Count(EPublication::ManagedAccessEnded),Probe->Disposed.load(),Probe->Destroyed.load(),RecordClockReleased?1:0,
    Gate ? Gate->NumOwnedOperations() : 0,static_cast<unsigned long long>(Gate ? Gate->NumOwnedInputs() : 0)));
  if (Phase==0)
  {
   if (!Intrinsic()) return false;
   RegisteredRoot=FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct(),TEXT("VaRest.Test.GT.Optional.Root.v1"));
   if (!RegisteredRoot) return Fail(TEXT("owned native Root registration refused"));
   Advance();return false;
  }
  if (Phase==1)
  {
   if (GFrameCounter<=PublicationFrame || !Intrinsic()) return false;
   RegisteredLeaf=FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct(),TEXT("VaRest.Test.GT.Optional.Leaf.v1"));
   if (!RegisteredLeaf) return Fail(TEXT("owned native Optional leaf registration refused"));
   Advance();return false;
  }
  if (Phase==2)
  {
   if (GFrameCounter<=PublicationFrame || !Intrinsic()) return false;
   if (!Start()) return Fail(TEXT("real live-service managed RequestDynamic setup refused"));
   Phase=3;return false;
  }
  if (Phase==3)
  {
   if (!Probe->BothHeldOnGT()) return false;
   if (Probe->IdentityOnGT().Request!=Handle.GetId() || Testing::LiveDynamicCarriersOnGT()!=CarrierBaseline ||
    Probe->Materializations.load()!=0 || Probe->Callbacks.load()!=0 || Probe->Disposed.load()!=0 || Probe->Destroyed.load()!=0 ||
    Probe->Count(EPublication::ManagedWorkerEnded)!=0 || Probe->Count(EPublication::ManagedReturnExecuted)!=0 ||
    Probe->Count(EPublication::ManagedReturnCompleted)!=0 || Probe->Count(EPublication::ManagedReturnCaptureRan)!=0 ||
    Probe->Count(EPublication::ManagedReturnCaptureAbandoned)!=0 || Probe->Count(EPublication::ManagedAccessEnded)!=0 ||
    !Operations || Operations->NumOccupied()!=1 || !Gate || Gate->NumOwnedOperations()!=1)
    return Fail(TEXT("actual parsed worker was not still held with an occupied owned operation before return/materialization"));
   if (!BindObserver()) return Fail(TEXT("private observer did not bind under the matching actual shared Access"));
   // BindObserver's only strong Access pin has been explicitly reset and its scope returned before this release.
   Probe->Release();Phase=4;return false;
  }
  if (Phase==4)
  {
   if (!ReturnedMarks()) return false;
   if (!RecordClockReleased)
   {
    // The fake's completed history still owns the real managed dispatch clock view/physical lease.
    if (!Rig || Rig->Fake->NumCalls()!=1 || !Rig->Fake->GetCall(0).bCompleted ||
     Rig->Fake->GetCall(0).Request.RequestId!=Handle.GetId() || !Rig->Fake->GetCall(0).Request.Clock.IsValid() ||
     !Operations || Operations->NumOccupied()!=1 || !Gate || Gate->NumOwnedOperations()!=1 || Gate->NumOwnedInputs()!=1 ||
     Probe->Destroyed.load()!=0 || Probe->ClocksDestroyed.load()!=0)
     return Fail(TEXT("own completed fake record did not retain its actual dispatch view before final retirement"));
    if (!Rig->Fake->ReleaseCompletedClock(0) || Rig->Fake->GetCall(0).Request.Clock.IsValid())
     return Fail(TEXT("own completed fake record did not release its actual dispatch clock view"));
    RecordClockReleased=true;
   }
   if (!OwnedQuiescent()) return false;
   if (!Probe->CallbackOK.load(std::memory_order_acquire) || !Destination || !Destination->Verify(EVaRestTypeContainer::None,nullptr) ||
    !CompleteMarks() || Testing::LiveDynamicCarriersOnGT()!=CarrierBaseline || DynamicLifecycleTest::ObserveOnGT().WrongThread!=0)
    return Fail(TEXT("real typed return/capture/AccessEnded/Frame/input retirement did not retain its exact live-service contract"));
   Destination.Reset();DropClient();Phase=5;return false;
  }
  if (WeakClock.IsValid() || Probe->ClocksDestroyed.load(std::memory_order_acquire)==0) return false;
  if (Probe->ClocksDestroyed.load()!=1 || Probe->WrongThread.load()!=0 || !CompleteMarks() || Control->IsStopped() || Control->IsExitRetentionRequested())
   return Fail(TEXT("last original clock/affine retirement was not exactly once on GT while the real service stayed live"));
  CleanupTypes();return true;
 }
private:
 bool Intrinsic()
 {
  FVaRestDynamicResultConfig Config;Config.ResponseType.Kind=EVaRestTypeKind::Struct;Config.ResponseType.Type=FVaRestJson::StaticStruct();
  FString Error,Code;const auto State=FDynamicTypedState::CreateOnGT(Config,Error,&Code);
  if (!State && Code==TEXT("EditorMutationHooksUnavailable")) { Fail(TEXT("real editor mutation hooks are unavailable"));return false; }
  return State.IsValid();
 }
 void Advance() { PublicationFrame=GFrameCounter;++Phase; }
 bool Start()
 {
  const auto Trace=ModuleStopFixture::ObserveTraceSelectionOnGT(false,nullptr);
  if (!Trace.MembershipMatches || !Trace.MaskMatches) return false;
  Rig=MakeUnique<Testing::FTestRig>(TEXTVIEW("https://held-retirement.test"));Rig->Fake->bCompleteOnWorker=false;
  const auto Clock=MakeShared<FClock,ESPMode::ThreadSafe>(Rig->Clock,Probe);
  WeakClock=Clock;
  FVaRestTraceScope Info;Info.Label=TEXT("DynamicHeldWorkerRetirement");
  const auto Scope=FNativeScopeLifetime::Open(MoveTemp(Info),Rig->Clock->Now());if (!Scope) return false;
  auto Initial=FClientAccess::InitialConfig(TEXTVIEW("https://held-retirement.test"));
  Initial.Transport=Rig->Fake;Initial.Clock=Clock;Initial.Connectivity=Rig->Connectivity;Initial.LogLevel=EVaRestLogLevel::None;
  FName Error;Rig->Client=FClientAccess::CreateManaged(MoveTemp(Initial),Scope.ToSharedRef(),Error);
  const auto State=FClientAccess::State(Rig->Client);if (!State || !State->GetManagedLifetime().IsValid()) return false;
  const auto& Registration=State->GetManagedLifetime();Control=Registration.Control;Gate=Registration.Gate;Operations=Registration.Operations;
  if (Control!=NativeControlForNewClientOnGT() || Control->IsStopped() || Control->IsExitRetentionRequested() || !Operations || Operations->NumOccupied()!=0) return false;
  DynamicLifecycleTest::BeginObservationOnGT();Observing=true;
  Destination=MakeShared<ModuleStopFixture::FDestination,ESPMode::ThreadSafe>();
  if (!Destination->Initialize(EVaRestTypeContainer::None,nullptr)) return false;
  FVaRestDynamicResultConfig Config;Config.ResponseType.Kind=EVaRestTypeKind::Struct;Config.ResponseType.Type=FVaRestDynamicLifecycleRoot::StaticStruct();
  auto Request=Rig->Client.RequestDynamic(TEXTVIEW("GET"),TEXTVIEW("/held"),MoveTemp(Config));
  if (!FDynamicRequestAccess::BindModuleStopProbeOnGT(Request,Probe) ||
   !FDynamicRequestAccess::SetPreparationOnGT(Request,[Owned=Destination,Marker=MakeShared<FCapture,ESPMode::ThreadSafe>(Probe,true)](FDynamicPrepareToken& Token){ (void)Marker;Owned->Prepare(Token); }))
  { FDynamicRequestAccess::DiscardOnGT(Request);return false; }
  Rig->Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("{\"name\":\"stop-native\",\"maybe\":{\"text\":\"nested\",\"number\":23}}"),TEXTVIEW("application/json")));
  Handle=MoveTemp(Request).OnComplete([Native=Probe,Marker=MakeShared<FCapture,ESPMode::ThreadSafe>(Probe,false)](const TVaRestResult<FVaRestDynamicResult>& Result) {
   (void)Marker;Native->PublicResult(!Result.Error.IsError() && Result.Value.IsSet() && Result.Value->GetType().Availability==EVaRestDynamicAvailability::Available);
  }).Send();
  return Handle.GetId()!=0;
 }
 bool BindObserver()
 {
  bool Bound=false;const auto Identity=Probe->IdentityOnGT();
  const auto State=FClientAccess::State(Rig->Client);if (!State) return false;
  State->ForEachInFlight([&](FExchangeState& Exchange) {
   if (Exchange.RequestId!=Identity.Request || Exchange.ClientId!=Identity.Client) return;
   auto Access=Exchange.NativeCompletion.Pin();
   if (Access) { WeakAccess=Access;Bound=Access->ObserveRetirementForTestsOnGT(Identity,Probe); }
   Access.Reset();
   Test.TestFalse(TEXT("fixture's temporary actual Access root is dropped before native gate release"),Access.IsValid());
  });
  const auto Clock=WeakClock.Pin();
  return Bound && Clock && Probe->ClockOnGT()==static_cast<uint64>(reinterpret_cast<UPTRINT>(Clock.Get()));
 }
 bool OwnedQuiescent() const
 {
  return Operations && Operations->NumOccupied()==0 && Gate && Gate->NumOwnedOperations()==0 &&
   Control && Gate->NumOwnedInputs()==0 && !WeakAccess.IsValid();
 }
 bool CompleteMarks() const
 {
  return Probe->Destroyed.load(std::memory_order_acquire)==1 && ReturnedMarks();
 }
 bool ReturnedMarks() const
 {
  return Probe->Disposed.load(std::memory_order_acquire)==1 &&
   Probe->Materializations.load()==1 && Probe->Deliveries.load()==1 && Probe->Callbacks.load()==1 &&
   Probe->PreparationDestroyed.load()==1 && Probe->CallbackDestroyed.load()==1 && Probe->WrongThread.load()==0 &&
   Probe->Count(EPublication::ManagedWorkerEnded)==1 && Probe->Count(EPublication::ManagedAccessEnded)==1 &&
   Probe->Count(EPublication::ManagedReturnExecuted)==1 && Probe->Count(EPublication::ManagedReturnCompleted)==1 &&
   Probe->Count(EPublication::ManagedReturnCaptureRan)==1 && Probe->Count(EPublication::ManagedReturnCaptureAbandoned)==0;
 }
 bool Fail(const TCHAR* Message)
 {
  if (!Cleaning) { Test.AddError(Message);Cleaning=true;CleanupDeadline=FPlatformTime::Seconds()+10.0;Probe->Release();RetireClient(); }
  return false;
 }
 void ReleaseOwnCompletedClock()
 {
  if (Rig && Handle.GetId()!=0 && Rig->Fake->NumCalls()==1 && Rig->Fake->GetCall(0).bCompleted &&
   Rig->Fake->GetCall(0).Request.RequestId==Handle.GetId() && Rig->Fake->GetCall(0).Request.Clock.IsValid())
   Rig->Fake->ReleaseCompletedClock(0); // exact owned completed record only; never an active or foreign call
 }
 void RetireClient()
 { if (Rig && !Retired) { ReleaseOwnCompletedClock();Retired=true;Operations=nullptr;FClientAccess::RetireManaged(Rig->Client); } }
 void DropClient()
 {
  RetireClient();Operations=nullptr;Rig.Reset();Handle=FVaRestRequestHandle();Gate.Reset();
 }
 bool CleanupStep()
 {
  Probe->Release();
  // RetireClient cleared the raw store pointer BEFORE the client row may retire/recycle; inspect owned gates only.
  if (Rig && Gate && (Gate->NumOwnedOperations()!=0 || Gate->NumOwnedInputs()!=0 || WeakAccess.IsValid()))
  {
   if (FPlatformTime::Seconds()<CleanupDeadline) return false;
   Test.AddError(FString::Printf(TEXT("owned failure cleanup timed out: work=%u inputs=%llu AccessAlive=%d; no synthetic cleanup/retirement pass"),
    Gate->NumOwnedOperations(),static_cast<unsigned long long>(Gate->NumOwnedInputs()),WeakAccess.IsValid()?1:0));
  }
  Destination.Reset();DropClient();
  if (WeakClock.IsValid() && FPlatformTime::Seconds()<CleanupDeadline) return false;
  if (WeakClock.IsValid()) Test.AddError(TEXT("owned clock remains retained after finite failure cleanup; lifecycle qualification FAILED"));
  CleanupTypes();return true;
 }
 void CleanupTypes()
 {
  if (Observing) { DynamicLifecycleTest::EndObservationOnGT();Observing=false; }
  if (RegisteredRoot) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct());RegisteredRoot=false; }
  if (RegisteredLeaf) { FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct());RegisteredLeaf=false; }
 }
 FAutomationTestBase& Test;
 TSharedRef<FProbe,ESPMode::ThreadSafe> Probe;
 const double Deadline;
 double CleanupDeadline=0;
 const int32 CarrierBaseline;
 TUniquePtr<Testing::FTestRig> Rig;
 TSharedPtr<ModuleStopFixture::FDestination,ESPMode::ThreadSafe> Destination;
 TSharedPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Control;
 TSharedPtr<FClientAdmissionGate,ESPMode::ThreadSafe> Gate;
 FNativeOperationStore* Operations=nullptr;
 TWeakPtr<FManagedCompletionAccess,ESPMode::ThreadSafe> WeakAccess;
 TWeakPtr<FClock,ESPMode::ThreadSafe> WeakClock;
 FVaRestRequestHandle Handle;
 uint64 PublicationFrame=0;
 int32 Phase=0;
 bool RegisteredRoot=false,RegisteredLeaf=false,Observing=false,Retired=false,Cleaning=false,RecordClockReleased=false;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestDynamicHeldWorkerRetirementTest,"VaRest.Http.Dynamic.HeldWorkerRetirement",
 (WITH_EDITOR ? EAutomationTestFlags::EditorContext : (EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext)) | EAutomationTestFlags::EngineFilter)
bool FVaRestDynamicHeldWorkerRetirementTest::RunTest(const FString& Parameters)
{
 if (!FPlatformProcess::SupportsMultithreading()) { AddError(TEXT("held-worker retirement requires a genuine managed native worker"));return false; }
 ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::HeldRetirementFixture::FRun(*this));
 return true;
}

namespace VaRest::Http::HeldRetirementFixture
{
// The real Hub owner retires this scope while its managed parser is held. Unlike the LIVE
// case above, no public result or reflected materialization is permitted after gate closure.
class FScopeStopRun final : public IAutomationLatentCommand
{
public:
 explicit FScopeStopRun(FAutomationTestBase& InTest)
  : Test(InTest),Probe(MakeShared<FProbe,ESPMode::ThreadSafe>()),Deadline(FPlatformTime::Seconds()+30.0),CarrierBaseline(Testing::LiveDynamicCarriersOnGT()) {}
 ~FScopeStopRun() override { Probe->Release(); }
 bool Update() override
 {
  check(IsInGameThread());
  if (Closing) return CloseStep();
  if (FPlatformTime::Seconds()>=Deadline || Probe->FailedOnGT())
   return Fail(TEXT("held scope retirement deadline or native identity/thread failure; retaining owned cleanup until quiescent"));
  if (Phase==0)
  {
   if (!Intrinsic()) return false;
   RegisteredRoot=FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct(),TEXT("VaRest.Test.GT.Optional.Root.v1"));
   if (!RegisteredRoot) return Fail(TEXT("owned scope Root registration refused"));
   Advance();return false;
  }
  if (Phase==1)
  {
   if (GFrameCounter<=PublicationFrame || !Intrinsic()) return false;
   RegisteredLeaf=FVaRestDynamicNativeTypes::RegisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct(),TEXT("VaRest.Test.GT.Optional.Leaf.v1"));
   if (!RegisteredLeaf) return Fail(TEXT("owned scope Optional leaf registration refused"));
   Advance();return false;
  }
  if (Phase==2)
  {
   if (GFrameCounter<=PublicationFrame || !Intrinsic()) return false;
   if (!Start()) return Fail(TEXT("actual Hub managed held-worker scope setup refused"));
   Phase=3;return false;
  }
  if (Phase==3)
  {
   if (!Probe->BothHeldOnGT()) return false;
   if (!Operations || Operations->NumOccupied()!=1 || !Gate || Gate->NumOwnedOperations()!=1 || Gate->NumOwnedInputs()!=1 ||
    !RetainedWhileHeld() || !BindObserver())
    return Fail(TEXT("actual parsed worker/original GTK owners were not held under the matching operation"));
   // The actual Hub sequence closes all gates, retires its clients, then requests native scope retirement.
   // No raw store survives that call: its owning slot may subsequently retire/recycle.
   RetireOwner();
   if (!Runtime || !Runtime->IsClosedOnGT() || !Gate->IsClosed() || !LiveControl() || !RetainedWhileHeld())
    return Fail(TEXT("actual Hub retirement did not close admission and suppress return while the worker stayed held"));
   if (!ReleaseOwnCompletedClock()) return Fail(TEXT("matching completed physical clock view could not be released"));
   PublicationFrame=GFrameCounter;Phase=4;return false;
  }
  if (Phase==4)
  {
   if (GFrameCounter<=PublicationFrame) return false;
   if (!RecordClockReleased || !RetainedWhileHeld() || !Runtime->IsClosedOnGT() || !Gate->IsClosed() ||
    Gate->NumOwnedOperations()!=1 || Gate->NumOwnedInputs()!=1 || !WeakAccess.IsValid() || !LiveControl())
    return Fail(TEXT("closed scope lost its real held authority or prematurely disposed the worker-borrowed GTK owners"));
   RetainedBeforeRelease=true;
   Probe->Release();Phase=5;return false;
  }
  if (Phase==5)
  {
   if (!OwnedQuiescent() || WeakDestination.IsValid()) return false;
   if (!ReturnMarks() || !Runtime || !Runtime->IsClosedOnGT() || !Gate->IsClosed() || !LiveControl() ||
    Testing::LiveDynamicCarriersOnGT()!=CarrierBaseline)
    return Fail(TEXT("actual closed-scope return did not retire only its own GTK/frame/input owners under a live service"));
   DropQuiescentOwners();Phase=6;return false;
  }
  if (WeakClock.IsValid()) return false;
  if (!RetainedBeforeRelease || !RecordClockReleased || !ReturnMarks() || Probe->ClocksDestroyed.load()!=1 || !LiveControl())
   return Fail(TEXT("closed-scope final original clock/return retirement was not exactly once on GT"));
  Test.AddInfo(FString::Printf(TEXT("DynamicHeldScope: retired=1 retainedBeforeRelease=1 viewReleased=1 callback=%d materialized=%d delivered=%d return=%d/%d capture=%d/%d access=%d disposed=%d frame=%d work=%u inputs=%llu clock=%d live=1 result=1"),
   Probe->Callbacks.load(),Probe->Materializations.load(),Probe->Deliveries.load(),Probe->Count(EPublication::ManagedReturnExecuted),
   Probe->Count(EPublication::ManagedReturnCompleted),Probe->Count(EPublication::ManagedReturnCaptureRan),Probe->Count(EPublication::ManagedReturnCaptureAbandoned),
   Probe->Count(EPublication::ManagedAccessEnded),Probe->Disposed.load(),Probe->Destroyed.load(),Gate->NumOwnedOperations(),
   static_cast<unsigned long long>(Gate->NumOwnedInputs()),Probe->ClocksDestroyed.load()));
  CleanupTypes();return true;
 }
private:
 bool Intrinsic()
 {
  FVaRestDynamicResultConfig Config;Config.ResponseType.Kind=EVaRestTypeKind::Struct;Config.ResponseType.Type=FVaRestJson::StaticStruct();
  FString Error,Code;const auto State=FDynamicTypedState::CreateOnGT(Config,Error,&Code);
  if (!State && Code==TEXT("EditorMutationHooksUnavailable")) { Fail(TEXT("real editor mutation hooks are unavailable"));return false; }
  return State.IsValid();
 }
 void Advance() { PublicationFrame=GFrameCounter;++Phase; }
 bool Start()
 {
  const auto Trace=ModuleStopFixture::ObserveTraceSelectionOnGT(false,nullptr);
  if (!Trace.MembershipMatches || !Trace.MaskMatches)
  { Test.AddInfo(FString::Printf(TEXT("held scope actual trace state: sinks=%d mask=%u"),Trace.SinkCount,Trace.Mask));return false; }
  Rig=MakeUnique<Testing::FTestRig>(TEXTVIEW("https://held-scope-retirement.test"));Rig->Fake->bCompleteOnWorker=false;
  const auto Clock=MakeShared<FClock,ESPMode::ThreadSafe>(Rig->Clock,Probe);WeakClock=Clock;
  FVaRestTraceScope Info;Info.Label=TEXT("DynamicHeldWorkerScopeRetirement");FString Error;
  Runtime=FHubRuntime::CreateOnGT(MoveTemp(Info),TEXT("HeldWorkerScopeRetirement"),Error);
  if (!Runtime) return false;
  ScopeId=Runtime->ScopeId();Runtime->ConnectivityOnGT()->ReportOnline(); // this owned host, never the default reachability
  auto Initial=FClientAccess::InitialConfig(TEXTVIEW("https://held-scope-retirement.test"));
  Initial.Transport=Rig->Fake;Initial.Clock=Clock;Initial.LogLevel=EVaRestLogLevel::None;
  Rig->Client=Runtime->CreateClientOnGT(MoveTemp(Initial),Error);
  const auto State=FClientAccess::State(Rig->Client);
  if (!State || State->GetScopeId()!=ScopeId || !State->GetManagedLifetime().IsValid()) return false;
  const auto& Registration=State->GetManagedLifetime();Control=Registration.Control;Gate=Registration.Gate;Operations=Registration.Operations;
  if (!LiveControl() || !Operations || Operations->NumOccupied()!=0 || Gate->IsClosed()) return false;
  auto Destination=MakeShared<ModuleStopFixture::FDestination,ESPMode::ThreadSafe>();
  if (!Destination->Initialize(EVaRestTypeContainer::None,nullptr)) return false;
  WeakDestination=Destination; // the runner never keeps an extra strong destination/preparation owner
  FVaRestDynamicResultConfig Config;Config.ResponseType.Kind=EVaRestTypeKind::Struct;
  Config.ResponseType.Type=FVaRestDynamicLifecycleRoot::StaticStruct();Config.Budget=Runtime->DynamicBudgetOnGT();
  auto Request=Rig->Client.RequestDynamic(TEXTVIEW("GET"),TEXTVIEW("/held-scope"),MoveTemp(Config));
  if (!FDynamicRequestAccess::BindModuleStopProbeOnGT(Request,Probe) ||
   !FDynamicRequestAccess::SetPreparationOnGT(Request,[Owned=MoveTemp(Destination),Marker=MakeShared<FCapture,ESPMode::ThreadSafe>(Probe,true)](FDynamicPrepareToken& Token) { (void)Marker;Owned->Prepare(Token); }))
  { FDynamicRequestAccess::DiscardOnGT(Request);return false; }
  Rig->Fake->Enqueue(FVaRestFakeResponse::Text(200,TEXT("{\"name\":\"stop-native\",\"maybe\":{\"text\":\"nested\",\"number\":23}}"),TEXTVIEW("application/json")));
  Handle=MoveTemp(Request).OnComplete([Native=Probe,Marker=MakeShared<FCapture,ESPMode::ThreadSafe>(Probe,false)](const TVaRestResult<FVaRestDynamicResult>& Result) {
   (void)Marker;Native->PublicResult(!Result.Error.IsError() && Result.Value.IsSet());
  }).Send();
  return Handle.GetId()!=0;
 }
 bool BindObserver()
 {
  bool Bound=false;const auto Identity=Probe->IdentityOnGT();
  const auto State=FClientAccess::State(Rig->Client);
  if (!State || State->GetScopeId()!=ScopeId || Identity.Request!=Handle.GetId() || Identity.Client!=State->GetClientId()) return false;
  State->ForEachInFlight([&](FExchangeState& Exchange) {
   if (Exchange.RequestId!=Identity.Request || Exchange.ClientId!=Identity.Client) return;
   auto Access=Exchange.NativeCompletion.Pin();
   if (Access) { WeakAccess=Access;Bound=Access->ObserveRetirementForTestsOnGT(Identity,Probe); }
   Access.Reset();
   if (Access.IsValid()) Bound=false;
  });
  const auto Clock=WeakClock.Pin();
  return Bound && Clock && Probe->ClockOnGT()==static_cast<uint64>(reinterpret_cast<UPTRINT>(Clock.Get()));
 }
 bool LiveControl() const
 { return Control && Control==NativeControlForNewClientOnGT() && !Control->IsStopped() && !Control->IsExitRetentionRequested(); }
 bool RetainedWhileHeld() const
 {
  return Probe->BothHeldOnGT() && WeakDestination.IsValid() && Probe->PreparationDestroyed.load()==0 && Probe->CallbackDestroyed.load()==0 &&
   Probe->Callbacks.load()==0 && Probe->Materializations.load()==0 && Probe->Deliveries.load()==0 && Probe->Disposed.load()==0 &&
   Probe->Destroyed.load()==0 && Probe->ClocksDestroyed.load()==0 && Probe->WrongThread.load()==0 &&
   Probe->Count(EPublication::ManagedWorkerEnded)==0 && Probe->Count(EPublication::ManagedReturnExecuted)==0 &&
   Probe->Count(EPublication::ManagedReturnCompleted)==0 && Probe->Count(EPublication::ManagedReturnCaptureRan)==0 &&
   Probe->Count(EPublication::ManagedReturnCaptureAbandoned)==0 && Probe->Count(EPublication::ManagedAccessEnded)==0;
 }
 bool ReturnMarks() const
 {
  return Probe->Disposed.load(std::memory_order_acquire)==1 && Probe->Destroyed.load()==1 &&
   Probe->PreparationDestroyed.load()==1 && Probe->CallbackDestroyed.load()==1 && Probe->WrongThread.load()==0 &&
   Probe->Callbacks.load()==0 && Probe->Materializations.load()==0 && Probe->Deliveries.load()==0 &&
   Probe->Count(EPublication::ManagedWorkerEnded)==1 && Probe->Count(EPublication::ManagedReturnExecuted)==1 &&
   Probe->Count(EPublication::ManagedReturnCompleted)==1 && Probe->Count(EPublication::ManagedReturnCaptureRan)==1 &&
   Probe->Count(EPublication::ManagedReturnCaptureAbandoned)==0 && Probe->Count(EPublication::ManagedAccessEnded)==1;
 }
 bool ReleaseOwnCompletedClock()
 {
  if (RecordClockReleased) return true;
  if (!Rig || Handle.GetId()==0 || Rig->Fake->NumCalls()!=1 || !Rig->Fake->GetCall(0).bCompleted ||
   Rig->Fake->GetCall(0).Request.RequestId!=Handle.GetId() || !Rig->Fake->GetCall(0).Request.Clock.IsValid()) return false;
  RecordClockReleased=Rig->Fake->ReleaseCompletedClock(0) && !Rig->Fake->GetCall(0).Request.Clock.IsValid();
  return RecordClockReleased;
 }
 void RetireOwner()
 {
  Operations=nullptr; // no raw store access after the owner may retire/recycle its client slot
  if (Runtime && !Retired) { Retired=true;Runtime->RetireOnGT(); }
 }
 bool OwnedQuiescent() const
 { return (!Gate || (Gate->NumOwnedOperations()==0 && Gate->NumOwnedInputs()==0)) && !WeakAccess.IsValid(); }
 void DropQuiescentOwners()
 {
  check(OwnedQuiescent());
  Rig.Reset();Handle=FVaRestRequestHandle();Runtime.Reset();
 }
 bool Fail(const TCHAR* Message)
 {
  if (!Closing) { Test.AddError(Message);Closing=true;RetireOwner();Probe->Release(); }
  return false;
 }
 bool CloseStep()
 {
  Probe->Release();RetireOwner();
  if (Handle.GetId()!=0 && !RecordClockReleased && !ReleaseOwnCompletedClock())
  {
   // Failure may precede physical Start, or its matching completed view may already be absent.
   // This is not a successful view-release observation and never sets RecordClockReleased.
   const int32 Calls=Rig ? Rig->Fake->NumCalls() : -1;
   const bool NoPhysical=Rig && (Calls==0 || (Calls==1 && Rig->Fake->GetCall(0).bCompleted &&
    Rig->Fake->GetCall(0).Request.RequestId==Handle.GetId() && !Rig->Fake->GetCall(0).Request.Clock.IsValid()));
   const bool NoWorker=!Probe->BothHeldOnGT() || Probe->Count(EPublication::ManagedWorkerEnded)==1;
   if (!OwnedQuiescent() || !NoPhysical || !NoWorker) return false;
  }
  if (!OwnedQuiescent()) return false; // no timeout frees a rig/frame/clock still referenced by native work
  DropQuiescentOwners();
  if (WeakAccess.IsValid() || WeakDestination.IsValid() || WeakClock.IsValid()) return false;
  CleanupTypes();return true; // remains an automation failure; no synthetic lifecycle pass
 }
 void CleanupTypes()
 {
  if (RegisteredRoot) { RegisteredRoot=false;FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleRoot::StaticStruct()); }
  if (RegisteredLeaf) { RegisteredLeaf=false;FVaRestDynamicNativeTypes::UnregisterGameThreadType(FVaRestDynamicLifecycleLeaf::StaticStruct()); }
 }
 FAutomationTestBase& Test;
 TSharedRef<FProbe,ESPMode::ThreadSafe> Probe;
 const double Deadline;
 const int32 CarrierBaseline;
 TUniquePtr<Testing::FTestRig> Rig;
 TSharedPtr<FHubRuntime,ESPMode::ThreadSafe> Runtime;
 TSharedPtr<FNativeLifetimeControl,ESPMode::ThreadSafe> Control;
 TSharedPtr<FClientAdmissionGate,ESPMode::ThreadSafe> Gate;
 FNativeOperationStore* Operations=nullptr;
 TWeakPtr<FManagedCompletionAccess,ESPMode::ThreadSafe> WeakAccess;
 TWeakPtr<ModuleStopFixture::FDestination,ESPMode::ThreadSafe> WeakDestination;
 TWeakPtr<FClock,ESPMode::ThreadSafe> WeakClock;
 FVaRestRequestHandle Handle;
 uint64 ScopeId=0,PublicationFrame=0;
 int32 Phase=0;
 bool RegisteredRoot=false,RegisteredLeaf=false,Retired=false,Closing=false,RecordClockReleased=false,RetainedBeforeRelease=false;
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestDynamicHeldWorkerScopeRetirementTest,"VaRest.Http.Dynamic.HeldWorkerScopeRetirement",
 (WITH_EDITOR ? EAutomationTestFlags::EditorContext : (EAutomationTestFlags::ClientContext | EAutomationTestFlags::ServerContext)) | EAutomationTestFlags::EngineFilter)
void FVaRestDynamicHeldWorkerScopeRetirementTest::GetTests(TArray<FString>& Names,TArray<FString>& Parameters) const
{
 if (FParse::Param(FCommandLine::Get(),TEXT("VaRestM5HeldScopeStopProof"))) { Names.Add(TEXT("ClientScope"));Parameters.Add(TEXT("ClientScope")); }
}
bool FVaRestDynamicHeldWorkerScopeRetirementTest::RunTest(const FString& Parameters)
{
 if (!FParse::Param(FCommandLine::Get(),TEXT("VaRestM5HeldScopeStopProof")) || Parameters!=TEXT("ClientScope"))
 { AddError(TEXT("select the dedicated ClientScope fixture with -VaRestM5HeldScopeStopProof"));return false; }
 if (!FPlatformProcess::SupportsMultithreading()) { AddError(TEXT("scope retirement requires a genuine managed native worker"));return false; }
 ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::HeldRetirementFixture::FScopeStopRun(*this));
 return true;
}
#endif
