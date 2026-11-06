// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"

class IVaRestAuth;

namespace VaRest::Http
{
	/** One session event waiting for its listeners (concurrency review I-2) */
	struct FSessionEvent
	{
		/** OnSessionExpired; else OnSessionChanged(Change) */
		bool bExpired = false;
		EVaRestSessionChange Change = EVaRestSessionChange::Replaced;
		/** IN-9 CauseRequestId; 0 = the game */
		uint64 CauseRequestId = 0;
		/** IN-9 Text (the trace point scrubs it) */
		FString Text;
	};

	/** How a waiter's refresh ended (plan decision 3, ruling I-7) */
	enum class ERefreshEnd : uint8
	{
		/** New credentials, from the refresh or from the game: go */
		Ready,
		/** The refresh was rejected, or NotifySessionExpired: Kind=Auth */
		Rejected,
		/** The refresh failed otherwise (Kind: Connection, Timeout or Config): the session stays */
		Failed,
		/** ClearSession (a logout) while waiting: Kind=Auth */
		Cleared,
		Unavailable // private managed External delivery when no refresh can start; RequestGate never receives this
	};

	struct FRefreshEnded
	{
		ERefreshEnd End = ERefreshEnd::Ready;
		/** Spec review I-2: the refresh request's RequestId (IN-2 AuthRefreshId) */
		uint64 RefreshId = 0;
		/** Failed: what the waiters fail with */
		EVaRestErrorKind Kind = EVaRestErrorKind::Connection;
		FString Message;
		/** FRefreshInFlight::bProactive of the refresh that ended (M4a final review B6) */
		bool bProactive = false;
	};

	using FRefreshWaiter = TUniqueFunction<void(const FRefreshEnded&)>;
	enum class ERefreshWaiterKind : uint8 { RequestGate,External };
	struct FRefreshWaiterEntry
	{
		FRefreshWaiterEntry(uint64 InId,FRefreshWaiter&& InCallback,ERefreshWaiterKind InKind)
			: Key(InId),Value(MoveTemp(InCallback)),Kind(InKind) {}
		uint64 Key=0;
		FRefreshWaiter Value;
		ERefreshWaiterKind Kind=ERefreshWaiterKind::RequestGate;
	};

	/** Spec 4.2's single flight: the refresh running now and who waits for it */
	struct FRefreshInFlight
	{
		uint64 RefreshId = 0;
		/** IN-9 CauseRequestId: the request whose 401 (or due expiry) started it; 0 for RefreshAuth */
		uint64 CauseRequestId = 0;
		/**
		 * M4a final review B6: a park before an attempt started it (spec 4.2's "proactively": a due expiry, no access token),
		 * not a 401 or RefreshAuth. When it fails, a request parked before its attempt whose token is still unexpired is sent
		 * with it (the auth gate's waiter).
		 */
		bool bProactive = false;
		/** Original provider without prolonging its lifetime: dropping a provider must still release its pending Done. */
		TWeakPtr<IVaRestAuth, ESPMode::ThreadSafe> Auth;
		TArray<FRefreshWaiterEntry> Waiters;
	};

	/**
	 * Spec 4.2: one client's session, live (plan decision 6). Game thread. FClientState owns it; its session functions
	 * (VaRestSession.cpp) change it inside RunSessionChange, publish its secrets and queue its events, and its refresh in
	 * flight (Task 16) ends with the game's own changes.
	 */
	struct FSessionState
	{
		FVaRestSessionData Data;
		FVaRestSessionChangedEvent Changed;
		FVaRestSessionExpiredEvent Expired;
		/** Concurrency review I-2: queued by a change, delivered in order by the outermost RunSessionChange */
		TArray<FSessionEvent> Pending;
		/** RunSessionChange calls in progress; every session function checks for one (concurrency review C-1) */
		int32 ChangeDepth = 0;
		/** The outermost RunSessionChange is delivering Pending: a nested change only queues */
		bool bDelivering = false;
		/** Null when no refresh runs */
		TUniquePtr<FRefreshInFlight> Refresh;
		uint64 NextWaiterId = 0;
	};

	// ---- Who owns a secret slot. Each door forgets exactly its owner's: ClearSession the session's, ClearSecretValues the
	// game's (review I-1); an auth's (M4a Task 14, VaRest.Auth.*) stay while it is configured.
	/** Every secret slot the session takes starts with this, so ClearSession forgets exactly them (plan decision 17) */
	inline const TCHAR* SessionSecretPrefix() { return TEXT("VaRest.Session."); }

	/** Spec review M-12: ImportSession parses on the game thread, so its input is bounded (a session is a few hundred bytes) */
	inline constexpr int32 MaxSessionJsonChars = 64 * 1024;
	/**
	 * Review M-1, by shape: FClientState::SetSecret (SetSecretValue, the game's one door) stores every game slot under this,
	 * so no name a game picks -- "VaRest.Session.AccessToken" included -- is the session's or an auth's slot
	 */
	inline const TCHAR* GameSecretPrefix() { return TEXT("VaRest.Game."); }
	/** The slot a game's SetSecretValue(Slot) is stored as */
	inline FName GameSecretSlot(FName Slot) { return FName(*(FString(GameSecretPrefix()) + Slot.ToString())); }
	// FName text keeps the casing it was first made with, so the prefixes compare ignoring case
	inline bool IsSessionSecretSlot(FName Slot) { return Slot.ToString().StartsWith(SessionSecretPrefix(), ESearchCase::IgnoreCase); }
	inline bool IsGameSecretSlot(FName Slot) { return Slot.ToString().StartsWith(GameSecretPrefix(), ESearchCase::IgnoreCase); }
	/** Every slot an auth's values take (FClientState::SetAuth); neither ClearSession nor ClearSecretValues forgets them */
	inline const TCHAR* AuthSecretPrefix() { return TEXT("VaRest.Auth."); }
	/**
	 * Task 14 carry: the slot an IVaRestAuth::GetSecretValues name is stored as. A custom auth may return any name; under
	 * this prefix it can never be a session's (VaRest.Session.*) or a game's (VaRest.Game.*) slot, as GameSecretSlot keeps
	 * a game from the others. A name already under it (any case: FName text keeps its first casing) is kept as it is.
	 */
	inline FName AuthSecretSlot(FName Slot)
	{
		const FString Name = Slot.ToString();
		return Name.StartsWith(AuthSecretPrefix(), ESearchCase::IgnoreCase) ? Slot : FName(*(FString(AuthSecretPrefix()) + Name));
	}
}
