// Copyright 2014 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestPlugin.h"
#include "VaRestSettings.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestRequestJSON.h"
#include "VaRestPluginPrivatePCH.h"

//#include "UObject/Package.h"
//#include "Misc/ConfigCacheIni.h"

#include "ISettingsModule.h"

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

		KitSettings = NewObject<UVaRestSettings>(GetTransientPackage(), "VaRestSettings", RF_Standalone);
		KitSettings->AddToRoot();

		// We need to manually load the config properties here, as this module is loaded before the UObject system is setup to do this
		GConfig->GetBool(TEXT("/Script/VaRest.VaRestSettings"), TEXT("ExtendedLog"), KitSettings->bExtendedLog, GEngineIni);

		// Register settings
		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->RegisterSettings("Project", "Plugins", "VaRest",
				LOCTEXT("RuntimeSettingsName", "VaRest Kit"),
				LOCTEXT("RuntimeSettingsDescription", "Configure API keys for VaRest"),
				KitSettings
			);
		}
	}

	virtual void ShutdownModule() override
	{
		if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
		{
			SettingsModule->UnregisterSettings("Project", "Plugins", "VaRest");
		}

		if (!GExitPurge)
		{
			// If we're in exit purge, this object has already been destroyed
			KitSettings->RemoveFromRoot();
		}
		else
		{
			KitSettings = nullptr;
		}
	}

	/** Holds the plugin settings */
	UVaRestSettings* KitSettings;
};

IMPLEMENT_MODULE( FVaRestPlugin, VaRestPlugin )

DEFINE_LOG_CATEGORY(LogVaRest);

#undef LOCTEXT_NAMESPACE
