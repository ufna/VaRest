// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestOpenApiPlan.h"
#include "VaRestOpenApiImportOptions.generated.h"

UCLASS(BlueprintType,Config=EditorPerProjectUserSettings)
class VARESTOPENAPI_API UVaRestOpenApiImportOptions : public UObject
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Types",meta=(ContentDir))
	FString TypesFolder;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Types")
	FString TypePrefix;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Types")
	bool bIntegersWithoutFormatAsInt32=false;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Types")
	bool bDateTimeAsString=false;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Types")
	bool bEnumsAsString=false;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Types")
	bool bGenerateUnreferencedSchemas=false;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Envelope")
	bool bUnwrapEnvelope=true;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Source")
	bool bAllowReferencesOutsideSpecFolder=false;
	UPROPERTY(BlueprintReadWrite,Category="Selection")
	bool bImportAllOperations=true;
	/** Exact operationId or 'METHOD /path'. Empty with ImportAllOperations off imports no operations. */
	UPROPERTY(BlueprintReadWrite,Category="Selection")
	TArray<FString> SelectedOperations;
	UPROPERTY(EditAnywhere,BlueprintReadWrite,Config,Category="Reimport")
	bool bIncludeNewOperations=true;
	FVaRestOpenApiPlanOptions PlanOptions(const FString& ApiName) const;
	FVaRestJson ToJson() const;
	bool FromJson(const FVaRestJson& Json);
};

UCLASS(Config=Editor,DefaultConfig,meta=(DisplayName="VaRest OpenAPI"))
class VARESTOPENAPI_API UVaRestOpenApiSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	/** Restart the editor after changing factory registration. */
	UPROPERTY(EditAnywhere,Config,Category="Import",meta=(ConfigRestartRequired=true))
	bool bRegisterForJsonFiles=true;
};
