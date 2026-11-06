// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

namespace VaRest::Http
{
class FClientAdmissionGate;
class FRequestSpec;
class FNativeLifetimeControl;
class FNativeClientSlots;

/** Move-only access authority; Control, not a notification or this token, owns the whole consumed spec. */
class FNativeInputBorrow final
{
public:
	FNativeInputBorrow() = default;
	~FNativeInputBorrow();
	FNativeInputBorrow(FNativeInputBorrow&& Other) noexcept;
	FNativeInputBorrow& operator=(FNativeInputBorrow&& Other) noexcept;
	FNativeInputBorrow(const FNativeInputBorrow&) = delete;
	FNativeInputBorrow& operator=(const FNativeInputBorrow&) = delete;

	bool IsValid() const { return Input != nullptr; }
	bool IsUnpublishedSenderFor(const FNativeLifetimeControl* Expected) const;
	FRequestSpec& Get() const;
	/** Sender-only, before Ready/any external publication: reserves the initial native handoff's access. */
	FNativeInputBorrow ForkBeforePublication() const;
	/** Sender-only; all SENDER payload writes precede this handoff. Later GTK/kernel access requires its existing borrow. */
	bool PublishReady();
	/** After all future payload access has been barred. Other existing borrows still prevent GT deletion. */
	void Finish(bool bDiscarded = false);
	void Reset();

private:
	friend class FNativeLifetimeControl;
	static constexpr uint64 ClosedBit = uint64(1) << 63;
	static constexpr uint64 CountMask = ClosedBit - 1;
	FNativeInputBorrow(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InControl, FRequestSpec* InInput, bool bInSender);
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	FRequestSpec* Input = nullptr;
	bool bSender = false;
};

/**
 * Native Control owns fixed client/operation storage and the whole-input disposal inventory. The module-owned
 * factory, service/ticker, complete cancellation/queue obligations and public Send routing remain separate work.
 * Its transient whole-spec inventory is explicitly UNBOUNDED and is separate from admitted operation-cell limits.
 * GT alone deletes payload; stopped unresolved inputs remain in this same Control's process-exit self-hold.
 */
class FNativeLifetimeControl final : public TSharedFromThis<FNativeLifetimeControl, ESPMode::ThreadSafe>
{
public:
	static TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Create();
	~FNativeLifetimeControl();
	/** Any thread, before the first consuming mutation. Adoption uses the existing spec allocation, with no permit. */
	FNativeInputBorrow AdoptInput(TUniquePtr<FRequestSpec>& Input);
	FNativeInputBorrow BorrowInputOnGT(const FNativeInputBorrow& Existing);
	/** Bounded scan work, not a bound on pending payload count/bytes or a promise of synchronous disposal. */
	uint32 CollectInputsOnGT(uint32 Budget = 256);
	/** Fixed client/storage ownership. Publication/queue/service wiring remains private and separate. */
	FNativeClientSlots& ClientSlotsOnGT();
	/** Seals owned slots before Stopped; full module cancellation/queue shutdown still belongs to its owner. */
	void StopInputsOnGT();
	bool IsStopped() const { return bStopped.load(std::memory_order_seq_cst); }
	uint64 NumPendingInputs() const { return RegisteredInputs.load(std::memory_order_seq_cst); }
	bool IsExitRetentionRequested() const { return bExitHoldClaimed.load(std::memory_order_acquire); }
#if WITH_DEV_AUTOMATION_TESTS
	/** Fixture cleanup only: all producers/borrows are quiescent, no later adoption. Production never clears its hold. */
	bool DropQuiescentExitHoldForTests();
	uint32 LastInputScanVisitsForTests() const
	{
		check(IsInGameThread());
		return LastInputScanVisits;
	}
#endif

private:
	friend class FNativeInputBorrow;
	FNativeLifetimeControl();
	static bool TryBorrowInput(FRequestSpec* Input);
	void EnsureExitHold();
	void AppendDeferred(FRequestSpec* Input);
	std::atomic<bool> bStopped{false};
	std::atomic<uint64> RegisteredInputs{0};
	std::atomic<FRequestSpec*> Incoming{nullptr};
	std::atomic<bool> bExitHoldClaimed{false};
	// Exactly one writer wins bExitHoldClaimed, holding its own shared reference through installation. No live reset.
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> ExitSelfHold;
	TUniquePtr<FNativeClientSlots> ClientSlots;
	FRequestSpec* IncomingCursor = nullptr; // GT only
	FRequestSpec* DeferredHead = nullptr;
	FRequestSpec* DeferredTail = nullptr;
	bool bPreferDeferred = false;
	bool bCollecting = false;
#if WITH_DEV_AUTOMATION_TESTS
	uint32 LastInputScanVisits = 0;
#endif
};

/**
 * One counted admission, moved into the managed operation's settlement record before publishing any work.
 * Release is atomic-only on any thread. The combined logical/physical settlement predicate owns that release;
 * destroying a request handle or seeing Completed is not permission to release an admitted operation.
 */
class FClientWorkLease final
{
public:
	FClientWorkLease() = default;
	~FClientWorkLease();
	FClientWorkLease(FClientWorkLease&& Other) noexcept;
	FClientWorkLease& operator=(FClientWorkLease&& Other) noexcept;
	FClientWorkLease(const FClientWorkLease&) = delete;
	FClientWorkLease& operator=(const FClientWorkLease&) = delete;

	bool IsHeld() const { return Gate.IsValid(); }
	bool BelongsTo(const FClientAdmissionGate* Expected) const { return Gate.IsValid() && Gate.Get() == Expected; }
	void Release();

private:
	friend class FClientAdmissionGate;
	TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
};

/** One irreversible admission decision shared by every handle of one opted-in managed native client. */
class FClientAdmissionGate final : public TSharedFromThis<FClientAdmissionGate, ESPMode::ThreadSafe>
{
public:
	static constexpr uint64 ClosedBit = uint64(1) << 63;
	static constexpr uint64 CountMask = ClosedBit - 1;
	static constexpr uint32 MaxOwnedOperations = 1024;

	/** Any thread. Called before current config reads, exchange/identity allocation, or Enter publication. */
	bool TryAcquire(FClientWorkLease& OutLease);
	/** GT, idempotent. Shares one atomic modification order with TryAcquire; it can never reopen. */
	void ClosePermanently();
	bool IsClosed() const;
	uint32 NumOwnedOperations() const;
	uint64 NumOwnedInputs() const;

private:
	friend class FClientWorkLease;
	friend class FNativeLifetimeControl;
	bool TryCountInput();
	void ReleaseInput();
	std::atomic<uint64> Word{0};
	std::atomic<uint64> InputWord{0};
	std::atomic<bool> bInputCountOverflow{false};
};

/**
 * Atomic settlement payload of one preadmitted operation cell. The fixed Control storage and exact-generation token
 * authorities must keep it alive through every method call; zero counters alone do not authorize cell reclamation.
 * Child acquisition/terminal selection are GT-only. Workers only settle an already acquired tail/dispatch authority.
 */
class FManagedOperationSettlement final
{
public:
	static constexpr uint32 MaxLogicalTails = 64;
	static constexpr uint32 MaxDispatchSettlements = 32;

	/** Before publication. Initial handoff/completion/delivery reservations are included in the same 64-tail cap. */
	explicit FManagedOperationSettlement(FClientWorkLease&& Work, uint32 InitialTails = 1);
	~FManagedOperationSettlement();
	FManagedOperationSettlement(const FManagedOperationSettlement&) = delete;
	FManagedOperationSettlement& operator=(const FManagedOperationSettlement&) = delete;

	bool TryAddLogicalTail();
	bool TryAddDispatchSettlement();
	void CloseChildAdmission();
	void MarkLogicalTerminalSettled();
	/** Only after the captured Control is known Stopped; never a normal result/Completed publication. */
	void MarkDiscardedAtStop();
	/** Any thread, once per previously acquired native authority. No UObject/service/task work is performed. */
	void SettleLogicalTail();
	void SettleDispatch();

	uint32 NumLogicalTails() const { return PendingLogicalTails.load(std::memory_order_seq_cst); }
	uint32 NumDispatchSettlements() const { return PendingDispatchSettlements.load(std::memory_order_seq_cst); }
	bool WasDiscardedAtStop() const { return bDiscardedAtStop.load(std::memory_order_seq_cst); }
	/** A claim is not a reclamation fence: Control must also observe all token authorities consumed. */
	bool WasWorkReleaseClaimed() const { return bWorkLeaseReleased.load(std::memory_order_seq_cst); }

private:
	void TryReleaseWorkLease();
	FClientWorkLease AdmittedWork;
	std::atomic<uint32> PendingLogicalTails{1};
	std::atomic<uint32> PendingDispatchSettlements{0};
	std::atomic<bool> bChildAdmissionClosed{false};
	std::atomic<bool> bLogicalTerminalSettled{false};
	std::atomic<bool> bDiscardedAtStop{false};
	std::atomic<bool> bWorkLeaseReleased{false};
};
} // namespace VaRest::Http
