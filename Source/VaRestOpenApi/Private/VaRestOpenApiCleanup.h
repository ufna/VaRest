// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiMerge.h"
class UVaRestApi;
namespace VaRest::OpenApi
{
struct FCleanupRow { FVaRestOpenApiMergeItem Item;FString Referencers;bool bSelected=false; };
bool ShowCleanupPicker(TArray<TSharedPtr<FCleanupRow>>& Rows);
int32 CleanupRemoved(UVaRestApi* Api);
}
