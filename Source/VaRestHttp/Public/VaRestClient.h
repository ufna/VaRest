// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/Paths.h"
#include "Misc/SourceLocation.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestAuthTypes.h"
#include "VaRestBody.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonConvert.h"
#include "VaRestResultHandler.h"
#include "VaRestRetry.h"
#include "VaRestUrl.h"
#include "VaRestStage.h"

#include <type_traits>

class FVaRestAuthRefreshContext;
class FVaRestClient;
class FVaRestRequestHandle;
class IVaRestAuth;
/** Declared here for FRequestSpec's friendship; the definition below carries [[nodiscard]] (allowed on a later declaration) */
template <typename T>
class TVaRestRequest;

namespace VaRest::Http
{
	struct FClientAccess;
	struct FRequestSpecAccess;

	/**
	 * What a TVaRestRequest collects; Send moves it into an exchange. Private by shape (bead VaRest-uq3, plan decision 18):
	 * only the builder, FVaRestClient (which makes one) and VaRestHttp's own code (FRequestSpecAccess, defined in a Private
	 * header) reach its fields, so no module can hand-build a spec that skips the builder's checks. Resolve's checks run
	 * for every request anyway, and the transport stage checks the final headers again (M3 review I3).
	 */
	class VARESTHTTP_API FRequestSpec
	{
	public:
		~FRequestSpec();
		FRequestSpec(const FRequestSpec&) = delete;
		FRequestSpec& operator=(const FRequestSpec&) = delete;

	private:
		template <typename>
		friend class ::TVaRestRequest;
		friend class ::FVaRestClient;
		friend struct FRequestSpecAccess;

		FRequestSpec(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState, FStringView InVerb, FStringView InPath);

		/** Records the first misuse met while building; Send then fails with Kind=Config (decisions 10, 11) */
		void Fail(FString Problem);

		TSharedPtr<FClientState, ESPMode::ThreadSafe> State;
		/** The client's configuration when the builder was made: struct bodies encode with its naming policy in Body() */
		TSharedPtr<const FClientConfig, ESPMode::ThreadSafe> ConfigAtBuild;
		FString Verb;
		FString Path;
		TArray<TPair<FString, FString>> PathParams;
		TArray<TPair<FString, FString>> Query;
		FVaRestHeaders Headers;
		FVaRestRequestBody Body;
		FString BuildError;
		FName Tag;
		EVaRestPriority Priority = EVaRestPriority::Normal;
		TWeakObjectPtr<const UObject> Owner;
		bool bHasOwner = false;
		bool bIdempotent = false;
		bool bKeepBody = false;
		bool bOnHttpThread = false;
		FString SaveTo;
		TOptional<float> Timeout;
		TOptional<float> ActivityTimeout;
		TOptional<int64> MaxJsonBytes;
		EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
		bool bStrictDecode = false;
		/** Retry() / NoRetry(); unset: the client's policy */
		TOptional<FVaRestRetryPolicy> Retry;
		/** Envelope(); unset with bNoEnvelope false: the client's */
		TOptional<FVaRestEnvelope> Envelope;
		/** NoEnvelope() */
		bool bNoEnvelope = false;
		/** AuthMode() / NoAuth() (plan decision 4) */
		EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
		/** A refresh request (FVaRestAuthRefreshContext::Request; plan decision 23) */
		bool bAuthRefresh = false;
		/** Spec review I-2: the request id to use instead of a new one -- the refresh's own (FVaRestTrace::NewRequestId); 0 = a new one */
		uint64 PresetRequestId = 0;
		/** The current body is a struct, encoded with Naming: Naming() now fails. A later Body() or BodyFile() clears it. */
		bool bStructBodySet = false;
		TUniquePtr<IResultHandler> Handler;
		bool bSent = false;
	};

	/** The inline builder's Send() calls this from the caller's module, so it is exported; it forwards to FRequestSpecAccess::Send */
	VARESTHTTP_API FVaRestRequestHandle SendRequest(TUniquePtr<FRequestSpec>&& Spec, const char* SourceFile, uint32 SourceLine);
}

/** Spec 3.2: weak -- it does not keep the request alive */
class VARESTHTTP_API FVaRestRequestHandle
{
public:
	FVaRestRequestHandle() = default;
	explicit FVaRestRequestHandle(const FVaRestExchangeRef& InExchange);

	/** The request id, also in log lines (#57) and traces (IN-10, IN-16) */
	uint64 GetId() const { return Id; }
	/**
	 * Any thread: sent and not finished. With OnHttpThread(), false once the result is handed to the worker; the callbacks
	 * may still be running.
	 */
	bool IsActive() const;
	/**
	 * Any thread. The callbacks then get Kind=Canceled (decision 2), never inside Cancel() (decision 3); after it returns
	 * on the game thread no OnSuccess runs, with two exceptions. A SaveTo() whose rename already replaced the destination
	 * delivers its success with the saved file, the truthful outcome (review M-4). With OnHttpThread(), a result already
	 * handed to the worker is past canceling (IsActive() is false): Cancel() is then a no-op while its callbacks are queued
	 * or running there. No-op on a finished request.
	 */
	void Cancel() const;

private:
	TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Exchange;
	uint64 Id = 0;
};

/**
 * Spec 3.2: one request, built fluently. Every method returns the builder by value (a moved handle, cheap), so the
 * class-level [[nodiscard]] makes a forgotten Send() a compile warning; the methods are && so a builder is used once.
 * Any thread. A body (Body(), BodyFile()) goes on the wire only for the verbs the transport uploads: see
 * FVaRestClient::Request.
 */
template <typename T>
class [[nodiscard]] TVaRestRequest
{
public:
	TVaRestRequest(TVaRestRequest&&) = default;
	TVaRestRequest& operator=(TVaRestRequest&&) = default;
	TVaRestRequest(const TVaRestRequest&) = delete;
	TVaRestRequest& operator=(const TVaRestRequest&) = delete;

	/** {Name} in the path becomes Value, percent-encoded as one segment (spec 3.2); the last value for a name wins */
	TVaRestRequest Path(FStringView Name, FStringView Value) &&
	{
		Spec->PathParams.Emplace(FString(Name), FString(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
		requires std::is_integral_v<ValueType>
	TVaRestRequest Path(FStringView Name, ValueType Value) &&
	{
		return MoveTemp(*this).Path(Name, FStringView(LexToString(Value)));
	}

	/** Appends Name=Value, percent-encoded; a repeated name is appended again (OpenAPI N9) */
	TVaRestRequest Query(FStringView Name, FStringView Value) &&
	{
		Spec->Query.Emplace(FString(Name), FString(Value));
		return MoveTemp(*this);
	}

	template <typename ValueType>
		requires std::is_integral_v<ValueType>
	TVaRestRequest Query(FStringView Name, ValueType Value) &&
	{
		return MoveTemp(*this).Query(Name, FStringView(LexToString(Value)));
	}

	/** An array value, written as Style says (OpenAPI N9, decision 34): Repeat (default) tag=a&tag=b; Csv, Ssv, Pipes one joined pair */
	TVaRestRequest Query(FStringView Name, TConstArrayView<FString> Values, EVaRestQueryStyle Style = EVaRestQueryStyle::Repeat) &&
	{
		VaRest::Http::AddQueryValues(Spec->Query, Name, Values, Style);
		return MoveTemp(*this);
	}

	/**
	 * Sets a header (replacing one of that name, whatever its case); it wins over the client's (WithHeader). Content-Type
	 * has three levels (final review I2): this request's Header() wins over the body's own type (Body(), BodyFile()), which
	 * wins over a client default, so a multipart body always names its boundary unless this request overrides it. A name
	 * that is not an RFC 9110 token, or a value holding a control character other than HTAB (CR, LF, NUL among them), fails
	 * the request with Kind=Config and nothing is sent (decision 11: header injection). A session header of the same name
	 * (FVaRestClient::SetSessionHeader) replaces it on each attempt, and the client's auth replaces both; NoAuth() keeps it.
	 */
	TVaRestRequest Header(FStringView Name, FStringView Value) &&
	{
		if (!VaRest::Http::IsValidHeaderName(Name))
		{
			Spec->Fail(FString::Printf(TEXT("invalid header name \"%s\""), *FString(Name).ReplaceCharWithEscapedChar()));
		}
		else if (!VaRest::Http::IsValidHeaderValue(Value))
		{
			Spec->Fail(FString::Printf(TEXT("the value of header \"%s\" holds CR, LF, NUL or another control character"), *FString(Name)));
		}
		else
		{
			Spec->Headers.Set(Name, Value);
		}
		return MoveTemp(*this);
	}

	/** application/json; an invalid (missing) handle sends no body, the way JSON.stringify(undefined) is nothing (spec 2.2) */
	TVaRestRequest Body(const FVaRestJson& Json) &&
	{
		if (Json.IsValid())
		{
			TArray<uint8> Bytes;
			const TValueOrError<void, FVaRestJsonError> Written = Json.TryToUtf8(Bytes);
			if (Written.HasError())
			{
				Spec->Fail(FString::Printf(TEXT("the JSON body cannot be written: %s"), *Written.GetError().Message));
			}
			Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Json, TEXT("application/json"));
			Spec->bStructBodySet = false; // task 9 review minor 1: a later Body() replaced the struct one
		}
		return MoveTemp(*this);
	}

	/** text/plain; charset=utf-8 */
	TVaRestRequest Body(FStringView Text) &&
	{
		FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		Spec->Body = FVaRestRequestBody::FromBytes(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), EVaRestBodyKind::Text, TEXT("text/plain; charset=utf-8"));
		Spec->bStructBodySet = false;
		return MoveTemp(*this);
	}

	/** application/octet-stream, moved: no copy until the transport (spec 2.4) */
	TVaRestRequest Body(TArray<uint8>&& Bytes) &&
	{
		Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Binary, TEXT("application/octet-stream"));
		Spec->bStructBodySet = false;
		return MoveTemp(*this);
	}

	/** application/octet-stream, copied */
	TVaRestRequest Body(TConstArrayView<uint8> Bytes) &&
	{
		return MoveTemp(*this).Body(TArray<uint8>(Bytes.GetData(), Bytes.Num()));
	}

	/**
	 * A USTRUCT, encoded straight to UTF-8 bytes (spec 2.4) with the naming policy known now: the request's Naming(),
	 * else the client's as it was when this builder was made (Get(), Post(), Request()): a WithNaming() on the client after
	 * that does not reach this body (task 9 review minor 3). Call Naming() first; after a struct body it fails the request.
	 */
	template <typename StructType>
		requires (VaRest::Json::bHasScriptStruct<StructType> && !std::is_same_v<StructType, FVaRestJson>)
	TVaRestRequest Body(const StructType& Value) &&
	{
		FVaRestJsonConvertOptions Options;
		Options.Naming = Spec->Naming;
		TArray<uint8> Bytes;
		FVaRestJsonError EncodeError;
		if (!FVaRestJsonConvert::StructToUtf8(VaRest::Json::GetScriptStruct<StructType>(), &Value, Bytes, Options, EVaRestJsonFormat::Condensed, &EncodeError))
		{
			Spec->Fail(FString::Printf(TEXT("the struct body cannot be written: %s"), *EncodeError.Message));
		}
		Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Json, TEXT("application/json"));
		Spec->bStructBodySet = true;
		return MoveTemp(*this);
	}

	/** application/x-www-form-urlencoded */
	TVaRestRequest Body(const FVaRestForm& Form) &&
	{
		Spec->Body = FVaRestRequestBody::FromForm(Form);
		Spec->bStructBodySet = false;
		return MoveTemp(*this);
	}

	/** multipart/form-data; file parts stream from disk while uploading */
	TVaRestRequest Body(const FVaRestMultipart& Multipart) &&
	{
		TValueOrError<FVaRestRequestBody, FString> Built = FVaRestRequestBody::FromMultipart(Multipart);
		if (Built.HasError())
		{
			Spec->Fail(Built.StealError());
		}
		else
		{
			Spec->Body = Built.StealValue();
			Spec->bStructBodySet = false;
		}
		return MoveTemp(*this);
	}

	/**
	 * A file streamed while uploading, as one segment of an FVaRestSegmentStream handed to IHttpRequest::SetContentFromStream
	 * (VaRestHttpTransport.cpp:214-229; the counting archive when OnProgress is bound, decision 29), not
	 * SetContentAsStreamedFile, whose reader sends stale bytes when the file shrinks mid-upload (task 7 review M-1). Its
	 * size is read now and checked again at the start; a missing file fails the request with Kind=Config.
	 */
	TVaRestRequest BodyFile(FStringView FilePath, FStringView ContentType = TEXTVIEW("application/octet-stream")) &&
	{
		TValueOrError<FVaRestRequestBody, FString> Built = FVaRestRequestBody::FromFile(FilePath, ContentType);
		if (Built.HasError())
		{
			Spec->Fail(Built.StealError());
		}
		else
		{
			Spec->Body = Built.StealValue();
			Spec->bStructBodySet = false;
		}
		return MoveTemp(*this);
	}

	/** Spec 3.2: "adds a stable Idempotency-Key: <uuid> across attempts" (M4's retry reads the flag too). A key set with Header is kept. */
	TVaRestRequest Idempotent() &&
	{
		Spec->bIdempotent = true;
		return MoveTemp(*this);
	}

	/** The naming policy of a struct body set after this (spec 2.4: per client, API or request) */
	TVaRestRequest Naming(EVaRestJsonNaming InNaming) &&
	{
		if (Spec->bStructBodySet)
		{
			Spec->Fail(TEXT("Naming() after a struct Body() has no effect: call Naming() first"));
		}
		Spec->Naming = InNaming;
		return MoveTemp(*this);
	}

	/** Spec 2.4: every mismatch while decoding a struct response is an error, not a warning */
	TVaRestRequest StrictDecode(bool bStrict = true) &&
	{
		Spec->bStrictDecode = bStrict;
		return MoveTemp(*this);
	}

	/** Spec 3.2: keep the raw bytes in FVaRestResponse::Body after a successful decode (a copy for JSON: the parse is in situ) */
	TVaRestRequest KeepBody() &&
	{
		Spec->bKeepBody = true;
		return MoveTemp(*this);
	}

	/**
	 * Spec 3.2: streams the body into a temp file next to FilePath and renames it onto FilePath on success; a failed or
	 * canceled download leaves FilePath untouched and deletes the temp file. Only for void and FVaRestResponse results
	 * (decision 9): a body on disk has nothing to decode. The file is written on the HTTP thread, as the engine's own
	 * SetResponseBodyReceiveStream to a file archive does (decision 31); the rename runs on a UE::Tasks worker. A Cancel()
	 * that lands after the rename delivers the success with the saved file: the destination was already replaced, and
	 * success is the truthful outcome (review M-4). The rename replaces the destination atomically everywhere but on
	 * Windows, whose MoveFileW will not replace a file: there the old file is first moved aside to a .bak next to it, and
	 * moved back if the new one cannot be moved in, so a crash between those two renames leaves the old file only as that
	 * .bak. An error response's body goes to the temp file too; its first 4 KiB are read back for FVaRestError, so a JSON
	 * error body larger than that is not parsed into FVaRestError::Body and arrives only as its RawBody prefix.
	 */
	TVaRestRequest SaveTo(FStringView FilePath) && requires (std::is_void_v<T> || std::is_same_v<T, FVaRestResponse>)
	{
		if (FilePath.IsEmpty())
		{
			Spec->Fail(TEXT("SaveTo() needs a file path"));
		}
		Spec->SaveTo = FPaths::ConvertRelativePathToFull(FString(FilePath));
		return MoveTemp(*this);
	}

	/**
	 * Spec 3.2: callbacks on a worker thread instead of the game thread, after the game thread has seen the result
	 * (decision 4). With Owner(), the owner is checked on the game thread and then pinned (TStrongObjectPtr) until the
	 * callbacks return, so GC cannot free it under them (decision 28): it may outlive its own destruction by one callback.
	 * The callbacks are also destroyed on that worker once they return, and with them everything they captured: a capture
	 * that is not safe to release off the game thread -- a NotThreadSafe TSharedPtr or TSharedRef, or an object whose
	 * destructor touches a UObject or other game-thread state -- races there (task 8 review).
	 */
	TVaRestRequest OnHttpThread() &&
	{
		Spec->bOnHttpThread = true;
		return MoveTemp(*this);
	}

	/**
	 * The JSON size cap for this request (decision 20), replacing the client's; 0 = no cap at all. Mind the neighbour
	 * (final review M6): here 0 lifts the client's limit, while Timeout(0) keeps the client's timeout.
	 */
	TVaRestRequest MaxJsonBytes(int64 Bytes) &&
	{
		Spec->MaxJsonBytes = FMath::Max<int64>(0, Bytes);
		return MoveTemp(*this);
	}

	/**
	 * Spec 4.7, per attempt: a value above 0 replaces the client's; 0 (or less) keeps the client's -- unlike MaxJsonBytes(0),
	 * which lifts the client's cap (final review M6). There is no "no timeout" for one request: to let one long download
	 * outlive the client's total, pass a larger total here. With a total and no activity timeout the activity timeout is
	 * the total too, so the engine's 30 s idle default never fires first (decision 30); on Mac and iOS the
	 * engine's connection timeout still caps silence (docs/dev/http-thread-completion.md).
	 */
	TVaRestRequest Timeout(float TotalSeconds, float ActivitySeconds = 0.f) &&
	{
		if (TotalSeconds > 0.f)
		{
			Spec->Timeout = TotalSeconds;
		}
		if (ActivitySeconds > 0.f)
		{
			Spec->ActivityTimeout = ActivitySeconds;
		}
		return MoveTemp(*this);
	}

	/**
	 * (const FVaRestProgress&): on the game thread, at most every 100 ms while bytes move, and a last call when the attempt
	 * ends if the counts moved since the previous one (decision 8) -- none after Cancel(), with the owner gone or at engine
	 * exit, and none for an attempt that changed nothing. It is not an end signal: use OnComplete for that (task 13 review
	 * M4). On curl backends an upload with OnProgress is read through a counting archive (decision 29), which curl
	 * cannot rewind: with [HTTP.Curl] bAllowSeekFunction on, a 307/308 redirect or an auth replay that must resend the body
	 * fails (with the engine default, off, curl rewinds no body at all, so nothing changes). Mac and iOS are not affected.
	 */
	template <typename FuncType>
	TVaRestRequest OnProgress(FuncType&& Func) &&
	{
		Handler().OnProgress = Forward<FuncType>(Func);
		return MoveTemp(*this);
	}

	TVaRestRequest Tag(FName InTag) &&
	{
		Spec->Tag = InTag;
		return MoveTemp(*this);
	}

	/**
	 * The limiter's queue order (spec 4.6): Highest first, first come, first served within a priority. Strict: under a
	 * steady stream of higher-priority requests a Low one can wait indefinitely, and a timeout does not cover time spent
	 * queued (see FVaRestClient::WithMaxConcurrent).
	 */
	TVaRestRequest Priority(EVaRestPriority InPriority) &&
	{
		Spec->Priority = InPriority;
		return MoveTemp(*this);
	}

	/**
	 * Spec 4.1: this request's retry policy, replacing the client's (WithRetry). A request is retried only when it is
	 * idempotent: GET, HEAD, OPTIONS, PUT or DELETE, or marked Idempotent(). A retry is a new attempt through the limiter,
	 * behind whoever waits there (spec 4.6), on the request's clock; Cancel() and the owner's destruction stop it while it
	 * waits.
	 */
	TVaRestRequest Retry(const FVaRestRetryPolicy& InPolicy) &&
	{
		if (Spec->bAuthRefresh && InPolicy.IsEnabled())
		{
			// Plan decision 23: MarkAuthRefresh ran before the auth's own chain (FVaRestAuthRefreshContext::Request), so this
			// would undo its Disabled(): every request parked behind the refresh would wait out the refresh's backoffs
			Spec->Fail(TEXT("Retry() on an auth refresh request: it is never retried (plan decision 23); the requests waiting on it retry by their own policy"));
		}
		else
		{
			Spec->Retry = InPolicy;
		}
		return MoveTemp(*this);
	}

	/** No retry for this request, whatever the client's policy */
	TVaRestRequest NoRetry() &&
	{
		Spec->Retry = FVaRestRetryPolicy::Disabled();
		return MoveTemp(*this);
	}

	/**
	 * Spec 3.4: this request's envelope, replacing the client's (WithEnvelope). See FVaRestEnvelope for which results it
	 * applies to. It judges the final attempt only: a retry is decided on each attempt's status first, so an envelope's
	 * Kind=Server failure is never retried.
	 */
	TVaRestRequest Envelope(const FVaRestEnvelope& InEnvelope) &&
	{
		if (Spec->bAuthRefresh)
		{
			// Plan decision 23: a refresh response is read raw (the auth's token paths are against the whole body)
			Spec->Fail(TEXT("Envelope() on an auth refresh request: its response is read raw (plan decision 23)"));
		}
		else
		{
			Spec->Envelope = InEnvelope;
			Spec->bNoEnvelope = false;
		}
		return MoveTemp(*this);
	}

	/** The body as it comes, whatever the client's envelope */
	TVaRestRequest NoEnvelope() &&
	{
		Spec->Envelope.Reset();
		Spec->bNoEnvelope = true;
		return MoveTemp(*this);
	}

	/** Spec 4.2 / OpenAPI N15 (plan decision 4): Required (the default), IfAvailable, or None */
	TVaRestRequest AuthMode(EVaRestAuthMode InMode) &&
	{
		if (Spec->bAuthRefresh && InMode != EVaRestAuthMode::None)
		{
			// Plan decision 23: the refresh request carries no credentials -- the ones it would carry are what it replaces
			Spec->Fail(TEXT("AuthMode() on an auth refresh request: it carries no credentials (plan decision 23)"));
		}
		else
		{
			Spec->AuthMode = InMode;
		}
		return MoveTemp(*this);
	}

	/** Spec 3.2: no credentials, no session headers, no refresh -- a login request */
	TVaRestRequest NoAuth() &&
	{
		return MoveTemp(*this).AuthMode(EVaRestAuthMode::None);
	}

	/**
	 * Spec 3.2: weak. Once the owner is gone there are no callbacks and no further attempts. An owner that goes while the
	 * request waits -- backing off before a retry, or parked behind a session refresh (Task 17) -- is noticed when that
	 * wait ends (the timer fires, the refresh finishes); until then the request keeps its callbacks, what they capture and
	 * the client alive, and counts in GetInFlightCount(). Cancel() ends a wait at once.
	 */
	TVaRestRequest Owner(const UObject* InOwner) &&
	{
		Spec->Owner = InOwner;
		Spec->bHasOwner = InOwner != nullptr;
		return MoveTemp(*this);
	}

	/** (const T&) or (const T&, const FVaRestResponse&); for T = void: () or (const FVaRestResponse&) */
	template <typename FuncType>
	TVaRestRequest OnSuccess(FuncType&& Func) &&
	{
		if constexpr (std::is_void_v<T>)
		{
			if constexpr (std::is_invocable_v<FuncType&, const FVaRestResponse&>)
			{
				Handler().OnSuccess = Forward<FuncType>(Func);
			}
			else
			{
				static_assert(std::is_invocable_v<FuncType&>, "OnSuccess of a void request takes () or (const FVaRestResponse&)");
				Handler().OnSuccess = [Callback = Forward<FuncType>(Func)](const FVaRestResponse&) mutable { Callback(); };
			}
		}
		else
		{
			if constexpr (std::is_invocable_v<FuncType&, const T&, const FVaRestResponse&>)
			{
				Handler().OnSuccess = Forward<FuncType>(Func);
			}
			else
			{
				static_assert(std::is_invocable_v<FuncType&, const T&>, "OnSuccess takes (const T&) or (const T&, const FVaRestResponse&)");
				Handler().OnSuccess = [Callback = Forward<FuncType>(Func)](const T& Result, const FVaRestResponse&) mutable { Callback(Result); };
			}
		}
		return MoveTemp(*this);
	}

	/** (const FVaRestError&) */
	template <typename FuncType>
	TVaRestRequest OnError(FuncType&& Func) &&
	{
		Handler().OnError = Forward<FuncType>(Func);
		return MoveTemp(*this);
	}

	/** (const TVaRestResult<T>&), after OnSuccess or OnError */
	template <typename FuncType>
	TVaRestRequest OnComplete(FuncType&& Func) &&
	{
		Handler().OnComplete = Forward<FuncType>(Func);
		return MoveTemp(*this);
	}

	/**
	 * Any thread. The request enters its client's pipeline on the game thread. Callbacks never run inside Send()
	 * (decision 3). Where is the C++ call site for traces (Inspector IN-15); leave it defaulted.
	 */
	FVaRestRequestHandle Send(UE::FSourceLocation Where = UE::FSourceLocation::Current()) &&
	{
		return VaRest::Http::SendRequest(MoveTemp(Spec), Where.GetFileName(), Where.GetLine());
	}

private:
	/** Only Request<T>, which gives the spec a TResultHandler<T>: Handler() casts to that type (final review I1) */
	friend class FVaRestClient;
	/** MarkAuthRefresh: a refresh request is made only by the context an auth's Refresh is handed (plan decision 23) */
	friend class ::FVaRestAuthRefreshContext;

	/**
	 * Plan decision 23: a refresh request bypasses the auth stages (AuthMode None), the envelope, retry and (ruling M-8) the
	 * limiter's queue; InPresetId, when not 0, becomes its request id (spec review I-2). Retry(), Envelope() and AuthMode()
	 * refuse to undo it afterwards.
	 */
	TVaRestRequest MarkAuthRefresh(uint64 InPresetId) &&
	{
		Spec->bAuthRefresh = true;
		Spec->PresetRequestId = InPresetId;
		Spec->AuthMode = EVaRestAuthMode::None;
		Spec->Envelope.Reset();
		Spec->bNoEnvelope = true;
		Spec->Retry = FVaRestRetryPolicy::Disabled();
		// Not the limiter's (it skips the queue): the transport's, for the engine request
		Spec->Priority = EVaRestPriority::Highest;
		return MoveTemp(*this);
	}

	explicit TVaRestRequest(TUniquePtr<VaRest::Http::FRequestSpec>&& InSpec)
		: Spec(MoveTemp(InSpec))
	{
	}

	VaRest::Http::TResultHandler<T>& Handler()
	{
		return static_cast<VaRest::Http::TResultHandler<T>&>(*Spec->Handler);
	}

	TUniquePtr<VaRest::Http::FRequestSpec> Spec;
};

/**
 * Spec 3.1: a cheap handle to one client's state -- base URL, default headers, policies, stages. Copies share it.
 * Configure it on the game thread; requests already sent keep the configuration they started with. Send from any thread.
 */
class VARESTHTTP_API FVaRestClient
{
public:
	/** An empty handle; Make() a real one. A request built on it fails with Kind=Config (see Request). */
	FVaRestClient() = default;

	/** Game thread. Defaults from UVaRestHttpSettings; the FHttpModule transport; the real clock. */
	static FVaRestClient Make(FStringView BaseUrl);

	bool IsValid() const { return State.IsValid(); }
	uint64 GetId() const;
	FString GetBaseUrl() const;
	/** Game thread: requests sent and not yet finished, those waiting in the limiter's queue included (spec 4.6) */
	int32 GetInFlightCount() const;
	/**
	 * Game thread: cancels every in-flight request with this tag (all of them for NAME_None), like Cancel() on each. It sees
	 * the requests that have reached the game thread: one sent from another thread an instant earlier, whose hop here has
	 * not run yet, is not among them (IsActive() is already true for it); cancel that one by its handle.
	 */
	void CancelAll(FName InTag = NAME_None);

	// ---- configuration (game thread)
	/** The name traces show (Inspector IN-8). Without one, or after WithName(""), the client is named by its base URL. */
	FVaRestClient& WithName(FStringView DisplayName);
	FVaRestClient& WithBaseUrl(FStringView BaseUrl);
	/**
	 * A default header for every request; a request's Header() of the same name wins. A default Content-Type never relabels
	 * a body: the body's own type wins over it, and a request's Header("Content-Type", ...) over both (final review I2). A
	 * session header of the same name (SetSessionHeader) replaces both on each attempt, and the auth replaces all three.
	 */
	FVaRestClient& WithHeader(FStringView Name, FStringView Value);
	FVaRestClient& WithoutHeader(FStringView Name);
	/** How struct bodies name their fields (spec 2.4); decode accepts every spelling regardless */
	FVaRestClient& WithNaming(EVaRestJsonNaming Naming);
	FVaRestClient& WithTransport(TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Transport);
	/** Requests already sent keep the clock they started with; a rotated secret value keeps the time it has left (SetSecretValue) */
	FVaRestClient& WithClock(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock);
	/** Game thread: seeds the stream retry jitter draws from (plan decision 26), so a test knows each delay */
	FVaRestClient& WithRandomSeed(int32 Seed);
	/**
	 * Spec 4.1: the retry policy of this client's requests (a request's Retry()/NoRetry() replaces it). Without one a client
	 * does not retry (plan decision 1). Game thread; requests already sent keep the policy they started with.
	 */
	FVaRestClient& WithRetry(const FVaRestRetryPolicy& Policy);
	/**
	 * Spec 3.4: the envelope of this client's responses (a request's Envelope() or NoEnvelope() replaces it). Game thread;
	 * requests already sent keep the envelope they started with.
	 */
	FVaRestClient& WithEnvelope(const FVaRestEnvelope& InEnvelope);
	FVaRestClient& WithoutEnvelope();
	/** The largest response body parsed as JSON (decision 20); 0 = none. Default: UVaRestHttpSettings::MaxJsonBytes (32 MiB). */
	FVaRestClient& WithMaxJsonBytes(int64 Bytes);
	/**
	 * Spec 4.6: requests at once (at least 1); the rest wait in a queue, Highest first and first come, first served within a
	 * priority. Default: UVaRestHttpSettings::MaxConcurrent (16). Lowering it cancels nothing: the requests in flight
	 * finish, and the queue moves again once fewer than the new limit remain.
	 * The waits this adds:
	 * - The queue is strict: while higher-priority requests keep arriving, a Low one can wait indefinitely (review M-19).
	 * - A timeout does not cover the wait: it is per attempt and starts at the transport (spec 4.7), and a retry queues again.
	 *   A retry policy's Deadline does: the wait ends at it, Kind=Timeout (plan decision 10).
	 * - A queued request whose Owner() is gone leaves the queue only when a slot reaches it, and then ends silently; until
	 *   then it keeps its callbacks, everything they capture and this client alive. Cancel() takes a request out of the
	 *   queue at once.
	 * - The queue opens each attempt (ruling I-3): a PerAttempt stage's signature and the request's log lines are written
	 *   when it leaves the queue, and so are an auth's credentials and the session headers (AuthApply). Headers set with
	 *   WithHeader() are still the ones the request snapshotted at Send() (spec 3.1). A PerAttempt stage holds the slot
	 *   while it works; one that answers its attempt itself while the transport call still runs gives the slot back with
	 *   its answer, and the call it left runs on, uncounted, until the engine ends it.
	 * - An auth's refresh request (FVaRestAuthRefreshContext::Request) skips the queue: during a refresh up to
	 *   MaxConcurrent + 1 requests are out, so a client whose slots all hold long polls can still refresh (ruling M-8).
	 */
	FVaRestClient& WithMaxConcurrent(int32 MaxConcurrent);
	/**
	 * Spec 3.1 / 4.7: seconds per attempt; 0 = the engine's default ([HTTP] HttpTotalTimeout, HttpActivityTimeout). An
	 * activity of 0 with a total above 0 bounds silence by the total (decision 30). Under a retry policy's Deadline both are
	 * cut to the time left, and a total of 0 is the time left (plan decision 10).
	 */
	FVaRestClient& WithTimeout(float Total, float Activity = 0.f);
	/** Spec 4.5; the cvar VaRest.LogLevel (>= 0) overrides every client's */
	FVaRestClient& WithLogLevel(EVaRestLogLevel Level);
	/**
	 * Inspector IN-13 layer 2: more keys whose values are redacted in logs and traces, matched exactly after normalisation.
	 * A key holding '?', ';', '#', '=' or '&' is refused with a Warning (FVaRestRedactRules::AddKey).
	 */
	FVaRestClient& WithRedactedKeys(TConstArrayView<FString> Keys);
	/**
	 * Inspector IN-13 layer 3: a secret value (token, API key, session id) replaced by *** wherever it appears in a log
	 * line or a trace. Setting a slot again keeps the old value known for 10 minutes (Inspector D21). Values under 8 bytes
	 * are ignored. Game thread.
	 * These are the game's own slots. The session's values and an auth's are VaRest's, in internal slots (VaRest.*) kept
	 * apart from these: a game slot of any name, "VaRest.Session.AccessToken" included, neither replaces one of them nor
	 * is forgotten by ClearSession().
	 */
	void SetSecretValue(FName Slot, const FString& Value);
	/**
	 * Forgets every value SetSecretValue set, the rotated ones too. Game thread. The session's values (and an auth's) stay
	 * known while they are in use: a logout is ClearSession().
	 */
	void ClearSecretValues();
	/** A custom stage (spec 4): Outer sees each logical request once; PerAttempt every attempt, after the limiter's grant, with its final headers */
	FVaRestClient& Use(FVaRestStageRef Stage, EVaRestStageSlot Slot);

	// ---- session (spec 4.2; game thread). Live state: every attempt uses the session as it is when it starts (plan decision 6).
	// A listener of OnSessionChanged or OnSessionExpired may change the session again or drop this client: the events of one
	// change reach every listener in order, after the change is done (concurrency review C-1, I-2).
	/**
	 * The access token (a login, a token from elsewhere). Replaced, or Refreshed with bSameIdentity (plan decision 5); the
	 * refresh token stays, the expiry becomes unknown. An empty token is ClearSession(). A token holding a control character
	 * is refused with an Error log (it would go into a header line).
	 */
	void SetBearerToken(FStringView AccessToken, bool bSameIdentity = false);
	/**
	 * A login's result: both tokens and the access token's lifetime in seconds (0 = unknown), taken as the token's whole
	 * lifetime. A game restoring a session it saved itself uses ImportSession, which records the lifetime as unknown. Both
	 * tokens empty is ClearSession(), as SetBearerToken("") is. Either token holding a control character is refused with an
	 * Error log.
	 */
	void SetSession(FStringView AccessToken, FStringView RefreshToken, double ExpiresInSeconds = 0.0, bool bSameIdentity = false);
	/**
	 * dawg's X-Session-ID pattern: sent with every request whose auth mode is not None; its value is a secret, its name a
	 * key. It replaces a request's own header of the same name (Header(), WithHeader()) on each attempt, and the client's
	 * auth replaces both (an auth's Authorization over a session header named Authorization).
	 */
	void SetSessionHeader(FStringView Name, FStringView Value);
	void RemoveSessionHeader(FStringView Name);
	/** Logout: tokens, session headers and their secret values; OnSessionChanged(Cleared) */
	void ClearSession();
	FVaRestSessionData GetSession() const;
	/**
	 * Spec 4.2: the session as JSON, for a game that persists it itself. It holds the tokens in clear: store it like a
	 * password (the platform's keychain), never in a save file others can read. Written on the game thread (spec review
	 * M-12: the session JSON is the one game-thread parse, bounded).
	 */
	FString ExportSession() const;
	/**
	 * False, and nothing changes, when Json is not an ExportSession() result: not JSON, another version, a control character
	 * in a token or a header, a bad date, a field of the wrong JSON type (both tokens must be present strings; expiresAt a
	 * string if present; headers an array of {name, value} string objects if present), a header name twice, or more than 64 Ki
	 * characters (parsed on the game thread, so bounded; spec review M-12). Unknown fields are ignored.
	 * An import whose tokens are both explicitly "" is a logout, as SetSession("", "") is: ClearSession(), true.
	 * Otherwise it is SetSession (the lifetime unknown) plus the headers, and each part announces by its own rule: the
	 * tokens are Replaced, or Refreshed with bSameIdentity, and no event at all when they and the expiry are unchanged; the
	 * headers announce nothing, as SetSessionHeader announces nothing (the headers an import drops are retired). So a game
	 * importing the same tokens with other headers sees the headers change and no OnSessionChanged.
	 */
	bool ImportSession(FStringView Json, bool bSameIdentity = false);
	/** WebSocket spec 19 item 2: a socket's terminal auth failure. The tokens go; OnSessionExpired fires, once per expiry. */
	void NotifySessionExpired();
	/** Game thread. An empty client returns a delegate that never fires. */
	FVaRestSessionChangedEvent& OnSessionChanged();
	FVaRestSessionExpiredEvent& OnSessionExpired();

	// ---- auth (spec 4.2; game thread)
	/**
	 * This client's auth: FVaRestAuth::Bearer(), ApiKey(), Basic(), BearerRefresh(), or an IVaRestAuth of your own. Requests
	 * already sent keep the auth they started with (spec 3.1); the session they apply is the live one (plan decision 6). The
	 * secret values the auth it replaces held and this one does not are retired: still scrubbed for 10 minutes, then
	 * forgotten (Inspector D21). ClearSession() and ClearSecretValues() leave an auth's values alone.
	 */
	FVaRestClient& WithAuth(TSharedRef<IVaRestAuth, ESPMode::ThreadSafe> InAuth);
	/** No auth: requests carry the session headers only. The auth's secret values are retired, as WithAuth retires them. */
	FVaRestClient& WithoutAuth();
	/** Spec 3.1: WithAuth(FVaRestAuth::Bearer()) and SetBearerToken(InToken) */
	FVaRestClient& WithBearer(FStringView InToken);
	/**
	 * WebSocket spec 19 item 2: the session headers, then this client's auth, into OutHeaders and OutQuery (a socket
	 * handshake). False when the client has an auth and it has no credentials (a socket's Kind=Auth), or when the auth is
	 * misconfigured (ruling Q7): then OutConfigError, when given, says why (scrubbed of this client's known secrets), and the
	 * socket fails Kind=Config. True with no auth at all. OutConfigError is emptied first.
	 * Every value this writes is a secret and every name a redaction key (credentials and session-header values): the core
	 * records nothing for the caller, so the caller (the socket, W5) keeps them redacted in its own lines and traces.
	 * A false for missing credentials may leave the session headers in OutHeaders already.
	 */
	bool ApplyAuth(FVaRestHeaders& OutHeaders, TArray<TPair<FString, FString>>& OutQuery, FString* OutConfigError = nullptr) const;

	// ---- requests (any thread)
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Get(FStringView Path) const { return Request<T>(TEXTVIEW("GET"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Post(FStringView Path) const { return Request<T>(TEXTVIEW("POST"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Put(FStringView Path) const { return Request<T>(TEXTVIEW("PUT"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Patch(FStringView Path) const { return Request<T>(TEXTVIEW("PATCH"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Delete(FStringView Path) const { return Request<T>(TEXTVIEW("DELETE"), Path); }
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Head(FStringView Path) const { return Request<T>(TEXTVIEW("HEAD"), Path); }

	/**
	 * Any verb, an RFC 9110 token taken as it is (no trimming): anything else, an empty verb included, fails the request
	 * with Kind=Config and nothing is sent (final review I3). T defaults to FVaRestJson (spec 3.2). An empty client gives
	 * a builder whose Send() logs an error, sends nothing and delivers Kind=Config to OnError and OnComplete from a later
	 * game-thread tick (final review M7; decision 3).
	 * The handle that Send() returns is empty: IsActive() is false and
	 * Cancel() does nothing, also while that delivery is still queued (bead VaRest-3dj).
	 *
	 * A body is sent only with POST, PUT, PATCH and DELETE on the engine's curl transport (Linux, Windows, Android): only
	 * those attach curl's read function (CurlHttp.cpp:941-968, :979-988, :995-1001). GET drops its body with a Warning
	 * (spec 3.2). A body on any other verb -- HEAD (CURLOPT_NOBODY, :975-978), OPTIONS, QUERY, PROPFIND -- fails the request
	 * with Kind=Config before anything is sent, on every platform (plan decision 13; IVaRestTransport::SendsBodyFor).
	 */
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const
	{
		TUniquePtr<VaRest::Http::FRequestSpec> NewSpec(new VaRest::Http::FRequestSpec(State, Verb, Path));
		NewSpec->Handler = MakeUnique<VaRest::Http::TResultHandler<T>>();
		return TVaRestRequest<T>(MoveTemp(NewSpec));
	}

private:
	/** Defined in VaRestClientState.h (Private): VaRestHttp and its tests reach the state through it */
	friend struct VaRest::Http::FClientAccess;

	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> GetStateInternal() const { return State; }

	explicit FVaRestClient(TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State;
};
