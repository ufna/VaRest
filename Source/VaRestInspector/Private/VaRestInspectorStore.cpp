// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorStore.h"
#include "VaRestInspectorPhases.h"
#include "VaRestInspectorSnapshot.h"

#include "CoreGlobals.h"
#include "HAL/UnrealMemory.h"
#include "Math/UnrealMathUtility.h"
#include "Misc/ScopeExit.h"
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
// Fixed storage is acquired transactionally. No container can grow outside its
// credit, and every element/backing is destroyed before that credit releases.
template <typename T>
class TStoreBlock
{
public:
	~TStoreBlock() { Reset(); }
	bool IsAllocated() const { return Data != nullptr; }
	bool Allocate(TRefCountPtr<FBudgetAccount> Account, int32 InCount)
	{
		if (Data || !Account || !Account->Lifetime || InCount <= 0)
			return false;
		const uint64 Request = uint64(InCount) * sizeof(T);
		if (Request > uint64(MAX_int64))
			return false;
		FAllocationAdapter& Adapter = Account->Lifetime->Allocator;
		const auto Bound = Adapter.Bound(SIZE_T(Request), alignof(T));
		if (!Bound.IsSet() || Bound.GetValue() > SIZE_T(MAX_int64))
			return false;
		auto Reservation = FStoreCredit::Acquire(Account, int64(Bound.GetValue()));
		if (!Reservation.IsSet())
			return false;
		void* Memory = Adapter.AllocateReserved(SIZE_T(Request), alignof(T), Bound.GetValue());
		if (!Memory)
			return false;
		Credit = MoveTemp(Reservation.GetValue());
		Data = static_cast<T*>(Memory);
		Count = InCount;
		for (int32 Index = 0; Index < Count; ++Index)
			new (Data + Index) T();
		return true;
	}
	void Reset()
	{
		if (!Data)
			return;
		FAllocationAdapter& Adapter = Credit.GetAccount()->Lifetime->Allocator;
		for (int32 Index = 0; Index < Count; ++Index)
			Data[Index].~T();
		Adapter.Free(Data);
		Data = nullptr;
		Count = 0;
		Credit.Reset();
	}
	T& operator[](int32 Index)
	{
		check(Index >= 0 && Index < Count);
		return Data[Index];
	}
	const T& operator[](int32 Index) const
	{
		check(Index >= 0 && Index < Count);
		return Data[Index];
	}

private:
	FStoreCredit Credit;
	T* Data = nullptr;
	int32 Count = 0;
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

struct FStore::FRow
{
	FVaRestInspectorRecordView Value;
	ELivenessEvidence Evidence = ELivenessEvidence::None;
	uint64 PresenceStamp = 0;
	FStorePacketPtr Started, Completed, RequestExcerpt;
	// This permanently admitted fixed slot preserves all actual terminal
	// scalars even when optional Completed packet ownership is refused.
	FPacketFixed CompletionFixed;
	TStoreBlock<FNoteEntry> Notes;
	TStoreBlock<FTimelineSlot> Timeline;
	int32 NoteHead = 0, NumNotes = 0, TimelineHead = 0, NumTimeline = 0;
	uint64 NextNoteSerial = 0;
	FDispatchSlotPtr Dispatches[16];
	int32 NumDispatches = 0, PreviousDispatchSlot = INDEX_NONE;
	int32 IndexSlot = INDEX_NONE, DeadlineIndex = INDEX_NONE;
};
struct FStore::FStorage
{
	enum class EIndexState : uint8
	{
		Empty,
		Occupied,
		Deleted
	};
	struct FIndexEntry
	{
		EIndexState State = EIndexState::Empty;
		uint64 RequestId = 0;
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
	// Ring positions store index-slot identities. Matching fixed capacities
	// permit bounded insertion/deletion without reallocating either domain.
	TStoreBlock<int32> Arrival;
	TStoreBlock<FClientEntry> Clients;
	TStoreBlock<FDeadlineEntry> DeadlineHeap;
	TStoreBlock<FRetainedIdEntry> RetainedIds;
	TStoreBlock<FRetainedLookupEntry> RetainedLookup;
	int32 IndexCapacity = 0, MaxRows = 0, ClientCapacity = 0;
	int32 RetainedCapacity = 0, RetainedLookupCapacity = 0;
	int32 RetainedHead = 0, NumRetained = 0, NumDeadline = 0;
	int32 NumRows = 0, NumClients = 0;
};
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
bool FStore::Initialize(TRefCountPtr<FBudgetAccount> InAccount)
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
	FAllocationAdapter& Allocator = InAccount->Lifetime->Allocator;
	const auto SelfBound = Allocator.Bound(sizeof(FStorage), alignof(FStorage));
	const auto RowBound = Allocator.Bound(sizeof(FRow), alignof(FRow));
	if (!SelfBound.IsSet() || !RowBound.IsSet())
	{
		Increment(State.UnsupportedAllocator);
		return false;
	}
	int32 MaxRows = Limits.MaxRecords, ClientCapacity = FNormalizedLimits::Clients;
	int32 IndexCapacity = 0, RetainedCapacity = 0, RetainedLookupCapacity = 0;
	bool bFits = false;
	while (true)
	{
		IndexCapacity = 2;
		while (IndexCapacity < MaxRows * 2)
			IndexCapacity *= 2;
		RetainedCapacity = FMath::Min(FNormalizedLimits::Tombstones, MaxRows);
		RetainedLookupCapacity = 2;
		while (RetainedLookupCapacity < RetainedCapacity * 2)
			RetainedLookupCapacity *= 2;
		const auto IndexBound = Allocator.Bound(SIZE_T(IndexCapacity) * sizeof(FStorage::FIndexEntry), alignof(FStorage::FIndexEntry));
		const auto ArrivalBound = Allocator.Bound(SIZE_T(IndexCapacity) * sizeof(int32), alignof(int32));
		const auto ClientsBound = Allocator.Bound(SIZE_T(ClientCapacity) * sizeof(FClientEntry), alignof(FClientEntry));
		const auto DeadlineBound = Allocator.Bound(SIZE_T(MaxRows) * sizeof(FStorage::FDeadlineEntry), alignof(FStorage::FDeadlineEntry));
		const auto RetainedBound = Allocator.Bound(SIZE_T(RetainedCapacity) * sizeof(FStorage::FRetainedIdEntry), alignof(FStorage::FRetainedIdEntry));
		const auto RetainedLookupBound = Allocator.Bound(SIZE_T(RetainedLookupCapacity) * sizeof(FStorage::FRetainedLookupEntry), alignof(FStorage::FRetainedLookupEntry));
		int64 Startup = InAccount->BootstrapBytes;
		bFits = IndexBound.IsSet() && ArrivalBound.IsSet() && ClientsBound.IsSet() && DeadlineBound.IsSet() && RetainedBound.IsSet() && RetainedLookupBound.IsSet() &&
				AddChecked(Startup, int64(SelfBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(IndexBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(ArrivalBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(ClientsBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(DeadlineBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(RetainedBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(RetainedLookupBound.GetValue()), Limits.StoreBytes / 4) &&
				AddChecked(Startup, int64(RowBound.GetValue()), Limits.StoreBytes) && AddChecked(Startup, 4096, Limits.StoreBytes);
		if (bFits)
			break;
		if (ClientCapacity > 1)
			ClientCapacity /= 2;
		else if (MaxRows > 1)
			MaxRows /= 2;
		else
			break;
	}
	if (!bFits)
	{
		Increment(State.RefusedRows);
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
	if (!Candidate->Index.Allocate(InAccount, IndexCapacity) || !Candidate->Arrival.Allocate(InAccount, IndexCapacity) ||
		!Candidate->Clients.Allocate(InAccount, ClientCapacity) || !Candidate->DeadlineHeap.Allocate(InAccount, MaxRows) ||
		!Candidate->RetainedIds.Allocate(InAccount, RetainedCapacity) || !Candidate->RetainedLookup.Allocate(InAccount, RetainedLookupCapacity))
	{
		Increment(State.RefusedRows);
		return false;
	}
	Candidate->IndexCapacity = IndexCapacity;
	Candidate->MaxRows = MaxRows;
	Candidate->ClientCapacity = ClientCapacity;
	Candidate->RetainedCapacity = RetainedCapacity;
	Candidate->RetainedLookupCapacity = RetainedLookupCapacity;
	Budget = MoveTemp(InAccount);
	Storage = MoveTemp(Candidate);
	return true;
}
FStore::FRow* FStore::Find(uint64 RequestId) const
{
	if (!Storage || RequestId == 0)
		return nullptr;
	const uint32 Mask = uint32(Storage->IndexCapacity - 1);
	const uint32 Start = uint32(RequestId ^ (RequestId >> 32)) * 0x9e3779b9u;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const auto& Entry = Storage->Index[int32((Start + Offset) & Mask)];
		if (Entry.State == FStorage::EIndexState::Empty)
			return nullptr;
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.RequestId == RequestId)
			return Entry.Row.Get();
	}
	return nullptr;
}
FStore::FRow* FStore::AdmitRow(uint64 RequestId, bool bPartial)
{
	if (!Storage || RequestId == 0 || Storage->NumRows >= Storage->MaxRows || NextArrival == MAX_uint64)
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	const uint32 Mask = uint32(Storage->IndexCapacity - 1);
	const uint32 Start = uint32(RequestId ^ (RequestId >> 32)) * 0x9e3779b9u;
	int32 Slot = INDEX_NONE;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Candidate = int32((Start + Offset) & Mask);
		auto& Entry = Storage->Index[Candidate];
		if (Entry.State == FStorage::EIndexState::Occupied)
		{
			if (Entry.RequestId == RequestId)
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
	if (Slot == INDEX_NONE || Storage->NumRows >= Storage->IndexCapacity)
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
	auto Row = MakeInspectorOwner<FRow>(Budget, Allocator.GetSupport(), Bound.GetValue(), false);
	if (!Row)
	{
		Increment(State.RefusedRows);
		return nullptr;
	}
	Row->Value.RequestId = RequestId;
	Row->Value.bPartial = bPartial;
	Row->Value.ArrivalOrdinal = ++NextArrival;
	Row->Value.BirthMetadataOrdinal = LastConsumedMetadataOrdinal;
	Row->IndexSlot = Slot;
	Storage->Index[Slot].State = FStorage::EIndexState::Occupied;
	Storage->Index[Slot].RequestId = RequestId;
	Storage->Index[Slot].Row = MoveTemp(Row);
	Storage->Index[Slot].bRemovePending = false;
	Storage->Arrival[Storage->NumRows++] = Slot;
	FRow* Result = Storage->Index[Slot].Row.Get();
	Touch(*Result);
	return Result;
}
void FStore::Touch(FRow& Row)
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
	if (!FStorePacketOwner::TryTake(Packet, Budget, Allocator, Stored))
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
	if (ClientId == 0)
	{
		Increment(State.OmittedClients);
		return;
	}
	FClientEntry* Entry = nullptr;
	for (int32 Index = 0; Index < Storage->NumClients; ++Index)
		if (Storage->Clients[Index].ClientId == ClientId)
		{
			Entry = &Storage->Clients[Index];
			break;
		}
	if (!Entry)
	{
		if (Storage->NumClients >= Storage->ClientCapacity)
		{
			Increment(State.OmittedClients);
			return;
		}
		Entry = &Storage->Clients[Storage->NumClients++];
		Entry->ClientId = ClientId;
	}
	Entry->ScopeId = ScopeId;
	Entry->bCreated = bCreated;
	if (Label.IsEmpty() || (Entry->Label && FStringView(Entry->Label, Entry->Chars) == Label))
		return;
	if (Label.Len() > FNormalizedLimits::MaxStringChars)
	{
		Increment(State.OmittedClients);
		return;
	}
	FAllocationAdapter& Allocator = Budget->Lifetime->Allocator;
	const SIZE_T Request = (SIZE_T(Label.Len()) + 1) * sizeof(TCHAR);
	const auto Bound = Allocator.Bound(Request, alignof(TCHAR));
	if (!Bound.IsSet())
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
	if (CurrentReconciliationStamp == MAX_uint64)
	{
		for (int32 Index = 0; Index < Storage->NumRows; ++Index)
		{
			FRow& Row = *Storage->Index[Storage->Arrival[Index]].Row;
			if (!Row.Value.bSealed)
				Row.PresenceStamp = 0;
		}
		CurrentReconciliationStamp = 0;
	}
	++CurrentReconciliationStamp;
	for (const FVaRestTraceClient& Client : Snapshot.Clients)
		ApplyClient(Client.ClientId, Client.ScopeId, Client.bCreated, FStringView(Client.DisplayName));
	for (const FVaRestTraceStarted& Borrowed : Snapshot.InFlight)
		ApplyAttachedStarted(Borrowed, Policy, Pending, Allocator);
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
	{
		FRow& Row = *Storage->Index[Storage->Arrival[Index]].Row;
		if (Row.Value.bTerminalKnown || Row.Value.bSealed || Row.Value.BirthMetadataOrdinal > LastConsumedMetadataOrdinal || Row.PresenceStamp == CurrentReconciliationStamp)
			continue;
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
	}
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
			else if (!FDispatchHeaderOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Slot->Headers))
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
		if (!FStorePacketOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Slot->Ended))
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
	if (!FStorePacketOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Candidate))
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
	if (!FStorePacketOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Row.Completed))
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
		RemoveDeadline(Row);
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
	for (int32 Arrival = 0; Arrival < Storage->NumRows; ++Arrival)
	{
		FRow& Row = *Storage->Index[Storage->Arrival[Arrival]].Row;
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
	}
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
	return Row.Notes.IsAllocated() || Row.Notes.Allocate(Budget, FNormalizedLimits::NoteSlots);
}
bool FStore::EnsureTimelineStorage(FRow& Row)
{
	return Row.Timeline.IsAllocated() || Row.Timeline.Allocate(Budget, FNormalizedLimits::TimelineSlots);
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
	if (!FStorePacketOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Candidate))
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
		if (!Row->Started)
		{
			FStorePacketPtr Owner;
			if (FStorePacketOwner::TryTake(Packet, Budget, Budget->Lifetime->Allocator, Owner))
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
	const uint32 Start = uint32(RequestId ^ (RequestId >> 32)) * 0x9e3779b9u;
	for (uint32 Offset = 0; Offset <= Mask; ++Offset)
	{
		const int32 Slot = int32((Start + Offset) & Mask);
		auto& Entry = Storage->Index[Slot];
		if (Entry.State == FStorage::EIndexState::Empty)
			return false;
		if (Entry.State != FStorage::EIndexState::Occupied || Entry.RequestId != RequestId)
			continue;
		if (!Entry.Row || Entry.Row->Value.bPinned)
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
	if (Entry.State != FStorage::EIndexState::Occupied || !Entry.Row || Entry.Row->Value.bPinned)
		return false;
	FRow& Row = *Entry.Row;
	RemoveDeadline(Row);
	bool bArrivalRemoved = false;
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
		if (Storage->Arrival[Index] == IndexSlot)
		{
			for (int32 Move = Index; Move + 1 < Storage->NumRows; ++Move)
				Storage->Arrival[Move] = Storage->Arrival[Move + 1];
			--Storage->NumRows;
			Storage->Arrival[Storage->NumRows] = INDEX_NONE;
			bArrivalRemoved = true;
			break;
		}
	check(bArrivalRemoved);
	RetainRemovedId(Entry.RequestId);
	Entry.bRemovePending = false;
	Entry.Row.Reset();
	Entry.RequestId = 0;
	Entry.State = FStorage::EIndexState::Deleted;
	Increment(RevisionValue);
	return true;
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
		if (Entry.State == FStorage::EIndexState::Occupied && Entry.Row && !Entry.Row->Value.bPinned)
			RemoveEligibleRowNow(Index);
	}
}
void FStore::FlushDeferred()
{
	if (bEnumerating)
		return;
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
	Generation = InGeneration;
	LastConsumedMetadataOrdinal = ConsumedOrdinal;
	OrphanEpoch = 0;
	CurrentReconciliationStamp = 0;
	if (!Storage)
		return;
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
	{
		FRow& Row = *Storage->Index[Storage->Arrival[Index]].Row;
		if (Row.Value.bTerminalKnown || Row.Value.bSealed)
			continue;
		Row.Value.BirthMetadataOrdinal = ConsumedOrdinal;
		Row.PresenceStamp = 0;
	}
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
		return;
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
		Storage->Index[Index].RequestId = 0;
		Storage->Index[Index].State = FStorage::EIndexState::Empty;
	}
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
		Storage->Arrival[Index] = INDEX_NONE;
	for (int32 Index = 0; Index < Storage->NumClients; ++Index)
		Storage->Clients[Index].Reset();
	Storage->NumRows = 0;
	Storage->NumClients = 0;
	CurrentReconciliationStamp = 0;
	OrphanEpoch = 0;
	ResetRetainedIds();
	Increment(RevisionValue);
}
uint64 FStore::Revision() const
{
	check(IsInGameThread());
	return RevisionValue;
}
FStringView FStore::ClientLabel(uint64 ClientId) const
{
	if (!Storage)
		return {};
	for (int32 Index = 0; Index < Storage->NumClients; ++Index)
	{
		const FClientEntry& Client = Storage->Clients[Index];
		if (Client.ClientId == ClientId && Client.Label)
			return {Client.Label, Client.Chars};
	}
	return {};
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
bool FStore::TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const
{
	check(IsInGameThread());
	if (!Storage || !Budget || !Budget->Lifetime || !HasInspectorControllerCalibration<FRow>(*Budget->Lifetime) ||
		!HasInspectorControllerCalibration<FDispatchHeaderOwner>(*Budget->Lifetime) ||
		!HasInspectorControllerCalibration<FVaRestInspectorSnapshot>(*Budget->Lifetime))
		return false;
	FSnapshotBuilder Builder;
	if (!Builder.Begin(*this, Storage->NumRows))
		return false;
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
	{
		const FRow& Row = *Storage->Index[Storage->Arrival[Index]].Row;
		const FStringView Label = ClientLabel(Row.Value.ClientId);
		if (ShouldShareRow(Row))
		{
			if (!Builder.MeasureSealedRow(Label))
				return false;
		}
		else if (!Builder.MeasureMutableRow(Label) || !MeasureMutableSnapshot(Builder, Row))
			return false;
	}
	if (!Builder.FinishMeasurement() || !Builder.Reserve())
		return false;
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
	{
		const auto& Entry = Storage->Index[Storage->Arrival[Index]];
		const FRow& Row = *Entry.Row;
		const FStringView Label = ClientLabel(Row.Value.ClientId);
		if (ShouldShareRow(Row))
		{
			TSharedPtr<const FRow, ESPMode::ThreadSafe> Owner = Entry.Row;
			if (!Builder.ConstructSealedEntry(Owner, Label))
				return false;
		}
		else
		{
			FSnapshotRowCopy* Copy = Builder.ConstructMutableRow();
			if (!Copy || !ConstructMutableSnapshot(Builder, Row, *Copy) || !Builder.ConstructMutableEntry(Copy, Label))
				return false;
		}
	}
	return Builder.Publish(Out);
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
	bEnumerating = true;
	ON_SCOPE_EXIT
	{
		bEnumerating = false;
		const_cast<FStore*>(this)->FlushDeferred();
	};
	for (int32 Index = 0; Index < Storage->NumRows; ++Index)
	{
		const FRow& Row = *Storage->Index[Storage->Arrival[Index]].Row;
		VisitImmutableRow(Row, ClientLabel(Row.Value.ClientId), Visitor);
	}
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
