// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VaRestHttpLog.h"
#include "VaRestRedactionPipe.h"

DEFINE_LOG_CATEGORY(LogVaRestHttp);

class FVaRestHttpModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		VaRest::Http::StartRedactionPipe();
	}

	virtual void ShutdownModule() override
	{
		VaRest::Http::StopRedactionPipe();
	}
};

IMPLEMENT_MODULE(FVaRestHttpModule, VaRestHttp);
