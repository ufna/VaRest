// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"

namespace VaRest::LwsProbe
{
	/**
	 * Whether the engine's WebSockets module was loaded when this module started, before any test ran (WS W0 review I-4):
	 * automation runs tests in name order, so Probe.VaRest.Lws.Coexist.* (which loads WebSockets) runs before
	 * Probe.VaRest.Lws.Service.Environment, which reports this value. WebSocket spec D30 branches on it.
	 */
	bool GWebSocketsLoadedAtModuleStartup = false;
}

class FVaRestTestsModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		VaRest::LwsProbe::GWebSocketsLoadedAtModuleStartup = FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets"));
	}
};

IMPLEMENT_MODULE(FVaRestTestsModule, VaRestTests);
