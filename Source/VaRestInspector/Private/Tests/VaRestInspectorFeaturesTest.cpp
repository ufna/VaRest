// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorTestUtil.h"

#if WITH_DEV_AUTOMATION_TESTS && VAREST_WITH_INSPECTOR
#include "VaRestInspector.h"
#include "VaRestInspectorExportService.h"
#include "VaRestInspectorStats.h"
#include "VaRestInspectorFilter.h"
#include "VaRestInspectorCurl.h"
#include "VaRestJson.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Misc/Base64.h"

namespace VaRest::Inspector::Private::FeatureTesting
{
using namespace CaptureTesting;
class FFacade final : public IVaRestInspector
{
public:
 explicit FFacade(FService& InService):Service(InService) {}
 void SetCapture(bool B) override { Service.SetCapture(B); }
 bool IsCapturing() const override { return Service.IsCapturing(); }
 void Clear() override { Service.Clear(); }
 bool ClearScope(uint64 Id) override { return Service.ClearScope(Id); }
 void BeginPIESession(bool B) override { Service.BeginPIESession(B); }
 uint64 GetRevision() const override { return Service.Revision(); }
 FVaRestInspectorDiagnostics GetDiagnostics() const override { return Service.GetDiagnostics(); }
 FVaRestInspectorChanged& OnChanged() override { return Changed; }
 bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe>& Out) const override { return Service.TrySnapshot(Out); }
 bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> V) const override { return Service.ForEachRecord(V); }
 bool ForEachLink(TFunctionRef<void(const FVaRestInspectorLinkView&)> V) const override { return Service.ForEachLink(V); }
 FVaRestInspectorExportTask ExportHar(FString Path,const FVaRestInspectorExportOptions& O) override { return Export::BeginOnGT(*this,MoveTemp(Path),O); }
private:
 FService& Service;
 FVaRestInspectorChanged Changed;
};
template<class P> void Emit(EVaRestTraceKind Kind,double At,P Payload)
{
 FVaRestTrace::Emit(Kind,At,FVaRestTracePayload(TInPlaceType<P>(),MoveTemp(Payload)),EVaRestTraceSource::Socket);
}
void SocketHistory(FCaptureFixture& F,uint64 Id)
{
 FVaRestTraceSocket Socket; Socket.SocketId=Id; Socket.ScopeId=77; Socket.Name=TEXT("FeatureSocket"); Socket.Protocol=TEXT("sio");
 Socket.Url=TEXT("wss://capture.invalid/socket"); Emit(EVaRestTraceKind::Socket,1.,Socket);
 for (uint64 A=1;A<=20;++A)
 {
  FVaRestTraceSocketAttempt Attempt; Attempt.SocketId=Id; Attempt.AttemptId=A; Attempt.Url=Socket.Url;
  Emit(EVaRestTraceKind::SocketAttempt,double(A)*10,Attempt);
  Attempt.Phase=EVaRestTraceSocketAttemptPhase::Open; Emit(EVaRestTraceKind::SocketAttempt,double(A)*10+1,Attempt);
  if (A==1) for (int32 I=0;I<5;++I)
  {
   FVaRestTraceSocketFrame Frame; Frame.SocketId=Id; Frame.AttemptId=A; Frame.FrameId=FVaRestTrace::NewSocketFrameId();
   Frame.Kind=EVaRestTraceSocketFrameKind::Binary; Frame.Bytes=10+I; Frame.Channel=TEXT("/room"); Frame.Route=TEXT("update");
   Frame.Excerpt.Kind=EVaRestExcerptKind::Skipped; Frame.Excerpt.SkipReason=EVaRestExcerptSkip::Disabled;
   Emit(EVaRestTraceKind::SocketFrame,12.+double(I)*0.01,MoveTemp(Frame));
  }
  Attempt.Phase=EVaRestTraceSocketAttemptPhase::Closed; Attempt.Close.Code=1000; Attempt.Close.bClean=true;
  Emit(EVaRestTraceKind::SocketAttempt,double(A)*10+2,MoveTemp(Attempt));
 }
 FVaRestTraceSocketNote Channel; Channel.SocketId=Id; Channel.AttemptId=20; Channel.Kind=EVaRestTraceSocketNoteKind::Channel;
 Channel.Channel=TEXT("/room"); Channel.PublicId=TEXT("public-id"); Channel.ChannelState=EVaRestTraceSocketChannelState::Up;
 Emit(EVaRestTraceKind::SocketNote,203.,MoveTemp(Channel));
 for (int32 I=0;I<70;++I)
 {
  FVaRestTraceSocketNote Ping; Ping.SocketId=Id; Ping.AttemptId=20; Ping.Kind=EVaRestTraceSocketNoteKind::Ping; Ping.Rtt=0.25;
  Emit(EVaRestTraceKind::SocketNote,204.+double(I)*0.01,MoveTemp(Ping));
 }
 Socket.Phase=EVaRestTraceSocketLifecycle::Released; Emit(EVaRestTraceKind::Socket,205.,MoveTemp(Socket)); F.DrainCapture();
}
FVaRestJson ReadHar(FAutomationTestBase& Test,const FString& Path,FVaRestInspectorExportTask Task)
{
 Task.Wait(); const auto& Result=Task.GetResult();
 if (!Test.TestTrue(TEXT("owned HAR worker publishes successfully"),Result.HasValue())) return {};
 TArray<uint8> Bytes; if (!Test.TestTrue(TEXT("read the published file"),FFileHelper::LoadFileToArray(Bytes,*Path))) return {};
 auto Parsed=FVaRestJson::Parse(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num()));
 if (!Test.TestTrue(TEXT("HAR file parses as real JSON"),Parsed.HasValue())) return {};
 return Parsed.StealValue().Get(TEXT("log")).Get(TEXT("entries"));
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorFeatureExportTest,"VaRest.Inspector.Features.SocketAndHar",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorFeatureExportTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Inspector;
 using namespace VaRest::Inspector::Private;
 using namespace FeatureTesting;
 using namespace VaRest::Http::Testing;
 FCapturePolicy Policy; Policy.Limits.MaxFramesPerSocket=3;
 FCaptureFixture F(*this,false,true,false,&Policy); if (!F.Ready()) return false;
 F.Rig.Fake->Enqueue(CaptureTesting::Json(200));
 auto Outcome=MakeOutcome<FVaRestJson>();
 auto Request=F.Track(Observe(F.Rig.Client.Get(TEXT("/http")).Header(TEXT("X-Copy"),TEXT("a'b"))
  .Header(TEXT("Authorization"),TEXT("Bearer CopySecret")),Outcome).Send());
 if (!TestTrue(TEXT("real fake-transport HTTP reaches terminal before draining"),F.Rig.Wait(Outcome))) return false;
 if (!F.Settle()) return false;
 const uint64 Id=Request.GetId(); SocketHistory(F,Id);
 int32 Http=0,Sockets=0;
 F.Service->ForEachRecord([&](const FVaRestInspectorRecordView& R)
 {
  if (R.RequestId!=Id) return;
  if (R.Kind==EVaRestInspectorRecordKind::Http)
  {
   ++Http; FVaRestInspectorFilter Filter; Filter.SetText(TEXT("status:2xx method:GET -is:socket"));
   TestTrue(TEXT("shared filter matches actual HTTP capture"),Filter.Matches(R));
   const FString Curl=FVaRestInspectorCurl::Build(R);
   TestTrue(TEXT("captured header apostrophe is shell quoted"),Curl.Contains(TEXT("'X-Copy: a'\\''b'")));
   TestFalse(TEXT("copy never restores a captured authorization secret"),Curl.Contains(TEXT("CopySecret")));
   TestTrue(TEXT("copy constructs a curl command from actual capture"),Curl.Contains(TEXT("curl --url '")));
   TestTrue(TEXT("missing selected attempt produces a refusal comment"),FVaRestInspectorCurl::Build(R,MAX_uint64).StartsWith(TEXT("# The selected attempt")));
   return;
  }
  if (R.Kind!=EVaRestInspectorRecordKind::Socket) return;
  ++Sockets; FVaRestInspectorFilter Filter; Filter.SetText(TEXT("is:socket is:sio scope:77"));
  TestTrue(TEXT("shared filter matches actual socket protocol and scope"),Filter.Matches(R));
  Filter.SetText(TEXT("-is:socket")); TestFalse(TEXT("socket negation excludes actual socket"),Filter.Matches(R));
  TestEqual(TEXT("retained frame cap"),R.SocketFramesRetained,3);
  TestEqual(TEXT("old frames evicted"),R.SocketFramesEvicted,uint64(2));
  TestEqual(TEXT("row cumulative bytes include evicted frames"),R.WireBytesDown,uint64(60));
  TestEqual(TEXT("old attempts are omitted"),R.SocketAttemptsOmitted,uint64(4));
  int32 Channels=0; R.ForEachSocketChannel([&](const FVaRestInspectorPacketView& C)
  { ++Channels; TestEqual(TEXT("census survives more than one full note ring"),FString(C.Text(View::EText::SocketChannel)),FString(TEXT("/room"))); });
  TestEqual(TEXT("one current channel survives note history eviction"),Channels,1);
 });
 TestEqual(TEXT("HTTP numeric identity retained"),Http,1); TestEqual(TEXT("same numeric socket identity is independent"),Sockets,1);
 auto Snapshot=F.Snapshot(); if (!Snapshot) return false;
 int32 Stats=0; Snapshot->ForEachSocketStats([&](const FVaRestInspectorSocketStats& S)
 {
  if (S.SocketId!=Id || S.bOverflow) return; ++Stats;
  TestEqual(TEXT("all attempts accumulated beyond retained history"),S.Attempts,uint64(20));
  TestEqual(TEXT("all received frames survive eviction in stats"),S.FramesIn,uint64(5));
  TestEqual(TEXT("all received bytes survive eviction in stats"),S.BytesDown,uint64(60));
  TestEqual(TEXT("all ping samples accumulated beyond notes"),S.RttCount,uint64(70));
  TestEqual(TEXT("RTT sample sum"),S.RttSum,17.5); TestTrue(TEXT("release observed"),S.bReleased);
 }); TestEqual(TEXT("one attributed socket stats group"),Stats,1); Snapshot.Reset();

 FFacade Inspector(*F.Service);
 const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("VaRestTests"),TEXT("HarFeatures-"))+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 const auto Gate=MakeShared<Export::FWorkerGate,ESPMode::ThreadSafe>();
 TArray<FVaRestInspectorExportTask> Jobs;
 ON_SCOPE_EXIT
 {
  Gate->Released.store(true); Export::SetWorkerGateOnGT(nullptr);
  for (const auto& Job:Jobs) Job.Wait();
  IFileManager::Get().DeleteDirectory(*Directory,false,true);
 };
 FVaRestInspectorExportOptions OnlyHttp; OnlyHttp.RequestIds.Add(Id);
 FVaRestInspectorExportOptions OnlySocket; OnlySocket.SocketIds.Add(Id);
 const FString HttpPath=FPaths::Combine(Directory,TEXT("http.har")),SocketPath=FPaths::Combine(Directory,TEXT("socket.har"));
 Export::SetWorkerGateOnGT(Gate);
 Jobs.Add(Inspector.ExportHar(HttpPath,OnlyHttp)); Jobs.Add(Inspector.ExportHar(SocketPath,OnlySocket));
 const double Deadline=FPlatformTime::Seconds()+5;
 while (Gate->Entered.load()<2 && FPlatformTime::Seconds()<Deadline) FPlatformProcess::SleepNoStats(0.001f);
 if (!TestEqual(TEXT("both workers hold only native copies"),Gate->Entered.load(),2)) return false;
 auto Refused=Inspector.ExportHar(FPaths::Combine(Directory,TEXT("third.har")),{});
 TestTrue(TEXT("third export refusal is immediate"),Refused.IsCompleted()); Refused.Wait();
 TestFalse(TEXT("third concurrent export is refused"),Refused.GetResult().HasValue());
 Inspector.Clear(); int32 AfterClear=0; Inspector.ForEachRecord([&](const auto&) { ++AfterClear; });
 TestEqual(TEXT("live capture clears before file workers proceed"),AfterClear,0);
 Gate->Released.store(true); Export::SetWorkerGateOnGT(nullptr);
 const auto HttpEntries=ReadHar(*this,HttpPath,Jobs[0]); TestEqual(TEXT("HTTP-only selection does not alias socket"),HttpEntries.Num(),1);
 const auto SocketEntries=ReadHar(*this,SocketPath,Jobs[1]); TestEqual(TEXT("retained attempts plus orphan frame attempt exported"),SocketEntries.Num(),17);
 int32 Orphans=0,Messages=0;
 for (int32 I=0;I<SocketEntries.Num();++I)
 {
  const auto Entry=SocketEntries[I],Detail=Entry.Get(TEXT("_varest"));
  TestEqual(TEXT("socket-only selection contains socket entries"),Entry.Get(TEXT("_resourceType")).AsString(),FString(TEXT("websocket")));
  TestEqual(TEXT("socket typed identity"),Detail.Get(TEXT("socketId")).AsString(),LexToString(Id));
  TestEqual(TEXT("owned census retained after live Clear"),Detail.Get(TEXT("channels")).Num(),1);
  if (!Detail.Get(TEXT("attemptDetailsMissing")).AsBool()) continue;
  ++Orphans; TestEqual(TEXT("orphan attempt identity retained"),Detail.Get(TEXT("attemptId")).AsString(),FString(TEXT("1")));
  const auto Frames=Entry.Get(TEXT("_webSocketMessages")),Descriptors=Detail.Get(TEXT("messages"));
  TestEqual(TEXT("message descriptors stay aligned"),Frames.Num(),Descriptors.Num()); Messages+=Frames.Num();
  for (int32 M=0;M<Frames.Num();++M)
  {
   TestEqual(TEXT("binary opcode"),Frames[M].Get(TEXT("opcode")).AsInt(),2);
   TestTrue(TEXT("uncaptured binary data is empty"),Frames[M].Get(TEXT("data")).AsString().IsEmpty());
   TestFalse(TEXT("uncaptured binary data is explicitly unavailable"),Descriptors[M].Get(TEXT("dataAvailable")).AsBool());
  }
 }
 TestEqual(TEXT("one orphan attempt"),Orphans,1); TestEqual(TEXT("only retained frames exported"),Messages,3);
 // Explicit empty and sockets-disabled selection are checked on a fresh, nonempty capture.
 SocketHistory(F,Id); FVaRestInspectorExportOptions Empty; Empty.bRestrictRequestIds=true;
 const FString EmptyPath=FPaths::Combine(Directory,TEXT("empty.har")); Jobs.Add(Inspector.ExportHar(EmptyPath,Empty));
 TestEqual(TEXT("explicit empty union exports no records"),ReadHar(*this,EmptyPath,Jobs.Last()).Num(),0);
 FVaRestInspectorExportOptions Disabled; Disabled.bIncludeSockets=false;
 const FString DisabledPath=FPaths::Combine(Directory,TEXT("disabled.har")); Jobs.Add(Inspector.ExportHar(DisabledPath,Disabled));
 TestEqual(TEXT("socket inclusion flag suppresses socket capture"),ReadHar(*this,DisabledPath,Jobs.Last()).Num(),0);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestInspectorFeatureBinaryCaptureTest,"VaRest.Inspector.Features.BinaryCapture",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestInspectorFeatureBinaryCaptureTest::RunTest(const FString& Parameters)
{
 using namespace VaRest::Inspector;
 using namespace VaRest::Inspector::Private;
 using namespace FeatureTesting;
 using namespace VaRest::Http::Testing;
 const TArray<uint8> Raw{0,0xff,0x89,1,2,3,4,5,6,7};
 for (int32 Cap:{0,4})
 {
  FCapturePolicy Policy; Policy.Limits.BinaryBytes=Cap; Policy.Limits.FrameBytes=8;
  FCaptureFixture F(*this,false,true,true,&Policy); if (!F.Ready()) return false;
  // Another sink asks for more bytes than this Inspector. Its own policy must still govern retained views/references.
  F.Recorder->Detail.BinaryCap=8; FVaRestTrace::RefreshDetail();
  F.Rig.Fake->Enqueue(FVaRestFakeResponse::Bytes(200,Raw)); auto Outcome=MakeOutcome<TArray<uint8>>();
  const uint64 Id=F.Track(Observe(F.Rig.Client.Get<TArray<uint8>>(TEXT("/binary-policy")),Outcome).Send()).GetId();
  if (!TestTrue(TEXT("real HTTP reaches terminal before native capture barriers"),F.Rig.Wait(Outcome)) || !F.Settle()) return false;
  FVaRestTraceExcerpt Produced; int32 ProducedCount=0;
  for (const auto& E:F.Recorder->ExcerptsOf(Id)) if (E.Owner==EVaRestTraceExcerptOwner::Response) { Produced=E.Excerpt; ++ProducedCount; }
  if (!TestTrue(TEXT("actual producer captures the other sink's eight-byte prefix"),ProducedCount==1 && Produced.bBinaryCaptured && Produced.Bytes && Produced.Bytes->Num()==8)) return false;
  int32 Responses=0;
  F.Row(Id,[&](const FVaRestInspectorRecordView& R)
  {
   for (const auto& A:R.Dispatches) if (A.ResponseExcerpt.Fixed)
   {
    ++Responses; const auto& P=A.ResponseExcerpt;
    TestEqual(TEXT("HTTP view enforces this Inspector's cap"),P.Excerpt.Num(),Cap);
    TestEqual(TEXT("HTTP marker reports actual content admission"),P.Fixed->Scalars.bBinaryCaptured,Cap>0);
   }
  });
  TestEqual(TEXT("one retained HTTP response descriptor/capture"),Responses,1);
  FVaRestTraceSocket Socket; Socket.SocketId=Id; Socket.ScopeId=77; Socket.Url=TEXT("ws://binary.invalid/");
  FeatureTesting::Emit(EVaRestTraceKind::Socket,1.,Socket);
  FVaRestTraceSocketFrame Frame; Frame.SocketId=Id; Frame.AttemptId=1; Frame.FrameId=FVaRestTrace::NewSocketFrameId();
  Frame.Kind=EVaRestTraceSocketFrameKind::Binary; Frame.Bytes=Raw.Num(); Frame.Excerpt=Produced;
  FeatureTesting::Emit(EVaRestTraceKind::SocketFrame,2.,MoveTemp(Frame)); F.DrainCapture();
  int32 Frames=0;
  F.Service->ForEachRecord([&](const FVaRestInspectorRecordView& R)
  {
   if (R.Kind!=EVaRestInspectorRecordKind::Socket || R.RequestId!=Id) return;
   R.ForEachSocketFrame([&](const FVaRestInspectorSocketFrameView& V)
   {
    ++Frames; if (!TestTrue(TEXT("binary frame retains metadata with either policy"),V.Excerpt.Fixed!=nullptr)) return;
    TestEqual(TEXT("frame view enforces this Inspector's cap"),V.Excerpt.Excerpt.Num(),Cap);
    TestEqual(TEXT("frame capture marker follows policy"),V.Excerpt.Fixed->Scalars.bBinaryCaptured,Cap>0);
   });
  });
  TestEqual(TEXT("one retained native frame"),Frames,1);
  FFacade Inspector(*F.Service);
  const FString Directory=FPaths::Combine(FPaths::ProjectSavedDir(),TEXT("VaRestTests"),TEXT("BinaryCapture-"))+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Directory,false,true); };
  const auto Entries=ReadHar(*this,FPaths::Combine(Directory,TEXT("binary.har")),Inspector.ExportHar(FPaths::Combine(Directory,TEXT("binary.har")),{}));
  TestEqual(TEXT("HAR retains HTTP and socket typed identities"),Entries.Num(),2);
  int32 Checked=0;
  const FString Encoded=Cap>0 ? FBase64::Encode(Raw.GetData(),Cap) : FString();
  for (const auto& Entry:Entries.Elements())
  {
   const bool SocketEntry=Entry.Get(TEXT("_resourceType")).AsString()==TEXT("websocket");
   const auto Body=SocketEntry ? Entry.Get(TEXT("_varest")).Get(TEXT("messages"))[0].Get(TEXT("body")) : Entry.Get(TEXT("_varest")).Get(TEXT("responseBody"));
   TestEqual(TEXT("HAR explicitly records binary capture opt-in"),Body.Get(TEXT("binaryCaptured")).AsBool(),Cap>0);
   TestEqual(TEXT("HAR reports body bytes rather than descriptor-label length"),Body.Get(TEXT("capturedBytes")).AsInt(),Cap);
   TestEqual(TEXT("HAR encodes only the admitted prefix"),Body.Get(TEXT("base64")).AsString(),Encoded);
   if (SocketEntry) TestEqual(TEXT("Chrome binary frame data is captured Base64 or empty"),Entry.Get(TEXT("_webSocketMessages"))[0].Get(TEXT("data")).AsString(),Encoded);
   else TestFalse(TEXT("binary bytes never enter HAR UTF-8 content text"),Entry.Get(TEXT("response")).Get(TEXT("content")).Get(TEXT("text")).IsValid());
   ++Checked;
  }
  TestEqual(TEXT("both owned native exports were inspected"),Checked,2);
 }
 return true;
}
#endif
