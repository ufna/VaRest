// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestQueueReplay.h"
#include "VaRestOfflineInternal.h"

namespace VaRest::Http
{
FQueueReplayOperation::FQueueReplayOperation(TSharedRef<FOfflineQueue, ESPMode::ThreadSafe> InQueue, uint64 InId,
	FQueueMemberKey InMember, const FQueueItem& Item, bool bInProbe, FName InTerminalDetail, FVaRestProbeToken Token)
	: Queue(MoveTemp(InQueue))
	, Member(InMember)
	, ReplayId(InId)
	, QueueId(Item.QueueId)
	, Sequence(Item.Sequence)
	, Tag(Item.Tag.IsEmpty() ? NAME_None : FName(*Item.Tag))
	, TerminalDetail(InTerminalDetail)
	, bProbe(bInProbe)
	, ProbeToken(Token)
{
	check(IsInGameThread() && InId != 0 && Member.Epoch != 0);
}

void FQueueReplayOperation::BindManagedBeforePublication(const FNativeOperationRef& Ref, const FVaRestRequestHandle& Handle)
{
	check(IsInGameThread() && Ref.IsValid() && !Managed.IsValid() && !bManagedSettled && Phase == EPhase::Request);
	Managed = Ref;
	Request = Handle; // weak exchange identity, installed before any synchronous Started/cancellation callout
}

void FQueueReplayOperation::BeginTerminalAcceptanceOnGT()
{
	check(IsInGameThread() && !bAcceptanceStarted && !bCoreTerminal);
	bAcceptanceStarted = true; // no IO, user callback or mutable reflected materialization
}

void FQueueReplayOperation::PublishTerminalOnGT(const FFinish& Finish, const TArray<uint8>& RawBody)
{
	check(IsInGameThread() && bAcceptanceStarted && !bCoreTerminal);
	FVaRestQueuedResult Published;
	Published.QueueId = QueueId;
	Published.Tag = Tag;
	Published.bSuccess = !Finish.Error.IsError();
	Published.Response = Finish.Response;
	Published.Error = Finish.Error;
	if (!Finish.Error.IsError())
		Published.Response.Body = RawBody;
	Result = MoveTemp(Published);
	Disposition = Phase == EPhase::CancelRetain ? EDisposition::RetainDetach : ((Finish.Error.Kind == EVaRestErrorKind::Connection || Finish.Error.Kind == EVaRestErrorKind::Offline) && TerminalDetail != TEXT("QueueExpired") ? EDisposition::RetainNetwork : EDisposition::Retire);
	Phase = EPhase::TerminalPublished;
	bCoreTerminal = true;
}

void FQueueReplayOperation::CompletedTraceReturnedOnGT()
{
	check(IsInGameThread() && bCoreTerminal);
	bTraceReturned = true;
	if (Queue)
		Queue->AdvanceManagedReplay(AsShared());
}

void FQueueReplayOperation::ManagedAnchorSettledOnGT(const FNativeOperationRef& Ref, bool bDiscarded)
{
	check(IsInGameThread() && Managed.IsValid() && Managed.Control == Ref.Control && Managed.Store == Ref.Store &&
		  Managed.Cell.Index == Ref.Cell.Index && Managed.Cell.Generation == Ref.Cell.Generation);
	const auto Self = AsShared();
	bManagedSettled = true;
	// Consume the settlement identity INSIDE its actual Reclaiming anchor, before that cell can become Free/reused.
	// Later queue/IO progress uses this sealed witness, never a stale cell pointer or a work-release-claim as a fence.
	Managed = {};
	Request = {};
	if (!Queue)
		return;
	const auto Kept = Queue;
	if (bDiscarded)
		Kept->DiscardManagedReplay(Self);
	else
		Kept->AdvanceManagedReplay(Self);
}

void FQueueReplayOperation::AdmissionRefusedOnGT()
{
	check(IsInGameThread() && !Managed.IsValid() && !bCoreTerminal);
	bAdmissionRefused = true; // no exchange/terminal/trace or physical obligation was ever admitted
	bManagedSettled = true;
	Disposition = EDisposition::RetainDetach;
	if (Queue)
		Queue->AdvanceManagedReplay(AsShared());
}

void FQueueReplayOperation::DetachOwnerOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	bOwnerDetached = true;
	if (bAcceptanceStarted || Phase == EPhase::RetireIo || Phase == EPhase::Settled)
		return;                   // an accepting/published result or accepted tombstone is never relabeled by later departure
	Phase = EPhase::CancelRetain; // retain-head BEFORE the actual public handle can synchronously complete
	Disposition = EDisposition::RetainDetach;
	CompleteProbeOnGT(); // only this operation's token, even when the registered candidate was a different client
	Request.Cancel();
}
void FQueueReplayOperation::CompleteProbeOnGT()
{
	check(IsInGameThread());
	const auto Self = AsShared();
	if (!bProbe || bProbeCompleted || !Queue)
		return;
	bProbeCompleted = true;
	const auto Kept = Queue;
	if (const auto Reachability = Kept->Connectivity.Pin())
	{
		if (ProbeToken.IsValid())
			Reachability->CompleteOwnedProbe(ProbeToken);
		else
			Reachability->ProbeFinished();
	}
}
} // namespace VaRest::Http
