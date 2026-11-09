// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS && VAREST_COOKED_COEXIST_TESTS

#include "Engine/Engine.h"
#include "IWebSocket.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreMisc.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "VaRestClient.h"
#include "VaRestHttpTesting.h"
#include "VaRestSocketTesting.h"
#include "WebSocketsModule.h"

namespace VaRest::WebSocket::CookedCoexist
{
	using namespace VaRest::WebSocket::Testing;
	using VaRest::Http::Testing::PumpUntil;

	bool Dedicated()
	{
		return FParse::Param(FCommandLine::Get(), TEXT("VaRestCookedCoexistFixture"))
			&& FParse::Param(FCommandLine::Get(), TEXT("unattended"))
			&& FParse::Param(FCommandLine::Get(), TEXT("nowrite"));
	}

	struct FEngineSide
	{
		bool bOpen = false, bEnded = false, bClean = false;
		int32 CloseCode = 0;
		FString Error;
		TArray<FString> Messages;
	};

	FString LogVerbosity()
	{
		FStringOutputDevice Output;
		Output.SetAutoEmitLineTerminator(true);
		GEngine->Exec(nullptr, TEXT("Log list LogWebSockets"), Output);
		TArray<FString> Lines; Output.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> Tokens; Line.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() >= 2 && Tokens[0].Equals(TEXT("LogWebSockets"), ESearchCase::CaseSensitive)) return Tokens[1];
		}
		return FString();
	}

	bool FetchCa(FAutomationTestBase& Test, const FString& Base, TArray<uint8>& Out)
	{
		using namespace VaRest::Http::Testing;
		FVaRestClient Client = FVaRestClient::Make(Base);
		Client.WithLogLevel(EVaRestLogLevel::None);
		auto Outcome = MakeOutcome<TArray<uint8>>();
		const FVaRestRequestHandle Handle = Observe(Client.Get<TArray<uint8>>(TEXT("/tls/ca.pem")), Outcome).Send();
		if (!PumpUntil([Outcome] { return Outcome->IsDone(); }, 20.0, true))
		{
			Handle.Cancel();
			PumpUntil([Outcome, &Handle] { return Outcome->IsDone() && !Handle.IsActive(); }, 5.0, true);
			Test.AddError(TEXT("The real test peer did not supply its public CA within20s")); return false;
		}
		if (Outcome->SuccessCalls.load() != 1 || !Outcome->Value.IsSet() || Outcome->Value->IsEmpty())
		{
			Test.AddError(TEXT("The real test peer public-CA request failed")); return false;
		}
		Out = Outcome->Value.GetValue();
		return true;
	}

	struct FPrivateSide
	{
		TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection;
		bool bOpen = false, bEcho = false;
		int32 Terminals = 0;
		FVaRestSocketEnd End;
		explicit FPrivateSide(TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> In) : Connection(MoveTemp(In)) {}
		void Pull()
		{
			TArray<FVaRestSocketEvent> Events; Connection->PopEvents(Events);
			const FTCHARToUTF8 Expected(TEXT("varest-game-coexist"));
			for (const FVaRestSocketEvent& Event : Events)
			{
				bOpen |= Event.Type == EVaRestSocketEventType::Open;
				if (Event.Type == EVaRestSocketEventType::Message)
				{
					bEcho |= Event.Kind == EVaRestSocketFrameKind::Text && Event.bFinal && Event.Bytes.Num() == Expected.Length()
						&& FMemory::Memcmp(Event.Bytes.GetData(), Expected.Get(), Expected.Length()) == 0;
					Connection->ConsumedMessages(Event.Bytes.Num(), 1);
				}
				if (Event.IsTerminal()) { ++Terminals; End = Event.End; }
			}
		}
	};

	TSharedRef<IWebSocket> OpenEngine(FWebSocketsModule& Module, const FString& Url, const TSharedRef<FEngineSide>& Engine)
	{
		const TSharedRef<IWebSocket> Socket = Module.CreateWebSocket(Url, FString());
		// SDK IWebSocket dispatches these on the game thread. No callback captures a stack/test pointer.
		Socket->OnConnected().AddLambda([Engine] { Engine->bOpen = true; });
		Socket->OnMessage().AddLambda([Engine](const FString& Text) { Engine->Messages.Add(Text); });
		Socket->OnConnectionError().AddLambda([Engine](const FString& Why) { Engine->Error = Why; Engine->bEnded = true; });
		Socket->OnClosed().AddLambda([Engine](int32 Code, const FString&, bool bClean)
			{ Engine->CloseCode = Code; Engine->bClean = bClean; Engine->bEnded = true; });
		Socket->Connect();
		return Socket;
	}

	bool BothAtOnce(FAutomationTestBase& Test, FWebSocketsModule& Module, ILwsTestTransport& Transport,
		const FString& EngineUrl, FVaRestSocketConnectRequest&& Request)
	{
		Test.AddInfo(FString::Printf(TEXT("CookedCoexist journal-key URLs: %s %s"), *EngineUrl, *Request.Url));
		const TSharedRef<FEngineSide> Engine = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> Socket = OpenEngine(Module, EngineUrl, Engine);
		FPrivateSide Own(Transport.Connect(MoveTemp(Request)));
		const bool bSettled = PumpUntil([&] { Own.Pull(); return (Own.bOpen || Own.Terminals > 0) && (Engine->bOpen || Engine->bEnded); }, 20.0, true);
		const bool bBothOpen = bSettled && Own.bOpen && Own.Terminals == 0 && Engine->bOpen && !Engine->bEnded;
		bool bSent = false, bEchoed = false;
		if (bBothOpen)
		{
			Socket->Send(TEXT("engine-game-coexist"));
			bSent = Own.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("varest-game-coexist"))));
			bEchoed = PumpUntil([&] { Own.Pull(); return Own.bEcho && Engine->Messages.ContainsByPredicate([](const FString& Text)
				{ return Text.Equals(TEXT("engine-game-coexist"), ESearchCase::CaseSensitive); }); }, 20.0, true);
		}
		if (!Engine->bEnded) Socket->Close(1000, TEXT("coexist done"));
		if (Own.Terminals == 0) Own.Connection->Close(1000, TEXT("coexist done"));
		const bool bDrained = PumpUntil([&] { Own.Pull(); return Engine->bEnded && Own.Terminals > 0 && Transport.GetCounters().LiveConnections == 0; }, 10.0, true);
		const bool bClean = bDrained && Own.Terminals == 1 && Own.End.Failure == EVaRestSocketFailure::None && Own.End.bWasClean
			&& Own.End.Code == 1000 && Engine->Error.IsEmpty() && Engine->bClean && Engine->CloseCode == 1000;
		Test.AddInfo(FString::Printf(TEXT("CookedCoexist %s bothOpen=%d sent=%d echoed=%d drained=%d clean=%d ownTerminals=%d ownCode=%d engineCode=%d engineError=%s"),
			*EngineUrl, bBothOpen, bSent, bEchoed, bDrained, bClean, Own.Terminals, Own.End.Code, Engine->CloseCode, *Engine->Error));
		return bBothOpen && bSent && bEchoed && bClean;
	}
}

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FVaRestCookedCoexistTest, "VaRest.WebSocket.Lws.CookedCoexist",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

void FVaRestCookedCoexistTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	if (VaRest::WebSocket::CookedCoexist::Dedicated()) { Names.Add(TEXT("Both")); Commands.Add(TEXT("Both")); }
}

bool FVaRestCookedCoexistTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::CookedCoexist;
	using namespace VaRest::WebSocket::Testing;
	if (!Dedicated() || Parameters != TEXT("Both") || !IsRunningGame() || GIsEditor || !GEngine)
	{
		AddError(TEXT("CookedCoexist requires its dedicated ordinary Game Development nowrite/unattended process")); return false;
	}
	if (!TestTrue(TEXT("the compiled actual backend has the private numeric route"), UsesPrivateNumericRoute())) return false;
	bool bExternalInsecure = false;
	if (FParse::Param(FCommandLine::Get(), TEXT("lws-insecure"))
		|| FParse::Bool(FCommandLine::Get(), TEXT("lws-insecure="), bExternalInsecure))
	{
		AddError(TEXT("An external lws-insecure override would invalidate the real Engine policy restoration control")); return false;
	}
	int32 Port = 0, TlsPort = 0;
	if (!FParse::Value(FCommandLine::Get(), TEXT("VaRestCoexistPort="), Port)
		|| !FParse::Value(FCommandLine::Get(), TEXT("VaRestCoexistTlsPort="), TlsPort)
		|| Port < 1024 || Port > 65535 || TlsPort < 1024 || TlsPort > 65535 || Port == TlsPort)
	{
		AddError(TEXT("Two distinct real loopback listeners are required by VaRestCoexistPort/TlsPort")); return false;
	}
	const FString HttpBase = FString::Printf(TEXT("http://127.0.0.1:%d"), Port);
	FLwsTransportOptions Options; Options.bOverrideAllowlist = true;
	Options.Allowlist.Emplace(TEXT("ws"), TArray<FString>{TEXT("127.0.0.1")});
	Options.Allowlist.Emplace(TEXT("wss"), TArray<FString>{TEXT("localhost")});
	Options.TcpAddressOverride = TEXT("127.0.0.1");
	if (!FetchCa(*this, HttpBase, Options.ExtraTrustedPem)) return false;
	const auto Transport = MakeLwsTransport(MoveTemp(Options));
	ON_SCOPE_EXIT { Transport->Shutdown(); };
	AddExpectedMessagePlain(TEXT("libwebsockets("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	VaRest::Http::Testing::FLogCapture Capture;
	FVaRestSocketConnectRequest Warmup; Warmup.Url = FString::Printf(TEXT("ws://127.0.0.1:%d/ws/echo?key=%s"), Port, *FGuid::NewGuid().ToString(EGuidFormats::Digits));
	AddInfo(FString::Printf(TEXT("CookedCoexist journal-key URL: %s"), *Warmup.Url));
	FPrivateSide Ready(Transport->Connect(MoveTemp(Warmup)));
	if (!TestTrue(TEXT("real private context opens before the Engine-module observation"), PumpUntil([&] { Ready.Pull(); return Ready.bOpen || Ready.Terminals > 0; }, 20.0, true) && Ready.bOpen)) return false;
	Ready.Connection->Close(1000, TEXT("warmup done"));
	if (!TestTrue(TEXT("warmup physically drains before any new context/TLS operation"), PumpUntil([&] { Ready.Pull(); return Ready.Terminals == 1 && Transport->GetCounters().LiveConnections == 0; }, 10.0, true))) return false;
	const FString PrivateLevel = DescribeLwsLogLevel();
	TestEqualSensitive(TEXT("private C emitter has its actual D30 level"), PrivateLevel, FString(TEXT("err=1 warn=2 notice=0 info=0 client=0")));
	const bool bEngineAlreadyLoaded = FModuleManager::Get().IsModuleLoaded(TEXT("WebSockets"));
	bool bBeforeDisabled = false;
	const bool bHadConfig = GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bBeforeDisabled, GEngineIni);
	if (!TestFalse(TEXT("dedicated Engine leg initially uses secure certificate policy"), bBeforeDisabled)) return false;
	bool bWs = false, bWss = false; FString OldVerbosity;
	const FString WsKey = FGuid::NewGuid().ToString(EGuidFormats::Digits), WssKey = FGuid::NewGuid().ToString(EGuidFormats::Digits);
	{
		ON_SCOPE_EXIT
		{
			GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bBeforeDisabled, GEngineIni);
			FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{TEXT("LwsWebSocket")});
			if (!bHadConfig) GConfig->RemoveKey(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), GEngineIni);
			if (!OldVerbosity.IsEmpty()) GEngine->Exec(nullptr, *FString::Printf(TEXT("Log LogWebSockets %s"), *OldVerbosity));
		};
		// The Engine leg alone bypasses test-CA validation in memory. No roots/files change; VaRest still verifies CA/name.
		GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), true, GEngineIni);
		FWebSocketsModule& Engine = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{TEXT("LwsWebSocket")});
		TestEqualSensitive(TEXT("Engine module/context creation did not alter the private C emitter"), DescribeLwsLogLevel(), PrivateLevel);
		OldVerbosity = LogVerbosity();
		if (!TestFalse(TEXT("actual Engine log category is present"), OldVerbosity.IsEmpty())) return false;
		GEngine->Exec(nullptr, TEXT("Log LogWebSockets Verbose"));
		FVaRestSocketConnectRequest Ws; Ws.Url = FString::Printf(TEXT("ws://127.0.0.1:%d/ws/echo?key=%s"), Port, *WsKey);
		bWs = BothAtOnce(*this, Engine, *Transport, FString::Printf(TEXT("ws://127.0.0.1:%d/ws/echo?key=%s"), Port, *FGuid::NewGuid().ToString(EGuidFormats::Digits)), MoveTemp(Ws));
		FVaRestSocketConnectRequest Wss; Wss.Url = FString::Printf(TEXT("wss://localhost:%d/ws/echo?key=%s"), TlsPort, *WssKey);
		bWss = BothAtOnce(*this, Engine, *Transport, FString::Printf(TEXT("wss://127.0.0.1:%d/ws/echo?key=%s"), TlsPort, *FGuid::NewGuid().ToString(EGuidFormats::Digits)), MoveTemp(Wss));
	}
	bool bAfterDisabled = false;
	TestEqual(TEXT("Engine test-CA key presence restored"), GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bAfterDisabled, GEngineIni), bHadConfig);
	TestEqual(TEXT("Engine test-CA value restored"), bAfterDisabled, bBeforeDisabled);
	TestEqualSensitive(TEXT("Engine category verbosity restored"), LogVerbosity(), OldVerbosity);
	TestEqualSensitive(TEXT("actual private C emitter survived both Engine legs"), DescribeLwsLogLevel(), PrivateLevel);
	// Verify the restored manager policy by real I/O too, not only by reading the memory key we wrote.
	const int32 RestoredLogBegin = Capture.GetLines().Num();
	const TSharedRef<FEngineSide> Restored = MakeShared<FEngineSide>();
	const auto Probe = OpenEngine(FModuleManager::GetModuleChecked<FWebSocketsModule>(TEXT("WebSockets")),
		FString::Printf(TEXT("wss://127.0.0.1:%d/ws/echo?key=%s"), TlsPort, *FGuid::NewGuid().ToString(EGuidFormats::Digits)), Restored);
	const bool bPolicySettled = PumpUntil([&] { return Restored->bOpen || Restored->bEnded; }, 20.0, true);
	if (!Restored->bEnded) Probe->Close(1000, TEXT("restore control done"));
	const bool bPolicyDrained = PumpUntil([&] { return Restored->bEnded; }, 10.0, true);
	// This SDK can report only "client connect failed" to IWebSocket. Its C emitter carries the verification cause.
	FString VerificationFailure;
	const TArray<FString> RestoredLines = Capture.GetLines();
	for (int32 Index = RestoredLogBegin; Index < RestoredLines.Num(); ++Index)
	{
		const FString& Line = RestoredLines[Index];
		if (Line.StartsWith(TEXT("LogWebSockets: "), ESearchCase::CaseSensitive)
			&& Line.Contains(TEXT("Lws(Error): SSL error:"), ESearchCase::CaseSensitive)
			&& Line.Contains(TEXT("certificate"), ESearchCase::IgnoreCase)
			&& Line.Contains(TEXT("preverify_ok=0"), ESearchCase::CaseSensitive)) VerificationFailure = Line;
	}
	const bool bPolicyRefused = bPolicySettled && bPolicyDrained && !Restored->bOpen
		&& !Restored->Error.IsEmpty() && !VerificationFailure.IsEmpty();
	TestTrue(TEXT("actual Engine WSS rejects the peer test CA after secure-policy restoration"), bPolicyRefused);
	AddInfo(FString::Printf(TEXT("CookedCoexist restored-policy refusal: %s; verification: %s"), *Restored->Error, *VerificationFailure));
	int32 EngineLines = 0, PrivateKeysInEngine = 0;
	for (const FString& Line : Capture.GetLines())
	{
		if (!Line.StartsWith(TEXT("LogWebSockets: "), ESearchCase::CaseSensitive)) continue;
		EngineLines += Line.Contains(TEXT("Lws("), ESearchCase::CaseSensitive) ? 1 : 0;
		PrivateKeysInEngine += Line.Contains(WsKey, ESearchCase::CaseSensitive) || Line.Contains(WssKey, ESearchCase::CaseSensitive) ? 1 : 0;
	}
	TestTrue(TEXT("actual Engine C emitter produced routed LWS lines"), EngineLines > 0);
	TestEqual(TEXT("private request identities did not cross into Engine LWS output"), PrivateKeysInEngine, 0);
	TestTrue(TEXT("both real WS connections open/echo/close together"), bWs);
	TestTrue(TEXT("both real WSS connections open/echo/close together"), bWss);
	AddInfo(FString::Printf(TEXT("CookedCoexist EngineLoadedBefore=%d EngineLines=%d PrivateLwsAddress=%p PrivateSymbolImage=%s; monolithic original/private symbol ownership must also be joined from the real linked ELF"),
		bEngineAlreadyLoaded, EngineLines, GetLwsCreateContextAddress(), *DescribeLwsCopy()));
	return bWs && bWss && bPolicyRefused && EngineLines > 0 && PrivateKeysInEngine == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS && VAREST_COOKED_COEXIST_TESTS
