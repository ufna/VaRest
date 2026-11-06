// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// GTK presentation only. Borrowed frame/attempt readers never escape this call.
int64 FVaRestInspectorFrameRowModel::PresentationBytes() const
{
 int64 Bytes=sizeof(*this)+128;
 for (const auto* Value:{&Channel,&Route,&MessageId,&Protocol,&Searchable}) Bytes+=Value->GetAllocatedSize();
 for (const auto& Value:Columns) Bytes+=int64(Value.ToString().Len()+1)*sizeof(TCHAR);
 return Bytes;
}
bool FVaRestInspectorViewModel::MatchesFrame(const FVaRestInspectorFrameRowModel& F) const
{
 if (!F.bSynthetic && F.Id==RevealedFrame) return true;
 if (!F.bSynthetic && bHideControl && F.MessageKind==EVaRestTraceSocketMessageKind::Control) return false;
 for (FString Term:FrameTerms)
 {
  const bool Not=Term.RemoveFromStart(TEXT("-")); FString Key,Value; bool Match=false;
  if (!Term.Split(TEXT(":"),&Key,&Value)) Match=F.Searchable.Contains(Term);
  else
  {
   Key.ToLowerInline();
   if (Key==TEXT("route")) Match=F.Route.Contains(Value);
   else if (Key==TEXT("ns") || Key==TEXT("namespace") || Key==TEXT("channel")) Match=F.Channel.Contains(Value);
   else if (Key==TEXT("id") || Key==TEXT("msgid")) Match=F.MessageId.Equals(Value,ESearchCase::CaseSensitive) || LexToString(F.Id)==Value;
   else if (Key==TEXT("dir")) Match=!F.bSynthetic && ((Value.Equals(TEXT("in"),ESearchCase::IgnoreCase) && F.Direction==EVaRestTraceSocketDirection::In) || (Value.Equals(TEXT("out"),ESearchCase::IgnoreCase) && F.Direction==EVaRestTraceSocketDirection::Out));
   else if (Key==TEXT("kind")) Match=F.bSynthetic ? F.Columns[3].ToString().Contains(Value) : Value.Equals(TEXT("binary"),ESearchCase::IgnoreCase) ? F.FrameKind==EVaRestTraceSocketFrameKind::Binary : Value.Equals(TEXT("text"),ESearchCase::IgnoreCase) ? F.FrameKind==EVaRestTraceSocketFrameKind::Text : F.Columns[3].ToString().Contains(Value);
   else if (Key==TEXT("is")) Match=Value.Equals(TEXT("control"),ESearchCase::IgnoreCase) ? F.MessageKind==EVaRestTraceSocketMessageKind::Control : Value.Equals(TEXT("autopong"),ESearchCase::IgnoreCase) && F.bAutoPong;
   else Match=F.Searchable.Contains(Term);
  }
  if (Match==Not) return false;
 }
 return true;
}
void FVaRestInspectorViewModel::ReadSocketDetails(const FVaRestInspectorRecordView& Row)
{
 Detail={}; Detail.bSocket=true; Detail.Id=LexToString(Row.RequestId); Detail.Url=Copy(Row.Started.Text(EText::Url),8192);
 Detail.ScriptObject=Text(Row.Started,EText::ScriptObject); Detail.ScriptFunction=Text(Row.Started,EText::ScriptFunction); Detail.SourceFile=Text(Row.Started,EText::SourceFile);
 Detail.ScriptCodeOffset=Row.Started.Fixed ? Row.Started.Fixed->Scalars.ScriptCodeOffset : INDEX_NONE; Detail.SourceLine=Row.Started.Fixed ? Row.Started.Fixed->Scalars.SourceLine : 0;
 const bool Sio=Row.SocketProtocol==TEXT("sio") || Row.SocketProtocol==TEXT("SocketIO");
 Detail.Overview=FText::FromString(FString::Printf(TEXT("Socket #%llu · %s · %s%s\n%s\nScope %llu: %s\nName: %s · Entry: %s\nReady: %s · Released: %s · Partial capture: %s\n%llu observed attempts (%llu older details omitted)\nFrames: %llu in / %llu out · %d retained · %llu evicted · %llu omitted\nBytes: %llu up / %llu down\n%llu dropped frames · late excerpts: %llu evicted / %llu unknown\nInitiator: %s\n%s\n%s:%d"),Row.RequestId,*Row.SocketProtocol.ToString(),*Row.SocketState.ToString(),Row.SocketCloseCode ? *FString::Printf(TEXT(" %d"),Row.SocketCloseCode) : TEXT(""),*Detail.Url,Row.ScopeId,*Copy(Row.ScopeLabel),*Row.SocketName.ToString(),*Row.SocketEntry.ToString(),Row.bSocketReady ? TEXT("yes") : TEXT("no"),Row.bSocketReleased ? TEXT("yes") : TEXT("no"),Row.bPartial ? TEXT("yes") : TEXT("no"),Row.ObservedDispatches,Row.SocketAttemptsOmitted,Row.SocketFramesIn,Row.SocketFramesOut,Row.SocketFramesRetained,Row.SocketFramesEvicted,Row.SocketFramesOmitted,Row.WireBytesUp,Row.WireBytesDown,Row.SocketDroppedFrames,Row.SocketLateExcerptsEvicted,Row.SocketLateExcerptsUnknown,*Detail.ScriptObject,*Detail.ScriptFunction,*Detail.SourceFile,Detail.SourceLine));
 FString Connection=Detail.Overview.ToString()+FString::Printf(TEXT("\n\nFlow: %s · backpressure %s · pending %lld bytes\nRTT: %s\nFailure: %s\nObserved closed connection time: %s (open/uncaptured intervals are excluded)\n"),Row.bSocketReadPaused ? TEXT("read paused") : TEXT("read enabled"),Row.bSocketBackpressure ? TEXT("high") : TEXT("normal"),Row.SocketPendingBytes,Row.SocketRtt>=0 ? *FString::Printf(TEXT("%.3f ms"),Row.SocketRtt*1000) : TEXT("unknown"),*Row.SocketFailure.ToString(),Row.Total.bKnown ? *FString::Printf(TEXT("%.3f ms"),Row.Total.Seconds*1000) : TEXT("unknown"));
 TArray<TSharedPtr<FVaRestInspectorAttemptOption>> Options;
 auto AddAttempt=[&](uint64 Id,FString Label)
 {
  TSharedPtr<FVaRestInspectorAttemptOption> Item; for (const auto& Old:AttemptOptions) if (Old->Id==Id) { Item=Old; break; }
  if (!Item) Item=MakeShared<FVaRestInspectorAttemptOption>(); Item->Id=Id; Item->Label=FText::FromString(MoveTemp(Label)); Options.Add(MoveTemp(Item));
 };
 AddAttempt(0,TEXT("Latest retained connection attempt"));
 FString SelectedAttempt; uint64 Last=0; bool SelectionFound=false;
 Row.ForEachSocketAttempt([&](const FVaRestInspectorSocketAttemptView& A)
 {
  Last=A.AttemptId; AddAttempt(A.AttemptId,FString::Printf(TEXT("Connection attempt %llu%s"),A.AttemptId,A.bPartial ? TEXT(" · partial") : TEXT("")));
  FString Part=FString::Printf(TEXT("\nConnection attempt %llu%s\n"),A.AttemptId,A.bPartial ? TEXT(" (partial)") : TEXT(""));
  if (A.Connecting.Fixed)
  {
   const auto& S=A.Connecting.Fixed->Scalars; Part+=Text(A.Connecting,EText::Url)+TEXT("\nHandshake headers\n")+Headers(A.Connecting)+TEXT("Offered protocols: ");
   for (const auto& P:A.Connecting.Pairs(A.Connecting.Fixed->Protocols)) Part+=Copy(A.Connecting.Read(P.First),128)+TEXT(" ");
   Part+=FString::Printf(TEXT("\nGap: %.3f ms · kind %d\n"),S.GapSeconds*1000,static_cast<int32>(S.SocketGap));
  }
  if (A.Opened.Fixed) Part+=FString::Printf(TEXT("Open: connect %.3f ms · selected protocol %s\n"),A.Opened.Fixed->Scalars.SocketConnectMs,*Text(A.Opened,EText::SocketProtocol));
  if (A.Closed.Fixed)
  {
   const auto& S=A.Closed.Fixed->Scalars;
   const TCHAR* By[]={TEXT("client"),TEXT("server"),TEXT("transport"),TEXT("heartbeat"),TEXT("timeout")}; const int32 Index=static_cast<int32>(S.SocketCloseBy);
   Part+=FString::Printf(TEXT("Close %d by %s · clean %s · HTTP status %d\nReason: %s\nFailure: %s · error kind %d\nReconnect: %s in %.3f s · gave up %s · while read paused %s\n"),S.SocketCloseCode,Index<UE_ARRAY_COUNT(By) ? By[Index] : TEXT("unknown"),S.bSocketClean ? TEXT("yes") : TEXT("no"),S.Status,*Text(A.Closed,EText::SocketCloseReason),*S.FailureDetail.ToString(),static_cast<int32>(S.ErrorKind),S.bSocketReconnect ? TEXT("yes") : TEXT("no"),S.Delay,S.bSocketGaveUp ? TEXT("yes") : TEXT("no"),S.bSocketReadPaused ? TEXT("yes") : TEXT("no"));
  }
  if (!SelectedDispatch || SelectedDispatch==A.AttemptId) { SelectedAttempt=Part; SelectionFound=SelectedDispatch==A.AttemptId; }
  if (Connection.Len()<100000) Connection+=Part;
 });
 AttemptOptions=MoveTemp(Options); Detail.AttemptLabel=FText::FromString(SelectedDispatch && !SelectionFound ? TEXT("Selected connection attempt was evicted") : Last ? FString::Printf(TEXT("Connection attempt %llu"),SelectedDispatch ? SelectedDispatch : Last) : TEXT("No retained connection attempts"));
 if (SelectedDispatch && SelectionFound) Connection+=TEXT("\nSelected attempt\n")+SelectedAttempt;
 Connection+=FString::Printf(TEXT("\nChannels · %d retained · %llu older states omitted\nNamespace\tState\tPublic id\tRecovered\tLast reason\n"),Row.SocketChannelsRetained,Row.SocketChannelsOmitted);
 Row.ForEachSocketChannel([&](const FVaRestInspectorPacketView& P)
 {
  if (!P.Fixed || Connection.Len()>100000) return;
  const auto& S=P.Fixed->Scalars;
  const TCHAR* State=S.SocketChannelState==EVaRestTraceSocketChannelState::Up ? TEXT("Up") : S.SocketChannelState==EVaRestTraceSocketChannelState::Refused ? TEXT("Refused") : TEXT("Down");
  Connection+=Text(P,EText::SocketChannel)+TEXT("\t")+State+TEXT("\t")+Text(P,EText::SocketPublicId)+(S.bSocketRecovered ? TEXT("\tyes\t") : TEXT("\tno\t"))+S.Reason.ToString()+TEXT(" ")+Text(P,EText::SocketMessage)+TEXT("\n");
 });
 Connection+=TEXT("\nRecent connection / request / namespace notes\n");
 const TCHAR* Notes[]={TEXT("Send dropped"),TEXT("Request / ack ended"),TEXT("Flow"),TEXT("Backpressure"),TEXT("Channel"),TEXT("Ping")};
 for (const auto& Note:Row.Notes) if (Note.Fixed)
 {
  const auto& S=Note.Fixed->Scalars; const int32 K=static_cast<int32>(S.SocketNoteKind);
  if (Connection.Len()>=120000) break;
  Connection+=FString::Printf(TEXT("%s · attempt %llu · "),K<UE_ARRAY_COUNT(Notes) ? Notes[K] : TEXT("Note"),S.SocketAttemptId)+Text(Note,EText::SocketChannel)+TEXT(" ")+Text(Note,EText::SocketRoute)+TEXT(" id=")+Text(Note,EText::SocketMsgId)
   +FString::Printf(TEXT(" · outcome %d · error %d · latency %.3f ms · count %lld · "),static_cast<int32>(S.SocketOutcome),static_cast<int32>(S.ErrorKind),S.SocketLatency*1000,S.SocketCount)+Text(Note,EText::SocketMessage)+TEXT("\n");
 }
 Detail.Connection=FText::FromString(Connection.Left(131072));
 TSet<uint64> Seen; TArray<TSharedPtr<FVaRestInspectorFrameRowModel>> Next; int64 Bytes=0; int32 Hidden=0,Omitted=0; bool FrameFound=false;
 Row.ForEachSocketFrame([&](const FVaRestInspectorSocketFrameView& V)
 {
  if (!V.Metadata.Fixed) return; const auto& M=*V.Metadata.Fixed; const auto& S=M.Scalars;
  auto F=FrameCache.FindRef(V.FrameId); const uint64 ExcerptSeq=V.Excerpt.Fixed ? V.Excerpt.Fixed->Header.Seq : 0;
  if (!F || F->MetadataSeq!=M.Header.Seq || F->ExcerptSeq!=ExcerptSeq || F->ExcerptState!=V.ExcerptState || F->bMetadataOmitted!=M.Header.bMetadataOmitted)
  {
   if (!F) F=MakeShared<FVaRestInspectorFrameRowModel>(); F->Id=V.FrameId; F->AttemptId=V.AttemptId; F->MetadataSeq=M.Header.Seq; F->Clock=M.Header.Time; F->Direction=S.SocketDirection; F->FrameKind=S.SocketFrameKind; F->bAutoPong=S.bSocketAutoReplied; F->ExcerptSeq=ExcerptSeq; F->ExcerptState=V.ExcerptState; F->bMetadataOmitted=M.Header.bMetadataOmitted;
   F->Channel=Copy(V.Metadata.Text(EText::SocketChannel),256); F->Route=Copy(V.Metadata.Text(EText::SocketRoute),256); F->MessageId=Copy(V.Metadata.Text(EText::SocketMsgId),1024); F->Protocol=S.SocketProtocolTag.IsNone() ? FString() : S.SocketProtocolTag.ToString(); F->MessageKind=S.SocketMessageKind;
   F->bLinkable=!M.Header.bMetadataOmitted && !F->MessageId.IsEmpty() && V.Metadata.Text(EText::SocketMsgId).Len()<=1024 && V.Metadata.Text(EText::SocketChannel).Len()<=256;
   for (TCHAR C:F->MessageId) if (C==0) F->bLinkable=false;
   for (TCHAR C:F->Channel) if (C==0) F->bLinkable=false;
   FString Attachments; for (int32 I=0;I<S.AttachmentCount;++I) Attachments+=FString::Printf(TEXT("%s%lld"),I ? TEXT(", ") : TEXT(""),S.AttachmentSizes[I]);
   const TCHAR* Kinds[]={TEXT("Message"),TEXT("Request"),TEXT("Reply"),TEXT("Error reply"),TEXT("Control")}; const int32 K=static_cast<int32>(S.SocketMessageKind);
   const TCHAR* Lanes[]={TEXT("Bypass"),TEXT("Ready"),TEXT("Backlog")}; const int32 L=static_cast<int32>(S.SocketLane);
   FString Values[]={LexToString(V.FrameId),S.SocketDirection==EVaRestTraceSocketDirection::In ? FString(TEXT("In")) : FString(TEXT("Out")),FString::Printf(TEXT("%.6f"),M.Header.Time),S.SocketFrameKind==EVaRestTraceSocketFrameKind::Binary ? FString(TEXT("Binary")) : FString(TEXT("Text")),F->Channel,F->Route,F->MessageId,F->Protocol,LexToString(S.SocketBytes),M.Header.bMetadataOmitted ? FString(TEXT("unknown")) : FString(L<UE_ARRAY_COUNT(Lanes) ? Lanes[L] : TEXT("unknown")),Attachments};
   if (F->bAutoPong) Values[7]+=TEXT(" · auto-pong");
   if (!M.Header.bMetadataOmitted) Values[3]+=TEXT(" · ")+FString(K<UE_ARRAY_COUNT(Kinds) ? Kinds[K] : TEXT("unknown"));
   for (int32 I=0;I<UE_ARRAY_COUNT(Values);++I) F->Columns[I]=FText::FromString(MoveTemp(Values[I]));
   F->Searchable=F->Channel+TEXT(" ")+F->Route+TEXT(" ")+F->MessageId+TEXT(" ")+F->Protocol+TEXT(" ")+F->Columns[1].ToString()+TEXT(" ")+F->Columns[3].ToString();
  }
  if (Seen.Num()>=5000 || F->PresentationBytes()>8*1024*1024-Bytes) { ++Omitted; return; }
  Bytes+=F->PresentationBytes(); Seen.Add(V.FrameId); FrameCache.Add(V.FrameId,F);
  if (V.FrameId==SelectedFrame)
  {
   FrameFound=true;
   if (V.ExcerptState==EVaRestInspectorDetailState::Present) Detail.FrameSource=BodySource(V.Excerpt);
   const TCHAR* States[]={TEXT("absent"),TEXT("pending"),TEXT("present"),TEXT("backlog"),TEXT("discarded"),TEXT("timed out"),TEXT("capture stopped"),TEXT("evicted"),TEXT("no producer"),TEXT("metadata omitted")}; const int32 St=static_cast<int32>(V.ExcerptState);
   Detail.FrameOverview=FText::FromString(FString::Printf(TEXT("Frame #%llu · attempt %llu · %lld bytes · %s\nChannel %s · route %s · id %s · protocol %s\nExcerpt: %s%s\nAuto-replied: %s · lane %s\nReceived: %.6f · called: %.6f\nAttachments: %s"),V.FrameId,V.AttemptId,S.SocketBytes,S.SocketDirection==EVaRestTraceSocketDirection::In ? TEXT("in") : TEXT("out"),*F->Channel,*F->Route,*F->MessageId,*F->Protocol,St<UE_ARRAY_COUNT(States) ? States[St] : TEXT("unknown"),M.Header.bMetadataOmitted ? TEXT("; some metadata was not retained") : TEXT(""),S.bSocketAutoReplied ? TEXT("yes") : TEXT("no"),*F->Columns[9].ToString(),S.SocketReceivedAt,S.SocketCalledAt,*F->Columns[10].ToString()));
  }
  if ((Sio && bHideEngineIo && F->Protocol.StartsWith(TEXT("eio:")) && F->Id!=RevealedFrame) || !MatchesFrame(*F)) { ++Hidden; return; }
  Next.Add(F);
 });
 for (auto It=FrameCache.CreateIterator();It;++It) if (!Seen.Contains(It.Key())) It.RemoveCurrent();
 const auto Marker=[&](const FVaRestInspectorPacketView& P,FString MarkerKind,FString Description)
 {
  if (!P.Fixed || Next.Num()>=5200) return;
  TSharedPtr<FVaRestInspectorFrameRowModel> F;
  for (const auto& Old:FrameRows) if (Old->bSynthetic && Old->MetadataSeq==P.Fixed->Header.Seq && Old->AttemptId==P.Fixed->Scalars.SocketAttemptId && Old->Columns[3].ToString()==MarkerKind) { F=Old; break; }
  if (!F) F=MakeShared<FVaRestInspectorFrameRowModel>(); F->bSynthetic=true; F->Clock=P.Fixed->Header.bSnapshot ? -1 : P.Fixed->Header.Time; F->MetadataSeq=P.Fixed->Header.Seq;
  F->AttemptId=P.Fixed->Scalars.SocketAttemptId; F->Channel=Text(P,EText::SocketChannel); F->Route=Text(P,EText::SocketRoute); F->MessageId=Text(P,EText::SocketMsgId);
  F->Columns[0]=FText::FromString(TEXT("—")); F->Columns[2]=FText::FromString(F->Clock>=0 ? FString::Printf(TEXT("%.6f"),F->Clock) : FString(TEXT("unknown"))); F->Columns[3]=FText::FromString(MarkerKind);
  F->Columns[4]=FText::FromString(F->Channel); F->Columns[5]=FText::FromString(F->Route); F->Columns[6]=FText::FromString(F->MessageId); F->Columns[7]=FText::FromString(Description.Left(256));
  F->MarkerDescription=FText::FromString(MoveTemp(Description)); F->Searchable=MarkerKind+TEXT(" ")+F->MarkerDescription.ToString()+TEXT(" ")+F->Channel+TEXT(" ")+F->Route+TEXT(" ")+F->MessageId;
  if (MatchesFrame(*F)) Next.Add(MoveTemp(F)); else ++Hidden;
 };
 Row.ForEachSocketAttempt([&](const FVaRestInspectorSocketAttemptView& A)
 {
  if (A.bHasConnecting) Marker(A.Connecting,TEXT("Attempt"),FString::Printf(TEXT("Connection attempt %llu started%s"),A.AttemptId,A.bPartial ? TEXT(" (partial)") : TEXT("")));
  if (A.bHasOpened) Marker(A.Opened,TEXT("Open"),FString::Printf(TEXT("Connection attempt %llu opened"),A.AttemptId));
  if (A.bHasClosed && A.Closed.Fixed) Marker(A.Closed,TEXT("Close"),FString::Printf(TEXT("Attempt %llu closed %d · "),A.AttemptId,A.Closed.Fixed->Scalars.SocketCloseCode)+Text(A.Closed,EText::SocketCloseReason));
 });
 for (const auto& P:Row.Notes) if (P.Fixed)
 {
  const auto& S=P.Fixed->Scalars;
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::SendDropped)
  {
   const TCHAR* Reasons[]={TEXT("volatile / not ready"),TEXT("queue overflow"),TEXT("rejected"),TEXT("stale pin"),TEXT("send limit")}; const int32 Reason=static_cast<int32>(S.SocketDropReason);
   Marker(P,TEXT("Dropped"),FString::Printf(TEXT("%lld sends dropped · %s"),S.SocketCount,Reason<UE_ARRAY_COUNT(Reasons) ? Reasons[Reason] : TEXT("unknown")));
  }
  else if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::RequestEnded)
  {
   const TCHAR* Outcomes[]={TEXT("replied"),TEXT("error reply"),TEXT("timed out"),TEXT("connection ended"),TEXT("canceled")}; const int32 O=static_cast<int32>(S.SocketOutcome);
   Marker(P,Sio ? TEXT("ACK ended") : TEXT("Request ended"),Text(P,EText::SocketMsgId)+TEXT(" · ")+(O<UE_ARRAY_COUNT(Outcomes) ? Outcomes[O] : TEXT("unknown"))+FString::Printf(TEXT(" after %.3f ms"),S.SocketLatency*1000));
  }
 }
 Next.StableSort([](const auto& A,const auto& B) { return A->Clock==B->Clock ? A->MetadataSeq<B->MetadataSeq : A->Clock<B->Clock; });
 FrameRows=MoveTemp(Next);
 if (SelectedFrame && !FrameFound) Detail.FrameOverview=FText::FromString(TEXT("The selected frame was evicted or cleared."));
 if (!SelectedFrame) Detail.FrameOverview=SelectedMarker.IsEmpty() ? FText::FromString(TEXT("Select a frame to inspect its captured excerpt.")) : SelectedMarker;
 if (const auto F=FrameCache.FindRef(SelectedFrame); F && F->bLinkable)
 {
  for (const auto& Pair:FrameCache)
  {
   const auto& Other=*Pair.Value; if (Other.Id==F->Id || !Other.bLinkable || Other.AttemptId!=F->AttemptId || !Other.Channel.Equals(F->Channel,ESearchCase::CaseSensitive) || !Other.MessageId.Equals(F->MessageId,ESearchCase::CaseSensitive) || Other.Direction==F->Direction) continue;
   const bool Reply=F->MessageKind==EVaRestTraceSocketMessageKind::Reply || F->MessageKind==EVaRestTraceSocketMessageKind::ErrorReply;
   const bool OtherReply=Other.MessageKind==EVaRestTraceSocketMessageKind::Reply || Other.MessageKind==EVaRestTraceSocketMessageKind::ErrorReply;
   if ((Reply && Other.MessageKind==EVaRestTraceSocketMessageKind::Request) || (OtherReply && F->MessageKind==EVaRestTraceSocketMessageKind::Request))
   {
    const auto Current=FrameCache.FindRef(Detail.LinkedFrameId);
    if (!Current || FMath::Abs(Other.Clock-F->Clock)<FMath::Abs(Current->Clock-F->Clock) || (FMath::Abs(Other.Clock-F->Clock)==FMath::Abs(Current->Clock-F->Clock) && Other.Id<Current->Id)) Detail.LinkedFrameId=Other.Id;
   }
  }
 }
 if (const auto F=FrameCache.FindRef(SelectedFrame); F && F->bLinkable && ((F->Direction==EVaRestTraceSocketDirection::Out && F->MessageKind==EVaRestTraceSocketMessageKind::Request) || (F->Direction==EVaRestTraceSocketDirection::In && (F->MessageKind==EVaRestTraceSocketMessageKind::Reply || F->MessageKind==EVaRestTraceSocketMessageKind::ErrorReply))))
 {
  const FVaRestInspectorPacketView* End=nullptr;
  for (const auto& P:Row.Notes) if (P.Fixed && P.Fixed->Scalars.SocketNoteKind==EVaRestTraceSocketNoteKind::RequestEnded && P.Fixed->Scalars.SocketAttemptId==F->AttemptId && P.Text(EText::SocketChannel).Equals(FStringView(F->Channel),ESearchCase::CaseSensitive) && P.Text(EText::SocketMsgId).Equals(FStringView(F->MessageId),ESearchCase::CaseSensitive) && (!End || P.Fixed->Header.Seq>End->Fixed->Header.Seq)) End=&P;
  if (End)
  {
   const auto& S=End->Fixed->Scalars; const TCHAR* Outcomes[]={TEXT("replied"),TEXT("error reply"),TEXT("timed out"),TEXT("connection ended"),TEXT("canceled")}; const int32 Outcome=static_cast<int32>(S.SocketOutcome);
   const FString Duration=FMath::IsFinite(S.SocketLatency) && S.SocketLatency>=0 ? FString::Printf(TEXT("%.3f ms"),S.SocketLatency*1000) : FString(TEXT("unknown"));
   Detail.FrameOverview=FText::FromString(Detail.FrameOverview.ToString()+(Sio ? TEXT("\nACK: ") : TEXT("\nRequest: "))+(Outcome<UE_ARRAY_COUNT(Outcomes) ? Outcomes[Outcome] : TEXT("unknown"))+TEXT(" · latency ")+Duration);
  }
 }

 Detail.FrameStatus=FText::FromString(FString::Printf(TEXT("%d frames retained · %d shown · %d hidden · %d omitted by view limits · %llu older frames evicted"),Row.SocketFramesRetained,FrameRows.Num(),Hidden,Omitted,Row.SocketFramesEvicted));
}
