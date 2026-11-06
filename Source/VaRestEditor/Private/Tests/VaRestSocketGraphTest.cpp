// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestSocketGraphTestTypes.h"
#include "K2Node_VaRestSocketAsync.h"
#include "K2Node_VaRestSocketCall.h"
#include "VaRestApi.h"
#include "VaRestSocketEndpoint.h"
#include "VaRestSocketHub.h"
#include "VaRestSocketLibrary.h"
#include "VaRestFakeSocketTransport.h"
#include "VaRestClock.h"
#include "VaRestJsonRules.h"
#include "BlueprintActionDatabase.h"
#include "CoreGlobals.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/AutomationTest.h"
#include "UObject/GarbageCollection.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRestSocketGraphTest
{
template <typename T> T* Node(UEdGraph* Graph)
{ auto* N=NewObject<T>(Graph); Graph->AddNode(N,false,false); N->CreateNewGuid(); return N; }
class FRun final : public IAutomationLatentCommand
{
public:
 explicit FRun(FAutomationTestBase& InTest):Test(InTest) {}
 ~FRun() { Cleanup(); }
 virtual bool Update() override
 {
  if (!Begun) { Begun=true; if (!Begin()) { Cleanup(); return true; } }
  if (FPlatformTime::Seconds()>Deadline || Owner->Errors || Owner->Unavailable || Owner->ConnectFailures)
  { Test.AddError(FString::Printf(TEXT("WS graph phase=%d ready=%d events=%d replies=%d errors=%d unavailable=%d connectFailed=%d last=%s"),Phase,Owner->Readies,Owner->Messages,Owner->Replies,Owner->Errors,Owner->Unavailable,Owner->ConnectFailures,*Owner->Error.Message)); Cleanup(); return true; }
  if (Phase==0)
  {
   if (!Socket.IsConnected() || !Fake->NumAttempts()) return false;
   Test.TestFalse(TEXT("real Connect action waits for application readiness"),Socket.IsReady());
   Fake->GetLast().Text(TEXT("{\"type\":\"welcome\",\"data\":{}}")); ++Phase;
  }
  else if (Phase==1)
  {
   if (Owner->Readies!=1 || !Socket.IsReady() || GFrameCounter<=BuiltFrame+1) return false;
   Owner->ProcessEvent(Owner->FindFunctionChecked(TEXT("Start")),nullptr);
   Listen.Reset(Owner->ListenAction); Request.Reset(Owner->RequestAction);
   if (!Test.TestTrue(TEXT("declared K2 factories create real active actions"),Listen && Request && !Listen->IsFinished() && !Request->IsFinished())) { Cleanup(); return true; }
   ++Phase;
  }
  else if (Phase==2)
  {
   const auto Ask=Sent(TEXT("ask")),Out=Sent(TEXT("out")); if (!Ask.IsValid() || !Out.IsValid()) return false;
   Test.TestTrue(TEXT("asset declared unlinked string request literal encoded"),Ask[TEXT("data")].IsString() && Ask[TEXT("data")].AsString()==TEXT("literal ask"));
   Test.TestTrue(TEXT("asset declared unlinked string emit literal encoded"),Out[TEXT("data")].IsString() && Out[TEXT("data")].AsString()==TEXT("literal emit"));
   Test.TestEqual(TEXT("declared emit is admitted"),Owner->EmitResult,EVaRestSendResult::Sent);
   Test.TestTrue(TEXT("actual request correlation identity exists"),Ask.Get(TEXT("id")).IsValid());
   Fake->GetLast().Text(TEXT("{\"type\":\"server\",\"id\":17,\"data\":\"from server\"}")); ++Phase;
  }
  else if (Phase==3)
  {
   if (Owner->Messages!=1 || PeerReplies()!=1) return false;
   Test.TestEqual(TEXT("typed incoming payload"),Owner->Received,FString(TEXT("from server")));
   Test.TestTrue(TEXT("native message metadata preserves numeric identity"),Owner->Message.bHasId && Owner->Message.bNumericId && Owner->Message.NumericId==17);
   Test.TestFalse(TEXT("all native message copies share the consumed reply token"),Owner->Message.CanReplyOnGT());
   Test.TestEqual(TEXT("compiled reply queues during dispatch"),Owner->FirstReply,EVaRestSendResult::Queued);
   Test.TestEqual(TEXT("second compiled reply is dropped"),Owner->SecondReply,EVaRestSendResult::Dropped);
   Test.TestEqual(TEXT("incoming decode warnings"),Owner->Warnings.Num(),0);
   const auto Ask=Sent(TEXT("ask"));
   Fake->GetLast().Text(FVaRestJson::Object().Set(TEXT("type"),TEXT("reply")).Set(TEXT("id"),Ask[TEXT("id")]).Set(TEXT("data"),TEXT("from peer")).ToString()); ++Phase;
  }
  else if (Phase==4)
  {
   if (Owner->Replies!=1) return false;
   Test.TestEqual(TEXT("typed correlated reply"),Owner->Reply,FString(TEXT("from peer")));
   Test.TestTrue(TEXT("request action finishes after typed delivery"),Request->IsFinished());
   Test.TestEqual(TEXT("all requests retired"),Socket.GetStats().PendingRequests,0);
   Test.TestEqual(TEXT("reply decode warnings"),Owner->Warnings.Num(),0);
   Listen->Stop(); Test.TestTrue(TEXT("listen action explicitly stops"),Listen->IsFinished());
   Test.TestTrue(TEXT("stopping actions preserves shared connection"),Socket.IsReady());
   Fake->GetLast().Text(TEXT("{\"type\":\"server\",\"id\":18,\"data\":\"ignored\"}")); StoppedFrame=GFrameCounter; ++Phase;
  }
  else
  {
   if (Socket.GetStats().FramesIn<4 || GFrameCounter<=StoppedFrame+1) return false;
   Test.TestEqual(TEXT("stopped typed listener receives no late event"),Owner->Messages,1);
   Test.TestEqual(TEXT("only one server reply packet"),PeerReplies(),1);
   Test.TestEqual(TEXT("one connect-ready callback"),Owner->Readies,1); Test.TestFalse(TEXT("callbacks stay on GT"),Owner->WrongThread.load(std::memory_order_acquire));
   Cleanup(); return true;
  }
  return false;
 }
private:
 FVaRestJson Sent(const TCHAR* Route) const
 {
  if (!Fake->NumAttempts()) return {};
  for (const auto& Frame:Fake->GetLast().GetSent())
  {
   if (Frame.Kind!=EVaRestSocketFrameKind::Text) continue; const auto Bytes=Frame.GetPayload();
   auto J=FVaRestJson::Parse(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num()));
   if (J.HasValue() && J.GetValue()[TEXT("type")].AsString()==Route) return J.StealValue();
  }
  return {};
 }
 int32 PeerReplies() const
 {
  int32 Count=0; if (!Fake->NumAttempts()) return Count;
  for (const auto& Frame:Fake->GetLast().GetSent())
  {
   if (Frame.Kind!=EVaRestSocketFrameKind::Text) continue; const auto Bytes=Frame.GetPayload();
   auto J=FVaRestJson::Parse(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num()));
   if (J.HasValue() && J.GetValue()[TEXT("type")].AsString()==TEXT("reply") && J.GetValue()[TEXT("id")].AsInt()==17)
   { Test.TestEqual(TEXT("declared reply literal on wire"),J.GetValue()[TEXT("data")].AsString(),FString(TEXT("literal reply"))); ++Count; }
  }
  return Count;
 }
 void Wire(UEdGraphPin* A,UEdGraphPin* B)
 {
  const bool Valid=A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A,B); Linked=Valid && Linked;
  if (!Valid) Test.AddError(FString::Printf(TEXT("WS fixture link failed: %s -> %s"),A ? *A->PinName.ToString() : TEXT("missing"),B ? *B->PinName.ToString() : TEXT("missing")));
 }
 UEdGraphPin* Assign(UEdGraph* Graph,UEdGraphPin* Exec,FName Name,UEdGraphPin* Value)
 {
  auto* S=Node<UK2Node_VariableSet>(Graph); S->VariableReference.SetSelfMember(Name); S->AllocateDefaultPins();
  Wire(Exec,S->GetExecPin()); Wire(Value,S->FindPin(Name)); return S->GetThenPin();
 }
 UEdGraphPin* Count(UEdGraph* Graph,UEdGraphPin* Exec,int32 Branch)
 {
  auto* C=Node<UK2Node_CallFunction>(Graph); C->SetFromFunction(UVaRestSocketGraphTestOwner::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSocketGraphTestOwner,Count))); C->AllocateDefaultPins();
  C->FindPinChecked(TEXT("Branch"))->DefaultValue=FString::FromInt(Branch); Wire(Exec,C->GetExecPin()); return C->GetThenPin();
 }
 void Declare(UEdGraph* Graph,UK2Node_VaRestSocketCall* N,EVaRestSocketCall Op,const TCHAR* Route)
 { N->Api=Api.Get(); N->Entry=TEXT("Fixture"); N->Operation=Op; N->Route=Route; N->bAssetEntry=true; N->bDeclaration=true; N->AllocateDefaultPins(); }
 void EditedLiteral(UEdGraph* Graph,UEdGraphPin* Pin,const TCHAR* Value)
 {
  if (!Pin) { Linked=false; Test.AddError(TEXT("missing declared input for pin editing")); return; }
  const auto Before=Pin->PinType;
  auto* Literal=Node<UK2Node_CallFunction>(Graph); Literal->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,MakeLiteralString))); Literal->AllocateDefaultPins();
  Literal->FindPinChecked(TEXT("Value"))->DefaultValue=TEXT("temporary wire"); Wire(Literal->GetReturnValuePin(),Pin);
  GetDefault<UEdGraphSchema_K2>()->BreakPinLinks(*Pin,true);
  const bool Retained=Pin->PinType==Before && Before.PinCategory==UEdGraphSchema_K2::PC_String && Before.bIsReference && Before.bIsConst;
  Linked=Retained && Linked; Test.TestTrue(TEXT("declared input keeps its concrete type and reference flags after Break Link(s)"),Retained);
  Pin->DefaultValue=Value;
 }
 bool Build()
 {
  Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(UVaRestSocketGraphTestOwner::StaticClass(),GetTransientPackage(),MakeUniqueObjectName(GetTransientPackage(),UBlueprint::StaticClass(),TEXT("VaRestSocketGraph")),BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
  if (!Blueprint) return false;
  auto* Graph=FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(),TEXT("SocketGraph"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass()); FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(),Graph);
  auto* Start=Node<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName=TEXT("Start"); Start->AllocateDefaultPins(); auto* Self=Node<UK2Node_Self>(Graph); Self->AllocateDefaultPins();
  auto* L=Node<UK2Node_VaRestSocketAsync>(Graph); L->Api=Api.Get(); L->Entry=TEXT("Fixture"); L->Operation=EVaRestSocketCall::ListenRequest; L->Route=TEXT("server"); L->bAssetEntry=true; L->bDeclaration=true; L->AllocateDefaultPins();
  auto* R=Node<UK2Node_VaRestSocketAsync>(Graph); R->Api=Api.Get(); R->Entry=TEXT("Fixture"); R->Operation=EVaRestSocketCall::Request; R->Route=TEXT("ask"); R->bAssetEntry=true; R->bDeclaration=true; R->AllocateDefaultPins(); EditedLiteral(Graph,R->FindPin(TEXT("Payload"),EGPD_Input),TEXT("literal ask"));
  for (auto* N:{L,R})
  {
   Wire(Self->FindPin(UEdGraphSchema_K2::PN_Self),N->FindPin(TEXT("WorldContextObject"),EGPD_Input));
   Count(Graph,Assign(Graph,N->FindPin(TEXT("OnError")),TEXT("Error"),N->FindPin(TEXT("Error"))),2);
   Count(Graph,Assign(Graph,N->FindPin(TEXT("OnTypedOutputUnavailable")),TEXT("Error"),N->FindPin(TEXT("Error"))),3);
  }
  Wire(Start->GetThenPin(),L->GetExecPin()); auto* Tail=Assign(Graph,L->GetThenPin(),TEXT("ListenAction"),L->FindPin(TEXT("AsyncTaskProxy"))); Wire(Tail,R->GetExecPin());
  Tail=Assign(Graph,R->GetThenPin(),TEXT("RequestAction"),R->FindPin(TEXT("AsyncTaskProxy")));
  auto* Emit=Node<UK2Node_VaRestSocketCall>(Graph); Declare(Graph,Emit,EVaRestSocketCall::Emit,TEXT("out")); EditedLiteral(Graph,Emit->FindPin(TEXT("Value")),TEXT("literal emit"));
  Wire(Self->FindPin(UEdGraphSchema_K2::PN_Self),Emit->FindPin(TEXT("WorldContextObject"))); Wire(Tail,Emit->GetExecPin()); Assign(Graph,Emit->GetThenPin(),TEXT("EmitResult"),Emit->GetReturnValuePin());
  Tail=Assign(Graph,L->FindPin(TEXT("OnMessage")),TEXT("Received"),L->FindPin(TEXT("Payload"),EGPD_Output)); Tail=Assign(Graph,Tail,TEXT("Message"),L->FindPin(TEXT("Message"))); Tail=Assign(Graph,Tail,TEXT("Warnings"),L->FindPin(TEXT("DecodeWarnings"))); Tail=Count(Graph,Tail,0);
  for (const FName Name:{FName(TEXT("FirstReply")),FName(TEXT("SecondReply"))})
  {
   auto* Reply=Node<UK2Node_VaRestSocketCall>(Graph); Declare(Graph,Reply,EVaRestSocketCall::Reply,TEXT("server")); EditedLiteral(Graph,Reply->FindPin(TEXT("Value")),TEXT("literal reply"));
   Wire(L->FindPin(TEXT("Message")),Reply->FindPin(TEXT("Message"))); Wire(Tail,Reply->GetExecPin()); Tail=Assign(Graph,Reply->GetThenPin(),Name,Reply->GetReturnValuePin());
  }
  Tail=Assign(Graph,R->FindPin(TEXT("OnReply")),TEXT("Reply"),R->FindPin(TEXT("Reply"),EGPD_Output)); Tail=Assign(Graph,Tail,TEXT("Warnings"),R->FindPin(TEXT("DecodeWarnings"))); Count(Graph,Tail,1);
  if (!Test.TestTrue(TEXT("all actual declared WebSocket K2 links"),Linked)) return false;
  FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get()); FCompilerResultsLog Log; FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
  if (!Test.TestTrue(TEXT("declared WebSocket graph compiles"),Log.NumErrors==0 && Blueprint->Status!=BS_Error) || !Test.TestEqual(TEXT("graph compiler warnings"),Log.NumWarnings,0)) return false;
  auto* Class=Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass); if (!Test.TestTrue(TEXT("actual socket ubergraph bytecode"),Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
  Owner.Reset(NewObject<UVaRestSocketGraphTestOwner>(World.Get(),Class)); BuiltFrame=GFrameCounter;
  return Test.TestNotNull(TEXT("actual persistent socket graph frame"),Class->GetPersistentUberGraphFrame(Owner.Get(),Class->UberGraphFunction));
 }
 bool Begin()
 {
  const FString Name=TEXT("VaRestSocketGraph_")+FGuid::NewGuid().ToString(EGuidFormats::Digits); Package.Reset(NewObject<UPackage>(nullptr,*(TEXT("/Temp/")+Name),RF_Transient)); Instance.Reset(NewObject<UGameInstance>(GEngine)); Instance->InitializeStandalone(*Name,Package.Get()); World.Reset(Instance->GetWorld());
  Hub.Reset(UVaRestSocketHub::Get(World.Get())); if (!Test.TestTrue(TEXT("real GI socket Hub configured before identity"),Hub && Hub->ConfigureTestingOnGT(Fake,Clock,887))) return false;
  Fake->OnConnect=[](FVaRestFakeSocketAttempt& A) { A.Open(); }; Api.Reset(NewObject<UVaRestApi>());
  FVaRestApiEnvironment E; E.Name=TEXT("Fixture"); E.BaseUrl=TEXT("https://socket-graph.invalid"); E.SocketBaseUrl=TEXT("ws://socket-graph.invalid"); Api->Environments.Add(E); Api->DefaultEnvironment=E.Name; Api->LogLevel=EVaRestLogLevel::None;
  auto* Config=NewObject<UVaRestSocketEndpoint>(Api.Get()); Config->Name=TEXT("Fixture"); Config->Path=TEXT("/socket"); Config->bNoAuth=true; Config->Routing=FVaRestSocketRouting::ByField(TEXT("type")).DataAt(TEXT("data")); Config->Routing.IdField=TEXT("id"); Config->Routing.ReplyRoute=TEXT("reply"); Config->ReadyRoute=TEXT("welcome");
  for (const auto Direction:{EVaRestSocketMessageDirection::Out,EVaRestSocketMessageDirection::Request,EVaRestSocketMessageDirection::InRequest})
  { FVaRestSocketMessageDeclaration D; D.Direction=Direction; D.Route=Direction==EVaRestSocketMessageDirection::Out ? TEXT("out") : Direction==EVaRestSocketMessageDirection::Request ? TEXT("ask") : TEXT("server"); D.PayloadType.Kind=EVaRestTypeKind::String; if (Direction!=EVaRestSocketMessageDirection::Out) D.ReplyType.Kind=EVaRestTypeKind::String; Config->Messages.Add(MoveTemp(D)); }
  Api->Sockets.Add(Config); FVaRestError Error; View.Reset(UVaRestSocketLibrary::GetSocket(World.Get(),Api.Get(),TEXT("Fixture"),Error));
  if (!Test.TestNotNull(*Error.Message,View.Get())) return false; Socket=View->GetNativeSocket(); Socket.NoReconnect(); Test.TestTrue(TEXT("weak wrapper cache identity"),Hub->Wrap(Socket)==View.Get());
  if (!Build()) return false;
  Connect.Reset(UVaRestConnectSocketAction::ConnectSocket(World.Get(),View.Get())); Connect->OnReady.AddDynamic(Owner.Get(),&UVaRestSocketGraphTestOwner::Ready); Connect->OnFailed.AddDynamic(Owner.Get(),&UVaRestSocketGraphTestOwner::ConnectFailed); Connect->Activate(); Deadline=FPlatformTime::Seconds()+30; return true;
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  if (Listen) Listen->Stop(); if (Request) Request->Stop(); if (Connect) Connect->Stop(); Listen.Reset(); Request.Reset(); Connect.Reset();
  if (Owner) { Owner->ClearOnGT(); const TWeakObjectPtr<UVaRestSocketGraphTestOwner> Weak(Owner.Get()); Owner.Reset(); CollectGarbage(RF_NoFlags); Test.TestFalse(TEXT("socket Blueprint owner/frame retire on GT"),Weak.IsValid()); }
  Socket={}; View.Reset(); if (Api) { FBlueprintActionDatabase::Get().ClearAssetActions(Api.Get()); FVaRestJsonRules::UnregisterOwner(Api.Get()); } Blueprint.Reset(); Api.Reset();
  if (Instance) Instance->Shutdown(); if (World) { World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World.Get()); if (World->IsRooted()) World->RemoveFromRoot(); World->SetGameInstance(nullptr); }
  World.Reset(); Hub.Reset(); Instance.Reset(); Package.Reset(); Test.TestEqual(TEXT("owned socket clock timers retired"),Clock->NumPendingTimers(),0); if (Fake->NumAttempts()) Test.TestTrue(TEXT("owned fake connection released"),Fake->GetLast().WasReleased());
 }
 FAutomationTestBase& Test; TStrongObjectPtr<UPackage> Package; TStrongObjectPtr<UGameInstance> Instance; TStrongObjectPtr<UWorld> World;
 TStrongObjectPtr<UVaRestSocketHub> Hub; TStrongObjectPtr<UVaRestApi> Api; TStrongObjectPtr<UBlueprint> Blueprint; TStrongObjectPtr<UVaRestSocketGraphTestOwner> Owner;
 TStrongObjectPtr<UVaRestSocketClient> View; TStrongObjectPtr<UVaRestConnectSocketAction> Connect; TStrongObjectPtr<UVaRestListenSocketAction> Listen; TStrongObjectPtr<UVaRestSocketRequestAction> Request;
 FVaRestSocket Socket; TSharedRef<FVaRestFakeSocketTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeSocketTransport,ESPMode::ThreadSafe>(); TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
 double Deadline=0; uint64 BuiltFrame=0,StoppedFrame=0; int32 Phase=0; bool Begun=false,Cleaned=false,Linked=true;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketGraphTest,"VaRest.WebSocket.Blueprint.Compiled",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSocketGraphTest::RunTest(const FString& Parameters)
{ ADD_LATENT_AUTOMATION_COMMAND(VaRestSocketGraphTest::FRun(*this)); return true; }
#endif
