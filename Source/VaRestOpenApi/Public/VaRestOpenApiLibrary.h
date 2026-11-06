// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestOpenApiLibrary.generated.h"

class UVaRestApi;
class UVaRestOpenApiImportOptions;

/** The same importer used by Content Browser, available to editor utilities and Python. */
UCLASS()
class VARESTOPENAPI_API UVaRestOpenApiLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** DestinationPackage is a complete unused asset package, for example /Game/Backend/Api. Marks dirty without saving. */
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static UVaRestApi* ImportFile(const FString& Filename,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options);
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static UVaRestApi* ImportFromFile(const FString& Filename,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options);
	/** Controlled GET on the editor engine Hub. Personal fetch headers come from Editor Preferences. Marks dirty without saving. */
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static UVaRestApi* ImportFromUrl(const FString& Url,const FString& DestinationPackage,UVaRestOpenApiImportOptions* Options);
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static bool ShowLastImportReport(UVaRestApi* Api);
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static bool Reimport(UVaRestApi* Api,bool bAllowBreaking=false);
	/** Report source drift without changing packages or the saved manifest. True means no drift. */
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static bool CheckForChanges(UVaRestApi* Api);
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static bool ChangeFileSource(UVaRestApi* Api,const FString& Filename);
	/** Records a URL source without issuing a request. Reimport fetches it using personal editor headers. */
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static bool ChangeUrlSource(UVaRestApi* Api,const FString& Url);
	/** Interactive explicit deletion of selected obsolete declarations; editor only, no automatic selection. */
	UFUNCTION(BlueprintCallable,Category="VaRest|OpenAPI")
	static int32 CleanUpRemovedItems(UVaRestApi* Api);
};
