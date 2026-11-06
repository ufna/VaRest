// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestSioSocket.h"
#include "VaRestSioClient.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "VaRestSocketHub.h"
#include "VaRestHub.h"
#include "VaRestClock.h"
#include "VaRestFakeSocketTransport.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace VaRestSioNativeSessionTest
{
FString Text(const FVaRestSocketFrame& Frame)
{
 return Frame.Kind==EVaRestSocketFrameKind::Text && Frame.IsValid()
  ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Frame.Storage.GetData()+Frame.Offset),Frame.Length) : FString();
}
struct FCase : TSharedFromThis<FCase,ESPMode::ThreadSafe>
{
 explicit FCase(FAutomationTestBase& InTest,bool InRecovery=false):Test(InTest),bRecovery(InRecovery) {}
 ~FCase() { Cleanup(); }
 FAutomationTestBase& Test;
 TStrongObjectPtr<UVaRestHub> Core{NewObject<UVaRestHub>()};
 TStrongObjectPtr<UVaRestSocketHub> Hub{NewObject<UVaRestSocketHub>()};
 TSharedRef<FVaRestFakeSocketTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeSocketTransport,ESPMode::ThreadSafe>();
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
 FVaRestSioSocket Socket;
 TStrongObjectPtr<UVaRestSioClient> View;
 FVaRestSioAckHandle Ack,TimeoutAck,Interrupted,AfterRecovery;
 FVaRestSioAck SavedServerAck;
 FVaRestSioConnect LastConnect;
 FVaRestSioDisconnect LastDisconnect;
 FVaRestFakeSocketAttempt* FirstAttempt=nullptr;
 FString InterruptedId;
 TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> WrongThread=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
 int32 Checkpoints=0,SavedRequests=0,InterruptedErrors=0,Disconnected=0,Retries=0,DropErrors=0,BearerReads=0,AuthBuilds=0;
 const bool bRecovery;
 bool bExpectDrop=false;
 int32 Phase=0,Connected=0,Events=0,Once=0,Binary=0,Acked=0,TimedOut=0,Errors=0;
 double Deadline=0;
 bool Cleaned=false;
 static TSharedPtr<FCase,ESPMode::ThreadSafe> Pin(const TWeakPtr<FCase,ESPMode::ThreadSafe>& Weak,const TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe>& Wrong)
 {
  if (!IsInGameThread()) { Wrong->store(true,std::memory_order_release); return {}; }
  return Weak.Pin();
 }
 bool Begin()
 {
  FString Error;
  if (!Test.TestTrue(TEXT("real editor core Hub initializes"),Core->InitializeOnGT(nullptr,true,Error)) ||
   !Test.TestTrue(TEXT("real socket Hub initializes"),Hub->InitializeOnGT(Core.Get())) ||
   !Test.TestTrue(TEXT("test dependencies installed before identity creation"),Hub->ConfigureTestingOnGT(Fake,Clock,123))) return false;
  Fake->OnConnect=[](FVaRestFakeSocketAttempt& Attempt)
  {
   Attempt.Open();
   Attempt.Text(TEXT("0{\"sid\":\"engine-private\",\"upgrades\":[],\"pingInterval\":60000,\"pingTimeout\":20000,\"maxPayload\":1000000}"));
  };
  Socket=FVaRestSio::Make(Hub.Get(),TEXT("NativeSession"),TEXT("ws://sio.invalid/room")).NoReconnect();
  if (bRecovery)
  {
   FVaRestReconnectPolicy Policy; Policy.BaseDelay=.1; Policy.MaxDelay=.1; Policy.MaxAttempts=2;
   Policy.Jitter=EVaRestSocketJitter::Ratio; Policy.JitterRatio=0;
   Socket.WithReconnect(Policy).WithRecovery(true);
  }
  if (!Test.TestTrue(TEXT("public Socket.IO factory creates native identity"),Socket.IsValid())) return false;
  Test.TestFalse(TEXT("Hub dependencies cannot change after identity publication"),Hub->ConfigureTestingOnGT(Fake,Clock,124));
  TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
  if (bRecovery)
  {
   Socket.WithBearer([Weak,Wrong]() -> FString
   {
    if (auto S=Pin(Weak,Wrong)) return FString::Printf(TEXT("owned-sio-token-%d"),++S->BearerReads);
    return {};
   }).WithAuthPlacement(FVaRestSioAuthPlacement::Payload(TEXT("access"),TEXT("Token ")))
    .WithAuth(FVaRestJson::Object().Set(TEXT("app"),TEXT("synthetic-game")))
    .OnConnectAuth(nullptr,[Weak,Wrong](FVaRestJson& Payload,const FVaRestSioConnectContext& Info)
    {
     if (auto S=Pin(Weak,Wrong))
     {
      ++S->AuthBuilds; S->Test.TestEqual(TEXT("CONNECT hook namespace"),Info.Namespace,FString(TEXT("/room")));
      S->Test.TestEqual(TEXT("CONNECT hook receives the physical attempt"),Info.Attempt,S->AuthBuilds);
      S->Test.TestEqual(TEXT("CONNECT hook distinguishes reconnect"),Info.bReconnect,S->AuthBuilds>1);
      Payload.Set(TEXT("nonce"),Info.Attempt);
     }
    });
  }
  Socket.OnConnect([Weak,Wrong](const FVaRestSioConnect& Info)
  { if (auto S=Pin(Weak,Wrong)) { ++S->Connected; S->LastConnect=Info; } });
  Socket.OnError([Weak,Wrong](const FVaRestError& Error)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    if (S->bExpectDrop && Error.Kind==EVaRestErrorKind::Connection) ++S->DropErrors;
    else { ++S->Errors; S->Test.AddError(Error.Message); }
   }
  });
  Socket.OnDisconnect(nullptr,[Weak,Wrong](const FVaRestSioDisconnect& Info)
  { if (auto S=Pin(Weak,Wrong)) { ++S->Disconnected; S->LastDisconnect=Info; } });
  Socket.OnRetryScheduled(nullptr,[Weak,Wrong](const FVaRestSocketStatus& Status,const FVaRestSocketClose& Close)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Retries; S->Test.TestTrue(TEXT("transport loss admits reconnect"),Close.bWillReconnect);
    S->Test.TestEqual(TEXT("retry waits in backoff"),Status.WaitReason,EVaRestSocketWait::Backoff);
   }
  });
  Socket.On(TEXT("checkpoint"),TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent& Event)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Checkpoints; S->Test.TestEqual(TEXT("dispatched recovery offset exposed"),Event.Offset,FString(TEXT("offset-1")));
    S->Test.TestEqual(TEXT("recovery offset remains in argument view"),Event.Args[1].AsString(),FString(TEXT("offset-1")));
   }
  }));
  Socket.On(TEXT("later"),TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent& Event)
  { if (auto S=Pin(Weak,Wrong)) { ++S->SavedRequests; S->SavedServerAck=Event.Ack; } }));
  Socket.On(TEXT("hello"),TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent& Event)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Events; S->Test.TestEqual(TEXT("event argument"),Event.Args[0].AsInt(),7);
    S->Test.TestTrue(TEXT("server event exposes live ACK token"),Event.Ack.CanReplyOnGT());
    Event.Ack.Send(FString(TEXT("ok")));
    S->Test.TestFalse(TEXT("all ACK copies observe single-use retirement"),Event.Ack.CanReplyOnGT());
   }
  }));
  Socket.Once(TEXT("hello"),TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent&) { if (auto S=Pin(Weak,Wrong)) ++S->Once; }));
  Socket.On(TEXT("binary"),TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent& Event)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Binary; const auto Bytes=Event.TryArg<TArray<uint8>>(0);
    S->Test.TestTrue(TEXT("placeholder resolves to the exact binary attachment"),Bytes.IsSet() && Bytes.GetValue()==TArray<uint8>({1,0,3}));
   }
  }));
  View.Reset(UVaRestSioClient::WrapOnGT(Hub.Get(),Socket));
  if (!Test.TestNotNull(TEXT("owned actual Blueprint client view"),View.Get())) return false;
  Test.TestEqual(TEXT("wrapping one native identity reuses one UObject view"),UVaRestSioClient::WrapOnGT(Hub.Get(),Socket),View.Get());
  View->Connect(); Deadline=FPlatformTime::Seconds()+25; return true;
 }
 bool Sent(FStringView Value) const
 {
  if (!Fake->NumAttempts()) return false;
  for (const auto& Frame:Fake->GetLast().GetSent()) if (Text(Frame)==Value) return true;
  return false;
 }
 FString EventId(const TCHAR* Name) const
 {
  if (!Fake->NumAttempts()) return {};
  const FString Needle=TEXT("[\"")+FString(Name)+TEXT("\"");
  for (const auto& Frame:Fake->GetLast().GetSent())
  {
   const FString Wire=Text(Frame); if (!Wire.StartsWith(TEXT("42/room,")) || !Wire.Contains(Needle)) continue;
   const int32 Array=Wire.Find(TEXT("[")); return Array>8 ? Wire.Mid(8,Array-8) : FString();
  }
  return {};
 }
 void CheckBlueprintView(FStringView Sid,bool Recovered)
 {
  Test.TestTrue(TEXT("Blueprint default namespace is connected"),View->IsNamespaceConnected());
  Test.TestEqual(TEXT("Blueprint default namespace state"),View->GetNamespaceState(),EVaRestSioNamespaceState::Connected);
  Test.TestEqual(TEXT("Blueprint default sid"),View->GetSid(),FString(Sid));
  Test.TestEqual(TEXT("Blueprint default recovery flag"),View->WasRecovered(),Recovered);
  Test.TestTrue(TEXT("explicit namespace is an independent positive control"),View->IsNamespaceConnected(TEXT("/room")) && View->GetSid(TEXT("/room"))==Sid);
  Test.TestFalse(TEXT("unknown namespace getter does not create readiness"),View->IsNamespaceConnected(TEXT("/absent")));
  Test.TestEqual(TEXT("Blueprint connection info agrees with native readiness"),View->GetLastConnectInfoOnGT().Sid,FString(Sid));
  auto* Function=View->FindFunction(TEXT("GetSid"));
  if (!Test.TestNotNull(TEXT("GetSid reflected function"),Function)) return;
  FStructOnScope Parameters(Function);
  const auto* Return=FindFProperty<FStrProperty>(Function,TEXT("ReturnValue"));
  if (!Test.TestNotNull(TEXT("GetSid reflected string result"),Return)) return;
  View->ProcessEvent(Function,Parameters.GetStructMemory());
  Test.TestEqual(TEXT("actual reflected GetSid call honors empty default namespace"),Return->GetPropertyValue_InContainer(Parameters.GetStructMemory()),FString(Sid));
 }
 void CheckConnectAuth(const FVaRestJson& Payload,int32 Attempt)
 {
  Test.TestEqual(TEXT("bearer source sampled once per physical attempt"),BearerReads,Attempt);
  Test.TestEqual(TEXT("auth hook runs once per CONNECT"),AuthBuilds,Attempt);
  Test.TestEqual(TEXT("custom CONNECT fields survive session auth"),Payload[TEXT("app")].AsString(),FString(TEXT("synthetic-game")));
  Test.TestEqual(TEXT("configured payload auth field and prefix"),Payload[TEXT("access")].AsString(),FString::Printf(TEXT("Token owned-sio-token-%d"),Attempt));
  Test.TestEqual(TEXT("hook output belongs to this attempt"),Payload[TEXT("nonce")].AsInt(),Attempt);
  const auto& Request=Fake->GetLast().GetRequest();
  Test.TestFalse(TEXT("payload placement removes bearer upgrade header"),Request.Headers.ContainsByPredicate([](const auto& Header) { return Header.Key.Equals(TEXT("Authorization"),ESearchCase::IgnoreCase); }));
  Test.TestFalse(TEXT("payload token never becomes a URL query"),Request.Url.Contains(TEXT("owned-sio-token-")));
 }
 bool Update()
 {
  if (FPlatformTime::Seconds()>Deadline)
  {
   const auto Stats=Socket.GetStats();
   Test.AddError(FString::Printf(TEXT("Socket.IO phase %d timed out: connected=%d events=%d once=%d binary=%d acked=%d timeout=%d errors=%d pong=%d sent=%d packetsIn=%lld pings=%lld attempts=%d checkpoints=%d lostAck=%d retries=%d dropErrors=%d"),
    Phase,Connected,Events,Once,Binary,Acked,TimedOut,Errors,Sent(TEXT("3")),Fake->NumAttempts() ? Fake->GetLast().GetSent().Num() : 0,Stats.EventsIn,Stats.PingsAnswered,Fake->NumAttempts(),Checkpoints,InterruptedErrors,Retries,DropErrors));
   Cleanup(); return true;
  }
  if (Phase==0)
  {
   if (!Fake->NumAttempts()) return false;
   bool ConnectSent=false; FString ConnectBody;
   for (const auto& Frame:Fake->GetLast().GetSent()) if (Text(Frame).StartsWith(TEXT("40/room,"))) { ConnectSent=true; ConnectBody=Text(Frame).Mid(8); }
   if (!ConnectSent) return false;
   Test.TestTrue(TEXT("handshake uses EIO4 WebSocket transport"),Fake->GetLast().GetRequest().Url.Contains(TEXT("EIO=4")) && Fake->GetLast().GetRequest().Url.Contains(TEXT("transport=websocket")));
   if (bRecovery)
   {
    auto Auth=FVaRestJson::Parse(FStringView(ConnectBody)); Test.TestTrue(TEXT("initial authenticated CONNECT JSON"),Auth.HasValue());
    if (Auth.HasValue()) CheckConnectAuth(Auth.GetValue(),1);
   }
   Test.TestEqual(TEXT("namespace waits for its own CONNECT reply"),Connected,0);
   Test.TestEqual(TEXT("negotiated service watchdog command admitted"),Fake->GetLast().GetIdleDeadlineSeconds(),80.0);
   Fake->GetLast().Text(TEXT("40/room,{\"sid\":\"room-public\",\"pid\":\"recovery-private\"}")); ++Phase;
  }
  else if (Phase==1)
  {
   if (!Connected) return false;
   CheckBlueprintView(TEXT("room-public"),false);
   Test.TestTrue(TEXT("public namespace state is connected"),Socket.Namespace(TEXT("/room")).IsConnected());
   Test.TestEqual(TEXT("public namespace sid"),Socket.Namespace(TEXT("/room")).GetSid(),FString(TEXT("room-public")));
   TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
   Ack=Socket.EmitWithAck(TEXT("ask"),42).OnSuccess(TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent& Event)
   { if (auto S=Pin(Weak,Wrong)) { ++S->Acked; S->Test.TestEqual(TEXT("client ACK payload"),Event.Args[0].AsInt(),99); } })).Send();
   ++Phase;
  }
  else if (Phase==2)
  {
   const FString Id=EventId(TEXT("ask")); if (Id.IsEmpty()) return false;
   Fake->GetLast().Text(TEXT("43/room,")+Id+TEXT("[99]"));
   Fake->GetLast().Text(TEXT("42/room,17[\"hello\",7]"));
   Fake->GetLast().Text(TEXT("42/room,18[\"hello\",7]"));
   Fake->GetLast().Text(TEXT("451-/room,[\"binary\",{\"_placeholder\":true,\"num\":0}]"));
   const uint8 Bytes[]={1,0,3}; Fake->GetLast().Message(EVaRestSocketFrameKind::Binary,MakeArrayView(Bytes));
   Fake->GetLast().Text(TEXT("2")); ++Phase;
  }
  else if (Phase==3)
  {
   if (!Acked || Events<2 || !Binary || !Sent(TEXT("3"))) return false;
   Test.TestEqual(TEXT("one-shot observer retires before second event"),Once,1);
   Test.TestEqual(TEXT("success delivered once"),Acked,1); Test.TestFalse(TEXT("successful client ACK retired"),Ack.IsActive());
   Test.TestTrue(TEXT("server ACK17 written"),Sent(TEXT("43/room,17[\"ok\"]")));
   Test.TestTrue(TEXT("server ACK18 written"),Sent(TEXT("43/room,18[\"ok\"]")));
   TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
   TimeoutAck=Socket.EmitWithAck(TEXT("timeout"),1).Timeout(.1).OnError([Weak,Wrong](const FVaRestError& Error)
   { if (auto S=Pin(Weak,Wrong)) { ++S->TimedOut; S->Test.TestTrue(TEXT("ACK timeout reports Timeout"),Error.Kind==EVaRestErrorKind::Timeout); } }).Send();
   ++Phase;
  }
  else if (Phase==4)
  {
   if (EventId(TEXT("timeout")).IsEmpty()) return false;
   Clock->Advance(.2); ++Phase;
  }
  else if (Phase==5)
  {
   if (!TimedOut) return false;
   Test.TestEqual(TEXT("timeout delivered once"),TimedOut,1); Test.TestFalse(TEXT("timed out ACK retired"),TimeoutAck.IsActive());
   Test.TestEqual(TEXT("no unexpected socket errors"),Errors,0);
   if (!bRecovery) { Cleanup(); return true; }
   Fake->GetLast().Text(TEXT("42/room,[\"checkpoint\",7,\"offset-1\"]"));
   Fake->GetLast().Text(TEXT("42/room,19[\"later\",7]"));
   TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
   Interrupted=Socket.EmitWithAck(TEXT("interrupted"),1).Timeout(0).OnError([Weak,Wrong](const FVaRestError& Error)
   {
    if (auto S=Pin(Weak,Wrong))
    { ++S->InterruptedErrors; S->Test.TestEqual(TEXT("sent ACK fails on transport loss"),Error.Kind,EVaRestErrorKind::Connection); }
   }).Send();
   ++Phase;
  }
  else if (Phase==6)
  {
   InterruptedId=EventId(TEXT("interrupted"));
   if (Checkpoints!=1 || SavedRequests!=1 || InterruptedId.IsEmpty()) return false;
   Test.TestTrue(TEXT("old server ACK is live before loss"),SavedServerAck.CanReplyOnGT());
   Test.TestEqual(TEXT("one held ACK before loss"),Socket.GetStats().AcksPending,1);
   FirstAttempt=&Fake->GetLast(); bExpectDrop=true; FirstAttempt->Drop(); ++Phase;
  }
  else if (Phase==7)
  {
   if (!InterruptedErrors || !Disconnected || !DropErrors || !Retries) return false;
   Test.TestEqual(TEXT("sent ACK fails once without replay"),InterruptedErrors,1);
   Test.TestFalse(TEXT("lost ACK retires"),Interrupted.IsActive());
   Test.TestTrue(TEXT("namespace disconnect carries transport/reconnect reason"),LastDisconnect.Reason==EVaRestSioDisconnectReason::TransportClose && LastDisconnect.bWillReconnect);
   Test.TestEqual(TEXT("namespace loses readiness before retry"),Socket.Namespace(TEXT("/room")).GetState(),EVaRestSioNamespaceState::Connecting);
   Test.TestFalse(TEXT("server ACK pin invalidated by loss"),SavedServerAck.CanReplyOnGT());
   Test.TestTrue(TEXT("previous fake attempt released"),FirstAttempt->WasReleased());
   Clock->Advance(.2); ++Phase;
  }
  else if (Phase==8)
  {
   if (Fake->NumAttempts()!=2) return false;
   FString Connect;
   for (const auto& Frame:Fake->GetLast().GetSent()) if (Text(Frame).StartsWith(TEXT("40/room,"))) Connect=Text(Frame).Mid(8);
   if (Connect.IsEmpty()) return false;
   auto Payload=FVaRestJson::Parse(FStringView(Connect));
   Test.TestTrue(TEXT("recovery CONNECT contains valid JSON"),Payload.HasValue());
   if (Payload.HasValue())
   {
    CheckConnectAuth(Payload.GetValue(),2);
    Test.TestEqual(TEXT("recovery CONNECT carries private pid"),Payload.GetValue()[TEXT("pid")].AsString(),FString(TEXT("recovery-private")));
    Test.TestEqual(TEXT("recovery CONNECT carries last dispatched offset"),Payload.GetValue()[TEXT("offset")].AsString(),FString(TEXT("offset-1")));
   }
   Test.TestEqual(TEXT("new transport does not imply namespace readiness"),Connected,1);
   Test.TestFalse(TEXT("namespace requires fresh CONNECT reply"),Socket.Namespace(TEXT("/room")).IsConnected());
   Test.TestTrue(TEXT("sent non-retry ACK is absent on new attempt"),EventId(TEXT("interrupted")).IsEmpty());
   Fake->GetLast().Text(TEXT("40/room,{\"sid\":\"room-public-2\",\"pid\":\"recovery-private\"}")); ++Phase;
  }
  else if (Phase==9)
  {
   if (Connected!=2) return false;
   CheckBlueprintView(TEXT("room-public-2"),true);
   Test.TestTrue(TEXT("namespace reports recovered session"),LastConnect.bReconnect && LastConnect.bRecovered && Socket.Namespace(TEXT("/room")).WasRecovered());
   Test.TestEqual(TEXT("recovered public sid replaces old public sid"),Socket.Namespace(TEXT("/room")).GetSid(),FString(TEXT("room-public-2")));
   const auto Stats=Socket.GetStats(); Test.TestEqual(TEXT("one successful recovery"),Stats.RecoveriesSucceeded,int64(1));
   Test.TestEqual(TEXT("no failed recovery"),Stats.RecoveriesFailed,int64(0));
   Fake->GetLast().Text(TEXT("43/room,")+InterruptedId+TEXT("[100]"));
   TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
   AfterRecovery=Socket.EmitWithAck(TEXT("after-recovery"),42).OnSuccess(TFunction<void(const FVaRestSioEvent&)>([Weak,Wrong](const FVaRestSioEvent& Event)
   { if (auto S=Pin(Weak,Wrong)) { ++S->Acked; S->Test.TestEqual(TEXT("fresh post-recovery ACK payload"),Event.Args[0].AsInt(),99); } })).Send();
   ++Phase;
  }
  else if (Phase==10)
  {
   const auto Id=EventId(TEXT("after-recovery")); if (Id.IsEmpty()) return false;
   Test.TestTrue(TEXT("post-recovery ACK has a new correlation id"),Id!=InterruptedId);
   Fake->GetLast().Text(TEXT("43/room,")+Id+TEXT("[99]")); ++Phase;
  }
  else
  {
   if (Acked!=2) return false;
   Test.TestFalse(TEXT("fresh ACK retired"),AfterRecovery.IsActive());
   Test.TestEqual(TEXT("late old ACK did not cause a second failure"),InterruptedErrors,1);
   Test.TestEqual(TEXT("one transport error for the deliberate loss"),DropErrors,1);
   Test.TestEqual(TEXT("one reconnect scheduled"),Retries,1);
   Test.TestEqual(TEXT("all logical ACKs retired"),Socket.GetStats().AcksPending,0);
   Test.TestEqual(TEXT("no unexpected recovery errors"),Errors,0);
   Cleanup(); return true;
  }
  return false;
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  Ack.Cancel(); TimeoutAck.Cancel(); Interrupted.Cancel(); AfterRecovery.Cancel();
  if (Hub) Hub->ShutdownOnGT();
  if (Core) Core->DeinitializeOnGT();
  SavedServerAck={}; Socket={}; View.Reset();
  Test.TestEqual(TEXT("owned timers retired before clock destruction"),Clock->NumPendingTimers(),0);
  if (Fake->NumAttempts()) Test.TestTrue(TEXT("Hub teardown releases fake connection"),Fake->GetLast().WasReleased());
  Test.TestFalse(TEXT("native callbacks only pin GTK owners on game thread"),WrongThread->load(std::memory_order_acquire));
 }
};
class FAdvance final : public IAutomationLatentCommand
{
public:
 explicit FAdvance(TSharedRef<FCase,ESPMode::ThreadSafe> InState):State(MoveTemp(InState)) {}
 bool Update() override { return State->Update(); }
private: TSharedRef<FCase,ESPMode::ThreadSafe> State;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioNativeSessionTest,"VaRest.SocketIO.Features.NativeSession",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioNativeSessionTest::RunTest(const FString& Parameters)
{
 const auto State=MakeShared<VaRestSioNativeSessionTest::FCase,ESPMode::ThreadSafe>(*this);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioNativeSessionTest::FAdvance(State));
 return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioNativeRecoveryTest,"VaRest.SocketIO.Features.NativeRecovery",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioNativeRecoveryTest::RunTest(const FString& Parameters)
{
 const auto State=MakeShared<VaRestSioNativeSessionTest::FCase,ESPMode::ThreadSafe>(*this,true);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioNativeSessionTest::FAdvance(State));
 return true;
}
#endif
