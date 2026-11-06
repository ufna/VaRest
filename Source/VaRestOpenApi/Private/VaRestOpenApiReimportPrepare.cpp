// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimportPrivate.h"
#include "Engine/UserDefinedEnum.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

namespace VaRest::OpenApi
{
bool FReimport::Current() const
{
	const auto State=DecodeState(Api.ImportState);if (!State.IsSet() || !State.GetValue().Equals(Preview.Manifest)) return false;
	const auto Now=FVaRestOpenApiMerge::Capture(&Api,Preview.Manifest);if (Now.Num()!=Preview.Mine.Num()) return false;
	for (int32 Index=0;Index<Now.Num();++Index)
	{
		const auto& A=Now[Index];const auto& B=Preview.Mine[Index];
		if (!Equal(A.Id,B.Id) || !Equal(A.Target,B.Target) || A.bPresent!=B.bPresent || !A.Properties.Equals(B.Properties) || A.ReservedTargets.Num()!=B.ReservedTargets.Num()) return false;
		for (int32 Name=0;Name<A.ReservedTargets.Num();++Name) if (!Equal(A.ReservedTargets[Name],B.ReservedTargets[Name])) return false;
	}
	return true;
}
void FReimport::Prepare()
{
	Options.Parent=Api.GetOuter();Options.ApiName=Api.GetFName();Options.bAddToSettings=false;
	Stage.Folder=Options.TypesFolder.IsEmpty() ? Api.TypesFolder : Options.TypesFolder;
	if (Stage.Folder.IsEmpty()) Stage.Folder=FPackageName::GetLongPackagePath(Api.GetOutermost()->GetName())/TEXT("Types");Stage.Folder.RemoveFromEnd(TEXT("/"));
	Stage.Result.Api=&Api;NewPlan.bValid=true;
	for (const auto& Item:Preview.Mine)
	{
		Live.Add(Item.Id,&Item);
		if (Item.bPresent && (Equal(Item.Kind,TEXT("struct")) || Equal(Item.Kind,TEXT("enum"))))
		{
			if (auto* Type=LoadObject<UField>(nullptr,*Item.Target,nullptr,LOAD_NoWarn|LOAD_Quiet)) { Stage.Types.Add(Item.Id,Type);ExistingRoots.Emplace(Type); }
		}
		if (Item.bPresent && Equal(Item.Kind,TEXT("endpoint"))) { FGuid Guid;if (FGuid::Parse(Item.Target,Guid)) Stage.Endpoints.Add(Item.Id,Guid); }
	}
	for (auto& Item:Preview.Merge.Items)
	{
		All.Add(&Item);Items.Add(Item.Id,&Item);if (!Item.Owner.IsEmpty()) Children.FindOrAdd(Item.Owner).Add(&Item);
		if (Equal(Item.Kind,TEXT("field"))) { FGuid Guid;if (FGuid::Parse(Item.Target,Guid)) Stage.Fields.Add(Item.Id,Guid); }
	}
	for (const auto& Row:FullPlan.Enums)
	{
		const auto* Item=Items.Find(Row.Id);if (!Item || !(*Item)->bPresent || Stage.Types.Contains(Row.Id)) continue;
		auto New=Row;New.Definition.Values.Reset();
		for (const auto& Value:Row.Definition.Values)
		{
			const auto* Entry=Items.Find(EnumValueId(Row.Id,Value.WireValue,Value.bUnset));if (!Entry || !(*Entry)->bPresent) continue;
			auto Copy=Value;Copy.InternalName=(*Entry)->Target;Copy.DisplayName=(*Entry)->Properties[TEXT("display")].AsString();Copy.WireValue=(*Entry)->Properties[TEXT("wire")].AsString();Copy.bUnset=(*Entry)->Properties[TEXT("unset")].AsBool();New.Definition.Values.Add(MoveTemp(Copy));
		}
		NewPlan.Enums.Add(MoveTemp(New));
	}
	for (const auto& Row:FullPlan.Structs)
	{
		const auto* Item=Items.Find(Row.Id);if (!Item || !(*Item)->bPresent || Stage.Types.Contains(Row.Id)) continue;
		auto New=Row;New.Tooltip=(*Item)->Properties[TEXT("tooltip")].AsString();New.Fields.Reset();
		for (const auto& Field:Row.Fields)
		{
			const auto* Entry=Items.Find(Field.Id);if (!Entry || !(*Entry)->bPresent) continue;const auto& Properties=(*Entry)->Properties;
			auto Copy=Field;Copy.FriendlyName=Properties[TEXT("name")].AsString();Copy.WireName=Properties[TEXT("wire")].AsString();Copy.Tooltip=Properties[TEXT("tooltip")].AsString();Copy.DefaultText=Properties[TEXT("defaultText")].AsString();
			const auto Wanted=PlannedType(Properties[TEXT("type")]);if (Wanted.IsSet()) Copy.Type=Wanted.GetValue();
			Copy.Default=Copy.Type.Kind==EVaRestOpenApiValueKind::String ? FVaRestJson::From(Copy.DefaultText) : FVaRestJson();Copy.bBase64=Properties[TEXT("base64")].AsBool();Copy.bNullable=Properties[TEXT("nullable")].AsBool();
			const auto Presence=NativeEnum(Properties[TEXT("presence")],EVaRestJsonPresence::Always);
			switch (Presence)
			{
			case EVaRestJsonPresence::OmitWhenDefault:Copy.Presence=EVaRestOpenApiPresence::OmitWhenDefault;break;
			case EVaRestJsonPresence::NullWhenDefault:Copy.Presence=EVaRestOpenApiPresence::NullWhenDefault;break;
			case EVaRestJsonPresence::DecodeOnly:Copy.Presence=EVaRestOpenApiPresence::DecodeOnly;break;
			case EVaRestJsonPresence::Ignore:Copy.Presence=EVaRestOpenApiPresence::Ignore;break;
			default:Copy.Presence=EVaRestOpenApiPresence::Always;break;
			}
			New.Fields.Add(MoveTemp(Copy));
		}
		NewPlan.Structs.Add(MoveTemp(New));
	}
}
void FReimport::CleanupFailed()
{
	for (const auto& Id:Stage.FailedTypes) if (const auto* Type=Stage.Types.Find(Id))
	{
		if (Stage.NewTypeIds.Contains(Id))
		{
			for (auto& Object:Stage.Created) if (Object.Get()==*Type) { Object->ClearFlags(RF_Public|RF_Standalone);Object->GetOutermost()->SetDirtyFlag(false);Object->MarkAsGarbage();Object.Reset();break; }
			Stage.Types.Remove(Id);
		}
	}
	for (auto* Item:All) if (Equal(Item->Kind,TEXT("struct")) || Equal(Item->Kind,TEXT("enum")))
	{
		const auto* Type=Stage.Types.Find(Item->Id);if (Type) Item->Target=(*Type)->GetPathName();else Item->bPresent=false;
	}
	for (auto* Item:All) if (Equal(Item->Kind,TEXT("field"))) if (const auto* Guid=Stage.Fields.Find(Item->Id)) Item->Target=Guid->ToString();
}
}
