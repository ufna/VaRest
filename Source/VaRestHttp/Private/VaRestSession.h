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

	/** Every secret slot the session takes starts with this, so ClearSession forgets exactly them (plan decision 17) */
	inline const TCHAR* SessionSecretPrefix() { return TEXT("VaRest.Session."); }
}
