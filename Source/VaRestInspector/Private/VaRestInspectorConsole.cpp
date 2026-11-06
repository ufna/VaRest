// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorConsole.h"
#include "VaRestInspector.h"
#include "VaRestInspectorContext.h"
#include "VaRestInspectorFilter.h"
#include "VaRestInspectorCurl.h"
#include "VaRestHub.h"
#include "Containers/Ticker.h"
#include "HAL/IConsoleManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"

DEFINE_LOG_CATEGORY_STATIC(LogVaRestInspector,Log,All);
namespace VaRest::Inspector::Console
{
#if VAREST_WITH_INSPECTOR
namespace
{
 bool Started=false;
 TArray<IConsoleObject*> Commands;
 TArray<FVaRestInspectorExportTask> Exports;
 FTSTicker::FDelegateHandle PollHandle;
 using EText=View::EText;
 TArray<FString> WithoutAll(const TArray<FString>& Args,bool& All)
 {
  TArray<FString> Out; All=false;
  for (const auto& Arg:Args) { if (Arg.Equals(TEXT("-all"),ESearchCase::IgnoreCase)) All=true; else Out.Add(Arg.Left(8192)); }
  return Out;
 }
 TOptional<uint64> Scope(UWorld* World,bool All)
 {
  if (All) return {};
  const auto* Hub=ExistingHub(World ? static_cast<const UObject*>(World) : static_cast<const UObject*>(GEngine));
  return Hub ? Hub->GetScopeId() : 0;
 }
 bool MatchesScope(const FVaRestInspectorRecordView& R,TOptional<uint64> Id)
 { return !Id.IsSet() || (R.bContextKnown && R.ScopeId==Id.GetValue()); }
 FString Text(const FVaRestInspectorPacketView& P,EText Field) { return FString(P.Text(Field)); }
 TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Snapshot()
 {
  TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Out; auto* Inspector=IVaRestInspector::TryGet();
  if (!Inspector || !Inspector->TrySnapshot(Out) || !Out) UE_LOG(LogVaRestInspector,Warning,TEXT("Snapshot unavailable under the current capture budget."));
  return Out;
 }
 void Capture(const TArray<FString>& Args,UWorld*)
 {
  int32 Enabled=0;
  if (Args.Num()!=1 || !LexTryParseString(Enabled,*Args[0]) || (Enabled!=0 && Enabled!=1)) { UE_LOG(LogVaRestInspector,Warning,TEXT("Usage: VaRest.Inspector.Capture 0|1")); return; }
  if (auto* Inspector=IVaRestInspector::TryGet()) { Inspector->SetCapture(Enabled!=0); UE_LOG(LogVaRestInspector,Display,TEXT("Inspector capture: %s"),Inspector->IsCapturing() ? TEXT("on") : TEXT("off")); }
 }
 void Clear(const TArray<FString>& Args,UWorld* World)
 {
  bool All=false; const auto Rest=WithoutAll(Args,All);
  if (!Rest.IsEmpty()) { UE_LOG(LogVaRestInspector,Warning,TEXT("Usage: VaRest.Inspector.Clear [-all]")); return; }
  if (auto* Inspector=IVaRestInspector::TryGet())
  {
   const auto Id=Scope(World,All);
   if (!Id.IsSet()) { Inspector->Clear(); UE_LOG(LogVaRestInspector,Display,TEXT("Cleared all Inspector scopes.")); }
   else if (Inspector->ClearScope(Id.GetValue())) { UE_LOG(LogVaRestInspector,Display,TEXT("Accepted clear for Inspector scope %llu; active requests may remain as partial records."),Id.GetValue()); }
   else { UE_LOG(LogVaRestInspector,Warning,TEXT("Scope clear was refused during nested control activity.")); }
  }
 }
 void List(const TArray<FString>& Args,UWorld* World)
 {
  bool All=false; auto Rest=WithoutAll(Args,All); int32 Limit=20,First=0;
  if (!Rest.IsEmpty() && LexTryParseString(Limit,*Rest[0])) { Limit=FMath::Clamp(Limit,1,1000); First=1; }
  FString QueryText;
  for (int32 I=First;I<Rest.Num();++I) QueryText+=(Rest[I].Contains(TEXT(" ")) ? TEXT("\"")+Rest[I]+TEXT("\"") : Rest[I])+TEXT(" ");
  FVaRestInspectorFilter Filter; Filter.SetText(QueryText); const auto Id=Scope(World,All);
  struct FLine { uint64 Arrival=0; FString Text; };
  TArray<FLine> Lines; Lines.Reserve(Limit); int32 Total=0;
  auto* Inspector=IVaRestInspector::TryGet(); if (!Inspector) return;
  const bool Read=Inspector->ForEachRecord([&](const FVaRestInspectorRecordView& R)
  {
   if (!MatchesScope(R,Id) || !Filter.Matches(R)) return; ++Total;
   FLine Line{R.ArrivalOrdinal,FString::Printf(TEXT("#%llu · scope %llu %s · client %llu · %s %s · status %d · %llu attempts · %.3f ms · %llu bytes"),R.RequestId,R.ScopeId,*FString(R.ScopeLabel),R.ClientId,*Text(R.Started,EText::Verb),*Text(R.Started,EText::Url),R.FinalStatus,R.ObservedDispatches,R.Total.bKnown ? R.Total.Seconds*1000 : -1,R.WireBytesDown)};
   if (R.Kind==EVaRestInspectorRecordKind::Socket) Line.Text=FString::Printf(TEXT("Socket #%llu · scope %s · %s %s · %s · %s · %llu attempts · %llu in/%llu out frames · %llu up/%llu down bytes · %llu dropped"),R.RequestId,R.bContextKnown ? *LexToString(R.ScopeId) : TEXT("unknown"),*R.SocketProtocol.ToString(),*R.SocketName.ToString(),*R.SocketState.ToString(),*Text(R.Started,EText::Url),R.ObservedDispatches,R.SocketFramesIn,R.SocketFramesOut,R.WireBytesUp,R.WireBytesDown,R.SocketDroppedFrames);
   if (R.Kind==EVaRestInspectorRecordKind::Marker) Line.Text=FString::Printf(TEXT("Marker · scope %llu · %s"),R.ScopeId,*Text(R.Marker,EText::MarkerText));
   if (Lines.Num()<Limit) Lines.Add(MoveTemp(Line));
   else
   {
    int32 Oldest=0; for (int32 I=1;I<Lines.Num();++I) if (Lines[I].Arrival<Lines[Oldest].Arrival) Oldest=I;
    if (Line.Arrival>Lines[Oldest].Arrival) Lines[Oldest]=MoveTemp(Line);
   }
  });
  if (!Read) { UE_LOG(LogVaRestInspector,Warning,TEXT("Capture is busy; retry List.")); return; }
  Lines.Sort([](const FLine& A,const FLine& B) { return A.Arrival<B.Arrival; });
  for (const auto& Line:Lines) UE_LOG(LogVaRestInspector,Display,TEXT("%s"),*Line.Text);
  UE_LOG(LogVaRestInspector,Display,TEXT("%d matches; showing the last %d%s."),Total,Lines.Num(),Id.IsSet() ? *FString::Printf(TEXT(" in scope %llu"),Id.GetValue()) : TEXT(" across all scopes"));
 }
 void Packet(const TCHAR* Caption,const FVaRestInspectorPacketView& P,bool FrameBody=false)
 {
  UE_LOG(LogVaRestInspector,Display,TEXT("%s%s"),Caption,P.Fixed ? TEXT("") : TEXT(" (unavailable)"));
  if (!P.Fixed) return;
  for (const auto& Pair:P.Pairs(P.Fixed->Headers)) UE_LOG(LogVaRestInspector,Display,TEXT("  %s: %s"),*FString(P.Read(Pair.First)),*FString(P.Read(Pair.Second)));
  if (P.Fixed->Header.Kind!=EVaRestTraceKind::Excerpt && !FrameBody) return;
  if (P.Fixed->Scalars.ExcerptKind==EVaRestExcerptKind::Binary) { UE_LOG(LogVaRestInspector,Display,TEXT("  Binary excerpt: %d captured / %lld total bytes"),P.Fixed->Scalars.bBinaryCaptured ? P.Excerpt.Num() : 0,P.Fixed->Scalars.TotalBytes); }
  else if (P.Fixed->Scalars.ExcerptKind!=EVaRestExcerptKind::Skipped)
  {
   const auto Bytes=P.Excerpt.Left(FrameBody ? 4096 : 65536); const FString Body=FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num());
   UE_LOG(LogVaRestInspector,Display,TEXT("%s%s"),*Body,P.Fixed->Scalars.bTruncated || P.Excerpt.Num()>Bytes.Num() ? TEXT("\n(excerpt truncated)") : TEXT(""));
  }
 }
 void ShowSocket(const FVaRestInspectorRecordView& R)
 {
  UE_LOG(LogVaRestInspector,Display,TEXT("Socket #%llu · scope %s · %s %s · %s · %s\n%llu attempts/%llu omitted · %llu in/%llu out frames/%d retained/%llu evicted/%llu omitted · %llu up/%llu down bytes · %llu dropped · partial %s\nInitiator %s %s %s:%d"),R.RequestId,R.bContextKnown ? *LexToString(R.ScopeId) : TEXT("unknown"),*R.SocketProtocol.ToString(),*R.SocketName.ToString(),*R.SocketState.ToString(),*Text(R.Started,EText::Url),R.ObservedDispatches,R.SocketAttemptsOmitted,R.SocketFramesIn,R.SocketFramesOut,R.SocketFramesRetained,R.SocketFramesEvicted,R.SocketFramesOmitted,R.WireBytesUp,R.WireBytesDown,R.SocketDroppedFrames,R.bPartial ? TEXT("yes") : TEXT("no"),*Text(R.Started,EText::ScriptObject),*Text(R.Started,EText::ScriptFunction),*Text(R.Started,EText::SourceFile),R.Started.Fixed ? R.Started.Fixed->Scalars.SourceLine : 0);
  R.ForEachSocketAttempt([&](const FVaRestInspectorSocketAttemptView& A)
  {
   UE_LOG(LogVaRestInspector,Display,TEXT("Connection attempt %llu · connecting/open/closed %d/%d/%d · partial %d"),A.AttemptId,A.bHasConnecting,A.bHasOpened,A.bHasClosed,A.bPartial);
   Packet(TEXT("Upgrade request headers"),A.Connecting);
   if (A.Opened.Fixed) { UE_LOG(LogVaRestInspector,Display,TEXT("  connect %.3f ms · selected protocol %s"),A.Opened.Fixed->Scalars.SocketConnectMs,*Text(A.Opened,EText::SocketProtocol)); }
   if (A.Closed.Fixed) { const auto& S=A.Closed.Fixed->Scalars; UE_LOG(LogVaRestInspector,Display,TEXT("  close %d by %d · clean %d · reason %s · failure %s · HTTP %d · reconnect %d / %.3f seconds"),S.SocketCloseCode,int32(S.SocketCloseBy),S.bSocketClean,*Text(A.Closed,EText::SocketCloseReason),*S.FailureDetail.ToString(),S.Status,S.bSocketReconnect,S.Delay); }
  });
  R.ForEachSocketChannel([&](const FVaRestInspectorPacketView& P)
  { if (P.Fixed) { const auto& S=P.Fixed->Scalars; UE_LOG(LogVaRestInspector,Display,TEXT("Namespace %s · state %d · public id %s · recovered %d · %s %s"),*Text(P,EText::SocketChannel),int32(S.SocketChannelState),*Text(P,EText::SocketPublicId),S.bSocketRecovered,*S.Reason.ToString(),*Text(P,EText::SocketMessage)); } });
  int32 Frames=0;
  R.ForEachSocketFrame([&](const FVaRestInspectorSocketFrameView& F)
  {
   if (++Frames>64 || !F.Metadata.Fixed) return; const auto& S=F.Metadata.Fixed->Scalars;
   UE_LOG(LogVaRestInspector,Display,TEXT("Frame #%llu · attempt %llu · %s/%s · %lld bytes · channel %s route %s id %s · protocol %s · excerpt state %d"),F.FrameId,F.AttemptId,S.SocketDirection==EVaRestTraceSocketDirection::In ? TEXT("in") : TEXT("out"),S.SocketFrameKind==EVaRestTraceSocketFrameKind::Binary ? TEXT("binary") : TEXT("text"),S.SocketBytes,*Text(F.Metadata,EText::SocketChannel),*Text(F.Metadata,EText::SocketRoute),*Text(F.Metadata,EText::SocketMsgId),*S.SocketProtocolTag.ToString(),int32(F.ExcerptState));
   if (F.ExcerptState==EVaRestInspectorDetailState::Present) Packet(TEXT("Captured frame excerpt"),F.Excerpt,true);
  });
  if (Frames>64) { UE_LOG(LogVaRestInspector,Display,TEXT("%d additional retained frames omitted from console output; use HAR for all retained frames."),Frames-64); }
 }
 void Show(const TArray<FString>& Args,UWorld* World)
 {
  bool All=false; const auto Rest=WithoutAll(Args,All); uint64 Request=0;
  FString Identity=Rest.Num()==1 ? Rest[0] : FString(); uint8 Kind=0;
  if (Identity.RemoveFromStart(TEXT("http:"))) Kind=1; else if (Identity.RemoveFromStart(TEXT("socket:"))) Kind=2;
  if (Rest.Num()!=1 || !LexTryParseString(Request,*Identity)) { UE_LOG(LogVaRestInspector,Warning,TEXT("Usage: VaRest.Inspector.Show <id|http:id|socket:id> [-all]")); return; }
  const auto Id=Scope(World,All); auto* Inspector=IVaRestInspector::TryGet(); if (!Inspector) return; bool Found=false;
  const bool Read=Inspector->ForEachRecord([&](const FVaRestInspectorRecordView& R)
  {
   if (R.Kind==EVaRestInspectorRecordKind::Marker || R.RequestId!=Request || !MatchesScope(R,Id)) return;
   if ((Kind==1 && R.Kind!=EVaRestInspectorRecordKind::Http) || (Kind==2 && R.Kind!=EVaRestInspectorRecordKind::Socket)) return;
   Found=true; if (R.Kind==EVaRestInspectorRecordKind::Socket) { ShowSocket(R); return; }
   UE_LOG(LogVaRestInspector,Display,TEXT("#%llu · scope %llu %s · client %llu %s\n%s %s\nStatus %d · %llu attempts · %llu up / %llu down\nInitiator %s %s %s:%d"),R.RequestId,R.ScopeId,*FString(R.ScopeLabel),R.ClientId,*FString(R.ClientLabel),*Text(R.Started,EText::Verb),*Text(R.Started,EText::Url),R.FinalStatus,R.ObservedDispatches,R.WireBytesUp,R.WireBytesDown,*Text(R.Started,EText::ScriptObject),*Text(R.Started,EText::ScriptFunction),*Text(R.Started,EText::SourceFile),R.Started.Fixed ? R.Started.Fixed->Scalars.SourceLine : 0);
   Packet(TEXT("As-built request headers"),R.Started);
   for (const auto& P:R.Excerpts) if (P.Fixed && P.Fixed->Scalars.Owner==EVaRestTraceExcerptOwner::Request) Packet(TEXT("Request excerpt"),P);
   for (const auto& A:R.Dispatches)
   {
    UE_LOG(LogVaRestInspector,Display,TEXT("Attempt %d · dispatch %llu · status %d · URL %s"),A.Attempt,A.DispatchId,A.Ended.Fixed ? A.Ended.Fixed->Scalars.Status : 0,*Text(A.Ended,EText::EffectiveUrl));
    Packet(TEXT("Dispatched request headers"),A.Dispatched); Packet(TEXT("Network response headers"),A.Ended); Packet(TEXT("Network response excerpt"),A.ResponseExcerpt);
    const FVaRestInspectorPhase* Phases[]={&A.Gap,&A.Auth,&A.Queued,&A.Prepare,&A.Send,&A.Wait,&A.Receive}; const TCHAR* Names[]={TEXT("Gap"),TEXT("Auth"),TEXT("Queued"),TEXT("Prepare"),TEXT("Send"),TEXT("Wait"),TEXT("Receive")};
    for (int32 I=0;I<7;++I) UE_LOG(LogVaRestInspector,Display,TEXT("  %s: %s"),Names[I],Phases[I]->bKnown ? *FString::Printf(TEXT("%.3f ms"),Phases[I]->Seconds*1000) : TEXT("unknown"));
   }
   Packet(TEXT("Delivered response headers (wire excerpts remain attached to their physical attempt)"),R.Completed);
   UE_LOG(LogVaRestInspector,Display,TEXT("Error: %s %s\nDecode %s · result %s\nEnvelope data=%s predicate=%s expected=%s actual=%s code=%s message=%s"),*Text(R.Completed,EText::ErrorCode),*Text(R.Completed,EText::ErrorMessage),R.Decode.bKnown ? *FString::Printf(TEXT("%.3f ms"),R.Decode.Seconds*1000) : TEXT("unknown"),R.Completed.Fixed ? *R.Completed.Fixed->Scalars.ResultType.ToString() : TEXT("unknown"),*Text(R.Completed,EText::DataPath),*Text(R.Completed,EText::PredicatePath),*Text(R.Completed,EText::Expected),*Text(R.Completed,EText::Actual),*Text(R.Completed,EText::EnvelopeCode),*Text(R.Completed,EText::EnvelopeMessage));
   if (R.Completed.Fixed) for (const auto& Warning:R.Completed.Pairs(R.Completed.Fixed->Warnings)) UE_LOG(LogVaRestInspector,Display,TEXT("Warning %s: %s"),*FString(R.Completed.Read(Warning.First)),*FString(R.Completed.Read(Warning.Second)));
  });
  if (!Read || !Found) UE_LOG(LogVaRestInspector,Warning,TEXT("Record is unavailable in this scope, evicted, or capture is busy. Use -all to search every scope."));
 }
 void Stats(const TArray<FString>& Args,UWorld* World)
 {
  bool All=false; const auto Rest=WithoutAll(Args,All); if (!Rest.IsEmpty()) { UE_LOG(LogVaRestInspector,Warning,TEXT("Usage: VaRest.Inspector.Stats [-all]")); return; }
  const auto Id=Scope(World,All); auto View=Snapshot(); if (!View) return;
  const auto Quantile=[](const FVaRestInspectorQuantile& Q)
  {
   if (!Q.bHasValue) return FString(TEXT("unknown"));
   if (Q.Kind==EVaRestInspectorQuantileKind::Zero) return FString(TEXT("0 ms"));
   return Q.Kind==EVaRestInspectorQuantileKind::LogInterval ? FString::Printf(TEXT("~%.3f ms [%.3f,%.3f)"),Q.Value*1000,Q.Lower*1000,Q.Upper*1000) : FString(TEXT("outside range / saturated"));
  };
  struct FLine { uint64 Count=0; FString Text; }; TArray<FLine> Top; uint64 StartedCount=0,Completed=0,Errors=0,Queued=0,Canceled=0,Dispatches=0,Retries=0,Auth=0; int32 Groups=0; bool Saturated=false;
  auto Add=[&](uint64& To,uint64 Value) { if (Value>MAX_uint64-To) { To=MAX_uint64; Saturated=true; } else To+=Value; };
  View->ForEachStats([&](const FVaRestInspectorHttpStats& G)
  {
   if (Id.IsSet() && (G.bOverflow || G.ScopeId!=Id.GetValue())) return; ++Groups; Saturated|=G.bSaturated;
   Add(StartedCount,G.Started); Add(Completed,G.Completed); Add(Errors,G.Error); Add(Queued,G.Queued); Add(Canceled,G.Canceled); Add(Dispatches,G.Dispatches); Add(Retries,G.Retries); Add(Auth,G.AuthReplays);
   const auto P50=G.Quantile(1,2),P95=G.Quantile(95,100),P99=G.Quantile(99,100);
   FLine Line{G.Completed,FString::Printf(TEXT("%s · scope %llu/client %llu · %llu completed/%llu errors · %llu attempts · p50 %s / p95 %s / p99 %s · cache %llu/%llu · wire %llu/%llu · partial %llu/incomplete %llu"),G.bOverflow ? TEXT("Overflow") : G.Endpoint.IsNone() ? *G.Key : *G.Endpoint.ToString(),G.ScopeId,G.ClientId,G.Completed,G.Error,G.Dispatches,*Quantile(P50),*Quantile(P95),*Quantile(P99),G.CacheHit,G.CacheDisk,G.WireBytesUp,G.WireBytesDown,G.Partial,G.Incomplete)};
   Line.Text+=FString::Printf(TEXT(" · errors: connection=%llu timeout=%llu HTTP=%llu server=%llu decode=%llu auth=%llu offline=%llu config=%llu canceled=%llu unknown=%llu"),G.ErrorsByKind.Connection,G.ErrorsByKind.Timeout,G.ErrorsByKind.Http,G.ErrorsByKind.Server,G.ErrorsByKind.Decode,G.ErrorsByKind.Auth,G.ErrorsByKind.Offline,G.ErrorsByKind.Config,G.ErrorsByKind.Canceled,G.ErrorsByKind.Unknown);
   int32 At=0; while (At<Top.Num() && Top[At].Count>=Line.Count) ++At; if (At<32) { Top.Insert(MoveTemp(Line),At); if (Top.Num()>32) Top.Pop(EAllowShrinking::No); }
  });
  TArray<FLine> SocketTop; int32 SocketGroups=0;
  View->ForEachSocketStats([&](const FVaRestInspectorSocketStats& G)
  {
   if (Id.IsSet() && (!G.bContextKnown || G.ScopeId!=Id.GetValue())) return; ++SocketGroups;
   FLine Line{G.FramesIn>MAX_uint64-G.FramesOut ? MAX_uint64 : G.FramesIn+G.FramesOut,FString::Printf(TEXT("Socket #%llu %s · scope %s · %s · %llu attempts/%llu reconnects · %llu opens/%llu closes/%llu failures · %llu in/%llu out frames · wire %llu/%llu · %llu drops · average RTT %s · partial %d%s"),G.SocketId,*G.Name.ToString(),G.bContextKnown ? *LexToString(G.ScopeId) : TEXT("unknown"),*G.Protocol.ToString(),G.Attempts,G.Reconnects,G.Opened,G.Closed,G.Failures,G.FramesIn,G.FramesOut,G.BytesUp,G.BytesDown,G.Drops,G.RttCount ? *FString::Printf(TEXT("%.3f ms"),G.RttSum/double(G.RttCount)*1000) : TEXT("unknown"),G.bPartial,G.bOverflow ? TEXT(" (unattributed overflow)") : G.bSaturated ? TEXT(" (saturated)") : TEXT(""))};
   int32 At=0; while (At<SocketTop.Num() && SocketTop[At].Count>=Line.Count) ++At;
   if (At<32) { SocketTop.Insert(MoveTemp(Line),At); if (SocketTop.Num()>32) SocketTop.Pop(EAllowShrinking::No); }
  });
  const auto D=View->GetDiagnostics(); View.Reset();
  UE_LOG(LogVaRestInspector,Display,TEXT("%d scope-selected groups; independent of row eviction/text filters. %llu started/%llu completed/%llu errors (%.1f%%)/%llu queued/%llu canceled · %llu attempts/%llu retries/%llu auth replays%s"),Groups,StartedCount,Completed,Errors,Completed ? double(Errors)*100/double(Completed) : 0,Queued,Canceled,Dispatches,Retries,Auth,Saturated ? TEXT(" (saturated)") : TEXT(""));
  for (const auto& Line:Top) UE_LOG(LogVaRestInspector,Display,TEXT("%s"),*Line.Text);
  UE_LOG(LogVaRestInspector,Display,TEXT("%d socket groups, showing top 32 by observed frames; counters survive row/frame eviction. Partial capture excludes unseen history; recent duplicate windows are bounded."),SocketGroups);
  for (const auto& Line:SocketTop) UE_LOG(LogVaRestInspector,Display,TEXT("%s"),*Line.Text);
  UE_LOG(LogVaRestInspector,Display,TEXT("Global capture: %lld/%lld bytes, peak %lld; pending %lld, dropped %llu events/%llu bytes, refused %llu rows, omitted %llu metadata. Scope stats exclude unattributed overflow unless -all."),D.StoreBytes,D.EffectiveStoreLimit,D.StorePeakBytes,D.PendingBytes,D.DroppedEvents,D.DroppedBytes,D.RefusedRows,D.MetadataOmitted);
 }
 bool PollExports(float)
 {
  for (int32 Pass=0;Started && Pass<2;++Pass)
  {
   const int32 Index=Exports.IndexOfByPredicate([](const auto& Job) { return Job.IsCompleted(); }); if (Index==INDEX_NONE) break;
   const auto& R=Exports[Index].GetResult(); const bool Success=R.HasValue(); const FString Message=Success ? R.GetValue() : R.HasError() ? R.GetError() : FString(TEXT("Export returned no result."));
   Exports.RemoveAtSwap(Index);
   if (Success) { UE_LOG(LogVaRestInspector,Display,TEXT("HAR saved: %s"),*Message); } else { UE_LOG(LogVaRestInspector,Warning,TEXT("HAR export failed: %s"),*Message); }
  }
  if (!Started || Exports.IsEmpty()) { PollHandle.Reset(); return false; } return true;
 }
 void Export(const TArray<FString>& Args,UWorld* World)
 {
  bool All=false; const auto Rest=WithoutAll(Args,All);
  if (Rest.Num()>1) { UE_LOG(LogVaRestInspector,Warning,TEXT("Usage: VaRest.Inspector.Export [path] [-all]")); return; }
  if (Exports.Num()>=2) { UE_LOG(LogVaRestInspector,Warning,TEXT("Two console exports are pending.")); return; }
  auto* Inspector=IVaRestInspector::TryGet(); if (!Inspector) return;
  FVaRestInspectorExportOptions Options; Options.ScopeId=Scope(World,All); Exports.Add(Inspector->ExportHar(Rest.IsEmpty() ? DefaultHarPath() : Rest[0],Options));
  if (!PollHandle.IsValid()) PollHandle=FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&PollExports),.05f);
 }
}
#endif
void StartOnGT()
{
 check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
 if (Started) return; Started=true;
 struct FCommand { const TCHAR* Name; const TCHAR* Help; void (*Function)(const TArray<FString>&,UWorld*); };
 const FCommand ListCommands[]={{TEXT("Capture"),TEXT("0|1: enable capture"),&Capture},{TEXT("Clear"),TEXT("[-all]: clear this world's scope or all scopes"),&Clear},{TEXT("List"),TEXT("[N] [filter...] [-all]: list retained rows"),&List},{TEXT("Show"),TEXT("<id> [-all]: show redacted request details"),&Show},{TEXT("Stats"),TEXT("[-all]: scope statistics, independent of retained rows"),&Stats},{TEXT("Export"),TEXT("[path] [-all]: export captured HTTP requests as HAR"),&Export}};
 for (const auto& C:ListCommands) Commands.Add(IConsoleManager::Get().RegisterConsoleCommand(*(TEXT("VaRest.Inspector.")+FString(C.Name)),C.Help,FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(C.Function),ECVF_Default));
#endif
}
void StopOnGT()
{
 check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
 Started=false; if (PollHandle.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(PollHandle); PollHandle.Reset(); Exports.Reset();
 for (auto* Command:Commands) if (Command) IConsoleManager::Get().UnregisterConsoleObject(Command); Commands.Reset();
#endif
}
}
