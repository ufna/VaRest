// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "VaRestHttpTypes.h"

#include "VaRestAuthTypes.generated.h"

/** Spec 4.2 / OpenAPI N15 (plan decision 4): what a request does with its client's auth */
UENUM(BlueprintType)
enum class EVaRestAuthMode : uint8
{
	/** Credentials, session headers, refresh and replay. With an auth that has no credentials (and cannot refresh): Kind=Auth, nothing sent. */
	Required,
	/** The same, but without credentials the request goes out without them */
	IfAvailable,
	/** NoAuth(): no credentials, no session headers, no park, no replay (a login request) */
	None
};

/** Spec 4.2 ApiKey */
UENUM(BlueprintType)
enum class EVaRestApiKeyPlacement : uint8
{
	Header,
	Query
};

/** Spec 4.2 BearerRefresh / OpenAPI N15: how the refresh request carries the refresh token */
UENUM(BlueprintType)
enum class EVaRestRefreshStyle : uint8
{
	/** application/json: { <RefreshBodyField>: "<refresh token>" } */
	Json,
	/** application/x-www-form-urlencoded, RFC 6749 6: grant_type=refresh_token&refresh_token=...[&client_id=...] */
	OAuth2Form
};

/** WebSocket spec 19 item 2 rev 2 (22 A1), plan decision 5 */
UENUM(BlueprintType)
enum class EVaRestSessionChange : uint8
{
	/** The same identity, a new token: the auth refreshed it, or the game said so (bSameIdentity) */
	Refreshed,
	/** Another identity's token: SetBearerToken, SetSession or ImportSession by the game (a login) */
	Replaced,
	/** ClearSession: logout */
	Cleared
};

/**
 * Spec 4.2 BearerRefresh: the refresh endpoint and where the tokens are. BlueprintType: M5's auth config asset holds one,
 * so the field names freeze then (plan decision 27). Paths are FVaRestJson::Path's and read the raw refresh response (no
 * envelope; plan decision 23): a wrapped API writes "data.accessToken".
 */
USTRUCT(BlueprintType)
struct VARESTHTTP_API FVaRestBearerRefreshConfig
{
	GENERATED_BODY()

	/** POSTed to: a path joined to the client's base URL, or an absolute URL */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString RefreshPath = TEXT("/auth/refresh");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	EVaRestRefreshStyle Style = EVaRestRefreshStyle::Json;

	/**
	 * Json style: the request body's key for the refresh token (OAuth2Form always sends refresh_token). Named for the body
	 * (M4a final review B9), as RefreshTokenPath below is a path in the response: the two are pin names from M5.
	 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString RefreshBodyField = TEXT("refreshToken");

	/** OAuth2Form: client_id, when the server wants one; empty sends none. OAuth's, not VaRest's client id (GetId; M4a final review B9). */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString OAuthClientId;

	/** The new access token in the response (required) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString AccessTokenPath = TEXT("accessToken");

	/** A new refresh token (optional: the current one is kept when the response has none) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString RefreshTokenPath = TEXT("refreshToken");

	/** Seconds the new access token lives (optional), a number or a numeric string */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	FString ExpiresInPath = TEXT("expiresIn");

	/** A response with one of these refreshes the session and replays the request once (spec 4.2: 401, configurable). Empty: refresh only before a known expiry. */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http")
	TArray<int32> RefreshStatuses = { 401 };

	/** Refresh this many seconds before a known expiry (spec 4.2: 30) */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "VaRest|Http", meta = (ClampMin = "0"))
	float RefreshLeadSeconds = 30.f;
};

/** Spec 4.2: a client's session, as FVaRestClient::GetSession copies it out. Game thread. */
struct FVaRestSessionData
{
	FString AccessToken;
	FString RefreshToken;
	/** Client-clock seconds (IVaRestClock) when the access token expires; 0 = unknown */
	double ExpiresAt = 0.0;
	/**
	 * The seconds the access token was given when it was set (a login's or a refresh's expires_in); 0 = unknown, as for an
	 * imported session, whose time left is not its lifetime (re-review N-10). BearerRefresh refreshes at most half of it
	 * early (concurrency review M-7).
	 */
	double Lifetime = 0.0;
	/** dawg's X-Session-ID pattern: sent with every request whose auth mode is not None */
	FVaRestHeaders Headers;
	/** Bumped by every change of the session: tokens, a clear, an expiry, a session header */
	uint32 Generation = 0;
	/**
	 * Bumped by token changes, clears and expiries only, not by a session header (concurrency review M-6): an attempt that
	 * applied an older one replays without a refresh (Task 17), and a header change must not use up that one replay
	 */
	uint32 CredentialGeneration = 0;

	bool HasAccessToken() const { return !AccessToken.IsEmpty(); }
	bool HasRefreshToken() const { return !RefreshToken.IsEmpty(); }
};

/** Game thread */
using FVaRestSessionChangedEvent = TMulticastDelegate<void(EVaRestSessionChange)>;
/** Game thread: a refresh was rejected, or NotifySessionExpired */
using FVaRestSessionExpiredEvent = TMulticastDelegate<void()>;
