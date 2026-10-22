// Copyright 2014-2026 Vladimir Alyamkin. All Rights Reserved.

#if WITH_DEV_AUTOMATION_TESTS

#include "HAL/IConsoleManager.h"
#include "Misc/AutomationTest.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/Parse.h"
#include "Misc/ScopeExit.h"
#include "UObject/Package.h"
#include "VaRestAuth.h"
#include "VaRestClient.h"
#include "VaRestClientState.h"
#include "VaRestHttpSettings.h"
#include "VaRestHttpTesting.h"

namespace VaRest::Http::AuthTestPrivate
{
	FVaRestFakeResponse Json(int32 Code, const char* Text = "{}")
	{
		return FVaRestFakeResponse::Json(Code, FUtf8StringView(reinterpret_cast<const UTF8CHAR*>(Text)));
	}

	/** The index of the Nth call whose URL path is Path (the query ignored), or INDEX_NONE */
	int32 FindCall(const VaRest::Http::Testing::FTestRig& Rig, const TCHAR* Path, int32 Nth = 0)
	{
		for (int32 Index = 0; Index < Rig.Fake->NumCalls(); ++Index)
		{
			FString Url = Rig.Fake->GetCall(Index).Request.Url;
			int32 Query = INDEX_NONE;
			if (Url.FindChar(TEXT('?'), Query))
			{
				Url.LeftInline(Query);
			}
			if (Url.EndsWith(Path) && Nth-- == 0)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	/** Drains a rig whose test stopped early */
	void Abandon(VaRest::Http::Testing::FTestRig& Rig)
	{
		Rig.Client.CancelAll();
		VaRest::Http::Testing::PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; });
	}

	/** Records the URL each PerAttempt pass sees: the final one, auth query included (plan decision 19) */
	class FUrlProbe final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("UrlProbe"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Seen->Add(Exchange->GetAttemptUrl());
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}
		TSharedRef<TArray<FString>, ESPMode::ThreadSafe> Seen = MakeShared<TArray<FString>, ESPMode::ThreadSafe>();
	};

	/** A Custom auth without credentials: its Apply reports failure, which stays Kind=Auth (ruling Q7's other half) */
	class FEmptyCustomAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("EmptyCustom"); }
		virtual bool Apply(FVaRestAuthTarget&) const override { return false; }
	};

	/** What the client's current redactor makes of Text */
	FString Scrub(const FVaRestClient& Client, const FString& Text)
	{
		const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Client);
		return State.IsValid() ? State->GetRedactor()->ScrubText(Text) : Text;
	}

	/**
	 * Task 14 carry: a custom auth whose slot names are another owner's -- the session's token slot and a game slot. The
	 * core stores them under VaRest.Auth., so they replace nothing and only a new auth retires them.
	 */
	class FSlotNamingAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("SlotNaming"); }
		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			Target.Headers.Set(TEXT("X-Custom-Auth"), TEXT("custom-auth-value-1234"));
			return true;
		}
		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("VaRest.Session.AccessToken"), TEXT("custom-auth-value-1234"));
			OutValues.Emplace(TEXT("VaRest.Game.Slot"), TEXT("custom-game-value-1234"));
		}
	};

	/** Review M-1: a custom auth whose configuration error quotes one of its own declared secrets */
	class FLeakyConfigAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("LeakyConfig"); }
		virtual bool Apply(FVaRestAuthTarget&) const override { return true; }
		virtual FString GetConfigError() const override { return TEXT("the signing key signing-secret-1234 is malformed"); }
		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("Signing"), TEXT("signing-secret-1234"));
		}
	};

	/** Review M-3: a custom auth that returns two values under one slot name */
	class FDuplicateSlotAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("DuplicateSlot"); }
		virtual bool Apply(FVaRestAuthTarget&) const override { return true; }
		virtual void GetSecretValues(TArray<TPair<FName, FString>>& OutValues) const override
		{
			OutValues.Emplace(TEXT("Signing"), TEXT("first-value-123456"));
			OutValues.Emplace(TEXT("Signing"), TEXT("second-value-123456"));
		}
	};

	/** Spec 4.1's policy with InMaxRetries retries and no jitter: each delay exact */
	FVaRestRetryPolicy ExactRetry(int32 InMaxRetries)
	{
		FVaRestRetryPolicy Policy = FVaRestRetryPolicy::Exponential(InMaxRetries);
		Policy.Backoff.Jitter = EVaRestJitter::None;
		return Policy;
	}

	/** Every string a request's trace events and snapshot entries hold: URLs, header values, messages (Task 12 review M-7) */
	TArray<FString> TraceStrings(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 RequestId)
	{
		TArray<FString> Out;
		const auto AddHeaders = [&Out](const FVaRestHeaders& Headers)
		{
			for (const FVaRestHeaders::FPair& Pair : Headers)
			{
				Out.Add(Pair.Value);
			}
		};
		const auto AddStarted = [&Out, &AddHeaders](const FVaRestTraceStarted& Started)
		{
			Out.Add(Started.Url);
			Out.Add(Started.PathTemplate);
			AddHeaders(Started.Headers);
		};
		for (const FVaRestTraceSnapshot& Snapshot : Recorder.GetSnapshots())
		{
			for (const FVaRestTraceStarted& Entry : Snapshot.InFlight)
			{
				if (Entry.RequestId == RequestId)
				{
					AddStarted(Entry);
				}
			}
		}
		for (const FVaRestTraceEvent& Event : Recorder.For(RequestId))
		{
			switch (Event.Kind)
			{
			case EVaRestTraceKind::Started:
				AddStarted(Event.Payload.Get<FVaRestTraceStarted>());
				break;
			case EVaRestTraceKind::AttemptDispatched:
				AddHeaders(Event.Payload.Get<FVaRestTraceAttemptDispatched>().Headers);
				break;
			case EVaRestTraceKind::AttemptEnded:
			{
				const FVaRestTraceAttemptEnded& Ended = Event.Payload.Get<FVaRestTraceAttemptEnded>();
				Out.Add(Ended.EffectiveUrl);
				Out.Add(Ended.ContentType);
				AddHeaders(Ended.Headers);
				break;
			}
			case EVaRestTraceKind::Completed:
			{
				const FVaRestTraceCompleted& Done = Event.Payload.Get<FVaRestTraceCompleted>();
				Out.Add(Done.Error.Message);
				Out.Add(Done.Error.Code);
				break;
			}
			default:
				break;
			}
		}
		return Out;
	}

	/** A bearer whose refresh the test answers by hand: a refresh endpoint as slow as the test wants */
	class FHandAuth final : public IVaRestAuth
	{
	public:
		virtual FName GetAuthName() const override { return TEXT("Hand"); }

		virtual bool Apply(FVaRestAuthTarget& Target) const override
		{
			if (!Target.Session.HasAccessToken())
			{
				return false;
			}
			Target.Headers.Set(TEXT("Authorization"), FString(TEXT("Bearer ")) + Target.Session.AccessToken);
			return true;
		}

		virtual bool CanRefresh(const FVaRestSessionData& Session) const override { return bFromNothing || Session.HasRefreshToken(); }

		virtual bool NeedsRefresh(const FVaRestSessionData& Session, double Now) const override
		{
			return CanRefresh(Session) && (!Session.HasAccessToken() || (Session.ExpiresAt > 0.0 && Now >= Session.ExpiresAt - 30.0));
		}

		virtual void Refresh(const FVaRestAuthRefreshContext& Context, TUniqueFunction<void(FVaRestAuthRefreshResult&&)>&& Done) override
		{
			++Refreshes;
			LastRefreshId = Context.GetRefreshId();
			Pending = MoveTemp(Done);
		}

		bool IsPending() const { return static_cast<bool>(Pending); }

		void Answer(FVaRestAuthRefreshResult&& Result)
		{
			if (Pending)
			{
				TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Done = MoveTemp(Pending);
				Pending.Reset();
				Done(MoveTemp(Result));
			}
		}

		/** Concurrency review I-4: an auth that drops its Done uncalled (an error branch that forgot it) */
		void Drop()
		{
			Pending.Reset();
		}

		int32 Refreshes = 0;
		uint64 LastRefreshId = 0;
		/** Task 16 review M-1: an auth that refreshes from no token at all (a platform ticket): CanRefresh is always true */
		bool bFromNothing = false;

	private:
		TUniqueFunction<void(FVaRestAuthRefreshResult&&)> Pending;
	};

	/** 200 for the accepted tokens, 401 for anything else */
	void AcceptTokens(VaRest::Http::Testing::FTestRig& Rig, TArray<FString> Accepted)
	{
		Rig.Fake->SetResponder([Accepted](const FVaRestTransportRequest& Request)
		{
			const FString Given = Request.Headers.Get(TEXT("Authorization"));
			return Accepted.ContainsByPredicate([&Given](const FString& Token) { return Given == FString(TEXT("Bearer ")) + Token; }) ? Json(200) : Json(401);
		});
	}

	TSharedRef<FHandAuth, ESPMode::ThreadSafe> HandRig(VaRest::Http::Testing::FTestRig& Rig)
	{
		TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = MakeShared<FHandAuth, ESPMode::ThreadSafe>();
		Rig.Client.WithAuth(Auth);
		Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
		AcceptTokens(Rig, { TEXT("new-access-123456") });
		return Auth;
	}

	TArray<FVaRestTraceAttemptDispatched> DispatchesOf(const VaRest::Http::Testing::FTraceRecorder& Recorder, uint64 Id)
	{
		TArray<FVaRestTraceAttemptDispatched> Out;
		for (const FVaRestTraceEvent& Event : Recorder.For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::AttemptDispatched)
			{
				Out.Add(Event.Payload.Get<FVaRestTraceAttemptDispatched>());
			}
		}
		return Out;
	}

	using FRefreshAnswer = TPair<EVaRestAuthRefreshOutcome, EVaRestErrorKind>;
	using FRefreshAnswers = TSharedRef<TArray<FRefreshAnswer>, ESPMode::ThreadSafe>;

	FRefreshAnswers MakeAnswers()
	{
		return MakeShared<TArray<FRefreshAnswer>, ESPMode::ThreadSafe>();
	}

	/** RefreshAuth's callback, recording what it was told */
	TUniqueFunction<void(EVaRestAuthRefreshOutcome, EVaRestErrorKind)> Record(const FRefreshAnswers& Into)
	{
		return [Into](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind FailedKind) { Into->Emplace(Outcome, FailedKind); };
	}

	/**
	 * As VaRest.Http.Retry.QueueDeadlineNotEarly's clock (Task 9 review minor 2): the real clock's core ticker counts
	 * game-time deltas, which a fixed timestep can run faster than Now(), so its timers may fire early -- here at Ratio of
	 * the delay, at least MinStep
	 */
	class FEarlyClock final : public IVaRestClock
	{
	public:
		FEarlyClock(TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> InTime, double InRatio, double InMinStep)
			: Time(MoveTemp(InTime))
			, Ratio(InRatio)
			, MinStep(InMinStep)
		{
		}

		virtual double Now() const override { return Time->Now(); }
		virtual FVaRestTimerHandle SetTimer(double DelaySeconds, TUniqueFunction<void()>&& Callback) override
		{
			return Time->SetTimer(FMath::Max(DelaySeconds * Ratio, MinStep), MoveTemp(Callback));
		}
		virtual void ClearTimer(FVaRestTimerHandle& Handle) override { Time->ClearTimer(Handle); }

	private:
		TSharedRef<FVaRestVirtualClock, ESPMode::ThreadSafe> Time;
		double Ratio = 1.0;
		double MinStep = 0.0;
	};

	/** A PerAttempt stage that remembers each exchange it sees, weakly (a strong one would hold the client: state -> chain -> stage) */
	class FExchangeGrab final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("ExchangeGrab"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Seen.Add(Exchange);
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}

		TArray<TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>> Seen;
	};

	/** A PerAttempt stage that holds each attempt instead of sending it, until the test answers it (game thread, synchronously) */
	class FHeldAttempt final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("HeldAttempt"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			Held = MoveTemp(OnOutcome);
		}

		bool IsHolding() const { return static_cast<bool>(Held); }

		void Answer(int32 Status)
		{
			if (Held)
			{
				FVaRestOutcomeCallback Done = MoveTemp(Held);
				Held.Reset();
				FVaRestAttemptOutcome Outcome;
				Outcome.Status = Status;
				Done(MoveTemp(Outcome));
			}
		}

	private:
		FVaRestOutcomeCallback Held;
	};

	/**
	 * An Outer stage in front of Retry that can answer the request itself while its attempt is still out (a cache serving a
	 * stored body): a JSON 200, which the pipeline then decodes on a task (Phase Decoding until that task comes back)
	 */
	class FEarlyAnswer final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("EarlyAnswer"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			const TSharedRef<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Shared = MakeShared<FVaRestOutcomeCallback, ESPMode::ThreadSafe>(MoveTemp(OnOutcome));
			Up = Shared;
			// The attempt's own outcome, after the early answer, goes nowhere: the stage answered once already
			Next.Proceed(Exchange, [Shared](FVaRestAttemptOutcome&& Outcome)
			{
				if (*Shared)
				{
					FVaRestOutcomeCallback Last = MoveTemp(*Shared);
					Shared->Reset();
					Last(MoveTemp(Outcome));
				}
			});
		}

		void AnswerNow()
		{
			const TSharedPtr<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Shared = Up.Pin();
			if (Shared.IsValid() && *Shared)
			{
				FVaRestOutcomeCallback Last = MoveTemp(*Shared);
				Shared->Reset();
				FVaRestAttemptOutcome Stored;
				Stored.Status = 200;
				Stored.ContentType = TEXT("application/json");
				Stored.Body.Append(reinterpret_cast<const uint8*>("{}"), 2);
				Last(MoveTemp(Stored));
			}
		}

	private:
		TWeakPtr<FVaRestOutcomeCallback, ESPMode::ThreadSafe> Up;
	};

	/** A PerAttempt stage that answers the first attempt itself, before the transport stage (a 503), and sends the rest */
	class FFirstAnswered final : public IVaRestStage
	{
	public:
		virtual FName GetStageName() const override { return TEXT("FirstAnswered"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			if (++Attempts == 1)
			{
				FVaRestAttemptOutcome Busy;
				Busy.Status = 503;
				OnOutcome(MoveTemp(Busy));
				return;
			}
			Next.Proceed(Exchange, MoveTemp(OnOutcome));
		}

		int32 Attempts = 0;
	};

	/**
	 * A PerAttempt stage that breaks its contract (IVaRestStage::Process: "Call OnOutcome exactly once"): it answers each
	 * attempt twice, inside Process, with Status (as VaRest.Http.Retry's FReportsTwice)
	 */
	class FReportsTwice final : public IVaRestStage
	{
	public:
		explicit FReportsTwice(int32 InStatus)
			: Status(InStatus)
		{
		}

		virtual FName GetStageName() const override { return TEXT("ReportsTwice"); }
		virtual void Process(const FVaRestExchangeRef& Exchange, const FVaRestStageNext& Next, FVaRestOutcomeCallback&& OnOutcome) override
		{
			++Attempts;
			for (int32 Pass = 0; Pass < 2; ++Pass)
			{
				FVaRestAttemptOutcome Outcome;
				Outcome.Status = Status;
				OnOutcome(MoveTemp(Outcome));
			}
		}

		int32 Attempts = 0;

	private:
		int32 Status = 0;
	};

	/**
	 * As VaRest.Http.Auth.RefreshIdInUseEnsures: ensureAlways reports every time only while core.EnsureAlwaysEnabled is on
	 * (AssertionMacros.cpp:136-140, :948) and every ensure is handled at HandleEnsurePercent 100
	 * (GenericPlatformMisc.cpp:2058-2068, :2070-2094); under other settings the count is chance. False (and an Info) then.
	 */
	bool EnsuresReportEachTime(FAutomationTestBase& Test)
	{
		IConsoleVariable* Always = IConsoleManager::Get().FindConsoleVariable(TEXT("core.EnsureAlwaysEnabled"));
		float Percent = 100.f;
		if (!GConfig || !GConfig->GetFloat(TEXT("Core.System"), TEXT("HandleEnsurePercent"), Percent, GEngineIni))
		{
			FParse::Value(FCommandLine::Get(), TEXT("handleensurepercent="), Percent);
		}
		if (!Always || !Always->GetBool() || Percent < 100.f)
		{
			Test.AddInfo(TEXT("core.EnsureAlwaysEnabled is off or HandleEnsurePercent is below 100: ensureAlways does not report every failure here; nothing to check"));
			return false;
		}
		return true;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthBearerTest, "VaRest.Http.Auth.Bearer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthBearerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	Rig.Client.WithBearer(TEXT("bearer-token-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("sid-1234567890"));
	auto Plain = MakeOutcome<FVaRestJson>();
	auto Login = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Plain).Send();
	Observe(Rig.Client.Post(TEXT("/login")).NoAuth().Body(TEXT("x")), Login).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Plain, Login] { return Plain->IsDone() && Login->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	const int32 A = FindCall(Rig, TEXT("/a"));
	const int32 L = FindCall(Rig, TEXT("/login"));
	if (!TestTrue(TEXT("both sent"), A != INDEX_NONE && L != INDEX_NONE))
	{
		return false;
	}
	const FVaRestHeaders& Sent = Rig.Fake->GetCall(A).Request.Headers;
	TestEqualSensitive(TEXT("spec 4.2 Bearer: the session's token"), Sent.Get(TEXT("Authorization")), FString(TEXT("Bearer bearer-token-123456")));
	TestEqualSensitive(TEXT("... and the session header"), Sent.Get(TEXT("X-Session-ID")), FString(TEXT("sid-1234567890")));
	const FVaRestHeaders& Bare = Rig.Fake->GetCall(L).Request.Headers;
	TestTrue(TEXT("NoAuth(): neither"), !Bare.Contains(TEXT("Authorization")) && !Bare.Contains(TEXT("X-Session-ID")));

	Rig.Client.ClearSession();
	auto Required = MakeOutcome<FVaRestJson>();
	auto Optional = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/r")), Required).Send();
	Observe(Rig.Client.Get(TEXT("/o")).AuthMode(EVaRestAuthMode::IfAvailable), Optional).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([Required, Optional] { return Required->IsDone() && Optional->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("plan decision 4: Required without credentials is Kind Auth, not retryable, saying what to do"), Required->Error.Kind == EVaRestErrorKind::Auth
		&& !Required->Error.bRetryable && Required->Error.Message.Contains(TEXT("no credentials")));
	TestEqual(TEXT("... and is never sent"), FindCall(Rig, TEXT("/r")), INDEX_NONE);
	const int32 O = FindCall(Rig, TEXT("/o"));
	TestTrue(TEXT("IfAvailable: sent without credentials"), Optional->SuccessCalls.load() == 1 && O != INDEX_NONE
		&& !Rig.Fake->GetCall(O).Request.Headers.Contains(TEXT("Authorization")));

	FTestRig NoAuth;
	auto Unchanged = MakeOutcome<FVaRestJson>();
	Observe(NoAuth.Client.Get(TEXT("/p")), Unchanged).Send();
	TestTrue(TEXT("a client with no auth sends as in M3"), NoAuth.Wait(Unchanged) && Unchanged->SuccessCalls.load() == 1);
	if (!Unchanged->IsDone())
	{
		Abandon(NoAuth);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthKeysTest, "VaRest.Http.Auth.ApiKeyAndBasic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthKeysTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	const FString Key = TEXT("api-key-value-1234");
	FTestRig Header;
	Header.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-Api-Key"), Key));
	FTestRig Query;
	const TSharedRef<FUrlProbe, ESPMode::ThreadSafe> Probe = MakeShared<FUrlProbe, ESPMode::ThreadSafe>();
	Query.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT("app_ref"), Key)).Use(Probe, EVaRestStageSlot::PerAttempt)
		.WithLogLevel(EVaRestLogLevel::Headers);
	FTestRig Basic;
	Basic.Client.WithAuth(FVaRestAuth::Basic(TEXT("neo"), TEXT("trinity-password")));
	FLogCapture Capture;
	auto H = MakeOutcome<FVaRestJson>();
	auto Q = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Header.Client.Get(TEXT("/x")), H).Send();
	Observe(Query.Client.Get(TEXT("/x")).Query(TEXT("page"), 2), Q).Send();
	Observe(Basic.Client.Get(TEXT("/x")), B).Send();
	if (!TestTrue(TEXT("all complete"), PumpUntil([H, Q, B] { return H->IsDone() && Q->IsDone() && B->IsDone(); }))
		|| !TestTrue(TEXT("one call each"), Header.Fake->NumCalls() == 1 && Query.Fake->NumCalls() == 1 && Basic.Fake->NumCalls() == 1))
	{
		Abandon(Header);
		Abandon(Query);
		Abandon(Basic);
		return false;
	}
	TestEqualSensitive(TEXT("ApiKey in a header"), Header.Fake->GetCall(0).Request.Headers.Get(TEXT("X-Api-Key")), Key);
	const FString QueryUrl = Query.Fake->GetCall(0).Request.Url;
	TestEqualSensitive(TEXT("ApiKey in the query, after the request's own pairs"), QueryUrl, FString(TEXT("https://api.test/x?page=2&app_ref=api-key-value-1234")));
	TestTrue(TEXT("a PerAttempt stage sees the final URL (GetAttemptUrl)"), Probe->Seen->Num() == 1 && (*Probe->Seen)[0] == QueryUrl);
	TestEqualSensitive(TEXT("Basic: base64 of user:password"), Basic.Fake->GetCall(0).Request.Headers.Get(TEXT("Authorization")),
		FString(TEXT("Basic bmVvOnRyaW5pdHktcGFzc3dvcmQ=")));
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> QueryRedactor = VaRest::Http::FClientAccess::State(Query.Client)->GetRedactor();
	// "app_ref" matches no built-in rule (VaRestJsonRedact.h), so only the auth's added key redacts it
	TestTrue(TEXT("IN-13 layer 2: the query name is a key"), QueryRedactor->GetRules().ShouldRedact(FStringView(TEXT("app_ref")), EVaRestRedactKeyKind::Query));
	TestEqualSensitive(TEXT("layer 3: the key is a secret"), QueryRedactor->ScrubText(Key), FString(TEXT("***")));
	const TSharedRef<const FVaRestRedactor, ESPMode::ThreadSafe> BasicRedactor = VaRest::Http::FClientAccess::State(Basic.Client)->GetRedactor();
	TestEqualSensitive(TEXT("... so are the Basic credential and the password"), BasicRedactor->ScrubText(TEXT("bmVvOnRyaW5pdHktcGFzc3dvcmQ= trinity-password")),
		FString(TEXT("*** ***")));
	TestFalse(TEXT("no log line holds the key"), Capture.GetLines().ContainsByPredicate([&Key](const FString& Line) { return Line.Contains(Key); }));
	TestTrue(TEXT("... the request line shows it redacted"), Capture.Contains(TEXT("app_ref=***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthApplyTest, "VaRest.Http.Auth.ApplyToHandshake",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthApplyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	// WebSocket spec 19 item 2 / 3.3: a socket applies the client's session and auth to its handshake
	FTestRig Rig;
	Rig.Client.WithBearer(TEXT("bearer-token-123456"));
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), TEXT("sid-1234567890"));
	FVaRestHeaders Headers;
	TArray<TPair<FString, FString>> Query;
	TestTrue(TEXT("applied"), Rig.Client.ApplyAuth(Headers, Query));
	TestTrue(TEXT("... the token and the session header"), Headers.Get(TEXT("Authorization")) == TEXT("Bearer bearer-token-123456")
		&& Headers.Get(TEXT("X-Session-ID")) == TEXT("sid-1234567890") && Query.Num() == 0);
	Rig.Client.ClearSession();
	FVaRestHeaders After;
	TestFalse(TEXT("no credentials: false"), Rig.Client.ApplyAuth(After, Query));
	FString NoConfigError;
	TestTrue(TEXT("... and no configuration error: a socket reports Kind=Auth"), !Rig.Client.ApplyAuth(After, Query, &NoConfigError) && NoConfigError.IsEmpty());
	FTestRig Plain;
	FVaRestHeaders Nothing;
	TestTrue(TEXT("no auth at all: true, nothing written"), Plain.Client.ApplyAuth(Nothing, Query) && Nothing.IsEmpty());
	// Ruling Q7 for sockets (spec review C-1): a misconfigured built-in is false with the reason, so W5 maps it to Kind=Config
	AddExpectedErrorPlain(TEXT("VaRest: FVaRestAuth::Basic: the user holds ':'"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig Broken;
	Broken.Client.WithAuth(FVaRestAuth::Basic(TEXT("a:b"), TEXT("password-123456")));
	FVaRestHeaders Unused;
	FString ConfigError;
	TestTrue(TEXT("a misconfigured auth: false, saying why"), !Broken.Client.ApplyAuth(Unused, Query, &ConfigError) && ConfigError.Contains(TEXT("':'")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthInvalidBuiltinTest, "VaRest.Http.Auth.InvalidBuiltinAuth",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthInvalidBuiltinTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Ruling Q7 (spec review C-1): a built-in auth whose configuration cannot work is a programmer error -- WithAuth's
	// factory logs an Error once, and every request fails Kind=Config, not retryable, without being sent, so a game that
	// shows a login screen on Kind=Auth does not show one for a typo
	AddExpectedErrorPlain(TEXT("VaRest: FVaRestAuth::ApiKey: the header name"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("VaRest: FVaRestAuth::ApiKey: the query key is empty"), EAutomationExpectedErrorFlags::Contains, 1);
	AddExpectedErrorPlain(TEXT("VaRest: FVaRestAuth::Basic: the user holds ':'"), EAutomationExpectedErrorFlags::Contains, 1);
	// Review M-2 ruling: an empty key is configuration too -- one missing from the ini must not send the player to a login
	AddExpectedErrorPlain(TEXT("VaRest: FVaRestAuth::ApiKey: the key is empty"), EAutomationExpectedErrorFlags::Contains, 1);
	FTestRig BadHeader;
	BadHeader.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("Bad Name"), TEXT("key-value-123456")));
	FTestRig EmptyQuery;
	EmptyQuery.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT(""), TEXT("key-value-123456")));
	FTestRig BadUser;
	BadUser.Client.WithAuth(FVaRestAuth::Basic(TEXT("a:b"), TEXT("password-123456")));
	FTestRig EmptyKey;
	EmptyKey.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-Api-Key"), TEXT("")));
	FTestRig* const Rigs[] = { &BadHeader, &EmptyQuery, &BadUser, &EmptyKey };
	const TCHAR* const Names[] = { TEXT("ApiKey header \"Bad Name\""), TEXT("ApiKey query \"\""), TEXT("Basic user \"a:b\""), TEXT("ApiKey key \"\"") };
	const TCHAR* const Reasons[] = { TEXT("header name is not an RFC 9110 token"), TEXT("query key is empty"), TEXT("holds ':'"), TEXT("the API key is empty") };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Rigs); ++Index)
	{
		FTestRig& Rig = *Rigs[Index];
		auto Required = MakeOutcome<FVaRestJson>();
		auto Optional = MakeOutcome<FVaRestJson>();
		Observe(Rig.Client.Get(TEXT("/x")), Required).Send();
		Observe(Rig.Client.Get(TEXT("/y")).AuthMode(EVaRestAuthMode::IfAvailable), Optional).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: both complete"), Names[Index]), PumpUntil([Required, Optional] { return Required->IsDone() && Optional->IsDone(); })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Drain();
		for (const auto& Outcome : { Required, Optional })
		{
			TestTrue(FString::Printf(TEXT("%s: Kind Config, not retryable, AuthInvalid"), Names[Index]), Outcome->Error.Kind == EVaRestErrorKind::Config
				&& !Outcome->Error.bRetryable && Outcome->Error.Message.Contains(TEXT("AuthInvalid")));
			TestTrue(FString::Printf(TEXT("%s: the message says why"), Names[Index]), Outcome->Error.Message.Contains(Reasons[Index]));
		}
		TestEqual(FString::Printf(TEXT("%s: nothing sent"), Names[Index]), Rig.Fake->NumCalls(), 0);
	}
	auto Login = MakeOutcome<FVaRestJson>();
	Observe(BadHeader.Client.Post(TEXT("/login")).NoAuth().Body(TEXT("x")), Login).Send();
	TestTrue(TEXT("NoAuth() still goes out on a misconfigured client (a login)"), BadHeader.Wait(Login) && Login->SuccessCalls.load() == 1 && BadHeader.Fake->NumCalls() == 1);
	if (!Login->IsDone())
	{
		Abandon(BadHeader);
	}

	// The control: a Custom auth with no credentials is Kind=Auth, as before
	FTestRig Custom;
	Custom.Client.WithAuth(MakeShared<FEmptyCustomAuth, ESPMode::ThreadSafe>());
	auto NoToken = MakeOutcome<FVaRestJson>();
	Observe(Custom.Client.Get(TEXT("/z")), NoToken).Send();
	TestTrue(TEXT("a Custom auth that cannot apply: Kind Auth"), Custom.Wait(NoToken) && NoToken->Error.Kind == EVaRestErrorKind::Auth && Custom.Fake->NumCalls() == 0);
	if (!NoToken->IsDone())
	{
		Abandon(Custom);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthQueuedTest, "VaRest.Http.Auth.QueuedGetsNewToken",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthQueuedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Ruling I-3: credentials are written when a request leaves the limiter's queue (Task 4), so a token set while it waits
	// is the one it goes out with, and a logout while it waits stops it before the network
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithBearer(TEXT("old-token-123456"));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(200); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	for (int32 Half = 0; Half < 2; ++Half)
	{
		const TCHAR* const Change = Half == 0 ? TEXT("a token set while queued") : TEXT("a logout while queued");
		auto Holder = MakeOutcome<FVaRestJson>();
		auto Queued = MakeOutcome<FVaRestJson>();
		const int32 Before = Rig.Fake->NumCalls();
		Observe(Rig.Client.Get(TEXT("/hold")).NoAuth(), Holder).Send();
		Observe(Rig.Client.Get(TEXT("/queued")), Queued).Send();
		if (!TestTrue(FString::Printf(TEXT("%s: the holder has the slot, the other waits"), Change),
			PumpUntil([&Rig, &Slots, Before] { return Rig.Fake->NumCalls() == Before + 1 && Slots.NumQueued() == 1; })))
		{
			Abandon(Rig);
			return false;
		}
		if (Half == 0)
		{
			Rig.Client.SetBearerToken(TEXT("new-token-123456"));
		}
		else
		{
			Rig.Client.ClearSession();
		}
		Rig.Fake->Complete(Before, Json(200));
		if (!TestTrue(FString::Printf(TEXT("%s: both complete"), Change), PumpUntil([Holder, Queued] { return Holder->IsDone() && Queued->IsDone(); })))
		{
			Abandon(Rig);
			return false;
		}
		Rig.Drain();
		if (Half == 0)
		{
			const int32 Call = FindCall(Rig, TEXT("/queued"));
			TestTrue(TEXT("a token set while queued: its first and only call carries it"), Call != INDEX_NONE && FindCall(Rig, TEXT("/queued"), 1) == INDEX_NONE
				&& Rig.Fake->GetCall(Call).Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-token-123456") && Queued->SuccessCalls.load() == 1);
		}
		else
		{
			TestEqual(TEXT("a logout while queued: never sent"), Rig.Fake->NumCalls(), Before + 1);
			TestTrue(TEXT("... it fails Kind Auth, no credentials"), Queued->Error.Kind == EVaRestErrorKind::Auth && Queued->Error.Message.Contains(TEXT("NoCredentials")));
		}
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthFlagsTest, "VaRest.Http.Auth.TraceFlags",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthFlagsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	FTestRig Rig;
	Rig.Client.WithBearer(TEXT("bearer-token-123456"));
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto With = MakeOutcome<FVaRestJson>();
	auto Without = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle A = Observe(Rig.Client.Get(TEXT("/a")), With).Send();
	const FVaRestRequestHandle B = Observe(Rig.Client.Get(TEXT("/b")).NoAuth(), Without).Send();
	if (!TestTrue(TEXT("both complete"), PumpUntil([With, Without] { return With->IsDone() && Without->IsDone(); })))
	{
		// Spec review M-5: drained before the early return
		VaRest::Http::AuthTestPrivate::Abandon(Rig);
		return false;
	}
	const auto FlagsOf = [&Scoped](uint64 Id)
	{
		for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(Id))
		{
			if (Event.Kind == EVaRestTraceKind::Started)
			{
				return Event.Payload.Get<FVaRestTraceStarted>().Flags;
			}
		}
		return EVaRestTraceRequestFlags::Partial;
	};
	TestFalse(TEXT("IN-1: an authenticated request has no NoAuth flag"), EnumHasAnyFlags(FlagsOf(A.GetId()), EVaRestTraceRequestFlags::NoAuth | EVaRestTraceRequestFlags::Partial));
	TestTrue(TEXT("... NoAuth() has it"), EnumHasAnyFlags(FlagsOf(B.GetId()), EVaRestTraceRequestFlags::NoAuth));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthAppliedRedactedTest, "VaRest.Http.Auth.AppliedTokenStaysRedacted",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthAppliedRedactedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 12 review M-7: AuthApply writes the session as it is when the attempt leaves the queue (plan decision 6). A token
	// and a session header set while the request waits are in neither its Send() snapshot nor, after a logout while the
	// transport has it, the client's current redactor: only what the exchange remembers at AuthApply keeps them scrubbed.
	// The request's own query carries the token under a key no rule names, so only layer 3 hides it there (in its outcome
	// line, its error's URL, the trace's effective URL); the response echoes the session header (a "<" line, the trace).
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	const FString Token = TEXT("tok-late-123456");
	const FString SessionId = TEXT("sid-late-7654321");
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithAuth(FVaRestAuth::Bearer()).WithLogLevel(EVaRestLogLevel::Headers);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("/late?q=*** -> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	auto Holder = MakeOutcome<FVaRestJson>();
	auto Late = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")).NoAuth(), Holder).Send();
	const FVaRestRequestHandle LateHandle = Observe(Rig.Client.Get(TEXT("/late")).Query(TEXT("q"), Token), Late).Send();
	if (!TestTrue(TEXT("no session at Send(): the holder has the slot, the request waits"),
		PumpUntil([&Rig, &Slots] { return Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetBearerToken(Token);
	Rig.Client.SetSessionHeader(TEXT("X-Session-ID"), SessionId);
	// Attached once the client knows both values: what reaches this sink after AuthApply is what the row is about (the
	// Started event, written at Enter, held a value nobody had declared a secret yet)
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata | EVaRestTraceDetail::Headers);
	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("released: AuthApply wrote the session and the transport has the request"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	const FVaRestHeaders& Sent = Rig.Fake->GetCall(1).Request.Headers;
	TestTrue(TEXT("... with the token and the session header set while it waited"),
		Sent.Get(TEXT("Authorization")) == FString(TEXT("Bearer ")) + Token && Sent.Get(TEXT("X-Session-ID")) == SessionId);
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("the logout: the client itself forgot the token"), Scrub(Rig.Client, Token), Token);
	TestEqualSensitive(TEXT("... and the session header's value"), Scrub(Rig.Client, SessionId), SessionId);
	FVaRestFakeResponse Echo = Json(404);
	Echo.Headers.Set(TEXT("X-Echo"), SessionId);
	Rig.Fake->Complete(1, MoveTemp(Echo));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Holder, Late] { return Holder->IsDone() && Late->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	const TArray<FString> Lines = Capture.GetLines();
	const bool bLeaked = Lines.ContainsByPredicate([&Token, &SessionId](const FString& Line) { return Line.Contains(Token) || Line.Contains(SessionId); });
	TestFalse(TEXT("no log line holds the token or the session header's value"), bLeaked);
	TestTrue(TEXT("... the outcome line shows the token as ***"), Capture.Contains(TEXT("/late?q=*** -> 404")));
	TestTrue(TEXT("... and the echoed header as ***"), Capture.Contains(TEXT("<   X-Echo: ***")));
	TestTrue(TEXT("the error: Kind Http 404"), Late->Error.Kind == EVaRestErrorKind::Http && Late->Error.HttpStatus == 404);
	TestFalse(TEXT("... its URL scrubs the token"), Late->Error.Url.Contains(Token));
	const TArray<FString> Traced = TraceStrings(*Scoped.Recorder, LateHandle.GetId());
	TestEqual(TEXT("the trace has the attempt and the end"), Scoped.Recorder->KindsOf(LateHandle.GetId()), FString(TEXT("Dispatched,Ended,Completed")));
	TestFalse(TEXT("... and no trace string holds either value"),
		Traced.ContainsByPredicate([&Token, &SessionId](const FString& Text) { return Text.Contains(Token) || Text.Contains(SessionId); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRetiresSlotsTest, "VaRest.Http.Auth.ReplacedAuthRetiresSlots",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRetiresSlotsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 14 carry: an auth replaced (WithAuth) or removed (WithoutAuth) retires its values, as an import retires a header
	// it drops (Task 13): still scrubbed at once -- a request that carried them may still be logging -- then forgotten after
	// Inspector D21's 10 minutes, not kept current forever
	const FString Key = TEXT("api-key-value-1234");
	const FString Password = TEXT("trinity-password");
	FTestRig Rig;
	Rig.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-Api-Key"), Key));
	TestEqualSensitive(TEXT("ApiKey: the key is a secret"), Scrub(Rig.Client, Key), FString(TEXT("***")));
	// Task 12 review M-1: an auth's slots are neither the game's nor the session's, so neither door forgets them
	Rig.Client.SetBearerToken(TEXT("session-token-123456"));
	Rig.Client.ClearSecretValues();
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("... ClearSecretValues and ClearSession leave it"), Scrub(Rig.Client, Key), FString(TEXT("***")));
	Rig.Client.WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Header, TEXT("X-Api-Key"), Key));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("the same auth again: its slot stays current"), Scrub(Rig.Client, Key), FString(TEXT("***")));

	Rig.Client.WithAuth(FVaRestAuth::Basic(TEXT("neo"), Password));
	TestEqualSensitive(TEXT("switched to Basic: the key is retired, still scrubbed at once"), Scrub(Rig.Client, Key), FString(TEXT("***")));
	TestEqualSensitive(TEXT("... Basic's password is known"), Scrub(Rig.Client, Password), FString(TEXT("***")));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("... the key is forgotten after its 10 minutes"), Scrub(Rig.Client, Key), Key);
	TestEqualSensitive(TEXT("... the current auth's values are not"), Scrub(Rig.Client, Password), FString(TEXT("***")));

	Rig.Client.WithoutAuth();
	TestEqualSensitive(TEXT("WithoutAuth: Basic's password is retired, still scrubbed at once"), Scrub(Rig.Client, Password), FString(TEXT("***")));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("... and forgotten after its 10 minutes"), Scrub(Rig.Client, Password), Password);
	TestEqualSensitive(TEXT("... the credential too"), Scrub(Rig.Client, TEXT("bmVvOnRyaW5pdHktcGFzc3dvcmQ=")), FString(TEXT("bmVvOnRyaW5pdHktcGFzc3dvcmQ=")));
	auto Plain = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/p")), Plain).Send();
	TestTrue(TEXT("no auth: a request goes out without credentials"), Rig.Wait(Plain) && Plain->SuccessCalls.load() == 1 && Rig.Fake->NumCalls() == 1
		&& !Rig.Fake->GetCall(0).Request.Headers.Contains(TEXT("Authorization")));
	if (!Plain->IsDone())
	{
		Abandon(Rig);
	}
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthCustomSlotsTest, "VaRest.Http.Auth.CustomSlotsStayTheAuths",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthCustomSlotsTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 14 carry: a custom auth's GetSecretValues names any slot. Stored under VaRest.Auth., a name that is the session's
	// token slot or a game slot replaces neither: the live token is not rotated out (it would expire after 10 minutes while
	// still in use), and the auth's value survives ClearSession and ClearSecretValues, which forget only their owners'
	const FString Token = TEXT("session-token-123456");
	const FString AuthValue = TEXT("custom-auth-value-1234");
	const FString GameNamed = TEXT("custom-game-value-1234");
	FTestRig Rig;
	Rig.Client.SetBearerToken(Token);
	Rig.Client.WithAuth(MakeShared<FSlotNamingAuth, ESPMode::ThreadSafe>());
	TestTrue(TEXT("the token and the auth's values are known"), Scrub(Rig.Client, Token) == TEXT("***") && Scrub(Rig.Client, AuthValue) == TEXT("***")
		&& Scrub(Rig.Client, GameNamed) == TEXT("***"));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("the session's token was not replaced: still scrubbed after 10 minutes"), Scrub(Rig.Client, Token), FString(TEXT("***")));
	Rig.Client.ClearSecretValues();
	TestEqualSensitive(TEXT("ClearSecretValues: the auth's value named like a game slot stays"), Scrub(Rig.Client, GameNamed), FString(TEXT("***")));
	Rig.Client.ClearSession();
	TestEqualSensitive(TEXT("ClearSession forgets the session's token"), Scrub(Rig.Client, Token), Token);
	TestEqualSensitive(TEXT("... not the auth's value named like the token's slot"), Scrub(Rig.Client, AuthValue), FString(TEXT("***")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthConfigErrorScrubbedTest, "VaRest.Http.Auth.ConfigErrorScrubbed",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthConfigErrorScrubbedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Review M-1: a custom auth's GetConfigError is third-party text that the core puts into FVaRestError::Message (and a
	// socket's reason, through ApplyAuth): a declared secret in it is scrubbed there, as everywhere else
	const FString Secret = TEXT("signing-secret-1234");
	FTestRig Rig;
	Rig.Client.WithAuth(MakeShared<FLeakyConfigAuth, ESPMode::ThreadSafe>());
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/x")), Refused).Send();
	if (!TestTrue(TEXT("completes"), Rig.Wait(Refused)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("Kind Config, AuthInvalid, nothing sent"), Refused->Error.Kind == EVaRestErrorKind::Config
		&& Refused->Error.Message.Contains(TEXT("AuthInvalid")) && Rig.Fake->NumCalls() == 0);
	TestFalse(TEXT("Error.Message holds no declared secret"), Refused->Error.Message.Contains(Secret));
	TestTrue(TEXT("... it shows the auth's text with ***"), Refused->Error.Message.Contains(TEXT("the signing key *** is malformed")));
	FVaRestHeaders Headers;
	TArray<TPair<FString, FString>> Query;
	FString Reason;
	TestFalse(TEXT("ApplyAuth: false for the misconfigured auth"), Rig.Client.ApplyAuth(Headers, Query, &Reason));
	TestTrue(TEXT("... its reason scrubbed the same way"), !Reason.Contains(Secret) && Reason.Contains(TEXT("the signing key *** is malformed")));
	// Review M-5: a reused string never reads stale
	FTestRig Plain;
	FString Stale = TEXT("a reason from an earlier call");
	FVaRestHeaders Nothing;
	TestTrue(TEXT("a healthy client: true, and OutConfigError emptied"), Plain.Client.ApplyAuth(Nothing, Query, &Stale) && Stale.IsEmpty());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthDuplicateSlotTest, "VaRest.Http.Auth.DuplicateSlotLastWins",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthDuplicateSlotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Review M-3: two values under one slot name -- SetAuth sets them in turn, so the last stays current and the first is
	// retired (known for 10 minutes, then forgotten), as IVaRestAuth::GetSecretValues says. SetAuth reports it with an
	// ensureMsgf, which fires once per session (AssertionMacros.cpp:970-975); the thread's ensure handler takes it here, as in
	// VaRest.Http.Limiter.DoubleReleaseEnsuresEachTime, so it leaves no callstack (FDebug::EnsureFailed asks it first,
	// AssertionMacros.cpp:499).
#if DO_ENSURE
	const TFunction<bool(const FEnsureHandlerArgs&)> Outer = GetEnsureHandler();
	SetEnsureHandler([Outer](const FEnsureHandlerArgs& Args)
	{
		if (Args.Message && FCString::Strstr(Args.Message, TEXT("returns the secret slot")))
		{
			return true;
		}
		return Outer ? Outer(Args) : false;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(Outer); };
#endif
	FTestRig Rig;
	Rig.Client.WithAuth(MakeShared<FDuplicateSlotAuth, ESPMode::ThreadSafe>());
	TestTrue(TEXT("both values known at once"), Scrub(Rig.Client, TEXT("first-value-123456")) == TEXT("***") && Scrub(Rig.Client, TEXT("second-value-123456")) == TEXT("***"));
	Rig.Clock->Advance(FVaRestSecretSet::PreviousLifetime + 1.0);
	TestEqualSensitive(TEXT("the last value stays current"), Scrub(Rig.Client, TEXT("second-value-123456")), FString(TEXT("***")));
	TestEqualSensitive(TEXT("the first was retired: forgotten after the window"), Scrub(Rig.Client, TEXT("first-value-123456")), FString(TEXT("first-value-123456")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRetryTest, "VaRest.Http.Auth.RetryAppliesAndRedactsPerAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRetryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Review M-7, the two halves no single attempt reaches:
	// - The attempt URL is per attempt: the boundary resets it, so a retry carries the query key once, not once per attempt.
	// - The applied join accumulates. The first token is set while the request queues, so attempt 1 applies it. A logout
	//   and a second token come between the attempts, so attempt 2 applies under a redactor that never knew the first. A
	//   logout before the final outcome leaves the client knowing neither. The request's query carries the first token
	//   (its key no rule names) and the final response echoes the second.
	TGuardConsoleVariable<int32> Level(TEXT("VaRest.LogLevel"), -1);
	const FString Key = TEXT("api-key-value-1234");
	const FString First = TEXT("tok-first-123456");
	const FString Second = TEXT("tok-second-123456");
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithAuth(FVaRestAuth::ApiKey(EVaRestApiKeyPlacement::Query, TEXT("app_ref"), Key)).WithRetry(ExactRetry(1))
		.WithLogLevel(EVaRestLogLevel::Headers);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const VaRest::Http::FLimiter& Slots = VaRest::Http::FClientAccess::State(Rig.Client)->GetLimiter();
	FLogCapture Capture;
	AddExpectedMessagePlain(TEXT("/r?q=***&app_ref=*** -> 503"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	AddExpectedMessagePlain(TEXT("/r?q=***&app_ref=*** -> 404"), ELogVerbosity::Warning, EAutomationExpectedMessageFlags::Contains, 1);
	auto Holder = MakeOutcome<FVaRestJson>();
	auto Retried = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")).NoAuth(), Holder).Send();
	Observe(Rig.Client.Get(TEXT("/r")).Query(TEXT("q"), First), Retried).Send();
	if (!TestTrue(TEXT("the holder has the slot, the request waits"), PumpUntil([&Rig, &Slots] { return Rig.Fake->NumCalls() == 1 && Slots.NumQueued() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.SetBearerToken(First);
	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("attempt 1 sent"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Fake->Complete(1, Json(503));
	if (!TestTrue(TEXT("a 503: the backoff is armed"), PumpUntil([&Rig] { return Rig.Clock->NumPendingTimers() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.ClearSession();
	Rig.Client.SetBearerToken(Second);
	Rig.Clock->Advance(1.0);
	if (!TestTrue(TEXT("attempt 2 sent"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.ClearSession();
	TestTrue(TEXT("the client knows neither token now"), Scrub(Rig.Client, First) == First && Scrub(Rig.Client, Second) == Second);
	FVaRestFakeResponse Echo = Json(404);
	Echo.Headers.Set(TEXT("X-Echo"), Second);
	Rig.Fake->Complete(2, MoveTemp(Echo));
	if (!TestTrue(TEXT("both complete"), PumpUntil([Holder, Retried] { return Holder->IsDone() && Retried->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	FlushRedactionPipe();
	TestEqualSensitive(TEXT("the retry's call carries the key once, after the request's own pairs"), Rig.Fake->GetCall(2).Request.Url,
		FString(TEXT("https://api.test/r?q=tok-first-123456&app_ref=api-key-value-1234")));
	TestTrue(TEXT("two attempts, the last a 404"), Retried->Error.Kind == EVaRestErrorKind::Http && Retried->Error.HttpStatus == 404 && Retried->Error.Attempts == 2);
	const TArray<FString> Lines = Capture.GetLines();
	TestFalse(TEXT("no log line holds either token"),
		Lines.ContainsByPredicate([&First, &Second](const FString& Line) { return Line.Contains(First) || Line.Contains(Second); }));
	TestTrue(TEXT("... the echoed second token shows as ***"), Capture.Contains(TEXT("<   X-Echo: ***")));
	TestFalse(TEXT("Error.Url holds neither"), Retried->Error.Url.Contains(First) || Retried->Error.Url.Contains(Second));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshContextTest, "VaRest.Http.Auth.RefreshContext",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshContextTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 23: what Context.Request() sends -- no credentials, no retry, no envelope; past a full limiter (ruling
	// M-8); the first one under the refresh's id (spec review I-2); and a context that does not keep its client (M-10)
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithBearer(TEXT("bearer-token-123456")).WithRetry(FVaRestRetryPolicy::Exponential(3));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/hold")) ? FVaRestFakeResponse::Manual() : Json(503); });
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Holder = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")), Holder).Send();
	if (!TestTrue(TEXT("the only slot is held"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	const uint64 RefreshId = FVaRestTrace::NewRequestId();
	// Task 15 review M-1: the constructor is private; the core and the tests make one through FClientAccess
	const FVaRestAuthRefreshContext Context = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, Rig.Client.GetSession(), RefreshId);
	// Re-review N-9: an auth that keeps the context by value, copied before any request
	const FVaRestAuthRefreshContext Copy = Context;
	auto First = MakeOutcome<FVaRestJson>();
	auto Second = MakeOutcome<FVaRestJson>();
	// Re-review N-2: Idempotent(), so the client's Exponential(3) would retry it -- only MarkAuthRefresh's Disabled() stops that
	const FVaRestRequestHandle FirstHandle = Observe(Context.Request(TEXT("POST"), TEXT("/auth/refresh")).Idempotent().Body(TEXT("{}")), First).Send();
	const FVaRestRequestHandle SecondHandle = Observe(Context.Request(TEXT("POST"), TEXT("/auth/other")).Body(TEXT("{}")), Second).Send();
	if (!TestTrue(TEXT("ruling M-8: both complete although the only slot is held"), PumpUntil([First, Second] { return First->IsDone() && Second->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("spec review I-2: the first carries the refresh's id as its own"), FirstHandle.GetId(), RefreshId);
	TestTrue(TEXT("... a second one gets a fresh id"), SecondHandle.GetId() != RefreshId && SecondHandle.GetId() != 0);
	auto FromCopy = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle CopyHandle = Observe(Copy.Request(TEXT("POST"), TEXT("/auth/copy")).Body(TEXT("{}")), FromCopy).Send();
	TestTrue(TEXT("... and so does one from a copy of the context (re-review N-9)"), Rig.Wait(FromCopy) && CopyHandle.GetId() != RefreshId);
	const int32 Call = FindCall(Rig, TEXT("/auth/refresh"));
	if (!TestTrue(TEXT("sent"), Call != INDEX_NONE))
	{
		Abandon(Rig);
		return false;
	}
	TestFalse(TEXT("plan decision 23: no credentials on a refresh request"), Rig.Fake->GetCall(Call).Request.Headers.Contains(TEXT("Authorization")));
	TestTrue(TEXT("... no retry: the 503 is the answer, after one attempt"), First->Error.HttpStatus == 503 && First->Error.Attempts == 1);
	TestEqual(TEXT("... and one transport call"), FindCall(Rig, TEXT("/auth/refresh"), 1), INDEX_NONE);
	EVaRestTraceRequestFlags Flags = EVaRestTraceRequestFlags::Partial;
	for (const FVaRestTraceEvent& Event : Scoped.Recorder->For(RefreshId))
	{
		if (Event.Kind == EVaRestTraceKind::Started)
		{
			Flags = Event.Payload.Get<FVaRestTraceStarted>().Flags;
		}
	}
	TestTrue(TEXT("IN-1: flagged AuthRefresh"), EnumHasAnyFlags(Flags, EVaRestTraceRequestFlags::AuthRefresh) && !EnumHasAnyFlags(Flags, EVaRestTraceRequestFlags::Partial));

	// Concurrency review M-10: an auth may keep its context to answer later; the context must not keep the client alive
	TUniquePtr<FVaRestAuthRefreshContext> Kept;
	TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> GoneState;
	{
		FVaRestClient Gone = FVaRestClient::Make(TEXT("https://gone.test"));
		GoneState = VaRest::Http::FClientAccess::State(Gone);
		Kept = MakeUnique<FVaRestAuthRefreshContext>(VaRest::Http::FClientAccess::MakeRefreshContext(Gone, FVaRestSessionData(), FVaRestTrace::NewRequestId()));
	}
	TestFalse(TEXT("M-10: the context does not keep its client alive"), GoneState.IsValid());
	// Re-review N-1: Send() on an empty client logs this Error before it delivers Kind=Config (VaRestClient.cpp:152-156)
	AddExpectedErrorPlain(TEXT("Send() on an empty FVaRestClient"), EAutomationExpectedErrorFlags::Contains, 1);
	auto Orphan = MakeOutcome<FVaRestJson>();
	Observe(Kept->Request(TEXT("POST"), TEXT("/auth/refresh")), Orphan).Send();
	TestTrue(TEXT("... and a request it builds then fails Kind Config"), PumpUntil([Orphan] { return Orphan->IsDone(); }) && Orphan->Error.Kind == EVaRestErrorKind::Config);
	Rig.Fake->Complete(0, Json(200));
	TestTrue(TEXT("the holder finishes"), Rig.Wait(Holder));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshBypassTest, "VaRest.Http.Auth.RefreshBypass",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshBypassTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 23, past RefreshContext: the limiter's books are untouched by a request that skipped its queue (ruling
	// M-8); the client's envelope never judges the refresh body; the client's retry never repeats it; and the auth's own
	// chain on Context.Request() cannot undo any of it
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithBearer(TEXT("bearer-token-123456")).WithRetry(ExactRetry(2))
		.WithEnvelope(FVaRestEnvelope::DataAt(TEXT("data")).SuccessWhen(TEXT("ok"), { TEXT("true") }));
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		if (Request.Url.EndsWith(TEXT("/hold")) || Request.Url.EndsWith(TEXT("/auth/refresh")))
		{
			return FVaRestFakeResponse::Manual();
		}
		if (Request.Url.EndsWith(TEXT("/auth/busy")))
		{
			return Json(503);
		}
		if (Request.Url.EndsWith(TEXT("/queued")))
		{
			return Json(200, R"({"ok":true,"data":{"n":1}})");
		}
		// The client's envelope fails this body: "ok" is false
		return Json(200, R"({"ok":false,"accessToken":"fresh-access-token-123"})");
	});
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> ClientState = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), ClientState.IsValid()))
	{
		return false;
	}
	VaRest::Http::FLimiter& Limiter = ClientState->GetLimiter();
	auto Holder = MakeOutcome<FVaRestJson>();
	auto Queued = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")), Holder).Send();
	Observe(Rig.Client.Get(TEXT("/queued")), Queued).Send();
	if (!TestTrue(TEXT("the only slot is held, a request queued behind it"), PumpUntil([&Rig, &Limiter] { return Rig.Fake->NumCalls() == 1 && Limiter.NumQueued() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	const int32 ActiveBefore = Limiter.NumActive();
	const int32 QueuedBefore = Limiter.NumQueued();
	const int32 InFlightBefore = Rig.Client.GetInFlightCount();
	TestTrue(TEXT("one held, one queued, two in flight"), ActiveBefore == 1 && QueuedBefore == 1 && InFlightBefore == 2);

	const FVaRestAuthRefreshContext Context = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, Rig.Client.GetSession(), FVaRestTrace::NewRequestId());
	auto Raw = MakeOutcome<FVaRestJson>();
	Observe(Context.Request(TEXT("POST"), TEXT("/auth/refresh")).Body(TEXT("{}")), Raw).Send();
	if (!TestTrue(TEXT("ruling M-8: the refresh request goes out past the full limiter"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... taking no slot and leaving the queue as it was"), Limiter.NumActive() == ActiveBefore && Limiter.NumQueued() == QueuedBefore);
	TestEqual(TEXT("... counted in flight as any request"), Rig.Client.GetInFlightCount(), InFlightBefore + 1);
	Rig.Fake->Complete(1, Json(200, R"({"ok":false,"accessToken":"fresh-access-token-123"})"));
	if (!TestTrue(TEXT("the refresh request completes"), Rig.Wait(Raw)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("its end released no slot: the queued request still waits"), Limiter.NumActive() == ActiveBefore && Limiter.NumQueued() == QueuedBefore
		&& Rig.Fake->NumCalls() == 2);
	TestEqual(TEXT("... and the in-flight count is back"), Rig.Client.GetInFlightCount(), InFlightBefore);
	TestTrue(TEXT("plan decision 23: the client's envelope would fail this body; the refresh gets it raw"), Raw->SuccessCalls.load() == 1 && Raw->Value.IsSet()
		&& Raw->Value->Get(TEXT("accessToken")).AsString() == TEXT("fresh-access-token-123"));

	Rig.Fake->Complete(0, Json(200, R"({"ok":true,"data":{}})"));
	if (!TestTrue(TEXT("the holder ends, and the queue drains normally"), PumpUntil([Holder, Queued] { return Holder->IsDone() && Queued->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... the queued request was granted the slot and succeeded"), Holder->SuccessCalls.load() == 1 && Queued->SuccessCalls.load() == 1
		&& FindCall(Rig, TEXT("/queued")) != INDEX_NONE);
	TestTrue(TEXT("... and the books are empty"), Limiter.NumActive() == 0 && Limiter.NumQueued() == 0 && Rig.Client.GetInFlightCount() == 0);

	// The control: the same body through the client fails the client's envelope
	auto Control = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/auth/control")).Body(TEXT("{}")), Control).Send();
	TestTrue(TEXT("through the client the envelope fails it, Kind Server"), Rig.Wait(Control) && Control->Error.Kind == EVaRestErrorKind::Server);

	// Retry: Idempotent(), so the client's ExactRetry(2) would apply; a retried refresh would wait on the virtual clock
	auto Busy = MakeOutcome<FVaRestJson>();
	Observe(Context.Request(TEXT("POST"), TEXT("/auth/busy")).Idempotent().Body(TEXT("{}")), Busy).Send();
	const auto BusyDone = [Busy] { return Busy->IsDone(); };
	for (int32 Step = 0; Step < 3 && !PumpUntil(BusyDone, 1.0); ++Step)
	{
		// Only a retried refresh gets here: past the backoff it armed (0.5 s, then 1 s)
		Rig.Clock->Advance(2.0);
	}
	TestTrue(TEXT("a 503 refresh under WithRetry: the 503 is the answer"), Busy->IsDone() && Busy->Error.HttpStatus == 503 && Busy->Error.Attempts == 1);
	TestTrue(TEXT("... after exactly one transport call"), FindCall(Rig, TEXT("/auth/busy")) != INDEX_NONE && FindCall(Rig, TEXT("/auth/busy"), 1) == INDEX_NONE);

	// The auth's chain runs after MarkAuthRefresh: what would undo the bypass fails the request, nothing sent
	auto Retried = MakeOutcome<FVaRestJson>();
	auto Enveloped = MakeOutcome<FVaRestJson>();
	auto Credentialed = MakeOutcome<FVaRestJson>();
	auto Bare = MakeOutcome<FVaRestJson>();
	Observe(Context.Request(TEXT("POST"), TEXT("/auth/retry")).Retry(ExactRetry(2)).Body(TEXT("{}")), Retried).Send();
	Observe(Context.Request(TEXT("POST"), TEXT("/auth/envelope")).Envelope(FVaRestEnvelope::DataAt(TEXT("data"))).Body(TEXT("{}")), Enveloped).Send();
	Observe(Context.Request(TEXT("POST"), TEXT("/auth/required")).AuthMode(EVaRestAuthMode::Required).Body(TEXT("{}")), Credentialed).Send();
	Observe(Context.Request(TEXT("POST"), TEXT("/auth/bare")).NoRetry().NoEnvelope().NoAuth().Body(TEXT("{}")), Bare).Send();
	if (!TestTrue(TEXT("all four complete"), PumpUntil([Retried, Enveloped, Credentialed, Bare] { return Retried->IsDone() && Enveloped->IsDone() && Credentialed->IsDone() && Bare->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("Retry() on a refresh request: Kind Config, not sent"), Retried->Error.Kind == EVaRestErrorKind::Config
		&& Retried->Error.Message.Contains(TEXT("never retried")) && FindCall(Rig, TEXT("/auth/retry")) == INDEX_NONE);
	TestTrue(TEXT("Envelope() on a refresh request: Kind Config, not sent"), Enveloped->Error.Kind == EVaRestErrorKind::Config
		&& Enveloped->Error.Message.Contains(TEXT("read raw")) && FindCall(Rig, TEXT("/auth/envelope")) == INDEX_NONE);
	TestTrue(TEXT("AuthMode(Required) on a refresh request: Kind Config, not sent"), Credentialed->Error.Kind == EVaRestErrorKind::Config
		&& Credentialed->Error.Message.Contains(TEXT("no credentials")) && FindCall(Rig, TEXT("/auth/required")) == INDEX_NONE);
	const int32 BareCall = FindCall(Rig, TEXT("/auth/bare"));
	TestTrue(TEXT("NoRetry(), NoEnvelope() and NoAuth() keep it as it is: sent bare, read raw"), Bare->SuccessCalls.load() == 1 && BareCall != INDEX_NONE
		&& !Rig.Fake->GetCall(BareCall).Request.Headers.Contains(TEXT("Authorization")) && Bare->Value.IsSet() && Bare->Value->Get(TEXT("accessToken")).IsValid());
	TestEqual(TEXT("everything sent has ended"), Rig.Client.GetInFlightCount(), 0);
	return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshPublicTest, "VaRest.Http.Auth.RefreshAuthPublic",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshPublicTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// WebSocket spec 19 item 2: a socket refreshes through the client, sharing HTTP's single flight. Spec review I-5: it is
	// told how the refresh ended, so a rejection (the session is gone) is not mistaken for a network failure (try later).
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}

	// Refreshed, one refresh for two callers
	const FRefreshAnswers Shared = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Shared));
	Rig.Client.RefreshAuth(Record(Shared));
	TestEqual(TEXT("one refresh for both"), Auth->Refreshes, 1);
	TestEqual(TEXT("... two waiters"), State->NumRefreshWaitersInternal(), 2);
	TestTrue(TEXT("spec review I-2: the refresh's id is a request id"), Auth->LastRefreshId != 0);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestEqual(TEXT("concurrency review I-1: nothing inside the answer either"), Shared->Num(), 0);
	TestTrue(TEXT("both told: Refreshed"), PumpUntil([Shared] { return Shared->Num() == 2; })
		&& (*Shared)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None) && (*Shared)[1] == (*Shared)[0]);
	TestEqualSensitive(TEXT("... the session holds the new token"), Rig.Client.GetSession().AccessToken, FString(TEXT("new-access-123456")));

	// The game's own token ends the refresh in flight (Ready); its waiters go on from a later task, not inside the call (I-1)
	const FRefreshAnswers Game = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Game));
	Rig.Client.SetBearerToken(TEXT("game-access-123456"), /*bSameIdentity*/ true);
	TestEqual(TEXT("concurrency review I-1: the waiter does not run inside SetBearerToken"), Game->Num(), 0);
	TestTrue(TEXT("... it runs later: Refreshed"), PumpUntil([Game] { return Game->Num() == 1; })
		&& (*Game)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None));
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("late-access-123456")));
	Rig.Drain();
	TestEqualSensitive(TEXT("... and the ended refresh's late answer changes nothing"), Rig.Client.GetSession().AccessToken, FString(TEXT("game-access-123456")));

	// Failed(Connection): the session stays (ruling Q1, I-7)
	const FRefreshAnswers Failed = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Failed));
	Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("no route to the refresh endpoint")));
	TestTrue(TEXT("Failed: (Failed, Connection)"), PumpUntil([Failed] { return Failed->Num() == 1; })
		&& (*Failed)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Connection));
	TestTrue(TEXT("... the session kept, nothing expired"), Rig.Client.GetSession().HasRefreshToken() && *Expired == 0);

	// Concurrency review I-4: Done dropped uncalled ends the refresh as Failed(Connection), the session kept
	const FRefreshAnswers Dropped = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Dropped));
	Auth->Drop();
	TestTrue(TEXT("a dropped Done: (Failed, Connection)"), PumpUntil([Dropped] { return Dropped->Num() == 1; })
		&& (*Dropped)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Connection));
	TestTrue(TEXT("... the session kept, no refresh left running"), Rig.Client.GetSession().HasRefreshToken() && !State->IsRefreshing() && *Expired == 0);

	// Rejected: the session goes, OnSessionExpired once -- also when the socket calls NotifySessionExpired anyway (I-2)
	const FRefreshAnswers Rejected = MakeAnswers();
	Rig.Client.RefreshAuth([Rejected, Notifier = Rig.Client](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind FailedKind) mutable
	{
		Rejected->Emplace(Outcome, FailedKind);
		Notifier.NotifySessionExpired();
	});
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
	TestTrue(TEXT("Rejected: (Rejected, None)"), PumpUntil([Rejected] { return Rejected->Num() == 1; })
		&& (*Rejected)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Rejected, EVaRestErrorKind::None));
	TestTrue(TEXT("... the session expired"), !Rig.Client.GetSession().HasAccessToken() && !Rig.Client.GetSession().HasRefreshToken());
	TestEqual(TEXT("... OnSessionExpired once, the socket's NotifySessionExpired notwithstanding"), *Expired, 1);

	// No auth (or nothing to refresh with): (Failed, Config), and still not inside the call
	FTestRig Plain;
	const FRefreshAnswers None = MakeAnswers();
	Plain.Client.RefreshAuth(Record(None));
	TestEqual(TEXT("decision 3: nothing inside the call"), None->Num(), 0);
	TestTrue(TEXT("no auth: (Failed, Config)"), PumpUntil([None] { return None->Num() == 1; })
		&& (*None)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Config));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshEndsWithGameTest, "VaRest.Http.Auth.RefreshEndsWithGameChanges",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshEndsWithGameTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 3: the game's logout and NotifySessionExpired end a refresh in flight too -- its waiters are told the
	// session is gone (Rejected), from a later task (concurrency review I-1) -- and the ended refresh's late answer is
	// ignored: a Refreshed after a logout must not log the player back in
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), State.IsValid()))
	{
		return false;
	}
	const FRefreshAnswers Cleared = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Cleared));
	Rig.Client.ClearSession();
	TestEqual(TEXT("ClearSession: the waiter does not run inside it"), Cleared->Num(), 0);
	TestFalse(TEXT("... the refresh has ended"), State->IsRefreshing());
	TestTrue(TEXT("... it runs later: Rejected (the session is gone)"), PumpUntil([Cleared] { return Cleared->Num() == 1; })
		&& (*Cleared)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Rejected, EVaRestErrorKind::None));
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("late-access-123456"), TEXT("late-refresh-123456")));
	Rig.Drain();
	TestFalse(TEXT("... the late Refreshed does not log back in"), Rig.Client.GetSession().HasAccessToken() || Rig.Client.GetSession().HasRefreshToken());

	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	const FRefreshAnswers Notified = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Notified));
	TestEqual(TEXT("a second refresh, the first ended"), Auth->Refreshes, 2);
	Rig.Client.NotifySessionExpired();
	TestEqual(TEXT("NotifySessionExpired: the waiter does not run inside it"), Notified->Num(), 0);
	TestTrue(TEXT("... it runs later: Rejected"), PumpUntil([Notified] { return Notified->Num() == 1; })
		&& (*Notified)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Rejected, EVaRestErrorKind::None));
	TestEqual(TEXT("... OnSessionExpired once"), *Expired, 1);
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
	Rig.Drain();
	TestEqual(TEXT("... and the ended refresh's late Rejected expires nothing more"), *Expired, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshDefaultResultTest, "VaRest.Http.Auth.RefreshDefaultResultKeepsSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshDefaultResultTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 15 review M-5 (ruled): a result built by hand that forgets its outcome is Failed(Connection), as a dropped Done
	// is (concurrency review I-4) -- it keeps the session instead of logging the player out
	const FVaRestAuthRefreshResult Default;
	TestTrue(TEXT("a default result: Failed, Connection"), Default.Outcome == EVaRestAuthRefreshOutcome::Failed && Default.FailedKind == EVaRestErrorKind::Connection);
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	const FRefreshAnswers Answers = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Answers));
	Auth->Answer(FVaRestAuthRefreshResult());
	TestTrue(TEXT("FinishRefresh with a default result: the waiter fails (Failed, Connection)"), PumpUntil([Answers] { return Answers->Num() == 1; })
		&& (*Answers)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Connection));
	TestTrue(TEXT("... the session kept, nothing expired"), Rig.Client.GetSession().AccessToken.Equals(TEXT("old-access-123456"), ESearchCase::CaseSensitive)
		&& Rig.Client.GetSession().HasRefreshToken() && *Expired == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshWaiterDropsClientTest, "VaRest.Http.Auth.RefreshWaiterDropsClient",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshWaiterDropsClientTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review C-1's rule for waiters: a socket's RefreshAuth callback may drop the last FVaRestClient (a fresh
	// client after the refresh). The waiters' task keeps the state until the last waiter has run, as RunSessionChange
	// keeps it for listeners (VaRest.Http.Session.HandlerDropsClient); a weak probe shows it alive in both and gone after.
	using FClientHolder = TSharedRef<FVaRestClient, ESPMode::ThreadSafe>;
	using FStateWeak = TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe>;
	using FAlive = TSharedRef<TArray<bool>, ESPMode::ThreadSafe>;
	const FClientHolder Holder = MakeShared<FVaRestClient, ESPMode::ThreadSafe>(FVaRestClient::Make(TEXT("https://api.test")));
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = MakeShared<FHandAuth, ESPMode::ThreadSafe>();
	Holder->WithAuth(Auth);
	Holder->SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	const FStateWeak Weak = VaRest::Http::FClientAccess::State(*Holder);
	const FAlive Alive = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
	Holder->RefreshAuth([Holder, Weak, Alive](EVaRestAuthRefreshOutcome, EVaRestErrorKind)
	{
		// Copied out first: the assignment may free what this lambda captured with the client
		const FClientHolder Owner = Holder;
		const FStateWeak Watched = Weak;
		const FAlive Into = Alive;
		*Owner = FVaRestClient();
		Into->Add(Watched.IsValid());
	});
	Holder->RefreshAuth([Weak, Alive](EVaRestAuthRefreshOutcome, EVaRestErrorKind) { Alive->Add(Weak.IsValid()); });
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("the first waiter dropped the last client and saw the state alive; the second ran too and saw it alive"),
		PumpUntil([Alive] { return Alive->Num() == 2; }) && (*Alive)[0] && (*Alive)[1]);
	TestFalse(TEXT("... and the state went after the last waiter"), Weak.IsValid());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshCancelTest, "VaRest.Http.Auth.RefreshCancelLeavesLimiter",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshCancelTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 15 review M-4: a refresh request held at the transport and then canceled took no slot, so its end releases none
	// -- the limiter's books stay as they were and a queued request still waits (ruling M-8) -- and its log line carries
	// its id, the refresh's (spec review I-2), as every core line about a request carries #<RequestId>
	FTestRig Rig;
	Rig.Client.WithMaxConcurrent(1).WithBearer(TEXT("bearer-token-123456")).WithLogLevel(EVaRestLogLevel::Basic);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/hold")) || Request.Url.EndsWith(TEXT("/auth/refresh")) ? FVaRestFakeResponse::Manual() : Json(200);
	});
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> ClientState = VaRest::Http::FClientAccess::State(Rig.Client);
	if (!TestTrue(TEXT("a client state"), ClientState.IsValid()))
	{
		return false;
	}
	VaRest::Http::FLimiter& Limiter = ClientState->GetLimiter();
	auto Holder = MakeOutcome<FVaRestJson>();
	auto Queued = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/hold")), Holder).Send();
	Observe(Rig.Client.Get(TEXT("/queued")), Queued).Send();
	if (!TestTrue(TEXT("the only slot is held, a request queued behind it"), PumpUntil([&Rig, &Limiter] { return Rig.Fake->NumCalls() == 1 && Limiter.NumQueued() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	const int32 ActiveBefore = Limiter.NumActive();
	const int32 QueuedBefore = Limiter.NumQueued();
	const int32 InFlightBefore = Rig.Client.GetInFlightCount();

	FLogCapture Capture;
	const uint64 RefreshId = FVaRestTrace::NewRequestId();
	const FVaRestAuthRefreshContext Context = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, Rig.Client.GetSession(), RefreshId);
	auto Raw = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Context.Request(TEXT("POST"), TEXT("/auth/refresh")).Body(TEXT("{}")), Raw).Send();
	if (!TestTrue(TEXT("the refresh request is held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... past the full limiter, taking no slot"), Limiter.NumActive() == ActiveBefore && Limiter.NumQueued() == QueuedBefore
		&& Rig.Client.GetInFlightCount() == InFlightBefore + 1);
	Handle.Cancel();
	if (!TestTrue(TEXT("Cancel() ends it: Kind Canceled"), PumpUntil([Raw] { return Raw->IsDone(); }) && Raw->Error.Kind == EVaRestErrorKind::Canceled))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("its end released no slot: the books are as they were"), Limiter.NumActive() == ActiveBefore && Limiter.NumQueued() == QueuedBefore);
	TestEqual(TEXT("... the in-flight count is back"), Rig.Client.GetInFlightCount(), InFlightBefore);
	TestTrue(TEXT("... and the queued request still waits"), !Queued->IsDone() && Rig.Fake->NumCalls() == 2 && FindCall(Rig, TEXT("/queued")) == INDEX_NONE);
	const FString Prefix = FString::Printf(TEXT("#%llu POST "), RefreshId);
	TestTrue(TEXT("a Basic line carries #<RefreshId>: the refresh request's canceled line"),
		Capture.GetLines().ContainsByPredicate([&Prefix](const FString& Line) { return Line.Contains(Prefix) && Line.Contains(TEXT("/auth/refresh -> canceled")); }));

	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("the holder ends, and the queue drains normally"), PumpUntil([Holder, Queued] { return Holder->IsDone() && Queued->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... the queued request was granted the slot and succeeded"), Queued->SuccessCalls.load() == 1 && FindCall(Rig, TEXT("/queued")) != INDEX_NONE);
	TestTrue(TEXT("... and the books are empty"), Limiter.NumActive() == 0 && Limiter.NumQueued() == 0 && Rig.Client.GetInFlightCount() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshIdInUseTest, "VaRest.Http.Auth.RefreshIdInUseEnsures",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshIdInUseTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
#if DO_ENSURE
	// As VaRest.Http.Limiter.DoubleReleaseEnsuresEachTime: ensureAlways reports every time only while core.EnsureAlwaysEnabled
	// is on (AssertionMacros.cpp:136-140, :948) and every ensure is handled at HandleEnsurePercent 100
	// (GenericPlatformMisc.cpp:2058-2068, :2070-2094); under other settings the count is chance
	{
		IConsoleVariable* Always = IConsoleManager::Get().FindConsoleVariable(TEXT("core.EnsureAlwaysEnabled"));
		float Percent = 100.f;
		if (!GConfig || !GConfig->GetFloat(TEXT("Core.System"), TEXT("HandleEnsurePercent"), Percent, GEngineIni))
		{
			FParse::Value(FCommandLine::Get(), TEXT("handleensurepercent="), Percent);
		}
		if (!Always || !Always->GetBool() || Percent < 100.f)
		{
			AddInfo(TEXT("core.EnsureAlwaysEnabled is off or HandleEnsurePercent is below 100: ensureAlways does not report every failure here; nothing to check"));
			return true;
		}
	}
	// Task 15 review M-1: only FClientAccess::MakeRefreshContext makes a context, and the coordinator gives it a fresh id.
	// A context made with an id in use -- two here, each giving it away once -- puts a second exchange under that id: Enter
	// ensures. The thread's ensure handler (AssertionMacros.h:55-61) takes this one, so the run leaves no callstack;
	// FDebug::EnsureFailed asks it first (AssertionMacros.cpp:499).
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Fired = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const TFunction<bool(const FEnsureHandlerArgs&)> Outer = GetEnsureHandler();
	SetEnsureHandler([Fired, Outer](const FEnsureHandlerArgs& Args)
	{
		if (Args.Message && FCString::Strstr(Args.Message, TEXT("is already in flight on this client")))
		{
			++*Fired;
			return true;
		}
		return Outer ? Outer(Args) : false;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(Outer); };
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request) { return Request.Url.EndsWith(TEXT("/auth/first")) ? FVaRestFakeResponse::Manual() : Json(200); });
	const uint64 RefreshId = FVaRestTrace::NewRequestId();
	const FVaRestAuthRefreshContext First = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, FVaRestSessionData(), RefreshId);
	const FVaRestAuthRefreshContext Second = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, FVaRestSessionData(), RefreshId);
	auto FirstOutcome = MakeOutcome<FVaRestJson>();
	auto SecondOutcome = MakeOutcome<FVaRestJson>();
	Observe(First.Request(TEXT("POST"), TEXT("/auth/first")).Body(TEXT("{}")), FirstOutcome).Send();
	if (!TestTrue(TEXT("the first is held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("one id in flight: no ensure yet"), Fired->load(), 0);
	Observe(Second.Request(TEXT("POST"), TEXT("/auth/second")).Body(TEXT("{}")), SecondOutcome).Send();
	if (!TestTrue(TEXT("the second completes"), Rig.Wait(SecondOutcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("Task 15 review M-1: a second exchange under an id in flight ensures"), Fired->load(), 1);
	Rig.Fake->Complete(0, Json(200));
	TestTrue(TEXT("the first ends too"), Rig.Wait(FirstOutcome));
	Rig.Drain();
#else
	AddInfo(TEXT("ensures are compiled out: nothing to check"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthSingleFlightTest, "VaRest.Http.Auth.SingleFlight",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthSingleFlightTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	TArray<uint64> Ids;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Ids.Add(Observe(Rig.Client.Get(FString::Printf(TEXT("/p/%d"), Index)), Outcomes.Last()).Send().GetId());
	}
	if (!TestTrue(TEXT("twenty 401s, one refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 20; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	// Spec 4.2: "or start while it runs" -- these park before their first attempt
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Ids.Add(Observe(Rig.Client.Get(FString::Printf(TEXT("/late/%d"), Index)), Outcomes.Last()).Send().GetId());
	}
	Rig.Drain();
	TestEqual(TEXT("the late ones wait: no call yet"), Rig.Fake->NumCalls(), 20);
	TestEqual(TEXT("one refresh for all of them"), Auth->Refreshes, 1);
	Rig.Clock->Advance(0.25);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), TEXT("new-refresh-123456")));
	const auto AllDone = [&Outcomes] { for (const auto& One : Outcomes) { if (!One->IsDone()) { return false; } } return true; };
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone)))
	{
		Abandon(Rig);
		return false;
	}
	int32 Successes = 0;
	for (int32 Index = 0; Index < Outcomes.Num(); ++Index)
	{
		Successes += Outcomes[Index]->SuccessCalls.load();
		TestEqual(FString::Printf(TEXT("request %d: attempts"), Index), Outcomes[Index]->Response.Attempts, Index < 20 ? 2 : 1);
	}
	TestEqual(TEXT("all succeed"), Successes, 22);
	TestEqual(TEXT("each 401 replayed once, each late one sent once"), Rig.Fake->NumCalls(), 42);
	TestEqual(TEXT("still one refresh"), Auth->Refreshes, 1);
	TestEqualSensitive(TEXT("the session holds the new tokens"), Rig.Client.GetSession().RefreshToken, FString(TEXT("new-refresh-123456")));

	// IN-2: a replay's gap is the refresh; a late request's first attempt waited for it
	const TArray<FVaRestTraceAttemptDispatched> Replayed = DispatchesOf(*Scoped.Recorder, Ids[0]);
	const TArray<FVaRestTraceAttemptDispatched> Late = DispatchesOf(*Scoped.Recorder, Ids[20]);
	if (!TestTrue(TEXT("two dispatches, then one"), Replayed.Num() == 2 && Late.Num() == 1))
	{
		return false;
	}
	// Spec review I-1 (Inspector 3.3): a replay's park is the Gap before it, outside its attempt -- no AuthWait; a park
	// before the first attempt is inside it, which starts when it parked
	TestTrue(TEXT("the replay: GapBefore SessionRefresh 0.25 s, no AuthWait, the refresh's id"), Replayed[1].GapKind == EVaRestTraceGap::SessionRefresh
		&& FMath::IsNearlyEqual(Replayed[1].GapSeconds, 0.25) && Replayed[1].AuthWait == 0.0 && Replayed[1].AuthRefreshId == Auth->LastRefreshId);
	TestTrue(TEXT("the late one: attempt 1 started when it parked, AuthWait 0.25 s, the same refresh"), Late[0].GapKind == EVaRestTraceGap::None
		&& Late[0].AttemptStart == 0.0 && FMath::IsNearlyEqual(Late[0].AuthWait, 0.25) && Late[0].AuthRefreshId == Auth->LastRefreshId && Late[0].Attempt == 1);
	const TArray<FVaRestTraceMarker> Markers = Scoped.Recorder->MarkersOf(Rig.Client.GetId());
	TestTrue(TEXT("IN-9: SessionRefreshed, caused by one of the 401s"), Markers.ContainsByPredicate([&Ids](const FVaRestTraceMarker& Marker)
	{
		return Marker.Kind == EVaRestTraceMarkerKind::SessionRefreshed && Ids.Contains(Marker.CauseRequestId);
	}));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRejectedTest, "VaRest.Http.Auth.RefreshRejected",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRejectedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("both parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
	if (!TestTrue(TEXT("both complete"), PumpUntil([A, B] { return A->IsDone() && B->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("spec 4.2: parked requests fail Kind Auth, not retryable"), A->Error.Kind == EVaRestErrorKind::Auth && B->Error.Kind == EVaRestErrorKind::Auth
		&& !A->Error.bRetryable && A->Error.Message.Contains(TEXT("rejected")));
	TestEqual(TEXT("... and the client fires OnSessionExpired once"), *Expired, 1);
	TestTrue(TEXT("plan decision 3: a rejected refresh drops the tokens"), !Rig.Client.GetSession().HasAccessToken() && !Rig.Client.GetSession().HasRefreshToken());
	auto After = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), After).Send();
	TestTrue(TEXT("the next request: no credentials, not sent"), Rig.Wait(After) && After->Error.Kind == EVaRestErrorKind::Auth && Rig.Fake->NumCalls() == 2);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthFailedTest, "VaRest.Http.Auth.RefreshFailedKeepsSession",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthFailedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 3, rulings Q1 and I-7; spec review I-3 (Review Focus line 2): twenty requests park on their 401s, two
	// more start while the refresh runs, and the refresh fails on the network -- one refresh, each request fails once,
	// none replays, the player stays logged in
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	TArray<TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>> Outcomes;
	for (int32 Index = 0; Index < 20; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/p/%d"), Index)), Outcomes.Last()).Send();
	}
	if (!TestTrue(TEXT("twenty 401s, one refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 20; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	for (int32 Index = 0; Index < 2; ++Index)
	{
		Outcomes.Add(MakeOutcome<FVaRestJson>());
		Observe(Rig.Client.Get(FString::Printf(TEXT("/late/%d"), Index)), Outcomes.Last()).Send();
	}
	Rig.Drain();
	Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Connection, TEXT("the refresh endpoint did not answer")));
	const auto AllDone = [&Outcomes] { for (const auto& One : Outcomes) { if (!One->IsDone()) { return false; } } return true; };
	if (!TestTrue(TEXT("all complete"), PumpUntil(AllDone)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("one refresh"), Auth->Refreshes, 1);
	bool bEachOnce = true;
	for (const auto& One : Outcomes)
	{
		bEachOnce &= One->ErrorCalls.load() == 1 && One->CompleteCalls.load() == 1 && One->SuccessCalls.load() == 0
			&& One->Error.Kind == EVaRestErrorKind::Connection && One->Error.bRetryable && One->Error.Message.Contains(TEXT("AuthRefreshFailed"));
	}
	TestTrue(TEXT("each fails once, as the refresh did: Connection, retryable, saying why"), bEachOnce);
	TestEqual(TEXT("none replayed; the late ones were never sent"), Rig.Fake->NumCalls(), 20);
	TestTrue(TEXT("the session is kept"), Rig.Client.GetSession().AccessToken == TEXT("old-access-123456") && Rig.Client.GetSession().HasRefreshToken());
	TestEqual(TEXT("no OnSessionExpired"), *Expired, 0);

	// Ruling I-7's row: a refresh the configuration refuses (HTTP disabled) keeps the session too, and is not retryable
	auto Refused = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/c")), Refused).Send();
	if (!TestTrue(TEXT("the next 401 starts a new refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 21; }) && Rig.Drain() && Auth->Refreshes == 2))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Failed(EVaRestErrorKind::Config, TEXT("HttpDisabled")));
	TestTrue(TEXT("HTTP disabled: session kept, the parked request Kind Config, not retryable"), Rig.Wait(Refused) && Refused->Error.Kind == EVaRestErrorKind::Config
		&& !Refused->Error.bRetryable && Rig.Client.GetSession().HasRefreshToken() && *Expired == 0);

	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("a third refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 22; }) && Rig.Drain() && Auth->Refreshes == 3))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("... which works"), Rig.Wait(B) && B->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthStopTest, "VaRest.Http.Auth.StopWhileParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthStopTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	// Concurrency review I-5: an Outer stage sees decision 7's one outcome per request; the waiter count shows LeaveRefresh
	const TSharedRef<FOutcomeProbe, ESPMode::ThreadSafe> Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	UObject* Owner = NewObject<UVaRestHttpSettings>(GetTransientPackage(), NAME_None, RF_Transient);
	auto Canceled = MakeOutcome<FVaRestJson>();
	auto Owned = MakeOutcome<FVaRestJson>();
	auto ShutDown = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/c")), Canceled).Send();
	const FVaRestRequestHandle OwnedHandle = Observe(Rig.Client.Get(TEXT("/o")).Owner(Owner), Owned).Send();
	const FVaRestRequestHandle ShutDownHandle = Observe(Rig.Client.Get(TEXT("/s")).Tag(TEXT("Exit")), ShutDown).Send();
	if (!TestTrue(TEXT("all three parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; }) && Rig.Drain() && Auth->IsPending()
		&& State->NumRefreshWaitersInternal() == 3))
	{
		Abandon(Rig);
		return false;
	}
	Handle.Cancel();
	// Inside Cancel(), before any pump: the park's interrupt left the refresh and answered through the chain (decision 7)
	TestEqual(TEXT("Cancel() leaves the refresh: two waiters left"), State->NumRefreshWaitersInternal(), 2);
	TestEqual(TEXT("... and the Outer stage got its one Canceled inside Cancel()"), Probe->Count(Handle.GetId(), EVaRestTransportFailure::Canceled), 1);
	if (!TestTrue(TEXT("Cancel() ends the park"), PumpUntil([Canceled] { return Canceled->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... one OnError(Canceled), one OnComplete"), Canceled->ErrorCalls.load() == 1 && Canceled->CompleteCalls.load() == 1
		&& Canceled->Error.Kind == EVaRestErrorKind::Canceled);
	VaRest::Http::FClientAccess::State(Rig.Client)->CancelAll(TEXT("Exit"), EVaRestCancelReason::Shutdown);
	Owner->MarkAsGarbage();
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("nothing left in flight"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("no replay after any stop"), Rig.Fake->NumCalls(), 3);
	TestEqual(TEXT("shutdown and owner gone: no callbacks"), ShutDown->CompleteCalls.load() + Owned->CompleteCalls.load() + ShutDown->ErrorCalls.load()
		+ Owned->ErrorCalls.load(), 0);
	TestEqualSensitive(TEXT("the refresh still updates the session"), Rig.Client.GetSession().AccessToken, FString(TEXT("new-access-123456")));
	TestTrue(TEXT("each stop: one outcome for the Outer stage, a Canceled"), Probe->Total(Handle.GetId()) == 1 && Probe->Total(OwnedHandle.GetId()) == 1
		&& Probe->Total(ShutDownHandle.GetId()) == 1 && Probe->Count(OwnedHandle.GetId(), EVaRestTransportFailure::Canceled) == 1
		&& Probe->Count(ShutDownHandle.GetId(), EVaRestTransportFailure::Canceled) == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthProactiveTest, "VaRest.Http.Auth.Proactive",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthProactiveTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	// Spec 4.2: "proactively 30 s before a known expiry"
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
	TestTrue(TEXT("WS 19 item 2: NeedsAuthRefresh"), Rig.Client.NeedsAuthRefresh());
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/p")), Outcome).Send();
	Rig.Drain();
	if (!TestTrue(TEXT("parked before its first attempt"), Rig.Fake->NumCalls() == 0 && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), FString(), 3600.0));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("one attempt, with the new token"), Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 1 && Rig.Fake->NumCalls() == 1);
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Handle.GetId());
	TestTrue(TEXT("IN-2: attempt 1 started at the park (spec review I-1), AuthWait 0.5 s, no gap"), Dispatched.Num() == 1 && Dispatched[0].AttemptStart == 0.0
		&& FMath::IsNearlyEqual(Dispatched[0].AuthWait, 0.5) && Dispatched[0].GapKind == EVaRestTraceGap::None);
	TestFalse(TEXT("an hour left: no refresh due"), Rig.Client.NeedsAuthRefresh());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthReplayOnceTest, "VaRest.Http.Auth.ReplayOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthReplayOnceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	AcceptTokens(Rig, {});
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/never")), Outcome).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Outcome)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("spec 4.2 \"replay once\": the second 401 is the answer"), Outcome->Error.Kind == EVaRestErrorKind::Http && Outcome->Error.HttpStatus == 401
		&& Outcome->Response.Attempts == 2);
	TestEqual(TEXT("... no second refresh"), Auth->Refreshes, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthGameChangesTest, "VaRest.Http.Auth.GameChangesWhileRefreshing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthGameChangesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	AcceptTokens(Rig, { TEXT("game-access-123456"), TEXT("new-access-123456") });
	const auto ParkOne = [&Rig, &Auth](const TCHAR* Path, const TSharedRef<TTestOutcome<FVaRestJson>, ESPMode::ThreadSafe>& Outcome)
	{
		const int32 Before = Rig.Fake->NumCalls();
		Observe(Rig.Client.Get(Path), Outcome).Send();
		return PumpUntil([&Rig, Before] { return Rig.Fake->NumCalls() == Before + 1; }) && Rig.Drain() && Auth->IsPending();
	};
	// A login by the game while a refresh runs: the parked request goes with the game's token; the refresh's late answer is dropped
	auto A = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("A parked"), ParkOne(TEXT("/a"), A)))
	{
		Abandon(Rig);
		return false;
	}
	const int32 BeforeLogin = Rig.Fake->NumCalls();
	Rig.Client.SetBearerToken(TEXT("game-access-123456"));
	// Concurrency review I-1: the waiters resume from a later game-thread task, so nothing is dispatched inside the call
	TestEqual(TEXT("decision 3: A's replay is not dispatched inside SetBearerToken()"), Rig.Fake->NumCalls(), BeforeLogin);
	TestTrue(TEXT("A replays with the game's token"), Rig.Wait(A) && A->SuccessCalls.load() == 1);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	// Re-review N-3: Done's answer is applied from a later game-thread task (FRefreshDoneGuard): let it land first
	Rig.Drain();
	TestEqualSensitive(TEXT("the late refresh changes nothing"), Rig.Client.GetSession().AccessToken, FString(TEXT("game-access-123456")));

	// A logout while a refresh runs: Kind Auth, SessionCleared
	AcceptTokens(Rig, {});
	auto B = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("B parked"), ParkOne(TEXT("/b"), B)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.ClearSession();
	TestFalse(TEXT("decision 3: B's callbacks do not run inside ClearSession()"), B->IsDone());
	TestTrue(TEXT("B fails Kind Auth, the session cleared"), Rig.Wait(B) && B->Error.Kind == EVaRestErrorKind::Auth && B->Error.Message.Contains(TEXT("SessionCleared")));

	// A socket's NotifySessionExpired while a refresh runs: Kind Auth, rejected
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"));
	auto C = MakeOutcome<FVaRestJson>();
	if (!TestTrue(TEXT("C parked"), ParkOne(TEXT("/c"), C)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Client.NotifySessionExpired();
	TestFalse(TEXT("... nor C's inside NotifySessionExpired()"), C->IsDone());
	TestTrue(TEXT("C fails Kind Auth"), Rig.Wait(C) && C->Error.Kind == EVaRestErrorKind::Auth && C->Error.Message.Contains(TEXT("AuthRejected")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthExpiredOnceTest, "VaRest.Http.Auth.ExpiredOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthExpiredOnceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review I-2: a parked request's OnError calls NotifySessionExpired -- the documented call for a terminal
	// auth failure -- after its refresh was rejected, which already expired the session: one expiry, one OnSessionExpired
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	const TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> AuthErrors = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	auto Parked = MakeOutcome<FVaRestJson>();
	// After Observe: this OnError replaces its recorder; OnComplete still records
	Observe(Rig.Client.Get(TEXT("/a")), Parked).OnError([AuthErrors, Notifier = Rig.Client](const FVaRestError& Error) mutable
	{
		*AuthErrors += Error.Kind == EVaRestErrorKind::Auth ? 1 : 0;
		Notifier.NotifySessionExpired();
	}).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the refresh endpoint answered 400")));
	if (!TestTrue(TEXT("completes"), Rig.Wait(Parked)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("the request failed Kind Auth"), AuthErrors->load(), 1);
	TestEqual(TEXT("one expiry, one OnSessionExpired"), *Expired, 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthDroppedTest, "VaRest.Http.Auth.RefreshDropped",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthDroppedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review I-4: an auth drops its Done uncalled; the parked request still ends (Connection, retryable: no
	// logout), and the next 401 refreshes again
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	auto Parked = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Parked).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Drop();
	if (!TestTrue(TEXT("the parked request ends"), Rig.Wait(Parked)))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... Connection, retryable"), Parked->Error.Kind == EVaRestErrorKind::Connection && Parked->Error.bRetryable);
	TestTrue(TEXT("no refresh left running, none due, the session kept"), !State->IsRefreshing() && !Rig.Client.NeedsAuthRefresh()
		&& Rig.Client.GetSession().HasRefreshToken());
	auto Next = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/b")), Next).Send();
	if (!TestTrue(TEXT("a new 401 starts a second refresh"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain() && Auth->Refreshes == 2))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("... which works"), Rig.Wait(Next) && Next->SuccessCalls.load() == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthDropsClientParkedTest, "VaRest.Http.Auth.HandlerDropsClientWhileParked",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthDropsClientParkedTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review C-1, its parked half: a parked request's OnError drops the last client during a logout. With
	// I-1 the callback runs from a later game-thread task, not inside ClearSession; that task holds the exchange, and so
	// the state, while it runs.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	AcceptTokens(Rig, {});
	const TSharedRef<FVaRestClient, ESPMode::ThreadSafe> Holder = MakeShared<FVaRestClient, ESPMode::ThreadSafe>(Rig.Client);
	Rig.Client = FVaRestClient(); // the holder is the only handle now
	const TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Weak = VaRest::Http::FClientAccess::State(*Holder);
	const TSharedRef<TArray<bool>, ESPMode::ThreadSafe> Alive = MakeShared<TArray<bool>, ESPMode::ThreadSafe>();
	auto Parked = MakeOutcome<FVaRestJson>();
	Observe(Holder->Get(TEXT("/b")), Parked).OnError([Holder, Weak, Alive](const FVaRestError&)
	{
		const TSharedRef<FVaRestClient, ESPMode::ThreadSafe> Owner = Holder;
		const TWeakPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> Watched = Weak;
		const TSharedRef<TArray<bool>, ESPMode::ThreadSafe> Into = Alive;
		*Owner = FVaRestClient();
		Into->Add(Watched.IsValid());
	}).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig, &Auth] { return Rig.Fake->NumCalls() == 1 && Auth->IsPending(); })))
	{
		Holder->CancelAll();
		PumpUntil([Parked] { return Parked->IsDone(); });
		return false;
	}
	Holder->ClearSession();
	TestEqual(TEXT("concurrency review I-1: the callback does not run inside ClearSession()"), Alive->Num(), 0);
	TestTrue(TEXT("it runs later, the state alive inside it"), PumpUntil([Alive] { return Alive->Num() == 1; }) && (*Alive)[0]);
	TestTrue(TEXT("... and the state goes once the request is done"), PumpUntil([Weak] { return !Weak.IsValid(); }));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthParkDeadlineTest, "VaRest.Http.Auth.ParkEndsAtItsDeadline",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthParkDeadlineTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Concurrency review M-1: a park is bounded by the request's retry deadline. Behind a refresh the auth holds open (a
	// re-login screen, ruling I-4) the request ends at its deadline -- Timeout (Deadline), not retryable -- and leaves the
	// refresh, which runs on and replays nothing for it. On a clock whose timers fire early (half the delay, at least
	// 0.1 s) the park's timer re-arms for the rest, as the queue's does (Task 9 review minor 2): it fires at 0.5, 0.75,
	// 0.875 and 0.975 s before the 1 s deadline on Now(), and only the one at 1.075 s ends the park.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Client.WithClock(MakeShared<FEarlyClock, ESPMode::ThreadSafe>(Rig.Clock, 0.5, 0.1));
	const TSharedRef<FOutcomeProbe, ESPMode::ThreadSafe> Probe = MakeShared<FOutcomeProbe, ESPMode::ThreadSafe>();
	Rig.Client.Use(Probe, EVaRestStageSlot::Outer);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	auto Parked = MakeOutcome<FVaRestJson>();
	// Disabled, so nothing is retried: the deadline alone (plan decision 10: set for any policy with one)
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/a")).Retry(FVaRestRetryPolicy::Disabled().WithDeadline(1.f)), Parked).Send();
	if (!TestTrue(TEXT("parked on its 401"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()
		&& State->NumRefreshWaitersInternal() == 1))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.9);
	Rig.Drain();
	TestFalse(TEXT("early fires do not end the park: still parked at 0.9 s"), Parked->IsDone());
	TestEqual(TEXT("... still waiting on the refresh"), State->NumRefreshWaitersInternal(), 1);
	Rig.Clock->Advance(0.3);
	if (!TestTrue(TEXT("past the deadline on Now(), the park ends"), PumpUntil([Parked] { return Parked->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... as Timeout, not retryable, saying Deadline"), Parked->Error.Kind == EVaRestErrorKind::Timeout && !Parked->Error.bRetryable
		&& Parked->Error.Message.Contains(TEXT("Deadline")));
	TestTrue(TEXT("... once: one OnError, one OnComplete, one outcome for the Outer stage"), Parked->ErrorCalls.load() == 1 && Parked->CompleteCalls.load() == 1
		&& Probe->Total(Handle.GetId()) == 1);
	TestEqual(TEXT("it left the refresh"), State->NumRefreshWaitersInternal(), 0);
	TestTrue(TEXT("... which runs on"), State->IsRefreshing() && Auth->IsPending());
	TestEqual(TEXT("nothing left armed"), Rig.Clock->NumPendingTimers(), 0);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	Rig.Drain();
	TestEqual(TEXT("the refresh's end replays nothing for it"), Rig.Fake->NumCalls(), 1);
	TestEqual(TEXT("... and delivers nothing more"), Parked->CompleteCalls.load(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthParkHoldsNoSlotTest, "VaRest.Http.Auth.ParkHoldsNoSlot",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthParkHoldsNoSlotTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 4: an attempt holds its limiter slot until it answers. The gate is an Outer stage, before the attempt boundary and
	// the limiter, so a parked request holds none: with one slot, two requests park on their 401s (the second was granted the
	// slot the first freed) and a NoAuth() request, which the gate passes, still goes out at once
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Client.WithMaxConcurrent(1);
	Rig.Fake->SetResponder([](const FVaRestTransportRequest& Request)
	{
		return Request.Url.EndsWith(TEXT("/free")) || Request.Headers.Get(TEXT("Authorization")) == TEXT("Bearer new-access-123456") ? Json(200) : Json(401);
	});
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	VaRest::Http::FLimiter& Limiter = State->GetLimiter();
	auto A = MakeOutcome<FVaRestJson>();
	auto B = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), A).Send();
	Observe(Rig.Client.Get(TEXT("/b")), B).Send();
	if (!TestTrue(TEXT("both parked, one after the other through the one slot"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; }) && Rig.Drain()
		&& Auth->IsPending() && State->NumRefreshWaitersInternal() == 2))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("parked: no slot held, nothing queued"), Limiter.NumActive() == 0 && Limiter.NumQueued() == 0);
	auto Free = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/free")).NoAuth(), Free).Send();
	TestTrue(TEXT("a NoAuth() request takes the slot at once and completes"), Rig.Wait(Free) && Free->SuccessCalls.load() == 1);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("both replay and complete"), PumpUntil([A, B] { return A->IsDone() && B->IsDone(); })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("... with the new token"), A->SuccessCalls.load() == 1 && B->SuccessCalls.load() == 1 && Rig.Fake->NumCalls() == 5);
	Rig.Drain();
	TestTrue(TEXT("the books are empty"), Limiter.NumActive() == 0 && Limiter.NumQueued() == 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthReplayBodyTest, "VaRest.Http.Auth.ReplaySendsTheBodyAgain",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthReplayBodyTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Plan decision 8: a body is moved into the engine unless something may send it again. A POST is not retried (spec 4.1),
	// but an auth that can refresh may replay it, so Resolve copies its body and the replay sends the same bytes
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	auto Posted = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Post(TEXT("/p")).Body(TEXT("{\"name\":\"value\"}")), Posted).Send();
	if (!TestTrue(TEXT("the POST parked on its 401"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("replayed once, and it succeeds"), Rig.Wait(Posted) && Posted->SuccessCalls.load() == 1 && Rig.Fake->NumCalls() == 2))
	{
		Abandon(Rig);
		return false;
	}
	const TArray<uint8> First = Rig.Fake->ReadBody(0);
	const TArray<uint8> Replayed = Rig.Fake->ReadBody(1);
	TestTrue(TEXT("the replay sent the same body"), First.Num() > 0 && First == Replayed);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthNoParkAfterAnswerTest, "VaRest.Http.Auth.NoParkAfterTheAnswer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthNoParkAfterAnswerTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Re-review N-6's family (Task 8 re-review), for the gate: a custom Outer stage in front answers the request while its
	// attempt is still out, and the pipeline accepts that answer (Phase Decoding: OnChainDone already ran EndWaits). The
	// attempt's own 401 then comes back: the gate answers it up, as Retry does (VaRestRetry.cpp:176), instead of parking
	// -- a park now would start a refresh for a request already being delivered, and nothing would end its wait
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedRef<FEarlyAnswer, ESPMode::ThreadSafe> Early = MakeShared<FEarlyAnswer, ESPMode::ThreadSafe>();
	const TSharedRef<FHeldAttempt, ESPMode::ThreadSafe> Held = MakeShared<FHeldAttempt, ESPMode::ThreadSafe>();
	Rig.Client.Use(Early, EVaRestStageSlot::Outer).Use(Held, EVaRestStageSlot::PerAttempt);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Outcome).Send();
	if (!TestTrue(TEXT("the attempt is out"), PumpUntil([Held] { return Held->IsHolding(); })))
	{
		Abandon(Rig);
		return false;
	}
	// Synchronously, before any pump: the decode task's result comes back only through a game-thread task
	Early->AnswerNow();
	Held->Answer(401);
	TestEqual(TEXT("the late 401 after an accepted answer starts no refresh"), Auth->Refreshes, 0);
	if (!TestTrue(TEXT("the early answer is delivered"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... once, and no refresh runs"), Outcome->CompleteCalls.load() == 1 && Auth->Refreshes == 0 && !Auth->IsPending());
	// Drains a refresh the bug would have left (restore-the-bug only)
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	Rig.Drain();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthReportsTwiceTest, "VaRest.Http.Auth.AttemptReportedTwice",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthReportsTwiceTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// As Retry's StartAttempt (Task 8 re-review, out of scope 2): a PerAttempt stage that reports an attempt twice (review
	// C-3: the second report is ignored) must not answer a request the first report parked. The limiter passes every report
	// up (only its Release is once-only), so the gate takes one per attempt itself.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedRef<FReportsTwice, ESPMode::ThreadSafe> Twice = MakeShared<FReportsTwice, ESPMode::ThreadSafe>(401);
	Rig.Client.Use(Twice, EVaRestStageSlot::PerAttempt);
	auto Outcome = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Outcome).Send();
	Rig.Drain();
	if (!TestTrue(TEXT("the first report parked it; the second did not answer it"), Twice->Attempts == 1 && Auth->IsPending() && !Outcome->IsDone()))
	{
		Abandon(Rig);
		return false;
	}
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	if (!TestTrue(TEXT("replayed once: the 401 of attempt 2 is the answer"), Rig.Wait(Outcome) && Twice->Attempts == 2 && Outcome->Error.HttpStatus == 401
		&& Outcome->Response.Attempts == 2))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("... once, after one refresh"), Outcome->ErrorCalls.load() == 1 && Outcome->CompleteCalls.load() == 1 && Auth->Refreshes == 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRejectedFromNothingTest, "VaRest.Http.Auth.RejectedFromNothingExpiresOnce",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRejectedFromNothingTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 16 review M-1 (ruled): an auth that refreshes from no token (a platform ticket) on a session that starts empty.
	// FinishRefresh takes the refresh out before it expires the session, so ExpireSession's already-expired return
	// (concurrency review I-2) saw nothing left to expire: the waiters were told Rejected and the game heard nothing. Spec
	// 4.2: a rejection "fires OnSessionExpired (once)" -- here also when the socket calls NotifySessionExpired anyway.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = MakeShared<FHandAuth, ESPMode::ThreadSafe>();
	Auth->bFromNothing = true;
	Rig.Client.WithAuth(Auth);
	AcceptTokens(Rig, { TEXT("new-access-123456") });
	TSharedRef<int32, ESPMode::ThreadSafe> Expired = MakeShared<int32, ESPMode::ThreadSafe>(0);
	Rig.Client.OnSessionExpired().AddLambda([Expired] { ++*Expired; });
	TestTrue(TEXT("the session starts empty, and a refresh is due"), !Rig.Client.GetSession().HasAccessToken() && !Rig.Client.GetSession().HasRefreshToken()
		&& Rig.Client.NeedsAuthRefresh());
	auto Parked = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Parked).Send();
	Rig.Drain();
	if (!TestTrue(TEXT("the request parks before its first attempt; one refresh"), Rig.Fake->NumCalls() == 0 && Auth->IsPending() && Auth->Refreshes == 1))
	{
		Abandon(Rig);
		return false;
	}
	const FRefreshAnswers Socket = MakeAnswers();
	Rig.Client.RefreshAuth([Socket, Notifier = Rig.Client](EVaRestAuthRefreshOutcome Outcome, EVaRestErrorKind FailedKind) mutable
	{
		Socket->Emplace(Outcome, FailedKind);
		Notifier.NotifySessionExpired();
	});
	TestEqual(TEXT("... a socket shares it"), Auth->Refreshes, 1);
	Auth->Answer(FVaRestAuthRefreshResult::Rejected(TEXT("the platform refused the ticket")));
	if (!TestTrue(TEXT("both told"), PumpUntil([Parked, Socket] { return Parked->IsDone() && Socket->Num() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("the request fails Kind Auth; the socket is told Rejected"), Parked->Error.Kind == EVaRestErrorKind::Auth
		&& (*Socket)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Rejected, EVaRestErrorKind::None));
	TestEqual(TEXT("ruling M-1: OnSessionExpired once, with no token to drop"), *Expired, 1);
	TestEqual(TEXT("nothing was sent"), Rig.Fake->NumCalls(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthFailedKindTest, "VaRest.Http.Auth.RefreshFailedKindNormalized",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthFailedKindTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 16 review M-2: a Failed result built by hand with a kind the Failed() factory would not keep (Auth) -- the field
	// is public, and Task 15 review M-5 ruled that hand-built results matter. FinishRefresh keeps Timeout and Config and
	// makes the rest Connection, as the factory does, so a socket and a parked request agree about one refresh.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	auto Parked = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Parked).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	const FRefreshAnswers Socket = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Socket));
	FVaRestAuthRefreshResult ByHand;
	ByHand.Outcome = EVaRestAuthRefreshOutcome::Failed;
	ByHand.FailedKind = EVaRestErrorKind::Auth;
	ByHand.Message = TEXT("built by hand");
	Auth->Answer(MoveTemp(ByHand));
	if (!TestTrue(TEXT("both told"), PumpUntil([Parked, Socket] { return Parked->IsDone() && Socket->Num() == 1; })))
	{
		Abandon(Rig);
		return false;
	}
	TestTrue(TEXT("the socket: (Failed, Connection), as Failed(Auth, ...) would have made it"),
		(*Socket)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Failed, EVaRestErrorKind::Connection));
	TestTrue(TEXT("the parked request agrees: Connection, retryable"), Parked->Error.Kind == EVaRestErrorKind::Connection && Parked->Error.bRetryable);
	TestTrue(TEXT("the session is kept"), Rig.Client.GetSession().AccessToken.Equals(TEXT("old-access-123456"), ESearchCase::CaseSensitive)
		&& Rig.Client.GetSession().HasRefreshToken());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthRefreshAnnouncesTest, "VaRest.Http.Auth.RefreshAnnouncesOnlyAChange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthRefreshAnnouncesTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 16 review M-3: FinishRefresh(Refreshed) fires OnSessionChanged(Refreshed) -- sockets depend on it (WebSocket
	// spec 19 item 2) -- once; and, as SetTokensByGame, only when the tokens changed: a refresh that returns the very tokens
	// the session holds announces nothing, while its waiters still go on (Ready)
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedRef<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe> Changes = MakeShared<TArray<EVaRestSessionChange>, ESPMode::ThreadSafe>();
	Rig.Client.OnSessionChanged().AddLambda([Changes](EVaRestSessionChange Change) { Changes->Add(Change); });
	const FRefreshAnswers First = MakeAnswers();
	Rig.Client.RefreshAuth(Record(First));
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), TEXT("new-refresh-123456")));
	if (!TestTrue(TEXT("Refreshed"), PumpUntil([First] { return First->Num() == 1; })
		&& (*First)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None)))
	{
		return false;
	}
	Rig.Drain();
	TestTrue(TEXT("new tokens: OnSessionChanged(Refreshed), once"), Changes->Num() == 1 && (*Changes)[0] == EVaRestSessionChange::Refreshed);
	const FRefreshAnswers Same = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Same));
	// The access token it holds, the refresh token kept (empty), no expiry -- as before: UpdateTokens changes nothing
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456")));
	TestTrue(TEXT("the same tokens: the waiter still goes on"), PumpUntil([Same] { return Same->Num() == 1; })
		&& (*Same)[0] == FRefreshAnswer(EVaRestAuthRefreshOutcome::Refreshed, EVaRestErrorKind::None));
	Rig.Drain();
	TestEqual(TEXT("... and nothing is announced"), Changes->Num(), 1);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthShutdownWaitersTest, "VaRest.Http.Auth.ShutdownDropsRefreshWaiters",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthShutdownWaitersTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// Task 16 review M-6: at engine exit no callback runs (decision 2). The shutdown cancel takes a parked request out of the
	// refresh, silently; the auth's late answer is dropped (FRefreshDoneGuard::Post); and a session change that ends the
	// refresh -- a logout during exit -- drops its other waiters uncalled instead of running them from a later task.
	// CancelForShutdown() is process-wide (VaRest.Http.Cancel.AllAndShutdown): run it where no other VaRest traffic matters.
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	const TSharedPtr<VaRest::Http::FClientState, ESPMode::ThreadSafe> State = VaRest::Http::FClientAccess::State(Rig.Client);
	auto Parked = MakeOutcome<FVaRestJson>();
	Observe(Rig.Client.Get(TEXT("/a")), Parked).Send();
	if (!TestTrue(TEXT("parked"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 1; }) && Rig.Drain() && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	const FRefreshAnswers Socket = MakeAnswers();
	Rig.Client.RefreshAuth(Record(Socket));
	TestEqual(TEXT("two waiters"), State->NumRefreshWaitersInternal(), 2);
	VaRest::Http::FClientState::CancelForShutdown();
	// Review N-6: the editor goes on, so every later test must send again -- also when this test returns early
	ON_SCOPE_EXIT { VaRest::Http::FClientState::ResetShutdownInternal(); };
	TestEqual(TEXT("the shutdown cancel took the parked request out"), State->NumRefreshWaitersInternal(), 1);
	Rig.Client.ClearSession();
	TestFalse(TEXT("the logout ended the refresh"), State->IsRefreshing());
	TestTrue(TEXT("the request finishes"), PumpUntil([&Rig] { return Rig.Client.GetInFlightCount() == 0; }));
	Rig.Drain();
	TestEqual(TEXT("decision 2: the socket's waiter is dropped at engine exit, never called"), Socket->Num(), 0);
	TestEqual(TEXT("... and the parked request delivers nothing"), Parked->CompleteCalls.load() + Parked->ErrorCalls.load(), 0);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthInFlightEntryTest, "VaRest.Http.Auth.InFlightEntryIsTheExchange",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthInFlightEntryTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
#if DO_ENSURE
	if (!EnsuresReportEachTime(*this))
	{
		return true;
	}
	// Task 16 review M-7: the in-flight map's entry is compared, not only its id. Two exchanges under one id (a context made
	// twice with it, Task 15 review M-1): Enter ensures for the second, and the first to complete removes only its own entry
	// -- the second stays in flight, so CancelAll and the shutdown cancel still reach it. The same exchange entered again (no
	// path does today; harmless) is not another exchange: no ensure.
	TSharedRef<std::atomic<int32>, ESPMode::ThreadSafe> Fired = MakeShared<std::atomic<int32>, ESPMode::ThreadSafe>(0);
	const TFunction<bool(const FEnsureHandlerArgs&)> Outer = GetEnsureHandler();
	SetEnsureHandler([Fired, Outer](const FEnsureHandlerArgs& Args)
	{
		if (Args.Message && FCString::Strstr(Args.Message, TEXT("is already in flight on this client")))
		{
			++*Fired;
			return true;
		}
		return Outer ? Outer(Args) : false;
	});
	ON_SCOPE_EXIT { SetEnsureHandler(Outer); };
	FTestRig Rig;
	Rig.Fake->SetResponder([](const FVaRestTransportRequest&) { return FVaRestFakeResponse::Manual(); });
	const TSharedRef<FExchangeGrab, ESPMode::ThreadSafe> Grab = MakeShared<FExchangeGrab, ESPMode::ThreadSafe>();
	Rig.Client.Use(Grab, EVaRestStageSlot::PerAttempt);
	const uint64 RefreshId = FVaRestTrace::NewRequestId();
	const FVaRestAuthRefreshContext FirstContext = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, FVaRestSessionData(), RefreshId);
	const FVaRestAuthRefreshContext SecondContext = VaRest::Http::FClientAccess::MakeRefreshContext(Rig.Client, FVaRestSessionData(), RefreshId);
	auto First = MakeOutcome<FVaRestJson>();
	auto Second = MakeOutcome<FVaRestJson>();
	Observe(FirstContext.Request(TEXT("POST"), TEXT("/auth/first")).Body(TEXT("{}")), First).Send();
	Observe(SecondContext.Request(TEXT("POST"), TEXT("/auth/second")).Body(TEXT("{}")), Second).Send();
	if (!TestTrue(TEXT("both held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 2; })))
	{
		Abandon(Rig);
		return false;
	}
	TestEqual(TEXT("the second exchange under the id ensured"), Fired->load(), 1);
	Rig.Fake->Complete(0, Json(200));
	if (!TestTrue(TEXT("the first completes"), Rig.Wait(First)))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Drain();
	TestEqual(TEXT("ruling M-7: the second is still in flight"), Rig.Client.GetInFlightCount(), 1);
	Rig.Client.CancelAll();
	TestTrue(TEXT("... so CancelAll reaches it"), Rig.Wait(Second) && Second->Error.Kind == EVaRestErrorKind::Canceled);
	if (!Second->IsDone())
	{
		// Drains what an id-keyed removal would have left behind (restore-the-bug only)
		Rig.Fake->Complete(1, Json(200));
		Rig.Wait(Second);
	}
	Rig.Drain();

	auto Again = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/again")), Again).Send();
	if (!TestTrue(TEXT("held at the transport"), PumpUntil([&Rig] { return Rig.Fake->NumCalls() == 3; })))
	{
		Abandon(Rig);
		return false;
	}
	TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Exchange;
	for (const TWeakPtr<FVaRestExchange, ESPMode::ThreadSafe>& Seen : Grab->Seen)
	{
		const TSharedPtr<FVaRestExchange, ESPMode::ThreadSafe> Pinned = Seen.Pin();
		if (Pinned.IsValid() && Pinned->GetRequestId() == Handle.GetId())
		{
			Exchange = Pinned;
		}
	}
	if (!TestTrue(TEXT("its exchange"), Exchange.IsValid()))
	{
		Abandon(Rig);
		return false;
	}
	// Flagged and its transport call canceled; that Canceled comes back only through a game-thread task, so the exchange is
	// still in flight, in the chain, when it enters again: Enter finishes it Canceled (decision 3: from a later task)
	Handle.Cancel();
	VaRest::Http::FClientAccess::State(Rig.Client)->Enter(Exchange.ToSharedRef());
	TestEqual(TEXT("the same exchange entered again: no ensure"), Fired->load(), 1);
	TestTrue(TEXT("it completes once, Canceled"), Rig.Wait(Again) && Again->Error.Kind == EVaRestErrorKind::Canceled);
	Rig.Drain();
	TestTrue(TEXT("... one OnError, one OnComplete, nothing left in flight"), Again->ErrorCalls.load() == 1 && Again->CompleteCalls.load() == 1
		&& Rig.Client.GetInFlightCount() == 0);
#else
	AddInfo(TEXT("ensures are compiled out: nothing to check"));
#endif
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FVaRestHttpAuthWaitPerAttemptTest, "VaRest.Http.Auth.AuthWaitIsPerAttempt",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FVaRestHttpAuthWaitPerAttemptTest::RunTest(const FString& Parameters)
{
	using namespace VaRest::Http::Testing;
	using namespace VaRest::Http::AuthTestPrivate;
	// IN-2: AuthWait and AuthRefreshId are each attempt's own. The transport stage resets them after a dispatch, but an
	// attempt a stage answers before the transport (here a PerAttempt stage's 503) never gets there: the gate sets both on
	// every pass, so the retry's attempt, which waited behind no refresh, does not report attempt 1's park
	FTestRig Rig;
	const TSharedRef<FHandAuth, ESPMode::ThreadSafe> Auth = HandRig(Rig);
	Rig.Client.SetSession(TEXT("old-access-123456"), TEXT("old-refresh-123456"), 20.0);
	const TSharedRef<FFirstAnswered, ESPMode::ThreadSafe> First = MakeShared<FFirstAnswered, ESPMode::ThreadSafe>();
	Rig.Client.Use(First, EVaRestStageSlot::PerAttempt);
	FScopedTraceRecorder Scoped(EVaRestTraceDetail::Metadata);
	auto Outcome = MakeOutcome<FVaRestJson>();
	const FVaRestRequestHandle Handle = Observe(Rig.Client.Get(TEXT("/p")).Retry(ExactRetry(1)), Outcome).Send();
	Rig.Drain();
	if (!TestTrue(TEXT("parked before attempt 1 (a due expiry)"), Rig.Fake->NumCalls() == 0 && Auth->IsPending()))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(0.5);
	Auth->Answer(FVaRestAuthRefreshResult::Refreshed(TEXT("new-access-123456"), FString(), 3600.0));
	if (!TestTrue(TEXT("attempt 1 answered by the stage, a backoff armed"), PumpUntil([First] { return First->Attempts == 1; }) && Rig.Drain()))
	{
		Abandon(Rig);
		return false;
	}
	Rig.Clock->Advance(60.0);
	if (!TestTrue(TEXT("attempt 2 sent and succeeds"), Rig.Wait(Outcome) && Outcome->SuccessCalls.load() == 1 && Outcome->Response.Attempts == 2
		&& Rig.Fake->NumCalls() == 1))
	{
		Abandon(Rig);
		return false;
	}
	const TArray<FVaRestTraceAttemptDispatched> Dispatched = DispatchesOf(*Scoped.Recorder, Handle.GetId());
	TestTrue(TEXT("attempt 2's dispatch: a backoff gap, no AuthWait, no refresh id"), Dispatched.Num() == 1 && Dispatched[0].Attempt == 2
		&& Dispatched[0].GapKind == EVaRestTraceGap::Backoff && Dispatched[0].AuthWait == 0.0 && Dispatched[0].AuthRefreshId == 0);
	return true;
}

#endif
