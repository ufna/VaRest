// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestGetJsonAction.h"
#include "VaRestRequestLibrary.generated.h"

class UVaRestApiClient;
class UVaRestAuthRefreshContext;
struct FVaRestDynamicResultConfig;

UCLASS()
class VARESTHTTP_API UVaRestRequestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable,Category="VaRest|Http",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestGetJsonAction* GetJson(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Path,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestGetJsonAction* RequestJson(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Verb,const FString& Path,const FVaRestJson& Body,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestRequestAction* DownloadFile(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Path,const FString& FilePath,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestGetJsonAction* UploadFile(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Verb,const FString& Path,const FString& FilePath,const FString& ContentType,FString& Error);
	UFUNCTION(BlueprintCallable,Category="VaRest|Auth",meta=(BlueprintInternalUseOnly="true",WorldContext="WorldContextObject"))
	static UVaRestGetJsonAction* RequestRefreshJson(const UObject* WorldContextObject,UVaRestAuthRefreshContext* Context,const FString& Verb,const FString& Path,const FVaRestJson& Body,FString& Error);
private:
	static UVaRestApiClient* ResolveClientOnGT(const UObject* WorldContextObject,UVaRestApiClient* Client,FString& Error);
	static FVaRestDynamicResultConfig MakeConfigOnGT(UVaRestApiClient* Client,bool bFile=false);
};
