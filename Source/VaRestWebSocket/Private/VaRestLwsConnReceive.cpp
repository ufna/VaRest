// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "VaRestJsonUtf8.h"
#include "VaRestWebSocketLog.h"

int32 VaRest::WebSocket::Lws::FConn::OnReceive(FService& Service, lws* InWsi, const void* In, size_t Len)
{
	// A connection that is closing (ours, or a protocol failure) delivers nothing more
	if (State != EConnState::Open || bCloseQueued)
	{
		return 0;
	}
	if (!bAssembling)
	{
		bAssembling = true;
		// The kind from the first fragment (spec 2.3); LWS repeats the flag on every chunk [rx.binary_flag_on_continuations]
		AssemblyKind = lws_frame_is_binary(InWsi) ? EVaRestSocketFrameKind::Binary : EVaRestSocketFrameKind::Text;
		Assembly.Reset();
	}
	const int64 Remaining = static_cast<int64>(lws_remaining_packet_payload(InWsi));
	// The size limit as chunks arrive (spec 2.3): over it, the message is dropped and the connection closed with 1009
	if (static_cast<int64>(Assembly.Num()) + static_cast<int64>(Len) > Request.MaxMessageBytes)
	{
		Fail(Service, 1009, TEXT("message too big"), EVaRestSocketFailure::MessageTooBig);
		return 0;
	}
	// Bytes, appended (spec 2.3): nothing is converted per chunk, so a code point cut by a chunk or a frame is whole again (E9).
	// Room for what arrived and the slack byte, grown geometrically from the bytes received, so many 64 KiB chunks are not
	// copied once each -- never from the frame's declared length, which would let a server make every connection commit up
	// to MaxMessageBytes by declaring a huge frame and sending one byte (review conc M-13)
	const int64 Need = FMath::Min<int64>(static_cast<int64>(Assembly.Num()) + static_cast<int64>(Len) + 1, Request.MaxMessageBytes + 1);
	if (Assembly.Max() < Need)
	{
		Assembly.Reserve(static_cast<int32>(FMath::Min<int64>(FMath::Max<int64>(Need, static_cast<int64>(Assembly.Max()) * 2), Request.MaxMessageBytes + 1)));
	}
	if (In && Len > 0)
	{
		Assembly.Append(static_cast<const uint8*>(In), static_cast<int32>(Len));
	}
	// Complete when the frame is final and nothing of it remains (T8) [rx.final_flag, rx.remaining_payload]
	if (!lws_is_final_fragment(InWsi) || Remaining != 0)
	{
		return 0;
	}
	bAssembling = false;
	if (AssemblyKind == EVaRestSocketFrameKind::Text
		&& !VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Assembly.GetData()), Assembly.Num())))
	{
		// Once, on the whole message, with VaRestJson's validator (spec 2.3, core 2.2): invalid closes with 1007 (RFC 6455 8.1)
		Fail(Service, 1007, TEXT("invalid UTF-8"), EVaRestSocketFailure::InvalidUtf8);
		return 0;
	}
	FVaRestSocketEvent Message;
	Message.Type = EVaRestSocketEventType::Message;
	Message.Kind = AssemblyKind;
	// The slack byte for FVaRestJson::ParseOwned: reserved above, and ensured by the link (Task 3)
	Message.Bytes = MoveTemp(Assembly);
	Link->Push(MoveTemp(Message));
	Assembly.Reset();
	return 0;
}

void VaRest::WebSocket::Lws::FConn::Fail(FService& Service, int32 Code, const TCHAR* Reason, EVaRestSocketFailure Failure)
{
	// The partial message and what was queued to send are dropped and the close goes next: between two of our fragments if need
	// be, which RFC 6455 5.4 allows a control frame (plan decision 18)
	Assembly.Empty();
	bAssembling = false;
	DropOutbox();
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: %s, closing with %d"), *GetLabel(), LexToString(Failure), Code);
	bool bTruncated = false;
	Close(Service, Code, CloseReasonBytes(Reason, bTruncated), EVaRestSocketCloseBy::Transport, Failure);
}
