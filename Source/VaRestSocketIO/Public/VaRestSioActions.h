// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSocketActions.h"
#include "VaRestSioClient.h"
#include "VaRestSioActions.generated.h"

UCLASS(Abstract)
class VARESTSOCKETIO_API UVaRestSioAsyncAction : public UVaRestSocketAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure, Category="VaRest|Internal", meta=(BlueprintInternalUseOnly="true"))
	static FVaRestSioEvent GetPreparedEvent(UVaRestPreparedOutputAction* Action, const FString& RequestIdText);
	virtual void BeginDestroy() override;
protected:
	UVaRestSioClient* SioClient() const { return Cast<UVaRestSioClient>(Client); }
	void FinishSioOnGT();
	FString PrepareEventOutputOnGT(const FVaRestSioEvent& Event, EVaRestSioArgs ArgsMode, FVaRestError& Error);
	bool DeliverEventOutputOnGT(const FString& Cookie);
	virtual void CanceledOnGT(const FVaRestError& Error) override;
private:
	TMap<FString, FVaRestSioEvent> PreparedEvents;
};

/** Repeats for every default-namespace connect; a terminal end before its first connect reports OnFailed. */
UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Action"))
class VARESTSOCKETIO_API UVaRestConnectSioAction : public UVaRestSioAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Connect Socket.IO"))
	static UVaRestConnectSioAction* ConnectSocketIO(const UObject* WorldContextObject, UVaRestSioClient* Socket);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="Connect Socket.IO Entry"))
	static UVaRestConnectSioAction* ConnectAsset(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioConnectEvent OnConnect;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioConnectErrorEvent OnFailed;
	virtual void Activate() override;
protected:
	virtual void CanceledOnGT(const FVaRestError& Error) override;
private:
	void EndOnGT(FVaRestError Error, FVaRestSioConnectError ConnectError = {});
	bool bConnected = false;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FVaRestSioListenEvent, const FVaRestJson&, Payload, const FVaRestSioEvent&, Event, const FVaRestSioAck&, Ack);

/** Native event stream. Reflection conversion remains an explicit GTK operation. */
UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Action"))
class VARESTSOCKETIO_API UVaRestListenSioAction : public UVaRestSioAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", DisplayName="On Socket.IO Event"))
	static UVaRestListenSioAction* Listen(const UObject* WorldContextObject, UVaRestSioClient* Socket, const FString& Event, const FString& Namespace = TEXT(""), bool bOnce = false, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject"))
	static UVaRestListenSioAction* ListenAsset(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Event, const FString& Namespace = TEXT(""), bool bOnce = false, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First, UVaRestSioClient* Socket = nullptr);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioListenEvent OnEvent;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSocketErrorEvent OnError;
	virtual void Activate() override;
private:
	FString EventName, NamespaceName;
	bool bSingle = false;
	EVaRestSioArgs Mode = EVaRestSioArgs::First;
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FVaRestSioAckEvent, const FVaRestJson&, Reply, const FVaRestJson&, Args, const FVaRestSioEvent&, Event);

/** Prepares one native packet in the factory; Send begins only after Blueprint delegates have been bound. */
UCLASS(BlueprintType, meta=(ExposedAsyncProxy="Action"))
class VARESTSOCKETIO_API UVaRestEmitSioAckAction : public UVaRestSioAsyncAction
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", CustomStructureParam="Payload", AutoCreateRefTerm="Payload", DisplayName="Emit With Ack"))
	static UVaRestEmitSioAckAction* EmitWithAck(const UObject* WorldContextObject, UVaRestSioClient* Socket, const FString& Event, const int32& Payload,
		const FString& Namespace = TEXT(""), double Timeout = 10.0, bool bErrorFirst = false, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First, EVaRestSioArgs ReplyArgsMode = EVaRestSioArgs::First);
	DECLARE_FUNCTION(execEmitWithAck);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", CustomStructureParam="Payload", AutoCreateRefTerm="Payload"))
	static UVaRestEmitSioAckAction* EmitAssetWithAck(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Event, const int32& Payload,
		const FString& Namespace = TEXT(""), double Timeout = 10.0, bool bErrorFirst = false, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First, EVaRestSioArgs ReplyArgsMode = EVaRestSioArgs::First, UVaRestSioClient* Socket = nullptr);
	DECLARE_FUNCTION(execEmitAssetWithAck);
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioAckEvent OnAck;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSocketErrorEvent OnError;
	virtual void Activate() override;
	virtual void BeginDestroy() override;
protected:
	virtual void CanceledOnGT(const FVaRestError& Error) override;
private:
	static UVaRestEmitSioAckAction* PrepareOnGT(const UObject* Context, UVaRestSioClient* Socket, FStringView Event, FStringView Namespace,
		const FProperty* Property, const void* Address, double Timeout, bool bErrorFirst, EVaRestSioArgs ArgsMode, EVaRestSioArgs ReplyArgsMode);
	TUniquePtr<FVaRestSioEmitWithAck> Prepared;
	FVaRestSioAckHandle Handle;
	EVaRestSioArgs ReplyMode = EVaRestSioArgs::First;
};
