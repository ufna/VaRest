// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Explicit Editor asset authoring. No map, Blueprint, cooker gameplay preload, or runtime explicit loading.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestM5SettingsOnlyProof.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "CoreGlobals.h"
#include "EdGraphSchema_K2.h"
#include "Editor.h"
#include "Engine/UserDefinedEnum.h"
#include "HAL/FileManager.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "VaRestApi.h"
#include "VaRestJsonRules.h"

namespace VaRestM5SettingsAuthor
{
bool Dedicated()
{
    return !IsRunningCommandlet() && FParse::Param(FCommandLine::Get(), TEXT("VaRestM5AuthorSettingsOnly"))
        && FParse::Param(FCommandLine::Get(), TEXT("unattended")) && FParse::Param(FCommandLine::Get(), TEXT("nowrite"));
}
FEdGraphPinType Pin(FName Category, UObject* Type = nullptr, EPinContainerType Container = EPinContainerType::None)
{
    return FEdGraphPinType(Category, NAME_None, Type, Container, false, FEdGraphTerminalType());
}
FGuid FirstBoolean(FAutomationTestBase& Test, UUserDefinedStruct* Struct, const TCHAR* Friendly)
{
    if (!Test.TestTrue(TEXT("new UDS has exactly its real initial boolean"), Struct && FStructureEditorUtils::GetVarDesc(Struct).Num() == 1)) return {};
    const FGuid Guid = FStructureEditorUtils::GetVarDesc(Struct)[0].VarGuid;
    if (!Test.TestTrue(TEXT("rename real initial boolean"), FStructureEditorUtils::RenameVariable(Struct, Guid, Friendly))) return {};
    return Guid;
}
FGuid Add(FAutomationTestBase& Test, UUserDefinedStruct* Struct, const TCHAR* Friendly, const FEdGraphPinType& Type, const TCHAR* Default = nullptr)
{
    if (!Test.TestTrue(FString::Printf(TEXT("add real field %s"), Friendly), FStructureEditorUtils::AddVariable(Struct, Type))) return {};
    const FGuid Guid = FStructureEditorUtils::GetVarDesc(Struct).Last().VarGuid;
    if (!Test.TestTrue(FString::Printf(TEXT("name real field %s"), Friendly), FStructureEditorUtils::RenameVariable(Struct, Guid, Friendly))) return {};
    if (Default && !Test.TestTrue(FString::Printf(TEXT("author default %s"), Friendly), FStructureEditorUtils::ChangeVariableDefaultValue(Struct, Guid, Default))) return {};
    return Guid;
}
void Rule(FVaRestApiJsonTypeRules& Type, FGuid Guid, const TCHAR* Wire, EVaRestJsonPresence Presence = EVaRestJsonPresence::Always,
    bool bNullable = false, bool bBase64 = false)
{
    FVaRestJsonFieldRule Value; Value.Field = Guid; Value.Json = Wire; Value.Presence = Presence;
    Value.bNullable = bNullable; Value.bBase64 = bBase64; Type.Fields.Add(MoveTemp(Value));
}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestM5SettingsOnlyAuthor, "VaRest.M5.SettingsOnly.Author",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FVaRestM5SettingsOnlyAuthor::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    if (VaRestM5SettingsAuthor::Dedicated()) { Names.Add(TEXT("SettingsOnly001")); Commands.Add(TEXT("SettingsOnly001")); }
}
bool FVaRestM5SettingsOnlyAuthor::RunTest(const FString& Parameters)
{
    using namespace VaRestM5SettingsOnly;
    using namespace VaRestM5SettingsAuthor;
    if (!Dedicated() || Parameters != TEXT("SettingsOnly001") || !GEditor || !GEditor->Trans)
    { AddError(TEXT("Requires the dedicated unattended nowrite real Editor process with transaction buffer")); return false; }
    const TArray<FString> Packages {
        TEXT("/VaRestM5Smoke/SettingsOnly001/Types/SM5_SettingsOnlyState"),
        TEXT("/VaRestM5Smoke/SettingsOnly001/Types/SM5_SettingsOnlyNested"),
        TEXT("/VaRestM5Smoke/SettingsOnly001/Types/SM5_SettingsOnlyResponse"),
        TEXT("/VaRestM5Smoke/SettingsOnly001/SM5_SettingsOnlyApi")
    };
    FString OutputFolder;
    if (!FPackageName::TryConvertLongPackageNameToFilename(FString(Folder), OutputFolder)
        || IFileManager::Get().DirectoryExists(*OutputFolder))
    { AddError(TEXT("Fixture content mount unavailable or fresh owned namespace already occupied; no overwrite")); return false; }
    for (const FString& Package : Packages)
        if (FindPackage(nullptr, *Package) || FPackageName::DoesPackageExist(Package))
        { AddError(TEXT("An owned fixture package is already loaded/on disk; no overwrite")); return false; }

    TStrongObjectPtr<UPackage> EnumPackage(CreatePackage(*Packages[0]));
    TStrongObjectPtr<UPackage> NestedPackage(CreatePackage(*Packages[1]));
    TStrongObjectPtr<UPackage> RootPackage(CreatePackage(*Packages[2]));
    TStrongObjectPtr<UPackage> ApiPackage(CreatePackage(*Packages[3]));
    constexpr EObjectFlags AssetFlags = RF_Public | RF_Standalone | RF_Transactional;
    TStrongObjectPtr<UUserDefinedEnum> State(Cast<UUserDefinedEnum>(FEnumEditorUtils::CreateUserDefinedEnum(
        EnumPackage.Get(), TEXT("SM5_SettingsOnlyState"), AssetFlags)));
    TStrongObjectPtr<UUserDefinedStruct> Nested(FStructureEditorUtils::CreateUserDefinedStruct(
        NestedPackage.Get(), TEXT("SM5_SettingsOnlyNested"), AssetFlags));
    TStrongObjectPtr<UUserDefinedStruct> Root(FStructureEditorUtils::CreateUserDefinedStruct(
        RootPackage.Get(), TEXT("SM5_SettingsOnlyResponse"), AssetFlags));
    TStrongObjectPtr<UVaRestApi> Api(NewObject<UVaRestApi>(ApiPackage.Get(), TEXT("SM5_SettingsOnlyApi"), AssetFlags));
    if (!TestTrue(TEXT("four genuine independent asset objects"), State && Nested && Root && Api)) return false;
    for (int32 Index = 0; Index != 3; ++Index) FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(State.Get());
    if (!TestEqual(TEXT("three real enum entries plus MAX"), State->NumEnums(), 4)
        || !TestTrue(TEXT("real pretty unset display name"), FEnumEditorUtils::SetEnumeratorDisplayName(State.Get(), 0, FText::FromString(TEXT("Unset Display"))))
        || !TestTrue(TEXT("real mapped display differs from wire"), FEnumEditorUtils::SetEnumeratorDisplayName(State.Get(), 1, FText::FromString(TEXT("Pretty State One"))))
        || !TestTrue(TEXT("real unmapped pretty display name"), FEnumEditorUtils::SetEnumeratorDisplayName(State.Get(), 2, FText::FromString(TEXT("Pretty State Two"))))) return false;

    FVaRestApiJsonTypeRules NestedRules; NestedRules.Type = Nested.Get();
    Rule(NestedRules, FirstBoolean(*this, Nested.Get(), TEXT("Nested Flag")), TEXT("Nested.Flag"));
    Rule(NestedRules, Add(*this, Nested.Get(), TEXT("Nested Value"), Pin(UEdGraphSchema_K2::PC_Int)), TEXT("Nested.Value"));
    FVaRestApiJsonTypeRules RootRules; RootRules.Type = Root.Get();
    Rule(RootRules, FirstBoolean(*this, Root.Get(), TEXT("Always Flag")), TEXT("ExactFlag"));
    const FGuid RenamedGuid = Add(*this, Root.Get(), TEXT("Original Score"), Pin(UEdGraphSchema_K2::PC_Int), TEXT("42"));
    Rule(RootRules, RenamedGuid, TEXT("Wire.Score"));
    const FGuid SanitizedGuid = Add(*this, Root.Get(), TEXT("Sanitized Label"), Pin(UEdGraphSchema_K2::PC_String));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Omit Count"), Pin(UEdGraphSchema_K2::PC_Int), TEXT("42")), TEXT("omitCount"), EVaRestJsonPresence::OmitWhenDefault);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Null Count"), Pin(UEdGraphSchema_K2::PC_Int), TEXT("42")), TEXT("nullCount"), EVaRestJsonPresence::NullWhenDefault);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Decode Only"), Pin(UEdGraphSchema_K2::PC_String)), TEXT("decodeOnly"), EVaRestJsonPresence::DecodeOnly);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Ignored Text"), Pin(UEdGraphSchema_K2::PC_String), TEXT("kept")), TEXT("ignored"), EVaRestJsonPresence::Ignore);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Nullable Text"), Pin(UEdGraphSchema_K2::PC_String), TEXT("kept")), TEXT("nullable"), EVaRestJsonPresence::Always, true);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Blob Bytes"), Pin(UEdGraphSchema_K2::PC_Byte, nullptr, EPinContainerType::Array)), TEXT("bytes"), EVaRestJsonPresence::Always, false, true);
    const FEdGraphPinType EnumScalar = Pin(UEdGraphSchema_K2::PC_Byte, State.Get());
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Unset State"), EnumScalar), TEXT("state"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Null State"), EnumScalar), TEXT("nullState"), EVaRestJsonPresence::NullWhenDefault);
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Mapped State"), EnumScalar), TEXT("mapped"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Pretty State"), EnumScalar), TEXT("pretty"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("States"), Pin(UEdGraphSchema_K2::PC_Byte, State.Get(), EPinContainerType::Array)), TEXT("states"));
    FEdGraphPinType EnumMap = Pin(UEdGraphSchema_K2::PC_String, nullptr, EPinContainerType::Map);
    EnumMap.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Byte;
    EnumMap.PinValueType.TerminalSubCategoryObject = State.Get();
    Rule(RootRules, Add(*this, Root.Get(), TEXT("State Map"), EnumMap), TEXT("stateMap"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Nested"), Pin(UEdGraphSchema_K2::PC_Struct, Nested.Get())), TEXT("nested"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Scalars"), Pin(UEdGraphSchema_K2::PC_Int, nullptr, EPinContainerType::Array)), TEXT("scalars"));
    FEdGraphPinType ScalarMap = Pin(UEdGraphSchema_K2::PC_String, nullptr, EPinContainerType::Map);
    ScalarMap.PinValueType.TerminalCategory = UEdGraphSchema_K2::PC_Int;
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Scalar Map"), ScalarMap), TEXT("scalarMap"));
    Rule(RootRules, Add(*this, Root.Get(), TEXT("Stamp"), Pin(UEdGraphSchema_K2::PC_Struct, TBaseStructure<FDateTime>::Get())), TEXT("stamp"));
    if (!RenamedGuid.IsValid() || !SanitizedGuid.IsValid()) return false;
    for (const auto* Type : { &NestedRules, &RootRules })
        for (const FVaRestJsonFieldRule& Field : Type->Fields)
            if (!TestTrue(TEXT("every authored rule has an actual field GUID"), Field.Field.IsValid())) return false;

    FVaRestApiJsonTypeRules EnumRules; EnumRules.Type = State.Get();
    FVaRestJsonEnumRule Unset; Unset.Enumerator = State->GetNameByIndex(0); Unset.bUnset = true;
    FVaRestJsonEnumRule One; One.Enumerator = State->GetNameByIndex(1); One.Json = TEXT("wire-ONE");
    EnumRules.Enums = { Unset, One }; // Index 2 intentionally uses its real prettified authored display text.
    Api->JsonTypes = { RootRules, NestedRules, EnumRules };
    Api->TypesFolder = FString(Folder) + TEXT("/Types");
    FVaRestApiEnvironment Environment; Environment.Name = TEXT("SettingsOnly"); Environment.BaseUrl = TEXT("https://settings-only.invalid");
    Api->Environments.Add(Environment); Api->DefaultEnvironment = Environment.Name;
    Api->NotifyEdited();
    FString Error;
    if (!TestTrue(TEXT("real editor default conversion before GUID-preserving rename"), VerifyDefaultConversion(Api.Get(), Root.Get(), Error)))
    { AddError(Error); return false; }
    const uint64 RulesGeneration = FVaRestJsonRules::GetGeneration(Api.Get());
    if (!TestTrue(TEXT("actual editor rename/recompile retains original field GUID"), FStructureEditorUtils::RenameVariable(Root.Get(), RenamedGuid, TEXT("Renamed Score")))
        || !TestEqual(TEXT("rename needs no rule republish"), FVaRestJsonRules::GetGeneration(Api.Get()), RulesGeneration)) return false;
    if (!TestTrue(TEXT("actual renamed cooked property name"), Root->FindPropertyNameFromGuid(RenamedGuid).ToString().StartsWith(TEXT("RenamedScore_"), ESearchCase::CaseSensitive))) return false;
    if (!VerifyDefaultConversion(Api.Get(), Root.Get(), Error)) { AddError(Error); return false; }
    FVaRestApiValidationContext Validation; Api->Validate(Validation);
    if (!TestFalse(TEXT("actual API validation has no configuration/rule errors"), Validation.HasErrors())) return false;

    const TArray<UObject*> Assets { State.Get(), Nested.Get(), Root.Get(), Api.Get() };
    if (!IFileManager::Get().MakeDirectory(*OutputFolder, true)) { AddError(TEXT("Cannot create fresh owned output namespace")); return false; }
    for (int32 Index = 0; Index != Assets.Num(); ++Index)
    {
        UObject* Asset = Assets[Index]; UPackage* Package = Asset->GetOutermost();
        FAssetRegistryModule::AssetCreated(Asset); Package->MarkPackageDirty();
        const FString Filename = FPackageName::LongPackageNameToFilename(Packages[Index], FPackageName::GetAssetPackageExtension());
        if (!IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true)) { AddError(TEXT("Cannot create owned type directory")); return false; }
        FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
        if (!UPackage::SavePackage(Package, Asset, *Filename, Save)) { AddError(TEXT("Owned fixture save failed; retain partial output as failed evidence, never overwrite it")); return false; }
        AddInfo(FString::Printf(TEXT("VAREST_M5_SETTINGS_SAVED package=%s object=%s filename=%s"), *Packages[Index], *Asset->GetPathName(), *Filename));
    }
    AddInfo(FString::Printf(TEXT("VAREST_M5_SETTINGS_AUTHORED proof=B namespace=%s renamedFieldGuid=%s sanitizedFieldGuid=%s editorDefaultConversion=PASS packages=4 runtimeCookMutants=UNRUN proofA=OPEN wholeTask14=OPEN"),
        Folder, *RenamedGuid.ToString(EGuidFormats::Digits), *SanitizedGuid.ToString(EGuidFormats::Digits)));
    return true;
}
#endif
