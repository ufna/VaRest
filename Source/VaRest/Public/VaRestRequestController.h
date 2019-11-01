// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestDefines.h"

#include "VaRestRequestController.generated.h"

UCLASS()
class VAREST_API UVaRestRequestController : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Controller")
	void Initialize();
};
