// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorViewModel.h"
#include "VaRestHttpTypes.h"
#include "VaRestInspectorCurl.h"

namespace
{
	using EText = VaRest::Inspector::View::EText;
	FString Copy(FStringView Value, int32 Cap = 1024) { return FString(Value.Left(Cap)); }
	FString Text(const FVaRestInspectorPacketView& Packet, EText Field) { return Copy(Packet.Text(Field)); }
	FString Body(const FVaRestInspectorPacketView& Packet)
	{
		if (!Packet.Fixed || Packet.Excerpt.IsEmpty()) return TEXT("(excerpt unavailable)");
		const auto Bytes = Packet.Excerpt.Left(65536);
		if (Packet.Fixed->Scalars.ExcerptKind == EVaRestExcerptKind::Binary)
			return FString::Printf(TEXT("(binary: %lld bytes; captured excerpt %d bytes)"), Packet.Fixed->Scalars.TotalBytes, Bytes.Num());
		FString Out = FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num());
		if (Packet.Fixed->Scalars.bTruncated || Bytes.Num() != Packet.Excerpt.Num()) Out += TEXT("\n(excerpt truncated)");
		return Out;
	}
	FVaRestInspectorBodySource BodySource(const FVaRestInspectorPacketView& Packet)
	{
		FVaRestInspectorBodySource Out;
		if (!Packet.Fixed) return Out;
		const auto Bytes = Packet.Excerpt.Left(65536); Out.Bytes.Append(Bytes.GetData(), Bytes.Num());
		Out.Kind = Packet.Fixed->Scalars.ExcerptKind; Out.bTruncated = Packet.Fixed->Scalars.bTruncated || Bytes.Num() != Packet.Excerpt.Num();
		Out.bRedacted = Packet.Fixed->Scalars.bRedacted; Out.TotalBytes = Packet.Fixed->Scalars.TotalBytes;
		Out.bBinaryCaptured = Packet.Fixed->Scalars.bBinaryCaptured;
		Out.bAvailable = Out.Kind != EVaRestExcerptKind::Skipped; return Out;
	}
	FString Headers(const FVaRestInspectorPacketView& Packet)
	{
		if (!Packet.Fixed) return TEXT("(headers unavailable)\n");
		FString Out;
		for (const auto& Pair : Packet.Pairs(Packet.Fixed->Headers))
		{
			if (Out.Len() >= 32768) { Out += TEXT("(header display truncated)\n"); break; }
			Out += Copy(Packet.Read(Pair.First), 256) + TEXT(": ") + Copy(Packet.Read(Pair.Second), 2048) + TEXT("\n");
		}
		return Out.IsEmpty() ? TEXT("(no retained headers)\n") : Out;
	}
	FString State(EVaRestInspectorEndState Value)
	{
		switch (Value)
		{
		case EVaRestInspectorEndState::InFlight: return TEXT("pending");
		case EVaRestInspectorEndState::Success: return TEXT("success");
		case EVaRestInspectorEndState::Error: return TEXT("error");
		case EVaRestInspectorEndState::Queued: return TEXT("queued");
		case EVaRestInspectorEndState::Canceled: return TEXT("canceled");
		default: return TEXT("capture interrupted");
		}
	}

}
int64 FVaRestInspectorRowModel::PresentationBytes() const
{
	int64 Out = sizeof(*this) + 128;
	for (const auto* TextValue : {&Method, &Name, &Url, &Initiator, &Scope, &Client, &Tag, &Searchable}) Out += TextValue->GetAllocatedSize();
	for (const auto& Value : Columns) Out += int64(Value.ToString().Len() + 1) * sizeof(TCHAR);
	Out += int64(Tooltip.ToString().Len() + OfflineBase.ToString().Len() + 2) * sizeof(TCHAR);
	return Out;
}
TSharedRef<FVaRestInspectorViewModel> FVaRestInspectorViewModel::Create()
{
	check(IsInGameThread()); TSharedRef<FVaRestInspectorViewModel> Out = MakeShareable(new FVaRestInspectorViewModel());
	if (auto* Inspector = IVaRestInspector::TryGet()) Out->Changed = Inspector->OnChanged().AddSP(Out, &FVaRestInspectorViewModel::Invalidate);
	return Out;
}
FVaRestInspectorViewModel::~FVaRestInspectorViewModel() { Detach(); }
void FVaRestInspectorViewModel::Detach()
{
	check(IsInGameThread()); if (bDetached) return; bDetached = true;
	if (auto* Inspector = IVaRestInspector::TryGet()) Inspector->OnChanged().Remove(Changed);
	Changed.Reset(); Wake = {};
}
void FVaRestInspectorViewModel::Invalidate(uint64)
{ check(IsInGameThread()); if (!bDetached) { bDirty = true; if (Wake) Wake(); } }
void FVaRestInspectorViewModel::SetFilter(FString TextValue, double Now)
{ Revealed.Reset(); PendingFilter = TextValue.Left(4096); FilterAt = Now + .15; bFilterPending = true; if (Wake) Wake(); }
void FVaRestInspectorViewModel::SetScope(TOptional<uint64> Id)
{ Revealed.Reset(); Scope = Id; Invalidate(); }
void FVaRestInspectorViewModel::Sort(EVaRestInspectorColumn Column, bool Ascending)
{ SortColumn = Column; bAscending = Ascending; Refilter(); if (Wake) Wake(); }
bool FVaRestInspectorViewModel::Matches(const FVaRestInspectorRowModel& Row) const
{
 if (Revealed.IsSet() && Row.Identity==Revealed.GetValue()) return true;
 if (Scope.IsSet() && (!Row.bContextKnown || Row.ScopeId!=Scope.GetValue())) return false;
 if ((Kind==EVaRestInspectorKindFilter::Http && Row.Identity.Kind!=EVaRestInspectorRecordKind::Http)
  || (Kind==EVaRestInspectorKindFilter::WebSocket && Row.Method!=TEXT("WS")) || (Kind==EVaRestInspectorKindFilter::SocketIO && Row.Method!=TEXT("SIO"))
  || (Kind==EVaRestInspectorKindFilter::Errors && !Row.bError) || (Kind==EVaRestInspectorKindFilter::Markers && Row.Identity.Kind!=EVaRestInspectorRecordKind::Marker)) return false;
 FVaRestInspectorFilterFields F;
 F.Id=Row.Identity.Id; F.ScopeId=Row.ScopeId; F.Bytes=Row.Bytes; F.Attempts=Row.Attempts; F.Status=Row.Status; F.Duration=Row.Duration;
 F.Method=Row.Method; F.Name=Row.Name; F.Url=Row.Url; F.Initiator=Row.Initiator; F.Scope=Row.Scope; F.Client=Row.Client; F.Tag=Row.Tag; F.Searchable=Row.Searchable;
 F.bSocket=Row.Identity.Kind==EVaRestInspectorRecordKind::Socket; F.bSio=F.bSocket && Row.Method==TEXT("SIO");
 F.bContextKnown=Row.bContextKnown; F.bError=Row.bError; F.bRunning=Row.bRunning; F.bCached=Row.bCached; F.bQueued=Row.bQueued; F.bAuth=Row.bAuth;
 return Query.Matches(F);
}
void FVaRestInspectorViewModel::RefreshScopes()
{
	TMap<uint64, FString> Labels;
	for (const auto& Pair : Cache)
		if (Pair.Value->bContextKnown && (Labels.Num() < 256 || Labels.Contains(Pair.Value->ScopeId))) Labels.Add(Pair.Value->ScopeId, Pair.Value->Scope);
	if (Scope.IsSet() && !Labels.Contains(Scope.GetValue())) Labels.Add(Scope.GetValue(), LexToString(Scope.GetValue()));
	TArray<uint64> Ids; Labels.GetKeys(Ids); Ids.Sort();
	TArray<TSharedPtr<FVaRestInspectorScopeOption>> Next;
	auto Add = [&](bool All, uint64 Id, const FString& Label)
	{
		TSharedPtr<FVaRestInspectorScopeOption> Value;
		for (const auto& Old : ScopeOptions) if (Old->bAll == All && Old->Id == Id) { Value = Old; break; }
		if (!Value) Value = MakeShared<FVaRestInspectorScopeOption>();
		Value->bAll = All; Value->Id = Id; Value->Label = FText::FromString(Label); Next.Add(MoveTemp(Value));
	};
	Add(true, 0, TEXT("All scopes"));
	for (uint64 Id : Ids) Add(false, Id, Labels[Id] + FString::Printf(TEXT(" (#%llu)"), Id));
	ScopeOptions = MoveTemp(Next);
}
void FVaRestInspectorViewModel::Refilter()
{
	Visible.Reset(); for (const auto& Pair : Cache) if (Matches(*Pair.Value)) Visible.Add(Pair.Value);
	Visible.StableSort([&](const auto& A, const auto& B)
	{
		int32 Compare = 0;
		if (SortColumn == EVaRestInspectorColumn::Duration) Compare = A->Duration < B->Duration ? -1 : A->Duration > B->Duration;
		else if (SortColumn == EVaRestInspectorColumn::Size) Compare = A->Bytes < B->Bytes ? -1 : A->Bytes > B->Bytes;
		else if (SortColumn == EVaRestInspectorColumn::Attempts) Compare = A->Attempts < B->Attempts ? -1 : A->Attempts > B->Attempts;
		else if (SortColumn == EVaRestInspectorColumn::Time) Compare = A->Identity.Arrival < B->Identity.Arrival ? -1 : A->Identity.Arrival > B->Identity.Arrival;
		else if (SortColumn == EVaRestInspectorColumn::Status) Compare = A->Status < B->Status ? -1 : A->Status > B->Status;
		else if (SortColumn == EVaRestInspectorColumn::Id) Compare = A->Identity.Id < B->Identity.Id ? -1 : A->Identity.Id > B->Identity.Id;
		else Compare = A->Columns[static_cast<int32>(SortColumn)].ToString().Compare(B->Columns[static_cast<int32>(SortColumn)].ToString());
		if (!Compare) return A->Identity.Arrival < B->Identity.Arrival; return bAscending ? Compare < 0 : Compare > 0;
	});
}
bool FVaRestInspectorViewModel::Refresh(double Now)
{
	check(IsInGameThread()); if (bDetached) return false;
	if (bFilterPending && Now >= FilterAt) { Filter = MoveTemp(PendingFilter); Query.SetText(Filter); bFilterPending = false; bDirty = true; }
	if (!bDirty) return false; bDirty = false;
	auto* Inspector = IVaRestInspector::TryGet(); if (!Inspector) return false;
	bCapturing = Inspector->IsCapturing(); TSet<FVaRestInspectorRowIdentity> Seen; int64 Bytes = 0; int32 Total = 0, Omitted = 0;
	const bool Read = Inspector->ForEachRecord([&](const FVaRestInspectorRecordView& Source)
	{
		++Total; const FVaRestInspectorRowIdentity Key{Source.RequestId, Source.ArrivalOrdinal, Source.Kind};
		if (Seen.Num() >= MaxRows || Bytes >= MaxPresentationBytes) { ++Omitted; return; }
		auto Row = Cache.FindRef(Key);
		const FString ScopeLabel = Source.bContextKnown ? (Source.ScopeId == 0 ? FString(TEXT("Process")) : Copy(Source.ScopeLabel, 128)) : FString(TEXT("unknown"));
		const FString ClientLabel = Copy(Source.ClientLabel, 128);
		if (!Row || Row->Revision != Source.Revision || Row->Scope != (ScopeLabel.IsEmpty() ? LexToString(Source.ScopeId) : ScopeLabel) || (Source.Kind != EVaRestInspectorRecordKind::Socket && Row->Client != ClientLabel))
		{
			if (!Row) Row = MakeShared<FVaRestInspectorRowModel>(); Row->Identity = Key; Row->Revision = Source.Revision;
			Row->ScopeId = Source.ScopeId; Row->ClientId = Source.ClientId; Row->Status = Source.FinalStatus; Row->bContextKnown = Source.bContextKnown;
			Row->Method = Source.Kind == EVaRestInspectorRecordKind::Marker ? TEXT("MARK") : Text(Source.Started, EText::Verb);
			Row->Name = Source.Started.Fixed ? Source.Started.Fixed->Scalars.Endpoint.ToString() : FString();
			if (Row->Name.IsEmpty() || Row->Name == TEXT("None")) Row->Name = Text(Source.Started, EText::PathTemplate);
			if (Source.Kind == EVaRestInspectorRecordKind::Marker) Row->Name = Text(Source.Marker, EText::MarkerText);
			Row->Url = Text(Source.Started, EText::Url);
			if (Source.Kind==EVaRestInspectorRecordKind::Http && Row->Name.IsEmpty())
			{
				FString Path=Row->Url, Scheme, Host;
				if (Path.Split(TEXT("://"), &Scheme, &Path)) { if (Path.Split(TEXT("/"), &Host, &Path)) Path=TEXT("/")+Path; else Path=TEXT("/"); }
				FString UrlSuffix; if (Path.Contains(TEXT("?"))) Path.Split(TEXT("?"), &Path, &UrlSuffix); if (Path.Contains(TEXT("#"))) Path.Split(TEXT("#"), &Path, &UrlSuffix);
				Row->Name=MoveTemp(Path);
			}
			Row->Tag = Source.Started.Fixed ? Source.Started.Fixed->Scalars.Tag.ToString() : FString();
			Row->Scope = ScopeLabel.IsEmpty() ? LexToString(Source.ScopeId) : ScopeLabel;
			Row->Client = ClientLabel; Row->Initiator = Text(Source.Started, EText::ScriptFunction);
			if (!Row->Initiator.IsEmpty() && !Source.Started.Text(EText::ScriptObject).IsEmpty()) Row->Initiator=Text(Source.Started,EText::ScriptObject)+TEXT(":")+Row->Initiator;
			if (Row->Initiator.IsEmpty()) { Row->Initiator = Text(Source.Started, EText::SourceFile); if (!Row->Initiator.IsEmpty() && Source.Started.Fixed) Row->Initiator += FString::Printf(TEXT(":%d"), Source.Started.Fixed->Scalars.SourceLine); }
			if (Row->Initiator.IsEmpty()) Row->Initiator = Text(Source.Started, EText::OwnerPath);
			if (Source.Kind==EVaRestInspectorRecordKind::Socket)
			{
				Row->Method=Source.SocketProtocol==TEXT("sio") || Source.SocketProtocol==TEXT("SocketIO") ? TEXT("SIO") : TEXT("WS");
				Row->Name=Source.SocketName.IsNone() ? Source.SocketProtocol.ToString() : Source.SocketName.ToString();
				Row->Client=Source.SocketEntry.IsNone() ? Source.SocketProtocol.ToString() : Source.SocketEntry.ToString();
				Row->Tag=Source.SocketProtocol.ToString();
			}
			Row->Started = Source.StartedAt; Row->Duration = Source.Total.bKnown ? Source.Total.Seconds : -1;
			Row->Bytes = Source.WireBytesDown; Row->Attempts = Source.ObservedDispatches;
			Row->bError = Source.EndState == EVaRestInspectorEndState::Error; Row->bRunning = Source.EndState == EVaRestInspectorEndState::InFlight;
			Row->bQueued = Source.EndState == EVaRestInspectorEndState::Queued;
			Row->bCached = Source.Completed.Fixed && Source.Completed.Fixed->Scalars.bFromCache;
			Row->bAuth = Source.Started.Fixed && EnumHasAnyFlags(Source.Started.Fixed->Scalars.Flags, EVaRestTraceRequestFlags::AuthRefresh);
			FString Values[] = {Source.FinalStatus ? FString::FromInt(Source.FinalStatus) : State(Source.EndState), Row->Method, Row->Name, Row->Initiator,
				Row->Duration < 0 ? FString(TEXT("unknown")) : FString::Printf(TEXT("%.1f ms"), Row->Duration * 1000), LexToString(Row->Bytes), LexToString(Row->Attempts), Row->Scope, Row->Client, Row->Url,
				Row->bCached ? FString(TEXT("cached")) : FString(), Row->bQueued ? FString(TEXT("queued")) : FString(), Row->Tag, LexToString(Source.RequestId), Source.bStartUtcKnown ? Source.StartUtc.ToString() : FString(TEXT("unknown"))};
			if (Source.Kind==EVaRestInspectorRecordKind::Http)
			{
				FString Chain; int32 Previous=INDEX_NONE; bool AuthReplay=false;
				for (const auto& A:Source.Dispatches)
				{
					if (A.Ended.Fixed && A.bHasEnded) { const int32 StatusCode=A.Ended.Fixed->Scalars.Status; if (StatusCode!=Previous) { if (!Chain.IsEmpty()) Chain+=TEXT("→"); Chain+=LexToString(StatusCode); Previous=StatusCode; } }
					AuthReplay|=A.Dispatched.Fixed && A.Dispatched.Fixed->Scalars.GapKind==EVaRestTraceGap::SessionRefresh;
				}
				if (!Chain.IsEmpty() && Source.FinalStatus && Previous!=Source.FinalStatus) Chain+=TEXT("→")+LexToString(Source.FinalStatus);
				if (Chain.Contains(TEXT("→"))) Values[0]=MoveTemp(Chain);
				else if (Row->bError && !Source.FinalStatus && Source.Completed.Fixed) Values[0]=TEXT("failed: ")+Source.Completed.Fixed->Scalars.FailureDetail.ToString()+FString::Printf(TEXT(" (kind %d)"),int32(Source.Completed.Fixed->Scalars.ErrorKind));
				if (AuthReplay) Values[6]+=TEXT(" · auth");
				const auto Lookup=Source.Completed.Fixed ? Source.Completed.Fixed->Scalars.CacheLookup : Source.Started.Fixed ? Source.Started.Fixed->Scalars.CacheLookup : EVaRestTraceCacheLookup::None;
				const TCHAR* CacheNames[]={TEXT("—"),TEXT("miss"),TEXT("hit"),TEXT("disk"),TEXT("stale"),TEXT("304")}; Values[10]=CacheNames[FMath::Clamp(int32(Lookup),0,5)];
				for (const auto& N:Source.Notes) if (N.Fixed)
				{
					if (N.Fixed->Scalars.NoteKind==EVaRestTraceNoteKind::CacheStored) Values[10]=TEXT("stored");
					else if (N.Fixed->Scalars.NoteKind==EVaRestTraceNoteKind::CacheRevalidated) Values[10]=TEXT("304");
					else if (N.Fixed->Scalars.NoteKind==EVaRestTraceNoteKind::CacheServedStale) Values[10]=TEXT("stale");
					else if (N.Fixed->Scalars.NoteKind==EVaRestTraceNoteKind::QueueExpired) Values[11]=TEXT("expired");
				}
				if (Source.Started.Fixed && !Source.Started.Text(EText::QueueId).IsEmpty()) Values[11]=TEXT("replay of ")+Text(Source.Started,EText::QueueId).Left(128);
			}
			Row->Tooltip=FText::FromString(FString::Printf(TEXT("%llu bytes up / %llu bytes down%s\n%llu older attempt details omitted%s"),Source.WireBytesUp,Source.WireBytesDown,Source.bWireBytesUnknown ? TEXT(" (some sizes unknown)") : TEXT(""),Source.OmittedDispatches,Source.bPartial ? TEXT("\nPartial capture") : TEXT("")));
			if (Source.Kind==EVaRestInspectorRecordKind::Socket)
			{
				Values[0]=Source.SocketState.ToString();
				if (Source.SocketCloseCode) Values[0]+=FString::Printf(TEXT(" %d"),Source.SocketCloseCode);
				if (!Source.SocketFailure.IsNone()) Values[0]+=TEXT(" · ")+Source.SocketFailure.ToString();
				Values[5]=FString::Printf(TEXT("↓%llu ↑%llu"),Source.WireBytesDown,Source.WireBytesUp); Values[10].Reset(); Values[11].Reset();
				if (Source.Total.bKnown && !Source.bSocketReleased) Values[4]=TEXT("≥ ")+Values[4];
			}
			Row->OfflineBase=FText::FromString(Values[11]);
			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Values); ++Index) Row->Columns[Index] = FText::FromString(MoveTemp(Values[Index]));
			Row->Searchable = Row->Url + TEXT(" ") + Row->Name + TEXT(" ") + Row->Initiator + TEXT(" ") + Row->Client + TEXT(" ") + Row->Tag;
		}
		const int64 Cost = Row->PresentationBytes(); if (Cost > MaxPresentationBytes - Bytes) { ++Omitted; return; }
		Bytes += Cost; Seen.Add(Key); Cache.Add(Key, Row);
		if (Selected.IsSet() && Key == Selected.GetValue()) ReadDetails(Source);
	});
	if (!Read) { Status = FText::FromString(TEXT("Capture is busy; keeping the previous view.")); bDirty = true; return false; }
	for (auto It = Cache.CreateIterator(); It; ++It) if (!Seen.Contains(It.Key())) It.RemoveCurrent();
	if (Selected.IsSet() && !Cache.Contains(Selected.GetValue())) { Selected.Reset(); FrameCache.Reset(); FrameRows.Reset(); SelectedFrame = 0; Detail = {}; Detail.Overview = FText::FromString(TEXT("The selected record was evicted or capture was cleared.")); }
	ReadQueueLinks(); ReadLinks();
	if (bStatsEnabled) ReadStats();
	RefreshScopes(); Refilter(); const auto Diagnostics=Inspector->GetDiagnostics();
	Status = FText::FromString(FString::Printf(TEXT("%d records · %d shown · %d omitted by view limits · observed ↑%llu / ↓%llu bytes%s · store %lld/%lld bytes · dropped %llu events · %s"), Total, Visible.Num(), Omitted, Diagnostics.ObservedBytesUp, Diagnostics.ObservedBytesDown, Diagnostics.bObservedBytesPartial ? TEXT(" (partial)") : TEXT(""), Diagnostics.StoreBytes, Diagnostics.EffectiveStoreLimit, Diagnostics.DroppedEvents, bCapturing ? TEXT("capturing") : TEXT("paused")));
	return true;
}
void FVaRestInspectorViewModel::Select(TSharedPtr<FVaRestInspectorRowModel> Row)
{
	check(IsInGameThread()); SelectedDispatch = 0; SelectedFrame = 0; RevealedFrame = 0; SelectedMarker = FText(); FrameCache.Reset(); FrameRows.Reset(); AttemptOptions.Reset(); Selected = Row ? TOptional<FVaRestInspectorRowIdentity>(Row->Identity) : TOptional<FVaRestInspectorRowIdentity>(); Detail = {};
	if (auto* Inspector = IVaRestInspector::TryGet(); Inspector && Selected.IsSet()) Inspector->ForEachRecord([&](const auto& View)
	{ if (FVaRestInspectorRowIdentity{View.RequestId, View.ArrivalOrdinal, View.Kind} == Selected.GetValue()) ReadDetails(View); });
	ReadLinks();
}
void FVaRestInspectorViewModel::ReadQueueLinks()
{
 auto* Inspector=IVaRestInspector::TryGet(); if (!Inspector) return;
 TMap<uint64,TSharedPtr<FVaRestInspectorRowModel>> Requests;
 for (const auto& Pair:Cache) if (Pair.Key.Kind==EVaRestInspectorRecordKind::Http)
 { Pair.Value->Columns[11]=Pair.Value->OfflineBase; Requests.Add(Pair.Key.Id,Pair.Value); }
 TMap<uint64,uint64> Replays;
 Inspector->ForEachLink([&](const FVaRestInspectorLinkView& L)
 {
  if (L.bAuth || !L.ToRequestId || !L.FromRequestId || !Requests.Contains(L.ToRequestId)) return;
  uint64& Latest=Replays.FindOrAdd(L.ToRequestId); Latest=FMath::Max(Latest,L.FromRequestId);
 });
 for (const auto& Pair:Replays)
 {
  auto Original=Requests.FindRef(Pair.Key),Replay=Requests.FindRef(Pair.Value);
  const TCHAR* Outcome=!Replay ? TEXT("unavailable") : Replay->bRunning ? TEXT("pending") : Replay->bError ? TEXT("failed") : Replay->Status>=200 && Replay->Status<300 ? TEXT("ok") : TEXT("ended");
  Original->Columns[11]=FText::FromString(FString::Printf(TEXT("queued→#%llu %s"),Pair.Value,Outcome));
 }
}
void FVaRestInspectorViewModel::ReadLinks()
{
 LinkOptions.Reset(); LinkStatus=FText();
 if (!Selected.IsSet() || Selected->Kind!=EVaRestInspectorRecordKind::Http) return;
 auto* Inspector=IVaRestInspector::TryGet(); if (!Inspector) return;
 int32 Omitted=0;
 const bool Read=Inspector->ForEachLink([&](const FVaRestInspectorLinkView& L)
 {
  const bool Out=L.FromRequestId==Selected->Id,In=L.ToRequestId==Selected->Id;
  if (!Out && !In) return;
  if (LinkOptions.Num()>=128) { ++Omitted; return; }
  const uint64 Target=Out ? L.ToRequestId : L.FromRequestId; bool Available=false;
  for (const auto& Pair:Cache) if (Pair.Key.Kind==EVaRestInspectorRecordKind::Http && Pair.Key.Id==Target) { Available=true; break; }
  auto Option=MakeShared<FVaRestInspectorLinkOption>(); Option->TargetId=Target; Option->bAvailable=Target && Available;
  FString Label=L.bAuth ? Out ? FString(TEXT("Auth refresh")) : FString(TEXT("Parked request")) : Out ? FString(TEXT("Original queued request")) : FString(TEXT("Queue replay"));
  if (Target) Label+=FString::Printf(TEXT(" #%llu"),Target);
  if (!L.QueueId.IsEmpty()) Label+=TEXT(" · ")+Copy(L.QueueId,256);
  if (!Option->bAvailable) Label+=Out && L.bTargetEvicted ? TEXT(" · evicted") : Target ? TEXT(" · unavailable in this view") : TEXT(" · no request id from this process");
  Option->Label=FText::FromString(MoveTemp(Label)); LinkOptions.Add(MoveTemp(Option));
 });
 LinkStatus=FText::FromString(!Read ? FString(TEXT("Links are unavailable while capture is busy.")) : Omitted ? FString::Printf(TEXT("%d links · %d more omitted by view limit"),LinkOptions.Num(),Omitted) : LinkOptions.IsEmpty() ? FString(TEXT("No captured request links")) : FString::Printf(TEXT("%d request links"),LinkOptions.Num()));
}
TSharedPtr<FVaRestInspectorRowModel> FVaRestInspectorViewModel::RevealRequest(uint64 Id)
{
 check(IsInGameThread()); if (bDetached) return {};
 for (const auto& Pair:Cache) if (Pair.Key.Kind==EVaRestInspectorRecordKind::Http && Pair.Key.Id==Id)
 {
  auto Row=Pair.Value; Revealed=Row->Identity; Refilter(); Select(Row); return Row;
 }
 return {};
}
void FVaRestInspectorViewModel::SelectAttempt(uint64 Id)
{
 check(IsInGameThread()); SelectedDispatch=Id; Invalidate();
}
#include "VaRestInspectorStatsView.inl"
void FVaRestInspectorViewModel::ReadStats()
{
 StatsTable.Reset(); StatsPresentationBytes=0; bStatsPresentationOmitted=false;
 auto* Inspector=IVaRestInspector::TryGet(); TSharedPtr<const FVaRestInspectorSnapshot,ESPMode::ThreadSafe> Snapshot;
 if (!Inspector || !Inspector->TrySnapshot(Snapshot) || !Snapshot) { Stats=FText::FromString(TEXT("Statistics snapshot is unavailable under the current capture budget.")); return; }
 struct FLine { uint64 Completed=0; FString Text; };
 TArray<FLine> Top; FVaRestInspectorHttpStats Sum; int32 Groups=0; bool Unattributed=false;
 auto Add=[&](uint64& To,uint64 Value) { if (Value>MAX_uint64-To) { To=MAX_uint64; Sum.bSaturated=true; } else To+=Value; };
 Snapshot->ForEachStats([&](const FVaRestInspectorHttpStats& Item)
 {
  if (Scope.IsSet() && (Item.bOverflow || Item.ScopeId!=Scope.GetValue())) { Unattributed|=Item.bOverflow; return; }
  AddStatsRow(Item);
  ++Groups; Sum.bSaturated|=Item.bSaturated; Sum.bWireBytesUnknown|=Item.bWireBytesUnknown; Sum.bDurationCountsSaturated|=Item.bDurationCountsSaturated;
  Add(Sum.Started,Item.Started); Add(Sum.Completed,Item.Completed); Add(Sum.Success,Item.Success); Add(Sum.Error,Item.Error); Add(Sum.Queued,Item.Queued); Add(Sum.Canceled,Item.Canceled);
  Add(Sum.Dispatches,Item.Dispatches); Add(Sum.Retries,Item.Retries); Add(Sum.AuthReplays,Item.AuthReplays); Add(Sum.CacheHit,Item.CacheHit); Add(Sum.CacheDisk,Item.CacheDisk);
  Add(Sum.WireBytesUp,Item.WireBytesUp); Add(Sum.WireBytesDown,Item.WireBytesDown); Add(Sum.Partial,Item.Partial); Add(Sum.Incomplete,Item.Incomplete); Add(Sum.Dropped,Item.Dropped);
  if (Item.DurationCount) { if (!Sum.DurationCount) { Sum.DurationMin=Item.DurationMin; Sum.DurationMax=Item.DurationMax; } else { Sum.DurationMin=FMath::Min(Sum.DurationMin,Item.DurationMin); Sum.DurationMax=FMath::Max(Sum.DurationMax,Item.DurationMax); } }
  Add(Sum.DurationCount,Item.DurationCount); Add(Sum.DurationUnderflow,Item.DurationUnderflow); Add(Sum.DurationOverflow,Item.DurationOverflow); Add(Sum.DurationUnknown,Item.DurationUnknown); Add(Sum.DurationNonFinite,Item.DurationNonFinite);
  Sum.DurationSum+=Item.DurationSum;
  for (int32 I=0;I<512;++I) Add(Sum.Histogram[I],Item.Histogram[I]);
  const FString Name=Item.bOverflow ? FString(TEXT("Unattributed overflow")) : Item.Endpoint.IsNone() ? Item.Key.Left(256) : Item.Endpoint.ToString();
  FLine Line{Item.Completed,FString::Printf(TEXT("%s · scope %llu · client %llu: %llu completed, %llu errors, %llu attempts"),*Name,Item.ScopeId,Item.ClientId,Item.Completed,Item.Error,Item.Dispatches)};
  int32 At=0; while (At<Top.Num() && Top[At].Completed>=Line.Completed) ++At;
  if (At<32) { Top.Insert(MoveTemp(Line),At); if (Top.Num()>32) Top.Pop(EAllowShrinking::No); }
 });
 int32 SocketGroups=0;
 Snapshot->ForEachSocketStats([&](const FVaRestInspectorSocketStats& Item)
 {
  if (Scope.IsSet() && (!Item.bContextKnown || Item.ScopeId!=Scope.GetValue())) { Unattributed|=Item.bOverflow; return; }
  ++SocketGroups; AddStatsRow(Item);
 });
 SortStatsRows();
 const auto Quantile=[](const FVaRestInspectorQuantile& Q)
 {
  if (!Q.bHasValue) return FString(TEXT("unknown"));
  return Q.Kind==EVaRestInspectorQuantileKind::LogInterval ? FString::Printf(TEXT("~%.3f ms [%.3f, %.3f)"),Q.Value*1000,Q.Lower*1000,Q.Upper*1000)
   : Q.Kind==EVaRestInspectorQuantileKind::Zero ? FString(TEXT("0 ms")) : FString(TEXT("outside histogram range / saturated"));
 };
 const auto& D=Snapshot->GetDiagnostics();
 FString Out=FString::Printf(TEXT("Scope statistics · %d groups · independent of text filter and row eviction\n%llu started · %llu completed · %llu success · %llu errors (%.1f%%) · %llu queued · %llu canceled\n%llu dispatches · %llu retries · %llu auth replays · %llu memory/cache hits · %llu disk hits\nWire: %llu bytes up / %llu down%s\n%llu partial · %llu incomplete · %llu dropped\n"),Groups,Sum.Started,Sum.Completed,Sum.Success,Sum.Error,Sum.Completed ? double(Sum.Error)*100/double(Sum.Completed) : 0,Sum.Queued,Sum.Canceled,Sum.Dispatches,Sum.Retries,Sum.AuthReplays,Sum.CacheHit,Sum.CacheDisk,Sum.WireBytesUp,Sum.WireBytesDown,Sum.bWireBytesUnknown ? TEXT(" (some sizes unknown)") : TEXT(""),Sum.Partial,Sum.Incomplete,Sum.Dropped);
 Out+=TEXT("p50: ")+Quantile(Sum.Quantile(1,2))+TEXT("\np95: ")+Quantile(Sum.Quantile(95,100))+TEXT("\np99: ")+Quantile(Sum.Quantile(99,100));
 if (Sum.DurationCount && FMath::IsFinite(Sum.DurationSum)) Out+=FString::Printf(TEXT("\nMean %.3f ms · min %.3f ms · max %.3f ms"),Sum.DurationSum/double(Sum.DurationCount)*1000,Sum.DurationMin*1000,Sum.DurationMax*1000);
 if (Sum.bSaturated || Sum.bDurationCountsSaturated) Out+=TEXT("\nCounters or duration samples are saturated; ratios/quantiles may be incomplete.");
 if (Unattributed) Out+=TEXT("\nGlobal overflow cannot be attributed to this scope and is excluded.");
 Out+=FString::Printf(TEXT("\n\nGlobal capture budget: %lld / %lld bytes · peak %lld · pending %lld (peak %lld)\nLost %llu events / %llu bytes · %llu refused rows · %llu omitted metadata · %llu scopes · %llu stats groups\n"),D.StoreBytes,D.EffectiveStoreLimit,D.StorePeakBytes,D.PendingBytes,D.PendingPeakBytes,D.DroppedEvents,D.DroppedBytes,D.RefusedRows,D.MetadataOmitted,D.OmittedScopes,D.OmittedStatsGroups);
 Out+=FString::Printf(TEXT("\n%d socket groups; counters survive row/frame eviction. Partial captures exclude unseen history. Recent duplicate identity windows are bounded.\n"),SocketGroups);
 if (bStatsPresentationOmitted) Out+=TEXT("Table presentation limit omitted groups.\n");
 Out+=TEXT("\nTop 32 endpoint/client groups by completions\n"); for (const auto& Line:Top) Out+=Line.Text+TEXT("\n");
 Stats=FText::FromString(MoveTemp(Out));
}
void FVaRestInspectorViewModel::ReadDetails(const FVaRestInspectorRecordView& Row)
{
	if (Row.Kind==EVaRestInspectorRecordKind::Socket) { ReadSocketDetails(Row); return; }
	Detail.bSocket=false;
	const auto* Last = Row.Dispatches.IsEmpty() ? nullptr : &Row.Dispatches.Last();
	bool MissingAttempt = false;
	if (SelectedDispatch) { const auto* Found = Row.Dispatches.FindByPredicate([&](const auto& Item) { return Item.DispatchId == SelectedDispatch; }); if (Found) Last = Found; else MissingAttempt = true; }
	TArray<TSharedPtr<FVaRestInspectorAttemptOption>> Options;
	auto AddOption = [&](uint64 Id, FString Label)
	{
		TSharedPtr<FVaRestInspectorAttemptOption> Value; for (const auto& Old : AttemptOptions) if (Old->Id == Id) { Value = Old; break; }
		if (!Value) Value = MakeShared<FVaRestInspectorAttemptOption>(); Value->Id = Id; Value->Label = FText::FromString(MoveTemp(Label)); Options.Add(MoveTemp(Value));
	};
	AddOption(0, TEXT("Latest retained attempt"));
	for (const auto& Item : Row.Dispatches) AddOption(Item.DispatchId, FString::Printf(TEXT("Attempt %d · dispatch %llu · status %d"), Item.Attempt, Item.DispatchId, Item.Ended.Fixed ? Item.Ended.Fixed->Scalars.Status : 0));
	AttemptOptions = MoveTemp(Options);
	Detail.AttemptLabel = FText::FromString(MissingAttempt ? TEXT("Selected attempt evicted; showing latest") : Last ? FString::Printf(TEXT("Attempt %d · dispatch %llu"), Last->Attempt, Last->DispatchId) : TEXT("No retained network attempt"));
	const FVaRestInspectorPacketView Request = bRequestAsBuilt ? Row.Started : Last ? Last->Dispatched : Row.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched ? Row.Started : FVaRestInspectorPacketView();
	Detail.Curl = FVaRestInspectorCurl::Build(Row, MissingAttempt ? SelectedDispatch : Last ? Last->DispatchId : 0);
	Detail.Url = Copy(Row.Started.Text(EText::Url), 8192); Detail.Id = LexToString(Row.RequestId);
	Detail.Overview = FText::FromString(FString::Printf(TEXT("#%llu · %s\n%s %s\nScope: %s · Client: %s\nStatus: %d · Attempts: %llu\n%llu bytes sent · %llu bytes received\n%s\n%s"),
		Row.RequestId, *State(Row.EndState), *Text(Row.Started, EText::Verb), *Text(Row.Started, EText::Url), *Copy(Row.ScopeLabel), *Copy(Row.ClientLabel), Row.FinalStatus,
		Row.ObservedDispatches, Row.WireBytesUp, Row.WireBytesDown, *Text(Row.Completed, EText::ErrorCode), *Text(Row.Completed, EText::ErrorMessage)));
	const auto* Started = Row.Started.Fixed; const auto* Completed = Row.Completed.Fixed;
	FString Metadata = FString::Printf(TEXT("\n\nContext known: %s · Scope #%llu · Client #%llu\nStart UTC: %s\nDuration: %s · Partial capture: %s · Metadata omitted: %s\n%llu older attempts omitted · wire byte sizes %s"),
		Row.bContextKnown ? TEXT("yes") : TEXT("no"), Row.ScopeId, Row.ClientId, Row.bStartUtcKnown ? *Row.StartUtc.ToIso8601() : TEXT("unknown"),
		Row.Total.bKnown ? *FString::Printf(TEXT("%.3f ms"), Row.Total.Seconds * 1000) : TEXT("unknown"), Row.bPartial ? TEXT("yes") : TEXT("no"), Row.bMetadataOmitted ? TEXT("yes") : TEXT("no"), Row.OmittedDispatches, Row.bWireBytesUnknown ? TEXT("partly unknown") : TEXT("observed"));
	if (Started) Metadata += TEXT("\nEndpoint: ") + Started->Scalars.Endpoint.ToString() + TEXT(" · Template: ") + Text(Row.Started, EText::PathTemplate)
		+ TEXT("\nTag: ") + Started->Scalars.Tag.ToString() + TEXT(" · Cache policy: ") + Started->Scalars.CachePolicy.ToString()
		+ FString::Printf(TEXT(" · Priority: %d\nFlags: 0x%02x · Idempotent %s · Queue when offline %s · No auth %s · Auth refresh %s"), int32(Started->Scalars.Priority), uint32(Started->Scalars.Flags),
			EnumHasAnyFlags(Started->Scalars.Flags, EVaRestTraceRequestFlags::Idempotent) ? TEXT("yes") : TEXT("no"), EnumHasAnyFlags(Started->Scalars.Flags, EVaRestTraceRequestFlags::QueueWhenOffline) ? TEXT("yes") : TEXT("no"),
			EnumHasAnyFlags(Started->Scalars.Flags, EVaRestTraceRequestFlags::NoAuth) ? TEXT("yes") : TEXT("no"), EnumHasAnyFlags(Started->Scalars.Flags, EVaRestTraceRequestFlags::AuthRefresh) ? TEXT("yes") : TEXT("no"));
	if (Completed) Metadata += FString::Printf(TEXT("\nError kind: %d · Transport failure: %d · Detail: %s · Cancel reason: %d\nCache lookup: %d · From cache: %s · Response storage: %d"), int32(Completed->Scalars.ErrorKind), int32(Completed->Scalars.Failure), *Completed->Scalars.FailureDetail.ToString(), int32(Completed->Scalars.CancelReason), int32(Completed->Scalars.CacheLookup), Completed->Scalars.bFromCache ? TEXT("yes") : TEXT("no"), int32(Completed->Scalars.Storage));
	Detail.Overview = FText::FromString(Detail.Overview.ToString() + Metadata);
	Detail.RequestBody = TEXT("(request excerpt unavailable)");
	Detail.RequestSource = {}; Detail.AttemptResponseSource = {};
	for (const auto& Excerpt : Row.Excerpts) if (Excerpt.Fixed && Excerpt.Fixed->Scalars.Owner == EVaRestTraceExcerptOwner::Request)
	{ Detail.RequestBody = Body(Excerpt); Detail.RequestSource = BodySource(Excerpt); }
	if (Last && Last->ResponseExcerptState == EVaRestInspectorDetailState::Present) Detail.AttemptResponseSource = BodySource(Last->ResponseExcerpt);
	Detail.ResponseBody = Last && Row.FinalPreviewState == EVaRestInspectorDetailState::Present ? Body(Last->ResponseExcerpt)
		: Row.FinalPreviewState == EVaRestInspectorDetailState::NoProducer ? TEXT("(the delivered entity has no preview producer)") : TEXT("(delivered preview unavailable)");
	FString QueryRows, BeforeQuery, CapturedQuery;
	if (Detail.Url.Split(TEXT("?"), &BeforeQuery, &CapturedQuery))
	{
		FString Fragment; if (CapturedQuery.Contains(TEXT("#"))) CapturedQuery.Split(TEXT("#"), &CapturedQuery, &Fragment);
		TArray<FString> Params; CapturedQuery.ParseIntoArray(Params, TEXT("&"), false);
		for (int32 I = 0; I < FMath::Min(Params.Num(), 256); ++I) { FString Name, Value; if (!Params[I].Split(TEXT("="), &Name, &Value)) Name = Params[I]; QueryRows += Name + TEXT(": ") + Value + TEXT("\n"); }
		if (Params.Num() > 256) QueryRows += TEXT("(additional parameters omitted)\n");
	}
	if (QueryRows.IsEmpty()) QueryRows = TEXT("(none captured)\n");
	Detail.Request = FText::FromString((bRequestAsBuilt ? FString(TEXT("As-built headers\n")) : Detail.AttemptLabel.ToString() + TEXT(" · dispatched headers\n")) + Detail.Url + TEXT("\n\nQuery parameters (URL encoded)\n") + QueryRows + TEXT("\nHeaders\n") + Headers(Request) + TEXT("\n") + Detail.RequestBody);
	const FString AttemptBody = Last && Last->ResponseExcerptState == EVaRestInspectorDetailState::Present ? Body(Last->ResponseExcerpt) : FString(TEXT("(attempt excerpt unavailable)"));
	Detail.Response = FText::FromString(TEXT("Delivered response\n") + Headers(Row.Completed) + TEXT("\n") + Detail.ResponseBody
		+ TEXT("\n\nSelected network attempt response\n") + Detail.AttemptLabel.ToString()
		+ (Last && Last->Ended.Fixed ? FString::Printf(TEXT(" · status %d\n"), Last->Ended.Fixed->Scalars.Status) + Text(Last->Ended, EText::EffectiveUrl) + TEXT("\n") : FString(TEXT("\n"))) + (Last ? Headers(Last->Ended) : FString(TEXT("(no retained network attempt)\n"))) + TEXT("\n") + AttemptBody + TEXT("\n\nDecode / Envelope\n")
		+ TEXT("Type: ") + (Row.Completed.Fixed ? Row.Completed.Fixed->Scalars.ResultType.ToString() : FString(TEXT("unknown")))
		+ (Row.Decode.bKnown ? FString::Printf(TEXT(" · decode %.3f ms"), Row.Decode.Seconds * 1000) : FString(TEXT(" · decode time unknown")))
		+ TEXT("\nData path: ") + Text(Row.Completed, EText::DataPath) + TEXT("\nPredicate: ") + Text(Row.Completed, EText::PredicatePath)
		+ TEXT("\nExpected: ") + Text(Row.Completed, EText::Expected) + TEXT("\nActual: ") + Text(Row.Completed, EText::Actual)
		+ TEXT("\nCode: ") + Text(Row.Completed, EText::EnvelopeCode) + TEXT("\nMessage: ") + Text(Row.Completed, EText::EnvelopeMessage));
	FString Warnings; int32 Displayed = 0;
	if (Row.Completed.Fixed) for (const auto& Pair : Row.Completed.Pairs(Row.Completed.Fixed->Warnings))
	{ Warnings += TEXT("\n") + Copy(Row.Completed.Read(Pair.First), 512) + TEXT(": ") + Copy(Row.Completed.Read(Pair.Second), 1024); ++Displayed; }
	if (Row.Completed.Fixed && Row.Completed.Fixed->Scalars.DecodeWarningCount > Displayed) Warnings += FString::Printf(TEXT("\n+%d more warnings not retained"), Row.Completed.Fixed->Scalars.DecodeWarningCount - Displayed);
	Detail.Response = FText::FromString(Detail.Response.ToString() + TEXT("\n\nDecode warnings") + (Warnings.IsEmpty() ? FString(TEXT("\n(none retained)")) : Warnings));
	FString Timing; Detail.Waterfall.Reset(); Detail.WaterfallBegin = TNumericLimits<double>::Max(); Detail.WaterfallEnd = -TNumericLimits<double>::Max();
	for (const auto& Attempt : Row.Dispatches)
	{
		Timing += FString::Printf(TEXT("Attempt %d · dispatch %llu\n"), Attempt.Attempt, Attempt.DispatchId);
		const FVaRestInspectorPhase* Phases[] = {&Attempt.Gap, &Attempt.Auth, &Attempt.Queued, &Attempt.Prepare, &Attempt.Send, &Attempt.Wait, &Attempt.Receive};
		const TCHAR* Names[] = {TEXT("Gap"), TEXT("Auth"), TEXT("Queued"), TEXT("Prepare"), TEXT("Send"), TEXT("Wait"), TEXT("Receive")};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Phases); ++Index) Timing += Phases[Index]->bKnown ? FString::Printf(TEXT("  %s: %.3f ms\n"), Names[Index], Phases[Index]->Seconds * 1000) : FString::Printf(TEXT("  %s: unknown\n"), Names[Index]);
		FVaRestInspectorWaterfallLane Lane; bool Unknown = false;
		const double Starts[] = {Attempt.AttemptStart - Attempt.Gap.Seconds, Attempt.AttemptStart,
			Attempt.Auth.bKnown ? Attempt.AttemptStart + Attempt.Auth.Seconds : Attempt.Prepare.bKnown ? Attempt.DispatchAt - Attempt.Prepare.Seconds - Attempt.Queued.Seconds : -1,
			Attempt.DispatchAt - Attempt.Prepare.Seconds, Attempt.DispatchAt,
			Attempt.UploadDoneAt >= 0 ? Attempt.UploadDoneAt : Attempt.bCombinedSendWait ? Attempt.DispatchAt : -1, Attempt.StatusAt};
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(Phases); ++Index)
		{
			const auto& Phase = *Phases[Index]; const double Start = Starts[Index];
			if (!Phase.bKnown || !FMath::IsFinite(Phase.Seconds) || Phase.Seconds < 0 || !FMath::IsFinite(Start) || Start < 0) { Unknown = true; continue; }
			if (Phase.Seconds <= 0) continue;
			Lane.Segments.Add({Start, Phase.Seconds, static_cast<uint8>(Index)});
			Detail.WaterfallBegin = FMath::Min(Detail.WaterfallBegin, Start); Detail.WaterfallEnd = FMath::Max(Detail.WaterfallEnd, Start + Phase.Seconds);
		}
		if (Attempt.bCombinedSendWait) Timing += TEXT("  Send/Wait are combined by the transport.\n");
		Lane.Label = FText::FromString(FString::Printf(TEXT("Attempt %d%s"), Attempt.Attempt, Unknown ? TEXT(" ?") : TEXT(""))); Detail.Waterfall.Add(MoveTemp(Lane));
	}
	if (!FMath::IsFinite(Detail.WaterfallEnd - Detail.WaterfallBegin) || Detail.WaterfallEnd < Detail.WaterfallBegin) Detail.WaterfallBegin = Detail.WaterfallEnd = 0;
	Timing += TEXT("\nTimeline (trace clock seconds)\n");
	const TCHAR* KindNames[] = {TEXT("Started"),TEXT("Dispatched"),TEXT("Attempt ended"),TEXT("Note"),TEXT("Completed"),TEXT("Excerpt"),TEXT("Scope"),TEXT("Client"),TEXT("Marker"),TEXT("Socket"),TEXT("Socket attempt"),TEXT("Socket frame"),TEXT("Socket note")};
	for (const auto& Event : Row.Timeline) Timing += FString::Printf(TEXT("%.6f · %s · seq %llu · attempt %d · dispatch %llu · detail %d\n"), Event.Time, static_cast<int32>(Event.Kind)<UE_ARRAY_COUNT(KindNames) ? KindNames[static_cast<int32>(Event.Kind)] : TEXT("Unknown"), Event.Seq, Event.Attempt, Event.DispatchId, static_cast<int32>(Event.DetailState));
	const TCHAR* NoteNames[] = {TEXT("Cache revalidated"),TEXT("Cache stored"),TEXT("Cache served stale"),TEXT("Retry scheduled"),TEXT("Queued offline"),TEXT("Queue expired")};
	for (const auto& Note : Row.Notes) if (Note.Fixed) Timing += FString::Printf(TEXT("%s · %.6f s · delay %.3f s · "), static_cast<int32>(Note.Fixed->Scalars.NoteKind)<UE_ARRAY_COUNT(NoteNames) ? NoteNames[static_cast<int32>(Note.Fixed->Scalars.NoteKind)] : TEXT("Unknown note"), Note.Fixed->Header.Time, Note.Fixed->Scalars.Delay) + Note.Fixed->Scalars.Reason.ToString() + TEXT(" ") + Text(Note, EText::QueueId) + TEXT("\n");
	if (Row.OmittedTimeline || Row.OmittedNotes) Timing += FString::Printf(TEXT("%llu older timeline entries / %llu notes omitted\n"), Row.OmittedTimeline, Row.OmittedNotes);
	Detail.Timing = FText::FromString(MoveTemp(Timing));
	Detail.ScriptObject = Text(Row.Started, EText::ScriptObject); Detail.ScriptFunction = Text(Row.Started, EText::ScriptFunction); Detail.SourceFile = Text(Row.Started, EText::SourceFile);
	Detail.ScriptCodeOffset = Row.Started.Fixed ? Row.Started.Fixed->Scalars.ScriptCodeOffset : INDEX_NONE; Detail.SourceLine = Row.Started.Fixed ? Row.Started.Fixed->Scalars.SourceLine : 0;
	Detail.Overview = FText::FromString(Detail.Overview.ToString() + TEXT("\n\nInitiator\n") + Detail.ScriptObject + TEXT("\n") + Detail.ScriptFunction
		+ FString::Printf(TEXT(" (offset %d)\n"), Detail.ScriptCodeOffset) + Detail.SourceFile + FString::Printf(TEXT(":%d"), Detail.SourceLine));
}

#include "VaRestInspectorSocketView.inl"
