// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Templates/ValueOrError.h"
#include "VaRestApiTypes.h"
#include "VaRestBody.h"
#include "VaRestOffline.h"

#include "VaRestHttpBlueprintTypes.generated.h"

struct FVaRestAuthTarget;

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestDecodeIssue
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Path;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString Message;
};

/** An owned reflected response copy. Raw bodies are never shortened into a preview. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestResponseData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString RequestId = TEXT("0");
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int32 Status = 0;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	TArray<FVaRestHeader> Headers;
	/** Native effective URL, which may contain credentials. Redact before displaying/logging it. */
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString EffectiveUrl;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString ContentType;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	double Elapsed = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int32 Attempts = 0;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	bool bFromCache = false;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	TArray<uint8> Body;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	FString SavedFile;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	TArray<FVaRestDecodeIssue> DecodeWarnings;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int64 BytesUp = 0;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Http")
	int64 BytesDown = 0;

	static FVaRestResponseData FromNative(const FVaRestResponse& Response);
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestQueuedResultData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Offline")
	FString QueueId;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Offline")
	FName Tag;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Offline")
	bool bSuccess = false;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Offline")
	FVaRestResponseData Response;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Offline")
	FVaRestError Error;

	static FVaRestQueuedResultData FromNative(const FVaRestQueuedResult& Result);
};

/** A view for auth callbacks, not a trusted serialized-session import record. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestSessionDataView
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FGuid IdentityId;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FString AccessToken;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FString RefreshToken;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	double ExpiresAt = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	double Lifetime = 0.0;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	TArray<FVaRestHeader> Headers;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FString Generation = TEXT("0");
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FString CredentialGeneration = TEXT("0");

	static FVaRestSessionDataView FromNative(const FVaRestSessionData& Session);
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestAuthTargetData
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FVaRestSessionDataView Session;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	TArray<FVaRestHeader> Headers;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Auth")
	TArray<FVaRestQueryParameter> Query;
	UPROPERTY(BlueprintReadOnly, Category = "VaRest|Auth")
	FString RequestId = TEXT("0");

	static FVaRestAuthTargetData FromNative(const FVaRestAuthTarget& Target);
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestMultipartPartData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString FileName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ContentType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString FieldValue;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<uint8> Bytes;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString FilePath;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	bool bField = false;
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestMultipartData
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<FVaRestMultipartPartData> Parts;

	/** Validates exclusive sources before delegating to the real native multipart/file validation. */
	TValueOrError<FVaRestMultipart, FString> ToNative() const;
};

namespace VaRest::Http
{
/** Canonical unsigned decimal, with no narrowing through a signed integer or double. */
VARESTHTTP_API FString UnsignedIdText(uint64 Value);
/** Failure preserves OutValue. Rejects signs, whitespace, leading zeros and overflow. */
VARESTHTTP_API bool TryParseUnsignedId(FStringView Text, uint64& OutValue);
VARESTHTTP_API bool TryParseGeneration(FStringView Text, uint32& OutValue);
} // namespace VaRest::Http
