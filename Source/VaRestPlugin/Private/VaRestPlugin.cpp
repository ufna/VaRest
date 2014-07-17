// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"

class FVaRestPlugin : public IVaRestPlugin
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		// @HACK Force classes to be compiled on shipping build
		UVaRestJsonObject::StaticClass();
		UVaRestJsonValue::StaticClass();
		UVaRestRequestJSON::StaticClass();
		UVaRestParseManager::StaticClass();
	}

	virtual void ShutdownModule() override
	{

	}
};

IMPLEMENT_MODULE( FVaRestPlugin, VaRestPlugin )

DEFINE_LOG_CATEGORY(LogVaRest);
