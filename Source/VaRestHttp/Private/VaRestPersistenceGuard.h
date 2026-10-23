// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestBody.h"
#include "VaRestHttpTypes.h"
#include "VaRestJsonRedact.h"

#include <atomic>

namespace VaRest::Http
{
	/** Immutable when published. Copy for each candidate: scans consume one shared finite work budget. */
	struct FPersistenceGuard
	{
	    FVaRestRedactRules Rules;
	    TArray<FString> ExtraKeys;
	    TArray<TArray<uint8>> Needles;
	    bool bRefusePersistence=false;
	    int64 NeedleBytes=0;
	    mutable bool bWorkExhausted=false;
	    mutable int64 RemainingWork=128LL*1024*1024; // one private guard copy per candidate, worker-owned during scans
	    TSharedPtr<std::atomic<bool>,ESPMode::ThreadSafe> AbortSignal;
	    void AddSecret(FStringView Value);
	    void AddKey(FStringView Key);
	    void UnionWith(const FPersistenceGuard& Other);
	    bool ContainsSecret(TConstArrayView<uint8> Bytes) const;
	    bool AllowsBody(EVaRestBodyKind Kind, FStringView ContentType, TConstArrayView<uint8> Bytes) const;
	    bool AllowsUrl(FStringView Url) const;
	    FVaRestHeaders SafeHeaders(const FVaRestHeaders& Input) const;
	};
	int64 PersistenceGuardReservation(EVaRestBodyKind Kind, FStringView ContentType, int64 BodyBytes);
}
