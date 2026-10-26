// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorTestUtil.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private::CaptureTesting
{
FVaRestFakeResponse Json(int32 Status, const char* Text)
{
 return FVaRestFakeResponse::Json(Status, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
}
namespace
{
void Text(FAutomationTestBase& T, const FVaRestInspectorPacketView& P, View::EText Field, const FString& Expected)
{
 T.TestEqualSensitive(TEXT("captured text equals redacted source"), FString(P.Text(Field)), Expected);
}
void Headers(FAutomationTestBase& T,const FVaRestInspectorPacketView& P,const FVaRestHeaders& H)
{
 if(!P.Fixed)return;
 const auto Actual=P.Pairs(P.Fixed->Headers);const auto& Expected=H.GetPairs();
 if(T.TestEqual(TEXT("exact redacted header count"),Actual.Num(),Expected.Num()))for(int32 I=0;I<Actual.Num();++I){
  T.TestEqualSensitive(TEXT("header name retained"),FString(P.Read(Actual[I].First)),Expected[I].Key);
  T.TestEqualSensitive(TEXT("header value retained"),FString(P.Read(Actual[I].Second)),Expected[I].Value);
 }
}
void Header(FAutomationTestBase& T, const FVaRestInspectorPacketView& P, const FVaRestTraceEvent& E)
{
 if (!T.TestNotNull(TEXT("source event has retained packet"), P.Fixed)) return;
 T.TestEqual(TEXT("source Seq retained"), P.Fixed->Header.Seq, E.Seq);
 T.TestEqual(TEXT("source seconds retained, including zero"), P.Fixed->Header.Time, E.Time);
 T.TestTrue(TEXT("kind/source retained"), P.Fixed->Header.Kind==E.Kind && P.Fixed->Header.Source==E.Source);
}
}
void CheckCompleted(FAutomationTestBase& T, const FVaRestInspectorRecordView& R, const FVaRestTraceEvent& E)
{
 const auto& C=E.Payload.Get<FVaRestTraceCompleted>();
 Header(T,R.Completed,E);
 if (!R.Completed.Fixed) return;
 const auto& S=R.Completed.Fixed->Scalars;
 T.TestEqual(TEXT("returned request identity retained"), R.RequestId, C.RequestId);
 T.TestEqual(TEXT("delivered final status, not Error.HttpStatus"), R.FinalStatus,C.Response.Status);
 T.TestEqual(TEXT("delivered body bytes are separate from wire"),R.FinalBodyBytes,C.Response.BodyBytes);
 T.TestTrue(TEXT("delivered storage retained"),R.FinalStorage==C.Response.Storage);
 T.TestEqual(TEXT("Completed time is source seconds"),R.CompletedAt,E.Time);
 T.TestEqual(TEXT("decode start forwarded verbatim"),R.DecodeStartAt,C.DecodeStartAt);
 T.TestEqual(TEXT("decode end forwarded verbatim"),R.DecodeEndAt,C.DecodeEndAt);
 T.TestEqual(TEXT("result type name retained"),S.ResultType,C.ResultType);
 T.TestEqual(TEXT("warning count retained"),S.DecodeWarningCount,C.DecodeWarningCount);
 T.TestTrue(TEXT("completion outcome/cache/error retained"),S.Outcome==C.Outcome && S.CacheLookup==C.CacheLookup && S.bFromCache==C.bFromCache && S.ErrorKind==C.Error.Kind && S.ErrorHttpStatus==C.Error.HttpStatus);
 T.TestEqual(TEXT("terminal refresh identity retained"),S.AuthRefreshId,C.AuthRefreshId);
 T.TestTrue(TEXT("envelope applied retained"),S.bEnvelopeApplied==C.Envelope.bApplied);
 Headers(T,R.Completed,C.Response.Headers);
 Text(T,R.Completed,View::EText::EffectiveUrl,C.Response.EffectiveUrl);
 Text(T,R.Completed,View::EText::ResponseContentType,C.Response.ContentType);
 Text(T,R.Completed,View::EText::ErrorCode,C.Error.Code);
 Text(T,R.Completed,View::EText::ErrorMessage,C.Error.Message);
 Text(T,R.Completed,View::EText::DataPath,C.Envelope.DataPath);
 Text(T,R.Completed,View::EText::PredicatePath,C.Envelope.PredicatePath);
 Text(T,R.Completed,View::EText::Expected,C.Envelope.Expected);
 Text(T,R.Completed,View::EText::Actual,C.Envelope.Actual);
 Text(T,R.Completed,View::EText::EnvelopeCode,C.Envelope.Code);
 Text(T,R.Completed,View::EText::EnvelopeMessage,C.Envelope.Message);
 const auto W=R.Completed.Pairs(R.Completed.Fixed->Warnings);
 if (T.TestEqual(TEXT("exact retained warnings"),W.Num(),C.DecodeWarnings.Num()))
  for(int32 I=0;I<W.Num();++I) {
   T.TestEqualSensitive(TEXT("exact warning path"),FString(R.Completed.Read(W[I].First)),C.DecodeWarnings[I].Path);
   T.TestEqualSensitive(TEXT("exact warning message"),FString(R.Completed.Read(W[I].Second)),C.DecodeWarnings[I].Message);
  }
 T.TestTrue(TEXT("real terminal observed"),R.bTerminalKnown && R.bActualCompletionObserved);
}
void FRedactionScan::Text(FStringView Value)
{
 if(Value.IsEmpty()) return;
 FString Copy(Value);
 for(const FString& Needle:Needles)
  if(!Needle.IsEmpty() && Copy.Contains(Needle,ESearchCase::CaseSensitive)) Leaks.Add(Needle);
 Seen.Add(MoveTemp(Copy));
}
bool FRedactionScan::Contains(FStringView Needle) const
{
 const FString Copy(Needle);
 for(const FString& Value:Seen) if(Value.Contains(Copy,ESearchCase::CaseSensitive)) return true;
 return false;
}
void FRedactionScan::Packet(const FVaRestInspectorPacketView& P)
{
 if(!P.Fixed) return;
 ++Packets;
 for(int32 I=0;I<static_cast<int32>(View::EText::Count);++I) { ++TextSlots; Text(P.Text(static_cast<View::EText>(I))); }
 for(const auto& Pair:P.Pairs(P.Fixed->Headers)) { ++HeaderPairs; Text(P.Read(Pair.First)); Text(P.Read(Pair.Second)); }
 for(const auto& Pair:P.Pairs(P.Fixed->Warnings)) { ++WarningPairs; Text(P.Read(Pair.First)); Text(P.Read(Pair.Second)); }
 const auto& S=P.Fixed->Scalars;
 if(P.Fixed->Header.Kind==EVaRestTraceKind::Excerpt) {if(S.bRedacted)++Redacted;if(S.bTruncated)++Truncated;}
 for(FName N:{S.Endpoint,S.Tag,S.CachePolicy,S.FailureDetail,S.ResultType,S.Reason}) Text(N.ToString());
 if(!P.Excerpt.IsEmpty()) {
  ++Excerpts;
  // These are the producer's UTF-8 redacted excerpt/descriptor bytes, never a raw body cast.
  // Also compare byte ranges directly, so malformed UTF-8 cannot hide an ASCII canary.
  for(const FString& Needle:Needles) {
   FTCHARToUTF8 N(*Needle);
   if(N.Length()>0) for(int32 I=0;I+N.Length()<=P.Excerpt.Num();++I)
    if(FMemory::Memcmp(P.Excerpt.GetData()+I,N.Get(),N.Length())==0) { Leaks.Add(Needle); break; }
  }
  FUTF8ToTCHAR U(reinterpret_cast<const ANSICHAR*>(P.Excerpt.GetData()),P.Excerpt.Num());
  Text(FStringView(U.Get(),U.Length()));
 }
}
void FRedactionScan::Record(const FVaRestInspectorRecordView& R)
{
 ++Rows; if(R.Kind==EVaRestInspectorRecordKind::Marker) ++Markers;
 Text(R.ScopeLabel); Text(R.ClientLabel); Text(R.MarkerInfo.QueueId);
 Packet(R.Started); Packet(R.Completed); Packet(R.Marker);
 for(const auto& D:R.Dispatches) { Packet(D.Dispatched); Packet(D.Ended); Packet(D.ResponseExcerpt); }
 for(const auto& P:R.DispatchEvents) Packet(P);
 for(const auto& P:R.Notes) Packet(P);
 for(const auto& P:R.Excerpts) Packet(P);
}
void FRedactionScan::Snapshot(const FVaRestInspectorSnapshot& S)
{
 S.ForEachRecord([this](const auto& R){Record(R);});
 S.ForEachStats([this](const auto& V){++Stats;Text(V.Key);Text(V.Endpoint.ToString());});
 for(const auto& L:S.GetLinks()) {++Links;Text(L.QueueId);}
}

FCaptureFixture::FCaptureFixture(FAutomationTestBase& InTest,bool bPersistence,bool bAutoStart,bool bRecord,const FCapturePolicy* InPolicy):Test(InTest)
{
 using namespace VaRest::Http::Testing;
 if(bPersistence) {
  Disk=MakeUnique<FScopedPersistenceTest>();
  Rig.Client.WithStorageDirectories(FPaths::Combine(Disk->Root,TEXT("Cache")),FPaths::Combine(Disk->Root,TEXT("Queue")));
 }
 Policy.Limits.StoreBytes=16ll*1024*1024; Policy.Limits.MaxRecords=512;
 Policy.Limits.PendingBytes=4ll*1024*1024; Policy.Limits.PendingEvents=4096;
 Policy.Limits.BodyBytes=1024;
 if(InPolicy) Policy=*InPolicy;
 Recorder=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata|EVaRestTraceDetail::Headers|EVaRestTraceDetail::Bodies);
 Recorder->Detail.BodyCap=Policy.Limits.BodyBytes; Recorder->Detail.BinaryCap=Policy.Limits.BinaryBytes; Recorder->Detail.FrameCap=0;
 FServiceTestOptions O; O.Witness=&Witness;
 if (!Test.TestTrue(TEXT("real capture factory admission"),FService::TryCreateForTest(Policy,O,Service))) return;
 Clock=Service->ClockForTest(); Test.TestTrue(TEXT("independent admitted receipt clock"),Clock.IsValid());
 if(bRecord) AddRecorder();
 if(bAutoStart) Start();
}
void FCaptureFixture::AddRecorder()
{
 if(!bRecorderAdded) { FVaRestTrace::AddSink(Recorder.ToSharedRef()); bRecorderAdded=true; }
}
bool FCaptureFixture::Start() { return Ready() && Test.TestTrue(TEXT("real service starts"),Service->Start(Policy)); }
void FCaptureFixture::DrainCapture() { if(Service) for(int32 I=0;I<8;++I) Service->Tick(); }
bool FCaptureFixture::Settle()
{
 using namespace VaRest::Http::Testing;
 const bool B=Test.TestTrue(TEXT("owned HTTP work drains"),Rig.Drain());
 FlushRedactionPipe();
 if(Disk) Test.TestTrue(TEXT("owned persistence drains"),Disk->Drain());
 DrainCapture(); return B;
}
bool FCaptureFixture::Row(uint64 Id,TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor)
{
 if(!Service) return false;
 int32 Hits=0;
 Service->ForEachRecord([Id,&Hits,&Visitor](const FVaRestInspectorRecordView& R) { if(R.Kind==EVaRestInspectorRecordKind::Http && R.RequestId==Id) { ++Hits; Visitor(R); } });
 return Test.TestEqual(TEXT("exactly one real handle row"),Hits,1);
}
bool FCaptureFixture::Check(uint64 Id,bool bPartial)
{
 const auto Source=Recorder->For(Id);
 return Row(Id,[&](const FVaRestInspectorRecordView& R) {
  Test.TestEqual(TEXT("no false partial without loss"),R.bPartial,bPartial);
  Test.TestEqual(TEXT("no omitted dispatches"),R.OmittedDispatches,uint64(0));
  Test.TestEqual(TEXT("no omitted ends"),R.OmittedEnds,uint64(0));
  uint64 Up=0,Down=0; int32 Dispatches=0,Ends=0;
  for(const auto& E:Source) {
   if(E.Kind==EVaRestTraceKind::Started) {
    const auto& P=E.Payload.Get<FVaRestTraceStarted>(); Header(Test,R.Started,E); Headers(Test,R.Started,P.Headers);
    Test.TestEqual(TEXT("client identity"),R.ClientId,P.ClientId); Test.TestEqual(TEXT("scope identity"),R.ScopeId,P.ScopeId);
    Test.TestTrue(TEXT("source zero start remains known"),R.bStartedAtKnown);
    Test.TestEqual(TEXT("source start seconds"),R.StartedAt,E.Time);
    Test.TestEqual(TEXT("UTC known only if supplied"),R.bStartUtcKnown,P.StartUtc.GetTicks()!=0);
    if(R.bStartUtcKnown) Test.TestEqual(TEXT("UTC not monotonic reconstruction"),R.StartUtc.GetTicks(),P.StartUtc.GetTicks());
    Text(Test,R.Started,View::EText::Verb,P.Verb); Text(Test,R.Started,View::EText::Url,P.Url); Text(Test,R.Started,View::EText::PathTemplate,P.PathTemplate);
    Text(Test,R.Started,View::EText::QueueId,P.QueueReplayOf.QueueId);
    if(R.Started.Fixed) Test.TestEqual(TEXT("original queue request"),R.Started.Fixed->Scalars.OriginalRequestId,P.QueueReplayOf.OriginalRequestId);
   } else if(E.Kind==EVaRestTraceKind::AttemptDispatched) {
    ++Dispatches; const auto& P=E.Payload.Get<FVaRestTraceAttemptDispatched>();
    const auto* D=R.Dispatches.FindByPredicate([&P](const FVaRestInspectorDispatchView& V){return V.DispatchId==P.DispatchId;});
    if(!Test.TestNotNull(TEXT("physical dispatch identity retained"),D)) continue;
    Header(Test,D->Dispatched,E); Headers(Test,D->Dispatched,P.Headers);
    Test.TestEqual(TEXT("same attempt preserved"),D->Attempt,P.Attempt);
    Test.TestEqual(TEXT("attempt start seconds"),D->AttemptStart,P.AttemptStart);
    Test.TestEqual(TEXT("dispatch source seconds"),D->DispatchAt,E.Time);
    Test.TestTrue(TEXT("gap retained"),D->GapKind==P.GapKind); Test.TestEqual(TEXT("gap seconds"),D->GapSeconds,P.GapSeconds);
    if(D->Dispatched.Fixed) {
     Test.TestEqual(TEXT("auth wait seconds"),D->Dispatched.Fixed->Scalars.AuthWait,P.AuthWait);
     Test.TestEqual(TEXT("limiter wait seconds"),D->Dispatched.Fixed->Scalars.LimiterWait,P.LimiterWait);
     Test.TestEqual(TEXT("refresh backlink"),D->Dispatched.Fixed->Scalars.AuthRefreshId,P.AuthRefreshId);
    }
   } else if(E.Kind==EVaRestTraceKind::AttemptEnded) {
    ++Ends; const auto& P=E.Payload.Get<FVaRestTraceAttemptEnded>();
    const auto* D=R.Dispatches.FindByPredicate([&P](const FVaRestInspectorDispatchView& V){return V.DispatchId==P.DispatchId;});
    if(!Test.TestNotNull(TEXT("end routed by dispatch identity"),D)) continue;
    Header(Test,D->Ended,E); Headers(Test,D->Ended,P.Headers);
    Test.TestTrue(TEXT("dispatch ended"),D->bHasEnded);
    Test.TestEqual(TEXT("HTTP transport time forwarded"),D->TransportDoneAt,P.TransportDoneAt);
    Test.TestEqual(TEXT("upload forwarded"),D->UploadDoneAt,P.UploadDoneAt); Test.TestEqual(TEXT("status time forwarded"),D->StatusAt,P.StatusAt);
    if(D->Ended.Fixed) Test.TestEqual(TEXT("wire status"),D->Ended.Fixed->Scalars.Status,P.Status);
    if(P.BytesUp>=0) Up+=uint64(P.BytesUp); if(P.BytesDown>=0) Down+=uint64(P.BytesDown);
   } else if(E.Kind==EVaRestTraceKind::Note) {
    const auto& P=E.Payload.Get<FVaRestTraceNote>();
    const auto* N=R.Notes.FindByPredicate([&](const auto& V){return V.Fixed && V.Fixed->Header.Seq==E.Seq;});
    if(!Test.TestNotNull(TEXT("source note retained in order"),N))continue;
    Header(Test,*N,E);
    Test.TestTrue(TEXT("note kind/reason/attempt/bytes retained"),N->Fixed->Scalars.NoteKind==P.Kind && N->Fixed->Scalars.Reason==P.Reason && N->Fixed->Scalars.Attempt==P.Attempt && N->Fixed->Scalars.NoteBytes==P.Bytes);
    Test.TestEqual(TEXT("retry delay seconds"),N->Fixed->Scalars.Delay,P.Delay);Test.TestEqual(TEXT("Retry-After seconds"),N->Fixed->Scalars.RetryAfter,P.RetryAfter);
    Text(Test,*N,View::EText::QueueId,P.QueueId);
   } else if(E.Kind==EVaRestTraceKind::Completed) CheckCompleted(Test,R,E);
   else if(E.Kind==EVaRestTraceKind::Excerpt) {
    const auto& P=E.Payload.Get<FVaRestTraceExcerptEvent>();
    if(P.Owner!=EVaRestTraceExcerptOwner::Response) continue;
    const auto* D=R.Dispatches.FindByPredicate([&P](const FVaRestInspectorDispatchView& V){return V.DispatchId==P.DispatchId;});
    if(!Test.TestNotNull(TEXT("response excerpt resolves physical dispatch"),D)) continue;
    Header(Test,D->ResponseExcerpt,E);
    const TArray<uint8>* Bytes=P.Excerpt.Bytes.Get();
    const int32 N=Bytes?Bytes->Num():0;
    if(Test.TestEqual(TEXT("excerpt byte count"),D->ResponseExcerpt.Excerpt.Num(),N) && N)
     Test.TestTrue(TEXT("exact redacted excerpt bytes"),FMemory::Memcmp(D->ResponseExcerpt.Excerpt.GetData(),Bytes->GetData(),N)==0);
   }
  }
  Test.TestEqual(TEXT("all physical starts counted"),R.ObservedDispatches,uint64(Dispatches));
  Test.TestEqual(TEXT("all physical ends counted"),R.ObservedEnds,uint64(Ends));
  Test.TestEqual(TEXT("only wire bytes up"),R.WireBytesUp,Up); Test.TestEqual(TEXT("only wire bytes down"),R.WireBytesDown,Down);
 });
}
TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> FCaptureFixture::Snapshot()
{
 TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Out;
 Test.TestTrue(TEXT("real immutable snapshot"),Service && Service->TrySnapshot(Out)); if(Out) Retained.Add(Out); return Out;
}
FCaptureFixture::~FCaptureFixture()
{
 using namespace VaRest::Http::Testing;
 for(auto& Release:Releases) Release(); Releases.Reset();
 for(const auto& Handle:Handles) Handle.Cancel();
 Rig.Client.CancelAll();
 Test.TestTrue(TEXT("fixture cancellation settles"),PumpUntil([this]{return Rig.Client.GetInFlightCount()==0;}));
 Settle(); Handles.Reset();
 if(bRecorderAdded) FVaRestTrace::RemoveSink(Recorder.ToSharedRef());
 if(Service) Service->Shutdown();
 Retained.Reset(); Clock.Reset(); Service.Reset(); Recorder.Reset();
 if(Witness.bRootCreated && Test.TestNotNull(TEXT("root stored receipt held by sole top weak"),Witness.Receipt)) {
  FBothLedgerImage Freed;
  Test.TestFalse(TEXT("no charged strong owner after fixture release"),Witness.Root.IsValid());
  if(Test.TestTrue(TEXT("receipt read only after actual root backing free"),Witness.Receipt->ReadFreed(Freed))) {
   Test.TestEqual(TEXT("pending dynamic bytes released"),Freed.Pending.Bytes,int64(0));
   Test.TestEqual(TEXT("pending events released"),Freed.Pending.Count,int64(0));
   Test.TestEqual(TEXT("excerpt reservations released"),Freed.Pending.ExcerptReservations,int64(0));
   Test.TestEqual(TEXT("physical context backing freed"),Freed.ContextProbe.LiveBacking,int64(0));
   Test.TestEqual(TEXT("physical store backing freed"),Freed.StoreProbe.LiveBacking,int64(0));
   Test.TestEqual(TEXT("all snapshot controllers retired"),Freed.SnapshotControllers,int64(0));
   Test.TestEqual(TEXT("all store controllers retired"),Freed.StoreProbe.LiveControllers,int64(0));
   Test.TestEqual(TEXT("only top weak controller remains"),Freed.ContextProbe.LiveControllers,int64(1));
   Test.TestEqual(TEXT("all root manual payloads destroyed"),Freed.ConstructedSlotMask,uint16(0));
   Test.TestTrue(TEXT("only root-controller claim remains"),Freed.Claims[0].IsEmpty() && Freed.Claims[1].Reserved==4096);
   Test.TestEqual(TEXT("sole weak root controller pending credit"),Freed.PendingTotal,int64(4096));
   Test.TestEqual(TEXT("sole weak root controller store credit"),Freed.StoreTotal,int64(4096));
  }
 }
 Witness.Receipt=nullptr; Witness.Root.Reset(); // Final stored deleter enforces its zero equation.
}
}
#endif
