// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestOpenApiModel.h"
#include "VaRestOpenApiPlan.h"
#include "VaRestOpenApiApply.h"
#include "VaRestOpenApiReimport.h"
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

#endif
