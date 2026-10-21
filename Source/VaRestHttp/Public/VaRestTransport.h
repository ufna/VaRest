// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBody.h"
#include "VaRestClock.h"
#include "VaRestHttpTypes.h"

#include <atomic>

/**
 * Written from the transport's threads while an attempt runs, read on the game thread: progress (spec 3.2) and the IN-3
 * transport times. Atomics, so neither side needs the other's lock.
 */
struct FVaRestTransportProbe
{
	std::atomic<int64> BytesSent{ 0 };
	std::atomic<int64> BytesReceived{ 0 };
	/** The request body's size, set before Start */
	std::atomic<int64> UploadTotal{ 0 };
	/** The response's Content-Length once known; -1 until then and for chunked responses */
	std::atomic<int64> DownloadTotal{ -1 };
	/** IVaRestClock seconds; -1 = not reached (or not watched) */
	std::atomic<double> UploadDoneAt{ -1.0 };
	std::atomic<double> StatusAt{ -1.0 };
};

/** One attempt, as the transport sees it */
struct FVaRestTransportRequest
{
	uint64 RequestId = 0;
	int32 Attempt = 1;
	FString Verb;
	FString Url;
	FVaRestHeaders Headers;
	/** In-memory body, moved into IHttpRequest::SetContent(TArray<uint8>&&) (spec 2.4) */
	TArray<uint8> Body;
	/**
	 * A file streamed while uploading, through a one-segment FVaRestSegmentStream and IHttpRequest::SetContentFromStream.
	 * Probe->UploadTotal must be its size when the body was built: Start fails a file that is missing or unreadable
	 * (BodyFileMissing) or no longer that size (UploadReadFailed) before connecting, and a change while it uploads fails it
	 * with UploadReadFailed (task 7 review M-1).
	 */
	FString BodyFile;
	/** Multipart with file parts, streamed with IHttpRequest::SetContentFromStream */
	TArray<FVaRestBodySegment> BodySegments;
	/**
	 * Seconds; 0 = VaRest sets none (spec 4.7). With Timeout > 0 and ActivityTimeout 0 the HTTP transport also sets the
	 * engine's activity timeout to Timeout, so the engine's 30 s idle default never fires first (review I-5).
	 */
	float Timeout = 0.f;
	float ActivityTimeout = 0.f;
	EVaRestPriority Priority = EVaRestPriority::Normal;
	/** Non-empty: stream the response body into this file (SaveTo's temp file) instead of memory */
	FString ResponseFile;
	/**
	 * The result's JSON cap (MaxJsonBytes, decision 20); 0 for results that are not JSON or have no cap. The body sink
	 * stops the transfer once more arrived (Failure Other, detail ResponseTooLarge) and reserves at most MaxBodyBytes + 1
	 * up front (8 MiB + 1 without a cap), so a lying Content-Length costs no more than the cap (reviews M-6, M13, M-3).
	 */
	int64 MaxBodyBytes = 0;
	/** Bind progress reporting (a progress callback or trace metadata) */
	bool bReportProgress = false;
	/** A progress callback is bound (OnProgress): on curl the upload is counted as it is read (decision 29, review N-9) */
	bool bProgressCallback = false;
	/** Bind the status-code callback and stamp transport times (trace metadata only, IN-11) */
	bool bReportStatusTime = false;
	TSharedRef<FVaRestTransportProbe, ESPMode::ThreadSafe> Probe = MakeShared<FVaRestTransportProbe, ESPMode::ThreadSafe>();
	/** Stamps the probe's and the result's times */
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
};

/** What one attempt produced */
struct FVaRestTransportResult
{
	/** 0 = no response */
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** The body, with one byte of slack so FVaRestJson::ParseOwned parses it without a copy (spec 2.1) */
	TArray<uint8> Body;
	/** The body went to the request's ResponseFile */
	bool bBodyInFile = false;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	/**
	 * The transport's own reason: LexToString(EHttpFailureReason), "ActivityTimeout", and VaRest's "ResponseTooLarge"
	 * (MaxBodyBytes), "ResponseFileWriteFailed", "ResponseBodyRejected", "BodyRejected", "InvalidHeader",
	 * "TransportDropped" (the transport never called back), "UploadReadFailed" (the body could not be read while it was
	 * sent, or a BodyFile changed size since it was built), "BodyFileMissing" (a BodyFile missing or unreadable at Start:
	 * nothing was sent). BodyRejected, InvalidHeader, UploadReadFailed and BodyFileMissing describe the request itself
	 * (Kind=Config, not retryable).
	 */
	FName FailureDetail;
	int64 BytesUp = 0;
	int64 BytesDown = 0;
	/** IVaRestClock seconds; -1 unless bReportStatusTime */
	double TransportDoneAt = -1.0;
};

class IVaRestTransportCall
{
public:
	virtual ~IVaRestTransportCall() = default;

	/** Game thread. Asks the transport to stop; its completion then reports Canceled, or the result it already had. */
	virtual void Cancel() = 0;
};

using FVaRestTransportComplete = TUniqueFunction<void(FVaRestTransportResult&&)>;

/** Spec 4: the chain's last step. FHttpModule by default (Task 7); tests inject FVaRestFakeTransport (spec 7.1). */
class IVaRestTransport
{
public:
	virtual ~IVaRestTransport() = default;

	/**
	 * Game thread. OnComplete runs exactly once, on any thread (the HTTP thread for FHttpModule), possibly before Start
	 * returns; the core moves the result to the game thread itself (spec 1). A transport that destroys OnComplete without
	 * calling it (the engine does at HTTP module shutdown and with NullHttp) still finishes the request: the core wraps
	 * OnComplete so that its destruction reports Failure Other, detail TransportDropped (review I-3).
	 */
	virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) = 0;
};

namespace VaRest::Http
{
	/** The FHttpModule transport (spec 0.4: "UE's FHttpModule is the transport"); every client uses it unless WithTransport replaces it */
	VARESTHTTP_API TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> GetDefaultTransport();
}
