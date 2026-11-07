// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestOpenApiModel.h"
#include "VaRestOpenApiPlan.h"
#include "VaRestOpenApiApply.h"
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiImportOptions.h"
#include "VaRestOpenApiFactory.h"
#include "VaRestHttpSettings.h"
#include "AssetImportTask.h"
#include "CoreGlobals.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "VaRestOpenApiImportDialog.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "Engine/DataTable.h"
#include "Async/Async.h"
#include "VaRestOpenApiSource.h"
#include "VaRestOpenApiUrlPolicy.h"
#include "VaRestApi.h"
#include "VaRestJsonRules.h"
#include "Engine/UserDefinedEnum.h"
#include "StructUtils/UserDefinedStruct.h"
#include "Kismet2/StructureEditorUtils.h"
#include "BlueprintActionDatabase.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "HAL/FileManager.h"
#include "UObject/SavePackage.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/ReferenceChainSearch.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRestOpenApiOfflineTest
{
TArray<uint8> Bytes(const FString& Text)
{
 const FTCHARToUTF8 Utf8(*Text,Text.Len()); return TArray<uint8>(reinterpret_cast<const uint8*>(Utf8.Get()),Utf8.Length());
}
bool NoErrors(FAutomationTestBase& Test,TConstArrayView<FVaRestOpenApiIssue> Issues,const TCHAR* Step)
{
 bool Success=true;
 for (const auto& I:Issues)
 {
  const FString Text=FString::Printf(TEXT("%s: %s %s [%s%s]"),Step,*I.Code,*I.Message,*I.Document,*I.Pointer);
  if (I.Severity==EVaRestOpenApiSeverity::Error) { Test.AddError(Text); Success=false; }
  else Test.AddInfo(Text);
 }
 return Success;
}
bool Has(TConstArrayView<FVaRestOpenApiIssue> Issues,const TCHAR* Code)
{
 for (const auto& I:Issues) if (I.Code==Code) return true; return false;
}
const TCHAR* Source=TEXT(R"json({
 "openapi":"3.0.3","info":{"title":"Pets","version":"1.0"},
 "servers":[{"url":"https://pets.invalid/v1"}],
 "paths":{"/pets/{id}":{"get":{"operationId":"fetchPet","summary":"Original summary",
  "parameters":[{"name":"id","in":"path","required":true,"schema":{"type":"integer","format":"int64"}}],
  "responses":{"200":{"description":"ok","content":{"application/json":{"schema":{"$ref":"#/components/schemas/Pet"}}}}}}}},
 "components":{"schemas":{
  "Status":{"type":"string","enum":["waiting","ready"]},
  "Pet":{"type":"object","required":["id","display_name"],"properties":{
   "id":{"type":"integer","format":"int64"},"display_name":{"type":"string","default":"original"},
   "status":{"$ref":"#/components/schemas/Status"}}}}}
})json");
struct FOwnedAssets
{
 UVaRestApi* Api=nullptr;
 TArray<TStrongObjectPtr<UObject>> Objects;
 ~FOwnedAssets()
 {
  if (Api)
  {
   // NotifyEdited registers endpoint spawners even when Asset Registry publication is disabled.
   // Retire only this fixture's palette entry (and its cached templates) before GC.
   FBlueprintActionDatabase::Get().ClearAssetActions(Api);
   FVaRestJsonRules::UnregisterOwner(Api);
  }
  for (auto& Object:Objects) if (Object.IsValid())
  {
   Object->ClearFlags(RF_Public|RF_Standalone); Object->GetOutermost()->SetDirtyFlag(false);
  }
  Objects.Reset(); CollectGarbage(RF_NoFlags);
 }
 void Keep(const FVaRestOpenApiApplyResult& Result)
 {
  if (Result.Api) { Api=Result.Api; Objects.Emplace(Api); }
  for (auto* Object:Result.AdditionalObjects) if (Object) Objects.Emplace(Object);
 }
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiLoaderOfflineTest,"VaRest.OpenApi.Offline.Loader",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiLoaderOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 const FString Root=TEXT("/VaRestOpenApiQualification/root.json");
 const auto Input=Bytes(Source); const auto Original=Input;
 auto Analysis=FVaRestOpenApiLoader::Analyze(Root,Input);
 if (!TestTrue(TEXT("valid OpenAPI 3 JSON"),Analysis.bDocumentValid) || !NoErrors(*this,Analysis.Issues,TEXT("analyze"))) return false;
 TestTrue(TEXT("source bytes remain unchanged"),Input==Original);
 TestEqual(TEXT("operation count"),Analysis.Model.Operations.Num(),1);
 TestEqual(TEXT("no implicit external IO"),Analysis.Model.SourceDocuments.Num(),1);
 const auto Plan=FVaRestOpenApiPlanner::Build(Analysis);
 if (!TestTrue(TEXT("native plan"),Plan.bValid) || !NoErrors(*this,Plan.Issues,TEXT("plan"))) return false;
 TestEqual(TEXT("one referenced object type"),Plan.Structs.Num(),1);
 TestEqual(TEXT("one referenced enum type"),Plan.Enums.Num(),1);
 if (!TestEqual(TEXT("endpoint count"),Plan.Endpoints.Num(),1)) return false;
 TestEqual(TEXT("status key 200 resolves to response struct"),Plan.Endpoints[0].ResponseType.Kind,EVaRestOpenApiValueKind::Struct);
 if (!TestEqual(TEXT("path parameter count"),Plan.Endpoints[0].Parameters.Num(),1)) return false;
 TestEqual(TEXT("int64 path parameter"),Plan.Endpoints[0].Parameters[0].Type.Kind,EVaRestOpenApiValueKind::Int64);
 const auto Again=FVaRestOpenApiPlanner::Build(Analysis);
 TestTrue(TEXT("plain plan is deterministic"),Plan.ToJson().Equals(Again.ToJson()));
 auto Bad=FVaRestOpenApiLoader::Analyze(Root,Bytes(TEXT("{\"openapi\":\"2.0\",\"paths\":{}}")));
 TestFalse(TEXT("unsupported document version refuses"),Bad.bDocumentValid);
 TestTrue(TEXT("unsupported version has a diagnostic"),Has(Bad.Issues,TEXT("OA-DOC-VERSION")));
 auto Yaml=FVaRestOpenApiLoader::Analyze(Root,Bytes(TEXT("openapi: 3.0.3\npaths: {}\n")));
 TestFalse(TEXT("separate YAML delivery is not silently approximated"),Yaml.bDocumentValid);
 TestTrue(TEXT("JSON-only release gives the conversion diagnostic"),Has(Yaml.Issues,TEXT("OA-YAML-SYNTAX")));
 FVaRestOpenApiLoadOptions Limit; Limit.MaxDocumentBytes=Input.Num()-1;
 auto Large=FVaRestOpenApiLoader::Analyze(Root,Input,Limit);
 TestFalse(TEXT("document size limit"),Large.bDocumentValid);
 TestTrue(TEXT("size refusal code"),Has(Large.Issues,TEXT("OA-DOC-LIMIT")));
 int32 Reads=0;
 FVaRestOpenApiLoadOptions Confined; Confined.Read=[&](const FVaRestOpenApiReadRequest&,FVaRestOpenApiReadResult&) { ++Reads; return false; };
 FString Escaped=FString(Source).Replace(TEXT("#/components/schemas/Pet"),TEXT("../outside.json#/Pet"));
 auto Escape=FVaRestOpenApiLoader::Analyze(Root,Bytes(Escaped),Confined);
 TestEqual(TEXT("outside-root reference refused before injected IO"),Reads,0);
 TestFalse(TEXT("failed item is reported"),Escape.Issues.IsEmpty());
 TestEqual(TEXT("failed operation provenance survives"),Escape.Model.FailedOperations.Num(),1);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiUrlReferencesOfflineTest,"VaRest.OpenApi.Offline.UrlReferences",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiUrlReferencesOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 using namespace VaRest::OpenApi;
 const FString Root=TEXT("HTTPS://Specs.Invalid:443/team/Root%20Doc.json?rev=1");
 const FString CanonicalRoot=TEXT("https://specs.invalid/team/Root%20Doc.json?rev=1");
 const FString Revision2=TEXT("https://specs.invalid/team/Root%20Doc.json?rev=2");
 const FString Revision3=TEXT("https://specs.invalid/team/Root%20Doc.json?rev=3");
 TArray<FString> Reads;
 FVaRestOpenApiLoadOptions Options;
 Options.Read=[&](const FVaRestOpenApiReadRequest& Request,FVaRestOpenApiReadResult& Out)
 {
  Reads.Add(Request.Uri);
  TestTrue(TEXT("URL reader retains URL mode"),Request.bUrl);
  TestEqual(TEXT("every read carries the original directory boundary"),Request.RootBase,FString(TEXT("https://specs.invalid/team/")));
  TestFalse(TEXT("URL references cannot opt out of the boundary"),Request.bAllowOutsideRoot);
  Out.CanonicalUri=Request.Uri;
  if (Request.Uri==Revision2) Out.Bytes=Bytes(TEXT(R"json({"Pet":{"$ref":"?rev=3#/Pet"}})json"));
  else if (Request.Uri==Revision3) Out.Bytes=Bytes(TEXT(R"json({"Pet":{"type":"object","properties":{"revision":{"type":"integer"}}}})json"));
  else { Out.Error=TEXT("Unexpected source URI; no IO is performed."); return false; }
  return true;
 };
 const FString Input=FString(Source).Replace(TEXT("#/components/schemas/Pet"),TEXT("?rev=2#/Pet"));
 auto Analysis=FVaRestOpenApiLoader::Analyze(Root,Bytes(Input),Options);
 if (!TestTrue(TEXT("query-only URL references load"),Analysis.bDocumentValid)
  || !NoErrors(*this,Analysis.Issues,TEXT("query references"))) return false;
 TestEqual(TEXT("root identity is normalized once"),Analysis.Model.RootDocument,CanonicalRoot);
 TestTrue(TEXT("each query reference keeps the filename, including when nested"),Reads==TArray<FString>{Revision2,Revision3});
 TestEqual(TEXT("query variants are separate source documents"),Analysis.Model.SourceDocuments.Num(),3);
 TestTrue(TEXT("first revision document retained"),Analysis.Model.SourceDocuments.Contains(Revision2));
 TestTrue(TEXT("second revision document retained"),Analysis.Model.SourceDocuments.Contains(Revision3));
 TestEqual(TEXT("referenced operation survives"),Analysis.Model.Operations.Num(),1);
 TestEqual(TEXT("no failed operation"),Analysis.Model.FailedOperations.Num(),0);
 const auto Plan=FVaRestOpenApiPlanner::Build(Analysis);
 TestTrue(TEXT("resolved query graph produces a usable plan"),Plan.bValid);
 NoErrors(*this,Plan.Issues,TEXT("query plan"));

 for (const FString& Target:{FString(TEXT("?rev=2\r\nInjected:yes#/Pet")),FString(TEXT("../outside.json#/Pet")),
  FString(TEXT("https://other.invalid/team/spec.json#/Pet")),FString(TEXT("%2e%2e/outside.json#/Pet")),FString(TEXT("child%2Foutside.json#/Pet")),
  FString(TEXT("child\\u0000suffix.json#/Pet"))})
 {
  Reads.Reset();
  const FString EscapedTarget=Target.Replace(TEXT("\r"),TEXT("\\r")).Replace(TEXT("\n"),TEXT("\\n"));
  const FString Invalid=FString(Source).Replace(TEXT("#/components/schemas/Pet"),*EscapedTarget);
  const auto Refused=FVaRestOpenApiLoader::Analyze(Root,Bytes(Invalid),Options);
  TestEqual(TEXT("invalid URL reference is refused before the reader"),Reads.Num(),0);
  TestTrue(TEXT("invalid URL reference reports an issue"),Has(Refused.Issues,TEXT("OA-REF-OUTSIDE")) || Has(Refused.Issues,TEXT("OA-REF-REMOTE")));
 }
 Reads.Reset();
 const FString InvalidFragment=FString(Source).Replace(TEXT("#/components/schemas/Pet"),TEXT("#/components/schemas/Pet\\u0000/suffix"));
 const auto Fragment=FVaRestOpenApiLoader::Analyze(Root,Bytes(InvalidFragment),Options);
 TestEqual(TEXT("embedded null fragment performs no reader call"),Reads.Num(),0);
 TestTrue(TEXT("embedded null fragment refuses instead of matching the truncated existing pointer"),Has(Fragment.Issues,TEXT("OA-REF-UNRESOLVED")));
 TestEqual(TEXT("invalid fragment cannot silently import its operation"),Fragment.Model.FailedOperations.Num(),1);

 FVaRestOpenApiReadRequest Boundary;Boundary.bUrl=true;Boundary.Uri=CanonicalRoot;Boundary.RootBase=TEXT("https://specs.invalid/team/");
 for (const auto& Pair:TArray<TPair<FString,FString>>{
  {TEXT("?rev=2"),Revision2},{TEXT("#schema"),CanonicalRoot},
  {TEXT("nested/../next%20doc.json?rev=4#schema"),TEXT("https://specs.invalid/team/next%20doc.json?rev=4")},
  {TEXT("//SPECS.INVALID:443/team/other.json"),TEXT("https://specs.invalid/team/other.json")}})
 {
  FString Error;const auto Redirect=SourceRedirect(Boundary,CanonicalRoot,Pair.Key,Error);
  if (TestTrue(TEXT("confined redirect is admitted"),Redirect.IsSet())) TestEqual(TEXT("redirect canonical target"),Redirect.GetValue().Uri,Pair.Value);
  TestTrue(TEXT("confined redirect has no error"),Error.IsEmpty());
 }
 for (const FString& Target:{FString(TEXT("../outside.json")),FString(TEXT("/team-other/spec.json")),
  FString(TEXT("https://other.invalid/team/spec.json")),FString(TEXT("http://specs.invalid/team/spec.json")),
  FString(TEXT("https://user@specs.invalid/team/spec.json")),FString(TEXT("%2e%2e/outside.json")),
  FString(TEXT("child%2foutside.json")),FString(TEXT("?rev=2\r\nInjected:yes"))})
 {
  FString Error;const auto Redirect=SourceRedirect(Boundary,CanonicalRoot,Target,Error);
  TestFalse(TEXT("unconfined or malformed redirect is refused"),Redirect.IsSet());
  TestFalse(TEXT("refused redirect has a diagnostic"),Error.IsEmpty());
 }
 const TCHAR NullName[]=TEXT("next\0doc.json");
 const FString NullRedirect(UE_ARRAY_COUNT(NullName)-1,NullName);
 TestTrue(TEXT("raw redirect fixture contains its counted interior null and suffix"),NullRedirect.Len()==UE_ARRAY_COUNT(NullName)-1 && NullRedirect[4]==0 && NullRedirect.EndsWith(TEXT("doc.json")));
 FString NullError;const auto NullTarget=SourceRedirect(Boundary,CanonicalRoot,NullRedirect,NullError);
 TestFalse(TEXT("raw counted-null redirect refuses rather than fetching a shortened filename"),NullTarget.IsSet());
 TestFalse(TEXT("raw counted-null redirect gives a diagnostic"),NullError.IsEmpty());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiAssetsOfflineTest,"VaRest.OpenApi.Offline.Assets",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiAssetsOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 FOwnedAssets Owned;
 const FString Root=TEXT("/VaRestOpenApiQualification/root.json");
 FVaRestOpenApiPlanOptions PlanOptions; PlanOptions.ApiName=TEXT("O1Fixture");
 const auto Analysis=FVaRestOpenApiLoader::Analyze(Root,Bytes(Source));
 const auto Plan=FVaRestOpenApiPlanner::Build(Analysis,PlanOptions);
 if (!TestTrue(TEXT("valid source plan"),Plan.bValid) || !NoErrors(*this,Plan.Issues,TEXT("plan"))) return false;
 const FString Folder=TEXT("/Game/__VaRestO1Qualification_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("Api"))));
 FVaRestOpenApiApplyOptions Options; Options.Parent=Package.Get(); Options.ApiName=TEXT("Api"); Options.TypesFolder=Folder/TEXT("Types");
 Options.bRegisterAssets=false; Options.bAddToSettings=false;
 const auto Applied=FVaRestOpenApiApplier::Apply(Plan,Options); Owned.Keep(Applied);
 if (!NoErrors(*this,Applied.Issues,TEXT("apply")) || !TestNotNull(TEXT("actual API asset"),Applied.Api)) return false;
 auto* Api=Applied.Api;
 if (!TestEqual(TEXT("actual endpoint"),Api->Endpoints.Num(),1)) return false;
 const FGuid EndpointId=Api->Endpoints[0].Id;
 auto* Struct=Cast<UUserDefinedStruct>(Api->Endpoints[0].ResponseType.Type.Get());
 UUserDefinedEnum* Enum=nullptr;
 for (auto* Object:Applied.AdditionalObjects) if (auto* Found=Cast<UUserDefinedEnum>(Object)) Enum=Found;
 if (!TestNotNull(TEXT("native compiled response struct"),Struct) || !TestNotNull(TEXT("native generated enum"),Enum)) return false;
 TestEqual(TEXT("struct compiled"),Struct->Status,UDSS_UpToDate);
 TestFalse(TEXT("generated struct can be used in Blueprint variables"),Struct->HasAnyFlags(RF_Transient));
 TSet<FGuid> MemberIds; FGuid DefaultId;
 for (const auto& Member:FStructureEditorUtils::GetVarDesc(Struct)) { MemberIds.Add(Member.VarGuid); if (Member.DefaultValue.Contains(TEXT("original"))) DefaultId=Member.VarGuid; }
 TestTrue(TEXT("native string default applied"),DefaultId.IsValid());
 const auto InitialMembers=FStructureEditorUtils::GetVarDesc(Struct).Num();
 Api->Timeout=42; Api->Endpoints[0].Description=TEXT("User description");
 FString Updated=FString(Source).Replace(TEXT("\"default\":\"original\""),TEXT("\"default\":\"updated\""))
  .Replace(TEXT("Original summary"),TEXT("Upstream summary"));
 if (!TestTrue(TEXT("seed stable generated identities"),FVaRestOpenApiReimporter::SeedOptions(Api,PlanOptions))) return false;
 const auto NewPlan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(Root,Bytes(Updated)),PlanOptions);
 if (!TestTrue(TEXT("updated plan"),NewPlan.bValid) || !NoErrors(*this,NewPlan.Issues,TEXT("updated plan"))) return false;
 FVaRestOpenApiMergeOptions Automated; Automated.bAutomated=true;
 auto Preview=FVaRestOpenApiReimporter::Preview(Api,NewPlan,Automated);
 if (!TestTrue(TEXT("actual reimport preview"),Preview.bValid) || !NoErrors(*this,Preview.Issues,TEXT("preview"))) return false;
 TestTrue(TEXT("local/upstream description conflict detected"),Preview.Merge.Conflicts>0);
 const auto Changed=FVaRestOpenApiReimporter::Apply(Api,NewPlan,Preview,Options); Owned.Keep(Changed);
 if (!TestNotNull(TEXT("actual reimport applied"),Changed.Api) || !NoErrors(*this,Changed.Issues,TEXT("reimport"))) return false;
 TestEqual(TEXT("endpoint GUID remains stable"),Api->Endpoints[0].Id,EndpointId);
 TestEqual(TEXT("response struct is edited in place"),Api->Endpoints[0].ResponseType.Type.Get(),static_cast<UObject*>(Struct));
 TestEqual(TEXT("unowned API policy survives"),Api->Timeout,42.f);
 TestEqual(TEXT("conflicting user description survives"),Api->Endpoints[0].Description,FString(TEXT("User description")));
 TestEqual(TEXT("member count remains stable"),FStructureEditorUtils::GetVarDesc(Struct).Num(),InitialMembers);
 for (const auto& Member:FStructureEditorUtils::GetVarDesc(Struct)) TestTrue(TEXT("member GUID survives reimport"),MemberIds.Contains(Member.VarGuid));
 const auto* UpdatedMember=FStructureEditorUtils::GetVarDescByGuid(Struct,DefaultId);
 TestTrue(TEXT("independent upstream string default applied"),UpdatedMember && UpdatedMember->DefaultValue.Contains(TEXT("updated")));
 for (const auto& Object:Owned.Objects) if (Object.IsValid()) Object->GetOutermost()->SetDirtyFlag(false);
 auto NoopPreview=FVaRestOpenApiReimporter::Preview(Api,NewPlan,Automated);
 const auto Noop=FVaRestOpenApiReimporter::Apply(Api,NewPlan,NoopPreview,Options);
 if (!NoErrors(*this,Noop.Issues,TEXT("no-op"))) return false;
 TestTrue(TEXT("identical reimport is a no-op despite preserved local edit"),Noop.bNoChanges);
 TestFalse(TEXT("no-op does not dirty the API package"),Package->IsDirty());
 auto Stale=FVaRestOpenApiReimporter::Preview(Api,NewPlan,Automated);
 Api->Endpoints[0].Path=TEXT("/user-edit/{id}");
 const auto Refused=FVaRestOpenApiReimporter::Apply(Api,NewPlan,Stale,Options);
 TestNull(TEXT("stale preview refuses before mutation"),Refused.Api);
 TestTrue(TEXT("stale preview diagnostic"),Has(Refused.Issues,TEXT("OA-REIMPORT-STALE")));
 TestEqual(TEXT("stale apply retains the intervening user edit"),Api->Endpoints[0].Path,FString(TEXT("/user-edit/{id}")));
 return true;
}

namespace VaRestOpenApiOfflineTest
{
struct FSavedFixture
{
 FString MountRoot=TEXT("/VaRestO1Saved_")+FGuid::NewGuid().ToString(EGuidFormats::Digits)+TEXT("/");
 FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("VaRestOpenApiQualification")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
 FString ApiPath;
 TArray<FString> Packages,Objects;
 TMap<FString,TArray<uint8>> Files;
 FGuid EndpointId,DefaultId;
 TSet<FGuid> Members;
 FVaRestApiImportState ImportState;
 FSavedFixture()
 {
  const FString Root=MountRoot,Content=Directory+TEXT("/");
  FPackageName::RegisterMountPoint(Root,Content);
  // Save schedules editor validators. Retain native paths until their pre-exit boundary.
  FCoreDelegates::OnEnginePreExit.AddLambda([Root,Content] { FPackageName::UnRegisterMountPoint(Root,Content); });
 }
};
bool SaveFixture(FAutomationTestBase& Test,FSavedFixture& Saved)
{
 FOwnedAssets Owned;
 FVaRestOpenApiPlanOptions PlanOptions; PlanOptions.ApiName=TEXT("O1Saved");
 const auto Plan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/root.json"),Bytes(Source)),PlanOptions);
 if (!Test.TestTrue(TEXT("persistence source plan"),Plan.bValid) || !NoErrors(Test,Plan.Issues,TEXT("saved plan"))) return false;
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Saved.MountRoot+TEXT("Api"))));
 FVaRestOpenApiApplyOptions Options; Options.Parent=Package.Get(); Options.ApiName=TEXT("Api");
 Options.TypesFolder=Saved.MountRoot+TEXT("Types"); Options.bRegisterAssets=false; Options.bAddToSettings=false;
 const auto Applied=FVaRestOpenApiApplier::Apply(Plan,Options); Owned.Keep(Applied);
 if (!NoErrors(Test,Applied.Issues,TEXT("saved apply")) || !Test.TestNotNull(TEXT("API to serialize"),Applied.Api)) return false;
 auto* Api=Applied.Api;
 if (!Test.TestEqual(TEXT("saved endpoint count"),Api->Endpoints.Num(),1)) return false;
 auto* Struct=Cast<UUserDefinedStruct>(Api->Endpoints[0].ResponseType.Type.Get());
 if (!Test.TestNotNull(TEXT("saved native struct"),Struct)) return false;
 Saved.EndpointId=Api->Endpoints[0].Id; Saved.ApiPath=Api->GetPathName(); Saved.ImportState=Api->ImportState;
 if (!Test.TestTrue(TEXT("real import manifest has persistent bytes"),!Saved.ImportState.Blob.IsEmpty())) return false;
 for (const auto& Member:FStructureEditorUtils::GetVarDesc(Struct))
 {
  Saved.Members.Add(Member.VarGuid);
  if (Member.DefaultValue.Contains(TEXT("original"))) Saved.DefaultId=Member.VarGuid;
 }
 if (!Test.TestTrue(TEXT("saved default member identity"),Saved.DefaultId.IsValid())) return false;
 TArray<UObject*> Objects=Applied.AdditionalObjects; Objects.Add(Api);
 if (!Test.TestEqual(TEXT("three generated objects serialized"),Objects.Num(),3)) return false;
 for (auto* Object:Objects)
 {
  auto* ObjectPackage=Object->GetOutermost(); const FString Name=ObjectPackage->GetName();
  if (!Test.TestTrue(TEXT("save target belongs to this fixture mount"),Name.StartsWith(Saved.MountRoot))) return false;
  const FString Filename=FPackageName::LongPackageNameToFilename(Name,FPackageName::GetAssetPackageExtension());
  if (!Test.TestFalse(TEXT("never overwrite an existing fixture file"),IFileManager::Get().FileExists(*Filename)) ||
   !Test.TestTrue(TEXT("create only owned artifact directories"),IFileManager::Get().MakeDirectory(*FPaths::GetPath(Filename),true))) return false;
  FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
  if (!Test.TestTrue(TEXT("save actual generated native asset"),UPackage::SavePackage(ObjectPackage,Object,*Filename,Args))) return false;
  TArray<uint8> BytesOnDisk;
  if (!Test.TestTrue(TEXT("read actual saved asset bytes"),FFileHelper::LoadFileToArray(BytesOnDisk,*Filename)) ||
   !Test.TestTrue(TEXT("saved asset is nonempty"),!BytesOnDisk.IsEmpty())) return false;
  Saved.Packages.Add(Name); Saved.Objects.Add(Object->GetPathName()); Saved.Files.Add(Filename,MoveTemp(BytesOnDisk));
 }
 Test.AddInfo(TEXT("OpenApiSavedFixture ")+Saved.Directory);
 return true; // all strong asset/package references leave before Owned unregisters rules and collects
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiPersistenceOfflineTest,"VaRest.OpenApi.Offline.Persistence",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiPersistenceOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 FSavedFixture Saved;
 if (!SaveFixture(*this,Saved)) return false;
 UObject* Retained=nullptr;
 for (const auto& Path:Saved.Objects) if (auto* Object=FindObject<UObject>(nullptr,*Path))
 {
  AddInfo(FString::Printf(TEXT("Retained fixture object %s flags=%x rooted=%d"),*Path,uint32(Object->GetFlags()),Object->IsRooted()));
  if (!Retained) Retained=Object;
 }
 for (const auto& Name:Saved.Packages) if (auto* Package=FindPackage(nullptr,*Name))
 {
  AddInfo(FString::Printf(TEXT("Retained fixture package %s flags=%x rooted=%d linker=%d"),*Name,uint32(Package->GetFlags()),Package->IsRooted(),Package->GetLinker()!=nullptr));
  if (!Retained) Retained=Package;
 }
 if (Retained) { FReferenceChainSearch Search(Retained,EReferenceChainSearchMode::Shortest|EReferenceChainSearchMode::PrintResults); }
 for (const auto& Name:Saved.Packages)
  if (!TestNull(TEXT("saved package really leaves memory before disk reload"),FindPackage(nullptr,*Name))) return false;
 FOwnedAssets Owned;
 TStrongObjectPtr<UVaRestApi> Api(LoadObject<UVaRestApi>(nullptr,*Saved.ApiPath));
 if (!TestNotNull(TEXT("load generated API from disk"),Api.Get())) return false;
 Owned.Api=Api.Get(); Owned.Objects.Emplace(Api.Get());
 for (const auto& Path:Saved.Objects)
 {
  auto* Object=LoadObject<UObject>(nullptr,*Path);
  if (!TestNotNull(TEXT("reload every generated type from disk"),Object)) return false;
  if (Object!=Api.Get()) Owned.Objects.Emplace(Object);
 }
 TestEqual(TEXT("importer identity serializes"),Api->ImportState.Importer,Saved.ImportState.Importer);
 TestEqual(TEXT("manifest version serializes"),Api->ImportState.Version,Saved.ImportState.Version);
 TestTrue(TEXT("manifest bytes serialize exactly"),Api->ImportState.Blob==Saved.ImportState.Blob);
 if (!TestEqual(TEXT("reloaded endpoint count"),Api->Endpoints.Num(),1)) return false;
 TestEqual(TEXT("endpoint GUID serializes"),Api->Endpoints[0].Id,Saved.EndpointId);
 auto* Struct=Cast<UUserDefinedStruct>(Api->Endpoints[0].ResponseType.Type.Get());
 if (!TestNotNull(TEXT("reloaded response retains real struct reference"),Struct)) return false;
 TestEqual(TEXT("reloaded struct remains compiled"),Struct->Status,UDSS_UpToDate);
 TestEqual(TEXT("member count serializes"),FStructureEditorUtils::GetVarDesc(Struct).Num(),Saved.Members.Num());
 for (const auto& Member:FStructureEditorUtils::GetVarDesc(Struct)) TestTrue(TEXT("member GUID serializes"),Saved.Members.Contains(Member.VarGuid));
 const auto* Before=FStructureEditorUtils::GetVarDescByGuid(Struct,Saved.DefaultId);
 TestTrue(TEXT("generated string default serializes"),Before && Before->DefaultValue.Contains(TEXT("original")));
 FVaRestOpenApiPlanOptions PlanOptions;
 if (!TestTrue(TEXT("disk-loaded manifest seeds stable type and endpoint identities"),FVaRestOpenApiReimporter::SeedOptions(Api.Get(),PlanOptions))) return false;
 const auto Plan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/root.json"),Bytes(Source)),PlanOptions);
 FVaRestOpenApiMergeOptions Automated; Automated.bAutomated=true;
 auto Preview=FVaRestOpenApiReimporter::Preview(Api.Get(),Plan,Automated);
 if (!TestTrue(TEXT("disk-loaded manifest supports preview"),Preview.bValid) || !NoErrors(*this,Preview.Issues,TEXT("loaded preview"))) return false;
 FVaRestOpenApiApplyOptions Options; Options.Parent=Api->GetOutermost(); Options.ApiName=Api->GetFName();
 Options.TypesFolder=Saved.MountRoot+TEXT("Types"); Options.bRegisterAssets=false; Options.bAddToSettings=false;
 const auto Noop=FVaRestOpenApiReimporter::Apply(Api.Get(),Plan,Preview,Options);
 if (!NoErrors(*this,Noop.Issues,TEXT("loaded no-op"))) return false;
 TestTrue(TEXT("disk-loaded identical reimport is a no-op"),Noop.bNoChanges && Noop.Api==Api.Get());
 TestFalse(TEXT("loaded no-op leaves package clean"),Api->GetOutermost()->IsDirty());
 for (const auto& Pair:Saved.Files)
 {
  TArray<uint8> Current;
  TestTrue(TEXT("saved asset still readable"),FFileHelper::LoadFileToArray(Current,*Pair.Key));
  TestTrue(TEXT("reload and no-op never rewrite saved files"),Current==Pair.Value);
 }
 const FString Updated=FString(Source).Replace(TEXT("\"default\":\"original\""),TEXT("\"default\":\"after-reload\""));
 const auto UpdatedPlan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/root.json"),Bytes(Updated)),PlanOptions);
 auto UpdatePreview=FVaRestOpenApiReimporter::Preview(Api.Get(),UpdatedPlan,Automated);
 if (!TestTrue(TEXT("loaded asset supports upstream update preview"),UpdatePreview.bValid)) return false;
 const auto Changed=FVaRestOpenApiReimporter::Apply(Api.Get(),UpdatedPlan,UpdatePreview,Options); Owned.Keep(Changed);
 if (!NoErrors(*this,Changed.Issues,TEXT("loaded update")) || !TestNotNull(TEXT("reimport loaded asset"),Changed.Api)) return false;
 TestEqual(TEXT("loaded reimport retains endpoint identity"),Api->Endpoints[0].Id,Saved.EndpointId);
 TestEqual(TEXT("loaded reimport edits the existing struct"),Api->Endpoints[0].ResponseType.Type.Get(),static_cast<UObject*>(Struct));
 const auto* After=FStructureEditorUtils::GetVarDescByGuid(Struct,Saved.DefaultId);
 TestTrue(TEXT("upstream default updates through persisted manifest"),After && After->DefaultValue.Contains(TEXT("after-reload")));
 for (const auto& Member:FStructureEditorUtils::GetVarDesc(Struct)) TestTrue(TEXT("loaded reimport retains member GUID"),Saved.Members.Contains(Member.VarGuid));
 return true;
}

namespace VaRestOpenApiOfflineTest
{
FVaRestJson SchemaResponse(const FVaRestJson& Schema)
{
 return FVaRestJson::Object().Set(TEXT("description"),TEXT("payload"))
  .Set(TEXT("content"),FVaRestJson::Object().Set(TEXT("application/json"),FVaRestJson::Object().Set(TEXT("schema"),Schema)));
}
FVaRestJson PropertySchema(const FString& Name,bool bInteger=false)
{
 auto Value=FVaRestJson::Object().Set(TEXT("type"),bInteger ? TEXT("integer") : TEXT("string"));
 if (bInteger) Value.Set(TEXT("format"),TEXT("int32"));
 return FVaRestJson::Object().Set(TEXT("type"),TEXT("object")).Set(TEXT("properties"),FVaRestJson::Object().Set(Name,Value));
}
FVaRestOpenApiPlan ShapePlan(const FVaRestJson& A,const FVaRestJson& B,bool bErrors,const FVaRestJson& Schemas={})
{
 auto Responses=FVaRestJson::Object();
 if (bErrors) Responses.Set(TEXT("200"),SchemaResponse(FVaRestJson::Object().Set(TEXT("type"),TEXT("string"))))
  .Set(TEXT("400"),SchemaResponse(A)).Set(TEXT("404"),SchemaResponse(B));
 else Responses.Set(TEXT("200"),SchemaResponse(A)).Set(TEXT("201"),SchemaResponse(B));
 auto Operation=FVaRestJson::Object().Set(TEXT("operationId"),TEXT("shapeCase")).Set(TEXT("responses"),Responses);
 auto Document=FVaRestJson::Object().Set(TEXT("openapi"),TEXT("3.0.3"))
  .Set(TEXT("info"),FVaRestJson::Object().Set(TEXT("title"),TEXT("Shapes")).Set(TEXT("version"),TEXT("1")))
  .Set(TEXT("servers"),FVaRestJson::Array(FVaRestJson::Object().Set(TEXT("url"),TEXT("https://shapes.invalid"))))
  .Set(TEXT("paths"),FVaRestJson::Object().Set(TEXT("/shape"),FVaRestJson::Object().Set(TEXT("get"),Operation)));
 if (Schemas.IsObject()) Document.Set(TEXT("components"),FVaRestJson::Object().Set(TEXT("schemas"),Schemas));
 FVaRestOpenApiPlanOptions Options; Options.bDetectEnvelope=false;
 return FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/shapes.json"),Bytes(Document.ToString())),Options);
}
bool CheckShapes(FAutomationTestBase& Test,const FVaRestJson& A,const FVaRestJson& B,bool bDifferent,const FVaRestJson& Schemas={})
{
 for (const bool bErrors:{false,true})
 {
  const auto Plan=ShapePlan(A,B,bErrors,Schemas);
  if (!Test.TestTrue(TEXT("real shape source produces a valid public plan"),Plan.bValid) || !NoErrors(Test,Plan.Issues,TEXT("shape plan"))
   || !Test.TestEqual(TEXT("shape fixture has one actual endpoint"),Plan.Endpoints.Num(),1)) return false;
  const auto& Endpoint=Plan.Endpoints[0];
  if (bErrors)
  {
   Test.TestEqual(TEXT("error shape ambiguity has the actual public issue"),Has(Plan.Issues,TEXT("OA-RESP-ERRORS")),bDifferent);
   Test.TestEqual(TEXT("only equivalent errors retain a typed error output"),Endpoint.ErrorType.Kind,bDifferent ? EVaRestOpenApiValueKind::None : EVaRestOpenApiValueKind::Struct);
  }
  else
  {
   Test.TestEqual(TEXT("success shape ambiguity has the actual public issue"),Has(Plan.Issues,TEXT("OA-RESP-MULTI2XX")),bDifferent);
   Test.TestEqual(TEXT("actual success mixed-shape flag"),Endpoint.bMixedSuccessShapes,bDifferent);
   Test.TestEqual(TEXT("only equivalent success bodies retain a typed output"),Endpoint.ResponseType.Kind,bDifferent ? EVaRestOpenApiValueKind::Json : EVaRestOpenApiValueKind::Struct);
  }
 }
 return true;
}
FString ParameterDocument(bool bRequired,const TCHAR* Default,const TCHAR* Path)
{
 auto Param=FVaRestJson::Object().Set(TEXT("name"),TEXT("q")).Set(TEXT("in"),TEXT("query")).Set(TEXT("required"),bRequired)
  .Set(TEXT("schema"),FVaRestJson::Object().Set(TEXT("type"),TEXT("string")).Set(TEXT("default"),Default));
 auto Operation=FVaRestJson::Object().Set(TEXT("operationId"),TEXT("lookup"))
  .Set(TEXT("parameters"),FVaRestJson::Array(Param))
  .Set(TEXT("responses"),FVaRestJson::Object().Set(TEXT("200"),SchemaResponse(FVaRestJson::Object().Set(TEXT("type"),TEXT("string")))));
 return FVaRestJson::Object().Set(TEXT("openapi"),TEXT("3.0.3"))
  .Set(TEXT("info"),FVaRestJson::Object().Set(TEXT("title"),TEXT("Requiredness")).Set(TEXT("version"),TEXT("1")))
  .Set(TEXT("servers"),FVaRestJson::Array(FVaRestJson::Object().Set(TEXT("url"),TEXT("https://requiredness.invalid"))))
  .Set(TEXT("paths"),FVaRestJson::Object().Set(Path,FVaRestJson::Object().Set(TEXT("get"),Operation))).ToString();
}
bool RequirednessCase(FAutomationTestBase& Test,bool bAllowBreaking)
{
 FOwnedAssets Owned;
 const FString Root=TEXT("/VaRestOpenApiQualification/requiredness.json");
 FVaRestOpenApiPlanOptions PlanOptions; PlanOptions.ApiName=TEXT("RequirednessFixture"); PlanOptions.bDetectEnvelope=false;
 const auto InitialPlan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(Root,Bytes(ParameterDocument(false,TEXT("original"),TEXT("/lookup")))),PlanOptions);
 if (!Test.TestTrue(TEXT("optional parameter source plan"),InitialPlan.bValid) || !NoErrors(Test,InitialPlan.Issues,TEXT("optional plan"))
  || !Test.TestEqual(TEXT("optional plan endpoint"),InitialPlan.Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("optional plan parameter"),InitialPlan.Endpoints[0].Parameters.Num(),1)) return false;
 const FString Folder=TEXT("/Game/__VaRestRequiredness_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("Api"))));
 FVaRestOpenApiApplyOptions Options; Options.Parent=Package.Get(); Options.ApiName=TEXT("Api"); Options.TypesFolder=Folder/TEXT("Types");
 Options.bRegisterAssets=false; Options.bAddToSettings=false;
 const auto Initial=FVaRestOpenApiApplier::Apply(InitialPlan,Options); Owned.Keep(Initial);
 if (!NoErrors(Test,Initial.Issues,TEXT("optional apply")) || !Test.TestNotNull(TEXT("actual optional API asset"),Initial.Api)) return false;
 auto* Api=Initial.Api;
 if (!Test.TestEqual(TEXT("one actual optional endpoint"),Api->Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("one actual optional parameter"),Api->Endpoints[0].Params.Num(),1)) return false;
 const FGuid EndpointId=Api->Endpoints[0].Id; const FName Pin=Api->Endpoints[0].Params[0].PinName;
 const FString OriginalDefault=Api->Endpoints[0].Params[0].Default;
 const FVaRestTypeRef OriginalType=Api->Endpoints[0].Params[0].Type;
 Test.TestFalse(TEXT("initial imported parameter is optional"),Api->Endpoints[0].Params[0].bRequired);
 Test.TestEqual(TEXT("initial imported parameter is String"),OriginalType.Kind,EVaRestTypeKind::String);
 Test.TestEqual(TEXT("initial default is the real planned native literal"),OriginalDefault,InitialPlan.Endpoints[0].Parameters[0].DefaultText);
 if (!Test.TestTrue(TEXT("seed actual endpoint identity before changed route"),FVaRestOpenApiReimporter::SeedOptions(Api,PlanOptions))) return false;
 const auto RequiredPlan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(Root,Bytes(ParameterDocument(true,TEXT("upstream"),TEXT("/lookup-v2")))),PlanOptions);
 if (!Test.TestTrue(TEXT("required parameter source plan"),RequiredPlan.bValid) || !NoErrors(Test,RequiredPlan.Issues,TEXT("required plan"))
  || !Test.TestEqual(TEXT("required plan endpoint"),RequiredPlan.Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("required plan parameter"),RequiredPlan.Endpoints[0].Parameters.Num(),1)) return false;
 const FString ParamId=RequiredPlan.Endpoints[0].Parameters[0].Id;
 FVaRestOpenApiMergeOptions Automated; Automated.bAutomated=true;
 auto Deferred=FVaRestOpenApiReimporter::Preview(Api,RequiredPlan,Automated);
 if (!Test.TestTrue(TEXT("actual automated requiredness preview"),Deferred.bValid) || !NoErrors(Test,Deferred.Issues,TEXT("deferred preview"))) return false;
 const auto Find=[&](const FVaRestOpenApiReimportPreview& Preview,const FString& Id,const TCHAR* Property) {
  return Preview.Merge.Changes.FindByPredicate([&](const auto& Row) { return Row.Id==Id && Row.Property==Property; });
 };
 const auto* Required=Find(Deferred,ParamId,TEXT("required"));
 if (!Test.TestNotNull(TEXT("existing requiredness change is present"),Required)) return false;
 Test.TestTrue(TEXT("optional to required is breaking and deferred automatically"),Required->bBreaking && Required->bDeferred && !Required->bApply);
 const auto* Default=Find(Deferred,ParamId,TEXT("defaultText"));
 const auto* Path=Find(Deferred,RequiredPlan.Endpoints[0].Id,TEXT("path"));
 Test.TestTrue(TEXT("blocked endpoint also defers parameter default companion"),Default && Default->bDeferred && !Default->bApply);
 Test.TestTrue(TEXT("blocked endpoint also defers runtime route companion"),Path && Path->bDeferred && !Path->bApply);
 const auto Held=FVaRestOpenApiReimporter::Apply(Api,RequiredPlan,Deferred,Options); Owned.Keep(Held);
 if (!NoErrors(Test,Held.Issues,TEXT("deferred apply")) || !Test.TestEqual(TEXT("deferred apply keeps the actual asset"),Held.Api,Api)
  || !Test.TestEqual(TEXT("deferred apply retains one endpoint"),Api->Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("deferred apply retains one parameter"),Api->Endpoints[0].Params.Num(),1)) return false;
 Test.TestFalse(TEXT("automated refused upgrade leaves the actual parameter optional"),Api->Endpoints[0].Params[0].bRequired);
 Test.TestEqual(TEXT("automated refused upgrade preserves native default"),Api->Endpoints[0].Params[0].Default,OriginalDefault);
 Test.TestEqual(TEXT("automated refused upgrade preserves route"),Api->Endpoints[0].Path,FString(TEXT("/lookup")));
 Test.TestEqual(TEXT("automated refused upgrade preserves parameter type"),Api->Endpoints[0].Params[0].Type.Kind,OriginalType.Kind);
 Test.TestEqual(TEXT("automated refused upgrade preserves parameter container"),Api->Endpoints[0].Params[0].Type.Container,OriginalType.Container);
 Test.TestEqual(TEXT("automated refused upgrade preserves parameter type object"),Api->Endpoints[0].Params[0].Type.Type.Get(),OriginalType.Type.Get());
 Test.TestEqual(TEXT("automated refused upgrade preserves JSON-value mode"),Api->Endpoints[0].Params[0].Type.bJsonValue,OriginalType.bJsonValue);
 Test.TestEqual(TEXT("automated refused upgrade preserves parameter pin"),Api->Endpoints[0].Params[0].PinName,Pin);
 Test.TestEqual(TEXT("automated refused upgrade preserves endpoint GUID"),Api->Endpoints[0].Id,EndpointId);
 auto Again=FVaRestOpenApiReimporter::Preview(Api,RequiredPlan,Automated);
 if (!Test.TestTrue(TEXT("deferred base supports a later real preview"),Again.bValid)) return false;
 const auto* StillRequired=Find(Again,ParamId,TEXT("required"));
 Test.TestTrue(TEXT("deferred requiredness remains pending on repeated automated preview"),StillRequired && StillRequired->bBreaking && StillRequired->bDeferred);
 FVaRestOpenApiMergeOptions Allowed; Allowed.bAutomated=bAllowBreaking; Allowed.bAllowBreaking=bAllowBreaking;
 auto Accepted=FVaRestOpenApiReimporter::Preview(Api,RequiredPlan,Allowed);
 if (!Test.TestTrue(TEXT("actual allowed-policy preview"),Accepted.bValid) || !NoErrors(Test,Accepted.Issues,TEXT("allowed preview"))) return false;
 const auto* AcceptedRequired=Find(Accepted,ParamId,TEXT("required"));
 Test.TestTrue(bAllowBreaking ? TEXT("AllowBreaking admits existing requiredness upgrade") : TEXT("interactive policy admits existing requiredness upgrade"),
  AcceptedRequired && AcceptedRequired->bBreaking && AcceptedRequired->bApply && !AcceptedRequired->bDeferred);
 const auto Changed=FVaRestOpenApiReimporter::Apply(Api,RequiredPlan,Accepted,Options); Owned.Keep(Changed);
 if (!NoErrors(Test,Changed.Issues,TEXT("allowed apply")) || !Test.TestEqual(TEXT("allowed apply keeps asset identity"),Changed.Api,Api)
  || !Test.TestEqual(TEXT("allowed apply retains one endpoint"),Api->Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("allowed apply retains one parameter"),Api->Endpoints[0].Params.Num(),1)) return false;
 Test.TestTrue(TEXT("allowed upgrade actually sets requiredness"),Api->Endpoints[0].Params[0].bRequired);
 Test.TestEqual(TEXT("allowed upgrade applies native default companion"),Api->Endpoints[0].Params[0].Default,RequiredPlan.Endpoints[0].Parameters[0].DefaultText);
 Test.TestEqual(TEXT("allowed upgrade applies route companion"),Api->Endpoints[0].Path,FString(TEXT("/lookup-v2")));
 Test.TestEqual(TEXT("allowed upgrade preserves type"),Api->Endpoints[0].Params[0].Type.Kind,OriginalType.Kind);
 Test.TestEqual(TEXT("allowed upgrade preserves container"),Api->Endpoints[0].Params[0].Type.Container,OriginalType.Container);
 Test.TestEqual(TEXT("allowed upgrade preserves type object"),Api->Endpoints[0].Params[0].Type.Type.Get(),OriginalType.Type.Get());
 Test.TestEqual(TEXT("allowed upgrade preserves JSON-value mode"),Api->Endpoints[0].Params[0].Type.bJsonValue,OriginalType.bJsonValue);
 Test.TestEqual(TEXT("allowed upgrade preserves pin"),Api->Endpoints[0].Params[0].PinName,Pin);
 Test.TestEqual(TEXT("allowed upgrade preserves endpoint GUID"),Api->Endpoints[0].Id,EndpointId);
 if (!Test.TestTrue(TEXT("seed before required-to-optional relaxation"),FVaRestOpenApiReimporter::SeedOptions(Api,PlanOptions))) return false;
 const auto RelaxedPlan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(Root,Bytes(ParameterDocument(false,TEXT("relaxed"),TEXT("/lookup-v2")))),PlanOptions);
 if (!Test.TestTrue(TEXT("relaxed parameter plan"),RelaxedPlan.bValid) || !NoErrors(Test,RelaxedPlan.Issues,TEXT("relaxed plan"))
  || !Test.TestEqual(TEXT("relaxed plan endpoint"),RelaxedPlan.Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("relaxed plan parameter"),RelaxedPlan.Endpoints[0].Parameters.Num(),1)) return false;
 auto Relaxed=FVaRestOpenApiReimporter::Preview(Api,RelaxedPlan,Automated);
 if (!Test.TestTrue(TEXT("actual automated relaxation preview"),Relaxed.bValid) || !NoErrors(Test,Relaxed.Issues,TEXT("relaxed preview"))) return false;
 const auto* Relax=Find(Relaxed,ParamId,TEXT("required"));
 Test.TestTrue(TEXT("required to optional is nonbreaking and automatically applied"),Relax && !Relax->bBreaking && Relax->bApply && !Relax->bDeferred);
 const auto RelaxedResult=FVaRestOpenApiReimporter::Apply(Api,RelaxedPlan,Relaxed,Options); Owned.Keep(RelaxedResult);
 if (!NoErrors(Test,RelaxedResult.Issues,TEXT("relaxed apply")) || !Test.TestEqual(TEXT("relaxation keeps asset identity"),RelaxedResult.Api,Api)
  || !Test.TestEqual(TEXT("relaxed apply retains one endpoint"),Api->Endpoints.Num(),1)
  || !Test.TestEqual(TEXT("relaxed apply retains one parameter"),Api->Endpoints[0].Params.Num(),1)) return false;
 Test.TestFalse(TEXT("real relaxed parameter becomes optional"),Api->Endpoints[0].Params[0].bRequired);
 Test.TestEqual(TEXT("relaxation applies nonblocked default"),Api->Endpoints[0].Params[0].Default,RelaxedPlan.Endpoints[0].Parameters[0].DefaultText);
 Test.TestEqual(TEXT("relaxation preserves endpoint GUID"),Api->Endpoints[0].Id,EndpointId);
 return true;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiShapesOfflineTest,"VaRest.OpenApi.Offline.ShapeEquivalence",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiShapesOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 for (const FString& Name:TArray<FString>{TEXT("description"),TEXT("example"),TEXT("examples")})
 {
  if (!CheckShapes(*this,PropertySchema(Name),PropertySchema(Name,true),true)) return false;
  auto A=PropertySchema(TEXT("code"),true).Set(TEXT("default"),FVaRestJson::Object().Set(Name,1));
  auto B=PropertySchema(TEXT("code"),true).Set(TEXT("default"),FVaRestJson::Object().Set(Name,2));
  if (!CheckShapes(*this,A,B,true)) return false;
 }
 auto A=PropertySchema(TEXT("description")).Set(TEXT("description"),TEXT("root before"))
  .Set(TEXT("example"),FVaRestJson::Object().Set(TEXT("description"),TEXT("before"))).Set(TEXT("examples"),FVaRestJson::Array(TEXT("before")));
 auto B=PropertySchema(TEXT("description")).Set(TEXT("description"),TEXT("root after"))
  .Set(TEXT("example"),FVaRestJson::Object().Set(TEXT("description"),TEXT("after"))).Set(TEXT("examples"),FVaRestJson::Array(TEXT("after")));
 A[TEXT("properties")][TEXT("description")].Set(TEXT("description"),TEXT("child before")).Set(TEXT("example"),TEXT("before")).Set(TEXT("examples"),FVaRestJson::Array(TEXT("before")));
 B[TEXT("properties")][TEXT("description")].Set(TEXT("description"),TEXT("child after")).Set(TEXT("example"),TEXT("after")).Set(TEXT("examples"),FVaRestJson::Array(TEXT("after")));
 if (!CheckShapes(*this,A,B,false)) return false;
 const auto Schemas=FVaRestJson::Object().Set(TEXT("Shared"),PropertySchema(TEXT("description")));
 auto RefA=FVaRestJson::Object().Set(TEXT("$ref"),TEXT("#/components/schemas/Shared")).Set(TEXT("description"),TEXT("reference before"))
  .Set(TEXT("example"),FVaRestJson::Object().Set(TEXT("description"),TEXT("before"))).Set(TEXT("examples"),FVaRestJson::Array(TEXT("before")));
 auto RefB=FVaRestJson::Object().Set(TEXT("$ref"),TEXT("#/components/schemas/Shared")).Set(TEXT("description"),TEXT("reference after"))
  .Set(TEXT("example"),FVaRestJson::Object().Set(TEXT("description"),TEXT("after"))).Set(TEXT("examples"),FVaRestJson::Array(TEXT("after")));
 return CheckShapes(*this,RefA,RefB,false,Schemas);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiRequirednessOfflineTest,"VaRest.OpenApi.Offline.Requiredness",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiRequirednessOfflineTest::RunTest(const FString& Parameters)
{
 return VaRestOpenApiOfflineTest::RequirednessCase(*this,true) && VaRestOpenApiOfflineTest::RequirednessCase(*this,false);
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiSourceNoopOfflineTest,"VaRest.OpenApi.Offline.SourceNoop",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiSourceNoopOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 using namespace VaRest::OpenApi;
 FOwnedAssets Owned;
 const FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("VaRestOpenApiQualification")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
 const FString Filename=Directory/TEXT("source-noop.json");
 if (!TestFalse(TEXT("source fixture never overwrites a preexisting file"),IFileManager::Get().FileExists(*Filename)) ||
  !TestTrue(TEXT("create owned source fixture directory"),IFileManager::Get().MakeDirectory(*Directory,true))) return false;
 ON_SCOPE_EXIT { IFileManager::Get().Delete(*Filename,false,true); IFileManager::Get().DeleteDirectory(*Directory,false,false); };
 if (!TestTrue(TEXT("write actual initial source file"),FFileHelper::SaveStringToFile(Source,*Filename))) return false;
 const auto Loaded=AnalyzeLocalFile(Filename,false);
 if (!TestTrue(TEXT("real source reader accepts fixture"),Loaded.Analysis.bDocumentValid) || !NoErrors(*this,Loaded.Analysis.Issues,TEXT("source load"))) return false;
 TStrongObjectPtr<UVaRestOpenApiImportOptions> ImportOptions(NewObject<UVaRestOpenApiImportOptions>());
 if (!TestTrue(TEXT("fixture options are independent of user config"),ImportOptions->FromJson(FVaRestJson::Object()))) return false;
 ImportOptions->bImportAllOperations=false; ImportOptions->bIncludeNewOperations=false;
 ImportOptions->SelectedOperations={TEXT("GET /pets/{id}")};
 const FString Folder=TEXT("/Game/__VaRestSourceNoop_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("Api"))));
 FVaRestOpenApiApplyOptions Options; Options.Parent=Package.Get(); Options.ApiName=TEXT("Api"); Options.TypesFolder=Folder/TEXT("StoredTypes");
 Options.bRegisterAssets=false; Options.bAddToSettings=false; Options.Source=Loaded.Source;
 Options.Options=ImportOptions->ToJson().Set(TEXT("knownOperations"),OperationRecords(Loaded.Analysis));
 auto PlanOptions=ImportOptions->PlanOptions(TEXT("Api"));
 const auto Plan=FVaRestOpenApiPlanner::Build(Loaded.Analysis,PlanOptions);
 const auto Applied=FVaRestOpenApiApplier::Apply(Plan,Options); Owned.Keep(Applied);
 if (!NoErrors(*this,Applied.Issues,TEXT("source initial apply")) || !TestNotNull(TEXT("actual source-backed API"),Applied.Api)) return false;
 auto* Api=Applied.Api;
 if (!TestEqual(TEXT("one selected initial endpoint"),Api->Endpoints.Num(),1) ||
  !TestEqual(TEXT("fixture creates real struct and enum packages"),Applied.AdditionalObjects.Num(),2)) return false;
 const FGuid EndpointId=Api->Endpoints[0].Id;
 TArray<TPair<UPackage*,uint32>> PackageFlags;
 for (const auto& Object:Owned.Objects) if (Object.IsValid())
 {
  auto* ObjectPackage=Object->GetOutermost(); ObjectPackage->SetDirtyFlag(false);
  PackageFlags.Emplace(ObjectPackage,uint32(ObjectPackage->GetPackageFlags()));
 }
 const FVaRestApiImportState InitialState=Api->ImportState;
 const FString Extra=FString(Source).Replace(TEXT("\"paths\":{"),TEXT("\"paths\":{\"/ignored\":{\"get\":{\"operationId\":\"ignored\",\"responses\":{\"200\":{\"description\":\"ok\"}}}},"));
 const auto ExtraLoaded=FVaRestOpenApiLoader::Analyze(Filename,Bytes(Extra));
 if (!TestTrue(TEXT("ignored operation addition is genuinely valid"),ExtraLoaded.bDocumentValid) ||
  !TestEqual(TEXT("upstream now has two real operations"),ExtraLoaded.Model.Operations.Num(),2) ||
  !TestTrue(TEXT("write source containing only an unselected operation addition"),FFileHelper::SaveStringToFile(Extra,*Filename))) return false;
 const auto Unchanged=[&]()
 {
  TestEqual(TEXT("no-op preserves importer identity"),Api->ImportState.Importer,InitialState.Importer);
  TestEqual(TEXT("no-op preserves manifest version"),Api->ImportState.Version,InitialState.Version);
  TestTrue(TEXT("no-op preserves every manifest byte including source hash and known operations"),Api->ImportState.Blob==InitialState.Blob);
  for (const auto& Entry:PackageFlags)
  {
   TestFalse(TEXT("no-op leaves every generated package clean"),Entry.Key->IsDirty());
   TestEqual(TEXT("no-op preserves all generated package flags"),uint32(Entry.Key->GetPackageFlags()),Entry.Value);
  }
 };
 const auto Checked=FVaRestOpenApiReimporter::FromSource(Api,true,false,true);
 if (!NoErrors(*this,Checked.Issues,TEXT("source check")) || !TestTrue(TEXT("actual source Check reports no drift"),Checked.Api==Api && Checked.bNoChanges)) return false;
 Unchanged();
 for (int32 Pass=0;Pass<2;++Pass)
 {
  const auto Noop=FVaRestOpenApiReimporter::FromSource(Api,true);
  if (!NoErrors(*this,Noop.Issues,TEXT("source no-op")) || !TestTrue(TEXT("actual source Apply remains a no-op"),Noop.Api==Api && Noop.bNoChanges)) return false;
  TestTrue(TEXT("source no-op reports no changed packages or new assets"),Noop.ChangedPackages.IsEmpty() && Noop.AdditionalObjects.IsEmpty()); Unchanged();
 }
 // An explicit metadata option edit must still commit through the public Apply path.
 if (!TestTrue(TEXT("seed current identities for explicit option control"),FVaRestOpenApiReimporter::SeedOptions(Api,PlanOptions))) return false;
 const auto SamePlan=FVaRestOpenApiPlanner::Build(ExtraLoaded,PlanOptions);
 FVaRestOpenApiMergeOptions Automated; Automated.bAutomated=true;
 auto OptionPreview=FVaRestOpenApiReimporter::Preview(Api,SamePlan,Automated);
 if (!TestTrue(TEXT("explicit option control starts from a valid preview"),OptionPreview.bValid) ||
  !TestFalse(TEXT("explicit option control has no content writes"),OptionPreview.Merge.HasWrites()) ||
  !TestTrue(TEXT("explicit option control has no base changes"),FVaRestOpenApiMerge::ItemsJson(OptionPreview.Merge.NextBase(OptionPreview.Base,OptionPreview.Theirs)).Equals(OptionPreview.Manifest[TEXT("items")]))) return false;
 auto ExplicitOptions=Options; ExplicitOptions.Options=Options.Options.Clone().Set(TEXT("allowOutside"),true);
 const auto OptionChanged=FVaRestOpenApiReimporter::Apply(Api,SamePlan,OptionPreview,ExplicitOptions); Owned.Keep(OptionChanged);
 if (!NoErrors(*this,OptionChanged.Issues,TEXT("explicit option apply")) || !TestTrue(TEXT("explicit option edit is a real commit"),OptionChanged.Api==Api && !OptionChanged.bNoChanges)) return false;
 TestTrue(TEXT("explicit option edit changes actual import state"),Api->ImportState.Blob!=InitialState.Blob);
 TestTrue(TEXT("explicit option edit dirties the actual API package"),Package->IsDirty());
 const auto OptionState=Api->ImportState;
 const auto AfterOption=FVaRestOpenApiReimporter::FromSource(Api,true);
 if (!NoErrors(*this,AfterOption.Issues,TEXT("source after option edit")) || !TestTrue(TEXT("source wrapper respects committed options without discovery churn"),AfterOption.Api==Api && AfterOption.bNoChanges)) return false;
 TestTrue(TEXT("option control state survives the next source no-op"),Api->ImportState.Blob==OptionState.Blob);
 const FString Content=Extra.Replace(TEXT("Original summary"),TEXT("Selected source update"));
 if (!TestTrue(TEXT("write a genuine selected operation update"),FFileHelper::SaveStringToFile(Content,*Filename))) return false;
 const auto Changed=FVaRestOpenApiReimporter::FromSource(Api,true); Owned.Keep(Changed);
 if (!NoErrors(*this,Changed.Issues,TEXT("selected source update")) || !TestTrue(TEXT("selected content update still commits"),Changed.Api==Api && !Changed.bNoChanges) ||
  !TestEqual(TEXT("unselected operation remains unimported"),Api->Endpoints.Num(),1)) return false;
 TestEqual(TEXT("real selected source description updates"),Api->Endpoints[0].Description,FString(TEXT("Selected source update")));
 TestEqual(TEXT("source update preserves actual endpoint GUID"),Api->Endpoints[0].Id,EndpointId);
 TestTrue(TEXT("real content update advances import state"),Api->ImportState.Blob!=OptionState.Blob);
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiTypeFoldersOfflineTest,"VaRest.OpenApi.Offline.TypeFolders",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiTypeFoldersOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;
 const FString Root=TEXT("/VaRestOpenApiQualification/folders.json");
 for (int32 Mode=0;Mode<3;++Mode)
 {
  FOwnedAssets Owned;
  const FString Folder=TEXT("/Game/__VaRestTypeFolders_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("Pets"))));
  FVaRestOpenApiPlanOptions PlanOptions; PlanOptions.ApiName=TEXT("Pets");
  const auto Plan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(Root,Bytes(Source)),PlanOptions);
  FVaRestOpenApiApplyOptions Options; Options.Parent=Package.Get(); Options.ApiName=TEXT("Pets");
  Options.bRegisterAssets=false; Options.bAddToSettings=false;
  if (Mode!=0) Options.TypesFolder=Folder/(Mode==1 ? TEXT("ExplicitTypes") : TEXT("Types"));
  const FString InitialFolder=Mode==0 ? Folder/TEXT("Pets_Types") : Options.TypesFolder;
  const auto Applied=FVaRestOpenApiApplier::Apply(Plan,Options); Owned.Keep(Applied);
  if (!NoErrors(*this,Applied.Issues,TEXT("folder initial apply")) || !TestNotNull(TEXT("folder fixture actual API"),Applied.Api) ||
   !TestEqual(TEXT("folder fixture creates both real type assets"),Applied.AdditionalObjects.Num(),2)) return false;
  auto* Api=Applied.Api;
  TestEqual(TEXT("default or explicit actual folder is stored on the API"),Api->TypesFolder,InitialFolder);
  TArray<TPair<UObject*,FString>> ExistingPaths;
  for (auto* Object:Applied.AdditionalObjects)
  {
   TestEqual(TEXT("each actual initial type lives in the chosen folder"),FPackageName::GetLongPackagePath(Object->GetOutermost()->GetName()),InitialFolder);
   ExistingPaths.Emplace(Object,Object->GetPathName());
  }
  // Legacy assets without recorded folder metadata get a fallback for NEW types only.
  // Existing objects retain their original package paths, including the old shared Types folder.
  if (Mode==2) Api->TypesFolder.Reset();
  const FString Expected=Mode==2 ? Folder/TEXT("Pets_Types") : InitialFolder;
  if (!TestTrue(TEXT("seed actual existing type identities"),FVaRestOpenApiReimporter::SeedOptions(Api,PlanOptions))) return false;
  const FString Updated=FString(Source).Replace(TEXT("\"status\":{\"$ref\":\"#/components/schemas/Status\"}"),
   TEXT("\"status\":{\"$ref\":\"#/components/schemas/Status\"},\"details\":{\"type\":\"object\",\"properties\":{\"count\":{\"type\":\"integer\",\"format\":\"int32\"}}}"));
  const auto NewPlan=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(Root,Bytes(Updated)),PlanOptions);
  FVaRestOpenApiMergeOptions Automated; Automated.bAutomated=true;
  auto Preview=FVaRestOpenApiReimporter::Preview(Api,NewPlan,Automated);
  auto ReimportOptions=Options; ReimportOptions.TypesFolder.Reset(); // use stored folder, not the initial explicit argument
  const auto Changed=FVaRestOpenApiReimporter::Apply(Api,NewPlan,Preview,ReimportOptions); Owned.Keep(Changed);
  if (!NoErrors(*this,Changed.Issues,TEXT("folder reimport")) || !TestTrue(TEXT("new referenced type is genuinely created"),Changed.Api==Api && !Changed.AdditionalObjects.IsEmpty())) return false;
  for (auto* Object:Changed.AdditionalObjects) TestEqual(TEXT("new reimport types use the stored explicit/default or legacy fallback folder"),FPackageName::GetLongPackagePath(Object->GetOutermost()->GetName()),Expected);
  for (const auto& Entry:ExistingPaths) TestEqual(TEXT("reimport never relocates an existing generated type"),Entry.Key->GetPathName(),Entry.Value);
  TestEqual(TEXT("stored folder metadata is preserved, including honest legacy absence"),Api->TypesFolder,Mode==2 ? FString() : InitialFolder);
 }
 return true;
}

namespace VaRestOpenApiOfflineTest
{
FVaRestOpenApiPlan NoticePlan(const FVaRestJson& Document,FVaRestOpenApiPlanOptions Options={})
{
 auto Analysis=FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/notices.json"),Bytes(Document.ToString()));
 // Only plain owned analysis/options enter the same real planner used by import. No test/UObject/registry closure.
 return Async(EAsyncExecution::ThreadPool,[Analysis=MoveTemp(Analysis),Options=MoveTemp(Options)]() { return FVaRestOpenApiPlanner::Build(Analysis,Options); }).Get();
}
FVaRestJson NoticeDocument(const FString& Name,const FString& EnumName,bool Integer,const FVaRestJson& Default)
{
 auto Properties=FVaRestJson::Object().Set(TEXT("payload"),FVaRestJson::Object().Set(TEXT("type"),Integer ? TEXT("integer") : TEXT("string")))
  .Set(TEXT("metadata"),FVaRestJson::Object().Set(TEXT("type"),TEXT("object")).Set(TEXT("additionalProperties"),FVaRestJson::Object().Set(TEXT("type"),TEXT("integer"))).Set(TEXT("default"),Default))
  .Set(TEXT("state"),FVaRestJson::Object().Set(TEXT("$ref"),TEXT("#/components/schemas/")+EnumName));
 auto Schemas=FVaRestJson::Object().Set(Name,FVaRestJson::Object().Set(TEXT("type"),TEXT("object")).Set(TEXT("properties"),Properties))
  .Set(EnumName,FVaRestJson::Object().Set(TEXT("type"),TEXT("string")).Set(TEXT("enum"),FVaRestJson::Array().Add(TEXT("first")).Add(TEXT("second"))));
 auto Operation=FVaRestJson::Object().Set(TEXT("operationId"),TEXT("notice"))
  .Set(TEXT("responses"),FVaRestJson::Object().Set(TEXT("200"),SchemaResponse(FVaRestJson::Object().Set(TEXT("$ref"),TEXT("#/components/schemas/")+Name))));
 return FVaRestJson::Object().Set(TEXT("openapi"),TEXT("3.0.3"))
  .Set(TEXT("info"),FVaRestJson::Object().Set(TEXT("title"),TEXT("Notices")).Set(TEXT("version"),TEXT("1")))
  .Set(TEXT("servers"),FVaRestJson::Array().Add(FVaRestJson::Object().Set(TEXT("url"),TEXT("https://notices.invalid"))))
  .Set(TEXT("paths"),FVaRestJson::Object().Set(TEXT("/notice"),FVaRestJson::Object().Set(TEXT("get"),Operation)))
  .Set(TEXT("components"),FVaRestJson::Object().Set(TEXT("schemas"),Schemas));
}
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiPlanNoticesOfflineTest,"VaRest.OpenApi.Offline.PlanNotices",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiPlanNoticesOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;using namespace VaRest::OpenApi;
 auto Paths=FVaRestJson::Object();
 for (int32 Index=0;Index<4;++Index)
 {
  auto Properties=Index<3 ? FVaRestJson::Object().Set(TEXT("data"),PropertySchema(TEXT("value")))
   .Set(TEXT("ok"),FVaRestJson::Object().Set(TEXT("type"),TEXT("boolean"))) : FVaRestJson::Object().Set(TEXT("plain"),FVaRestJson::Object().Set(TEXT("type"),TEXT("string")));
  auto Schema=FVaRestJson::Object().Set(TEXT("type"),TEXT("object")).Set(TEXT("properties"),Properties);
  if (Index<3) Schema.Set(TEXT("required"),FVaRestJson::Array().Add(TEXT("ok")));
  auto Operation=FVaRestJson::Object().Set(TEXT("operationId"),TEXT("selected")+FString::FromInt(Index))
   .Set(TEXT("responses"),FVaRestJson::Object().Set(TEXT("200"),SchemaResponse(Schema)));
  Paths.Set(TEXT("/selected")+FString::FromInt(Index),FVaRestJson::Object().Set(TEXT("get"),Operation));
 }
 auto Document=FVaRestJson::Object().Set(TEXT("openapi"),TEXT("3.0.3")).Set(TEXT("info"),FVaRestJson::Object().Set(TEXT("title"),TEXT("Envelope evidence")).Set(TEXT("version"),TEXT("1")))
  .Set(TEXT("servers"),FVaRestJson::Array().Add(FVaRestJson::Object().Set(TEXT("url"),TEXT("https://notices.invalid")))).Set(TEXT("paths"),Paths);
 FVaRestOpenApiPlanOptions Options;Options.bDetectEnvelope=false;
 const auto Detected=NoticePlan(Document,Options);
 if (!TestTrue(TEXT("actual selected envelope plan is valid"),Detected.bValid) || !NoErrors(*this,Detected.Issues,TEXT("detected plan"))) return false;
 TestEqual(TEXT("unchecked unwrapping still records the real candidate key"),Detected.EnvelopeCandidate.DataPath,FString(TEXT("data")));
 TestEqual(TEXT("real wrapped count"),Detected.EnvelopeCandidate.WrappedOperations,3);
 TestEqual(TEXT("real considered count"),Detected.EnvelopeCandidate.ConsideredOperations,4);
 TestTrue(TEXT("candidate does not silently apply unwrapping"),Detected.Envelope.DataPath.IsEmpty() && Detected.Envelope.WrappedOperations==0);
 const auto Evidence=Detected.ToJson()[TEXT("envelopeCandidate")];
 TestTrue(TEXT("public plain plan JSON retains candidate evidence separately"),Evidence[TEXT("dataPath")].AsString()==TEXT("data") && Evidence[TEXT("wrapped")].AsInt()==3 && Evidence[TEXT("considered")].AsInt()==4);
 Options.bDetectEnvelope=true;const auto AppliedEnvelope=NoticePlan(Document,Options);
 TestTrue(TEXT("enabled plan applies the same real detector result"),AppliedEnvelope.Envelope.DataPath==TEXT("data") && AppliedEnvelope.Envelope.WrappedOperations==3 && AppliedEnvelope.Envelope.ConsideredOperations==4);
 Options.bSelectAllOperations=false;Options.SelectedOperations={TEXT("selected0"),TEXT("selected1")};
 const auto Subset=NoticePlan(Document,Options);
 TestTrue(TEXT("replanning selection recomputes detector counts instead of using a stale whole-document result"),Subset.Endpoints.Num()==2 && Subset.EnvelopeCandidate.ConsideredOperations==2 && Subset.EnvelopeCandidate.WrappedOperations==0 && Subset.EnvelopeCandidate.DataPath.IsEmpty() && Subset.Envelope.DataPath.IsEmpty());

 auto CollisionDocument=NoticeDocument(TEXT("pet_owner"),TEXT("State"),false,FVaRestJson::Object());
 auto CollisionSchemas=CollisionDocument[TEXT("components")][TEXT("schemas")].Clone().Set(TEXT("petOwner"),PropertySchema(TEXT("other")));
 CollisionDocument.Set(TEXT("components"),FVaRestJson::Object().Set(TEXT("schemas"),CollisionSchemas));
 FVaRestOpenApiPlanOptions CollisionOptions;CollisionOptions.bGenerateUnreferencedSchemas=true;
 const auto Collision=NoticePlan(CollisionDocument,CollisionOptions);
 TestTrue(TEXT("source-order generated collision is reported without fabricating a registry shadow"),Has(Collision.Issues,TEXT("OA-NAME-COLLISION")) && !Has(Collision.Issues,TEXT("OA-NAME-SHADOW")));
 TestTrue(TEXT("actual allocator retains the original unsuffixed and next source-order name"),Collision.Structs.ContainsByPredicate([](const auto& Row) { return Row.AssetName==TEXT("S_PetOwner"); }) && Collision.Structs.ContainsByPredicate([](const auto& Row) { return Row.AssetName==TEXT("S_PetOwner_2"); }));

 FOwnedAssets Owned;FVaRestOpenApiPlanOptions OriginalOptions;
 const auto Original=FVaRestOpenApiPlanner::Build(FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/root.json"),Bytes(Source)),OriginalOptions);
 const FString Folder=TEXT("/Game/__VaRestPlanNotices_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("Api"))));
 FVaRestOpenApiApplyOptions Apply;Apply.Parent=Package.Get();Apply.ApiName=TEXT("Api");Apply.TypesFolder=Folder/TEXT("Types");Apply.bRegisterAssets=false;Apply.bAddToSettings=false;
 const auto Created=FVaRestOpenApiApplier::Apply(Original,Apply);Owned.Keep(Created);
 if (!NoErrors(*this,Created.Issues,TEXT("notice fixture apply")) || !TestNotNull(TEXT("actual notice API"),Created.Api)) return false;
 TArray<UObject*> Registered;
 ON_SCOPE_EXIT { for (auto* Object:Registered) FAssetRegistryModule::AssetDeleted(Object); };
 for (auto* Object:Created.AdditionalObjects) if (Object && (Object->IsA<UUserDefinedStruct>() || Object->IsA<UUserDefinedEnum>()))
 { FAssetRegistryModule::AssetCreated(Object);Registered.Add(Object); }
 if (!TestEqual(TEXT("owned registry fixture has one real struct and enum"),Registered.Num(),2)) return false;
 const auto Names=SnapshotExistingTypeNames();
 for (auto* Object:Registered) TestTrue(TEXT("GT registry snapshot contains the actual existing type"),Names.ContainsByPredicate([&](const FString& Name) { return Name.Equals(Object->GetName(),ESearchCase::IgnoreCase); }));
 FVaRestOpenApiPlanOptions ShadowOptions;ShadowOptions.ExistingTypeNames=Names;
 auto Analysis=FVaRestOpenApiLoader::Analyze(TEXT("/VaRestOpenApiQualification/root.json"),Bytes(Source));
 const auto Shadow=Async(EAsyncExecution::ThreadPool,[Analysis,ShadowOptions]() { return FVaRestOpenApiPlanner::Build(Analysis,ShadowOptions); }).Get();
 TestTrue(TEXT("real project-name reservation reports collision and shadow"),Has(Shadow.Issues,TEXT("OA-NAME-COLLISION")) && Has(Shadow.Issues,TEXT("OA-NAME-SHADOW")));
 for (const auto& Issue:Shadow.Issues)
 {
  if (Issue.Code==TEXT("OA-NAME-COLLISION")) TestEqual(TEXT("collision is informational"),Issue.Severity,EVaRestOpenApiSeverity::Info);
  if (Issue.Code==TEXT("OA-NAME-SHADOW")) TestTrue(TEXT("shadow warns and suggests a real prefix option"),Issue.Severity==EVaRestOpenApiSeverity::Warning && Issue.Message.Contains(TEXT("Type prefix")));
 }
 if (!TestTrue(TEXT("seed actual retained type identities"),FVaRestOpenApiReimporter::SeedOptions(Created.Api,ShadowOptions))) return false;
 const auto Retained=FVaRestOpenApiPlanner::Build(Analysis,ShadowOptions);
 for (const auto& Type:Original.Structs) TestTrue(TEXT("existing definition identity keeps its actual name despite registry reservations"),Retained.Structs.ContainsByPredicate([&](const auto& Row) { return Row.Id==Type.Id && Row.AssetName==Type.AssetName; }));
 for (const auto& Type:Original.Enums) TestTrue(TEXT("existing enum identity keeps its actual name"),Retained.Enums.ContainsByPredicate([&](const auto& Row) { return Row.Id==Type.Id && Row.AssetName==Type.AssetName; }));
 TestFalse(TEXT("retained definitions are not newly reported as shadows"),Has(Retained.Issues,TEXT("OA-NAME-SHADOW")));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestOpenApiReimportNoticesOfflineTest,"VaRest.OpenApi.Offline.ReimportNotices",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestOpenApiReimportNoticesOfflineTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;using namespace VaRest::OpenApi;
 const auto Defaults=FVaRestJson::Object().Set(TEXT("id"),1).Set(TEXT("name"),2).Set(TEXT("location"),3).Set(TEXT("description"),4);
 const auto Before=NoticePlan(NoticeDocument(TEXT("Before"),TEXT("StateBefore"),false,Defaults));
 FOwnedAssets Owned;const FString Folder=TEXT("/Game/__VaRestReimportNotices_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("Api"))));
 FVaRestOpenApiApplyOptions Apply;Apply.Parent=Package.Get();Apply.ApiName=TEXT("Api");Apply.TypesFolder=Folder/TEXT("Types");Apply.bRegisterAssets=false;Apply.bAddToSettings=false;
 const auto Created=FVaRestOpenApiApplier::Apply(Before,Apply);Owned.Keep(Created);
 if (!NoErrors(*this,Created.Issues,TEXT("reimport notices apply")) || !TestNotNull(TEXT("real persisted notice API"),Created.Api) || !TestEqual(TEXT("notice endpoint count"),Created.Api->Endpoints.Num(),1)) return false;
 auto* Api=Created.Api;auto* Struct=Cast<UUserDefinedStruct>(Api->Endpoints[0].ResponseType.Type.Get());
 if (!TestNotNull(TEXT("actual generated row struct"),Struct)) return false;
 const auto OriginalState=Api->ImportState;const FGuid Endpoint=Api->Endpoints[0].Id;
 FVaRestOpenApiPlanOptions PlanOptions;if (!TestTrue(TEXT("seed persisted actual reimport identities"),FVaRestOpenApiReimporter::SeedOptions(Api,PlanOptions))) return false;
 const auto Renamed=NoticePlan(NoticeDocument(TEXT("After"),TEXT("StateBefore"),false,Defaults),PlanOptions);
 auto Preview=FVaRestOpenApiReimporter::Preview(Api,Renamed);
 if (!TestTrue(TEXT("actual renamed-source preview is valid"),Preview.bValid) || !NoErrors(*this,Preview.Issues,TEXT("rename preview"))) return false;
 TestTrue(TEXT("equal recorded struct shape gives informational possible-rename notice"),Has(Preview.Issues,TEXT("OA-REIMPORT-MAYBERENAME")));
 for (const auto& Issue:Preview.Issues) if (Issue.Code==TEXT("OA-REIMPORT-MAYBERENAME")) TestEqual(TEXT("possible rename never demands or performs adoption"),Issue.Severity,EVaRestOpenApiSeverity::Info);
 TestTrue(TEXT("rename hint retains distinct removed and added identities"),Preview.Merge.Changes.ContainsByPredicate([](const auto& Row) { return Row.Kind==TEXT("struct") && Row.Change==EVaRestOpenApiChange::Removed; }) && Preview.Merge.Changes.ContainsByPredicate([](const auto& Row) { return Row.Kind==TEXT("struct") && Row.Change==EVaRestOpenApiChange::Added; }));
 const auto EnumRenamed=NoticePlan(NoticeDocument(TEXT("Before"),TEXT("StateAfter"),false,Defaults),PlanOptions);
 const auto EnumPreview=FVaRestOpenApiReimporter::Preview(Api,EnumRenamed);
 TestTrue(TEXT("equal persisted enum wire shape is also a possible rename"),Has(EnumPreview.Issues,TEXT("OA-REIMPORT-MAYBERENAME")));
 for (const TCHAR* Key:{TEXT("id"),TEXT("name"),TEXT("location"),TEXT("description")})
 {
  const auto Different=Defaults.Clone().Set(Key,99);
  const auto ChangedDefault=NoticePlan(NoticeDocument(TEXT("After"),TEXT("StateBefore"),false,Different),PlanOptions);
  const auto ChangedPreview=FVaRestOpenApiReimporter::Preview(Api,ChangedDefault);
  TestFalse(FString::Printf(TEXT("actual default data key '%s' is never stripped as comparison metadata"),Key),Has(ChangedPreview.Issues,TEXT("OA-REIMPORT-MAYBERENAME")));
 }
 // An incomplete persisted comparison record is not permission to infer equality or transfer identity.
 auto Historical=Preview.Manifest.Clone();auto StoredPlan=Historical[TEXT("plan")].Clone();auto Rows=StoredPlan[TEXT("structs")].Clone();
 if (!TestTrue(TEXT("real persisted manifest includes its original struct plan"),Rows.IsArray() && Rows.Num()>0)) return false;
 auto Missing=Rows.At(0).Clone();Missing.Remove(TEXT("fields"));Rows.SetAt(0,Missing);StoredPlan.Set(TEXT("structs"),Rows);Historical.Set(TEXT("plan"),StoredPlan);
 { ON_SCOPE_EXIT { Api->ImportState=OriginalState; };
  if (!TestTrue(TEXT("encode an owned incomplete comparison-history fixture"),EncodeState(Historical,Api->ImportState))) return false;
  const auto Limited=FVaRestOpenApiReimporter::Preview(Api,Renamed);
  TestTrue(TEXT("incomplete historical comparison is explained"),Has(Limited.Issues,TEXT("OA-REIMPORT-RENAMEUNAVAILABLE")));
  TestFalse(TEXT("missing historical fields never establish a rename"),Has(Limited.Issues,TEXT("OA-REIMPORT-MAYBERENAME")));
 }
 TStrongObjectPtr<UPackage> TablePackage(CreatePackage(*(Folder/TEXT("Rows"))));
 auto* Table=NewObject<UDataTable>(TablePackage.Get(),TEXT("Rows"),RF_Public|RF_Standalone);Table->RowStruct=Struct;Owned.Objects.Emplace(Table);
 for (const auto& Object:Owned.Objects) Object->GetOutermost()->SetDirtyFlag(false);
 const auto Broken=NoticePlan(NoticeDocument(TEXT("Before"),TEXT("StateBefore"),true,Defaults),PlanOptions);
 const auto Breaking=FVaRestOpenApiReimporter::Preview(Api,Broken);
 TestTrue(TEXT("actual upstream field type change is breaking"),Breaking.Merge.Changes.ContainsByPredicate([](const auto& Row) { return Row.Kind==TEXT("field") && Row.Property==TEXT("type") && Row.bBreaking; }));
 TestTrue(TEXT("actual live DataTable row-struct reference produces a preview warning"),Breaking.Issues.ContainsByPredicate([&](const auto& Issue) { return Issue.Code==TEXT("OA-REIMPORT-DATATABLE") && Issue.Severity==EVaRestOpenApiSeverity::Warning && Issue.AssetPath==Table->GetPathName(); }));
 const auto Ordinary=FVaRestOpenApiReimporter::Preview(Api,Before);
 TestFalse(TEXT("unchanged row type has no DataTable breaking warning"),Has(Ordinary.Issues,TEXT("OA-REIMPORT-DATATABLE")));
 TestTrue(TEXT("selected preview includes real plan counts/envelope notice for the dialog"),Has(Breaking.Issues,TEXT("OA-PLAN-PREVIEW")));
 TestTrue(TEXT("preview warnings never rewrite persisted ownership or endpoint identity"),Api->ImportState.Blob==OriginalState.Blob && Api->Endpoints[0].Id==Endpoint);
 TestTrue(TEXT("preview never replaces the DataTable row struct or populates/resets rows"),Table->GetRowStruct()==Struct && Table->GetRowMap().IsEmpty());
 for (const auto& Object:Owned.Objects) TestFalse(TEXT("notice-only previews do not dirty owned assets"),Object->GetOutermost()->IsDirty());
 return true;
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestOpenApiHeadlessFactoryTest,"VaRest.OpenApi.Offline.FactoryHeadless",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestOpenApiHeadlessFactoryTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
 if (FCommandLine::IsInitialized() && FParse::Param(FCommandLine::Get(),TEXT("VaRestOpenApiFactoryFixture")) && FParse::Param(FCommandLine::Get(),TEXT("nowrite")))
 {
  Names.Add(TEXT("SelectedPlan")); Commands.Add(TEXT("SelectedPlan"));
 }
}
bool FVaRestOpenApiHeadlessFactoryTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOpenApiOfflineTest;using namespace VaRest::OpenApi;
 if (!FCommandLine::IsInitialized() || !FParse::Param(FCommandLine::Get(),TEXT("VaRestOpenApiFactoryFixture")) ||
  !FParse::Param(FCommandLine::Get(),TEXT("nowrite")) || !Parameters.Equals(TEXT("SelectedPlan"),ESearchCase::CaseSensitive))
 {
  AddError(TEXT("FactoryHeadless.SelectedPlan requires its dedicated fresh-process -VaRestOpenApiFactoryFixture -nowrite invocation")); return false;
 }
 if (!TestTrue(TEXT("public factory fixture runs on the actual game thread"),IsInGameThread())) return false;
 auto* Settings=GetMutableDefault<UVaRestHttpSettings>();
 FString ConfigFilename=FPaths::ConvertRelativePathToFull(Settings->GetDefaultConfigFilename());
 FString ConfigRoot=FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir());
 FPaths::NormalizeFilename(ConfigFilename); FPaths::NormalizeDirectoryName(ConfigRoot);
 if (!TestTrue(TEXT("default config path has no unresolved parent traversal"),FPaths::CollapseRelativeDirectories(ConfigFilename)) ||
  !TestTrue(TEXT("project config root has no unresolved parent traversal"),FPaths::CollapseRelativeDirectories(ConfigRoot)) ||
  !TestTrue(TEXT("actual settings default is inside this test host's Config directory"),ConfigFilename.StartsWith(ConfigRoot+TEXT("/"),ESearchCase::CaseSensitive)) ||
  !TestEqual(TEXT("fixture affects only the actual Engine default config declaration"),FPaths::GetCleanFilename(ConfigFilename),FString(TEXT("DefaultEngine.ini")))) return false;
 AddInfo(TEXT("FactoryHeadlessFixture DefaultConfig=")+ConfigFilename+TEXT(" EngineDestination=")+GEngineIni);
 FOwnedAssets Owned; // Must outlive the settings restore guard below.
 const auto OriginalApis=Settings->Apis;
 ON_SCOPE_EXIT { Settings->Apis=OriginalApis; TestTrue(TEXT("factory preload memory is restored before owned asset cleanup"),Settings->Apis==OriginalApis); };
 // The real factory flush/reload is allowed only in this dedicated -nowrite process.
 // Cache custody ends at process exit; this guard does not claim an unchanged config cache.
 const FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("VaRestOpenApiQualification")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
 const FString Filename=Directory/TEXT("factory-headless.json");
 if (!TestFalse(TEXT("factory fixture never overwrites an existing source"),IFileManager::Get().FileExists(*Filename)) ||
  !TestTrue(TEXT("create only the owned factory source directory"),IFileManager::Get().MakeDirectory(*Directory,true))) return false;
 ON_SCOPE_EXIT { IFileManager::Get().Delete(*Filename,false,true); IFileManager::Get().DeleteDirectory(*Directory,false,false); };
 auto Schemas=FVaRestJson::Object()
  .Set(TEXT("State"),FVaRestJson::Object().Set(TEXT("type"),TEXT("string")).Set(TEXT("enum"),FVaRestJson::Array().Add(TEXT("waiting")).Add(TEXT("ready"))))
  .Set(TEXT("Pet"),FVaRestJson::Object().Set(TEXT("type"),TEXT("object")).Set(TEXT("properties"),FVaRestJson::Object()
   .Set(TEXT("id"),FVaRestJson::Object().Set(TEXT("type"),TEXT("integer")).Set(TEXT("format"),TEXT("int64")))
   .Set(TEXT("state"),FVaRestJson::Object().Set(TEXT("$ref"),TEXT("#/components/schemas/State")))))
  .Set(TEXT("Envelope"),FVaRestJson::Object().Set(TEXT("type"),TEXT("object")).Set(TEXT("required"),FVaRestJson::Array().Add(TEXT("ok")))
   .Set(TEXT("properties"),FVaRestJson::Object().Set(TEXT("data"),FVaRestJson::Object().Set(TEXT("$ref"),TEXT("#/components/schemas/Pet")))
    .Set(TEXT("ok"),FVaRestJson::Object().Set(TEXT("type"),TEXT("boolean")))))
  .Set(TEXT("Unused"),PropertySchema(TEXT("mustNotBeGenerated")));
 auto Paths=FVaRestJson::Object();
 for (int32 Index=0;Index<4;++Index)
 {
  const FString Id=Index<3 ? TEXT("selected")+FString::FromInt(Index) : FString(TEXT("unselected"));
  const FString Ref=Index<3 ? TEXT("#/components/schemas/Envelope") : TEXT("#/components/schemas/Unused");
  const auto Operation=FVaRestJson::Object().Set(TEXT("operationId"),Id)
   .Set(TEXT("responses"),FVaRestJson::Object().Set(TEXT("200"),SchemaResponse(FVaRestJson::Object().Set(TEXT("$ref"),Ref))));
  Paths.Set(TEXT("/")+Id,FVaRestJson::Object().Set(TEXT("get"),Operation));
 }
 const auto Document=FVaRestJson::Object().Set(TEXT("openapi"),TEXT("3.0.3"))
  .Set(TEXT("info"),FVaRestJson::Object().Set(TEXT("title"),TEXT("Headless factory")).Set(TEXT("version"),TEXT("1")))
  .Set(TEXT("servers"),FVaRestJson::Array().Add(FVaRestJson::Object().Set(TEXT("url"),TEXT("https://factory-headless.invalid"))))
  .Set(TEXT("paths"),Paths).Set(TEXT("components"),FVaRestJson::Object().Set(TEXT("schemas"),Schemas));
 if (!TestTrue(TEXT("write the actual local JSON factory input"),FFileHelper::SaveStringToFile(Document.ToString(),*Filename))) return false;
 TStrongObjectPtr<UVaRestOpenApiImportOptions> Options(NewObject<UVaRestOpenApiImportOptions>());
 if (!TestTrue(TEXT("explicit task options do not inherit user config"),Options->FromJson(FVaRestJson::Object()))) return false;
 Options->bImportAllOperations=false;Options->SelectedOperations={TEXT("selected0"),TEXT("selected1"),TEXT("selected2")};
 Options->bIncludeNewOperations=false;Options->bUnwrapEnvelope=false;
 const auto OriginalOptions=Options->ToJson();
 const FString Folder=TEXT("/Game/__VaRestHeadlessFactory_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
 TStrongObjectPtr<UPackage> Package(CreatePackage(*(Folder/TEXT("FactoryApi"))));
 TStrongObjectPtr<UVaRestOpenApiFactory> Factory(NewObject<UVaRestOpenApiFactory>());
 TStrongObjectPtr<UAssetImportTask> Task(NewObject<UAssetImportTask>());
 Task->Filename=Filename;Task->DestinationPath=Folder;Task->DestinationName=TEXT("FactoryApi");Task->Options=Options.Get();Task->Factory=Factory.Get();
 Task->bAutomated=true;Task->bSave=false;Task->bAsync=false;Task->bReplaceExisting=false;Task->bReplaceExistingSettings=false;
 Factory->SetAssetImportTask(Task.Get());
 if (!TestTrue(TEXT("real import task and public factory admit the automated branch before any import"),Task->bAutomated && Factory->IsAutomatedImport())) return false;
 UFactory* PublicFactory=Factory.Get();bool Cancelled=true;
 UObject* Imported=PublicFactory->FactoryCreateFile(UVaRestApi::StaticClass(),Package.Get(),TEXT("FactoryApi"),RF_Public|RF_Standalone|RF_Transactional,Filename,nullptr,nullptr,Cancelled);
 auto* Api=Cast<UVaRestApi>(Imported);Owned.Api=Api;
 if (Imported) Owned.Objects.Emplace(Imported);
 const auto& Additional=PublicFactory->GetAdditionalImportedObjects();for (auto* Object:Additional) if (Object) Owned.Objects.Emplace(Object);
 if (!TestNotNull(TEXT("public automated factory returns the actual API"),Api) || !TestFalse(TEXT("real automated import was not cancelled"),Cancelled)) return false;
 TestTrue(TEXT("factory duplicated task options instead of modifying them"),Options->ToJson().Equals(OriginalOptions));
 TestTrue(TEXT("real factory intentionally registers its API in preload memory"),Settings->Apis.Contains(TSoftObjectPtr<UVaRestApi>(Api)));
 TestEqual(TEXT("only the owned new API was added to preload memory"),Settings->Apis.Num(),OriginalApis.Num()+1);
 if (!TestEqual(TEXT("actual API contains only the three selected operations"),Api->Endpoints.Num(),3) ||
  !TestEqual(TEXT("factory exposes its actual generated two structs and enum"),Additional.Num(),3)) return false;
 TestEqual(TEXT("factory uses the per-API default type folder"),Api->TypesFolder,Folder/TEXT("FactoryApi_Types"));
 int32 Structs=0,Enums=0;
 for (auto* Object:Additional)
 {
  if (!TestNotNull(TEXT("each factory additional object is real"),Object)) return false;
  Structs+=Object->IsA<UUserDefinedStruct>();Enums+=Object->IsA<UUserDefinedEnum>();
  if (const auto* Struct=Cast<UUserDefinedStruct>(Object)) TestEqual(TEXT("actual factory struct is compiled"),Struct->Status,UDSS_UpToDate);
  TestTrue(TEXT("actual API owns rules for every returned native type"),Api->JsonTypes.ContainsByPredicate([&](const auto& Rules) { return Rules.Type.Get()==Object; }));
  TestEqual(TEXT("actual generated package is in the API's recorded folder"),FPackageName::GetLongPackagePath(Object->GetOutermost()->GetName()),Api->TypesFolder);
 }
 TestEqual(TEXT("actual native compiled struct count"),Structs,2);TestEqual(TEXT("actual native enum count"),Enums,1);
 for (int32 Index=0;Index<3;++Index)
  TestTrue(TEXT("each selected route survives the real factory"),Api->Endpoints.ContainsByPredicate([&](const auto& Endpoint) { return Endpoint.Path==TEXT("/selected")+FString::FromInt(Index); }));
 TestFalse(TEXT("unselected route is absent from the actual API"),Api->Endpoints.ContainsByPredicate([](const auto& Endpoint) { return Endpoint.Path==TEXT("/unselected"); }));
 const auto State=DecodeState(Api->ImportState);
 if (!TestTrue(TEXT("factory produced readable actual ImportState"),State.IsSet())) return false;
 const auto SavedPlan=State.GetValue()[TEXT("plan")],SavedOptions=State.GetValue()[TEXT("options")];
 TestTrue(TEXT("serialized selected plan matches actual native object counts"),SavedPlan[TEXT("endpoints")].Num()==3 && SavedPlan[TEXT("structs")].Num()==2 && SavedPlan[TEXT("enums")].Num()==1);
 TestTrue(TEXT("factory stores the explicit partial selection without auto-including new operations"),!SavedOptions[TEXT("allOperations")].AsBool() && SavedOptions[TEXT("operations")].Num()==3 && !SavedOptions[TEXT("includeNewOperations")].AsBool());
 TestEqual(TEXT("factory analyzed all four real declared operations"),SavedOptions[TEXT("knownOperations")].Num(),4);
 TestTrue(TEXT("factory stores disabled unwrapping while retaining the real selected candidate"),!SavedOptions[TEXT("unwrapEnvelope")].AsBool() &&
  SavedPlan[TEXT("envelopeCandidate")][TEXT("dataPath")].AsString()==TEXT("data") && SavedPlan[TEXT("envelopeCandidate")][TEXT("wrapped")].AsInt()==3 && SavedPlan[TEXT("envelopeCandidate")][TEXT("considered")].AsInt()==3);
 TestTrue(TEXT("candidate evidence was not applied as runtime unwrapping"),Api->Envelope.DataPath.IsEmpty() && SavedPlan[TEXT("envelope")][TEXT("dataPath")].AsString().IsEmpty() && SavedPlan[TEXT("envelope")][TEXT("wrapped")].AsInt()==0);
 TestTrue(TEXT("real factory records a local file source"),State.GetValue()[TEXT("source")][TEXT("kind")].AsString()==TEXT("file") && State.GetValue()[TEXT("source")][TEXT("documents")].IsArray() && State.GetValue()[TEXT("source")][TEXT("documents")].Num()>0);
 AddInfo(TEXT("FactoryHeadless fixture checks native selected-plan/preload memory only; -nowrite disables config persistence, and Engine cache custody ends with this dedicated process."));
 return true;
}
#endif
