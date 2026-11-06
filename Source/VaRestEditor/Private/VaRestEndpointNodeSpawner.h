// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "BlueprintNodeSpawner.h"
#include "VaRestEndpointNodeSpawner.generated.h"

class UVaRestApi;
struct FVaRestApiEndpoint;

UCLASS(Transient)
class UVaRestEndpointNodeSpawner final : public UBlueprintNodeSpawner
{
	GENERATED_BODY()
public:
	static UVaRestEndpointNodeSpawner* CreateEndpoint(UVaRestApi& Api,const FVaRestApiEndpoint& Endpoint);
	virtual FBlueprintNodeSignature GetSpawnerSignature() const override;
private:
	UPROPERTY() TObjectPtr<UVaRestApi> Api;
	UPROPERTY() FGuid EndpointId;
};

namespace VaRestEditor
{
FDelegateHandle RegisterHttpEndpointProvider();
}
