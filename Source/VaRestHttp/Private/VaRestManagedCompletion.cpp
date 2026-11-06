// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestManagedCompletion.h"
#include "Tasks/Task.h"
#include "VaRestClientState.h"
#include "VaRestManagedOperation.h"

namespace VaRest::Http
{
/** At most ONE named-GTK cancel ticket per operation; all copies retain the SAME preadmitted Completion obligation. */
class FManagedCancelTask final
{
public:
	explicit FManagedCancelTask(FManagedCompletionRef InAccess)
		: Access(MoveTemp(InAccess))
	{
	}
	~FManagedCancelTask()
	{
		Access->bCancelTaskEnded.store(true, std::memory_order_release); // LAST access before its real native shared reference ends
	}
	void RunOnGT() { Access->CancelOnGT(); }

private:
	FManagedCompletionRef Access;
};

/** Native worker borrow only. Its actual permission/callable remain in the GTK-owned frame until ended acquire. */
class FManagedCompletionWorker final : public TSharedFromThis<FManagedCompletionWorker, ESPMode::ThreadSafe>
{
public:
	FManagedCompletionWorker(FManagedCompletionRef InCompletion, uint64 InGeneration)
		: Completion(MoveTemp(InCompletion))
		, Generation(InGeneration)
	{
	}
	~FManagedCompletionWorker()
	{
		auto& Frame = Completion->Payload();
		check(Frame.WorkerGeneration == Generation);
		Frame.bWorkerEnded.store(true, std::memory_order_release); // LAST frame use; Completion keeps actual row/allocation through return
	}
	void Run()
	{
		const auto Self = AsShared();
		const auto Control = Completion->Control();
		auto& Frame = Completion->Payload();
		if (!Control->IsStopped())
		{
			Frame.bWorkerDidRun.store(true, std::memory_order_relaxed);
			Frame.WorkerKernel(); // ACTUAL callable/handler/finish are owned by Frame, not this worker or its closure
		}
	}

private:
	FManagedCompletionRef Completion;
	uint64 Generation;
};

FManagedCompletionPayload::FManagedCompletionPayload(FNativeOperationRef InRef, FVaRestExchange* InTarget)
	: Ref(MoveTemp(InRef))
	, Target(InTarget)
{
	check(IsInGameThread() && Ref.IsValid() && Target);
}
FManagedCompletionPayload::~FManagedCompletionPayload()
{
	check(IsInGameThread() && bDataDisposed && !Ref.IsValid());
	check(!ReturnedCompletion.IsHeld() && !WorkerPermission.IsHeld() && !DeliveryWorker.IsHeld());
}
void FManagedCompletionPayload::DisposeDataOnGT(bool bDiscarded)
{
	check(IsInGameThread() && !bWorkerActive);
	if (bCleaning || bDataDisposed)
		return;
	const auto Control = Ref.Control;
	bCleaning = true;
	auto Discard = MoveTemp(DiscardKernel);
	if (bDiscarded && Discard)
		Discard(); // actual native bookkeeping only, no normal Completed/user callback/dead service lookup
	Handler.Reset();
	Finish = FFinish();
	TerminalObserver = nullptr;
	ReplayBody.Reset();
	WorkerKernel = nullptr;
	AfterWorker = nullptr;
	CompleteKernel = nullptr;
	DiscardKernel = nullptr;
	DeliveryWorker.Reset();
	WorkerPermission.Reset();
	bQueuedGTK = false;
	bDataDisposed = true;
	bCleaning = false;
}
void FManagedCompletionPayload::ServiceReadyOnGT(bool bStopped)
{
	check(IsInGameThread());
	if (bServicing || !bAccessEnded.load(std::memory_order_acquire))
		return;
	const auto Control = Ref.Control;
	bServicing = true;
	if (bWorkerActive)
	{
		check(bWorkerEnded.load(std::memory_order_acquire)); // every worker token owns Completion until after its ended publication
		bWorkerActive = false;
	}
	if (ReturnedCompletion.IsHeld() && !bStopped && !Control->IsStopped() && !bDataDisposed)
	{
		auto Access = FManagedCompletionAccess::CreateOnGT(*this, MoveTemp(ReturnedCompletion));
		bQueuedGTK = false; // old native GTK continuation has actually ended; default delivery is still the named task
		if (WorkerPermission.IsHeld())
			Access->WorkerReturnedOnGT();
		else if (!bFinalized)
			Access->CompleteOnGT();
		else
			Access->DisposeOnGT(false);
	}
	else
	{
		DisposeDataOnGT(bStopped || Control->IsStopped());
		ReturnedCompletion.Reset(); // actual GTK destruction precedes final logical release
	}
	bServicing = false;
}
void FManagedCompletionPayload::DestroyOnGT(bool bDiscarded)
{
	check(IsInGameThread() && !ReturnedCompletion.IsHeld());
	check(!WorkerPermission.IsHeld() && !DeliveryWorker.IsHeld() && !bWorkerActive);
	DisposeDataOnGT(bDiscarded);
	Target = nullptr;
	Ref = {};
}

FManagedCompletionAccess::FManagedCompletionAccess(FManagedCompletionPayload& InOwner, FNativeLogicalTailLease&& InCompletion)
	: Ref(InCompletion.Reference())
	, Completion(MoveTemp(InCompletion))
{
	BindFrameOnGT(InOwner);
}
FManagedCompletionAccess::FManagedCompletionAccess(FManagedOperationPayload& InOperation, FNativeLogicalTailLease&& InCompletion)
	: Ref(InCompletion.Reference())
	, Completion(MoveTemp(InCompletion))
	, Operation(&InOperation)
{
	check(Completion.IsHeld() && Operation->MatchesOperation(Ref)); // Claim remains exclusive until infallible publication
}
void FManagedCompletionAccess::BindFrameOnGT(FManagedCompletionPayload& Frame)
{
	check(IsInGameThread() && Completion.IsHeld() && !Owner && !BoundFrame.load(std::memory_order_relaxed));
	check(Ref.Control == Frame.Ref.Control && Ref.Store == Frame.Ref.Store && Ref.Cell.Index == Frame.Ref.Cell.Index && Ref.Cell.Generation == Frame.Ref.Cell.Generation);
	check(!Frame.ReturnedCompletion.IsHeld());
	Owner = &Frame;
	Owner->bAccessEnded.store(false, std::memory_order_relaxed);
	BoundFrame.store(Owner, std::memory_order_release); // final native destructor/worker borrows never race a GTK frame binding
}
FManagedOperationPayload& FManagedCompletionAccess::OperationOnGT() const
{
	check(IsInGameThread() && Completion.IsHeld() && Operation);
	return *Operation;
}
FManagedCompletionRef FManagedCompletionAccess::CreateOnGT(FManagedCompletionPayload& Owner, FNativeLogicalTailLease&& Completion)
{
	return MakeShareable(new FManagedCompletionAccess(Owner, MoveTemp(Completion)));
}
FManagedCompletionRef FManagedCompletionAccess::CreateForClaim(FManagedOperationPayload& Operation, FNativeLogicalTailLease&& Completion)
{
	return MakeShareable(new FManagedCompletionAccess(Operation, MoveTemp(Completion)));
}
FManagedCompletionAccess::~FManagedCompletionAccess()
{
	auto* Frame = BoundFrame.load(std::memory_order_acquire);
	if (!Frame)
	{
		Completion.Reset(); // native-only unselected initial tail; actual whole Ex/spec still belongs to GTK anchor reclamation
		return;             // LAST cell access; Ref retains captured allocation through complete native settle method
	}
	const bool bDropHere = IsInGameThread() && Frame->bDataDisposed;
	if (!bDropHere)
		Frame->ReturnedCompletion = MoveTemp(Completion);       // native-only mailbox; GTK reads it only after ended acquire
	Frame->bAccessEnded.store(true, std::memory_order_release); // LAST frame access
	if (bDropHere)
		Completion.Reset(); // LAST cell access; exact Ref retains allocation through full native settle method
}
void FManagedCompletionAccess::QueueCancel()
{
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	if (Control->IsStopped())
		return;
	bool Expected = false;
	if (!bCancelPosted.compare_exchange_strong(Expected, true, std::memory_order_acq_rel))
		return; // immediate atomic cancellation is already recorded; do not append GTK callback/history or acquire a child
	TSharedPtr<FManagedCancelTask, ESPMode::ThreadSafe> Ticket = MakeShared<FManagedCancelTask, ESPMode::ThreadSafe>(Self);
	if (Control->IsStopped())
		return; // a paused caller still owns the SAME counted Completion until this native stack unwinds
	UE::Tasks::Launch(TEXT("VaRest.ManagedCancel"), [Held = MoveTemp(Ticket)] { Held->RunOnGT(); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}
void FManagedCompletionAccess::CancelOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	if (Control->IsStopped() || !Operation)
		return;
	bCancelInvoked = true;
	const auto Target = Operation->Exchange;
	if (!Target || Target->IsDone())
		return;
	const auto State = Internals(*Target).State;
	if (State)
		State->Cancel(Target.ToSharedRef(), EVaRestCancelReason::User); // actual existing GTK API stack; no copied completion
}
void FManagedCompletionAccess::ServicePendingCancelOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	if (bCancelPosted.load(std::memory_order_acquire) && bCancelTaskEnded.load(std::memory_order_acquire) && !bCancelInvoked)
		CancelOnGT(); // actual uncalled native task ended; NOT the normal latency/delivery route
}
void FManagedCompletionAccess::CompleteOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	if (Control->IsStopped())
	{
		if (!Owner->bWorkerActive)
			Owner->DisposeDataOnGT(true);
		return;
	}
	// Complete may dispose its frame or reenter Stop. Its executing callable has a GTK local owner until return.
	auto Kernel = MoveTemp(Owner->CompleteKernel);
	if (!Owner->bDataDisposed && Kernel)
		Kernel(Self);
	if (!Owner->bDataDisposed)
		Owner->CompleteKernel = MoveTemp(Kernel);
}
void FManagedCompletionAccess::QueueCompleteOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	if (Control->IsStopped() || Owner->bQueuedGTK)
		return;
	Owner->bQueuedGTK = true;
	UE::Tasks::Launch(TEXT("VaRest.ManagedComplete"), [Self] {
		Self->Owner->bQueuedGTK = false;
		Self->CompleteOnGT(); }, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
}
bool FManagedCompletionAccess::StartWorkerOnGT(FNativeLogicalTailLease&& Permission, TUniqueFunction<void()>&& Work, FManagedCompletionKernel&& After)
{
	check(IsInGameThread() && Permission.IsHeld() && Work && After && !Owner->bWorkerActive && !Owner->WorkerPermission.IsHeld());
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	const auto& WorkerRef = Permission.Reference();
	check(WorkerRef.Control == Ref.Control && WorkerRef.Store == Ref.Store && WorkerRef.Cell.Index == Ref.Cell.Index && WorkerRef.Cell.Generation == Ref.Cell.Generation);
	if (Control->IsStopped() || Owner->WorkerGeneration == MAX_uint64)
		return false;
	Owner->WorkerPermission = MoveTemp(Permission);
	Owner->WorkerKernel = MoveTemp(Work);
	Owner->AfterWorker = MoveTemp(After);
	LaunchWorkerOnGT();
	return true;
}
void FManagedCompletionAccess::LaunchWorkerOnGT()
{
	check(IsInGameThread() && Owner->WorkerPermission.IsHeld());
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	if (Control->IsStopped())
	{
		Owner->bWorkerActive = false;
		Owner->DisposeDataOnGT(true);
		return;
	}
	check(Owner->WorkerGeneration != MAX_uint64); // only decode and delivery normally; retries inherit this SAME still-unexecuted permission
	++Owner->WorkerGeneration;
	Owner->bWorkerActive = true;
	Owner->bWorkerEnded.store(false, std::memory_order_relaxed);
	Owner->bWorkerDidRun.store(false, std::memory_order_relaxed);
	TSharedPtr<FManagedCompletionWorker, ESPMode::ThreadSafe> Token = MakeShared<FManagedCompletionWorker, ESPMode::ThreadSafe>(Self, Owner->WorkerGeneration);
	UE::Tasks::FTaskEvent Published(TEXT("VaRest.ManagedCompletionPublished"));
	auto Worker = UE::Tasks::Launch(TEXT("VaRest.ManagedCompletionWorker"), [Held = MoveTemp(Token)] { Held->Run(); }, UE::Tasks::Prerequisites(Published));
	// GTK handback exists BEFORE any worker completion, even when Stop makes the worker skip user/decoder work.
	UE::Tasks::Launch(TEXT("VaRest.ManagedCompletionReturn"), [Self] { Self->WorkerReturnedOnGT(); }, UE::Tasks::Prerequisites(Worker), UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	Published.Trigger(); // actual Task.h prerequisite; worker cannot run before BOTH GTK-owned continuations were published
}
void FManagedCompletionAccess::WorkerReturnedOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	const auto Control = Ref.Control;
	if (!Owner->bWorkerEnded.load(std::memory_order_acquire))
		return; // any remaining real worker token still retains Completion; its final mailbox publication permits owned GTK service
	Owner->bWorkerActive = false;
	if (Control->IsStopped())
	{
		Owner->DisposeDataOnGT(true);
		return;
	}
	if (!Owner->bWorkerDidRun.load(std::memory_order_relaxed) && Owner->WorkerGeneration != MAX_uint64)
	{
		LaunchWorkerOnGT(); // abandoned LIVE task executed nothing: inherit same permission/fields, never fabricate decoded success
		return;
	}
	Owner->WorkerKernel = nullptr; // actual callable captures die on GTK AFTER ended acquire, BEFORE worker permission settles
	Owner->WorkerPermission.Reset();
	auto After = MoveTemp(Owner->AfterWorker);
	if (After)
		After(Self);
}
void FManagedCompletionAccess::DisposeOnGT(bool bDiscarded)
{
	check(IsInGameThread() && !Owner->bWorkerActive);
	const auto Self = AsShared();
	Owner->DisposeDataOnGT(bDiscarded);
}
} // namespace VaRest::Http
