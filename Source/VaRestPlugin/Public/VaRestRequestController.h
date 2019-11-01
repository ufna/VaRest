// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestPluginDefines.h"

#include "VaRestRequestController.generated.h"

UCLASS()
class VARESTPLUGIN_API UVaRestRequestController : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** */
	UFUNCTION(BlueprintCallable, Category = "VaRest|Controller")
	void Initialize();
};
