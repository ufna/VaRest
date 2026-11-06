// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeLifetime.h"
#include <optional>

namespace VaRest::Http
{
class FNativeOperationStore;
class FManagedOperationPayload;
class FManagedCompletionAccess;

struct FNativeOperationHandle
{
	uint16 Index = 0;
	uint64 Generation = 0;
	bool IsValid() const { return Generation != 0; }
};

/** Identity alone conveys no cell access authority. A stale identity must never dereference the cell payload. */
struct FNativeOperationRef
{
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	FNativeOperationStore* Store = nullptr;
	FNativeOperationHandle Cell;
	bool IsValid() const { return Control.IsValid() && Store && Cell.IsValid(); }
};

class FNativeLogicalTailLease final
{
public:
	FNativeLogicalTailLease() = default;
	~FNativeLogicalTailLease();
	FNativeLogicalTailLease(FNativeLogicalTailLease&& Other) noexcept;
	FNativeLogicalTailLease& operator=(FNativeLogicalTailLease&& Other) noexcept;
	FNativeLogicalTailLease(const FNativeLogicalTailLease&) = delete;
	FNativeLogicalTailLease& operator=(const FNativeLogicalTailLease&) = delete;
	bool IsHeld() const { return Ref.IsValid(); }
	const FNativeOperationRef& Reference() const { return Ref; }
	/** GT only. Caller must keep THIS actual authority held through every use of the returned payload pointer. */
	FManagedOperationPayload* ManagedPayloadOnGT() const;
	void Reset();

private:
	friend class FNativeOperationStore;
	friend class FNativeOperationReservation;
	explicit FNativeLogicalTailLease(FNativeOperationRef InRef)
		: Ref(MoveTemp(InRef))
	{
	}
	FNativeOperationRef Ref;
};

class FNativeDispatchSettlementLease final
{
public:
	FNativeDispatchSettlementLease() = default;
	~FNativeDispatchSettlementLease();
	FNativeDispatchSettlementLease(FNativeDispatchSettlementLease&& Other) noexcept;
	FNativeDispatchSettlementLease& operator=(FNativeDispatchSettlementLease&& Other) noexcept;
	FNativeDispatchSettlementLease(const FNativeDispatchSettlementLease&) = delete;
	FNativeDispatchSettlementLease& operator=(const FNativeDispatchSettlementLease&) = delete;
	bool IsHeld() const { return Ref.IsValid(); }
	void Reset();

private:
	friend class FNativeOperationStore;
	explicit FNativeDispatchSettlementLease(FNativeOperationRef InRef)
		: Ref(MoveTemp(InRef))
	{
	}
	FNativeOperationRef Ref;
};

/** Exclusive Claim owner. Sender disposal precedes abort publication; GT never reads a Claim's payload. */
class FNativeOperationReservation final
{
public:
	FNativeOperationReservation() = default;
	~FNativeOperationReservation();
	FNativeOperationReservation(FNativeOperationReservation&& Other) noexcept;
	FNativeOperationReservation& operator=(FNativeOperationReservation&& Other) noexcept;
	FNativeOperationReservation(const FNativeOperationReservation&) = delete;
	FNativeOperationReservation& operator=(const FNativeOperationReservation&) = delete;
	bool IsValid() const { return Ref.IsValid(); }
	FRequestSpec& Input() const { return Sender.Get(); }
	FNativeInputBorrow ForkInputAnchor() const { return Sender.ForkBeforePublication(); }
	/** Before moving any affine owner into the exchange; callback/captures are invoked/destroyed only on GT. */
	void SetReleaseAnchor(TUniqueFunction<void(bool bDiscarded)>&& Anchor);
	/** Once while Claimed, before affine moves. Null if Stop bars the input anchor; caller then aborts the claim. */
	FManagedOperationPayload* CreateManagedPayload();
	/** Primitive one-tail publication. Managed Send uses the overload with separate completion/delivery authority. */
	FNativeLogicalTailLease Publish();
	/** Returns Handoff and fills Completion/optional worker. Different empty outputs; aliasing preserves the claim/owners. */
	FNativeLogicalTailLease Publish(FNativeLogicalTailLease& OutCompletion, FNativeLogicalTailLease& OutDeliveryWorker);
	/** Managed Send only: transfer an ALREADY counted/claimed operation even if closure sealed new admission meanwhile. */
	FNativeLogicalTailLease PublishAdmitted(FNativeLogicalTailLease& OutCompletion, FNativeLogicalTailLease& OutDeliveryWorker);
	/** Actual managed Send: install SAME initial Completion's native shared/weak cancellation route before publication. */
	FNativeLogicalTailLease PublishManaged();
	void Abort();

private:
	friend class FNativeOperationStore;
	void SetManagedPayload(TUniquePtr<FManagedOperationPayload>&& Payload);
	FNativeLogicalTailLease PublishInitial(FNativeLogicalTailLease* OutCompletion, FNativeLogicalTailLease* OutDeliveryWorker);
	FNativeOperationRef Ref;
	FNativeInputBorrow Sender;
};

struct FNativeCellSweepResult
{
	uint32 Visited = 0;
	uint32 Reclaimed = 0;
};

/**
 * One fixed per-client array, allocated on GT before managed publication. Control must retain the store allocation
 * while any reference can address it; recycling a client slot keeps this array's monotonic cell generations.
 * The module-owned Control/slot/gate/factory and queue integration are separate from this storage primitive.
 */
class FNativeOperationStore final
{
public:
	static constexpr uint32 Capacity = FClientAdmissionGate::MaxOwnedOperations;
	FNativeOperationStore(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InOwner,
		TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> InGate);
	~FNativeOperationStore();
	FNativeOperationStore(const FNativeOperationStore&) = delete;
	FNativeOperationStore& operator=(const FNativeOperationStore&) = delete;

	/** Any thread after matching gate CAS. Reserve 1 primitive tail or 2/3 managed initial tails; refusal moves nothing. */
	bool TryClaim(FClientWorkLease& Work, FNativeInputBorrow& Sender, FNativeOperationReservation& Out, uint32 InitialTails = 1);
	FNativeLogicalTailLease TryAddLogicalTailOnGT(const FNativeOperationRef& Ref);
	FNativeDispatchSettlementLease TryAddDispatchOnGT(const FNativeOperationRef& Ref);
	/** GTK-only kernel entry: transfer actual parked initial completion BEFORE any callout, without acquiring another tail. */
	bool TakeInitialCompletionOnGT(const FNativeOperationRef& Ref, FNativeLogicalTailLease& Completion, FNativeLogicalTailLease& DeliveryWorker);
	/** Actual native shared form; neither selector acquires a child, and both share the same once-selection flag. */
	bool TakeManagedCompletionOnGT(const FNativeOperationRef& Ref, TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe>& Completion,
		FNativeLogicalTailLease& DeliveryWorker);
	/** Final outcome/decode selection bars new children BEFORE real terminal bookkeeping/delivery has finished. */
	bool CloseChildrenOnGT(const FNativeOperationRef& Ref);
	bool MarkTerminalOnGT(const FNativeOperationRef& Ref);
	/** Before captured Control becomes Stopped: bars every GT child/new-operation acquisition. */
	void SealOnGT();
	/** Only after the old client gate is closed/zero and every old cell/authority was reclaimed. */
	bool ResetForNextClientOnGT(TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> NewGate);
	FNativeCellSweepResult SweepOnGT(uint32 Budget);
	uint32 NumOccupied() const { return Occupied.load(std::memory_order_acquire); }
#if WITH_DEV_AUTOMATION_TESTS
	void SeedFreeGenerationsForTests(uint64 Generation);
#endif

private:
	friend class FNativeLogicalTailLease;
	friend class FNativeDispatchSettlementLease;
	friend class FNativeOperationReservation;
	enum class ECellState : uint8
	{
		Free,
		Claimed,
		Published,
		Reclaiming
	};
	static constexpr uint64 MaxGeneration = MAX_uint64 >> 2;
	static constexpr uint64 Tag(uint64 Generation, ECellState State) { return (Generation << 2) | uint64(State); }
	struct FCell
	{
		std::atomic<uint64> State{Tag(1, ECellState::Free)};
		std::atomic<uint64> Authorities{0};
		std::optional<FManagedOperationSettlement> Settlement;
		TUniqueFunction<void(bool)> ReleaseAnchor;
		FManagedOperationPayload* ManagedPayload = nullptr; // nonowning; ReleaseAnchor is the sole unique owner
		uint32 InitialTails = 0;                            // immutable while Claimed/Published; Abort consumes every unexposed reservation
		bool bAborted = false;
	};
	FCell* FindPublished(FNativeOperationHandle Handle) const;
	FCell& OwnClaim(FNativeOperationHandle Handle) const;
	bool Matches(const FNativeOperationRef& Ref) const;
	void PublishClaim(FNativeOperationHandle Handle);
	void AbortClaim(FNativeOperationHandle Handle, bool bStopped);
	void SettleLogical(FNativeOperationHandle Handle, bool bStopped);
	void SettlePhysical(FNativeOperationHandle Handle, bool bStopped);
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Owner;
	TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate; // GT reset only after old closed gate/count/cells are zero
	TUniquePtr<FCell[]> Cells;
	std::atomic<uint32> NextClaim{0};
	std::atomic<uint32> Occupied{0};
	std::atomic<bool> bSealed{false};
	uint32 SweepCursor = 0;
	bool bSweeping = false;
};
} // namespace VaRest::Http
