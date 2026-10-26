// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspector.h"
#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
#include "Misc/AutomationTest.h"
#include "CoreGlobals.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTesting.h"
#include "VaRestHttpTestTypes.h"
#include "VaRestInspectorSettings.h"

namespace VaRestTestsInspectorDecodePrivate
{
FVaRestFakeResponse Json(int32 Status,const char* Body)
{
 return FVaRestFakeResponse::Json(Status,FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Body)));
}
const char* const Profile=R"({"id":9007199254740993,"name":"neo","items":[{"name":"a","level":1},{"name":"b","level":"2"}]})";
struct FCase
{
 TUniquePtr<VaRest::Http::Testing::FTestRig> Rig=MakeUnique<VaRest::Http::Testing::FTestRig>();
 TSharedRef<VaRest::Http::Testing::TTestOutcome<FVaRestHttpTestProfile>,ESPMode::ThreadSafe> Outcome=VaRest::Http::Testing::MakeOutcome<FVaRestHttpTestProfile>();
 uint64 Id=0;
 int32 Index=0;
};
struct FState
{
 explicit FState(FAutomationTestBase& T):Test(T),Inspector(IVaRestInspector::Get()) {}
 ~FState(){Cleanup();}
 FAutomationTestBase& Test;
 IVaRestInspector& Inspector;
 UVaRestInspectorSettings* Settings=nullptr;
 FVaRestInspectorLimits OriginalEditor,OriginalPackaged;
 bool OriginalCapture=false,OriginalHeaders=false,OriginalBodies=false,Cleaned=false,Begun=false;
 TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
 TSharedPtr<VaRest::Http::Testing::FTraceRecorder,ESPMode::ThreadSafe> Source;
 TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Held;
 TArray<TUniquePtr<FCase>> Cases;
 int32 OnBefore=0,OffBefore=0,Polls=0;
 uint64 InitialFrame=0;
 bool Begin()
 {
  using namespace VaRest::Http::Testing;
  Begun=true;Settings=GetMutableDefault<UVaRestInspectorSettings>();
  OriginalEditor=Settings->Editor;OriginalPackaged=Settings->Packaged;
  OriginalHeaders=Settings->bCaptureHeaders;OriginalBodies=Settings->bCaptureBodies;OriginalCapture=Inspector.IsCapturing();
  Inspector.SetCapture(false);Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>();
  auto& Limits=GIsEditor?Settings->Editor:Settings->Packaged;
  Limits.BudgetMiB=16;Limits.MaxPendingMiB=4;Limits.MaxRecords=512;Limits.MaxPendingEvents=4096;
  Settings->bCaptureHeaders=true;Settings->bCaptureBodies=false;
  Inspector.Clear();Inspector.SetCapture(true);
  if(!Test.TestTrue(TEXT("public facade capture admitted"),Inspector.IsCapturing()))return false;
  Source=MakeShared<FTraceRecorder,ESPMode::ThreadSafe>(EVaRestTraceDetail::Metadata|EVaRestTraceDetail::Headers);
  FVaRestTrace::AddSink(Source.ToSharedRef());
  FlushRedactionPipe();
  OnBefore=Counters().DecodesOnGameThread.load();OffBefore=Counters().DecodesOffGameThread.load();
  for(int32 Index=0;Index<9;++Index) {
   auto C=MakeUnique<FCase>();C->Index=Index;auto& Rig=*C->Rig;
   // Each case owns a distinct client: a later known-value union cannot scrub
   // the benign control's earlier warning path or configured envelope strings.
   Rig.Clock->Advance(double(Index)*.125);
   const char* Body=Profile;int32 Status=200;
   FVaRestEnvelope Envelope;bool HasEnvelope=false;
   if(Index==2){Body="";}
   if(Index==3){Status=204;Body="ignored stray body";}
   if(Index==4){Body=R"({"id":7,"name":"root","ok":true})";Envelope=FVaRestEnvelope::DataAt(TEXT("")).SuccessWhen(TEXT("ok"),{TEXT("true")});HasEnvelope=true;}
   if(Index==5){Body=R"({"data":{"id":8,"name":"nested"},"ok":true})";Envelope=FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"),{TEXT("true")});HasEnvelope=true;}
   if(Index==6){
    Rig.Client.SetSecretValue(TEXT("EnvelopeValue"),TEXT("EnvelopeValueCanary57391"));
    Body=R"({"data":{"id":9},"ok":"denied-EnvelopeValueCanary57391","code":"E_EnvelopeValueCanary57391","message":"ordinary EnvelopeValueCanary57391 refused"})";
    Envelope=FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"),{TEXT("true")}).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"));HasEnvelope=true;
   }
   if(Index==7){
    // Producer prerequisite VaRest-8e5.1; the consumer never scrubs or fixes it.
    Rig.Client.SetSecretValue(TEXT("EnvelopeData"),TEXT("EnvelopeDataCanary57391"));
    Rig.Client.SetSecretValue(TEXT("EnvelopePredicate"),TEXT("EnvelopePredicateCanary68402"));
    Rig.Client.SetSecretValue(TEXT("EnvelopeExpected"),TEXT("EnvelopeExpectedCanary79513"));
    Body=R"({"data":{"EnvelopeDataCanary57391":{"id":10}},"checks":{"EnvelopePredicateCanary68402":"ordinary-denied"},"code":"E_CONFIG","message":"ordinary configuration refused"})";
    Envelope=FVaRestEnvelope::DataAt(TEXT("data.EnvelopeDataCanary57391")).SuccessWhen(TEXT("checks.EnvelopePredicateCanary68402"),{TEXT("accepted-EnvelopeExpectedCanary79513")}).ErrorCodeAt(TEXT("code")).ErrorMessageAt(TEXT("message"));HasEnvelope=true;
   }
   if(Index==8)Rig.Client.SetSecretValue(TEXT("WarningPath"),TEXT("items[1].level"));
   Rig.Fake->Enqueue(Json(Status,Body));
   auto Request=Rig.Client.Get<FVaRestHttpTestProfile>(FString::Printf(TEXT("/decode/%d"),Index));
   if(Index==1)Request=MoveTemp(Request).StrictDecode();
   if(HasEnvelope)Request=MoveTemp(Request).Envelope(Envelope);
   C->Id=Observe(MoveTemp(Request),C->Outcome).Send().GetId();Cases.Add(MoveTemp(C));
  }
  InitialFrame=GFrameCounter;return true;
 }
 bool Ready() const
 {
  for(const auto& C:Cases)if(!C->Outcome->IsDone())return false;
  int32 Rows=0;
  Inspector.ForEachRecord([&](const auto& R){if(Cases.ContainsByPredicate([&](const auto& C){return C->Id==R.RequestId;}) && R.bActualCompletionObserved && R.CompletedMetadataState==EVaRestInspectorDetailState::Present)++Rows;});
  return Rows==Cases.Num() && GFrameCounter>InitialFrame;
 }
 void Compare(const FCase& C,const FVaRestInspectorRecordView& R,const FVaRestTraceEvent& E)
 {
  using namespace VaRest::Inspector::View;
  const auto& P=E.Payload.Get<FVaRestTraceCompleted>();
  if(!Test.TestNotNull(TEXT("typed Completed packet retained"),R.Completed.Fixed))return;
  const auto& S=R.Completed.Fixed->Scalars;
  Test.TestEqual(TEXT("typed handle/live/retained identity"),R.RequestId,C.Id);
  Test.TestEqual(TEXT("typed exact source Seq"),R.Completed.Fixed->Header.Seq,E.Seq);
  Test.TestEqual(TEXT("typed source completion seconds"),R.CompletedAt,E.Time);
  Test.TestEqual(TEXT("decode start source seconds, never receipt units"),R.DecodeStartAt,P.DecodeStartAt);
  Test.TestEqual(TEXT("decode end source seconds, never receipt units"),R.DecodeEndAt,P.DecodeEndAt);
  Test.TestEqual(TEXT("reflected result type"),S.ResultType,FName(TEXT("VaRestHttpTestProfile")));
  Test.TestEqual(TEXT("exact source result name"),S.ResultType,P.ResultType);
  Test.TestEqual(TEXT("warning count source"),S.DecodeWarningCount,P.DecodeWarningCount);
  Test.TestEqual(TEXT("delivered final HTTP status"),R.FinalStatus,P.Response.Status);
  Test.TestTrue(TEXT("captured typed record complete without loss"),!R.bPartial && R.bTerminalKnown && R.bSealed);
  Test.TestTrue(TEXT("source zero is known and UTC is independently nullable"),R.bStartedAtKnown);
  Test.TestTrue(TEXT("envelope applied matches source"),S.bEnvelopeApplied==P.Envelope.bApplied);
  const TPair<EText,FString> Fields[]={
   {EText::DataPath,P.Envelope.DataPath},{EText::PredicatePath,P.Envelope.PredicatePath},{EText::Expected,P.Envelope.Expected},
   {EText::Actual,P.Envelope.Actual},{EText::EnvelopeCode,P.Envelope.Code},{EText::EnvelopeMessage,P.Envelope.Message}};
  for(const auto& F:Fields)Test.TestEqualSensitive(TEXT("exact source-redacted envelope text"),FString(R.Completed.Text(F.Key)),F.Value);
  const auto W=R.Completed.Pairs(R.Completed.Fixed->Warnings);
  if(Test.TestEqual(TEXT("retained exact warning array"),W.Num(),P.DecodeWarnings.Num()))for(int32 I=0;I<W.Num();++I){
   Test.TestEqualSensitive(TEXT("warning path exact"),FString(R.Completed.Read(W[I].First)),P.DecodeWarnings[I].Path);
   Test.TestEqualSensitive(TEXT("warning message exact"),FString(R.Completed.Read(W[I].Second)),P.DecodeWarnings[I].Message);
  }
  if(C.Index==0 || C.Index==8){
   Test.TestEqual(TEXT("one real mismatch warning"),P.DecodeWarningCount,1);
   if(Test.TestEqual(TEXT("one warning retained"),W.Num(),1)){
    Test.TestEqualSensitive(TEXT("benign or producer-redacted warning path"),FString(R.Completed.Read(W[0].First)),FString(C.Index==8?TEXT("***"):TEXT("items[1].level")));
    Test.TestEqualSensitive(TEXT("ordinary warning message remains nonempty and exact"),FString(R.Completed.Read(W[0].Second)),FString(TEXT("an integer was read from a string")));
   }
  }
  if(C.Index==4)Test.TestTrue(TEXT("real root envelope keeps empty data path and evaluates predicate"),S.bEnvelopeApplied && R.Completed.Text(EText::DataPath).IsEmpty() && C.Outcome->SuccessCalls.load()==1);
  if(C.Index==5)Test.TestEqualSensitive(TEXT("nonempty data path retained"),FString(R.Completed.Text(EText::DataPath)),FString(TEXT("data")));
  if(C.Index==6){
   Test.TestEqualSensitive(TEXT("server actual redacted by producer"),FString(R.Completed.Text(EText::Actual)),FString(TEXT("denied-***")));
   Test.TestEqualSensitive(TEXT("server code redacted by producer"),FString(R.Completed.Text(EText::EnvelopeCode)),FString(TEXT("E_***")));
   Test.TestEqualSensitive(TEXT("server message redacted with ordinary control"),FString(R.Completed.Text(EText::EnvelopeMessage)),FString(TEXT("ordinary *** refused")));
  }
  if(C.Index==7){
   Test.TestEqualSensitive(TEXT("producer-config-data-path redaction retained"),FString(R.Completed.Text(EText::DataPath)),FString(TEXT("data.***")));
   Test.TestEqualSensitive(TEXT("producer-config-predicate-path redaction retained"),FString(R.Completed.Text(EText::PredicatePath)),FString(TEXT("checks.***")));
   Test.TestEqualSensitive(TEXT("producer-config-expected redaction retained"),FString(R.Completed.Text(EText::Expected)),FString(TEXT("accepted-***")));
   Test.TestEqualSensitive(TEXT("benign actual survives"),FString(R.Completed.Text(EText::Actual)),FString(TEXT("ordinary-denied")));
   Test.TestEqualSensitive(TEXT("benign code survives"),FString(R.Completed.Text(EText::EnvelopeCode)),FString(TEXT("E_CONFIG")));
   Test.TestEqualSensitive(TEXT("benign message survives"),FString(R.Completed.Text(EText::EnvelopeMessage)),FString(TEXT("ordinary configuration refused")));
  }
 }
 void Check()
 {
  using namespace VaRest::Http::Testing;
  for(auto& C:Cases)Test.TestTrue(TEXT("typed owning rig drain"),C->Rig->Drain());FlushRedactionPipe();
  const int32 Off=Counters().DecodesOffGameThread.load(),On=Counters().DecodesOnGameThread.load();
  Test.TestTrue(TEXT("real typed decoding happened off GT"),Off>OffBefore);
  Test.TestEqual(TEXT("typed decode never in GT sink/store"),On-OnBefore,0);
  if(!Test.TestTrue(TEXT("public immutable decode snapshot admitted"),Inspector.TrySnapshot(Held) && Held.IsValid()))return;
  for(const auto& C:Cases){
   TOptional<FVaRestTraceEvent> Completion;TOptional<FVaRestTraceStarted> Start;
   for(const auto& E:Source->For(C->Id)) {
    if(E.Kind==EVaRestTraceKind::Completed)Completion=E;
    if(E.Kind==EVaRestTraceKind::Started)Start=E.Payload.Get<FVaRestTraceStarted>();
   }
   if(!Test.TestTrue(TEXT("actual typed source completion and start exist"),Completion.IsSet() && Start.IsSet()))continue;
   int32 Live=0,Retained=0;uint64 Arrival=0,Birth=0,Revision=0,Seq=0;
   Inspector.ForEachRecord([&](const auto& R){if(R.RequestId==C->Id){++Live;Arrival=R.ArrivalOrdinal;Birth=R.BirthMetadataOrdinal;Revision=R.Revision;Seq=R.Completed.Fixed?R.Completed.Fixed->Header.Seq:0;Compare(*C,R,Completion.GetValue());Test.TestEqual(TEXT("source start time known including zero"),R.StartedAt,Start->StartedAt);Test.TestEqual(TEXT("nullable UTC derives only from source"),R.bStartUtcKnown,Start->StartUtc.GetTicks()!=0);}});
   Held->ForEachRecord([&](const auto& R){if(R.RequestId==C->Id){++Retained;Compare(*C,R,Completion.GetValue());Test.TestTrue(TEXT("live and retained row identity/revision agree"),R.ArrivalOrdinal==Arrival && R.BirthMetadataOrdinal==Birth && R.Revision==Revision && R.Completed.Fixed && R.Completed.Fixed->Header.Seq==Seq);}});
   Test.TestTrue(TEXT("same handle appears once in live and retained snapshot"),Live==1 && Retained==1);
   const auto& O=C->Outcome;
   if(C->Index==1)Test.TestTrue(TEXT("strict public typed result really fails"),O->Error.Kind==EVaRestErrorKind::Decode && !O->Value.IsSet() && O->Error.Path==TEXT("items[1].level"));
   else if(C->Index==6 || C->Index==7)Test.TestTrue(TEXT("real envelope failure is Server error, not decode value"),O->Error.Kind==EVaRestErrorKind::Server && !O->Value.IsSet());
   else if(Test.TestTrue(TEXT("real typed value delivered"),O->Value.IsSet())) {
    const auto& V=O->Value.GetValue();
    if(C->Index==2 || C->Index==3)Test.TestTrue(TEXT("empty200/204 retain struct defaults"),V.Id==0 && V.Name.IsEmpty() && V.Items.IsEmpty() && V.Score==7);
    if(C->Index==0 || C->Index==8)Test.TestTrue(TEXT("tolerant public typed result is real"),V.Id==9007199254740993ll && V.Name==TEXT("neo") && V.Items.Num()==2 && V.Items[1].Level==2 && V.Score==7);
   }
  }
  Test.TestEqual(TEXT("reading live and retained views performs no extra decode"),Counters().DecodesOffGameThread.load(),Off);
  Test.TestEqual(TEXT("reading views performs no GT decode"),Counters().DecodesOnGameThread.load(),On);
 }
 void Cleanup()
 {
  if(Cleaned)return;Cleaned=true;if(!Begun)return;
  using namespace VaRest::Http::Testing;
  for(auto& C:Cases)C->Rig->Client.CancelAll();
  for(auto& C:Cases){Test.TestTrue(TEXT("latent fixture cancels all owned requests"),PumpUntil([Rig=C->Rig.Get()]{return Rig->Client.GetInFlightCount()==0;}));Test.TestTrue(TEXT("latent fixture drains owned tasks"),C->Rig->Drain());}
  FlushRedactionPipe();
  if(Source)FVaRestTrace::RemoveSink(Source.ToSharedRef());Source.Reset();Held.Reset();
  Inspector.SetCapture(false);Cases.Reset();
  Settings->Editor=OriginalEditor;Settings->Packaged=OriginalPackaged;Settings->bCaptureHeaders=OriginalHeaders;Settings->bCaptureBodies=OriginalBodies;
  // Explicit Clear commits restored account capacities before registry/capture restoration.
  Inspector.Clear();Isolation.Reset();Inspector.SetCapture(OriginalCapture);
 }
};
class FWaitForRealEndFrame final:public IAutomationLatentCommand
{
public:
 explicit FWaitForRealEndFrame(TSharedRef<FState,ESPMode::ThreadSafe> In):State(MoveTemp(In)){}
 bool Update() override {
  if(!State->Begun){if(!State->Begin()){State->Cleanup();return true;}return false;}
  // Normal automation frames drive the facade's actual OnEndFrame binding.
  // No private FService, Tick(), PumpGT-as-frame or recursive Broadcast here.
  if(!State->Ready() && ++State->Polls<256)return false;
  if(State->Test.TestTrue(TEXT("normal engine EndFrame delivered every typed record"),State->Ready()))State->Check();
  State->Cleanup();return true;
 }
private:TSharedRef<FState,ESPMode::ThreadSafe> State;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorCaptureDecodeTest,"VaRest.Inspector.Capture.Decode",EAutomationTestFlags_ApplicationContextMask|EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorCaptureDecodeTest::RunTest(const FString& Parameters)
{
 using namespace VaRestTestsInspectorDecodePrivate;
 AddCommand(new FWaitForRealEndFrame(MakeShared<FState,ESPMode::ThreadSafe>(*this)));return true;
}
#endif
