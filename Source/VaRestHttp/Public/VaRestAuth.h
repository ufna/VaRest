// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestClient.h"

/**
 * What an auth writes into (WebSocket spec 19 item 2): one HTTP attempt, or one WebSocket handshake. Game thread; valid
 * only during IVaRestAuth::Apply.
 */
struct FVaRestAuthTarget
{
	FVaRestAuthTarget(FVaRestHeaders& InHeaders, TArray<TPair<FString, FString>>& InQuery, const FVaRestSessionData& InSession)
		: Headers(InHeaders)
		, Query(InQuery)
		, Session(InSession)
	{
	}

	/** Credential headers go here, with Set: a header of the same name the request set is replaced */
	FVaRestHeaders& Headers;
	/** Pairs appended to the URL, each percent-encoded by the core (an API key in the query) */
	TArray<TPair<FString, FString>>& Query;
	/** The client's session as it is when this attempt starts (plan decision 6) */
	const FVaRestSessionData& Session;
	/** The request (IN-10); 0 for a socket handshake */
	uint64 RequestId = 0;
};

/**
 * Spec 4.2 / WebSocket spec 19 item 2: how a client authenticates. Every function runs on the game thread. Apply runs once
 * per attempt, when the attempt leaves the limiter's queue (and once per socket handshake), with the session as it is
 * then. Configure with FVaRestClient::WithAuth. Task 15 adds the refresh functions.
 */
class IVaRestAuth
{
public:
	virtual ~IVaRestAuth() = default;

	/** For logs, errors and traces */
	virtual FName GetAuthName() const = 0;

	/**
	 * Game thread: this auth's credentials into Target; false when it has none (Kind=Auth under EVaRestAuthMode::Required).
	 * Called from the client's AuthApply stage as the attempt leaves the limiter's queue, and from FVaRestClient::ApplyAuth.
	 */
	virtual bool Apply(FVaRestAuthTarget& Target) const = 0;

	/**
	 * Ruling Q7: non-empty when this auth's configuration cannot work (a programmer error). Every request whose auth mode is
	 * Required or IfAvailable then fails Kind=Config, detail AuthInvalid, and nothing is sent. The built-ins set it; a
	 * Custom auth keeps the default and reports missing credentials by returning false from Apply (Kind=Auth).
	 */
	virtual FString GetConfigError() const { return FString(); }

	/** IN-13 layer 2: key names this auth puts secrets under (an API key's header or query name, a token path's last segment) */
	virtual void GetRedactedKeys(TArray<FString>& OutKeys) const {}

	/**
	 * IN-13 layer 3: secret values this auth holds itself (an API key, Basic credentials); the session's tokens are the
	 * core's. Read once, by WithAuth. Each slot is stored as "VaRest.Auth.<Slot>" (a name that already starts with
	 * "VaRest.Auth." is kept), so an auth never replaces the session's or the game's values; a later WithAuth or
	 * WithoutAuth retires the slots it does not set again, still scrubbed for 10 minutes (Inspector D21).
	 * Two limits. Retired and rotated values share one first-in-first-out list of FVaRestSecretSet::MaxPrevious (4) for the
	 * whole client -- the game's, the session's and the auth's together -- so a retire beyond it forgets the oldest of them
	 * at once rather than after 10 minutes. And slot names must be distinct: of two values under one name only the last
	 * stays current, the other is retired (an ensure reports it).
	 */
	virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const {}
};

/** Spec 4.2's built-in auths */
class VARESTHTTP_API FVaRestAuth
{
public:
	using FRef = TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>;

	/** Authorization: Bearer <the session's access token> (SetBearerToken, SetSession); WithBearer is the short form */
	static FRef Bearer();
	/**
	 * The key in a header or in the query. A header name that is not an RFC 9110 token, an empty query key, an empty key (one
	 * missing from the configuration is not a logout), or a key with a control character logs an Error here, and every
	 * request of the client fails Kind=Config (ruling Q7).
	 * Prefer Header placement. VaRest redacts a query key in its own logs and traces, but two places it does not control see
	 * the URL as sent: FVaRestResponse::EffectiveUrl, which the game gets as the transport reports it, and the engine's own
	 * timeout lines, which print the whole URL (HttpRequestCommon.cpp:580 at Warning for the total timeout, :489 at Log for
	 * the activity timeout).
	 */
	static FRef ApiKey(EVaRestApiKeyPlacement InPlacement, FStringView InName, FStringView InKey);
	/** Authorization: Basic base64(user:password), UTF-8. A user holding ':' (RFC 7617 2) logs an Error and fails each request Kind=Config (ruling Q7). */
	static FRef Basic(FStringView InUser, FStringView InPassword);
};
