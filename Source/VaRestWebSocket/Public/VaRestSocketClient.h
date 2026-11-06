// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestSocket.h"
#include "VaRestSocketClient.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSocketConnectedEvent, const FVaRestSocketOpen&, Open);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVaRestSocketReadyEvent, const FVaRestSocketOpen&, Open, bool, bIsReconnect);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSocketDisconnectedEvent, const FVaRestSocketClose&, Close);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVaRestSocketRetryEvent, const FVaRestSocketStatus&, Status, const FVaRestSocketClose&, Close);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVaRestSocketGaveUpEvent, const FVaRestError&, Error, const FVaRestSocketClose&, Close);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVaRestSocketStateEvent, EVaRestSocketState, OldState, EVaRestSocketState, NewState);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSocketMessageEvent, const FVaRestSocketMessage&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSocketErrorEvent, const FVaRestError&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FVaRestSocketDrainedEvent);

/** Weak-cached UObject view of one native socket. All functions and delegates run on the game thread. */
UCLASS(BlueprintType)
class VARESTWEBSOCKET_API UVaRestSocketClient : public UObject
{
	GENERATED_BODY()
public:
	FVaRestSocket GetNativeSocket() const { check(IsInGameThread()); return Native; }
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") virtual void Connect();
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void Close(int32 Code = 1000, const FString& Reason = TEXT(""));
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void ReconnectNow();
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void StopReconnecting();
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void ResetSession();
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") EVaRestSendResult SendText(const FString& Text, EVaRestSend Flags = EVaRestSend::Default);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") EVaRestSendResult SendBytes(const TArray<uint8>& Bytes, EVaRestSend Flags = EVaRestSend::Default);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") EVaRestSendResult SendJson(const FVaRestJson& Json, EVaRestSend Flags = EVaRestSend::Default);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(CustomStructureParam="Value", AutoCreateRefTerm="Value"))
	EVaRestSendResult SendStruct(const int32& Value, EVaRestSend Flags = EVaRestSend::Default);
	DECLARE_FUNCTION(execSendStruct);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") EVaRestSendResult EmitJson(const FString& Route, const FVaRestJson& Json, EVaRestSend Flags = EVaRestSend::Default);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(CustomStructureParam="Value", AutoCreateRefTerm="Value", DisplayName="Emit Socket Message"))
	EVaRestSendResult Emit(const FString& Route, const int32& Value, EVaRestSend Flags = EVaRestSend::Default);
	DECLARE_FUNCTION(execEmit);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") EVaRestSendResult ReplyToMessage(const FVaRestSocketMessage& Message, const FVaRestJson& Json);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") bool IsValidSocket() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") EVaRestSocketState GetState() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") FVaRestSocketStatus GetStatus() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") FVaRestSocketStats GetStats() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") bool IsConnected() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") bool IsReady() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") double GetLatency() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") double GetLatencyAverage() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") int64 GetBufferedAmount() const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") int32 GetQueuedCount() const;
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void ClearQueue();
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetHeader(const FString& Header, const FString& Value);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void RemoveHeader(const FString& Header);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetQueryParam(const FString& Param, const FString& Value);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void RemoveQueryParam(const FString& Param);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetUrl(const FString& Url);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetOrigin(const FString& Origin, bool bDeriveFromUrl = false);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetProtocols(const TArray<FString>& Protocols);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetReconnectPolicy(const FVaRestReconnectPolicy& Policy);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetHeartbeat(const FVaRestHeartbeat& Heartbeat);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetProtocolPing(const FVaRestProtocolPing& Ping);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetLogLevel(EVaRestLogLevel Level);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetRouting(const FVaRestSocketRouting& Routing);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetAuthPlacement(const FVaRestSocketAuthPlacement& Placement);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetLimits(const FVaRestSocketLimits& Limits);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void SetQueuePolicy(const FVaRestSocketQueuePolicy& Policy);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void ReadyWhen(const FString& Route);
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketConnectedEvent OnConnected;
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketReadyEvent OnReady;
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketDisconnectedEvent OnDisconnected;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketRetryEvent OnRetryScheduled;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketGaveUpEvent OnGaveUp;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketStateEvent OnStateChanged;
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketMessageEvent OnMessage;
	UPROPERTY(BlueprintAssignable, Category="VaRestSocketTransport") FVaRestSocketMessageEvent OnUnrouted;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketErrorEvent OnError;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketDrainedEvent OnDrained;
	virtual void BeginDestroy() override;
protected:
	void InitializeOnGT(FVaRestSocket Socket, bool bTransportEvents = true);
private:
	friend class UVaRestSocketHub;
	FVaRestSocket Native;
};
