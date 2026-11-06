// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestApiSocketConfig.h"
#include "VaRestSocketCodec.h"
#include "VaRestSocketEndpoint.generated.h"

class FVaRestSocket;

UENUM(BlueprintType)
enum class EVaRestSocketMessageDirection : uint8 { In, Out, Request, InRequest };

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketMessageDeclaration
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Message") FString Channel;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Message") FString Route;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Message") EVaRestSocketMessageDirection Direction = EVaRestSocketMessageDirection::In;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Message") FVaRestTypeRef PayloadType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Message") FVaRestTypeRef ReplyType;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Message") FString Description;
};

/** Shared connection fields for WebSocket and protocol modules such as Socket.IO. */
UCLASS(Abstract, BlueprintType, EditInlineNew, DefaultToInstanced)
class VARESTWEBSOCKET_API UVaRestSocketEndpointBase : public UVaRestApiSocketConfig
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") FString Path = TEXT("/");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TArray<FVaRestQueryParameter> Query;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TArray<FVaRestHeader> Headers;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") bool bOverrideOrigin = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection", meta=(EditCondition="bOverrideOrigin")) FString Origin;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Authentication") bool bNoAuth = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reconnect") bool bOverrideReconnect = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Reconnect", meta=(EditCondition="bOverrideReconnect")) FVaRestReconnectPolicy Reconnect;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Limits") bool bOverrideLimits = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Limits", meta=(EditCondition="bOverrideLimits")) FVaRestSocketLimits Limits;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sending") bool bOverrideQueue = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Sending", meta=(EditCondition="bOverrideQueue")) FVaRestSocketQueuePolicy Queue;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lifecycle") bool bOverrideBackground = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Lifecycle", meta=(EditCondition="bOverrideBackground")) EVaRestSocketBackground Background = EVaRestSocketBackground::Keep;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ping") bool bOverrideProtocolPing = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ping", meta=(EditCondition="bOverrideProtocolPing")) FVaRestProtocolPing ProtocolPing;
	virtual FVaRestSocketAuthPlacement GetAuthPlacement() const { return {}; }
	virtual void ConfigureSocket(FVaRestSocket& Socket) const;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
	/** Joins API base/path, then maps http(s) to ws(s). No I/O or connection is started. */
	FString ResolveUrl(FStringView BaseUrl) const;
};

UCLASS(BlueprintType, EditInlineNew, DefaultToInstanced, meta=(DisplayName="VaRest Socket"))
class VARESTWEBSOCKET_API UVaRestSocketEndpoint : public UVaRestSocketEndpointBase
{
	GENERATED_BODY()
public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Connection") TArray<FString> Protocols;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Routing") FVaRestSocketRouting Routing;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Routing") FString ReadyRoute;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Messages") TArray<FVaRestSocketMessageDeclaration> Messages;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Authentication") FVaRestSocketAuthPlacement AuthPlacement;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Heartbeat") bool bOverrideHeartbeat = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Heartbeat", meta=(EditCondition="bOverrideHeartbeat")) FVaRestHeartbeat Heartbeat;
	virtual FName GetProtocolTag() const override { return TEXT("WebSocket"); }
	virtual FVaRestSocketAuthPlacement GetAuthPlacement() const override { return AuthPlacement; }
	virtual void ConfigureSocket(FVaRestSocket& Socket) const override;
	virtual void Validate(FVaRestApiValidationContext& Context) const override;
};
