// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestClientCredentials.h"

#include "VaRestClientCredentialsPrivate.h"

#include "Misc/Base64.h"
#include "VaRestBody.h"
#include "VaRestClientState.h"
#include "VaRestHttpLog.h"
#include "VaRestIdentity.h"
#include "VaRestJsonRedact.h"
#include "VaRestUrl.h"
#include "VaRestUrlValidation.h"

namespace VaRest::Http::ClientCredentialsPrivate
{
bool HasControl(FStringView Text)
{
	for (const TCHAR Char : Text)
	{
		if (Char < 32 || Char == 127)
			return true;
	}
	return false;
}

bool ValidIndex(FStringView Text)
{
	if (Text.IsEmpty() || Text.Len() > 9 || (Text.Len() > 1 && Text[0] == TEXT('0')))
		return false;
	for (const TCHAR Char : Text)
		if (Char < TEXT('0') || Char > TEXT('9'))
			return false;
	return true;
}

/** The syntactic subset walked by FVaRestJson::Path; no sample document or token is needed to validate it. */
bool ValidJsonPath(FStringView Path)
{
	if (Path.IsEmpty() || HasControl(Path))
		return false;
	if (Path[0] == TEXT('/'))
	{
		for (int32 Index = 1; Index < Path.Len(); ++Index)
		{
			if (Path[Index] != TEXT('~'))
				continue;
			if (++Index == Path.Len() || (Path[Index] != TEXT('0') && Path[Index] != TEXT('1')))
				return false;
		}
		return true;
	}
	int32 Index = 0;
	bool bFirst = true;
	while (Index < Path.Len())
	{
		const int32 Start = Index;
		while (Index < Path.Len() && Path[Index] != TEXT('.') && Path[Index] != TEXT('[') && Path[Index] != TEXT(']'))
			++Index;
		if (Index == Start && !(bFirst && Index < Path.Len() && Path[Index] == TEXT('[')))
			return false;
		while (Index < Path.Len() && Path[Index] == TEXT('['))
		{
			const int32 Digits = ++Index;
			while (Index < Path.Len() && Path[Index] != TEXT(']'))
				++Index;
			if (Index == Path.Len() || !ValidIndex(Path.Mid(Digits, Index - Digits)))
				return false;
			++Index;
		}
		if (Index < Path.Len() && (Path[Index++] != TEXT('.') || Index == Path.Len()))
			return false;
		bFirst = false;
	}
	return true;
}

FString TokenKey(const FString& Path)
{
	if (Path.StartsWith(TEXT("/")))
	{
		TArray<FString> Parts;
		Path.Mid(1).ParseIntoArray(Parts, TEXT("/"), false);
		while (Parts.Num() > 1 && ValidIndex(Parts.Last()))
			Parts.Pop();
		if (Parts.IsEmpty() || (Parts.Num() == 1 && ValidIndex(Parts.Last())))
			return FString();
		FString Key;
		const FString& Part = Parts.Last();
		for (int32 Index = 0; Index < Part.Len(); ++Index)
		{
			if (Part[Index] != TEXT('~'))
				Key.AppendChar(Part[Index]);
			else
			{
				if (++Index == Part.Len() || (Part[Index] != TEXT('0') && Part[Index] != TEXT('1')))
					return FString();
				Key.AppendChar(Part[Index] == TEXT('0') ? TEXT('~') : TEXT('/'));
			}
		}
		return Key;
	}
	FString Name = Path;
	int32 Bracket = INDEX_NONE;
	while (Name.EndsWith(TEXT("]")) && Name.FindLastChar(TEXT('['), Bracket))
		Name.LeftInline(Bracket);
	int32 Dot = INDEX_NONE;
	return Name.FindLastChar(TEXT('.'), Dot) ? Name.Mid(Dot + 1) : Name;
}

bool ProtectableTokenKey(const FString& Key)
{
	if (Key.IsEmpty() || !FVaRestRedactRules::IsAcceptableKey(Key))
		return false;
	FVaRestRedactRules Protection;
	Protection.AddKey(Key);
	return Protection.ShouldRedact(Key, EVaRestRedactKeyKind::Json);
}

FString CanonicalEndpoint(const FString& Path)
{
	FString Origin;
	if (!IsAbsoluteHttpUrl(Path) || !CanonicalOrigin(Path, Origin))
		return Path;
	const int32 Begin = Path.Find(TEXT("://")) + 3;
	int32 End = Begin;
	while (End < Path.Len() && Path[End] != TEXT('/') && Path[End] != TEXT('?'))
		++End;
	FString Suffix = Path.Mid(End);
	if (Suffix.IsEmpty() || Suffix[0] == TEXT('?'))
		Suffix = TEXT("/") + Suffix;
	return Origin + Suffix;
}

FString BasicCredential(const FVaRestClientCredentialsConfig& Config)
{
	// RFC 6749 2.3.1: first form-encode EACH component, then base64 the UTF-8 user:password pair.
	const FString Pair = FormEncode(Config.ClientId) + TEXT(":") + FormEncode(Config.ClientSecret);
	const FTCHARToUTF8 Utf8(*Pair, Pair.Len());
	return FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
}

FVaRestAuthRefreshResult ReadTokens(const FVaRestClientCredentialsConfig& Config, const FString& Host, const TVaRestResult<FVaRestJson>& Result)
{
	if (Result.IsSuccess())
	{
		const FVaRestJson Body = Result.Value.IsSet() ? Result.Value.GetValue() : FVaRestJson();
		const FString Access = Body.Path(Config.AccessTokenPath).AsString();
		if (Access.IsEmpty())
			return FVaRestAuthRefreshResult::Rejected(TEXT("the token response has no access token at the configured path"));
		const FVaRestJson Type = Body[TEXT("token_type")];
		if (Type.IsValid() && !Type.IsNull() && (!Type.IsString() || !Type.AsString().Equals(TEXT("Bearer"), ESearchCase::IgnoreCase)))
			return FVaRestAuthRefreshResult::Rejected(TEXT("the token endpoint returned an unsupported token type"));
		const FVaRestJson Expiry = Config.ExpiresInPath.IsEmpty() ? FVaRestJson() : Body.Path(Config.ExpiresInPath);
		double Seconds = 0.0;
		if (Expiry.IsNumber())
			Seconds = Expiry.AsDouble();
		else if (Expiry.IsString())
		{
			const FString Text = Expiry.AsString();
			if (Text.IsNumeric())
				Seconds = FCString::Atod(*Text);
		}
		return FVaRestAuthRefreshResult::Refreshed(Access, FString(), FMath::IsFinite(Seconds) ? FMath::Max(0.0, Seconds) : 0.0);
	}
	const FVaRestError& Error = Result.Error;
	const FString Why = Error.Kind == EVaRestErrorKind::Http ? FString::Printf(TEXT("the token endpoint answered %d"), Error.HttpStatus) : Error.Message;
	const bool bRefused = Error.Kind == EVaRestErrorKind::Http && Error.HttpStatus >= 400 && Error.HttpStatus < 500 && Error.HttpStatus != 408 && Error.HttpStatus != 429;
	const bool bMalformed = Error.Kind == EVaRestErrorKind::Decode && Result.Response.Status >= 200 && Result.Response.Status < 300;
	if (bMalformed)
	{
		const FString AnsweredBy = GetUrlHost(Result.Response.EffectiveUrl);
		if (!Host.IsEmpty() && !AnsweredBy.IsEmpty() && !AnsweredBy.Equals(Host, ESearchCase::IgnoreCase))
			return FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("another host answered the token request with a malformed response"));
	}
	return bRefused || bMalformed ? FVaRestAuthRefreshResult::Rejected(Why) : FVaRestAuthRefreshResult::Failed(Error.Kind, Why);
}

class FClientCredentialsAuth final : public IVaRestAuth
{
public:
	explicit FClientCredentialsAuth(const FVaRestClientCredentialsConfig& InConfig)
		: Config(InConfig)
	{
		Config.Validate(ConfigError);
		if (ConfigError.IsEmpty())
		{
			// Conservative for relative paths: the client's own base/URL domain still separates cache and queue entries.
			// A changed secret starts a new identity; rotating an access token does not.
			Identity = HashFields(TEXT("VaRest/client-credentials/v1"), TArray<FString>{
																			Config.ClientId, CanonicalEndpoint(Config.TokenPath), Config.Scope, Config.ClientSecret});
		}
	}
	virtual FName GetAuthName() const override { return TEXT("ClientCredentials"); }
	virtual FName GetIdentityContractName() const override { return TEXT("VaRest.ClientCredentials.v1"); }
	virtual FString GetConfigError() const override { return ConfigError; }
	virtual bool GetIdentity(const FVaRestSessionData&, FString& OutIdentity) const override
	{
		OutIdentity = Identity;
		return !Identity.IsEmpty();
	}
	virtual bool Apply(FVaRestAuthTarget& Target) const override
	{
		if (!ConfigError.IsEmpty() || !Target.Session.HasAccessToken())
			return false;
		Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
		return true;
	}
	virtual bool CanRefresh(const FVaRestSessionData&) const override { return ConfigError.IsEmpty(); }
	virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const override
	{
		if (!ConfigError.IsEmpty())
			return false;
		const double Lead = Session.Lifetime > 0.0 ? FMath::Min(static_cast<double>(Config.RefreshLeadSeconds), Session.Lifetime * 0.5) : Config.RefreshLeadSeconds;
		return !Session.HasAccessToken() || (Session.ExpiresAt > 0.0 && Now >= Session.ExpiresAt - Lead);
	}
	virtual bool IsAuthFailure(int32 Status) const override { return Config.RefreshStatuses.Contains(Status); }
	virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
	{
		if (!ConfigError.IsEmpty())
		{
			Done(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Config, ConfigError));
			return;
		}
		const FString Host = GetUrlHost(JoinUrl(FClientAccess::ClientOf(Context).GetBaseUrl(), Config.TokenPath));
		FVaRestForm Form;
		Form.Add(TEXT("grant_type"), TEXT("client_credentials"));
		if (!Config.Scope.IsEmpty())
			Form.Add(TEXT("scope"), Config.Scope);
		TVaRestRequest<FVaRestJson> Request = Context.RequestForEndpoint<FVaRestJson>(TEXTVIEW("POST"), Config.TokenPath, Config.TraceEndpoint).Header(TEXT("Accept"), TEXT("application/json"));
		if (Config.ClientAuthMethod == EVaRestOAuthClientAuthMethod::Basic)
			Request = MoveTemp(Request).Header(TEXT("Authorization"), TEXT("Basic ") + BasicCredential(Config));
		else
		{
			Form.Add(TEXT("client_id"), Config.ClientId);
			Form.Add(TEXT("client_secret"), Config.ClientSecret);
		}
		MoveTemp(Request).Body(Form).OnComplete([Paths = Config, Host, Finish = MoveTemp(Done)](const TVaRestResult<FVaRestJson>& Result) mutable {
										Finish(ReadTokens(Paths, Host, Result));
									})
			.Send();
	}
	virtual void GetRedactedKeys(TArray<FString>& OutKeys) const override
	{
		OutKeys.Add(TEXT("client_secret"));
		OutKeys.Add(TEXT("access_token"));
		const FString Key = TokenKey(Config.AccessTokenPath);
		if (ProtectableTokenKey(Key))
			OutKeys.Add(Key);
	}
	virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
	{
		OutValues.Emplace(TEXT("ClientSecret"), Config.ClientSecret);
		OutValues.Emplace(TEXT("ClientSecretForm"), FormEncode(Config.ClientSecret));
		OutValues.Emplace(TEXT("ClientBasic"), BasicCredential(Config));
		OutValues.Emplace(TEXT("ClientPair"), FormEncode(Config.ClientId) + TEXT(":") + FormEncode(Config.ClientSecret));
	}

private:
	FVaRestClientCredentialsConfig Config;
	FString ConfigError;
	FString Identity;
};
} // namespace VaRest::Http::ClientCredentialsPrivate

bool FVaRestClientCredentialsConfig::Validate(FString& OutError, bool bRequireCredentials) const
{
	using namespace VaRest::Http;
	using namespace VaRest::Http::ClientCredentialsPrivate;
	OutError.Reset();
	if (TokenPath.IsEmpty() || HasControl(TokenPath) || !TokenPath.Equals(TokenPath.TrimStartAndEnd(), ESearchCase::CaseSensitive) || TokenPath.Contains(TEXT(" ")) || TokenPath.Contains(TEXT("\\")) || TokenPath.Contains(TEXT("#")) || TokenPath.Contains(TEXT("{")) || TokenPath.Contains(TEXT("}")))
		OutError = TEXT("ClientCredentials TokenPath must be a nonempty URL/path without controls, spaces, fragments or templates");
	else if (TokenPath.Contains(TEXT("://")))
	{
		if (!IsValidHttpUrl(TokenPath, false))
			OutError = TEXT("ClientCredentials TokenPath must have a valid HTTP(S) authority without userinfo");
	}
	if (!OutError.IsEmpty())
		return false;
	if (bRequireCredentials && (ClientId.IsEmpty() || ClientSecret.IsEmpty()))
		OutError = TEXT("ClientCredentials needs a nonempty client ID and secret");
	else if (HasControl(ClientId) || HasControl(ClientSecret) || HasControl(Scope))
		OutError = TEXT("ClientCredentials credentials and scope cannot contain controls");
	else if (ClientAuthMethod != EVaRestOAuthClientAuthMethod::Basic && ClientAuthMethod != EVaRestOAuthClientAuthMethod::Form)
		OutError = TEXT("ClientCredentials has an unknown client authentication method");
	else if (!ValidJsonPath(AccessTokenPath) || TokenKey(AccessTokenPath).IsEmpty() || (!ExpiresInPath.IsEmpty() && !ValidJsonPath(ExpiresInPath)))
		OutError = TEXT("ClientCredentials has an invalid token response path");
	else if (!ProtectableTokenKey(TokenKey(AccessTokenPath)))
		OutError = TEXT("ClientCredentials access token path must end in a key supported by response redaction");
	else if (!FMath::IsFinite(RefreshLeadSeconds) || RefreshLeadSeconds < 0.f)
		OutError = TEXT("ClientCredentials refresh lead must be finite and nonnegative");
	else
	{
		for (int32 Status : RefreshStatuses)
			if (Status < 400 || Status > 599)
			{
				OutError = TEXT("ClientCredentials refresh statuses must be HTTP error statuses");
				break;
			}
	}
	return OutError.IsEmpty();
}

FVaRestAuth::FRef VaRest::Http::MakeClientCredentialsAuth(const FVaRestClientCredentialsConfig& Config)
{
	const auto Auth = MakeShared<ClientCredentialsPrivate::FClientCredentialsAuth, ESPMode::ThreadSafe>(Config);
	if (!Auth->GetConfigError().IsEmpty())
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ClientCredentials: %s; requests fail with Kind=Config"), *Auth->GetConfigError());
	return Auth;
}
