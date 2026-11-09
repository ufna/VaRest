// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "VaRestSocketClient.h"
#include "VaRestSioSocket.h"
#include "VaRestSioClient.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSioConnectEvent, const FVaRestSioConnect&, Connect);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVaRestSioConnectErrorEvent, const FVaRestError&, Error, const FVaRestSioConnectError&, ConnectError);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSioDisconnectEvent, const FVaRestSioDisconnect&, Disconnect);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestSioEventEvent, const FVaRestSioEvent&, Event);

/** GTK Blueprint view of one Socket.IO identity; namespaces use strings, with empty selecting the default. */
UCLASS(BlueprintType, HideCategories=(VaRestSocketTransport), HideFunctions=(SendText,SendBytes,SendJson,SendStruct,Emit,EmitJson,ReplyToMessage,SetProtocols,SetHeartbeat,SetRouting,SetAuthPlacement,SetLimits,ReadyWhen,SetReconnectPolicy,SetUrl,GetStats))
class VARESTSOCKETIO_API UVaRestSioClient : public UVaRestSocketClient
{
	GENERATED_BODY()
public:
	static UVaRestSioClient* WrapOnGT(UVaRestSocketHub* Hub, FVaRestSioSocket Socket);
	FVaRestSioSocket GetNativeSioSocket() const { check(IsInGameThread()); return Socket; }
	FVaRestSioConnect GetLastConnectInfoOnGT(FStringView Namespace = {}) const;
	virtual void Connect() override;
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(CustomStructureParam="Payload", AutoCreateRefTerm="Payload", DisplayName="Emit"))
	EVaRestSendResult EmitEvent(const FString& Event, const int32& Payload, const FString& Namespace = TEXT(""), bool bVolatile = false, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First);
	DECLARE_FUNCTION(execEmitEvent);
	/** Internal bounded variadic thunk; the dedicated Emit node exposes only ArgumentCount pins. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", CustomStructureParam="Arg0,Arg1,Arg2,Arg3,Arg4,Arg5,Arg6,Arg7,Arg8,Arg9,Arg10,Arg11,Arg12,Arg13,Arg14,Arg15,Arg16,Arg17,Arg18,Arg19,Arg20,Arg21,Arg22,Arg23,Arg24,Arg25,Arg26,Arg27,Arg28,Arg29,Arg30,Arg31", AutoCreateRefTerm="Arg0,Arg1,Arg2,Arg3,Arg4,Arg5,Arg6,Arg7,Arg8,Arg9,Arg10,Arg11,Arg12,Arg13,Arg14,Arg15,Arg16,Arg17,Arg18,Arg19,Arg20,Arg21,Arg22,Arg23,Arg24,Arg25,Arg26,Arg27,Arg28,Arg29,Arg30,Arg31"))
	EVaRestSendResult EmitArguments(const FString& Event, const FString& Namespace, bool bVolatile, int32 ArgumentCount,
		const int32& Arg0, const int32& Arg1, const int32& Arg2, const int32& Arg3, const int32& Arg4, const int32& Arg5, const int32& Arg6, const int32& Arg7,
		const int32& Arg8, const int32& Arg9, const int32& Arg10, const int32& Arg11, const int32& Arg12, const int32& Arg13, const int32& Arg14, const int32& Arg15,
		const int32& Arg16, const int32& Arg17, const int32& Arg18, const int32& Arg19, const int32& Arg20, const int32& Arg21, const int32& Arg22, const int32& Arg23,
		const int32& Arg24, const int32& Arg25, const int32& Arg26, const int32& Arg27, const int32& Arg28, const int32& Arg29, const int32& Arg30, const int32& Arg31);
	DECLARE_FUNCTION(execEmitArguments);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO") EVaRestSendResult EmitArgs(const FString& Event, const FVaRestJson& Args, const FString& Namespace = TEXT(""), bool bVolatile = false);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(AutoCreateRefTerm="Auth")) void ConnectNamespace(const FString& Namespace, const FVaRestJson& Auth);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO") void DisconnectNamespace(const FString& Namespace);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") bool IsNamespaceConnected(const FString& Namespace = TEXT("")) const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") EVaRestSioNamespaceState GetNamespaceState(const FString& Namespace = TEXT("")) const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") FString GetSid(const FString& Namespace = TEXT("")) const;
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") bool WasRecovered(const FString& Namespace = TEXT("")) const;
	/** Empty selects the whole socket; a named namespace filters logical packet/ack counters. */
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO", meta=(DisplayName="Get Stats")) FVaRestSioStats GetSioStats(const FString& Namespace = TEXT("")) const;
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO") void SetAuthPayload(const FString& Namespace, const FVaRestJson& Auth);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(DisplayName="Set Socket.IO Auth Placement")) void SetSioAuthPlacement(const FVaRestSioAuthPlacement& Placement);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(DisplayName="Set Socket.IO Limits")) void SetSioLimits(const FVaRestSioLimits& Limits);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(DisplayName="Set Reconnect Policy")) void SetSioReconnectPolicy(const FVaRestReconnectPolicy& Policy);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") int32 GetNamespaceQueuedCount(const FString& Namespace = TEXT("")) const;
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO") void ClearNamespaceQueue(const FString& Namespace = TEXT(""));
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioConnectEvent OnConnect;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioConnectErrorEvent OnConnectError;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioDisconnectEvent OnDisconnect;
	UPROPERTY(BlueprintAssignable, Category="VaRest|Socket.IO") FVaRestSioEventEvent OnEvent;
	/** Eagerly encodes the borrowed GTK value. No property/address survives this call. */
	EVaRestSendResult EmitPropertyOnGT(FStringView Event, FStringView Namespace, const FProperty* Property, const void* Address, EVaRestSioArgs Mode, bool bVolatile);
	FVaRestSioEmitWithAck PrepareAckOnGT(FStringView Event, FStringView Namespace, const FProperty* Property, const void* Address, EVaRestSioArgs Mode);
	bool ListenOnGT(FStringView Namespace, FStringView Event, const UObject* Owner, TFunction<void(const FVaRestSioEvent&)> Handler, bool bOnce);
	virtual void BeginDestroy() override;
private:
	bool InitializeSioOnGT(FVaRestSioSocket InSocket);
	FVaRestSioNamespace ResolveNamespace(FStringView Namespace, bool bCreate = true) const;
	FVaRestSioSocket Socket;
	using UVaRestSocketClient::SetHeartbeat;
	using UVaRestSocketClient::SetRouting;
};
