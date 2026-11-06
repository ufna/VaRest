// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestManagedDispatch.h"
#include "Tasks/Task.h"
#if WITH_DEV_AUTOMATION_TESTS
#include "HAL/PlatformProcess.h"
#endif

namespace VaRest::Http
{
/** No affine owner lives in this object. Last destruction settles the already-counted authority on its actual thread. */
class FManagedNativeDispatchAccess final : public TSharedFromThis<FManagedNativeDispatchAccess, ESPMode::ThreadSafe>
{
public:
	FManagedNativeDispatchAccess(FNativeOperationRef InRef, FNativeDispatchSettlementLease&& InLease,
		FManagedDispatchSlots* InSlots, uint8 InIndex, uint64 InGeneration)
		: Ref(MoveTemp(InRef))
		, Lease(MoveTemp(InLease))
		, Owner(InSlots)
		, Index(InIndex)
		, Generation(InGeneration)
	{
		check(Ref.IsValid() && Lease.IsHeld() && Owner);
	}
	~FManagedNativeDispatchAccess()
	{
		Owner->HeldSlot(Index, Generation).bAccessEnded.store(true, std::memory_order_release); // LAST slot access, after ALL result/clock/task use
		Lease.Reset();                                                                          // complete native settle method, LAST cell access; Ref retains the exact Control through return
	}
	const FDispatchTag& TagOnGT()
	{
		check(IsInGameThread());
		return Owner->HeldSlot(Index, Generation).Tag;
	}
	IVaRestClock& Clock() const
	{
		return *Owner->HeldSlot(Index, Generation).OriginalClock;
	}
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Control() const { return Ref.Control; }
	void Publish(FVaRestTransportResult&& Result)
	{
		const auto Self = AsShared();
		const auto Control = Ref.Control;
		bool Expected = false;
		if (!bPublished.compare_exchange_strong(Expected, true, std::memory_order_acq_rel) || Control->IsStopped())
			return;
		auto& Slot = Owner->HeldSlot(Index, Generation);
		Slot.Result = MoveTemp(Result);
		Slot.bReady.store(true, std::memory_order_release); // GT may consume Result immediately; no further Result access
#if WITH_DEV_AUTOMATION_TESTS
		if (PublicationProbe)
		{
			PublicationProbe->Published.store(true, std::memory_order_relaxed); // progress witness, deliberately NOT a reclamation fence
			while (!PublicationProbe->Resume.load(std::memory_order_acquire))
				FPlatformProcess::SleepNoStats(0.0f);
		}
#endif
		if (Control->IsStopped())
			return;
		UE::Tasks::Launch(TEXT("VaRest.ManagedAttemptDone"), [Self] {
			const auto Control = Self->Ref.Control;
			if (!Control->IsStopped())
				Self->Owner->DeliverOnGT(Self->Index, Self->Generation); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}
	void Drop()
	{
		if (Ref.Control->IsStopped())
			return;
		FVaRestTransportResult Dropped;
		Dropped.Failure = EVaRestTransportFailure::Other;
		Dropped.FailureDetail = TEXT("TransportDropped");
		Publish(MoveTemp(Dropped));
	}
	FVaRestOutcomeCallback TakeUnstartedOnGT()
	{
		const auto Self = AsShared();
		const auto Control = Ref.Control;
		check(IsInGameThread());
		return Owner->TakeUnstartedOnGT(Index, Generation);
	}
#if WITH_DEV_AUTOMATION_TESTS
	void PausePublicationForTests(TSharedRef<FManagedDispatchPublicationProbe, ESPMode::ThreadSafe> Probe)
	{
		check(IsInGameThread());
		PublicationProbe = MoveTemp(Probe);
	}
#endif

private:
	FNativeOperationRef Ref;
	FNativeDispatchSettlementLease Lease;
	FManagedDispatchSlots* Owner;
	uint8 Index;
	uint64 Generation;
	std::atomic<bool> bPublished{false};
#if WITH_DEV_AUTOMATION_TESTS
	TSharedPtr<FManagedDispatchPublicationProbe, ESPMode::ThreadSafe> PublicationProbe;
#endif
};

/** Holds only the native access; the ORIGINAL strong clock owner stays in the operation's GT-owned slot. */
class FManagedDispatchClockView final : public IVaRestClock, public TSharedFromThis<FManagedDispatchClockView, ESPMode::ThreadSafe>
{
public:
	explicit FManagedDispatchClockView(TSharedRef<FManagedNativeDispatchAccess, ESPMode::ThreadSafe> InAccess)
		: Access(MoveTemp(InAccess))
	{
	}
	double Now() const override
	{
		const auto Self = AsShared();
		const auto Control = Access->Control();
		return Access->Clock().Now(); // EXACT original semantics, including already-owned late reads after Stopped
	}
	FVaRestTimerHandle SetTimer(double Delay, TUniqueFunction<void()>&& Callback) override
	{
		check(IsInGameThread());
		const auto Self = AsShared();
		const auto Control = Access->Control();
		if (Control->IsStopped())
		{
			Callback = nullptr; // GT closed refusal; actual FHttp transport uses only Now through Request.Clock
			return {};
		}
		return Access->Clock().SetTimer(Delay, MoveTemp(Callback));
	}
	void ClearTimer(FVaRestTimerHandle& Handle) override
	{
		check(IsInGameThread());
		const auto Self = AsShared();
		const auto Control = Access->Control();
		Access->Clock().ClearTimer(Handle);
	}

private:
	TSharedRef<FManagedNativeDispatchAccess, ESPMode::ThreadSafe> Access;
};

FManagedDispatchGuard::FManagedDispatchGuard(TSharedRef<FManagedNativeDispatchAccess, ESPMode::ThreadSafe> InAccess)
	: Access(MoveTemp(InAccess))
{
}
FManagedDispatchGuard::FManagedDispatchGuard(FManagedDispatchGuard&& Other) noexcept
	: Access(MoveTemp(Other.Access))
	, bResolved(Other.bResolved)
{
	Other.Access.Reset();
	Other.bResolved = true;
}
FManagedDispatchGuard::~FManagedDispatchGuard()
{
	if (Access && !bResolved)
		Access->Drop();
}
void FManagedDispatchGuard::Deliver(FVaRestTransportResult&& Result)
{
	if (!Access || bResolved)
		return;
	bResolved = true;
	Access->Publish(MoveTemp(Result)); // keep Access until ACTUAL guard destruction, even after invocation
}

FManagedDispatchPreparation::FManagedDispatchPreparation(FManagedDispatchGuard&& InGuard,
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe>&& InClock)
	: Guard(MoveTemp(InGuard))
	, ClockView(MoveTemp(InClock))
{
}
const FDispatchTag& FManagedDispatchPreparation::TagOnGT() const
{
	check(IsInGameThread() && IsValid());
	return Guard.Access->TagOnGT();
}
TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> FManagedDispatchPreparation::ClockViewOnGT() const
{
	check(IsInGameThread() && IsValid());
	return ClockView;
}
FVaRestTransportComplete FManagedDispatchPreparation::TakeCompletionOnGT()
{
	check(IsInGameThread() && IsValid() && !Guard.bResolved);
	return [Moved = FManagedDispatchGuard(MoveTemp(Guard))](FVaRestTransportResult&& Result) mutable { Moved.Deliver(MoveTemp(Result)); };
}
FVaRestOutcomeCallback FManagedDispatchPreparation::TakeUnstartedOutcomeOnGT()
{
	check(IsInGameThread() && IsValid() && !Guard.bResolved);
	Guard.bResolved = true;
	return Guard.Access->TakeUnstartedOnGT();
}
#if WITH_DEV_AUTOMATION_TESTS
void FManagedDispatchPreparation::PausePublicationForTests(TSharedRef<FManagedDispatchPublicationProbe, ESPMode::ThreadSafe> Probe)
{
	check(IsInGameThread() && IsValid() && !Guard.bResolved);
	Guard.Access->PausePublicationForTests(MoveTemp(Probe));
}
#endif

FManagedDispatchSlots::FManagedDispatchSlots(FNativeOperationRef InRef)
	: Ref(MoveTemp(InRef))
{
	check(IsInGameThread() && Ref.IsValid()); // lazy fixed storage, anchored before any preparation is exposed
}
FManagedDispatchSlots::~FManagedDispatchSlots()
{
	check(IsInGameThread());
	for (const auto& Slot : Slots)
		check(!Slot.bOccupied);
}
FManagedDispatchPreparation FManagedDispatchSlots::PrepareOnGT(FDispatchTag& Tag, FVaRestOutcomeCallback& Outcome,
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock, FManagedAttemptDelivery& Delivery)
{
	check(IsInGameThread());
	if (Ref.Control->IsStopped() || !Clock || !Outcome || !Delivery)
		return {};
	ServiceReadyOnGT(false);                               // local fixed32 cleanup lets sequential retries reuse slots without waiting for a cell sweep
	if (Ref.Control->IsStopped() || !Outcome || !Delivery) // cleanup destructors can reenter Stop/completion
		return {};
	for (uint8 Index = 0; Index < Slots.size(); ++Index)
	{
		auto& Slot = Slots[Index];
		if (Slot.bOccupied || Slot.bRetired)
			continue;
		auto Physical = Ref.Store->TryAddDispatchOnGT(Ref);
		if (!Physical.IsHeld())
			return {};
		Slot.bOccupied = true;
		Slot.bExecuting = false;
		Slot.bDelivered = false;
		Slot.bReady.store(false, std::memory_order_relaxed);
		Slot.bAccessEnded.store(false, std::memory_order_relaxed);
		Slot.Tag = MoveTemp(Tag);
		Slot.Outcome = MoveTemp(Outcome);
		Slot.Delivery = MoveTemp(Delivery);
		Slot.OriginalClock = MoveTemp(Clock);
		TSharedRef<FManagedNativeDispatchAccess, ESPMode::ThreadSafe> Access = MakeShared<FManagedNativeDispatchAccess, ESPMode::ThreadSafe>(
			Ref, MoveTemp(Physical), this, Index, Slot.Generation);
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> View = MakeShared<FManagedDispatchClockView, ESPMode::ThreadSafe>(Access);
		return FManagedDispatchPreparation(FManagedDispatchGuard(Access), MoveTemp(View));
	}
	return {};
}
FManagedDispatchSlots::FSlot& FManagedDispatchSlots::HeldSlot(uint8 Index, uint64 Generation)
{
	check(Index < Slots.size());
	auto& Slot = Slots[Index];
	check(Slot.bOccupied && Slot.Generation == Generation);
	return Slot; // native access holds its real physical authority through every use, including nested clock methods
}
void FManagedDispatchSlots::DeliverOnGT(uint8 Index, uint64 Generation)
{
	check(IsInGameThread());
	const auto Control = Ref.Control;
	auto& Slot = HeldSlot(Index, Generation);
	if (Control->IsStopped() || Slot.bExecuting || Slot.bDelivered || !Slot.bReady.load(std::memory_order_acquire))
		return;
	Slot.bDelivered = true;
	Slot.bExecuting = true;
	Slot.Delivery(MoveTemp(Slot.Tag), MoveTemp(Slot.Result), MoveTemp(Slot.Outcome));
	Slot.Delivery = nullptr;
	Slot.Outcome = nullptr;
	Slot.Tag = {};
	Slot.Result = {};
	Slot.bExecuting = false; // original clock survives until ALL actual guard/view/task/method owners end
}
FVaRestOutcomeCallback FManagedDispatchSlots::TakeUnstartedOnGT(uint8 Index, uint64 Generation)
{
	check(IsInGameThread());
	const auto Control = Ref.Control;
	if (Control->IsStopped())
		return {};
	auto& Slot = HeldSlot(Index, Generation);
	check(!Slot.bExecuting && !Slot.bDelivered && !Slot.bReady.load(std::memory_order_acquire));
	Slot.bDelivered = true;
	Slot.bExecuting = true;
	FVaRestOutcomeCallback Outcome = MoveTemp(Slot.Outcome);
	Slot.Delivery = nullptr;
	Slot.bExecuting = false;
	return Outcome; // actual caller invokes/destroys it on GT while its preparation keeps the physical authority
}
void FManagedDispatchSlots::ServiceReadyOnGT(bool bStopped)
{
	check(IsInGameThread());
	if (bServicing)
		return;
	const auto Control = Ref.Control;
	bServicing = true;
	for (uint8 Index = 0; Index < Slots.size(); ++Index)
	{
		auto& Slot = Slots[Index];
		if (!Slot.bOccupied || Slot.bExecuting || !Slot.bAccessEnded.load(std::memory_order_acquire))
			continue;
		// A named task discarded while LIVE may leave its actual result. This is an owned fallback, never sole delivery.
		if (!bStopped && !Control->IsStopped() && !Slot.bDelivered && Slot.bReady.load(std::memory_order_acquire))
			DeliverOnGT(Index, Slot.Generation);
		Slot.bExecuting = true;
		Slot.Delivery = nullptr;
		Slot.Outcome = nullptr;
		Slot.Tag = {};
		Slot.Result = {};
		Slot.OriginalClock.Reset(); // no native method/view can read it after the actual shared access has ended
		Slot.bReady.store(false, std::memory_order_relaxed);
		Slot.bDelivered = false;
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
