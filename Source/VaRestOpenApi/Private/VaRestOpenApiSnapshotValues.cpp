// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiSnapshots.h"
#include "VaRestOpenApiPlanning.h"
#include "VaRestBuiltinAuthConfigs.h"
#include "Misc/PackageName.h"

namespace VaRest::OpenApi
{
bool SafeAssetPath(const FString& Path)
{
	return Path.Len()<2*NAME_SIZE && FPackageName::ObjectPathToPackageName(Path).Len()<NAME_SIZE && FPackageName::ObjectPathToObjectName(Path).Len()<NAME_SIZE && FPackageName::IsValidObjectPath(Path);
}
FVaRestJson OwnedProperties(const FVaRestJson& Row,std::initializer_list<const TCHAR*> Keys)
{
	auto Out=FVaRestJson::Object();for (const auto* Key:Keys) Out.Set(Key,Row[Key]);return Out;
}
FString EnumValueId(const FString& Owner,const FString& Wire,bool Unset) { return SemanticId(Owner,Unset ? TEXT("unset") : TEXT("value"),Wire); }
FVaRestJson EndpointIdentityHistory(const FVaRestJson& Previous,const FVaRestJson& Plan)
{
	auto Rows=FVaRestJson::Array();TStringMap<int32> Indices;
	for (const auto& Row:Previous.Elements()) { const FString Id=Row[TEXT("id")].AsString();Indices.Add(Id,Rows.Num());Rows.Add(OwnedProperties(Row,{TEXT("id"),TEXT("operationId"),TEXT("verb"),TEXT("path")})); }
	for (const auto& Row:Plan[TEXT("endpoints")].Elements())
	{
		const FString Id=Row[TEXT("id")].AsString();auto Identity=OwnedProperties(Row,{TEXT("id"),TEXT("operationId"),TEXT("verb"),TEXT("path")});
		if (const auto* Index=Indices.Find(Id)) Rows.SetAt(*Index,Identity);else { Indices.Add(Id,Rows.Num());Rows.Add(Identity); }
	}
	return Rows;
}
FVaRestJson SnapshotType(const FVaRestTypeRef& Type,const TStringMap<FString>& Definitions)
{
	const FString Kind=StaticEnum<EVaRestTypeKind>()->GetNameStringByValue(static_cast<int64>(Type.Kind));
	const FString Container=StaticEnum<EVaRestTypeContainer>()->GetNameStringByValue(static_cast<int64>(Type.Container));FString Definition;
	if (Type.Type) { const FString Path=Type.Type->GetPathName();const auto* Found=Definitions.Find(Path);Definition=Found ? *Found : TEXT("user:")+Path; }
	return FVaRestJson::Object().Set(TEXT("kind"),Kind).Set(TEXT("container"),Container).Set(TEXT("definition"),Definition).Set(TEXT("jsonValue"),Type.bJsonValue);
}
FVaRestJson SnapshotEnvelope(const FVaRestEnvelope& Envelope)
{
	auto Values=FVaRestJson::Array();for (const auto& Value:Envelope.SuccessValues) Values.Add(Value);
	return FVaRestJson::Object().Set(TEXT("dataPath"),Envelope.DataPath).Set(TEXT("successPath"),Envelope.SuccessPath).Set(TEXT("successValues"),Values)
		.Set(TEXT("errorMessagePath"),Envelope.ErrorMessagePath).Set(TEXT("errorCodePath"),Envelope.ErrorCodePath).Set(TEXT("parseErrorBodies"),Envelope.bParseErrorBodies);
}
FVaRestJson SnapshotEndpoint(const FVaRestApiEndpoint& Endpoint,const TStringMap<FString>& Definitions)
{
	return FVaRestJson::Object().Set(TEXT("name"),Endpoint.Name.ToString()).Set(TEXT("verb"),Endpoint.Verb).Set(TEXT("path"),Endpoint.Path).Set(TEXT("category"),Endpoint.Category).Set(TEXT("description"),Endpoint.Description)
		.Set(TEXT("deprecated"),Endpoint.bDeprecated).Set(TEXT("deprecationMessage"),Endpoint.DeprecationMessage).Set(TEXT("idempotent"),Endpoint.bIdempotent)
		.Set(TEXT("requestType"),SnapshotType(Endpoint.RequestType,Definitions)).Set(TEXT("responseType"),SnapshotType(Endpoint.ResponseType,Definitions)).Set(TEXT("errorType"),SnapshotType(Endpoint.ErrorType,Definitions))
		.Set(TEXT("bodyKind"),StaticEnum<EVaRestEndpointBodyKind>()->GetNameStringByValue(static_cast<int64>(Endpoint.BodyKind))).Set(TEXT("contentType"),Endpoint.BodyContentType)
		.Set(TEXT("authMode"),StaticEnum<EVaRestAuthMode>()->GetNameStringByValue(static_cast<int64>(Endpoint.AuthMode))).Set(TEXT("overrideEnvelope"),Endpoint.bOverrideEnvelope).Set(TEXT("envelope"),SnapshotEnvelope(Endpoint.Envelope));
}
FVaRestJson SnapshotParameter(const FVaRestEndpointParam& Param,const TStringMap<FString>& Definitions)
{
	return FVaRestJson::Object().Set(TEXT("wire"),Param.Name).Set(TEXT("pin"),Param.PinName.ToString()).Set(TEXT("in"),Param.In==EVaRestEndpointParamLocation::Path ? TEXT("path") : Param.In==EVaRestEndpointParamLocation::Header ? TEXT("header") : TEXT("query"))
		.Set(TEXT("type"),SnapshotType(Param.Type,Definitions)).Set(TEXT("required"),Param.bRequired).Set(TEXT("defaultText"),Param.Default).Set(TEXT("description"),Param.Description)
		.Set(TEXT("style"),StaticEnum<EVaRestQueryStyle>()->GetNameStringByValue(static_cast<int64>(Param.Style))).Set(TEXT("removed"),Param.bRemoved);
}
FVaRestJson SnapshotAuth(const UVaRestAuthConfig* Auth)
{
	// Runtime/development credentials are deliberately outside importer ownership and never enter the manifest.
	auto Out=FVaRestJson::Object();
	if (!Auth) return Out.Set(TEXT("kind"),TEXT("None"));
	if (const auto* Key=Cast<UVaRestApiKeyAuthConfig>(Auth)) return Out.Set(TEXT("kind"),TEXT("ApiKey")).Set(TEXT("name"),Key->Name).Set(TEXT("placement"),Key->Placement==EVaRestApiKeyPlacement::Query ? TEXT("query") : TEXT("header"));
	if (const auto* Refresh=Cast<UVaRestBearerRefreshAuthConfig>(Auth)) return Out.Set(TEXT("kind"),TEXT("BearerRefresh")).Set(TEXT("tokenUrl"),Refresh->Config.RefreshPath)
		.Set(TEXT("style"),StaticEnum<EVaRestRefreshStyle>()->GetNameStringByValue(static_cast<int64>(Refresh->Config.Style))).Set(TEXT("accessTokenPath"),Refresh->Config.AccessTokenPath).Set(TEXT("refreshTokenPath"),Refresh->Config.RefreshTokenPath).Set(TEXT("expiresInPath"),Refresh->Config.ExpiresInPath);
	if (const auto* Credentials=Cast<UVaRestClientCredentialsAuthConfig>(Auth)) return Out.Set(TEXT("kind"),TEXT("ClientCredentials")).Set(TEXT("tokenUrl"),Credentials->Config.TokenPath).Set(TEXT("accessTokenPath"),Credentials->Config.AccessTokenPath).Set(TEXT("expiresInPath"),Credentials->Config.ExpiresInPath);
	if (Auth->IsA<UVaRestBasicAuthConfig>()) return Out.Set(TEXT("kind"),TEXT("Basic"));
	if (Auth->IsA<UVaRestBearerAuthConfig>()) return Out.Set(TEXT("kind"),TEXT("Bearer"));
	return Out.Set(TEXT("kind"),TEXT("user:")+Auth->GetClass()->GetPathName());
}
}
