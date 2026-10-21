// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTransport.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#include "VaRestBodySink.h"
#include "VaRestHttpTransportInternal.h"
#include "VaRestTrace.h"

namespace VaRest::Http::HttpTransportPrivate
{
	EVaRestTransportFailure MapFailure(EHttpFailureReason Reason)
	{
		switch (Reason)
		{
		case EHttpFailureReason::None:
			return EVaRestTransportFailure::None;
		case EHttpFailureReason::ConnectionError:
			// Includes curl's CURLE_OPERATION_TIMEDOUT (CurlHttp.cpp:1320-1329) and an engine activity timeout
			// (HttpRequestCommon.cpp:303-305): decision 19
			return EVaRestTransportFailure::Connection;
		case EHttpFailureReason::Cancelled:
			return EVaRestTransportFailure::Canceled;
		case EHttpFailureReason::TimedOut:
			// The total timeout (HttpRequestCommon.cpp:299-301)
			return EVaRestTransportFailure::Timeout;
		default:
			return EVaRestTransportFailure::Other;
		}
	}

	EHttpRequestPriority MapPriority(EVaRestPriority Priority)
	{
		switch (Priority)
		{
		case EVaRestPriority::Highest: return EHttpRequestPriority::Highest;
		case EVaRestPriority::High: return EHttpRequestPriority::High;
		case EVaRestPriority::Low: return EHttpRequestPriority::Low;
		default: return EHttpRequestPriority::Normal;
		}
	}

	/** Two writers can report BytesSent (the counting archive and the engine's progress delegate): keep the larger */
	void AtomicMax(std::atomic<int64>& Target, int64 Value)
	{
		int64 Seen = Target.load();
		while (Value > Seen && !Target.compare_exchange_weak(Seen, Value))
		{
		}
	}

	/** Shared by the engine delegates of one request; completes exactly once */
	struct FCompletion
	{
		FCompletion(FVaRestTransportComplete&& InOnComplete, TSharedRef<FBodySink, ESPMode::ThreadSafe> InSink,
			TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> InProbe, TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> InClock)
			: OnComplete(MoveTemp(InOnComplete))
			, Sink(MoveTemp(InSink))
			, Probe(MoveTemp(InProbe))
			, Clock(MoveTemp(InClock))
		{
		}

		/**
		 * Review I-3: the engine does not always call back. FHttpManager::Shutdown calls Request->Shutdown() on every
		 * outstanding request (HttpManager.cpp:164-169), which unbinds the delegates (HttpRequestImpl.cpp:32-38); with [HTTP]
		 * bUseNullHttp, CreateRequest returns a request that never completes (HttpModule.cpp:377-380, NullHttp.cpp:142-149).
		 * Either way this object dies with the request: finish it. At shutdown the posted game-thread task never runs, which
		 * is harmless.
		 */
		~FCompletion()
		{
			if (!bDone.exchange(true) && OnComplete)
			{
				FVaRestTransportResult Dropped;
				Dropped.Failure = EVaRestTransportFailure::Other;
				Dropped.FailureDetail = TEXT("TransportDropped");
				OnComplete(MoveTemp(Dropped)); // only posts a game-thread task
			}
		}

		double Now() const
		{
			return Clock.IsValid() ? Clock->Now() : FPlatformTime::Seconds();
		}

		FVaRestTransportComplete OnComplete;
		TSharedRef<FBodySink, ESPMode::ThreadSafe> Sink;
		TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
		std::atomic<bool> bDone{ false };
		/** The response named a Content-Encoding (set by the header delegate, read by the first body chunk; review M-16) */
		std::atomic<bool> bEncodedBody{ false };
		/** Stamp transport times (trace metadata, IN-11) */
		bool bStamp = false;
		/**
		 * Set when the body goes through FCountingUpload: BytesUp is then what was sent, not what was meant (review M-18),
		 * and a failed read fails the request (review N-4). The engine's payload holds it too; it holds no completion.
		 */
		TSharedPtr<FCountingUpload, ESPMode::ThreadSafe> CountedUpload;
		/**
		 * Set when multipart segments go to the engine as they are (not counted): the engine ignores the stream's error flag
		 * (FillOutputBuffer, HttpRequestPayload.cpp:77-97), so a file part that could not be read -- FVaRestSegmentStream
		 * sends zeroes and sets the flag -- fails the request here like a counted one (Task 3 review M4).
		 */
		TSharedPtr<FVaRestSegmentStream, ESPMode::ThreadSafe> SegmentUpload;
		int64 BytesUp = 0;
	};

	class FCall final : public IVaRestTransportCall
	{
	public:
		FCall(TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> InRequest, TWeakPtr<FCompletion, ESPMode::ThreadSafe> InCompletion)
			: Request(MoveTemp(InRequest))
			, Completion(MoveTemp(InCompletion))
		{
		}

		virtual void Cancel() override
		{
			check(IsInGameThread());
			// Review M-5: a request already finished on the HTTP thread is left alone. For one that ended by an abort
			// (a timeout), curl's AbortRequest would run FinishRequestNotInHttpManager a second time (CurlHttp.cpp:1194-1209,
			// HttpRequestCommon.cpp:224-237): the engine's finish, stats and logs twice.
			const TSharedPtr<FCompletion, ESPMode::ThreadSafe> Done = Completion.Pin();
			if (!Done.IsValid() || Done->bDone.load())
			{
				return;
			}
			// The completion delegate then reports EHttpFailureReason::Cancelled (HttpRequestCommon.cpp:295-298)
			if (TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pinned = Request.Pin())
			{
				Pinned->CancelRequest();
			}
		}

	private:
		TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> Request;
		TWeakPtr<FCompletion, ESPMode::ThreadSafe> Completion;
	};

	class FHttpModuleTransport final : public IVaRestTransport
	{
	public:
		virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) override;
	};

	TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> FHttpModuleTransport::Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete)
	{
		check(IsInGameThread());
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = FHttpModule::Get().CreateRequest();
		Http->SetVerb(Request.Verb);
		Http->SetURL(Request.Url);
		for (const FVaRestHeaders::FPair& Pair : Request.Headers)
		{
			// SetHeader replaces; a repeated name is joined with ", " by AppendToHeader (IHttpRequest.h:299-310)
			if (Http->GetHeader(Pair.Key).IsEmpty())
			{
				Http->SetHeader(Pair.Key, Pair.Value);
			}
			else
			{
				Http->AppendToHeader(Pair.Key, Pair.Value);
			}
		}

		const TSharedRef<FBodySink, ESPMode::ThreadSafe> Sink = Request.ResponseFile.IsEmpty()
			? MakeShared<FBodySink, ESPMode::ThreadSafe>(Request.MaxBodyBytes)
			: MakeShared<FBodySink, ESPMode::ThreadSafe>(Request.ResponseFile, Request.MaxBodyBytes);
		const TSharedRef<FCompletion, ESPMode::ThreadSafe> Completion = MakeShared<FCompletion, ESPMode::ThreadSafe>(MoveTemp(OnComplete), Sink, Request.Probe, Request.Clock);
		Completion->bStamp = Request.bReportStatusTime;
		Completion->BytesUp = Request.Probe->UploadTotal.load();

		bool bBodyAccepted = true;
		const int64 UploadTotal = Request.Probe->UploadTotal.load();
		if (ShouldCountUpload(UploadTotal, Request.bProgressCallback, Request.bReportProgress, CurlMayRewindBodies()))
		{
			// Decision 29: counted as curl reads it (FCountingUpload), on curl only and only when asked (reviews N-1, N-9).
			// The default path stays SetContent(TArray&&).
			TSharedPtr<FCountingUpload, ESPMode::ThreadSafe> Counted;
			if (!Request.BodyFile.IsEmpty())
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MoveTemp(Request.BodyFile), UploadTotal, Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			else if (Request.BodySegments.Num() > 0)
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Request.BodySegments)),
					Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			else
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MoveTemp(Request.Body), Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			Completion->CountedUpload = Counted;
			bBodyAccepted = Http->SetContentFromStream(Counted.ToSharedRef());
		}
		else if (!Request.BodyFile.IsEmpty())
		{
			bBodyAccepted = Http->SetContentAsStreamedFile(Request.BodyFile);
		}
		else if (Request.BodySegments.Num() > 0)
		{
			// Serialize runs on the HTTP thread (IHttpRequest.h:250-257); the stream owns its segments. The completion keeps
			// it too, to see its error flag, which the engine ignores (Task 3 review M4)
			const TSharedRef<FVaRestSegmentStream, ESPMode::ThreadSafe> SegmentStream = MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Request.BodySegments));
			Completion->SegmentUpload = SegmentStream;
			bBodyAccepted = Http->SetContentFromStream(SegmentStream);
		}
		else if (Request.Body.Num() > 0)
		{
			// Spec 2.4: the body produced in one pass is moved, not copied (IHttpRequest.h:225-233)
			Http->SetContent(MoveTemp(Request.Body));
		}
		if (!bBodyAccepted)
		{
			Completion->bDone = true;
			FVaRestTransportResult Result;
			Result.Failure = EVaRestTransportFailure::Other;
			Result.FailureDetail = TEXT("BodyRejected");
			FVaRestTransportComplete Callback = MoveTemp(Completion->OnComplete);
			Callback(MoveTemp(Result));
			return MakeShared<FCall, ESPMode::ThreadSafe>(TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>(Http), Completion);
		}
		if (Request.Timeout > 0.f)
		{
			Http->SetTimeout(Request.Timeout);
		}
		const float Activity = EffectiveActivityTimeout(Request.Timeout, Request.ActivityTimeout);
		if (Activity > 0.f)
		{
			Http->SetActivityTimeout(Activity);
		}
		Http->SetPriority(MapPriority(Request.Priority));
		// Spec 1: complete on the HTTP thread; the completion below only moves bytes and dispatches
		Http->SetDelegateThreadPolicy(EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);

		// Review M-16: the response's own headers cannot be read while it is in flight -- GetHeader logs a Warning and
		// returns "" until OnFinishRequest sets bIsReady (HttpResponseCommon.cpp:76-92, HttpRequestCommon.cpp:870-873) --
		// so a Content-Encoding is noted as it arrives. Under CompleteOnHttpThread curl calls this from its header callback on
		// the HTTP thread, before that response's first body byte (CurlHttp.cpp:532-535); Apple from didReceiveResponse, on
		// the session's serial delegate queue that later delivers the body (AppleHttp.cpp:171-187, :865-874;
		// HttpRequestImpl.cpp:40-60; delegateQueue nil, ApplePlatformHttp.cpp:265). Not an IN-11 bind: the sink needs it
		// whether or not anything is traced, so DelegateBinds does not count it.
		Http->OnHeaderReceived().BindLambda([Completion](FHttpRequestPtr, const FString& HeaderName, const FString& HeaderValue)
		{
			if (!HeaderValue.IsEmpty() && HeaderName.Equals(TEXT("Content-Encoding"), ESearchCase::IgnoreCase))
			{
				Completion->bEncodedBody = true;
			}
		});

		const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakHttp = Http;
		Http->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Completion, WeakHttp](void* Ptr, int64& InOutLength)
		{
			int64 ContentLength = -1;
			if (Completion->Sink->Num() == 0)
			{
				// The first chunk: the headers are in (curl creates the response before passing body bytes, CurlHttp.cpp:584)
				if (const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> Pinned = WeakHttp.Pin())
				{
					if (const FHttpResponsePtr Response = Pinned->GetResponse())
					{
						// Review M-16: curl decompresses (CURLOPT_ACCEPT_ENCODING "", CurlHttp.cpp:1016-1021), so with a
						// Content-Encoding the Content-Length counts the compressed bytes: the received total is unknown.
						// GetContentLength has no readiness check (curl: the parsed header, CurlHttp.cpp:513-517, :1422-1425).
						const bool bEncoded = Completion->bEncodedBody.load();
						ContentLength = !bEncoded && Response->GetContentLength() > 0 ? static_cast<int64>(Response->GetContentLength()) : -1;
					}
				}
				Completion->Probe->DownloadTotal = ContentLength;
			}
			if (!Completion->Sink->Write(Ptr, InOutLength, ContentLength))
			{
				// A short count fails the request (IHttpRequest.cpp:36-44 sets the archive error; IHttpRequest.h:274-275)
				InOutLength = 0;
			}
		}));

		if (Request.bReportProgress)
		{
			++FVaRestTrace::Counters().DelegateBinds;
			Http->OnRequestProgress64().BindLambda([Completion](FHttpRequestPtr, uint64 Sent, uint64 Received)
			{
				FVaRestTransportProbe& Probe = *Completion->Probe;
				AtomicMax(Probe.BytesSent, static_cast<int64>(Sent));
				Probe.BytesReceived = static_cast<int64>(Received);
				if (Completion->bStamp && Probe.UploadDoneAt.load() < 0.0 && static_cast<int64>(Sent) >= Probe.UploadTotal.load())
				{
					Probe.UploadDoneAt = Completion->Now();
				}
			});
		}
		if (Request.bReportStatusTime)
		{
			++FVaRestTrace::Counters().DelegateBinds;
			Http->OnStatusCodeReceived().BindLambda([Completion](FHttpRequestPtr, int32)
			{
				if (Completion->Probe->StatusAt.load() < 0.0)
				{
					Completion->Probe->StatusAt = Completion->Now();
				}
			});
		}

		Http->OnProcessRequestComplete().BindLambda([Completion](FHttpRequestPtr Req, FHttpResponsePtr Resp, bool bSucceeded)
		{
			// The HTTP thread. Read what the engine holds, take the body from our sink, hand the result on: no parsing here.
			if (Completion->bDone.exchange(true))
			{
				return;
			}
			FVaRestTransportResult Result;
			if (Resp.IsValid())
			{
				Result.Status = Resp->GetResponseCode();
				Result.Headers = FVaRestHeaders::FromLines(Resp->GetAllHeaders());
				Result.EffectiveUrl = Resp->GetEffectiveURL();
				Result.ContentType = Resp->GetContentType();
			}
			// "Received", not "Sink": a lambda local may not hide an enclosing local (MSVC C4456 is an error here, review I2)
			FBodySink& Received = *Completion->Sink;
			if (Resp.IsValid() && Received.Num() == 0 && Resp->GetContent().Num() > 0)
			{
				// Review M-15: engine mock responses append to the engine's payload, not to the stream
				// (HttpRequestCommon.cpp:124-160); take it, through the sink, so the slack and the cap still hold
				const TArray<uint8>& Engine = Resp->GetContent();
				Received.Write(Engine.GetData(), Engine.Num(), Engine.Num());
			}
			if (!bSucceeded || !Resp.IsValid())
			{
				const EHttpFailureReason Reason = Req.IsValid() ? Req->GetFailureReason() : EHttpFailureReason::Other;
				Result.Failure = MapFailure(Reason);
				if (Result.Failure == EVaRestTransportFailure::None)
				{
					Result.Failure = EVaRestTransportFailure::Other;
				}
				Result.FailureDetail = FName(LexToString(Reason));
			}
			if (Received.IsFile())
			{
				Result.bBodyInFile = true;
				if (!Received.CloseFile() && Result.Failure == EVaRestTransportFailure::None)
				{
					Result.Failure = EVaRestTransportFailure::Other;
					Result.FailureDetail = TEXT("ResponseFileWriteFailed");
				}
			}
			else
			{
				Result.Body = Received.TakeBytes();
			}
			if (Received.IsTooLarge())
			{
				// Review M13: the sink stopped the transfer at the result's JSON cap; whatever the engine then reported
				Result.Failure = EVaRestTransportFailure::Other;
				Result.FailureDetail = TEXT("ResponseTooLarge");
				Result.Body.Empty();
			}
			else if (Received.HasError() && Result.Failure == EVaRestTransportFailure::None)
			{
				Result.Failure = EVaRestTransportFailure::Other;
				Result.FailureDetail = TEXT("ResponseBodyRejected");
			}
			// Review N-4, Task 3 review M4: the engine sent zeroes where the body could not be read (FillOutputBuffer ignores
			// the archive's error, HttpRequestPayload.cpp:86-96): whatever the server answered, this request did not send its
			// body. On curl the archive is read on this thread, from curl's read callback (CurlHttp.cpp:668-683), before
			// the finish that calls this (HttpThread.cpp:452-461). On Apple NSURLSession reads it on a worker thread
			// (AppleHttp.cpp:402-421) and completes the task only after the body is sent.
			const bool bCountedReadFailed = Completion->CountedUpload.IsValid() && Completion->CountedUpload->HasReadFailed();
			const bool bSegmentReadFailed = Completion->SegmentUpload.IsValid() && Completion->SegmentUpload->IsError();
			if (bCountedReadFailed || bSegmentReadFailed)
			{
				Result.Failure = EVaRestTransportFailure::Other;
				Result.FailureDetail = TEXT("UploadReadFailed");
				Result.Body.Empty();
			}
			// Review M-18: with a counted upload, what was sent; otherwise the body's size
			Result.BytesUp = Completion->CountedUpload.IsValid() ? Completion->Probe->BytesSent.load() : Completion->BytesUp;
			Result.BytesDown = Received.Num();
			if (Completion->bStamp)
			{
				Result.TransportDoneAt = Completion->Now();
			}
			FVaRestTransportComplete Callback = MoveTemp(Completion->OnComplete);
			Callback(MoveTemp(Result));
		});

		// The completion delegate runs even when ProcessRequest refuses to start ("always called when the request completes
		// or on error", IHttpRequest.h:351): the refusal arrives there as a failure, so the return value adds nothing.
		// When the engine drops it instead, ~FCompletion finishes the request (review I-3).
		Http->ProcessRequest();
		return MakeShared<FCall, ESPMode::ThreadSafe>(WeakHttp, Completion);
	}
}

float VaRest::Http::EffectiveActivityTimeout(float TotalSeconds, float ActivitySeconds)
{
	return ActivitySeconds > 0.f ? ActivitySeconds : FMath::Max(TotalSeconds, 0.f);
}

bool VaRest::Http::ShouldCountUpload(int64 UploadTotal, bool bProgressCallback, bool bReportProgress, bool bCurlMayRewind)
{
#if PLATFORM_APPLE
	// Review N-1: NSURLSession reports upload progress itself and cannot resend a body stream
	return false;
#else
	if (UploadTotal <= 0)
	{
		return false;
	}
	// Review N-9: the trace must never change an outcome -- metadata alone counts only a body curl could not rewind anyway
	return bProgressCallback || (bReportProgress && !bCurlMayRewind);
#endif
}

bool VaRest::Http::CurlMayRewindBodies()
{
	bool bAllowSeek = false;
	GConfig->GetBool(TEXT("HTTP.Curl"), TEXT("bAllowSeekFunction"), bAllowSeek, GEngineIni);
	return bAllowSeek;
}

VaRest::Http::FCountingUpload::FCountingUpload(TArray<uint8>&& InBytes, FProbeRef InProbe, FClockPtr InClock, bool bInStamp)
	: Bytes(MoveTemp(InBytes))
	, Total(Bytes.Num())
	, Probe(MoveTemp(InProbe))
	, Clock(MoveTemp(InClock))
	, bStamp(bInStamp)
{
	SetIsLoading(true);
}

VaRest::Http::FCountingUpload::FCountingUpload(TSharedRef<FArchive, ESPMode::ThreadSafe> InInner, FProbeRef InProbe, FClockPtr InClock, bool bInStamp)
	: Inner(MoveTemp(InInner))
	, Total(Inner->TotalSize()) // Inner is declared before Total, so it is set here (review N-4: one size for both)
	, Probe(MoveTemp(InProbe))
	, Clock(MoveTemp(InClock))
	, bStamp(bInStamp)
{
	SetIsLoading(true);
}

VaRest::Http::FCountingUpload::FCountingUpload(FString InFilePath, int64 InTotal, FProbeRef InProbe, FClockPtr InClock, bool bInStamp)
	: FilePath(MoveTemp(InFilePath))
	, Total(InTotal)
	, Probe(MoveTemp(InProbe))
	, Clock(MoveTemp(InClock))
	, bStamp(bInStamp)
{
	SetIsLoading(true);
}

VaRest::Http::FCountingUpload::~FCountingUpload() = default;

void VaRest::Http::FCountingUpload::Serialize(void* Data, int64 Length)
{
	if (Length <= 0)
	{
		return;
	}
	if (bReadFailed.load() || Position + Length > Total)
	{
		FailRead(Data, Length);
		return;
	}
	if (Inner.IsValid())
	{
		Inner->Seek(Position);
		Inner->Serialize(Data, Length);
		if (Inner->IsError())
		{
			FailRead(Data, Length);
			return;
		}
	}
	else if (!FilePath.IsEmpty())
	{
		if (!File.IsValid())
		{
			// On the reading thread (the HTTP thread for curl): never disk IO on the game thread
			File.Reset(IFileManager::Get().CreateFileReader(*FilePath));
			if (File.IsValid() && File->TotalSize() != Total)
			{
				// Review N-4: the file changed since its size became the Content-Length; sending it would disagree
				File.Reset();
			}
		}
		if (!File.IsValid())
		{
			FailRead(Data, Length);
			return;
		}
		File->Seek(Position);
		File->Serialize(Data, Length);
		if (File->IsError())
		{
			FailRead(Data, Length);
			return;
		}
	}
	else
	{
		FMemory::Memcpy(Data, Bytes.GetData() + Position, static_cast<SIZE_T>(Length));
	}
	Count(Length);
}

void VaRest::Http::FCountingUpload::FailRead(void* Data, int64 Length)
{
	// Review N-4: curl and NSURLSession send this buffer whatever the archive says (HttpRequestPayload.cpp:86-96; Apple's
	// read:, AppleHttp.cpp:405-421): zeroes, never stale bytes, and the completion fails the request
	FMemory::Memzero(Data, static_cast<SIZE_T>(Length));
	bReadFailed = true;
	SetError();
}

void VaRest::Http::FCountingUpload::Seek(int64 InPos)
{
	Position = FMath::Clamp<int64>(InPos, 0, Total);
}

void VaRest::Http::FCountingUpload::Count(int64 Length)
{
	Position += Length;
	HttpTransportPrivate::AtomicMax(Probe->BytesSent, Position);
	if (bStamp && Position == Total && Probe->UploadDoneAt.load() < 0.0)
	{
		Probe->UploadDoneAt = Clock.IsValid() ? Clock->Now() : FPlatformTime::Seconds();
	}
}

TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> VaRest::Http::GetDefaultTransport()
{
	static TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Instance = MakeShared<HttpTransportPrivate::FHttpModuleTransport, ESPMode::ThreadSafe>();
	return Instance;
}
