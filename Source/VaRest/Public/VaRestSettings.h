// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#pragma once

#include "VaRestSettings.generated.h"

UCLASS(config = Engine, defaultconfig)
class VAREST_API UVaRestSettings : public UObject
{
	GENERATED_UCLASS_BODY()

public:
	/** You can disable request content logging to avoid security vulnerability */
	UPROPERTY(Config, EditAnywhere, Category = "VaRest")
	bool bExtendedLog;

	/** Use custom chunked parses (best for memory, but has issues with hex-encoded utf-8) */
	UPROPERTY(Config, EditAnywhere, Category = "VaRest")
	bool bUseChunkedParser;
};
