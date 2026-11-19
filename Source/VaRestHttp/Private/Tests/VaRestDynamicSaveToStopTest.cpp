// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && PLATFORM_LINUX
#include "VaRestDynamicRequest.h"
#include "CoreGlobals.h"
#include "VaRestDynamicFileCleanup.h"
#include "VaRestClientState.h"
#include "VaRestHttpTesting.h"
#include "VaRestTrace.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Async/Fundamental/Scheduler.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <unistd.h>

namespace VaRest::Http::PrepublishFileTest
{
class FOwnedDirectory final
{
 FAutomationTestBase& Test;
 char Name[sizeof("/tmp/varest-dynamic-file-XXXXXX")]="/tmp/varest-dynamic-file-XXXXXX";
 struct stat Directory{};
 TMap<FString,struct stat> Files;
 bool Created=false,Verified=false,Finished=false;
public:
 explicit FOwnedDirectory(FAutomationTestBase& T):Test(T)
 {
  if (!::mkdtemp(Name)) { Test.AddError(TEXT("dynamic file mkdtemp failed"));return; }
  Created=true;
  Verified=::lstat(Name,&Directory)==0 && S_ISDIR(Directory.st_mode) && Directory.st_uid==::geteuid() && (Directory.st_mode&0777)==0700;
  if (!Verified) Test.AddError(TEXT("dynamic file temp directory identity refused"));
 }
 ~FOwnedDirectory() { if (Created && !Finished) Test.AddError(TEXT("dynamic file fixture retained its owned directory because native quiescence/cleanup was not proved")); }
 bool Ready() const { return Verified; }
 FString Root() const { return FString(UTF8_TO_TCHAR(Name)); }
 FString Destination() const { return Root()/TEXT("destination.bin"); }
 bool Observe(const FString& Path,bool Replace=false)
 {
  if (!Verified || (Path!=Destination() && !Path.StartsWith(Destination()+TEXT(".varest-")))) return false;
  struct stat S{};
  if (::lstat(TCHAR_TO_UTF8(*Path),&S)!=0 || !S_ISREG(S.st_mode) || S.st_uid!=::geteuid() || S.st_nlink!=1) return false;
  if (const auto* Old=Files.Find(Path);Old && !Replace && (Old->st_dev!=S.st_dev || Old->st_ino!=S.st_ino)) return false;
  Files.Add(Path,S);return true;
 }
 bool Finish()
 {
  Finished=true;
  struct stat S{};
  if (!Verified || ::lstat(Name,&S)!=0 || !S_ISDIR(S.st_mode) || S.st_dev!=Directory.st_dev || S.st_ino!=Directory.st_ino || S.st_uid!=::geteuid()) return false;
  bool Ok=true;
  for (const auto& Entry:Files)
  {
   const auto& Path=Entry.Key;const auto& Old=Entry.Value;
   if (::lstat(TCHAR_TO_UTF8(*Path),&S)==0)
   {
    if (!S_ISREG(S.st_mode) || S.st_uid!=::geteuid() || S.st_nlink!=1 || S.st_dev!=Old.st_dev || S.st_ino!=Old.st_ino)
    { Test.AddError(TEXT("dynamic file cleanup refuses a replaced/unknown file"));Ok=false; }
    else if (::unlink(TCHAR_TO_UTF8(*Path))!=0) { Test.AddError(TEXT("dynamic file exact-owned unlink failed"));Ok=false; }
   }
   else if (errno!=ENOENT) { Test.AddError(TEXT("dynamic file exact-owned cleanup stat failed"));Ok=false; }
  }
  if (::rmdir(Name)!=0) { Test.AddError(TEXT("dynamic file exact-owned empty-directory cleanup failed"));Ok=false; }
  if (Ok) Test.AddInfo(FString::Printf(TEXT("DynamicFile owned-directory cleanup complete: %s"),UTF8_TO_TCHAR(Name)));
  return Ok;
 }
};
class FTimingSink final : public IVaRestTraceSink
{
public:
 FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings S;S.Flags=EVaRestTraceDetail::Metadata;return S; }
 void OnAttached(const FVaRestTraceSnapshot&) override {}
 void OnEvent(const FVaRestTraceEvent&) override {}
};
struct FState
{
 FAutomationTestBase* Test=nullptr;
 FOwnedDirectory* Directory=nullptr;
 FString Mode,Temporary,Saved;
 TWeakPtr<FVaRestExchange,ESPMode::ThreadSafe> Exchange;
 TWeakPtr<FDynamicFileCleanupData,ESPMode::ThreadSafe> Cleanup;
 TSharedPtr<FDynamicFileCleanupObservation,ESPMode::ThreadSafe> Observation;
 uint64 Request=0;
 std::atomic<int32> Success{0},Errors{0},Complete{0};
 std::atomic<bool> CallbacksOnGT{true},CopyOnWorker{false};
 int32 StopCalls=0;
 bool StopReturned=false,SameLoadedModule=false,Failed=false,Closing=false;
 void ObserveAndMaybeStop(bool FromCopy)
 {
  if (!IsInGameThread()) { if (FromCopy) CopyOnWorker.store(true);return; }
  if (Closing) return;
  const auto Ex=Exchange.Pin();if (!Ex || !Request || Ex->GetRequestId()!=Request) return;
  const auto& E=Internals(*Ex);
  // COPY is the earlier MakeFinish window; CLOCK is the later GTK decode-preparation window.
  const bool Window=FromCopy ? E.Phase==EVaRestExchangePhase::InChain && !E.bCompletionQueued :
   E.Phase==EVaRestExchangePhase::Decoding && E.bLaterFramePending;
  if (!Window || E.DynamicFileSerial==0) return;
  {
   const auto Data=E.DynamicFileData.Pin();
   if (!Data || Data->Request!=Request || Data->Serial!=E.DynamicFileSerial || Data->Path!=E.TempFile ||
       !IFileManager::Get().FileExists(*Data->Path)) return;
   if (!Directory->Observe(Data->Path)) { Test->AddError(TEXT("observe exact owned response-temp identity"));Failed=true;return; }
   Temporary=Data->Path;Cleanup=Data;Observation=Data->FinalObservation.State;
  } // No strong cleanup-data/token observation survives the Stop call.
  if (StopCalls || (Mode!=TEXT("ClockStop") && Mode!=TEXT("CopyStop")) ||
      (FromCopy ? Mode!=TEXT("CopyStop") : Mode!=TEXT("ClockStop"))) return;
  ++StopCalls;
  auto* Module=FModuleManager::GetModulePtr<IModuleInterface>(TEXT("VaRestHttp"));
  if (!Module) { Test->AddError(TEXT("loaded actual HTTP module missing at targeted Stop"));Failed=true;return; }
  Module->ShutdownModule(); // dedicated opt-in process ONLY; does not unload executable code
  SameLoadedModule=FModuleManager::GetModulePtr<IModuleInterface>(TEXT("VaRestHttp"))==Module;
  StopReturned=true;
 }
};
class FClock final : public IVaRestClock
{
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock;
 TSharedRef<FState,ESPMode::ThreadSafe> State;
public:
 FClock(TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> C,TSharedRef<FState,ESPMode::ThreadSafe> S):Clock(MoveTemp(C)),State(MoveTemp(S)) {}
 ~FClock() override { check(IsInGameThread()); }
 double Now() const override { if (IsInGameThread()) State->ObserveAndMaybeStop(false);return Clock->Now(); }
 FVaRestTimerHandle SetTimer(double Delay,TUniqueFunction<void()>&& Callback) override { check(IsInGameThread());return Clock->SetTimer(Delay,MoveTemp(Callback)); }
 void ClearTimer(FVaRestTimerHandle& Handle) override { check(IsInGameThread());Clock->ClearTimer(Handle); }
};
struct FStopOnCopy
{
 TSharedRef<FState,ESPMode::ThreadSafe> State;
 explicit FStopOnCopy(TSharedRef<FState,ESPMode::ThreadSafe> S):State(MoveTemp(S)) {}
 FStopOnCopy(const FStopOnCopy& Other):State(Other.State) { State->ObserveAndMaybeStop(true); }
 FStopOnCopy(FStopOnCopy&&)=default;
 bool operator()(const FVaRestJson&,const FString&,TArray<uint8>&) const { return false; }
};
class FSetupStage final : public IVaRestStage
{
 TSharedRef<FState,ESPMode::ThreadSafe> S;
public:
 explicit FSetupStage(TSharedRef<FState,ESPMode::ThreadSafe> InState):S(MoveTemp(InState)) {}
 FName GetStageName() const override { return TEXT("PrepublishFileSetup"); }
 void Process(const FVaRestExchangeRef& Exchange,const FVaRestStageNext& Next,FVaRestOutcomeCallback&& Done) override
 {
  S->Exchange=Exchange;
  // Existing private protocol option storage, real public request/actual MakeFinish copying; no synthetic decoder/drop.
  if (S->Mode==TEXT("CopyStop")) Internals(*Exchange).DecodeOptions.BytesResolver=FStopOnCopy(S);
  Next.Proceed(Exchange,MoveTemp(Done));
 }
};
class FRun final : public IAutomationLatentCommand
{
 FAutomationTestBase& Test;
 const FString Mode;
 FOwnedDirectory Directory;
 TSharedRef<FState,ESPMode::ThreadSafe> State;
 TUniquePtr<Testing::FTestRig> Rig;
 TSharedPtr<FClock,ESPMode::ThreadSafe> InjectedClock;
 TWeakPtr<FClock,ESPMode::ThreadSafe> WeakClock;
 TSharedPtr<FTimingSink,ESPMode::ThreadSafe> Sink;
 double Deadline=0;
 uint64 SettledFrame=0;
 int32 Phase=0;
 bool Finished=false,Closing=false,ClosingSuccess=true,ClosingReleased=false,TimeoutReported=false;
 const TArray<uint8> Old={0x11,0x22,0x33};
 const TArray<uint8> Body={7,8,9};
 bool TraceMatches() const
 {
  const auto Sinks=FVaRestTrace::GetSinks();
  return Sink && Sinks && Sinks->Num()==1 && &(*Sinks)[0].Get()==Sink.Get() && FVaRestTrace::Detail()==EVaRestTraceDetail::Metadata;
 }
 bool PollClose()
 {
  // Retain FRun/Directory/Test/State while any owned physical/native/GTK obligation remains.
  if (Rig) Rig->Clock->Advance(0.0);
  const bool PhysicalQuiet=!Rig || Rig->Fake->NumCalls()==0 ||
   (Rig->Fake->NumCalls()==1 && Rig->Fake->GetCall(0).bCompleted && Rig->Fake->GetCall(0).Request.RequestId==State->Request);
  const bool NativeQuiet=!Rig || (State->Request==0 && Rig->Fake->NumCalls()==0 && !State->Observation) ||
   (State->Observation && State->Observation->Ended.load(std::memory_order_acquire) &&
    State->Observation->DataDestroyed.load(std::memory_order_acquire) && !State->Cleanup.IsValid());
  if (!PhysicalQuiet || !NativeQuiet) return false;
  if (!ClosingReleased)
  {
   if (Rig)
   {
    if (Rig->Fake->NumCalls()==1 && Rig->Fake->GetCall(0).Request.Clock)
     ClosingSuccess=Test.TestTrue(TEXT("release ONLY own completed fake clock view"),Rig->Fake->ReleaseCompletedClock(0)) && ClosingSuccess;
    Rig->Client=FVaRestClient();
   }
   InjectedClock.Reset();ClosingReleased=true;
  }
  if (WeakClock.IsValid()) return false; // actual GTK retirement; no deadline extension or global drain/reset
  if (Sink) { FVaRestTrace::RemoveSink(Sink.ToSharedRef());Sink.Reset(); }
  ClosingSuccess=Test.TestTrue(TEXT("remove only exact own files and empty directory after native quiescence"),Directory.Finish()) && ClosingSuccess;
  Rig.Reset();Finished=true;State->Test=nullptr;State->Directory=nullptr;
  Test.AddInfo(FString::Printf(TEXT("DynamicFile mode=%s stop=%d returned=%d mapped=%d success=%d error=%d complete=%d nativeQuiet=%d result=%d"),
   *Mode,State->StopCalls,State->StopReturned,State->SameLoadedModule,State->Success.load(),State->Errors.load(),State->Complete.load(),NativeQuiet,ClosingSuccess));
  return true;
 }
 bool End(bool Success)
 {
  Closing=true;ClosingSuccess=ClosingSuccess && Success;State->Closing=true;
  return PollClose();
 }
public:
 FRun(FAutomationTestBase& T,FString M):Test(T),Mode(MoveTemp(M)),Directory(T),State(MakeShared<FState,ESPMode::ThreadSafe>())
 { State->Test=&Test;State->Directory=&Directory;State->Mode=Mode;Deadline=FPlatformTime::Seconds()+45.0; }
 ~FRun() override
 {
  check(IsInGameThread());
  if (!Finished)
  {
   State->Closing=true;State->Test=nullptr;State->Directory=nullptr;
   if (Sink) { FVaRestTrace::RemoveSink(Sink.ToSharedRef());Sink.Reset(); }
   // Forced latent teardown is unqualified; no active worker's file is removed.
   Rig.Reset();InjectedClock.Reset();
  }
 }
 bool Update() override
 {
  check(IsInGameThread());
  if (Closing)
  {
   if (!TimeoutReported && FPlatformTime::Seconds()>=Deadline)
   { Test.AddError(TEXT("owned SaveTo closing obligations did not retire before the original deadline"));TimeoutReported=true;ClosingSuccess=false; }
   return PollClose(); // retain owned state until genuine retirement; outer supervisor bounds an unqualified hang
  }
  if (FPlatformTime::Seconds()>=Deadline || State->Failed)
  { Test.AddError(TEXT("prepublish SaveTo ownership did not settle at actual engine frames"));TimeoutReported=true;return End(false); }
  if (Phase==0)
  {
   if (!Test.TestTrue(TEXT("fresh atomically owned temp directory"),Directory.Ready())) return End(false);
   const auto ExistingSinks=FVaRestTrace::GetSinks();
   if ((ExistingSinks && ExistingSinks->Num()!=0) || FVaRestTrace::Detail()!=EVaRestTraceDetail::None)
   { Test.AddError(FString::Printf(TEXT("dedicated SaveTo fixture requires empty trace registry/None mask: actual sinks=%d mask=%u; no global reset"),
      ExistingSinks ? ExistingSinks->Num() : 0,static_cast<uint32>(FVaRestTrace::Detail())));return End(false); }
   if (!Test.TestTrue(TEXT("prepare old destination sentinel"),FFileHelper::SaveArrayToFile(Old,*Directory.Destination())) ||
       !Test.TestTrue(TEXT("record actual owned destination identity"),Directory.Observe(Directory.Destination()))) return End(false);
   Rig=MakeUnique<Testing::FTestRig>(TEXTVIEW("https://prepublish-file.test"));Rig->Fake->bCompleteOnWorker=false;
   InjectedClock=MakeShared<FClock,ESPMode::ThreadSafe>(Rig->Clock,State);WeakClock=InjectedClock;
   auto Config=FClientAccess::InitialConfig(TEXTVIEW("https://prepublish-file.test"));
   Config.Transport=Rig->Fake;Config.Clock=InjectedClock;Config.Connectivity=Rig->Connectivity;Config.LogLevel=EVaRestLogLevel::None;
   Rig->Client=FClientAccess::Create(MoveTemp(Config));
   Rig->Client.Use(MakeShared<FSetupStage,ESPMode::ThreadSafe>(State),EVaRestStageSlot::Outer);
   Sink=MakeShared<FTimingSink,ESPMode::ThreadSafe>();FVaRestTrace::AddSink(Sink.ToSharedRef());
   if (!Test.TestTrue(TEXT("exact own Metadata sink selects actual decode timing"),TraceMatches())) return End(false);
   Rig->Fake->Enqueue(FVaRestFakeResponse::Bytes(200,TArray<uint8>(Body)));
   FVaRestDynamicResultConfig Dynamic;Dynamic.ResponseType.Kind=EVaRestTypeKind::None;
   const auto Handle=Rig->Client.RequestDynamic(TEXTVIEW("GET"),TEXTVIEW("/owned"),MoveTemp(Dynamic)).SaveTo(Directory.Destination())
    .OnSuccess([S=State](const FVaRestDynamicResult&,const FVaRestResponse& Response) {
     S->Success.fetch_add(1);if (!IsInGameThread()) { S->CallbacksOnGT.store(false);return; }S->Saved=Response.SavedFile;
    }).OnError([S=State](const FVaRestError&) { S->Errors.fetch_add(1);if (!IsInGameThread()) S->CallbacksOnGT.store(false); })
    .OnComplete([S=State](const TVaRestResult<FVaRestDynamicResult>&) { S->Complete.fetch_add(1);if (!IsInGameThread()) S->CallbacksOnGT.store(false); }).Send();
   State->Request=Handle.GetId();
   if (const auto Exchange=State->Exchange.Pin())
   {
    const auto Data=Internals(*Exchange).DynamicFileData.Pin();
    if (Data && Data->Request==State->Request)
    { State->Cleanup=Data;State->Observation=Data->FinalObservation.State; }
   } // Observe native lifetime before a failure can enter the closing phase; retain no strong data/token.
   if (!Test.TestTrue(TEXT("real public ordinary dynamic SaveTo admitted"),State->Request!=0)) return End(false);
   Phase=1;return false;
  }
  if (!TraceMatches()) { Test.AddError(TEXT("owned timing sink membership/mask changed"));return End(false); }
  Rig->Clock->Advance(0.0);
  if (!State->Observation || !State->Observation->Ended.load(std::memory_order_acquire) ||
      !State->Observation->DataDestroyed.load(std::memory_order_acquire) || State->Cleanup.IsValid()) return false;
  if (Mode==TEXT("Healthy") && State->Complete.load()!=1) return false;
  if (!SettledFrame) { SettledFrame=GFrameCounter;return false; }
  if (GFrameCounter<=SettledFrame+1) return false; // actual later engine frames, never a frame-counter rewrite
  bool Ok=Test.TestTrue(TEXT("actual cleanup job ran on a native thread"),State->Observation->Ran.load(std::memory_order_acquire) &&
   !State->Observation->OnGT.load(std::memory_order_acquire) && State->Observation->Thread.load()!=0);
  Ok&=Test.TestTrue(TEXT("real physical fake/file call completed"),Rig->Fake->NumCalls()==1 && Rig->Fake->GetCall(0).bCompleted);
  Ok&=Test.TestFalse(TEXT("the exact response temp has been removed by its preselected native owner"),State->Temporary.IsEmpty() || IFileManager::Get().FileExists(*State->Temporary));
  Ok&=Test.TestTrue(TEXT("all callbacks obey GTK affinity"),State->CallbacksOnGT.load());
  Ok&=Test.TestFalse(TEXT("original conversion callback was not copied onto a native worker"),State->CopyOnWorker.load());
  TArray<uint8> Actual;
  Ok&=Test.TestTrue(TEXT("read only owned actual destination"),FFileHelper::LoadFileToArray(Actual,*Directory.Destination()));
  if (Mode==TEXT("Healthy"))
  {
   Ok&=Test.TestEqual(TEXT("healthy callback completes once"),State->Complete.load(),1);
   Ok&=Test.TestEqual(TEXT("healthy success once"),State->Success.load(),1);
   Ok&=Test.TestEqual(TEXT("healthy error zero"),State->Errors.load(),0);
   Ok&=Test.TestEqual(TEXT("healthy SavedFile names exact owned destination"),State->Saved,Directory.Destination());
   Ok&=Test.TestTrue(TEXT("healthy actual destination bytes"),Actual==Body);
   Ok&=Test.TestEqual(TEXT("healthy mode performs no Stop"),State->StopCalls,0);
   Ok&=Test.TestTrue(TEXT("adopt only actual healthy writer replacement for fixture cleanup"),Directory.Observe(Directory.Destination(),true));
  }
  else
  {
   Ok&=Test.TestEqual(TEXT("targeted actual Stop exactly once"),State->StopCalls,1);
   Ok&=Test.TestTrue(TEXT("actual ShutdownModule returned while its registered code stays loaded"),State->StopReturned && State->SameLoadedModule);
   Ok&=Test.TestEqual(TEXT("no callback after Stop"),State->Complete.load()+State->Success.load()+State->Errors.load(),0);
   Ok&=Test.TestTrue(TEXT("Stop preserves old destination sentinel bytes"),Actual==Old);
   Ok&=Test.TestTrue(TEXT("Stop preserves the original destination identity"),Directory.Observe(Directory.Destination()));
   Ok&=Test.TestTrue(TEXT("Stop never reports a SavedFile success"),State->Saved.IsEmpty());
  }
  return End(Ok);
 }
};
bool Selected(FString& Mode)
{
 return FCommandLine::IsInitialized() && FParse::Param(FCommandLine::Get(),TEXT("VaRestM5PrepublishSaveToProof")) &&
  FParse::Value(FCommandLine::Get(),TEXT("VaRestM5FileCase="),Mode) &&
  (Mode==TEXT("Healthy") || Mode==TEXT("ClockStop") || Mode==TEXT("CopyStop"));
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestDynamicSaveToPrepublishTest,"VaRest.Http.Dynamic.SaveToPrepublishOwnership",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestDynamicSaveToPrepublishTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
 FString Mode;if (VaRest::Http::PrepublishFileTest::Selected(Mode)) { Names.Add(Mode);Commands.Add(Mode); }
}
bool FVaRestDynamicSaveToPrepublishTest::RunTest(const FString& Parameters)
{
 FString Mode;
 if (!VaRest::Http::PrepublishFileTest::Selected(Mode) || Mode!=Parameters || !FPlatformProcess::SupportsMultithreading() ||
      LowLevelTasks::FScheduler::Get().GetNumWorkers()==0)
 { AddError(TEXT("requires one exact dedicated fresh-process SaveTo ownership case and genuine native workers"));return false; }
 ADD_LATENT_AUTOMATION_COMMAND(VaRest::Http::PrepublishFileTest::FRun(*this,Mode));return true;
}
#endif
