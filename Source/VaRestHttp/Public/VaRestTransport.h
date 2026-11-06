// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBody.h"
#include "VaRestClock.h"
#include "VaRestHttpTypes.h"

#include <atomic>

class IHttpRequest;

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
	 * Seconds, final (spec 4.7): a value above 0 is this attempt's total timeout, 0 leaves the engine's default ([HTTP]
	 * HttpTotalTimeout). Never negative. The core has already cut it to the retry deadline's time left (plan decision 10).
	 */
	float Timeout = 0.f;
	/**
	 * Seconds without any transfer, final: a value above 0 is this attempt's activity timeout, 0 leaves the engine's idle
	 * default ([HTTP] HttpActivityTimeout, 30 s). Never negative. The core has already applied decision 30 (a request with
	 * a total and no activity timeout of its own gets its total here, so the engine's 30 s cannot fire first; review I-5)
	 * and the deadline's cut, so a transport sets it as it is and never derives it from Timeout (Task 9 review I-1).
	 */
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
	 * nothing was sent), "HttpDisabled" and "DomainNotAllowed" (the engine's PreCheck would refuse it: [HTTP] bEnableHttp
	 * off, or a URL the HTTP manager's domain filter does not allow; nothing was sent, plan decision 15). BodyRejected,
	 * InvalidHeader, UploadReadFailed, BodyFileMissing, HttpDisabled and DomainNotAllowed describe the request itself
	 * (Kind=Config, not retryable; ClassifyAttempt, VaRestClassify.h).
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
	 * Request.Timeout and Request.ActivityTimeout are final (decision 30 and the retry deadline are applied): set each that
	 * is above 0 as it is, leave the engine's default for one that is 0, and derive neither from the other.
	 */
	virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) = 0;

	/**
	 * Game thread. False when this transport sends no request body with Verb: Resolve then fails a request with a body on
	 * that verb, Kind=Config, instead of letting it go out without one (plan decision 13). GET is spec 3.2's case and never
	 * reaches this (its body is dropped with a Warning). Default: every verb.
	 */
	virtual bool SendsBodyFor(FStringView Verb) const { return true; }
};

namespace VaRest::Http
{
	/** The FHttpModule transport (spec 0.4: "UE's FHttpModule is the transport"); every client uses it unless WithTransport replaces it */
	VARESTHTTP_API TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> GetDefaultTransport();
	/** GT compatibility bridge. The next attempt uses this engine request with the normal sink, policy and completion machinery. */
	VARESTHTTP_API TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> TransportForRequest(TSharedRef<IHttpRequest,ESPMode::ThreadSafe> Request);

	/**
	 * The verbs the engine's HTTP transport sends a body with: POST, PUT, PATCH, DELETE, any case (curl upper-cases the verb,
	 * CurlHttp.cpp:321, and attaches its read function only for those four, :941-993; HEAD sets CURLOPT_NOBODY, :975-978)
	 */
	VARESTHTTP_API bool IsBodyVerb(FStringView Verb);
}
