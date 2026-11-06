// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestHttpBlueprintTypes.h"
#include "VaRestRequestHandle.h"
#include <atomic>
#include "VaRestEndpointGraphTestTypes.generated.h"

class UVaRestApiClient;

/** Editor-only owner for actual compiled Call Endpoint continuations. */
UCLASS(BlueprintType, Blueprintable, Transient, meta=(ShowWorldContextPin))
class UVaRestEndpointGraphTestOwner : public UObject
{
 GENERATED_BODY()
public:
 virtual UWorld* GetWorld() const override { return GetOuter() ? GetOuter()->GetWorld() : nullptr; }
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestApiClient> Client;
 UPROPERTY(BlueprintReadWrite, Category="Test") FString Input;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<uint8> InputBytes;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<int32> Numbers;
 UPROPERTY(BlueprintReadWrite, Category="Test") FString Text;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<uint8> Bytes;
 UPROPERTY(BlueprintReadWrite, Category="Test") FString ErrorBody;
 UPROPERTY(BlueprintReadWrite, Category="Test") bool ErrorBodyValid=false;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestResponseData Http;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestError Error;
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestRequestHandle> Handle;
 int32 Successes=0,Errors=0,Unavailable=0,Thens=0;
 bool InsideStart=false,Inline=false;
 std::atomic<bool> WrongThread{false};
 UFUNCTION(BlueprintCallable, Category="Test") void Count(int32 Branch)
 {
  if (!IsInGameThread()) { WrongThread.store(true,std::memory_order_release); return; }
  if (Branch<3 && InsideStart) Inline=true;
  if (Branch==0) ++Successes; else if (Branch==1) ++Errors; else if (Branch==2) ++Unavailable; else if (Branch==3) ++Thens;
 }
 void ClearOnGT()
 { check(IsInGameThread()); Text.Reset(); Bytes.Reset(); ErrorBody.Reset(); ErrorBodyValid=false; Http={}; Error={}; Handle=nullptr; }
};
