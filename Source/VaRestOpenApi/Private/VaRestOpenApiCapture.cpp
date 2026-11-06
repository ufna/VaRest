// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiSnapshots.h"
#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/PackageName.h"
#include "UObject/NoExportTypes.h"
#include "UObject/StrongObjectPtr.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace
{
using namespace VaRest::OpenApi;
FVaRestJson PinJson(FEdGraphPinType Pin,const TStringMap<FString>& Definitions)
{
	FVaRestTypeRef Type;
	if (Pin.ContainerType==EPinContainerType::Map)
	{
		Type.Container=EVaRestTypeContainer::Map;Pin.PinCategory=Pin.PinValueType.TerminalCategory;Pin.PinSubCategory=Pin.PinValueType.TerminalSubCategory;Pin.PinSubCategoryObject=Pin.PinValueType.TerminalSubCategoryObject;
	}
	else if (Pin.ContainerType==EPinContainerType::Array) Type.Container=EVaRestTypeContainer::Array;
	if (Pin.PinCategory==UEdGraphSchema_K2::PC_Boolean) Type.Kind=EVaRestTypeKind::Bool;
	else if (Pin.PinCategory==UEdGraphSchema_K2::PC_Int) Type.Kind=EVaRestTypeKind::Int;
	else if (Pin.PinCategory==UEdGraphSchema_K2::PC_Int64) Type.Kind=EVaRestTypeKind::Int64;
	else if (Pin.PinCategory==UEdGraphSchema_K2::PC_Real) Type.Kind=EVaRestTypeKind::Double;
	else if (Pin.PinCategory==UEdGraphSchema_K2::PC_String) Type.Kind=EVaRestTypeKind::String;
	else if (Pin.PinCategory==UEdGraphSchema_K2::PC_Byte)
	{
		if (auto* Enum=Cast<UEnum>(Pin.PinSubCategoryObject.Get())) { Type.Kind=EVaRestTypeKind::Enum;Type.Type=Enum; }
		else if (Type.Container==EVaRestTypeContainer::Array) { Type.Kind=EVaRestTypeKind::Bytes;Type.Container=EVaRestTypeContainer::None; }
		else return FVaRestJson::Object().Set(TEXT("kind"),TEXT("user:Byte"));
	}
	else if (Pin.PinCategory==UEdGraphSchema_K2::PC_Struct)
	{
		if (Pin.PinSubCategoryObject==FVaRestJson::StaticStruct()) Type.Kind=EVaRestTypeKind::Json;
		else if (Pin.PinSubCategoryObject==TBaseStructure<FDateTime>::Get()) Type.Kind=EVaRestTypeKind::DateTime;
		else { Type.Kind=EVaRestTypeKind::Struct;Type.Type=Pin.PinSubCategoryObject.Get(); }
	}
	else return FVaRestJson::Object().Set(TEXT("kind"),TEXT("user:")+Pin.PinCategory.ToString());
	return SnapshotType(Type,Definitions);
}
}
TArray<FVaRestOpenApiMergeItem> FVaRestOpenApiMerge::Capture(UVaRestApi* Api,const FVaRestJson& Manifest)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());auto Items=Base(Manifest);if (!Api) { for (auto& Item:Items) Item.bPresent=false;return Items; }
	TStringMap<UField*> Definitions;TStringMap<FString> Reverse;TArray<TStrongObjectPtr<UObject>> Loaded;
	TStringMap<const FVaRestApiEndpoint*> Endpoints;
	for (auto& Item:Items)
	{
		if (Equal(Item.Kind,TEXT("enum")) || Equal(Item.Kind,TEXT("struct")))
		{
			UField* Type=nullptr;
			if (SafeAssetPath(Item.Target)) Type=LoadObject<UField>(nullptr,*Item.Target,nullptr,LOAD_NoWarn|LOAD_Quiet);
			const bool Valid=Equal(Item.Kind,TEXT("enum")) ? Type && Type->IsA<UUserDefinedEnum>() : Type && Type->IsA<UUserDefinedStruct>();
			if (Valid) { Definitions.Add(Item.Id,Type);Reverse.Add(Type->GetPathName(),Item.Id);Loaded.Emplace(Type); }else Item.bPresent=false;
		}
		else if (Equal(Item.Kind,TEXT("endpoint"))) { FGuid Guid;if (FGuid::Parse(Item.Target,Guid)) if (const auto* Endpoint=Api->FindEndpoint(Guid)) Endpoints.Add(Item.Id,Endpoint); }
	}
	for (auto& Item:Items)
	{
		if (Equal(Item.Kind,TEXT("enum")) || Equal(Item.Kind,TEXT("struct")))
		{
			const auto* Found=Definitions.Find(Item.Id);if (!Found) { Item.bPresent=false;continue; }
			Item.Target=(*Found)->GetPathName();
			if (const auto* Enum=Cast<UEnum>(*Found)) for (int32 Index=0;Index<Enum->NumEnums()-1;++Index) Item.ReservedTargets.Add(Enum->GetNameStringByIndex(Index));
			const FString Tooltip=Equal(Item.Kind,TEXT("struct")) ? FStructureEditorUtils::GetTooltip(CastChecked<UUserDefinedStruct>(*Found)) : (*Found)->GetMetaData(TEXT("ToolTip"));
			Item.Properties=FVaRestJson::Object().Set(TEXT("tooltip"),Tooltip);
		}
		else if (Equal(Item.Kind,TEXT("field")))
		{
			const auto* Found=Definitions.Find(Item.Owner);FGuid Guid;const auto* Struct=Found ? Cast<UUserDefinedStruct>(*Found) : nullptr;
			const auto* Desc=Struct && FGuid::Parse(Item.Target,Guid) ? FStructureEditorUtils::GetVarDescByGuid(Struct,Guid) : nullptr;
			if (!Desc) { Item.bPresent=false;continue; }
			const auto* Rules=Api->JsonTypes.FindByPredicate([&](const auto& Row) { return Row.Type==Struct; });const auto* Rule=Rules ? Rules->Fields.FindByPredicate([&](const auto& Row) { return Row.Field==Guid; }) : nullptr;
			FString Default=Desc->DefaultValue;const auto Pin=Desc->ToPinType();
			if (const auto* Enum=Cast<UEnum>(Pin.PinSubCategoryObject.Get()); Enum && Pin.ContainerType==EPinContainerType::None)
			{ const int32 Index=Enum->GetIndexByNameString(Default);if (Index!=INDEX_NONE) Default=Enum->GetNameStringByIndex(Index); }
			Item.Properties=FVaRestJson::Object().Set(TEXT("wire"),Rule ? Rule->Json : Desc->FriendlyName).Set(TEXT("name"),Desc->FriendlyName).Set(TEXT("type"),PinJson(Pin,Reverse))
				.Set(TEXT("presence"),StaticEnum<EVaRestJsonPresence>()->GetNameStringByValue(static_cast<int64>(Rule ? Rule->Presence : EVaRestJsonPresence::Always)))
				.Set(TEXT("nullable"),Rule && Rule->bNullable).Set(TEXT("base64"),Rule && Rule->bBase64).Set(TEXT("tooltip"),Desc->ToolTip).Set(TEXT("defaultText"),Default);
		}
		else if (Equal(Item.Kind,TEXT("enumValue")))
		{
			const auto* Found=Definitions.Find(Item.Owner);const auto* Enum=Found ? Cast<UEnum>(*Found) : nullptr;const int32 Index=Enum && Item.Target.Len()<NAME_SIZE ? Enum->GetIndexByNameString(Item.Target) : INDEX_NONE;
			if (!Enum || Index==INDEX_NONE || Index==Enum->NumEnums()-1) { Item.bPresent=false;continue; }
			const auto* Rules=Api->JsonTypes.FindByPredicate([&](const auto& Row) { return Row.Type==Enum; });
			const auto* Rule=Rules ? Rules->Enums.FindByPredicate([&](const auto& Row) { return Row.Enumerator==Enum->GetNameByIndex(Index) || Equal(Row.Enumerator.ToString(),Enum->GetNameStringByIndex(Index)); }) : nullptr;
			Item.Properties=FVaRestJson::Object().Set(TEXT("wire"),Rule ? Rule->Json : Enum->GetAuthoredNameStringByIndex(Index)).Set(TEXT("display"),Enum->GetDisplayNameTextByIndex(Index).ToString()).Set(TEXT("unset"),Rule && Rule->bUnset);
		}
		else if (Equal(Item.Kind,TEXT("endpoint")))
		{
			const auto* Found=Endpoints.Find(Item.Id);if (!Found) { Item.bPresent=false;continue; }Item.Properties=SnapshotEndpoint(**Found,Reverse);
		}
		else if (Equal(Item.Kind,TEXT("param")))
		{
			const auto* Found=Endpoints.Find(Item.Owner);const FVaRestEndpointParam* Param=nullptr;
			if (Found)
			{
				if (Item.Target.Len()<NAME_SIZE) Param=(*Found)->Params.FindByPredicate([&](const auto& Row) { return Row.PinName==FName(*Item.Target); });
				if (!Param) Param=(*Found)->Params.FindByPredicate([&](const auto& Row) { const FString In=Row.In==EVaRestEndpointParamLocation::Path ? TEXT("path") : Row.In==EVaRestEndpointParamLocation::Header ? TEXT("header") : TEXT("query");return Equal(Row.Name,Item.Properties[TEXT("wire")].AsString()) && Equal(In,Item.Properties[TEXT("in")].AsString()); });
			}
			if (!Param) { Item.bPresent=false;continue; }Item.Target=Param->PinName.ToString();Item.Properties=SnapshotParameter(*Param,Reverse);
		}
		else if (Equal(Item.Kind,TEXT("environment")))
		{
			const auto* Environment=Item.Target.Len()<NAME_SIZE ? Api->FindEnvironment(*Item.Target) : nullptr;
			if (!Environment) Environment=Api->Environments.FindByPredicate([&](const auto& Row) { return Equal(Row.BaseUrl,Item.Properties[TEXT("url")].AsString()); });
			if (!Environment) { Item.bPresent=false;continue; }Item.Target=Environment->Name.ToString();Item.Properties=FVaRestJson::Object().Set(TEXT("name"),Environment->Name.ToString()).Set(TEXT("url"),Environment->BaseUrl);
		}
		else if (Equal(Item.Kind,TEXT("auth"))) Item.Properties=SnapshotAuth(Api->Auth);
		else if (Equal(Item.Kind,TEXT("envelope"))) Item.Properties=SnapshotEnvelope(Api->Envelope);
		else if (Equal(Item.Kind,TEXT("defaults"))) Item.Properties=FVaRestJson::Object().Set(TEXT("defaultEnvironment"),Api->DefaultEnvironment.ToString());
	}
	return Items;
}
