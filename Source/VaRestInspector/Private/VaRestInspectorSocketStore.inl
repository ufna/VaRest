// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Included inside VaRest::Inspector::Private after the HTTP store definitions.
namespace
{
EVaRestInspectorDetailState SocketExcerptState(const FPacketScalars& S)
{
 if (S.bExcerptPending) return EVaRestInspectorDetailState::Pending;
 if (S.ExcerptKind!=EVaRestExcerptKind::Skipped) return EVaRestInspectorDetailState::Present;
 switch (S.Skip)
 {
 case EVaRestExcerptSkip::Backlog:return EVaRestInspectorDetailState::Backlog;
 case EVaRestExcerptSkip::Discarded:return EVaRestInspectorDetailState::Discarded;
 case EVaRestExcerptSkip::Disabled:return EVaRestInspectorDetailState::CaptureStopped;
 default:return EVaRestInspectorDetailState::MetadataOmitted;
 }
}
FVaRestInspectorSocketFrameView SocketFrameView(const FSocketFrameSlot& Slot,uint64 SocketId,FPacketFixed& Fallback)
{
 FVaRestInspectorSocketFrameView View; View.FrameId=Slot.Id; View.AttemptId=Slot.AttemptId; View.ExcerptState=Slot.State;
 if (Slot.Metadata) View.Metadata=Slot.Metadata->View();
 else
 {
  Fallback={}; Fallback.Header.Kind=EVaRestTraceKind::SocketFrame; Fallback.Header.Source=EVaRestTraceSource::Socket; Fallback.Header.bMetadataOmitted=true;
  Fallback.Header.Seq=Slot.Seq; Fallback.Header.Time=Slot.Time; Fallback.Scalars.SocketId=SocketId; Fallback.Scalars.FrameId=Slot.Id; Fallback.Scalars.SocketAttemptId=Slot.AttemptId;
  Fallback.Scalars.SocketBytes=Slot.Bytes; Fallback.Scalars.SocketDirection=Slot.Direction; Fallback.Scalars.SocketFrameKind=Slot.Kind;
  Fallback.Scalars.ExcerptKind=EVaRestExcerptKind::Skipped; View.Metadata.Fixed=&Fallback;
 }
 if (Slot.Excerpt) View.Excerpt=Slot.Excerpt->View(); else if (Slot.Metadata && Slot.State!=EVaRestInspectorDetailState::Pending) View.Excerpt=View.Metadata;
 return View;
}
FVaRestInspectorSocketAttemptView SocketAttemptView(const FSocketAttemptSlot& Slot,uint64 SocketId,FPacketFixed (&Fallback)[3])
{
 FVaRestInspectorSocketAttemptView View; View.AttemptId=Slot.Id; View.bPartial=Slot.Partial;
 View.bHasConnecting=Slot.HasConnecting; View.bHasOpened=Slot.HasOpened; View.bHasClosed=Slot.HasClosed;
 const FStorePacketPtr* Owners[]={&Slot.Connecting,&Slot.Opened,&Slot.Closed};
 FVaRestInspectorPacketView* Packets[]={&View.Connecting,&View.Opened,&View.Closed};
 const bool Present[]={Slot.HasConnecting,Slot.HasOpened,Slot.HasClosed}; const double Times[]={Slot.ConnectingAt,Slot.OpenedAt,Slot.ClosedAt};
 for (int32 I=0;I<3;++I)
 {
  if (!Present[I]) continue;
  if (*Owners[I]) { *Packets[I]=(*Owners[I])->View(); continue; }
  auto& F=Fallback[I]; F={}; F.Header.Kind=EVaRestTraceKind::SocketAttempt; F.Header.Source=EVaRestTraceSource::Socket; F.Header.Time=Times[I]; F.Header.bMetadataOmitted=true; F.Header.bSnapshot=Slot.Partial;
  auto& S=F.Scalars; S.SocketId=SocketId; S.SocketAttemptId=Slot.Id; S.SocketAttemptPhase=static_cast<EVaRestTraceSocketAttemptPhase>(I); S.SocketConnectMs=Slot.ConnectMs;
  if (I==2) { S.SocketCloseCode=Slot.CloseCode; S.Status=Slot.HttpStatus; S.SocketCloseBy=Slot.CloseBy; S.bSocketClean=Slot.Clean; S.bSocketReconnect=Slot.Reconnect; S.bSocketGaveUp=Slot.GaveUp; S.bSocketReadPaused=Slot.ReadPaused; S.ErrorKind=Slot.ErrorKind; S.FailureDetail=Slot.Failure; S.Delay=Slot.Delay; }
  Packets[I]->Fixed=&F;
 }
 return View;
}
}
void FStore::VisitSocketAttempts(const void* Source,TFunctionRef<void(const FVaRestInspectorSocketAttemptView&)> Visitor)
{
 const auto& Row=*static_cast<const FRow*>(Source);
 for (int32 I=0;I<Row.NumSocketAttempts;++I) { FPacketFixed Fallback[3]; const auto View=SocketAttemptView(Row.SocketAttempts[I]->Value,Row.Key.Id,Fallback); Visitor(View); }
}
void FStore::VisitSocketFrames(const void* Source,TFunctionRef<void(const FVaRestInspectorSocketFrameView&)> Visitor)
{
 const auto& Row=*static_cast<const FRow*>(Source);
 for (int32 I=0;I<Row.SocketFrames.Num();++I) { FPacketFixed Fallback; const auto View=SocketFrameView(Row.SocketFrames.At(I),Row.Key.Id,Fallback); Visitor(View); }
}
void FStore::VisitSocketChannels(const void* Source,TFunctionRef<void(const FVaRestInspectorPacketView&)> Visitor)
{
 const auto& Row=*static_cast<const FRow*>(Source);
 for (int32 I=0;I<Row.NumSocketChannels;++I)
 {
  const auto& Cell=Row.SocketChannels[I]; if (!Cell.Serial) continue;
  if (Cell.Archived) { Visitor(Cell.Archived->View()); continue; }
  for (int32 N=0;N<Row.NumNotes;++N) { const auto& Note=Row.Notes[(Row.NoteHead+N)%FNormalizedLimits::NoteSlots]; if (Note.Serial==Cell.Serial && Note.Owner) { Visitor(Note.Owner->View()); break; } }
 }
}
void FStore::ArchiveSocketChannel(FRow& Row,uint64 Serial,FStorePacketPtr& Owner)
{
 if (!Owner || !Serial) return;
 for (int32 I=0;I<Row.NumSocketChannels;++I) if (Row.SocketChannels[I].Serial==Serial)
 { check(!Row.SocketChannels[I].Archived); Row.SocketChannels[I].Archived=MoveTemp(Owner); return; }
}
int32 FStore::PrepareSocketChannel(FRow& Row,FStringView Channel,bool& Replaced)
{
 Replaced=false; if (Channel.IsEmpty()) return INDEX_NONE;
 if (!Row.SocketChannels.IsAllocated())
 {
  const auto Size=MeasureStoreBlock<FSocketChannelCell>(*Budget,256);
  if (!Size.bSupported || !MakeAdmissionRoom({0,Size.Bytes,{Row.IndexSlot,INDEX_NONE}}) || !Row.SocketChannels.Allocate(Budget,256))
  { AddObservation(Row.Value.SocketChannelsOmitted,1,Row.Value.bCountersSaturated); return INDEX_NONE; }
 }
 for (int32 I=0;I<Row.NumSocketChannels;++I)
 {
  const auto& Cell=Row.SocketChannels[I];
  if (Cell.Archived && Cell.Archived->View().Text(EText::SocketChannel)==Channel) return I;
  if (!Cell.Archived) for (int32 N=0;N<Row.NumNotes;++N)
  { const auto& Note=Row.Notes[(Row.NoteHead+N)%FNormalizedLimits::NoteSlots]; if (Note.Serial==Cell.Serial && Note.Owner && Note.Owner->View().Text(EText::SocketChannel)==Channel) return I; }
 }
 if (Row.NumSocketChannels<256) return Row.NumSocketChannels;
 int32 Oldest=0; for (int32 I=1;I<Row.NumSocketChannels;++I) if (Row.SocketChannels[I].Serial<Row.SocketChannels[Oldest].Serial) Oldest=I;
 Replaced=true; return Oldest;
}
void FStore::UpdateSocketDeadline(FRow& Row)
{
 if (!Row.SocketPendingFrames) RemoveDeadline(Row);
 else if (Row.DeadlineIndex==INDEX_NONE) AddDeadline(Row);
}
bool FStore::DropSocketFrame(FRow& Row)
{
 if (!Row.SocketFrames.Num()) return false;
 const auto& Frame=Row.SocketFrames.At(0);
 if (Frame.State==EVaRestInspectorDetailState::Pending) { check(Row.SocketPendingFrames>0); --Row.SocketPendingFrames; }
 RetainKey(FRowKey(ERowKeyKind::SocketFrame,Frame.Id),nullptr,false,Row.Value.ScopeId,Row.Value.bContextKnown);
 Row.SocketFrames.Pop(); Row.Value.SocketFramesRetained=Row.SocketFrames.Num(); AddObservation(Row.Value.SocketFramesEvicted,1,Row.Value.bCountersSaturated);
 UpdateSocketDeadline(Row); Touch(Row); return true;
}
bool FStore::ApplySocketFrame(FRow& Row,FPacket& Packet)
{
 const FPacketFixed Fixed=Packet.Fixed(); const auto& S=Fixed.Scalars;
 if (!S.FrameId || S.SocketBytes<0) { Increment(State.ContractViolations); return false; }
 if (IsKeyRetained(FRowKey(ERowKeyKind::SocketFrame,S.FrameId))) return true;
 for (int32 I=Row.SocketFrames.Num()-1;I>=0;--I) if (Row.SocketFrames.At(I).Id==S.FrameId) return true;
 const auto Omit=[&] { AddObservation(Row.Value.SocketFramesOmitted,1,Row.Value.bCountersSaturated); AddObservation(State.MetadataOmitted,1,State.bLossCountersSaturated); RetainKey(FRowKey(ERowKeyKind::SocketFrame,S.FrameId),nullptr,false,Row.Value.ScopeId,Row.Value.bContextKnown); return false; };
 if (S.SocketDirection==EVaRestTraceSocketDirection::In) { AddObservation(Row.Value.SocketFramesIn,1,Row.Value.bCountersSaturated); AddObservation(Row.Value.WireBytesDown,uint64(S.SocketBytes),Row.Value.bCountersSaturated); }
 else { AddObservation(Row.Value.SocketFramesOut,1,Row.Value.bCountersSaturated); AddObservation(Row.Value.WireBytesUp,uint64(S.SocketBytes),Row.Value.bCountersSaturated); }
 const int32 Cap=Row.SocketFrames.Max() ? FMath::Min(Row.SocketFrames.Max(),Limits.MaxFramesPerSocket) : Limits.MaxFramesPerSocket;
 while (Row.SocketFrames.Num()>=Cap) DropSocketFrame(Row);
 const auto PacketCharge=FStorePacketOwner::Charge(Packet,Budget->Lifetime->Allocator); if (!PacketCharge.IsSet()) return Omit();
 bool Full=false;
 while (true)
 {
  const auto SlotCharge=Row.SocketFrames.NewSlotCharge(*Budget,Limits.MaxFramesPerSocket); if (!SlotCharge.IsSet()) return Omit();
  int64 Needed=SlotCharge.GetValue(); if (!AddChecked(Needed,PacketCharge.GetValue(),MAX_int64)) return Omit();
  if (MakeAdmissionRoom({0,Needed,{Row.IndexSlot,INDEX_NONE}})) { Full=true; break; }
  if (!DropSocketFrame(Row))
  {
   if (!MakeAdmissionRoom({0,SlotCharge.GetValue(),{Row.IndexSlot,INDEX_NONE}})) return Omit();
   break;
  }
 }
 auto* Slot=Row.SocketFrames.Add(Budget,Limits.MaxFramesPerSocket); if (!Slot) return Omit();
 Slot->Id=S.FrameId; Slot->AttemptId=S.SocketAttemptId; Slot->Seq=Fixed.Header.Seq; Slot->Time=Fixed.Header.Time; Slot->Bytes=S.SocketBytes; Slot->Direction=S.SocketDirection; Slot->Kind=S.SocketFrameKind;
 Slot->State=SocketExcerptState(S);
 if (Full && FStorePacketOwner::TryTake(Packet,Budget,Budget->Lifetime->Allocator,Slot->Metadata))
 { if (Fixed.Header.bMetadataOmitted && !S.bExcerptPending) Slot->State=EVaRestInspectorDetailState::MetadataOmitted; }
 else { Slot->State=S.bExcerptPending ? EVaRestInspectorDetailState::Pending : EVaRestInspectorDetailState::MetadataOmitted; Row.Value.bMetadataOmitted=true; AddObservation(State.MetadataOmitted,1,State.bLossCountersSaturated); }
 if (Slot->State==EVaRestInspectorDetailState::Pending) { ++Row.SocketPendingFrames; const double Now=ReceiptNow(); Slot->Due=FMath::IsFinite(Now) && FMath::IsFinite(Now+5) ? Now+5 : std::numeric_limits<double>::infinity(); }
 Row.Value.SocketFramesRetained=Row.SocketFrames.Num(); UpdateSocketDeadline(Row); Touch(Row); return true;
}
bool FStore::ApplySocketAttempt(FRow& Row,FPacket& Packet)
{
 const FPacketFixed Fixed=Packet.Fixed(); const auto& S=Fixed.Scalars; if (!S.SocketAttemptId) return false;
 FSocketAttemptSlot* Slot=nullptr;
 for (int32 I=0;I<Row.NumSocketAttempts;++I) if (Row.SocketAttempts[I]->Value.Id==S.SocketAttemptId) { Slot=&Row.SocketAttempts[I]->Value; break; }
 if (!Slot)
 {
  if (Row.NumSocketAttempts==16)
  {
   Row.SocketAttempts[0].Reset(); for (int32 I=1;I<16;++I) Row.SocketAttempts[I-1]=MoveTemp(Row.SocketAttempts[I]); --Row.NumSocketAttempts;
   AddObservation(Row.Value.SocketAttemptsOmitted,1,Row.Value.bCountersSaturated);
  }
  const auto Bound=Budget->Lifetime->Allocator.Bound(sizeof(FOwnedSocketAttempt),alignof(FOwnedSocketAttempt));
  if (!Bound.IsSet() || !MakeAdmissionRoom({0,int64(Bound.GetValue()),{Row.IndexSlot,INDEX_NONE}})) { Row.Value.bMetadataOmitted=true; AddObservation(Row.Value.SocketAttemptsOmitted,1,Row.Value.bCountersSaturated); return false; }
  auto Credit=FStoreCredit::Acquire(Budget,int64(Bound.GetValue())); if (!Credit.IsSet()) return false;
  void* Memory=Budget->Lifetime->Allocator.AllocateReserved(sizeof(FOwnedSocketAttempt),alignof(FOwnedSocketAttempt),Bound.GetValue()); if (!Memory) return false;
  auto* Owned=new(Memory) FOwnedSocketAttempt(); Owned->Credit=MoveTemp(Credit.GetValue()); Row.SocketAttempts[Row.NumSocketAttempts++].Reset(Owned); Slot=&Owned->Value;
  Slot->Id=S.SocketAttemptId; Slot->Partial=S.bSocketPartial || Fixed.Header.bSnapshot || S.SocketAttemptPhase!=EVaRestTraceSocketAttemptPhase::Connecting;
  AddObservation(Row.Value.ObservedDispatches,1,Row.Value.bCountersSaturated);
 }
 if ((Slot->HasClosed && S.SocketAttemptPhase!=EVaRestTraceSocketAttemptPhase::Closed) || (Slot->HasOpened && S.SocketAttemptPhase==EVaRestTraceSocketAttemptPhase::Connecting)) return true;
 const bool SourceTimeKnown=!Fixed.Header.bSnapshot && !S.bSocketPartial && FMath::IsFinite(Fixed.Header.Time) && Fixed.Header.Time>=0;
 switch (S.SocketAttemptPhase)
 {
 case EVaRestTraceSocketAttemptPhase::Connecting:
  if (!Slot->HasConnecting) { Slot->HasConnecting=true; Slot->ConnectingAt=SourceTimeKnown ? Fixed.Header.Time : -1; TakePacket(Row,Packet,Slot->Connecting); }
  Row.Value.SocketState=TEXT("Connecting"); Row.Value.bPinned=true; Row.Value.EndState=EVaRestInspectorEndState::InFlight; Row.Value.bSocketReady=false; break;
 case EVaRestTraceSocketAttemptPhase::Open:
  if (!Slot->HasOpened) { Slot->HasOpened=true; Slot->OpenedAt=SourceTimeKnown ? Fixed.Header.Time : -1; Slot->ConnectMs=S.SocketConnectMs; TakePacket(Row,Packet,Slot->Opened); }
  Row.Value.SocketState=TEXT("Connected"); Row.Value.bPinned=true; Row.Value.bSocketReady=Row.Value.SocketProtocol!=TEXT("sio") && Row.Value.SocketProtocol!=TEXT("SocketIO"); Row.Value.EndState=EVaRestInspectorEndState::InFlight; Row.Value.FinalStatus=101; Row.Value.SocketFailure=NAME_None; break;
 case EVaRestTraceSocketAttemptPhase::Closed:
  if (!Slot->HasClosed)
  {
   Slot->HasClosed=true; Slot->ClosedAt=SourceTimeKnown ? Fixed.Header.Time : -1; Slot->CloseCode=S.SocketCloseCode; Slot->HttpStatus=S.Status; Slot->CloseBy=S.SocketCloseBy;
   Slot->Clean=S.bSocketClean; Slot->Reconnect=S.bSocketReconnect; Slot->GaveUp=S.bSocketGaveUp; Slot->ReadPaused=S.bSocketReadPaused; Slot->ErrorKind=S.ErrorKind; Slot->Failure=S.FailureDetail; Slot->Delay=S.Delay;
	 if (Slot->OpenedAt>=0 && Slot->ClosedAt>=Slot->OpenedAt)
	 { Row.SocketClosedSeconds+=Slot->ClosedAt-Slot->OpenedAt; Row.bSocketClosedSecondsKnown=FMath::IsFinite(Row.SocketClosedSeconds); }
   TakePacket(Row,Packet,Slot->Closed);
  }
  Row.Value.SocketState=S.bSocketReconnect ? FName(TEXT("Reconnecting")) : S.bSocketGaveUp ? FName(TEXT("GaveUp")) : FName(TEXT("Closed"));
  Row.Value.bPinned=S.bSocketReconnect; Row.Value.bSocketReady=false; Row.Value.SocketCloseCode=S.SocketCloseCode; Row.Value.SocketFailure=S.FailureDetail; Row.Value.FinalStatus=S.Status;
  Row.Value.EndState=S.bSocketReconnect ? EVaRestInspectorEndState::InFlight : S.ErrorKind!=EVaRestErrorKind::None ? EVaRestInspectorEndState::Error : EVaRestInspectorEndState::Success; break;
 }
 Row.Value.Total={Row.SocketClosedSeconds,Row.bSocketClosedSecondsKnown}; Touch(Row); return true;
}
void FStore::ObserveSocketStats(const FPacketFixed& Fixed)
{
 const auto& S=Fixed.Scalars;
 if (!Storage || !Budget || !S.SocketId || Fixed.Header.Kind==EVaRestTraceKind::Excerpt) return;
 const bool Created=Fixed.Header.Kind==EVaRestTraceKind::Socket && S.SocketLifecycle==EVaRestTraceSocketLifecycle::Created;
 const auto* Row=FindRow(FRowKey(ERowKeyKind::Socket,S.SocketId));
 const bool ContextKnown=Created || (Row && Row->Value.bContextKnown);
 const uint64 ScopeId=Created ? S.ScopeId : Row ? Row->Value.ScopeId : 0;
 if (ContextKnown && IsClearedScope(ScopeId)) return;
 if (!Storage->SocketStats.IsAllocated())
 {
  const int32 Capacity=FMath::Min(Storage->MaxRows,256)+1;
  const auto Size=MeasureStoreBlock<FSocketStatsCell>(*Budget,Capacity);
  const FAdmissionAnchors Anchors{Row ? Row->IndexSlot : INDEX_NONE,INDEX_NONE};
  if (!Size.bSupported || !MakeAdmissionRoom({0,Size.Bytes,Anchors}) || !Storage->SocketStats.Allocate(Budget,Capacity))
  { Increment(State.OmittedStatsGroups); return; }
  Storage->SocketStatsCapacity=Capacity; Storage->NumSocketStats=1;
  Storage->SocketStats[0].Value.bOverflow=true; Storage->SocketStats[0].Value.bPartial=true;
 }
 FSocketStatsCell* Cell=nullptr;
 for (int32 I=1;I<Storage->NumSocketStats;++I) if (Storage->SocketStats[I].Value.SocketId==S.SocketId) { Cell=&Storage->SocketStats[I]; break; }
 bool New=false;
 if (!Cell)
 {
  if (Storage->NumSocketStats<Storage->SocketStatsCapacity)
  { Cell=&Storage->SocketStats[Storage->NumSocketStats++]; Cell->Value.SocketId=S.SocketId; New=true; }
  else { Cell=&Storage->SocketStats[0]; Increment(State.OmittedStatsGroups); }
 }
 auto& V=Cell->Value;
 if (V.bContextKnown && IsClearedScope(V.ScopeId)) return;
 if (!V.bOverflow && ContextKnown) { V.ScopeId=ScopeId; V.bContextKnown=true; }
 if (Created && !V.bOverflow) { V.Name=S.SocketName; V.Protocol=S.SocketProtocolName; V.Entry=S.SocketEntry; }
 V.bPartial|=Fixed.Header.bSnapshot || Fixed.Header.bMetadataOmitted || S.bSocketPartial || (New && !Created);
 if (!Fixed.Header.bSnapshot && Fixed.Header.Seq)
 {
  for (uint64 Seq:Cell->Sequences) if (Seq==Fixed.Header.Seq) return;
  Cell->Sequences[Cell->NextSeq++%64]=Fixed.Header.Seq;
 }
 const auto Add=[&](uint64& To,uint64 N) { AddObservation(To,N,V.bSaturated); };
 if (Fixed.Header.Kind==EVaRestTraceKind::Socket)
 {
  if (!Created && !V.bOverflow) V.bReleased=true;
 }
 else if (Fixed.Header.Kind==EVaRestTraceKind::SocketAttempt && S.SocketAttemptId)
 {
  FSocketStatsCell::FAttempt* A=nullptr;
  for (auto& Candidate:Cell->Attempts) if (Candidate.SocketId==S.SocketId && Candidate.Id==S.SocketAttemptId) { A=&Candidate; break; }
  if (!A)
  {
   A=&Cell->Attempts[Cell->NextAttempt++%16]; *A={S.SocketId,S.SocketAttemptId,0};
   if (S.SocketAttemptPhase==EVaRestTraceSocketAttemptPhase::Connecting && (V.bOverflow ? S.SocketGap!=EVaRestTraceSocketGap::None : V.Attempts>0)) Add(V.Reconnects,1);
   Add(V.Attempts,1);
  }
  const uint8 Bit=uint8(1u<<static_cast<uint8>(S.SocketAttemptPhase));
  if (!(A->Phases&Bit))
  {
   A->Phases|=Bit;
   if (S.SocketAttemptPhase==EVaRestTraceSocketAttemptPhase::Open) Add(V.Opened,1);
   else if (S.SocketAttemptPhase==EVaRestTraceSocketAttemptPhase::Closed)
   { Add(V.Closed,1); if (S.ErrorKind!=EVaRestErrorKind::None || !S.FailureDetail.IsNone()) Add(V.Failures,1); }
  }
 }
 else if (Fixed.Header.Kind==EVaRestTraceKind::SocketFrame && !Fixed.Header.bSnapshot && S.FrameId)
 {
  for (const auto& F:Cell->Frames) if (F.SocketId==S.SocketId && F.Id==S.FrameId) return;
  Cell->Frames[Cell->NextFrame++%64]={S.SocketId,S.FrameId};
  const bool In=S.SocketDirection==EVaRestTraceSocketDirection::In; Add(In ? V.FramesIn : V.FramesOut,1);
  if (S.SocketBytes>=0) Add(In ? V.BytesDown : V.BytesUp,uint64(S.SocketBytes)); else V.bPartial=true;
 }
 else if (Fixed.Header.Kind==EVaRestTraceKind::SocketNote && !Fixed.Header.bSnapshot)
 {
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::SendDropped && S.SocketCount>0) Add(V.Drops,uint64(S.SocketCount));
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::Ping && FMath::IsFinite(S.SocketRtt) && S.SocketRtt>=0 && S.SocketRtt<=1e12)
  {
   if (V.RttCount<MAX_uint64 && FMath::IsFinite(V.RttSum+S.SocketRtt)) { ++V.RttCount; V.RttSum+=S.SocketRtt; }
   else V.bSaturated=true;
  }
 }
 Increment(RevisionValue);
}
bool FStore::ApplySocketPacket(FPacket& Packet)
{
 const FPacketFixed Fixed=Packet.Fixed(); const auto& S=Fixed.Scalars; if (!S.SocketId) return false;
 const FRowKey Key(ERowKeyKind::Socket,S.SocketId); FRow* Row=FindRow(Key);
 const bool Lifecycle=Fixed.Header.Kind==EVaRestTraceKind::Socket;
 const bool NewLife=Lifecycle && S.SocketLifecycle==EVaRestTraceSocketLifecycle::Created;
 const bool Connect=Fixed.Header.Kind==EVaRestTraceKind::SocketAttempt && S.SocketAttemptPhase!=EVaRestTraceSocketAttemptPhase::Closed;
 if (IsKeyCleared(Key) && !NewLife && !Connect) return false;
 if (NewLife && IsClearedScope(S.ScopeId)) return false;
 ObserveSocketStats(Fixed);
 if (!Row)
 {
  if (Fixed.Header.Kind==EVaRestTraceKind::Excerpt) { AddObservation(State.ExcerptsSkipped,1,State.bLossCountersSaturated); return false; }
  if (IsKeyCleared(Key) && !NewLife && !Connect) return false;
  if (IsKeyRetained(Key) && !NewLife && !Connect) return false;
  if (NewLife && IsClearedScope(S.ScopeId)) return false;
  if (NewLife || Connect) ForgetRetainedKey(Key);
  Row=AdmitRow(Key,!NewLife || S.bSocketPartial || Fixed.Header.bSnapshot); if (!Row) return false;
  Row->Value.SocketState=TEXT("Partial"); Row->Value.bPinned=true; Row->Value.DispatchHistory=EVaRestInspectorDispatchHistory::Unknown;
 }
 if (Fixed.Header.bMetadataOmitted) Row->Value.bMetadataOmitted=true;
 if (Row->Value.bSocketReleased && Fixed.Header.Kind!=EVaRestTraceKind::Excerpt) return true;
 if (Row->Value.bContextKnown && IsClearedScope(Row->Value.ScopeId)) return false;
 Row->PresenceStamp=CurrentReconciliationStamp;
 if (Lifecycle)
 {
  if (NewLife)
  {
   Row->Value.ScopeId=S.ScopeId; Row->Value.bContextKnown=true; Row->Value.SocketName=S.SocketName; Row->Value.SocketProtocol=S.SocketProtocolName; Row->Value.SocketEntry=S.SocketEntry;
   Row->Value.SocketState=S.SocketState; Row->Value.bSocketReady=S.bSocketReady;
   if (Fixed.Header.bSnapshot || S.bSocketPartial) Row->Value.bPinned=S.SocketState!=TEXT("Disconnected");
   if (!Row->Started) TakePacket(*Row,Packet,Row->Started);
   if (Row->Started) Row->Value.bInitiatorKnown=!Row->Started->View().Text(EText::OwnerPath).IsEmpty() || !Row->Started->View().Text(EText::ScriptFunction).IsEmpty() || !Row->Started->View().Text(EText::SourceFile).IsEmpty();
   if (!Fixed.Header.bSnapshot && !S.bSocketPartial && FMath::IsFinite(S.StartedAt) && S.StartUtc.GetTicks()>0)
   { Row->Value.StartedAt=S.StartedAt; Row->Value.StartUtc=S.StartUtc; Row->Value.bStartedAtKnown=true; Row->Value.bStartUtcKnown=true; }
  }
  else
  {
   Row->Value.bSocketReleased=true; Row->Value.bPinned=false; Row->Value.bSocketReady=false; Row->Value.SocketState=TEXT("Released"); Row->Value.bTerminalKnown=true;
   if (Row->Value.EndState==EVaRestInspectorEndState::InFlight) Row->Value.EndState=EVaRestInspectorEndState::Success;
   Row->CompletionFixed=ScalarCopy(Fixed); Row->Value.bActualCompletionObserved=true; Row->Value.CompletedAt=Fixed.Header.Time; Row->Value.CompletedObservedAt=ReceiptNow(); TakePacket(*Row,Packet,Row->Completed);
  }
 }
 else if (Fixed.Header.Kind==EVaRestTraceKind::SocketAttempt) ApplySocketAttempt(*Row,Packet);
 else if (Fixed.Header.Kind==EVaRestTraceKind::SocketFrame) return ApplySocketFrame(*Row,Packet);
 else if (Fixed.Header.Kind==EVaRestTraceKind::Excerpt)
 {
  FSocketFrameSlot* Frame=nullptr; for (int32 I=Row->SocketFrames.Num()-1;I>=0;--I) if (Row->SocketFrames.At(I).Id==S.FrameId) { Frame=&Row->SocketFrames.At(I); break; }
  if (!Frame)
  { auto& Count=IsKeyRetained(FRowKey(ERowKeyKind::SocketFrame,S.FrameId)) ? Row->Value.SocketLateExcerptsEvicted : Row->Value.SocketLateExcerptsUnknown; AddObservation(Count,1,Row->Value.bCountersSaturated); Touch(*Row); return false; }
  if (Frame->State!=EVaRestInspectorDetailState::Pending) return true;
  if (S.SocketAttemptId && S.SocketAttemptId!=Frame->AttemptId) return false;
  Frame->State=TakePacket(*Row,Packet,Frame->Excerpt) ? SocketExcerptState(S) : EVaRestInspectorDetailState::MetadataOmitted;
  check(Row->SocketPendingFrames>0); --Row->SocketPendingFrames; UpdateSocketDeadline(*Row); CompleteMutation(*Row); return true;
 }
 else if (Fixed.Header.Kind==EVaRestTraceKind::SocketNote)
 {
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::SendDropped && S.SocketCount>0) AddObservation(Row->Value.SocketDroppedFrames,uint64(S.SocketCount),Row->Value.bCountersSaturated);
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::Flow) Row->Value.bSocketReadPaused=S.bSocketPaused;
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::Backpressure) { Row->Value.bSocketBackpressure=S.bSocketHigh; Row->Value.SocketPendingBytes=S.SocketPendingBytes; }
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::Ping) Row->Value.SocketRtt=S.SocketRtt;
  if (S.SocketNoteKind==EVaRestTraceSocketNoteKind::Channel && Packet.Text(EText::SocketChannel)==TEXT("/")) Row->Value.bSocketReady=S.SocketChannelState==EVaRestTraceSocketChannelState::Up;
  bool Replaced=false; const int32 ChannelSlot=S.SocketNoteKind==EVaRestTraceSocketNoteKind::Channel ? PrepareSocketChannel(*Row,Packet.Text(EText::SocketChannel),Replaced) : INDEX_NONE;
  uint64 Serial=0; const auto StateOfNote=ApplyNote(*Row,Packet,Serial);
  if (Serial && ChannelSlot!=INDEX_NONE)
  {
   auto& Cell=Row->SocketChannels[ChannelSlot]; Cell.Archived.Reset(); Cell.Serial=Serial;
   if (ChannelSlot==Row->NumSocketChannels) ++Row->NumSocketChannels;
   if (Replaced) AddObservation(Row->Value.SocketChannelsOmitted,1,Row->Value.bCountersSaturated);
   Row->Value.SocketChannelsRetained=Row->NumSocketChannels;
  } FVaRestInspectorTimelineEntry Timeline;
  Timeline.Kind=Fixed.Header.Kind; Timeline.Seq=Fixed.Header.Seq; Timeline.Time=Fixed.Header.Time; Timeline.DetailState=StateOfNote; AppendTimeline(*Row,Timeline,Serial);
 }
 if (Lifecycle || Fixed.Header.Kind==EVaRestTraceKind::SocketAttempt)
 {
  FVaRestInspectorTimelineEntry Timeline; Timeline.Kind=Fixed.Header.Kind; Timeline.Seq=Fixed.Header.Seq; Timeline.Time=Fixed.Header.Time; Timeline.DispatchId=S.SocketAttemptId;
  Timeline.DetailState=Fixed.Header.bMetadataOmitted ? EVaRestInspectorDetailState::MetadataOmitted : EVaRestInspectorDetailState::Present; AppendTimeline(*Row,Timeline);
 }
 UpdateSocketDeadline(*Row); CompleteMutation(*Row); return true;
}
void FStore::ApplySocketSnapshot(const FVaRestTraceSnapshot& Snapshot,const FCapturePolicy& Policy,TSharedRef<FPendingAccount,ESPMode::ThreadSafe> Pending,FAllocationAdapter& Allocator)
{
 const auto Apply=[&](EVaRestTraceKind Kind,FVaRestTracePayload&& Payload)
 {
  FVaRestTraceEvent Event; Event.Kind=Kind; Event.Source=EVaRestTraceSource::Socket; Event.Payload=MoveTemp(Payload);
  FPacketLayout Layout; if (FPacketCodec::Measure(Event,Policy,Allocator,Layout,true)==EMeasure::Unsupported) { Increment(State.AttachLosses); return; }
  Layout.Fixed.Header.Generation=Generation; auto Ticket=FPendingTicket::Acquire(Pending,Layout.PendingCharge); if (!Ticket.IsSet()) { Increment(State.AttachLosses); return; }
  auto Packet=FPacketCodec::Write(Event,Layout,MoveTemp(Ticket.GetValue()),Allocator); if (Packet.IsValid()) ApplySocketPacket(Packet); else Increment(State.AttachLosses);
 };
 for (const auto& Socket:Snapshot.Sockets) Apply(EVaRestTraceKind::Socket,FVaRestTracePayload(TInPlaceType<FVaRestTraceSocket>(),Socket));
 for (const auto& Attempt:Snapshot.SocketAttempts) Apply(EVaRestTraceKind::SocketAttempt,FVaRestTracePayload(TInPlaceType<FVaRestTraceSocketAttempt>(),Attempt));
 for (const auto& Note:Snapshot.SocketNotes) Apply(EVaRestTraceKind::SocketNote,FVaRestTracePayload(TInPlaceType<FVaRestTraceSocketNote>(),Note));
}
