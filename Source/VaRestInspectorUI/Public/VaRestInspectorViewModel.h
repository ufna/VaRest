// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestInspector.h"
#include "VaRestInspectorFilter.h"
#include "VaRestInspectorBodyViewModel.h"

/** No Slate types: all borrowed capture views are consumed synchronously into bounded presentation values. */
enum class EVaRestInspectorColumn : uint8 { Status, Method, Name, Initiator, Duration, Size, Attempts, Scope, Client, Url, Cache, Offline, Tag, Id, Time, Count };
enum class EVaRestInspectorKindFilter : uint8 { All, Http, WebSocket, SocketIO, Errors, Markers };
struct FVaRestInspectorRowIdentity
{
	uint64 Id = 0, Arrival = 0;
	EVaRestInspectorRecordKind Kind = EVaRestInspectorRecordKind::Http;
	bool operator==(const FVaRestInspectorRowIdentity&) const = default;
	friend uint32 GetTypeHash(const FVaRestInspectorRowIdentity& Key)
	{ return HashCombine(HashCombine(GetTypeHash(Key.Id), GetTypeHash(Key.Arrival)), GetTypeHash(static_cast<uint8>(Key.Kind))); }
};
struct FVaRestInspectorRowModel
{
	FVaRestInspectorRowIdentity Identity;
	uint64 Revision = 0, ScopeId = 0, ClientId = 0;
	FString Method, Name, Url, Initiator, Scope, Client, Tag, Searchable;
	FText Columns[static_cast<int32>(EVaRestInspectorColumn::Count)];
	FText Tooltip;
	FText OfflineBase;
	double Started = 0, Duration = -1;
	uint64 Bytes = 0, Attempts = 0;
	int32 Status = 0;
	bool bContextKnown = false, bError = false, bRunning = false, bCached = false, bQueued = false, bAuth = false;
	int64 PresentationBytes() const;
};
struct FVaRestInspectorFrameRowModel
{
	uint64 Id = 0, AttemptId = 0, MetadataSeq = 0, ExcerptSeq = 0;
	FText Columns[11];
	FString Channel, Route, MessageId, Protocol, Searchable;
	EVaRestTraceSocketMessageKind MessageKind = EVaRestTraceSocketMessageKind::Message;
	EVaRestInspectorDetailState ExcerptState = EVaRestInspectorDetailState::Absent;
	bool bMetadataOmitted = false, bLinkable = false;
	bool bSynthetic = false, bAutoPong = false;
	double Clock = -1;
	EVaRestTraceSocketDirection Direction = EVaRestTraceSocketDirection::In;
	EVaRestTraceSocketFrameKind FrameKind = EVaRestTraceSocketFrameKind::Text;
	FText MarkerDescription;
	int64 PresentationBytes() const;
};
struct FVaRestInspectorScopeOption
{
	uint64 Id = 0;
	bool bAll = false;
	FText Label;
};
struct FVaRestInspectorWaterfallSegment
{
	double Start = 0, Seconds = 0;
	uint8 Phase = 0; // gap, auth, queue, prepare, send, wait, receive
};
struct FVaRestInspectorWaterfallLane
{
	FText Label;
	TArray<FVaRestInspectorWaterfallSegment> Segments;
};
struct FVaRestInspectorDetailModel
{
	FText Overview, Request, Response, Timing;
	FText Connection, FrameOverview, FrameStatus;
	FVaRestInspectorBodySource FrameSource;
	uint64 LinkedFrameId = 0;
	bool bSocket = false;
	FText AttemptLabel;
	FString Curl, Url, Id;
	TArray<FVaRestInspectorWaterfallLane> Waterfall;
	double WaterfallBegin = 0, WaterfallEnd = 0;
	FString RequestBody, ResponseBody;
	FVaRestInspectorBodySource RequestSource, AttemptResponseSource;
	FString ScriptObject, ScriptFunction, SourceFile;
	int32 ScriptCodeOffset = INDEX_NONE, SourceLine = 0;
};
struct FVaRestInspectorAttemptOption
{
	uint64 Id = 0; // zero selects the latest retained physical attempt
	FText Label;
};
struct FVaRestInspectorStatsCell
{
	FText Text;
	uint64 Count = 0;
	double Number = 0;
	uint8 Kind = 0; // unknown, text, unsigned counter, finite number
};
struct FVaRestInspectorStatsRowModel
{
	static constexpr int32 ColumnCount = 23;
	FVaRestInspectorStatsCell Columns[ColumnCount];
};
struct FVaRestInspectorLinkOption
{
	uint64 TargetId = 0;
	bool bAvailable = false;
	FText Label;
};

class VARESTINSPECTORUI_API FVaRestInspectorViewModel final : public TSharedFromThis<FVaRestInspectorViewModel>
{
public:
	static TSharedRef<FVaRestInspectorViewModel> Create();
	~FVaRestInspectorViewModel();
	void Detach();
	void SetWake(TFunction<void()> Callback) { Wake = MoveTemp(Callback); }
	void Invalidate(uint64 Revision = 0);
	bool NeedsRefresh() const { return bDirty || bFilterPending; }
	bool Refresh(double Now);
	void SetFilter(FString Text, double Now);
	void SetScope(TOptional<uint64> Id);
	void SetKind(EVaRestInspectorKindFilter Value) { Revealed.Reset(); Kind = Value; Invalidate(); }
	void SetHideEngineIo(bool Value) { RevealedFrame = 0; bHideEngineIo = Value; Invalidate(); }
	void SetFrameFilter(FString Text) { RevealedFrame = 0; FrameTerms = FVaRestInspectorFilter::Tokenize(Text.Left(1024)); Invalidate(); }
	void SetHideControl(bool Value) { RevealedFrame = 0; bHideControl = Value; Invalidate(); }
	void Sort(EVaRestInspectorColumn Column, bool bAscending);
	void Select(TSharedPtr<FVaRestInspectorRowModel> Row);
	TSharedPtr<FVaRestInspectorRowModel> RevealRequest(uint64 Id);
	const TArray<TSharedPtr<FVaRestInspectorLinkOption>>& Links() const { return LinkOptions; }
	const FText& LinksText() const { return LinkStatus; }
	void SelectAttempt(uint64 DispatchId);
	void SelectFrame(uint64 Id) { SelectedMarker = FText(); SelectedFrame = Id; Invalidate(); }
	void RevealFrame(uint64 Id) { RevealedFrame = Id; SelectFrame(Id); }
	void SelectFrameMarker(FText Text) { SelectedFrame = 0; SelectedMarker = MoveTemp(Text); Invalidate(); }
	void SetRequestAsBuilt(bool Value) { bRequestAsBuilt = Value; Invalidate(); }
	void EnableStats(bool Value) { bStatsEnabled = Value; if (Value) Invalidate(); }
	const TArray<TSharedPtr<FVaRestInspectorRowModel>>& Rows() const { return Visible; }
	const TArray<TSharedPtr<FVaRestInspectorScopeOption>>& Scopes() const { return ScopeOptions; }
	const FVaRestInspectorDetailModel& Details() const { return Detail; }
	const TArray<TSharedPtr<FVaRestInspectorAttemptOption>>& Attempts() const { return AttemptOptions; }
	const FText& StatsText() const { return Stats; }
	const TArray<TSharedPtr<FVaRestInspectorStatsRowModel>>& StatsRows() const { return StatsTable; }
	void SortStats(int32 Column, bool bAscending);
	FString StatsTsv() const;
	const TArray<TSharedPtr<FVaRestInspectorFrameRowModel>>& Frames() const { return FrameRows; }
	const FText& StatusText() const { return Status; }
	bool IsCapturing() const { return bCapturing; }
	bool IsAttached() const { return !bDetached; }
private:
	FVaRestInspectorViewModel() = default;
	bool Matches(const FVaRestInspectorRowModel& Row) const;
	void Refilter();
	void RefreshScopes();
	void ReadStats();
	void ReadLinks();
	void ReadQueueLinks();
	void AddStatsRow(const FVaRestInspectorHttpStats&);
	void AddStatsRow(const FVaRestInspectorSocketStats&);
	void AddStatsPresentation(TSharedRef<FVaRestInspectorStatsRowModel>);
	void SortStatsRows();
	void ReadDetails(const FVaRestInspectorRecordView& Row);
	void ReadSocketDetails(const FVaRestInspectorRecordView& Row);
	bool MatchesFrame(const FVaRestInspectorFrameRowModel& Row) const;
	TMap<FVaRestInspectorRowIdentity, TSharedPtr<FVaRestInspectorRowModel>> Cache;
	TArray<TSharedPtr<FVaRestInspectorRowModel>> Visible;
	TArray<TSharedPtr<FVaRestInspectorScopeOption>> ScopeOptions;
	TOptional<uint64> Scope;
	TOptional<FVaRestInspectorRowIdentity> Selected;
	TOptional<FVaRestInspectorRowIdentity> Revealed;
	TArray<TSharedPtr<FVaRestInspectorLinkOption>> LinkOptions;
	FText LinkStatus;
	FVaRestInspectorDetailModel Detail;
	TMap<uint64, TSharedPtr<FVaRestInspectorFrameRowModel>> FrameCache;
	TArray<TSharedPtr<FVaRestInspectorFrameRowModel>> FrameRows;
	TArray<FString> FrameTerms;
	FText SelectedMarker;
	uint64 SelectedFrame = 0;
	uint64 RevealedFrame = 0;
	EVaRestInspectorKindFilter Kind = EVaRestInspectorKindFilter::All;
	bool bHideEngineIo = true;
	bool bHideControl = false;
	TArray<TSharedPtr<FVaRestInspectorAttemptOption>> AttemptOptions;
	FText Stats;
	TArray<TSharedPtr<FVaRestInspectorStatsRowModel>> StatsTable;
	int64 StatsPresentationBytes = 0;
	int32 StatsSortColumn = 1;
	bool bStatsAscending = true, bStatsPresentationOmitted = false;
	uint64 SelectedDispatch = 0;
	bool bRequestAsBuilt = false, bStatsEnabled = false;
	FText Status;
	FString Filter, PendingFilter;
	FVaRestInspectorFilter Query;
	TFunction<void()> Wake;
	FDelegateHandle Changed;
	double FilterAt = 0;
	EVaRestInspectorColumn SortColumn = EVaRestInspectorColumn::Time;
	bool bDirty = true, bFilterPending = false, bAscending = true, bCapturing = false, bDetached = false;
	static constexpr int32 MaxRows = 10000;
	static constexpr int64 MaxPresentationBytes = 8 * 1024 * 1024;
};
