// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "HAL/UnrealMemory.h"
#include "Templates/SharedPointerInternals.h"
#include "VaRestInspectorAllocation.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
struct FBudgetAccount : public FRefCountedObject
{
	FBudgetAccount(int64 InLimit, int64 InBootstrap, TRefCountPtr<FAllocationLifetime> InLifetime);
	~FBudgetAccount();
	void InheritLifetime(const FBudgetAccount& Previous);
	const uint64 Identity;
	const int64 Limit, BootstrapBytes;
	std::atomic<int64> UsedBytes, PeakBytes;
	std::atomic<int64> SnapshotControllers{0};
	std::atomic<int32> ControlLeases{0};
	std::atomic<bool> PublicStringsUnsupported{false};
	FAllocationCounters Probe;
	const TRefCountPtr<FAllocationLifetime> Lifetime;
};

class FBudgetControlLease
{
public:
	explicit FBudgetControlLease(FBudgetAccount& InAccount)
		: Account(&InAccount)
	{
		Account->ControlLeases.fetch_add(1, std::memory_order_acq_rel);
	}
	~FBudgetControlLease()
	{
		if (Account)
			Account->ControlLeases.fetch_sub(1, std::memory_order_acq_rel);
	}
	FBudgetControlLease(const FBudgetControlLease&) = delete;
	FBudgetControlLease& operator=(const FBudgetControlLease&) = delete;

private:
	FBudgetAccount* Account;
};

void CarryPendingLifetime(const FPendingAccount& Previous, FPendingAccount& Next);

class FStoreCredit
{
public:
	FStoreCredit() = default;
	FStoreCredit(const FStoreCredit&) = delete;
	FStoreCredit& operator=(const FStoreCredit&) = delete;
	FStoreCredit(FStoreCredit&& Other) noexcept;
	FStoreCredit& operator=(FStoreCredit&& Other) noexcept;
	~FStoreCredit();
	static TOptional<FStoreCredit> Acquire(TRefCountPtr<FBudgetAccount> Account, int64 Charge, bool bSnapshotSlot = false);
	FBudgetAccount* GetAccount() const { return Account.GetReference(); }
	int64 Charge() const { return Bytes; }
	void Reset();

private:
	TRefCountPtr<FBudgetAccount> Account;
	int64 Bytes = 0;
	bool bSlot = false;
};

template <typename T>
class TInspectorDeleter
{
public:
	TInspectorDeleter(FStoreCredit&& InPayload, FStoreCredit&& InController)
		: Payload(MoveTemp(InPayload))
		, Controller(MoveTemp(InController))
	{
	}
	TInspectorDeleter(const TInspectorDeleter&) = delete;
	TInspectorDeleter& operator=(const TInspectorDeleter&) = delete;
	TInspectorDeleter(TInspectorDeleter&& Other) noexcept
		: Payload(MoveTemp(Other.Payload))
		, Controller(MoveTemp(Other.Controller))
	{
		check(!Other.bStored);
		bStored = true;
		Controller.GetAccount()->Probe.LiveControllers.fetch_add(1);
	}
	~TInspectorDeleter()
	{
		if (bStored)
		{
			FBudgetAccount* Account = Controller.GetAccount();
			Account->Probe.LiveControllers.fetch_sub(1);
			Account->Probe.DestroyedControllers.fetch_add(1);
		}
		// Final-weak stored-deleter destruction releases controller credit.
		// The following hidden engine delete/free tail is excluded (decision8).
	}
	void operator()(T* Object)
	{
		FAllocationAdapter& Allocator = Payload.GetAccount()->Lifetime->Allocator;
		Object->~T();
		Allocator.Free(Object);
		Payload.Reset();
	}

private:
	FStoreCredit Payload, Controller;
	bool bStored = false;
};

template <typename T>
bool HasInspectorControllerCalibration(const FAllocationLifetime& Lifetime)
{
	using D = TInspectorDeleter<T>;
	using C = SharedPointerInternals::TReferenceControllerWithDeleter<T, D, ESPMode::ThreadSafe>;
	constexpr uint32 OrdinaryAlignment = sizeof(C) <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	const FSupportedAllocator Current = Lifetime.Allocator.GetSupport();
	return Current.SupportsController(sizeof(C), alignof(C)) &&
		   Lifetime.Probe.CalibrationRequest.load() == int64(sizeof(C)) &&
		   Lifetime.Probe.CalibrationAlignment.load() == OrdinaryAlignment &&
		   Lifetime.Probe.CalibrationCapacity.load() >= int64(sizeof(C)) &&
		   Lifetime.Probe.CalibrationCapacity.load() <= 4096;
}

template <typename T, typename... Args>
TSharedPtr<T, ESPMode::ThreadSafe> MakeInspectorOwner(TRefCountPtr<FBudgetAccount> Account,
	const FSupportedAllocator&, SIZE_T PayloadBound, bool bSnapshotSlot, Args&&... Arguments)
{
	if (!Account || !Account->Lifetime || !HasInspectorControllerCalibration<T>(*Account->Lifetime) || PayloadBound > SIZE_T(MAX_int64))
		return {};
	FAllocationAdapter& Allocator = Account->Lifetime->Allocator;
	const auto Bound = Allocator.Bound(sizeof(T), alignof(T));
	if (!Bound.IsSet() || Bound.GetValue() != PayloadBound)
		return {};
	auto Controller = FStoreCredit::Acquire(Account, 4096, bSnapshotSlot);
	if (!Controller.IsSet())
		return {};
	auto Payload = FStoreCredit::Acquire(Account, int64(PayloadBound));
	if (!Payload.IsSet())
		return {};
	void* Storage = Allocator.AllocateReserved(sizeof(T), alignof(T), PayloadBound);
	if (!Storage)
		return {};
	T* Object = new (Storage) T(Forward<Args>(Arguments)...);
	TInspectorDeleter<T> Deleter(MoveTemp(Payload.GetValue()), MoveTemp(Controller.GetValue()));
	LLM_SCOPE_BYTAG(VaRest_Inspector);
	return TSharedPtr<T, ESPMode::ThreadSafe>(Object, MoveTemp(Deleter));
}
} // namespace VaRest::Inspector::Private
#endif
