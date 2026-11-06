// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"
#include "VaRestSocketTypes.h"
#include "VaRestSocketClientTypes.generated.h"

UENUM(BlueprintType)
enum class EVaRestSocketState : uint8 { Disconnected, Connecting, Connected, Reconnecting };

UENUM(BlueprintType)
enum class EVaRestSocketWait : uint8 { None, Backoff, Offline, Background, Auth };

UENUM(BlueprintType)
enum class EVaRestSocketJitter : uint8 { Full, Ratio };

UENUM(BlueprintType)
enum class EVaRestSocketBackground : uint8 { Keep, Disconnect };

UENUM(BlueprintType)
enum class EVaRestSend : uint8 { Default, Volatile, Handshake };

UENUM(BlueprintType)
enum class EVaRestSendResult : uint8 { Sent, Queued, Dropped, Rejected };

UENUM(BlueprintType)
enum class EVaRestSocketQueueOverflow : uint8 { DropOldest, DropNewest, Reject };

UENUM(BlueprintType)
enum class EVaRestHeartbeatMode : uint8 { None, ClientPing, ServerPing };

UENUM(BlueprintType)
enum class EVaRestSocketAuthPlacement : uint8 { Header, Query, FirstMessage };

/** Placement of a Bearer credential; cookies and other session headers keep their normal semantics. */
USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketAuthPlacement
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") EVaRestSocketAuthPlacement Mode = EVaRestSocketAuthPlacement::Header;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString Name = TEXT("token");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString Route = TEXT("auth");
	static FVaRestSocketAuthPlacement Header() { return {}; }
	static FVaRestSocketAuthPlacement Query(FString Key = TEXT("token"))
	{ FVaRestSocketAuthPlacement Out; Out.Mode = EVaRestSocketAuthPlacement::Query; Out.Name = MoveTemp(Key); return Out; }
	static FVaRestSocketAuthPlacement FirstMessage(FString RouteName = TEXT("auth"), FString Field = TEXT("token"))
	{ FVaRestSocketAuthPlacement Out; Out.Mode = EVaRestSocketAuthPlacement::FirstMessage; Out.Name = MoveTemp(Field); Out.Route = MoveTemp(RouteName); return Out; }
};

enum class EVaRestChannelEnd : uint8 { WillRetry, Ended };

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestHeartbeat
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") EVaRestHeartbeatMode Mode = EVaRestHeartbeatMode::None;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") bool bJson = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString Ping = TEXT("ping");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") FString Pong = TEXT("pong");
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(DisplayName="Interval", ClampMin="0")) double IntervalSeconds = 20.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(DisplayName="Timeout", ClampMin="0")) double TimeoutSeconds = 10.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") bool bAnyInboundIsAlive = true;
	static FVaRestHeartbeat Text(FString Ping, FString Pong);
	static FVaRestHeartbeat Json(FString PingRoute, FString PongRoute);
	static FVaRestHeartbeat ServerPing(FString Ping, FString Pong, bool bJson = false);
	FVaRestHeartbeat& Interval(double Seconds) { IntervalSeconds = Seconds; return *this; }
	FVaRestHeartbeat& Timeout(double Seconds) { TimeoutSeconds = Seconds; return *this; }
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketQueuePolicy
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int32 MaxQueued = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int64 MaxQueuedBytes = 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") EVaRestSocketQueueOverflow Overflow = EVaRestSocketQueueOverflow::DropOldest;
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketLimits
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int64 MaxMessageBytes = 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int64 MaxPendingInboundBytes = 16 * 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int64 MaxSendBytes = 16 * 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int64 MaxBufferedBytes = 16 * 1024 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int64 SendHighWater = 256 * 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int32 MaxPendingRequests = 1024;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="1")) int32 MaxChannels = 256;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) double ReadyTimeout = 0.0; // zero uses ConnectTimeout
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestReconnectPolicy
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") bool bEnabled = true;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) int32 MaxAttempts = 0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) double BaseDelay = 0.5;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="1")) double Multiplier = 2.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) double MaxDelay = 30.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") EVaRestSocketJitter Jitter = EVaRestSocketJitter::Full;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0", ClampMax="1")) double JitterRatio = 0.2;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket", meta=(ClampMin="0")) double StableAfter = 5.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") bool bStopOnNormalClose = false;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") TArray<int32> TerminalCloseCodes;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Socket") TArray<int32> ReconnectCloseCodes;
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketStatus
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketState State = EVaRestSocketState::Disconnected;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 Attempt = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bAttemptInFlight = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketWait WaitReason = EVaRestSocketWait::None;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") double NextAttemptIn = 0.0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bReady = false;
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestProtocolPing
{
	GENERATED_BODY()
	/** Zero disables protocol ping. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ping", meta=(ClampMin="0")) double Interval = 20.0;
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Ping", meta=(ClampMin="0.05")) double Timeout = 10.0;
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketOpen
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 Attempt = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bReconnect = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") double ConnectSeconds = 0.0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FString Protocol;
	/** Protocol-owned public channel id; never a private Engine.IO sid or recovery pid. */
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FString PublicId;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bRecovered = false;
};

USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketClose
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 Code = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FString Reason;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") FVaRestJson ReasonJson;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bWasClean = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bWhileReadPaused = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketCloseBy By = EVaRestSocketCloseBy::Transport;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") EVaRestSocketFailure Failure = EVaRestSocketFailure::None;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 HttpStatus = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bWillReconnect = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") double ReconnectDelay = 0.0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 Attempt = 0;
};

/** Per-attempt mutable view. It is consumed after the hook returns; never keep references to its fields. */
struct FVaRestSocketHandshake
{
	FString Url;
	TArray<TPair<FString, FString>> Query;
	FVaRestHeaders Headers;
	TArray<FString> Protocols;
	TOptional<FString> Origin;
	int32 Attempt = 0;
	bool bReconnect = false;
};

/** Optional send-batch pin. Zero channel epoch leaves the channel unpinned; a connection generation is exact. */
struct FVaRestSocketBatchPin
{
	uint64 ConnectionGeneration = 0;
	FString Channel;
	uint64 ChannelEpoch = 0;
};
