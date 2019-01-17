// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPlugin.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestPluginPrivatePCH.h"
#include "VaRestRequestJSON.h"
#include "VaRestSettings.h"

//#include "UObject/Package.h"
//#include "Misc/ConfigCacheIni.h"

#include "Developer/Settings/Public/ISettingsModule.h"

#define LOCTEXT_NAMESPACE "VaRest"

class FVaRestPlugin : public IVaRestPlugin
{
	/** IModuleInterface implementation */
	virtual void StartupModule() override
	{
		// @HACK Force classes to be compiled on shipping build
		UVaRestJsonObject::StaticClass();
		UVaRestJsonValue::StaticClass();
		UVaRestRequestJSON::StaticClass();

		// Register settings
		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->RegisterSettings("Project", "Plugins", "VaRest",
				LOCTEXT("RuntimeSettingsName", "VaRest Kit"),
				LOCTEXT("RuntimeSettingsDescription", "Configure API keys for VaRest"),
				GetMutableDefault<UVaRestSettings>());
		}
	}

	virtual void ShutdownModule() override
	{
		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->UnregisterSettings("Project", "Plugins", "VaRest");
		}
	}
};

IMPLEMENT_MODULE(FVaRestPlugin, VaRestPlugin)

DEFINE_LOG_CATEGORY(LogVaRest);

#undef LOCTEXT_NAMESPACE
