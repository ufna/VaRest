// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLegacyHttpTestTypes.h"

void UVaRestLegacyHttpTestProbe::LatentDone(int32 Linkage)
{
 bOnGT&=IsInGameThread(); ++LatentContinuations; LastLatentLinkage=Linkage;
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestConnectivity.h"
#include "VaRestGameInstanceSubsystem.h"
#include "VaRestHub.h"
#include "VaRestJsonObject.h"
#include "VaRestTrace.h"

namespace VaRestLegacyMissingBranches
{
enum class ECase : uint8 { ApplyURL, TransportConnection, HubStopPending };
struct FNativeCounts
{
 int32 Complete=0,Fail=0;
 bool OnGT=true,Identity=true;
};
class FObserver final : public IVaRestTraceSink
{
public:
 uint64 Scope=0,RequestId=0,ClientId=0,CompletedFrame=0;
 int32 Started=0,Completed=0;
 bool OnGT=true;
 EVaRestTraceOutcome Outcome=EVaRestTraceOutcome::Success;
 EVaRestErrorKind Error=EVaRestErrorKind::None;
 virtual FVaRestTraceDetailSettings GetDetail() const override
 { FVaRestTraceDetailSettings D; D.Flags=EVaRestTraceDetail::Metadata; return D; }
 virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
 virtual void OnEvent(const FVaRestTraceEvent& Event) override
 {
  OnGT&=IsInGameThread();
  if (Event.Kind==EVaRestTraceKind::Started)
  {
   const auto& S=Event.Payload.Get<FVaRestTraceStarted>();
   if (S.ScopeId==Scope) { ++Started; RequestId=S.RequestId; ClientId=S.ClientId; }
  }
  else if (Event.Kind==EVaRestTraceKind::Completed)
  {
   const auto& C=Event.Payload.Get<FVaRestTraceCompleted>();
   if (RequestId!=0 && C.RequestId==RequestId)
   { ++Completed; CompletedFrame=GFrameCounter; Outcome=C.Outcome; Error=C.Error.Kind; }
  }
 }
};

class FRun final : public IAutomationLatentCommand
{
public:
 FRun(FAutomationTestBase& InTest,ECase InCase):Test(InTest),Case(InCase) {}
 virtual ~FRun() override
 {
  // Update never returns while owned close is unresolved. A runner interruption
  // is not qualification: detach its owned latent reference without forcing GC
  // or World teardown before the terminal barrier.
  BeginClose();
  TickLatent();
  if (Quiescent()) Cleanup();
 }
 virtual bool Update() override
 {
  check(IsInGameThread());
  if (!Begun)
  {
   Begun=true;
   if (!Begin()) BeginClose();
   return false;
  }
  TickLatent();
  if (Closing)
  {
   if (Quiescent()) { Cleanup(); return true; }
   if (FPlatformTime::Seconds()>CloseDeadline)
   {
    if (!CleanupDeadlineReported)
    {
     CleanupDeadlineReported=true;
     Test.AddError(FString::Printf(TEXT("legacy owned cleanup deadline: started=%d completed=%d latent=%d; retaining owners until terminal or outer supervisor failure"),
      Observer ? Observer->Started : 0,Observer ? Observer->Completed : 0,LatentActions()));
    }
    return false;
   }
   return false;
  }
  if (FPlatformTime::Seconds()>Deadline)
  {
   Test.AddError(FString::Printf(TEXT("legacy branch deadline: started=%d completed=%d dynamic=%d/%d latent=%d"),
    Observer->Started,Observer->Completed,Probe->Completes,Probe->Failures,Probe->LatentContinuations));
   BeginClose(); return false;
  }
  if (Observer->Completed!=1 || !Quiescent()) return false;
  Verify();
  BeginClose(); return false;
 }
private:
 bool Begin()
 {
  if (!Test.TestNotNull(TEXT("engine for private standalone GI"),GEngine)) return false;
  DefaultConnectivity=FVaRestConnectivity::GetDefault(); DefaultOnline=DefaultConnectivity->IsOnline();
  EditorHub=UVaRestHub::GetForEditor();
  if (auto* Existing=EditorHub.Get()) { EditorConnectivity=Existing->GetConnectivity(); EditorOnline=Existing->IsOnline(); }
  const FString Name=TEXT("M6LegacyBranches_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Package.Reset(NewObject<UPackage>(nullptr,*(TEXT("/Temp/")+Name),RF_Transient));
  Instance.Reset(NewObject<UGameInstance>(GEngine)); Instance->InitializeStandalone(*Name,Package.Get());
  World.Reset(Instance->GetWorld());
  auto* Host=Instance->GetSubsystem<UVaRestGameInstanceSubsystem>();
  if (!Test.TestNotNull(TEXT("private actual GI World"),World.Get())
   || !Test.TestNotNull(TEXT("private actual GI subsystem"),Host)
   || !Test.TestEqual(TEXT("private World belongs to own GI"),World->GetGameInstance(),Instance.Get())) return false;
  auto* Candidate=Host->GetHub();
  if (!Test.TestNotNull(TEXT("private actual GI Hub"),Candidate)
   || !Test.TestTrue(TEXT("own Hub differs from engine fallback"),Candidate!=EditorHub.Get())
   || !Test.TestEqual(TEXT("actual World selects its own GI subsystem Hub"),UVaRestHub::Get(World.Get()),Candidate)) return false;
  Hub.Reset(Candidate); // Store teardown authority only after actual ownership is proved.
  FString Error;
  if (!Test.TestTrue(TEXT("private storage profile"),Hub->SetStorageProfile(Name,Error))) return false;
  OwnConnectivity=Hub->GetConnectivity();
  if (!Test.TestTrue(TEXT("own reachability is an independent online scope"),OwnConnectivity && OwnConnectivity.Get()!=DefaultConnectivity.Get() && OwnConnectivity->IsOnline())) return false;
  OwnConnectivity->SetProbeInterval(120.0); // Only this new scope; no automatic retry can outlive the finite case.
  Mock=IConsoleManager::Get().FindConsoleVariable(TEXT("http.UrlPatternsToMockResponse"));
  if (!Test.TestNotNull(TEXT("native engine HTTP mock provider"),Mock)) return false;
  OriginalMock=Mock->GetString(); OriginalFlags=uint32(Mock->GetFlags()); Tag=FName(*Name);
  Root=TEXT("https://m6-legacy-")+Name.Right(32)+TEXT(".invalid");
  FString Patterns=Root+TEXT("/connection->0 ")+Root+TEXT("/success->200++{\"ok\":7}++Content-Type:application/json ")+
   Root+TEXT("/stop->200++{\"ok\":7}++Content-Type:application/json");
  if (!OriginalMock.IsEmpty()) Patterns+=TEXT(" ")+OriginalMock;
  Mock->Set(*Patterns,ECVF_SetByConsole,Tag); MockSet=true;
  if (!Test.TestEqual(TEXT("only admitted native mock patterns installed"),Mock->GetString(),Patterns)) return false;
  Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
  Observer=MakeShared<FObserver,ESPMode::ThreadSafe>(); Observer->Scope=Hub->GetScopeId();
  if (!Test.TestTrue(TEXT("owned GI has a real nonzero scope"),Observer->Scope!=0)) return false;
  FVaRestTrace::AddSink(Observer.ToSharedRef()); Attached=true;
  Probe.Reset(NewObject<UVaRestLegacyHttpTestProbe>());
  Request.Reset(NewObject<UVaRestRequestJSON>()); Weak=Request.Get(); Counts=MakeShared<FNativeCounts,ESPMode::ThreadSafe>();
  Request->OnRequestComplete.AddDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::Completed);
  Request->OnRequestFail.AddDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::Failed);
  Request->OnStaticRequestComplete.AddLambda([C=Counts,Expected=Weak](UVaRestRequestJSON* Actual)
  { C->OnGT&=IsInGameThread(); C->Identity&=Actual==Expected.Get(); ++C->Complete; });
  Request->OnStaticRequestFail.AddLambda([C=Counts,Expected=Weak](UVaRestRequestJSON* Actual)
  { C->OnGT&=IsInGameThread(); C->Identity&=Actual==Expected.Get(); ++C->Fail; });
  float TotalTimeout=10.0f;
  if (Case==ECase::TransportConnection)
  {
   // Native mock code captures this exact int32 delay, then sets ConnectionError.
   // Never change a global engine timeout or shorten this into total-timeout.
   const float Configured=FHttpModule::Get().GetHttpConnectionTimeout();
   if (!Test.TestTrue(TEXT("bounded native connection mock delay0..30s"),FMath::IsFinite(Configured) && Configured>=0 && Configured<=30)) return false;
   TotalTimeout=float(int32(Configured)+10);
   Deadline=FPlatformTime::Seconds()+double(int32(Configured))+20.0;
  }
  else Deadline=FPlatformTime::Seconds()+20.0;
  Engine=Request->GetHttpRequest(); Engine->SetTimeout(TotalTimeout);
  FLatentActionInfo Info; Info.CallbackTarget=Probe.Get(); Info.ExecutionFunction=GET_FUNCTION_NAME_CHECKED(UVaRestLegacyHttpTestProbe,LatentDone);
  Info.Linkage=Linkage; Info.UUID=UUID;
  const FString Path=Case==ECase::TransportConnection ? TEXT("/connection") : Case==ECase::HubStopPending ? TEXT("/stop") : TEXT("/success");
  Request->ApplyURL(Root+Path,LatentResult,World.Get(),Info);
  if (!Test.TestEqual(TEXT("public ApplyURL is pending before deferred delivery"),Request->GetStatus(),EVaRestRequestStatus::Processing)
   || !Test.TestEqual(TEXT("no inline latent continuation"),Probe->LatentContinuations,0)
   || !Test.TestNull(TEXT("result reference starts empty"),LatentResult)
   || !Test.TestTrue(TEXT("actual manager owns the actual legacy latent action"),World->GetLatentActionManager().FindExistingAction<FVaRestLatentAction<UVaRestJsonObject*>>(Probe.Get(),UUID)!=nullptr)
   || !Test.TestTrue(TEXT("actual managed legacy request uses own scope/client"),Observer->Started==1 && Observer->RequestId!=0 && Observer->ClientId!=0)) return false;
  if (Case==ECase::HubStopPending)
  {
   Request.Reset(); // No external strong request crosses stop or the later GC.
   if (!Test.TestTrue(TEXT("unrooted legacy request remains pending under own Hub"),Weak.IsValid() && !Weak->IsRooted() && Weak->GetStatus()==EVaRestRequestStatus::Processing)) return false;
   Hub->DeinitializeOnGT(); HubStopped=true; // Actual registered in-flight cancellation path.
   if (!Test.TestTrue(TEXT("Hub stop silently cancels the actual legacy request"),Weak.IsValid() && Weak->GetStatus()==EVaRestRequestStatus::Failed && Weak->GetResponseCode()==-1 && Weak->GetResponseContent().IsEmpty())) return false;
  }
  Test.AddInfo(FString::Printf(TEXT("LegacyMissingBranch start case=%d Scope=%llu Client=%llu Request=%llu"),int32(Case),
   Observer->Scope,Observer->ClientId,Observer->RequestId));
  return true;
 }
 void TickLatent()
 {
  if (!World || !Probe) return;
  auto& Manager=World->GetLatentActionManager(); Manager.BeginFrame(); Manager.ProcessLatentActions(Probe.Get(),0.0f);
 }
 int32 LatentActions() const
 { return World && Probe ? World->GetLatentActionManager().GetNumActionsForObject(Probe.Get()) : 0; }
 bool Quiescent() const
 {
  const bool Terminal=!Observer || (Observer->Started==Observer->Completed &&
   (Observer->Started==0 || GFrameCounter>Observer->CompletedFrame+1));
  const bool NotProcessing=!Weak.IsValid() || Weak->GetStatus()!=EVaRestRequestStatus::Processing;
  const bool NativeStopped=!Engine || Engine->GetStatus()!=EHttpRequestStatus::Processing;
  return Terminal && NotProcessing && NativeStopped && LatentActions()==0;
 }
 void Verify()
 {
  Test.TestEqual(TEXT("one actual owned logical request"),Observer->Started,1);
  Test.TestEqual(TEXT("one actual owned logical terminal"),Observer->Completed,1);
  Test.TestTrue(TEXT("native trace/delegates and actual continuation stay on GT"),Observer->OnGT && Counts->OnGT && Probe->bOnGT);
  Test.TestTrue(TEXT("native callbacks keep exact request identity"),Counts->Identity);
  if (Case==ECase::ApplyURL)
  {
   Test.TestTrue(TEXT("real ApplyURL completes once through both public delegate families"),Probe->Completes==1 && Probe->Failures==0 && Counts->Complete==1 && Counts->Fail==0);
   Test.TestEqual(TEXT("actual LatentActionManager invokes once"),Probe->LatentContinuations,1);
   Test.TestEqual(TEXT("actual linkage is passed to the UFUNCTION"),Probe->LastLatentLinkage,Linkage);
   Test.TestEqual(TEXT("actual latent output is request's real response object"),LatentResult,Request->GetResponseObject());
   Test.TestTrue(TEXT("actual latent response status/value"),Request->GetStatus()==EVaRestRequestStatus::Succeeded && Request->GetResponseCode()==200 && LatentResult && LatentResult->GetIntegerField(TEXT("ok"))==7);
   Test.TestEqual(TEXT("actual latent core success"),Observer->Outcome,EVaRestTraceOutcome::Success);
  }
  else
  {
   Test.TestTrue(TEXT("aborted legacy latent output never continues or receives a result"),Probe->LatentContinuations==0 && LatentResult==nullptr);
   if (Case==ECase::TransportConnection)
   {
    Test.TestTrue(TEXT("genuine native ConnectionError reaches dynamic/static failure once"),Probe->Completes==0 && Probe->Failures==1 && Counts->Complete==0 && Counts->Fail==1);
    Test.TestEqual(TEXT("actual engine failure reason"),Engine->GetFailureReason(),EHttpFailureReason::ConnectionError);
    Test.TestEqual(TEXT("legacy connection classification"),Request->GetStatus(),EVaRestRequestStatus::Failed_ConnectionError);
    Test.TestTrue(TEXT("no invented HTTP response on transport failure"),Request->GetResponseCode()==-1 && Request->GetResponseContent().IsEmpty() && !Request->bIsValidJsonResponse);
    Test.TestEqual(TEXT("actual core error classification"),Observer->Error,EVaRestErrorKind::Connection);
    Test.TestFalse(TEXT("only own Hub observation becomes offline"),OwnConnectivity->IsOnline());
   }
   else
   {
    Test.TestTrue(TEXT("actual Hub stop remains silent through all public delegates"),Probe->Completes==0 && Probe->Failures==0 && Counts->Complete==0 && Counts->Fail==0);
    Test.TestEqual(TEXT("actual stopped owned core terminal is cancellation"),Observer->Outcome,EVaRestTraceOutcome::Canceled);
    Test.TestTrue(TEXT("actual stopped Hub rejects client admission"),HubStopped && Hub->GetDefaultClient()==nullptr);
   }
  }
  Test.TestEqual(TEXT("process default reachability never changed"),DefaultConnectivity->IsOnline(),DefaultOnline);
  Test.TestEqual(TEXT("engine fallback Hub identity unchanged"),UVaRestHub::GetForEditor(),EditorHub.Get());
  if (EditorConnectivity) Test.TestEqual(TEXT("engine fallback reachability never changed"),EditorConnectivity->IsOnline(),EditorOnline);
  Request.Reset(); LatentResult=nullptr;
  // The actual manager no longer holds Result&, every owned logical request is
  // terminal, and two ordinary frames passed. Keep the SAME own Hub/GI alive.
  CollectGarbage(RF_NoFlags);
  Test.TestFalse(TEXT("owned legacy request released while same Hub/GI/probe remain alive"),Weak.IsValid());
  Test.AddInfo(FString::Printf(TEXT("LegacyMissingBranch terminal case=%d complete=%d fail=%d latent=%d GT=1 ownedRequestReleased=%d"),
   int32(Case),Probe->Completes,Probe->Failures,Probe->LatentContinuations,!Weak.IsValid()));
 }
 void BeginClose()
 {
  if (Closing) return; Closing=true; CloseDeadline=FPlatformTime::Seconds()+10.0;
  if (auto* Pending=Weak.Get()) Pending->Cancel();
  if (Hub) Hub->DeinitializeOnGT();
  if (World && Probe) World->GetLatentActionManager().RemoveActionsForObject(Probe.Get());
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  TickLatent();
  Request.Reset(); LatentResult=nullptr;
  if (Hub) Hub->DeinitializeOnGT();
  Engine.Reset(); Hub.Reset(); OwnConnectivity.Reset();
  if (Instance) Instance->Shutdown();
  if (World)
  {
   World->DestroyWorld(false);
   if (GEngine) GEngine->DestroyWorldContext(World.Get());
   if (World->IsRooted()) World->RemoveFromRoot();
   World->SetGameInstance(nullptr);
  }
  World.Reset(); Instance.Reset(); Package.Reset(); Probe.Reset();
  if (Attached) { FVaRestTrace::RemoveSink(Observer.ToSharedRef()); Attached=false; }
  Observer.Reset(); Isolation.Reset(); Counts.Reset();
  if (MockSet)
  {
   Mock->Unset(ECVF_SetByConsole,Tag);
   Test.TestEqual(TEXT("native mock exact value restored"),Mock->GetString(),OriginalMock);
   Test.TestEqual(TEXT("native mock priority/flags restored"),uint32(Mock->GetFlags()),OriginalFlags);
  }
  EditorConnectivity.Reset(); DefaultConnectivity.Reset();
 }
 FAutomationTestBase& Test;
 ECase Case;
 static constexpr int32 Linkage=417,UUID=17021;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UGameInstance> Instance;
 TStrongObjectPtr<UWorld> World;
 TStrongObjectPtr<UVaRestHub> Hub;
 TStrongObjectPtr<UVaRestLegacyHttpTestProbe> Probe;
 TStrongObjectPtr<UVaRestRequestJSON> Request;
 TWeakObjectPtr<UVaRestRequestJSON> Weak;
 TWeakObjectPtr<UVaRestHub> EditorHub;
 UVaRestJsonObject* LatentResult=nullptr; // referenced only while this command and own actual manager survive.
 FHttpRequestPtr Engine;
 TSharedPtr<FNativeCounts,ESPMode::ThreadSafe> Counts;
 TSharedPtr<FObserver,ESPMode::ThreadSafe> Observer;
 TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> OwnConnectivity,DefaultConnectivity,EditorConnectivity;
 TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
 IConsoleVariable* Mock=nullptr;
 FString Root,OriginalMock;
 FName Tag;
 uint32 OriginalFlags=0;
 double Deadline=0,CloseDeadline=0;
 bool DefaultOnline=false,EditorOnline=false,Begun=false,Closing=false,Cleaned=false,Attached=false,MockSet=false,HubStopped=false,CleanupDeadlineReported=false;
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestLegacyHttpMissingBranchesTest,"VaRest.Compat.Legacy.MissingBranches",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestLegacyHttpMissingBranchesTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
 for (const TCHAR* Name:{TEXT("ApplyURL"),TEXT("TransportConnection"),TEXT("HubStopPending")})
 { Names.Add(Name); Commands.Add(Name); }
}
bool FVaRestLegacyHttpMissingBranchesTest::RunTest(const FString& Parameters)
{
 VaRestLegacyMissingBranches::ECase MissingCase;
 if (Parameters==TEXT("ApplyURL")) MissingCase=VaRestLegacyMissingBranches::ECase::ApplyURL;
 else if (Parameters==TEXT("TransportConnection")) MissingCase=VaRestLegacyMissingBranches::ECase::TransportConnection;
 else if (Parameters==TEXT("HubStopPending")) MissingCase=VaRestLegacyMissingBranches::ECase::HubStopPending;
 else { AddError(TEXT("Unknown owned legacy missing-branch case")); return false; }
 ADD_LATENT_AUTOMATION_COMMAND(VaRestLegacyMissingBranches::FRun(*this,MissingCase)); return true;
}
#endif
