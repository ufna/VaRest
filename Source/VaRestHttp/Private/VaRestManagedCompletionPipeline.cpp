// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "UObject/StrongObjectPtr.h"
#include "VaRestClientState.h"
#include "VaRestDecode.h"
#include "VaRestHttpTesting.h"
#include "VaRestManagedCompletion.h"
#include "VaRestManagedOperation.h"
#include "VaRestPersistence.h"
#include "VaRestQueueReplay.h"
#include "VaRestTraceExcerpts.h"
#include "VaRestTracePoints.h"
#include "VaRestUrl.h"

namespace VaRest::Http
{
void FClientState::BeginManagedCompletion(const FVaRestExchangeRef& Exchange, FFinish&& Finish, bool bConsiderDecode)
{
	check(IsInGameThread());
	auto& Ex = Internals(*Exchange);
	const auto Ref = Ex.Managed;
	const auto Control = Ref.Control;
	if (Control->IsStopped() || Ex.bFinished)
		return;
	FNativeLogicalTailLease Completion, Delivery;
	TSharedPtr<FManagedCompletionAccess, ESPMode::ThreadSafe> Selected;
	if (!Ref.Store->TakeManagedCompletionOnGT(Ref, Selected, Delivery) && !Ref.Store->TakeInitialCompletionOnGT(Ref, Completion, Delivery))
		return; // real selected decode/completion already owns the initial tail; cancellation is its atomic input
	auto* Operation = Selected ? &Selected->OperationOnGT() : Completion.ManagedPayloadOnGT();
	check(Operation);
	auto& Frame = Operation->CompletionOnGT();
	Frame.Handler = MoveTemp(Ex.Handler);
	Frame.Finish = MoveTemp(Finish);
	Frame.DeliveryWorker = MoveTemp(Delivery);
	Frame.TerminalObserver = MoveTemp(Ex.ManagedTerminalObserver);
	if (Frame.TerminalObserver || Ex.QueueReplay)
		Frame.ReplayBody = Frame.Finish.Body; // raw entity BEFORE a typed bytes/JSON handler consumes it
	Frame.CompleteKernel = [this](FManagedCompletionRef Access) { FinishManagedCompletion(MoveTemp(Access)); };
	Frame.DiscardKernel = [this, Exchange] { DropManagedCompletion(Exchange); };
	if (Selected)
		Selected->BindFrameOnGT(Frame);
	else
		Selected = FManagedCompletionAccess::CreateOnGT(Frame, MoveTemp(Completion)); // retained primitive fixture/caller form
	const auto Access = Selected.ToSharedRef();
	const bool bHasBody = Frame.Finish.Body.Num() > 0 || Frame.Finish.bBodyInFile;
	const bool bDownload = !Frame.Finish.TempFile.IsEmpty() || (!Frame.Finish.SaveTo.IsEmpty() && !Frame.Finish.Error.IsError());
	const bool bEnvelopeCheck = Frame.Handler && Frame.Finish.Envelope.IsValid() && Frame.Finish.Envelope->HasPredicate() &&
								Frame.Handler->AppliesEnvelope() && !Frame.Finish.bBodyInFile;
	const bool bTask = bConsiderDecode && (bDownload ||
											  (Frame.Finish.Error.IsError() ? Frame.Finish.Error.Kind == EVaRestErrorKind::Http && bHasBody
																			: Frame.Handler && (Frame.Handler->DecodesBody() || bEnvelopeCheck) && bHasBody));
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	FNativeLogicalTailLease DecodePermission;
	if (bTask)
	{
		// A physical SaveTo already reserved this SAME finalizer's tail before Start. No extra acquisition at cap64.
		DecodePermission = Ex.FileCleanup.IsHeld() ? MoveTemp(Ex.FileCleanup) : Ref.Store->TryAddLogicalTailOnGT(Ref);
		if (bEnvelopeCheck && Ex.LogLevel != EVaRestLogLevel::None)
			Frame.Finish.LogRedactor = RedactorFor(Ex);
		if (Ex.ExcerptOnDecodeAttempt > 0 && Ex.ExcerptOnDecodeAttempt == Ex.Attempt &&
			!Frame.Finish.Error.IsError() && Frame.Finish.Body.Num() > 0 && DecodePermission.IsHeld())
		{
			Frame.Finish.ExcerptAttempt = Ex.ExcerptOnDecodeAttempt;
			Frame.Finish.ExcerptDispatchId = Ex.ExcerptOnDecodeDispatchId;
			Frame.Finish.ExcerptRedactor = RedactorFor(Ex);
			const auto ExcerptSettings = FVaRestTrace::GetDetailSettings();
			Frame.Finish.ExcerptCap = ExcerptSettings.BodyCap;
			Frame.Finish.ExcerptBinaryCap = ExcerptSettings.BinaryCap;
			Ex.ExcerptOnDecodeAttempt = 0;
			Ex.ExcerptOnDecodeDispatchId = 0;
		}
		Frame.Finish.bTimeDecode = FVaRestTrace::Detail() != EVaRestTraceDetail::None;
	}
	Ref.Store->CloseChildrenOnGT(Ref); // no new children after final selection; existing physical/native authorities survive
	Ex.Phase = EVaRestExchangePhase::Decoding;
	EndWaits(Ex); // actual hook can reenter Stop; all selected affine owners are ALREADY in Frame under Completion
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	if (bTask && DecodePermission.IsHeld())
	{
		auto* Actual = &Frame;
		TUniqueFunction<void()> Work = [Actual] {
			LLM_SCOPE_BYTAG(VaRest);
			PipelinePrivate::RunDecode(Actual->Handler.Get(), Actual->Finish, Internals(Actual->Exchange()));
#if !UE_BUILD_SHIPPING
			++Testing::Counters().DecodeTasksHandedBack; // GTK prerequisite was already published by the owning GTK caller
#endif
		};
		FManagedCompletionKernel After = [](FManagedCompletionRef Returned) {
			auto& Done = Returned->Payload();
			if (!Done.WorkerDidRun())
			{
				Done.Finish.Error.Kind = EVaRestErrorKind::Config;
				Done.Finish.Error.Message = TEXT("the managed decode task was discarded before execution");
			}
			Done.bDidDecode = true;
			Returned->CompleteOnGT();
		};
#if !UE_BUILD_SHIPPING
		++Testing::Counters().DecodeTasksLaunched;
#endif
		if (Access->StartWorkerOnGT(MoveTemp(DecodePermission), MoveTemp(Work), MoveTemp(After)))
			return;
		if (Control->IsStopped())
		{
			Access->DisposeOnGT(true);
			return;
		}
	}
	if (bTask)
	{
		Frame.Finish.Error.Kind = EVaRestErrorKind::Config;
		Frame.Finish.Error.Message = TEXT("the managed request has no available logical decode permission (LogicalCapacity)");
		// No GTK IO or unowned cleanup. A still-pending physical/file worker retains its actual cleanup permission.
	}
	else if (bConsiderDecode && Frame.Handler && !Frame.Finish.Error.IsError())
		Frame.Handler->Decode(Frame.Finish); // actual no-body/raw decode; only moves on GTK, as for ordinary requests
	Frame.bDidDecode = bConsiderDecode;
	Access->CompleteOnGT();
}

void FClientState::FinishRequestBookkeeping(const FVaRestExchangeRef& Exchange)
{
	check(IsInGameThread());
	auto& Ex = Internals(*Exchange);
	Ex.bFinished = true;
	Ex.Phase = EVaRestExchangePhase::Done;
	Ex.bDone = true;
	if (Ex.bQueueWhenOffline)
		Ex.QueueAdmission.store(EQueueAdmission::Finished, std::memory_order_release);
	if (const auto* Entry = InFlight.Find(Ex.RequestId); Entry && *Entry == Exchange)
		InFlight.Remove(Ex.RequestId);
	if (Ex.bCounted && !Ex.bStatsFinished)
	{
		Ex.bStatsFinished = true;
		FHttpStats::RequestFinished(Counters);
	}
}

void FClientState::DropManagedCompletion(const FVaRestExchangeRef& Exchange)
{
	// Stop has already sealed native work. Truthful native discard only: no normal terminal, trace, timer, service or callback.
	FinishRequestBookkeeping(Exchange);
}

void FClientState::FinishManagedCompletion(FManagedCompletionRef Access)
{
	check(IsInGameThread());
	const auto Control = Access->Control();
	auto& Frame = Access->Payload();
	auto& Ex = Internals(Frame.Exchange());
	if (Control->IsStopped() || IsShuttingDown())
	{
		Access->DisposeOnGT(true);
		return;
	}
	if (Frame.bFinalized || Frame.bFinalizingInternal)
		return;
	if (Frame.Handler && Frame.Handler->GetCompletionPolicy()==EResultCompletionPolicy::LaterFrame && !Frame.bLaterFrameReady)
	{
		if (Frame.bLaterFramePending) return;
		Frame.bLaterFramePending=true;
		TUniqueFunction<void()> Resume=[Access] {
			if (Access->Control()->IsStopped()) { Access->DisposeOnGT(true);return; }
			auto& Returned=Access->Payload();
			Returned.bLaterFrameReady=true;
			Returned.bLaterFramePending=false;
			Access->CompleteOnGT(); // SAME reserved Completion; the frame callback owns no newly admitted child
		};
		if (Frame.Handler->DeferCompletionOnGT(Resume)) return;
		Frame.bLaterFramePending=false;
		if (Control->IsStopped()) { Access->DisposeOnGT(true);return; }
		Frame.bLaterFrameReady=true;
		Frame.Finish.Error.Kind=EVaRestErrorKind::Config;
		Frame.Finish.Error.Code=TEXT("DecodeBudget");
		Frame.Finish.Error.Message=TEXT("the dynamic request has no available preadmitted frame completion");
	}
	const bool bOwnedSilentShutdown = Ex.bPersistenceOwned && Ex.CancelReason == EVaRestCancelReason::Shutdown;
	if (Ex.ApiDepth > 0 && !bOwnedSilentShutdown)
	{
		Ex.bCompletionQueued = true;
		Access->QueueCompleteOnGT(); // same initial reserved Completion, never an additional cap/counter/affine task capture
		return;
	}
	TGuardValue<bool> Finalizing(Frame.bFinalizingInternal, true);
	const uint64 PersistenceTicket = Ex.PersistenceCompletionTicket;
	Ex.PersistenceCompletionTicket = 0;
	ReleasePersistenceCompletion(PersistenceTicket);
	// Reflection/default/copy work belongs BEFORE accepting an internal result. A recursive completion has no authority
	// to replace this candidate; cancellation still records its normal atomic flag and interruption.
	if (Frame.Handler)
	{
		const bool bDynamic=Frame.Handler->GetCompletionPolicy()==EResultCompletionPolicy::LaterFrame;
		if (bDynamic && IsOwnerGone(Ex)) MarkOwnerGone(Ex);
		const bool bClosed=Ex.State && Ex.State->GetManagedLifetime().IsValid() && Ex.State->GetManagedLifetime().Gate->IsClosed();
		const bool bSkipDynamicValue=bDynamic && (Frame.Finish.bQueued || Ex.IsCancelRequested() ||
			!Ex.State || bClosed);
		if (bSkipDynamicValue) Frame.Handler->DiscardValue();
		else Frame.Handler->Finalize(Frame.Finish);
	}
	if (Ex.bCacheLookupPending)
	{
		Ex.bCacheLookupPending = false;
		Ex.CacheLookup = EVaRestTraceCacheLookup::None;
	}
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	Frame.Finish.Response.Elapsed = Ex.Config->Clock->Now() - Ex.StartedAt;
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	const FString FinalUrl = RedactorFor(Ex)->RedactUrl(Ex.Url.IsEmpty() ? JoinUrl(Ex.Config->BaseUrl, Ex.RawPath) : Ex.Url);
	if (Frame.Handler) Frame.Handler->ValidateTerminalOnGT(Frame.Finish);
	const auto SampleCancellation = [&Ex]() {
		if (IsOwnerGone(Ex))
			MarkOwnerGone(Ex);
		if (Ex.State->GetManagedLifetime().Gate->IsClosed())
			Ex.bCancelRequested.store(true, std::memory_order_relaxed);
		const bool bCanceled = Ex.IsCancelRequested();
		if (bCanceled && Ex.CancelReason == EVaRestCancelReason::None)
			Ex.CancelReason = EVaRestCancelReason::User;
		return bCanceled;
	};
	const bool bSelectedIo = !Frame.Finish.Response.SavedFile.IsEmpty() || Ex.bQueueCommitted ||
							 Ex.QueueAdmission.load(std::memory_order_acquire) == EQueueAdmission::Committing;
	bool bCanceled = SampleCancellation();
	bool bCleared = false;
	if (Frame.Handler && (Frame.Finish.Error.IsError() || (bCanceled && !bSelectedIo) ||
							 Ex.CancelReason == EVaRestCancelReason::OwnerGone || Ex.State->GetManagedLifetime().Gate->IsClosed()))
	{
		Frame.Handler->DiscardValue();
		bCleared = true;
	}
	bCanceled = SampleCancellation();
	if (!bCleared && Frame.Handler && bCanceled && !bSelectedIo)
	{
		Frame.Handler->DiscardValue(); // cleanup itself can reenter; no value is reconstructed afterward
		bCanceled = SampleCancellation();
	}
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	TOptional<FFinish> RejectedFinish;
	if (bCanceled && !bSelectedIo)
	{
		const double Elapsed = Frame.Finish.Response.Elapsed;
		FFinish Canceled = MakeFinish(Ex);
		Canceled.Error.Kind = EVaRestErrorKind::Canceled;
		Canceled.Error.Message = FVaRestError::DefaultMessage(EVaRestErrorKind::Canceled, 0);
		Canceled.Response.Elapsed = Elapsed;
		// Preserve all old Finish owners in a GTK local. Replacing metadata must not run a last custom-clock destructor
		// between the final sample and the acceptance marker; its eventual destruction still holds this method's tail.
		RejectedFinish.Emplace(MoveTemp(Frame.Finish));
		Frame.Finish = MoveTemp(Canceled);
	}
	Frame.Finish.Response.Attempts = Ex.Attempt;
	if (Frame.Finish.Error.IsError())
	{
		Frame.Finish.Error.Attempts = Ex.Attempt;
		Frame.Finish.Error.Tag = Ex.Tag;
		Frame.Finish.Error.Url = FinalUrl;
		if (Frame.Finish.Error.Message.IsEmpty())
			Frame.Finish.Error.Message = FVaRestError::DefaultMessage(Frame.Finish.Error.Kind, Frame.Finish.Error.HttpStatus);
	}
	const auto Exchange = Frame.Exchange().AsShared();
	// The last sample above is the acceptance linearization. Only native fields/owned value moves intervene here;
	// no reflected materialization, user destructor, clock, IO or delegate can freeze success before that sample.
	if (Ex.QueueReplay)
		Ex.QueueReplay->BeginTerminalAcceptanceOnGT();
	Frame.bFinalized = true;
	FinishRequestBookkeeping(Exchange);
	EndWaits(Ex); // acceptance is already marked; a reentrant detach cannot turn this result into retain-head cancellation
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	if (Ex.QueueReplay)
		Ex.QueueReplay->PublishTerminalOnGT(Frame.Finish, Frame.ReplayBody);
	// Accept the actual result (including silent/no-user cases) BEFORE Completed. The observer itself is a GTK local.
	auto Observer = MoveTemp(Frame.TerminalObserver);
	if (Observer)
		Observer(Frame.Finish, Frame.ReplayBody);
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	Access->Reference().Store->MarkTerminalOnGT(Access->Reference());
	TracePoints::Completed(Ex, Frame.Finish, Frame.Handler.Get());
	if (!Control->IsStopped() && Ex.QueueReplay)
		Ex.QueueReplay->CompletedTraceReturnedOnGT();
	Ex.UnionRedactor.Reset();
	Ex.UnionBuiltFrom.Reset();
	if (Control->IsStopped())
	{
		Access->DisposeOnGT(true);
		return;
	}
	const bool bSilent = Ex.CancelReason == EVaRestCancelReason::OwnerGone || Ex.CancelReason == EVaRestCancelReason::Shutdown ||
						 IsOwnerGone(Ex) || IsShuttingDown() || Ex.State->GetManagedLifetime().Gate->IsClosed();
	if (!Frame.Handler || bSilent)
	{
		Access->DisposeOnGT(false);
		return;
	}
	VAREST_LLM_SCOPE_SENDER(Ex.SenderTags);
	if (Frame.Finish.bQueued)
	{
		auto Queued = MoveTemp(Ex.OnQueued);
		if (Queued)
			Queued(Frame.Finish.QueueId);
		Access->DisposeOnGT(false);
		return;
	}
	if (Frame.Finish.Error.IsError() && PrimaryError.IsBound())
	{
		// A wrapper listener may close its Hub or Stop the module. Keep its argument independent of Frame disposal.
		const auto ObserverOwner=AsShared(); // the multicast itself survives a reentrant last native-client release
		const FVaRestError Error=Frame.Finish.Error;
		ObserverOwner->PrimaryError.Broadcast(Error);
		if (Control->IsStopped()) { Access->DisposeOnGT(true); return; }
		if (IsShuttingDown() || IsOwnerGone(Ex) || Ex.State->GetManagedLifetime().Gate->IsClosed())
		{
			Access->DisposeOnGT(false);
			return;
		}
	}
	if (Ex.bOnHttpThread && !Frame.Handler->RequiresGameThreadDelivery())
	{
		check(Frame.DeliveryWorker.IsHeld());
		TStrongObjectPtr<const UObject> Pin;
		if (Ex.bHasOwner)
			Pin.Reset(Ex.Owner.Get()); // GTK selects eligibility; worker never reads TWeakObjectPtr
		Frame.Finish.Clock.Reset();
		if (Control->IsStopped())
		{
			Access->DisposeOnGT(true);
			return;
		}
		auto* Actual = &Frame;
		TUniqueFunction<void()> Work = [Actual, Control, Tags = Ex.SenderTags, Kept = MoveTemp(Pin)]() mutable {
			if (!Control->IsStopped() && !FClientState::IsShuttingDown())
			{
				VAREST_LLM_SCOPE_SENDER(Tags);
				Actual->Handler->Deliver(Actual->Finish);
			}
			Kept.Reset(); // actual primary Engine API permits worker pin release; functor/handler/finish destruction stays GTK
		};
		FManagedCompletionKernel After = [](FManagedCompletionRef Returned) { Returned->DisposeOnGT(false); };
#if !UE_BUILD_SHIPPING
		++Testing::Counters().OffGameThreadDeliveries;
#endif
		if (Access->StartWorkerOnGT(MoveTemp(Frame.DeliveryWorker), MoveTemp(Work), MoveTemp(After)))
			return;
		Access->DisposeOnGT(Control->IsStopped());
		return;
	}
	Frame.Handler->Deliver(Frame.Finish);
	Access->DisposeOnGT(false); // actual handler/captures/finish die GTK, BEFORE Completion's final authority settles
}
} // namespace VaRest::Http
