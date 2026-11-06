// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeLifetime.h"
#include "VaRestClient.h"
#include "VaRestNativeClientSlots.h"
#include "VaRestClientState.h"

namespace VaRest::Http
{
FNativeInputBorrow::FNativeInputBorrow(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> InControl, FRequestSpec* InInput, bool bInSender)
	: Control(MoveTemp(InControl))
	, Input(InInput)
	, bSender(bInSender)
{
}

FNativeInputBorrow::~FNativeInputBorrow()
{
	Reset();
}

FNativeInputBorrow::FNativeInputBorrow(FNativeInputBorrow&& Other) noexcept
	: Control(MoveTemp(Other.Control))
	, Input(Other.Input)
	, bSender(Other.bSender)
{
	Other.Input = nullptr;
	Other.bSender = false;
}

FNativeInputBorrow& FNativeInputBorrow::operator=(FNativeInputBorrow&& Other) noexcept
{
	if (this != &Other)
	{
		Reset();
		Control = MoveTemp(Other.Control);
		Input = Other.Input;
		bSender = Other.bSender;
		Other.Input = nullptr;
		Other.bSender = false;
	}
	return *this;
}

FRequestSpec& FNativeInputBorrow::Get() const
{
	check(Input && Control.IsValid());
	check(bSender || Input->bNativeInputReady.load(std::memory_order_acquire));
	return *Input;
}

bool FNativeInputBorrow::IsUnpublishedSenderFor(const FNativeLifetimeControl* Expected) const
{
	return Input && bSender && Control.Get() == Expected && !Input->bNativeInputReady.load(std::memory_order_acquire);
}

FNativeInputBorrow FNativeInputBorrow::ForkBeforePublication() const
{
	check(Input && bSender && !Input->bNativeInputReady.load(std::memory_order_acquire));
	check(!Input->bNativeInputFinished.load(std::memory_order_acquire));
	if (Control->IsStopped() || !FNativeLifetimeControl::TryBorrowInput(Input))
		return FNativeInputBorrow();
	return FNativeInputBorrow(Control.ToSharedRef(), Input, false);
}

bool FNativeInputBorrow::PublishReady()
{
	check(Input && bSender && !Input->bNativeInputReady.load(std::memory_order_acquire));
	const bool bLive = !Control->IsStopped();
	if (!bLive)
	{
		Input->bNativeInputDiscarded.store(true, std::memory_order_release);
		Input->NativeInputBorrows.fetch_or(ClosedBit, std::memory_order_acq_rel);
		Input->bNativeInputFinished.store(true, std::memory_order_release);
	}
	Input->bNativeInputReady.store(true, std::memory_order_release);
	return bLive;
}

void FNativeInputBorrow::Finish(bool bDiscarded)
{
	check(Input && Input->bNativeInputReady.load(std::memory_order_acquire));
	if (bDiscarded)
		Input->bNativeInputDiscarded.store(true, std::memory_order_release);
	Input->NativeInputBorrows.fetch_or(ClosedBit, std::memory_order_acq_rel);
	Input->bNativeInputFinished.store(true, std::memory_order_release);
}

void FNativeInputBorrow::Reset()
{
	FRequestSpec* Releasing = Input;
	Input = nullptr;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = MoveTemp(Control);
	if (!Releasing)
		return;
	if (bSender && !Releasing->bNativeInputReady.load(std::memory_order_acquire))
	{
		// Unpublished initialization unwind still has a Control owner. No public result or replacement task is made.
		Releasing->bNativeInputDiscarded.store(true, std::memory_order_release);
		Releasing->NativeInputBorrows.fetch_or(ClosedBit, std::memory_order_acq_rel);
		Releasing->bNativeInputFinished.store(true, std::memory_order_release);
		Releasing->bNativeInputReady.store(true, std::memory_order_release);
	}
	bSender = false;
	const uint64 Before = Releasing->NativeInputBorrows.fetch_sub(1, std::memory_order_release);
	check((Before & CountMask) != 0);
	// No input access follows the decrement: GT may now delete it. Kept only releases a native Control reference.
}

TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> FNativeLifetimeControl::Create()
{
	check(IsInGameThread());
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Created = MakeShareable(new FNativeLifetimeControl());
	Created->ClientSlots = MakeUnique<FNativeClientSlots>(Created);
	return Created;
}

FNativeLifetimeControl::FNativeLifetimeControl() = default;

FNativeLifetimeControl::~FNativeLifetimeControl()
{
	check(ClientSlots && ClientSlots->NumOccupied() == 0);
	check(RegisteredInputs.load(std::memory_order_seq_cst) == 0);
	check(!Incoming.load(std::memory_order_acquire) && !IncomingCursor && !DeferredHead && !DeferredTail);
}

FNativeClientSlots& FNativeLifetimeControl::ClientSlotsOnGT()
{
	check(IsInGameThread() && ClientSlots);
	return *ClientSlots;
}

FNativeInputBorrow FNativeLifetimeControl::AdoptInput(TUniquePtr<FRequestSpec>& Input)
{
	check(Input.IsValid());
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Self = AsShared();
	// Separate from operation admission: even a refused whole payload can still own GT-affine data during retirement.
	auto InputGate=Input->State ? Input->State->GetManagedLifetime().Gate : nullptr;
	const bool bCountedInput=InputGate && InputGate->TryCountInput();
	// Paired seq_cst order with Stop: either Stop sees this registration or this sender sees Stopped and self-holds.
	const uint64 Before = RegisteredInputs.fetch_add(1, std::memory_order_seq_cst);
	check(Before != MAX_uint64);
	if (IsStopped())
		EnsureExitHold();
	FRequestSpec* Owned = Input.Release();
	if (bCountedInput) Owned->NativeInputGate=MoveTemp(InputGate);
	Owned->bSent = true;
	Owned->NativeInputBorrows.store(1, std::memory_order_relaxed);
	Owned->bNativeInputReady.store(false, std::memory_order_relaxed);
	Owned->bNativeInputFinished.store(false, std::memory_order_relaxed);
	Owned->bNativeInputDiscarded.store(false, std::memory_order_relaxed);
	FRequestSpec* Head = Incoming.load(std::memory_order_relaxed);
	do
	{
		Owned->NativeInputNext = Head;
	}
	while (!Incoming.compare_exchange_weak(Head, Owned, std::memory_order_release, std::memory_order_relaxed));
	return FNativeInputBorrow(MoveTemp(Self), Owned, true);
}

FNativeInputBorrow FNativeLifetimeControl::BorrowInputOnGT(const FNativeInputBorrow& Existing)
{
	check(IsInGameThread());
	check(Existing.Control.Get() == this && Existing.Input);
	FRequestSpec* Input = Existing.Input;
	if (IsStopped() || !Input->bNativeInputReady.load(std::memory_order_acquire) || !TryBorrowInput(Input))
		return FNativeInputBorrow();
	return FNativeInputBorrow(AsShared(), Input, false);
}

bool FNativeLifetimeControl::TryBorrowInput(FRequestSpec* Input)
{
	uint64 Before = Input->NativeInputBorrows.load(std::memory_order_acquire);
	for (;;)
	{
		if ((Before & FNativeInputBorrow::ClosedBit) != 0)
			return false;
		check((Before & FNativeInputBorrow::CountMask) != FNativeInputBorrow::CountMask);
		if (Input->NativeInputBorrows.compare_exchange_weak(Before, Before + 1, std::memory_order_acq_rel, std::memory_order_acquire))
			return true;
	}
}

void FNativeLifetimeControl::AppendDeferred(FRequestSpec* Input)
{
	Input->NativeInputNext = nullptr;
	if (DeferredTail)
		DeferredTail->NativeInputNext = Input;
	else
		DeferredHead = Input;
	DeferredTail = Input;
}

uint32 FNativeLifetimeControl::CollectInputsOnGT(uint32 Budget)
{
	check(IsInGameThread());
	if (bCollecting)
		return 0; // payload destructors can reenter native APIs; the outer sweep remains the sole list mutator
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = AsShared();
	bCollecting = true;
#if WITH_DEV_AUTOMATION_TESTS
	LastInputScanVisits = 0;
#endif
	uint32 Collected = 0;
	for (uint32 Step = 0; Step < Budget; ++Step)
	{
		if (!IncomingCursor)
			IncomingCursor = Incoming.exchange(nullptr, std::memory_order_acq_rel);
		if (!IncomingCursor && !DeferredHead)
			break;
		FRequestSpec* Input;
		if (DeferredHead && (bPreferDeferred || !IncomingCursor))
		{
			Input = DeferredHead;
			DeferredHead = Input->NativeInputNext;
			if (!DeferredHead)
				DeferredTail = nullptr;
		}
		else
		{
			Input = IncomingCursor;
			IncomingCursor = Input->NativeInputNext;
		}
		bPreferDeferred = !bPreferDeferred;
		Input->NativeInputNext = nullptr;
#if WITH_DEV_AUTOMATION_TESTS
		++LastInputScanVisits;
#endif
		if (Input->bNativeInputReady.load(std::memory_order_acquire) && Input->bNativeInputFinished.load(std::memory_order_acquire) && (Input->NativeInputBorrows.load(std::memory_order_acquire) & FNativeInputBorrow::CountMask) == 0)
		{
			auto InputGate=MoveTemp(Input->NativeInputGate);
			delete Input; // complete payload, including historical config and arbitrary callback captures, dies on GT
			if (InputGate) InputGate->ReleaseInput(); // a reentrant destructor saw this input counted until AFTER it returned
			const uint64 Before = RegisteredInputs.fetch_sub(1, std::memory_order_seq_cst);
			check(Before != 0);
			++Collected;
		}
		else
			AppendDeferred(Input);
	}
	bCollecting = false;
	return Collected;
}

void FNativeLifetimeControl::EnsureExitHold()
{
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = AsShared();
	bool Expected = false;
	if (bExitHoldClaimed.compare_exchange_strong(Expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
		ExitSelfHold = MoveTemp(Kept);
}

void FNativeLifetimeControl::StopInputsOnGT()
{
	check(IsInGameThread());
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = AsShared(); // cleanup can reset the caller's last owner
	ClientSlots->BeginStop();                                                  // every gate/store is sealed BEFORE any worker can observe Stopped
	bStopped.store(true, std::memory_order_seq_cst);
	ClientSlots->DropRefreshCallbacksAtStopOnGT(); // GT-owned payloads; native wake/ticket authorities retain this allocation
	ClientSlots->SweepOperations();
	CollectInputsOnGT();
	if (RegisteredInputs.load(std::memory_order_seq_cst) != 0 || ClientSlots->NumOccupied() != 0)
		EnsureExitHold();
}

#if WITH_DEV_AUTOMATION_TESTS
bool FNativeLifetimeControl::DropQuiescentExitHoldForTests()
{
	check(IsInGameThread());
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = AsShared();
	if (!IsStopped() || NumPendingInputs() != 0 || ClientSlots->NumOccupied() != 0)
		return false;
	ExitSelfHold.Reset(); // test teardown only, after all actual producers have joined; once-claim flag stays closed
	return true;
}
#endif

FClientWorkLease::~FClientWorkLease()
{
	Release();
}

FClientWorkLease::FClientWorkLease(FClientWorkLease&& Other) noexcept
	: Gate(MoveTemp(Other.Gate))
{
}

FClientWorkLease& FClientWorkLease::operator=(FClientWorkLease&& Other) noexcept
{
	if (this != &Other)
	{
		Release();
		Gate = MoveTemp(Other.Gate);
	}
	return *this;
}

void FClientWorkLease::Release()
{
	// Clear the lease before the release decrement. A later GT zero observation must not race a row-field reset.
	TSharedPtr<FClientAdmissionGate, ESPMode::ThreadSafe> Releasing = MoveTemp(Gate);
	if (Releasing.IsValid())
	{
		const uint64 Before = Releasing->Word.fetch_sub(1, std::memory_order_release);
		check((Before & FClientAdmissionGate::CountMask) != 0);
	}
}

bool FClientAdmissionGate::TryAcquire(FClientWorkLease& OutLease)
{
	if (OutLease.IsHeld())
		return false;
	TSharedRef<FClientAdmissionGate, ESPMode::ThreadSafe> Self = AsShared();
	uint64 Before = Word.load(std::memory_order_acquire);
	for (;;)
	{
		if ((Before & ClosedBit) != 0 || (Before & CountMask) >= MaxOwnedOperations)
			return false;
		if (Word.compare_exchange_weak(Before, Before + 1, std::memory_order_acq_rel, std::memory_order_acquire))
		{
			OutLease.Gate = MoveTemp(Self);
			return true;
		}
	}
}

void FClientAdmissionGate::ClosePermanently()
{
	check(IsInGameThread());
	Word.fetch_or(ClosedBit, std::memory_order_acq_rel);
	InputWord.fetch_or(ClosedBit,std::memory_order_acq_rel); // failed later input capture implies operation admission is closed too
}

bool FClientAdmissionGate::TryCountInput()
{
	uint64 Before=InputWord.load(std::memory_order_acquire);
	for (;;)
	{
		if (Before&ClosedBit) return false;
		if ((Before&CountMask)==CountMask)
		{
			bInputCountOverflow.store(true,std::memory_order_release); // fail closed: never assert false cleanup quiescence
			return false;
		}
		if (InputWord.compare_exchange_weak(Before,Before+1,std::memory_order_acq_rel,std::memory_order_acquire)) return true;
	}
}
void FClientAdmissionGate::ReleaseInput()
{
	const uint64 Before=InputWord.fetch_sub(1,std::memory_order_release);
	check((Before&CountMask)!=0);
}
uint64 FClientAdmissionGate::NumOwnedInputs() const
{
	if (bInputCountOverflow.load(std::memory_order_acquire)) return MAX_uint64;
	return InputWord.load(std::memory_order_acquire)&CountMask;
}

bool FClientAdmissionGate::IsClosed() const
{
	return (Word.load(std::memory_order_acquire) & ClosedBit) != 0;
}

uint32 FClientAdmissionGate::NumOwnedOperations() const
{
	return static_cast<uint32>(Word.load(std::memory_order_acquire) & CountMask);
}

FManagedOperationSettlement::FManagedOperationSettlement(FClientWorkLease&& Work, uint32 InitialTails)
	: AdmittedWork(MoveTemp(Work))
	, PendingLogicalTails(InitialTails)
{
	check(AdmittedWork.IsHeld());
	check(InitialTails != 0 && InitialTails <= MaxLogicalTails);
}

FManagedOperationSettlement::~FManagedOperationSettlement()
{
	// The Control must reclaim this payload on GT only after all exact-generation authorities have been consumed.
	check(!AdmittedWork.IsHeld());
	check(NumLogicalTails() == 0 && NumDispatchSettlements() == 0);
}

bool FManagedOperationSettlement::TryAddLogicalTail()
{
	check(IsInGameThread());
	if (bChildAdmissionClosed.load(std::memory_order_seq_cst) || NumLogicalTails() >= MaxLogicalTails)
		return false;
	// GT is the only incrementing thread, with no external callout between the closed/cap checks and this increment.
	const uint32 Before = PendingLogicalTails.fetch_add(1, std::memory_order_seq_cst);
	check(Before < MaxLogicalTails);
	return true;
}

bool FManagedOperationSettlement::TryAddDispatchSettlement()
{
	check(IsInGameThread());
	if (bChildAdmissionClosed.load(std::memory_order_seq_cst) || NumDispatchSettlements() >= MaxDispatchSettlements)
		return false;
	const uint32 Before = PendingDispatchSettlements.fetch_add(1, std::memory_order_seq_cst);
	check(Before < MaxDispatchSettlements);
	return true;
}

void FManagedOperationSettlement::CloseChildAdmission()
{
	check(IsInGameThread());
	bChildAdmissionClosed.store(true, std::memory_order_seq_cst);
}

void FManagedOperationSettlement::MarkLogicalTerminalSettled()
{
	check(IsInGameThread());
	check(bChildAdmissionClosed.load(std::memory_order_seq_cst));
	bLogicalTerminalSettled.store(true, std::memory_order_seq_cst);
	TryReleaseWorkLease();
}

void FManagedOperationSettlement::MarkDiscardedAtStop()
{
	// The stopped Control has already barred GT child acquisition. This path cannot publish a normal terminal result.
	bDiscardedAtStop.store(true, std::memory_order_seq_cst);
	bChildAdmissionClosed.store(true, std::memory_order_seq_cst);
	bLogicalTerminalSettled.store(true, std::memory_order_seq_cst);
	TryReleaseWorkLease();
}

void FManagedOperationSettlement::SettleLogicalTail()
{
	const uint32 Before = PendingLogicalTails.fetch_sub(1, std::memory_order_seq_cst);
	check(Before != 0);
	TryReleaseWorkLease();
}

void FManagedOperationSettlement::SettleDispatch()
{
	const uint32 Before = PendingDispatchSettlements.fetch_sub(1, std::memory_order_seq_cst);
	check(Before != 0);
	TryReleaseWorkLease();
}

void FManagedOperationSettlement::TryReleaseWorkLease()
{
	// All predicate-changing transitions and reads share one seq_cst order. Its last final transition must see
	// the other final states; separate acquire/release objects allow both final callers to miss each other.
	if (!bLogicalTerminalSettled.load(std::memory_order_seq_cst) || !bChildAdmissionClosed.load(std::memory_order_seq_cst) || NumLogicalTails() != 0 || NumDispatchSettlements() != 0)
		return;
	bool Expected = false;
	if (bWorkLeaseReleased.compare_exchange_strong(Expected, true, std::memory_order_seq_cst, std::memory_order_seq_cst))
		AdmittedWork.Release();
}
} // namespace VaRest::Http
