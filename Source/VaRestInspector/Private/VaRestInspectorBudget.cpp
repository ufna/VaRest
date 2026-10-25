// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorBudget.h"

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
namespace
{
std::atomic<uint64> LastBudgetIdentity{0};
uint64 NextBudgetIdentity()
{
	uint64 Previous = LastBudgetIdentity.load(std::memory_order_relaxed);
	while (Previous != MAX_uint64)
		if (LastBudgetIdentity.compare_exchange_weak(Previous, Previous + 1, std::memory_order_relaxed))
			return Previous + 1;
	return 0;
}
}
FBudgetAccount::FBudgetAccount(int64 InLimit, int64 InBootstrap, TRefCountPtr<FAllocationLifetime> InLifetime)
	: Identity(NextBudgetIdentity())
	, Limit(InLimit)
	, BootstrapBytes(InBootstrap)
	, UsedBytes(InBootstrap)
	, PeakBytes(InBootstrap)
	, Lifetime(MoveTemp(InLifetime))
{
}
void FBudgetAccount::InheritLifetime(const FBudgetAccount& Previous)
{
	const int64 PreviousPeak = Previous.PeakBytes.load(std::memory_order_acquire);
	PeakBytes.store(PreviousPeak > BootstrapBytes ? PreviousPeak : BootstrapBytes, std::memory_order_release);
	Probe.DestroyedControllers.store(Previous.Probe.DestroyedControllers.load(std::memory_order_acquire), std::memory_order_release);
	PublicStringsUnsupported.store(Previous.PublicStringsUnsupported.load(std::memory_order_acquire), std::memory_order_release);
}
FBudgetAccount::~FBudgetAccount()
{
	check(UsedBytes.load() == BootstrapBytes && SnapshotControllers.load() == 0 && Probe.LiveControllers.load() == 0 && ControlLeases.load() == 0);
}
void CarryPendingLifetime(const FPendingAccount& Previous, FPendingAccount& Next)
{
	check(Next.Count.load() == 0 && Next.Bytes.load() == 0 && Next.ExcerptReservations.load() == 0 && Next.Lifetime == Previous.Lifetime);
	Next.PeakCount.store(Previous.PeakCount.load(std::memory_order_acquire), std::memory_order_release);
	Next.PeakBytes.store(Previous.PeakBytes.load(std::memory_order_acquire), std::memory_order_release);
	Next.DroppedCount.store(Previous.DroppedCount.load(std::memory_order_acquire), std::memory_order_release);
	Next.DroppedBytes.store(Previous.DroppedBytes.load(std::memory_order_acquire), std::memory_order_release);
	Next.MetadataOmitted.store(Previous.MetadataOmitted.load(std::memory_order_acquire), std::memory_order_release);
	Next.ExcerptsSkipped.store(Previous.ExcerptsSkipped.load(std::memory_order_acquire), std::memory_order_release);
	Next.LossEpoch.store(Previous.LossEpoch.load(std::memory_order_acquire), std::memory_order_release);
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
