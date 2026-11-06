// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Misc/Optional.h"
#include "VaRestInspectorStore.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
struct FSnapshotPacket;
struct FSnapshotRowCopy;
struct FSnapshotRowEntry;

enum class ESnapshotAllocationKind : uint8
{
	RowEntries,
	MutableRow,
	Dispatches,
	Packets,
	Notes,
	Timeline,
	SocketAttempt, SocketFrame, SocketChannel,
	SocketStats,
	StatsGroups,
	Links,
	Raw
};

class FSnapshotBuilder
{
public:
	using FRow = FStore::FRow;
	using ELivenessEvidence = FStore::ELivenessEvidence;

	FSnapshotBuilder();
	~FSnapshotBuilder();
	FSnapshotBuilder(const FSnapshotBuilder&) = delete;
	FSnapshotBuilder& operator=(const FSnapshotBuilder&) = delete;

	bool Begin(const FStore& Store, int32 InRowCount);
	bool MeasureSealedRow(FStringView Label, FStringView Scope = {});
	bool MeasureMutableRow(FStringView Label, FStringView Scope = {});
	bool MeasureDispatches(int32 Count);
	bool MeasureHeaders(int32 Count);
	bool MeasureNotes(int32 Count);
	bool MeasureTimeline(int32 Count);
	bool MeasurePacket(const FVaRestInspectorPacketView& Source,
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt);
	bool MeasureSocketAttempt(const FVaRestInspectorSocketAttemptView&);
	bool MeasureSocketChannel(const FVaRestInspectorPacketView&);
	bool ConstructSocketChannel(FSnapshotRowCopy&,const FVaRestInspectorPacketView&);
	bool ConstructSocketAttempt(FSnapshotRowCopy&,const FVaRestInspectorSocketAttemptView&);
	bool MeasureSocketFrame(const FVaRestInspectorSocketFrameView&,const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>*,const FVaRestInspectorPacketView&,const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>*);
	bool ConstructSocketFrame(FSnapshotRowCopy&,const FVaRestInspectorSocketFrameView&,const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>*,const FVaRestInspectorPacketView&,const TSharedPtr<const TArray<uint8>,ESPMode::ThreadSafe>*);
	bool MeasureStats(int32 Count);
	bool MeasureSocketStats(int32 Count);
	bool ConstructSocketStats(int32 Count);
	bool ConstructSocketStatsGroup(const FVaRestInspectorSocketStats&,int32 Destination);
	bool MeasureStatsGroup(const FStatsGroupSlot& Group);
	bool ConstructStats(int32 Count);
	bool ConstructStatsGroup(const FStatsGroupSlot& Group, int32 Destination);
	bool MeasureLinks(int32 Count);
	bool MeasureLink(const FStoreLinkView& Link);
	bool ConstructLinks(int32 Count);
	bool ConstructLink(const FStoreLinkView& Link, int32 Destination);
	bool HasUnsupportedPublicStrings() const { return bUnsupportedPublicStrings; }
	bool MeasuredCosts(int64& Outer, int64& Nested) const;
	static int32 PrivateStatsCount(const FVaRestInspectorSnapshot& Snapshot);
	static bool HasPrivateStatsGroup(const FVaRestInspectorSnapshot& Snapshot, const FStatsKeyView& Key);
	static bool PrivateStatsHistogramsAreZero(const FVaRestInspectorSnapshot& Snapshot);
	bool FinishMeasurement();
	bool Reserve();
	FSnapshotRowCopy* ConstructMutableRow();
	bool ConstructDispatches(FSnapshotRowCopy& Copy, int32 Count);
	bool ConstructHeaders(FSnapshotRowCopy& Copy, int32 Count);
	bool ConstructNotes(FSnapshotRowCopy& Copy, int32 Count);
	bool ConstructTimeline(FSnapshotRowCopy& Copy, int32 Count);
	bool ConstructPacket(const FVaRestInspectorPacketView& Source,
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt, FSnapshotPacket& Out);
	void ConstructScalarPacket(const FPacketFixed& Source, FSnapshotPacket& Out);
	bool ConstructSealedEntry(const TSharedPtr<const FRow, ESPMode::ThreadSafe>& Owner, FStringView Label, FStringView Scope = {});
	bool ConstructMutableEntry(FSnapshotRowCopy* Copy, FStringView Label, FStringView Scope = {});
	bool Publish(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out);
	static void DestroyDomain(FVaRestInspectorSnapshot::FData& Data, FAllocationAdapter& Allocator);

private:
	bool MeasurePublicString(FStringView Source);
	bool ConstructPublicString(FStringView Source, FString& Out);
	bool AddNestedBytes(int64 Amount);
	bool AddBuiltBytes(SIZE_T Amount);
	bool MeasureAllocation(SIZE_T Request, uint32 Alignment);
	void* AllocateChild(SIZE_T Request, uint32 Alignment, ESnapshotAllocationKind Kind);
	void SetConstructed(void* Address, int32 Count);
	bool EnsureDirectoryPage();
	bool AddRecord(void* Address, ESnapshotAllocationKind Kind);
	bool EnsureEntrySegment();
	FSnapshotRowEntry* NextEntry();
	bool CopyLabel(FStringView Label, FSnapshotRowEntry& Entry, bool bScope = false);

	TRefCountPtr<FBudgetAccount> Account;
	FAllocationAdapter* Allocator = nullptr;
	FStoreCredit Controller, Payload;
	TUniquePtr<FVaRestInspectorSnapshot::FData, FVaRestInspectorSnapshot::FDataDeleter> Data;
	uint64 Revision = 0;
	FVaRestInspectorDiagnostics Diagnostics;
	TOptional<SIZE_T> PayloadBound, DataBound;
	int64 NestedBytes = 0, BuiltNestedBytes = 0;
	int32 RowCount = 0, EntriesPerSegment = 0;
	int32 NumEntrySegments = 0, BuiltEntrySegments = 0, BuiltEntries = 0;
	int32 NumAllocationRecords = 0, BuiltAllocationRecords = 0;
	int32 NumDirectoryPages = 0, BuiltDirectoryPages = 0;
	int32 NumMutableRows = 0, BuiltMutableRows = 0;
	int32 NumExcerptReferences = 0, BuiltExcerptReferences = 0;
	int32 NumPublicText = 0, BuiltPublicText = 0;
	bool bMeasurementFinished = false, bReserved = false;
	bool bUnsupportedPublicStrings = false;
};

struct FSnapshotPacket
{
	FPacketFixed Fixed;
	const uint8* Backing = nullptr;
	int32 BackingBytes = 0;
	int32 ExcerptBytes = 0;
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Excerpt;
	bool bPresent = false;
};

struct FSnapshotSocketAttempt
{
 FSnapshotSocketAttempt* Next=nullptr;
 uint64 Id=0;
 bool Partial=false,HasConnecting=false,HasOpened=false,HasClosed=false;
 FSnapshotPacket Connecting,Opened,Closed;
};
struct FSnapshotSocketFrame
{
 FSnapshotSocketFrame* Next=nullptr;
 uint64 Id=0,Attempt=0;
 EVaRestInspectorDetailState State=EVaRestInspectorDetailState::Absent;
 FSnapshotPacket Metadata,Excerpt;
};
struct FSnapshotSocketChannel
{
 FSnapshotSocketChannel* Next=nullptr;
 FSnapshotPacket Packet;
};
struct FSnapshotDispatch
{
	uint64 DispatchId = 0;
	int32 Attempt = 0;
	bool bHasDispatched = false, bHasEnded = false;
	FPacketFixed DispatchedFixed, EndedFixed;
	EVaRestInspectorHeadersState HeadersState = EVaRestInspectorHeadersState::Unknown;
	EVaRestInspectorDetailState ResponseExcerptState = EVaRestInspectorDetailState::Absent;
	FSnapshotPacket Ended, ResponseExcerpt;
	int32 HeaderIndex = INDEX_NONE;
};

struct FSnapshotNote
{
	FSnapshotPacket Packet;
	uint64 Serial = 0;
};

struct FSnapshotTimeline
{
	FVaRestInspectorTimelineEntry Value;
	uint64 NoteSerial = 0;
};

struct FSnapshotRowCopy
{
 FSnapshotSocketChannel* SocketChannels=nullptr;
 FSnapshotSocketChannel* SocketChannelsTail=nullptr;
 FSnapshotSocketAttempt* SocketAttempts=nullptr;
 FSnapshotSocketAttempt* SocketAttemptsTail=nullptr;
 FSnapshotSocketFrame* SocketFrames=nullptr;
 FSnapshotSocketFrame* SocketFramesTail=nullptr;
	FVaRestInspectorRecordView Value;
	FSnapshotBuilder::ELivenessEvidence Evidence = FSnapshotBuilder::ELivenessEvidence::None;
	uint64 PresenceStamp = 0;
	int32 IndexSlot = INDEX_NONE, DeadlineIndex = INDEX_NONE;
	FPacketFixed CompletionFixed;
	FSnapshotPacket Started, Completed, RequestExcerpt, Marker;
	bool bHasStarted = false, bHasCompletedOwner = false, bHasRequestExcerpt = false, bHasMarker = false;
	FSnapshotDispatch* Dispatches = nullptr;
	FSnapshotPacket* Headers = nullptr;
	FSnapshotNote* Notes = nullptr;
	FSnapshotTimeline* Timeline = nullptr;
	int32 NumDispatches = 0, NumHeaders = 0;
	int32 NoteHead = 0, NumNotes = 0, NoteSlots = 0;
	int32 TimelineHead = 0, NumTimeline = 0, TimelineSlots = 0;
	uint64 NextNoteSerial = 0;
	int32 PreviousDispatchSlot = INDEX_NONE;
};

struct FSnapshotRowEntry
{
	TSharedPtr<const FSnapshotBuilder::FRow, ESPMode::ThreadSafe> Sealed;
	FSnapshotRowCopy* Mutable = nullptr;
	const TCHAR* ClientLabel = nullptr;
	int32 ClientLabelChars = 0;
	const TCHAR* ScopeLabel = nullptr;
	int32 ScopeLabelChars = 0;
};
} // namespace VaRest::Inspector::Private
#endif
