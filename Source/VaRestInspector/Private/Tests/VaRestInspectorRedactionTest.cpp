// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorTestUtil.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "VaRestAuth.h"
#include "VaRestBody.h"
#include "VaRestCache.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpSettings.h"
#include "VaRestOffline.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR && VAREST_WITH_TRACE_EXCERPTS
namespace VaRest::Inspector::Private::CaptureTesting
{
namespace
{
using namespace VaRest::Http::Testing;
struct FPrivacyVerdicts
{
 bool Header=true, Number=true, Substring=true, Known=true, Replay=true;
};
FCapturePolicy Policy()
{
 FCapturePolicy P; P.Limits.StoreBytes=16ll*1024*1024; P.Limits.MaxRecords=512;
 P.Limits.PendingBytes=4ll*1024*1024; P.Limits.PendingEvents=4096; P.Limits.BodyBytes=1024;
 return P;
}
void Emit(EVaRestTraceKind Kind,FVaRestTracePayload&& Payload)
{
 FVaRestTrace::Emit(Kind,0.0,MoveTemp(Payload));
}
template<class P> void Emit(EVaRestTraceKind Kind,P Payload)
{
 Emit(Kind,FVaRestTracePayload(TInPlaceType<P>(),MoveTemp(Payload)));
}
void ScanRetained(FCaptureFixture& F,FRedactionScan& S)
{
 F.DrainCapture();
 F.Service->ForEachRecord([&](const auto& R){S.Record(R);});
 auto Held=F.Snapshot(); if(!Held) return;
 const uint64 Revision=Held->GetRevision();
 S.Snapshot(*Held);
 // Read a new current snapshot and then the old immutable one after live destruction.
 auto Fresh=F.Snapshot(); if(Fresh) S.Snapshot(*Fresh);
 F.Service->Clear(); S.Snapshot(*Held);
 F.Test.TestEqual(TEXT("Task13 held snapshot revision survives live Clear"),Held->GetRevision(),Revision);
 F.Test.TestTrue(TEXT("Task13 nonempty live and retained packet inventory"),S.Rows>0 && S.Packets>0);
 F.Test.TestEqual(TEXT("Task13 every declared text slot visited"),S.TextSlots,S.Packets*static_cast<int32>(View::EText::Count));
 F.Test.TestTrue(TEXT("Task13 nonempty real stats grouping"),S.Stats>0);
}
template<class TResult> bool Send(FCaptureFixture& F,TVaRestRequest<TResult>&& Request,FVaRestFakeResponse Response,uint64* Id=nullptr)
{
 F.Rig.Fake->Enqueue(MoveTemp(Response));
 auto O=MakeOutcome<TResult>(); auto H=F.Track(Observe(MoveTemp(Request),O).Send());
 if(Id) *Id=H.GetId();
 if(!F.Test.TestTrue(TEXT("Task13 real public request completes"),F.Rig.Wait(O))) return false;
 return F.Settle();
}
struct FDefaultKey
{
 TArray<FString> Previous;
 FDefaultKey() {
  auto* S=GetMutableDefault<UVaRestHttpSettings>(); Previous=S->RedactedKeys;
  S->RedactedKeys.AddUnique(TEXT("projectVault"));
 }
 ~FDefaultKey(){GetMutableDefault<UVaRestHttpSettings>()->RedactedKeys=MoveTemp(Previous);}
};
bool HttpMatrix(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FDefaultKey Default; // Restored after the fixture and its client are destroyed.
 FCaptureFixture F(T); if(!F.Ready()) return false;
 const FString Known=TEXT("Known-Path-Canary-13"), Encoded=TEXT("Odd+Path/Canary==13");
 F.Rig.Client.SetSecretValue(TEXT("known"),Known); F.Rig.Client.SetSecretValue(TEXT("encoded"),Encoded);
 F.Rig.Client.WithName(TEXT("Inspector benign client")).WithLogLevel(EVaRestLogLevel::None);
 TArray<FString> Needles={Known,Encoded,TEXT("Odd%2BPath%2FCanary%3D%3D13"),TEXT("Bearer-Canary-13"),TEXT("Cookie-Canary-13"),
  TEXT("Query-Canary-13"),TEXT("Request-Canary-13"),TEXT("Response-Canary-13"),TEXT("Form-Canary-13"),TEXT("Part-Canary-13"),TEXT("Project-Canary-13")};
 auto Reply=FVaRestFakeResponse::Text(200,TEXT("{\"password\":\"Response-Canary-13\",\"ok\":\"benign-response\"}"),TEXT("application/json"));
 Reply.Headers.Set(TEXT("Set-Cookie"),TEXT("sid=Cookie-Canary-13")); Reply.Headers.Set(TEXT("X-Visible"),TEXT("benign-header"));
 Reply.EffectiveUrl=TEXT("https://api.test/effective/")+Known+TEXT("?token=Query-Canary-13&visible=benign-query");
 uint64 Main=0;
 if(!Send(F,F.Rig.Client.Post<void>(TEXT("/path/{part}")).Path(TEXT("part"),Known)
  .Query(TEXT("token"),TEXT("Query-Canary-13")).Query(TEXT("visible"),TEXT("benign-query"))
  .Header(TEXT("Authorization"),TEXT("Bearer Bearer-Canary-13")).Header(TEXT("Cookie"),TEXT("sid=Cookie-Canary-13"))
  .Body(FVaRestJson::Object().Set(TEXT("password"),TEXT("Request-Canary-13")).Set(TEXT("ok"),TEXT("benign-request"))),
  MoveTemp(Reply),&Main)) return false;
 // Both actual wire and retained metadata have positive nonempty controls.
 F.Row(Main,[&](const auto& R){T.TestTrue(TEXT("Task13 real matrix status and physical history"),R.FinalStatus==200 && R.Dispatches.Num()==1 && R.bActualCompletionObserved);});
 if(!Send(F,F.Rig.Client.Get<void>(TEXT("/encoded/{part}")).Path(TEXT("part"),Encoded),Json(200))) return false;
 if(!Send(F,F.Rig.Client.Post<void>(TEXT("/form")).Body(FVaRestForm().Add(TEXT("password"),TEXT("Form-Canary-13")).Add(TEXT("name"),TEXT("benign-form"))),Json(200))) return false;
 if(!Send(F,F.Rig.Client.Post<void>(TEXT("/parts")).Body(FVaRestMultipart().AddField(TEXT("password"),TEXT("Part-Canary-13")).AddField(TEXT("name"),TEXT("benign-part"))),Json(200))) return false;
 if(!Send(F,F.Rig.Client.Get<void>(TEXT("/default-rule")),
  FVaRestFakeResponse::Text(200,TEXT("{\"projectVault\":\"Project-Canary-13\",\"ok\":\"benign-default\"}"),TEXT("application/json")))) return false;
 // A real failed envelope echoes a known value through error/envelope text.
 FVaRestEnvelope Envelope=FVaRestEnvelope::DataAt(TEXT("Known-Path-Canary-13"))
  .SuccessWhen(TEXT("Known-Path-Canary-13"),{TEXT("Known-Path-Canary-13")}).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"));
 F.Rig.Client.WithEnvelope(Envelope);
 uint64 Error=0;
 if(!Send(F,F.Rig.Client.Get(TEXT("/envelope")),FVaRestFakeResponse::Text(200,
  TEXT("{\"Known-Path-Canary-13\":\"wrong\",\"code\":\"Known-Path-Canary-13\",\"message\":\"benign-error Known-Path-Canary-13\"}"),TEXT("application/json")),&Error)) return false;
 F.Row(Error,[&](const auto& R){T.TestTrue(TEXT("Task13 envelope is a real server failure"),R.EndState==EVaRestInspectorEndState::Error && R.Completed.Fixed && R.Completed.Fixed->Scalars.bEnvelopeApplied);});
 FRedactionScan S(Needles); ScanRetained(F,S);
 bool OtherLeaks=false;
 for(const FString& Needle:Needles) if(Needle!=TEXT("Cookie-Canary-13")) OtherLeaks|=S.Leaked(Needle);
 // R11 changes the common matcher for headers too: Set-Cookie's substring protection joins that verdict.
 V.Substring &= !S.Leaked(TEXT("Cookie-Canary-13"));
 V.Known &= !OtherLeaks && S.Contains(TEXT("***")) && S.Contains(TEXT("benign-request")) && S.Contains(TEXT("benign-response"))
  && S.Contains(TEXT("benign-query")) && S.Contains(TEXT("benign-header")) && S.Contains(TEXT("benign-form")) && S.Contains(TEXT("benign-part")) && S.Contains(TEXT("benign-default"));
 T.TestTrue(TEXT("Task13 request and response previews and headers really retained"),S.Excerpts>0 && S.HeaderPairs>0 && S.Redacted>0);
 return true;
}
bool HeaderOracle(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FCaptureFixture F(T); if(!F.Ready()) return false;
 // Deliberately not a known value: only the configured sensitive header key protects it.
 const TArray<FString> Keys={TEXT("X-Inspector-Vault")}; F.Rig.Client.WithRedactedKeys(Keys);
 auto Reply=FVaRestFakeResponse::Text(200,TEXT("benign-r1")); Reply.Headers.Set(TEXT("X-Inspector-Vault"),TEXT("Header-Canary-R1"));
 if(!Send(F,F.Rig.Client.Get<void>(TEXT("/r1")),MoveTemp(Reply))) return false;
 FRedactionScan S({TEXT("Header-Canary-R1")}); ScanRetained(F,S);
 V.Header &= !S.Leaked() && S.Contains(TEXT("***")) && S.HeaderPairs>0;
 T.TestTrue(TEXT("Task13 R1 benign response retained"),S.Contains(TEXT("benign-r1")));
 return true;
}
bool SubstringOracle(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FCaptureFixture F(T); if(!F.Ready()) return false;
 // Neither value is registered or also known as auth/session material.
 if(!Send(F,F.Rig.Client.Get<void>(TEXT("/r11")),FVaRestFakeResponse::Text(200,
  TEXT("{\"refreshToken\":\"Camel-Canary-R11\",\"REFRESH_token\":\"Case-Canary-R11\",\"name\":\"benign-r11\"}"),TEXT("application/json")))) return false;
 FRedactionScan S({TEXT("Camel-Canary-R11"),TEXT("Case-Canary-R11")}); ScanRetained(F,S);
 V.Substring &= !S.Leaked() && S.Redacted>0 && S.Contains(TEXT("\"refreshToken\":\"***\"")) && S.Contains(TEXT("\"REFRESH_token\":\"***\""));
 T.TestTrue(TEXT("Task13 R11 benign nonsecret property retained"),S.Contains(TEXT("benign-r11")));
 return true;
}
bool PathOracle(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FCaptureFixture F(T); if(!F.Ready()) return false;
 F.Rig.Client.SetSecretValue(TEXT("path"),TEXT("Path-Canary-R13"));
 if(!Send(F,F.Rig.Client.Get<void>(TEXT("/ordinary/Path-Canary-R13/visible")),FVaRestFakeResponse::Text(200,TEXT("benign-r13")))) return false;
 FRedactionScan S({TEXT("Path-Canary-R13")}); ScanRetained(F,S);
 V.Known &= !S.Leaked() && S.Contains(TEXT("/ordinary/***/visible"));
 T.TestTrue(TEXT("Task13 R13 benign route and response retained"),S.Contains(TEXT("ordinary")) && S.Contains(TEXT("benign-r13")));
 return true;
}
bool NumberAndMalformed(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 const FString Prefix=TEXT("{\"ok\":7,\"n\":"); FTCHARToUTF8 P(*Prefix);
 auto C=Policy(); C.Limits.BodyBytes=P.Length()+8;
 {
  FCaptureFixture F(T,false,true,true,&C); if(!F.Ready()) return false;
  if(!Send(F,F.Rig.Client.Get<void>(TEXT("/r2")),FVaRestFakeResponse::Text(200,
   Prefix+TEXT("12345678901234567890}"),TEXT("application/json")))) return false;
  FRedactionScan S({TEXT("12345678")}); bool Truncated=false;
  F.Service->ForEachRecord([&](const auto& R){for(const auto& E:R.Excerpts) if(E.Fixed) Truncated|=E.Fixed->Scalars.bTruncated;});
  ScanRetained(F,S); V.Number &= !S.Leaked();
  T.TestTrue(TEXT("Task13 R2 cap and benign prefix are real"),Truncated && S.Contains(TEXT("\"ok\":7")) && S.Contains(TEXT("cap")));
 }
 {
  FCaptureFixture F(T); if(!F.Ready()) return false;
  if(!Send(F,F.Rig.Client.Get<void>(TEXT("/malformed")),FVaRestFakeResponse::Text(200,
   TEXT("{\"password\":\"Malformed-Canary-13\",\"ok\":\"benign-malformed\", broken"),TEXT("application/json")))) return false;
  FRedactionScan S({TEXT("Malformed-Canary-13")}); ScanRetained(F,S);
  V.Known &= !S.Leaked() && S.Contains(TEXT("***"));
  T.TestTrue(TEXT("Task13 malformed prefix has explicit unavailable tail"),S.Contains(TEXT("parse error")) && S.Contains(TEXT("benign-malformed")));
 }
 {
  const FString CutJsonPrefix=TEXT("{\"ok\":\"benign-cut\",\"password\":\"");
  FTCHARToUTF8 CutPrefix(*CutJsonPrefix); auto Cut=Policy(); Cut.Limits.BodyBytes=CutPrefix.Length()+8;
  FCaptureFixture F(T,false,true,true,&Cut); if(!F.Ready()) return false;
  if(!Send(F,F.Rig.Client.Get<void>(TEXT("/truncated-json")),FVaRestFakeResponse::Text(200,
   CutJsonPrefix+TEXT("Cut-Json-Canary-13\"}"),TEXT("application/json")))) return false;
  FRedactionScan S({TEXT("Cut-Json"),TEXT("Cut-Json-Canary-13")}); ScanRetained(F,S);
  V.Known &= !S.Leaked() && S.Contains(TEXT("***"));
  T.TestTrue(TEXT("Task13 truncated JSON retains benign prefix and explicit cap"),S.Contains(TEXT("benign-cut")) && S.Contains(TEXT("cap")));
 }
 return true;
}

bool UnionRotation(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FDefaultKey Default; FCaptureFixture F(T); if(!F.Ready()) return false;
 const FString Source=TEXT("Source-Canary-13"), Current=TEXT("Current-Canary-13"), Applied=TEXT("Applied-Canary-13");
 F.Rig.Client.WithMaxConcurrent(1).WithAuth(FVaRestAuth::Bearer());
 F.Rig.Client.SetSecretValue(TEXT("union"),Source);
 F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
 auto Holder=MakeOutcome<void>(); F.Track(Observe(F.Rig.Client.Get<void>(TEXT("/holder")).NoAuth(),Holder).Send());
 if(!T.TestTrue(TEXT("Task13 public limiter holder occupies real transport"),PumpUntil([&]{return F.Rig.Fake->NumCalls()==1;}))) return false;
 F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual());
 auto O=MakeOutcome<FVaRestJson>(); const auto H=F.Track(Observe(F.Rig.Client.Get(TEXT("/union")),O).Send());
 T.TestEqual(TEXT("Task13 target still waits behind public holder"),F.Rig.Fake->NumCalls(),1);
 // Applied is absent at Send and current is added after Send; source stays only in the held send snapshot.
 F.Rig.Client.ClearSecretValues(); F.Rig.Client.SetSecretValue(TEXT("union"),Current); F.Rig.Client.SetBearerToken(Applied);
 F.Rig.Fake->Complete(0,Json(200));
 if(!T.TestTrue(TEXT("Task13 late Apply reaches actual target wire"),PumpUntil([&]{return F.Rig.Fake->NumCalls()==2;}))) return false;
 T.TestEqualSensitive(TEXT("Task13 wire really used late applied bearer"),F.Rig.Fake->GetCall(1).Request.Headers.Get(TEXT("Authorization")),TEXT("Bearer ")+Applied);
 F.Rig.Client.ClearSession(); // The in-flight request must keep what Apply used.
 F.Rig.Fake->Complete(1,FVaRestFakeResponse::Text(200,
  TEXT("{\"note\":\"")+Source+TEXT(" ")+Current+TEXT(" ")+Applied+TEXT("\",\"projectVault\":\"Project-Union-Canary\",\"ok\":\"benign-union\"}"),TEXT("application/json")));
 if(!T.TestTrue(TEXT("Task13 union response and holder complete"),F.Rig.Wait(O) && F.Rig.Wait(Holder))) return false;
 F.Settle();
 T.TestTrue(TEXT("Task13 decoder sees real unredacted entity"),O->Value.IsSet() && O->Value->Get(TEXT("note")).AsString().Contains(Applied));
 FRedactionScan S({Source,Current,Applied,TEXT("Project-Union-Canary")}); ScanRetained(F,S);
 V.Known &= !S.Leaked() && S.Contains(TEXT("***")) && S.Contains(TEXT("benign-union"));
 return true;
}
bool ValidatedConsumer(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 // Consumer retention fixture only. The real typed warning/config producer coverage is owning accepted Task12 Decode
 // and HTTP Golden.EnvelopeConfigRedaction. No redactor runs here; these exact already-redacted shapes are intentional.
 FCaptureFixture F(T); if(!F.Ready()) return false;
 const uint64 Id=FVaRestTrace::NewRequestId();
 FVaRestTraceStarted A; A.RequestId=Id; A.ClientId=F.Rig.Client.GetId(); A.Verb=TEXT("GET"); A.Url=TEXT("https://api.test/validated-consumer"); A.PathTemplate=TEXT("/validated-consumer");
 Emit(EVaRestTraceKind::Started,MoveTemp(A));
 FVaRestTraceCompleted C; C.RequestId=Id; C.Response.Status=200; C.Outcome=EVaRestTraceOutcome::Error;
 C.Error.Kind=EVaRestErrorKind::Server; C.Error.Message=TEXT("benign-consumer ***");
 C.Envelope.bApplied=true; C.Envelope.DataPath=TEXT("***"); C.Envelope.PredicatePath=TEXT("***"); C.Envelope.Expected=TEXT("***");
 C.Envelope.Actual=TEXT("bad"); C.Envelope.Code=TEXT("BUSINESS"); C.Envelope.Message=TEXT("benign-envelope ***");
 C.DecodeWarningCount=1; C.DecodeWarnings.Add({TEXT("***"),TEXT("an integer was read from a string")});
 Emit(EVaRestTraceKind::Completed,MoveTemp(C)); F.DrainCapture();
 F.Row(Id,[&](const auto& R){
  T.TestTrue(TEXT("Task13 validated consumer fields retain exact redacted shape"),
   R.Completed.Text(View::EText::DataPath)==TEXT("***") && R.Completed.Text(View::EText::PredicatePath)==TEXT("***") && R.Completed.Text(View::EText::Expected)==TEXT("***")
   && R.Completed.Text(View::EText::Actual)==TEXT("bad") && R.Completed.Text(View::EText::EnvelopeCode)==TEXT("BUSINESS"));
 });
 FRedactionScan S({TEXT("items[1].level"),TEXT("Consumer-Forbidden-Canary")}); ScanRetained(F,S);
 V.Known &= !S.Leaked() && S.WarningPairs>0 && S.Contains(TEXT("an integer was read from a string")) && S.Contains(TEXT("benign-envelope"));
 return true;
}
bool DetailIsolation(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 for(int32 Mode=0;Mode<2;++Mode) {
  auto P=Policy(); P.bHeaders=Mode!=0; P.bBodies=Mode!=1;
  FCaptureFixture F(T,false,true,true,&P); if(!F.Ready()) return false;
  F.Rig.Client.SetSecretValue(TEXT("option"),TEXT("Option-Canary-13"));
  auto Reply=FVaRestFakeResponse::Text(200,TEXT("Option-Canary-13 benign-option")); Reply.Headers.Set(TEXT("X-Only-Other-Sink"),TEXT("benign-option-header"));
  uint64 Id=0;
  if(!Send(F,F.Rig.Client.Post<void>(TEXT("/option")).Header(TEXT("X-Only-Other-Sink"),TEXT("benign-option-header")).Body(TEXT("Option-Canary-13 benign-request")),MoveTemp(Reply),&Id)) return false;
  const auto Events=F.Recorder->For(Id);
  bool OtherHeaders=false,OtherBodies=false;
  for(const auto& E:Events) {
   if(E.Kind==EVaRestTraceKind::AttemptEnded) OtherHeaders|=!E.Payload.Get<FVaRestTraceAttemptEnded>().Headers.Get(TEXT("X-Only-Other-Sink")).IsEmpty();
   if(E.Kind==EVaRestTraceKind::Excerpt) OtherBodies|=E.Payload.Get<FVaRestTraceExcerptEvent>().Excerpt.Bytes.IsValid();
  }
  T.TestTrue(TEXT("Task13 unrelated sink still receives requested detail"),OtherHeaders && OtherBodies);
  FRedactionScan S({TEXT("Option-Canary-13")}); ScanRetained(F,S);
  V.Known &= !S.Leaked();
  if(Mode==0) T.TestEqual(TEXT("Task13 Headers=false retains no header names or values"),S.HeaderPairs,0);
  else T.TestEqual(TEXT("Task13 Bodies=false retains no excerpt bytes"),S.Excerpts,0);
  T.TestTrue(TEXT("Task13 disabled detail still retains benign URL"),S.Contains(TEXT("/option")));
 }
 return true;
}
bool BinaryDescriptors(FAutomationTestBase& T)
{
 for(int32 BinaryCap:{0,1024}) {
  auto P=Policy(); P.Limits.BinaryBytes=BinaryCap;
  FCaptureFixture F(T,false,true,true,&P); if(!F.Ready()) return false;
  const FString Raw=TEXT("Raw-Binary-Canary-13"); FTCHARToUTF8 U(*Raw);
  // Explicit binary capture is now implemented; its known-value scrub applies before bytes reach either sink.
  if(BinaryCap>0) F.Rig.Client.SetSecretValue(TEXT("binary-canary"),Raw);
  TArray<uint8> Small; Small.Add(0x89); Small.Append(reinterpret_cast<const uint8*>(U.Get()),U.Length());
  TArray<uint8> Large=Small; Large.SetNumZeroed(2048);
  int64 Copies[2]={-1,-1}; uint64 Ids[2]={0,0};
  for(int32 I=0;I<2;++I) {
   const auto& Body=I?Small:Large;
   F.Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200,Body)); auto O=MakeOutcome<TArray<uint8>>();
   const int64 Before=Counters().TraceExcerptBytesCopied.load();
   const auto H=F.Track(Observe(F.Rig.Client.Get<TArray<uint8>>(I?TEXT("/binary-small"):TEXT("/binary-large")),O).Send()); Ids[I]=H.GetId();
   if(!T.TestTrue(TEXT("Task13 binary request completes"),F.Rig.Wait(O) && F.Settle())) return false;
   Copies[I]=Counters().TraceExcerptBytesCopied.load()-Before;
   T.TestTrue(TEXT("Task13 actual decoder still owns raw binary"),O->Value.IsSet() && O->Value->Num()==Body.Num() && (*O->Value)[0]==0x89);
   const auto Excerpts=F.Recorder->ExcerptsOf(H.GetId()); int32 Responses=0;
   for(const auto& E:Excerpts) if(E.Owner==EVaRestTraceExcerptOwner::Response) {
    ++Responses; T.TestTrue(TEXT("binary producer preserves default description or explicit capture"),E.Excerpt.Kind==EVaRestExcerptKind::Binary
     && E.Excerpt.bBinaryCaptured==(BinaryCap>0) && E.Excerpt.TotalBytes==Body.Num() && !E.Excerpt.ToText().Contains(Raw));
    if(BinaryCap==0) T.TestTrue(TEXT("default binary is still a descriptor"),E.Excerpt.ToText().StartsWith(TEXT("<binary: ")));
    else T.TestTrue(TEXT("opted-in binary scrubs the known value before publication"),E.Excerpt.bRedacted && E.Excerpt.Bytes && E.Excerpt.Bytes->Num()<=BinaryCap
     && E.Excerpt.Bytes->Num()>=4 && (*E.Excerpt.Bytes)[0]==0x89 && (*E.Excerpt.Bytes)[1]=='*' && (*E.Excerpt.Bytes)[2]=='*' && (*E.Excerpt.Bytes)[3]=='*');
    T.TestEqual(TEXT("Task13 hash only for complete within-cap body"),!E.Excerpt.Sha256Prefix.IsEmpty(),I==1);
   }
   T.TestEqual(TEXT("Task13 binary response has one descriptor"),Responses,1);
  }
  T.TestEqual(TEXT("default oversized binary copies nothing; opt-in copies only its cap"),Copies[0],int64(BinaryCap));
  T.TestEqual(TEXT("Task13 within BodyCap binary permitted hash copy"),Copies[1],int64(Small.Num()));
  FRedactionScan S({Raw}); ScanRetained(F,S);
  T.TestTrue(TEXT("binary policy retains no known secret in live or immutable capture"),!S.Leaked() && S.Excerpts>0
   && (BinaryCap>0 ? S.Contains(TEXT("***")) : S.Contains(TEXT("application/octet-stream")) && S.Contains(TEXT("sha256"))));
 }
 return true;
}
bool StreamAndCache(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FCaptureFixture F(T,true); if(!F.Ready()) return false;
 const FString File=FPaths::Combine(F.Disk->Root,TEXT("stream-body.bin"));
 if(!T.TestTrue(TEXT("Task13 owned stream fixture written"),FFileHelper::SaveStringToFile(TEXT("Stream-Canary-13"),*File,FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))) return false;
 uint64 StreamId=0;
 if(!Send(F,F.Rig.Client.Post<void>(TEXT("/stream")).BodyFile(File),Json(200),&StreamId)) return false;
 bool Stream=false;
 for(const auto& E:F.Recorder->ExcerptsOf(StreamId)) if(E.Owner==EVaRestTraceExcerptOwner::Request)
  Stream|=E.Excerpt.Kind==EVaRestExcerptKind::Stream && E.Excerpt.TotalBytes==16 && !E.Excerpt.ToText().Contains(TEXT("Stream-Canary-13"));
 T.TestTrue(TEXT("Task13 streamed upload preview unavailable but descriptor present"),Stream);
 auto Cache=FVaRestCachePolicy::Memory(); Cache.DefaultTtlSeconds=60; F.Rig.Client.WithCache(Cache);
 auto Reply=FVaRestFakeResponse::Text(200,TEXT("benign-cache")); Reply.Headers.Set(TEXT("Cache-Control"),TEXT("max-age=60"));
 if(!Send(F,F.Rig.Client.Get<FString>(TEXT("/cache")),MoveTemp(Reply))) return false;
 auto O=MakeOutcome<FString>(); const auto H=F.Track(Observe(F.Rig.Client.Get<FString>(TEXT("/cache")),O).Send());
 if(!T.TestTrue(TEXT("Task13 cached response delivered"),F.Rig.Wait(O) && F.Settle())) return false;
 F.Row(H.GetId(),[&](const auto& R){T.TestTrue(TEXT("Task13 cached final remains NoProducer"),R.FinalPreviewState==EVaRestInspectorDetailState::NoProducer && R.Dispatches.IsEmpty() && R.FinalStatus==200);});
 FRedactionScan S({TEXT("Stream-Canary-13")}); ScanRetained(F,S); V.Known &= !S.Leaked();
 T.TestTrue(TEXT("Task13 stream/cache metadata and benign cache retained"),S.Contains(TEXT("/stream")) && S.Contains(TEXT("benign-cache")));
 return true;
}

FVaRestTraceEvent BorrowedStarted(FString Url)
{
 FVaRestTraceStarted S; S.RequestId=FVaRestTrace::NewRequestId(); S.Url=MoveTemp(Url);
 FVaRestTraceEvent E; E.Kind=EVaRestTraceKind::Started; E.Payload=FVaRestTracePayload(TInPlaceType<FVaRestTraceStarted>(),MoveTemp(S)); return E;
}
void EmptyQueue(FCaptureQueue& Q)
{
 for(;;) {auto P=Q.TryPopMetadata();if(!P.IsSet())break;}
 for(;;) {auto P=Q.TryPopExcerpt();if(!P.IsSet())break;}
}
bool Admission(FAutomationTestBase& T)
{
 for(int32 Mode=0;Mode<2;++Mode) {
  auto P=Policy(); if(Mode==0) P.Limits.PendingEvents=8;
  FCaptureFixture F(T,false,true,true,&P); if(!F.Ready()) return false;
  F.DrainCapture(); auto Q=F.Service->QueueForTest(); if(!T.TestTrue(TEXT("Task13 admitted actual active queue"),Q.IsValid())) return false;
  EmptyQueue(*Q);
  auto Bytes=MakeShared<TArray<uint8>,ESPMode::ThreadSafe>(); Bytes->SetNumZeroed(8*1024*1024);
  FVaRestTraceExcerptEvent X; X.RequestId=FVaRestTrace::NewRequestId(); X.Excerpt.Bytes=Bytes; X.Excerpt.StopReason=TEXT("borrowed-must-not-copy");
  FVaRestTraceEvent E; E.Kind=EVaRestTraceKind::Excerpt; E.Payload=FVaRestTracePayload(TInPlaceType<FVaRestTraceExcerptEvent>(),MoveTemp(X));
  // This upstream test owner is larger than the admitted pending budget: the full owner cannot fit.
  const auto BeforeFallback=F.Service->TestImage(); const auto Fallback=Q->TryPush(E); const auto AfterFallback=F.Service->TestImage();
  T.TestTrue(TEXT("Task13 oversized excerpt uses scalar fallback"),Fallback==EPush::ScalarOnly);
  T.TestEqual(TEXT("Task13 scalar fallback acquired no excerpt owner"),AfterFallback.ExcerptRefs,BeforeFallback.ExcerptRefs);
  T.TestEqual(TEXT("Task13 scalar fallback copied no borrowed text"),AfterFallback.TextCopies,BeforeFallback.TextCopies);
  EmptyQueue(*Q);
  const FString Fill=Mode==0?TEXT("count-slack"):FString::ChrN(8192,TEXT('b'));
  int32 Accepted=0; EPush Last=EPush::Accepted;
  for(int32 I=0;I<=P.Limits.PendingEvents;++I) {
   Last=Q->TryPush(BorrowedStarted(Fill)); if(Last!=EPush::Accepted) break; ++Accepted;
  }
  T.TestTrue(TEXT("Task13 actual pressure reached after positive admissions"),Accepted>0 && Last==EPush::Dropped);
  if(Mode==0) {
   T.TestEqual(TEXT("Task13 count admission saturates exact count with byte slack"),Accepted,P.Limits.PendingEvents);
   T.TestTrue(TEXT("Task13 count pressure leaves actual admitted byte headroom"),F.Service->TestImage().PendingTotal<F.Service->TestImage().EffectivePending);
  }
  else {
   T.TestTrue(TEXT("Task13 byte admission refused with count slack"),Accepted<P.Limits.PendingEvents);
   // Fill the remaining byte tail with minimum metadata packets, still without ticking the store.
   for(int32 I=0;I<=P.Limits.PendingEvents;++I) {
    Last=Q->TryPush(BorrowedStarted(FString())); if(Last!=EPush::Accepted) break; ++Accepted;
   }
   T.TestTrue(TEXT("Task13 byte tail exhausted before count saturation"),Last==EPush::Dropped && Accepted<P.Limits.PendingEvents);
  }
  const int32 SourceRefs=Bytes.GetSharedReferenceCount();
  const auto Before=F.Service->TestImage(); const auto Denied=Q->TryPush(E); const auto After=F.Service->TestImage();
  T.TestTrue(TEXT("Task13 pressure actually denies even scalar excerpt admission"),Denied==EPush::Dropped);
  T.TestEqual(TEXT("Task13 denied admission copies no text"),After.TextCopies,Before.TextCopies);
  T.TestEqual(TEXT("Task13 denied admission acquires no excerpt reference"),After.ExcerptRefs,Before.ExcerptRefs);
  T.TestEqual(TEXT("Task13 denied admission preserves source owner refs"),Bytes.GetSharedReferenceCount(),SourceRefs);
  EmptyQueue(*Q); Q.Reset();
 }
 {
  FCaptureFixture F(T); if(!F.Ready()) return false;
  F.DrainCapture(); auto Q=F.Service->QueueForTest(); if(!Q) return false; EmptyQueue(*Q);
  const FString Raw=FString::ChrN(FNormalizedLimits::MaxStringChars+1,TEXT('q'))+TEXT("Long-Metadata-Canary-13");
  const auto E=BorrowedStarted(Raw); const auto Before=F.Service->TestImage(); const auto Result=Q->TryPush(E); const auto After=F.Service->TestImage();
  T.TestTrue(TEXT("Task13 long metadata has truthful scalar omission"),Result==EPush::ScalarOnly);
  auto Packet=Q->TryPopMetadata();
  if(!T.TestTrue(TEXT("Task13 scalar fallback is an actual packet"),Packet.IsSet())) return false;
  bool Empty=Packet->Fixed().Header.bMetadataOmitted && !Packet->Excerpt().IsValid() && Packet->Headers().IsEmpty() && Packet->Warnings().IsEmpty();
  for(int32 I=0;I<static_cast<int32>(View::EText::Count);++I) Empty &= Packet->Text(static_cast<View::EText>(I)).IsEmpty();
  T.TestTrue(TEXT("Task13 no truncated raw secret in scalar metadata"),Empty && Packet->Fixed().Scalars.RequestId==E.Payload.Get<FVaRestTraceStarted>().RequestId);
  T.TestEqual(TEXT("Task13 long metadata fallback copies zero text"),After.TextCopies,Before.TextCopies);
  T.TestEqual(TEXT("Task13 long metadata fallback takes zero excerpt refs"),After.ExcerptRefs,Before.ExcerptRefs);
  Packet.Reset(); Q.Reset();
 }
 return true;
}
bool CoreOnlyRedactor(FAutomationTestBase& T)
{
 FCaptureFixture F(T); if(!F.Ready()) return false;
 F.Rig.Client.SetSecretValue(TEXT("counter"),TEXT("Counter-Canary-13")); F.Rig.Client.WithLogLevel(EVaRestLogLevel::None);
 uint64 Delta[2]={0,0};
 for(int32 On=0;On<2;++On) {
  if(On) {if(!F.Start()) return false;} else F.Service->Pause();
  F.Settle(); // Stable always-active recorder keeps exactly the same merged mask/caps.
  const auto Detail=FVaRestTrace::GetDetailSettings();
  T.TestEqual(TEXT("Task13 core body cap fixed across Inspector OFF/ON"),Detail.BodyCap,1024);
  const uint64 Before=FVaRestTrace::Counters().RedactorCalls.load();
  uint64 Id=0;
  if(!Send(F,F.Rig.Client.Post<void>(TEXT("/counter/Counter-Canary-13")).Body(TEXT("Counter-Canary-13")),
   FVaRestFakeResponse::Text(200,TEXT("Counter-Canary-13 benign-counter")),&Id)) return false;
  Delta[On]=FVaRestTrace::Counters().RedactorCalls.load()-Before;
  if(On) F.Row(Id,[&](const auto& R){T.TestTrue(TEXT("Task13 ON counter interval produced real retained row"),R.Completed.Fixed!=nullptr);});
 }
 T.TestTrue(TEXT("Task13 real producer has positive redactor work"),Delta[0]>0);
 T.TestEqual(TEXT("Task13 Inspector capture never invokes a second redactor"),Delta[1],Delta[0]);
 return true;
}
bool ReplayAccepted(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FCaptureFixture F(T,true); if(!F.Ready()) return false;
 F.Rig.Client.WithOfflineQueue(TEXT("redaction-ordinary-replay")); F.Rig.Connectivity->SetProbeInterval(200); F.Rig.Connectivity->ReportOffline();
 auto Queued=MakeShared<TArray<FString>,ESPMode::ThreadSafe>(); auto Results=MakeShared<TArray<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
 F.Rig.Client.OnQueuedResult().AddLambda([Results](const auto& R){Results->Add(R);});
 auto O=MakeOutcome<void>(); const auto Original=F.Track(Observe(F.Rig.Client.Post<void>(TEXT("/replay")).Body(TEXT("q7!")).Idempotent().QueueWhenOffline()
  .OnQueued([Queued](const FString& Q){Queued->Add(Q);}),O).Send());
 if(!T.TestTrue(TEXT("Task13 ordinary short sentinel durably accepted"),PumpUntil([Queued]{return Queued->Num()==1;}) && F.Settle())) return false;
 FGuid Guid; T.TestTrue(TEXT("Task13 replay uses real queue identity"),FGuid::Parse((*Queued)[0],Guid) && Guid.IsValid());
 // No SetSecretValue for q7!: a known persistence secret would be rejected before this seam.
 F.Rig.Fake->Enqueue(FVaRestFakeResponse::Manual()); F.Rig.Clock->Advance(100); F.Settle();
 const int64 Before=Counters().TraceExcerptBytesCopied.load();
 F.Rig.Connectivity->ReportOnline(Original.GetId());
 if(!T.TestTrue(TEXT("Task13 replay reaches actual held dispatch"),PumpUntil([&]{return F.Rig.Fake->NumCalls()==1;}))) return false;
 FlushRedactionPipe(); F.DrainCapture();
 const int64 RequestCopied=Counters().TraceExcerptBytesCopied.load()-Before;
 const uint64 Id=F.Rig.Fake->GetCall(0).Request.RequestId;
 T.TestTrue(TEXT("Task13 genuine replay has new request id"),Id!=0 && Id!=Original.GetId());
 bool Promise=false,RequestExcerpt=false,ReplayIdentity=false;
 for(const auto& E:F.Recorder->For(Id)) {
  if(E.Kind==EVaRestTraceKind::Started) {const auto& S=E.Payload.Get<FVaRestTraceStarted>();Promise|=S.bExcerptPending;ReplayIdentity|=S.QueueReplayOf.QueueId==(*Queued)[0] && S.QueueReplayOf.OriginalRequestId==Original.GetId();}
  if(E.Kind==EVaRestTraceKind::Excerpt) RequestExcerpt|=E.Payload.Get<FVaRestTraceExcerptEvent>().Owner==EVaRestTraceExcerptOwner::Request;
 }
 T.TestTrue(TEXT("Task13 public Started proves replay provenance"),ReplayIdentity);
 FRedactionScan Replay({TEXT("q7!")}); F.Row(Id,[&](const auto& R){Replay.Record(R);});
 V.Replay &= RequestCopied==0 && !Promise && !RequestExcerpt && !Replay.Leaked();
 F.Rig.Fake->Complete(0,FVaRestFakeResponse::Text(200,TEXT("benign-replay-response")));
 if(!T.TestTrue(TEXT("Task13 durable replay retires successfully"),PumpUntil([Results]{return Results->Num()==1;}) && F.Settle())) return false;
 T.TestTrue(TEXT("Task13 successful durable retirement"),(*Results)[0].bSuccess && F.Rig.Client.GetQueuedCount()==0);
 T.TestEqual(TEXT("Task13 queued original has no ordinary completion"),O->CompleteCalls.load(),0);
 F.Row(Id,[&](const auto& R){T.TestTrue(TEXT("Task13 replay response preview follows ordinary producer"),R.FinalStatus==200 && R.Dispatches.Num()==1 && !R.Dispatches[0].ResponseExcerpt.Excerpt.IsEmpty());});
 auto Held=F.Snapshot(); if(!Held) return false;
 T.TestTrue(TEXT("Task13 actual replay link present"),Held->GetLinks().ContainsByPredicate([&](const auto& L){return L.FromRequestId==Id && L.ToRequestId==Original.GetId() && L.QueueId==(*Queued)[0];}));
 FRedactionScan Inventory; ScanRetained(F,Inventory);
 T.TestTrue(TEXT("Task13 marker/link/stats surfaces have independent witnesses"),Inventory.Markers>0 && Inventory.Links>0 && Inventory.Stats>0);
 // Ordinary nonreplay POST is the positive control for the same copy/promise observations.
 const int64 PositiveBefore=Counters().TraceExcerptBytesCopied.load(); uint64 Ordinary=0;
 if(!Send(F,F.Rig.Client.Post<void>(TEXT("/ordinary-copy")).Body(TEXT("ordinary-preview")),FVaRestFakeResponse::Text(200,TEXT("benign-positive")),&Ordinary)) return false;
 bool PositivePromise=false,PositiveExcerpt=false;
 for(const auto& E:F.Recorder->For(Ordinary)) {
  if(E.Kind==EVaRestTraceKind::Started) PositivePromise|=E.Payload.Get<FVaRestTraceStarted>().bExcerptPending;
  if(E.Kind==EVaRestTraceKind::Excerpt) PositiveExcerpt|=E.Payload.Get<FVaRestTraceExcerptEvent>().Owner==EVaRestTraceExcerptOwner::Request;
 }
 T.TestTrue(TEXT("Task13 ordinary request positively copies and announces preview"),Counters().TraceExcerptBytesCopied.load()>PositiveBefore && PositivePromise && PositiveExcerpt);
 return true;
}
bool ReplayKnownRefusal(FAutomationTestBase& T,FPrivacyVerdicts& V)
{
 FCaptureFixture F(T,true); if(!F.Ready()) return false;
 const FString CacheRoot=FPaths::Combine(F.Disk->Root,TEXT("Cache")), QueueRoot=FPaths::Combine(F.Disk->Root,TEXT("Queue"));
 {
  FTestRig Original; Original.Connectivity->ReportOffline();
  Original.Client.WithStorageDirectories(CacheRoot,QueueRoot).WithOfflineQueue(TEXT("known-short-refusal"));
  if(!T.TestTrue(TEXT("Task13 original short-control load drained"),Original.Drain())) return false;
  auto Queued=MakeShared<int32,ESPMode::ThreadSafe>(0);
  Original.Client.Put<void>(TEXT("/known-short")).Body(TEXT("q7!")).NoAuth().QueueWhenOffline()
   .OnQueued([Queued](const FString&){++*Queued;}).Send();
  if(!T.TestTrue(TEXT("Task13 known-short control persisted before recovery"),PumpUntil([Queued]{return *Queued==1;}) && Original.Drain() && F.Disk->Drain())) return false;
 }
 F.Rig.Client.SetSecretValue(TEXT("late-known"),TEXT("q7!"));
 auto Result=MakeShared<TOptional<FVaRestQueuedResult>,ESPMode::ThreadSafe>();
 F.Rig.Client.OnQueuedResult().AddLambda([Result](const auto& R){*Result=R;});
 F.Rig.Client.WithOfflineQueue(TEXT("known-short-refusal"));
 if(!T.TestTrue(TEXT("Task13 recovered known-short item classified"),PumpUntil([Result]{return Result->IsSet();}) && F.Settle())) return false;
 T.TestTrue(TEXT("Task13 original Config guard remains intact"),!Result->GetValue().bSuccess && Result->GetValue().Error.Kind==EVaRestErrorKind::Config && F.Rig.Fake->NumCalls()==0);
 const uint64 Id=Result->GetValue().Response.RequestId;
 bool Promise=false,Request=false;
 for(const auto& E:F.Recorder->For(Id)) {
  if(E.Kind==EVaRestTraceKind::Started) Promise|=E.Payload.Get<FVaRestTraceStarted>().bExcerptPending;
  if(E.Kind==EVaRestTraceKind::Excerpt) Request|=E.Payload.Get<FVaRestTraceExcerptEvent>().Owner==EVaRestTraceExcerptOwner::Request;
 }
 FRedactionScan S({TEXT("q7!")}); F.Row(Id,[&](const auto& R){S.Record(R);});
 // This is the early Config-refusal control, not the discriminating accepted replay oracle.
 V.Known &= !S.Leaked() && !Promise && !Request;
 return true;
}
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorHttpRedactionTest,"VaRest.Inspector.Redaction.Http",
 EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorHttpRedactionTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Inspector::Private::CaptureTesting;
 FPrivacyVerdicts V;
 // Run all independent finite fixtures; each owns cancellation/drain/isolation on every return.
 bool Built=true;
 Built=HttpMatrix(*this,V) && Built; Built=HeaderOracle(*this,V) && Built; Built=SubstringOracle(*this,V) && Built;
 Built=PathOracle(*this,V) && Built; Built=NumberAndMalformed(*this,V) && Built; Built=UnionRotation(*this,V) && Built;
 Built=ValidatedConsumer(*this,V) && Built; Built=DetailIsolation(*this,V) && Built; Built=BinaryDescriptors(*this) && Built;
 Built=StreamAndCache(*this,V) && Built; Built=Admission(*this) && Built; Built=CoreOnlyRedactor(*this) && Built;
 Built=ReplayAccepted(*this,V) && Built; Built=ReplayKnownRefusal(*this,V) && Built;
 TestTrue(TEXT("Task13 R1 custom sensitive header retained protection"),V.Header);
 TestTrue(TEXT("Task13 R2 cut number prefix absent"),V.Number);
 TestTrue(TEXT("Task13 R11 sensitive substring retained protection"),V.Substring);
 TestTrue(TEXT("Task13 R13 known values absent from every retained surface"),V.Known);
 TestTrue(TEXT("Task13 replay suppressed before request copy and announcement"),V.Replay);
 return Built;
}
#endif
