// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketComponent.h"
#include "VaRestSocketHub.h"
#include "VaRestSocketLibrary.h"

UVaRestSocketClient* UVaRestSocketComponent::CreateClientOnGT(FVaRestError& Error)
{
	if (Api || !Entry.IsNone()) return UVaRestSocketLibrary::GetSocket(this, Api, Entry, Error);
	if (Scope != EVaRestSocketScope::Actor && Scope != EVaRestSocketScope::GameInstance)
	{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("A URL socket requires Actor or GameInstance scope"); return nullptr; }
	if (Scope == EVaRestSocketScope::GameInstance && SocketName.IsNone())
	{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("A GameInstance URL socket requires SocketName"); return nullptr; }
	return UVaRestSocketLibrary::MakeSocket(this, Url, {}, {}, Error, Scope == EVaRestSocketScope::Actor ? NAME_None : SocketName);
}
void UVaRestSocketComponent::BeginPlay()
{
	bEndingPlay = false;
	Super::BeginPlay();
	if (bEndingPlay) return;
	const bool bOwnsUrl = !Api && Entry.IsNone() && Scope == EVaRestSocketScope::Actor;
	FVaRestError Error; auto* Created = CreateClientOnGT(Error);
	if (bEndingPlay) { if (Created && bOwnsUrl) Created->Close(1001, TEXT("component EndPlay")); return; }
	Client = Created;
	if (!Client) { OnError.Broadcast(Error); return; }
	bActorOwned = bOwnsUrl;
	if (bActorOwned) Client->GetNativeSocket().Owner(this);
	Client->OnConnected.AddDynamic(this, &UVaRestSocketComponent::ForwardConnected);
	Client->OnReady.AddDynamic(this, &UVaRestSocketComponent::ForwardReady);
	Client->OnDisconnected.AddDynamic(this, &UVaRestSocketComponent::ForwardDisconnected);
	Client->OnRetryScheduled.AddDynamic(this, &UVaRestSocketComponent::ForwardRetry);
	Client->OnGaveUp.AddDynamic(this, &UVaRestSocketComponent::ForwardGaveUp);
	Client->OnStateChanged.AddDynamic(this, &UVaRestSocketComponent::ForwardState);
	Client->OnError.AddDynamic(this, &UVaRestSocketComponent::ForwardError);
	if (bAutoConnect) Client->Connect();
}
void UVaRestSocketComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	bEndingPlay = true;
	if (Client)
	{
		Client->OnConnected.RemoveDynamic(this, &UVaRestSocketComponent::ForwardConnected);
		Client->OnReady.RemoveDynamic(this, &UVaRestSocketComponent::ForwardReady);
		Client->OnDisconnected.RemoveDynamic(this, &UVaRestSocketComponent::ForwardDisconnected);
		Client->OnRetryScheduled.RemoveDynamic(this, &UVaRestSocketComponent::ForwardRetry);
		Client->OnGaveUp.RemoveDynamic(this, &UVaRestSocketComponent::ForwardGaveUp);
		Client->OnStateChanged.RemoveDynamic(this, &UVaRestSocketComponent::ForwardState);
		Client->OnError.RemoveDynamic(this, &UVaRestSocketComponent::ForwardError);
		if (bActorOwned || bCloseOnEndPlay) Client->Close(1001, TEXT("component EndPlay"));
	}
	Client = nullptr; bActorOwned = false;
	Super::EndPlay(EndPlayReason);
}
void UVaRestSocketComponent::ForwardConnected(const FVaRestSocketOpen& Open) { OnConnected.Broadcast(Open); }
void UVaRestSocketComponent::ForwardReady(const FVaRestSocketOpen& Open, bool bIsReconnect) { OnReady.Broadcast(Open, bIsReconnect); }
void UVaRestSocketComponent::ForwardDisconnected(const FVaRestSocketClose& Close) { OnDisconnected.Broadcast(Close); }
void UVaRestSocketComponent::ForwardRetry(const FVaRestSocketStatus& Status, const FVaRestSocketClose& Close) { OnRetryScheduled.Broadcast(Status, Close); }
void UVaRestSocketComponent::ForwardGaveUp(const FVaRestError& Error, const FVaRestSocketClose& Close) { OnGaveUp.Broadcast(Error, Close); }
void UVaRestSocketComponent::ForwardState(EVaRestSocketState OldState, EVaRestSocketState NewState) { OnStateChanged.Broadcast(OldState, NewState); }
void UVaRestSocketComponent::ForwardError(const FVaRestError& Error) { OnError.Broadcast(Error); }
