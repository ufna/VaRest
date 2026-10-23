// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestHttpTypes.h"

class IVaRestAuth;

namespace VaRest::Http
{
    struct FClientConfig;
    bool RequestIdentityScope(const FClientConfig& Config, const FVaRestSessionData& Session,
        EVaRestAuthMode Mode, const FVaRestHeaders& BuiltHeaders, FString& OutScope);
	/** Strict scheme/ASCII host/port origin; no userinfo. IPv6 alternatives compare conservatively. */
	bool CanonicalOrigin(FStringView Url, FString& OutOrigin);
	bool SameOrigin(FStringView Left, FStringView Right);
	/** Domain-separated, length-framed UTF-8 SHA256; no raw fields escape in the result. */
	FString HashFields(FStringView Domain, TConstArrayView<FString> Fields);
	TArray<FString> CanonicalHeaders(const FVaRestHeaders& Headers);
	/** Consistency only, not authentication: callers own and trust exported credential state. */
	FString SessionBinding(const FVaRestSessionData& Data);
	FString SessionScope(const FVaRestSessionData& Data);
	/** Pure query; every false result clears OutIdentity. Never invokes Apply. */
	bool AuthIdentity(const IVaRestAuth* Auth, EVaRestAuthMode Mode,
		const FVaRestSessionData& Session, FString& OutIdentity);
	FString CacheKey(FStringView Verb, FStringView Url, FStringView Identity,
		const FVaRestHeaders& Built, const FVaRestHeaders& SessionHeaders);
}
