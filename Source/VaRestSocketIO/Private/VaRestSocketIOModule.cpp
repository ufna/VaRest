// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "Modules/ModuleManager.h"
#include "VaRestSioLog.h"
#include "VaRestSioStatsInternal.h"

DEFINE_LOG_CATEGORY(LogVaRestSio);

class FVaRestSocketIOModule final : public IModuleInterface
{
public:
	virtual void StartupModule() override { VaRest::Sio::Stats::StartOnGT(); }
	virtual void ShutdownModule() override { VaRest::Sio::Stats::StopOnGT(); }
	virtual bool SupportsDynamicReloading() override { return false; }
};

IMPLEMENT_MODULE(FVaRestSocketIOModule, VaRestSocketIO);
