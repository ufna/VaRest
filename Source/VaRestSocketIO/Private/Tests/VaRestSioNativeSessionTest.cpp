// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestSioSocket.h"
#include "VaRestSioClient.h"
#include "UObject/StructOnScope.h"
#include "UObject/UnrealType.h"
#include "VaRestSocketHub.h"
#include "VaRestHub.h"
#include "VaRestAuth.h"
#include "VaRestTrace.h"
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
 explicit FCase(FAutomationTestBase& InTest,bool InRecovery=false,bool InTypedBytes=false):Test(InTest),bRecovery(InRecovery),bTypedBytes(InTypedBytes) {}
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
 const bool bTypedBytes;
 int32 BytesValueCalls=0,BytesReferenceCalls=0,BytesDecodeErrors=0,BytesFences=0;
 bool bExpectBytesDecode=false;
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
  if (bTypedBytes) Socket.WithStrictDecode(true);
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
    if (S->bTypedBytes && S->bExpectBytesDecode && Error.Kind==EVaRestErrorKind::Decode)
    {
     ++S->BytesDecodeErrors;
     S->Test.TestTrue(TEXT("strict byte decode failure keeps the positional path"),Error.Message.Contains(TEXT("[0]")));
    }
    else if (S->bExpectDrop && Error.Kind==EVaRestErrorKind::Connection) ++S->DropErrors;
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
  if (bTypedBytes)
  {
   Socket.On(TEXT("bytes-value"),[Weak,Wrong](TArray<uint8> Value)
   { if (auto S=Pin(Weak,Wrong)) S->CheckTypedBytes(false,Value); });
   Socket.On(TEXT("bytes-reference"),[Weak,Wrong](const TArray<uint8>& Value)
   { if (auto S=Pin(Weak,Wrong)) S->CheckTypedBytes(true,Value); });
   Socket.On(TEXT("bytes-fence"),[Weak,Wrong](int32 Value)
   {
    if (auto S=Pin(Weak,Wrong))
    {
     ++S->BytesFences;
     S->Test.TestEqual(TEXT("ordered public typed fence after byte events"),Value,S->BytesFences);
    }
   });
  }
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
 void CheckTypedBytes(bool bReference,const TArray<uint8>& Value)
 {
  int32& Calls=bReference ? BytesReferenceCalls : BytesValueCalls; ++Calls;
  Test.TestTrue(TEXT("direct byte handler is called only for the six expected deliveries"),Calls<=6);
  const bool bEmpty=Calls==2 || Calls>=5;
  Test.TestTrue(bReference ? TEXT("direct const-reference byte argument has exact content/default") : TEXT("direct value byte argument has exact content/default"),
   bEmpty ? Value.IsEmpty() : Value==TArray<uint8>({0,255,0,128,1}));
 }
 void FeedInvalidTypedBytes()
 {
  // First valid quartet can write a prefix before the later invalid quartet.
  Fake->GetLast().Text(TEXT("42/room,[\"bytes-value\",\"AP8A!!!!\"]"));
  Fake->GetLast().Text(TEXT("42/room,[\"bytes-reference\",\"AP8A!!!!\"]"));
  // The JSON escape becomes a NUL INSIDE the counted Base64 string.
  Fake->GetLast().Text(TEXT("42/room,[\"bytes-value\",\"AP8A\\u0000AQ==\"]"));
  Fake->GetLast().Text(TEXT("42/room,[\"bytes-reference\",\"AP8A\\u0000AQ==\"]"));
 }
 bool UpdateTypedBytes()
 {
  if (Phase==0)
  {
   if (!Fake->NumAttempts()) return false;
   bool ConnectSent=false;
   for (const auto& Frame:Fake->GetLast().GetSent()) if (Text(Frame).StartsWith(TEXT("40/room,"))) ConnectSent=true;
   if (!ConnectSent) return false;
   Fake->GetLast().Text(TEXT("40/room,{\"sid\":\"typed-bytes-public\"}")); ++Phase;
  }
  else if (Phase==1)
  {
   if (Connected!=1) return false;
   Test.TestTrue(TEXT("typed byte fixture uses a real connected public namespace"),Socket.Namespace(TEXT("/room")).IsConnected());
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-value\",\"AP8AgAE=\"]"));
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-reference\",\"AP8AgAE=\"]"));
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-value\",\"\"]"));
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-reference\",\"\"]"));
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-value\",[0,255,0,128,1]]"));
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-reference\",[0,255,0,128,1]]"));
   Fake->GetLast().Text(TEXT("451-/room,[\"bytes-value\",{\"_placeholder\":true,\"num\":0}]"));
   const uint8 Bytes[]={0,255,0,128,1}; Fake->GetLast().Message(EVaRestSocketFrameKind::Binary,MakeArrayView(Bytes));
   Fake->GetLast().Text(TEXT("451-/room,[\"bytes-reference\",{\"_placeholder\":true,\"num\":0}]"));
   Fake->GetLast().Message(EVaRestSocketFrameKind::Binary,MakeArrayView(Bytes)); ++Phase;
  }
  else if (Phase==2)
  {
   if (BytesValueCalls<4 || BytesReferenceCalls<4) return false;
   Test.TestEqual(TEXT("Base64/empty/number-array/attachment value callbacks"),BytesValueCalls,4);
   Test.TestEqual(TEXT("Base64/empty/number-array/attachment const-reference callbacks"),BytesReferenceCalls,4);
   Test.TestEqual(TEXT("valid byte representations have no decode failures"),BytesDecodeErrors,0);
   bExpectBytesDecode=true; FeedInvalidTypedBytes();
   Fake->GetLast().Text(TEXT("42/room,[\"bytes-fence\",1]")); ++Phase;
  }
  else if (Phase==3)
  {
   if (BytesFences<1) return false;
   Test.TestEqual(TEXT("both strict handlers reject both invalid counted inputs"),BytesDecodeErrors,4);
   Test.TestEqual(TEXT("strict invalid input never reaches value callback"),BytesValueCalls,4);
   Test.TestEqual(TEXT("strict invalid input never reaches const-reference callback"),BytesReferenceCalls,4);
   bExpectBytesDecode=false; Socket.WithStrictDecode(false);
   Test.AddExpectedError(TEXT("Socket.IO handler materialized arguments with 1 decode warning"),EAutomationExpectedErrorFlags::Contains,4);
   FeedInvalidTypedBytes(); Fake->GetLast().Text(TEXT("42/room,[\"bytes-fence\",2]")); ++Phase;
  }
  else
  {
   if (BytesFences<2) return false;
   Test.TestEqual(TEXT("lenient invalid input delivers each empty value default once"),BytesValueCalls,6);
   Test.TestEqual(TEXT("lenient invalid input delivers each empty const-reference default once"),BytesReferenceCalls,6);
   Test.TestEqual(TEXT("lenient mismatches warn without new strict errors"),BytesDecodeErrors,4);
   Test.TestEqual(TEXT("one ordered fence per strict/lenient batch"),BytesFences,2);
   Test.TestEqual(TEXT("no unexpected byte-fixture socket errors"),Errors,0);
   Test.TestEqual(TEXT("byte tests use one actual fake connection"),Fake->NumAttempts(),1);
   Test.TestTrue(TEXT("decode mismatches leave the namespace live"),Socket.Namespace(TEXT("/room")).IsConnected());
   Cleanup(); return true;
  }
  return false;
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
  if (bTypedBytes) return UpdateTypedBytes();
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
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioNativeBytesTest,"VaRest.SocketIO.Features.NativeBytes",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioNativeBytesTest::RunTest(const FString& Parameters)
{
 const auto State=MakeShared<VaRestSioNativeSessionTest::FCase,ESPMode::ThreadSafe>(*this,false,true);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioNativeSessionTest::FAdvance(State));
 return true;
}

namespace VaRestSioRefreshCycleTest
{
/** Owns only copied public snapshot metadata; event callbacks never touch it. */
class FConnectedSnapshot final : public IVaRestTraceSink
{
public:
 FConnectedSnapshot(uint64 InScope,FName InName):Scope(InScope),Name(InName) {}
 virtual FVaRestTraceDetailSettings GetDetail() const override { FVaRestTraceDetailSettings Out; Out.Flags=EVaRestTraceDetail::Metadata; return Out; }
 virtual void OnAttached(const FVaRestTraceSnapshot& Snapshot) override
 {
  check(IsInGameThread()); uint64 Id=0;
  for (const auto& Row:Snapshot.Sockets) if (Row.ScopeId==Scope && Row.Name==Name) { Id=Row.SocketId; ++Sockets; }
  if (!Id || Sockets!=1) return;
  for (const auto& Row:Snapshot.SocketNotes) if (Row.SocketId==Id && Row.Kind==EVaRestTraceSocketNoteKind::Channel && Row.Channel==TEXT("/room"))
  { Current=Row; ++Notes; }
 }
 virtual void OnEvent(const FVaRestTraceEvent&) override {}
 int32 Sockets=0,Notes=0;
 FVaRestTraceSocketNote Current;
private:
 const uint64 Scope;
 const FName Name;
};
/** A real public auth provider. Only the core can supply its refresh epoch and completion. */
class FRefreshAuth final : public IVaRestAuth
{
public:
 virtual FName GetAuthName() const override { return TEXT("SioRefreshCycle"); }
 virtual bool Apply(FVaRestAuthTarget& Target) const override
 {
  check(IsInGameThread());
  if (!Target.Session.HasAccessToken()) return false;
  Target.Headers.Set(TEXT("Authorization"),FString(TEXT("Bearer "))+Target.Session.AccessToken); return true;
 }
 virtual bool CanRefresh(const FVaRestSessionData& Session) const override { return Session.HasRefreshToken(); }
 // No proactive refresh: every observed Refresh below must originate in an actual namespace refusal.
 virtual void Refresh(const FVaRestAuthRefreshContext& Context,TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
 {
  check(IsInGameThread()); bOverlapped|=static_cast<bool>(Pending); ++Calls;
  PreviousId=LastId; LastId=Context.GetRefreshId(); bCurrent=Context.IsCurrentOnGT();
  LastToken=Context.GetSession().RefreshToken; LastGeneration=Context.GetSession().Generation;
  Pending=MoveTemp(Done);
 }
 bool IsPending() const { check(IsInGameThread()); return static_cast<bool>(Pending); }
 bool Answer(FString Token)
 {
  check(IsInGameThread()); if (!Pending) return false;
  auto Done=MoveTemp(Pending); Pending.Reset(); Done(FVaRestAuthRefreshResult::Refreshed(MoveTemp(Token))); return true;
 }
 void RetireOnGT() { check(IsInGameThread()); Pending.Reset(); }
 int32 Calls=0;
 uint64 LastId=0,PreviousId=0;
 uint32 LastGeneration=0;
 bool bCurrent=false,bOverlapped=false;
 FString LastToken;
private:
 TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Pending;
};
struct FCase : TSharedFromThis<FCase,ESPMode::ThreadSafe>
{
 FCase(FAutomationTestBase& InTest,EVaRestSioAuthPlacement InMode):Test(InTest),Mode(InMode) {}
 ~FCase() { Cleanup(); }
 FAutomationTestBase& Test;
 const EVaRestSioAuthPlacement Mode;
 TStrongObjectPtr<UVaRestHub> Core{NewObject<UVaRestHub>()};
 TStrongObjectPtr<UVaRestSocketHub> Hub{NewObject<UVaRestSocketHub>()};
 TStrongObjectPtr<UVaRestApiClient> ClientView;
 FVaRestClient Client;
 FVaRestSioSocket Socket;
 TSharedRef<FVaRestFakeSocketTransport,ESPMode::ThreadSafe> Fake=MakeShared<FVaRestFakeSocketTransport,ESPMode::ThreadSafe>();
 TSharedRef<FVaRestVirtualClock,ESPMode::ThreadSafe> Clock=MakeShared<FVaRestVirtualClock,ESPMode::ThreadSafe>();
 TSharedRef<FRefreshAuth,ESPMode::ThreadSafe> Auth=MakeShared<FRefreshAuth,ESPMode::ThreadSafe>();
 TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe> WrongThread=MakeShared<std::atomic<bool>,ESPMode::ThreadSafe>(false);
 FDelegateHandle Changed,Expired;
 FGuid Identity;
 uint32 InitialGeneration=0,FirstGeneration=0;
 int32 Phase=0,Connected=0,Refreshed=0,ExpiredCount=0,Terminal=0,ServerErrors=0,DropErrors=0,Retries=0,Disconnects=0,Errors=0;
 int32 LiveAttempt=0;
 double Deadline=0;
 bool Cleaned=false,bExpectDrop=false,bExpectTerminal=false;
 static TSharedPtr<FCase,ESPMode::ThreadSafe> Pin(const TWeakPtr<FCase,ESPMode::ThreadSafe>& Weak,const TSharedRef<std::atomic<bool>,ESPMode::ThreadSafe>& Wrong)
 {
  if (!IsInGameThread()) { Wrong->store(true,std::memory_order_release); return {}; }
  auto Value=Weak.Pin(); return Value && !Value->Cleaned ? Value : TSharedPtr<FCase,ESPMode::ThreadSafe>();
 }
 bool Begin()
 {
  FString Error;
  if (!Test.TestTrue(TEXT("auth-cycle owned core Hub initializes"),Core->InitializeOnGT(nullptr,true,Error)) ||
   !Test.TestTrue(TEXT("auth-cycle real socket Hub initializes"),Hub->InitializeOnGT(Core.Get())) ||
   !Test.TestTrue(TEXT("auth-cycle dependencies installed before socket publication"),Hub->ConfigureTestingOnGT(Fake,Clock,777))) return false;
  if (!Test.TestTrue(TEXT("auth-cycle owns a unique profile before managed admission"),Core->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return false;
  ClientView.Reset(Core->MakeClient(TEXT("https://sio-refresh.invalid")));
  if (!Test.TestNotNull(TEXT("actual managed public session client"),ClientView.Get())) return false;
  Client=ClientView->GetNativeClient();
  Client.WithClock(Clock).WithAuth(Auth); Client.SetSession(TEXT("cycle-access-initial-12345"),TEXT("cycle-refresh-kept-12345"));
  const auto Session=Client.GetSession(); Identity=Session.IdentityId; InitialGeneration=Session.Generation;
  TWeakPtr<FCase,ESPMode::ThreadSafe> Weak=AsShared(); const auto Wrong=WrongThread;
  Changed=Client.OnSessionChanged().AddLambda([Weak,Wrong](EVaRestSessionChange Change)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    S->Test.TestEqual(TEXT("provider refresh preserves session change kind"),Change,EVaRestSessionChange::Refreshed); ++S->Refreshed;
   }
  });
  Expired=Client.OnSessionExpired().AddLambda([Weak,Wrong]() { if (auto S=Pin(Weak,Wrong)) ++S->ExpiredCount; });
  Fake->OnConnect=[](FVaRestFakeSocketAttempt& Attempt)
  { Attempt.Open(); Attempt.Text(TEXT("0{\"sid\":\"refresh-engine-private\",\"upgrades\":[],\"pingInterval\":60000,\"pingTimeout\":20000,\"maxPayload\":1000000}")); };
  FVaRestReconnectPolicy Policy; Policy.BaseDelay=.1; Policy.MaxDelay=.1; Policy.MaxAttempts=3;
  Policy.Jitter=EVaRestSocketJitter::Ratio; Policy.JitterRatio=0;
  const auto Placement=Mode==EVaRestSioAuthPlacement::Payload ? FVaRestSioAuthPlacement::Payload(TEXT("access"))
   : Mode==EVaRestSioAuthPlacement::Header ? FVaRestSioAuthPlacement::Header() : FVaRestSioAuthPlacement::Query(TEXT("access"));
  Socket=FVaRestSio::Make(Hub.Get(),TEXT("AuthCycle"),TEXT("ws://sio-refresh.invalid/room"))
   .WithClient(Client).WithAuthPlacement(Placement).WithReconnect(Policy).WithLogLevel(EVaRestLogLevel::None);
  if (!Test.TestTrue(TEXT("auth-cycle public socket identity"),Socket.IsValid())) return false;
  Socket.OnConnect([Weak,Wrong](const FVaRestSioConnect& Info)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Connected; S->Test.TestEqual(TEXT("only the first cycle receives a namespace CONNECT success"),S->Connected,1);
    S->Test.TestEqual(TEXT("real successful CONNECT namespace"),Info.Namespace,FString(TEXT("/room")));
   }
  }).OnConnectError(nullptr,[Weak,Wrong](const FVaRestError& Failure,const FVaRestSioConnectError& Info)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Terminal;
    S->Test.TestTrue(TEXT("only repeated refusal before success becomes terminal"),S->bExpectTerminal && !Info.bWillRetry && Failure.Kind==EVaRestErrorKind::Server);
    S->Test.TestEqual(TEXT("terminal refusal retains original server data"),Info.Data.Get(TEXT("code")).AsInt(),401);
   }
  }).OnDisconnect(nullptr,[Weak,Wrong](const FVaRestSioDisconnect& Info)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    ++S->Disconnects;
    S->Test.TestTrue(TEXT("actual successful cycle loses its transport and remains wanted"),Info.bWillReconnect && Info.Reason==EVaRestSioDisconnectReason::TransportClose);
   }
  }).OnRetryScheduled(nullptr,[Weak,Wrong](const FVaRestSocketStatus& Status,const FVaRestSocketClose& Close)
  {
   if (auto S=Pin(Weak,Wrong))
   { ++S->Retries; S->Test.TestTrue(TEXT("owned transport loss uses genuine backoff"),Close.bWillReconnect && Status.WaitReason==EVaRestSocketWait::Backoff); }
  }).OnError([Weak,Wrong](const FVaRestError& Failure)
  {
   if (auto S=Pin(Weak,Wrong))
   {
    if (S->bExpectDrop && Failure.Kind==EVaRestErrorKind::Connection) ++S->DropErrors;
    else if (S->bExpectTerminal && Failure.Kind==EVaRestErrorKind::Server) ++S->ServerErrors;
    else { ++S->Errors; S->Test.AddError(Failure.Message); }
   }
  });
  Socket.Connect(); Deadline=FPlatformTime::Seconds()+25; return true;
 }
 void CheckConnectedTrace()
 {
  const auto Sink=MakeShared<FConnectedSnapshot,ESPMode::ThreadSafe>(Core->GetScopeId(),Socket.GetName());
  FVaRestTrace::AddSink(Sink); FVaRestTrace::RemoveSink(Sink);
  Test.TestEqual(TEXT("actual snapshot identifies this owned live socket"),Sink->Sockets,1);
  if (!Test.TestEqual(TEXT("actual snapshot contains this namespace once"),Sink->Notes,1)) return;
  Test.TestEqual(TEXT("successful namespace snapshot is Up"),Sink->Current.ChannelState,EVaRestTraceSocketChannelState::Up);
  Test.TestEqual(TEXT("successful namespace snapshot has current Connected reason"),Sink->Current.Reason,FName(TEXT("Connected")));
  Test.TestEqual(TEXT("successful CONNECT clears completed refusal error kind"),Sink->Current.ErrorKind,EVaRestErrorKind::None);
  Test.TestTrue(TEXT("successful CONNECT clears completed refusal message"),Sink->Current.Message.IsEmpty());
 }
 bool ObserveConnect(int32 AttemptIndex,int32 Count,FStringView Token)
 {
  if (Fake->NumAttempts()<=AttemptIndex) return false;
  const auto& Attempt=Fake->GetAttempt(AttemptIndex); int32 Found=0; FString Packet;
  for (const auto& Frame:Attempt.GetSent())
  {
   const auto Wire=VaRestSioNativeSessionTest::Text(Frame);
   if (Wire.StartsWith(TEXT("40/room,"))) { ++Found; Packet=Wire; }
  }
  if (Found<Count) return false;
  Test.TestEqual(TEXT("exact CONNECT count for this owned attempt"),Found,Count);
  const auto& Request=Attempt.GetRequest(); const FString Credential=FString(Token);
  const bool Header=Request.Headers.ContainsByPredicate([&Credential](const auto& Item)
  { return Item.Key.Equals(TEXT("Authorization"),ESearchCase::IgnoreCase) && Item.Value==FString(TEXT("Bearer "))+Credential; });
  const bool AnyHeader=Request.Headers.ContainsByPredicate([](const auto& Item) { return Item.Key.Equals(TEXT("Authorization"),ESearchCase::IgnoreCase); });
  if (Mode==EVaRestSioAuthPlacement::Payload)
  {
   auto Payload=FVaRestJson::Parse(FStringView(Packet).Mid(8));
   Test.TestTrue(TEXT("real payload CONNECT carries the current refreshed token"),Payload.HasValue() && Payload.GetValue().Get(TEXT("access")).AsString()==Token);
   Test.TestFalse(TEXT("payload placement keeps credentials out of upgrade"),AnyHeader || Request.Url.Contains(Credential));
  }
  else
  {
   Test.TestEqual(TEXT("upgrade-auth CONNECT has no synthetic payload"),Packet,FString(TEXT("40/room,")));
   Test.TestTrue(TEXT("new upgrade carries the actual current credential in its chosen place"),Mode==EVaRestSioAuthPlacement::Header
    ? Header && !Request.Url.Contains(Credential) : !AnyHeader && Request.Url.Contains(FString(TEXT("access="))+Credential));
  }
  return true;
 }
 void Refuse() { Fake->GetLast().Text(TEXT("44/room,{\"message\":\"token refused by fixture\",\"data\":{\"code\":401}}")); }
 bool AnswerRefresh(int32 ExpectedCalls,FString Token,uint32 ExpectedGeneration)
 {
  if (!Auth->IsPending()) return false;
  Test.TestEqual(TEXT("one provider refresh for this refused connect cycle"),Auth->Calls,ExpectedCalls);
  Test.TestTrue(TEXT("core supplied a fresh current non-overlapping native refresh epoch"),Auth->bCurrent && !Auth->bOverlapped && Auth->LastId!=0 && Auth->LastId!=Auth->PreviousId);
  Test.TestEqual(TEXT("refresh reads the actual unchanged refresh credential"),Auth->LastToken,FString(TEXT("cycle-refresh-kept-12345")));
  Test.TestEqual(TEXT("refresh context belongs to the expected real session generation"),Auth->LastGeneration,ExpectedGeneration);
  Test.TestTrue(TEXT("actual pending provider Done is invoked exactly once"),Auth->Answer(MoveTemp(Token))); return true;
 }
 bool Update()
 {
  // Unexpected terminal/error callbacks already recorded a failure; retire this owned graph promptly.
  if (Errors || (Terminal && !bExpectTerminal)) { Cleanup(); return true; }
  if (FPlatformTime::Seconds()>Deadline || Auth->Calls>2)
  {
   Test.AddError(FString::Printf(TEXT("auth cycle mode=%d phase=%d attempts=%d refresh=%d connected=%d terminal=%d server=%d drop=%d retry=%d"),
    int32(Mode),Phase,Fake->NumAttempts(),Auth->Calls,Connected,Terminal,ServerErrors,DropErrors,Retries)); Cleanup(); return true;
  }
  const bool Upgrade=Mode!=EVaRestSioAuthPlacement::Payload;
  if (Phase==0)
  {
   if (!ObserveConnect(0,1,TEXTVIEW("cycle-access-initial-12345"))) return false;
   Test.TestEqual(TEXT("no proactive provider call before first real refusal"),Auth->Calls,0); Refuse(); ++Phase;
  }
  else if (Phase==1)
  { if (!AnswerRefresh(1,TEXT("cycle-access-first-12345"),InitialGeneration)) return false; ++Phase; }
  else if (Phase==2)
  {
   LiveAttempt=Upgrade ? 1 : 0;
   if (!ObserveConnect(LiveAttempt,Upgrade ? 1 : 2,TEXTVIEW("cycle-access-first-12345"))) return false;
   Test.TestEqual(TEXT("first refresh was actually published to the session"),Refreshed,1);
   const auto Session=Client.GetSession(); FirstGeneration=Session.Generation;
   Test.TestTrue(TEXT("first refresh preserves identity and advances real session generation"),Session.IdentityId==Identity && FirstGeneration>InitialGeneration && Session.AccessToken==TEXT("cycle-access-first-12345"));
   Test.TestEqual(TEXT("refreshed retry is not yet namespace readiness"),Connected,0);
   if (Upgrade) Test.TestTrue(TEXT("refresh-induced upgrade actually releases its predecessor"),Fake->GetAttempt(0).WasReleased());
   Fake->GetLast().Text(TEXT("40/room,{\"sid\":\"first-cycle-success\"}")); ++Phase;
  }
  else if (Phase==3)
  {
   if (Connected!=1 || !Socket.Namespace(TEXT("/room")).IsConnected()) return false;
   CheckConnectedTrace();
   bExpectDrop=true; Fake->GetLast().Drop(); ++Phase;
  }
  else if (Phase==4)
  {
   if (!DropErrors || !Retries || !Disconnects || !Fake->GetAttempt(LiveAttempt).WasReleased()) return false;
   Test.TestEqual(TEXT("only one deliberate connected transport loss"),Disconnects,1);
   Clock->Advance(.2); ++LiveAttempt; ++Phase;
  }
  else if (Phase==5)
  {
   if (!ObserveConnect(LiveAttempt,1,TEXTVIEW("cycle-access-first-12345"))) return false;
   Test.TestEqual(TEXT("new cycle still waits for namespace CONNECT success"),Connected,1);
   Test.TestEqual(TEXT("reconnect does not itself spend another refresh"),Auth->Calls,1); Refuse(); ++Phase;
  }
  else if (Phase==6)
  { if (!AnswerRefresh(2,TEXT("cycle-access-second-12345"),FirstGeneration)) return false; ++Phase; }
  else if (Phase==7)
  {
   const int32 RetryAttempt=LiveAttempt+(Upgrade ? 1 : 0);
   if (!ObserveConnect(RetryAttempt,Upgrade ? 1 : 2,TEXTVIEW("cycle-access-second-12345"))) return false;
   Test.TestEqual(TEXT("second independent cycle publishes its own refresh"),Refreshed,2);
   const auto Session=Client.GetSession();
   Test.TestTrue(TEXT("second refresh preserves identity and advances actual generation"),Session.IdentityId==Identity && Session.Generation>FirstGeneration && Session.AccessToken==TEXT("cycle-access-second-12345"));
   if (Upgrade) Test.TestTrue(TEXT("second refresh-induced upgrade releases the previous attempt"),Fake->GetAttempt(LiveAttempt).WasReleased());
   LiveAttempt=RetryAttempt; bExpectTerminal=true; Refuse(); ++Phase;
  }
  else
  {
   if (!Terminal || !ServerErrors || Socket.GetState()!=EVaRestSocketState::Disconnected || !Fake->GetAttempt(LiveAttempt).WasReleased()) return false;
   Test.TestEqual(TEXT("repeated refusal before CONNECT success is terminal once"),Terminal,1);
   Test.TestEqual(TEXT("no third refresh or refresh-induced restart loop"),Auth->Calls,2);
   Test.TestFalse(TEXT("no provider completion is left pending"),Auth->IsPending());
   Test.TestEqual(TEXT("header/query restart count is bounded by actual two refreshes"),Fake->NumAttempts(),Upgrade ? 4 : 2);
   Test.TestEqual(TEXT("only the owned loss was a backoff retry"),Retries,1);
   Test.TestEqual(TEXT("only the owned loss reports a connection error"),DropErrors,1);
   Test.TestEqual(TEXT("no spurious namespace CONNECT success"),Connected,1);
   Test.TestEqual(TEXT("auth refusal after successful refresh does not expire the session"),ExpiredCount,0);
   Test.TestTrue(TEXT("server refusal preserves real identity and refresh credentials"),Client.GetSession().IdentityId==Identity && Client.GetSession().HasRefreshToken());
   Test.TestEqual(TEXT("no unexpected socket errors"),Errors,0);
   Test.TestEqual(TEXT("terminal namespace is no longer wanted/connected"),Socket.Namespace(TEXT("/room")).GetState(),EVaRestSioNamespaceState::Disconnected);
   Cleanup(); return true;
  }
  return false;
 }
 void Cleanup()
 {
  if (Cleaned) return; Cleaned=true;
  if (Client.IsValid()) { Client.OnSessionChanged().Remove(Changed); Client.OnSessionExpired().Remove(Expired); }
  if (Hub) Hub->ShutdownOnGT();
  if (Core) Core->DeinitializeOnGT();
  Auth->RetireOnGT(); Socket={}; Client={}; ClientView.Reset();
  Test.TestFalse(TEXT("auth-cycle native callbacks remain on GT"),WrongThread->load(std::memory_order_acquire));
  Test.TestEqual(TEXT("auth-cycle owned timers retire before clock destruction"),Clock->NumPendingTimers(),0);
  for (int32 I=0;I<Fake->NumAttempts();++I) Test.TestTrue(TEXT("all auth-cycle fake connections actually release"),Fake->GetAttempt(I).WasReleased());
 }
};
class FAdvance final : public IAutomationLatentCommand
{
public:
 explicit FAdvance(TSharedRef<FCase,ESPMode::ThreadSafe> In):State(MoveTemp(In)) {}
 bool Update() override { return State->Update(); }
private:
 TSharedRef<FCase,ESPMode::ThreadSafe> State;
};
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioAuthPayloadCycleTest,"VaRest.SocketIO.Features.NativeAuthCycle.Payload",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioAuthPayloadCycleTest::RunTest(const FString& Parameters)
{
 const auto State=MakeShared<VaRestSioRefreshCycleTest::FCase,ESPMode::ThreadSafe>(*this,EVaRestSioAuthPlacement::Payload);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioRefreshCycleTest::FAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioAuthHeaderCycleTest,"VaRest.SocketIO.Features.NativeAuthCycle.Header",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioAuthHeaderCycleTest::RunTest(const FString& Parameters)
{
 const auto State=MakeShared<VaRestSioRefreshCycleTest::FCase,ESPMode::ThreadSafe>(*this,EVaRestSioAuthPlacement::Header);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioRefreshCycleTest::FAdvance(State)); return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestSioAuthQueryCycleTest,"VaRest.SocketIO.Features.NativeAuthCycle.Query",
 EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FVaRestSioAuthQueryCycleTest::RunTest(const FString& Parameters)
{
 const auto State=MakeShared<VaRestSioRefreshCycleTest::FCase,ESPMode::ThreadSafe>(*this,EVaRestSioAuthPlacement::Query);
 if (!State->Begin()) return false;
 ADD_LATENT_AUTOMATION_COMMAND(VaRestSioRefreshCycleTest::FAdvance(State)); return true;
}
#endif
