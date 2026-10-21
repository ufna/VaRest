// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/Guid.h"
#include "VaRestClientState.h"

#include "HAL/FileManager.h"
#include "Tasks/Task.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestDecode.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"
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

	EVaRestErrorKind KindFor(EVaRestTransportFailure Failure, FName Detail)
	{
		// Review M14: a local failure is not a network one, and M4's retry reads bRetryable from the kind
		static const FName BodyRejected(TEXT("BodyRejected"));
		static const FName InvalidHeader(TEXT("InvalidHeader"));
		// Review N-4: the body could not be read while it was sent (a file gone or changed); sending it again fails again
		static const FName UploadReadFailed(TEXT("UploadReadFailed"));
		// Task 7 review M-1: a BodyFile missing or unreadable at Start; nothing was sent, and nothing would be next time
		static const FName BodyFileMissing(TEXT("BodyFileMissing"));
		static const FName FileWriteFailed(TEXT("ResponseFileWriteFailed"));
		static const FName BodyTooBig(TEXT("ResponseBodyRejected"));
		static const FName TooLarge(TEXT("ResponseTooLarge"));
		if (Detail == BodyRejected || Detail == InvalidHeader || Detail == UploadReadFailed || Detail == BodyFileMissing)
		{
			return EVaRestErrorKind::Config;
		}
		if (Detail == FileWriteFailed || Detail == BodyTooBig || Detail == TooLarge)
		{
			return EVaRestErrorKind::Decode;
		}
		switch (Failure)
		{
		case EVaRestTransportFailure::None: return EVaRestErrorKind::None;
		case EVaRestTransportFailure::Timeout: return EVaRestErrorKind::Timeout;
		case EVaRestTransportFailure::Canceled: return EVaRestErrorKind::Canceled;
		default: return EVaRestErrorKind::Connection; // Connection, Other: no usable response
		}
	}

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
	 * nothing and reports nothing.
	 */
	class FCompletionGuard
	{
	public:
		FCompletionGuard(const FVaRestExchangeRef& InExchange, FVaRestOutcomeCallback&& InOutcome)
			: Held(InExchange)
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
			OnGameThread(TEXT("VaRest.AttemptDone"), [Target = MoveTemp(Held), Callback = MoveTemp(Outcome), Moved = MoveTemp(Result)]() mutable
			{
				Internals(*Target).State->OnAttemptDone(Target.ToSharedRef(), MoveTemp(Moved), MoveTemp(Callback));
			});
		}

	private:
		TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Held;
		FVaRestOutcomeCallback Outcome;
	};

	bool IsOwnerGone(const FExchangeState& Ex)
	{
		// IsValid() is false for an object marked as garbage, not only for one collected (WeakObjectPtr.h:290)
		return Ex.bHasOwner && !Ex.Owner.IsValid();
	}

	void MarkOwnerGone(FExchangeState& Ex)
	{
		// Silence wins over an earlier User cancel (review C-2); Shutdown is silent too, and stays
		if (Ex.CancelReason != EVaRestCancelReason::Shutdown)
		{
			Ex.CancelReason = EVaRestCancelReason::OwnerGone;
		}
		Ex.bCancelRequested = true;
	}

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
		Ex.Handler->Progress(Current);
	}

	void ScheduleProgress(const FVaRestExchangeRef& Exchange)
	{
		FExchangeState& Ex = Internals(*Exchange);
		if (!Ex.TransportCall.IsValid() || Exchange->IsCancelRequested() || Ex.bFinished)
		{
			// Review M-3: the timer stops once the attempt is canceled -- an OnProgress that called Cancel() included; the
			// transport's Canceled answer is on its way, and OnAttemptDone delivers nothing for it. Task 13 review minor 5
			// (P13.5): and once the request is finished -- a stage that answered while its inner transport call still runs
			// leaves that call live, and Complete took the handler, so a tick could only re-arm itself until the call ends.
			return;
		}
		const TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Weak = Exchange;
		Ex.ProgressTimer = Ex.Config->Clock->SetTimer(ProgressInterval, [Weak]
		{
			const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Weak.Pin();
			if (!Pinned.IsValid() || !Internals(*Pinned).TransportCall.IsValid())
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
			ScheduleProgress(Pinned.ToSharedRef());
		});
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
			++Ex.Attempt;
			Ex.AttemptStartedAt = Ex.Config->Clock->Now();
			Ex.AttemptHeaders = Ex.RequestHeaders;
			Ex.LimiterWait = 0.0;
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};

	/** Spec 4.6: per attempt, right before the transport */
	class FLimiterStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Limiter"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
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
					Granted.LimiterWait = Granted.Config->Clock->Now() - EnqueuedAt;
					Next.Proceed(Exchange, [Exchange, Outcome](FVaRestAttemptOutcome&& Result)
					{
						// The slot is freed before the outcome goes further up: a stage that re-issues queues again
						Internals(*Exchange).State->GetLimiter().Release();
						(*Outcome)(MoveTemp(Result));
					});
				},
				[Exchange, Outcome, EnqueuedAt]()
				{
					FExchangeState& Aborted = Internals(*Exchange);
					Aborted.bInLimiterQueue = false;
					// Task 14 re-review N2 (P14.2): the time spent queued is the queue's, as for a grant, so the log line
					// shows "canceled (0 ms, N ms queued)" and not the wait as the attempt's own milliseconds
					Aborted.LimiterWait = Aborted.Config->Clock->Now() - EnqueuedAt;
					(*Outcome)(FVaRestAttemptOutcome::Canceled());
				});
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
			Request.Url = Ex.Url;
			Request.Headers = Ex.AttemptHeaders;
			if (!Ex.Body.FilePath.IsEmpty())
			{
				Request.BodyFile = Ex.Body.FilePath;
			}
			else if (Cfg.bMayReissue)
			{
				// A later attempt needs the body again (decision 12)
				Request.BodySegments = Ex.Body.Segments;
				Request.Body = Ex.Body.Bytes;
			}
			else
			{
				// One attempt: moved all the way into IHttpRequest::SetContent(TArray<uint8>&&) (spec 2.4)
				Request.BodySegments = MoveTemp(Ex.Body.Segments);
				Request.Body = MoveTemp(Ex.Body.Bytes);
			}
			Request.Timeout = Ex.Timeout;
			Request.ActivityTimeout = Ex.ActivityTimeout;
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
			// IN-2: on the game thread, immediately before the transport starts
			TracePoints::AttemptDispatched(Ex);
			Ex.TransportCall = Cfg.Transport->Start(MoveTemp(Request), [Guard = FCompletionGuard(Exchange, MoveTemp(OnOutcome))](FVaRestTransportResult&& Result) mutable
			{
				Guard.Deliver(MoveTemp(Result));
			});
			if (bWantsProgress)
			{
				ScheduleProgress(Exchange);
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
	if (VaRest::Http::Internals(*Exchange).bFinished || Exchange->IsCancelRequested())
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
		OutError = FString::Printf(TEXT("the base URL \"%s\" has a fragment; a base URL may carry a query but not a fragment"), *Cfg.Redactor->RedactUrl(Cfg.BaseUrl));
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
		OutError = FString::Printf(TEXT("%s (path template \"%s\")"), *Expanded.GetError(), *Cfg.Redactor->RedactUrl(Exchange.PathTemplate));
		return false;
	}
	FString Resolved = JoinUrl(Cfg.BaseUrl, bHasQuery ? Expanded.GetValue() + Exchange.RawPath.Mid(QueryAt) : Expanded.GetValue());
	if (!IsAbsoluteHttpUrl(Resolved))
	{
		OutError = FString::Printf(TEXT("\"%s\" is not an absolute http(s) URL (base URL \"%s\")"), *Cfg.Redactor->RedactUrl(Resolved), *Cfg.Redactor->RedactUrl(Cfg.BaseUrl));
		return false;
	}
	if (HasNonAsciiHost(Resolved))
	{
		// Task 3 review I2: JoinUrl encodes a non-ASCII path, but a host would need punycode (IDNA), which is out of scope,
		// and curl takes the URL through TCHAR_TO_ANSI (CurlHttp.cpp:926), so every such character would go out as '?'
		OutError = FString::Printf(TEXT("the host of \"%s\" is not ASCII; an internationalised domain name must be given in its punycode (xn--) form"),
			*Cfg.Redactor->RedactUrl(Resolved));
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
		UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: GET %s has a body; it is dropped (spec 3.2)"), Exchange.RequestId, *EscapeForLog(Cfg.Redactor->RedactUrl(Exchange.Url)));
		Exchange.Body = FVaRestRequestBody();
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

	Exchange.Timeout = Exchange.TimeoutOverride.Get(Cfg.Timeout);
	Exchange.ActivityTimeout = Exchange.ActivityTimeoutOverride.Get(Cfg.ActivityTimeout);
	Exchange.MaxJsonBytes = Exchange.MaxJsonBytesOverride.Get(Cfg.MaxJsonBytes);
	Exchange.DecodeOptions.Naming = Exchange.Naming;
	Exchange.DecodeOptions.bStrict = Exchange.bStrictDecode;
	Exchange.LogLevel = EffectiveLogLevel(Cfg.LogLevel);
	return true;
}

void VaRest::Http::FClientState::Enter(const FVaRestExchangeRef& Exchange)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
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
	InFlight.Add(Ex.RequestId, Exchange);
	if (IsShuttingDown())
	{
		// Review M-14: after OnEnginePreExit; the HTTP module unloads next (LaunchEngineLoop.cpp:5182)
		Ex.CancelReason = EVaRestCancelReason::Shutdown;
		Ex.bCancelRequested = true;
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

void VaRest::Http::FClientState::OnAttemptDone(const FVaRestExchangeRef& Exchange, FVaRestTransportResult&& Result, FVaRestOutcomeCallback&& OnOutcome)
{
	using namespace PipelinePrivate;
	check(IsInGameThread());
	FExchangeState& Ex = Internals(*Exchange);
	Ex.TransportCall.Reset();
	if (IsOwnerGone(Ex))
	{
		// Seen as canceled by every stage the outcome passes on its way up (M4's retry stops here)
		MarkOwnerGone(Ex);
	}
	if (!Ex.bFinished)
	{
		// IN-3, "when the outcome re-enters the stage chain". After Completed (a cancel finished the request while the
		// answer was queued) the record is sealed (Inspector 2.3), so a late attempt is not reported.
		TracePoints::AttemptEnded(Ex, Result);
	}
	Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
	// The final counts, so a caller sees 100% even when the transfer was quicker than one tick (decision 8)
	DeliverProgress(Ex);
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
	if (Ex.bFinished || Ex.Phase != EVaRestExchangePhase::InChain)
	{
		// Finished, or a second outcome while the first is being decoded (a stage that reported twice): ignored (review C-3).
		// A finished one's attempt file is removed (a second outcome's file is its decode task's to finish).
		if (Ex.bFinished && !Ex.TempFile.IsEmpty())
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
	static const FName Dropped(TEXT("TransportDropped"));
	if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge && Outcome.Status > 0 && !IsSuccessStatus(Outcome.Status))
	{
		// Reviews M-6, M13: an error response whose body passed the JSON cap is still the HTTP error it was; the body is gone
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FString::Printf(TEXT("%s (its body is over the MaxJsonBytes limit of %lld and was not kept)"),
			*FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status), Ex.MaxJsonBytes);
		Finish.Error.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
	}
	else if (Outcome.IsTransportFailure() && Outcome.FailureDetail == TooLarge)
	{
		// Decision 20: the transport stopped the body at the cap -- bounded memory on untrusted input
		Finish.Error.Kind = EVaRestErrorKind::Decode;
		Finish.Error.Message = FString::Printf(TEXT("the JSON response is over the MaxJsonBytes limit of %lld; the transfer stopped there"), Ex.MaxJsonBytes);
	}
	else if (Outcome.IsTransportFailure())
	{
		Finish.Error.Kind = KindFor(Outcome.Failure, Outcome.FailureDetail);
		Finish.Error.Message = FVaRestError::DefaultMessage(Finish.Error.Kind, 0);
		if (!Outcome.FailureDetail.IsNone())
		{
			Finish.Error.Message += FString::Printf(TEXT(" (%s)"), *Outcome.FailureDetail.ToString());
		}
		// A dropped completion (engine shutdown, NullHttp) would only be dropped again
		Finish.Error.bRetryable = (Finish.Error.Kind == EVaRestErrorKind::Connection || Finish.Error.Kind == EVaRestErrorKind::Timeout)
			&& Outcome.FailureDetail != Dropped;
	}
	else if (!IsSuccessStatus(Outcome.Status))
	{
		Finish.Error.Kind = EVaRestErrorKind::Http;
		Finish.Error.HttpStatus = Outcome.Status;
		Finish.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Http, Outcome.Status);
		Finish.Error.bRetryable = FVaRestError::IsRetryableStatus(Outcome.Status);
	}

	const bool bHasBody = Finish.Body.Num() > 0 || Finish.bBodyInFile;
	// A download to finish (SaveTo's success, from the transport or from a stage, review M1) or to clean up: disk IO, never
	// on the game thread
	const bool bDownload = !Finish.TempFile.IsEmpty() || (!Finish.SaveTo.IsEmpty() && !Finish.Error.IsError());
	const bool bTask = bDownload
		|| (Finish.Error.IsError() ? Finish.Error.Kind == EVaRestErrorKind::Http && bHasBody : Ex.Handler->DecodesBody() && bHasBody);
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

	Finish.bTimeDecode = FVaRestTrace::Detail() != EVaRestTraceDetail::None;
	Ex.Phase = EVaRestExchangePhase::Decoding;
	// The handler goes with the task and comes back with the result: nothing else touches it meanwhile
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	// The exchange travels as a TSharedPtr moved into the continuation, so the worker never drops the last reference
	// (review I-1; TSharedRef's move constructor copies, SharedPointer.h:354-361)
#if !UE_BUILD_SHIPPING
	++Testing::Counters().DecodeTasksLaunched;
#endif
	UE::Tasks::Launch(TEXT("VaRest.Decode"), [Held = TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe>(Exchange), Decoder = MoveTemp(Handler), Pending = MoveTemp(Finish)]() mutable
	{
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
	if (Ex.ApiDepth > 0)
	{
		// Decision 3: never inside Send() or Cancel(); the callbacks run from a later game-thread task
		OnGameThread(TEXT("VaRest.Complete"), [Exchange, Pending = MoveTemp(Finish)]() mutable
		{
			Internals(*Exchange).State->Complete(Exchange, MoveTemp(Pending));
		});
		return;
	}
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

	const FClientConfig& Cfg = *Ex.Config;
	Finish.Response.Elapsed = Cfg.Clock->Now() - Ex.StartedAt;
	Finish.Response.Attempts = Ex.Attempt;
	if (Finish.Error.IsError())
	{
		Finish.Error.Attempts = Ex.Attempt;
		Finish.Error.Tag = Ex.Tag;
		// Spec 3.3: "Url (redacted)"
		Finish.Error.Url = Cfg.Redactor->RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
		if (Finish.Error.Message.IsEmpty())
		{
			Finish.Error.Message = FVaRestError::DefaultMessage(Finish.Error.Kind, Finish.Error.HttpStatus);
		}
	}

	Ex.bDone = true;
	InFlight.Remove(Ex.RequestId);
	// IN-5: before any callback, OnHttpThread ones included, and also when none will run (owner gone, shutdown)
	TracePoints::Completed(Ex, Finish);
	TUniquePtr<IResultHandler> Handler = MoveTemp(Ex.Handler);
	// Spec 3.2: owner gone -> no callbacks; nor at engine exit (decision 2)
	const bool bSilent = Ex.CancelReason == EVaRestCancelReason::OwnerGone || Ex.CancelReason == EVaRestCancelReason::Shutdown;
	if (!Handler.IsValid() || bSilent)
	{
		return;
	}
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
		// Review M-3: no progress tick after Cancel() returns (ScheduleProgress re-arms nothing once the flag is set)
		Ex.Config->Clock->ClearTimer(Ex.ProgressTimer);
		// The transport answers Canceled through the normal path, which releases what the attempt holds
		Ex.TransportCall->Cancel();
		return;
	}
	// Held by a custom stage, or its answer is queued for the game thread: finish now; what arrives later is ignored
	Complete(Exchange, MakeFinish(Ex));
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
