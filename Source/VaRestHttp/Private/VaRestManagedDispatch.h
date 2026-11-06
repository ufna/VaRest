// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestExchangeState.h"
#include "VaRestNativeOperationStore.h"
#include <array>

namespace VaRest::Http
{
class FManagedOperationPayload;
class FManagedNativeDispatchAccess;
class FManagedDispatchSlots;

#if WITH_DEV_AUTOMATION_TESTS
/** Owned atomic rendezvous only; no callback/config/GT-affine payload is carried by this test seam. */
struct FManagedDispatchPublicationProbe
{
	std::atomic<bool> Published{false};
	std::atomic<bool> Resume{false};
};
#endif

/** The actual GT kernel bridge and its captures are owned by a slot, never by the transport callback/task. */
using FManagedAttemptDelivery = TUniqueFunction<void(FDispatchTag&&, FVaRestTransportResult&&, FVaRestOutcomeCallback&&)>;

/** Native-only guard. One physical authority covers its guard, single shared clock view and owned GT handback. */
class FManagedDispatchGuard final
{
public:
	FManagedDispatchGuard() = default;
	~FManagedDispatchGuard();
	FManagedDispatchGuard(FManagedDispatchGuard&& Other) noexcept;
	FManagedDispatchGuard& operator=(FManagedDispatchGuard&&) = delete;
	FManagedDispatchGuard(const FManagedDispatchGuard&) = delete;
	FManagedDispatchGuard& operator=(const FManagedDispatchGuard&) = delete;
	bool IsValid() const { return Access.IsValid(); }
	void Deliver(FVaRestTransportResult&& Result);

private:
	friend class FManagedDispatchPreparation;
	friend class FManagedDispatchSlots;
	explicit FManagedDispatchGuard(TSharedRef<FManagedNativeDispatchAccess, ESPMode::ThreadSafe> InAccess);
	TSharedPtr<FManagedNativeDispatchAccess, ESPMode::ThreadSafe> Access;
	bool bResolved = false;
};

/** GT pre-Start owner; its actual native guard is transferred into the transport completion exactly once. */
class FManagedDispatchPreparation final
{
public:
	FManagedDispatchPreparation() = default;
	FManagedDispatchPreparation(FManagedDispatchPreparation&&) = default;
	FManagedDispatchPreparation& operator=(FManagedDispatchPreparation&&) = delete;
	FManagedDispatchPreparation(const FManagedDispatchPreparation&) = delete;
	FManagedDispatchPreparation& operator=(const FManagedDispatchPreparation&) = delete;
	bool IsValid() const { return Guard.IsValid(); }
	/** Valid only while this preparation still holds its guard and before transferring it to Start. */
	const FDispatchTag& TagOnGT() const;
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> ClockViewOnGT() const;
	FVaRestTransportComplete TakeCompletionOnGT();
	/** Pre-Start cancellation keeps the existing inline GT outcome/trace order; caller still owns this preparation. */
	FVaRestOutcomeCallback TakeUnstartedOutcomeOnGT();
#if WITH_DEV_AUTOMATION_TESTS
	/** Before completion transfer. Pauses the real Publish AFTER result-ready and BEFORE its second stopped check. */
	void PausePublicationForTests(TSharedRef<FManagedDispatchPublicationProbe, ESPMode::ThreadSafe> Probe);
#endif

private:
	friend class FManagedDispatchSlots;
	FManagedDispatchPreparation(FManagedDispatchGuard&& InGuard, TSharedPtr<IVaRestClock, ESPMode::ThreadSafe>&& InClock);
	FManagedDispatchGuard Guard;
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> ClockView;
};

/**
 * Fixed per-operation storage, uniquely owned INSIDE the operation's GT release anchor before any preparation. Actual
 * tag/outcome/GT bridge/result/original clock live here. Native shared access owns only one real physical lease plus plain
 * identity; its copies are borrows of that same dispatch, not extra admissions or per-retry records. Ordinary dispatch
 * and public managed Send routing are separate integration work. No arbitrary caller may allocate a detached slot set.
 */
class FManagedDispatchSlots final
{
public:
	~FManagedDispatchSlots();
	FManagedDispatchSlots(const FManagedDispatchSlots&) = delete;
	FManagedDispatchSlots& operator=(const FManagedDispatchSlots&) = delete;
	/** GT. Failure consumes none of Tag/Outcome/Delivery; reserve before trace/Start. Clock is the exact original. */
	FManagedDispatchPreparation PrepareOnGT(FDispatchTag& Tag, FVaRestOutcomeCallback& Outcome,
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock, FManagedAttemptDelivery& Delivery);

private:
	friend class FManagedOperationPayload;
	friend class FManagedNativeDispatchAccess;
	explicit FManagedDispatchSlots(FNativeOperationRef InRef);
	struct FSlot
	{
		uint64 Generation = 1;
		bool bOccupied = false;
		bool bRetired = false;
		bool bExecuting = false;
		bool bDelivered = false;
		std::atomic<bool> bReady{false};
		std::atomic<bool> bAccessEnded{false};
		FDispatchTag Tag;
		FVaRestTransportResult Result;
		FVaRestOutcomeCallback Outcome;
		FManagedAttemptDelivery Delivery;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> OriginalClock;
	};
	FSlot& HeldSlot(uint8 Index, uint64 Generation);
	void DeliverOnGT(uint8 Index, uint64 Generation);
	FVaRestOutcomeCallback TakeUnstartedOnGT(uint8 Index, uint64 Generation);
	/** Existing owned sweep only; normal ready delivery is a named GT task, not a full-table polling policy. */
	void ServiceReadyOnGT(bool bStopped);
	FNativeOperationRef Ref;
	std::array<FSlot, FManagedOperationSettlement::MaxDispatchSettlements> Slots;
	bool bServicing = false;
};
} // namespace VaRest::Http
