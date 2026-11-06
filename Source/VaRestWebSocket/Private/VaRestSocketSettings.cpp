// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSocketSettings.h"

UVaRestSocketSettings::UVaRestSocketSettings()
{
	CategoryName = TEXT("Plugins");
	SectionName = TEXT("VaRestWebSocket");
#if PLATFORM_ANDROID || PLATFORM_IOS
	Background = EVaRestSocketBackground::Disconnect;
#endif
}
