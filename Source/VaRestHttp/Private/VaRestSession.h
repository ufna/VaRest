// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuthTypes.h"

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

	/**
	 * Spec 4.2: one client's session, live (plan decision 6). Game thread. FClientState owns it; its session functions
	 * (VaRestSession.cpp) change it inside RunSessionChange, publish its secrets and queue its events. Task 16 adds the
	 * refresh in flight.
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
}
