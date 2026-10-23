// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: connect, handshake, open and refusal against Tools/testserver/ws.py, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"

#include <atomic>

namespace VaRest::WsTest::VaRestWsConnectTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	struct FSinkLog
	{
		std::atomic<int32> Calls{ 0 };
		std::atomic<int32> OnGameThread{ 0 };
		std::atomic<bool> bComplete{ false };
	};

	/**
	 * Holds the service thread from inside a connection's OnEvents, which runs there for an Error that FConn::Start queues
	 * (E7): until the transport's Shutdown has begun (its first line sets bShuttingDown, VaRestLwsService.cpp:241), at most
	 * 10 s, so the commands queued behind it run after it -- the real condition, never a fixed sleep (Task 7 review M-1).
	 * OnEvents' contract is to only schedule work; this test breaks it on purpose
	 */
	struct FServiceGate
	{
		std::atomic<bool> bEntered{ false };
		/** Hold saw Shutdown begin; false when it gave up at its cap */
		std::atomic<bool> bSawShutdown{ false };

		/**
		 * The service thread. Weak: the transport owns the link that owns the OnEvents holding this, so a strong reference
		 * would be a cycle. The test's frame holds the transport until its Shutdown has joined this thread, so a pin here is
		 * never the last reference
		 */
		void Hold(const TWeakPtr<ILwsTestTransport, ESPMode::ThreadSafe>& Transport)
		{
			bEntered = true;
			const double Until = FPlatformTime::Seconds() + 10.0;
			while (FPlatformTime::Seconds() < Until)
			{
				const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Pinned = Transport.Pin();
				if (Pinned.IsValid() && Pinned->IsShuttingDown())
				{
					bSawShutdown = true;
					return;
				}
				FPlatformProcess::SleepNoStats(0.001f);
			}
		}
	};

	/** Until the transport holds no connection: LWS destroyed every wsi */
	bool WaitIdle(ILwsTestTransport& Transport, double Seconds = 5.0)
	{
		return VaRest::Http::Testing::PumpUntil([&Transport] { return Transport.GetCounters().LiveConnections == 0; }, Seconds, true);
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN(bNeedTls) \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsConnectTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site, bNeedTls)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectOpenTest, "VaRest.Integration.WebSocket.Transport.Connect.Open", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectOpenTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FEvents Events(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + Key)));
		const bool bOpened = Events.WaitType(EVaRestSocketEventType::Open);
		const FVaRestSocketEvent* Open = Events.Find(EVaRestSocketEventType::Open);
		if (!TestTrue(FString::Printf(TEXT("%s: opened (%s)"), Scheme.Name, *Events.Describe()), bOpened && Open != nullptr))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd();
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: no subprotocol selected"), Scheme.Name), Open->Protocol.IsEmpty());
		TestTrue(FString::Printf(TEXT("%s: the connect time is measured (%.4f s)"), Scheme.Name, Open->Seconds), Open->Seconds > 0.0 && Open->Seconds < 10.0);
		TestFalse(FString::Printf(TEXT("%s: not ended"), Scheme.Name), Events.Connection->HasEnded());
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd());
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: Closed by the client, 4000, clean (%s)"), Scheme.Name, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Closed && End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && End->End.bWasClean);
		TestEqual(FString::Printf(TEXT("%s: one terminal event"), Scheme.Name), Events.Count(EVaRestSocketEventType::Closed) + Events.Count(EVaRestSocketEventType::Error), 1);
		FString Reason;
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqual(FString::Printf(TEXT("%s: the server got our close 4000"), Scheme.Name), JournalCloseCode(Journal, &Reason), 4000);
		TestEqualSensitive(FString::Printf(TEXT("%s: ... with its reason"), Scheme.Name), Reason, FString(TEXT("bye")));
	}
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	// Task 6's carry: each close asked for leaves UnsentCloses exactly once (written, or ended unwritten) -- a missed
	// decrement holds Shutdown's flush for ExitFlushSeconds, a second one ends it before a close is written
	TestEqual(TEXT("each close asked for left UnsentCloses once"), Transport->GetCounters().UnsentCloses, 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectHandshakeTest, "VaRest.Integration.WebSocket.Transport.Connect.Handshake", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectHandshakeTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Req.Headers = { { TEXT("X-VaRest-Test"), TEXT("value 1") }, { TEXT("Authorization"), TEXT("Bearer abc") } };
		const TUniquePtr<FEvents> Events = Visit(*Transport, MoveTemp(Req));
		TestNotNull(FString::Printf(TEXT("%s: opened (%s)"), Scheme.Name, *Events->Describe()), Events->Find(EVaRestSocketEventType::Open));
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqualSensitive(FString::Printf(TEXT("%s: a custom header"), Scheme.Name), JournalHeader(Journal, TEXT("X-VaRest-Test")), FString(TEXT("value 1")));
		TestEqualSensitive(FString::Printf(TEXT("%s: Authorization"), Scheme.Name), JournalHeader(Journal, TEXT("Authorization")), FString(TEXT("Bearer abc")));
		const FString Host = Scheme.bTls ? FString::Printf(TEXT("localhost:%d"), Site.TlsPort) : FString::Printf(TEXT("%s:%d"), *Site.Host, Site.Port);
		TestEqualSensitive(FString::Printf(TEXT("%s: Host keeps the non-default port [hs.host_header]"), Scheme.Name), JournalHeader(Journal, TEXT("Host")), Host);
		TestEqualSensitive(FString::Printf(TEXT("%s: the derived Origin, sent raw (D29, T4)"), Scheme.Name), JournalHeader(Journal, TEXT("Origin")),
			FString(Scheme.bTls ? TEXT("https://") : TEXT("http://")) + Host);
		TestEqualSensitive(FString::Printf(TEXT("%s: no subprotocol offered"), Scheme.Name), JournalHeader(Journal, TEXT("Sec-WebSocket-Protocol")), FString(TEXT("(absent)")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectOriginTest, "VaRest.Integration.WebSocket.Transport.Connect.Origin", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectOriginTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Given = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + Given);
		Req.Origin = FString(TEXT("https://game.example:8443"));
		Visit(*Transport, MoveTemp(Req));
		TestEqualSensitive(FString::Printf(TEXT("%s: an Origin as given [hs.raw_origin]"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, Given), TEXT("Origin")),
			FString(TEXT("https://game.example:8443")));
		const FString Omitted = NewKey();
		FVaRestSocketConnectRequest None = Request(Scheme, TEXT("/ws/echo?key=") + Omitted);
		None.Origin = FString();
		Visit(*Transport, MoveTemp(None));
		TestEqualSensitive(FString::Printf(TEXT("%s: an empty Origin omits it [hs.origin_omitted]"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, Omitted), TEXT("Origin")),
			FString(TEXT("(absent)")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectProtocolsTest, "VaRest.Integration.WebSocket.Transport.Connect.Protocols", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectProtocolsTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/protocol?select=chat.v1&key=") + Key);
		Req.Protocols = { TEXT("chat.v2"), TEXT("chat.v1") };
		const TUniquePtr<FEvents> Second = Visit(*Transport, MoveTemp(Req));
		const FVaRestSocketEvent* Open = Second->Find(EVaRestSocketEventType::Open);
		if (TestNotNull(FString::Printf(TEXT("%s: opened (%s)"), Scheme.Name, *Second->Describe()), Open))
		{
			// T17: the selection is read in FILTER_PRE_ESTABLISH; LWS skips the space after the comma when it matches it
			TestEqualSensitive(FString::Printf(TEXT("%s: the second offered protocol, selected"), Scheme.Name), Open->Protocol, FString(TEXT("chat.v1")));
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: offered in order"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, Key), TEXT("Sec-WebSocket-Protocol")),
			FString(TEXT("chat.v2, chat.v1")));
		FVaRestSocketConnectRequest NoneSelected = Request(Scheme, TEXT("/ws/protocol?key=") + NewKey());
		NoneSelected.Protocols = { TEXT("chat.v2") };
		const TUniquePtr<FEvents> Unselected = Visit(*Transport, MoveTemp(NoneSelected));
		const FVaRestSocketEvent* OpenWithout = Unselected->Find(EVaRestSocketEventType::Open);
		TestTrue(FString::Printf(TEXT("%s: a server that selects none still opens, with no protocol [hs.none_selected]"), Scheme.Name), OpenWithout && OpenWithout->Protocol.IsEmpty());
		FVaRestSocketConnectRequest Evil = Request(Scheme, TEXT("/ws/protocol?select=evil&key=") + NewKey());
		Evil.Protocols = { TEXT("chat.v2") };
		const TUniquePtr<FEvents> Mismatch = Visit(*Transport, MoveTemp(Evil));
		TestTrue(FString::Printf(TEXT("%s: a protocol not offered is ProtocolMismatch [hs.unoffered_selected] (%s)"), Scheme.Name, *Mismatch->Describe()),
			IsError(*Mismatch, EVaRestSocketFailure::ProtocolMismatch));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectRefusedTest, "VaRest.Integration.WebSocket.Transport.Connect.Refused", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectRefusedTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const int32 Status : { 401, 403, 404, 429, 500, 503 })
		{
			const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/status/%d?key=%s"), Status, *NewKey())));
			const FVaRestSocketEvent* End = Events->GetEnd();
			TestTrue(FString::Printf(TEXT("%s %d: Error UpgradeRefused with the status, code 0 (%s)"), Scheme.Name, Status, *Events->Describe()),
				End && End->Type == EVaRestSocketEventType::Error && End->End.Failure == EVaRestSocketFailure::UpgradeRefused && End->End.HttpStatus == Status && End->End.Code == 0);
		}
		for (const TCHAR* What : { TEXT("accept"), TEXT("upgrade"), TEXT("connection") })
		{
			const TUniquePtr<FEvents> Events = Visit(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/broken?what=%s&key=%s"), What, *NewKey())));
			TestTrue(FString::Printf(TEXT("%s: a 101 with a bad %s is BadUpgradeResponse (%s)"), Scheme.Name, What, *Events->Describe()),
				IsError(*Events, EVaRestSocketFailure::BadUpgradeResponse));
		}
		const TUniquePtr<FEvents> Dropped = Visit(*Transport, Request(Scheme, TEXT("/ws/noupgrade?key=") + NewKey()));
		TestTrue(FString::Printf(TEXT("%s: no status line is NoUpgradeResponse (22 A2) (%s)"), Scheme.Name, *Dropped->Describe()),
			IsError(*Dropped, EVaRestSocketFailure::NoUpgradeResponse));
	}
	FVaRestSocketConnectRequest Closed;
	Closed.Url = TEXT("ws://127.0.0.1:1/");
	const TUniquePtr<FEvents> Refused = Visit(*Transport, MoveTemp(Closed));
	TestTrue(FString::Printf(TEXT("a refused TCP connect is ConnectFailed [hs.connect_refused] (%s)"), *Refused->Describe()), IsError(*Refused, EVaRestSocketFailure::ConnectFailed));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectBadRequestTest, "VaRest.Integration.WebSocket.Transport.Connect.BadRequest", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectBadRequestTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	const TSharedRef<FSinkLog, ESPMode::ThreadSafe> Log = MakeShared<FSinkLog, ESPMode::ThreadSafe>();
	FVaRestSocketConnectRequest Bad;
	Bad.Url = TEXT("ftp://127.0.0.1/");
	const TSharedRef<FCallbackGate, ESPMode::ThreadSafe> Gate = MakeShared<FCallbackGate, ESPMode::ThreadSafe>();
	ON_SCOPE_EXIT { Gate->bRelease = true; };
	FEvents Events(Transport->Connect(MoveTemp(Bad), [Log, Gate]
	{
		Gate->Hold();
		++Log->Calls;
		Log->OnGameThread += IsInGameThread() ? 1 : 0;
		Log->bComplete = true;
	}));
	TestTrue(TEXT("notification entered its bounded gate"), Gate->WaitEntered());
	TestTrue(TEXT("a bad URL ends while its notification is held"), Events.WaitEnd());
	TestEqual(TEXT("the queued event is visible before notification observations"), Log->Calls.load(), 0);
	TestTrue(TEXT("notification gate released normally"), Gate->ReleaseAndWait());
	TestTrue(TEXT("notification observations completed"), VaRest::Http::Testing::PumpUntil([Log] { return Log->bComplete.load(); }, 5.0, true));
	TestTrue(FString::Printf(TEXT("... as Error BadUrl (%s)"), *Events.Describe()), IsError(Events, EVaRestSocketFailure::BadUrl));
	TestTrue(TEXT("the sink ran"), Log->Calls.load() >= 1);
	// E7: queued like any other event, from the service thread -- so never inside Connect
	TestEqual(TEXT("... never on the game thread, so never inside Connect"), Log->OnGameThread.load(), 0);

	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Injected = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Injected.Headers = { { TEXT("X-B"), TEXT("a\r\nX-C: b") } };
		const TUniquePtr<FEvents> Refused = Visit(*Transport, MoveTemp(Injected));
		TestTrue(FString::Printf(TEXT("%s: CR/LF in a header value is BadHandshake (%s)"), Scheme.Name, *Refused->Describe()), IsError(*Refused, EVaRestSocketFailure::BadHandshake));
		FVaRestSocketConnectRequest Accented = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Accented.Headers = { { TEXT("X-D"), TEXT("café") } };
		TestTrue(FString::Printf(TEXT("%s: a non-ASCII header value is BadHandshake"), Scheme.Name), IsError(*Visit(*Transport, MoveTemp(Accented)), EVaRestSocketFailure::BadHandshake));
		TestEqual(FString::Printf(TEXT("%s: neither reached the server"), Scheme.Name), FetchJournal(*this, Site, Key, 1, 500)[TEXT("connections")].AsInt(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectNotAllowedTest, "VaRest.Integration.WebSocket.Transport.Connect.NotAllowed", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectNotAllowedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Plan decision 19: the filter's map injected, never the host project's ini
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	Options.bOverrideAllowlist = true;
	// TTuple takes no nested braces: built with Emplace, as VaRestSocketTargetTest.cpp's allowlist
	Options.Allowlist.Emplace(FString(TEXT("ws")), TArray<FString>{ FString(TEXT("example.com")) });
	Options.Allowlist.Emplace(FString(TEXT("wss")), TArray<FString>{ FString(TEXT("example.com")) });
	const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Filtered = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
	ON_SCOPE_EXIT
	{
		Filtered->Shutdown();
	};
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		const TUniquePtr<FEvents> Events = Visit(*Filtered, Request(Scheme, TEXT("/ws/echo?key=") + Key));
		TestTrue(FString::Printf(TEXT("%s: a host the filter does not list is NotAllowed (%s)"), Scheme.Name, *Events->Describe()), IsError(*Events, EVaRestSocketFailure::NotAllowed));
		TestEqual(FString::Printf(TEXT("%s: and never reached the server"), Scheme.Name), FetchJournal(*this, Site, Key, 1, 500)[TEXT("connections")].AsInt(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectHeaderRoomTest, "VaRest.Integration.WebSocket.Transport.Connect.HeaderRoom", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectHeaderRoomTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Over the 32 KiB block: refused on the game thread, before any connect (plan decision 14)
		const FString TooBigKey = NewKey();
		FVaRestSocketConnectRequest TooBig = Request(Scheme, TEXT("/ws/echo?key=") + TooBigKey);
		TooBig.Headers = { { TEXT("X-Fill"), FString::ChrN(33000, TEXT('a')) } };
		const TUniquePtr<FEvents> Early = Visit(*Transport, MoveTemp(TooBig));
		const FVaRestSocketEvent* EarlyEnd = Early->GetEnd();
		TestTrue(FString::Printf(TEXT("%s: headers over 32 KiB are BadHandshake before the connect (%s)"), Scheme.Name, *Early->Describe()),
			EarlyEnd && EarlyEnd->End.Failure == EVaRestSocketFailure::BadHandshake && EarlyEnd->End.Detail.Contains(TEXT("32768"), ESearchCase::CaseSensitive));
		// Under the block limit, over the room LWS reports (32482 bytes for a short path [hs.append_header_space]): refused in
		// APPEND_HANDSHAKE_HEADER, the whole block or nothing (E19); the request never goes out
		const FString NoRoomKey = NewKey();
		FVaRestSocketConnectRequest NoRoom = Request(Scheme, TEXT("/ws/echo?key=") + NoRoomKey);
		NoRoom.Headers = { { TEXT("X-Fill"), FString::ChrN(32600, TEXT('a')) } };
		const TUniquePtr<FEvents> Late = Visit(*Transport, MoveTemp(NoRoom));
		const FVaRestSocketEvent* LateEnd = Late->GetEnd();
		TestTrue(FString::Printf(TEXT("%s: headers over LWS's room are BadHandshake from APPEND_HANDSHAKE_HEADER (%s)"), Scheme.Name, *Late->Describe()),
			LateEnd && LateEnd->End.Failure == EVaRestSocketFailure::BadHandshake && LateEnd->End.Detail.Contains(TEXT("handshake buffer has"), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("%s: neither request reached the server"), Scheme.Name),
			FetchJournal(*this, Site, TooBigKey, 1, 300)[TEXT("connections")].AsInt() + FetchJournal(*this, Site, NoRoomKey, 1, 300)[TEXT("connections")].AsInt(), 0);
		// A block that fits arrives whole [hs.append_header_fill]
		const FString FitsKey = NewKey();
		FVaRestSocketConnectRequest Fits = Request(Scheme, TEXT("/ws/echo?key=") + FitsKey);
		Fits.Headers = { { TEXT("X-Fill"), FString::ChrN(30000, TEXT('b')) } };
		const TUniquePtr<FEvents> Opened = Visit(*Transport, MoveTemp(Fits));
		TestNotNull(FString::Printf(TEXT("%s: 30000 bytes of header open (%s)"), Scheme.Name, *Opened->Describe()), Opened->Find(EVaRestSocketEventType::Open));
		TestEqual(FString::Printf(TEXT("%s: ... and arrive whole"), Scheme.Name), JournalHeader(FetchJournal(*this, Site, FitsKey), TEXT("X-Fill")).Len(), 30000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectWriteTest, "VaRest.Integration.WebSocket.Transport.Connect.Write", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectWriteTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/sink?key=") + Key);
		// Every send reaches the mark, so the buffered amount's return to 0 is a Drained (spec 2.4)
		Req.SendHighWater = 1;
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd();
			continue;
		}
		const uint8 Raw[] = { 1, 2, 3 };
		TestTrue(FString::Printf(TEXT("%s: a text send is accepted"), Scheme.Name), Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("hello")))));
		TestTrue(FString::Printf(TEXT("%s: a binary send is accepted"), Scheme.Name), Events.Connection->Send(FVaRestSocketFrame::Binary(Raw)));
		Events.Connection->Close(1000, TEXT("done"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		if (!TestEqual(FString::Printf(TEXT("%s: two messages reached the server"), Scheme.Name), Messages.Num(), 2))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: the text first, as sent"), Scheme.Name), Messages[0][TEXT("kind")].AsString().Equals(TEXT("text"), ESearchCase::CaseSensitive)
			&& Messages[0][TEXT("text")].AsString().Equals(TEXT("hello"), ESearchCase::CaseSensitive));
		TestTrue(FString::Printf(TEXT("%s: then the binary"), Scheme.Name), Messages[1][TEXT("kind")].AsString().Equals(TEXT("binary"), ESearchCase::CaseSensitive) && Messages[1][TEXT("len")].AsInt() == 3);
		TestEqual(FString::Printf(TEXT("%s: then our close"), Scheme.Name), JournalCloseCode(Journal), 1000);
		TestEqual(FString::Printf(TEXT("%s: nothing left buffered"), Scheme.Name), Events.Connection->GetBufferedAmount(), static_cast<int64>(0));
		// Task 3 review I-1: the mark is decided on what FHandle::Send's own add produced, carried to FConn::Send; the
		// writes' releases then bring the amount back to 0 before our close goes out
		TestTrue(FString::Printf(TEXT("%s: the sends past the high-water mark drained (%s)"), Scheme.Name, *Events.Describe()), Events.Count(EVaRestSocketEventType::Drained) >= 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectReleaseTest, "VaRest.Integration.WebSocket.Transport.Connect.Release", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectReleaseTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	for (const FScheme& Scheme : Schemes(Site))
	{
		// An open connection whose last handle goes is closed with 1001 "released" (spec 1.4)
		const FString OpenKey = NewKey();
		TUniquePtr<FEvents> Opened = MakeUnique<FEvents>(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + OpenKey)));
		TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Opened->WaitType(EVaRestSocketEventType::Open));
		Opened.Reset();
		FString Reason;
		const FVaRestJson OpenJournal = FetchJournal(*this, Site, OpenKey);
		TestEqual(FString::Printf(TEXT("%s: the server got 1001"), Scheme.Name), JournalCloseCode(OpenJournal, &Reason), 1001);
		TestEqualSensitive(FString::Printf(TEXT("%s: ... \"released\""), Scheme.Name), Reason, FString(TEXT("released")));
		// A pending one is killed: the server sees its TCP end
		const FString PendingKey = NewKey();
		TUniquePtr<FEvents> Pending = MakeUnique<FEvents>(Transport->Connect(Request(Scheme, TEXT("/ws/hang?ms=20000&key=") + PendingKey)));
		// FetchJournal pumps by itself (PumpUntil is not re-entrant): a plain loop, each fetch waiting up to 200 ms
		bool bSent = false;
		const double Until = FPlatformTime::Seconds() + 10.0;
		while (!bSent && FPlatformTime::Seconds() < Until)
		{
			bSent = FetchJournal(*this, Site, PendingKey, 1, 200)[TEXT("connections")].AsInt() == 1;
		}
		TestTrue(FString::Printf(TEXT("%s: the upgrade reached the server"), Scheme.Name), bSent);
		Pending.Reset();
		TestTrue(FString::Printf(TEXT("%s: the released attempt ended at the server"), Scheme.Name), FetchJournal(*this, Site, PendingKey, 1, 5000)[TEXT("ended")].AsBool());
	}
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	TestTrue(TEXT("the pending ones were killed"), Transport->GetCounters().KillsIssued >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectShutdownTest, "VaRest.Integration.WebSocket.Transport.Connect.Shutdown", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectShutdownTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	const TArray<FScheme> Both = Schemes(Site);
	FEvents Pending(Transport->Connect(Request(Both[0], TEXT("/ws/hang?ms=20000&key=") + NewKey())));
	const TSharedRef<FSinkLog, ESPMode::ThreadSafe> OpenLog = MakeShared<FSinkLog, ESPMode::ThreadSafe>();
	FEvents Open(Transport->Connect(Request(Both[0], TEXT("/ws/echo?key=") + NewKey()), [OpenLog] { ++OpenLog->Calls; OpenLog->OnGameThread += IsInGameThread() ? 1 : 0; }));
	TestTrue(TEXT("one opened"), Open.WaitType(EVaRestSocketEventType::Open));
	const int32 GameThreadCallsBefore = OpenLog->OnGameThread.load();
	Transport->Shutdown();
	// Task 6 review M-2: with the thread joined, FConn runs on the game thread -- its Shutdown end is queued and notified
	// there, inside Shutdown (IVaRestSocketTransport::Connect), and lws_context_destroy's callbacks arrive there after it
	TestEqual(TEXT("the open one's end was notified on the game thread, inside Shutdown"), OpenLog->OnGameThread.load() - GameThreadCallsBefore, 1);
	Pending.Pull();
	Open.Pull();
	const FVaRestSocketEvent* PendingEnd = Pending.GetEnd();
	const FVaRestSocketEvent* OpenEnd = Open.GetEnd();
	TestTrue(FString::Printf(TEXT("the pending one: Error Shutdown (%s)"), *Pending.Describe()), PendingEnd && PendingEnd->Type == EVaRestSocketEventType::Error && PendingEnd->End.Failure == EVaRestSocketFailure::Shutdown);
	TestTrue(FString::Printf(TEXT("the open one: Closed 1006, Shutdown (%s)"), *Open.Describe()), OpenEnd && OpenEnd->Type == EVaRestSocketEventType::Closed && OpenEnd->End.Failure == EVaRestSocketFailure::Shutdown);
	FEvents After(Transport->Connect(Request(Both[0], TEXT("/ws/echo?key=") + NewKey())));
	TestTrue(TEXT("a connect after Shutdown ends at once"), After.WaitEnd(2.0));
	TestTrue(FString::Printf(TEXT("... with Shutdown (%s)"), *After.Describe()), IsError(After, EVaRestSocketFailure::Shutdown));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectShutdownFlushTest, "VaRest.Integration.WebSocket.Transport.Connect.ShutdownFlush", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectShutdownFlushTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsConnectTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// ws only, so no CA; a flush long enough to outlast the held thread below
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	Options.ExitFlushSeconds = 5.0;
	const TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> Held = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
	ON_SCOPE_EXIT
	{
		Held->Shutdown();
	};
	const FScheme Plain = Schemes(Site)[0];
	const FString OpenKey = NewKey();
	FEvents Open(Held->Connect(Request(Plain, TEXT("/ws/echo?key=") + OpenKey)));
	if (!TestTrue(TEXT("one opened"), Open.WaitType(EVaRestSocketEventType::Open)))
	{
		Open.Connection->Close(1000, TEXT("done"));
		Open.WaitEnd();
		return true;
	}
	const TSharedRef<FServiceGate, ESPMode::ThreadSafe> Gate = MakeShared<FServiceGate, ESPMode::ThreadSafe>();
	FVaRestSocketConnectRequest Bad;
	Bad.Url = TEXT("ftp://127.0.0.1/");
	FEvents Holder(Held->Connect(MoveTemp(Bad), [Gate, WeakHeld = TWeakPtr<ILwsTestTransport, ESPMode::ThreadSafe>(Held)] { Gate->Hold(WeakHeld); }));
	TestTrue(TEXT("the service thread is held"), VaRest::Http::Testing::PumpUntil([Gate] { return Gate->bEntered.load(); }, 5.0, true));
	// Both run on the service thread after Shutdown has begun (FServiceGate)
	Open.Connection->Close(4001, TEXT("exit"));
	const FString LateKey = NewKey();
	FEvents Late(Held->Connect(Request(Plain, TEXT("/ws/echo?key=") + LateKey)));
	Held->Shutdown();
	Open.Pull();
	Late.Pull();
	Holder.Pull();
	TestTrue(TEXT("the held service thread went on once Shutdown had begun, not at its 10 s cap"), Gate->bSawShutdown.load());
	TestTrue(FString::Printf(TEXT("the holder ended BadUrl (%s)"), *Holder.Describe()), IsError(Holder, EVaRestSocketFailure::BadUrl));
	// Task 6 review M-7: FConn::Close is never gated on IsShuttingDown -- the close asked for just before Shutdown goes out in its flush
	FString Reason;
	TestEqual(TEXT("the close asked for just before Shutdown reached the server"), JournalCloseCode(FetchJournal(*this, Site, OpenKey), &Reason), 4001);
	TestEqualSensitive(TEXT("... with its reason"), Reason, FString(TEXT("exit")));
	TestEqual(FString::Printf(TEXT("... and that connection ended once (%s)"), *Open.Describe()),
		Open.Count(EVaRestSocketEventType::Closed) + Open.Count(EVaRestSocketEventType::Error), 1);
	// ... and a connect adopted once Shutdown has begun ends with Shutdown through FConn::Start's rule, never connecting
	const FVaRestSocketEvent* LateEnd = Late.GetEnd();
	TestTrue(FString::Printf(TEXT("a connect adopted during Shutdown: Error Shutdown, before the connect (%s)"), *Late.Describe()),
		LateEnd && LateEnd->Type == EVaRestSocketEventType::Error && LateEnd->End.Failure == EVaRestSocketFailure::Shutdown
		&& LateEnd->End.Detail.Contains(TEXT("before the connect"), ESearchCase::CaseSensitive));
	TestEqual(TEXT("... and never reached the server"), FetchJournal(*this, Site, LateKey, 1, 500)[TEXT("connections")].AsInt(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectCloseRulesTest, "VaRest.Integration.WebSocket.Transport.Connect.CloseRules", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectCloseRulesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(false);
	// Task 3 review I-2 and M-1: FHandle::Close sanitises through the shared MakeOutboundClose (spec 3.5), and its Warnings
	// name the connection by the link's label -- the request's LogName here. Game-thread lines, so the counts are exact
	AddExpectedMessagePlain(TEXT("chat#7: close code 999 cannot be sent (RFC 6455 7.4); 1000 is sent instead"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("chat#7: the close reason is cut to 123 bytes (RFC 6455 5.5)"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	const FString Key = NewKey();
	FVaRestSocketConnectRequest Req = Request(Schemes(Site)[0], TEXT("/ws/echo?key=") + Key);
	Req.LogName = TEXT("chat#7");
	FEvents Events(Transport->Connect(MoveTemp(Req)));
	if (!TestTrue(TEXT("opened"), Events.WaitType(EVaRestSocketEventType::Open)))
	{
		Events.Connection->Close(1000, TEXT("done"));
		Events.WaitEnd();
		return true;
	}
	Events.Connection->Close(999, FString::ChrN(200, TEXT('r')));
	TestTrue(TEXT("ended"), Events.WaitEnd());
	const FVaRestSocketEvent* End = Events.GetEnd();
	TestTrue(FString::Printf(TEXT("Closed 1000 by the client (%s)"), *Events.Describe()),
		End && End->Type == EVaRestSocketEventType::Closed && End->End.Code == 1000 && End->End.By == EVaRestSocketCloseBy::Client);
	FString Reason;
	TestEqual(TEXT("the server got 1000, not 999"), JournalCloseCode(FetchJournal(*this, Site, Key), &Reason), 1000);
	TestEqualSensitive(TEXT("... and the reason cut to 123 bytes"), Reason, FString::ChrN(123, TEXT('r')));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectDefaultTest, "VaRest.Integration.WebSocket.Transport.Connect.Default", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectDefaultTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// The process's transport (spec 2.1), ws only: it is never shut down here, so no wss may be in flight on it (VaRest-rti).
	// Its thread then lives until the process ends: an Automation RunTests ...; Quit run calls neither OnEnginePreExit nor
	// ShutdownModule (CLAUDE.md). That is the shape VaRest-x2v suspected for the probe's exit; noted there (review spec M-7)
	const TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> Default = VaRest::WebSocket::GetDefaultTransport();
	TestTrue(TEXT("one instance"), &Default.Get() == &VaRest::WebSocket::GetDefaultTransport().Get());
	FVaRestSocketConnectRequest Req;
	Req.Url = FString::Printf(TEXT("ws://%s:%d/ws/echo?key=%s"), *Site.Host, Site.Port, *NewKey());
	const TUniquePtr<FEvents> Events = Visit(*Default, MoveTemp(Req));
	TestNotNull(FString::Printf(TEXT("the default transport opens (%s)"), *Events->Describe()), Events->Find(EVaRestSocketEventType::Open));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectStartRefusalsTest, "VaRest.Integration.WebSocket.Transport.Connect.StartRefusals", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectStartRefusalsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsConnectTestPrivate;
	using VaRest::WebSocket::Testing::FLwsTransportOptions;
	using VaRest::WebSocket::Testing::MakeLwsTransport;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	AddExpectedErrorPlain(TEXT("the service thread could not start"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("VaRest wss handshake(s) are in flight"), EAutomationExpectedErrorFlags::Contains, 1);
	const FScheme Plain = Schemes(Site)[0];
	{
		// Review conc I-2: a transport whose start failed never makes a second context -- in a shared LWS copy a retry could
		// run under an engine wss handshake (VaRest-rti, ruling Q2)
		FLwsTransportOptions Options;
		Options.TcpAddressOverride = Site.Host;
		Options.bFailThreadForTest = true;
		const TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> Failing = MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT
		{
			Failing->Shutdown();
		};
		FEvents First(Failing->Connect(Request(Plain, TEXT("/ws/echo?key=") + NewKey())));
		FEvents Second(Failing->Connect(Request(Plain, TEXT("/ws/echo?key=") + NewKey())));
		TestTrue(TEXT("the first connect ends"), First.WaitEnd(2.0));
		TestTrue(TEXT("... and so does the second"), Second.WaitEnd(2.0));
		TestTrue(FString::Printf(TEXT("both with Shutdown (%s; %s)"), *First.Describe(), *Second.Describe()),
			IsError(First, EVaRestSocketFailure::Shutdown) && IsError(Second, EVaRestSocketFailure::Shutdown));
		TestEqual(TEXT("one lws_create_context for both connects"), Failing->GetCounters().ContextCreations, 1);
	}
	{
		// Review conc I-3: no context while a VaRest wss handshake is in flight; not latched, so the next connect opens
		FLwsTransportOptions Options;
		Options.TcpAddressOverride = Site.Host;
		const TSharedRef<ILwsTestTransport, ESPMode::ThreadSafe> Gated = MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT
		{
			Gated->Shutdown();
		};
		{
			VaRest::WebSocket::Testing::FScopedTlsHandshakeInFlight InFlight;
			FEvents Refused(Gated->Connect(Request(Plain, TEXT("/ws/echo?key=") + NewKey())));
			TestTrue(TEXT("a connect while a wss handshake is in flight ends"), Refused.WaitEnd(2.0));
			TestTrue(FString::Printf(TEXT("... with Shutdown (%s)"), *Refused.Describe()), IsError(Refused, EVaRestSocketFailure::Shutdown));
			TestEqual(TEXT("... and no context made"), Gated->GetCounters().ContextCreations, 0);
		}
		const TUniquePtr<FEvents> Later = Visit(*Gated, Request(Plain, TEXT("/ws/echo?key=") + NewKey()));
		TestNotNull(FString::Printf(TEXT("once none is in flight, the same transport opens (%s)"), *Later->Describe()), Later->Find(EVaRestSocketEventType::Open));
		TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Gated));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectTlsCountedTest, "VaRest.Integration.WebSocket.Transport.Connect.TlsHandshakeCounted", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectTlsCountedTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN(true);
	using VaRest::WebSocket::Testing::GetTlsHandshakesInFlight;
	const TArray<FScheme> Both = Schemes(Site);
	if (!TestEqual(TEXT("no wss attempt is in flight at the start"), GetTlsHandshakesInFlight(), 0))
	{
		return false;
	}
	// Review conc I-3: a wss attempt counts from its connect until LWS establishes or destroys it. Re-review N-2: from the
	// moment Connect returns, on the game thread, with no pumping
	FEvents Pending(Transport->Connect(Request(Both[1], TEXT("/ws/hang?ms=20000&key=") + NewKey())));
	TestEqual(TEXT("a wss attempt is counted as soon as Connect returns"), GetTlsHandshakesInFlight(), 1);
	TestTrue(TEXT("... and while it is pending"), VaRest::Http::Testing::PumpUntil([] { return GetTlsHandshakesInFlight() == 1; }, 5.0, true));
	const FString PlainKey = NewKey();
	FEvents Plain(Transport->Connect(Request(Both[0], TEXT("/ws/hang?ms=20000&key=") + PlainKey)));
	// Until the ws upgrade reached the server, so its Start has run. FetchJournal pumps by itself (PumpUntil is not
	// re-entrant): a plain loop, each fetch waiting up to 200 ms
	bool bSent = false;
	const double Until = FPlatformTime::Seconds() + 10.0;
	while (!bSent && FPlatformTime::Seconds() < Until)
	{
		bSent = FetchJournal(*this, Site, PlainKey, 1, 200)[TEXT("connections")].AsInt() == 1;
	}
	TestTrue(TEXT("the ws upgrade reached the server"), bSent);
	TestEqual(TEXT("a ws attempt is not counted"), GetTlsHandshakesInFlight(), 1);
	Pending.Connection->Close(1000, TEXT("cancel"));
	Plain.Connection->Close(1000, TEXT("cancel"));
	TestTrue(TEXT("both end"), Pending.WaitEnd() && Plain.WaitEnd());
	// The terminal event comes at the decision; the count only when LWS destroyed the wsi (decision 3)
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	TestEqual(TEXT("... and the count is back to 0"), GetTlsHandshakesInFlight(), 0);
	const TUniquePtr<FEvents> Opened = Visit(*Transport, Request(Both[1], TEXT("/ws/echo?key=") + NewKey()));
	TestNotNull(FString::Printf(TEXT("a wss attempt opens (%s)"), *Opened->Describe()), Opened->Find(EVaRestSocketEventType::Open));
	TestTrue(TEXT("every wsi is destroyed"), WaitIdle(*Transport));
	TestEqual(TEXT("an opened one leaves the count too"), GetTlsHandshakesInFlight(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsConnectLocalhostTest, "Probe.VaRest.Lws.Connect.LocalhostAddress", VAREST_WS_TEST_FLAGS)

bool FVaRestWsConnectLocalhostTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// A record, out of the default suite (Probe.*, review spec M-1): its outcome is the host resolver's. Ruling Q5: LWS 3.0
	// connects to the first address getaddrinfo returns (spec 2.2); localhost resolves to ::1 first here, the listener is
	// 127.0.0.1 only. The fallback is W1b B9 (VaRest-gi0); every other test sets the TCP address (FServiceOptions::
	// TcpAddressOverride) and keeps localhost as the Host and TLS name. No TCP override here, no CA
	const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Plain = VaRest::WebSocket::Testing::MakeLwsTransport(VaRest::WebSocket::Testing::FLwsTransportOptions());
	ON_SCOPE_EXIT
	{
		Plain->Shutdown();
	};
	FVaRestSocketConnectRequest Req;
	Req.Url = FString::Printf(TEXT("ws://localhost:%d/ws/echo?key=%s"), Site.Port, *NewKey());
	const TUniquePtr<FEvents> Events = Visit(*Plain, MoveTemp(Req));
	AddInfo(FString::Printf(TEXT("ws://localhost against a 127.0.0.1-only listener: %s"), *Events->Describe()));
	TestNotNull(TEXT("it ended either way (drained)"), Events->GetEnd());
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
