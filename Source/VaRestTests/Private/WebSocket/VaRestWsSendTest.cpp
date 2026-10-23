// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: fragmented and large sends, frames with a payload offset (SW-4), the buffered amount and Drained, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsSendTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	struct FFrameSeen
	{
		int32 Op = -1;
		int32 Fin = -1;
		int32 Len = -1;
	};

	/** The client's data frames (op 0, 1, 2) as the server journaled them */
	TArray<FFrameSeen> DataFrames(const FVaRestJson& Journal)
	{
		TArray<FFrameSeen> Out;
		for (const FVaRestJson& Each : JournalEvents(Journal, TEXT("frame")))
		{
			const int32 Op = Each[TEXT("op")].AsInt(-1);
			if (Op >= 0 && Op <= 2)
			{
				Out.Add({ Op, Each[TEXT("fin")].AsInt(-1), Each[TEXT("len")].AsInt(-1) });
			}
		}
		return Out;
	}

	TArray<uint8> Pattern(int32 Size)
	{
		TArray<uint8> Out;
		Out.SetNumUninitialized(Size);
		for (int32 Index = 0; Index < Size; ++Index)
		{
			Out[Index] = static_cast<uint8>((Index * 7) % 251);
		}
		return Out;
	}

	/** Opens Path on Scheme; false (and drained) when it did not open */
	bool OpenOn(ILwsTestTransport& Transport, const FScheme& Scheme, const FString& Path, TUniquePtr<FEvents>& Out, FVaRestSocketConnectRequest Req = FVaRestSocketConnectRequest())
	{
		Req.Url = Scheme.Base + Path;
		Out = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
		if (Out->WaitType(EVaRestSocketEventType::Open))
		{
			return true;
		}
		Out->WaitEnd();
		return false;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsSendTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendFragmentsTest, "VaRest.Integration.WebSocket.Transport.Send.Fragments", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendFragmentsTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		// 300000 bytes: four 64 KiB fragments and a fifth of 37856 (spec 2.4, 15)
		const FString Text = FString::ChrN(300000, TEXT('a'));
		FVaRestSocketFrame Frame = FVaRestSocketFrame::Text(FStringView(Text));
		const FString Expected = Sha256Hex(Frame.GetPayload());
		Events->Connection->Send(MoveTemp(Frame));
		Events->Connection->Close(1000, TEXT("done"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events->WaitEnd());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FFrameSeen> Frames = DataFrames(Journal);
		if (!TestEqual(FString::Printf(TEXT("%s: five frames on the wire"), Scheme.Name), Frames.Num(), 5))
		{
			continue;
		}
		const int32 Lens[] = { 65536, 65536, 65536, 65536, 37856 };
		for (int32 Index = 0; Index < 5; ++Index)
		{
			TestEqual(FString::Printf(TEXT("%s: frame %d is %s"), Scheme.Name, Index, Index == 0 ? TEXT("TEXT") : TEXT("CONTINUATION")), Frames[Index].Op, Index == 0 ? 1 : 0);
			TestEqual(FString::Printf(TEXT("%s: frame %d's FIN"), Scheme.Name, Index), Frames[Index].Fin, Index == 4 ? 1 : 0);
			TestEqual(FString::Printf(TEXT("%s: frame %d's length"), Scheme.Name, Index), Frames[Index].Len, Lens[Index]);
		}
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		TestTrue(FString::Printf(TEXT("%s: one message, byte for byte"), Scheme.Name), Messages.Num() == 1 && Messages[0][TEXT("sha256")].AsString().Equals(Expected, ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendOffsetTest, "VaRest.Integration.WebSocket.Transport.Send.Offset", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendOffsetTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		// SW-4: an encoder that kept a 40-byte gap (0xAA) before its payload
		TArray<uint8> Storage;
		Storage.Init(0xAA, 40);
		for (const TCHAR C : FString(TEXT("offset-ok")))
		{
			Storage.Add(static_cast<uint8>(C));
		}
		TOptional<FVaRestSocketFrame> Frame = FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Text, MoveTemp(Storage), 40, 9);
		// A long one too, so a fragment after the first writes its header into the bytes before it (plan decision 6)
		TArray<uint8> Long;
		Long.Init(0xAA, 100);
		Long.Append(Pattern(200000));
		TOptional<FVaRestSocketFrame> LongFrame = FVaRestSocketFrame::FromStorage(EVaRestSocketFrameKind::Binary, MoveTemp(Long), 100, 200000);
		if (!TestTrue(TEXT("both frames are taken"), Frame.IsSet() && LongFrame.IsSet()))
		{
			Events->Connection->Close(1000, TEXT("done"));
			Events->WaitEnd();
			continue;
		}
		const FString LongSha = Sha256Hex(Pattern(200000));
		// The binary first (Task 8 review M-5): a text written from the gap is invalid UTF-8 (0xAA), which ws.py closes with
		// 1007 (ws.py serve's strict_utf8) -- sent first, it would end the connection before the long frame's row could fail
		Events->Connection->Send(MoveTemp(LongFrame.GetValue()));
		Events->Connection->Send(MoveTemp(Frame.GetValue()));
		Events->Connection->Close(1000, TEXT("done"));
		Events->WaitEnd();
		const TArray<FVaRestJson> Messages = JournalEvents(FetchJournal(*this, Site, Key), TEXT("message"));
		if (!TestEqual(FString::Printf(TEXT("%s: two messages"), Scheme.Name), Messages.Num(), 2))
		{
			continue;
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: a long frame at an offset, byte for byte"), Scheme.Name), Messages[0][TEXT("sha256")].AsString(), LongSha);
		TestEqualSensitive(FString::Printf(TEXT("%s: the payload from its offset, not the gap"), Scheme.Name), Messages[1][TEXT("text")].AsString(), FString(TEXT("offset-ok")));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendBufferedTest, "VaRest.Integration.WebSocket.Transport.Send.BufferedAndDrained", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendBufferedTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		FVaRestSocketConnectRequest Req;
		Req.SendHighWater = 256 * 1024;
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events, MoveTemp(Req))))
		{
			continue;
		}
		const TArray<uint8> Megabyte = Pattern(1024 * 1024);
		const FString Expected = Sha256Hex(Megabyte);
		for (int32 Index = 0; Index < 8; ++Index)
		{
			Events->Connection->Send(FVaRestSocketFrame::Binary(Megabyte));
		}
		TestTrue(FString::Printf(TEXT("%s: the sends are buffered"), Scheme.Name), Events->Connection->GetBufferedAmount() > 0);
		// Review conc I-6: the service thread may write the first megabyte out before the game thread adds the second, and
		// that early Drained is right (spec 2.4). So the wait is for a Drained with nothing left buffered, not for the first
		const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> Connection = Events->Connection;
		const bool bSettled = Events->WaitFor([Connection](const TArray<FVaRestSocketEvent>& All)
		{
			return Connection->GetBufferedAmount() == 0 && All.ContainsByPredicate([](const FVaRestSocketEvent& Each) { return Each.Type == EVaRestSocketEventType::Drained; });
		}, 30.0);
		TestTrue(FString::Printf(TEXT("%s: Drained after the high-water mark, and nothing buffered then (spec 2.4) (%s)"), Scheme.Name, *Events->Describe()), bSettled);
		TestTrue(FString::Printf(TEXT("%s: at least one Drained"), Scheme.Name), Events->Count(EVaRestSocketEventType::Drained) >= 1);
		Events->Connection->Close(1000, TEXT("done"));
		Events->WaitEnd();
		const TArray<FVaRestJson> Messages = JournalEvents(FetchJournal(*this, Site, Key), TEXT("message"));
		bool bAllIntact = Messages.Num() == 8;
		for (const FVaRestJson& Each : Messages)
		{
			bAllIntact = bAllIntact && Each[TEXT("len")].AsInt() == 1024 * 1024 && Each[TEXT("sha256")].AsString().Equals(Expected, ESearchCase::CaseSensitive);
		}
		TestTrue(FString::Printf(TEXT("%s: eight 1 MiB messages arrived intact (got %d)"), Scheme.Name, Messages.Num()), bAllIntact);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendBeforeOpenTest, "VaRest.Integration.WebSocket.Transport.Send.BeforeOpen", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendBeforeOpenTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		// Plan decision 18: queued before the open and written after it -- LWS drops a WRITEABLE request made during the handshake (spec 2.4)
		FVaRestSocketConnectRequest Req = Request(Scheme, TEXT("/ws/sink?key=") + Key);
		// Task 3 residual: the send's 5 bytes reach the high-water mark before the open, so its write is a Drained
		Req.SendHighWater = 5;
		const TSharedRef<FCallbackGate, ESPMode::ThreadSafe> Gate = MakeShared<FCallbackGate, ESPMode::ThreadSafe>();
		ON_SCOPE_EXIT { Gate->bRelease = true; };
		FVaRestSocketConnectRequest Blocker;
		Blocker.Url = TEXT("ftp://127.0.0.1/");
		FEvents Blocked(Transport->Connect(MoveTemp(Blocker), [Gate] { Gate->Hold(); }));
		if (!TestTrue(TEXT("service gate entered before target Connect"), Gate->WaitEntered()))
		{
			Gate->bRelease = true;
			Blocked.WaitEnd();
			return false;
		}
		FEvents Events(Transport->Connect(MoveTemp(Req)));
		// Independent HTTP work deliberately lets the game thread arrive late while the service remains held.
		TestEqual(TEXT("the held target has sent no request"), FetchJournal(*this, Site, Key, 1, 100)[TEXT("connections")].AsInt(), 0);
		TestTrue(FString::Printf(TEXT("%s: a send before the open is accepted"), Scheme.Name), Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("early")))));
		TestTrue(TEXT("service gate released normally"), Gate->ReleaseAndWait());
		TestTrue(TEXT("gate connection drained"), Blocked.WaitEnd());
		TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open));
		// ... and nothing comes ahead of the Open (IVaRestSocketConnection): its Drained follows it on the link's one queue.
		// Waited for before the Close: the close's own WRITEABLE request writes the queued send too, so only a write before
		// it shows the re-request at CLIENT_ESTABLISHED (VaRestLwsConn.cpp:242-245)
		TestTrue(FString::Printf(TEXT("%s: the send is Drained once written (spec 2.4)"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Drained));
		const TArray<FVaRestSocketEvent>& Seen = Events.GetAll();
		TestTrue(FString::Printf(TEXT("%s: the first event is the Open, and no Drained precedes it (%s)"), Scheme.Name, *Events.Describe()),
			Seen.Num() > 0 && Seen[0].Type == EVaRestSocketEventType::Open);
		// Review conc M-8: closed and ended first, so the journal's default wait (for the end, ws.py:15) is deterministic
		Events.Connection->Close(1000, TEXT("done"));
		Events.WaitEnd();
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		TestTrue(FString::Printf(TEXT("%s: it went out after the open"), Scheme.Name), Messages.Num() == 1 && Messages[0][TEXT("text")].AsString().Equals(TEXT("early"), ESearchCase::CaseSensitive));
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendCloseAfterDataTest, "VaRest.Integration.WebSocket.Transport.Send.CloseAfterData", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendCloseAfterDataTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		Events->Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("one"))));
		Events->Connection->Send(FVaRestSocketFrame::Binary(Pattern(200000)));
		Events->Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("three"))));
		// Plan decision 18: the close goes after what was queued, even a fragmented message
		Events->Connection->Close(4321, TEXT("after"));
		TestTrue(FString::Printf(TEXT("%s: ended"), Scheme.Name), Events->WaitEnd());
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		TArray<FString> Order;
		for (const FVaRestJson& Each : Journal[TEXT("events")].Elements())
		{
			const FString Kind = Each[TEXT("ev")].AsString();
			if (Kind.Equals(TEXT("message"), ESearchCase::CaseSensitive) || Kind.Equals(TEXT("close"), ESearchCase::CaseSensitive))
			{
				Order.Add(Kind);
			}
		}
		TestEqualSensitive(FString::Printf(TEXT("%s: three messages, then the close"), Scheme.Name), FString::Join(Order, TEXT(",")), FString(TEXT("message,message,message,close")));
		TestEqual(FString::Printf(TEXT("%s: with our code"), Scheme.Name), JournalCloseCode(Journal), 4321);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsSendEdgesTest, "VaRest.Integration.WebSocket.Transport.Send.Edges", VAREST_WS_TEST_FLAGS)

bool FVaRestWsSendEdgesTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		const FString Key = NewKey();
		TUniquePtr<FEvents> Events;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), OpenOn(*Transport, Scheme, TEXT("/ws/sink?key=") + Key, Events)))
		{
			continue;
		}
		TestTrue(FString::Printf(TEXT("%s: an empty text is sent"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Text(FStringView())));
		TestTrue(FString::Printf(TEXT("%s: an empty binary is sent"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Binary(TConstArrayView<uint8>())));
		// Exactly one fragment's worth: one frame, FIN set
		TestTrue(FString::Printf(TEXT("%s: 64 KiB is sent"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Binary(Pattern(64 * 1024))));
		TestFalse(FString::Printf(TEXT("%s: an invalid frame is refused"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame()));
		Events->Connection->Close(1000, TEXT("done"));
		Events->WaitEnd();
		TestFalse(FString::Printf(TEXT("%s: a send after the end is refused"), Scheme.Name), Events->Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("late")))));
		const FVaRestJson Journal = FetchJournal(*this, Site, Key);
		const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
		TestTrue(FString::Printf(TEXT("%s: two empty messages, then 64 KiB"), Scheme.Name), Messages.Num() == 3 && Messages[0][TEXT("len")].AsInt() == 0
			&& Messages[1][TEXT("len")].AsInt() == 0 && Messages[2][TEXT("len")].AsInt() == 64 * 1024);
		const TArray<FFrameSeen> Frames = DataFrames(Journal);
		TestTrue(FString::Printf(TEXT("%s: 64 KiB is one final frame"), Scheme.Name), Frames.Num() == 3 && Frames[2].Fin == 1 && Frames[2].Len == 64 * 1024);
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
