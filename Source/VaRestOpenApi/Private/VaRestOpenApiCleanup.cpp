// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiCleanup.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiSnapshots.h"
#include "VaRestOpenApiSource.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Editor.h"
#include "Engine/UserDefinedEnum.h"
#include "Framework/Notifications/NotificationManager.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "ObjectTools.h"
#include "ScopedTransaction.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

namespace VaRest::OpenApi
{
int32 CleanupRemoved(UVaRestApi* Api)
{
	check(IsInGameThread());if (!Api) return 0;TStrongObjectPtr<UVaRestApi> Root(Api);const auto State=DecodeState(Api->ImportState);if (!State.IsSet()) return 0;
	const auto Captured=FVaRestOpenApiMerge::Capture(Api,State.GetValue());TStringMap<const FVaRestOpenApiMergeItem*> Items;for (const auto& Item:Captured) Items.Add(Item.Id,&Item);
	TArray<TSharedPtr<FCleanupRow>> Rows;auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();TMap<FName,FString> ReferenceText;
	for (const auto& Item:Captured) if (Item.bRemovedUpstream && Item.bPresent)
	{
		auto Row=MakeShared<FCleanupRow>();Row->Item=Item;const auto* Owner=Item.Owner.IsEmpty() ? &Item : Items.FindRef(Item.Owner);
		const FName Package=Owner && SafeAssetPath(Owner->Target) ? FName(*FPackageName::ObjectPathToPackageName(Owner->Target)) : Api->GetOutermost()->GetFName();
		if (const auto* Text=ReferenceText.Find(Package)) Row->Referencers=*Text;
		else { TArray<FName> Refs;Registry.GetReferencers(Package,Refs,UE::AssetRegistry::EDependencyCategory::Package);Refs.Sort(FNameLexicalLess());TArray<FString> Names;for (FName Name:Refs) Names.Add(Name.ToString());Row->Referencers=Refs.IsEmpty() ? TEXT("No package referencers recorded") : TEXT("Referencers: ")+FString::Join(Names,TEXT(", "));ReferenceText.Add(Package,Row->Referencers); }
		Rows.Add(Row);
	}
	if (Rows.IsEmpty()) { FNotificationInfo Info(NSLOCTEXT("VaRestOpenApi","NothingToClean","No removed items to clean up."));Info.ExpireDuration=4;FSlateNotificationManager::Get().AddNotification(Info);return 0; }
	if (!ShowCleanupPicker(Rows)) return 0;
	FStringSet Requested;for (const auto& Row:Rows) if (Row->bSelected) Requested.Add(Row->Item.Id);if (Requested.IsEmpty()) return 0;
	const auto Current=DecodeState(Api->ImportState);if (!Current.IsSet() || !Current.GetValue().Equals(State.GetValue())) return 0;
	const auto Live=FVaRestOpenApiMerge::Capture(Api,State.GetValue());
	for (const auto& Row:Rows) if (Row->bSelected)
	{
		const auto* Item=Live.FindByPredicate([&](const auto& Value) { return Equal(Value.Id,Row->Item.Id); });
		if (!Item || !Item->bPresent || !Equal(Item->Target,Row->Item.Target) || !Item->Properties.Equals(Row->Item.Properties)) { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-REIMPORT-STALE");Issue.Message=TEXT("A declaration changed while cleanup was open. Refresh the cleanup list before deleting.");Report(MakeArrayView(&Issue,1));return 0; }
	}
	// Preflight the largest suppression update before destructive work.
	FStringSet Suppressed;for (const auto& Id:State.GetValue()[TEXT("suppressions")].Elements()) Suppressed.Add(Id.AsString());
	const auto SuppressionJson=[](const FStringSet& Set) { auto Out=FVaRestJson::Array();TArray<FString> Names=Set.Array();Names.Sort([](const auto& A,const auto& B) { return A.Compare(B,ESearchCase::CaseSensitive)<0; });for (const auto& Name:Names) Out.Add(Name);return Out; };
	auto Planned=State.GetValue().Clone();auto RequestedAll=Suppressed;for (const auto& Id:Requested) RequestedAll.Add(Id);
	bool Expand=true;while (Expand) { Expand=false;for (const auto& Item:Captured) if (!Item.Owner.IsEmpty() && RequestedAll.Contains(Item.Owner) && !RequestedAll.Contains(Item.Id)) { RequestedAll.Add(Item.Id);Expand=true; } }
	Planned.Set(TEXT("suppressions"),SuppressionJson(RequestedAll));FVaRestApiImportState Preflight;if (!EncodeState(Planned,Preflight)) return 0;
	const bool WasDirty=Api->GetOutermost()->IsDirty();int32 Count=0;TArray<UObject*> DeleteAssets;TStringMap<TWeakObjectPtr<UObject>> DeleteTargets;
	const auto OwnerType=[&](const FVaRestOpenApiMergeItem& Item)->UField* { const auto* Owner=Items.FindRef(Item.Owner);return Owner && SafeAssetPath(Owner->Target) ? LoadObject<UField>(nullptr,*Owner->Target,nullptr,LOAD_NoWarn|LOAD_Quiet) : nullptr; };
	{
		FScopedTransaction Changes(NSLOCTEXT("VaRestOpenApi","CleanupChanges","Clean up removed OpenAPI declarations"));Api->Modify();
		for (const auto& Row:Rows) if (Row->bSelected)
		{
			const auto& Item=Row->Item;bool Removed=false;
			if (Equal(Item.Kind,TEXT("field")))
			{
				auto* Struct=Cast<UUserDefinedStruct>(OwnerType(Item));FGuid Guid;if (Struct && FGuid::Parse(Item.Target,Guid)) Removed=FStructureEditorUtils::RemoveVariable(Struct,Guid);
				if (Removed) for (auto& Rules:Api->JsonTypes) if (Rules.Type==Struct) Rules.Fields.RemoveAll([&](const auto& Rule) { return Rule.Field==Guid; });
			}
			else if (Equal(Item.Kind,TEXT("enumValue")))
			{
				auto* Enum=Cast<UUserDefinedEnum>(OwnerType(Item));const int32 Index=Enum && Item.Target.Len()<NAME_SIZE ? Enum->GetIndexByNameString(Item.Target) : INDEX_NONE;
				if (Enum && Index!=INDEX_NONE && Index<Enum->NumEnums()-1) { const FName Name=Enum->GetNameByIndex(Index);FEnumEditorUtils::RemoveEnumeratorFromUserDefinedEnum(Enum,Index);Removed=Enum->GetIndexByNameString(Item.Target)==INDEX_NONE;if (Removed) for (auto& Rules:Api->JsonTypes) if (Rules.Type==Enum) Rules.Enums.RemoveAll([&](const auto& Rule) { return Rule.Enumerator==Name || Equal(Rule.Enumerator.ToString(),Item.Target); }); }
			}
			else if (Equal(Item.Kind,TEXT("endpoint"))) { FGuid Guid;if (FGuid::Parse(Item.Target,Guid)) Removed=Api->Endpoints.RemoveAll([&](const auto& Endpoint) { return Endpoint.Id==Guid; })>0; }
			else if (Equal(Item.Kind,TEXT("param")))
			{
				const auto* Owner=Items.FindRef(Item.Owner);FGuid Guid;if (Owner && FGuid::Parse(Owner->Target,Guid)) if (auto* Endpoint=Api->Endpoints.FindByPredicate([&](const auto& Value) { return Value.Id==Guid; })) Removed=Endpoint->Params.RemoveAll([&](const auto& Param) { return Param.PinName==FName(*Item.Target); })>0;
			}
			else if (Equal(Item.Kind,TEXT("environment"))) { const FName Name(*Item.Target);Removed=Api->Environments.RemoveAll([&](const auto& Environment) { return Environment.Name==Name; })>0;if (Removed && Api->DefaultEnvironment==Name) Api->DefaultEnvironment=Api->Environments.IsEmpty() ? NAME_None : Api->Environments[0].Name; }
			else if ((Equal(Item.Kind,TEXT("struct")) || Equal(Item.Kind,TEXT("enum"))) && SafeAssetPath(Item.Target)) { if (auto* Object=LoadObject<UObject>(nullptr,*Item.Target,nullptr,LOAD_NoWarn|LOAD_Quiet)) { DeleteAssets.Add(Object);DeleteTargets.Add(Item.Id,Object); } }
			if (Removed) { ++Count;Suppressed.Add(Item.Id); }
		}
		Changes.Cancel();
	}
	if (!DeleteAssets.IsEmpty())
	{
		const auto Rules=Api->JsonTypes;TArray<TWeakObjectPtr<UField>> RuleTypes;TArray<bool> NullTypes;for (const auto& Rule:Rules) { RuleTypes.Add(Rule.Type.Get());NullTypes.Add(!Rule.Type); }
		Api->JsonTypes.RemoveAll([&](const auto& Rule) { return DeleteAssets.Contains(Rule.Type.Get()); });
		ObjectTools::DeleteObjects(DeleteAssets,true); // the engine reviews asset references before actual deletion
		FStringSet DeletedIds;
		for (const auto& Target:DeleteTargets) if (!Target.Value.IsValid()) { DeletedIds.Add(Target.Key);Suppressed.Add(Target.Key);++Count; }
		Api->JsonTypes.Reset();for (int32 Index=0;Index<Rules.Num();++Index) if (NullTypes[Index] || RuleTypes[Index].IsValid()) { auto Rule=Rules[Index];Rule.Type=RuleTypes[Index].Get();Api->JsonTypes.Add(MoveTemp(Rule)); }
	}
	if (Count==0) { Api->GetOutermost()->SetDirtyFlag(WasDirty);return 0; }
	bool Added=true;while (Added) { Added=false;for (const auto& Item:Captured) if (!Item.Owner.IsEmpty() && Suppressed.Contains(Item.Owner) && !Suppressed.Contains(Item.Id)) { Suppressed.Add(Item.Id);Added=true; } }
	auto Updated=State.GetValue().Clone();Updated.Set(TEXT("suppressions"),SuppressionJson(Suppressed));auto Pending=FVaRestJson::Array();for (const auto& Row:Updated[TEXT("deferred")].Elements()) if (!Suppressed.Contains(Row[TEXT("id")].AsString())) Pending.Add(Row);Updated.Set(TEXT("deferred"),Pending);
	FVaRestApiImportState Encoded;if (EncodeState(Updated,Encoded)) Api->ImportState=MoveTemp(Encoded);else { FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-APPLY-STATE");Issue.Message=TEXT("Cleanup changed declarations but could not save suppressions; review before the next reimport.");Report(MakeArrayView(&Issue,1)); }
	Api->MarkPackageDirty();Api->NotifyEdited();if (GEditor) GEditor->ResetTransaction(NSLOCTEXT("VaRestOpenApi","CleanupUndo","OpenAPI cleanup removed generated declarations"));
	FNotificationInfo Info(FText::Format(NSLOCTEXT("VaRestOpenApi","CleanupComplete","Removed {0} obsolete declarations. Undo history was cleared."),FText::AsNumber(Count)));Info.ExpireDuration=6;FSlateNotificationManager::Get().AddNotification(Info);return Count;
}
}
