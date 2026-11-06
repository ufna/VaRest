// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsConn.h"
#include "HAL/PlatformTime.h"

void VaRest::WebSocket::Lws::FConn::RefreshReadFlow(FService& Service, double Now)
{
	if (ReapIfDropped() || bDestroyed || State != EConnState::Open || !Wsi) return;
	// A dequeue can beat publication of the paused flag. Re-read once immediately after pausing; consumer rechecks are
	// also unconditional and coalesced, with their bit cleared before reading these current counters.
	for (int32 Pass = 0; Pass != 2; ++Pass)
	{
		const int64 PendingBytes = Link->PendingInboundBytes.load(std::memory_order_acquire);
		const int32 Queued = Link->GetQueuedEventCount();
		const auto& Watermarks = Service.GetOptions().QueuedEvents;
		const bool bHigh = PendingBytes > Request.MaxPendingInboundBytes || Watermarks.IsHigh(Queued) || AutoReplyDebt.Num() >= MaxAutoReplyDebt - 1;
		const bool bLow = PendingBytes <= Request.MaxPendingInboundBytes / 2 && Watermarks.IsLow(Queued) && AutoReplyDebt.Num() < MaxAutoReplyDebt / 2;
		const bool bPause = !bCloseQueued && (bReadPaused ? !bLow : bHigh);
		if (bPause == bReadPaused) return;
		if (bPause) ReadPausedAt = Now;
		else
		{
			const double PausedFor = FMath::Max(0.0, Now - ReadPausedAt);
			NextPingAt += PausedFor; if (bAwaitingPong) PingCheckAt += PausedFor; if (IdleSeconds > 0.0) IdleAt += PausedFor;
			// PingSentAt stays physical: reported RTT includes time before the native consumer observed the pong.
		}
		bReadPaused = bPause; Link->bReadPaused.store(bPause, std::memory_order_release);
		lws_rx_flow_control(Wsi, bPause ? 0 : 1);
#if WITH_DEV_AUTOMATION_TESTS
		if (const auto& Probe = Link->GetPressureProbeForTests())
		{
			(bPause ? Probe->RxDisables : Probe->RxEnables).fetch_add(1, std::memory_order_relaxed);
			Probe->NoteTransition({Queued, AutoReplyDebt.Num(), PendingBytes, !bPause, bPause});
		}
#endif
		FVaRestSocketEvent Event; Event.Type = EVaRestSocketEventType::Flow; Event.bReadPaused = bPause; Event.PendingBytes = PendingBytes; Link->Push(MoveTemp(Event));
		if (!bPause) return;
	}
}
void VaRest::WebSocket::Lws::FConn::SetIdleDeadline(FService& Service, double Seconds)
{
	if (!FMath::IsFinite(Seconds) || Seconds < 0.0 || State != EConnState::Open) return;
	IdleSeconds = Seconds; IdleAt = FPlatformTime::Seconds() + Seconds;
	RefreshReadFlow(Service, FPlatformTime::Seconds());
}
void VaRest::WebSocket::Lws::FConn::Abort(FService& Service, FString Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure)
{
	if (State == EConnState::Ended) return;
	if (State == EConnState::Open) EndOpen(1006, MoveTemp(Reason), false, By, Failure);
	else EndPreOpen(Failure == EVaRestSocketFailure::None ? EVaRestSocketFailure::Canceled : Failure, By, 0, MoveTemp(Reason));
	Kill(Service);
}
void VaRest::WebSocket::Lws::FConn::NoteInboundData(double Now)
{
	if (bAwaitingPong) PingCheckAt = Now + Request.PingTimeout;
	if (IdleSeconds > 0.0) IdleAt = Now + IdleSeconds;
}
void VaRest::WebSocket::Lws::FConn::OnPong(const void* In, size_t Len)
{
	if (State != EConnState::Open || bCloseQueued) return;
	const double Now = FPlatformTime::Seconds();
	if (bAwaitingPong) PingCheckAt = Now + Request.PingTimeout;
	if (bAwaitingPong && In && Len == sizeof(PingPayload) && FMemory::Memcmp(In, PingPayload, sizeof(PingPayload)) == 0)
	{
		bAwaitingPong = false;
		FVaRestSocketEvent Pong; Pong.Type = EVaRestSocketEventType::Pong; Pong.Seconds = FMath::Max(0.0, Now - PingSentAt); Link->Push(MoveTemp(Pong));
	}
}
void VaRest::WebSocket::Lws::FConn::CheckOpenDeadlines(FService& Service, double Now)
{
	RefreshReadFlow(Service, Now);
	if (bDestroyed || State != EConnState::Open || bCloseQueued || bReadPaused) return;
	if (IdleSeconds > 0.0 && Now >= IdleAt) { Abort(Service, TEXT("idle timeout"), EVaRestSocketCloseBy::Heartbeat, EVaRestSocketFailure::IdleTimeout); return; }
	if (bAwaitingPong && Now >= PingCheckAt) { Abort(Service, TEXT("ping timeout"), EVaRestSocketCloseBy::Heartbeat, EVaRestSocketFailure::HeartbeatTimeout); return; }
	if (Request.PingInterval > 0.0 && Now >= NextPingAt && !bPingQueued)
	{
		bPingQueued = true; lws_callback_on_writable(Wsi);
	}
}
double VaRest::WebSocket::Lws::FConn::NextOpenDeadline() const
{
	if (State != EConnState::Open || bCloseQueued || bReadPaused) return TNumericLimits<double>::Max();
	double Next = TNumericLimits<double>::Max();
	if (IdleSeconds > 0.0) Next = IdleAt;
	if (bAwaitingPong) Next = FMath::Min(Next, PingCheckAt);
	if (Request.PingInterval > 0.0 && !bPingQueued) Next = FMath::Min(Next, NextPingAt);
	return Next;
}
TOptional<int32> VaRest::WebSocket::Lws::FConn::WriteServiceControl(lws* InWsi)
{
	if (bControlHeld && lws_partial_buffered(InWsi)) { lws_callback_on_writable(InWsi); return 0; }
	bControlHeld = false;
	if (bPingQueued && !bCloseQueued && !bReadPaused)
	{
		bPingQueued = false;
		const uint32 Sequence = ++PingSequence;
		const double Now = FPlatformTime::Seconds(); const uint32 Milliseconds = static_cast<uint32>(static_cast<uint64>(Now * 1000.0));
		FMemory::Memcpy(PingPayload, &Sequence, sizeof(Sequence)); FMemory::Memcpy(PingPayload + sizeof(Sequence), &Milliseconds, sizeof(Milliseconds));
		uint8 Wire[VaRest::WebSocket::FrameHeadroom + sizeof(PingPayload)] = {};
		FMemory::Memcpy(Wire + VaRest::WebSocket::FrameHeadroom, PingPayload, sizeof(PingPayload));
		const int Wrote = lws_write(InWsi, Wire + VaRest::WebSocket::FrameHeadroom, sizeof(PingPayload), LWS_WRITE_PING);
		if (Wrote < 0) { EndOpen(1006, {}, false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None, TEXT("protocol ping write failed")); return -1; }
		PingSentAt = Now; PingCheckAt = Now + Request.PingTimeout; NextPingAt = Now + Request.PingInterval; bAwaitingPong = true;
		bControlHeld = lws_partial_buffered(InWsi) != 0;
		if (Outbox.Num() || AutoReplyDebt.Num() || bCloseQueued || bControlHeld) lws_callback_on_writable(InWsi);
		return 0;
	}
	// Exact text replies are data frames. A protocol PING can interrupt a fragmented message; these cannot.
	if (NextFragment == 0 && !AutoReplyDebt.IsEmpty())
	{
		const auto Debt = AutoReplyDebt[0]; AutoReplyDebt.RemoveAt(0, 1, EAllowShrinking::No);
		auto Frame = FVaRestSocketFrame::Binary(Request.AutoReplies[Debt.Rule].Reply);
		const int Wrote = lws_write(InWsi, Frame.Storage.GetData() + Frame.Offset, Frame.Length, LWS_WRITE_TEXT);
		if (Wrote < 0) { EndOpen(1006, {}, false, EVaRestSocketCloseBy::Transport, EVaRestSocketFailure::None, TEXT("automatic text reply write failed")); return -1; }
		if (Request.ServiceStats) Request.ServiceStats->RecordAutoReply(FMath::Max(0.0, FPlatformTime::Seconds() - Debt.ReceivedAt));
		bControlHeld = lws_partial_buffered(InWsi) != 0;
		if (Owner) RefreshReadFlow(*Owner, FPlatformTime::Seconds());
		if (Outbox.Num() || AutoReplyDebt.Num() || bCloseQueued || bControlHeld || bPingQueued) lws_callback_on_writable(InWsi);
		return 0;
	}
	return {};
}
