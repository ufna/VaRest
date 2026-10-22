// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClock.h"
#include "VaRestTransport.h"

#include <atomic>

#if WITH_DEV_AUTOMATION_TESTS

/** A scripted response */
struct VARESTHTTP_API FVaRestFakeResponse
{
	int32 Status = 200;
	FVaRestHeaders Headers;
	TArray<uint8> Body;
	/** Empty: the request URL */
	FString EffectiveUrl;
	EVaRestTransportFailure Failure = EVaRestTransportFailure::None;
	FName FailureDetail;
	/** Virtual seconds until it completes; 0 = at once; negative = only when the test calls Complete() */
	double Delay = 0.0;

	static FVaRestFakeResponse Json(int32 InStatus, FUtf8StringView InJson);
	static FVaRestFakeResponse Text(int32 InStatus, FStringView InText, FStringView InContentType = TEXTVIEW("text/plain; charset=utf-8"));
	static FVaRestFakeResponse Bytes(int32 InStatus, TArray<uint8> InBody, FStringView InContentType = TEXTVIEW("application/octet-stream"));
	static FVaRestFakeResponse Failed(EVaRestTransportFailure InFailure, FName InDetail = NAME_None);
	static FVaRestFakeResponse Manual();
};

/**
 * The fake transport of spec 7.1, on a virtual clock. Mirrors the real transport where the core depends on it:
 * completion from a background task (bCompleteOnWorker), a body with one byte of slack, a total timeout as Timeout and an
 * activity timeout as Connection (HttpRequestCommon.cpp:299-305), the probe, ResponseFile, MaxBodyBytes (a body over it is
 * Failure Other, detail ResponseTooLarge, nothing kept), the verbs it sends a body with (SendsBodyFor), and IN-11's bind
 * counting. Game thread, except completions with bCompleteOnWorker. Compiled with WITH_DEV_AUTOMATION_TESTS only (review
 * M15): it is not for games.
 */
class VARESTHTTP_API FVaRestFakeTransport final : public IVaRestTransport, public TSharedFromThis<FVaRestFakeTransport, ESPMode::ThreadSafe>
{
public:
	explicit FVaRestFakeTransport(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InClock);

	using FResponder = TFunction<FVaRestFakeResponse(const FVaRestTransportRequest&)>;

	/** Answers every call that Enqueue did not script; default: 200 with an empty JSON object */
	void SetResponder(FResponder InResponder);
	/** The next call's response (first in, first out), before the responder */
	void Enqueue(FVaRestFakeResponse Response);

	/** true (default): completions come from a UE::Tasks task, as the HTTP thread's do; false: inline, inside Start, Complete or Advance */
	bool bCompleteOnWorker = true;

	/**
	 * Seconds; 0 (default) = none. The engine's [HTTP] HttpActivityTimeout (30 s, HttpModule.cpp:98), applied as the
	 * engine applies it: to a call for which the HTTP transport sets no activity timeout (EffectiveActivityTimeout is 0),
	 * ending its silence as Connection, detail ActivityTimeout. A call with only a Timeout keeps no activity timer here:
	 * its total fires first or with it.
	 */
	float EngineActivityTimeout = 0.f;

	/** false (default): like the HTTP transport, a body only with POST, PUT, PATCH and DELETE (plan decision 13); true: any verb */
	bool bSendsBodyForAnyVerb = false;

	virtual bool SendsBodyFor(FStringView Verb) const override { return bSendsBodyForAnyVerb || VaRest::Http::IsBodyVerb(Verb); }

	struct FCall
	{
		FVaRestTransportRequest Request;
		double StartedAt = 0.0;
		bool bCompleted = false;
		bool bCanceled = false;
		/** The completed body's buffer and size (zero-copy tests compare it with the parsed document's strings) */
		const uint8* BodyData = nullptr;
		int32 BodyNum = 0;
	};

	int32 NumCalls() const { return Entries.Num(); }
	const FCall& GetCall(int32 Index) const { return Entries[Index]->Call; }
	int32 NumInFlight() const;
	int32 NumCanceled() const;
	/** What the call sent: its bytes, its upload file, or its multipart segments read through FVaRestSegmentStream */
	TArray<uint8> ReadBody(int32 Index) const;
	/** Completes a Manual() call or one still waiting for its delay; a completed call ignores it */
	void Complete(int32 Index, FVaRestFakeResponse Response);
	/** Writes the call's probe as the HTTP thread would, and counts as activity for its activity timeout */
	void ReportProgress(int32 Index, int64 BytesSent, int64 BytesReceived, int64 DownloadTotal = -1);
	/** Destroys the call's completion without calling it, as the engine does at HTTP module shutdown and with NullHttp (review I-3) */
	void Drop(int32 Index);

	/**
	 * Completions queued on the worker pipe (bCompleteOnWorker; game thread), and those whose callback has returned there
	 * (any thread). The callback queues its game-thread task before it returns, so once the two are equal every completion's
	 * game-thread task is queued (FTestRig::Drain, final review M10). Inline completions count in neither.
	 */
	int32 NumCompletionsQueued() const { return CompletionsQueued; }
	int32 NumCompletionsRun() const { return CompletionsRun->load(); }

	virtual TSharedRef<IVaRestTransportCall, ESPMode::ThreadSafe> Start(FVaRestTransportRequest&& Request, FVaRestTransportComplete&& OnComplete) override;

private:
	class FCallHandle;

	struct FEntry
	{
		FCall Call;
		FVaRestTransportComplete OnComplete;
		FVaRestTimerHandle DelayTimer;
		FVaRestTimerHandle TimeoutTimer;
		FVaRestTimerHandle ActivityTimer;
		double LastActivityAt = 0.0;
		/** The activity timeout this call runs with: its own, or EngineActivityTimeout when the transport would set none */
		float Activity = 0.f;
	};

	void Finish(int32 Index, FVaRestFakeResponse&& Response);
	void CancelCall(int32 Index);
	void ArmActivity(int32 Index);

	TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Clock;
	FResponder Responder;
	TArray<FVaRestFakeResponse> Queue;
	TArray<TSharedRef<FEntry>> Entries;
	int32 CompletionsQueued = 0;
	/** Shared with the queued completions: one may run after this transport is gone */
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> CompletionsRun = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
};

#endif // WITH_DEV_AUTOMATION_TESTS
