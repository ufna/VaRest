// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRest.h"

#include "VaRestDefines.h"
#include "VaRestJsonObject.h"
#include "VaRestJsonValue.h"
#include "VaRestRequestController.h"
#include "VaRestRequestJSON.h"
#include "VaRestSettings.h"

#include "Developer/Settings/Public/ISettingsModule.h"

#define LOCTEXT_NAMESPACE "FVaRestModule"

void FVaRestModule::StartupModule()
{
	// @HACK Force classes to be compiled on shipping build
	UVaRestJsonObject::StaticClass();
	UVaRestJsonValue::StaticClass();
	UVaRestRequestJSON::StaticClass();

	VaRestSettings = NewObject<UVaRestSettings>(GetTransientPackage(), "VaRestSettings", RF_Standalone);
	VaRestSettings->AddToRoot();

	// Register settings
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->RegisterSettings("Project", "Plugins", "VaRest",
			LOCTEXT("RuntimeSettingsName", "VaRest"),
			LOCTEXT("RuntimeSettingsDescription", "Configure VaRest plugin settings"),
			VaRestSettings);
	}

	FWorldDelegates::OnWorldCleanup.AddLambda([this](UWorld* World, bool bSessionEnded, bool bCleanupResources) {
		RequestControllers.Remove(World);

		UE_LOG(LogVaRest, Log, TEXT("%s: Request Controller is removed for: %s"), *VA_FUNC_LINE, *World->GetName());
	});

	FWorldDelegates::OnPostWorldInitialization.AddLambda([this](UWorld* World, const UWorld::InitializationValues IVS) {
		auto Controller = NewObject<UVaRestRequestController>(GetTransientPackage());
		Controller->SetFlags(RF_Standalone);
		Controller->AddToRoot();

		Controller->Initialize();

		RequestControllers.Add(World, Controller);

		UE_LOG(LogVaRest, Log, TEXT("%s: Request Controller is created for: %s"), *VA_FUNC_LINE, *World->GetName());
	});

	UE_LOG(LogVaRest, Log, TEXT("%s: VaRest module started"), *VA_FUNC_LINE);
}

void FVaRestModule::ShutdownModule()
{
	if (ISettingsModule* SettingsModule = FModuleManager::GetModulePtr<ISettingsModule>("Settings"))
	{
		SettingsModule->UnregisterSettings("Project", "Plugins", "VaRest");
	}

	if (!GExitPurge)
	{
		VaRestSettings->RemoveFromRoot();

		// If we're in exit purge, this object has already been destroyed
		for (auto Controller : RequestControllers)
		{
			Controller.Value->RemoveFromRoot();
		}
	}
	else
	{
		VaRestSettings = nullptr;
	}

	RequestControllers.Empty();
}

UVaRestSettings* FVaRestModule::GetSettings() const
{
	check(VaRestSettings);
	return VaRestSettings;
}

UVaRestRequestController* FVaRestModule::GetRequestController(UWorld* World) const
{
	return RequestControllers.FindChecked(World);
}

IMPLEMENT_MODULE(FVaRestModule, VaRest)

DEFINE_LOG_CATEGORY(LogVaRest);

#undef LOCTEXT_NAMESPACE
