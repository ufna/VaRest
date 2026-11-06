// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"

#include "HAL/PlatformTime.h"
#include "VaRestLwsErrors.h"
#include "VaRestWebSocketLog.h"
#include "VaRestJsonUtf8.h"
#include "VaRestSocketQueryWarning.h"
#if !PLATFORM_WINDOWS
#include <sys/socket.h>
#include <errno.h>
#endif

namespace VaRest::WebSocket::Lws::VaRestLwsConnPrivate
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    enum : uint32 {
        NativeSubmitAttempted1=1, NativeTicketOwned1=2, NativeDiscardAttempted1=4,
        NativeInterestRetained1=8, NativeAuthorized1=16, NativePendingOpen1=32,
        NativeOptionAttempted1=64, NativeEpisodeFailed1=128, NativeRetiredBinding1=256,
        NativeCancelAttempted1=512
    };
#endif
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

VaRest::WebSocket::Lws::FOurCloseEnd VaRest::WebSocket::Lws::ClassifyOurCloseEnd(bool bCloseWritten, bool bPeerCloseRead, double SinceCloseWritten)
{
	// LWS 3.0 raises no callback for the peer's reply to our close [close.ours_sequence], so an end before CloseTimeout counts
	// as clean (plan decision 4). The peer's own close, crossing ours before it went out, ends it cleanly too; a close that
	// went out neither way ended unclean. Review conc I-5: an end by LWS's own close-ack timer (T20) with no close from the
	// peer is the unanswered close CloseTimeout exists for: not clean. Task 9 review M1: that timer ends the connection only
	// more than LwsCloseAckSeconds after it was armed -- it counts whole seconds of time() from the second it was armed in
	// (the re-arm note in FConn::CheckDeadlines), walks once a second (T26) and expires strictly after them:
	// [close.ours_noecho_wait_ms] measured 5526 ms with 100 ms waits, which a ">=" expiry could not give -- and it is armed
	// after CloseSentAt is read, when LWS writes the frame. So, absent a wall-clock step (LWS counts in time(), CloseSentAt
	// is monotonic: an NTP step forward could end an unanswered close sooner, reported clean), an end before
	// LwsCloseAckSeconds is not the timer's and needs no margin: an answer 4.76 s after our close is clean
	// (Timeout.CloseAnsweredLate, Lws.Conn.CloseAck). An end at or past it is reported unclean, even an answer that just beat
	// the timer: VaRest cannot see the reply
	FOurCloseEnd Out;
	Out.bAckTimedOut = bCloseWritten && !bPeerCloseRead && SinceCloseWritten >= LwsCloseAckSeconds;
	Out.bClean = (bCloseWritten || bPeerCloseRead) && !Out.bAckTimedOut;
	return Out;
}

VaRest::WebSocket::Lws::FConnectTimeoutClamp VaRest::WebSocket::Lws::ClampConnectTimeout(double Asked)
{
	// Spec 2.2: the context's timeout_secs backs up every socket, so a longer value is clamped (plan decision 17); so is one
	// that is not a positive number. NaN fails every comparison, hence IsFinite first
	FConnectTimeoutClamp Out;
	Out.bWarn = !FMath::IsFinite(Asked) || Asked <= 0.0 || Asked > MaxConnectTimeoutSeconds;
	Out.Seconds = Out.bWarn ? MaxConnectTimeoutSeconds : Asked;
	return Out;
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
#if WITH_DEV_AUTOMATION_TESTS
	if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->bRetired.store(true, std::memory_order_release);
#endif
}

#if WITH_DEV_AUTOMATION_TESTS
bool VaRest::WebSocket::Lws::FConn::MatchesPressureProbeForTests(const PressureTesting::FProbe& Probe) const
{
	return Link->GetPressureProbeForTests().Get() == &Probe && Id == Probe.Connection.load(std::memory_order_acquire);
}

VaRest::WebSocket::PressureTesting::FSnapshot VaRest::WebSocket::Lws::FConn::PressureSnapshotForTests(uint64 RequestId, uint64 Pass) const
{
	PressureTesting::FSnapshot Out;
	if (const auto& Probe = Link->GetPressureProbeForTests()) Out.Owner = Probe->Owner;
	Out.Connection = Id; Out.Request = RequestId; Out.Pass = Pass;
	Out.Queued = Link->GetQueuedEventCount(); Out.Bytes = Link->PendingInboundBytes.load(std::memory_order_acquire);
	Out.Messages = Link->PendingInboundMessages.load(std::memory_order_acquire);
	Out.ReplyDebt = AutoReplyDebt.Num(); Out.bPaused = bReadPaused;
	return Out;
}
#endif

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
	if (!Options.QueuedEvents.IsValid() || Request.MaxPendingInboundBytes < 0 || !FMath::IsFinite(Request.PingInterval) || Request.PingInterval < 0.0
		|| !FMath::IsFinite(Request.PingTimeout) || Request.PingTimeout < 0.05 || Request.AutoReplies.Num() > 16)
	{
		PendingFailure = EVaRestSocketFailure::BadHandshake; PendingDetail = TEXT("invalid service heartbeat, pressure or reply configuration"); return;
	}
	for (int32 Index = 0; Index < Request.AutoReplies.Num(); ++Index)
	{
		const auto& Rule = Request.AutoReplies[Index];
		if (Rule.Match.Num() > 4096 || Rule.Reply.Num() > 4096
			|| !VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Rule.Match.GetData()), Rule.Match.Num()))
			|| !VaRest::Json::IsValidUtf8(FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Rule.Reply.GetData()), Rule.Reply.Num())))
		{ PendingFailure = EVaRestSocketFailure::BadHandshake; PendingDetail = TEXT("automatic text reply rules require UTF-8 values of at most 4096 bytes"); return; }
		for (int32 Previous = 0; Previous < Index; ++Previous) if (Request.AutoReplies[Previous].Match == Rule.Match)
		{ PendingFailure = EVaRestSocketFailure::BadHandshake; PendingDetail = TEXT("automatic text reply rules have duplicate matches"); return; }
	}
	check(IsInGameThread());
	// CloseTimeout (spec 2.5, 15): a value that is not a positive number is LWS's own close-ack wait (plan decision 17)
	if (!FMath::IsFinite(Request.CloseTimeout) || Request.CloseTimeout <= 0.0)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: CloseTimeout %g s is not a positive number; %g s is used"), *GetLabel(), Request.CloseTimeout, LwsCloseAckSeconds);
		Request.CloseTimeout = LwsCloseAckSeconds;
	}
	const FConnectTimeoutClamp Clamped = ClampConnectTimeout(Request.ConnectTimeout);
	if (Clamped.bWarn)
	{
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: ConnectTimeout %g s is outside (0, %g]; %g s is used"), *GetLabel(), Request.ConnectTimeout, MaxConnectTimeoutSeconds, Clamped.Seconds);
	}
	Request.ConnectTimeout = Clamped.Seconds;
	// A TArray holds the assembly (plan decision 17)
	if (Request.MaxMessageBytes < 1 || Request.MaxMessageBytes > static_cast<int64>(MAX_int32) - 1)
	{
		const int64 ClampedMessageLimit = FMath::Clamp<int64>(Request.MaxMessageBytes, 1, static_cast<int64>(MAX_int32) - 1);
		UE_LOG(LogVaRestSocket, Warning, TEXT("%s: MaxMessageBytes %lld is outside [1, %d]; %lld is used"), *GetLabel(), static_cast<long long>(Request.MaxMessageBytes), MAX_int32 - 1, static_cast<long long>(ClampedMessageLimit));
		Request.MaxMessageBytes = ClampedMessageLimit;
	}
	TValueOrError<FSocketTarget, FTargetError> Made = PrepareTarget(Request);
	if (Made.HasError())
	{
		PendingFailure = Made.GetError().Failure;
		PendingDetail = Made.GetError().Message;
		return;
	}
	Target = Made.StealValue();
	WarnForSharedQueryLogging(Target.PathAndQuery);
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
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (Service.NativeMode()) { StartNative(Service); return; }
#endif
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
	ConnectDeadline = StartedAt + Request.ConnectTimeout;
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

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
namespace VaRest::WebSocket::Lws
{
void FConn::StartNative(FService& Service)
{
    using namespace VaRestLwsConnPrivate;
    Native.Stage = 1; State = EConnState::Connecting;
    uint64_t Started = 0;
    const auto Stamp = NumericConnectorC1::BorrowedTurnTimestamp1(Service.NativeBorrow(), Service.NativeBinding(), &Started);
    const uint64 Interval = static_cast<uint64>(Request.ConnectTimeout * 1000000000.0);
    if (Stamp != NumericConnectorC1::EStatus1::OK || !Interval || Started > UINT64_MAX - Interval) {
        EndPreOpen(EVaRestSocketFailure::BadHandshake, EVaRestSocketCloseBy::Transport, 0, TEXT("native original deadline unavailable"));
        Native.Stage = 9; bDestroyed = true; return;
    }
    Native.DeadlineNs = Started + Interval; // ONCE. UE time is only public elapsed/close telemetry.
    Native.Flags |= NativeSubmitAttempted1;
    const VrStatus Raw = Service.SubmitNativeResolve(Target.Host, static_cast<uint16>(Target.Port), Native.DeadlineNs, Native.Ticket);
    if (Raw == VR_STATUS_OK && Service.NativeTicketMatches(Native.Ticket)) { Native.Flags |= NativeTicketOwned1; return; }
    if (Native.Ticket.owner_epoch || Native.Ticket.service_epoch || Native.Ticket.slot_generation || Native.Ticket.slot || Native.Ticket.reserved)
        Native.Flags |= NativeInterestRetained1; // Preserve malformed/uncertain actual output, never resubmit.
    EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("native resolver submission did not issue a valid ticket"));
    bDestroyed = true;
    if (!(Native.Flags & NativeInterestRetained1)) Native.Stage = 9;
}
bool FConn::NativeInterestUncertain() const
{ return (Native.Flags & VaRestLwsConnPrivate::NativeInterestRetained1) != 0; }
void FConn::RetireNativeInterest(FService& Service)
{
    using namespace VaRestLwsConnPrivate;
    if (!(Native.Flags & NativeTicketOwned1) || (Native.Flags & NativeDiscardAttempted1)) return;
    Native.Flags |= NativeDiscardAttempted1; // Actual once-only callsite, NO BUSY retry.
    const VrStatus Raw = Service.DiscardNativeResolve(Native.Ticket);
    if (Raw == VR_STATUS_OK) Native.Flags &= ~NativeTicketOwned1;
    else Native.Flags |= NativeInterestRetained1;
}
void FConn::AdvanceNative(FService& Service)
{
    using namespace VaRestLwsConnPrivate;
    namespace N = NumericConnectorC1;
    if (!Native.Stage || Native.Stage == 9 || Owner != &Service || FConn::Find(Service, Id) != this || !Service.NativeBorrow()) return;
    uint64_t Captured = 0;
    if (N::BorrowedTurnTimestamp1(Service.NativeBorrow(), Service.NativeBinding(), &Captured) != N::EStatus1::OK) return;
    if (State == EConnState::Connecting && (Captured >= Native.DeadlineNs || Service.IsShuttingDown())) {
        EndPreOpen(Service.IsShuttingDown() ? EVaRestSocketFailure::Shutdown : EVaRestSocketFailure::ConnectTimeout,
            Service.IsShuttingDown() ? EVaRestSocketCloseBy::Transport : EVaRestSocketCloseBy::Timeout, 0, TEXT("the original native connect interval ended"));
    }
    if (State == EConnState::Ended) {
        RetireNativeInterest(Service);
        if (Native.Reservation.Generation && !(Native.Flags & NativeCancelAttempted1)) {
            // No tracked client was ever obtained in stages2/3. That actual
            // source boundary supplies all-associated-WSI retirement, not NULL.
            if (!Wsi && Native.Stage == 2) {
                if (N::ReleaseUnbound1(Service.NativeObject(), Service.NativeBinding(), &Native.Reservation) != N::EStatus1::OK) return;
                Native.Flags |= NativeRetiredBinding1;
            } else if (!Wsi && Native.Stage == 3) {
                if (N::RetireBound1(Service.NativeObject(), Service.NativeBinding(), &Native.Binding) != N::EStatus1::OK) return;
                Native.Flags |= NativeRetiredBinding1;
            }
            Native.Flags |= NativeCancelAttempted1;
            (void)N::Cancel1(Service.NativeObject(), Service.NativeBinding(), &Native.Reservation);
        }
        if (Wsi && !bDestroyed) Kill(Service);
        if (!Wsi && !NativeInterestUncertain() && (!Native.Reservation.Generation
            || ((Native.Flags & NativeRetiredBinding1) && N::NativeHalfReusable1(Service.NativeObject(), Service.NativeBinding(), &Native.Reservation) == N::EStatus1::OK))) {
            Native.Stage = 9; bDestroyed = true;
        }
    }
    if (Native.Stage == 1 && State == EConnState::Connecting) {
        N::FAttempt1 Attempt {}; Attempt.Size = sizeof(Attempt); Attempt.Version = 1;
        Attempt.OriginalDeadlineNs = Native.DeadlineNs; Attempt.Mode = 0;
        const FString Authority = FString::Printf(TEXT("%s:%d"),
            *(Target.Host.Contains(TEXT(":")) ? FString::Printf(TEXT("[%s]"), *Target.Host) : Target.Host), Target.Port);
        FTCHARToUTF8 Encoded(*Authority);
        if (Encoded.Length() < 1 || Encoded.Length() > 512) { EndPreOpen(EVaRestSocketFailure::BadHandshake, EVaRestSocketCloseBy::Transport, 0, TEXT("native authority exceeds the fixed bound")); return; }
        Attempt.AuthorityBytes = Encoded.Length(); FMemory::Memcpy(Attempt.Authority, Encoded.Get(), Attempt.AuthorityBytes);
        uint32_t Slot = 2;
        if (N::PrepareResolveCopySlot1(Service.NativeBorrow(), Service.NativeBinding(), Native.Ticket.slot, &Slot) != N::EStatus1::OK) return;
        // NativeCopyResult2 consumes a completed slot. There is NO successful
        // re-copy/discard-after-copy and no result/candidate array retained here.
        VrResult Result {}; Result.size = sizeof(Result); Result.version = VR_OWNER_VERSION;
        const VrStatus Raw = Service.CopyNativeResolve(Native.Ticket, Result);
        if (Raw == VR_STATUS_PENDING || Raw == VR_STATUS_BUSY) return;
        const bool Consumed = Result.size == sizeof(Result) && Result.version == VR_OWNER_VERSION
            && FMemory::Memcmp(&Result.ticket, &Native.Ticket, sizeof(VrTicket)) == 0
            && (Raw == VR_STATUS_OK || Raw == VR_STATUS_NATIVE_ERROR);
        if (Consumed || Raw == VR_STATUS_EXPIRED) Native.Flags &= ~NativeTicketOwned1;
        if (Raw != VR_STATUS_OK || Result.size != sizeof(Result) || Result.version != VR_OWNER_VERSION
            || Result.count == 0 || Result.count > 8 || Result.resolver_error
            || FMemory::Memcmp(&Result.ticket, &Native.Ticket, sizeof(VrTicket)) != 0) {
            EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("native resolution failed or returned an incoherent result"));
            RetireNativeInterest(Service); if (!NativeInterestUncertain()) { bDestroyed = true; Native.Stage = 9; } return;
        }
        for (uint32 I = 0; I < Result.count; ++I) {
            const VrAddress& A = Result.address[I];
            if (A.size != sizeof(A) || A.version != VR_OWNER_VERSION || (A.family != 4 && A.family != 6)
                || A.reserved16 || A.port_be == 0 || A.port_be != static_cast<uint16>((Target.Port << 8) | (Target.Port >> 8))
                || A.reserved8[0] || A.reserved8[1] || A.reserved8[2] || A.reserved8[3]) {
                EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("native address result is malformed")); return;
            }
            auto& E = Attempt.Candidates[I]; E.Family = A.family; E.Scope = A.scope; E.PortBe = A.port_be;
            FMemory::Memcpy(E.Address, A.address, sizeof(E.Address));
        }
        Attempt.CandidateCount = Result.count; Attempt.Route = Slot + 1;
        N::FReservation1 Issued {}; Issued.Size = sizeof(Issued); Issued.Version = 1;
        const auto Reserved = N::ReserveFirst1(Service.NativeObject(), Service.NativeBinding(), Slot, &Attempt, &Issued);
        if (Reserved == N::EStatus1::OK) { Native.Reservation = Issued; Native.Stage = 2; }
        else {
            // Preflight+same actor make local room stable. Actual Stop/clock or
            // protocol failure ENDS this attempt, never re-copies a consumed result.
            EndPreOpen(Reserved == N::EStatus1::STOPPING ? EVaRestSocketFailure::Shutdown : EVaRestSocketFailure::ConnectFailed,
                EVaRestSocketCloseBy::Transport, 0, TEXT("native post-copy reservation could not complete"));
            RetireNativeInterest(Service);
            if (!NativeInterestUncertain()) { Native.Stage = 9; bDestroyed = true; }
        }
        return; // Both transient records retire; successful copy already consumed its interest.
    }
    if (Native.Stage == 2 && State == EConnState::Connecting) {
        Native.Binding = {sizeof(N::FBinding1),1,Native.Reservation.Slot,0,Native.Reservation.Generation,Native.Reservation.Route,Id};
        const auto Bound = N::Bind1(Service.NativeObject(), Service.NativeBinding(), &Native.Binding);
        if (Bound == N::EStatus1::OK) Native.Stage = 3;
        else if (Bound != N::EStatus1::PENDING) EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("native binding refused"));
        return;
    }
    if (Native.Stage == 3 && State == EConnState::Connecting) {
        lws_client_connect_info Info {}; Info.context = Service.GetContext(); Info.vhost = Service.NativeVhost(Native.Binding.Slot);
        FTCHARToUTF8 OriginalAddress(*Target.Host);
        Info.address = OriginalAddress.Get(); Info.port = Target.Port; // ORIGINAL target; C's owned internal route only changes TCP.
        Info.path = PathUtf8.GetData(); Info.host = HostUtf8.GetData();
        Info.origin = OriginUtf8.Num() ? OriginUtf8.GetData() : nullptr;
        Info.protocol = ProtocolsUtf8.Num() ? ProtocolsUtf8.GetData() : nullptr;
        Info.local_protocol_name = "varest"; Info.ietf_version_or_minus_one = -1;
        Info.userdata = static_cast<IServiceConnection*>(this); Info.pwsi = &Wsi;
        Info.ssl_connection = Target.bTls ? LCCSCF_USE_SSL : 0;
        if (Target.bTls && Target.bInsecure) Info.ssl_connection |= LCCSCF_ALLOW_SELFSIGNED | LCCSCF_SKIP_SERVER_CERT_HOSTNAME_CHECK;
        VrLwsResult1 Actual {};
        const int32 Raw = varest_lws_connect_client_tracked1(&Info, Service.NativeView(), Service.NativeBorrow(), &Actual);
        // Source-owned partial logical generation stays associated even on denial.
        if (Actual.RawObject) { Wsi = static_cast<lws*>(Actual.RawObject); Native.Stage = 4; }
        else if (Raw != VR_LWS_DEFERRED_BEFORE_ACQUIRE1) EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("tracked native client creation refused"));
        if (Raw == VR_LWS_RELEASE_UNCERTAIN1 || Raw == VR_LWS_PROTOCOL_FAULT1 || Actual.Size != sizeof(Actual)
            || Actual.Version != 1 || Actual.Disposition != static_cast<uint32>(Raw)) Native.Flags |= NativeInterestRetained1;
        return;
    }
    if (Native.Stage == 4 && Wsi && !bDestroyed && !(Native.Flags & NativeAuthorized1)) {
        N::FClaim1 Claims[4] {}; uint32 Count = 0;
        const auto Copied = N::CopyClaims1(Service.NativeObject(), Claims, 4, &Count);
        if (Copied != N::EStatus1::OK || Count > 4) return;
        for (uint32 I = 0; I < Count; ++I) {
            const auto& C = Claims[I];
            if (C.Conn != Id || C.Slot != Native.Binding.Slot || C.Generation != Native.Binding.Generation || C.Route != Native.Binding.Route) continue;
            // Genuine owner-map/Conn/WSI/binding membership precedes fd getter.
            if (FConn::Find(Service, Id) != this || Owner != &Service || !Wsi || bDestroyed) return;
            const int32 Fd = lws_get_socket_fd(Wsi);
            const auto Authorized = N::AuthorizeCurrent1(Service.NativeObject(), Service.NativeBinding(), &C, &Native.Binding,
                reinterpret_cast<uint64>(Wsi), Fd);
            if (Authorized == N::EStatus1::OK) Native.Flags |= NativeAuthorized1;
            else if (Authorized != N::EStatus1::PENDING && Authorized != N::EStatus1::BUSY) EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, TEXT("actual native ingress association refused"));
            break;
        }
    }
    if ((Native.Flags & NativePendingOpen1) && !bDestroyed) (void)CompleteNativeOpen(Service);
    if (bDestroyed && Wsi && Native.DestroyedInTurn && Native.DestroyedInTurn < Service.NativeBorrow()->Generation) {
        // This NEXT Between follows WHOLE C service return. Selected Step4/5
        // guarantees fd/poll/AH/cache/callback consumers retired BEFORE reset.
        const auto Physical = N::ObserveRetiredClientEpisode1(Service.NativeBorrow(), Service.NativeBinding(), &Native.Binding, reinterpret_cast<uint64>(Wsi));
        if (Physical != N::EStatus1::OK || N::NativeHalfReusable1(Service.NativeObject(), Service.NativeBinding(), &Native.Reservation) != N::EStatus1::OK) return;
        if (!(Native.Flags & NativeRetiredBinding1)) {
            const auto Retired = N::RetireBound1(Service.NativeObject(), Service.NativeBinding(), &Native.Binding);
            if (Retired != N::EStatus1::OK) return;
            Native.Flags |= NativeRetiredBinding1;
        }
        if (State == EConnState::Connecting && (Native.Flags & NativeEpisodeFailed1)) {
            N::FReservation1 Next {}; Next.Size = sizeof(Next); Next.Version = 1;
            const auto Reused = N::ReserveNext1(Service.NativeObject(), Service.NativeBinding(), &Native.Reservation, &Next);
            if (Reused == N::EStatus1::PENDING || Reused == N::EStatus1::BUSY) return;
            if (Reused == N::EStatus1::OK) {
                Native.Reservation = Next; Native.Binding = {}; Native.Stage = 2;
                Native.Flags &= ~(NativeAuthorized1 | NativePendingOpen1 | NativeOptionAttempted1 | NativeEpisodeFailed1 | NativeRetiredBinding1);
                Native.DestroyedInTurn = 0; Native.OptionResult = 0; Native.OptionError = 0;
                Wsi = nullptr; bDestroyed = false; bLeftPollSet = false; bKillIssued = false; AppendCalls = 0;
                PendingDetail.Reset(); return;
            }
            const FConnectionErrorEnd Described = DescribeConnectionError(PendingDetail, FPlatformTime::Seconds() - StartedAt);
            EndPreOpen(Described.Failure, Described.By, Described.HttpStatus, Described.Detail);
        }
        RetireNativeInterest(Service);
        if (!NativeInterestUncertain()) { Wsi = nullptr; Native.Stage = 9; }
    }
}
bool FConn::CompleteNativeOpen(FService& Service)
{
    using namespace VaRestLwsConnPrivate;
    namespace N = NumericConnectorC1;
    if (!(Native.Flags & NativePendingOpen1) || State != EConnState::Connecting || Service.IsShuttingDown()
        || bDestroyed || !Wsi || Owner != &Service || FConn::Find(Service, Id) != this || !Service.NativeBorrow()) return false;
    if (N::ValidateCurrentWsi1(Service.NativeBorrow(), Service.NativeBinding(), &Native.Binding, reinterpret_cast<uint64>(Wsi)) != N::EStatus1::OK) return false;
    const int32 Fd = lws_get_socket_fd(Wsi); // AFTER actual current membership.
    if (!(Native.Flags & NativeOptionAttempted1)) {
        if (!Request.AutoReplies.IsEmpty()) {
            const auto Debit = N::DebitCurrentSocketControl1(Service.NativeBorrow(), Service.NativeBinding(), &Native.Binding, reinterpret_cast<uint64>(Wsi), Fd);
            if (Debit.Disposition == N::ResourceQuotaDeferred1) return false;
            if (Debit.Disposition != N::ResourceCompleted1) { Native.Flags |= NativeInterestRetained1; return false; }
            Native.Flags |= NativeOptionAttempted1;
            const int BufferBytes = 256 * 1024;
            Native.OptionResult = ::setsockopt(Fd, SOL_SOCKET, SO_SNDBUF, &BufferBytes, sizeof(BufferBytes));
            Native.OptionError = Native.OptionResult == 0 ? 0 : errno;
            if (Native.OptionResult != 0) {
                EndPreOpen(EVaRestSocketFailure::BadHandshake, EVaRestSocketCloseBy::Transport, 0, TEXT("the automatic-reply socket send-buffer bound could not be installed")); Kill(Service); return false;
            }
        } else Native.Flags |= NativeOptionAttempted1; // Source skip, not a fabricated syscall result.
    }
    // Successful option remains captured while actual Fresh clock quota may
    // defer. Never repeat it, renew the deadline or fabricate another callback.
    const auto Established = N::ObserveActualWebSocketEstablished1(Service.NativeBorrow(), Service.NativeBinding(), &Native.Binding, reinterpret_cast<uint64>(Wsi), Fd);
    if (Established == N::EStatus1::PENDING) return false;
    if (Established != N::EStatus1::OK) {
        EndPreOpen(EVaRestSocketFailure::ConnectTimeout, EVaRestSocketCloseBy::Timeout, 0, TEXT("the original native deadline ended before Open")); Kill(Service); return false;
    }
    Native.Flags &= ~NativePendingOpen1;
    State = EConnState::Open; NextPingAt = FPlatformTime::Seconds() + Request.PingInterval;
    FVaRestSocketEvent Opened; Opened.Type = EVaRestSocketEventType::Open; Opened.Protocol = SelectedProtocol;
    Opened.Seconds = FPlatformTime::Seconds() - StartedAt; Link->Push(MoveTemp(Opened));
    lws_rx_flow_control(Wsi, 1); // Public Open already precedes every newly admitted Message.
    RefreshReadFlow(Service, FPlatformTime::Seconds());
    if (Outbox.Num() || bCloseQueued) lws_callback_on_writable(Wsi);
    return true;
}
}
#endif

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
    case LWS_CALLBACK_CLIENT_RECEIVE:
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        if (Native.Stage && State != EConnState::Open) return -1;
#endif
		// in: a chunk of a data frame, len its size: every frame end and every rx_buffer_size (T8, libwebsockets.h:1345)
		return OnReceive(Service, InWsi, In, Len);
	case LWS_CALLBACK_CLIENT_RECEIVE_PONG:
		OnPong(In, Len); return 0;
	case LWS_CALLBACK_CLIENT_WRITEABLE:
		return OnWriteable(InWsi);
	case LWS_CALLBACK_WS_PEER_INITIATED_CLOSE:
		return OnPeerClose(In, Len);
	case LWS_CALLBACK_WSI_CREATE:
		OnWsiCreate();
		return 0;
	case LWS_CALLBACK_ADD_POLL_FD:
		bLeftPollSet = false;
		return 0;
	case LWS_CALLBACK_DEL_POLL_FD:
		bLeftPollSet = true;
		return 0;
	case LWS_CALLBACK_OPENSSL_PERFORM_SERVER_CERT_VERIFICATION:
		// user is the X509_STORE_CTX, len preverify_ok (libwebsockets.h:1015-1044); the service found this connection by
		// lws_wsi_user, as the engine does (LwsWebSocketsManager.cpp:447-456)
		return OnVerifyServerCertificate(Service, static_cast<X509_STORE_CTX*>(User), static_cast<int32>(Len));
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
	if (AppendCalls > 1)
	{
		// A second APPEND_HANDSHAKE_HEADER in one attempt: LWS followed a 3xx and builds the request to the new target
		// [hs.redirect_relative, hs.redirect_absolute, hs.redirect_target_visible]. Ruling Q2: nothing is appended -- no auth and
		// no header reaches another target -- and the attempt fails here, before the second request goes out; LWS then gives no
		// CONNECTION_ERROR [hs.redirect_veto], so OnEnded reports it (plan decision 10). OnWsiCreate set the same failure
		// already; set again here, so this guard stands on its own count and leaves bRedirected to OnWsiCreate
		SetRedirectFailure();
		return -1;
	}
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
	if (bRedirected)
	{
		// The response to a redirected request: a second guard behind the APPEND veto ("HS: Rejected by filter cb",
		// hs.redirect_veto), on OnWsiCreate's signal rather than the APPEND count (Task 10 review M-2)
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
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (Native.Stage) {
        Native.Flags |= VaRestLwsConnPrivate::NativePendingOpen1;
        lws_rx_flow_control(InWsi, 0);
        (void)CompleteNativeOpen(*Owner);
        return State == EConnState::Ended ? -1 : 0;
    }
#endif
    if (!Request.AutoReplies.IsEmpty())
    {
        const int BufferBytes = 256 * 1024;
		if (::setsockopt(lws_get_socket_fd(InWsi), SOL_SOCKET, SO_SNDBUF, reinterpret_cast<const char*>(&BufferBytes), sizeof(BufferBytes)) != 0)
		{ EndPreOpen(EVaRestSocketFailure::BadHandshake, EVaRestSocketCloseBy::Transport, 0, TEXT("the automatic-reply socket send-buffer bound could not be installed")); return -1; }
	}
	State = EConnState::Open; NextPingAt = FPlatformTime::Seconds() + Request.PingInterval;
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

void VaRest::WebSocket::Lws::FConn::OnWsiCreate()
{
	// Task 10 review M-2 [Probe.VaRest.Lws.Handshake.RedirectToClosedPort]: when LWS follows a 3xx, lws_client_reset closes
	// the socket and calls lws_client_connect_2, which makes a new one and raises WSI_CREATE before its connect() (the
	// probe's comment cites the archive). The attempt's own WSI_CREATE comes before its first APPEND_HANDSHAKE_HEADER, so
	// one after it is a redirect's, seen before the redirect's TCP connect: a refused connect or a TLS failure at the new
	// target ends as Redirect too, a stop row in spec 3.4, never ConnectFailed, which W2 would retry. Not before LWS
	// resolves the new host, which the new socket's address family comes from: a Location whose name does not resolve never
	// reaches here -- LWS frees the attempt with no callback, and ReapIfDropped reports it (Redirect.Unresolvable)
	if (State != EConnState::Ended && AppendCalls > 0)
	{
		bRedirected = true;
		SetRedirectFailure();
	}
}

void VaRest::WebSocket::Lws::FConn::SetRedirectFailure()
{
	PendingFailure = EVaRestSocketFailure::Redirect;
	PendingDetail = TEXT("the server answered the upgrade with a redirect, which VaRest does not follow (ruling Q2)");
}

void VaRest::WebSocket::Lws::FConn::OnConnectionError(const void* In, size_t Len)
{
	if (State == EConnState::Ended)
	{
		return;
	}
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (Native.Stage && Native.Reservation.Generation && State == EConnState::Connecting && PendingFailure == EVaRestSocketFailure::None) {
        Native.Flags |= VaRestLwsConnPrivate::NativeEpisodeFailed1;
        const FString Text = In && Len ? FString::ConstructFromPtrSize(static_cast<const UTF8CHAR*>(In), static_cast<int32>(Len)) : FString();
        PendingDetail = CleanConnectionErrorText(Text); // Owned bounded text; no callback arguments escape.
        return; // Real C1 candidate sequence/physical retirement decides retry.
    }
#endif
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
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (Native.Stage && State == EConnState::Connecting && (Native.Flags & VaRestLwsConnPrivate::NativeEpisodeFailed1)) return;
#endif
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
		// Our close (spec 2.5), classified by ClassifyOurCloseEnd, whose comment gives the rules and their bounds
		const double SinceCloseSent = FPlatformTime::Seconds() - CloseSentAt;
		const FOurCloseEnd Classified = ClassifyOurCloseEnd(bCloseSent, bPeerClosed, SinceCloseSent);
		const bool bPeerFirst = !bCloseSent && bPeerClosed;
		const int32 Code = bCloseSent ? CloseCode : (bPeerClosed ? Peer.Code : 1006);
		FString Reason = bCloseSent ? Utf8ToString(CloseReason) : (bPeerClosed ? Peer.Reason : FString());
		// Re-review N-8, Task 9 review M1: worded by what VaRest knows -- it saw no close from the server, and cannot tell
		// an end by LWS's timer from an answer just before it
		FString Detail = Classified.bAckTimedOut
			? FString::Printf(TEXT("the connection ended %.1f s after our close frame with no close seen from the server; libwebsockets' close-ack timer ends an unanswered close after 5 s"), SinceCloseSent)
			: bPeerFirst ? PeerCloseDetail(Peer) : FString();
		EndOpen(Code, MoveTemp(Reason), Classified.bClean, CloseBy, CloseFailure, MoveTemp(Detail));
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
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (Native.Stage) {
        // Physical WSI persists. Only the callback fact is recorded here; no
        // RetireBound/reuse/erasure until WHOLE service return +NEXT Between.
        Native.DestroyedInTurn = Service.NativeBorrow() ? Service.NativeBorrow()->Generation : 0;
        bDestroyed = true;
        return;
    }
#endif
    Wsi = nullptr;
    bDestroyed = true;
#if WITH_DEV_AUTOMATION_TESTS
	if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->bWsiDestroyed.store(true, std::memory_order_release);
#endif
}

bool VaRest::WebSocket::Lws::FConn::ReapIfDropped()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    if (Native.Stage) return false; // Prepared tracked WSI has a positive full-return retirement path.
#endif
	if (!bLeftPollSet || bDestroyed || !Wsi)
	{
		return false;
	}
	// Task 11 carry, measured by Redirect.Unresolvable (APPEND_HANDSHAKE_HEADER, DEL_POLL_FD, LWS's "lws_client_connect_2:
	// getaddrinfo failed -2" and "Redirect failed", then no callback at all). LWS 3.0 frees a client wsi without a callback
	// when a change of socket fails before the new one joins the poll set. Following a redirect, lws_client_reset takes the
	// socket out of the set, sets the state to 0x200 (client-handshake.c.o 0xc50, 0xc82) and calls lws_client_connect_2. A
	// getaddrinfo failure there (0x6b6), a socket() or socket-option failure (0x5ee, 0x8be) or a refused insert (0x8d6)
	// jumps to 0x6bd, which raises CONNECTION_ERROR only for a state without the 0x200 bit -- lws_client_reset's 0x200 and
	// connect_2's 0x201 (0x4f7) have it -- and frees a wsi outside the set with lws_free, no WSI_DESTROY (0x702-0x75b). The
	// parser, handed NULL, returns 1 without touching it (client.c.o 0xd84-0xdb9). connect_2's next-address loop takes the
	// wsi out of the set too (0x46e) but never reaches that path: it closes the socket (0x47d) and, the descriptor number
	// left in place (only 0xc78 and 0xe55 store -1), connect()s it again (0x3e7-0x40a) and fails by CONNECTION_ERROR and
	// lws_close_free_wsi (0x8e2, 0x8fc, 0x931). A close takes a wsi out of the set only on its way to WSI_DESTROY in the same call
	// (libwebsockets.c.o 0xd35, 0xf0e, then 0x1108-0x1115; the one early return, 0x10bb, is an event library's logical
	// close). So a wsi that left the set and was not destroyed, seen here outside its own callbacks, is freed memory, and a
	// later wsi may have its address (VaRest-81s): no LWS call names it again. After the first APPEND only a redirect changes
	// sockets, and a failed redirect is a stop row (spec 3.4), not a ConnectFailed for W2 to retry. The Detail names no host
	Wsi = nullptr;
	if (AppendCalls > 0 && PendingFailure == EVaRestSocketFailure::None)
	{
		SetRedirectFailure();
	}
	const FString Dropped = TEXT("libwebsockets dropped the attempt with no error string while it changed sockets (a new host that does not resolve, or a socket that could not be made)");
	if (PendingFailure != EVaRestSocketFailure::None)
	{
		EndPreOpen(PendingFailure, EVaRestSocketCloseBy::Transport, 0, PendingDetail + TEXT("; ") + Dropped);
	}
	else
	{
		EndPreOpen(EVaRestSocketFailure::ConnectFailed, EVaRestSocketCloseBy::Transport, 0, Dropped);
	}
	// OnDestroyed's bookkeeping, as no WSI_DESTROY will come; no kill landed, so the kill counters stay
	UncountTls();
	UncountClose();
	bDestroyed = true;
	return true;
}

void VaRest::WebSocket::Lws::FConn::Kill(FService& Service)
{
	if (!Wsi || bDestroyed || bKillIssued || ReapIfDropped())
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
	// First: everything below may touch the wsi. A test can leave the reap to Kill alone (bOnlyKillReapsForTest), so Kill's
	// own guard has a row of its own (Redirect.UnresolvableKillGuard)
    if (!Service.GetOptions().bOnlyKillReapsForTest
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        && !Native.Stage
#endif
        )
	{
		ReapIfDropped();
	}
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
    if (State == EConnState::Connecting && Now >= ConnectDeadline
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
        && !Native.Stage
#endif
        )
	{
		// Spec 2.2, D32: measured here, so a hitching game thread cannot delay it -- exact unless the service thread is itself
		// blocked in LWS's synchronous DNS (review conc I-4; W1b B9, VaRest-gi0); reported at the deadline and killed (plan
		// decision 3). LWS's own cuts (its 20 s connect stage, timeout_secs) also end in CONNECTION_ERROR "", so the detail
		// says which one fired (review conc M-1, M-2)
		const FString Passed = FString::Printf(TEXT("VaRest's ConnectTimeout (%g s) passed"), Request.ConnectTimeout);
		if (PendingFailure != EVaRestSocketFailure::None)
		{
			// Task 11 review I-1: the attempt's own failure explains the end better, as in OnConnectionError and OnEnded -- above
			// all a redirect LWS is following (OnWsiCreate) to a target that hangs: plan decision 10 makes it Redirect, a stop
			// row (spec 3.4), whichever timer cuts it. By Transport: before the open, By Timeout is ConnectTimeout's alone
			EndPreOpen(PendingFailure, EVaRestSocketCloseBy::Transport, 0, PendingDetail + TEXT("; ") + Passed);
		}
		else
		{
			EndPreOpen(EVaRestSocketFailure::ConnectTimeout, EVaRestSocketCloseBy::Timeout, 0, Passed);
		}
		Kill(Service);
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
		CheckOpenDeadlines(Service, Now);
	}
}

double VaRest::WebSocket::Lws::FConn::NextDeadline() const
{
	double Next = NextOpenDeadline();
	if (State == EConnState::Open && bCloseQueued)
	{
		Next = CloseDeadline;
	}
	if (State == EConnState::Connecting)
	{
		Next = FMath::Min(Next, ConnectDeadline);
	}
	return Next;
}

bool VaRest::WebSocket::Lws::FConn::WantsShortWait() const
{
	return (bKillIssued && !bDestroyed) || HeldBytes > 0 || bControlHeld;
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
	Event.End.bWhileReadPaused = bReadPaused;
	Event.End.By = By;
	Event.End.Failure = Failure;
	Event.End.Detail = MoveTemp(Detail);
	UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: closed %d by %s%s"), *GetLabel(), Code, LexToString(By), bClean ? TEXT(", clean") : TEXT(""));
	Link->Push(MoveTemp(Event));
}
