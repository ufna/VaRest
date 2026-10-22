// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuthInternal.h"

#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestRetryInternal.h"

namespace VaRest::Http::AuthGatePrivate
{
	/** One exchange's way through the gate: shared by its attempt continuations, its park's waiter and its deadline timer */
	struct FRun : public TSharedFromThis<FRun, ESPMode::ThreadSafe>
	{
		FRun(const FVaRestExchangeRef& InExchange, const FVaRestStageNext& InNext, FVaRestOutcomeCallback&& InDone, TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth)
			: Exchange(InExchange)
			, Next(InNext)
			, Done(MoveTemp(InDone))
			, Auth(MoveTemp(InAuth))
		{
		}

		bool IsAnswered() const { return !Done; }

		/** The stage's one outcome (IVaRestStage::Process): a second call is ignored */
		void Answer(FVaRestAttemptOutcome&& Outcome)
		{
			if (!Done)
			{
				return;
			}
			FVaRestOutcomeCallback Last = MoveTemp(Done);
			Done.Reset();
			Last(MoveTemp(Outcome));
		}

		FVaRestExchangeRef Exchange;
		FVaRestStageNext Next;
		FVaRestOutcomeCallback Done;
		/** The Send() snapshot's auth (spec 3.1; plan decision 6: the auth is configuration, the session is not) */
		TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> Auth;
		/** Spec 4.2: "then replay once each" */
		bool bReplayed = false;
		/** Concurrency review M-1: the retry deadline while parked */
		FVaRestTimerHandle DeadlineTimer;
	};

	using FRunRef = TSharedRef<FRun, ESPMode::ThreadSafe>;

	FVaRestAttemptOutcome FailureFor(const FExchangeState& Ex, const FRefreshEnded& End)
	{
		// Task 17 review M-5, Task 14 review M-1's rule: the refresh's message is the auth's text, third-party, and
		// FVaRestError::Message must hold no credential (the trace scrubs its own copy; the game's is this one) -- scrubbed
		// by this exchange's redactor, which still knows a token an expiry retired, then escaped
		const FString Why = EscapeControlsForLog(RedactorFor(Ex)->ScrubText(End.Message));
		FVaRestAttemptOutcome Out;
		switch (End.End)
		{
		case ERefreshEnd::Failed:
			// Plan decision 3, ruling I-7: the session stays and the refresh's own kind comes through (FinishRefresh keeps
			// Timeout and Config and makes the rest Connection, as the Failed() factory does: Task 16 review M-2) --
			// Connection and Timeout retryable, so Retry (outside) and M4b's offline queue can act; Config not
			// (ClassifyAttempt: AuthRefreshConfig)
			if (End.Kind == EVaRestErrorKind::Config)
			{
				Out.Failure = EVaRestTransportFailure::Other;
				Out.FailureDetail = TEXT("AuthRefreshConfig");
			}
			else
			{
				Out.Failure = End.Kind == EVaRestErrorKind::Timeout ? EVaRestTransportFailure::Timeout : EVaRestTransportFailure::Connection;
				Out.FailureDetail = TEXT("AuthRefreshFailed");
			}
			Out.FailureMessage = FString::Printf(TEXT("the session refresh failed: %s"), *Why);
			break;
		case ERefreshEnd::Cleared:
			Out.Failure = EVaRestTransportFailure::Other;
			Out.FailureDetail = TEXT("SessionCleared");
			Out.FailureMessage = TEXT("the session was cleared (a logout) while this request waited for a refresh");
			break;
		default:
			Out.Failure = EVaRestTransportFailure::Other;
			Out.FailureDetail = TEXT("AuthRejected");
			Out.FailureMessage = FString::Printf(TEXT("the session refresh was rejected: %s"), *Why);
			break;
		}
		return Out;
	}

	/**
	 * Task 17 review I-1 (ruling I-3 as spec review I-1 extends it): a park before an attempt is that attempt's Auth phase,
	 * so an end that answers it without a dispatch writes the attempt's Basic line (LogEndedInPark) -- before
	 * PendingAttemptStart is reset, as that is when the attempt started. A replay's park (PendingAttemptStart -1) is a Gap
	 * outside any attempt and stays silent, as a backoff does; so does an end for a request whose answer is accepted or
	 * that left the chain (EndWaits), as the limiter's abort (VaRestPipeline.cpp:374-377).
	 */
	void LogParkEnd(const FExchangeState& Ex, const FVaRestAttemptOutcome& Outcome)
	{
		if (Ex.PendingAttemptStart >= 0.0 && Ex.Phase == EVaRestExchangePhase::InChain && !Ex.bCompletionQueued)
		{
			LogEndedInPark(Ex, Outcome);
		}
	}

	/**
	 * Concurrency review M-1: the retry deadline is armed in a park too. Behind a slow refresh, or one an auth holds open (a
	 * re-login screen, ruling I-4), the request ends at its deadline: Timeout (Deadline), not retryable. A fire before
	 * DeadlineAt on the exchange's clock re-arms for the rest, as the limiter queue's deadline timer does (ArmQueueDeadline,
	 * Task 9 review minor 2): a Deadline answered early is not retryable. Weak: the park's waiter holds the run.
	 */
	void ArmParkDeadline(FRun& Run, uint64 WaiterId, double Delay)
	{
		Run.DeadlineTimer = Internals(*Run.Exchange).Config->Clock->SetTimer(Delay, [Weak = TWeakPtr<FRun, ESPMode::ThreadSafe>(Run.AsShared()), WaiterId]
		{
			const TSharedPtr<FRun, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid())
			{
				return;
			}
			Pinned->DeadlineTimer.Invalidate();
			if (Pinned->IsAnswered())
			{
				// The park ended another way first; each of those clears this timer, so only a fire already on its way lands here
				return;
			}
			FExchangeState& Late = Internals(*Pinned->Exchange);
			const double Rest = Late.DeadlineAt - Late.Config->Clock->Now();
			if (Rest > DeadlineTimerSlack)
			{
				ArmParkDeadline(*Pinned, WaiterId, Rest);
				return;
			}
			// Unconditionally: one stage waits at a time, so the hook is still this park's own (as Retry's backoff timer)
			Late.Interrupt.Reset();
			Late.State->LeaveRefresh(WaiterId);
			FVaRestAttemptOutcome Ended = MakeDeadlineOutcome();
			LogParkEnd(Late, Ended);
			// M4 plan re-review N-11: no attempt follows this park; a stage that re-issues after it starts a fresh one
			Late.PendingAttemptStart = -1.0;
			Pinned->Answer(MoveTemp(Ended));
		});
	}

	void Dispatch(const FRunRef& Run);

	/** Holds Run behind the client's refresh (plan decision 7); bReplay: this follows a refused attempt (IN-2 GapBefore) */
	void Park(const FRunRef& Run, bool bReplay)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		FClientState& Client = *Ex.State;
		const uint64 WaiterId = Client.NewWaiterId();
		const double ParkedAt = Ex.Config->Clock->Now();
		if (!bReplay)
		{
			// Spec review I-1 (Inspector 3.3): a park before an attempt starts is inside that attempt (its Auth phase), so the
			// attempt starts here; the boundary takes this time. A replay's park is the Gap before its attempt, outside both.
			// M4a final review C-I-1 (ruling, option b): the attempt takes its number here too, and the boundary does not
			// number it again (PendingAttemptStart set), so an attempt this park ends unsent is counted as the log line, Retry's
			// note and FVaRestError::Attempts count it -- with no Dispatched/Ended pair, as the limiter's queue ends one.
			++Ex.Attempt;
			Ex.PendingAttemptStart = ParkedAt;
		}
		const TWeakPtr<FRun, ESPMode::ThreadSafe> Weak = Run;
		// Set before JoinRefresh, as a backoff's is (decision 7): Cancel, owner-gone and shutdown reach the park through it,
		// and so does a Cancel() from inside the auth's Refresh, which JoinRefresh may call
		Ex.Interrupt = [Weak, WaiterId]() -> bool
		{
			const TSharedPtr<FRun, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid())
			{
				return false;
			}
			FExchangeState& Stopped = Internals(*Pinned->Exchange);
			Stopped.Config->Clock->ClearTimer(Pinned->DeadlineTimer);
			// Concurrency review I-5: the waiter goes uncalled, the refresh goes on for the others
			Stopped.State->LeaveRefresh(WaiterId);
			FVaRestAttemptOutcome Ended = FVaRestAttemptOutcome::Canceled();
			// A Cancel() writes the line; EndWaits (the answer accepted) does not
			LogParkEnd(Stopped, Ended);
			// M4 plan re-review N-11: no attempt follows this park; a stage that re-issues after it starts a fresh one
			Stopped.PendingAttemptStart = -1.0;
			Pinned->Answer(MoveTemp(Ended));
			return true;
		};
		if (Ex.DeadlineAt > 0.0)
		{
			ArmParkDeadline(*Run, WaiterId, FMath::Max(0.0, Ex.DeadlineAt - ParkedAt));
		}
		// The waiter runs from the refresh's one game-thread task (concurrency review I-1), never inside this call. It holds
		// the run, so the exchange (and through it the client, as any request in flight does, spec 3.2) until the refresh
		// ends; a request that finishes first leaves the list through the hook above (LeaveRefresh), so no waiter outlives it.
		// C-I-1: kept for Completed. Set after the call: a request the auth's Refresh ends from inside it (a Cancel()) is
		// completed from a later task (FClientState::Complete defers inside Cancel, decision 3)
		Ex.ParkRefreshId = Client.JoinRefresh(WaiterId, Run->Auth, Ex.RequestId, [Run, ParkedAt, bReplay](const FRefreshEnded& End)
		{
			if (Run->IsAnswered())
			{
				// Canceled, past its deadline, or its answer accepted while the end was on its way: answered once already
				return;
			}
			FExchangeState& Back = Internals(*Run->Exchange);
			Back.Interrupt.Reset();
			Back.Config->Clock->ClearTimer(Run->DeadlineTimer);
			if (End.End != ERefreshEnd::Ready || IsOwnerGone(Back))
			{
				FVaRestAttemptOutcome Ended;
				if (End.End != ERefreshEnd::Ready)
				{
					Ended = FailureFor(Back, End);
				}
				else
				{
					// Spec 3.2: "owner gone -> no callbacks, no further retries"
					MarkOwnerGone(Back);
					Ended = FVaRestAttemptOutcome::Canceled();
				}
				LogParkEnd(Back, Ended);
				// No attempt follows this park: a retry after it (a Failed refresh is retryable) starts its own attempt
				Back.PendingAttemptStart = -1.0;
				Run->Answer(MoveTemp(Ended));
				return;
			}
			const double Seconds = Back.Config->Clock->Now() - ParkedAt;
			Back.AuthRefreshId = End.RefreshId;
			if (bReplay)
			{
				// Inspector 3.3: a replay's park is its Gap (SessionRefresh), drawn before the attempt -- not AuthWait as well
				Back.NextGap = EVaRestTraceGap::SessionRefresh;
				Back.NextGapSeconds = Seconds;
				Back.AuthWait = 0.0;
			}
			else
			{
				// ... and a park before an attempt starts is its Auth phase, inside the span PendingAttemptStart opened
				Back.AuthWait = Seconds;
			}
			// A request canceled or answered meanwhile without this run hearing of it (a worker's Cancel() still on its way)
			// is refused by Proceed, which answers Canceled: its Phase and bCompletionQueued checks are the pipeline's own
			Dispatch(Run);
		});
	}

	void AfterAttempt(const FRunRef& Run, FVaRestAttemptOutcome&& Outcome)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		// As Retry (VaRestRetry.cpp:176): a canceled or finished request, one whose owner is gone, or one whose answer is
		// already accepted -- being decoded, or its delivery queued (a custom Outer stage in front answered while this
		// attempt ran) -- is not parked: a park now would start a refresh for a request already being delivered, and
		// EndWaits, which ran when that answer was accepted, would never end it (re-review N-6). Task 17 review M-3 (ruled):
		// at or past the retry deadline no attempt may start (plan decision 10), so the 401 is the answer -- DecideRetry's
		// rule with no delay (VaRestRetry.cpp:102-107), which answers the last attempt's outcome; Timeout (Deadline) stays
		// for a park the deadline cuts (ArmParkDeadline)
		if (Ex.bFinished || Ex.bCompletionQueued || Ex.Phase != EVaRestExchangePhase::InChain || Run->Exchange->IsCancelRequested() || IsOwnerGone(Ex)
			|| Run->bReplayed || Outcome.IsTransportFailure() || !Run->Auth->IsAuthFailure(Outcome.Status)
			|| (Ex.DeadlineAt > 0.0 && Ex.Config->Clock->Now() >= Ex.DeadlineAt))
		{
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		const FVaRestSessionData& Session = Ex.State->GetSessionData();
		// Plan decision 8: a body moved into the engine cannot go again (Resolve copies it when an auth may replay); a
		// BodyFile always can (its path is kept)
		const bool bCanResend = Ex.bMayReissue || Ex.Body.IsEmpty() || !Ex.Body.FilePath.IsEmpty();
		// Concurrency review M-6: the credentials' own generation -- a session header changed since does not count
		const bool bNewerCredentials = Session.CredentialGeneration != Ex.AppliedCredentialGeneration;
		if (!bCanResend || (!bNewerCredentials && !Ex.State->IsRefreshing() && !Run->Auth->CanRefresh(Session)))
		{
			// Nothing would change: the refusal is the answer
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		Run->bReplayed = true;
		if (bNewerCredentials && !Ex.State->IsRefreshing())
		{
			// Another request's refresh, or the game, changed the credentials since this attempt took its own: replay now,
			// with no refresh to name
			Ex.NextGap = EVaRestTraceGap::SessionRefresh;
			Ex.NextGapSeconds = 0.0;
			Ex.AuthWait = 0.0;
			Ex.AuthRefreshId = 0;
			Dispatch(Run);
			return;
		}
		Park(Run, /*bReplay*/ true);
	}

	void Dispatch(const FRunRef& Run)
	{
		// Once per attempt, as Retry's StartAttempt (Task 8 re-review): a PerAttempt stage that breaks its contract by reporting
		// twice (review C-3: the second report is ignored) would otherwise answer a request its first report parked. The
		// limiter between them passes every report up; only its Release is once-only.
		Run->Next.Proceed(Run->Exchange, [Run, bSeen = false](FVaRestAttemptOutcome&& Outcome) mutable
		{
			if (bSeen)
			{
				return;
			}
			bSeen = true;
			AfterAttempt(Run, MoveTemp(Outcome));
		});
	}

	void BeforeAttempt(const FRunRef& Run)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		FClientState& Client = *Ex.State;
		const FVaRestSessionData& Session = Client.GetSessionData();
		// Spec 4.2: a refresh in flight parks every request that starts meanwhile; a due expiry refreshes first ("proactively")
		if (Client.IsRefreshing() || (Run->Auth->CanRefresh(Session) && Run->Auth->NeedsRefresh(Session, Ex.Config->Clock->Now())))
		{
			Park(Run, /*bReplay*/ false);
			return;
		}
		// IN-2: this attempt waited behind no refresh (an earlier attempt's values are not carried over: a stage that answers
		// an attempt before the transport stage resets them leaves them set)
		Ex.AuthWait = 0.0;
		Ex.AuthRefreshId = 0;
		Dispatch(Run);
	}

	class FAuthGateStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("AuthGate"); }

		/** True, but not read: per exchange (FExchangeState::bMayReissue, plan decision 8) */
		virtual bool MayReissue() const override { return true; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			// The refresh request itself (plan decision 23) and NoAuth() requests bypass the auth stages (spec 4.2), and a
			// client with no auth has nothing to refresh
			if (Ex.AuthMode == EVaRestAuthMode::None || Ex.bIsAuthRefresh || !Ex.Config->Auth.IsValid())
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			BeforeAttempt(MakeShared<FRun, ESPMode::ThreadSafe>(Exchange, Next, MoveTemp(OnOutcome), Ex.Config->Auth.ToSharedRef()));
		}
	};
}

FVaRestStageRef VaRest::Http::MakeAuthGateStage()
{
	return MakeShared<AuthGatePrivate::FAuthGateStage, ESPMode::ThreadSafe>();
}
