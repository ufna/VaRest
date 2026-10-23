// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

#include "VaRestCache.generated.h"

UENUM(BlueprintType)
enum class EVaRestCacheMode : uint8
{
	None,
	Memory,
	MemoryAndDisk
};

/** Private-client HTTP cache policy. Disabled until a client or request opts in. */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestCachePolicy
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Cache")
	EVaRestCacheMode Mode = EVaRestCacheMode::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Cache")
	int64 MemoryBytes = 8 * 1024 * 1024;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Cache")
	int64 DiskBytes = 64 * 1024 * 1024;

	/** Fallback only when neither max-age nor a valid Expires supplies a lifetime. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Cache")
	double DefaultTtlSeconds = 0.0;

	/** Allows Connection/Offline fallback only; no-cache and must-revalidate still require validation. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Cache")
	bool bServeStaleOnError = false;

	static FVaRestCachePolicy Memory()
	{
		FVaRestCachePolicy Result;
		Result.Mode = EVaRestCacheMode::Memory;
		return Result;
	}

	static FVaRestCachePolicy MemoryAndDisk()
	{
		FVaRestCachePolicy Result = Memory();
		Result.Mode = EVaRestCacheMode::MemoryAndDisk;
		return Result;
	}
};
