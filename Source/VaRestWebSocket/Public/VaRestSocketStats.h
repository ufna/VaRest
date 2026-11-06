// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketStats.generated.h"

/** Payload counters count assembled transport messages, including codec control and attachment packets.
 * FramesOut/BytesOut count accepted connection sends; they do not promise peer receipt. Gauges are sampled. */
USTRUCT(BlueprintType)
struct VARESTWEBSOCKET_API FVaRestSocketStats
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bOpen = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 Attempts = 0;
	/** Successful opens after this socket's first open. */
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 Reconnects = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 FramesIn = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 FramesOut = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 BytesIn = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 BytesOut = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 DecodeErrors = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 SendRefusals = 0;
	/** Exact-text service replies admitted by lws_write, across this socket's attempts; excludes GTK fallbacks. */
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 AutoRepliesOut = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") double LongestAutoReplyDelay = 0.0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 Queued = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 QueuedBytes = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 BufferedBytes = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int64 PendingInboundBytes = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") int32 PendingRequests = 0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") bool bHasLatency = false;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") double Latency = 0.0;
	UPROPERTY(BlueprintReadOnly, Category="VaRest|Socket") double LatencyAverage = 0.0;
};
