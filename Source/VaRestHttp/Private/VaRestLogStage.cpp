// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "HAL/IConsoleManager.h"
#include "Tasks/Pipe.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestRedactionPipe.h"

namespace VaRest::Http::LogStagePrivate
{
	TAutoConsoleVariable<int32> CVarLogLevel(
		TEXT("VaRest.LogLevel"),
		-1,
		TEXT("Overrides every VaRest client's log level (spec 4.5): -1 = each client's own, 0 None, 1 Basic, 2 Headers, 3 Body (Body is compiled out in Shipping)."),
		ECVF_Default);

	using FRedactorRef = TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe>;

	void LogHeaders(uint64 RequestId, const TCHAR* Direction, const FVaRestHeaders& Redacted)
	{
		for (const FVaRestHeaders::FPair& Pair : Redacted)
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s   %s: %s"), RequestId, Direction, *Pair.Key, *Pair.Value);
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

	/** On the redaction pipe */
	void WriteExcerpt(uint64 RequestId, const TCHAR* Direction, const FVaRestTraceExcerpt& Excerpt)
	{
		UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s body (%s, %lld B%s): %s"), RequestId, Direction, KindName(Excerpt.Kind), Excerpt.TotalBytes,
			Excerpt.bTruncated ? TEXT(", truncated") : TEXT(""), *Excerpt.ToText());
	}

	/** Copies at most LogBodyCap bytes here (game thread, one memcpy); redacts and writes on the redaction pipe (Inspector 7) */
	void LogBodyLater(uint64 RequestId, const TCHAR* Direction, const FRedactorRef& Redactor, FString ContentType, TConstArrayView<uint8> Bytes, int64 TotalBytes)
	{
		UE::Tasks::FPipe* Pipe = GetRedactionPipe();
		if (!Pipe)
		{
			return;
		}
		TArray<uint8> Head(Bytes.GetData(), FMath::Min(Bytes.Num(), LogBodyCap));
		Pipe->Launch(TEXT("VaRest.LogBody"), [RequestId, Direction, Redactor, Type = MoveTemp(ContentType), Prefix = MoveTemp(Head), TotalBytes]
		{
			WriteExcerpt(RequestId, Direction, Redactor->RedactBody(Type, Prefix, TotalBytes, LogBodyCap));
		});
	}

	void LogRequestBody(const FVaRestExchange& Ex, const FRedactorRef& Redactor)
	{
		const FVaRestRequestBody& Upload = Ex.Body;
		if (Upload.IsEmpty())
		{
			return;
		}
		UE::Tasks::FPipe* Pipe = GetRedactionPipe();
		if (!Pipe)
		{
			return;
		}
		const uint64 RequestId = Ex.RequestId;
		if (!Upload.FilePath.IsEmpty())
		{
			Pipe->Launch(TEXT("VaRest.LogBody"), [RequestId, Redactor, Path = Upload.FilePath, Type = Upload.ContentType, Total = Upload.ContentLength]
			{
				WriteExcerpt(RequestId, TEXT(">"), Redactor->DescribeStream(Path, Type, Total));
			});
		}
		else if (Upload.Kind == EVaRestBodyKind::Multipart)
		{
			Pipe->Launch(TEXT("VaRest.LogBody"), [RequestId, Redactor, Parts = Upload.PartDescriptors, Total = Upload.ContentLength]
			{
				WriteExcerpt(RequestId, TEXT(">"), Redactor->DescribeMultipart(Parts, Total, LogBodyCap));
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

	void LogOutcome(const FVaRestExchange& Ex, const FRedactorRef& Redactor, const FString& RedactedUrl, EVaRestLogLevel Level, double StartedAt, const FVaRestAttemptOutcome& Outcome)
	{
		const double Ms = (Ex.Config->Clock->Now() - StartedAt) * 1000.0;
		if (Outcome.Failure == EVaRestTransportFailure::Canceled)
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> canceled (%.0f ms)"), Ex.RequestId, *Ex.Verb, *RedactedUrl, Ms);
		}
		else if (Outcome.IsTransportFailure())
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %s %s (%.0f ms)"), Ex.RequestId, *Ex.Verb, *RedactedUrl, LexToString(Outcome.Failure), *Outcome.FailureDetail.ToString(), Ms);
		}
		else if (Outcome.Status >= 400)
		{
			UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu %s %s -> %d (%.0f ms, %lld B down, %lld B up)"), Ex.RequestId, *Ex.Verb, *RedactedUrl, Outcome.Status, Ms, Outcome.BytesDown, Outcome.BytesUp);
		}
		else
		{
			UE_LOG(LogVaRestHttp, Log, TEXT("#%llu %s %s -> %d (%.0f ms, %lld B down, %lld B up)"), Ex.RequestId, *Ex.Verb, *RedactedUrl, Outcome.Status, Ms, Outcome.BytesDown, Outcome.BytesUp);
		}
		if (Level >= EVaRestLogLevel::Headers && !Outcome.IsTransportFailure())
		{
			LogHeaders(Ex.RequestId, TEXT("<"), Redactor->RedactHeaders(Outcome.Headers));
		}
#if !UE_BUILD_SHIPPING
		if (Level >= EVaRestLogLevel::Body && !Outcome.IsTransportFailure())
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

	/** Spec 4.5, per attempt: after the custom PerAttempt stages, before the limiter */
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
			FVaRestExchange& Ex = *Exchange;
			const EVaRestLogLevel Level = Ex.LogLevel;
			if (Level == EVaRestLogLevel::None)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			// URLs and headers are redacted here, on the game thread; bodies only on the redaction pipe (Inspector 7)
			const FRedactorRef Redactor = Ex.Config->Redactor.ToSharedRef();
			FString Redacted = Redactor->RedactUrl(Ex.Url);
			if (Level >= EVaRestLogLevel::Headers)
			{
				UE_LOG(LogVaRestHttp, Log, TEXT("#%llu > %s %s (attempt %d)"), Ex.RequestId, *Ex.Verb, *Redacted, Ex.Attempt);
				LogHeaders(Ex.RequestId, TEXT(">"), Redactor->RedactHeaders(Ex.AttemptHeaders));
			}
#if !UE_BUILD_SHIPPING
			if (Level >= EVaRestLogLevel::Body)
			{
				// Before the transport stage, which moves the bytes into the engine request (decision 12)
				LogRequestBody(Ex, Redactor);
			}
#endif
			const double StartedAt = Ex.Config->Clock->Now();
			Next.Proceed(Exchange, [Exchange, Redactor, Line = MoveTemp(Redacted), Level, StartedAt, Done = MoveTemp(OnOutcome)](FVaRestAttemptOutcome&& Outcome) mutable
			{
				LogOutcome(*Exchange, Redactor, Line, Level, StartedAt, Outcome);
				Done(MoveTemp(Outcome));
			});
#endif
		}
	};
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
