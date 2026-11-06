// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestApiTypes.h"

namespace VaRest::Http::Editor
{
/** Editor GT only. JSON array syntax; resulting strings are scalar K2 defaults, not retained property addresses. */
bool ArrayParameterDefaults(const FVaRestTypeRef& Type,const FString& Text,TArray<FString>& Elements,FString& Error);
}
