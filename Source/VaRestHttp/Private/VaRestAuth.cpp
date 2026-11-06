// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuth.h"
#include "VaRestOfflineInternal.h"

#include "Containers/UnrealString.h"
#include "Misc/Base64.h"
#include "VaRestAuthInternal.h"
#include "VaRestClientState.h"
#include "VaRestClientCredentialsPrivate.h"
#include "VaRestExchangeState.h"
#include "VaRestHttpLog.h"
#include "VaRestIdentity.h"
#include "VaRestUrl.h"

FVaRestAuthRefreshResult FVaRestAuthRefreshResult::Refreshed(FString InAccessToken, FString InRefreshToken, double InExpiresInSeconds)
{
	FVaRestAuthRefreshResult Out;
	Out.Outcome = EVaRestAuthRefreshOutcome::Refreshed;
	Out.AccessToken = MoveTemp(InAccessToken);
	Out.RefreshToken = MoveTemp(InRefreshToken);
	Out.ExpiresInSeconds = InExpiresInSeconds;
	return Out;
}

FVaRestAuthRefreshResult FVaRestAuthRefreshResult::Rejected(FString InMessage)
{
	FVaRestAuthRefreshResult Out;
	Out.Outcome = EVaRestAuthRefreshOutcome::Rejected;
	Out.Message = MoveTemp(InMessage);
	return Out;
}

FVaRestAuthRefreshResult FVaRestAuthRefreshResult::Failed(EVaRestErrorKind InKind, FString InMessage)
{
	FVaRestAuthRefreshResult Out;
	Out.Outcome = EVaRestAuthRefreshOutcome::Failed;
	// Ruling I-7: the kinds a parked request can fail with and keep its meaning; the rest is a network failure
	Out.FailedKind = InKind == EVaRestErrorKind::Timeout || InKind == EVaRestErrorKind::Config || InKind == EVaRestErrorKind::Offline ? InKind : EVaRestErrorKind::Connection;
	Out.Message = MoveTemp(InMessage);
	return Out;
}

FVaRestAuthRefreshContext::FVaRestAuthRefreshContext(const FVaRestClient& InClient, FVaRestSessionData InSession, uint64 InRefreshId)
	: State(VaRest::Http::FClientAccess::State(InClient))
	, Session(MoveTemp(InSession))
	, RefreshId(InRefreshId)
	, IdTaken(MakeShared<std::atomic<bool>, ESPMode::ThreadSafe>(false))
{
}

FVaRestClient FVaRestAuthRefreshContext::GetClient() const
{
	// Concurrency review M-10: an empty client's requests fail Kind=Config, which the auth reports as its refresh's failure
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Pinned = State.Pin();
	return Pinned.IsValid() ? VaRest::Http::FClientAccess::Wrap(Pinned) : FVaRestClient();
}

bool FVaRestAuthRefreshContext::IsCurrentOnGT() const
{
	check(IsInGameThread());
	const auto Pinned=State.Pin(); return Pinned && Pinned->IsCurrentRefreshOnGT(RefreshId);
}

namespace VaRest::Http::AuthPrivate
{
	class FBearerAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("Bearer"); }
        virtual FName GetIdentityContractName() const override { return TEXT("VaRest.Bearer.v1"); }

		virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
		{
			OutIdentity.Reset();
			if (!Session.HasAccessToken() && !Session.HasRefreshToken()) return false;
			OutIdentity = VaRest::Http::SessionScope(Session);
			return true;
		}

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
        virtual FName GetIdentityContractName() const override { return TEXT("VaRest.ApiKey.v1"); }

		virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
		{
			OutIdentity.Reset();
			if (!ConfigError.IsEmpty()) return false;
			OutIdentity = VaRest::Http::HashFields(TEXT("VaRest/apikey/v1"), TArray<FString>{
				LexToString(static_cast<uint8>(Placement)),
				Placement == EVaRestApiKeyPlacement::Header ? Name.ToLower() : Name, Key});
			return true;
		}

		virtual FString GetConfigError() const override { return ConfigError; }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			// An empty key is a configuration error since the review's M-2 ruling: the factory set ConfigError for it
			if (!ConfigError.IsEmpty())
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
        virtual FName GetIdentityContractName() const override { return TEXT("VaRest.Basic.v1"); }

		virtual bool GetIdentity(const FVaRestSessionData& Session, FString& OutIdentity) const override
		{
			OutIdentity.Reset();
			if (!ConfigError.IsEmpty()) return false;
			OutIdentity = VaRest::Http::HashFields(TEXT("VaRest/basic/v1"), TArray<FString>{Credential});
			return true;
		}

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
			const auto Continue=[&]() {
				if ((Ex.Managed.IsValid() && Ex.Managed.Control->IsStopped()) || FClientState::IsShuttingDown() ||
					Ex.bFinished || Ex.Phase!=EVaRestExchangePhase::InChain) return false;
				if (Ex.IsCancelRequested() || (Ex.Managed.IsValid() && Ex.State->GetManagedLifetime().Gate->IsClosed()))
				{
					auto Canceled=MoveTemp(OnOutcome); if (Canceled) Canceled(FVaRestAttemptOutcome::Canceled()); return false;
				}
				return true;
			};
			if (!Continue()) return;
            if (Ex.bQueueWhenOffline && !QueueIdentityStillCurrent(Ex))
            { if (Continue()) OnOutcome(LocalFailure(TEXT("QueueIdentity"))); return; }
			if (!Continue()) return;
            if (!Ex.ReplayIdentityScope.IsEmpty())
            {
                const auto Current = Ex.State->GetConfig();
                if (!SameOrigin(Current->BaseUrl, Ex.Url))
                { OnOutcome(LocalFailure(TEXT("QueueOrigin"))); return; }
                FString CurrentScope;
                if ((Ex.AuthMode != EVaRestAuthMode::None && Current->Auth != Ex.Config->Auth)
                    || !RequestIdentityScope(*Current, Ex.State->GetSessionData(), Ex.AuthMode, Ex.RequestHeaders, CurrentScope, &Ex.State->GetManagedLifetime())
                    || CurrentScope != Ex.ReplayIdentityScope)
                { if (Continue()) OnOutcome(LocalFailure(TEXT("QueueIdentity"))); return; }
            }
			if (!Continue()) return;
			CheckConditionalIdentity(Ex);
			if (!Continue()) return;
			// First, before the configuration check and the session: the refresh request (plan decision 23) and NoAuth() -- a
			// login -- go out bare, with neither credentials nor session headers. MarkAuthRefresh sends the refresh request with
			// AuthMode None too; the flag is checked as well, so the refresh request is bare by what it is, not by a mode.
			if (Ex.bIsAuthRefresh || Ex.AuthMode == EVaRestAuthMode::None)
			{
				Next.Proceed(Exchange, MoveTemp(OnOutcome));
				return;
			}
			// The Send() snapshot's auth (spec 3.1: the configuration a request started with); the session below is the live one
			const IVaRestAuth* Auth = Ex.Config->Auth.Get();
			if (Auth)
			{
				const FString Invalid = Auth->GetConfigError();
				if (!Continue()) return;
				if (!Invalid.IsEmpty())
				{
					// Ruling Q7: a programmer error, for Required and IfAvailable alike -- sending without the key would hide it.
					// Escaped: a custom auth names itself, and the message is text a game may log as it is (as Resolve's).
					const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> Redactor = RedactorFor(Ex);
					const FString AuthName = EscapeForLog(Auth->GetAuthName().ToString());
					if (!Continue()) return;
					if (Ex.LogLevel != EVaRestLogLevel::None && UE_LOG_ACTIVE(LogVaRestHttp, Warning))
					{
						// Task 3: the request's own redactor, as every line about it
						UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: %s %s -> misconfigured auth (%s); not sent"), Ex.RequestId, *EscapeForLog(Ex.Verb),
							*EscapeForLog(Redactor->RedactUrl(Ex.Url)), *AuthName);
					}
					if (!Continue()) return;
					FVaRestAttemptOutcome Refused;
					Refused.Failure = EVaRestTransportFailure::Other;
					Refused.FailureDetail = TEXT("AuthInvalid");
					// Review M-1: a custom auth's text is third-party, and FVaRestError::Message must hold no credential (the
					// trace scrubs its own copy; the game's is this one): scrubbed, then escaped
					Refused.FailureMessage = FString::Printf(TEXT("the client's auth (%s) is misconfigured: %s"), *AuthName,
						*EscapeControlsForLog(Redactor->ScrubText(Invalid)));
					OnOutcome(MoveTemp(Refused));
					return;
				}
			}
			const FVaRestSessionData Session = Ex.State->GetSessionData(); // an Apply callback can change the live session reentrantly
			TArray<TPair<FString, FString>> AuthQuery;
			FVaRestAuthTarget Target(Ex.AttemptHeaders, AuthQuery, Session);
			Target.RequestId = Ex.RequestId;
			// False only for an auth with no credentials: no auth at all is true (ApplyCredentials)
			const bool bApplied=ApplyCredentials(Auth,Target);
			if (!Continue()) return;
			if (!Target.ConfigError.IsEmpty())
			{
				FVaRestAttemptOutcome Refused;
				Refused.Failure=EVaRestTransportFailure::Other; Refused.FailureDetail=TEXT("AuthInvalid");
				Refused.FailureMessage=EscapeControlsForLog(RedactorFor(Ex)->ScrubText(Target.ConfigError));
				OnOutcome(MoveTemp(Refused)); return;
			}
			if (!bApplied && Ex.AuthMode == EVaRestAuthMode::Required && Auth)
			{
				const FString AuthName = EscapeForLog(Auth->GetAuthName().ToString());
				if (!Continue()) return;
				if (Ex.LogLevel != EVaRestLogLevel::None && UE_LOG_ACTIVE(LogVaRestHttp, Warning))
				{
					UE_LOG(LogVaRestHttp, Warning, TEXT("#%llu VaRest: %s %s -> no credentials (%s); not sent"), Ex.RequestId, *EscapeForLog(Ex.Verb),
						*EscapeForLog(RedactorFor(Ex)->RedactUrl(Ex.Url)), *AuthName);
				}
				if (!Continue()) return;
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
			if (!Continue()) return;
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
	};
}

bool VaRest::Http::ApplyCredentials(const IVaRestAuth* Auth, FVaRestAuthTarget& Target)
{
	Target.ConfigError.Reset();
	for (const FVaRestHeaders::FPair& Pair : Target.Session.Headers)
	{
		Target.Headers.Set(Pair.Key, Pair.Value);
	}
	const bool bApplied=!Auth || Auth->Apply(Target);
	return bApplied && Target.ConfigError.IsEmpty();
}

bool VaRest::Http::CanRefreshNow(const IVaRestAuth& Auth, const FVaRestSessionData& Session)
{
	// The configuration first: a custom auth's CanRefresh need not know about its own config error (review B m-3)
	return Auth.GetConfigError().IsEmpty() && Auth.CanRefresh(Session);
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
	else if (InKey.IsEmpty())
	{
		// Review M-2 ruling: the key is configuration, not a session -- a key missing from the ini is a typo to fix, and
		// Kind=Auth would send the player to the login screen for it
		UE_LOG(LogVaRestHttp, Error, TEXT("VaRest: FVaRestAuth::ApiKey: the key is empty; requests fail with Kind=Config"));
		ConfigError = TEXT("the API key is empty");
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

FVaRestAuth::FRef FVaRestAuth::ClientCredentials(const FVaRestClientCredentialsConfig& Config)
{
	return VaRest::Http::MakeClientCredentialsAuth(Config);
}
