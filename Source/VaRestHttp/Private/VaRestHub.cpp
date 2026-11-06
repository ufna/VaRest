// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHub.h"
#include "UnrealEngine.h"
#include "VaRestApi.h"
#include "VaRestEngineSubsystem.h"
#include "VaRestGameInstanceSubsystem.h"
#include "VaRestHubRuntime.h"
#include "VaRestHttpLog.h"
#include "VaRestHttpSettings.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "UObject/Package.h"
#include "UObject/StrongObjectPtr.h"
#include "Misc/ScopeExit.h"

namespace VaRest::Http::HubPrivate
{
FGuid PlayIdentity; // native value only, replaced by the real PreBeginPIE event, shared across that play's GIs
FString Field(const FString& Text) { const FTCHARToUTF8 Utf8(*Text); return FString::Printf(TEXT("%d:"),Utf8.Length())+Text; }
}
void UVaRestHub::BeginPlaySessionOnGT() { check(IsInGameThread()); VaRest::Http::HubPrivate::PlayIdentity=FGuid::NewGuid(); }
bool UVaRestHub::InitializeOnGT(UGameInstance* GameInstance,bool bEditorFallback,FString& Error)
{
	using namespace VaRest::Http;
	check(IsInGameThread());
	if (Runtime || bClosing) { Error=TEXT("HubAlreadyInitialized"); return false; }
	FVaRestTraceScope Info;
	Info.GameInstance=GameInstance; Info.bEditorScope=bEditorFallback && GIsEditor;
	FString Profile=GetDefault<UVaRestHttpSettings>()->StorageProfile;
	if (bEditorFallback)
	{
		Info.Label=GIsEditor ? TEXT("Editor") : TEXT("Engine");
		Profile=(GIsEditor ? FString(TEXT("M5Editor")) : FString(TEXT("M5Engine")))+HubPrivate::Field(Profile);
	}
	else
	{
		if (!GameInstance) { Error=TEXT("GameInstanceRequired"); return false; }
		const auto* World=GameInstance->GetWorld();
		Info.Label=World ? GetDebugStringForWorld(World) : GameInstance->GetName();
#if WITH_EDITOR
		if (World && World->WorldType==EWorldType::PIE)
		{
			if (!HubPrivate::PlayIdentity.IsValid()) { Error=TEXT("PlaySessionIdentityUnavailable"); return false; }
			Info.PIEInstance=World->GetPackage()->GetPIEInstanceID();
			Profile=TEXT("M5PIE")+HubPrivate::Field(Profile)+HubPrivate::Field(HubPrivate::PlayIdentity.ToString(EGuidFormats::Digits))+
				HubPrivate::Field(FString::FromInt(Info.PIEInstance));
		}
#endif
	}
	const TStrongObjectPtr<UVaRestHub> Self(this);
	auto Created=FHubRuntime::CreateOnGT(MoveTemp(Info),MoveTemp(Profile),Error);
	if (!Created) return false;
	if (bClosing) { Created->RetireOnGT(); Error=TEXT("LifecycleClosed"); return false; }
	Runtime=MoveTemp(Created); // a Scope Opened sink cannot resurrect a facade it deinitialized reentrantly
	const TWeakObjectPtr<UVaRestHub> Weak(this);
	ConnectivityHandle=Runtime->ConnectivityOnGT()->OnChanged().AddWeakLambda(this,[Weak](bool bOnline) {
		if (auto* Target=Weak.Get(); Target && !Target->bClosing) { const TStrongObjectPtr<UVaRestHub> Kept(Target); Target->OnConnectivityChanged.Broadcast(bOnline); }
	});
	RulesHandle=FVaRestJsonRules::OnRulesChanged().AddWeakLambda(this,[Weak] {
		if (auto* Target=Weak.Get(); Target && !Target->bClosing)
		{
			const TStrongObjectPtr<UVaRestHub> Kept(Target);
			TArray<FVaRestClient> NativeClients;
			TArray<TSharedPtr<const FVaRestJsonRuleSnapshot,ESPMode::ThreadSafe>> Snapshots;
			for (UVaRestApiClient* Wrapper:Target->Clients)
			{
				if (!Wrapper) continue;
				NativeClients.Add(Wrapper->GetNativeClient()); Snapshots.Add(FVaRestJsonRules::Snapshot(Wrapper->GetApi()));
			}
			for (int32 Index=0;Index<NativeClients.Num() && !Target->bClosing;++Index) NativeClients[Index].WithJsonRules(Snapshots[Index]);
		}
	});
	return true;
}
UVaRestHub* UVaRestHub::Get(const UObject* WorldContextObject)
{
	if (GEngine)
	{
		if (auto* World=GEngine->GetWorldFromContextObject(WorldContextObject,EGetWorldErrorMode::ReturnNull))
			if (auto* Instance=World->GetGameInstance())
			{
				const auto* Host=Instance->GetSubsystem<UVaRestGameInstanceSubsystem>();
				return Host ? Host->GetHub() : nullptr;
			}
	}
	return GetForEditor();
}
UVaRestHub* UVaRestHub::GetForEditor()
{
	const auto* Host=GEngine ? GEngine->GetEngineSubsystem<UVaRestEngineSubsystem>() : nullptr;
	return Host ? Host->GetHub() : nullptr;
}
UVaRestApiClient* UVaRestHub::GetClient(UVaRestApi* Asset)
{
	check(IsInGameThread());
	if (!IsValid(Asset) || bClosing || !Runtime) return nullptr;
	for (UVaRestApiClient* Existing:Clients) if (Existing && Existing->GetApi()==Asset)
		return Existing->GetNativeClient().IsValid() ? Existing : nullptr;
	const TStrongObjectPtr<UVaRestHub> Self(this);
	auto* Wrapper=NewObject<UVaRestApiClient>(this);
	Clients.Add(Wrapper); // internal BeforeCreated installs its complete handle before any Created sink can find this row
	++CreatingClients; ON_SCOPE_EXIT { --CreatingClients; };
	FString Error;
	if (!Wrapper->InitializeOnGT(this,Asset,Runtime.ToSharedRef(),Error))
	{
		Clients.Remove(Wrapper); Wrapper->DetachOnGT();
		UE_LOG(LogVaRestHttp,Warning,TEXT("VaRest API client refused: %s"),*Error); return nullptr;
	}
	return bClosing ? nullptr : Wrapper;
}
UVaRestApiClient* UVaRestHub::MakeClient(const FString& BaseUrl)
{
	check(IsInGameThread());
	if (bClosing || !Runtime) return nullptr;
	const TStrongObjectPtr<UVaRestHub> Self(this);
	auto* Wrapper=NewObject<UVaRestApiClient>(this); Clients.Add(Wrapper);
	++CreatingClients; ON_SCOPE_EXIT { --CreatingClients; };
	FString Error;
	if (!Wrapper->InitializeOnGT(this,nullptr,Runtime.ToSharedRef(),Error,BaseUrl))
	{
		Clients.Remove(Wrapper); Wrapper->DetachOnGT();
		UE_LOG(LogVaRestHttp,Warning,TEXT("VaRest client refused: %s"),*Error); return nullptr;
	}
	return bClosing ? nullptr : Wrapper;
}
UVaRestApiClient* UVaRestHub::GetDefaultClient()
{
	check(IsInGameThread());
	if (bClosing || !Runtime) return nullptr;
	if (DefaultClient) return DefaultClient->GetNativeClient().IsValid() ? DefaultClient.Get() : nullptr;
	const TStrongObjectPtr<UVaRestHub> Self(this);
	auto* Wrapper=NewObject<UVaRestApiClient>(this);
	DefaultClient=Wrapper; Clients.Add(Wrapper); // reserve the default identity before Created can reenter this getter
	++CreatingClients; ON_SCOPE_EXIT { --CreatingClients; };
	FString Error;
	if (!Wrapper->InitializeOnGT(this,nullptr,Runtime.ToSharedRef(),Error))
	{
		if (DefaultClient==Wrapper) DefaultClient=nullptr;
		Clients.Remove(Wrapper); Wrapper->DetachOnGT();
		UE_LOG(LogVaRestHttp,Warning,TEXT("VaRest default client refused: %s"),*Error); return nullptr;
	}
	return bClosing ? nullptr : Wrapper;
}
FVaRestClient UVaRestHub::Api(UVaRestApi* Asset) { const auto* Wrapper=GetClient(Asset); return Wrapper ? Wrapper->GetNativeClient() : FVaRestClient(); }
bool UVaRestHub::SetStorageProfile(const FString& Profile,FString& Error)
{
	const auto Owner=Runtime;
	if (!Owner || bClosing) { Error=TEXT("LifecycleClosed"); return false; }
	if (CreatingClients!=0) { Error=TEXT("ClientCreationInProgress"); return false; }
	return Owner->SetStorageProfileOnGT(Profile,Error);
}
uint64 UVaRestHub::GetScopeId() const { return Runtime ? Runtime->ScopeId() : 0; }
FString UVaRestHub::GetScopeIdText() const { return VaRest::Http::UnsignedIdText(GetScopeId()); }
TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> UVaRestHub::GetConnectivity() const { if (!Runtime) return nullptr; return Runtime->ConnectivityOnGT(); }
TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> UVaRestHub::GetDynamicBudget() const { return Runtime ? Runtime->DynamicBudgetOnGT() : nullptr; }
bool UVaRestHub::IsOnline() const { const auto Value=GetConnectivity(); return !bClosing && Value && Value->IsOnline(); }
void UVaRestHub::ReportOnline() { const auto Value=GetConnectivity(); if (!bClosing && Value) Value->ReportOnline(); }
void UVaRestHub::NotifyEnvironmentPublished(UVaRestApi* Asset) { if (!bClosing) { const TStrongObjectPtr<UVaRestHub> Self(this); OnEnvironmentChanged.Broadcast(Asset); } }
uint64 UVaRestHub::RetainInFlight(UObject* Object,TUniqueFunction<void()>&& Cancel)
{
	check(IsInGameThread());if (bClosing || !Runtime || !IsValid(Object) || InFlightObjects.Num()>=1024 || NextInFlight==0 || !Cancel) return 0;
	const uint64 Registration=NextInFlight++;InFlightObjects.Add(Registration,Object);InFlightCancellation.Add(Registration,MoveTemp(Cancel));return Registration;
}
void UVaRestHub::ReleaseInFlight(uint64 Registration)
{
	check(IsInGameThread());if (Registration==0) return;
	TUniqueFunction<void()> Dropped;if (auto* Cancel=InFlightCancellation.Find(Registration)) Dropped=MoveTemp(*Cancel);
	InFlightCancellation.Remove(Registration);InFlightObjects.Remove(Registration); // disable the entry before callback-capture destruction
}
void UVaRestHub::DeinitializeOnGT()
{
	check(IsInGameThread());
	if (bClosing) return;
	TStrongObjectPtr<UVaRestHub> Self;
	if (!HasAnyFlags(RF_BeginDestroyed)) Self.Reset(this);
	bClosing=true;
	const auto Owner=Runtime;
	if (Owner) Owner->CloseAdmissionOnGT();
	if (Self) OnDeinitializing.Broadcast(); // gates already closed; no user dispatch from an already-unreachable GC backstop
	auto Cancelling=MoveTemp(InFlightCancellation);
	for (auto& Entry:Cancelling)
	{
		UObject* Object=InFlightObjects.FindRef(Entry.Key);TStrongObjectPtr<UObject> Kept;
		if (IsValid(Object)) Kept.Reset(Object);InFlightObjects.Remove(Entry.Key);if (Entry.Value) Entry.Value();
	}
	InFlightObjects.Reset();
	FVaRestJsonRules::OnRulesChanged().Remove(RulesHandle);
	if (Owner) Owner->ConnectivityOnGT()->OnChanged().Remove(ConnectivityHandle);
	for (UVaRestApiClient* Wrapper:Clients) if (Wrapper) Wrapper->DetachOnGT(); // live reflected ownership stays through all callouts
	Clients.Reset(); DefaultClient=nullptr;
	if (Owner) Owner->RetireOnGT();
	Runtime.Reset();
}
void UVaRestHub::BeginDestroy() { DeinitializeOnGT(); Super::BeginDestroy(); }
