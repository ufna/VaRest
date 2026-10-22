// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"
#include "Logging/LogMacros.h"

/** WebSocket spec 12: the socket's log category; the transport's own lines (W1) and VaRest's libwebsockets lines go here */
DECLARE_LOG_CATEGORY_EXTERN(LogVaRestSocket, Log, All);

/**
 * The module's LLM tag (plan decision 16): spec 2.1 names the core's VaRest tag, which M4a defines and W1 must not depend
 * on; W5 parents this one under it
 */
LLM_DECLARE_TAG(VaRest_WebSocket);
