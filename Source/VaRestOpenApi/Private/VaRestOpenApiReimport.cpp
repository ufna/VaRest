// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiReimportPrivate.h"
#include "VaRestOpenApiSource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "EditorFramework/AssetImportData.h"
#include "Misc/PackageName.h"
#include "Misc/ScopeExit.h"
#include "Misc/SecureHash.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"

namespace VaRest::OpenApi
{
namespace { TSet<const UVaRestApi*> ApplyingApis; }
FVaRestOpenApiReimportResult FReimport::Run()
{
	check(IsInGameThread());FVaRestOpenApiReimportResult Result;Result.Issues=Preview.Issues;
	const auto Fail=[&](const TCHAR* Code,const FString& Message)
	{
		FVaRestOpenApiIssue Issue;Issue.Code=Code;Issue.Message=Message;Issue.AssetPath=Api.GetPathName();Result.Issues.Add(MoveTemp(Issue));
	};
	if (!Preview.bValid || !FullPlan.bValid || ApplyingApis.Contains(&Api)) { Fail(TEXT("OA-REIMPORT-STATE"),TEXT("The reimport preview is invalid or this API is already being applied."));return Result; }
	if (!Current()) { Fail(TEXT("OA-REIMPORT-STALE"),TEXT("The API or a generated type changed after the preview. Refresh the preview before applying."));return Result; }
	Preview.Merge.ResolveChoices(Preview.Mine,Preview.Theirs);
	auto Bases=Preview.Merge.NextBase(Preview.Base,Preview.Theirs);
	const auto Suppressed=Suppressions(),Deferred=Deferrals();
	const bool MetadataChanged=!FVaRestOpenApiMerge::ItemsJson(Bases).Equals(Preview.Manifest[TEXT("items")]) || !Suppressed.Equals(Preview.Manifest[TEXT("suppressions")]) || !Deferred.Equals(Preview.Manifest[TEXT("deferred")]) ||
		(Options.Options.IsValid() && !Options.Options.Equals(Preview.Manifest[TEXT("options")]));
	TStringMap<bool> Present;for (const auto& Item:Preview.Merge.Items) Present.Add(Item.Id,Item.bPresent);
	bool NativeChanges=false;for (const auto& Change:Preview.Merge.Changes) NativeChanges=NativeChanges || (Change.bApply && Present.FindRef(Change.Id));
	if (!NativeChanges && !MetadataChanged) { Result.Api=&Api;Result.bNoChanges=true;return Result; }
	if (Options.IsCancelled && Options.IsCancelled()) { Result.bCancelled=true;return Result; }
	ApplyingApis.Add(&Api);ON_SCOPE_EXIT { ApplyingApis.Remove(&Api); };
	FScopedTransaction Transaction(NSLOCTEXT("VaRestOpenApi","ReimportTransaction","Reimport VaRest OpenAPI"));
	bool Committed=false;
	ON_SCOPE_EXIT { Transaction.Cancel();if (!Committed) Stage.Discard(); };
	Prepare();
	if (Stage.Folder.Len()+81>=NAME_SIZE || !FPackageName::IsValidLongPackageName(Stage.Folder/TEXT("VaRestType"))) { Fail(TEXT("OA-APPLY-TARGET"),TEXT("The types folder is invalid or exceeds the engine name limit."));return Result; }
	TSet<TWeakObjectPtr<UObject>> Touched;
	const auto Modified=FCoreUObjectDelegates::OnObjectModified.AddLambda([&](UObject* Object) { Touched.Add(Object); });
	ON_SCOPE_EXIT { FCoreUObjectDelegates::OnObjectModified.Remove(Modified); };
	if (NativeChanges)
	{
		Stage.Enums();Stage.Structs();CleanupFailed();
		if (Options.IsCancelled && Options.IsCancelled()) { Result.bCancelled=true;return Result; }
		// No cancellation from here: existing assets are changed in place, with explicit diagnostics on native refusal.
		for (auto* Item:All)
		{
			const auto* Before=Live.Find(Item->Id);
			if (Before && (*Before)->bPresent && !(*Before)->Properties.Equals(Item->Properties))
			{
				const auto* Type=Stage.Types.Find(Item->Owner.IsEmpty() ? Item->Id : Item->Owner);
				if (Type) { Touched.Add(*Type);if (const auto* Struct=Cast<UUserDefinedStruct>(*Type)) Touched.Add(Struct->EditorData.Get()); }
				if (Equal(Item->Kind,TEXT("auth")) && Api.Auth) Touched.Add(Api.Auth.Get());
			}
		}
		ExistingTypes();CleanupFailed();Api.Modify();Touched.Add(&Api);Stage.EditApi(All,Live);Stage.EditRules(All);
		for (const auto& Key:FullPlan.Auth.RedactedKeys) if (!Api.RedactedKeys.ContainsByPredicate([&](const FString& Existing) { return Equal(Existing,Key); })) Api.RedactedKeys.Add(Key);
		RecordActualBases(Bases);
	}
	Result.Issues.Append(Stage.Result.Issues);
	auto State=Preview.Manifest.Clone();State.Set(TEXT("items"),FVaRestOpenApiMerge::ItemsJson(Bases)).Set(TEXT("suppressions"),Suppressions()).Set(TEXT("deferred"),Deferrals()).Set(TEXT("plan"),FullPlan.ToJson()).Set(TEXT("report"),IssueJson(Result.Issues));
	const auto History=Preview.Manifest[TEXT("endpointIdentities")];State.Set(TEXT("endpointIdentities"),EndpointIdentityHistory(History.IsArray() ? History : Preview.Manifest[TEXT("plan")][TEXT("endpoints")],State[TEXT("plan")]));
	if (Options.Source.IsValid()) State.Set(TEXT("source"),Options.Source);if (Options.Options.IsValid()) State.Set(TEXT("options"),Options.Options);
	FVaRestApiImportState Encoded;
	if (!EncodeState(State,Encoded))
	{
		Fail(TEXT("OA-APPLY-STATE"),TEXT("The updated import state could not be encoded. Existing edits require review; the previous manifest remains available."));
		if (!NativeChanges) return Result;
	}
	else
	{
		Api.Modify();Touched.Add(&Api);Api.ImportState=MoveTemp(Encoded);
		if (Options.Source.IsValid() && Api.AssetImportData)
		{
			Api.AssetImportData->Modify();Touched.Add(Api.AssetImportData.Get());TArray<FAssetImportInfo::FSourceFile> Sources;
			if (Equal(Options.Source[TEXT("kind")].AsString(),TEXT("file"))) for (const auto& Document:Options.Source[TEXT("documents")].Elements()) { FMD5Hash Hash;LexFromString(Hash,*Document[TEXT("hash")].AsString());Sources.Emplace(UAssetImportData::SanitizeImportFilename(Document[TEXT("location")].AsString(),Api.GetOutermost()),FDateTime(0),Hash); }
			Api.AssetImportData->SetSourceFiles(MoveTemp(Sources));
		}
	}
	for (auto& Object:Stage.Created) if (Object.IsValid())
	{
		Object->MarkPackageDirty();if (Options.bRegisterAssets) FAssetRegistryModule::AssetCreated(Object.Get());Result.AdditionalObjects.Add(Object.Get());
	}
	Api.MarkPackageDirty();if (NativeChanges) Api.NotifyEdited();Transaction.Cancel();
	if (GEditor)
	{
		for (const auto& Object:Touched) if (Object.IsValid() && GEditor->IsObjectInTransactionBuffer(Object.Get())) { Result.bUndoHistoryCleared=true;break; }
		if (Result.bUndoHistoryCleared) GEditor->ResetTransaction(NSLOCTEXT("VaRestOpenApi","ReimportResetUndo","OpenAPI reimport updated generated assets"));
	}
	TSet<FName> ChangedPackages;ChangedPackages.Add(Api.GetOutermost()->GetFName());
	for (const auto& Object:Stage.Created) if (Object.IsValid()) ChangedPackages.Add(Object->GetOutermost()->GetFName());
	for (const auto& Object:Touched) if (Object.IsValid())
	{
		const auto* Package=Object->GetOutermost();if (Package!=GetTransientPackage() && Package->IsDirty() && !Package->HasAnyPackageFlags(PKG_CompiledIn)) ChangedPackages.Add(Package->GetFName());
	}
	Result.ChangedPackages=ChangedPackages.Array();Result.ChangedPackages.Sort(FNameLexicalLess());
	Committed=true;Result.Api=&Api;return Result;
}
}
FVaRestOpenApiReimportResult FVaRestOpenApiReimporter::Apply(UVaRestApi* Api,const FVaRestOpenApiPlan& Plan,FVaRestOpenApiReimportPreview& Preview,const FVaRestOpenApiApplyOptions& Options)
{
	if (!Api) return {};return VaRest::OpenApi::FReimport(*Api,Plan,Preview,Options).Run();
}
