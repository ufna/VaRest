// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestSocketSettings.generated.h"

/** Defaults are copied when a socket is made; an existing socket keeps its explicit configuration. */
UCLASS(config=Engine, defaultconfig, meta=(DisplayName="VaRest WebSocket"))
class VARESTWEBSOCKET_API UVaRestSocketSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UVaRestSocketSettings();
	UPROPERTY(config, EditAnywhere, Category="Logging") EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
	UPROPERTY(config, EditAnywhere, Category="Connection", meta=(ClampMin="0", ClampMax="60")) double ConnectTimeout = 10.0;
	UPROPERTY(config, EditAnywhere, Category="Connection", meta=(ClampMin="0", ClampMax="5")) double CloseTimeout = 5.0;
	/** Transport ignores this in Shipping. */
	UPROPERTY(config, EditAnywhere, Category="Connection") bool bAllowSelfSignedTls = false;
	UPROPERTY(config, EditAnywhere, Category="Connection") FVaRestReconnectPolicy Reconnect;
	UPROPERTY(config, EditAnywhere, Category="Sending") FVaRestSocketQueuePolicy Queue;
	UPROPERTY(config, EditAnywhere, Category="Limits") FVaRestSocketLimits Limits;
	UPROPERTY(config, EditAnywhere, Category="Heartbeat") FVaRestHeartbeat Heartbeat;
	UPROPERTY(config, EditAnywhere, Category="Ping") FVaRestProtocolPing ProtocolPing;
	UPROPERTY(config, EditAnywhere, Category="Lifecycle") EVaRestSocketBackground Background = EVaRestSocketBackground::Keep;
};
