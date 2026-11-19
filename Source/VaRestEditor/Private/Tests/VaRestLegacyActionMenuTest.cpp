// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreMinimal.h"
#include "BlueprintActionDatabase.h"
#include "BlueprintActionFilter.h"
#include "BlueprintActionMenuBuilder.h"
#include "BlueprintActionMenuItem.h"
#include "BlueprintEditorSettings.h"
#include "BlueprintFunctionNodeSpawner.h"
#include "BlueprintNodeSpawner.h"
#include "K2Node_VaRestMakeJson.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRest_BreakJson.h"
#include "VaRestJsonLibrary.h"
#include "VaRestJsonObject.h"

namespace VaRestLegacyActionMenuTest
{
#include "VaRestLegacyV1TagFunctions.inl"
struct FObservedActions
{
 int32 LegacyFunctions=0;
 int32 LegacyNodes=0;
 int32 V2MakeJson=0;
 TSet<const UFunction*> PositiveFunctions;
};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyActionMenuTest,"VaRest.Compat.ActionMenuSelectivity",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyActionMenuTest::RunTest(const FString& Parameters)
{
 using namespace VaRestLegacyActionMenuTest;
 if (!TestTrue(TEXT("action database/menu checks run on the game thread"),IsInGameThread())) return false;
 TSet<const UFunction*> Legacy;
 TArray<UClass*> Classes;
 bool Ok=TestEqual(TEXT("the frozen manifest retains all 109 identities"),int32(UE_ARRAY_COUNT(TaggedFunctions)),109);
 for (const auto& Row:TaggedFunctions)
 {
  UClass* Owner=FindObject<UClass>(nullptr,Row.Owner);
  if (!TestNotNull(Row.Owner,Owner)) return false;
  UFunction* Function=Owner->FindFunctionByName(FName(Row.Name));
  if (!TestNotNull(Row.Name,Function)) return false;
  Classes.AddUnique(Owner); Legacy.Add(Function);
  const FString Label=FString(Row.Owner)+TEXT(".")+Row.Name;
  Ok&=TestTrue(*Label,Function->HasAnyFunctionFlags(FUNC_BlueprintCallable)
   && !Function->HasAnyFunctionFlags(FUNC_BlueprintEvent)
   && Function->HasMetaData(TEXT("DeprecatedFunction"))
   && !Function->GetMetaData(TEXT("DeprecationMessage")).IsEmpty()
   && Function->GetBoolMetaData(TEXT("BlueprintInternalUseOnly")));
 }
 Ok&=TestEqual(TEXT("frozen identities resolve to 109 distinct live functions"),Legacy.Num(),109);
 if (!Ok) return false;

 const UClass* OldMake=UVaRest_MakeJson::StaticClass();
 const UClass* OldBreak=UVaRest_BreakJson::StaticClass();
 const UClass* NewMake=UK2Node_VaRestMakeJson::StaticClass();
 Classes.AddUnique(UVaRest_MakeJson::StaticClass());
 Classes.AddUnique(UVaRest_BreakJson::StaticClass());
 Classes.AddUnique(UK2Node_VaRestMakeJson::StaticClass());
 Classes.AddUnique(UVaRestJsonLibrary::StaticClass());
 TSet<const UFunction*> Positive;
 Positive.Add(UVaRestJsonObject::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestJsonObject,ToVaRestJson)));
 Positive.Add(UVaRestJsonObject::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestJsonObject,FromVaRestJson)));
 Positive.Add(UVaRestJsonLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary,MakeJsonObject)));
 Positive.Add(UVaRestJsonLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestJsonLibrary,JsonToString)));
 if (!TestEqual(TEXT("four distinct bridge/v2 positive controls"),Positive.Num(),4)
  || !TestFalse(TEXT("all positive controls resolve"),Positive.Contains(nullptr))) return false;
 for (const UFunction* Function:Positive)
 {
  Ok&=TestFalse(TEXT("a positive control is not in the frozen legacy set"),Legacy.Contains(Function));
  Ok&=TestFalse(TEXT("a positive control is not internal-only"),Function->GetBoolMetaData(TEXT("BlueprintInternalUseOnly")));
 }
 if (!Ok) return false;

 FBlueprintActionDatabase& Database=FBlueprintActionDatabase::Get();
 UBlueprintEditorSettings* Settings=GetMutableDefault<UBlueprintEditorSettings>();
 const bool SavedExpose=Settings->bExposeDeprecatedFunctions;
 ON_SCOPE_EXIT
 {
  Settings->bExposeDeprecatedFunctions=SavedExpose;
  for (UClass* Class:Classes) Database.RefreshClassActions(Class);
 };
 const auto Observe=[&](const UBlueprintNodeSpawner* Spawner,const UFunction* Function,FObservedActions& Out)
 {
  if (!Spawner) return;
  if (Legacy.Contains(Function)) ++Out.LegacyFunctions;
  if (Positive.Contains(Function)) Out.PositiveFunctions.Add(Function);
  if (Spawner->NodeClass.Get()==OldMake || Spawner->NodeClass.Get()==OldBreak) ++Out.LegacyNodes;
  if (Spawner->NodeClass.Get()==NewMake) ++Out.V2MakeJson;
 };
 const auto Check=[&](const TCHAR* Where,const FObservedActions& Seen)
 {
  bool Passed=TestEqual(*(FString(Where)+TEXT(" hides all frozen function identities")),Seen.LegacyFunctions,0);
  Passed&=TestEqual(*(FString(Where)+TEXT(" hides both old Make/Break node classes")),Seen.LegacyNodes,0);
  Passed&=TestEqual(*(FString(Where)+TEXT(" retains both same-class bridges and both v2 functions")),Seen.PositiveFunctions.Num(),4);
  Passed&=TestTrue(*(FString(Where)+TEXT(" retains the v2 Make Json custom node")),Seen.V2MakeJson>0);
  return Passed;
 };
 const bool Modes[]={false,true};
 for (const bool Expose:Modes)
 {
  Settings->bExposeDeprecatedFunctions=Expose;
  for (UClass* Class:Classes) Database.RefreshClassActions(Class);
  FObservedActions Unfiltered;
  // Keep registry references inside this scope; subsequent refreshes invalidate them.
  {
   const auto& Registry=Database.GetAllActions();
   for (const auto& Entry:Registry)
   {
    const UObject* Owner=Entry.Key.ResolveObjectPtr();
    if (!Owner) continue;
    for (const UBlueprintNodeSpawner* Spawner:Entry.Value)
    {
     if (!Spawner) continue;
     FBlueprintActionInfo Info(Owner,Spawner);
     Observe(Spawner,Info.GetAssociatedFunction(),Unfiltered);
    }
   }
  }
  Ok&=Check(TEXT("actual database"),Unfiltered);

  FBlueprintActionFilter Filter;
  FBlueprintActionMenuBuilder Menu(FBlueprintActionMenuBuilder::DefaultConfig);
  Menu.AddMenuSection(Filter);
  Menu.RebuildActionList();
  Ok&=TestEqual(TEXT("default menu build completes synchronously"),Menu.GetNumPendingActions(),0);
  FObservedActions Visible;
  for (int32 Index=0;Index<Menu.GetNumActions();++Index)
  {
   const TSharedPtr<FEdGraphSchemaAction>& Action=Menu.GetSchemaAction(Index);
   if (!Action.IsValid() || Action->GetTypeId()!=FBlueprintActionMenuItem::StaticGetTypeId()) continue;
   const auto* Item=static_cast<const FBlueprintActionMenuItem*>(Action.Get());
   const UBlueprintNodeSpawner* Spawner=Item->GetRawAction();
   const auto* FunctionSpawner=Cast<UBlueprintFunctionNodeSpawner>(Spawner);
   Observe(Spawner,FunctionSpawner ? FunctionSpawner->GetFunction() : nullptr,Visible);
  }
  Ok&=Check(TEXT("actual standard menu"),Visible);
  AddInfo(FString::Printf(TEXT("ActionMenu exposure=%d frozen=109 callable=108 helper=1 databaseLegacy=%d databaseOldNodes=%d databasePositive=%d databaseV2Make=%d menuLegacy=%d menuOldNodes=%d menuPositive=%d menuV2Make=%d"),
   Expose ? 1 : 0,Unfiltered.LegacyFunctions,Unfiltered.LegacyNodes,Unfiltered.PositiveFunctions.Num(),Unfiltered.V2MakeJson,
   Visible.LegacyFunctions,Visible.LegacyNodes,Visible.PositiveFunctions.Num(),Visible.V2MakeJson));
 }
 return Ok;
}
#endif
