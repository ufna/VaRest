// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestInspectorAllocation.h"

#include "CoreGlobals.h"
#include "HAL/PlatformMemory.h"
#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"

LLM_DEFINE_TAG(VaRest_Inspector);

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
bool TryReserve(std::atomic<int64>& Used, int64 Limit, int64 Amount, int64* OutUsed)
{
	if (Amount < 0 || Amount > Limit)
		return false;
	int64 Seen = Used.load(std::memory_order_relaxed);
	do
	{
		if (Seen < 0 || Seen > Limit - Amount)
			return false;
	}
	while (!Used.compare_exchange_weak(Seen, Seen + Amount, std::memory_order_acq_rel, std::memory_order_relaxed));
	if (OutUsed)
		*OutUsed = Seen + Amount;
	return true;
}
bool AddChecked(int64& Sum, int64 Amount, int64 Limit)
{
	if (Amount < 0 || Sum < 0 || Amount > Limit || Sum > Limit - Amount)
		return false;
	Sum += Amount;
	return true;
}
void ObservePeak(std::atomic<int64>& Peak, int64 Value)
{
	int64 Seen = Peak.load(std::memory_order_relaxed);
	while (Seen < Value && !Peak.compare_exchange_weak(Seen, Value, std::memory_order_relaxed))
	{
	}
}
void AddSaturated(std::atomic<uint64>& Counter, uint64 Amount)
{
	uint64 Seen = Counter.load(std::memory_order_relaxed);
	uint64 Next;
	do
	{
		Next = Amount > MAX_uint64 - Seen ? MAX_uint64 : Seen + Amount;
	}
	while (!Counter.compare_exchange_weak(Seen, Next, std::memory_order_release, std::memory_order_relaxed));
}
FPendingAccount::FPendingAccount(int64 InCountLimit, int64 InByteLimit)
	: CountLimit(InCountLimit)
	, ByteLimit(InByteLimit)
{
}
FPendingAccount::~FPendingAccount()
{
	check(Count.load() == 0 && Bytes.load() == 0 && ExcerptReservations.load() == 0);
}
FPendingTicket::FPendingTicket(FPendingTicket&& Other) noexcept
	: Account(MoveTemp(Other.Account))
	, Charge(Other.Charge)
{
	Other.Charge = 0;
}
FPendingTicket& FPendingTicket::operator=(FPendingTicket&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Account = MoveTemp(Other.Account);
		Charge = Other.Charge;
		Other.Charge = 0;
	}
	return *this;
}
FPendingTicket::~FPendingTicket()
{
	Reset();
}
TOptional<FPendingTicket> FPendingTicket::Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge, bool* OutCountRefused)
{
	if (OutCountRefused)
		*OutCountRefused = false;
	int64 ReservedCount = 0, ReservedBytes = 0;
	if (!TryReserve(InAccount->Count, InAccount->CountLimit, 1, &ReservedCount))
	{
		if (OutCountRefused)
			*OutCountRefused = true;
		return {};
	}
	ObservePeak(InAccount->PeakCount, ReservedCount);
	if (!TryReserve(InAccount->Bytes, InAccount->ByteLimit, InCharge, &ReservedBytes))
	{
		InAccount->Count.fetch_sub(1, std::memory_order_acq_rel);
		return {};
	}
	ObservePeak(InAccount->PeakBytes, ReservedBytes);
	FPendingTicket Ticket;
	Ticket.Account = InAccount;
	Ticket.Charge = InCharge;
	return TOptional<FPendingTicket>(MoveTemp(Ticket));
}
void FPendingTicket::Reset()
{
	if (Account.IsValid())
	{
		Account->Bytes.fetch_sub(Charge, std::memory_order_acq_rel);
		Account->Count.fetch_sub(1, std::memory_order_acq_rel);
		Charge = 0;
		Account.Reset();
	}
}
FPendingBaseline::FPendingBaseline(FPendingBaseline&& Other) noexcept
	: Account(MoveTemp(Other.Account))
	, Charge(Other.Charge)
{
	Other.Charge = 0;
}
FPendingBaseline& FPendingBaseline::operator=(FPendingBaseline&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Account = MoveTemp(Other.Account);
		Charge = Other.Charge;
		Other.Charge = 0;
	}
	return *this;
}
FPendingBaseline::~FPendingBaseline()
{
	Reset();
}
TOptional<FPendingBaseline> FPendingBaseline::Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge)
{
	int64 ReservedBytes = 0;
	if (!TryReserve(InAccount->Bytes, InAccount->ByteLimit, InCharge, &ReservedBytes))
		return {};
	ObservePeak(InAccount->PeakBytes, ReservedBytes);
	FPendingBaseline Result;
	Result.Account = InAccount;
	Result.Charge = InCharge;
	return TOptional<FPendingBaseline>(MoveTemp(Result));
}
void FPendingBaseline::Reset()
{
	if (Account.IsValid())
	{
		Account->Bytes.fetch_sub(Charge, std::memory_order_acq_rel);
		Charge = 0;
		Account.Reset();
	}
}
FAllocationAdapter::FAllocationAdapter(FAllocationCounters& InCounters, FSupportedAllocator InSupport)
	: Probe(&InCounters)
	, Support(InSupport)
{
}
FSupportedAllocator FAllocationAdapter::GetSupport() const
{
	return bFailed.load(std::memory_order_acquire) ? FSupportedAllocator{} : Support;
}
bool FAllocationAdapter::Calibrate(std::atomic<int64>& StartupUsed, int64 StartupLimit, SIZE_T ControllerRequest, uint32 TypeAlignment)
{
	check(IsInGameThread());
	if (bCalibrationAttempted)
		return false;
	bCalibrationAttempted = true;
	if (Support.Family == FSupportedAllocator::EFamily::Unsupported || !Support.bVerifiedModuleNew ||
		!Support.bVerifiedNoInflatingProxy || Support.bCalibrationPassed || sizeof(void*) != 8 ||
		ControllerRequest == 0 || ControllerRequest > 1024 || TypeAlignment == 0 || TypeAlignment > 16 ||
		(TypeAlignment & (TypeAlignment - 1)) != 0 || __STDCPP_DEFAULT_NEW_ALIGNMENT__ > 16)
	{
		AddSaturated(Probe->UnsupportedAllocator);
		return false;
	}
	if (!TryReserve(StartupUsed, StartupLimit, 4096))
		return false;
	const uint32 OrdinaryAlignment = ControllerRequest <= 8 ? 8 : __STDCPP_DEFAULT_NEW_ALIGNMENT__;
	LLM_SCOPE_BYTAG(VaRest_Inspector);
	void* Ptr = FMemory::Malloc(ControllerRequest, OrdinaryAlignment);
	const SIZE_T Actual = Ptr ? FMemory::GetAllocSize(Ptr) : 0;
	Probe->CalibrationRequest.store(int64(ControllerRequest));
	Probe->CalibrationAlignment.store(OrdinaryAlignment);
	Probe->CalibrationCapacity.store(int64(Actual));
	const int64 Live = Probe->LiveBacking.fetch_add(int64(Actual)) + int64(Actual);
	ObservePeak(Probe->PeakBacking, Live);
	FMemory::Free(Ptr);
	Probe->LiveBacking.fetch_sub(int64(Actual));
	StartupUsed.fetch_sub(4096, std::memory_order_acq_rel);
	if (!Ptr || Actual < ControllerRequest || Actual > 4096)
	{
		bFailed.store(true, std::memory_order_release);
		AddSaturated(Probe->BoundFailures);
		ensureMsgf(false, TEXT("Inspector allocator calibration violated its certified bound"));
		return false;
	}
	Support.bCalibrationPassed = true;
	return true;
}
TOptional<SIZE_T> FAllocationAdapter::Bound(SIZE_T Request, uint32 Alignment) const
{
	const FSupportedAllocator Capability = GetSupport();
	if (!Capability.bCalibrationPassed || !Capability.bVerifiedModuleNew || !Capability.bVerifiedNoInflatingProxy ||
		Request == 0 || Request > SIZE_T(FNormalizedLimits::MaxMetadataBytes) || Alignment == 0 || Alignment > 16 ||
		(Alignment & (Alignment - 1)) != 0)
		return {};
	const auto& Constants = FPlatformMemory::GetConstants();
	if (Capability.Family == FSupportedAllocator::EFamily::Mimalloc200)
	{
		if (Constants.PageSize == 0 || Constants.PageSize > 65536 || (Constants.PageSize & (Constants.PageSize - 1)) != 0)
			return {};
		// Audited mimalloc2.0.0 alloc-aligned.c:17-59 adds at most15;
		// page-queue.c:55-94/init.c:52-69 bins fit the next power of two.
		// page.c:783-815/os.c:98-108 round large blocks (here <2MiB)
		// to at most64KiB. The power-of-two bound covers that rounding.
		int64 Padded = int64(Request);
		if (!AddChecked(Padded, 15 + 8, MAX_int64))
			return {};
		SIZE_T Capacity = 64;
		while (Capacity < SIZE_T(Padded))
		{
			if (Capacity > SIZE_T(MAX_int64) / 2)
				return {};
			Capacity *= 2;
		}
		return Capacity;
	}
	if (Capability.Family == FSupportedAllocator::EFamily::Binned2)
	{
		const SIZE_T Granularity = Constants.BinnedAllocationGranularity ? Constants.BinnedAllocationGranularity : Constants.BinnedPageSize;
		if (Granularity == 0 || Granularity > 65536 || (Granularity & (Granularity - 1)) != 0 || Constants.BinnedPageSize < 16)
			return {};
		// MallocBinned2.cpp:717-758 records the aligned OS allocation;
		// MallocBinnedCommon.h:896-922 returns that capacity. Small bins
		// are <=32752; the <=1024 controller proof has a4096 bound.
		if (Request <= 1024)
			return SIZE_T(4096);
		if (Request <= 65536)
			return SIZE_T(65536);
		const SIZE_T Multiple = Granularity < 16 ? 16 : Granularity;
		int64 Rounded = int64(Request);
		if (!AddChecked(Rounded, int64(Multiple - 1), MAX_int64))
			return {};
		return SIZE_T(Rounded) & ~(Multiple - 1);
	}
	return {};
}
void* FAllocationAdapter::AllocateReserved(SIZE_T Request, uint32 Alignment, SIZE_T ReservedBound)
{
	const auto Certified = Bound(Request, Alignment);
	if (!Certified.IsSet() || Certified.GetValue() != ReservedBound)
	{
		AddSaturated(Probe->UnsupportedAllocator);
		return nullptr;
	}
	LLM_SCOPE_BYTAG(VaRest_Inspector);
	void* Ptr = FMemory::Malloc(Request, Alignment);
	const SIZE_T Actual = Ptr ? FMemory::GetAllocSize(Ptr) : 0;
	if (!Ptr || Actual < Request || Actual > ReservedBound)
	{
		FMemory::Free(Ptr);
		bFailed.store(true, std::memory_order_release);
		AddSaturated(Probe->BoundFailures);
		ensureMsgf(false, TEXT("Inspector allocation violated its certified bound"));
		return nullptr;
	}
	const int64 Live = Probe->LiveBacking.fetch_add(int64(Actual)) + int64(Actual);
	ObservePeak(Probe->PeakBacking, Live);
	return Ptr;
}
void FAllocationAdapter::Free(void* Ptr)
{
	if (!Ptr)
		return;
	const SIZE_T Actual = FMemory::GetAllocSize(Ptr);
	FMemory::Free(Ptr);
	Probe->LiveBacking.fetch_sub(int64(Actual), std::memory_order_acq_rel);
}
FAllocationCounters& FAllocationAdapter::Counters() const
{
	return *Probe;
}
} // namespace VaRest::Inspector::Private
#endif
