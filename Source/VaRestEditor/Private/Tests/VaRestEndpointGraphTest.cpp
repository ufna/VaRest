// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestEndpointGraphTestTypes.h"
#include "K2Node_VaRestCallEndpoint.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestRequestAction.h"
#include "VaRestHub.h"
#include "VaRestFakeTransport.h"
#include "VaRestTrace.h"
#include "VaRestJsonRules.h"
#include "VaRestHttpTesting.h"
#include "CoreGlobals.h"
#include "BlueprintActionDatabase.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRestEndpointGraphTest
{
template <typename T> T* Node(UEdGraph* Graph)
{ auto* Result=NewObject<T>(Graph); Graph->AddNode(Result,false,false); Result->CreateNewGuid(); return Result; }
UK2Node_VariableGet* Get(UEdGraph* Graph,FName Name)
{ auto* Result=Node<UK2Node_VariableGet>(Graph); Result->VariableReference.SetSelfMember(Name); Result->AllocateDefaultPins(); return Result; }
UK2Node_VariableSet* Set(UEdGraph* Graph,FName Name)
{ auto* Result=Node<UK2Node_VariableSet>(Graph); Result->VariableReference.SetSelfMember(Name); Result->AllocateDefaultPins(); return Result; }
FVaRestTypeRef Type(EVaRestTypeKind Kind,bool Json=false,EVaRestTypeContainer Container=EVaRestTypeContainer::None)
{ FVaRestTypeRef Result; Result.Kind=Kind; Result.bJsonValue=Json; Result.Container=Container; return Result; }
bool Exact(const FString& A,const FString& B)
{ return A.Len()==B.Len() && FMemory::Memcmp(*A,*B,A.Len()*sizeof(TCHAR))==0; }
class FScope final : public IVaRestTraceSink
{
public:
 uint64 Id=0; bool Closed=false;
 virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings D; D.Flags=EVaRestTraceDetail::Metadata; return D; }
 virtual void OnAttached(const FVaRestTraceSnapshot&) override {}
 virtual void OnEvent(const FVaRestTraceEvent& Event) override
 { if (Event.Kind==EVaRestTraceKind::Scope) { const auto& S=Event.Payload.Get<FVaRestTraceScope>(); if (Id && S.ScopeId==Id && !S.bOpened) Closed=true; } }
};
class FRun final : public IAutomationLatentCommand
{
public:
 explicit FRun(FAutomationTestBase& InTest):Test(InTest) {}
 ~FRun() { Cleanup(); }
 virtual bool Update() override
 {
  if (!Begun) { Begun=true; if (!Begin()) { Cleanup(); return true; } }
  if (FPlatformTime::Seconds()>Deadline)
  { Test.AddError(FString::Printf(TEXT("endpoint graph case=%d closing=%d success=%d errors=%d unavailable=%d calls=%d inflight=%d last=%s"),Case,Closing,Owner ? Owner->Successes : -1,Owner ? Owner->Errors : -1,Owner ? Owner->Unavailable : -1,Fake->NumCalls(),Fake->NumInFlight(),Owner ? *Owner->Error.Message : TEXT("retired"))); Cleanup(); return true; }
  Clock->Advance(0);
  if (Closing)
  {
   if (!Scope->Closed) return false;
   Test.TestEqual(TEXT("all typed carriers retired before native Scope Closed"),VaRest::Http::Testing::LiveDynamicCarriersOnGT(),CarrierBaseline);
   Test.TestEqual(TEXT("owned clock timers retired"),Clock->NumPendingTimers(),0); Cleanup(); return true;
  }
  if (!Sent)
  {
   // Actual editor end frames settle the freshly compiled type registry; never simulate its frame counter.
   if (GFrameCounter<=BuiltFrame+1) return false;
   if (!PreActivationChecked)
   {
    FString Error; TStrongObjectPtr<UVaRestRequestAction> Prepared(UVaRestRequestAction::CreateEndpointRequest(Owner.Get(),Api.Get(),Api->Endpoints[0].Id,NAME_None,NAME_None,Error));
    if (!Test.TestNotNull(*FString::Printf(TEXT("settled preactivation factory: %s"),*Error),Prepared.Get())) { Cleanup(); return true; }
    Test.TestEqual(TEXT("unactivated action has no native request identity"),Prepared->GetHandle()->GetId(),uint64(0));
    const TWeakObjectPtr<UVaRestRequestAction> Weak(Prepared.Get()); Prepared.Reset(); CollectGarbage(RF_NoFlags);
    Test.TestFalse(TEXT("unactivated owned action is collected"),Weak.IsValid()); Test.TestEqual(TEXT("unactivated action never sends"),Fake->NumCalls(),0);
    PreActivationChecked=true; BuiltFrame=GFrameCounter; return false;
   }
   const int32 Terminals=Owner->Successes+Owner->Errors+Owner->Unavailable;
   Owner->ClearOnGT(); Owner->InsideStart=true;
   Owner->ProcessEvent(Owner->FindFunctionChecked(FName(FString::Printf(TEXT("Start%d"),Case))),nullptr);
   Owner->InsideStart=false;
   Test.TestEqual(TEXT("Then is the immediate admission/capture signal"),Owner->Thens,Case+1);
   Test.TestEqual(TEXT("no terminal callback inline with the graph start"),Owner->Successes+Owner->Errors+Owner->Unavailable,Terminals);
   if (Case==4) Owner->Input=TEXT("changed after Send Request");
   if (Case==5) Owner->InputBytes={42};
   Sent=true; return false;
  }
  if (Owner->Successes+Owner->Errors+Owner->Unavailable<Case+1 || Client->GetNativeClient().GetInFlightCount() || Fake->NumInFlight()) return false;
  Verify(); ++Case; Sent=false;
  if (Case==6) BeginClose();
  return false;
 }
private:
 void Wire(UEdGraphPin* A,UEdGraphPin* B)
 {
  const bool Valid=A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A,B); Linked=Valid && Linked;
  if (!Valid) Test.AddError(FString::Printf(TEXT("endpoint fixture link failed: %s -> %s"),A ? *A->PinName.ToString() : TEXT("missing"),B ? *B->PinName.ToString() : TEXT("missing")));
 }
 UEdGraphPin* Assign(UEdGraph* Graph,UEdGraphPin* Exec,FName Name,UEdGraphPin* Value)
 { auto* S=Set(Graph,Name); Wire(Exec,S->GetExecPin()); Wire(Value,S->FindPin(Name)); return S->GetThenPin(); }
 void Count(UEdGraph* Graph,UEdGraphPin* Exec,int32 Branch)
 {
  auto* Call=Node<UK2Node_CallFunction>(Graph); Call->SetFromFunction(UVaRestEndpointGraphTestOwner::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestEndpointGraphTestOwner,Count)));
  Call->AllocateDefaultPins(); Call->FindPinChecked(TEXT("Branch"))->DefaultValue=FString::FromInt(Branch); Wire(Exec,Call->GetExecPin());
 }
 void Param(FVaRestApiEndpoint& Endpoint,const TCHAR* Name,const TCHAR* Identity,FVaRestTypeRef Shape,const TCHAR* Default,bool Required=false,EVaRestEndpointParamLocation Location=EVaRestEndpointParamLocation::Query)
 {
  FVaRestEndpointParam P; P.Name=Name; P.PinName=Identity; P.Type=Shape; P.Default=Default; P.bRequired=Required; P.In=Location;
  if (Location==EVaRestEndpointParamLocation::Header) P.Style=EVaRestQueryStyle::Csv;
  Endpoint.Params.Add(MoveTemp(P));
 }
 bool Begin()
 {
  Isolation=MakeUnique<FVaRestTrace::FScopedSinkIsolation>(); Scope=MakeShared<FScope,ESPMode::ThreadSafe>(); FVaRestTrace::AddSink(Scope.ToSharedRef()); Attached=true;
  const FString Name=TEXT("VaRestEndpointGraph_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Package.Reset(NewObject<UPackage>(nullptr,*(TEXT("/Temp/")+Name),RF_Transient)); Instance.Reset(NewObject<UGameInstance>(GEngine));
  Instance->InitializeStandalone(*Name,Package.Get()); World.Reset(Instance->GetWorld());
  Hub.Reset(UVaRestHub::Get(World.Get())); FString Error;
  if (!Test.TestTrue(TEXT("real GI world and API subsystem Hub"),World && Hub) || !Test.TestTrue(TEXT("owned storage profile"),Hub->SetStorageProfile(Name,Error))) return false;
  Scope->Id=Hub->GetScopeId(); Api.Reset(NewObject<UVaRestApi>());
  FVaRestApiEnvironment Env; Env.Name=TEXT("Fixture"); Env.BaseUrl=TEXT("https://endpoint-graph.invalid"); Api->Environments.Add(Env); Api->DefaultEnvironment=Env.Name;
  Api->Retry=FVaRestRetryPolicy::Disabled(); Api->LogLevel=EVaRestLogLevel::None;
  for (int32 I=0;I<4;++I)
  {
   FVaRestApiEndpoint E; E.Name=FName(FString::Printf(TEXT("Case%d"),I)); E.Verb=I<2 ? TEXT("POST") : TEXT("GET"); E.Path=I==0 ? TEXT("/values/{account}") : FString::Printf(TEXT("/case%d"),I); E.AuthMode=EVaRestAuthMode::None;
   E.ResponseType=Type(I==1 ? EVaRestTypeKind::Bytes : EVaRestTypeKind::String,I!=2);
   if (I<2) E.RequestType=E.ResponseType;
   if (I==3) E.ErrorType=Type(EVaRestTypeKind::String,true);
   if (I==0)
   {
    E.DefaultQuery.Add({TEXT("fixed"),TEXT("base")});
    Param(E,TEXT("account"),TEXT("Account"),Type(EVaRestTypeKind::String),TEXT("space user"),true,EVaRestEndpointParamLocation::Path);
    Param(E,TEXT("required"),TEXT("DefaultWords"),Type(EVaRestTypeKind::String,false,EVaRestTypeContainer::Array),TEXT("[\"A B\",\"C/D\"]"),true);
    Param(E,TEXT("empty"),TEXT("EmptyNumbers"),Type(EVaRestTypeKind::Int,false,EVaRestTypeContainer::Array),TEXT("[]"),true);
    Param(E,TEXT("CaseWire"),TEXT("CaseValue"),Type(EVaRestTypeKind::String),TEXT("foo"));
    Param(E,TEXT("omit"),TEXT("OmittedWords"),Type(EVaRestTypeKind::String,false,EVaRestTypeContainer::Array),TEXT("[\"default\"]"));
    Param(E,TEXT("X-Linked"),TEXT("LinkedNumbers"),Type(EVaRestTypeKind::Int,false,EVaRestTypeContainer::Array),TEXT("[]"),true,EVaRestEndpointParamLocation::Header);
    Param(E,TEXT("removed"),TEXT("Removed"),Type(EVaRestTypeKind::String),TEXT("old")); E.Params.Last().bRemoved=true;
   }
   Api->Endpoints.Add(MoveTemp(E));
  }
  FVaRestApiValidationContext Validation; Api->Validate(Validation);
  for (const auto& Issue:Validation.Issues) if (!Issue.bWarning) Test.AddError(Issue.PropertyPath+TEXT(": ")+Issue.Message.ToString());
  if (Validation.HasErrors()) return false;
  Client.Reset(Hub->GetClient(Api.Get())); if (!Test.TestNotNull(TEXT("actual shared API client"),Client.Get())) return false;
  Fake->bCompleteOnWorker=false;
  Fake->Enqueue(FVaRestFakeResponse::Json(201,UTF8TEXT("\"Hello\\u0000world\"")));
  Fake->Enqueue(FVaRestFakeResponse::Json(200,UTF8TEXT("\"AP9BCg==\"")));
  Fake->Enqueue(FVaRestFakeResponse::Json(200,UTF8TEXT("\"raw value\"")));
  Fake->Enqueue(FVaRestFakeResponse::Json(422,UTF8TEXT("\"denied\"")));
  Fake->Enqueue(FVaRestFakeResponse::Json(200,UTF8TEXT("\"from generic\"")));
  Fake->Enqueue(FVaRestFakeResponse::Json(200,UTF8TEXT("\"AP9BCg==\"")));
  if (!Build()) return false;
  Owner->Client=Client.Get(); OriginalInput=Owner->Input;
  auto Native=Client->GetNativeClient(); Native.WithTransport(Fake).WithClock(Clock).WithConnectivity(FVaRestConnectivity::Make(Clock));
  BuiltFrame=GFrameCounter; CarrierBaseline=VaRest::Http::Testing::LiveDynamicCarriersOnGT(); Deadline=FPlatformTime::Seconds()+30;
  // A call in the actual compile frame must visibly refuse type admission, without sending or a forgotten-Send log.
  Owner->InsideStart=true; Owner->ProcessEvent(Owner->FindFunctionChecked(TEXT("Start0")),nullptr); Owner->InsideStart=false;
  if (!Test.TestTrue(TEXT("cold compile-frame admission refuses with explicit Config/ID0/null handle before any request"),
   Owner->Errors==1 && Owner->Successes==0 && Owner->Unavailable==0 && Owner->Thens==1 && Owner->Error.Kind==EVaRestErrorKind::Config && Owner->Http.RequestId==TEXT("0") && !Owner->Handle && Fake->NumCalls()==0)) return false;
  Owner->ClearOnGT(); Owner->Successes=0; Owner->Errors=0; Owner->Unavailable=0; Owner->Thens=0; Owner->Inline=false;
  return true;
 }
 bool Build()
 {
  Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(UVaRestEndpointGraphTestOwner::StaticClass(),GetTransientPackage(),MakeUniqueObjectName(GetTransientPackage(),UBlueprint::StaticClass(),TEXT("VaRestEndpointGraph")),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
  if (!Blueprint) return false;
  auto* Graph=FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(),TEXT("Requests"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass()); FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(),Graph);
  for (int32 I=0;I<6;++I)
  {
   auto* Start=Node<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName=FName(FString::Printf(TEXT("Start%d"),I)); Start->AllocateDefaultPins();
   UK2Node_VaRestRequestBase* Request=nullptr;
   if (I<4)
   {
    auto* Endpoint=Node<UK2Node_VaRestCallEndpoint>(Graph); Endpoint->Api=Api.Get(); Endpoint->EndpointId=Api->Endpoints[I].Id; Endpoint->EndpointName=TEXT("OldNameThatMustNotOverrideGuid"); Endpoint->AllocateDefaultPins(); Request=Endpoint;
   }
   else
   {
    auto* Send=Node<UK2Node_VaRestSendRequest>(Graph); Send->ResponseType=Type(I==4 ? EVaRestTypeKind::String : EVaRestTypeKind::Bytes,true); Send->AllocateDefaultPins(); Request=Send;
    Send->FindPinChecked(TEXT("Verb"))->DefaultValue=TEXT("POST"); Send->FindPinChecked(TEXT("Path"))->DefaultValue=I==4 ? TEXT("/send-string") : TEXT("/send-bytes");
    Send->FindPinChecked(TEXT("AuthMode"))->DefaultValue=StaticEnum<EVaRestAuthMode>()->GetNameStringByValue(static_cast<int64>(EVaRestAuthMode::None));
    Wire(Get(Graph,TEXT("Client"))->FindPin(TEXT("Client")),Send->FindPin(TEXT("Client")));
   }
   Wire(Start->GetThenPin(),Request->GetExecPin());
   if (I<2 || I>=4)
   {
    const FName BodyName=I==0 || I==4 ? FName(TEXT("Input")) : FName(TEXT("InputBytes"));
    Wire(Get(Graph,BodyName)->FindPin(BodyName),Request->FindPin(TEXT("Body")));
   }
   if (I==0)
   {
    Request->FindPinChecked(UK2Node_VaRestRequestBase::ParameterPin(TEXT("CaseValue")))->DefaultValue=TEXT("FOO");
    Wire(Get(Graph,TEXT("Numbers"))->FindPin(TEXT("Numbers")),Request->FindPin(UK2Node_VaRestRequestBase::ParameterPin(TEXT("LinkedNumbers"))));
    Wire(Get(Graph,TEXT("Input"))->FindPin(TEXT("Input")),Request->FindPin(UK2Node_VaRestRequestBase::ParameterPin(TEXT("Removed"))));
   }
   auto* Tail=Assign(Graph,Request->GetThenPin(),TEXT("Handle"),Request->FindPin(TEXT("Handle"))); Count(Graph,Tail,3);
   Tail=Assign(Graph,Request->FindPin(TEXT("OnSuccess")),I==1 || I==5 ? TEXT("Bytes") : TEXT("Text"),Request->FindPin(TEXT("Response")));
   Tail=Assign(Graph,Tail,TEXT("Http"),Request->FindPin(TEXT("Http"))); Tail=Assign(Graph,Tail,TEXT("Error"),Request->FindPin(TEXT("Error"))); Count(Graph,Tail,0);
   Tail=Request->FindPin(TEXT("OnError"));
   if (I==3) { Tail=Assign(Graph,Tail,TEXT("ErrorBody"),Request->FindPin(TEXT("ErrorBody"))); Tail=Assign(Graph,Tail,TEXT("ErrorBodyValid"),Request->FindPin(TEXT("ErrorBodyValid"))); }
   Tail=Assign(Graph,Tail,TEXT("Http"),Request->FindPin(TEXT("Http"))); Tail=Assign(Graph,Tail,TEXT("Error"),Request->FindPin(TEXT("Error"))); Count(Graph,Tail,1);
   Tail=Assign(Graph,Request->FindPin(TEXT("OnTypedOutputUnavailable")),TEXT("Error"),Request->FindPin(TEXT("Error"))); Count(Graph,Tail,2);
  }
  if (!Test.TestTrue(TEXT("all real Call Endpoint graph links are legal"),Linked)) return false;
  FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get()); FCompilerResultsLog Log;
  FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
  if (!Test.TestTrue(TEXT("complete Call Endpoint graph compiles"),Log.NumErrors==0 && Blueprint->Status!=BS_Error)) return false;
  auto* Class=Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass); if (!Test.TestTrue(TEXT("actual compiled event graph bytecode"),Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
  Owner.Reset(NewObject<UVaRestEndpointGraphTestOwner>(World.Get(),Class));
  const TCHAR InputUnits[]=TEXT("request\n\"quote\0tail");
  Owner->Input=FString::ConstructFromPtrSize(InputUnits,int32(UE_ARRAY_COUNT(InputUnits))-1); Owner->InputBytes={0,255,7}; Owner->Numbers={9,10};
  if (!Test.TestTrue(TEXT("fixture input really contains a counted interior NUL"),Owner->Input.Len()==19 && Owner->Input[14]==0)) return false;
  return Test.TestNotNull(TEXT("real persistent graph frame"),Class->GetPersistentUberGraphFrame(Owner.Get(),Class->UberGraphFunction));
 }
 void Verify()
 {
  Test.TestEqual(TEXT("no unavailable output"),Owner->Unavailable,0); Test.TestFalse(TEXT("terminal stayed noninline"),Owner->Inline); Test.TestFalse(TEXT("continuations stay on GT"),Owner->WrongThread.load(std::memory_order_acquire));
  if (!Test.TestEqual(TEXT("one physical request per graph start"),Fake->NumCalls(),Case+1))
  { Test.AddError(TEXT("endpoint result before expected wire request: ")+Owner->Error.Message); return; }
  const auto& Request=Fake->GetCall(Case).Request;
  Test.TestEqual(TEXT("metadata keeps actual wire identity"),Owner->Http.RequestId,LexToString(Request.RequestId));
  Test.TestTrue(TEXT("captured request handle is terminal and matches metadata"),Owner->Handle && !Owner->Handle->IsActive() && Owner->Handle->GetIdText()==Owner->Http.RequestId);
  Test.TestEqual(TEXT("HTTP status"),Owner->Http.Status,Case==0 ? 201 : Case==3 ? 422 : 200);
  if (Case==0)
  {
   const auto& Url=Request.Url;
   Test.TestTrue(TEXT("declared required path default is encoded"),Url.Contains(TEXT("/values/space%20user?"),ESearchCase::CaseSensitive));
   Test.TestTrue(TEXT("required array default expands ordered repeated query"),Url.Contains(TEXT("required=A%20B&required=C%2FD"),ESearchCase::CaseSensitive));
   Test.TestTrue(TEXT("case-only edit is transmitted"),Url.Contains(TEXT("CaseWire=FOO"),ESearchCase::CaseSensitive));
   Test.TestTrue(TEXT("endpoint default query retained"),Url.Contains(TEXT("fixed=base"),ESearchCase::CaseSensitive));
   Test.TestFalse(TEXT("unchanged optional array omitted"),Url.Contains(TEXT("omit="),ESearchCase::CaseSensitive)); Test.TestFalse(TEXT("removed linked pin never transmitted"),Url.Contains(TEXT("removed="),ESearchCase::CaseSensitive));
   Test.TestFalse(TEXT("required empty array emits no phantom element"),Url.Contains(TEXT("empty="),ESearchCase::CaseSensitive));
   Test.TestEqual(TEXT("linked array uses header CSV"),Request.Headers.Get(TEXT("X-Linked")),FString(TEXT("9,10")));
   const auto Wire=Fake->ReadBody(Case); auto Parsed=FVaRestJson::Parse(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Wire.GetData()),Wire.Num()));
   Test.TestTrue(TEXT("JSON scalar request encoded as a counted string"),Parsed.HasValue() && Parsed.GetValue().IsString() && Exact(Parsed.GetValue().AsString(),Owner->Input));
   const TCHAR ExpectedUnits[]=TEXT("Hello\0world"); const FString Expected=FString::ConstructFromPtrSize(ExpectedUnits,int32(UE_ARRAY_COUNT(ExpectedUnits))-1);
   Test.TestTrue(TEXT("fixture expected string really contains a counted interior NUL"),Expected.Len()==11 && Expected[5]==0);
   Test.TestTrue(TEXT("JSON string response is decoded without wire quotes and retains interior NUL"),Exact(Owner->Text,Expected));
  }
  else if (Case==1)
  {
   Test.TestTrue(TEXT("JSON base64 body is encoded from raw bytes"),Fake->ReadBody(Case)==TArray<uint8>({'"','A','P','8','H','"'}));
   Test.TestTrue(TEXT("JSON base64 response decodes to original binary"),Owner->Bytes==TArray<uint8>({0,255,65,10}));
  }
  else if (Case==2) Test.TestTrue(TEXT("raw string control keeps JSON wire quotes"),Exact(Owner->Text,TEXT("\"raw value\"")));
  else if (Case==3)
  {
   Test.TestEqual(TEXT("typed HTTP error retains primary classification"),Owner->Error.Kind,EVaRestErrorKind::Http);
   Test.TestTrue(TEXT("typed JSON string error body is available"),Owner->ErrorBodyValid && Exact(Owner->ErrorBody,TEXT("denied")));
  }
  else if (Case==4)
  {
   const FTCHARToUTF8 Encoded(*OriginalInput,OriginalInput.Len()); const auto Wire=Fake->ReadBody(Case);
   Test.TestTrue(TEXT("generic Send Request snapshots counted raw string before caller mutation"),Wire.Num()==Encoded.Length() && FMemory::Memcmp(Wire.GetData(),Encoded.Get(),Wire.Num())==0);
   Test.TestTrue(TEXT("generic static JSON string descriptor reaches typed result"),Exact(Owner->Text,TEXT("from generic")));
  }
  else
  {
   Test.TestTrue(TEXT("generic Send Request snapshots raw binary before caller mutation"),Fake->ReadBody(Case)==TArray<uint8>({0,255,7}));
   Test.TestTrue(TEXT("generic static JSON bytes descriptor reaches typed result"),Owner->Bytes==TArray<uint8>({0,255,65,10}));
  }
  Test.TestEqual(TEXT("success count"),Owner->Successes,Case+1-(Case>=3 ? 1 : 0)); Test.TestEqual(TEXT("only the HTTP-error case reports an error"),Owner->Errors,Case>=3 ? 1 : 0);
 }
 void BeginClose()
 {
  Closing=true;
  for (int32 I=0;I<Fake->NumCalls();++I) Fake->ReleaseCompletedClock(I);
  RetireOwner(); if (Api) { FBlueprintActionDatabase::Get().ClearAssetActions(Api.Get()); FVaRestJsonRules::UnregisterOwner(Api.Get()); }
  Blueprint.Reset(); Hub->DeinitializeOnGT(); Client.Reset();
 }
 void RetireOwner()
 {
  if (!Owner) return; Owner->ClearOnGT(); Owner->Client=nullptr; const TWeakObjectPtr<UVaRestEndpointGraphTestOwner> Weak(Owner.Get()); Owner.Reset(); CollectGarbage(RF_NoFlags);
  Test.TestFalse(TEXT("real Blueprint owner and frame collected before Hub close"),Weak.IsValid());
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  RetireOwner(); for (int32 I=0;I<Fake->NumCalls();++I) if (Fake->GetCall(I).bCompleted) Fake->ReleaseCompletedClock(I);
  if (Api) { FBlueprintActionDatabase::Get().ClearAssetActions(Api.Get()); FVaRestJsonRules::UnregisterOwner(Api.Get()); }
  if (Hub) Hub->DeinitializeOnGT(); Client.Reset(); Blueprint.Reset(); Api.Reset(); Hub.Reset();
  if (Instance) Instance->Shutdown();
  if (World) { World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World.Get()); if (World->IsRooted()) World->RemoveFromRoot(); World->SetGameInstance(nullptr); }
  World.Reset(); Instance.Reset(); Package.Reset();
  if (Attached) { FVaRestTrace::RemoveSink(Scope.ToSharedRef()); Attached=false; } Scope.Reset(); Isolation.Reset();
 }
 FAutomationTestBase& Test;
 TStrongObjectPtr<UPackage> Package; TStrongObjectPtr<UGameInstance> Instance; TStrongObjectPtr<UWorld> World;
 TStrongObjectPtr<UVaRestHub> Hub; TStrongObjectPtr<UVaRestApi> Api; TStrongObjectPtr<UVaRestApiClient> Client;
 TStrongObjectPtr<UBlueprint> Blueprint; TStrongObjectPtr<UVaRestEndpointGraphTestOwner> Owner;
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
 TSharedRef<FVaRestFakeTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeTransport,ESPMode::ThreadSafe>(Clock);
 TSharedPtr<FScope,ESPMode::ThreadSafe> Scope; TUniquePtr<FVaRestTrace::FScopedSinkIsolation> Isolation;
 FString OriginalInput;
 int32 Case=0,CarrierBaseline=0; double Deadline=0; uint64 BuiltFrame=0;
 bool Begun=false,Sent=false,Closing=false,Cleaned=false,Attached=false,Linked=true,PreActivationChecked=false;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestEndpointGraphTest,"VaRest.Api.Blueprint.EndpointIntegration",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestEndpointGraphTest::RunTest(const FString& Parameters)
{ ADD_LATENT_AUTOMATION_COMMAND(VaRestEndpointGraphTest::FRun(*this)); return true; }
#endif
