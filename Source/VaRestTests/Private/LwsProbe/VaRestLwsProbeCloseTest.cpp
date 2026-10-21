// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): what LWS 3.0 hands over and sends when a connection closes -- the peer's codes, empty and
// short close frames, our own close against an echoing and a non-echoing server, drops -- and how it pings and pongs
// (WebSocket spec 2.5). Whether a connection ended is a library outcome, noted and never failed (decision 3, review M-2).
// Close.Ours and Ping.Silent ask what LWS's own timers do, so their rigs wait 100 ms in lws_service (review C-1: LWS
// checks its deadlines only when lws_service returns)
//
// Harness invariants are asserted, so a harness failure is red and writes no fact (never "differs"): the connections a
// test's facts read opened (the server's 101), and the journals those facts read arrived

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"

namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate
{
	/** The PEER_INITIATED_CLOSE event of a connection, or nullptr */
	const FEvent* PeerClose(const TArray<FEvent>& Seen)
	{
		return Seen.FindByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_WS_PEER_INITIATED_CLOSE; });
	}

	/** The big-endian code LWS handed over, -1 when the payload has none */
	int32 ReceivedCode(const FEvent* Close)
	{
		return Close && Close->Bytes.Num() >= 2 ? (static_cast<int32>(Close->Bytes[0]) << 8) | Close->Bytes[1] : -1;
	}

	/** The client's close as the server received it ("close" in its journal): "<hex>" or "none" */
	FString ClientCloseHex(const FVaRestJson& Journal)
	{
		const TArray<FVaRestJson> Closes = JournalEvents(Journal, TEXT("close"));
		return Closes.Num() > 0 ? FString::Printf(TEXT("\"%s\""), *Closes[0][TEXT("hex")].AsString()) : FString(TEXT("none"));
	}

	/** Connects and waits until the connection ends by itself (the server closes it); false when it does not */
	bool RunToEnd(FRig& Rig, const FString& Path, TArray<FEvent>& Out, int32 PeerCloseReturn = 0, double Seconds = 20.0)
	{
		FConnectSpec Spec;
		Spec.Path = Path;
		Spec.PeerCloseReturn = PeerCloseReturn;
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		const bool bEnded = Rig.WaitEnded(Conn, Seconds);
		Out = Rig.Events(Conn);
		return bEnded;
	}

	/** The callbacks from ESTABLISHED on */
	FString AfterOpen(const TArray<FEvent>& Seen)
	{
		const int32 Open = Seen.IndexOfByPredicate([](const FEvent& Each) { return Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED; });
		return CallSequence(Open == INDEX_NONE ? Seen : TArray<FEvent>(Seen.GetData() + Open, Seen.Num() - Open));
	}

	/**
	 * What our callback returns from the WRITEABLEs LWS sends after the one that ran our close. LWS calls WRITEABLE again
	 * after that -1 (Probe.VaRest.Lws.Close.Ours: "close> > WRITEABLE > close>"), so the answer there decides whether LWS
	 * gets to wait for the echo at all
	 */
	enum class EAfterClose : uint8
	{
		Engine, // -1 again, from every WRITEABLE while the close is pending: the rig's WriteNext, as the engine (LwsWebSocket.cpp:515-530)
		Zero,   // 0: only the first WRITEABLE runs lws_close_reason and returns -1, then LWS's own wait for the echo decides
	};

	/** EAfterClose::Zero's hook (service thread): 0 from every WRITEABLE after the one where WriteNext ran our close */
	int32 ZeroAfterClose(FConn& Conn, lws*, int32 Reason, void*, void*, size_t)
	{
		// The condition of WriteNext's close branch (VaRestLwsProbe.cpp, FRig::WriteNext): nothing queued, a close code set
		if (Reason != LWS_CALLBACK_CLIENT_WRITEABLE || Conn.CloseCode == 0 || Conn.Outbox.Num() > 0)
		{
			return KeepDefault;
		}
		int64& Closes = Conn.Counters.FindOrAdd(FName(TEXT("closes")));
		if (Closes++ == 0)
		{
			return KeepDefault;
		}
		Conn.Rig->Record(Conn.Id, Marker, Closes, TEXT("WRITEABLE after our close: 0"));
		return 0;
	}

	/** A close we start on an open connection, timed from CloseRequested to WSI_DESTROY */
	struct FOurClose
	{
		bool bOpened = false; // the precondition: the connection opened, so our close had something to close
		bool bEnded = false;
		double Ms = -1.0;     // -1: not timed (no CloseRequested, or no end)
		double Seconds = 0.0; // how long the end was waited for
		FString Sequence;
	};

	FOurClose CloseOurs(FRig& Rig, const FString& Path, int32 Code, const FString& Reason, double Seconds, EAfterClose After)
	{
		FOurClose Out;
		Out.Seconds = Seconds;
		FConnectSpec Spec;
		Spec.Path = Path;
		if (After == EAfterClose::Zero)
		{
			Spec.Hook = &ZeroAfterClose;
		}
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		if (!Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED))
		{
			Rig.WaitEnded(Conn);
			return Out;
		}
		Out.bOpened = true;
		Rig.Close(Conn, Code, Reason);
		Out.bEnded = Rig.WaitEnded(Conn, Seconds);
		FEvent Requested;
		if (Rig.Find(Conn, CloseRequested, Requested))
		{
			// The sequence also when the connection did not end: what LWS did with our close so far
			Out.Sequence = CallSequence(Rig.Events(Conn).FilterByPredicate([&Requested](const FEvent& Each) { return Each.Time >= Requested.Time; }));
			FEvent Destroyed;
			if (Out.bEnded && Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed))
			{
				Out.Ms = (Destroyed.Time - Requested.Time) * 1000.0;
			}
		}
		else
		{
			Out.Sequence = TEXT("(our close never ran: no CloseRequested) ") + AfterOpen(Rig.Events(Conn));
		}
		return Out;
	}

	/** "<ms> ms", or why the close has no time (Task 5's rule: an untimed end reads "not timed", never a number) */
	FString Took(const FOurClose& Close)
	{
		if (Close.Ms >= 0.0)
		{
			return FString::Printf(TEXT("%.1f ms"), Close.Ms);
		}
		return Close.bEnded ? FString::Printf(TEXT("not timed (%s)"), *Close.Sequence)
			: FString::Printf(TEXT("not timed: no end within %.0f s (%s)"), Close.Seconds, *Close.Sequence);
	}

	/** The server's side of our close on /ws/noecho: its "close", then its "eof" (the client's TCP end) */
	FString ServerEnd(const FVaRestJson& Journal)
	{
		const TArray<FVaRestJson> Saw = JournalEvents(Journal, TEXT("close"));
		const TArray<FVaRestJson> Eof = JournalEvents(Journal, TEXT("eof"));
		return Saw.Num() > 0 && Eof.Num() > 0
			? FString::Printf(TEXT("the server saw our close, then TCP end %.2f s later"), Eof[0][TEXT("t")].AsDouble() - Saw[0][TEXT("t")].AsDouble())
			: FString(TEXT("the server journal lacks the close or the TCP end"));
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbePeerCodesTest, "Probe.VaRest.Lws.Close.PeerCodes",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbePeerCodesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	// The engine's ::FEvent (HAL/Event.h:21) would make the name ambiguous under the directive above
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	// Every code a server may send and several it must not (RFC 6455 7.4): which ones does LWS 3.0 hand over unchanged?
	const int32 Codes[] = { 1000, 1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014, 1015, 1016,
		2000, 2999, 3000, 4001, 4999, 5000, 65535 };
	struct FPeer
	{
		int32 Code = 0;
		int32 Conn = 0;
		FString Key;
	};
	TArray<FPeer> Peers;
	for (const int32 Code : Codes)
	{
		FPeer& Peer = Peers.AddDefaulted_GetRef();
		Peer.Code = Code;
		Peer.Key = NewKey();
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/close/%d?reason=bye-%d&key=%s"), Code, Code, *Peer.Key);
		Peer.Conn = Rig.Connect(MoveTemp(Spec));
	}
	const FString Utf8Reason = TEXT("пока ✓");
	const FString Utf8Key = NewKey();
	FConnectSpec Utf8Spec;
	Utf8Spec.Path = FString::Printf(TEXT("/ws/close/4002?reason=%s&key=%s"), *UrlValue(Utf8Reason), *Utf8Key);
	const int32 Utf8Conn = Rig.Connect(MoveTemp(Utf8Spec));
	// Each one ends: the server closes after the client's answer or after CLOSE_WAIT
	bool bAll = Rig.WaitEnded(Utf8Conn, 20.0);
	for (const FPeer& Peer : Peers)
	{
		bAll = Rig.WaitEnded(Peer.Conn, 20.0) && bAll;
	}
	NoteOutcome(*this, TEXT("every peer-closed connection ended within 20 s"), bAll);
	// Every fact below reads a close the server sent after its 101: a connection that never opened is a harness failure
	bool bAllOpened = Opened(Rig.Events(Utf8Conn));
	for (const FPeer& Peer : Peers)
	{
		bAllOpened = Opened(Rig.Events(Peer.Conn)) && bAllOpened;
	}
	if (!TestTrue(TEXT("every /ws/close connection opened"), bAllOpened))
	{
		return false;
	}

	TArray<FString> Rewritten;
	TArray<FString> EchoNotes;
	bool bAllEchoed = true;
	FString PeerSequence;
	for (const FPeer& Peer : Peers)
	{
		const TArray<FEvent> Seen = Rig.Events(Peer.Conn);
		const int32 Received = ReceivedCode(PeerClose(Seen));
		if (Received != Peer.Code)
		{
			Rewritten.Add(Received < 0 ? FString::Printf(TEXT("%d -> no code (%s)"), Peer.Code, *AfterOpen(Seen)) : FString::Printf(TEXT("%d -> %d"), Peer.Code, Received));
		}
		const FVaRestJson Journal = FetchJournal(*this, Site, Peer.Key);
		if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
		{
			return false;
		}
		const TArray<FVaRestJson> Answers = JournalEvents(Journal, TEXT("close"));
		if (Answers.Num() == 0)
		{
			bAllEchoed = false;
			EchoNotes.Add(FString::Printf(TEXT("%d: no answer"), Peer.Code));
		}
		else if (!Answers[0].Has(TEXT("code")) || Answers[0][TEXT("code")].AsInt() != Peer.Code)
		{
			EchoNotes.Add(FString::Printf(TEXT("%d: answered %s"), Peer.Code, *ClientCloseHex(Journal)));
		}
		if (Peer.Code == 1000)
		{
			PeerSequence = AfterOpen(Seen);
		}
	}
	Fact(*this, TEXT("close.rewritten_codes"), EVerdict::Recorded,
		Rewritten.Num() == 0 ? FString(TEXT("none: every code arrived as sent")) : FString::Join(Rewritten, TEXT(", ")),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): which received close codes LWS 3.0 rewrites; 2.5 normalises codes a peer must not send"),
		TEXT("the server closed with each code (raw); the first two bytes of PEER_INITIATED_CLOSE's in"));
	Fact(*this, TEXT("close.echo_on_return_0"), AgreesIf(bAllEchoed),
		bAllEchoed ? FString(TEXT("every server close was answered with a close")) : FString::Join(EchoNotes.FilterByPredicate([](const FString& Each) { return Each.EndsWith(TEXT("no answer")); }), TEXT(", ")),
		TEXT("T12: PEER_INITIATED_CLOSE returning 0 makes LWS echo the close (libwebsockets.h:1217-1225)"),
		TEXT("the server's journal of each connection"));
	Fact(*this, TEXT("close.echo_payload"), EVerdict::Recorded,
		EchoNotes.Num() == 0 ? FString(TEXT("every answer carried the code the server sent")) : FString::Join(EchoNotes, TEXT(", ")),
		TEXT("RFC 6455 5.5.1: the answer typically echoes the status code"), TEXT("the server's journal: the client's close payload"));

	const TArray<FEvent> Utf8Seen = Rig.Events(Utf8Conn);
	const FEvent* Utf8Close = PeerClose(Utf8Seen);
	const TArray<uint8> Expected = Utf8Bytes(Utf8Reason);
	const bool bUtf8Intact = Utf8Close && Utf8Close->Bytes.Num() == Expected.Num() + 2
		&& FMemory::Memcmp(Utf8Close->Bytes.GetData() + 2, Expected.GetData(), Expected.Num()) == 0;
	Fact(*this, TEXT("close.reason_utf8_bytes"), AgreesIf(bUtf8Intact),
		Utf8Close ? FString::Printf(TEXT("reason bytes %s"), *HexOf(TConstArrayView<uint8>(Utf8Close->Bytes).RightChop(2))) : FString(TEXT("no PEER_INITIATED_CLOSE")),
		TEXT("WebSocket spec 2.5: the reason is decoded as UTF-8 by VaRest, so LWS must hand the bytes over unchanged"),
		FString::Printf(TEXT("close 4002 with reason \"%s\" (%s)"), *Utf8Reason, *HexOf(Expected)));
	Fact(*this, TEXT("close.peer_sequence"), EVerdict::Recorded, PeerSequence,
		TEXT("T12: CLIENT_CLOSED ends a client session (libwebsockets.h:1313-1314)"), TEXT("the 1000 connection, from ESTABLISHED on"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeEmptyCloseTest, "Probe.VaRest.Lws.Close.EmptyAndShort",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeEmptyCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyNone = NewKey();
	const FString KeyOne = NewKey();
	TArray<FEvent> NoneSeen;
	TArray<FEvent> OneSeen;
	bool bAll = RunToEnd(Rig, TEXT("/ws/close/none?key=") + KeyNone, NoneSeen);
	bAll = RunToEnd(Rig, TEXT("/ws/close/onebyte?key=") + KeyOne, OneSeen) && bAll;
	NoteOutcome(*this, TEXT("both connections ended within 20 s"), bAll);
	if (!TestTrue(TEXT("both /ws/close connections opened"), Opened(NoneSeen) && Opened(OneSeen)))
	{
		return false;
	}
	const FVaRestJson NoneJournal = FetchJournal(*this, Site, KeyNone);
	const FVaRestJson OneJournal = FetchJournal(*this, Site, KeyOne);
	if (!TestTrue(TEXT("the server's journals arrived"), NoneJournal.IsValid() && OneJournal.IsValid()))
	{
		return false;
	}
	const FEvent* NoneClose = PeerClose(NoneSeen);
	Fact(*this, TEXT("close.empty_len"), AgreesIf(NoneClose && NoneClose->Len == 0),
		NoneClose ? FString::Printf(TEXT("PEER_INITIATED_CLOSE len=%lld"), NoneClose->Len) : FString::Printf(TEXT("no PEER_INITIATED_CLOSE: %s"), *AfterOpen(NoneSeen)),
		TEXT("WebSocket spec 2.5: length 0 -> 1005 (the engine reads two bytes anyway, E27: LwsWebSocket.cpp:351)"),
		TEXT("/ws/close/none: a close frame with an empty payload"));
	Fact(*this, TEXT("close.empty_echo"), EVerdict::Recorded, TEXT("the client answered ") + ClientCloseHex(NoneJournal),
		TEXT("none"), TEXT("the server's journal: the client's close payload"));
	const FEvent* OneClose = PeerClose(OneSeen);
	Fact(*this, TEXT("close.one_byte"), EVerdict::Recorded,
		FString::Printf(TEXT("%s; the client answered %s"),
			OneClose ? *FString::Printf(TEXT("PEER_INITIATED_CLOSE len=%lld bytes %s"), OneClose->Len, *HexOf(OneClose->Bytes)) : *FString::Printf(TEXT("no PEER_INITIATED_CLOSE (%s)"), *AfterOpen(OneSeen)),
			*ClientCloseHex(OneJournal)),
		TEXT("WebSocket spec 2.5: length 1 -> 1002 (malformed)"), TEXT("/ws/close/onebyte: a close frame with the payload 03"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbePeerReturnNonZeroTest, "Probe.VaRest.Lws.Close.PeerReturnNonZero",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbePeerReturnNonZeroTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	TArray<FEvent> Seen;
	NoteOutcome(*this, TEXT("the connection ended within 20 s"), RunToEnd(Rig, TEXT("/ws/close/1000?reason=bye&key=") + Key, Seen, 1));
	if (!TestTrue(TEXT("the /ws/close connection opened"), Opened(Seen)))
	{
		return false;
	}
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	Fact(*this, TEXT("close.return_1_echo"), EVerdict::Recorded,
		FString::Printf(TEXT("the client answered %s; %s"), *ClientCloseHex(Journal), *AfterOpen(Seen)),
		TEXT("libwebsockets.h:1217-1225: nonzero -> LWS just closes; the engine returns 1 (LwsWebSocket.cpp:389-390)"),
		TEXT("PEER_INITIATED_CLOSE returning 1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeOurCloseTest, "Probe.VaRest.Lws.Close.Ours",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeOurCloseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// The close-ack wait is LWS's own timer: the short service wait, so the value is LWS's and not the rig's (review C-1)
	FRigOptions Options;
	Options.ServiceTimeoutMs = 100;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString KeyEcho = NewKey();
	const FString KeyNoEcho = NewKey();
	const FString KeyEchoZero = NewKey();
	const FString KeyNoEchoZero = NewKey();
	const FString KeyLong = NewKey();
	// Each wait in two shapes: the engine's close (-1 from every WRITEABLE while closing), and its control, 0 from the
	// WRITEABLEs after the first -1. LWS calls WRITEABLE again after that -1 (close.ours_sequence), and the engine's
	// second -1 ends the connection there, before LWS can wait for anything: without the control the wait facts would
	// not measure the wait they name
	const FOurClose Echoed = CloseOurs(Rig, TEXT("/ws/echo?key=") + KeyEcho, 1000, TEXT("bye"), 30.0, EAfterClose::Engine);
	const FOurClose Unanswered = CloseOurs(Rig, TEXT("/ws/noecho?key=") + KeyNoEcho, 1000, TEXT("bye"), 45.0, EAfterClose::Engine);
	const FOurClose EchoedZero = CloseOurs(Rig, TEXT("/ws/echo?key=") + KeyEchoZero, 1000, TEXT("bye"), 30.0, EAfterClose::Zero);
	const FOurClose UnansweredZero = CloseOurs(Rig, TEXT("/ws/noecho?key=") + KeyNoEchoZero, 1000, TEXT("bye"), 45.0, EAfterClose::Zero);
	const FOurClose Long = CloseOurs(Rig, TEXT("/ws/echo?key=") + KeyLong, 4000, FString::ChrN(200, TEXT('r')), 30.0, EAfterClose::Engine);
	NoteOutcome(*this, TEXT("our five closes ended"), Echoed.bEnded && Unanswered.bEnded && EchoedZero.bEnded && UnansweredZero.bEnded && Long.bEnded);
	// Our close needs an open connection: one that never opened is a harness failure, not a close fact
	if (!TestTrue(TEXT("the five connections opened (/ws/echo and /ws/noecho in both shapes, the long reason's /ws/echo)"),
		Echoed.bOpened && Unanswered.bOpened && EchoedZero.bOpened && UnansweredZero.bOpened && Long.bOpened))
	{
		return false;
	}
	const FVaRestJson EchoJournal = FetchJournal(*this, Site, KeyEcho);
	const FVaRestJson NoEchoJournal = FetchJournal(*this, Site, KeyNoEcho);
	const FVaRestJson EchoZeroJournal = FetchJournal(*this, Site, KeyEchoZero);
	const FVaRestJson NoEchoZeroJournal = FetchJournal(*this, Site, KeyNoEchoZero);
	const FVaRestJson LongJournal = FetchJournal(*this, Site, KeyLong);
	if (!TestTrue(TEXT("the server's journals arrived"), EchoJournal.IsValid() && NoEchoJournal.IsValid() && EchoZeroJournal.IsValid()
		&& NoEchoZeroJournal.IsValid() && LongJournal.IsValid()))
	{
		return false;
	}
	const TCHAR* const Shapes = TEXT("lws_close_reason + -1 from WRITEABLE, then -1 (the engine's close) or 0 from the WRITEABLEs after it; lws_service waiting up to 100 ms");
	Fact(*this, TEXT("close.ours_echoed_ms"), EVerdict::Recorded,
		FString::Printf(TEXT("the engine's close (-1 from every WRITEABLE): %s from our close to WSI_DESTROY against an echoing server; -1 once, then 0: %s"),
			*Took(Echoed), *Took(EchoedZero)),
		TEXT("none"), FString::Printf(TEXT("/ws/echo; %s"), Shapes));
	Fact(*this, TEXT("close.ours_noecho_wait_ms"), EVerdict::Recorded,
		FString::Printf(TEXT("the engine's close (-1 from every WRITEABLE): %s from our close to WSI_DESTROY against a server that never answers, %s; -1 once, then 0: %s, %s"),
			*Took(Unanswered), *ServerEnd(NoEchoJournal), *Took(UnansweredZero), *ServerEnd(NoEchoZeroJournal)),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): whether LWS waits for the close echo; 2.5 bounds it with CloseTimeout (5 s)"),
		FString::Printf(TEXT("/ws/noecho, timeout_secs left at LWS's default; %s"), Shapes));
	Fact(*this, TEXT("close.ours_sequence"), EVerdict::Recorded,
		FString::Printf(TEXT("the engine's close: %s; -1 once, then 0: %s"), *Echoed.Sequence, *EchoedZero.Sequence),
		TEXT("none"), TEXT("callbacks from our close (CloseRequested, \"close>\") on /ws/echo, in both shapes"));
	const TArray<FVaRestJson> LongSaw = JournalEvents(LongJournal, TEXT("close"));
	const TArray<FVaRestJson> LongFail = JournalEvents(LongJournal, TEXT("fail"));
	Fact(*this, TEXT("close.ours_long_reason"), EVerdict::Recorded,
		LongSaw.Num() > 0 ? FString::Printf(TEXT("the server got a %lld-byte close payload"), LongSaw[0][TEXT("len")].AsInt64())
			: LongFail.Num() > 0 ? FString::Printf(TEXT("the server failed the frame: %s"), *LongFail[0][TEXT("reason")].AsString())
			: FString(TEXT("no close reached the server")),
		TEXT("WebSocket spec 15: close reasons are cut to 123 bytes by VaRest; lws_close_reason takes \"up to 124 bytes\" (libwebsockets.h:832-845)"),
		TEXT("lws_close_reason with a 200-byte reason"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeDropsTest, "Probe.VaRest.Lws.Close.Drops",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeDropsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeCloseTestPrivate;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	TArray<FEvent> FinSeen;
	TArray<FEvent> RstSeen;
	bool bAll = RunToEnd(Rig, TEXT("/ws/drop?after_ms=200&key=") + NewKey(), FinSeen);
	bAll = RunToEnd(Rig, TEXT("/ws/drop?after_ms=200&rst=1&key=") + NewKey(), RstSeen) && bAll;
	NoteOutcome(*this, TEXT("both dropped connections ended within 20 s"), bAll);
	// A drop of a connection that never opened is a refused handshake, not a drop
	if (!TestTrue(TEXT("both /ws/drop connections opened"), Opened(FinSeen) && Opened(RstSeen)))
	{
		return false;
	}
	const TCHAR* const UncleanSpec = TEXT("WebSocket spec 2.5: an unclean end (CLIENT_CLOSED / WSI_DESTROY without a close frame) -> 1006");
	Fact(*this, TEXT("close.drop_fin_sequence"), EVerdict::Recorded, AfterOpen(FinSeen), UncleanSpec, TEXT("/ws/drop: TCP FIN, no close frame"));
	Fact(*this, TEXT("close.drop_rst_sequence"), EVerdict::Recorded, AfterOpen(RstSeen), UncleanSpec, TEXT("/ws/drop?rst=1: TCP RST"));
	const bool bFinClosed = OfReason(FinSeen, LWS_CALLBACK_CLIENT_CLOSED).Num() > 0;
	const bool bRstClosed = OfReason(RstSeen, LWS_CALLBACK_CLIENT_CLOSED).Num() > 0;
	Fact(*this, TEXT("close.drop_reports_client_closed"), EVerdict::Recorded,
		FString::Printf(TEXT("CLIENT_CLOSED on FIN: %s, on RST: %s; PEER_INITIATED_CLOSE on either: %s"), YesNo(bFinClosed), YesNo(bRstClosed),
			YesNo(PeerClose(FinSeen) != nullptr || PeerClose(RstSeen) != nullptr)),
		UncleanSpec, TEXT("the two drops above"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeServerPingsTest, "Probe.VaRest.Lws.Ping.ServerPings",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeServerPingsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FString Key = NewKey();
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/serverping?count=3&interval_ms=200&key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	// Completion: the server's "pings-done" text comes after its last PING (Task 2's route)
	const bool bDone = Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([Conn](const FEvent& Each)
		{
			return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("pings-done"), ESearchCase::CaseSensitive);
		});
	}, 15.0);
	NoteOutcome(*this, TEXT("the server's pings-done arrived"), bDone);
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	// The server pings only an open connection: without the open, "0 of 3 answered" would be a harness failure
	if (!TestTrue(TEXT("the /ws/serverping connection opened"), Opened(Rig.Events(Conn))))
	{
		return false;
	}
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	int32 Answered = 0;
	double Slowest = 0.0;
	const TArray<FVaRestJson> Frames = JournalEvents(Journal, TEXT("frame"));
	for (const FVaRestJson& Sent : JournalEvents(Journal, TEXT("sent")))
	{
		if (Sent[TEXT("op")].AsInt() != 9)
		{
			continue;
		}
		const FString Payload = Sent[TEXT("b64")].AsString();
		const FVaRestJson* Pong = Frames.FindByPredicate([&Payload, &Sent](const FVaRestJson& Each)
		{
			return Each[TEXT("op")].AsInt() == 10 && Each[TEXT("b64")].AsString().Equals(Payload, ESearchCase::CaseSensitive)
				&& Each[TEXT("t")].AsDouble() >= Sent[TEXT("t")].AsDouble();
		});
		if (Pong)
		{
			++Answered;
			Slowest = FMath::Max(Slowest, (*Pong)[TEXT("t")].AsDouble() - Sent[TEXT("t")].AsDouble());
		}
	}
	Fact(*this, TEXT("ping.auto_pong"), AgreesIf(Answered == 3), FString::Printf(TEXT("%d of 3 server PINGs answered with a PONG of the same payload"), Answered),
		TEXT("WebSocket spec 0.3 (inferred from 4.3): LWS answers server PINGs (RFC 6455 5.5.2 requires it)"),
		TEXT("/ws/serverping: three PINGs 200 ms apart; the server's journal"));
	Fact(*this, TEXT("ping.auto_pong_latency_ms"), EVerdict::Recorded,
		Answered > 0 ? FString::Printf(TEXT("slowest PONG %.1f ms after its PING"), Slowest * 1000.0) : FString(TEXT("not timed: no PING was answered")),
		TEXT("none"), TEXT("the server's journal times"));
	const TArray<FEvent> Seen = Rig.Events(Conn);
	TArray<FString> Other;
	for (const FEvent& Each : Seen)
	{
		const bool bExpected = Each.Reason < 0 || Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE || Each.Reason == LWS_CALLBACK_CLIENT_WRITEABLE
			|| Each.Reason == LWS_CALLBACK_CLIENT_ESTABLISHED || Each.Reason == LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH
			|| Each.Reason == LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER || Each.Reason == LWS_CALLBACK_WSI_CREATE
			|| Each.Reason == LWS_CALLBACK_WSI_DESTROY || Each.Reason == LWS_CALLBACK_CLIENT_CLOSED || Each.Reason == LWS_CALLBACK_CLOSED;
		if (!bExpected)
		{
			Other.AddUnique(ReasonName(Each.Reason));
		}
	}
	Fact(*this, TEXT("ping.client_sees_server_ping"), EVerdict::Recorded,
		Other.Num() == 0 ? FString(TEXT("no callback for the server's PINGs")) : FString::Printf(TEXT("other callbacks: %s"), *FString::Join(Other, TEXT(", "))),
		TEXT("none"), TEXT("the connection's callbacks other than the usual ones"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeClientPingTest, "Probe.VaRest.Lws.Ping.ClientPing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeClientPingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	FRig Rig(Site);
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/echo?key=") + NewKey();
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// WebSocket spec 2.5's ping: 8 bytes (sequence + send time); then the largest a control frame may carry
	const TArray<uint8> Eight = { 1, 2, 3, 4, 5, 6, 7, 8 };
	TArray<uint8> Largest;
	Largest.Init(static_cast<uint8>('p'), 125);
	Rig.Write(Conn, LWS_WRITE_PING, Eight);
	const auto PongCount = [Conn](const TArray<FEvent>& All, int32 Count)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE_PONG; }).Num() >= Count;
	};
	const bool bFirst = Rig.WaitFor([&PongCount](const TArray<FEvent>& All) { return PongCount(All, 1); });
	Rig.Write(Conn, LWS_WRITE_PING, Largest);
	const bool bSecond = Rig.WaitFor([&PongCount](const TArray<FEvent>& All) { return PongCount(All, 2); });
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const TArray<FEvent> Pongs = OfReason(Rig.Events(Conn), LWS_CALLBACK_CLIENT_RECEIVE_PONG);
	const TArray<FEvent> Writes = OfReason(Rig.Events(Conn), WriteDone);
	const bool bEchoed = bFirst && Pongs.Num() >= 1 && Pongs[0].Bytes == Eight;
	Fact(*this, TEXT("ping.pong_payload_echo"), AgreesIf(bEchoed),
		Pongs.Num() >= 1 ? FString::Printf(TEXT("CLIENT_RECEIVE_PONG len=%lld bytes %s"), Pongs[0].Len, *HexOf(Pongs[0].Bytes)) : FString(TEXT("no CLIENT_RECEIVE_PONG")),
		TEXT("T9: LWS_WRITE_PING, and CLIENT_RECEIVE_PONG hands over the payload WebSocket spec 2.5 matches"),
		TEXT("an 8-byte PING 0102030405060708 to /ws/echo"));
	Fact(*this, TEXT("ping.rtt_ms"), EVerdict::Recorded,
		bEchoed && Writes.Num() >= 1 ? FString::Printf(TEXT("%.2f ms from lws_write(PING) to CLIENT_RECEIVE_PONG on loopback"), (Pongs[0].Time - Writes[0].Time) * 1000.0) : FString(TEXT("not measured")),
		TEXT("WebSocket spec 2.5: RTT measured on the service thread"), TEXT("the first ping"));
	Fact(*this, TEXT("ping.max_payload_125"), EVerdict::Recorded,
		bSecond && Pongs.Num() >= 2 ? FString::Printf(TEXT("a 125-byte PING came back as %lld bytes, %s"), Pongs[1].Len, Pongs[1].Bytes == Largest ? TEXT("unchanged") : TEXT("changed"))
			: FString(TEXT("no second CLIENT_RECEIVE_PONG")),
		TEXT("RFC 6455 5.5: a control frame carries at most 125 bytes"), TEXT("a 125-byte PING to /ws/echo"));

	FConnectSpec Unsolicited;
	Unsolicited.Path = TEXT("/ws/serverping?count=1&interval_ms=100&unsolicited=1&key=") + NewKey();
	const int32 Other = Rig.Connect(MoveTemp(Unsolicited));
	const bool bDone = Rig.WaitFor([Other](const TArray<FEvent>& All)
	{
		return All.ContainsByPredicate([Other](const FEvent& Each)
		{
			return Each.Conn == Other && Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && EventText(Each).Equals(TEXT("pings-done"), ESearchCase::CaseSensitive);
		});
	});
	Rig.Close(Other, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the unsolicited-pong connection ended after our close"), Rig.WaitEnded(Other));
	// The unsolicited PONG goes only to an open connection
	if (!TestTrue(TEXT("the unsolicited-pong connection opened"), Opened(Rig.Events(Other))))
	{
		return false;
	}
	const TArray<FEvent> OtherPongs = OfReason(Rig.Events(Other), LWS_CALLBACK_CLIENT_RECEIVE_PONG);
	Fact(*this, TEXT("ping.unsolicited_pong_delivered"), EVerdict::Recorded,
		bDone ? FString::Printf(TEXT("%d CLIENT_RECEIVE_PONG for an unsolicited PONG \"unsolicited\"%s"), OtherPongs.Num(),
			OtherPongs.Num() > 0 && EventText(OtherPongs[0]).Equals(TEXT("unsolicited"), ESearchCase::CaseSensitive) ? TEXT(", payload intact") : TEXT(""))
			: FString(TEXT("pings-done never arrived")),
		TEXT("RFC 6455 5.5.3: an unsolicited PONG is a heartbeat; WebSocket spec 2.5 counts inbound bytes as alive"),
		TEXT("/ws/serverping?unsolicited=1"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeSilentTest, "Probe.VaRest.Lws.Ping.Silent",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeSilentTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using VaRest::LwsProbe::FEvent;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// A short timeout_secs: if LWS ends idle connections by itself, it does so inside this test. The short service wait
	// and a window past timeout_secs + the wait + 5 s, so an idle kill is seen inside it (review C-1)
	constexpr uint32 SilentTimeoutSecs = 3;
	constexpr int32 SilentWaitMs = 100;
	const double Window = SilentTimeoutSecs + SilentWaitMs / 1000.0 + 5.0;
	FRigOptions Options;
	Options.TimeoutSecs = SilentTimeoutSecs;
	Options.ServiceTimeoutMs = SilentWaitMs;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	FConnectSpec Spec;
	Spec.Path = TEXT("/ws/silent?key=") + NewKey();
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	const double OpenedAt = FPlatformTime::Seconds();
	Rig.Write(Conn, LWS_WRITE_PING, Utf8Bytes(TEXT("dead?")));
	// Completion before silence, asserted: our PING has left (WriteDone, lws_write >= 0), then 3 s without a PONG. A PING
	// that never left would make "no PONG" agree for a situation that was not reached
	FEvent Written;
	const bool bWritten = Rig.WaitReason(Conn, WriteDone) && Rig.Find(Conn, WriteDone, Written) && !Written.Note.Contains(TEXT("ret=-"), ESearchCase::CaseSensitive);
	if (!TestTrue(TEXT("our PING was written"), bWritten))
	{
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(*this, TEXT("the silent connection ended after our close"), Rig.WaitEnded(Conn, 40.0));
		return false;
	}
	const bool bPong = Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_RECEIVE_PONG, 3.0);
	Fact(*this, TEXT("ping.silent_no_pong"), AgreesIf(!bPong), bPong ? FString(TEXT("a PONG arrived")) : FString(TEXT("no PONG within 3 s")),
		TEXT("the control for WebSocket spec 2.5's dead-link detection: /ws/silent never answers"), TEXT("one PING to /ws/silent"));
	const double Left = FMath::Max(0.0, Window - (FPlatformTime::Seconds() - OpenedAt));
	const bool bDied = Rig.WaitEnded(Conn, Left);
	FEvent Destroyed;
	Fact(*this, TEXT("ping.idle_survives_timeout_secs"), AgreesIf(!bDied),
		bDied && Rig.Find(Conn, LWS_CALLBACK_WSI_DESTROY, Destroyed)
			? FString::Printf(TEXT("LWS ended the idle connection %.1f s after it opened: %s"), Destroyed.Time - OpenedAt, *CallSequence(Rig.Events(Conn)))
			: FString::Printf(TEXT("still open %.1f s after it opened, with timeout_secs = %u and lws_service waiting up to %d ms"), Window, SilentTimeoutSecs, SilentWaitMs),
		TEXT("WebSocket spec 2.1: ws_ping_pong_interval = 0, so LWS neither pings nor drops an idle connection; VaRest's own ping decides (2.5)"),
		TEXT("an open connection to /ws/silent, nothing sent after one PING"));
	if (!bDied)
	{
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(*this, TEXT("the silent connection ended after our close"), Rig.WaitEnded(Conn, 40.0));
	}
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
