// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorBudget.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
FBudgetAccount::FBudgetAccount(int64 InLimit, int64 InBootstrap, TRefCountPtr<FAllocationLifetime> InLifetime)
	: Limit(InLimit)
	, BootstrapBytes(InBootstrap)
	, UsedBytes(InBootstrap)
	, PeakBytes(InBootstrap)
	, Lifetime(MoveTemp(InLifetime))
{
}
FBudgetAccount::~FBudgetAccount()
{
	check(UsedBytes.load() == BootstrapBytes && SnapshotControllers.load() == 0 && Probe.LiveControllers.load() == 0);
}
FStoreCredit::FStoreCredit(FStoreCredit&& Other) noexcept
	: Account(MoveTemp(Other.Account))
	, Bytes(Other.Bytes)
	, bSlot(Other.bSlot)
{
	Other.Bytes = 0;
	Other.bSlot = false;
}
FStoreCredit& FStoreCredit::operator=(FStoreCredit&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Account = MoveTemp(Other.Account);
		Bytes = Other.Bytes;
		bSlot = Other.bSlot;
		Other.Bytes = 0;
		Other.bSlot = false;
	}
	return *this;
}
FStoreCredit::~FStoreCredit()
{
	Reset();
}
TOptional<FStoreCredit> FStoreCredit::Acquire(TRefCountPtr<FBudgetAccount> InAccount, int64 Charge, bool bSnapshotSlot)
{
	if (!InAccount || !InAccount->Lifetime || InAccount->BootstrapBytes < 0 || InAccount->BootstrapBytes > InAccount->Limit || Charge < 0)
		return {};
	if (bSnapshotSlot && !TryReserve(InAccount->SnapshotControllers, FNormalizedLimits::SnapshotControllers, 1))
		return {};
	int64 ReservedBytes = 0;
	if (!TryReserve(InAccount->UsedBytes, InAccount->Limit, Charge, &ReservedBytes))
	{
		if (bSnapshotSlot)
			InAccount->SnapshotControllers.fetch_sub(1);
		return {};
	}
	ObservePeak(InAccount->PeakBytes, ReservedBytes);
	FStoreCredit Result;
	Result.Account = MoveTemp(InAccount);
	Result.Bytes = Charge;
	Result.bSlot = bSnapshotSlot;
	return TOptional<FStoreCredit>(MoveTemp(Result));
}
void FStoreCredit::Reset()
{
	if (!Account)
		return;
	Account->UsedBytes.fetch_sub(Bytes, std::memory_order_acq_rel);
	if (bSlot)
		Account->SnapshotControllers.fetch_sub(1, std::memory_order_acq_rel);
	Bytes = 0;
	bSlot = false;
	Account.SafeRelease();
}
} // namespace VaRest::Inspector::Private
#endif
