// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: close against a non-echoing server (CloseTimeout); Task 11 adds the connect timeout and the kill latency

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsTimeoutTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsTimeoutTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutCloseTest, "VaRest.Integration.WebSocket.Transport.Timeout.Close", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutCloseTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// FConn::Prepare's line, on the game thread inside Connect: one per scheme, exact
	AddExpectedMessagePlain(TEXT("CloseTimeout -1 s is not a positive number; 5 s is used"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/noecho?key=") + Key);
		Req.CloseTimeout = 1.0;
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		const double ClosedAt = FPlatformTime::Seconds();
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd(10.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		const double Took = End->Time - ClosedAt;
		// The server never echoes [close.ours_noecho_wait_ms: LWS alone waits 5526 ms]; CloseTimeout cuts it (plan decisions 3,
		// 17). Margins: 0.1 s below (the deadline is armed on the service thread, after the call), 0.6 s above (the service
		// wait ends at the deadline, ComputeWaitMs; the rest is the command's and the event's trip on a loaded box)
		TestTrue(FString::Printf(TEXT("%s: cut by CloseTimeout after %.2f s (1 s asked)"), Scheme.Name, Took), Took >= 0.9 && Took < 1.6);
		TestTrue(FString::Printf(TEXT("%s: Closed by the client, our code, not clean (%s)"), Scheme.Name, *Events.Describe()), End->Type == EVaRestSocketEventType::Closed
			&& End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && !End->End.bWasClean && End->End.Detail.Contains(TEXT("CloseTimeout"), ESearchCase::CaseSensitive));
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqual(FString::Printf(TEXT("%s: the server got our close"), Scheme.Name), JournalCloseCode(Journal), 4000);

		// Spec 15: a CloseTimeout that is not a positive number is LWS's own 5 s (plan decision 17), with a Warning. Taken as
		// given, -1 would cut the close at the first deadline check after it, before an echoing server's answer
		FVaRestSocketConnectRequest Negative = Request(Scheme, TEXT("/ws/echo?key=") + NewKey());
		Negative.CloseTimeout = -1.0;
		FEvents Echoed(Transport->Connect(MoveTemp(Negative)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened (CloseTimeout -1)"), Scheme.Name), Echoed.WaitType(EVaRestSocketEventType::Open)))
		{
			Echoed.WaitEnd();
			continue;
		}
		Echoed.Connection->Close(1000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended (CloseTimeout -1)"), Scheme.Name), Echoed.WaitEnd(10.0));
		const FVaRestSocketEvent* EchoedEnd = Echoed.GetEnd();
		TestTrue(FString::Printf(TEXT("%s: CloseTimeout -1 acts as 5 s: the echoed close ends clean (%s)"), Scheme.Name, *Echoed.Describe()),
			EchoedEnd && EchoedEnd->Type == EVaRestSocketEventType::Closed && EchoedEnd->End.Code == 1000 && EchoedEnd->End.bWasClean);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutCloseAckTest, "VaRest.Integration.WebSocket.Transport.Timeout.CloseLongerThanAck", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutCloseAckTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Review conc I-5: with CloseTimeout above LWS's fixed 5 s close-ack (T20), LWS ends the unanswered close itself, at about
	// 5.5 s [close.ours_noecho_wait_ms] -- and that end is the unanswered close CloseTimeout exists for: not clean
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/noecho?key=") + Key);
		Req.CloseTimeout = 8.0;
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		const double ClosedAt = FPlatformTime::Seconds();
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd(12.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		const double Took = End->Time - ClosedAt;
		// Margins: LWS's 5 s timer runs on whole-second timeout walks (T26), so 5.0-6.0 s plus the service wait (1 s cap);
		// 0.1 s below 5, and 7.5 s stays 0.5 s clear of the 8 s CloseTimeout, whose end would say CloseTimeout instead
		TestTrue(FString::Printf(TEXT("%s: ended by LWS's close-ack after %.2f s, before the 8 s CloseTimeout"), Scheme.Name, Took), Took >= 4.9 && Took <= 7.5);
		TestTrue(FString::Printf(TEXT("%s: Closed 4000 by the client, not clean, the detail naming the close-ack (%s)"), Scheme.Name, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Closed && End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && !End->End.bWasClean
			&& End->End.Detail.Contains(TEXT("close-ack"), ESearchCase::CaseSensitive));
		TestEqual(FString::Printf(TEXT("%s: the server got our close"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, Key)), 4000);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutCloseAnsweredLateTest, "VaRest.Integration.WebSocket.Transport.Timeout.CloseAnsweredLate", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutCloseAnsweredLateTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Task 9 review M1: LWS's close-ack timer ends an unanswered close only more than 5 s after it was armed, and it is armed
	// after VaRest reads CloseSentAt -- so a close the server answers 4.76 s after ours is an answered close, ended by the
	// answer: clean, no Detail. A margin under 5 s would misread it (Task 9's 0.25 s did, from 4.75 s). The classification
	// itself is VaRest.WebSocket.Lws.Conn.CloseAck's (Task 10 review M-3); this row checks its wiring. 4.76 s, not lower: the
	// server's wait starts after it read our close, so the end only comes later, and a restored 0.25 s margin must still fail
	// here
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/noecho?echo_after_ms=4760&key=") + Key);
		Req.CloseTimeout = 8.0;
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		const double ClosedAt = FPlatformTime::Seconds();
		Events.Connection->Close(4000, TEXT("bye"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd(12.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			continue;
		}
		const double Took = End->Time - ClosedAt;
		// The harness: the answer came 4.76 s after the server read our close, inside the 4.75-5.0 s window this row is about.
		// Past 5 s an end is no longer told apart from LWS's timer, and this row would say nothing
		TestTrue(FString::Printf(TEXT("%s: ended by the server's answer after %.2f s (4.76 s asked)"), Scheme.Name, Took), Took >= 4.75 && Took < 5.0);
		TestTrue(FString::Printf(TEXT("%s: Closed 4000 by the client, clean, no Detail (%s)"), Scheme.Name, *Events.Describe()),
			End->Type == EVaRestSocketEventType::Closed && End->End.By == EVaRestSocketCloseBy::Client && End->End.Code == 4000 && End->End.bWasClean
			&& End->End.Detail.IsEmpty());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TestEqual(FString::Printf(TEXT("%s: the server got our close"), Scheme.Name), JournalCloseCode(Journal), 4000);
		const TArray<FVaRestJson> Sent = JournalEvents(Journal, TEXT("sent"));
		TestEqual(FString::Printf(TEXT("%s: the server answered it with one close"), Scheme.Name),
			Sent.FilterByPredicate([](const FVaRestJson& Each) { return Each[TEXT("op")].AsInt() == 8; }).Num(), 1);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutConnectTest, "VaRest.Integration.WebSocket.Transport.Timeout.Connect", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutConnectTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/hang?ms=20000&key=") + NewKey());
		Req.ConnectTimeout = 1.0;
		const double Started = FPlatformTime::Seconds();
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		TestTrue(FString::Printf(TEXT("%s: ended within 5 s"), Scheme.Name), Events.WaitEnd(5.0));
		const FVaRestSocketEvent* End = Events.GetEnd();
		if (!TestNotNull(FString::Printf(TEXT("%s: a terminal event"), Scheme.Name), End))
		{
			Events.Connection->Close(1000, TEXT("done"));
			Events.WaitEnd(30.0);
			continue;
		}
		const double Took = End->Time - Started;
		// Measured on the service thread and reported at the deadline (spec 2.2, D32; plan decision 3), not at LWS's kill
		TestTrue(FString::Printf(TEXT("%s: reported at the deadline, %.3f s after the connect (1 s asked)"), Scheme.Name, Took), Took >= 0.95 && Took < 1.5);
		TestTrue(FString::Printf(TEXT("%s: Error ConnectTimeout, by Timeout, VaRest's own (%s)"), Scheme.Name, *Events.Describe()), End->Type == EVaRestSocketEventType::Error
			&& End->End.Failure == EVaRestSocketFailure::ConnectTimeout && End->End.By == EVaRestSocketCloseBy::Timeout && End->End.Detail.Contains(TEXT("VaRest's ConnectTimeout"), ESearchCase::CaseSensitive));
	}
	TestTrue(TEXT("the killed attempts are destroyed"), VaRest::Http::Testing::PumpUntil([&Transport] { return Transport->GetCounters().LiveConnections == 0; }, 5.0, true));
	const VaRest::WebSocket::Testing::FLwsCounters Counters = Transport->GetCounters();
	AddInfo(FString::Printf(TEXT("kill to WSI_DESTROY: last %.1f ms, longest %.1f ms (VaRest-wcv, 100 ms wait cap while a kill is in flight)"),
		Counters.LastKillToDestroySeconds * 1000.0, Counters.MaxKillToDestroySeconds * 1000.0));
	TestTrue(TEXT("both kills landed"), Counters.KillsLanded >= 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutClampTest, "VaRest.Integration.WebSocket.Transport.Timeout.Clamp", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutClampTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Plan decision 17: over the context's 60 s backstop, or not a positive number: 60 s, with a Warning (two values, two schemes)
	AddExpectedMessagePlain(TEXT("ConnectTimeout 120 s is outside (0, 60]"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("ConnectTimeout 0 s is outside (0, 60]"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const double Asked : { 120.0, 0.0 })
		{
			FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + NewKey());
			Req.ConnectTimeout = Asked;
			const TUniquePtr<FEvents> Events = Visit(*Transport, MoveTemp(Req));
			TestNotNull(FString::Printf(TEXT("%s %g s: still opens"), Scheme.Name, Asked), Events->Find(EVaRestSocketEventType::Open));
		}
		// A slow upgrade inside the deadline opens (/ws/slowupgrade waits before its 101)
		FVaRestSocketConnectRequest Slow = Request(Scheme, TEXT("/ws/slowupgrade?ms=300&key=") + NewKey());
		Slow.ConnectTimeout = 2.0;
		const TUniquePtr<FEvents> SlowEvents = Visit(*Transport, MoveTemp(Slow));
		TestNotNull(FString::Printf(TEXT("%s: a 300 ms upgrade opens within 2 s (%s)"), Scheme.Name, *SlowEvents->Describe()), SlowEvents->Find(EVaRestSocketEventType::Open));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutKillLatencyTest, "VaRest.Integration.WebSocket.Transport.Timeout.KillLatency", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutKillLatencyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// VaRest-wcv: LWS walks its timeouts only when the wall-clock second changed, on a return of lws_service (T26). Measured
	// with the short cap VaRest uses while a kill is in flight (plan decision 12), and, for the record, with the 1 s cap alone
	double Longest[2] = { -1.0, -1.0 };
	const int32 Caps[2] = { 100, 1000 };
	for (int32 Run = 0; Run < 2; ++Run)
	{
		VaRest::WebSocket::Testing::FLwsTransportOptions Options;
		Options.TcpAddressOverride = Site.Host;
		Options.ShortWaitCapMs = Caps[Run];
		const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Measured = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
		ON_SCOPE_EXIT
		{
			Measured->Shutdown();
		};
		// The kills measured are the deadline's: a handle dropped at the end of an iteration releases its attempt with a kill
		// of its own (spec 1.4), which this count tells apart (it passed with no connect deadline at all, Task 11 RED)
		int32 TimedOut = 0;
		for (int32 Index = 0; Index < 4; ++Index)
		{
			FVaRestSocketConnectRequest Req;
			Req.Url = FString::Printf(TEXT("ws://%s:%d/ws/hang?ms=20000&key=%s"), *Site.Host, Site.Port, *NewKey());
			Req.ConnectTimeout = 0.5;
			FEvents Events(Measured->Connect(MoveTemp(Req)));
			Events.WaitEnd(5.0);
			TimedOut += IsError(Events, EVaRestSocketFailure::ConnectTimeout) ? 1 : 0;
			VaRest::Http::Testing::PumpUntil([&Measured] { return Measured->GetCounters().LiveConnections == 0; }, 5.0, true);
		}
		TestEqual(FString::Printf(TEXT("wait cap %d ms: four attempts timed out"), Caps[Run]), TimedOut, 4);
		TestEqual(FString::Printf(TEXT("wait cap %d ms: every kill landed"), Caps[Run]), Measured->GetCounters().KillsLanded, 4);
		Longest[Run] = Measured->GetCounters().MaxKillToDestroySeconds;
		AddInfo(FString::Printf(TEXT("wait cap %d ms while a kill is in flight: longest kill to WSI_DESTROY over 4 kills %.1f ms"), Caps[Run], Longest[Run] * 1000.0));
	}
	TestTrue(FString::Printf(TEXT("with VaRest's 100 ms cap a kill lands within 1.6 s (%.1f ms)"), Longest[0] * 1000.0), Longest[0] >= 0.0 && Longest[0] < 1.6);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsTimeoutKillLatencyBusyTest, "VaRest.Integration.WebSocket.Transport.Timeout.KillLatencyBusy", VAREST_WS_TEST_FLAGS)

bool FVaRestWsTimeoutKillLatencyBusyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site, false))
	{
		return false;
	}
	// Re-review N-1: kills beside a busy neighbour on the same service. The neighbour's send to /ws/sink keeps the service
	// thread in WRITEABLE callbacks (each masks and writes 64 KiB) while four attempts time out one after another. A kill
	// re-armed on every pass is stamped with the new second whenever a second boundary falls inside a callback, so the walk of
	// that second keeps the wsi and the kill moves on: past decision 12's 1.6 s. The game thread tops the send up while it
	// waits, so the neighbour stays busy for every kill (32 MiB alone would drain in tens of milliseconds)
	VaRest::WebSocket::Testing::FLwsTransportOptions Options;
	Options.TcpAddressOverride = Site.Host;
	const TSharedRef<VaRest::WebSocket::Testing::ILwsTestTransport, ESPMode::ThreadSafe> Busy = VaRest::WebSocket::Testing::MakeLwsTransport(MoveTemp(Options));
	ON_SCOPE_EXIT
	{
		Busy->Shutdown();
	};
	FVaRestSocketConnectRequest SinkRequest;
	SinkRequest.Url = FString::Printf(TEXT("ws://%s:%d/ws/sink?key=%s"), *Site.Host, Site.Port, *NewKey());
	FEvents Sink(Busy->Connect(MoveTemp(SinkRequest)));
	if (!TestTrue(TEXT("the busy neighbour opened"), Sink.WaitType(EVaRestSocketEventType::Open)))
	{
		Sink.WaitEnd();
		return false;
	}
	TArray<uint8> Megabyte;
	Megabyte.SetNumZeroed(1024 * 1024);
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Neighbour = Sink.Connection;
	int64 SentBytes = 0;
	// Game thread, from the waits below: keeps at least 4 MiB queued, in 1 MiB frames
	const auto TopUp = [&Neighbour, &Megabyte, &SentBytes]
	{
		while (Neighbour->GetBufferedAmount() < 4 * 1024 * 1024 && Neighbour->Send(FVaRestSocketFrame::Binary(Megabyte)))
		{
			SentBytes += Megabyte.Num();
		}
	};
	TopUp();
	int32 TimedOut = 0;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FVaRestSocketConnectRequest Req;
		Req.Url = FString::Printf(TEXT("ws://%s:%d/ws/hang?ms=20000&key=%s"), *Site.Host, Site.Port, *NewKey());
		Req.ConnectTimeout = 0.5;
		FEvents Events(Busy->Connect(MoveTemp(Req)));
		Events.WaitFor([&TopUp](const TArray<FVaRestSocketEvent>& All)
		{
			TopUp();
			return All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.IsTerminal(); });
		}, 5.0);
		TimedOut += IsError(Events, EVaRestSocketFailure::ConnectTimeout) ? 1 : 0;
		// Until LWS destroyed the killed one; the neighbour stays
		VaRest::Http::Testing::PumpUntil([&TopUp, &Busy] { TopUp(); return Busy->GetCounters().LiveConnections == 1; }, 5.0, true);
	}
	const VaRest::WebSocket::Testing::FLwsCounters Counters = Busy->GetCounters();
	AddInfo(FString::Printf(TEXT("beside a busy send (%lld MiB queued in all): longest kill to WSI_DESTROY %.1f ms over %d kills"),
		SentBytes / (1024 * 1024), Counters.MaxKillToDestroySeconds * 1000.0, Counters.KillsLanded));
	TestEqual(TEXT("four attempts timed out"), TimedOut, 4);
	TestEqual(TEXT("every kill landed"), Counters.KillsLanded, 4);
	TestTrue(FString::Printf(TEXT("beside a busy neighbour a kill still lands within 1.6 s (%.1f ms)"), Counters.MaxKillToDestroySeconds * 1000.0),
		Counters.MaxKillToDestroySeconds >= 0.0 && Counters.MaxKillToDestroySeconds < 1.6);
	TestTrue(TEXT("the neighbour was still sending at the end"), Neighbour->GetBufferedAmount() > 0 && !Neighbour->HasEnded());
	Neighbour->Close(1000, TEXT("done"));
	Sink.WaitEnd(30.0);
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
