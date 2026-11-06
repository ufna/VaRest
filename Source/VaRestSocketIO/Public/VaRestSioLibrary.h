// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "VaRestApiTypes.h"
#include "VaRestSioClient.h"
#include "VaRestSioLibrary.generated.h"

UCLASS()
class VARESTSOCKETIO_API UVaRestSioLibrary : public UBlueprintFunctionLibrary
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(WorldContext="WorldContextObject", DisplayName="Get Socket.IO"))
	static UVaRestSioClient* GetSocketIO(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, FVaRestError& Error);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO", meta=(WorldContext="WorldContextObject", AutoCreateRefTerm="Headers,Auth", DisplayName="Make Socket.IO"))
	static UVaRestSioClient* MakeSocketIO(const UObject* WorldContextObject, const FString& Url, const FString& Namespace, const TArray<FVaRestHeader>& Headers, const FVaRestJson& Auth, FVaRestError& Error, FName Name = NAME_None);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(BlueprintInternalUseOnly="true", WorldContext="WorldContextObject", CustomStructureParam="Payload", AutoCreateRefTerm="Payload"))
	static EVaRestSendResult EmitAssetEvent(const UObject* WorldContextObject, UVaRestApi* Api, FName Entry, const FString& Event,
		const int32& Payload, FVaRestError& Error, const FString& Namespace = TEXT(""), bool bVolatile = false, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First, UVaRestSioClient* Socket = nullptr);
	DECLARE_FUNCTION(execEmitAssetEvent);
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(CustomStructureParam="Payload", AutoCreateRefTerm="Payload"))
	static EVaRestSendResult AckReply(const FVaRestSioAck& Ack, const int32& Payload, EVaRestSioArgs ArgsMode = EVaRestSioArgs::First);
	DECLARE_FUNCTION(execAckReply);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket.IO") static EVaRestSendResult AckReplyArgs(const FVaRestSioAck& Ack, const FVaRestJson& Args);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") static bool IsAckValid(const FVaRestSioAck& Ack);
	/** Explicit synchronous GTK conversion of one already-parsed argument, including binary placeholders. */
	UFUNCTION(BlueprintCallable, CustomThunk, Category="VaRest|Socket.IO", meta=(CustomStructureParam="Value"))
	static bool EventArgToStruct(const FVaRestSioEvent& Event, int32 Index, int32& Value, EVaRestJsonNaming Naming = EVaRestJsonNaming::CamelCase, bool bStrict = false);
	DECLARE_FUNCTION(execEventArgToStruct);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") static int32 GetEventArgCount(const FVaRestSioEvent& Event);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") static TArray<uint8> GetEventAttachment(const FVaRestSioEvent& Event, int32 Num);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket.IO") static FString DescribeDisconnect(const FVaRestSioDisconnect& Disconnect);
};
