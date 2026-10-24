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
	void ApplyAttached(const FVaRestTraceSnapshot& Snapshot, const FCapturePolicy& Policy,
		TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator);
	void BeginGeneration(uint64 InGeneration, uint64 ConsumedOrdinal);
	void Clear();
	uint64 Revision() const;
	bool TrySnapshot(TSharedPtr<const FVaRestInspectorSnapshot, ESPMode::ThreadSafe>& Out) const;
	bool ForEachRecord(TFunctionRef<void(const FVaRestInspectorRecordView&)> Visitor) const;
	FVaRestInspectorDiagnostics Diagnostics() const;

private:
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
	struct FRow;
	struct FStorage;
	struct FStorageDeleter
	{
		void operator()(FStorage* Storage) const;
	};
	FRow* Find(uint64 RequestId) const;
	FRow* AdmitRow(uint64 RequestId, bool bPartial);
	EAttachedBase PrepareAttachedBase(uint64 RequestId, bool& bNeedsStarted);
	void AdoptAttachedPacket(uint64 RequestId, FStorePacketPtr&& Packet);
	void NoteAttachLoss(uint64 RequestId);
	bool ApplyAttachedStarted(const FVaRestTraceStarted& Borrowed, const FCapturePolicy& Policy,
		TSharedRef<FPendingAccount, ESPMode::ThreadSafe> Pending, FAllocationAdapter& Allocator);
	void SetStartedScalars(FRow& Row, const FPacketScalars& Scalars);
	void Touch(FRow& Row);
	void ApplyClient(uint64 ClientId, uint64 ScopeId, bool bCreated, FStringView Label);
	const FNormalizedLimits Limits;
	TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock;
	TRefCountPtr<FBudgetAccount> Budget;
	TUniquePtr<FStorage, FStorageDeleter> Storage;
	FVaRestInspectorDiagnostics State;
	uint64 Generation = 0, LastConsumedMetadataOrdinal = 0, CurrentReconciliationStamp = 0;
	uint64 OrphanEpoch = 0, RevisionValue = 0, NextArrival = 0;
	mutable bool bEnumerating = false;
	mutable bool bClearPending = false;
};
} // namespace VaRest::Inspector::Private
#endif
