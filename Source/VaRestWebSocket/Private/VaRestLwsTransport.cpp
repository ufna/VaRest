// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestLwsTransport.h"
#include <stdint.h>

#include "Async/Async.h"
#include "HAL/PlatformTime.h"
#include "Misc/ScopeLock.h"
#include "Modules/ModuleManager.h"
#include "VaRestLwsConn.h"
#include "VaRestSocketLink.h"
#include "VaRestWebSocketLog.h"
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
#include "NativeOwner/VaRestNativeOwnerServiceAdapter.h"
#endif

namespace VaRest::WebSocket::Lws::VaRestLwsTransportPrivate
{
#if WITH_DEV_AUTOMATION_TESTS
	/** GT scoped admission only; no pointer to the fixture survives Connect. */
	struct FPressureAdmission
	{
		FTransport* Transport;
		FString Url;
		TSharedRef<PressureTesting::FProbe, ESPMode::ThreadSafe> Probe;
	};
	FPressureAdmission* PressureAdmission = nullptr;
	TWeakPtr<PressureTesting::FProbe, ESPMode::ThreadSafe> LastPressureProbe;
#endif
	/**
	 * The process's slot, leaked on purpose as VaRest::Http::GetDefaultTransport: nothing of it runs at static destruction;
	 * the engine-exit hook shuts its transport down. A function-local static: its first use from any thread makes it once
	 * (C++11 [stmt.dcl]/4)
	 */
	FDefaultTransportSlot& DefaultSlot()
	{
		static FDefaultTransportSlot* const Slot = new FDefaultTransportSlot(FServiceOptions());
		return *Slot;
	}

	/**
	 * The consumer's handle on one FConn (WebSocket spec 1.3): commands go to the service thread, and the link is shared with
	 * it. Send and Close are game thread; PopEvents and Consumed the consumer's thread; the getters and the destructor any thread
	 */
	class FHandle final : public IVaRestSocketConnection
	{
	public:
		FHandle(TWeakPtr<FService, ESPMode::ThreadSafe> InService, uint64 InId, TSharedRef<FSocketLink, ESPMode::ThreadSafe> InLink)
			: Service(MoveTemp(InService))
			, Id(InId)
			, Link(MoveTemp(InLink))
		{
		}

		virtual ~FHandle() override
		{
#if WITH_DEV_AUTOMATION_TESTS
			if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->HandleIdentity.store(nullptr, std::memory_order_release);
#endif
			// Any thread (spec 1.4): the events stop at once; the service thread closes or cancels the connection
			Link->bReleased = true;
			Post([ConnId = Id](FService& Self)
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Release(Self);
				}
			});
		}

		virtual int32 PopEvents(TArray<FVaRestSocketEvent>& Out) override
		{
			const int32 Popped = Link->PopAll(Out);
			// Empty messages retire queue slots here too. Payload acknowledgements, including Consumed(0), are independent.
			if (Popped) WakeReadFlow();
			return Popped;
		}

		virtual bool Send(FVaRestSocketFrame&& Frame) override
		{
			check(IsInGameThread());
			// Review conc M-6: after Close the service thread would drop it (FConn::Send, bCloseQueued); say so here instead
			if (!Frame.IsValid() || Link->bEnded.load() || Link->bCloseRequested.load())
			{
				return false;
			}
			// Counted on the caller's side, so the buffered amount includes what the service thread has not seen yet (spec 2.4).
			// What this add produced goes with the frame: the high-water mark is decided on it (Task 3 review I-1)
			const int64 Length = Frame.Length;
			const int64 After = Link->BufferedBytes.fetch_add(Length) + Length;
			const bool bQueued = Post([ConnId = Id, Moved = MoveTemp(Frame), Shared = Link, After](FService& Self) mutable
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Send(Self, MoveTemp(Moved), After);
				}
				else
				{
					Shared->BufferedBytes.fetch_sub(Moved.Length);
				}
			});
			if (!bQueued)
			{
				// No service took it (Task 6: use Enqueue's result): the bytes were never the socket's
				Link->BufferedBytes.fetch_sub(Length);
			}
			return bQueued;
		}

		virtual void Close(int32 Code, FStringView Reason) override
		{
			check(IsInGameThread());
			// Spec 3.5 through the helper the fake shares, Warnings and all, at every call (Task 3 review I-2)
			FOutboundClose Outbound = MakeOutboundClose(Link->GetLabel(), Code, Reason);
			Link->bCloseRequested = true;
			Post([ConnId = Id, Sent = Outbound.Code, Moved = MoveTemp(Outbound.Reason)](FService& Self) mutable
			{
				if (FConn* Conn = FConn::Find(Self, ConnId))
				{
					Conn->Close(Self, Sent, MoveTemp(Moved), EVaRestSocketCloseBy::Client, EVaRestSocketFailure::None);
				}
			});
		}

		virtual void Consumed(int64 Bytes) override
		{
			Link->PendingInboundBytes.fetch_sub(Bytes);
#if WITH_DEV_AUTOMATION_TESTS
			if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->AckBytes.fetch_add(Bytes, std::memory_order_relaxed);
#endif
			WakeReadFlow();
		}
		virtual void ConsumedMessages(int64 Bytes, int32 Messages) override
		{
			Link->PendingInboundBytes.fetch_sub(Bytes); Link->PendingInboundMessages.fetch_sub(Messages);
#if WITH_DEV_AUTOMATION_TESTS
			if (const auto& Probe = Link->GetPressureProbeForTests())
			{
				Probe->AckBytes.fetch_add(Bytes, std::memory_order_relaxed);
				Probe->AckMessages.fetch_add(Messages, std::memory_order_relaxed);
			}
#endif
			WakeReadFlow();
		}
		virtual bool SetIdleDeadline(double Seconds) override
		{
			check(IsInGameThread());
			if (!FMath::IsFinite(Seconds) || Seconds < 0.0 || Link->bEnded.load() || Link->bCloseRequested.load()) return false;
			return Post([ConnId = Id, Seconds](FService& Self) { if (auto* Conn = FConn::Find(Self, ConnId)) Conn->SetIdleDeadline(Self, Seconds); });
		}
		virtual bool Abort(FStringView Reason, EVaRestSocketCloseBy By, EVaRestSocketFailure Failure) override
		{
			check(IsInGameThread()); if (Link->bEnded.load()) return false;
			Link->bCloseRequested.store(true, std::memory_order_release);
			return Post([ConnId = Id, Detail = FString(Reason), By, Failure](FService& Self) mutable { if (auto* Conn = FConn::Find(Self, ConnId)) Conn->Abort(Self, MoveTemp(Detail), By, Failure); });
		}
		virtual bool IsReadPaused() const override { return Link->bReadPaused.load(std::memory_order_acquire); }

		virtual int64 GetBufferedAmount() const override
		{
			return FMath::Max<int64>(Link->BufferedBytes.load(), 0);
		}

		virtual int64 GetPendingInboundBytes() const override
		{
			return Link->PendingInboundBytes.load();
		}

		virtual bool HasEnded() const override
		{
			return Link->bEnded.load();
		}

#if WITH_DEV_AUTOMATION_TESTS
		uint64 RequestPressureFence(const TSharedRef<PressureTesting::FProbe, ESPMode::ThreadSafe>& Probe)
		{
			check(IsInGameThread());
			if (Link->GetPressureProbeForTests().Get() != &Probe.Get() || Probe->bCanceled.load(std::memory_order_acquire)
				|| Probe->Connection.load(std::memory_order_acquire) != Id || Probe->ActiveRequest.load(std::memory_order_acquire)) return 0;
			const uint64 Request = Probe->NextRequest.fetch_add(1, std::memory_order_relaxed) + 1;
			check(Request != 0);
			Probe->ActiveRequest.store(Request, std::memory_order_release);
			Probe->InFlightRequests.fetch_add(1, std::memory_order_acq_rel);
			if (!Post([Probe, ConnId = Id, Request](FService& Self)
			{
				Self.QueuePressureFenceForTests({Probe, Probe->Owner, ConnId, Request});
			})) Probe->Acknowledge(Request, PressureTesting::EFenceStatus::Refused);
			return Request;
		}
#endif

	private:
		void WakeReadFlow() const
		{
			if (Link->bEnded.load(std::memory_order_acquire)) return;
			if (Link->bFlowWakeQueued.exchange(true, std::memory_order_acq_rel))
			{
#if WITH_DEV_AUTOMATION_TESTS
				if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->WakeCoalesced.fetch_add(1, std::memory_order_relaxed);
#endif
				return;
			}
#if WITH_DEV_AUTOMATION_TESTS
			if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->WakePosts.fetch_add(1, std::memory_order_relaxed);
#endif
			if (!Post([ConnId = Id, Shared = Link](FService& Self) {
				Shared->bFlowWakeQueued.store(false, std::memory_order_release);
#if WITH_DEV_AUTOMATION_TESTS
				if (const auto& Probe = Shared->GetPressureProbeForTests()) Probe->WakeRuns.fetch_add(1, std::memory_order_relaxed);
#endif
				if (auto* Conn = FConn::Find(Self, ConnId)) Conn->RefreshReadFlow(Self, FPlatformTime::Seconds());
			}))
			{
				Link->bFlowWakeQueued.store(false, std::memory_order_release);
#if WITH_DEV_AUTOMATION_TESTS
				if (const auto& Probe = Link->GetPressureProbeForTests()) Probe->WakeRefused.fetch_add(1, std::memory_order_relaxed);
#endif
			}
		}
		/** Any thread: true when the service queued it; false when there is none or it no longer accepts (the command is released) */
		bool Post(FCommand&& Command) const
		{
			if (const TSharedPtr<FService, ESPMode::ThreadSafe> Pinned = Service.Pin())
			{
				return Pinned->Enqueue(MoveTemp(Command));
			}
			return false;
		}

		const TWeakPtr<FService, ESPMode::ThreadSafe> Service;
		const uint64 Id;
		const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link;
	};

	/**
	 * Game thread, inside Connect: no service to report from, so the Error is queued here and its notification posted to a
	 * later game-thread task -- never a call back inside Connect (E7)
	 */
	TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> RefuseConnect(const TSharedRef<FSocketLink, ESPMode::ThreadSafe>& Refused, uint64 ConnId)
	{
		FVaRestSocketEvent Event;
		Event.Type = EVaRestSocketEventType::Error;
		Event.End.Failure = EVaRestSocketFailure::Shutdown;
		Event.End.Detail = TEXT("the WebSocket transport is shut down or could not start");
		UE_LOG(LogVaRestSocket, Verbose, TEXT("%s: never opened: %s, %s"), *Refused->GetLabel(), LexToString(Event.End.Failure), *Event.End.Detail);
		Refused->Push(MoveTemp(Event), false);
		AsyncTask(ENamedThreads::GameThread, [Refused] { Refused->Notify(); });
		return MakeShared<FHandle, ESPMode::ThreadSafe>(TWeakPtr<FService, ESPMode::ThreadSafe>(), ConnId, Refused);
	}
}

VaRest::WebSocket::Lws::FTransport::FTransport(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

VaRest::WebSocket::Lws::FTransport::~FTransport()
{
	if (Service.IsValid())
	{
		check(IsInGameThread());
		Service->Shutdown();
	}
}

bool VaRest::WebSocket::Lws::FTransport::EnsureStarted()
{
	check(IsInGameThread());
	if (bShutDown)
	{
		return false;
	}
	if (!Service.IsValid())
	{
		Service = MakeShared<FService, ESPMode::ThreadSafe>(Options);
	}
	// FService::Start makes no second context after a failure (review conc I-2) and none while a wss attempt is in flight (I-3)
	const TSharedRef<FService, ESPMode::ThreadSafe> KeepAlive = Service.ToSharedRef();
    return KeepAlive->IsRunning() || KeepAlive->Start();
}

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
[[gnu::used]] VrStatus VaRest::WebSocket::Lws::FTransport::EnsureNativeStartedOnCurrentControl()
{
#if !(defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3)
    return VR_STATUS_DISCOVERY_UNESTABLISHED;
#else
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("native Host startup before Service allocation"));
    if (Role != VR_STATUS_OK) return Role;
    if (!NativeOwnerRealThreadModeAllowed()) return VR_STATUS_FORK_UNESTABLISHED;
    if (bShutDown) return VR_STATUS_STALE;
    if (!Service.IsValid()) Service = MakeShared<FService, ESPMode::ThreadSafe>(Options);
    const TSharedRef<FService, ESPMode::ThreadSafe> KeepAlive = Service.ToSharedRef();
    if (bShutDown) return NativeHostStatus1(*KeepAlive, VR_STATUS_STALE); // Reentrant allocation cutoff.
    const VrStatus Raw = KeepAlive->StartNativeOnCurrentControl();
    // Local shared ownership spans actual entry/manager/constructor/factory
    // reentrancy. Continuous producer/WS code O and pending owner remain OPEN.
    // A reentrant actual Finish/Shutdown already latched Service's cutoff.
    // L4 consumes that cutoff throughout construction/factory/unwind. Never
    // add a void cleanup here that could discard a later actual19 status.
    return Raw;
#endif
}

#endif

namespace VaRest::WebSocket::Lws
{
namespace
{
    static FVaRestNativeOwnerHostStep1 HostStep1(EVaRestNativeOwnerHostOutcome1 Outcome, int32 Status)
    { return {16,1,static_cast<uint32>(Outcome),Status}; }
}
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
VrStatus FTransport::NativeHostStatus1(const FService& S, VrStatus Raw) const
{
    // GT-owned historical ACTUAL observations stay intact. This is not a fresh
    // native query/status or an inference of native19 from local retention.
    return Raw == VR_STATUS_RELEASE_UNCERTAIN || S.NativeCreating.AdmissionStatus == VR_STATUS_RELEASE_UNCERTAIN
        || S.NativeCreating.AttachStatus == VR_STATUS_RELEASE_UNCERTAIN || S.NativeCreating.CallerDispositionStatus == VR_STATUS_RELEASE_UNCERTAIN
        ? VR_STATUS_RELEASE_UNCERTAIN : Raw;
}
bool FTransport::NativeHostFinished1(const FService& S) const
{
    // Finished is written only after actual no-create/full resource+value
    // closure. Test that causal boundary FIRST, before joined-only fields.
    if (S.NativeCreating.Phase != FService::ENativeEntryPhase1::Finished) return false;
    return !S.NativeCreating.Adapter.IsValid() && !S.NativeCreating.Resources && !S.NativeChildSnapshot
        && !S.Thread && !S.Context && !S.NativeCreating.InEntry && !S.NativeCreating.InCreate && !S.NativeCreating.InCleanup
        && !S.NativeRunning.InC && !S.NativeRunning.Borrow && !S.NativeRunning.Vhosts[0] && !S.NativeRunning.Vhosts[1]
        && !S.bAccepting.load();
}
bool FTransport::NativeHostStartCommitted1(const FService& S) const
{
    // Only creator-owned/stable or atomic fields are read while the child may
    // execute. NEVER read its mutable Borrow/InC/journal/phase/cursors here.
    return S.NativeCreating.Phase == FService::ENativeEntryPhase1::ChildCreated
        && S.NativeCreating.Adapter.IsValid() && S.NativeCreating.Resources && S.Thread && S.Context && S.NativeChildSnapshot
        && S.NativeChildSnapshot->GetRegistrationDiagnostic() == VR_STATUS_OK
        && !S.NativeCreating.InEntry && !S.NativeCreating.InCreate && !S.NativeCreating.InCleanup
        && S.bAccepting.load() && !S.bStopping.load() && !S.bShuttingDown.load() && !bShutDown;
}
bool FTransport::NativeHostAdvanceAllowed1(const FService& S) const
{
    using P = FService::ENativeEntryPhase1;
    if (S.NativeCreating.InEntry || S.NativeCreating.InCreate || S.NativeCreating.InCleanup) return false;
    // Explicitly exclude the normal first-join source state. Pointer absence
    // never supplies NoChild; these are the actual accepted L4 phase writers.
    if (S.NativeCreating.Phase == P::ChildCreated) return false;
    if (S.NativeCreating.Phase == P::JoinedPending || S.NativeCreating.Phase == P::NoCreatePending
        || S.NativeCreating.Phase == P::Finished) return true;
    // Retained writers: entry/attach/no-create have no created join; rejected
    // ObserveRealThreadCreated never commits CreatedJoinUnobserved (type failure
    // sets Uncertain); PrepareFirstJoin sets JoinPrepared BEFORE any returned
    // pointer; false Kill sets Uncertain; postjoin diagnostic failures already
    // own the genuine join. Thus the exact L4 callee cannot perform a first
    // Kill from this reachable phase. No generic caller flag grants that fact.
    if (S.NativeCreating.Phase == P::Retained) return true;
    return S.NativeRunning.FinishStep == 3 || S.NativeRunning.FinishStep == 4 || S.NativeRunning.FinishStep == 5;
}
FVaRestNativeOwnerHostStep1 FTransport::NativeHostFinishResult1(const FService& S, VrStatus Raw) const
{
    const VrStatus Status = NativeHostStatus1(S, Raw);
    if (Status == VR_STATUS_RELEASE_UNCERTAIN) return HostStep1(EVaRestNativeOwnerHostOutcome1::Retained, Status);
    const bool Finished = NativeHostFinished1(S);
    if (Status == VR_STATUS_OK && Finished && bShutDown && S.bShuttingDown.load())
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Complete, Status);
    if (S.NativeRunning.FinishStep == 5 || S.NativeCreating.Phase == FService::ENativeEntryPhase1::Retained)
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Retained, Status);
    if (Status == VR_STATUS_PENDING || Status == VR_STATUS_BUSY)
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Pending, Status);
    if (Status == VR_STATUS_OLD_OWNER_BUSY && !Finished && NativeHostAdvanceAllowed1(S))
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Pending, Status);
    return HostStep1(Finished ? EVaRestNativeOwnerHostOutcome1::Refused : EVaRestNativeOwnerHostOutcome1::Retained, Status);
}
#endif

FVaRestNativeOwnerHostStep1 FTransport::StartNativeOwnerSession1()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("Host start before lazy Service"));
    if (Role != VR_STATUS_OK) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Role);
    if (!NativeOwnerRealThreadModeAllowed()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_FORK_UNESTABLISHED);
    if (bShutDown) {
        const VrStatus Status = Service.IsValid() ? NativeHostStatus1(*Service, VR_STATUS_STALE) : VR_STATUS_STALE;
        return HostStep1(Status == VR_STATUS_RELEASE_UNCERTAIN ? EVaRestNativeOwnerHostOutcome1::Retained : EVaRestNativeOwnerHostOutcome1::Refused, Status);
    }
    if (!Service.IsValid()) Service = MakeShared<FService, ESPMode::ThreadSafe>(Options);
    const TSharedRef<FService, ESPMode::ThreadSafe> Held = Service.ToSharedRef();
    if (bShutDown) {
        const VrStatus Status = NativeHostStatus1(*Held, VR_STATUS_STALE);
        return HostStep1(Status == VR_STATUS_RELEASE_UNCERTAIN ? EVaRestNativeOwnerHostOutcome1::Retained : EVaRestNativeOwnerHostOutcome1::Refused, Status);
    }
    const bool Repeated = Held->NativeCreating.Phase != FService::ENativeEntryPhase1::Fresh;
    const VrStatus Raw = EnsureNativeStartedOnCurrentControl(); // Actual L4, same Service, no ordinary fallback.
    const VrStatus Status = NativeHostStatus1(*Held, Raw);
    if (Status == VR_STATUS_RELEASE_UNCERTAIN) return HostStep1(EVaRestNativeOwnerHostOutcome1::Retained, Status);
    if (Status == VR_STATUS_OK && NativeHostStartCommitted1(*Held)) return HostStep1(EVaRestNativeOwnerHostOutcome1::Complete, Status);
    if (Repeated && Status == VR_STATUS_STALE) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Status);
    if (Held->NativeCreating.Phase == FService::ENativeEntryPhase1::Fresh || NativeHostFinished1(*Held))
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Status);
    if (Held->NativeCreating.Phase != FService::ENativeEntryPhase1::Retained && Held->NativeRunning.FinishStep != 5
        && (Status == VR_STATUS_PENDING || Status == VR_STATUS_BUSY
            || (Status == VR_STATUS_OLD_OWNER_BUSY && NativeHostAdvanceAllowed1(*Held))))
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Pending, Status);
    return HostStep1(EVaRestNativeOwnerHostOutcome1::Retained, Status); // No fabricated native19.
#else
    return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, 7);
#endif
}
FVaRestNativeOwnerHostStep1 FTransport::FinishNativeOwnerSession1()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("Host first finish"));
    if (Role != VR_STATUS_OK) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Role);
    if (!NativeOwnerRealThreadModeAllowed()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_FORK_UNESTABLISHED);
    if (!Service.IsValid()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_NO_RESULT);
    const TSharedRef<FService, ESPMode::ThreadSafe> Held = Service.ToSharedRef();
    if (Held->NativeCreating.Phase == FService::ENativeEntryPhase1::Fresh)
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Held->Thread || Held->Context || Held->IsRunning() ? VR_STATUS_OLD_OWNER_BUSY : VR_STATUS_NO_RESULT);
    bShutDown = true; // Actual finish request, retained across every pending return.
    if (Held->NativeCreating.InEntry || Held->NativeCreating.InCreate || Held->NativeCreating.InCleanup) {
        Held->Shutdown(); // Existing actual cutoff/stop/permit branch, not a completion result.
        const VrStatus Status = NativeHostStatus1(*Held, VR_STATUS_BUSY);
        return HostStep1(Status == VR_STATUS_RELEASE_UNCERTAIN ? EVaRestNativeOwnerHostOutcome1::Retained : EVaRestNativeOwnerHostOutcome1::Pending, Status);
    }
    const VrStatus Raw = Held->FinishNativeCleanupChildOnCurrentControl(); // ONE status-bearing first-finish step.
    return NativeHostFinishResult1(*Held, Raw);
#else
    return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, 7);
#endif
}
FVaRestNativeOwnerHostStep1 FTransport::AdvanceNativeOwnerFinish1()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("Host bounded finish advance"));
    if (Role != VR_STATUS_OK) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Role);
    if (!NativeOwnerRealThreadModeAllowed()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_FORK_UNESTABLISHED);
    if (!Service.IsValid() || !bShutDown) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_NO_RESULT);
    const TSharedRef<FService, ESPMode::ThreadSafe> Held = Service.ToSharedRef();
    if (!Held->IsShuttingDown()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_NO_RESULT);
    if (Held->NativeCreating.InEntry || Held->NativeCreating.InCreate || Held->NativeCreating.InCleanup) {
        Held->Shutdown(); // Already requested; actual reentrant cutoff only, no second status/join step.
        const VrStatus Status = NativeHostStatus1(*Held, VR_STATUS_BUSY);
        return HostStep1(Status == VR_STATUS_RELEASE_UNCERTAIN ? EVaRestNativeOwnerHostOutcome1::Retained : EVaRestNativeOwnerHostOutcome1::Pending, Status);
    }
    if (!NativeHostAdvanceAllowed1(*Held)) {
        const VrStatus Status = NativeHostStatus1(*Held, VR_STATUS_OLD_OWNER_BUSY);
        return HostStep1(EVaRestNativeOwnerHostOutcome1::Retained, Status); // No inferred NoChild/first join.
    }
    const VrStatus Raw = Held->FinishNativeCleanupChildOnCurrentControl(); // ONE pending step; no Start/first/repeated Kill.
    return NativeHostFinishResult1(*Held, Raw);
#else
    return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, 7);
#endif
}
FVaRestNativeOwnerHostStep1 FTransport::PumpNativeOwnerSession1()
{
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__) && defined(VAREST_USE_PRIVATE_LWS3) && VAREST_USE_PRIVATE_LWS3
    const VrStatus Role = RequireNativeOwnerControlThread(TEXT("Host direct native frame"));
    if (Role != VR_STATUS_OK) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, Role);
    if (!NativeOwnerRealThreadModeAllowed()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_FORK_UNESTABLISHED);
    if (!Service.IsValid()) return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_NO_RESULT);
    const TSharedRef<FService, ESPMode::ThreadSafe> Held = Service.ToSharedRef();
    // This whole-return pin protects the existing object, never producer O/code H.
    // The no-call path still preserves real prior19 in the actual causal journals.
    if (bShutDown)
        return Held->NativeFrameDisposition1(HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, VR_STATUS_STALE));
    return Held->PumpNativeOnCurrentControl();
#else
    return HostStep1(EVaRestNativeOwnerHostOutcome1::Refused, 7);
#endif
}
}

void VaRest::WebSocket::Lws::FTransport::Shutdown()
{
	check(IsInGameThread());
	bShutDown = true;
	if (Service.IsValid())
	{
		Service->Shutdown();
	}
}

void VaRest::WebSocket::Lws::FTransport::ResetAfterFork()
{
	check(IsInGameThread());
	if (Service.IsValid())
	{
        const TSharedRef<FService, ESPMode::ThreadSafe> KeepAlive = Service.ToSharedRef();
        KeepAlive->Shutdown();
#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
        if (KeepAlive->NativeCreating.Phase != FService::ENativeEntryPhase1::Fresh) {
            bShutDown = true;
            return; // No foreign-fork reset/forget of a retained native graph.
        }
#endif
        Service.Reset();
	}
	bShutDown = false;
}

const VaRest::WebSocket::Lws::FServiceCounters* VaRest::WebSocket::Lws::FTransport::GetCounters() const
{
	return Service.IsValid() ? &Service->GetCounters() : nullptr;
}

bool VaRest::WebSocket::Lws::FTransport::IsShuttingDown() const
{
	return Service.IsValid() && Service->IsShuttingDown();
}

TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe> VaRest::WebSocket::Lws::FTransport::Connect(FVaRestSocketConnectRequest&& Request, TFunction<void()> OnEvents)
{
	using namespace VaRest::WebSocket::Lws::VaRestLwsTransportPrivate;
	check(IsInGameThread());
	LLM_SCOPE_BYTAG(VaRest_WebSocket);
	const uint64 ConnId = NextId;
    if (NextId == UINT64_MAX) bShutDown = true;
    else ++NextId; // Never wrap an actual Conn/request association.
	// Task 3 review M-1: the link names the connection in its own lines too -- the one label, FConn and the handle read it there
	const TSharedRef<FSocketLink, ESPMode::ThreadSafe> Link = MakeShared<FSocketLink, ESPMode::ThreadSafe>(MoveTemp(OnEvents), FConn::MakeLabel(ConnId, Request));
#if WITH_DEV_AUTOMATION_TESTS
	FPressureAdmission* Admission = PressureAdmission;
	const bool bPressureAdmission = Admission && Admission->Transport == this && Admission->Url == Request.Url;
	if (bPressureAdmission)
	{
		PressureAdmission = nullptr; // consume this exact admission before Prepare/start can produce any reentrant logging
		check(Admission->Probe->Connection.load(std::memory_order_acquire) == 0);
		Admission->Probe->Connection.store(ConnId, std::memory_order_release);
		Link->SetPressureProbeForTests(Admission->Probe);
	}
#endif
	TUniquePtr<FConn> Conn = MakeUnique<FConn>(ConnId, Link, MoveTemp(Request));
#if WITH_DEV_AUTOMATION_TESTS
	if (bPressureAdmission)
	{
		// Only this exact private loopback request gets a preparation copy. All other target/handshake/proxy rules still run.
		FServiceOptions PrepareOptions = Options;
		PrepareOptions.bOverrideAllowlist = true;
		PrepareOptions.Allowlist.Reset();
		PrepareOptions.Allowlist.Emplace(FString(TEXT("ws")), TArray<FString>{FString(TEXT("127.0.0.1"))});
		Conn->Prepare(PrepareOptions);
	}
	else
#endif
	Conn->Prepare(Options);
	if (!EnsureStarted())
	{
		return RefuseConnect(Link, ConnId);
	}
	if (Conn->NeedsTlsCount())
	{
		// Re-review N-2: counted here, on the game thread, where every FService::Start reads the count, so a transport started
		// right after this Connect -- before the service thread ran the Adopt -- still sees the attempt (VaRest-rti, I-3)
		Conn->MarkTlsCounted();
	}
	if (!Service->Enqueue([Moved = MoveTemp(Conn)](FService& Self) mutable { Self.Adopt(MoveTemp(Moved)); }))
	{
		// Task 6: Enqueue's result is used. Not reached while EnsureStarted holds -- Shutdown runs on this thread too -- but if
		// it were, the command and the FConn in it are released already (uncounting the attempt), and the consumer still gets
		// its one terminal event
		return RefuseConnect(Link, ConnId);
	}
	const auto Handle = MakeShared<FHandle, ESPMode::ThreadSafe>(Service, ConnId, Link);
#if WITH_DEV_AUTOMATION_TESTS
	if (bPressureAdmission) Admission->Probe->HandleIdentity.store(&Handle.Get(), std::memory_order_release);
#endif
	return Handle;
}

#if WITH_DEV_AUTOMATION_TESTS
TSharedPtr<IVaRestSocketConnection, ESPMode::ThreadSafe> VaRest::WebSocket::PressureTesting::ConnectOnDefault(
	int32 OwnedPort, FVaRestSocketConnectRequest Request, TSharedRef<FProbe, ESPMode::ThreadSafe> Probe)
{
	using namespace Lws;
	using namespace Lws::VaRestLwsTransportPrivate;
	check(IsInGameThread());
	if (OwnedPort < 1 || OwnedPort > 65535 || Probe->bCanceled.load(std::memory_order_acquire)
		|| Probe->Connection.load(std::memory_order_acquire) || PressureAdmission
		|| DefaultLiveConnections() != 0 || TlsHandshakesInFlight().load(std::memory_order_acquire) != 0) return {};
	if (const auto Previous = LastPressureProbe.Pin())
	{
		// A hard-failed test may release its GT owner while a native command still holds this probe. Do not start a new
		// wire lease in that contaminated state, even if its connection retired before its last fence acknowledged.
		if (!Previous->bRetired.load(std::memory_order_acquire) || Previous->InFlightRequests.load(std::memory_order_acquire) != 0) return {};
	}
	const FString Url = FString::Printf(TEXT("ws://127.0.0.1:%d%s"), OwnedPort, *OwnedPath(*Probe));
	if (Request.Url != Url || !Request.AutoReplies.IsEmpty() || !Request.Protocols.IsEmpty()) return {};
	FTransport& Transport = GetDefault();
	LastPressureProbe = Probe;
	FPressureAdmission Admission{&Transport, Url, Probe};
	TGuardValue<FPressureAdmission*> Guard(PressureAdmission, &Admission);
	const auto Handle = Transport.Connect(MoveTemp(Request));
	return Handle;
}

uint64 VaRest::WebSocket::PressureTesting::RequestPassFence(
	const TSharedRef<IVaRestSocketConnection, ESPMode::ThreadSafe>& Handle, const TSharedRef<FProbe, ESPMode::ThreadSafe>& Probe)
{
	check(IsInGameThread());
	if (Probe->bCanceled.load(std::memory_order_acquire) || Probe->bRetired.load(std::memory_order_acquire)
		|| Probe->HandleIdentity.load(std::memory_order_acquire) != &Handle.Get()) return 0;
	return static_cast<Lws::VaRestLwsTransportPrivate::FHandle&>(Handle.Get()).RequestPressureFence(Probe);
}

int32 VaRest::WebSocket::PressureTesting::DefaultLiveConnections()
{
	check(IsInGameThread());
	const Lws::FServiceCounters* Counters = Lws::GetDefault().GetCounters();
	return Counters ? Counters->LiveConnections.load(std::memory_order_acquire) : 0;
}

int32 VaRest::WebSocket::PressureTesting::DefaultContextCreations()
{
	check(IsInGameThread());
	const Lws::FServiceCounters* Counters = Lws::GetDefault().GetCounters();
	return Counters ? Counters->ContextCreations.load(std::memory_order_acquire) : 0;
}
#endif

VaRest::WebSocket::Lws::FDefaultTransportSlot::FDefaultTransportSlot(FServiceOptions InOptions)
	: Options(MoveTemp(InOptions))
{
}

TSharedRef<VaRest::WebSocket::Lws::FTransport, ESPMode::ThreadSafe> VaRest::WebSocket::Lws::FDefaultTransportSlot::Get()
{
	// Any thread (GetDefaultTransport's promise): the lock makes the first use make it once. FTransport's constructor only
	// copies the options -- the context and the thread come with its first Connect (EnsureStarted)
	FScopeLock Guard(&Lock);
	if (!Made.IsValid())
	{
		Made = MakeShared<FTransport, ESPMode::ThreadSafe>(Options);
	}
	return Made.ToSharedRef();
}

void VaRest::WebSocket::Lws::FDefaultTransportSlot::ShutdownForExit()
{
	check(IsInGameThread());
	// Task 7 review M-3: unconditionally, made here if nothing used it yet. Shutdown on a transport that never started only
	// sets bShutDown, so the first GetDefaultTransport after OnEnginePreExit gets a transport whose Connect ends with Shutdown
	Get()->Shutdown();
}

TSharedRef<IVaRestSocketTransport, ESPMode::ThreadSafe> VaRest::WebSocket::GetDefaultTransport()
{
	return VaRest::WebSocket::Lws::VaRestLwsTransportPrivate::DefaultSlot().Get();
}

VaRest::WebSocket::Lws::FTransport& VaRest::WebSocket::Lws::GetDefault()
{
	// The slot keeps the transport for the process's life, so the reference outlives the TSharedRef copy
	return VaRestLwsTransportPrivate::DefaultSlot().Get().Get();
}

void VaRest::WebSocket::Lws::OnModuleStartup()
{
	constexpr bool bSharedCopy = IS_MONOLITHIC || UE_MERGED_MODULES;
	// In a monolithic or merged build, ModuleExists lists the statically linked modules (ModuleManager.cpp:325-345, through
	// FindModules' statically linked branch, :283-322)
	const auto IsLinked = [](const TCHAR* Name) { return FModuleManager::Get().ModuleExists(Name); };
	if (!ShouldCreateAtStartup(bSharedCopy, IsLinked))
	{
		return;
	}
	// Plan decision 8 (VaRest-rti): the engine's WebSockets module first -- it makes its one context at startup
	// (WebSocketsModule.cpp:29-44) -- so that context exists before any VaRest handshake; then VaRest's own, before game code
	// can start an engine handshake. Creations by WebSocketServer, WebSocketNetworking and HttpNetworkReplayStreaming later
	// remain a documented residual risk (spec 1.2)
	if (IsLinked(TEXT("WebSockets")))
	{
		FModuleManager::Get().LoadModule(TEXT("WebSockets"));
	}
	GetDefault().EnsureStarted();
}

void VaRest::WebSocket::Lws::ShutdownDefault()
{
	VaRestLwsTransportPrivate::DefaultSlot().ShutdownForExit();
}

void VaRest::WebSocket::Lws::ResetDefaultAfterFork()
{
	// A transport made here has no service yet: its ResetAfterFork only clears bShutDown
	GetDefault().ResetAfterFork();
	OnModuleStartup();
}
