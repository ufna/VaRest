// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestUtilityLibrary.generated.h"

class UVaRestHttpSettings;

/** Runtime world URL data, independent of the legacy VaRest module. This is not an HTTP endpoint URL. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestWorldUrl
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly,Category="URL") FString Protocol;
	UPROPERTY(BlueprintReadOnly,Category="URL") FString Host;
	UPROPERTY(BlueprintReadOnly,Category="URL") int32 Port=0;
	UPROPERTY(BlueprintReadOnly,Category="URL") int32 Valid=0;
	UPROPERTY(BlueprintReadOnly,Category="URL") FString Map;
	UPROPERTY(BlueprintReadOnly,Category="URL") FString RedirectURL;
	UPROPERTY(BlueprintReadOnly,Category="URL") TArray<FString> Op;
	UPROPERTY(BlueprintReadOnly,Category="URL") FString Portal;
};

/** Small runtime helpers for projects that link only the v2 modules. */
UCLASS()
class VARESTHTTP_API UVaRestUtilityLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static FString PercentEncode(const FString& Source);
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static FString Base64Encode(const FString& Source);
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static bool Base64Decode(const FString& Source,FString& Destination);
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static bool Base64EncodeData(const TArray<uint8>& Data,FString& Destination);
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static bool Base64DecodeData(const FString& Source,TArray<uint8>& Destination);
	/** Explicit UTF-8 input; these hashes are compatibility/data-format helpers. */
	UFUNCTION(BlueprintPure,Category="VaRest|Utility",meta=(DisplayName="MD5 (UTF-8)")) static FString Md5Utf8(const FString& Source);
	UFUNCTION(BlueprintPure,Category="VaRest|Utility",meta=(DisplayName="SHA1 (UTF-8)")) static FString Sha1Utf8(const FString& Source);
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static FString GetVaRestVersion();
	UFUNCTION(BlueprintPure,Category="VaRest|Utility",meta=(WorldContext="WorldContextObject")) static FVaRestWorldUrl GetWorldUrl(const UObject* WorldContextObject);
	/** The v2 settings object; legacy UVaRestSettings remains in its original config section. */
	UFUNCTION(BlueprintPure,Category="VaRest|Utility") static UVaRestHttpSettings* GetHttpSettings();
};
