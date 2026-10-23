// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: empty close -> 1005, close codes and UTF-8 reasons, unclean drop -> 1006, our close, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestHttpTesting.h"
#include "VaRestUrl.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsCloseTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** The connection's Closed event, or null */
	const FVaRestSocketEvent* Closed(const FEvents& Events)
	{
		const FVaRestSocketEvent* End = Events.GetEnd();
		return End && End->Type == EVaRestSocketEventType::Closed ? End : nullptr;
	}

	/** Until the transport holds no connection: LWS destroyed every wsi */
	bool WaitIdle(ILwsTestTransport& Transport, double Seconds = 5.0)
	{
		return VaRest::Http::Testing::PumpUntil([&Transport] { return Transport.GetCounters().LiveConnections == 0; }, Seconds, true);
	}

	/**
	 * Connects and waits for the server to end the connection, never closing first (Visit closes once it sees the Open): a
	 * Close that races the server's close or drop is reported as ours -- out first, it makes LWS take the server's close for
	 * its echo, which raises no callback [close.ours_sequence]. Closes with 1000 only when no end came within Seconds, so the
	 * connection always drains
	 */
	TUniquePtr<FEvents> UntilServerEnds(IVaRestSocketTransport& Transport, FVaRestSocketConnectRequest&& Req, double Seconds = 15.0)
	{
		TUniquePtr<FEvents> Events = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
		if (!Events->WaitEnd(Seconds))
		{
			Events->Connection->Close(1000, TEXT("the server did not end it"));
			Events->WaitEnd(Seconds);
		}
		return Events;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsCloseTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsClosePeerCodesTest, "VaRest.Integration.WebSocket.Transport.Close.PeerCodes", VAREST_WS_TEST_FLAGS)

bool FVaRestWsClosePeerCodesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const int32 Code : { 1000, 1001, 1003, 1011, 4001, 4999 })
		{
			const FString Key = NewKey();
			const TUniquePtr<FEvents> Events = UntilServerEnds(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/close/%d?reason=r%d&key=%s"), Code, Code, *Key)));
			const FVaRestSocketEvent* End = Closed(*Events);
			TestTrue(FString::Printf(TEXT("%s %d: Closed by the server, its code and reason, clean (%s)"), Scheme.Name, Code, *Events->Describe()),
				End && End->End.By == EVaRestSocketCloseBy::Server && End->End.Code == Code && End->End.bWasClean
				&& End->End.Reason.Equals(FString::Printf(TEXT("r%d"), Code), ESearchCase::CaseSensitive));
			// RFC 6455 5.5.1: VaRest returns 0 from PEER_INITIATED_CLOSE, so LWS answers [close.echo_on_return_0]
			TestTrue(FString::Printf(TEXT("%s %d: the client answered with a close"), Scheme.Name, Code), JournalCloseCode(FetchJournal(*this, Site, Key)) != -1);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsClosePeerOddTest, "VaRest.Integration.WebSocket.Transport.Close.PeerOdd", VAREST_WS_TEST_FLAGS)

bool FVaRestWsClosePeerOddTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Spec 16.5 item 5: an empty close frame is 1005, never two bytes of garbage (E27) [close.empty_len]
		const TUniquePtr<FEvents> Empty = UntilServerEnds(*Transport, Request(Scheme, TEXT("/ws/close/none?key=") + NewKey()));
		const FVaRestSocketEvent* EmptyEnd = Closed(*Empty);
		TestTrue(FString::Printf(TEXT("%s: an empty close is 1005, clean, no reason (%s)"), Scheme.Name, *Empty->Describe()),
			EmptyEnd && EmptyEnd->End.Code == 1005 && EmptyEnd->End.bWasClean && EmptyEnd->End.Reason.IsEmpty());
		const TUniquePtr<FEvents> One = UntilServerEnds(*Transport, Request(Scheme, TEXT("/ws/close/onebyte?key=") + NewKey()));
		const FVaRestSocketEvent* OneEnd = Closed(*One);
		// Task 10 review M-7: the Detail names the malformed payload (PeerCloseDetail's one-byte branch)
		TestTrue(FString::Printf(TEXT("%s: a one-byte close is 1002, the detail naming it (%s)"), Scheme.Name, *One->Describe()),
			OneEnd && OneEnd->End.Code == 1002 && OneEnd->End.Detail.Contains(TEXT("one byte"), ESearchCase::CaseSensitive));
		// Task 2 review M-4: a code normalised to 1005 is named in the Detail, as received -- a number, never the reason.
		// LWS 3.0 passes codes from 5000 up unchanged (it rewrites only those below 3000, [close.rewritten_codes])
		for (const int32 Sent : { 5000, 65535 })
		{
			const TUniquePtr<FEvents> Big = UntilServerEnds(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/close/%d?key=%s"), Sent, *NewKey())));
			const FVaRestSocketEvent* BigEnd = Closed(*Big);
			TestTrue(FString::Printf(TEXT("%s: %d, which a peer must not send, is 1005, the detail naming it (%s)"), Scheme.Name, Sent, *Big->Describe()),
				BigEnd && BigEnd->End.Code == 1005 && BigEnd->End.Detail.Contains(FString::Printf(TEXT("close code %d,"), Sent), ESearchCase::CaseSensitive)
				&& BigEnd->End.Detail.Contains(TEXT("reported as 1005"), ESearchCase::CaseSensitive));
		}
		// Plan decision 23: LWS 3.0 rewrites 1012 to 1002 before VaRest sees it [close.rewritten_codes]; pinned for W2 (VaRest-diy)
		const TUniquePtr<FEvents> Restart = UntilServerEnds(*Transport, Request(Scheme, TEXT("/ws/close/1012?key=") + NewKey()));
		const FVaRestSocketEvent* RestartEnd = Closed(*Restart);
		TestTrue(FString::Printf(TEXT("%s: a server's 1012 arrives as 1002 on LWS 3.0 (%s)"), Scheme.Name, *Restart->Describe()), RestartEnd && RestartEnd->End.Code == 1002);
		// close.reason_utf8_bytes: "пока ✓", percent-encoded into the query
		const FString Bye = TEXT("пока ✓");
		// Unicode, not Utf8: VaRest::WsTest::Utf8 is a function the using-directive brings in (C4459, review spec M-10)
		const TUniquePtr<FEvents> Unicode = UntilServerEnds(*Transport, Request(Scheme, FString::Printf(TEXT("/ws/close/4002?reason=%s&key=%s"), *VaRest::Http::PercentEncode(Bye), *NewKey())));
		const FVaRestSocketEvent* UnicodeEnd = Closed(*Unicode);
		TestTrue(FString::Printf(TEXT("%s: a UTF-8 reason arrives whole (%s)"), Scheme.Name, *Unicode->Describe()), UnicodeEnd && UnicodeEnd->End.Code == 4002 && UnicodeEnd->End.Reason.Equals(Bye, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsClosePeerBadUtf8Test, "VaRest.Integration.WebSocket.Transport.Close.PeerBadUtf8Reason", VAREST_WS_TEST_FLAGS)

bool FVaRestWsClosePeerBadUtf8Test::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Spec 2.5: ill-formed bytes become U+FFFD, with a warning (one per scheme). Service-thread lines, one at a time: the
	// second connection starts after the first has ended
	AddExpectedMessagePlain(TEXT("the server's close reason is not UTF-8"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const TUniquePtr<FEvents> Events = UntilServerEnds(*Transport, Request(Scheme, TEXT("/ws/badutf8?where=close&key=") + NewKey()));
		const FVaRestSocketEvent* End = Closed(*Events);
		TestTrue(FString::Printf(TEXT("%s: 1000 with the reason repaired (%s)"), Scheme.Name, *Events->Describe()),
			End && End->End.Code == 1000 && End->End.Reason.Equals(TEXT("�("), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseDropTest, "VaRest.Integration.WebSocket.Transport.Close.Drop", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseDropTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const TCHAR* How : { TEXT("0"), TEXT("1") })
		{
			// FIN and RST both give CLIENT_CLOSED with no close frame [close.drop_fin_sequence, close.drop_rst_sequence]. The
			// server drops only after our first message (on_message=1), which goes out after the Open: the drop always meets
			// an open connection. An RST right behind the 101 can discard it unread (seen on wss: "read failed" before the
			// open, NoUpgradeResponse [hs.no_upgrade_response]; bead VaRest-81s.1)
			FEvents Events(Transport->Connect(Request(Scheme, FString::Printf(TEXT("/ws/drop?on_message=1&rst=%s&key=%s"), How, *NewKey()))));
			const bool bOpened = Events.WaitType(EVaRestSocketEventType::Open);
			if (!TestTrue(FString::Printf(TEXT("%s rst=%s: opened (%s)"), Scheme.Name, How, *Events.Describe()), bOpened))
			{
				Events.WaitEnd();
				continue;
			}
			// Not Visit or a Close of ours: our close racing the drop would be reported as ours (UntilServerEnds' comment)
			TestTrue(FString::Printf(TEXT("%s rst=%s: the message that ends it is taken"), Scheme.Name, How), Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("drop now")))));
			if (!TestTrue(FString::Printf(TEXT("%s rst=%s: the server ended it"), Scheme.Name, How), Events.WaitEnd()))
			{
				Events.Connection->Close(1000, TEXT("the server did not end it"));
				Events.WaitEnd();
				continue;
			}
			const FVaRestSocketEvent* End = Closed(Events);
			TestTrue(FString::Printf(TEXT("%s rst=%s: 1006, by the transport, not clean (%s)"), Scheme.Name, How, *Events.Describe()),
				End && End->End.Code == 1006 && End->End.By == EVaRestSocketCloseBy::Transport && !End->End.bWasClean && End->End.Failure == EVaRestSocketFailure::None);
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseOursTest, "VaRest.Integration.WebSocket.Transport.Close.Ours", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseOursTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Spec 3.5: a code that may not be sent becomes 1000, a long reason is cut to 123 bytes; one Warning each, per scheme
	AddExpectedMessagePlain(TEXT("close code 1006 cannot be sent"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	AddExpectedMessagePlain(TEXT("the close reason is cut to 123 bytes"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 2);
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Sanitised = NewKey();
		FEvents Bad(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + Sanitised)));
		if (TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Bad.WaitType(EVaRestSocketEventType::Open)))
		{
			Bad.Connection->Close(1006, TEXT("x"));
			// Review conc M-6: refused at the call, not dropped later on the service thread
			TestFalse(FString::Printf(TEXT("%s: a send after Close is refused"), Scheme.Name), Bad.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("late")))));
		}
		Bad.WaitEnd();
		TestEqual(FString::Printf(TEXT("%s: the server got 1000"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, Sanitised)), 1000);

		const FString LongKey = NewKey();
		FEvents Long(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + LongKey)));
		if (TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Long.WaitType(EVaRestSocketEventType::Open)))
		{
			Long.Connection->Close(1000, FString::ChrN(200, TEXT('r')));
		}
		Long.WaitEnd();
		const TArray<FVaRestJson> Closes = JournalEvents(FetchJournal(*this, Site, LongKey), TEXT("close"));
		TestTrue(FString::Printf(TEXT("%s: the server got a 125-byte close payload (2 + 123)"), Scheme.Name), Closes.Num() >= 1 && Closes[0][TEXT("len")].AsInt() == 125);

		// Before the open: no close frame, the attempt is cancelled (plan decision 5)
		FEvents Pending(Transport->Connect(Request(Scheme, TEXT("/ws/hang?ms=20000&key=") + NewKey())));
		Pending.Connection->Close(1000, TEXT("never mind"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Pending.WaitEnd());
		const FVaRestSocketEvent* PendingEnd = Pending.GetEnd();
		TestTrue(FString::Printf(TEXT("%s: Error Canceled, by the client (%s)"), Scheme.Name, *Pending.Describe()), PendingEnd && PendingEnd->Type == EVaRestSocketEventType::Error
			&& PendingEnd->End.Failure == EVaRestSocketFailure::Canceled && PendingEnd->End.By == EVaRestSocketCloseBy::Client);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseCancelTest, "VaRest.Integration.WebSocket.Transport.Close.CancelSendsNothing", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseCancelTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	// Review conc I-1: an attempt the game cancels in the tick that started it never sends its upgrade request, headers and
	// all. Its kill lands at LWS's next timeout walk (up to about 1 s, decision 12) and LWS's own stage timers can overwrite
	// it meanwhile, while a loopback TCP connect or TLS handshake finishes in a few ms: the veto in APPEND_HANDSHAKE_HEADER
	// keeps the request off the wire
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/echo?key=") + Key);
		Req.Headers = { { TEXT("Authorization"), TEXT("Bearer cancel-test") } };
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		Events.Connection->Close(1000, TEXT("never mind"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events.WaitEnd());
		TestTrue(FString::Printf(TEXT("%s: Error Canceled (%s)"), Scheme.Name, *Events.Describe()), IsError(Events, EVaRestSocketFailure::Canceled));
		TestTrue(FString::Printf(TEXT("%s: LWS destroyed it within 2 s"), Scheme.Name), WaitIdle(*Transport, 2.0));
		TestEqual(FString::Printf(TEXT("%s: the server never saw the request"), Scheme.Name), FetchJournal(*this, Site, Key, 1, 1000)[TEXT("connections")].AsInt(), 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsCloseShutdownFlushTest, "VaRest.Integration.WebSocket.Transport.Close.ShutdownWritesQueuedClose", VAREST_WS_TEST_FLAGS)

bool FVaRestWsCloseShutdownFlushTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WsTest;
	using namespace VaRest::WsTest::VaRestWsCloseTestPrivate;
	ExpectLwsLines(*this);
	FSite Site;
	if (!FindSite(*this, Site))
	{
		return false;
	}
	// Review conc M-3: at engine exit the hub's 1001 closes (spec 1.4) are queued just before the transport shuts down.
	// Shutdown lets the service thread write them first (ExitFlushSeconds, 0.25 s). The close waits behind 8 MiB: about 128
	// service passes, one 64 KiB fragment each (re-review N-3: 2 MiB was only 1-3 times the about 1 ms a Shutdown that did
	// not wait takes to stop the thread; the server unmasks in one XOR, ws.py:193-199, so the real run stays far inside 0.25 s)
	for (const FScheme& Scheme : Schemes(Site))
	{
		// One transport per scheme, made after the previous one was shut down (VaRest-rti)
		const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Exiting = MakeTransport(*this, Site);
		if (!Exiting.IsValid())
		{
			return false;
		}
		ON_SCOPE_EXIT
		{
			Exiting->Shutdown();
		};
		const FString Key = NewKey();
		FEvents Events(Exiting->Connect(Request(Scheme, TEXT("/ws/sink?key=") + Key)));
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Events.WaitEnd();
			continue;
		}
		TArray<uint8> Payload;
		Payload.SetNumZeroed(8 * 1024 * 1024);
		Events.Connection->Send(FVaRestSocketFrame::Binary(Payload));
		Events.Connection->Close(1001, TEXT("shutdown"));
		Exiting->Shutdown();
		Events.Pull();
		TestNotNull(FString::Printf(TEXT("%s: ended by Shutdown (%s)"), Scheme.Name, *Events.Describe()), Events.GetEnd());
		TestEqual(FString::Printf(TEXT("%s: nothing is left unsent"), Scheme.Name), Exiting->GetCounters().UnsentCloses, 0);
		FString Reason;
		TestEqual(FString::Printf(TEXT("%s: the server got the 1001 asked for just before Shutdown"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, Key), &Reason), 1001);
		TestEqualSensitive(FString::Printf(TEXT("%s: ... with its reason"), Scheme.Name), Reason, FString(TEXT("shutdown")));
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
