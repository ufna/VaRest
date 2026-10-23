// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

// FConn's write path (WebSocket spec 2.4, 2.5): one lws_write per WRITEABLE, a message in fragments of FragmentBytes

#include "VaRestLwsConn.h"

#include "HAL/PlatformTime.h"
#include "VaRestWebSocketLog.h"

void VaRest::WebSocket::Lws::FConn::Send(FService& Service, FVaRestSocketFrame&& Frame, int64 BufferedAfterSend)
{
	if (State == EConnState::Ended || bCloseQueued)
	{
		// Too late: its bytes leave the buffered amount, and no Drained follows an end
		Link->BufferedBytes.fetch_sub(Frame.Length);
		return;
	}
	Outbox.Add(MoveTemp(Frame));
	// The sender's own count (Task 3 review I-1): earlier frames may have left since, and a re-read here would miss a
	// crossing the app saw. This frame's bytes stay counted until it is released, so the note comes before any return to 0
	Link->NoteBuffered(BufferedAfterSend, Request.SendHighWater);
	// Before the open LWS would drop the request (spec 2.4): OnEstablished asks for what is queued (plan decision 18)
	if (State == EConnState::Open)
	{
		lws_callback_on_writable(Wsi);
	}
}

void VaRest::WebSocket::Lws::FConn::Close(FService& Service, int32 Code, TArray<uint8>&& Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure)
{
	// Never gated on Service.IsShuttingDown() (Task 6 review M-7): Shutdown's flush runs these to write the closes asked for
	if (State == EConnState::Ended || bCloseQueued)
	{
		return;
	}
	if (State != EConnState::Open)
	{
		// No close frame before the open: the attempt is abandoned (spec 3.1; plan decision 5)
		EndPreOpen(EVaRestSocketFailure::Canceled, By, 0, TEXT("closed before the open"));
		Kill(Service);
		return;
	}
	bCloseQueued = true;
	CloseCode = Code;
	CloseReason = MoveTemp(Reason);
	CloseBy = By;
	CloseFailure = Failure;
	// Review conc M-3: Shutdown's flush waits for this close frame (at most ExitFlushSeconds); UncountClose lowers it once,
	// when the frame is written or the connection ends first
	bCloseCounted = true;
	Service.GetCounters().UnsentCloses.fetch_add(1);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: closing with %d"), *GetLabel(), Code);
	// After the data already queued (plan decision 18)
	lws_callback_on_writable(Wsi);
}

int32 VaRest::WebSocket::Lws::FConn::OnWriteable(lws* InWsi)
{
	// After our close frame, 0 from every WRITEABLE: LWS then waits for the echo, up to its own 5 s (T20; plan decision 4)
	if (bCloseSent || State != EConnState::Open)
	{
		return 0;
	}
	// LWS calls WRITEABLE only after a truncated write drained (libwebsockets.h:4914-4930)
	ReleaseHeld();
	if (Outbox.Num() > 0)
	{
		FVaRestSocketFrame& Head = Outbox[0];
		// Plan decision 6: fragment k is written from the frame's storage, its header in the 16 bytes before it -- the tail of
		// fragment k-1, which LWS has sent or copied into its own buffer (T10, libwebsockets.h:4914-4930); fragment 0's goes
		// into the frame's headroom (Offset >= FrameHeadroom), and LWS masks each payload in place (RFC 6455 5.3). FragmentBytes
		// each (spec 2.4), so a control frame can go between them (RFC 6455 5.4) [tx.ping_between_fragments]: W1b's PING, or a
		// failure close written after DropOutbox. Our Close() waits for the whole outbox (plan decision 18, Send.CloseAfterData)
		// FragmentStart and FragmentSize, never Start or Size: a local named like a member function of FConn (Start) is
		// C4458, an error on MSVC that clang's -Wshadow does not report (review spec I-3)
		const int64 FragmentStart = static_cast<int64>(NextFragment) * FragmentBytes;
		const int32 FragmentSize = static_cast<int32>(FMath::Min<int64>(FragmentBytes, Head.Length - FragmentStart));
		const bool bLast = FragmentStart + FragmentSize >= Head.Length;
		int32 Protocol = NextFragment == 0 ? (Head.Kind == EVaRestSocketFrameKind::Text ? LWS_WRITE_TEXT : LWS_WRITE_BINARY) : LWS_WRITE_CONTINUATION;
		if (!bLast)
		{
			// T10: NO_FIN, then CONTINUATION, one frame each, as written [tx.no_fin_frames]
			Protocol |= LWS_WRITE_NO_FIN;
		}
		// One lws_write per WRITEABLE: a second in one callback returns -1 and is lost (spec 2.4) [tx.two_writes_one_writeable]
		const int Wrote = lws_write(InWsi, Head.Storage.GetData() + Head.Offset + FragmentStart, static_cast<size_t>(FragmentSize), static_cast<lws_write_protocol>(Protocol));
		if (bLast)
		{
			Outbox.RemoveAt(0);
			NextFragment = 0;
		}
		else
		{
			++NextFragment;
		}
		if (Wrote < 0)
		{
			// This fragment is neither in the outbox nor released: it leaves the count here, the rest in DropOutbox (EndOpen)
			Link->BufferedBytes.fetch_sub(FragmentSize);
			EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None, TEXT("lws_write failed"));
			return -1;
		}
		// A data write returns its payload length, a control frame's counts its header: only < 0 fails (spec 2.4)
		// [tx.partial_after_fragment]. lws_partial_buffered says LWS still holds part of this one [tx.large_write_result]
		if (lws_partial_buffered(InWsi))
		{
			HeldBytes += FragmentSize;
		}
		else
		{
			Link->ReleaseBuffered(FragmentSize);
		}
		if (Outbox.Num() > 0 || bCloseQueued || HeldBytes > 0)
		{
			lws_callback_on_writable(InWsi);
		}
		return 0;
	}
	if (bCloseQueued)
	{
		// Our close (spec 2.5, T20): its code and reason (libwebsockets.h:831-845), -1 from this WRITEABLE and 0 from every
		// one after it, so LWS waits for the peer's echo [close.ours_noecho_wait_ms, close.ours_sequence]
		lws_close_reason(InWsi, static_cast<lws_close_status>(CloseCode), CloseReason.GetData(), static_cast<size_t>(CloseReason.Num()));
		bCloseSent = true;
		// LWS's own 5 s close-ack timer starts with this frame (T20): Task 9's OnEnded reads it (review conc I-5)
		CloseSentAt = FPlatformTime::Seconds();
		UncountClose();
		return -1;
	}
	return 0;
}

void VaRest::WebSocket::Lws::FConn::ReleaseHeld()
{
	// lws_partial_buffered (libwebsockets.h:6020-6037): set while LWS still holds part of the last write
	if (HeldBytes > 0 && Wsi && !lws_partial_buffered(Wsi))
	{
		const int64 Bytes = HeldBytes;
		HeldBytes = 0;
		Link->ReleaseBuffered(Bytes);
	}
}

void VaRest::WebSocket::Lws::FConn::DropOutbox()
{
	int64 Bytes = HeldBytes;
	for (int32 Index = 0; Index < Outbox.Num(); ++Index)
	{
		// The fragments of the head already written were released (or are held, counted above)
		Bytes += Outbox[Index].Length - (Index == 0 ? static_cast<int64>(NextFragment) * FragmentBytes : 0);
	}
	Outbox.Empty();
	NextFragment = 0;
	HeldBytes = 0;
	if (Bytes > 0)
	{
		Link->BufferedBytes.fetch_sub(Bytes);
	}
}
