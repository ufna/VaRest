// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestClock.h"
#include "VaRestEnvelope.h"
#include "VaRestHttpTypes.h"
#include "VaRestJson.h"
#include "VaRestJsonConvert.h"
#include "VaRestTrace.h"

#include <type_traits>

namespace VaRest::Http
{
	/** A response on its way to the callbacks: built on the game thread, decoded on a background task, delivered on the game thread (spec 1) */
	struct FFinish
	{
		FVaRestResponse Response;
		/** Kind None: a success so far */
		FVaRestError Error;
		/** The response body with its byte of slack; ParseOwned takes it (spec 2.1) */
		TArray<uint8> Body;
		/** The body went to TempFile (SaveTo) */
		bool bBodyInFile = false;
		FString TempFile;
		FString SaveTo;
		bool bKeepBody = false;
		FVaRestJsonConvertOptions DecodeOptions;
		/** MaxInputBytes = the request's or the client's MaxJsonBytes (decision 20) */
		FVaRestJsonParseOptions ParseOptions;
		/** The trace mask was set at the finish: decode times are recorded (IN-5; IN-10: clock reads follow the mask) */
		bool bTimeDecode = false;
		double DecodeStartAt = -1.0;
		double DecodeEndAt = -1.0;
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock;
		/** Spec 3.4: applied on the decode task (plan decision 9); null = none */
		TSharedPtr<const FVaRestEnvelope, ESPMode::ThreadSafe> Envelope;
		/** IN-5: what the envelope did, for Completed (unscrubbed here; the trace point scrubs it). Filled only when bTimeDecode (IN-11). */
		FVaRestTraceEnvelope EnvelopeTrace;
		/** The envelope's DataPath named nothing: a struct result fails (plan decision 9) and keeps EnvelopeBody as its error body */
		bool bEnvelopeDataMissing = false;
		FVaRestJson EnvelopeBody;
		/** The request's effective log level: the decode task's one line (a void predicate it could not check, spec review M-13) */
		EVaRestLogLevel LogLevel = EVaRestLogLevel::None;
	};

	/** The typed half of a request: one result type's decode and callbacks (TResultHandler<T>) */
	class IResultHandler
	{
	public:
		virtual ~IResultHandler() = default;

		/** For traces (Inspector IN-5 ResultType) */
		virtual FName GetResultType() const = 0;
		/** A 2xx body needs decoding off the game thread: JSON, a struct, text */
		virtual bool DecodesBody() const = 0;
		/** The body is parsed as JSON (FVaRestJson, a struct): the transport applies MaxJsonBytes to it (reviews M-6, M13) */
		virtual bool DecodesJson() const = 0;
		/** Spec 3.4 / plan decision 9: FVaRestJson and struct results unwrap; void ones check the predicate; raw ones are left alone */
		virtual bool AppliesEnvelope() const = 0;
		/**
		 * A 2xx response into the typed value; a failure sets Finish.Error (Kind Decode, or Kind Server from the envelope). A
		 * background task when DecodesBody(), or for a void result whose envelope has a predicate and a body to judge; else
		 * the game thread.
		 */
		virtual void Decode(FFinish& Finish) = 0;
		/** OnSuccess or OnError, then OnComplete. The game thread, or a background task with OnHttpThread(). */
		virtual void Deliver(FFinish& Finish) = 0;
		virtual bool WantsProgress() const = 0;
		/** Game thread */
		virtual void Progress(const FVaRestProgress& Snapshot) = 0;
	};

	/**
	 * The body as JSON. 204 or an empty body is an invalid handle and a success (spec 3.2: "204/empty -> T default"). A
	 * Content-Type that is not JSON is a Decode error (decision 6: a missing one is parsed). ParseOwned takes the body
	 * in situ, zero-copy thanks to its slack (spec 2.1); KeepBody() copies it first. Background thread.
	 */
	VARESTHTTP_API bool DecodeJson(FFinish& Finish, FVaRestJson& Out);

	/**
	 * Plan decision 9: a void result's envelope predicate -- a JSON body (by type, or sniffed without one) is parsed (a copy;
	 * void keeps no value) and a failed predicate sets a Kind=Server error. Nothing to judge without a predicate, without a
	 * body, for 204, for a body in a SaveTo file, or for a body that is not JSON. Background thread.
	 */
	VARESTHTTP_API void CheckEnvelopePredicate(FFinish& Finish);

	/**
	 * DecodeJson, then FVaRestJsonConvert::JsonToStruct into Data, an initialised instance of Struct. Warnings land in
	 * Response.DecodeWarnings; a failure is a Decode error with the first issue's JSON path. Background thread. Struct
	 * must stay alive and unchanged until this returns: a native struct always does; a caller passing a Blueprint
	 * UUserDefinedStruct (M5's FInstancedStruct carrier) must hold it across garbage collection for the whole decode
	 * (TStrongObjectPtr or an FGCObject) and decode into memory its task owns (decision 22).
	 */
	VARESTHTTP_API bool DecodeStruct(FFinish& Finish, const UScriptStruct* Struct, void* Data);

	/** UTF-8 to text, an ill-formed sequence as U+FFFD (spec 2.2); the raw bytes released per spec 3.2 */
	VARESTHTTP_API void DecodeText(FFinish& Finish, FString& Out);

	/** Spec 3.2: raw bytes are released after a successful decode, unless KeepBody() asked to keep them */
	VARESTHTTP_API void ReleaseBody(FFinish& Finish);

	template <typename T>
	inline constexpr bool bIsSupportedResult = std::is_same_v<T, FVaRestJson> || std::is_same_v<T, FString> || std::is_same_v<T, TArray<uint8>>
		|| std::is_same_v<T, FVaRestResponse> || VaRest::Json::bHasScriptStruct<T>;

	template <typename T>
	class TResultHandler final : public IResultHandler
	{
		static_assert(bIsSupportedResult<T>, "TVaRestRequest<T>: T is FVaRestJson (the default), void, FString, TArray<uint8>, FVaRestResponse or a USTRUCT (spec 3.2)");
		static constexpr bool bIsStruct = !std::is_same_v<T, FVaRestJson> && VaRest::Json::bHasScriptStruct<T>;

	public:
		TResultHandler()
		{
			if constexpr (bIsStruct)
			{
				Struct = VaRest::Json::GetScriptStruct<T>();
			}
		}

		TUniqueFunction<void(const T&, const FVaRestResponse&)> OnSuccess;
		TUniqueFunction<void(const FVaRestError&)> OnError;
		TUniqueFunction<void(const TVaRestResult<T>&)> OnComplete;
		TUniqueFunction<void(const FVaRestProgress&)> OnProgress;

		virtual FName GetResultType() const override
		{
			if constexpr (std::is_same_v<T, FVaRestJson>)
			{
				return FName(TEXT("Json"));
			}
			else if constexpr (std::is_same_v<T, FString>)
			{
				return FName(TEXT("String"));
			}
			else if constexpr (std::is_same_v<T, TArray<uint8>>)
			{
				return FName(TEXT("Bytes"));
			}
			else if constexpr (std::is_same_v<T, FVaRestResponse>)
			{
				return FName(TEXT("Response"));
			}
			else
			{
				return Struct->GetFName();
			}
		}

		virtual bool DecodesBody() const override
		{
			return !std::is_same_v<T, TArray<uint8>> && !std::is_same_v<T, FVaRestResponse>;
		}

		virtual bool DecodesJson() const override
		{
			return DecodesBody() && !std::is_same_v<T, FString>;
		}

		virtual bool AppliesEnvelope() const override
		{
			return std::is_same_v<T, FVaRestJson> || bIsStruct;
		}

		virtual void Decode(FFinish& Finish) override
		{
			if constexpr (std::is_same_v<T, FVaRestJson>)
			{
				FVaRestJson Json;
				if (DecodeJson(Finish, Json))
				{
					Value.Emplace(MoveTemp(Json));
				}
			}
			else if constexpr (std::is_same_v<T, FString>)
			{
				FString Text;
				DecodeText(Finish, Text);
				Value.Emplace(MoveTemp(Text));
			}
			else if constexpr (std::is_same_v<T, TArray<uint8>>)
			{
				Value.Emplace(MoveTemp(Finish.Body));
			}
			else if constexpr (std::is_same_v<T, FVaRestResponse>)
			{
				FVaRestResponse Raw = Finish.Response;
				Raw.Body = MoveTemp(Finish.Body);
				Value.Emplace(MoveTemp(Raw));
			}
			else
			{
				// Built the way reflection builds it, as FVaRestJsonConvert::FromJson does, so a field the JSON lacks keeps its default
				alignas(T) uint8 Storage[sizeof(T)];
				Struct->InitializeStruct(Storage);
				if (DecodeStruct(Finish, Struct, Storage))
				{
					Value.Emplace(MoveTemp(*reinterpret_cast<T*>(Storage)));
				}
				Struct->DestroyStruct(Storage);
			}
		}

		virtual void Deliver(FFinish& Finish) override
		{
			if (Finish.Error.IsError())
			{
				Value.Reset();
				if (OnError)
				{
					OnError(Finish.Error);
				}
			}
			else if (OnSuccess && Value.IsSet())
			{
				OnSuccess(Value.GetValue(), Finish.Response);
			}
			if (OnComplete)
			{
				TVaRestResult<T> Result;
				Result.Value = MoveTemp(Value);
				Result.Error = Finish.Error;
				Result.Response = MoveTemp(Finish.Response);
				OnComplete(Result);
			}
		}

		virtual bool WantsProgress() const override
		{
			return static_cast<bool>(OnProgress);
		}

		virtual void Progress(const FVaRestProgress& Snapshot) override
		{
			if (OnProgress)
			{
				OnProgress(Snapshot);
			}
		}

	private:
		UScriptStruct* Struct = nullptr;
		TOptional<T> Value;
	};

	template <>
	class TResultHandler<void> final : public IResultHandler
	{
	public:
		TUniqueFunction<void(const FVaRestResponse&)> OnSuccess;
		TUniqueFunction<void(const FVaRestError&)> OnError;
		TUniqueFunction<void(const TVaRestResult<void>&)> OnComplete;
		TUniqueFunction<void(const FVaRestProgress&)> OnProgress;

		virtual FName GetResultType() const override { return NAME_None; }
		virtual bool DecodesBody() const override { return false; }
		virtual bool DecodesJson() const override { return false; }
		virtual bool AppliesEnvelope() const override { return true; }

		virtual void Decode(FFinish& Finish) override
		{
			CheckEnvelopePredicate(Finish);
			if (Finish.Error.IsError())
			{
				// Spec 3.2: raw bytes "are kept on error for diagnostics"; the predicate parsed a copy, so they are whole
				Finish.Response.Body = MoveTemp(Finish.Body);
				return;
			}
			ReleaseBody(Finish);
		}

		virtual void Deliver(FFinish& Finish) override
		{
			if (Finish.Error.IsError())
			{
				if (OnError)
				{
					OnError(Finish.Error);
				}
			}
			else if (OnSuccess)
			{
				OnSuccess(Finish.Response);
			}
			if (OnComplete)
			{
				TVaRestResult<void> Result;
				Result.Error = Finish.Error;
				Result.Response = MoveTemp(Finish.Response);
				OnComplete(Result);
			}
		}

		virtual bool WantsProgress() const override { return static_cast<bool>(OnProgress); }

		virtual void Progress(const FVaRestProgress& Snapshot) override
		{
			if (OnProgress)
			{
				OnProgress(Snapshot);
			}
		}
	};
}
