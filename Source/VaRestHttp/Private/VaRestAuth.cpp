// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuth.h"

#include "Misc/Base64.h"
#include "VaRestAuthInternal.h"
#include "VaRestClientState.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestUrl.h"

namespace VaRest::Http::AuthPrivate
{
	class FBearerAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("Bearer"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!Target.Session.HasAccessToken())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
			return true;
		}
	};

	class FApiKeyAuth final : public IVaRestAuth
	{
	public:
		FApiKeyAuth(EVaRestApiKeyPlacement InPlacement, FString InName, FString InKey, FString InConfigError)
			: Placement(InPlacement)
			, Name(MoveTemp(InName))
			, Key(MoveTemp(InKey))
			, ConfigError(MoveTemp(InConfigError))
		{
		}

		virtual FName GetAuthName() const override { return TEXT("ApiKey"); }

		virtual FString GetConfigError() const override { return ConfigError; }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!ConfigError.IsEmpty() || Key.IsEmpty())
			{
				return false;
			}
			if (Placement == EVaRestApiKeyPlacement::Header)
			{
				Target.Headers.Set(Name, Key);
			}
			else
			{
				Target.Query.Emplace(Name, Key);
			}
			return true;
		}

		virtual void GetRedactedKeys(TArray<FString>& OutKeys) const override
		{
			if (!Name.IsEmpty())
			{
				OutKeys.Add(Name);
			}
		}

		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("VaRest.Auth.ApiKey"), Key);
		}

	private:
		EVaRestApiKeyPlacement Placement;
		FString Name;
		FString Key;
		/** Ruling Q7: empty unless the factory found the configuration unusable */
		FString ConfigError;
	};

	class FBasicAuth final : public IVaRestAuth
	{
	public:
		FBasicAuth(FString InCredential, FString InPassword, FString InConfigError)
			: Credential(MoveTemp(InCredential))
			, Password(MoveTemp(InPassword))
			, ConfigError(MoveTemp(InConfigError))
		{
		}

		virtual FName GetAuthName() const override { return TEXT("Basic"); }

		virtual FString GetConfigError() const override { return ConfigError; }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!ConfigError.IsEmpty())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Basic ")) + Credential);
			return true;
		}

		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("VaRest.Auth.Basic"), Credential);
			OutValues.Emplace(TEXT("VaRest.Auth.BasicPassword"), Password);
		}

	private:
		/** base64(user:password) */
		FString Credential;
		FString Password;
		/** Ruling Q7: empty unless the user holds ':' */
		FString ConfigError;
	};

	/**
	 * Spec 4.2, per attempt (plan decisions 4, 6). Game thread. Right after the limiter (ruling I-3), so it holds a slot: every
	 * path below either proceeds or answers, never neither (Task 4).
	 */
	class FAuthApplyStage final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("AuthApply"); }

		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			FExchangeState& Ex = Internals(*Exchange);
			// NoAuth() -- a login, and (Task 15) the refresh request, which MarkAuthRefresh sends with AuthMode None -- goes out bare
			if (Ex.AuthMode == EVaRestAuthMode::None)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			// The Send() snapshot's auth (spec 3.1: the configuration a request started with); the session below is the live one
			const IVaRestAuth* Auth = Ex.Config->Auth.Get();
			if (Auth)
			{
				const FString Invalid = Auth->GetConfigError();
				if (!Invalid.IsEmpty())
				{
					// Ruling Q7: a programmer error, for Required and IfAvailable alike -- sending without the key would hide it.
					// Escaped: a custom auth names itself, and the message is text a game may log as it is (as Resolve's).
					const FString AuthName = EscapeForLog(Auth->GetAuthName().ToString());
					if (Ex.LogLevel != EVaRestLogLevel::None && UE_LOG_ACTIVE(LogVaRestHttp, Warning))
					{
						// Task 3: the request's own redactor, as every line about it
						UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: %s %s -> misconfigured auth (%s); not sent"), Ex.RequestId, *EscapeForLog(Ex.Verb),
							*EscapeForLog(RedactorFor(Ex)->RedactUrl(Ex.Url)), *AuthName);
					}
					FVaRestAttemptOutcome Refused;
					Refused.Failure = EVaRestTransportFailure::Other;
					Refused.FailureDetail = TEXT("AuthInvalid");
					Refused.FailureMessage = FString::Printf(TEXT("the client's auth (%s) is misconfigured: %s"), *AuthName, *EscapeControlsForLog(Invalid));
					OnOutcome(MoveTemp(Refused));
					return;
				}
			}
			const FVaRestSessionData& Session = Ex.State->GetSessionData();
			TArray<TPair<FString, FString>> AuthQuery;
			FVaRestAuthTarget Target(Ex.AttemptHeaders, AuthQuery, Session);
			Target.RequestId = Ex.RequestId;
			// False only for an auth with no credentials: no auth at all is true (ApplyCredentials)
			if (!ApplyCredentials(Auth, Target) && Ex.AuthMode == EVaRestAuthMode::Required && Auth)
			{
				const FString AuthName = EscapeForLog(Auth->GetAuthName().ToString());
				if (Ex.LogLevel != EVaRestLogLevel::None && UE_LOG_ACTIVE(LogVaRestHttp, Warning))
				{
					UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: %s %s -> no credentials (%s); not sent"), Ex.RequestId, *EscapeForLog(Ex.Verb),
						*EscapeForLog(RedactorFor(Ex)->RedactUrl(Ex.Url)), *AuthName);
				}
				FVaRestAttemptOutcome Refused;
				Refused.Failure = EVaRestTransportFailure::Other;
				Refused.FailureDetail = TEXT("NoCredentials");
				Refused.FailureMessage = FString::Printf(TEXT("the client's auth (%s) has no credentials: log in first, or send with AuthMode(IfAvailable) or NoAuth()"), *AuthName);
				OnOutcome(MoveTemp(Refused));
				return;
			}
			// Concurrency review M-6: the token's generation, which a session header does not move
			Ex.AppliedCredentialGeneration = Session.CredentialGeneration;
			if (AuthQuery.Num() > 0)
			{
				AppendQuery(Ex.AttemptUrl, AuthQuery);
			}
			// Task 12 review M-7: what this attempt now carries -- a token set while it queued among it -- stays scrubbed from
			// its lines, its trace and its error's URL after the client forgets it (a logout while the transport has it)
			RememberAppliedRedactor(Ex);
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};
}

bool VaRest::Http::ApplyCredentials(const IVaRestAuth* Auth, FVaRestAuthTarget& Target)
{
	for (const FVaRestHeaders::FPair& Pair : Target.Session.Headers)
	{
		Target.Headers.Set(Pair.Key, Pair.Value);
	}
	return !Auth || Auth->Apply(Target);
}

FVaRestStageRef VaRest::Http::MakeAuthApplyStage()
{
	return MakeShared<AuthPrivate::FAuthApplyStage, ESPMode::ThreadSafe>();
}

FVaRestAuth::FRef FVaRestAuth::Bearer()
{
	return MakeShared<VaRest::Http::AuthPrivate::FBearerAuth, ESPMode::ThreadSafe>();
}

FVaRestAuth::FRef FVaRestAuth::ApiKey(EVaRestApiKeyPlacement InPlacement, FStringView InName, FStringView InKey)
{
	// Ruling Q7: an unusable configuration is said once here (each pattern its own, for the tests' expected errors) and
	// fails every request Kind=Config through GetConfigError
	FString ConfigError;
	if (InPlacement == EVaRestApiKeyPlacement::Header && !VaRest::Http::IsValidHeaderName(InName))
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ApiKey: the header name \"%s\" is not an RFC 9110 token; requests fail with Kind=Config"),
			*VaRest::Http::EscapeForLog(InName));
		ConfigError = TEXT("the API key's header name is not an RFC 9110 token");
	}
	else if (InPlacement == EVaRestApiKeyPlacement::Query && InName.IsEmpty())
	{
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ApiKey: the query key is empty; requests fail with Kind=Config"));
		ConfigError = TEXT("the API key's query key is empty");
	}
	else if (!VaRest::Http::IsValidHeaderValue(InKey))
	{
		// The key is a secret: not in the line
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ApiKey: the key holds a control character; requests fail with Kind=Config"));
		ConfigError = TEXT("the API key holds a control character");
	}
	return MakeShared<VaRest::Http::AuthPrivate::FApiKeyAuth, ESPMode::ThreadSafe>(InPlacement, FString(InName), FString(InKey), MoveTemp(ConfigError));
}

FVaRestAuth::FRef FVaRestAuth::Basic(FStringView InUser, FStringView InPassword)
{
	FString ConfigError;
	int32 Colon = INDEX_NONE;
	if (InUser.FindChar(TEXT(':'), Colon))
	{
		// Ruling Q7: the server would split the credential at the user's own ':' (RFC 7617 2: the user-id holds none)
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::Basic: the user holds ':' (RFC 7617 2); requests fail with Kind=Config"));
		ConfigError = TEXT("the Basic user holds ':' (RFC 7617 2)");
	}
	// RFC 7617 2.1: user-id ":" password, UTF-8 (the charset the RFC recommends), base64
	const FString Pair = FString(InUser) + TEXT(":") + FString(InPassword);
	const FTCHARToUTF8 Utf8(*Pair, Pair.Len());
	const FString Credential = FBase64::Encode(reinterpret_cast<const uint8*>(Utf8.Get()), static_cast<uint32>(Utf8.Length()));
	return MakeShared<VaRest::Http::AuthPrivate::FBasicAuth, ESPMode::ThreadSafe>(Credential, FString(InPassword), MoveTemp(ConfigError));
}
