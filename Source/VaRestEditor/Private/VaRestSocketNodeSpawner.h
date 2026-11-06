// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "BlueprintNodeSpawner.h"
#include "K2Node_VaRestSocketCall.h"
#include "VaRestSocketNodeSpawner.generated.h"

UCLASS(Transient)
class UVaRestSocketNodeSpawner final : public UBlueprintNodeSpawner
{
	GENERATED_BODY()
public:
	static UVaRestSocketNodeSpawner* Create(UVaRestApi* Api, FName Entry, EVaRestSocketCall Operation,
		const FString& Channel = TEXT(""), const FString& Route = TEXT(""), bool bDeclaration = false, const FString& Description = TEXT(""));
	virtual FBlueprintNodeSignature GetSpawnerSignature() const override;
private:
	UPROPERTY() TObjectPtr<UVaRestApi> Api;
	UPROPERTY() FName Entry;
	UPROPERTY() FString Channel;
	UPROPERTY() FString Route;
	UPROPERTY() EVaRestSocketCall Operation = EVaRestSocketCall::Get;
	UPROPERTY() bool bDeclaration = false;
};

namespace VaRestEditor { FDelegateHandle RegisterSocketEndpointProvider(); }
