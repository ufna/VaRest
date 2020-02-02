// Copyright 2014-2020 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSubsystem.h"

#include "VaRestDefines.h"
#include "VaRestSettings.h"

#include "Developer/Settings/Public/ISettingsModule.h"

#define LOCTEXT_NAMESPACE "FVaRest"

UVaRestSubsystem::UVaRestSubsystem()
	: UGameInstanceSubsystem()
{
}

void UVaRestSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	Settings = NewObject<UVaRestSettings>(GetTransientPackage(), "VaRestSettings", RF_Standalone);

	// Register settings
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "VaRest",
			LOCTEXT("RuntimeSettingsName", "VaRest"),
			LOCTEXT("RuntimeSettingsDescription", "Configure VaRest plugin settings"),
			Settings);
	}

	UE_LOG(LogVaRest, Log, TEXT("%s: VaRest subsystem initialized"), *VA_FUNC_LINE);
}

void UVaRestSubsystem::Deinitialize()
{
	// Do nothing for now
	Super::Deinitialize();
}

UVaRestSettings* UVaRestSubsystem::GetSettings() const
{
	check(Settings);
	return Settings;
}

#undef LOCTEXT_NAMESPACE
