// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"
#include "VaRestOfflineInternal.h"

namespace VaRest::Http
{
bool FClientState::ActivateManagedQueue(FString& OutError)
{
	check(IsInGameThread());
	const auto Self = AsShared();
	OutError.Reset();
	if (!ManagedLifetime.IsValid() || ManagedAdmissionIsClosed())
	{
		OutError = TEXT("LifecycleClosed");
		return false;
	}
	const auto Current = GetConfig();
	if (Current->QueueName.IsEmpty())
	{
		OutError = TEXT("QueueAccountRequired");
		return false;
	}
	if (Current->bQueueActivationRequested && (Current->ManagedQueueStatus == EManagedQueueStatus::Active ||
												  Current->ManagedQueueStatus == EManagedQueueStatus::PendingBusy))
	{
		OutError = Current->QueueConfigError;
		return true; // same pending intent, no second reservation or target history
	}
	if (!TrySwitchQueue(FClientConfig(*Current), OutError, true))
		return false;
	// The target is already completely published. Normal activation can join now; QueueBusy retains that one intent.
	if (!ManagedAdmissionIsClosed())
		RefreshQueueRegistration();
	const auto Selected = GetConfig();
	OutError = Selected->QueueConfigError;
	return Selected->ManagedQueueStatus == EManagedQueueStatus::Active || Selected->ManagedQueueStatus == EManagedQueueStatus::PendingBusy;
}

bool FClientState::PauseManagedQueue(FString& OutError)
{
	check(IsInGameThread());
	const auto Self = AsShared();
	OutError.Reset();
	if (!ManagedLifetime.IsValid() || ManagedAdmissionIsClosed())
	{
		OutError = TEXT("LifecycleClosed");
		return false;
	}
	if (QueueRefreshRevision == MAX_uint64 || bQueueRegistrationActive)
	{
		OutError = TEXT("LifecycleBusy");
		return false;
	}
	const auto Previous = GetConfig();
	if (!Previous->bQueueActivationRequested && !Previous->RegisteredQueue)
		return true;
	auto Next = MakeShared<FClientConfig, ESPMode::ThreadSafe>(*Previous);
	Next->bQueueActivationRequested = false;
	Next->ManagedQueueStatus = Next->QueueName.IsEmpty() ? EManagedQueueStatus::AccountRequired : EManagedQueueStatus::Paused;
	Next->QueueConfigError = Next->QueueName.IsEmpty() ? TEXT("QueueAccountRequired") : TEXT("QueueNotActivated");
	Next->RegisteredQueue.Reset();
	Next->QueueBinding.Reset();
	const auto PreviousQueue = Queue;
	const auto PreviousBinding = QueueBinding;
	if (!Publish(Next, Previous, true))
	{
		OutError = ManagedAdmissionIsClosed() ? TEXT("LifecycleClosed") : TEXT("LifecycleBusy");
		return false;
	}
	++QueueRefreshRevision;
	Queue.Reset();
	QueueBinding.Reset();
	ManagedLifetime.Control->ClientSlotsOnGT().DiscardPendingQueueTarget(ManagedLifetime);
	if (PreviousQueue && !ManagedLifetime.Control->IsStopped())
		ManagedLifetime.Control->ClientSlotsOnGT().DetachQueueBinding(ManagedLifetime, PreviousBinding, true);
	return true; // existing active row becomes its drain; no third row is required just to revoke/pause B
}

int64 FClientState::ManagedQueuedCount(bool& bLoaded) const
{
	check(IsInGameThread());
	bLoaded = false;
	if (!ManagedLifetime.IsValid() || ManagedAdmissionIsClosed())
		return 0;
	const auto Current = GetConfig();
	if (Current->ManagedQueueStatus != EManagedQueueStatus::Active || !Current->RegisteredQueue ||
		!Current->QueueBinding || !Current->QueueBinding->AllowsEnqueue() || !Current->RegisteredQueue->IsLoadedOnGT())
		return 0;
	bLoaded = true;
	return static_cast<int64>(Current->RegisteredQueue->Count());
}
} // namespace VaRest::Http
