// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiModel.h"

namespace VaRest::OpenApi
{
struct FSourceUrl
{
	FString Uri,Origin,Path,Query,Host,Base;
	static TOptional<FSourceUrl> Parse(FString Value);
};
/** Never performs IO. Checks canonical origin and directory prefix before a transport can connect. */
bool AllowSourceUrl(const FVaRestOpenApiReadRequest& Request,FSourceUrl& Out,FString& Error);
TOptional<FSourceUrl> SourceRedirect(const FVaRestOpenApiReadRequest& Request,const FString& Current,const FString& Location,FString& Error);
}
