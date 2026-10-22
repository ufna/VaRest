// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "VaRestAuth.h"
#include "VaRestStage.h"

namespace VaRest::Http
{
	/**
	 * Game thread: the session headers, then Auth (it wins on a name both set); true when Auth applied credentials or there
	 * is none
	 */
	bool ApplyCredentials(const IVaRestAuth* Auth, FVaRestAuthTarget& Target);

	/** Spec 4.2, per attempt: the first built-in PerAttempt stage, before the custom ones (they sign the final headers) */
	FVaRestStageRef MakeAuthApplyStage();

	/** Spec 4.2: the built-in Outer stage after retry -- parks, refreshes, replays once through the attempt boundary */
	FVaRestStageRef MakeAuthGateStage();
}
