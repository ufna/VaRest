// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "HAL/IConsoleManager.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpTesting.h"
#include "VaRestRedactionPipe.h"
#include "VaRestUrl.h"

namespace VaRest::Http::LogStagePrivate
{
	TAutoConsoleVariable<int32> CVarLogLevel(
		TEXT("VaRest.LogLevel"),
		-1,
		TEXT("Overrides every VaRest client's log level (spec 4.5): -1 = each client's own, 0 None, 1 Basic, 2 Headers, 3 Body (Body is compiled out in Shipping)."),
		ECVF_Default);

	using FRedactorRef = TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe>;

	bool NeedsEscape(TCHAR Char)
	{
		const uint32 Code = static_cast<uint32>(Char);
		return (Code < 0x20 && Code != '\t') || Code == '\\' || (Code >= 0x7F && Code <= 0x9F) || Code == 0x2028 || Code == 0x2029;
	}

	/** A URL as a line shows it: redacted, then escaped */
	FString ShownUrl(const FRedactorRef& Redactor, FStringView Url)
	{
		return EscapeForLog(Redactor->RedactUrl(Url));
	}

	void LogHeaders(uint64 RequestId, const TCHAR* Direction, const FVaRestHeaders& Redacted)
	{
		for (const FVaRestHeaders::FPair& Pair : Redacted)
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s   %s: %s"), RequestId, Direction, *EscapeForLog(Pair.Key), *EscapeForLog(Pair.Value));
		}
	}

#if !UE_BUILD_SHIPPING
	const TCHAR* KindName(EVaRestExcerptKind Kind)
	{
		switch (Kind)
		{
		case EVaRestExcerptKind::Json: return TEXT("json");
		case EVaRestExcerptKind::Text: return TEXT("text");
		case EVaRestExcerptKind::Form: return TEXT("form");
		case EVaRestExcerptKind::Multipart: return TEXT("multipart");
		case EVaRestExcerptKind::Binary: return TEXT("binary");
		case EVaRestExcerptKind::Stream: return TEXT("stream");
		default: return TEXT("skipped");
		}
	}

	/** Game thread: what body logging copied there (Inspector 7: "at most ... up to the cap"); tests read it (review M7) */
	void CountCopied(int64 Bytes)
	{
		Testing::Counters().LogBodyBytesCopied += Bytes;
	}

	/** On the redaction pipe. A multipart excerpt is one line per part, and a text body may hold any byte: escaped, so it stays one line. */
	void WriteExcerpt(uint64 RequestId, const TCHAR* Direction, const FVaRestTraceExcerpt& Excerpt)
	{
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s body (%s, %lld B%s): %s"), RequestId, Direction, KindName(Excerpt.Kind), Excerpt.TotalBytes,
			Excerpt.bTruncated ? TEXT(", truncated") : TEXT(""), *EscapeForLog(Excerpt.ToText()));
	}

	/** Copies at most LogBodyCap bytes here (game thread, one memcpy); redacts and writes on the redaction pipe (Inspector 7) */
	void LogBodyLater(uint64 RequestId, const TCHAR* Direction, const FRedactorRef& Redactor, FString ContentType, TConstArrayView<uint8> Bytes, int64 TotalBytes)
	{
		TArray<uint8> Head(Bytes.GetData(), FMath::Min(Bytes.Num(), LogBodyCap));
		CountCopied(Head.Num());
		LaunchRedaction(TEXT("VaRest.LogBody"), [RequestId, Direction, Redactor, Type = MoveTemp(ContentType), Prefix = MoveTemp(Head), TotalBytes]
		{
			WriteExcerpt(RequestId, Direction, Redactor->RedactBody(Type, Prefix, TotalBytes, LogBodyCap));
		});
	}

	/**
	 * Review M3: the multipart descriptors with their field bytes cut to what DescribeMultipart can read (the cap plus the
	 * longest needle), all parts together. A cut field keeps its Size: that is how DescribeMultipart knows to drop a known
	 * secret the cut split in two.
	 */
	TArray<FVaRestMultipartPart> CopyPartsForLog(TConstArrayView<FVaRestMultipartPart> Parts, const FVaRestRedactor& Redactor)
	{
		int64 Budget = static_cast<int64>(LogBodyCap) + Redactor.GetMaxNeedleBytes();
		TArray<FVaRestMultipartPart> Copies;
		Copies.Reserve(Parts.Num());
		for (const FVaRestMultipartPart& Part : Parts)
		{
			FVaRestMultipartPart& Copied = Copies.AddDefaulted_GetRef();
			Copied.Name = Part.Name;
			Copied.FileName = Part.FileName;
			Copied.ContentType = Part.ContentType;
			Copied.FilePath = Part.FilePath;
			Copied.Size = Part.Size;
			Copied.bField = Part.bField;
			const int32 Take = static_cast<int32>(FMath::Min<int64>(Part.Bytes.Num(), Budget));
			Copied.Bytes.Append(Part.Bytes.GetData(), Take);
			Budget -= Take;
			CountCopied(Take);
		}
		return Copies;
	}

	void LogRequestBody(const FExchangeState& Ex, const FRedactorRef& Redactor)
	{
		const FVaRestRequestBody& Upload = Ex.Body;
		if (Upload.IsEmpty())
		{
			return;
		}
		const uint64 RequestId = Ex.RequestId;
		if (!Upload.FilePath.IsEmpty())
		{
			LaunchRedaction(TEXT("VaRest.LogBody"), [RequestId, Redactor, Path = Upload.FilePath, Type = Upload.ContentType, Total = Upload.ContentLength]
			{
				WriteExcerpt(RequestId, TEXT(">"), Redactor->DescribeStream(Path, Type, Total));
			});
		}
		else if (Upload.Kind == EVaRestBodyKind::Multipart)
		{
			LaunchRedaction(TEXT("VaRest.LogBody"), [RequestId, Redactor, Capped = CopyPartsForLog(Upload.PartDescriptors, *Redactor), Total = Upload.ContentLength]
			{
				WriteExcerpt(RequestId, TEXT(">"), Redactor->DescribeMultipart(Capped, Total, LogBodyCap));
			});
		}
		else
		{
			// The final Content-Type: a PerAttempt stage may have changed it, and a request's own header wins (spec 3.2)
			const FString* TypeHeader = Ex.AttemptHeaders.Find(TEXT("Content-Type"));
			LogBodyLater(RequestId, TEXT(">"), Redactor, TypeHeader ? *TypeHeader : Upload.ContentType, Upload.Bytes, Upload.ContentLength);
		}
	}
#endif

	void LogOutcome(const FExchangeState& Ex, EVaRestLogLevel Level, double StartedAt, const FVaRestAttemptOutcome& Outcome)
	{
		// Plan decision 17, review I-1: read when the outcome is back -- a secret set while the attempt ran (an auth refresh)
		// is known, and one the client forgot meanwhile (a logout) still is, from the Send() snapshot
		const FRedactorRef Redactor = RedactorFor(Ex);
		// Review M5: the attempt's own time. StartedAt is taken after the limiter's grant (ruling I-3: the limiter opens the
		// attempt), so the wait in the queue (IN-2's LimiterWait, on this exchange's clock) is not in it; it is shown apart,
		// only when it rounds to a millisecond or more
		const double QueuedMs = Ex.LimiterWait * 1000.0;
		const double Ms = FMath::Max(0.0, (Ex.Config->Clock->Now() - StartedAt) * 1000.0);
		const FString Queued = QueuedMs >= 0.5 ? FString::Printf(TEXT(", %.0f ms queued"), QueuedMs) : FString();
		const bool bCanceled = Outcome.Failure == EVaRestTransportFailure::Canceled;
		const bool bWarning = Outcome.IsTransportFailure() ? !bCanceled : Outcome.Status >= 400;
		// Review M2: a line the category drops is not built; a Warning line can be active when Log is not
		if (bWarning ? UE_LOG_ACTIVE(LogVaRestHttp, Warning) : UE_LOG_ACTIVE(LogVaRestHttp, Log))
		{
			// Built here, only for a line that is written, with this redactor: not the request line's
			// The URL the attempt went to: an auth's query pair is redacted by its key (plan decision 19)
			const FString Url = ShownUrl(Redactor, Ex.AttemptUrl);
			const FString ShownVerb = EscapeForLog(Ex.Verb);
			if (bCanceled)
			{
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> canceled (%.0f ms%s)"), Ex.RequestId, *ShownVerb, *Url, Ms, *Queued);
			}
			else if (Outcome.IsTransportFailure())
			{
				UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %s %s (%.0f ms%s)"), Ex.RequestId, *ShownVerb, *Url, LexToString(Outcome.Failure),
					*EscapeForLog(Outcome.FailureDetail.ToString()), Ms, *Queued);
			}
			else if (Outcome.Status >= 400)
			{
				UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %d (%.0f ms%s, %lld B down, %lld B up)"), Ex.RequestId, *ShownVerb, *Url, Outcome.Status, Ms, *Queued,
					Outcome.BytesDown, Outcome.BytesUp);
			}
			else
			{
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> %d (%.0f ms%s, %lld B down, %lld B up)"), Ex.RequestId, *ShownVerb, *Url, Outcome.Status, Ms, *Queued,
					Outcome.BytesDown, Outcome.BytesUp);
			}
		}
		if (Outcome.IsTransportFailure() || !UE_LOG_ACTIVE(LogVaRestHttp, Log))
		{
			return;
		}
		if (Level >= EVaRestLogLevel::Headers)
		{
			LogHeaders(Ex.RequestId, TEXT("<"), Redactor->RedactHeaders(Outcome.Headers));
		}
#if !UE_BUILD_SHIPPING
		if (Level >= EVaRestLogLevel::Body)
		{
			if (Outcome.bBodyInFile)
			{
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu < body (%lld B): written to the SaveTo file"), Ex.RequestId, Outcome.BytesDown);
			}
			else if (Outcome.Body.Num() > 0)
			{
				LogBodyLater(Ex.RequestId, TEXT("<"), Redactor, Outcome.ContentType, Outcome.Body, Outcome.Body.Num());
			}
		}
#endif
	}

	/** Spec 4.5, per attempt: after the custom PerAttempt stages, right before the transport */
	class FLogStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("Log"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
#if NO_LOGGING
			// UE_LOG writes nothing here (LogMacros.h:184-194): nothing to redact a URL or a header for
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
#else
			FExchangeState& Ex = Internals(*Exchange);
			const EVaRestLogLevel Level = Ex.LogLevel;
			// Review M2: with the category below Log (-LogCmds="LogVaRestHttp Warning"), the header and body lines are dropped,
			// so nothing is redacted or copied for them. UE_LOG_ACTIVE tests the compiled-in and the runtime verbosity
			// (LogMacros.h:242-262), as UE_LOG itself does (:355).
			const bool bLogActive = UE_LOG_ACTIVE(LogVaRestHttp, Log);
			if (Level == EVaRestLogLevel::None || (!bLogActive && !UE_LOG_ACTIVE(LogVaRestHttp, Warning)))
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			// URLs and headers are redacted here, on the game thread; bodies only on the redaction pipe (Inspector 7). Every
			// value is escaped after it is redacted (review C1).
			const FRedactorRef Redactor = RedactorFor(Ex);
			if (bLogActive && Level >= EVaRestLogLevel::Headers)
			{
				const FString Shown = ShownUrl(Redactor, Ex.AttemptUrl);
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu > %s %s (attempt %d)"), Ex.RequestId, *EscapeForLog(Ex.Verb), *Shown, Ex.Attempt);
				LogHeaders(Ex.RequestId, TEXT(">"), Redactor->RedactHeaders(Ex.AttemptHeaders));
			}
#if !UE_BUILD_SHIPPING
			if (bLogActive && Level >= EVaRestLogLevel::Body)
			{
				// Before the transport stage, which moves the bytes into the engine request (decision 12)
				LogRequestBody(Ex, Redactor);
			}
#endif
			const double StartedAt = Ex.Config->Clock->Now();
			Next.Proceed(Exchange, [Exchange, Level, StartedAt, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				LogOutcome(Internals(*Exchange), Level, StartedAt, Outcome);
				Done(MoveTemp(Outcome));
			});
#endif
		}
	};
}

namespace VaRest::Http::LogStagePrivate
{
	FString Escape(FStringView Text, bool bBackslash)
	{
		const auto Needs = [bBackslash](TCHAR Char) { return NeedsEscape(Char) && (bBackslash || Char != TEXT('\\')); };
		int32 First = 0;
		while (First < Text.Len() && !Needs(Text[First]))
		{
			++First;
		}
		if (First == Text.Len())
		{
			return FString(Text);
		}
		FString Out;
		Out.Reserve(Text.Len() + 16);
		Out.Append(Text.GetData(), First);
		for (int32 i = First; i < Text.Len(); ++i)
		{
			const TCHAR Char = Text[i];
			if (!Needs(Char))
			{
				Out.AppendChar(Char);
			}
			else if (Char == TEXT('\\'))
			{
				Out += TEXT("\\\\");
			}
			else if (Char == TEXT('\r'))
			{
				Out += TEXT("\\r");
			}
			else if (Char == TEXT('\n'))
			{
				Out += TEXT("\\n");
			}
			else if (static_cast<uint32>(Char) <= 0xFF)
			{
				Out.Appendf(TEXT("\\x%02X"), static_cast<uint32>(Char));
			}
			else
			{
				Out.Appendf(TEXT("\\u%04X"), static_cast<uint32>(Char));
			}
		}
		return Out;
	}
}

FString VaRest::Http::EscapeForLog(FStringView Text)
{
	return LogStagePrivate::Escape(Text, /*bBackslash*/ true);
}

FString VaRest::Http::EscapeControlsForLog(FStringView Text)
{
	return LogStagePrivate::Escape(Text, /*bBackslash*/ false);
}

void VaRest::Http::LogConfigError(const FExchangeState& Ex, FStringView Message)
{
	// Task 14 fix round 2: a builder error or a URL Resolve refused (spec 3.3, Kind=Config) never enters the chain, so the
	// log stage never sees it. A Warning, as every failure but a cancel; the URL as the error carries it, redacted, then escaped.
	const FClientConfig& Cfg = *Ex.Config;
	if (EffectiveLogLevel(Cfg.LogLevel) == EVaRestLogLevel::None || !UE_LOG_ACTIVE(LogVaRestHttp, Warning))
	{
		return;
	}
	const FString Url = LogStagePrivate::ShownUrl(RedactorFor(Ex), Ex.Url.IsEmpty() ? JoinUrl(Cfg.BaseUrl, Ex.RawPath) : Ex.Url);
	UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> Config error: %s"), Ex.RequestId, *EscapeForLog(Ex.Verb), *Url, *EscapeControlsForLog(Message));
}

void VaRest::Http::LogEndedInQueue(const FExchangeState& Ex, const FVaRestAttemptOutcome& Outcome)
{
	// Ruling I-3: an attempt the limiter ends (a cancel, engine exit, the retry deadline) never reaches the log stage. Its
	// Basic line is written as that stage would have: the attempt's own 0 ms, the wait apart. The same gate as FLogStage.
	if (Ex.LogLevel == EVaRestLogLevel::None || !UE_LOG_ACTIVE(LogVaRestHttp, Warning))
	{
		return;
	}
	LogStagePrivate::LogOutcome(Ex, Ex.LogLevel, Ex.Config->Clock->Now(), Outcome);
}

void VaRest::Http::LogEndedInPark(const FExchangeState& Ex, const FVaRestAttemptOutcome& Outcome)
{
	// Ruling I-3 as spec review I-1 extends it (Task 17 review I-1): a park behind a session refresh before an attempt is
	// that attempt's Auth phase -- it started when it parked (PendingAttemptStart) -- and an attempt the park ends (a
	// rejected or failed refresh, a logout, the deadline, a cancel, the owner gone) never reaches the log stage. Its Basic
	// line is written as that stage would have, with the same gate as FLogStage: the attempt's own 0 ms, the wait behind
	// the refresh apart. Not LogOutcome: the boundary has not run, so AttemptUrl is not made yet and LimiterWait is an
	// earlier attempt's; the URL is the request's, as the error carries it, redacted, then escaped.
	if (Ex.LogLevel == EVaRestLogLevel::None || !UE_LOG_ACTIVE(LogVaRestHttp, Warning))
	{
		return;
	}
	const bool bCanceled = Outcome.Failure == EVaRestTransportFailure::Canceled;
	if (bCanceled && !UE_LOG_ACTIVE(LogVaRestHttp, Log))
	{
		return;
	}
	const double WaitMs = Ex.PendingAttemptStart >= 0.0 ? FMath::Max(0.0, (Ex.Config->Clock->Now() - Ex.PendingAttemptStart) * 1000.0) : 0.0;
	const FString Url = LogStagePrivate::ShownUrl(RedactorFor(Ex), Ex.Url);
	const FString ShownVerb = EscapeForLog(Ex.Verb);
	if (bCanceled)
	{
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> canceled (0 ms, %.0f ms auth wait)"), Ex.RequestId, *ShownVerb, *Url, WaitMs);
	}
	else
	{
		UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %s %s (0 ms, %.0f ms auth wait)"), Ex.RequestId, *ShownVerb, *Url, LexToString(Outcome.Failure),
			*EscapeForLog(Outcome.FailureDetail.ToString()), WaitMs);
	}
}

FVaRestStageRef VaRest::Http::MakeLogStage()
{
	return MakeShared<LogStagePrivate::FLogStage, ESPMode::ThreadSafe>();
}

EVaRestLogLevel VaRest::Http::EffectiveLogLevel(EVaRestLogLevel ClientLevel)
{
	const int32 Forced = LogStagePrivate::CVarLogLevel.GetValueOnAnyThread();
	return Forced < 0 ? ClientLevel : static_cast<EVaRestLogLevel>(FMath::Clamp(Forced, 0, 3));
}
