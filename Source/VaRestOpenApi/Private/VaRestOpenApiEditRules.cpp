// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiReimportValues.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/UnrealType.h"

namespace VaRest::OpenApi
{
void FApply::EditRules(TConstArrayView<FVaRestOpenApiMergeItem*> Items)
{
	for (auto* Item:Items) if (Item->bPresent && (Equal(Item->Kind,TEXT("field")) || Equal(Item->Kind,TEXT("enumValue"))))
	{
		const auto* Owner=Types.Find(Item->Owner);if (!Owner || FailedTypes.Contains(Item->Owner)) continue;
		auto* Rules=Result.Api->JsonTypes.FindByPredicate([&](const auto& Row) { return Row.Type==*Owner; });
		if (!Rules) { FVaRestApiJsonTypeRules Row;Row.Type=*Owner;Rules=&Result.Api->JsonTypes.Add_GetRef(MoveTemp(Row)); }
		if (Equal(Item->Kind,TEXT("field")))
		{
			const auto* Struct=Cast<UUserDefinedStruct>(*Owner);FGuid Guid;FGuid::Parse(Item->Target,Guid);const FProperty* Property=Struct ? FStructureEditorUtils::GetPropertyByGuid(Struct,Guid) : nullptr;if (!Property) continue;
			auto* Rule=Rules->Fields.FindByPredicate([&](const auto& Row) { return Row.Field==Guid; });if (!Rule) { FVaRestJsonFieldRule Row;Row.Field=Guid;Rule=&Rules->Fields.Add_GetRef(MoveTemp(Row)); }
			Rule->Json=Item->Properties[TEXT("wire")].AsString();Rule->Presence=NativeEnum(Item->Properties[TEXT("presence")],Rule->Presence);Rule->bNullable=Item->Properties[TEXT("nullable")].AsBool();
			const auto* Array=CastField<FArrayProperty>(Property);const auto* Byte=Array ? CastField<FByteProperty>(Array->Inner) : nullptr;Rule->bBase64=Item->Properties[TEXT("base64")].AsBool() && Byte && !Byte->Enum;
		}
		else
		{
			const auto* Enum=Cast<UEnum>(*Owner);const int32 Index=Enum ? Enum->GetIndexByNameString(Item->Target) : INDEX_NONE;if (Index==INDEX_NONE || Index==Enum->NumEnums()-1) continue;
			const FName Name=Enum->GetNameByIndex(Index);auto* Rule=Rules->Enums.FindByPredicate([&](const auto& Row) { return Row.Enumerator==Name || Equal(Row.Enumerator.ToString(),Enum->GetNameStringByIndex(Index)); });
			if (!Rule) { FVaRestJsonEnumRule Row;Row.Enumerator=Name;Rule=&Rules->Enums.Add_GetRef(MoveTemp(Row)); }
			Rule->Enumerator=Name;Rule->Json=Item->Properties[TEXT("wire")].AsString();Rule->bUnset=Item->Properties[TEXT("unset")].AsBool();
		}
	}
}
}
