// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestLegacyHttpTestTypes.h"
#include "VaRestSubsystem.h"
#include "Engine/Engine.h"

void UVaRestLegacyHttpTestProbe::Completed(UVaRestRequestJSON* Request)
{
 bOnGT&=IsInGameThread(); ++Completes;
 if (Completes==ResendAt) Request->ExecuteProcessRequest();
}
void UVaRestLegacyHttpTestProbe::Failed(UVaRestRequestJSON*) { bOnGT&=IsInGameThread(); ++Failures; }
void UVaRestLegacyHttpTestProbe::CallCompleted(UVaRestRequestJSON* Request)
{
 bOnGT&=IsInGameThread(); ++CallCompletions; CallRequests.Add(Request);
 if (!bFanout) return;
 bFanout=false;
 FVaRestCallDelegate Callback; Callback.BindDynamic(this,&UVaRestLegacyHttpTestProbe::CallCompleted);
 auto* Subsystem=GEngine->GetEngineSubsystem<UVaRestSubsystem>();
 // A duplicate notification during user code must already see the old entry
 // retired. This gives a deterministic assertion for the reentrancy contract.
 Subsystem->OnCallComplete(Request);
 for (int32 Index=0;Index<64;++Index)
  Subsystem->CallURL(Root+TEXT("/fan/")+FString::FromInt(Index),EVaRestRequestVerb::GET,EVaRestRequestContentType::json,nullptr,Callback);
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestJson.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestSettings.h"
#include "VaRestTrace.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"

namespace VaRestLegacyHttpFacadeTest
{
struct FCounts { int32 Complete=0,Fail=0; bool bOnGT=true; };
class FObserver final : public IVaRestTraceSink
{
public:
 FString Root;
 TSet<uint64> Started,Completed;
 TMap<uint64,FVaRestTraceExcerpt> RequestBodies;
 uint64 LastStarted=0;
 uint64 LastCompletedFrame=0;
 TMap<uint64,int32> CompletionCounts;
 TMap<uint64,EVaRestTraceOutcome> Outcomes;
 TFunction<void(const FVaRestTraceStarted&)> StartedCallout;
 virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings D; D.Flags=EVaRestTraceDetail::Metadata|EVaRestTraceDetail::Bodies; D.BodyCap=1024; D.BinaryCap=16; return D; }
 virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
 virtual void OnEvent(const FVaRestTraceEvent& Event) override
 {
  if (Event.Kind==EVaRestTraceKind::Started)
  {
   const auto& S=Event.Payload.Get<FVaRestTraceStarted>(); if (S.Url.Contains(Root)) { Started.Add(S.RequestId); LastStarted=S.RequestId; if (StartedCallout) StartedCallout(S); }
  }
  else if (Event.Kind==EVaRestTraceKind::Completed)
  {
   const auto& C=Event.Payload.Get<FVaRestTraceCompleted>();
   if (Started.Contains(C.RequestId)) { Completed.Add(C.RequestId); ++CompletionCounts.FindOrAdd(C.RequestId); Outcomes.Add(C.RequestId,C.Outcome); LastCompletedFrame=GFrameCounter; }
  }
  else if (Event.Kind==EVaRestTraceKind::Excerpt)
  {
   const auto& E=Event.Payload.Get<FVaRestTraceExcerptEvent>();
   if (E.Owner==EVaRestTraceExcerptOwner::Request && Started.Contains(E.RequestId)) RequestBodies.Add(E.RequestId,E.Excerpt);
  }
 }
};
class FRun final : public IAutomationLatentCommand
{
public:
 explicit FRun(FAutomationTestBase& InTest):Test(InTest) {}
 virtual ~FRun() override
 {
  StopCallURLCapture();
  if (CallOwner) CallOwner->Deinitialize();
  for (const auto& Call:OwnedCalls) if (auto* Pending=Call.Get()) Pending->Cancel();
  CallOwner.Reset();
  if (auto* Pending=Weak.Get()) Pending->Cancel();
  Request.Reset(); Probe.Reset();
  if (bAttached) FVaRestTrace::RemoveSink(Observer.ToSharedRef());
  Observer.Reset(); Isolation.Reset();
  if (MockVariable)
  {
   MockVariable->Unset(ECVF_SetByConsole,OverrideTag);
   Test.TestEqual(TEXT("engine mock configuration restored"),MockVariable->GetString(),OriginalMock);
   Test.TestEqual(TEXT("engine mock priority/flags restored"),uint32(MockVariable->GetFlags()),OriginalFlags);
  }
 }
 virtual bool Update() override
 {
  if (!Probe && !Initialize()) return true;
  if (Case==10) return true;
  if (!bStarted) return BeginCase();
  if (Case>=8) return UpdateCallURLRetirementCase();
  if (FPlatformTime::Seconds()-StartedAt>30) { Test.AddError(FString::Printf(TEXT("legacy HTTP owned terminal barrier timed out at case %d: starts=%d completed=%d"),Case,Observer->Started.Num(),Observer->Completed.Num())); return true; }
  if (Observer->Started.Num()!=ExpectedStarts || Observer->Completed.Num()!=ExpectedStarts || GFrameCounter<=Observer->LastCompletedFrame) return false;
  if ((Case==0 || Case==3 || Case==4) && !Observer->RequestBodies.Contains(CurrentRequestId)) return false;
  const int32 Want=Case==5 ? 0 : Case==2 ? 2 : Case==7 ? 0 : 1;
  if (Probe->Completes-BeforeComplete!=Want || (Case==7 && Probe->CallCompletions!=65)) return false;
  Test.TestEqual(TEXT("no failure event for HTTP responses or explicit silent cancel"),Probe->Failures,0);
  Test.TestEqual(TEXT("native complete count agrees with dynamic events"),Counts->Complete,Probe->Completes);
  Test.TestEqual(TEXT("native failure event count"),Counts->Fail,0);
  Test.TestTrue(TEXT("legacy callbacks stay on GT"),Probe->bOnGT && Counts->bOnGT);
  if (Case==0 || Case==4) Test.TestEqual(TEXT("actual core JSON request body"),Observer->RequestBodies.FindChecked(CurrentRequestId).ToText(),FString(TEXT("{\"n\":9}")));
  if (Case==0)
  {
   Test.TestEqual(TEXT("JSON HTTP status"),Request->GetResponseCode(),201);
   Test.TestEqual(TEXT("v1 completed status"),Request->GetStatus(),EVaRestRequestStatus::Succeeded);
   Test.TestTrue(TEXT("valid JSON result"),Request->bIsValidJsonResponse);
   Test.TestEqual(TEXT("parsed response field"),Request->GetResponseObject()->GetIntegerField(TEXT("ok")),7);
   Test.TestEqual(TEXT("response header"),Request->GetResponseHeader(TEXT("X-M6")),FString(TEXT("present")));
   // The actual engine-mock result must expose one mutable response graph through both public views.
   if (!Test.TestEqual(TEXT("object response value type"),Request->GetResponseValue()->GetType(),EVaJson::Object)) return true;
   TStrongObjectPtr<UVaRestJsonObject> ValueView(Request->GetResponseValue()->AsObject());
   if (!Test.TestNotNull(TEXT("owned response-value object view"),ValueView.Get())) return true;
   auto* ObjectView=Request->GetResponseObject();
   Test.TestTrue(TEXT("response object and value views share the actual engine root"),&ObjectView->GetRootObject().Get()==&ValueView->GetRootObject().Get());
   const TArray<uint8> OriginalResponse=Request->GetResponseContent();
   const TArray<uint8> ExpectedResponse={'{','"','o','k','"',':','7','}'};
   Test.TestTrue(TEXT("actual object response starts with the original wire bytes"),OriginalResponse==ExpectedResponse);
   ObjectView->SetIntegerField(TEXT("ok"),11);
   Test.TestEqual(TEXT("object-view field mutation reaches the response-value view"),ValueView->GetIntegerField(TEXT("ok")),11);
   ValueView->SetIntegerField(TEXT("ok"),13);
   Test.TestEqual(TEXT("response-value field mutation reaches the response-object view"),ObjectView->GetIntegerField(TEXT("ok")),13);
   TStrongObjectPtr<UVaRestJsonObject> InsertedChild(NewObject<UVaRestJsonObject>());
   InsertedChild->SetIntegerField(TEXT("n"),17); ObjectView->SetObjectField(TEXT("child"),InsertedChild.Get());
   if (!Test.TestTrue(TEXT("object-view nested field reaches the response-value view"),ValueView->HasField(TEXT("child")))) return true;
   TStrongObjectPtr<UVaRestJsonObject> ObjectChild(ObjectView->GetObjectField(TEXT("child")));
   TStrongObjectPtr<UVaRestJsonObject> ValueChild(ValueView->GetObjectField(TEXT("child")));
   if (!Test.TestNotNull(TEXT("owned nested response-object view"),ObjectChild.Get())
    || !Test.TestNotNull(TEXT("owned nested response-value view"),ValueChild.Get())) return true;
   ObjectChild->SetIntegerField(TEXT("n"),19);
   Test.TestEqual(TEXT("nested object mutation reaches the response-value graph"),ValueChild->GetIntegerField(TEXT("n")),19);
   ValueChild->SetIntegerField(TEXT("n"),23);
   Test.TestEqual(TEXT("nested value mutation reaches the response-object graph"),ObjectChild->GetIntegerField(TEXT("n")),23);
   auto Projected=FVaRestJson::Parse(FStringView(Request->GetResponseContentAsString(false)));
   if (!Test.TestTrue(TEXT("uncached mutated response projection is valid JSON"),Projected.HasValue())) return true;
   Test.TestEqual(TEXT("uncached content projection sees value-view field mutation"),Projected.GetValue().Get(TEXT("ok")).AsInt(),13);
   Test.TestEqual(TEXT("uncached content projection sees nested value-view mutation"),Projected.GetValue().Get(TEXT("child")).Get(TEXT("n")).AsInt(),23);
   Test.TestTrue(TEXT("view mutations never rewrite the retained raw response bytes"),Request->GetResponseContent()==OriginalResponse);
  }
  else if (Case==1)
  {
   Test.TestEqual(TEXT("HTTP error still means completed in v1"),Request->GetStatus(),EVaRestRequestStatus::Succeeded);
   Test.TestEqual(TEXT("error status retained"),Request->GetResponseCode(),422);
   Test.TestFalse(TEXT("non-JSON is not a valid JSON result"),Request->bIsValidJsonResponse);
   Test.TestEqual(TEXT("non-JSON body remains accessible"),Request->GetResponseContentAsString(),FString(TEXT("not-json")));
  }
  else if (Case==2) Test.TestEqual(TEXT("reentrant repeat does not duplicate form query"),Request->GetURL(),Root+TEXT("/form?space%20key=a%26b"));
  else if (Case==3)
  {
   Test.TestTrue(TEXT("top-level JSON array stays valid"),Request->bIsValidJsonResponse);
   if (Test.TestEqual(TEXT("top-level array value"),Request->GetResponseValue()->GetType(),EVaJson::Array))
    Test.TestEqual(TEXT("array length"),Request->GetResponseValue()->AsArray().Num(),2);
   const auto& Body=Observer->RequestBodies.FindChecked(CurrentRequestId);
   Test.TestEqual(TEXT("binary core request byte count"),Body.TotalBytes,int64(3));
   Test.TestEqual(TEXT("binary core request NUL/high-byte SHA-256 prefix"),Body.Sha256Prefix,FString(TEXT("92e469e6")));
  }
  else if (Case==5)
  {
   Test.TestEqual(TEXT("cancel status"),Request->GetStatus(),EVaRestRequestStatus::Failed);
   Test.TestEqual(TEXT("cancel clears response code"),Request->GetResponseCode(),-1);
   Test.TestTrue(TEXT("cancel clears body"),Request->GetResponseContent().IsEmpty());
  }
  else if (Case==6)
  {
   CollectGarbage(RF_NoFlags);
   Test.TestFalse(TEXT("Hub releases the unrooted legacy object after completion"),Weak.IsValid());
  }
  else if (Case==7)
  {
   Test.TestEqual(TEXT("reentrant CallURL creates 64 more requests"),Probe->CallRequests.Num(),65);
   CollectGarbage(RF_NoFlags);
   for (const auto& Call:Probe->CallRequests) Test.TestFalse(TEXT("CallURL retired every map entry before user callback/GC"),Call.IsValid());
  }
  Request.Reset(); Weak.Reset(); ++Case; bStarted=false; return false;
 }
private:
 bool Initialize()
 {
  MockVariable=IConsoleManager::Get().FindConsoleVariable(TEXT("http.UrlPatternsToMockResponse"));
  if (!Test.TestNotNull(TEXT("engine native HTTP mock provider"),MockVariable)) return false;
  OriginalMock=MockVariable->GetString(); OriginalFlags=uint32(MockVariable->GetFlags());
  Root=TEXT("https://m6-")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".invalid");
  OverrideTag=FName(*Root);
  FString Patterns=Root+TEXT("/json++POST->201++{\"ok\":7}++Content-Type:application/json,X-M6:present ")+
   Root+TEXT("/plain->422++not-json++Content-Type:text/plain ")+
   Root+TEXT("/binary++POST->200++[1,2]++Content-Type:application/json ")+
   Root+TEXT("->200++{\"ok\":7}++Content-Type:application/json");
  if (!OriginalMock.IsEmpty()) Patterns+=TEXT(" ")+OriginalMock;
  MockVariable->Set(*Patterns,ECVF_SetByConsole,OverrideTag);
  if (!Test.TestEqual(TEXT("owned exact mock patterns admitted"),MockVariable->GetString(),Patterns)) return false;
  Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
  Observer=MakeShared<FObserver,ESPMode::ThreadSafe>(); Observer->Root=Root; FVaRestTrace::AddSink(Observer.ToSharedRef()); bAttached=true;
  Counts=MakeShared<FCounts,ESPMode::ThreadSafe>(); Probe.Reset(NewObject<UVaRestLegacyHttpTestProbe>()); Probe->Root=Root;
  if (GetDefault<UVaRestSettings>()->bUseChunkedParser)
   Test.AddExpectedError(TEXT("VaRest 2 legacy requests use the v2 JSON parser"),EAutomationExpectedErrorFlags::Contains,1);
  return true;
 }
 bool BeginCase()
 {
  if (Case>=8) return BeginCallURLRetirementCase();
  StartedAt=FPlatformTime::Seconds(); BeforeComplete=Probe->Completes;
  ExpectedStarts=Observer->Started.Num()+(Case==2 ? 2 : Case==7 ? 65 : 1); bStarted=true;
  Test.AddInfo(FString::Printf(TEXT("LegacyHttpCase %d"),Case));
  if (Case==7)
  {
   auto* Subsystem=GEngine->GetEngineSubsystem<UVaRestSubsystem>();
   if (!Test.TestNotNull(TEXT("real legacy engine subsystem"),Subsystem)) return true;
   Probe->bFanout=true; FVaRestCallDelegate Callback; Callback.BindDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::CallCompleted);
   Subsystem->CallURL(Root+TEXT("/fan/start"),EVaRestRequestVerb::GET,EVaRestRequestContentType::json,nullptr,Callback); return false;
  }
  Request.Reset(NewObject<UVaRestRequestJSON>()); Weak=Request.Get();
  Request->OnRequestComplete.AddDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::Completed);
  Request->OnRequestFail.AddDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::Failed);
  Request->OnStaticRequestComplete.AddLambda([C=Counts](UVaRestRequestJSON*) { ++C->Complete; C->bOnGT&=IsInGameThread(); });
  Request->OnStaticRequestFail.AddLambda([C=Counts](UVaRestRequestJSON*) { ++C->Fail; C->bOnGT&=IsInGameThread(); });
  Request->GetHttpRequest()->SetTimeout(5);
  FString Path=TEXT("/default");
  if (Case==0) { Path=TEXT("/json"); Request->SetVerb(EVaRestRequestVerb::POST); Request->SetContentType(EVaRestRequestContentType::json); Request->GetRequestObject()->SetIntegerField(TEXT("n"),9); }
  else if (Case==1) Path=TEXT("/plain");
  else if (Case==2) { Path=TEXT("/form"); Request->GetRequestObject()->SetStringField(TEXT("space key"),TEXT("a&b")); Probe->ResendAt=Probe->Completes+1; }
  else if (Case==3) { Path=TEXT("/binary"); Request->SetVerb(EVaRestRequestVerb::POST); Request->SetContentType(EVaRestRequestContentType::binary); Request->SetBinaryRequestContent({0,255,7}); Request->SetBinaryContentType(TEXT("application/x-binary")); Request->SetHeader(TEXT("Content-Type"),TEXT("application/x-override")); }
  else if (Case==4) { Request->SetVerb(EVaRestRequestVerb::CUSTOM); Request->SetCustomVerb(TEXT("PATCH")); Request->SetContentType(EVaRestRequestContentType::json); Request->GetRequestObject()->SetIntegerField(TEXT("n"),9); }
  Request->ProcessURL(TEXT("  ")+Root+Path+TEXT(" \t"));
  CurrentRequestId=Observer->LastStarted;
  Test.TestEqual(TEXT("public processing state before delivery"),Request->GetStatus(),EVaRestRequestStatus::Processing);
  Test.TestEqual(TEXT("no inline UObject result delivery"),Probe->Completes,BeforeComplete);
  const auto Engine=Request->GetHttpRequest();
  if (Case==3)
  {
   Test.TestEqual(TEXT("binary engine payload size"),Engine->GetContentLength(),uint64(3));
   Test.TestEqual(TEXT("explicit legacy header wins"),Engine->GetHeader(TEXT("Content-Type")),FString(TEXT("application/x-override")));
  }
  if (Case==4) Test.TestEqual(TEXT("custom verb staging"),Engine->GetVerb(),FString(TEXT("PATCH")));
  if (Case==5) Request->Cancel();
  if (Case==6)
  {
   Test.TestFalse(TEXT("request does not use AddToRoot"),Request->IsRooted()); Request.Reset(); CollectGarbage(RF_NoFlags);
   Test.TestTrue(TEXT("actual Hub retains the pending unrooted request across GC"),Weak.IsValid());
  }
  return false;
 }
 // Public construction observation only; the SAME owner remains strong across GC.
 // No request strong pin or reflected/private RequestMap inspection is an oracle.
 void StopCallURLCapture()
 {
  if (Observer) Observer->StartedCallout={};
  if (ConstructionHandle.IsValid())
  {
   FCoreUObjectDelegates::OnObjectConstructed.Remove(ConstructionHandle); ConstructionHandle.Reset();
  }
 }
 bool PendingCallIsObserved(int32 Index,const FString& Url)
 {
  if (!Test.TestEqual(TEXT("CallURL constructs exactly the owned public requests"),OwnedCalls.Num(),Index+1)) return false;
  auto* Pending=OwnedCalls[Index].Get();
  if (!Test.TestNotNull(TEXT("public construction signal captured the actual CallURL request"),Pending)) return false;
  return Test.TestEqual(TEXT("captured CallURL has the submitted URL"),Pending->GetURL(),Url)
   && Test.TestEqual(TEXT("actual CallURL is pending before delivery"),Pending->GetStatus(),EVaRestRequestStatus::Processing)
   && Test.TestFalse(TEXT("CallURL request is not rooted"),Pending->IsRooted())
   && Test.TestTrue(TEXT("pending CallURL has the owner's real completion handler"),Pending->OnStaticRequestComplete.IsBoundToObject(CallOwner.Get()))
   && Test.TestTrue(TEXT("pending CallURL has the owner's real failure handler"),Pending->OnStaticRequestFail.IsBoundToObject(CallOwner.Get()));
 }
 FVaRestCallDelegate OwnedCallCallback()
 {
  FVaRestCallDelegate Callback; Callback.BindDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::CallCompleted); return Callback;
 }
 void AssertCallOwnerHandlersDisabled()
 {
  for (const auto& Call:OwnedCalls)
  {
   auto* Pending=Call.Get();
   if (!Test.TestNotNull(TEXT("retired request remains inspectable before controlled GC"),Pending)) { bOwnedSetupValid=false; continue; }
   bOwnedSetupValid&=Test.TestFalse(TEXT("retirement removes actual owner completion handler"),Pending->OnStaticRequestComplete.IsBoundToObject(CallOwner.Get()));
   bOwnedSetupValid&=Test.TestFalse(TEXT("retirement removes actual owner failure handler"),Pending->OnStaticRequestFail.IsBoundToObject(CallOwner.Get()));
   bOwnedSetupValid&=Test.TestEqual(TEXT("retired CallURL has canceled legacy status"),Pending->GetStatus(),EVaRestRequestStatus::Failed);
  }
 }
 bool BeginCallURLRetirementCase()
 {
  StartedAt=FPlatformTime::Seconds(); BeforeComplete=Probe->Completes;
  BeforeCallComplete=Probe->CallCompletions; BeforeStaticComplete=Counts->Complete; BeforeStaticFail=Counts->Fail;
  ExpectedStarts=Observer->Started.Num()+(Case==8 ? 1 : 2); bStarted=true;
  bOwnedSetupValid=true; bNestedDeinitialized=false; Probe->bFanout=false;
  OwnedCallRoot=Root+(Case==8 ? TEXT("/owned-cancel") : TEXT("/owned-deinit"));
  // Use a test-owned REAL subsystem with its proper Engine outer, preserving
  // the registered singleton used by the original fanout and subsequent tests.
  CallOwner.Reset(NewObject<UVaRestSubsystem>(GEngine));
  ConstructionHandle=FCoreUObjectDelegates::OnObjectConstructed.AddLambda([this](UObject* Object)
  {
   if (!IsInGameThread() || !CallOwner || Object->GetOuter()!=CallOwner.Get()) return;
   if (auto* Made=Cast<UVaRestRequestJSON>(Object))
   {
    OwnedCalls.Add(Made);
    Made->OnRequestComplete.AddDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::Completed);
    Made->OnRequestFail.AddDynamic(Probe.Get(),&UVaRestLegacyHttpTestProbe::Failed);
    Made->OnStaticRequestComplete.AddLambda([C=Counts](UVaRestRequestJSON*) { ++C->Complete; C->bOnGT&=IsInGameThread(); });
    Made->OnStaticRequestFail.AddLambda([C=Counts](UVaRestRequestJSON*) { ++C->Fail; C->bOnGT&=IsInGameThread(); });
   }
  });
  Observer->StartedCallout=[this](const FVaRestTraceStarted& Started)
  {
   if (!Started.Url.StartsWith(OwnedCallRoot)) return;
   OwnedCallIds.Add(Started.RequestId);
   if (Case!=9 || Started.Url!=OwnedCallRoot+TEXT("/second") || bNestedDeinitialized) return;
   // Genuine reentry while the second public CallURL/Send stack is still live.
   bNestedDeinitialized=true;
   bOwnedSetupValid&=Test.TestEqual(TEXT("two actual CallURL objects exist before nested teardown"),OwnedCalls.Num(),2);
   bOwnedSetupValid&=Test.TestEqual(TEXT("neither owned call delivered before nested teardown"),Probe->CallCompletions,BeforeCallComplete);
   CallOwner->Deinitialize();
   AssertCallOwnerHandlersDisabled();
   // Explicit stale PUBLIC notification injection, not a simulated HTTP result.
   for (const auto& Call:OwnedCalls) if (auto* Pending=Call.Get()) CallOwner->OnCallComplete(Pending);
   CallOwner->CallURL(OwnedCallRoot+TEXT("/reentrant-after-deinit"),EVaRestRequestVerb::GET,EVaRestRequestContentType::json,nullptr,OwnedCallCallback());
   bOwnedSetupValid&=Test.TestEqual(TEXT("closing rejects CallURL reentry during the original Send stack"),OwnedCalls.Num(),2);
  };
  Test.AddInfo(FString::Printf(TEXT("LegacyHttpCase %d: public CallURL %s"),Case,Case==8 ? TEXT("silent cancel") : TEXT("nested owner teardown")));
  const FString FirstUrl=OwnedCallRoot+TEXT("/first");
  CallOwner->CallURL(FirstUrl,EVaRestRequestVerb::GET,EVaRestRequestContentType::json,nullptr,OwnedCallCallback());
  if (!PendingCallIsObserved(0,FirstUrl)) return true;
  Test.TestEqual(TEXT("owned mock has no inline result delivery"),Probe->CallCompletions,BeforeCallComplete);
  // No target strong pin crosses GC: actual Hub/map ownership retains it.
  CollectGarbage(RF_NoFlags);
  if (!Test.TestTrue(TEXT("actual pending CallURL survives GC under its live owner"),OwnedCalls[0].IsValid())) return true;
  if (Case==8)
  {
   OwnedCalls[0].Get()->Cancel();
   AssertCallOwnerHandlersDisabled();
   if (auto* Canceled=OwnedCalls[0].Get())
   {
    Test.TestEqual(TEXT("explicit CallURL cancel clears response code"),Canceled->GetResponseCode(),-1);
    Test.TestTrue(TEXT("explicit CallURL cancel clears response bytes"),Canceled->GetResponseContent().IsEmpty());
   }
  }
  else
  {
   CallOwner->CallURL(OwnedCallRoot+TEXT("/second"),EVaRestRequestVerb::GET,EVaRestRequestContentType::json,nullptr,OwnedCallCallback());
   bOwnedSetupValid&=Test.TestTrue(TEXT("actual second Started trace reentered Deinitialize"),bNestedDeinitialized);
   AssertCallOwnerHandlersDisabled();
   const int32 CreatedBefore=OwnedCalls.Num();
   CallOwner->CallURL(OwnedCallRoot+TEXT("/after-deinit"),EVaRestRequestVerb::GET,EVaRestRequestContentType::json,nullptr,OwnedCallCallback());
   bOwnedSetupValid&=Test.TestEqual(TEXT("deinitialized owner rejects later CallURL before creating a request"),OwnedCalls.Num(),CreatedBefore);
   CallOwner->Deinitialize(); // Actual repeated teardown is silent/idempotent.
  }
  return !bOwnedSetupValid;
 }
 bool UpdateCallURLRetirementCase()
 {
  if (FPlatformTime::Seconds()-StartedAt>30)
  {
   Test.AddError(FString::Printf(TEXT("CallURL retirement owned terminal barrier timed out at case %d: starts=%d completed=%d"),Case,Observer->Started.Num(),Observer->Completed.Num())); return true;
  }
  const int32 OwnedCount=Case==8 ? 1 : 2;
  if (!Test.TestEqual(TEXT("closing admitted no extra traced request"),Observer->Started.Num(),ExpectedStarts)
   || !Test.TestEqual(TEXT("one Started trace per owned CallURL"),OwnedCallIds.Num(),OwnedCount)) return true;
  for (uint64 Id:OwnedCallIds) if (!Observer->Completed.Contains(Id)) return false;
  if (GFrameCounter<=Observer->LastCompletedFrame) return false;
  for (uint64 Id:OwnedCallIds)
  {
   Test.TestEqual(TEXT("each real canceled CallURL reaches exactly one core terminal"),Observer->CompletionCounts.FindRef(Id),1);
   Test.TestEqual(TEXT("actual core CallURL terminal is cancellation"),Observer->Outcomes.FindChecked(Id),EVaRestTraceOutcome::Canceled);
  }
  Test.TestEqual(TEXT("CallURL retirement never invokes the user callback"),Probe->CallCompletions,BeforeCallComplete);
  Test.TestEqual(TEXT("CallURL retirement never emits a legacy dynamic complete"),Probe->Completes,BeforeComplete);
  Test.TestEqual(TEXT("CallURL retirement never emits a legacy dynamic failure"),Probe->Failures,0);
  Test.TestEqual(TEXT("CallURL retirement never emits a static completion"),Counts->Complete,BeforeStaticComplete);
  Test.TestEqual(TEXT("CallURL retirement never emits a static failure"),Counts->Fail,BeforeStaticFail);
  Test.TestTrue(TEXT("owned observations and callbacks stay on GT"),Probe->bOnGT && Counts->bOnGT);
  // Keep the SAME owner strong: losing it cannot hide a leaked reflected map pin.
  Test.TestNotNull(TEXT("same owner remains strong across the release oracle"),CallOwner.Get());
  CollectGarbage(RF_NoFlags);
  for (const auto& Call:OwnedCalls) Test.TestFalse(TEXT("silent retirement releases each request while its owner is alive"),Call.IsValid());
  StopCallURLCapture(); CallOwner->Deinitialize(); CallOwner.Reset(); OwnedCalls.Empty(); OwnedCallIds.Empty();
  ++Case; bStarted=false; return false;
 }
 FAutomationTestBase& Test;
 TStrongObjectPtr<UVaRestLegacyHttpTestProbe> Probe;
 TStrongObjectPtr<UVaRestRequestJSON> Request;
 TWeakObjectPtr<UVaRestRequestJSON> Weak;
 TSharedPtr<FCounts,ESPMode::ThreadSafe> Counts;
 TSharedPtr<FObserver,ESPMode::ThreadSafe> Observer;
 TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
 IConsoleVariable* MockVariable=nullptr;
 FString OriginalMock,Root;
 FName OverrideTag;
 uint64 CurrentRequestId=0;
 uint32 OriginalFlags=0;
 int32 Case=0,BeforeComplete=0,ExpectedStarts=0;
 double StartedAt=0;
 bool bStarted=false,bAttached=false;
 TStrongObjectPtr<UVaRestSubsystem> CallOwner;
 TArray<TWeakObjectPtr<UVaRestRequestJSON>> OwnedCalls;
 TArray<uint64> OwnedCallIds;
 FDelegateHandle ConstructionHandle;
 FString OwnedCallRoot;
 int32 BeforeCallComplete=0,BeforeStaticComplete=0,BeforeStaticFail=0;
 bool bOwnedSetupValid=true,bNestedDeinitialized=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyHttpFacadeTest,"VaRest.Compat.Legacy.HttpFacade",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyHttpFacadeTest::RunTest(const FString& Parameters)
{
 ADD_LATENT_AUTOMATION_COMMAND(VaRestLegacyHttpFacadeTest::FRun(*this)); return true;
}
#endif
