// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimportPrivate.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/StructureEditorUtils.h"

namespace VaRest::OpenApi
{
void FReimport::ExistingTypes()
{
	for (auto* Item:All) if (Item->bPresent && Equal(Item->Kind,TEXT("enum")) && !Stage.NewTypeIds.Contains(Item->Id))
	{
		const auto* Type=Stage.Types.Find(Item->Id);auto* Enum=Type ? Cast<UUserDefinedEnum>(*Type) : nullptr;if (!Enum) continue;
		const auto* Values=Children.Find(Item->Id);
		if (Values && !Stage.EditEnum(Enum,*Values)) Stage.FailedTypes.Add(Item->Id);
		const FString Tooltip=Item->Properties[TEXT("tooltip")].AsString();
		if (!Equal(Enum->GetMetaData(TEXT("ToolTip")),Tooltip)) { Enum->Modify();Enum->SetMetaData(TEXT("ToolTip"),*Tooltip);Enum->MarkPackageDirty(); }
	}
	FStringSet Visited;
	const auto Edit=[&](FVaRestOpenApiMergeItem& Item)
	{
		if (!Item.bPresent || Stage.NewTypeIds.Contains(Item.Id) || Visited.Contains(Item.Id)) return;Visited.Add(Item.Id);
		const auto* Type=Stage.Types.Find(Item.Id);auto* Struct=Type ? Cast<UUserDefinedStruct>(*Type) : nullptr;if (!Struct) return;
		const auto* Fields=Children.Find(Item.Id);if (Fields && !Stage.EditStruct(Struct,*Fields,Live)) Stage.FailedTypes.Add(Item.Id);
		FStructureEditorUtils::ChangeTooltip(Struct,Item.Properties[TEXT("tooltip")].AsString());
	};
	for (const auto& Row:FullPlan.Structs) if (auto* Item=Items.Find(Row.Id)) Edit(**Item);
	for (auto* Item:All) if (Equal(Item->Kind,TEXT("struct"))) Edit(*Item); // retained removed schemas may have tooltip changes
}
FVaRestJson FReimport::Suppressions() const
{
	FStringSet Values;for (const auto& Id:Preview.Manifest[TEXT("suppressions")].Elements()) Values.Add(Id.AsString());
	for (const auto& Change:Preview.Merge.Changes)
	{
		if (Change.bApply && (Change.Change==EVaRestOpenApiChange::Restore || Change.Change==EVaRestOpenApiChange::Suppressed)) Values.Remove(Change.Id);
		if (Change.bSuppress || ((Change.Change==EVaRestOpenApiChange::Restore || Change.Change==EVaRestOpenApiChange::Added) && !Change.bApply && !Change.bDeferred)) Values.Add(Change.Id);
	}
	TArray<FString> Ordered=Values.Array();Ordered.Sort([](const FString& A,const FString& B) { return A.Compare(B,ESearchCase::CaseSensitive)<0; });
	auto Out=FVaRestJson::Array();for (const auto& Id:Ordered) Out.Add(Id);return Out;
}
FVaRestJson FReimport::Deferrals() const
{
	auto Rows=FVaRestJson::Array();
	for (const auto& Change:Preview.Merge.Changes) if (Change.bDeferred)
	{
		Rows.Add(FVaRestJson::Object().Set(TEXT("id"),Change.Id).Set(TEXT("property"),Change.Property).Set(TEXT("value"),Change.Theirs).Set(TEXT("breaking"),Change.bBreaking));
	}
	return Rows;
}
void FReimport::RecordActualBases(TArray<FVaRestOpenApiMergeItem>& Bases)
{
	auto Temporary=Preview.Manifest.Clone();Temporary.Set(TEXT("items"),FVaRestOpenApiMerge::ItemsJson(Preview.Merge.Items));
	const auto Actual=FVaRestOpenApiMerge::Capture(&Api,Temporary);TStringMap<const FVaRestOpenApiMergeItem*> ActualById;for (const auto& Item:Actual) ActualById.Add(Item.Id,&Item);
	for (auto& Change:Preview.Merge.Changes) if (Change.bApply)
	{
		const auto* Found=ActualById.Find(Change.Id);const bool Missing=!Found || !(*Found)->bPresent;
		if (Missing || (Stage.FailedTypes.Contains(Change.Owner) && (Change.Property.IsEmpty() || !(*Found)->Properties[Change.Property].Equals(Change.Theirs))))
		{
			Change.bApply=false;Change.bDeferred=true;
			Stage.Issue(TEXT("OA-REIMPORT-BREAKINGDEFERRED"),TEXT("An unapplied item remains pending after a native construction/edit failure: ")+Change.Id,&Api);
		}
	}
	Preview.Merge.Deferred=0;for (const auto& Change:Preview.Merge.Changes) Preview.Merge.Deferred+=Change.bDeferred;
	Bases=Preview.Merge.NextBase(Preview.Base,Preview.Theirs);
	TStringMap<FVaRestOpenApiMergeItem*> ById;for (auto& Item:Bases) ById.Add(Item.Id,&Item);
	for (const auto& ActualItem:Actual) if (auto* Base=ById.Find(ActualItem.Id)) (*Base)->Target=ActualItem.Target;
	for (const auto& Change:Preview.Merge.Changes) if (Change.bApply && !Change.bDeferred)
	{
		const auto* Found=ActualById.Find(Change.Id);auto* Base=ById.Find(Change.Id);if (!Found || !Base || !(*Found)->bPresent) continue;
		if (Change.Property.IsEmpty()) (*Base)->Properties=(*Found)->Properties.Clone();else (*Base)->Properties.Set(Change.Property,(*Found)->Properties[Change.Property]);
	}
}
}
