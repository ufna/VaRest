// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestDynamicBudget.h"
#include "VaRestHttpSettings.h"

FVaRestDynamicLimits FVaRestDynamicLimits::FromSettingsOnGT(const UVaRestHttpSettings& Settings)
{
	check(IsInGameThread());
	FVaRestDynamicLimits Limits;
	Limits.MaxPending=Settings.DynamicMaxPending;
	Limits.MaxInputBytes=Settings.DynamicMaxInputBytes;
	Limits.PendingInputBytes=Settings.DynamicMaxPendingInputBytes;
	Limits.ParseAdmissionBytes=Settings.DynamicParseAdmissionBytes;
	Limits.MaxParseValues=Settings.DynamicMaxParseValues;
	Limits.MaxParseWorkUnits=Settings.DynamicMaxParseWorkUnits;
	Limits.MaxOutputBytes=Settings.DynamicMaxOutputBytes;
	Limits.PendingOutputBytes=Settings.DynamicPendingOutputBytes;
	Limits.MaxValues=Settings.DynamicMaxValues;
	Limits.MaxWorkUnits=Settings.DynamicMaxWorkUnits;
	return Limits;
}

bool FVaRestDynamicLimits::Validate(FString& Error) const
{
	Error.Reset();
	if (MaxPending <= 0 || MaxPending > 256 || MaxInputBytes <= 0 || PendingInputBytes < MaxInputBytes ||
		ParseAdmissionBytes <= 0 || MaxParseValues <= 0 || MaxParseWorkUnits <= 0 || MaxOutputBytes <= 0 ||
		PendingOutputBytes < MaxOutputBytes || MaxValues <= 0 || MaxWorkUnits <= 0)
	{
		Error = TEXT("dynamic limits must be positive, within256 pending, with aggregate bytes at least the per-result limit");
		return false;
	}
	return true;
}
TSharedPtr<FVaRestDynamicBudget, ESPMode::ThreadSafe> FVaRestDynamicBudget::Create(const FVaRestDynamicLimits& Limits, FString& Error)
{
	if (!Limits.Validate(Error)) return nullptr;
	return MakeShareable(new FVaRestDynamicBudget(Limits));
}
bool FVaRestDynamicBudget::TryReserve(FVaRestDynamicBudgetLease& Out)
{
	if (Out.IsValid()) return false;
	int32 Count = Pending.load(std::memory_order_relaxed);
	while (Count < Limits.MaxPending)
		if (Pending.compare_exchange_weak(Count, Count + 1, std::memory_order_acq_rel, std::memory_order_relaxed))
		{
			Out.Budget = AsShared();
			return true;
		}
	return false;
}
bool FVaRestDynamicBudget::Resize(std::atomic<int64>& Total, int64& Owned, int64 Wanted, int64 PerResult, int64 Aggregate)
{
	if (Wanted < 0 || Wanted > PerResult) return false;
	if (Wanted <= Owned)
	{
		Total.fetch_sub(Owned - Wanted, std::memory_order_acq_rel);
		Owned = Wanted;
		return true;
	}
	const int64 More = Wanted - Owned;
	int64 Current = Total.load(std::memory_order_relaxed);
	while (Current <= Aggregate - More) // More<=PerResult<=Aggregate; subtract before adding, no signed overflow
		if (Total.compare_exchange_weak(Current, Current + More, std::memory_order_acq_rel, std::memory_order_relaxed))
		{
			Owned = Wanted;
			return true;
		}
	return false;
}
FVaRestDynamicBudgetLease::~FVaRestDynamicBudgetLease() { Reset(); }
FVaRestDynamicBudgetLease::FVaRestDynamicBudgetLease(FVaRestDynamicBudgetLease&& Other) noexcept
	: Budget(MoveTemp(Other.Budget)), Input(Other.Input), ParseEstimate(Other.ParseEstimate), Output(Other.Output)
{
	Other.Input = Other.ParseEstimate = Other.Output = 0;
}
FVaRestDynamicBudgetLease& FVaRestDynamicBudgetLease::operator=(FVaRestDynamicBudgetLease&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Budget = MoveTemp(Other.Budget);
		Input = Other.Input; ParseEstimate = Other.ParseEstimate; Output = Other.Output;
		Other.Input = Other.ParseEstimate = Other.Output = 0;
	}
	return *this;
}
bool FVaRestDynamicBudgetLease::SetInputBytes(int64 Bytes)
{
	return Budget && FVaRestDynamicBudget::Resize(Budget->Input, Input, Bytes, Budget->Limits.MaxInputBytes, Budget->Limits.PendingInputBytes);
}
bool FVaRestDynamicBudgetLease::SetParseEstimateBytes(int64 Bytes)
{
	return Budget && FVaRestDynamicBudget::Resize(Budget->ParseEstimate, ParseEstimate, Bytes, Budget->Limits.ParseAdmissionBytes, Budget->Limits.ParseAdmissionBytes);
}
bool FVaRestDynamicBudgetLease::SetOutputBytes(int64 Bytes)
{
	return Budget && FVaRestDynamicBudget::Resize(Budget->Output, Output, Bytes, Budget->Limits.MaxOutputBytes, Budget->Limits.PendingOutputBytes);
}
void FVaRestDynamicBudgetLease::Reset()
{
	if (!Budget) return;
	auto Kept = MoveTemp(Budget);
	Kept->Input.fetch_sub(Input, std::memory_order_acq_rel);
	Kept->ParseEstimate.fetch_sub(ParseEstimate, std::memory_order_acq_rel);
	Kept->Output.fetch_sub(Output, std::memory_order_acq_rel);
	Input = ParseEstimate = Output = 0;
	Kept->Pending.fetch_sub(1, std::memory_order_acq_rel);
}
