// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestCache.h"
#include "VaRestEnvelope.h"
#include "VaRestJsonConvert.h"
#include "VaRestRetry.h"

#include "VaRestApiTypes.generated.h"

enum class EVaRestBodyKind : uint8;

UENUM(BlueprintType)
enum class EVaRestTypeKind : uint8
{
	None,
	Json,
	Struct,
	String,
	Bytes,
	Bool,
	Int,
	Int64,
	Double,
	Enum,
	DateTime
};

UENUM(BlueprintType)
enum class EVaRestTypeContainer : uint8
{
	None,
	Array,
	Map
};

/** Asset body selection. Native EVaRestBodyKind and its serialized values remain unchanged. */
UENUM(BlueprintType)
enum class EVaRestEndpointBodyKind : uint8
{
	Json,
	Form,
	Multipart,
	Binary,
	Text
};

UENUM(BlueprintType)
enum class EVaRestEndpointParamLocation : uint8
{
	Path,
	Query,
	Header
};

/** Runtime validation data; asset/editor adapters can display it without a runtime editor dependency. */
struct FVaRestApiValidationIssue
{
	FName Code;
	FString PropertyPath;
	FText Message;
	bool bWarning = false;
};

struct VARESTHTTP_API FVaRestApiValidationContext
{
	TArray<FVaRestApiValidationIssue> Issues;
	void Error(FName Code, FStringView PropertyPath, FText Message);
	void Warning(FName Code, FStringView PropertyPath, FText Message);
	bool HasErrors() const;
};

/** One value type and, optionally, one Array or FString-keyed Map. Nested containers are not descriptors. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestTypeRef
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Type")
	EVaRestTypeKind Kind = EVaRestTypeKind::None;

	/** UScriptStruct for Struct, UEnum for Enum; null for every other kind (including DateTime). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Type", meta = (AllowedClasses = "/Script/CoreUObject.ScriptStruct,/Script/CoreUObject.Enum"))
	TObjectPtr<UObject> Type = nullptr;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Type")
	EVaRestTypeContainer Container = EVaRestTypeContainer::None;

	/** String/Bytes only, without another container: decode a JSON string/base64 value instead of a raw HTTP body. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Type")
	bool bJsonValue = false;

	static FVaRestTypeRef Json();
	/** bParameter restricts the descriptor to scalar/enum or one array, with no maps/body-only kinds. */
	bool Validate(FVaRestApiValidationContext& Context, FStringView PropertyPath, bool bParameter = false) const;
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestHeader
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString Value;
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestQueryParameter
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString Value;
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestApiEnvironment
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FName Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString BaseUrl;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString SocketBaseUrl;
	/** Explicit isolation name; empty uses the complete environment name under the M5 durable-name contract. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString QueueNamespace;
};

/** Runtime JSON naming/shape rules remain in the cooked API asset; import state is separate editor-only data. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestApiJsonTypeRules
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	TObjectPtr<UField> Type = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	TArray<FVaRestJsonFieldRule> Fields;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|JSON")
	TArray<FVaRestJsonEnumRule> Enums;
};

/** Opaque importer payload; only UVaRestApi's WITH_EDITORONLY_DATA field stores it. */
USTRUCT()
struct VARESTHTTP_API FVaRestApiImportState
{
	GENERATED_BODY()

	UPROPERTY()
	FName Importer;
	UPROPERTY()
	int32 Version = 0;
	UPROPERTY()
	TArray<uint8> Blob;
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestEndpointParam
{
	GENERATED_BODY()

	/** Exact case-sensitive wire spelling; never replaced with the FName pin identity. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FName PinName;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	EVaRestEndpointParamLocation In = EVaRestEndpointParamLocation::Query;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FVaRestTypeRef Type;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	bool bRequired = false;
	/** Scalar K2 literal. Arrays use a JSON array of scalar literals; enum elements use internal enumerator names. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Default;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	EVaRestQueryStyle Style = EVaRestQueryStyle::Repeat;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Description;
	/** Retain the pin for reconstruction, but never transmit this parameter. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	bool bRemoved = false;

	void Validate(FVaRestApiValidationContext& Context, FStringView PropertyPath) const;
};

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestApiEndpoint
{
	GENERATED_BODY()

	FVaRestApiEndpoint();

	/** Random identity assigned once; operation renames retain it. */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "VaRest|API", meta = (IgnoreForMemberInitializationTest))
	FGuid Id;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FName Name;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Verb = TEXT("GET");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Path;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Category;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString Description;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	bool bDeprecated = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API", meta = (EditCondition = "bDeprecated"))
	FString DeprecationMessage;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	TArray<FVaRestQueryParameter> DefaultQuery;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	TArray<FVaRestHeader> Headers;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	TArray<FVaRestEndpointParam> Params;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FVaRestTypeRef RequestType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	EVaRestEndpointBodyKind BodyKind = EVaRestEndpointBodyKind::Json;
	/** Multipart boundaries are generated by the native multipart encoder, never authored here. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FString BodyContentType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FVaRestTypeRef ResponseType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	FVaRestTypeRef ErrorType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	EVaRestAuthMode AuthMode = EVaRestAuthMode::Required;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	bool bIdempotent = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|API")
	bool bQueueWhenOffline = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideRetry = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideRetry"))
	FVaRestRetryPolicy Retry;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideTimeouts = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideTimeouts", ClampMin = "0"))
	float Timeout = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideTimeouts", ClampMin = "0"))
	float ActivityTimeout = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideCache = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideCache"))
	FVaRestCachePolicy Cache;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideEnvelope = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideEnvelope"))
	FVaRestEnvelope Envelope;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideNaming = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideNaming"))
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverridePriority = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverridePriority"))
	EVaRestPriority Priority = EVaRestPriority::Normal;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideMaxJsonBytes = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideMaxJsonBytes", ClampMin = "0"))
	int64 MaxJsonBytes = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (InlineEditConditionToggle))
	bool bOverrideStrictDecode = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Overrides", meta = (EditCondition = "bOverrideStrictDecode"))
	bool bStrictDecode = false;
};

namespace VaRest::Http
{
/** Checked symbolic mapping: the asset enum is never cast to the native queue/body enum. */
VARESTHTTP_API bool TryGetNativeBodyKind(EVaRestEndpointBodyKind Kind, EVaRestBodyKind& OutKind);
/** Lossless ordered copies; repeated headers (including Set-Cookie) are individual rows. */
VARESTHTTP_API TArray<FVaRestHeader> ToHeaderData(const FVaRestHeaders& Headers);
VARESTHTTP_API FVaRestHeaders ToNativeHeaders(TConstArrayView<FVaRestHeader> Headers);
} // namespace VaRest::Http
