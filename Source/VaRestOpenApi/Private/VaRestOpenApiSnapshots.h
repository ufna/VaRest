// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#pragma once
#include "VaRestOpenApiMerge.h"
#include "VaRestOpenApiAnalysis.h"
#include "VaRestApi.h"

namespace VaRest::OpenApi
{
FVaRestJson SnapshotType(const FVaRestTypeRef& Type,const TStringMap<FString>& Definitions);
FVaRestJson SnapshotEnvelope(const FVaRestEnvelope& Envelope);
FVaRestJson SnapshotEndpoint(const FVaRestApiEndpoint& Endpoint,const TStringMap<FString>& Definitions);
FVaRestJson SnapshotParameter(const FVaRestEndpointParam& Param,const TStringMap<FString>& Definitions);
FVaRestJson SnapshotAuth(const UVaRestAuthConfig* Auth);
FVaRestJson OwnedProperties(const FVaRestJson& Row,std::initializer_list<const TCHAR*> Keys);
FString EnumValueId(const FString& Owner,const FString& Wire,bool Unset);
FVaRestJson EndpointIdentityHistory(const FVaRestJson& Previous,const FVaRestJson& Plan);
bool SafeAssetPath(const FString& Path);
}
