// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"
#include "VaRestClient.h"

#include <atomic>

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

/** How a refresh ended (plan decision 3, ruling I-7) */
enum class EVaRestAuthRefreshOutcome : uint8
{
	/** New credentials */
	Refreshed,
	/**
	 * The refresh endpoint said no -- a 4xx other than 408 or 429, or a success that is not JSON or has no usable access
	 * token: the session expires, parked requests fail Kind=Auth, OnSessionExpired fires
	 */
	Rejected,
	/**
	 * Anything else -- no connection, a timeout, 408, 429, 5xx, a canceled or locally refused refresh request (HTTP
	 * disabled, a domain not allowed): the session stays; parked requests fail with FailedKind
	 */
	Failed
};

/** What IVaRestAuth::Refresh reports */
struct VARESTHTTP_API FVaRestAuthRefreshResult
{
	/**
	 * Failed, FailedKind Connection, until set (Task 15 review M-5, ruling): a result built by hand that forgets its outcome
	 * keeps the session, as a dropped Done does (concurrency review I-4) -- it never logs the player out. Use the factories
	 * below.
	 */
	EVaRestAuthRefreshOutcome Outcome = EVaRestAuthRefreshOutcome::Failed;
	FString AccessToken;
	/** Empty: keep the current refresh token (an OAuth2 server need not rotate it) */
	FString RefreshToken;
	/** 0 = unknown */
	double ExpiresInSeconds = 0.0;
	/**
	 * Failed: what parked requests fail with -- Connection or Timeout (retryable: Retry and M4b's offline queue act on
	 * them) or Config (not retryable: sending again fails again). ClassifyAttempt decides bRetryable, as for any attempt.
	 */
	EVaRestErrorKind FailedKind = EVaRestErrorKind::Connection;
	/** Why, for the error message and the trace marker; never a token */
	FString Message;

	static FVaRestAuthRefreshResult Refreshed(FString InAccessToken, FString InRefreshToken = FString(), double InExpiresInSeconds = 0.0);
	static FVaRestAuthRefreshResult Rejected(FString InMessage);
	/** Ruling I-7: the session stays. InKind Timeout and Config are kept; any other kind is Connection. */
	static FVaRestAuthRefreshResult Failed(EVaRestErrorKind InKind, FString InMessage);
};

/**
 * Handed to IVaRestAuth::Refresh: the session the refresh starts from and the one way to send the refresh request (plan
 * decision 23). Game thread. Holds its client weakly (concurrency review M-10): an auth may keep it, or a copy, to answer
 * later. Only the core makes one (Task 15 review M-1): its refresh id is a fresh request id, never one in use.
 */
class VARESTHTTP_API FVaRestAuthRefreshContext
{
public:
	/**
	 * A refresh request on the client. It bypasses the auth stages (no credentials, no session headers, no park, no
	 * replay), the envelope (the body is read raw), retry and the limiter's queue (ruling M-8: one more connection per
	 * refresh request the auth sends, so a client whose slots all hold long polls still refreshes), and carries trace flag
	 * AuthRefresh. Chaining Retry() with an enabled policy, Envelope() or an AuthMode() other than None onto it would undo
	 * that: each fails the request Kind=Config, and nothing is sent. The first one a context builds has the refresh's id
	 * as its request id (spec review I-2). On a client that is gone: a builder on an empty client, whose request fails
	 * Kind=Config (and, as any Send() on an empty client, logs an Error). A refresh sent through the client itself would
	 * park behind its own refresh.
	 */
	template <typename T = FVaRestJson>
	TVaRestRequest<T> Request(FStringView Verb, FStringView Path) const
	{
		// Re-review N-9: one flag for every copy of this context, so the id goes to one request, never to two
		const uint64 Preset = IdTaken->exchange(true) ? 0 : RefreshId;
		return GetClient().Request<T>(Verb, Path).MarkAuthRefresh(Preset);
	}

	/** The session the refresh started from (its refresh token) */
	const FVaRestSessionData& GetSession() const { return Session; }
	/** IN-2 AuthRefreshId: the refresh request's RequestId when the auth sends one through Request() (spec review I-2) */
	uint64 GetRefreshId() const { return RefreshId; }

private:
	/** Task 15 review M-1, by shape: FClientAccess::MakeRefreshContext (the coordinator, VaRestHttp's tests) is the one maker */
	friend struct VaRest::Http::FClientAccess;

	FVaRestAuthRefreshContext(const FVaRestClient& InClient, FVaRestSessionData InSession, uint64 InRefreshId);

	/** The client while it lives, else an empty one (VaRestAuth.cpp) */
	FVaRestClient GetClient() const;

	TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State;
	FVaRestSessionData Session;
	uint64 RefreshId = 0;
	/**
	 * Request() gave the id away already. Shared by the copies (an auth that captures the context by value for a later
	 * callback): two exchanges with one RequestId would replace each other in the client's in-flight map (re-review N-9).
	 */
	TSharedRef<std::atomic<bool>, ESPMode::ThreadSafe> IdTaken;
};

/**
 * Spec 4.2 / WebSocket spec 19 item 2: how a client authenticates. Every function runs on the game thread. Apply runs once
 * per attempt, when the attempt leaves the limiter's queue (and once per socket handshake), with the session as it is
 * then. Configure with FVaRestClient::WithAuth. An auth that refreshes overrides CanRefresh, NeedsRefresh, IsAuthFailure and
 * Refresh (plan decision 23); the core runs the refresh (Task 16).
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

	/** Refresh() can get new credentials for this session (a refresh token is there) */
	virtual bool CanRefresh(const FVaRestSessionData& Session) const { return false; }

	/** Refresh before the next attempt: no access token yet, or a known expiry within the lead time (spec 4.2: 30 s) */
	virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const { return false; }

	/** A status that means "credentials refused": the request refreshes and replays once (spec 4.2: 401, configurable) */
	virtual bool IsAuthFailure(int32 Status) const { return Status == 401; }

	/**
	 * Gets new credentials; the core shares one Refresh among every request and socket that needs it (single flight, Task
	 * 16). Call Done exactly once, from any thread: the core finishes the refresh on the game thread, from a later task; a
	 * second call is ignored. Done dropped uncalled -- an error path that forgets it, an SDK that frees its callback -- ends
	 * the refresh as Failed(Connection), the session kept (concurrency review I-4). Holding Done without calling it is
	 * allowed (a re-login screen): parked requests wait, each bounded by its own retry deadline and Cancel() (CancelAll(),
	 * engine exit); one whose owner goes meanwhile gets no callback, and ends when its park does -- until then the client
	 * holds the request, its callbacks and everything they capture (Task 17 review M-4). No watchdog ends the refresh
	 * (ruling I-4). Send the refresh request with Context.Request(), never through the client.
	 */
	virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done)
	{
		Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Config, TEXT("this auth cannot refresh")));
	}

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
	 * timeout lines, which print the whole URL (GenericPlatform/HttpRequestCommon.cpp:580 at Warning for the total timeout,
	 * :489 at Log for the activity timeout).
	 */
	static FRef ApiKey(EVaRestApiKeyPlacement InPlacement, FStringView InName, FStringView InKey);
	/** Authorization: Basic base64(user:password), UTF-8. A user holding ':' (RFC 7617 2) logs an Error and fails each request Kind=Config (ruling Q7). */
	static FRef Basic(FStringView InUser, FStringView InPassword);
	/**
	 * Spec 4.2 BearerRefresh: the access token as Bearer; on a refresh status (401) or a known expiry within the lead time,
	 * one refresh shared by every request (Task 16): a POST of the refresh token to Config.RefreshPath, the new tokens read
	 * from the raw response at Config's paths. The endpoint's own "no" -- a 4xx other than 408 and 429, or a success that
	 * is not JSON or has no access token -- expires the session; anything else keeps it (no answer, a timeout, 408, 429,
	 * 5xx, a refusal by the configuration such as HTTP disabled), and the parked requests fail with that kind (plan
	 * decision 3, ruling I-7). The proactive lead is RefreshLeadSeconds, at most half the token's lifetime (concurrency
	 * review M-7). Each path's last segment becomes a redaction key (plan decision 22).
	 * What bounds a refresh: its request is built on the client (FVaRestAuthRefreshContext::Request), so it has the client's
	 * WithTimeout() like any request, and with none the engine's defaults (HttpModule.cpp:96-98: no total timeout, 30 s to
	 * connect, 30 s of activity). BearerRefresh sets no total timeout of its own (the config has no field for one): a refresh
	 * endpoint that stops answering ends at the activity timeout, Kind=Connection, the session kept; one that trickles bytes
	 * forever is bounded only by a client total. A parked request is bounded by its own retry Deadline and Cancel().
	 * The response is decoded as JSON off the game thread under the client's MaxJsonBytes (a larger 2xx is a malformed
	 * success: Rejected); only the path reads run on the game thread.
	 */
	static FRef BearerRefresh(const FVaRestBearerRefreshConfig& Config);
};
