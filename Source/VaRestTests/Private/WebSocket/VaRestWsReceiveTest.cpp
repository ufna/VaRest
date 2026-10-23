// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.2: fragmented and large text (Cyrillic split inside a code point and at 4096 bytes), binary, interleaved
// order, invalid UTF-8 -> 1007, size limit -> 1009, ws and wss

#if WITH_DEV_AUTOMATION_TESTS

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "VaRestJsonUtf8.h"
#include "VaRestWsTestUtil.h"

namespace VaRest::WsTest::VaRestWsReceiveTestPrivate
{
	using VaRest::WebSocket::Testing::ILwsTestTransport;

	/** Any thread (pure): the server's "message_sent" event: what /ws/fragment and /ws/large sent (kind, len, sha256) */
	FVaRestJson MessageSent(const FVaRestJson& Journal)
	{
		const TArray<FVaRestJson> Sent = JournalEvents(Journal, TEXT("message_sent"));
		return Sent.Num() > 0 ? Sent[0] : FVaRestJson();
	}

	/** Game thread: closes and observes the terminal event; a timeout still drains through Shutdown before returning */
	void Drain(FAutomationTestBase& Test, ILwsTestTransport& Transport, FEvents& Events)
	{
		if (!Events.GetEnd())
		{
			Events.Connection->Close(1000, TEXT("done"));
		}
		if (!Test.TestTrue(FString::Printf(TEXT("connection drained (%s)"), *Events.Describe()), Events.WaitEnd()))
		{
			Transport.Shutdown();
			Events.Pull();
			Test.TestNotNull(TEXT("Shutdown supplies the terminal event"), Events.GetEnd());
		}
	}

	/** Game thread: opens Path, waits for Count messages or the end, closes, waits for the end */
	TUniquePtr<FEvents> Receive(FAutomationTestBase& Test, ILwsTestTransport& Transport, const FScheme& Scheme, const FString& Path, int32 Count, FVaRestSocketConnectRequest Req = FVaRestSocketConnectRequest())
	{
		Req.Url = Scheme.Base + Path;
		TUniquePtr<FEvents> Events = MakeUnique<FEvents>(Transport.Connect(MoveTemp(Req)));
		Events->WaitMessages(Count, 20.0);
		Drain(Test, Transport, *Events);
		return Events;
	}

	/** Game thread: one message that is the server's message_sent, byte for byte: kind, length, SHA-256 */
	bool SameAsSent(FAutomationTestBase& Test, const TCHAR* What, const FEvents& Events, const FVaRestJson& Journal, EVaRestSocketFrameKind Kind)
	{
		const TArray<const FVaRestSocketEvent*> Messages = Events.GetMessages();
		const FVaRestJson Sent = MessageSent(Journal);
		if (!Test.TestEqual(FString::Printf(TEXT("%s: one message (%s)"), What, *Events.Describe()), Messages.Num(), 1) || !Test.TestTrue(FString::Printf(TEXT("%s: the server said what it sent"), What), Sent.IsValid()))
		{
			return false;
		}
		const FVaRestSocketEvent& Message = *Messages[0];
		bool bSame = Test.TestTrue(FString::Printf(TEXT("%s: its kind"), What), Message.Kind == Kind);
		bSame &= Test.TestEqual(FString::Printf(TEXT("%s: its length"), What), Message.Bytes.Num(), Sent[TEXT("len")].AsInt());
		bSame &= Test.TestEqualSensitive(FString::Printf(TEXT("%s: byte for byte"), What), Sha256Hex(Message.Bytes), Sent[TEXT("sha256")].AsString());
		bSame &= Test.TestTrue(FString::Printf(TEXT("%s: a slack byte for ParseOwned (core 2.1)"), What), Message.Bytes.Max() > Message.Bytes.Num());
		return bSame;
	}
}

#define VAREST_WS_TEST_FLAGS (EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)
#define VAREST_WS_BEGIN() \
	using namespace VaRest::WsTest; \
	using namespace VaRest::WsTest::VaRestWsReceiveTestPrivate; \
	ExpectLwsLines(*this); \
	FSite Site; \
	if (!FindSite(*this, Site)) { return false; } \
	const TSharedPtr<ILwsTestTransport, ESPMode::ThreadSafe> Transport = MakeTransport(*this, Site); \
	if (!Transport.IsValid()) { return false; } \
	ON_SCOPE_EXIT { Transport->Shutdown(); }

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveSplitTest, "VaRest.Integration.WebSocket.Transport.Receive.SplitCodePoint", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveSplitTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Spec 16.5 item 6, E9: a frame boundary inside a two-byte letter [rx.frames_split_utf8]
		const FString Inside = NewKey();
		const TUniquePtr<FEvents> Split = Receive(*this, *Transport, Scheme, TEXT("/ws/fragment?cyrillic=5120&split_inside=4096&key=") + Inside, 1);
		if (SameAsSent(*this, *FString::Printf(TEXT("%s, cut inside a code point"), Scheme.Name), *Split, FetchJournal(*this, Site, Inside), EVaRestSocketFrameKind::Text))
		{
			TestFalse(FString::Printf(TEXT("%s: no U+FFFD anywhere"), Scheme.Name), Utf8(Split->GetMessages()[0]->Bytes).Contains(TEXT("�"), ESearchCase::CaseSensitive));
		}
		// ... and exactly at 4096 bytes
		const FString At = NewKey();
		const TUniquePtr<FEvents> Exact = Receive(*this, *Transport, Scheme, TEXT("/ws/fragment?cyrillic=8192&at=4096&key=") + At, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, cut at 4096"), Scheme.Name), *Exact, FetchJournal(*this, Site, At), EVaRestSocketFrameKind::Text);
		// A PING between the fragments is answered and the message stays whole (RFC 6455 5.4) [rx.control_inside_fragmented]
		const FString Ping = NewKey();
		const TUniquePtr<FEvents> Between = Receive(*this, *Transport, Scheme, TEXT("/ws/fragment?cyrillic=5120&split_inside=2000&ping_between=1&key=") + Ping, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, a PING between the fragments"), Scheme.Name), *Between, FetchJournal(*this, Site, Ping), EVaRestSocketFrameKind::Text);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveLargeTest, "VaRest.Integration.WebSocket.Transport.Receive.Large", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveLargeTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// One 200000-byte frame of Cyrillic, handed over in 64 KiB chunks [rx.chunk_sizes_single_frame]: code points cut by chunks
		const FString Single = NewKey();
		const TUniquePtr<FEvents> Large = Receive(*this, *Transport, Scheme, TEXT("/ws/large?bytes=200000&ascii=0&key=") + Single, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, one large frame"), Scheme.Name), *Large, FetchJournal(*this, Site, Single), EVaRestSocketFrameKind::Text);
		// Binary in three frames: the kind from the first fragment [rx.binary_flag_on_continuations]
		const FString Bytes = NewKey();
		const TUniquePtr<FEvents> Binary = Receive(*this, *Transport, Scheme, TEXT("/ws/fragment?bytes=10000&binary=1&at=3000,6000&key=") + Bytes, 1);
		SameAsSent(*this, *FString::Printf(TEXT("%s, binary in three frames"), Scheme.Name), *Binary, FetchJournal(*this, Site, Bytes), EVaRestSocketFrameKind::Binary);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveOrderTest, "VaRest.Integration.WebSocket.Transport.Receive.Order", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveOrderTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// Spec 7.1, E12: one ordered queue for both kinds [rx.order_preserved]
		const TUniquePtr<FEvents> Events = Receive(*this, *Transport, Scheme, TEXT("/ws/order?n=400&key=") + NewKey(), 400);
		const TArray<const FVaRestSocketEvent*> Messages = Events->GetMessages();
		if (!TestEqual(FString::Printf(TEXT("%s: 400 messages"), Scheme.Name), Messages.Num(), 400))
		{
			continue;
		}
		int32 Wrong = 0;
		for (int32 Index = 0; Index < 400; ++Index)
		{
			const FVaRestSocketEvent& Each = *Messages[Index];
			if (Index % 2 == 0)
			{
				Wrong += Each.Kind == EVaRestSocketFrameKind::Text && Utf8(Each.Bytes).Equals(FString::Printf(TEXT("t%d"), Index), ESearchCase::CaseSensitive) ? 0 : 1;
			}
			else
			{
				const bool bRight = Each.Kind == EVaRestSocketFrameKind::Binary && Each.Bytes.Num() == 4
					&& ((Each.Bytes[0] << 24) | (Each.Bytes[1] << 16) | (Each.Bytes[2] << 8) | Each.Bytes[3]) == Index;
				Wrong += bRight ? 0 : 1;
			}
		}
		TestEqual(FString::Printf(TEXT("%s: every message in place, text and binary alternating"), Scheme.Name), Wrong, 0);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveInvalidTest, "VaRest.Integration.WebSocket.Transport.Receive.Invalid", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveInvalidTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		// RFC 6455 8.1, spec 2.3: invalid UTF-8 in a text message closes with 1007, the message never delivered [rx.invalid_utf8_passthrough]
		const FString BadKey = NewKey();
		const TUniquePtr<FEvents> Bad = Receive(*this, *Transport, Scheme, TEXT("/ws/badutf8?key=") + BadKey, 1);
		const FVaRestSocketEvent* BadEnd = Bad->GetEnd();
		TestEqual(FString::Printf(TEXT("%s: the invalid text is not delivered"), Scheme.Name), Bad->Count(EVaRestSocketEventType::Message), 0);
		TestTrue(FString::Printf(TEXT("%s: Closed 1007 by the transport, InvalidUtf8 (%s)"), Scheme.Name, *Bad->Describe()), BadEnd && BadEnd->Type == EVaRestSocketEventType::Closed
			&& BadEnd->End.Code == 1007 && BadEnd->End.By == EVaRestSocketCloseBy::Transport && BadEnd->End.Failure == EVaRestSocketFailure::InvalidUtf8);
		TestEqual(FString::Printf(TEXT("%s: the server got our 1007"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, BadKey)), 1007);
		// Spec 16.5 item 9: over MaxMessageBytes, closed with 1009 as the chunks arrive, never delivered
		const FString BigKey = NewKey();
		FVaRestSocketConnectRequest Limited;
		Limited.MaxMessageBytes = 65536;
		const TUniquePtr<FEvents> Big = Receive(*this, *Transport, Scheme, TEXT("/ws/large?bytes=200000&key=") + BigKey, 1, MoveTemp(Limited));
		const FVaRestSocketEvent* BigEnd = Big->GetEnd();
		TestEqual(FString::Printf(TEXT("%s: the oversize message is not delivered"), Scheme.Name), Big->Count(EVaRestSocketEventType::Message), 0);
		TestTrue(FString::Printf(TEXT("%s: Closed 1009 by the transport, MessageTooBig (%s)"), Scheme.Name, *Big->Describe()), BigEnd && BigEnd->Type == EVaRestSocketEventType::Closed
			&& BigEnd->End.Code == 1009 && BigEnd->End.By == EVaRestSocketCloseBy::Transport && BigEnd->End.Failure == EVaRestSocketFailure::MessageTooBig);
		TestEqual(FString::Printf(TEXT("%s: the server got our 1009"), Scheme.Name), JournalCloseCode(FetchJournal(*this, Site, BigKey)), 1009);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveLimitsTest, "VaRest.Integration.WebSocket.Transport.Receive.Limits", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveLimitsTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		for (const int32 Size : { 65536, 65537 })
		{
			const FString Key = NewKey();
			FVaRestSocketConnectRequest Limited;
			Limited.MaxMessageBytes = 65536;
			const TUniquePtr<FEvents> Events = Receive(*this, *Transport, Scheme,
				FString::Printf(TEXT("/ws/large?bytes=%d&ascii=1&key=%s"), Size, *Key), 1, MoveTemp(Limited));
			const FVaRestJson Journal = FetchJournal(*this, Site, Key);
			if (Size == 65536)
			{
				SameAsSent(*this, *FString::Printf(TEXT("%s: exactly at the 65536-byte limit"), Scheme.Name), *Events, Journal, EVaRestSocketFrameKind::Text);
				const TArray<const FVaRestSocketEvent*> Messages = Events->GetMessages();
				if (TestEqual(TEXT("one message at the exact limit"), Messages.Num(), 1))
				{
					TestEqual(TEXT("exactly 65536 bytes delivered"), Messages[0]->Bytes.Num(), 65536);
				}
			}
			else
			{
				const FVaRestSocketEvent* End = Events->GetEnd();
				TestEqual(FString::Printf(TEXT("%s: 65537 bytes deliver no message"), Scheme.Name), Events->Count(EVaRestSocketEventType::Message), 0);
				TestTrue(FString::Printf(TEXT("%s: one byte over the limit closes 1009 / MessageTooBig (%s)"), Scheme.Name, *Events->Describe()),
					End && End->Type == EVaRestSocketEventType::Closed && End->End.Code == 1009
					&& End->End.By == EVaRestSocketCloseBy::Transport && End->End.Failure == EVaRestSocketFailure::MessageTooBig);
				TestEqual(FString::Printf(TEXT("%s: the server got our 1009"), Scheme.Name), JournalCloseCode(Journal), 1009);
			}
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestWsReceiveEchoTest, "VaRest.Integration.WebSocket.Transport.Receive.Echo", VAREST_WS_TEST_FLAGS)

bool FVaRestWsReceiveEchoTest::RunTest(const FString& Parameters)
{
	VAREST_WS_BEGIN();
	for (const FScheme& Scheme : Schemes(Site))
	{
		FEvents Events(Transport->Connect(Request(Scheme, TEXT("/ws/echo?key=") + NewKey())));
		Events.bAcknowledge = false;
		if (!TestTrue(FString::Printf(TEXT("%s: opened"), Scheme.Name), Events.WaitType(EVaRestSocketEventType::Open)))
		{
			Drain(*this, *Transport, Events);
			continue;
		}
		const uint8 Raw[] = { 1, 2, 3 };
		const FString Long = FString::ChrN(300000, TEXT('z'));
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(TEXT("hello"))));
		Events.Connection->Send(FVaRestSocketFrame::Binary(Raw));
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView(Long)));
		Events.Connection->Send(FVaRestSocketFrame::Text(FStringView()));
		Events.Connection->Send(FVaRestSocketFrame::Binary(TConstArrayView<uint8>()));
		const bool bAll = Events.WaitMessages(5);
		const TArray<const FVaRestSocketEvent*> Messages = Events.GetMessages();
		if (TestTrue(FString::Printf(TEXT("%s: five ordered echoes (%s)"), Scheme.Name, *Events.Describe()), bAll && Messages.Num() == 5))
		{
			TestTrue(FString::Printf(TEXT("%s: the text"), Scheme.Name), Messages[0]->Kind == EVaRestSocketFrameKind::Text && Utf8(Messages[0]->Bytes).Equals(TEXT("hello"), ESearchCase::CaseSensitive));
			TestTrue(FString::Printf(TEXT("%s: the binary"), Scheme.Name), Messages[1]->Kind == EVaRestSocketFrameKind::Binary && Messages[1]->Bytes == TArray<uint8>(Raw, 3));
			TestTrue(FString::Printf(TEXT("%s: the 300000 bytes, sent in fragments, back whole"), Scheme.Name), Messages[2]->Bytes.Num() == 300000 && Utf8(Messages[2]->Bytes).Equals(Long, ESearchCase::CaseSensitive));
			TestTrue(FString::Printf(TEXT("%s: the fourth echo is empty text"), Scheme.Name), Messages[3]->Kind == EVaRestSocketFrameKind::Text && Messages[3]->Bytes.Num() == 0);
			TestTrue(FString::Printf(TEXT("%s: the fifth echo is empty binary"), Scheme.Name), Messages[4]->Kind == EVaRestSocketFrameKind::Binary && Messages[4]->Bytes.Num() == 0);
			// Spec 2.3: what is queued and not yet delivered counts; Consumed gives it back
			TestEqual(FString::Printf(TEXT("%s: pending inbound bytes"), Scheme.Name), Events.Connection->GetPendingInboundBytes(), static_cast<int64>(5 + 3 + 300000));
			Events.Connection->Consumed(5 + 3 + 300000);
			TestEqual(FString::Printf(TEXT("%s: ... none after Consumed"), Scheme.Name), Events.Connection->GetPendingInboundBytes(), static_cast<int64>(0));
		}
		Drain(*this, *Transport, Events);
	}
	return true;
}

#undef VAREST_WS_BEGIN
#undef VAREST_WS_TEST_FLAGS

#endif // WITH_DEV_AUTOMATION_TESTS
