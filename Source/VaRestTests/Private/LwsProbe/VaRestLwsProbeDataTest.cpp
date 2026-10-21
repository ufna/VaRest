// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// WebSocket spec 16.4 (W0): how LWS 3.0 hands received frames over (chunks, flags, remaining bytes, order), pauses and
// resumes reading, writes fragments, large buffers and two frames per WRITEABLE, lets a reply in between two messages,
// and takes SO_SNDBUF on its socket (WebSocket spec 2.3, 2.4)

#include "VaRestLwsProbe.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/AutomationTest.h"
#include "VaRestSha256.h"

#if PLATFORM_LINUX || PLATFORM_MAC
#include <sys/socket.h>
#endif

namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate
{
	constexpr int32 AutoReplyFragmentBytes = 64 * 1024;
	constexpr int32 AutoReplyFragments = 32;

	/** The connection's CLIENT_RECEIVE events */
	TArray<FEvent> Chunks(FRig& Rig, int32 Conn)
	{
		return OfReason(Rig.Events(Conn), LWS_CALLBACK_CLIENT_RECEIVE);
	}

	TArray<uint8> Assemble(const TArray<FEvent>& Received)
	{
		TArray<uint8> Out;
		for (const FEvent& Each : Received)
		{
			Out.Append(Each.Bytes);
		}
		return Out;
	}

	/** "len[flags,remaining]" per chunk: F first, L final, B binary */
	FString Describe(const TArray<FEvent>& Received)
	{
		TArray<FString> Parts;
		for (const FEvent& Each : Received)
		{
			Parts.Add(FString::Printf(TEXT("%lld[%s%s%s,%lld]"), Each.Len, (Each.Flags & FirstFragment) != 0 ? TEXT("F") : TEXT(""),
				(Each.Flags & FinalFragment) != 0 ? TEXT("L") : TEXT(""), (Each.Flags & BinaryFrame) != 0 ? TEXT("B") : TEXT(""), Each.Remaining));
		}
		return FString::Join(Parts, TEXT(" "));
	}

	/** Indexes of the chunks with a flag set */
	FString Where(const TArray<FEvent>& Received, uint32 Flag)
	{
		TArray<FString> Parts;
		for (int32 Index = 0; Index < Received.Num(); ++Index)
		{
			if ((Received[Index].Flags & Flag) != 0)
			{
				Parts.Add(FString::FromInt(Index));
			}
		}
		return FString::Printf(TEXT("[%s] of %d"), *FString::Join(Parts, TEXT(",")), Received.Num());
	}

	FString Sha(TConstArrayView<uint8> Bytes)
	{
		return VaRest::Http::FSha256::Hash(Bytes).ToHex();
	}

	/** Connect, wait until Bytes arrived, close, wait for the end; what happened is noted, never failed (review M-2) */
	void Receive(FAutomationTestBase& Test, FRig& Rig, const FString& Path, int64 Bytes, int32& OutConn)
	{
		FConnectSpec Spec;
		Spec.Path = Path;
		OutConn = Rig.Connect(MoveTemp(Spec));
		const bool bGot = WaitBytes(Rig, OutConn, Bytes, 20.0);
		Rig.Close(OutConn, 1000, TEXT("done"));
		const bool bEnded = Rig.WaitEnded(OutConn, 30.0);
		NoteOutcome(Test, FString::Printf(TEXT("%s: %lld bytes %s, connection %s"), *Path, Bytes, bGot ? TEXT("arrived") : TEXT("did not arrive"), bEnded ? TEXT("ended") : TEXT("did not end")), bGot && bEnded);
	}

	/** The client's frames as the server saw them, "op/fin/len" (close frames left out) */
	FString ClientFrames(const FVaRestJson& Journal, bool bWithControl)
	{
		TArray<FString> Parts;
		for (const FVaRestJson& Frame : JournalEvents(Journal, TEXT("frame")))
		{
			const int32 Op = Frame[TEXT("op")].AsInt();
			if (Op == 8 || (!bWithControl && Op >= 8))
			{
				continue;
			}
			Parts.Add(FString::Printf(TEXT("%d/%d/%lld"), Op, Frame[TEXT("fin")].AsInt(), Frame[TEXT("len")].AsInt64()));
		}
		return FString::Join(Parts, TEXT(", "));
	}

	/** One text message of Fragments frames of FragmentBytes bytes of Byte, appended to an outbox (service thread) */
	void QueueTextMessage(TArray<FWrite>& Outbox, uint8 Byte, int32 FragmentBytes, int32 Fragments)
	{
		for (int32 Index = 0; Index < Fragments; ++Index)
		{
			FWrite& Item = Outbox.AddDefaulted_GetRef();
			Item.Protocol = (Index == 0 ? static_cast<int32>(LWS_WRITE_TEXT) : static_cast<int32>(LWS_WRITE_CONTINUATION))
				| (Index == Fragments - 1 ? 0 : static_cast<int32>(LWS_WRITE_NO_FIN));
			Item.Buffer.SetNumZeroed(LWS_PRE);
			Item.Buffer.AddUninitialized(FragmentBytes);
			FMemory::Memset(Item.Buffer.GetData() + LWS_PRE, Byte, FragmentBytes);
		}
	}

	TArray<uint8> Filled(int32 Num, uint8 Byte)
	{
		TArray<uint8> Out;
		Out.Init(Byte, Num);
		return Out;
	}

	/** The flood's size: more than loopback socket buffers hold even with raised tcp_wmem/tcp_rmem (review M-6) */
	constexpr int64 FloodBytes = 32 * 1024 * 1024;

	struct FFlood
	{
		bool bRan = false;
		int64 PauseReturned = MIN_int64;
		int64 AfterPause = 0;
		int32 AfterPauseCalls = 0;
		int64 ResumeReturned = MIN_int64;
		bool bResumed = false;
		bool bFloodLogged = false;
		double Longest = -1.0;
		int32 SlowWrites = 0;
		/** Set when the flood broke off ("flood_error"): what it wrote, how long it ran, the error */
		FString BrokeOff;
	};

	/**
	 * One round on its own rig: a flood paused with lws_rx_flow_control(wsi, 0) inside its first CLIENT_RECEIVE (where
	 * WebSocket spec 2.3 counts undelivered bytes), 2 s of watching, then lws_rx_flow_control(wsi, ResumeArg) from a
	 * command. bRan is false only when the harness failed (no rig, or the flood's first chunk never came)
	 */
	FFlood Flood(FAutomationTestBase& Test, const FServer& Site, int32 ResumeArg)
	{
		FFlood Out;
		FRig Rig(Site);
		if (!Test.TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
		{
			return Out;
		}
		const FString Key = NewKey();
		FConnectSpec Spec;
		Spec.Path = FString::Printf(TEXT("/ws/flood?bytes=%lld&chunk=65536&key=%s"), FloodBytes, *Key);
		Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
		{
			const FName PausedKey(TEXT("paused"));
			if (Reason == LWS_CALLBACK_CLIENT_RECEIVE && C.Counters.FindRef(PausedKey) == 0)
			{
				C.Counters.Add(PausedKey, 1);
				C.Rig->Record(C.Id, Marker, lws_rx_flow_control(Wsi, 0), TEXT("paused"));
			}
			return KeepDefault;
		};
		const int32 Conn = Rig.Connect(MoveTemp(Spec));
		if (!Test.TestTrue(TEXT("the flood's first chunk arrives"), Rig.WaitReason(Conn, Marker)))
		{
			return Out;
		}
		Out.bRan = true;
		FEvent Paused;
		Rig.Find(Conn, Marker, Paused, TEXT("paused"));
		Out.PauseReturned = Paused.Len;
		// Completion before silence: the pause is recorded; now the window in which no more should arrive
		FPlatformProcess::Sleep(2.0f);
		for (const FEvent& Each : Rig.Events(Conn))
		{
			if (Each.Reason == LWS_CALLBACK_CLIENT_RECEIVE && Each.Time > Paused.Time)
			{
				Out.AfterPause += Each.Len;
				++Out.AfterPauseCalls;
			}
		}
		Rig.ExecOn(Conn, TEXT("resume"), [ResumeArg](FConn& C) { C.Rig->Record(C.Id, Marker, lws_rx_flow_control(C.Wsi, ResumeArg), TEXT("resumed")); });
		Out.bResumed = WaitBytes(Rig, Conn, FloodBytes, 20.0);
		FEvent Resumed;
		if (Rig.Find(Conn, Marker, Resumed, TEXT("resumed")))
		{
			Out.ResumeReturned = Resumed.Len;
		}
		Rig.Close(Conn, 1000, TEXT("done"));
		NoteOutcome(Test, TEXT("the flood connection ended after our close"), Rig.WaitEnded(Conn, 30.0));
		const FVaRestJson Journal = FetchJournal(Test, Site, Key);
		// "flood" when every byte went out, "flood_error" when a write failed (Tools/testserver/ws.py:808, :815): a round whose
		// resume never came ends at our close with the server still writing. flood_error's longest_write_s leaves out the
		// write that failed, since ws_flood times a write only when it returns (ws.py:804-812), so only a "flood" carries
		// the timing the fact needs
		const TArray<FVaRestJson> Finished = JournalEvents(Journal, TEXT("flood"));
		const TArray<FVaRestJson> Failed = JournalEvents(Journal, TEXT("flood_error"));
		if (Finished.Num() > 0)
		{
			Out.bFloodLogged = true;
			Out.Longest = Finished[0][TEXT("longest_write_s")].AsDouble();
			Out.SlowWrites = Finished[0][TEXT("writes_over_100ms")].AsInt();
		}
		else if (Failed.Num() > 0)
		{
			Out.BrokeOff = FString::Printf(TEXT("the flood broke off after %lld bytes in %.2f s (%s); the write that failed is not timed"),
				Failed[0][TEXT("bytes")].AsInt64(), Failed[0][TEXT("seconds")].AsDouble(), *Failed[0][TEXT("text")].AsString());
		}
		return Out;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeReceiveChunkingTest, "Probe.VaRest.Lws.Data.ReceiveChunking",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeReceiveChunkingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	// The engine's ::FEvent (HAL/Event.h:21) is visible here too, so the directive alone makes FEvent ambiguous
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

	// A. 5 KiB of Cyrillic in frames cut inside a code point at about 4096 (review item 5, E9)
	const FString KeyCut = NewKey();
	int32 Cut = 0;
	Receive(*this, Rig, TEXT("/ws/fragment?cyrillic=5120&split_inside=4096&key=") + KeyCut, 5120, Cut);
	// B. one 200 000-byte frame
	int32 Single = 0;
	Receive(*this, Rig, TEXT("/ws/large?bytes=200000&ascii=1&key=") + NewKey(), 200000, Single);
	// C. a binary message in three frames
	int32 Binary = 0;
	Receive(*this, Rig, TEXT("/ws/fragment?bytes=10000&binary=1&at=3000,6000&key=") + NewKey(), 10000, Binary);
	// D. a PING from the server between two fragments of a text message
	const FString KeyPing = NewKey();
	int32 Pinged = 0;
	Receive(*this, Rig, TEXT("/ws/fragment?cyrillic=5120&split_inside=2000&ping_between=1&key=") + KeyPing, 5120, Pinged);
	// E. 400 small messages, text and binary in turn
	constexpr int32 OrderCount = 400;
	int64 OrderBytes = 0;
	for (int32 Index = 0; Index < OrderCount; ++Index)
	{
		OrderBytes += Index % 2 == 0 ? FString::Printf(TEXT("t%d"), Index).Len() : 4;
	}
	int32 Order = 0;
	Receive(*this, Rig, FString::Printf(TEXT("/ws/order?n=%d&key=%s"), OrderCount, *NewKey()), OrderBytes, Order);
	// F. invalid UTF-8 in a text message
	int32 Bad = 0;
	Receive(*this, Rig, TEXT("/ws/badutf8?where=text&key=") + NewKey(), 5, Bad);

	const FVaRestJson CutJournal = FetchJournal(*this, Site, KeyCut);
	const TArray<FVaRestJson> CutSent = JournalEvents(CutJournal, TEXT("message_sent"));
	const TArray<FEvent> CutChunks = Chunks(Rig, Cut);
	const TArray<uint8> CutBytes = Assemble(CutChunks);
	TArray<FString> InsideCodePoint;
	int64 Offset = 0;
	for (int32 Index = 0; Index + 1 < CutChunks.Num(); ++Index)
	{
		Offset += CutChunks[Index].Len;
		if (Offset < CutBytes.Num() && (CutBytes[static_cast<int32>(Offset)] & 0xC0) == 0x80)
		{
			InsideCodePoint.Add(FString::Printf(TEXT("%lld"), Offset));
		}
	}
	const FString CutAt = CutSent.Num() > 0 ? CutSent[0][TEXT("frames_at")].ToString() : FString(TEXT("?"));
	Fact(*this, TEXT("rx.frames_split_utf8"), EVerdict::Recorded,
		FString::Printf(TEXT("frames cut at %s; chunks %s; chunk ends inside a code point at [%s]"), *CutAt, *Describe(CutChunks), *FString::Join(InsideCodePoint, TEXT(","))),
		TEXT("WebSocket spec 2.3 and E9: one CLIENT_RECEIVE per frame end or full rx buffer; VaRest assembles bytes, so a split code point is harmless"),
		TEXT("/ws/fragment?cyrillic=5120&split_inside=4096"));
	const bool bOnlyLastFinal = CutChunks.Num() > 0 && Where(CutChunks, FinalFragment).Equals(FString::Printf(TEXT("[%d] of %d"), CutChunks.Num() - 1, CutChunks.Num()), ESearchCase::CaseSensitive);
	const int32 Completions = CutChunks.FilterByPredicate([](const FEvent& Each) { return (Each.Flags & FinalFragment) != 0 && Each.Remaining == 0; }).Num();
	Fact(*this, TEXT("rx.final_flag"), AgreesIf(bOnlyLastFinal && Completions == 1),
		FString::Printf(TEXT("lws_is_final_fragment on chunks %s; final with remaining 0 on %d chunk(s)"), *Where(CutChunks, FinalFragment), Completions),
		TEXT("WebSocket spec 2.3: a message is complete when lws_is_final_fragment and lws_remaining_packet_payload == 0 (T8)"),
		TEXT("the same message, two frames"));
	Fact(*this, TEXT("rx.first_flag"), EVerdict::Recorded, FString::Printf(TEXT("lws_is_first_fragment on chunks %s"), *Where(CutChunks, FirstFragment)),
		TEXT("none"), TEXT("the same message"));
	Fact(*this, TEXT("rx.bytes_intact"), AgreesIf(CutSent.Num() > 0 && Sha(CutBytes).Equals(CutSent[0][TEXT("sha256")].AsString(), ESearchCase::CaseSensitive)),
		FString::Printf(TEXT("%d bytes assembled, sha256 %s"), CutBytes.Num(), *Sha(CutBytes)),
		TEXT("WebSocket spec 2.3: chunks appended as bytes give the message exactly"), TEXT("against the server's message_sent sha256"));

	const TArray<FEvent> SingleChunks = Chunks(Rig, Single);
	bool bRemainingFalls = SingleChunks.Num() > 0 && SingleChunks.Last().Remaining == 0;
	for (int32 Index = 0; Index + 1 < SingleChunks.Num(); ++Index)
	{
		bRemainingFalls = bRemainingFalls && SingleChunks[Index].Remaining > 0 && (SingleChunks[Index].Flags & FinalFragment) == 0;
	}
	Fact(*this, TEXT("rx.chunk_sizes_single_frame"), EVerdict::Recorded, Describe(SingleChunks),
		TEXT("E9: a callback at every frame end and every 64 KiB (rx_buffer_size 65536)"), TEXT("/ws/large?bytes=200000: one frame"));
	Fact(*this, TEXT("rx.remaining_payload"), AgreesIf(bRemainingFalls),
		bRemainingFalls ? FString(TEXT("lws_remaining_packet_payload > 0 and no final flag until the last chunk, 0 there"))
			: FString::Printf(TEXT("not so: %s"), *Describe(SingleChunks)),
		TEXT("WebSocket spec 2.3: complete when final and remaining == 0 (T8; libwebsockets.h:5212-5232)"), TEXT("the same frame"));
	Fact(*this, TEXT("rx.binary_flag_on_continuations"), EVerdict::Recorded,
		FString::Printf(TEXT("lws_frame_is_binary on chunks %s"), *Where(Chunks(Rig, Binary), BinaryFrame)),
		TEXT("WebSocket spec 2.3: the kind comes from the first fragment"), TEXT("/ws/fragment?bytes=10000&binary=1&at=3000,6000"));

	const FVaRestJson PingJournal = FetchJournal(*this, Site, KeyPing);
	const TArray<FVaRestJson> PingSent = JournalEvents(PingJournal, TEXT("message_sent"));
	const TArray<uint8> PingBytes = Assemble(Chunks(Rig, Pinged));
	const bool bPongBetween = JournalEvents(PingJournal, TEXT("frame")).ContainsByPredicate([](const FVaRestJson& Each)
	{
		return Each[TEXT("op")].AsInt() == 10 && JournalBytes(Each) == Utf8Bytes(TEXT("between"));
	});
	const bool bPingIntact = PingSent.Num() > 0 && Sha(PingBytes).Equals(PingSent[0][TEXT("sha256")].AsString(), ESearchCase::CaseSensitive);
	Fact(*this, TEXT("rx.control_inside_fragmented"), AgreesIf(bPongBetween && bPingIntact),
		FString::Printf(TEXT("PONG for the PING between fragments: %s; message intact: %s"), YesNo(bPongBetween), YesNo(bPingIntact)),
		TEXT("RFC 6455 5.4: control frames may sit between fragments; WebSocket spec 2.3 assembles across them"),
		TEXT("/ws/fragment?cyrillic=5120&split_inside=2000&ping_between=1"));

	const TArray<FEvent> OrderChunks = Chunks(Rig, Order);
	int32 Mismatches = 0;
	int32 Position = 0;
	for (const FEvent& Each : OrderChunks)
	{
		const bool bWantBinary = Position % 2 == 1;
		const bool bMatches = bWantBinary
			? (Each.Flags & BinaryFrame) != 0 && Each.Bytes.Num() == 4
				&& ((static_cast<uint32>(Each.Bytes[0]) << 24) | (static_cast<uint32>(Each.Bytes[1]) << 16) | (static_cast<uint32>(Each.Bytes[2]) << 8) | Each.Bytes[3]) == static_cast<uint32>(Position)
			: (Each.Flags & BinaryFrame) == 0 && EventText(Each).Equals(FString::Printf(TEXT("t%d"), Position), ESearchCase::CaseSensitive);
		Mismatches += bMatches ? 0 : 1;
		++Position;
	}
	Fact(*this, TEXT("rx.order_preserved"), AgreesIf(Mismatches == 0 && OrderChunks.Num() == OrderCount),
		FString::Printf(TEXT("%d messages, %d out of place or wrong"), OrderChunks.Num(), Mismatches),
		TEXT("WebSocket spec 7.1: one ordered path; review item 4: count inversions between text and binary"), TEXT("/ws/order?n=400"));
	const TArray<uint8> BadBytes = Assemble(Chunks(Rig, Bad));
	const TArray<uint8> BadSent = { 0x6f, 0x6b, 0x20, 0xc3, 0x28 };
	Fact(*this, TEXT("rx.invalid_utf8_passthrough"), EVerdict::Recorded,
		FString::Printf(TEXT("delivered %s (%s); %s"), *HexOf(BadBytes), BadBytes == BadSent ? TEXT("unchanged") : TEXT("changed"), *CallSequence(Rig.Events(Bad))),
		TEXT("WebSocket spec 2.3: VaRest validates UTF-8 once on the assembled message, so LWS must pass the bytes through"),
		TEXT("/ws/badutf8: text 6f6b20c328"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeFlowControlTest, "Probe.VaRest.Lws.Data.FlowControl",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeFlowControlTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
	FServer Site;
	if (!FindServer(*this, Site))
	{
		return false;
	}
	// Round 1: the bool form, as the engine and WebSocket spec 2.3 write it. Round 2, its own rig and flood: the explicit
	// form for a change made outside the connection's callback (libwebsockets.h:5192-5194). lws_rx_flow_control turns a
	// bool into APPLIES | USER_BOOL | (enable << 13), and only APPLIES_ENABLE_BIT clears the bitmap (libwebsockets.c.o
	// 0x2570-0x25b6, review I-2), so the explicit resume is APPLIES_ENABLE, not APPLIES_DISABLE (libwebsockets.h:5164-5173)
	const FFlood BoolRound = Flood(*this, Site, 1);
	if (!BoolRound.bRan)
	{
		return false;
	}
	const FFlood NowRound = Flood(*this, Site, LWS_RXFLOW_REASON_APPLIES_ENABLE | LWS_RXFLOW_REASON_USER_BOOL | LWS_RXFLOW_REASON_FLAG_PROCESS_NOW);
	if (!NowRound.bRan)
	{
		return false;
	}
	Fact(*this, TEXT("rx.flow_pause_stops_callbacks"), AgreesIf(BoolRound.AfterPause <= 256 * 1024),
		FString::Printf(TEXT("%lld bytes in %d CLIENT_RECEIVE calls in the 2 s after lws_rx_flow_control(wsi, 0) (returned %lld); round 2: %lld bytes"),
			BoolRound.AfterPause, BoolRound.AfterPauseCalls, BoolRound.PauseReturned, NowRound.AfterPause),
		TEXT("T11 and WebSocket spec 2.3: lws_rx_flow_control(wsi, 0) stops reading, so TCP pushes back"),
		FString::Printf(TEXT("/ws/flood: %lld bytes as 64 KiB binary messages; paused inside the first CLIENT_RECEIVE"), FloodBytes));
	Fact(*this, TEXT("rx.flow_resume_from_command"), AgreesIf(BoolRound.bResumed || NowRound.bResumed),
		FString::Printf(TEXT("all %lld bytes arrived after a resume from a command: bool form %s, explicit form %s"), FloodBytes, YesNo(BoolRound.bResumed), YesNo(NowRound.bResumed)),
		TEXT("WebSocket spec 2.3: reading resumes (below half) from the service thread"), TEXT("the resume as a command, i.e. from EVENT_WAIT_CANCELLED, 2 s after the pause"));
	Fact(*this, TEXT("rx.flow_resume_form"), EVerdict::Recorded,
		FString::Printf(TEXT("lws_rx_flow_control(wsi, 1): %s (returned %lld); lws_rx_flow_control(wsi, APPLIES_ENABLE | USER_BOOL | FLAG_PROCESS_NOW): %s (returned %lld)"),
			BoolRound.bResumed ? TEXT("resumed reading") : TEXT("did not resume within 20 s"), BoolRound.ResumeReturned,
			NowRound.bResumed ? TEXT("resumed reading") : TEXT("did not resume within 20 s"), NowRound.ResumeReturned),
		TEXT("none"), TEXT("two rounds, each its own rig and flood; the return values are lws_rx_flow_control's"));
	// Only a flood that ran to its end timed every write: one that broke off, or never ended, is recorded, never "differs"
	Fact(*this, TEXT("rx.flow_backpressure_reached_server"), BoolRound.bFloodLogged ? AgreesIf(BoolRound.Longest >= 1.0) : EVerdict::Recorded,
		BoolRound.bFloodLogged ? FString::Printf(TEXT("the server's longest write blocked %.2f s; %d writes blocked over 100 ms"), BoolRound.Longest, BoolRound.SlowWrites)
			: (!BoolRound.BrokeOff.IsEmpty() ? BoolRound.BrokeOff : FString(TEXT("no flood or flood_error event in the journal"))),
		TEXT("WebSocket spec 2.3: with reading paused, TCP pushes back on the server"),
		FString::Printf(TEXT("the server's flood event, round 1 (%lld bytes: more than loopback socket buffers hold, review M-6)"), FloodBytes));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeFragmentedSendTest, "Probe.VaRest.Lws.Data.FragmentedSend",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeFragmentedSendTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
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
	Spec.Path = TEXT("/ws/sink?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// One 200 000-byte text message in four fragments, one per WRITEABLE (WebSocket spec 2.4), a PING after the second
	Rig.Write(Conn, LWS_WRITE_TEXT | LWS_WRITE_NO_FIN, Filled(50000, 'a'));
	Rig.Write(Conn, LWS_WRITE_CONTINUATION | LWS_WRITE_NO_FIN, Filled(50000, 'b'));
	Rig.Write(Conn, LWS_WRITE_PING, Utf8Bytes(TEXT("between")));
	Rig.Write(Conn, LWS_WRITE_CONTINUATION | LWS_WRITE_NO_FIN, Filled(50000, 'c'));
	Rig.Write(Conn, LWS_WRITE_CONTINUATION, Filled(50000, 'd'));
	NoteOutcome(*this, TEXT("all five writes were done"), Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == WriteDone; }).Num() >= 5;
	}));
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	const FString Data = ClientFrames(Journal, false);
	const FString WithPing = ClientFrames(Journal, true);
	Fact(*this, TEXT("tx.no_fin_frames"), AgreesIf(Data.Equals(TEXT("1/0/50000, 0/0/50000, 0/0/50000, 0/1/50000"), ESearchCase::CaseSensitive)), Data,
		TEXT("T10: LWS_WRITE_NO_FIN and LWS_WRITE_CONTINUATION give one frame each, as written (op/fin/len)"), TEXT("four fragments of 50 000 bytes"));
	Fact(*this, TEXT("tx.ping_between_fragments"), AgreesIf(WithPing.Equals(TEXT("1/0/50000, 0/0/50000, 9/1/7, 0/0/50000, 0/1/50000"), ESearchCase::CaseSensitive)), WithPing,
		TEXT("WebSocket spec 2.4: PINGs and the close frame can be written between fragments (RFC 6455 5.4)"), TEXT("a PING queued after the second fragment"));
	TArray<uint8> Whole = Filled(50000, 'a');
	Whole.Append(Filled(50000, 'b'));
	Whole.Append(Filled(50000, 'c'));
	Whole.Append(Filled(50000, 'd'));
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	Fact(*this, TEXT("tx.message_intact"), AgreesIf(Messages.Num() == 1 && Messages[0][TEXT("sha256")].AsString().Equals(Sha(Whole), ESearchCase::CaseSensitive)),
		FString::Printf(TEXT("%d message(s) at the server"), Messages.Num()), TEXT("none"), TEXT("the server's message sha256 against ours"));
	TArray<FString> Notes;
	for (const FEvent& Each : OfReason(Rig.Events(Conn), WriteDone))
	{
		Notes.Add(Each.Note);
	}
	Fact(*this, TEXT("tx.partial_after_fragment"), EVerdict::Recorded, FString::Join(Notes, TEXT("; ")),
		TEXT("T10 and WebSocket spec 2.4: lws_partial_buffered is checked after each write"), TEXT("the five writes"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeLargeWriteTest, "Probe.VaRest.Lws.Data.LargeWrite",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeLargeWriteTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
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
	Spec.Path = TEXT("/ws/sink?key=") + Key;
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	// One lws_write of 1 MiB: more than rx_buffer_size, which caps one send when tx_packet_size is 0 (libwebsockets.h:2346-2353)
	const TArray<uint8> Big = Filled(1024 * 1024, 'x');
	Rig.Write(Conn, LWS_WRITE_TEXT, Big);
	NoteOutcome(*this, TEXT("the write was done"), Rig.WaitReason(Conn, WriteDone));
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn, 30.0));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	FEvent Written;
	Rig.Find(Conn, WriteDone, Written);
	Fact(*this, TEXT("tx.large_write_result"), EVerdict::Recorded, FString::Printf(TEXT("lws_write(1 MiB): %s"), *Written.Note),
		TEXT("T10: LWS buffers a truncated write itself and suppresses WRITEABLE until it drains (libwebsockets.h:4914-4930)"), TEXT("one 1 MiB text write"));
	Fact(*this, TEXT("tx.large_write_frames"), EVerdict::Recorded, ClientFrames(Journal, false), TEXT("none"), TEXT("the server's frames (op/fin/len)"));
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	Fact(*this, TEXT("tx.large_write_intact"), AgreesIf(Messages.Num() == 1 && Messages[0][TEXT("sha256")].AsString().Equals(Sha(Big), ESearchCase::CaseSensitive)),
		FString::Printf(TEXT("%d message(s) at the server"), Messages.Num()), TEXT("none"), TEXT("the server's message sha256 against ours"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeTwoWritesTest, "Probe.VaRest.Lws.Data.TwoWritesPerWriteable",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeTwoWritesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
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
	Spec.Path = TEXT("/ws/sink?key=") + Key;
	// "libwebsockets-3.0 only allows us to send once per LWS_CALLBACK_*_WRITABLE event" (LwsWebSocket.cpp:195): is it so?
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		const FName Double(TEXT("double"));
		if (Reason == LWS_CALLBACK_CLIENT_WRITEABLE && C.Counters.FindRef(Double) == 0)
		{
			C.Counters.Add(Double, 1);
			C.Rig->WriteNow(C, Wsi, LWS_WRITE_PING, Utf8Bytes(TEXT("one")), TEXT("first"));
			C.Rig->WriteNow(C, Wsi, LWS_WRITE_TEXT, Utf8Bytes(TEXT("two")), TEXT("second"));
			return 0;
		}
		return KeepDefault;
	};
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	Rig.ExecOn(Conn, TEXT("kick"), [](FConn& C) { lws_callback_on_writable(C.Wsi); });
	NoteOutcome(*this, TEXT("both writes ran"), Rig.WaitFor([Conn](const TArray<FEvent>& All)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == WriteDone; }).Num() >= 2;
	}));
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	TArray<FString> Notes;
	for (const FEvent& Each : OfReason(Rig.Events(Conn), WriteDone))
	{
		Notes.Add(Each.Note);
	}
	const bool bPing = JournalEvents(Journal, TEXT("frame")).ContainsByPredicate([](const FVaRestJson& Each) { return Each[TEXT("op")].AsInt() == 9 && JournalBytes(Each) == Utf8Bytes(TEXT("one")); });
	const bool bText = JournalEvents(Journal, TEXT("message")).ContainsByPredicate([](const FVaRestJson& Each) { return Each[TEXT("text")].AsString().Equals(TEXT("two"), ESearchCase::CaseSensitive); });
	Fact(*this, TEXT("tx.two_writes_one_writeable"), EVerdict::Recorded,
		FString::Printf(TEXT("PING arrived: %s, text arrived: %s; %s"), YesNo(bPing), YesNo(bText), *FString::Join(Notes, TEXT("; "))),
		TEXT("WebSocket spec 2.4 writes one frame per WRITEABLE; the engine's comment says 3.0 allows only one (LwsWebSocket.cpp:195)"),
		TEXT("a PING and a text written in the same WRITEABLE"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeAutoReplyTest, "Probe.VaRest.Lws.Data.AutoReplyBoundary",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeAutoReplyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using namespace VaRest::LwsProbe::VaRestLwsProbeDataTestPrivate;
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
	// The server pushes "2" after our first data frame (a heartbeat, as Engine.IO's); we answer "3" at the next message
	// boundary, never inside message A, and message B goes in behind the answer (WebSocket spec 2.4 rev 2, 22 SW-5)
	Spec.Path = TEXT("/ws/sink?push=2&after_frames=1&key=") + Key;
	Spec.Hook = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		const FName Reply(TEXT("reply"));
		if (Reason == LWS_CALLBACK_CLIENT_RECEIVE && Len == 1 && static_cast<const uint8*>(In)[0] == '2' && C.Counters.FindRef(Reply) == 0)
		{
			C.Counters.Add(Reply, 1);
			// Len: the fragments of A still queued when the push arrived
			C.Rig->Record(C.Id, Marker, C.Outbox.Num(), TEXT("push"));
			lws_callback_on_writable(Wsi);
			return KeepDefault;
		}
		if (Reason == LWS_CALLBACK_CLIENT_WRITEABLE && C.Counters.FindRef(Reply) == 1)
		{
			if (C.Outbox.Num() == 0 || IsMessageStart(C.Outbox[0].Protocol))
			{
				C.Counters.Add(Reply, 2);
				const uint8 Three = '3';
				C.Rig->WriteNow(C, Wsi, LWS_WRITE_TEXT, TConstArrayView<uint8>(&Three, 1), TEXT("reply"));
				QueueTextMessage(C.Outbox, 'B', AutoReplyFragmentBytes, AutoReplyFragments);
				lws_callback_on_writable(Wsi);
				return 0;
			}
			// Review I-1: inside A the default write below sends the next fragment, and FRig::WriteNext re-arms only while
			// its outbox is non-empty; one more WRITEABLE, so this check runs again after A's last fragment
			lws_callback_on_writable(Wsi);
		}
		return KeepDefault;
	};
	const int32 Conn = Rig.Connect(MoveTemp(Spec));
	if (!TestTrue(TEXT("the connection opens"), Rig.WaitReason(Conn, LWS_CALLBACK_CLIENT_ESTABLISHED)))
	{
		return false;
	}
	Rig.ExecOn(Conn, TEXT("queue A"), [](FConn& C)
	{
		QueueTextMessage(C.Outbox, 'A', AutoReplyFragmentBytes, AutoReplyFragments);
		lws_callback_on_writable(C.Wsi);
	});
	const int32 Expected = AutoReplyFragments * 2 + 1;
	const bool bWritten = Rig.WaitFor([Conn, Expected](const TArray<FEvent>& All)
	{
		return All.FilterByPredicate([Conn](const FEvent& Each) { return Each.Conn == Conn && Each.Reason == WriteDone; }).Num() >= Expected;
	}, 30.0);
	NoteOutcome(*this, TEXT("A, the reply and B were all written"), bWritten);
	Rig.Close(Conn, 1000, TEXT("done"));
	NoteOutcome(*this, TEXT("the connection ended after our close"), Rig.WaitEnded(Conn, 30.0));
	const FVaRestJson Journal = FetchJournal(*this, Site, Key);
	if (!TestTrue(TEXT("the server's journal arrived"), Journal.IsValid()))
	{
		return false;
	}
	const TArray<FVaRestJson> Messages = JournalEvents(Journal, TEXT("message"));
	TArray<FString> Order;
	for (const FVaRestJson& Each : Messages)
	{
		Order.Add(Each.Has(TEXT("text")) ? FString::Printf(TEXT("\"%s\""), *Each[TEXT("text")].AsString()) : FString::Printf(TEXT("%lld bytes"), Each[TEXT("len")].AsInt64()));
	}
	const int64 MessageBytes = static_cast<int64>(AutoReplyFragmentBytes) * AutoReplyFragments;
	const bool bBetween = Messages.Num() == 3 && Messages[0][TEXT("len")].AsInt64() == MessageBytes
		&& Messages[1][TEXT("text")].AsString().Equals(TEXT("3"), ESearchCase::CaseSensitive) && Messages[2][TEXT("len")].AsInt64() == MessageBytes;
	FEvent Push;
	FEvent Replied;
	const bool bPushed = Rig.Find(Conn, Marker, Push, TEXT("push"));
	const bool bTimed = bPushed && Rig.Find(Conn, WriteDone, Replied, TEXT("reply"));
	// Review I-1: the boundary is exercised only when the push lands while fragments of A are still queued
	const bool bExercised = bPushed && Push.Len > 0;
	const FString AtServer = FString::Printf(TEXT("messages at the server: %s"), *FString::Join(Order, TEXT(", ")));
	Fact(*this, TEXT("tx.autoreply_between_messages"), bExercised ? AgreesIf(bBetween) : EVerdict::Recorded,
		bExercised ? FString::Printf(TEXT("%s; the push arrived with %lld of A's %d fragments still queued"), *AtServer, Push.Len, AutoReplyFragments)
			: FString::Printf(TEXT("%s; push arrived after A (%s): boundary not exercised"), *AtServer, bPushed ? TEXT("no fragment of A queued") : TEXT("no push seen")),
		TEXT("WebSocket spec 2.4 rev 2 (22 SW-5): a reply is written at the next message boundary, never inside a fragmented message"),
		TEXT("A and B: 2 MiB text in 32 fragments each; the reply \"3\" to the server's \"2\"; the boundary rule is the rig's own (the hook writes the reply only when the outbox head starts a message), so the row checks LWS's write order and the delay"));
	Fact(*this, TEXT("tx.autoreply_delay_ms"), EVerdict::Recorded,
		bTimed ? FString::Printf(TEXT("%.1f ms from the push to the reply's lws_write"), (Replied.Time - Push.Time) * 1000.0) : FString(TEXT("not measured")),
		TEXT("none"), TEXT("bounded by the fragments of A still queued"));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestLwsProbeSoSndbufTest, "Probe.VaRest.Lws.Data.SoSndbuf",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestLwsProbeSoSndbufTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::LwsProbe;
	using VaRest::LwsProbe::FEvent;
	const TCHAR* const SndbufSpec = TEXT("WebSocket spec 2.4 rev 2 (22 SW-5): connections with auto-reply rules get SO_SNDBUF 256 KiB through lws_get_socket_fd + setsockopt");
#if PLATFORM_LINUX || PLATFORM_MAC
	FServer Site;
	if (!FindServer(*this, Site, true))
	{
		return false;
	}
	FRigOptions Options;
	Options.Tls = ETls::LwsGlobalInit;
	FRig Rig(Site, MoveTemp(Options));
	if (!TestTrue(TEXT("the probe's context exists"), Rig.IsReady()))
	{
		return false;
	}
	const FHook SetSndbuf = [](FConn& C, lws* Wsi, int32 Reason, void* User, void* In, size_t Len) -> int32
	{
		if (Reason == LWS_CALLBACK_CLIENT_ESTABLISHED)
		{
			const lws_sockfd_type Fd = lws_get_socket_fd(Wsi);
			int Asked = 256 * 1024;
			const int Set = setsockopt(Fd, SOL_SOCKET, SO_SNDBUF, &Asked, sizeof(Asked));
			int Got = 0;
			socklen_t GotLen = sizeof(Got);
			getsockopt(Fd, SOL_SOCKET, SO_SNDBUF, &Got, &GotLen);
			C.Rig->Record(C.Id, Marker, Got, FString::Printf(TEXT("setsockopt(SO_SNDBUF, 262144) returned %d on fd %d; getsockopt reports %d"), Set, static_cast<int32>(Fd), Got));
		}
		return KeepDefault;
	};
	FConnectSpec Plain;
	Plain.Hook = SetSndbuf;
	FConnectSpec Tls;
	Tls.bTls = true;
	Tls.SslFlags = LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	Tls.Hook = SetSndbuf;
	TArray<FEvent> PlainSeen;
	TArray<FEvent> TlsSeen;
	Visit(*this, Rig, Plain, PlainSeen);
	Visit(*this, Rig, Tls, TlsSeen);
	const FEvent* PlainMark = PlainSeen.FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker; });
	const FEvent* TlsMark = TlsSeen.FindByPredicate([](const FEvent& Each) { return Each.Reason == Marker; });
	Fact(*this, TEXT("tx.so_sndbuf_ws"), EVerdict::Recorded, PlainMark ? PlainMark->Note : FString::Printf(TEXT("never opened: %s"), *ErrorOf(PlainSeen)), SndbufSpec, TEXT("ws://, in CLIENT_ESTABLISHED"));
	Fact(*this, TEXT("tx.so_sndbuf_wss"), EVerdict::Recorded, TlsMark ? TlsMark->Note : FString::Printf(TEXT("never opened: %s"), *ErrorOf(TlsSeen)), SndbufSpec, TEXT("wss:// (the TCP socket under TLS), in CLIENT_ESTABLISHED"));
	return true;
#else
	Fact(*this, TEXT("tx.so_sndbuf_ws"), EVerdict::Unverified, TEXT("unverified here; release-pass item (Win64): setsockopt takes a SOCKET and a char* there"), SndbufSpec, TEXT("this platform"));
	Fact(*this, TEXT("tx.so_sndbuf_wss"), EVerdict::Unverified, TEXT("unverified here; release-pass item (Win64)"), SndbufSpec, TEXT("this platform"));
	return true;
#endif
}

#endif // WITH_DEV_AUTOMATION_TESTS
