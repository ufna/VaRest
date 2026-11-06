// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.
#include "VaRestOpenApiReimportValues.h"
#include "VaRestBuiltinAuthConfigs.h"

namespace VaRest::OpenApi
{
TOptional<FVaRestOpenApiPlannedType> PlannedType(const FVaRestJson& Json)
{
	static const TCHAR* Kinds[]={TEXT("None"),TEXT("Json"),TEXT("Struct"),TEXT("Enum"),TEXT("String"),TEXT("Bytes"),TEXT("Bool"),TEXT("Int"),TEXT("Int64"),TEXT("Double"),TEXT("DateTime")};
	FVaRestOpenApiPlannedType Type;bool Found=false;
	for (int32 Index=0;Index<UE_ARRAY_COUNT(Kinds);++Index) if (Equal(Json[TEXT("kind")].AsString(),Kinds[Index])) { Type.Kind=static_cast<EVaRestOpenApiValueKind>(Index);Found=true;break; }
	if (!Found) return {};
	const FString Container=Json[TEXT("container")].AsString();
	if (Equal(Container,TEXT("None"))) Type.Container=EVaRestOpenApiContainer::None;
	else if (Equal(Container,TEXT("Array"))) Type.Container=EVaRestOpenApiContainer::Array;
	else if (Equal(Container,TEXT("Map"))) Type.Container=EVaRestOpenApiContainer::Map;else return {};
	Type.DefinitionId=Json[TEXT("definition")].AsString();Type.bJsonValue=Json[TEXT("jsonValue")].AsBool();return Type;
}
void ReadEnvelope(const FVaRestJson& Json,FVaRestEnvelope& Out)
{
	Out.DataPath=Json[TEXT("dataPath")].AsString();Out.SuccessPath=Json[TEXT("successPath")].AsString();Out.SuccessValues.Reset();
	for (const auto& Value:Json[TEXT("successValues")].Elements()) Out.SuccessValues.Add(Value.AsString());
	Out.ErrorMessagePath=Json[TEXT("errorMessagePath")].AsString();Out.ErrorCodePath=Json[TEXT("errorCodePath")].AsString();Out.bParseErrorBodies=Json[TEXT("parseErrorBodies")].AsBool();
}
void ReadAuth(const FVaRestJson& Json,UVaRestApi& Api)
{
	const FString Kind=Json[TEXT("kind")].AsString();
	if (Kind.StartsWith(TEXT("user:"),ESearchCase::CaseSensitive)) return; // untouched custom user auth
	UClass* Class=nullptr;
	if (Equal(Kind,TEXT("Bearer"))) Class=UVaRestBearerAuthConfig::StaticClass();
	else if (Equal(Kind,TEXT("Basic"))) Class=UVaRestBasicAuthConfig::StaticClass();
	else if (Equal(Kind,TEXT("ApiKey"))) Class=UVaRestApiKeyAuthConfig::StaticClass();
	else if (Equal(Kind,TEXT("BearerRefresh"))) Class=UVaRestBearerRefreshAuthConfig::StaticClass();
	else if (Equal(Kind,TEXT("ClientCredentials"))) Class=UVaRestClientCredentialsAuthConfig::StaticClass();
	if (!Class) { if (Equal(Kind,TEXT("None"))) Api.Auth=nullptr;return; }
	if (!Api.Auth || Api.Auth->GetClass()!=Class) Api.Auth=NewObject<UVaRestAuthConfig>(&Api,Class);
	Api.Auth->Modify();
	if (auto* Key=Cast<UVaRestApiKeyAuthConfig>(Api.Auth)) { Key->Name=Json[TEXT("name")].AsString();Key->Placement=Equal(Json[TEXT("placement")].AsString(),TEXT("query")) ? EVaRestApiKeyPlacement::Query : EVaRestApiKeyPlacement::Header; }
	if (auto* Refresh=Cast<UVaRestBearerRefreshAuthConfig>(Api.Auth))
	{
		Refresh->Config.RefreshPath=Json[TEXT("tokenUrl")].AsString();Refresh->Config.Style=NativeEnum(Json[TEXT("style")],Refresh->Config.Style);
		Refresh->Config.AccessTokenPath=Json[TEXT("accessTokenPath")].AsString();Refresh->Config.RefreshTokenPath=Json[TEXT("refreshTokenPath")].AsString();Refresh->Config.ExpiresInPath=Json[TEXT("expiresInPath")].AsString();
	}
	if (auto* Credentials=Cast<UVaRestClientCredentialsAuthConfig>(Api.Auth)) { Credentials->Config.TokenPath=Json[TEXT("tokenUrl")].AsString();Credentials->Config.AccessTokenPath=Json[TEXT("accessTokenPath")].AsString();Credentials->Config.ExpiresInPath=Json[TEXT("expiresInPath")].AsString(); }
	// Secrets, client IDs, scopes and every auth policy absent from the source remain application/user-owned.
}
}
