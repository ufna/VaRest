// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestHttpSettings.h"

UVaRestHttpSettings::UVaRestHttpSettings(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// UDeveloperSettings reads these protected members (DeveloperSettings.h:63-66)
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("VaRestHttp");
}
