// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestSioActions.h"
#include <atomic>
#include "VaRestSioActionTestTypes.generated.h"

/** Editor-only automation receiver; its native observations never cross into socket workers. */
UCLASS(Transient)
class UVaRestSioActionTestProbe final : public UObject
{
 GENERATED_BODY()
public:
 int32 Connects=0,ConnectFailures=0,Events=0,Acks=0,Errors=0;
 std::atomic<bool> WrongThread{false};
 FVaRestJson LastPayload,LastArgs;
 FVaRestError LastError;
 FVaRestSioEvent LastEvent;
 bool Enter() { if (IsInGameThread()) return true; WrongThread.store(true,std::memory_order_release); return false; }
 UFUNCTION() void Connected(const FVaRestSioConnect& Info) { if (Enter()) ++Connects; }
 UFUNCTION() void ConnectFailed(const FVaRestError& Error,const FVaRestSioConnectError& Info)
 { if (Enter()) { ++ConnectFailures; LastError=Error; } }
 UFUNCTION() void Event(const FVaRestJson& Payload,const FVaRestSioEvent& Info,const FVaRestSioAck& Ack)
 { if (Enter()) { ++Events; LastPayload=Payload; LastEvent=Info; } }
 UFUNCTION() void Acked(const FVaRestJson& Reply,const FVaRestJson& Args,const FVaRestSioEvent& Info)
 { if (Enter()) { ++Acks; LastPayload=Reply; LastArgs=Args; LastEvent=Info; } }
 UFUNCTION() void Failed(const FVaRestError& Error) { if (Enter()) { ++Errors; LastError=Error; } }
 void ClearOnGT() { check(IsInGameThread()); LastPayload={}; LastArgs={}; LastError={}; LastEvent={}; }
};

/** Real Blueprint context for compiled protocol-node automation in an owned live world. */
UCLASS(BlueprintType, Blueprintable, Transient, meta=(ShowWorldContextPin))
class UVaRestSioGraphTestOwner : public UObject
{
 GENERATED_BODY()
public:
 virtual UWorld* GetWorld() const override { return GetOuter() ? GetOuter()->GetWorld() : nullptr; }
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestSioClient> Socket;
 UPROPERTY(BlueprintReadWrite, Category="Test") FString Input;
 UPROPERTY(BlueprintReadWrite, Category="Test") int32 InputNumber=73;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<int32> InputArray;
 UPROPERTY(BlueprintReadWrite, Category="Test") EVaRestSendResult EmitResult=EVaRestSendResult::Rejected;
 UPROPERTY(BlueprintReadWrite, Category="Test") EVaRestSendResult FirstReply=EVaRestSendResult::Rejected;
 UPROPERTY(BlueprintReadWrite, Category="Test") EVaRestSendResult SecondReply=EVaRestSendResult::Sent;
 UPROPERTY(BlueprintReadWrite, Category="Test") int32 ListenValue=0;
 UPROPERTY(BlueprintReadWrite, Category="Test") int32 AckValue=0;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestSioEvent ListenInfo;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestSioEvent AckInfo;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestJson AckArgs;
 UPROPERTY(BlueprintReadWrite, Category="Test") FVaRestError LastFailure;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<FVaRestJsonError> ListenWarnings;
 UPROPERTY(BlueprintReadWrite, Category="Test") TArray<FVaRestJsonError> AckWarnings;
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestListenSioAction> ListenAction;
 UPROPERTY(BlueprintReadWrite, Category="Test") TObjectPtr<UVaRestEmitSioAckAction> AckAction;
 int32 Listens=0,Acks=0,Errors=0,Unavailable=0;
 std::atomic<bool> WrongThread{false};
 UFUNCTION(BlueprintCallable, Category="Test") void Count(int32 Branch)
 {
  if (!IsInGameThread()) { WrongThread.store(true,std::memory_order_release); return; }
  if (Branch==0) ++Listens; else if (Branch==1) ++Acks; else if (Branch==2) ++Errors; else if (Branch==3) ++Unavailable;
 }
 void ClearOnGT()
 {
  check(IsInGameThread()); Socket=nullptr; ListenAction=nullptr; AckAction=nullptr;
  ListenInfo={}; AckInfo={}; AckArgs={}; LastFailure={}; ListenWarnings.Reset(); AckWarnings.Reset();
 }
};
