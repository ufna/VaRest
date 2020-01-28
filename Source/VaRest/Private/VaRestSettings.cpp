// Copyright 2014-2019 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSettings.h"

UVaRestSettings::UVaRestSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	bExtendedLog = false;
	bUseChunkedParser = false;
}
