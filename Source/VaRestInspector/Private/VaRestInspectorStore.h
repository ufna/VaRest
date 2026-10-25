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
	static bool TryCompactStarted(const FStorePacketOwner& Source, uint64 ExpectedRequestId,
		TRefCountPtr<FBudgetAccount> Budget, FAllocationAdapter& Allocator, FStorePacketPtr& Out);
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

struct FStatsKeyView
{
	uint64 ScopeId = 0, ClientId = 0;
	FName Endpoint;
	FStringView Key;
};
struct FStatsErrorValues
{
	uint64 Canceled = 0, Connection = 0, Timeout = 0, Http = 0, Server = 0;
	uint64 Decode = 0, Auth = 0, Offline = 0, Config = 0, Unknown = 0;
	bool bSaturated = false;
};
struct FStatsGroup
{
	uint64 ScopeId = 0, ClientId = 0;
	FName Endpoint;
	bool bOverflow = false, bSaturated = false, bWireBytesUnknown = false;
	uint64 Started = 0, Completed = 0, Success = 0, Error = 0, Queued = 0, Canceled = 0;
	uint64 Dispatches = 0, Retries = 0, AuthReplays = 0, RetryScheduled = 0;
	uint64 UnknownDispatchClassification = 0;
	uint64 CacheHit = 0, CacheDisk = 0, Revalidated = 0, Stale = 0, Replayed = 0;
	uint64 WireBytesUp = 0, WireBytesDown = 0, Partial = 0, Incomplete = 0, Dropped = 0;
	FStatsErrorValues ErrorsByKind;
	uint64 Histogram[512] = {};
	uint64 DurationCount = 0, DurationUnderflow = 0, DurationOverflow = 0;
	double DurationSum = 0, DurationMin = 0, DurationMax = 0;
};
struct FTicketedText
{
	const TCHAR* Data = nullptr;
	int32 Chars = 0;
	FStoreCredit Credit;
	FTicketedText() = default;
	FTicketedText(const FTicketedText&) = delete;
	FTicketedText& operator=(const FTicketedText&) = delete;
	FTicketedText(FTicketedText&& Other) noexcept
		: Data(Other.Data)
		, Chars(Other.Chars)
		, Credit(MoveTemp(Other.Credit))
	{
		Other.Data = nullptr;
		Other.Chars = 0;
	}
	FTicketedText& operator=(FTicketedText&& Other) noexcept
	{
		if (this != &Other)
		{
			Reset();
			Data = Other.Data;
			Chars = Other.Chars;
			Credit = MoveTemp(Other.Credit);
			Other.Data = nullptr;
			Other.Chars = 0;
		}
		return *this;
	}
	~FTicketedText() { Reset(); }
	FStringView View() const { return Data ? FStringView(Data, Chars) : FStringView(); }
	void Reset();
};
struct FStatsGroupSlot
{
	FStoreCredit ObjectCredit;
	FStatsGroup Value;
	FTicketedText Key;
	int32 IndexSlot = INDEX_NONE;
};
struct FStatsGroupSlotDeleter
{
	void operator()(FStatsGroupSlot* Slot) const;
};
using FStatsGroupSlotPtr = TUniquePtr<FStatsGroupSlot, FStatsGroupSlotDeleter>;
struct FSnapshotStatsGroup
{
	FStatsGroup Value;
	const TCHAR* Key = nullptr;
	int32 KeyChars = 0;
};
struct FLinkSeed
{
	uint64 FromRequestId = 0, ToRequestId = 0;
	FStringView QueueId;
	bool bAuth = false;
};
struct FStoreLinkView
{
	uint64 FromRequestId = 0, ToRequestId = 0;
	FStringView QueueId;
	bool bAuth = false, bTargetEvicted = false;
};
enum class ELinkAdmission : uint8
{
	Added,
	AlreadyPresent,
	ReplacedOldest,
	Refused
};
struct FAdmissionAnchors
{
	int32 MutationIndexSlot = INDEX_NONE;
	int32 BorrowedSourceIndexSlot = INDEX_NONE;
};
struct FAdmissionRequest
{
	int32 NewRows = 0;
	int64 NewBytes = 0;
	FAdmissionAnchors Anchors;
};
struct FStoreStoragePlan
{
	int32 IndexCapacity = 0, ArrivalCapacity = 0, MaxRows = 0;
	int32 ClientCapacity = 0, ClientLookupCapacity = 0;
	int32 DeadlineCapacity = 0;
	int32 RetainedCapacity = 0, RetainedLookupCapacity = 0;
	int32 StatsGroupCapacity = 0, StatsIndexCapacity = 0;
	int32 LinkCapacity = 0, LinkIndexCapacity = 0, LinkTargetCapacity = 0;
	int64 FixedBytes = 0, MinimumHttpBytes = 0;
	bool bSupported = false, bFits = false;
};
enum class ERowKeyKind : uint8
{
	Http,
	CoreMarker,
	CaptureOverflow,
	DerivedQueueExpired
};
struct FRowKey
{
	FRowKey() = default;
	FRowKey(uint64 InId) : Id(InId) {}
	FRowKey(ERowKeyKind InKind, uint64 InId) : Kind(InKind), Id(InId) {}
	ERowKeyKind Kind = ERowKeyKind::Http;
	uint64 Id = 0;
};

class FSnapshotBuilder;
struct FSnapshotRowCopy;
#if WITH_DEV_AUTOMATION_TESTS
namespace SnapshotTesting
{
struct FStoreAccess;
}
#endif

#if WITH_DEV_AUTOMATION_TESTS
namespace BudgetTesting
{
struct FStoreAccess;
}
namespace LinksTesting
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
	bool BindLossWitness(TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending);
	void ObserveQueueLoss(uint64 InGeneration, const FPendingAccount& Source);
	FStatsGroup* FindOrAdmitStatsGroup(const FStatsKeyView& Key, const FAdmissionAnchors& Anchors);
	FStatsGroup* FindOrAdmitOverflowStatsGroup();
	const FStatsGroup* FindStatsGroup(const FStatsKeyView& Key) const;
	ELinkAdmission TryAdmitLink(const FLinkSeed& Seed, const FAdmissionAnchors& Anchors);
	void RequestLimits(const FNormalizedLimits& Requested);
	bool CanRecreateAccount(const FBudgetAccount& OldBudget, const FPendingAccount& OldPending) const;
	bool CompleteRecreation(const FNormalizedLimits& Effective,
		const FBudgetAccount& OldBudget, const FPendingAccount& OldPending,
		TRefCountPtr<FBudgetAccount> NewBudget, TSharedRef<FPendingAccount, ESPMode::ThreadSafe> NewPending);

private:
	friend class FSnapshotBuilder;
	friend class ::FVaRestInspectorSnapshot;
#if WITH_DEV_AUTOMATION_TESTS
	friend struct SnapshotTesting::FStoreAccess;
	friend struct BudgetTesting::FStoreAccess;
	friend struct LinksTesting::FStoreAccess;
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
	static FStoreStoragePlan MeasureStoragePlan(const FNormalizedLimits& InLimits, const FBudgetAccount& Account);
	FStoreStoragePlan EffectiveStoragePlan() const;
	int32 NumClients() const;
	int32 NumStatsGroups() const;
	int32 NumLinks() const;
	int64 RowStorageBound() const;
	int64 TimelineStorageBound() const;
	bool MeasureSnapshotCosts(int64& Outer, int64& Nested) const;
	bool HasLink(const FLinkSeed& Seed) const;
	int32 NumLinksForTarget(uint64 RequestId) const;
	bool InitializeStorage(TRefCountPtr<FBudgetAccount> InAccount);
	void ForEachRow(TFunctionRef<void(FRow&)> Visitor) const;
	FRow* Find(uint64 RequestId) const;
	FRow* FindRow(const FRowKey& Key) const;
	FRow* AdmitRow(const FRowKey& Key, bool bPartial, const FAdmissionAnchors& Anchors = {});
	bool TakePacket(FRow& Anchor, FPacket& Packet, FStorePacketPtr& Out);
	bool TakeHeader(FRow& Anchor, FPacket& Packet, TSharedPtr<const FDispatchHeaderOwner, ESPMode::ThreadSafe>& Out);
	bool MakeAdmissionRoom(const FAdmissionRequest& Request);
	bool TrimPinnedDetails(FRow& Row);
	static bool CanReleaseWhole(const FRow& Row);
	FDispatchSlot* FindDispatch(FRow& Row, const FPacketScalars& Scalars) const;
	FDispatchSlot* AdmitDispatch(FRow& Row, const FPacketScalars& Scalars);
	void ApplyPhysical(FRow& Row, FPacket& Packet);
	EExcerptApplication ApplyExcerpt(FRow& Row, FPacket& Packet, EVaRestInspectorDetailState& OutDetail);
	void ApplyCompletion(FRow& Row, FPacket& Packet);
	EVaRestInspectorDetailState ApplyNote(FRow& Row, FPacket& Packet, uint64& OutNoteSerial);
	bool ApplyMarker(FPacket& Packet);
	bool TryAdmitInternalMarker(const FVaRestInspectorMarkerInfo& Info, uint64 ScopeId, uint64 ClientId,
		bool bContextKnown, bool bMetadataOmitted, const FAdmissionAnchors& Anchors);
	bool TryAdmitQueueExpiredMarker(FRow& SourceRow, uint64 NoteSerial);
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
	void Touch(FRow& Row) const;
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
	void AppendArrival(FRow& Row);
	void UnlinkArrival(FRow& Row);
	void ResetArrival();
	void FlushDeferred();
	void FlushDeferredRemovals();
	bool IsIdRetained(uint64 RequestId) const;
	void RetainRemovedId(uint64 RequestId);
	void ResetRetainedIds();
	int32 IndexCapacity() const;
	int32 RetainedIdCapacity() const;
	void ApplyClient(uint64 ClientId, uint64 ScopeId, bool bCreated, FStringView Label);
	void ApplyScope(uint64 ScopeId, bool bOpened, FStringView Label, bool bMetadataOmitted = false);
	FStringView ClientLabel(uint64 ClientId, TOptional<uint64> ExpectedScope = {}) const;
	FStringView RowClientLabel(const FRow& Row) const;
	bool AdmitText(FStringView Source, const FAdmissionAnchors& Anchors, FTicketedText& Out);
	bool CopyTextAdmitted(FStringView Source, FTicketedText& Out);
	bool ValidateBorrowedSource(FStringView Source, const FAdmissionAnchors& Anchors) const;
	void ClearStats();
	void ClearLinks();
	struct FLinkTargetEntry;
	FLinkTargetEntry* FindLinkTarget(uint64 RequestId, bool bCreate);
	void RemoveLinkAt(int32 LinkSlot);
	bool MeasureMutableSnapshot(FSnapshotBuilder& Builder, const FRow& Row) const;
	bool ConstructMutableSnapshot(FSnapshotBuilder& Builder, const FRow& Row, FSnapshotRowCopy& Copy) const;
	bool MeasureSnapshot(FSnapshotBuilder& Builder) const;
	static void VisitImmutableRow(const FRow& Row, FStringView Label, TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor);
	void RefreshLossWitness() const;
	void ResetLossObservation();
	void InvalidateKnownNeverHistory() const;
	void NoteUnsupportedPublicStrings() const;
	void ReleaseFixedStorage();
	FNormalizedLimits Limits;
	FNormalizedLimits RequestedLimits;
	TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock;
	TRefCountPtr<FBudgetAccount> Budget;
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> LossWitness;
	TWeakPtr<FPendingAccount, ESPMode::ThreadSafe> OriginatingPending;
	uint64 OriginatingAccountIdentity = 0;
	TUniquePtr<FStorage, FStorageDeleter> Storage;
	mutable FVaRestInspectorDiagnostics State;
	uint64 Generation = 0, LastConsumedMetadataOrdinal = 0, CurrentReconciliationStamp = 0;
	uint64 OrphanEpoch = 0, NextArrival = 0, NextMarkerOrdinal = 0;
	uint64 DeferredGeneration = 0, DeferredConsumedOrdinal = 0, GenerationLossEpoch = 0;
	mutable uint64 RevisionValue = 0;
	bool bGenerationPending = false, bStopPending = false, bExpiryPending = false;
	bool bOriginatingPendingBound = false;
	mutable bool bLossWitnessBound = false;
	mutable bool bLossDirty = true;
	mutable bool bEnumerating = false;
	mutable bool bClearPending = false;
};
} // namespace VaRest::Inspector::Private
#endif
