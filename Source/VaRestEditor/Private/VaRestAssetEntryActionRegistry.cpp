// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAssetEntryActionRegistry.h"
#include "VaRestAssetEntryActionRegistryPrivate.h"
#include "VaRestApi.h"
#include "VaRestApiEditorBridge.h"
#include "VaRestApiSocketConfig.h"
#include "BlueprintActionDatabase.h"
#include "Editor.h"
#include "Misc/CoreMisc.h"
#include "Misc/CoreDelegates.h"
#include "Misc/ScopeExit.h"
#include "UObject/ObjectKey.h"

IVaRestAssetEntryActionProvider::~IVaRestAssetEntryActionProvider() = default;

namespace VaRestEditor
{
struct FAssetEntryRegistryState
{
	struct FProvider { FDelegateHandle Handle; FName Protocol; TSharedPtr<IVaRestAssetEntryActionProvider> Provider; };
	~FAssetEntryRegistryState() { check(IsInGameThread()); check(Providers.IsEmpty()); }
	TArray<FProvider> Providers;
	TSet<FObjectKey> Refreshing;
	TArray<TWeakObjectPtr<UVaRestApi>> PendingRefresh;
	FDelegateHandle Edited,Frame,EngineInit;
	uint32 CallbackDepth=0;
	bool bStopped=false,bDatabaseRefreshing=false,bPendingFullRefresh=false;
};
namespace AssetRegistryPrivate
{
// Weak only: the actual module owner and GT-local methods own all provider/callback destruction.
TWeakPtr<FAssetEntryRegistryState> Active;
TSharedPtr<FAssetEntryRegistryState> Get()
{
	check(IsInGameThread()); const auto State=Active.Pin(); return State && !State->bStopped ? State : nullptr;
}
void RefreshProviders(const TSharedPtr<FAssetEntryRegistryState>& State)
{
	if (!State || State->bStopped || Active.Pin()!=State || IsEngineExitRequested()) return;
	// A Default-phase provider must not initialize the action database: it loads editor modules that require GEditor.
	if (!GEditor) { State->bPendingFullRefresh=true;return; }
	State->bPendingFullRefresh=false;
	TGuardValue<bool> Guard(State->bDatabaseRefreshing,true);FBlueprintActionDatabase::Get().RefreshAll();
}
bool Current(const TSharedPtr<FAssetEntryRegistryState>& State,const FAssetEntryRegistryState::FProvider& Provider)
{
	return State && !State->bStopped && Active.Pin()==State && State->Providers.ContainsByPredicate([&](const auto& Row) {
		return Row.Handle==Provider.Handle && Row.Provider==Provider.Provider;
	});
}
void RefreshAsset(const TSharedPtr<FAssetEntryRegistryState>& State,UVaRestApi* Api)
{
	if (!State || State->bStopped || Active.Pin()!=State || !IsValid(Api) || IsEngineExitRequested()) return;
	const FObjectKey Key(Api); if (State->Refreshing.Contains(Key)) return;
	if (!GEditor || State->bDatabaseRefreshing) { State->PendingRefresh.AddUnique(TWeakObjectPtr<UVaRestApi>(Api)); return; }
	const TStrongObjectPtr<UVaRestApi> Pin(Api);
	State->Refreshing.Add(Key); ON_SCOPE_EXIT { State->Refreshing.Remove(Key); };
	{ TGuardValue<bool> Guard(State->bDatabaseRefreshing,true); FBlueprintActionDatabase::Get().RefreshAssetActions(Api); }
	// Exactly once for this complete NotifyEdited batch; same-asset reconstruction notifications are coalesced above.
	const auto Providers=State->Providers; // stable original providers across reconstruction/unregister callouts
	for (const auto& Provider:Providers) if (Current(State,Provider))
	{ TGuardValue<uint32> Guard(State->CallbackDepth,State->CallbackDepth+1); Provider.Provider->RefreshLoadedNodes(*Api); }
}
}
TUniquePtr<FAssetEntryRegistryOwner> FAssetEntryRegistryOwner::CreateOnGT()
{
	check(IsInGameThread()); if (AssetRegistryPrivate::Get()) return nullptr;
	TUniquePtr<FAssetEntryRegistryOwner> Owner(new FAssetEntryRegistryOwner());
	Owner->State=MakeShared<FAssetEntryRegistryState>(); AssetRegistryPrivate::Active=Owner->State;
	const TWeakPtr<FAssetEntryRegistryState> Weak=Owner->State;
	Owner->State->Edited=FVaRestApiEditorBridge::OnEdited().AddLambda([Weak](UVaRestApi* Api) { AssetRegistryPrivate::RefreshAsset(Weak.Pin(),Api); });
	Owner->State->EngineInit=FCoreDelegates::GetOnPostEngineInit().AddLambda([Weak] { const auto State=Weak.Pin();if (State && State->bPendingFullRefresh) AssetRegistryPrivate::RefreshProviders(State); });
	Owner->State->Frame=FCoreDelegates::OnEndFrame.AddLambda([Weak] {
		const auto State=Weak.Pin(); if (!State || State->bStopped || !GEditor || IsEngineExitRequested()) return;
		auto Pending=MoveTemp(State->PendingRefresh); State->PendingRefresh.Reset();
		for (const auto& Api:Pending) AssetRegistryPrivate::RefreshAsset(State,Api.Get());
	});
	return Owner;
}
FAssetEntryRegistryOwner::~FAssetEntryRegistryOwner()
{
	check(IsInGameThread()); if (!State) return;
	State->bStopped=true;
	if (AssetRegistryPrivate::Active.Pin()==State) AssetRegistryPrivate::Active.Reset();
	FVaRestApiEditorBridge::OnEdited().Remove(State->Edited);
	FCoreDelegates::OnEndFrame.Remove(State->Frame);
	FCoreDelegates::GetOnPostEngineInit().Remove(State->EngineInit);
	auto Dropped=MoveTemp(State->Providers); Dropped.Reset(); State.Reset(); // real provider destruction on GT after disable
}
}

FDelegateHandle FVaRestAssetEntryActionRegistry::RegisterProvider(TSharedRef<IVaRestAssetEntryActionProvider> Provider,FString& Error)
{
	using namespace VaRestEditor; using namespace AssetRegistryPrivate;
	check(IsInGameThread()); Error.Reset();
	const auto State=Get(); if (!State) { Error=TEXT("VaRestEditor registry is unavailable"); return {}; }
	if (State->CallbackDepth || State->bDatabaseRefreshing) { Error=TEXT("provider registration is busy in an action callback"); return {}; }
	FName Protocol;
	{ TGuardValue<uint32> Guard(State->CallbackDepth,State->CallbackDepth+1); Protocol=Provider->GetProtocolTag(); }
	if (State->bStopped || Active.Pin()!=State) { Error=TEXT("VaRestEditor registry closed during registration"); return {}; }
	if (Protocol.IsNone()) { Error=TEXT("a protocol provider needs a nonempty tag"); return {}; }
	if (State->Providers.ContainsByPredicate([Protocol](const auto& Row) { return Row.Protocol==Protocol; }))
	{ Error=TEXT("this protocol already has a registered provider"); return {}; }
	const FDelegateHandle Handle(FDelegateHandle::GenerateNewHandle);
	State->Providers.Add({Handle,Protocol,Provider});
	RefreshProviders(State);
	if (!IsRegistered(Handle)) { Error=TEXT("the provider was removed during action refresh"); return {}; }
	return Handle;
}
bool FVaRestAssetEntryActionRegistry::UnregisterProvider(FDelegateHandle Handle)
{
	using namespace VaRestEditor::AssetRegistryPrivate;
	check(IsInGameThread()); const auto State=Get(); if (!State || !Handle.IsValid()) return false;
	if (State->CallbackDepth || State->bDatabaseRefreshing) return false;
	const int32 Index=State->Providers.IndexOfByPredicate([Handle](const auto& Row) { return Row.Handle==Handle; });
	if (Index==INDEX_NONE) return false;
	auto Dropped=MoveTemp(State->Providers[Index]); State->Providers.RemoveAt(Index);
	RefreshProviders(State);
	return true; // Dropped's real provider lives until cached actions are refreshed
}
bool FVaRestAssetEntryActionRegistry::IsRegistered(FDelegateHandle Handle)
{
	check(IsInGameThread()); const auto State=VaRestEditor::AssetRegistryPrivate::Get();
	return State && Handle.IsValid() && State->Providers.ContainsByPredicate([Handle](const auto& Row) { return Row.Handle==Handle; });
}
TArray<TStrongObjectPtr<UBlueprintNodeSpawner>> FVaRestAssetEntryActionRegistry::GatherActions(UVaRestApi* Api)
{
	using namespace VaRestEditor; using namespace AssetRegistryPrivate;
	check(IsInGameThread()); TArray<TStrongObjectPtr<UBlueprintNodeSpawner>> Result;
	const auto State=Get(); if (!State || !IsValid(Api)) return Result;
	const TStrongObjectPtr<UVaRestApi> Pin(Api);
	const auto Providers=State->Providers;
	TArray<TStrongObjectPtr<UVaRestApiSocketConfig>> Entries;
	for (const auto& Entry:Api->Sockets) if (IsValid(Entry.Get())) Entries.Emplace(Entry.Get());
	TSet<UBlueprintNodeSpawner*> Seen;
	const auto Gather=[&](const FAssetEntryRegistryState::FProvider& Provider,UVaRestApiSocketConfig* Entry) {
		if (!Current(State,Provider)) return;
		TArray<UBlueprintNodeSpawner*> Actions;
		{ TGuardValue<uint32> Guard(State->CallbackDepth,State->CallbackDepth+1); Provider.Provider->GatherActions(*Api,Entry,Actions); }
		if (!Current(State,Provider)) return;
		for (auto* Action:Actions) if (IsValid(Action) && !Seen.Contains(Action)) { Seen.Add(Action); Result.Emplace(Action); }
	};
	for (const auto& Provider:Providers)
	{
		Gather(Provider,nullptr);
		for (const auto& Entry:Entries)
		{
			if (!Current(State,Provider)) break;
			FName Protocol;
			{ TGuardValue<uint32> Guard(State->CallbackDepth,State->CallbackDepth+1); Protocol=Entry->GetProtocolTag(); }
			if (Protocol==Provider.Protocol) Gather(Provider,Entry.Get());
		}
	}
	return Result;
}
