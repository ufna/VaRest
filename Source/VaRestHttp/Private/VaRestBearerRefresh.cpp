// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#include "VaRestAuth.h"

#include "VaRestBody.h"

namespace VaRest::Http::BearerRefreshPrivate
{
	/**
	 * Carry (d), M3 review M19: the key a token path ends at -- "data.auth.refresh_token" -> "refresh_token", "/a/b" -> "b",
	 * "items[3]" -> "items". Read in the path's own syntax (FVaRestJson::Path, VaRestJsonPath.cpp:158-160): a pointer splits on
	 * '/' only and unescapes RFC 6901's "~1" and "~0" (VaRestJsonPath.cpp:31-49), so "/t/access.key" ends at "access.key" and
	 * "/t/a~1b" at "a/b"; a dotted path splits on '.' only, so "auth/x.renewal" ends at "renewal" (a dotted name may hold '/',
	 * :97). Splitting on both would add a key the response never holds and miss the one it does. A pointer's trailing array
	 * indexes go as "[3]" does ("/items/3" -> "items"): a key "3" would mask every "3" in every body, and not this token.
	 */
	FString LastSegment(FString Path)
	{
		if (Path.StartsWith(TEXT("/")))
		{
			TArray<FString> Tokens;
			// Empty tokens kept: "/a//b" walks "a", "", "b" (VaRestJsonPath.cpp:67-86)
			Path.Mid(1).ParseIntoArray(Tokens, TEXT("/"), /*InCullEmpty*/ false);
			// An array index as the walker reads one (VaRestJsonPath.cpp:7-25): 1 to 9 digits, no leading zero
			const auto IsIndex = [](const FString& Candidate)
			{
				if (Candidate.IsEmpty() || Candidate.Len() > 9 || (Candidate.Len() > 1 && Candidate[0] == TEXT('0')))
				{
					return false;
				}
				for (const TCHAR Char : Candidate)
				{
					if (Char < TEXT('0') || Char > TEXT('9'))
					{
						return false;
					}
				}
				return true;
			};
			while (Tokens.Num() > 1 && IsIndex(Tokens.Last()))
			{
				Tokens.RemoveAt(Tokens.Num() - 1);
			}
			if (Tokens.IsEmpty())
			{
				return FString();
			}
			const FString& Token = Tokens.Last();
			// One left-to-right pass, as the walker unescapes ("~01" is "~1"); a stray '~' makes the pointer miss, so no key
			FString Key;
			for (int32 Index = 0; Index < Token.Len(); ++Index)
			{
				if (Token[Index] != TEXT('~'))
				{
					Key.AppendChar(Token[Index]);
					continue;
				}
				if (Index + 1 >= Token.Len() || (Token[Index + 1] != TEXT('0') && Token[Index + 1] != TEXT('1')))
				{
					return FString();
				}
				Key.AppendChar(Token[Index + 1] == TEXT('0') ? TEXT('~') : TEXT('/'));
				++Index;
			}
			return Key;
		}
		int32 Bracket = INDEX_NONE;
		while (Path.EndsWith(TEXT("]")) && Path.FindLastChar(TEXT('['), Bracket))
		{
			Path.LeftInline(Bracket);
		}
		int32 Dot = INDEX_NONE;
		return Path.FindLastChar(TEXT('.'), Dot) ? Path.Mid(Dot + 1) : Path;
	}

	double ReadSeconds(const FVaRestJson& Value)
	{
		if (Value.IsNumber())
		{
			return Value.AsDouble();
		}
		if (Value.IsString())
		{
			const FString Number = Value.AsString();
			return Number.IsNumeric() ? FCString::Atod(*Number) : 0.0;
		}
		return 0.0;
	}

	/** Game thread: the response was decoded as JSON on a task (spec 1, VaRestPipeline.cpp:949-952); only the path reads run here */
	FVaRestAuthRefreshResult ReadRefresh(const FVaRestBearerRefreshConfig& Config, const TVaRestResult<FVaRestJson>& Result)
	{
		if (Result.IsSuccess())
		{
			const FVaRestJson Body = Result.Value.IsSet() ? Result.Value.GetValue() : FVaRestJson();
			const FString Access = Body.Path(Config.AccessTokenPath).AsString();
			if (Access.IsEmpty())
			{
				return FVaRestAuthRefreshResult::Rejected(FString::Printf(TEXT("the refresh response has no access token at \"%s\""), *Config.AccessTokenPath));
			}
			const FString Refresh = Config.RefreshTokenPath.IsEmpty() ? FString() : Body.Path(Config.RefreshTokenPath).AsString();
			const double ExpiresIn = Config.ExpiresInPath.IsEmpty() ? 0.0 : ReadSeconds(Body.Path(Config.ExpiresInPath));
			return FVaRestAuthRefreshResult::Refreshed(Access, Refresh, FMath::IsFinite(ExpiresIn) ? FMath::Max(0.0, ExpiresIn) : 0.0);
		}
		const FVaRestError& Error = Result.Error;
		const FString Why = Error.Kind == EVaRestErrorKind::Http ? FString::Printf(TEXT("the refresh endpoint answered %d"), Error.HttpStatus) : Error.Message;
		// Ruling I-7: only the endpoint's own "no" expires the session -- a 4xx other than 408 and 429, or a success that is
		// not JSON (a 2xx Decode error, a body over MaxJsonBytes among them; one without an access token is caught above)
		const int32 Status = Result.Response.Status;
		const bool bRefused = Error.Kind == EVaRestErrorKind::Http && Error.HttpStatus >= 400 && Error.HttpStatus < 500 && Error.HttpStatus != 408 && Error.HttpStatus != 429;
		const bool bMalformed = Error.Kind == EVaRestErrorKind::Decode && Status >= 200 && Status < 300;
		if (bRefused || bMalformed)
		{
			return FVaRestAuthRefreshResult::Rejected(Why);
		}
		// Everything else keeps the session: no answer, a timeout, 408, 429, 5xx, a canceled refresh request (CancelAll, shutdown),
		// and a refusal by the configuration (HTTP disabled, a domain not allowed), whose kind the parked requests fail with.
		// Failed keeps Timeout and Config and makes the rest Connection.
		return FVaRestAuthRefreshResult::Failed(Error.Kind, Why);
	}

	class FBearerRefreshAuth final : public IVaRestAuth
	{
	public:
		explicit FBearerRefreshAuth(const FVaRestBearerRefreshConfig& InConfig)
			: Config(InConfig)
		{
		}

		virtual FName GetAuthName() const override { return TEXT("BearerRefresh"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!Target.Session.HasAccessToken())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
			return true;
		}

		virtual bool CanRefresh(const FVaRestSessionData& Session) const override
		{
			return Session.HasRefreshToken();
		}

		virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const override
		{
			// ClampMin is an editor rule only: a value set in code may be negative or not finite
			const double Configured = FMath::IsFinite(Config.RefreshLeadSeconds) ? FMath::Max(0.0, static_cast<double>(Config.RefreshLeadSeconds)) : 0.0;
			// Concurrency review M-7: at most half the token's lifetime early, recorded when it was set -- a 20 s dev-server
			// token with the 30 s lead would refresh before every request. An import's lifetime is unknown (0, re-review
			// N-10): the configured lead then.
			const double Lead = Session.Lifetime > 0.0 ? FMath::Min(Configured, Session.Lifetime * 0.5) : Configured;
			return Session.HasRefreshToken() && (!Session.HasAccessToken() || (Session.ExpiresAt > 0.0 && Now >= Session.ExpiresAt - Lead));
		}

		virtual bool IsAuthFailure(int32 Status) const override
		{
			return Config.RefreshStatuses.Contains(Status);
		}

		virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
		{
			const FString& Token = Context.GetSession().RefreshToken;
			// Plan decision 23: through the context, so the request skips the auth stages, the envelope (the body is read raw),
			// retry and the limiter's queue (ruling M-8), and carries the refresh's id (spec review I-2)
			TVaRestRequest<FVaRestJson> Refreshing = Context.Request<FVaRestJson>(TEXTVIEW("POST"), Config.RefreshPath);
			if (Config.Style == EVaRestRefreshStyle::OAuth2Form)
			{
				// RFC 6749 6
				FVaRestForm Form;
				Form.Add(TEXT("grant_type"), TEXT("refresh_token"));
				Form.Add(TEXT("refresh_token"), Token);
				if (!Config.ClientId.IsEmpty())
				{
					Form.Add(TEXT("client_id"), Config.ClientId);
				}
				Refreshing = MoveTemp(Refreshing).Body(Form);
			}
			else
			{
				Refreshing = MoveTemp(Refreshing).Body(FVaRestJson::Object().Set(Config.RefreshTokenField, Token));
			}
			// The game thread (no OnHttpThread()): Done may be called from any thread, but the path reads need none
			MoveTemp(Refreshing).OnComplete([Paths = Config, Finish = MoveTemp(Done)](const TVaRestResult<FVaRestJson>& Result) mutable
			{
				Finish(ReadRefresh(Paths, Result));
			}).Send();
		}

		virtual void GetRedactedKeys(TArray<FString>& OutKeys) const override
		{
			// Plan decision 22: added keys match whole normalised keys (M3 decision 18), so the last segment, not the path. The
			// refresh response holds the new tokens before FinishRefresh makes them session secrets (layer 3), and the log stage
			// and the trace read that response first: these keys are what masks them there.
			for (const FString& Path : { Config.AccessTokenPath, Config.RefreshTokenPath })
			{
				const FString Segment = LastSegment(Path);
				if (!Segment.IsEmpty())
				{
					OutKeys.Add(Segment);
				}
			}
			if (!Config.RefreshTokenField.IsEmpty())
			{
				OutKeys.Add(Config.RefreshTokenField);
			}
		}

	private:
		FVaRestBearerRefreshConfig Config;
	};
}

FVaRestAuth::FRef FVaRestAuth::BearerRefresh(const FVaRestBearerRefreshConfig& Config)
{
	return MakeShared<VaRest::Http::BearerRefreshPrivate::FBearerRefreshAuth, ESPMode::ThreadSafe>(Config);
}
