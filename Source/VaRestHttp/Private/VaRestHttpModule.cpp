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
		// Before core modules shut down (CoreDelegates.h:261-262); ShutdownModule does not run in an automation session
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
