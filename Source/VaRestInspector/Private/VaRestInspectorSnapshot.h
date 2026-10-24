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
	bool MeasureSealedRow(FStringView Label);
	bool MeasureMutableRow(FStringView Label);
	bool MeasureDispatches(int32 Count);
	bool MeasureHeaders(int32 Count);
	bool MeasureNotes(int32 Count);
	bool MeasureTimeline(int32 Count);
	bool MeasurePacket(const FVaRestInspectorPacketView& Source,
		const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt);
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
	bool ConstructSealedEntry(const TSharedPtr<const FRow, ESPMode::ThreadSafe>& Owner, FStringView Label);
	bool ConstructMutableEntry(FSnapshotRowCopy* Copy, FStringView Label);
	bool Publish(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out);
	static void DestroyDomain(FVaRestInspectorSnapshot::FData& Data, FAllocationAdapter& Allocator);

private:
	bool AddNestedBytes(int64 Amount);
	bool AddBuiltBytes(SIZE_T Amount);
	bool MeasureAllocation(SIZE_T Request, uint32 Alignment);
	void* AllocateChild(SIZE_T Request, uint32 Alignment, ESnapshotAllocationKind Kind);
	void SetConstructed(void* Address, int32 Count);
	bool EnsureDirectoryPage();
	bool AddRecord(void* Address, ESnapshotAllocationKind Kind);
	bool EnsureEntrySegment();
	FSnapshotRowEntry* NextEntry();
	bool CopyLabel(FStringView Label, FSnapshotRowEntry& Entry);

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
	bool bMeasurementFinished = false, bReserved = false;
};

struct FSnapshotPacket
{
	FPacketFixed Fixed;
	const uint8* Backing = nullptr;
	int32 BackingBytes = 0;
	TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe> Excerpt;
	bool bPresent = false;
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
	FVaRestInspectorRecordView Value;
	FSnapshotBuilder::ELivenessEvidence Evidence = FSnapshotBuilder::ELivenessEvidence::None;
	uint64 PresenceStamp = 0;
	int32 IndexSlot = INDEX_NONE, DeadlineIndex = INDEX_NONE;
	FPacketFixed CompletionFixed;
	FSnapshotPacket Started, Completed, RequestExcerpt;
	bool bHasStarted = false, bHasCompletedOwner = false, bHasRequestExcerpt = false;
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
};
} // namespace VaRest::Inspector::Private
#endif
