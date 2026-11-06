// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerPump.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "CoreGlobals.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTLS.h"
#include "Misc/CommandLine.h"
#include "Misc/Fork.h"
#include "../VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws
{
	bool NativeOwnerValuesHaveValidShape(const FNativeOwnerServiceValues& Values)
	{
		return Values.Owner != nullptr && reinterpret_cast<UPTRINT>(Values.Owner) % alignof(uint64_t) == 0
			&& Values.Image.hi != 0 && Values.Image.lo != 0
			&& Values.Api.size == sizeof(VrOwnerApi2) && Values.Api.version == VR_OWNER_VERSION
			&& Values.Api.query && Values.Api.poll_retirement && Values.Api.attach_service && Values.Api.detach_service
			&& Values.Api.submit && Values.Api.copy_result && Values.Api.discard && Values.Api.retire
			&& Values.Calls.NeverCreated && Values.Calls.RegisterCurrentThread && Values.Calls.JoinObserved
			&& Values.Calls.RequestStop && Values.Calls.PumpRegistered && Values.Calls.PumpEnter
			&& Values.Calls.PumpLeave && Values.Calls.RequestDetachAfterJoin;
	}

	VrStatus RequireNativeOwnerControlThread(const TCHAR* Operation)
	{
		// Ordinary runtime branches remain active in Shipping. A task tag or an
		// early IsInGameThread true cannot appoint the physical control thread.
		if (!GIsGameThreadIdInitialized || FPlatformTLS::GetCurrentThreadId() != GGameThreadId || !IsInGameThread())
		{
			UE_LOG(LogVaRestSocket, Error, TEXT("Native owner lifecycle refused outside the initialized physical game thread: %s"), Operation);
			return VR_STATUS_WRONG_THREAD;
		}
		return VR_STATUS_OK;
	}

	bool NativeOwnerRealThreadModeAllowed()
	{
		return FCommandLine::IsInitialized() && FPlatformProcess::SupportsMultithreading() && !FForkProcessHelper::IsForkRequested()
			&& !FForkProcessHelper::IsForkedChildProcess() && !FForkProcessHelper::IsForkedMultithreadInstance();
	}

	FActiveOwnerPump::FActiveOwnerPump(const FNativeOwnerServiceValues& Values, VrSession InSession)
		: Image(Values.Image), Owner(Values.Owner), Api(Values.Api), Session(InSession)
		, RequestStop(Values.Calls.RequestStop), SetRegistered(Values.Calls.PumpRegistered)
		, Enter(Values.Calls.PumpEnter), Leave(Values.Calls.PumpLeave)
	{
	}

	[[gnu::used]] TSharedPtr<FActiveOwnerPump, ESPMode::ThreadSafe> FActiveOwnerPump::Create(const FNativeOwnerServiceValues& Values, VrSession Session)
	{
		if (RequireNativeOwnerControlThread(TEXT("pump construction")) != VR_STATUS_OK
			|| !NativeOwnerValuesHaveValidShape(Values) || Session.owner_epoch == 0 || Session.service_epoch == 0)
		{
			return nullptr;
		}
		return TSharedPtr<FActiveOwnerPump, ESPMode::ThreadSafe>(new FActiveOwnerPump(Values, Session));
	}

	FActiveOwnerPump::~FActiveOwnerPump()
	{
		if (RequireNativeOwnerControlThread(TEXT("pump destruction")) != VR_STATUS_OK)
		{
			return;
		}
		if (BeginFrameHandle.IsValid() || Registering || NativeRegistrationKnown || RemovalPending || InCallback)
		{
			UE_LOG(LogVaRestSocket, Error, TEXT("Native owner pump destruction violated its completed-removal/return precondition; no native completion is claimed"));
		}
	}

	VrStatus FActiveOwnerPump::RegisterBeforeServicePermit()
	{
		if (RequireNativeOwnerControlThread(TEXT("pump registration")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FActiveOwnerPump, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (Stopped)
		{
			return VR_STATUS_STALE;
		}
		if (Registering || BeginFrameHandle.IsValid() || NativeRegistrationKnown || RemovalPending)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
        // Genuine pre-Start Host control flow ALREADY committed the direct
        // frame/retention interval under qualified Source H. This scalar call
        // records it under the actual lease; no bit/delegate creates authority.
		Registering = true;
		VrStatus Status = Stopped ? VR_STATUS_STALE : SetRegistered(Owner, Session, 1);
		if (Status == VR_STATUS_OK)
		{
			NativeRegistrationKnown = true;
		}
		Registering = false;
		if (Status != VR_STATUS_OK || Stopped)
		{
			// Issued native service responsibility survives failed registration/bookkeeping.
			const VrStatus Removed = StopOnControlThread();
			return Status == VR_STATUS_RELEASE_UNCERTAIN || Removed == VR_STATUS_RELEASE_UNCERTAIN
				? VR_STATUS_RELEASE_UNCERTAIN : (Removed == VR_STATUS_OK ? (Status == VR_STATUS_OK ? VR_STATUS_STALE : Status) : Removed);
		}
		Enabled = true;
		return VR_STATUS_OK;
	}

	bool FActiveOwnerPump::CanDropServiceReference() const
	{
        // The actual GT keeps the service reference while its frame is entered;
        // current local Service/Adapter/Pump pins and the real lease/code protection
        // also survive Leave and FULL return. Later finish work may drop it.
		return Stopped && StopIssued && !BeginFrameHandle.IsValid() && !Registering && !NativeRegistrationKnown && !RemovalPending && !InCallback;
	}

	VrStatus FActiveOwnerPump::StopOnControlThread()
	{
		if (RequireNativeOwnerControlThread(TEXT("pump stop/removal")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FActiveOwnerPump, ESPMode::ThreadSafe> KeepAlive = AsShared();
        Enabled = false; // Cut future direct entries BEFORE Stop/native bookkeeping.
        Stopped = true;
        // Known deregistration/Stop accounting is distinct from permission to
        // drop the service reference while an entered frame still has to return.
        if (StopIssued && !BeginFrameHandle.IsValid() && !Registering && !NativeRegistrationKnown && !RemovalPending)
        {
            return VR_STATUS_OK; // Already accounted: NEVER repeat native0 merely because InCallback is still true.
        }
		if (RemovalPending && LastRemovalStatus != VR_STATUS_PENDING && LastRemovalStatus != VR_STATUS_BUSY)
		{
			return LastRemovalStatus; // A refused/unknown native state is not blindly retried.
		}
		if (!StopIssued)
		{
			RequestStop(Owner, Session);
			StopIssued = true; // Callsite premises, not a success acknowledgement from void.
		}
		if (Registering)
		{
			// The outer registration return will account the real native registration.
			RemovalPending = true;
			LastRemovalStatus = VR_STATUS_BUSY;
			return VR_STATUS_BUSY;
		}
		const VrStatus Status = SetRegistered(Owner, Session, 0);
		LastRemovalStatus = Status;
		RemovalPending = Status != VR_STATUS_OK;
		if (Status == VR_STATUS_OK)
		{
			NativeRegistrationKnown = false;
		}
		return Status;
	}

    FVaRestNativeOwnerHostStep1 FActiveOwnerPump::PumpOnControlThread()
    {
        const VrStatus Role = RequireNativeOwnerControlThread(TEXT("direct Host frame pump"));
        if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
        const TSharedRef<FActiveOwnerPump, ESPMode::ThreadSafe> KeepAlive = AsShared();
        if (!NativeOwnerRealThreadModeAllowed())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
        if (LastRemovalStatus == VR_STATUS_RELEASE_UNCERTAIN)
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Retained),VR_STATUS_RELEASE_UNCERTAIN};
        if (Registering || InCallback)
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Pending),VR_STATUS_BUSY};
        if (!Enabled || Stopped || StopIssued || !NativeRegistrationKnown || RemovalPending || BeginFrameHandle.IsValid())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_STALE};
        InCallback = true;
        const VrStatus Entered = Enter(Owner, Session);
        if (Entered != VR_STATUS_OK)
        {
            InCallback = false;
            if (Entered != VR_STATUS_BUSY && Entered != VR_STATUS_PENDING && Entered != VR_STATUS_OLD_OWNER_BUSY) Enabled = false;
            if (Entered == VR_STATUS_RELEASE_UNCERTAIN || LastRemovalStatus == VR_STATUS_RELEASE_UNCERTAIN)
                return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Retained),VR_STATUS_RELEASE_UNCERTAIN};
            return {16,1,static_cast<uint32>(Entered == VR_STATUS_BUSY || Entered == VR_STATUS_PENDING || Entered == VR_STATUS_OLD_OWNER_BUSY
                ? EVaRestNativeOwnerHostOutcome1::Pending : EVaRestNativeOwnerHostOutcome1::Retained),Entered}; // No acquired charge, NO Leave.
        }
        VrSnapshot Snapshot {};
        Snapshot.size = sizeof(Snapshot); // Existing ABI2 Snapshot64; no result/owner copy escapes.
        Snapshot.version = VR_OWNER_VERSION;
        const VrStatus Polled = Api.poll_retirement(Owner, &Snapshot); // Exactly ONE poll.
        // EVERY returned poll status/reentrant Stop keeps this SAME pinned
        // invocation and immutable Session through exactly ONE atomic-clear Leave.
        Leave(Owner, Session);
        InCallback = false;
        if (Polled != VR_STATUS_OK && Polled != VR_STATUS_BUSY && Polled != VR_STATUS_PENDING) Enabled = false;
        if (Polled == VR_STATUS_RELEASE_UNCERTAIN || LastRemovalStatus == VR_STATUS_RELEASE_UNCERTAIN)
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Retained),VR_STATUS_RELEASE_UNCERTAIN};
        // No last-frame cache: never repurpose removal/creating journals. Host
        // retains terminal actual results and ceases future dispatch. A later
        // truthful local refusal cannot discharge that earlier retained duty.
        return {16,1,static_cast<uint32>(Polled == VR_STATUS_OK ? EVaRestNativeOwnerHostOutcome1::Complete
            : (Polled == VR_STATUS_BUSY || Polled == VR_STATUS_PENDING ? EVaRestNativeOwnerHostOutcome1::Pending
                : EVaRestNativeOwnerHostOutcome1::Retained)),Polled};
    }
}

#endif
