// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestSioSettings.h"

UVaRestSioSettings::UVaRestSioSettings()
{
	CategoryName = TEXT("Plugins"); SectionName = TEXT("VaRestSocketIO");
	Reconnect.BaseDelay = 1.0; Reconnect.Multiplier = 2.0; Reconnect.MaxDelay = 5.0;
	Reconnect.Jitter = EVaRestSocketJitter::Ratio; Reconnect.JitterRatio = 0.5;
	Queue.MaxQueuedBytes = 4 * 1024 * 1024;
}
