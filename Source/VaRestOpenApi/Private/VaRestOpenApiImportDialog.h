// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiModel.h"
class UVaRestOpenApiImportOptions;
struct FVaRestOpenApiReimportPreview;
namespace VaRest::OpenApi
{
bool ShowImportOptions(const FVaRestOpenApiAnalysis& Analysis,UVaRestOpenApiImportOptions& Options,bool bDefaultIncludeNew=true);
/** URL only when Destination is null (changing source); otherwise also choose a complete new asset package. */
bool ShowSourceUrl(FString& Url,FString* Destination=nullptr);
bool ShowReimportPreview(FVaRestOpenApiReimportPreview& Preview,const FVaRestOpenApiAnalysis& Analysis,UVaRestOpenApiImportOptions& Options,bool& bChangeOptions);
}
