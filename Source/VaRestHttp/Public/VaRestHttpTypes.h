// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"

#include "VaRestHttpTypes.generated.h"

/** Limiter queue order (spec 4.6): Highest first, FIFO within one priority. Strict: under steady higher-priority load a Low request can wait indefinitely. */
UENUM(BlueprintType)
enum class EVaRestPriority : uint8
{
	Highest,
	High,
	Normal,
	Low
};

/** Spec 4.5 */
UENUM(BlueprintType)
enum class EVaRestLogLevel : uint8
{
	None,
	/** Verb, redacted URL, status, milliseconds, bytes */
	Basic,
	/** Basic plus the redacted request and response headers */
	Headers,
	/** Headers plus redacted body excerpts of up to 4 KiB; compiled out in Shipping, where it logs as Headers */
	Body
};

/** Spec 3.3 */
UENUM(BlueprintType)
enum class EVaRestErrorKind : uint8
{
	/** No error */
	None,
	Canceled,
	/** No response: the server could not be reached, the connection broke, or no data arrived for the activity timeout */
	Connection,
	/** The total timeout passed */
	Timeout,
	/** A non-2xx status */
	Http,
	/** 2xx, but the envelope says failure (M4) */
	Server,
	/** The response could not be turned into the result type */
	Decode,
	/** Refresh failed or no credentials (M4) */
	Auth,
	/** No connectivity, not queued, or a queued item expired (M4) */
	Offline,
	/** The request is invalid: a bad URL, path parameter, header or body */
	Config
};

/**
 * OpenAPI N9: how an array-valued query parameter is written (decision 34). Repeat: tag=a&tag=b; the others one pair,
 * the values joined before encoding: Csv "a,b", Ssv "a b", Pipes "a|b". A UENUM because M5's nodes take it.
 */
UENUM(BlueprintType)
enum class EVaRestQueryStyle : uint8
{
	Repeat,
	Csv,
	Ssv,
	Pipes
};

/** Where a custom stage goes (spec 4) */
enum class EVaRestStageSlot : uint8
{
	/** Sees each logical request once, before the attempt boundary */
	Outer,
	/** Sees every attempt and its final headers (request signing) */
	PerAttempt
};

/** Why an attempt got no usable response (Inspector IN-3 Failure) */
enum class EVaRestTransportFailure : uint8
{
	None,
	Connection,
	Timeout,
	Canceled,
	Other
};

/** Inspector IN-5 CancelReason */
enum class EVaRestCancelReason : uint8
{
	None,
	/** FVaRestRequestHandle::Cancel or FVaRestClient::CancelAll: the callbacks get Kind=Canceled */
	User,
	/** The request's owner was destroyed: no callbacks (spec 3.2) */
	OwnerGone,
	/** The engine is exiting: no callbacks */
	Shutdown
};

/**
 * HTTP headers in order. Names compare ignoring case (RFC 9110 5.1) and may repeat (Set-Cookie). Not thread-safe: copy
 * it to hand it to another thread.
 */
class VARESTHTTP_API FVaRestHeaders
{
public:
	using FPair = TPair<FString, FString>;

	/** One Name: Value, replacing every header of that name; it takes the first one's position */
	void Set(FStringView Name, FStringView Value);
	/** Another Name: Value after the existing ones */
	void Add(FStringView Name, FStringView Value);
	/** Removes every header of that name; false when there was none */
	bool Remove(FStringView Name);
	/** The first value of Name, or null */
	const FString* Find(FStringView Name) const;
	/** The first value of Name, or "" */
	FString Get(FStringView Name) const;
	bool Contains(FStringView Name) const { return Find(Name) != nullptr; }
	int32 Num() const { return Pairs.Num(); }
	bool IsEmpty() const { return Pairs.Num() == 0; }
	const TArray<FPair>& GetPairs() const { return Pairs; }
	auto begin() const { return Pairs.begin(); }
	auto end() const { return Pairs.end(); }

	/** Parses the engine's "Name: Value" lines (IHttpBase::GetAllHeaders, IHttpBase.h:140-145); a line without ':' is skipped */
	static FVaRestHeaders FromLines(TConstArrayView<FString> Lines);

private:
	TArray<FPair> Pairs;
};

/** Spec 3.3. A USTRUCT because M5 hands it to Blueprints. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestError
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	EVaRestErrorKind Kind = EVaRestErrorKind::None;

	/** 0 when there was no response */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int32 HttpStatus = 0;

	/** From the envelope (M4) or the transport; may be empty */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Code;

	/** Human-readable; never empty for an error */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Message;

	/** The parsed error body when it is JSON, else an invalid handle */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FVaRestJson Body;

	/** The first 4 KiB of a body that is not JSON, as text */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString RawBody;

	/** Decode errors: the JSON path of the value that did not fit ("items[3].level") */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Path;

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int32 Attempts = 0;

	/** A connection error, a timeout, or status 408/425/429/500/502/503/504 (spec 4.1) */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	bool bRetryable = false;

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FName Tag;

	/** The request URL, redacted */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Url;

	bool IsError() const { return Kind != EVaRestErrorKind::None; }

	/** "Http 503 [E503]: message at path (url)", the parts that are set */
	FString Describe() const;

	/** The human-readable fallback for a kind ("HTTP 404 Not Found" for Http) */
	static FString DefaultMessage(EVaRestErrorKind ForKind, int32 Status);

	/** Spec 4.1's retryable statuses: 408, 425, 429, 500, 502, 503, 504 */
	static bool IsRetryableStatus(int32 Status);
};

/** Decision 8: one total per direction */
struct FVaRestProgress
{
	int64 BytesSent = 0;
	int64 BytesReceived = 0;
	/** The request body's size; 0 without one */
	int64 UploadTotal = 0;
	/** The response's Content-Length; -1 when it is unknown (chunked) */
	int64 DownloadTotal = -1;

	bool operator==(const FVaRestProgress& Other) const = default;
};

/** Spec 3.3: the metadata of a response, and its raw bytes per spec 3.2's release rule */
struct FVaRestResponse
{
	uint64 RequestId = 0;
	/** 0 when there was no response */
	int32 Status = 0;
	FVaRestHeaders Headers;
	FString EffectiveUrl;
	FString ContentType;
	/** Seconds from entering the pipeline to the result */
	double Elapsed = 0.0;
	int32 Attempts = 0;
	bool bFromCache = false;
	/**
	 * Raw bytes (spec 3.2's release rule, decision 26): kept for raw results, with KeepBody(), and on an error that left
	 * them intact (an HTTP error, a non-JSON Content-Type, a body over MaxJsonBytes refused before parsing). A JSON body
	 * is parsed in place (zero copy), so after a syntax error or a struct mismatch the bytes are gone: the error keeps
	 * the first 4 KiB as RawBody (copied before the parse) and, for a mismatch, the parsed DOM as Body. KeepBody() keeps
	 * all of it (a copy before the parse). Released after a successful decode.
	 */
	TArray<uint8> Body;
	/** SaveTo(): where the body was written */
	FString SavedFile;
	/** What a tolerant struct decode noticed, each with its JSON path (spec 2.4) */
	TArray<FVaRestJsonError> DecodeWarnings;
	int64 BytesUp = 0;
	int64 BytesDown = 0;
};

/** Spec 3.3: a value or an error, plus the response */
template <typename T>
struct TVaRestResult
{
	TOptional<T> Value;
	FVaRestError Error;
	FVaRestResponse Response;

	bool IsSuccess() const { return !Error.IsError(); }
	const T& GetValue() const { return Value.GetValue(); }
};

template <>
struct TVaRestResult<void>
{
	FVaRestError Error;
	FVaRestResponse Response;

	bool IsSuccess() const { return !Error.IsError(); }
};

VARESTHTTP_API const TCHAR* LexToString(EVaRestErrorKind Kind);
VARESTHTTP_API const TCHAR* LexToString(EVaRestTransportFailure Failure);
VARESTHTTP_API const TCHAR* LexToString(EVaRestLogLevel Level);

namespace VaRest::Http
{
	/** application/json, or any type ending in +json (RFC 6839); case, spaces and parameters ignored (spec 3.2) */
	VARESTHTTP_API bool IsJsonContentType(FStringView ContentType);

	inline bool IsSuccessStatus(int32 Status)
	{
		return Status >= 200 && Status <= 299;
	}
}
