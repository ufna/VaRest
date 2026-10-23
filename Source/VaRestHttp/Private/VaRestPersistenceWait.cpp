// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPersistenceWait.h"

#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestPersistence.h"
#include "VaRestRetryInternal.h"

VaRest::Http::FPersistenceWait::FPersistenceWait(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done)
    : HeldExchange(Exchange), Completion(MoveTemp(Done))
{
}
TSharedRef<VaRest::Http::FPersistenceWait, ESPMode::ThreadSafe> VaRest::Http::FPersistenceWait::Begin(const FVaRestExchangeRef& Exchange,
                                                                                                      FVaRestOutcomeCallback&& Done)
{
	const TSharedRef<FPersistenceWait, ESPMode::ThreadSafe> Wait = MakeShareable(new FPersistenceWait(Exchange, MoveTemp(Done)));
	check(IsInGameThread());
	FExchangeState& Ex = Internals(*Exchange);
	if (Ex.PersistenceCompletionTicket == 0)
	{
		Ex.PersistenceCompletionTicket = AcquirePersistenceCompletion();
		if (Ex.PersistenceCompletionTicket == 0)
		{
			// No wait/interrupt/job is installed. This remains an ordinary asynchronously refused request.
			FVaRestAttemptOutcome Refused;
			Refused.Failure = EVaRestTransportFailure::Other;
			Refused.FailureDetail = TEXT("PersistenceStopped");
			Refused.FailureMessage = TEXT("persistence completion capacity unavailable");
			Wait->bEnded = true;
			auto Callback = MoveTemp(Wait->Completion);
			if (Callback)
				Callback(MoveTemp(Refused));
			return Wait;
		}
	}
	Ex.bPersistenceOwned = true;
	const TWeakPtr<FPersistenceWait, ESPMode::ThreadSafe> Weak(Wait);
	Ex.Interrupt = [Weak]
	{
		if (const auto Pinned = Weak.Pin())
			return Pinned->InterruptNow();
		return false;
	};
	Wait->CheckDeadline();
	return Wait;
}
void VaRest::Http::FPersistenceWait::Disarm()
{
	auto& Ex = Internals(*HeldExchange);
	Ex.Config->Clock->ClearTimer(Timer);
	Ex.Interrupt.Reset();
}
FVaRestOutcomeCallback VaRest::Http::FPersistenceWait::TakeCompletion()
{
	check(IsInGameThread());
	if (bEnded)
		return {};
	bEnded = true;
	Disarm();
	return MoveTemp(Completion);
}
void VaRest::Http::FPersistenceWait::FinishUnchecked(FVaRestAttemptOutcome&& Outcome)
{
	auto Callback = TakeCompletion();
	if (Callback)
		Callback(MoveTemp(Outcome));
}
bool VaRest::Http::FPersistenceWait::CanHandoff()
{
	check(IsInGameThread());
	if (bEnded)
		return false;
	auto& Ex = Internals(*HeldExchange);
	if (IsOwnerGone(Ex))
		MarkOwnerGone(Ex);
	if (Ex.bFinished || Ex.bCompletionQueued || Ex.Phase != EVaRestExchangePhase::InChain || Ex.IsCancelRequested())
	{
		Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
		FinishUnchecked(FVaRestAttemptOutcome::Canceled());
		return false;
	}
	if (Ex.DeadlineAt > 0 && Ex.Config->Clock->Now() >= Ex.DeadlineAt - DeadlineTimerSlack)
	{
		Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
		FinishUnchecked(MakeDeadlineOutcome());
		return false;
	}
	return true;
}
FVaRestOutcomeCallback VaRest::Http::FPersistenceWait::Resume()
{
	return CanHandoff() ? TakeCompletion() : FVaRestOutcomeCallback();
}
void VaRest::Http::FPersistenceWait::Finish(FVaRestAttemptOutcome&& Outcome)
{
	if (CanHandoff())
		FinishUnchecked(MoveTemp(Outcome));
}
bool VaRest::Http::FPersistenceWait::InterruptNow()
{
	if (bEnded)
		return false;
	Internals(*HeldExchange).PersistenceAbort->store(true, std::memory_order_relaxed);
	FinishUnchecked(FVaRestAttemptOutcome::Canceled());
	return true;
}
void VaRest::Http::FPersistenceWait::CheckDeadline()
{
	if (bEnded)
		return;
	auto& Ex = Internals(*HeldExchange);
	if (Ex.IsCancelRequested())
	{
		InterruptNow();
		return;
	}
	if (Ex.DeadlineAt <= 0.0)
		return;
	const double Left = Ex.DeadlineAt - Ex.Config->Clock->Now();
	if (Left <= DeadlineTimerSlack)
	{
		Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
		FinishUnchecked(MakeDeadlineOutcome());
		return;
	}
	const TWeakPtr<FPersistenceWait, ESPMode::ThreadSafe> Weak(AsShared());
	Timer = Ex.Config->Clock->SetTimer(Left,
	                                   [Weak]
	                                   {
		                                   if (const auto Pinned = Weak.Pin())
			                                   Pinned->CheckDeadline();
	                                   });
}
