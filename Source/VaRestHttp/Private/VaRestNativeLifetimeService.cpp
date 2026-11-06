// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeLifetimeService.h"
#include "VaRestNativeClientSlots.h"

namespace VaRest::Http
{
TUniquePtr<FNativeLifetimeService> FNativeLifetimeService::CreateOnGT(FTSTicker& Ticker)
{
	check(IsInGameThread());
	TUniquePtr<FNativeLifetimeService> Service(new FNativeLifetimeService());
	Service->Control = FNativeLifetimeControl::Create();
	TWeakPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Weak = Service->Control;
	Service->TickHandle = Ticker.AddTicker(TEXT("VaRest.NativeLifetime"), 0.0f,
		[Weak](float) {
			check(IsInGameThread());
			auto Kept = Weak.Pin();
			if (!Kept || Kept->IsStopped())
				return false;
			Kept->ClientSlotsOnGT().RetireClosingClientsOnGT();
			if (!Kept->IsStopped()) Kept->ClientSlotsOnGT().SweepOperations(256);
			if (!Kept->IsStopped())
				Kept->ClientSlotsOnGT().SweepQueueDrains();
			// Destruction of a ready anchor may reenter module Stop. Only the captured Control is used afterward.
			if (!Kept->IsStopped())
				Kept->CollectInputsOnGT(256);
			return !Kept->IsStopped();
		});
	if (!Service->TickHandle.IsValid())
	{
		Service->StopOnGT(); // nothing has been published or admitted yet
		return nullptr;
	}
	return Service;
}

FNativeLifetimeService::~FNativeLifetimeService()
{
	StopOnGT();
}

TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> FNativeLifetimeService::ControlForNewClientOnGT() const
{
	check(IsInGameThread());
	if (bStopping || !Control || Control->IsStopped() || Control->ClientSlotsOnGT().IsStopping())
		return nullptr;
	return Control;
}

void FNativeLifetimeService::BeginStoppingOnGT()
{
	check(IsInGameThread());
	if (bStopping)
		return;
	bStopping = true;
	if (Control)
		Control->ClientSlotsOnGT().BeginStop();
}

void FNativeLifetimeService::StopOnGT()
{
	check(IsInGameThread());
	BeginStoppingOnGT();
	const bool bGraceful = bPreExitQueuesDetached;
	TSharedPtr<FNativeLifetimeControl, ESPMode::ThreadSafe> Kept = MoveTemp(Control);
	FTSTicker::FDelegateHandle Removing = MoveTemp(TickHandle);
	TickHandle.Reset();
	// Cleanup can destroy the service's owning unique pointer. No member/this access follows that callout.
	if (Kept && !Kept->IsStopped())
	{
		if (bGraceful)
			Kept->ClientSlotsOnGT().SweepQueueDrains(); // no new IO while stopping; actual completed/failed witnesses only
		if (!Kept->IsStopped())
			Kept->StopInputsOnGT(); // unresolved slots/inputs retain this same Control
	}
	FTSTicker::RemoveTicker(Removing);
	// No wait for arbitrary transport, general GT drain, replacement cleanup task or late service lookup.
}
void FNativeLifetimeService::DetachQueuesForPreExitOnGT()
{
	check(IsInGameThread() && bStopping);
	bPreExitQueuesDetached = true;
	const auto Kept = Control;
	if (Kept && !Kept->IsStopped())
	{
		Kept->ClientSlotsOnGT().RetireClosingClientsOnGT();
		if (!Kept->IsStopped()) Kept->ClientSlotsOnGT().DetachAllQueuesForStop();
	}
	// No service/this access after a real queue/clock/cancel callout.
}
void FNativeLifetimeService::FlushQuiescentQueuesForPreExitOnGT()
{
	check(IsInGameThread() && bStopping && bPreExitQueuesDetached);
	const auto Kept = Control;
	if (Kept && !Kept->IsStopped())
		Kept->ClientSlotsOnGT().SweepQueueDrains(true);
}
} // namespace VaRest::Http
