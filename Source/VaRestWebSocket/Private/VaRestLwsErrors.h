// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestSocketTypes.h"

namespace VaRest::WebSocket::Lws
{
	/** What a CLIENT_CONNECTION_ERROR string means (plan decision 11) */
	struct FConnectionErrorClass
	{
		EVaRestSocketFailure Failure = EVaRestSocketFailure::Unknown;
		int32 HttpStatus = 0;
		/**
		 * The string was empty after trimming: one of LWS's own stage timers (a fixed 20 s for the TCP connect and the proxy,
		 * timeout_secs for the others; review conc M-1) or a kill VaRest did not make. Failure is Unknown; the connection
		 * decides by the time the attempt took (review conc M-2)
		 */
		bool bEmpty = false;
	};

	/**
	 * Any thread: libwebsockets 3.0's error string for a client that never opened (libwebsockets.h:1255-1299; the strings of
	 * lib/Unix/x86_64-unknown-linux-gnu/Release/libwebsockets.a; passport hs.*, tls.untrusted_rejected, timeout.*), as VaRest
	 * classifies it. Only trailing whitespace and NULs are trimmed. The status (100-599, else Unknown with 0) comes only from the string: lws_http_client_http_response is never called there (T18).
	 * A failure VaRest caused itself (a redirect veto, a refused certificate, its own kill) is the connection's to report: it
	 * overrides what this says.
	 */
	FConnectionErrorClass ClassifyConnectionError(FStringView Text);

	/**
	 * LWS's own timer for the TCP connect and the proxy exchange, fixed in 3.0 whatever timeout_secs says (review conc M-1:
	 * lws_client_connect_2 arms (2 | 8 | 1, 20) at client.c.o 0x572-0x57f, 0x7b6-0x7c3, 0x843-0x850 of the Release archive)
	 */
	inline constexpr double LwsConnectStageSeconds = 20.0;

	/**
	 * How far under LwsConnectStageSeconds an elapsed time still counts as LWS's stage timer: LWS's timers fire on its
	 * whole-second service ticks, so VaRest's wall clock can read just under 20 s when the stage fired. The plan (decision 17,
	 * review conc M-2) fixes the split at "19.5 s"; the passport gives no measurement behind the 0.5
	 */
	inline constexpr double LwsStageToleranceSeconds = 0.5;

	/**
	 * Any thread: an empty CONNECTION_ERROR (bEmpty) by the time the attempt took (review conc M-1, M-2; re-review: a pure
	 * function, so the split is table-tested). From LwsConnectStageSeconds - LwsStageToleranceSeconds (19.5 s, inclusive) on it is LWS's own stage timer --
	 * ConnectTimeout; before, Unknown (a kill VaRest did not make); so are a negative and a NaN time. OutDetail says which, with the time
	 */
	FConnectionErrorClass ClassifyEmptyConnectionError(double TookSeconds, FString& OutDetail);
}
