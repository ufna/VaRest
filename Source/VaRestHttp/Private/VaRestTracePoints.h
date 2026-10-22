// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestTrace.h"

struct FVaRestTransportResult;

namespace VaRest::Http
{
	struct FClientConfig;
	struct FExchangeState;
	struct FFinish;
	class FClientState;

	/**
	 * Inspector 9.1's trace points. Game thread. Each starts with the one relaxed load of the mask and builds nothing
	 * when it is 0 (IN-11); every string in a payload comes through the client's redactor (Inspector 2.2, ruling R1).
	 */
	namespace TracePoints
	{
		/** IN-1, on entry after Resolve. Reads the initiator (IN-15) and StartUtc into the exchange for the snapshot. */
		void Started(FExchangeState& Ex);
		/** IN-2, immediately before the transport starts */
		void AttemptDispatched(const FExchangeState& Ex);
		/**
		 * IN-3, when the transport's result is back on the game thread. Non-const: it records which attempt's response the
		 * decode task excerpts (FExchangeState::ExcerptOnDecodeAttempt, IN-14).
		 */
		void AttemptEnded(FExchangeState& Ex, const FVaRestTransportResult& Result);
		/**
		 * IN-5, before any callback. Non-const: an announced decode excerpt that never reached the decode task resolves
		 * first (Excerpts::ResolveDiscarded, Task 21 review M-1).
		 */
		void Completed(FExchangeState& Ex, const FFinish& Finish);
		/** IN-8. Any thread; the event is emitted on the game thread. */
		void Client(uint64 ClientId, const FClientConfig& Config, bool bCreated);
		/** IN-4, game thread: stamps the exchange's request id, attempt and clock */
		void Note(const FExchangeState& Ex, FVaRestTraceNote&& Payload);
		/** IN-9, game thread: Text is value-scrubbed with the client's current redactor */
		void Marker(const FClientState& Client, EVaRestTraceMarkerKind Kind, uint64 CauseRequestId, FStringView Text);
		/** IN-12: every live client and in-flight request; the module registers it with FVaRestTrace::OnBuildSnapshot */
		void ContributeSnapshot(FVaRestTraceSnapshot& Snapshot);
	}
}
