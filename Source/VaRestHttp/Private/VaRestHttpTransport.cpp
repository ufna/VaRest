// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestTransport.h"

#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/ConfigCacheIni.h"
#include "VaRestBodySink.h"
#include "VaRestHttpStats.h"
#include "VaRestHttpTransportInternal.h"
#include "VaRestTrace.h"

namespace VaRest::Http::HttpTransportPrivate
{
	TAutoConsoleVariable<bool> CVarCompleteOnGameThread(
		TEXT("VaRest.Http.CompleteOnGameThread"),
		false,
		TEXT("Spec 9 fallback for an HTTP backend that cannot complete requests on its own thread: complete them on the game thread instead. Decoding still runs on UE::Tasks. Read when each request starts; see docs/dev/http-thread-completion.md."),
		ECVF_Default);

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

	/**
	 * Task 7 review M-1: a BodyFile's size as one open read handle sees it, on the game thread before ProcessRequest; -1
	 * when it cannot be opened. The engine's own SetContentAsStreamedFile path opened the file at the same point
	 * (ProcessRequest -> PreProcess -> SetupRequest -> OpenRequestPayloadDefaultImpl, HttpRequestCommon.cpp:180-201,
	 * :752-762; HttpRequestPayload.cpp:99-115), and Apple's stats it there too (AppleHttp.cpp:604-610).
	 */
	int64 MeasureReadableFile(const FString& Path)
	{
		const TUniquePtr<IFileHandle> Handle(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
		return Handle.IsValid() ? Handle->Size() : -1;
	}

	class FCall final : public IVaRestTransportCall
	{
	public:
		FCall(TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> InRequest, TWeakPtr<FHttpTransportCompletion, ESPMode::ThreadSafe> InCompletion)
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
			const TSharedPtr<FHttpTransportCompletion, ESPMode::ThreadSafe> Done = Completion.Pin();
			if (!Done.IsValid() || Done->IsDone())
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
		TWeakPtr<FHttpTransportCompletion, ESPMode::ThreadSafe> Completion;
	};

	/**
	 * Completes a request that never reaches the engine: no connection, nothing sent. On the calling (game) thread, before
	 * Start returns, which IVaRestTransport::Start allows.
	 */
	TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> FailBeforeConnect(const TSharedRef<IHttpRequest, ESPMode::ThreadSafe>& Http,
		const TSharedRef<FHttpTransportCompletion, ESPMode::ThreadSafe>& Completion, FName Detail)
	{
		if (FVaRestTransportComplete Callback = Completion->Claim())
		{
			FVaRestTransportResult Result;
			Result.Failure = EVaRestTransportFailure::Other;
			Result.FailureDetail = Detail;
			Callback(MoveTemp(Result));
		}
		return MakeShared<FCall, ESPMode::ThreadSafe>(TWeakPtr<IHttpRequest, ESPMode::ThreadSafe>(Http), Completion);
	}

	class FHttpModuleTransport final : public IVaRestTransport
	{
	public:
		FHttpModuleTransport()=default;
		explicit FHttpModuleTransport(TSharedRef<IHttpRequest,ESPMode::ThreadSafe> Request):Provided(MoveTemp(Request)) {}
		virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) override;
		virtual bool SendsBodyFor(FStringView Verb) const override { return IsBodyVerb(Verb); }
	private:
		TSharedPtr<IHttpRequest,ESPMode::ThreadSafe> Provided;
	};

	TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> FHttpModuleTransport::Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete)
	{
		check(IsInGameThread());
		// Spec 4.8: the engine request VaRest makes and what it copies into it are VaRest's
		LLM_SCOPE_BYTAG(VaRest);
		const bool bProvided=Provided.IsValid();
		const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Http = bProvided ? Provided.ToSharedRef() : FHttpModule::Get().CreateRequest();Provided.Reset();
		Http->SetVerb(Request.Verb);
		Http->SetURL(Request.Url);
		TSet<FString> ReplacedHeaders;
		for (const FVaRestHeaders::FPair& Pair : Request.Headers)
		{
			// SetHeader replaces; a repeated name is joined with ", " by AppendToHeader (IHttpRequest.h:299-310)
			if (Http->GetHeader(Pair.Key).IsEmpty() || (bProvided && !ReplacedHeaders.Contains(Pair.Key)))
			{
				Http->SetHeader(Pair.Key, Pair.Value);
			}
			else
			{
				Http->AppendToHeader(Pair.Key, Pair.Value);
			}
			if (bProvided) ReplacedHeaders.Add(Pair.Key);
		}

		const TSharedRef<FBodySink, ESPMode::ThreadSafe> Sink = Request.ResponseFile.IsEmpty()
			? MakeShared<FBodySink, ESPMode::ThreadSafe>(Request.MaxBodyBytes)
			: MakeShared<FBodySink, ESPMode::ThreadSafe>(Request.ResponseFile, Request.MaxBodyBytes);
		const TSharedRef<FHttpTransportCompletion, ESPMode::ThreadSafe> Completion = MakeShared<FHttpTransportCompletion, ESPMode::ThreadSafe>(MoveTemp(OnComplete), Sink, Request.Probe, Request.Clock);
		const int64 UploadTotal = Request.Probe->UploadTotal.load();
		Completion->bStamp = Request.bReportStatusTime;
		Completion->bStampUploadFromProgress = Request.bReportStatusTime && ShouldStampUploadFromProgress(UploadTotal, PLATFORM_APPLE != 0);
		Completion->bProgressBound = Request.bReportProgress;
		Completion->BytesUp = UploadTotal;

		if (!Request.BodyFile.IsEmpty())
		{
			// Task 7 review M-1: never send a file that is not the body that was built. Missing or unreadable: fail before
			// connecting (the stream would send zeroes). Another size: UploadTotal, the Content-Length, came from the old one.
			// A change after this is caught by the reader's own size check and read errors (UploadReadFailed).
			const int64 OnDisk = MeasureReadableFile(Request.BodyFile);
			if (OnDisk < 0)
			{
				return FailBeforeConnect(Http, Completion, TEXT("BodyFileMissing"));
			}
			if (OnDisk != UploadTotal)
			{
				return FailBeforeConnect(Http, Completion, TEXT("UploadReadFailed"));
			}
		}
		// Plan decision 15 (final review M9): the engine's PreCheck refuses these (HttpRequestCommon.cpp:83-87, :109-113) as
		// EHttpFailureReason::Other, a retryable-looking network failure; they are the configuration's (DomainFilterInput: why)
		if (!FHttpModule::Get().IsHttpEnabled())
		{
			return FailBeforeConnect(Http, Completion, TEXT("HttpDisabled"));
		}
		if (!FHttpModule::Get().GetHttpManager().IsDomainAllowed(DomainFilterInput(Request.Url)))
		{
			return FailBeforeConnect(Http, Completion, TEXT("DomainNotAllowed"));
		}

		bool bBodyAccepted = true;
		// Task 7 review M-10: the config lookup only when its answer can matter (an uncounted, traced body on curl)
		const bool bCurlMayRewind = !PLATFORM_APPLE && UploadTotal > 0 && !Request.bProgressCallback && Request.bReportProgress && CurlMayRewindBodies();
		if (ShouldCountUpload(UploadTotal, Request.bProgressCallback, Request.bReportProgress, bCurlMayRewind))
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
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Request.BodySegments), Request.RequestId),
					Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			else
			{
				Counted = MakeShared<FCountingUpload, ESPMode::ThreadSafe>(MoveTemp(Request.Body), Request.Probe, Request.Clock, Request.bReportStatusTime);
			}
			Completion->CountedUpload = Counted;
			bBodyAccepted = Http->SetContentFromStream(Counted.ToSharedRef());
		}
		else if (!Request.BodyFile.IsEmpty() || Request.BodySegments.Num() > 0)
		{
			// Serialize runs on the HTTP thread (IHttpRequest.h:250-257); the stream owns its segments. The completion keeps
			// it too, to see a failed read, which the engine ignores (Task 3 review M4). A BodyFile is one segment of the size
			// measured above, not SetContentAsStreamedFile: the engine's reader sends stale bytes when the file shrinks
			// mid-upload and the request succeeds (task 7 review M-1). Neither path is seekable on curl (CurlHttp.cpp:368-392).
			TArray<FVaRestBodySegment> Segments = MoveTemp(Request.BodySegments);
			if (!Request.BodyFile.IsEmpty())
			{
				FVaRestBodySegment& FileSegment = Segments.AddDefaulted_GetRef();
				FileSegment.FilePath = MoveTemp(Request.BodyFile);
				FileSegment.FileSize = UploadTotal;
			}
			const TSharedRef<FVaRestSegmentStream, ESPMode::ThreadSafe> SegmentStream = MakeShared<FVaRestSegmentStream, ESPMode::ThreadSafe>(MoveTemp(Segments), Request.RequestId);
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
			return FailBeforeConnect(Http, Completion, TEXT("BodyRejected"));
		}
		// Final values (IVaRestTransport::Start): the transport stage resolved decision 30 and the retry deadline; 0 leaves
		// the engine's default. On Apple the connection timeout still caps silence: NSURLSession's timeoutInterval is
		// [HTTP] HttpConnectionTimeout (30 s, BaseEngine.ini:40; AppleHttp.cpp:736-738), and an activity timeout above it
		// logs "HttpConnectionTimeout can't be less than HttpActivityTimeout" as a Warning on every such request
		// (AppleHttp.cpp:740-749; task 7 review M-9) -- .Timeout(60) without an activity timeout is one.
		// docs/dev/http-thread-completion.md (Task 17) records both.
		if (Request.Timeout > 0.f)
		{
			Http->SetTimeout(Request.Timeout);
		}
		if (Request.ActivityTimeout > 0.f)
		{
			Http->SetActivityTimeout(Request.ActivityTimeout);
		}
		Http->SetPriority(MapPriority(Request.Priority));
		// Spec 1: complete on the HTTP thread; the completion below only moves bytes and dispatches. Spec 9's fallback
		// completes on the game thread instead; the completion code is the same either way.
		Http->SetDelegateThreadPolicy(CVarCompleteOnGameThread.GetValueOnAnyThread()
			? EHttpRequestDelegateThreadPolicy::CompleteOnGameThread
			: EHttpRequestDelegateThreadPolicy::CompleteOnHttpThread);

		// Review M-16: the response's own headers cannot be read while it is in flight -- GetHeader logs a Warning and
		// returns "" until OnFinishRequest sets bIsReady (HttpResponseCommon.cpp:76-92, HttpRequestCommon.cpp:870-873) --
		// so a Content-Encoding is noted as it arrives. Under CompleteOnHttpThread curl calls this from its header callback on
		// the HTTP thread, before that response's first body byte (CurlHttp.cpp:532-535); Apple from didReceiveResponse, on
		// the session's serial delegate queue that later delivers the body (AppleHttp.cpp:171-187, :865-874;
		// HttpRequestImpl.cpp:40-60; delegateQueue nil, ApplePlatformHttp.cpp:265). Under the spec 9 fallback both defer it
		// to the game-thread tick (CurlHttp.cpp:536-539, :1212-1219; AppleHttp.cpp:875-881) while body chunks still arrive on
		// the receiving thread, so the first chunk can come first: the reserve then follows a compressed Content-Length and
		// DownloadTotal shows it (docs/dev/http-thread-completion.md). It also collects each header line apart (AddHeader,
		// plan decision 14). Not an IN-11 bind: the sink and the result need it whether or not anything is traced, so
		// DelegateBinds does not count it.
		Http->OnHeaderReceived().BindLambda([Completion](FHttpRequestPtr, const FString& HeaderName, const FString& HeaderValue)
		{
			// Spec 4.8 (Task 20 review M-3): the header list grows here, on the HTTP thread, outside any scope VaRest set
			LLM_SCOPE_BYTAG(VaRest);
			if (!HeaderValue.IsEmpty() && HeaderName.Equals(TEXT("Content-Encoding"), ESearchCase::IgnoreCase))
			{
				Completion->bEncodedBody = true;
			}
			Completion->AddHeader(HeaderName, HeaderValue);
		});

		const TWeakPtr<IHttpRequest, ESPMode::ThreadSafe> WeakHttp = Http;
		Http->SetResponseBodyReceiveStreamDelegateV2(FHttpRequestStreamDelegateV2::CreateLambda([Completion, WeakHttp](void* Ptr, int64& InOutLength)
		{
			// Spec 4.8: the response body grows here, on the HTTP thread, outside any scope VaRest set on the game thread
			LLM_SCOPE_BYTAG(VaRest);
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
				// Review I-1: only with a body, and only where this delegate sees the upload end (Apple); a counted curl upload
				// is stamped by FCountingUpload, an uncounted one stays -1 (ShouldStampUploadFromProgress)
				if (Completion->bStampUploadFromProgress && Probe.UploadDoneAt.load() < 0.0 && static_cast<int64>(Sent) >= Probe.UploadTotal.load())
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
			// The HTTP thread (the game thread under the spec 9 fallback). Read what the engine holds, take the body from our
			// sink, hand the result on: no parsing here.
			// Spec 4.8 (Task 20 review M-3): the result's headers, URL and type are copied here, outside any VaRest scope
			LLM_SCOPE_BYTAG(VaRest);
			FVaRestTransportComplete Callback = Completion->Claim();
			if (!Callback)
			{
				return;
			}
			FVaRestTransportResult Result;
			if (Resp.IsValid())
			{
				Result.Status = Resp->GetResponseCode();
				// Plan decision 14: the lines one by one when the delegate saw them, else the engine's folded list
				Result.Headers = Completion->TakeHeaders();
				if (Result.Headers.IsEmpty())
				{
					Result.Headers = FVaRestHeaders::FromLines(Resp->GetAllHeaders());
				}
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
			// body. Both flags are atomics, so reading them here is never a data race (task 7 review M-2). Seeing a failed
			// read needs the read to happen before this: on curl it does, the read callback (CurlHttp.cpp:668-683) and this
			// finish (HttpThread.cpp:452-461) run on the HTTP thread in that order. On Apple NSURLSession reads the stream on
			// a worker thread (AppleHttp.cpp:402-421); with a response body, its delivery reaches TakeBytes above through the
			// sink mutex, and without one only NSURLSession's own, undocumented ordering puts the read before
			// didCompleteWithError (AppleHttp.cpp:220-233): a bodyless response there may miss the flag.
			const bool bCountedReadFailed = Completion->CountedUpload.IsValid() && Completion->CountedUpload->HasReadFailed();
			const bool bSegmentReadFailed = Completion->SegmentUpload.IsValid() && Completion->SegmentUpload->HasReadFailed();
			if (bCountedReadFailed || bSegmentReadFailed)
			{
				Result.Failure = EVaRestTransportFailure::Other;
				Result.FailureDetail = TEXT("UploadReadFailed");
				Result.Body.Empty();
			}
			// Review M-18 (task 7 review M-4): what was sent when anything watched it -- the counting archive, or the progress
			// delegate, whose last call precedes this (curl: CurlHttp.cpp:1274) -- otherwise the body's size
			Result.BytesUp = Completion->CountedUpload.IsValid() || Completion->bProgressBound ? Completion->Probe->BytesSent.load() : Completion->BytesUp;
			Result.BytesDown = Received.Num();
			if (Completion->bStamp)
			{
				Result.TransportDoneAt = Completion->Now();
			}
			Callback(MoveTemp(Result));
		});

		// The completion delegate runs even when ProcessRequest refuses to start ("always called when the request completes
		// or on error", IHttpRequest.h:351): the refusal arrives there as a failure, so the return value adds nothing.
		// When the engine drops it instead, ~FHttpTransportCompletion finishes the request (review I-3).
		Http->ProcessRequest();
		return MakeShared<FCall, ESPMode::ThreadSafe>(WeakHttp, Completion);
	}
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

bool VaRest::Http::ShouldStampUploadFromProgress(int64 UploadTotal, bool bBackendReportsUploadProgress)
{
	return UploadTotal > 0 && bBackendReportsUploadProgress;
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
			// Silent (task 7 review M-5): a file that shrinks mid-upload makes the reader log a Warning or an Error
			// (FileManagerGeneric.cpp:808, :858, :868, :875); the failure is reported once, as UploadReadFailed
			File.Reset(IFileManager::Get().CreateFileReader(*FilePath, FILEREAD_Silent));
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

FString VaRest::Http::DomainFilterInput(const FString& Url)
{
	const int32 SchemeEnd = Url.Find(TEXT("://"), ESearchCase::CaseSensitive);
	if (SchemeEnd == INDEX_NONE)
	{
		return Url;
	}
	int32 AuthorityEnd = SchemeEnd + 3;
	while (AuthorityEnd < Url.Len() && Url[AuthorityEnd] != TEXT('/') && Url[AuthorityEnd] != TEXT('?') && Url[AuthorityEnd] != TEXT('#'))
	{
		++AuthorityEnd;
	}
	return Url.Left(AuthorityEnd);
}

bool VaRest::Http::IsBodyVerb(FStringView Verb)
{
	return Verb.Equals(TEXTVIEW("POST"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("PUT"), ESearchCase::IgnoreCase)
		|| Verb.Equals(TEXTVIEW("PATCH"), ESearchCase::IgnoreCase) || Verb.Equals(TEXTVIEW("DELETE"), ESearchCase::IgnoreCase);
}

TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> VaRest::Http::GetDefaultTransport()
{
	// Leaked: nothing in VaRestHttp runs at static destruction (review I-4, task 7 review M-7)
	static TSharedRef<IVaRestTransport, ESPMode::ThreadSafe>* const Instance =
		new TSharedRef<IVaRestTransport, ESPMode::ThreadSafe>(MakeShared<HttpTransportPrivate::FHttpModuleTransport, ESPMode::ThreadSafe>());
	return *Instance;
}
TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> VaRest::Http::TransportForRequest(TSharedRef<IHttpRequest,ESPMode::ThreadSafe> Request)
{
	check(IsInGameThread());return MakeShared<HttpTransportPrivate::FHttpModuleTransport,ESPMode::ThreadSafe>(MoveTemp(Request));
}
