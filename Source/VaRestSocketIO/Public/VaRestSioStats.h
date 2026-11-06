// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "CoreMinimal.h"
#include "VaRestSioStats.generated.h"

/** Logical packet counters across reconnects. Out counts connection Send admission, without promising peer receipt.
 * Namespace snapshots filter logical counters; ping statistics describe their shared physical connection. */
USTRUCT(BlueprintType)
struct VARESTSOCKETIO_API FVaRestSioStats
{
	GENERATED_BODY()
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int32 ConnectedNamespaces = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 EventsIn = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 EventsOut = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int32 AcksPending = 0;
	/** Timed out attempts, including attempts followed by an explicit ack retry. */
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 AcksTimedOut = 0;
	/** Server CONNECT_ERROR packets and namespace CONNECT timeouts. */
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 ConnectErrors = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 RecoveriesSucceeded = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 RecoveriesFailed = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 PingsAnswered = 0;
	/** Measured complete-ping receive to service lws_write admission, seconds; GTK fallback has no service sample. */
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") double LongestPongDelay = 0.0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") bool bHasPongDelay = false;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 AttachmentsIn = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 AttachmentsOut = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 AttachmentBytesIn = 0;
	UPROPERTY(BlueprintReadOnly, Category="Socket.IO") int64 AttachmentBytesOut = 0;
};
