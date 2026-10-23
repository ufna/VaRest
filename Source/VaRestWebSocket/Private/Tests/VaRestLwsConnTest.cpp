// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// FConn's send bookkeeping without a socket: a connection that never started (no wsi), driven as the handle drives it

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "VaRestLwsConn.h"
#include "VaRestSocketLink.h"

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

#endif // WITH_DEV_AUTOMATION_TESTS
