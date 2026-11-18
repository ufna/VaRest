// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "Modules/ModuleManager.h"

#if WITH_DEV_AUTOMATION_TESTS && PLATFORM_LINUX
#include "VaRestModuleStopTest.h"
#include "Features/IModularFeatures.h"
#include "Containers/StringConv.h"
#include "HAL/PlatformTLS.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace VaRest::ModuleStopTest
{
// This channel is opened before arming. It has no FArchive, IFileManager, log redirector,
// UObject, callback, task or module/service owner. Normal shutdown leaves this DLL's code mapped.
class FNativeChannel final
{
public:
 explicit FNativeChannel(std::FILE* InFile) : File(InFile) {}
 ~FNativeChannel() { if (File) std::fclose(File); }
 bool Emit(const char* Event, FWorkerIdentity Id = {}, uint32 Thread = 0, bool GT = true,
  bool Stopped = false, int64 A = 0, int64 B = 0, int64 C = 0, int64 D = 0)
 {
  std::lock_guard<std::mutex> Guard(Lock);
  if (!File || Failed.load(std::memory_order_acquire)) return false;
  const int Wrote=std::fprintf(File,
   "{\"seq\":%llu,\"event\":\"%s\",\"route\":%u,\"request\":%llu,\"client\":%llu,\"workerGeneration\":%llu,\"token\":%llu,\"cell\":%u,\"cellGeneration\":%llu,\"thread\":%u,\"gt\":%u,\"stopped\":%u,\"a\":%lld,\"b\":%lld,\"c\":%lld,\"d\":%lld}\n",
   static_cast<unsigned long long>(++Sequence),Event,static_cast<unsigned>(Id.Route),
   static_cast<unsigned long long>(Id.Request),static_cast<unsigned long long>(Id.Client),
   static_cast<unsigned long long>(Id.Generation),static_cast<unsigned long long>(Id.NativeToken),static_cast<unsigned>(Id.Cell),
   static_cast<unsigned long long>(Id.CellGeneration),Thread,unsigned(GT),unsigned(Stopped),
   static_cast<long long>(A),static_cast<long long>(B),static_cast<long long>(C),static_cast<long long>(D));
  const bool Good=Wrote>0 && std::fflush(File)==0 && ::fsync(::fileno(File))==0;
  if (!Good) Failed.store(true,std::memory_order_release);
  return Good;
 }
 bool Good() const { return !Failed.load(std::memory_order_acquire); }
private:
 std::FILE* File=nullptr;
 std::mutex Lock;
 uint64 Sequence=0;
 std::atomic<bool> Failed{false};
};

class FNativeProbe final : public INativeProbe
{
public:
 explicit FNativeProbe(TSharedRef<FNativeChannel,ESPMode::ThreadSafe> InChannel)
  : Channel(MoveTemp(InChannel)),Deadline(std::chrono::steady_clock::now()+std::chrono::seconds(60)) {}
 void HoldAfterNativeParse(FWorkerIdentity Id,bool Parsed,bool TimeDecode,uint64 ClockAddress,uint32 Thread,bool OnGT) override
 {
  const unsigned R=static_cast<unsigned>(Id.Route);
  if (R>1) { FailNative(); return; }
  if (OnGT || !Parsed || !Thread || !Id.Request || !Id.Client || !Id.NativeToken ||
   (Id.Route==ERoute::Ordinary && (Id.Generation || Id.CellGeneration || Id.Cell)) ||
   (Id.Route==ERoute::Managed && (Id.Generation!=1 || !Id.CellGeneration))) { FailNative(); return; }
  bool Expected=false;
  if (!Held[R].compare_exchange_strong(Expected,true,std::memory_order_acq_rel)) { FailNative(); return; }
  Identities[R]=Id; ParseThreads[R]=Thread; ActualTiming[R]=TimeDecode;ActualClock[R]=ClockAddress; // immutable before Entered release; later publications acquire it
  if (!Channel->Emit("native-parsed-held",Id,Thread,OnGT,false,Parsed,TimeDecode,static_cast<int64>(ClockAddress))) { FailNative(); return; }
  Entered[R].store(true,std::memory_order_release);
  while (!Released.load(std::memory_order_acquire))
  {
   if (std::chrono::steady_clock::now()>=Deadline)
   {
    Channel->Emit("native-deadline-failed",Id,Thread,OnGT);
    FailNative(); return; // finite failure escape, never fabricated settlement/success
   }
   std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool Gone=HttpRemoved.load(std::memory_order_acquire);
  const bool Wrote=Channel->Emit("native-gate-return",Id,Thread,OnGT,Gone,Gone);
  if (!Gone || !Wrote) { FailNative(); return; }
  Returned[R].store(true,std::memory_order_release);
 }
 void Published(FWorkerIdentity Id,EPublication Event,uint32 Thread,bool OnGT,bool Stopped) override
 {
  const unsigned R=static_cast<unsigned>(Id.Route);
  const bool Matches=R<2 && Entered[R].load(std::memory_order_acquire) && Same(Identities[R],Id);
  const char* Name="invalid-publication";
  bool KindGood=false,FinalWorker=false;
  switch (Event)
  {
   case EPublication::OrdinaryReady:Name="ordinary-ready-published";KindGood=Id.Route==ERoute::Ordinary;FinalWorker=true;break;
   case EPublication::ManagedWorkerEnded:Name="managed-worker-ended-published";KindGood=Id.Route==ERoute::Managed;FinalWorker=true;break;
   case EPublication::ManagedAccessEnded:Name="managed-access-ended-published";KindGood=Id.Route==ERoute::Managed;break;
   case EPublication::ManagedReturnExecuted:Name="managed-prerequisite-return-executed";KindGood=Id.Route==ERoute::Managed;break;
   case EPublication::ManagedReturnCompleted:Name="managed-prerequisite-return-completed";KindGood=Id.Route==ERoute::Managed;break;
   case EPublication::ManagedReturnCaptureRan:Name="managed-prerequisite-capture-destroyed-ran";KindGood=Id.Route==ERoute::Managed;break;
   case EPublication::ManagedReturnCaptureAbandoned:Name="managed-prerequisite-capture-destroyed-abandoned";KindGood=Id.Route==ERoute::Managed;break;
  }
  const bool Gone=HttpRemoved.load(std::memory_order_acquire);
  // a=actual after-removal flag, b=actual parse thread, c=identity match. Scheduler token destruction
  // may run on another worker; record both actual threads, do not invent same-thread continuity.
  const bool Wrote=Channel->Emit(Name,Id,Thread,OnGT,Stopped,Gone,Matches ? ParseThreads[R] : 0,Matches);
  if (!Matches || !KindGood || !Thread || !Wrote) { FailNative(); return; }
  if (FinalWorker)
  {
   if (OnGT || !Stopped || !Gone || !Returned[R].load(std::memory_order_acquire)) { FailNative(); return; }
   bool Expected=false;
   if (!Ended[R].compare_exchange_strong(Expected,true,std::memory_order_acq_rel)) { FailNative(); return; }
  }
  if ((Event==EPublication::ManagedReturnExecuted || Event==EPublication::ManagedReturnCompleted) && (!OnGT || !Stopped || !Gone)) FailNative();
  // Observation only: EXTERNAL acceptance also needs actual SoftQuit/nonforced exit0, complete
  // capture outcome/quarantine review, and exact source/build/package lineage.
  if (Ended[0].load(std::memory_order_acquire) && Ended[1].load(std::memory_order_acquire) &&
   !Failed.load(std::memory_order_acquire) && Channel->Good())
  {
   bool Expected=false;
   if (Final.compare_exchange_strong(Expected,true,std::memory_order_acq_rel))
    if (!Channel->Emit("both-late-publications-observed",{},Thread,OnGT,true)) FailNative();
  }
 }
 void ReflectedMaterializationOnGT() override
 { check(IsInGameThread()); Channel->Emit("unexpected-reflected-materialization"); FailNative(); }
 void UserDeliveryOnGT() override
 { check(IsInGameThread()); Channel->Emit("unexpected-user-delivery"); FailNative(); }
 bool BothHeldOnGT() const override
 { check(IsInGameThread()); return Entered[0].load(std::memory_order_acquire) && Entered[1].load(std::memory_order_acquire); }
 bool FailedOnGT() const override
 { check(IsInGameThread()); return Failed.load(std::memory_order_acquire) || !Channel->Good(); }
 bool ArmOnGT() override
 {
  check(IsInGameThread());
  if (!BothHeldOnGT() || FailedOnGT() || !SetupGood || !ClockGood) { FailNative(); return false; }
  Armed.store(true,std::memory_order_release);
  if (!Channel->Emit("armed-only-not-pass",{},FPlatformTLS::GetCurrentThreadId())) { FailNative(); return false; }
  return true;
 }
 void SetupOnGT(bool Unknown,bool Leaf,bool Materialized,bool ShellOffGT) override
 {
  check(IsInGameThread()); SetupGood=Unknown && Leaf && Materialized && ShellOffGT;
  if (!Channel->Emit("setup-native-positive-and-refusals",{},FPlatformTLS::GetCurrentThreadId(),true,false,Unknown,Leaf,Materialized,ShellOffGT))
   SetupGood=false;
  if (!SetupGood) FailNative();
 }
 bool ObserveClockOnGT(bool Timing,uint32 TraceMask,uint64 OrdinaryClock,uint64 ManagedClock,
  int64 OrdinaryGT,int64 OrdinaryWorker,int64 ManagedGT,int64 ManagedWorker) override
 {
  check(IsInGameThread());
  if (!BothHeldOnGT()) { FailNative();return false; }
  ExpectedTiming=Timing;
  // Public IVaRestClock::Now is any-thread. The managed parser calls this SAME injected
  // original clock twice for timing; ordinary dynamic parsing has stripped its clock on GT.
  ClockGood=TraceMask==(Timing ? 1u : 0u) && ActualTiming[0]==Timing && ActualTiming[1]==Timing &&
   OrdinaryClock && ManagedClock && !ActualClock[0] && ActualClock[1]==ManagedClock &&
   OrdinaryGT>0 && ManagedGT>0 && OrdinaryWorker==0 && ManagedWorker==(Timing ? 2 : 0);
  const bool Selection=Channel->Emit("clock-trace-selection",{},FPlatformTLS::GetCurrentThreadId(),true,false,
   Timing,TraceMask,ActualTiming[0],ActualTiming[1]);
  const bool Identity=Channel->Emit("clock-actual-input-identities",{},FPlatformTLS::GetCurrentThreadId(),true,false,
   static_cast<int64>(OrdinaryClock),static_cast<int64>(ManagedClock),static_cast<int64>(ActualClock[0]),static_cast<int64>(ActualClock[1]));
  const bool Calls=Channel->Emit("clock-actual-now-counts",{},FPlatformTLS::GetCurrentThreadId(),true,false,
   OrdinaryGT,OrdinaryWorker,ManagedGT,ManagedWorker);
  ClockGood &= Selection && Identity && Calls;
  if (!ClockGood) FailNative();
  return ClockGood;
 }
 void FailOnGT() override { check(IsInGameThread()); FailNative(); }
 void ObservePreExitOnGT(const FGTObservation& O)
 {
  check(IsInGameThread());
  const bool ReflectionWrote=Channel->Emit("gt-preexit-owned-reflection",{},FPlatformTLS::GetCurrentThreadId(),true,O.ControlStopped,
   O.FramesStopped,O.OwnedReflectionRetired,O.Carriers,O.CarrierBaseline);
  const bool RetentionWrote=Channel->Emit("gt-preexit-native-retention",{},FPlatformTLS::GetCurrentThreadId(),true,O.ControlStopped,
   O.GateClosed,O.ExitRetention,O.OccupiedClients,O.OccupiedCells);
  const bool CountsWrote=Channel->Emit("gt-preexit-counted-obligations",{},FPlatformTLS::GetCurrentThreadId(),true,O.ControlStopped,
   O.CountedOperations,O.PendingInputs,O.AllNativeOperationsOnGT);
  const bool ClockWrote=Channel->Emit("gt-preexit-clock-worker-counts",{},FPlatformTLS::GetCurrentThreadId(),true,O.ControlStopped,
   O.OrdinaryClockWorkerCalls,O.ManagedClockWorkerCalls,ExpectedTiming);
  const bool TraceWrote=Channel->Emit("gt-preexit-trace-membership",{},FPlatformTLS::GetCurrentThreadId(),true,O.ControlStopped,
   O.TraceMembershipMatches,O.TraceMaskMatches,O.TraceSinkCount,O.TraceMask);
  const bool TraceGood=O.TraceMembershipMatches && O.TraceMaskMatches &&
   O.TraceSinkCount==(ExpectedTiming ? 1 : 0) && O.TraceMask==(ExpectedTiming ? 1u : 0u);
  PreExitGood=TraceGood && TraceWrote && O.FramesStopped && O.OwnedReflectionRetired && O.ControlStopped && O.GateClosed &&
   O.ExitRetention && O.OccupiedClients>0 && O.OccupiedCells>0 && O.CountedOperations>0 &&
   O.Carriers==O.CarrierBaseline && O.AllNativeOperationsOnGT && ReflectionWrote && RetentionWrote && CountsWrote &&
   ClockGood && ClockWrote && O.OrdinaryClockWorkerCalls==0 && O.ManagedClockWorkerCalls==(ExpectedTiming ? 2 : 0);
  // Caller invoked then reset/destroyed its real GT observer before entering this native-only method.
  if (!Channel->Emit("gt-observer-destroyed-before-latephase",{},FPlatformTLS::GetCurrentThreadId(),true,O.ControlStopped,PreExitGood))
   PreExitGood=false;
  if (!PreExitGood || !Armed.load(std::memory_order_acquire)) FailNative();
 }
 void ReleaseAfterHttpRemovalOnGT(bool Removed)
 {
  check(IsInGameThread());
  const bool Wrote=Channel->Emit("witness-shutdown-http-module-removed",{},FPlatformTLS::GetCurrentThreadId(),true,Removed,Removed,PreExitGood);
  if (!Wrote || !Removed || !PreExitGood || !Armed.load(std::memory_order_acquire) || FailedOnGT()) { FailNative(); return; }
  HttpRemoved.store(true,std::memory_order_release);
  Released.store(true,std::memory_order_release); // ONLY actual witness ShutdownModule releases a successful experiment
 }
private:
 static bool Same(const FWorkerIdentity& A,const FWorkerIdentity& B)
 {
  return A.Route==B.Route && A.Request==B.Request && A.Client==B.Client && A.Generation==B.Generation &&
   A.CellGeneration==B.CellGeneration && A.Cell==B.Cell && A.NativeToken==B.NativeToken;
 }
 void FailNative()
 {
  if (!Failed.exchange(true,std::memory_order_acq_rel)) Channel->Emit("experiment-failed");
  Released.store(true,std::memory_order_release); // failure escape always leaves an explicit failed experiment
 }
 TSharedRef<FNativeChannel,ESPMode::ThreadSafe> Channel;
 const std::chrono::steady_clock::time_point Deadline;
 FWorkerIdentity Identities[2]{};
 uint32 ParseThreads[2]{};
 bool ActualTiming[2]{};
 uint64 ActualClock[2]{};
 std::atomic<bool> Held[2]{},Entered[2]{},Returned[2]{},Ended[2]{};
 std::atomic<bool> Released{false},HttpRemoved{false},Failed{false},Armed{false},Final{false};
 bool SetupGood=false,PreExitGood=false,ClockGood=false,ExpectedTiming=false; // GT only
};

// The supplied lineage/case cannot inject rows into a diagnostic. They remain caller claims, not build verification.
bool SafeLabel(const FString& Text)
{
 if (Text.IsEmpty() || Text.Len()>160) return false;
 for (const TCHAR C:Text) if (!((C>='a' && C<='z') || (C>='A' && C<='Z') ||
  (C>='0' && C<='9') || C=='.' || C=='_' || C=='-')) return false;
 return true;
}
}

class FVaRestM5StopWitnessModule final : public IModuleInterface, public VaRest::ModuleStopTest::IWitness
{
public:
 void StartupModule() override
 {
  using namespace VaRest::ModuleStopTest;
  check(IsInGameThread());
  if (!FParse::Param(FCommandLine::Get(),TEXT("VaRestM5ModuleStopProof"))) return;
  FString Path,Lineage;
  if (!FParse::Value(FCommandLine::Get(),TEXT("VaRestM5StopEvidence="),Path) || FPaths::IsRelative(Path) ||
   !FParse::Value(FCommandLine::Get(),TEXT("VaRestM5StopLineage="),Lineage) || !SafeLabel(Lineage)) return;
  // Linux UTF-8 pathname. Exclusive create prevents overwriting or appending to prior evidence.
  std::FILE* File=std::fopen(TCHAR_TO_UTF8(*Path),"wx");
  if (!File) return;
  Channel=MakeShared<FNativeChannel,ESPMode::ThreadSafe>(File);
  const FString Header=TEXT("lineage-label-")+Lineage;
  if (!Channel->Emit(TCHAR_TO_UTF8(*Header))) { Channel.Reset(); return; }
  IModularFeatures::Get().RegisterModularFeature(FeatureName(),this);
  bRegistered=true;
  bEarlier=!FModuleManager::Get().IsModuleLoaded(TEXT("VaRestHttp"));
  Channel->Emit("witness-startup-before-http",{},FPlatformTLS::GetCurrentThreadId(),true,false,bEarlier);
  PreExit=FCoreDelegates::OnPreExit.AddLambda([this]
  {
   check(IsInGameThread());
   // Actual OnPreExit is later than the entire real UEngine::PreExit/OnEnginePreExit phase.
   if (Probe && GTObserver)
   {
    const auto Observation=GTObserver();
    GTObserver.Reset(); // destroys EVERY callback/GT-only owner before the native late phase
    Probe->ObservePreExitOnGT(Observation);
   }
   else if (Probe) Probe->FailOnGT();
  });
 }
 void ShutdownModule() override
 {
  using namespace VaRest::ModuleStopTest;
  check(IsInGameThread());
  FCoreDelegates::OnPreExit.Remove(PreExit);
  if (GTObserver) { GTObserver.Reset(); if (Probe) Probe->FailOnGT(); }
  // ModuleManager resets the Http interface only AFTER its real ShutdownModule returned.
  // False here also authenticates the observed inverse-completed-startup ordering.
  if (Probe) Probe->ReleaseAfterHttpRemovalOnGT(!FModuleManager::Get().IsModuleLoaded(TEXT("VaRestHttp")));
  if (bRegistered) IModularFeatures::Get().UnregisterModularFeature(FeatureName(),this);
  Probe.Reset(); Channel.Reset(); // existing worker captures now own only native probe/channel state
 }
 bool SupportsDynamicReloading() override { return false; }
 bool StartupPrecededHttpOnGT() const override { check(IsInGameThread()); return bEarlier; }
 TSharedPtr<VaRest::ModuleStopTest::INativeProbe,ESPMode::ThreadSafe> CreateProbeOnGT(const FString& Case) override
 {
  using namespace VaRest::ModuleStopTest;
  check(IsInGameThread());
  if (!bEarlier || Probe || !Channel || !Channel->Good() || !SafeLabel(Case) ||
   !FModuleManager::Get().IsModuleLoaded(TEXT("VaRestHttp"))) return {};
  const FString Header=TEXT("case-")+Case;
  Channel->Emit(TCHAR_TO_UTF8(*Header));
  Channel->Emit("http-loaded-at-fixture-setup",{},FPlatformTLS::GetCurrentThreadId(),true,false,1);
  Probe=MakeShared<FNativeProbe,ESPMode::ThreadSafe>(Channel.ToSharedRef());
  return Probe;
 }
 bool SetGTObserverOnGT(TUniqueFunction<VaRest::ModuleStopTest::FGTObservation()>&& Observer) override
 {
  check(IsInGameThread());
  if (!Probe || GTObserver || !Observer) return false;
  GTObserver=MoveTemp(Observer); return true;
 }
private:
 TSharedPtr<VaRest::ModuleStopTest::FNativeChannel,ESPMode::ThreadSafe> Channel;
 TSharedPtr<VaRest::ModuleStopTest::FNativeProbe,ESPMode::ThreadSafe> Probe;
 TUniqueFunction<VaRest::ModuleStopTest::FGTObservation()> GTObserver;
 FDelegateHandle PreExit;
 bool bEarlier=false,bRegistered=false;
};
#else
class FVaRestM5StopWitnessModule final : public IModuleInterface {};
#endif
IMPLEMENT_MODULE(FVaRestM5StopWitnessModule, VaRestM5StopWitness);
