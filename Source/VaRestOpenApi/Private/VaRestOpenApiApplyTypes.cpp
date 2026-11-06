// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "EdGraphSchema_K2.h"
#include "Engine/UserDefinedEnum.h"
#include "Kismet2/EnumEditorUtils.h"
#include "UObject/NoExportTypes.h"

namespace VaRest::OpenApi
{
void FApply::Enums()
{
	for (const auto& Row:Plan.Enums)
	{
		if (Types.Contains(Row.Id)) continue;
		if (Cancelled()) return;FName Name;UPackage* Package=TypePackage(Row.AssetName,Name);if (!Package) { FailedTypes.Add(Row.Id);continue; }
		auto* Enum=Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(Package,Name,Options.Flags|RF_Public|RF_Standalone|RF_Transactional));
		if (!Enum) { FailedTypes.Add(Row.Id);Issue(TEXT("OA-APPLY-ENUM"),TEXT("Could not create enum ")+Row.AssetName,nullptr,EVaRestOpenApiSeverity::Error);continue; }
		Keep(Enum);Types.Add(Row.Id,Enum);NewTypeIds.Add(Row.Id);TArray<TPair<FName,int64>> Names;
		bool SafeNames=true;
		for (int32 Index=0;Index<Row.Definition.Values.Num();++Index) { const FString FullName=Enum->GenerateFullEnumName(*Row.Definition.Values[Index].InternalName);if (FullName.Len()>=NAME_SIZE) { SafeNames=false;break; }Names.Emplace(*FullName,Index); }
		bool Valid=SafeNames && !Names.IsEmpty() && Names.Num()<=254 && Enum->SetEnums(Names,UEnum::ECppForm::Namespaced,UEnum::EUnderlyingType::uint8,EEnumFlags::None,UEnum::EAddMaxKeyIfMissing::Yes);
		if (Valid)
		{
			FEnumEditorUtils::EnsureAllDisplayNamesExist(Enum);
			for (int32 Index=0;Index<Row.Definition.Values.Num();++Index)
			{
				const auto& Value=Row.Definition.Values[Index];
				Valid=FEnumEditorUtils::SetEnumeratorDisplayName(Enum,Index,FText::FromString(Value.DisplayName)) && Valid;
				Valid=Equal(Enum->GetNameStringByIndex(Index),Value.InternalName) && Enum->GetValueByIndex(Index)==Index && Valid;
			}
		}
		if (!Valid) { FailedTypes.Add(Row.Id);Issue(TEXT("OA-APPLY-ENUM"),TEXT("The enum failed native identity/display validation; uses fall back to String."),Enum,EVaRestOpenApiSeverity::Error);continue; }
		Enum->SetMetaData(TEXT("ToolTip"),*Row.Tooltip);Types.Add(Row.Id,Enum);
	}
}
FVaRestTypeRef FApply::Type(const FVaRestOpenApiPlannedType& Value) const
{
	FVaRestTypeRef Out;
	switch (Value.Kind)
	{
	case EVaRestOpenApiValueKind::None:Out.Kind=EVaRestTypeKind::None;break;
	case EVaRestOpenApiValueKind::Json:Out.Kind=EVaRestTypeKind::Json;break;
	case EVaRestOpenApiValueKind::Struct:Out.Kind=EVaRestTypeKind::Struct;break;
	case EVaRestOpenApiValueKind::Enum:Out.Kind=EVaRestTypeKind::Enum;break;
	case EVaRestOpenApiValueKind::String:Out.Kind=EVaRestTypeKind::String;break;
	case EVaRestOpenApiValueKind::Bytes:Out.Kind=EVaRestTypeKind::Bytes;break;
	case EVaRestOpenApiValueKind::Bool:Out.Kind=EVaRestTypeKind::Bool;break;
	case EVaRestOpenApiValueKind::Int:Out.Kind=EVaRestTypeKind::Int;break;
	case EVaRestOpenApiValueKind::Int64:Out.Kind=EVaRestTypeKind::Int64;break;
	case EVaRestOpenApiValueKind::Double:Out.Kind=EVaRestTypeKind::Double;break;
	case EVaRestOpenApiValueKind::DateTime:Out.Kind=EVaRestTypeKind::DateTime;break;
	}
	if (Value.Kind==EVaRestOpenApiValueKind::Struct || Value.Kind==EVaRestOpenApiValueKind::Enum)
	{
		const auto* Found=Types.Find(Value.DefinitionId);
		if (Found && !FailedTypes.Contains(Value.DefinitionId)) Out.Type=*Found;
		else Out.Kind=Value.Kind==EVaRestOpenApiValueKind::Enum ? EVaRestTypeKind::String : EVaRestTypeKind::Json;
	}
	switch (Value.Container)
	{
	case EVaRestOpenApiContainer::None:Out.Container=EVaRestTypeContainer::None;break;
	case EVaRestOpenApiContainer::Array:Out.Container=EVaRestTypeContainer::Array;break;
	case EVaRestOpenApiContainer::Map:Out.Container=EVaRestTypeContainer::Map;break;
	}
	Out.bJsonValue=Value.bJsonValue && (Out.Kind==EVaRestTypeKind::String || Out.Kind==EVaRestTypeKind::Bytes) && Out.Container==EVaRestTypeContainer::None;return Out;
}
FEdGraphPinType FApply::Pin(const FVaRestOpenApiPlannedType& Value) const
{
	const FVaRestTypeRef Ref=Type(Value);FEdGraphPinType Out;
	switch (Ref.Kind)
	{
	case EVaRestTypeKind::Enum:Out.PinCategory=UEdGraphSchema_K2::PC_Byte;Out.PinSubCategoryObject=Ref.Type;break;
	case EVaRestTypeKind::Struct:Out.PinCategory=UEdGraphSchema_K2::PC_Struct;Out.PinSubCategoryObject=Ref.Type;break;
	case EVaRestTypeKind::Bool:Out.PinCategory=UEdGraphSchema_K2::PC_Boolean;break;
	case EVaRestTypeKind::Int:Out.PinCategory=UEdGraphSchema_K2::PC_Int;break;
	case EVaRestTypeKind::Int64:Out.PinCategory=UEdGraphSchema_K2::PC_Int64;break;
	case EVaRestTypeKind::Double:Out.PinCategory=UEdGraphSchema_K2::PC_Real;Out.PinSubCategory=UEdGraphSchema_K2::PC_Double;break;
	case EVaRestTypeKind::String:Out.PinCategory=UEdGraphSchema_K2::PC_String;break;
	case EVaRestTypeKind::Bytes:Out.PinCategory=UEdGraphSchema_K2::PC_Byte;Out.ContainerType=EPinContainerType::Array;break;
	case EVaRestTypeKind::DateTime:Out.PinCategory=UEdGraphSchema_K2::PC_Struct;Out.PinSubCategoryObject=TBaseStructure<FDateTime>::Get();break;
	default:Out.PinCategory=UEdGraphSchema_K2::PC_Struct;Out.PinSubCategoryObject=FVaRestJson::StaticStruct();break;
	}
	if (Ref.Container==EVaRestTypeContainer::Array) Out.ContainerType=EPinContainerType::Array;
	else if (Ref.Container==EVaRestTypeContainer::Map)
	{
		Out.PinValueType.TerminalCategory=Out.PinCategory;Out.PinValueType.TerminalSubCategory=Out.PinSubCategory;Out.PinValueType.TerminalSubCategoryObject=Out.PinSubCategoryObject;
		Out.PinCategory=UEdGraphSchema_K2::PC_String;Out.PinSubCategory=NAME_None;Out.PinSubCategoryObject=nullptr;Out.ContainerType=EPinContainerType::Map;
	}
	return Out;
}
FString FApply::Default(const FVaRestOpenApiPlannedField& Field,const FEdGraphPinType& PinType)
{
	if (Field.bDefaultInTooltipOnly) return {};
	FString Text=Field.DefaultText;
	// Kismet string defaults are literal FString contents, not JSON or property-export text.
	if (Field.Type.Kind==EVaRestOpenApiValueKind::String && Field.Default.IsString()) Text=Field.Default.AsString();
	if (Field.Type.Kind==EVaRestOpenApiValueKind::Enum)
	{
		const auto* Enum=Cast<UEnum>(PinType.PinSubCategoryObject.Get());
		if (Enum)
		{
			if (Text.Len()>=NAME_SIZE) { Issue(TEXT("OA-APPLY-DEFAULT"),TEXT("An enum default exceeds the engine name limit."));return {}; }
			const int32 Index=Enum->GetIndexByNameString(Text,EGetByNameFlags::None);
			if (Index!=INDEX_NONE) Text=Enum->GetNameByIndex(Index).ToString();
		}
		else if (Field.Default.IsString()) Text=Field.Default.AsString();else Text.Reset();
	}
	if (Text.IsEmpty()) return Text;
	FString Error;
	bool EmbeddedNull=false;for (const TCHAR Char:Text) EmbeddedNull=EmbeddedNull || Char==0;
	if (EmbeddedNull || !GetDefault<UEdGraphSchema_K2>()->DefaultValueSimpleValidation(PinType,NAME_None,Text,nullptr,FText::GetEmpty(),&Error))
	{
		Issue(TEXT("OA-APPLY-DEFAULT"),TEXT("Dropped an invalid default for ")+Field.FriendlyName+TEXT(": ")+Error);return {};
	}
	return Text;
}
}
