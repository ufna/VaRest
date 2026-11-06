// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestDynamicTransferTestLibrary.generated.h"

class UVaRestRequestAction;
class UVaRestApiClient;
class UBlueprint;

/** Stable native observation object, independent of the BP instance being synchronously reinstanced. */
UCLASS()
class UVaRestDynamicTransferTestProbe : public UObject
{
	GENERATED_BODY()
public:
	bool bCalled=false,bNoNativeCarrier=false,bCompiled=false;
	bool bRecompileRequested=false,bRecompileFinished=false;
	uint64 RecompileRequestedFrame=0;
	TWeakObjectPtr<UBlueprint> PendingRecompile;
	int32 ObservedValue=0,ContinuationCount=0;
	FString ObservedCookie;
	UPROPERTY() TObjectPtr<UVaRestApiClient> ReentrantClient;
	UPROPERTY() TObjectPtr<UVaRestRequestAction> ReentrantAction;
	FString ReentrantError;
	int32 ReentrantEntries=0;
	uint64 ReentrantSendFrame=0;
	bool bSecondSent=false;
};

UCLASS()
class UVaRestDynamicTransferTestLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	/** Copies user observations and requests a real recompile after the executing Blueprint VM frame unwinds. */
	UFUNCTION(BlueprintCallable,meta=(BlueprintInternalUseOnly="true"),Category="VaRest|Internal")
	static void RecompileTarget(UVaRestRequestAction* Action,UObject* Target,UVaRestDynamicTransferTestProbe* Probe,int32 Value,FString Cookie,int32 ContinuationCount);
	/** Reenters the same graph Start once, from its real generated user continuation. */
	UFUNCTION(BlueprintCallable,meta=(BlueprintInternalUseOnly="true"),Category="VaRest|Internal")
	static void ReenterTarget(UVaRestRequestAction* Action,UObject* Target,UVaRestDynamicTransferTestProbe* Probe,int32 Value,FString Cookie,int32 ContinuationCount);
};
