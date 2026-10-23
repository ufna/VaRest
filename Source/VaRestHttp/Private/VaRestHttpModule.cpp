// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestOfflineInternal.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestRedactionPipe.h"
#include "VaRestPersistence.h"
#include "VaRestDiskCache.h"
#include "VaRestTrace.h"
#include "VaRestTracePoints.h"

DEFINE_LOG_CATEGORY(LogVaRestHttp);

class FVaRestHttpModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		VaRest::Http::StartRedactionPipe();
		VaRest::Http::StartPersistence();
		// Ordering: OnEnginePreExit fires before any module shuts down (CoreDelegates.h:261-262; GEngine->PreExit() at
		// LaunchEngineLoop.cpp:5065 precedes UnloadModulesAtShutdown at :5182). Modules then shut down last-loaded first
		// (ModuleManager.cpp:1458), so the HTTP module, loaded lazily after this one, can be gone before our ShutdownModule
		// runs: too late to cancel its requests there. In a packaged game the broadcast comes after the worlds' EndPlay(Quit),
		// GameInstance::Shutdown and CleanupWorld (GameEngine.cpp:1373, :1377, :1380, then Super::PreExit() at :1396): a
		// request that completes during that teardown still gets its callbacks. An automation run's Quit force-exits
		// (AutomationCommandline.cpp:504; on Linux a forced RequestExit is _exit, UnixPlatformMisc.cpp:350), so neither
		// this hook nor ShutdownModule runs there.
		PreExitHandle = FCoreDelegates::OnEnginePreExit.AddLambda([]
		{
			VaRest::Http::FClientState::CancelForShutdown();
            VaRest::Http::FOfflineQueue::StopAll();
			VaRest::Http::StopPersistence();
			VaRest::Http::FDiskCache::StopControls();
		});
		// IN-12: a sink that attaches late learns the live clients and in-flight requests. OnBuildSnapshot and the live-client
		// registry it walks are leaked singletons (review I-4), so neither runs anything at static destruction.
		SnapshotHandle = FVaRestTrace::OnBuildSnapshot().AddStatic(&VaRest::Http::TracePoints::ContributeSnapshot);
	}

	virtual void ShutdownModule() override
	{
		FVaRestTrace::OnBuildSnapshot().Remove(SnapshotHandle);
		FCoreDelegates::OnEnginePreExit.Remove(PreExitHandle);
		VaRest::Http::StopPersistence();
		VaRest::Http::FDiskCache::StopControls();
		VaRest::Http::StopRedactionPipe();
	}

private:
	FDelegateHandle PreExitHandle;
	FDelegateHandle SnapshotHandle;
};

IMPLEMENT_MODULE(FVaRestHttpModule, VaRestHttp);
