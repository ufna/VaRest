// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorTestUtil.h"
#include "Misc/Guid.h"
#include "VaRestCache.h"
#include "VaRestEnvelope.h"
#include "VaRestOffline.h"
#include "VaRestRetry.h"
#include "VaRestStage.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private::CaptureTesting
{
namespace
{
template<class P> void Emit(EVaRestTraceKind Kind,double At,P Payload)
{
 FVaRestTrace::Emit(Kind,At,FVaRestTracePayload(TInPlaceType<P>(),MoveTemp(Payload)));
}
bool Sequence(FCaptureFixture& F,uint64 Id,const TCHAR* Expected)
{
 return F.Test.TestEqualSensitive(TEXT("actual producer sequence"),F.Recorder->KindsOf(Id),FString(Expected));
}
bool SuccessAndRefusal(FAutomationTestBase& T)
{
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T); if(!F.Ready()) return false;
 F.Rig.Fake->Enqueue(Json(200,"{\"ordinary\":7}"));
 auto Good=MakeOutcome<FVaRestJson>(); const auto H=F.Track(Observe(F.Rig.Client.Get(TEXT("/success")),Good).Send());
 if(!T.TestTrue(TEXT("success completes"),F.Rig.Wait(Good))) return false;
 F.Clock->AdvanceNanos(900000000000ll); F.Settle();
 Sequence(F,H.GetId(),TEXT("Started,Dispatched,Ended,Completed")); F.Check(H.GetId());
 F.Row(H.GetId(),[&T](const auto& R) {
  T.TestEqual(TEXT("virtual source start zero is known"),R.StartedAt,0.0);
  T.TestEqual(TEXT("source completion zero is not receipt 900 seconds"),R.CompletedAt,0.0);
  T.TestEqual(TEXT("independent receipt clock"),R.CompletedObservedAt,900.0);
  T.TestTrue(TEXT("source success complete, sealed and unpinned"),R.bSealed && !R.bPinned && !R.bPartial && R.EndState==EVaRestInspectorEndState::Success);
 });
 auto Bad=MakeOutcome<FVaRestJson>(); const auto B=F.Track(Observe(F.Rig.Client.Get(TEXT("/bad/{id")),Bad).Send());
 if(!T.TestTrue(TEXT("local refusal completes"),F.Rig.Wait(Bad))) return false;
 F.Settle(); Sequence(F,B.GetId(),TEXT("Started,Completed")); F.Check(B.GetId());
 F.Row(B.GetId(),[&T](const auto& R) {
  T.TestTrue(TEXT("local refusal has known empty physical history"),R.DispatchHistory==EVaRestInspectorDispatchHistory::KnownNeverDispatched && R.Dispatches.IsEmpty());
  T.TestEqual(TEXT("local refusal final status unknown"),R.FinalStatus,0);
  T.TestTrue(TEXT("local refusal does not invent delivered entity or wire"),R.FinalBodyBytes==-1 && R.FinalStorage==EVaRestTraceResponseStorage::Unknown && R.WireBytesUp==0 && R.WireBytesDown==0);
 });
 auto S=F.Snapshot(); if(S) S->ForEachRecord([&](const auto& R){if(R.RequestId==H.GetId()) T.TestEqual(TEXT("retained identity equals live handle"),R.RequestId,H.GetId());});
 return true;
}
bool Retry(FAutomationTestBase& T)
{
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T); if(!F.Ready()) return false;
 auto P=FVaRestRetryPolicy::Exponential(3); P.Backoff.Jitter=EVaRestJitter::None; F.Rig.Client.WithRetry(P);
 auto Busy=Json(503); Busy.Headers.Set(TEXT("Retry-After"),TEXT("2")); F.Rig.Fake->Enqueue(Busy); F.Rig.Fake->Enqueue(Json(200));
 auto O=MakeOutcome<FVaRestJson>(); auto H=F.Track(Observe(F.Rig.Client.Get(TEXT("/busy")),O).Send());
 if(!T.TestTrue(TEXT("503 backoff armed"),PumpUntil([Clock=F.Rig.Clock]{return Clock->NumPendingTimers()==1;}))) return false;
 F.DrainCapture(); F.Rig.Clock->Advance(2);
 if(!T.TestTrue(TEXT("retry succeeds"),F.Rig.Wait(O))) return false;
 F.Settle(); Sequence(F,H.GetId(),TEXT("Started,Dispatched,Ended,Note,Dispatched,Ended,Completed")); F.Check(H.GetId());
 F.Row(H.GetId(),[&T](const auto& R) {
  if(!T.TestEqual(TEXT("retry retains two physical rows"),R.Dispatches.Num(),2)) return;
  T.TestEqual(TEXT("backoff in seconds"),R.Dispatches[1].GapSeconds,2.0);
  T.TestTrue(TEXT("backoff known"),R.Dispatches[1].Gap.bKnown);
  T.TestEqual(TEXT("completion source 2 seconds"),R.CompletedAt,2.0);
 }); return true;
}
bool EnvelopeHttp(FAutomationTestBase& T)
{
 using namespace VaRest::Http::Testing;
 for(bool Secret:{false,true}){
  FCaptureFixture F(T);if(!F.Ready())return false;
  if(Secret){
   F.Rig.Client.SetSecretValue(TEXT("EnvelopeData"),TEXT("EnvelopeDataCanary57391"));
   F.Rig.Client.SetSecretValue(TEXT("EnvelopePredicate"),TEXT("EnvelopePredicateCanary68402"));
   F.Rig.Client.SetSecretValue(TEXT("EnvelopeExpected"),TEXT("EnvelopeExpectedCanary79513"));
  }
  F.Rig.Client.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data.EnvelopeDataCanary57391")).SuccessWhen(TEXT("checks.EnvelopePredicateCanary68402"),{TEXT("accepted-EnvelopeExpectedCanary79513")}).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message")));
  F.Rig.Fake->Enqueue(Json(200,R"({"data":{"EnvelopeDataCanary57391":{"value":7}},"checks":{"EnvelopePredicateCanary68402":"ordinary-denied"},"code":"E_CONFIG","message":"ordinary configuration refused"})"));
  auto O=MakeOutcome<FVaRestJson>();const auto H=F.Track(Observe(F.Rig.Client.Get(TEXT("/envelope-config")),O).Send());
  if(!T.TestTrue(TEXT("public envelope failure delivered"),F.Rig.Wait(O)))return false;
  F.Settle();F.Check(H.GetId());Sequence(F,H.GetId(),TEXT("Started,Dispatched,Ended,Completed"));
  T.TestTrue(TEXT("real public predicate failure"),O->Error.Kind==EVaRestErrorKind::Server && O->SuccessCalls.load()==0);
  F.Row(H.GetId(),[&](const auto& R){
   T.TestEqualSensitive(TEXT("HTTP producer configured data path survives redacted"),FString(R.Completed.Text(View::EText::DataPath)),FString(Secret?TEXT("data.***"):TEXT("data.EnvelopeDataCanary57391")));
   T.TestEqualSensitive(TEXT("HTTP producer configured predicate path survives redacted"),FString(R.Completed.Text(View::EText::PredicatePath)),FString(Secret?TEXT("checks.***"):TEXT("checks.EnvelopePredicateCanary68402")));
   T.TestEqualSensitive(TEXT("HTTP producer configured expected survives redacted"),FString(R.Completed.Text(View::EText::Expected)),FString(Secret?TEXT("accepted-***"):TEXT("accepted-EnvelopeExpectedCanary79513")));
   T.TestEqualSensitive(TEXT("HTTP benign actual survives"),FString(R.Completed.Text(View::EText::Actual)),FString(TEXT("ordinary-denied")));
   T.TestEqualSensitive(TEXT("HTTP benign code survives"),FString(R.Completed.Text(View::EText::EnvelopeCode)),FString(TEXT("E_CONFIG")));
   T.TestEqualSensitive(TEXT("HTTP benign message survives"),FString(R.Completed.Text(View::EText::EnvelopeMessage)),FString(TEXT("ordinary configuration refused")));
   T.TestEqual(TEXT("envelope failure delivered HTTP status stays 200"),R.FinalStatus,200);
  });
 }
 return true;
}
class FCancelSink final:public IVaRestTraceSink
{
public:
 explicit FCancelSink(TSharedRef<FVaRestRequestHandle,ESPMode::ThreadSafe> H):Target(MoveTemp(H)) {}
 FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings D;D.Flags=EVaRestTraceDetail::Metadata;return D; }
 void OnAttached(const FVaRestTraceSnapshot&) override {}
 void OnEvent(const FVaRestTraceEvent& E) override {
  if(E.Kind==EVaRestTraceKind::AttemptDispatched && E.Payload.Get<FVaRestTraceAttemptDispatched>().RequestId==Target->GetId()) Target->Cancel();
 }
private:TSharedRef<FVaRestRequestHandle,ESPMode::ThreadSafe> Target;
};
bool Cancellation(FAutomationTestBase& T,bool bAtDispatch)
{
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T); if(!F.Ready()) return false;
 F.Rig.Client.WithMaxConcurrent(1); F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
 auto A=MakeOutcome<FVaRestJson>(); auto B=MakeOutcome<FVaRestJson>();
 const auto Block=F.Track(Observe(F.Rig.Client.Get(TEXT("/block")),A).Send());
 auto Target=MakeShared<FVaRestRequestHandle,ESPMode::ThreadSafe>();
 auto Canceler=MakeShared<FCancelSink,ESPMode::ThreadSafe>(Target);
 if(bAtDispatch) { FVaRestTrace::AddSink(Canceler); F.OnCleanup([Canceler]{FVaRestTrace::RemoveSink(Canceler);}); }
 *Target=F.Track(Observe(F.Rig.Client.Get(TEXT("/target")),B).Send());
 if(!T.TestEqual(TEXT("target initially waits in limiter"),F.Rig.Fake->NumCalls(),1)) return false;
 if(!bAtDispatch) Target->Cancel();
 F.Rig.Fake->Complete(0,Json(200));
 if(!T.TestTrue(TEXT("both requests settle"),F.Rig.Wait(A) && F.Rig.Wait(B))) return false;
 F.Settle(); Sequence(F,Target->GetId(),bAtDispatch?TEXT("Started,Dispatched,Ended,Completed"):TEXT("Started,Completed"));
 F.Check(Target->GetId()); T.TestEqual(TEXT("canceled target never starts transport"),F.Rig.Fake->NumCalls(),1);
 F.Row(Target->GetId(),[&](const auto& R) {
  T.TestTrue(TEXT("real canceled terminal"),R.EndState==EVaRestInspectorEndState::Canceled && R.bSealed);
  T.TestEqual(TEXT("cancellation physical count"),R.Dispatches.Num(),bAtDispatch?1:0);
  if(!bAtDispatch) T.TestTrue(TEXT("unsent limiter cancellation has no invented row"),R.DispatchHistory==EVaRestInspectorDispatchHistory::KnownNeverDispatched);
 }); return true;
}
FVaRestFakeResponse Cached(const TCHAR* Age=TEXT("max-age=60"))
{
 auto R=FVaRestFakeResponse::Text(200,TEXT("original-body"));
 R.Headers.Set(TEXT("Cache-Control"),Age); R.Headers.Set(TEXT("ETag"),TEXT("\"golden\"")); return R;
}
bool Cache(FAutomationTestBase& T,int32 Mode)
{
 // 0 memory; 1 disk; 2 conditional 304; 3 known-offline stale; 4 attempted stale.
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T,Mode==1); if(!F.Ready()) return false;
 auto P=Mode==1?FVaRestCachePolicy::MemoryAndDisk():FVaRestCachePolicy::Memory();
 if(Mode==1) P.MemoryBytes=0;
 P.DefaultTtlSeconds=60; P.bServeStaleOnError=Mode>=3; F.Rig.Client.WithCache(P);
 F.Rig.Fake->Enqueue(Cached(Mode>=2?TEXT("max-age=0"):TEXT("max-age=60")));
 auto Seed=MakeOutcome<FString>(); F.Track(Observe(F.Rig.Client.Get<FString>(TEXT("/cache")),Seed).Send());
 if(!T.TestTrue(TEXT("cache seed ready"),F.Rig.Wait(Seed) && F.Settle())) return false;
 if(Mode==2) {auto R=FVaRestFakeResponse::Text(304,TEXT(""));R.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));F.Rig.Fake->Enqueue(R);}
 if(Mode==3) F.Rig.Connectivity->ReportOffline();
 if(Mode==4) F.Rig.Fake->Enqueue(FVaRestFakeResponse::Failed(EVaRestTransportFailure::Connection));
 auto O=MakeOutcome<FString>(); const auto H=F.Track(Observe(F.Rig.Client.Get<FString>(TEXT("/cache")).NoRetry(),O).Send());
 if(!T.TestTrue(TEXT("cached result completes"),F.Rig.Wait(O))) return false;
 F.Settle(); F.Check(H.GetId());
 T.TestTrue(TEXT("real cache delivers original entity"),O->Value.IsSet() && O->Value.GetValue()==TEXT("original-body"));
 const TCHAR* Expected=Mode<2?TEXT("Started,Completed"):(Mode==3?TEXT("Started,Note,Completed"):TEXT("Started,Dispatched,Ended,Note,Completed"));
 Sequence(F,H.GetId(),Expected);
 F.Row(H.GetId(),[&](const auto& R) {
  T.TestEqual(TEXT("delivered response remains 200"),R.FinalStatus,200);
  T.TestEqual(TEXT("delivered cached bytes"),R.FinalBodyBytes,int64(13));
  T.TestTrue(TEXT("no final cached body producer invented"),R.FinalPreviewState==EVaRestInspectorDetailState::NoProducer);
  T.TestEqual(TEXT("cached final is complete without false partial"),R.bPartial,false);
  if(Mode<2 || Mode==3) T.TestTrue(TEXT("cache without transport has no wire or attempt"),R.WireBytesDown==0 && R.Dispatches.IsEmpty());
  if(Mode==2 && !R.Dispatches.IsEmpty() && R.Dispatches[0].Ended.Fixed) T.TestEqual(TEXT("wire status 304 differs from delivered 200"),R.Dispatches[0].Ended.Fixed->Scalars.Status,304);
  if(R.Completed.Fixed) {
   const auto Want=Mode==0?EVaRestTraceCacheLookup::HitMemory:Mode==1?EVaRestTraceCacheLookup::HitDisk:Mode==2?EVaRestTraceCacheLookup::Revalidate:EVaRestTraceCacheLookup::Stale;
   T.TestTrue(TEXT("final actual cache provenance"),R.Completed.Fixed->Scalars.CacheLookup==Want && R.Completed.Fixed->Scalars.bFromCache);
  }
 });
 const auto Before=F.Recorder->KindsOf(H.GetId()); F.Settle();
 T.TestEqualSensitive(TEXT("persistence emits no late CacheStored"),F.Recorder->KindsOf(H.GetId()),Before);
 return true;
}
bool QueueReplay(FAutomationTestBase& T)
{
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T,true); if(!F.Ready()) return false;
 F.Rig.Client.WithOfflineQueue(TEXT("inspector-owned-queue")); F.Rig.Connectivity->SetProbeInterval(200); F.Rig.Connectivity->ReportOffline();
 auto Queued=MakeShared<TArray<FString>,ESPMode::ThreadSafe>();
 auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
 auto O=MakeOutcome<void>();
 F.Rig.Client.OnQueuedResult().AddLambda([Results](const FVaRestQueuedResult& R){Results->Add(R);});
 const auto H=F.Track(Observe(F.Rig.Client.Post<void>(TEXT("/write")).Body(TEXT("safe-body")).Idempotent().QueueWhenOffline().OnQueued([Queued](const FString& Id){Queued->Add(Id);}),O).Send());
 if(!T.TestTrue(TEXT("durable queue acknowledgement"),PumpUntil([Queued]{return Queued->Num()==1;}))) return false;
 F.Settle(); Sequence(F,H.GetId(),TEXT("Started,Note,Completed")); F.Check(H.GetId());
 FGuid QueueGuid;T.TestTrue(TEXT("real queue GUID and inactive original"),FGuid::Parse((*Queued)[0],QueueGuid) && QueueGuid.IsValid() && !H.IsActive());
 T.TestEqual(TEXT("queued original never calls ordinary completion"),O->CompleteCalls.load(),0);
 F.Row(H.GetId(),[&](const auto& R){T.TestTrue(TEXT("queued is a sealed terminal, no transport success"),R.EndState==EVaRestInspectorEndState::Queued && R.bSealed && R.Dispatches.IsEmpty());});
 F.Rig.Clock->Advance(100); F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); F.Rig.Connectivity->ReportOnline(H.GetId());
 if(!T.TestTrue(TEXT("public replay reaches fake transport"),PumpUntil([Fake=F.Rig.Fake]{return Fake->NumCalls()==1;}))) return false;
 const uint64 Replay=F.Rig.Fake->GetCall(0).Request.RequestId;
 if(!T.TestTrue(TEXT("new logical replay identity"),Replay!=0 && Replay!=H.GetId())) return false;
 F.Rig.Clock->Advance(2); F.Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("replayed")));
 if(!T.TestTrue(TEXT("durable retirement delivered"),PumpUntil([Results]{return Results->Num()==1;}))) return false;
 F.Settle(); F.Check(Replay); Sequence(F,Replay,TEXT("Started,Dispatched,Ended,Completed"));
 T.TestTrue(TEXT("actual durable retirement names the same successful item"),(*Results)[0].QueueId==(*Queued)[0] && (*Results)[0].bSuccess && F.Rig.Client.GetQueuedCount()==0);
 F.Row(Replay,[&](const auto& R){
  T.TestEqualSensitive(TEXT("replay exact queue GUID"),FString(R.Started.Text(View::EText::QueueId)),(*Queued)[0]);
  if(R.Started.Fixed) T.TestEqual(TEXT("replay original handle"),R.Started.Fixed->Scalars.OriginalRequestId,H.GetId());
  T.TestTrue(TEXT("queue days excluded from replay total"),R.Total.bKnown && R.Total.Seconds==2.0);
 });
 auto S=F.Snapshot(); if(S) {
  T.TestTrue(TEXT("real replay link retained"),S->GetLinks().ContainsByPredicate([&](const auto& L){return L.FromRequestId==Replay && L.ToRequestId==H.GetId() && L.QueueId==(*Queued)[0];}));
  bool Marker=false; S->ForEachRecord([&](const auto& R){if(R.Kind==EVaRestInspectorRecordKind::Marker && R.MarkerInfo.bHasSourceObservation && R.MarkerInfo.CauseRequestId==H.GetId() && R.ClientId==F.Rig.Client.GetId() && R.ScopeId==FVaRestTrace::ProcessScopeId) Marker=true;});
  T.TestTrue(TEXT("connectivity marker keeps actual cause link"),Marker);
 }
 T.TestEqual(TEXT("replay never refires original success"),O->SuccessCalls.load(),0);
 T.TestEqual(TEXT("replay never refires original error"),O->ErrorCalls.load(),0);
 T.TestEqual(TEXT("replay never refires original completion"),O->CompleteCalls.load(),0);
 return true;
}
bool LateAttach(FAutomationTestBase& T,bool bPreviouslyOff)
{
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T,false,false,!bPreviouslyOff); if(!F.Ready()) return false;
 if(bPreviouslyOff) T.TestTrue(TEXT("request genuinely starts with trace mask off"),FVaRestTrace::Detail()==EVaRestTraceDetail::None);
 F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); auto O=MakeOutcome<FVaRestJson>();
 const auto H=F.Track(Observe(F.Rig.Client.Get(TEXT("/late")),O).Send());
 if(!T.TestTrue(TEXT("actual request in flight before attachment"),PumpUntil([Fake=F.Rig.Fake]{return Fake->NumCalls()==1;}))) return false;
 F.AddRecorder(); if(!F.Start()) return false; F.DrainCapture();
 F.Row(H.GetId(),[&](const auto& R){
  T.TestTrue(TEXT("OnAttached owns real partial in-flight row"),R.bPartial && R.bPinned && !R.bTerminalKnown);
  T.TestTrue(TEXT("snapshot preserves known zero monotonic start"),R.bStartedAtKnown && R.StartedAt==0.0);
  if(bPreviouslyOff) T.TestFalse(TEXT("no reconstructed historical UTC"),R.bStartUtcKnown);
 });
 auto Held=F.Snapshot(); const uint64 Revision=Held?Held->GetRevision():0;
 F.Rig.Clock->Advance(1); F.Rig.Fake->Complete(0,Json(200));
 if(!T.TestTrue(TEXT("late attached request completes"),F.Rig.Wait(O))) return false; F.Settle();
 F.Row(H.GetId(),[&](const auto& R){T.TestTrue(TEXT("late completion preserves partial history"),R.bPartial && R.bTerminalKnown && R.RequestId==H.GetId());});
 if(Held) {T.TestEqual(TEXT("retained snapshot revision immutable"),Held->GetRevision(),Revision);Held->ForEachRecord([&](const auto& R){if(R.RequestId==H.GetId()) T.TestFalse(TEXT("old in-flight snapshot never turns terminal"),R.bTerminalKnown);});}
 return true;
}
class FTwice final:public IVaRestStage
{
public:
 FName GetStageName() const override {return TEXT("InspectorPublicHedge");}
 bool MayReissue() const override {return true;}
 void Process(const FVaRestExchangeRef& E,const FVaRestStageNext& N,FVaRestOutcomeCallback&& Callback) override {
  Weak=E; Next.Emplace(N); Done=MakeShared<FVaRestOutcomeCallback,ESPMode::ThreadSafe>(MoveTemp(Callback));
  N.Proceed(E,[Count=FirstEnds](FVaRestAttemptOutcome&&){++*Count;});
 }
 void Again() {
  auto E=Weak.Pin(); if(!E || !Next.IsSet()) return;
  Next->Proceed(E.ToSharedRef(),[Into=Held](FVaRestAttemptOutcome&& O){Into->Emplace(MoveTemp(O));});
 }
 void Answer() {
  if(!Held->IsSet() || !Done || !*Done) return;
  auto O=MoveTemp(Held->GetValue()); Held->Reset(); auto C=MoveTemp(*Done);Done->Reset();C(MoveTemp(O));
 }
 void Release() {Done.Reset();Next.Reset();Held->Reset();Weak.Reset();}
 TSharedRef<std::atomic<int32>,ESPMode::ThreadSafe> FirstEnds=MakeShared<std::atomic<int32>,ESPMode::ThreadSafe>(0);
 TSharedRef<TOptional<FVaRestAttemptOutcome>,ESPMode::ThreadSafe> Held=MakeShared<TOptional<FVaRestAttemptOutcome>,ESPMode::ThreadSafe>();
private:
 TWeakPtr<FVaRestExchange,ESPMode::ThreadSafe> Weak;
 TOptional<FVaRestStageNext> Next;
 TSharedPtr<FVaRestOutcomeCallback,ESPMode::ThreadSafe> Done;
};
bool SameAttempt(FAutomationTestBase& T,bool bReverse)
{
 using namespace VaRest::Http::Testing;
 FCaptureFixture F(T); if(!F.Ready()) return false;
 auto Twice=MakeShared<FTwice,ESPMode::ThreadSafe>(); F.OnCleanup([Twice]{Twice->Release();});
 F.Rig.Client.Use(Twice,EVaRestStageSlot::PerAttempt); F.Rig.Fake->bCompleteOnWorker=false;
 F.Rig.Fake->SetResponder([](const FVaRestTransportRequest&){return FVaRestFakeResponse::Manual();});
 auto O=MakeOutcome<FVaRestJson>(); auto H=F.Track(Observe(F.Rig.Client.Get(TEXT("/same-attempt")),O).Send()); Twice->Again();
 if(!T.TestEqual(TEXT("two actual physical starts"),F.Rig.Fake->NumCalls(),2)) return false;
 T.TestTrue(TEXT("physical calls share Attempt 1"),F.Rig.Fake->GetCall(0).Request.Attempt==1 && F.Rig.Fake->GetCall(1).Request.Attempt==1);
 const int32 First=bReverse?1:0; F.Rig.Fake->Complete(First,Json(200,First?"{\"n\":2}":"{\"n\":1}"));
 if(!T.TestTrue(TEXT("first returned call observed"),PumpUntil([Twice,bReverse]{return bReverse?Twice->Held->IsSet():Twice->FirstEnds->load()==1;}))) return false;
 F.Rig.Fake->Complete(1-First,Json(200,First?"{\"n\":1}":"{\"n\":2}"));
 if(!T.TestTrue(TEXT("both ended before logical completion"),PumpUntil([Twice]{return Twice->Held->IsSet() && Twice->FirstEnds->load()==1;}))) return false;
 Twice->Answer(); if(!T.TestTrue(TEXT("hedge completes once"),F.Rig.Wait(O))) return false;
 Twice->Release(); F.Settle(); F.Check(H.GetId());
 F.Row(H.GetId(),[&](const auto& R) {
  if(!T.TestEqual(TEXT("same attempt never merges physical dispatch rows"),R.Dispatches.Num(),2)) return;
  T.TestTrue(TEXT("distinct captured nonzero IDs"),R.Dispatches[0].DispatchId!=0 && R.Dispatches[1].DispatchId!=0 && R.Dispatches[0].DispatchId!=R.Dispatches[1].DispatchId);
  T.TestTrue(TEXT("both source ends precede Completed, including stale end"),R.Dispatches[0].Ended.Fixed && R.Dispatches[1].Ended.Fixed && R.Completed.Fixed && R.Dispatches[0].Ended.Fixed->Header.Seq<R.Completed.Fixed->Header.Seq && R.Dispatches[1].Ended.Fixed->Header.Seq<R.Completed.Fixed->Header.Seq);
 }); return true;
}
// SOURCE-VALIDATED REDACTED CONSUMER FIXTURES: owning core source goldens,
// not freshly recorded private auth/response-route executions. Only the owning
// assertions' projected fields are materialized. Omitted wire counters/times
// use unknown sentinels; source UTC/headers are absent. Provenance in report.
uint64 BeginConsumer(FCaptureFixture& F,const TCHAR* Path,uint64 Client=0,EVaRestTraceRequestFlags Flags=EVaRestTraceRequestFlags::None)
{
 const uint64 Id=FVaRestTrace::NewRequestId(); FVaRestTraceStarted S;
 S.RequestId=Id;S.ClientId=Client?Client:F.Rig.Client.GetId();S.Verb=TEXT("GET");S.PathTemplate=Path;S.Url=FString(TEXT("https://api.test"))+Path;S.Flags=Flags;
 Emit(EVaRestTraceKind::Started,0.0,S);return Id;
}
void Dispatch(uint64 Id,uint64 Serial,int32 Attempt,double At,double Start=0,double Auth=0,uint64 Refresh=0,EVaRestTraceGap Gap=EVaRestTraceGap::None,double GapSeconds=0)
{
 FVaRestTraceAttemptDispatched D;D.RequestId=Id;D.DispatchId=Serial;D.Attempt=Attempt;D.AttemptStart=Start;D.AuthWait=Auth;D.AuthRefreshId=Refresh;D.GapKind=Gap;D.GapSeconds=GapSeconds;Emit(EVaRestTraceKind::AttemptDispatched,At,D);
}
void End(uint64 Id,uint64 Serial,int32 Attempt,int32 Status,double At,bool Pending=false,int64 WireDown=-1)
{
 FVaRestTraceAttemptEnded E;E.RequestId=Id;E.DispatchId=Serial;E.Attempt=Attempt;E.Status=Status;E.BytesUp=-1;E.BytesDown=WireDown;E.bExcerptPending=Pending;Emit(EVaRestTraceKind::AttemptEnded,At,E);
}
void Complete(uint64 Id,double At,uint64 Refresh=0)
{
 FVaRestTraceCompleted C;C.RequestId=Id;C.Response.Status=200;C.AuthRefreshId=Refresh;Emit(EVaRestTraceKind::Completed,At,C);
}
bool AuthConsumers(FAutomationTestBase& T)
{
 FCaptureFixture F(T);if(!F.Ready()) return false;
 // RefreshParked: projected A/B/C exact order, waits and source clock values.
 const uint64 A=BeginConsumer(F,TEXT("/a")); Dispatch(A,1,1,0);End(A,1,1,401,0);
 const uint64 B=BeginConsumer(F,TEXT("/b")),C=BeginConsumer(F,TEXT("/c"));
 const uint64 Refresh=FVaRestTrace::NewRequestId(); // Opaque owning FHandAuth refresh identity; no invented refresh wire row.
 Dispatch(A,2,2,.4,.4,0,Refresh,EVaRestTraceGap::SessionRefresh,.4);End(A,2,2,200,.4);Complete(A,.4,Refresh);
 for(uint64 Id:{B,C}) {Dispatch(Id,1,1,.4,0,.4,Refresh);End(Id,1,1,200,.4);Complete(Id,.4,Refresh);}
 FVaRestTraceMarker M;M.ClientId=F.Rig.Client.GetId();M.CauseRequestId=A;M.Kind=EVaRestTraceMarkerKind::SessionRefreshed;Emit(EVaRestTraceKind::Marker,.4,M);
 // ProactiveParked owns a first attempt's in-attempt AuthWait, not a backoff.
 const uint64 ProactiveRefresh=FVaRestTrace::NewRequestId();
 const uint64 P=BeginConsumer(F,TEXT("/p"));Dispatch(P,1,1,.3,0,.3,ProactiveRefresh);End(P,1,1,200,.3);Complete(P,.3,ProactiveRefresh);
 // ParkedRejected owns no physical dispatch despite its terminal refresh link.
 const uint64 RejectedRefresh=FVaRestTrace::NewRequestId();
 const uint64 Rejected=BeginConsumer(F,TEXT("/p"));FVaRestTraceCompleted Reject;Reject.RequestId=Rejected;Reject.Outcome=EVaRestTraceOutcome::Error;Reject.Error.Kind=EVaRestErrorKind::Auth;Reject.AuthRefreshId=RejectedRefresh;Emit(EVaRestTraceKind::Completed,0.,Reject);
 // ParkedFailed: unsent attempt 1 retry note, only attempt 2 has a physical row.
 const uint64 FailedRefresh=FVaRestTrace::NewRequestId();
 const uint64 Failed=BeginConsumer(F,TEXT("/target"));FVaRestTraceNote N;N.RequestId=Failed;N.Attempt=1;N.Kind=EVaRestTraceNoteKind::RetryScheduled;N.Reason=TEXT("Connection");N.Delay=.5;Emit(EVaRestTraceKind::Note,.25,N);
 Dispatch(Failed,1,2,.75,.75,0,0,EVaRestTraceGap::Backoff,.5);End(Failed,1,2,503,.75);
 FVaRestTraceCompleted Fail;Fail.RequestId=Failed;Fail.Outcome=EVaRestTraceOutcome::Error;Fail.Response.Status=503;Fail.Error.Kind=EVaRestErrorKind::Http;Fail.Error.HttpStatus=503;Fail.AuthRefreshId=FailedRefresh;Emit(EVaRestTraceKind::Completed,.75,Fail);
 F.DrainCapture();for(uint64 Id:{A,B,C,P,Rejected,Failed}) F.Check(Id);
 F.Row(B,[&T](const auto& R){if(!R.Dispatches.IsEmpty()) T.TestTrue(TEXT("parked first attempt Auth=.4 seconds"),R.Dispatches[0].Auth.bKnown && R.Dispatches[0].Auth.Seconds==.4);});
 F.Row(P,[&T](const auto& R){if(!R.Dispatches.IsEmpty()) T.TestTrue(TEXT("proactive park is Auth=.3 seconds"),R.Dispatches[0].Auth.bKnown && R.Dispatches[0].Auth.Seconds==.3);});
 F.Row(Rejected,[&T](const auto& R){T.TestTrue(TEXT("rejected unsent attempt not fabricated"),R.Dispatches.IsEmpty() && R.bTerminalKnown);});
 F.Row(Failed,[&T](const auto& R){T.TestTrue(TEXT("failed unsent attempt 1 never fabricated"),R.Dispatches.Num()==1 && R.Dispatches[0].Attempt==2);});
 auto S=F.Snapshot();if(S) T.TestTrue(TEXT("auth backlink real consumer link"),S->GetLinks().ContainsByPredicate([&](const auto& L){return L.bAuth && L.FromRequestId==A && L.ToRequestId==Refresh;}));
 return true;
}
void Excerpt(uint64 Id,uint64 Serial,const char* Body)
{
 auto Bytes=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>();for(const char* C=Body;*C;++C)Bytes->Add(uint8(*C));
 FVaRestTraceExcerptEvent X;X.RequestId=Id;X.DispatchId=Serial;X.Attempt=1;X.Owner=EVaRestTraceExcerptOwner::Response;X.Excerpt.Kind=EVaRestExcerptKind::Json;X.Excerpt.Bytes=Bytes;X.Excerpt.TotalBytes=Bytes->Num();Emit(EVaRestTraceKind::Excerpt,0.,X);
}
bool MixedConsumer(FAutomationTestBase& T)
{
 FCaptureFixture F(T);if(!F.Ready()) return false;
 const uint64 Id=BeginConsumer(F,TEXT("/mixed-excerpt"));Dispatch(Id,1,1,0);End(Id,1,1,503,0,true,7);
 Dispatch(Id,2,1,0);End(Id,2,1,200,0,true,7);Excerpt(Id,2,"{\"n\":2}");
 FVaRestTraceCompleted C;C.RequestId=Id;C.Response.Status=200;C.Response.BodyBytes=7;C.Response.Storage=EVaRestTraceResponseStorage::Memory;Emit(EVaRestTraceKind::Completed,0.,C);
 F.DrainCapture();auto Before=F.Snapshot();
 F.Row(Id,[&T](const auto& R){T.TestTrue(TEXT("one distinct promise still pending after Completed"),!R.bSealed && R.Dispatches.Num()==2 && R.Dispatches[0].ResponseExcerptState==EVaRestInspectorDetailState::Pending);});
 Excerpt(Id,1,"{\"n\":1}");F.DrainCapture();F.Check(Id);
 F.Row(Id,[&T](const auto& R){T.TestTrue(TEXT("both same-attempt promises resolve by dispatch identity"),R.bSealed && R.Dispatches.Num()==2 && R.Dispatches[0].ResponseExcerptState==EVaRestInspectorDetailState::Present && R.Dispatches[1].ResponseExcerptState==EVaRestInspectorDetailState::Present);});
 if(Before) Before->ForEachRecord([&](const auto& R){if(R.RequestId==Id && T.TestEqual(TEXT("retained mixed physical rows"),R.Dispatches.Num(),2)) T.TestTrue(TEXT("retained pre-late excerpt remains immutable"),!R.bSealed && R.Dispatches[0].ResponseExcerptState==EVaRestInspectorDetailState::Pending);});
 // Valid available payload alternatives; ignored before interpreting unavailable socket kinds.
 auto Unknown=FVaRestTraceStarted();Unknown.RequestId=FVaRestTrace::NewRequestId();
 FVaRestTrace::Emit(EVaRestTraceKind::Socket,0.,FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(),Unknown),EVaRestTraceSource::Socket);
 FVaRestTrace::Emit(static_cast<EVaRestTraceKind>(255),0.,FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(),Unknown));F.DrainCapture();
 int32 Ignored=0;F.Service->ForEachRecord([&](const auto& R){if(R.RequestId==Unknown.RequestId)++Ignored;});T.TestEqual(TEXT("socket/unknown produce no HTTP row"),Ignored,0);
 return true;
}
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorCaptureHttpTest,"VaRest.Inspector.Capture.Http",EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorCaptureHttpTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Inspector::Private::CaptureTesting;
 bool Good=true;
 Good=SuccessAndRefusal(*this)&&Good;Good=Retry(*this)&&Good;Good=EnvelopeHttp(*this)&&Good;
 for(bool At:{false,true})Good=Cancellation(*this,At)&&Good;
 for(int32 Mode=0;Mode<5;++Mode)Good=Cache(*this,Mode)&&Good;
 Good=QueueReplay(*this)&&Good;
 for(bool Off:{false,true})Good=LateAttach(*this,Off)&&Good;
 for(bool Reverse:{false,true})Good=SameAttempt(*this,Reverse)&&Good;
 Good=AuthConsumers(*this)&&Good;Good=MixedConsumer(*this)&&Good;
 // Reflected warning/envelope cases are owned by Capture.Decode in VaRestTests.
 return Good;
}
#endif
