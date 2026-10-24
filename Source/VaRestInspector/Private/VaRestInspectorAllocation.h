// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreTypes.h"
#include "HAL/LowLevelMemTracker.h"
#include "Misc/Optional.h"
#include "Templates/RefCounting.h"
#include "Templates/SharedPointer.h"
#include "VaRestInspector.h"
#include "VaRestInspectorPolicy.h"
#include <atomic>

LLM_DECLARE_TAG(VaRest_Inspector);

#if VAREST_WITH_INSPECTOR
namespace VaRest::Inspector::Private
{
bool TryReserve(std::atomic<int64>& Used, int64 Limit, int64 Amount, int64* OutUsed = nullptr);
bool AddChecked(int64& Sum, int64 Amount, int64 Limit);
void ObservePeak(std::atomic<int64>& Peak, int64 Value);
void AddSaturated(std::atomic<uint64>& Counter, uint64 Amount = 1);

struct FAllocationLifetime;
struct FPendingAccount
{
	FPendingAccount(int64 InCountLimit, int64 InByteLimit);
	~FPendingAccount();
	const int64 CountLimit;
	const int64 ByteLimit;
	std::atomic<int64> Count{0}, Bytes{0}, PeakCount{0}, PeakBytes{0};
	std::atomic<int64> ExcerptReservations{0};
	std::atomic<uint64> DroppedCount{0}, DroppedBytes{0}, MetadataOmitted{0}, ExcerptsSkipped{0}, LossEpoch{0};
	// Production service root factories pre-admit this context/account storage.
	// The context owns no account or child; pending/store roots can share it.
	TRefCountPtr<FAllocationLifetime> Lifetime;
};

class FPendingTicket
{
public:
	FPendingTicket() = default;
	FPendingTicket(const FPendingTicket&) = delete;
	FPendingTicket& operator=(const FPendingTicket&) = delete;
	FPendingTicket(FPendingTicket&& Other) noexcept;
	FPendingTicket& operator=(FPendingTicket&& Other) noexcept;
	~FPendingTicket();
	static TOptional<FPendingTicket> Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge, bool* OutCountRefused = nullptr);
	void Reset();
	bool IsValid() const { return Account.IsValid(); }
	int64 Bytes() const { return Charge; }

private:
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Account;
	int64 Charge = 0;
};

class FPendingBaseline
{
public:
	FPendingBaseline() = default;
	FPendingBaseline(const FPendingBaseline&) = delete;
	FPendingBaseline& operator=(const FPendingBaseline&) = delete;
	FPendingBaseline(FPendingBaseline&& Other) noexcept;
	FPendingBaseline& operator=(FPendingBaseline&& Other) noexcept;
	~FPendingBaseline();
	static TOptional<FPendingBaseline> Acquire(const TSharedRef<FPendingAccount, ESPMode::ThreadSafe>& InAccount, int64 InCharge);
	void Reset();

private:
	TSharedPtr<FPendingAccount, ESPMode::ThreadSafe> Account;
	int64 Charge = 0;
};

struct FAllocationCounters
{
	std::atomic<int64> LiveBacking{0}, PeakBacking{0}, TextBytesCopied{0};
	std::atomic<int64> ExcerptRefsAcquired{0}, OverlapObservations{0};
	std::atomic<int64> LiveControllers{0}, DestroyedControllers{0};
	std::atomic<uint64> UnsupportedAllocator{0}, BoundFailures{0};
	std::atomic<int64> CalibrationRequest{0}, CalibrationAlignment{0}, CalibrationCapacity{0};
};

class FAllocationAdapter
{
public:
	FAllocationAdapter(FAllocationCounters& InCounters, FSupportedAllocator InSupport);
	// GT-only, before publishing any consumer; actual pre-reserved calibration.
	// The external produced-build/profile gate must precede this operation.
	bool Calibrate(std::atomic<int64>& StartupUsed, int64 StartupLimit, SIZE_T ControllerRequest, uint32 TypeAlignment);
	FSupportedAllocator GetSupport() const;
	TOptional<SIZE_T> Bound(SIZE_T Request, uint32 Alignment) const;
	void* AllocateReserved(SIZE_T Request, uint32 Alignment, SIZE_T ReservedBound);
	void Free(void* Ptr);
	FAllocationCounters& Counters() const;
	SIZE_T QueueNodeBound() const;

private:
	FAllocationCounters* Probe;
	FSupportedAllocator Support;
	bool bCalibrationAttempted = false;
	std::atomic<bool> bFailed{false};
};

struct FAllocationLifetime final : FRefCountedObject
{
	FAllocationCounters Probe;
	FAllocationAdapter Allocator;
	explicit FAllocationLifetime(FSupportedAllocator InSupport)
		: Allocator(Probe, InSupport)
	{
	}
};
} // namespace VaRest::Inspector::Private
#endif
