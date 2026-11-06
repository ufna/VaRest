// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiUserSettings.h"
#include "VaRestOpenApiUrlPolicy.h"
#include "VaRestOpenApiAnalysis.h"

TArray<FVaRestOpenApiFetchHeader> UVaRestOpenApiUserSettings::HeadersFor(const FString& Uri) const
{
	using namespace VaRest::OpenApi;check(IsInGameThread());TArray<FVaRestOpenApiFetchHeader> Out;const auto Url=FSourceUrl::Parse(Uri);if (!Url.IsSet()) return Out;
	for (const auto& Host:Hosts)
	{
		const auto Origin=FSourceUrl::Parse(Host.Origin.Contains(TEXT("://")) ? Host.Origin : TEXT("https://")+Host.Origin);
		if (Origin.IsSet() && Origin.GetValue().Path==TEXT("/") && Origin.GetValue().Query.IsEmpty() && Equal(Origin.GetValue().Origin,Url.GetValue().Origin)) Out.Append(Host.Headers);
	}
	return Out;
}
