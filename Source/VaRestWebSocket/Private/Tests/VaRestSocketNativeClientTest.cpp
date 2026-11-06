// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestSocket.h"
#include "VaRestSocketHub.h"
#include "VaRestSocketRequest.h"
#include "VaRestHub.h"
#include "VaRestClock.h"
#include "VaRestFakeSocketTransport.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace VaRestSocketNativeClientTest
{
FString Text(const FVaRestSocketFrame& Frame)
{
 return Frame.Kind==EVaRestSocketFrameKind::Text && Frame.IsValid()
  ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Frame.Storage.GetData()+Frame.Offset),Frame.Length) : FString();
}
class FReadyHoldStage final : public IVaRestSocketStage
{
public:
 void OnOpen(const FVaRestSocketOpen&,FVaRestSocketStageContext& Context) override
 { check(IsInGameThread()); Context.HoldInbound(); }
 EVaRestSocketStageResult OnInbound(const FVaRestSocketMessage& Message,FVaRestSocketStageContext& Context) override
 {
  check(IsInGameThread());
  if (Message.Route==TEXT("welcome")) Context.SetReady();
  return EVaRestSocketStageResult::Continue;
 }
};
struct FCase : TSharedFromThis<FCase,ESPMode::ThreadSafe>
{
 explicit FCase(FAutomationTestBase& InTest,bool InReconnect=false,bool InHoldInbound=false)
  :Test(InTest),bReconnect(InReconnect),bHoldInbound(InHoldInbound) {}
 ~FCase() { Cleanup(); }
 FAutomationTestBase& Test;
 TStrongObjectPtr<UVaRestHub> Core{NewObject<UVaRestHub>()};
 TStrongObjectPtr<UVaRestSocketHub> Hub{NewObject<UVaRestSocketHub>()};
 TSharedRef<FVaRestFakeSocketTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeSocketTransport,ESPMode::ThreadSafe>();
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
 TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> WrongThread=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
 FVaRestSocket Socket;
 FVaRestSocketRequestHandle Request,Timeout,Cancel,Interrupted;
 FVaRestSocketMessage SavedRequest;
 FVaRestSocketClose LastClose;
 FVaRestFakeSocketAttempt* FirstAttempt=nullptr;
 int32 Phase=0,Connected=0,Ready=0,Events=0,Binary=0,ServerRequests=0,Replies=0,TimedOut=0,Canceled=0,Disconnected=0,Errors=0;
 int32 SavedRequests=0,InterruptedErrors=0,DropErrors=0,Retries=0,StaleReplyErrors=0;
 int64 RefusalsBeforeStale=0;
 double Deadline=0;
 const bool bReconnect;
 const bool bHoldInbound;
 bool Cleaned=false,bExpectDrop=false,bExpectStaleReply=false;
 static TSharedPtr<FCase,ESPMode::ThreadSafe> Pin(const TWeakPtr<FCase,ESPMode::ThreadSafe>& Weak,const TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe>& Wrong)
 {
  if (!IsInGameThread()) { Wrong->store(true,std::memory_order_release); return {}; }
  return Weak.Pin(); // no GTK/UObject owner can be retained by a misdelivered worker callback
 }
 bool Begin()
 {
  FString Error;
  if (!Test.TestTrue(TEXT("owned real editor core Hub"),Core->InitializeOnGT(nullptr,true,Error)) ||
   !Test.TestTrue(TEXT("owned real socket Hub"),Hub->InitializeOnGT(Core.Get())) ||
   !Test.TestTrue(TEXT("fake dependencies installed before publication"),Hub->ConfigureTestingOnGT(Fake,Clock,731))) return false;
  Fake->OnConnect=[](FVaRestFakeSocketAttempt& Attempt) { Attempt.Open(); };
  auto Routing=FVaRestSocketRouting::ByField(TEXT("type")).DataAt(TEXT("data")); Routing.IdField=TEXT("id"); Routing.ReplyRoute=TEXT("reply");
  Socket=Hub->Make(TEXT("NativeClient"),TEXT("ws://native-client.invalid/socket")).WithRouting(Routing).ReadyWhen(TEXT("welcome")).NoReconnect().WithLogLevel(EVaRestLogLevel::None);
  if (bHoldInbound) Socket.Use(MakeShared<FReadyHoldStage>());
  if (bReconnect)
  {
   FVaRestReconnectPolicy Policy; Policy.BaseDelay=.1; Policy.MaxDelay=.1; Policy.MaxAttempts=2;
   Policy.Jitter=EVaRestSocketJitter::Ratio; Policy.JitterRatio=0;
   Socket.WithReconnect(Policy);
  }
  if (!Test.TestTrue(TEXT("public native socket identity"),Socket.IsValid())) return false;
  Test.TestTrue(TEXT("named lookup returns the same native identity"),Hub->Find(TEXT("NativeClient")).IsSameSocket(Socket));
  TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
  Socket.OnConnected([Weak,Wrong](const FVaRestSocketOpen&) { if (auto S=Pin(Weak,Wrong)) ++S->Connected; });
  Socket.OnReady([Weak,Wrong](const FVaRestSocketOpen&) { if (auto S=Pin(Weak,Wrong)) ++S->Ready; });
  Socket.OnDisconnected([Weak,Wrong](const FVaRestSocketClose& Close) { if (auto S=Pin(Weak,Wrong)) { ++S->Disconnected; S->LastClose=Close; } });
  Socket.OnError([Weak,Wrong](const FVaRestError& Failure)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    if (S->bExpectDrop && Failure.Kind==EVaRestErrorKind::Connection) ++S->DropErrors;
    else if (S->bExpectStaleReply && Failure.Kind==EVaRestErrorKind::Canceled) ++S->StaleReplyErrors;
    else { ++S->Errors; S->Test.AddError(Failure.Message); }
   }
  });
  Socket.OnRetryScheduled([Weak,Wrong](const FVaRestSocketStatus& Status,const FVaRestSocketClose& Close)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Retries; S->Test.TestTrue(TEXT("retry follows classified disconnect"),Close.bWillReconnect && S->Disconnected==1 && S->DropErrors==1);
    S->Test.TestEqual(TEXT("owned online retry uses backoff"),Status.WaitReason,EVaRestSocketWait::Backoff);
   }
  });
  Socket.On(TEXT("hello"),TFunction<void(const FVaRestJson&)>([Weak,Wrong](const FVaRestJson& Data)
  { if (auto S=Pin(Weak,Wrong)) { ++S->Events; S->Test.TestEqual(TEXT("routed native data"),Data[TEXT("n")].AsInt(),7); } }));
  Socket.OnBinary([Weak,Wrong](TConstArrayView<uint8> Bytes,const FVaRestSocketMessage& Message)
  { if (auto S=Pin(Weak,Wrong)) { ++S->Binary; S->Test.TestTrue(TEXT("opaque binary preserves NUL/high byte"),TArray<uint8>(Bytes)==TArray<uint8>({0,255,7})); S->Test.TestTrue(TEXT("native binary kind"),Message.IsBinary()); } });
  Socket.OnRequest(TEXT("ask"),TFunction<void(const FVaRestSocketMessage&)>([Weak,Wrong](const FVaRestSocketMessage& Message)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->ServerRequests;
    S->Test.TestTrue(TEXT("server numeric correlation identity"),Message.bHasId && Message.bNumericId && Message.NumericId==17);
    S->Test.TestTrue(TEXT("server request owns a live once reply token"),Message.CanReplyOnGT());
    const auto Sent=Message.Reply(FVaRestJson::Object().Set(TEXT("answer"),42));
    S->Test.TestTrue(TEXT("reply admitted to native send staging"),Sent==EVaRestSendResult::Sent || Sent==EVaRestSendResult::Queued);
    S->Test.TestFalse(TEXT("reply token retires for every copy"),Message.CanReplyOnGT());
    S->Test.TestEqual(TEXT("duplicate reply is dropped"),Message.Reply(FVaRestJson::From(9)),EVaRestSendResult::Dropped);
   }
  }));
  Socket.OnRequest(TEXT("later"),TFunction<void(const FVaRestSocketMessage&)>([Weak,Wrong](const FVaRestSocketMessage& Message)
  { if (auto S=Pin(Weak,Wrong)) { ++S->SavedRequests; S->SavedRequest=Message; } }));
  Socket.Connect(); Deadline=FPlatformTime::Seconds()+25; return true;
 }
 bool Sent(FStringView Route) const
 {
  if (!Fake->NumAttempts()) return false;
  for (const auto& Frame:Fake->GetLast().GetSent())
  {
   const auto Wire=Text(Frame); if (Wire.IsEmpty()) continue;
   auto Json=FVaRestJson::Parse(FStringView(Wire));
   if (Json.HasValue() && Json.GetValue()[TEXT("type")].AsString()==Route) return true;
  }
  return false;
 }
 bool ServerReplySent() const
 {
  for (const auto& Frame:Fake->GetLast().GetSent())
  {
   const auto Wire=Text(Frame); if (Wire.IsEmpty()) continue;
   auto Json=FVaRestJson::Parse(FStringView(Wire)); if (!Json.HasValue()) continue;
   const auto& Value=Json.GetValue();
   if (Value[TEXT("type")].AsString()==TEXT("reply") && Value[TEXT("id")].AsInt()==17 && Value[TEXT("data")][TEXT("answer")].AsInt()==42) return true;
  }
  return false;
 }
 FString Reply() const
 { return FVaRestJson::Object().Set(TEXT("type"),TEXT("reply")).Set(TEXT("id"),Request.GetId()).Set(TEXT("data"),FVaRestJson::Object().Set(TEXT("answer"),42)).ToString(); }
 bool Update()
 {
  if (FPlatformTime::Seconds()>Deadline)
  {
   Test.AddError(FString::Printf(TEXT("native WS phase=%d timed out: connected=%d ready=%d events=%d binary=%d serverRequests=%d replies=%d timedOut=%d canceled=%d disconnected=%d attempts=%d pending=%d retries=%d interrupted=%d saved=%d dropErrors=%d"),
    Phase,Connected,Ready,Events,Binary,ServerRequests,Replies,TimedOut,Canceled,Disconnected,Fake->NumAttempts(),Socket.GetStats().PendingRequests,Retries,InterruptedErrors,SavedRequests,DropErrors)); Cleanup(); return true;
  }
  TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
  if (Phase==0)
  {
   if (Connected!=1) return false;
   Test.TestTrue(TEXT("connected before application readiness"),Socket.IsConnected()); Test.TestFalse(TEXT("ready route is still required"),Socket.IsReady());
   Test.TestEqual(TEXT("pre-ready message queues"),Socket.Emit(TEXT("queued"),FVaRestJson::From(1)),EVaRestSendResult::Queued);
   Test.TestFalse(TEXT("pre-ready message is not sent early"),Sent(TEXT("queued")));
   if (bHoldInbound)
   {
    Fake->GetLast().Text(TEXT("{\"type\":\"hello\",\"data\":{\"n\":7}}")); Phase=-1;
   }
   else { Fake->GetLast().Text(TEXT("{\"type\":\"welcome\",\"data\":{}}")); Phase=1; }
  }
  else if (Phase==-1)
  {
   if (Socket.GetStats().FramesIn<1) return false; // the early message was already decoded before the next frame exists
   Test.TestEqual(TEXT("ready hold prevents early application delivery"),Events,0);
   Test.TestFalse(TEXT("early data did not satisfy readiness"),Socket.IsReady());
   Fake->GetLast().Text(TEXT("{\"type\":\"welcome\",\"data\":{}}")); Phase=1;
  }
  else if (Phase==1)
  {
   if (Ready!=1 || !Sent(TEXT("queued")) || (bHoldInbound && Events!=1)) return false;
   Test.TestTrue(TEXT("ready event reflects actual status"),Socket.IsReady());
   if (bHoldInbound) Test.TestEqual(TEXT("next-batch readiness releases exactly the earlier held message"),Events,1);
   Fake->GetLast().Text(TEXT("{\"type\":\"hello\",\"data\":{\"n\":7}}"));
   Fake->GetLast().Text(TEXT("{\"type\":\"hello\",\"data\":{\"n\":7}}"));
   const TArray<uint8> Bytes{0,255,7}; Fake->GetLast().Message(EVaRestSocketFrameKind::Binary,Bytes);
   Fake->GetLast().Text(TEXT("{\"type\":\"ask\",\"id\":17,\"data\":{}}"));
   Request=Socket.Request(TEXT("calculate"),FVaRestJson::From(7)).Timeout(2).OnSuccess([Weak,Wrong](const FVaRestJson& Data)
   { if (auto S=Pin(Weak,Wrong)) { ++S->Replies; S->Test.TestEqual(TEXT("client correlated reply"),Data[TEXT("answer")].AsInt(),42); } })
    .OnError([Weak,Wrong](const FVaRestError& Failure) { if (auto S=Pin(Weak,Wrong)) S->Test.AddError(Failure.Message); }).Send(); ++Phase;
  }
  else if (Phase==2)
  {
   if (Events!=(bHoldInbound ? 3 : 2) || Binary!=1 || ServerRequests!=1 || !ServerReplySent() || !Sent(TEXT("calculate"))) return false;
   Test.TestTrue(TEXT("admitted request active before reply"),Request.IsActive()); Fake->GetLast().Text(Reply()); ++Phase;
  }
  else if (Phase==3)
  {
   if (Replies!=1) return false;
   Test.TestFalse(TEXT("reply retires request identity"),Request.IsActive()); Fake->GetLast().Text(Reply());
   Timeout=Socket.Request(TEXT("timeout"),FVaRestJson::Null()).Timeout(.1).OnError([Weak,Wrong](const FVaRestError& Failure)
   { if (auto S=Pin(Weak,Wrong)) { ++S->TimedOut; S->Test.TestEqual(TEXT("reply timeout kind"),Failure.Kind,EVaRestErrorKind::Timeout); } }).Send();
   Cancel=Socket.Request(TEXT("cancel"),FVaRestJson::Null()).Timeout(0).OnError([Weak,Wrong](const FVaRestError& Failure)
   { if (auto S=Pin(Weak,Wrong)) { ++S->Canceled; S->Test.TestEqual(TEXT("explicit cancel kind"),Failure.Kind,EVaRestErrorKind::Canceled); } }).Send(); Cancel.Cancel(); ++Phase;
  }
  else if (Phase==4)
  {
   if (!Sent(TEXT("timeout")) || Canceled!=1) return false;
   Clock->Advance(.2); ++Phase;
  }
  else if (Phase==5)
  {
   if (TimedOut!=1 || Socket.GetStats().FramesIn<7) return false;
   Test.TestEqual(TEXT("late reply cannot redeliver success"),Replies,1);
   Test.TestEqual(TEXT("all pending requests retired"),Socket.GetStats().PendingRequests,0);
   Test.TestFalse(TEXT("timed-out handle inactive"),Timeout.IsActive()); Test.TestFalse(TEXT("canceled handle inactive"),Cancel.IsActive());
   if (!bReconnect) { Fake->GetLast().ServerClose(1000,TEXT("done")); Phase=10; }
   else
   {
    Fake->GetLast().Text(TEXT("{\"type\":\"later\",\"id\":18,\"data\":{}}"));
    Interrupted=Socket.Request(TEXT("interrupted"),FVaRestJson::Null()).Timeout(0).OnError([Weak,Wrong](const FVaRestError& Failure)
    { if (auto S=Pin(Weak,Wrong)) { ++S->InterruptedErrors; S->Test.TestEqual(TEXT("sent request fails on connection loss"),Failure.Kind,EVaRestErrorKind::Connection); } }).Send(); ++Phase;
   }
  }
  else if (Phase==6)
  {
   if (SavedRequests!=1 || !Sent(TEXT("interrupted"))) return false;
   Test.TestTrue(TEXT("unanswered server token is live before loss"),SavedRequest.CanReplyOnGT());
   FirstAttempt=&Fake->GetLast(); bExpectDrop=true; FirstAttempt->Drop(); ++Phase;
  }
  else if (Phase==7)
  {
   if (Disconnected!=1 || InterruptedErrors!=1 || Retries!=1) return false;
   Test.TestEqual(TEXT("transport loss code"),LastClose.Code,1006);
   Test.TestTrue(TEXT("old attempt released before retry"),FirstAttempt->WasReleased());
   Test.TestFalse(TEXT("lost request is inactive"),Interrupted.IsActive());
   Clock->Advance(.2); ++Phase;
  }
  else if (Phase==8)
  {
   if (Fake->NumAttempts()!=2 || Connected!=2) return false;
   Test.TestFalse(TEXT("new connection requires its own ready route"),Socket.IsReady());
   Test.TestFalse(TEXT("old server token cannot cross connection generation"),SavedRequest.CanReplyOnGT());
   bExpectStaleReply=true; RefusalsBeforeStale=Socket.GetStats().SendRefusals;
   Test.TestEqual(TEXT("stale server reply is dropped"),SavedRequest.Reply(FVaRestJson::From(1)),EVaRestSendResult::Dropped);
   Fake->GetLast().Text(TEXT("{\"type\":\"welcome\",\"data\":{}}")); ++Phase;
  }
  else if (Phase==9)
  {
   if (Ready!=2 || StaleReplyErrors!=1) return false;
   Test.TestEqual(TEXT("successful reconnect counter"),Socket.GetStats().Reconnects,int64(1));
   Test.TestEqual(TEXT("exact attempt counter"),Socket.GetStats().Attempts,int64(2));
   Test.TestEqual(TEXT("one classified transport error"),DropErrors,1);
   Test.TestEqual(TEXT("one reported stale-reply refusal"),StaleReplyErrors,1);
   Test.TestEqual(TEXT("stale reply increments the native refusal count"),Socket.GetStats().SendRefusals,RefusalsBeforeStale+1);
   Socket.NoReconnect(); Fake->GetLast().ServerClose(1000,TEXT("done")); ++Phase;
  }
  else
  {
   if (Disconnected!=(bReconnect ? 2 : 1) || Socket.GetState()!=EVaRestSocketState::Disconnected || !Fake->GetLast().WasReleased()) return false;
   Test.TestEqual(TEXT("clean final server close code"),LastClose.Code,1000);
   Test.TestEqual(TEXT("exact admitted attempts"),Fake->NumAttempts(),bReconnect ? 2 : 1);
   Test.TestEqual(TEXT("no unexpected socket errors"),Errors,0); Cleanup(); return true;
  }
  return false;
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  Request.Cancel(); Timeout.Cancel(); Cancel.Cancel(); Interrupted.Cancel(); SavedRequest={};
  if (Hub) Hub->ShutdownOnGT(); if (Core) Core->DeinitializeOnGT(); Socket={};
  Test.TestFalse(TEXT("every callback stayed GTK"),WrongThread->load(std::memory_order_acquire));
  Test.TestEqual(TEXT("owned socket timers retired"),Clock->NumPendingTimers(),0);
  if (Fake->NumAttempts()) Test.TestTrue(TEXT("Hub releases its fake connection"),Fake->GetLast().WasReleased());
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketNativeClientTest,"VaRest.WebSocket.Features.NativeClient",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSocketNativeClientTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSocketNativeClientTest::FCase,ESPMode::ThreadSafe>(*this);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSocketNativeClientTest::FAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketNativeReconnectTest,"VaRest.WebSocket.Features.NativeReconnect",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSocketNativeReconnectTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSocketNativeClientTest::FCase,ESPMode::ThreadSafe>(*this,true);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSocketNativeClientTest::FAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSocketNativeReadyHoldTest,"VaRest.WebSocket.Features.ReadyHold",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSocketNativeReadyHoldTest::RunTest(const FString& Parameters)
{
 auto State=MakeShared<VaRestSocketNativeClientTest::FCase,ESPMode::ThreadSafe>(*this,false,true);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSocketNativeClientTest::FAdvance(State)); return true;
}
#endif
