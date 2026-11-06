// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestOpenApiUserSettings.generated.h"

USTRUCT()
struct FVaRestOpenApiFetchHeader
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere,Category="Fetch")
	FString Name;
	UPROPERTY(EditAnywhere,Category="Fetch",meta=(PasswordField=true))
	FString Value;
};
USTRUCT()
struct FVaRestOpenApiFetchHost
{
	GENERATED_BODY()
	/** A bare host uses HTTPS. Include http:// explicitly for a development HTTP origin. */
	UPROPERTY(EditAnywhere,Category="Fetch",meta=(DisplayName="Host or origin"))
	FString Origin;
	UPROPERTY(EditAnywhere,Category="Fetch")
	TArray<FVaRestOpenApiFetchHeader> Headers;
};
/** Personal editor settings. Fetch credentials never enter an API asset or its import manifest. */
UCLASS(Config=EditorPerProjectUserSettings,meta=(DisplayName="VaRest OpenAPI Sources"))
class VARESTOPENAPI_API UVaRestOpenApiUserSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,Config,Category="Fetch headers")
	TArray<FVaRestOpenApiFetchHost> Hosts;
	virtual FName GetContainerName() const override { return TEXT("Editor"); }
	virtual FName GetCategoryName() const override { return TEXT("Plugins"); }
	/** GT snapshot. Plain copied strings only; no settings UObject is retained by a worker. */
	TArray<FVaRestOpenApiFetchHeader> HeadersFor(const FString& Uri) const;
};
