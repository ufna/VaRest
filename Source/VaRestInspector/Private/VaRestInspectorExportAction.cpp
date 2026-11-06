// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestInspectorExportAction.h"
#include "VaRestInspector.h"
#include "VaRestInspectorContext.h"
#include "VaRestHub.h"
#include "Containers/Ticker.h"
#include "UObject/StrongObjectPtr.h"

namespace
{
	TArray<TWeakObjectPtr<UVaRestInspectorExportAction>> ActiveActions;
}
UVaRestInspectorExportAction* UVaRestInspectorExportAction::ExportInspectorHAR(UObject* Owner, const FString& Path, bool All)
{
	check(IsInGameThread()); auto* Action = NewObject<UVaRestInspectorExportAction>();
	Action->Context = Owner; Action->Destination = Path; Action->bAll = All; return Action;
}
void UVaRestInspectorExportAction::Activate()
{
	check(IsInGameThread()); if (bActivated || bFinished) return; bActivated = true;
	const TStrongObjectPtr<UVaRestInspectorExportAction> Self(this);
#if VAREST_WITH_INSPECTOR
	if (!Context.IsValid()) { Finish(false, TEXT("A live world context is required.")); return; }
	auto* Inspector = IVaRestInspector::TryGet(); if (!Inspector) { Finish(false, TEXT("Inspector is stopped.")); return; }
	auto* OwnerHub = VaRest::Inspector::ExistingHub(Context.Get());
	if (!OwnerHub) { Finish(false, TEXT("The context has no active VaRest Hub.")); return; }
	ActiveActions.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
	if (ActiveActions.Num() >= 16) { Finish(false, TEXT("Inspector export action capacity is exhausted.")); return; }
	Hub = OwnerHub;
	Registration = OwnerHub->RetainInFlight(this, [Weak = TWeakObjectPtr<UVaRestInspectorExportAction>(this)] { if (auto* Action = Weak.Get()) Action->Retire(); });
	if (!Registration) { Finish(false, TEXT("The VaRest Hub is closing or refused the action.")); return; }
	RegisterWithGameInstance(Context.Get()); ActiveActions.Add(this);
	FVaRestInspectorExportOptions Options; if (!bAll) Options.ScopeId = OwnerHub->GetScopeId();
	Task = Inspector->ExportHar(Destination.IsEmpty() ? VaRest::Inspector::DefaultHarPath() : Destination, Options);
	if (bFinished) { Task = {}; return; }
	if (Poll(0)) Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &UVaRestInspectorExportAction::Poll), .05f);
#else
	Finish(false, TEXT("Inspector capture/export is unavailable in this build."));
#endif
}
bool UVaRestInspectorExportAction::Poll(float)
{
	check(IsInGameThread()); if (bFinished) return false;
	if (!Context.IsValid() || !Hub.IsValid()) { Retire(); return false; }
	if (!Task.IsValid()) { Finish(false, TEXT("The export job is unavailable.")); return false; }
	if (!Task.IsCompleted()) return true;
	const auto& Result = Task.GetResult();
	const bool Success = Result.HasValue(); const FString Message = Success ? Result.GetValue() : Result.HasError() ? Result.GetError() : FString(TEXT("Export completed without a result."));
	Finish(Success, Message); return false;
}
void UVaRestInspectorExportAction::Finish(bool Success, FString Message)
{
	if (bFinished) return; const TStrongObjectPtr<UVaRestInspectorExportAction> Self(this);
	Retire(); // release scope/owner registrations before any Blueprint continuation can reenter shutdown
	if (Success) OnDone.Broadcast(Message); else OnFailed.Broadcast(Message);
}
void UVaRestInspectorExportAction::Retire()
{
	check(IsInGameThread()); if (bFinished) return; bFinished = true;
	if (Ticker.IsValid()) FTSTicker::GetCoreTicker().RemoveTicker(Ticker); Ticker.Reset();
	Task = {}; const auto Owner = Hub; Hub.Reset(); const uint64 Token = Registration; Registration = 0;
	if (auto* Value = Owner.Get(); Value && Token) Value->ReleaseInFlight(Token);
	Context.Reset(); Destination.Reset(); ActiveActions.RemoveAll([this](const auto& Weak) { return !Weak.IsValid() || Weak.Get() == this; }); SetReadyToDestroy();
}
void UVaRestInspectorExportAction::BeginDestroy() { Retire(); Super::BeginDestroy(); }
void UVaRestInspectorExportAction::ShutdownAllOnGT()
{
	check(IsInGameThread()); const auto Copy = MoveTemp(ActiveActions); ActiveActions.Reset();
	for (const auto& Weak : Copy) if (auto* Action = Weak.Get()) Action->Retire();
}
