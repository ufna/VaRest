// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestRequestLibrary.h"
#include "VaRestApi.h"
#include "VaRestApiClient.h"
#include "VaRestHub.h"
#include "VaRestCustomAuthConfig.h"
#include "VaRestDynamicRequest.h"
#include "VaRestClientState.h"
#include "UObject/StrongObjectPtr.h"

UVaRestApiClient* UVaRestRequestLibrary::ResolveClientOnGT(const UObject* WorldContextObject,UVaRestApiClient* Client,FString& Error)
{
	check(IsInGameThread()); Error.Reset();
	if (!Client) { auto* Hub=UVaRestHub::Get(WorldContextObject); Client=Hub ? Hub->GetDefaultClient() : nullptr; }
	const auto Native=IsValid(Client) && !Client->bDetached ? VaRest::Http::FClientAccess::State(Client->Native) : nullptr;
	if (!Native || !Client->Hub.IsValid() || Native->ManagedAdmissionIsClosed() || VaRest::Http::FClientState::IsShuttingDown())
	{ Error=TEXT("LifecycleClosed"); return nullptr; }
	return Client;
}
FVaRestDynamicResultConfig UVaRestRequestLibrary::MakeConfigOnGT(UVaRestApiClient* Client,bool bFile)
{
	check(IsInGameThread()); FVaRestDynamicResultConfig Config;
	Config.ResponseType=bFile ? FVaRestTypeRef() : FVaRestTypeRef::Json();
	Config.RuleOwner=Client->Asset; Config.Budget=Client->Hub.Get()->GetDynamicBudget(); return Config;
}
UVaRestGetJsonAction* UVaRestRequestLibrary::GetJson(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Path,FString& Error)
{
	Client=ResolveClientOnGT(WorldContextObject,Client,Error); if (!Client) return nullptr;
	const TStrongObjectPtr<UVaRestApiClient> Kept(Client);
	auto Request=Client->Native.RequestDynamic(TEXTVIEW("GET"),Path,MakeConfigOnGT(Client)).Owner(WorldContextObject);
	return Cast<UVaRestGetJsonAction>(UVaRestRequestAction::AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),false,false,UVaRestGetJsonAction::StaticClass(),Error));
}
UVaRestGetJsonAction* UVaRestRequestLibrary::RequestJson(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Verb,const FString& Path,const FVaRestJson& Body,FString& Error)
{
	Client=ResolveClientOnGT(WorldContextObject,Client,Error); if (!Client) return nullptr;
	const TStrongObjectPtr<UVaRestApiClient> Kept(Client);
	auto Request=Client->Native.RequestDynamic(Verb,Path,MakeConfigOnGT(Client)).Owner(WorldContextObject);
	if (Body.IsValid()) Request=MoveTemp(Request).Body(Body);
	return Cast<UVaRestGetJsonAction>(UVaRestRequestAction::AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),false,false,UVaRestGetJsonAction::StaticClass(),Error));
}
UVaRestRequestAction* UVaRestRequestLibrary::DownloadFile(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Path,const FString& FilePath,FString& Error)
{
	Client=ResolveClientOnGT(WorldContextObject,Client,Error); if (!Client) return nullptr;
	const TStrongObjectPtr<UVaRestApiClient> Kept(Client);
	auto Request=Client->Native.RequestDynamic(TEXTVIEW("GET"),Path,MakeConfigOnGT(Client,true)).Owner(WorldContextObject).NoEnvelope().SaveTo(FilePath);
	return UVaRestRequestAction::AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),false,false,UVaRestRequestAction::StaticClass(),Error);
}
UVaRestGetJsonAction* UVaRestRequestLibrary::UploadFile(const UObject* WorldContextObject,UVaRestApiClient* Client,const FString& Verb,const FString& Path,const FString& FilePath,const FString& ContentType,FString& Error)
{
	Client=ResolveClientOnGT(WorldContextObject,Client,Error); if (!Client) return nullptr;
	const TStrongObjectPtr<UVaRestApiClient> Kept(Client);
	auto Request=Client->Native.RequestDynamic(Verb,Path,MakeConfigOnGT(Client)).Owner(WorldContextObject).BodyFile(FilePath,ContentType);
	return Cast<UVaRestGetJsonAction>(UVaRestRequestAction::AdoptOnGT(WorldContextObject,Client,MoveTemp(Request),false,false,UVaRestGetJsonAction::StaticClass(),Error));
}
UVaRestGetJsonAction* UVaRestRequestLibrary::RequestRefreshJson(const UObject* WorldContextObject,UVaRestAuthRefreshContext* Context,const FString& Verb,const FString& Path,const FVaRestJson& Body,FString& Error)
{
	check(IsInGameThread());
	if (!IsValid(Context)) { Error=TEXT("StaleRefreshContext"); return nullptr; }
	return Context->RequestJson(WorldContextObject,Verb,Path,Body,Error);
}
