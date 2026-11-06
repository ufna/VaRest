// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestNativeScope.h"
#include "HAL/PlatformTime.h"
#include "VaRestNativeClientSlots.h"
#include "VaRestNativeLifetime.h"
#include "VaRestTraceSnapshot.h"

namespace VaRest::Http::NativeScopePrivate
{
TMap<uint64, TWeakPtr<FNativeScopeLifetime, ESPMode::ThreadSafe>>& Registry()
{
	// No strong host/client/UObject ownership, and no static destruction ordering dependency.
	static auto* Scopes = new TMap<uint64, TWeakPtr<FNativeScopeLifetime, ESPMode::ThreadSafe>>();
	return *Scopes;
}
void RemoveExpired()
{
	for (auto It = Registry().CreateIterator(); It; ++It)
		if (!It.Value().IsValid())
			It.RemoveCurrent();
}
} // namespace VaRest::Http::NativeScopePrivate

TSharedPtr<VaRest::Http::FNativeScopeLifetime, ESPMode::ThreadSafe> VaRest::Http::FNativeScopeLifetime::Open(FVaRestTraceScope Info, double Time)
{
	check(IsInGameThread());
	Info.ScopeId = FVaRestTrace::NewScopeId();
	if (Info.ScopeId == 0)
		return nullptr;
	Info.bOpened = true;
	NativeScopePrivate::RemoveExpired();
	TSharedRef<FNativeScopeLifetime, ESPMode::ThreadSafe> Scope = MakeShareable(new FNativeScopeLifetime(MoveTemp(Info)));
	NativeScopePrivate::Registry().Add(Scope->GetId(), Scope);
	if (FVaRestTrace::Detail() != EVaRestTraceDetail::None)
		FVaRestTrace::Emit(EVaRestTraceKind::Scope, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceScope>(), Scope->Info));
	return Scope;
}

bool VaRest::Http::FNativeScopeLifetime::SetLabel(FStringView Label)
{
	check(IsInGameThread());
	if (!AcceptsClientsOnGT())
		return false;
	Info.Label = FString(Label);
	return true;
}

void VaRest::Http::FNativeScopeLifetime::Close(double Time)
{
	check(IsInGameThread());
	if (!Info.bOpened)
		return;
	if (OwnedClients != 0 || SnapshotPins != 0)
	{
		bRetirementRequested = true;
		return;
	}                     // never emit Closed around an admitted native owner
	const auto Control = NativeControl.Pin();
	if (bHasControl && (!Control || Control->IsStopped()))
		return; // stopped cleanup never fabricates a normal Closed event
	Info.bOpened = false; // label/weak host metadata stays frozen while an inert client shell retains this record
	if (FVaRestTrace::Detail() != EVaRestTraceDetail::None)
		FVaRestTrace::Emit(EVaRestTraceKind::Scope, Time, FVaRestTracePayload(TInPlaceType<FVaRestTraceScope>(), Info));
}

void VaRest::Http::FNativeScopeLifetime::RequestRetirementOnGT()
{
	check(IsInGameThread());
	bRetirementRequested = true;
	if (OwnedClients == 0)
		Close(FPlatformTime::Seconds());
}
bool VaRest::Http::FNativeScopeLifetime::TryAddClientOnGT(TSharedRef<FNativeLifetimeControl, ESPMode::ThreadSafe> Control)
{
	check(IsInGameThread());
	if (!AcceptsClientsOnGT() || OwnedClients >= FNativeClientSlots::MaxPerScope ||
		(bHasControl && NativeControl.Pin() != Control))
		return false;
	NativeControl = Control;
	bHasControl = true;
	++OwnedClients;
	return true;
}

void VaRest::Http::FNativeScopeLifetime::AddSnapshotPinOnGT()
{
	check(IsInGameThread() && SnapshotPins != MAX_uint64);
	++SnapshotPins;
}
void VaRest::Http::FNativeScopeLifetime::ReleaseSnapshotPinOnGT()
{
	check(IsInGameThread() && SnapshotPins != 0);
	--SnapshotPins;
	if (SnapshotPins == 0 && OwnedClients == 0 && bRetirementRequested)
		Close(FPlatformTime::Seconds());
}
void VaRest::Http::FNativeScopeLifetime::ReleaseClientOnGT(bool bAllowCloseTrace)
{
	check(IsInGameThread() && OwnedClients != 0);
	--OwnedClients;
	if (bAllowCloseTrace && OwnedClients == 0 && bRetirementRequested)
		Close(FPlatformTime::Seconds());
}

void VaRest::Http::FNativeScopeLifetime::ContributeSnapshot(FVaRestTraceSnapshot& Snapshot)
{
	check(IsInGameThread());
	NativeScopePrivate::RemoveExpired();
	TArray<uint64> Ids;
	NativeScopePrivate::Registry().GetKeys(Ids);
	Ids.Sort();
	for (uint64 Id : Ids)
		if (const auto Scope = NativeScopePrivate::Registry().FindChecked(Id).Pin())
		{
			TracePrivate::HoldSnapshotScopeOnGT(Scope.ToSharedRef());
			Snapshot.Scopes.Add(Scope->Info);
		}
}
