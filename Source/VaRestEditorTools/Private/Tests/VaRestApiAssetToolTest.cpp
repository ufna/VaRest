// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "IAssetTypeActions.h"
#include "Misc/AutomationTest.h"
#include "Modules/ModuleManager.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestApi.h"
#include "VaRestApiFactory.h"
#include "VaRestEditorTools.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestApiAssetToolTest, "VaRest.Editor.Api.AssetCreation",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FVaRestApiAssetToolTest::RunTest(const FString& Parameters)
{
	FModuleManager::LoadModuleChecked<FVaRestEditorToolsModule>(TEXT("VaRestEditorTools"));
	TStrongObjectPtr<UVaRestApiFactory> Factory(NewObject<UVaRestApiFactory>());
	TestTrue(TEXT("API factory is available through Create New"), Factory->CanCreateNew() && Factory->ShouldShowInNewMenu());
	TestEqual(TEXT("factory supports the runtime API asset class"), Factory->GetSupportedClass(), UVaRestApi::StaticClass());
	TStrongObjectPtr<UVaRestApi> Api(Cast<UVaRestApi>(Factory->FactoryCreateNew(UVaRestApi::StaticClass(), GetTransientPackage(), NAME_None, RF_Transient, nullptr, nullptr)));
	if (!TestNotNull(TEXT("the real factory creates an API asset"), Api.Get()))
		return false;
	TestTrue(TEXT("factory preserves requested ownership and transactional editing"), Api->GetOuter() == GetTransientPackage() && Api->HasAllFlags(RF_Transient | RF_Transactional));
	TestTrue(TEXT("new API exposes the intended exponential retry default"), Api->Retry.IsEnabled() && Api->Retry.MaxRetries == 3);
	TestNull(TEXT("factory does not create an unrelated class"), Factory->FactoryCreateNew(UObject::StaticClass(), GetTransientPackage(), NAME_None, RF_Transient, nullptr, nullptr));
	IAssetTools& Tools = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get();
	const auto Actions = Tools.GetAssetTypeActionsForClass(UVaRestApi::StaticClass()).Pin();
	if (!TestTrue(TEXT("module registered actual API asset actions"), Actions.IsValid()))
		return false;
	TestEqual(TEXT("Content Browser actions support the same runtime class"), Actions->GetSupportedClass(), UVaRestApi::StaticClass());
	TestTrue(TEXT("factory and actions share the real registered VaRest category"), Factory->GetMenuCategories() == Actions->GetCategories() && Factory->GetMenuCategories() == Tools.FindAdvancedAssetCategory(TEXT("VaRest")));
	TestTrue(TEXT("standard details editing remains available"), Actions->ShouldFindEditorForAsset());
	// UI rendering, opening the Details editor and the actual Content Browser menus need a real Editor window.
	return true;
}

#endif
