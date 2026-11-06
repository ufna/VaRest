// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeOwnerServiceAdapter.h"

#if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)

#include "HAL/RunnableThread.h"
#include <type_traits>
#include "../VaRestWebSocketLog.h"

namespace VaRest::WebSocket::Lws
{
	FNativeOwnerServiceChildSnapshot::FNativeOwnerServiceChildSnapshot(const FNativeOwnerServiceValues& InValues,
		VrSession InSession, NumericConnectorC1::FNative1* InNative, const NumericConnectorC1::FResourceBinding1* InResources, lws_context* InContext, const VrLwsCustody1* InView, lws_vhost* V0, lws_vhost* V1)
		: Values(InValues), Session(InSession), Native(InNative), Resources(InResources), Context(InContext), View(InView), Vhosts{V0,V1}
	{
		static_assert(sizeof(FNativeOwnerServiceValues) == 160 && sizeof(VrSession) == 16);
		static_assert(sizeof(std::atomic<int32>) == 4 && sizeof(std::atomic<uint32>) == 4);
		static_assert(sizeof(FNativeOwnerServiceChildSnapshot) == 248 && alignof(FNativeOwnerServiceChildSnapshot) == 8);
		static_assert(std::is_standard_layout_v<FNativeOwnerServiceChildSnapshot>);
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceChildSnapshot::RegisterFromActualInit() const
	{
		// Actual real-child Init, BEFORE Core's existing true-Init sync/TLS step.
		// No parent journal, UE TLS, sharedptr or GT phase access occurs here.
		const NumericConnectorC1::EStatus1 Entered = NumericConnectorC1::EnterService1(Native, Resources);
		C1EntryDiagnostic.store(static_cast<int32>(Entered), std::memory_order_release);
		VrStatus Status = VR_STATUS_STALE; // Explicit source-derived skipped call, not a producer observation.
		if ((Entered == NumericConnectorC1::EStatus1::OK || Entered == NumericConnectorC1::EStatus1::STOPPING)
			&& LocalPermit.load(std::memory_order_acquire) == 1)
		{
			Status = Values.Calls.RegisterCurrentThread(Values.Owner, Session); // Sole attempt.
		}
		// Actual native0 stays0 even if Stop closed the advisory permit meanwhile.
		RegistrationDiagnostic.store(Status, std::memory_order_release);
		if (Status != VR_STATUS_OK) LocalPermit.store(0, std::memory_order_release);
		return Status;
	}

	[[gnu::used]] void FNativeOwnerServiceChildSnapshot::RunCleanupFromActualRun() const
	{
		const auto Entered = static_cast<NumericConnectorC1::EStatus1>(C1EntryDiagnostic.load(std::memory_order_acquire));
		if (Entered != NumericConnectorC1::EStatus1::OK && Entered != NumericConnectorC1::EStatus1::STOPPING)
			return; // No invented entry/no-child cleanup; keep skipped cleanup diagnostic PENDING.
		NumericConnectorC1::RequestStop1(Native);
		// A closed permit is not a rewritten successful registration. The raw
		// result selects genuine registered versus journal-only unregistered work.
		const NumericConnectorC1::EStatus1 Cleaned = RegistrationDiagnostic.load(std::memory_order_acquire) == VR_STATUS_OK
			? NumericConnectorC1::StopPairs1(Native, Resources)
			: NumericConnectorC1::StopUnregisteredServicePairs1(Native, Resources);
		C1CleanupDiagnostic.store(static_cast<int32>(Cleaned), std::memory_order_release);
		// One existing bounded cleanup turn only, no LWS loop or registration retry.
	}

	[[gnu::used]] void FNativeOwnerServiceChildSnapshot::RequestCleanupStopFromActualStop() const
	{
		LocalPermit.store(0, std::memory_order_release);
		NumericConnectorC1::RequestStop1(Native); // Sole cross-thread C1 atomic source operation.
	}
	[[gnu::used]] int32 FNativeOwnerServiceChildSnapshot::GetC1EntryDiagnostic() const
	{ return C1EntryDiagnostic.load(std::memory_order_acquire); }
	[[gnu::used]] int32 FNativeOwnerServiceChildSnapshot::GetC1CleanupDiagnostic() const
	{ return C1CleanupDiagnostic.load(std::memory_order_acquire); }

	[[gnu::used]] bool FNativeOwnerServiceChildSnapshot::IsLocalNativeWorkPermitted() const
	{
		return LocalPermit.load(std::memory_order_acquire) == 1
			&& RegistrationDiagnostic.load(std::memory_order_acquire) == VR_STATUS_OK;
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceChildSnapshot::GetRegistrationDiagnostic() const
	{
		return RegistrationDiagnostic.load(std::memory_order_acquire);
	}

	void FNativeOwnerServiceChildSnapshot::CloseLocalPermit()
	{
		LocalPermit.store(0, std::memory_order_release);
	}

	FNativeOwnerServiceAdapter::FNativeOwnerServiceAdapter(const FNativeOwnerServiceValues& InValues)
		: Values(InValues)
	{
		static_assert(sizeof(VrLwsPhaseBudget1) == 16 && alignof(VrLwsPhaseBudget1) == 4);
		static_assert(alignof(FNativeOwnerServiceAdapter) == 8);
		// ONLY new Adapter member is this tail16 after the old int32 tail:
		// no extra inter-member padding; natural8 whole-object growth is16.
	}

	[[gnu::used]] TSharedPtr<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> FNativeOwnerServiceAdapter::CreateForService(const FNativeOwnerServiceValues& Values)
	{
		if (RequireNativeOwnerControlThread(TEXT("service adapter construction")) != VR_STATUS_OK
			|| !NativeOwnerValuesHaveValidShape(Values))
		{
			return nullptr;
		}
		return TSharedPtr<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe>(new FNativeOwnerServiceAdapter(Values));
	}

	bool FNativeOwnerServiceAdapter::HasOutstandingCreatingResponsibility() const
	{
		return Attaching || CommittingOutcome || (Outcome != ECreatingOutcome::None && !LocalCallsRetired);
	}

	FNativeOwnerServiceAdapter::~FNativeOwnerServiceAdapter()
	{
		if (RequireNativeOwnerControlThread(TEXT("service adapter destruction")) != VR_STATUS_OK)
		{
			return;
		}
		if (HasOutstandingCreatingResponsibility())
		{
			UE_LOG(LogVaRestSocket, Error, TEXT("Native owner adapter destroyed with an unresolved creating journal; the creating owner violated retention, no completion is claimed"));
		}
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::AttachBeforeCreatingResources()
	{
		if (RequireNativeOwnerControlThread(TEXT("native service attach")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (!NativeOwnerRealThreadModeAllowed())
		{
			return VR_STATUS_DISCOVERY_UNESTABLISHED;
		}
		if (Outcome != ECreatingOutcome::None || LocalCallsRetired || Attaching || StopRequested)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		Attaching = true;
		VrSnapshot Snapshot {};
		Snapshot.size = sizeof(Snapshot);
		Snapshot.version = VR_OWNER_VERSION;
		VrStatus Status = Values.Api.query(Values.Owner, &Snapshot);
		if (Status != VR_STATUS_OK)
		{
			Attaching = false;
			return Status;
		}
		if (Snapshot.size != sizeof(Snapshot) || Snapshot.version != VR_OWNER_VERSION || Snapshot.reserved != 0
			|| Snapshot.owner_epoch == 0 || Snapshot.image.hi != Values.Image.hi || Snapshot.image.lo != Values.Image.lo
			|| Snapshot.occupied_slots > VR_MAX_SLOTS || Snapshot.connector_leased > 1 || Snapshot.service_executing > 1
			|| Snapshot.state > VR_STATE_POISONED || Snapshot.poison_reason > VR_POISON_NATIVE_INVARIANT
			|| (Snapshot.state == VR_STATE_POISONED) != (Snapshot.poison_reason != VR_POISON_NONE)
			|| (Snapshot.connector_leased == 0) != (Snapshot.service_epoch == 0))
		{
			Attaching = false;
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		if (Snapshot.state == VR_STATE_POISONED)
		{
			Attaching = false;
			return VR_STATUS_POISONED;
		}
		if (Snapshot.state != VR_STATE_ACTIVE && Snapshot.state != VR_STATE_DORMANT)
		{
			Attaching = false;
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		if (StopRequested)
		{
			Attaching = false;
			return VR_STATUS_STALE;
		}
		VrSession Issued {};
		Status = Values.Api.attach_service(Values.Owner, &Issued);
		if (Status != VR_STATUS_OK)
		{
			Attaching = false;
			return Status;
		}
		// Store actual obtained responsibility BEFORE any fallible UE allocation.
		Session = Issued;
		Outcome = ECreatingOutcome::StartReserved;
		if (Session.owner_epoch != Snapshot.owner_epoch || Session.service_epoch == 0)
		{
			Outcome = ECreatingOutcome::Uncertain;
			Attaching = false;
			return VR_STATUS_ABI_INCOMPATIBLE;
		}
		if (!StopRequested)
		{
			Pump = FActiveOwnerPump::Create(Values, Session);
			Status = Pump.IsValid() ? Pump->RegisterBeforeServicePermit() : VR_STATUS_NATIVE_ERROR;
		}
		else
		{
			Status = VR_STATUS_STALE;
		}
		if (Status == VR_STATUS_OK && !StopRequested)
		{
			// Snapshot needs actual resource pointers, so its SINGLE allocation is
			// deferred to the genuine pre-create preparation after those exist.
			Attaching = false;
			return VR_STATUS_OK;
		}
		// No create was possible before this method returned a child snapshot.
		StopRequested = true;
		const VrStatus Stopped = StopPump();
		Outcome = ECreatingOutcome::NeverCreatedPending;
		Attaching = false;
		const VrStatus Recorded = CommitKnownNoChild();
		return Status == VR_STATUS_RELEASE_UNCERTAIN || Stopped == VR_STATUS_RELEASE_UNCERTAIN || Recorded == VR_STATUS_RELEASE_UNCERTAIN
			? VR_STATUS_RELEASE_UNCERTAIN : (Stopped != VR_STATUS_OK ? Stopped : (Recorded != VR_STATUS_OK ? Recorded : (Status == VR_STATUS_OK ? VR_STATUS_STALE : Status)));
	}

    FVaRestNativeOwnerHostStep1 FNativeOwnerServiceAdapter::PreserveFrameDisposition1(FVaRestNativeOwnerHostStep1 Step) const
    {
        // Actual GT-only creating/detach/removal observations only; never rewrite
        // them as a journal of this frame. Terminal poll results stay with Host.
        if (LastOutcomeStatus == VR_STATUS_RELEASE_UNCERTAIN || LastDetachStatus == VR_STATUS_RELEASE_UNCERTAIN
            || (Pump.IsValid() && Pump->LastRemovalStatus == VR_STATUS_RELEASE_UNCERTAIN))
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Retained),VR_STATUS_RELEASE_UNCERTAIN};
        if (Pump.IsValid() && (Pump->InCallback || Pump->Registering))
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Pending),VR_STATUS_BUSY};
        return Step;
    }

    FVaRestNativeOwnerHostStep1 FNativeOwnerServiceAdapter::PumpOnControlThread()
    {
        const VrStatus Role = RequireNativeOwnerControlThread(TEXT("existing adapter direct frame"));
        if (Role != VR_STATUS_OK) return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),Role};
        const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
        if (!NativeOwnerRealThreadModeAllowed())
            return {16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_FORK_UNESTABLISHED};
        if (Attaching || CommittingOutcome)
            return PreserveFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Pending),VR_STATUS_BUSY});
        if (StopRequested || LocalCallsRetired || CreatingResourcesClosed)
            return PreserveFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_STALE});
        if (Outcome != ECreatingOutcome::CreatedJoinUnobserved || !CreatedThread || !Child.IsValid()
            || Session.owner_epoch == 0 || Session.service_epoch == 0 || !Pump.IsValid())
            return PreserveFrameDisposition1({16,1,static_cast<uint32>(EVaRestNativeOwnerHostOutcome1::Refused),VR_STATUS_OLD_OWNER_BUSY});
        const TSharedRef<FActiveOwnerPump, ESPMode::ThreadSafe> Current = Pump.ToSharedRef();
        const FVaRestNativeOwnerHostStep1 Actual = Current->PumpOnControlThread();
        // Reentrant Stop may release the service's Pump reference. Both this
        // local pin and Current survive the FULL call and all balanced returns.
        return PreserveFrameDisposition1(Actual);
    }

	VrStatus FNativeOwnerServiceAdapter::StopPump()
	{
		if (Child.IsValid())
		{
			Child->CloseLocalPermit();
		}
		if (Pump.IsValid())
		{
			return Pump->StopOnControlThread();
		}
		if (LastOutcomeStatus != VR_STATUS_PENDING && LastOutcomeStatus != VR_STATUS_OK && LastOutcomeStatus != VR_STATUS_BUSY)
		{
			return LastOutcomeStatus;
		}
		Values.Calls.RequestStop(Values.Owner, Session);
		return VR_STATUS_OK; // Void call is valid only under the actual callsite premises.
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::StopBeforeCreatingJoin()
	{
		if (RequireNativeOwnerControlThread(TEXT("service stop before creating join")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (LocalCallsRetired)
		{
			return VR_STATUS_OK;
		}
		StopRequested = true;
		return Outcome == ECreatingOutcome::None ? VR_STATUS_OK : StopPump();
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::InitializeStartupPhaseBeforeResources()
	{
		if (RequireNativeOwnerControlThread(TEXT("native creating phase initialization")) != VR_STATUS_OK) return VR_STATUS_WRONG_THREAD;
		if (Attaching || StopRequested || CreatePrepared || LocalCallsRetired || Outcome != ECreatingOutcome::StartReserved
			|| CreatingPhase.Phase || CreatingPhase.Remaining || CreatingPhase.Used || CreatingPhase.Reserved) return VR_STATUS_INVALID_INPUT;
		static_assert(sizeof(VrLwsPhaseBudget1) == 16 && alignof(VrLwsPhaseBudget1) == 4);
		// Held conservative source budget, NOT observed NativeCalls51/4. Init's4
		// fits the existing helper guard; no new128/124 runtime allowance exists.
		CreatingPhase = {1536 - 55, 55, VR_LWS_PHASE_STARTUP1, 0};
		return VR_STATUS_OK;
	}
	[[gnu::used]] VrLwsPhaseBudget1* FNativeOwnerServiceAdapter::GetCreatingPhaseOnControlThread()
	{
		if (RequireNativeOwnerControlThread(TEXT("native creating phase borrow")) != VR_STATUS_OK || CreatingPhase.Reserved) return nullptr;
		const uint32 Cap = CreatingPhase.Phase == VR_LWS_PHASE_STARTUP1 ? 1536u
			: (CreatingPhase.Phase == VR_LWS_PHASE_FINAL1 ? 2048u : 0u);
		return Cap && CreatingPhase.Used >= 55 && CreatingPhase.Used <= Cap
			&& CreatingPhase.Remaining == Cap - CreatingPhase.Used ? &CreatingPhase : nullptr;
	}
	void FNativeOwnerServiceAdapter::EnterFinalPhaseAfterKnownOutcome()
	{
		// Genuine no-child/first successful join is established at the ONLY callers.
		if (CreatingPhase.Phase == VR_LWS_PHASE_STARTUP1)
			CreatingPhase = {2048 - 55, 55, VR_LWS_PHASE_FINAL1, 0};
		// Phase0 is genuine metadata-only I3-A; phase2 is never renewed.
	}
	[[gnu::used]] const FNativeOwnerServiceChildSnapshot* FNativeOwnerServiceAdapter::PrepareRealThreadCreate(
		NumericConnectorC1::FNative1* Native, const NumericConnectorC1::FResourceBinding1* Resources,
        lws_context* Context, const VrLwsCustody1* View, lws_vhost* V0, lws_vhost* V1)
	{
		if (RequireNativeOwnerControlThread(TEXT("real service create preparation")) != VR_STATUS_OK
			|| !NativeOwnerRealThreadModeAllowed() || Attaching || StopRequested || CreatePrepared || CreatingResourcesClosed || LocalCallsRetired
			|| Outcome != ECreatingOutcome::StartReserved || Child.IsValid() || !Native || !Resources
			|| !NumericConnectorC1::ResourceBindingValid1(Resources) || Resources->Owner != Values.Owner
			|| Resources->OwnerEpoch != Session.owner_epoch || Resources->ServiceEpoch != Session.service_epoch
			|| !GetCreatingPhaseOnControlThread() || CreatingPhase.Phase != VR_LWS_PHASE_STARTUP1) return nullptr;
		// Actual resources/source lifetime are caller premises, not pointer checks.
		Child.Reset(new FNativeOwnerServiceChildSnapshot(Values, Session, Native, Resources, Context, View, V0, V1));
		if (!Child.IsValid() || StopRequested)
		{
			if (Child.IsValid()) Child->CloseLocalPermit();
			return nullptr; // Factory was NOT attempted. FService's InCreate protects reentry.
		}
		CreatePrepared = true;
		return Child.Get();
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::ObserveRealThreadCreated(FRunnableThread& Thread)
	{
		if (RequireNativeOwnerControlThread(TEXT("known real service create outcome")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (!CreatePrepared || CreatedThread || Outcome != ECreatingOutcome::StartReserved || LocalCallsRetired)
		{
			return VR_STATUS_INVALID_INPUT;
		}
		CreatedThread = &Thread;
		if (Thread.GetThreadType() != FRunnableThread::ThreadType::Real)
		{
			Outcome = ECreatingOutcome::Uncertain;
			StopRequested = true;
			(void)StopPump();
			return VR_STATUS_NATIVE_ERROR;
		}
		Outcome = ECreatingOutcome::CreatedJoinUnobserved;
		return VR_STATUS_OK;
	}

	VrStatus FNativeOwnerServiceAdapter::CommitKnownNoChild()
	{
		if (CommittingOutcome)
		{
			return VR_STATUS_BUSY;
		}
		EnterFinalPhaseAfterKnownOutcome();
		CommittingOutcome = true;
		LastOutcomeStatus = Values.Calls.NeverCreated(Values.Owner, Session);
		CommittingOutcome = false;
		if (LastOutcomeStatus == VR_STATUS_OK)
		{
			Outcome = ECreatingOutcome::NeverCreatedRecorded;
		}
		return LastOutcomeStatus;
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::AbortStartupBeforeThreadCreate()
	{
		if (RequireNativeOwnerControlThread(TEXT("known pre-create startup abort")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (CreatePrepared || CreatedThread || Outcome != ECreatingOutcome::StartReserved || Attaching || LocalCallsRetired)
		{
			return VR_STATUS_INVALID_INPUT;
		}
		StopRequested = true;
		const VrStatus Stopped = StopPump();
		Outcome = ECreatingOutcome::NeverCreatedPending;
		const VrStatus Recorded = CommitKnownNoChild();
		return Stopped == VR_STATUS_RELEASE_UNCERTAIN || Recorded == VR_STATUS_RELEASE_UNCERTAIN
			? VR_STATUS_RELEASE_UNCERTAIN : (Stopped == VR_STATUS_OK ? Recorded : Stopped);
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::ObserveKnownNoChildAfterCreateFailure()
	{
		if (RequireNativeOwnerControlThread(TEXT("actual no-child create failure")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		// Actual approved real-create failure/no-child proof is an I3 source premise.
		if (!CreatePrepared || CreatedThread || Outcome != ECreatingOutcome::StartReserved || LocalCallsRetired)
		{
			return VR_STATUS_INVALID_INPUT;
		}
		StopRequested = true;
		const VrStatus Stopped = StopPump();
		Outcome = ECreatingOutcome::NeverCreatedPending;
		const VrStatus Recorded = CommitKnownNoChild();
		return Stopped == VR_STATUS_RELEASE_UNCERTAIN || Recorded == VR_STATUS_RELEASE_UNCERTAIN
			? VR_STATUS_RELEASE_UNCERTAIN : (Stopped == VR_STATUS_OK ? Recorded : Stopped);
	}

	[[gnu::used]] void FNativeOwnerServiceAdapter::ObserveUncertainCreateOutcome()
	{
		if (RequireNativeOwnerControlThread(TEXT("uncertain real service create outcome")) != VR_STATUS_OK)
		{
			return;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (!CreatePrepared || Outcome != ECreatingOutcome::StartReserved || LocalCallsRetired)
		{
			return;
		}
		Outcome = ECreatingOutcome::Uncertain;
		StopRequested = true;
		(void)StopPump();
		// No nullptr/absence inference, no NeverCreated or join, no second create.
	}

	[[gnu::used]] FRunnableThread* FNativeOwnerServiceAdapter::PrepareFirstCreatingJoin()
	{
		if (RequireNativeOwnerControlThread(TEXT("first real creating join preparation")) != VR_STATUS_OK
			|| !StopRequested || JoinPrepared || Outcome != ECreatingOutcome::CreatedJoinUnobserved || !CreatedThread || LocalCallsRetired)
		{
			return nullptr;
		}
		JoinPrepared = true;
		return CreatedThread; // I3 alone performs the actual first sole-owner Kill(true).
	}

	VrStatus FNativeOwnerServiceAdapter::CommitKnownJoin()
	{
		if (CommittingOutcome)
		{
			return VR_STATUS_BUSY;
		}
		CommittingOutcome = true;
		LastOutcomeStatus = Values.Calls.JoinObserved(Values.Owner, Session);
		CommittingOutcome = false;
		if (LastOutcomeStatus == VR_STATUS_OK || LastOutcomeStatus == VR_STATUS_BUSY)
		{
			// S3 BUSY is AFTER durable genuine-fact publication, never a join failure.
			JoinFactDurable = true;
			EnterFinalPhaseAfterKnownOutcome();
		}
		if (LastOutcomeStatus == VR_STATUS_OK)
		{
			Outcome = ECreatingOutcome::JoinedRecorded;
		}
		return LastOutcomeStatus;
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::ObserveKnownFirstSuccessfulJoin()
	{
		if (RequireNativeOwnerControlThread(TEXT("actual first successful service join")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		// Sole approved real thread's first Kill(true) ALREADY returned success.
		if (!JoinPrepared || Outcome != ECreatingOutcome::CreatedJoinUnobserved || !CreatedThread || LocalCallsRetired)
		{
			return VR_STATUS_INVALID_INPUT;
		}
		Outcome = ECreatingOutcome::JoinedPending;
		CreatedThread = nullptr; // Never deletion or evidence inferred from pointer absence.
		return CommitKnownJoin();
	}

	[[gnu::used]] void FNativeOwnerServiceAdapter::ObserveUncertainFirstJoin()
	{
		if (RequireNativeOwnerControlThread(TEXT("uncertain first service join")) != VR_STATUS_OK)
		{
			return;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (!JoinPrepared || Outcome != ECreatingOutcome::CreatedJoinUnobserved || LocalCallsRetired)
		{
			return;
		}
		Outcome = ECreatingOutcome::Uncertain;
		if (Child.IsValid())
		{
			Child->CloseLocalPermit();
		}
		// No second join, NeverCreated, JoinObserved, delete or guessed completion.
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::RetryPendingOutcomeOnControlThread()
	{
		if (RequireNativeOwnerControlThread(TEXT("pending creating outcome bookkeeping")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (LocalCallsRetired)
		{
			return NativeDetached ? VR_STATUS_OK : VR_STATUS_PENDING;
		}
		if (CommittingOutcome)
		{
			return VR_STATUS_BUSY;
		}
		if (LastOutcomeStatus != VR_STATUS_BUSY)
		{
			return LastOutcomeStatus; // No blind retry after another refusal/uncertainty.
		}
		if (Outcome == ECreatingOutcome::NeverCreatedPending)
		{
			return CommitKnownNoChild();
		}
		if (Outcome == ECreatingOutcome::JoinedPending)
		{
			return CommitKnownJoin();
		}
		return VR_STATUS_OLD_OWNER_BUSY;
	}

	void FNativeOwnerServiceAdapter::RetireLocalCallingValues()
	{
		// Called only at the genuine creating resource/callback completion boundary.
		if (!CreatingResourcesClosed)
		{
			return;
		}
		// Function-pointer bits are data only; after this point no method uses them.
		LocalCallsRetired = true;
		Session = {};
		Child.Reset(); // Real child is known joined, or genuinely never existed.
		Pump.Reset(); // A still-executing callback retains its independent local/dispatch pin.
	}

	[[gnu::used]] VrStatus FNativeOwnerServiceAdapter::AfterKnownCreatingResourcesClosed()
	{
		if (RequireNativeOwnerControlThread(TEXT("actual creating resource closure")) != VR_STATUS_OK)
		{
			return VR_STATUS_WRONG_THREAD;
		}
		const TSharedRef<FNativeOwnerServiceAdapter, ESPMode::ThreadSafe> KeepAlive = AsShared();
		if (LocalCallsRetired)
		{
			return NativeDetached ? VR_STATUS_OK : VR_STATUS_PENDING;
		}
		// The actual I3 acquire/known-close and callback-return boundaries establish
		// this event. No caller Boolean or counter normalization supplies the fact.
		if (!StopRequested)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		CreatingResourcesClosed = true;
		if (Attaching || CommittingOutcome || (Pump.IsValid() && !Pump->CanDropServiceReference()))
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		if ((Outcome == ECreatingOutcome::JoinedPending || Outcome == ECreatingOutcome::JoinedRecorded) && JoinFactDurable)
		{
			// Native S3 owns the durable deferred request. No UE callback or later
			// call through these addresses is needed, and no detach OK is invented.
			LastDetachStatus = VR_STATUS_PENDING;
			RetireLocalCallingValues();
			return VR_STATUS_PENDING;
		}
		if (Outcome != ECreatingOutcome::NeverCreatedRecorded)
		{
			return VR_STATUS_OLD_OWNER_BUSY;
		}
		if (LastDetachStatus != VR_STATUS_PENDING && LastDetachStatus != VR_STATUS_BUSY && LastDetachStatus != VR_STATUS_OLD_OWNER_BUSY)
		{
			return LastDetachStatus; // In particular, STALE is not guessed completion.
		}
		LastDetachStatus = Values.Api.detach_service(Values.Owner, Session);
		if (LastDetachStatus == VR_STATUS_OK)
		{
			NativeDetached = true;
			RetireLocalCallingValues();
		}
		return LastDetachStatus;
	}

	[[gnu::used]] bool FNativeOwnerServiceAdapter::CanReleaseCreatingAdapter() const
	{
		if (RequireNativeOwnerControlThread(TEXT("creating adapter release check")) != VR_STATUS_OK)
		{
			return false;
		}
		return !HasOutstandingCreatingResponsibility();
	}
}

#endif
