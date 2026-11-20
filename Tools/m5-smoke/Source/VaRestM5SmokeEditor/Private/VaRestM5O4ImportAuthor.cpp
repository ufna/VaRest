// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestM5O4WireProof.h"
#include "Engine/UserDefinedEnum.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/StructureEditorUtils.h"
#include "Misc/AutomationTest.h"
#include "Misc/Base64.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "StructUtils/UserDefinedStruct.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "VaRestApi.h"
#include "VaRestHttpSettings.h"
#include "VaRestJson.h"
#include "VaRestJsonRules.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiLibrary.h"

namespace VaRestM5O4Author
{
// Same bytes are retained separately as schema.openapi.json for a Root-owned source-file input.
const TCHAR* Schema = TEXT(R"O4JSON({
  "openapi": "3.0.3",
  "info": {
    "title": "VaRest O4 imported wire fixture",
    "version": "1.0"
  },
  "servers": [
    {
      "url": "http://127.0.0.1:18765"
    }
  ],
  "paths": {
    "/echo/o4wire": {
      "post": {
        "operationId": "o4Echo",
        "requestBody": {
          "required": true,
          "content": {
            "application/json": {
              "schema": {
                "$ref": "#/components/schemas/Payload"
              }
            }
          }
        },
        "responses": {
          "200": {
            "description": "Official echo envelope; runtime decodes its body as Payload",
            "content": {
              "application/json": {
                "schema": {
                  "$ref": "#/components/schemas/Payload"
                }
              }
            }
          }
        }
      }
    }
  },
  "components": {
    "schemas": {
      "State": {
        "type": "string",
        "enum": [
          "wire-ONE",
          "wire-TWO"
        ]
      },
      "Payload": {
        "type": "object",
        "required": [
          "Wire.Score",
          "@type",
          "nullCount",
          "pretty"
        ],
        "properties": {
          "Wire.Score": {
            "type": "integer",
            "format": "int32",
            "default": 7
          },
          "@type": {
            "type": "string",
            "default": "alpha"
          },
          "omitCount": {
            "type": "integer",
            "format": "int32",
            "default": 42
          },
          "nullCount": {
            "type": "integer",
            "format": "int32",
            "nullable": true,
            "default": 42
          },
          "state": {
            "$ref": "#/components/schemas/State"
          },
          "pretty": {
            "type": "string",
            "enum": [
              "wire-ONE",
              "wire-TWO"
            ],
            "default": "wire-ONE"
          }
        }
      }
    }
  }
})O4JSON");
bool Author(FAutomationTestBase& Test)
{
    using namespace VaRestM5O4;
    check(IsInGameThread());
    const auto Fail = [&Test](const TCHAR* Reason) { Test.AddError(Reason); return false; };
    FString SchemaFile, ProofFile, OutputFolder;
    if (!FParse::Param(FCommandLine::Get(), TEXT("VaRestO4FixtureAuthor"))
        || !FParse::Param(FCommandLine::Get(), TEXT("nowrite"))
        || !FParse::Value(FCommandLine::Get(), TEXT("VaRestO4Schema="), SchemaFile)
        || !FParse::Value(FCommandLine::Get(), TEXT("VaRestO4AuthorProof="), ProofFile)
        || FPaths::IsRelative(SchemaFile) || FPaths::IsRelative(ProofFile)
        || IFileManager::Get().FileExists(*ProofFile)
        || !IFileManager::Get().DirectoryExists(*FPaths::GetPath(ProofFile))
        || !FPackageName::TryConvertLongPackageNameToFilename(FString(Folder), OutputFolder)
        || IFileManager::Get().DirectoryExists(*OutputFolder)
        || FindObject<UObject>(nullptr, ApiPath) || FPackageName::DoesPackageExist(ApiPackage))
        return Fail(TEXT("Opt-in/nowrite, exact source input, fresh proof and unused owned O4 namespace are required"));
    FString ActualSchema;
    if (!FFileHelper::LoadFileToString(ActualSchema, *SchemaFile)) return Fail(TEXT("Cannot read Root-admitted local schema source"));
    const auto Input = FVaRestJson::Parse(FStringView(ActualSchema));
    const auto Expected = FVaRestJson::Parse(FStringView(Schema));
    if (!Input.HasValue() || !Expected.HasValue() || !Input.GetValue().Equals(Expected.GetValue()))
        return Fail(TEXT("Local schema does not equal the six-feature self-contained O4 fixture"));
    auto* Settings = GetMutableDefault<UVaRestHttpSettings>();
    const auto OldApis = Settings->Apis;
    ON_SCOPE_EXIT { Settings->Apis = OldApis; }; // -nowrite plus Root's whole-config guard protects disk.
    TStrongObjectPtr<UVaRestOpenApiImportOptions> Options(NewObject<UVaRestOpenApiImportOptions>());
    if (!Options->FromJson(FVaRestJson::Object())) return Fail(TEXT("Cannot initialize explicit public import options"));
    Options->TypesFolder = TypesFolder; Options->TypePrefix = TEXT("O4");
    Options->bUnwrapEnvelope = false; Options->bEnumsAsString = false;
    Options->bGenerateUnreferencedSchemas = false; Options->bAllowReferencesOutsideSpecFolder = false;
    Options->bImportAllOperations = false; Options->SelectedOperations = { TEXT("o4Echo") };
    // Genuine public library/factory path, not the lower-level applier or manually manufactured JsonTypes.
    TStrongObjectPtr<UVaRestApi> Api(UVaRestOpenApiLibrary::ImportFromFile(SchemaFile, ApiPackage, Options.Get()));
    auto* Root = Api.IsValid() && Api->Endpoints.Num() == 1
        ? Cast<UUserDefinedStruct>(Api->Endpoints[0].ResponseType.Type.Get()) : nullptr;
    if (!Api.IsValid() || !Root || !Api->GetPathName().Equals(ApiPath, ESearchCase::CaseSensitive)
        || !Api->TypesFolder.Equals(TypesFolder, ESearchCase::CaseSensitive)
        || Api->ImportState.Importer.IsNone() || Api->ImportState.Version <= 0 || Api->ImportState.Blob.Num() == 0)
        return Fail(TEXT("Public importer did not return a real imported API/root/manifest"));
    const auto OriginalManifest = Api->ImportState.Blob;
    TArray<UObject*> Assets { Api.Get() };
    TArray<FVaRestJsonFieldRule> OriginalFields;
    TMap<UField*, TArray<FVaRestJsonEnumRule>> OriginalEnums;
    FGuid RenamedGuid;
    int32 UnsetEnums = 0, DisplayEdits = 0;
    auto Members = FVaRestJson::Array();
    auto Enums = FVaRestJson::Array();
    for (const auto& Type : Api->JsonTypes)
    {
        auto* Object = Type.Type.Get();
        if (!Object || !Object->GetPathName().StartsWith(FString(TypesFolder) + TEXT("/"), ESearchCase::CaseSensitive))
            return Fail(TEXT("Imported type escaped the unused owned namespace"));
        Assets.AddUnique(Object);
        if (Object == Root)
        {
            OriginalFields = Type.Fields;
            for (const auto& Rule : Type.Fields)
            {
                if (!Rule.Field.IsValid() || Root->FindPropertyNameFromGuid(Rule.Field).IsNone())
                    return Fail(TEXT("Imported field has no real member GUID/property"));
                if (Rule.Json.Equals(TEXT("Wire.Score"), ESearchCase::CaseSensitive)) RenamedGuid = Rule.Field;
                Members.Add(FVaRestJson::Object().Set(TEXT("wire"), Rule.Json)
                    .Set(TEXT("guid"), Rule.Field.ToString(EGuidFormats::DigitsWithHyphens))
                    .Set(TEXT("property_before"), Root->FindPropertyNameFromGuid(Rule.Field).ToString()));
            }
        }
        else if (!Cast<UUserDefinedEnum>(Object))
            return Fail(TEXT("Unexpected non-root/non-enum declaration in the bounded fixture"));
        if (auto* Enum = Cast<UUserDefinedEnum>(Object))
        {
            OriginalEnums.Add(Object, Type.Enums);
            bool bUnset = false;
            for (const auto& Rule : Type.Enums)
            {
                bUnset |= Rule.bUnset;
                if (!Rule.bUnset && Rule.Json.Equals(TEXT("wire-TWO"), ESearchCase::CaseSensitive))
                {
                    const int32 Index = Enum->GetIndexByName(Rule.Enumerator);
                    if (Index == INDEX_NONE) return Fail(TEXT("Imported wire enum rule has no genuine enumerator"));
                    FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(TEXT("Designer Two")));
                    if (!Enum->GetDisplayNameTextByIndex(Index).ToString().Equals(TEXT("Designer Two"), ESearchCase::CaseSensitive))
                        return Fail(TEXT("Real enum-editor display-name edit failed"));
                    ++DisplayEdits;
                }
                Enums.Add(FVaRestJson::Object().Set(TEXT("type"), Enum->GetPathName())
                    .Set(TEXT("enumerator"), Rule.Enumerator.ToString()).Set(TEXT("wire"), Rule.Json).Set(TEXT("unset"), Rule.bUnset));
            }
            if (bUnset) ++UnsetEnums;
        }
    }
    if (OriginalFields.Num() != 6 || !RenamedGuid.IsValid() || DisplayEdits == 0 || UnsetEnums == 0)
        return Fail(TEXT("Importer did not supply all six fields, mapped/prettified enum and optional unset variant"));
    const uint64 Generation = FVaRestJsonRules::GetGeneration(Api.Get());
    if (!FStructureEditorUtils::RenameVariable(Root, RenamedGuid, TEXT("Designer Score"))
        || !Root->FindPropertyNameFromGuid(RenamedGuid).ToString().StartsWith(TEXT("DesignerScore_"), ESearchCase::CaseSensitive)
        || FVaRestJsonRules::GetGeneration(Api.Get()) != Generation)
        return Fail(TEXT("Actual struct-editor rename did not preserve the imported GUID/default rule binding"));
    for (const auto& Type : Api->JsonTypes)
    {
        if (Type.Type.Get() == Root && Type.Fields != OriginalFields)
            return Fail(TEXT("Fixture modified importer-generated JSON field rules"));
        if (const auto* Before = OriginalEnums.Find(Type.Type.Get()))
            if (Type.Enums != *Before) return Fail(TEXT("Display edit modified imported wire enum mappings"));
    }
    if (Api->ImportState.Blob != OriginalManifest)
        return Fail(TEXT("Designer rename/display edits rewrote the imported manifest"));
    FString Wire, Error;
    if (!VerifyConversion(Api.Get(), Root, Wire, Error)) { Test.AddError(Error); return false; }
    FVaRestApiValidationContext Validation; Api->Validate(Validation);
    if (Validation.HasErrors()) return Fail(TEXT("Imported API validation reports errors"));
    auto Saved = FVaRestJson::Array();
    for (UObject* Object : Assets)
    {
        UPackage* Package = Object->GetOutermost();
        const FString PackageName = Package->GetName();
        if (!PackageName.StartsWith(FString(Folder) + TEXT("/"), ESearchCase::CaseSensitive))
            return Fail(TEXT("Save target escaped the owned O4 namespace"));
        const FString Filename = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
        if (IFileManager::Get().FileExists(*Filename)
            || !IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename), true))
            return Fail(TEXT("Save refuses any preexisting asset or unavailable owned directory"));
        FSavePackageArgs Save; Save.TopLevelFlags = RF_Public | RF_Standalone; Save.SaveFlags = SAVE_NoError;
        Package->MarkPackageDirty();
        if (!UPackage::SavePackage(Package, Object, *Filename, Save))
            return Fail(TEXT("Imported asset save failed; retain partial namespace, never overwrite or sweep it"));
        Saved.Add(FVaRestJson::Object().Set(TEXT("package"), PackageName).Set(TEXT("object"), Object->GetPathName()).Set(TEXT("filename"), Filename));
        Test.AddInfo(FString::Printf(TEXT("VAREST_O4_SAVED object=%s filename=%s"), *Object->GetPathName(), *Filename));
    }
    auto Proof = FVaRestJson::Object().Set(TEXT("schema"), TEXT("VaRest.O4.ImportedAuthor.1"))
        .Set(TEXT("public_import"), true).Set(TEXT("source_file"), SchemaFile).Set(TEXT("api"), ApiPath)
        .Set(TEXT("root"), Root->GetPathName()).Set(TEXT("types_folder"), TypesFolder)
        .Set(TEXT("importer"), Api->ImportState.Importer.ToString()).Set(TEXT("manifest_version"), Api->ImportState.Version)
        .Set(TEXT("manifest_base64"), FBase64::Encode(OriginalManifest)).Set(TEXT("manifest_unchanged_by_designer_edits"), true)
        .Set(TEXT("members"), Members).Set(TEXT("enums"), Enums).Set(TEXT("saved"), Saved)
        .Set(TEXT("renamed_guid"), RenamedGuid.ToString(EGuidFormats::DigitsWithHyphens))
        .Set(TEXT("property_after"), Root->FindPropertyNameFromGuid(RenamedGuid).ToString())
        .Set(TEXT("author_wire"), Wire).Set(TEXT("cold_editor_cooked_echo"), TEXT("UNRUN"));
    if (!FFileHelper::SaveStringToFile(Proof.ToString(), *ProofFile, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM,
            &IFileManager::Get(), FILEWRITE_NoReplaceExisting))
        return Fail(TEXT("Fresh author proof could not be retained"));
    Test.AddInfo(TEXT("VAREST_O4_AUTHORED publicImporter=1 savedImportedAssets=1 memberGuids=1 manifest=1 designerRename=1 enumDisplay=1 coldEditorCookedEcho=UNRUN"));
    return true;
}
class FWaitEditorWire final : public IAutomationLatentCommand
{
public:
    explicit FWaitEditorWire(FAutomationTestBase* InTest) : Test(InTest), Deadline(FPlatformTime::Seconds() + 35.) {}
    virtual bool Update() override
    {
        FString Detail, Wire;
        const int32 Outcome = VaRestM5O4::GetOutcome(Detail, Wire);
        if (Outcome == 0 && FPlatformTime::Seconds() < Deadline) return false;
        if (Outcome != 1) Test->AddError(FString::Printf(TEXT("Cold O4 Editor wire witness did not pass: %s"), *Detail));
        else Test->AddInfo(FString::Printf(TEXT("VAREST_O4_EDITOR_WIRE_PASS wire=%s"), *Wire));
        return true;
    }
private:
    FAutomationTestBase* Test;
    double Deadline;
};
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestO4ImportedWireTest, "VaRest.OpenApi.O4ImportedWire",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
void FVaRestO4ImportedWireTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
    if (!FParse::Param(FCommandLine::Get(), TEXT("VaRestO4WireEditor"))
        && FParse::Param(FCommandLine::Get(), TEXT("VaRestO4FixtureAuthor"))
        && FParse::Param(FCommandLine::Get(), TEXT("nowrite")))
    { Names.Add(TEXT("Author")); Commands.Add(TEXT("Author")); }
    else if (FParse::Param(FCommandLine::Get(), TEXT("VaRestO4WireEditor"))
        && !FParse::Param(FCommandLine::Get(), TEXT("VaRestO4FixtureAuthor")))
    { Names.Add(TEXT("EditorWire")); Commands.Add(TEXT("EditorWire")); }
}
bool FVaRestO4ImportedWireTest::RunTest(const FString& Parameters)
{
    if (Parameters == TEXT("Author") && FParse::Param(FCommandLine::Get(), TEXT("VaRestO4FixtureAuthor"))
        && FParse::Param(FCommandLine::Get(), TEXT("nowrite"))
        && !FParse::Param(FCommandLine::Get(), TEXT("VaRestO4WireEditor")))
        return VaRestM5O4Author::Author(*this);
    if (Parameters == TEXT("EditorWire") && FParse::Param(FCommandLine::Get(), TEXT("VaRestO4WireEditor"))
        && !FParse::Param(FCommandLine::Get(), TEXT("VaRestO4FixtureAuthor")))
    { AddCommand(new VaRestM5O4Author::FWaitEditorWire(this)); return true; }
    AddError(TEXT("Only the explicitly opted-in O4 role is executable")); return false;
}
#endif
