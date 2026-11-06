// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestFlushQueueAction.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestDynamicBridge.h"
#include "VaRestClientState.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/ScopeExit.h"
#include "UObject/StrongObjectPtr.h"
#include <atomic>

namespace VaRest::Http
{
enum class EFlushDelivery : uint8 { Suppressed,Flushed,Failed,Refused };
struct FFlushNativeResult
{
	std::atomic<EFlushDelivery> Outcome{EFlushDelivery::Suppressed};
};
/** Original native callback owns this plain guard. A last release never touches an action or posts a new GT task. */
struct FFlushDeliveryGuard
{
	FFlushDeliveryGuard(TSharedRef<FFlushNativeResult,ESPMode::ThreadSafe> InResult,TSharedRef<FDynamicCompletion,ESPMode::ThreadSafe> InCompletion)
		: Result(MoveTemp(InResult)),Completion(MoveTemp(InCompletion)) {}
	FFlushDeliveryGuard(const FFlushDeliveryGuard&)=delete;
	TSharedRef<FFlushNativeResult,ESPMode::ThreadSafe> Result;
	TSharedRef<FDynamicCompletion,ESPMode::ThreadSafe> Completion;
	~FFlushDeliveryGuard() { Completion->NotifyReadyFromWorker(); }
};
struct FFlushActionState
{
	~FFlushActionState() { check(IsInGameThread()); }
	FVaRestClient Native;
	TWeakObjectPtr<const UObject> Owner;
	TWeakObjectPtr<UVaRestApiClient> Client;
	TWeakObjectPtr<UVaRestHub> Hub;
	TSharedPtr<FDynamicCompletion,ESPMode::ThreadSafe> Completion;
	TSharedRef<FFlushNativeResult,ESPMode::ThreadSafe> Result=MakeShared<FFlushNativeResult,ESPMode::ThreadSafe>();
	EVaRestFlushQueueOutcome PublishedResult=EVaRestFlushQueueOutcome::Refused;
	bool bHasOwner=false,bActivated=false,bAdmitted=false,bTerminal=false,bPublished=false,bReleased=false;
};
}

UVaRestFlushQueueAction::UVaRestFlushQueueAction(const FObjectInitializer& Initializer) : Super(Initializer) {}
UVaRestFlushQueueAction::UVaRestFlushQueueAction(FVTableHelper& Helper) : Super(Helper) {}
UVaRestFlushQueueAction::~UVaRestFlushQueueAction() = default;
UVaRestFlushQueueAction* UVaRestFlushQueueAction::FlushQueue(const UObject* WorldContextObject,UVaRestApiClient* Client,FString& Error)
{
	check(IsInGameThread()); using namespace VaRest::Http; Error.Reset();
	if (FClientState::IsShuttingDown()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	if (!Client) { auto* Hub=UVaRestHub::Get(WorldContextObject); Client=Hub ? Hub->GetDefaultClient() : nullptr; }
	const TStrongObjectPtr<UVaRestApiClient> KeptClient(Client);
	auto* Hub=IsValid(Client) && !Client->bDetached ? Client->Hub.Get() : nullptr;
	const auto NativeState=Client ? FClientAccess::State(Client->Native) : nullptr;
	if (!Hub || !NativeState || NativeState->ManagedAdmissionIsClosed()) { Error=TEXT("LifecycleClosed"); return nullptr; }
	const auto Completion=FDynamicCompletion::ReserveOnGT();
	if (!Completion) { Error=TEXT("DynamicCapacity"); return nullptr; }
	const TStrongObjectPtr<UVaRestHub> KeptHub(Hub);
	auto* Action=NewObject<UVaRestFlushQueueAction>();
	const TStrongObjectPtr<UVaRestFlushQueueAction> KeptAction(Action);
	Action->State=MakeUnique<FFlushActionState>();
	auto& State=*Action->State; State.Native=Client->Native; State.Client=Client; State.Hub=Hub;
	State.Owner=WorldContextObject; State.bHasOwner=WorldContextObject!=nullptr; State.Completion=Completion;
	if (Completion->IsStopped() || NativeState->ManagedAdmissionIsClosed()) { Action->ReleaseOnGT(); Error=TEXT("LifecycleClosed"); return nullptr; }
	Hub->OnDeinitializing.AddDynamic(Action,&UVaRestFlushQueueAction::HubClosing); return Action;
}
bool UVaRestFlushQueueAction::CanDeliverOnGT() const
{
	check(IsInGameThread()); using namespace VaRest::Http;
	if (!State || State->bReleased || FClientState::IsShuttingDown() || (State->bHasOwner && !State->Owner.IsValid())) return false;
	const auto* Client=State->Client.Get(); const auto Native=FClientAccess::State(State->Native);
	return IsValid(Client) && !Client->bDetached && State->Hub.IsValid() && Native && !Native->ManagedAdmissionIsClosed();
}
void UVaRestFlushQueueAction::Activate()
{
	check(IsInGameThread()); using namespace VaRest::Http;
	if (!State || State->bActivated || State->bReleased) return;
	const TStrongObjectPtr<UVaRestFlushQueueAction> Self(this); State->bActivated=true;
	if (!CanDeliverOnGT()) { ReleaseOnGT(); return; }
	TUniqueFunction<void()> Cleanup=[Pin=TStrongObjectPtr<UVaRestFlushQueueAction>(this)] { Pin->ReleaseOnGT(); };
	if (!State->Completion->SetActionLifetimeOnGT(MoveTemp(Cleanup))) { ReleaseOnGT(); return; }
	const TWeakObjectPtr<UVaRestFlushQueueAction> Weak(this);
	const auto Result=State->Result;
	TUniqueFunction<void()> Finish=[Weak,Result] { if (auto* Action=Weak.Get()) Action->FinishOnGT(Result->Outcome.load(std::memory_order_acquire)); };
	if (!State->Completion->BindWorkerOnGT(Finish)) { ReleaseOnGT(); return; }
	if (State->bHasOwner && GEngine)
		if (auto* World=GEngine->GetWorldFromContextObject(State->Owner.Get(),EGetWorldErrorMode::ReturnNull))
			if (auto* Instance=World->GetGameInstance()) RegisterWithGameInstance(Instance);
	const auto Guard=MakeShared<FFlushDeliveryGuard,ESPMode::ThreadSafe>(Result,State->Completion.ToSharedRef());
	// Local native value survives a reentrant host close. Callback and last Guard release own only native fields.
	FVaRestClient Client=State->Native;
	State->bAdmitted=Client.FlushQueue([Guard](bool bSuccess) {
		Guard->Result->Outcome.store(bSuccess ? EFlushDelivery::Flushed : EFlushDelivery::Failed,std::memory_order_release);
	});
	if (!State->bAdmitted) Result->Outcome.store(EFlushDelivery::Refused,std::memory_order_release);
	// The current publication stack also retains Guard: neither false admission nor inline discard can signal early.
}
void UVaRestFlushQueueAction::FinishOnGT(VaRest::Http::EFlushDelivery Result)
{
	check(IsInGameThread()); using namespace VaRest::Http;
	if (!State || State->bTerminal || State->bReleased) return;
	const TStrongObjectPtr<UVaRestFlushQueueAction> Self(this);
	State->bTerminal=true; ON_SCOPE_EXIT { ReleaseOnGT(); };
	if (Result==EFlushDelivery::Suppressed || !CanDeliverOnGT()) return;
	switch (Result)
	{
	case EFlushDelivery::Flushed:State->PublishedResult=EVaRestFlushQueueOutcome::Flushed;break;
	case EFlushDelivery::Failed:State->PublishedResult=EVaRestFlushQueueOutcome::Failed;break;
	case EFlushDelivery::Refused:State->PublishedResult=EVaRestFlushQueueOutcome::Refused;break;
	default:return;
	}
	State->bPublished=true; OnCompleted.Broadcast(State->PublishedResult);
}
bool UVaRestFlushQueueAction::WasAdmitted() const { check(IsInGameThread()); return State && State->bAdmitted; }
bool UVaRestFlushQueueAction::HasResult() const { check(IsInGameThread()); return State && State->bPublished; }
bool UVaRestFlushQueueAction::IsFinished() const { check(IsInGameThread()); return State && (State->bTerminal || State->bReleased); }
EVaRestFlushQueueOutcome UVaRestFlushQueueAction::GetResult() const { check(IsInGameThread()); return State ? State->PublishedResult : EVaRestFlushQueueOutcome::Refused; }
void UVaRestFlushQueueAction::ReleaseOnGT()
{
	check(IsInGameThread()); if (!State || State->bReleased) return;
	State->bReleased=true;
	if (auto* Hub=State->Hub.Get()) Hub->OnDeinitializing.RemoveDynamic(this,&UVaRestFlushQueueAction::HubClosing);
	if (State->Completion) State->Completion->CancelOnGT();
	State->Completion.Reset(); State->Native=FVaRestClient(); State->Client.Reset(); State->Hub.Reset(); SetReadyToDestroy();
}
void UVaRestFlushQueueAction::HubClosing() { ReleaseOnGT(); }
void UVaRestFlushQueueAction::BeginDestroy() { ReleaseOnGT(); Super::BeginDestroy(); }
