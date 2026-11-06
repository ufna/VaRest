// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestSocketActions.h"
#include <atomic>
#include "VaRestSocketGraphTestTypes.generated.h"

UCLASS(BlueprintType, Blueprintable, Transient, meta=(ShowWorldContextPin))
class UVaRestSocketGraphTestOwner : public UObject
{
 GENERATED_BODY()
public:
 virtual UWorld* GetWorld() const override { return GetOuter() ? GetOuter()->GetWorld() : nullptr; }
 UPROPERTY(BlueprintReadWrite, Category="Test") FString Received;
 UPROPERTY(BlueprintReadWrite, Category="Test") FString Reply;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestSocketMessage Message;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestError Error;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<FVaRestJsonError> Warnings;
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestListenSocketAction> ListenAction;
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestSocketRequestAction> RequestAction;
 UPROPERTY(BlueprintReadWrite, Category="Test") EVaRestSendResult EmitResult=EVaRestSendResult::Rejected;
 UPROPERTY(BlueprintReadWrite, Category="Test") EVaRestSendResult FirstReply=EVaRestSendResult::Rejected;
 UPROPERTY(BlueprintReadWrite, Category="Test") EVaRestSendResult SecondReply=EVaRestSendResult::Sent;
 int32 Readies=0,ConnectFailures=0,Messages=0,Replies=0,Errors=0,Unavailable=0;
 std::atomic<bool> WrongThread{false};
 bool Enter() { if (IsInGameThread()) return true; WrongThread.store(true,std::memory_order_release); return false; }
 UFUNCTION() void Ready(const FVaRestSocketOpen& Open,bool Reconnect) { if (Enter()) ++Readies; }
 UFUNCTION() void ConnectFailed(const FVaRestError& Failure,const FVaRestSocketClose& Close) { if (Enter()) { ++ConnectFailures; Error=Failure; } }
 UFUNCTION(BlueprintCallable, Category="Test") void Count(int32 Branch)
 { if (!Enter()) return; if (Branch==0) ++Messages; else if (Branch==1) ++Replies; else if (Branch==2) ++Errors; else if (Branch==3) ++Unavailable; }
 void ClearOnGT() { check(IsInGameThread()); Message={}; Error={}; Warnings.Reset(); ListenAction=nullptr; RequestAction=nullptr; }
};
