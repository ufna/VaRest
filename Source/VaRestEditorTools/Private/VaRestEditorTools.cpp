// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestEditorTools.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "ToolMenus.h"
#include "VaRestApiAssetTypeActions.h"
#include "VaRestSioComponentDetails.h"
#include "VaRestSioComponent.h"
#include "PropertyEditorModule.h"

#define LOCTEXT_NAMESPACE "VaRestEditorTools"

FName FVaRestEditorToolsModule::ApiAssetMenuName()
{
	return TEXT("ContentBrowser.AssetContextMenu.VaRestApi");
}
FName FVaRestEditorToolsModule::ApiAssetExtensionSection()
{
	return TEXT("VaRestApiExtensions");
}

void FVaRestEditorToolsModule::StartupModule()
{
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	const uint32 Category = Tools.RegisterAdvancedAssetCategory(TEXT("VaRest"), LOCTEXT("Category", "VaRest"));
	ApiActions = MakeShared<FVaRestApiAssetTypeActions>(Category);
	Tools.RegisterAssetTypeActions(ApiActions.ToSharedRef());
	auto& Properties = FModuleManager::LoadModuleChecked<FPropertyEditorModule>(TEXT("PropertyEditor"));
	Properties.RegisterCustomClassLayout(UVaRestSioComponent::StaticClass()->GetFName(), FOnGetDetailCustomizationInstance::CreateStatic(&FVaRestSioComponentDetails::MakeInstance));
	Properties.NotifyCustomizationModuleChanged();
	MenuStartup = UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FVaRestEditorToolsModule::RegisterMenus));
}

void FVaRestEditorToolsModule::RegisterMenus()
{
	FToolMenuOwnerScoped Owner(this);
	UToolMenu* Menu = UToolMenus::Get()->ExtendMenu(ApiAssetMenuName());
	Menu->FindOrAddSection(ApiAssetExtensionSection(), LOCTEXT("ApiExtensions", "VaRest"));
}

void FVaRestEditorToolsModule::ShutdownModule()
{
	if (auto* Properties = FModuleManager::GetModulePtr<FPropertyEditorModule>(TEXT("PropertyEditor")))
	{
		Properties->UnregisterCustomClassLayout(UVaRestSioComponent::StaticClass()->GetFName());
		Properties->NotifyCustomizationModuleChanged();
	}
	UToolMenus::UnRegisterStartupCallback(MenuStartup);
	MenuStartup.Reset();
	UToolMenus::UnregisterOwner(this);
	if (ApiActions.IsValid())
	{
		if (auto* Tools = FModuleManager::GetModulePtr<FAssetToolsModule>(TEXT("AssetTools")))
			Tools->Get().UnregisterAssetTypeActions(ApiActions.ToSharedRef());
		ApiActions.Reset();
	}
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVaRestEditorToolsModule, VaRestEditorTools)
