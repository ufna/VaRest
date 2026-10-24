// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "Templates/UniquePtr.h"
#include "VaRestInspectorBudget.h"
#include "VaRestInspectorPacket.h"
#include "VaRestInspectorRecords.h"

class IVaRestClock;

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
class FStorePacketOwner;
struct FStorePacketDeleter
{
	void operator()(FStorePacketOwner* Object) const;
};
using FStorePacketPtr = TUniquePtr<FStorePacketOwner, FStorePacketDeleter>;
class FStorePacketOwner
{
public:
	static TOptional<int64> Charge(const FPacket& Packet, const FAllocationAdapter& Allocator);
	static bool CanTake(const FPacket& Packet, const FBudgetAccount* Budget, const FAllocationAdapter& Allocator);
	static bool TryTake(FPacket& InOut, TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator, FStorePacketPtr& Out);
	const FPacket& Get() const { return Data; }
	FVaRestInspectorPacketView View() const;

private:
	friend struct FStorePacketDeleter;
	FStorePacketOwner(FStoreCredit&& InCredit, FPacket&& InData, FAllocationAdapter& InAllocator);
	FStoreCredit Credit;
	FPacket Data;
	FAllocationAdapter* Allocator;
};

// Published only as const after the existing independently charged packet
// transfer succeeds. Equal headers share this one real packet/backing owner.
class FDispatchHeaderOwner
{
public:
	FDispatchHeaderOwner() = default;
	static bool TryTake(FPacket& InOut, TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator,
		TSharedPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe>& Out);
	FVaRestInspectorPacketView View() const;
	bool Matches(const FPacket& Other) const;

private:
	FStorePacketPtr Packet;
};

struct FDispatchSlot;
struct FDispatchSlotDeleter
{
	void operator()(FDispatchSlot* Slot) const;
};
using FDispatchSlotPtr = TUniquePtr<FDispatchSlot, FDispatchSlotDeleter>;
// One allocation per admitted slot; the row contains only sixteen pointers.
struct FDispatchSlot
{
	explicit FDispatchSlot(FStoreCredit&& InCredit) : Credit(MoveTemp(InCredit)) {}
	FStoreCredit Credit;
	uint64 DispatchId = 0;
	int32 Attempt = 0;
	bool bHasDispatched = false, bHasEnded = false;
	FPacketFixed DispatchedFixed, EndedFixed;
	EVaRestInspectorHeadersState HeadersState = EVaRestInspectorHeadersState::Unknown;
	EVaRestInspectorDetailState ResponseExcerptState = EVaRestInspectorDetailState::Absent;
	TSharedPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe> Headers;
	FStorePacketPtr Ended, ResponseExcerpt;
};

class FSnapshotBuilder;
struct FSnapshotRowCopy;
#if WITH_DEV_AUTOMATION_TESTS
namespace SnapshotTesting
{
struct FStoreAccess;
}
#endif

class FStore
{
public:
	// Allocation-free GT shell. Task11 pre-admits production wrapper/root storage.
	FStore(const FNormalizedLimits& InLimits, TSharedRef<IVaRestClock, ESPMode::ThreadSafe> InClock);
	~FStore();
	FStore(const FStore&) = delete;
	FStore& operator=(const FStore&) = delete;
	static bool CalibrateRows(FAllocationLifetime& Lifetime, std::atomic<int64>& StartupUsed, int64 StartupLimit);
	bool Initialize(TRefCountPtr<FBudgetAccount> InAccount);
	bool Apply(FPacket&& Packet);
	bool ApplyAttached(const FVaRestTraceSnapshot& Snapshot, const FCapturePolicy& Policy,
		TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator);
	void BeginGeneration(uint64 InGeneration, uint64 ConsumedOrdinal);
	void Clear();
	uint64 Revision() const;
	bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const;
	bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const;
	FVaRestInspectorDiagnostics Diagnostics() const;

private:
	friend class FSnapshotBuilder;
	friend class ::FVaRestInspectorSnapshot;
#if WITH_DEV_AUTOMATION_TESTS
	friend struct SnapshotTesting::FStoreAccess;
#endif
	enum class EAttachedBase : uint8
	{
		Refused,
		Terminal,
		Mutable
	};
	enum class ELivenessEvidence : uint8
	{
		None,
		Started,
		Snapshot
	};
	enum class EExcerptApplication : uint8
	{
		Resolved,
		Refused,
		Ignored
	};
	struct FRow;
	struct FStorage;
	struct FStorageDeleter
	{
		void operator()(FStorage* Storage) const;
	};
	FRow* Find(uint64 RequestId) const;
	FRow* AdmitRow(uint64 RequestId, bool bPartial);
	FDispatchSlot* FindDispatch(FRow& Row, const FPacketScalars& Scalars) const;
	FDispatchSlot* AdmitDispatch(FRow& Row, const FPacketScalars& Scalars);
	void ApplyPhysical(FRow& Row, FPacket& Packet);
	EExcerptApplication ApplyExcerpt(FRow& Row, FPacket& Packet, EVaRestInspectorDetailState& OutDetail);
	void ApplyCompletion(FRow& Row, FPacket& Packet);
	EVaRestInspectorDetailState ApplyNote(FRow& Row, FPacket& Packet, uint64& OutNoteSerial);
	bool EnsureNoteStorage(FRow& Row);
	bool EnsureTimelineStorage(FRow& Row);
	void AppendTimeline(FRow& Row, const FVaRestInspectorTimelineEntry& Entry, uint64 NoteSerial = 0);
	void InvalidateNoteTimeline(FRow& Row, uint64 NoteSerial);
	EAttachedBase PrepareAttachedBase(uint64 RequestId, bool& bNeedsStarted);
	void AdoptAttachedPacket(uint64 RequestId, FStorePacketPtr&& Packet);
	void NoteAttachLoss(uint64 RequestId);
	bool ApplyAttachedStarted(const FVaRestTraceStarted& Borrowed, const FCapturePolicy& Policy,
		TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator);
	void SetStartedScalars(FRow& Row, const FPacketScalars& Scalars);
	void Touch(FRow& Row);
	void CompleteMutation(FRow& Row);
	static bool HasPendingExcerpt(const FRow& Row);
	static bool CanSeal(const FRow& Row);
	static bool ShouldShareRow(const FRow& Row);
	void AddDeadline(FRow& Row);
	void RemoveDeadline(FRow& Row);
	int32 ExpirePendingExcerpts();
	void StopPendingExcerpts();
	bool RemoveEligibleRow(uint64 RequestId);
	bool RemoveEligibleRowNow(int32 IndexSlot);
	void FlushDeferred();
	void FlushDeferredRemovals();
	bool IsIdRetained(uint64 RequestId) const;
	void RetainRemovedId(uint64 RequestId);
	void ResetRetainedIds();
	int32 IndexCapacity() const;
	int32 RetainedIdCapacity() const;
	void ApplyClient(uint64 ClientId, uint64 ScopeId, bool bCreated, FStringView Label);
	FStringView ClientLabel(uint64 ClientId) const;
	bool MeasureMutableSnapshot(FSnapshotBuilder& Builder, const FRow& Row) const;
	bool ConstructMutableSnapshot(FSnapshotBuilder& Builder, const FRow& Row, FSnapshotRowCopy& Copy) const;
	static void VisitImmutableRow(const FRow& Row, FStringView Label, TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor);
	const FNormalizedLimits Limits;
	TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock;
	TRefCountPtr<FBudgetAccount> Budget;
	TUniquePtr<FStorage, FStorageDeleter> Storage;
	FVaRestInspectorDiagnostics State;
	uint64 Generation = 0, LastConsumedMetadataOrdinal = 0, CurrentReconciliationStamp = 0;
	uint64 OrphanEpoch = 0, RevisionValue = 0, NextArrival = 0;
	uint64 DeferredGeneration = 0, DeferredConsumedOrdinal = 0;
	mutable bool bEnumerating = false;
	mutable bool bClearPending = false;
	bool bGenerationPending = false, bStopPending = false, bExpiryPending = false;
};
} // namespace VaRest::Inspector::Private
#endif
