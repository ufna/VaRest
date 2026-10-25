// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorStore.h"
#include "VaRestInspectorPhases.h"
#include "VaRestInspectorSnapshot.h"

#include "CoreGlobals.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeExit.h"
#include "Templates/TypeHash.h"
#include "Templates/AlignmentTemplates.h"
#include "VaRestClock.h"

#include <limits>

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
namespace
{
void Increment(uint64& Value)
{
	if (Value != MAX_uint64)
		++Value;
}
FPacketFixed ScalarCopy(const FPacketFixed& Input)
{
	FPacketFixed Result;
	Result.Header = Input.Header;
	Result.Scalars = Input.Scalars;
	return Result;
}
void AddObservation(uint64& Value, uint64 Amount, bool& bSaturated)
{
	if (Amount > MAX_uint64 - Value)
	{
		Value = MAX_uint64;
		bSaturated = true;
	}
	else
		Value += Amount;
}
void AddWire(uint64& Value, int64 Amount, FVaRestInspectorRecordView& Row)
{
	if (Amount < 0)
		Row.bWireBytesUnknown = true;
	else
		AddObservation(Value, uint64(Amount), Row.bCountersSaturated);
}
template <typename T>
struct TStoreChunk
{
	FStoreCredit Credit;
	TStoreChunk* Prev = nullptr;
	int32 Count = 0;
	T* Values() { return reinterpret_cast<T*>(reinterpret_cast<uint8*>(this) + Align(sizeof(TStoreChunk), alignof(T))); }
	const T* Values() const { return reinterpret_cast<const T*>(reinterpret_cast<const uint8*>(this) + Align(sizeof(TStoreChunk), alignof(T))); }
};
struct FStoreBlockMeasure
{
	int64 Bytes = 0;
	int32 NumChunks = 0, PerChunk = 0;
	bool bSupported = false;
};
template <typename T>
FStoreBlockMeasure MeasureStoreBlock(const FBudgetAccount& Account, int32 Count)
{
	FStoreBlockMeasure Result;
	if (!Account.Lifetime || Count < 0)
		return Result;
	if (Count == 0)
	{
		Result.bSupported = true;
		return Result;
	}
	constexpr uint32 ChunkAlignment = alignof(TStoreChunk<T>) > alignof(T) ? alignof(TStoreChunk<T>) : alignof(T);
	if (ChunkAlignment > 16)
		return Result;
	const SIZE_T Header = Align(sizeof(TStoreChunk<T>), alignof(T));
	if (Header >= SIZE_T(FNormalizedLimits::MaxMetadataBytes) || sizeof(T) > SIZE_T(FNormalizedLimits::MaxMetadataBytes) - Header)
		return Result;
	const int32 PerChunk = FMath::Min(int32((SIZE_T(FNormalizedLimits::MaxMetadataBytes) - Header) / sizeof(T)), Count);
	if (PerChunk <= 0)
		return Result;
	const int32 NumChunks = int32((int64(Count) + PerChunk - 1) / PerChunk);
	FAllocationAdapter& Adapter = Account.Lifetime->Allocator;
	const auto DirectoryBound = Adapter.Bound(SIZE_T(NumChunks) * sizeof(TStoreChunk<T>*), alignof(TStoreChunk<T>*));
	if (!DirectoryBound.IsSet() || DirectoryBound.GetValue() > SIZE_T(MAX_int64) || !AddChecked(Result.Bytes, int64(DirectoryBound.GetValue()), MAX_int64))
		return FStoreBlockMeasure{};
	for (int32 Chunk = 0; Chunk < NumChunks; ++Chunk)
	{
		const int32 Elements = FMath::Min(PerChunk, Count - Chunk * PerChunk);
		const SIZE_T Request = Header + SIZE_T(Elements) * sizeof(T);
		const auto Bound = Adapter.Bound(Request, ChunkAlignment);
		if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64) || !AddChecked(Result.Bytes, int64(Bound.GetValue()), MAX_int64))
			return FStoreBlockMeasure{};
	}
	Result.NumChunks = NumChunks;
	Result.PerChunk = PerChunk;
	Result.bSupported = true;
	return Result;
}
template <typename T>
class TStoreBlock
{
public:
	~TStoreBlock() { Reset(); }
	bool IsAllocated() const { return Chunks != nullptr; }
	bool Allocate(TRefCountPtr<FBudgetAccount> Account, int32 InCount)
	{
		if (Chunks || !Account || !Account->Lifetime || InCount < 0)
			return false;
		if (InCount == 0)
			return true;
		const FStoreBlockMeasure Measure = MeasureStoreBlock<T>(*Account, InCount);
		if (!Measure.bSupported)
			return false;
		FAllocationAdapter& Adapter = Account->Lifetime->Allocator;
		const SIZE_T DirectoryRequest = SIZE_T(Measure.NumChunks) * sizeof(TStoreChunk<T>*);
		const auto DirectoryBound = Adapter.Bound(DirectoryRequest, alignof(TStoreChunk<T>*));
		if (!DirectoryBound.IsSet())
			return false;
		auto DirectoryReservation = FStoreCredit::Acquire(Account, int64(DirectoryBound.GetValue()));
		if (!DirectoryReservation.IsSet())
			return false;
		void* DirectoryMemory = Adapter.AllocateReserved(DirectoryRequest, alignof(TStoreChunk<T>*), DirectoryBound.GetValue());
		if (!DirectoryMemory)
			return false;
		DirectoryCredit = MoveTemp(DirectoryReservation.GetValue());
		bool bComplete = false;
		ON_SCOPE_EXIT
		{
			if (!bComplete)
				Reset();
		};
		Chunks = static_cast<TStoreChunk<T>**>(DirectoryMemory);
		Count = InCount;
		NumChunks = Measure.NumChunks;
		PerChunk = Measure.PerChunk;
		for (int32 Index = 0; Index < NumChunks; ++Index)
			Chunks[Index] = nullptr;
		for (int32 Index = 0; Index < NumChunks; ++Index)
		{
			const int32 Elements = FMath::Min(PerChunk, Count - Index * PerChunk);
			const SIZE_T Request = Align(sizeof(TStoreChunk<T>), alignof(T)) + SIZE_T(Elements) * sizeof(T);
			const auto Bound = Adapter.Bound(Request, alignof(TStoreChunk<T>) > alignof(T) ? alignof(TStoreChunk<T>) : alignof(T));
			if (!Bound.IsSet())
				return false;
			auto Reservation = FStoreCredit::Acquire(Account, int64(Bound.GetValue()));
			if (!Reservation.IsSet())
				return false;
			void* Memory = Adapter.AllocateReserved(Request, alignof(TStoreChunk<T>) > alignof(T) ? alignof(TStoreChunk<T>) : alignof(T), Bound.GetValue());
			if (!Memory)
				return false;
			auto* Chunk = new (Memory) TStoreChunk<T>();
			Chunk->Credit = MoveTemp(Reservation.GetValue());
			Chunk->Count = Elements;
			for (int32 Element = 0; Element < Elements; ++Element)
				new (Chunk->Values() + Element) T();
			Chunk->Prev = Last;
			Last = Chunk;
			Chunks[Index] = Chunk;
		}
		bComplete = true;
		return true;
	}
	void Reset()
	{
		if (!DirectoryCredit.GetAccount())
		{
			Chunks = nullptr;
			Last = nullptr;
			Count = NumChunks = PerChunk = 0;
			return;
		}
		FAllocationAdapter& Adapter = DirectoryCredit.GetAccount()->Lifetime->Allocator;
		while (Last)
		{
			TStoreChunk<T>* Next = Last->Prev;
			FStoreCredit Credit = MoveTemp(Last->Credit);
			for (int32 Index = 0; Index < Last->Count; ++Index)
				Last->Values()[Index].~T();
			Last->~TStoreChunk();
			Adapter.Free(Last);
			Last = Next;
			Credit.Reset();
		}
		if (Chunks)
			Adapter.Free(Chunks);
		Chunks = nullptr;
		Last = nullptr;
		Count = NumChunks = PerChunk = 0;
		DirectoryCredit.Reset();
	}
	T& operator[](int32 Index)
	{
		check(Index >= 0 && Index < Count);
		return Chunks[Index / PerChunk]->Values()[Index % PerChunk];
	}
	const T& operator[](int32 Index) const
	{
		check(Index >= 0 && Index < Count);
		return Chunks[Index / PerChunk]->Values()[Index % PerChunk];
	}

private:
	FStoreCredit DirectoryCredit;
	TStoreChunk<T>** Chunks = nullptr;
	TStoreChunk<T>* Last = nullptr;
	int32 Count = 0, NumChunks = 0, PerChunk = 0;
};
struct FClientEntry
{
	~FClientEntry() { Reset(); }
	void Reset()
	{
		if (Label)
			LabelCredit.GetAccount()->Lifetime->Allocator.Free(Label);
		Label = nullptr;
		Chars = 0;
		LabelCredit.Reset();
		ClientId = 0;
		ScopeId = 0;
		bCreated = false;
	}
	uint64 ClientId = 0, ScopeId = 0;
	FStoreCredit LabelCredit;
	TCHAR* Label = nullptr;
	int32 Chars = 0;
	bool bCreated = false;
};
struct FNoteEntry
{
	FStorePacketPtr Owner;
	uint64 Serial = 0;
};
struct FTimelineSlot
{
	FVaRestInspectorTimelineEntry Value;
	uint64 NoteSerial = 0;
};
} // namespace

FStorePacketOwner::FStorePacketOwner(FStoreCredit&& InCredit, FPacket&& InData, FAllocationAdapter& InAllocator)
	: Credit(MoveTemp(InCredit))
	, Data(MoveTemp(InData))
	, Allocator(&InAllocator)
{
}
TOptional<int64> FStorePacketOwner::Charge(const FPacket& Packet, const FAllocationAdapter& Allocator)
{
	if (!Packet.IsValid())
		return {};
	const auto Wrapper = Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
	if (!Wrapper.IsSet() || Wrapper.GetValue() > SIZE_T(MAX_int64))
		return {};
	int64 Total = Packet.RetainedCapacity();
	if (!AddChecked(Total, int64(Wrapper.GetValue()), MAX_int64))
		return {};
	return Total;
}
bool FStorePacketOwner::CanTake(const FPacket& Packet, const FBudgetAccount* Budget, const FAllocationAdapter& Allocator)
{
	return Packet.IsValid() && Packet.Pending.IsValid() && Budget && Budget->Lifetime &&
		&Budget->Lifetime->Allocator == &Allocator && Packet.Allocator == &Allocator;
}
bool FStorePacketOwner::TryTake(FPacket& InOut, TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator, FStorePacketPtr& Out)
{
	if (Out.IsValid() || !CanTake(InOut, Budget.GetReference(), Allocator))
		return false;
	const auto Needed = Charge(InOut, Allocator);
	if (!Needed.IsSet())
		return false;
	auto Reserved = FStoreCredit::Acquire(Budget, Needed.GetValue());
	if (!Reserved.IsSet())
		return false;
	const auto Wrapper = Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
	if (!Wrapper.IsSet())
		return false;
	void* Memory = Allocator.AllocateReserved(sizeof(FStorePacketOwner), alignof(FStorePacketOwner), Wrapper.GetValue());
	if (!Memory)
		return false;
	if (InOut.Pending.IsValid() && Budget->UsedBytes.load() >= Needed.GetValue())
		Allocator.Counters().OverlapObservations.fetch_add(1);
	auto* Owner = new (Memory) FStorePacketOwner(MoveTemp(Reserved.GetValue()), MoveTemp(InOut), Allocator);
	Owner->Data.Pending.Reset();
	Out.Reset(Owner);
	return true;
}
void FStorePacketDeleter::operator()(FStorePacketOwner* Object) const
{
	if (!Object)
		return;
	FStoreCredit Credit = MoveTemp(Object->Credit);
	FAllocationAdapter* Allocator = Object->Allocator;
	Object->~FStorePacketOwner();
	Allocator->Free(Object);
	Credit.Reset();
}
FVaRestInspectorPacketView FStorePacketOwner::View() const
{
	FVaRestInspectorPacketView Result;
	if (!Data.IsValid())
		return Result;
	check(Data.BackingRequest <= SIZE_T(MAX_int32));
	Result.Fixed = &Data.Fixed();
	Result.Backing = {static_cast<const uint8*>(Data.Backing), int32(Data.BackingRequest)};
	if (Data.ExcerptOwner)
		Result.Excerpt = {Data.ExcerptOwner->GetData(), Data.ExcerptOwner->Num()};
	return Result;
}

bool FDispatchHeaderOwner::TryTake(FPacket& InOut, TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator,
	TSharedPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe>& Out)
{
	if (Out || !FStorePacketOwner::CanTake(InOut, Budget.GetReference(), Allocator) ||
		InOut.Fixed().Header.Kind != EVaRestTraceKind::AttemptDispatched || InOut.Fixed().Header.bMetadataOmitted || InOut.Headers().IsEmpty())
		return false;
	const auto Bound = Allocator.Bound(sizeof(FDispatchHeaderOwner), alignof(FDispatchHeaderOwner));
	if (!Bound.IsSet())
		return false;
	auto Candidate = MakeInspectorOwner<FDispatchHeaderOwner>(Budget, Allocator.GetSupport(), Bound.GetValue(), false);
	if (!Candidate || !FStorePacketOwner::TryTake(InOut, Budget, Allocator, Candidate->Packet))
		return false;
	Out = MoveTemp(Candidate);
	return true;
}
FVaRestInspectorPacketView FDispatchHeaderOwner::View() const
{
	return Packet ? Packet->View() : FVaRestInspectorPacketView();
}
bool FDispatchHeaderOwner::Matches(const FPacket& Other) const
{
	if (!Packet || !Other.IsValid() || Other.Fixed().Header.bMetadataOmitted)
		return false;
	const auto Existing = View();
	const auto Left = Existing.Pairs(Existing.Fixed->Headers);
	const auto Right = Other.Headers();
	if (Left.IsEmpty() || Left.Num() != Right.Num())
		return false;
	for (int32 Index = 0; Index < Left.Num(); ++Index)
		if (Existing.Read(Left[Index].First) != Other.Read(Right[Index].First) ||
			Existing.Read(Left[Index].Second) != Other.Read(Right[Index].Second))
			return false;
	return true;
}
void FDispatchSlotDeleter::operator()(FDispatchSlot* Slot) const
{
	if (!Slot)
		return;
	FStoreCredit Credit = MoveTemp(Slot->Credit);
	FAllocationAdapter& Allocator = Credit.GetAccount()->Lifetime->Allocator;
	Slot->~FDispatchSlot();
	Allocator.Free(Slot);
	Credit.Reset();
}

enum class EStoreIndexState : uint8
{
	Empty,
	Occupied,
	Deleted
};
struct FArrivalEntry
{
	int32 IndexSlot = INDEX_NONE;
	int32 Prev = INDEX_NONE, Next = INDEX_NONE;
	int32 NextFree = INDEX_NONE;
};
struct FClientLookupEntry
{
	EStoreIndexState State = EStoreIndexState::Empty;
	uint64 ClientId = 0;
	int32 ClientSlot = INDEX_NONE;
};
struct FStatsIndexEntry
{
	EStoreIndexState State = EStoreIndexState::Empty;
	FStatsGroupSlotPtr Group;
};
struct FLinkEntry
{
	uint64 FromRequestId = 0, ToRequestId = 0;
	FTicketedText QueueId;
	bool bAuth = false, bTargetEvicted = false, bOccupied = false;
	int32 ForwardIndexSlot = INDEX_NONE, TargetIndexSlot = INDEX_NONE;
	int32 PrevFifo = INDEX_NONE, NextFifo = INDEX_NONE;
	int32 PrevForTarget = INDEX_NONE, NextForTarget = INDEX_NONE;
	int32 NextFree = INDEX_NONE;
};
struct FLinkForwardEntry
{
	EStoreIndexState State = EStoreIndexState::Empty;
	int32 LinkSlot = INDEX_NONE;
};

struct FStore::FRow
{
	FVaRestInspectorRecordView Value;
	FRowKey Key;
	ELivenessEvidence Evidence = ELivenessEvidence::None;
	uint64 PresenceStamp = 0;
	FStorePacketPtr Started, Completed, RequestExcerpt, Marker;
	// This permanently admitted fixed slot preserves all actual terminal
	// scalars even when optional Completed packet ownership is refused.
	FPacketFixed CompletionFixed;
	TStoreBlock<FNoteEntry> Notes;
	TStoreBlock<FTimelineSlot> Timeline;
	int32 NoteHead = 0, NumNotes = 0, TimelineHead = 0, NumTimeline = 0;
	uint64 NextNoteSerial = 0;
	FDispatchSlotPtr Dispatches[16];
	int32 NumDispatches = 0, PreviousDispatchSlot = INDEX_NONE;
	int32 IndexSlot = INDEX_NONE, DeadlineIndex = INDEX_NONE, ArrivalSlot = INDEX_NONE;
};
struct FStore::FLinkTargetEntry
{
	EStoreIndexState State = EStoreIndexState::Empty;
	uint64 ToRequestId = 0;
	int32 HeadSlot = INDEX_NONE, TailSlot = INDEX_NONE;
	int32 Count = 0, IndexSlot = INDEX_NONE;
};
struct FStore::FStorage
{
	using EIndexState = EStoreIndexState;
	struct FIndexEntry
	{
		EIndexState State = EIndexState::Empty;
		FRowKey Key;
		TSharedPtr<FRow, ESPMode::ThreadSafe> Row;
		bool bRemovePending = false;
	};
	struct FDeadlineEntry
	{
		double Due = std::numeric_limits<double>::infinity();
		int32 IndexSlot = INDEX_NONE;
	};
	struct FRetainedIdEntry
	{
		uint64 RequestId = 0;
		int32 LookupSlot = INDEX_NONE;
	};
	struct FRetainedLookupEntry
	{
		EIndexState State = EIndexState::Empty;
		uint64 RequestId = 0;
		int32 RingSlot = INDEX_NONE;
	};
	explicit FStorage(FStoreCredit&& InCredit)
		: SelfCredit(MoveTemp(InCredit))
	{
	}
	FStoreCredit SelfCredit;
	TStoreBlock<FIndexEntry> Index;
	TStoreBlock<FArrivalEntry> Arrival;
	TStoreBlock<FClientEntry> Clients;
	TStoreBlock<FClientLookupEntry> ClientLookup;
	TStoreBlock<FDeadlineEntry> DeadlineHeap;
	TStoreBlock<FRetainedIdEntry> RetainedIds;
	TStoreBlock<FRetainedLookupEntry> RetainedLookup;
	TStoreBlock<FStatsIndexEntry> StatsIndex;
	FStatsGroupSlotPtr OverflowStats;
	TStoreBlock<FLinkEntry> Links;
	TStoreBlock<FLinkForwardEntry> LinkForward;
	TStoreBlock<FLinkTargetEntry> LinkTargets;
	FStoreStoragePlan Plan;
	int32 IndexCapacity = 0, MaxRows = 0, ClientCapacity = 0;
	int32 RetainedCapacity = 0, RetainedLookupCapacity = 0;
	int32 RetainedHead = 0, NumRetained = 0, NumDeadline = 0;
	int32 ArrivalHead = INDEX_NONE, ArrivalTail = INDEX_NONE, ArrivalFreeHead = INDEX_NONE, NextArrivalSlot = 0;
	int32 NumRows = 0, NumClients = 0;
	int32 NumStatsGroups = 0, NumLinkSlotsUsed = 0, NumLinks = 0;
	int32 LinkFreeHead = INDEX_NONE, LinkHead = INDEX_NONE, LinkTail = INDEX_NONE;
};
void FTicketedText::Reset()
{
	if (Data)
	{
		check(Credit.GetAccount() && Credit.GetAccount()->Lifetime);
		Credit.GetAccount()->Lifetime->Allocator.Free(const_cast<TCHAR*>(Data));
	}
	Data = nullptr;
	Chars = 0;
	Credit.Reset();
}
void FStatsGroupSlotDeleter::operator()(FStatsGroupSlot* Slot) const
{
	if (!Slot)
		return;
	FStoreCredit Credit = MoveTemp(Slot->ObjectCredit);
	FAllocationAdapter& Allocator = Credit.GetAccount()->Lifetime->Allocator;
	Slot->~FStatsGroupSlot();
	Allocator.Free(Slot);
	Credit.Reset();
}

namespace
{
uint32 HashRequestId(uint64 RequestId)
{
	return uint32(RequestId ^ (RequestId >> 32)) * 0x9e3779b9u;
}
uint32 HashRowKey(const FRowKey& Key)
{
	const uint32 IdHash = HashRequestId(Key.Id);
	return Key.Kind == ERowKeyKind::Http ? IdHash : HashCombine(IdHash, static_cast<uint32>(Key.Kind));
}
uint32 HashStatsKey(const FStatsKeyView& Key)
{
	uint32 Hash = GetTypeHash(Key.ScopeId);
	Hash = HashCombine(Hash, GetTypeHash(Key.ClientId));
	Hash = HashCombine(Hash, GetTypeHash(Key.Endpoint));
	return HashCombine(Hash, GetTypeHash(Key.Key));
}
uint32 HashLinkSeed(const FLinkSeed& Seed)
{
	uint32 Hash = GetTypeHash(Seed.FromRequestId);
	Hash = HashCombine(Hash, GetTypeHash(Seed.ToRequestId));
	Hash = HashCombine(Hash, GetTypeHash(Seed.QueueId));
	return HashCombine(Hash, Seed.bAuth ? 1u : 0u);
}
}

void FStore::ForEachRow(TFunctionRef<void(FRow&)> Visitor) const
{
	if (!Storage)
		return;
	for (int32 Slot = Storage->ArrivalHead; Slot != INDEX_NONE; Slot = Storage->Arrival[Slot].Next)
	{
		const int32 IndexSlot = Storage->Arrival[Slot].IndexSlot;
		check(IndexSlot >= 0 && IndexSlot < Storage->IndexCapacity && Storage->Index[IndexSlot].Row);
		Visitor(*Storage->Index[IndexSlot].Row);
	}
}
FStoreStoragePlan FStore::EffectiveStoragePlan() const
{
	return Storage ? Storage->Plan : FStoreStoragePlan{};
}
int32 FStore::NumClients() const
{
	return Storage ? Storage->NumClients : 0;
}
int32 FStore::NumStatsGroups() const
{
	return Storage ? Storage->NumStatsGroups : 0;
}
int32 FStore::NumLinks() const
{
	return Storage ? Storage->NumLinks : 0;
}
int64 FStore::RowStorageBound() const
{
	if (!Budget || !Budget->Lifetime)
		return 0;
	const auto Bound = Budget->Lifetime->Allocator.Bound(sizeof(FRow), alignof(FRow));
	return Bound.IsSet() ? int64(Bound.GetValue()) : 0;
}
int64 FStore::TimelineStorageBound() const
{
	if (!Budget || !Budget->Lifetime)
		return 0;
	const FStoreBlockMeasure Measure = MeasureStoreBlock<FTimelineSlot>(*Budget, FNormalizedLimits::TimelineSlots);
	return Measure.bSupported ? Measure.Bytes : 0;
}
bool FStore::CanReleaseWhole(const FRow& Row)
{
	return !Row.Value.bPinned &&
		(Row.Value.Kind == EVaRestInspectorRecordKind::Marker || Row.Value.bTerminalKnown || Row.Value.bSealed ||
		 Row.Value.EndState == EVaRestInspectorEndState::CaptureInterrupted);
}
void FStore::FStorageDeleter::operator()(FStorage* Object) const
{
	if (!Object)
		return;
	FStoreCredit Credit = MoveTemp(Object->SelfCredit);
	FAllocationAdapter& Allocator = Credit.GetAccount()->Lifetime->Allocator;
	Object->~FStorage();
	Allocator.Free(Object);
	Credit.Reset();
}
FStore::FStore(const FNormalizedLimits& InLimits, TSharedRef<IVaRestClock, ESPMode::ThreadSafe> InClock)
	: Limits(InLimits)
	, RequestedLimits(InLimits)
	, Clock(MoveTemp(InClock))
{
	check(IsInGameThread());
	State.EffectiveStoreLimit = Limits.StoreBytes;
	State.RequestedStoreLimit = Limits.StoreBytes;
}
FStore::~FStore()
{
	check(IsInGameThread() && !bEnumerating);
	Storage.Reset();
}
bool FStore::CalibrateRows(FAllocationLifetime& Lifetime, std::atomic<int64>& StartupUsed, int64 StartupLimit)
{
	check(IsInGameThread());
	using D = TInspectorDeleter<FRow>;
	using C = SharedPointerInternals::TReferenceControllerWithDeleter<FRow, D, ESPMode::ThreadSafe>;
	using HeaderD = TInspectorDeleter<FDispatchHeaderOwner>;
	using HeaderC = SharedPointerInternals::TReferenceControllerWithDeleter<FDispatchHeaderOwner, HeaderD, ESPMode::ThreadSafe>;
	using SnapshotD = TInspectorDeleter<FVaRestInspectorSnapshot>;
	using SnapshotC = SharedPointerInternals::TReferenceControllerWithDeleter<FVaRestInspectorSnapshot, SnapshotD, ESPMode::ThreadSafe>;
	constexpr uint32 RowNewAlignment = sizeof(C) <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	constexpr uint32 HeaderNewAlignment = sizeof(HeaderC) <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	constexpr uint32 SnapshotNewAlignment = sizeof(SnapshotC) <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	static_assert(sizeof(C) == sizeof(HeaderC) && sizeof(C) == sizeof(SnapshotC) && sizeof(C) > 0 && sizeof(C) <= 1024,
		"Shared retained calibration requires equal concrete controller requests");
	static_assert(alignof(C) > 0 && alignof(C) <= 16 && alignof(HeaderC) > 0 && alignof(HeaderC) <= 16 &&
		alignof(SnapshotC) > 0 && alignof(SnapshotC) <= 16 && RowNewAlignment == HeaderNewAlignment && RowNewAlignment == SnapshotNewAlignment,
		"All concrete controllers must use the calibrated ordinary new alignment");
	return Lifetime.Allocator.Calibrate(StartupUsed, StartupLimit, sizeof(C), alignof(C)) &&
		HasInspectorControllerCalibration<FRow>(Lifetime) && HasInspectorControllerCalibration<FDispatchHeaderOwner>(Lifetime) &&
		HasInspectorControllerCalibration<FVaRestInspectorSnapshot>(Lifetime);
}
FStoreStoragePlan FStore::MeasureStoragePlan(const FNormalizedLimits& InLimits, const FBudgetAccount& Account)
{
	FStoreStoragePlan Plan;
	if (!Account.Lifetime)
		return Plan;
	FAllocationAdapter& Allocator = Account.Lifetime->Allocator;
	const auto SelfBound = Allocator.Bound(sizeof(FStorage), alignof(FStorage));
	const auto RowBound = Allocator.Bound(sizeof(FRow), alignof(FRow));
	const auto PacketBound = Allocator.Bound(sizeof(FPacketFixed), alignof(FPacketFixed));
	const auto OwnerBound = Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
	const auto OverflowBound = Allocator.Bound(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot));
	if (!SelfBound.IsSet() || !RowBound.IsSet() || !PacketBound.IsSet() || !OwnerBound.IsSet() || !OverflowBound.IsSet())
		return Plan;
	const auto NextPow2 = [](int32 Needed) {
		int32 Capacity = 2;
		while (Capacity < Needed)
			Capacity *= 2;
		return Capacity;
	};
	Plan.MaxRows = InLimits.MaxRecords;
	Plan.ClientCapacity = FNormalizedLimits::Clients;
	Plan.StatsGroupCapacity = FNormalizedLimits::StatsGroups;
	Plan.LinkCapacity = FNormalizedLimits::Links;
	while (true)
	{
		Plan.IndexCapacity = NextPow2(Plan.MaxRows * 2);
		Plan.ArrivalCapacity = Plan.MaxRows;
		Plan.ClientLookupCapacity = NextPow2(Plan.ClientCapacity * 2);
		Plan.DeadlineCapacity = Plan.MaxRows;
		Plan.RetainedCapacity = FMath::Min(FNormalizedLimits::Tombstones, Plan.MaxRows);
		Plan.RetainedLookupCapacity = NextPow2(Plan.RetainedCapacity * 2);
		const int32 NamedStats = FMath::Max(0, Plan.StatsGroupCapacity - 1);
		Plan.StatsIndexCapacity = NamedStats ? NextPow2(NamedStats * 2) : 0;
		Plan.LinkIndexCapacity = NextPow2(Plan.LinkCapacity * 2);
		Plan.LinkTargetCapacity = NextPow2(Plan.LinkCapacity * 2);
		Plan.FixedBytes = Account.BootstrapBytes;
		Plan.MinimumHttpBytes = 0;
		bool bValid = AddChecked(Plan.FixedBytes, int64(SelfBound.GetValue()), MAX_int64) &&
			AddChecked(Plan.FixedBytes, int64(OverflowBound.GetValue()), MAX_int64) &&
			AddChecked(Plan.MinimumHttpBytes, int64(RowBound.GetValue()), MAX_int64) &&
			AddChecked(Plan.MinimumHttpBytes, 4096, MAX_int64) &&
			AddChecked(Plan.MinimumHttpBytes, int64(PacketBound.GetValue()), MAX_int64) &&
			AddChecked(Plan.MinimumHttpBytes, int64(OwnerBound.GetValue()), MAX_int64);
		const auto AddMeasure = [&Plan](const FStoreBlockMeasure& Measure) {
			return Measure.bSupported && AddChecked(Plan.FixedBytes, Measure.Bytes, MAX_int64);
		};
		bValid = bValid && AddMeasure(MeasureStoreBlock<FStorage::FIndexEntry>(Account, Plan.IndexCapacity)) &&
			AddMeasure(MeasureStoreBlock<FArrivalEntry>(Account, Plan.ArrivalCapacity)) &&
			AddMeasure(MeasureStoreBlock<FClientEntry>(Account, Plan.ClientCapacity)) &&
			AddMeasure(MeasureStoreBlock<FClientLookupEntry>(Account, Plan.ClientLookupCapacity)) &&
			AddMeasure(MeasureStoreBlock<FStorage::FDeadlineEntry>(Account, Plan.DeadlineCapacity)) &&
			AddMeasure(MeasureStoreBlock<FStorage::FRetainedIdEntry>(Account, Plan.RetainedCapacity)) &&
			AddMeasure(MeasureStoreBlock<FStorage::FRetainedLookupEntry>(Account, Plan.RetainedLookupCapacity)) &&
			AddMeasure(MeasureStoreBlock<FStatsIndexEntry>(Account, Plan.StatsIndexCapacity)) &&
			AddMeasure(MeasureStoreBlock<FLinkEntry>(Account, Plan.LinkCapacity)) &&
			AddMeasure(MeasureStoreBlock<FLinkForwardEntry>(Account, Plan.LinkIndexCapacity)) &&
			AddMeasure(MeasureStoreBlock<FLinkTargetEntry>(Account, Plan.LinkTargetCapacity));
		if (!bValid)
		{
			Plan.bSupported = false;
			Plan.bFits = false;
			return Plan;
		}
		Plan.bSupported = true;
		Plan.bFits = Plan.FixedBytes <= Account.Limit / 4 && Plan.FixedBytes + Plan.MinimumHttpBytes <= Account.Limit;
		if (Plan.bFits)
			return Plan;
		if (Plan.ClientCapacity > 1)
			Plan.ClientCapacity /= 2;
		else if (Plan.StatsGroupCapacity > 1)
			Plan.StatsGroupCapacity /= 2;
		else if (Plan.LinkCapacity > 1)
			Plan.LinkCapacity /= 2;
		else if (Plan.MaxRows > 1)
			Plan.MaxRows /= 2;
		else
			return Plan;
	}
}
bool FStore::Initialize(TRefCountPtr<FBudgetAccount> InAccount)
{
	check(IsInGameThread());
	if (!InAccount || InAccount->Identity == 0 ||
		(OriginatingAccountIdentity != 0 && OriginatingAccountIdentity != InAccount->Identity))
		return false;
	const uint64 Identity = InAccount->Identity;
	if (!InitializeStorage(MoveTemp(InAccount)))
		return false;
	OriginatingAccountIdentity = Identity;
	return true;
}
bool FStore::InitializeStorage(TRefCountPtr<FBudgetAccount> InAccount)
{
	check(IsInGameThread());
	if (bEnumerating)
		return false;
	if (!InAccount || !InAccount->Lifetime || InAccount->Limit != Limits.StoreBytes ||
		InAccount->BootstrapBytes < 4096 || InAccount->BootstrapBytes > InAccount->Limit ||
		Limits.MaxRecords < 1 || Limits.MaxRecords > 65536 || Limits.StoreBytes <= 0 ||
		!HasInspectorControllerCalibration<FRow>(*InAccount->Lifetime) ||
		!HasInspectorControllerCalibration<FDispatchHeaderOwner>(*InAccount->Lifetime) ||
		!HasInspectorControllerCalibration<FVaRestInspectorSnapshot>(*InAccount->Lifetime))
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	if (Storage)
		return Budget == InAccount;
	const FStoreStoragePlan Plan = MeasureStoragePlan(Limits, *InAccount);
	if (!Plan.bSupported)
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	if (!Plan.bFits)
	{
		Increment(State.RefusedRows);
		return false;
	}
	FAllocationAdapter& Allocator = InAccount->Lifetime->Allocator;
	const auto SelfBound = Allocator.Bound(sizeof(FStorage), alignof(FStorage));
	if (!SelfBound.IsSet())
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	auto Credit = FStoreCredit::Acquire(InAccount, int64(SelfBound.GetValue()));
	if (!Credit.IsSet())
	{
		Increment(State.RefusedRows);
		return false;
	}
	void* Memory = Allocator.AllocateReserved(sizeof(FStorage), alignof(FStorage), SelfBound.GetValue());
	if (!Memory)
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	TUniquePtr<FStorage, FStorageDeleter> Candidate(new (Memory) FStorage(MoveTemp(Credit.GetValue())));
	if (!Candidate->Index.Allocate(InAccount, Plan.IndexCapacity) || !Candidate->Arrival.Allocate(InAccount, Plan.ArrivalCapacity) ||
		!Candidate->Clients.Allocate(InAccount, Plan.ClientCapacity) || !Candidate->ClientLookup.Allocate(InAccount, Plan.ClientLookupCapacity) ||
		!Candidate->DeadlineHeap.Allocate(InAccount, Plan.DeadlineCapacity) || !Candidate->RetainedIds.Allocate(InAccount, Plan.RetainedCapacity) ||
		!Candidate->RetainedLookup.Allocate(InAccount, Plan.RetainedLookupCapacity) || !Candidate->StatsIndex.Allocate(InAccount, Plan.StatsIndexCapacity) ||
		!Candidate->Links.Allocate(InAccount, Plan.LinkCapacity) || !Candidate->LinkForward.Allocate(InAccount, Plan.LinkIndexCapacity) ||
		!Candidate->LinkTargets.Allocate(InAccount, Plan.LinkTargetCapacity))
	{
		Increment(State.RefusedRows);
		return false;
	}
	const auto OverflowBound = Allocator.Bound(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot));
	if (!OverflowBound.IsSet())
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	auto OverflowCredit = FStoreCredit::Acquire(InAccount, int64(OverflowBound.GetValue()));
	if (!OverflowCredit.IsSet())
	{
		Increment(State.RefusedRows);
		return false;
	}
	void* OverflowMemory = Allocator.AllocateReserved(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot), OverflowBound.GetValue());
	if (!OverflowMemory)
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	Candidate->OverflowStats.Reset(new (OverflowMemory) FStatsGroupSlot());
	Candidate->OverflowStats->ObjectCredit = MoveTemp(OverflowCredit.GetValue());
	Candidate->OverflowStats->Value.bOverflow = true;
	Candidate->NumStatsGroups = 1;
	Candidate->IndexCapacity = Plan.IndexCapacity;
	Candidate->MaxRows = Plan.MaxRows;
	Candidate->ClientCapacity = Plan.ClientCapacity;
	Candidate->RetainedCapacity = Plan.RetainedCapacity;
	Candidate->RetainedLookupCapacity = Plan.RetainedLookupCapacity;
	Candidate->Plan = Plan;
	Budget = MoveTemp(InAccount);
	Storage = MoveTemp(Candidate);
	return true;
}
FStore::FRow* FStore::Find(uint64 RequestId) const
{
	return FindRow(FRowKey(RequestId));
}
FStore::FRow* FStore::FindRow(const FRowKey& Key) const
{
	if (!Storage || Key.Id == 0)
		return nullptr;
	const uint32 Mask = uint32(Storage->IndexCapacity - 1);
	const uint32 Start = HashRowKey(Key);
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Entry = Storage->Index[int32((Start + Offset) & Mask)];
		if (Entry.State == FStorage::EIndexState::Empty)
			return nullptr;
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.Key.Kind == Key.Kind && Entry.Key.Id == Key.Id)
			return Entry.Row.Get();
	}
	return nullptr;
}
void FStore::AppendArrival(FRow& Row)
{
	check(Storage && Row.IndexSlot >= 0 && Row.IndexSlot < Storage->IndexCapacity && Row.ArrivalSlot == INDEX_NONE);
	int32 Slot = Storage->ArrivalFreeHead;
	if (Slot != INDEX_NONE)
		Storage->ArrivalFreeHead = Storage->Arrival[Slot].NextFree;
	else
		Slot = Storage->NextArrivalSlot++;
	check(Slot >= 0 && Slot < Storage->Plan.ArrivalCapacity && Storage->NextArrivalSlot <= Storage->Plan.ArrivalCapacity);
	auto& Entry = Storage->Arrival[Slot];
	Entry.IndexSlot = Row.IndexSlot;
	Entry.Prev = Storage->ArrivalTail;
	Entry.Next = INDEX_NONE;
	Entry.NextFree = INDEX_NONE;
	if (Storage->ArrivalTail != INDEX_NONE)
		Storage->Arrival[Storage->ArrivalTail].Next = Slot;
	else
		Storage->ArrivalHead = Slot;
	Storage->ArrivalTail = Slot;
	Row.ArrivalSlot = Slot;
	++Storage->NumRows;
}
void FStore::UnlinkArrival(FRow& Row)
{
	check(Storage && Row.ArrivalSlot >= 0 && Row.ArrivalSlot < Storage->Plan.ArrivalCapacity);
	const int32 Slot = Row.ArrivalSlot;
	auto& Entry = Storage->Arrival[Slot];
	check(Entry.IndexSlot == Row.IndexSlot && Storage->NumRows > 0);
	if (Entry.Prev != INDEX_NONE)
		Storage->Arrival[Entry.Prev].Next = Entry.Next;
	else
		Storage->ArrivalHead = Entry.Next;
	if (Entry.Next != INDEX_NONE)
		Storage->Arrival[Entry.Next].Prev = Entry.Prev;
	else
		Storage->ArrivalTail = Entry.Prev;
	const int32 PreviousFree = Storage->ArrivalFreeHead;
	Entry = FArrivalEntry{};
	Entry.NextFree = PreviousFree;
	Storage->ArrivalFreeHead = Slot;
	Row.ArrivalSlot = INDEX_NONE;
	--Storage->NumRows;
}
void FStore::ResetArrival()
{
	if (!Storage)
		return;
	for (int32 Index = 0; Index < Storage->Plan.ArrivalCapacity; ++Index)
		Storage->Arrival[Index] = FArrivalEntry{};
	Storage->ArrivalHead = Storage->ArrivalTail = Storage->ArrivalFreeHead = INDEX_NONE;
	Storage->NextArrivalSlot = 0;
	Storage->NumRows = 0;
}
bool FStore::MakeAdmissionRoom(const FAdmissionRequest& Request)
{
	if (!Storage || !Budget || bEnumerating || Request.NewRows < 0 || Request.NewBytes < 0)
		return false;
	RefreshLossWitness();
	const auto Available = [&] {
		return Request.NewRows <= Storage->MaxRows - Storage->NumRows &&
			Request.NewBytes <= Budget->Limit - Budget->UsedBytes.load(std::memory_order_acquire);
	};
	int32 Scanned = 0;
	int32 Arrival = Storage->ArrivalHead;
	while (!Available() && Arrival != INDEX_NONE && Scanned < Limits.MaxRecords)
	{
		const int32 Current = Arrival;
		Arrival = Storage->Arrival[Current].Next;
		const int32 IndexSlot = Storage->Arrival[Current].IndexSlot;
		if (IndexSlot == Request.Anchors.MutationIndexSlot || IndexSlot == Request.Anchors.BorrowedSourceIndexSlot)
		{
			++Scanned;
			continue;
		}
		FRow& Row = *Storage->Index[IndexSlot].Row;
		if (CanReleaseWhole(Row))
			RemoveEligibleRowNow(IndexSlot);
		++Scanned;
	}
	if (Request.NewRows > Storage->MaxRows - Storage->NumRows)
		return false;
	if (Request.NewBytes > 0)
	{
		Arrival = Storage->ArrivalHead;
		Scanned = 0;
		while (!Available() && Arrival != INDEX_NONE && Scanned < Limits.MaxRecords)
		{
			const int32 Current = Arrival;
			Arrival = Storage->Arrival[Current].Next;
			const int32 IndexSlot = Storage->Arrival[Current].IndexSlot;
			if (IndexSlot == Request.Anchors.MutationIndexSlot || IndexSlot == Request.Anchors.BorrowedSourceIndexSlot)
			{
				++Scanned;
				continue;
			}
			FRow& Row = *Storage->Index[IndexSlot].Row;
			if (Row.Value.bPinned && !Row.Value.bSealed && Row.Value.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched)
				TrimPinnedDetails(Row);
			++Scanned;
		}
	}
	return Available();
}
bool FStore::TakePacket(FRow& Anchor, FPacket& Packet, FStorePacketPtr& Out)
{
	if (Out.IsValid())
		return false;
	const auto Needed = FStorePacketOwner::Charge(Packet, Budget->Lifetime->Allocator);
	if (!Needed.IsSet())
		return false;
	FAdmissionAnchors Anchors{Anchor.IndexSlot, INDEX_NONE};
	if (!MakeAdmissionRoom({0, Needed.GetValue(), Anchors}))
		return false;
	return FStorePacketOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Out);
}
bool FStore::TakeHeader(FRow& Anchor, FPacket& Packet, TSharedPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe>& Out)
{
	if (Out)
		return false;
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const auto PacketCharge = FStorePacketOwner::Charge(Packet, Allocator);
	const auto OwnerBound = Allocator.Bound(sizeof(FDispatchHeaderOwner), alignof(FDispatchHeaderOwner));
	if (!PacketCharge.IsSet() || !OwnerBound.IsSet())
		return false;
	int64 Needed = 4096;
	if (!AddChecked(Needed, int64(OwnerBound.GetValue()), MAX_int64) || !AddChecked(Needed, PacketCharge.GetValue(), MAX_int64))
		return false;
	if (!MakeAdmissionRoom({0, Needed, {Anchor.IndexSlot, INDEX_NONE}}))
		return false;
	return FDispatchHeaderOwner::TryTake(Packet, Budget, Allocator, Out);
}
FStore::FRow* FStore::AdmitRow(const FRowKey& Key, bool bPartial)
{
	if (!Storage || Key.Id == 0 || (Key.Kind == ERowKeyKind::Http && Key.Id == 0) || NextArrival == MAX_uint64)
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	const uint32 Mask = uint32(Storage->IndexCapacity - 1);
	const uint32 Start = HashRowKey(Key);
	int32 Slot = INDEX_NONE;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		auto& Entry = Storage->Index[Candidate];
		if (Entry.State == FStorage::EIndexState::Occupied)
		{
			if (Entry.Key.Kind == Key.Kind && Entry.Key.Id == Key.Id)
				return Entry.Row.Get();
			continue;
		}
		if (Entry.State == FStorage::EIndexState::Deleted)
		{
			if (Slot == INDEX_NONE)
				Slot = Candidate;
			continue;
		}
		if (Slot == INDEX_NONE)
			Slot = Candidate;
		break;
	}
	if (Slot == INDEX_NONE)
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const auto Bound = Allocator.Bound(sizeof(FRow), alignof(FRow));
	if (!Bound.IsSet())
	{
		Increment(State.UnsupportedAllocator);
		Increment(State.RefusedRows);
		return nullptr;
	}
	int64 Needed = int64(Bound.GetValue());
	if (!AddChecked(Needed, 4096, MAX_int64))
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	if (!MakeAdmissionRoom({1, Needed, {}}))
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	auto Row = MakeInspectorOwner<FRow>(Budget, Allocator.GetSupport(), Bound.GetValue(), false);
	if (!Row)
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	Row->Key = Key;
	Row->Value.Kind = Key.Kind == ERowKeyKind::Http ? EVaRestInspectorRecordKind::Http : EVaRestInspectorRecordKind::Marker;
	Row->Value.RequestId = Key.Kind == ERowKeyKind::Http ? Key.Id : 0;
	Row->Value.bPartial = bPartial;
	Row->Value.ArrivalOrdinal = ++NextArrival;
	Row->Value.BirthMetadataOrdinal = LastConsumedMetadataOrdinal;
	Row->Value.DispatchHistory = EVaRestInspectorDispatchHistory::Unknown;
	Row->Value.StartedHeadersState = EVaRestInspectorHeadersState::Unknown;
	Row->IndexSlot = Slot;
	Storage->Index[Slot].State = FStorage::EIndexState::Occupied;
	Storage->Index[Slot].Key = Key;
	Storage->Index[Slot].Row = MoveTemp(Row);
	Storage->Index[Slot].bRemovePending = false;
	AppendArrival(*Storage->Index[Slot].Row);
	FRow* Result = Storage->Index[Slot].Row.Get();
	Touch(*Result);
	return Result;
}
void FStore::Touch(FRow& Row) const
{
	DeriveCompletionPhases(Row.Value);
	Increment(RevisionValue);
	Row.Value.Revision = RevisionValue;
}
void FStore::SetStartedScalars(FRow& Row, const FPacketScalars& Scalars)
{
	Row.Value.ScopeId = Scalars.ScopeId;
	Row.Value.ClientId = Scalars.ClientId;
	if (!Row.Started && Row.Value.ObservedDispatches == 0 && Row.Evidence != ELivenessEvidence::Snapshot)
		Row.Value.CurrentAttempt = FMath::Max(Row.Value.CurrentAttempt, Scalars.CurrentAttempt);
	if (!Row.Value.bStartedAtKnown && FMath::IsFinite(Scalars.StartedAt) && Scalars.StartedAt >= 0)
	{
		Row.Value.StartedAt = Scalars.StartedAt;
		Row.Value.bStartedAtKnown = true;
	}
	if (!Row.Value.bStartUtcKnown && Scalars.StartUtc.GetTicks() != 0)
	{
		Row.Value.StartUtc = Scalars.StartUtc;
		Row.Value.bStartUtcKnown = true;
	}
	Row.Value.bPartial |= EnumHasAnyFlags(Scalars.Flags, EVaRestTraceRequestFlags::Partial);
	if (Scalars.bExcerptPending && Row.Value.RequestExcerptState == EVaRestInspectorDetailState::Absent)
		Row.Value.RequestExcerptState = EVaRestInspectorDetailState::Pending;
}
FStore::EAttachedBase FStore::PrepareAttachedBase(uint64 RequestId, bool& bNeedsStarted)
{
	bNeedsStarted = false;
	if (!Storage || RequestId == 0)
		return EAttachedBase::Refused;
	FRow* Row = Find(RequestId);
	if (!Row)
		Row = AdmitRow(RequestId, true);
	if (!Row)
		return EAttachedBase::Refused;
	if (Row->Value.bTerminalKnown || Row->Value.bSealed)
		return EAttachedBase::Terminal;
	Row->PresenceStamp = CurrentReconciliationStamp;
	Row->Evidence = ELivenessEvidence::Snapshot;
	Row->Value.bPartial = true;
	Row->Value.bPinned = true;
	Row->Value.EndState = EVaRestInspectorEndState::InFlight;
	bNeedsStarted = !Row->Started;
	Touch(*Row);
	return EAttachedBase::Mutable;
}
void FStore::AdoptAttachedPacket(uint64 RequestId, FStorePacketPtr&& Packet)
{
	FRow* Row = Find(RequestId);
	check(Row && Packet && !Row->Started);
	const FPacket& Data = Packet->Get();
	SetStartedScalars(*Row, Data.Fixed().Scalars);
	Row->Value.bInitiatorKnown = !Data.Text(EText::OwnerPath).IsEmpty() || !Data.Text(EText::ScriptObject).IsEmpty() ||
								 !Data.Text(EText::ScriptFunction).IsEmpty() || !Data.Text(EText::SourceFile).IsEmpty() ||
								 Data.Fixed().Scalars.ScriptCodeOffset != INDEX_NONE || Data.Fixed().Scalars.SourceLine != 0;
	Row->Value.bMetadataOmitted |= Data.Fixed().Header.bMetadataOmitted;
	if (Data.Fixed().Header.bMetadataOmitted)
		Row->Value.StartedHeadersState = EVaRestInspectorHeadersState::MetadataOmitted;
	else if (!Data.Headers().IsEmpty())
		Row->Value.StartedHeadersState = EVaRestInspectorHeadersState::Captured;
	Row->Started = MoveTemp(Packet);
	Touch(*Row);
}
void FStore::NoteAttachLoss(uint64 RequestId)
{
	Increment(State.AttachLosses);
	Increment(State.MetadataOmitted);
	Increment(OrphanEpoch);
	if (FRow* Row = Find(RequestId))
	{
		Row->Value.bMetadataOmitted = true;
		Touch(*Row);
	}
}
bool FStore::ApplyAttachedStarted(const FVaRestTraceStarted& Borrowed, const FCapturePolicy& Policy,
	TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator)
{
	bool bNeedsStarted = false;
	const EAttachedBase Base = PrepareAttachedBase(Borrowed.RequestId, bNeedsStarted);
	if (Base == EAttachedBase::Refused)
		return false;
	if (Base == EAttachedBase::Terminal)
		return true;
	// Retain independently available scalar history even if optional text fails.
	FRow* Row = Find(Borrowed.RequestId);
	check(Row);
	Row->Value.ScopeId = Borrowed.ScopeId;
	Row->Value.ClientId = Borrowed.ClientId;
	Row->Value.CurrentAttempt = Borrowed.CurrentAttempt;
	if (!Row->Value.bStartedAtKnown && FMath::IsFinite(Borrowed.StartedAt) && Borrowed.StartedAt >= 0)
	{
		Row->Value.StartedAt = Borrowed.StartedAt;
		Row->Value.bStartedAtKnown = true;
	}
	if (!Row->Value.bStartUtcKnown && Borrowed.StartUtc.GetTicks() != 0)
	{
		Row->Value.StartUtc = Borrowed.StartUtc;
		Row->Value.bStartUtcKnown = true;
	}
	if (Policy.bBodies && Borrowed.bExcerptPending && Row->Value.RequestExcerptState == EVaRestInspectorDetailState::Absent)
		Row->Value.RequestExcerptState = EVaRestInspectorDetailState::Pending;
	Touch(*Row);
	if (!bNeedsStarted)
		return true;
	FPacketHeader Header;
	Header.bSnapshot = true;
	Header.Generation = Generation;
	Header.Time = Borrowed.StartedAt;
	FPacketLayout Layout;
	const EMeasure Measured = FPacketCodec::MeasureStarted(Borrowed, Header, Policy, Allocator, Layout);
	if (Measured == EMeasure::Unsupported)
	{
		NoteAttachLoss(Borrowed.RequestId);
		return false;
	}
	auto Ticket = FPendingTicket::Acquire(Pending, Layout.PendingCharge);
	if (!Ticket.IsSet())
	{
		NoteAttachLoss(Borrowed.RequestId);
		return false;
	}
	FPacket Packet = FPacketCodec::WriteStarted(Borrowed, Layout, MoveTemp(Ticket.GetValue()), Allocator);
	if (!Packet.IsValid())
	{
		NoteAttachLoss(Borrowed.RequestId);
		return false;
	}
	FStorePacketPtr Stored;
	if (!TakePacket(*Row, Packet, Stored))
	{
		NoteAttachLoss(Borrowed.RequestId);
		return false;
	}
	AdoptAttachedPacket(Borrowed.RequestId, MoveTemp(Stored));
	const FPacketFixed& Attached = Row->Started->Get().Fixed();
	FVaRestInspectorTimelineEntry Timeline;
	Timeline.Kind = Attached.Header.Kind;
	Timeline.Seq = Attached.Header.Seq;
	Timeline.Time = Attached.Header.Time;
	Timeline.Attempt = Attached.Scalars.Attempt;
	Timeline.DispatchId = Attached.Scalars.DispatchId;
	Timeline.DetailState = Attached.Header.bMetadataOmitted ?
		EVaRestInspectorDetailState::MetadataOmitted : EVaRestInspectorDetailState::Present;
	AppendTimeline(*Row, Timeline);
	if (Measured == EMeasure::ScalarOnly)
		NoteAttachLoss(Borrowed.RequestId);
	return true;
}
void FStore::ApplyClient(uint64 ClientId, uint64 ScopeId, bool bCreated, FStringView Label)
{
	if (!Storage)
		return;
	if (ClientId == 0)
	{
		Increment(State.OmittedClients);
		return;
	}
	const uint32 Mask = uint32(Storage->Plan.ClientLookupCapacity - 1);
	const uint32 Start = HashRequestId(ClientId);
	int32 LookupSlot = INDEX_NONE;
	FClientEntry* Entry = nullptr;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		auto& Lookup = Storage->ClientLookup[Candidate];
		if (Lookup.State == FStorage::EIndexState::Empty)
		{
			if (LookupSlot == INDEX_NONE)
				LookupSlot = Candidate;
			break;
		}
		if (Lookup.State == FStorage::EIndexState::Deleted)
		{
			if (LookupSlot == INDEX_NONE)
				LookupSlot = Candidate;
			continue;
		}
		if (Lookup.ClientId == ClientId)
		{
			Entry = &Storage->Clients[Lookup.ClientSlot];
			LookupSlot = Candidate;
			break;
		}
	}
	if (!Entry)
	{
		if (Storage->NumClients >= Storage->ClientCapacity || LookupSlot == INDEX_NONE)
		{
			Increment(State.OmittedClients);
			return;
		}
		const int32 ClientSlot = Storage->NumClients++;
		Entry = &Storage->Clients[ClientSlot];
		Entry->ClientId = ClientId;
		auto& Lookup = Storage->ClientLookup[LookupSlot];
		Lookup.State = FStorage::EIndexState::Occupied;
		Lookup.ClientId = ClientId;
		Lookup.ClientSlot = ClientSlot;
	}
	Entry->ScopeId = ScopeId;
	Entry->bCreated = bCreated;
	if (Label.IsEmpty() || (Entry->Label && FStringView(Entry->Label, Entry->Chars) == Label))
		return;
	if (Label.Len() < 0 || Label.Len() > FNormalizedLimits::MaxStringChars || (Label.Len() > 0 && !Label.GetData()))
	{
		Increment(State.OmittedClients);
		return;
	}
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const SIZE_T Request = (SIZE_T(Label.Len()) + 1) * sizeof(TCHAR);
	const auto Bound = Allocator.Bound(Request, alignof(TCHAR));
	if (!Bound.IsSet() || !MakeAdmissionRoom({0, int64(Bound.GetValue()), {}}))
	{
		Increment(State.OmittedClients);
		return;
	}
	auto Credit = FStoreCredit::Acquire(Budget, int64(Bound.GetValue()));
	if (!Credit.IsSet())
	{
		Increment(State.OmittedClients);
		return;
	}
	void* Memory = Allocator.AllocateReserved(Request, alignof(TCHAR), Bound.GetValue());
	if (!Memory)
	{
		Increment(State.OmittedClients);
		return;
	}
	FMemory::Memcpy(Memory, Label.GetData(), SIZE_T(Label.Len()) * sizeof(TCHAR));
	static_cast<TCHAR*>(Memory)[Label.Len()] = TCHAR(0);
	Allocator.Counters().TextBytesCopied.fetch_add(int64(Request));
	if (Entry->Label)
		Allocator.Free(Entry->Label);
	Entry->LabelCredit.Reset();
	Entry->LabelCredit = MoveTemp(Credit.GetValue());
	Entry->Label = static_cast<TCHAR*>(Memory);
	Entry->Chars = Label.Len();
}
bool FStore::ApplyAttached(const FVaRestTraceSnapshot& Snapshot, const FCapturePolicy& Policy,
	TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator)
{
	check(IsInGameThread());
	if (!Storage || bEnumerating)
		return false;
	if (Pending->Lifetime != Budget->Lifetime || &Allocator != &Budget->Lifetime->Allocator)
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	RefreshLossWitness();
	if (CurrentReconciliationStamp == MAX_uint64)
	{
		ForEachRow([](FRow& Row) {
			if (!Row.Value.bSealed)
				Row.PresenceStamp = 0;
		});
		CurrentReconciliationStamp = 0;
	}
	++CurrentReconciliationStamp;
	for (const FVaRestTraceClient& Client : Snapshot.Clients)
		ApplyClient(Client.ClientId, Client.ScopeId, Client.bCreated, FStringView(Client.DisplayName));
	for (const FVaRestTraceStarted& Borrowed : Snapshot.InFlight)
		ApplyAttachedStarted(Borrowed, Policy, Pending, Allocator);
	ForEachRow([&](FRow& Row) {
		if (Row.Value.bTerminalKnown || Row.Value.bSealed || Row.Value.BirthMetadataOrdinal > LastConsumedMetadataOrdinal || Row.PresenceStamp == CurrentReconciliationStamp)
			return;
		if (Row.Value.bPinned)
			Increment(State.InterruptedRows);
		Row.Value.bPinned = false;
		Row.Value.bPartial = true;
		Row.Value.EndState = EVaRestInspectorEndState::CaptureInterrupted;
		Row.Evidence = ELivenessEvidence::None;
		if (Row.Value.RequestExcerptState == EVaRestInspectorDetailState::Pending)
			Row.Value.RequestExcerptState = EVaRestInspectorDetailState::CaptureStopped;
		for (int32 Dispatch = 0; Dispatch < Row.NumDispatches; ++Dispatch)
			if (Row.Dispatches[Dispatch]->ResponseExcerptState == EVaRestInspectorDetailState::Pending)
				Row.Dispatches[Dispatch]->ResponseExcerptState = EVaRestInspectorDetailState::CaptureStopped;
		CompleteMutation(Row);
	});
	return true;
}
FDispatchSlot* FStore::FindDispatch(FRow& Row, const FPacketScalars& Scalars) const
{
	// Attempt is a retry label; multiple physical dispatches can share it.
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
		if (Row.Dispatches[Index]->DispatchId == Scalars.DispatchId)
			return Row.Dispatches[Index].Get();
	return nullptr;
}
FDispatchSlot* FStore::AdmitDispatch(FRow& Row, const FPacketScalars& Scalars)
{
	if (Row.NumDispatches == UE_ARRAY_COUNT(Row.Dispatches))
		return nullptr;
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const auto Bound = Allocator.Bound(sizeof(FDispatchSlot), alignof(FDispatchSlot));
	if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
		return nullptr;
	if (!MakeAdmissionRoom({0, int64(Bound.GetValue()), {Row.IndexSlot, INDEX_NONE}}))
		return nullptr;
	auto Credit = FStoreCredit::Acquire(Budget, int64(Bound.GetValue()));
	if (!Credit.IsSet())
		return nullptr;
	void* Memory = Allocator.AllocateReserved(sizeof(FDispatchSlot), alignof(FDispatchSlot), Bound.GetValue());
	if (!Memory)
		return nullptr;
	auto* Slot = new (Memory) FDispatchSlot(MoveTemp(Credit.GetValue()));
	Slot->DispatchId = Scalars.DispatchId;
	Slot->Attempt = Scalars.Attempt;
	Row.Dispatches[Row.NumDispatches++].Reset(Slot);
	return Slot;
}
void FStore::ApplyPhysical(FRow& Row, FPacket& Packet)
{
	const FPacketFixed& Fixed = Packet.Fixed();
	const bool bDispatched = Fixed.Header.Kind == EVaRestTraceKind::AttemptDispatched;
	FDispatchSlot* Slot = FindDispatch(Row, Fixed.Scalars);
	if (Slot && (bDispatched ? Slot->bHasDispatched : Slot->bHasEnded))
		return;
	if (bDispatched)
	{
		Row.Value.DispatchHistory = EVaRestInspectorDispatchHistory::Dispatched;
		AddObservation(Row.Value.ObservedDispatches, 1, Row.Value.bCountersSaturated);
		Row.Value.CurrentAttempt = FMath::Max(Row.Value.CurrentAttempt, Fixed.Scalars.Attempt);
	}
	else
	{
		AddObservation(Row.Value.ObservedEnds, 1, Row.Value.bCountersSaturated);
		AddWire(Row.Value.WireBytesUp, Fixed.Scalars.BytesUp, Row.Value);
		AddWire(Row.Value.WireBytesDown, Fixed.Scalars.BytesDown, Row.Value);
	}
	if (!Slot)
		Slot = AdmitDispatch(Row, Fixed.Scalars);
	if (!Slot)
	{
		AddObservation(bDispatched ? Row.Value.OmittedDispatches : Row.Value.OmittedEnds, 1, Row.Value.bCountersSaturated);
		Row.Value.bMetadataOmitted = true;
		Increment(State.MetadataOmitted);
		if (bDispatched)
			Row.PreviousDispatchSlot = INDEX_NONE;
		return;
	}
	if (bDispatched)
	{
		// Copy current identity and raw scalars before either sharing an old
		// header backing or moving this packet into a new immutable owner.
		Slot->DispatchedFixed = ScalarCopy(Fixed);
		Slot->bHasDispatched = true;
		Slot->Attempt = Fixed.Scalars.Attempt;
		if (Fixed.Header.bMetadataOmitted)
			Slot->HeadersState = EVaRestInspectorHeadersState::MetadataOmitted;
		else if (!Packet.Headers().IsEmpty())
		{
			const FDispatchSlot* Previous = Row.PreviousDispatchSlot != INDEX_NONE ? Row.Dispatches[Row.PreviousDispatchSlot].Get() : nullptr;
			if (Previous && Previous->HeadersState == EVaRestInspectorHeadersState::Captured && Previous->Headers && Previous->Headers->Matches(Packet))
				Slot->Headers = Previous->Headers;
			else if (!TakeHeader(Row, Packet, Slot->Headers))
			{
				Slot->HeadersState = EVaRestInspectorHeadersState::MetadataOmitted;
				Slot->DispatchedFixed.Header.bMetadataOmitted = true;
				Row.Value.bMetadataOmitted = true;
				Increment(State.MetadataOmitted);
			}
			if (Slot->Headers)
				Slot->HeadersState = EVaRestInspectorHeadersState::Captured;
		}
		// Unknown/omitted observations are barriers, never evidence that a
		// previously captured set remains current.
		Row.PreviousDispatchSlot = INDEX_NONE;
		if (Slot->HeadersState == EVaRestInspectorHeadersState::Captured)
			for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
				if (Row.Dispatches[Index].Get() == Slot)
					Row.PreviousDispatchSlot = Index;
	}
	else
	{
		Slot->EndedFixed = ScalarCopy(Fixed);
		Slot->bHasEnded = true;
		if (!Slot->bHasDispatched)
			Slot->Attempt = Fixed.Scalars.Attempt;
		if (Fixed.Scalars.bExcerptPending && Slot->ResponseExcerptState == EVaRestInspectorDetailState::Absent)
			Slot->ResponseExcerptState = EVaRestInspectorDetailState::Pending;
		if (!TakePacket(Row, Packet, Slot->Ended))
		{
			Slot->EndedFixed.Header.bMetadataOmitted = true;
			Row.Value.bMetadataOmitted = true;
			Increment(State.MetadataOmitted);
		}
	}
}
FStore::EExcerptApplication FStore::ApplyExcerpt(FRow& Row, FPacket& Packet, EVaRestInspectorDetailState& OutDetail)
{
	OutDetail = EVaRestInspectorDetailState::Absent;
	if (Row.Value.bSealed)
		return EExcerptApplication::Ignored;
	const FPacketScalars Scalars = Packet.Fixed().Scalars;
	FStorePacketPtr* Destination = &Row.RequestExcerpt;
	EVaRestInspectorDetailState* Detail = &Row.Value.RequestExcerptState;
	if (Scalars.Owner == EVaRestTraceExcerptOwner::Response)
	{
		FDispatchSlot* Slot = FindDispatch(Row, Scalars);
		if (!Slot)
		{
			if (Row.Value.bTerminalKnown)
				return EExcerptApplication::Ignored;
			Slot = AdmitDispatch(Row, Scalars);
		}
		if (!Slot)
		{
			AddObservation(Row.Value.OmittedExcerpts, 1, Row.Value.bCountersSaturated);
			Row.Value.bMetadataOmitted = true;
			Increment(State.MetadataOmitted);
			OutDetail = EVaRestInspectorDetailState::MetadataOmitted;
			return EExcerptApplication::Refused;
		}
		Destination = &Slot->ResponseExcerpt;
		Detail = &Slot->ResponseExcerptState;
	}
	if (*Destination || (*Detail != EVaRestInspectorDetailState::Absent && *Detail != EVaRestInspectorDetailState::Pending) ||
		(Row.Value.bTerminalKnown && *Detail != EVaRestInspectorDetailState::Pending))
		return EExcerptApplication::Ignored;
	FStorePacketPtr Candidate;
	if (!TakePacket(Row, Packet, Candidate))
	{
		AddObservation(Row.Value.OmittedExcerpts, 1, Row.Value.bCountersSaturated);
		Row.Value.bMetadataOmitted = true;
		Increment(State.MetadataOmitted);
		OutDetail = EVaRestInspectorDetailState::MetadataOmitted;
		return EExcerptApplication::Refused;
	}
	*Destination = MoveTemp(Candidate);
	switch (Scalars.Skip)
	{
	case EVaRestExcerptSkip::None: *Detail = EVaRestInspectorDetailState::Present; break;
	case EVaRestExcerptSkip::Backlog: *Detail = EVaRestInspectorDetailState::Backlog; break;
	case EVaRestExcerptSkip::Disabled: *Detail = EVaRestInspectorDetailState::CaptureStopped; break;
	case EVaRestExcerptSkip::Discarded: *Detail = EVaRestInspectorDetailState::Discarded; break;
	}
	OutDetail = *Detail;
	return EExcerptApplication::Resolved;
}
void FStore::ApplyCompletion(FRow& Row, FPacket& Packet)
{
	check(!Row.Value.bActualCompletionObserved && !Row.Value.bTerminalKnown && !Row.Completed);
	Row.CompletionFixed = ScalarCopy(Packet.Fixed());
	const FPacketScalars& Scalars = Row.CompletionFixed.Scalars;
	Row.Value.bActualCompletionObserved = true;
	Row.Value.bTerminalKnown = true;
	Row.Value.bPinned = false;
	// Keep even a nonfinite receipt unknown. Only the first real completion
	// reaches this path; source absence/attachment never creates this evidence.
	Row.Value.CompletedObservedAt = Clock->Now();
	Row.Value.CompletedAt = Row.CompletionFixed.Header.Time;
	Row.Value.FinalStatus = Scalars.Status;
	Row.Value.FinalBodyBytes = Scalars.DeliveredBodyBytes;
	Row.Value.FinalStorage = Scalars.Storage;
	Row.Value.DecodeStartAt = Scalars.DecodeStartAt;
	Row.Value.DecodeEndAt = Scalars.DecodeEndAt;
	Row.Value.FinalPreviewState = Scalars.bFromCache ? EVaRestInspectorDetailState::NoProducer : EVaRestInspectorDetailState::Absent;
	// Completed has no final DispatchId. Physical Ended metadata and excerpts
	// remain in their own slots; neither status nor arrival proves selection.
	switch (Scalars.Outcome)
	{
	case EVaRestTraceOutcome::Success: Row.Value.EndState = EVaRestInspectorEndState::Success; break;
	case EVaRestTraceOutcome::Error: Row.Value.EndState = EVaRestInspectorEndState::Error; break;
	case EVaRestTraceOutcome::Queued: Row.Value.EndState = EVaRestInspectorEndState::Queued; break;
	case EVaRestTraceOutcome::Canceled: Row.Value.EndState = EVaRestInspectorEndState::Canceled; break;
	}
	Row.Value.CompletedMetadataState = Row.CompletionFixed.Header.bMetadataOmitted ?
		EVaRestInspectorDetailState::MetadataOmitted : EVaRestInspectorDetailState::Present;
	if (!TakePacket(Row, Packet, Row.Completed))
	{
		Row.Value.CompletedMetadataState = EVaRestInspectorDetailState::MetadataOmitted;
		Row.Value.bMetadataOmitted = true;
		Increment(State.MetadataOmitted);
	}
	// The fallback never contains slices, even when the admitted full owner
	// has text. Its metadata is always unavailable without that separate owner.
	Row.CompletionFixed.Header.bMetadataOmitted = true;
}
bool FStore::HasPendingExcerpt(const FRow& Row)
{
	if (Row.Value.RequestExcerptState == EVaRestInspectorDetailState::Pending)
		return true;
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
		if (Row.Dispatches[Index]->ResponseExcerptState == EVaRestInspectorDetailState::Pending)
			return true;
	return false;
}
bool FStore::CanSeal(const FRow& Row)
{
	return Row.Value.bTerminalKnown && !HasPendingExcerpt(Row);
}
bool FStore::ShouldShareRow(const FRow& Row)
{
	return Row.Value.bSealed;
}
void FStore::CompleteMutation(FRow& Row)
{
	if (Row.Value.bSealed)
		return;
	const bool bSeal = CanSeal(Row);
	if (bSeal)
	{
		// Every sealing route observes real loss before history becomes immutable.
		// Sticky dirty makes subsequent refreshes allocation-free constant work.
		RefreshLossWitness();
		RemoveDeadline(Row);
	}
	Touch(Row);
	if (bSeal)
		Row.Value.bSealed = true;
}
void FStore::AddDeadline(FRow& Row)
{
	if (Row.DeadlineIndex != INDEX_NONE)
		return;
	check(Storage && Row.IndexSlot >= 0 && Row.IndexSlot < Storage->IndexCapacity && Storage->NumDeadline < Storage->MaxRows);
	check(Storage->Index[Row.IndexSlot].Row.Get() == &Row);
	double Due = std::numeric_limits<double>::infinity();
	if (FMath::IsFinite(Row.Value.CompletedObservedAt))
	{
		const double Candidate = Row.Value.CompletedObservedAt + 5.0;
		if (FMath::IsFinite(Candidate))
			Due = Candidate;
	}
	int32 Position = Storage->NumDeadline++;
	Storage->DeadlineHeap[Position] = {Due, Row.IndexSlot};
	Row.DeadlineIndex = Position;
	auto SetPosition = [&](int32 Index) {
		FRow* Other = Storage->Index[Storage->DeadlineHeap[Index].IndexSlot].Row.Get();
		check(Other);
		Other->DeadlineIndex = Index;
	};
	while (Position > 0)
	{
		const int32 Parent = (Position - 1) / 2;
		if (!(Storage->DeadlineHeap[Position].Due < Storage->DeadlineHeap[Parent].Due))
			break;
		const auto Temporary = Storage->DeadlineHeap[Parent];
		Storage->DeadlineHeap[Parent] = Storage->DeadlineHeap[Position];
		Storage->DeadlineHeap[Position] = Temporary;
		SetPosition(Parent);
		SetPosition(Position);
		Position = Parent;
	}
}
void FStore::RemoveDeadline(FRow& Row)
{
	const int32 Removed = Row.DeadlineIndex;
	if (Removed == INDEX_NONE)
		return;
	check(Storage && Removed >= 0 && Removed < Storage->NumDeadline && Row.IndexSlot >= 0 && !Row.Value.bSealed);
	const int32 Last = Storage->NumDeadline - 1;
	Row.DeadlineIndex = INDEX_NONE;
	if (Removed != Last)
	{
		Storage->DeadlineHeap[Removed] = Storage->DeadlineHeap[Last];
		FRow* Moved = Storage->Index[Storage->DeadlineHeap[Removed].IndexSlot].Row.Get();
		check(Moved);
		Moved->DeadlineIndex = Removed;
	}
	Storage->NumDeadline = Last;
	if (Removed == Last)
		return;
	auto SetPosition = [&](int32 Index) {
		FRow* Other = Storage->Index[Storage->DeadlineHeap[Index].IndexSlot].Row.Get();
		check(Other);
		Other->DeadlineIndex = Index;
	};
	auto SwapEntries = [&](int32 Left, int32 Right) {
		const auto Temporary = Storage->DeadlineHeap[Left];
		Storage->DeadlineHeap[Left] = Storage->DeadlineHeap[Right];
		Storage->DeadlineHeap[Right] = Temporary;
		SetPosition(Left);
		SetPosition(Right);
	};
	int32 Position = Removed;
	while (Position > 0)
	{
		const int32 Parent = (Position - 1) / 2;
		if (!(Storage->DeadlineHeap[Position].Due < Storage->DeadlineHeap[Parent].Due))
			break;
		SwapEntries(Position, Parent);
		Position = Parent;
	}
	while (true)
	{
		const int32 Left = Position * 2 + 1;
		if (Left >= Storage->NumDeadline)
			break;
		const int32 Right = Left + 1;
		int32 Smallest = Left;
		if (Right < Storage->NumDeadline && Storage->DeadlineHeap[Right].Due < Storage->DeadlineHeap[Left].Due)
			Smallest = Right;
		if (!(Storage->DeadlineHeap[Smallest].Due < Storage->DeadlineHeap[Position].Due))
			break;
		SwapEntries(Position, Smallest);
		Position = Smallest;
	}
}
int32 FStore::ExpirePendingExcerpts()
{
	check(IsInGameThread());
	if (!Storage)
		return 0;
	if (bEnumerating)
	{
		bExpiryPending = true;
		return 0;
	}
	const double Now = Clock->Now();
	if (!FMath::IsFinite(Now))
		return 0;
	int32 Expired = 0;
	while (Storage->NumDeadline > 0 && Expired < 256)
	{
		const auto Next = Storage->DeadlineHeap[0];
		if (!FMath::IsFinite(Next.Due) || Now < Next.Due)
			break;
		FRow* Row = Storage->Index[Next.IndexSlot].Row.Get();
		check(Row && Row->DeadlineIndex == 0);
		RemoveDeadline(*Row);
		bool bChanged = false;
		if (Row->Value.RequestExcerptState == EVaRestInspectorDetailState::Pending)
		{
			Row->Value.RequestExcerptState = EVaRestInspectorDetailState::TimedOut;
			bChanged = true;
		}
		for (int32 Index = 0; Index < Row->NumDispatches; ++Index)
			if (Row->Dispatches[Index]->ResponseExcerptState == EVaRestInspectorDetailState::Pending)
			{
				Row->Dispatches[Index]->ResponseExcerptState = EVaRestInspectorDetailState::TimedOut;
				bChanged = true;
			}
		check(bChanged);
		CompleteMutation(*Row);
		++Expired;
	}
	return Expired;
}
void FStore::StopPendingExcerpts()
{
	check(IsInGameThread());
	if (bEnumerating)
	{
		bStopPending = true;
		return;
	}
	if (!Storage)
		return;
	ForEachRow([this](FRow& Row) {
		bool bChanged = false;
		if (Row.Value.RequestExcerptState == EVaRestInspectorDetailState::Pending)
		{
			Row.Value.RequestExcerptState = EVaRestInspectorDetailState::CaptureStopped;
			bChanged = true;
		}
		for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
			if (Row.Dispatches[Index]->ResponseExcerptState == EVaRestInspectorDetailState::Pending)
			{
				Row.Dispatches[Index]->ResponseExcerptState = EVaRestInspectorDetailState::CaptureStopped;
				bChanged = true;
			}
		if (bChanged)
			CompleteMutation(Row);
	});
}
int32 FStore::IndexCapacity() const
{
	return Storage ? Storage->IndexCapacity : 0;
}
int32 FStore::RetainedIdCapacity() const
{
	return Storage ? Storage->RetainedCapacity : 0;
}
bool FStore::EnsureNoteStorage(FRow& Row)
{
	if (Row.Notes.IsAllocated())
		return true;
	const FStoreBlockMeasure Measure = MeasureStoreBlock<FNoteEntry>(*Budget, FNormalizedLimits::NoteSlots);
	return Measure.bSupported && MakeAdmissionRoom({0, Measure.Bytes, {Row.IndexSlot, INDEX_NONE}}) && Row.Notes.Allocate(Budget, FNormalizedLimits::NoteSlots);
}
bool FStore::EnsureTimelineStorage(FRow& Row)
{
	if (Row.Timeline.IsAllocated())
		return true;
	const FStoreBlockMeasure Measure = MeasureStoreBlock<FTimelineSlot>(*Budget, FNormalizedLimits::TimelineSlots);
	return Measure.bSupported && MakeAdmissionRoom({0, Measure.Bytes, {Row.IndexSlot, INDEX_NONE}}) && Row.Timeline.Allocate(Budget, FNormalizedLimits::TimelineSlots);
}
void FStore::InvalidateNoteTimeline(FRow& Row, uint64 NoteSerial)
{
	check(NoteSerial != 0);
	for (int32 Index = 0; Index < Row.NumTimeline; ++Index)
	{
		auto& Slot = Row.Timeline[(Row.TimelineHead + Index) % FNormalizedLimits::TimelineSlots];
		if (Slot.NoteSerial == NoteSerial)
		{
			Slot.Value.DetailState = EVaRestInspectorDetailState::Evicted;
			Slot.NoteSerial = 0;
		}
	}
}
EVaRestInspectorDetailState FStore::ApplyNote(FRow& Row, FPacket& Packet, uint64& OutNoteSerial)
{
	OutNoteSerial = 0;
	const auto Omit = [&] {
		AddObservation(Row.Value.OmittedNotes, 1, Row.Value.bCountersSaturated);
		Row.Value.bMetadataOmitted = true;
		Increment(State.MetadataOmitted);
		return EVaRestInspectorDetailState::MetadataOmitted;
	};
	// Private association serials must never wrap into a still-retained slot.
	if (Row.NextNoteSerial == MAX_uint64)
	{
		Row.Value.bCountersSaturated = true;
		return Omit();
	}
	if (!EnsureNoteStorage(Row))
		return Omit();
	const bool bMetadataOmitted = Packet.Fixed().Header.bMetadataOmitted;
	FStorePacketPtr Candidate;
	// Pending, the candidate and every old note remain fully charged during
	// admission. A failed candidate cannot displace any existing history.
	if (!TakePacket(Row, Packet, Candidate))
		return Omit();
	int32 Destination = (Row.NoteHead + Row.NumNotes) % FNormalizedLimits::NoteSlots;
	if (Row.NumNotes == FNormalizedLimits::NoteSlots)
	{
		Destination = Row.NoteHead;
		auto& Old = Row.Notes[Destination];
		InvalidateNoteTimeline(Row, Old.Serial);
		Old.Owner.Reset(); // Packet/backing/wrapper free before their credit.
		Old.Serial = 0;
		Row.NoteHead = (Row.NoteHead + 1) % FNormalizedLimits::NoteSlots;
		AddObservation(Row.Value.OmittedNotes, 1, Row.Value.bCountersSaturated);
		Row.Value.bMetadataOmitted = true;
	}
	else
		++Row.NumNotes;
	auto& Entry = Row.Notes[Destination];
	check(!Entry.Owner);
	Entry.Owner = MoveTemp(Candidate);
	Entry.Serial = ++Row.NextNoteSerial;
	OutNoteSerial = Entry.Serial;
	return bMetadataOmitted ? EVaRestInspectorDetailState::MetadataOmitted : EVaRestInspectorDetailState::Present;
}
void FStore::AppendTimeline(FRow& Row, const FVaRestInspectorTimelineEntry& Entry, uint64 NoteSerial)
{
	if (!EnsureTimelineStorage(Row))
	{
		AddObservation(Row.Value.OmittedTimeline, 1, Row.Value.bCountersSaturated);
		Row.Value.bMetadataOmitted = true;
		Increment(State.MetadataOmitted);
		return;
	}
	int32 Destination = (Row.TimelineHead + Row.NumTimeline) % FNormalizedLimits::TimelineSlots;
	if (Row.NumTimeline == FNormalizedLimits::TimelineSlots)
	{
		Destination = Row.TimelineHead;
		Row.TimelineHead = (Row.TimelineHead + 1) % FNormalizedLimits::TimelineSlots;
		AddObservation(Row.Value.OmittedTimeline, 1, Row.Value.bCountersSaturated);
		Row.Value.bMetadataOmitted = true;
	}
	else
		++Row.NumTimeline;
	Row.Timeline[Destination].Value = Entry;
	Row.Timeline[Destination].NoteSerial = NoteSerial;
}
bool FStorePacketOwner::TryCompactStarted(const FStorePacketOwner& Source, uint64 ExpectedRequestId,
	TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator, FStorePacketPtr& Out)
{
	if (Out.IsValid() || !Budget || !Budget->Lifetime || &Budget->Lifetime->Allocator != &Allocator ||
		Source.Data.Allocator != &Allocator || Source.Data.Pending.IsValid() || !Source.Data.IsValid() ||
		Source.Data.Fixed().Header.Kind != EVaRestTraceKind::Started || Source.Data.Fixed().Header.Source != EVaRestTraceSource::Http ||
		Source.Data.Fixed().Scalars.RequestId != ExpectedRequestId || Source.Data.Fixed().Warnings.Count != 0 ||
		Source.Data.ExcerptOwner || Source.Data.Fixed().Headers.Count == 0)
	{
		return false;
	}
	int64 Cursor = int64(Align(sizeof(FPacketFixed), alignof(TCHAR)));
	for (int32 Index = 0; Index < static_cast<int32>(EText::Count); ++Index)
	{
		const FTextSlice Slice = Source.Data.Fixed().Text[Index];
		if (!Slice.Chars)
			continue;
		if (Slice.Offset % alignof(TCHAR) != 0 || uint64(Slice.Offset) + (uint64(Slice.Chars) + 1) * sizeof(TCHAR) > uint64(Source.Data.BackingRequest)
			|| !AddChecked(Cursor, (int64(Slice.Chars) + 1) * sizeof(TCHAR), FNormalizedLimits::MaxMetadataBytes))
		{
			return false;
		}
	}
	const auto BackingBound = Allocator.Bound(SIZE_T(Cursor), alignof(FPacketFixed));
	const auto WrapperBound = Allocator.Bound(sizeof(FStorePacketOwner), alignof(FStorePacketOwner));
	const auto OldCharge = Charge(Source.Data, Allocator);
	if (!BackingBound.IsSet() || !WrapperBound.IsSet() || !OldCharge.IsSet())
		return false;
	int64 NewCharge = int64(BackingBound.GetValue());
	if (!AddChecked(NewCharge, int64(WrapperBound.GetValue()), MAX_int64) || NewCharge >= OldCharge.GetValue())
		return false;
	auto Reservation = FStoreCredit::Acquire(Budget, NewCharge);
	if (!Reservation.IsSet())
		return false;
	void* WrapperMemory = Allocator.AllocateReserved(sizeof(FStorePacketOwner), alignof(FStorePacketOwner), WrapperBound.GetValue());
	if (!WrapperMemory)
		return false;
	FStorePacketPtr Candidate(new (WrapperMemory) FStorePacketOwner(MoveTemp(Reservation.GetValue()), FPacket(), Allocator));
	void* BackingMemory = Allocator.AllocateReserved(SIZE_T(Cursor), alignof(FPacketFixed), BackingBound.GetValue());
	if (!BackingMemory)
		return false;
	Candidate->Data.Allocator = &Allocator;
	Candidate->Data.Backing = BackingMemory;
	Candidate->Data.BackingCapacity = BackingBound.GetValue();
	Candidate->Data.BackingRequest = SIZE_T(Cursor);
	new (BackingMemory) FPacketFixed(Source.Data.Fixed());
	auto& Fixed = *static_cast<FPacketFixed*>(BackingMemory);
	Fixed.Headers = {};
	Fixed.Warnings = {};
	for (FTextSlice& Slice : Fixed.Text)
		Slice = {};
	int64 WriteCursor = int64(Align(sizeof(FPacketFixed), alignof(TCHAR)));
	for (int32 Index = 0; Index < static_cast<int32>(EText::Count); ++Index)
	{
		const FTextSlice SourceSlice = Source.Data.Fixed().Text[Index];
		if (!SourceSlice.Chars)
			continue;
		const uint8* SourceText = static_cast<const uint8*>(Source.Data.Backing) + SourceSlice.Offset;
		uint8* Destination = static_cast<uint8*>(BackingMemory) + WriteCursor;
		const SIZE_T Bytes = SIZE_T(SourceSlice.Chars) * sizeof(TCHAR);
		FMemory::Memcpy(Destination, SourceText, Bytes);
		*reinterpret_cast<TCHAR*>(Destination + Bytes) = TCHAR(0);
		Fixed.Text[Index] = {uint32(WriteCursor), SourceSlice.Chars};
		WriteCursor += int64(Bytes + sizeof(TCHAR));
		Allocator.Counters().TextBytesCopied.fetch_add(int64(Bytes + sizeof(TCHAR)));
	}
	check(WriteCursor == Cursor);
	Out = MoveTemp(Candidate);
	return true;
}
bool FStore::TrimPinnedDetails(FRow& Row)
{
	if (!Row.Value.bPinned || Row.Value.bSealed || Row.Value.DispatchHistory != EVaRestInspectorDispatchHistory::KnownNeverDispatched)
		return false;
	bool bChanged = false;
	if (Row.RequestExcerpt && Row.Value.RequestExcerptState != EVaRestInspectorDetailState::Pending)
	{
		Row.RequestExcerpt.Reset();
		Row.Value.RequestExcerptState = EVaRestInspectorDetailState::Evicted;
		bChanged = true;
	}
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
	{
		FDispatchSlot& Slot = *Row.Dispatches[Index];
		if (Slot.ResponseExcerpt && Slot.ResponseExcerptState != EVaRestInspectorDetailState::Pending)
		{
			Slot.ResponseExcerpt.Reset();
			Slot.ResponseExcerptState = EVaRestInspectorDetailState::Evicted;
			bChanged = true;
		}
	}
	if (Row.Started && Row.Value.StartedHeadersState == EVaRestInspectorHeadersState::Captured)
	{
		FStorePacketPtr Compacted;
		if (FStorePacketOwner::TryCompactStarted(*Row.Started, Row.Value.RequestId, Budget, Budget->Lifetime->Allocator, Compacted))
		{
			Row.Started = MoveTemp(Compacted);
			Row.Value.StartedHeadersState = EVaRestInspectorHeadersState::Evicted;
			bChanged = true;
		}
	}
	if (bChanged)
		Touch(Row);
	return bChanged;
}
bool FStore::ApplyMarker(FPacket& Packet)
{
	if (NextMarkerOrdinal == MAX_uint64 || Packet.Fixed().Header.Kind != EVaRestTraceKind::Marker)
	{
		Increment(State.OmittedMarkers);
		Increment(State.MetadataOmitted);
		return false;
	}
	const FRowKey Key(ERowKeyKind::CoreMarker, NextMarkerOrdinal + 1);
	FRow* Row = AdmitRow(Key, false);
	if (!Row)
	{
		Increment(State.OmittedMarkers);
		Increment(State.MetadataOmitted);
		return false;
	}
	FStorePacketPtr Owner;
	if (!TakePacket(*Row, Packet, Owner))
	{
		RemoveEligibleRowNow(Row->IndexSlot);
		Increment(State.OmittedMarkers);
		Increment(State.MetadataOmitted);
		return false;
	}
	const FPacketFixed& Fixed = Owner->Get().Fixed();
	Row->Value.ScopeId = Fixed.Scalars.ScopeId;
	Row->Value.ClientId = Fixed.Scalars.ClientId;
	Row->Value.bMetadataOmitted = Fixed.Header.bMetadataOmitted;
	Row->Marker = MoveTemp(Owner);
	Row->Value.bSealed = true;
	++NextMarkerOrdinal;
	Touch(*Row);
	return true;
}
bool FStore::Apply(FPacket&& Incoming)
{
	check(IsInGameThread());
	FPacket Packet = MoveTemp(Incoming);
	if (!Storage || bEnumerating || !Packet.IsValid())
		return false;
	const FPacketFixed& Fixed = Packet.Fixed();
	if (Fixed.Header.Generation != Generation)
		return false;
	if (Fixed.Header.Kind != EVaRestTraceKind::Excerpt && !Fixed.Header.bSnapshot)
		LastConsumedMetadataOrdinal = FMath::Max(LastConsumedMetadataOrdinal, Fixed.Header.MetadataOrdinal);
	if (Fixed.Header.Source != EVaRestTraceSource::Http)
		return false;
	RefreshLossWitness();
	if (Fixed.Header.Kind == EVaRestTraceKind::Marker)
		return ApplyMarker(Packet);
	if (Fixed.Header.Kind == EVaRestTraceKind::Client)
	{
		ApplyClient(Fixed.Scalars.ClientId, Fixed.Scalars.ScopeId, Fixed.Scalars.bCreated, Packet.Text(EText::ClientLabel));
		Increment(RevisionValue);
		return true;
	}
	if (Fixed.Header.Kind == EVaRestTraceKind::Scope)
		return true;
	const bool bStarted = Fixed.Header.Kind == EVaRestTraceKind::Started;
	const bool bCompleted = Fixed.Header.Kind == EVaRestTraceKind::Completed;
	const bool bExcerpt = Fixed.Header.Kind == EVaRestTraceKind::Excerpt;
	const bool bOrphanKind = Fixed.Header.Kind == EVaRestTraceKind::AttemptDispatched || Fixed.Header.Kind == EVaRestTraceKind::AttemptEnded ||
							 Fixed.Header.Kind == EVaRestTraceKind::Note || bExcerpt;
	if ((!bStarted && !bCompleted && !bOrphanKind) || Fixed.Scalars.RequestId == 0)
		return false;
	if (((Fixed.Header.Kind == EVaRestTraceKind::AttemptDispatched || Fixed.Header.Kind == EVaRestTraceKind::AttemptEnded) && Fixed.Scalars.DispatchId == 0) ||
		(bExcerpt && ((Fixed.Scalars.Owner == EVaRestTraceExcerptOwner::Response && Fixed.Scalars.DispatchId == 0) ||
			 (Fixed.Scalars.Owner == EVaRestTraceExcerptOwner::Request && Fixed.Scalars.DispatchId != 0) ||
			 (Fixed.Scalars.Owner != EVaRestTraceExcerptOwner::Request && Fixed.Scalars.Owner != EVaRestTraceExcerptOwner::Response))))
	{
		Increment(State.ContractViolations);
		return false;
	}
	if (bExcerpt && IsIdRetained(Fixed.Scalars.RequestId))
		return false;
	FRow* Row = Find(Fixed.Scalars.RequestId);
	if (Row && !bExcerpt && (Row->Value.bTerminalKnown || Row->Value.bSealed))
	{
		Increment(State.ContractViolations);
		return false;
	}
	const bool bNew = !Row;
	if (!Row)
		Row = AdmitRow(Fixed.Scalars.RequestId, !bStarted || Fixed.Header.bSnapshot);
	if (!Row)
		return false;
	if (bExcerpt)
	{
		EVaRestInspectorDetailState Detail = EVaRestInspectorDetailState::Absent;
		const EExcerptApplication Result = ApplyExcerpt(*Row, Packet, Detail);
		if (Result == EExcerptApplication::Ignored)
			return false;
		Row->Value.bMetadataOmitted |= Fixed.Header.bMetadataOmitted;
		if (!Row->Value.bTerminalKnown && (bNew || !Row->Value.bPinned))
		{
			Row->Value.bPartial = true;
			Row->Value.EndState = EVaRestInspectorEndState::CaptureInterrupted;
			Increment(OrphanEpoch);
		}
		FVaRestInspectorTimelineEntry Timeline;
		Timeline.Kind = Fixed.Header.Kind;
		Timeline.Seq = Fixed.Header.Seq;
		Timeline.Time = Fixed.Header.Time;
		Timeline.Attempt = Fixed.Scalars.Attempt;
		Timeline.DispatchId = Fixed.Scalars.DispatchId;
		Timeline.DetailState = Fixed.Header.bMetadataOmitted ? EVaRestInspectorDetailState::MetadataOmitted : Detail;
		AppendTimeline(*Row, Timeline);
		CompleteMutation(*Row);
		return Result == EExcerptApplication::Resolved;
	}
	// Queue-origin omission is already counted upstream. Preserve its row
	// truth without adding a second Store omission diagnostic.
	Row->Value.bMetadataOmitted |= Fixed.Header.bMetadataOmitted;
	// Snapshot identity and source timestamps before any packet can move into
	// an immutable owner. Receipt/application order is the ring order only.
	FVaRestInspectorTimelineEntry Timeline;
	Timeline.Kind = Fixed.Header.Kind;
	Timeline.Seq = Fixed.Header.Seq;
	Timeline.Time = Fixed.Header.Time;
	Timeline.Attempt = Fixed.Scalars.Attempt;
	Timeline.DispatchId = Fixed.Scalars.DispatchId;
	const FPacketScalars SourceScalars = Fixed.Scalars;
	const bool bSourceMetadataOmitted = Fixed.Header.bMetadataOmitted;
	uint64 NoteSerial = 0;
	if (bStarted)
	{
		SetStartedScalars(*Row, Fixed.Scalars);
		Row->Value.bPinned = true;
		Row->Value.EndState = EVaRestInspectorEndState::InFlight;
		Row->Evidence = Fixed.Header.bSnapshot ? ELivenessEvidence::Snapshot : ELivenessEvidence::Started;
		if (bNew && !Fixed.Header.bSnapshot && !EnumHasAnyFlags(Fixed.Scalars.Flags, EVaRestTraceRequestFlags::Partial) &&
			LossWitness && bLossWitnessBound && !bLossDirty && LossWitness->LossEpoch.load(std::memory_order_acquire) == GenerationLossEpoch)
		{
			Row->Value.DispatchHistory = EVaRestInspectorDispatchHistory::KnownNeverDispatched;
		}
		if (!Row->Started)
		{
			FStorePacketPtr Owner;
			if (TakePacket(*Row, Packet, Owner))
				AdoptAttachedPacket(Row->Value.RequestId, MoveTemp(Owner));
			else
			{
				Row->Value.bMetadataOmitted = true;
				Increment(State.MetadataOmitted);
			}
		}
		Timeline.DetailState = Row->Started && !Row->Started->Get().Fixed().Header.bMetadataOmitted ?
			EVaRestInspectorDetailState::Present : EVaRestInspectorDetailState::MetadataOmitted;
	}
	else if (bCompleted)
	{
		ApplyCompletion(*Row, Packet);
		Timeline.DetailState = Row->Value.CompletedMetadataState;
		if (HasPendingExcerpt(*Row))
			AddDeadline(*Row);
	}
	else if (bNew || !Row->Value.bPinned)
	{
		Row->Value.bPartial = true;
		Row->Value.EndState = EVaRestInspectorEndState::CaptureInterrupted;
		Increment(OrphanEpoch);
	}
	if (!bStarted && !bCompleted)
	{
		if (Timeline.Kind == EVaRestTraceKind::AttemptDispatched || Timeline.Kind == EVaRestTraceKind::AttemptEnded)
		{
			ApplyPhysical(*Row, Packet);
			const FDispatchSlot* Slot = FindDispatch(*Row, SourceScalars);
			const bool bAvailable = Slot && (Timeline.Kind == EVaRestTraceKind::AttemptDispatched ?
				!Slot->DispatchedFixed.Header.bMetadataOmitted && Slot->HeadersState != EVaRestInspectorHeadersState::MetadataOmitted :
				!Slot->EndedFixed.Header.bMetadataOmitted);
			Timeline.DetailState = bAvailable ? EVaRestInspectorDetailState::Present : EVaRestInspectorDetailState::MetadataOmitted;
		}
		else if (Timeline.Kind == EVaRestTraceKind::Note)
			Timeline.DetailState = ApplyNote(*Row, Packet, NoteSerial);
	}
	if (bSourceMetadataOmitted)
		Timeline.DetailState = EVaRestInspectorDetailState::MetadataOmitted;
	AppendTimeline(*Row, Timeline, NoteSerial);
	CompleteMutation(*Row);
	return true;
}
bool FStore::IsIdRetained(uint64 RequestId) const
{
	if (!Storage || !RequestId || Storage->RetainedLookupCapacity <= 0)
		return false;
	const uint32 Mask = uint32(Storage->RetainedLookupCapacity - 1);
	const uint32 Start = uint32(RequestId ^ (RequestId >> 32)) * 0x9e3779b9u;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Entry = Storage->RetainedLookup[int32((Start + Offset) & Mask)];
		if (Entry.State == FStorage::EIndexState::Empty)
			return false;
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.RequestId == RequestId)
			return Entry.RingSlot >= 0 && Entry.RingSlot < Storage->RetainedCapacity &&
				Storage->RetainedIds[Entry.RingSlot].RequestId == RequestId;
	}
	return false;
}
void FStore::RetainRemovedId(uint64 RequestId)
{
	check(Storage && RequestId && Storage->RetainedCapacity > 0 && Storage->RetainedLookupCapacity > 0);
	if (IsIdRetained(RequestId))
		return;
	int32 RingSlot = INDEX_NONE;
	if (Storage->NumRetained == Storage->RetainedCapacity)
	{
		RingSlot = Storage->RetainedHead;
		auto& Retained = Storage->RetainedIds[RingSlot];
		const int32 OldLookup = Retained.LookupSlot;
		check(OldLookup >= 0 && OldLookup < Storage->RetainedLookupCapacity);
		auto& OldEntry = Storage->RetainedLookup[OldLookup];
		check(OldEntry.State == FStorage::EIndexState::Occupied && OldEntry.RequestId == Retained.RequestId && OldEntry.RingSlot == RingSlot);
		OldEntry.State = FStorage::EIndexState::Deleted;
		OldEntry.RequestId = 0;
		OldEntry.RingSlot = INDEX_NONE;
		Storage->RetainedHead = (Storage->RetainedHead + 1) % Storage->RetainedCapacity;
	}
	else
	{
		RingSlot = (Storage->RetainedHead + Storage->NumRetained) % Storage->RetainedCapacity;
		++Storage->NumRetained;
	}
	const uint32 Mask = uint32(Storage->RetainedLookupCapacity - 1);
	const uint32 Start = uint32(RequestId ^ (RequestId >> 32)) * 0x9e3779b9u;
	int32 LookupSlot = INDEX_NONE;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		auto& Entry = Storage->RetainedLookup[Candidate];
		if (Entry.State == FStorage::EIndexState::Occupied)
		{
			if (Entry.RequestId == RequestId)
			{
				LookupSlot = Candidate;
				break;
			}
			continue;
		}
		if (Entry.State == FStorage::EIndexState::Deleted)
		{
			if (LookupSlot == INDEX_NONE)
				LookupSlot = Candidate;
			continue;
		}
		if (LookupSlot == INDEX_NONE)
			LookupSlot = Candidate;
		break;
	}
	check(LookupSlot != INDEX_NONE);
	auto& Lookup = Storage->RetainedLookup[LookupSlot];
	Lookup.State = FStorage::EIndexState::Occupied;
	Lookup.RequestId = RequestId;
	Lookup.RingSlot = RingSlot;
	Storage->RetainedIds[RingSlot].RequestId = RequestId;
	Storage->RetainedIds[RingSlot].LookupSlot = LookupSlot;
}
void FStore::ResetRetainedIds()
{
	if (!Storage)
		return;
	for (int32 Index = 0; Index < Storage->RetainedCapacity; ++Index)
		Storage->RetainedIds[Index] = {};
	for (int32 Index = 0; Index < Storage->RetainedLookupCapacity; ++Index)
		Storage->RetainedLookup[Index] = {};
	Storage->RetainedHead = 0;
	Storage->NumRetained = 0;
}
bool FStore::RemoveEligibleRow(uint64 RequestId)
{
	check(IsInGameThread());
	if (!Storage || !RequestId)
		return false;
	const uint32 Mask = uint32(Storage->IndexCapacity - 1);
	const uint32 Start = HashRequestId(RequestId);
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Slot = int32((Start + Offset) & Mask);
		auto& Entry = Storage->Index[Slot];
		if (Entry.State == FStorage::EIndexState::Empty)
			return false;
		if (Entry.State != FStorage::EIndexState::Occupied || Entry.Key.Kind != ERowKeyKind::Http || Entry.Key.Id != RequestId)
			continue;
		if (!Entry.Row || !CanReleaseWhole(*Entry.Row))
			return false;
		if (bEnumerating)
		{
			Entry.bRemovePending = true;
			return true;
		}
		return RemoveEligibleRowNow(Slot);
	}
	return false;
}
bool FStore::RemoveEligibleRowNow(int32 IndexSlot)
{
	check(Storage && IndexSlot >= 0 && IndexSlot < Storage->IndexCapacity && !bEnumerating);
	auto& Entry = Storage->Index[IndexSlot];
	if (Entry.State != FStorage::EIndexState::Occupied || !Entry.Row || !CanReleaseWhole(*Entry.Row))
		return false;
	FRow& Row = *Entry.Row;
	RemoveDeadline(Row);
	UnlinkArrival(Row);
	if (Row.Key.Kind == ERowKeyKind::Http && Row.Key.Id != 0)
		RetainRemovedId(Row.Key.Id);
	Entry.bRemovePending = false;
	Entry.Row.Reset();
	Entry.Key = FRowKey();
	Entry.State = FStorage::EIndexState::Deleted;
	Increment(RevisionValue);
	return true;
}
bool FStore::BindLossWitness(TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending)
{
	check(IsInGameThread());
	if (!Storage || !Budget || bEnumerating || !Pending->Lifetime || Pending->Lifetime != Budget->Lifetime)
		return false;
	if (LossWitness && LossWitness.Get() != &Pending.Get())
		return false;
	const auto OriginalPending = OriginatingPending.Pin();
	if (OriginalPending && OriginalPending.Get() != &Pending.Get())
		return false;
	if (!OriginalPending && bOriginatingPendingBound)
		return false;
	if (LossWitness.Get() == &Pending.Get() && bLossWitnessBound)
		return true;
	InvalidateKnownNeverHistory();
	LossWitness = Pending;
	OriginatingPending = Pending;
	bOriginatingPendingBound = true;
	GenerationLossEpoch = 0;
	bLossWitnessBound = false;
	bLossDirty = true;
	return true;
}
void FStore::RefreshLossWitness() const
{
	check(IsInGameThread());
	if (bEnumerating)
		return;
	if (!LossWitness)
	{
		bLossWitnessBound = false;
		bLossDirty = true;
		return;
	}
	const uint64 Current = LossWitness->LossEpoch.load(std::memory_order_acquire);
	if (Current == MAX_uint64 || !bLossWitnessBound || Current != GenerationLossEpoch)
	{
		if (!bLossDirty)
			InvalidateKnownNeverHistory();
		bLossDirty = true;
	}
}
void FStore::InvalidateKnownNeverHistory() const
{
	check(IsInGameThread() && !bEnumerating);
	ForEachRow([this](FRow& Row) {
		if (!Row.Value.bSealed && Row.Value.DispatchHistory == EVaRestInspectorDispatchHistory::KnownNeverDispatched)
		{
			Row.Value.DispatchHistory = EVaRestInspectorDispatchHistory::Unknown;
			Touch(Row);
		}
	});
}
void FStore::FlushDeferredRemovals()
{
	if (!Storage)
		return;
	for (int32 Index = 0; Index < Storage->IndexCapacity; ++Index)
	{
		auto& Entry = Storage->Index[Index];
		if (!Entry.bRemovePending)
			continue;
		Entry.bRemovePending = false;
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.Row)
			RemoveEligibleRowNow(Index);
	}
}
void FStore::FlushDeferred()
{
	if (bEnumerating)
		return;
	RefreshLossWitness();
	if (bGenerationPending)
	{
		const uint64 NewGeneration = DeferredGeneration;
		const uint64 ConsumedOrdinal = DeferredConsumedOrdinal;
		bGenerationPending = false;
		BeginGeneration(NewGeneration, ConsumedOrdinal);
	}
	if (bClearPending)
	{
		bClearPending = false;
		bStopPending = false;
		bExpiryPending = false;
		Clear();
		return;
	}
	if (bStopPending)
	{
		bStopPending = false;
		StopPendingExcerpts();
	}
	if (bExpiryPending)
	{
		bExpiryPending = false;
		ExpirePendingExcerpts();
	}
	FlushDeferredRemovals();
}
void FStore::BeginGeneration(uint64 InGeneration, uint64 ConsumedOrdinal)
{
	check(IsInGameThread());
	if (bEnumerating)
	{
		DeferredGeneration = InGeneration;
		DeferredConsumedOrdinal = ConsumedOrdinal;
		bGenerationPending = true;
		return;
	}
	InvalidateKnownNeverHistory();
	GenerationLossEpoch = LossWitness ? LossWitness->LossEpoch.load(std::memory_order_acquire) : 0;
	bLossWitnessBound = LossWitness.IsValid();
	bLossDirty = !bLossWitnessBound || GenerationLossEpoch == MAX_uint64;
	Generation = InGeneration;
	LastConsumedMetadataOrdinal = ConsumedOrdinal;
	OrphanEpoch = 0;
	CurrentReconciliationStamp = 0;
	if (!Storage)
		return;
	ForEachRow([&](FRow& Row) {
		if (Row.Value.bTerminalKnown || Row.Value.bSealed)
			return;
		Row.Value.BirthMetadataOrdinal = ConsumedOrdinal;
		Row.PresenceStamp = 0;
	});
}
void FStore::Clear()
{
	check(IsInGameThread());
	if (bEnumerating)
	{
		bClearPending = true;
		return;
	}
	bStopPending = false;
	bExpiryPending = false;
	if (!Storage)
	{
		if (State.bPendingLimits)
			ReleaseFixedStorage();
		return;
	}
	while (Storage->NumDeadline > 0)
	{
		FRow* Row = Storage->Index[Storage->DeadlineHeap[0].IndexSlot].Row.Get();
		check(Row);
		RemoveDeadline(*Row);
	}
	for (int32 Index = 0; Index < Storage->IndexCapacity; ++Index)
	{
		Storage->Index[Index].bRemovePending = false;
		Storage->Index[Index].Row.Reset();
		Storage->Index[Index].Key = FRowKey();
		Storage->Index[Index].State = FStorage::EIndexState::Empty;
	}
	ResetArrival();
	for (int32 Index = 0; Index < Storage->NumClients; ++Index)
		Storage->Clients[Index].Reset();
	for (int32 Index = 0; Index < Storage->Plan.ClientLookupCapacity; ++Index)
		Storage->ClientLookup[Index] = FClientLookupEntry{};
	Storage->NumClients = 0;
	ClearStats();
	ClearLinks();
	CurrentReconciliationStamp = 0;
	OrphanEpoch = 0;
	ResetRetainedIds();
	Increment(RevisionValue);
	if (State.bPendingLimits)
		ReleaseFixedStorage();
}
void FStore::RequestLimits(const FNormalizedLimits& Requested)
{
	check(IsInGameThread());
	RequestedLimits = Requested;
	State.RequestedStoreLimit = Requested.StoreBytes;
	State.bPendingLimits = State.bPendingLimits || Requested.StoreBytes != Limits.StoreBytes ||
		Requested.PendingBytes != Limits.PendingBytes || Requested.PendingEvents != Limits.PendingEvents || Requested.MaxRecords != Limits.MaxRecords;
}
void FStore::ReleaseFixedStorage()
{
	check(IsInGameThread() && !bEnumerating);
	if (Budget)
	{
		State.StoreBytes = Budget->UsedBytes.load(std::memory_order_acquire);
		State.StorePeakBytes = Budget->PeakBytes.load(std::memory_order_acquire);
		State.SnapshotControllers = int32(Budget->SnapshotControllers.load(std::memory_order_acquire));
	}
	Storage.Reset();
	LossWitness.Reset();
	GenerationLossEpoch = 0;
	bLossWitnessBound = false;
	bLossDirty = true;
	Budget.SafeRelease();
}
bool FStore::CanRecreateAccount(const FBudgetAccount& OldBudget, const FPendingAccount& OldPending) const
{
	check(IsInGameThread());
	const auto OriginalPending = OriginatingPending.Pin();
	return OriginatingAccountIdentity != 0 && OldBudget.Identity == OriginatingAccountIdentity &&
		OriginalPending.Get() == &OldPending &&
		!bEnumerating && !bClearPending && !bGenerationPending && !bStopPending && !bExpiryPending && !Storage && !Budget &&
		OldBudget.ControlLeases.load(std::memory_order_acquire) == 1 && OldBudget.Lifetime && OldPending.Lifetime == OldBudget.Lifetime &&
		OldBudget.UsedBytes.load(std::memory_order_acquire) == OldBudget.BootstrapBytes &&
		OldBudget.SnapshotControllers.load(std::memory_order_acquire) == 0 && OldBudget.Probe.LiveControllers.load(std::memory_order_acquire) == 0 &&
		OldBudget.Lifetime->Probe.LiveBacking.load(std::memory_order_acquire) == 0 &&
		OldPending.Count.load(std::memory_order_acquire) == 0 && OldPending.Bytes.load(std::memory_order_acquire) == 0 &&
		OldPending.ExcerptReservations.load(std::memory_order_acquire) == 0;
}
bool FStore::CompleteRecreation(const FNormalizedLimits& Effective,
	const FBudgetAccount& OldBudget, const FPendingAccount& OldPending,
	TRefCountPtr<FBudgetAccount> NewBudget, TSharedRef<FPendingAccount, ESPMode::ThreadSafe> NewPending)
{
	check(IsInGameThread());
	if (!CanRecreateAccount(OldBudget, OldPending) || !NewBudget || !NewBudget->Lifetime || NewBudget->Lifetime != OldBudget.Lifetime ||
		NewBudget->Identity == 0 || NewBudget->Identity == OldBudget.Identity || &NewPending.Get() == &OldPending ||
		NewPending->Lifetime != OldBudget.Lifetime || Effective.StoreBytes != RequestedLimits.StoreBytes ||
		Effective.PendingBytes != RequestedLimits.PendingBytes || Effective.PendingEvents != RequestedLimits.PendingEvents ||
		Effective.MaxRecords != RequestedLimits.MaxRecords || NewBudget->Limit != Effective.StoreBytes ||
		NewPending->ByteLimit != Effective.PendingBytes || NewPending->CountLimit != Effective.PendingEvents ||
		NewBudget->UsedBytes.load(std::memory_order_acquire) != NewBudget->BootstrapBytes ||
		NewBudget->SnapshotControllers.load(std::memory_order_acquire) != 0 || NewBudget->Probe.LiveControllers.load(std::memory_order_acquire) != 0 ||
		NewPending->Count.load(std::memory_order_acquire) != 0 || NewPending->Bytes.load(std::memory_order_acquire) != 0 ||
		NewPending->ExcerptReservations.load(std::memory_order_acquire) != 0)
	{
		return false;
	}
	NewBudget->InheritLifetime(OldBudget);
	CarryPendingLifetime(OldPending, *NewPending);
	const FNormalizedLimits Previous = Limits;
	Limits = Effective;
	if (!InitializeStorage(NewBudget))
	{
		Limits = Previous;
		State.EffectiveStoreLimit = Previous.StoreBytes;
		State.bPendingLimits = true;
		Storage.Reset();
		Budget.SafeRelease();
		return false;
	}
	OriginatingAccountIdentity = NewBudget->Identity;
	OriginatingPending = NewPending;
	bOriginatingPendingBound = true;
	RequestedLimits = Effective;
	State.EffectiveStoreLimit = Effective.StoreBytes;
	State.RequestedStoreLimit = Effective.StoreBytes;
	State.bPendingLimits = false;
	return true;
}
uint64 FStore::Revision() const
{
	check(IsInGameThread());
	return RevisionValue;
}
FStringView FStore::ClientLabel(uint64 ClientId) const
{
	if (!Storage || ClientId == 0)
		return {};
	const uint32 Mask = uint32(Storage->Plan.ClientLookupCapacity - 1);
	const uint32 Start = HashRequestId(ClientId);
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Lookup = Storage->ClientLookup[int32((Start + Offset) & Mask)];
		if (Lookup.State == FStorage::EIndexState::Empty)
			return {};
		if (Lookup.State == FStorage::EIndexState::Occupied && Lookup.ClientId == ClientId)
		{
			const FClientEntry& Client = Storage->Clients[Lookup.ClientSlot];
			return Client.Label ? FStringView(Client.Label, Client.Chars) : FStringView();
		}
	}
	return {};
}
bool FStore::CopyTextAdmitted(FStringView Source, FTicketedText& Out)
{
	if (Out.Data || Out.Credit.GetAccount())
		return false;
	if (Source.IsEmpty())
		return true;
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const SIZE_T Request = (SIZE_T(Source.Len()) + 1) * sizeof(TCHAR);
	const auto Bound = Allocator.Bound(Request, alignof(TCHAR));
	if (!Bound.IsSet())
		return false;
	auto Credit = FStoreCredit::Acquire(Budget, int64(Bound.GetValue()));
	if (!Credit.IsSet())
		return false;
	void* Memory = Allocator.AllocateReserved(Request, alignof(TCHAR), Bound.GetValue());
	if (!Memory)
		return false;
	FMemory::Memcpy(Memory, Source.GetData(), SIZE_T(Source.Len()) * sizeof(TCHAR));
	static_cast<TCHAR*>(Memory)[Source.Len()] = TCHAR(0);
	Allocator.Counters().TextBytesCopied.fetch_add(int64(Request));
	Out.Credit = MoveTemp(Credit.GetValue());
	Out.Data = static_cast<const TCHAR*>(Memory);
	Out.Chars = Source.Len();
	return true;
}
bool FStore::AdmitText(FStringView Source, const FAdmissionAnchors& Anchors, FTicketedText& Out)
{
	if (Out.Data || Out.Credit.GetAccount() || Source.Len() < 0 || Source.Len() > FNormalizedLimits::MaxStringChars ||
		(Source.Len() > 0 && !Source.GetData()) || !ValidateBorrowedSource(Source, Anchors))
	{
		return false;
	}
	if (Source.IsEmpty())
		return true;
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const SIZE_T Request = (SIZE_T(Source.Len()) + 1) * sizeof(TCHAR);
	const auto Bound = Allocator.Bound(Request, alignof(TCHAR));
	return Bound.IsSet() && MakeAdmissionRoom({0, int64(Bound.GetValue()), Anchors}) && CopyTextAdmitted(Source, Out);
}
bool FStore::ValidateBorrowedSource(FStringView Source, const FAdmissionAnchors& Anchors) const
{
	if (Source.IsEmpty() || Anchors.BorrowedSourceIndexSlot == INDEX_NONE)
		return true;
	if (!Storage || Anchors.BorrowedSourceIndexSlot < 0 || Anchors.BorrowedSourceIndexSlot >= Storage->IndexCapacity)
		return false;
	const auto& Entry = Storage->Index[Anchors.BorrowedSourceIndexSlot];
	if (Entry.State != FStorage::EIndexState::Occupied || !Entry.Row)
		return false;
	const auto Contains = [&Source](const FVaRestInspectorPacketView& View) {
		if (!View.Fixed || Source.IsEmpty())
			return false;
		const uint8* Begin = View.Backing.GetData();
		const uint8* End = Begin + View.Backing.Num();
		const uint8* TextBegin = reinterpret_cast<const uint8*>(Source.GetData());
		const uint8* TextEnd = TextBegin + SIZE_T(Source.Len()) * sizeof(TCHAR);
		return TextBegin >= Begin && TextEnd <= End;
	};
	const auto& Row = *Entry.Row;
	if ((Row.Started && Contains(Row.Started->View())) || (Row.Completed && Contains(Row.Completed->View())) ||
		(Row.Marker && Contains(Row.Marker->View())) || (Row.RequestExcerpt && Contains(Row.RequestExcerpt->View())))
		return true;
	if (Row.Notes.IsAllocated())
		for (int32 Index = 0; Index < FNormalizedLimits::NoteSlots; ++Index)
			if (Row.Notes[Index].Owner && Contains(Row.Notes[Index].Owner->View()))
				return true;
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
	{
		const auto& Slot = *Row.Dispatches[Index];
		if ((Slot.Ended && Contains(Slot.Ended->View())) || (Slot.ResponseExcerpt && Contains(Slot.ResponseExcerpt->View())) ||
			(Slot.Headers && Contains(Slot.Headers->View())))
			return true;
	}
	return false;
}
const FStatsGroup* FStore::FindStatsGroup(const FStatsKeyView& Key) const
{
	if (!Storage || Storage->Plan.StatsIndexCapacity <= 0 || Key.Key.Len() < 0 || Key.Key.Len() > FNormalizedLimits::MaxStringChars)
		return nullptr;
	const uint32 Mask = uint32(Storage->Plan.StatsIndexCapacity - 1);
	const uint32 Start = HashStatsKey(Key);
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Entry = Storage->StatsIndex[int32((Start + Offset) & Mask)];
		if (Entry.State == FStorage::EIndexState::Empty)
			return nullptr;
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.Group && Entry.Group->Value.ScopeId == Key.ScopeId &&
			Entry.Group->Value.ClientId == Key.ClientId && Entry.Group->Value.Endpoint == Key.Endpoint && Entry.Group->Key.View() == Key.Key)
		{
			return &Entry.Group->Value;
		}
	}
	return nullptr;
}
FStatsGroup* FStore::FindOrAdmitOverflowStatsGroup()
{
	return Storage && Storage->OverflowStats ? &Storage->OverflowStats->Value : nullptr;
}
FStatsGroup* FStore::FindOrAdmitStatsGroup(const FStatsKeyView& Key, const FAdmissionAnchors& Anchors)
{
	FStatsGroup* Overflow = FindOrAdmitOverflowStatsGroup();
	if (!Storage || Key.Key.Len() < 0 || Key.Key.Len() > FNormalizedLimits::MaxStringChars || (Key.Key.Len() > 0 && !Key.Key.GetData()) || !ValidateBorrowedSource(Key.Key, Anchors))
	{
		Increment(State.OmittedStatsGroups);
		Increment(State.MetadataOmitted);
		return Overflow;
	}
	if (const FStatsGroup* Existing = FindStatsGroup(Key))
		return const_cast<FStatsGroup*>(Existing);
	const int32 NamedCapacity = Storage->Plan.StatsGroupCapacity - 1;
	int32 ClientGroups = 0;
	for (int32 Index = 0; Index < Storage->Plan.StatsIndexCapacity; ++Index)
		if (Storage->StatsIndex[Index].Group && Storage->StatsIndex[Index].Group->Value.ClientId == Key.ClientId)
			++ClientGroups;
	if (Storage->NumStatsGroups - 1 >= NamedCapacity || ClientGroups >= FMath::Min(FNormalizedLimits::KeysPerClient, NamedCapacity))
	{
		Increment(State.OmittedStatsGroups);
		Increment(State.MetadataOmitted);
		return Overflow;
	}
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const auto ObjectBound = Allocator.Bound(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot));
	const SIZE_T TextRequest = (SIZE_T(Key.Key.Len()) + 1) * sizeof(TCHAR);
	const auto TextBound = Key.Key.IsEmpty() ? TOptional<SIZE_T>(0) : Allocator.Bound(TextRequest, alignof(TCHAR));
	if (!ObjectBound.IsSet() || !TextBound.IsSet())
	{
		Increment(State.UnsupportedAllocator);
		Increment(State.OmittedStatsGroups);
		return Overflow;
	}
	int64 Needed = int64(ObjectBound.GetValue());
	if (!AddChecked(Needed, int64(TextBound.GetValue()), MAX_int64) || !MakeAdmissionRoom({0, Needed, Anchors}))
	{
		Increment(State.OmittedStatsGroups);
		Increment(State.MetadataOmitted);
		return Overflow;
	}
	auto Credit = FStoreCredit::Acquire(Budget, int64(ObjectBound.GetValue()));
	if (!Credit.IsSet())
	{
		Increment(State.OmittedStatsGroups);
		Increment(State.MetadataOmitted);
		return Overflow;
	}
	void* Memory = Allocator.AllocateReserved(sizeof(FStatsGroupSlot), alignof(FStatsGroupSlot), ObjectBound.GetValue());
	if (!Memory)
	{
		Increment(State.UnsupportedAllocator);
		Increment(State.OmittedStatsGroups);
		return Overflow;
	}
	FStatsGroupSlotPtr Slot(new (Memory) FStatsGroupSlot());
	Slot->ObjectCredit = MoveTemp(Credit.GetValue());
	Slot->Value.ScopeId = Key.ScopeId;
	Slot->Value.ClientId = Key.ClientId;
	Slot->Value.Endpoint = Key.Endpoint;
	if (!CopyTextAdmitted(Key.Key, Slot->Key))
	{
		Increment(State.OmittedStatsGroups);
		Increment(State.MetadataOmitted);
		return Overflow;
	}
	const uint32 Mask = uint32(Storage->Plan.StatsIndexCapacity - 1);
	const uint32 Start = HashStatsKey(Key);
	int32 IndexSlot = INDEX_NONE;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		const auto& Entry = Storage->StatsIndex[Candidate];
		if (Entry.State != FStorage::EIndexState::Occupied)
		{
			IndexSlot = Candidate;
			break;
		}
	}
	if (IndexSlot == INDEX_NONE)
	{
		Increment(State.OmittedStatsGroups);
		Increment(State.MetadataOmitted);
		return Overflow;
	}
	Slot->IndexSlot = IndexSlot;
	Storage->StatsIndex[IndexSlot].State = FStorage::EIndexState::Occupied;
	Storage->StatsIndex[IndexSlot].Group = MoveTemp(Slot);
	++Storage->NumStatsGroups;
	return &Storage->StatsIndex[IndexSlot].Group->Value;
}
FStore::FLinkTargetEntry* FStore::FindLinkTarget(uint64 RequestId, bool bCreate)
{
	if (!Storage || Storage->Plan.LinkTargetCapacity <= 0)
		return nullptr;
	const uint32 Mask = uint32(Storage->Plan.LinkTargetCapacity - 1);
	const uint32 Start = HashRequestId(RequestId);
	int32 FirstDeleted = INDEX_NONE;
	const auto InitializeTarget = [&](int32 Slot) -> FLinkTargetEntry* {
		auto& NewTarget = Storage->LinkTargets[Slot];
		NewTarget.State = FStorage::EIndexState::Occupied;
		NewTarget.ToRequestId = RequestId;
		NewTarget.HeadSlot = NewTarget.TailSlot = INDEX_NONE;
		NewTarget.Count = 0;
		NewTarget.IndexSlot = Slot;
		return &NewTarget;
	};
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		auto& Target = Storage->LinkTargets[Candidate];
		if (Target.State == FStorage::EIndexState::Empty)
		{
			if (!bCreate)
				return nullptr;
			return InitializeTarget(FirstDeleted != INDEX_NONE ? FirstDeleted : Candidate);
		}
		if (Target.State == FStorage::EIndexState::Deleted)
		{
			if (FirstDeleted == INDEX_NONE)
				FirstDeleted = Candidate;
			continue;
		}
		if (Target.ToRequestId == RequestId)
			return &Target;
	}
	return bCreate && FirstDeleted != INDEX_NONE ? InitializeTarget(FirstDeleted) : nullptr;
}
void FStore::RemoveLinkAt(int32 LinkSlot)
{
	check(Storage && LinkSlot >= 0 && LinkSlot < Storage->Plan.LinkCapacity);
	auto& Entry = Storage->Links[LinkSlot];
	check(Entry.bOccupied);
	auto& Forward = Storage->LinkForward[Entry.ForwardIndexSlot];
	Forward.State = FStorage::EIndexState::Deleted;
	Forward.LinkSlot = INDEX_NONE;
	if (Entry.TargetIndexSlot != INDEX_NONE)
	{
		auto& Target = Storage->LinkTargets[Entry.TargetIndexSlot];
		check(Target.ToRequestId == Entry.ToRequestId && Target.Count > 0);
		if (Entry.PrevForTarget != INDEX_NONE)
			Storage->Links[Entry.PrevForTarget].NextForTarget = Entry.NextForTarget;
		else
			Target.HeadSlot = Entry.NextForTarget;
		if (Entry.NextForTarget != INDEX_NONE)
			Storage->Links[Entry.NextForTarget].PrevForTarget = Entry.PrevForTarget;
		else
			Target.TailSlot = Entry.PrevForTarget;
		--Target.Count;
		if (Target.Count == 0)
		{
			Target.State = FStorage::EIndexState::Deleted;
			Target.ToRequestId = 0;
			Target.HeadSlot = Target.TailSlot = INDEX_NONE;
		}
	}
	if (Entry.PrevFifo != INDEX_NONE)
		Storage->Links[Entry.PrevFifo].NextFifo = Entry.NextFifo;
	else
		Storage->LinkHead = Entry.NextFifo;
	if (Entry.NextFifo != INDEX_NONE)
		Storage->Links[Entry.NextFifo].PrevFifo = Entry.PrevFifo;
	else
		Storage->LinkTail = Entry.PrevFifo;
	Entry.QueueId.Reset();
	const int32 PreviousFree = Storage->LinkFreeHead;
	Entry = FLinkEntry{};
	Entry.NextFree = PreviousFree;
	Storage->LinkFreeHead = LinkSlot;
	--Storage->NumLinks;
	Increment(RevisionValue);
}
bool FStore::HasLink(const FLinkSeed& Seed) const
{
	if (!Storage)
		return false;
	const uint32 Mask = uint32(Storage->Plan.LinkIndexCapacity - 1);
	const uint32 Start = HashLinkSeed(Seed);
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Entry = Storage->LinkForward[int32((Start + Offset) & Mask)];
		if (Entry.State == FStorage::EIndexState::Empty)
			return false;
		if (Entry.State != FStorage::EIndexState::Occupied)
			continue;
		const auto& Link = Storage->Links[Entry.LinkSlot];
		if (Link.bOccupied && Link.FromRequestId == Seed.FromRequestId && Link.ToRequestId == Seed.ToRequestId &&
			Link.bAuth == Seed.bAuth && Link.QueueId.View() == Seed.QueueId)
			return true;
	}
	return false;
}
int32 FStore::NumLinksForTarget(uint64 RequestId) const
{
	if (!Storage)
		return 0;
	const uint32 Mask = uint32(Storage->Plan.LinkTargetCapacity - 1);
	const uint32 Start = HashRequestId(RequestId);
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Entry = Storage->LinkTargets[int32((Start + Offset) & Mask)];
		if (Entry.State == FStorage::EIndexState::Empty)
			return 0;
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.ToRequestId == RequestId)
			return Entry.Count;
	}
	return 0;
}
ELinkAdmission FStore::TryAdmitLink(const FLinkSeed& Seed, const FAdmissionAnchors& Anchors)
{
	if (!Storage || Seed.FromRequestId == 0 || (Seed.bAuth && Seed.ToRequestId == 0) ||
		Seed.QueueId.Len() < 0 || Seed.QueueId.Len() > FNormalizedLimits::MaxStringChars ||
		(Seed.QueueId.Len() > 0 && !Seed.QueueId.GetData()) || !ValidateBorrowedSource(Seed.QueueId, Anchors))
	{
		Increment(State.OmittedLinks);
		Increment(State.MetadataOmitted);
		return ELinkAdmission::Refused;
	}
	const uint32 Mask = uint32(Storage->Plan.LinkIndexCapacity - 1);
	const uint32 Start = HashLinkSeed(Seed);
	int32 ForwardSlot = INDEX_NONE;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		const auto& Entry = Storage->LinkForward[Candidate];
		if (Entry.State == FStorage::EIndexState::Empty)
		{
			if (ForwardSlot == INDEX_NONE)
				ForwardSlot = Candidate;
			break;
		}
		if (Entry.State == FStorage::EIndexState::Deleted)
		{
			if (ForwardSlot == INDEX_NONE)
				ForwardSlot = Candidate;
			continue;
		}
		const auto& Link = Storage->Links[Entry.LinkSlot];
		if (Link.bOccupied && Link.FromRequestId == Seed.FromRequestId && Link.ToRequestId == Seed.ToRequestId &&
			Link.bAuth == Seed.bAuth && Link.QueueId.View() == Seed.QueueId)
		{
			return ELinkAdmission::AlreadyPresent;
		}
	}
	if (ForwardSlot == INDEX_NONE)
	{
		Increment(State.OmittedLinks);
		Increment(State.MetadataOmitted);
		return ELinkAdmission::Refused;
	}
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const SIZE_T TextRequest = (SIZE_T(Seed.QueueId.Len()) + 1) * sizeof(TCHAR);
	const auto TextBound = Seed.QueueId.IsEmpty() ? TOptional<SIZE_T>(0) : Allocator.Bound(TextRequest, alignof(TCHAR));
	if (!TextBound.IsSet() || !MakeAdmissionRoom({0, int64(TextBound.GetValue()), Anchors}))
	{
		Increment(State.OmittedLinks);
		Increment(State.MetadataOmitted);
		return ELinkAdmission::Refused;
	}
	FTicketedText QueueId;
	if (!CopyTextAdmitted(Seed.QueueId, QueueId))
	{
		Increment(State.OmittedLinks);
		Increment(State.MetadataOmitted);
		return ELinkAdmission::Refused;
	}
	bool bReplaced = false;
	if (Storage->NumLinks >= Storage->Plan.LinkCapacity)
	{
		RemoveLinkAt(Storage->LinkHead);
		bReplaced = true;
	}
	if (auto* Target = FindLinkTarget(Seed.ToRequestId, false))
	{
		const int32 TargetLimit = FMath::Min(FNormalizedLimits::LinksPerTarget, Storage->Plan.LinkCapacity);
		if (Target->Count >= TargetLimit)
		{
			RemoveLinkAt(Target->HeadSlot);
			bReplaced = true;
		}
	}
	auto* Target = FindLinkTarget(Seed.ToRequestId, true);
	check(Target);
	int32 Slot = Storage->LinkFreeHead;
	if (Slot != INDEX_NONE)
		Storage->LinkFreeHead = Storage->Links[Slot].NextFree;
	else
		Slot = Storage->NumLinkSlotsUsed++;
	check(Slot >= 0 && Slot < Storage->Plan.LinkCapacity);
	auto& Entry = Storage->Links[Slot];
	Entry.FromRequestId = Seed.FromRequestId;
	Entry.ToRequestId = Seed.ToRequestId;
	Entry.QueueId = MoveTemp(QueueId);
	Entry.bAuth = Seed.bAuth;
	Entry.bTargetEvicted = false;
	Entry.bOccupied = true;
	Entry.ForwardIndexSlot = ForwardSlot;
	Entry.TargetIndexSlot = Target->IndexSlot;
	Entry.PrevFifo = Storage->LinkTail;
	Entry.NextFifo = INDEX_NONE;
	Entry.PrevForTarget = Target->TailSlot;
	Entry.NextForTarget = INDEX_NONE;
	Entry.NextFree = INDEX_NONE;
	if (Storage->LinkTail != INDEX_NONE)
		Storage->Links[Storage->LinkTail].NextFifo = Slot;
	else
		Storage->LinkHead = Slot;
	Storage->LinkTail = Slot;
	if (Target->TailSlot != INDEX_NONE)
		Storage->Links[Target->TailSlot].NextForTarget = Slot;
	else
		Target->HeadSlot = Slot;
	Target->TailSlot = Slot;
	++Target->Count;
	Storage->LinkForward[ForwardSlot].State = FStorage::EIndexState::Occupied;
	Storage->LinkForward[ForwardSlot].LinkSlot = Slot;
	++Storage->NumLinks;
	Increment(RevisionValue);
	return bReplaced ? ELinkAdmission::ReplacedOldest : ELinkAdmission::Added;
}
void FStore::ClearLinks()
{
	if (!Storage)
		return;
	for (int32 Index = 0; Index < Storage->Plan.LinkCapacity; ++Index)
	{
		Storage->Links[Index].QueueId.Reset();
		Storage->Links[Index] = FLinkEntry{};
	}
	for (int32 Index = 0; Index < Storage->Plan.LinkIndexCapacity; ++Index)
		Storage->LinkForward[Index] = FLinkForwardEntry{};
	for (int32 Index = 0; Index < Storage->Plan.LinkTargetCapacity; ++Index)
		Storage->LinkTargets[Index] = FLinkTargetEntry{};
	Storage->NumLinkSlotsUsed = Storage->NumLinks = 0;
	Storage->LinkFreeHead = Storage->LinkHead = Storage->LinkTail = INDEX_NONE;
}
void FStore::ClearStats()
{
	if (!Storage)
		return;
	for (int32 Index = 0; Index < Storage->Plan.StatsIndexCapacity; ++Index)
	{
		Storage->StatsIndex[Index].Group.Reset();
		Storage->StatsIndex[Index].State = FStorage::EIndexState::Empty;
	}
	Storage->NumStatsGroups = Storage->OverflowStats ? 1 : 0;
	if (Storage->OverflowStats)
	{
		Storage->OverflowStats->Value = FStatsGroup{};
		Storage->OverflowStats->Value.bOverflow = true;
		Storage->OverflowStats->Key.Reset();
	}
}
bool FStore::MeasureMutableSnapshot(FSnapshotBuilder& Builder, const FRow& Row) const
{
	if (!Builder.MeasureDispatches(Row.NumDispatches) ||
		!Builder.MeasureNotes(Row.Notes.IsAllocated() ? FNormalizedLimits::NoteSlots : 0) ||
		!Builder.MeasureTimeline(Row.Timeline.IsAllocated() ? FNormalizedLimits::TimelineSlots : 0))
		return false;
	if (Row.Started && !Builder.MeasurePacket(Row.Started->View(), &Row.Started->Get().Excerpt()))
		return false;
	if (Row.Completed && Row.Value.CompletedMetadataState != EVaRestInspectorDetailState::MetadataOmitted &&
		!Builder.MeasurePacket(Row.Completed->View(), &Row.Completed->Get().Excerpt()))
		return false;
	if (Row.RequestExcerpt && !Builder.MeasurePacket(Row.RequestExcerpt->View(), &Row.RequestExcerpt->Get().Excerpt()))
		return false;
	if (Row.Marker && !Builder.MeasurePacket(Row.Marker->View(), nullptr))
		return false;
	if (Row.Notes.IsAllocated())
		for (int32 Index = 0; Index < FNormalizedLimits::NoteSlots; ++Index)
			if (Row.Notes[Index].Owner && !Builder.MeasurePacket(Row.Notes[Index].Owner->View(), &Row.Notes[Index].Owner->Get().Excerpt()))
				return false;
	const FDispatchHeaderOwner* HeaderOwners[16] = {};
	int32 NumHeaders = 0;
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
	{
		const FDispatchSlot& Slot = *Row.Dispatches[Index];
		if (Slot.Ended && !Builder.MeasurePacket(Slot.Ended->View(), &Slot.Ended->Get().Excerpt()))
			return false;
		if (Slot.ResponseExcerpt && !Builder.MeasurePacket(Slot.ResponseExcerpt->View(), &Slot.ResponseExcerpt->Get().Excerpt()))
			return false;
		if (!Slot.Headers)
			continue;
		int32 Header = 0;
		while (Header < NumHeaders && HeaderOwners[Header] != Slot.Headers.Get())
			++Header;
		if (Header == NumHeaders)
			HeaderOwners[NumHeaders++] = Slot.Headers.Get();
	}
	if (!Builder.MeasureHeaders(NumHeaders))
		return false;
	for (int32 Index = 0; Index < NumHeaders; ++Index)
		if (!Builder.MeasurePacket(HeaderOwners[Index]->View(), nullptr))
			return false;
	return true;
}
bool FStore::ConstructMutableSnapshot(FSnapshotBuilder& Builder, const FRow& Row, FSnapshotRowCopy& Copy) const
{
	Copy.Value = Row.Value;
	Copy.Value.Started = {};
	Copy.Value.Completed = {};
	Copy.Value.Marker = {};
	Copy.Value.Dispatches = {};
	Copy.Value.DispatchEvents = {};
	Copy.Value.Notes = {};
	Copy.Value.Excerpts = {};
	Copy.Value.Timeline = {};
	Copy.Value.ScopeLabel = {};
	Copy.Value.ClientLabel = {};
	Copy.Evidence = Row.Evidence;
	Copy.PresenceStamp = Row.PresenceStamp;
	Copy.IndexSlot = Row.IndexSlot;
	Copy.DeadlineIndex = Row.DeadlineIndex;
	Copy.CompletionFixed = Row.CompletionFixed;
	Copy.NoteHead = Row.NoteHead;
	Copy.NumNotes = Row.NumNotes;
	Copy.TimelineHead = Row.TimelineHead;
	Copy.NumTimeline = Row.NumTimeline;
	Copy.NextNoteSerial = Row.NextNoteSerial;
	Copy.PreviousDispatchSlot = Row.PreviousDispatchSlot;
	if (Row.Started)
	{
		Copy.bHasStarted = true;
		if (!Builder.ConstructPacket(Row.Started->View(), &Row.Started->Get().Excerpt(), Copy.Started))
			return false;
	}
	if (Row.Completed && Row.Value.CompletedMetadataState != EVaRestInspectorDetailState::MetadataOmitted)
	{
		Copy.bHasCompletedOwner = true;
		if (!Builder.ConstructPacket(Row.Completed->View(), &Row.Completed->Get().Excerpt(), Copy.Completed))
			return false;
	}
	if (Row.RequestExcerpt)
	{
		Copy.bHasRequestExcerpt = true;
		if (!Builder.ConstructPacket(Row.RequestExcerpt->View(), &Row.RequestExcerpt->Get().Excerpt(), Copy.RequestExcerpt))
			return false;
	}
	if (Row.Marker)
	{
		Copy.bHasMarker = true;
		if (!Builder.ConstructPacket(Row.Marker->View(), nullptr, Copy.Marker))
			return false;
	}
	if (!Builder.ConstructTimeline(Copy, Row.Timeline.IsAllocated() ? FNormalizedLimits::TimelineSlots : 0))
		return false;
	for (int32 Index = 0; Index < Copy.TimelineSlots; ++Index)
	{
		Copy.Timeline[Index].Value = Row.Timeline[Index].Value;
		Copy.Timeline[Index].NoteSerial = Row.Timeline[Index].NoteSerial;
	}
	if (!Builder.ConstructNotes(Copy, Row.Notes.IsAllocated() ? FNormalizedLimits::NoteSlots : 0))
		return false;
	for (int32 Index = 0; Index < Copy.NoteSlots; ++Index)
	{
		Copy.Notes[Index].Serial = Row.Notes[Index].Serial;
		if (Row.Notes[Index].Owner && !Builder.ConstructPacket(Row.Notes[Index].Owner->View(), &Row.Notes[Index].Owner->Get().Excerpt(), Copy.Notes[Index].Packet))
			return false;
	}
	const FDispatchHeaderOwner* HeaderOwners[16] = {};
	int32 NumHeaders = 0;
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
	{
		const auto& Headers = Row.Dispatches[Index]->Headers;
		if (!Headers)
			continue;
		int32 Header = 0;
		while (Header < NumHeaders && HeaderOwners[Header] != Headers.Get())
			++Header;
		if (Header == NumHeaders)
			HeaderOwners[NumHeaders++] = Headers.Get();
	}
	if (!Builder.ConstructHeaders(Copy, NumHeaders))
		return false;
	for (int32 Index = 0; Index < NumHeaders; ++Index)
		if (!Builder.ConstructPacket(HeaderOwners[Index]->View(), nullptr, Copy.Headers[Index]))
			return false;
	if (!Builder.ConstructDispatches(Copy, Row.NumDispatches))
		return false;
	for (int32 Index = 0; Index < Row.NumDispatches; ++Index)
	{
		const FDispatchSlot& Source = *Row.Dispatches[Index];
		FSnapshotDispatch& Target = Copy.Dispatches[Index];
		Target.DispatchId = Source.DispatchId;
		Target.Attempt = Source.Attempt;
		Target.bHasDispatched = Source.bHasDispatched;
		Target.bHasEnded = Source.bHasEnded;
		Target.DispatchedFixed = Source.DispatchedFixed;
		Target.EndedFixed = Source.EndedFixed;
		Target.HeadersState = Source.HeadersState;
		Target.ResponseExcerptState = Source.ResponseExcerptState;
		if (Source.bHasEnded)
		{
			if (Source.Ended)
			{
				if (!Builder.ConstructPacket(Source.Ended->View(), &Source.Ended->Get().Excerpt(), Target.Ended))
					return false;
			}
			else
				Builder.ConstructScalarPacket(Source.EndedFixed, Target.Ended);
		}
		if (Source.ResponseExcerpt && !Builder.ConstructPacket(Source.ResponseExcerpt->View(), &Source.ResponseExcerpt->Get().Excerpt(), Target.ResponseExcerpt))
			return false;
		if (Source.Headers)
			for (int32 Header = 0; Header < NumHeaders; ++Header)
				if (HeaderOwners[Header] == Source.Headers.Get())
				{
					Target.HeaderIndex = Header;
					break;
				}
	}
	return true;
}
bool FStore::MeasureSnapshot(FSnapshotBuilder& Builder) const
{
	if (!Builder.Begin(*this, Storage->NumRows))
		return false;
	bool bMeasured = true;
	ForEachRow([&](FRow& Row) {
		if (!bMeasured)
			return;
		const FStringView Label = ClientLabel(Row.Value.ClientId);
		if (ShouldShareRow(Row))
			bMeasured = Builder.MeasureSealedRow(Label);
		else
			bMeasured = Builder.MeasureMutableRow(Label) && MeasureMutableSnapshot(Builder, Row);
	});
	if (bMeasured)
		bMeasured = Builder.MeasureStats(Storage->NumStatsGroups);
	int32 NumMeasuredStats = 0;
	const auto MeasureGroup = [&](const FStatsGroupSlotPtr& Group) {
		if (!Group)
			return;
		if (bMeasured)
			bMeasured = Builder.MeasureStatsGroup(*Group);
		++NumMeasuredStats;
	};
	MeasureGroup(Storage->OverflowStats);
	for (int32 Index = 0; Index < Storage->Plan.StatsIndexCapacity; ++Index)
		MeasureGroup(Storage->StatsIndex[Index].Group);
	if (NumMeasuredStats != Storage->NumStatsGroups || !bMeasured || !Builder.FinishMeasurement())
		return false;
	return true;
}
bool FStore::MeasureSnapshotCosts(int64& Outer, int64& Nested) const
{
	check(IsInGameThread());
	if (!Storage || !Budget || bEnumerating)
		return false;
	RefreshLossWitness();
	FSnapshotBuilder Builder;
	return MeasureSnapshot(Builder) && Builder.MeasuredCosts(Outer, Nested);
}
void FStore::NoteUnsupportedPublicStrings() const
{
	check(Budget);
	Budget->PublicStringsUnsupported.store(true, std::memory_order_release);
	Increment(State.UnsupportedAllocator);
}
bool FStore::TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const
{
	check(IsInGameThread());
	if (!Storage || !Budget || !Budget->Lifetime ||
		!HasInspectorControllerCalibration<FRow>(*Budget->Lifetime) ||
		!HasInspectorControllerCalibration<FDispatchHeaderOwner>(*Budget->Lifetime) ||
		!HasInspectorControllerCalibration<FVaRestInspectorSnapshot>(*Budget->Lifetime))
		return false;
	if (bEnumerating && !bLossDirty)
	{
		// Only the first dirty transition needs a row mutation outside enumeration.
		const uint64 Current = LossWitness ? LossWitness->LossEpoch.load(std::memory_order_acquire) : MAX_uint64;
		if (!bLossWitnessBound || Current == MAX_uint64 || Current != GenerationLossEpoch)
			return false;
	}
	RefreshLossWitness();
	FSnapshotBuilder Builder;
	const auto NoteFailure = [this, &Builder]() {
		if (Builder.HasUnsupportedPublicStrings())
			NoteUnsupportedPublicStrings();
	};
	if (!MeasureSnapshot(Builder) || !Builder.Reserve())
	{
		NoteFailure();
		return false;
	}
	bool bConstructed = true;
	ForEachRow([&](FRow& Row) {
		if (!bConstructed)
			return;
		const auto& Entry = Storage->Index[Row.IndexSlot];
		const FStringView Label = ClientLabel(Row.Value.ClientId);
		if (ShouldShareRow(Row))
		{
			TSharedPtr<const FRow, ESPMode::ThreadSafe> Owner = Entry.Row;
			bConstructed = Builder.ConstructSealedEntry(Owner, Label);
		}
		else
		{
			FSnapshotRowCopy* Copy = Builder.ConstructMutableRow();
			bConstructed = Copy && ConstructMutableSnapshot(Builder, Row, *Copy) && Builder.ConstructMutableEntry(Copy, Label);
		}
	});
	if (bConstructed)
		bConstructed = Builder.ConstructStats(Storage->NumStatsGroups);
	int32 NumConstructedStats = 0;
	const auto ConstructGroup = [&](const FStatsGroupSlotPtr& Group) {
		if (!Group)
			return;
		if (bConstructed)
			bConstructed = Builder.ConstructStatsGroup(*Group, NumConstructedStats);
		++NumConstructedStats;
	};
	ConstructGroup(Storage->OverflowStats);
	for (int32 Index = 0; Index < Storage->Plan.StatsIndexCapacity; ++Index)
		ConstructGroup(Storage->StatsIndex[Index].Group);
	if (NumConstructedStats != Storage->NumStatsGroups)
		bConstructed = false;
	if (!bConstructed || !Builder.Publish(Out))
	{
		NoteFailure();
		return false;
	}
	return true;
}
void FStore::VisitImmutableRow(const FRow& Row, FStringView Label, TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor)
{
	FVaRestInspectorRecordView View = Row.Value;
	View.Started = {};
	View.Completed = {};
	View.Marker = {};
	if (Row.Started)
		View.Started = Row.Started->View();
	if (Row.Completed)
		View.Completed = Row.Completed->View();
	else if (Row.Value.bActualCompletionObserved)
		View.Completed.Fixed = &Row.CompletionFixed;
	if (Row.Marker)
		View.Marker = Row.Marker->View();
	// All composition is visitor-local. Persistent row state never borrows
	// these arrays, and shared header backing keeps its initialized extent.
	FPacketFixed DispatchFixed[16];
	FVaRestInspectorDispatchView DispatchViews[16];
	FVaRestInspectorPacketView DispatchEvents[32], Excerpts[17];
	FVaRestInspectorPacketView Notes[FNormalizedLimits::NoteSlots];
	FVaRestInspectorTimelineEntry Timeline[FNormalizedLimits::TimelineSlots];
	int32 NumEvents = 0, NumExcerpts = 0;
	for (int32 NoteIndex = 0; NoteIndex < Row.NumNotes; ++NoteIndex)
		Notes[NoteIndex] = Row.Notes[(Row.NoteHead + NoteIndex) % FNormalizedLimits::NoteSlots].Owner->View();
	for (int32 TimelineIndex = 0; TimelineIndex < Row.NumTimeline; ++TimelineIndex)
		Timeline[TimelineIndex] = Row.Timeline[(Row.TimelineHead + TimelineIndex) % FNormalizedLimits::TimelineSlots].Value;
	View.Notes = {Notes, Row.NumNotes};
	View.Timeline = {Timeline, Row.NumTimeline};
	if (Row.RequestExcerpt)
		Excerpts[NumExcerpts++] = Row.RequestExcerpt->View();
	for (int32 DispatchIndex = 0; DispatchIndex < Row.NumDispatches; ++DispatchIndex)
	{
		const FDispatchSlot& Slot = *Row.Dispatches[DispatchIndex];
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
			if (Slot.Headers)
			{
				const auto Headers = Slot.Headers->View();
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
			if (Slot.Ended)
				Dispatch.Ended = Slot.Ended->View();
			else
				Dispatch.Ended.Fixed = &Slot.EndedFixed;
			Dispatch.UploadDoneAt = Slot.EndedFixed.Scalars.UploadDoneAt;
			Dispatch.StatusAt = Slot.EndedFixed.Scalars.StatusAt;
			Dispatch.TransportDoneAt = Slot.EndedFixed.Scalars.TransportDoneAt;
			DispatchEvents[NumEvents++] = Dispatch.Ended;
		}
		if (Slot.ResponseExcerpt)
		{
			Dispatch.ResponseExcerpt = Slot.ResponseExcerpt->View();
			Excerpts[NumExcerpts++] = Dispatch.ResponseExcerpt;
		}
		DeriveDispatchPhases(Dispatch);
	}
	View.Dispatches = {DispatchViews, Row.NumDispatches};
	View.DispatchEvents = {DispatchEvents, NumEvents};
	View.Excerpts = {Excerpts, NumExcerpts};
	if (View.ScopeId == 0)
		View.ScopeLabel = TEXT("Process");
	View.ClientLabel = Label;
	Visitor(View);
}
bool FStore::ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const
{
	check(IsInGameThread());
	if (!Storage || bEnumerating)
		return false;
	RefreshLossWitness();
	bEnumerating = true;
	ON_SCOPE_EXIT
	{
		bEnumerating = false;
		const_cast<FStore*>(this)->FlushDeferred();
	};
	ForEachRow([&](FRow& Row) {
		VisitImmutableRow(Row, ClientLabel(Row.Value.ClientId), Visitor);
	});
	return true;
}
FVaRestInspectorDiagnostics FStore::Diagnostics() const
{
	check(IsInGameThread());
	FVaRestInspectorDiagnostics Result = State;
	if (Budget)
	{
		Result.StoreBytes = Budget->UsedBytes.load();
		Result.StorePeakBytes = Budget->PeakBytes.load();
		Result.SnapshotControllers = int32(Budget->SnapshotControllers.load());
	}
	return Result;
}
} // namespace VaRest::Inspector::Private
#endif
