// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/** One attempt's outcome as an error (spec 3.3) */
	struct FAttemptClass
	{
		EVaRestErrorKind Kind = EVaRestErrorKind::None;
		/** Spec 4.1's rule, which FVaRestError::bRetryable reports: a connection error, a timeout, or 408/425/429/500/502/503/504 */
		bool bRetryable = false;

		bool IsError() const { return Kind != EVaRestErrorKind::None; }
	};

	/** The kind a transport failure stands for, its detail first (plan decision 12): a local refusal is no network failure */
	EVaRestErrorKind KindFor(EVaRestTransportFailure Failure, FName Detail);

	/**
	 * The one mapping from an attempt's outcome to Kind and bRetryable (plan decision 12). The pipeline's FVaRestError and
	 * the retry stage both read it, so an error that says bRetryable is one the retry stage could have retried.
	 */
	FAttemptClass ClassifyAttempt(const FVaRestAttemptOutcome& Outcome);
}
