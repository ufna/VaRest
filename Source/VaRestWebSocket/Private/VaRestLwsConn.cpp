// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "HAL/PlatformTime.h"
#include "VaRestLwsErrors.h"
#include "VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate
{
	FString Utf8ToString(const TArray<uint8>& Bytes)
	{
		return Bytes.Num() > 0 ? FString::ConstructFromPtrSize(reinterpret_cast<const UTF8CHAR*>(Bytes.GetData()), Bytes.Num()) : FString();
	}

	/**
	 * The Detail of an end that reports the peer's close code, from both OnEnded paths that do (Task 9 review M2). Task 2
	 * review M-4: a normalised code is named as received -- a number, never the reason, which is payload
	 */
	FString PeerCloseDetail(const VaRest::WebSocket::FPeerClose& Received)
	{
		if (Received.bCodeNormalised)
		{
			return FString::Printf(TEXT("the server sent close code %d, which a peer must not send; reported as %d"), Received.ReceivedCode, Received.Code);
		}
		return Received.bMalformed ? FString(TEXT("the server's close payload was one byte; reported as 1002")) : FString();
	}
}

VaRest::WebSocket::Lws::FConn::FConn(uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink, FVaRestSocketConnectRequest&& InRequest)
	: Id(InId)
	, Link(MoveTemp(InLink))
	, Request(MoveTemp(InRequest))
{
}

VaRest::WebSocket::Lws::FConn::~FConn()
{
	UncountTls();
}

FString VaRest::WebSocket::Lws::FConn::MakeLabel(uint64 InId, const FVaRestSocketConnectRequest& InRequest)
{
	return InRequest.LogName.IsEmpty() ? FString::Printf(TEXT("ws#%llu"), static_cast<unsigned long long>(InId)) : InRequest.LogName;
}

void VaRest::WebSocket::Lws::FConn::MarkTlsCounted()
{
	check(IsInGameThread());
	if (!bTlsCounted)
	{
		TlsHandshakesInFlight().fetch_add(1);
		bTlsCounted = true;
	}
}

VaRest::WebSocket::Lws::FConn* VaRest::WebSocket::Lws::FConn::Find(FService& Service, uint64 InId)
{
	return static_cast<FConn*>(Service.Find(InId));
}

void VaRest::WebSocket::Lws::FConn::Prepare(const FServiceOptions& Options)
{
	check(IsInGameThread());
	// CloseTimeout (spec 2.5, 15): a value that is not a positive number is LWS's own close-ack wait (plan decision 17)
	if (!FMath::IsFinite(Request.CloseTimeout) || Request.CloseTimeout <= 0.0)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: CloseTimeout %g s is not a positive number; %g s is used"), *GetLabel(), Request.CloseTimeout, LwsCloseAckSeconds);
		Request.CloseTimeout = LwsCloseAckSeconds;
	}
	TValueOrError<FSocketTarget, FTargetError> Made = PrepareTarget(Request);
	if (Made.HasError())
	{
		PendingFailure = Made.GetError().Failure;
		PendingDetail = Made.GetError().Message;
		return;
	}
	Target = Made.StealValue();
	// Spec 2.2 (LwsWebSocket.cpp:112-122): the engine's URL filter, read here on the game thread; its refusal is a queued Error (E7)
	if (!IsAllowedByFilter(Target.FilterUrl, Options.bOverrideAllowlist ? &Options.Allowlist : nullptr))
	{
		PendingFailure = EVaRestSocketFailure::NotAllowed;
		PendingDetail = TEXT("the URL request filter ([Online.HttpManager <scheme>] AllowedDomains in the engine ini) refuses this host");
		return;
	}
	Utf8Copy(Options.TcpAddressOverride.IsEmpty() ? Target.Host : Options.TcpAddressOverride, AddressUtf8);
	Utf8Copy(Target.PathAndQuery, PathUtf8);
	Utf8Copy(Target.HostHeader, HostUtf8);
	if (!Target.Origin.IsEmpty())
	{
		Utf8Copy(Target.Origin, OriginUtf8);
	}
	if (!Target.Protocols.IsEmpty())
	{
		Utf8Copy(Target.Protocols, ProtocolsUtf8);
	}
}

void VaRest::WebSocket::Lws::FConn::Start(FService& Service)
{
	Owner = &Service;
	StartedAt = FPlatformTime::Seconds();
	if (Service.IsShuttingDown())
	{
		// Task 6 review M-7: adopted once Shutdown has begun -- the flush, or Shutdown's own drain on the game thread. LWS 3.0
		// resolves the host synchronously on this thread, which would hold the flush and the join, so it never connects
		EndPreOpen(EVaRestSocketFailure::Shutdown, EVaRestSocketCloseBy::Transport, 0, TEXT("the transport shut down before the connect"));
		UncountTls();
		bDestroyed = true;
		return;
	}
	if (PendingFailure != EVaRestSocketFailure::None)
	{
		EndPreOpen(PendingFailure, EVaRestSocketCloseBy::Transport, 0, PendingDetail);
		bDestroyed = true;
		return;
	}
	lws_client_connect_info Info = {};
	Info.context = Service.GetContext();
	Info.address = AddressUtf8.GetData();
	Info.port = Target.Port;
	Info.ssl_connection = Target.bTls ? LCCSCF_USE_SSL : 0;
	if (Target.bTls && Target.bInsecure)
	{
		// Spec 2.2: a self-signed chain allowed and the host name not checked; ALLOW_SELFSIGNED alone fails an IP host
		// [tls.insecure_ip_host, tls.insecure_skip_hostname]
		Info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
	}
	Info.path = PathUtf8.GetData();
	Info.host = HostUtf8.GetData();
	// Sent as given: the context has JUST_USE_RAW_ORIGIN (T4, libwebsockets.h:2618-2623) [hs.raw_origin]; null omits it [hs.origin_omitted]
	Info.origin = OriginUtf8.Num() > 0 ? OriginUtf8.GetData() : nullptr;
	Info.protocol = ProtocolsUtf8.Num() > 0 ? ProtocolsUtf8.GetData() : nullptr;
	// T3, T17: bound to VaRest's handler whatever the server selects, so no protocol needs registering (E17)
	// [hs.unregistered_selected_bound; libwebsockets.h:3455-3461]
	Info.local_protocol_name = "varest";
	Info.ietf_version_or_minus_one = -1;
	// FService::FindForCallback matches LWS's userdata against this IServiceConnection pointer by address (VaRestLwsService.h)
	Info.userdata = static_cast<IServiceConnection*>(this);
	// The wsi early, so a kill reaches an attempt that has not opened yet (libwebsockets.h:3441-3451)
	Info.pwsi = &Wsi;
	State = EConnState::Connecting;
	// Review conc I-3 (VaRest-rti): a wss attempt was counted by FTransport::Connect on the game thread (MarkTlsCounted,
	// re-review N-2) and stays counted until LWS establishes or destroys it -- the whole upgrade, not only the TLS handshake:
	// conservative, and simple. UncountTls lowers it once
	// Ruling Q5: LWS 3.0 resolves the address itself, synchronously, here on the service thread (getaddrinfo inside
	// lws_client_connect_2, review conc I-4), and tries the first address it returns only; W1b B9 (VaRest-gi0) resolves on
	// UE::Tasks and falls back to the next address
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: connecting"), *GetLabel());
	if (!lws_client_connect_via_info(&Info))
	{
		// NULL: scrubbed (libwebsockets.h:3442-3450), maybe after a CONNECTION_ERROR inside the call that reported already;
		// *pwsi may name freed memory, so it is never touched again
		Wsi = nullptr;
		EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("lws_client_connect_via_info returned NULL"));
		UncountTls();
		bDestroyed = true;
	}
}

int32 VaRest::WebSocket::Lws::FConn::OnCallback(FService& Service, lws* InWsi, int32 Reason, void* User, void* In, size_t Len)
{
	switch (Reason)
	{
	case LWS_CALLBACK_CLIENT_APPEND_HANDSHAKE_HEADER:
		return OnAppendHeaders(In, Len);
	case LWS_CALLBACK_CLIENT_FILTER_PRE_ESTABLISH:
		return OnFilterPreEstablish(InWsi);
	case LWS_CALLBACK_CLIENT_ESTABLISHED:
		return OnEstablished(InWsi);
	case LWS_CALLBACK_CLIENT_CONNECTION_ERROR:
		OnConnectionError(In, Len);
		return 0;
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		return OnWriteable(InWsi);
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		return OnPeerClose(In, Len);
	case LWS_CALLBACK_CLIENT_CLOSED:
	case LWS_CALLBACK_CLOSED_CLIENT_HTTP:
		// A client ends with CLIENT_CLOSED, never CLOSED (T29, close.peer_sequence); a handshake VaRest refused, with
		// CLOSED_CLIENT_HTTP and no CONNECTION_ERROR [hs.redirect_veto]
		OnEnded();
		return 0;
	case LWS_CALLBACK_WSI_DESTROY:
		OnDestroyed(Service);
		return 0;
	default:
		return 0;
	}
}

int32 VaRest::WebSocket::Lws::FConn::OnAppendHeaders(void* In, size_t Len)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an attempt VaRest already ended (cancelled, released, timed out) never sends its request, headers
		// and all. The kill may not have landed: LWS's own stage timers overwrite it (lws_generate_client_handshake calls
		// lws_set_timeout(wsi, 0, 0)). A non-zero return here refuses the client before the request goes out [hs.redirect_veto]
		return -1;
	}
	++AppendCalls;
	if (Target.HeaderBlock.Num() == 0)
	{
		return 0;
	}
	// in: a char** to the next free byte; len: the room left (T5, libwebsockets.h:1316-1343) [hs.append_header_space]. The
	// whole block or nothing: never past the room (E19)
	if (static_cast<size_t>(Target.HeaderBlock.Num()) > Len)
	{
		PendingFailure = EVaRestSocketFailure::BadHandshake;
		PendingDetail = FString::Printf(TEXT("the headers need %d bytes and the handshake buffer has %llu left"), Target.HeaderBlock.Num(), static_cast<unsigned long long>(Len));
		// A non-zero return refuses the client, with no CONNECTION_ERROR after it [hs.redirect_veto]: OnEnded reports PendingFailure
		return -1;
	}
	char** Cursor = static_cast<char**>(In);
	FMemory::Memcpy(*Cursor, Target.HeaderBlock.GetData(), Target.HeaderBlock.Num());
	*Cursor += Target.HeaderBlock.Num();
	return 0;
}

int32 VaRest::WebSocket::Lws::FConn::OnFilterPreEstablish(lws* InWsi)
{
	if (State == EConnState::Ended)
	{
		// Review conc I-1: an ended attempt never opens ("HS: Rejected by filter cb")
		return -1;
	}
	// The last point the response headers exist (T6, libwebsockets.h:1301-1307): the subprotocol the server selected (T17)
	// [hs.selected_protocol_in_filter]
	const int Total = lws_hdr_total_length(InWsi, WSI_TOKEN_PROTOCOL);
	if (Total > 0)
	{
		TArray<ANSICHAR> Selected;
		Selected.SetNumZeroed(Total + 1);
		if (lws_hdr_copy(InWsi, Selected.GetData(), Selected.Num(), WSI_TOKEN_PROTOCOL) > 0)
		{
			SelectedProtocol = UTF8_TO_TCHAR(Selected.GetData());
		}
	}
	return 0;
}

int32 VaRest::WebSocket::Lws::FConn::OnEstablished(lws* InWsi)
{
	// The upgrade is done: no verify callback of this attempt runs again (review conc I-3)
	UncountTls();
	if (State == EConnState::Ended)
	{
		// Ended meanwhile (a deadline, a release): the open is refused, and nothing more is reported
		return -1;
	}
	State = EConnState::Open;
	FVaRestSocketEvent Opened;
	Opened.Type = EVaRestSocketEventType::Open;
	Opened.Protocol = SelectedProtocol;
	Opened.Seconds = FPlatformTime::Seconds() - StartedAt;
	// Whether a subprotocol was selected, never which: it is a header value, and subprotocols can carry bearer tokens (Task 4 review I-2)
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: open after %.1f ms (%s)"), *GetLabel(), Opened.Seconds * 1000.0,
		SelectedProtocol.IsEmpty() ? TEXT("no subprotocol") : TEXT("a subprotocol selected"));
	Link->Push(MoveTemp(Opened));
	// LWS 3.0 drops a WRITEABLE request made during the handshake (spec 2.4; Probe.VaRest.Lws.Rig.WriteBeforeOpen,
	// VaRestLwsProbe.cpp:606-613): ask again for what was queued meanwhile (plan decision 18)
	if (Outbox.Num() > 0 || bCloseQueued)
	{
		lws_callback_on_writable(InWsi);
	}
	return 0;
}

void VaRest::WebSocket::Lws::FConn::OnConnectionError(const void* In, size_t Len)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	// in: libwebsockets' error string, len its length; either may be empty (libwebsockets.h:1255-1299)
	const FString Text = In && Len > 0 ? FString::ConstructFromPtrSize(static_cast<const UTF8CHAR*>(In), static_cast<int32>(Len)) : FString();
	if (PendingFailure != EVaRestSocketFailure::None)
	{
		// VaRest's own decision explains this end better than LWS's string (plan decisions 10, 11); the string is cleaned
		// and capped all the same (Task 5 review M-4)
		EndPreOpen(PendingFailure, EVaRestSocketCloseBy::Transport, 0, FString::Printf(TEXT("%s (libwebsockets: \"%s\")"), *PendingDetail, *CleanConnectionErrorText(Text)));
		return;
	}
	// Plan decision 11, Task 5's table: every class keeps the cleaned string in Detail, Unknown included (Task 5 review M-4)
	FConnectionErrorEnd Described = DescribeConnectionError(Text, FPlatformTime::Seconds() - StartedAt);
	EndPreOpen(Described.Failure, Described.By, Described.HttpStatus, MoveTemp(Described.Detail));
}

int32 VaRest::WebSocket::Lws::FConn::OnPeerClose(const void* In, size_t Len)
{
	// in: the close frame's payload, its code first in network order (T12, libwebsockets.h:1217-1225); LWS 3.0 rewrote some
	// codes to 1002 before this [close.rewritten_codes]. An empty payload is 1005, never two bytes read past it (E27,
	// LwsWebSocket.cpp:351) [close.empty_len]. Recorded only: CLIENT_CLOSED follows and OnEnded reports it [close.peer_sequence]
	Peer = ParsePeerClose(TConstArrayView<uint8>(static_cast<const uint8*>(In), In ? static_cast<int32>(Len) : 0));
	bPeerClosed = true;
	if (Peer.bReasonRepaired && State == EConnState::Open)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: the server's close reason is not UTF-8; ill-formed bytes are shown as U+FFFD"), *GetLabel());
	}
	// 0: LWS echoes the close (RFC 6455 5.5.1) [close.echo_on_return_0]; the engine returns 1 (LwsWebSocket.cpp:389-390),
	// so it never answers [close.return_1_echo]
	return 0;
}

void VaRest::WebSocket::Lws::FConn::OnEnded()
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate;
	if (State == EConnState::Ended)
	{
		return;
	}
	if (State != EConnState::Open)
	{
		// No CONNECTION_ERROR came: VaRest refused the handshake itself (a non-zero return, hs.redirect_veto), or LWS gave up silently
		const bool bOwn = PendingFailure != EVaRestSocketFailure::None;
		EndPreOpen(bOwn ? PendingFailure : EVaRestSocketFailure::Unknown, EVaRestSocketCloseBy::Transport, 0,
			bOwn ? PendingDetail : FString(TEXT("the connection ended before the open without an error string")));
		return;
	}
	if (bCloseQueued)
	{
		// Our close (spec 2.5): LWS 3.0 raises no callback for the peer's reply [close.ours_sequence], so an end before
		// CloseTimeout counts as clean (plan decision 4). The peer's own close, crossing ours before it went out, ends it
		// cleanly too, reported as the peer's; a close that went out neither way ended unclean. Review conc I-5: an end by
		// LWS's own close-ack timer (T20) with no close from the peer is the unanswered close CloseTimeout exists for: not
		// clean. Task 9 review M1: that timer ends the connection only more than LwsCloseAckSeconds after it was armed -- it
		// counts whole seconds from the second it was armed in (the re-arm note in CheckDeadlines), walks once a second
		// (T26) and expires strictly after them: [close.ours_noecho_wait_ms] measured 5526 ms with 100 ms waits, which a
		// ">=" expiry could not give -- and it is armed after CloseSentAt is read, when LWS writes the frame. So an end
		// before LwsCloseAckSeconds is never the timer's and needs no margin: an answer 4.8 s after our close is clean
		// (Timeout.CloseAnsweredLate). An end at or past it is reported unclean, even an answer that just beat the timer:
		// VaRest cannot see the reply
		const double SinceCloseSent = FPlatformTime::Seconds() - CloseSentAt;
		const bool bAckTimedOut = bCloseSent && !bPeerClosed && SinceCloseSent >= LwsCloseAckSeconds;
		const bool bPeerFirst = !bCloseSent && bPeerClosed;
		const int32 Code = bCloseSent ? CloseCode : (bPeerClosed ? Peer.Code : 1006);
		FString Reason = bCloseSent ? Utf8ToString(CloseReason) : (bPeerClosed ? Peer.Reason : FString());
		// Re-review N-8, Task 9 review M1: worded by what VaRest knows -- it saw no close from the server, and cannot tell
		// an end by LWS's timer from an answer just before it
		FString Detail = bAckTimedOut
			? FString::Printf(TEXT("the connection ended %.1f s after our close frame with no close seen from the server; libwebsockets' close-ack timer ends an unanswered close after 5 s"), SinceCloseSent)
			: bPeerFirst ? PeerCloseDetail(Peer) : FString();
		EndOpen(Code, MoveTemp(Reason), (bCloseSent || bPeerClosed) && !bAckTimedOut, CloseBy, CloseFailure, MoveTemp(Detail));
		return;
	}
	if (bPeerClosed)
	{
		// The peer's close, echoed (OnPeerClose returned 0)
		EndOpen(Peer.Code, Peer.Reason, true, EVaRestSocketCloseBy::Server, EVaRestSocketFailure::None, PeerCloseDetail(Peer));
		return;
	}
	// An end without a close frame (spec 2.5): FIN and RST both give CLIENT_CLOSED [close.drop_reports_client_closed]
	EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None);
}

void VaRest::WebSocket::Lws::FConn::OnDestroyed(FService& Service)
{
	// The connection's last callback: whatever has not reported does now, once
	OnEnded();
	UncountTls();
	UncountClose();
	if (bKillIssued)
	{
		const double Took = FPlatformTime::Seconds() - KillAt;
		FServiceCounters& Counters = Service.GetCounters();
		Counters.KillsLanded.fetch_add(1);
		Counters.LastKillToDestroySeconds = Took;
		if (Took > Counters.MaxKillToDestroySeconds.load())
		{
			Counters.MaxKillToDestroySeconds = Took;
		}
	}
	Wsi = nullptr;
	bDestroyed = true;
}

void VaRest::WebSocket::Lws::FConn::Kill(FService& Service)
{
	if (!Wsi || bKillIssued)
	{
		return;
	}
	bKillIssued = true;
	KillAt = FPlatformTime::Seconds();
	KillArmedAt = KillAt;
	Service.GetCounters().KillsIssued.fetch_add(1);
	// T14, T26: the connection ends at LWS's next timeout walk (libwebsockets.h:4673-4679); the wait stays short until it
	// does (WantsShortWait, VaRest-wcv)
	lws_set_timeout(Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC);
}

void VaRest::WebSocket::Lws::FConn::CheckDeadlines(FService& Service, double Now)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate;
	if (bKillIssued && !bDestroyed && Wsi && Now - KillArmedAt >= KillRearmSeconds)
	{
		// Review conc I-1: LWS's own stage timers replace a pending timeout (lws_client_socket_service and
		// lws_generate_client_handshake call lws_set_timeout themselves), so a kill that has not landed one timeout walk
		// after it was armed (T26) is armed again, until WSI_DESTROY. Re-review N-1: not on every pass -- each
		// lws_set_timeout restarts the timeout at the current second (__lws_set_timeout stores time() as
		// pending_timeout_set, libwebsockets.c.o 0x1275-0x1287), so a re-arm made after a second changed and before that
		// second's walk would postpone the kill again and again while the service thread is busy in callbacks. Outside any
		// callback, as libwebsockets.h:4673-4679 allows for LWS_TO_KILL_ASYNC
		lws_set_timeout(Wsi, PENDING_TIMEOUT_USER_REASON_BASE, LWS_TO_KILL_ASYNC);
		KillArmedAt = Now;
	}
	if (bDestroyed || State == EConnState::Ended)
	{
		return;
	}
	if (State == EConnState::Open && bCloseQueued && Now >= CloseDeadline)
	{
		// Spec 2.5: CloseTimeout cuts the close VaRest started; reported at once, then killed (plan decision 3). Never gated
		// on Service.IsShuttingDown(): a close Shutdown's flush waits for keeps its bound (Task 6 review M-7)
		EndOpen(bCloseSent ? CloseCode : 1006, bCloseSent ? Utf8ToString(CloseReason) : FString(), false, CloseBy, CloseFailure,
			FString::Printf(TEXT("CloseTimeout (%g s) passed without the connection ending"), Request.CloseTimeout));
		Kill(Service);
		return;
	}
	if (State == EConnState::Open)
	{
		// LWS calls WRITEABLE only once a held write drained; this sweep releases it even when no WRITEABLE was asked for (plan decision 7)
		ReleaseHeld();
	}
}

double VaRest::WebSocket::Lws::FConn::NextDeadline() const
{
	// Task 11 adds the connect deadline beside it
	double Next = TNumericLimits<double>::Max();
	if (State == EConnState::Open && bCloseQueued)
	{
		Next = CloseDeadline;
	}
	return Next;
}

bool VaRest::WebSocket::Lws::FConn::WantsShortWait() const
{
	return (bKillIssued && !bDestroyed) || HeldBytes > 0;
}

void VaRest::WebSocket::Lws::FConn::EndForShutdown()
{
	// The thread is stopped and the context is destroyed next, on this thread, before any other context can be made: no
	// handshake of this attempt runs again, so it leaves the count here even if lws_context_destroy calls no WSI_DESTROY for it
	UncountTls();
	if (State == EConnState::Open)
	{
		EndOpen(1006, FString(), false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::Shutdown, TEXT("the transport shut down"));
	}
	else
	{
		EndPreOpen(EVaRestSocketFailure::Shutdown, EVaRestSocketCloseBy::Transport, 0, TEXT("the transport shut down"));
	}
}

void VaRest::WebSocket::Lws::FConn::Release(FService& Service)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	// Spec 1.4: the last handle is gone -- an open connection is closed with 1001 "released", a pending one cancelled (Close
	// does both); the link discards what follows. Built by the one helper that builds our close (Task 7 review M-2)
	FOutboundClose Released = MakeOutboundClose(GetLabel(), 1001, TEXT("released"));
	Close(Service, Released.Code, MoveTemp(Released.Reason), EVaRestSocketCloseBy::Client, EVaRestSocketFailure::None);
}

void VaRest::WebSocket::Lws::FConn::UncountTls()
{
	if (bTlsCounted)
	{
		bTlsCounted = false;
		TlsHandshakesInFlight().fetch_sub(1);
	}
}

void VaRest::WebSocket::Lws::FConn::UncountClose()
{
	if (bCloseCounted && Owner)
	{
		bCloseCounted = false;
		Owner->GetCounters().UnsentCloses.fetch_sub(1);
	}
}

void VaRest::WebSocket::Lws::FConn::EndPreOpen(EVaRestSocketFailure Failure, EVaRestSocketCloseBy By, int32 HttpStatus, FString Detail)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	State = EConnState::Ended;
	DropOutbox();
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Error;
	Event.End.By = By;
	Event.End.Failure = Failure;
	Event.End.HttpStatus = HttpStatus;
	Event.End.Detail = MoveTemp(Detail);
	// Detail never quotes a URL, a header value or the path (FTargetError::Message), and LWS's string is cleaned (Task 5 review M-4)
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: never opened: %s, %s"), *GetLabel(), LexToString(Failure), *Event.End.Detail);
	Link->Push(MoveTemp(Event));
}

void VaRest::WebSocket::Lws::FConn::EndOpen(int32 Code, FString Reason, bool bClean, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure, FString Detail)
{
	if (State == EConnState::Ended)
	{
		return;
	}
	// Nothing more will be written: Shutdown's flush stops waiting for this close (review conc M-3)
	UncountClose();
	State = EConnState::Ended;
	DropOutbox();
	FVaRestSocketEvent Event;
	Event.Type = EVaRestSocketEventType::Closed;
	Event.End.Code = Code;
	Event.End.Reason = MoveTemp(Reason);
	Event.End.bWasClean = bClean;
	Event.End.By = By;
	Event.End.Failure = Failure;
	Event.End.Detail = MoveTemp(Detail);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: closed %d by %s%s"), *GetLabel(), Code, LexToString(By), bClean ? TEXT(", clean") : TEXT(""));
	Link->Push(MoveTemp(Event));
}
