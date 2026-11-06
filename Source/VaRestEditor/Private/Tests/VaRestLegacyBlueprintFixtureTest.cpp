// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Misc/AutomationTest.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VaRest_BreakJson.h"

namespace VaRestLegacyBlueprintFixtureTest
{
#include "VaRestLegacyV1TagFunctions.inl"
// The tag exposes 108 callable functions. Its 109th UFUNCTION is used only
// by the original Make Json compiler handler, never by a CallFunction node.
bool IsCustomNodeHelper(const FTaggedFunction& Row)
{
 return FCString::Strcmp(Row.Owner,TEXT("/Script/VaRest.VaRestSubsystem"))==0
  && FCString::Strcmp(Row.Name,TEXT("StaticConstructVaRestJsonObject"))==0;
}
const TCHAR* MountRoot=TEXT("/VaRestM6Compat_2a5b51b7/");
const TCHAR* PackageName=TEXT("/VaRestM6Compat_2a5b51b7/BP_FromTagV1");
struct FMount
{
 FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("VaRestM6Compat_2a5b51b7"));
 FMount()
 {
  if (FPackageName::MountPointExists(MountRoot)) return;
  const FString Root=MountRoot,Content=Directory+TEXT("/");
  FPackageName::RegisterMountPoint(Root,Content);
  // Save queues Editor validation for a later tick. Its localization validator
  // still needs this root after RunTest returns. Retain only native path values
  // until the process's explicit pre-exit boundary, before core modules stop.
  FCoreDelegates::OnEnginePreExit.AddLambda([Root,Content] { FPackageName::UnRegisterMountPoint(Root,Content); });
 }
 FString Filename() const { return Directory/TEXT("BP_FromTagV1.uasset"); }
};
template<class T> T* Node(UEdGraph* Graph)
{
 auto* Result=NewObject<T>(Graph); Graph->AddNode(Result,false,false); Result->CreateNewGuid(); return Result;
}
FName Variable(int32 Index,FName Parameter)
{ return FName(*FString::Printf(TEXT("Compat_%d_%s"),Index,*Parameter.ToString())); }
bool AutoParameter(const UFunction& Function,FName Name)
{
 const FString Text=Name.ToString();
 return Text==Function.GetMetaData(TEXT("WorldContext")) || Text==Function.GetMetaData(TEXT("LatentInfo"));
}
bool AddVariable(FAutomationTestBase& Test,UBlueprint* BP,FName Name,FEdGraphPinType Type)
{
 Type.bIsReference=false; Type.bIsConst=false;
 return Test.TestTrue(*FString::Printf(TEXT("declare %s"),*Name.ToString()),FBlueprintEditorUtils::AddMemberVariable(BP,Name,Type));
}
UEdGraphPin* Read(UEdGraph* Graph,FName Name)
{
 auto* Get=Node<UK2Node_VariableGet>(Graph); Get->VariableReference.SetSelfMember(Name); Get->AllocateDefaultPins(); return Get->FindPin(Name);
}
bool Connect(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UEdGraphPin* From,UEdGraphPin* To)
{
 return Test.TestTrue(*FString::Printf(TEXT("connect %s to %s"),From ? *From->PinName.ToString() : TEXT("missing"),To ? *To->PinName.ToString() : TEXT("missing")),
  From && To && Schema.TryCreateConnection(From,To));
}
bool CaptureOutput(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UEdGraph* Graph,FName Name,UEdGraphPin* Value,UEdGraphPin*& Flow)
{
 auto* Set=Node<UK2Node_VariableSet>(Graph); Set->VariableReference.SetSelfMember(Name); Set->AllocateDefaultPins();
 if (!Connect(Test,Schema,Value,Set->FindPin(Name)) || !Connect(Test,Schema,Flow,Set->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
 Flow=Set->FindPin(UEdGraphSchema_K2::PN_Then); return true;
}
bool Validate(FAutomationTestBase& Test,UBlueprint& BP)
{
 TSet<FString> Seen; int32 Calls=0,CustomMake=0,CustomBreak=0;
 TArray<UEdGraph*> Graphs; BP.GetAllGraphs(Graphs);
 for (const auto* Graph:Graphs) for (const UEdGraphNode* Raw:Graph->Nodes)
 {
  CustomMake+=Raw->IsA<UVaRest_MakeJson>(); CustomBreak+=Raw->IsA<UVaRest_BreakJson>();
  const auto* Call=Cast<UK2Node_CallFunction>(Raw); if (!Call) continue;
  const UFunction* Function=Call->GetTargetFunction(); if (!Function || Function->GetOutermost()->GetName()!=TEXT("/Script/VaRest")) continue;
  const FString Owner=Function->GetOuterUClass()->GetPathName();
  const FTaggedFunction* Expected=nullptr;
  for (const auto& Row:TaggedFunctions) if (Owner==Row.Owner && Function->GetFName()==FName(Row.Name)) { Expected=&Row; break; }
  if (!Test.TestNotNull(TEXT("serialized legacy reference is present in the frozen tag"),Expected)) return false;
  TArray<FString> Parameters; FString(Expected->Parameters).ParseIntoArray(Parameters,TEXT("|"),true);
  int32 NativeParameters=0;
  for (TFieldIterator<FProperty> It(Function);It;++It) if (It->HasAnyPropertyFlags(CPF_Parm) && !It->HasAnyPropertyFlags(CPF_ReturnParm)) ++NativeParameters;
  Test.TestEqual(TEXT("tag parameter count"),NativeParameters,Parameters.Num());
  for (const auto& Parameter:Parameters) Test.TestNotNull(*FString::Printf(TEXT("tag pin %s.%s"),Expected->Name,*Parameter),Call->FindPin(FName(*Parameter)));
  ++Calls; Seen.Add(Owner+TEXT(".")+Expected->Name);
 }
 Test.TestEqual(TEXT("all 108 callable tag function nodes retained"),Calls,108);
 Test.TestEqual(TEXT("all 108 callable frozen identities are distinct"),Seen.Num(),108);
 Test.TestEqual(TEXT("old Make Json class retained"),CustomMake,1); Test.TestEqual(TEXT("old Break Json class retained"),CustomBreak,1);
 auto* Subsystem=FindObject<UClass>(nullptr,TEXT("/Script/VaRest.VaRestSubsystem"));
 const auto* Helper=Subsystem ? Subsystem->FindFunctionByName(TEXT("StaticConstructVaRestJsonObject")) : nullptr;
 if (!Test.TestNotNull(TEXT("109th tag function: original Make Json helper"),Helper)) return false;
 Test.TestFalse(TEXT("tag helper retains its internal non-callable flags"),Helper->HasAnyFunctionFlags(FUNC_BlueprintCallable|FUNC_BlueprintPure));
 return Calls==108 && Seen.Num()==108 && CustomMake==1 && CustomBreak==1;
}
bool Compile(FAutomationTestBase& Test,UBlueprint& BP)
{
 FCompilerResultsLog Log; FKismetEditorUtilities::CompileBlueprint(&BP,EBlueprintCompileOptions::SkipGarbageCollection,&Log);
 Test.AddInfo(FString::Printf(TEXT("V1 tag fixture compile: errors=%d warnings=%d tag=%s"),Log.NumErrors,Log.NumWarnings,TagCommit));
 Test.TestEqual(TEXT("tag fixture compiles without errors"),Log.NumErrors,0);
 return Log.NumErrors==0 && BP.Status!=BS_Error;
}
bool ExecuteCustomProjection(FAutomationTestBase& Test,UBlueprint& BP)
{
 auto* Instance=BP.GeneratedClass ? BP.GeneratedClass->GetDefaultObject() : nullptr;
 if (!Test.TestNotNull(TEXT("compiled fixture CDO"),Instance)) return false;
 auto* Function=Instance->FindFunction(TEXT("CustomProjection"));
 auto* Result=FindFProperty<FStrProperty>(BP.GeneratedClass,TEXT("CustomProjectionResult"));
 if (!Test.TestNotNull(TEXT("compiled old Make/Break event"),Function) || !Test.TestNotNull(TEXT("compiled projection result"),Result)) return false;
 Result->SetPropertyValue_InContainer(Instance,TEXT(""));
 Instance->ProcessEvent(Function,nullptr);
 return Test.TestEqual(TEXT("original Make/Break compiler handlers execute the internal tag helper"),Result->GetPropertyValue_InContainer(Instance),FString(TEXT("kept")));
}
bool Build(FAutomationTestBase& Test,UBlueprint& BP)
{
 const auto& Schema=*GetDefault<UEdGraphSchema_K2>();
 TArray<UFunction*> Functions;
 for (int32 Index=0;Index<UE_ARRAY_COUNT(TaggedFunctions);++Index)
 {
  const auto& Row=TaggedFunctions[Index]; UClass* Owner=FindObject<UClass>(nullptr,Row.Owner);
  if (!Test.TestNotNull(Row.Owner,Owner)) return false;
  UFunction* Function=Owner->FindFunctionByName(Row.Name); if (!Test.TestNotNull(Row.Name,Function)) return false;
  Functions.Add(Function);
  if (IsCustomNodeHelper(Row)) continue;
  if (!Function->HasAnyFunctionFlags(FUNC_Static))
  {
   FEdGraphPinType Type; Type.PinCategory=Schema.PC_Object; Type.PinSubCategoryObject=Owner;
   if (!AddVariable(Test,&BP,Variable(Index,TEXT("Target")),Type)) return false;
  }
  for (TFieldIterator<FProperty> It(Function);It;++It)
  {
   if (!It->HasAnyPropertyFlags(CPF_Parm) || AutoParameter(*Function,It->GetFName()) || It->IsA<FDelegateProperty>()) continue;
   FEdGraphPinType Type;
   if (!Test.TestTrue(TEXT("native tag parameter has a Blueprint type"),Schema.ConvertPropertyToPinType(*It,Type)) || !AddVariable(Test,&BP,Variable(Index,It->GetFName()),Type)) return false;
  }
 }
 FEdGraphPinType StringType; StringType.PinCategory=Schema.PC_String;
 if (!AddVariable(Test,&BP,TEXT("CustomProjectionResult"),StringType)) return false;
 auto* Graph=FBlueprintEditorUtils::CreateNewGraph(&BP,TEXT("FrozenV1Calls"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
 FBlueprintEditorUtils::AddUbergraphPage(&BP,Graph);
 for (int32 Index=0;Index<Functions.Num();++Index)
 {
  if (IsCustomNodeHelper(TaggedFunctions[Index])) continue;
  UFunction* Function=Functions[Index];
  auto* Start=Node<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName=Variable(Index,TEXT("Invoke")); Start->AllocateDefaultPins();
  auto* Call=Node<UK2Node_CallFunction>(Graph); Call->SetFromFunction(Function); Call->AllocateDefaultPins();
  if (!Function->HasAnyFunctionFlags(FUNC_Static) && !Connect(Test,Schema,Read(Graph,Variable(Index,TEXT("Target"))),Call->FindPin(Schema.PN_Self))) return false;
  for (auto* Pin:Call->Pins)
  {
   if (Pin->Direction!=EGPD_Input || Pin->PinType.PinCategory==Schema.PC_Exec || Pin->PinName==Schema.PN_Self || AutoParameter(*Function,Pin->PinName)) continue;
   const auto* Property=FindFProperty<FProperty>(Function,Pin->PinName); if (!Property) continue;
   if (const auto* Delegate=CastField<FDelegateProperty>(Property))
   {
    auto* Callback=UK2Node_CustomEvent::CreateFromFunction(FVector2D::ZeroVector,Graph,Variable(Index,TEXT("Callback")).ToString(),Delegate->SignatureFunction,false);
    if (!Connect(Test,Schema,Callback->GetDelegatePin(),Pin)) return false;
   }
   else if (!Connect(Test,Schema,Read(Graph,Variable(Index,Pin->PinName)),Pin)) return false;
  }
  UEdGraphPin* Flow=Start->FindPin(Schema.PN_Then);
  if (auto* Execute=Call->FindPin(Schema.PN_Execute))
  { if (!Connect(Test,Schema,Flow,Execute)) return false; Flow=Call->FindPin(Schema.PN_Then); }
  for (auto* Pin:Call->Pins) if (Pin->Direction==EGPD_Output && Pin->PinType.PinCategory!=Schema.PC_Exec)
   if (!CaptureOutput(Test,Schema,Graph,Variable(Index,Pin->PinName),Pin,Flow)) return false;
 }
 auto* Start=Node<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName=TEXT("CustomProjection"); Start->AllocateDefaultPins();
 FVaRest_NamedType Field; Field.Name=TEXT("value"); Field.Type=EVaRest_JsonType::JSON_String;
 auto* Make=Node<UVaRest_MakeJson>(Graph); Make->Inputs.Add(Field); Make->AllocateDefaultPins();
 auto* Break=Node<UVaRest_BreakJson>(Graph); Break->Outputs.Add(Field); Break->AllocateDefaultPins();
 Schema.TrySetDefaultValue(*Make->FindPinChecked(TEXT("value")),TEXT("kept"));
 if (!Connect(Test,Schema,Make->FindPin(TEXT("Target")),Break->FindPin(TEXT("Target")))) return false;
 UEdGraphPin* Flow=Start->FindPin(Schema.PN_Then);
 if (!CaptureOutput(Test,Schema,Graph,TEXT("CustomProjectionResult"),Break->FindPin(TEXT("value")),Flow)) return false;
 FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&BP);
 return Validate(Test,BP) && Compile(Test,BP) && ExecuteCustomProjection(Test,BP);
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyTagGenerateTest,"VaRest.Compat.TagFixture.Generate",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyTagGenerateTest::RunTest(const FString& Parameters)
{
 using namespace VaRestLegacyBlueprintFixtureTest;
 FMount Mount;
 if (!TestFalse(TEXT("never overwrite an existing fixture; inspect/load it instead"),IFileManager::Get().FileExists(*Mount.Filename()))) return false;
 TStrongObjectPtr<UPackage> Package(CreatePackage(PackageName));
 TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),TEXT("BP_FromTagV1"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
 if (!TestNotNull(TEXT("owned tag fixture blueprint"),BP.Get()) || !Build(*this,*BP)) return false;
 if (!TestTrue(TEXT("create owned artifact directory"),IFileManager::Get().MakeDirectory(*Mount.Directory,true))) return false;
 FSavePackageArgs Args; Args.TopLevelFlags=RF_Public|RF_Standalone; Args.SaveFlags=SAVE_NoError;
 if (!TestTrue(TEXT("save actual serialized tag fixture"),UPackage::SavePackage(Package.Get(),BP.Get(),*Mount.Filename(),Args))) return false;
 AddInfo(TEXT("V1FixtureArtifact ")+Mount.Filename());
 return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLegacyTagLoadTest,"VaRest.Compat.TagFixture.Load",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestLegacyTagLoadTest::RunTest(const FString& Parameters)
{
 using namespace VaRestLegacyBlueprintFixtureTest;
 FMount Mount;
 if (!TestTrue(TEXT("separate-process fixture artifact exists"),IFileManager::Get().FileExists(*Mount.Filename()))) return false;
 if (!TestNull(TEXT("load proof starts without the fixture package in memory"),FindPackage(nullptr,PackageName))) return false;
 TStrongObjectPtr<UPackage> Package(LoadPackage(nullptr,PackageName,LOAD_None));
 if (!TestNotNull(TEXT("reload serialized fixture through the package loader"),Package.Get())) return false;
 TStrongObjectPtr<UBlueprint> BP(FindObject<UBlueprint>(Package.Get(),TEXT("BP_FromTagV1")));
 if (!TestNotNull(TEXT("loaded fixture blueprint"),BP.Get())) return false;
 AddInfo(TEXT("V1FixtureArtifact ")+Mount.Filename());
 return Validate(*this,*BP) && Compile(*this,*BP) && ExecuteCustomProjection(*this,*BP);
}
#endif
