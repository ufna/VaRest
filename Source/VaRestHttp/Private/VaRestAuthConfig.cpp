// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuthConfig.h"
#include "VaRestApi.h"

void UVaRestAuthConfig::SetRuntimeApiOnGT(const UVaRestApi* Api) { check(IsInGameThread()); RuntimeApi=Api; }
const UVaRestApi* UVaRestAuthConfig::GetApiOnGT() const
{
	check(IsInGameThread());
	return RuntimeApi.IsValid() ? RuntimeApi.Get() : GetTypedOuter<UVaRestApi>();
}
