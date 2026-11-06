// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeOperationStore.h"
#include "VaRestQueueFence.h"

namespace VaRest::Http
{
class FClientState;
class FNativeScopeLifetime;
class FNativeClientSlots;
class FOfflineQueue;
class FQueueBinding;
class FManagedRefreshDelivery;

/** Exclusive GT reservation for one inline queue row, retained through reentrant target validation/join. */
class FNativeQueueBindingReservation final
{
public:
	FNativeQueueBindingReservation() = default;
	~FNativeQueueBindingReservation();
	FNativeQueueBindingReservation(FNativeQueueBindingReservation&& Other) noexcept;
	FNativeQueueBindingReservation& operator=(FNativeQueueBindingReservation&& Other) noexcept;
	FNativeQueueBindingReservation(const FNativeQueueBindingReservation&) = delete;
	FNativeQueueBindingReservation& operator=(const FNativeQueueBindingReservation&) = delete;
	bool IsValid() const { return Slots != nullptr; }
	bool Commit(TSharedRef<FOfflineQueue, ESPMode::ThreadSafe> Queue, TSharedRef<FQueueBinding, ESPMode::ThreadSafe> Binding);
	/** Retains this inline position for the client's CURRENT complete target, without retaining a config. */
	bool ParkForCurrentTarget();
	void Abort();

private:
	friend class FNativeClientSlots;
	FNativeClientSlots* Slots = nullptr;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	uint16 ClientIndex = 0;
	uint64 ClientGeneration = 0, Generation = 0;
	uint8 DrainIndex = 0;
	bool bResumePending = false;
};

struct FNativeClientSlotHandle
{
	uint16 Index = 0;
	uint64 Generation = 0;
	bool IsValid() const { return Generation != 0; }
};

/** Immutable client identity, installed before Create publishes anything. It owns no affine client/config payload. */
struct FNativeClientRegistration
{
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control;
	TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
	FNativeOperationStore* Operations = nullptr;
	FNativeClientSlotHandle Client;
	bool IsValid() const { return Control.IsValid() && Gate.IsValid() && Operations && Client.IsValid(); }
};

/** GT-only construction reservation. The slot stays Reserved through every reentrant Create/trace call. */
class FNativeClientReservation final
{
public:
	FNativeClientReservation() = default;
	~FNativeClientReservation();
	FNativeClientReservation(FNativeClientReservation&& Other) noexcept;
	FNativeClientReservation& operator=(FNativeClientReservation&& Other) noexcept;
	FNativeClientReservation(const FNativeClientReservation&) = delete;
	FNativeClientReservation& operator=(const FNativeClientReservation&) = delete;
	bool IsValid() const { return Registration.IsValid(); }
	const FNativeClientRegistration& InitialRegistration() const { return Registration; }
	/** After private initial config/state construction; refusal still has owned reservation unwind. */
	bool CommitClient(TSharedRef<FClientState, ESPMode::ThreadSafe> Client);
	void Abort();

private:
	friend class FNativeClientSlots;
	FNativeClientSlots* Slots = nullptr;
	FNativeClientRegistration Registration;
};

struct FNativeClientSweepResult
{
	uint32 ClientsInspected = 0;
	uint32 OperationCellsInspected = 0;
	uint32 OperationCellsReclaimed = 0;
};

/**
 * GT-only fixed client inventory. The captured Control must own this allocation and its operation stores.
 * Queue/drain/fence completion is not inferred here: its owner must supply that separate retirement condition
 * before TryReleaseQuiescent. No strong client, Hub, World or Blueprint carrier is stored in this registry.
 */
class FNativeClientSlots final
{
public:
	static constexpr uint32 Capacity = 256;
	static constexpr uint32 MaxPerScope = 64;
	explicit FNativeClientSlots(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InOwner);
	~FNativeClientSlots();
	FNativeClientSlots(const FNativeClientSlots&) = delete;
	FNativeClientSlots& operator=(const FNativeClientSlots&) = delete;

	bool TryReserve(TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope, FNativeClientReservation& Out, FName& Error);
	/** Idempotent; closes the permanent gate and bars operation children before any cancellation callout. */
	bool BeginRetirement(const FNativeClientRegistration& Registration);
	bool ReserveQueueBinding(const FNativeClientRegistration& Registration, FNativeQueueBindingReservation& Out, FName& Error, bool bForSwitch = false);
	void DiscardPendingQueueTarget(const FNativeClientRegistration& Registration);
	/** After all required gates have closed; actual detach/cancel callouts are allowed here, never in BeginStop's gate loop. */
	void DetachQueuesForRetirement(const FNativeClientRegistration& Registration);
	void DetachQueueBinding(const FNativeClientRegistration& Registration, const TSharedPtr<FQueueBinding, ESPMode::ThreadSafe>& Binding, bool bRevoke = true);
	/** Existing service scans 256 clients / 512 drains, with weak-client activation and coalesced limiter grants. */
	void SweepQueueDrains(bool bAllowStoppingFlush = false);
	void DetachAllQueuesForStop();
	/** One managed service-turn pump instead of one named-GT task per setter; ordinary client scheduling stays unchanged. */
	void RequestLimiterPump(const FNativeClientRegistration& Registration);
	/** Bounded weak-client close actions. No client/Hub is retained while physical or IO obligations settle. */
	void RetireClosingClientsOnGT();
	FManagedRefreshDelivery* RefreshDeliveryOnGT(const FNativeClientRegistration& Registration) const;
	void DropRefreshCallbacksAtStopOnGT();
	/**
	 * Records the caller's completed queue/drain/fence/probe/auth barrier for this exact closing generation.
	 * False may mean operation anchors remain; the existing bounded ticker retries reclamation afterward.
	 */
	bool TryReleaseQuiescent(const FNativeClientRegistration& Registration);
	TSharedPtr<FClientState, ESPMode::ThreadSafe> PinClient(const FNativeClientRegistration& Registration) const;
	/** Across the entire array, never more than Capacity clients and Budget (clamped to 256) cells. */
	FNativeClientSweepResult SweepOperations(uint32 Budget = 256);
	/** First shutdown phase, before Control publishes Stopped; no client/cancel/trace callout here. */
	void BeginStop();
	bool IsStopping() const { return bStopping; }   // GT only
	uint32 NumOccupied() const { return Occupied; } // includes Reserved, Live and Closing
#if WITH_DEV_AUTOMATION_TESTS
	void SeedFreeGenerationsForTests(uint64 Generation);
#endif

private:
	friend class FNativeClientReservation;
	friend class FNativeQueueBindingReservation;
	enum class EDrainPhase : uint8
	{
		Empty,
		Reserved,
		Pending,
		Active,
		Detaching,
		Draining,
		Cleaning,
		Exhausted
	};
	struct FQueueDrain
	{
		EDrainPhase Phase = EDrainPhase::Empty;
		TSharedPtr<FOfflineQueue, ESPMode::ThreadSafe> Queue;
		TSharedPtr<FQueueBinding, ESPMode::ThreadSafe> Binding;
		uint64 Through = 0;
		FNativeQueueFenceState Native;
		bool bFlushAttempted = false, bFlushAdmitted = false, bDrainAcknowledged = false;
		EQueueFenceResult Flush = EQueueFenceResult::Pending;
	};
	enum class EPhase : uint8
	{
		Free,
		Reserved,
		Live,
		Closing,
		Exhausted
	};
	struct FSlot
	{
		uint64 Generation = 1;
		EPhase Phase = EPhase::Free;
		bool bCloseRequested = false;
		bool bNonOperationQuiescent = false;
		bool bLimiterPumpPending = false;
		TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
		TSharedPtr<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope;
		TWeakPtr<FClientState, ESPMode::ThreadSafe> Client;
		// Retained across client reuse: old operation identities can never alias a reset cell generation.
		TUniquePtr<FNativeOperationStore> Operations;
		TUniquePtr<FManagedRefreshDelivery> RefreshDelivery;
		FQueueDrain Drains[2]; // active binding already owns its row; permanent close never needs a third reservation
	};
	bool CommitQueueBinding(const FNativeQueueBindingReservation& Reservation,
		TSharedRef<FOfflineQueue, ESPMode::ThreadSafe> Queue, TSharedRef<FQueueBinding, ESPMode::ThreadSafe> Binding);
	bool ParkQueueBinding(const FNativeQueueBindingReservation& Reservation);
	void AbortQueueBinding(const FNativeQueueBindingReservation& Reservation);
	void BeginQueueDrain(FQueueDrain& Drain, bool bRevoke = true);
	FSlot* Find(const FNativeClientRegistration& Registration) const;
	bool Commit(const FNativeClientRegistration& Registration, TSharedRef<FClientState, ESPMode::ThreadSafe> Client);
	void Abandon(const FNativeClientRegistration& Registration);
	void Close(FSlot& Slot);
	bool TryReclaim(FSlot& Slot);
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Owner;
	TUniquePtr<FSlot[]> Slots;
	uint32 Occupied = 0;
	uint32 NextReservation = 0;
	uint32 NextOperationClient = 0;
	bool bStopping = false;
	bool bSweeping = false;
};
} // namespace VaRest::Http
