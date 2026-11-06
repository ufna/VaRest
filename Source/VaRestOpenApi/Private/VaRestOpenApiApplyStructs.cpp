// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiApplyPrivate.h"
#include "EdGraphSchema_K2.h"
#include "HAL/IConsoleManager.h"
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/Package.h"

namespace VaRest::OpenApi
{
namespace
{
TAutoConsoleVariable<int32> SlowBuild(TEXT("VaRest.OpenApi.SlowStructBuild"),0,TEXT("Use the public per-member struct construction path for OpenAPI imports."),ECVF_Default);
FString StrippedName(const FName Name)
{
	const FString Text=Name.ToString();if (Text.Len()<=35) return Text;
	const FString Prefix=Text.LeftChop(33);int32 Underscore;return Prefix.FindLastChar(TEXT('_'),Underscore) && Underscore>0 ? Prefix.Left(Underscore) : Text;
}
}
bool FApply::CheckStruct(const UUserDefinedStruct* Struct,const TArray<FStructVariableDescription>& Members) const
{
	if (Struct->Status!=UDSS_UpToDate || FStructureEditorUtils::GetVarDesc(Struct).Num()!=Members.Num()) return false;
	for (const auto& Member:Members)
	{
		const auto* Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Member.VarGuid);
		if (!Actual || Actual->VarName!=Member.VarName || !Equal(Actual->FriendlyName,Member.FriendlyName) || Actual->bInvalidMember ||
			Struct->FindPropertyNameFromGuid(Member.VarGuid)!=Member.VarName || FStructureEditorUtils::GetGuidFromPropertyName(Member.VarName)!=Member.VarGuid || !Equal(StrippedName(Member.VarName),Member.FriendlyName)) return false;
		const FProperty* Property=FStructureEditorUtils::GetPropertyByGuid(Struct,Member.VarGuid);FEdGraphPinType ActualType;
		if (!Property || !GetDefault<UEdGraphSchema_K2>()->ConvertPropertyToPinType(Property,ActualType) || ActualType!=Member.ToPinType()) return false;
	}
	return true;
}
bool FApply::SlowStruct(UUserDefinedStruct* Struct,TArray<FStructVariableDescription>& Members)
{
	// New imports only. Do not remove a member from an existing reimport through this helper.
	FStructureEditorUtils::GetVarDesc(Struct).Reset();FStructureEditorUtils::OnStructureChanged(Struct);
	for (auto& Member:Members)
	{
		if (!FStructureEditorUtils::AddVariable(Struct,Member.ToPinType())) return false;
		const FGuid Guid=FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
		if (!FStructureEditorUtils::RenameVariable(Struct,Guid,Member.FriendlyName)) return false;
		if (!Member.DefaultValue.IsEmpty() && !FStructureEditorUtils::ChangeVariableDefaultValue(Struct,Guid,Member.DefaultValue)) return false;
		FStructureEditorUtils::ChangeVariableTooltip(Struct,Guid,Member.ToolTip);
		const auto* Actual=FStructureEditorUtils::GetVarDescByGuid(Struct,Guid);if (!Actual) return false;
		Member=*Actual;
	}
	return CheckStruct(Struct,Members);
}
bool FApply::BuildStruct(UUserDefinedStruct* Struct,const FVaRestOpenApiPlannedStruct& Row)
{
	TArray<FStructVariableDescription> Members;auto* EditorData=CastChecked<UUserDefinedStructEditorData>(Struct->EditorData);
	for (const auto& Field:Row.Fields)
	{
		FStructVariableDescription Member;const auto* ExistingGuid=Fields.Find(Field.Id);Member.VarGuid=ExistingGuid ? *ExistingGuid : FGuid::NewGuid();Member.FriendlyName=Field.FriendlyName;
		FEdGraphPinType PinType=Pin(Field.Type);FString Error;
		if (!FStructureEditorUtils::CanHaveAMemberVariableOfType(Struct,PinType,&Error))
		{
			Issue(TEXT("OA-APPLY-TYPE"),TEXT("Field ")+Field.FriendlyName+TEXT(" uses Json because its planned type was refused: ")+Error,Struct);
			PinType=FEdGraphPinType();PinType.PinCategory=UEdGraphSchema_K2::PC_Struct;PinType.PinSubCategoryObject=FVaRestJson::StaticStruct();
		}
		Member.SetPinType(PinType);Member.DefaultValue=Default(Field,PinType);Member.ToolTip=Field.Tooltip;
		Member.VarName=*FString::Printf(TEXT("%s_%u_%s"),*Member.FriendlyName,EditorData->GenerateUniqueNameIdForMemberVariable(),*Member.VarGuid.ToString(EGuidFormats::Digits));Members.Add(MoveTemp(Member));
	}
	if (Members.IsEmpty()) return false;
	bool Valid=false;
	if (SlowBuild.GetValueOnGameThread()==0)
	{
		FStructureEditorUtils::ModifyStructData(Struct);FStructureEditorUtils::GetVarDesc(Struct)=Members;FStructureEditorUtils::OnStructureChanged(Struct);Valid=CheckStruct(Struct,Members);
	}
	if (!Valid)
	{
		Issue(TEXT("OA-APPLY-SLOWPATH"),TEXT("Rebuilding this struct through the public per-member editor API."),Struct);
		Valid=SlowStruct(Struct,Members);
	}
	if (!Valid) { Issue(TEXT("OA-APPLY-COMPILE"),TEXT("Struct compilation/identity validation failed; endpoint uses fall back to Json."),Struct,EVaRestOpenApiSeverity::Error);return false; }
	FStructureEditorUtils::ChangeTooltip(Struct,Row.Tooltip);
	for (int32 Index=0;Index<Members.Num();++Index) Fields.Add(Row.Fields[Index].Id,Members[Index].VarGuid);
	return true;
}
void FApply::Structs()
{
	// Enums are complete before shells exist, avoiding enum broadcasts recompiling their users.
	for (const auto& Row:Plan.Structs)
	{
		if (Types.Contains(Row.Id)) continue;
		if (Cancelled()) return;FName Name;auto* Package=TypePackage(Row.AssetName,Name);
		auto* Struct=Package ? FStructureEditorUtils::CreateUserDefinedStruct(Package,Name,Options.Flags|RF_Public|RF_Standalone|RF_Transactional) : nullptr;
		if (!Struct) { FailedTypes.Add(Row.Id);Issue(TEXT("OA-APPLY-COMPILE"),TEXT("Could not create struct ")+Row.AssetName,nullptr,EVaRestOpenApiSeverity::Error);continue; }
		Keep(Struct);Types.Add(Row.Id,Struct);NewTypeIds.Add(Row.Id);
	}
	// Planner rows are child-first. A failed child is replaced before the parent is built.
	for (const auto& Row:Plan.Structs)
	{
		if (!NewTypeIds.Contains(Row.Id)) continue;
		if (Cancelled()) return;const auto* Found=Types.Find(Row.Id);if (!Found) continue;
		if (!BuildStruct(CastChecked<UUserDefinedStruct>(*Found),Row)) FailedTypes.Add(Row.Id);
	}
}
}
