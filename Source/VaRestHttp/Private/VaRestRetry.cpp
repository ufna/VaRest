// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRetryInternal.h"

#include "VaRestClassify.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestTracePoints.h"

FVaRestRetryPolicy FVaRestRetryPolicy::Disabled()
{
	FVaRestRetryPolicy Policy;
	Policy.MaxRetries = 0;
	return Policy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::Exponential(int32 InMaxRetries)
{
	FVaRestRetryPolicy Policy;
	Policy.MaxRetries = FMath::Max(0, InMaxRetries);
	return Policy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::WithDeadline(float Seconds) const
{
	FVaRestRetryPolicy Copy = *this;
	Copy.Deadline = FMath::IsFinite(Seconds) ? FMath::Max(0.f, Seconds) : 0.f;
	return Copy;
}

FVaRestRetryPolicy FVaRestRetryPolicy::WithBackoff(const FVaRestBackoff& InBackoff) const
{
	FVaRestRetryPolicy Copy = *this;
	Copy.Backoff = InBackoff;
	return Copy;
}

bool VaRest::Http::IsIdempotentVerb(FStringView Verb)
{
	return Verb.Equals(TEXTVIEW("GET"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("HEAD"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("OPTIONS"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("PUT"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("DELETE"), ESearchCase::IgnoreCase);
}

VaRest::Http::FRetryDecision VaRest::Http::DecideRetry(const FVaRestRetryPolicy& Policy, const FVaRestAttemptOutcome& Outcome, int32 RetriesDone,
	double Now, double DeadlineAt, const FDateTime& NowUtc, FRandomStream& Random)
{
	FRetryDecision Out;
	if (!Policy.IsEnabled() || RetriesDone >= Policy.MaxRetries)
	{
		Out.Reason = TEXT("Exhausted");
		return Out;
	}
	// Plan decision 12: the same classification the request's FVaRestError gets
	const FAttemptClass Class = ClassifyAttempt(Outcome);
	bool bEligible = false;
	switch (Class.Kind)
	{
	case EVaRestErrorKind::Connection:
		bEligible = Class.bRetryable && Policy.bRetryConnectionErrors;
		Out.Reason = TEXT("Connection");
		break;
	case EVaRestErrorKind::Timeout:
		bEligible = Class.bRetryable && Policy.bRetryTimeouts;
		Out.Reason = TEXT("Timeout");
		break;
	case EVaRestErrorKind::Http:
		// The policy's list, not FVaRestError::IsRetryableStatus: a game may add 409 or drop 500
		bEligible = Policy.RetryStatuses.Contains(Outcome.Status);
		Out.Reason = FName(*FString::Printf(TEXT("Status%d"), Outcome.Status));
		break;
	default:
		Out.Reason = TEXT("NotRetryable");
		break;
	}
	if (!bEligible)
	{
		return Out;
	}
	TOptional<double> Asked;
	if (Class.Kind == EVaRestErrorKind::Http && Policy.bRespectRetryAfter)
	{
		if (const FString* Header = Outcome.Headers.Find(TEXT("Retry-After")))
		{
			Asked = ParseRetryAfter(*Header, NowUtc);
		}
	}
	double Delay = 0.0;
	if (Asked.IsSet())
	{
		// Plan decision 11: what the server asked, capped (spec 4.1: 60 s)
		Out.RetryAfter = Asked.GetValue();
		const double Cap = FMath::IsFinite(Policy.MaxRetryAfter) ? FMath::Max(0.0, static_cast<double>(Policy.MaxRetryAfter)) : 0.0;
		Delay = FMath::Min(Asked.GetValue(), Cap);
	}
	else
	{
		Delay = Policy.Backoff.GetDelay(RetriesDone + 1, Random);
	}
	if (DeadlineAt > 0.0 && Now + Delay >= DeadlineAt)
	{
		// Plan decision 10: the next attempt would start at or past the deadline
		Out.Reason = TEXT("Deadline");
		return Out;
	}
	Out.bRetry = true;
	Out.Delay = Delay;
	return Out;
}

namespace VaRest::Http::RetryPrivate
{
	/** One exchange's way through the retry stage: shared by its attempt continuations and its backoff timer */
	struct FRun : public TSharedFromThis<FRun, ESPMode::ThreadSafe>
	{
		FRun(const FVaRestExchangeRef& InExchange, const FVaRestStageNext& InNext, FVaRestOutcomeCallback&& InDone)
			: Exchange(InExchange)
			, Next(InNext)
			, Done(MoveTemp(InDone))
		{
		}

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
		FVaRestTimerHandle Timer;
	};

	using FRunRef = TSharedRef<FRun, ESPMode::ThreadSafe>;

	void LogRetry(const FExchangeState& Ex, const FRetryDecision& Decision)
	{
		if (Ex.LogLevel == EVaRestLogLevel::None || !UE_LOG_ACTIVE(LogVaRestHttp, Log))
		{
			return;
		}
		// IN-16: the request id; the reason is VaRest's own FName, no request text in it
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu VaRest: %s; retry %d of %d in %.0f ms%s"), Ex.RequestId, *Decision.Reason.ToString(), Ex.RetriesDone,
			Ex.RetryPolicy.MaxRetries, Decision.Delay * 1000.0, Decision.RetryAfter >= 0.0 ? TEXT(" (Retry-After)") : TEXT(""));
	}

	void StartAttempt(const FRunRef& Run);

	void OnAttempt(const FRunRef& Run, FVaRestAttemptOutcome&& Outcome)
	{
		FExchangeState& Ex = Internals(*Run->Exchange);
		// The attempt's end for the next gap (spec review M-10): its outcome reaches this stage in the call that ended it
		Ex.LastAttemptEndedAt = Ex.Config->Clock->Now();
		// Spec 3.2: "owner gone -> no callbacks, no further retries"; a canceled or finished request is not retried either,
		// nor one whose answer is already being decoded (a custom Outer stage in front answered while this attempt ran):
		// a backoff armed now would fire inside the decode window that re-review N-6 closes for a waiting stage
		if (Ex.bFinished || Ex.Phase != EVaRestExchangePhase::InChain || Run->Exchange->IsCancelRequested() || IsOwnerGone(Ex))
		{
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		const FRetryDecision Decision = DecideRetry(Ex.RetryPolicy, Outcome, Ex.RetriesDone, Ex.Config->Clock->Now(), Ex.DeadlineAt, FDateTime::UtcNow(),
			Ex.State->GetRandom());
		if (!Decision.bRetry)
		{
			Run->Answer(MoveTemp(Outcome));
			return;
		}
		++Ex.RetriesDone;
		// The boundary measures the seconds when the next attempt starts (spec review M-10)
		Ex.NextGap = EVaRestTraceGap::Backoff;
		// This attempt's outcome ends here: its body is freed with it; a SaveTo temp file of it is removed by the next
		// transport start (FTransportStage), or by the decode task when the wait is canceled.
		// Concurrency review M-5: the hook and the timer before the log line and the note -- both run code that is not
		// VaRest's (a log device, a trace sink), which may cancel this request, and that cancel must find the wait.
		const TWeakPtr<FRun, ESPMode::ThreadSafe> Weak = Run;
		Ex.Interrupt = [Weak]() -> bool
		{
			const TSharedPtr<FRun, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid())
			{
				return false;
			}
			Internals(*Pinned->Exchange).Config->Clock->ClearTimer(Pinned->Timer);
			Pinned->Answer(FVaRestAttemptOutcome::Canceled());
			return true;
		};
		Run->Timer = Ex.Config->Clock->SetTimer(Decision.Delay, [Run]
		{
			FExchangeState& Waited = Internals(*Run->Exchange);
			Waited.Interrupt.Reset();
			if (IsOwnerGone(Waited))
			{
				MarkOwnerGone(Waited);
				Run->Answer(FVaRestAttemptOutcome::Canceled());
				return;
			}
			StartAttempt(Run);
		});
		LogRetry(Ex, Decision);
		FVaRestTraceNote Note;
		Note.Kind = EVaRestTraceNoteKind::RetryScheduled;
		Note.Reason = Decision.Reason;
		Note.Delay = Decision.Delay;
		Note.RetryAfter = Decision.RetryAfter;
		TracePoints::Note(Ex, MoveTemp(Note));
	}

	void StartAttempt(const FRunRef& Run)
	{
		Run->Next.Proceed(Run->Exchange, [Run](FVaRestAttemptOutcome&& Outcome) { OnAttempt(Run, MoveTemp(Outcome)); });
	}

	/** Spec 4.1. Outer: it sees each logical request once and re-issues it; the attempt boundary after it numbers the attempts. */
	class FRetryStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Retry"); }

		/** True, but not read: a built-in stage decides per exchange (FExchangeState::bMayReissue, plan decision 8) */
		virtual bool MayReissue() const override { return true; }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (!Internals(*Exchange).bRetryEligible)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			StartAttempt(MakeShared<FRun, ESPMode::ThreadSafe>(Exchange, Next, MoveTemp(OnOutcome)));
		}
	};
}

FVaRestStageRef VaRest::Http::MakeRetryStage()
{
	return MakeShared<RetryPrivate::FRetryStage, ESPMode::ThreadSafe>();
}
