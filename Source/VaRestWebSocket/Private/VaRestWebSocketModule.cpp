// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VaRestWebSocketLog.h"

DEFINE_LOG_CATEGORY(LogVaRestSocket);
LLM_DEFINE_TAG(VaRest_WebSocket);

/** Task 7 adds the startup policy (plan decision 8) and the engine-exit and fork hooks */
class FVaRestWebSocketModule final : public IModuleInterface
{
};

IMPLEMENT_MODULE(FVaRestWebSocketModule, VaRestWebSocket);
