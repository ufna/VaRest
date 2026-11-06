// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "VaRestClient.h"
#include "VaRestRequestHandle.generated.h"

/** Weak request control. Keeping this UObject does not keep either an action or a native request alive. */
UCLASS(BlueprintType)
class VARESTHTTP_API UVaRestRequestHandle : public UObject
{
	GENERATED_BODY()
public:
	UFUNCTION(BlueprintCallable,Category="VaRest|Http") void Cancel() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") bool IsActive() const;
	UFUNCTION(BlueprintPure,Category="VaRest|Http") FString GetIdText() const;
	uint64 GetId() const { return Native.GetId(); }
private:
	friend class UVaRestRequestAction;
	FVaRestRequestHandle Native;
};
