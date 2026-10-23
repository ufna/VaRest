// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestStage.h"
#include "VaRestClock.h"

namespace VaRest::Http
{
	class FPersistenceWait : public TSharedFromThis<FPersistenceWait, ESPMode::ThreadSafe>
	{
		public:
		static TSharedRef<FPersistenceWait, ESPMode::ThreadSafe> Begin(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done);
		bool IsPending() const
		{
			return !bEnded;
		} // GT only
		bool CanHandoff(); // checks live cancellation/owner/deadline before consuming any result
		void Finish(FVaRestAttemptOutcome&& Outcome);
		// End hold without answering; transfer callback to the next pipeline phase.
		FVaRestOutcomeCallback Resume();

		private:
		FPersistenceWait(const FVaRestExchangeRef& Exchange, FVaRestOutcomeCallback&& Done);
		FVaRestOutcomeCallback TakeCompletion();
		void FinishUnchecked(FVaRestAttemptOutcome&& Outcome);
		friend class FQueueAdmissionJob; // Task 8 alone uses TakeCompletion after its accepted commit CAS
		bool InterruptNow();
		void CheckDeadline();
		void Disarm();
		FVaRestExchangeRef HeldExchange;
		FVaRestOutcomeCallback Completion;
		FVaRestTimerHandle Timer;
		bool bEnded = false;
	};
} // namespace VaRest::Http
