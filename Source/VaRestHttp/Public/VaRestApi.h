// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "Templates/ValueOrError.h"
#include "VaRestApiSocketConfig.h"
#include "VaRestApiTypes.h"
#include "VaRestAuthConfig.h"
#include "VaRestOffline.h"

#include "VaRestApi.generated.h"

class UAssetImportData;

/** Reusable API contract. Hub/client instances own runtime credentials and connectivity, never this asset. */
UCLASS(BlueprintType)
class VARESTHTTP_API UVaRestApi : public UDataAsset
{
	GENERATED_BODY()

public:
	UVaRestApi();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environments")
	TArray<FVaRestApiEnvironment> Environments;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environments")
	FName DefaultEnvironment;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Environments")
	FName ShippingEnvironment;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests")
	TArray<FVaRestHeader> Headers;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests")
	FString UserAgent;
	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category = "Authentication")
	TObjectPtr<UVaRestAuthConfig> Auth = nullptr;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests")
	FVaRestRetryPolicy Retry;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests", meta = (ClampMin = "0"))
	float Timeout = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests", meta = (ClampMin = "0"))
	float ActivityTimeout = 0.f;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests", meta = (ClampMin = "0"))
	int64 MaxJsonBytes = 32 * 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Requests", meta = (ClampMin = "1"))
	int32 MaxConcurrent = 16;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Cache")
	FVaRestCachePolicy Cache;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Offline")
	FVaRestOfflinePolicy Offline;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "JSON")
	FVaRestEnvelope Envelope;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "JSON")
	EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "JSON")
	bool bStrictDecode = false;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "JSON")
	FString JsonContentType = TEXT("application/json");
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "JSON")
	TArray<FVaRestApiJsonTypeRules> JsonTypes;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "JSON", meta = (ContentDir))
	FString TypesFolder;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Logging")
	EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Logging")
	TArray<FString> RedactedKeys;
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Endpoints")
	TArray<FVaRestApiEndpoint> Endpoints;
	UPROPERTY(EditAnywhere, Instanced, BlueprintReadOnly, Category = "Sockets")
	TArray<TObjectPtr<UVaRestApiSocketConfig>> Sockets;

#if WITH_EDITORONLY_DATA
	UPROPERTY(VisibleAnywhere, Instanced, Category = "Import Settings")
	TObjectPtr<UAssetImportData> AssetImportData;
	UPROPERTY()
	FVaRestApiImportState ImportState;
#endif

	const FVaRestApiEndpoint* FindEndpoint(const FGuid& Id) const;
	const FVaRestApiEndpoint* FindEndpoint(FName Name) const;
	const FVaRestApiEnvironment* FindEnvironment(FName Name) const;
	/** Per-asset CLI, global CLI, ShippingEnvironment in Shipping, then DefaultEnvironment. */
	TValueOrError<FName, FString> ResolveEnvironmentName() const;
	/** Refresh/token references use only the declared POST endpoint path and its name for trace identity. */
	TValueOrError<FString, FString> ResolveEndpointPath(FName EndpointName) const;
	void Validate(FVaRestApiValidationContext& Context) const;
	/** Programmatic importers call this once after their complete batch, rather than per field. GT only. */
	void NotifyEdited();

	virtual void PostLoad() override;
	virtual void BeginDestroy() override;

#if WITH_EDITOR
	virtual void PostInitProperties() override;
	virtual void PostEditChangeProperty(FPropertyChangedEvent& Event) override;
	virtual EDataValidationResult IsDataValid(FDataValidationContext& Context) const override;
	virtual void GetAssetRegistryTags(FAssetRegistryTagsContext Context) const override;
#endif

private:
	void RegisterJsonRules();
	FString RuleRegistrationError;
};
