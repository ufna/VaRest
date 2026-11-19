// Additional TU for the sealed001 producer module. No second IMPLEMENT_MODULE and no reflected producer class.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreMinimal.h"
#include "EdGraph/EdGraph.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VaRestRequestJSON.h"
#include "VaRestSubsystem.h"

namespace VaRestOriginalTagFixture
{
bool CheckProducer(FAutomationTestBase& Test);
bool Compile(FAutomationTestBase& Test,UBlueprint& BP);
}

namespace VaRestOriginalRequestFixture
{
const TCHAR* MountRoot=TEXT("/VaRestM6Compat_2a5b51b7/");
const TCHAR* PackageName=TEXT("/VaRestM6Compat_2a5b51b7/BP_RequestFromTagV1");
struct FMount
{
 FString Directory=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("VaRestM6Compat_2a5b51b7"));
 bool bRegistered=false;
 void Register()
 {
  if (FPackageName::MountPointExists(MountRoot)) return;
  const FString Root=MountRoot,Content=Directory+TEXT("/");
  FPackageName::RegisterMountPoint(Root,Content);bRegistered=true;
  FCoreDelegates::OnEnginePreExit.AddLambda([Root,Content] { FPackageName::UnRegisterMountPoint(Root,Content); });
 }
 FString Filename() const { return Directory/TEXT("BP_RequestFromTagV1.uasset"); }
};
template<class T> T* Node(UEdGraph* Graph)
{
 auto* Result=NewObject<T>(Graph);Graph->AddNode(Result,false,false);Result->CreateNewGuid();return Result;
}
bool Connect(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UEdGraphPin* From,UEdGraphPin* To)
{
 return Test.TestTrue(TEXT("connect exact request fixture pins"),From && To && Schema.TryCreateConnection(From,To));
}
bool Variable(FAutomationTestBase& Test,UBlueprint& BP,FName Name,FName Category,UClass* ObjectClass=nullptr)
{
 FEdGraphPinType Type;Type.PinCategory=Category;Type.PinSubCategoryObject=ObjectClass;
 return Test.TestTrue(*FString::Printf(TEXT("declare %s"),*Name.ToString()),FBlueprintEditorUtils::AddMemberVariable(&BP,Name,Type));
}
UEdGraphPin* Read(UEdGraph* Graph,FName Name)
{
 auto* Get=Node<UK2Node_VariableGet>(Graph);Get->VariableReference.SetSelfMember(Name);Get->AllocateDefaultPins();return Get->FindPin(Name);
}
bool Store(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UEdGraph* Graph,FName Name,UEdGraphPin* Value,UEdGraphPin*& Flow)
{
 auto* Set=Node<UK2Node_VariableSet>(Graph);Set->VariableReference.SetSelfMember(Name);Set->AllocateDefaultPins();
 if (!Connect(Test,Schema,Value,Set->FindPin(Name)) || !Connect(Test,Schema,Flow,Set->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
 Flow=Set->FindPin(UEdGraphSchema_K2::PN_Then);return true;
}
UK2Node_CallFunction* Call(FAutomationTestBase& Test,UEdGraph* Graph,UClass* Owner,FName FunctionName)
{
 auto* Function=Owner ? Owner->FindFunctionByName(FunctionName) : nullptr;
 if (!Test.TestNotNull(*FunctionName.ToString(),Function)) return nullptr;
 auto* Result=Node<UK2Node_CallFunction>(Graph);Result->SetFromFunction(Function);Result->AllocateDefaultPins();return Result;
}
bool Advance(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UK2Node_CallFunction* CallNode,UEdGraphPin*& Flow)
{
 if (auto* Execute=CallNode->FindPin(UEdGraphSchema_K2::PN_Execute))
 {
  if (!Connect(Test,Schema,Flow,Execute)) return false;
  Flow=CallNode->FindPin(UEdGraphSchema_K2::PN_Then);
 }
 return true;
}
bool Record(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UEdGraph* Graph,UK2Node_CustomEvent* Event,FName Counter)
{
 auto* Request=Event->FindPin(TEXT("Request"));auto* Flow=Event->FindPin(UEdGraphSchema_K2::PN_Then);
 auto* Status=Call(Test,Graph,UVaRestRequestJSON::StaticClass(),TEXT("GetResponseCode"));
 auto* Body=Call(Test,Graph,UVaRestRequestJSON::StaticClass(),TEXT("GetResponseContentAsString"));
 auto* MathClass=FindObject<UClass>(nullptr,TEXT("/Script/Engine.KismetMathLibrary"));
 auto* Add=Call(Test,Graph,MathClass,TEXT("Add_IntInt"));
 if (!Request || !Status || !Body || !Add) return false;
 if (!Connect(Test,Schema,Request,Status->FindPin(UEdGraphSchema_K2::PN_Self)) || !Advance(Test,Schema,Status,Flow) ||
     !Store(Test,Schema,Graph,TEXT("StatusCode"),Status->FindPin(UEdGraphSchema_K2::PN_ReturnValue),Flow)) return false;
 if (!Connect(Test,Schema,Request,Body->FindPin(UEdGraphSchema_K2::PN_Self)) || !Advance(Test,Schema,Body,Flow) ||
     !Store(Test,Schema,Graph,TEXT("RawBody"),Body->FindPin(UEdGraphSchema_K2::PN_ReturnValue),Flow)) return false;
 auto* One=Add->FindPin(TEXT("B"));
 if (!Test.TestNotNull(TEXT("integer increment B pin"),One)) return false;
 Schema.TrySetDefaultValue(*One,TEXT("1"));
 if (!Test.TestEqual(TEXT("integer increment B default is one"),One->DefaultValue,FString(TEXT("1")))) return false;
 if (!Connect(Test,Schema,Read(Graph,Counter),Add->FindPin(TEXT("A"))) || !Advance(Test,Schema,Add,Flow)) return false;
 return Store(Test,Schema,Graph,Counter,Add->FindPin(UEdGraphSchema_K2::PN_ReturnValue),Flow);
}
bool Bind(FAutomationTestBase& Test,const UEdGraphSchema_K2& Schema,UEdGraph* Graph,FName PropertyName,FName EventName,FName Counter,UEdGraphPin*& Flow)
{
 const auto* Property=FindFProperty<FMulticastDelegateProperty>(UVaRestRequestJSON::StaticClass(),PropertyName);
 if (!Test.TestNotNull(*PropertyName.ToString(),Property)) return false;
 auto* Event=UK2Node_CustomEvent::CreateFromFunction(FVector2D::ZeroVector,Graph,EventName.ToString(),Property->SignatureFunction,false);
 if (!Test.TestNotNull(TEXT("real BP multicast callback event"),Event)) return false;
 auto* Add=Node<UK2Node_AddDelegate>(Graph);Add->SetFromProperty(Property,false,UVaRestRequestJSON::StaticClass());Add->AllocateDefaultPins();
 if (!Connect(Test,Schema,Read(Graph,TEXT("LiveRequest")),Add->FindPin(UEdGraphSchema_K2::PN_Self)) ||
     !Connect(Test,Schema,Event->GetDelegatePin(),Add->GetDelegatePin()) ||
     !Connect(Test,Schema,Flow,Add->FindPin(UEdGraphSchema_K2::PN_Execute))) return false;
 Flow=Add->FindPin(UEdGraphSchema_K2::PN_Then);
 return Record(Test,Schema,Graph,Event,Counter);
}
bool Build(FAutomationTestBase& Test,UBlueprint& BP)
{
 const auto& Schema=*GetDefault<UEdGraphSchema_K2>();
 if (!Variable(Test,BP,TEXT("LegacySubsystem"),Schema.PC_Object,UVaRestSubsystem::StaticClass()) ||
     !Variable(Test,BP,TEXT("Url"),Schema.PC_String) || !Variable(Test,BP,TEXT("LiveRequest"),Schema.PC_Object,UVaRestRequestJSON::StaticClass()) ||
     !Variable(Test,BP,TEXT("StatusCode"),Schema.PC_Int) || !Variable(Test,BP,TEXT("RawBody"),Schema.PC_String) ||
     !Variable(Test,BP,TEXT("CompletionCount"),Schema.PC_Int) || !Variable(Test,BP,TEXT("FailureCount"),Schema.PC_Int)) return false;
 auto* Graph=FBlueprintEditorUtils::CreateNewGraph(&BP,TEXT("OriginalV1Request"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
 FBlueprintEditorUtils::AddUbergraphPage(&BP,Graph);
 auto* Prepare=Node<UK2Node_CustomEvent>(Graph);Prepare->CustomFunctionName=TEXT("PrepareLocalGet");Prepare->AllocateDefaultPins();
 auto* Flow=Prepare->FindPin(UEdGraphSchema_K2::PN_Then);
 auto* Construct=Call(Test,Graph,UVaRestSubsystem::StaticClass(),TEXT("ConstructVaRestRequestExt"));
 if (!Construct) return false;
 auto* Verb=Construct->FindPin(TEXT("Verb"));auto* Content=Construct->FindPin(TEXT("ContentType"));
 if (!Test.TestNotNull(TEXT("GET verb pin"),Verb) || !Test.TestNotNull(TEXT("JSON content type pin"),Content)) return false;
 Schema.TrySetDefaultValue(*Verb,TEXT("GET"));
 Schema.TrySetDefaultValue(*Content,TEXT("json"));
 if (!Test.TestEqual(TEXT("request verb default is GET"),Verb->DefaultValue,FString(TEXT("GET"))) ||
     !Test.TestEqual(TEXT("request content type default is json"),Content->DefaultValue,FString(TEXT("json")))) return false;
 if (!Connect(Test,Schema,Read(Graph,TEXT("LegacySubsystem")),Construct->FindPin(UEdGraphSchema_K2::PN_Self)) || !Advance(Test,Schema,Construct,Flow) ||
     !Store(Test,Schema,Graph,TEXT("LiveRequest"),Construct->FindPin(UEdGraphSchema_K2::PN_ReturnValue),Flow)) return false;
 if (!Bind(Test,Schema,Graph,TEXT("OnRequestComplete"),TEXT("RecordComplete"),TEXT("CompletionCount"),Flow) ||
     !Bind(Test,Schema,Graph,TEXT("OnRequestFail"),TEXT("RecordFailure"),TEXT("FailureCount"),Flow)) return false;
 auto* Start=Node<UK2Node_CustomEvent>(Graph);Start->CustomFunctionName=TEXT("StartLocalGet");Start->AllocateDefaultPins();
 Flow=Start->FindPin(UEdGraphSchema_K2::PN_Then);
 auto* Process=Call(Test,Graph,UVaRestRequestJSON::StaticClass(),TEXT("ProcessURL"));
 if (!Process || !Connect(Test,Schema,Read(Graph,TEXT("LiveRequest")),Process->FindPin(UEdGraphSchema_K2::PN_Self)) ||
     !Connect(Test,Schema,Read(Graph,TEXT("Url")),Process->FindPin(TEXT("Url"))) || !Advance(Test,Schema,Process,Flow)) return false;
 FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&BP);
 // Compile only: neither request event is invoked by the producer.
 return VaRestOriginalTagFixture::Compile(Test,BP);
}
FString DedicatedPhase()
{
 FString Phase;
 if (!FCommandLine::IsInitialized() || !FParse::Param(FCommandLine::Get(),TEXT("unattended")) || !FParse::Param(FCommandLine::Get(),TEXT("nowrite")) ||
     !FParse::Value(FCommandLine::Get(),TEXT("VaRestLegacyOriginalProducerPhase="),Phase) || !Phase.Equals(TEXT("GenerateRequest"),ESearchCase::CaseSensitive)) return {};
 return Phase;
}
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestOriginalRequestProducerTest,"VaRest.Compat.OriginalTagRequestProducer",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestOriginalRequestProducerTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
 const FString Phase=VaRestOriginalRequestFixture::DedicatedPhase();if (!Phase.IsEmpty()) { Names.Add(Phase);Commands.Add(Phase); }
}
bool FVaRestOriginalRequestProducerTest::RunTest(const FString& Parameters)
{
 using namespace VaRestOriginalRequestFixture;
 const FString Phase=DedicatedPhase();
 if (Phase.IsEmpty() || !Parameters.Equals(Phase,ESearchCase::CaseSensitive)) { AddError(TEXT("Request producer requires exact GenerateRequest/unattended/nowrite opt-in."));return false; }
 if (!TestTrue(TEXT("request BP author runs on GT"),IsInGameThread()) || !VaRestOriginalTagFixture::CheckProducer(*this)) return false;
 FMount Mount;
 if (!TestFalse(TEXT("never reuse a producer output directory"),IFileManager::Get().DirectoryExists(*Mount.Directory)) ||
     !TestFalse(TEXT("never overwrite a request fixture"),IFileManager::Get().FileExists(*Mount.Filename())) ||
     !TestNull(TEXT("never reuse a resident request package"),FindPackage(nullptr,PackageName))) return false;
 // Mount subscribers may create this initially absent directory. Permit only an empty owned namespace.
 Mount.Register();
 if (!TestTrue(TEXT("new owned request mount"),Mount.bRegistered)) return false;
 TArray<FString> MountedEntries;
 IFileManager::Get().FindFiles(MountedEntries,*(Mount.Directory/TEXT("*")),true,true);
 if (!TestEqual(TEXT("mount creates no request directory entries"),MountedEntries.Num(),0) ||
     !TestFalse(TEXT("mount never overwrites a request fixture"),IFileManager::Get().FileExists(*Mount.Filename())) ||
     !TestNull(TEXT("mount never reuses a resident request package"),FindPackage(nullptr,PackageName))) return false;
 TStrongObjectPtr<UPackage> Package(CreatePackage(PackageName));
 TStrongObjectPtr<UBlueprint> BP(FKismetEditorUtilities::CreateBlueprint(AActor::StaticClass(),Package.Get(),TEXT("BP_RequestFromTagV1"),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
 if (!TestNotNull(TEXT("actual original request BP"),BP.Get()) || !Build(*this,*BP)) return false;
 if (!TestTrue(TEXT("create owned request artifact directory"),IFileManager::Get().MakeDirectory(*Mount.Directory,true))) return false;
 FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;Args.SaveFlags=SAVE_NoError;
 if (!TestTrue(TEXT("serialize original authored request BP"),UPackage::SavePackage(Package.Get(),BP.Get(),*Mount.Filename(),Args))) return false;
 AddInfo(TEXT("OriginalV1RequestFixtureArtifact ")+Mount.Filename());
 AddInfo(TEXT("Request BP authored/compiled/saved only: PrepareLocalGet and StartLocalGet were NOT invoked; no producer HTTP."));
 return true;
}
#endif
