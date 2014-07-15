// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPluginPrivatePCH.h"
//#include "VaRestPlugin.generated.inl"

class FVaRestPlugin : public IVaRestPlugin
{
	/** IModuleInterface implementation */
	virtual void StartupModule() OVERRIDE
	{
		// @HACK Force classes to be compiled on shipping build
		UVaRestJsonObject::StaticClass();
		UVaRestJsonValue::StaticClass();
		UVaRestRequestJSON::StaticClass();
		UVaRestParseManager::StaticClass();
	}

	virtual void ShutdownModule() OVERRIDE
	{

	}
};

IMPLEMENT_MODULE( FVaRestPlugin, VaRestPlugin )

DEFINE_LOG_CATEGORY(LogVaRest);
