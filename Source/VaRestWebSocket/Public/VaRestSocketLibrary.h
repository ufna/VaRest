// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestApiTypes.h"
#include "VaRestSocketCodec.h"
#include "VaRestSocketLibrary.generated.h"

class UVaRestApi;
class UVaRestSocketClient;

UCLASS()
class VARESTWEBSOCKET_API UVaRestSocketLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(WorldContext="WorldContextObject"))
	static UVaRestSocketClient* GetSocket(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, FVaRestError& Error);
	/** The declared Emit node resolves the shared asset socket and reports lookup failures before encoding its input. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", CustomStructureParam="Value", AutoCreateRefTerm="Value"))
	static EVaRestSendResult EmitAssetMessage(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Route, const int32& Value, FVaRestError& Error, EVaRestSend Flags = EVaRestSend::Default);
	DECLARE_FUNCTION(execEmitAssetMessage);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="Protocols,Headers"))
	static UVaRestSocketClient* MakeSocket(const UObject* WorldContextObject, const FString& Url, const TArray<FString>& Protocols, const TArray<FVaRestHeader>& Headers, FVaRestError& Error, FName Name = NAME_None);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FString DescribeSocketClose(const FVaRestSocketClose& Close);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestJson SocketMessageJson(const FVaRestSocketMessage& Message);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestJson SocketMessageData(const FVaRestSocketMessage& Message);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FString SocketMessageText(const FVaRestSocketMessage& Message);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static TArray<uint8> SocketMessageBytes(const FVaRestSocketMessage& Message);
	/** Explicit synchronous GT conversion of the already parsed message data. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(CustomStructureParam="Value"))
	static bool SocketMessageToStruct(const FVaRestSocketMessage& Message, int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase, bool bStrict = false);
	DECLARE_FUNCTION(execSocketMessageToStruct);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket", meta=(CustomStructureParam="Value", AutoCreateRefTerm="Value"))
	static EVaRestSendResult ReplyToSocketMessage(const FVaRestSocketMessage& Message, const int32& Value);
	DECLARE_FUNCTION(execReplyToSocketMessage);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") static EVaRestSendResult ReplySocketError(const FVaRestSocketMessage& Message, const FString& Code, const FString& Reason);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestHeartbeat TextHeartbeat(const FString& Ping = TEXT("ping"), const FString& Pong = TEXT("pong"));
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestHeartbeat JsonHeartbeat(const FString& PingRoute = TEXT("ping"), const FString& PongRoute = TEXT("pong"));
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestHeartbeat ServerHeartbeat(const FString& Ping = TEXT("ping"), const FString& Pong = TEXT("pong"), bool bJson = false);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestSocketRouting RoutingByField(const FString& Field = TEXT("type"));
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static FVaRestSocketRouting JsonRpcRouting();
};
