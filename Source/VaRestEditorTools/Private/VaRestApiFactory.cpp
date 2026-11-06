// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestApiFactory.h"

#include "AssetToolsModule.h"
#include "IAssetTools.h"
#include "Modules/ModuleManager.h"
#include "VaRestApi.h"

#define LOCTEXT_NAMESPACE "VaRestApiFactory"

UVaRestApiFactory::UVaRestApiFactory()
{
	SupportedClass = UVaRestApi::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
	bEditorImport = false;
}

UObject* UVaRestApiFactory::FactoryCreateNew(UClass* Class, UObject* Parent, FName Name, EObjectFlags Flags, UObject*, FFeedbackContext*)
{
	if (!Class || !Class->IsChildOf(UVaRestApi::StaticClass()))
		return nullptr;
	return NewObject<UVaRestApi>(Parent, Class, Name, Flags | RF_Transactional);
}

FText UVaRestApiFactory::GetDisplayName() const
{
	return LOCTEXT("DisplayName", "VaRest API");
}

uint32 UVaRestApiFactory::GetMenuCategories() const
{
	return FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().FindAdvancedAssetCategory(TEXT("VaRest"));
}

FName UVaRestApiFactory::GetNewAssetThumbnailOverride() const
{
	return TEXT("ClassThumbnail.DataAsset");
}
FName UVaRestApiFactory::GetNewAssetIconOverride() const
{
	return TEXT("ClassIcon.DataAsset");
}

#undef LOCTEXT_NAMESPACE
