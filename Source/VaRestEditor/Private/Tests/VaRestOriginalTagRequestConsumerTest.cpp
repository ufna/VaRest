// Proposal for the current VaRestEditor module; consumes DATAONLY original-provider BP, never generates/saves it.
#include "VaRestOriginalTagRequestConsumerTypes.h"
#include "VaRestRequestJSON.h"
void UVaRestOriginalTagRequestObserver::Complete(UVaRestRequestJSON* Request)
{
 if (!IsInGameThread()) bOnGT.Store(false);
 if (Request!=ExpectedRequest.Load()) bIdentity.Store(false);
 CompleteCount.FetchAdd(1);
}
void UVaRestOriginalTagRequestObserver::Fail(UVaRestRequestJSON* Request)
{
 if (!IsInGameThread()) bOnGT.Store(false);
 if (Request!=ExpectedRequest.Load()) bIdentity.Store(false);
 FailureCount.FetchAdd(1);
}

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "CoreGlobals.h"
#include "EdGraph/EdGraph.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "K2Node_AddDelegate.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "KismetCompiler.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/LexFromString.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"
#include "VaRestJsonObject.h"
#include "VaRestSubsystem.h"

namespace VaRestOriginalRequestConsumer
{
const TCHAR* MountRoot=TEXT("/VaRestM6Compat_2a5b51b7/");
const TCHAR* PackageName=TEXT("/VaRestM6Compat_2a5b51b7/BP_RequestFromTagV1");
const TCHAR* ExpectedBody=TEXT("{\"producer\":\"v1-on-5.8\",\"value\":17}");
bool Requested(FString& Url)
{
 if (!FCommandLine::IsInitialized() || !FParse::Param(FCommandLine::Get(),TEXT("VaRestLegacyOriginalRequestConsumer")) ||
     !FParse::Param(FCommandLine::Get(),TEXT("unattended")) || !FParse::Param(FCommandLine::Get(),TEXT("nowrite")) ||
     !FParse::Value(FCommandLine::Get(),TEXT("VaRestLegacyRequestConsumerUrl="),Url)) return false;
 const FString Prefix=TEXT("http://127.0.0.1:");
 if (!Url.StartsWith(Prefix,ESearchCase::CaseSensitive)) return false;
 const FString Tail=Url.Mid(Prefix.Len());int32 Slash=INDEX_NONE;
 if (!Tail.FindChar(TCHAR('/'),Slash) || Slash<=0 || Tail.Mid(Slash)!=TEXT("/varest-original-fixture")) return false;
 const FString PortText=Tail.Left(Slash);
 for (TCHAR Unit:PortText) if (Unit<TCHAR('0') || Unit>TCHAR('9')) return false;
 int32 Port=0;return LexTryParseString(Port,*PortText) && Port>0 && Port<=65535;
}
class FRun final : public IAutomationLatentCommand
{
public:
 FRun(FAutomationTestBase& InTest,FString InUrl):Test(InTest),Url(MoveTemp(InUrl)) {}
 virtual ~FRun() override { Cleanup(); }
 virtual bool Update() override
 {
  check(IsInGameThread());
  if (!bStarted)
  {
   bStarted=true;StartedAt=FPlatformTime::Seconds();
   if (!Setup()) { Cleanup();return true; }
   return false;
  }
  if (!Observer.IsValid() || !Request.IsValid()) { Test.AddError(TEXT("owned request consumer state missing"));return true; }
  const int32 NativeComplete=Observer->CompleteCount.Load(),NativeFail=Observer->FailureCount.Load();
  if (!Observer->bOnGT.Load() || !Observer->bIdentity.Load()) { Test.AddError(TEXT("native delegate violated GT/owned identity"));Cleanup();return true; }
  if (NativeFail || NativeComplete>1) { Test.AddError(TEXT("owned request failed or completed more than once"));Cleanup();return true; }
  if (FPlatformTime::Seconds()-StartedAt>20.0) { Test.AddError(TEXT("original BP local GET timed out; cancelling only its owned request"));Cleanup();return true; }
  if (!NativeComplete) return false;
  if (!FieldsValid()) { Cleanup();return true; }
  if (!bSawTerminal) { bSawTerminal=true;TerminalFrame=GFrameCounter;return false; }
  if (GFrameCounter<=TerminalFrame+1) return false;
  if (!FieldsValid()) { Cleanup();return true; }
  Test.AddInfo(FString::Printf(TEXT("OriginalV1RequestConsumer URL=%s GET status=200 BPcomplete=1 BPfail=0 nativeComplete=1 nativeFail=0 GT=1 raw=%s"),*Url,ExpectedBody));
  Cleanup();
  Test.AddInfo(TEXT("OriginalV1RequestConsumer retired owned script/native bindings, called Cancel and released owned roots; no global drain or physical-closure claim."));
  return true;
 }
private:
 bool Setup()
 {
  FString UserDir;
  if (!Test.TestTrue(TEXT("consumer requires explicit private UserDir"),FParse::Value(FCommandLine::Get(),TEXT("UserDir="),UserDir) && !FPaths::IsRelative(UserDir))) return false;
  FString ExpectedSaved=FPaths::ConvertRelativePathToFull(UserDir/TEXT("Saved"));FPaths::NormalizeDirectoryName(ExpectedSaved);
  FString ActualSaved=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir());FPaths::NormalizeDirectoryName(ActualSaved);
  if (!Test.TestEqual(TEXT("consumer Saved stays in private UserDir"),ActualSaved,ExpectedSaved)) return false;
  const FString Directory=ActualSaved/TEXT("VaRestM6Compat_2a5b51b7"),Filename=Directory/TEXT("BP_RequestFromTagV1.uasset");
  if (!Test.TestFalse(TEXT("consumer does not reuse a logical mount"),FPackageName::MountPointExists(MountRoot)) ||
      !Test.TestTrue(TEXT("original DATAONLY request fixture must already exist"),IFileManager::Get().FileExists(*Filename)) ||
      !Test.TestNull(TEXT("request package not resident before load"),FindPackage(nullptr,PackageName))) return false;
  const FString Root=MountRoot,Content=Directory+TEXT("/");FPackageName::RegisterMountPoint(Root,Content);
  FCoreDelegates::OnEnginePreExit.AddLambda([Root,Content] { FPackageName::UnRegisterMountPoint(Root,Content); });
  Package.Reset(LoadPackage(nullptr,PackageName,LOAD_None));
  if (!Test.TestNotNull(TEXT("load original request package"),Package.Get())) return false;
  Blueprint.Reset(FindObject<UBlueprint>(Package.Get(),TEXT("BP_RequestFromTagV1")));
  if (!Test.TestNotNull(TEXT("load original request blueprint"),Blueprint.Get()) || !GraphValid()) return false;
  FCompilerResultsLog Log;FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
  Test.AddInfo(FString::Printf(TEXT("Original request BP current-provider compile errors=%d warnings=%d"),Log.NumErrors,Log.NumWarnings));
  if (!Test.TestEqual(TEXT("original request BP compiles under current provider"),Log.NumErrors,0) || Blueprint->Status==BS_Error || !Blueprint->GeneratedClass) return false;
  Instance.Reset(Blueprint->GeneratedClass->GetDefaultObject());
  SubsystemProperty=FindFProperty<FObjectPropertyBase>(Blueprint->GeneratedClass,TEXT("LegacySubsystem"));
  UrlProperty=FindFProperty<FStrProperty>(Blueprint->GeneratedClass,TEXT("Url"));
  RequestProperty=FindFProperty<FObjectPropertyBase>(Blueprint->GeneratedClass,TEXT("LiveRequest"));
  StatusProperty=FindFProperty<FIntProperty>(Blueprint->GeneratedClass,TEXT("StatusCode"));
  BodyProperty=FindFProperty<FStrProperty>(Blueprint->GeneratedClass,TEXT("RawBody"));
  CompleteProperty=FindFProperty<FIntProperty>(Blueprint->GeneratedClass,TEXT("CompletionCount"));
  FailureProperty=FindFProperty<FIntProperty>(Blueprint->GeneratedClass,TEXT("FailureCount"));
  if (!Test.TestTrue(TEXT("saved BP has all original request fields"),Instance.IsValid() && SubsystemProperty && UrlProperty && RequestProperty && StatusProperty && BodyProperty && CompleteProperty && FailureProperty)) return false;
  if (!Test.TestNull(TEXT("fresh loaded CDO has no preexisting request"),RequestProperty->GetObjectPropertyValue_InContainer(Instance.Get())) ||
      !Test.TestEqual(TEXT("fresh BP completion count"),CompleteProperty->GetPropertyValue_InContainer(Instance.Get()),0) ||
      !Test.TestEqual(TEXT("fresh BP failure count"),FailureProperty->GetPropertyValue_InContainer(Instance.Get()),0) ||
      !Test.TestEqual(TEXT("fresh BP status cannot preclaim a response"),StatusProperty->GetPropertyValue_InContainer(Instance.Get()),0) ||
      !Test.TestTrue(TEXT("fresh BP raw body cannot preclaim a response"),BodyProperty->GetPropertyValue_InContainer(Instance.Get()).IsEmpty())) return false;
  auto* Subsystem=GEngine ? GEngine->GetEngineSubsystem<UVaRestSubsystem>() : nullptr;
  if (!Test.TestNotNull(TEXT("actual current legacy subsystem"),Subsystem)) return false;
  SubsystemProperty->SetObjectPropertyValue_InContainer(Instance.Get(),Subsystem);UrlProperty->SetPropertyValue_InContainer(Instance.Get(),Url);
  UFunction* Prepare=Instance->FindFunction(TEXT("PrepareLocalGet"));UFunction* Start=Instance->FindFunction(TEXT("StartLocalGet"));
  if (!Test.TestTrue(TEXT("saved BP defines both real request events"),Prepare && Start)) return false;
  Instance->ProcessEvent(Prepare,nullptr);
  Request.Reset(Cast<UVaRestRequestJSON>(RequestProperty->GetObjectPropertyValue_InContainer(Instance.Get())));
  if (!Test.TestNotNull(TEXT("saved BP itself constructed request"),Request.Get()) ||
      !Test.TestEqual(TEXT("BP prepared GET"),Request->GetVerb(),EVaRestRequestVerb::GET) ||
      !Test.TestEqual(TEXT("BP preparation did not start HTTP"),Request->GetStatus(),EVaRestRequestStatus::NotStarted) ||
      !Test.TestEqual(TEXT("prepare emits no completion callback"),CompleteProperty->GetPropertyValue_InContainer(Instance.Get()),0) ||
      !Test.TestEqual(TEXT("prepare emits no failure callback"),FailureProperty->GetPropertyValue_InContainer(Instance.Get()),0)) return false;
  Observer.Reset(NewObject<UVaRestOriginalTagRequestObserver>());Observer->ExpectedRequest.Store(Request.Get());
  Request->OnRequestComplete.AddDynamic(Observer.Get(),&UVaRestOriginalTagRequestObserver::Complete);
  Request->OnRequestFail.AddDynamic(Observer.Get(),&UVaRestOriginalTagRequestObserver::Fail);
  // The only request start is the saved BP's ProcessURL node, not a native request call.
  Instance->ProcessEvent(Start,nullptr);
  Test.AddInfo(TEXT("OriginalV1RequestConsumer loaded DATAONLY package and invoked saved BP request events; no asset generated or saved."));
  return true;
 }
 bool GraphValid()
 {
  int32 Construct=0,Process=0,CompleteBind=0,FailBind=0;
  TArray<UEdGraph*> Graphs;Blueprint->GetAllGraphs(Graphs);
  for (const auto* Graph:Graphs) for (const UEdGraphNode* Node:Graph->Nodes)
  {
   if (const auto* Call=Cast<UK2Node_CallFunction>(Node))
   {
    const UFunction* Function=Call->GetTargetFunction();
    if (!Test.TestNotNull(TEXT("loaded original callable resolves"),Function)) return false;
    if (Function->GetOuterUClass()==UVaRestSubsystem::StaticClass() && Function->GetFName()==TEXT("ConstructVaRestRequestExt")) ++Construct;
    if (Function->GetOuterUClass()==UVaRestRequestJSON::StaticClass() && Function->GetFName()==TEXT("ProcessURL")) ++Process;
   }
   if (const auto* Bind=Cast<UK2Node_AddDelegate>(Node))
   { CompleteBind+=Bind->GetPropertyName()==TEXT("OnRequestComplete");FailBind+=Bind->GetPropertyName()==TEXT("OnRequestFail"); }
  }
  return Test.TestTrue(TEXT("loaded saved graph retains actual construct/start/BP delegate nodes"),Construct==1 && Process==1 && CompleteBind==1 && FailBind==1);
 }
 bool FieldsValid()
 {
  const int32 Complete=CompleteProperty->GetPropertyValue_InContainer(Instance.Get()),Fail=FailureProperty->GetPropertyValue_InContainer(Instance.Get());
  const FString Body=BodyProperty->GetPropertyValue_InContainer(Instance.Get());
  auto* Json=Request->GetResponseObject();
  return Test.TestTrue(TEXT("both BP and native delegates delivered exactly once on GT"),Complete==1 && Fail==0 && Observer->CompleteCount.Load()==1 && Observer->FailureCount.Load()==0 && Observer->bOnGT.Load() && Observer->bIdentity.Load()) &&
   Test.TestEqual(TEXT("BP callback recorded actual HTTP status"),StatusProperty->GetPropertyValue_InContainer(Instance.Get()),200) &&
   Test.TestEqual(TEXT("BP callback recorded exact raw response body"),Body,FString(ExpectedBody)) &&
   Test.TestTrue(TEXT("actual JSON response has expected original marker/value"),Json && Json->GetStringField(TEXT("producer"))==TEXT("v1-on-5.8") && Json->GetIntegerField(TEXT("value"))==17) &&
   Test.TestEqual(TEXT("owned request is terminal"),Request->GetStatus(),EVaRestRequestStatus::Succeeded);
 }
 void Cleanup()
 {
  check(IsInGameThread());
  if (Request.IsValid())
  {
   if (Observer.IsValid())
   {
    Request->OnRequestComplete.RemoveDynamic(Observer.Get(),&UVaRestOriginalTagRequestObserver::Complete);
    Request->OnRequestFail.RemoveDynamic(Observer.Get(),&UVaRestOriginalTagRequestObserver::Fail);
   }
   // This request is created solely for this case; remove its owned BP callbacks before Cancel.
   Request->OnRequestComplete.Clear();Request->OnRequestFail.Clear();Request->Cancel();
  }
  if (Instance.IsValid())
  {
   if (RequestProperty) RequestProperty->SetObjectPropertyValue_InContainer(Instance.Get(),nullptr);
   if (SubsystemProperty) SubsystemProperty->SetObjectPropertyValue_InContainer(Instance.Get(),nullptr);
  }
  if (Observer.IsValid()) Observer->ExpectedRequest.Store(nullptr);
  Observer.Reset();Request.Reset();Instance.Reset();Blueprint.Reset();Package.Reset();
 }
 FAutomationTestBase& Test;FString Url;
 bool bStarted=false,bSawTerminal=false;double StartedAt=0;uint64 TerminalFrame=0;
 TStrongObjectPtr<UPackage> Package{nullptr};TStrongObjectPtr<UBlueprint> Blueprint{nullptr};TStrongObjectPtr<UObject> Instance{nullptr};
 TStrongObjectPtr<UVaRestRequestJSON> Request{nullptr};TStrongObjectPtr<UVaRestOriginalTagRequestObserver> Observer{nullptr};
 FObjectPropertyBase* SubsystemProperty=nullptr;FObjectPropertyBase* RequestProperty=nullptr;FStrProperty* UrlProperty=nullptr;FStrProperty* BodyProperty=nullptr;
 FIntProperty* StatusProperty=nullptr;FIntProperty* CompleteProperty=nullptr;FIntProperty* FailureProperty=nullptr;
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestOriginalTagRequestConsumerTest,"VaRest.Compat.OriginalTagRequest",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestOriginalTagRequestConsumerTest::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
 FString Url;if (VaRestOriginalRequestConsumer::Requested(Url)) { Names.Add(TEXT("LoadAndGet"));Commands.Add(TEXT("LoadAndGet")); }
}
bool FVaRestOriginalTagRequestConsumerTest::RunTest(const FString& Parameters)
{
 FString Url;
 if (!Parameters.Equals(TEXT("LoadAndGet"),ESearchCase::CaseSensitive) || !VaRestOriginalRequestConsumer::Requested(Url))
 { AddError(TEXT("Original BP consumer requires exact opt-in, private UserDir and validated loopback fixture URL."));return false; }
 ADD_LATENT_AUTOMATION_COMMAND(VaRestOriginalRequestConsumer::FRun(*this,Url));return true;
}
#endif
