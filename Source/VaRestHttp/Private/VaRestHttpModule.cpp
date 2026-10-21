// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestRedactionPipe.h"

DEFINE_LOG_CATEGORY(LogVaRestHttp);

class FVaRestHttpModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		VaRest::Http::StartRedactionPipe();
		// Ordering: OnEnginePreExit fires before any module shuts down (CoreDelegates.h:261-262; GEngine->PreExit() at
		// LaunchEngineLoop.cpp:5065 precedes UnloadModulesAtShutdown at :5182). Modules then shut down last-loaded first
		// (ModuleManager.cpp:1458), so the HTTP module, loaded lazily after this one, can be gone before our ShutdownModule
		// runs: too late to cancel its requests there. In a packaged game the broadcast comes after the worlds' EndPlay(Quit),
		// GameInstance::Shutdown and CleanupWorld (GameEngine.cpp:1373, :1377, :1380, then Super::PreExit() at :1396): a
		// request that completes during that teardown still gets its callbacks. An automation run's Quit force-exits
		// (AutomationCommandline.cpp:504; on Linux a forced RequestExit is _exit, UnixPlatformMisc.cpp:350), so neither
		// this hook nor ShutdownModule runs there.
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddStatic(&VaRest::Http::FClientState::CancelForShutdown);
	}

	virtual void ShutdownModule() override
	{
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		VaRest::Http::StopRedactionPipe();
	}

private:
	FDelegateHandle PreExitHandle;
};

IMPLEMENT_MODULE(FVaRestHttpModule, VaRestHttp);
