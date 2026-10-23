// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSession.h"

#include "Tasks/Task.h"
#include "VaRestAuth.h"
#include "VaRestClientState.h"
#include "VaRestIdentity.h"
#include "VaRestTrace.h"
#include "VaRestTracePoints.h"

#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

#include <atomic>

namespace VaRest::Http::SessionPrivate
{
	FName Slot(const TCHAR* Name)
	{
		return FName(*(FString(SessionSecretPrefix()) + Name));
	}

	FName HeaderSlot(FStringView Header)
	{
		return FName(*(FString(SessionSecretPrefix()) + TEXT("Header.") + FString(Header)));
	}

	/**
	 * Concurrency review I-1 (ruled): the waiters of an ended refresh run from ONE later game-thread task that holds them,
	 * so no request callback and no dispatch runs inside the session call that ended the refresh -- M3 decision 3's rule
	 * for configuration calls (VaRestClientState.cpp:268-271, the limiter's grants after a Publish). A waiter whose request
	 * was canceled meanwhile finds it answered and returns; one that LeaveRefresh dropped is not in this list any more.
	 * The task holds the state while the waiters run, as RunSessionChange does (concurrency review C-1): a waiter may drop
	 * the last FVaRestClient, and the state then goes after the last waiter, not under one. Until the task runs it holds the
	 * state weakly, so an ended refresh keeps no client alive: the waiters of a refresh that ended before its state went are
	 * still told (Task 16 review M-4). A refresh still in flight when the state goes never ends: its waiters die with the
	 * state, uncalled, on the game thread (~FClientState sends them there, concurrency review M-9). Only RefreshAuth's
	 * waiters can meet that: a parked request's waiter holds its exchange, and so the state.
	 * At engine exit nothing is told (Task 16 review M-6): no callback runs then (decision 2), as FRefreshDoneGuard::Post
	 * drops the auth's answer, and the waiters die here, uncalled, on the game thread. The shutdown cancel has already taken
	 * every parked request out (its interrupt hook leaves the refresh), so what dies here is RefreshAuth's.
	 */
	void ResolveWaitersLater(FClientState& Owner, TUniquePtr<FRefreshInFlight>&& Ended, FRefreshEnded&& End)
	{
		if (FClientState::IsShuttingDown())
		{
			return;
		}
		UE::Tasks::Launch(TEXT("VaRest.RefreshEnded"),
			[Weak = TWeakPtr<FClientState, ESPMode::ThreadSafe>(Owner.AsShared()), Waiting = MoveTemp(Ended), Result = MoveTemp(End)]() mutable
			{
				const TSharedPtr<FClientState, ESPMode::ThreadSafe> Keep = Weak.Pin();
				for (TPair<uint64, FRefreshWaiter>& Waiter : Waiting->Waiters)
				{
					Waiter.Value(Result);
				}
			}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
	}

	/**
	 * Concurrency review I-4: what the auth's Done owns. The first Deliver wins (any thread); a second is ignored. Destroyed
	 * without one -- the auth dropped Done uncalled -- it ends the refresh as Failed(Connection), the session kept (ruling
	 * Q1), so nothing parks forever behind a refresh that can no longer end. An auth that holds Done uncalled (a re-login
	 * screen) is not ended: no watchdog (ruling I-4). Either way the refresh finishes from a later game-thread task, inside
	 * RunSessionChange (the answer runs listeners, concurrency review C-1). Weak (concurrency review M-10): an auth that
	 * keeps Done does not keep the client alive.
	 */
	class FRefreshDoneGuard
	{
	public:
		FRefreshDoneGuard(TWeakPtr<FClientState, ESPMode::ThreadSafe> InState, uint64 InRefreshId)
			: State(MoveTemp(InState))
			, RefreshId(InRefreshId)
			, Answered(MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false))
		{
		}

		/** Moving leaves the source without a flag: only the guard the auth's Done holds can fire */
		FRefreshDoneGuard(FRefreshDoneGuard&&) = default;
		FRefreshDoneGuard& operator=(FRefreshDoneGuard&&) = default;
		FRefreshDoneGuard(const FRefreshDoneGuard&) = delete;
		FRefreshDoneGuard& operator=(const FRefreshDoneGuard&) = delete;

		~FRefreshDoneGuard()
		{
			if (Answered.IsValid() && !Answered->exchange(true))
			{
				Post(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("the auth dropped its refresh callback")));
			}
		}

		void Deliver(FVaRestAuthRefreshResult&& Result)
		{
			if (Answered.IsValid() && !Answered->exchange(true))
			{
				Post(MoveTemp(Result));
			}
		}

	private:
		void Post(FVaRestAuthRefreshResult&& Result) const
		{
			if (FClientState::IsShuttingDown())
			{
				// Engine exit: the refresh no longer matters, and the task system may be going
				return;
			}
			UE::Tasks::Launch(TEXT("VaRest.RefreshDone"), [Weak = State, Id = RefreshId, Moved = MoveTemp(Result)]() mutable
			{
				if (const TSharedPtr<FClientState, ESPMode::ThreadSafe> Pinned = Weak.Pin())
				{
					Pinned->RunSessionChange([Id, &Moved](FClientState& Target) { Target.FinishRefresh(Id, MoveTemp(Moved)); });
				}
			}, UE::Tasks::ETaskPriority::Normal, UE::Tasks::EExtendedTaskPriority::GameThreadNormalPri);
		}

		TWeakPtr<FClientState, ESPMode::ThreadSafe> State;
		uint64 RefreshId = 0;
		/** Shared by every copy the move-only callables made on the way to the auth; null in a moved-from guard */
		TSharedPtr<std::atomic<bool>, ESPMode::ThreadSafe> Answered;
	};
}

void VaRest::Http::FClientState::CheckInSessionChange() const
{
	checkf(IsInGameThread() && Session.ChangeDepth > 0, TEXT("VaRest: a session change outside FClientState::RunSessionChange (concurrency review C-1)"));
}

void VaRest::Http::FClientState::RunSessionChange(TFunctionRef<void(FClientState&)> Change)
{
	check(IsInGameThread());
	// Concurrency review C-1: the caller's FVaRestClient may hold the only reference, and a listener may drop it
	const TSharedRef<FClientState, ESPMode::ThreadSafe> Keep = AsShared();
	++Session.ChangeDepth;
	Change(*this);
	--Session.ChangeDepth;
	if (Session.ChangeDepth > 0 || Session.bDelivering)
	{
		// Nested -- a listener's change, or a change inside a change: the outer call delivers what this one queued
		return;
	}
	TGuardValue<bool> Delivering(Session.bDelivering, true);
	// Concurrency review I-2: by index, since listeners queue more while this runs; each event reaches every listener
	// before the next one goes out
	for (int32 Index = 0; Index < Session.Pending.Num(); ++Index)
	{
		const FSessionEvent Event = Session.Pending[Index]; // a copy: the array may grow under it
		if (Event.bExpired)
		{
			TracePoints::Marker(*this, EVaRestTraceMarkerKind::SessionExpired, Event.CauseRequestId, Event.Text);
			Session.Expired.Broadcast();
			continue;
		}
		const EVaRestTraceMarkerKind Kind = Event.Change == EVaRestSessionChange::Refreshed ? EVaRestTraceMarkerKind::SessionRefreshed
			: Event.Change == EVaRestSessionChange::Replaced ? EVaRestTraceMarkerKind::SessionReplaced : EVaRestTraceMarkerKind::SessionCleared;
		TracePoints::Marker(*this, Kind, Event.CauseRequestId, Event.Text);
		Session.Changed.Broadcast(Event.Change);
	}
	Session.Pending.Reset();
}

void VaRest::Http::FClientState::PublishSessionSecrets()
{
	PublishSessionSecrets([](FClientConfig&) {});
}

void VaRest::Http::FClientState::PublishSessionSecrets(TFunctionRef<void(FClientConfig&)> AlsoEdit)
{
	using namespace SessionPrivate;
	// IN-13 layer 3: an empty value retires a slot (its old value stays known for 10 minutes, Inspector D21)
	TArray<TPair<FName, FString>> Values;
	Values.Emplace(Slot(TEXT("AccessToken")), Session.Data.AccessToken);
	Values.Emplace(Slot(TEXT("RefreshToken")), Session.Data.RefreshToken);
	for (const FVaRestHeaders::FPair& Pair : Session.Data.Headers)
	{
		Values.Emplace(HeaderSlot(Pair.Key), Pair.Value);
	}
	SetSecrets(Values, AlsoEdit);
}

bool VaRest::Http::FClientState::UpdateTokens(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, double LifetimeSeconds)
{
	CheckInSessionChange();
	FVaRestSessionData& Data = Session.Data;
	const double ExpiresAt = FMath::IsFinite(ExpiresInSeconds) && ExpiresInSeconds > 0.0 ? GetConfig()->Clock->Now() + ExpiresInSeconds : 0.0;
	const double Lifetime = FMath::IsFinite(LifetimeSeconds) && LifetimeSeconds > 0.0 ? LifetimeSeconds : 0.0;
	const bool bSame = Data.AccessToken.Equals(AccessToken, ESearchCase::CaseSensitive)
		&& (!RefreshToken || Data.RefreshToken.Equals(*RefreshToken, ESearchCase::CaseSensitive)) && Data.ExpiresAt == ExpiresAt;
	if (bSame)
	{
		return false;
	}
	// Concurrency review M-6, M4a final review B3 (review B m-1): what the auth gate's "newer credentials" replay compares
	// is the access token's value, which is what AuthApply sends. A new expiry or refresh token alone -- SetSession(T1, R1,
	// 3600), SetBearerToken(T1) on a session with an expiry, an ImportSession of the live session (whose expiresAt never
	// round-trips equal) -- is not a new credential: a 401 then refreshes instead of replaying the refused token.
	if (!Data.AccessToken.Equals(AccessToken, ESearchCase::CaseSensitive))
	{
		++Data.CredentialGeneration;
	}
	Data.AccessToken = AccessToken;
	if (RefreshToken)
	{
		Data.RefreshToken = *RefreshToken;
	}
	Data.ExpiresAt = ExpiresAt;
	// Concurrency review M-7: the lifetime it was given, when known; BearerRefresh refreshes at most half of it early
	Data.Lifetime = Lifetime;
	++Data.Generation;
	PublishSessionSecrets();
	return true;
}

void VaRest::Http::FClientState::AnnounceSession(EVaRestSessionChange Change, uint64 CauseRequestId, const TCHAR* Text)
{
	CheckInSessionChange();
	// Concurrency review I-2: queued; the outermost RunSessionChange delivers it once the change is done, after every
	// listener got the event before it (a listener may change the session again: log in again from OnSessionChanged(Cleared))
	FSessionEvent& Event = Session.Pending.AddDefaulted_GetRef();
	Event.Change = Change;
	Event.CauseRequestId = CauseRequestId;
	Event.Text = Text;
}

void VaRest::Http::FClientState::SetTokensByGame(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, double LifetimeSeconds, bool bSameIdentity, const FGuid* IdentityOverride)
{
	const uint32 CredentialsBefore = Session.Data.CredentialGeneration;
	const bool bNewRefreshToken = RefreshToken && !Session.Data.RefreshToken.Equals(*RefreshToken, ESearchCase::CaseSensitive);
	// Import selected this only after validating every existing field, even when tokens are unchanged.
	if (IdentityOverride) Session.Data.IdentityId = *IdentityOverride;
	if (UpdateTokens(AccessToken, RefreshToken, ExpiresInSeconds, LifetimeSeconds))
	{
		if (!IdentityOverride && !bSameIdentity) Session.Data.IdentityId = FGuid::NewGuid();
		if (!AccessToken.IsEmpty() && Session.Data.CredentialGeneration != CredentialsBefore)
		{
			// Plan decision 3: the game's credentials end a refresh in flight -- its waiters go on with them (Ready), and the
			// refresh's own late answer is ignored (FinishRefresh). M4a final review B3 (review B m-1): only a new access
			// token is new credentials; a write that keeps it leaves the refresh to finish, so its waiters do not replay the
			// token that was just refused.
			EndRefresh(ERefreshEnd::Ready, TEXT("the game set new credentials"));
		}
		else if (AccessToken.IsEmpty() && bNewRefreshToken)
		{
			// M4a final review B4 (review B m-2): a refresh token only, and another one (a restore of a saved refresh token):
			// the refresh in flight is the old token's, so it ends -- its late answer is ignored -- and its waiters find no
			// access token: a parked request parks again behind a refresh from the new token (the auth gate's waiter), rather
			// than going out with no credentials. The same refresh token with the access token dropped keeps the refresh: it
			// is already one from that token, and a second refresh request on one refresh token is what a rotating server's
			// reuse detection revokes the whole session for.
			EndRefresh(ERefreshEnd::Ready, TEXT("the game set a refresh token only"));
		}
		AnnounceSession(bSameIdentity ? EVaRestSessionChange::Refreshed : EVaRestSessionChange::Replaced, 0, TEXT("set by the game"));
	}
}

void VaRest::Http::FClientState::SetSessionHeader(FStringView Name, FStringView Value)
{
	CheckInSessionChange();
	const FString* Existing = Session.Data.Headers.Find(Name);
	if (Existing && FStringView(*Existing).Equals(Value, ESearchCase::CaseSensitive))
	{
		// Review M-5: nothing changed -- no generation for an attempt to compare, nothing to publish
		return;
	}
	Session.Data.Headers.Set(Name, Value);
	// Not CredentialGeneration: a header change must not use up a 401's one replay (concurrency review M-6)
	++Session.Data.Generation;
	// IN-13 layer 2 in the same republish as layer 3: the name is a key wherever it appears (a header, a query, a JSON
	// body that echoes it)
	PublishSessionSecrets([Name](FClientConfig& Edited) { AppendRedactKeys(Edited.RedactedKeys, TArray<FString>{ FString(Name) }); });
}

void VaRest::Http::FClientState::RemoveSessionHeader(FStringView Name)
{
	CheckInSessionChange();
	if (!Session.Data.Headers.Remove(Name))
	{
		return;
	}
	++Session.Data.Generation;
	// Retired, not forgotten: its value stays known for 10 minutes (Inspector D21)
	SetSecrets(TArray<TPair<FName, FString>>{ TPair<FName, FString>(SessionPrivate::HeaderSlot(Name), FString()) });
}

void VaRest::Http::FClientState::ClearSession()
{
	CheckInSessionChange();
	// Inspector 12, "cleared on logout": the session's values go now, previous ones included; the game's own slots stay.
	// Before the empty check (review M-3): an expired session has no token left, but its retired ones are still known.
	if (Secrets.ClearSlots([](FName Slot) { return IsSessionSecretSlot(Slot); }) > 0)
	{
		EditConfig([](FClientConfig&) {});
		ScheduleSecretExpiry();
	}
	FVaRestSessionData& Data = Session.Data;
	if (!Data.HasAccessToken() && !Data.HasRefreshToken() && Data.Headers.IsEmpty())
	{
		// No session to clear: no generation, no Cleared. A refresh in flight still ends: a logout is a logout (Kind=Auth).
		EndRefresh(ERefreshEnd::Cleared, TEXT("SessionCleared"));
		return;
	}
	const uint32 Generation = Data.Generation + 1;
	// M4a final review B3: the access token's value changes only when there was one
	const uint32 CredentialGeneration = Data.CredentialGeneration + (Data.HasAccessToken() ? 1 : 0);
	Data = FVaRestSessionData();
	Data.Generation = Generation;
	Data.CredentialGeneration = CredentialGeneration;
	EndRefresh(ERefreshEnd::Cleared, TEXT("SessionCleared"));
	AnnounceSession(EVaRestSessionChange::Cleared, 0, TEXT("ClearSession"));
}

void VaRest::Http::FClientState::ExpireSession(uint64 CauseRequestId, FStringView Why)
{
	CheckInSessionChange();
	const FVaRestSessionData& Data = Session.Data;
	if (!Data.HasAccessToken() && !Data.HasRefreshToken() && !Session.Refresh.IsValid())
	{
		// Concurrency review I-2: expired already -- a parked request's OnError calling NotifySessionExpired after a
		// rejected refresh, a listener of OnSessionExpired calling it again: one expiry, one event. A refresh in flight
		// (an auth that refreshes from no token) is not expired yet: it ends in ExpireSessionNow.
		return;
	}
	ExpireSessionNow(CauseRequestId, Why);
}

void VaRest::Http::FClientState::ExpireSessionNow(uint64 CauseRequestId, FStringView Why)
{
	CheckInSessionChange();
	FVaRestSessionData& Data = Session.Data;
	if (Data.HasAccessToken())
	{
		// M4a final review B3: the access token's value changes only when there was one
		++Data.CredentialGeneration;
	}
	Data.IdentityId = FGuid::NewGuid();
	Data.AccessToken.Reset();
	Data.RefreshToken.Reset();
	Data.ExpiresAt = 0.0;
	Data.Lifetime = 0.0;
	++Data.Generation;
	PublishSessionSecrets();
	// NotifySessionExpired (the game's, or a socket's) ending a refresh still running: its waiters fail Kind=Auth. From
	// FinishRefresh(Rejected) there is none: it took its refresh out before it came here, and tells the waiters itself.
	EndRefresh(ERefreshEnd::Rejected, TEXT("AuthRejected"));
	FSessionEvent& Event = Session.Pending.AddDefaulted_GetRef();
	Event.bExpired = true;
	Event.CauseRequestId = CauseRequestId;
	Event.Text = FString(Why);
}

FString VaRest::Http::FClientState::ExportSession() const
{
	check(IsInGameThread());
	const FVaRestSessionData& Data = Session.Data;
	FVaRestJson Root = FVaRestJson::Object();
	Root.Set(TEXT("version"), 1);
	Root.Set(TEXT("accessToken"), Data.AccessToken);
	Root.Set(TEXT("refreshToken"), Data.RefreshToken);
	if (Data.ExpiresAt > 0.0)
	{
		// The client's clock is monotonic and per process: an export carries the expiry as a UTC time
		const double Left = Data.ExpiresAt - GetConfig()->Clock->Now();
		Root.Set(TEXT("expiresAt"), (FDateTime::UtcNow() + FTimespan::FromSeconds(Left)).ToIso8601());
	}
	FVaRestJson Headers = FVaRestJson::Array();
	for (const FVaRestHeaders::FPair& Pair : Data.Headers)
	{
		Headers.Add(FVaRestJson::Object().Set(TEXT("name"), Pair.Key).Set(TEXT("value"), Pair.Value));
	}
	Root.Set(TEXT("headers"), Headers);
	Root.Set(TEXT("cacheIdentity"), Data.IdentityId.ToString(EGuidFormats::DigitsLower));
	Root.Set(TEXT("cacheBinding"), SessionBinding(Data));
	return Root.ToString();
}

bool VaRest::Http::FClientState::ImportSession(FStringView InJson, bool bSameIdentity)
{
	CheckInSessionChange();
	// Spec review M-12: the game thread never parses a body (spec 1, Inspector 7) -- this is the one bounded exception: a
	// session is a few hundred bytes and the call is synchronous; anything past MaxSessionJsonChars is refused unparsed
	if (InJson.Len() > MaxSessionJsonChars)
	{
		return false;
	}
	TValueOrError<FVaRestJson, FVaRestJsonError> Parsed = FVaRestJson::Parse(InJson);
	if (Parsed.HasError())
	{
		return false;
	}
	const FVaRestJson Root = Parsed.StealValue();
	if (!Root.IsObject() || Root[TEXT("version")].AsInt(0) != 1)
	{
		return false;
	}
	// Review I-1: strict types, before any mutation. AsString() gives "" for a missing or wrong-typed value, and two empty
	// tokens are a logout (controller ruling): only an explicit "" pair may log out, never a malformed file.
	// ExportSession() always writes both tokens, so a missing one is not an export either.
	const FVaRestJson AccessNode = Root[TEXT("accessToken")];
	const FVaRestJson RefreshNode = Root[TEXT("refreshToken")];
	if (!AccessNode.IsString() || !RefreshNode.IsString())
	{
		return false;
	}
	const FString Access = AccessNode.AsString();
	const FString Refresh = RefreshNode.AsString();
	if (!IsValidHeaderValue(Access) || !IsValidHeaderValue(Refresh))
	{
		return false;
	}
	double ExpiresIn = 0.0;
	const FVaRestJson ExpiresNode = Root[TEXT("expiresAt")];
	if (ExpiresNode.IsValid() && !ExpiresNode.IsString())
	{
		return false;
	}
	const FString ExpiresText = ExpiresNode.AsString();
	if (!ExpiresText.IsEmpty())
	{
		FDateTime When;
		if (!FDateTime::ParseIso8601(*ExpiresText, When))
		{
			return false;
		}
		// Already expired: a moment from now, so the auth refreshes at once (NeedsRefresh), instead of "unknown"
		ExpiresIn = FMath::Max((When - FDateTime::UtcNow()).GetTotalSeconds(), 1e-3);
	}
	FVaRestHeaders Headers;
	const FVaRestJson HeadersNode = Root[TEXT("headers")];
	if (HeadersNode.IsValid() && !HeadersNode.IsArray())
	{
		return false;
	}
	for (const FVaRestJson& Entry : HeadersNode.Elements())
	{
		if (!Entry.IsObject() || !Entry[TEXT("name")].IsString() || !Entry[TEXT("value")].IsString())
		{
			return false;
		}
		const FString Name = Entry[TEXT("name")].AsString();
		const FString Value = Entry[TEXT("value")].AsString();
		// Review M-1: a name twice (any case) is not an ExportSession() result -- refused, not last-wins
		if (!IsValidHeaderName(Name) || !IsValidHeaderValue(Value) || Headers.Find(Name))
		{
			return false;
		}
		Headers.Set(Name, Value);
	}
	// An unkeyed binding detects mixed exports; it does not authenticate caller-owned credentials.
	const bool bLiveIdentity = Session.Data.HasAccessToken() || Session.Data.HasRefreshToken()
		|| !Session.Data.Headers.IsEmpty();
	FVaRestSessionData Candidate;
	Candidate.AccessToken = Access;
	Candidate.RefreshToken = Refresh;
	Candidate.Headers = Headers;
	FGuid ImportedIdentity;
	const FVaRestJson IdentityNode = Root[TEXT("cacheIdentity")];
	const FVaRestJson BindingNode = Root[TEXT("cacheBinding")];
	const FString IdentityText = IdentityNode.IsString() ? IdentityNode.AsString() : FString();
	const FString BindingText = BindingNode.IsString() ? BindingNode.AsString() : FString();
	bool bValidBinding = false;
	if (IdentityText.Len() == 32 && FGuid::ParseExact(IdentityText, EGuidFormats::Digits, ImportedIdentity)
		&& ImportedIdentity.IsValid() && BindingText.Len() == 64)
	{
		Candidate.IdentityId = ImportedIdentity;
		bValidBinding = SessionBinding(Candidate).Equals(BindingText, ESearchCase::CaseSensitive);
	}
	const FGuid ChosenIdentity = bSameIdentity && bLiveIdentity ? Session.Data.IdentityId
		: bValidBinding ? ImportedIdentity : FGuid::NewGuid();
	if (Access.IsEmpty() && Refresh.IsEmpty())
	{
		// Controller ruling (Task 13), consistent with review M-4: a session with no token is a logout, not a login --
		// an export of an empty session imports as an empty one, and no Replaced goes out with nothing to send.
		ClearSession();
		return true;
	}
	TArray<FString> Names;
	TArray<TPair<FName, FString>> Retired;
	for (const FVaRestHeaders::FPair& Pair : Headers)
	{
		Names.Add(Pair.Key);
	}
	for (const FVaRestHeaders::FPair& Old : Session.Data.Headers)
	{
		if (!Headers.Find(Old.Key))
		{
			// As RemoveSessionHeader: a header the import drops is retired, its value known for 10 minutes (Inspector D21)
			Retired.Emplace(SessionPrivate::HeaderSlot(Old.Key), FString());
		}
	}
	Session.Data.Headers = MoveTemp(Headers);
	++Session.Data.Generation;
	// IN-13 layer 2 and 3 in one republish, as SetSessionHeader: the imported names are keys, the values session slots
	PublishSessionSecrets([&Names](FClientConfig& Edited) { AppendRedactKeys(Edited.RedactedKeys, Names); });
	if (!Retired.IsEmpty())
	{
		SetSecrets(Retired);
	}
	// Re-review N-10: the time left is not the token's lifetime -- unknown, so BearerRefresh's configured lead applies
	SetTokensByGame(Access, &Refresh, ExpiresIn, 0.0, bSameIdentity, &ChosenIdentity);
	return true;
}

uint64 VaRest::Http::FClientState::JoinRefresh(uint64 WaiterId, const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth, uint64 CauseRequestId, bool bProactive,
	FRefreshWaiter&& OnDone)
{
	check(IsInGameThread());
	const bool bStart = !Session.Refresh.IsValid();
	if (bStart)
	{
		Session.Refresh = MakeUnique<FRefreshInFlight>();
		// Spec review I-2: a process-unique request id, which the refresh request carries as its own (Task 15), so IN-2's
		// AuthRefreshId names a record the Inspector has
		Session.Refresh->RefreshId = FVaRestTrace::NewRequestId();
		Session.Refresh->CauseRequestId = CauseRequestId;
		Session.Refresh->bProactive = bProactive;
	}
	Session.Refresh->Waiters.Emplace(WaiterId, MoveTemp(OnDone));
	// Read before StartRefresh: the auth's Refresh is game code and may end this refresh inside the call
	const uint64 Joined = Session.Refresh->RefreshId;
	if (bStart)
	{
		StartRefresh(Auth);
	}
	return Joined;
}

void VaRest::Http::FClientState::JoinRefreshInFlight(uint64 WaiterId, FRefreshWaiter&& OnDone)
{
	check(IsInGameThread());
	checkf(Session.Refresh.IsValid(), TEXT("VaRest: JoinRefreshInFlight with no refresh in flight (IsRefreshing() first)"));
	Session.Refresh->Waiters.Emplace(WaiterId, MoveTemp(OnDone));
}

void VaRest::Http::FClientState::LeaveRefresh(uint64 WaiterId)
{
	check(IsInGameThread());
	if (Session.Refresh.IsValid())
	{
		Session.Refresh->Waiters.RemoveAll([WaiterId](const TPair<uint64, FRefreshWaiter>& Waiter) { return Waiter.Key == WaiterId; });
	}
}

void VaRest::Http::FClientState::StartRefresh(const TSharedRef<IVaRestAuth, ESPMode::ThreadSafe>& Auth)
{
	const uint64 Id = Session.Refresh->RefreshId;
	// Task 15 review M-1: the one maker of a context; Id is fresh, so the refresh request's id is in use nowhere else
	const FVaRestAuthRefreshContext Context = FClientAccess::MakeRefreshContext(FClientAccess::Wrap(AsShared()), Session.Data, Id);
	// Concurrency review I-4: Done owns the guard; called or dropped, the refresh ends from a later game-thread task. The
	// auth's Refresh is game code and may end this refresh itself (a SetSession inside it): nothing here reads
	// Session.Refresh after the call.
	Auth->Refresh(Context, [Guard = SessionPrivate::FRefreshDoneGuard(AsShared(), Id)](FVaRestAuthRefreshResult&& Result) mutable
	{
		Guard.Deliver(MoveTemp(Result));
	});
}

void VaRest::Http::FClientState::FinishRefresh(uint64 RefreshId, FVaRestAuthRefreshResult&& Result)
{
	CheckInSessionChange();
	if (!Session.Refresh.IsValid() || Session.Refresh->RefreshId != RefreshId)
	{
		// Ended without its result: the game set, cleared or expired the session meanwhile (EndRefresh)
		return;
	}
	TUniquePtr<FRefreshInFlight> Ended = MoveTemp(Session.Refresh);
	FRefreshEnded End;
	End.RefreshId = RefreshId;
	End.Message = Result.Message;
	End.bProactive = Ended->bProactive;
	if (Result.Outcome == EVaRestAuthRefreshOutcome::Refreshed && !Result.AccessToken.IsEmpty() && IsValidHeaderValue(Result.AccessToken)
		&& IsValidHeaderValue(Result.RefreshToken))
	{
		// A refresh response's expires_in is the new token's whole lifetime (re-review N-10), as a login's is. Task 16 review
		// M-3: announced only when something changed, as SetTokensByGame announces; the same tokens back (and no expiry)
		// are no event, while the waiters still go on with them (Ready)
		End.End = ERefreshEnd::Ready;
		if (UpdateTokens(Result.AccessToken, Result.RefreshToken.IsEmpty() ? nullptr : &Result.RefreshToken, Result.ExpiresInSeconds, Result.ExpiresInSeconds))
		{
			AnnounceSession(EVaRestSessionChange::Refreshed, Ended->CauseRequestId, TEXT("refreshed by the auth"));
		}
	}
	else if (Result.Outcome == EVaRestAuthRefreshOutcome::Failed || Result.Outcome == EVaRestAuthRefreshOutcome::Unavailable)
	{
		// Plan decision 3, ruling I-7: the session stays; the waiters fail with the refresh's kind. Task 16 review M-2: kept
		// as the Failed() factory keeps it (VaRestAuth.cpp:35) -- Timeout and Config, the rest Connection -- since the field
		// is public and a result built by hand counts (Task 15 review M-5): a socket's RefreshAuth and a parked request
		// (the auth gate's FailureFor) then agree about one refresh. Unavailable is RefreshAuth's answer, not an auth's: one
		// that reports it anyway keeps the session as a Failed does (Task 15 review M-5's rule), never a logout.
		End.End = ERefreshEnd::Failed;
		End.Kind = Result.FailedKind == EVaRestErrorKind::Timeout || Result.FailedKind == EVaRestErrorKind::Config ? Result.FailedKind : EVaRestErrorKind::Connection;
	}
	else
	{
		// Rejected, or a success with nothing usable in it (a malformed success, ruling Q1): the session expires, once (I-2)
		End.End = ERefreshEnd::Rejected;
		if (Result.Outcome == EVaRestAuthRefreshOutcome::Refreshed)
		{
			End.Message = TEXT("the refresh gave no usable access token");
		}
		// Task 16 review M-1 (ruled): forced. The refresh is out of Session.Refresh already, so ExpireSession's already-expired
		// return (concurrency review I-2) would find a session with no token -- an auth that refreshes from none, a platform
		// ticket -- and queue nothing: the waiters told Rejected, the game nothing. This is the rejection's one expiry (spec
		// 4.2: "fires OnSessionExpired (once)"); a later NotifySessionExpired finds it expired.
		ExpireSessionNow(Ended->CauseRequestId, End.Message);
	}
	// The events queued above reach the listeners when this change returns; the waiters run after, from their own task
	SessionPrivate::ResolveWaitersLater(*this, MoveTemp(Ended), MoveTemp(End));
}

void VaRest::Http::FClientState::EndRefresh(ERefreshEnd EndKind, const TCHAR* Message)
{
	CheckInSessionChange();
	if (!Session.Refresh.IsValid())
	{
		return;
	}
	TUniquePtr<FRefreshInFlight> Ended = MoveTemp(Session.Refresh);
	FRefreshEnded End;
	End.End = EndKind;
	End.RefreshId = Ended->RefreshId;
	End.Message = Message;
	End.bProactive = Ended->bProactive;
	// The refresh's own late answer finds another id (or none) and is ignored (FinishRefresh)
	SessionPrivate::ResolveWaitersLater(*this, MoveTemp(Ended), MoveTemp(End));
}
