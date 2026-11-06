// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestManagedOutcome.h"

namespace VaRest::Http
{
/** Native-only adapter owner; its last destruction never invokes/destroys the actual stage payload. */
class FManagedNativeOutcomeAccess final : public TSharedFromThis<FManagedNativeOutcomeAccess, ESPMode::ThreadSafe>
{
public:
	FManagedNativeOutcomeAccess(FNativeOperationRef InRef, FNativeLogicalTailLease&& InLease,
		FManagedOutcomeSlots* InOwner, uint8 InIndex, uint64 InGeneration)
		: Ref(MoveTemp(InRef))
		, Lease(MoveTemp(InLease))
		, Owner(InOwner)
		, Index(InIndex)
		, Generation(InGeneration)
	{
		check(Ref.IsValid() && Lease.IsHeld() && Owner);
	}
	~FManagedNativeOutcomeAccess()
	{
		Owner->HeldSlot(Index, Generation).bAccessEnded.store(true, std::memory_order_release); // LAST slot access
		Lease.Reset();                                                                          // LAST cell access; Ref keeps the exact Control/store allocation through the whole settle method
	}
	void InvokeOnGT(FVaRestAttemptOutcome&& Outcome)
	{
		check(IsInGameThread());
		const auto Self = AsShared();
		const auto Control = Ref.Control;
		if (!Control->IsStopped())
			Owner->InvokeOnGT(Index, Generation, MoveTemp(Outcome));
	}
	void InvokeTaskOnGT()
	{
		check(IsInGameThread());
		const auto Self = AsShared();
		const auto Control = Ref.Control;
		if (!Control->IsStopped())
			Owner->InvokeTaskOnGT(Index, Generation);
	}

private:
	FNativeOperationRef Ref;
	FNativeLogicalTailLease Lease;
	FManagedOutcomeSlots* Owner;
	uint8 Index;
	uint64 Generation;
};

FManagedOutcomeSlots::FManagedOutcomeSlots(FNativeOperationRef InRef)
	: Ref(MoveTemp(InRef))
{
	check(IsInGameThread() && Ref.IsValid());
}
FManagedOutcomeSlots::~FManagedOutcomeSlots()
{
	check(IsInGameThread());
	for (const auto& Slot : Slots)
		check(!Slot.bOccupied);
}

FVaRestOutcomeCallback FManagedOutcomeSlots::WrapOnGT(FVaRestOutcomeCallback& Original)
{
	auto Access = ReserveOnGT(&Original, nullptr);
	if (!Access)
		return {};
	return [Access = MoveTemp(Access)](FVaRestAttemptOutcome&& Outcome) { Access->InvokeOnGT(MoveTemp(Outcome)); };
}
TUniqueFunction<void()> FManagedOutcomeSlots::WrapTaskOnGT(TUniqueFunction<void()>& Original)
{
	auto Access = ReserveOnGT(nullptr, &Original);
	if (!Access)
		return {};
	return [Access = MoveTemp(Access)] { Access->InvokeTaskOnGT(); };
}
TSharedPtr<FManagedNativeOutcomeAccess, ESPMode::ThreadSafe> FManagedOutcomeSlots::ReserveOnGT(
	FVaRestOutcomeCallback* Outcome, TUniqueFunction<void()>* Task)
{
	check(IsInGameThread());
	if ((Outcome != nullptr) == (Task != nullptr))
		return {};
	const auto HasCallback = [&] { return Outcome ? bool(*Outcome) : bool(*Task); };
	if (Ref.Control->IsStopped() || !HasCallback())
		return {};
	ServiceReadyOnGT();                             // bounded local reuse; a completed callback never becomes per-retry history
	if (Ref.Control->IsStopped() || !HasCallback()) // actual callback destructors may reenter Stop or complete the caller
		return {};
	for (uint8 Index = 0; Index < Slots.size(); ++Index)
	{
		auto& Slot = Slots[Index];
		if (Slot.bOccupied || Slot.bRetired)
			continue;
		auto Tail = Ref.Store->TryAddLogicalTailOnGT(Ref);
		if (!Tail.IsHeld())
			return {};
		Slot.bOccupied = true;
		Slot.bInvoked = false;
		Slot.bExecuting = false;
		Slot.bAccessEnded.store(false, std::memory_order_relaxed);
		if (Outcome)
			Slot.Original = MoveTemp(*Outcome);
		else
			Slot.Task = MoveTemp(*Task);
		auto Access = MakeShared<FManagedNativeOutcomeAccess, ESPMode::ThreadSafe>(Ref, MoveTemp(Tail), this, Index, Slot.Generation);
		return Access;
	}
	return {};
}

FManagedOutcomeSlots::FSlot& FManagedOutcomeSlots::HeldSlot(uint8 Index, uint64 Generation)
{
	check(Index < Slots.size());
	auto& Slot = Slots[Index];
	check(Slot.bOccupied && Slot.Generation == Generation);
	return Slot; // active native access retains the real authority through every callback and nested destructor
}

void FManagedOutcomeSlots::InvokeOnGT(uint8 Index, uint64 Generation, FVaRestAttemptOutcome&& Outcome)
{
	check(IsInGameThread());
	const auto Control = Ref.Control;
	auto& Slot = HeldSlot(Index, Generation);
	if (Control->IsStopped() || Slot.bExecuting || Slot.bInvoked || !Slot.Original)
		return;
	Slot.bInvoked = true;
	Slot.bExecuting = true;
	Slot.Original(MoveTemp(Outcome));
	Slot.Original = nullptr; // actual captured owners die on GT BEFORE the invocation self-pin can settle its authority
	Slot.bExecuting = false;
}

void FManagedOutcomeSlots::InvokeTaskOnGT(uint8 Index, uint64 Generation)
{
	check(IsInGameThread());
	const auto Control = Ref.Control;
	auto& Slot = HeldSlot(Index, Generation);
	if (Control->IsStopped() || Slot.bExecuting || Slot.bInvoked || !Slot.Task)
		return;
	Slot.bInvoked = true;
	Slot.bExecuting = true;
	Slot.Task();
	Slot.Task = nullptr;
	Slot.bExecuting = false;
}

void FManagedOutcomeSlots::ServiceReadyOnGT()
{
	check(IsInGameThread());
	if (bServicing)
		return;
	const auto Control = Ref.Control;
	bServicing = true;
	for (auto& Slot : Slots)
	{
		if (!Slot.bOccupied || Slot.bExecuting || !Slot.bAccessEnded.load(std::memory_order_acquire))
			continue;
		Slot.bExecuting = true;
		Slot.Original = nullptr; // an uncalled adapter dropped on a worker leaves its ORIGINAL payload here for GT disposal
		Slot.Task = nullptr;
		Slot.bInvoked = false;
		Slot.bExecuting = false;
		Slot.bOccupied = false;
		if (Slot.Generation == MAX_uint64)
			Slot.bRetired = true;
		else
			++Slot.Generation;
	}
	bServicing = false;
}
} // namespace VaRest::Http
