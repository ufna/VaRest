// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"
#include "VaRestManagedRefreshDelivery.h"
#include "VaRestOfflineInternal.h"

namespace VaRest::Http
{
void FClientState::BeginPermanentRetirement()
{
	check(IsInGameThread());
	if (!ManagedLifetime.IsValid())
		return;
	const auto Self = AsShared();
	const auto Control = ManagedLifetime.Control;
	// Hub closes all of its gates before OnDeinitializing. This native backstop is also idempotent and has no callout.
	Control->ClientSlotsOnGT().BeginRetirement(ManagedLifetime);
	if (bPermanentRetirementStarted || Control->IsStopped())
		return;
	bPermanentRetirementStarted = true;
	if (QueueRefreshRevision != MAX_uint64)
		++QueueRefreshRevision;
	const auto Current = GetConfig();
	DetachQueueBindingOnGT();
	if (Control->IsStopped())
		return;
	Control->ClientSlotsOnGT().DetachQueuesForRetirement(ManagedLifetime); // includes an in-progress switch/paused target
	if (Control->IsStopped())
		return;
	Queue.Reset();
	QueueBinding.Reset(); // actual native rows keep the detached snapshots through their IO fences
	Current->Connectivity->UnregisterProbe(ClientId);
	if (Control->IsStopped())
		return;
	CancelAll(NAME_None, EVaRestCancelReason::OwnerGone); // actual interrupts remove parked RequestGate entries first
	if (auto* Delivery = Control->ClientSlotsOnGT().RefreshDeliveryOnGT(ManagedLifetime))
		Delivery->RetireOnGT();
	if (Control->IsStopped())
		return;
	FVaRestTimerHandle Removing = SecretExpiryTimer;
	SecretExpiryTimer.Invalidate();
	if (Removing.IsValid())
		Current->Clock->ClearTimer(Removing);
	if (Control->IsStopped())
		return;
	// Clear ownership before destruction can reenter. Late Done observes a closed gate and a missing refresh id.
	auto AbandonedRefresh = MoveTemp(Session.Refresh);
	Session.Pending.Reset();
	Session.Data.AccessToken.Reset();
	Session.Data.RefreshToken.Reset();
	Session.Data.Headers = FVaRestHeaders();
	Session.Data.ExpiresAt = Session.Data.Lifetime = 0.0;
	if (Session.Data.Generation != MAX_uint32)
		++Session.Data.Generation;
	if (Session.Data.CredentialGeneration != MAX_uint32)
		++Session.Data.CredentialGeneration;
	Secrets.ClearAll();
	AuthSlots.Reset();
	Session.Changed.Clear();
	Session.Expired.Clear();
	QueuedResult.Clear();
	AbandonedRefresh.Reset(); // arbitrary external captures die here on GT, never inside a worker's late Done
	if (!Control->IsStopped()) bPermanentRetirementActionsComplete=true;
}
void FClientState::TryFinishPermanentRetirementOnGT()
{
	check(IsInGameThread());
	const auto Self=AsShared();
	if (!ManagedLifetime.IsValid() || ManagedLifetime.Control->IsStopped() || !bPermanentRetirementActionsComplete ||
		bQueueRegistrationActive || Queue || QueueBinding || Session.Refresh || !Session.Pending.IsEmpty() ||
		Session.ChangeDepth!=0 || Session.bDelivering || SecretExpiryTimer.IsValid() || PendingApiCompletionWrappers!=0 || !InFlight.IsEmpty())
		return;
	// UnregisterProbe/actual cancellation and capture destruction have returned. This records only this owner's
	// non-operation barrier: the slot still independently requires real cell/input/auth/queue-fence quiescence.
	ManagedLifetime.Control->ClientSlotsOnGT().TryReleaseQuiescent(ManagedLifetime);
}
} // namespace VaRest::Http
