// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketHub.h"
#include "VaRestSocketClient.h"
#include "VaRestSocketEndpoint.h"
#include "VaRestApi.h"
#include "VaRestWebSocketLog.h"
#include "VaRestSocketState.h"
#include "VaRestSocketSubsystems.h"
#include "VaRestHub.h"
#include "VaRestEngineSubsystem.h"
#include "VaRestGameInstanceSubsystem.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"

UVaRestSocketHub* UVaRestSocketHub::Get(const UObject* WorldContextObject)
{
	check(IsInGameThread());
	const auto* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContextObject, EGetWorldErrorMode::ReturnNull) : nullptr;
	const auto* Instance = World ? World->GetGameInstance() : nullptr;
	const auto* Host = Instance ? Instance->GetSubsystem<UVaRestSocketGameInstanceSubsystem>() : nullptr;
	return Host ? Host->GetHub() : nullptr;
}
UVaRestSocketHub* UVaRestSocketHub::GetForEditor()
{
	check(IsInGameThread());
	const auto* Host = GEngine ? GEngine->GetEngineSubsystem<UVaRestSocketEngineSubsystem>() : nullptr;
	return Host ? Host->GetHub() : nullptr;
}
bool UVaRestSocketHub::InitializeOnGT(UVaRestHub* InCoreHub)
{
	check(IsInGameThread());
	if (bClosing || CoreHub.IsValid() || !IsValid(InCoreHub) || !InCoreHub->GetScopeId()) return false;
	CoreHub = InCoreHub;
	InCoreHub->OnDeinitializing.AddDynamic(this, &UVaRestSocketHub::OnCoreDeinitializing);
	InCoreHub->OnEnvironmentChanged.AddDynamic(this, &UVaRestSocketHub::OnEnvironmentChanged);
	return true;
}
UVaRestHub* UVaRestSocketHub::GetCoreHubOnGT() const { check(IsInGameThread()); return bClosing ? nullptr : CoreHub.Get(); }
#if WITH_DEV_AUTOMATION_TESTS
bool UVaRestSocketHub::ConfigureTestingOnGT(TSharedPtr<IVaRestSocketTransport, ESPMode::ThreadSafe> Transport,
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock, int32 Seed)
{
	check(IsInGameThread());
	if (bClosing || !CoreHub.IsValid() || !All.IsEmpty() || !Transport || !Clock) return false;
	TestTransport = MoveTemp(Transport); TestClock = MoveTemp(Clock); TestSeed = Seed;
	return true;
}
#endif
FVaRestSocket UVaRestSocketHub::Make(FStringView Url) { return Make(NAME_None, Url); }
FVaRestSocket UVaRestSocketHub::Make(FName Name, FStringView Url)
{
	return MakeForProtocol(Name, Url, TEXT("WebSocket"), [](FVaRestSocket&) {});
}
FVaRestSocket UVaRestSocketHub::MakeForProtocol(FName Name, FStringView Url, FName Protocol, TFunctionRef<void(FVaRestSocket&)> Initialize)
{
	check(IsInGameThread());
	if (bClosing || Protocol.IsNone() || (!Name.IsNone() && CreatingNamed.Contains(Name))) return {};
	if (!Name.IsNone())
	{
		if (const auto* Existing = Named.Find(Name); Existing && Existing->IsValid())
		{
			if (Existing->GetProtocolTag() == Protocol) return *Existing;
			UE_LOG(LogVaRestSocket, Error, TEXT("Config: use the owning protocol accessor for this named socket")); return {};
		}
		Named.Remove(Name);
	}
	const TStrongObjectPtr<UVaRestSocketHub> Pin(this);
	if (!Name.IsNone()) CreatingNamed.Add(Name);
	ON_SCOPE_EXIT { if (!Name.IsNone()) CreatingNamed.Remove(Name); };
	auto Socket = VaRest::WebSocket::FSocketAccess::CreateOnGT(CoreHub.Get(), Name, FString(Url)
#if WITH_DEV_AUTOMATION_TESTS
		, TestTransport, TestClock, TestSeed
#endif
	);
	if (!Socket.IsValid() || bClosing) return {};
	Initialize(Socket);
	if (!Socket.IsValid() || bClosing || Socket.GetProtocolTag() != Protocol)
	{
		if (Socket.IsValid()) VaRest::WebSocket::FSocketAccess::DisposeOnGT(Socket, TEXTVIEW("protocol initialization failed"));
		return {};
	}
	All.RemoveAll([](const auto& Weak) { return !Weak.Pin().IsValid(); });
	All.Add(Socket.AsWeak());
	if (!Name.IsNone()) Named.Add(Name, Socket);
	return Socket;
}
FVaRestSocket UVaRestSocketHub::Find(FName Name) const
{
	return FindForProtocol(Name, TEXT("WebSocket"));
}
FVaRestSocket UVaRestSocketHub::FindForProtocol(FName Name, FName Protocol) const
{
	check(IsInGameThread());
	if (bClosing || Name.IsNone()) return {};
	const auto* Existing = Named.Find(Name);
	if (!Existing || !Existing->IsValid()) return {};
	if (Existing->GetProtocolTag() != Protocol)
	{ UE_LOG(LogVaRestSocket, Error, TEXT("Config: use the owning protocol accessor for this named socket")); return {}; }
	return *Existing;
}
namespace
{
	const UVaRestSocketEndpointBase* FindSocketEntry(const UVaRestApi* Api, FName Entry, FName Protocol, FVaRestError& Error)
	{
		Error = {}; Error.Kind = EVaRestErrorKind::Config;
		if (!IsValid(Api) || Entry.IsNone()) { Error.Message = TEXT("A socket requires an API asset and entry name"); return nullptr; }
		const UVaRestApiSocketConfig* Found = nullptr;
		for (const auto& Candidate : Api->Sockets)
			if (Candidate && Candidate->Name == Entry)
			{
				if (Found) { Error.Message = TEXT("The socket entry name is ambiguous"); return nullptr; }
				Found = Candidate;
			}
		if (!Found) { Error.Message = TEXT("The API asset has no socket with that name"); return nullptr; }
		if (Found->GetProtocolTag() != Protocol)
		{ Error.Message = TEXT("This socket belongs to another protocol; use its module's socket accessor"); return nullptr; }
		const auto* Endpoint = Cast<UVaRestSocketEndpointBase>(Found);
		if (!Endpoint) { Error.Message = TEXT("The socket entry has an unsupported concrete type"); return nullptr; }
		FVaRestApiValidationContext Validation; Endpoint->Validate(Validation);
		if (Validation.HasErrors()) { Error.Message = TEXT("The socket entry has invalid configuration; validate the API asset"); return nullptr; }
		Error = {}; return Endpoint;
	}
}
FVaRestSocket UVaRestSocketHub::Socket(UVaRestApi* Api, FName Entry, FVaRestError& Error)
{
	return SocketForProtocol(Api, Entry, TEXT("WebSocket"), Error, [](FVaRestSocket& Created, const UVaRestSocketEndpointBase& Endpoint, FStringView)
	{
		if (!Cast<UVaRestSocketEndpoint>(&Endpoint)) return false;
		Endpoint.ConfigureSocket(Created); return true;
	});
}
FVaRestSocket UVaRestSocketHub::SocketForProtocol(UVaRestApi* Api, FName Entry, FName Protocol, FVaRestError& Error,
	TFunctionRef<bool(FVaRestSocket&, const UVaRestSocketEndpointBase&, FStringView)> Initialize,
	TFunction<void(UVaRestSocketHub&, UVaRestApi*, FName, FVaRestSocket&)> EnvironmentChanged)
{
	check(IsInGameThread());
	Error = {};
	const TStrongObjectPtr<UVaRestSocketHub> HubPin(this);
	if (bClosing || !CoreHub.IsValid()) { Error.Kind = EVaRestErrorKind::Canceled; Error.Message = TEXT("The socket Hub is closing"); return {}; }
	const TStrongObjectPtr<UVaRestApi> ApiPin(Api);
	const auto* Endpoint = FindSocketEntry(Api, Entry, Protocol, Error); if (!Endpoint) return {};
	for (const auto& Binding : AssetSockets)
		if (Binding.Api == Api && Binding.Entry == Entry && Binding.Socket.IsValid())
		{
			if (Binding.Socket.GetProtocolTag() == Protocol) return Binding.Socket;
			Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Use the owning protocol module's socket accessor"); return {};
		}
	AssetSockets.RemoveAll([Api, Entry](const auto& Binding) { return Binding.Api == Api && Binding.Entry == Entry; });
	if (CreatingAssets.ContainsByPredicate([Api, Entry](const auto& Row) { return Row.Key.Get() == Api && Row.Value == Entry; }))
	{ Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("Recursive creation of the same socket entry"); return {}; }
	CreatingAssets.Emplace(Api, Entry);
	ON_SCOPE_EXIT { CreatingAssets.RemoveAll([Api, Entry](const auto& Row) { return Row.Key.Get() == Api && Row.Value == Entry; }); };
	auto* Client = CoreHub->GetClient(Api);
	if (bClosing || !CoreHub.IsValid()) { Error.Kind = EVaRestErrorKind::Canceled; Error.Message = TEXT("The socket Hub closed during client creation"); return {}; }
	Endpoint = FindSocketEntry(Api, Entry, Protocol, Error); if (!Endpoint) return {};
	if (!Client) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("The API client could not be initialized"); return {}; }
	const auto* Environment = Api->FindEnvironment(Client->GetEnvironment());
	if (!Environment) { Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("The API environment is unavailable"); return {}; }
	const FString ResolvedUrl = Endpoint->ResolveUrl(Environment->SocketBaseUrl.IsEmpty() ? FStringView(Environment->BaseUrl) : FStringView(Environment->SocketBaseUrl));
	auto Created = VaRest::WebSocket::FSocketAccess::CreateOnGT(CoreHub.Get(), Entry, ResolvedUrl
#if WITH_DEV_AUTOMATION_TESTS
		, TestTransport, TestClock, TestSeed
#endif
	);
	if (!Created.IsValid()) { Error.Kind = EVaRestErrorKind::Canceled; Error.Message = TEXT("The socket could not be admitted"); return {}; }
	Created.WithClient(Client->GetNativeClient(), !Endpoint->bNoAuth);
	VaRest::WebSocket::FSocketAccess::SetEntryOnGT(Created, Entry);
	Created.WithNaming(Api->Naming);
	if (!Initialize(Created, *Endpoint, ResolvedUrl) || Created.GetProtocolTag() != Protocol)
	{
		VaRest::WebSocket::FSocketAccess::DisposeOnGT(Created, TEXTVIEW("protocol initialization failed"));
		Error.Kind = EVaRestErrorKind::Config; Error.Message = TEXT("The socket protocol could not be initialized"); return {};
	}
	if (bClosing || !Created.IsValid()) { Error.Kind = EVaRestErrorKind::Canceled; Error.Message = TEXT("Socket configuration was canceled"); return {}; }
	FVaRestSocketAssetBinding Binding; Binding.Api = Api; Binding.Entry = Entry; Binding.Socket = Created; Binding.EnvironmentChanged = MoveTemp(EnvironmentChanged);
	AssetSockets.Add(MoveTemp(Binding));
	All.RemoveAll([](const auto& Weak) { return !Weak.Pin().IsValid(); }); All.Add(Created.AsWeak());
	return Created;
}
UVaRestSocketClient* UVaRestSocketHub::Wrap(FVaRestSocket Native)
{
	check(IsInGameThread());
	if (bClosing || !Native.IsValid() || Native.GetProtocolTag() != TEXT("WebSocket")) return nullptr;
	BlueprintClients.RemoveAll([](const auto& Weak) { return !Weak.IsValid(); });
	for (const auto& Weak : BlueprintClients)
		if (auto* View = Weak.Get(); View && View->GetNativeSocket().IsSameSocket(Native)) return View;
	auto* View = NewObject<UVaRestSocketClient>(this); View->InitializeOnGT(MoveTemp(Native));
	BlueprintClients.Add(View); return View;
}
UVaRestSocketClient* UVaRestSocketHub::GetSocket(UVaRestApi* Api, FName Entry, FVaRestError& Error) { return Wrap(Socket(Api, Entry, Error)); }
void UVaRestSocketHub::ReleaseSocket(UVaRestApi* Api, FName Entry)
{
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestSocketHub> HubPin(this);
	FVaRestSocket Old;
	for (int32 Index = AssetSockets.Num() - 1; Index >= 0; --Index)
		if (AssetSockets[Index].Api == Api && AssetSockets[Index].Entry == Entry)
		{ Old = AssetSockets[Index].Socket; AssetSockets.RemoveAt(Index); }
	if (Old.IsValid()) VaRest::WebSocket::FSocketAccess::DisposeOnGT(Old, TEXTVIEW("released"));
}
void UVaRestSocketHub::OnEnvironmentChanged(UVaRestApi* Api)
{
	check(IsInGameThread());
	if (bClosing || !CoreHub.IsValid()) return;
	const TStrongObjectPtr<UVaRestSocketHub> HubPin(this);
	const auto Snapshot = AssetSockets;
	for (const auto& Binding : Snapshot)
	{
		if (bClosing) break;
		if (Binding.Api != Api || !Binding.Socket.IsValid()) continue;
		if (Binding.EnvironmentChanged)
		{
			auto Current = Binding.Socket;
			Binding.EnvironmentChanged(*this, Api, Binding.Entry, Current); continue;
		}
		FVaRestError Error;
		auto* Client = CoreHub->GetClient(Api);
		if (bClosing || !CoreHub.IsValid()) break;
		const auto* Endpoint = FindSocketEntry(Api, Binding.Entry, TEXT("WebSocket"), Error);
		const auto* Environment = Client ? Api->FindEnvironment(Client->GetEnvironment()) : nullptr;
		auto Current = Binding.Socket;
		if (!Endpoint || !Environment) { Current.Close(1001, TEXTVIEW("invalid API environment")); continue; }
		Current.WithUrl(Endpoint->ResolveUrl(Environment->SocketBaseUrl.IsEmpty() ? FStringView(Environment->BaseUrl) : FStringView(Environment->SocketBaseUrl)));
		Current.WithClient(Client->GetNativeClient(), !Endpoint->bNoAuth);
		Current.Restart(TEXTVIEW("environment changed"));
	}
}

void UVaRestSocketHub::Release(FName Name)
{
	check(IsInGameThread());
	FVaRestSocket Socket;
	if (!Named.RemoveAndCopyValue(Name, Socket)) return;
	VaRest::WebSocket::FSocketAccess::DisposeOnGT(Socket, TEXTVIEW("released"));
}
TArray<FVaRestSocket> UVaRestSocketHub::GetSockets() const
{
	check(IsInGameThread());
	TArray<FVaRestSocket> Out;
	if (!bClosing)
		for (const auto& Weak : All) { auto Socket = Weak.Pin(); if (Socket.IsValid()) Out.Add(MoveTemp(Socket)); }
	return Out;
}
void UVaRestSocketHub::CloseAll()
{
	check(IsInGameThread());
	const TStrongObjectPtr<UVaRestSocketHub> Pin(this);
	const auto Sockets = GetSockets();
	for (const auto& Socket : Sockets) { auto Copy = Socket; Copy.Close(); }
}
void UVaRestSocketHub::OnCoreDeinitializing() { ShutdownOnGT(); }
void UVaRestSocketHub::ShutdownOnGT()
{
	check(IsInGameThread());
	if (bClosing) return;
	const auto Sockets = GetSockets();
	bClosing = true;
	if (auto* Core = CoreHub.Get())
	{
		Core->OnDeinitializing.RemoveDynamic(this, &UVaRestSocketHub::OnCoreDeinitializing);
		Core->OnEnvironmentChanged.RemoveDynamic(this, &UVaRestSocketHub::OnEnvironmentChanged);
	}
	for (const auto& Socket : Sockets) VaRest::WebSocket::FSocketAccess::DisposeOnGT(Socket, TEXTVIEW("shutdown"));
	Named.Reset(); AssetSockets.Reset(); BlueprintClients.Reset(); All.Reset(); CoreHub.Reset();
#if WITH_DEV_AUTOMATION_TESTS
	TestTransport.Reset(); TestClock.Reset();
#endif
}
void UVaRestSocketHub::BeginDestroy() { ShutdownOnGT(); Super::BeginDestroy(); }

void UVaRestSocketGameInstanceSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const auto* CoreHost = Collection.InitializeDependency<UVaRestGameInstanceSubsystem>();
	Hub = NewObject<UVaRestSocketHub>(this);
	if (!Hub->InitializeOnGT(CoreHost ? CoreHost->GetHub() : nullptr)) Hub = nullptr;
}
void UVaRestSocketGameInstanceSubsystem::Deinitialize()
{
	if (Hub) Hub->ShutdownOnGT();
	Hub = nullptr; Super::Deinitialize();
}
void UVaRestSocketEngineSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	const auto* CoreHost = Collection.InitializeDependency<UVaRestEngineSubsystem>();
	Hub = NewObject<UVaRestSocketHub>(this);
	if (!Hub->InitializeOnGT(CoreHost ? CoreHost->GetHub() : nullptr)) Hub = nullptr;
}
void UVaRestSocketEngineSubsystem::Deinitialize()
{
	if (Hub) Hub->ShutdownOnGT();
	Hub = nullptr; Super::Deinitialize();
}
