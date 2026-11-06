// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once

#include "BlueprintNodeSpawner.h"
#include "K2Node_VaRestSioCall.h"
#include "VaRestSioNodeSpawner.generated.h"

UCLASS(Transient)
class UVaRestSioNodeSpawner final : public UBlueprintNodeSpawner
{
	GENERATED_BODY()
public:
	static UVaRestSioNodeSpawner* Create(UVaRestApi* Api, FName Entry, EVaRestSioCall Operation,
		const FString& Namespace = TEXT(""), const FString& Event = TEXT(""), bool bDeclaration = false, const FString& Description = TEXT(""));
	virtual FBlueprintNodeSignature GetSpawnerSignature() const override;
private:
	UPROPERTY() TObjectPtr<UVaRestApi> Api;
	UPROPERTY() FName Entry;
	UPROPERTY() FString Namespace;
	UPROPERTY() FString Event;
	UPROPERTY() EVaRestSioCall Operation = EVaRestSioCall::Get;
	UPROPERTY() bool bDeclaration = false;
};

namespace VaRestEditor { FDelegateHandle RegisterSioEndpointProvider(); }
