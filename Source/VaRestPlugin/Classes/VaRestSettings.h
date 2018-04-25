// Copyright 2018 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSettings.generated.h"

UCLASS(config = Engine, defaultconfig)
class VARESTPLUGIN_API UVaRestSettings : public UObject
{
	GENERATED_UCLASS_BODY()
	
public:
	/**  */
	UPROPERTY(Config, EditAnywhere)
	bool bExtendedLog;
};
