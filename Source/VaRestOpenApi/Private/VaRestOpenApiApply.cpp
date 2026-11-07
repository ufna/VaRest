// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApply.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "VaRestHttpSettings.h"

namespace VaRest::OpenApi
{
void FApply::Issue(const TCHAR* Code,const FString& Message,const UObject* Object,EVaRestOpenApiSeverity Severity)
{
	FVaRestOpenApiIssue Row;Row.Code=Code;Row.Message=Message;Row.Severity=Severity;if (Object) Row.AssetPath=Object->GetPathName();Result.Issues.Add(MoveTemp(Row));
}
void FApply::Keep(UObject* Object) { Created.Emplace(Object); }
bool FApply::Cancelled()
{
	if (Options.IsCancelled && Options.IsCancelled()) Result.bCancelled=true;return Result.bCancelled;
}
UPackage* FApply::TypePackage(const FString& Name,FName& OutName)
{
	if (Folder.Len()+Name.Len()+17>=NAME_SIZE) { Issue(TEXT("OA-APPLY-PACKAGE"),TEXT("The type package path exceeds the engine name limit."),nullptr,EVaRestOpenApiSeverity::Error);return nullptr; }
	FString PackageName,AssetName;
	FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools")).Get().CreateUniqueAssetName(Folder/Name,TEXT(""),PackageName,AssetName);
	if (!FPackageName::IsValidLongPackageName(PackageName) || !FName::IsValidXName(AssetName,INVALID_OBJECTNAME_CHARACTERS))
	{ Issue(TEXT("OA-APPLY-PACKAGE"),TEXT("Cannot allocate a type package for ")+Name,nullptr,EVaRestOpenApiSeverity::Error);return nullptr; }
	OutName=*AssetName;return CreatePackage(*PackageName);
}
void FApply::Discard()
{
	for (auto& Object:Created)
	{
		if (!Object.IsValid()) continue;
		Object->ClearFlags(RF_Public|RF_Standalone);Object->MarkAsGarbage();
		if (Object->GetOutermost()!=Options.Parent->GetOutermost()) Object->GetOutermost()->SetDirtyFlag(false);
	}
	Result.Api=nullptr;Result.AdditionalObjects.Reset();
}
FVaRestOpenApiApplyResult FApply::Run()
{
	check(IsInGameThread());
	if (!Plan.bValid || !Options.Parent || !Options.Parent->IsA<UPackage>() || Options.ApiName.IsNone() || !FName::IsValidXName(Options.ApiName.ToString(),INVALID_OBJECTNAME_CHARACTERS) ||
		FindObjectFast<UObject>(Options.Parent,Options.ApiName))
	{ Issue(TEXT("OA-APPLY-TARGET"),TEXT("A valid plan and an unused primary asset name/package are required; use reimport for an existing API."),nullptr,EVaRestOpenApiSeverity::Error);return MoveTemp(Result); }
	Folder=Options.TypesFolder.IsEmpty() ? FPackageName::GetLongPackagePath(Options.Parent->GetOutermost()->GetName())/(Options.ApiName.ToString()+TEXT("_Types")) : Options.TypesFolder;
	Folder.RemoveFromEnd(TEXT("/"));
	if (Folder.Len()+81>=NAME_SIZE || !FPackageName::IsValidLongPackageName(Folder/TEXT("VaRestType")))
	{ Issue(TEXT("OA-APPLY-TARGET"),TEXT("Types folder must be a writable mounted content path."),nullptr,EVaRestOpenApiSeverity::Error);return MoveTemp(Result); }
	const bool ParentWasDirty=Options.Parent->GetOutermost()->IsDirty();
	FScopedTransaction Transaction(NSLOCTEXT("VaRestOpenApi","Import","Import VaRest OpenAPI"));
	bool Committed=false;
	ON_SCOPE_EXIT { Transaction.Cancel();if (!Committed) { Discard();Options.Parent->GetOutermost()->SetDirtyFlag(ParentWasDirty); } };
	if (Cancelled()) return MoveTemp(Result);
	Enums();if (Cancelled()) return MoveTemp(Result);
	Structs();if (Cancelled()) return MoveTemp(Result);
	for (const auto& Id:FailedTypes) if (auto* Found=Types.Find(Id))
	{
		for (auto& Object:Created) if (Object.Get()==*Found) { Object->ClearFlags(RF_Public|RF_Standalone);Object->GetOutermost()->SetDirtyFlag(false);Object->MarkAsGarbage();Object.Reset();break; }
		Types.Remove(Id);
	}
	Api();Rules();if (!Manifest()) { Result.Api=nullptr;return MoveTemp(Result); }
	if (Cancelled()) { Result.Api=nullptr;return MoveTemp(Result); }
	// Commit is indivisible from the UI's perspective. Callbacks can no longer cancel this operation.
	for (auto& Object:Created)
	{
		if (!Object.IsValid()) continue;
		Object->MarkPackageDirty();if (Options.bRegisterAssets) FAssetRegistryModule::AssetCreated(Object.Get());
		if (Object.Get()!=Result.Api) Result.AdditionalObjects.Add(Object.Get());
	}
	Result.Api->NotifyEdited();
	if (Options.bAddToSettings)
	{
		auto* Settings=GetMutableDefault<UVaRestHttpSettings>();const TSoftObjectPtr<UVaRestApi> Ref(Result.Api);
		if (!Settings->Apis.Contains(Ref))
		{
			Settings->Apis.Add(Ref);
			if (!Settings->TryUpdateDefaultConfigFile()) Issue(TEXT("OA-APPLY-SETTINGS"),TEXT("The API preload entry exists in this editor session but could not be saved to project settings; save VaRest HTTP settings before cooking."),Result.Api);
		}
	}
	Transaction.Cancel();
	if (GEditor)
	{
		for (const auto& Object:Created) if (GEditor->IsObjectInTransactionBuffer(Object.Get())) { Result.bUndoHistoryCleared=true;break; }
		if (Result.bUndoHistoryCleared) GEditor->ResetTransaction(NSLOCTEXT("VaRestOpenApi","ResetUndo","OpenAPI import updated generated assets"));
	}
	Committed=true;return MoveTemp(Result);
}
}
FVaRestOpenApiApplyResult FVaRestOpenApiApplier::Apply(const FVaRestOpenApiPlan& Plan,const FVaRestOpenApiApplyOptions& Options)
{
	return VaRest::OpenApi::FApply(Plan,Options).Run();
}
