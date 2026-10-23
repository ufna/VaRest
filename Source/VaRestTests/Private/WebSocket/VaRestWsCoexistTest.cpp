// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 1.2 and 17 (W1 exit "editor coexistence"): the engine's IWebSocket and VaRest's transport open at once, ws and
// wss; VaRestWebSocket's own LWS copy and log level (VaRest-5ia M-1, D30); the engine's LWS lines still in LogWebSockets. And a
// transport shut down with connections in every state, ten times (VaRest-x2v's suspect: a teardown race)

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/Engine.h"
#include "IWebSocket.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "Misc/StringOutputDevice.h"
#include "Modules/ModuleManager.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"
#include "WebSocketsModule.h"

#if PLATFORM_LINUX
#include <dlfcn.h>
#endif

namespace VaRest::WsTest::VaRestWsCoexistTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;


#if WITH_WEBSOCKETS
	/** Game thread: one engine IWebSocket and what it reported (its delegates run on the game thread, from the core ticker) */
	struct FEngineSide
	{
		bool bConnected = false;
		bool bEnded = false;
		FString Error;
		TArray<FString> Messages;
	};

	/** Game thread: bind owned delegate state and start an engine socket */
	TSharedRef<IWebSocket> OpenEngineSocket(FWebSocketsModule& Module, const FString& Url, const TSharedRef<FEngineSide>& Side)
	{
		// CreateWebSocket is virtual (WebSocketsModule.h:72): no link to the WebSockets module. Connected from the test body,
		// never from a handler (E25)
		TSharedRef<IWebSocket> Socket = Module.CreateWebSocket(Url, FString());
		Socket->OnConnected().AddLambda([Side] { Side->bConnected = true; });
		Socket->OnMessage().AddLambda([Side](const FString& Message) { Side->Messages.Add(Message); });
		Socket->OnConnectionError().AddLambda([Side](const FString& Reason) { Side->Error = Reason; Side->bEnded = true; });
		Socket->OnClosed().AddLambda([Side](int32 Code, const FString& Reason, bool bWasClean) { Side->bEnded = true; });
		Socket->Connect();
		return Socket;
	}

	/** Game thread: the exact text was received */
	bool Echoed(const TSharedRef<FEngineSide>& Side, const TCHAR* Text)
	{
		return Side->Messages.ContainsByPredicate([Text](const FString& Each) { return Each.Equals(Text, ESearchCase::CaseSensitive); });
	}

	/** Game thread: a log category's verbosity as "Log list <cat>" prints it (LogSuppressionInterface.cpp:593-614), the probe's way (VaRestLwsProbeTlsTest.cpp:181-210) */
	FString LogVerbosityOf(const TCHAR* Category)
	{
		FString Out;
#if UE_ALLOW_EXEC_COMMANDS
		if (!GEngine)
		{
			return Out;
		}
		FStringOutputDevice Listing;
		Listing.SetAutoEmitLineTerminator(true);
		GEngine->Exec(nullptr, *FString::Printf(TEXT("Log list %s"), Category), Listing);
		TArray<FString> Lines;
		Listing.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			TArray<FString> Tokens;
			Line.ParseIntoArrayWS(Tokens);
			if (Tokens.Num() >= 2 && Tokens[0].Equals(Category, ESearchCase::CaseSensitive))
			{
				Out = Tokens[1];
			}
		}
#endif
		return Out;
	}

	/** Game thread: an engine socket and a VaRest connection open at once, both echo, both end; false with the reason when any step fails */
	bool BothAtOnce(FWebSocketsModule& Module, ILwsTestTransport& Transport, const FString& EngineUrl, FVaRestSocketConnectRequest&& Req, FString& OutWhy)
	{
		const TSharedRef<FEngineSide> Side = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> Socket = OpenEngineSocket(Module, EngineUrl, Side);
		FEvents Ours(Transport.Connect(MoveTemp(Req)));
		Ours.WaitFor([Side](const TArray<FVaRestSocketEvent>& All)
		{
			const bool bOursSettled = All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.Type == EVaRestSocketEventType::Open || Each.IsTerminal(); });
			return bOursSettled && (Side->bConnected || Side->bEnded);
		}, 20.0);
		const bool bBothOpen = Side->bConnected && Ours.Find(EVaRestSocketEventType::Open) != nullptr;
		bool bEchoed = false;
		if (bBothOpen)
		{
			Socket->Send(TEXT("engine-hello"));
			Ours.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("varest-hello"))));
			bEchoed = Ours.WaitFor([Side](const TArray<FVaRestSocketEvent>& All)
			{
				return Echoed(Side, TEXT("engine-hello")) && All.ContainsByPredicate([](const FVaRestSocketEvent& Each)
				{
					return Each.Type == EVaRestSocketEventType::Message && Utf8(Each.Bytes).Equals(TEXT("varest-hello"), ESearchCase::CaseSensitive);
				});
			}, 20.0);
		}
		// Both drain, whatever happened
		if (!Side->bEnded)
		{
			Socket->Close(1000, TEXT("done"));
		}
		if (!Ours.GetEnd())
		{
			Ours.Connection->Close(1000, TEXT("done"));
		}
		const bool bEnded = Ours.WaitFor([Side](const TArray<FVaRestSocketEvent>& All)
		{
			return Side->bEnded && All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
		}, 20.0);
		OutWhy = FString::Printf(TEXT("engine: connected=%d ended=%d error=\"%s\" echoed=%d; VaRest: %s; both ended=%d"), Side->bConnected, Side->bEnded, *Side->Error,
			Echoed(Side, TEXT("engine-hello")), *Ours.Describe(), bEnded);
		return bBothOpen && bEchoed && bEnded;
	}
#endif // WITH_WEBSOCKETS
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCoexistEngineTest, "VaRest.Integration.WebSocket.Transport.Coexist.EngineWebSocket",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestWsCoexistEngineTest::RunTest(const FString& Parameters)
{
#if WITH_WEBSOCKETS
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsCoexistTestPrivate;
	ExpectLwsLines(*this);
	// The engine's own LWS lines (LwsWebSocketsManager.cpp:403-424) are this test's evidence, not failures
	AddExpectedMessagePlain(TEXT("Lws("), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, -1);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	VaRest::Http::Testing::FLogCapture Capture;
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
	if (!Transport.IsValid())
	{
		return false;
	}
	ON_SCOPE_EXIT
	{
		Transport->Shutdown();
	};
	const TArray<FScheme> Both = Schemes(Site);
	// VaRest's context first (ws only: no wss in flight while the engine module makes its context, VaRest-rti)
	const TUniquePtr<FEvents> Warmup = Visit(*Transport, Request(Both[0], TEXT("/ws/echo?key=") + NewKey()));
	if (!TestNotNull(TEXT("VaRest's transport opens before the engine module loads"), Warmup->Find(EVaRestSocketEventType::Open))
		|| !TestNotNull(TEXT("the warmup connection drained"), Warmup->GetEnd()))
	{
		return false;
	}
	const FString LevelBefore = VaRest::WebSocket::Testing::DescribeLwsLogLevel();
	FWebSocketsModule& Module = FModuleManager::LoadModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	const FString LevelAfter = VaRest::WebSocket::Testing::DescribeLwsLogLevel();
	// D30 (spec 1.2): in a modular build VaRest's copy keeps errors and warnings only; the engine's InitWebSockets set its own
	// copy's level (LwsWebSocketsManager.cpp:174-176) [coexist.shared_lws_state]
	TestEqualSensitive(TEXT("VaRest's LWS log level: errors and warnings (D30)"), LevelAfter, FString(TEXT("err=1 warn=2 notice=0 info=0 client=0")));
	TestEqualSensitive(TEXT("... untouched by the engine's WebSockets module"), LevelAfter, LevelBefore);
#if PLATFORM_LINUX
	// VaRest-5ia M-1: the per-copy claim rests on this measurement, not on RTLD_LOCAL alone (spec 1.2) [coexist.lws_symbol_owner]
	const FString OwnFile = VaRest::WebSocket::Testing::DescribeLwsCopy();
	TestTrue(FString::Printf(TEXT("VaRest's lws_create_context is in VaRestWebSocket's own library (%s)"), *OwnFile), OwnFile.Contains(TEXT("VaRestWebSocket"), ESearchCase::CaseSensitive));
	// The engine module's own path, from its object's vtable pointer (the Itanium ABI puts it first; VaRestLwsProbeTlsTest.cpp:486-503)
	Dl_info ModuleInfo = {};
	void* EngineLib = dladdr(*reinterpret_cast<void**>(&Module), &ModuleInfo) != 0 && ModuleInfo.dli_fname ? dlopen(ModuleInfo.dli_fname, RTLD_LAZY | RTLD_NOLOAD) : nullptr;
	if (TestNotNull(TEXT("the engine's WebSockets library"), EngineLib))
	{
		const void* EngineSymbol = dlsym(EngineLib, "lws_create_context");
		TestTrue(TEXT("... has an lws_create_context of its own, not VaRest's"), EngineSymbol != nullptr && EngineSymbol != VaRest::WebSocket::Testing::GetLwsCreateContextAddress());
		// RTLD_NOLOAD took a reference: give it back
		dlclose(EngineLib);
	}
#endif
	// The engine's leg needs its certificate check off for the test CA and the IP host: the probe's scope-guarded flip
	// (VaRestLwsProbeTlsTest.cpp:521-589, ruling Q4) -- restored on every path out of this block, then checked
	FString WsWhy;
	FString WssWhy;
	bool bWs = false;
	bool bWss = false;
	int32 EngineLwsLines = 0;
	int32 VaRestKeysInEngineCategory = 0;
	// Review spec M-4: VaRest's own emitter writes LogVaRestSocket only, so a count of its lines in LogWebSockets could never
	// fail. What would show VaRest's handshake running through the engine's LWS copy is the engine's Verbose client lines
	// naming a VaRest request: these keys are in VaRest's URLs only
	const FString OursWsKey = NewKey();
	const FString OursWssKey = NewKey();
	bool bWasDisabled = false;
	const bool bHadKey = GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
	const FString OldVerbosity = LogVerbosityOf(TEXT("LogWebSockets"));
	{
		ON_SCOPE_EXIT
		{
			// The engine re-reads the flag only when the key exists (LwsWebSocketsManager.cpp:93, ConfigCacheIni.cpp:5424-5438): the
			// old value goes back first and is read, then an absent key is removed again
			GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bWasDisabled, GEngineIni);
			FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
			if (!bHadKey)
			{
				GConfig->RemoveKey(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), GEngineIni);
			}
#if UE_ALLOW_EXEC_COMMANDS
			if (!OldVerbosity.IsEmpty() && GEngine)
			{
				GEngine->Exec(nullptr, *FString::Printf(TEXT("Log LogWebSockets %s"), *OldVerbosity));
			}
#endif
		};
		GConfig->SetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), true, GEngineIni);
		FCoreDelegates::TSOnConfigSectionsChanged().Broadcast(GEngineIni, TSet<FString>{ TEXT("LwsWebSocket") });
#if UE_ALLOW_EXEC_COMMANDS
		if (GEngine && !OldVerbosity.IsEmpty())
		{
			GEngine->Exec(nullptr, TEXT("Log LogWebSockets Verbose"));
		}
#endif
		bWs = BothAtOnce(Module, *Transport, FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *NewKey()), Request(Both[0], TEXT("/ws/echo?key=") + OursWsKey), WsWhy);
		bWss = BothAtOnce(Module, *Transport, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Request(Both[1], TEXT("/ws/echo?key=") + OursWssKey), WssWhy);
		for (const FString& Line : Capture.GetLines())
		{
			const bool bEngineCategory = Line.StartsWith(TEXT("LogWebSockets: "), ESearchCase::CaseSensitive);
			EngineLwsLines += bEngineCategory && Line.Contains(TEXT("Lws("), ESearchCase::CaseSensitive) ? 1 : 0;
			VaRestKeysInEngineCategory += bEngineCategory && (Line.Contains(OursWsKey, ESearchCase::CaseSensitive) || Line.Contains(OursWssKey, ESearchCase::CaseSensitive)) ? 1 : 0;
		}
		AddInfo(FString::Printf(TEXT("LogWebSockets verbosity before: %s"), OldVerbosity.IsEmpty() ? TEXT("(not read)") : *OldVerbosity));
	}
	bool bRestoredDisabled = false;
	TestEqual(TEXT("the certificate config key presence was restored"), GConfig->GetBool(TEXT("LwsWebSocket"), TEXT("bDisableCertValidation"), bRestoredDisabled, GEngineIni), bHadKey);
	TestEqual(TEXT("the certificate config value was restored"), bRestoredDisabled, bWasDisabled);
	TestEqualSensitive(TEXT("the engine log verbosity was restored"), LogVerbosityOf(TEXT("LogWebSockets")), OldVerbosity);
	TestTrue(TEXT("the engine LWS emitter produced evidence in LogWebSockets"), EngineLwsLines > 0);
	TestTrue(FString::Printf(TEXT("ws: both open, echo and end at once (%s)"), *WsWhy), bWs);
	TestTrue(FString::Printf(TEXT("wss: both open, echo and end at once (%s)"), *WssWhy), bWss);
	TestEqual(TEXT("the engine's LWS copy never logged a VaRest request (each kept its own copy)"), VaRestKeysInEngineCategory, 0);
	AddInfo(FString::Printf(TEXT("%d libwebsockets lines of the engine's copy in LogWebSockets during the test [coexist.engine_lws_log_routed]"), EngineLwsLines));
	// Harness: the restore took (the probe's own check, VaRestLwsProbeTlsTest.cpp:630-660); -lws-insecure= overrides the ini
	bool bInsecureArg = false;
#if !UE_BUILD_SHIPPING
	const bool bHasInsecureArg = FParse::Bool(FCommandLine::Get(), TEXT("lws-insecure="), bInsecureArg);
#else
	const bool bHasInsecureArg = false;
#endif
	if (!bHasInsecureArg)
	{
		const TSharedRef<FEngineSide> Checked = MakeShared<FEngineSide>();
		const TSharedRef<IWebSocket> CheckedSocket = OpenEngineSocket(Module, FString::Printf(TEXT("wss://%s:%d/ws/echo?key=%s"), *Site.Host, Site.TlsPort, *NewKey()), Checked);
		VaRest::Http::Testing::PumpUntil([Checked] { return Checked->bConnected || Checked->bEnded; }, 20.0, true);
		const bool bOpened = Checked->bConnected;
		if (!Checked->bEnded)
		{
			CheckedSocket->Close(1000, TEXT("done"));
		}
		TestTrue(TEXT("the engine restore-check socket drained"), VaRest::Http::Testing::PumpUntil([Checked] { return Checked->bEnded; }, 20.0, true));
		TestEqual(TEXT("the engine certificate check follows its original config after restore"), bOpened, bWasDisabled);
	}
	else
	{
		// The probe's case (VaRestLwsProbeTlsTest.cpp:667-680): the flag is re-set from the command line on every re-read
		AddInfo(TEXT("the restore check is skipped: -lws-insecure= on the command line (LwsWebSocketsManager.cpp:95-98)"));
	}
	return true;
#else
	AddInfo(TEXT("WITH_WEBSOCKETS is 0 on this platform: nothing to coexist with"));
	return true;
#endif
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCoexistShutdownTest, "VaRest.Integration.WebSocket.Transport.Coexist.ShutdownStress",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestWsCoexistShutdownTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsCoexistTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	const TArray<FScheme> Both = Schemes(Site);
	for (int32 Round = 0; Round < 10; ++Round)
	{
		// One transport at a time (VaRest-rti): each round's is shut down before the next is made
		const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site);
		if (!Transport.IsValid())
		{
			return false;
		}
		ON_SCOPE_EXIT { Transport->Shutdown(); };
		TArray<TUniquePtr<FEvents>> All;
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[0], TEXT("/ws/hang?ms=20000&key=") + NewKey()))));   // pending
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[0], TEXT("/ws/sink?key=") + NewKey()))));            // open, writing
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[1], TEXT("/ws/echo?key=") + NewKey()))));            // open, TLS
		All.Add(MakeUnique<FEvents>(Transport->Connect(Request(Both[0], TEXT("/ws/noecho?key=") + NewKey()))));          // closing
		for (int32 Index = 1; Index < All.Num(); ++Index)
		{
			TestTrue(FString::Printf(TEXT("round %d, connection %d: opened"), Round, Index), All[Index]->WaitType(EVaRestSocketEventType::Open, 10.0));
		}
		TArray<uint8> Big;
		Big.SetNumZeroed(4 * 1024 * 1024);
		All[1]->Connection->Send(FVaRestSocketFrame::Binary(Big));
		All[3]->Connection->Close(1000, TEXT("never echoed"));
		Transport->Shutdown();
		TestEqual(TEXT("Shutdown destroyed every live connection"), Transport->GetCounters().LiveConnections, 0);
		for (int32 Index = 0; Index < All.Num(); ++Index)
		{
			// Shutdown reports every connection before it returns (EndForShutdown): no pumping here. Without that, the
			// connections would still end -- from the callbacks lws_context_destroy makes -- but as Unknown or a transport
			// end, not Shutdown, which is how W2 tells an engine exit from a network failure
			All[Index]->Pull();
			const int32 Terminal = All[Index]->Count(EVaRestSocketEventType::Closed) + All[Index]->Count(EVaRestSocketEventType::Error);
			TestEqual(FString::Printf(TEXT("round %d, connection %d: exactly one terminal event (%s)"), Round, Index, *All[Index]->Describe()), Terminal, 1);
			const FVaRestSocketEvent* End = All[Index]->GetEnd();
			const EVaRestSocketEventType Wanted = Index == 0 ? EVaRestSocketEventType::Error : EVaRestSocketEventType::Closed;
			TestTrue(FString::Printf(TEXT("round %d, connection %d: %s with Failure Shutdown (%s)"), Round, Index, Index == 0 ? TEXT("Error") : TEXT("Closed 1006"), *All[Index]->Describe()),
				End && End->Type == Wanted && End->End.Failure == EVaRestSocketFailure::Shutdown && (Index == 0 || End->End.Code == 1006));
		}
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
