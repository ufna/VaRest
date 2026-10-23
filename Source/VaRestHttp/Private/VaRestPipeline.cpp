// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/Guid.h"
#include "VaRestClientState.h"
#include "VaRestPersistence.h"

#include "HAL/FileManager.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestAuthInternal.h"
#include "VaRestClassify.h"
#include "VaRestDecode.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpStatsInternal.h"
#include "VaRestHttpTesting.h"
#include "VaRestRetryInternal.h"
#include "VaRestTraceExcerpts.h"
#include "VaRestTracePoints.h"
#include "VaRestUrl.h"

namespace VaRest::Http::PipelinePrivate
{
	/**
	 * A game-thread task: UE::Tasks' GameThreadNormalPri is ENamedThreads::GameThread (TaskPrivate.cpp:614-615). Every
	 * delivery goes through here. The game thread runs its named queue at frame boundaries and also inside tick-group and
	 * task waits, so a callback can run between tick groups, mid-frame (decision 32; v1 delivered from the core ticker).
	 * Spec 1 says "on the game thread", which this is; M5's Blueprint async actions decide whether to defer to a frame
	 * boundary, and if so it is this one function that changes.
	 */
	template <typename FuncType>
	void OnGameThread(const TCHAR* DebugName, FuncType&& Func)
	{
		UE::Tasks::Launch(DebugName, Forward<FuncType>(Func), UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}

	/**
	 * While Enter (so a game-thread Send(), and the Enter task of a request sent from another thread) or Cancel runs on an
	 * exchange, Complete defers its callbacks (decision 3)
	 */
	struct FApiScope
	{
		explicit FApiScope(FExchangeState& InExchange)
			: Exchange(InExchange)
		{
			++Exchange.ApiDepth;
		}
		~FApiScope()
		{
			--Exchange.ApiDepth;
		}
		FExchangeState& Exchange;
	};

	/**
	 * Review I3: decision 11 holds for the final headers too. The builder checks the request's headers, but a custom
	 * PerAttempt stage edits AttemptHeaders after every builder check. A body also needs a Content-Type: curl checks one
	 * for a POST or DELETE whose payload is not URL-encoded, and a counted stream never is (CurlHttp.cpp:945, :983;
	 * HttpRequestPayload.cpp:64-69), so a stage that removes it would trip a check in a dev build (review N-5). True and
	 * the first bad name in OutName when one is bad, a missing Content-Type included.
	 */
	bool FindBadHeader(const FExchangeState& Ex, FString& OutName)
	{
		for (const FVaRestHeaders::FPair& Pair : Ex.AttemptHeaders)
		{
			if (!IsValidHeaderName(Pair.Key) || !IsValidHeaderValue(Pair.Value))
			{
				OutName = Pair.Key;
				return true;
			}
		}
		if (!Ex.Body.IsEmpty())
		{
			const FString* Type = Ex.AttemptHeaders.Find(TEXT("Content-Type"));
			if (!Type || Type->TrimStartAndEnd().IsEmpty())
			{
				OutName = TEXT("Content-Type");
				return true;
			}
		}
		return false;
	}

	/**
	 * Review I-3: the transport's completion, wrapped. It carries the exchange as a TSharedPtr that is moved, never
	 * copied, into the game-thread continuation (review I-1: the last reference is dropped on the game thread). If the
	 * transport destroys it uncalled -- the engine does at HTTP module shutdown and with NullHttp -- its destructor
	 * reports TransportDropped through the same path, so the request still finishes once. A moved-from guard holds
	 * nothing and reports nothing. M4a final review A1: it carries its dispatch's tag, so OnAttemptDone can tell this
	 * call's end from the current dispatch's.
	 */
	class FCompletionGuard
	{
	public:
		FCompletionGuard(const FVaRestExchangeRef& InExchange, FDispatchTag&& InDispatch, FVaRestOutcomeCallback&& InOutcome)
			: Held(InExchange)
			, Dispatch(MoveTemp(InDispatch))
			, Outcome(MoveTemp(InOutcome))
		{
		}
		FCompletionGuard(FCompletionGuard&&) = default;
		FCompletionGuard& operator=(FCompletionGuard&&) = delete;

		~FCompletionGuard()
		{
			if (Held.IsValid())
			{
				FVaRestTransportResult Dropped;
				Dropped.Failure = EVaRestTransportFailure::Other;
				Dropped.FailureDetail = TEXT("TransportDropped");
				Deliver(MoveTemp(Dropped));
			}
		}

		/** Any thread -- the HTTP thread for FHttpModule. It only moves the result to the game thread (spec 1). */
		void Deliver(FVaRestTransportResult&& Result)
		{
			if (!Held.IsValid())
			{
				return;
			}
			OnGameThread(TEXT("VaRest.AttemptDone"), [Target = MoveTemp(Held), Tag = MoveTemp(Dispatch), Callback = MoveTemp(Outcome), Moved = MoveTemp(Result)]() mutable
			{
				Internals(*Target).State->OnAttemptDone(Target.ToSharedRef(), MoveTemp(Tag), MoveTemp(Moved), MoveTemp(Callback));
			});
		}

	private:
		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Held;
		FDispatchTag Dispatch;
		FVaRestOutcomeCallback Outcome;
	};

	/** Decision 8's throttle: the game thread reads the probe this often while an attempt runs */
	constexpr double ProgressInterval = 0.1;

	/** Game thread: calls OnProgress when the probe changed since the last call */
	void DeliverProgress(FExchangeState& Ex)
	{
		// No progress for a request being canceled, whose owner is gone, or at shutdown (spec 3.2: no callbacks; review C-1).
		// The last call comes from OnAttemptDone after Task 11's owner check only marks it: without this, OnProgress
		// would run into a destroyed owner.
		if (!Ex.Probe.IsValid() || !Ex.Handler.IsValid() || !Ex.Handler->WantsProgress() || Ex.IsCancelRequested() || IsOwnerGone(Ex))
		{
			return;
		}
		FVaRestProgress Current;
		Current.BytesSent = Ex.Probe->BytesSent.load();
		Current.BytesReceived = Ex.Probe->BytesReceived.load();
		Current.UploadTotal = Ex.Probe->UploadTotal.load();
		Current.DownloadTotal = Ex.Probe->DownloadTotal.load();
		if (Current.DownloadTotal >= 0 && Current.BytesReceived > Current.DownloadTotal)
		{
			// Review M-16: a compressed body's Content-Length counts compressed bytes; a percentage must never pass 100%
			Current.DownloadTotal = -1;
		}
		if (Current == Ex.LastProgress)
		{
			return;
		}
		Ex.LastProgress = Current;
		// Spec 4.8: the game's callback, under the tags it was sent with (FLlmSenderTags), not VaRest's
		VAREST_LLM_SCOPE_SENDER(Ex.SenderTags);
		Ex.Handler->Progress(Current);
	}

	/** Serial: the dispatch this chain polls for (M4a final review A1); it stops once that dispatch is not the live one */
	void ScheduleProgress(const FVaRestExchangeRef& Exchange, uint32 Serial)
	{
		FExchangeState& Ex = Internals(*Exchange);
		if (!Ex.TransportCall.IsValid() || Ex.LiveDispatch != Serial || Exchange->IsCancelRequested() || Ex.bFinished)
		{
			// Review M-3: the timer stops once the attempt is canceled -- an OnProgress that called Cancel() included; the
			// transport's Canceled answer is on its way, and OnAttemptDone delivers nothing for it. Task 13 review minor 5
			// (P13.5): and once the request is finished. A1: and once another dispatch is the live one (a PerAttempt stage
			// that proceeded again while this call ran), so two chains never poll one probe.
			return;
		}
		const TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Weak = Exchange;
		Ex.ProgressTimer = Ex.Config->Clock->SetTimer(ProgressInterval, [Weak, Serial]
		{
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid() || !Internals(*Pinned).TransportCall.IsValid() || Internals(*Pinned).LiveDispatch != Serial)
			{
				return;
			}
			if (IsOwnerGone(Internals(*Pinned)))
			{
				// Spec 3.2: an owner gone mid-download stops it now rather than at the end
				Internals(*Pinned).State->Cancel(Pinned.ToSharedRef(), EVaRestCancelReason::OwnerGone);
				return;
			}
			DeliverProgress(Internals(*Pinned));
			ScheduleProgress(Pinned.ToSharedRef(), Serial);
		});
	}

	/**
	 * Game thread. M4a final review A1 (c, d; review A M-1): the call TransportCall holds is an orphan -- a stage answered
	 * its attempt while it ran (the attempt boundary sees that attempt's outcome come back), or the request's answer was
	 * accepted (FClientState::EndWaits) -- and nothing will use its result. It is canceled now, so it stops moving bytes
	 * and the limiter slot it holds (if its outcome has not passed the grant yet) comes back with its Canceled end; its
	 * progress timer goes. The slot is cleared first: the end arrives later (FCompletionGuard always hops) as a stale one.
	 * A call already finished ignores the cancel (IVaRestTransportCall::Cancel); the engine's CancelRequest is once-only
	 * (HttpRequestCommon.cpp:394-400).
	 */
	void CancelHeldCall(FExchangeState& Ex)
	{
		if (!Ex.TransportCall.IsValid())
		{
			return;
		}
		Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
		const TSharedPtr<IVaRestTransportCall, ESPMode::ThreadSafe> Orphan = MoveTemp(Ex.TransportCall);
		Ex.TransportCall.Reset();
		Ex.LiveDispatch = 0;
		Orphan->Cancel();
	}

	/**
	 * Background thread (the restore-the-bug runs put it on the game thread): spec 1's "parsing, decoding ... run on
	 * UE::Tasks". Ex is read only through its atomic cancel flag, which any thread may read.
	 */
	void RunDecode(IResultHandler& Handler, FFinish& Finish, const FExchangeState& Ex)
	{
#if !UE_BUILD_SHIPPING
		if (IsInGameThread())
		{
			++Testing::Counters().DecodesOnGameThread;
		}
		else
		{
			++Testing::Counters().DecodesOffGameThread;
		}
#endif
		// IN-14: first, before anything reads or rewrites the body -- Decode's ParseOwned unescapes it in place
		// (VaRestDecode.cpp:115) and a JSON result moves it into the document; a canceled response was announced too
		Excerpts::EmitOnDecode(Finish);
		const bool bCanceled = Ex.IsCancelRequested();
		if (Finish.bTimeDecode)
		{
			Finish.DecodeStartAt = Finish.Clock->Now();
		}
		if (!Finish.TempFile.IsEmpty() && (bCanceled || Finish.Error.IsError()))
		{
			// A failed or canceled download: the destination is never touched, the temp file goes
			if (Finish.Error.Kind == EVaRestErrorKind::Http && Finish.bBodyInFile)
			{
				LoadFilePrefix(Finish.TempFile, Finish.Body);
			}
			IFileManager::Get().Delete(*Finish.TempFile, false, false, true);
		}
		if (!Finish.Error.IsError() && !bCanceled)
		{
			// Review M-4: the cancel flag is read again right before the rename
			if (Finish.SaveTo.IsEmpty() || FinalizeSaveTo(Finish, [&Ex] { return Ex.IsCancelRequested(); }))
			{
				Handler.Decode(Finish);
			}
		}
		else if (Finish.Error.Kind == EVaRestErrorKind::Http)
		{
			ProcessErrorBody(Finish);
		}
		if (Finish.bTimeDecode)
		{
			Finish.DecodeEndAt = Finish.Clock->Now();
		}
	}

	/** Spec 4's boundary between Outer and PerAttempt stages: each pass is a new attempt */
	class FAttemptBoundaryStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("AttemptBoundary"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			// Spec review I-1 (Inspector 3.3): an attempt parked behind a refresh before it started (the auth gate) started
			// when it parked -- its Auth phase is inside its span, so Prepare (dispatch - start - Auth - Queued) is never
			// negative. M4a final review C-I-1: that park numbered it too (VaRestAuthGate.cpp, Park). Taken once: a later pass
			// is another attempt.
			if (Ex.PendingAttemptStart >= 0.0)
			{
				Ex.AttemptStartedAt = Ex.PendingAttemptStart;
			}
			else
			{
				++Ex.Attempt;
				Ex.AttemptStartedAt = Ex.Config->Clock->Now();
			}
			Ex.PendingAttemptStart = -1.0;
			if (Ex.NextGap == EVaRestTraceGap::Backoff)
			{
				// Spec review M-10 (Inspector 3.3: from the previous attempt's end to this attempt's start): what passed,
				// not the delay asked for -- a real-clock ticker fires up to a frame late
				Ex.NextGapSeconds = FMath::Max(0.0, Ex.AttemptStartedAt - Ex.LastAttemptEndedAt);
			}
			Ex.AttemptHeaders = Ex.RequestHeaders;
			// Plan decision 19: each attempt starts from the request's URL; AuthApply appends an auth's query pairs to it
			Ex.AttemptUrl = Ex.Url;
			Ex.LimiterWait = 0.0;
			// M4a final review A1 (c): the attempt ends with its outcome. On the normal path OnAttemptDone cleared the slot
			// before this runs; a call of this attempt still held here is one a stage answered while it ran (the stage
			// contract allows it, VaRestStage.h:89-90). It would otherwise run on -- after Retry backs off or the gate
			// replays -- holding its connection, writing a SaveTo file and polling progress. A held call of another attempt
			// (an Outer stage that proceeded twice) is that attempt's own and is left alone.
			const int32 ThisAttempt = Ex.Attempt;
			Next.Proceed(Exchange, [Exchange, ThisAttempt, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				FExchangeState& Back = Internals(*Exchange);
				if (Back.TransportCall.IsValid() && Back.LiveDispatchAttempt == ThisAttempt)
				{
					CancelHeldCall(Back);
				}
				Done(MoveTemp(Outcome));
			});
		}
	};

	/**
	 * Concurrency review M-1: the deadline's timer while an attempt waits in the limiter's queue. At DeadlineAt it takes the
	 * entry out of the queue and FLimiterStage's abort answers Deadline. Weak: the queue entry holds the exchange already.
	 * A fire before DeadlineAt on the exchange's clock re-arms for the rest (Task 9 review minor 2): the real clock's core
	 * ticker counts game-time deltas, which a fixed timestep can run faster than Now() (FVaRestRealClock), and a Deadline
	 * answered early is not retryable. The engine re-arms its own activity timer the same way (HttpRequestCommon.cpp:478-484).
	 */
	void ArmQueueDeadline(FExchangeState& Ex, const FVaRestExchangeRef& Exchange, double Delay)
	{
		Ex.QueueDeadline = Ex.Config->Clock->SetTimer(Delay, [Weak = TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>(Exchange)]
		{
			// M4a final review A5 (review A M-4, spec 4.8): a game-thread re-entry from the clock's ticker
			LLM_SCOPE_BYTAG(VaRest);
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid())
			{
				return;
			}
			FExchangeState& Late = Internals(*Pinned);
			Late.QueueDeadline.Invalidate();
			if (!Late.bInLimiterQueue)
			{
				return;
			}
			const double Rest = Late.DeadlineAt - Late.Config->Clock->Now();
			if (Rest > DeadlineTimerSlack)
			{
				ArmQueueDeadline(Late, Pinned.ToSharedRef(), Rest);
				return;
			}
			Late.bQueueDeadlinePassed = true;
			if (!Late.State->GetLimiter().Abort(Late.RequestId))
			{
				Late.bQueueDeadlinePassed = false;
			}
		});
	}

	/** Spec 4.6 as amended (ruling I-3): the first stage of every attempt, right after the attempt boundary */
	class FLimiterStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Limiter"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			if (Ex.bIsAuthRefresh)
			{
				// Ruling M-8: the refresh request skips the queue -- one more connection per refresh request the auth sends
				// (single flight runs one refresh per client) -- since every parked request waits on it: behind long polls
				// holding every slot it would never run. It takes no slot, so its outcome goes up as it is -- never through the
				// grant's Release below -- and the queue, its deadline timer (ArmQueueDeadline) and LimiterWait (0 from the
				// attempt boundary) never see it.
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			// One of the two closures below runs, exactly once: the grant or the abort
			TSharedRef<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Outcome = MakeShared<FVaRestOutcomeCallback, ESPMode::ThreadSafe>(MoveTemp(OnOutcome));
			// Inspector IN-2: both ends of the wait on this exchange's own clock (review Minor 3). The grant comes from
			// another exchange's Release, or from the client's pump task, and either may hold another clock (WithClock()).
			const double EnqueuedAt = Ex.Config->Clock->Now();
			Ex.bInLimiterQueue = true;
			Ex.State->GetLimiter().Acquire(Ex.RequestId, Ex.Priority,
				[Exchange, Next, Outcome, EnqueuedAt]()
				{
					FExchangeState& Granted = Internals(*Exchange);
					Granted.bInLimiterQueue = false;
					// Concurrency review M-1: out of the queue, the deadline's queue timer goes; the transport stage checks the
					// deadline again at dispatch
					Granted.Config->Clock->ClearTimer(Granted.QueueDeadline);
					Granted.LimiterWait = Granted.Config->Clock->Now() - EnqueuedAt;
					if (!Granted.bFinished && Exchange->IsCancelRequested())
					{
						// Ruling I-3: flagged while it waited with no Abort behind it (a worker's Cancel() until its hop runs,
						// VaRestClient.cpp:262-263). Proceed answers it Canceled below, before the log stage, so its Basic
						// line is written here, as for an abort. The flag is never cleared, so Proceed's own read agrees.
						LogEndedInQueue(Granted, FVaRestAttemptOutcome::Canceled());
					}
					Next.Proceed(Exchange, [Exchange, Outcome, bReleased = false](FVaRestAttemptOutcome&& Result) mutable
					{
						// The slot is freed before the outcome goes further up: an Outer stage that re-issues (Retry, the auth
						// gate) queues again; a PerAttempt stage that proceeds twice re-issues inside this one grant (ruling
						// I-3, re-review N-12). Once: this callback goes to a custom PerAttempt stage now, not only to the
						// transport stage's exactly-once completion, and a stage that reports twice (review C-3: the second
						// report is ignored) must not free another attempt's slot.
						if (!bReleased)
						{
							bReleased = true;
							Internals(*Exchange).State->GetLimiter().Release();
						}
						(*Outcome)(MoveTemp(Result));
					});
				},
				[Exchange, Outcome, EnqueuedAt]()
				{
					FExchangeState& Aborted = Internals(*Exchange);
					Aborted.bInLimiterQueue = false;
					Aborted.Config->Clock->ClearTimer(Aborted.QueueDeadline);
					// Task 14 re-review N2 (P14.2): the time spent queued is the queue's, as for a grant, so the log line
					// shows "canceled (0 ms, N ms queued)" and not the wait as the attempt's own milliseconds
					Aborted.LimiterWait = Aborted.Config->Clock->Now() - EnqueuedAt;
					// Concurrency review M-1: the deadline's timer took it out -- the refusal the transport stage makes for an
					// attempt that would start past the deadline. Read once and cleared, so no later abort inherits it.
					const bool bPastDeadline = Aborted.bQueueDeadlinePassed;
					Aborted.bQueueDeadlinePassed = false;
					FVaRestAttemptOutcome Ended = bPastDeadline ? MakeDeadlineOutcome() : FVaRestAttemptOutcome::Canceled();
					// Ruling I-3: nothing after the limiter ran, the log stage included, so the line is written here -- for an
					// attempt the request itself lost (a cancel, engine exit, the deadline). One dropped because the request's
					// answer was accepted (FClientState::EndWaits: Phase moved on, or the delivery is queued) ends silently, as
					// a finished request's dead entry does at its grant (Task 8 re-review minor 1).
					if (Aborted.Phase == EVaRestExchangePhase::InChain && !Aborted.bCompletionQueued)
					{
						LogEndedInQueue(Aborted, Ended);
					}
					(*Outcome)(MoveTemp(Ended));
				});
			if (Ex.bInLimiterQueue && Ex.DeadlineAt > 0.0 && !Ex.QueueDeadline.IsValid())
			{
				// Concurrency review M-1: armed, not only checked at the grant, so a wait behind long requests ends at the
				// deadline (ArmQueueDeadline). An entry granted inside Acquire has bInLimiterQueue false again by now, so no
				// timer is armed for it; a valid handle is the timer of an entry queued while this call ran (a stage that
				// re-issued synchronously), and is kept.
				ArmQueueDeadline(Ex, Exchange, FMath::Max(0.0, Ex.DeadlineAt - EnqueuedAt));
			}
		}
	};

	/** The chain's last stage: hands the attempt to IVaRestTransport */
	class FTransportStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Transport"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			if (IsOwnerGone(Ex))
			{
				// Spec 3.2: "owner gone -> no callbacks, no further retries": no attempt starts for it
				MarkOwnerGone(Ex);
				OnOutcome(FVaRestAttemptOutcome::Canceled());
				return;
			}
			const FClientConfig& Cfg = *Ex.Config;
			FString BadName;
			if (FindBadHeader(Ex, BadName))
			{
				// Review I3: a header a stage set (or a missing or empty Content-Type with a body, review N-5) never reaches
				// the transport. Review N-I1: "missing or empty Content-Type with a body" is this line's own; the tests expect it.
				// The name is escaped: a stage's CR LF must not end this line either (task 14 review, Critical 1).
				UE_LOG(LogVaRestHttp, Error, TEXT("#%llu VaRest: header \"%s\" is not a token, holds CR, LF or NUL, or is a missing or empty Content-Type with a body; the request fails with Kind=Config"),
					Ex.RequestId, *EscapeForLog(BadName));
				FVaRestAttemptOutcome Refused;
				Refused.Failure = EVaRestTransportFailure::Other;
				Refused.FailureDetail = TEXT("InvalidHeader");
				OnOutcome(MoveTemp(Refused));
				return;
			}
			// Decision 30 (review I-5), resolved here so every transport gets final values (Task 9 review I-1): the
			// activity timeout is the request's own, else its own total, so the engine's 30 s idle default
			// ([HTTP] HttpActivityTimeout, HttpRequestCommon.cpp:733-736) never fires before a total the request set. 0
			// leaves the engine's default.
			double AttemptTimeout = Ex.Timeout;
			double AttemptActivity = Ex.ActivityTimeout > 0.f ? Ex.ActivityTimeout : FMath::Max(Ex.Timeout, 0.f);
			if (Ex.DeadlineAt > 0.0)
			{
				// Plan decision 10 (spec 4.7, "plus the retry Deadline across attempts"): the time left bounds this attempt,
				// and with none left it never starts -- a long wait in the limiter's queue counts
				const double Left = Ex.DeadlineAt - Cfg.Clock->Now();
				if (Left <= 0.0)
				{
					OnOutcome(MakeDeadlineOutcome());
					return;
				}
				// The controller's Task 9 ruling: the deadline only shortens what is set. The total is the request's own cut to
				// the time left, or the time left when it set none; the activity timeout is the one above (never the time
				// left) cut to the time left, and with neither timeout set it stays 0, the engine's idle default, so a
				// stalled attempt still fails at it and is retried (the total, at most the time left now, ends the attempt by
				// the deadline whatever that default is).
				AttemptActivity = AttemptActivity > 0.0 ? FMath::Min(AttemptActivity, Left) : 0.0;
				AttemptTimeout = AttemptTimeout > 0.0 ? FMath::Min(AttemptTimeout, Left) : Left;
			}
			if (!Ex.SaveTo.IsEmpty())
			{
				if (!Ex.TempFile.IsEmpty())
				{
					// An earlier attempt's file; removed off the game thread (spec 1: disk IO on UE::Tasks)
					UE::Tasks::Launch(TEXT("VaRest.DeleteTemp"), [Stale = Ex.TempFile] { IFileManager::Get().Delete(*Stale, false, false, true); });
				}
				// Next to the destination: the same file system, so the final rename is a rename (spec 3.2: "atomic rename on
				// success"). A new name for every start, not every attempt: a stage after the attempt boundary may re-issue
				// (auth replay, spec 4), and the stale delete above must never name the live file (task 10 review I1).
				Ex.TempFile = MakeTempFileName(Ex.SaveTo, Ex.RequestId);
			}
			FVaRestTransportRequest Request;
			Request.RequestId = Ex.RequestId;
			Request.Attempt = Ex.Attempt;
			Request.Verb = Ex.Verb;
			// The attempt's own URL: an API key in the query is per attempt (AuthApply), as the credential headers are
			Request.Url = Ex.AttemptUrl;
			Request.Headers = Ex.AttemptHeaders;
			if (!Ex.Body.FilePath.IsEmpty())
			{
				Request.BodyFile = Ex.Body.FilePath;
			}
			else if (Ex.bMayReissue)
			{
				// A later attempt needs the body again (decision 12, plan decision 8)
				Request.BodySegments = Ex.Body.Segments;
				Request.Body = Ex.Body.Bytes;
			}
			else
			{
				// One attempt: moved all the way into IHttpRequest::SetContent(TArray<uint8>&&) (spec 2.4)
				Request.BodySegments = MoveTemp(Ex.Body.Segments);
				Request.Body = MoveTemp(Ex.Body.Bytes);
			}
			Request.Timeout = static_cast<float>(AttemptTimeout);
			Request.ActivityTimeout = static_cast<float>(AttemptActivity);
			Request.Priority = Ex.Priority;
			Request.ResponseFile = Ex.TempFile;
			// Reviews M-6, M13: the transport stops a JSON body past its cap instead of receiving all of it first
			Request.MaxBodyBytes = Ex.Handler.IsValid() && Ex.Handler->DecodesJson() ? Ex.MaxJsonBytes : 0;
			Request.Clock = Cfg.Clock;
			Request.Probe->UploadTotal = Ex.Body.ContentLength;
			Ex.Probe = Request.Probe;
			// IN-11: the progress delegate is bound only when someone reads it
			Request.bReportProgress = Ex.Handler.IsValid() && Ex.Handler->WantsProgress();
			// Decision 29 (review N-9): a callback, not only the trace, asked -- on curl the upload is then counted
			Request.bProgressCallback = Request.bReportProgress;
			Ex.LastProgress = FVaRestProgress();
			const bool bWantsProgress = Request.bReportProgress;
			// IN-11: the transport's timing delegates are bound only when a sink wants metadata at dispatch. UploadDoneAt needs
			// the progress binding too: curl stamps it from the counted upload, Apple from the progress delegate (Task 7). The
			// callback's own flags were taken above, so no progress timer runs and curl is not told a callback asked.
			if (FVaRestTrace::Wants(EVaRestTraceDetail::Metadata))
			{
				Request.bReportStatusTime = true;
				Request.bReportProgress = true;
			}
			// M4a final review A1: this dispatch's identity, which its completion carries back (FDispatchTag)
			FDispatchTag ThisDispatch;
			ThisDispatch.Attempt = Ex.Attempt;
			ThisDispatch.Serial = ++Ex.DispatchCount;
			ThisDispatch.Probe = Request.Probe;
			ThisDispatch.ResponseFile = Ex.TempFile;
			// IN-2: on the game thread, immediately before the transport starts
			TracePoints::AttemptDispatched(Ex);
			// The gap was this attempt's; the next one reports its own (IN-2), and so its own wait behind a refresh
			Ex.NextGap = EVaRestTraceGap::None;
			Ex.NextGapSeconds = 0.0;
			Ex.AuthWait = 0.0;
			Ex.AuthRefreshId = 0;
			if (Exchange->IsCancelRequested() || Ex.bFinished || Ex.bCompletionQueued)
			{
				// M4a final review A2 (review C M-1): the emit ran code that is not VaRest's -- a trace sink -- and it may
				// cancel (concurrency review M-5's rule, kept at every other emit). No attempt goes out after Cancel() returned
				// (spec 3.2: "no further attempt"), and none would hold a slot until the transport answered. The dispatch the
				// sinks saw gets its matching end, so the pair holds (Inspector 3.3); the answer is Canceled, which meets the
				// queued completion's guards on its way up.
				FVaRestTransportResult Unsent;
				Unsent.Failure = EVaRestTransportFailure::Canceled;
				Unsent.FailureDetail = TEXT("Canceled");
				if (!Ex.bFinished)
				{
					TracePoints::AttemptEnded(Ex, Unsent, ThisDispatch, /*bCurrent*/ true);
				}
				OnOutcome(FVaRestAttemptOutcome::Canceled());
				return;
			}
			// A call still held here belongs to a PerAttempt stage that proceeded again while it ran (a hedge; the stage
			// contract does not promise concurrent trips): this dispatch becomes the live one, and the other's progress chain
			// stops (ScheduleProgress), its end arriving as a stale one
			Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
			Ex.LiveDispatch = ThisDispatch.Serial;
			Ex.LiveDispatchAttempt = ThisDispatch.Attempt;
			Ex.TransportCall = Cfg.Transport->Start(MoveTemp(Request), [Guard = FCompletionGuard(Exchange, MoveTemp(ThisDispatch), MoveTemp(OnOutcome))](FVaRestTransportResult&& Result) mutable
			{
				Guard.Deliver(MoveTemp(Result));
			});
			if (bWantsProgress)
			{
				ScheduleProgress(Exchange, Ex.LiveDispatch);
			}
		}
	};
}

FVaRestAttemptOutcome FVaRestAttemptOutcome::Canceled()
{
	FVaRestAttemptOutcome Out;
	Out.Failure = EVaRestTransportFailure::Canceled;
	Out.FailureDetail = TEXT("Canceled");
	return Out;
}

FVaRestExchange::~FVaRestExchange()
{
	if (IsInGameThread())
	{
		return;
	}
#if !UE_BUILD_SHIPPING
	// The pipeline's own references die on the game thread (review I-1); only a pin taken elsewhere may be the last
	++VaRest::Http::Testing::Counters().ExchangesDestroyedOffGameThread;
#endif
	VaRest::Http::FExchangeState& Ex = *Impl;
	if (Ex.State.IsValid() || Ex.Config.IsValid() || Ex.Handler.IsValid())
	{
		// Review I-1 backstop: a pin taken on another thread (IsActive, Cancel) can be the last reference. The handler goes
		// too: it holds the user's callbacks and their captures (null by now in every path but a shutdown that never ran
		// the Enter task; task 8 review, minor 4).
		VaRest::Http::PipelinePrivate::OnGameThread(TEXT("VaRest.ReleaseExchange"),
			[DoomedState = MoveTemp(Ex.State), DoomedConfig = MoveTemp(Ex.Config), DoomedHandler = MoveTemp(Ex.Handler)] {});
	}
}

FVaRestExchange::FVaRestExchange()
	: Impl(MakeUnique<VaRest::Http::FExchangeState>())
{
}

uint64 FVaRestExchange::GetRequestId() const { return Impl->RequestId; }
uint64 FVaRestExchange::GetClientId() const { return Impl->ClientId; }
const FString& FVaRestExchange::GetVerb() const { return Impl->Verb; }
const FString& FVaRestExchange::GetUrl() const { return Impl->Url; }
const FString& FVaRestExchange::GetAttemptUrl() const { return Impl->AttemptUrl; }
const FString& FVaRestExchange::GetPathTemplate() const { return Impl->PathTemplate; }
FName FVaRestExchange::GetTag() const { return Impl->Tag; }
EVaRestPriority FVaRestExchange::GetPriority() const { return Impl->Priority; }
bool FVaRestExchange::IsIdempotent() const { return Impl->bIdempotent; }
const FVaRestHeaders& FVaRestExchange::GetRequestHeaders() const { return Impl->RequestHeaders; }
FVaRestHeaders& FVaRestExchange::GetAttemptHeaders() { return Impl->AttemptHeaders; }
const FVaRestHeaders& FVaRestExchange::GetAttemptHeaders() const { return Impl->AttemptHeaders; }
const FVaRestRequestBody& FVaRestExchange::GetBody() const { return Impl->Body; }
int32 FVaRestExchange::GetAttempt() const { return Impl->Attempt; }
bool FVaRestExchange::IsCancelRequested() const { return Impl->bCancelRequested.load(); }
bool FVaRestExchange::IsDone() const { return Impl->bDone.load(); }

void FVaRestStageNext::Proceed(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& OnOutcome) const
{
	check(IsInGameThread());
	const VaRest::Http::FExchangeState& Ex = VaRest::Http::Internals(*Exchange);
	// Only a request in the chain starts an attempt. Its answer accepted -- being decoded (Phase Decoding), delivered
	// (Done), or its delivery queued (Task 8 review minor 1; Phase stays InChain until that task runs) -- a backoff, a park
	// or a stage that proceeds again after it starts none: one check for re-review N-6's whole family (Task 8 re-review)
	if (Ex.Phase != EVaRestExchangePhase::InChain || Ex.bFinished || Ex.bCompletionQueued || Exchange->IsCancelRequested())
	{
		OnOutcome(FVaRestAttemptOutcome::Canceled());
		return;
	}
	checkf(Chain->IsValidIndex(Index), TEXT("FVaRestStageNext: the transport stage is last and never proceeds"));
	(*Chain)[Index]->Process(Exchange, FVaRestStageNext(Chain, Index + 1), MoveTemp(OnOutcome));
}

FVaRestStageRef VaRest::Http::MakeAttemptBoundaryStage()
{
	return MakeShared<PipelinePrivate::FAttemptBoundaryStage, ESPMode::ThreadSafe>();
}

FVaRestStageRef VaRest::Http::MakeLimiterStage()
{
	return MakeShared<PipelinePrivate::FLimiterStage, ESPMode::ThreadSafe>();
}

FVaRestStageRef VaRest::Http::MakeTransportStage()
{
	return MakeShared<PipelinePrivate::FTransportStage, ESPMode::ThreadSafe>();
}

VaRest::Http::FFinish VaRest::Http::FClientState::MakeFinish(const FExchangeState& Exchange) const
{
	FFinish Finish;
	Finish.Response.RequestId = Exchange.RequestId;
	Finish.Response.Attempts = Exchange.Attempt;
	Finish.SaveTo = Exchange.SaveTo;
	Finish.TempFile = Exchange.TempFile;
	Finish.bKeepBody = Exchange.bKeepBody;
	Finish.DecodeOptions = Exchange.DecodeOptions;
	Finish.ParseOptions.MaxInputBytes = Exchange.MaxJsonBytes;
	Finish.Clock = Exchange.Config->Clock;
	Finish.Envelope = Exchange.Envelope;
	Finish.LogLevel = Exchange.LogLevel;
	return Finish;
}

bool VaRest::Http::FClientState::Resolve(FExchangeState& Exchange, FString& OutError) const
{
	const FClientConfig& Cfg = *Exchange.Config;
	// Final review I3: the verb is request-line text. curl sends any verb but GET, POST, PUT, HEAD and DELETE verbatim as
	// CURLOPT_CUSTOMREQUEST (CurlHttp.cpp:990-993), so CR LF or a space in it would rewrite the request line and inject
	// headers. The engine refuses only an empty one, as a retryable-looking EHttpFailureReason::Other
	// (HttpRequestCommon.cpp:103-107). A method is an RFC 9110 9.1 token, IsValidHeaderName's rule; nothing is trimmed, so
	// " GET" is refused, not repaired. Escaped: the message is text a game may log as it is.
	if (Exchange.Verb.IsEmpty())
	{
		OutError = TEXT("the verb is empty; a method is an RFC 9110 token such as GET");
		return false;
	}
	if (!IsValidHeaderName(Exchange.Verb))
	{
		OutError = FString::Printf(TEXT("the verb \"%s\" is not an RFC 9110 token (letters, digits and !#$%%&'*+-.^_`|~ only: no space or control character)"),
			*EscapeForLog(Exchange.Verb));
		return false;
	}
	if (Cfg.BaseUrl.Contains(TEXT("#")))
	{
		// Decision 33 (review M22): a base URL may carry a query (kept, before the request's own pairs), not a fragment
		OutError = FString::Printf(TEXT("the base URL \"%s\" has a fragment; a base URL may carry a query but not a fragment"), *RedactorFor(Exchange)->RedactUrl(Cfg.BaseUrl));
		return false;
	}
	int32 QueryAt = INDEX_NONE;
	const bool bHasQuery = Exchange.RawPath.FindChar(TEXT('?'), QueryAt);
	Exchange.PathTemplate = bHasQuery ? Exchange.RawPath.Left(QueryAt) : Exchange.RawPath;

	// The template is expanded before the join, so a brace in the base URL is never touched. Only the path is a template
	// (review M12): a brace in the query is the caller's text and goes out as written.
	TValueOrError<FString, FString> Expanded = ExpandPathTemplate(Exchange.PathTemplate, Exchange.PathParams);
	if (Expanded.HasError())
	{
		// Final review M8: ExpandPathTemplate has no redactor, so its message names no template; an absolute Path may carry
		// userinfo ("https://u:pw@h/{x"), and this text becomes Error.Message, the Basic Warning (LogConfigError escapes,
		// it does not redact) and the trace (ScrubText only). The template is shown here, redacted like every URL.
		OutError = FString::Printf(TEXT("%s (path template \"%s\")"), *Expanded.GetError(), *RedactorFor(Exchange)->RedactUrl(Exchange.PathTemplate));
		return false;
	}
	FString Resolved = JoinUrl(Cfg.BaseUrl, bHasQuery ? Expanded.GetValue() + Exchange.RawPath.Mid(QueryAt) : Expanded.GetValue());
	if (!IsAbsoluteHttpUrl(Resolved))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not an absolute http(s) URL (base URL \"%s\")"), *RedactorFor(Exchange)->RedactUrl(Resolved), *RedactorFor(Exchange)->RedactUrl(Cfg.BaseUrl));
		return false;
	}
	if (HasNonAsciiHost(Resolved))
	{
		// Task 3 review I2: JoinUrl encodes a non-ASCII path, but a host would need punycode (IDNA), which is out of scope,
		// and curl takes the URL through TCHAR_TO_ANSI (CurlHttp.cpp:926), so every such character would go out as '?'
		OutError = FString::Printf(TEXT("the host of \"%s\" is not ASCII; an internationalised domain name must be given in its punycode (xn--) form"),
			*RedactorFor(Exchange)->RedactUrl(Resolved));
		return false;
	}
	if (HasControlInAuthority(Resolved))
	{
		// Task 14 fix round 2: JoinUrl percent-encodes CTLs and spaces only after the authority (VaRestUrl.cpp:146), and
		// FindAuthority ends it only at '/', '?' or '#' (:121), so "https://h\r\n.../x" -- an absolute URL a server handed
		// back -- would reach the transport, which refuses it without saying why. The URL stays out of the message: it is
		// in Error.Url (redacted), and the message is text a game may log as it is.
		OutError = TEXT("the URL's authority (after the scheme, to the first '/', '?' or '#') holds a control character or a space");
		return false;
	}
	AppendQuery(Resolved, Exchange.Query);
	Exchange.Url = MoveTemp(Resolved);

	// Client defaults, then the request's own: they win whatever the case of the name (spec 3.2: "Every content type can be overridden with Header")
	Exchange.RequestHeaders = Cfg.DefaultHeaders;
	for (const FVaRestHeaders::FPair& Pair : Exchange.OwnHeaders)
	{
		Exchange.RequestHeaders.Set(Pair.Key, Pair.Value);
	}
	if (!Exchange.Body.IsEmpty() && Exchange.Verb.Equals(TEXT("GET"), ESearchCase::IgnoreCase))
	{
		// Spec 3.2: "A body on GET is dropped with a warning (v1 rule)"; the engine would refuse the request
		// (HttpRequestCommon.cpp:764-768: "An HTTP Get request cannot contain a payload")
		UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: GET %s has a body; it is dropped (spec 3.2)"), Exchange.RequestId, *EscapeForLog(RedactorFor(Exchange)->RedactUrl(Exchange.Url)));
		Exchange.Body = FVaRestRequestBody();
	}
	if (!Exchange.Body.IsEmpty() && Cfg.Transport.IsValid() && !Cfg.Transport->SendsBodyFor(Exchange.Verb))
	{
		// Plan decision 13 (final review M1): sent without its body it would be another request, and BytesUp would count bytes
		// that never left (VaRestHttpTransport.cpp:165). The verb is a token here (checked above), so it is shown as it is.
		OutError = FString::Printf(TEXT("the client's transport sends no request body with %s (the engine's HTTP transport sends one only with POST, PUT, PATCH and DELETE)"),
			*Exchange.Verb);
		return false;
	}
	if (!Exchange.Body.IsEmpty() && !Exchange.OwnHeaders.Contains(TEXT("Content-Type")))
	{
		// Final review I2: the request's own Header() beats the body's type, which beats a client default (WithHeader):
		// a multipart body must name its boundary, and a client-wide "application/json" must not relabel a form. Set
		// replaces a client default of any case, in its place (FVaRestHeaders::Set, VaRestHttpTypes.cpp:8-36).
		Exchange.RequestHeaders.Set(TEXT("Content-Type"), Exchange.Body.ContentType);
	}
	if (Exchange.bIdempotent && !Exchange.RequestHeaders.Contains(TEXT("Idempotency-Key")))
	{
		// Generated once per logical request, into the request headers every attempt copies: stable across attempts (spec 3.2)
		Exchange.RequestHeaders.Set(TEXT("Idempotency-Key"), FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower));
	}

	// Spec 4.1, plan decisions 1 and 8
	Exchange.RetryPolicy = Exchange.RetryOverride.IsSet() ? Exchange.RetryOverride.GetValue() : Cfg.Retry;
	Exchange.bRetryEligible = Exchange.RetryPolicy.IsEnabled() && (Exchange.bIdempotent || IsIdempotentVerb(Exchange.Verb));
	// Plan decision 8: the body is copied only when something may send it again -- a custom stage, a retry, or a session
	// replay (the auth gate, spec 4.2): an auth that can refresh this session may replay any verb after a 401 (the gate's
	// own rule, CanRefreshNow: M4a final review B5)
	const bool bAuthMayReplay = Cfg.Auth.IsValid() && Exchange.AuthMode != EVaRestAuthMode::None && !Exchange.bIsAuthRefresh
		&& CanRefreshNow(*Cfg.Auth, GetSessionData());
	Exchange.bMayReissue = Cfg.bMayReissue || Exchange.bRetryEligible || bAuthMayReplay;
	// Plan decision 10: an absolute time on the request's clock; Enter stamped StartedAt before Resolve
	const float Deadline = Exchange.RetryPolicy.Deadline;
	Exchange.DeadlineAt = FMath::IsFinite(Deadline) && Deadline > 0.f ? Exchange.StartedAt + Deadline : 0.0;
	// Spec 3.4: the request's own, else the client's; NoEnvelope() none. Once per logical request: only the final attempt is
	// decoded (OnChainDone), so the envelope judges that one, after the retry stage has decided on every attempt's status.
	if (Exchange.bNoEnvelope)
	{
		Exchange.Envelope.Reset();
	}
	else if (Exchange.EnvelopeOverride.IsSet())
	{
		// Moved: the shared copy is the only one the request needs from here on (Task 11 review M-6)
		Exchange.Envelope = MakeShared<const FVaRestEnvelope, ESPMode::ThreadSafe>(MoveTemp(Exchange.EnvelopeOverride.GetValue()));
		Exchange.EnvelopeOverride.Reset();
	}
	else
	{
		Exchange.Envelope = Cfg.Envelope;
	}
	Exchange.Timeout = Exchange.TimeoutOverride.Get(Cfg.Timeout);
	Exchange.ActivityTimeout = Exchange.ActivityTimeoutOverride.Get(Cfg.ActivityTimeout);
	Exchange.MaxJsonBytes = Exchange.MaxJsonBytesOverride.Get(Cfg.MaxJsonBytes);
	Exchange.DecodeOptions.Naming = Exchange.Naming;
	Exchange.DecodeOptions.bStrict = Exchange.bStrictDecode;
	Exchange.LogLevel = EffectiveLogLevel(Cfg.LogLevel);
	if (Exchange.RetryOverride.IsSet() && Exchange.RetryPolicy.IsEnabled() && !Exchange.bRetryEligible && Exchange.LogLevel != EVaRestLogLevel::None
		&& UE_LOG_ACTIVE(LogVaRestHttp, Log))
	{
		// M4a final review A4 (review A M-3): this request asked for a retry policy itself, and spec 4.1's safety rule makes
		// it inert -- a POST or PATCH is retried only when marked Idempotent() -- which compiles and is silent otherwise.
		// The request's own Retry() only: a client-wide WithRetry() is the rule working as meant for every POST. The verb is
		// a token here (checked above), so it is shown as it is.
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu VaRest: %s is not retried without Idempotent() (spec 4.1)"), Exchange.RequestId, *Exchange.Verb);
	}
	return true;
}

void VaRest::Http::FClientState::Enter(const FVaRestExchangeRef& Exchange)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	LLM_SCOPE_BYTAG(VaRest);
	FExchangeState& Ex = Internals(*Exchange);
	FApiScope Scope(Ex);
	// Spec 3.1: "A request snapshots the configuration at Send()" -- SendRequest took it (decision 27); an exchange made
	// any other way gets one here
	if (!Ex.Config.IsValid())
	{
		Ex.Config = GetConfig();
	}
	Ex.StartedAt = Ex.Config->Clock->Now();
	Ex.Phase = EVaRestExchangePhase::InChain;
	// Task 15 review M-1: a refresh request's id is preset (spec review I-2), and a second exchange under one id would
	// replace the first here -- CancelAll and the shutdown cancel would no longer see the first (Complete removes only its
	// own entry, Task 16 review M-7, so the second stays reachable). Only FClientAccess::MakeRefreshContext can make one with
	// an id in use. Always: each duplicate is the same bug, not only the session's first (AssertionMacros.cpp:970-975).
	// Task 16 review M-7: another exchange under the id, not this one entered again (harmless: the entry stays its own).
	const FVaRestExchangeRef* InUse = InFlight.Find(Ex.RequestId);
	ensureAlwaysMsgf(!InUse || *InUse == Exchange, TEXT("VaRest: request id #%llu is already in flight on this client (a refresh context made with an id in use)"), Ex.RequestId);
	InFlight.Add(Ex.RequestId, Exchange);
	if (!Ex.bCounted)
	{
		// Spec 4.8: one request per exchange, whatever ends it -- every end reaches Complete, which counts it out. Before the
		// cancel and owner checks below: a request that ends here (shutdown, owner gone, canceled before the hop, a
		// configuration error) is counted in and out like any other. Once: an exchange entered again stays one request.
		Ex.bCounted = true;
		FHttpStats::RequestStarted(Counters);
	}
	if (IsShuttingDown())
	{
		// Review M-14: after OnEnginePreExit; the HTTP module unloads next (LaunchEngineLoop.cpp:5182)
		Ex.CancelReason = EVaRestCancelReason::Shutdown;
		Ex.bCancelRequested = true;
		Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
	}
	if (IsOwnerGone(Ex))
	{
		// Silent by shape: marked here, so the entry path does not rest on Complete's own re-check (task 11 review, minor 1)
		MarkOwnerGone(Ex);
	}
	if (Ex.IsCancelRequested())
	{
		// Canceled before it reached the game thread, its owner is already gone, or the engine is exiting: no transport call.
		// Still Started and Completed for a sink (Inspector 2.3: the record is sealed, not missing).
		TracePoints::Started(Ex);
		Complete(Exchange, MakeFinish(Ex));
		return;
	}

	FString Problem;
	const bool bResolved = Ex.BuildError.IsEmpty() && Resolve(Ex, Problem);
	// IN-1, after Resolve: a sink sees the URL and the headers the request goes out with
	TracePoints::Started(Ex);
	if (!bResolved)
	{
		FFinish Finish = MakeFinish(Ex);
		Finish.Error.Kind = EVaRestErrorKind::Config;
		Finish.Error.Message = Ex.BuildError.IsEmpty() ? MoveTemp(Problem) : Ex.BuildError;
		// It never reaches the log stage: its Basic line is written here
		LogConfigError(Ex, Finish.Error.Message);
		Complete(Exchange, MoveTemp(Finish));
		return;
	}
	FVaRestStageNext(Ex.Config->Chain.ToSharedRef(), 0).Proceed(Exchange, [Exchange](FVaRestAttemptOutcome&& Outcome)
	{
		Internals(*Exchange).State->OnChainDone(Exchange, MoveTemp(Outcome));
	});
}

void VaRest::Http::FClientState::OnAttemptDone(const FVaRestExchangeRef& Exchange, FDispatchTag&& Tag, FVaRestTransportResult&& Result, FVaRestOutcomeCallback&& OnOutcome)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	LLM_SCOPE_BYTAG(VaRest);
	FExchangeState& Ex = Internals(*Exchange);
	// M4a final review A1 (VaRest-tqt): only the end of the dispatch TransportCall holds touches the exchange's one-call
	// slots. Any other is stale: a call a stage answered for, which the attempt boundary or EndWaits canceled, or one a
	// PerAttempt stage's second trip replaced. Its slots now name another call (or none), so resetting them would leave
	// Cancel() unable to reach the live call, its limiter slot held and its progress stopped (review A I-1).
	const bool bCurrent = Tag.Serial != 0 && Tag.Serial == Ex.LiveDispatch;
	if (bCurrent)
	{
		Ex.TransportCall.Reset();
		Ex.LiveDispatch = 0;
	}
	// Spec 4.8: every attempt that reached a transport, as the transport counted it -- a retried or replayed attempt each
	// time, a late one whose request a cancel already finished, and a stale one (their bytes moved all the same)
	FHttpStats::Transferred(Counters, Result.BytesUp, Result.BytesDown);
	if (IsOwnerGone(Ex))
	{
		// Seen as canceled by every stage the outcome passes on its way up (M4's retry stops here)
		MarkOwnerGone(Ex);
	}
	if (!Ex.bFinished)
	{
		// IN-3, "when the outcome re-enters the stage chain". After Completed (a cancel finished the request while the
		// answer was queued) the record is sealed (Inspector 2.3), so a late attempt is not reported. A1 (ruling: relabel,
		// not drop): a stale end is still reported, under its own dispatch's attempt and transport times, so its
		// AttemptDispatched stays paired; it announces no excerpt and leaves the decode marker alone.
		TracePoints::AttemptEnded(Ex, Result, Tag, bCurrent);
	}
	if (bCurrent)
	{
		Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
		// The final counts, so a caller sees 100% even when the transfer was quicker than one tick (decision 8)
		DeliverProgress(Ex);
	}
	else if (!Tag.ResponseFile.IsEmpty() && (Tag.ResponseFile != Ex.TempFile || Ex.bFinished || Ex.bCompletionQueued))
	{
		// A1: a stale call's own SaveTo file, which it may have created after the next dispatch deleted that name (the file
		// sink creates it at the first byte, VaRestBodySink.cpp:48), or which a Windows delete could not remove while the
		// call held it open. The call has ended, so this cannot race it; off the game thread (spec 1). Ex.TempFile while the
		// request is in the chain or decoding is its later owner's: the next dispatch deletes it, or the decode task renames
		// or deletes it (RunDecode); once the answer is accepted without a decode, nothing else will.
		UE::Tasks::Launch(TEXT("VaRest.DeleteTemp"), [Stale = MoveTemp(Tag.ResponseFile)] { IFileManager::Get().Delete(*Stale, false, false, true); });
	}
	FVaRestAttemptOutcome Outcome;
	Outcome.Status = Result.Status;
	Outcome.Headers = MoveTemp(Result.Headers);
	Outcome.EffectiveUrl = MoveTemp(Result.EffectiveUrl);
	Outcome.ContentType = MoveTemp(Result.ContentType);
	Outcome.Body = MoveTemp(Result.Body);
	Outcome.bBodyInFile = Result.bBodyInFile;
	Outcome.Failure = Result.Failure;
	Outcome.FailureDetail = Result.FailureDetail;
	Outcome.BytesUp = Result.BytesUp;
	Outcome.BytesDown = Result.BytesDown;
	OnOutcome(MoveTemp(Outcome));
}

void VaRest::Http::FClientState::OnChainDone(const FVaRestExchangeRef& Exchange, FVaRestAttemptOutcome&& Outcome)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FExchangeState& Ex = Internals(*Exchange);
	if (Ex.bFinished || Ex.bCompletionQueued || Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Finished, its answer's delivery queued (Task 8 review minor 1), or a second outcome while the first is being
		// decoded (a stage that reported twice): ignored (review C-3), so a Canceled cannot overtake a queued answer. A
		// finished or queued one's attempt file is removed (a second outcome's file is its decode task's to finish).
		if ((Ex.bFinished || Ex.bCompletionQueued) && !Ex.TempFile.IsEmpty())
		{
			UE::Tasks::Launch(TEXT("VaRest.DeleteTemp"), [Stale = Ex.TempFile] { IFileManager::Get().Delete(*Stale, false, false, true); });
		}
		return;
	}
	FFinish Finish = MakeFinish(Ex);
	Finish.Response.Status = Outcome.Status;
	Finish.Response.Headers = MoveTemp(Outcome.Headers);
	Finish.Response.EffectiveUrl = MoveTemp(Outcome.EffectiveUrl);
	Finish.Response.ContentType = MoveTemp(Outcome.ContentType);
	Finish.Response.BytesUp = Outcome.BytesUp;
	Finish.Response.BytesDown = Outcome.BytesDown;
	Finish.Body = MoveTemp(Outcome.Body);
	Finish.bBodyInFile = Outcome.bBodyInFile;

	static const FName TooLarge(TEXT("ResponseTooLarge"));
	// Plan decision 12: one classification for this error and for the retry stage (Task 8)
	const FAttemptClass Class = ClassifyAttempt(Outcome);
	if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge && Class.Kind == EVaRestErrorKind::Http)
	{
		// Reviews M-6, M13: an error response whose body passed the JSON cap is still the HTTP error it was; the body is gone
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FString::Printf(TEXT("%s (its body is over the MaxJsonBytes limit of %lld and was not kept)"),
			*FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status), Ex.MaxJsonBytes);
		Finish.Error.bRetryable = Class.bRetryable;
	}
	else if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge)
	{
		// Decision 20: the transport stopped the body at the cap -- bounded memory on untrusted input
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("the JSON response is over the MaxJsonBytes limit of %lld; the transfer stopped there"), Ex.MaxJsonBytes);
	}
	else if (Outcome.IsTransportFailure())
	{
		Finish.Error.Kind = Class.Kind;
		Finish.Error.Message = Outcome.FailureMessage.IsEmpty() ? FVaRestError::DefaultMessage(Class.Kind, 0) : Outcome.FailureMessage;
		if (!Outcome.FailureDetail.IsNone())
		{
			Finish.Error.Message += FString::Printf(TEXT(" (%s)"), *Outcome.FailureDetail.ToString());
		}
		Finish.Error.bRetryable = Class.bRetryable;
	}
	else if (Class.Kind == EVaRestErrorKind::Http)
	{
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status);
		Finish.Error.bRetryable = Class.bRetryable;
	}

	const bool bHasBody = Finish.Body.Num() > 0 || Finish.bBodyInFile;
	// A download to finish (SaveTo's success, from the transport or from a stage, review M1) or to clean up: disk IO, never
	// on the game thread
	const bool bDownload = !Finish.TempFile.IsEmpty() || (!Finish.SaveTo.IsEmpty() && !Finish.Error.IsError());
	// Plan decision 9: a void result with a predicate is parsed too, on the task, never here (concurrency review I-6)
	const bool bEnvelopeCheck = Finish.Envelope.IsValid() && Finish.Envelope->HasPredicate() && Ex.Handler->AppliesEnvelope() && !Finish.bBodyInFile;
	if (bEnvelopeCheck && Ex.LogLevel != EVaRestLogLevel::None)
	{
		// M4a final review A6 (review C M-3): the decode task's Basic Warning shows the server's Content-Type; values in log
		// lines are redacted, then escaped, as the trace (VaRestTracePoints.cpp:230) and the Headers lines
		// (VaRestLogStage.cpp:172) scrub that header. RedactorFor checks the game thread, so it is taken here.
		Finish.LogRedactor = RedactorFor(Ex);
	}
	const bool bTask = bDownload
		|| (Finish.Error.IsError() ? Finish.Error.Kind == EVaRestErrorKind::Http && bHasBody : (Ex.Handler->DecodesBody() || bEnvelopeCheck) && bHasBody);
	if (!bTask)
	{
		// Nothing to parse (no body, raw bytes, void): the game thread only moves things
		if (!Finish.Error.IsError())
		{
			Ex.Handler->Decode(Finish);
		}
		Complete(Exchange, MoveTemp(Finish));
		return;
	}

	if (Ex.ExcerptOnDecodeAttempt > 0 && Ex.ExcerptOnDecodeAttempt == Ex.Attempt && !Finish.Error.IsError() && Finish.Body.Num() > 0)
	{
		// IN-14: this response goes to the decoder; the task excerpts it from the body itself. The redactor is taken here, on
		// the game thread (RedactorFor checks it): the Send() snapshot's secrets, what AuthApply joined in for the attempts
		// (Task 12 review M-7) and the current ones -- a refresh response's token keys among them (Task 18).
		Finish.ExcerptAttempt = Ex.Attempt;
		Finish.ExcerptRedactor = RedactorFor(Ex);
		Finish.ExcerptCap = FVaRestTrace::GetDetailSettings().BodyCap;
		// Task 21 review M-1: handed over, so the decode task resolves it. EmitOnDecode drops a handed-over excerpt only when
		// no sink wants Bodies any more; the body check above keeps its empty-body return from dropping one. One not handed
		// over (an error, an empty body a stage put there, a request finished first) stays marked and resolves as Skipped:
		// Discarded (Excerpts::ResolveDiscarded).
		Ex.ExcerptOnDecodeAttempt = 0;
	}
	Finish.bTimeDecode = FVaRestTrace::Detail() != EVaRestTraceDetail::None;
	// Re-review N-6: a stage that waits (a backoff, a park) is ended here, as Complete ends it -- the decode task runs for
	// a while before Complete, and a timer or a refresh's end in that window would start a network attempt for a request
	// already being delivered; so is a retry waiting in the limiter's queue (Task 8 review minor 3). Ended right after the
	// phase moves (nothing runs in between: the ruling's "taken before, run after"), so a Canceled sent back up the chain
	// meets OnChainDone's guard (Phase != InChain) instead of counting as a second outcome.
	Ex.Phase = EVaRestExchangePhase::Decoding;
	EndWaits(Ex);
	// The handler goes with the task and comes back with the result: nothing else touches it meanwhile
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	// The exchange travels as a TSharedPtr moved into the continuation, so the worker never drops the last reference
	// (review I-1; TSharedRef's move constructor copies, SharedPointer.h:354-361)
#if !UE_BUILD_SHIPPING
	++Testing::Counters().DecodeTasksLaunched;
#endif
	UE::Tasks::Launch(TEXT("VaRest.Decode"), [Held = TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe>(Exchange), Decoder = MoveTemp(Handler), Pending = MoveTemp(Finish)]() mutable
	{
		// Spec 4.8: the decode's DOM and values are allocated here, so they are VaRest's. LLM charges memory where it is
		// allocated, to the thread's active tags (LLM.cpp:5237-5239), stores that group with the pointer (:5245-5252) and
		// credits the same group back at the free (:5285, :5292): a decoded value the game keeps after its callback stays
		// under VaRest until it is freed, whoever holds it. A launch from OnAttemptDone or Enter inherits this tag from their
		// scope (TaskPrivate.h:192, :521); one from elsewhere -- a custom stage answering from its own timer -- would not.
		LLM_SCOPE_BYTAG(VaRest);
		// Spec 1: "Parsing, decoding, cache disk IO ... run on UE::Tasks background tasks"
		RunDecode(*Decoder, Pending, Internals(*Held));
		OnGameThread(TEXT("VaRest.Deliver"), [Target = MoveTemp(Held), Delivered = MoveTemp(Decoder), Done = MoveTemp(Pending)]() mutable
		{
			FExchangeState& Returned = Internals(*Target);
			Returned.Handler = MoveTemp(Delivered);
			Returned.State->Complete(Target.ToSharedRef(), MoveTemp(Done));
		});
#if !UE_BUILD_SHIPPING
		// Last, after the result is queued for the game thread (Testing::FTestRig::Drain)
		++Testing::Counters().DecodeTasksHandedBack;
#endif
	});
}

void VaRest::Http::FClientState::Complete(const FVaRestExchangeRef& Exchange, FFinish&& Finish)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FExchangeState& Ex = Internals(*Exchange);
	if (Ex.bFinished)
	{
		return;
	}
	if (Ex.Phase == EVaRestExchangePhase::Decoding && !Ex.Handler.IsValid())
	{
		// Review C-3: a decode task holds the handler. It completes the exchange when it returns (as Canceled when the
		// cancel flag is set: see below), so finishing here would leave nothing to deliver to and lose every callback.
		return;
	}
	const bool bOwnedSilentShutdown = Ex.bPersistenceOwned && Ex.CancelReason == EVaRestCancelReason::Shutdown;
	if (Ex.ApiDepth > 0 && !bOwnedSilentShutdown)
	{
		Ex.bCompletionQueued = true;
		EndWaits(Ex);
		if (Ex.bPersistenceOwned)
		{
			check(Ex.PersistenceCompletionTicket != 0);
			SetPersistenceCompletion(Ex.PersistenceCompletionTicket,
			                         [Exchange, Pending = MoveTemp(Finish)]() mutable { Internals(*Exchange).State->Complete(Exchange, MoveTemp(Pending)); });
			return;
		}
		++PendingApiCompletionWrappers;
		OnGameThread(TEXT("VaRest.Complete"),
		             [Exchange, Pending = MoveTemp(Finish)]() mutable
		             {
			             const auto Client = Internals(*Exchange).State;
			             --Client->PendingApiCompletionWrappers;
			             Client->Complete(Exchange, MoveTemp(Pending));
		             });
		return;
	}
	ReleasePersistenceCompletion(Ex.PersistenceCompletionTicket);
	Ex.PersistenceCompletionTicket = 0;

	if (IsOwnerGone(Ex))
	{
		// Spec 3.2: owner gone -> no callbacks, whatever canceled it first (a Cancel() in the owner's EndPlay, review C-2)
		MarkOwnerGone(Ex);
	}
	// Read once: a worker's Cancel() landing between two reads would deliver Kind=Canceled with no reason (task 11 review, minor 4)
	const bool bCanceled = Ex.IsCancelRequested();
	if (bCanceled && Ex.CancelReason == EVaRestCancelReason::None)
	{
		Ex.CancelReason = EVaRestCancelReason::User; // the flag set off the game thread before the reason
	}
	if (bCanceled && Finish.Response.SavedFile.IsEmpty())
	{
		// After Cancel() nothing but Canceled is delivered, even an answer that raced it (decision 2). A SaveTo whose
		// rename already replaced the destination delivers its success: the truthful outcome (review M-4).
		FFinish Canceled = MakeFinish(Ex);
		Canceled.Error.Kind = EVaRestErrorKind::Canceled;
		Canceled.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Canceled, 0);
		Finish = MoveTemp(Canceled);
	}
	Ex.bFinished = true;
	Ex.Phase = EVaRestExchangePhase::Done;
	// Concurrency review M-3: something other than Cancel finished a request a stage holds waiting (a custom Outer stage in
	// front of Retry answering by itself): the hook clears that stage's timer or park, and a queued retry leaves the
	// limiter's queue (Task 8 review minor 3); their Canceled lands on a finished request, which OnChainDone ignores
	EndWaits(Ex);

	const FClientConfig& Cfg = *Ex.Config;
	Finish.Response.Elapsed = Cfg.Clock->Now() - Ex.StartedAt;
	Finish.Response.Attempts = Ex.Attempt;
	if (Finish.Error.IsError())
	{
		Finish.Error.Attempts = Ex.Attempt;
		Finish.Error.Tag = Ex.Tag;
		// Spec 3.3: "Url (redacted)"
		Finish.Error.Url = RedactorFor(Ex)->RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
		if (Finish.Error.Message.IsEmpty())
		{
			Finish.Error.Message = FVaRestError::DefaultMessage(Finish.Error.Kind, Finish.Error.HttpStatus);
		}
	}

	Ex.bDone = true;
	// Task 16 review M-7: this exchange's own entry only. A second exchange that Enter let in under the same id (it ensures)
	// replaced it; removing by id would take that one out, and CancelAll and the shutdown cancel would no longer reach it.
	if (const FVaRestExchangeRef* Entry = InFlight.Find(Ex.RequestId); Entry && *Entry == Exchange)
	{
		InFlight.Remove(Ex.RequestId);
	}
	if (Ex.bCounted)
	{
		// Spec 4.8: here, once (bFinished above), for every end -- before the silent return below (owner gone, shutdown), and
		// whether or not the map entry was this exchange's: a second exchange under the id (the ensure in Enter) replaced
		// it, and both were counted in
		FHttpStats::RequestFinished(Counters);
	}
	// IN-5: before any callback, OnHttpThread ones included, and also when none will run (owner gone, shutdown)
	TracePoints::Completed(Ex, Finish);
	// The request has ended: its cached union goes (a late outcome's line rebuilds one). What its attempts applied
	// (AppliedRedactor, Task 12 review M-7) stays with the exchange, as its snapshot does.
	Ex.UnionRedactor.Reset();
	Ex.UnionBuiltFrom.Reset();
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	// Spec 3.2: owner gone -> no callbacks; nor at engine exit (decision 2)
	const bool bSilent = Ex.CancelReason == EVaRestCancelReason::OwnerGone || Ex.CancelReason == EVaRestCancelReason::Shutdown;
	if (!Handler.IsValid() || bSilent)
	{
		return;
	}
	// Spec 4.8: the game's callbacks run under the tags the request was sent with, not VaRest's (FLlmSenderTags); the
	// worker task below takes them from this scope at its launch (TaskPrivate.h:192)
	VAREST_LLM_SCOPE_SENDER(Ex.SenderTags);
	if (Ex.bOnHttpThread)
	{
		// Decision 4: the game thread has seen the result (Completed, Task 15); the callbacks run on a worker.
		// Decision 28 (review C-5): the owner, live here on the game thread, is pinned until the callbacks return -- a
		// worker may not ask a TWeakObjectPtr (WeakObjectPtr.h:290-300), and GC could free the owner under a [this]
		// callback. TStrongObjectPtr takes its reference here (Reset(InNewObject), StrongObjectPtrTemplates.h:152-169,
		// AddRef at :167, with the incremental-GC barrier of FUObjectItem::AddRef, UObjectArray.h:347-366) and drops it on
		// the worker, which 5.8.1 allows: ~TStrongObjectPtr -> Reset -> ReleaseUObject (StrongObjectPtrTemplates.h:47-50,
		// :122-131; StrongObjectPtr.cpp:10-16) is an atomic decrement (UObjectArray.h:375-386) whose root-dirtying takes a
		// try-lock or queues lock-free, from any thread (GarbageCollection.cpp:701-719).
		TStrongObjectPtr<const UObject> Pin;
		if (Ex.bHasOwner)
		{
			Pin.Reset(Ex.Owner.Get());
		}
		// Review I-1: nothing the worker drops may be the clock's last reference (a clock's destructor may clear
		// game-thread timers); the callbacks do not read it, so it stays behind and dies on this thread
		Finish.Clock.Reset();
#if !UE_BUILD_SHIPPING
		++Testing::Counters().OffGameThreadDeliveries;
#endif
		UE::Tasks::Launch(TEXT("VaRest.DeliverOffGameThread"), [Delivered = MoveTemp(Handler), Done = MoveTemp(Finish), Kept = MoveTemp(Pin)]() mutable
		{
			Delivered->Deliver(Done);
			Kept.Reset(); // after the callbacks, on this worker: legal, see above
		});
		return;
	}
	Handler->Deliver(Finish);
}

void VaRest::Http::FClientState::Cancel(const FVaRestExchangeRef& Exchange, EVaRestCancelReason Reason)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	// M4a final review A5 (review A M-4, spec 4.8): the game's call (or the progress timer's owner check): what a cancel
	// runs -- a wait's answer up the chain, the log line, the FFinish -- is VaRest's; the callbacks still switch to the
	// sender's tags (Complete)
	LLM_SCOPE_BYTAG(VaRest);
	FExchangeState& Ex = Internals(*Exchange);
	if (Ex.bFinished)
	{
		return;
	}
	// Review C-2: the first reason wins, but silence (OwnerGone, Shutdown) overrides User
	if (Ex.CancelReason == EVaRestCancelReason::None || Reason == EVaRestCancelReason::Shutdown || Reason == EVaRestCancelReason::OwnerGone)
	{
		Ex.CancelReason = Reason;
	}
	Ex.bCancelRequested = true;
	Ex.PersistenceAbort->store(true, std::memory_order_relaxed);
	if (Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Created: Enter sees the flag. Decoding: Complete replaces the decoded result when it comes back.
		return;
	}
	FApiScope Scope(Ex);
	if (Ex.bInLimiterQueue && Limiter.Abort(Ex.RequestId))
	{
		// Out of the queue: its abort answers Canceled up the chain, and no slot was held
		return;
	}
	if (Ex.TransportCall.IsValid())
	{
		if (!Ex.Interrupt)
		{
			// Review M-3: no progress tick after Cancel() returns (ScheduleProgress re-arms nothing once the flag is set)
			Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
			// The transport answers Canceled through the normal path, which releases what the attempt holds
			Ex.TransportCall->Cancel();
			return;
		}
		// Task 8 review I-1: a PerAttempt stage answered this call's attempt itself while the call ran (its own timeout), and
		// a stage now waits (Retry's backoff): the call is stale -- its Canceled reaches only the stage that answered it --
		// and the wait holds the request, so the wait is ended below as well. Defence in depth since M4a final review A1:
		// the attempt boundary cancels such a call when the stage's answer passes it, so a wait never starts with one held.
		CancelHeldCall(Ex);
	}
	if (Ex.Interrupt)
	{
		// Plan decision 7: a stage holds it waiting (a backoff, a park behind a refresh); it answers Canceled through the
		// chain, so every Outer stage gets its one outcome. Taken first: answering may let a stage set another.
		// Defence in depth (Task 9 review minor 4): without this block Cancel falls through to Complete, whose deferral
		// (ApiDepth > 0 here) runs EndWaits and reaches the same hook -- Task 9's restore (a) passes for that reason. This
		// keeps Cancel's promise from resting on that path: a hook a stage installs that Complete's deferral would not
		// reach (a Complete that stops deferring, or a wait EndWaits does not end) is still ended here, inside Cancel().
		TUniqueFunction<bool()> Wake = MoveTemp(Ex.Interrupt);
		Ex.Interrupt.Reset();
		if (Wake())
		{
			return;
		}
		// Concurrency review M-4: nothing answered (the stage's state is gone); finish it below, as for any other holder
	}
	// Held by a custom stage, or its answer is queued for the game thread: finish now; what arrives later is ignored
	Complete(Exchange, MakeFinish(Ex));
}

void VaRest::Http::FClientState::EndWaits(FExchangeState& Ex)
{
	check(IsInGameThread());
	// M4a final review A1 (d), review A M-1: an Outer stage's answer accepted while its attempt is on the wire (a user's own
	// timeout stage). Nothing uses that call's result any more; canceled, it stops holding its limiter slot -- its Canceled
	// end passes the grant's release -- and writing a SaveTo file the decode task is about to finish. On the normal path
	// OnAttemptDone cleared the slot before OnChainDone runs, so this does nothing there.
	PipelinePrivate::CancelHeldCall(Ex);
	if (Ex.bInLimiterQueue)
	{
		// Its abort answers Canceled up the chain (FLimiterStage), and no slot was held
		Limiter.Abort(Ex.RequestId);
	}
	if (Ex.Interrupt)
	{
		// Taken first: answering may let a stage set another
		TUniqueFunction<bool()> Wake = MoveTemp(Ex.Interrupt);
		Ex.Interrupt.Reset();
		Wake();
	}
}

void VaRest::Http::FClientState::CancelAll(FName InTag, EVaRestCancelReason Reason)
{
	check(IsInGameThread());
	TArray<FVaRestExchangeRef> Targets;
	for (const TPair<uint64, FVaRestExchangeRef>& Pair : InFlight)
	{
		if (InTag.IsNone() || Pair.Value->GetTag() == InTag)
		{
			Targets.Add(Pair.Value);
		}
	}
	for (const FVaRestExchangeRef& Target : Targets)
	{
		Cancel(Target, Reason);
	}
}

namespace VaRest::Http::PipelinePrivate
{
	/** Trivially destructible: safe as a static (review I-4) */
	std::atomic<bool> bShuttingDown{ false };
}

void VaRest::Http::FClientState::CancelForShutdown()
{
	PipelinePrivate::bShuttingDown = true;
	ForEachLive([](FClientState& Client) { Client.CancelAll(NAME_None, EVaRestCancelReason::Shutdown); });
}

bool VaRest::Http::FClientState::IsShuttingDown()
{
	return PipelinePrivate::bShuttingDown.load();
}

#if WITH_DEV_AUTOMATION_TESTS
void VaRest::Http::FClientState::ResetShutdownInternal()
{
	PipelinePrivate::bShuttingDown = false;
}
#endif
