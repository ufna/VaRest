// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestHttpTypes.h"

namespace VaRest::Http
{
	/** Pure response metadata; monotonic elapsed time never goes negative. */
	struct FCacheMetadata
	{
		bool bNoStore = false;
		bool bNoCache = false;
		bool bMustRevalidate = false;
		double Lifetime = 0.0;
		double InitialAge = 0.0;
		FDateTime StoredUtc;
		double StoredAt = 0.0;
		FString ETag;
		FString LastModified;
		TArray<FString> Vary;

		bool IsFresh(double Now) const
		{
			return !bNoCache && !bNoStore && InitialAge + FMath::Max(0.0, Now - StoredAt) < Lifetime;
		}

		bool CanServeStale() const { return !bNoCache && !bNoStore && !bMustRevalidate; }
	};

	/** Strict decimal seconds in [0, ten years]; failure leaves OutSeconds unchanged. */
	bool ReadDeltaSeconds(FStringView Text, double& OutSeconds);
	FCacheMetadata ReadCacheMetadata(const FVaRestHeaders& Headers, double DefaultTtl,
		double Now, FDateTime NowUtc);
	/** Vary must refer only to known built/session request headers; no transport guesses. */
	bool CacheHeadersEligible(const FVaRestHeaders& Request, const FVaRestHeaders& Response);
	/** Merge metadata only; caller retains bytes. HEAD's length describes the GET representation. */
	FVaRestHeaders MergeNotModified(const FVaRestHeaders& Stored, const FVaRestHeaders& Fresh,
		int64 BodyBytes, bool bHead);
} // namespace VaRest::Http
