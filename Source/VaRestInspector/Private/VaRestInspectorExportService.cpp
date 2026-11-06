// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorExportService.h"
#include "VaRestInspector.h"
#include "VaRestJsonWriterInternal.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Misc/Base64.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#endif

namespace VaRest::Inspector::Export
{
namespace
{
bool Running=false;
TArray<FVaRestInspectorExportTask> Jobs;
#if WITH_DEV_AUTOMATION_TESTS
TSharedPtr<FWorkerGate,ESPMode::ThreadSafe> WorkerGate;
#endif
FVaRestInspectorExportTask Failed(const TCHAR* Error)
{ return UE::Tasks::MakeCompletedTask<FVaRestInspectorExportResult>(MakeError(FString(Error))); }
#if VAREST_WITH_INSPECTOR
using EText = View::EText;
struct FPacket
{
 View::FPacketFixed Fixed;
 TArray<uint8> Backing, Excerpt;
 bool Present=false;
 static int64 Measure(const FVaRestInspectorPacketView& P) { return int64(P.Backing.Num())+P.Excerpt.Num(); }
 void Copy(const FVaRestInspectorPacketView& P)
 {
  if (!P.Fixed) return;
  Fixed=*P.Fixed; Present=true;
  if (!P.Backing.IsEmpty()) Backing.Append(P.Backing.GetData(),P.Backing.Num());
  if (!P.Excerpt.IsEmpty()) Excerpt.Append(P.Excerpt.GetData(),P.Excerpt.Num());
 }
 FVaRestInspectorPacketView ViewOf() const { return {Present ? &Fixed : nullptr,Backing,Excerpt}; }
 FString Text(EText Field) const { return FString(ViewOf().Text(Field)); }
};
struct FAttempt
{
 uint64 Dispatch=0; int32 Number=0;
 double Start=-1;
 FPacket Request,Response,Body;
 FVaRestInspectorPhase Auth,Queued,Prepare,Send,Wait,Receive,Wall,Gap;
 EVaRestTraceGap GapKind=EVaRestTraceGap::None;
 bool HeadersKnown=false,HasDispatch=false,HasEnded=false,Combined=false;
};
struct FSocketAttempt
{
 uint64 Id=0;
 FPacket Connecting,Opened,Closed;
 bool HasConnecting=false,HasOpened=false,HasClosed=false,Partial=false;
};
struct FSocketFrame
{
 uint64 Id=0,AttemptId=0;
 FPacket Metadata,Excerpt;
 EVaRestInspectorDetailState State=EVaRestInspectorDetailState::Absent;
};
struct FRecord
{
 EVaRestInspectorRecordKind Kind=EVaRestInspectorRecordKind::Http;
 uint64 Id=0,Scope=0,Client=0,Arrival=0,Observed=0,Omitted=0,Up=0,Down=0;
 FDateTime Utc;
 double Start=0;
 int32 Status=0;
 bool UtcKnown=false,StartKnown=false,ContextKnown=false;
 EVaRestInspectorEndState State=EVaRestInspectorEndState::CaptureInterrupted;
 FString ScopeLabel,ClientLabel;
 FPacket Started,Completed,RequestBody;
 TArray<FAttempt> Attempts;
 TArray<FPacket> Notes;
 TArray<FSocketAttempt> SocketAttempts;
 TArray<FSocketFrame> Frames;
 TArray<FPacket> Channels;
 FName SocketName,Protocol,Entry,SocketState,Failure;
 int32 CloseCode=0;
 uint64 FramesIn=0,FramesOut=0,FramesEvicted=0,FramesOmitted=0,DroppedFrames=0,ChannelsOmitted=0;
 bool Partial=false,Ready=false,Released=false;
};
struct FDocument
{
 TArray<FRecord> Records;
 TArray<FVaRestInspectorLink> Links;
 FVaRestInspectorDiagnostics Diagnostics;
 uint64 Revision=0;
 bool SocketsRequested=true;
};
struct FEntry
{
 int32 Record=0,Attempt=INDEX_NONE;
 FDateTime Utc;
 bool UtcKnown=false;
 uint64 SocketAttemptId=0;
 TArray<int32> Frames;
};
constexpr int64 OutputLimit=64ll*1024*1024;

bool MakeDocument(IVaRestInspector& Inspector,const FVaRestInspectorExportOptions& Options,TUniquePtr<FDocument>& Out,FString& Error)
{
 if (int64(Options.RequestIds.Num())+Options.SocketIds.Num()>10000) { Error=TEXT("Export selection exceeds 10000 identities."); return false; }
 TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Snapshot;
 if (!Inspector.TrySnapshot(Snapshot) || !Snapshot) { Error=TEXT("Capture snapshot refused the export under its current budget."); return false; }
 TSet<uint64> Selection,SocketSelection;
 for (uint64 Id:Options.RequestIds) Selection.Add(Id);
 for (uint64 Id:Options.SocketIds) SocketSelection.Add(Id);
 const bool Restrict=Options.bRestrictRequestIds || !Selection.IsEmpty() || !SocketSelection.IsEmpty();
 const auto Matches=[&](const FVaRestInspectorRecordView& R)
 {
  if (Options.ScopeId.IsSet() && (!R.bContextKnown || R.ScopeId!=Options.ScopeId.GetValue())) return false;
  if (R.Kind==EVaRestInspectorRecordKind::Http) return !Restrict || Selection.Contains(R.RequestId);
  return Options.bIncludeSockets && R.Kind==EVaRestInspectorRecordKind::Socket && (!Restrict || SocketSelection.Contains(R.RequestId));
 };
 const int64 Limit=int64(FMath::Clamp(Options.MaxCopyMiB,1,64))*1024*1024;
 int64 Bytes=sizeof(FDocument); int32 Count=0; bool Fits=true; TSet<uint64> Selected;
 const auto Add=[&](int64 N) { if (N<0 || N>Limit-Bytes) Fits=false; else Bytes+=N; };
 Snapshot->ForEachRecord([&](const FVaRestInspectorRecordView& R)
 {
  if (!Matches(R) || !Fits) return;
  ++Count; if (Count>10000) { Fits=false; return; } if (R.Kind==EVaRestInspectorRecordKind::Http) Selected.Add(R.RequestId);
  Add(sizeof(FRecord)+int64(R.Dispatches.Num())*sizeof(FAttempt)+(int64(R.ScopeLabel.Len())+R.ClientLabel.Len()+2)*sizeof(TCHAR));
  Add(FPacket::Measure(R.Started)); Add(FPacket::Measure(R.Completed));
  Add(int64(R.Notes.Num())*sizeof(FPacket)); for (const auto& N:R.Notes) Add(FPacket::Measure(N));
  if (R.Kind==EVaRestInspectorRecordKind::Http)
  {
   for (const auto& P:R.Excerpts) if (P.Fixed && P.Fixed->Scalars.Owner==EVaRestTraceExcerptOwner::Request) { Add(FPacket::Measure(P)); break; }
   for (const auto& A:R.Dispatches) { Add(FPacket::Measure(A.Dispatched)); Add(FPacket::Measure(A.Ended)); Add(FPacket::Measure(A.ResponseExcerpt)); }
  }
  else
  {
   R.ForEachSocketAttempt([&](const FVaRestInspectorSocketAttemptView& A) { Add(sizeof(FSocketAttempt)); Add(FPacket::Measure(A.Connecting)); Add(FPacket::Measure(A.Opened)); Add(FPacket::Measure(A.Closed)); });
   R.ForEachSocketFrame([&](const FVaRestInspectorSocketFrameView& F) { Add(sizeof(FSocketFrame)); Add(FPacket::Measure(F.Metadata)); Add(FPacket::Measure(F.Excerpt)); });
   R.ForEachSocketChannel([&](const FVaRestInspectorPacketView& P) { Add(sizeof(FPacket)); Add(FPacket::Measure(P)); });
  }
 });
 int32 LinkCount=0;
 for (const auto& Link:Snapshot->GetLinks()) if (Selected.Contains(Link.FromRequestId)) { Add(sizeof(FVaRestInspectorLink)+int64(Link.QueueId.Len()+1)*sizeof(TCHAR)); ++LinkCount; }
 if (!Fits) { Error=TEXT("Export exceeds its bounded DTO copy limit. Select fewer requests or reduce capture excerpts."); return false; }
 auto Doc=MakeUnique<FDocument>(); Doc->Records.Reserve(Count); Doc->Links.Reserve(LinkCount);
 Doc->Diagnostics=Snapshot->GetDiagnostics(); Doc->Revision=Snapshot->GetRevision(); Doc->SocketsRequested=Options.bIncludeSockets;
 Snapshot->ForEachRecord([&](const FVaRestInspectorRecordView& R)
 {
  if (!Matches(R)) return;
  auto& D=Doc->Records.AddDefaulted_GetRef(); D.Kind=R.Kind; D.Id=R.RequestId; D.Scope=R.ScopeId; D.Client=R.ClientId; D.Arrival=R.ArrivalOrdinal; D.Observed=R.ObservedDispatches;
  D.Omitted=R.OmittedDispatches; D.Up=R.WireBytesUp; D.Down=R.WireBytesDown; D.Utc=R.StartUtc; D.UtcKnown=R.bStartUtcKnown; D.ContextKnown=R.bContextKnown;
  D.Start=R.StartedAt; D.StartKnown=R.bStartedAtKnown; D.Status=R.FinalStatus; D.State=R.EndState;
  D.ScopeLabel=FString(R.ScopeLabel); D.ClientLabel=FString(R.ClientLabel); D.Started.Copy(R.Started); D.Completed.Copy(R.Completed);
  if (R.Kind==EVaRestInspectorRecordKind::Http)
   for (const auto& P:R.Excerpts) if (P.Fixed && P.Fixed->Scalars.Owner==EVaRestTraceExcerptOwner::Request) { D.RequestBody.Copy(P); break; }
  D.Notes.Reserve(R.Notes.Num()); for (const auto& N:R.Notes) D.Notes.AddDefaulted_GetRef().Copy(N);
  D.Attempts.Reserve(R.Dispatches.Num());
  for (const auto& A:R.Dispatches)
  {
   auto& T=D.Attempts.AddDefaulted_GetRef(); T.Dispatch=A.DispatchId; T.Number=A.Attempt; T.Start=A.AttemptStart;
   T.HasDispatch=A.bHasDispatched; T.HasEnded=A.bHasEnded; T.HeadersKnown=A.HeadersState==EVaRestInspectorHeadersState::Captured; T.Combined=A.bCombinedSendWait;
   T.Request.Copy(A.Dispatched); T.Response.Copy(A.Ended); T.Body.Copy(A.ResponseExcerpt);
   T.Auth=A.Auth; T.Queued=A.Queued; T.Prepare=A.Prepare; T.Send=A.Send; T.Wait=A.Wait; T.Receive=A.Receive; T.Wall=A.Wall; T.Gap=A.Gap; T.GapKind=A.GapKind;
  }
  if (R.Kind==EVaRestInspectorRecordKind::Socket)
  {
   D.SocketName=R.SocketName; D.Protocol=R.SocketProtocol; D.Entry=R.SocketEntry; D.SocketState=R.SocketState; D.Failure=R.SocketFailure;
   D.CloseCode=R.SocketCloseCode; D.Partial=R.bPartial; D.Ready=R.bSocketReady; D.Released=R.bSocketReleased;
   D.FramesIn=R.SocketFramesIn; D.FramesOut=R.SocketFramesOut; D.FramesEvicted=R.SocketFramesEvicted; D.FramesOmitted=R.SocketFramesOmitted;
   D.DroppedFrames=R.SocketDroppedFrames; D.ChannelsOmitted=R.SocketChannelsOmitted; D.Omitted=R.SocketAttemptsOmitted;
   D.SocketAttempts.Reserve(16); D.Frames.Reserve(R.SocketFramesRetained); D.Channels.Reserve(R.SocketChannelsRetained);
   R.ForEachSocketAttempt([&](const FVaRestInspectorSocketAttemptView& A)
   {
    auto& T=D.SocketAttempts.AddDefaulted_GetRef(); T.Id=A.AttemptId; T.HasConnecting=A.bHasConnecting; T.HasOpened=A.bHasOpened; T.HasClosed=A.bHasClosed; T.Partial=A.bPartial;
    T.Connecting.Copy(A.Connecting); T.Opened.Copy(A.Opened); T.Closed.Copy(A.Closed);
   });
   R.ForEachSocketFrame([&](const FVaRestInspectorSocketFrameView& F)
   { auto& T=D.Frames.AddDefaulted_GetRef(); T.Id=F.FrameId; T.AttemptId=F.AttemptId; T.State=F.ExcerptState; T.Metadata.Copy(F.Metadata); T.Excerpt.Copy(F.Excerpt); });
   R.ForEachSocketChannel([&](const FVaRestInspectorPacketView& P) { D.Channels.AddDefaulted_GetRef().Copy(P); });
  }
 });
 for (const auto& Link:Snapshot->GetLinks()) if (Selected.Contains(Link.FromRequestId)) Doc->Links.Add(Link);
 // Snapshot, borrowed visitors and all GT selection state die before publishing the native DTO to a worker.
 Snapshot.Reset(); Out=MoveTemp(Doc); return true;
}
struct FOutput
{
 using Ch=char; TArray<uint8> Bytes; bool Overflow=false;
 void Put(char C) { if (Bytes.Num()<OutputLimit) Bytes.Add(static_cast<uint8>(C)); else Overflow=true; }
 void Flush() {}
};
inline void PutReserve(FOutput&,size_t) {}
inline void PutUnsafe(FOutput& Out,char C) { Out.Put(C); }
using FWriter=rapidjson::Writer<FOutput,rapidjson::UTF8<>,rapidjson::UTF8<>,VaRest::Json::FWriterAllocator>;
void String(FWriter& W,const FString& Value) { FTCHARToUTF8 Utf8(*Value,Value.Len()); W.String(Utf8.Get(),Utf8.Length()); }
void String(FWriter& W,const char* Key,const FString& Value) { W.Key(Key); String(W,Value); }
void Id(FWriter& W,const char* Key,uint64 Value) { String(W,Key,LexToString(Value)); }
void Bool(FWriter& W,const char* Key,bool Value) { W.Key(Key); W.Bool(Value); }
void Number(FWriter& W,const char* Key,double Value) { W.Key(Key); W.Double(FMath::IsFinite(Value) ? Value : 0); }
const TCHAR* Phrase(int32 Status)
{
 struct FPair { int32 Code; const TCHAR* Text; };
 static const FPair Phrases[]={{100,TEXT("Continue")},{101,TEXT("Switching Protocols")},{103,TEXT("Early Hints")},{200,TEXT("OK")},{201,TEXT("Created")},{202,TEXT("Accepted")},{203,TEXT("Non-Authoritative Information")},{204,TEXT("No Content")},{205,TEXT("Reset Content")},{206,TEXT("Partial Content")},{300,TEXT("Multiple Choices")},{301,TEXT("Moved Permanently")},{302,TEXT("Found")},{303,TEXT("See Other")},{304,TEXT("Not Modified")},{307,TEXT("Temporary Redirect")},{308,TEXT("Permanent Redirect")},{400,TEXT("Bad Request")},{401,TEXT("Unauthorized")},{402,TEXT("Payment Required")},{403,TEXT("Forbidden")},{404,TEXT("Not Found")},{405,TEXT("Method Not Allowed")},{406,TEXT("Not Acceptable")},{407,TEXT("Proxy Authentication Required")},{408,TEXT("Request Timeout")},{409,TEXT("Conflict")},{410,TEXT("Gone")},{411,TEXT("Length Required")},{412,TEXT("Precondition Failed")},{413,TEXT("Content Too Large")},{414,TEXT("URI Too Long")},{415,TEXT("Unsupported Media Type")},{416,TEXT("Range Not Satisfiable")},{417,TEXT("Expectation Failed")},{421,TEXT("Misdirected Request")},{422,TEXT("Unprocessable Content")},{426,TEXT("Upgrade Required")},{428,TEXT("Precondition Required")},{429,TEXT("Too Many Requests")},{431,TEXT("Request Header Fields Too Large")},{451,TEXT("Unavailable For Legal Reasons")},{500,TEXT("Internal Server Error")},{501,TEXT("Not Implemented")},{502,TEXT("Bad Gateway")},{503,TEXT("Service Unavailable")},{504,TEXT("Gateway Timeout")},{505,TEXT("HTTP Version Not Supported")},{511,TEXT("Network Authentication Required")}};
 for (const auto& P:Phrases) if (P.Code==Status) return P.Text;
 return TEXT("");
}
void Headers(FWriter& W,const FPacket& P)
{
 W.Key("headers"); W.StartArray(); const auto V=P.ViewOf();
 if (V.Fixed) for (const auto& Pair:V.Pairs(V.Fixed->Headers)) { W.StartObject(); String(W,"name",FString(V.Read(Pair.First))); String(W,"value",FString(V.Read(Pair.Second))); W.EndObject(); }
 W.EndArray(); W.Key("cookies"); W.StartArray(); W.EndArray(); Number(W,"headersSize",-1);
}
FString Header(const FPacket& P,const TCHAR* Name)
{
 const auto V=P.ViewOf(); if (V.Fixed) for (const auto& Pair:V.Pairs(V.Fixed->Headers)) if (FString(V.Read(Pair.First)).Equals(Name,ESearchCase::IgnoreCase)) return FString(V.Read(Pair.Second)); return {};
}
FString DecodeQuery(const FString& Input)
{
 FTCHARToUTF8 Utf8(*Input,Input.Len()); TArray<uint8> Bytes; Bytes.Reserve(Utf8.Length());
 const auto Hex=[](char C)->int32 { return C>='0' && C<='9' ? C-'0' : C>='a' && C<='f' ? C-'a'+10 : C>='A' && C<='F' ? C-'A'+10 : -1; };
 for (int32 I=0;I<Utf8.Length();++I)
 {
  const char C=Utf8.Get()[I];
  if (C=='%' && I+2<Utf8.Length() && Hex(Utf8.Get()[I+1])>=0 && Hex(Utf8.Get()[I+2])>=0) { Bytes.Add(uint8(Hex(Utf8.Get()[I+1])*16+Hex(Utf8.Get()[I+2]))); I+=2; }
  else Bytes.Add(C=='+' ? uint8(' ') : uint8(C));
 }
 return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num());
}
void Query(FWriter& W,const FString& Url)
{
 W.Key("queryString"); W.StartArray(); FString Before,QueryText;
 if (Url.Split(TEXT("?"),&Before,&QueryText))
 {
  FString Ignored; if (QueryText.Contains(TEXT("#"))) QueryText.Split(TEXT("#"),&QueryText,&Ignored);
  TArray<FString> Parts; QueryText.ParseIntoArray(Parts,TEXT("&"),false);
  for (const auto& Part:Parts) { FString Name,Value; if (!Part.Split(TEXT("="),&Name,&Value)) Name=Part; W.StartObject(); String(W,"name",DecodeQuery(Name)); String(W,"value",DecodeQuery(Value)); W.EndObject(); }
 }
 W.EndArray();
}
FString BodyText(const FPacket& P)
{ return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(P.Excerpt.GetData()),P.Excerpt.Num()); }
void BodyDescriptor(FWriter& W,const char* Key,const FPacket& P)
{
 W.Key(Key); W.StartObject(); Bool(W,"available",P.Present && P.Fixed.Scalars.ExcerptKind!=EVaRestExcerptKind::Skipped);
 if (P.Present)
 {
  const auto& S=P.Fixed.Scalars;
  Number(W,"kind",static_cast<uint8>(S.ExcerptKind)); Number(W,"totalBytes",double(S.TotalBytes));
  Number(W,"capturedBytes",S.ExcerptKind==EVaRestExcerptKind::Binary && !S.bBinaryCaptured ? 0 : P.Excerpt.Num());
  Bool(W,"truncated",S.bTruncated); Bool(W,"redacted",S.bRedacted); String(W,"sha256Prefix",P.Text(EText::Sha256Prefix));
  if (S.ExcerptKind==EVaRestExcerptKind::Binary)
  {
   Bool(W,"binaryCaptured",S.bBinaryCaptured);
   if (S.bBinaryCaptured) String(W,"base64",FBase64::Encode(P.Excerpt));
  }
 }
 W.EndObject();
}
bool ValidPhase(const FVaRestInspectorPhase& P) { return P.bKnown && FMath::IsFinite(P.Seconds) && P.Seconds>=0 && P.Seconds<=1e12; }
double Millis(const FVaRestInspectorPhase& P) { return ValidPhase(P) ? P.Seconds*1000 : 0; }
FEntry EntryFor(const FRecord& R,int32 Index,int32 Attempt)
{
 FEntry E; E.Record=Index; E.Attempt=Attempt; E.Utc=FDateTime(1970,1,1);
 if (!R.UtcKnown) return E;
 if (Attempt==INDEX_NONE) { E.Utc=R.Utc; E.UtcKnown=true; return E; }
 if (!R.StartKnown || R.Attempts[Attempt].Start<0) { E.Utc=R.Utc; return E; }
 const double Offset=R.Attempts[Attempt].Start-R.Start;
 const double Ticks=double(R.Utc.GetTicks())+Offset*ETimespan::TicksPerSecond;
 if (!FMath::IsFinite(Ticks) || Ticks<0 || Ticks>double(FDateTime::MaxValue().GetTicks())) { E.Utc=R.Utc; return E; }
 const int64 SafeTicks=int64(Ticks); if (SafeTicks<0 || SafeTicks>FDateTime::MaxValue().GetTicks()) { E.Utc=R.Utc; return E; }
 E.Utc=FDateTime(SafeTicks); E.UtcKnown=true; return E;
}
void WriteEntry(FWriter& W,const FDocument& Doc,const FEntry& E)
{
 const auto& R=Doc.Records[E.Record]; const FAttempt* A=E.Attempt==INDEX_NONE ? nullptr : &R.Attempts[E.Attempt];
 const auto& Request=A ? A->Request : R.Started; const auto& Response=A ? A->Response : R.Completed;
 const FPacket* Body=A ? &A->Body : nullptr;
 const int32 Status=A ? A->HasEnded ? A->Response.Fixed.Scalars.Status : 0 : R.Status;
 const double Blocked=A ? Millis(A->Auth)+Millis(A->Queued)+Millis(A->Prepare) : 0;
 const double Send=A ? Millis(A->Send) : 0,Wait=A ? Millis(A->Wait) : 0,Receive=A ? Millis(A->Receive) : 0;
 W.StartObject(); String(W,"startedDateTime",E.Utc.ToIso8601()); Number(W,"time",Blocked+Send+Wait+Receive);
 W.Key("request"); W.StartObject(); String(W,"method",R.Started.Text(EText::Verb)); String(W,"url",R.Started.Text(EText::Url)); String(W,"httpVersion",TEXT(""));
 Headers(W,Request); Query(W,R.Started.Text(EText::Url)); Number(W,"bodySize",double(R.Started.Fixed.Scalars.BodyBytes));
 if (R.RequestBody.Present && R.RequestBody.Fixed.Scalars.ExcerptKind!=EVaRestExcerptKind::Binary && R.RequestBody.Fixed.Scalars.ExcerptKind!=EVaRestExcerptKind::Skipped)
 {
  W.Key("postData"); W.StartObject(); const FString Mime=R.Started.Text(EText::BodyContentType); String(W,"mimeType",Mime); String(W,"text",BodyText(R.RequestBody));
  if (Mime.StartsWith(TEXT("application/x-www-form-urlencoded")))
  {
   W.Key("params"); W.StartArray(); TArray<FString> Params; BodyText(R.RequestBody).ParseIntoArray(Params,TEXT("&"),false);
   for (const auto& Param:Params) { FString Name,Value; if (!Param.Split(TEXT("="),&Name,&Value)) Name=Param; W.StartObject(); String(W,"name",DecodeQuery(Name)); String(W,"value",DecodeQuery(Value)); W.EndObject(); } W.EndArray();
  }
  W.EndObject();
 }
 W.EndObject();
 W.Key("response"); W.StartObject(); Number(W,"status",Status); String(W,"statusText",Phrase(Status)); String(W,"httpVersion",TEXT("")); Headers(W,Response);
 String(W,"redirectURL",Header(Response,TEXT("Location"))); Number(W,"bodySize",-1); W.Key("content"); W.StartObject();
 Number(W,"size",FMath::Max(0.0,Body && Body->Present ? double(Body->Fixed.Scalars.TotalBytes) : A ? double(A->Response.Fixed.Scalars.BytesDown) : double(R.Completed.Fixed.Scalars.DeliveredBodyBytes)));
 String(W,"mimeType",Response.Text(EText::ResponseContentType));
 if (Body && Body->Present && Body->Fixed.Scalars.ExcerptKind!=EVaRestExcerptKind::Binary && Body->Fixed.Scalars.ExcerptKind!=EVaRestExcerptKind::Skipped) String(W,"text",BodyText(*Body));
 W.EndObject(); W.EndObject(); W.Key("cache"); W.StartObject(); W.EndObject();
 W.Key("timings"); W.StartObject(); Number(W,"blocked",Blocked); Number(W,"dns",-1); Number(W,"connect",-1); Number(W,"ssl",-1); Number(W,"send",Send); Number(W,"wait",Wait); Number(W,"receive",Receive); W.EndObject();
 if (!Status) { FString Error=R.Completed.Text(EText::ErrorCode)+TEXT(" ")+R.Completed.Text(EText::ErrorMessage); if (Error.TrimStartAndEnd().IsEmpty()) Error=TEXT("No captured response"); String(W,"_error",Error); }
 if (!A && R.Completed.Fixed.Scalars.bFromCache) String(W,"_fromCache",R.Completed.Fixed.Scalars.CacheLookup==EVaRestTraceCacheLookup::HitDisk ? TEXT("disk") : TEXT("memory"));
 W.Key("_varest"); W.StartObject(); Id(W,"requestId",R.Id); Id(W,"scopeId",R.Scope); Id(W,"clientId",R.Client); String(W,"scope",R.ScopeLabel); String(W,"client",R.ClientLabel);
 Bool(W,"contextKnown",R.ContextKnown); Bool(W,"synthetic",!A); Bool(W,"startUtcKnown",E.UtcKnown); Number(W,"attempt",A ? A->Number : 0); Id(W,"dispatchId",A ? A->Dispatch : 0); Id(W,"observedAttempts",R.Observed); Id(W,"omittedAttempts",R.Omitted);
 String(W,"endpoint",R.Started.Fixed.Scalars.Endpoint.ToString()); String(W,"pathTemplate",R.Started.Text(EText::PathTemplate)); String(W,"tag",R.Started.Fixed.Scalars.Tag.ToString());
 String(W,"owner",R.Started.Text(EText::OwnerPath)); String(W,"scriptObject",R.Started.Text(EText::ScriptObject)); String(W,"scriptFunction",R.Started.Text(EText::ScriptFunction)); String(W,"sourceFile",R.Started.Text(EText::SourceFile)); Number(W,"sourceLine",R.Started.Fixed.Scalars.SourceLine); Number(W,"scriptOffset",R.Started.Fixed.Scalars.ScriptCodeOffset);
 Number(W,"outcome",static_cast<uint8>(R.State)); Number(W,"errorKind",static_cast<uint8>(R.Completed.Fixed.Scalars.ErrorKind)); String(W,"errorCode",R.Completed.Text(EText::ErrorCode)); String(W,"errorMessage",R.Completed.Text(EText::ErrorMessage));
 Bool(W,"finalHeadersKnown",A && A->HeadersKnown); Bool(W,"combinedSendWait",A && A->Combined); Bool(W,"fromCache",R.Completed.Fixed.Scalars.bFromCache); Number(W,"cacheLookup",static_cast<uint8>(R.Completed.Fixed.Scalars.CacheLookup));
 Number(W,"gap",A ? Millis(A->Gap) : 0); Number(W,"gapKind",A ? static_cast<uint8>(A->GapKind) : 0); Bool(W,"gapKnown",A && A->Gap.bKnown); Number(W,"wall",A ? Millis(A->Wall) : 0); Bool(W,"wallKnown",A && A->Wall.bKnown);
 W.Key("unknownTimings"); W.StartArray(); if (A) { const FVaRestInspectorPhase* P[]={&A->Auth,&A->Queued,&A->Prepare,&A->Send,&A->Wait,&A->Receive}; const char* Names[]={"auth","queued","prepare","send","wait","receive"}; for (int32 I=0;I<6;++I) if (!ValidPhase(*P[I])) W.String(Names[I]); } W.EndArray();
 BodyDescriptor(W,"requestBody",R.RequestBody); if (Body) BodyDescriptor(W,"responseBody",*Body);
 Id(W,"replayOfRequestId",R.Started.Fixed.Scalars.OriginalRequestId); String(W,"queueId",R.Started.Text(EText::QueueId)); Number(W,"decodeStartAt",R.Completed.Fixed.Scalars.DecodeStartAt); Number(W,"decodeEndAt",R.Completed.Fixed.Scalars.DecodeEndAt); String(W,"resultType",R.Completed.Fixed.Scalars.ResultType.ToString());
 W.Key("notes"); W.StartArray(); for (const auto& N:R.Notes) { W.StartObject(); Number(W,"kind",static_cast<uint8>(N.Fixed.Scalars.NoteKind)); Number(W,"time",N.Fixed.Header.Time); Number(W,"attempt",N.Fixed.Scalars.Attempt); String(W,"reason",N.Fixed.Scalars.Reason.ToString()); Number(W,"delay",N.Fixed.Scalars.Delay); Number(W,"retryAfter",N.Fixed.Scalars.RetryAfter); String(W,"queueId",N.Text(EText::QueueId)); W.EndObject(); } W.EndArray();
 W.Key("redactedHeaderNames"); W.StartArray(); const auto RequestView=Request.ViewOf(); if (RequestView.Fixed) for (const auto& P:RequestView.Pairs(RequestView.Fixed->Headers)) if (FString(RequestView.Read(P.Second)).Contains(TEXT("***"))) String(W,FString(RequestView.Read(P.First))); W.EndArray();
 W.Key("decodeWarnings"); W.StartArray(); const auto C=R.Completed.ViewOf(); if (C.Fixed) for (const auto& Pair:C.Pairs(C.Fixed->Warnings)) { W.StartObject(); String(W,"path",FString(C.Read(Pair.First))); String(W,"message",FString(C.Read(Pair.Second))); W.EndObject(); } W.EndArray(); Number(W,"decodeWarningCount",R.Completed.Fixed.Scalars.DecodeWarningCount);
 W.Key("envelope"); W.StartObject(); Bool(W,"applied",R.Completed.Fixed.Scalars.bEnvelopeApplied); String(W,"dataPath",R.Completed.Text(EText::DataPath)); String(W,"predicatePath",R.Completed.Text(EText::PredicatePath)); String(W,"expected",R.Completed.Text(EText::Expected)); String(W,"actual",R.Completed.Text(EText::Actual)); String(W,"code",R.Completed.Text(EText::EnvelopeCode)); String(W,"message",R.Completed.Text(EText::EnvelopeMessage)); W.EndObject();
 W.Key("links"); W.StartArray(); for (const auto& L:Doc.Links) if (L.FromRequestId==R.Id) { W.StartObject(); Id(W,"requestId",L.ToRequestId); String(W,"queueId",L.QueueId); Bool(W,"auth",L.bAuth); Bool(W,"targetEvicted",L.bTargetEvicted); W.EndObject(); } W.EndArray();
 W.EndObject(); W.EndObject();
}
#include "VaRestInspectorSocketExport.inl"
FVaRestInspectorExportResult WriteDocument(FString Path,TUniquePtr<FDocument> Doc)
{
 TArray<FEntry> Entries;
 for (int32 I=0;I<Doc->Records.Num();++I)
 {
  const auto& R=Doc->Records[I];
  if (R.Kind==EVaRestInspectorRecordKind::Socket) AddSocketEntries(R,I,Entries);
  else if (R.Attempts.IsEmpty()) Entries.Add(EntryFor(R,I,INDEX_NONE));
  else for (int32 A=0;A<R.Attempts.Num();++A) Entries.Add(EntryFor(R,I,A));
 }
 Entries.StableSort([](const FEntry& A,const FEntry& B) { return A.Utc<B.Utc; });
 FOutput Out; FWriter W(Out); W.StartObject(); W.Key("log"); W.StartObject(); W.Key("version"); W.String("1.2"); W.Key("creator"); W.StartObject(); W.Key("name"); W.String("VaRest"); W.Key("version"); W.String("2.0"); W.EndObject();
 W.Key("entries"); W.StartArray(); for (const auto& E:Entries) { if (Doc->Records[E.Record].Kind==EVaRestInspectorRecordKind::Socket) WriteSocketEntry(W,*Doc,E); else WriteEntry(W,*Doc,E); if (Out.Overflow) break; } W.EndArray();
 W.Key("_varest"); W.StartObject(); Id(W,"revision",Doc->Revision); Bool(W,"socketExportRequested",Doc->SocketsRequested); Bool(W,"socketCaptureAvailable",true); W.Key("statusTextSource"); W.String("synthesized HTTP status phrases"); Number(W,"droppedEvents",double(Doc->Diagnostics.DroppedEvents)); Number(W,"metadataOmitted",double(Doc->Diagnostics.MetadataOmitted)); W.EndObject(); W.EndObject(); W.EndObject();
 if (Out.Overflow || !W.IsComplete()) return MakeError(FString(TEXT("HAR exceeds the 64 MiB output limit.")));
 Doc.Reset(); Entries.Reset();
 const FString Temporary=Path+TEXT(".")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT(".tmp");
 if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path),true) || !FFileHelper::SaveArrayToFile(Out.Bytes,*Temporary))
 { IFileManager::Get().Delete(*Temporary,false,false,true); return MakeError(FString(TEXT("Could not write the HAR temporary file."))); }
 if (!IFileManager::Get().Move(*Path,*Temporary,true,false,false,true))
 { IFileManager::Get().Delete(*Temporary,false,false,true); return MakeError(FString(TEXT("Could not publish the HAR file."))); }
 return MakeValue(MoveTemp(Path));
}
#endif
} // namespace
void StartOnGT() { check(IsInGameThread()); Running=true; }
#if WITH_DEV_AUTOMATION_TESTS
void SetWorkerGateOnGT(TSharedPtr<FWorkerGate,ESPMode::ThreadSafe> Gate)
{ check(IsInGameThread()); WorkerGate=MoveTemp(Gate); }
#endif
void StopOnGT()
{
 check(IsInGameThread()); Running=false;
 for (auto& Job:Jobs) Job.Wait(); Jobs.Reset(); // workers never wait for GT or retain capture snapshots
}
FVaRestInspectorExportTask BeginOnGT(IVaRestInspector& Inspector,FString Path,const FVaRestInspectorExportOptions& Options)
{
 check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
 if (!Running) return Failed(TEXT("Inspector export is stopped."));
 Jobs.RemoveAll([](const auto& Job) { return Job.IsCompleted(); });
 if (Jobs.Num()>=2) return Failed(TEXT("Two Inspector exports are already running."));
 if (Path.IsEmpty() || Path.Len()>8192 || Path.Contains(TEXT("\r")) || Path.Contains(TEXT("\n"))) return Failed(TEXT("A valid HAR destination path is required."));
 for (TCHAR C:Path) if (C==0) return Failed(TEXT("The HAR destination contains NUL."));
 Path=FPaths::ConvertRelativePathToFull(Path);
 TUniquePtr<FDocument> Doc; FString Error;
 if (!MakeDocument(Inspector,Options,Doc,Error)) return UE::Tasks::MakeCompletedTask<FVaRestInspectorExportResult>(MakeError(MoveTemp(Error)));
 auto Job=UE::Tasks::Launch(UE_SOURCE_LOCATION,[Path=MoveTemp(Path),Doc=MoveTemp(Doc)
#if WITH_DEV_AUTOMATION_TESTS
 ,Gate=WorkerGate
#endif
 ]() mutable
 {
#if WITH_DEV_AUTOMATION_TESTS
  if (Gate)
  {
   Gate->Entered.fetch_add(1);
   const double Deadline=FPlatformTime::Seconds()+10;
   while (!Gate->Released.load() && FPlatformTime::Seconds()<Deadline) FPlatformProcess::SleepNoStats(0.001f);
   if (!Gate->Released.load()) return FVaRestInspectorExportResult(MakeError(FString(TEXT("Inspector test worker gate timed out."))));
  }
#endif
  return WriteDocument(MoveTemp(Path),MoveTemp(Doc));
 });
 Jobs.Add(Job); return Job;
#else
 return Failed(TEXT("Inspector capture/export is unavailable in this build."));
#endif
}
} // namespace VaRest::Inspector::Export
