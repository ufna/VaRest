// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSession.h"

#include "VaRestClientState.h"
#include "VaRestTracePoints.h"

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
	Data.AccessToken = AccessToken;
	if (RefreshToken)
	{
		Data.RefreshToken = *RefreshToken;
	}
	Data.ExpiresAt = ExpiresAt;
	// Concurrency review M-7: the lifetime it was given, when known; BearerRefresh refreshes at most half of it early
	Data.Lifetime = Lifetime;
	++Data.Generation;
	// Concurrency review M-6: a token change -- what the auth gate's "newer credentials" replay compares
	++Data.CredentialGeneration;
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

void VaRest::Http::FClientState::SetTokensByGame(const FString& AccessToken, const FString* RefreshToken, double ExpiresInSeconds, double LifetimeSeconds, bool bSameIdentity)
{
	if (UpdateTokens(AccessToken, RefreshToken, ExpiresInSeconds, LifetimeSeconds))
	{
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
		// No session to clear: no generation, no Cleared
		return;
	}
	const uint32 Generation = Data.Generation + 1;
	const uint32 CredentialGeneration = Data.CredentialGeneration + 1;
	Data = FVaRestSessionData();
	Data.Generation = Generation;
	Data.CredentialGeneration = CredentialGeneration;
	AnnounceSession(EVaRestSessionChange::Cleared, 0, TEXT("ClearSession"));
}

void VaRest::Http::FClientState::ExpireSession(uint64 CauseRequestId, FStringView Why)
{
	CheckInSessionChange();
	FVaRestSessionData& Data = Session.Data;
	if (!Data.HasAccessToken() && !Data.HasRefreshToken())
	{
		// Concurrency review I-2: expired already -- a parked request's OnError calling NotifySessionExpired after a
		// rejected refresh, a listener of OnSessionExpired calling it again: one expiry, one event (Task 16 adds "and no
		// refresh in flight")
		return;
	}
	Data.AccessToken.Reset();
	Data.RefreshToken.Reset();
	Data.ExpiresAt = 0.0;
	Data.Lifetime = 0.0;
	++Data.Generation;
	++Data.CredentialGeneration;
	PublishSessionSecrets();
	FSessionEvent& Event = Session.Pending.AddDefaulted_GetRef();
	Event.bExpired = true;
	Event.CauseRequestId = CauseRequestId;
	Event.Text = FString(Why);
}
