// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestSioActionTestTypes.h"
#include "VaRestSocketHub.h"
#include "VaRestFakeSocketTransport.h"
#include "VaRestClock.h"
#include "VaRestApi.h"
#include "VaRestSioEndpoint.h"
#include "VaRestSioLibrary.h"
#include "VaRestSioComponent.h"
#include "GameFramework/Actor.h"
#include "VaRestJsonRules.h"
#include "BlueprintActionDatabase.h"
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "K2Node_VaRestSioAsync.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "Engine/BlueprintGeneratedClass.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_Self.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/CompilerResultsLog.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet/KismetSystemLibrary.h"
#include "UObject/GarbageCollection.h"

namespace VaRestSioActionTest
{
FString Text(const FVaRestSocketFrame& Frame)
{ const auto Bytes=Frame.GetPayload(); return FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()),Bytes.Num()); }
struct FCase
{
 explicit FCase(FAutomationTestBase& InTest,bool InAsset=false):Test(InTest),Asset(InAsset) {}
 ~FCase() { Cleanup(); }
 FAutomationTestBase& Test;
 TStrongObjectPtr<UGameInstance> Instance;
 TStrongObjectPtr<UWorld> World;
 TStrongObjectPtr<UPackage> Package;
 TStrongObjectPtr<UVaRestApi> Api;
 TStrongObjectPtr<UVaRestSioClient> View;
 TStrongObjectPtr<UVaRestSioActionTestProbe> Probe;
 TStrongObjectPtr<UVaRestConnectSioAction> Connect;
 TStrongObjectPtr<UVaRestListenSioAction> Listen;
 TStrongObjectPtr<UVaRestEmitSioAckAction> Ack,Timeout,Cancel;
 FVaRestSioSocket Native;
 TSharedRef<FVaRestFakeSocketTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeSocketTransport,ESPMode::ThreadSafe>();
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
 int32 Phase=0;
 double Deadline=0;
 bool Cleaned=false,Asset=false;
 bool Begin(bool bConnect=true)
 {
  check(IsInGameThread());
  const FString Name=TEXT("VaRestSioAction_")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
  Package.Reset(NewObject<UPackage>(nullptr,*(TEXT("/Temp/")+Name),RF_Transient));
  Instance.Reset(NewObject<UGameInstance>(GEngine));
  Instance->InitializeStandalone(*Name,Package.Get()); World.Reset(Instance->GetWorld());
  if (!Test.TestNotNull(TEXT("owned live standalone world"),World.Get())) return false;
  auto* Hub=UVaRestSocketHub::Get(World.Get());
  if (!Test.TestNotNull(TEXT("real GI socket subsystem Hub"),Hub) ||
   !Test.TestTrue(TEXT("fake dependencies installed before socket publication"),Hub->ConfigureTestingOnGT(Fake,Clock,449))) return false;
  Fake->OnConnect=[](FVaRestFakeSocketAttempt& Attempt)
  { Attempt.Open(); Attempt.Text(TEXT("0{\"sid\":\"engine\",\"upgrades\":[],\"pingInterval\":60000,\"pingTimeout\":20000,\"maxPayload\":1000000}")); };
  if (Asset)
  {
   Api.Reset(NewObject<UVaRestApi>()); FVaRestApiEnvironment Env; Env.Name=TEXT("Fixture"); Env.BaseUrl=TEXT("https://sio-action.invalid"); Env.SocketBaseUrl=TEXT("ws://sio-action.invalid"); Api->Environments.Add(Env); Api->DefaultEnvironment=Env.Name; Api->LogLevel=EVaRestLogLevel::None;
   auto* Endpoint=NewObject<UVaRestSioEndpoint>(Api.Get()); Endpoint->Name=TEXT("Fixture"); Endpoint->DefaultNamespace=TEXT("/room"); Endpoint->bNoAuth=true;
   auto& Namespace=Endpoint->Namespaces.AddDefaulted_GetRef(); Namespace.Name=Endpoint->DefaultNamespace; Namespace.bSendSession=false;
   for (auto Direction:{EVaRestSioEventDirection::Out,EVaRestSioEventDirection::InWithAck,EVaRestSioEventDirection::OutWithAck})
   {
    auto& Event=Namespace.Events.AddDefaulted_GetRef(); Event.Direction=Direction; Event.Event=Direction==EVaRestSioEventDirection::Out ? TEXT("declaredEdit") : Direction==EVaRestSioEventDirection::InWithAck ? TEXT("graphNotice") : TEXT("graphAsk");
    Event.PayloadType.Kind=Direction==EVaRestSioEventDirection::InWithAck ? EVaRestTypeKind::Int : EVaRestTypeKind::String;
    if (Direction==EVaRestSioEventDirection::OutWithAck) { Event.ReplyType.Kind=EVaRestTypeKind::Int; Event.bErrorFirstReply=true; }
   }
   Api->Sockets.Add(Endpoint); FVaRestError Error; View.Reset(UVaRestSioLibrary::GetSocketIO(World.Get(),Api.Get(),TEXT("Fixture"),Error));
   if (!Test.TestNotNull(*Error.Message,View.Get())) return false;
   Native=View->GetNativeSioSocket().NoReconnect();
   Test.TestTrue(TEXT("asset socket getter reuses the actual weak-cached wrapper"),UVaRestSioLibrary::GetSocketIO(World.Get(),Api.Get(),TEXT("Fixture"),Error)==View.Get());
  }
  else
  { Native=FVaRestSio::Make(Hub,TEXT("ActionFixture"),TEXT("ws://sio-action.invalid/room")).NoReconnect(); View.Reset(UVaRestSioClient::WrapOnGT(Hub,Native)); }
  Probe.Reset(NewObject<UVaRestSioActionTestProbe>());
  if (!Test.TestNotNull(TEXT("real UObject socket facade"),View.Get())) return false;
  if (bConnect)
  {
   Connect.Reset(UVaRestConnectSioAction::ConnectSocketIO(World.Get(),View.Get()));
   Connect->OnConnect.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Connected);
   Connect->OnFailed.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::ConnectFailed);
   Connect->Activate();
  }
  Deadline=FPlatformTime::Seconds()+30; return true;
 }
 FString EventWire(const TCHAR* Event) const
 {
  if (!Fake->NumAttempts()) return {};
  const FString Needle=TEXT("[\"")+FString(Event)+TEXT("\"");
  for (const auto& Frame:Fake->GetLast().GetSent())
  { const FString Wire=Text(Frame); if (Wire.StartsWith(TEXT("42/room,")) && Wire.Contains(Needle)) return Wire; }
  return {};
 }
 FString EventId(const TCHAR* Event) const
 { const auto Wire=EventWire(Event); const int32 Array=Wire.Find(TEXT("[")); return Array>8 ? Wire.Mid(8,Array-8) : FString(); }
 UVaRestEmitSioAckAction* MakeAck(const TCHAR* Event,double Seconds,bool ErrorFirst=false)
 {
  auto* Function=UVaRestEmitSioAckAction::StaticClass()->FindFunctionByName(TEXT("EmitWithAck"));
  if (!Test.TestNotNull(TEXT("actual reflected custom-thunk factory"),Function)) return nullptr;
  FStructOnScope Params(Function); void* Memory=Params.GetStructMemory();
  const auto Object=[&](const TCHAR* Name,UObject* Value)
  { auto* Property=FindFProperty<FObjectPropertyBase>(Function,Name); check(Property); Property->SetObjectPropertyValue_InContainer(Memory,Value); };
  const auto String=[&](const TCHAR* Name,const FString& Value)
  { auto* Property=FindFProperty<FStrProperty>(Function,Name); check(Property); Property->SetPropertyValue_InContainer(Memory,Value); };
  Object(TEXT("WorldContextObject"),World.Get()); Object(TEXT("Socket"),View.Get()); String(TEXT("Event"),Event); String(TEXT("Namespace"),TEXT(""));
  auto* Payload=FindFProperty<FIntProperty>(Function,TEXT("Payload")); check(Payload && Payload->HasAnyPropertyFlags(CPF_OutParm));
  Payload->SetPropertyValue_InContainer(Memory,42);
  auto* TimeoutProperty=FindFProperty<FDoubleProperty>(Function,TEXT("Timeout")); check(TimeoutProperty);
  auto* ErrorFirstProperty=FindFProperty<FBoolProperty>(Function,TEXT("bErrorFirst")); check(ErrorFirstProperty);
  TimeoutProperty->SetPropertyValue_InContainer(Memory,Seconds);
  ErrorFirstProperty->SetPropertyValue_InContainer(Memory,ErrorFirst);
  const EVaRestSioArgs First=EVaRestSioArgs::First;
  for (const TCHAR* Name:{TEXT("ArgsMode"),TEXT("ReplyArgsMode")})
  { auto* Property=Function->FindPropertyByName(Name); check(Property && Property->GetSize()==sizeof(First)); Property->CopyCompleteValue(Property->ContainerPtrToValuePtr<void>(Memory),&First); }
  GetMutableDefault<UVaRestEmitSioAckAction>()->ProcessEvent(Function,Memory);
  const auto* Return=FindFProperty<FObjectPropertyBase>(Function,TEXT("ReturnValue")); check(Return);
  const TStrongObjectPtr<UVaRestEmitSioAckAction> Result(Cast<UVaRestEmitSioAckAction>(Return->GetObjectPropertyValue_InContainer(Memory)));
  Payload->SetPropertyValue_InContainer(Memory,999); // the prepared packet must no longer borrow this storage
  return Result.Get();
 }
 void Bind(UVaRestEmitSioAckAction* Action)
 {
  Action->OnAck.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Acked);
  Action->OnError.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Failed);
 }
 bool Update()
 {
  if (FPlatformTime::Seconds()>Deadline)
  {
   Test.AddError(FString::Printf(TEXT("SIO action phase=%d timed out: connect=%d failed=%d event=%d ack=%d error=%d pending=%d last=%s"),Phase,Probe->Connects,Probe->ConnectFailures,Probe->Events,Probe->Acks,Probe->Errors,Native.GetStats().AcksPending,*Probe->LastError.Message));
   Cleanup(); return true;
  }
  if (Probe->ConnectFailures) { Test.AddError(Probe->LastError.Message); Cleanup(); return true; }
  if (Phase==0)
  {
   if (!Fake->NumAttempts()) return false;
   bool Sent=false; for (const auto& Frame:Fake->GetLast().GetSent()) Sent|=Text(Frame).StartsWith(TEXT("40/room,"));
   if (!Sent) return false;
   Test.TestFalse(TEXT("connect action remains live before namespace reply"),Connect->IsFinished());
   Fake->GetLast().Text(TEXT("40/room,{\"sid\":\"bp-room\"}")); ++Phase;
  }
  else if (Phase==1)
  {
   if (!Probe->Connects) return false;
   Test.TestEqual(TEXT("dynamic connect callback delivered once"),Probe->Connects,1);
   Listen.Reset(UVaRestListenSioAction::Listen(World.Get(),View.Get(),TEXT("notice"),TEXT(""),true,EVaRestSioArgs::First));
   Listen->OnEvent.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Event); Listen->OnError.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Failed);
   Listen->Activate(); Listen->Activate();
   Ack.Reset(MakeAck(TEXT("bpAsk"),1,true)); if (!Test.TestNotNull(TEXT("custom thunk creates prepared ACK action"),Ack.Get())) { Cleanup(); return true; }
   Bind(Ack.Get()); Test.TestTrue(TEXT("factory does not send before activation"),EventWire(TEXT("bpAsk")).IsEmpty());
   Ack->Activate(); Ack->Activate();
   Fake->GetLast().Text(TEXT("42/room,[\"notice\",7]")); Fake->GetLast().Text(TEXT("42/room,[\"notice\",8]")); ++Phase;
  }
  else if (Phase==2)
  {
   const FString Id=EventId(TEXT("bpAsk")); if (Id.IsEmpty() || !Probe->Events) return false;
   Test.TestEqual(TEXT("once action removes its listener before next event"),Probe->Events,1);
   Test.TestTrue(TEXT("once listen action finishes"),Listen->IsFinished()); Test.TestEqual(TEXT("listen scalar payload"),Probe->LastPayload.AsInt(),7);
   Test.TestTrue(TEXT("prepared thunk input copied before caller mutation"),EventWire(TEXT("bpAsk")).EndsWith(TEXT("[\"bpAsk\",42]")));
   Fake->GetLast().Text(TEXT("43/room,")+Id+TEXT("[null,99]")); ++Phase;
  }
  else if (Phase==3)
  {
   if (!Probe->Acks) return false;
   Test.TestEqual(TEXT("dynamic ACK callback once"),Probe->Acks,1); Test.TestTrue(TEXT("ACK action finishes before callback returns"),Ack->IsFinished());
   Test.TestEqual(TEXT("error-first null is stripped from result"),Probe->LastPayload.AsInt(),99); Test.TestEqual(TEXT("error-first argument array"),Probe->LastArgs.Num(),1);
   Timeout.Reset(MakeAck(TEXT("bpTimeout"),.1)); if (!Timeout) { Test.AddError(TEXT("timeout factory refused")); Cleanup(); return true; }
   Bind(Timeout.Get()); Timeout->Activate(); ++Phase;
  }
  else if (Phase==4)
  { if (EventId(TEXT("bpTimeout")).IsEmpty()) return false; Clock->Advance(.2); ++Phase; }
  else if (Phase==5)
  {
   if (!Probe->Errors) return false;
   Test.TestTrue(TEXT("timeout action delivers its primary error and finishes"),Probe->Errors==1 && Probe->LastError.Kind==EVaRestErrorKind::Timeout && Timeout->IsFinished());
   Cancel.Reset(MakeAck(TEXT("bpCancel"),0)); if (!Cancel) { Test.AddError(TEXT("cancel factory refused")); Cleanup(); return true; }
   Bind(Cancel.Get()); Cancel->Activate(); ++Phase;
  }
  else if (Phase==6)
  { if (EventId(TEXT("bpCancel")).IsEmpty()) return false; Cancel->Stop(); ++Phase; }
  else
  {
   if (Probe->Errors<2 || Native.GetStats().AcksPending) return false;
   Test.TestTrue(TEXT("explicit Stop cancels once and finishes"),Probe->Errors==2 && Probe->LastError.Kind==EVaRestErrorKind::Canceled && Cancel->IsFinished());
   Test.TestEqual(TEXT("no duplicate ACK success after cancellation"),Probe->Acks,1);
   Test.TestFalse(TEXT("dynamic callbacks stay on GT"),Probe->WrongThread.load(std::memory_order_acquire));
   Connect->Stop(); Test.TestTrue(TEXT("connect stream explicitly retires"),Connect->IsFinished());
   Cleanup(); return true;
  }
  return false;
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  if (Listen) Listen->Stop(); if (Ack) Ack->Stop(); if (Timeout) Timeout->Stop(); if (Cancel) Cancel->Stop(); if (Connect) Connect->Stop();
  Listen.Reset(); Ack.Reset(); Timeout.Reset(); Cancel.Reset(); Connect.Reset();
  if (Probe) Probe->ClearOnGT();
  Native={}; View.Reset();
  if (Api) { FBlueprintActionDatabase::Get().ClearAssetActions(Api.Get()); FVaRestJsonRules::UnregisterOwner(Api.Get()); } Api.Reset();
  if (Instance) Instance->Shutdown();
  if (World)
  {
   World->DestroyWorld(false); if (GEngine) GEngine->DestroyWorldContext(World.Get());
   if (World->IsRooted()) World->RemoveFromRoot(); World->SetGameInstance(nullptr);
  }
  World.Reset(); Instance.Reset(); Package.Reset(); Probe.Reset();
  Test.TestEqual(TEXT("owned socket timers retired before clock destruction"),Clock->NumPendingTimers(),0);
  if (Fake->NumAttempts()) Test.TestTrue(TEXT("GI teardown releases the fake connection"),Fake->GetLast().WasReleased());
 }
};
struct FComponentCase : FCase
{
 explicit FComponentCase(FAutomationTestBase& InTest):FCase(InTest,true) {}
 ~FComponentCase() { RetireComponents(); }
 TArray<TStrongObjectPtr<AActor>> Actors;
 TArray<TStrongObjectPtr<UVaRestSocketComponent>> Components;
 TStrongObjectPtr<UVaRestSioClient> SharedView,ActorViewA,ActorViewB;
 UVaRestSocketComponent* Add(bool bSio=true)
 {
  auto* Actor=World->SpawnActor<AActor>(); if (!Actor) return nullptr; Actors.Emplace(Actor);
  UVaRestSocketComponent* Component=bSio ? NewObject<UVaRestSioComponent>(Actor) : NewObject<UVaRestSocketComponent>(Actor);
  Components.Emplace(Component); Actor->AddInstanceComponent(Component); Component->RegisterComponent();
  Component->Url=TEXT("ws://sio-component.invalid/room");
  Component->OnError.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Failed);
  if (auto* Sio=Cast<UVaRestSioComponent>(Component))
  { Sio->Namespace=TEXT("/room"); Sio->OnConnect.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Connected); }
  return Component;
 }
 UVaRestSioComponent* Sio(int32 Index) const { return CastChecked<UVaRestSioComponent>(Components[Index].Get()); }
 void StartLast()
 {
  Test.TestTrue(TEXT("actor is initialized before BeginPlay/EndPlay"),Actors.Last()->IsActorInitialized());
  Actors.Last()->DispatchBeginPlay();
  Test.TestTrue(TEXT("actual actor BeginPlay routed to registered component"),Components.Last()->HasBegunPlay());
 }
 bool Sent(int32 Index,const TCHAR* Prefix) const
 {
  if (Fake->NumAttempts()<=Index) return false;
  for (const auto& Frame:Fake->GetAttempt(Index).GetSent()) if (Text(Frame).StartsWith(Prefix)) return true;
  return false;
 }
 bool Begin()
 {
  if (!FCase::Begin(false)) return false;
  // InitializeStandalone creates a dummy world; RouteEndPlay requires initialized actors.
  World->InitializeActorsForPlay(FURL());
  for (bool bSio:{true,false})
  {
   auto* Component=Add(bSio); if (!Component) return false;
   Component->Scope=static_cast<EVaRestSocketScope>(255); StartLast();
   Test.TestNull(TEXT("invalid URL scope refuses the client"),Component->GetClient());
   Test.TestEqual(TEXT("invalid URL scope reports Config"),Probe->LastError.Kind,EVaRestErrorKind::Config);
  }
  auto* MissingName=Add(); if (!MissingName) return false;
  MissingName->Scope=EVaRestSocketScope::GameInstance; StartLast();
  Test.TestNull(TEXT("shared URL scope requires a name"),MissingName->GetClient());
  auto* InvalidNamespace=CastChecked<UVaRestSioComponent>(Add());
  InvalidNamespace->Api=Api.Get(); InvalidNamespace->Entry=TEXT("Fixture"); InvalidNamespace->Namespaces={TEXT("/valid"),TEXT("invalid")}; StartLast();
  Test.TestNull(TEXT("invalid extra namespace refuses asset component"),InvalidNamespace->GetSioClient());
  Test.TestEqual(TEXT("all malformed configurations report one error"),Probe->Errors,4);
  Test.TestEqual(TEXT("configuration refusal starts no connection"),Fake->NumAttempts(),0);
  for (auto& Actor:Actors) Actor->Destroy();
  for (int32 Index=0;Index<2;++Index)
  {
   auto* Component=CastChecked<UVaRestSioComponent>(Add()); Component->Api=Api.Get(); Component->Entry=TEXT("Fixture");
   if (Index==0) Component->Namespaces={TEXT("/extra")}; StartLast();
   Test.TestTrue(TEXT("component reuses the asset getter socket"),Component->GetSioClient()==View.Get());
  }
  return true;
 }
 bool Update()
 {
  if (FPlatformTime::Seconds()>Deadline)
  { Test.AddError(FString::Printf(TEXT("component phase=%d connects=%d errors=%d attempts=%d"),Phase,Probe->Connects,Probe->Errors,Fake->NumAttempts())); Cleanup(); return true; }
  if (Phase==0)
  {
   if (!Sent(0,TEXT("40/room,")) || !Sent(0,TEXT("40/extra,"))) return false;
   Test.TestEqual(TEXT("two asset components make one physical connection"),Fake->NumAttempts(),1);
   Fake->GetAttempt(0).Text(TEXT("40/room,{\"sid\":\"component-room\"}"));
   Fake->GetAttempt(0).Text(TEXT("40/extra,{\"sid\":\"component-extra\"}")); ++Phase;
  }
  else if (Phase==1)
  {
   if (Probe->Connects<4) return false;
   Test.TestEqual(TEXT("both asset components forward both namespace connects"),Probe->Connects,4);
   Actors[4]->RouteEndPlay(EEndPlayReason::RemovedFromWorld);
   Test.TestTrue(TEXT("level-unloaded actor and component stay valid for the late-event check"),IsValid(Actors[4].Get()) && IsValid(Components[4].Get()));
   Test.TestFalse(TEXT("level unload actually routes component EndPlay"),Components[4]->HasBegunPlay());
   Test.TestNull(TEXT("ended component clears its protocol client"),Sio(4)->GetSioClient());
   Test.TestNull(TEXT("ended component clears its base client"),Components[4]->GetClient());
   Test.TestTrue(TEXT("shared asset socket survives one owner ending"),View->IsNamespaceConnected(TEXT("/room")) && !Fake->GetAttempt(0).WasCloseRequested());
   View->ConnectNamespace(TEXT("/late"),{}); ++Phase;
  }
  else if (Phase==2)
  { if (!Sent(0,TEXT("40/late,"))) return false; Fake->GetAttempt(0).Text(TEXT("40/late,{\"sid\":\"component-late\"}")); ++Phase; }
  else if (Phase==3)
  {
   if (Probe->Connects<5) return false;
   Test.TestEqual(TEXT("ended component no longer forwards shared socket events"),Probe->Connects,5);
   Actors[5]->Destroy();
   Test.TestTrue(TEXT("asset socket also survives the last component ending"),View->IsNamespaceConnected(TEXT("/room")) && !Fake->GetAttempt(0).WasCloseRequested());
   View->Close(1000,TEXT("asset fixture done")); ++Phase;
  }
  else if (Phase==4)
  {
   if (!Fake->GetAttempt(0).WasReleased()) return false; Probe->Connects=0;
   for (int32 Index=0;Index<2;++Index)
   {
    auto* Component=CastChecked<UVaRestSioComponent>(Add()); Component->Scope=EVaRestSocketScope::GameInstance; Component->SocketName=TEXT("ComponentGI"); StartLast();
    if (!Test.TestNotNull(TEXT("named component URL client"),Component->GetSioClient())) { Cleanup(); return true; }
    Component->GetSioClient()->GetNativeSioSocket().NoReconnect();
    if (Index==0) SharedView.Reset(Component->GetSioClient());
    else Test.TestTrue(TEXT("same GI name reuses one protocol wrapper"),SharedView.Get()==Component->GetSioClient());
   }
   ++Phase;
  }
  else if (Phase==5)
  { if (!Sent(1,TEXT("40/room,"))) return false; Test.TestEqual(TEXT("named GI components make one new connection"),Fake->NumAttempts(),2); Fake->GetAttempt(1).Text(TEXT("40/room,{\"sid\":\"component-gi\"}")); ++Phase; }
  else if (Phase==6)
  {
   if (Probe->Connects<2) return false; Test.TestEqual(TEXT("both named components forward connect"),Probe->Connects,2);
   Actors[6]->Destroy(); Actors[7]->Destroy();
   Test.TestTrue(TEXT("named URL socket survives all component owners"),SharedView->IsNamespaceConnected(TEXT("/room")) && !Fake->GetAttempt(1).WasCloseRequested());
   SharedView->Close(1000,TEXT("named fixture done")); ++Phase;
  }
  else if (Phase==7)
  {
   if (!Fake->GetAttempt(1).WasReleased()) return false; SharedView.Reset(); Probe->Connects=0;
   for (int32 Index=0;Index<2;++Index)
   {
    auto* Component=CastChecked<UVaRestSioComponent>(Add()); StartLast();
    if (!Test.TestNotNull(TEXT("actor URL client"),Component->GetSioClient())) { Cleanup(); return true; }
    Component->GetSioClient()->GetNativeSioSocket().NoReconnect();
    (Index==0 ? ActorViewA : ActorViewB).Reset(Component->GetSioClient());
   }
   Test.TestTrue(TEXT("actor URL clients are distinct"),ActorViewA.Get()!=ActorViewB.Get()); ++Phase;
  }
  else if (Phase==8)
  {
   if (!Sent(2,TEXT("40/room,")) || !Sent(3,TEXT("40/room,"))) return false;
   Test.TestEqual(TEXT("actor URL components own separate physical connections"),Fake->NumAttempts(),4);
   Fake->GetAttempt(2).Text(TEXT("40/room,{\"sid\":\"actor-a\"}")); Fake->GetAttempt(3).Text(TEXT("40/room,{\"sid\":\"actor-b\"}")); ++Phase;
  }
  else if (Phase==9)
  { if (Probe->Connects<2) return false; Test.TestEqual(TEXT("actor components each forward connect"),Probe->Connects,2); Actors[8]->Destroy(); ++Phase; }
  else if (Phase==10)
  {
   if (!Fake->GetAttempt(2).WasCloseRequested()) return false;
   Test.TestEqual(TEXT("actor EndPlay closes its socket with going-away"),Fake->GetAttempt(2).GetCloseCode(),1001);
   Test.TestTrue(TEXT("another actor socket remains connected"),ActorViewB->IsNamespaceConnected(TEXT("/room")) && !Fake->GetAttempt(3).WasCloseRequested());
   Test.TestNull(TEXT("actor EndPlay clears its client"),Sio(8)->GetSioClient()); Actors[9]->Destroy(); ++Phase;
  }
  else
  {
   if (!Fake->GetAttempt(2).WasReleased() || !Fake->GetAttempt(3).WasReleased()) return false;
   Test.TestEqual(TEXT("second actor EndPlay closes its own socket"),Fake->GetAttempt(3).GetCloseCode(),1001);
   Test.TestEqual(TEXT("no additional component configuration errors"),Probe->Errors,4);
   Test.TestFalse(TEXT("component dynamic callbacks stay on GT"),Probe->WrongThread.load(std::memory_order_acquire));
   Cleanup(); return true;
  }
  return false;
 }
 void RetireComponents()
 {
  for (auto& Actor:Actors) if (IsValid(Actor.Get())) Actor->Destroy();
  ActorViewA.Reset(); ActorViewB.Reset(); SharedView.Reset(); Components.Reset(); Actors.Reset();
 }
 void Cleanup() { RetireComponents(); FCase::Cleanup(); }
};
class FComponentAdvance final : public IAutomationLatentCommand
{
public: explicit FComponentAdvance(TSharedRef<FComponentCase> In):State(MoveTemp(In)) {} bool Update() override { return State->Update(); }
private: TSharedRef<FComponentCase> State;
};
template <typename T> T* Node(UEdGraph* Graph)
{ auto* Result=NewObject<T>(Graph); Graph->AddNode(Result,false,false); Result->CreateNewGuid(); return Result; }
bool Link(UEdGraphPin* A,UEdGraphPin* B)
{ return A && B && GetDefault<UEdGraphSchema_K2>()->TryCreateConnection(A,B); }
UK2Node_VariableGet* Get(UEdGraph* Graph,FName Name)
{ auto* Result=Node<UK2Node_VariableGet>(Graph); Result->VariableReference.SetSelfMember(Name); Result->AllocateDefaultPins(); return Result; }
UK2Node_VariableSet* Set(UEdGraph* Graph,FName Name)
{ auto* Result=Node<UK2Node_VariableSet>(Graph); Result->VariableReference.SetSelfMember(Name); Result->AllocateDefaultPins(); return Result; }
struct FGraphCase : FCase
{
 explicit FGraphCase(FAutomationTestBase& InTest,bool InLiteral=false,bool InAsset=false):FCase(InTest,InAsset),Literal(InLiteral) {}
 ~FGraphCase() { RetireOwner(); }
 TStrongObjectPtr<UBlueprint> Blueprint;
 TStrongObjectPtr<UVaRestSioGraphTestOwner> Owner;
 bool Linked=true,Literal=false;
 void Wire(UEdGraphPin* A,UEdGraphPin* B)
 {
  const bool Valid=Link(A,B); Linked=Valid && Linked;
  if (!Valid) Test.AddError(FString::Printf(TEXT("K2 fixture link failed: %s.%s -> %s.%s"),
   A ? *A->GetOwningNode()->GetClass()->GetName() : TEXT("missing"),A ? *A->PinName.ToString() : TEXT("pin"),
   B ? *B->GetOwningNode()->GetClass()->GetName() : TEXT("missing"),B ? *B->PinName.ToString() : TEXT("pin")));
 }
 UEdGraphPin* Assign(UEdGraph* Graph,UEdGraphPin* Exec,FName Name,UEdGraphPin* Value)
 {
  auto* Setter=Set(Graph,Name); Wire(Exec,Setter->GetExecPin()); Wire(Value,Setter->FindPin(Name)); return Setter->GetThenPin();
 }
 UEdGraphPin* Count(UEdGraph* Graph,UEdGraphPin* Exec,int32 Branch)
 {
  auto* Call=Node<UK2Node_CallFunction>(Graph);
  Call->SetFromFunction(UVaRestSioGraphTestOwner::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UVaRestSioGraphTestOwner,Count)));
  Call->AllocateDefaultPins(); Call->FindPinChecked(TEXT("Branch"))->DefaultValue=FString::FromInt(Branch); Wire(Exec,Call->GetExecPin()); return Call->GetThenPin();
 }
 bool Build()
 {
  const FName Name=MakeUniqueObjectName(GetTransientPackage(),UBlueprint::StaticClass(),TEXT("VaRestSioCompiledGraph"));
  Blueprint.Reset(FKismetEditorUtilities::CreateBlueprint(UVaRestSioGraphTestOwner::StaticClass(),GetTransientPackage(),Name,
   BPTYPE_Normal,UBlueprint::StaticClass(),UBlueprintGeneratedClass::StaticClass()));
  if (!Test.TestNotNull(TEXT("actual Socket.IO test Blueprint"),Blueprint.Get())) return false;
  auto* Graph=FBlueprintEditorUtils::CreateNewGraph(Blueprint.Get(),TEXT("SioEventGraph"),UEdGraph::StaticClass(),UEdGraphSchema_K2::StaticClass());
  FBlueprintEditorUtils::AddUbergraphPage(Blueprint.Get(),Graph);
  auto* Start=Node<UK2Node_CustomEvent>(Graph); Start->CustomFunctionName=TEXT("Start"); Start->AllocateDefaultPins();
  auto* Self=Node<UK2Node_Self>(Graph); Self->AllocateDefaultPins();
  auto* Listener=Node<UK2Node_VaRestSioAsync>(Graph); Listener->Operation=Asset ? EVaRestSioCall::ListenWithAck : EVaRestSioCall::Listen; Listener->bOnce=true;
  if (Asset) { Listener->Api=Api.Get(); Listener->Entry=TEXT("Fixture"); Listener->Namespace=TEXT("/room"); Listener->Event=TEXT("graphNotice"); Listener->bAssetEntry=true; Listener->bDeclaration=true; }
  Listener->PayloadType.Kind=EVaRestTypeKind::Int; Listener->AllocateDefaultPins();
  Listener->FindPinChecked(TEXT("Event"),EGPD_Input)->DefaultValue=TEXT("graphNotice");
  auto* Request=Node<UK2Node_VaRestSioAsync>(Graph); Request->Operation=EVaRestSioCall::EmitWithAck;
  if (Asset) { Request->Api=Api.Get(); Request->Entry=TEXT("Fixture"); Request->Namespace=TEXT("/room"); Request->Event=TEXT("graphAsk"); Request->bAssetEntry=true; Request->bDeclaration=true; }
  Request->PayloadType.Kind=EVaRestTypeKind::String; Request->ReplyType.Kind=EVaRestTypeKind::Int; Request->AllocateDefaultPins();
  Request->FindPinChecked(TEXT("Event"),EGPD_Input)->DefaultValue=TEXT("graphAsk");
  Request->FindPinChecked(TEXT("bErrorFirst"))->DefaultValue=TEXT("true");
  for (auto* Async:{Listener,Request})
  {
   Wire(Self->FindPin(UEdGraphSchema_K2::PN_Self),Async->FindPin(TEXT("WorldContextObject"),EGPD_Input));
   if (!Asset) Wire(Get(Graph,TEXT("Socket"))->FindPin(TEXT("Socket")),Async->FindPin(TEXT("Socket"),EGPD_Input));
   Count(Graph,Assign(Graph,Async->FindPin(TEXT("OnError"),EGPD_Output),TEXT("LastFailure"),Async->FindPin(TEXT("Error"),EGPD_Output)),2);
   Count(Graph,Assign(Graph,Async->FindPin(TEXT("OnTypedOutputUnavailable"),EGPD_Output),TEXT("LastFailure"),Async->FindPin(TEXT("Error"),EGPD_Output)),3);
  }
  Wire(Start->GetThenPin(),Listener->GetExecPin());
  auto* Tail=Assign(Graph,Listener->GetThenPin(),TEXT("ListenAction"),Listener->FindPin(TEXT("AsyncTaskProxy"),EGPD_Output));
  Wire(Tail,Request->GetExecPin());
  Tail=Assign(Graph,Request->GetThenPin(),TEXT("AckAction"),Request->FindPin(TEXT("AsyncTaskProxy"),EGPD_Output));
  auto* Emit=Node<UK2Node_VaRestSioCall>(Graph); Emit->Operation=EVaRestSioCall::Emit; Emit->ArgumentCount=3; Emit->AllocateDefaultPins();
  Emit->FindPinChecked(TEXT("Event"))->DefaultValue=TEXT("graphEmit");
  Wire(Tail,Emit->GetExecPin()); Wire(Get(Graph,TEXT("Socket"))->FindPin(TEXT("Socket")),Emit->FindPin(UEdGraphSchema_K2::PN_Self));
  Wire(Get(Graph,TEXT("Input"))->FindPin(TEXT("Input")),Emit->FindPin(TEXT("Arg0")));
  Wire(Get(Graph,TEXT("InputNumber"))->FindPin(TEXT("InputNumber")),Emit->FindPin(TEXT("Arg1")));
  Wire(Get(Graph,TEXT("InputArray"))->FindPin(TEXT("InputArray")),Emit->FindPin(TEXT("Arg2")));
  Assign(Graph,Emit->GetThenPin(),TEXT("EmitResult"),Emit->GetReturnValuePin());
  if (Literal) Request->FindPinChecked(TEXT("Payload"),EGPD_Input)->DefaultValue=TEXT("literal snapshot");
  else Wire(Get(Graph,TEXT("Input"))->FindPin(TEXT("Input")),Request->FindPin(TEXT("Payload"),EGPD_Input));
  Tail=Assign(Graph,Listener->FindPin(TEXT("OnEvent"),EGPD_Output),TEXT("ListenValue"),Listener->FindPin(TEXT("Payload"),EGPD_Output));
  Tail=Assign(Graph,Tail,TEXT("ListenInfo"),Listener->FindPin(TEXT("Event"),EGPD_Output));
  Tail=Assign(Graph,Tail,TEXT("ListenWarnings"),Listener->FindPin(TEXT("DecodeWarnings"),EGPD_Output)); Tail=Count(Graph,Tail,0);
  for (const FName Field:{FName(TEXT("FirstReply")),FName(TEXT("SecondReply"))})
  {
   auto* Reply=Node<UK2Node_VaRestSioCall>(Graph); Reply->Operation=EVaRestSioCall::Reply; Reply->AllocateDefaultPins();
   Wire(Tail,Reply->GetExecPin()); Wire(Listener->FindPin(TEXT("Ack"),EGPD_Output),Reply->FindPin(TEXT("Ack")));
   Wire(Get(Graph,TEXT("ListenValue"))->FindPin(TEXT("ListenValue")),Reply->FindPin(TEXT("Payload")));
   Tail=Assign(Graph,Reply->GetThenPin(),Field,Reply->GetReturnValuePin());
  }
  Tail=Assign(Graph,Request->FindPin(TEXT("OnAck"),EGPD_Output),TEXT("AckValue"),Request->FindPin(TEXT("Reply"),EGPD_Output));
  Tail=Assign(Graph,Tail,TEXT("AckInfo"),Request->FindPin(TEXT("Event"),EGPD_Output));
  Tail=Assign(Graph,Tail,TEXT("AckArgs"),Request->FindPin(TEXT("Args"),EGPD_Output));
  Tail=Assign(Graph,Tail,TEXT("AckWarnings"),Request->FindPin(TEXT("DecodeWarnings"),EGPD_Output)); Count(Graph,Tail,1);
  // The additional event runs only in the asset fixture; generic fixtures still compile its edited declaration.
  TStrongObjectPtr<UVaRestApi> DeclaredApi(Asset ? Api.Get() : NewObject<UVaRestApi>());
  ON_SCOPE_EXIT { if (!Asset) { FBlueprintActionDatabase::Get().ClearAssetActions(DeclaredApi.Get()); FVaRestJsonRules::UnregisterOwner(DeclaredApi.Get()); } };
  if (!Asset)
  {
   FVaRestApiEnvironment Env; Env.Name=TEXT("Fixture"); Env.BaseUrl=TEXT("https://sio-pin-edit.invalid"); Env.SocketBaseUrl=TEXT("ws://sio-pin-edit.invalid"); DeclaredApi->Environments.Add(Env); DeclaredApi->DefaultEnvironment=Env.Name;
   auto* Endpoint=NewObject<UVaRestSioEndpoint>(DeclaredApi.Get()); Endpoint->Name=TEXT("Fixture"); Endpoint->bNoAuth=true;
   auto& Namespace=Endpoint->Namespaces.AddDefaulted_GetRef(); Namespace.bSendSession=false;
   auto& Event=Namespace.Events.AddDefaulted_GetRef(); Event.Event=TEXT("declaredEdit"); Event.Direction=EVaRestSioEventDirection::Out; Event.PayloadType.Kind=EVaRestTypeKind::String;
   DeclaredApi->Sockets.Add(Endpoint);
  }
  auto* Edited=Node<UK2Node_VaRestSioCall>(Graph); Edited->Api=DeclaredApi.Get(); Edited->Entry=TEXT("Fixture"); Edited->Namespace=Asset ? TEXT("/room") : TEXT("/"); Edited->Event=TEXT("declaredEdit"); Edited->Operation=EVaRestSioCall::Emit; Edited->bAssetEntry=true; Edited->bDeclaration=true; Edited->AllocateDefaultPins();
  auto* EditedStart=Node<UK2Node_CustomEvent>(Graph); EditedStart->CustomFunctionName=TEXT("DeclaredEditorPins"); EditedStart->AllocateDefaultPins();
  Wire(EditedStart->GetThenPin(),Edited->GetExecPin()); Wire(Self->FindPin(UEdGraphSchema_K2::PN_Self),Edited->FindPin(TEXT("WorldContextObject")));
  auto* Value=Edited->FindPinChecked(TEXT("Payload")); const auto Before=Value->PinType;
  auto* LiteralNode=Node<UK2Node_CallFunction>(Graph); LiteralNode->SetFromFunction(UKismetSystemLibrary::StaticClass()->FindFunctionByName(GET_FUNCTION_NAME_CHECKED(UKismetSystemLibrary,MakeLiteralString))); LiteralNode->AllocateDefaultPins();
  Wire(LiteralNode->GetReturnValuePin(),Value); GetDefault<UEdGraphSchema_K2>()->BreakPinLinks(*Value,true);
  if (!Test.TestTrue(TEXT("declared Socket.IO Emit retains type/reference flags after Break Link(s)"),Value->PinType==Before && Before.PinCategory==UEdGraphSchema_K2::PC_String && Before.bIsReference && Before.bIsConst)) return false;
  Value->DefaultValue=TEXT("declared literal");
  if (Asset) Assign(Graph,Edited->GetThenPin(),TEXT("FirstReply"),Edited->GetReturnValuePin());
  if (!Test.TestTrue(TEXT("actual Socket.IO K2 pin links are legal"),Linked)) return false;
  FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint.Get()); FCompilerResultsLog Log;
  FKismetEditorUtilities::CompileBlueprint(Blueprint.Get(),EBlueprintCompileOptions::SkipGarbageCollection,&Log);
  if (!Test.TestTrue(TEXT("actual Socket.IO K2 graph compiles without errors"),Log.NumErrors==0 && Blueprint->Status!=BS_Error)) return false;
  if (!Test.TestEqual(TEXT("new protocol graph has no compiler warnings"),Log.NumWarnings,0)) return false;
  auto* Class=Cast<UBlueprintGeneratedClass>(Blueprint->GeneratedClass);
  if (!Test.TestTrue(TEXT("real compiled ubergraph bytecode"),Class && Class->UberGraphFunction && !Class->UberGraphFunction->Script.IsEmpty())) return false;
  Owner.Reset(NewObject<UVaRestSioGraphTestOwner>(World.Get(),Class)); Owner->Socket=View.Get(); Owner->Input=TEXT("snapshot"); Owner->InputArray={5,6};
  if (!Test.TestNotNull(TEXT("real persistent ubergraph storage"),Class->GetPersistentUberGraphFrame(Owner.Get(),Class->UberGraphFunction))) return false;
  for (auto* Async:{Listener,Request}) for (const TCHAR* Prefix:{TEXT("__VaRest_Prepare_"),TEXT("__VaRest_Continue_"),TEXT("__VaRest_Unavailable_")})
  {
   const FName Callback(FString(Prefix)+Async->NodeGuid.ToString(EGuidFormats::Digits)); const auto* Function=Owner->FindFunction(Callback);
   if (!Test.TestTrue(TEXT("production expansion generated a real script callback"),Function && Function->GetOuter()==Class && !Function->Script.IsEmpty())) return false;
  }
  return true;
 }
 bool Begin() { if (!FCase::Begin() || !Build()) return false; Deadline=FPlatformTime::Seconds()+30; return true; }
 bool Update()
 {
  if (FPlatformTime::Seconds()>Deadline || Probe->ConnectFailures || (Owner && (Owner->Errors || Owner->Unavailable)))
  {
   Test.AddError(FString::Printf(TEXT("compiled SIO phase=%d connect=%d failed=%d listens=%d acks=%d errors=%d unavailable=%d last=%s"),Phase,Probe->Connects,Probe->ConnectFailures,
    Owner ? Owner->Listens : -1,Owner ? Owner->Acks : -1,Owner ? Owner->Errors : -1,Owner ? Owner->Unavailable : -1,Owner ? *Owner->LastFailure.Message : TEXT("no owner")));
   RetireOwner(); Cleanup(); return true;
  }
  if (Phase==0)
  {
   if (!Fake->NumAttempts()) return false;
   bool Sent=false; for (const auto& Frame:Fake->GetLast().GetSent()) Sent|=Text(Frame).StartsWith(TEXT("40/room,"));
   if (!Sent) return false; Fake->GetLast().Text(TEXT("40/room,{\"sid\":\"bp-room\"}")); ++Phase;
  }
  else if (Phase==1)
  {
   if (!Probe->Connects) return false;
   Owner->ProcessEvent(Owner->FindFunctionChecked(TEXT("Start")),nullptr);
   if (Asset)
   {
    Owner->ProcessEvent(Owner->FindFunctionChecked(TEXT("DeclaredEditorPins")),nullptr);
    Test.TestEqual(TEXT("asset-declared Emit uses the already ready socket"),Owner->FirstReply,EVaRestSendResult::Sent);
   }
   Listen.Reset(Owner->ListenAction); Ack.Reset(Owner->AckAction);
   if (!Test.TestTrue(TEXT("K2 creates both actual actions and activates them"),Listen && Ack && !Listen->IsFinished() && !Ack->IsFinished()))
   { RetireOwner(); Cleanup(); return true; }
   Listen->OnEvent.AddDynamic(Probe.Get(),&UVaRestSioActionTestProbe::Event); Bind(Ack.Get());
   Owner->Input=TEXT("changed"); Owner->InputNumber=0; Owner->InputArray.Reset();
   Fake->GetLast().Text(TEXT("42/room,9[\"graphNotice\",71]")); Fake->GetLast().Text(TEXT("42/room,10[\"graphNotice\",72]")); ++Phase;
  }
  else if (Phase==2)
  {
   const auto Id=EventId(TEXT("graphAsk")); if (Id.IsEmpty() || !Owner->Listens || EventWire(TEXT("graphEmit")).IsEmpty()) return false;
   if (Asset)
   {
    if (EventWire(TEXT("declaredEdit")).IsEmpty()) return false;
    Test.TestEqual(TEXT("declared edited literal reaches its asset namespace"),EventWire(TEXT("declaredEdit")),FString(TEXT("42/room,[\"declaredEdit\",\"declared literal\"]")));
    Test.TestEqual(TEXT("asset factories and explicit client share one physical connection"),Fake->NumAttempts(),1);
   }
   bool Replied=false; for (const auto& Frame:Fake->GetLast().GetSent()) Replied|=Text(Frame)==TEXT("43/room,9[71]");
   if (!Replied) return false;
   Test.TestEqual(TEXT("compiled variadic Emit accepted"),Owner->EmitResult,EVaRestSendResult::Sent);
   Test.TestEqual(TEXT("compiled heterogeneous arguments copied in order"),EventWire(TEXT("graphEmit")),FString(TEXT("42/room,[\"graphEmit\",\"snapshot\",73,[5,6]]")));
   Test.TestEqual(TEXT("compiled Ack Reply queues once during event dispatch"),Owner->FirstReply,EVaRestSendResult::Queued);
   Test.TestEqual(TEXT("compiled second Ack Reply refuses same token"),Owner->SecondReply,EVaRestSendResult::Rejected);
   Test.TestTrue(TEXT("typed listener exposes valid incoming ACK metadata"),Owner->ListenInfo.Ack.IsValid());
   Test.TestFalse(TEXT("shared ACK token was consumed by compiled reply"),Owner->ListenInfo.Ack.CanReplyOnGT());
   Test.TestEqual(TEXT("typed listen continuation exactly once"),Owner->Listens,1); Test.TestEqual(TEXT("typed listen value"),Owner->ListenValue,71);
   Test.TestEqual(TEXT("native listener metadata callback once"),Probe->Events,1); Test.TestTrue(TEXT("once action retired after typed continuation"),Listen->IsFinished());
   Test.TestTrue(TEXT("compiled typed FString payload preserves its literal or caller snapshot"),EventWire(TEXT("graphAsk")).EndsWith(
    Literal ? TEXT("[\"graphAsk\",\"literal snapshot\"]") : TEXT("[\"graphAsk\",\"snapshot\"]")));
   Test.TestEqual(TEXT("listener native metadata namespace"),Owner->ListenInfo.Namespace,FString(TEXT("/room")));
   Test.TestEqual(TEXT("listener native metadata payload"),Owner->ListenInfo.Args.At(0).AsInt(),71);
   Test.TestEqual(TEXT("listener decode warnings"),Owner->ListenWarnings.Num(),0);
   Fake->GetLast().Text(TEXT("43/room,")+Id+TEXT("[null,99]")); ++Phase;
  }
  else
  {
   if (!Owner->Acks) return false;
   Test.TestEqual(TEXT("typed ACK continuation exactly once"),Owner->Acks,1); Test.TestEqual(TEXT("typed ACK value"),Owner->AckValue,99);
   Test.TestEqual(TEXT("split native Args metadata"),Owner->AckArgs.At(0).AsInt(),99); Test.TestEqual(TEXT("split Args count"),Owner->AckArgs.Num(),1);
   Test.TestEqual(TEXT("ACK event metadata namespace"),Owner->AckInfo.Namespace,FString(TEXT("/room")));
   Test.TestEqual(TEXT("ACK decode warnings"),Owner->AckWarnings.Num(),0);
   Test.TestEqual(TEXT("native ACK metadata callback once"),Probe->Acks,1); Test.TestTrue(TEXT("ACK action finishes after typed delivery"),Ack->IsFinished());
   Test.TestFalse(TEXT("actual compiled continuations stay on GT"),Owner->WrongThread.load(std::memory_order_acquire));
   int32 ReplyPackets=0; for (const auto& Frame:Fake->GetLast().GetSent()) if (Text(Frame).StartsWith(TEXT("43/room,"))) ++ReplyPackets;
   Test.TestEqual(TEXT("exactly one server ACK packet across two calls and once listener"),ReplyPackets,1);
   Test.TestEqual(TEXT("no ACKs remain"),Native.GetStats().AcksPending,0);
   RetireOwner(); Cleanup(); return true;
  }
  return false;
 }
 void RetireOwner()
 {
  if (!Owner) return;
  if (Listen) Listen->Stop(); if (Ack) Ack->Stop();
  Owner->ClearOnGT(); const TWeakObjectPtr<UVaRestSioGraphTestOwner> Weak(Owner.Get()); Owner.Reset();
  CollectGarbage(RF_NoFlags); Test.TestFalse(TEXT("compiled Blueprint owner and persistent frame retire on GT"),Weak.IsValid());
 }
};
class FGraphAdvance final : public IAutomationLatentCommand
{
public: explicit FGraphAdvance(TSharedRef<FGraphCase> In):State(MoveTemp(In)) {} bool Update() override { return State->Update(); }
private: TSharedRef<FGraphCase> State;
};
class FAdvance final : public IAutomationLatentCommand
{
public: explicit FAdvance(TSharedRef<FCase> In):State(MoveTemp(In)) {} bool Update() override { return State->Update(); }
private: TSharedRef<FCase> State;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioComponentLifetimeTest,"VaRest.SocketIO.Blueprint.Components",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioComponentLifetimeTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSioActionTest::FComponentCase>(*this); if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioActionTest::FComponentAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioBlueprintActionsTest,"VaRest.SocketIO.Blueprint.Actions",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioBlueprintActionsTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSioActionTest::FCase>(*this); if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioActionTest::FAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioBlueprintCompiledTest,"VaRest.SocketIO.Blueprint.Compiled",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioBlueprintCompiledTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSioActionTest::FGraphCase>(*this); if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioActionTest::FGraphAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioBlueprintAssetTest,"VaRest.SocketIO.Blueprint.Asset",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioBlueprintAssetTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSioActionTest::FGraphCase>(*this,true,true); if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioActionTest::FGraphAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioBlueprintLiteralTest,"VaRest.SocketIO.Blueprint.Literal",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioBlueprintLiteralTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSioActionTest::FGraphCase>(*this,true); if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioActionTest::FGraphAdvance(State)); return true;
}
#endif
