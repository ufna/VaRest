// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientState.h"
#include "VaRestNativeLifetimeService.h"
#include "VaRestNativeScope.h"

FVaRestClient VaRest::Http::FClientAccess::CreateManaged(FClientConfig&& Initial,
	TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope, FName& Error,TFunction<void(const FVaRestClient&)> BeforeCreated)
{
	check(IsInGameThread());
	auto Control = NativeControlForNewClientOnGT();
	if (!Control)
	{
		Error = TEXT("LifecycleClosed");
		return {};
	}
	return CreateManagedInControl(MoveTemp(Initial), MoveTemp(Scope), Control.ToSharedRef(), Error,MoveTemp(BeforeCreated));
}

FVaRestClient VaRest::Http::FClientAccess::CreateManagedInControl(FClientConfig&& Initial,
	TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope,
	TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control, FName& Error,TFunction<void(const FVaRestClient&)> BeforeCreated)
{
	check(IsInGameThread());
	FNativeClientReservation Reservation;
	if (!Control->ClientSlotsOnGT().TryReserve(Scope, Reservation, Error))
		return {};
	Initial.ScopeId = Scope->GetId();
	Initial.ScopeLifetime = Scope;
	// Immutable gate, Control and generation exist before derived config, registry, probes or Created call out.
	auto State = FClientState::Create(MoveTemp(Initial), Reservation.InitialRegistration(), false);
	if (!Reservation.CommitClient(State))
	{
		Error = TEXT("LifecycleClosed");
		return {}; // the exclusive native reservation still owns complete construction unwind
	}
	const FVaRestClient Client=Wrap(State);
	if (BeforeCreated) BeforeCreated(Client); // internal GTK facade binding, after complete native ownership, before trace publication
	if (!State->PublishCreatedOnGT())
	{
		Error = TEXT("LifecycleClosed");
		return {};
	}
	return Client;
}
