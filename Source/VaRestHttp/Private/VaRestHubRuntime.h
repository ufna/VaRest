// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestClientState.h"
#include "VaRestDynamicBudget.h"
#include "VaRestNativeScope.h"

namespace VaRest::Http
{
/** GT owner of native host state. No strong UObject/World/GI/facade/wrapper reference can enter this object. */
class FHubRuntime final : public TSharedFromThis<FHubRuntime,ESPMode::ThreadSafe>
{
public:
	static TSharedPtr<FHubRuntime,ESPMode::ThreadSafe> CreateOnGT(FVaRestTraceScope Info,FString Profile,FString& Error);
	~FHubRuntime();
	FVaRestClient CreateClientOnGT(FClientConfig&& Initial,FString& Error,TFunction<void(const FVaRestClient&)> BeforeCreated={});
	bool SetStorageProfileOnGT(FString Profile,FString& Error);
	void CloseAdmissionOnGT();
	void RetireOnGT();
	bool IsClosedOnGT() const { check(IsInGameThread()); return bClosed; }
	uint64 ScopeId() const { return Scope->GetId(); }
	const FString& StorageProfileOnGT() const { check(IsInGameThread()); return Profile; }
	TSharedRef<FVaRestConnectivity,ESPMode::ThreadSafe> ConnectivityOnGT() const { check(IsInGameThread()); return Connectivity.ToSharedRef(); }
	TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> DynamicBudgetOnGT() const { check(IsInGameThread()); return DynamicBudget; }
	static bool ValidatePartition(FStringView Text,bool bAllowEmpty,FString& Error);
	static bool QueueName(FStringView ApiPath,FStringView StorageProfile,FStringView Environment,FStringView Account,FString& Name,FString& Error);
private:
	TSharedPtr<FNativeScopeLifetime,ESPMode::ThreadSafe> Scope;
	TSharedPtr<FVaRestConnectivity,ESPMode::ThreadSafe> Connectivity;
	TSharedPtr<FVaRestDynamicBudget,ESPMode::ThreadSafe> DynamicBudget;
	TArray<FVaRestClient> Clients;
	FString Profile;
	bool bClosed=false,bRetired=false,bEverAdmitted=false;
	uint32 Creating=0;
};
}
