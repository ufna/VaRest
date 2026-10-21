// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Misc/SourceLocation.h"
#include "UObject/WeakObjectPtr.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonConvert.h"
#include "VaRestResultHandler.h"
#include "VaRestUrl.h"
#include "VaRestStage.h"

#include <type_traits>

class FVaRestRequestHandle;

namespace VaRest::Http
{
	/** What a TVaRestRequest collects; Send moves it into an exchange */
	struct VARESTHTTP_API FRequestSpec
	{
		FRequestSpec(TSharedPtr<FClientState, ESPMode::ThreadSafe> InState, FStringView InVerb, FStringView InPath);
		~FRequestSpec();
		FRequestSpec(const FRequestSpec&) = delete;
		FRequestSpec& operator=(const FRequestSpec&) = delete;

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
		bool bStructBodySet = false;
		TUniquePtr<IResultHandler> Handler;
		bool bSent = false;
	};

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
	/** Any thread: sent and not finished */
	bool IsActive() const;

private:
	TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe> Exchange;
	uint64 Id = 0;
};

/**
 * Spec 3.2: one request, built fluently. Every method returns the builder by value (a moved handle, cheap), so the
 * class-level [[nodiscard]] makes a forgotten Send() a compile warning; the methods are && so a builder is used once.
 * Any thread.
 */
template <typename T>
class [[nodiscard]] TVaRestRequest
{
public:
	explicit TVaRestRequest(TUniquePtr<VaRest::Http::FRequestSpec>&& InSpec)
		: Spec(MoveTemp(InSpec))
	{
	}

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
	 * Sets a header (replacing one of that name, whatever its case); it wins over the client's and over a body's
	 * Content-Type. A name that is not an RFC 9110 token, or a value holding a control character other than HTAB
	 * (CR, LF, NUL among them), fails the request with Kind=Config and nothing is sent (decision 11: header injection).
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
		}
		return MoveTemp(*this);
	}

	/** text/plain; charset=utf-8 */
	TVaRestRequest Body(FStringView Text) &&
	{
		FTCHARToUTF8 Utf8(Text.GetData(), Text.Len());
		Spec->Body = FVaRestRequestBody::FromBytes(TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length()), EVaRestBodyKind::Text, TEXT("text/plain; charset=utf-8"));
		return MoveTemp(*this);
	}

	/** application/octet-stream, moved: no copy until the transport (spec 2.4) */
	TVaRestRequest Body(TArray<uint8>&& Bytes) &&
	{
		Spec->Body = FVaRestRequestBody::FromBytes(MoveTemp(Bytes), EVaRestBodyKind::Binary, TEXT("application/octet-stream"));
		return MoveTemp(*this);
	}

	/** application/octet-stream, copied */
	TVaRestRequest Body(TConstArrayView<uint8> Bytes) &&
	{
		return MoveTemp(*this).Body(TArray<uint8>(Bytes.GetData(), Bytes.Num()));
	}

	/**
	 * A USTRUCT, encoded straight to UTF-8 bytes (spec 2.4) with the naming policy known now: the request's Naming(),
	 * else the client's. Call Naming() first; after a struct body it fails the request.
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
		}
		return MoveTemp(*this);
	}

	/** A file streamed while uploading (IHttpRequest::SetContentAsStreamedFile). Its size is read now; a missing file fails the request. */
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

	TVaRestRequest Tag(FName InTag) &&
	{
		Spec->Tag = InTag;
		return MoveTemp(*this);
	}

	/** The limiter's queue order (spec 4.6) */
	TVaRestRequest Priority(EVaRestPriority InPriority) &&
	{
		Spec->Priority = InPriority;
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
	/** An empty handle; Make() a real one */
	FVaRestClient() = default;

	/** Game thread. Defaults from UVaRestHttpSettings; the FHttpModule transport; the real clock. */
	static FVaRestClient Make(FStringView BaseUrl);

	bool IsValid() const { return State.IsValid(); }
	uint64 GetId() const;
	FString GetBaseUrl() const;
	/** Game thread */
	int32 GetInFlightCount() const;

	// ---- configuration (game thread)
	FVaRestClient& WithName(FStringView DisplayName);
	FVaRestClient& WithBaseUrl(FStringView BaseUrl);
	FVaRestClient& WithHeader(FStringView Name, FStringView Value);
	FVaRestClient& WithoutHeader(FStringView Name);
	/** How struct bodies name their fields (spec 2.4); decode accepts every spelling regardless */
	FVaRestClient& WithNaming(EVaRestJsonNaming Naming);
	FVaRestClient& WithTransport(TSharedRef<IVaRestTransport, ESPMode::ThreadSafe> Transport);
	FVaRestClient& WithClock(TSharedRef<IVaRestClock, ESPMode::ThreadSafe> Clock);
	/** A custom stage (spec 4): Outer sees each logical request once, PerAttempt every attempt with its final headers */
	FVaRestClient& Use(FVaRestStageRef Stage, EVaRestStageSlot Slot);

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

	/** Any verb; T defaults to FVaRestJson (spec 3.2). An empty client gives a builder whose Send() logs an error and sends nothing. */
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const
	{
		TUniquePtr<VaRest::Http::FRequestSpec> NewSpec = MakeUnique<VaRest::Http::FRequestSpec>(State, Verb, Path);
		NewSpec->Handler = MakeUnique<VaRest::Http::TResultHandler<T>>();
		return TVaRestRequest<T>(MoveTemp(NewSpec));
	}

	/** VaRestHttp and its tests; not a stable API */
	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> GetStateInternal() const { return State; }

private:
	explicit FVaRestClient(TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> InState)
		: State(MoveTemp(InState))
	{
	}

	TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State;
};
