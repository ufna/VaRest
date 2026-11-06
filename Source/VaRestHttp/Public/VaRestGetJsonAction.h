// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestRequestAction.h"
#include "VaRestGetJsonAction.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FVaRestActionSuccessJson,const FVaRestJson&,Json,const FVaRestResponseData&,Http);

/** Plain immutable JSON may be shared by every metadata listener; clone before independent mutation. */
UCLASS(BlueprintType,meta=(ExposedAsyncProxy="Action"))
class VARESTHTTP_API UVaRestGetJsonAction : public UVaRestRequestAction
{
	GENERATED_BODY()
public:
	UPROPERTY(BlueprintAssignable,Category="VaRest|Http") FVaRestActionSuccessJson OnSuccessJson;
protected:
	virtual void BroadcastSuccessOnGT(const FVaRestDynamicResult& Result) override;
};
