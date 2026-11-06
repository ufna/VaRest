// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiSnapshots.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

bool FVaRestOpenApiReimporter::SeedOptions(UVaRestApi* Api,FVaRestOpenApiPlanOptions& Options)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());if (!Api) return false;
	const auto State=DecodeState(Api->ImportState);if (!State.IsSet()) return false;
	Options.ExistingEndpoints.Reset();Options.ExistingDefinitions.Reset();
	const auto Live=FVaRestOpenApiMerge::Capture(Api,State.GetValue());
	for (const auto& Item:Live) if (Equal(Item.Kind,TEXT("struct")) || Equal(Item.Kind,TEXT("enum")))
	{
		FString Name=FPackageName::ObjectPathToObjectName(Item.Target);
		if (Name.IsEmpty()) for (const TCHAR* Key:{TEXT("structs"),TEXT("enums")}) for (const auto& Row:State.GetValue()[TEXT("plan")][Key].Elements()) if (Equal(Row[TEXT("id")].AsString(),Item.Id)) Name=Row[TEXT("name")].AsString();
		if (!Name.IsEmpty()) Options.ExistingDefinitions.Emplace(Item.Id,Name);
	}
	const auto History=State.GetValue()[TEXT("endpointIdentities")];const auto Identities=History.IsArray() ? History : State.GetValue()[TEXT("plan")][TEXT("endpoints")];
	for (const auto& Row:Identities.Elements())
	{
		FVaRestOpenApiEndpointIdentity Identity;Identity.Id=Row[TEXT("id")].AsString();Identity.OperationId=Row[TEXT("operationId")].AsString();Identity.Verb=Row[TEXT("verb")].AsString();Identity.Path=Row[TEXT("path")].AsString();Options.ExistingEndpoints.Add(MoveTemp(Identity));
	}
	return true;
}
FVaRestOpenApiReimportPreview FVaRestOpenApiReimporter::Preview(UVaRestApi* Api,const FVaRestOpenApiPlan& Plan,const FVaRestOpenApiMergeOptions& InOptions)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());FVaRestOpenApiReimportPreview Out;Out.Issues=Plan.Issues;
	const auto State=Api ? DecodeState(Api->ImportState) : TOptional<FVaRestJson>();
	if (!Plan.bValid || !State.IsSet())
	{
		FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-REIMPORT-STATE");Issue.Message=TEXT("A valid plan and a readable VaRest OpenAPI import manifest are required.");Out.Issues.Add(MoveTemp(Issue));return Out;
	}
	Out.Manifest=State.GetValue();Out.Base=FVaRestOpenApiMerge::Base(Out.Manifest);Out.Mine=FVaRestOpenApiMerge::Capture(Api,Out.Manifest);Out.Theirs=FVaRestOpenApiMerge::Desired(Plan,Out.Manifest,Out.Mine);
	auto Options=InOptions;for (const auto& Id:Out.Manifest[TEXT("suppressions")].Elements()) Options.SuppressedIds.Add(Id.AsString());
	Out.Merge=FVaRestOpenApiMerge::Compare(Out.Base,Out.Mine,Out.Theirs,Options);
	TStringMap<const FVaRestOpenApiMergeItem*> Items;for (const auto& Item:Out.Merge.Items) Items.Add(Item.Id,&Item);
	TSet<FName> Packages;
	for (const auto& Change:Out.Merge.Changes) if (Change.bApply)
	{
		const auto* Item=Items.Find(Change.Id);if (!Item) continue;
		if (!Change.Owner.IsEmpty()) Item=Items.Find(Change.Owner);
		if (Item && (Equal((*Item)->Kind,TEXT("struct")) || Equal((*Item)->Kind,TEXT("enum"))) && SafeAssetPath((*Item)->Target)) Packages.Add(*FPackageName::ObjectPathToPackageName((*Item)->Target));
	}
	const auto NextBase=Out.Merge.NextBase(Out.Base,Out.Theirs);
	if (Out.Merge.HasWrites() || !FVaRestOpenApiMerge::ItemsJson(NextBase).Equals(Out.Manifest[TEXT("items")])) Packages.Add(Api->GetOutermost()->GetFName());
	TArray<FName> Work=Packages.Array();auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	for (int32 Index=0;Index<Work.Num();++Index)
	{
		TArray<FName> Referencers;Registry.GetReferencers(Work[Index],Referencers,UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName Name:Referencers) if (!Packages.Contains(Name)) { Packages.Add(Name);Work.Add(Name); }
	}
	Out.AffectedPackages=Packages.Array();Out.AffectedPackages.Sort(FNameLexicalLess());Out.bValid=true;return Out;
}
