// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimport.h"
#include "VaRestOpenApiApplyPrivate.h"
#include "VaRestOpenApiSnapshots.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/ARFilter.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/DataTable.h"
#include "UObject/UObjectIterator.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include <initializer_list>

namespace
{
using namespace VaRest::OpenApi;
/** Compare only typed generated-plan records. Arbitrary JSON defaults are always compared as complete data. */
struct FRecordedShapeComparison
{
 const FVaRestJson& OldPlan;const FVaRestJson& NewPlan;int32& Remaining;bool& Unavailable;
 FRecordedShapeComparison(const FVaRestJson& Old,const FVaRestJson& New,int32& Budget,bool& Missing):OldPlan(Old),NewPlan(New),Remaining(Budget),Unavailable(Missing) {}
 struct FPair { FString OldId,NewId;bool Enum=false; };
 TArray<FPair> Seen;
 bool Charge(int64 Amount=1)
 {
  if (Amount>Remaining) { Remaining=0;Unavailable=true;return false; }Remaining-=int32(Amount);return true;
 }
 bool DataBudget(const FVaRestJson& Value,int32 Depth=0)
 {
  if (Depth>VaRest::Json::MaxDepth || !Charge()) { Unavailable=true;return false; }
  if (Value.IsString()) return Charge(Value.AsUtf8View().Len());
  if (Value.IsArray()) { for (const auto& Child:Value.Elements()) if (!DataBudget(Child,Depth+1)) return false; }
  else if (Value.IsObject()) for (const auto& Child:Value.Members()) if (!Charge(Child.GetKey().Len()) || !DataBudget(Child.Value,Depth+1)) return false;
  return true;
 }
 bool Same(const FVaRestJson& A,const FVaRestJson& B,const TCHAR* Key)
 {
  if (A.Has(Key)!=B.Has(Key)) return false;
  return DataBudget(A[Key]) && DataBudget(B[Key]) && A[Key].Equals(B[Key]);
 }
 FVaRestJson Find(const FVaRestJson& Plan,const FString& Id,bool Enum)
 {
  const auto Rows=Plan[Enum ? TEXT("enums") : TEXT("structs")];
  if (!Rows.IsArray()) { Unavailable=true;return {}; }
  for (const auto& Row:Rows.Elements()) { if (!Charge()) return {};if (Equal(Row[TEXT("id")].AsString(),Id)) return Row; }
  Unavailable=true;return {};
 }
 bool Type(const FVaRestJson& A,const FVaRestJson& B,int32 Depth)
 {
  if (!Charge()) return false;
  if (!A[TEXT("kind")].IsString() || !B[TEXT("kind")].IsString() || !A[TEXT("container")].IsString() || !B[TEXT("container")].IsString() ||
   !A[TEXT("definition")].IsString() || !B[TEXT("definition")].IsString() || !A[TEXT("jsonValue")].IsBool() || !B[TEXT("jsonValue")].IsBool()) { Unavailable=true;return false; }
  if (!Same(A,B,TEXT("kind")) || !Same(A,B,TEXT("container")) || !Same(A,B,TEXT("jsonValue"))) return false;
  const FString Kind=A[TEXT("kind")].AsString(),Container=A[TEXT("container")].AsString();
  bool Known=false;for (const TCHAR* Candidate:{TEXT("None"),TEXT("Json"),TEXT("Struct"),TEXT("Enum"),TEXT("String"),TEXT("Bytes"),TEXT("Bool"),TEXT("Int"),TEXT("Int64"),TEXT("Double"),TEXT("DateTime")}) Known|=Equal(Kind,Candidate);
  if (!Known || !(Equal(Container,TEXT("None")) || Equal(Container,TEXT("Array")) || Equal(Container,TEXT("Map")))) { Unavailable=true;return false; }
  if (Equal(Kind,TEXT("Struct")) || Equal(Kind,TEXT("Enum"))) return Definition(A[TEXT("definition")].AsString(),B[TEXT("definition")].AsString(),Equal(Kind,TEXT("Enum")),Depth+1);
  return Same(A,B,TEXT("definition"));
 }
 bool Definition(const FString& OldId,const FString& NewId,bool Enum,int32 Depth=0)
 {
  if (Depth>VaRest::Json::MaxDepth || !Charge()) { Unavailable=true;return false; }
  if (OldId.IsEmpty() || NewId.IsEmpty()) { Unavailable=true;return false; }
  const auto A=Find(OldPlan,OldId,Enum),B=Find(NewPlan,NewId,Enum);if (!A.IsObject() || !B.IsObject()) return false;
  for (const auto& Pair:Seen) if (Pair.Enum==Enum && Equal(Pair.OldId,OldId) && Equal(Pair.NewId,NewId)) return true;
  Seen.Add({OldId,NewId,Enum});
  const auto Left=A[Enum ? TEXT("values") : TEXT("fields")],Right=B[Enum ? TEXT("values") : TEXT("fields")];
  if (!Left.IsArray() || !Right.IsArray()) { Unavailable=true;return false; }
  if (Left.Num()!=Right.Num()) return false;
  if (Enum && (!A[TEXT("hasUnset")].IsBool() || !B[TEXT("hasUnset")].IsBool() || !A[TEXT("stringFallback")].IsBool() || !B[TEXT("stringFallback")].IsBool())) { Unavailable=true;return false; }
  if (Enum && (!Same(A,B,TEXT("hasUnset")) || !Same(A,B,TEXT("stringFallback")))) return false;
  for (int32 Index=0;Index<Left.Num();++Index)
  {
   if (!Charge()) return false;const auto X=Left.At(Index),Y=Right.At(Index);
   if (!X[TEXT("wire")].IsString() || !Y[TEXT("wire")].IsString()) { Unavailable=true;return false; }
   if (!Same(X,Y,TEXT("wire"))) return false;
   if (Enum)
   {
    if (!X[TEXT("unset")].IsBool() || !Y[TEXT("unset")].IsBool()) { Unavailable=true;return false; }
    if (!Same(X,Y,TEXT("unset"))) return false;
   }
   else
   {
    if (!X[TEXT("presence")].IsString() || !Y[TEXT("presence")].IsString() || !X[TEXT("defaultText")].IsString() || !Y[TEXT("defaultText")].IsString()) { Unavailable=true;return false; }
    for (const TCHAR* Key:{TEXT("nullable"),TEXT("base64"),TEXT("required"),TEXT("defaultTooltipOnly")})
    { if (!X[Key].IsBool() || !Y[Key].IsBool()) { Unavailable=true;return false; }if (!Same(X,Y,Key)) return false; }
    if (!Same(X,Y,TEXT("presence")) || !Same(X,Y,TEXT("default")) || !Type(X[TEXT("type")],Y[TEXT("type")],Depth)) return false;
   }
  }
  return true;
 }
};
void RenameNotices(FVaRestOpenApiReimportPreview& Preview,const FVaRestOpenApiPlan& Plan)
{
 const auto& OldPlan=Preview.Manifest[TEXT("plan")];const auto NewPlan=Plan.ToJson();int32 Remaining=1000000;bool Unavailable=false;
 for (const auto& Removed:Preview.Merge.Changes)
 {
  if (Removed.Change!=EVaRestOpenApiChange::Removed || !(Equal(Removed.Kind,TEXT("struct")) || Equal(Removed.Kind,TEXT("enum")))) continue;
  for (const auto& Added:Preview.Merge.Changes)
  {
   if (Remaining==0) break;
   if (Added.Change!=EVaRestOpenApiChange::Added || !Equal(Added.Kind,Removed.Kind)) continue;
   FRecordedShapeComparison Compare{OldPlan,NewPlan,Remaining,Unavailable};
   if (!Compare.Definition(Removed.Id,Added.Id,Equal(Removed.Kind,TEXT("enum")))) continue;
   FVaRestOpenApiIssue Issue;Issue.Severity=EVaRestOpenApiSeverity::Info;Issue.Code=TEXT("OA-REIMPORT-MAYBERENAME");
   Issue.Message=TEXT("Removed ")+Removed.Kind+TEXT(" '")+Removed.Id+TEXT("' and added '")+Added.Id+TEXT("' have equal recorded generated shapes; this may be a rename. Identities and assets will not be reassigned.");Preview.Issues.Add(MoveTemp(Issue));break;
  }
 }
 if (Unavailable)
 {
  FVaRestOpenApiIssue Issue;Issue.Severity=EVaRestOpenApiSeverity::Info;Issue.Code=TEXT("OA-REIMPORT-RENAMEUNAVAILABLE");
  Issue.Message=TEXT("Some possible renames could not be compared from the saved generated-plan records (legacy/incomplete records or comparison limit). No original-schema equivalence or identity transfer is assumed.");Preview.Issues.Add(MoveTemp(Issue));
 }
}
void DataTableNotices(FVaRestOpenApiReimportPreview& Preview,IAssetRegistry& Registry)
{
 TStringMap<FString> Broken;
 for (const auto& Change:Preview.Merge.Changes) if (Change.bBreaking && Change.Change!=EVaRestOpenApiChange::KeptUserEdit && Equal(Change.Kind,TEXT("field")))
  for (const auto& Item:Preview.Merge.Items) if (Equal(Item.Id,Change.Owner) && Equal(Item.Kind,TEXT("struct")) && SafeAssetPath(Item.Target)) Broken.Add(Item.Id,Item.Target);
 for (const auto& Struct:Broken)
 {
  TSet<FName> Packages;TArray<FName> Work;Work.Add(*FPackageName::ObjectPathToPackageName(Struct.Value));Packages.Add(Work[0]);
  for (int32 Index=0;Index<Work.Num();++Index)
  {
   TArray<FName> Referencers;Registry.GetReferencers(Work[Index],Referencers,UE::AssetRegistry::EDependencyCategory::Package);
   for (const auto Name:Referencers) if (!Packages.Contains(Name)) { Packages.Add(Name);Work.Add(Name); }
  }
  FARFilter Filter;Filter.PackageNames=Work;Filter.ClassPaths.Add(UDataTable::StaticClass()->GetClassPathName());Filter.bRecursiveClasses=true;
  TArray<FAssetData> Assets;Registry.GetAssets(Filter,Assets,false);TArray<FString> Tables;
  for (const auto& Asset:Assets)
  {
   FString RowType;
   if (Asset.GetTagValue(FName(TEXT("RowStructure")),RowType) &&
    (Equal(RowType,Struct.Value) || Packages.Contains(*FPackageName::ObjectPathToPackageName(RowType)))) Tables.AddUnique(Asset.GetObjectPathString());
  }
  // Include a live, unsaved DataTable whose actual row-struct pointer is this generated struct.
  const auto* Changed=FindObject<UScriptStruct>(nullptr,*Struct.Value);
  if (Changed) for (TObjectIterator<UDataTable> Table;Table;++Table)
   if (IsValid(*Table) && !Table->HasAnyFlags(RF_ClassDefaultObject|RF_ArchetypeObject) && Table->GetRowStruct()==Changed) Tables.AddUnique(Table->GetPathName());
  Tables.Sort();
  for (const auto& Table:Tables)
  {
   FVaRestOpenApiIssue Issue;Issue.Severity=EVaRestOpenApiSeverity::Warning;Issue.Code=TEXT("OA-REIMPORT-DATATABLE");Issue.AssetPath=Table;
   Issue.Message=TEXT("DataTable '")+Table+TEXT("' references breaking struct '")+Struct.Value+TEXT("'. Applying the changed member type may reset stored row values; review this table before applying.");Preview.Issues.Add(MoveTemp(Issue));
  }
 }
}
}
bool FVaRestOpenApiReimporter::SeedOptions(UVaRestApi* Api,FVaRestOpenApiPlanOptions& Options)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());if (!Api) return false;
	const auto State=DecodeState(Api->ImportState);if (!State.IsSet()) return false;
	Options.ExistingEndpoints.Reset();Options.ExistingDefinitions.Reset();
	const auto Live=FVaRestOpenApiMerge::Capture(Api,State.GetValue());
	for (const auto& Item:Live) if (Equal(Item.Kind,TEXT("struct")) || Equal(Item.Kind,TEXT("enum")))
	{
		FString Name=FPackageName::ObjectPathToObjectName(Item.Target);
		if (Name.IsEmpty()) for (const TCHAR* Key:{TEXT("structs"),TEXT("enums")}) for (const auto& Row:State.GetValue()[TEXT("plan")][Key].Elements()) if (Equal(Row[TEXT("id")].AsString(),Item.Id)) Name=Row[TEXT("name")].AsString();
		if (!Name.IsEmpty()) Options.ExistingDefinitions.Emplace(Item.Id,Name);
	}
	const auto History=State.GetValue()[TEXT("endpointIdentities")];const auto Identities=History.IsArray() ? History : State.GetValue()[TEXT("plan")][TEXT("endpoints")];
	for (const auto& Row:Identities.Elements())
	{
		FVaRestOpenApiEndpointIdentity Identity;Identity.Id=Row[TEXT("id")].AsString();Identity.OperationId=Row[TEXT("operationId")].AsString();Identity.Verb=Row[TEXT("verb")].AsString();Identity.Path=Row[TEXT("path")].AsString();Options.ExistingEndpoints.Add(MoveTemp(Identity));
	}
	return true;
}
FVaRestOpenApiReimportPreview FVaRestOpenApiReimporter::Preview(UVaRestApi* Api,const FVaRestOpenApiPlan& Plan,const FVaRestOpenApiMergeOptions& InOptions)
{
	using namespace VaRest::OpenApi;check(IsInGameThread());FVaRestOpenApiReimportPreview Out;Out.Issues=Plan.Issues;
	const auto State=Api ? DecodeState(Api->ImportState) : TOptional<FVaRestJson>();
	if (!Plan.bValid || !State.IsSet())
	{
		FVaRestOpenApiIssue Issue;Issue.Code=TEXT("OA-REIMPORT-STATE");Issue.Message=TEXT("A valid plan and a readable VaRest OpenAPI import manifest are required.");Out.Issues.Add(MoveTemp(Issue));return Out;
	}
	Out.Manifest=State.GetValue();Out.Base=FVaRestOpenApiMerge::Base(Out.Manifest);Out.Mine=FVaRestOpenApiMerge::Capture(Api,Out.Manifest);Out.Theirs=FVaRestOpenApiMerge::Desired(Plan,Out.Manifest,Out.Mine);
	auto Options=InOptions;for (const auto& Id:Out.Manifest[TEXT("suppressions")].Elements()) Options.SuppressedIds.Add(Id.AsString());
	Out.Merge=FVaRestOpenApiMerge::Compare(Out.Base,Out.Mine,Out.Theirs,Options);
	FVaRestOpenApiIssue Summary;Summary.Severity=EVaRestOpenApiSeverity::Info;Summary.Code=TEXT("OA-PLAN-PREVIEW");
	Summary.Message=FString::Printf(TEXT("Selected plan: %d endpoints, %d structs, %d enums. Applied envelope DataPath='%s' (%d of %d considered operations); detected candidate DataPath='%s' (%d of %d)."),Plan.Endpoints.Num(),Plan.Structs.Num(),Plan.Enums.Num(),*Plan.Envelope.DataPath,Plan.Envelope.WrappedOperations,Plan.Envelope.ConsideredOperations,*Plan.EnvelopeCandidate.DataPath,Plan.EnvelopeCandidate.WrappedOperations,Plan.EnvelopeCandidate.ConsideredOperations);Out.Issues.Add(MoveTemp(Summary));
	RenameNotices(Out,Plan);
	TStringMap<const FVaRestOpenApiMergeItem*> Items;for (const auto& Item:Out.Merge.Items) Items.Add(Item.Id,&Item);
	TSet<FName> Packages;
	for (const auto& Change:Out.Merge.Changes) if (Change.bApply)
	{
		const auto* Item=Items.Find(Change.Id);if (!Item) continue;
		if (!Change.Owner.IsEmpty()) Item=Items.Find(Change.Owner);
		if (Item && (Equal((*Item)->Kind,TEXT("struct")) || Equal((*Item)->Kind,TEXT("enum"))) && SafeAssetPath((*Item)->Target)) Packages.Add(*FPackageName::ObjectPathToPackageName((*Item)->Target));
	}
	const auto NextBase=Out.Merge.NextBase(Out.Base,Out.Theirs);
	if (Out.Merge.HasWrites() || !FVaRestOpenApiMerge::ItemsJson(NextBase).Equals(Out.Manifest[TEXT("items")])) Packages.Add(Api->GetOutermost()->GetFName());
	TArray<FName> Work=Packages.Array();auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
	DataTableNotices(Out,Registry);
	for (int32 Index=0;Index<Work.Num();++Index)
	{
		TArray<FName> Referencers;Registry.GetReferencers(Work[Index],Referencers,UE::AssetRegistry::EDependencyCategory::Package);
		for (const FName Name:Referencers) if (!Packages.Contains(Name)) { Packages.Add(Name);Work.Add(Name); }
	}
	Out.AffectedPackages=Packages.Array();Out.AffectedPackages.Sort(FNameLexicalLess());Out.bValid=true;return Out;
}
