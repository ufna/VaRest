// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Native owned export DTOs only; no snapshot readers or GTK state reach this code.
bool SourceTimeKnown(const FPacket& P)
{ return P.Present && !P.Fixed.Header.bSnapshot && FMath::IsFinite(P.Fixed.Header.Time) && P.Fixed.Header.Time>=0; }
bool SocketUtc(const FRecord& R,double Clock,FDateTime& Utc)
{
 if (!R.UtcKnown || !R.StartKnown || !FMath::IsFinite(Clock) || Clock<0) return false;
 const double Ticks=double(R.Utc.GetTicks())+(Clock-R.Start)*ETimespan::TicksPerSecond;
 if (!FMath::IsFinite(Ticks) || Ticks<0 || Ticks>double(FDateTime::MaxValue().GetTicks())) return false;
 const int64 SafeTicks=int64(Ticks);
 if (SafeTicks<0 || SafeTicks>FDateTime::MaxValue().GetTicks()) return false;
 Utc=FDateTime(SafeTicks); return true;
}
FEntry SocketEntryFor(const FRecord& R,int32 Record,int32 Attempt,uint64 AttemptId)
{
 FEntry E; E.Record=Record; E.Attempt=Attempt; E.SocketAttemptId=AttemptId; E.Utc=R.UtcKnown ? R.Utc : FDateTime(1970,1,1);
 if (Attempt!=INDEX_NONE)
 {
  const auto& A=R.SocketAttempts[Attempt];
  if (SourceTimeKnown(A.Connecting)) E.UtcKnown=SocketUtc(R,A.Connecting.Fixed.Header.Time,E.Utc);
 }
 return E;
}
void AddSocketEntries(const FRecord& R,int32 Record,TArray<FEntry>& Entries)
{
 TMap<uint64,int32> ByAttempt;
 for (int32 I=0;I<R.SocketAttempts.Num();++I)
 { const uint64 Id=R.SocketAttempts[I].Id; ByAttempt.Add(Id,Entries.Add(SocketEntryFor(R,Record,I,Id))); }
 for (int32 I=0;I<R.Frames.Num();++I)
 {
  const auto& F=R.Frames[I]; int32* Found=ByAttempt.Find(F.AttemptId);
  if (!Found) { const int32 Added=Entries.Add(SocketEntryFor(R,Record,INDEX_NONE,F.AttemptId)); ByAttempt.Add(F.AttemptId,Added); Found=ByAttempt.Find(F.AttemptId); }
  auto& E=Entries[*Found]; E.Frames.Add(I);
 }
 if (ByAttempt.IsEmpty()) { Entries.Add(SocketEntryFor(R,Record,INDEX_NONE,0)); return; }
 for (const auto& Pair:ByAttempt)
 {
  auto& E=Entries[Pair.Value];
  E.Frames.StableSort([&R](int32 L,int32 Other)
  {
   const auto& A=R.Frames[L].Metadata.Fixed.Header; const auto& B=R.Frames[Other].Metadata.Fixed.Header;
   if (A.Time!=B.Time) return A.Time<B.Time; return A.Seq<B.Seq;
  });
  // Orphan frames retain their own attempt identity; unknown connection times stay explicitly unknown.
  if (E.Attempt==INDEX_NONE && !E.Frames.IsEmpty())
  {
   const auto& P=R.Frames[E.Frames[0]].Metadata;
   if (SourceTimeKnown(P)) E.UtcKnown=SocketUtc(R,P.Fixed.Header.Time,E.Utc);
  }
 }
}
double MessageEpoch(const FRecord& R,const FPacket& P,bool& Known)
{
 FDateTime Utc; Known=SourceTimeKnown(P) && SocketUtc(R,P.Fixed.Header.Time,Utc);
 return Known ? double(Utc.GetTicks()-FDateTime(1970,1,1).GetTicks())/ETimespan::TicksPerSecond : 0;
}
bool HasFrameData(const FSocketFrame& F)
{
 return F.State==EVaRestInspectorDetailState::Present && F.Excerpt.Present && F.Excerpt.Fixed.Scalars.ExcerptKind!=EVaRestExcerptKind::Skipped
  && (F.Metadata.Fixed.Scalars.SocketFrameKind!=EVaRestTraceSocketFrameKind::Binary || F.Excerpt.Fixed.Scalars.bBinaryCaptured)
  && F.Excerpt.Fixed.Scalars.ExcerptLimit>0 && (F.Excerpt.Fixed.Scalars.TotalBytes==0 || !F.Excerpt.Excerpt.IsEmpty());
}
void SocketNote(FWriter& W,const FPacket& P)
{
 const auto& S=P.Fixed.Scalars; W.StartObject(); Number(W,"kind",static_cast<uint8>(S.SocketNoteKind));
 Id(W,"attemptId",S.SocketAttemptId); Number(W,"sourceTime",P.Fixed.Header.Time); Bool(W,"sourceTimeKnown",SourceTimeKnown(P));
 String(W,"channel",P.Text(EText::SocketChannel)); String(W,"route",P.Text(EText::SocketRoute)); String(W,"msgId",P.Text(EText::SocketMsgId));
 String(W,"message",P.Text(EText::SocketMessage)); String(W,"publicId",P.Text(EText::SocketPublicId)); String(W,"reason",S.Reason.ToString());
 Number(W,"channelState",static_cast<uint8>(S.SocketChannelState)); Bool(W,"recovered",S.bSocketRecovered);
 Number(W,"count",double(S.SocketCount)); Number(W,"dropReason",static_cast<uint8>(S.SocketDropReason)); Number(W,"outcome",static_cast<uint8>(S.SocketOutcome));
 Number(W,"errorKind",static_cast<uint8>(S.ErrorKind)); Number(W,"latency",S.SocketLatency); Number(W,"rtt",S.SocketRtt); Number(W,"pendingBytes",double(S.SocketPendingBytes));
 Bool(W,"paused",S.bSocketPaused); Bool(W,"high",S.bSocketHigh); Bool(W,"metadataOmitted",P.Fixed.Header.bMetadataOmitted); W.EndObject();
}
void SocketClose(FWriter& W,const FPacket& P)
{
 const auto& S=P.Fixed.Scalars; W.StartObject(); Bool(W,"observed",P.Present); Number(W,"code",S.SocketCloseCode);
 Number(W,"by",static_cast<uint8>(S.SocketCloseBy)); Bool(W,"clean",S.bSocketClean); String(W,"reason",P.Text(EText::SocketCloseReason));
 String(W,"failure",S.FailureDetail.ToString()); Number(W,"errorKind",static_cast<uint8>(S.ErrorKind)); Number(W,"httpStatus",S.Status);
 Bool(W,"reconnect",S.bSocketReconnect); Number(W,"delay",S.Delay); Bool(W,"gaveUp",S.bSocketGaveUp); Bool(W,"readPaused",S.bSocketReadPaused);
 Number(W,"sourceTime",P.Fixed.Header.Time); Bool(W,"sourceTimeKnown",SourceTimeKnown(P)); Bool(W,"metadataOmitted",P.Fixed.Header.bMetadataOmitted); W.EndObject();
}
void WriteSocketEntry(FWriter& W,const FDocument& Doc,const FEntry& E)
{
 const auto& R=Doc.Records[E.Record]; const FSocketAttempt* A=E.Attempt==INDEX_NONE ? nullptr : &R.SocketAttempts[E.Attempt];
 const FPacket Empty;
 const auto& Request=A && A->Connecting.Present ? A->Connecting : R.Started;
 const auto& Open=A ? A->Opened : Empty; const auto& Close=A ? A->Closed : Empty;
 const FString Url=Request.Text(EText::Url).IsEmpty() ? R.Started.Text(EText::Url) : Request.Text(EText::Url);
 const bool ConnectKnown=A && A->HasOpened && Open.Present && !Open.Fixed.Header.bSnapshot && FMath::IsFinite(Open.Fixed.Scalars.SocketConnectMs) && Open.Fixed.Scalars.SocketConnectMs>=0 && Open.Fixed.Scalars.SocketConnectMs<=1e12;
 const double Wait=ConnectKnown ? Open.Fixed.Scalars.SocketConnectMs : 0;
 const int32 Status=A && A->HasOpened ? 101 : A && A->HasClosed ? Close.Fixed.Scalars.Status : 0;
 W.StartObject(); String(W,"startedDateTime",E.Utc.ToIso8601()); Number(W,"time",Wait); String(W,"_resourceType",TEXT("websocket"));
 W.Key("request"); W.StartObject(); String(W,"method",TEXT("GET")); String(W,"url",Url); String(W,"httpVersion",TEXT(""));
 Headers(W,Request); Query(W,Url); Number(W,"bodySize",0); W.EndObject();
 W.Key("response"); W.StartObject(); Number(W,"status",Status); String(W,"statusText",Phrase(Status)); String(W,"httpVersion",TEXT(""));
 Headers(W,Empty); String(W,"redirectURL",TEXT("")); Number(W,"bodySize",-1); W.Key("content"); W.StartObject(); Number(W,"size",0); String(W,"mimeType",TEXT("")); W.EndObject(); W.EndObject();
 if (!Status) String(W,"_error",A && A->HasClosed ? FString(TEXT("WebSocket connection failed: "))+Close.Fixed.Scalars.FailureDetail.ToString() : FString(TEXT("No captured WebSocket handshake response")));
 W.Key("cache"); W.StartObject(); W.EndObject(); W.Key("timings"); W.StartObject();
 Number(W,"blocked",0); Number(W,"dns",-1); Number(W,"connect",-1); Number(W,"ssl",-1); Number(W,"send",0); Number(W,"wait",Wait); Number(W,"receive",0); W.EndObject();
 W.Key("_webSocketMessages"); W.StartArray();
 for (int32 Index:E.Frames)
 {
  const auto& F=R.Frames[Index]; const auto& S=F.Metadata.Fixed.Scalars; bool Known=false; const double Epoch=MessageEpoch(R,F.Metadata,Known);
  const bool Binary=S.SocketFrameKind==EVaRestTraceSocketFrameKind::Binary;
  FString Data; if (HasFrameData(F)) Data=Binary ? FBase64::Encode(F.Excerpt.Excerpt) : BodyText(F.Excerpt);
  W.StartObject(); String(W,"type",S.SocketDirection==EVaRestTraceSocketDirection::In ? TEXT("receive") : TEXT("send")); Number(W,"time",Epoch); Number(W,"opcode",Binary ? 2 : 1); String(W,"data",Data); W.EndObject();
 }
 if (A && A->HasClosed)
 {
  bool Known=false; const double Epoch=MessageEpoch(R,Close,Known);
  const FString Message=FString::Printf(TEXT("Close %d: "),Close.Fixed.Scalars.SocketCloseCode)+Close.Text(EText::SocketCloseReason)+TEXT(" ")+Close.Fixed.Scalars.FailureDetail.ToString();
  W.StartObject(); String(W,"type",TEXT("error")); Number(W,"time",Epoch); Number(W,"opcode",-1); String(W,"data",Message); W.EndObject();
 }
 W.EndArray();
 W.Key("_varest"); W.StartObject(); Id(W,"socketId",R.Id); Id(W,"attemptId",E.SocketAttemptId); Id(W,"scopeId",R.Scope); String(W,"scope",R.ScopeLabel); Bool(W,"contextKnown",R.ContextKnown);
 String(W,"name",R.SocketName.ToString()); String(W,"protocol",R.Protocol.ToString()); String(W,"entry",R.Entry.ToString()); String(W,"state",R.SocketState.ToString());
 Bool(W,"synthetic",!A); Bool(W,"attemptDetailsMissing",!A); Bool(W,"partial",R.Partial || (A && A->Partial)); Bool(W,"startUtcKnown",E.UtcKnown); Bool(W,"connectTimingKnown",ConnectKnown);
 Bool(W,"requestHeadersKnown",Request.Present && !Request.Fixed.Header.bMetadataOmitted && Request.Fixed.Headers.Count>0);
 Bool(W,"responseHeadersKnown",false); Bool(W,"ready",R.Ready); Bool(W,"released",R.Released); Id(W,"observedAttempts",R.Observed); Id(W,"omittedAttempts",R.Omitted);
 Id(W,"framesIn",R.FramesIn); Id(W,"framesOut",R.FramesOut); Id(W,"framesEvicted",R.FramesEvicted); Id(W,"framesOmitted",R.FramesOmitted); Id(W,"droppedFrames",R.DroppedFrames); Id(W,"bytesUp",R.Up); Id(W,"bytesDown",R.Down);
 String(W,"selectedProtocol",Open.Text(EText::SocketProtocol)); W.Key("offeredProtocols"); W.StartArray(); const auto P=Request.ViewOf();
 if (P.Fixed) for (const auto& Pair:P.Pairs(P.Fixed->Protocols)) String(W,FString(P.Read(Pair.First))); W.EndArray();
 Number(W,"gap",A ? A->Connecting.Fixed.Scalars.GapSeconds : 0); Number(W,"gapKind",A ? static_cast<uint8>(A->Connecting.Fixed.Scalars.SocketGap) : 0); Bool(W,"gapKnown",A && SourceTimeKnown(A->Connecting) && !A->Connecting.Fixed.Header.bMetadataOmitted);
 String(W,"owner",R.Started.Text(EText::OwnerPath)); String(W,"scriptObject",R.Started.Text(EText::ScriptObject)); String(W,"scriptFunction",R.Started.Text(EText::ScriptFunction)); String(W,"sourceFile",R.Started.Text(EText::SourceFile)); Number(W,"sourceLine",R.Started.Fixed.Scalars.SourceLine); Number(W,"scriptOffset",R.Started.Fixed.Scalars.ScriptCodeOffset);
 W.Key("close"); SocketClose(W,Close);
 W.Key("messages"); W.StartArray();
 for (int32 Index:E.Frames)
 {
  const auto& F=R.Frames[Index]; const auto& S=F.Metadata.Fixed.Scalars; bool Known=false; MessageEpoch(R,F.Metadata,Known);
  W.StartObject(); Id(W,"frameId",F.Id); Id(W,"attemptId",F.AttemptId); Bool(W,"utcKnown",Known); Number(W,"sourceTime",F.Metadata.Fixed.Header.Time); Bool(W,"sourceTimeKnown",SourceTimeKnown(F.Metadata));
  Bool(W,"metadataOmitted",F.Metadata.Fixed.Header.bMetadataOmitted); Number(W,"excerptState",static_cast<uint8>(F.State)); Bool(W,"dataAvailable",HasFrameData(F));
  String(W,"channel",F.Metadata.Text(EText::SocketChannel)); String(W,"route",F.Metadata.Text(EText::SocketRoute)); String(W,"msgId",F.Metadata.Text(EText::SocketMsgId)); String(W,"protocolTag",S.SocketProtocolTag.ToString());
  Number(W,"msgKind",static_cast<uint8>(S.SocketMessageKind)); Number(W,"lane",static_cast<uint8>(S.SocketLane)); Number(W,"bytes",double(S.SocketBytes)); Number(W,"calledAt",S.SocketCalledAt); Number(W,"receivedAt",S.SocketReceivedAt); Bool(W,"autoReplied",S.bSocketAutoReplied);
  if (HasFrameData(F) && S.SocketFrameKind==EVaRestTraceSocketFrameKind::Binary && !F.Excerpt.Excerpt.IsEmpty()) String(W,"dataEncoding",TEXT("base64"));
  W.Key("attachments"); W.StartArray(); for (int32 I=0;I<FMath::Clamp(S.AttachmentCount,0,10);++I) W.Int64(S.AttachmentSizes[I]); W.EndArray();
  BodyDescriptor(W,"body",F.Excerpt); W.EndObject();
 }
 if (A && A->HasClosed) { bool Known=false; MessageEpoch(R,Close,Known); W.StartObject(); String(W,"type",TEXT("error")); Bool(W,"utcKnown",Known); W.Key("close"); SocketClose(W,Close); W.EndObject(); }
 W.EndArray();
 W.Key("notes"); W.StartArray(); for (const auto& N:R.Notes) if (N.Fixed.Scalars.SocketAttemptId==E.SocketAttemptId) SocketNote(W,N); W.EndArray();
 // The census is the current socket state, distinct from the bounded per-attempt history above.
 Bool(W,"channelsAreCurrentSocketState",true); Id(W,"channelsOmitted",R.ChannelsOmitted); W.Key("channels"); W.StartArray(); for (const auto& C:R.Channels) SocketNote(W,C); W.EndArray();
 W.EndObject(); W.EndObject();
}
