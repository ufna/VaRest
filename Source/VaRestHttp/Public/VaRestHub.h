// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestApiClient.h"
#include "VaRestConnectivity.h"
#include "VaRestDynamicResult.h"
#include "VaRestHub.generated.h"

class UGameInstance;
namespace VaRest::Http { class FHubRuntime; }
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestHubConnectivityChanged,bool,bOnline);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FVaRestHubEnvironmentChanged,UVaRestApi*,Api);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FVaRestHubDeinitializing);

/** Shared facade for a real GI or the engine fallback. It is never rooted and never fabricates a game instance. */
UCLASS(BlueprintType)
class VARESTHTTP_API UVaRestHub : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintPure,Category="VaRest",meta=(WorldContext="WorldContextObject")) static UVaRestHub* Get(const UObject* WorldContextObject);
	UFUNCTION(BlueprintPure,Category="VaRest") static UVaRestHub* GetForEditor();
	UFUNCTION(BlueprintCallable,Category="VaRest|API") UVaRestApiClient* GetClient(UVaRestApi* Api);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http") UVaRestApiClient* MakeClient(const FString& BaseUrl);
	UFUNCTION(BlueprintCallable,Category="VaRest|Http") UVaRestApiClient* GetDefaultClient();
	UFUNCTION(BlueprintCallable,Category="VaRest|Storage") bool SetStorageProfile(const FString& Profile,FString& Error);
	UFUNCTION(BlueprintPure,Category="VaRest") FString GetScopeIdText() const;
	UFUNCTION(BlueprintPure,Category="VaRest") bool IsOnline() const;
	UFUNCTION(BlueprintCallable,Category="VaRest") void ReportOnline();
	UPROPERTY(BlueprintAssignable,Category="VaRest") FVaRestHubConnectivityChanged OnConnectivityChanged;
	UPROPERTY(BlueprintAssignable,Category="VaRest") FVaRestHubEnvironmentChanged OnEnvironmentChanged;
	UPROPERTY(BlueprintAssignable,Category="VaRest") FVaRestHubDeinitializing OnDeinitializing;
	FVaRestClient Api(UVaRestApi* Asset);
	TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> GetConnectivity() const;
	TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> GetDynamicBudget() const;
	uint64 GetScopeId() const;
	/** GT adapters (including legacy requests): retain the UObject through its logical completion, without AddToRoot.
	 * At most 1024 retained entries; zero refuses admission. Cancellation is called once at Hub teardown. */
	uint64 RetainInFlight(UObject* Object,TUniqueFunction<void()>&& Cancel);
	void ReleaseInFlight(uint64 Registration);
	bool InitializeOnGT(UGameInstance* GameInstance,bool bEditorFallback,FString& Error);
	void DeinitializeOnGT();
	void NotifyEnvironmentPublished(UVaRestApi* Asset);
	/** Editor PreBeginPIE creates ONE shared transient play identity for all real GIs of that play. */
	static void BeginPlaySessionOnGT();
	virtual void BeginDestroy() override;
private:
	UPROPERTY(Transient) TArray<TObjectPtr<UVaRestApiClient>> Clients;
	UPROPERTY(Transient) TObjectPtr<UVaRestApiClient> DefaultClient;
	UPROPERTY(Transient) TMap<uint64,TObjectPtr<UObject>> InFlightObjects;
	TMap<uint64,TUniqueFunction<void()>> InFlightCancellation;
	uint64 NextInFlight=1;
	TSharedPtr<VaRest::Http::FHubRuntime,ESPMode::ThreadSafe> Runtime;
	FDelegateHandle ConnectivityHandle,RulesHandle;
	bool bClosing=false;
	uint32 CreatingClients=0;
};
