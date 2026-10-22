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

	/**
	 * Game thread. M4a final review B5 (review B m-3): the one answer to "can a refresh start for this session now?" --
	 * the auth gate's proactive and reactive paths, Resolve's replay copy, NeedsAuthRefresh and RefreshAuth all ask it.
	 * A misconfigured auth (ruling Q7: GetConfigError non-empty) never refreshes, whatever its CanRefresh says: AuthApply
	 * refuses each of its requests Kind=Config, so a refresh would only send what the configuration cannot do, and its
	 * Rejected would expire the session over a typo. BearerRefresh's own CanRefresh gate stays, as defence in depth.
	 */
	bool CanRefreshNow(const IVaRestAuth& Auth, const FVaRestSessionData& Session);

	/** Spec 4.2, per attempt: the first built-in PerAttempt stage, before the custom ones (they sign the final headers) */
	FVaRestStageRef MakeAuthApplyStage();

	/** Spec 4.2: the built-in Outer stage after retry -- parks, refreshes, replays once through the attempt boundary */
	FVaRestStageRef MakeAuthGateStage();
}
