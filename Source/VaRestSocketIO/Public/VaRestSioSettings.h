// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "VaRestSioTypes.h"
#include "VaRestSioSettings.generated.h"

/** Copied when a Socket.IO socket is made; existing sockets retain their own configuration. */
UCLASS(config=Engine, defaultconfig, meta=(DisplayName="VaRest Socket.IO"))
class VARESTSOCKETIO_API UVaRestSioSettings : public UDeveloperSettings
{
	GENERATED_BODY()
public:
	UVaRestSioSettings();
	UPROPERTY(config, EditAnywhere, Category="Connection") FString Path = TEXT("/socket.io/");
	UPROPERTY(config, EditAnywhere, Category="Connection", meta=(ClampMin="0.05", ClampMax="60")) double ConnectTimeout = 20.0;
	UPROPERTY(config, EditAnywhere, Category="Connection", meta=(ClampMin="0", ClampMax="5")) double CloseTimeout = 5.0;
	UPROPERTY(config, EditAnywhere, Category="Authentication") FVaRestSioAuthPlacement Auth;
	UPROPERTY(config, EditAnywhere, Category="Recovery") bool bRecovery = true;
	UPROPERTY(config, EditAnywhere, Category="Acks", meta=(ClampMin="0")) double AckTimeout = 10.0;
	UPROPERTY(config, EditAnywhere, Category="Reconnect") FVaRestReconnectPolicy Reconnect;
	UPROPERTY(config, EditAnywhere, Category="Sending") FVaRestSocketQueuePolicy Queue;
	UPROPERTY(config, EditAnywhere, Category="Limits") FVaRestSioLimits Limits;
	UPROPERTY(config, EditAnywhere, Category="Ping") FVaRestProtocolPing ProtocolPing;
	UPROPERTY(config, EditAnywhere, Category="Logging") EVaRestLogLevel LogLevel = EVaRestLogLevel::Basic;
	UPROPERTY(config, EditAnywhere, Category="Lifecycle") EVaRestSocketBackground Background = EVaRestSocketBackground::Keep;
};
