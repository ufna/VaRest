// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "HAL/LowLevelMemTracker.h"

/**
 * Spec 4.8: LLM tag VaRest (requests, bodies, decodes); VaRest_Trace ("VaRest/Trace") for the trace's copies and excerpts
 * (Inspector IN-14). Run with -llm to see them; another module scopes its own work with LLM_SCOPE_BYTAG(VaRest).
 */
LLM_DECLARE_TAG_API(VaRest, VARESTHTTP_API);
LLM_DECLARE_TAG_API(VaRest_Trace, VARESTHTTP_API);
