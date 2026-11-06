// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include <atomic>

class UVaRestHttpSettings;

/** Logical admission settings. Native defaults/constructors, allocator slack and caller-retained results are excluded. */
struct VARESTHTTP_API FVaRestDynamicLimits
{
	int32 MaxPending = 64;
	int64 MaxInputBytes = 32LL * 1024 * 1024;
	int64 PendingInputBytes = 128LL * 1024 * 1024;
	/** Estimate-account admission, not a bound on actual parser arena/scratch or heap ownership. */
	int64 ParseAdmissionBytes = 128LL * 1024 * 1024;
	int64 MaxParseValues = 1000000;
	int64 MaxParseWorkUnits = 8000000;
	int64 MaxOutputBytes = 16LL * 1024 * 1024;
	int64 PendingOutputBytes = 128LL * 1024 * 1024;
	int64 MaxValues = 1000000;
	int64 MaxWorkUnits = 8000000;
	bool Validate(FString& Error) const;
	/** GT activation snapshot only; subsequent native accounting never dereferences the settings UObject. */
	static FVaRestDynamicLimits FromSettingsOnGT(const UVaRestHttpSettings& Settings);
};

class FVaRestDynamicBudget;

/** Move-only admitted account. Destruction is atomic-only; it owns no type, UObject, callback or GT permit. */
class VARESTHTTP_API FVaRestDynamicBudgetLease final
{
public:
	FVaRestDynamicBudgetLease() = default;
	~FVaRestDynamicBudgetLease();
	FVaRestDynamicBudgetLease(FVaRestDynamicBudgetLease&& Other) noexcept;
	FVaRestDynamicBudgetLease& operator=(FVaRestDynamicBudgetLease&& Other) noexcept;
	FVaRestDynamicBudgetLease(const FVaRestDynamicBudgetLease&) = delete;
	FVaRestDynamicBudgetLease& operator=(const FVaRestDynamicBudgetLease&) = delete;
	bool IsValid() const { return Budget.IsValid(); }
	/** Single owner updates these charges BEFORE retaining/growing the corresponding controlled payload. */
	bool SetInputBytes(int64 Bytes);
	bool SetParseEstimateBytes(int64 Bytes);
	bool SetOutputBytes(int64 Bytes);
	void Reset();

private:
	friend class FVaRestDynamicBudget;
	TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> Budget;
	int64 Input = 0, ParseEstimate = 0, Output = 0;
};

/** Shared per-Hub/native budget. Frame/native control also enforces its separate fixed module-wide256 slots. */
class VARESTHTTP_API FVaRestDynamicBudget final : public TSharedFromThis<FVaRestDynamicBudget, ESPMode::ThreadSafe>
{
public:
	static TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> Create(const FVaRestDynamicLimits& Limits, FString& Error);
	bool TryReserve(FVaRestDynamicBudgetLease& Out);
	const FVaRestDynamicLimits& GetLimits() const { return Limits; }
	int32 NumPending() const { return Pending.load(std::memory_order_acquire); }
	int64 InputBytes() const { return Input.load(std::memory_order_acquire); }
	int64 ParseEstimateBytes() const { return ParseEstimate.load(std::memory_order_acquire); }
	int64 OutputBytes() const { return Output.load(std::memory_order_acquire); }

private:
	friend class FVaRestDynamicBudgetLease;
	explicit FVaRestDynamicBudget(const FVaRestDynamicLimits& InLimits) : Limits(InLimits) {}
	static bool Resize(std::atomic<int64>& Total, int64& Owned, int64 Wanted, int64 PerResult, int64 Aggregate);
	const FVaRestDynamicLimits Limits;
	std::atomic<int32> Pending{0};
	std::atomic<int64> Input{0}, ParseEstimate{0}, Output{0};
};
