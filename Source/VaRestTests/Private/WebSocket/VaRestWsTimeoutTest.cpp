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

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
