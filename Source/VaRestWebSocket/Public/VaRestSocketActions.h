// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "VaRestPreparedOutputAction.h"
#include "VaRestSocketClient.h"
#include "VaRestSocketActions.generated.h"

class UVaRestHub;
class UWorld;

/** Common bounded Hub retention and weak context lifetime. Stopping an action leaves a shared socket running. */
UCLASS(Abstract)
class VARESTWEBSOCKET_API UVaRestSocketAsyncAction : public UVaRestPreparedOutputAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	static FVaRestSocketMessage GetPreparedMessage(UVaRestPreparedOutputAction* Action, const FString& RequestIdText);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void Stop();
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") bool IsFinished() const { return bFinished; }
	virtual void BeginDestroy() override;
protected:
	void InitializeContextOnGT(const UObject* Context, UVaRestSocketClient* Socket);
	bool StartScopeOnGT(FVaRestError& Error);
	void FinishOnGT();
	virtual void CanceledOnGT(const FVaRestError& Error);
	virtual bool CanDeliverPreparedOutputOnGT() const override;
	/** One activation snapshot; binary attachment resolution is local to each preparation. */
	FVaRestJsonConvertOptions OutputOptions;
	FString PrepareMessageOutputOnGT(const FVaRestSocketMessage& Message, const FVaRestJson& Value, FVaRestError& Error);
	bool DeliverMessageOutputOnGT(const FString& Cookie);
	UPROPERTY(Transient) TObjectPtr<UVaRestSocketClient> Client;
	TWeakObjectPtr<const UObject> ContextOwner;
	bool bActivated = false, bFinished = false;
	FVaRestError InitialError;
private:
	bool TickOwner(float DeltaSeconds);
	TWeakObjectPtr<UWorld> ContextWorld;
	TWeakObjectPtr<UVaRestHub> CoreHub;
	uint64 Registration = 0;
	FTSTicker::FDelegateHandle OwnerTick;
	TMap<FString, FVaRestSocketMessage> PreparedMessages;
};

/** OnReady repeats after each reconnect. OnFailed covers terminal ends before the first open. */
UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Action"))
class VARESTWEBSOCKET_API UVaRestConnectSocketAction : public UVaRestSocketAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Connect Socket"))
	static UVaRestConnectSocketAction* ConnectSocket(const UObject* WorldContextObject, UVaRestSocketClient* Socket);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketReadyEvent OnReady;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketGaveUpEvent OnFailed;
	virtual void Activate() override;
protected:
	virtual void CanceledOnGT(const FVaRestError& Error) override;
private:
	void EndOnGT(const FVaRestError& Error, const FVaRestSocketClose& Close);
	bool bOpened = false;
};

/** JSON/native message stream. Typed reflected delivery is a separate gated API. */
UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Action"))
class VARESTWEBSOCKET_API UVaRestListenSocketAction : public UVaRestSocketAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Listen for Socket Message"))
	static UVaRestListenSocketAction* Listen(const UObject* WorldContextObject, UVaRestSocketClient* Socket, const FString& Route, const FString& Channel = TEXT(""), bool bRequestsOnly = false);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"))
	static UVaRestListenSocketAction* ListenAsset(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Route, const FString& Channel = TEXT(""), bool bRequestsOnly = false);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketMessageEvent OnMessage;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketErrorEvent OnError;
	virtual void Activate() override;
protected:
	virtual void CanceledOnGT(const FVaRestError& Error) override;
private:
	FString RouteName, ChannelName;
	bool bOnlyRequests = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSocketReplyEvent, const FVaRestJson&, Reply);

UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Action"))
class VARESTWEBSOCKET_API UVaRestSocketRequestAction : public UVaRestSocketAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Socket JSON Request"))
	static UVaRestSocketRequestAction* Request(const UObject* WorldContextObject, UVaRestSocketClient* Socket, const FString& Route, const FVaRestJson& Payload, double Timeout = 10.0, const FString& Channel = TEXT(""));
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"))
	static UVaRestSocketRequestAction* RequestAsset(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Route, const FVaRestJson& Payload, double Timeout = 10.0, const FString& Channel = TEXT(""));
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", CustomStructureParam="Payload", AutoCreateRefTerm="Payload"))
	static UVaRestSocketRequestAction* RequestValue(const UObject* WorldContextObject, UVaRestSocketClient* Socket, const FString& Route, const int32& Payload, double Timeout = 10.0, const FString& Channel = TEXT(""));
	DECLARE_FUNCTION(execRequestValue);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", CustomStructureParam="Payload", AutoCreateRefTerm="Payload"))
	static UVaRestSocketRequestAction* RequestAssetValue(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Route, const int32& Payload, double Timeout = 10.0, const FString& Channel = TEXT(""));
	DECLARE_FUNCTION(execRequestAssetValue);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketReplyEvent OnReply;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket") FVaRestSocketErrorEvent OnError;
	virtual void Activate() override;
	virtual void BeginDestroy() override;
protected:
	virtual void CanceledOnGT(const FVaRestError& Error) override;
private:
	static UVaRestSocketRequestAction* PrepareOnGT(const UObject* Context, UVaRestSocketClient* Socket, const FString& Route,
		const FProperty* Property, const void* Address, double Timeout, const FString& Channel);
	FString RouteName, ChannelName;
	FVaRestJson Body;
	TUniquePtr<FVaRestSocketJsonRequest> PreparedInput;
	double TimeoutSeconds = 10.0;
	FVaRestSocketRequestHandle RequestHandle;
};
