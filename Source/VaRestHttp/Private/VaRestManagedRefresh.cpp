// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuthInternal.h"
#include "VaRestClientState.h"
#include "VaRestManagedRefreshDelivery.h"

namespace VaRest::Http
{
void FClientState::RefreshManagedExternal(FRefreshWaiter&& OnDone)
{
	check(IsInGameThread() && ManagedLifetime.IsValid());
	const auto Self = AsShared();
	const auto Control = ManagedLifetime.Control;
	if (ManagedAdmissionIsClosed())
		return;
	auto* Delivery = Control->ClientSlotsOnGT().RefreshDeliveryOnGT(ManagedLifetime);
	if (!Delivery)
		return;
	if (!Delivery->TryReserveOnGT(OnDone))
	{
		if (!ManagedAdmissionIsClosed())
			Delivery->RefuseLaterOnGT(MoveTemp(OnDone));
		return;
	}
	// OnDone is now a native ticket. The real external captures remain in this exact Control-owned fixed position.
	if (ManagedAdmissionIsClosed())
		return;
	const uint64 WaiterId = NewWaiterId();
	if (WaiterId == 0)
	{
		FRefreshEnded End;
		End.End = ERefreshEnd::Failed;
		End.Kind = EVaRestErrorKind::Config;
		End.Message = TEXT("managed refresh waiter generation exhausted");
		OnDone(End);
		return;
	}
	if (IsRefreshing())
	{
		JoinRefreshInFlight(WaiterId, MoveTemp(OnDone), ERefreshWaiterKind::External);
		return;
	}
	const auto Auth = GetConfig()->Auth;
	const bool bAvailable = Auth && CanRefreshNow(*Auth, GetSessionData());
	if (ManagedAdmissionIsClosed())
		return; // CanRefresh/config checks are actual provider callouts
	if (!bAvailable)
	{
		const bool bMisconfigured = Auth && !Auth->GetConfigError().IsEmpty();
		if (ManagedAdmissionIsClosed())
			return;
		FRefreshEnded End;
		End.End = bMisconfigured ? ERefreshEnd::Failed : ERefreshEnd::Unavailable;
		End.Kind = bMisconfigured ? EVaRestErrorKind::Config : EVaRestErrorKind::None;
		OnDone(End); // marks the reserved position Ready; actual user delivery is a later native-only named-GT wake
		return;
	}
	JoinRefresh(WaiterId, Auth.ToSharedRef(), 0, false, MoveTemp(OnDone), ERefreshWaiterKind::External);
}
} // namespace VaRest::Http
