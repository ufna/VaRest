// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
// Opt-in real official Node peer tests. No fake transport, clock or protocol.
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "VaRestSioReferenceTestTypes.h"
#include "VaRestSioSocket.h"
#include "VaRestSocketHub.h"
#include "VaRestHub.h"
#include "VaRestApiClient.h"
#include "VaRestAuth.h"
#include "Algo/AllOf.h"
#include "Async/Async.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "UObject/StrongObjectPtr.h"

namespace VaRestSioReferenceTest
{
enum class ECase { Recovery, ServerAcks, Auth, Payload, Namespaces, Binary, Heartbeat, UnsupportedV2 };
const TArray<uint8> Bytes{0,255,0,128,1};
bool Admitted(EVaRestSendResult R) { return R==EVaRestSendResult::Sent || R==EVaRestSendResult::Queued; }

struct FCase : TSharedFromThis<FCase,ESPMode::ThreadSafe>
{
	FAutomationTestBase& Test;
	const ECase Kind;
	TStrongObjectPtr<UVaRestHub> Core{NewObject<UVaRestHub>()};
	TStrongObjectPtr<UVaRestSocketHub> Hub{NewObject<UVaRestSocketHub>()};
	TStrongObjectPtr<UVaRestApiClient> ClientView;
	FVaRestClient Client;
	FVaRestSioSocket Socket, Other;
	FVaRestSioNamespace Admin, Slow;
	FVaRestSioAck Late;
	TFuture<EVaRestSendResult> Worker;
	TArray<FVaRestSioAckHandle> Requests;
	FString Version,LastConfigMessage;
	int32 Port=0,Phase=0,Connected=0,AdminConnected=0,AdminDisconnected=0,SlowErrors=0;
	int32 Errors=0,ConfigErrors=0,TerminalServer=0,Refused=0,Refreshed=0,PlainReports=0,BinaryReports=0,TimeoutReports=0,StaleReports=0;
	int32 BinaryEvents=0,BinaryAcks=0,NestedAcks=0,SizedAcks=0,LargeEvents=0,EchoAcks=0;
	int32 RootConnection=0,AdminConnection=0,Reports=0,WrongAttempts=0;
	TArray<int32> Counters;
	bool Recovered=false,BinaryOffset=false,Cleaned=false,PendingControl=false,WorkerChecked=false;
	bool IsStalling=false,UploadDone=false;
	int64 PingsBefore=0,StallPongs=0,UploadPongs=0,UploadPauses=0;
	double Deadline=0,ObserveUntil=0,UploadElapsed=0;
	FDelegateHandle SessionChanged;
	explicit FCase(FAutomationTestBase& In,ECase InKind):Test(In),Kind(InKind) {}
	~FCase() { Cleanup(); }
	TWeakPtr<FCase,ESPMode::ThreadSafe> Weak() { return AsShared(); }
	FString Url(int32 Delta=0,FStringView Namespace=TEXTVIEW("/")) const
	{ return FString::Printf(TEXT("ws://127.0.0.1:%d"),Port+Delta)+FString(Namespace); }
	void Check(bool B,const TCHAR* What) { Test.TestTrue(What,B); }
	void Track(FVaRestSioAckHandle H) { Requests.Add(MoveTemp(H)); }
	void Control(FStringView Op,FVaRestSioNamespace NS,TFunction<void(const FVaRestSioEvent&)> Done={})
	{
		const auto W=Weak();
		Track(NS.EmitWithAck(TEXT("__test"),FVaRestJson::Object().Set(TEXT("op"),Op)).Timeout(4)
			.OnSuccess(TFunction<void(const FVaRestSioEvent&)>([W,Done=MoveTemp(Done)](const FVaRestSioEvent& E)
			{ if (auto S=W.Pin();S && !S->Cleaned) { if (Done) Done(E); } }))
			.OnError([W](const FVaRestError&) { if (auto S=W.Pin();S && !S->Cleaned) S->Test.AddError(TEXT("real reference control ACK failed")); }).Send());
	}
	void Echo(FVaRestSioNamespace NS)
	{
		const auto W=Weak();
		Track(NS.EmitWithAck(TEXT("echo"),TEXT("varest-public-sio")).Timeout(4)
			.OnSuccess([W](const FVaRestSioEvent& E) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(E.Arg<FString>(0)==TEXT("varest-public-sio"),TEXT("real peer echo ACK bytes"));++S->EchoAcks; } })
			.OnError([W](const FVaRestError&) { if (auto S=W.Pin();S && !S->Cleaned) S->Test.AddError(TEXT("real reference echo ACK failed")); }).Send());
	}
	bool Begin()
	{
		FString Endpoint;
		if(FParse::Value(FCommandLine::Get(),TEXT("VaRestSioInterop="),Endpoint) && Endpoint.StartsWith(TEXT("ws://127.0.0.1:"),ESearchCase::CaseSensitive))
		{
			const FString Digits=Endpoint.Mid(15);bool Valid=Digits.Len()>=4 && Digits.Len()<=5;
			if(Valid) for(const TCHAR C:Digits) { if(C<TEXT('0') || C>TEXT('9')) {Valid=false;break;}Port=Port*10+int32(C-TEXT('0')); }
			if(!Valid)Port=0;
		}
		if (Port<1024 || Port>65532 ||
			!FParse::Value(FCommandLine::Get(),TEXT("SioReferenceVersion="),Version))
		{ Test.AddError(TEXT("real reference tests require explicit VaRestSioInterop=ws://127.0.0.1:PORT and SioReferenceVersion; never pass/skip without a server"));return false; }
		const bool Supported=Kind==ECase::UnsupportedV2 ? Version==TEXT("2") :
			Kind==ECase::Recovery ? (Version==TEXT("4.8") || Version==TEXT("4.5")) :
			Kind==ECase::Payload ? (Version==TEXT("4.8") || Version==TEXT("4.4")) : Version==TEXT("4.8");
		if (!Supported) { Test.AddError(TEXT("reference version/case is outside its explicit contract matrix"));return false; }
		FString Error;
		if (!Test.TestTrue(TEXT("owned real Core Hub initializes"),Core->InitializeOnGT(nullptr,true,Error)) ||
			!Test.TestTrue(TEXT("owned real Socket Hub initializes"),Hub->InitializeOnGT(Core.Get()))) return false;
		Deadline=FPlatformTime::Seconds()+45;
		Socket=FVaRestSio::Make(Hub.Get(),TEXT("Reference"),Url(Kind==ECase::Heartbeat ? 2:0)).NoReconnect().WithLogLevel(EVaRestLogLevel::None);
		if (!Test.TestTrue(TEXT("public real-default-transport factory"),Socket.IsValid())) return false;
		FVaRestSioLimits Limits;Limits.ReadyTimeout=1.5;
		if (Kind==ECase::Heartbeat) { Limits.MaxMessageBytes=20*1024*1024;Limits.MaxPacketBytes=20*1024*1024; }
		Socket.WithLimits(Limits);
		const auto W=Weak();
		Socket.OnConnect([W](const FVaRestSioConnect& C) { if (auto S=W.Pin();S && !S->Cleaned) { ++S->Connected; S->Recovered=C.bRecovered; } });
		Socket.OnError([W](const FVaRestError& E)
		{
			if (auto S=W.Pin();S && !S->Cleaned)
			{
				if (E.Kind==EVaRestErrorKind::Config) { ++S->ConfigErrors;S->LastConfigMessage=E.Message; }
				else if (E.Kind==EVaRestErrorKind::Server) ++S->TerminalServer;
				else if (!((S->Kind==ECase::Recovery && E.Kind==EVaRestErrorKind::Connection) ||
					(S->Kind==ECase::Namespaces && E.Kind==EVaRestErrorKind::Timeout)))
				{ ++S->Errors;S->Test.AddError(FString::Printf(TEXT("unexpected real-peer socket error Kind=%d"),int32(E.Kind))); }
			}
		});
		if (Kind==ECase::Recovery) SetupRecovery();
		if (Kind==ECase::ServerAcks) SetupAcks();
		if (Kind==ECase::Auth && !SetupAuth()) return false;
		if (Kind==ECase::Namespaces) SetupNamespaces();
		if (Kind==ECase::Binary) SetupBinary();
		if (Kind==ECase::Payload) Socket.On(TEXT("large-inbound"),[W](const FVaRestSioEvent& E)
		{ if (auto S=W.Pin();S && !S->Cleaned) { const auto B=E.TryArg<TArray<uint8>>(0); S->Check(B.IsSet() && B->Num()==2*1024*1024 && Algo::AllOf(B.GetValue(),[](uint8 V){return V==0x3c;}),TEXT("real two-MiB inbound binary exact bytes"));++S->LargeEvents; } });
		if (Kind==ECase::Payload) Socket.On(TEXT("report-sized"),[W](const FVaRestSioEvent& E)
		{ if (auto S=W.Pin();S && !S->Cleaned) {S->Check(E.Arg<FVaRestJson>(0).Get(TEXT("wire_bytes")).AsInt64()==900000,TEXT("official peer measured exact900000-byte Engine.IO text message"));++S->SizedAcks;} });
		if (Kind==ECase::Heartbeat) Socket.On(TEXT("report-stall"),[W](const FVaRestSioEvent& E) { if (auto S=W.Pin();S && !S->Cleaned) S->StallPongs=E.Arg<int64>(0); });
		Socket.Connect();return true;
	}
	void SetupRecovery()
	{
		FVaRestReconnectPolicy P;P.MaxAttempts=3;P.BaseDelay=.3;P.MaxDelay=.3;P.Jitter=EVaRestSocketJitter::Ratio;P.JitterRatio=0;
		Socket.WithReconnect(P).WithRecovery(true);
		const auto W=Weak();
		Socket.On(TEXT("counter"),[W](const FVaRestSioEvent& E) { if (auto S=W.Pin();S && !S->Cleaned)
		{ const int32 V=E.Arg<int32>(0);if (V>=3) S->Check(S->Connected==2,TEXT("missed events follow the new OnConnect"));S->Counters.Add(V); } });
		Socket.On(TEXT("counter-binary"),[W](const FVaRestSioEvent& E) { if (auto S=W.Pin();S && !S->Cleaned)
		{ S->Check(E.Arg<TArray<uint8>>(1)==Bytes,TEXT("binary recovery event retains attachment bytes"));S->Counters.Add(E.Arg<int32>(0));S->BinaryOffset=!E.Offset.IsEmpty(); } });
	}
	void SetupAcks()
	{
		const auto W=Weak();
		Socket.On(TEXT("server-plain"),[W](int32 V,FVaRestSioAck& Ack) { if (auto S=W.Pin();S && !S->Cleaned)
		{ S->Check(V==7,TEXT("typed server request integer"));S->Check(Admitted(Ack.Send(TEXT("typed-ok"))),TEXT("typed trailing ACK sent"));S->Check(Ack.Send(TEXT("twice"))==EVaRestSendResult::Rejected,TEXT("original ACK once flag survives a second send")); } });
		Socket.On(TEXT("server-binary"),[W](const TArray<uint8>& B,FVaRestSioAck& Ack) { if (auto S=W.Pin();S && !S->Cleaned)
		{ S->Check(B==Bytes,TEXT("typed server binary request bytes"));const auto Copy=Ack;S->Worker=Async(EAsyncExecution::Thread,[Copy] { FPlatformProcess::Sleep(.03f);return Copy.Send(Bytes); }); } });
		Socket.On(TEXT("server-no-reply"),[W](int32 V,FVaRestSioAck&) { if (auto S=W.Pin();S && !S->Cleaned) S->Check(V==9,TEXT("deliberately unanswered server ACK request")); });
		Socket.On(TEXT("report-plain"),[W](bool Good) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(Good,TEXT("official peer accepted plain ACK"));++S->PlainReports; } });
		Socket.On(TEXT("report-binary"),[W](bool Good) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(Good,TEXT("official peer accepted later worker binary ACK"));++S->BinaryReports; } });
		Socket.On(TEXT("report-timeout"),[W](bool Good) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(Good,TEXT("official peer observed unanswered ACK timeout"));++S->TimeoutReports; } });
		Socket.On(TEXT("report-stale"),[W](bool Good) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(Good,TEXT("stale namespace ACK never reaches peer"));++S->StaleReports; } });
		Admin=Socket.Namespace(TEXT("/admin"));
		Admin.OnConnect([W](const FVaRestSioConnect&) { if (auto S=W.Pin();S && !S->Cleaned) ++S->AdminConnected; });
		Admin.OnDisconnect(nullptr,[W](const FVaRestSioDisconnect&) { if (auto S=W.Pin();S && !S->Cleaned) ++S->AdminDisconnected; });
		Admin.On(TEXT("server-late"),[W](int32 V,FVaRestSioAck& Ack) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(V==11,TEXT("stale ACK fixture request"));S->Late=Ack; } });
	}
	bool SetupAuth()
	{
		FString Error;if (!Test.TestTrue(TEXT("unique managed auth fixture profile"),Core->SetStorageProfile(FGuid::NewGuid().ToString(EGuidFormats::Digits),Error))) return false;
		ClientView.Reset(Core->MakeClient(FString::Printf(TEXT("http://127.0.0.1:%d"),Port)));
		if (!Test.TestNotNull(TEXT("real managed HTTP refresh client"),ClientView.Get())) return false;
		Client=ClientView->GetNativeClient();FVaRestBearerRefreshConfig Config;Config.RefreshPath=TEXT("/token");
		Client.WithAuth(FVaRestAuth::BearerRefresh(Config)).WithTimeout(4).WithLogLevel(EVaRestLogLevel::None);
		Client.SetSession(TEXT("varest-public-sio-1"),TEXT("varest-public-sio-refresh"));
		const auto W=Weak();SessionChanged=Client.OnSessionChanged().AddLambda([W](EVaRestSessionChange Change)
		{ if (auto S=W.Pin();S && !S->Cleaned && Change==EVaRestSessionChange::Refreshed) ++S->Refreshed; });
		Socket.WithClient(Client).WithAuthPlacement(FVaRestSioAuthPlacement::Payload(TEXT("access"),TEXT("Bearer ")));
		Socket.Namespace(TEXT("/")).WithConnectOnStart(false);
		Admin=Socket.Namespace(TEXT("/auth"));
		Admin.OnConnect([W](const FVaRestSioConnect&) { if (auto S=W.Pin();S && !S->Cleaned) ++S->AdminConnected; });
		Admin.OnConnectError(nullptr,[W](const FVaRestError&,const FVaRestSioConnectError& E) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(E.Data.Get(TEXT("code")).AsInt64()==401,TEXT("actual middleware error code401"));++S->Refused; } });
		return true;
	}
	void SetupNamespaces()
	{
		const auto W=Weak();Admin=Socket.Namespace(TEXT("/admin"));Slow=Socket.Namespace(TEXT("/slow"));
		Admin.OnConnect([W](const FVaRestSioConnect&) { if (auto S=W.Pin();S && !S->Cleaned) ++S->AdminConnected; });
		Admin.OnDisconnect(nullptr,[W](const FVaRestSioDisconnect&) { if (auto S=W.Pin();S && !S->Cleaned) ++S->AdminDisconnected; });
		Slow.OnConnectError(nullptr,[W](const FVaRestError& E,const FVaRestSioConnectError&) { if (auto S=W.Pin();S && !S->Cleaned) { S->Check(E.Kind==EVaRestErrorKind::Timeout,TEXT("unanswered namespace reaches real timeout"));++S->SlowErrors; } });
	}
	void SetupBinary()
	{
		const auto W=Weak();Socket.On(TEXT("binary"),[W](const FVaRestSioEvent& E) { if (auto S=W.Pin();S && !S->Cleaned)
		{ const int32 Expected=S->BinaryEvents==0?1:S->BinaryEvents==1?9:10;S->Check(E.NumAttachments==Expected,TEXT("official event exact attachment count"));for(int32 I=0;I<E.NumAttachments;++I) S->Check(E.Arg<TArray<uint8>>(I)==Bytes,TEXT("official event exact counted binary bytes"));++S->BinaryEvents; } });
	}
	void BinaryAck(const FVaRestSioEvent& E,int32 Count)
	{ Check(E.NumAttachments==Count,TEXT("official BINARY_ACK attachment count"));for(int32 I=0;I<Count;++I) Check(E.Arg<TArray<uint8>>(I)==Bytes,TEXT("official BINARY_ACK byte identity"));++BinaryAcks; }
	void SendBinary(int32 Count)
	{
		const auto W=Weak();auto Done=[W,Count](const FVaRestSioEvent& E) { if (auto S=W.Pin();S && !S->Cleaned) S->BinaryAck(E,Count); };
		auto Fail=[W](const FVaRestError&) { if (auto S=W.Pin();S && !S->Cleaned) S->Test.AddError(TEXT("official binary ACK failed")); };
		if(Count==1) Track(Socket.EmitWithAck(TEXT("bytes"),Bytes).Timeout(4).OnSuccess(Done).OnError(Fail).Send());
		if(Count==9) Track(Socket.EmitWithAck(TEXT("bytes"),Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes).Timeout(4).OnSuccess(Done).OnError(Fail).Send());
		if(Count==10) Track(Socket.EmitWithAck(TEXT("bytes"),Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes).Timeout(4).OnSuccess(Done).OnError(Fail).Send());
	}
	bool Update()
	{
		if(Cleaned) return true;
		if(FPlatformTime::Seconds()>Deadline) { Test.AddError(FString::Printf(TEXT("real Socket.IO reference deadline: case=%d phase=%d connected=%d config=%d errors=%d"),int32(Kind),Phase,Connected,ConfigErrors,Errors));Cleanup();return true; }
		bool Done=false;
		switch(Kind)
		{
		case ECase::Recovery:Done=Recovery();break;
		case ECase::ServerAcks:Done=Acks();break;
		case ECase::Auth:Done=Auth();break;
		case ECase::Payload:Done=Payload();break;
		case ECase::Namespaces:Done=Namespaces();break;
		case ECase::Binary:Done=Binary();break;
		case ECase::Heartbeat:Done=Heartbeat();break;
		case ECase::UnsupportedV2:
			if(ConfigErrors>0 && Socket.GetState()==EVaRestSocketState::Disconnected) { Check(Connected==0,TEXT("Socket.IO2 is never silently accepted"));Check(ConfigErrors==1 && LastConfigMessage.Contains(TEXT("the server speaks Socket.IO v2 (EIO=3)")),TEXT("precise unsupported v2/EIO3 Config diagnostic"));Done=true; } break;
		}
		if(Done)
		{
			Check(Errors==0,TEXT("no unexpected real-peer errors"));
			Check(ConfigErrors==((Kind==ECase::Payload || Kind==ECase::Binary || Kind==ECase::UnsupportedV2)?1:0),TEXT("exact local Config refusals only"));
			if(Kind!=ECase::Auth) Check(TerminalServer==0,TEXT("no unexpected Server errors"));
			Cleanup();
		}return Done;
	}
	bool Recovery()
	{
		if(Phase==0 && Connected==1) { Control(TEXT("drop"),Socket.DefaultNamespace());++Phase; }
		else if(Phase==1 && Connected==2 && Counters.Num()>=(Version==TEXT("4.8")?4:2))
		{
			Check(Recovered==(Version==TEXT("4.8")),TEXT("actual reference version controls recovery"));
			Check(Counters==(Version==TEXT("4.8")?TArray<int32>{1,2,3,4}:TArray<int32>{1,2}),TEXT("missed counters exactly once and in server order"));
			Check(BinaryOffset==(Version==TEXT("4.8")),TEXT("binary event recovery offset exists only with recovery support"));
			Socket.Close();Socket.Connect();++Phase;
		}
		else if(Phase==2 && Connected==3) { Check(!Recovered,TEXT("local Close then Connect starts a new session"));Socket.ResetSession();Socket.ReconnectNow();++Phase; }
		else if(Phase==3 && Connected==4) { Check(!Recovered,TEXT("ResetSession starts a new session"));return true; }return false;
	}
	bool Acks()
	{
		if(Phase==0 && Connected==1 && AdminConnected==1) { Control(TEXT("server-acks"),Socket.DefaultNamespace());Control(TEXT("stale-ack"),Admin);++Phase; }
		if(!WorkerChecked && Worker.IsValid() && Worker.IsReady()) { Check(Admitted(Worker.Get()),TEXT("later worker original ACK admitted"));WorkerChecked=true; }
		if(Phase==1 && AdminDisconnected && Late.IsValid()) { Check(!Late.CanReplyOnGT(),TEXT("namespace disconnect invalidates original ACK pin"));Check(Late.Send(TEXT("late"))==EVaRestSendResult::Rejected,TEXT("stale namespace ACK rejected locally"));++Phase; }
		if(Phase==2 && PlainReports && BinaryReports && TimeoutReports && StaleReports && WorkerChecked)
		{ Check(PlainReports==1 && BinaryReports==1 && TimeoutReports==1 && StaleReports==1,TEXT("each real server ACK outcome occurs once"));Echo(Socket.DefaultNamespace());++Phase; }
		return Phase==3 && EchoAcks==1;
	}
	bool Auth()
	{
		if(Phase==0 && AdminConnected==1) { Check(Refreshed==1 && Refused==1,TEXT("middleware401 caused one real HTTP refresh and one retry"));Echo(Admin);++Phase; }
		else if(Phase==1 && EchoAcks==1)
		{
			Socket.Close();Client.SetSession(TEXT("varest-public-sio-1"),TEXT("varest-public-sio-refresh"));
			Socket.WithQuery(TEXT("mode"),TEXT("always-refuse"));Socket.Connect();++Phase;
		}
		else if(Phase==2 && TerminalServer>0 && Socket.GetState()==EVaRestSocketState::Disconnected)
		{
			Check(Refreshed==2 && Refused==3,TEXT("second cycle refreshes once then repeated middleware refusal is terminal"));
			Other=FVaRestSio::Make(Hub.Get(),TEXT("InvalidNamespace"),Url(0,TEXTVIEW("/not-registered"))).NoReconnect().WithLogLevel(EVaRestLogLevel::None);
			const auto W=Weak();Other.OnConnectError(nullptr,[W](const FVaRestError& E,const FVaRestSioConnectError&) { if(auto S=W.Pin();S && !S->Cleaned) { S->Check(E.Kind==EVaRestErrorKind::Config,TEXT("actual Invalid namespace is Config"));S->PendingControl=true; } });Other.Connect();++Phase;
		}
		return Phase==3 && PendingControl;
	}
	bool Payload()
	{
		const auto W=Weak();
		if(Phase==0 && Connected==1)
		{
			// Without an ACK id: literal '4', '2' and ["sized","..."] total14 ASCII framing bytes.
			Check(Admitted(Socket.Emit(TEXT("sized"),FString::ChrN(899986,TEXT('x')))),TEXT("exact900000-byte frame admitted"));
			Check(Socket.Emit(TEXT("sized"),FString::ChrN(1199986,TEXT('x')))==EVaRestSendResult::Rejected,TEXT("exact1.2MB frame refused locally above negotiated or fallback1MB"));
			Control(TEXT("large-inbound"),Socket.DefaultNamespace());++Phase;
		}
		else if(Phase==1 && SizedAcks==1 && LargeEvents==1 && ConfigErrors==1)
		{
			Check(Socket.DefaultNamespace().IsConnected(),TEXT("oversized local refusal preserves connection"));Echo(Socket.DefaultNamespace());
			Test.AddExpectedError(TEXT("Engine.IO maxPayload="),EAutomationExpectedErrorFlags::Contains,1);
			FVaRestSioLimits HighLimits;HighLimits.bOverrideFallbackPayloadLimit=Version==TEXT("4.4");
			Other=FVaRestSio::Make(Hub.Get(),TEXT("HighPayload"),Url(1)).NoReconnect().WithLimits(HighLimits).WithLogLevel(EVaRestLogLevel::Basic);
			Other.OnConnect([W](const FVaRestSioConnect&){if(auto S=W.Pin();S && !S->Cleaned) {S->PendingControl=true;S->Echo(S->Other.DefaultNamespace());}});Other.Connect();++Phase;
		}
		return Phase==2 && PendingControl && EchoAcks==2;
	}
	bool Namespaces()
	{
		const auto W=Weak();
		if(Phase==0 && Connected==1 && AdminConnected==1 && SlowErrors==1)
		{
			Control(TEXT("report"),Socket.DefaultNamespace(),[W](const FVaRestSioEvent& E){if(auto S=W.Pin();S && !S->Cleaned){S->RootConnection=E.Arg<FVaRestJson>(0).Get(TEXT("connection")).AsInt64();++S->Reports;}});
			Control(TEXT("report"),Admin,[W](const FVaRestSioEvent& E){if(auto S=W.Pin();S && !S->Cleaned){S->AdminConnection=E.Arg<FVaRestJson>(0).Get(TEXT("connection")).AsInt64();++S->Reports;}});++Phase;
		}
		else if(Phase==1 && Reports==2) { Check(RootConnection>0 && RootConnection==AdminConnection,TEXT("root and admin share actual server Engine.IO connection"));Admin.Disconnect();Admin.Connect();++Phase; }
		else if(Phase==2 && AdminConnected==2) { Control(TEXT("disconnect-admin"),Socket.DefaultNamespace());++Phase; }
		else if(Phase==3 && AdminDisconnected==2) { Check(Socket.DefaultNamespace().IsConnected(),TEXT("server namespace disconnect leaves root live"));Echo(Socket.DefaultNamespace());++Phase; }
		return Phase==4 && EchoAcks==1 && Connected==1;
	}
	bool Binary()
	{
		const auto W=Weak();
		if(Phase<3 && Connected==1 && BinaryEvents==Phase && BinaryAcks==Phase) { SendBinary(Phase==0?1:Phase==1?9:10);++Phase; }
		else if(Phase==3 && BinaryEvents==3 && BinaryAcks==3)
		{
			FVaRestSioReferenceOuter Value;Value.Inner.Bytes=Bytes;
			Track(Socket.EmitWithAck(TEXT("nested"),Value).Timeout(4).OnSuccess([W](const FVaRestSioEvent& E){if(auto S=W.Pin();S && !S->Cleaned){const auto V=E.TryArg<FVaRestSioReferenceOuter>(0);S->Check(V.IsSet() && V->Inner.Bytes==Bytes && E.NumAttachments==1,TEXT("nested typed struct actual binary roundtrip"));++S->NestedAcks;}})
				.OnError([W](const FVaRestError&){if(auto S=W.Pin();S && !S->Cleaned) S->Test.AddError(TEXT("nested official binary ACK failed"));}).Send());
			Check(Socket.Emit(TEXT("bytes"),Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes,Bytes)==EVaRestSendResult::Rejected,TEXT("eleven attachments rejected locally"));++Phase;
		}
		else if(Phase==4 && NestedAcks==1 && ConfigErrors==1) { Echo(Socket.DefaultNamespace());++Phase; }
		return Phase==5 && EchoAcks==1;
	}
	bool Heartbeat()
	{
		const auto W=Weak();
		if(Phase==0 && Connected==1)
		{ PingsBefore=Socket.GetStats().PingsAnswered;Control(TEXT("stall-window"),Socket.DefaultNamespace(),[W](const FVaRestSioEvent&){if(auto S=W.Pin();S && !S->Cleaned) S->IsStalling=true;});++Phase; }
		else if(Phase==1 && IsStalling) { IsStalling=false;FPlatformProcess::Sleep(1.05f);++Phase; }
		else if(Phase==2 && StallPongs>=2)
		{ Check(Connected==1 && Socket.GetStats().PingsAnswered-PingsBefore>=2,TEXT("real service answers pings during GT stall without reconnect"));Control(TEXT("throttle-upload"),Socket.DefaultNamespace(),[W](const FVaRestSioEvent& E){if(auto S=W.Pin();S && !S->Cleaned) {S->Check(E.Arg<FVaRestJson>(0).Get(TEXT("ready")).AsBool(),TEXT("peer enabled real TCP read throttling"));S->PendingControl=true;}});++Phase; }
		else if(Phase==3 && PendingControl)
		{
			TArray<uint8> Chunk;Chunk.Init(0x5a,1024*1024);
			Track(Socket.EmitWithAck(TEXT("upload"),Chunk,Chunk,Chunk,Chunk,Chunk,Chunk,Chunk,Chunk).Timeout(10)
				.OnSuccess([W](const FVaRestSioEvent& E){if(auto S=W.Pin();S && !S->Cleaned){const auto R=E.Arg<FVaRestJson>(0);S->Check(R.Get(TEXT("bytes")).AsInt64()==8*1024*1024,TEXT("peer received actual eight-MiB upload"));S->UploadPongs=R.Get(TEXT("pongs")).AsInt64();S->UploadPauses=R.Get(TEXT("pauses")).AsInt64();S->UploadElapsed=R.Get(TEXT("elapsed_ms")).AsDouble();S->UploadDone=true;}})
				.OnError([W](const FVaRestError&){if(auto S=W.Pin();S && !S->Cleaned) S->Test.AddError(TEXT("real throttled upload ACK failed"));}).Send());++Phase;
		}
		else if(Phase==4 && UploadDone)
		{
			Check(UploadPongs>=1 && UploadPauses==8 && UploadElapsed>=600,TEXT("real heartbeat pongs interleave during measured TCP-throttled upload"));
			Check(Connected==1 && Socket.DefaultNamespace().IsConnected(),TEXT("stall/upload preserve physical connection"));
			FVaRestReconnectPolicy P;P.MaxAttempts=3;P.BaseDelay=.1;P.MaxDelay=.1;P.Jitter=EVaRestSocketJitter::Ratio;P.JitterRatio=0;
			Other=FVaRestSio::Make(Hub.Get(),TEXT("WrongEnginePath"),Url()).WithPath(TEXT("/wrong-engine-path/")).WithReconnect(P).WithLogLevel(EVaRestLogLevel::None);
			Other.OnGaveUp(nullptr,[W](const FVaRestError& E,const FVaRestSocketClose& C){if(auto S=W.Pin();S && !S->Cleaned){S->Check(E.Kind==EVaRestErrorKind::Config && C.Attempt==3,TEXT("wrong Engine.IO path ends Config after exactly three real attempts"));S->WrongAttempts=C.Attempt;}});Other.Connect();++Phase;
		}
		return Phase==5 && WrongAttempts==3;
	}
	void Cleanup()
	{
		if(Cleaned)return;Cleaned=true;
		for(auto& H:Requests)H.Cancel();Requests.Reset();
		if(Client.IsValid()){Client.OnSessionChanged().Remove(SessionChanged);Client.CancelAll();}
		if(Hub)Hub->ShutdownOnGT();if(Core)Core->DeinitializeOnGT();
		Socket={};Other={};Admin={};Slow={};Late={};Client={};ClientView.Reset();
	}
};
class FAdvance final : public IAutomationLatentCommand
{
	TSharedRef<FCase,ESPMode::ThreadSafe> State;
public:
	explicit FAdvance(TSharedRef<FCase,ESPMode::ThreadSafe> In):State(MoveTemp(In)){}
	bool Update() override{return State->Update();}
};
}
IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestSioReference,"Interop.VaRest.SocketIO.Reference",EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
void FVaRestSioReference::GetTests(TArray<FString>& Names,TArray<FString>& Commands) const
{
	FString Version;FParse::Value(FCommandLine::Get(),TEXT("SioReferenceVersion="),Version);
	// Enumerate the actual explicit version contract, never a passing skipped case.
	const TArray<FString> Cases=Version==TEXT("4.8") ? TArray<FString>{TEXT("Recovery"),TEXT("ServerAcks"),TEXT("Auth"),TEXT("Payload"),TEXT("Namespaces"),TEXT("Binary"),TEXT("Heartbeat")} :
		Version==TEXT("4.5") ? TArray<FString>{TEXT("Recovery")} : Version==TEXT("4.4") ? TArray<FString>{TEXT("Payload")} :
		Version==TEXT("2") ? TArray<FString>{TEXT("UnsupportedV2")} : TArray<FString>{TEXT("ConfigurationRequired")};
	for(const auto& Case:Cases) { Names.Add(Case);Commands.Add(Case); }
}
bool FVaRestSioReference::RunTest(const FString& Parameters)
{
	using namespace VaRestSioReferenceTest;
	ECase Kind;
	if(Parameters==TEXT("Recovery"))Kind=ECase::Recovery;
	else if(Parameters==TEXT("ServerAcks"))Kind=ECase::ServerAcks;
	else if(Parameters==TEXT("Auth"))Kind=ECase::Auth;
	else if(Parameters==TEXT("Payload"))Kind=ECase::Payload;
	else if(Parameters==TEXT("Namespaces"))Kind=ECase::Namespaces;
	else if(Parameters==TEXT("Binary"))Kind=ECase::Binary;
	else if(Parameters==TEXT("Heartbeat"))Kind=ECase::Heartbeat;
	else if(Parameters==TEXT("UnsupportedV2"))Kind=ECase::UnsupportedV2;
	else {AddError(TEXT("explicit official reference version/configuration required"));return false;}
	const auto State=MakeShared<FCase,ESPMode::ThreadSafe>(*this,Kind);
	if(!State->Begin())return false;ADD_LATENT_AUTOMATION_COMMAND(FAdvance(State));return true;
}
#endif
