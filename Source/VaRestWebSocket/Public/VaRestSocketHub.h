// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestSocket.h"
#include "VaRestSocketHub.generated.h"

class UVaRestHub;
class UVaRestApi;
class UVaRestSocketClient;
class UVaRestSocketEndpointBase;
class UVaRestSocketHub;

USTRUCT()
struct FVaRestSocketAssetBinding
{
	GENERATED_BODY()
	UPROPERTY(Transient) TObjectPtr<UVaRestApi> Api;
	UPROPERTY(Transient) FName Entry;
	FVaRestSocket Socket;
	/** GT-owned protocol hook, released with the binding before Core Http Stop. */
	TFunction<void(UVaRestSocketHub&, UVaRestApi*, FName, FVaRestSocket&)> EnvironmentChanged;
};

/** Socket facade owned by a real GI subsystem, or explicitly by the engine's editor fallback subsystem.
 * Like UVaRestHub, one facade type serves both scopes; it never fabricates a GameInstance. */
UCLASS(BlueprintType)
class VARESTWEBSOCKET_API UVaRestSocketHub : public UObject
{
	GENERATED_BODY()
public:
	/** No implicit engine fallback: a missing game instance returns null. */
	UFUNCTION(BlueprintPure, Category="VaRest|Socket", meta=(WorldContext="WorldContextObject"))
	static UVaRestSocketHub* Get(const UObject* WorldContextObject);
	UFUNCTION(BlueprintPure, Category="VaRest|Socket") static UVaRestSocketHub* GetForEditor();
	FVaRestSocket Make(FStringView Url);
	FVaRestSocket Make(FName Name, FStringView Url);
	FVaRestSocket Find(FName Name) const;
	/** Native sibling-protocol factory. Initialization runs once on GT before the identity is published. */
	FVaRestSocket MakeForProtocol(FName Name, FStringView Url, FName Protocol, TFunctionRef<void(FVaRestSocket&)> Initialize);
	FVaRestSocket FindForProtocol(FName Name, FName Protocol) const;
	FVaRestSocket Socket(UVaRestApi* Api, FName Entry, FVaRestError& Error);
	/** API/entry identity is separate from code-first names, and shared by all protocol accessors. */
	FVaRestSocket SocketForProtocol(UVaRestApi* Api, FName Entry, FName Protocol, FVaRestError& Error,
		TFunctionRef<bool(FVaRestSocket&, const UVaRestSocketEndpointBase&, FStringView)> Initialize,
		TFunction<void(UVaRestSocketHub&, UVaRestApi*, FName, FVaRestSocket&)> EnvironmentChanged = {});
	/** One weak-cached UObject view per native identity; callers own the returned view through ordinary GC references. */
	UVaRestSocketClient* Wrap(FVaRestSocket Native);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") UVaRestSocketClient* GetSocket(UVaRestApi* Api, FName Entry, FVaRestError& Error);
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void ReleaseSocket(UVaRestApi* Api, FName Entry);
	void Release(FName Name);
	TArray<FVaRestSocket> GetSockets() const;
	UFUNCTION(BlueprintCallable, Category="VaRest|Socket") void CloseAll();
	bool InitializeOnGT(UVaRestHub* CoreHub);
	#if WITH_DEV_AUTOMATION_TESTS
	/** Configure an owned test Hub before its first identity; both protocol factories use the same injected dependencies. */
	bool ConfigureTestingOnGT(TSharedPtr<IVaRestSocketTransport, ESPMode::ThreadSafe> Transport,
		TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> Clock, int32 Seed = 1);
	#endif
	void ShutdownOnGT();
	UVaRestHub* GetCoreHubOnGT() const;
	virtual void BeginDestroy() override;
private:
	UFUNCTION() void OnCoreDeinitializing();
	UFUNCTION() void OnEnvironmentChanged(UVaRestApi* Api);
	TWeakObjectPtr<UVaRestHub> CoreHub;
	TMap<FName, FVaRestSocket> Named;
	TSet<FName> CreatingNamed;
	TArray<TPair<TWeakObjectPtr<UVaRestApi>, FName>> CreatingAssets;
	TArray<FVaRestSocketWeak> All;
	UPROPERTY(Transient) TArray<FVaRestSocketAssetBinding> AssetSockets;
	TArray<TWeakObjectPtr<UVaRestSocketClient>> BlueprintClients;
	bool bClosing = false;
	#if WITH_DEV_AUTOMATION_TESTS
	TSharedPtr<IVaRestSocketTransport, ESPMode::ThreadSafe> TestTransport;
	TSharedPtr<IVaRestClock, ESPMode::ThreadSafe> TestClock;
	int32 TestSeed = 0;
	#endif
};
