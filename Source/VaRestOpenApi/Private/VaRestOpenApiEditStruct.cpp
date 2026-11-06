// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiReimportValues.h"
#include "VaRestOpenApiNames.h"
#include "EdGraphSchema_K2.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/StructureEditorUtils.h"

namespace VaRest::OpenApi
{
bool FApply::EditStruct(UUserDefinedStruct* Struct,TConstArrayView<FVaRestOpenApiMergeItem*> Values,const TStringMap<const FVaRestOpenApiMergeItem*>& Live)
{
	const auto Before=FStructureEditorUtils::GetVarDesc(Struct);auto Members=Before;bool Compile=false,Tooltips=false;
	for (auto* Item:Values) if (Item->bPresent)
	{
		const auto* Old=Live.Find(Item->Id);const auto* Current=Old && (*Old)->bPresent ? *Old : nullptr;
		FGuid Guid;FGuid::Parse(Item->Target,Guid);
		int32 Index=Members.IndexOfByPredicate([&](const auto& Member) { return Member.VarGuid==Guid; });const bool New=Index==INDEX_NONE;
		if (!New && Current && Current->Properties.Equals(Item->Properties)) { Fields.Add(Item->Id,Guid);continue; }
		FStructVariableDescription Member=New ? FStructVariableDescription() : Members[Index];
		if (!Guid.IsValid()) Guid=FGuid::NewGuid();Member.VarGuid=Guid;
		const bool TypeChanged=New || !Current || !Current->Properties[TEXT("type")].Equals(Item->Properties[TEXT("type")]);
		if (TypeChanged)
		{
			const auto Wanted=PlannedType(Item->Properties[TEXT("type")]);FEdGraphPinType PinType=Wanted.IsSet() ? Pin(Wanted.GetValue()) : FEdGraphPinType();FString Error;
			if (!Wanted.IsSet() || !FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct,PinType,&Error))
			{
				Issue(TEXT("OA-APPLY-TYPE"),TEXT("The changed member uses Json because its desired type was refused: ")+Error,Struct);
				PinType=FEdGraphPinType();PinType.PinCategory=UEdGraphSchema_K2::PC_Struct;PinType.PinSubCategoryObject=FVaRestJson::StaticStruct();
			}
			Member.SetPinType(PinType);Member.DefaultValue.Reset();Compile=true;
		}
		FString Friendly=Item->Properties[TEXT("name")].AsString();
		if (!FName::IsValidXName(Friendly,INVALID_OBJECTNAME_CHARACTERS) || Friendly.IsEmpty()) Friendly=FVaRestOpenApiNameAllocator::SanitizeField(Friendly);
		const FString Base=Friendly;int32 Suffix=2;
		while (Members.ContainsByPredicate([&](const auto& Other) { return Other.VarGuid!=Guid && FName(*Other.FriendlyName)==FName(*Friendly); }))
		{
			int32 Count=FMath::Min(Base.Len(),88);if (Count>0 && Base[Count-1]>=0xd800 && Base[Count-1]<=0xdbff) --Count;
			Friendly=Base.Left(Count)+TEXT("_")+LexToString(Suffix++);
		}
		if (New || TypeChanged || !Equal(Member.FriendlyName,Friendly))
		{
			Member.FriendlyName=Friendly;auto* Data=CastChecked<UUserDefinedStructEditorData>(Struct->EditorData);
			Member.VarName=*FString::Printf(TEXT("%s_%u_%s"),*Friendly,Data->GenerateUniqueNameIdForMemberVariable(),*Guid.ToString(EGuidFormats::Digits));Compile=true;
		}
		if (New || TypeChanged || !Current || !Current->Properties[TEXT("defaultText")].Equals(Item->Properties[TEXT("defaultText")]))
		{
			FVaRestOpenApiPlannedField Field;const auto Wanted=PlannedType(Item->Properties[TEXT("type")]);if (Wanted.IsSet()) Field.Type=Wanted.GetValue();
			Field.FriendlyName=Friendly;Field.DefaultText=Item->Properties[TEXT("defaultText")].AsString();Member.DefaultValue=Default(Field,Member.ToPinType());Compile=true;
		}
		const FString Tooltip=Item->Properties[TEXT("tooltip")].AsString();Tooltips=Tooltips || !Equal(Member.ToolTip,Tooltip);Member.ToolTip=Tooltip;
		if (New) Members.Add(Member);else Members[Index]=Member;Item->Target=Guid.ToString();Fields.Add(Item->Id,Guid);
	}
	if (!Compile)
	{
		if (Tooltips) for (const auto& Member:Members) FStructureEditorUtils::ChangeVariableTooltip(Struct,Member.VarGuid,Member.ToolTip);return true;
	}
	FStructureEditorUtils::ModifyStructData(Struct);const auto* Slow=IConsoleManager::Get().FindConsoleVariable(TEXT("VaRest.OpenApi.SlowStructBuild"));
	if (!Slow || Slow->GetInt()==0)
	{
		FStructureEditorUtils::GetVarDesc(Struct)=Members;FStructureEditorUtils::OnStructureChanged(Struct);if (CheckStruct(Struct,Members)) return true;
	}
	Issue(TEXT("OA-APPLY-SLOWPATH"),TEXT("Reapplying changed members through the public editor operations while preserving their GUIDs."),Struct);
	FStructureEditorUtils::GetVarDesc(Struct)=Before;FStructureEditorUtils::OnStructureChanged(Struct);bool Good=true;
	for (auto& Wanted:Members)
	{
		auto* Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Wanted.VarGuid);
		if (!Actual)
		{
			if (!FStructureEditorUtils::AddVariable(Struct,Wanted.ToPinType())) { Good=false;break; }
			auto& Added=FStructureEditorUtils::GetVarDesc(Struct).Last();
			// Retain a restored field's saved GUID. Refuse if the engine's own public name parser no longer qualifies this format.
			const FString AddedName=Added.VarName.ToString();
			if (FStructureEditorUtils::GetGuidFromPropertyName(Added.VarName)!=Added.VarGuid || !AddedName.EndsWith(Added.VarGuid.ToString(EGuidFormats::Digits),ESearchCase::CaseSensitive)) { Good=false;break; }
			Added.VarGuid=Wanted.VarGuid;Added.VarName=*(AddedName.LeftChop(32)+Wanted.VarGuid.ToString(EGuidFormats::Digits));FStructureEditorUtils::OnStructureChanged(Struct);
			Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Wanted.VarGuid);
		}
		if (!Actual) { Good=false;break; }
		if (Actual->ToPinType()!=Wanted.ToPinType() && !FStructureEditorUtils::ChangeVariableType(Struct,Wanted.VarGuid,Wanted.ToPinType())) { Good=false;break; }
		Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Wanted.VarGuid);
		if (!Actual) { Good=false;break; }
		if (!Equal(Actual->FriendlyName,Wanted.FriendlyName) && !FStructureEditorUtils::RenameVariable(Struct,Wanted.VarGuid,Wanted.FriendlyName)) { Good=false;break; }
		Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Wanted.VarGuid);
		if (!Actual) { Good=false;break; }
		if (!Equal(Actual->DefaultValue,Wanted.DefaultValue) && !FStructureEditorUtils::ChangeVariableDefaultValue(Struct,Wanted.VarGuid,Wanted.DefaultValue)) { Good=false;break; }
		FStructureEditorUtils::ChangeVariableTooltip(Struct,Wanted.VarGuid,Wanted.ToolTip);
		Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Wanted.VarGuid);if (!Actual) { Good=false;break; }Wanted=*Actual;
	}
	if (Good && CheckStruct(Struct,Members)) return true;
	FStructureEditorUtils::GetVarDesc(Struct)=Before;FStructureEditorUtils::OnStructureChanged(Struct);
	Issue(TEXT("OA-APPLY-COMPILE"),TEXT("The changed struct failed both paths; its prior member descriptions were restored."),Struct,EVaRestOpenApiSeverity::Error);return false;
}
}
