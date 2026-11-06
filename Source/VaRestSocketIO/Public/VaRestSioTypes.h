// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestJson.h"
#include "VaRestSocketClientTypes.h"
#include "VaRestSioCodec.h"
#include "VaRestSioTypes.generated.h"

UENUM(BlueprintType)
enum class EVaRestSioNamespaceState : uint8 { Disconnected, Connecting, Connected };

UENUM(BlueprintType)
enum class EVaRestSioAuthPlacement : uint8 { Payload, Header, Query, None };

UENUM(BlueprintType)
enum class EVaRestSioRetry : uint8 { Default, RefreshAndRetry, Retry, Stop };

UENUM(BlueprintType)
enum class EVaRestSioDisconnectReason : uint8
{
	ClientDisconnect, SocketClosed, SessionReplaced, ServerDisconnect, PingTimeout, ParseError, TransportClose
};

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioLimits : public FVaRestSocketLimits
{
	GENERATED_BODY()
	FVaRestSioLimits() { MaxMessageBytes = 8 * 1024 * 1024; MaxPendingInboundBytes = 32 * 1024 * 1024; ReadyTimeout = 20.0; }
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO", meta=(ClampMin="0", ClampMax="4095")) int32 MaxAttachments = 10;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO", meta=(ClampMin="1")) int64 MaxPacketBytes = 16 * 1024 * 1024;
	/** Local ceiling; the server's advertised maxPayload may impose a smaller limit. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO", meta=(ClampMin="1")) int64 MaxOutboundFrameBytes = 16 * 1024 * 1024;
	/** Only for a known server that omits maxPayload and accepts frames above its 1 MB default. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO") bool bOverrideFallbackPayloadLimit = false;
	FVaRestSioCodecOptions ToCodecOptions() const;
	bool Validate(FString& Error) const;
};

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioAuthPlacement
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO") EVaRestSioAuthPlacement Mode = EVaRestSioAuthPlacement::Payload;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO") FString Name = TEXT("token");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Socket.IO") FString Prefix;
	static FVaRestSioAuthPlacement Payload(FString Field = TEXT("token"), FString TokenPrefix = {})
	{ FVaRestSioAuthPlacement Out; Out.Name = MoveTemp(Field); Out.Prefix = MoveTemp(TokenPrefix); return Out; }
	static FVaRestSioAuthPlacement Header()
	{ FVaRestSioAuthPlacement Out; Out.Mode = EVaRestSioAuthPlacement::Header; return Out; }
	static FVaRestSioAuthPlacement Query(FString Key = TEXT("token"))
	{ FVaRestSioAuthPlacement Out; Out.Mode = EVaRestSioAuthPlacement::Query; Out.Name = MoveTemp(Key); return Out; }
	static FVaRestSioAuthPlacement None()
	{ FVaRestSioAuthPlacement Out; Out.Mode = EVaRestSioAuthPlacement::None; return Out; }
};

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioConnect
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Namespace;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Sid;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") bool bRecovered = false;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") bool bReconnect = false;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int32 Attempt = 0;
};

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioConnectError
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Namespace;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Message;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FVaRestJson Data;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") bool bWillRetry = false;
};

USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioDisconnect
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString Namespace;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") EVaRestSioDisconnectReason Reason = EVaRestSioDisconnectReason::TransportClose;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FString ReasonText;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") bool bWillReconnect = false;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") FVaRestSocketClose Close;
};

struct FVaRestSioConnectContext
{
	FString Namespace;
	int32 Attempt = 0;
	bool bReconnect = false;
};
