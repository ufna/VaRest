// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestNativeLifetime.h"
#include "VaRestSession.h"
#include <array>

namespace VaRest::Http
{
class FManagedRefreshNativeAccess;

/**
 * Control-owned, GT-only actual External callback storage. Accepted Waiting+Ready+Delivering subscriptions share64
 * positions. Deferred tasks contain only native access/results. Rejected callbacks use the explicitly UNBOUNDED
 * transient refusal inventory selected by root-m5-task5-managed-refresh-delivery-source-ruling-v1.json.
 */
class FManagedRefreshDelivery final
{
public:
	static constexpr uint32 Capacity = 64;
	FManagedRefreshDelivery(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control,
		TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> Gate);
	~FManagedRefreshDelivery();
	FManagedRefreshDelivery(const FManagedRefreshDelivery&) = delete;
	FManagedRefreshDelivery& operator=(const FManagedRefreshDelivery&) = delete;
	/** GT. Accepted Original moves into fixed storage and becomes a native-only one-shot adapter. Failure preserves it. */
	bool TryReserveOnGT(FRefreshWaiter& Original);
	/** GT overflow only: no accepted waiter/auth/operation permit. Independent GT ownership precedes wake publication. */
	void RefuseLaterOnGT(FRefreshWaiter&& Original);
	/** Owned GT cleanup; invokes no callback and never posts after captured close/Stop. */
	void RetireOnGT();
	void ServiceOnGT();
	bool IsQuiescentOnGT() const;
	bool ResetForNextClientOnGT(TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> Gate);

private:
	friend class FManagedRefreshNativeAccess;
	enum class EPhase : uint8
	{
		Free,
		Waiting,
		Ready,
		Delivering,
		Retiring,
		Exhausted
	};
	struct FSlot
	{
		uint64 Generation = 1;
		EPhase Phase = EPhase::Free;
		std::atomic<uint64> Authorities{0};
		FRefreshWaiter Original;
		FRefreshEnded Result;
	};
	struct FRefusal
	{
		FRefreshWaiter Original;
		FRefusal* Next = nullptr;
	};
	TSharedRef<FManagedRefreshNativeAccess, ESPMode::ThreadSafe> AccessOnGT(int32 Index = -1, uint64 Generation = 0, bool bWake = false);
	void ReleaseAccess(int32 Index, uint64 Generation, bool bWake); // any thread, native fields only
	void SetReadyOnGT(uint32 Index, uint64 Generation, const FRefreshEnded& End);
	void WakeOnGT();
	void PublishWakeOnGT();
	void DeliverPendingOnGT();
	void CollectOnGT();
	void DropPendingOnGT();
	bool HasReadyOnGT() const;
	bool IsClosed() const;
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Owner;
	TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Gate;
	std::array<FSlot, Capacity> Slots;
	std::atomic<uint64> Authorities{0};
	std::atomic<bool> WakeEnded{false};
	FRefusal* RefusedHead = nullptr;
	FRefusal* RefusedTail = nullptr;
	uint32 NextDelivery = 0;
	bool bRetired = false, bWakeOutstanding = false, bWakeRan = false, bDelivering = false, bCollecting = false, bDropping = false;
};
} // namespace VaRest::Http
