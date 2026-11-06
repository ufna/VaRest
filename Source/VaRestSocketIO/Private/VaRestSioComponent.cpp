// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioComponent.h"
#include "VaRestSioLibrary.h"
#include "VaRestSioCodec.h"

UVaRestSocketClient* UVaRestSioComponent::CreateClientOnGT(FVaRestError& Error)
{
	for (const auto& Name : Namespaces)
		if (!Name.IsEmpty() && !FVaRestSioCodec::IsValidNamespace(Name))
		{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("An extra Socket.IO namespace has an invalid name"); return nullptr; }
	if (Api || !Entry.IsNone()) SioClient = UVaRestSioLibrary::GetSocketIO(this, Api, Entry, Error);
	else
	{
		if (Scope != EVaRestSocketScope::Actor && Scope != EVaRestSocketScope::GameInstance)
		{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("A Socket.IO URL requires Actor or GameInstance scope"); return nullptr; }
		if (Scope == EVaRestSocketScope::GameInstance && SocketName.IsNone())
		{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("A GameInstance Socket.IO URL requires SocketName"); return nullptr; }
		SioClient = UVaRestSioLibrary::MakeSocketIO(this, Url, Namespace, {}, {}, Error, Scope == EVaRestSocketScope::Actor ? NAME_None : SocketName);
	}
	if (!SioClient) return nullptr;
	SioClient->OnConnect.AddDynamic(this, &UVaRestSioComponent::ForwardConnect);
	SioClient->OnConnectError.AddDynamic(this, &UVaRestSioComponent::ForwardConnectError);
	SioClient->OnDisconnect.AddDynamic(this, &UVaRestSioComponent::ForwardDisconnect);
	SioClient->OnEvent.AddDynamic(this, &UVaRestSioComponent::ForwardEvent);
	for (const auto& Name : Namespaces) SioClient->GetNativeSioSocket().Namespace(Name).WithConnectOnStart(true);
	return SioClient;
}
void UVaRestSioComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (SioClient)
	{
		SioClient->OnConnect.RemoveDynamic(this, &UVaRestSioComponent::ForwardConnect);
		SioClient->OnConnectError.RemoveDynamic(this, &UVaRestSioComponent::ForwardConnectError);
		SioClient->OnDisconnect.RemoveDynamic(this, &UVaRestSioComponent::ForwardDisconnect);
		SioClient->OnEvent.RemoveDynamic(this, &UVaRestSioComponent::ForwardEvent);
	}
	Super::EndPlay(EndPlayReason); SioClient = nullptr;
}
void UVaRestSioComponent::ForwardConnect(const FVaRestSioConnect& Info) { OnConnect.Broadcast(Info); }
void UVaRestSioComponent::ForwardConnectError(const FVaRestError& Error, const FVaRestSioConnectError& Info) { OnConnectError.Broadcast(Error, Info); }
void UVaRestSioComponent::ForwardDisconnect(const FVaRestSioDisconnect& Info) { OnDisconnect.Broadcast(Info); }
void UVaRestSioComponent::ForwardEvent(const FVaRestSioEvent& Event) { OnEvent.Broadcast(Event); }
