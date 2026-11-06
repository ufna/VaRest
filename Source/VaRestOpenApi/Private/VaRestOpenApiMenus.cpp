// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiMenus.h"
#include "VaRestOpenApiLibrary.h"
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiFactory.h"
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestApi.h"
#include "ContentBrowserMenuContexts.h"
#include "ContentBrowserDataMenuContexts.h"
#include "ContentBrowserDataSubsystem.h"
#include "IContentBrowserDataModule.h"
#include "ContentBrowserModule.h"
#include "IContentBrowserSingleton.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "DesktopPlatformModule.h"
#include "Framework/Application/SlateApplication.h"
#include "IDesktopPlatform.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "ToolMenus.h"

namespace VaRest::OpenApi
{
namespace
{
bool Imported(const FAssetData& Data)
{
	FString Importer;if (Data.GetTagValue(TEXT("VaRestImporter"),Importer)) return Importer.Equals(TEXT("VaRestOpenApi"),ESearchCase::IgnoreCase);
	const auto* Api=Cast<UVaRestApi>(Data.FastGetAsset(false));return Api && Api->ImportState.Importer==TEXT("VaRestOpenApi");
}
void ChangeSource(const TArray<FAssetData>& Assets)
{
	if (Assets.Num()!=1) return;auto* Api=Cast<UVaRestApi>(Assets[0].GetAsset());auto* Desktop=FDesktopPlatformModule::Get();if (!Api || !Desktop) return;
	TArray<FString> Files;const void* Parent=FSlateApplication::Get().FindBestParentWindowHandleForDialogs(nullptr);
	if (Desktop->OpenFileDialog(Parent,TEXT("Change OpenAPI source"),FPaths::ProjectDir(),TEXT(""),TEXT("OpenAPI (*.json;*.yaml;*.yml)|*.json;*.yaml;*.yml"),EFileDialogFlags::None,Files) && Files.Num()==1) UVaRestOpenApiLibrary::ChangeFileSource(Api,Files[0]);
}
void ChangeUrl(const TArray<FAssetData>& Assets)
{
	if (Assets.Num()!=1) return;TStrongObjectPtr<UVaRestApi> Api(Cast<UVaRestApi>(Assets[0].GetAsset()));if (!Api.IsValid()) return;
	FString Url;const auto State=DecodeState(Api->ImportState);if (State.IsSet() && Equal(State.GetValue()[TEXT("source")][TEXT("kind")].AsString(),TEXT("url"))) Url=State.GetValue()[TEXT("source")][TEXT("location")].AsString();
	if (ShowSourceUrl(Url)) UVaRestOpenApiLibrary::ChangeUrlSource(Api.Get(),Url);
}
void ImportUrl(const FString& Folder)
{
	FString Url,Destination=Folder/TEXT("Api");if (!ShowSourceUrl(Url,&Destination)) return;
	TStrongObjectPtr<UVaRestOpenApiFactory> Factory(NewObject<UVaRestOpenApiFactory>());bool Cancelled=false;
	auto* Api=Cast<UVaRestApi>(Factory->CreateFromSource(UVaRestApi::StaticClass(),CreatePackage(*Destination),*FPackageName::GetLongPackageAssetName(Destination),RF_Public|RF_Standalone|RF_Transactional,Url,true,Cancelled));
	if (!Api) return;FAssetRegistryModule::AssetCreated(Api);for (auto* Object:Factory->GetAdditionalImportedObjects()) FAssetRegistryModule::AssetCreated(Object);
	FModuleManager::LoadModuleChecked<FContentBrowserModule>(TEXT("ContentBrowser")).Get().SyncBrowserToAssets(TArray<UObject*>{Api});
}
void AddUrlImport(FToolMenuSection& Section,const FString& Folder,bool CanModify)
{
	Section.AddSubMenu(TEXT("VaRestImport"),NSLOCTEXT("VaRestOpenApi","SourceMenu","VaRest"),FText::GetEmpty(),FNewToolMenuDelegate::CreateLambda([Folder,CanModify](UToolMenu* SubMenu)
	{
		auto& Import=SubMenu->AddSection(TEXT("Import"));Import.AddMenuEntry(TEXT("ImportOpenApiUrl"),NSLOCTEXT("VaRestOpenApi","ImportUrlMenu","Import OpenAPI from URL…"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Folder]() { ImportUrl(Folder); }),FCanExecuteAction::CreateLambda([CanModify]() { return CanModify; })));
	}));
}
}
void RegisterSourceMenus()
{
	FToolMenuOwnerScoped Owner(TEXT("VaRestOpenApi"));
	UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AddNewContextMenu"))->AddSection(TEXT("VaRestOpenApiImport")).AddDynamicEntry(TEXT("OpenApiUrlImport"),FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section)
	{
		const auto* Context=Section.FindContext<UContentBrowserDataMenuContext_AddNewMenu>();if (!Context || Context->SelectedPaths.Num()!=1) return;
		FString Folder;if (IContentBrowserDataModule::Get().GetSubsystem()->TryConvertVirtualPath(FNameBuilder(Context->SelectedPaths[0]),Folder)!=EContentBrowserPathType::Internal) return;
		AddUrlImport(Section,Folder,Context->bCanBeModified && Context->bContainsValidPackagePath);
	}));
	UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.FolderContextMenu"))->AddSection(TEXT("VaRestOpenApiImport")).AddDynamicEntry(TEXT("OpenApiUrlImport"),FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section)
	{
		const auto* Context=Section.FindContext<UContentBrowserFolderContext>();if (!Context || Context->SelectedPackagePaths.Num()!=1) return;AddUrlImport(Section,Context->SelectedPackagePaths[0],Context->bCanBeModified);
	}));
	auto* Menu=UToolMenus::Get()->ExtendMenu(TEXT("ContentBrowser.AssetContextMenu.VaRestApi"));
	Menu->AddSection(TEXT("VaRestOpenApiSource")).AddDynamicEntry(TEXT("OpenApiSourceActions"),FNewToolMenuSectionDelegate::CreateLambda([](FToolMenuSection& Section)
	{
		const auto* Context=Section.FindContext<UContentBrowserAssetContextMenuContext>();if (!Context) return;TArray<FAssetData> Assets;for (const auto& Data:Context->SelectedAssets) if (Imported(Data)) Assets.Add(Data);if (Assets.IsEmpty()) return;
		const bool CanModify=Context->bCanBeModified;
		Section.AddSubMenu(TEXT("VaRestSource"),NSLOCTEXT("VaRestOpenApi","SourceMenu","VaRest"),NSLOCTEXT("VaRestOpenApi","SourceMenuTip","OpenAPI source and generated declarations"),FNewToolMenuDelegate::CreateLambda([Assets,CanModify](UToolMenu* SubMenu)
		{
			auto& Source=SubMenu->AddSection(TEXT("OpenApi"),NSLOCTEXT("VaRestOpenApi","SourceSection","OpenAPI"));
			const auto Modify=FCanExecuteAction::CreateLambda([CanModify]() { return CanModify; });
			Source.AddMenuEntry(TEXT("Reimport"),NSLOCTEXT("VaRestOpenApi","ReimportSource","Reimport from source…"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Assets]() { for (const auto& Data:Assets) if (auto* Api=Cast<UVaRestApi>(Data.GetAsset())) FVaRestOpenApiReimporter::FromSource(Api); }),Modify));
			Source.AddMenuEntry(TEXT("Check"),NSLOCTEXT("VaRestOpenApi","CheckSource","Check for API changes"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Assets]() { for (const auto& Data:Assets) if (auto* Api=Cast<UVaRestApi>(Data.GetAsset())) FVaRestOpenApiReimporter::FromSource(Api,false,false,true); })));
			Source.AddMenuEntry(TEXT("Report"),NSLOCTEXT("VaRestOpenApi","LastReport","Show last import report"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Assets]() { for (const auto& Data:Assets) if (auto* Api=Cast<UVaRestApi>(Data.GetAsset())) UVaRestOpenApiLibrary::ShowLastImportReport(Api); })));
			Source.AddMenuEntry(TEXT("Cleanup"),NSLOCTEXT("VaRestOpenApi","CleanupRemoved","Clean up removed items…"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Assets]() { for (const auto& Data:Assets) if (auto* Api=Cast<UVaRestApi>(Data.GetAsset())) UVaRestOpenApiLibrary::CleanUpRemovedItems(Api); }),Modify));
			if (Assets.Num()==1) Source.AddMenuEntry(TEXT("ChangeSource"),NSLOCTEXT("VaRestOpenApi","ChangeSource","Change source file…"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Assets]() { ChangeSource(Assets); }),Modify));
			if (Assets.Num()==1) Source.AddMenuEntry(TEXT("ChangeUrl"),NSLOCTEXT("VaRestOpenApi","ChangeUrl","Change source URL…"),FText::GetEmpty(),FSlateIcon(),FUIAction(FExecuteAction::CreateLambda([Assets]() { ChangeUrl(Assets); }),Modify));
		}));
	}));
}
}
