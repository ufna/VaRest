// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Engine/UserDefinedEnum.h"

namespace VaRest::OpenApi
{
bool FApply::EditEnum(UUserDefinedEnum* Enum,TConstArrayView<FVaRestOpenApiMergeItem*> Values)
{
	TArray<TPair<FName,int64>> Names;
	for (int32 Index=0;Index<Enum->NumEnums()-1;++Index) Names.Emplace(Enum->GetNameByIndex(Index),Enum->GetValueByIndex(Index));
	const int32 Before=Names.Num();
	for (auto* Value:Values) if (Value->bPresent)
	{
		if (Value->Target.Len()>=NAME_SIZE || Enum->GenerateFullEnumName(*Value->Target).Len()>=NAME_SIZE) { Value->bPresent=false;Issue(TEXT("OA-APPLY-ENUM"),TEXT("An enumerator identity exceeds the engine name limit."),Enum,EVaRestOpenApiSeverity::Error);continue; }
		if (Enum->GetIndexByNameString(Value->Target)!=INDEX_NONE) continue;
		if (Names.Num()>=254) { Value->bPresent=false;Issue(TEXT("OA-APPLY-ENUM"),TEXT("Retained and new enum values exceed 254. This value was not appended; review the schema's enum/string choice."),Enum,EVaRestOpenApiSeverity::Error);continue; }
		Names.Emplace(*Enum->GenerateFullEnumName(*Value->Target),Names.Num());
	}
	if (Names.Num()!=Before)
	{
		FEnumEditorUtils::FEnumEditorManager::Get().PreChange(Enum,FEnumEditorUtils::Changed);Enum->Modify();
		if (!Enum->SetEnums(Names,UEnum::ECppForm::Namespaced,UEnum::EUnderlyingType::uint8,EEnumFlags::None,UEnum::EAddMaxKeyIfMissing::Yes))
		{ Issue(TEXT("OA-APPLY-ENUM"),TEXT("The enum append was refused."),Enum,EVaRestOpenApiSeverity::Error);return false; }
		FEnumEditorUtils::EnsureAllDisplayNamesExist(Enum);FEnumEditorUtils::PostEditUndo(Enum);
	}
	for (auto* Value:Values) if (Value->bPresent)
	{
		const int32 Index=Enum->GetIndexByNameString(Value->Target);
		if (Index==INDEX_NONE || Index>=Enum->NumEnums()-1) return false;
		const FString Display=Value->Properties[TEXT("display")].AsString();
		if (!Equal(Enum->GetDisplayNameTextByIndex(Index).ToString(),Display) && !FEnumEditorUtils::SetEnumeratorDisplayName(Enum,Index,FText::FromString(Display)))
			Issue(TEXT("OA-APPLY-ENUM"),TEXT("The enum display text was refused; its numeric identity remains unchanged."),Enum);
	}
	for (int32 Index=0;Index<Before;++Index) if (Enum->GetNameByIndex(Index)!=Names[Index].Key || Enum->GetValueByIndex(Index)!=Names[Index].Value)
	{ Issue(TEXT("OA-APPLY-ENUM"),TEXT("The append changed an existing enum identity."),Enum,EVaRestOpenApiSeverity::Error);return false; }
	return true;
}
}
