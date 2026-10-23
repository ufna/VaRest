// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"
#include "VaRestOffline.generated.h"

USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestOfflinePolicy
{
    GENERATED_BODY()
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    int32 MaxQueued = 256;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    double MaxAgeSeconds = 86400.0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    int64 MaxItemBytes = 32 * 1024 * 1024;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="VaRest|Offline")
    int64 MaxBodyBytes = 64 * 1024 * 1024;
};
// FVaRestResponse is a plain C++ type. M5 supplies the reflected event wrapper.
struct VARESTHTTP_API FVaRestQueuedResult
{
    FString QueueId;
    FName Tag;
    bool bSuccess = false;
    FVaRestResponse Response;
    FVaRestError Error;
};
using FVaRestQueuedResultEvent = TMulticastDelegate<void(const FVaRestQueuedResult&)>;
