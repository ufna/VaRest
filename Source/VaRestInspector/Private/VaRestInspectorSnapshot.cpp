// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorSnapshot.h"

#include "CoreGlobals.h"
#include "Containers/Array.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Templates/AlignmentTemplates.h"
#include "VaRestInspectorPhases.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
struct FAllocationRecord
{
	void* Address = nullptr;
	ESnapshotAllocationKind Kind = ESnapshotAllocationKind::Raw;
	int32 Constructed = 0;
};

struct FDirectoryPage
{
	FDirectoryPage* Prev = nullptr;
	int32 Used = 0, Capacity = 0;
	FAllocationRecord* Records()
	{
		return reinterpret_cast<FAllocationRecord*>(reinterpret_cast<uint8*>(this) + Align(sizeof(FDirectoryPage), alignof(FAllocationRecord)));
	}
	const FAllocationRecord* Records() const
	{
		return reinterpret_cast<const FAllocationRecord*>(reinterpret_cast<const uint8*>(this) + Align(sizeof(FDirectoryPage), alignof(FAllocationRecord)));
	}
};

struct FRowEntrySegment
{
	FRowEntrySegment* Prev = nullptr;
	FRowEntrySegment* Next = nullptr;
	int32 Used = 0, Count = 0;
	FSnapshotRowEntry* Entries()
	{
		return reinterpret_cast<FSnapshotRowEntry*>(reinterpret_cast<uint8*>(this) + Align(sizeof(FRowEntrySegment), alignof(FSnapshotRowEntry)));
	}
	const FSnapshotRowEntry* Entries() const
	{
		return reinterpret_cast<const FSnapshotRowEntry*>(reinterpret_cast<const uint8*>(this) + Align(sizeof(FRowEntrySegment), alignof(FSnapshotRowEntry)));
	}
};

struct FStatsSegment
{
	FStatsSegment* Next = nullptr;
	int32 Count = 0;
	FSnapshotStatsGroup* Values()
	{
		return reinterpret_cast<FSnapshotStatsGroup*>(reinterpret_cast<uint8*>(this) + Align(sizeof(FStatsSegment), alignof(FSnapshotStatsGroup)));
	}
	const FSnapshotStatsGroup* Values() const
	{
		return reinterpret_cast<const FSnapshotStatsGroup*>(reinterpret_cast<const uint8*>(this) + Align(sizeof(FStatsSegment), alignof(FSnapshotStatsGroup)));
	}
};

namespace
{
constexpr int32 DirectoryRecordsPerPage = 64;

SIZE_T DirectoryRequest(int32 Count)
{
	return Align(sizeof(FDirectoryPage), alignof(FAllocationRecord)) + SIZE_T(Count) * sizeof(FAllocationRecord);
}
SIZE_T EntrySegmentRequest(int32 Count)
{
	return Align(sizeof(FRowEntrySegment), alignof(FSnapshotRowEntry)) + SIZE_T(Count) * sizeof(FSnapshotRowEntry);
}
constexpr uint32 EntrySegmentAlignment = alignof(FRowEntrySegment) > alignof(FSnapshotRowEntry) ? alignof(FRowEntrySegment) : alignof(FSnapshotRowEntry);
constexpr uint32 DirectoryAlignment = alignof(FDirectoryPage) > alignof(FAllocationRecord) ? alignof(FDirectoryPage) : alignof(FAllocationRecord);
static_assert(EntrySegmentAlignment <= 16 && DirectoryAlignment <= 16, "Snapshot directories require the audited allocation alignment");
static_assert(alignof(FSnapshotRowCopy) <= 16 && alignof(FSnapshotDispatch) <= 16 && alignof(FSnapshotPacket) <= 16 &&
	alignof(FSnapshotNote) <= 16 && alignof(FSnapshotTimeline) <= 16, "Snapshot clone alignment must remain supported");

constexpr uint32 StatsSegmentAlignment = alignof(FStatsSegment) > alignof(FSnapshotStatsGroup) ? alignof(FStatsSegment) : alignof(FSnapshotStatsGroup);
static_assert(StatsSegmentAlignment <= 16, "Private stats chunks require audited alignment");
int32 StatsPerSegment()
{
	const SIZE_T Header = Align(sizeof(FStatsSegment), alignof(FSnapshotStatsGroup));
	return Header < SIZE_T(FNormalizedLimits::MaxMetadataBytes) ?
		int32((SIZE_T(FNormalizedLimits::MaxMetadataBytes) - Header) / sizeof(FSnapshotStatsGroup)) : 0;
}
SIZE_T StatsSegmentRequest(int32 Count)
{
	return Align(sizeof(FStatsSegment), alignof(FSnapshotStatsGroup)) + SIZE_T(Count) * sizeof(FSnapshotStatsGroup);
}

bool SnapshotLabelRequest(FStringView Label, SIZE_T& OutRequest)
{
	if (Label.Len() < 0 || Label.Len() > FNormalizedLimits::MaxStringChars)
		return false;
	const uint64 Request = (uint64(Label.Len()) + 1) * sizeof(TCHAR);
	if (Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes))
		return false;
	OutRequest = SIZE_T(Request);
	return true;
}

int64 SnapshotTextBytes(const FVaRestInspectorPacketView& Source)
{
	int64 Total = 0;
	auto AddSlice = [&Total](FTextSlice Slice) {
		if (Slice.Chars)
			Total += (int64(Slice.Chars) + 1) * sizeof(TCHAR);
	};
	for (int32 Index = 0; Index < static_cast<int32>(EText::Count); ++Index)
		AddSlice(Source.Fixed->Text[Index]);
	for (const FArraySlice Array : {Source.Fixed->Headers, Source.Fixed->Warnings})
		for (const FPairSlice& Pair : Source.Pairs(Array))
		{
			AddSlice(Pair.First);
			AddSlice(Pair.Second);
		}
	return Total;
}

FVaRestInspectorPacketView PacketView(const FSnapshotPacket& Packet)
{
	FVaRestInspectorPacketView Result;
	if (!Packet.bPresent)
		return Result;
	Result.Fixed = &Packet.Fixed;
	Result.Backing = {Packet.Backing, Packet.BackingBytes};
	if (Packet.Excerpt)
		Result.Excerpt = {Packet.Excerpt->GetData(), Packet.Excerpt->Num()};
	return Result;
}

template <typename T>
void DestroyElements(void* Address, int32 Count)
{
	T* Values = static_cast<T*>(Address);
	for (int32 Index = 0; Index < Count; ++Index)
		Values[Index].~T();
}

void DestroyRecord(const FAllocationRecord& Record)
{
	switch (Record.Kind)
	{
	case ESnapshotAllocationKind::RowEntries:
	{
		auto* Segment = static_cast<FRowEntrySegment*>(Record.Address);
		if (Record.Constructed > 0)
		{
			DestroyElements<FSnapshotRowEntry>(Segment->Entries(), Record.Constructed);
			Segment->~FRowEntrySegment();
		}
		break;
	}
	case ESnapshotAllocationKind::MutableRow: DestroyElements<FSnapshotRowCopy>(Record.Address, Record.Constructed); break;
	case ESnapshotAllocationKind::Dispatches: DestroyElements<FSnapshotDispatch>(Record.Address, Record.Constructed); break;
	case ESnapshotAllocationKind::Packets: DestroyElements<FSnapshotPacket>(Record.Address, Record.Constructed); break;
	case ESnapshotAllocationKind::Notes: DestroyElements<FSnapshotNote>(Record.Address, Record.Constructed); break;
	case ESnapshotAllocationKind::Timeline: DestroyElements<FSnapshotTimeline>(Record.Address, Record.Constructed); break;
	case ESnapshotAllocationKind::StatsGroups:
	{
		auto* Segment = static_cast<FStatsSegment*>(Record.Address);
		if (Record.Constructed > 0)
		{
			DestroyElements<FSnapshotStatsGroup>(Segment->Values(), Record.Constructed);
			Segment->~FStatsSegment();
		}
		break;
	}
	case ESnapshotAllocationKind::Links: DestroyElements<FVaRestInspectorLink>(Record.Address, Record.Constructed); break;
	case ESnapshotAllocationKind::Raw: break;
	}
}

void VisitSnapshotCopy(const FSnapshotRowCopy& Copy, FStringView Label,
	TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor)
{
	FVaRestInspectorRecordView View = Copy.Value;
	View.Started = {};
	View.Completed = {};
	View.Marker = {};
	View.Dispatches = {};
	View.DispatchEvents = {};
	View.Notes = {};
	View.Excerpts = {};
	View.Timeline = {};
	if (Copy.bHasStarted)
		View.Started = PacketView(Copy.Started);
	if (Copy.bHasCompletedOwner)
		View.Completed = PacketView(Copy.Completed);
	else if (View.bActualCompletionObserved)
		View.Completed.Fixed = &Copy.CompletionFixed;
	if (Copy.bHasMarker)
		View.Marker = PacketView(Copy.Marker);

	FPacketFixed DispatchFixed[16];
	FVaRestInspectorDispatchView DispatchViews[16];
	FVaRestInspectorPacketView DispatchEvents[32], Excerpts[17];
	FVaRestInspectorPacketView Notes[FNormalizedLimits::NoteSlots];
	FVaRestInspectorTimelineEntry Timeline[FNormalizedLimits::TimelineSlots];
	int32 NumEvents = 0, NumExcerpts = 0;
	for (int32 NoteIndex = 0; NoteIndex < Copy.NumNotes; ++NoteIndex)
		Notes[NoteIndex] = PacketView(Copy.Notes[(Copy.NoteHead + NoteIndex) % Copy.NoteSlots].Packet);
	for (int32 TimelineIndex = 0; TimelineIndex < Copy.NumTimeline; ++TimelineIndex)
		Timeline[TimelineIndex] = Copy.Timeline[(Copy.TimelineHead + TimelineIndex) % Copy.TimelineSlots].Value;
	View.Notes = {Notes, Copy.NumNotes};
	View.Timeline = {Timeline, Copy.NumTimeline};
	if (Copy.bHasRequestExcerpt)
		Excerpts[NumExcerpts++] = PacketView(Copy.RequestExcerpt);
	for (int32 DispatchIndex = 0; DispatchIndex < Copy.NumDispatches; ++DispatchIndex)
	{
		const FSnapshotDispatch& Slot = Copy.Dispatches[DispatchIndex];
		auto& Dispatch = DispatchViews[DispatchIndex];
		Dispatch.DispatchId = Slot.DispatchId;
		Dispatch.Attempt = Slot.Attempt;
		Dispatch.bHasDispatched = Slot.bHasDispatched;
		Dispatch.bHasEnded = Slot.bHasEnded;
		Dispatch.HeadersState = Slot.HeadersState;
		Dispatch.ResponseExcerptState = Slot.ResponseExcerptState;
		if (Slot.bHasDispatched)
		{
			auto& Fixed = DispatchFixed[DispatchIndex];
			Fixed = Slot.DispatchedFixed;
			Dispatch.Dispatched.Fixed = &Fixed;
			if (Slot.HeaderIndex >= 0 && Slot.HeaderIndex < Copy.NumHeaders)
			{
				const auto Headers = PacketView(Copy.Headers[Slot.HeaderIndex]);
				Fixed.Headers = Headers.Fixed->Headers;
				Dispatch.Dispatched.Backing = Headers.Backing;
			}
			Dispatch.AttemptStart = Fixed.Scalars.AttemptStart;
			Dispatch.DispatchAt = Fixed.Header.Time;
			Dispatch.GapKind = Fixed.Scalars.GapKind;
			Dispatch.GapSeconds = Fixed.Scalars.GapSeconds;
			DispatchEvents[NumEvents++] = Dispatch.Dispatched;
		}
		if (Slot.bHasEnded)
		{
			Dispatch.Ended = PacketView(Slot.Ended);
			Dispatch.UploadDoneAt = Slot.EndedFixed.Scalars.UploadDoneAt;
			Dispatch.StatusAt = Slot.EndedFixed.Scalars.StatusAt;
			Dispatch.TransportDoneAt = Slot.EndedFixed.Scalars.TransportDoneAt;
			DispatchEvents[NumEvents++] = Dispatch.Ended;
		}
		if (Slot.ResponseExcerpt.bPresent)
		{
			Dispatch.ResponseExcerpt = PacketView(Slot.ResponseExcerpt);
			Excerpts[NumExcerpts++] = Dispatch.ResponseExcerpt;
		}
		DeriveDispatchPhases(Dispatch);
	}
	View.Dispatches = {DispatchViews, Copy.NumDispatches};
	View.DispatchEvents = {DispatchEvents, NumEvents};
	View.Excerpts = {Excerpts, NumExcerpts};
	View.ScopeLabel = View.bContextKnown && View.ScopeId == 0 ? FStringView(TEXT("Process")) : FStringView();
	View.ClientLabel = View.bContextKnown && View.Kind == EVaRestInspectorRecordKind::Http ? Label : FStringView();
	Visitor(View);
}
} // namespace
} // namespace VaRest::Inspector::Private

struct FVaRestInspectorSnapshot::FData
{
	explicit FData(VaRest::Inspector::Private::FStoreCredit&& InNested)
		: Nested(MoveTemp(InNested))
	{
	}
	VaRest::Inspector::Private::FStoreCredit Nested;
	VaRest::Inspector::Private::FDirectoryPage* Directory = nullptr;
	VaRest::Inspector::Private::FRowEntrySegment* FirstEntries = nullptr;
	VaRest::Inspector::Private::FRowEntrySegment* LastEntries = nullptr;
	VaRest::Inspector::Private::FStatsSegment* FirstStats = nullptr;
	VaRest::Inspector::Private::FStatsSegment* LastStats = nullptr;
	FVaRestInspectorLink* Links = nullptr;
	int32 NumStats = 0, NumLinks = 0;
	uint64 Revision = 0;
	FVaRestInspectorDiagnostics Diagnostics;
};

namespace VaRest::Inspector::Private
{
void FSnapshotBuilder::DestroyDomain(FVaRestInspectorSnapshot::FData& Data, FAllocationAdapter& Allocator)
{
	for (FDirectoryPage* Page = Data.Directory; Page; Page = Page->Prev)
		for (int32 Index = Page->Used - 1; Index >= 0; --Index)
			DestroyRecord(Page->Records()[Index]);
	for (FDirectoryPage* Page = Data.Directory; Page; Page = Page->Prev)
		for (int32 Index = Page->Used - 1; Index >= 0; --Index)
			Allocator.Free(Page->Records()[Index].Address);
	FDirectoryPage* Page = Data.Directory;
	while (Page)
	{
		FDirectoryPage* Prev = Page->Prev;
		Page->~FDirectoryPage();
		Allocator.Free(Page);
		Page = Prev;
	}
	Data.Directory = nullptr;
	Data.FirstEntries = nullptr;
	Data.LastEntries = nullptr;
	Data.FirstStats = Data.LastStats = nullptr;
	Data.Links = nullptr;
	Data.NumStats = Data.NumLinks = 0;
}

FSnapshotBuilder::FSnapshotBuilder() = default;
FSnapshotBuilder::~FSnapshotBuilder() = default;

bool FSnapshotBuilder::AddNestedBytes(int64 Amount)
{
	return AddChecked(NestedBytes, Amount, MAX_int64);
}
bool FSnapshotBuilder::AddBuiltBytes(SIZE_T Amount)
{
	return Amount <= SIZE_T(MAX_int64) && AddChecked(BuiltNestedBytes, int64(Amount), MAX_int64);
}
bool FSnapshotBuilder::MeasureAllocation(SIZE_T Request, uint32 Alignment)
{
	if (bMeasurementFinished || bReserved || Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes) ||
		Alignment == 0 || Alignment > 16 || (Alignment & (Alignment - 1)) != 0 || NumAllocationRecords == MAX_int32)
		return false;
	const auto Bound = Allocator->Bound(Request, Alignment);
	if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64) || !AddNestedBytes(int64(Bound.GetValue())))
		return false;
	++NumAllocationRecords;
	return true;
}

bool FSnapshotBuilder::Begin(const FStore& Store, int32 InRowCount)
{
	if (Account || bReserved || InRowCount < 0 || !Store.Budget || !Store.Budget->Lifetime)
		return false;
	Account = Store.Budget;
	Allocator = &Account->Lifetime->Allocator;
	if (!HasInspectorControllerCalibration<FVaRestInspectorSnapshot>(*Account->Lifetime))
		return false;
	RowCount = InRowCount;
	Revision = Store.RevisionValue;
	Diagnostics = Store.Diagnostics();
	PayloadBound = Allocator->Bound(sizeof(FVaRestInspectorSnapshot), alignof(FVaRestInspectorSnapshot));
	DataBound = Allocator->Bound(sizeof(FVaRestInspectorSnapshot::FData), alignof(FVaRestInspectorSnapshot::FData));
	if (!PayloadBound.IsSet() || !DataBound.IsSet() || PayloadBound.GetValue() > SIZE_T(MAX_int64) || DataBound.GetValue() > SIZE_T(MAX_int64))
		return false;
	NestedBytes = int64(DataBound.GetValue());
	if (!RowCount)
		return true;
	const SIZE_T Header = Align(sizeof(FRowEntrySegment), alignof(FSnapshotRowEntry));
	if (Header >= SIZE_T(FNormalizedLimits::MaxMetadataBytes))
		return false;
	const SIZE_T Available = SIZE_T(FNormalizedLimits::MaxMetadataBytes) - Header;
	if (sizeof(FSnapshotRowEntry) > Available)
		return false;
	EntriesPerSegment = int32(Available / sizeof(FSnapshotRowEntry));
	NumEntrySegments = int32((int64(RowCount) + EntriesPerSegment - 1) / EntriesPerSegment);
	for (int32 Segment = 0; Segment < NumEntrySegments; ++Segment)
	{
		const int32 Count = FMath::Min(EntriesPerSegment, RowCount - Segment * EntriesPerSegment);
		if (!MeasureAllocation(EntrySegmentRequest(Count), EntrySegmentAlignment))
			return false;
	}
	return true;
}

bool FSnapshotBuilder::MeasureSealedRow(FStringView Label)
{
	SIZE_T Request = 0;
	if (bMeasurementFinished || bReserved || !SnapshotLabelRequest(Label, Request))
		return false;
	return Label.IsEmpty() || MeasureAllocation(Request, alignof(TCHAR));
}
bool FSnapshotBuilder::MeasureMutableRow(FStringView Label)
{
	SIZE_T Request = 0;
	if (bMeasurementFinished || bReserved || !SnapshotLabelRequest(Label, Request) ||
		!MeasureAllocation(sizeof(FSnapshotRowCopy), alignof(FSnapshotRowCopy)))
		return false;
	if (!Label.IsEmpty() && !MeasureAllocation(Request, alignof(TCHAR)))
		return false;
	++NumMutableRows;
	return true;
}
bool FSnapshotBuilder::MeasureDispatches(int32 Count)
{
	if (Count < 0)
		return false;
	const uint64 Request = uint64(Count) * sizeof(FSnapshotDispatch);
	return Count == 0 || (Request <= SIZE_T(FNormalizedLimits::MaxMetadataBytes) && MeasureAllocation(SIZE_T(Request), alignof(FSnapshotDispatch)));
}
bool FSnapshotBuilder::MeasureHeaders(int32 Count)
{
	if (Count < 0)
		return false;
	const uint64 Request = uint64(Count) * sizeof(FSnapshotPacket);
	return Count == 0 || (Request <= SIZE_T(FNormalizedLimits::MaxMetadataBytes) && MeasureAllocation(SIZE_T(Request), alignof(FSnapshotPacket)));
}
bool FSnapshotBuilder::MeasureNotes(int32 Count)
{
	if (Count < 0)
		return false;
	const uint64 Request = uint64(Count) * sizeof(FSnapshotNote);
	return Count == 0 || (Request <= SIZE_T(FNormalizedLimits::MaxMetadataBytes) && MeasureAllocation(SIZE_T(Request), alignof(FSnapshotNote)));
}
bool FSnapshotBuilder::MeasureTimeline(int32 Count)
{
	if (Count < 0)
		return false;
	const uint64 Request = uint64(Count) * sizeof(FSnapshotTimeline);
	return Count == 0 || (Request <= SIZE_T(FNormalizedLimits::MaxMetadataBytes) && MeasureAllocation(SIZE_T(Request), alignof(FSnapshotTimeline)));
}
bool FSnapshotBuilder::MeasurePacket(const FVaRestInspectorPacketView& Source,
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt)
{
	if (bMeasurementFinished || bReserved || !Source.Fixed || Source.Backing.Num() < 0)
		return false;
	if (Source.Backing.Num() > 0 && !MeasureAllocation(SIZE_T(Source.Backing.Num()), alignof(FPacketFixed)))
		return false;
	if (Excerpt && Excerpt->IsValid())
	{
		const SIZE_T Capacity = (*Excerpt)->GetAllocatedSize();
		if (Capacity > SIZE_T(MAX_int64) || NumExcerptReferences == MAX_int32 || !AddNestedBytes(int64(Capacity)))
			return false;
		++NumExcerptReferences;
	}
	return true;
}
bool FSnapshotBuilder::FinishMeasurement()
{
	if (bMeasurementFinished || bReserved || NumAllocationRecords < 0)
		return false;
	const int64 Pages = (int64(NumAllocationRecords) + DirectoryRecordsPerPage - 1) / DirectoryRecordsPerPage;
	if (Pages > MAX_int32)
		return false;
	NumDirectoryPages = int32(Pages);
	for (int32 Page = 0; Page < NumDirectoryPages; ++Page)
	{
		const int32 Count = FMath::Min(DirectoryRecordsPerPage, NumAllocationRecords - Page * DirectoryRecordsPerPage);
		const SIZE_T Request = DirectoryRequest(Count);
		if (Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes))
			return false;
		const auto Bound = Allocator->Bound(Request, DirectoryAlignment);
		if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64) || !AddNestedBytes(int64(Bound.GetValue())))
			return false;
	}
	bMeasurementFinished = true;
	return true;
}
bool FSnapshotBuilder::Reserve()
{
	if (!bMeasurementFinished || bReserved)
		return false;
	auto Control = FStoreCredit::Acquire(Account, 4096, true);
	if (!Control.IsSet())
		return false;
	Controller = MoveTemp(Control.GetValue());
	auto Outer = FStoreCredit::Acquire(Account, int64(PayloadBound.GetValue()));
	if (!Outer.IsSet())
		return false;
	Payload = MoveTemp(Outer.GetValue());
	auto Nested = FStoreCredit::Acquire(Account, NestedBytes);
	if (!Nested.IsSet())
		return false;
	void* Memory = Allocator->AllocateReserved(sizeof(FVaRestInspectorSnapshot::FData),
		alignof(FVaRestInspectorSnapshot::FData), DataBound.GetValue());
	if (!Memory)
		return false;
	Data.Reset(new (Memory) FVaRestInspectorSnapshot::FData(MoveTemp(Nested.GetValue())));
	Data->Revision = Revision;
	Data->Diagnostics = Diagnostics;
	BuiltNestedBytes = int64(DataBound.GetValue());
	bReserved = true;
	return true;
}

bool FSnapshotBuilder::EnsureDirectoryPage()
{
	if (Data->Directory && Data->Directory->Used < Data->Directory->Capacity)
		return true;
	if (BuiltDirectoryPages >= NumDirectoryPages)
		return false;
	const int32 Count = FMath::Min(DirectoryRecordsPerPage, NumAllocationRecords - BuiltDirectoryPages * DirectoryRecordsPerPage);
	const SIZE_T Request = DirectoryRequest(Count);
	const auto Bound = Allocator->Bound(Request, DirectoryAlignment);
	if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
		return false;
	void* Memory = Allocator->AllocateReserved(Request, DirectoryAlignment, Bound.GetValue());
	if (!Memory)
		return false;
	auto* Page = new (Memory) FDirectoryPage();
	Page->Capacity = Count;
	Page->Prev = Data->Directory;
	Data->Directory = Page;
	++BuiltDirectoryPages;
	return AddBuiltBytes(Bound.GetValue());
}
bool FSnapshotBuilder::AddRecord(void* Address, ESnapshotAllocationKind Kind)
{
	if (!EnsureDirectoryPage() || !Data->Directory || Data->Directory->Used >= Data->Directory->Capacity)
		return false;
	FDirectoryPage& Page = *Data->Directory;
	Page.Records()[Page.Used++] = FAllocationRecord{Address, Kind, 0};
	++BuiltAllocationRecords;
	return true;
}
void* FSnapshotBuilder::AllocateChild(SIZE_T Request, uint32 Alignment, ESnapshotAllocationKind Kind)
{
	if (!bReserved || Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes) ||
		Alignment == 0 || Alignment > 16 || (Alignment & (Alignment - 1)) != 0)
		return nullptr;
	const auto Bound = Allocator->Bound(Request, Alignment);
	if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
		return nullptr;
	void* Memory = Allocator->AllocateReserved(Request, Alignment, Bound.GetValue());
	if (!Memory)
		return nullptr;
	if (!AddRecord(Memory, Kind))
	{
		Allocator->Free(Memory);
		return nullptr;
	}
	if (!AddBuiltBytes(Bound.GetValue()))
		return nullptr;
	return Memory;
}
void FSnapshotBuilder::SetConstructed(void* Address, int32 Count)
{
	check(Data->Directory && Data->Directory->Used > 0);
	FAllocationRecord& Record = Data->Directory->Records()[Data->Directory->Used - 1];
	check(Record.Address == Address && Record.Constructed == 0 && Count >= 0);
	Record.Constructed = Count;
}

bool FSnapshotBuilder::EnsureEntrySegment()
{
	if (Data->LastEntries && Data->LastEntries->Used < Data->LastEntries->Count)
		return true;
	if (BuiltEntrySegments >= NumEntrySegments)
		return false;
	const int32 Count = FMath::Min(EntriesPerSegment, RowCount - BuiltEntrySegments * EntriesPerSegment);
	void* Memory = AllocateChild(EntrySegmentRequest(Count), EntrySegmentAlignment, ESnapshotAllocationKind::RowEntries);
	if (!Memory)
		return false;
	auto* Segment = new (Memory) FRowEntrySegment();
	Segment->Count = Count;
	Segment->Prev = Data->LastEntries;
	if (Data->LastEntries)
		Data->LastEntries->Next = Segment;
	else
		Data->FirstEntries = Segment;
	Data->LastEntries = Segment;
	for (int32 Index = 0; Index < Count; ++Index)
		new (Segment->Entries() + Index) FSnapshotRowEntry();
	SetConstructed(Segment, Count);
	++BuiltEntrySegments;
	return true;
}
FSnapshotRowEntry* FSnapshotBuilder::NextEntry()
{
	if (!EnsureEntrySegment() || !Data->LastEntries || Data->LastEntries->Used >= Data->LastEntries->Count)
		return nullptr;
	++BuiltEntries;
	return Data->LastEntries->Entries() + Data->LastEntries->Used++;
}
bool FSnapshotBuilder::CopyLabel(FStringView Label, FSnapshotRowEntry& Entry)
{
	if (Label.IsEmpty())
		return true;
	SIZE_T Request = 0;
	if (!SnapshotLabelRequest(Label, Request))
		return false;
	void* Memory = AllocateChild(Request, alignof(TCHAR), ESnapshotAllocationKind::Raw);
	if (!Memory)
		return false;
	FMemory::Memcpy(Memory, Label.GetData(), SIZE_T(Label.Len()) * sizeof(TCHAR));
	static_cast<TCHAR*>(Memory)[Label.Len()] = TCHAR(0);
	Allocator->Counters().TextBytesCopied.fetch_add(int64(Request));
	Entry.ClientLabel = static_cast<const TCHAR*>(Memory);
	Entry.ClientLabelChars = Label.Len();
	return true;
}

FSnapshotRowCopy* FSnapshotBuilder::ConstructMutableRow()
{
	void* Memory = AllocateChild(sizeof(FSnapshotRowCopy), alignof(FSnapshotRowCopy), ESnapshotAllocationKind::MutableRow);
	if (!Memory)
		return nullptr;
	auto* Copy = new (Memory) FSnapshotRowCopy();
	SetConstructed(Memory, 1);
	++BuiltMutableRows;
	return Copy;
}
bool FSnapshotBuilder::ConstructDispatches(FSnapshotRowCopy& Copy, int32 Count)
{
	if (Copy.Dispatches || Count < 0)
		return false;
	if (!Count)
		return true;
	void* Memory = AllocateChild(SIZE_T(Count) * sizeof(FSnapshotDispatch), alignof(FSnapshotDispatch), ESnapshotAllocationKind::Dispatches);
	if (!Memory)
		return false;
	auto* Values = static_cast<FSnapshotDispatch*>(Memory);
	for (int32 Index = 0; Index < Count; ++Index)
		new (Values + Index) FSnapshotDispatch();
	SetConstructed(Memory, Count);
	Copy.Dispatches = Values;
	Copy.NumDispatches = Count;
	return true;
}
bool FSnapshotBuilder::ConstructHeaders(FSnapshotRowCopy& Copy, int32 Count)
{
	if (Copy.Headers || Count < 0)
		return false;
	if (!Count)
		return true;
	void* Memory = AllocateChild(SIZE_T(Count) * sizeof(FSnapshotPacket), alignof(FSnapshotPacket), ESnapshotAllocationKind::Packets);
	if (!Memory)
		return false;
	auto* Values = static_cast<FSnapshotPacket*>(Memory);
	for (int32 Index = 0; Index < Count; ++Index)
		new (Values + Index) FSnapshotPacket();
	SetConstructed(Memory, Count);
	Copy.Headers = Values;
	Copy.NumHeaders = Count;
	return true;
}
bool FSnapshotBuilder::ConstructNotes(FSnapshotRowCopy& Copy, int32 Count)
{
	if (Copy.Notes || Count < 0)
		return false;
	if (!Count)
		return true;
	void* Memory = AllocateChild(SIZE_T(Count) * sizeof(FSnapshotNote), alignof(FSnapshotNote), ESnapshotAllocationKind::Notes);
	if (!Memory)
		return false;
	auto* Values = static_cast<FSnapshotNote*>(Memory);
	for (int32 Index = 0; Index < Count; ++Index)
		new (Values + Index) FSnapshotNote();
	SetConstructed(Memory, Count);
	Copy.Notes = Values;
	Copy.NoteSlots = Count;
	return true;
}
bool FSnapshotBuilder::ConstructTimeline(FSnapshotRowCopy& Copy, int32 Count)
{
	if (Copy.Timeline || Count < 0)
		return false;
	if (!Count)
		return true;
	void* Memory = AllocateChild(SIZE_T(Count) * sizeof(FSnapshotTimeline), alignof(FSnapshotTimeline), ESnapshotAllocationKind::Timeline);
	if (!Memory)
		return false;
	auto* Values = static_cast<FSnapshotTimeline*>(Memory);
	for (int32 Index = 0; Index < Count; ++Index)
		new (Values + Index) FSnapshotTimeline();
	SetConstructed(Memory, Count);
	Copy.Timeline = Values;
	Copy.TimelineSlots = Count;
	return true;
}
bool FSnapshotBuilder::ConstructPacket(const FVaRestInspectorPacketView& Source,
	const TSharedPtr<const TArray<uint8>, ESPMode::ThreadSafe>* Excerpt, FSnapshotPacket& Out)
{
	if (!bReserved || !Source.Fixed || Source.Backing.Num() < 0)
		return false;
	Out.Fixed = *Source.Fixed;
	Out.bPresent = true;
	if (Source.Backing.Num() > 0)
	{
		void* Memory = AllocateChild(SIZE_T(Source.Backing.Num()), alignof(FPacketFixed), ESnapshotAllocationKind::Raw);
		if (!Memory)
			return false;
		FMemory::Memcpy(Memory, Source.Backing.GetData(), SIZE_T(Source.Backing.Num()));
		Out.Backing = static_cast<const uint8*>(Memory);
		Out.BackingBytes = Source.Backing.Num();
		Allocator->Counters().TextBytesCopied.fetch_add(SnapshotTextBytes(Source));
	}
	if (Excerpt && Excerpt->IsValid())
	{
		const SIZE_T Capacity = (*Excerpt)->GetAllocatedSize();
		if (Capacity > SIZE_T(MAX_int64))
			return false;
		Out.Excerpt = *Excerpt;
		Allocator->Counters().ExcerptRefsAcquired.fetch_add(1);
		++BuiltExcerptReferences;
		if (!AddBuiltBytes(Capacity))
			return false;
	}
	return true;
}
void FSnapshotBuilder::ConstructScalarPacket(const FPacketFixed& Source, FSnapshotPacket& Out)
{
	Out.Fixed = Source;
	Out.bPresent = true;
}
bool FSnapshotBuilder::ConstructSealedEntry(const TSharedPtr<const FRow, ESPMode::ThreadSafe>& Owner, FStringView Label)
{
	if (!Owner)
		return false;
	FSnapshotRowEntry* Entry = NextEntry();
	if (!Entry || !CopyLabel(Label, *Entry))
		return false;
	Entry->Sealed = Owner;
	return true;
}
bool FSnapshotBuilder::ConstructMutableEntry(FSnapshotRowCopy* Copy, FStringView Label)
{
	if (!Copy)
		return false;
	FSnapshotRowEntry* Entry = NextEntry();
	if (!Entry || !CopyLabel(Label, *Entry))
		return false;
	Entry->Mutable = Copy;
	return true;
}
bool FSnapshotBuilder::MeasurePublicString(FStringView Source)
{
	if (Source.IsEmpty())
		return true;
	if (Account->PublicStringsUnsupported.load(std::memory_order_acquire))
	{
		bUnsupportedPublicStrings = true;
		return false;
	}
#if UE_ENABLE_ARRAY_SLACK_TRACKING
	bUnsupportedPublicStrings = true;
	return false;
#else
	if (bMeasurementFinished || bReserved || Source.Len() < 0 || Source.Len() > FNormalizedLimits::MaxStringChars || !Source.GetData())
		return false;
	using FStringArray = TArray<FString::ElementType, FString::AllocatorType>;
	using FInstance = FStringArray::ElementAllocatorType;
	FInstance MeasureInstance;
	const int32 N = Source.Len() + 1;
	int32 ReserveElements = 0;
	if constexpr (TAllocatorTraits<FString::AllocatorType>::SupportsElementAlignment)
		ReserveElements = MeasureInstance.CalculateSlackReserve(N, sizeof(TCHAR), alignof(TCHAR));
	else
		ReserveElements = MeasureInstance.CalculateSlackReserve(N, sizeof(TCHAR));
	if (ReserveElements < N || ReserveElements > MAX_int32)
		return false;
	const uint64 Request = uint64(ReserveElements) * sizeof(TCHAR);
	if (Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes))
		return false;
	const auto Bound = Allocator->Bound(SIZE_T(Request), alignof(TCHAR));
	if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
	{
		bUnsupportedPublicStrings = true;
		return false;
	}
	++NumPublicText;
	return AddNestedBytes(int64(Bound.GetValue()));
#endif
}
bool FSnapshotBuilder::ConstructPublicString(FStringView Source, FString& Out)
{
	if (Source.IsEmpty())
	{
		Out.Reset();
		return true;
	}
	if (Account->PublicStringsUnsupported.load(std::memory_order_acquire))
	{
		bUnsupportedPublicStrings = true;
		return false;
	}
#if UE_ENABLE_ARRAY_SLACK_TRACKING
	bUnsupportedPublicStrings = true;
	return false;
#else
	using FStringArray = TArray<FString::ElementType, FString::AllocatorType>;
	using FInstance = FStringArray::ElementAllocatorType;
	FInstance MeasureInstance;
	const int32 N = Source.Len() + 1;
	int32 ReserveElements = 0;
	if constexpr (TAllocatorTraits<FString::AllocatorType>::SupportsElementAlignment)
		ReserveElements = MeasureInstance.CalculateSlackReserve(N, sizeof(TCHAR), alignof(TCHAR));
	else
		ReserveElements = MeasureInstance.CalculateSlackReserve(N, sizeof(TCHAR));
	if (ReserveElements < N || ReserveElements > MAX_int32)
		return false;
	const uint64 Request = uint64(ReserveElements) * sizeof(TCHAR);
	const auto Bound = Allocator->Bound(SIZE_T(Request), alignof(TCHAR));
	if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
	{
		bUnsupportedPublicStrings = true;
		return false;
	}
	FString Value;
	Value.Reserve(Source.Len());
	if (Value.GetAllocatedSize() != Request)
	{
		bUnsupportedPublicStrings = true;
		return false;
	}
	const SIZE_T Actual = FMemory::GetAllocSize(Value.GetCharArray().GetData());
	if (Actual < Request || Actual > Bound.GetValue())
	{
		bUnsupportedPublicStrings = true;
		return false;
	}
	Value.GetCharArray().AddUninitialized(N);
	FMemory::Memcpy(Value.GetCharArray().GetData(), Source.GetData(), SIZE_T(Source.Len()) * sizeof(TCHAR));
	Value.GetCharArray()[N - 1] = TCHAR(0);
	Allocator->Counters().TextBytesCopied.fetch_add(int64(N) * sizeof(TCHAR));
	Out = MoveTemp(Value);
	++BuiltPublicText;
	return AddBuiltBytes(Bound.GetValue());
#endif
}
bool FSnapshotBuilder::MeasureStats(int32 Count)
{
	if (bMeasurementFinished || bReserved || Count < 0 || Count > FNormalizedLimits::StatsGroups)
		return false;
	const int32 PerSegment = StatsPerSegment();
	if (Count && PerSegment <= 0)
		return false;
	for (int32 Offset = 0; Offset < Count;)
	{
		const int32 Elements = FMath::Min(PerSegment, Count - Offset);
		if (!MeasureAllocation(StatsSegmentRequest(Elements), StatsSegmentAlignment))
			return false;
		Offset += Elements;
	}
	return true;
}
bool FSnapshotBuilder::MeasureStatsGroup(const FStatsGroupSlot& Group)
{
	if (bMeasurementFinished || bReserved)
		return false;
	const FStringView Key = Group.Key.View();
	return MeasurePublicString(Key);
}
bool FSnapshotBuilder::ConstructStats(int32 Count)
{
	if (!bReserved || Count < 0 || Count > FNormalizedLimits::StatsGroups || Data->FirstStats)
		return false;
	const int32 PerSegment = StatsPerSegment();
	if (Count && PerSegment <= 0)
		return false;
	for (int32 Offset = 0; Offset < Count;)
	{
		const int32 Elements = FMath::Min(PerSegment, Count - Offset);
		void* Memory = AllocateChild(StatsSegmentRequest(Elements), StatsSegmentAlignment, ESnapshotAllocationKind::StatsGroups);
		if (!Memory)
			return false;
		auto* Segment = new (Memory) FStatsSegment();
		Segment->Count = Elements;
		for (int32 Index = 0; Index < Elements; ++Index)
			new (Segment->Values() + Index) FSnapshotStatsGroup();
		SetConstructed(Memory, Elements);
		if (Data->LastStats)
			Data->LastStats->Next = Segment;
		else
			Data->FirstStats = Segment;
		Data->LastStats = Segment;
		Offset += Elements;
		Data->NumStats = Offset;
	}
	return true;
}
bool FSnapshotBuilder::ConstructStatsGroup(const FStatsGroupSlot& Group, int32 Destination)
{
	if (Destination < 0 || Destination >= Data->NumStats)
		return false;
	FStatsSegment* Segment = Data->FirstStats;
	while (Segment && Destination >= Segment->Count)
	{
		Destination -= Segment->Count;
		Segment = Segment->Next;
	}
	if (!Segment)
		return false;
	FSnapshotStatsGroup& Out = Segment->Values()[Destination];
	CopyStatsScalars(Group.Value, Out.Value);
	return ConstructPublicString(Group.Key.View(), Out.Value.Key);
}
bool FSnapshotBuilder::MeasureLinks(int32 Count)
{
	if (bMeasurementFinished || bReserved || Count < 0)
		return false;
	return Count == 0 || MeasureAllocation(SIZE_T(Count) * sizeof(FVaRestInspectorLink), alignof(FVaRestInspectorLink));
}
bool FSnapshotBuilder::MeasureLink(const FStoreLinkView& Link)
{
	return !bMeasurementFinished && !bReserved && MeasurePublicString(Link.QueueId);
}
bool FSnapshotBuilder::ConstructLinks(int32 Count)
{
	if (!bReserved || Count < 0)
		return false;
	if (!Count)
	{
		Data->Links = nullptr;
		Data->NumLinks = 0;
		return true;
	}
	void* Memory = AllocateChild(SIZE_T(Count) * sizeof(FVaRestInspectorLink), alignof(FVaRestInspectorLink), ESnapshotAllocationKind::Links);
	if (!Memory)
		return false;
	auto* Values = static_cast<FVaRestInspectorLink*>(Memory);
	for (int32 Index = 0; Index < Count; ++Index)
		new (Values + Index) FVaRestInspectorLink();
	SetConstructed(Memory, Count);
	Data->Links = Values;
	Data->NumLinks = Count;
	return true;
}
bool FSnapshotBuilder::ConstructLink(const FStoreLinkView& Link, int32 Destination)
{
	if (!Data->Links || Destination < 0 || Destination >= Data->NumLinks)
		return false;
	FVaRestInspectorLink& Out = Data->Links[Destination];
	Out.FromRequestId = Link.FromRequestId;
	Out.ToRequestId = Link.ToRequestId;
	Out.bAuth = Link.bAuth;
	Out.bTargetEvicted = Link.bTargetEvicted;
	return ConstructPublicString(Link.QueueId, Out.QueueId);
}
bool FSnapshotBuilder::MeasuredCosts(int64& Outer, int64& Nested) const
{
	if (!bMeasurementFinished || !PayloadBound.IsSet())
		return false;
	Outer = int64(PayloadBound.GetValue());
	Nested = NestedBytes;
	return true;
}
int32 FSnapshotBuilder::PrivateStatsCount(const FVaRestInspectorSnapshot& Snapshot)
{
	return Snapshot.Data ? Snapshot.Data->NumStats : 0;
}
bool FSnapshotBuilder::HasPrivateStatsGroup(const FVaRestInspectorSnapshot& Snapshot, const FStatsKeyView& Key)
{
	if (!Snapshot.Data)
		return false;
	for (const FStatsSegment* Segment = Snapshot.Data->FirstStats; Segment; Segment = Segment->Next)
		for (int32 Index = 0; Index < Segment->Count; ++Index)
		{
			const auto& Group = Segment->Values()[Index];
			const FStringView Text(Group.Value.Key);
			if (Group.Value.ScopeId == Key.ScopeId && Group.Value.ClientId == Key.ClientId &&
				Group.Value.Endpoint == Key.Endpoint && Text == Key.Key)
				return true;
		}
	return false;
}
bool FSnapshotBuilder::PrivateStatsHistogramsAreZero(const FVaRestInspectorSnapshot& Snapshot)
{
	if (!Snapshot.Data)
		return false;
	for (const FStatsSegment* Segment = Snapshot.Data->FirstStats; Segment; Segment = Segment->Next)
		for (int32 Index = 0; Index < Segment->Count; ++Index)
			for (uint64 Bin : Segment->Values()[Index].Value.Histogram)
				if (Bin != 0)
					return false;
	return true;
}
bool FSnapshotBuilder::Publish(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out)
{
	if (!bReserved || !Data || BuiltNestedBytes != NestedBytes || BuiltAllocationRecords != NumAllocationRecords ||
		BuiltDirectoryPages != NumDirectoryPages || BuiltEntrySegments != NumEntrySegments || BuiltEntries != RowCount ||
		BuiltMutableRows != NumMutableRows || BuiltExcerptReferences != NumExcerptReferences ||
		BuiltPublicText != NumPublicText || !HasInspectorControllerCalibration<FVaRestInspectorSnapshot>(*Account->Lifetime))
		return false;
	const auto CurrentPayload = Allocator->Bound(sizeof(FVaRestInspectorSnapshot), alignof(FVaRestInspectorSnapshot));
	if (!CurrentPayload.IsSet() || CurrentPayload.GetValue() != PayloadBound.GetValue())
		return false;
	void* Memory = Allocator->AllocateReserved(sizeof(FVaRestInspectorSnapshot), alignof(FVaRestInspectorSnapshot), PayloadBound.GetValue());
	if (!Memory)
		return false;
	auto* Object = new (Memory) FVaRestInspectorSnapshot();
	Object->Data = MoveTemp(Data);
	TInspectorDeleter<FVaRestInspectorSnapshot> Deleter(MoveTemp(Payload), MoveTemp(Controller));
	LLM_SCOPE_BYTAG(VaRest_Inspector);
	TSharedPtr<FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Mutable(Object, MoveTemp(Deleter));
	TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe> Result = MoveTemp(Mutable);
	Out = MoveTemp(Result);
	return true;
}
} // namespace VaRest::Inspector::Private
#else
struct FVaRestInspectorSnapshot::FData
{
};
#endif

void FVaRestInspectorSnapshot::FDataDeleter::operator()(FData* Object) const
{
#if VAREST_WITH_INSPECTOR
	if (!Object)
		return;
	VaRest::Inspector::Private::FStoreCredit Nested = MoveTemp(Object->Nested);
	VaRest::Inspector::Private::FAllocationAdapter& Allocator = Nested.GetAccount()->Lifetime->Allocator;
	VaRest::Inspector::Private::FSnapshotBuilder::DestroyDomain(*Object, Allocator);
	Object->~FData();
	Allocator.Free(Object);
	Nested.Reset();
#else
	delete Object;
#endif
}

FVaRestInspectorSnapshot::FVaRestInspectorSnapshot()
{
	check(IsInGameThread());
}
FVaRestInspectorSnapshot::~FVaRestInspectorSnapshot() = default;
uint64 FVaRestInspectorSnapshot::GetRevision() const
{
	check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
	return Data ? Data->Revision : 0;
#else
	return 0;
#endif
}
void FVaRestInspectorSnapshot::ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const
{
	check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
	if (!Data)
		return;
	for (auto* Segment = Data->FirstEntries; Segment; Segment = Segment->Next)
		for (int32 Index = 0; Index < Segment->Used; ++Index)
		{
			const auto& Entry = Segment->Entries()[Index];
			const FStringView Label = Entry.ClientLabel ? FStringView(Entry.ClientLabel, Entry.ClientLabelChars) : FStringView();
			if (Entry.Sealed)
				VaRest::Inspector::Private::FStore::VisitImmutableRow(*Entry.Sealed, Label, Visitor);
			else if (Entry.Mutable)
				VaRest::Inspector::Private::VisitSnapshotCopy(*Entry.Mutable, Label, Visitor);
		}
#else
	(void)Visitor;
#endif
}
void FVaRestInspectorSnapshot::ForEachStats(TFunctionRef<void(const FVaRestInspectorHttpStats&)> Visitor) const
{
	check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
	if (Data)
		for (const auto* Segment = Data->FirstStats; Segment; Segment = Segment->Next)
			for (int32 Index = 0; Index < Segment->Count; ++Index)
				Visitor(Segment->Values()[Index].Value);
#else
	(void)Visitor;
#endif
}
TConstArrayView<FVaRestInspectorLink> FVaRestInspectorSnapshot::GetLinks() const
{
	check(IsInGameThread());
#if VAREST_WITH_INSPECTOR
	if (Data && Data->NumLinks > 0)
		return {Data->Links, Data->NumLinks};
#endif
	return {};
}
const FVaRestInspectorDiagnostics& FVaRestInspectorSnapshot::GetDiagnostics() const
{
	check(IsInGameThread());
	static const FVaRestInspectorDiagnostics Empty;
#if VAREST_WITH_INSPECTOR
	return Data ? Data->Diagnostics : Empty;
#else
	return Empty;
#endif
}
