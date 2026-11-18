// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestM5EndpointRunner.generated.h"
class UVaRestApi;
class UVaRestApiClient;
/** Ordinary Blueprint base. Requests belong exclusively to the authored genuine product K2 nodes. */
UCLASS(Blueprintable)
class VARESTM5SMOKE_API AVaRestM5EndpointRunner : public AActor
{
    GENERATED_BODY()
public:
    AVaRestM5EndpointRunner();
    UPROPERTY(EditDefaultsOnly,BlueprintReadOnly,Category="M5") TObjectPtr<UVaRestApi> Api;
    UPROPERTY(Transient,BlueprintReadOnly,Category="M5") TObjectPtr<UVaRestApiClient> Client;
    UPROPERTY(Transient,BlueprintReadOnly,Category="M5") bool bPassed=false;
    UFUNCTION(BlueprintCallable,Category="M5") bool Arm();
    UFUNCTION(BlueprintCallable,Category="M5") bool CheckCase(int32 Case,const FVaRestResponseData& Http,const FVaRestError& Error,bool bErrorBodyValid);
    UFUNCTION(BlueprintCallable,Category="M5") void Reject(const FString& Reason,const FVaRestResponseData& Http,const FVaRestError& Error);
    virtual void Tick(float DeltaSeconds) override;
private:
    bool bArmed=false;
    int32 NextCase=0;
    double Deadline=0;
    TSet<FString> RequestIds;
    bool Fail(const FString& Reason);
};
