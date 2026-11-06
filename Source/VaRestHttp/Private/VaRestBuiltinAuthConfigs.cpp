// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestBuiltinAuthConfigs.h"

#include "VaRestApi.h"
#include "VaRestAuth.h"
#include "VaRestHttpLog.h"
#include "VaRestUrl.h"

namespace VaRest::Http::AuthConfigPrivate
{
void AddAuthConfigError(FVaRestApiValidationContext& Context, FName Code, const TCHAR* Field, const TCHAR* Message)
{
	Context.Error(Code, Field, FText::FromString(Message));
}

void ValidateEndpoint(const UVaRestAuthConfig& Config, FName Name, const TCHAR* Field, FVaRestApiValidationContext& Context)
{
	if (Name.IsNone())
		return;
	const UVaRestApi* Api = Config.GetApiOnGT();
	if (!Api)
	{
		Context.Error(TEXT("Auth.Endpoint"), Field, FText::FromString(TEXT("A named auth endpoint needs its owning API asset")));
		return;
	}
	const auto Resolved = Api->ResolveEndpointPath(Name);
	if (Resolved.HasError())
		Context.Error(TEXT("Auth.Endpoint"), Field, FText::FromString(Resolved.GetError()));
}

bool Ready(FName Requested, FName Resolved)
{
	if (Requested.IsNone() || Requested == Resolved)
		return true;
	UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: resolve the auth endpoint on the per-client config before CreateAuth"));
	return false;
}
} // namespace VaRest::Http::AuthConfigPrivate

TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> UVaRestBearerAuthConfig::CreateAuth() const
{
	return FVaRestAuth::Bearer();
}

void UVaRestBearerAuthConfig::Validate(FVaRestApiValidationContext&) const
{
}

TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> UVaRestApiKeyAuthConfig::CreateAuth() const
{
	return FVaRestAuth::ApiKey(Placement, Name, Key);
}

void UVaRestApiKeyAuthConfig::Validate(FVaRestApiValidationContext& Context) const
{
	using namespace VaRest::Http::AuthConfigPrivate;
	if (Placement != EVaRestApiKeyPlacement::Header && Placement != EVaRestApiKeyPlacement::Query)
		AddAuthConfigError(Context, TEXT("Auth.Placement"), TEXT("Auth.Placement"), TEXT("Unknown API key placement"));
	if (Name.IsEmpty() || (Placement == EVaRestApiKeyPlacement::Header && !VaRest::Http::IsValidHeaderName(Name)))
		AddAuthConfigError(Context, TEXT("Auth.KeyName"), TEXT("Auth.Name"), TEXT("API key needs a usable name; a header name must be an HTTP token"));
	if (!VaRest::Http::IsValidHeaderValue(Key))
		AddAuthConfigError(Context, TEXT("Auth.KeyValue"), TEXT("Auth.Key"), TEXT("API key cannot contain header controls"));
	if (Key.IsEmpty())
		Context.Warning(TEXT("Auth.RuntimeCredentials"), TEXT("Auth.Key"),
			FText::FromString(TEXT("Supply this API key at runtime; requests are refused until a usable key is set")));
}

TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> UVaRestBasicAuthConfig::CreateAuth() const
{
	return FVaRestAuth::Basic(User, Password);
}

void UVaRestBasicAuthConfig::Validate(FVaRestApiValidationContext& Context) const
{
	if (User.Contains(TEXT(":")))
		VaRest::Http::AuthConfigPrivate::AddAuthConfigError(Context, TEXT("Auth.BasicUser"), TEXT("Auth.User"), TEXT("A Basic auth user cannot contain ':'"));
}

bool UVaRestBearerRefreshAuthConfig::ResolveEndpoint(const UVaRestApi& Api, FString& OutError)
{
	ResolvedEndpoint = NAME_None;
	OutError.Reset();
	if (RefreshEndpoint.IsNone())
	{
		Config.TraceEndpoint = NAME_None;
		return true;
	}
	const auto Path = Api.ResolveEndpointPath(RefreshEndpoint);
	if (Path.HasError())
	{
		OutError = Path.GetError();
		return false;
	}
	Config.RefreshPath = Path.GetValue();
	Config.TraceEndpoint = RefreshEndpoint;
	ResolvedEndpoint = RefreshEndpoint;
	return true;
}

TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> UVaRestBearerRefreshAuthConfig::CreateAuth() const
{
	if (!VaRest::Http::AuthConfigPrivate::Ready(RefreshEndpoint, ResolvedEndpoint))
		return nullptr;
	return FVaRestAuth::BearerRefresh(Config);
}

void UVaRestBearerRefreshAuthConfig::Validate(FVaRestApiValidationContext& Context) const
{
	using namespace VaRest::Http::AuthConfigPrivate;
	ValidateEndpoint(*this, RefreshEndpoint, TEXT("Auth.RefreshEndpoint"), Context);
	if (RefreshEndpoint.IsNone() && Config.RefreshPath.TrimStartAndEnd().IsEmpty())
		AddAuthConfigError(Context, TEXT("Auth.RefreshPath"), TEXT("Auth.Config.RefreshPath"), TEXT("A refresh path or POST endpoint is required"));
	if (Config.AccessTokenPath.IsEmpty())
		AddAuthConfigError(Context, TEXT("Auth.AccessTokenPath"), TEXT("Auth.Config.AccessTokenPath"), TEXT("An access token response path is required"));
	else if (!FVaRestJson::IsValidPath(Config.AccessTokenPath))
		AddAuthConfigError(Context, TEXT("Auth.AccessTokenPath.Syntax"), TEXT("Auth.Config.AccessTokenPath"), TEXT("Invalid JSON response path syntax"));
	if (!FVaRestJson::IsValidPath(Config.RefreshTokenPath))
		AddAuthConfigError(Context, TEXT("Auth.RefreshTokenPath.Syntax"), TEXT("Auth.Config.RefreshTokenPath"), TEXT("Invalid JSON response path syntax"));
	if (!FVaRestJson::IsValidPath(Config.ExpiresInPath))
		AddAuthConfigError(Context, TEXT("Auth.ExpiresInPath.Syntax"), TEXT("Auth.Config.ExpiresInPath"), TEXT("Invalid JSON response path syntax"));
	if (Config.Style != EVaRestRefreshStyle::Json && Config.Style != EVaRestRefreshStyle::OAuth2Form)
		AddAuthConfigError(Context, TEXT("Auth.RefreshStyle"), TEXT("Auth.Config.Style"), TEXT("Unknown refresh request style"));
	if (Config.Style == EVaRestRefreshStyle::Json && Config.RefreshBodyField.IsEmpty())
		AddAuthConfigError(Context, TEXT("Auth.RefreshBodyField"), TEXT("Auth.Config.RefreshBodyField"), TEXT("JSON refresh needs a token body field"));
}

bool UVaRestClientCredentialsAuthConfig::ResolveEndpoint(const UVaRestApi& Api, FString& OutError)
{
	ResolvedEndpoint = NAME_None;
	OutError.Reset();
	if (TokenEndpoint.IsNone())
	{
		Config.TraceEndpoint = NAME_None;
		return true;
	}
	const auto Path = Api.ResolveEndpointPath(TokenEndpoint);
	if (Path.HasError())
	{
		OutError = Path.GetError();
		return false;
	}
	Config.TokenPath = Path.GetValue();
	Config.TraceEndpoint = TokenEndpoint;
	ResolvedEndpoint = TokenEndpoint;
	return true;
}

TSharedPtr<IVaRestAuth, ESPMode::ThreadSafe> UVaRestClientCredentialsAuthConfig::CreateAuth() const
{
	if (!VaRest::Http::AuthConfigPrivate::Ready(TokenEndpoint, ResolvedEndpoint))
		return nullptr;
	return FVaRestAuth::ClientCredentials(Config);
}

void UVaRestClientCredentialsAuthConfig::Validate(FVaRestApiValidationContext& Context) const
{
	VaRest::Http::AuthConfigPrivate::ValidateEndpoint(*this, TokenEndpoint, TEXT("Auth.TokenEndpoint"), Context);
	FVaRestClientCredentialsConfig Effective = Config;
	if (!TokenEndpoint.IsNone())
	{
		const UVaRestApi* Api = GetApiOnGT();
		if (Api)
		{
			const auto Path = Api->ResolveEndpointPath(TokenEndpoint);
			if (Path.HasValue())
				Effective.TokenPath = Path.GetValue();
		}
	}
	FString Error;
	if (!Effective.Validate(Error, false))
		Context.Error(TEXT("Auth.ClientCredentials"), TEXT("Auth.Config"), FText::FromString(Error));
	if (Config.ClientId.IsEmpty() || Config.ClientSecret.IsEmpty())
		Context.Warning(TEXT("Auth.RuntimeCredentials"), TEXT("Auth.Config"), FText::FromString(TEXT("Supply client ID and secret at runtime before the first authenticated request")));
}
