// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// FConn's send bookkeeping without a socket: a connection that never started (no wsi), driven as the handle drives it; the
// classification of our close's end (ClassifyOurCloseEnd); the ConnectTimeout clamp (ClampConnectTimeout)

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestLwsConn.h"
#include "VaRestSocketLink.h"

#include <limits>

namespace VaRest::WebSocket::Lws::VaRestLwsConnTestPrivate
{
	FVaRestSocketFrame MakeFrame(int32 Count)
	{
		TArray<uint8> Payload;
		Payload.SetNumZeroed(Count);
		return FVaRestSocketFrame::Binary(Payload);
	}

	int32 CountType(const TArray<FVaRestSocketEvent>& Events, EVaRestSocketEventType Type)
	{
		int32 Out = 0;
		for (const FVaRestSocketEvent& Each : Events)
		{
			Out += Each.Type == Type ? 1 : 0;
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsConnSendTest, "VaRest.WebSocket.Lws.Conn.Send",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsConnSendTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket;
	using namespace VaRest::WebSocket::Lws;
	using namespace VaRest::WebSocket::Lws::VaRestLwsConnTestPrivate;
	// Never started: no context, no thread; FConn::Send makes no LWS call before the open
	FService Service{ FServiceOptions() };
	{
		// Task 3 review I-1, carried here: the app sent A (100 bytes) and B (100 bytes), and saw 200, over the 150 mark. The
		// service thread wrote and released A before it ran B's command, so a re-read of the count there finds 100 and
		// misses the crossing; the value B's own add produced does not
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link = MakeShared<FSocketLink, ESPMode::ThreadSafe>(nullptr, TEXT("ws#1"));
		FVaRestSocketConnectRequest Req;
		Req.SendHighWater = 150;
		FConn Conn(1, Link, MoveTemp(Req));
		TestEqualSensitive(TEXT("the connection's label is its link's"), Conn.GetLabel(), FString(TEXT("ws#1")));
		// The releases below are OnWriteable's, which writes only once the connection is open: its Open is on the link
		// first, as OnEstablished queues it (a Drained ahead of the Open is a transport bug, FSocketLink::Push)
		FVaRestSocketEvent Opened;
		Opened.Type = EVaRestSocketEventType::Open;
		Link->Push(MoveTemp(Opened));
		// A's command runs before B is sent: 100 there, under the mark either way
		const int64 AfterA = Link->BufferedBytes.fetch_add(100) + 100;
		Conn.Send(Service, MakeFrame(100), AfterA);
		// B's add on the game thread: the app now sees 200
		const int64 AfterB = Link->BufferedBytes.fetch_add(100) + 100;
		// A on the socket, as OnWriteable releases it, before the service thread runs B's command
		Link->ReleaseBuffered(100);
		Conn.Send(Service, MakeFrame(100), AfterB);
		// B on the socket
		Link->ReleaseBuffered(100);
		TArray<FVaRestSocketEvent> Events;
		Link->PopAll(Events);
		TestEqual(TEXT("back to 0 after a crossing the app saw: one Drained"), CountType(Events, EVaRestSocketEventType::Drained), 1);
		TestEqual(TEXT("... and the amount is 0"), Link->BufferedBytes.load(), static_cast<int64>(0));
	}
	{
		// An end drops what is queued from the buffered amount, with no Drained; a send after the end is dropped there too
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link = MakeShared<FSocketLink, ESPMode::ThreadSafe>(nullptr, TEXT("ws#2"));
		FVaRestSocketConnectRequest Req;
		Req.SendHighWater = 10;
		FConn Conn(2, Link, MoveTemp(Req));
		Conn.Send(Service, MakeFrame(50), Link->BufferedBytes.fetch_add(50) + 50);
		// Before the open: the attempt is cancelled (plan decision 5); with no wsi there is nothing to kill
		Conn.Close(Service, 1000, TArray<uint8>(), EVaRestSocketCloseBy::Client, EVaRestSocketFailure::None);
		TestEqual(TEXT("the end took the outbox's bytes out"), Link->BufferedBytes.load(), static_cast<int64>(0));
		Conn.Send(Service, MakeFrame(30), Link->BufferedBytes.fetch_add(30) + 30);
		TestEqual(TEXT("a send after the end leaves no bytes behind"), Link->BufferedBytes.load(), static_cast<int64>(0));
		TArray<FVaRestSocketEvent> Events;
		Link->PopAll(Events);
		TestEqual(TEXT("one event"), Events.Num(), 1);
		TestTrue(TEXT("... Error Canceled, By Client"), Events.Num() == 1 && Events[0].Type == EVaRestSocketEventType::Error
			&& Events[0].End.Failure == EVaRestSocketFailure::Canceled && Events[0].End.By == EVaRestSocketCloseBy::Client);
		TestEqual(TEXT("no Drained after an end, though 50 bytes had passed the mark"), CountType(Events, EVaRestSocketEventType::Drained), 0);
		TestTrue(TEXT("the link has ended"), Link->bEnded.load());
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsConnCloseAckTest, "VaRest.WebSocket.Lws.Conn.CloseAck",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsConnCloseAckTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	// Task 10 review M-3: the bound itself, with no server timing. LWS's close-ack timer ends an unanswered close only more
	// than LwsCloseAckSeconds after our close frame (ClassifyOurCloseEnd's comment), so an end before it is an answer
	struct FRow
	{
		const TCHAR* What;
		bool bCloseWritten;
		bool bPeerCloseRead;
		double Seconds;
		bool bClean;
		bool bAckTimedOut;
	};
	const FRow Rows[] = {
		{ TEXT("our close written, the end at 4.99 s: the answer came first"), true, false, 4.99, true, false },
		{ TEXT("our close written, the end at 5.0 s: LWS's timer, as far as VaRest can tell"), true, false, 5.0, false, true },
		{ TEXT("our close written, the end at 5.01 s"), true, false, 5.01, false, true },
		{ TEXT("our close written, the end at 0.01 s"), true, false, 0.01, true, false },
		{ TEXT("our close written and the peer's close read, at 6 s: answered, never the timer"), true, true, 6.0, true, false },
		{ TEXT("our close not written, the peer's close read (the crossing)"), false, true, 0.5, true, false },
		{ TEXT("our close not written, the peer's close read, at 6 s"), false, true, 6.0, true, false },
		{ TEXT("neither close went out: unclean, not the timer"), false, false, 0.5, false, false },
		{ TEXT("neither close went out, at 6 s"), false, false, 6.0, false, false },
	};
	for (const FRow& Row : Rows)
	{
		const FOurCloseEnd Got = ClassifyOurCloseEnd(Row.bCloseWritten, Row.bPeerCloseRead, Row.Seconds);
		TestTrue(FString::Printf(TEXT("%s: clean is %s"), Row.What, Row.bClean ? TEXT("true") : TEXT("false")), Got.bClean == Row.bClean);
		TestTrue(FString::Printf(TEXT("%s: the close-ack timer is %s"), Row.What, Row.bAckTimedOut ? TEXT("true") : TEXT("false")), Got.bAckTimedOut == Row.bAckTimedOut);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsConnConnectTimeoutClampTest, "VaRest.WebSocket.Lws.Conn.ConnectTimeoutClamp",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsConnConnectTimeoutClampTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::WebSocket::Lws;
	// Task 11 review M-3: the value FConn::Prepare keeps, which Timeout.Clamp (the Warning and the open) cannot see.
	// (0, MaxConnectTimeoutSeconds] stands; the rest is the context's 60 s backstop, with a Warning (plan decision 17)
	struct FRow
	{
		const TCHAR* What;
		double Asked;
		double Seconds;
		bool bWarn;
	};
	const FRow Rows[] = {
		{ TEXT("120 s, over the backstop"), 120.0, VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
		{ TEXT("60 s, the backstop itself"), 60.0, 60.0, false },
		{ TEXT("60.001 s, just over it"), 60.001, VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
		{ TEXT("0.5 s (Timeout.KillLatency's)"), 0.5, 0.5, false },
		{ TEXT("10 s, the default"), 10.0, 10.0, false },
		{ TEXT("0 s, not positive"), 0.0, VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
		{ TEXT("-1 s"), -1.0, VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
		{ TEXT("NaN"), std::numeric_limits<double>::quiet_NaN(), VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
		{ TEXT("+infinity"), std::numeric_limits<double>::infinity(), VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
		{ TEXT("-infinity"), -std::numeric_limits<double>::infinity(), VaRest::WebSocket::MaxConnectTimeoutSeconds, true },
	};
	for (const FRow& Row : Rows)
	{
		const FConnectTimeoutClamp Got = ClampConnectTimeout(Row.Asked);
		TestEqual(FString::Printf(TEXT("%s: %g s kept"), Row.What, Row.Seconds), Got.Seconds, Row.Seconds);
		TestTrue(FString::Printf(TEXT("%s: the Warning is %s"), Row.What, Row.bWarn ? TEXT("logged") : TEXT("not logged")), Got.bWarn == Row.bWarn);
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
