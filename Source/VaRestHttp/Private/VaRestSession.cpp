// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSession.h"

#include "VaRestClientState.h"
#include "VaRestTracePoints.h"

#include "Misc/DateTime.h"
#include "Misc/Timespan.h"

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
	SetTokensByGame(Access, &Refresh, ExpiresIn, 0.0, bSameIdentity);
	return true;
}
