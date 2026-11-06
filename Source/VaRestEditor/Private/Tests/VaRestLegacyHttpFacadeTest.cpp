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
 virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings D; D.Flags=EVaRestTraceDetail::Metadata|EVaRestTraceDetail::Bodies; D.BodyCap=1024; D.BinaryCap=16; return D; }
 virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
 virtual void OnEvent(const FVaRestTraceEvent& Event) override
 {
  if (Event.Kind==EVaRestTraceKind::Started)
  {
   const auto& S=Event.Payload.Get<FVaRestTraceStarted>(); if (S.Url.Contains(Root)) { Started.Add(S.RequestId); LastStarted=S.RequestId; }
  }
  else if (Event.Kind==EVaRestTraceKind::Completed)
  {
   const auto& C=Event.Payload.Get<FVaRestTraceCompleted>();
   if (Started.Contains(C.RequestId)) { Completed.Add(C.RequestId); LastCompletedFrame=GFrameCounter; }
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
  if (Case==8) return true;
  if (!bStarted) return BeginCase();
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
   Test.TestEqual(TEXT("binary engine payload size"),Engine->GetContentLength(),3);
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
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyHttpFacadeTest,"VaRest.Compat.Legacy.HttpFacade",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyHttpFacadeTest::RunTest(const FString& Parameters)
{
 ADD_LATENT_AUTOMATION_COMMAND(VaRestLegacyHttpFacadeTest::FRun(*this)); return true;
}
#endif
